// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pyra::web {

inline constexpr auto kMaxRenderEdge = 4096u;
inline constexpr auto kOverviewEdge = 256u;
//
// HTTP workers, and requests that may wait for one. cpp-httplib holds a worker
// for each open keep-alive connection. A browser keeps six per host, but other
// clients open new sockets while idle ones still hold workers (Node's fetch: 55 s
// for a pass that takes 1.7 s with enough workers), and a request queued behind
// idle connections waits for the keep-alive timeout. Workers are cheap here:
// slide access is serialized anyway.
inline constexpr auto kHttpWorkerThreads = 32u;
inline constexpr auto kHttpQueuedRequests = 64u;
// Idle keep-alive. Longer than clients wait before reusing an idle socket (Node's
// fetch: 4 s), so a request never lands on a socket the server is just closing;
// the workers above already outnumber a browser's connections.
inline constexpr auto kKeepAliveSeconds = 5;
// Stored tiles and geometry are addressed by the slide generation, so a URL's
// content never changes: browsers may keep it this long without revalidating.
inline constexpr auto kImmutableCacheSeconds = 31536000u;
// Each of the browser client's tile caches (compressed and decoded). A view must
// fit: tiles are put between renders, unpinned, and uncompressed CZI sub-blocks
// are large.
inline constexpr auto kClientCacheMiB = 256u;

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
// "https://host[:port]" when a reverse proxy serves the viewer there: requests
// with that Host and Origin are accepted beside the loopback ones. The session
// token is still required. Empty: loopback only.
	std::string public_origin;
};

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
	const std::string &token () const;
private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace pyra::web
