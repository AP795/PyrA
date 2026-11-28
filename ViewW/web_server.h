// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "pvue_http.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pyra::web {

// Shared with Pvue's browser client (Pvue/pvue_http.h).
using pvue::kClientCacheMiB;
using pvue::kClientCacheMaxMiB;
using pvue::kServerDecodedCacheMiB;
using pvue::kHttpQueuedRequests;
using pvue::kHttpWorkerThreads;
using pvue::kImmutableCacheSeconds;
using pvue::kKeepAliveSeconds;
using pvue::kMaxRenderEdge;
using pvue::kOverviewEdge;
using pvue::kStaleGenerationStatus;
// Halo band around each viewer frame, percent of the view's raster width.
inline constexpr auto kViewHaloDefaultPercent = 20;
inline constexpr auto kViewHaloMaxPercent = 50;
//
// Browser sessions: each holds its own slide, caches included, so the count bounds
// the server's memory. A session unused this long is closed when a new browser
// needs room; one in use is never closed.
inline constexpr auto kMaxSessions = 8u;
inline constexpr auto kSessionIdleMinutes = 30u;
// Random secrets (session ids, page tokens, the access key), in 32-bit words.
inline constexpr auto kSecretWords = 4;

inline std::filesystem::path utf8_path ( std::string_view value )
{
	return std::filesystem::path( std::u8string( value.begin(), value.end() ) );
}


// A JSON string literal: quotes, backslashes and control characters escaped.
inline std::string json_string ( std::string_view value )
{
	auto result = std::string( "\"" );
	for (const auto ch : value) {
		if (ch == '\\' || ch == '"') {
			result += '\\';
			result += ch;
		} else if (static_cast<unsigned char>( ch ) < 32) {
			static constexpr char kHex[] = "0123456789abcdef";
			result += "\\u00";
			result += kHex[static_cast<unsigned char>( ch ) >> 4];
			result += kHex[static_cast<unsigned char>( ch ) & 15];
		} else {
			result += ch;
		}
	}
	return result + '"';
}

struct ServerOptions {
	std::filesystem::path root;
// Where a changed root is saved for the next start (save_root); empty: not saved.
	std::filesystem::path root_file;
	std::string initial_image;
	std::function<void ( const std::string & )> log;
//
// Analyse (Python processing bridge). The scripts folder is the only place
// scripts are taken from; empty paths disable what needs them.
	std::filesystem::path scripts;				// folder of *.py offered in the dialog
	std::filesystem::path bridge_cli;			// pyra_process executable, for "apply by CLI"
	std::filesystem::path bridge_worker;		// pyra_worker.py
	std::filesystem::path python;				// interpreter; empty: the bridge's default
//
// "https://host[:port]" for each address a reverse proxy serves the viewer at:
// requests with that Host and Origin are accepted beside the loopback ones, once
// the browser has opened the access URL (access_key()). Empty: loopback only.
	std::vector<std::string> public_origins;
//
// Network clients may also write: generate, make a gallery and analyse. Off,
// only loopback clients may.
	bool allow_remote_write = false;
//
// Default halo band of the viewer's frames, percent of the view width per side
// (0..kViewHaloMaxPercent). Each browser can change it from the toolbar.
	int view_halo_percent = kViewHaloDefaultPercent;
//
// Browser session bounds (kMaxSessions, kSessionIdleMinutes); tests shorten them.
	unsigned max_sessions = kMaxSessions;
	unsigned session_idle_seconds = kSessionIdleMinutes * 60;
};

// The root saved in `file` by the last change, when it is still a directory.
bool read_saved_root ( const std::filesystem::path &file, std::filesystem::path *root );
// Saves `root` (UTF-8, one line) to `file`, replacing it whole; false with the
// reason in `error`.
bool save_root ( const std::filesystem::path &file, const std::filesystem::path &root, std::string *error );

class Server {
public:
	explicit Server ( ServerOptions options );
	~Server ();
	Server ( const Server & ) = delete;
	Server &operator= ( const Server & ) = delete;
	int bind ( int port );
	bool listen ();
	void stop ();
	bool running () const;
// The key a network browser presents once, as "?key=" on the page URL, to be let
// in; random per run.
	const std::string &access_key () const;
private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace pyra::web
