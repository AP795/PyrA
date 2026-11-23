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

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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
			const auto analyse_headers = httplib::Headers{ { "X-Pyra-Session", analysing.server_.token() } };
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
