#include "pyra_openslide.h"

#include "build_config.h"
#include "pyra_api_internal.h"

#if USE_OPENSLIDE
#include <openslide.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <cstdint>
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


class Reader final : public OpenSlideReader {
public:
	Reader ( const std::string &path, openslide_cache_t *cache )
		: slide_( openslide_open( path.c_str() ) )
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
		check_slide( slide_.get() );
	}

	const pyra_plane_info &plane () const override { return plane_; }
	const std::vector<std::pair<std::string, std::string>> &properties () const override { return properties_; }
	std::vector<unsigned char> render ( const pyra_image_request &request,
		const pyra_image_layout &layout ) override;

private:
	Slide slide_;
	int64_t width_ = 0;
	int64_t height_ = 0;
	pyra_plane_info plane_{};
	std::vector<std::pair<std::string, std::string>> properties_;
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


std::vector<unsigned char> Reader::render ( const pyra_image_request &request,
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
	auto output = std::vector<unsigned char>( layout.capacity_bytes, 0 );
	auto source = std::vector<uint32_t>{};
	for (auto y = ys.first_; y < ys.end_;) {
		const auto y_end = ys.block_end( y, downsample );
		const auto origin_y = int64_t( std::floor( ys.centres_[y] / downsample ) * downsample );
		const auto height = int64_t( std::floor( ( ys.centres_[y_end - 1] - origin_y ) / downsample ) ) + 1;
		for (auto x = xs.first_; x < xs.end_;) {
			const auto x_end = xs.block_end( x, downsample );
			const auto origin_x = int64_t( std::floor( xs.centres_[x] / downsample ) * downsample );
			const auto width = int64_t( std::floor( ( xs.centres_[x_end - 1] - origin_x ) / downsample ) ) + 1;
			require( width > 0 && height > 0 && width <= kReadEdge && height <= kReadEdge,
				"OpenSlide staging rectangle exceeds its bound", PYRA_LIMIT_EXCEEDED );
			source.resize( size_t( width * height ) );
			openslide_read_region( slide_.get(), source.data(), origin_x, origin_y, level, width, height );
			check_slide( slide_.get() );
			for (auto row = y; row < y_end; ++row) {
				const auto sy = int64_t( std::floor( ( ys.centres_[row] - origin_y ) / downsample ) );
				for (auto column = x; column < x_end; ++column) {
					const auto sx = int64_t( std::floor( ( xs.centres_[column] - origin_x ) / downsample ) );
// The source is premultiplied ARGB. Dropping alpha composites on black,
// preserving the C API's zero background without an ICC/display transform.
					const auto pixel = source[size_t( sy * width + sx )];
					auto *destination = output.data() + size_t( row ) * layout.row_stride_bytes + size_t( column ) * 3;
					destination[0] = (unsigned char)( pixel >> 16 );
					destination[1] = (unsigned char)( pixel >> 8 );
					destination[2] = (unsigned char)pixel;
				}
			}
			x = x_end;
		}
		y = y_end;
	}
	return output;
}


class Context final : public OpenSlideContext {
public:
	explicit Context ( size_t capacity ) : cache_( openslide_cache_create( capacity ) ) {
		require( cache_ != nullptr, "OpenSlide cache allocation failed", PYRA_OUT_OF_MEMORY );
	}
	std::unique_ptr<OpenSlideReader> open ( const std::string &path ) override {
		return std::make_unique<Reader>( path, cache_.get() );
	}

private:
	Cache cache_;
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
	return std::make_unique<Context>( size_t( options.decoded_cache_bytes ) );
#else
	(void)options;
	throw ApiFailure{ PYRA_UNSUPPORTED, "OpenSlide backend is disabled; configure PYRA_WITH_OPENSLIDE=ON" };
#endif
}

} // namespace pyra::detail
