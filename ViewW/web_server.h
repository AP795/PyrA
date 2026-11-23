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
