// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Pvue's browser client, compiled to WebAssembly: one remote ("sister") slide
// opened from the server's geometry, fed with the server's stored tiles and
// rendered here by Pvue's display rule (view_compose), which composes channels
// the server sends as they are stored. Flat exports for pvue_worker.js; one
// slide at a time, used from one worker thread.

#include "display_rgba.h"
#include "pvue_cache.h"
#include "pvue_http.h"
#include "view_compose.h"

#include <pyra/pyra.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kBytesPerMiB = uint64_t{ 1024 } * 1024;

struct Client {
	pyra_context *context = nullptr;
	uint64_t compressed_bytes = 0;		// the compressed cache's budget
	uint64_t decoded_bytes = 0;			// the decoded cache's budget (0: none)
	std::array<double, 10> cache{};		// pvue_client_cache's answer
	pyra_wsi *wsi = nullptr;
	std::string error;
	std::vector<pyra_tile_id> missing;
	std::vector<pyra::pvue::PlaneDesc> planes;
	bool gallery = false;
	pyra::pvue::DisplayRecipe recipe;
	pyra::pvue::Frame frame;
	std::vector<unsigned char> samples;
};

Client client;


void close_slide ()
{
	if (client.wsi)
		pyra_close( client.wsi, nullptr );
	if (client.context)
		pyra_context_destroy( client.context, nullptr );
	client.wsi = nullptr;
	client.context = nullptr;
	client.missing.clear();
	client.planes.clear();
	client.recipe = {};
}


int report ( pyra_status status, const pyra_error &error )
{
	client.error = status == PYRA_OK ? std::string{} : std::string( error.message );
	return status;
}


int report ( pyra_status status, const std::string &error )
{
	client.error = status == PYRA_OK ? std::string{} : error;
	return status;
}


pyra::pvue::ViewRect view_rect ( double x, double y, double width, double height, double mpp )
{
	return { x, y, width, height, mpp };
}

} // namespace


extern "C" {

// Opens the slide a geometry blob describes, with budgets of `tbf_mib` for the
// compressed and `pbf_mib` for the decoded tile cache (0: none), laid out as
// `topology` (PYRA_CACHE_ARENA or PYRA_CACHE_PER_LEVEL), and up to `threads`
// decode threads (the
// calling thread included; at most PYRA_CLIENT_MAX_THREADS, the module's pool).
// Returns a pyra_status.
EMSCRIPTEN_KEEPALIVE int pvue_client_open ( const uint8_t *geometry, size_t size, double tbf_mib, double pbf_mib,
	uint32_t topology, uint32_t threads )
{
	close_slide();
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	options.decode_threads = std::clamp<uint32_t>( threads, 1, PYRA_CLIENT_MAX_THREADS );
	options.max_image_pixels = uint64_t( pyra::pvue::kMaxRenderEdge ) * pyra::pvue::kMaxRenderEdge;
	options.compressed_cache_bytes = uint64_t( std::max( 1.0, tbf_mib ) ) * kBytesPerMiB;
	options.decoded_cache_bytes = uint64_t( std::max( 0.0, pbf_mib ) ) * kBytesPerMiB;
	options.cache_topology = topology == PYRA_CACHE_PER_LEVEL ? PYRA_CACHE_PER_LEVEL : PYRA_CACHE_ARENA;
	client.compressed_bytes = options.compressed_cache_bytes;
	client.decoded_bytes = options.decoded_cache_bytes;
	auto error = pyra_error{};
	auto status = pyra_context_create( &options, &client.context, &error );
	if (status == PYRA_OK)
		status = pyra_open_remote( client.context, geometry, size, &client.wsi, &error );
	if (status != PYRA_OK) {
		close_slide();
		return report( status, error );
	}
	auto reason = std::string{};
	if (!pyra::pvue::slide_planes( client.wsi, &client.planes, &client.gallery, &reason )) {
		close_slide();
		return report( PYRA_IO_ERROR, reason );
	}
	return report( PYRA_OK, error );
}


EMSCRIPTEN_KEEPALIVE void pvue_client_close ()
{
	close_slide();
}


EMSCRIPTEN_KEEPALIVE uint32_t pvue_client_plane_count ()
{
	return uint32_t( client.planes.size() );
}


// The display recipe around `reference` (a DisplayKind); pvue_client_recipe_planes
// points at its plane indices, pvue_client_recipe_count says how many.
EMSCRIPTEN_KEEPALIVE int pvue_client_recipe ( uint32_t reference )
{
	client.recipe = pyra::pvue::display_recipe( client.planes, client.gallery, reference );
	return int( client.recipe.kind );
}


EMSCRIPTEN_KEEPALIVE const uint32_t *pvue_client_recipe_planes () { return client.recipe.planes.data(); }
EMSCRIPTEN_KEEPALIVE uint32_t pvue_client_recipe_count () { return uint32_t( client.recipe.planes.size() ); }


EMSCRIPTEN_KEEPALIVE int pvue_client_put ( uint32_t level, uint32_t tile, const uint8_t *data, size_t size )
{
	auto error = pyra_error{};
	return report( pyra_put_encoded_tile( client.wsi, level, tile, data, size, &error ), error );
}


// Queues up to `capacity` tiles the last renders needed; returns how many.
// pvue_client_missing() points at them as (level, tile) uint32 pairs.
EMSCRIPTEN_KEEPALIVE int pvue_client_take_missing ( uint32_t capacity )
{
	client.missing.resize( capacity );
	auto count = size_t{ 0 };
	auto error = pyra_error{};
	const auto status = pyra_take_missing_tiles( client.wsi, client.missing.data(), capacity, &count, &error );
	client.missing.resize( status == PYRA_OK ? count : 0 );
	return status == PYRA_OK ? int( count ) : -report( status, error );
}


EMSCRIPTEN_KEEPALIVE const pyra_tile_id *pvue_client_missing ()
{
	return client.missing.data();
}


// Renders a rectangle (microns) of one plane at `mpp` to RGBA8. Returns OK,
// INCOMPLETE (missing tiles are background and now queued) or an error.
EMSCRIPTEN_KEEPALIVE int pvue_client_render ( uint32_t plane, double x, double y, double width, double height,
	double mpp )
{
	auto error = std::string{};
	const auto status = pyra::pvue::render_plane( client.wsi, plane, view_rect( x, y, width, height, mpp ),
		&client.frame, &error );
	report( status == PYRA_INCOMPLETE ? PYRA_OK : status, error );
	return status;
}


// Renders the display view around `reference`: the composite of its R, G and B
// channels when the slide has them, otherwise the plane itself. Same results as
// pvue_client_render.
EMSCRIPTEN_KEEPALIVE int pvue_client_render_display ( uint32_t reference, double x, double y, double width,
	double height, double mpp )
{
	const auto recipe = pyra::pvue::display_recipe( client.planes, client.gallery, reference );
	auto error = std::string{};
	const auto status = pyra::pvue::render_display( client.wsi, recipe, view_rect( x, y, width, height, mpp ),
		&client.frame, &error );
	report( status == PYRA_INCOMPLETE ? PYRA_OK : status, error );
	return status;
}


// Decodes a frame the server rendered and encoded (/api/render?encoding=htj2k:
// display RGB8, losslessly) into the RGBA8 pixels pvue_client_pixels returns.
EMSCRIPTEN_KEEPALIVE int pvue_client_decode ( const uint8_t *data, size_t size )
{
	auto error = pyra_error{};
	auto width = uint32_t{ 0 };
	auto height = uint32_t{ 0 };
	auto status = pyra_decode_image( data, size, nullptr, &width, &height, &error );
	if (status != PYRA_OK)
		return report( status, error );
	const auto row = size_t( width ) * 3;
	client.samples.assign( row * height, 0 );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), PYRA_PIXEL_RGB8,
		client.samples.data(), client.samples.size(), row };
	status = pyra_decode_image( data, size, &buffer, &width, &height, &error );
	if (status != PYRA_OK)
		return report( status, error );
	auto layout = pyra_image_layout{};
	layout.struct_size = sizeof( layout );
	layout.pixel_format = PYRA_PIXEL_RGB8;
	layout.width = width;
	layout.height = height;
	layout.row_stride_bytes = row;
	layout.capacity_bytes = client.samples.size();
	client.frame.width = width;
	client.frame.height = height;
	client.frame.rgba.resize( size_t( width ) * height * 4 );
//	RGB8 at full range: the conversion copies the samples and sets alpha.
	pyra::pvue::display_rgba( client.samples.data(), layout, 255.0, client.frame.rgba.data() );
	return report( PYRA_OK, error );
}


EMSCRIPTEN_KEEPALIVE const unsigned char *pvue_client_pixels () { return client.frame.rgba.data(); }
EMSCRIPTEN_KEEPALIVE uint32_t pvue_client_width () { return client.frame.width; }
EMSCRIPTEN_KEEPALIVE uint32_t pvue_client_height () { return client.frame.height; }
EMSCRIPTEN_KEEPALIVE const char *pvue_client_error () { return client.error.c_str(); }


// The open slide's caches, as doubles: compressed bytes in use, budget, hits
// and misses, then the same for decoded, then the tiles each holds, compressed
// and decoded (pvue::cache_use). Null without an open slide.
EMSCRIPTEN_KEEPALIVE const double *pvue_client_cache ()
{
	if (!client.context)
		return nullptr;
	auto use = pyra::pvue::CacheUse{};
	if (!client.wsi || !pyra::pvue::cache_use( client.context, client.compressed_bytes, client.decoded_bytes, &use ))
		return nullptr;
	client.cache = { double( use.compressed_bytes ), double( use.compressed_cap ), double( use.compressed_hits ),
		double( use.compressed_misses ), double( use.decoded_bytes ), double( use.decoded_cap ), double( use.decoded_hits ),
		double( use.decoded_misses ), double( use.compressed_tiles ), double( use.decoded_tiles ) };
	return client.cache.data();
}



// Empties the open slide's caches named by `caches` (PYRA_CACHE_COMPRESSED |
// PYRA_CACHE_DECODED; pyra_clear_cache). Returns a pyra_status.
EMSCRIPTEN_KEEPALIVE int pvue_client_clear ( uint32_t caches )
{
	if (!client.wsi)
		return report( PYRA_INVALID_ARGUMENT, "No slide is open" );
	auto error = pyra_error{};
	const auto status = pyra_clear_cache( client.wsi, caches, nullptr, &error );
	return status == PYRA_OK ? status : report( status, error );
}

} // extern "C"
