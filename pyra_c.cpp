#include "pyra/pyra.h"

#include "pyra_api_internal.h"
#include "pyra_openslide.h"
#include "wsi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr auto kDefaultCacheBytes = uint64_t{ 256 } * 1024 * 1024;
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
	auto bytes = size_t{ 0 };
	for (const auto &level : state.pyramid_.levels)
		bytes += decoded ? ( level.pix ? level.pix->allocated_bytes() : 0 )
			: level.tbf.allocated_bytes();
	return bytes;
}


// Called only for another, idle slide under the owning context's mutex. Free
// backing allocations rather than merely marking their slots reusable.
size_t reclaim_cache ( WsiState &state, bool decoded )
{
	const auto before = cache_bytes( state, decoded );
	for (auto &level : state.pyramid_.levels) {
		if (level.synthetic)
			continue;
		if (std::any_of( level.tiles.begin(), level.tiles.end(), [] ( const auto &tile ) {
			return tile.pin_count.load() > 0 || tile.sts.load() == priml::TileSts::Loading ||
				tile.sts.load() == priml::TileSts::Decoding;
		} ))
			continue;
		for (auto &tile : level.tiles) {
			if (decoded) {
				tile.nixbuf = -1;
				tile.pbd_w = tile.pbd_h = tile.pbd_l = 0;
				tile.sts.store( tile.ninbuf >= 0 ? priml::TileSts::Loaded : priml::TileSts::Empty );
			} else {
				tile.ninbuf = -1;
				tile.bf_sz = 0;
				if (tile.sts.load() == priml::TileSts::Loaded)
					tile.sts.store( priml::TileSts::Empty );
			}
		}
		if (decoded && level.pix) {
			priml::visit_level_pixel_type( level.pixel_type, [&] ( auto pixel ) {
				auto &pool = priml::level_pix<decltype( pixel )>( level ).pxb;
				const auto max_slots = pool.max_slots;
				const auto dimensions = priml::Dim2D<int>{ pool.slot_w, pool.slot_h };
				pool = {};
				pool.init( dimensions, max_slots );
			} );
		} else if (!decoded) {
			const auto slot_bytes = level.tbf.slot_bytes();
			const auto max_slots = level.tbf.max_slots;
			level.tbf = {};
			level.tbf.init( (int)slot_bytes, max_slots );
		}
	}
	return before - cache_bytes( state, decoded );
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
	auto &pyramid = state->pyramid_;
	pyramid.decode_threads = (int)context.options_.decode_threads;
	pyramid.ram_cap_bytes = 0;
	pyramid.tbf_cap_bytes = (size_t)context.options_.compressed_cache_bytes;
	pyramid.pxb_cap_bytes = (size_t)context.options_.decoded_cache_bytes;
	pyramid.use_pixbf = context.options_.decoded_cache_bytes > 0;
	pyramid.cache_growth_budget = [&context, requester = state.get()] ( bool decoded, size_t wanted ) {
// Coarse-level construction needs temporary decoded tile scratch even when
// retained decoded caching is disabled. It is released before open returns.
		if (decoded && context.options_.decoded_cache_bytes == 0 && requester->opening_)
			return wanted;
		return context.growth_budget( requester, decoded, wanted );
	};
	state->loader_ = priml::WsiLoader::Create( filename );
	require( state->loader_ != nullptr, "No enabled loader supports this source", PYRA_UNSUPPORTED );
	auto type = priml::LevelPixelType::Rgb8;
	priml::ProbeWsiPixelType( filename, &type );
	const auto opened = priml::visit_level_pixel_type( type, [&] ( auto pixel ) {
		return state->loader_->OpenFile<decltype( pixel )>( filename, pyramid );
	} );
	if (const auto *reason = priml::img_codec_unavailable_reason( pyramid.unsupported_codec_->load() ))
		throw ApiFailure{ PYRA_UNSUPPORTED, reason };
	require( opened, "Unable to open slide", PYRA_IO_ERROR );
	state->opening_ = false;
	if (context.options_.decoded_cache_bytes == 0)
		(void)reclaim_cache( *state, true );
	require( !pyramid.planes.empty(), "Slide has no supported image planes", PYRA_UNSUPPORTED );
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
	options->decode_threads = 1;
	options->backend = PYRA_BACKEND_NATIVE;
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
		if (context->openslide_) {
			stats->decoded_cache_bytes = context->options_.decoded_cache_bytes > 0 ? PYRA_STAT_UNAVAILABLE : 0;
			stats->cache_pool_evictions = PYRA_STAT_UNAVAILABLE;
		}
		stats->wsi_handles = context->handles_;
		stats->render_calls = context->renders_;
		stats->incomplete_renders = context->incomplete_;
		for (const auto &weak : context->wsis_)
			stats->shared_wsis += !weak.expired();
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
			const auto pixels = state.openslide_->render( *request, layout );
			for (uint32_t row = 0; row < layout.height; ++row) {
				auto *output = static_cast<unsigned char *>( destination->data ) + size_t( row ) * destination->row_stride_bytes;
				std::memcpy( output, pixels.data() + size_t( row ) * layout.row_stride_bytes, layout.row_stride_bytes );
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
			auto pixels = priml::Pwhl<decltype( pixel )>{};
			try {
				pixels = state.loader_->Generate<decltype( pixel )>( state.pyramid_, nullptr,
					float( request->mpp ), rectangle, &dimensions, plane.plane );
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
			require( pixels.p && pixels.w == dimensions.w && pixels.h == dimensions.h,
				"Renderer could not produce the requested image", PYRA_IO_ERROR );
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
