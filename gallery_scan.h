// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// A directory of independent images -> a gallery slide.
//
// The other half of gallery.h: that file defines the manifest and the loader that
// serves it; this one decides what goes into a manifest. Turning a manifest into
// an analysis slide is the host's business (WSIA: priml/gallery_scan.h).
//
// Made on an explicit request only (a viewer's "Make gallery", a host's command). A directory of TIFFs is not a slide by itself, and an image
// opened on its own stays an ordinary image. Members are read where they are; the
// user may have the uncompressed ones replaced by compressed ones (gallery_resave.h).
//
// What the scan decides, in order:
//
// * Members: the single-page TIFFs directly in the directory, in natural name
//   order (cell2 before cell10). Sidecars the gallery itself writes beside its
//   manifest are not members. A file the codec cannot read is refused and reported,
//   not skipped silently; so is a file whose pixel type differs from the majority.
// * Pixel size: a member's own ".imd" first, then its TIFF resolution tags when
//   they describe a microscope rather than a screen, then the caller's value.
//   Members that state different sizes refuse the whole gallery - one raster has
//   one resolution, and resampling is not done here.
// * Layout: a grid in member order, every cell the size of the largest member plus
//   a gutter wide enough that no two members share a footprint component at the
//   application's overview resolution. The first member sits at the origin.
// * Zone names: the file stems, with characters a zone name may not hold replaced,
//   made unique by their file-system slug.
//
// An IPX file-set of independent fields can be a gallery too, by the user's choice
// when an ".ipx" member is opened (conversion to a pyramid is the alternative) or
// when the directory is made into one. A field is one zone and each of its ".ipx"
// files a channel, on the same grid, the fields' stage origins kept in the
// manifest. Fields go on the grid whether or not they overlap on the stage, so any
// set can be one; ipx_gallery_fit only says whether it looks like independent
// images, for the user to weigh. Tissue in an overlap is analysed in both fields.

#include "gallery.h"
#include "ipx_layout.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace priml {

// The zone-name grammar of a gallery: what a zone name may hold, and the
// file-system slug that keys it. A host whose analysis keys zones by name (WSIA)
// uses these same definitions, so a gallery's names and its zones agree.
//
// The delimiter between a stem and what is associated with it ("{stem}~zone~...").
constexpr char kZoneAssocDelimiter = '~';
// Characters a zone name - and any slide file stem - may not contain: the
// path-reserved set. kZoneAssocDelimiter is banned alongside it.
constexpr std::string_view kZoneNameReservedChars = "/\\:*?\"<>|";
// Legal in a zone name but unsafe in a file name, so zone_name_slug() replaces
// them (as it does the reserved set, the delimiter and control characters) with
// kZoneSlugReplacementChar: '.' is read as an extension separator by every stem()
// on a sidecar path, '%' is expanded by shells and URLs.
constexpr std::string_view kZoneSlugUnsafeChars = ".%";
constexpr char kZoneSlugReplacementChar = '_';

// `zone_name` trimmed, with every reserved, unsafe or control character and the
// delimiter replaced by kZoneSlugReplacementChar.
[[nodiscard]] std::string zone_name_slug ( std::string_view zone_name );

// Footprint decimation of a mosaic built from rectangles: how many base pixels one
// footprint pixel covers when the caller has no overview resolution to match.
// Powers of two, a couple of thousand footprint pixels on the long side.
[[nodiscard]] int rects_footprint_step ( const Dim2D<int> &mosaic_size );


// The zone name a host gives an IPX field, or empty for the field's own name
// (its stem without channel tag, scene id and slide name). WSIA names a field
// after the "{slide}.ROI" polygon holding it. Set once by the host; none by default.
using IpxFieldNamer = std::function<std::string ( const IpxField &, const IpxLayout & )>;
void set_ipx_field_namer ( IpxFieldNamer namer );


// Gutter between members in FOOTPRINT pixels. Two members are separate zones only
// when at least one whole empty footprint pixel lies between them; three leaves
// room for the rounding of a non-integer footprint scale on both sides.
inline constexpr int kGalleryGutterFootprintPx = 3;

// An IPX set looks like independent images while they cover less than this
// fraction of their envelope. Above it the fields are more likely a slide's tissue
// seen piece by piece. Advice, not a limit: see ipx_gallery_fit.
inline constexpr double kGalleryMaxFillFactor = 0.2;

// Columns : rows of the grid, the shape of a screen.
inline constexpr double kGalleryGridAspect = 4.0 / 3.0;

// A TIFF resolution larger than this many microns per pixel describes a print or a
// screen (72 dpi is 353 um), not a microscope camera, and is ignored.
inline constexpr double kGalleryMaxPlausibleTagMpp = 10.0;


struct GalleryScanOptions {
//
// Microns per pixel used only when no member states one. 0 = none given.
	double mpp = 0.0;
//
// The resolution the application renders an overview scene - and so a
// footprint - at. Sizes the gutter, and must be what build_gallery_slide is given.
// 0 = the default footprint step for the mosaic (tests, batch without a config).
	float overview_mpp = 0.0f;
};


struct GalleryScanReport {
	int candidates = 0;
	int accepted = 0;
//
// (file name, reason) for every candidate left out.
	std::vector<std::pair<std::string, std::string>> refused;
//
// Accepted members a lossless rewrite could compress (TiffImageInfo::rewritable).
	int uncompressed = 0;
	std::string mpp_source;
	int gutter_px = 0;
//
// Where the manifest belongs: "{directory}/{directory name}.glr", or
// "{directory}/{slide}.glr" for an IPX set, whose directory may hold several slides.
	std::filesystem::path manifest_path;
//
// What the user should weigh about a set that was accepted; empty when nothing.
	std::string note;
//
// The scan was refused only because no member states its pixel size and none was
// given: scanning again with GalleryScanOptions::mpp set can succeed.
	bool needs_mpp = false;
};


// How far a scan has got, in files: every member's header, then its samples.
// Unreadable ordinary members are listed and excluded. Called on the scanning thread,
// even while the samples are read on the work pool, so a GUI may pump its events
// from here.
using GalleryScanProgressCallback =
	std::function<void ( std::size_t completed, std::size_t total )>;


// "{directory}/{directory name}.glr".
[[nodiscard]] std::filesystem::path gallery_manifest_path (
	const std::filesystem::path &directory );

// Whether an IPX set looks like independent images: no two fields share a pixel
// (strictly - ipx_layout_has_overlaps asks only whether an overlap is wide enough
// to stitch) and their surface is under kGalleryMaxFillFactor of the envelope. It
// never prevents a gallery: `why` says what to weigh, in words for the user, and
// a set that does not fit is not pre-selected for one.
struct IpxGalleryFit {
	bool fits = false;
	double fill_factor = 0.0;
	std::string why;
};

[[nodiscard]] IpxGalleryFit ipx_gallery_fit ( const IpxLayout &layout );

// The manifest of an IPX set as a gallery (not written): one zone per field, one
// member per ".ipx", Gray16 planes named as the acquisition modes are. Zones are
// named from the "{slide}.ROI" polygon holding the field, else from the field's own
// stem. Thumbnails are the ".ith" beside each member. A set ipx_gallery_fit does
// not like is still made, with its reason in report->note. False, reason in `error`,
// when a member cannot be read.
bool gallery_from_ipx_layout ( const IpxLayout &layout,
	const GalleryScanOptions &options, GalleryManifest *out,
	GalleryScanReport *report, std::string *error = nullptr,
	const GalleryScanProgressCallback &progress = {} );

// Scan `directory` into a manifest (not written). A directory holding an IPX set
// goes through gallery_from_ipx_layout. False, reason in `error`, when nothing
// usable is there or the members disagree on their pixel size.
bool scan_gallery_directory ( const std::filesystem::path &directory,
	const GalleryScanOptions &options, GalleryManifest *out,
	GalleryScanReport *report, std::string *error = nullptr,
	const GalleryScanProgressCallback &progress = {} );

} // namespace priml
