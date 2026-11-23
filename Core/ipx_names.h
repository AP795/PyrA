// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace priml {

// Name of the single plane of an image that has no channel of its own.
inline constexpr const char *kDefaultImagePlaneName = "Img";

// ---------------------------------------------------------------------------
// IPX triplet filenames.
// A legacy IPX scene is stored as three monochrome 16-bit TIFFs whose names
// share a common stem and differ only in a single-letter channel tag:
//   <prefix>_R_<8 hex digits><.ext>   (and _G_, _B_ siblings)
// These helpers are the single source of truth for that grammar; all viewers
// and the scene loader route through them instead of re-parsing the suffix.
// ---------------------------------------------------------------------------

struct IpxTripletName {
	bool valid = false;
	int channel = -1;          // 0=R, 1=G, 2=B
	std::string prefix;        // everything before "_R_"/"_G_"/"_B_"
	std::string scene_id;      // the 8 hex characters
	std::string extension;     // including the leading dot, e.g. ".ipx"
};

// Parses one triplet member path. On failure returns a default (valid==false).
IpxTripletName parse_ipx_triplet_name ( std::string_view path );

// [R, G, B] sibling paths for a parsed name (all empty if !valid).
std::array<std::string, 3> ipx_triplet_paths ( const IpxTripletName &name );

// Channel (0/1/2) inferred from a triplet member path, or -1 if not one.
int ipx_triplet_channel ( std::string_view path );

// Canonical plane name for one IPX member: the acquisition mode string from its
// ".IMD" ("RED10", "GREEN10", "DAPI"), with the filename's R/G/B tag as the
// fallback when the record says nothing useful.
//
// The single source of truth for what a channel is CALLED. Scripts address planes
// by name, so the legacy file-set loader and the .ptf converter have to agree on
// it exactly or the same slide grows two different sets of plane names depending
// on which path opened it.
//
// A mode name it does not recognise is kept verbatim - that is what lets a
// fluorescence set carry arbitrarily many named channels rather than only three.
std::string canonical_ipx_plane_name ( std::string_view mode, int rgb_channel );

// The spelling of an ordinary plane name (a sidecar, a script target, a name a
// user typed): the EXACT R/G/B/Img aliases (r, img_r, red; g, img_g, green;
// b, img_b, blue; img) collapse, ignoring ASCII case, and anything else is
// kept trimmed. Unlike canonical_ipx_plane_name there is no prefix matching, so
// a derived plane such as "redness" keeps its name. Empty in, empty out.
std::string canonical_plane_name ( std::string_view name );

#ifdef _WIN32
int ipx_triplet_channel ( std::wstring_view path );
#endif

} // namespace priml
