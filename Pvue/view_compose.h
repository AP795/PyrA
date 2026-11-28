// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Pvue's display rule: which planes of a slide make the picture a viewer shows,
// and how they become top-down RGBA8. It reads pixels only through the public
// C API, so the same code renders a slide opened from its file and the browser
// client's remote ("sister") slide, which holds the server's stored tiles.

#include <pyra/pyra.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pyra::pvue {

// The property a gallery slide carries (one image per tile, Core/gallery.*).
inline constexpr std::string_view kGalleryProperty = "gallery.members";


// What the display rule needs to know of a plane.
struct PlaneDesc {
	std::string channel;				// empty: an unnamed plane
	pyra_pixel_format format = PYRA_PIXEL_RGB8;
	double range = 255;					// full-scale sample value (max_sample_value)
	double z = 0;						// NaN when only an index is known
};


enum class DisplayKind : int {
	None = 0,		// nothing this rule displays: the plane is shown on its own
	Rgb8 = 1,		// an RGB plane, its samples as they are
	Rgb16 = 2,		// an RGB plane narrowed from its range
	Channels = 3,	// channels named R, G and B composed into colour
	Gray = 4,		// one grey plane, scaled from its range
	Gallery = 5		// a gallery: composed per acquired field, not yet here
};


// The planes a view of `kind` is made of: R, G and B for Channels, otherwise the
// reference plane alone.
struct DisplayRecipe {
	DisplayKind kind = DisplayKind::None;
	std::vector<uint32_t> planes;
	bool composite () const { return kind == DisplayKind::Channels; }
};


// The display view around `reference`, the plane the viewer has chosen. A named
// grey channel is composed with the channels named R, G and B (ASCII,
// case-insensitive, first of each) at its own focal position when all three
// exist; any other plane is displayed on its own. A gallery is never composed
// here: its fields need tile coverage the rule does not see.
DisplayRecipe display_recipe ( const std::vector<PlaneDesc> &planes, bool gallery, uint32_t reference );


// The slide's planes as the rule sees them, and whether it is a gallery.
bool slide_planes ( pyra_wsi *wsi, std::vector<PlaneDesc> *planes, bool *gallery, std::string *error );


struct ViewRect {
	double x_um = 0;
	double y_um = 0;
	double width_um = 0;
	double height_um = 0;
	double mpp = 0;
};


// A rendered view. The sample buffers are scratch the next render reuses.
struct Frame {
	std::vector<unsigned char> rgba;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<unsigned char> samples;
	std::vector<unsigned char> channel_samples[3];
};


// One plane rendered to RGBA8 by display_rgba (any pixel format). Returns OK,
// INCOMPLETE (missing tiles are background) or the failure.
pyra_status render_plane ( pyra_wsi *wsi, uint32_t plane, const ViewRect &view, Frame *frame,
	std::string *error );


// The recipe's view: a composite renders each of its planes and composes them,
// INCOMPLETE when any plane is; any other recipe renders its plane.
pyra_status render_display ( pyra_wsi *wsi, const DisplayRecipe &recipe, const ViewRect &view, Frame *frame,
	std::string *error );


// One rendered grey channel, as the C API returned it.
struct ChannelRaster {
	const unsigned char *data = nullptr;
	size_t row_stride_bytes = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	pyra_pixel_format format = PYRA_PIXEL_GRAY8;
	double range = 255;
};


// R, G and B channels composed into RGBA8 by Core's compose_rgb_from_channels.
// False when a channel is not grey or the three sizes differ.
bool compose_display ( const std::array<ChannelRaster, 3> &channels, std::vector<unsigned char> *rgba, uint32_t *width,
	uint32_t *height );

} // namespace pyra::pvue
