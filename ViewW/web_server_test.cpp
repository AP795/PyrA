// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "web_server.h"

#if PYRA_WEB_WITH_BRIDGE
#include "chunk_process.h"
#include "dcm_series.h"
#include "dcm_write.h"
#include "pyra_subprocess.h"
#endif

#include <httplib.h>
#include <pyra/pyra.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require ( bool condition, const char *message )
{
	if (!condition) throw std::runtime_error( message );
}


struct RunningServer {
	pyra::web::Server server_;
	int port_;
	std::thread thread_;
	explicit RunningServer ( const std::filesystem::path &root ) : RunningServer( pyra::web::ServerOptions{ root, {}, {} } ) {}
	explicit RunningServer ( pyra::web::ServerOptions options ) : server_( std::move( options ) ), port_( server_.bind( 0 ) ) {
		require( port_ > 0, "Cannot bind test server" );
		thread_ = std::thread( [this] { server_.listen(); } );
	}
	~RunningServer () { server_.stop(); if (thread_.joinable()) thread_.join(); }
};


uint64_t generation ( const std::string &body )
{
	const auto marker = std::string( "\"generation\":" );
	const auto position = body.find( marker );
	require( position != std::string::npos, "Open response has no generation" );
	auto result = uint64_t{};
	const auto parsed = std::from_chars( body.data() + position + marker.size(), body.data() + body.size(), result );
	require( parsed.ec == std::errc{}, "Invalid response generation" );
	return result;
}


std::string wait_for_generation ( httplib::Client &client, const httplib::Headers &headers )
{
	for (auto attempt = 0; attempt < 300; ++attempt) {
		const auto status = client.Get( "/api/generate", headers );
		require( status && status->status == 200, "Generation status failed" );
		if (status->body.find( "\"running\":false" ) != std::string::npos)
			return status->body;
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}
	throw std::runtime_error( "Generation did not finish within 30 seconds" );
}


#if PYRA_WEB_WITH_BRIDGE && USE_DCMTK

bool python_with_numpy ()
{
	auto process = priml::Subprocess{};
	auto spec = priml::SubprocessSpec{};
	spec.executable = priml::default_bridge_python();
	spec.arguments = { "-c", "import numpy" };
	auto error = std::string{};
	if (!process.start( spec, &error ))
		return false;
	process.close_input();
	return process.wait() == 0;
}


// A small RGB DICOM slide, laid out as PyrA publishes one: slide.dcm and _slide_/.
void make_dicom_slide ( const std::filesystem::path &directory )
{
	std::filesystem::remove_all( directory );
	std::filesystem::create_directories( directory );
	auto meta = priml::DcmAcquisition{};
	meta.uid_root = "1.2.826.0.1.3680043.10.987";
	meta.name = "slide";
	meta.acquisition_datetime = "20261122101500+0000";
	meta.focus_method = "AUTO";
	meta.imaged_depth_um = 1.5;
	meta.illumination_color = "WHITE";
	meta.manufacturer = "Test scanner";
	meta.model = "Synthetic";
	meta.serial = "T1";
	meta.software = "1";
	auto writer = priml::DcmWriter{};
	auto error = std::string{};
	require( writer.open( directory / "stage", meta, &error ), "DICOM fixture writer did not open" );
	auto spec = priml::DcmLevelSpec{};
	spec.pixel_type = priml::LevelPixelType::Rgb8;
	spec.max_pixel_value = 255;
	spec.mpp = 0.5;
	spec.size = { 600, 400 };
	spec.tile = { 128, 128 };
	spec.codec = priml::kBridgeDefaultCodec;
	spec.encode.quality = priml::kImageLosslessQuality;
	const auto fetch = priml::DcmTileFetch<priml::Rgb>( [] ( const auto &rect, auto *out, std::string * ) {
		for (auto y = rect.tl.y; y < rect.br.y; ++y)
			for (auto x = rect.tl.x; x < rect.br.x; ++x)
				out->rowp( y - rect.tl.y )[x - rect.tl.x] = { (uint8_t)( x % 256 ), (uint8_t)( y % 256 ), 90 };
		return priml::DcmFetchResult::Pixels;
	} );
	require( writer.add_level<priml::Rgb>( spec, fetch, &error ) && writer.close( &error ), "DICOM fixture was not written" );
	auto stats = writer.stats();
	require( priml::publish_dcm_pyramid( directory / "slide.dcm", &stats, &error ), "DICOM fixture was not published" );
	std::filesystem::remove_all( directory / "stage" );
}


// Polls GET /api/analyse until the run ends; returns the final status body.
std::string wait_for_analysis ( httplib::Client &client, const httplib::Headers &headers )
{
	for (auto attempt = 0; attempt < 1200; ++attempt) {
		const auto status = client.Get( "/api/analyse", headers );
		require( status && status->status == 200, "Analysis status failed" );
		if (status->body.find( "\"running\":false" ) != std::string::npos)
			return status->body;
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}
	throw std::runtime_error( "Analysis did not finish within two minutes" );
}


httplib::Params analyse_params ( const char *mode, const char *process, const char *channel )
{
	return { { "mode", mode }, { "process", process }, { "prepare", "" }, { "channel", channel },
		{ "type", "gray8" }, { "halo", "0" }, { "workers", "2" }, { "params", "gain=1\n" } };
}

#endif


std::string view_url ( uint64_t version, const std::string &extra = "" )
{
	return "/api/render?generation=" + std::to_string( version ) + "&plane=0&x=0&y=0&width=3&height=2&mpp=1" + extra;
}


uint64_t open ( httplib::Client &client, const httplib::Headers &headers, const char *path )
{
	const auto response = client.Post( "/api/open", headers, httplib::Params{ { "path", path } } );
	require( response && response->status == 200, response ? response->body.c_str() : "Open HTTP request failed" );
	return generation( response->body );
}


// "name=value" of a Set-Cookie header, without its attributes; empty if absent.
std::string set_cookie ( const httplib::Result &response, const std::string &name )
{
	for (auto [it, end] = response->headers.equal_range( "Set-Cookie" ); it != end; ++it) {
		if (it->second.starts_with( name + "=" ))
			return it->second.substr( 0, it->second.find( ';' ) );
	}
	return {};
}


// A browser's API headers: loads the page (with `base`, e.g. a proxy's Host and
// an access cookie) and returns its session cookie and page token on top of `base`.
httplib::Headers connect ( httplib::Client &client, httplib::Headers base = {} )
{
	const auto page = client.Get( "/", base );
	require( page && page->status == 200, page ? page->body.c_str() : "Page request failed" );
	const auto cookie = set_cookie( page, "pyra_sid" );
	require( !cookie.empty(), "The page set no session cookie" );
	const auto marker = std::string( "name=\"pyra-session\" content=\"" );
	const auto start = page->body.find( marker );
	require( start != std::string::npos, "The page carries no session token" );
	const auto token = page->body.substr( start + marker.size(), page->body.find( '"', start + marker.size() ) -
		start - marker.size() );
	auto cookies = cookie;
	if (const auto found = base.find( "Cookie" ); found != base.end()) {
		cookies = found->second + "; " + cookie;
		base.erase( found );
	}
	base.emplace( "Cookie", cookies );
	base.emplace( "X-Pyra-Session", token );
	return base;
}


// A server behind a proxy at `origins`; returns the proxy-side headers of a
// browser that has opened the access URL.
httplib::Headers proxied_access ( httplib::Client &client, const pyra::web::Server &server, const std::string &host )
{
	const auto forwarded = httplib::Headers{ { "Host", host }, { "X-Forwarded-For", "192.0.2.7" } };
	const auto admitted = client.Get( "/?key=" + server.access_key(), forwarded );
	require( admitted && admitted->status == 302, "The access key was not accepted" );
	const auto access = set_cookie( admitted, "pyra_access" );
	require( !access.empty(), "The access key gave no access cookie" );
	auto headers = forwarded;
	headers.emplace( "Cookie", access );
	return headers;
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2) return 2;
	try {
		auto host = RunningServer( pyra::web::utf8_path( argv[1] ) );
		auto client = httplib::Client( "127.0.0.1", host.port_ );
		client.set_connection_timeout( 5, 0 );
		client.set_read_timeout( 15, 0 );
		const auto headers = connect( client );
		auto failed = 0;
		const auto run = [&] ( const char *name, auto test ) {
			const auto start = std::chrono::steady_clock::now();
			auto passed = true;
			try { test(); }
			catch (const std::exception &error) {
				std::cerr << name << ": " << error.what() << '\n';
				passed = false; ++failed;
			}
			std::cout << name << ": " << ( passed ? "PASS" : "FAIL" ) << " ("
				<< std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - start ).count() << " us)\n";
		};
		run( "test_web_embedded_assets", [&] {
			for (const auto *path : { "/", "/viewer.css", "/viewer.js", "/pvue_view.js" }) {
				const auto response = client.Get( path );
				require( response && response->status == 200 && !response->body.empty(), "Embedded asset missing" );
			}
		} );
		run( "test_web_api_requires_session", [&] {
			const auto response = client.Get( "/api/state" );
			require( response && response->status == 403, "Unauthenticated local API accepted" );
		} );
		run( "test_web_rejects_foreign_origin", [&] {
			auto foreign = headers; foreign.emplace( "Origin", "http://example.invalid" );
			const auto response = client.Get( "/api/state", foreign );
			require( response && response->status == 403, "Foreign-origin request accepted" );
		} );
		run( "test_web_rejects_foreign_host", [&] {
			auto foreign = headers; foreign.emplace( "Host", "example.invalid" );
			const auto response = client.Get( "/api/state", foreign );
			require( response && response->status == 403, "Foreign-host request accepted" );
		} );
		run( "test_web_rejects_parent_directory", [&] {
			const auto response = client.Get( "/api/files?path=..", headers );
			require( response && response->status == 400, "Directory traversal accepted" );
		} );
		run( "test_web_native_rgb8_samples", [&] {
			const auto version = open( client, headers, "images/rgb8/rgb8.glr" );
			const auto response = client.Get( view_url( version ), headers );
			require( response && response->status == 200 && response->body.size() == 3 * 2 * 4, "RGB8 frame layout differs" );
			for (auto y = 0; y < 2; ++y) for (auto x = 0; x < 3; ++x) for (auto c = 0; c < 4; ++c) {
				const auto expected = c == 3 ? 255 : ( x * 137 + y * 511 + c * 10003 ) % 256;
				require( static_cast<unsigned char>( response->body[( y * 3 + x ) * 4 + c] ) == expected, "RGB8 display sample differs" );
			}
			require( response->has_header( "X-Pyra-Render-Ms" ), "Native timing missing" );
		} );
		run( "test_web_native_rgb16_display_range", [&] {
			const auto version = open( client, headers, "images/rgb16/rgb16.glr" );
			const auto response = client.Get( view_url( version ), headers );
			require( response && response->status == 200 && response->body.size() == 24, "RGB16 display failed" );
			for (auto c = 0; c < 3; ++c)
				require( static_cast<unsigned char>( response->body[c] ) == uint8_t( c * 10003 * 255.0 / 65535.0 ), "RGB16 range differs" );
		} );
		run( "test_web_overview_is_bounded", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto response = client.Get( "/api/render?generation=" + std::to_string( version ) + "&plane=0&overview=1", headers );
			require( response && response->status == 200 && response->body.size() <= pyra::web::kOverviewEdge * pyra::web::kOverviewEdge * 4,
				"Overview exceeds bound" );
		} );
		run( "test_web_raw_tile_bytes_equal_encoded_tile", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto query = "?generation=" + std::to_string( version );
			const auto geometry = client.Get( "/api/geometry" + query, headers );
			require( geometry && geometry->status == 200 && !geometry->body.empty(), "Geometry was not served" );
			require( geometry->get_header_value( "Cache-Control" ).find( "immutable" ) != std::string::npos,
				"Geometry is not marked immutable" );
			auto *context = static_cast<pyra_context *>( nullptr );
			auto *remote = static_cast<pyra_wsi *>( nullptr );
			require( pyra_context_create( nullptr, &context, nullptr ) == PYRA_OK &&
				pyra_open_remote( context, geometry->body.data(), geometry->body.size(), &remote, nullptr ) == PYRA_OK,
				"Served geometry does not open as a remote slide" );
			auto *native = static_cast<pyra_wsi *>( nullptr );
			const auto path = pyra::web::utf8_path( argv[1] ) / "coarse.ptf";
			const auto utf8 = path.u8string();
			require( pyra_open( context, std::string( utf8.begin(), utf8.end() ).c_str(), &native, nullptr ) == PYRA_OK,
				"Fixture does not open natively" );
			auto expected = std::string{};
			const auto sink = [] ( void *user, const void *data, size_t size ) {
				static_cast<std::string *>( user )->assign( static_cast<const char *>( data ), size );
			};
			require( pyra_read_encoded_tile( native, 0, 0, sink, &expected, nullptr ) == PYRA_OK, "Native tile read failed" );
			const auto tile = client.Get( "/api/raw-tile" + query + "&level=0&tile=0", headers );
			require( tile && tile->status == 200 && tile->body == expected && !expected.empty(),
				"Served tile differs from the stored bytes" );
			require( pyra_put_encoded_tile( remote, 0, 0, tile->body.data(), tile->body.size(), nullptr ) == PYRA_OK,
				"Served tile was refused by the remote slide" );
			pyra_close( native, nullptr );
			pyra_close( remote, nullptr );
			pyra_context_destroy( context, nullptr );
		} );
		run( "test_web_server_sends_channel_tiles_unchanged", [&] {
			const auto version = open( client, headers, "channels_rgb.ptf" );
			auto *context = static_cast<pyra_context *>( nullptr );
			auto *native = static_cast<pyra_wsi *>( nullptr );
			const auto utf8 = ( pyra::web::utf8_path( argv[1] ) / "channels_rgb.ptf" ).u8string();
			require( pyra_context_create( nullptr, &context, nullptr ) == PYRA_OK &&
				pyra_open( context, std::string( utf8.begin(), utf8.end() ).c_str(), &native, nullptr ) == PYRA_OK,
				"Channel fixture does not open natively" );
			const auto sink = [] ( void *user, const void *data, size_t size ) {
				static_cast<std::string *>( user )->assign( static_cast<const char *>( data ), size );
			};
			auto compared = 0;
			for (auto level = 0u; ; ++level) {
				auto expected = std::string{};
				if (pyra_read_encoded_tile( native, level, 0, sink, &expected, nullptr ) != PYRA_OK)
					break;
				for (auto tile = 0u; pyra_read_encoded_tile( native, level, tile, sink, &expected, nullptr ) == PYRA_OK; ++tile) {
					const auto served = client.Get( std::format( "/api/raw-tile?generation={}&level={}&tile={}",
						version, level, tile ), headers );
					require( served && served->status == 200 && served->body == expected,
						std::format( "Level {} tile {} was not sent as stored", level, tile ).c_str() );
					++compared;
				}
			}
			pyra_close( native, nullptr );
			pyra_context_destroy( context, nullptr );
			require( compared >= 4 * 20, std::format( "Only {} channel tiles were compared", compared ).c_str() );
		} );
		run( "test_web_render_htj2k_is_lossless", [&] {
			const auto version = open( client, headers, "images/rgb8/rgb8.glr" );
			const auto raw = client.Get( view_url( version ), headers );
			const auto compressed = client.Get( view_url( version, "&encoding=htj2k" ), headers );
			require( raw && raw->status == 200 && compressed && compressed->status == 200, "Render failed" );
			if (!pyra_codec_can_encode( PYRA_CODEC_HTJ2K )) {
				require( compressed->get_header_value( "X-Pyra-Encoding" ) == "raw", "Unencodable frame not sent raw" );
				return;
			}
			require( compressed->get_header_value( "X-Pyra-Encoding" ) == "htj2k", "Frame was not HTJ2K" );
			auto w = 0u, h = 0u;
			auto rgb = std::string( raw->body.size() / 4 * 3, '\0' );
			const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), PYRA_PIXEL_RGB8, rgb.data(), rgb.size(),
				3 * 3 };
			require( pyra_decode_image( compressed->body.data(), compressed->body.size(), &buffer, &w, &h, nullptr ) ==
				PYRA_OK && w == 3 && h == 2, "HTJ2K frame did not decode to the view" );
			for (size_t i = 0, o = 0; i < raw->body.size(); i += 4, o += 3)
				require( raw->body.compare( i, 3, rgb, o, 3 ) == 0, "HTJ2K frame differs from the raw frame" );
			const auto refused = client.Get( view_url( version, "&encoding=png" ), headers );
			require( refused && refused->status == 400, "An unknown encoding was accepted" );
		} );
		run( "test_web_cross_origin_isolated", [&] {
			const auto page = client.Get( "/" );
			require( page && page->get_header_value( "Cross-Origin-Opener-Policy" ) == "same-origin" &&
				page->get_header_value( "Cross-Origin-Embedder-Policy" ) == "require-corp",
				"The page is not cross-origin isolated" );
		} );
		run( "test_web_raw_tile_concurrent_requests", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto url = "/api/raw-tile?generation=" + std::to_string( version ) + "&level=0&tile=0";
			constexpr auto kClients = 8;
			auto ok = std::array<bool, kClients>{};
			auto threads = std::vector<std::thread>{};
			for (auto i = 0; i < kClients; ++i)
				threads.emplace_back( [&, i] {
					auto own = httplib::Client( "127.0.0.1", host.port_ );
					own.set_read_timeout( 15, 0 );
					const auto response = own.Get( url, headers );
					ok[i] = response && response->status == 200 && !response->body.empty();
				} );
			for (auto &thread : threads) thread.join();
			require( std::all_of( ok.begin(), ok.end(), [] ( bool v ) { return v; } ),
				"A concurrent tile request was refused" );
		} );
		run( "test_web_cache_counts_tiles_read_and_served", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto url = "/api/raw-tile?generation=" + std::to_string( version ) + "&level=0&tile=0";
			for (auto i = 0; i < 2; ++i) {
				const auto tile = client.Get( url, headers );
				require( tile && tile->status == 200, "Cannot read a raw tile" );
			}
			const auto usage = client.Get( "/api/cache", headers );
			require( usage && usage->status == 200 && usage->body.find( "\"tiles\":{\"read\":" ) != std::string::npos &&
				usage->body.find( ",\"served\":2,\"served_bytes\":" ) != std::string::npos,
				( "Cache usage does not count the tiles read and served: " + usage->body ).c_str() );
		} );
		run( "test_web_cache_counts_compressed_hits", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto lookups = [&] {
				const auto usage = client.Get( "/api/cache", headers );
				require( usage && usage->status == 200, "Cannot read the cache usage" );
				const auto number = [&] ( const std::string &key ) {
					const auto at = usage->body.find( key );
					require( at != std::string::npos, ( "Cache usage lacks " + key + ": " + usage->body ).c_str() );
					return std::stoull( usage->body.substr( at + key.size() ) );
				};
				return std::array<uint64_t, 2>{ number( "\"hits\":" ), number( "\"misses\":" ) };
			};
			const auto before = lookups();
			const auto url = "/api/raw-tile?generation=" + std::to_string( version ) + "&level=0&tile=0";
			for (auto i = 0; i < 2; ++i) {
				const auto tile = client.Get( url, headers );
				require( tile && tile->status == 200, "Cannot read a raw tile" );
			}
			const auto after = lookups();
			require( after[0] > before[0] && after[1] >= before[1],
				std::format( "Serving a tile twice did not count a compressed hit: hits {} -> {}, misses {} -> {}",
					before[0], after[0], before[1], after[1] ).c_str() );
		} );
		run( "test_web_cache_clear_resets_compressed", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto url = "/api/raw-tile?generation=" + std::to_string( version ) + "&level=0&tile=0";
			for (auto i = 0; i < 2; ++i) {
				const auto tile = client.Get( url, headers );
				require( tile && tile->status == 200, "Cannot read a raw tile" );
			}
			const auto cleared = client.Post( "/api/cache/clear", headers, httplib::Params{ { "compressed", "1" } } );
			require( cleared && cleared->status == 200 && cleared->body.find( "{\"freed\":" ) == 0 &&
				cleared->body != "{\"freed\":0}", ( "The compressed cache was not cleared: " +
					( cleared ? cleared->body : std::string( "no response" ) ) ).c_str() );
			const auto empty = client.Get( "/api/cache", headers );
			require( empty && empty->status == 200 && empty->body.find( "\"compressed\":{\"bytes\":0," ) != std::string::npos &&
				empty->body.find( ",\"held\":0,\"hits\":0,\"misses\":0,\"bypassed\":0}" ) != std::string::npos,
				( "The cleared cache still reports content or lookups: " + empty->body ).c_str() );
			const auto again = client.Get( url, headers );
			require( again && again->status == 200, "Cannot read the tile after clearing" );
			const auto refilled = client.Get( "/api/cache", headers );
			require( refilled && refilled->body.find( ",\"held\":1,\"hits\":0,\"misses\":1,\"bypassed\":0}" ) != std::string::npos,
				( "The tile read after clearing was not a miss: " + refilled->body ).c_str() );
		} );
		run( "test_web_cache_clear_needs_choice_and_slide", [&] {
			auto fresh = RunningServer( pyra::web::utf8_path( argv[1] ) );
			auto own = httplib::Client( "127.0.0.1", fresh.port_ );
			const auto session = connect( own );
			const auto closed = own.Post( "/api/cache/clear", session, httplib::Params{ { "compressed", "1" } } );
			require( closed && closed->status == 409, "A clear without an open slide was not refused" );
			open( own, session, "coarse.ptf" );
			const auto none = own.Post( "/api/cache/clear", session, httplib::Params{} );
			require( none && none->status == 400, "A clear naming no cache was not refused" );
		} );
		run( "test_web_open_per_level_topology", [&] {
			const auto opened = client.Post( "/api/open", headers,
				httplib::Params{ { "path", "coarse.ptf" }, { "topology", "1" } } );
			require( opened && opened->status == 200, "Cannot open with the per-level cache layout" );
			const auto usage = client.Get( "/api/cache", headers );
			require( usage && usage->status == 200 && usage->body.find( ",\"topology\":1," ) != std::string::npos,
				( "The per-level layout is not reported: " + ( usage ? usage->body : std::string() ) ).c_str() );
			open( client, headers, "coarse.ptf" );
			const auto arena = client.Get( "/api/cache", headers );
			require( arena && arena->body.find( ",\"topology\":0," ) != std::string::npos,
				"An open without a layout did not use the arena" );
		} );
		run( "test_web_generation_unique_across_server_runs", [&] {
			auto generations = std::array<uint64_t, 2>{};
			for (auto &number : generations) {
				auto run = RunningServer( pyra::web::utf8_path( argv[1] ) );
				auto own = httplib::Client( "127.0.0.1", run.port_ );
				number = open( own, connect( own ), "coarse.ptf" );
			}
			require( generations[0] != generations[1] && generations[0] > 1,
				"A restarted server reissued an image number, so the browser would reuse the old run's tiles" );
			require( generations[1] < ( uint64_t{ 1 } << 53 ), "The image number is not exact as a JavaScript number" );
		} );
		run( "test_web_raw_tile_stale_generation", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto response = client.Get( "/api/raw-tile?generation=" + std::to_string( version + 1 ) +
				"&level=0&tile=0", headers );
			require( response && response->status == 410, "A stale tile request was not 410 Gone" );
		} );
		run( "test_web_state_sends_view_halo", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.view_halo_percent = 7;
			auto halo = RunningServer( std::move( options ) );
			auto halo_client = httplib::Client( "127.0.0.1", halo.port_ );
			const auto response = halo_client.Get( "/api/state", connect( halo_client ) );
			require( response && response->status == 200 && response->body.find( std::format(
				"\"view_halo\":7,\"view_halo_max\":{}", pyra::web::kViewHaloMaxPercent ) ) != std::string::npos,
				"State does not carry the configured view halo" );
		} );
		run( "test_web_state_sends_server_pbf", [&] {
			const auto response = client.Get( "/api/state", headers );
			require( response && response->status == 200 && response->body.find( std::format(
				"\"server_pbf_mib\":{},", pyra::web::kServerDecodedCacheMiB ) ) != std::string::npos,
				"State does not carry the server's PBF" );
		} );
		run( "test_web_public_origin_accepted", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.public_origins = { "https://viewer.example.org/" };
			auto proxied = RunningServer( std::move( options ) );
			auto proxy = httplib::Client( "127.0.0.1", proxied.port_ );
			auto forwarded = connect( proxy, proxied_access( proxy, proxied.server_, "viewer.example.org" ) );
			forwarded.emplace( "Origin", "https://viewer.example.org" );
			const auto accepted = proxy.Get( "/api/state", forwarded );
			require( accepted && accepted->status == 200, "A request from the public origin was refused" );
			forwarded.erase( "Origin" );
			forwarded.emplace( "Origin", "https://other.example.org" );
			const auto refused = proxy.Get( "/api/state", forwarded );
			require( refused && refused->status == 403, "A foreign origin was accepted through the proxy host" );
		} );
		run( "test_web_public_origin_requires_access_key", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.public_origins = { "https://viewer.example.org" };
			auto proxied = RunningServer( std::move( options ) );
			auto proxy = httplib::Client( "127.0.0.1", proxied.port_ );
			const auto forwarded = httplib::Headers{ { "Host", "viewer.example.org" }, { "X-Forwarded-For", "192.0.2.7" } };
			for (const auto *path : { "/", "/viewer.js", "/?key=0123" }) {
				const auto refused = proxy.Get( path, forwarded );
				require( refused && refused->status == 403, "A network browser without the access key was let in" );
			}
			auto stolen = forwarded;
			stolen.emplace( "Cookie", "pyra_access=" + proxied.server_.access_key() );
			const auto key_as_cookie = proxy.Get( "/", stolen );
			require( key_as_cookie && key_as_cookie->status == 403, "The access key itself served as the access cookie" );
			const auto admitted = proxied_access( proxy, proxied.server_, "viewer.example.org" );
			const auto page = proxy.Get( "/", admitted );
			require( page && page->status == 200, "The access cookie did not open the page" );
			auto secure = false;
			for (auto [it, end] = page->headers.equal_range( "Set-Cookie" ); it != end; ++it)
				secure = secure || it->second.find( "; Secure" ) != std::string::npos;
			require( secure, "The session cookie of an https origin is not Secure" );
		} );
		run( "test_web_forwarded_loopback_host_is_not_local", [&] {
			auto forwarded = headers;
			forwarded.emplace( "X-Forwarded-For", "192.0.2.7" );
			const auto refused = client.Get( "/api/state", forwarded );
			require( refused && refused->status == 403, "A proxied request naming the loopback host was taken as local" );
		} );
		run( "test_web_public_origin_multiple", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.public_origins = { "https://viewer.example.org", "https://192.0.2.10:8443" };
			auto proxied = RunningServer( std::move( options ) );
			auto proxy = httplib::Client( "127.0.0.1", proxied.port_ );
			for (const auto *host : { "viewer.example.org", "192.0.2.10:8443" }) {
				const auto response = proxy.Get( "/api/state", connect( proxy, proxied_access( proxy, proxied.server_, host ) ) );
				require( response && response->status == 200, "A configured public origin was refused" );
			}
		} );
		run( "test_web_remote_write_refused", [&] {
			for (const auto allowed : { false, true }) {
				auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
				options.public_origins = { "https://viewer.example.org" };
				options.allow_remote_write = allowed;
				auto proxied = RunningServer( std::move( options ) );
				auto proxy = httplib::Client( "127.0.0.1", proxied.port_ );
				const auto remote = connect( proxy, proxied_access( proxy, proxied.server_, "viewer.example.org" ) );
				const auto state = proxy.Get( "/api/state", remote );
				require( state && state->status == 200 && state->body.find( std::format( "\"local\":false,\"remote_write\":{}",
					allowed ) ) != std::string::npos, "State does not report the network client's write access" );
				const auto gallery = proxy.Post( "/api/make-gallery", remote, httplib::Params{ { "path", "images/gray8" } } );
				const auto generate = proxy.Post( "/api/generate", remote, httplib::Params{ { "path", "" } } );
				const auto cancel = proxy.Post( "/api/generate-cancel", remote, "", "text/plain" );
				const auto analyse = proxy.Post( "/api/analyse", remote, "", "text/plain" );
				for (const auto *response : { &gallery, &generate, &cancel, &analyse })
					require( *response && ( ( *response )->status == 403 ) == !allowed,
						allowed ? "An allowed network write was refused" : "A network client was allowed to write" );
				const auto local_state = proxy.Get( "/api/state", connect( proxy ) );
				require( local_state && local_state->body.find( "\"local\":true,\"remote_write\":true" ) != std::string::npos,
					"A loopback client may not write" );
			}
		} );
		run( "test_web_session_isolation", [&] {
			const auto second = connect( client );
			const auto first_version = open( client, headers, "coarse.ptf" );
			const auto second_version = open( client, second, "images/rgb8/rgb8.glr" );
			require( first_version != second_version, "Two sessions share a generation" );
			const auto first = client.Get( "/api/render?generation=" + std::to_string( first_version ) +
				"&plane=0&overview=1", headers );
			const auto mine = client.Get( view_url( second_version ), second );
			const auto crossed = client.Get( view_url( first_version ), second );
			require( first && first->status == 200 && mine && mine->status == 200 && mine->body.size() == 3 * 2 * 4,
				"A session lost its slide when another opened one" );
			require( crossed && crossed->status == 409, "A session rendered another session's slide" );
		} );
		run( "test_web_session_token_bound_to_cookie", [&] {
			const auto second = connect( client );
			auto mixed = headers;
			mixed.erase( "X-Pyra-Session" );
			mixed.emplace( "X-Pyra-Session", second.find( "X-Pyra-Session" )->second );
			const auto refused = client.Get( "/api/state", mixed );
			require( refused && refused->status == 403, "A token was accepted with another session's cookie" );
			auto cookie_only = headers;
			cookie_only.erase( "X-Pyra-Session" );
			const auto no_token = client.Get( "/api/state", cookie_only );
			require( no_token && no_token->status == 403, "A session cookie without its token was accepted" );
		} );
		run( "test_web_session_limit_refuses_when_busy", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.max_sessions = 1;
			auto limited = RunningServer( std::move( options ) );
			auto own = httplib::Client( "127.0.0.1", limited.port_ );
			const auto kept = connect( own );
			const auto refused = own.Get( "/" );
			require( refused && refused->status == 503, "A session beyond the limit was opened" );
			const auto same = own.Get( "/", kept );
			require( same && same->status == 200 && set_cookie( same, "pyra_sid" ).empty(),
				"A browser with a live session did not keep it" );
		} );
		run( "test_web_session_limit_evicts_idle", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.max_sessions = 1;
			options.session_idle_seconds = 0;
			auto limited = RunningServer( std::move( options ) );
			auto own = httplib::Client( "127.0.0.1", limited.port_ );
			const auto evicted = connect( own );
			const auto newer = connect( own );
			const auto gone = own.Get( "/api/state", evicted );
			const auto live = own.Get( "/api/state", newer );
			require( gone && gone->status == 403 && live && live->status == 200,
				"The idle session was not replaced by the new one" );
		} );
		run( "test_web_cache_reports_budgets", [&] {
			auto cached = RunningServer( pyra::web::utf8_path( argv[1] ) );
			auto cached_client = httplib::Client( "127.0.0.1", cached.port_ );
			const auto session = connect( cached_client );
			const auto closed = cached_client.Get( "/api/cache", session );
			require( closed && closed->status == 409, "Cache usage reported without an open slide" );
			const auto opened = cached_client.Post( "/api/open", session,
				httplib::Params{ { "path", "coarse.ptf" }, { "tbf_mib", "8" }, { "pbf_mib", "0" } } );
			require( opened && opened->status == 200, "Cannot open the cache fixture" );
			const auto version = generation( opened->body );
			const auto rendered = cached_client.Get( std::format( "/api/render?generation={}&plane=0&x=0&y=0&width=64&height=64&mpp=1",
				version ), session );
			require( rendered && rendered->status == 200, "Cannot render the cache fixture" );
			const auto usage = cached_client.Get( "/api/cache", session );
			require( usage && usage->status == 200 &&
				usage->body.find( std::format( ",\"cap\":{},\"held\":", 8 * 1024 * 1024 ) ) != std::string::npos &&
				usage->body.find( ",\"bypassed\":0},\"decoded\":{\"bytes\":0,\"cap\":0,\"held\":0,\"hits\":0,\"misses\":0}" ) !=
					std::string::npos && usage->body.find( "\"compressed\":{\"bytes\":0," ) == std::string::npos,
				( "Cache usage does not report the slide's budgets: " + usage->body ).c_str() );
			const auto reopened = cached_client.Post( "/api/open", session, httplib::Params{ { "path", "coarse.ptf" } } );
			require( reopened && reopened->status == 200, "Cannot reopen the cache fixture" );
			const auto native = cached_client.Get( "/api/cache", session );
			require( native && native->status == 200 && native->body.find( std::format( "\"decoded\":{{\"bytes\":0,\"cap\":{},",
				pyra::web::kServerDecodedCacheMiB * 1024 * 1024 ) ) != std::string::npos,
				( "A native open without PBF did not get the server's PBF: " + native->body ).c_str() );
		} );
		run( "test_web_root_change_lists_new_root", [&] {
			auto moved = RunningServer( pyra::web::utf8_path( argv[1] ) );
			auto moved_client = httplib::Client( "127.0.0.1", moved.port_ );
			const auto session = connect( moved_client );
			const auto images = pyra::web::utf8_path( argv[1] ) / "images";
			const auto changed = moved_client.Post( "/api/root", session, httplib::Params{ { "path", images.string() } } );
			require( changed && changed->status == 200 && changed->body.find( "/images\",\"slide\":null" ) != std::string::npos,
				"The local root change was not accepted" );
			const auto listing = moved_client.Get( "/api/files?path=", session );
			require( listing && listing->status == 200 && listing->body.find( "{\"name\":\"gray8\",\"directory\":true}" ) !=
				std::string::npos, "The file list does not start at the new root" );
			const auto outside = moved_client.Get( "/api/files?path=..", session );
			require( outside && outside->status == 400, "The old root's parent stayed reachable" );
		} );
		run( "test_web_root_change_saved_for_next_start", [&] {
			const auto file = pyra::web::utf8_path( argv[1] ) / "web_root_saved" / "web-root.txt";
			auto cleanup = std::error_code{};
			std::filesystem::remove_all( file.parent_path(), cleanup );
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.root_file = file;
			auto moved = RunningServer( std::move( options ) );
			auto moved_client = httplib::Client( "127.0.0.1", moved.port_ );
			const auto session = connect( moved_client );
			const auto images = std::filesystem::canonical( pyra::web::utf8_path( argv[1] ) / "images" );
			const auto changed = moved_client.Post( "/api/root", session, httplib::Params{ { "path", images.string() } } );
			require( changed && changed->status == 200 && changed->body.find( "\"saved\":true" ) != std::string::npos,
				"The changed root was not saved" );
			auto saved = std::filesystem::path{};
			require( pyra::web::read_saved_root( file, &saved ) && saved == images, "The saved root does not read back" );
			std::filesystem::remove_all( file.parent_path(), cleanup );
		} );
		run( "test_web_saved_root_ignored_when_missing", [&] {
			const auto file = pyra::web::utf8_path( argv[1] ) / "web_root_missing" / "web-root.txt";
			auto saved = std::filesystem::path{};
			require( !pyra::web::read_saved_root( file, &saved ), "A missing file gave a root" );
			auto error = std::string{};
			require( pyra::web::save_root( file, pyra::web::utf8_path( argv[1] ) / "no_such_directory", &error ),
				error.c_str() );
			require( !pyra::web::read_saved_root( file, &saved ) && saved.empty(),
				"A saved root that is no longer a directory was used" );
			auto cleanup = std::error_code{};
			std::filesystem::remove_all( file.parent_path(), cleanup );
		} );
		run( "test_web_root_change_rejects_relative_or_missing", [&] {
			auto moved = RunningServer( pyra::web::utf8_path( argv[1] ) );
			auto moved_client = httplib::Client( "127.0.0.1", moved.port_ );
			const auto session = connect( moved_client );
			const auto missing = ( pyra::web::utf8_path( argv[1] ) / "no_such_directory" ).string();
			for (const auto &path : { std::string( "images" ), std::string(), missing }) {
				const auto refused = moved_client.Post( "/api/root", session, httplib::Params{ { "path", path } } );
				require( refused && refused->status == 400, "A relative, empty or missing root was accepted" );
			}
		} );
		run( "test_web_root_change_refused_for_network", [&] {
			auto options = pyra::web::ServerOptions{ pyra::web::utf8_path( argv[1] ), {}, {} };
			options.public_origins = { "https://viewer.example.org" };
			options.allow_remote_write = true;
			auto proxied = RunningServer( std::move( options ) );
			auto proxy = httplib::Client( "127.0.0.1", proxied.port_ );
			const auto remote = connect( proxy, proxied_access( proxy, proxied.server_, "viewer.example.org" ) );
			const auto images = ( pyra::web::utf8_path( argv[1] ) / "images" ).string();
			const auto refused = proxy.Post( "/api/root", remote, httplib::Params{ { "path", images } } );
			require( refused && refused->status == 403, "A network client changed the image root" );
			const auto listing = proxy.Get( "/api/files?path=images", remote );
			require( listing && listing->status == 200, "The refused change moved the root" );
		} );
		run( "test_web_root_change_renames_open_slide", [&] {
			auto moved = RunningServer( pyra::web::utf8_path( argv[1] ) );
			auto moved_client = httplib::Client( "127.0.0.1", moved.port_ );
			const auto session = connect( moved_client );
			open( moved_client, session, "images/rgb8/rgb8.glr" );
			const auto images = ( pyra::web::utf8_path( argv[1] ) / "images" ).string();
			const auto changed = moved_client.Post( "/api/root", session, httplib::Params{ { "path", images } } );
			require( changed && changed->status == 200 && changed->body.find( "\"slide\":\"rgb8/rgb8.glr\"" ) !=
				std::string::npos, "The open slide was not named relative to the new root" );
			const auto inner = ( pyra::web::utf8_path( argv[1] ) / "images" / "gray8" ).string();
			const auto away = moved_client.Post( "/api/root", session, httplib::Params{ { "path", inner } } );
			require( away && away->status == 200 && away->body.find( "\"slide\":null" ) != std::string::npos,
				"A slide outside the new root kept a path" );
		} );
		run( "test_web_make_gallery_from_folder", [&] {
			const auto refused = client.Post( "/api/make-gallery", headers,
				httplib::Params{ { "path", "images/gray8" } } );
			require( refused && refused->status == 400 &&
				refused->body.find( "pixel size" ) != std::string::npos,
				"A folder stating no pixel size was not refused with the reason" );
			const auto made = client.Post( "/api/make-gallery", headers,
				httplib::Params{ { "path", "images/gray8" }, { "mpp", "1" } } );
			require( made && made->status == 200 &&
				made->body.find( "gray8.glr" ) != std::string::npos, "Gallery was not made" );
			open( client, headers, "images/gray8/gray8.glr" );
		} );
		run( "test_web_generate_synthetic_slide", [&] {
			const auto root = pyra::web::utf8_path( argv[1] );
			std::filesystem::remove( root / "web_synthetic.ptf" );
			auto params = httplib::Params{ { "path", "" }, { "name", "web_synthetic" }, { "width", "300" },
				{ "height", "200" }, { "tile", "128" }, { "z", "2" }, { "mpp", "0.5" }, { "z_step", "2" },
				{ "nucleus", "10" }, { "positive", "0.5" },
				{ "codec", std::to_string( PYRA_CODEC_JPEG ) }, { "quality", "90" } };
			const auto started = client.Post( "/api/generate", headers, params );
			require( started && started->status == 200 && started->body.find( "\"running\":true" ) != std::string::npos,
				"Generation did not start" );
			const auto status = wait_for_generation( client, headers );
			require( status.find( "\"error\":\"\"" ) != std::string::npos &&
				status.find( "\"path\":\"web_synthetic.ptf\"" ) != std::string::npos, status.c_str() );
			const auto opened = client.Post( "/api/open", headers, httplib::Params{ { "path", "web_synthetic.ptf" } } );
			require( opened && opened->status == 200 && opened->body.find( "\"z\":2" ) != std::string::npos,
				"Generated slide did not open with its second focal plane 2 um deep" );
			const auto existing = client.Post( "/api/generate", headers, params );
			require( existing && existing->status == 409, "An existing file was overwritten" );
			params.erase( "name" );
			params.emplace( "name", "../escape" );
			const auto escaped = client.Post( "/api/generate", headers, params );
			require( escaped && escaped->status == 400, "A path in the file name was accepted" );
		} );
		run( "test_web_generate_untiled_image", [&] {
			const auto root = pyra::web::utf8_path( argv[1] );
			std::filesystem::remove( root / "web_untiled.tif" );
			auto params = httplib::Params{ { "path", "" }, { "format", "ptf" }, { "name", "web_untiled.ptf" },
				{ "width", "300" }, { "height", "200" }, { "tile", "0" }, { "z", "1" }, { "mpp", "0.5" },
				{ "codec", std::to_string( PYRA_CODEC_LZW ) }, { "quality", "90" }, { "lossless", "1" } };
			const auto started = client.Post( "/api/generate", headers, params );
			require( started && started->status == 200, "Untiled generation did not start" );
			const auto status = wait_for_generation( client, headers );
			require( status.find( "\"error\":\"\"" ) != std::string::npos &&
				status.find( "\"path\":\"web_untiled.tif\"" ) != std::string::npos, status.c_str() );
			require( std::filesystem::is_regular_file( root / "web_untiled.tif" ), "No untiled TIFF was written" );
			params.erase( "format" );
			params.emplace( "format", "dicom" );
			const auto dicom = client.Post( "/api/generate", headers, params );
			require( dicom && dicom->status == 400, "An untiled DICOM was accepted" );
		} );
		run( "test_web_generate_dicom", [&] {
			const auto output = pyra::web::utf8_path( argv[1] ) / "generated_dicom";
			std::filesystem::remove_all( output );
			std::filesystem::create_directory( output );
			const auto request = [] ( const char *name, const char *width, pyra_codec codec ) {
				return httplib::Params{ { "path", "generated_dicom" }, { "name", name }, { "format", "dicom" },
					{ "width", width }, { "height", width }, { "tile", "128" }, { "z", "2" },
					{ "z_step", "3.25" }, { "mpp", "0.5" }, { "codec", std::to_string( codec ) },
					{ "quality", "90" }, { "lossless", codec == PYRA_CODEC_JPEG ? "0" : "1" } };
			};
			const auto jpeg = request( "jpeg.ptf", "301", PYRA_CODEC_JPEG );
			if (!pyra_synthetic_dicom_can_encode( PYRA_CODEC_JPEG )) {
				const auto refused = client.Post( "/api/generate", headers, jpeg );
				require( refused && refused->status == 400 && refused->body.find( "DCMTK" ) != std::string::npos,
					"Generation was not refused with a DICOM availability error" );
				return;
			}
			for (const auto &[name, codec] : { std::pair{ "jpeg", PYRA_CODEC_JPEG },
				std::pair{ "jpeg2000", PYRA_CODEC_JPEG2000 }, std::pair{ "htj2k", PYRA_CODEC_HTJ2K } }) {
				if (!pyra_synthetic_dicom_can_encode( codec ))
					continue;
				const auto params = codec == PYRA_CODEC_JPEG ? jpeg : request( name, "301", codec );
				const auto started = client.Post( "/api/generate", headers, params );
				require( started && started->status == 200, "DICOM generation did not start" );
				const auto relative = std::string( "generated_dicom/" ) + name + ".dcm";
				const auto status = wait_for_generation( client, headers );
				require( status.find( "\"error\":\"\"" ) != std::string::npos &&
					status.find( "\"path\":\"" + relative + "\"" ) != std::string::npos, status.c_str() );
				const auto auxiliary = output / ( std::string( "_" ) + name + "_" );
				require( std::distance( std::filesystem::directory_iterator( auxiliary ), {} ) == 2,
					"Expected two lower-resolution DICOM instances" );
				const auto opened = client.Post( "/api/open", headers, httplib::Params{ { "path", relative } } );
				require( opened && opened->status == 200 && opened->body.find( "\"index\":0,\"z\":0," ) != std::string::npos &&
					opened->body.find( "\"index\":1,\"z\":3.25," ) != std::string::npos,
					opened ? opened->body.c_str() : "DICOM open failed" );
				const auto base = "/api/render?generation=" + std::to_string( generation( opened->body ) ) +
					"&x=0&y=0&width=64&height=64&mpp=0.5&plane=";
				const auto first = client.Get( base + "0", headers );
				const auto second = client.Get( base + "1", headers );
				require( first && second && first->status == 200 && second->status == 200 && first->body != second->body,
					"DICOM focal planes did not render distinct pixels" );
				const auto existing = client.Post( "/api/generate", headers, params );
				require( existing && existing->status == 409, "Existing DICOM entry was not refused" );
			}
			const auto unsupported = client.Post( "/api/generate", headers, request( "jxl", "301", PYRA_CODEC_JPEGXL ) );
			require( unsupported && unsupported->status == 400, "JPEG XL was accepted for DICOM generation" );

			const auto started = client.Post( "/api/generate", headers, request( "cancel", "4096", PYRA_CODEC_JPEG ) );
			require( started && started->status == 200, "Cancellable DICOM generation did not start" );
			auto has_progress = false;
			for (auto attempt = 0; attempt < 200; ++attempt) {
				const auto progress = client.Get( "/api/generate", headers );
				require( progress && progress->status == 200 &&
					progress->body.find( "\"running\":true" ) != std::string::npos, "Generation ended before cancellation" );
				has_progress = progress->body.find( "\"done\":0," ) == std::string::npos;
				if (has_progress) break;
				std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
			}
			require( has_progress, "DICOM generation did not report tile progress" );
			const auto cancelled = client.Post( "/api/generate-cancel", headers );
			require( cancelled && cancelled->status == 200, "Cannot cancel DICOM generation" );
			const auto status = wait_for_generation( client, headers );
			require( status.find( "\"error\":\"cancelled\"" ) != std::string::npos, status.c_str() );
			require( !std::filesystem::exists( output / "cancel.dcm" ) && !std::filesystem::exists( output / "_cancel_" ),
				"Cancelled DICOM generation left published output" );
			for (const auto &entry : std::filesystem::directory_iterator( output ))
				require( !entry.path().filename().string().starts_with( ".pyra-generate-" ),
					"DICOM generation left a staging directory" );
		} );
		run( "test_web_rejects_stale_generation", [&] {
			const auto old = open( client, headers, "coarse.ptf" );
			open( client, headers, "images/gray8/gray8.glr" );
			const auto response = client.Get( view_url( old ), headers );
			require( response && response->status == 409, "Old image request accepted" );
		} );
		run( "test_web_rejects_oversized_raster", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto response = client.Get( "/api/render?generation=" + std::to_string( version ) +
				"&plane=0&x=0&y=0&width=" + std::to_string( pyra::web::kMaxRenderEdge + 1 ) + "&height=2&mpp=1", headers );
			require( response && response->status == 400, "Oversized raster accepted" );
		} );
		run( "test_web_rejects_nonfinite_mpp", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto response = client.Get( "/api/render?generation=" + std::to_string( version ) +
				"&plane=0&x=0&y=0&width=2&height=2&mpp=nan", headers );
			require( response && response->status == 400, "Nonfinite scale accepted" );
		} );
		run( "test_web_render_limit_roundoff_matches_c_api", [&] {
			const auto version = open( client, headers, "coarse.ptf" );
			const auto response = client.Get( "/api/render?generation=" + std::to_string( version ) +
				"&plane=0&x=0&y=0&width=4096.000000000004&height=1&mpp=1", headers );
			require( response && response->status == 200 && response->body.size() == pyra::web::kMaxRenderEdge * 4,
				"Viewer rejected a raster that the C API snaps to the render limit" );
		} );
		run( "test_web_zyp_exposes_five_indexed_focal_planes", [&] {
			const auto response = client.Post( "/api/open", headers, httplib::Params{ { "path", "zyp/five-plane.zyp" } } );
			require( response && response->status == 200, "ZYP stack did not open through the web API" );
			auto count = 0;
			for (auto pos = size_t{}; ( pos = response->body.find( "\"z_is_index\":true", pos ) ) != std::string::npos; ++pos) ++count;
			require( count == 5 && response->body.find( "nan" ) == std::string::npos &&
				response->body.find( "pyra.default-plane-index" ) != std::string::npos,
				"Web ZYP planes lost their index units or central-plane preference" );
			const auto version = generation( response->body );
			for (auto plane = 0; plane < 5; ++plane) {
				const auto frame = client.Get( "/api/render?generation=" + std::to_string( version ) +
					"&plane=" + std::to_string( plane ) + "&x=0&y=0&width=1&height=1&mpp=0.25", headers );
				require( frame && frame->status == 200 && !frame->body.empty() &&
					std::abs( int( static_cast<unsigned char>( frame->body[0] ) ) - ( 40 + plane * 35 ) ) <= 6,
					"Web ZYP request rendered a different focal plane" );
			}
		} );
#if PYRA_WEB_WITH_BRIDGE && USE_DCMTK
		{
			const auto root = pyra::web::utf8_path( argv[1] ) / "analyse";
			const auto scripts = root / "scripts";
			make_dicom_slide( root / "slide" );
			std::filesystem::create_directories( scripts );
			std::ofstream( scripts / "red.py", std::ios::binary ) << "def process(chunk, ctx):\n    return chunk[..., 0].copy()\n";
			std::filesystem::copy_file( pyra::web::utf8_path( argv[1] ) / "coarse.ptf", root / "coarse.ptf",
				std::filesystem::copy_options::overwrite_existing );
			auto options = pyra::web::ServerOptions{ root, {}, {} };
			options.scripts = scripts;
			options.bridge_cli = pyra::web::utf8_path( PYRA_WEB_BRIDGE_CLI );
			options.bridge_worker = pyra::web::utf8_path( PYRA_WEB_BRIDGE_PYTHON "/pyra_worker.py" );
			auto analysing = RunningServer( std::move( options ) );
			auto analyse_client = httplib::Client( "127.0.0.1", analysing.port_ );
			analyse_client.set_read_timeout( 30, 0 );
			const auto analyse_headers = connect( analyse_client );
			run( "test_web_analyse_capabilities", [&] {
				const auto state = analyse_client.Get( "/api/state", analyse_headers );
				require( state && state->body.find( "\"analyse\":{\"available\":true" ) != std::string::npos,
					"Analyse is not reported available" );
				const auto listing = analyse_client.Get( "/api/scripts", analyse_headers );
				require( listing && listing->body.find( "\"red.py\"" ) != std::string::npos, "The scripts folder is not listed" );
			} );
			run( "test_web_analyse_rejects_outside_script", [&] {
				open( analyse_client, analyse_headers, "slide/slide.dcm" );
				for (const auto *name : { "../red.py", "missing.py", "red.txt" }) {
					const auto refused = analyse_client.Post( "/api/analyse", analyse_headers, analyse_params( "server", name, "X" ) );
					require( refused && refused->status == 400, "A script outside the scripts folder was accepted" );
				}
			} );
			run( "test_web_analyse_refuses_non_dicom", [&] {
				open( analyse_client, analyse_headers, "coarse.ptf" );
				const auto refused = analyse_client.Post( "/api/analyse", analyse_headers, analyse_params( "server", "red.py", "X" ) );
				require( refused && refused->status == 400 && refused->body.find( "DICOM" ) != std::string::npos,
					"A non-DICOM slide was accepted" );
			} );
			if (!python_with_numpy()) {
				std::cout << "test_web_analyse_server_mode, test_web_analyse_cli_mode: SKIP (no Python with numpy)\n";
			} else {
				for (const auto *mode : { "server", "cli" }) {
					const auto name = std::string( "test_web_analyse_" ) + mode + "_mode";
					const auto channel = std::string( "R_" ) + mode;
					run( name.c_str(), [&] {
						open( analyse_client, analyse_headers, "slide/slide.dcm" );
						const auto started = analyse_client.Post( "/api/analyse", analyse_headers,
							analyse_params( mode, "red.py", channel.c_str() ) );
						require( started && started->status == 200, started ? started->body.c_str() : "Analyse request failed" );
						const auto status = wait_for_analysis( analyse_client, analyse_headers );
						require( status.find( "\"error\":\"\"" ) != std::string::npos, status.c_str() );
						require( status.find( "\"channels\":[\"" + channel + "\"]" ) != std::string::npos &&
							status.find( "\"wall_s\":" ) != std::string::npos && status.find( "\"time\":{\"opening\":" ) != std::string::npos &&
							status.find( "\"busy_workers\":" ) != std::string::npos,
							"The report has no channel or timing" );
						const auto reopened = analyse_client.Post( "/api/open", analyse_headers,
							httplib::Params{ { "path", "slide/slide.dcm" } } );
						require( reopened && reopened->body.find( "\"channel\":\"" + channel + "\"" ) != std::string::npos,
							"The reopened slide does not show the new channel" );
					} );
				}
			}
		}
#endif
		return failed ? 1 : 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n'; return 1;
	}
}
