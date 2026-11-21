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

struct ServerOptions {
	std::filesystem::path root;
	std::string initial_image;
	std::function<void ( const std::string & )> log;
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
