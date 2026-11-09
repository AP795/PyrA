#include "tiff_inc.h"

#include <pyra/pyra.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kTileEdge = uint32_t{ 16 };
constexpr auto kSentinel = uint8_t{ 0xcd };

void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


std::string utf8 ( const std::filesystem::path &path )
{
	const auto value = path.u8string();
	return { value.begin(), value.end() };
}


void write_fixture ( const std::filesystem::path &path, uint32_t width,
	uint32_t height, int levels, bool metric = true, bool alpha = false )
{
	auto tiff = std::unique_ptr<TIFF, decltype( &TIFFClose )>( TIFFOpen( utf8( path ).c_str(), "w" ), TIFFClose );
	require( tiff != nullptr, "Fixture TIFF open failed" );
	for (auto level = 0; level < levels; ++level) {
		auto *file = tiff.get();
		const auto set = [file] ( uint32_t tag, auto value ) {
			require( TIFFSetField( file, tag, value ) != 0, "Fixture TIFF tag failed" );
		};
		set( TIFFTAG_IMAGEWIDTH, width );
		set( TIFFTAG_IMAGELENGTH, height );
		set( TIFFTAG_TILEWIDTH, kTileEdge );
		set( TIFFTAG_TILELENGTH, kTileEdge );
		set( TIFFTAG_SAMPLESPERPIXEL, alpha ? 4 : 3 );
		set( TIFFTAG_BITSPERSAMPLE, 8 );
		set( TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB );
		set( TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
		set( TIFFTAG_COMPRESSION, metric && !alpha ? COMPRESSION_JPEG : COMPRESSION_LZW );
		if (metric && !alpha) {
			set( TIFFTAG_JPEGQUALITY, 100 );
			set( TIFFTAG_JPEGTABLESMODE, 0 );
		}
		set( TIFFTAG_SUBFILETYPE, level > 0 ? FILETYPE_REDUCEDIMAGE : 0 );
		if (metric) {
			set( TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER );
			set( TIFFTAG_XRESOLUTION, 20000.0 / ( 1 << level ) );
			set( TIFFTAG_YRESOLUTION, 10000.0 / ( 0.75 * ( 1 << level ) ) );
		}
		if (alpha) {
			auto extra = uint16_t{ EXTRASAMPLE_UNASSALPHA };
			require( TIFFSetField( file, TIFFTAG_EXTRASAMPLES, 1, &extra ) != 0, "Alpha tag failed" );
		}
		auto tile = std::vector<uint8_t>( kTileEdge * kTileEdge * ( alpha ? 4 : 3 ) );
		for (uint32_t y = 0; y < height; y += kTileEdge) {
			for (uint32_t x = 0; x < width; x += kTileEdge) {
				for (uint32_t row = 0; row < kTileEdge; ++row) {
					for (uint32_t column = 0; column < kTileEdge; ++column) {
						auto *pixel = tile.data() + ( row * kTileEdge + column ) * ( alpha ? 4 : 3 );
						pixel[0] = uint8_t( ( ( x + column ) * 3 + y + row ) % 251 );
						pixel[1] = uint8_t( ( x + column + ( y + row ) * 5 ) % 251 );
						pixel[2] = level > 0 ? 173 : 91;
						if (alpha)
							pixel[3] = 0;
					}
				}
				require( TIFFWriteEncodedTile( file, TIFFComputeTile( file, x, y, 0, 0 ),
					tile.data(), tmsize_t( tile.size() ) ) > 0, "Fixture tile write failed" );
			}
		}
		require( TIFFWriteDirectory( file ) != 0, "Fixture directory failed" );
		width /= 2;
		height /= 2;
	}
}


struct Context {
	pyra_context *value_ = nullptr;

	explicit Context ( pyra_backend backend, uint64_t cache = 1024 * 1024 ) {
		auto options = pyra_context_options{};
		pyra_context_options_init( &options );
		options.backend = backend;
		options.decoded_cache_bytes = cache;
		auto error = pyra_error{};
		require( pyra_context_create( &options, &value_, &error ) == PYRA_OK, error.message );
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


std::vector<uint8_t> render ( pyra_wsi *wsi, const pyra_image_request &request )
{
	auto layout = pyra_image_layout{ sizeof( pyra_image_layout ) };
	auto error = pyra_error{};
	require( pyra_get_image_layout( wsi, &request, &layout, &error ) == PYRA_OK, error.message );
	auto pixels = std::vector<uint8_t>( layout.capacity_bytes );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		pixels.data(), pixels.size(), layout.row_stride_bytes };
	require( pyra_get_image( wsi, &request, &buffer, &error ) == PYRA_OK, error.message );
	return pixels;
}


void test_geometry_pixels_and_sharing ( const std::filesystem::path &path )
{
	auto context = Context( PYRA_BACKEND_OPENSLIDE );
	auto first = Wsi( context.value_, path );
	auto second = Wsi( context.value_, path.parent_path() / "." / path.filename() );
	auto stats = pyra_context_stats{ sizeof( pyra_context_stats ) };
	require( pyra_context_get_stats( context.value_, &stats, nullptr ) == PYRA_OK &&
		stats.shared_wsis == 1 && stats.wsi_handles == 2 && stats.compressed_cache_bytes == 0 &&
		stats.decoded_cache_bytes == PYRA_STAT_UNAVAILABLE && stats.cache_pool_evictions == PYRA_STAT_UNAVAILABLE,
		"OpenSlide sharing/statistics contract differs" );
	require( pyra_context_destroy( context.value_, nullptr ) == PYRA_BUSY, "Live slide did not keep context busy" );
	auto info = pyra_plane_info{ sizeof( pyra_plane_info ) };
	require( pyra_get_plane_info( first.value_, 0, &info, nullptr ) == PYRA_OK &&
		info.level_count == 2 && info.pixel_format == PYRA_PIXEL_RGB8 &&
		std::abs( info.native_mpp - 0.5 ) < 1e-6 && std::abs( info.source_mpp_y - 0.75 ) < 1e-6 &&
		std::abs( info.bounds.width_um - 32 ) < 1e-5 && std::abs( info.bounds.height_um - 24 ) < 1e-5,
		"OpenSlide plane scalar geometry differs" );
	auto native = Context( PYRA_BACKEND_NATIVE );
	auto reference = Wsi( native.value_, path );
	for (const auto mpp : { 0.25, 0.5, 0.75, 1.0 }) {
		const auto request = pyra_image_request{ sizeof( pyra_image_request ), 0,
			{ -0.5, -1, 20.5, 17.5 }, mpp };
		const auto actual = render( first.value_, request );
		const auto expected = render( reference.value_, request );
		require( actual.size() == expected.size() && std::equal( actual.begin(), actual.end(), expected.begin(),
			[] ( auto a, auto b ) { return std::abs( int( a ) - int( b ) ) <= 2; } ),
			"Native/OpenSlide JPEG samples differ at fractional/clipped geometry" );
	}
	const auto request = pyra_image_request{ sizeof( pyra_image_request ), 0, { -1, -1, 4, 4 }, 0.5 };
	auto layout = pyra_image_layout{ sizeof( pyra_image_layout ) };
	require( pyra_get_image_layout( first.value_, &request, &layout, nullptr ) == PYRA_OK, "Layout failed" );
	const auto stride = layout.row_stride_bytes + 7;
	auto bytes = std::vector<uint8_t>( stride * layout.height, kSentinel );
	auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		bytes.data(), bytes.size(), stride };
	require( pyra_get_image( first.value_, &request, &buffer, nullptr ) == PYRA_OK, "Padded render failed" );
	for (uint32_t row = 0; row < layout.height; ++row)
		for (auto x = layout.row_stride_bytes; x < stride; ++x)
			require( bytes[row * stride + x] == kSentinel, "Row padding changed" );
	std::fill( bytes.begin(), bytes.end(), kSentinel );
	buffer.capacity_bytes = 1;
	require( pyra_get_image( first.value_, &request, &buffer, nullptr ) == PYRA_BUFFER_TOO_SMALL &&
		std::all_of( bytes.begin(), bytes.end(), [] ( auto value ) { return value == kSentinel; } ),
		"Failed render changed destination" );
	auto invalid = request;
	invalid.plane_index = 1;
	require( pyra_get_image_layout( first.value_, &invalid, &layout, nullptr ) == PYRA_INVALID_ARGUMENT,
		"Invalid plane accepted" );
	invalid = request;
	invalid.mpp = std::numeric_limits<double>::quiet_NaN();
	require( pyra_get_image_layout( first.value_, &invalid, &layout, nullptr ) == PYRA_INVALID_ARGUMENT,
		"Invalid resolution accepted" );
	require( pyra_close( first.value_, nullptr ) == PYRA_OK, "Close failed" );
	first.value_ = nullptr;
	(void)render( second.value_, request );
}


void test_sparse_sampling_and_alpha ( const std::filesystem::path &directory )
{
	auto context = Context( PYRA_BACKEND_OPENSLIDE, 0 );
	auto wsi = Wsi( context.value_, directory / "singlelevel.ptf" );
	const auto request = pyra_image_request{ sizeof( pyra_image_request ), 0, { 0, 0, 4096, 16 }, 1 };
	const auto pixels = render( wsi.value_, request );
	for (auto x = size_t{ 0 }; x < 4096; ++x)
		require( pixels[x * 3] == uint8_t( x * 3 % 251 ) && pixels[x * 3 + 2] == 91,
			"Bounded staging changed a sample across a block boundary" );
	auto coarse = request;
	coarse.mpp = 1000;
	require( render( wsi.value_, coarse ).size() == 15, "Sparse single-level overview failed" );
	auto stats = pyra_context_stats{ sizeof( pyra_context_stats ) };
	require( pyra_context_get_stats( context.value_, &stats, nullptr ) == PYRA_OK && stats.decoded_cache_bytes == 0,
		"Zero decoded budget retained a cache" );
	auto alpha = Wsi( context.value_, directory / "alpha.ptf" );
	const auto transparent = render( alpha.value_, { sizeof( pyra_image_request ), 0, { 0, 0, 16, 16 }, 1 } );
	require( std::all_of( transparent.begin(), transparent.end(), [] ( auto value ) { return value == 0; } ),
		"Transparent ARGB did not composite on zero" );
	const auto outside = render( wsi.value_, { sizeof( pyra_image_request ), 0, { -1e6, -1e6, 16, 16 }, 1 } );
	require( std::all_of( outside.begin(), outside.end(), [] ( auto value ) { return value == 0; } ),
		"Out-of-bounds request was not zero" );
}


void test_terminal_decode_failure ( const std::filesystem::path &directory )
{
	const auto path = directory / "corrupt.ptf";
	std::filesystem::copy_file( directory / "compare.ptf", path, std::filesystem::copy_options::overwrite_existing );
	auto offset = uint64_t{};
	{
		auto tiff = std::unique_ptr<TIFF, decltype( &TIFFClose )>( TIFFOpen( utf8( path ).c_str(), "r" ), TIFFClose );
		require( tiff != nullptr, "Corrupt fixture read failed" );
		uint64_t *offsets = nullptr;
		require( TIFFGetField( tiff.get(), TIFFTAG_TILEOFFSETS, &offsets ) != 0 && offsets,
			"Corrupt fixture tile offset missing" );
		offset = offsets[0];
	}
	{
		auto file = std::fstream( path, std::ios::binary | std::ios::in | std::ios::out );
		file.seekp( std::streamoff( offset ) );
		const char damage[8] = {};
		file.write( damage, sizeof( damage ) );
		require( bool( file ), "Corrupt fixture write failed" );
	}
	auto context = Context( PYRA_BACKEND_OPENSLIDE );
	auto wsi = Wsi( context.value_, path );
	auto request = pyra_image_request{ sizeof( pyra_image_request ), 0, { 0, 0, 8, 8 }, 0.5 };
	auto layout = pyra_image_layout{ sizeof( pyra_image_layout ) };
	require( pyra_get_image_layout( wsi.value_, &request, &layout, nullptr ) == PYRA_OK, "Corrupt fixture layout failed" );
	auto pixels = std::vector<uint8_t>( layout.capacity_bytes, kSentinel );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		pixels.data(), pixels.size(), layout.row_stride_bytes };
	for (auto attempt = 0; attempt < 2; ++attempt) {
		auto error = pyra_error{};
		require( pyra_get_image( wsi.value_, &request, &buffer, &error ) == PYRA_IO_ERROR &&
			std::strstr( error.message, "OpenSlide" ) &&
			std::all_of( pixels.begin(), pixels.end(), [] ( auto value ) { return value == kSentinel; } ),
			"OpenSlide terminal decode error changed the caller buffer or lost its diagnostic" );
	}
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2)
		return 2;
	try {
		const auto directory = std::filesystem::path( argv[1] );
		std::filesystem::create_directories( directory );
		write_fixture( directory / "compare.ptf", 64, 48, 2 );
		write_fixture( directory / "singlelevel.ptf", 4096, 16, 1, false );
		write_fixture( directory / "alpha.ptf", 16, 16, 1, false, true );
		test_geometry_pixels_and_sharing( directory / "compare.ptf" );
		const auto unicode = directory / std::filesystem::path( u8"compare-\u03b1-\u65e5.ptf" );
		std::filesystem::copy_file( directory / "compare.ptf", unicode, std::filesystem::copy_options::overwrite_existing );
		test_geometry_pixels_and_sharing( unicode );
		test_sparse_sampling_and_alpha( directory );
		test_terminal_decode_failure( directory );
		std::cout << "OpenSlide C API geometry, pixels, sharing, cache, staging and Unicode: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
