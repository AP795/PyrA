#include "tiff_inc.h"

#include <pyra/pyra.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr auto kTileEdge = uint32_t{ 16 };
constexpr auto kImageEdge = kTileEdge * 2;
constexpr auto kColourTolerance = 3;
constexpr auto kColours = std::array{
	std::array<uint8_t, 3>{ 235, 235, 235 },
	std::array<uint8_t, 3>{ 210, 40, 80 },
	std::array<uint8_t, 3>{ 20, 180, 60 },
	std::array<uint8_t, 3>{ 40, 70, 220 }
};

void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


std::string utf8 ( const std::filesystem::path &path )
{
	const auto value = path.u8string();
	return { value.begin(), value.end() };
}


auto open_tiff ( const std::filesystem::path &path, const char *mode )
{
	auto file = std::unique_ptr<TIFF, decltype( &TIFFClose )>( TIFFOpen( utf8( path ).c_str(), mode ), TIFFClose );
	require( file != nullptr, "Cannot open JPEG colour fixture" );
	return file;
}


void set_tags ( TIFF *file, uint16_t photometric, bool shared_tables )
{
	const auto set = [file] ( uint32_t tag, auto value ) {
		require( TIFFSetField( file, tag, value ) != 0, "Cannot set JPEG colour fixture tag" );
	};
	set( TIFFTAG_IMAGEWIDTH, kImageEdge );
	set( TIFFTAG_IMAGELENGTH, kImageEdge );
	set( TIFFTAG_TILEWIDTH, kTileEdge );
	set( TIFFTAG_TILELENGTH, kTileEdge );
	set( TIFFTAG_SAMPLESPERPIXEL, 3 );
	set( TIFFTAG_BITSPERSAMPLE, 8 );
	set( TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
	set( TIFFTAG_PHOTOMETRIC, photometric );
	set( TIFFTAG_COMPRESSION, COMPRESSION_JPEG );
	set( TIFFTAG_JPEGQUALITY, 100 );
	set( TIFFTAG_JPEGTABLESMODE, shared_tables ? JPEGTABLESMODE_QUANT | JPEGTABLESMODE_HUFF : 0 );
	set( TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER );
	set( TIFFTAG_XRESOLUTION, 10000.0 );
	set( TIFFTAG_YRESOLUTION, 10000.0 );
	set( TIFFTAG_IMAGEDESCRIPTION,
		"Aperio Image Library v12.0.0\n32x32 [0,0 32x32] (16x16) JPEG Q=100|MPP = 1.0" );
}


// Mimic the real SVS ambiguity: RGB is declared by TIFF, not by JPEG markers.
// Only header metadata changes; entropy-coded samples remain untouched.
std::vector<uint8_t> ambiguous_jpeg ( std::vector<uint8_t> bytes )
{
	require( bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8, "Missing fixture JPEG SOI" );
	auto output = std::vector<uint8_t>{ 0xff, 0xd8 };
	for (auto position = size_t{ 2 }; position < bytes.size();) {
		require( position + 1 < bytes.size() && bytes[position] == 0xff, "Invalid fixture JPEG marker" );
		const auto marker = bytes[position + 1];
		if (marker == 0xd9) {
			output.insert( output.end(), bytes.begin() + position, bytes.end() );
			return output;
		}
		require( position + 4 <= bytes.size(), "Truncated fixture JPEG header" );
		const auto length = ( size_t( bytes[position + 2] ) << 8 ) | bytes[position + 3];
		const auto end = position + 2 + length;
		require( length >= 2 && end <= bytes.size(), "Invalid fixture JPEG segment size" );
		if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
			require( length >= 17 && bytes[position + 9] == 3, "Expected a three-component JPEG frame" );
			for (auto component = size_t{}; component < 3; ++component)
				bytes[position + 10 + component * 3] = uint8_t( component );
		}
		if (marker == 0xda) {
			require( length >= 12 && bytes[position + 4] == 3, "Expected a three-component JPEG scan" );
			for (auto component = size_t{}; component < 3; ++component)
				bytes[position + 5 + component * 2] = uint8_t( component );
			output.insert( output.end(), bytes.begin() + position, bytes.end() );
			return output;
		}
		if (marker != 0xe0 && marker != 0xee)
			output.insert( output.end(), bytes.begin() + position, bytes.begin() + end );
		position = end;
	}
	throw std::runtime_error( "Fixture JPEG has no scan or EOI" );
}


void write_fixture ( const std::filesystem::path &path, uint16_t photometric, bool shared_tables )
{
	const auto staging = path.string() + ".encoded.tif";
	{
		auto file = open_tiff( staging, "w" );
		set_tags( file.get(), photometric, shared_tables );
		require( TIFFSetField( file.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB ) != 0,
			"Cannot configure RGB fixture input" );
		auto pixels = std::vector<uint8_t>( kTileEdge * kTileEdge * 3 );
		for (auto tile = uint32_t{}; tile < kColours.size(); ++tile) {
			for (auto offset = size_t{}; offset < pixels.size(); offset += 3)
				std::copy( kColours[tile].begin(), kColours[tile].end(), pixels.begin() + offset );
			require( TIFFWriteEncodedTile( file.get(), tile, pixels.data(), tmsize_t( pixels.size() ) ) > 0,
				"Cannot encode fixture tile" );
		}
	}
	auto source = open_tiff( staging, "r" );
	auto destination = open_tiff( path, "w" );
	set_tags( destination.get(), photometric, shared_tables );
	if (shared_tables) {
		uint32_t size = 0;
		void *data = nullptr;
		require( TIFFGetField( source.get(), TIFFTAG_JPEGTABLES, &size, &data ) != 0 && data,
			"Missing shared JPEG tables" );
		const auto *first = static_cast<const uint8_t *>( data );
		auto tables = ambiguous_jpeg( { first, first + size } );
		require( TIFFSetField( destination.get(), TIFFTAG_JPEGTABLES, uint32_t( tables.size() ), tables.data() ) != 0,
			"Cannot copy shared JPEG tables" );
	}
	for (auto tile = uint32_t{}; tile < kColours.size(); ++tile) {
		auto bytes = std::vector<uint8_t>( size_t( TIFFGetStrileByteCount( source.get(), tile ) ) );
		require( TIFFReadRawTile( source.get(), tile, bytes.data(), tmsize_t( bytes.size() ) ) == tmsize_t( bytes.size() ),
			"Cannot read encoded fixture tile" );
		auto patched = ambiguous_jpeg( std::move( bytes ) );
		require( TIFFWriteRawTile( destination.get(), tile, patched.data(), tmsize_t( patched.size() ) ) == tmsize_t( patched.size() ),
			"Cannot write ambiguous fixture tile" );
	}
}


std::vector<uint8_t> render_fixture ( const std::filesystem::path &path,
	pyra_backend backend, uint64_t decoded_cache, uint32_t threads, double mpp )
{
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	options.backend = backend;
	options.decoded_cache_bytes = decoded_cache;
	options.decode_threads = threads;
	auto error = pyra_error{};
	auto *context = static_cast<pyra_context *>( nullptr );
	const auto context_status = pyra_context_create( &options, &context, &error );
	require( context_status == PYRA_OK, error.message );
	const auto close_context = [] ( pyra_context *value ) { pyra_context_destroy( value, nullptr ); };
	auto owner = std::unique_ptr<pyra_context, decltype( close_context )>( context, close_context );
	auto *slide = static_cast<pyra_wsi *>( nullptr );
	const auto open_status = pyra_open( context, utf8( path ).c_str(), &slide, &error );
	require( open_status == PYRA_OK, path.filename().string() + ": " + error.message );
	const auto close_slide = [] ( pyra_wsi *value ) { pyra_close( value, nullptr ); };
	auto handle = std::unique_ptr<pyra_wsi, decltype( close_slide )>( slide, close_slide );
	const auto request = pyra_image_request{ sizeof( pyra_image_request ), 0, { 0, 0, kImageEdge, kImageEdge }, mpp };
	auto layout = pyra_image_layout{ sizeof( pyra_image_layout ) };
	require( pyra_get_image_layout( slide, &request, &layout, &error ) == PYRA_OK && layout.pixel_format == PYRA_PIXEL_RGB8,
		"Invalid fixture raster layout" );
	auto pixels = std::vector<uint8_t>( layout.capacity_bytes );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		pixels.data(), pixels.size(), layout.row_stride_bytes };
	for (auto repeat = 0; repeat < 2; ++repeat) {
		const auto status = pyra_get_image( slide, &request, &buffer, &error );
		require( status == PYRA_OK, path.filename().string() + ": " + error.message );
		for (auto tile = uint32_t{}; tile < kColours.size(); ++tile) {
			const auto x = uint32_t( ( ( tile % 2 ) * kTileEdge + kTileEdge / 2 ) / mpp );
			const auto y = uint32_t( ( ( tile / 2 ) * kTileEdge + kTileEdge / 2 ) / mpp );
			for (auto channel = size_t{}; channel < 3; ++channel) {
				const auto actual = pixels[size_t( y ) * layout.row_stride_bytes + x * 3 + channel];
				require( std::abs( int( actual ) - int( kColours[tile][channel] ) ) <= kColourTolerance,
					"Incorrect JPEG colour: " + path.filename().string() + " backend=" + std::to_string( backend ) +
					" tile=" + std::to_string( tile ) + " channel=" + std::to_string( channel ) +
					" actual=" + std::to_string( actual ) + " expected=" + std::to_string( kColours[tile][channel] ) );
			}
		}
	}
	return pixels;
}


void test_fixture ( const std::filesystem::path &path, bool openslide_available )
{
	for (const auto decoded : { uint64_t{ 0 }, uint64_t{ 1024 } * 1024 }) {
		for (const auto threads : { 1u, 4u }) {
			for (const auto mpp : { 1.0, 2.0 }) {
				const auto actual = render_fixture( path, PYRA_BACKEND_NATIVE, decoded, threads, mpp );
				if (openslide_available) {
					const auto reference = render_fixture( path, PYRA_BACKEND_OPENSLIDE, decoded, threads, mpp );
					require( actual.size() == reference.size() &&
						std::equal( actual.begin(), actual.end(), reference.begin(), [] ( auto a, auto b ) {
							return std::abs( int( a ) - int( b ) ) <= kColourTolerance;
						} ), "Native/OpenSlide JPEG colour pixels differ: " + path.filename().string() );
				}
			}
		}
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
		auto backend = pyra_backend_info{ sizeof( pyra_backend_info ), 0, nullptr, nullptr };
		require( pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &backend, nullptr ) == PYRA_OK, "Backend query failed" );
		for (const auto photometric : { uint16_t{ PHOTOMETRIC_RGB }, uint16_t{ PHOTOMETRIC_YCBCR } }) {
			for (const auto shared : { false, true }) {
				const auto name = std::string( photometric == PHOTOMETRIC_RGB ? "rgb" : "ycbcr" ) + ( shared ? "-shared" : "-full" );
				const auto path = directory / ( name + ".svs" );
				write_fixture( path, photometric, shared );
				test_fixture( path, backend.available != 0 );
// The generic TIFF loader currently expects complete tile streams. Shared
// JPEG-table reconstruction belongs to the SVS loader and is checked above.
				if (!shared) {
					const auto tiff_path = directory / ( name + ".ptf" );
					std::filesystem::copy_file( path, tiff_path, std::filesystem::copy_options::overwrite_existing );
					test_fixture( tiff_path, backend.available != 0 );
				}
				std::cout << name << ": known colours, cached/uncached and thread counts PASS\n";
			}
		}
		if (!backend.available)
			std::cout << "OpenSlide differential checks SKIPPED: optional backend disabled\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
