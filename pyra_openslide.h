// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "pyra/pyra.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pyra::detail {

class OpenSlideReader {
public:
	virtual ~OpenSlideReader () = default;
	virtual const pyra_plane_info &plane () const = 0;
	virtual const std::vector<std::pair<std::string, std::string>> &properties () const = 0;
// Returns layout.capacity_bytes of packed RGB8 owned by the reader, valid until
// its next render. A throw leaves the caller's destination untouched.
	virtual const unsigned char *render ( const pyra_image_request &request,
		const pyra_image_layout &layout ) = 0;
};

class OpenSlideContext {
public:
	virtual ~OpenSlideContext () = default;
	virtual std::unique_ptr<OpenSlideReader> open ( const std::string &path ) = 0;
};

const char *openslide_version ();
std::unique_ptr<OpenSlideContext> make_openslide_context ( const pyra_context_options &options );

} // namespace pyra::detail
