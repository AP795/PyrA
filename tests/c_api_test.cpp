#include <pyra/pyra.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto kFixtureEdge = 32u;
constexpr auto kFixtureValue = 117;
constexpr auto kTolerance = 3;
constexpr auto kPadding = size_t{ 7 };
constexpr auto kSentinel = uint8_t{ 0xcd };

void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


void test_backends ()
{
	auto info = pyra_backend_info{ sizeof( pyra_backend_info ) };
	require( pyra_get_backend_info( PYRA_BACKEND_NATIVE, &info, nullptr ) == PYRA_OK &&
		info.available && info.name && info.version, "Native backend information is missing" );
	require( pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &info, nullptr ) == PYRA_OK && info.name,
		"OpenSlide backend information is missing" );
	const auto available = info.available != 0;
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	require( options.backend == PYRA_BACKEND_NATIVE, "Native is not the default backend" );
	options.backend = PYRA_BACKEND_OPENSLIDE;
	auto *context = static_cast<pyra_context *>( nullptr );
	auto error = pyra_error{};
	const auto status = pyra_context_create( &options, &context, &error );
	require( available ? status == PYRA_OK && context :
		status == PYRA_UNSUPPORTED && !context && std::strstr( error.message, "PYRA_WITH_OPENSLIDE" ),
		"Optional backend availability contract differs" );
	if (context)
		pyra_context_destroy( context, nullptr );
	options.backend = UINT32_MAX;
	require( pyra_context_create( &options, &context, nullptr ) == PYRA_INVALID_ARGUMENT && !context,
		"Unknown backend was accepted" );
	require( pyra_get_backend_info( UINT32_MAX, &info, nullptr ) == PYRA_INVALID_ARGUMENT,
		"Unknown backend information was accepted" );
}


std::string utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return { text.begin(), text.end() };
}


struct Context {
	pyra_context *value_ = nullptr;

	explicit Context ( const pyra_context_options *options = nullptr ) {
		auto error = pyra_error{};
		require( pyra_context_create( options, &value_, &error ) == PYRA_OK, error.message );
	}

	~Context () { pyra_context_destroy( value_, nullptr ); }
	Context ( const Context & ) = delete;
	Context &operator= ( const Context & ) = delete;
};


struct Wsi {
	pyra_wsi *value_ = nullptr;

	Wsi ( pyra_context *context, const std::filesystem::path &path ) {
		auto error = pyra_error{};
		require( pyra_open( context, utf8( path ).c_str(), &value_, &error ) == PYRA_OK, error.message );
	}

	~Wsi () { if (value_) pyra_close( value_, nullptr ); }
	Wsi ( const Wsi & ) = delete;
	Wsi &operator= ( const Wsi & ) = delete;
};


pyra_image_request request ( double x = 0, double y = 0, double width = kFixtureEdge,
	double height = kFixtureEdge, double mpp = 1 )
{
	return { sizeof( pyra_image_request ), 0, { x, y, width, height }, mpp };
}


pyra_image_layout layout ( pyra_wsi *wsi, const pyra_image_request &image )
{
	auto value = pyra_image_layout{};
	value.struct_size = sizeof( value );
	auto error = pyra_error{};
	require( pyra_get_image_layout( wsi, &image, &value, &error ) == PYRA_OK, error.message );
	return value;
}


pyra_context_stats stats ( pyra_context *context )
{
	auto value = pyra_context_stats{};
	value.struct_size = sizeof( value );
	require( pyra_context_get_stats( context, &value, nullptr ) == PYRA_OK, "Statistics failed" );
	return value;
}


void render_fixture ( pyra_wsi *wsi )
{
	const auto image = request();
	const auto dimensions = layout( wsi, image );
	auto pixels = std::vector<uint8_t>( dimensions.capacity_bytes );
	auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), dimensions.pixel_format,
		pixels.data(), pixels.size(), dimensions.row_stride_bytes };
	auto error = pyra_error{};
	require( pyra_get_image( wsi, &image, &buffer, &error ) == PYRA_OK, error.message );
	for (auto pixel : pixels)
		require( std::abs( int( pixel ) - kFixtureValue ) <= kTolerance, "Rendered fixture pixel differs" );
}


void test_queries_and_ownership ( const std::filesystem::path &path )
{
	auto context = Context{};
	auto first = Wsi( context.value_, path );
	auto second = Wsi( context.value_, path.parent_path() / "." / path.filename() );
	const auto shared = stats( context.value_ );
	require( shared.shared_wsis == 1 && shared.wsi_handles == 2, "Repeated opens did not share source storage" );
	require( pyra_context_destroy( context.value_, nullptr ) == PYRA_BUSY, "Context destruction ignored live slides" );
	auto count = uint32_t{};
	require( pyra_get_plane_count( first.value_, &count, nullptr ) == PYRA_OK && count == 1, "Plane count differs" );
	auto info = pyra_plane_info{};
	info.struct_size = sizeof( info );
	require( pyra_get_plane_info( first.value_, 0, &info, nullptr ) == PYRA_OK &&
		info.pixel_format == PYRA_PIXEL_GRAY8 && info.native_mpp == 1 &&
		info.bounds.width_um == kFixtureEdge && info.max_sample_value == 255, "Plane information differs" );
	require( pyra_get_property_count( first.value_, &count, nullptr ) == PYRA_OK && count > 0, "Missing metadata" );
	auto property = pyra_property{};
	require( pyra_get_property( first.value_, 0, &property, nullptr ) == PYRA_OK &&
		property.key && property.value, "Missing metadata strings" );
	auto expected_keys = std::vector<std::string>{ "pyra.vendor", "pyra.mpp-x",
		"pyra.mpp-y", "pyra.level-count", "pyra.level[0].width" };
	for (auto index = uint32_t{ 0 }; index < count; ++index) {
		require( pyra_get_property( first.value_, index, &property, nullptr ) == PYRA_OK &&
			property.key && property.value, "Missing metadata strings" );
		std::erase( expected_keys, property.key );
	}
	require( expected_keys.empty(), "Missing normalized Pyra metadata keys" );
	require( pyra_close( first.value_, nullptr ) == PYRA_OK, "Close failed" );
	first.value_ = nullptr;
	render_fixture( second.value_ );
	require( stats( context.value_ ).wsi_handles == 1, "Closing a shared handle removed another handle" );
}


void test_layout_and_destination ( const std::filesystem::path &path )
{
	auto context = Context{};
	auto wsi = Wsi( context.value_, path );
	const auto image = request( -2, -1, 8, 7 );
	const auto dimensions = layout( wsi.value_, image );
	require( dimensions.width == 8 && dimensions.height == 7, "Layout dimensions differ" );
	const auto stride = dimensions.row_stride_bytes + kPadding;
	auto pixels = std::vector<uint8_t>( stride * dimensions.height, kSentinel );
	auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), dimensions.pixel_format,
		pixels.data(), pixels.size(), stride };
	require( pyra_get_image( wsi.value_, &image, &buffer, nullptr ) == PYRA_OK, "Padded render failed" );
	for (uint32_t y = 0; y < dimensions.height; ++y) {
		for (uint32_t x = 0; x < dimensions.width; ++x) {
			const auto pixel = pixels[y * stride + x];
			require( x < 2 || y < 1 ? pixel == 0 : std::abs( int( pixel ) - kFixtureValue ) <= kTolerance,
				"Out-of-bounds sampling stretched or changed the image" );
		}
		for (auto x = dimensions.row_stride_bytes; x < stride; ++x)
			require( pixels[y * stride + x] == kSentinel, "Renderer overwrote row padding" );
	}
	std::fill( pixels.begin(), pixels.end(), kSentinel );
	buffer.capacity_bytes = 1;
	require( pyra_get_image( wsi.value_, &image, &buffer, nullptr ) == PYRA_BUFFER_TOO_SMALL,
		"Undersized destination was accepted" );
	require( std::all_of( pixels.begin(), pixels.end(), [] ( auto p ) { return p == kSentinel; } ),
		"Rejected call modified destination" );
	buffer.capacity_bytes = pixels.size();
	buffer.row_stride_bytes = SIZE_MAX;
	require( pyra_get_image( wsi.value_, &image, &buffer, nullptr ) == PYRA_INVALID_ARGUMENT,
		"Overflowing stride was accepted" );
	buffer.row_stride_bytes = stride;
	buffer.pixel_format = PYRA_PIXEL_RGB8;
	require( pyra_get_image( wsi.value_, &image, &buffer, nullptr ) == PYRA_INVALID_ARGUMENT,
		"Mismatched destination format was accepted" );
	const auto fractional = layout( wsi.value_, request( 0, 0, 32, 24, 0.75 ) );
	require( fractional.width == 43 && fractional.height == 32, "Fractional resolution rounding differs" );
	const auto snapped = layout( wsi.value_, request( 0, 0, 12.8, 6.4, 0.1 ) );
	require( snapped.width == 128 && snapped.height == 64, "Integer raster ratio did not snap" );
	auto invalid = request();
	invalid.mpp = std::numeric_limits<double>::quiet_NaN();
	auto rejected = pyra_image_layout{ sizeof( pyra_image_layout ) };
	require( pyra_get_image_layout( wsi.value_, &invalid, &rejected, nullptr ) == PYRA_INVALID_ARGUMENT,
		"Nonfinite mpp was accepted" );
	invalid = request( 0, 0, 1e100, 1e100 );
	require( pyra_get_image_layout( wsi.value_, &invalid, &rejected, nullptr ) == PYRA_LIMIT_EXCEEDED,
		"Unrepresentable geometry was accepted" );
}


void test_shared_cache_eviction ( const std::filesystem::path &path )
{
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	{
		auto baseline = Context{};
		auto wsi = Wsi( baseline.value_, path );
		render_fixture( wsi.value_ );
		options.compressed_cache_bytes = stats( baseline.value_ ).compressed_cache_bytes;
	}
	require( options.compressed_cache_bytes > 0, "Fixture has no compressed cache allocation" );
	options.decoded_cache_bytes = kFixtureEdge * kFixtureEdge;
	auto context = Context( &options );
	auto first = Wsi( context.value_, path );
	const auto other_path = path.parent_path() / "second.ptf";
	std::filesystem::copy_file( path, other_path, std::filesystem::copy_options::overwrite_existing );
	auto second = Wsi( context.value_, other_path );
	for (auto i = 0; i < 6; ++i) {
		render_fixture( i % 2 ? second.value_ : first.value_ );
		const auto current = stats( context.value_ );
		require( current.compressed_cache_bytes <= options.compressed_cache_bytes &&
			current.decoded_cache_bytes <= options.decoded_cache_bytes, "Shared cache cap exceeded" );
	}
	require( stats( context.value_ ).cache_pool_evictions >= 10,
		"Compressed and decoded backing storage was not reclaimed across slides" );
	auto failures = std::atomic<unsigned>{ 0 };
	auto threads = std::vector<std::thread>{};
	for (auto i = 0; i < 4; ++i) {
		threads.emplace_back( [&, i] {
			try {
				for (auto repeat = 0; repeat < 4; ++repeat)
					render_fixture( i % 2 ? second.value_ : first.value_ );
			} catch (...) {
				++failures;
			}
		} );
	}
	for (auto &thread : threads)
		thread.join();
	require( failures == 0, "Concurrent calls failed" );
}


void test_unsupported_compression ( const std::filesystem::path &directory )
{
	for (const auto decoded : { true, false }) {
		auto options = pyra_context_options{};
		pyra_context_options_init( &options );
		if (!decoded)
			options.decoded_cache_bytes = 0;
		auto context = Context( &options );
		auto wsi = Wsi( context.value_, directory / "unsupported.ptf" );
		const auto image = request();
		const auto dimensions = layout( wsi.value_, image );
		auto pixels = std::vector<uint8_t>( dimensions.capacity_bytes, kSentinel );
		auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), dimensions.pixel_format,
			pixels.data(), pixels.size(), dimensions.row_stride_bytes };
		for (auto attempt = 0; attempt < 2; ++attempt) {
			auto error = pyra_error{};
			require( pyra_get_image( wsi.value_, &image, &buffer, &error ) == PYRA_UNSUPPORTED,
				"Unsupported compression was reported as a generic incomplete image" );
			require( std::string( error.message ).find( "JPEG-LS" ) != std::string::npos,
				"Unsupported-compression error omitted its codec" );
			require( std::all_of( pixels.begin(), pixels.end(), [] ( auto value ) { return value == kSentinel; } ),
				"Unsupported render modified the destination" );
		}
		auto supported = Wsi( context.value_, directory / "standalone.ptf" );
		render_fixture( supported.value_ );
	}
}


void test_color_storage ( const std::filesystem::path &directory )
{
	auto context = Context{};
	for (const auto wide : { false, true }) {
#if !PYRA_TEST_WITH_OPENJPEG
		if (wide)
			continue;
#endif
		auto wsi = Wsi( context.value_, directory / ( wide ? "rgb16.ptf" : "rgb8.ptf" ) );
		const auto image = request();
		const auto dimensions = layout( wsi.value_, image );
		require( dimensions.pixel_format == ( wide ? PYRA_PIXEL_RGB16 : PYRA_PIXEL_RGB8 ),
			"Native RGB storage format changed" );
		auto pixels = std::vector<uint8_t>( dimensions.capacity_bytes + 1 );
// Deliberately unaligned destination, supported by the byte-copy interface.
		auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), dimensions.pixel_format,
			pixels.data() + 1, pixels.size() - 1, dimensions.row_stride_bytes };
		require( pyra_get_image( wsi.value_, &image, &buffer, nullptr ) == PYRA_OK, "RGB render failed" );
		const int expected8[] = { 37, 117, 220 };
		const int expected16[] = { 511, 2048, 4095 };
		for (size_t index = 0; index < kFixtureEdge * kFixtureEdge * 3; ++index) {
			if (wide) {
				auto value = uint16_t{};
				std::memcpy( &value, pixels.data() + 1 + index * sizeof( value ), sizeof( value ) );
				require( value == expected16[index % 3], "RGB16 samples were rescaled or reordered" );
			} else {
				require( std::abs( int( pixels[index + 1] ) - expected8[index % 3] ) <= kTolerance,
					"RGB8 samples were reordered or changed" );
			}
		}
	}
}


void test_disabled_cache_and_incomplete ( const std::filesystem::path &path )
{
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	options.decoded_cache_bytes = 0;
	{
		auto context = Context( &options );
		auto wsi = Wsi( context.value_, path );
		render_fixture( wsi.value_ );
		require( stats( context.value_ ).decoded_cache_bytes == 0, "Disabled decoded cache retained memory" );
		auto coarse = Wsi( context.value_, path.parent_path() / "coarse.ptf" );
		auto info = pyra_plane_info{ sizeof( pyra_plane_info ) };
		require( pyra_get_plane_info( coarse.value_, 0, &info, nullptr ) == PYRA_OK && info.level_count > 1,
			"Disabled decoded cache prevented synthetic overview construction" );
		const auto overview = request( 0, 0, info.bounds.width_um, info.bounds.height_um, 4 );
		const auto dimensions = layout( coarse.value_, overview );
		auto pixels = std::vector<uint8_t>( dimensions.capacity_bytes );
		auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), dimensions.pixel_format,
			pixels.data(), pixels.size(), dimensions.row_stride_bytes };
		require( pyra_get_image( coarse.value_, &overview, &buffer, nullptr ) == PYRA_OK,
			"Synthetic overview rendering failed with decoded caching disabled" );
		require( std::all_of( pixels.begin(), pixels.end(), [] ( auto value ) {
			return std::abs( int( value ) - kFixtureValue ) <= kTolerance;
		} ), "Synthetic overview lost pixels with decoded caching disabled" );
		require( stats( context.value_ ).decoded_cache_bytes == 0,
			"Temporary decoded overview scratch remained allocated after open/render" );
	}
	options.compressed_cache_bytes = 1;
	auto context = Context( &options );
	auto wsi = Wsi( context.value_, path );
	const auto image = request();
	const auto dimensions = layout( wsi.value_, image );
	auto pixels = std::vector<uint8_t>( dimensions.capacity_bytes, kSentinel );
	auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), dimensions.pixel_format,
		pixels.data(), pixels.size(), dimensions.row_stride_bytes };
	const auto status = pyra_get_image( wsi.value_, &image, &buffer, nullptr );
	require( status == PYRA_INCOMPLETE, "Missing tiles were not reported as incomplete" );
	require( stats( context.value_ ).incomplete_renders == 1, "Incomplete result was not counted" );
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2)
		return 2;
	try {
		test_backends();
		const auto directory = std::filesystem::path( argv[1] );
		const auto source = directory / "standalone.ptf";
		test_queries_and_ownership( source );
		test_layout_and_destination( source );
		test_shared_cache_eviction( source );
		test_disabled_cache_and_incomplete( source );
		test_color_storage( directory );
		test_unsupported_compression( directory );
		const auto unicode = directory / std::filesystem::path( u8"unicode-\u03b1-\u65e5.ptf" );
		std::filesystem::copy_file( source, unicode, std::filesystem::copy_options::overwrite_existing );
		test_queries_and_ownership( unicode );
		std::cout << "C interface ownership, layout, pixels, caps, concurrency, and Unicode: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
