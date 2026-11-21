// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "web_server.h"

#include <httplib.h>
#include <pyra/pyra.h>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require ( bool condition, const char *message )
{
	if (!condition) throw std::runtime_error( message );
}


struct RunningServer {
	pyra::web::Server server_;
	int port_;
	std::thread thread_;
	explicit RunningServer ( const std::filesystem::path &root ) : server_( { root, {}, {} } ), port_( server_.bind( 0 ) ) {
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

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2) return 2;
	try {
		auto host = RunningServer( pyra::web::utf8_path( argv[1] ) );
		auto client = httplib::Client( "127.0.0.1", host.port_ );
		client.set_connection_timeout( 5, 0 );
		client.set_read_timeout( 15, 0 );
		const auto headers = httplib::Headers{ { "X-Pyra-Session", host.server_.token() } };
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
			for (const auto *path : { "/", "/viewer.css", "/viewer.js" }) {
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
			auto status = httplib::Result{};
			for (auto attempt = 0; attempt < 300; ++attempt) {
				status = client.Get( "/api/generate", headers );
				require( status && status->status == 200, "Generation status failed" );
				if (status->body.find( "\"running\":false" ) != std::string::npos) break;
				std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
			}
			require( status->body.find( "\"error\":\"\"" ) != std::string::npos &&
				status->body.find( "\"path\":\"web_synthetic.ptf\"" ) != std::string::npos,
				( "Generation failed or named another file: " + status->body ).c_str() );
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
		return failed ? 1 : 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n'; return 1;
	}
}
