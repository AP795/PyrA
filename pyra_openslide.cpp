// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "pyra_openslide.h"

#include "build_config.h"
#include "pyra_api_internal.h"

#if USE_OPENSLIDE
#include "wsi.h"

#include <openslide.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#endif

namespace pyra::detail {
#if USE_OPENSLIDE
namespace {

// Bound staging independently of the WSI dimensions, including single-level
// slides viewed at very coarse resolution. Output storage has the C API limit.
constexpr auto kReadEdge = 2048;
constexpr auto kMinimumVersion = std::array{ 4, 0, 1 };
//
// OpenSlide decodes on the calling thread, tile after tile, but reads of one
// handle may run concurrently (its tile cache and TIFF handle pool are locked).
// A render is therefore cut into row bands read by up to decode_threads
// workers. Bands are fixed slices of this many level rows, rounded up to whole
// tile rows: the cut - and with it every staging origin - does not depend on
// the worker count, so the output does not either, and no tile row is decoded
// by two bands.
constexpr auto kBandRows = int64_t{ 128 };

struct SlideCloser {
	void operator() ( openslide_t *slide ) const { openslide_close( slide ); }
};

struct CacheCloser {
	void operator() ( openslide_cache_t *cache ) const { openslide_cache_release( cache ); }
};

using Slide = std::unique_ptr<openslide_t, SlideCloser>;
using Cache = std::unique_ptr<openslide_cache_t, CacheCloser>;

void check_slide ( openslide_t *slide )
{
	if (const auto *error = openslide_get_error( slide ))
		throw ApiFailure{ PYRA_IO_ERROR, std::string( "OpenSlide: " ) + error };
}


void check_version ()
{
	auto text = std::string_view( openslide_get_version() );
	auto version = std::array<int, kMinimumVersion.size()>{};
	auto valid = true;
	for (auto i = size_t{}; i < version.size(); ++i) {
		const auto parsed = std::from_chars( text.data(), text.data() + text.size(), version[i] );
		if (parsed.ec != std::errc{} || ( i + 1 < version.size() &&
			( parsed.ptr == text.data() + text.size() || *parsed.ptr != '.' ) )) {
			valid = false;
			break;
		}
		if (i + 1 < version.size())
			text.remove_prefix( size_t( parsed.ptr - text.data() ) + 1 );
	}
	if (!valid || version < kMinimumVersion)
		throw ApiFailure{ PYRA_UNSUPPORTED, std::format( "OpenSlide >= {}.{}.{} is required",
			kMinimumVersion[0], kMinimumVersion[1], kMinimumVersion[2] ) };
}


double property_number ( openslide_t *slide, const char *key )
{
	const auto *value = openslide_get_property_value( slide, key );
	if (!value)
		return 0;
	const auto text = std::string_view( value );
	auto result = double{};
	const auto parsed = std::from_chars( text.data(), text.data() + text.size(), result );
	return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
		std::isfinite( result ) && result > 0 ? result : 0;
}


struct Band {
	uint32_t first = 0;
	uint32_t end = 0;
};


struct SampleAxis;


// One worker's render scratch, reused across calls and never value-initialised:
// OpenSlide clears the staging itself.
struct Scratch {
	std::unique_ptr<uint32_t[]> staging;
	size_t staging_pixels = 0;
	std::vector<int32_t> columns;

	uint32_t *reserve ( size_t pixels ) {
		if (pixels > staging_pixels) {
			staging = std::make_unique_for_overwrite<uint32_t[]>( pixels );
			staging_pixels = pixels;
		}
		return staging.get();
	}
};


class Reader final : public OpenSlideReader {
public:
	Reader ( const std::string &path, openslide_cache_t *cache, priml::DecodePool &pool, unsigned threads )
		: slide_( openslide_open( path.c_str() ) ), pool_( pool ), threads_( std::max( 1u, threads ) )
	{
		require( slide_ != nullptr, "OpenSlide does not support this source", PYRA_UNSUPPORTED );
		check_slide( slide_.get() );
		openslide_set_cache( slide_.get(), cache );
		openslide_get_level0_dimensions( slide_.get(), &width_, &height_ );
		const auto levels = openslide_get_level_count( slide_.get() );
		check_slide( slide_.get() );
		require( width_ > 0 && height_ > 0 && levels > 0,
			"OpenSlide returned invalid pyramid dimensions", PYRA_IO_ERROR );
		require( width_ <= std::numeric_limits<int>::max() && height_ <= std::numeric_limits<int>::max(),
			"OpenSlide source dimensions exceed the renderer range", PYRA_LIMIT_EXCEEDED );
		plane_.struct_size = sizeof( plane_ );
		plane_.pixel_format = PYRA_PIXEL_RGB8;
		plane_.level_count = uint32_t( levels );
		plane_.channel = "";
		plane_.source_mpp_x = property_number( slide_.get(), OPENSLIDE_PROPERTY_NAME_MPP_X );
		plane_.source_mpp_y = property_number( slide_.get(), OPENSLIDE_PROPERTY_NAME_MPP_Y );
// Match the native reader's scalar-X rendering geometry and 1 um fallback.
		plane_.native_mpp = plane_.source_mpp_x > 0 ? plane_.source_mpp_x : 1;
		plane_.bounds = { 0, 0, width_ * plane_.native_mpp, height_ * plane_.native_mpp };
		plane_.max_sample_value = 255;
		require( std::isfinite( plane_.bounds.width_um ) && std::isfinite( plane_.bounds.height_um ),
			"OpenSlide metric bounds overflow", PYRA_LIMIT_EXCEEDED );
		for (auto names = openslide_get_property_names( slide_.get() ); names && *names; ++names) {
			if (const auto *value = openslide_get_property_value( slide_.get(), *names )) {
				properties_.emplace_back( *names, value );
				const auto name = std::string_view( *names );
				if (name.starts_with( "openslide." ))
					properties_.emplace_back( "pyra." + std::string( name.substr( 10 ) ), value );
			}
		}
		properties_.emplace_back( "pyra.backend", "openslide" );
		properties_.emplace_back( "pyra.backend-version", openslide_get_version() );
		properties_.emplace_back( "pyra.render-mpp", std::to_string( plane_.native_mpp ) );
		properties_.emplace_back( "pyra.mpp-fallback", plane_.source_mpp_x > 0 ? "false" : "true" );
		for (auto level = 0; level < levels; ++level) {
			const auto key = std::format( "openslide.level[{}].tile-height", level );
			const auto tile_rows = int64_t( property_number( slide_.get(), key.c_str() ) );
			band_rows_.push_back( tile_rows > 0
				? ( kBandRows + tile_rows - 1 ) / tile_rows * tile_rows : kBandRows );
		}
		check_slide( slide_.get() );
	}

	const pyra_plane_info &plane () const override { return plane_; }
	const std::vector<std::pair<std::string, std::string>> &properties () const override { return properties_; }
	const unsigned char *render ( const pyra_image_request &request,
		const pyra_image_layout &layout ) override;

private:
	unsigned char *output ( size_t bytes );
	void read_band ( const Band &band, int32_t level, double downsample, const SampleAxis &xs,
		const SampleAxis &ys, size_t stride, unsigned char *output, Scratch &scratch );

	Slide slide_;
	priml::DecodePool &pool_;
	unsigned threads_ = 1;
	int64_t width_ = 0;
	int64_t height_ = 0;
	pyra_plane_info plane_{};
	std::vector<std::pair<std::string, std::string>> properties_;
	std::vector<int64_t> band_rows_;
// Render scratch, reused across calls (renders are serialised by the context
// mutex): one per worker, and the output, never value-initialised - every
// output byte is either sampled or explicitly zeroed.
	std::vector<Scratch> scratch_;
	std::unique_ptr<unsigned char[]> output_;
	size_t output_bytes_ = 0;
};


struct SampleAxis {
	std::vector<double> centres_;
	uint32_t first_ = 0;
	uint32_t end_ = 0;

	SampleAxis ( double origin, double extent, uint32_t size, double mpp, int64_t source_size )
		: centres_( size ), first_( size ), end_( size )
	{
		for (uint32_t i = 0; i < size; ++i) {
			const auto centre = ( origin + ( i + 0.5 ) * ( extent / size ) ) / mpp;
			centres_[i] = centre;
			if (centre >= 0 && centre < double( source_size )) {
				first_ = std::min( first_, i );
				end_ = i + 1;
			}
		}
	}

	uint32_t block_end ( uint32_t first, double downsample ) const {
		const auto limit = centres_[first] + ( kReadEdge - 2 ) * downsample;
		auto end = first + 1;
		while (end < end_ && centres_[end] < limit)
			++end;
		return end;
	}
};


// The source is premultiplied ARGB. Dropping alpha composites on black,
// preserving the C API's zero background without an ICC/display transform.
inline void store_rgb ( uint32_t pixel, unsigned char *destination )
{
	destination[0] = (unsigned char)( pixel >> 16 );
	destination[1] = (unsigned char)( pixel >> 8 );
	destination[2] = (unsigned char)pixel;
}


// Zero what sampling never writes: rows and columns whose sample centres fall
// outside the source plane.
void clear_uncovered ( unsigned char *output, const pyra_image_layout &layout,
	const SampleAxis &xs, const SampleAxis &ys )
{
	const auto stride = layout.row_stride_bytes;
	const auto left = size_t( xs.first_ ) * 3;
	const auto right = size_t( xs.end_ ) * 3;
	for (uint32_t row = 0; row < layout.height; ++row) {
		auto *line = output + size_t( row ) * stride;
		if (row < ys.first_ || row >= ys.end_ || left >= right) {
			std::memset( line, 0, stride );
			continue;
		}
		std::memset( line, 0, left );
		std::memset( line + right, 0, stride - right );
	}
}


// The bands of output rows [ys.first_, ys.end_): runs of consecutive rows whose
// level row lies in the same band_rows-aligned slice, each also within one
// staging block.
std::vector<Band> row_bands ( const SampleAxis &ys, double downsample, int64_t band_rows )
{
	const auto slice = [&] ( uint32_t row ) {
		return int64_t( std::floor( ys.centres_[row] / downsample ) ) / band_rows;
	};
	auto bands = std::vector<Band>{};
	for (auto y = ys.first_; y < ys.end_;) {
		const auto limit = ys.block_end( y, downsample );
		const auto first_slice = slice( y );
		auto end = y + 1;
		while (end < limit && slice( end ) == first_slice)
			++end;
		bands.push_back( { y, end } );
		y = end;
	}
	return bands;
}


unsigned char *Reader::output ( size_t bytes )
{
	if (bytes > output_bytes_) {
		output_ = std::make_unique_for_overwrite<unsigned char[]>( bytes );
		output_bytes_ = bytes;
	}
	return output_.get();
}


const unsigned char *Reader::render ( const pyra_image_request &request,
	const pyra_image_layout &layout )
{
	check_slide( slide_.get() );
	const auto level = openslide_get_best_level_for_downsample( slide_.get(), request.mpp / plane_.native_mpp );
	const auto downsample = openslide_get_level_downsample( slide_.get(), level );
	check_slide( slide_.get() );
	require( std::isfinite( downsample ) && downsample >= 1,
		"OpenSlide returned an invalid downsample", PYRA_IO_ERROR );
	const auto &r = request.rectangle;
	const auto xs = SampleAxis( r.x_um, r.width_um, layout.width, plane_.native_mpp, width_ );
	const auto ys = SampleAxis( r.y_um, r.height_um, layout.height, plane_.native_mpp, height_ );
	require( level >= 0 && size_t( level ) < band_rows_.size(),
		"OpenSlide returned an invalid level", PYRA_IO_ERROR );
	auto *output = this->output( layout.capacity_bytes );
	clear_uncovered( output, layout, xs, ys );
	const auto bands = row_bands( ys, downsample, band_rows_[size_t( level )] );
	const auto workers = (unsigned)std::min<size_t>( threads_, bands.size() );
	if (workers == 0)
		return output;
	if (scratch_.size() < workers)
		scratch_.resize( workers );
// Workers take bands in order; bands write disjoint output rows. A failing
// band stops the others from starting new ones; the pool rethrows its error.
	auto next = std::atomic<size_t>{ 0 };
	auto failed = std::atomic<bool>{ false };
	pool_.run_batch( workers, [&] ( unsigned worker ) {
		auto &scratch = scratch_[worker];
		for (auto index = next.fetch_add( 1 ); index < bands.size() && !failed.load();
			index = next.fetch_add( 1 )) {
			try {
				read_band( bands[index], level, downsample, xs, ys, layout.row_stride_bytes, output, scratch );
			} catch (...) {
				failed.store( true );
				throw;
			}
		}
	} );
	return output;
}


// Read one band's source rows block by block and sample them into its output
// rows.
void Reader::read_band ( const Band &band, int32_t level, double downsample, const SampleAxis &xs,
	const SampleAxis &ys, size_t stride, unsigned char *output, Scratch &scratch )
{
	const auto origin_y = int64_t( std::floor( ys.centres_[band.first] / downsample ) * downsample );
	const auto height = int64_t( std::floor( ( ys.centres_[band.end - 1] - origin_y ) / downsample ) ) + 1;
	for (auto x = xs.first_; x < xs.end_;) {
		const auto x_end = xs.block_end( x, downsample );
		const auto origin_x = int64_t( std::floor( xs.centres_[x] / downsample ) * downsample );
		const auto width = int64_t( std::floor( ( xs.centres_[x_end - 1] - origin_x ) / downsample ) ) + 1;
		require( width > 0 && height > 0 && width <= kReadEdge && height <= kReadEdge,
			"OpenSlide staging rectangle exceeds its bound", PYRA_LIMIT_EXCEEDED );
		auto *source = scratch.reserve( size_t( width * height ) );
		openslide_read_region( slide_.get(), source, origin_x, origin_y, level, width, height );
		check_slide( slide_.get() );
// Source columns once per block; at a 1:1 level they are consecutive.
		const auto count = size_t( x_end - x );
		auto &columns = scratch.columns;
		columns.resize( count );
		auto consecutive = true;
		for (size_t i = 0; i < count; ++i) {
			columns[i] = int32_t( std::floor( ( xs.centres_[x + i] - origin_x ) / downsample ) );
			consecutive = consecutive && columns[i] == columns[0] + int32_t( i );
		}
		for (auto row = band.first; row < band.end; ++row) {
			const auto sy = int64_t( std::floor( ( ys.centres_[row] - origin_y ) / downsample ) );
			const auto *line = source + size_t( sy * width );
			auto *destination = output + size_t( row ) * stride + size_t( x ) * 3;
			if (consecutive) {
				const auto *pixel = line + columns[0];
				for (size_t i = 0; i < count; ++i, destination += 3)
					store_rgb( pixel[i], destination );
			} else {
				for (size_t i = 0; i < count; ++i, destination += 3)
					store_rgb( line[columns[i]], destination );
			}
		}
		x = x_end;
	}
}


class Context final : public OpenSlideContext {
public:
	Context ( size_t capacity, unsigned threads )
		: cache_( openslide_cache_create( capacity ) ), threads_( std::max( 1u, threads ) ) {
		require( cache_ != nullptr, "OpenSlide cache allocation failed", PYRA_OUT_OF_MEMORY );
	}
	std::unique_ptr<OpenSlideReader> open ( const std::string &path ) override {
		return std::make_unique<Reader>( path, cache_.get(), pool_, threads_ );
	}

private:
	Cache cache_;
// Band workers shared by every slide of the context; renders are serialised by
// the context mutex, so one batch runs at a time.
	priml::DecodePool pool_;
	unsigned threads_ = 1;
};

} // namespace
#endif


const char *openslide_version ()
{
#if USE_OPENSLIDE
	return openslide_get_version();
#else
	return nullptr;
#endif
}


std::unique_ptr<OpenSlideContext> make_openslide_context ( const pyra_context_options &options )
{
#if USE_OPENSLIDE
	check_version();
	return std::make_unique<Context>( size_t( options.decoded_cache_bytes ), options.decode_threads );
#else
	(void)options;
	throw ApiFailure{ PYRA_UNSUPPORTED, "OpenSlide backend is disabled; configure PYRA_WITH_OPENSLIDE=ON" };
#endif
}

} // namespace pyra::detail
