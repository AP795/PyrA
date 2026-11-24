// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "pyra/pyra.h"

#include "pyra_api_internal.h"
#include "gallery_scan.h"
#include "pyra_openslide.h"
#include "remote_pyramid.h"
#include "synthetic.h"
#include "wsi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

static_assert( sizeof( float ) == 4 && std::numeric_limits<float>::is_iec559 );
static_assert( sizeof( priml::Rgb8 ) == 3 && sizeof( priml::Rgb16 ) == 6 );

constexpr auto kDefaultCacheBytes = uint64_t{ 32 } * 1024 * 1024;
constexpr auto kDefaultImagePixels = uint64_t{ 64 } * 1024 * 1024;
constexpr auto kDimensionTolerance = 1e-9;

using pyra::detail::ApiFailure;
using pyra::detail::require;


void write_error ( pyra_error *error, const char *message ) noexcept
{
	if (!error)
		return;
	const auto length = std::min( std::strlen( message ), sizeof( error->message ) - 1 );
	std::memcpy( error->message, message, length );
	error->message[length] = '\0';
}


template <typename F>
pyra_status api_call ( pyra_error *error, F &&function ) noexcept
{
	write_error( error, "" );
	try {
		function();
		return PYRA_OK;
	} catch (const ApiFailure &failure) {
		write_error( error, failure.message.c_str() );
		return failure.status;
	} catch (const std::bad_alloc &) {
		write_error( error, "Unable to allocate memory" );
		return PYRA_OUT_OF_MEMORY;
	} catch (const std::exception &exception) {
		write_error( error, exception.what() );
		return PYRA_INTERNAL_ERROR;
	} catch (...) {
		write_error( error, "Unexpected library failure" );
		return PYRA_INTERNAL_ERROR;
	}
}


struct WsiState {
	priml::Ppyr pyramid_;
	std::unique_ptr<priml::WsiLoader> loader_;
// Set, and owning loader_, for a slide opened from a remote geometry blob.
	priml::RemoteLoader *remote_ = nullptr;
	std::unique_ptr<pyra::detail::OpenSlideReader> openslide_;
	std::filesystem::path path_;
	uint64_t last_use_ = 0;
	bool opening_ = true;

	~WsiState () {
		if (loader_)
			loader_->Close( pyramid_ );
	}
};


size_t cache_bytes ( const WsiState &state, bool decoded )
{
	return priml::cache_kind( state.pyramid_, decoded ).allocated_bytes();
}


// Called only for another, idle slide under the owning context's mutex. Free
// backing allocations rather than merely marking their slots reusable.
size_t reclaim_cache ( WsiState &state, bool decoded )
{
	return priml::reclaim_idle_cache( state.pyramid_, decoded );
}


pyra_pixel_format public_format ( priml::LevelPixelType type )
{
	switch (type) {
	case priml::LevelPixelType::Rgb8: return PYRA_PIXEL_RGB8;
	case priml::LevelPixelType::Rgb16: return PYRA_PIXEL_RGB16;
	case priml::LevelPixelType::Gray8: return PYRA_PIXEL_GRAY8;
	case priml::LevelPixelType::Gray16: return PYRA_PIXEL_GRAY16;
	case priml::LevelPixelType::Float32: return PYRA_PIXEL_FLOAT32;
	}
	throw ApiFailure{ PYRA_UNSUPPORTED, "Unsupported plane pixel type" };
}

} // namespace


struct pyra_context {
	pyra_context_options options_;
	std::unique_ptr<pyra::detail::OpenSlideContext> openslide_;
	std::mutex mutex_;
	std::vector<std::weak_ptr<WsiState>> wsis_;
	uint64_t handles_ = 0;
	uint64_t clock_ = 0;
	uint64_t evictions_ = 0;
	uint64_t renders_ = 0;
	uint64_t incomplete_ = 0;
	uint64_t compressed_hits_ = 0;
	uint64_t compressed_misses_ = 0;
	uint64_t decoded_hits_ = 0;
	uint64_t decoded_misses_ = 0;

	uint64_t allocated ( bool decoded ) const {
		auto total = uint64_t{ 0 };
		for (const auto &weak : wsis_)
			if (const auto state = weak.lock())
				total += cache_bytes( *state, decoded );
		return total;
	}

	size_t growth_budget ( const WsiState *requester, bool decoded, size_t wanted ) {
		const auto cap = decoded ? options_.decoded_cache_bytes : options_.compressed_cache_bytes;
		auto used = allocated( decoded );
		auto available = used < cap ? cap - used : uint64_t{ 0 };
		if (available >= wanted)
			return wanted;
		auto victims = std::vector<std::shared_ptr<WsiState>>{};
		for (const auto &weak : wsis_) {
			if (auto state = weak.lock(); state && state.get() != requester && cache_bytes( *state, decoded ))
				victims.push_back( std::move( state ) );
		}
		std::sort( victims.begin(), victims.end(), [] ( const auto &a, const auto &b ) {
			return a->last_use_ < b->last_use_;
		} );
		for (const auto &victim : victims) {
			const auto reclaimed = reclaim_cache( *victim, decoded );
			if (reclaimed) {
				++evictions_;
				used -= reclaimed;
				available = used < cap ? cap - used : uint64_t{ 0 };
			}
			if (available >= wanted)
				break;
		}
		return (size_t)std::min<uint64_t>( available, wanted );
	}
};


struct pyra_wsi {
	pyra_context *context_ = nullptr;
	std::shared_ptr<WsiState> state_;
};


namespace {

const priml::PlaneStack &get_plane ( const WsiState &state, uint32_t index )
{
	require( index < state.pyramid_.planes.size(), "Plane index is out of range" );
	return state.pyramid_.planes[index];
}


uint32_t output_span ( double extent, double mpp )
{
	auto ratio = extent / mpp;
	require( std::isfinite( ratio ) && ratio > 0 && ratio <= std::numeric_limits<int>::max(),
		"Requested raster dimension exceeds the supported range", PYRA_LIMIT_EXCEEDED );
	const auto nearest = std::round( ratio );
	if (nearest > 0 && std::abs( ratio - nearest ) <= kDimensionTolerance * std::max( 1.0, ratio ))
		ratio = nearest;
	return (uint32_t)std::ceil( ratio );
}


pyra_image_layout image_layout ( const pyra_wsi &wsi, const pyra_image_request *request )
{
	require( request && request->struct_size >= sizeof( *request ), "Invalid image request structure" );
	auto pixel_type = priml::LevelPixelType::Rgb8;
	if (wsi.state_->openslide_)
		require( request->plane_index == 0, "Plane index is out of range" );
	else
		pixel_type = get_plane( *wsi.state_, request->plane_index ).pixel_type;
	const auto &r = request->rectangle;
	require( std::isfinite( request->mpp ) && request->mpp > 0 &&
		std::isfinite( r.x_um ) && std::isfinite( r.y_um ) &&
		std::isfinite( r.width_um ) && std::isfinite( r.height_um ) &&
		r.width_um > 0 && r.height_um > 0 &&
		std::isfinite( r.x_um + r.width_um ) && std::isfinite( r.y_um + r.height_um ),
		"Rectangle and mpp must be finite, with positive extents and mpp" );
// The current core uses float metric coordinates. Reject loss of a whole
// extent at this conversion rather than silently rendering a different view.
	const auto x = float( r.x_um );
	const auto y = float( r.y_um );
	const auto right = float( r.x_um + r.width_um );
	const auto bottom = float( r.y_um + r.height_um );
	require( std::isfinite( x ) && std::isfinite( y ) && std::isfinite( right ) &&
		std::isfinite( bottom ) && right > x && bottom > y &&
		std::isfinite( float( request->mpp ) ) && float( request->mpp ) > 0,
		"Requested geometry cannot be represented by the renderer", PYRA_LIMIT_EXCEEDED );
	auto layout = pyra_image_layout{};
	layout.struct_size = sizeof( layout );
	layout.pixel_format = public_format( pixel_type );
	layout.width = output_span( r.width_um, request->mpp );
	layout.height = output_span( r.height_um, request->mpp );
	const auto pixels = uint64_t( layout.width ) * layout.height;
	require( pixels <= wsi.context_->options_.max_image_pixels && pixels <= uint64_t( std::numeric_limits<int>::max() ),
		"Requested image exceeds the context pixel limit", PYRA_LIMIT_EXCEEDED );
	const auto bytes_per_pixel = priml::pixel_size_bytes( pixel_type );
	require( pixels <= std::numeric_limits<size_t>::max() / bytes_per_pixel,
		"Image byte size overflows size_t", PYRA_LIMIT_EXCEEDED );
	layout.row_stride_bytes = size_t( layout.width ) * bytes_per_pixel;
	layout.capacity_bytes = size_t( pixels ) * bytes_per_pixel;
	return layout;
}


// The context's cache options and shared budget, applied to a slide about to open.
void configure_cache ( pyra_context &context, WsiState &state )
{
	auto &pyramid = state.pyramid_;
	pyramid.decode_threads = (int)context.options_.decode_threads;
	pyramid.ram_cap_bytes = 0;
	pyramid.cache_topology = context.options_.cache_topology == PYRA_CACHE_PER_LEVEL
		? priml::CacheTopology::PerLevel : priml::CacheTopology::PerPyramid;
	pyramid.tbf_cap_bytes = (size_t)context.options_.compressed_cache_bytes;
	pyramid.pxb_cap_bytes = (size_t)context.options_.decoded_cache_bytes;
	pyramid.use_pixbf = context.options_.decoded_cache_bytes > 0;
	pyramid.cache_growth_budget = [&context, requester = &state] ( bool decoded, size_t wanted ) {
// Coarse-level construction needs temporary decoded tile scratch even when
// retained decoded caching is disabled. It is released before open returns.
		if (decoded && context.options_.decoded_cache_bytes == 0 && requester->opening_)
			return wanted;
		return context.growth_budget( requester, decoded, wanted );
	};
}


std::shared_ptr<WsiState> open_state ( pyra_context &context, const char *path )
{
	const auto source = pyr::path_from_utf8( path );
	auto filesystem_error = std::error_code{};
	const auto canonical = std::filesystem::canonical( source, filesystem_error );
	require( !filesystem_error, "Unable to resolve slide path", PYRA_IO_ERROR );
	std::erase_if( context.wsis_, [] ( const auto &weak ) { return weak.expired(); } );
	for (const auto &weak : context.wsis_) {
		if (auto state = weak.lock(); state && state->path_ == canonical) {
			state->last_use_ = ++context.clock_;
			return state;
		}
	}
	auto state = std::make_shared<WsiState>();
	state->path_ = canonical;
	state->last_use_ = ++context.clock_;
	context.wsis_.push_back( state );
	const auto utf8 = canonical.u8string();
	const auto filename = std::string( utf8.begin(), utf8.end() );
	if (context.openslide_) {
		state->openslide_ = context.openslide_->open( filename );
		return state;
	}
	configure_cache( context, *state );
	auto &pyramid = state->pyramid_;
	state->loader_ = priml::WsiLoader::Create( filename );
	require( state->loader_ != nullptr, "No enabled loader supports this source", PYRA_UNSUPPORTED );
	auto type = priml::LevelPixelType::Rgb8;
	priml::ProbeWsiPixelType( filename, &type );
	const auto opened = priml::visit_level_pixel_type( type, [&] ( auto pixel ) {
		return state->loader_->OpenFile<decltype( pixel )>( filename, pyramid );
	} );
	if (const auto *reason = priml::img_codec_unavailable_reason( pyramid.unsupported_codec_->load() ))
		throw ApiFailure{ PYRA_UNSUPPORTED, reason };
	require( opened, state->loader_->open_error_.empty() ? "Unable to open slide" :
		state->loader_->open_error_.c_str(), PYRA_IO_ERROR );
	state->opening_ = false;
	if (context.options_.decoded_cache_bytes == 0)
		(void)reclaim_cache( *state, true );
	require( !pyramid.planes.empty(), "Slide has no supported image planes", PYRA_UNSUPPORTED );
	return state;
}


// A sister slide built from a server's geometry blob. It has no path, so it is
// never shared with another open, but it draws on the context's cache budget.
std::shared_ptr<WsiState> open_remote_state ( pyra_context &context, const void *geometry, size_t size )
{
	require( !context.openslide_, "Remote slides need the native backend", PYRA_UNSUPPORTED );
	std::erase_if( context.wsis_, [] ( const auto &weak ) { return weak.expired(); } );
	auto state = std::make_shared<WsiState>();
	state->last_use_ = ++context.clock_;
	context.wsis_.push_back( state );
	configure_cache( context, *state );
	auto loader = std::make_unique<priml::RemoteLoader>();
	state->remote_ = loader.get();
	state->loader_ = std::move( loader );
	auto reason = std::string{};
	const auto opened = state->remote_->OpenRemote( static_cast<const uint8_t *>( geometry ), size, state->pyramid_, &reason );
	require( opened, reason.c_str() );
	state->opening_ = false;
	require( !state->pyramid_.planes.empty(), "Slide has no supported image planes", PYRA_UNSUPPORTED );
	return state;
}


// The open file a server describes and reads tiles from.
WsiState &served_state ( pyra_wsi &wsi )
{
	auto &state = *wsi.state_;
	require( state.loader_ && !state.remote_ && !state.openslide_,
		"Geometry and stored tiles come from a native slide opened from its file", PYRA_UNSUPPORTED );
	return state;
}

} // namespace


void pyra_context_options_init ( pyra_context_options *options )
{
	if (!options)
		return;
	*options = {};
	options->struct_size = sizeof( *options );
	options->abi_version = PYRA_ABI_VERSION;
	options->compressed_cache_bytes = kDefaultCacheBytes;
	options->decoded_cache_bytes = kDefaultCacheBytes;
	options->max_image_pixels = kDefaultImagePixels;
	options->decode_threads = 4;
	options->backend = PYRA_BACKEND_NATIVE;
	options->cache_topology = PYRA_CACHE_ARENA;
}


pyra_status pyra_make_gallery ( const char *directory_utf8, double mpp,
	char *manifest_utf8, size_t capacity, pyra_error *error )
{
	if (manifest_utf8 && capacity > 0)
		manifest_utf8[0] = '\0';
	return api_call( error, [&] {
		require( directory_utf8 && *directory_utf8, "Directory path is empty" );
		require( mpp >= 0.0 && std::isfinite( mpp ), "Invalid microns per pixel" );
		auto options = priml::GalleryScanOptions{};
		options.mpp = mpp;
		auto manifest = priml::GalleryManifest{};
		auto report = priml::GalleryScanReport{};
		auto reason = std::string{};
		const auto directory = pyr::path_from_utf8( directory_utf8 );
		if (!priml::scan_gallery_directory( directory, options, &manifest, &report, &reason )) {
			for (const auto &[file, why] : report.refused)
				reason += "; " + file + ": " + why;
			throw pyra::detail::ApiFailure{ report.needs_mpp ? PYRA_INVALID_ARGUMENT : PYRA_IO_ERROR,
				reason.empty() ? std::string( "No gallery could be made" ) : reason };
		}
		require( priml::write_gallery_manifest( report.manifest_path, manifest, &reason ),
			reason.c_str(), PYRA_IO_ERROR );
		if (manifest_utf8 && capacity > 0) {
			const auto text = report.manifest_path.u8string();
			const auto length = std::min( text.size(), capacity - 1 );
			std::memcpy( manifest_utf8, text.data(), length );
			manifest_utf8[length] = '\0';
		}
	} );
}


namespace {

// The public limits restate synthetic.h, which cannot include this header; keep
// them equal.
static_assert( priml::kSynthTileEdgeMin == PYRA_SYNTHETIC_TILE_MIN &&
	priml::kSynthTileEdgeMax == PYRA_SYNTHETIC_TILE_MAX &&
	priml::kSynthTileEdgeStep == PYRA_SYNTHETIC_TILE_STEP &&
	priml::kSynthImageEdgeMax == PYRA_SYNTHETIC_EDGE_MAX &&
	priml::kSynthZLevelsMax == PYRA_SYNTHETIC_Z_MAX &&
	priml::kSynthMppMin == PYRA_SYNTHETIC_MPP_MIN &&
	priml::kSynthMppMax == PYRA_SYNTHETIC_MPP_MAX &&
	priml::kSynthZStepMinUm == PYRA_SYNTHETIC_Z_STEP_MIN_UM &&
	priml::kSynthZStepMaxUm == PYRA_SYNTHETIC_Z_STEP_MAX_UM &&
	priml::kSynthNucleusDiameterMinUm == PYRA_SYNTHETIC_NUCLEUS_MIN_UM &&
	priml::kSynthNucleusDiameterMaxUm == PYRA_SYNTHETIC_NUCLEUS_MAX_UM &&
	priml::kSynthNucleusSpacingMinUm == PYRA_SYNTHETIC_SPACING_MIN_UM &&
	priml::kSynthNucleusSpacingMaxUm == PYRA_SYNTHETIC_SPACING_MAX_UM &&
	priml::kImageLosslessQuality == PYRA_QUALITY_LOSSLESS );
static_assert( priml::kSynthImageEdgeMax < std::numeric_limits<int>::max() );


priml::ImgCodec codec_from_c ( pyra_codec codec ) noexcept
{
	switch (codec) {
	case PYRA_CODEC_JPEG: return priml::ImgCodec::Jpeg;
	case PYRA_CODEC_JPEG2000: return priml::ImgCodec::Jp2k;
	case PYRA_CODEC_HTJ2K: return priml::ImgCodec::Htj2k;
	case PYRA_CODEC_JPEGXL: return priml::ImgCodec::Jxl;
	default: return priml::ImgCodec::None;
	}
}


int narrow_int ( uint32_t value ) noexcept
{
	return int( std::min<uint32_t>( value, uint32_t( std::numeric_limits<int>::max() ) ) );
}


priml::SyntheticSlideSpec spec_from_c ( const pyra_synthetic_options &options )
{
	auto spec = priml::SyntheticSlideSpec{};
	spec.size = { narrow_int( options.width ), narrow_int( options.height ) };
	spec.tile_edge = narrow_int( options.tile_edge );
	spec.z_levels = narrow_int( options.z_levels );
	spec.mpp = options.mpp;
	spec.codec = codec_from_c( options.codec );
	spec.quality = narrow_int( options.quality );
	spec.seed = options.seed;
	spec.z_step_um = options.z_step_um;
	spec.focus_fraction = options.focus_fraction;
	spec.nucleus_diameter_um = options.nucleus_diameter_um;
	spec.nucleus_spacing_um = options.nucleus_spacing_um;
	spec.positive_fraction = options.positive_fraction;
	spec.tumour_fraction = options.tumour_fraction;
	spec.lymphocyte_fraction = options.lymphocyte_fraction;
	return spec;
}

} // namespace


int pyra_codec_can_encode ( pyra_codec codec )
{
	return priml::img_codec_can_encode( codec_from_c( codec ) ) ? 1 : 0;
}


void pyra_synthetic_options_init ( pyra_synthetic_options *options )
{
	if (!options)
		return;
	const auto defaults = priml::SyntheticSlideSpec{};
	*options = {};
	options->struct_size = sizeof( *options );
	options->width = uint32_t( defaults.size.w );
	options->height = uint32_t( defaults.size.h );
	options->tile_edge = uint32_t( defaults.tile_edge );
	options->z_levels = uint32_t( defaults.z_levels );
	options->codec = PYRA_CODEC_JPEG;
	options->quality = uint32_t( defaults.quality );
	options->mpp = defaults.mpp;
	options->seed = defaults.seed;
	options->z_step_um = defaults.z_step_um;
	options->focus_fraction = defaults.focus_fraction;
	options->nucleus_diameter_um = defaults.nucleus_diameter_um;
	options->nucleus_spacing_um = defaults.nucleus_spacing_um;
	options->positive_fraction = defaults.positive_fraction;
	options->tumour_fraction = defaults.tumour_fraction;
	options->lymphocyte_fraction = defaults.lymphocyte_fraction;
}


static pyra_status make_synthetic ( const pyra_synthetic_options *options,
	const char *path_utf8, bool dicom, pyra_error *error )
{
	return api_call( error, [&] {
		require( options && options->struct_size >= sizeof( *options ),
			"Invalid synthetic options structure" );
		require( path_utf8 && *path_utf8, "Output path is empty" );
		const auto spec = spec_from_c( *options );
		if (dicom)
			require( priml::synthetic_dicom_can_encode( spec.codec ),
				"DICOM generation needs DCMTK and an enabled JPEG, JPEG 2000 or HTJ2K encoder", PYRA_UNSUPPORTED );
		auto reason = std::string{};
		if (!priml::synthetic_spec_valid( spec, &reason )) {
			const auto unavailable = spec.codec != priml::ImgCodec::None &&
				!priml::img_codec_can_encode( spec.codec );
			throw ApiFailure{ unavailable ? PYRA_UNSUPPORTED : PYRA_INVALID_ARGUMENT, reason };
		}
		auto progress = priml::SyntheticProgress{};
		if (options->progress) {
			progress = [options] ( long long done, long long total ) {
				return options->progress( options->progress_user, uint64_t( done ), uint64_t( total ) ) != 0;
			};
		}
		auto written = false;
		if (dicom) {
			const auto path = pyr::path_from_utf8( path_utf8 );
			auto extension = path.extension().u8string();
			std::transform( extension.begin(), extension.end(), extension.begin(),
				[] ( unsigned char ch ) { return std::tolower( ch ); } );
			require( extension == u8".dcm", "DICOM output needs a .dcm file name" );
			written = priml::write_synthetic_dicom( path, spec, progress, &reason );
		} else {
			written = priml::write_synthetic_slide( path_utf8, spec, progress, &reason );
		}
		if (!written)
			throw ApiFailure{ reason == "cancelled" ? PYRA_INCOMPLETE : PYRA_IO_ERROR, reason };
	} );
}


pyra_status pyra_make_synthetic ( const pyra_synthetic_options *options,
	const char *path_utf8, pyra_error *error )
{
	return make_synthetic( options, path_utf8, false, error );
}


int pyra_synthetic_dicom_can_encode ( pyra_codec codec )
{
	return priml::synthetic_dicom_can_encode( codec_from_c( codec ) ) ? 1 : 0;
}


pyra_status pyra_make_synthetic_dicom ( const pyra_synthetic_options *options,
	const char *path_utf8, pyra_error *error )
{
	return make_synthetic( options, path_utf8, true, error );
}


pyra_status pyra_synthetic_count_nuclei ( const pyra_synthetic_options *options,
	uint32_t x, uint32_t y, uint32_t width, uint32_t height,
	pyra_synthetic_nuclei *nuclei, pyra_error *error )
{
	return api_call( error, [&] {
		require( options && options->struct_size >= sizeof( *options ),
			"Invalid synthetic options structure" );
		require( nuclei && nuclei->struct_size >= sizeof( *nuclei ), "Invalid nuclei structure" );
		auto spec = spec_from_c( *options );
// Counting renders nothing, so any valid codec stands in for the requested one.
		spec.codec = priml::ImgCodec::Jpeg;
		spec.quality = priml::kImageDefaultLossyQuality;
		auto reason = std::string{};
		if (!priml::synthetic_spec_valid( spec, &reason ))
			throw ApiFailure{ PYRA_INVALID_ARGUMENT, reason };
		const auto x0 = narrow_int( x );
		const auto y0 = narrow_int( y );
		const auto census = priml::synthetic_census( spec, { { x0, y0 },
			{ narrow_int( uint32_t( std::min<uint64_t>( uint64_t( x0 ) + width, UINT32_MAX ) ) ),
			narrow_int( uint32_t( std::min<uint64_t>( uint64_t( y0 ) + height, UINT32_MAX ) ) ) } } );
		*nuclei = { sizeof( *nuclei ), 0u, uint64_t( census.tumour ), uint64_t( census.tumour_positive ),
			uint64_t( census.fibroblast ), uint64_t( census.lymphocyte ), census.tumour_diameter_um };
	} );
}


pyra_status pyra_get_backend_info ( pyra_backend backend, pyra_backend_info *info,
	pyra_error *error )
{
	return api_call( error, [&] {
		require( info && info->struct_size >= sizeof( *info ), "Invalid backend information structure" );
		require( backend == PYRA_BACKEND_NATIVE || backend == PYRA_BACKEND_OPENSLIDE, "Unknown backend" );
		const auto *version = backend == PYRA_BACKEND_NATIVE ? PYRA_VERSION : pyra::detail::openslide_version();
		*info = { sizeof( *info ), version != nullptr ? 1u : 0u,
			backend == PYRA_BACKEND_NATIVE ? "PyrA native" : "OpenSlide", version };
	} );
}


pyra_status pyra_context_create ( const pyra_context_options *options,
	pyra_context **context, pyra_error *error )
{
	if (context)
		*context = nullptr;
	return api_call( error, [&] {
		require( context != nullptr, "Context output pointer is null" );
		auto settings = pyra_context_options{};
		pyra_context_options_init( &settings );
		if (options) {
			require( options->struct_size >= sizeof( *options ) && options->abi_version == PYRA_ABI_VERSION,
				"Unsupported context options size or ABI version" );
			settings = *options;
		}
		require( settings.compressed_cache_bytes > 0 && settings.max_image_pixels > 0 &&
			settings.decode_threads > 0 && settings.decode_threads <= uint32_t( std::numeric_limits<int>::max() ) &&
			settings.compressed_cache_bytes <= std::numeric_limits<size_t>::max() &&
			settings.decoded_cache_bytes <= std::numeric_limits<size_t>::max(), "Invalid context resource limits" );
		auto owner = std::make_unique<pyra_context>();
		owner->options_ = settings;
		require( settings.backend == PYRA_BACKEND_NATIVE || settings.backend == PYRA_BACKEND_OPENSLIDE,
			"Unknown backend" );
		require( settings.cache_topology == PYRA_CACHE_ARENA ||
			settings.cache_topology == PYRA_CACHE_PER_LEVEL, "Unknown cache topology" );
		if (settings.backend == PYRA_BACKEND_OPENSLIDE)
			owner->openslide_ = pyra::detail::make_openslide_context( settings );
		*context = owner.release();
	} );
}


pyra_status pyra_context_destroy ( pyra_context *context, pyra_error *error )
{
	return api_call( error, [&] {
		require( context != nullptr, "Context is null" );
		{
			auto guard = std::scoped_lock( context->mutex_ );
			require( context->handles_ == 0, "Close slide handles before destroying the context", PYRA_BUSY );
		}
		auto owner = std::unique_ptr<pyra_context>( context );
	} );
}


pyra_status pyra_open ( pyra_context *context, const char *path_utf8,
	pyra_wsi **wsi, pyra_error *error )
{
	if (wsi)
		*wsi = nullptr;
	return api_call( error, [&] {
		require( context && wsi && path_utf8 && *path_utf8, "Context, path, and slide output are required" );
		auto guard = std::scoped_lock( context->mutex_ );
		auto owner = std::make_unique<pyra_wsi>();
		owner->context_ = context;
		owner->state_ = open_state( *context, path_utf8 );
		++context->handles_;
		*wsi = owner.release();
	} );
}


pyra_status pyra_close ( pyra_wsi *wsi, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi != nullptr, "Slide is null" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		auto owner = std::unique_ptr<pyra_wsi>( wsi );
		if (owner->state_.use_count() == 1 && !wsi->context_->openslide_) {
			const auto &stats = owner->state_->pyramid_.stats;
			wsi->context_->compressed_hits_ += stats.compressed_cache_hits;
			wsi->context_->compressed_misses_ += stats.compressed_cache_misses;
			wsi->context_->decoded_hits_ += stats.decoded_cache_hits;
			wsi->context_->decoded_misses_ += stats.decoded_cache_misses;
		}
		--wsi->context_->handles_;
	} );
}


pyra_status pyra_context_get_stats ( pyra_context *context,
	pyra_context_stats *stats, pyra_error *error )
{
	return api_call( error, [&] {
		require( context && stats && stats->struct_size >= sizeof( *stats ), "Invalid context or statistics structure" );
		auto guard = std::scoped_lock( context->mutex_ );
		*stats = {};
		stats->struct_size = sizeof( *stats );
		stats->compressed_cache_bytes = context->allocated( false );
		stats->decoded_cache_bytes = context->allocated( true );
		stats->cache_pool_evictions = context->evictions_;
		stats->compressed_cache_hits = context->compressed_hits_;
		stats->compressed_cache_misses = context->compressed_misses_;
		stats->decoded_cache_hits = context->decoded_hits_;
		stats->decoded_cache_misses = context->decoded_misses_;
		if (context->openslide_) {
			stats->decoded_cache_bytes = context->options_.decoded_cache_bytes > 0 ? PYRA_STAT_UNAVAILABLE : 0;
			stats->cache_pool_evictions = PYRA_STAT_UNAVAILABLE;
			stats->compressed_cache_hits = stats->compressed_cache_misses = PYRA_STAT_UNAVAILABLE;
			stats->decoded_cache_hits = stats->decoded_cache_misses = PYRA_STAT_UNAVAILABLE;
		}
		stats->wsi_handles = context->handles_;
		stats->render_calls = context->renders_;
		stats->incomplete_renders = context->incomplete_;
		for (const auto &weak : context->wsis_)
			if (const auto state = weak.lock()) {
				++stats->shared_wsis;
				if (!context->openslide_) {
					stats->compressed_cache_hits += state->pyramid_.stats.compressed_cache_hits;
					stats->compressed_cache_misses += state->pyramid_.stats.compressed_cache_misses;
					stats->decoded_cache_hits += state->pyramid_.stats.decoded_cache_hits;
					stats->decoded_cache_misses += state->pyramid_.stats.decoded_cache_misses;
				}
			}
	} );
}


pyra_status pyra_get_cache_usage ( pyra_wsi *wsi, int32_t level_index,
	pyra_cache_usage *usage, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && usage && usage->struct_size >= sizeof( *usage ), "Invalid WSI or cache usage structure" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		require( !wsi->state_->openslide_, "OpenSlide does not expose tile-cache slot occupancy", PYRA_UNSUPPORTED );
		const auto &pyramid = wsi->state_->pyramid_;
		require( level_index >= -1 && ( level_index < 0 || size_t( level_index ) < pyramid.levels.size() ),
			"Cache level index is out of range" );
		const auto selected = level_index >= 0 ? level_index : pyramid.stats.last_render_level;
		auto result = pyra_cache_usage{ sizeof( pyra_cache_usage ) };
		result.topology = wsi->context_->options_.cache_topology;
		result.level_index = selected;
		result.synthetic_level = selected >= 0 && pyramid.levels[selected].synthetic;
		if (result.topology == PYRA_CACHE_ARENA || ( selected >= 0 && !result.synthetic_level )) {
			const auto pool_index = result.topology == PYRA_CACHE_ARENA ? 0 : selected;
			const auto &compressed = pyramid.tbf.pools[pool_index];
			const auto &decoded = pyramid.pxb.pools[pool_index];
			result.compressed_used_slots = uint64_t( compressed.used_slots );
			result.compressed_allocated_slots = uint64_t( compressed.num_slots );
			result.decoded_used_slots = uint64_t( decoded.used_slots );
			result.decoded_allocated_slots = uint64_t( decoded.num_slots );
		}
		*usage = result;
	} );
}


pyra_status pyra_get_plane_count ( pyra_wsi *wsi, uint32_t *count, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && count, "Slide and plane count output are required" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		*count = wsi->state_->openslide_ ? 1u : (uint32_t)wsi->state_->pyramid_.planes.size();
	} );
}


pyra_status pyra_get_plane_info ( pyra_wsi *wsi, uint32_t plane_index,
	pyra_plane_info *info, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && info && info->struct_size >= sizeof( *info ), "Invalid slide or plane information structure" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		if (wsi->state_->openslide_) {
			require( plane_index == 0, "Plane index is out of range" );
			*info = wsi->state_->openslide_->plane();
			return;
		}
		const auto &plane = get_plane( *wsi->state_, plane_index );
		const auto geometry = priml::native_geometry_for_plane( wsi->state_->pyramid_, plane.plane );
		require( geometry && geometry->valid(), "Plane has no valid geometry", PYRA_UNSUPPORTED );
		*info = {};
		info->struct_size = sizeof( *info );
		info->pixel_format = public_format( plane.pixel_type );
		info->channel = plane.plane.channel.c_str();
		info->z_um = plane.plane.zlevel;
		if (const auto *unit = priml::find_property( wsi->state_->pyramid_, "pyra.z-unit" );
			unit && *unit == "index")
			info->z_um = std::numeric_limits<double>::quiet_NaN();
		info->level_count = (uint32_t)plane.ladder.size();
		const auto &bounds = geometry->metric_bounds;
		info->bounds = { bounds.tl.x, bounds.tl.y, bounds.br.x - bounds.tl.x, bounds.br.y - bounds.tl.y };
		info->native_mpp = geometry->render_mpp;
		info->source_mpp_x = geometry->source_pixel_metric.x;
		info->source_mpp_y = geometry->source_pixel_metric.y;
		info->max_sample_value = plane.pixel_type == priml::LevelPixelType::Float32
			? 1.0 : priml::plane_value_range( plane );
	} );
}


pyra_status pyra_get_property_count ( pyra_wsi *wsi, uint32_t *count, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && count, "Slide and property count output are required" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		*count = wsi->state_->openslide_ ? (uint32_t)wsi->state_->openslide_->properties().size()
			: (uint32_t)wsi->state_->pyramid_.properties.size();
	} );
}


pyra_status pyra_get_property ( pyra_wsi *wsi, uint32_t index,
	pyra_property *property, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && property, "Slide and property output are required" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		if (wsi->state_->openslide_) {
			const auto &properties = wsi->state_->openslide_->properties();
			require( index < properties.size(), "Property index is out of range" );
			*property = { properties[index].first.c_str(), properties[index].second.c_str() };
			return;
		}
		const auto &properties = wsi->state_->pyramid_.properties;
		require( index < properties.size(), "Property index is out of range" );
		*property = { properties[index].key.c_str(), properties[index].value.c_str() };
	} );
}


pyra_status pyra_get_image_layout ( pyra_wsi *wsi, const pyra_image_request *request,
	pyra_image_layout *layout, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && layout && layout->struct_size >= sizeof( *layout ), "Invalid slide or image layout structure" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		*layout = image_layout( *wsi, request );
	} );
}


pyra_status pyra_get_image ( pyra_wsi *wsi, const pyra_image_request *request,
	const pyra_image_buffer *destination, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && destination && destination->struct_size >= sizeof( *destination ) &&
			destination->data, "Invalid slide or destination buffer" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		const auto layout = image_layout( *wsi, request );
		require( destination->pixel_format == layout.pixel_format, "Destination format must match the plane format" );
		require( destination->row_stride_bytes >= layout.row_stride_bytes, "Destination row stride is too small" );
		const auto rows_before_last = size_t( layout.height - 1 );
		require( rows_before_last == 0 || destination->row_stride_bytes <=
			( std::numeric_limits<size_t>::max() - layout.row_stride_bytes ) / rows_before_last,
			"Destination stride calculation overflows size_t" );
		require( destination->capacity_bytes >= rows_before_last * destination->row_stride_bytes + layout.row_stride_bytes,
			"Destination buffer is too small", PYRA_BUFFER_TOO_SMALL );
		auto &state = *wsi->state_;
		state.last_use_ = ++wsi->context_->clock_;
		++wsi->context_->renders_;
		if (state.openslide_) {
			const auto *pixels = state.openslide_->render( *request, layout );
			for (uint32_t row = 0; row < layout.height; ++row) {
				auto *output = static_cast<unsigned char *>( destination->data ) + size_t( row ) * destination->row_stride_bytes;
				std::memcpy( output, pixels + size_t( row ) * layout.row_stride_bytes, layout.row_stride_bytes );
			}
			return;
		}
		const auto &plane = get_plane( state, request->plane_index );
		const auto before = state.pyramid_.stats.incomplete_renders;
		state.pyramid_.unsupported_codec_->store( priml::ImgCodec::None );
		const auto &r = request->rectangle;
		const auto rectangle = priml::Rtlbr<float>{
			{ float( r.x_um ), float( r.y_um ) },
			{ float( r.x_um + r.width_um ), float( r.y_um + r.height_um ) } };
		const auto dimensions = priml::Dim2D<int>{ (int)layout.width, (int)layout.height };
		priml::visit_level_pixel_type( plane.pixel_type, [&] ( auto pixel ) {
			using Pixel = decltype( pixel );
			auto pixels = priml::Pwhl<Pixel>{};
//
// The caller's buffer as the render target when its rows are whole, aligned
// pixels: a render sampled straight from cached tiles then writes there and
// skips the copy below. Generate settles every reason to decline before the
// first write, so a failed call still leaves the buffer unchanged.
			auto direct = priml::Pwhl<Pixel>{};
			const auto stride = destination->row_stride_bytes;
			if (stride % sizeof( Pixel ) == 0 && stride / sizeof( Pixel ) <= size_t( std::numeric_limits<int>::max() ) &&
				reinterpret_cast<std::uintptr_t>( destination->data ) % alignof( Pixel ) == 0)
				direct = priml::Pwhl<Pixel>( dimensions, int( stride / sizeof( Pixel ) ),
					static_cast<Pixel *>( destination->data ) );
			try {
				pixels = state.loader_->Generate<Pixel>( state.pyramid_, nullptr,
					float( request->mpp ), rectangle, &dimensions, plane.plane, direct.p ? &direct : nullptr );
			} catch (...) {
// Worker exceptions are joined and rethrown by DecodePool. Clear unfinished
// states and their pools so a subsequent request cannot wait on an abandoned tile.
				for (auto &level : state.pyramid_.levels)
					for (auto &tile : level.tiles)
						if (tile.sts.load() == priml::TileSts::Loading || tile.sts.load() == priml::TileSts::Decoding)
							tile.sts.store( priml::TileSts::Empty );
				(void)reclaim_cache( state, true );
				(void)reclaim_cache( state, false );
				throw;
			}
			if (const auto *reason = priml::img_codec_unavailable_reason( state.pyramid_.unsupported_codec_->load() ))
				throw ApiFailure{ PYRA_UNSUPPORTED, reason };
			require( pixels.p && priml::are_wh_eq( pixels, dimensions ),
				"Renderer could not produce the requested image", PYRA_IO_ERROR );
			if (pixels.p == direct.p)
				return;
			for (uint32_t row = 0; row < layout.height; ++row) {
				auto *output = static_cast<unsigned char *>( destination->data ) + size_t( row ) * destination->row_stride_bytes;
				std::memcpy( output, pixels.rowp( (int)row ), layout.row_stride_bytes );
			}
		} );
		if (state.pyramid_.stats.incomplete_renders != before) {
			++wsi->context_->incomplete_;
			throw ApiFailure{ PYRA_INCOMPLETE, "Image contains missing tiles; available pixels were returned" };
		}
	} );
}


pyra_status pyra_get_geometry ( pyra_wsi *wsi, pyra_bytes_fn sink, void *user, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && sink, "Slide and byte sink are required" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		auto &state = served_state( *wsi );
		auto bytes = std::vector<uint8_t>{};
		auto reason = std::string{};
		const auto written = priml::serialize_remote_geometry( state.pyramid_, &bytes, &reason );
		require( written, reason.c_str(), PYRA_UNSUPPORTED );
		sink( user, bytes.data(), bytes.size() );
	} );
}


pyra_status pyra_read_encoded_tile ( pyra_wsi *wsi, uint32_t level_index,
	uint32_t tile_index, pyra_bytes_fn sink, void *user, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi && sink, "Slide and byte sink are required" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		auto &state = served_state( *wsi );
		require( level_index < state.pyramid_.levels.size() &&
			tile_index <= uint32_t( std::numeric_limits<int>::max() ), "Tile index is out of range" );
		state.last_use_ = ++wsi->context_->clock_;
		auto bytes = std::vector<uint8_t>{};
		auto reason = std::string{};
		const auto read = priml::read_remote_tile( *state.loader_, state.pyramid_, int( level_index ), int( tile_index ),
			&bytes, &reason );
		require( read, reason.c_str(), PYRA_IO_ERROR );
		sink( user, bytes.data(), bytes.size() );
	} );
}


pyra_status pyra_open_remote ( pyra_context *context, const void *geometry, size_t size,
	pyra_wsi **wsi, pyra_error *error )
{
	if (wsi)
		*wsi = nullptr;
	return api_call( error, [&] {
		require( context && wsi && geometry && size > 0, "Context, geometry, and slide output are required" );
		auto guard = std::scoped_lock( context->mutex_ );
		auto owner = std::make_unique<pyra_wsi>();
		owner->context_ = context;
		owner->state_ = open_remote_state( *context, geometry, size );
		++context->handles_;
		*wsi = owner.release();
	} );
}


pyra_status pyra_put_encoded_tile ( pyra_wsi *wsi, uint32_t level_index,
	uint32_t tile_index, const void *data, size_t size, pyra_error *error )
{
	return api_call( error, [&] {
		require( wsi, "Slide is null" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		auto &state = *wsi->state_;
		require( state.remote_ != nullptr, "Tiles are put only into a remote slide", PYRA_UNSUPPORTED );
		require( level_index <= uint32_t( std::numeric_limits<int>::max() ) &&
			tile_index <= uint32_t( std::numeric_limits<int>::max() ), "Tile index is out of range" );
		auto reason = std::string{};
		const auto stored = state.remote_->PutTile( state.pyramid_, int( level_index ), int( tile_index ),
			static_cast<const uint8_t *>( data ), size, &reason );
		require( stored, reason.c_str() );
	} );
}


pyra_status pyra_take_missing_tiles ( pyra_wsi *wsi, pyra_tile_id *tiles,
	size_t capacity, size_t *count, pyra_error *error )
{
	if (count)
		*count = 0;
	return api_call( error, [&] {
		require( wsi && count && ( tiles || capacity == 0 ), "Slide, tile array, and count output are required" );
		auto guard = std::scoped_lock( wsi->context_->mutex_ );
		auto &state = *wsi->state_;
		require( state.remote_ != nullptr, "Only a remote slide has missing tiles", PYRA_UNSUPPORTED );
		const auto taken = state.remote_->TakeMissing( state.pyramid_, capacity );
		for (size_t i = 0; i < taken.size(); ++i)
			tiles[i] = { uint32_t( taken[i].level_no ), uint32_t( taken[i].tile_no ) };
		*count = taken.size();
	} );
}


namespace {

// Calls `f` with a value of the C++ pixel type of an integer public format.
template <typename F>
void visit_integer_format ( pyra_pixel_format format, F &&f )
{
	switch (format) {
	case PYRA_PIXEL_RGB8: f( priml::Rgb8{} ); return;
	case PYRA_PIXEL_RGB16: f( priml::Rgb16{} ); return;
	case PYRA_PIXEL_GRAY8: f( uint8_t{} ); return;
	case PYRA_PIXEL_GRAY16: f( uint16_t{} ); return;
	default: throw ApiFailure{ PYRA_UNSUPPORTED, "Pixel format must be RGB8, RGB16, GRAY8 or GRAY16" };
	}
}


// The rows of a caller's buffer, checked to hold `width` x `height` of `Pixel`.
template <typename Pixel>
const unsigned char *checked_rows ( const pyra_image_buffer &buffer, uint32_t width, uint32_t height )
{
	require( buffer.data && width > 0 && height > 0 && width <= uint32_t( std::numeric_limits<int>::max() ) &&
		height <= uint32_t( std::numeric_limits<int>::max() ), "Image buffer and size are required" );
	const auto row = size_t( width ) * sizeof( Pixel );
	require( buffer.row_stride_bytes >= row && buffer.capacity_bytes >= buffer.row_stride_bytes * ( height - 1 ) + row,
		"Image buffer is too small", PYRA_BUFFER_TOO_SMALL );
	return static_cast<const unsigned char *>( buffer.data );
}

} // namespace


pyra_status pyra_encode_image ( const pyra_image_buffer *source, uint32_t width, uint32_t height,
	pyra_codec codec, uint32_t quality, pyra_bytes_fn sink, void *user, pyra_error *error )
{
	return api_call( error, [&] {
		require( source && source->struct_size >= sizeof( *source ) && sink, "Source buffer and byte sink are required" );
		const auto internal = codec_from_c( codec );
		require( priml::img_codec_can_encode( internal ), "This build cannot encode the requested codec", PYRA_UNSUPPORTED );
		require( quality >= 1 && quality <= PYRA_QUALITY_LOSSLESS, "Quality must be 1..100 or PYRA_QUALITY_LOSSLESS" );
		visit_integer_format( source->pixel_format, [&] ( auto pixel ) {
			using Pixel = decltype( pixel );
			const auto *rows = checked_rows<Pixel>( *source, width, height );
			auto image = priml::Pwhl<Pixel>( priml::Dim2D<int>{ int( width ), int( height ) }, priml::uninit );
			for (auto y = 0u; y < height; ++y)
				std::memcpy( image.rowp( int( y ) ), rows + size_t( y ) * source->row_stride_bytes, size_t( width ) * sizeof( Pixel ) );
			auto options = priml::TileEncodeOptions{};
			options.quality = int( quality );
			auto bytes = std::vector<uint8_t>{};
			auto reason = std::string{};
			const auto encoded = priml::encode_tile( internal, image, &bytes, options, &reason );
			require( encoded, reason.c_str(), PYRA_UNSUPPORTED );
			sink( user, bytes.data(), bytes.size() );
		} );
	} );
}


pyra_status pyra_decode_image ( const void *data, size_t size, const pyra_image_buffer *destination,
	uint32_t *width, uint32_t *height, pyra_error *error )
{
	return api_call( error, [&] {
		require( data && size > 0 && width && height, "Codestream and size outputs are required" );
		const auto *bytes = static_cast<const uint8_t *>( data );
		if (!destination) {
			auto info = priml::ImgCodecInfo{};
			auto reason = std::string{};
			const auto known = priml::read_img_codec_info( bytes, size, &info, &reason );
			require( known && info.width > 0 && info.height > 0, reason.empty() ? "Unreadable codestream" : reason.c_str(),
				PYRA_UNSUPPORTED );
			*width = uint32_t( info.width );
			*height = uint32_t( info.height );
			return;
		}
		require( destination->struct_size >= sizeof( *destination ), "Invalid destination buffer structure" );
		visit_integer_format( destination->pixel_format, [&] ( auto pixel ) {
			using Pixel = decltype( pixel );
			auto image = priml::Pwhl<Pixel>{};
			require( priml::decode_to_image( bytes, size, image ), "The codestream could not be decoded", PYRA_UNSUPPORTED );
			*width = uint32_t( image.w );
			*height = uint32_t( image.h );
			const auto *rows = checked_rows<Pixel>( *destination, *width, *height );
			for (auto y = 0; y < image.h; ++y)
				std::memcpy( const_cast<unsigned char *>( rows ) + size_t( y ) * destination->row_stride_bytes, image.rowp( y ),
					size_t( image.w ) * sizeof( Pixel ) );
		} );
	} );
}
