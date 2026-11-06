#pragma once

// Plain TIFF images as a codec: a whole single-page TIFF - strips or tiles, any
// compression libtiff reads - decoded straight into a tile destination.
//
// The pyramid loaders never need this: their tiles carry JPEG/JPEG 2000/JPEG XL
// payloads the codec layer identifies by magic bytes. A gallery member (gallery.h)
// is different - an ordinary microscope image, often LZW or uncompressed, whose
// strips have no signature of their own. So the member's file IS the payload: the
// gallery loader reads its bytes as they are, and decode_tile() recognises the
// TIFF header and lands here, on the decode pool like every other tile.
//
// A member can also hold a JPEG XL or JPEG 2000 codestream as its one strip, written
// by tiff_image_write_lossless. libtiff has no strip codec for either, so that strip
// is read raw and goes to decode_tile, as a PTF tile does; the file stays a TIFF, with
// its name and tags, but only this code reads it.
//
// What is accepted is exactly what a gallery can hold: 8- or 16-bit unsigned
// samples, grey (min-is-black or min-is-white) or RGB (JPEG-in-TIFF YCbCr is
// converted by libjpeg), contiguous storage, extra samples (alpha) ignored. The
// header probe and the decoder share one classification, so a file the scan
// accepted is a file the loader can decode.

#include "img_codec.h"
#include "pyr.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct tiff;

namespace priml {

// A reduced-resolution page of the same image (SubfileType has the reduced-image
// bit) - what an acquisition writes as its thumbnail.
struct TiffReducedPage {
	int page = 0;
	Dim2D<int> size{ 0, 0 };
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
};


struct TiffImageInfo {
	Dim2D<int> size{ 0, 0 };
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
	int pages = 0;
//
// Every page after the first that is a reduced-resolution image this codec can
// decode. `other_pages` counts the pages that are not - another channel, another
// focal plane, an unreadable reduction - which is what makes a multi-page file
// something other than one image with its thumbnails.
	std::vector<TiffReducedPage> reduced;
	int other_pages = 0;
//
// The resolution tags as stored, before any judgement about them. Unit is the
// TIFF ResolutionUnit (1 none, 2 inch, 3 centimetre); 0 when absent.
	int resolution_unit = 0;
	double x_resolution = 0.0;
	double y_resolution = 0.0;
//
// Microns per pixel the tags imply; 0 when they imply none (no tag, or unit none).
	double tag_mpp_x = 0.0;
	double tag_mpp_y = 0.0;
//
// Uncompressed, one page, no extra samples, and every tag is one
// tiff_image_write_lossless carries over: rewriting the file loses nothing. A private
// tag, an ICC profile or a stage position is reason enough to leave a file alone.
	bool rewritable = false;
};

// Read `path`'s first-page header. Returns false, reason in `error`, for anything
// tiff_image_decode would refuse: palette, bilevel, float or signed samples,
// depths other than 8 or 16, planar-separate storage, raw YCbCr. `pages` reports
// the directory count; whether more than one is acceptable is the caller's call.
bool tiff_image_info ( const std::filesystem::path &path, TiffImageInfo *out,
	std::string *error = nullptr );

// Decode the first page of the TIFF held in `bf` into `dst`, converting to
// `pixeltype` by the tile-decode rules of pwhl.h (8-bit values widen unchanged,
// 16-bit narrow by dropping the low byte).
template <typename pixeltype>
bool tiff_image_decode ( const uint8_t *bf, size_t len,
	const TileDst<pixeltype> &dst, std::string *error = nullptr );

// Whether a TIFF of this compression tag holds a codestream libtiff cannot decode.
[[nodiscard]] bool tiff_compression_is_codec_strip ( uint16_t compression ) noexcept;

// Decode the current directory of an open TIFF into `dst`, by the same rules.
template <typename pixeltype>
bool tiff_handle_decode ( ::tiff *tif, const TileDst<pixeltype> &dst,
	std::string *error = nullptr );

// Decode page `page` (0-based) of the TIFF at `path` into `dst`, by the same rules.
// libtiff reads the directory chain up to that page and that page's own strips or
// tiles, nothing else - which is the point for a thumbnail on a remote disk.
template <typename pixeltype>
bool tiff_image_read_page ( const std::filesystem::path &path, int page,
	const TileDst<pixeltype> &dst, std::string *error = nullptr );

// How tiff_image_write_lossless stores the samples. There is no lossy member: this
// writes over acquisitions.
enum class TiffLosslessCodec { Lzw, Jxl, Jp2k };

// Write `image` as a one-strip TIFF at `path`, with the descriptive tags of the file
// at `tags_from`. Grey is written min-is-black: `image` holds decoded values.
template <typename pixeltype>
bool tiff_image_write_lossless ( const std::filesystem::path &path,
	const Pwhl<pixeltype> &image, TiffLosslessCodec codec,
	const std::filesystem::path &tags_from, std::string *error = nullptr );

} // namespace priml
