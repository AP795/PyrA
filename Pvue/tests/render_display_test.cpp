// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// render_display on real slides: the channel fixtures written by
// pyra_standalone (write_c_api_channel_fixtures) into the test output folder.

#include "view_compose.h"

#include <pyra/pyra.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

bool run_case ( const char *name, void ( *test )() );

namespace {

std::filesystem::path g_output;

void require ( bool condition, const std::string &message )
{
	if (!condition) throw std::runtime_error( message );
}


void append ( void *user, const void *data, size_t size )
{
	static_cast<std::string *>( user )->assign( static_cast<const char *>( data ), size );
}


// A context with one slide opened from a fixture, and its remote sister opened
// from the slide's geometry, holding no tile yet.
struct Slides {
	pyra_context *context = nullptr;
	pyra_wsi *native = nullptr;
	pyra_wsi *remote = nullptr;

	explicit Slides ( const char *name ) {
		require( pyra_context_create( nullptr, &context, nullptr ) == PYRA_OK, "context failed" );
		const auto path = ( g_output / name ).u8string();
		require( pyra_open( context, reinterpret_cast<const char *>( path.c_str() ), &native, nullptr ) == PYRA_OK,
			std::format( "{} does not open (run pyra_standalone first)", name ) );
		auto geometry = std::string{};
		require( pyra_get_geometry( native, append, &geometry, nullptr ) == PYRA_OK, "no geometry" );
		require( pyra_open_remote( context, geometry.data(), geometry.size(), &remote, nullptr ) == PYRA_OK,
			"the geometry does not open remotely" );
	}
	~Slides () {
		if (remote) pyra_close( remote, nullptr );
		if (native) pyra_close( native, nullptr );
		if (context) pyra_context_destroy( context, nullptr );
	}
	Slides ( const Slides & ) = delete;
	Slides &operator= ( const Slides & ) = delete;

// Puts every tile the remote slide's renders asked for, read from the native
// slide as the server sends it; false when none was missing.
	bool feed () {
		auto ids = std::vector<pyra_tile_id>( 256 );
		auto count = size_t{};
		require( pyra_take_missing_tiles( remote, ids.data(), ids.size(), &count, nullptr ) == PYRA_OK, "take failed" );
		for (auto i = size_t{}; i < count; ++i) {
			auto bytes = std::string{};
			require( pyra_read_encoded_tile( native, ids[i].level_index, ids[i].tile_index, append, &bytes, nullptr ) == PYRA_OK,
				"tile read failed" );
			require( pyra_put_encoded_tile( remote, ids[i].level_index, ids[i].tile_index, bytes.data(), bytes.size(), nullptr ) ==
				PYRA_OK, "tile put failed" );
		}
		return count > 0;
	}
};


pyra::pvue::ViewRect whole ( pyra_wsi *wsi, double scale )
{
	auto info = pyra_plane_info{};
	info.struct_size = sizeof( info );
	require( pyra_get_plane_info( wsi, 0, &info, nullptr ) == PYRA_OK, "no plane" );
	return { info.bounds.x_um, info.bounds.y_um, info.bounds.width_um, info.bounds.height_um, info.native_mpp * scale };
}


pyra::pvue::DisplayRecipe recipe_of ( pyra_wsi *wsi, uint32_t reference )
{
	auto planes = std::vector<pyra::pvue::PlaneDesc>{};
	auto gallery = false;
	auto error = std::string{};
	require( pyra::pvue::slide_planes( wsi, &planes, &gallery, &error ), error );
	return pyra::pvue::display_recipe( planes, gallery, reference );
}


// The composite rendered on the remote slide from the tiles the native one
// serves equals the composite rendered on the native slide, and both carry each
// channel's own one-plane render in their R, G and B components.
void check_remote_equals_native ( const char *fixture, double scale )
{
	auto slides = Slides( fixture );
	const auto recipe = recipe_of( slides.native, 0 );
	require( recipe.composite() && recipe_of( slides.remote, 0 ).planes == recipe.planes,
		"the fixture has no colour composite" );
	const auto view = whole( slides.native, scale );
	auto native = pyra::pvue::Frame{};
	auto error = std::string{};
	require( pyra::pvue::render_display( slides.native, recipe, view, &native, &error ) == PYRA_OK, error );
	auto remote = pyra::pvue::Frame{};
	pyra_status status = PYRA_INCOMPLETE;
	for (auto pass = 0; pass < 16 && status == PYRA_INCOMPLETE; ++pass) {
		status = pyra::pvue::render_display( slides.remote, recipe, view, &remote, &error );
		require( status == PYRA_OK || status == PYRA_INCOMPLETE, error );
		if (status == PYRA_INCOMPLETE)
			require( slides.feed(), "an incomplete render asked for no tile" );
	}
	require( status == PYRA_OK, "the remote composite never completed" );
	require( remote.width == native.width && remote.height == native.height && remote.rgba == native.rgba,
		"the remote composite differs from the native one" );
	for (auto c = 0; c < 3; ++c) {
		auto plane = pyra::pvue::Frame{};
		require( pyra::pvue::render_plane( slides.native, recipe.planes[c], view, &plane, &error ) == PYRA_OK, error );
		require( plane.rgba.size() == native.rgba.size(), "a channel renders to another size" );
		for (size_t i = 0; i < plane.rgba.size(); i += 4)
			require( native.rgba[i + c] == plane.rgba[i], std::format( "component {} differs at pixel {}", c, i / 4 ) );
	}
}


void test_render_display_remote_equals_native ()
{
	check_remote_equals_native( "channels_rgb.ptf", 1 );
	check_remote_equals_native( "channels_rgb.ptf", 4 );
}


void test_render_display_remote_equals_native_16bit ()
{
	if (!std::filesystem::exists( g_output / "channels_rgb16.ptf" ))
		return;
	check_remote_equals_native( "channels_rgb16.ptf", 1 );
}


void test_render_display_missing_tiles_incomplete ()
{
	auto slides = Slides( "channels_rgb.ptf" );
	const auto recipe = recipe_of( slides.remote, 1 );
	auto frame = pyra::pvue::Frame{};
	auto error = std::string{};
	require( pyra::pvue::render_display( slides.remote, recipe, whole( slides.remote, 1 ), &frame, &error ) ==
		PYRA_INCOMPLETE, "a remote slide without tiles rendered complete" );
	auto ids = std::vector<pyra_tile_id>( 1024 );
	auto count = size_t{};
	require( pyra_take_missing_tiles( slides.remote, ids.data(), ids.size(), &count, nullptr ) == PYRA_OK && count > 0,
		"no tile was asked for" );
	auto levels = std::vector<uint32_t>{};
	for (auto i = size_t{}; i < count; ++i)
		if (std::find( levels.begin(), levels.end(), ids[i].level_index ) == levels.end())
			levels.push_back( ids[i].level_index );
	require( levels.size() == 3, std::format( "the composite asked for tiles of {} levels, not one per channel",
		levels.size() ) );
}


// A whole-slide view of a remote slide reads the coarse level it chose while that
// level's tiles are still to arrive: a frame black only for want of tiles is no
// evidence the level is blank, so the request must not fall to the finest level.
void test_render_whole_slide_remote_keeps_coarse_level ()
{
	auto slides = Slides( "coarse.ptf" );
	const auto view = whole( slides.remote, 8 );
	auto frame = pyra::pvue::Frame{};
	auto error = std::string{};
	require( pyra::pvue::render_plane( slides.remote, 0, view, &frame, &error ) == PYRA_INCOMPLETE,
		"a remote slide without tiles rendered complete" );
	auto ids = std::vector<pyra_tile_id>( 256 );
	auto count = size_t{};
	require( pyra_take_missing_tiles( slides.remote, ids.data(), ids.size(), &count, nullptr ) == PYRA_OK && count > 0,
		"no tile was asked for" );
	for (auto i = size_t{}; i < count; ++i)
		require( ids[i].level_index != 0, "the whole-slide view fell back to the finest level" );
	for (auto i = size_t{}; i < count; ++i) {
		auto bytes = std::string{};
		require( pyra_read_encoded_tile( slides.native, ids[i].level_index, ids[i].tile_index, append, &bytes, nullptr ) ==
			PYRA_OK, "tile read failed" );
		require( pyra_put_encoded_tile( slides.remote, ids[i].level_index, ids[i].tile_index, bytes.data(), bytes.size(),
			nullptr ) == PYRA_OK, "tile put failed" );
	}
	pyra_status status = PYRA_INCOMPLETE;
	for (auto pass = 0; pass < 4 && status == PYRA_INCOMPLETE; ++pass) {
		status = pyra::pvue::render_plane( slides.remote, 0, view, &frame, &error );
		if (status == PYRA_INCOMPLETE)
			slides.feed();
	}
	auto native = pyra::pvue::Frame{};
	require( status == PYRA_OK && pyra::pvue::render_plane( slides.native, 0, view, &native, &error ) == PYRA_OK,
		"the whole-slide view never completed" );
	require( frame.width == native.width && frame.height == native.height && frame.rgba == native.rgba,
		"the remote whole-slide view differs from the native one" );
}


void test_render_display_channels_without_rgb_are_single ()
{
	auto slides = Slides( "channels_two.ptf" );
	for (auto plane = 0u; plane < 2; ++plane)
		require( recipe_of( slides.native, plane ).kind == pyra::pvue::DisplayKind::Gray, "two channels were composed" );
}

} // namespace


bool run_render_display_tests ( const std::filesystem::path &output )
{
	g_output = output;
	auto passed = true;
	passed &= run_case( "test_render_display_remote_equals_native", test_render_display_remote_equals_native );
	passed &= run_case( "test_render_display_remote_equals_native_16bit", test_render_display_remote_equals_native_16bit );
	passed &= run_case( "test_render_display_missing_tiles_incomplete", test_render_display_missing_tiles_incomplete );
	passed &= run_case( "test_render_whole_slide_remote_keeps_coarse_level", test_render_whole_slide_remote_keeps_coarse_level );
	passed &= run_case( "test_render_display_channels_without_rgb_are_single",
		test_render_display_channels_without_rgb_are_single );
	return passed;
}
