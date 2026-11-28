// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "web_server.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stopping = 0;

void stop_signal ( int ) { stopping = 1; }


// The viewer's per-user state directory (logs, the saved image root); empty when
// the environment names none.
std::filesystem::path user_state_directory ()
{
#ifdef _WIN32
//
// The wide variable: the narrow one is in the ANSI code page, not UTF-8.
	auto *base = (wchar_t *)nullptr;
	auto length = size_t{};
	if (_wdupenv_s( &base, &length, L"LOCALAPPDATA" ) == 0 && base) {
		auto path = std::filesystem::path( base );
		std::free( base );
		if (!path.empty())
			return path / "Pyra";
	}
#else
	if (const auto *base = std::getenv( "XDG_STATE_HOME" ); base && *base)
		return pyra::web::utf8_path( base ) / "pyra";
	if (const auto *base = std::getenv( "HOME" ); base && *base)
		return pyra::web::utf8_path( base ) / ".local/state/pyra";
#endif
	return {};
}


std::filesystem::path user_log_directory ()
{
	const auto state = user_state_directory();
	return state.empty() ? pyra::web::utf8_path( PYRA_WEB_LOG_DIRECTORY ) : state / "web-logs";
}


std::filesystem::path executable_directory ()
{
#if defined(_WIN32)
	auto buffer = std::wstring( 32768, L'\0' );
	buffer.resize( GetModuleFileNameW( nullptr, buffer.data(), (DWORD)buffer.size() ) );
	return std::filesystem::path( buffer ).parent_path();
#else
	auto buffer = std::string( 4096, '\0' );
	const auto length = readlink( "/proc/self/exe", buffer.data(), buffer.size() );
	if (length <= 0)
		return std::filesystem::current_path();
	buffer.resize( (size_t)length );
	return std::filesystem::path( buffer ).parent_path();
#endif
}


// Analyse defaults: pyra_process and python/ beside this program (an install),
// else where the build put them.
void default_bridge_paths ( pyra::web::ServerOptions *options )
{
#if PYRA_WEB_WITH_BRIDGE
	const auto here = executable_directory();
	auto error = std::error_code{};
#if defined(_WIN32)
	const auto cli = here / "pyra_process.exe";
#else
	const auto cli = here / "pyra_process";
#endif
	options->bridge_cli = std::filesystem::is_regular_file( cli, error ) ? cli : pyra::web::utf8_path( PYRA_WEB_BRIDGE_CLI );
	const auto python = std::filesystem::is_regular_file( here / "python" / "pyra_worker.py", error )
		? here / "python" : pyra::web::utf8_path( PYRA_WEB_BRIDGE_PYTHON );
	options->bridge_worker = python / "pyra_worker.py";
	options->scripts = python / "examples";
#else
	(void)options;
#endif
}

} // namespace


int main ( int argc, char **argv )
{
	try {
		auto root = pyra::web::utf8_path( PYRA_WEB_IMAGE_DIRECTORY );
		auto root_given = false;
		auto initial = std::string{};
		auto log_directory = user_log_directory();
		auto port = 8765;
		auto bridge = pyra::web::ServerOptions{};
		default_bridge_paths( &bridge );
		for (auto i = 1; i < argc; ++i) {
			const auto argument = std::string( argv[i] );
			if (argument == "--help") {
				std::cout << "Usage: PyrA_Web_Viewer [--root IMAGE_DIRECTORY] [--image RELATIVE_PATH]\n"
					"                     [--port PORT] [--log-dir DIRECTORY]\n"
					"                     [--scripts DIR] [--bridge-cli PATH] [--bridge-worker PATH] [--python PATH]\n"
					"                     [--public-origin https://HOST[:PORT]]... [--allow-remote-write]\n"
					"                     [--view-halo PERCENT]\n"
					"Without --root, the image directory last chosen in the viewer (Change) is used, else the build default.\n"
					"Analyse offers the *.py files of --scripts (default: the bridge's examples).\n"
					"--public-origin (repeatable) also accepts requests a reverse proxy forwards from that origin,\n"
					"from browsers that opened the printed access URL once. Network clients cannot generate,\n"
					"make galleries or analyse unless --allow-remote-write is given.\n"
					<< std::format( "--view-halo is the default band rendered around each view, percent of its width "
						"(0..{}, default {}).\n", pyra::web::kViewHaloMaxPercent, pyra::web::kViewHaloDefaultPercent )
					<< "Open the printed loopback URL in your browser. Ctrl+C stops the server.\n";
				return 0;
			}
			if (argument == "--allow-remote-write") { bridge.allow_remote_write = true; continue; }
			if (i + 1 == argc) throw std::invalid_argument( "Missing value for " + argument );
			const auto value = std::string( argv[++i] );
			if (argument == "--root") { root = pyra::web::utf8_path( value ); root_given = true; }
			else if (argument == "--image") initial = value;
			else if (argument == "--log-dir") log_directory = pyra::web::utf8_path( value );
			else if (argument == "--scripts") bridge.scripts = pyra::web::utf8_path( value );
			else if (argument == "--bridge-cli") bridge.bridge_cli = pyra::web::utf8_path( value );
			else if (argument == "--bridge-worker") bridge.bridge_worker = pyra::web::utf8_path( value );
			else if (argument == "--python") bridge.python = pyra::web::utf8_path( value );
			else if (argument == "--public-origin") bridge.public_origins.push_back( value );
			else if (argument == "--view-halo") {
				auto end = size_t{};
				bridge.view_halo_percent = std::stoi( value, &end );
				if (end != value.size() || bridge.view_halo_percent < 0 ||
					bridge.view_halo_percent > pyra::web::kViewHaloMaxPercent)
					throw std::invalid_argument( std::format( "View halo must be 0..{} percent",
						pyra::web::kViewHaloMaxPercent ) );
			}
			else if (argument == "--port") {
				auto end = size_t{};
				port = std::stoi( value, &end );
				if (end != value.size() || port < 0 || port > 65535)
					throw std::invalid_argument( "Port must be 0..65535 (0 selects a free port)" );
			} else throw std::invalid_argument( "Unknown option: " + argument );
		}
		std::filesystem::create_directories( log_directory );
		const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::system_clock::now().time_since_epoch() ).count();
		const auto log_path = log_directory / std::format( "pyra-web-{}.log", timestamp );
		auto log = std::ofstream( log_path, std::ios::out | std::ios::app );
		if (!log) throw std::runtime_error( "Cannot create session log" );
		auto log_mutex = std::mutex{};
		const auto write_log = [&] ( const std::string &message ) {
			const auto guard = std::lock_guard( log_mutex );
			log << message << '\n' << std::flush;
		};
// The root a browser last chose (Change in the Open dialog) unless --root names one.
		const auto state_directory = user_state_directory();
		if (!state_directory.empty())
			bridge.root_file = state_directory / "web-root.txt";
		auto root_source = std::string( root_given ? "--root" : "default" );
		if (!root_given && !bridge.root_file.empty()) {
			auto error = std::error_code{};
			if (pyra::web::read_saved_root( bridge.root_file, &root ))
				root_source = "saved";
			else if (std::filesystem::exists( bridge.root_file, error ))
				std::cout << "Saved image root in " << bridge.root_file << " is not a directory; using the default\n";
		}
		bridge.root = root;
		bridge.initial_image = initial;
		bridge.log = write_log;
		const auto scripts = bridge.scripts;
		const auto public_origins = bridge.public_origins;
		auto server = pyra::web::Server( std::move( bridge ) );
		port = server.bind( port );
		if (port < 0) throw std::runtime_error( "Cannot bind loopback port; use --port 0 to select a free one" );
		const auto url = std::format( "http://127.0.0.1:{}/", port );
		write_log( "Listening at " + url );
		std::cout << "PyrA viewer: " << url << "\nImage root: " << root << " (" << root_source << ")"
			<< "\nAnalyse scripts: " << ( scripts.empty() ? std::filesystem::path( "(none)" ) : scripts )
			<< "\nSession log: " << log_path << '\n';
		for (auto origin : public_origins) {
			while (origin.ends_with( '/' )) origin.pop_back();
			std::cout << "Network access (open once in each browser): " << origin << "/?key=" << server.access_key() << '\n';
		}
		std::cout << "Ctrl+C to stop.\n" << std::flush;
		std::signal( SIGINT, stop_signal );
		std::signal( SIGTERM, stop_signal );
		auto watcher = std::jthread( [&] ( std::stop_token token ) {
			while (!token.stop_requested()) {
				if (stopping && server.running()) { server.stop(); return; }
				std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
			}
		} );
		const auto success = server.listen();
		watcher.request_stop();
		write_log( "Server stopped" );
		return success ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "PyrA web viewer: " << error.what() << '\n';
		return 1;
	}
}
