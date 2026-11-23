// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace priml {

// What a ".IMD" record says about its raster.
struct IpxImdMetadata {
	bool loaded = false;
	double metric_per_pixel = 1.0;
	double origin_x_um = 0.0;
	double origin_y_um = 0.0;
	int image_width = 0;
	int image_height = 0;
	std::uint32_t scene_id = 0;
	std::string acquisition_datetime;
	bool fluorescence = false;
//
// Which of the slide's channels this plane is, -1 for a derived plane: from the
// .IMD on a raster that has one, else set where the base plane is created
// (priml::base_channel_of, mark_acquired_plane).
	int rgb_channel = -1;
//
// The acquisition's colour contribution, stored verbatim in the .IMD. Only the
// exact basis vectors (255,0,0), (0,255,0), (0,0,255) mean that three scalar
// acquisitions can be materialised as one RGB raster; every other triplet is a
// display mixing coefficient and must remain a named scalar plane.
	std::array<std::uint8_t, 3> rgb_coefficients{};
//
// Largest value a sample of this raster can take: 4095 for the 12-bit sensors
// these file-sets come from, stored in a 16-bit container. Read from the record's
// display-lookup high point, which is the only place the acquisition writes it.
// 0 when the record did not say.
//
// Without it a 12-bit plane is indistinguishable from a very dark 16-bit one, and
// every lookup table built over it is sized wrong.
	int max_pixel_value = 0;
	std::string plane_name;
	std::string slide_name;
	std::string zone_name;
};


// ---------------------------------------------------------------------------
// The ".IMD" sidecar of a legacy IPX scene.
//
// Each "<prefix>_R_<8 hex>.ipx" raster is described by a same-stemmed ".imd"
// binary record carrying the scene's identity (slide/zone/plane names, scene
// id) and its metric placement (microns per pixel, stage origin). This is the
// only reader of that format; the grammar of the raster filenames themselves
// lives next door in ipx.h.
//
// ---------------------------------------------------------------------------

// Fills `out` from the ".imd" beside `ipx_path`. Returns false when the path is
// not a ".ipx", the sidecar is absent or unreadable, the record version is
// outside 1000..1002, or the geometry it carries is not finite and positive.
// A false return leaves `out` untouched.
bool load_ipx_imd_metadata ( const std::filesystem::path &ipx_path,
	IpxImdMetadata *out );

// The same record for any raster with a same-named ".imd" / ".IMD" beside it - a
// gallery member written by the same acquisition software as a plain TIFF. The
// overloads above accept only an ".ipx" member and delegate here.
bool load_imd_metadata_beside ( const std::filesystem::path &raster_path,
	IpxImdMetadata *out );

bool load_ipx_imd_metadata ( const std::string &ipx_path,
	IpxImdMetadata *out );

#ifdef _WIN32
bool load_ipx_imd_metadata ( const std::wstring &ipx_path,
	IpxImdMetadata *out );
#endif

} // namespace priml
