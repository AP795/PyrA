#pragma once

// Gallery: a directory of independent images presented as one slide.
//
// Some acquisitions are not mosaics. A karyotyping run photographs one cell per
// image, at high magnification, and every image is analysed on its own - there is
// nothing to stitch and no stage geometry worth keeping. Such a directory becomes
// a slide by way of a small manifest, "{dir}/{dirname}.glr", that lays its images
// out side by side on a sparse mosaic. The GalleryLoader reads the manifest and
// serves every image as one tile at its rectangle; the pixels stay in the source
// files and are never copied. Each image later becomes one zone.
//
// The manifest is the single grammar shared by the writer (priml's scan) and the
// reader (wsi_glr.cpp), defined here once so the two cannot drift - the same rule
// as ptf_tags.h.
//
// The LAYOUT IS FROZEN in the manifest. Adding a file to the directory moves
// nothing: the zones, the footprint and every stored zone rectangle were derived
// from these rectangles and would all silently shift if the layout were recomputed
// from a directory listing. A member that changed on disk (size or mtime) makes
// the gallery refuse to open, naming the member - the slide has to be made again.
//
// Grammar: UTF-8 text, one "key=value" per line, "#" starts a comment line. The
// first line must be the marker. Header keys come first; each "member=" line holds
// TAB-separated fields (tab and newline are illegal in Windows file names and are
// refused in zone names):
//
//   wsia-gallery
//   version=3
//   pixel-type=gray16            (ptf_pixel_type_name spelling)
//   max-pixel-value=4095         (0 = the full range of the pixel type)
//   mpp=0.1                      (microns per pixel, identical for every member)
//   mpp-source=imd               (evidence only: imd | tiff | user)
//   channel-range=DAPI\t255      (measured full scale of this channel; overrides
//                                 the common max-pixel-value for its levels)
//   channel=DAPI                 (applies to the member lines after it; empty for
//                                 an RGB gallery, required for grey)
//   member=<x>\t<y>\t<w>\t<h>\t<bytes>\t<mtime>\t<stage-x>\t<stage-y>\t<zone>\t<file>
//   thumbnail=<member file>\t<page>\t<w>\t<h>\t<bytes>\t<mtime>\t<thumbnail file>
//
// One member line is one file. A gallery of several channels - an IPX field is one
// file per acquisition mode - has one line per (zone, channel), and the lines of a
// zone share its rectangle; each channel is a plane of the slide. A channel need not
// have every zone: a field acquired in one mode only is blank in the others.
//
// x, y, w, h are mosaic pixels, the rectangle half-open as everywhere in pyr.
// mtime is whole seconds since the Unix epoch. stage-x / stage-y are the member's
// own stage origin in microns when an .imd stated one, "-" otherwise - evidence for
// relocating a cell, never used for placement. `file` is relative to the manifest's
// directory with '/' separators.
//
// A "thumbnail=" line names a reduced copy of one member the acquisition already
// stored: a reduced-resolution page of the member itself (thumbnail file = the
// member file, page >= 1) or a separate file beside it (an IMSTAR ".ith", page 0).
// When EVERY member has one, of the gallery's pixel type and at one common scale,
// the loader serves them as a level of their own, and the overview is derived
// from it without reading a single full-size member - on a remote disk the open
// then reads kilobytes per member instead of megabytes. Anything less and the
// thumbnails are ignored, never mixed with derived pixels. A thumbnail that
// changed on disk drops the thumbnail level; it does not refuse the gallery,
// since zones never read it. Readers that predate the key ignore it.

#include "geom.h"
#include "pyr.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace priml {

// The extension, spelled once. kWsiFormats carries its own row for dispatch.
inline constexpr std::string_view kGalleryExtension = ".glr";

// First line of every manifest.
inline constexpr std::string_view kGalleryMarker = "wsia-gallery";

// Bumped only for a change a previous reader could misread.
inline constexpr int kGalleryVersion = 3;


// Two thumbnails agree on their scale, and a thumbnail on its own two axes, to
// within this fraction: the acquisition rounds a thumbnail's height to whole
// pixels (1541x1502 -> 128x125), and that rounding is all the slack a level of
// thumbnails tile-placed at rect / scale may have.
inline constexpr double kGalleryThumbnailScaleTolerance = 0.01;

// A thumbnail must be at least this much smaller than its member to be worth a
// level of its own.
inline constexpr double kGalleryThumbnailMinScale = 2.0;


struct GalleryThumbnail {
//
// Relative to the manifest's directory, '/' separators. The member's own file
// when the thumbnail is one of its reduced pages.
	std::string file;
	int page = 0;
	Dim2D<int> size{ 0, 0 };
//
// What a separate thumbnail file was when the manifest was written; unused (0)
// for a page of the member, which the member's own check covers.
	std::uint64_t file_bytes = 0;
	std::int64_t file_mtime = 0;
};


struct GalleryMember {
//
// Relative to the manifest's directory, '/' separators.
	std::string file;
//
// The member's zone name - the single source of it. The scan derives it; nothing
// downstream re-derives it. Members of one zone are its channels.
	std::string zone;
//
// The plane this file holds. Required for a grey member, because pyr renders an
// unnamed plane as colour; empty for an RGB one.
	std::string channel;
//
// Placement on the mosaic, mosaic pixels, half-open. Its size is the image's.
	Rtlbr<int> rect{ { 0, 0 }, { 0, 0 } };
//
// What the file was when the manifest was written; a mismatch refuses the open.
	std::uint64_t file_bytes = 0;
	std::int64_t file_mtime = 0;
//
// The member's stage origin in microns, when its .imd stated one.
	std::optional<Pxy<double>> stage_um;
//
// A reduced copy the acquisition stored, when there is one.
	std::optional<GalleryThumbnail> thumbnail;
};


struct GalleryManifest {
	int version = kGalleryVersion;
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
	int max_pixel_value = 0;
// Measured scales of named channels; the common scale above is the default.
	std::unordered_map<std::string, int> channel_max_pixel_values;
	double mpp = 0.0;
	std::string mpp_source;
	std::vector<GalleryMember> members;
};


// One zone of a gallery: what its members have in common.
struct GalleryZone {
	std::string name;
	Rtlbr<int> rect{ { 0, 0 }, { 0, 0 } };
};

// The manifest's zones and its channels, each once, in order of first appearance.
[[nodiscard]] std::vector<GalleryZone> gallery_zones ( const GalleryManifest &manifest );
[[nodiscard]] std::vector<std::string> gallery_channels ( const GalleryManifest &manifest );

// Whether a gallery plane was acquired in the requested zone. Full-size tile
// coverage is authoritative, including for lazy/reduced scenes. No decoding;
// non-gallery planes retain their ordinary slide-wide availability.
[[nodiscard]] bool gallery_plane_is_acquired ( const Ppyr &pyramid,
	const PlaneStack &plane, const Rtlbr<float> &region );

// True when `path` names a gallery manifest (by extension, any case).
[[nodiscard]] bool is_gallery_path ( const std::filesystem::path &path );

// Pixel types a gallery may hold: integer grey and colour, 8 or 16 bit.
[[nodiscard]] bool gallery_pixel_type_supported ( LevelPixelType type ) noexcept;

// The mosaic extent the members cover - the envelope of their rectangles, which
// the packer starts at the origin. Empty for a manifest with no members.
[[nodiscard]] Rtlbr<int> gallery_extent ( const GalleryManifest &manifest );

// Whole seconds since the Unix epoch of `path`'s last write, or nullopt.
[[nodiscard]] std::optional<std::int64_t> gallery_file_mtime (
	const std::filesystem::path &path );

// Checks a manifest is self-consistent: supported pixel type, a channel name on
// every grey member and none on an RGB one, a positive mpp, unique files, one member
// per (zone, channel), one rectangle per zone, zones that do not overlap, names free
// of tabs and newlines, and an extent starting at the origin. Returns false with
// the first problem in `error`.
bool validate_gallery_manifest ( const GalleryManifest &manifest,
	std::string *error = nullptr );

bool write_gallery_manifest ( const std::filesystem::path &path,
	const GalleryManifest &manifest, std::string *error = nullptr );

bool read_gallery_manifest ( const std::filesystem::path &path,
	GalleryManifest *out, std::string *error = nullptr );

// The common scale (member pixels per thumbnail pixel) of a gallery whose every
// member has a thumbnail usable as a pyramid level - one scale across members and
// axes within kGalleryThumbnailScaleTolerance, at least kGalleryThumbnailMinScale.
// Otherwise nullopt, with the first reason in `why`.
[[nodiscard]] std::optional<double> gallery_thumbnail_scale (
	const GalleryManifest &manifest, std::string *why = nullptr );

// Empty when `member`'s separate thumbnail file is still the one the manifest
// recorded (or the thumbnail is a page of the member); otherwise why not.
[[nodiscard]] std::string gallery_thumbnail_stale_reason (
	const std::filesystem::path &directory, const GalleryMember &member );

// Empty when `member` is still the file the manifest recorded; otherwise why not
// ("missing", "size changed", "modified").
[[nodiscard]] std::string gallery_member_stale_reason (
	const std::filesystem::path &directory, const GalleryMember &member );

} // namespace priml
