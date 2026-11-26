// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// pyra_synthetic_test <output directory>
//
// The synthetic z-stack writer through the public C interface: option
// validation, the plane and level ladder it writes, the focus profile across the
// stack and where it peaks, every compression this build can encode,
// cancellation, the untiled TIFF (tile 0, LZW or JPEG) read back through a
// gallery, and the nucleus census against the requested size, spacing and
// fractions.

#include <pyra/pyra.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kEdge = 512u;
constexpr auto kTile = 256u;
constexpr auto kZLevels = 4u;
constexpr auto kMpp = 0.5;
// Census slide: 4 mm square, enough nuclei for percent-level statistics.
constexpr auto kCensusEdge = 8192u;


void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


std::string utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return { text.begin(), text.end() };
}


pyra_synthetic_options small_options ()
{
	auto options = pyra_synthetic_options{};
	pyra_synthetic_options_init( &options );
	options.width = options.height = kEdge;
	options.tile_edge = kTile;
	options.z_levels = kZLevels;
	options.mpp = kMpp;
	return options;
}


void make ( const pyra_synthetic_options &options, const std::filesystem::path &path )
{
	auto error = pyra_error{};
	require( pyra_make_synthetic( &options, utf8( path ).c_str(), &error ) == PYRA_OK, error.message );
}


struct Slide {
	pyra_context *context_ = nullptr;
	pyra_wsi *wsi_ = nullptr;

	explicit Slide ( const std::filesystem::path &path ) {
		auto error = pyra_error{};
		require( pyra_context_create( nullptr, &context_, &error ) == PYRA_OK, error.message );
		require( pyra_open( context_, utf8( path ).c_str(), &wsi_, &error ) == PYRA_OK, error.message );
	}

	~Slide () {
		if (wsi_)
			pyra_close( wsi_, nullptr );
		pyra_context_destroy( context_, nullptr );
	}

	Slide ( const Slide & ) = delete;
	Slide &operator= ( const Slide & ) = delete;

	// Plane index by focal depth, microns.
	std::map<double, uint32_t> planes () const {
		auto count = uint32_t{};
		require( pyra_get_plane_count( wsi_, &count, nullptr ) == PYRA_OK, "Plane count failed" );
		auto out = std::map<double, uint32_t>{};
		for (auto i = 0u; i < count; ++i)
			out[info( i ).z_um] = i;
		return out;
	}

	pyra_plane_info info ( uint32_t index ) const {
		auto value = pyra_plane_info{};
		value.struct_size = sizeof( value );
		require( pyra_get_plane_info( wsi_, index, &value, nullptr ) == PYRA_OK, "Plane info failed" );
		return value;
	}

	// The whole base level of one plane, RGB8.
	std::vector<uint8_t> base ( uint32_t plane ) const {
		return region( plane, 0, 0, kEdge, kEdge );
	}

	// A base-pixel rectangle of one plane, RGB8.
	std::vector<uint8_t> region ( uint32_t plane, uint32_t x, uint32_t y, uint32_t width, uint32_t height ) const {
		const auto request = pyra_image_request{ sizeof( pyra_image_request ), plane,
			{ x * kMpp, y * kMpp, width * kMpp, height * kMpp }, kMpp };
		auto layout = pyra_image_layout{};
		layout.struct_size = sizeof( layout );
		auto error = pyra_error{};
		require( pyra_get_image_layout( wsi_, &request, &layout, &error ) == PYRA_OK, error.message );
		require( layout.width == width && layout.height == height && layout.pixel_format == PYRA_PIXEL_RGB8,
			"Base layout differs" );
		auto pixels = std::vector<uint8_t>( layout.capacity_bytes );
		auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
			pixels.data(), pixels.size(), layout.row_stride_bytes };
		require( pyra_get_image( wsi_, &request, &buffer, &error ) == PYRA_OK, error.message );
		return pixels;
	}
};


pyra_synthetic_nuclei census ( const pyra_synthetic_options &options )
{
	auto nuclei = pyra_synthetic_nuclei{};
	nuclei.struct_size = sizeof( nuclei );
	auto error = pyra_error{};
	require( pyra_synthetic_count_nuclei( &options, 0, 0, options.width, options.height, &nuclei, &error ) == PYRA_OK,
		error.message );
	return nuclei;
}


pyra_synthetic_options census_options ()
{
	auto options = pyra_synthetic_options{};
	pyra_synthetic_options_init( &options );
	options.width = options.height = kCensusEdge;
	return options;
}


// Mean squared horizontal and vertical green difference: higher is sharper.
double gradient_energy ( const std::vector<uint8_t> &rgb )
{
	auto sum = 0.0;
	auto count = 0.0;
	for (auto y = 0u; y + 1 < kEdge; ++y) {
		for (auto x = 0u; x + 1 < kEdge; ++x) {
			const auto g = int( rgb[( y * kEdge + x ) * 3 + 1] );
			const auto gx = int( rgb[( y * kEdge + x + 1 ) * 3 + 1] ) - g;
			const auto gy = int( rgb[( ( y + 1 ) * kEdge + x ) * 3 + 1] ) - g;
			sum += gx * gx + gy * gy;
			count += 1.0;
		}
	}
	return sum / count;
}


template <typename F>
void run ( const char *name, F &&test )
{
	const auto start = std::chrono::steady_clock::now();
	test();
	const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - start ).count();
	std::cout << std::format( "{}: PASS ({:.1f} ms)\n", name, us / 1000.0 );
}


void test_synthetic_invalid_options ( const std::filesystem::path &directory )
{
	const auto path = directory / "synthetic_invalid.ptf";
	std::filesystem::remove( path );
	auto check = [&] ( pyra_synthetic_options options, pyra_status expected, const char *what ) {
		auto error = pyra_error{};
		require( pyra_make_synthetic( &options, utf8( path ).c_str(), &error ) == expected,
			std::format( "{} was not refused as expected ({})", what, error.message ) );
		require( error.message[0] != '\0', std::format( "{} gave no reason", what ) );
		require( !std::filesystem::exists( path ), std::format( "{} left a file", what ) );
	};
	auto options = small_options();
	options.tile_edge = 100;
	check( options, PYRA_INVALID_ARGUMENT, "Tile edge 100" );
	options = small_options();
	options.z_levels = 0;
	check( options, PYRA_INVALID_ARGUMENT, "Zero focal planes" );
	options = small_options();
	options.quality = PYRA_QUALITY_LOSSLESS;
	check( options, PYRA_INVALID_ARGUMENT, "Lossless JPEG" );
	options = small_options();
	options.codec = 99;
	check( options, PYRA_INVALID_ARGUMENT, "Unknown codec" );
	options = small_options();
	options.mpp = 0.0;
	check( options, PYRA_INVALID_ARGUMENT, "Zero mpp" );
	options = small_options();
	options.z_step_um = 0.0;
	check( options, PYRA_INVALID_ARGUMENT, "Zero plane step" );
	options = small_options();
	options.focus_fraction = -0.1;
	check( options, PYRA_INVALID_ARGUMENT, "Focus below the stack" );
	options = small_options();
	options.nucleus_diameter_um = PYRA_SYNTHETIC_NUCLEUS_MIN_UM / 2;
	check( options, PYRA_INVALID_ARGUMENT, "Tiny nuclei" );
	options = small_options();
	options.nucleus_spacing_um = PYRA_SYNTHETIC_SPACING_MAX_UM * 2;
	check( options, PYRA_INVALID_ARGUMENT, "Huge spacing" );
	options = small_options();
	options.positive_fraction = 1.5;
	check( options, PYRA_INVALID_ARGUMENT, "Positive fraction above 1" );
	options = small_options();
	options.tumour_fraction = std::nan( "" );
	check( options, PYRA_INVALID_ARGUMENT, "NaN tumour fraction" );
	options = small_options();
	options.lymphocyte_fraction = -1.0;
	check( options, PYRA_INVALID_ARGUMENT, "Negative lymphocyte fraction" );
	auto nuclei = pyra_synthetic_nuclei{};
	nuclei.struct_size = sizeof( nuclei );
	require( pyra_synthetic_count_nuclei( &options, 0, 0, kEdge, kEdge, &nuclei, nullptr ) == PYRA_INVALID_ARGUMENT,
		"The census accepted invalid options" );
}


void test_synthetic_zstack_round_trip ( const std::filesystem::path &path )
{
	auto slide = Slide( path );
	const auto planes = slide.planes();
	require( planes.size() == kZLevels, std::format( "Expected {} planes, found {}", kZLevels, planes.size() ) );
	auto z = 0.0;
	for (const auto &[depth, index] : planes) {
		require( depth == z, std::format( "Plane depth {} where {} was expected", depth, z ) );
		const auto info = slide.info( index );
		require( info.level_count == 2 && info.native_mpp == kMpp && info.pixel_format == PYRA_PIXEL_RGB8 &&
			info.bounds.width_um == kEdge * kMpp && info.bounds.height_um == kEdge * kMpp,
			std::format( "Plane z={} ladder or metric differs", depth ) );
		z += 1.0;
	}
}


void test_synthetic_focus_order ( const std::filesystem::path &path )
{
	auto slide = Slide( path );
	auto energy = std::vector<double>{};
	auto dark = 0u;
	for (const auto &[depth, index] : slide.planes()) {
		const auto pixels = slide.base( index );
		energy.push_back( gradient_energy( pixels ) );
		if (depth == 1.0) {
			for (auto i = 0u; i < pixels.size(); i += 3)
				dark += pixels[i + 1] < 120;
		}
	}
	require( dark > kEdge * kEdge / 100, std::format( "In-focus plane has too few nucleus pixels ({})", dark ) );
	require( energy[1] > energy[2] && energy[2] > energy[0] && energy[0] > energy[3],
		std::format( "Focus order differs: z0 {:.1f}, z1 {:.1f}, z2 {:.1f}, z3 {:.1f}",
			energy[0], energy[1], energy[2], energy[3] ) );
}


// focus_fraction moves the sharp plane to either end of the stack.
void test_synthetic_focus_position ( const std::filesystem::path &directory )
{
	for (const auto fraction : { 0.0, 1.0 }) {
		auto options = small_options();
		options.focus_fraction = fraction;
		const auto path = directory / std::format( "synthetic_focus_{}.ptf", int( fraction ) );
		make( options, path );
		auto slide = Slide( path );
		auto energy = std::vector<double>{};
		for (const auto &[depth, index] : slide.planes())
			energy.push_back( gradient_energy( slide.base( index ) ) );
		const auto falling = energy[0] > energy[1] && energy[1] > energy[2] && energy[2] > energy[3];
		const auto rising = energy[0] < energy[1] && energy[1] < energy[2] && energy[2] < energy[3];
		require( fraction == 0.0 ? falling : rising,
			std::format( "Focus {} gave z0 {:.1f}, z1 {:.1f}, z2 {:.1f}, z3 {:.1f}",
				fraction, energy[0], energy[1], energy[2], energy[3] ) );
	}
}


void test_synthetic_z_step ( const std::filesystem::path &directory )
{
	auto options = small_options();
	options.z_levels = 3;
	options.z_step_um = 2.5;
	const auto path = directory / "synthetic_z_step.ptf";
	make( options, path );
	auto depth = 0.0;
	for (const auto &[z, index] : Slide( path ).planes()) {
		require( std::abs( z - depth ) < 1e-6, std::format( "Plane depth {} where {} was expected", z, depth ) );
		depth += options.z_step_um;
	}
	require( depth == 7.5, "Plane count differs" );
}


void test_synthetic_census_positive_fraction ()
{
	for (const auto fraction : { 0.0, 0.3, 1.0 }) {
		auto options = census_options();
		options.positive_fraction = fraction;
		const auto nuclei = census( options );
		require( nuclei.tumour > 5000, std::format( "Only {} tumour nuclei counted", nuclei.tumour ) );
		const auto measured = double( nuclei.tumour_positive ) / double( nuclei.tumour );
		const auto tolerance = fraction == 0.0 || fraction == 1.0 ? 0.0 : 0.04;
		require( std::abs( measured - fraction ) <= tolerance,
			std::format( "Positive fraction {:.3f} where {} was requested", measured, fraction ) );
	}
}


void test_synthetic_census_nucleus_diameter ()
{
	for (const auto diameter : { 5.0, 8.0, 15.0 }) {
		auto options = census_options();
		options.nucleus_diameter_um = diameter;
		const auto measured = census( options ).tumour_diameter_um;
		require( std::abs( measured / diameter - 1.0 ) < 0.05,
			std::format( "Mean tumour diameter {:.2f} um where {} was requested", measured, diameter ) );
	}
}


// Spacing is sqrt(area per nucleus): doubling it quarters the count.
void test_synthetic_census_spacing ()
{
	auto options = census_options();
	options.tumour_fraction = 1.0;
	const auto near = census( options ).tumour;
	options.nucleus_spacing_um *= 2.0;
	const auto far = census( options ).tumour;
	const auto ratio = double( near ) / double( far );
	require( std::abs( ratio / 4.0 - 1.0 ) < 0.15,
		std::format( "Doubling the spacing divided the count by {:.2f}, not 4", ratio ) );
}


// Tumour area share, estimated from the counts: tumour squares are filled with
// probability 0.85 and stromal ones with 0.4.
void test_synthetic_census_tumour_fraction ()
{
	constexpr auto kTumourFill = 0.85;
	constexpr auto kStromaFill = 0.4;
	for (const auto fraction : { 0.0, 0.2, 0.7, 1.0 }) {
		auto options = census_options();
		options.tumour_fraction = fraction;
		const auto nuclei = census( options );
		const auto tumour = double( nuclei.tumour ) / kTumourFill;
		const auto stroma = double( nuclei.fibroblast + nuclei.lymphocyte ) / kStromaFill;
		const auto measured = tumour / ( tumour + stroma );
		const auto tolerance = fraction == 0.0 || fraction == 1.0 ? 0.0 : 0.08;
		require( std::abs( measured - fraction ) <= tolerance,
			std::format( "Tumour area share {:.3f} where {} was requested", measured, fraction ) );
	}
}


void test_synthetic_census_lymphocyte_fraction ()
{
	for (const auto fraction : { 0.0, 0.4, 1.0 }) {
		auto options = census_options();
		options.lymphocyte_fraction = fraction;
		const auto nuclei = census( options );
		const auto measured = double( nuclei.lymphocyte ) / double( nuclei.lymphocyte + nuclei.fibroblast );
		const auto tolerance = fraction == 0.0 || fraction == 1.0 ? 0.0 : 0.03;
		require( std::abs( measured - fraction ) <= tolerance,
			std::format( "Lymphocyte share {:.3f} where {} was requested", measured, fraction ) );
	}
}


void test_synthetic_codecs ( const std::filesystem::path &directory )
{
	struct Codec { pyra_codec value; const char *name; };
	const Codec codecs[] = { { PYRA_CODEC_JPEG, "jpeg" }, { PYRA_CODEC_JPEG2000, "jp2k" },
		{ PYRA_CODEC_HTJ2K, "htj2k" }, { PYRA_CODEC_JPEGXL, "jxl" }, { PYRA_CODEC_JPEGXR, "jxr" } };
	auto reference = std::vector<uint8_t>{};
	for (const auto &codec : codecs) {
		auto options = small_options();
		options.z_levels = 1;
		options.codec = codec.value;
		const auto path = directory / std::format( "synthetic_{}.ptf", codec.name );
		if (!pyra_codec_can_encode( codec.value )) {
			auto error = pyra_error{};
			require( pyra_make_synthetic( &options, utf8( path ).c_str(), &error ) == PYRA_UNSUPPORTED,
				std::format( "Unavailable {} was not refused as unsupported", codec.name ) );
			continue;
		}
		make( options, path );
		(void)Slide( path ).base( 0 );
		if (codec.value == PYRA_CODEC_JPEG)
			continue;
		options.quality = PYRA_QUALITY_LOSSLESS;
		const auto lossless = directory / std::format( "synthetic_{}_lossless.ptf", codec.name );
		make( options, lossless );
		const auto pixels = Slide( lossless ).base( 0 );
		if (reference.empty())
			reference = pixels;
		require( pixels == reference, std::format( "Lossless {} pixels differ from another lossless codec", codec.name ) );
	}
}


// The single member of the gallery made from `directory`: its mosaic rectangle,
// read from the manifest line member=<x> <y> <w> <h> ...
std::array<uint32_t, 4> gallery_member_rect ( const std::filesystem::path &manifest )
{
	auto file = std::ifstream( manifest );
	auto line = std::string{};
	while (std::getline( file, line )) {
		if (line.rfind( "member=", 0 ) != 0)
			continue;
		auto fields = std::istringstream( line.substr( 7 ) );
		auto rect = std::array<uint32_t, 4>{};
		for (auto &value : rect)
			fields >> value;
		require( bool( fields ), "Unreadable gallery member line" );
		return rect;
	}
	throw std::runtime_error( "The gallery names no member" );
}


// An untiled TIFF written alone into its own directory, made into a gallery.
std::filesystem::path untiled_gallery ( const std::filesystem::path &directory, const char *name,
	pyra_codec codec, std::array<uint32_t, 4> *member )
{
	const auto folder = directory / name;
	std::filesystem::remove_all( folder );
	std::filesystem::create_directories( folder );
	auto options = small_options();
	options.tile_edge = 0;
	options.z_levels = 1;
	options.codec = codec;
	make( options, folder / ( std::string( name ) + ".tif" ) );
	auto manifest = std::array<char, 1024>{};
	auto error = pyra_error{};
	require( pyra_make_gallery( utf8( folder ).c_str(), 0.0, manifest.data(), manifest.size(), &error ) == PYRA_OK,
		error.message );
	*member = gallery_member_rect( manifest.data() );
	require( ( *member )[2] == kEdge && ( *member )[3] == kEdge,
		std::format( "Untiled member is {}x{}, not {}x{}", ( *member )[2], ( *member )[3], kEdge, kEdge ) );
	return manifest.data();
}


void test_synthetic_untiled_lzw_round_trip ( const std::filesystem::path &directory )
{
	auto member = std::array<uint32_t, 4>{};
	const auto manifest = untiled_gallery( directory, "synthetic_untiled_lzw", PYRA_CODEC_LZW, &member );
	const auto pixels = Slide( manifest ).region( 0, member[0], member[1], member[2], member[3] );
	for (const auto codec : { PYRA_CODEC_JPEGXL, PYRA_CODEC_JPEG2000, PYRA_CODEC_HTJ2K, PYRA_CODEC_JPEGXR }) {
		if (!pyra_codec_can_encode( codec ))
			continue;
		auto options = small_options();
		options.z_levels = 1;
		options.codec = codec;
		options.quality = PYRA_QUALITY_LOSSLESS;
		const auto reference = directory / "synthetic_untiled_reference.ptf";
		make( options, reference );
		require( pixels == Slide( reference ).base( 0 ), "LZW pixels differ from the lossless tiled slide" );
		return;
	}
	throw std::runtime_error( "No lossless tiled codec to compare LZW against" );
}


void test_synthetic_untiled_jpeg ( const std::filesystem::path &directory )
{
	auto member = std::array<uint32_t, 4>{};
	const auto manifest = untiled_gallery( directory, "synthetic_untiled_jpeg", PYRA_CODEC_JPEG, &member );
	const auto pixels = Slide( manifest ).region( 0, member[0], member[1], member[2], member[3] );
	auto tiled = small_options();
	tiled.z_levels = 1;
	const auto reference = directory / "synthetic_untiled_jpeg_reference.ptf";
	make( tiled, reference );
	const auto expected = Slide( reference ).base( 0 );
	auto error = 0.0;
	for (auto i = size_t{ 0 }; i < pixels.size(); ++i)
		error += std::abs( int( pixels[i] ) - int( expected[i] ) );
	error /= double( pixels.size() );
	require( error < 6.0, std::format( "Untiled JPEG differs from the tiled JPEG by {:.2f} grey levels", error ) );
}


void test_synthetic_untiled_invalid ( const std::filesystem::path &directory )
{
	const auto path = directory / "synthetic_untiled_invalid.tif";
	std::filesystem::remove( path );
	auto check = [&] ( pyra_synthetic_options options, pyra_status expected, const char *what, bool dicom = false ) {
		auto error = pyra_error{};
		const auto write = dicom ? pyra_make_synthetic_dicom : pyra_make_synthetic;
		const auto target = dicom ? directory / "synthetic_untiled_invalid.dcm" : path;
		require( write( &options, utf8( target ).c_str(), &error ) == expected,
			std::format( "{} was not refused as expected ({})", what, error.message ) );
		require( error.message[0] != '\0', std::format( "{} gave no reason", what ) );
		require( !std::filesystem::exists( target ), std::format( "{} left a file", what ) );
	};
	auto untiled = small_options();
	untiled.tile_edge = 0;
	untiled.z_levels = 1;
	untiled.codec = PYRA_CODEC_LZW;
	auto options = untiled;
	options.z_levels = 2;
	check( options, PYRA_INVALID_ARGUMENT, "An untiled z-stack" );
	options = small_options();
	options.codec = PYRA_CODEC_LZW;
	check( options, PYRA_INVALID_ARGUMENT, "Tiled LZW" );
	options = untiled;
	options.codec = PYRA_CODEC_JPEG2000;
	check( options, PYRA_INVALID_ARGUMENT, "Untiled JPEG 2000" );
	options = untiled;
	options.width = PYRA_SYNTHETIC_UNTILED_EDGE_MAX + 1;
	check( options, PYRA_INVALID_ARGUMENT, "An oversized untiled image" );
	options = untiled;
	options.tile_edge = 16;
	check( options, PYRA_INVALID_ARGUMENT, "Tile edge 16" );
	options = untiled;
	options.codec = PYRA_CODEC_JPEG;
	check( options, pyra_synthetic_dicom_can_encode( PYRA_CODEC_JPEG ) ? PYRA_INVALID_ARGUMENT : PYRA_UNSUPPORTED,
		"An untiled DICOM", true );
}


void test_synthetic_can_encode_layouts ()
{
	struct Case { pyra_codec codec; bool tiled; bool untiled; };
	const Case cases[] = { { PYRA_CODEC_JPEG, true, true }, { PYRA_CODEC_LZW, false, true },
		{ PYRA_CODEC_JPEG2000, true, false }, { PYRA_CODEC_HTJ2K, true, false },
		{ PYRA_CODEC_JPEGXL, true, false }, { PYRA_CODEC_JPEGXR, true, false }, { 99, false, false } };
	for (const auto &item : cases) {
		const auto built = item.codec == PYRA_CODEC_LZW || pyra_codec_can_encode( item.codec ) != 0;
		require( ( pyra_synthetic_can_encode( item.codec, PYRA_SYNTHETIC_TILE_MIN ) != 0 ) == ( item.tiled && built ) &&
			( pyra_synthetic_can_encode( item.codec, 0 ) != 0 ) == ( item.untiled && built ),
			std::format( "Codec {} offered for the wrong layouts", item.codec ) );
	}
	require( pyra_codec_can_encode( PYRA_CODEC_LZW ) == 1, "LZW is not reported encodable" );
}


void test_synthetic_cancel ( const std::filesystem::path &directory )
{
	const auto path = directory / "synthetic_cancelled.ptf";
	auto options = small_options();
	auto calls = 0;
	options.progress_user = &calls;
	options.progress = [] ( void *user, uint64_t done, uint64_t total ) -> int {
		++*static_cast<int *>( user );
		return done < 3 && done < total ? 1 : 0;
	};
	auto error = pyra_error{};
	require( pyra_make_synthetic( &options, utf8( path ).c_str(), &error ) == PYRA_INCOMPLETE,
		"Cancellation was not reported as incomplete" );
	require( calls == 3, std::format( "Progress ran {} times after cancelling at 3", calls ) );
	require( !std::filesystem::exists( path ), "Cancelled write left a file" );
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2) {
		std::cerr << "usage: pyra_synthetic_test <output directory>\n";
		return 2;
	}
	try {
		const auto directory = std::filesystem::path( argv[1] );
		std::filesystem::create_directories( directory );
		const auto stack = directory / "synthetic_stack.ptf";
		make( small_options(), stack );
		run( "test_synthetic_invalid_options", [&] { test_synthetic_invalid_options( directory ); } );
		run( "test_synthetic_zstack_round_trip", [&] { test_synthetic_zstack_round_trip( stack ); } );
		run( "test_synthetic_focus_order", [&] { test_synthetic_focus_order( stack ); } );
		run( "test_synthetic_focus_position", [&] { test_synthetic_focus_position( directory ); } );
		run( "test_synthetic_z_step", [&] { test_synthetic_z_step( directory ); } );
		run( "test_synthetic_census_positive_fraction", test_synthetic_census_positive_fraction );
		run( "test_synthetic_census_nucleus_diameter", test_synthetic_census_nucleus_diameter );
		run( "test_synthetic_census_spacing", test_synthetic_census_spacing );
		run( "test_synthetic_census_tumour_fraction", test_synthetic_census_tumour_fraction );
		run( "test_synthetic_census_lymphocyte_fraction", test_synthetic_census_lymphocyte_fraction );
		run( "test_synthetic_codecs", [&] { test_synthetic_codecs( directory ); } );
		run( "test_synthetic_cancel", [&] { test_synthetic_cancel( directory ); } );
		run( "test_synthetic_untiled_lzw_round_trip", [&] { test_synthetic_untiled_lzw_round_trip( directory ); } );
		run( "test_synthetic_untiled_jpeg", [&] { test_synthetic_untiled_jpeg( directory ); } );
		run( "test_synthetic_untiled_invalid", [&] { test_synthetic_untiled_invalid( directory ); } );
		run( "test_synthetic_can_encode_layouts", test_synthetic_can_encode_layouts );
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
