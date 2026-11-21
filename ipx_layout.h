// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// A legacy IPX file-set as one slide's fields: which ".IPX" rasters belong to
// which field and channel, where each field sits in the mosaic, and the pixel
// size they share. Built from the ".IMD" records alone, without reading pixels.

#include "geom.h"
#include "pwhl.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace priml {

// One acquisition field: the same scene photographed once per channel.
struct IpxField {
	std::uint32_t scene_id = 0;
//
// The region name the acquisition itself recorded, which is what the ROI file and
// the zone naming later match against. Often empty.
	std::string imd_zone_name;
	std::string roi_name;
	double origin_x_um = 0.0;
	double origin_y_um = 0.0;
	int width = 0;
	int height = 0;
//
// Placement in mosaic pixels, derived from the stage origin. Half-open: br is one
// past the last pixel, matching pyr's tile rectangles.
	Rtlbr<int> rect{ { 0, 0 }, { 0, 0 } };
//
// Canonical channel name -> the ".IPX" holding it.
	std::unordered_map<std::string, std::filesystem::path> planes;
};


// The full scale of an IPX channel, decided from its samples and never from its
// .IMD, which can say 4095 over samples that fit in a byte (legacy files do): 255
// when every sample fits in a byte, else the 12-bit contract. Everything scales by
// it, the display, the OD curve and the blank of a zone's margin, so a pyramid and
// a gallery of one set have to agree on it.
inline constexpr int kIpxByteRange = 255;

inline int ipx_channel_range ( bool needs_uint16 )
{
	return needs_uint16 ? pixel_max_16 : kIpxByteRange;
}


struct IpxLayout {
	bool valid = false;
//
// True as soon as one ".ipx" member is seen, whether or not the scan then
// succeeds. It is what separates "there is no file-set here" - an ordinary image,
// nothing to say - from "there is one and it cannot be converted", which has to
// be reported: every route from a file-set to a slide, conversion or gallery,
// starts from this scan, so a refusal is the end of the road, not a fallback.
	bool members_present = false;
	std::string slide_name;
	std::string acquisition_datetime;
	bool fluorescence = false;
	std::map<std::string, bool> channel_fluorescence;
	bool final_placement = false;
//
// The slide's directory: where the pyramid and the "{slide}.ROI" live.
	std::filesystem::path directory;
//
// Where the members were actually found, which is `directory` until they are
// moved aside and `directory/sav` afterwards. Kept apart from `directory`
// because the ROI does not move with them and is still looked up beside the
// slide.
	std::filesystem::path source_directory;
//
// Microns per pixel, identical across every field in the set - see the uniform
// gate in scan_ipx_file_set.
	double mpp = 0.0;
//
// Stage position of mosaic pixel (0,0). The one number that converts between the
// pyramid's frame and the acquisition's.
	double origin_x_um = 0.0;
	double origin_y_um = 0.0;
	Dim2D<int> size{ 0, 0 };
//
// Acquisition ceiling. IMD display limits do not determine storage depth;
// conversion scans each channel and stores it as Gray8 or Gray16.
	int max_pixel_value = pixel_max_16;
//
// Canonical channel names, R/G/B first and anything else after, which is the
// order the rest of the application already presents planes in.
	std::vector<std::string> channels;
//
// Source modality names assigned to the red, green and blue components. All
// three are set only when every field has the same three modalities and their
// .IMD coefficients are exactly the three RGB basis vectors. Empty means the
// converter must preserve the modalities as separate Gray8/Gray16 planes.
	std::array<std::string, 3> rgb_components{};

	[[nodiscard]] bool produces_rgb16 () const
	{
		return !rgb_components[0].empty() && !rgb_components[1].empty() &&
			!rgb_components[2].empty();
	}
	std::vector<IpxField> fields;
};


// The fixed archive directory and every name the scan looks in besides the
// slide's own directory. The File Tree excludes the same directories.
inline constexpr std::string_view kIpxSourceSubdirectory = "sav_old_fmt";
inline constexpr std::string_view kIpxSourceSubdirectories[] = {
	kIpxSourceSubdirectory, "sav", "bak" };


// Scan one directory for a slide's file-set. Reads ".IMD" records only - no
// pixels - so it stays cheap enough to run on open.
//
// Returns false, with a reason in `error`, when the set is not convertible:
// no fields, or fields that disagree on microns per pixel. Disagreeing fields
// would need resampling to share one raster, and resampling is the one thing this
// conversion does not do. Check `out->members_present` to tell a directory that
// holds no file-set from one whose file-set was refused.
bool scan_ipx_file_set ( const std::filesystem::path &directory,
	std::string_view slide_name, IpxLayout *out, std::string *error = nullptr );


// Sorted by (top, left), scene id last, so a later field wins where two overlap -
// the rule pyr applies to overlapping tiles.
void sort_ipx_fields ( std::vector<IpxField> *fields );

} // namespace priml
