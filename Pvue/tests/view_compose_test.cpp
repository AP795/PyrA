// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "display_rgba.h"
#include "view_compose.h"

#include "pyr.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

bool run_render_display_tests ( const std::filesystem::path &output );

namespace {

using pyra::pvue::DisplayKind;
using pyra::pvue::PlaneDesc;

void require ( bool condition, const std::string &message )
{
	if (!condition) throw std::runtime_error( message );
}


PlaneDesc gray ( const char *channel, double z = 0, pyra_pixel_format format = PYRA_PIXEL_GRAY8, double range = 255 )
{
	return { channel, format, range, z };
}


void expect ( const pyra::pvue::DisplayRecipe &recipe, DisplayKind kind, const std::vector<uint32_t> &planes )
{
	require( recipe.kind == kind, std::format( "kind {} instead of {}", int( recipe.kind ), int( kind ) ) );
	require( recipe.planes == planes, "the recipe names other planes" );
}


void test_display_recipe_rgb8_passthrough ()
{
	expect( pyra::pvue::display_recipe( { { "", PYRA_PIXEL_RGB8, 255, 0 } }, false, 0 ), DisplayKind::Rgb8, { 0 } );
}


void test_display_recipe_rgb16_narrowed ()
{
	expect( pyra::pvue::display_recipe( { { "", PYRA_PIXEL_RGB16, 4095, 0 } }, false, 0 ), DisplayKind::Rgb16, { 0 } );
}


void test_display_recipe_named_rgb_is_colour ()
{
	const auto planes = std::vector<PlaneDesc>{ gray( "DAPI" ), gray( "b" ), gray( "G" ), gray( "R" ), gray( "R" ) };
	for (auto reference = 0u; reference < planes.size(); ++reference)
		expect( pyra::pvue::display_recipe( planes, false, reference ), DisplayKind::Channels, { 3, 2, 1 } );
}


void test_display_recipe_channels_without_rgb_are_single ()
{
	const auto planes = std::vector<PlaneDesc>{ gray( "DAPI" ), gray( "FITC" ), gray( "R" ), gray( "G" ) };
	expect( pyra::pvue::display_recipe( planes, false, 1 ), DisplayKind::Gray, { 1 } );
	expect( pyra::pvue::display_recipe( planes, false, 2 ), DisplayKind::Gray, { 2 } );
}


void test_display_recipe_unnamed_gray ()
{
	expect( pyra::pvue::display_recipe( { gray( "", 0, PYRA_PIXEL_GRAY16, 4095 ) }, false, 0 ), DisplayKind::Gray, { 0 } );
}


void test_display_recipe_rgb_plane_beside_channels_stays_rgb ()
{
	const auto planes = std::vector<PlaneDesc>{ { "", PYRA_PIXEL_RGB8, 255, 0 }, gray( "R" ), gray( "G" ), gray( "B" ) };
	expect( pyra::pvue::display_recipe( planes, false, 0 ), DisplayKind::Rgb8, { 0 } );
	expect( pyra::pvue::display_recipe( planes, false, 2 ), DisplayKind::Channels, { 1, 2, 3 } );
}


void test_display_recipe_gallery_has_no_composite ()
{
	const auto planes = std::vector<PlaneDesc>{ gray( "R" ), gray( "G" ), gray( "B" ) };
	expect( pyra::pvue::display_recipe( planes, true, 1 ), DisplayKind::Gallery, { 1 } );
}


void test_display_recipe_z_of_reference_plane ()
{
	const auto nan = std::numeric_limits<double>::quiet_NaN();
	const auto planes = std::vector<PlaneDesc>{ gray( "R", 0 ), gray( "G", 0 ), gray( "B", 0 ),
		gray( "R", 2 ), gray( "G", 2 ), gray( "B", 2 ), gray( "R", nan ), gray( "G", nan ), gray( "B", 1 ) };
	expect( pyra::pvue::display_recipe( planes, false, 4 ), DisplayKind::Channels, { 3, 4, 5 } );
	expect( pyra::pvue::display_recipe( planes, false, 1 ), DisplayKind::Channels, { 0, 1, 2 } );
	expect( pyra::pvue::display_recipe( planes, false, 6 ), DisplayKind::Gray, { 6 } );
	expect( pyra::pvue::display_recipe( planes, false, 99 ), DisplayKind::None, {} );
}


void test_compose_display_equals_compose_rgb_from_channels ()
{
	constexpr auto w = 5u, h = 3u;
	auto samples = std::vector<std::vector<uint16_t>>( 3, std::vector<uint16_t>( w * h ) );
	auto rasters = std::array<pyra::pvue::ChannelRaster, 3>{};
	auto pixels = std::array<priml::Pwhl<uint16_t>, 3>{};
	auto named = std::vector<priml::NamedChannelPixels>{};
	const char *names[3] = { "R", "G", "B" };
	for (auto c = 0u; c < 3; ++c) {
		pixels[c] = priml::Pwhl<uint16_t>( priml::Dim2D<int>{ int( w ), int( h ) } );
		for (auto i = 0u; i < w * h; ++i) {
			samples[c][i] = uint16_t( ( i * 397 + c * 1201 ) % 4096 );
			pixels[c].rowp( int( i / w ) )[i % w] = samples[c][i];
		}
		rasters[c] = { reinterpret_cast<const unsigned char *>( samples[c].data() ), w * sizeof( uint16_t ), w, h,
			PYRA_PIXEL_GRAY16, 4095 };
		named.push_back( { names[c], &pixels[c], 4095 } );
	}
	auto rgba = std::vector<unsigned char>{};
	auto width = 0u, height = 0u;
	require( pyra::pvue::compose_display( rasters, &rgba, &width, &height ) && width == w && height == h,
		"composition failed" );
	const auto expected = priml::compose_rgb_from_channels( named );
	for (auto i = 0u; i < w * h; ++i) {
		const auto &pixel = expected.rowp( int( i / w ) )[i % w];
		require( rgba[i * 4] == pixel.r && rgba[i * 4 + 1] == pixel.g && rgba[i * 4 + 2] == pixel.b &&
			rgba[i * 4 + 3] == 255, std::format( "pixel {} differs", i ) );
	}
	rasters[1].width = w - 1;
	require( !pyra::pvue::compose_display( rasters, &rgba, &width, &height ), "channels of two sizes were composed" );
}


// The one-plane rule (display_rgba, doubles) and the composition's
// (narrow_to_8bit, integers) give the same 8 bits for every sample.
void test_display_rgba_equals_narrow_to_8bit ()
{
	for (const auto range : { 255, 4095, 65535 }) {
		auto samples = std::vector<uint16_t>( 65536 );
		for (auto v = 0u; v < samples.size(); ++v)
			samples[v] = uint16_t( v );
		auto layout = pyra_image_layout{};
		layout.struct_size = sizeof( layout );
		layout.pixel_format = PYRA_PIXEL_GRAY16;
		layout.width = uint32_t( samples.size() );
		layout.height = 1;
		layout.row_stride_bytes = samples.size() * sizeof( uint16_t );
		auto rgba = std::vector<unsigned char>( samples.size() * 4 );
		pyra::pvue::display_rgba( reinterpret_cast<const unsigned char *>( samples.data() ), layout, range, rgba.data() );
		for (auto v = 0u; v < samples.size(); ++v)
			require( rgba[v * 4] == priml::narrow_to_8bit( int( v ), range ),
				std::format( "value {} at range {}: {} vs {}", v, range, rgba[v * 4], priml::narrow_to_8bit( int( v ), range ) ) );
	}
}

} // namespace


bool run_case ( const char *name, void ( *test )() )
{
	const auto start = std::chrono::steady_clock::now();
	auto passed = true;
	try { test(); }
	catch (const std::exception &error) { std::cerr << name << ": " << error.what() << '\n'; passed = false; }
	std::cout << name << ": " << ( passed ? "PASS" : "FAIL" ) << " (" << std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - start ).count() << " us)\n";
	return passed;
}


int main ( int argc, char **argv )
{
	if (argc != 2) return 2;
	auto passed = true;
	passed &= run_case( "test_display_recipe_rgb8_passthrough", test_display_recipe_rgb8_passthrough );
	passed &= run_case( "test_display_recipe_rgb16_narrowed", test_display_recipe_rgb16_narrowed );
	passed &= run_case( "test_display_recipe_named_rgb_is_colour", test_display_recipe_named_rgb_is_colour );
	passed &= run_case( "test_display_recipe_channels_without_rgb_are_single",
		test_display_recipe_channels_without_rgb_are_single );
	passed &= run_case( "test_display_recipe_unnamed_gray", test_display_recipe_unnamed_gray );
	passed &= run_case( "test_display_recipe_rgb_plane_beside_channels_stays_rgb",
		test_display_recipe_rgb_plane_beside_channels_stays_rgb );
	passed &= run_case( "test_display_recipe_gallery_has_no_composite", test_display_recipe_gallery_has_no_composite );
	passed &= run_case( "test_display_recipe_z_of_reference_plane", test_display_recipe_z_of_reference_plane );
	passed &= run_case( "test_compose_display_equals_compose_rgb_from_channels",
		test_compose_display_equals_compose_rgb_from_channels );
	passed &= run_case( "test_display_rgba_equals_narrow_to_8bit", test_display_rgba_equals_narrow_to_8bit );
	passed &= run_render_display_tests( std::filesystem::path( std::u8string( reinterpret_cast<const char8_t *>( argv[1] ) ) ) );
	return passed ? 0 : 1;
}
