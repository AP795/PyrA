// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// The browser client of the web viewer, compiled to WebAssembly: one remote
// ("sister") slide opened from the server's geometry, fed with the server's
// stored tiles and rendered here with the same library code and display
// conversion the server uses. Flat exports for client_worker.js; one slide at a
// time, used from one worker thread.

#include "../display_rgba.h"

#include <pyra/pyra.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Same raster bound as the server's renders (web_server.h kMaxRenderEdge).
constexpr uint64_t kMaxRenderEdge = 4096;
constexpr uint64_t kBytesPerMiB = uint64_t{ 1024 } * 1024;

struct Client {
	pyra_context *context = nullptr;
	pyra_wsi *wsi = nullptr;
	std::string error;
	std::vector<pyra_tile_id> missing;
	std::vector<unsigned char> samples;
	std::vector<unsigned char> rgba;
	uint32_t width = 0;
	uint32_t height = 0;
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
}


int report ( pyra_status status, const pyra_error &error )
{
	client.error = status == PYRA_OK ? std::string{} : std::string( error.message );
	return status;
}

} // namespace


extern "C" {

// Opens the slide a geometry blob describes, with `cache_mib` for each of the
// compressed and decoded tile caches and up to `threads` decode threads (the
// calling thread included; at most PYRA_CLIENT_MAX_THREADS, the module's pool).
// Returns a pyra_status.
EMSCRIPTEN_KEEPALIVE int pyra_client_open ( const uint8_t *geometry, size_t size, double cache_mib, uint32_t threads )
{
	close_slide();
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	options.decode_threads = std::clamp<uint32_t>( threads, 1, PYRA_CLIENT_MAX_THREADS );
	options.max_image_pixels = kMaxRenderEdge * kMaxRenderEdge;
	options.compressed_cache_bytes = uint64_t( std::max( 1.0, cache_mib ) ) * kBytesPerMiB;
	options.decoded_cache_bytes = options.compressed_cache_bytes;
	auto error = pyra_error{};
	auto status = pyra_context_create( &options, &client.context, &error );
	if (status == PYRA_OK)
		status = pyra_open_remote( client.context, geometry, size, &client.wsi, &error );
	if (status != PYRA_OK)
		close_slide();
	return report( status, error );
}


EMSCRIPTEN_KEEPALIVE void pyra_client_close ()
{
	close_slide();
}


EMSCRIPTEN_KEEPALIVE int pyra_client_put ( uint32_t level, uint32_t tile, const uint8_t *data, size_t size )
{
	auto error = pyra_error{};
	return report( pyra_put_encoded_tile( client.wsi, level, tile, data, size, &error ), error );
}


// Queues up to `capacity` tiles the last renders needed; returns how many.
// pyra_client_missing() points at them as (level, tile) uint32 pairs.
EMSCRIPTEN_KEEPALIVE int pyra_client_take_missing ( uint32_t capacity )
{
	client.missing.resize( capacity );
	auto count = size_t{ 0 };
	auto error = pyra_error{};
	const auto status = pyra_take_missing_tiles( client.wsi, client.missing.data(), capacity, &count, &error );
	client.missing.resize( status == PYRA_OK ? count : 0 );
	return status == PYRA_OK ? int( count ) : -report( status, error );
}


EMSCRIPTEN_KEEPALIVE const pyra_tile_id *pyra_client_missing ()
{
	return client.missing.data();
}


// Renders a rectangle (microns) of a plane at `mpp` to RGBA8. Returns OK,
// INCOMPLETE (missing tiles are background and now queued) or an error.
EMSCRIPTEN_KEEPALIVE int pyra_client_render ( uint32_t plane, double x, double y, double width, double height,
	double mpp )
{
	auto error = pyra_error{};
	auto info = pyra_plane_info{};
	info.struct_size = sizeof( info );
	auto status = pyra_get_plane_info( client.wsi, plane, &info, &error );
	if (status != PYRA_OK)
		return report( status, error );
	const auto request = pyra_image_request{ sizeof( pyra_image_request ), plane, { x, y, width, height }, mpp };
	auto layout = pyra_image_layout{};
	layout.struct_size = sizeof( layout );
	status = pyra_get_image_layout( client.wsi, &request, &layout, &error );
	if (status != PYRA_OK)
		return report( status, error );
// Reused, not cleared: pyra_get_image writes every pixel, missing areas included.
	if (client.samples.size() < layout.capacity_bytes)
		client.samples.resize( layout.capacity_bytes );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		client.samples.data(), client.samples.size(), layout.row_stride_bytes };
	status = pyra_get_image( client.wsi, &request, &buffer, &error );
	if (status != PYRA_OK && status != PYRA_INCOMPLETE)
		return report( status, error );
	client.width = layout.width;
	client.height = layout.height;
	client.rgba.resize( size_t( layout.width ) * layout.height * 4 );
	pyra::web::display_rgba( client.samples.data(), layout, info.max_sample_value, client.rgba.data() );
	return report( status, error );
}


// Decodes a frame the server rendered and encoded (/api/render?encoding=htj2k:
// display RGB8, losslessly) into the RGBA8 pixels pyra_client_pixels returns.
EMSCRIPTEN_KEEPALIVE int pyra_client_decode ( const uint8_t *data, size_t size )
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
	client.width = width;
	client.height = height;
	client.rgba.resize( size_t( width ) * height * 4 );
// RGB8 at full range: the conversion copies the samples and sets alpha.
	pyra::web::display_rgba( client.samples.data(), layout, 255.0, client.rgba.data() );
	return report( PYRA_OK, error );
}


EMSCRIPTEN_KEEPALIVE const unsigned char *pyra_client_pixels () { return client.rgba.data(); }
EMSCRIPTEN_KEEPALIVE uint32_t pyra_client_width () { return client.width; }
EMSCRIPTEN_KEEPALIVE uint32_t pyra_client_height () { return client.height; }
EMSCRIPTEN_KEEPALIVE const char *pyra_client_error () { return client.error.c_str(); }

} // extern "C"
