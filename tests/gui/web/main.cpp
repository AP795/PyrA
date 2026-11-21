// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "web_server.h"

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

namespace {

volatile std::sig_atomic_t stopping = 0;

void stop_signal ( int ) { stopping = 1; }


std::filesystem::path user_log_directory ()
{
#ifdef _WIN32
	if (const auto *base = std::getenv( "LOCALAPPDATA" ))
		return pyra::web::utf8_path( base ) / "Pyra" / "web-logs";
#else
	if (const auto *base = std::getenv( "XDG_STATE_HOME" ); base && *base)
		return pyra::web::utf8_path( base ) / "pyra" / "web-logs";
	if (const auto *base = std::getenv( "HOME" ); base && *base)
		return pyra::web::utf8_path( base ) / ".local/state/pyra/web-logs";
#endif
	return pyra::web::utf8_path( PYRA_WEB_LOG_DIRECTORY );
}

} // namespace


int main ( int argc, char **argv )
{
	try {
		auto root = pyra::web::utf8_path( PYRA_WEB_IMAGE_DIRECTORY );
		auto initial = std::string{};
		auto log_directory = user_log_directory();
		auto port = 8765;
		for (auto i = 1; i < argc; ++i) {
			const auto argument = std::string( argv[i] );
			if (argument == "--help") {
				std::cout << "Usage: pyra_web_test [--root IMAGE_DIRECTORY] [--image RELATIVE_PATH]\n"
					"                     [--port PORT] [--log-dir DIRECTORY]\n"
					"Open the printed loopback URL in your browser. Ctrl+C stops the server.\n";
				return 0;
			}
			if (i + 1 == argc) throw std::invalid_argument( "Missing value for " + argument );
			const auto value = std::string( argv[++i] );
			if (argument == "--root") root = pyra::web::utf8_path( value );
			else if (argument == "--image") initial = value;
			else if (argument == "--log-dir") log_directory = pyra::web::utf8_path( value );
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
		auto server = pyra::web::Server( { root, initial, write_log } );
		port = server.bind( port );
		if (port < 0) throw std::runtime_error( "Cannot bind loopback port; use --port 0 to select a free one" );
		const auto url = std::format( "http://127.0.0.1:{}/", port );
		write_log( "Listening at " + url );
		std::cout << "PyrA viewer: " << url << "\nImage root: " << root
			<< "\nSession log: " << log_path << "\nCtrl+C to stop.\n" << std::flush;
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
