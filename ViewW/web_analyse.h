// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// "Analyse": the Python processing bridge (Core/bridge/chunk_process.h) run on the
// viewer's open DICOM slide, one run at a time, either through the pyra_process
// executable ("cli") or inside this server ("server"). Scripts come only from
// the configured scripts folder.

#include "web_server.h"

#include <memory>
#include <string>

namespace httplib { struct Request; }

namespace pyra::web {

// Largest halo the dialog may ask for, px.
inline constexpr int kAnalyseMaxHalo = 1024;


class Analyser {
public:
	explicit Analyser ( const ServerOptions &options );
	~Analyser ();
	Analyser ( const Analyser & ) = delete;
	Analyser &operator= ( const Analyser & ) = delete;
//
// {"available":..,"reason":..,"cli":..,"cores":..,"workers":..} for /api/state.
	std::string capabilities_json () const;
// {"folder":..,"scripts":[..]}: the .py files of the scripts folder.
	std::string scripts_json () const;
//
// Starts a run on `slide` (absolute path; `relative` is what the client sees).
// Throws std::invalid_argument for a bad request; returns false with the
// status JSON in `body` while another run is going.
	bool start ( const httplib::Request &request, const std::filesystem::path &slide,
		const std::string &relative, std::string *body );
	std::string status_json () const;
// A run is going: its paths are relative to the image root.
	bool running () const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace pyra::web
