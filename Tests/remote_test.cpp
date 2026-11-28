// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Remote ("sister") slides: a client fed only the server's geometry blob and
// stored tile bytes must render exactly what the server renders.
//
// Usage: pyra_remote_test OUTPUT_DIR [SLIDE ...]
// Generated fixtures cover stored JPEG/HTJ2K/JPEG XL levels, synthetic coarse
// levels of three pixel types and a multi-plane z-stack; extra SLIDE arguments
// are real slides checked the same way.

#include "ptf_write.h"

#include <pyra/pyra.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kFixtureEdge = 2304;
constexpr auto kFixtureTile = 256;
constexpr auto kViewEdge = 640;
// A render pass can only reveal tiles of one more level than the last; a few
// passes cover a level fallback, far more means tiles never arrive.
constexpr auto kMaxRenderPasses = 8;
constexpr auto kMissingBatch = size_t{ 64 };
// Both sides' tile caches. A client puts tiles between renders, unpinned, so a
// view whose tiles exceed the cache can never complete; real slides with
// uncompressed CZI sub-blocks need more than the library default to hold one.
constexpr auto kCacheBytes = uint64_t{ 256 } << 20;
constexpr auto kSmallCacheBytes = uint64_t{ 1 } << 20;
// Smaller than one compressed slot: no tile can be cached.
constexpr auto kSlotlessCacheBytes = uint64_t{ 1 } << 10;

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


struct Context {
	pyra_context *value_ = nullptr;

	explicit Context ( uint64_t compressed_cache_bytes = kCacheBytes, pyra_cache_topology topology = PYRA_CACHE_ARENA ) {
		auto options = pyra_context_options{};
		pyra_context_options_init( &options );
		options.compressed_cache_bytes = compressed_cache_bytes;
		options.cache_topology = topology;
		options.decoded_cache_bytes = kCacheBytes;
		auto error = pyra_error{};
		require( pyra_context_create( &options, &value_, &error ) == PYRA_OK, error.message );
	}
	~Context () { pyra_context_destroy( value_, nullptr ); }
	Context ( const Context & ) = delete;
	Context &operator= ( const Context & ) = delete;
};


struct Slide {
	pyra_wsi *value_ = nullptr;

	Slide () = default;
	~Slide () { if (value_) pyra_close( value_, nullptr ); }
	Slide ( const Slide & ) = delete;
	Slide &operator= ( const Slide & ) = delete;
};


void collect ( void *user, const void *data, size_t size )
{
	auto *bytes = static_cast<std::vector<uint8_t> *>( user );
	const auto *first = static_cast<const uint8_t *>( data );
	bytes->assign( first, first + size );
}


std::vector<uint8_t> geometry_of ( pyra_wsi *wsi )
{
	auto bytes = std::vector<uint8_t>{};
	auto error = pyra_error{};
	require( pyra_get_geometry( wsi, collect, &bytes, &error ) == PYRA_OK, error.message );
	require( !bytes.empty(), "Empty geometry blob" );
	return bytes;
}


void open_native ( Context &context, const std::filesystem::path &path, Slide &slide )
{
	auto error = pyra_error{};
	require( pyra_open( context.value_, utf8( path ).c_str(), &slide.value_, &error ) == PYRA_OK,
		utf8( path ) + ": " + error.message );
}


void open_remote ( Context &context, const std::vector<uint8_t> &geometry, Slide &slide )
{
	auto error = pyra_error{};
	require( pyra_open_remote( context.value_, geometry.data(), geometry.size(), &slide.value_, &error ) == PYRA_OK,
		std::string( "Remote open: " ) + error.message );
}


pyra_plane_info plane_info ( pyra_wsi *wsi, uint32_t index )
{
	auto info = pyra_plane_info{};
	info.struct_size = sizeof( info );
	auto error = pyra_error{};
	require( pyra_get_plane_info( wsi, index, &info, &error ) == PYRA_OK, error.message );
	return info;
}


struct Frame {
	pyra_status status = PYRA_OK;
	std::vector<uint8_t> pixels;
};


Frame render ( pyra_wsi *wsi, const pyra_image_request &request )
{
	auto layout = pyra_image_layout{};
	layout.struct_size = sizeof( layout );
	auto error = pyra_error{};
	require( pyra_get_image_layout( wsi, &request, &layout, &error ) == PYRA_OK, error.message );
	auto frame = Frame{};
	frame.pixels.assign( layout.capacity_bytes, 0 );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
		frame.pixels.data(), frame.pixels.size(), layout.row_stride_bytes };
	frame.status = pyra_get_image( wsi, &request, &buffer, &error );
	require( frame.status == PYRA_OK || frame.status == PYRA_INCOMPLETE,
		"Render status " + std::to_string( frame.status ) + ": " + error.message );
	return frame;
}


struct Transfer {
	int passes = 0;
	size_t tiles = 0;
	size_t bytes = 0;
};


// Moves every tile the remote render asked for from server to client.
size_t serve_missing ( pyra_wsi *server, pyra_wsi *client, Transfer &transfer )
{
	auto moved = size_t{ 0 };
	auto ids = std::vector<pyra_tile_id>( kMissingBatch );
	auto error = pyra_error{};
	while (true) {
		auto count = size_t{ 0 };
		require( pyra_take_missing_tiles( client, ids.data(), ids.size(), &count, &error ) == PYRA_OK, error.message );
		if (count == 0)
			return moved;
		for (size_t i = 0; i < count; ++i) {
			auto bytes = std::vector<uint8_t>{};
			const auto tile = std::to_string( ids[i].level_index ) + "/" + std::to_string( ids[i].tile_index );
			require( pyra_read_encoded_tile( server, ids[i].level_index, ids[i].tile_index, collect, &bytes, &error ) == PYRA_OK,
				"Read tile " + tile + ": " + error.message );
			require( pyra_put_encoded_tile( client, ids[i].level_index, ids[i].tile_index, bytes.data(), bytes.size(), &error ) == PYRA_OK,
				"Put tile " + tile + ": " + error.message );
			transfer.bytes += bytes.size();
		}
		transfer.tiles += count;
		moved += count;
	}
}


// Renders on the client until no tile is missing, serving tiles in between.
Frame render_remote ( pyra_wsi *server, pyra_wsi *client, const pyra_image_request &request, Transfer &transfer )
{
	for (auto pass = 0; pass < kMaxRenderPasses; ++pass) {
		auto frame = render( client, request );
		++transfer.passes;
		const auto moved = serve_missing( server, client, transfer );
		if (moved == 0)
			return frame;
	}
	throw std::runtime_error( "Remote render did not complete: the client cache cannot hold the view's tiles" );
}


// Views spanning every level: whole plane, two intermediate zooms, native 1:1.
std::vector<pyra_image_request> views ( const pyra_plane_info &plane, uint32_t index )
{
	auto requests = std::vector<pyra_image_request>{};
	const auto &b = plane.bounds;
	const auto fit = std::max( b.width_um, b.height_um ) / kViewEdge;
	for (const auto mpp : { fit, fit / 3.3, fit / 9.7, plane.native_mpp }) {
		const auto clamped = std::max( mpp, plane.native_mpp );
		const auto extent = kViewEdge * clamped;
		auto request = pyra_image_request{ sizeof( pyra_image_request ), index, {}, clamped };
		request.rectangle = { b.x_um + b.width_um * 0.37 - extent / 2, b.y_um + b.height_um * 0.41 - extent / 2,
			extent, extent * 0.75 };
		requests.push_back( request );
	}
	return requests;
}


double elapsed_ms ( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
}


void test_remote_geometry_round_trip ( const std::filesystem::path &path )
{
	const auto start = std::chrono::steady_clock::now();
	Context server_context, client_context;
	Slide server, client;
	open_native( server_context, path, server );
	const auto geometry = geometry_of( server.value_ );
	open_remote( client_context, geometry, client );
	auto planes = uint32_t{ 0 }, remote_planes = uint32_t{ 0 };
	require( pyra_get_plane_count( server.value_, &planes, nullptr ) == PYRA_OK &&
		pyra_get_plane_count( client.value_, &remote_planes, nullptr ) == PYRA_OK && planes == remote_planes,
		"Remote plane count differs" );
	for (auto i = 0u; i < planes; ++i) {
		const auto a = plane_info( server.value_, i );
		const auto b = plane_info( client.value_, i );
		require( a.pixel_format == b.pixel_format && a.level_count == b.level_count &&
			a.native_mpp == b.native_mpp && a.max_sample_value == b.max_sample_value &&
			std::memcmp( &a.bounds, &b.bounds, sizeof( a.bounds ) ) == 0 &&
			std::string( a.channel ? a.channel : "" ) == std::string( b.channel ? b.channel : "" ) &&
			( a.z_um == b.z_um || ( std::isnan( a.z_um ) && std::isnan( b.z_um ) ) ),
			"Remote plane " + std::to_string( i ) + " differs" );
	}
	auto properties = uint32_t{ 0 }, remote_properties = uint32_t{ 0 };
	require( pyra_get_property_count( server.value_, &properties, nullptr ) == PYRA_OK &&
		pyra_get_property_count( client.value_, &remote_properties, nullptr ) == PYRA_OK &&
		properties == remote_properties, "Remote property count differs" );
	std::cout << "test_remote_geometry_round_trip " << path.filename().string() << ": PASS ("
		<< geometry.size() << " B blob, " << elapsed_ms( start ) << " ms)\n";
}


void test_remote_render_matches_native ( const std::filesystem::path &path )
{
	const auto start = std::chrono::steady_clock::now();
	Context server_context, client_context;
	Slide server, client;
	open_native( server_context, path, server );
	open_remote( client_context, geometry_of( server.value_ ), client );
	auto planes = uint32_t{ 0 };
	require( pyra_get_plane_count( server.value_, &planes, nullptr ) == PYRA_OK, "Plane count" );
	auto transfer = Transfer{};
	auto checked = 0;
	for (auto index = 0u; index < planes; ++index) {
		for (const auto &request : views( plane_info( server.value_, index ), index )) {
			const auto native = render( server.value_, request );
			const auto remote = render_remote( server.value_, client.value_, request, transfer );
			require( remote.status == native.status, "Remote render status differs from native" );
			require( remote.pixels == native.pixels, utf8( path.filename() ) + ": plane " + std::to_string( index ) +
				" mpp " + std::to_string( request.mpp ) + ": remote pixels differ from native" );
			++checked;
		}
	}
	std::cout << "test_remote_render_matches_native " << path.filename().string() << ": PASS ("
		<< checked << " views, " << transfer.tiles << " tiles, " << transfer.bytes << " B, "
		<< transfer.passes << " passes, " << elapsed_ms( start ) << " ms)\n";
}


void test_remote_missing_tile_incomplete ( const std::filesystem::path &path )
{
	const auto start = std::chrono::steady_clock::now();
	Context server_context, client_context;
	Slide server, client;
	open_native( server_context, path, server );
	open_remote( client_context, geometry_of( server.value_ ), client );
	const auto request = views( plane_info( server.value_, 0 ), 0 ).back();
	const auto first = render( client.value_, request );
	require( first.status == PYRA_INCOMPLETE, "A render without tiles was not INCOMPLETE" );
	require( std::all_of( first.pixels.begin(), first.pixels.end(), [] ( auto v ) { return v == 0; } ),
		"Missing tiles were not background" );
	auto ids = std::vector<pyra_tile_id>( kMissingBatch );
	auto count = size_t{ 0 };
	require( pyra_take_missing_tiles( client.value_, ids.data(), ids.size(), &count, nullptr ) == PYRA_OK && count > 0,
		"No missing tiles were reported" );
	auto again = size_t{ 0 };
	require( pyra_take_missing_tiles( client.value_, ids.data(), ids.size(), &again, nullptr ) == PYRA_OK && again == 0,
		"Missing tiles were reported twice" );
	std::cout << "test_remote_missing_tile_incomplete: PASS (" << count << " tiles, "
		<< elapsed_ms( start ) << " ms)\n";
}


void test_remote_put_rejects_oversize ( const std::filesystem::path &path )
{
	const auto start = std::chrono::steady_clock::now();
	Context server_context, client_context;
	Slide server, client;
	open_native( server_context, path, server );
	open_remote( client_context, geometry_of( server.value_ ), client );
	const auto huge = std::vector<uint8_t>( size_t{ 64 } << 20, 0xff );
	require( pyra_put_encoded_tile( client.value_, 0, 0, huge.data(), huge.size(), nullptr ) == PYRA_INVALID_ARGUMENT,
		"An oversized tile was accepted" );
	require( pyra_put_encoded_tile( server.value_, 0, 0, huge.data(), 1, nullptr ) == PYRA_UNSUPPORTED,
		"A tile was put into a file-backed slide" );
	auto bytes = std::vector<uint8_t>{};
	require( pyra_get_geometry( client.value_, collect, &bytes, nullptr ) == PYRA_UNSUPPORTED,
		"A remote slide served geometry" );
	std::cout << "test_remote_put_rejects_oversize: PASS (" << elapsed_ms( start ) << " ms)\n";
}


pyra_context_stats context_stats ( Context &context )
{
	auto stats = pyra_context_stats{};
	stats.struct_size = sizeof( stats );
	auto error = pyra_error{};
	require( pyra_context_get_stats( context.value_, &stats, &error ) == PYRA_OK, error.message );
	return stats;
}


// A served tile goes through the compressed cache: the first read loads it into
// a slot (a miss), the next is copied from that slot (a hit), same bytes. The
// cache is small so that tile 0, which opening read first to build the coarse
// levels, has been evicted by the tiles read after it; the cache is full, so
// its allocated bytes do not move and the second read's hit is the evidence.
void test_remote_served_tile_cached ( const std::filesystem::path &path )
{
	const auto start = std::chrono::steady_clock::now();
	Context context( kSmallCacheBytes );
	Slide server;
	open_native( context, path, server );
	const auto before = context_stats( context );
	auto first = std::vector<uint8_t>{};
	auto second = std::vector<uint8_t>{};
	auto error = pyra_error{};
	require( pyra_read_encoded_tile( server.value_, 0, 0, collect, &first, &error ) == PYRA_OK && !first.empty(),
		std::string( "First read: " ) + error.message );
	const auto loaded = context_stats( context );
	require( loaded.compressed_cache_misses == before.compressed_cache_misses + 1 &&
		loaded.compressed_cache_hits == before.compressed_cache_hits, "The first read was not a compressed-cache miss" );
	require( loaded.source_tile_reads == before.source_tile_reads + 1, "The first read did not read the file" );
	require( pyra_read_encoded_tile( server.value_, 0, 0, collect, &second, &error ) == PYRA_OK,
		std::string( "Second read: " ) + error.message );
	const auto again = context_stats( context );
	require( again.compressed_cache_hits == loaded.compressed_cache_hits + 1 &&
		again.compressed_cache_misses == loaded.compressed_cache_misses, "The second read was not a compressed-cache hit" );
	require( again.source_tile_reads == loaded.source_tile_reads, "The cached read read the file again" );
	require( second == first, "The cached tile differs from the one read from the file" );
	std::cout << "test_remote_served_tile_cached: PASS (" << first.size() << " bytes, "
		<< elapsed_ms( start ) << " ms)\n";
}


// A compressed budget smaller than one slot holds no tile: a served tile is read
// past the cache, still with the file's bytes, and every such read is counted
// as a bypass (and a miss) so a viewer can say the budget is too small.
void test_remote_served_tile_bypass_counted ( const std::filesystem::path &path )
{
	auto expected = std::vector<uint8_t>{};
	auto error = pyra_error{};
	{
		Context context;
		Slide server;
		open_native( context, path, server );
		require( pyra_read_encoded_tile( server.value_, 0, 0, collect, &expected, &error ) == PYRA_OK && !expected.empty(),
			std::string( "Cached read: " ) + error.message );
		require( context_stats( context ).compressed_cache_bypasses == 0, "A cache with room counted a bypass" );
	}
	const auto start = std::chrono::steady_clock::now();
	Context context( kSlotlessCacheBytes );
	Slide server;
	open_native( context, path, server );
	const auto before = context_stats( context );
	for (auto read = uint64_t{ 1 }; read <= 2; ++read) {
		auto bytes = std::vector<uint8_t>{};
		require( pyra_read_encoded_tile( server.value_, 0, 0, collect, &bytes, &error ) == PYRA_OK,
			std::string( "Bypass read: " ) + error.message );
		require( bytes == expected, "The tile read past the cache differs from the cached one" );
		const auto after = context_stats( context );
		require( after.compressed_cache_bypasses == before.compressed_cache_bypasses + read &&
			after.compressed_cache_misses == before.compressed_cache_misses + read &&
			after.compressed_cache_hits == before.compressed_cache_hits, "The read past the cache was not counted as a bypass" );
		require( after.source_tile_reads == before.source_tile_reads + read, "The read past the cache was not counted as a file read" );
	}
	std::cout << "test_remote_served_tile_bypass_counted: PASS (" << expected.size() << " bytes, "
		<< elapsed_ms( start ) << " ms)\n";
}


// Renders a native-resolution view of plane 0, filling both caches; a coarse
// view could come from a level synthesized at open, held outside the caches.
void fill_caches ( pyra_wsi *wsi )
{
	auto plane = pyra_plane_info{};
	plane.struct_size = sizeof( plane );
	auto error = pyra_error{};
	require( pyra_get_plane_info( wsi, 0, &plane, &error ) == PYRA_OK, error.message );
	require( render( wsi, views( plane, 0 ).back() ).status == PYRA_OK, "The cache-filling render was incomplete" );
}


void test_pyra_clear_cache_compressed_only ( const std::filesystem::path &path )
{
	Context context;
	Slide slide;
	open_native( context, path, slide );
	fill_caches( slide.value_ );
	const auto start = std::chrono::steady_clock::now();
	const auto filled = context_stats( context );
	require( filled.compressed_cache_bytes > 0 && filled.decoded_cache_bytes > 0, "The render left a cache empty" );
	auto freed = uint64_t{};
	auto error = pyra_error{};
	require( pyra_clear_cache( slide.value_, PYRA_CACHE_COMPRESSED, &freed, &error ) == PYRA_OK, error.message );
	const auto cleared = context_stats( context );
	require( freed == filled.compressed_cache_bytes && cleared.compressed_cache_bytes == 0,
		"The compressed cache was not emptied" );
	require( cleared.compressed_cache_hits == 0 && cleared.compressed_cache_misses == 0 &&
		cleared.compressed_cache_bypasses == 0, "The compressed lookup counters did not restart" );
	require( cleared.decoded_cache_bytes == filled.decoded_cache_bytes &&
		cleared.decoded_cache_hits == filled.decoded_cache_hits && cleared.decoded_cache_misses == filled.decoded_cache_misses,
		"Clearing the compressed cache touched the decoded one" );
	require( cleared.source_tile_reads == filled.source_tile_reads, "Clearing restarted the tiles read from the file" );
	auto bytes = std::vector<uint8_t>{};
	require( pyra_read_encoded_tile( slide.value_, 0, 0, collect, &bytes, &error ) == PYRA_OK, error.message );
	const auto reread = context_stats( context );
	require( reread.compressed_cache_misses == 1 && reread.compressed_cache_hits == 0 &&
		reread.source_tile_reads == filled.source_tile_reads + 1, "A tile read after clearing was not a miss" );
	std::cout << "test_pyra_clear_cache_compressed_only: PASS (" << freed << " bytes freed, "
		<< elapsed_ms( start ) << " ms)\n";
}


void test_pyra_clear_cache_decoded_only ( const std::filesystem::path &path )
{
	Context context;
	Slide slide;
	open_native( context, path, slide );
	fill_caches( slide.value_ );
	const auto start = std::chrono::steady_clock::now();
	const auto filled = context_stats( context );
	auto error = pyra_error{};
	require( pyra_clear_cache( slide.value_, PYRA_CACHE_DECODED, nullptr, &error ) == PYRA_OK, error.message );
	const auto cleared = context_stats( context );
	require( cleared.decoded_cache_bytes == 0 && cleared.decoded_cache_hits == 0 && cleared.decoded_cache_misses == 0,
		"The decoded cache was not emptied" );
	require( cleared.compressed_cache_bytes == filled.compressed_cache_bytes &&
		cleared.compressed_cache_hits == filled.compressed_cache_hits, "Clearing the decoded cache touched the compressed one" );
	fill_caches( slide.value_ );
	const auto refilled = context_stats( context );
	require( refilled.decoded_cache_misses > 0 && refilled.compressed_cache_hits > filled.compressed_cache_hits &&
		refilled.source_tile_reads == filled.source_tile_reads,
		std::format( "The render after clearing did not decode again from the compressed cache: decoded misses {}, "
			"compressed hits {} -> {}, misses {} -> {}, tiles read {} -> {}", refilled.decoded_cache_misses,
			filled.compressed_cache_hits, refilled.compressed_cache_hits, filled.compressed_cache_misses,
			refilled.compressed_cache_misses, filled.source_tile_reads, refilled.source_tile_reads ) );
	std::cout << "test_pyra_clear_cache_decoded_only: PASS (" << elapsed_ms( start ) << " ms)\n";
}


// A compressed entry is exactly its tile: the used bytes are the bytes of the
// tiles held, and nothing is allocated ahead of them, whatever the topology.
void test_pyra_cache_used_bytes_exact_tiles ( const std::filesystem::path &path )
{
	for (const auto topology : { PYRA_CACHE_ARENA, PYRA_CACHE_PER_LEVEL }) {
		Context context( kCacheBytes, topology );
		Slide slide;
		open_native( context, path, slide );
		auto error = pyra_error{};
		require( pyra_clear_cache( slide.value_, PYRA_CACHE_COMPRESSED, nullptr, &error ) == PYRA_OK, error.message );
		const auto start = std::chrono::steady_clock::now();
		auto expected = uint64_t{};
		for (auto tile = 0u; tile < 2; ++tile) {
			auto bytes = std::vector<uint8_t>{};
			require( pyra_read_encoded_tile( slide.value_, 0, tile, collect, &bytes, &error ) == PYRA_OK, error.message );
			expected += bytes.size();
			const auto stats = context_stats( context );
			require( stats.compressed_cache_tiles == tile + 1, std::format( "The cache holds {} tiles after {} reads",
				stats.compressed_cache_tiles, tile + 1 ) );
			require( stats.compressed_cache_used_bytes == expected && stats.compressed_cache_bytes == expected,
				std::format( "Compressed bytes are not the tiles' own: {} used, {} allocated, {} read",
					stats.compressed_cache_used_bytes, stats.compressed_cache_bytes, expected ) );
		}
		std::cout << "test_pyra_cache_used_bytes_exact_tiles: PASS (" << ( topology == PYRA_CACHE_ARENA ? "arena" : "level" )
			<< ", " << expected << " bytes for 2 tiles, " << elapsed_ms( start ) << " ms)\n";
	}
}


// Decoded slots are fixed: the used bytes are the used slots' share of the
// allocated storage.
void test_pyra_cache_used_bytes_decoded_slots ( const std::filesystem::path &path )
{
	Context context;
	Slide slide;
	open_native( context, path, slide );
	fill_caches( slide.value_ );
	const auto start = std::chrono::steady_clock::now();
	const auto stats = context_stats( context );
	auto usage = pyra_cache_usage{};
	usage.struct_size = sizeof( usage );
	auto error = pyra_error{};
	require( pyra_get_cache_usage( slide.value_, -1, &usage, &error ) == PYRA_OK, error.message );
	require( usage.decoded_allocated_slots > 0 && stats.decoded_cache_used_bytes ==
		stats.decoded_cache_bytes / usage.decoded_allocated_slots * usage.decoded_used_slots,
		"Arena decoded used bytes are not the used slots' share" );
	require( stats.compressed_cache_used_bytes == stats.compressed_cache_bytes,
		"Compressed storage holds bytes beyond its tiles" );
	std::cout << "test_pyra_cache_used_bytes_decoded_slots: PASS (" << elapsed_ms( start ) << " ms)\n";
}


void test_pyra_clear_cache_rejects_no_cache ( const std::filesystem::path &path )
{
	Context context;
	Slide slide;
	open_native( context, path, slide );
	const auto start = std::chrono::steady_clock::now();
	auto error = pyra_error{};
	require( pyra_clear_cache( slide.value_, 0, nullptr, &error ) != PYRA_OK &&
		pyra_clear_cache( slide.value_, 4, nullptr, &error ) != PYRA_OK, "A clear naming no known cache was accepted" );
	std::cout << "test_pyra_clear_cache_rejects_no_cache: PASS (" << elapsed_ms( start ) << " ms)\n";
}


// A deterministic pattern with detail at every scale, so a misplaced or
// misdecoded tile changes pixels.
template <typename pixeltype>
pixeltype pattern ( int x, int y )
{
	const auto v = ( x * 7 + y * 13 + ( ( x >> 4 ) ^ ( y >> 4 ) ) * 29 + ( x * y >> 9 ) ) & 0xffff;
	if constexpr (std::is_same_v<pixeltype, priml::Rgb8>)
		return { uint8_t( v ), uint8_t( v >> 3 ), uint8_t( x ^ y ) };
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return uint16_t( v & 0x0fff );
	else
		return float( v ) / 65535.0f;
}


// One stored level only, large enough that the reader adds synthetic coarse
// levels, which the server must offer as virtual tiles.
template <typename pixeltype>
std::filesystem::path write_single_level ( const std::filesystem::path &directory, const char *name,
	priml::LevelPixelType type, priml::ImgCodec codec, int quality )
{
	const auto path = directory / name;
	auto error = std::string{};
	auto writer = priml::PtfWriter{};
	require( writer.open( utf8( path ), &error ), "PTF open: " + error );
	auto spec = priml::PtfLevelSpec{};
	spec.tags.valid = true;
	spec.tags.mpp = 0.5;
	spec.tags.pixel_type = type;
	spec.tags.max_pixel_value = type == priml::LevelPixelType::Gray16 ? 4095 : 0;
	spec.size = { kFixtureEdge, kFixtureEdge - 300 };
	spec.tile = { kFixtureTile, kFixtureTile };
	spec.codec = codec;
	spec.encode.quality = quality;
	auto fetch = priml::PtfTileFetch<pixeltype>(
		[] ( const priml::Rtlbr<int> &rect, priml::Pwhl<pixeltype> *tile ) {
			for (auto y = 0; y < tile->h; ++y)
				for (auto x = 0; x < tile->w; ++x)
					tile->rowp( y )[x] = pattern<pixeltype>( rect.tl.x + x, rect.tl.y + y );
			return true;
		} );
	require( writer.add_level( spec, fetch, &error ), "PTF write: " + error );
	require( writer.close( &error ), "PTF close: " + error );
	return path;
}


std::vector<std::filesystem::path> fixtures ( const std::filesystem::path &directory )
{
	std::filesystem::create_directories( directory );
	auto paths = std::vector<std::filesystem::path>{};
	paths.push_back( write_single_level<priml::Rgb8>( directory, "remote_rgb_jpeg.ptf",
		priml::LevelPixelType::Rgb8, priml::ImgCodec::Jpeg, priml::kImageDefaultLossyQuality ) );
	if (pyra_codec_can_encode( PYRA_CODEC_HTJ2K ))
		paths.push_back( write_single_level<uint16_t>( directory, "remote_gray16_htj2k.ptf",
			priml::LevelPixelType::Gray16, priml::ImgCodec::Htj2k, priml::kImageLosslessQuality ) );
	if (pyra_codec_can_encode( PYRA_CODEC_JPEGXL ))
		paths.push_back( write_single_level<float>( directory, "remote_float_jxl.ptf",
			priml::LevelPixelType::Float32, priml::ImgCodec::Jxl, priml::kImageLosslessQuality ) );
	auto options = pyra_synthetic_options{};
	pyra_synthetic_options_init( &options );
	options.width = 3072;
	options.height = 2048;
	options.z_levels = 2;
	const auto stack = directory / "remote_zstack.ptf";
	auto error = pyra_error{};
	require( pyra_make_synthetic( &options, utf8( stack ).c_str(), &error ) == PYRA_OK, error.message );
	paths.push_back( stack );
	return paths;
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc < 2)
		return 2;
	try {
		auto slides = fixtures( std::filesystem::path( argv[1] ) );
		test_remote_missing_tile_incomplete( slides.front() );
		test_remote_put_rejects_oversize( slides.front() );
		test_remote_served_tile_cached( slides.front() );
		test_remote_served_tile_bypass_counted( slides.front() );
		test_pyra_clear_cache_compressed_only( slides.front() );
		test_pyra_clear_cache_decoded_only( slides.front() );
		test_pyra_clear_cache_rejects_no_cache( slides.front() );
		test_pyra_cache_used_bytes_exact_tiles( slides.front() );
		test_pyra_cache_used_bytes_decoded_slots( slides.front() );
		for (auto i = 2; i < argc; ++i)
			slides.push_back( std::filesystem::path( argv[i] ) );
		for (const auto &slide : slides) {
			test_remote_geometry_round_trip( slide );
			test_remote_render_matches_native( slide );
		}
		std::cout << "Remote slides render as their server: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
