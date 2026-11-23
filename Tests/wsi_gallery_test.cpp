// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Ordinary TIFF images reach the reader as galleries: each fixture image sits in
// its own directory, scan_gallery_directory lays the directory out, and the C
// interface opens the ".glr" manifest. The directory is also the GUI and web
// viewer tests' fixture (images/<name>/<name>.glr).

#include "gallery_scan.h"
#include "tiff_inc.h"

#include <pyra/pyra.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kImageWidth = 37;
constexpr auto kImageHeight = 29;
constexpr auto kTileEdge = 16;
constexpr auto kStripRows = 5;
constexpr auto kLargeImageEdge = 1100;
// Microns per pixel given to a scan when no member states one.
constexpr auto kAssumedMpp = 1.0;


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


struct ImageSpec {
	int width = kImageWidth;
	int height = kImageHeight;
	int bits = 8;
	int samples = 1;
	uint16_t compression = COMPRESSION_LZW;
	uint16_t photometric = PHOTOMETRIC_MINISBLACK;
	uint16_t sample_format = SAMPLEFORMAT_UINT;
	uint16_t planar = PLANARCONFIG_CONTIG;
	bool tiled = false;
	bool reduced = false;
	double mpp_x = 0.0;
	double mpp_y = 0.0;
	int constant = -1;
};


uint16_t sample ( const ImageSpec &spec, int x, int y, int channel )
{
	if (spec.constant >= 0)
		return uint16_t( spec.constant );
	return uint16_t( ( x * 137 + y * 511 + channel * 10003 ) %
		( spec.bits == 8 ? 256 : 65536 ) );
}


void write_page ( TIFF *file, const ImageSpec &spec )
{
	const auto set = [file] ( uint32_t tag, auto value ) {
		require( TIFFSetField( file, tag, value ) != 0, "Cannot set image fixture tag" );
	};
	set( TIFFTAG_IMAGEWIDTH, spec.width );
	set( TIFFTAG_IMAGELENGTH, spec.height );
	set( TIFFTAG_BITSPERSAMPLE, spec.bits );
	set( TIFFTAG_SAMPLESPERPIXEL, spec.samples );
	set( TIFFTAG_SAMPLEFORMAT, spec.sample_format );
	set( TIFFTAG_PLANARCONFIG, spec.planar );
	set( TIFFTAG_PHOTOMETRIC, spec.photometric );
	set( TIFFTAG_COMPRESSION, spec.compression );
	set( TIFFTAG_IMAGEDESCRIPTION, "PyrA gallery image fixture" );
	if (spec.reduced)
		set( TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE );
	if (spec.mpp_x > 0.0 && spec.mpp_y > 0.0) {
		set( TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER );
		set( TIFFTAG_XRESOLUTION, 10000.0 / spec.mpp_x );
		set( TIFFTAG_YRESOLUTION, 10000.0 / spec.mpp_y );
	}
	if (spec.compression == COMPRESSION_LZW || spec.compression == COMPRESSION_ADOBE_DEFLATE)
		set( TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL );
	const auto fill = [&spec] ( int x, int y, int w, int h ) {
		auto bytes = std::vector<uint8_t>( size_t( w ) * h * spec.samples * ( spec.bits / 8 ) );
		for (auto row = 0; row < h; ++row) {
			for (auto column = 0; column < w; ++column) {
				for (auto c = 0; c < spec.samples; ++c) {
					const auto value = sample( spec, x + column, y + row, c );
					const auto offset = ( size_t( row * w + column ) * spec.samples + c ) * ( spec.bits / 8 );
					if (spec.bits == 16)
						std::memcpy( bytes.data() + offset, &value, sizeof( value ) );
					else
						bytes[offset] = uint8_t( value );
				}
			}
		}
		return bytes;
	};
	if (spec.tiled) {
		set( TIFFTAG_TILEWIDTH, kTileEdge );
		set( TIFFTAG_TILELENGTH, kTileEdge );
		for (auto y = 0; y < spec.height; y += kTileEdge) {
			for (auto x = 0; x < spec.width; x += kTileEdge) {
				auto bytes = fill( x, y, kTileEdge, kTileEdge );
				require( TIFFWriteEncodedTile( file, TIFFComputeTile( file, x, y, 0, 0 ),
					bytes.data(), tmsize_t( bytes.size() ) ) > 0, "Cannot write image fixture tile" );
			}
		}
	} else {
		set( TIFFTAG_ROWSPERSTRIP, kStripRows );
		for (auto y = 0; y < spec.height; y += kStripRows) {
			auto bytes = fill( 0, y, spec.width, std::min( kStripRows, spec.height - y ) );
			require( TIFFWriteEncodedStrip( file, y / kStripRows, bytes.data(),
				tmsize_t( bytes.size() ) ) > 0, "Cannot write image fixture strip" );
		}
	}
	require( TIFFWriteDirectory( file ) != 0, "Cannot finish image fixture page" );
}


void write_image ( const std::filesystem::path &path, const ImageSpec &spec,
	const ImageSpec *second = nullptr )
{
	std::filesystem::create_directories( path.parent_path() );
#ifdef _WIN32
	auto file = std::unique_ptr<TIFF, decltype( &TIFFClose )>( TIFFOpenW( path.c_str(), "w" ), TIFFClose );
#else
	auto file = std::unique_ptr<TIFF, decltype( &TIFFClose )>( TIFFOpen( path.c_str(), "w" ), TIFFClose );
#endif
	require( file != nullptr, "Cannot create image fixture" );
	write_page( file.get(), spec );
	if (second)
		write_page( file.get(), *second );
}


// Lay `directory` out as a gallery and write its manifest; the manifest's path.
std::filesystem::path make_gallery ( const std::filesystem::path &directory,
	double mpp = kAssumedMpp )
{
	auto options = priml::GalleryScanOptions{};
	options.mpp = mpp;
	auto manifest = priml::GalleryManifest{};
	auto report = priml::GalleryScanReport{};
	auto error = std::string{};
	require( priml::scan_gallery_directory( directory, options, &manifest, &report, &error ),
		"Gallery scan failed: " + error );
	require( priml::write_gallery_manifest( report.manifest_path, manifest, &error ),
		"Cannot write gallery manifest: " + error );
	return report.manifest_path;
}


// `name`.tif written in its own directory, made into a gallery.
std::filesystem::path image_gallery ( const std::filesystem::path &root, const char *name,
	const ImageSpec &spec, const ImageSpec *second = nullptr, double mpp = kAssumedMpp )
{
	const auto directory = root / name;
	std::filesystem::remove_all( directory );
	write_image( directory / ( std::string( name ) + ".tif" ), spec, second );
	return make_gallery( directory, mpp );
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


struct Image {
	pyra_wsi *value_ = nullptr;

	Image ( pyra_context *context, const std::filesystem::path &path ) {
		auto error = pyra_error{};
		require( pyra_open( context, utf8( path ).c_str(), &value_, &error ) == PYRA_OK, error.message );
	}
	~Image () { pyra_close( value_, nullptr ); }
	Image ( const Image & ) = delete;
	Image &operator= ( const Image & ) = delete;
};


pyra_plane_info plane_info ( pyra_wsi *image )
{
	auto count = uint32_t{};
	require( pyra_get_plane_count( image, &count, nullptr ) == PYRA_OK && count == 1,
		"Gallery did not expose exactly one plane" );
	auto info = pyra_plane_info{ sizeof( pyra_plane_info ) };
	require( pyra_get_plane_info( image, 0, &info, nullptr ) == PYRA_OK, "No gallery plane info" );
	return info;
}


std::string property ( pyra_wsi *image, const char *key )
{
	auto count = uint32_t{};
	require( pyra_get_property_count( image, &count, nullptr ) == PYRA_OK, "No property count" );
	for (auto i = uint32_t{}; i < count; ++i) {
		auto entry = pyra_property{};
		require( pyra_get_property( image, i, &entry, nullptr ) == PYRA_OK, "No property entry" );
		if (std::strcmp( entry.key, key ) == 0)
			return entry.value;
	}
	throw std::runtime_error( std::string( "Missing property: " ) + key );
}


std::vector<uint8_t> render ( pyra_wsi *image, const pyra_image_request &request )
{
	auto layout = pyra_image_layout{ sizeof( pyra_image_layout ) };
	auto error = pyra_error{};
	require( pyra_get_image_layout( image, &request, &layout, &error ) == PYRA_OK, error.message );
	auto bytes = std::vector<uint8_t>( layout.capacity_bytes );
	auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		bytes.data(), bytes.size(), layout.row_stride_bytes };
	require( pyra_get_image( image, &request, &buffer, &error ) == PYRA_OK, error.message );
	return bytes;
}


// The member's own rectangle at native resolution: the gallery puts the only
// member at the origin.
pyra_rect member_rect ( const ImageSpec &spec, double mpp )
{
	return { 0.0, 0.0, spec.width * mpp, spec.height * mpp };
}


void test_gallery_native_samples ( const std::filesystem::path &root, const char *name,
	const ImageSpec &spec, bool direct = false )
{
	const auto manifest = image_gallery( root, name, spec );
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	if (direct)
		options.decoded_cache_bytes = 0;
	auto context = Context( &options );
	auto image = Image( context.value_, manifest );
	const auto info = plane_info( image.value_ );
	const auto expected_format = spec.samples == 3 ? ( spec.bits == 8 ? PYRA_PIXEL_RGB8 : PYRA_PIXEL_RGB16 )
		: ( spec.bits == 8 ? PYRA_PIXEL_GRAY8 : PYRA_PIXEL_GRAY16 );
	require( info.pixel_format == expected_format && info.native_mpp == kAssumedMpp,
		"Gallery member sample type or pixel size changed" );
	require( property( image.value_, "gallery.mpp-source" ) == "user",
		"The caller's pixel size was not recorded as such" );
	const auto request = pyra_image_request{ sizeof( pyra_image_request ), 0,
		member_rect( spec, kAssumedMpp ), info.native_mpp };
	const auto bytes = render( image.value_, request );
	for (auto y = 0; y < spec.height; ++y) {
		for (auto x = 0; x < spec.width; ++x) {
			for (auto c = 0; c < spec.samples; ++c) {
				const auto offset = ( size_t( y * spec.width + x ) * spec.samples + c ) * ( spec.bits / 8 );
				auto actual = uint16_t{ bytes[offset] };
				if (spec.bits == 16)
					std::memcpy( &actual, bytes.data() + offset, sizeof( actual ) );
				auto expected = sample( spec, x, y, c );
				if (spec.photometric == PHOTOMETRIC_MINISWHITE)
					expected = uint16_t( ( spec.bits == 8 ? 255 : 65535 ) - expected );
				require( actual == expected, "Gallery member samples changed at a strip/tile boundary" );
			}
		}
	}
	if (direct) {
		auto stats = pyra_context_stats{ sizeof( pyra_context_stats ) };
		require( pyra_context_get_stats( context.value_, &stats, nullptr ) == PYRA_OK &&
			stats.decoded_cache_bytes == 0, "Direct gallery rendering retained decoded pixels" );
	}
}


void test_gallery_tiff_calibration_and_crop ( const std::filesystem::path &root )
{
	auto spec = ImageSpec{};
	spec.bits = 16;
	spec.mpp_x = spec.mpp_y = 0.5;
	const auto manifest = image_gallery( root, "metric", spec, nullptr, 0.0 );
	auto context = Context{};
	auto image = Image( context.value_, manifest );
	const auto info = plane_info( image.value_ );
	require( std::abs( info.native_mpp - spec.mpp_x ) < 1e-9, "TIFF calibration was lost" );
	require( property( image.value_, "gallery.mpp-source" ) == "tiff",
		"TIFF calibration source was not identified" );
	const auto bytes = render( image.value_, { sizeof( pyra_image_request ), 0,
		{ 3 * spec.mpp_x, 4 * spec.mpp_x, 7 * spec.mpp_x, 6 * spec.mpp_x }, spec.mpp_x } );
	for (auto y = 0; y < 6; ++y) {
		for (auto x = 0; x < 7; ++x) {
			auto value = uint16_t{};
			std::memcpy( &value, bytes.data() + size_t( y * 7 + x ) * sizeof( value ), sizeof( value ) );
			require( value == sample( spec, x + 3, y + 4, 0 ), "Metric crop changed member samples" );
		}
	}
}


void test_gallery_reduced_page_keeps_native ( const std::filesystem::path &root )
{
	auto base = ImageSpec{};
	base.width = base.height = kTileEdge * 2;
	base.constant = 37;
	auto reduced = base;
	reduced.width = reduced.height = kTileEdge;
	reduced.reduced = true;
	reduced.constant = 211;
	const auto manifest = image_gallery( root, "reduced", base, &reduced );
	auto context = Context{};
	auto image = Image( context.value_, manifest );
	const auto info = plane_info( image.value_ );
	const auto native = render( image.value_, { sizeof( pyra_image_request ), 0,
		member_rect( base, kAssumedMpp ), 1 } );
	require( std::all_of( native.begin(), native.end(), [&] ( auto b ) { return b == base.constant; } ),
		"The reduced page replaced native member data" );
	require( info.level_count >= 1, "Gallery exposed no level" );
}


void test_gallery_synthetic_pyramid ( const std::filesystem::path &root )
{
	auto spec = ImageSpec{};
	spec.width = spec.height = kLargeImageEdge;
	spec.samples = 3;
	spec.photometric = PHOTOMETRIC_RGB;
	spec.constant = 117;
	const auto manifest = image_gallery( root, "synthetic", spec );
	auto context = Context{};
	auto image = Image( context.value_, manifest );
	const auto info = plane_info( image.value_ );
	require( info.level_count > 1, "Large gallery member did not gain coarse levels" );
	const auto bytes = render( image.value_, { sizeof( pyra_image_request ), 0,
		member_rect( spec, kAssumedMpp ), 4 } );
	require( !bytes.empty() && std::all_of( bytes.begin(), bytes.end(),
		[&] ( auto b ) { return b == spec.constant; } ), "Coarse gallery level changed constant samples" );
}


void test_gallery_scan_refusal ( const std::filesystem::path &root, const char *name,
	const ImageSpec &spec, const char *reason, bool stack = false )
{
	const auto directory = root / name;
	std::filesystem::remove_all( directory );
	write_image( directory / ( std::string( name ) + ".tif" ), spec, stack ? &spec : nullptr );
	auto options = priml::GalleryScanOptions{};
	options.mpp = kAssumedMpp;
	auto manifest = priml::GalleryManifest{};
	auto report = priml::GalleryScanReport{};
	auto error = std::string{};
	require( !priml::scan_gallery_directory( directory, options, &manifest, &report, &error ),
		"An unusable image became a gallery" );
	auto reasons = std::string{};
	for (const auto &[file, why] : report.refused)
		reasons += why + ";";
	require( reasons.find( reason ) != std::string::npos,
		std::string( "Missing refusal reason '" ) + reason + "': " + reasons + " / " + error );
}


void test_gallery_several_members ( const std::filesystem::path &root )
{
	const auto directory = root / "cells";
	std::filesystem::remove_all( directory );
	auto spec = ImageSpec{};
	for (const auto *name : { "cell10.tif", "cell2.tif", "cell1.tif" })
		write_image( directory / name, spec );
	auto options = priml::GalleryScanOptions{};
	options.mpp = kAssumedMpp;
	auto manifest = priml::GalleryManifest{};
	auto report = priml::GalleryScanReport{};
	auto error = std::string{};
	require( priml::scan_gallery_directory( directory, options, &manifest, &report, &error ) &&
		manifest.members.size() == 3 && manifest.members[0].zone == "cell1" &&
		manifest.members[1].zone == "cell2" && manifest.members[2].zone == "cell10",
		"Members are not in natural order: " + error );
	for (size_t i = 0; i < manifest.members.size(); ++i)
		for (size_t j = i + 1; j < manifest.members.size(); ++j)
			require( !priml::overlaps( manifest.members[i].rect, manifest.members[j].rect ),
				"Gallery members overlap on the grid" );
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2)
		return 2;
	const auto directory = std::filesystem::path( argv[1] );
	std::filesystem::create_directories( directory );
	auto failed = 0;
	const auto run = [&failed] ( const char *name, const std::function<void()> &test ) {
		const auto start = std::chrono::steady_clock::now();
		auto passed = true;
		try { test(); }
		catch (const std::exception &error) {
			std::cerr << name << ": " << error.what() << '\n';
			++failed;
			passed = false;
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start ).count();
		std::cout << name << ": " << ( passed ? "PASS" : "FAIL" ) << " (" << elapsed << " us)\n";
	};
	run( "test_gallery_gray8_lzw_strips", [&] {
		test_gallery_native_samples( directory, "gray8", ImageSpec{} );
	} );
	run( "test_gallery_gray16_deflate", [&] {
		auto spec = ImageSpec{};
		spec.bits = 16;
		spec.compression = COMPRESSION_ADOBE_DEFLATE;
		test_gallery_native_samples( directory, "gui_raw", spec );
	} );
	run( "test_gallery_rgb8_lzw_tiles", [&] {
		auto spec = ImageSpec{};
		spec.samples = 3;
		spec.photometric = PHOTOMETRIC_RGB;
		spec.tiled = true;
		test_gallery_native_samples( directory, "rgb8", spec );
	} );
	run( "test_gallery_rgb16_uncompressed_direct", [&] {
		auto spec = ImageSpec{};
		spec.samples = 3;
		spec.bits = 16;
		spec.photometric = PHOTOMETRIC_RGB;
		spec.compression = COMPRESSION_NONE;
		test_gallery_native_samples( directory, "rgb16", spec, true );
	} );
	run( "test_gallery_gray16_miniswhite_tiles", [&] {
		auto spec = ImageSpec{};
		spec.bits = 16;
		spec.photometric = PHOTOMETRIC_MINISWHITE;
		spec.tiled = true;
		test_gallery_native_samples( directory, "white16", spec );
	} );
	run( "test_gallery_tiff_calibration_and_crop", [&] { test_gallery_tiff_calibration_and_crop( directory ); } );
	run( "test_gallery_reduced_page_keeps_native", [&] { test_gallery_reduced_page_keeps_native( directory ); } );
	run( "test_gallery_synthetic_pyramid", [&] { test_gallery_synthetic_pyramid( directory ); } );
	run( "test_gallery_several_members", [&] { test_gallery_several_members( directory ); } );
	run( "test_gallery_multipage_refused", [&] {
		test_gallery_scan_refusal( directory, "stack", ImageSpec{}, "multi-page", true );
	} );
	run( "test_gallery_signed_samples_refused", [&] {
		auto spec = ImageSpec{};
		spec.sample_format = SAMPLEFORMAT_INT;
		test_gallery_scan_refusal( directory, "signed", spec, "unsigned" );
	} );
	return failed == 0 ? 0 : 1;
}
