// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// One Python worker process speaking the bridge protocol (bridge_protocol.h).

#include "bridge_protocol.h"
#include "pyra_subprocess.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace priml {

using BridgeFields = std::vector<std::pair<std::string, std::string>>;

// "key=value\n" lines. Keys may not contain '=' or a line break, values may not
// contain a line break; both are checked, not silently repaired.
bool format_bridge_fields ( const BridgeFields &fields, std::string *text, std::string *error );
BridgeFields parse_bridge_fields ( std::string_view text );


struct BridgeMessage {
	BridgeHeader header{};
	std::string text;
	std::vector<uint8_t> payload;
};


struct BridgeWorkerSpec {
	std::filesystem::path python;
	std::filesystem::path worker_script;
	std::string mode;						// "prepare" or "process"
	std::filesystem::path script;
	int index = 0;							// for log lines
//
// One thread for numpy's BLAS and OpenMP pools: N chunk workers would otherwise
// each start one thread per core.
	bool single_threaded = false;
};


class BridgeWorker {
public:
// Starts the process and waits for its Hello (or the error that replaced it).
	bool start ( const BridgeWorkerSpec &spec, std::string *error );
	bool send ( BridgeKind kind, BridgeHeader header, std::string_view text,
		const std::vector<std::span<const uint8_t>> &payload, std::string *error );
//
// An Error message from the worker is returned as false with its text.
	bool receive ( BridgeMessage *message, std::string *error );
	void stop ();
	void kill ();
	[[nodiscard]] std::string hello ( std::string_view key ) const;

private:
	Subprocess process_;
	BridgeFields hello_;
	int index_ = 0;
	bool started_ = false;
};

} // namespace priml
