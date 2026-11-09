#pragma once

// Compressed-tile codec layer: sniff the encoding from its magic bytes, then
// dispatch to the matching decoder.
//
// Every WSI loader (SVS/NDPI/TIFF/DICOM) funnels its compressed tile or frame
// bytes through decode_tile(). Loaders therefore never name a codec: they hand
// over a buffer and a destination, and this layer decides what it is. Adding a
// format is one kSigs row plus one decode body in img_codec.cpp - no loader
// changes.
//
// Modelled on \usr\src\ir\io_encdec.h (io_get_encoding + io_pb_decode).

#include "build_config.h"
#include "pwhl.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace priml {

inline constexpr int kImageQualityMin = 1;
inline constexpr int kImageLossyQualityMax = 100;
inline constexpr int kImageLosslessQuality = 101;
inline constexpr int kImageDefaultLossyQuality = 90;

// Keep in step with img_codec_names[] in img_codec.cpp.
//
// CziSubBlock is not a file format: it is a CZI sub-block payload behind a small
// descriptor the CZI loader prepends (see kCziTileHeaderBytes). The payload's own
// encoding - JPEG XR, zstd, JPEG or none - is named in that descriptor rather
// than sniffed, because zstd is not reliably identifiable from its leading bytes
// and libCZI's decoder has to be told the pixel type and dimensions anyway.
// Prepending a header to reach the decoder is the same thing the NDPI and SVS
// loaders already do with their JPEG tables.
//
// Tiff is a whole plain TIFF image held in memory - a gallery member (gallery.h),
// read off disk as it is. Its strip encodings (LZW, Deflate, PackBits, none) have
// no magic bytes of their own, so the container is the unit: libtiff opens the
// buffer and decodes it on the decode pool like any other tile (img_tiff.h).
//
// RawSamples is pixels a loader has already decoded, behind a small descriptor
// (kRawTileHeaderBytes). A gallery's thumbnail tiles use it: a thumbnail is one
// page of a member TIFF or a small sidecar, read with libtiff page-wise so only
// its own bytes leave the disk, and at a few KB decoding it in the loader costs
// less than shipping the container.
enum class ImgCodec { None, Jpeg, Jp2k, JpegLs, Jxl, CziSubBlock, Tiff, RawSamples, Last };


// Descriptor the CZI loader writes ahead of a sub-block's encoded bytes.
// Little-endian, 24 bytes:
//
//   0  "CZSB"                     magic
//   4  uint8   version
//   5  uint8   libCZI CompressionMode
//   6  uint8   libCZI PixelType
//   7  uint8   reserved
//   8  uint32  width
//  12  uint32  height
//  16  float   black point        normalised 0..1, from the CZI display settings
//  20  float   white point        black == 0 && white == 1 means full range
//
// The black/white points travel with the tile because decode_tile() has no other
// channel for them, and a grey plane scaled to the wrong range is the difference
// between visible tissue and a black frame.
inline constexpr int kCziTileHeaderBytes = 24;
inline constexpr uint8_t kCziTileHeaderVersion = 1;


// Descriptor ahead of already-decoded samples. Little-endian, 16 bytes:
//
//   0  "RAWS"                     magic
//   4  uint8   version
//   5  uint8   RawSampleLayout
//   6  uint16  reserved
//   8  uint32  width
//  12  uint32  height
//  16  packed rows, width pixels each, host byte order
inline constexpr int kRawTileHeaderBytes = 16;
inline constexpr uint8_t kRawTileHeaderVersion = 1;

enum class RawSampleLayout : uint8_t { Gray8 = 1, Gray16 = 2, Rgb8 = 3, Rgb16 = 4 };

// Bytes one pixel of `layout` occupies in the payload; 0 for an unknown layout.
size_t raw_sample_pixel_bytes ( RawSampleLayout layout ) noexcept;

// The layout a packed `pixeltype` raster is written in.
template <typename pixeltype>
constexpr RawSampleLayout raw_sample_layout_of () noexcept
{
	if constexpr (std::is_same_v<pixeltype, uint8_t>)
		return RawSampleLayout::Gray8;
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return RawSampleLayout::Gray16;
	else if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return RawSampleLayout::Rgb16;
	else
		return RawSampleLayout::Rgb8;
}

// Write the descriptor at `dest`, which must hold kRawTileHeaderBytes.
void write_raw_tile_header ( uint8_t *dest, RawSampleLayout layout, int w, int h );

// Human-readable names, indexed by ImgCodec; used so a missing decoder can say
// which one it is instead of just failing.
extern const char *const img_codec_names[];

const char *img_codec_name ( ImgCodec codec );

// Null when available; otherwise a user-facing codec/build requirement.
const char *img_codec_unavailable_reason ( ImgCodec codec ) noexcept;

// Stable lower-case identifier for configuration files. Only codecs that can
// be selected for PTF encoding have an identifier; unsupported values return
// an empty string. Parsing follows the same rule so JPEG XR/CZI payloads cannot
// accidentally become a PTF setting.
std::string_view img_codec_id ( ImgCodec codec );
bool parse_img_codec_id ( std::string_view text, ImgCodec *codec );

// Identify the encoding from the leading bytes. Needs up to 12 bytes: the JPEG
// XL and JP2 signature boxes differ only in the box-type field at offset 4.
ImgCodec sniff_img_codec ( const uint8_t *bf, size_t len );


// Container-independent facts needed to choose an owning output plane before
// decode. The query reads headers only and never allocates a pixel raster.
struct ImgCodecInfo {
	ImgCodec codec = ImgCodec::None;
	int width = 0;
	int height = 0;
	int color_channels = 0;
	int bits_per_sample = 0;
	bool is_float = false;
};

bool read_img_codec_info ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error = nullptr );


// How to interpret the decoded colour components.
//
// FromStream is right whenever the encoding is self-describing: a JPEG 2000
// codestream signals its multiple-component transform (YBR_ICT / YBR_RCT) in
// the COD marker and the decoder undoes it, so the output is already RGB.
// YCbCr exists for Aperio compression 33003, which stores YCbCr with the MCT
// switched off - that is not recoverable from the codestream, so the container
// tag is the only source and the SVS loader must say so explicitly.
enum class ColorHint { FromStream, YCbCr, Rgb };


// Where a decoded tile should land.
//
// The codec calls reserve() once the geometry is known and writes through the
// returned pointer, so each caller decodes straight into its final buffer with
// no intermediate surface. Returning nullptr aborts the decode.
template <typename pixeltype>
struct TileDst {
	std::function<pixeltype *( int w, int h, int &stride )> reserve;
};


// Knobs shared by tile encoders. A numeric quality is always lossy; 101 is a
// separate sentinel for a codec's true lossless mode. JPEG rejects 101 because
// it has no lossless mode. bits_per_sample is used by JXL and JPEG 2000.
struct TileEncodeOptions {
//
// Significant bits per sample, 0 meaning "the full range of the pixel type".
//
// Sensor data is routinely narrower than its container - 12 bits in a uint16.
// Narrowing this is lossless (the encoder and decoder both switch to the
// codestream's own depth, and test_ptf_jxl_bits_per_sample pins the round trip),
// but MEASURED, it is not worth doing: on a 12-bit field it saved 0.02% - 223262
// bytes against 223311 - because lossless modular coding already pays nothing for
// bits that are always zero.
//
// So leave it at 0 unless there is a reason other than size. A narrow codestream
// is read correctly only by a decoder that asks for the codestream's depth; every
// other JPEG XL tool rescales our tiles by 65535/4095. Paying that for 0.02% is a
// bad trade, and the true range travels in the PTF block's max-pixel-value where
// the application needs it anyway.
//
// Lossy JPEG XL is different: libjxl quantises perceptually relative to the
// declared full scale, so 12-bit data coded as 16-bit is treated as near-black.
// Measured on a 256x256 12-bit RGB gradient at quality 90
// (test_img_codec_jxl_rgb16_lossy_declared_depth): container 628 B, mean error
// 8.44 codes, worst 97; declared 12 bits 1482 B, mean 1.58, worst 21. The
// quality setting means something different in the two cases, so a lossy writer
// of narrow data should declare the depth it means the quality to apply to.
	int bits_per_sample = 0;
//
	// JPEG-style quality. 1..100 are lossy; 101 selects true lossless where the
	// codec supports it. The default preserves existing callers' lossless contract.
	int quality = kImageLosslessQuality;
};


// Identifies the tile in log lines, and carries the measured decode time back.
struct TileDiag {
	int level_no = -1;
	int tile_no = -1;
	double decode_ms = 0.0;
	ImgCodec unsupported_codec = ImgCodec::None;
};


// Decode one compressed tile/frame. Returns false and logs (naming the codec)
// on an unknown encoding, a missing decoder, or a corrupt stream.
template <typename pixeltype>
bool decode_tile ( const uint8_t *bf, size_t len, ColorHint hint,
	const TileDst<pixeltype> &dst, TileDiag &diag );


// Encode one tile. The symmetric counterpart of decode_tile: a writer hands over
// pixels and names the codec by enum, never by calling an encoder directly - so
// adding an encoding stays one case here plus one body in img_codec.cpp, exactly
// as adding a decoding is.
//
// Only encodings this layer can actually produce are accepted; the rest return
// false naming themselves rather than silently writing something else.
template <typename pixeltype>
bool encode_tile ( ImgCodec codec, const Pwhl<pixeltype> &image,
	std::vector<uint8_t> *out, const TileEncodeOptions &options = {},
	std::string *error = nullptr );


// Convenience wrapper: decode a standalone buffer into an owning Pwhl. Used for
// the DICOM auxiliary images (label / overview).
template <typename pixeltype>
bool decode_to_image ( const uint8_t *bf, size_t len, Pwhl<pixeltype> &out );


// Buffer-level JPEG XL, independent of any image container.
//
// The whole-image helpers that wrap these into a priml::Image live in
// priml/img_jxl.* instead, so this layer keeps no dependency on the image model
// above it. The encoder belongs here rather than there because writing a pyramid
// level needs JXL-encoded tiles, with no priml::Image anywhere in sight.
struct JxlImageInfo {
	int width = 0;
	int height = 0;
	int color_channels = 0;			// 1 = grey, 3 = colour
	int bits_per_sample = 0;
	bool is_float = false;
};


bool jxl_read_info ( const uint8_t *bf, size_t len, JxlImageInfo *out,
	std::string *error = nullptr );

// Decode a whole JXL codestream into an owning Pwhl of the requested type.
template <typename pixeltype>
bool jxl_decode ( const uint8_t *bf, size_t len, Pwhl<pixeltype> *out,
	std::string *error = nullptr );

// JXL encode of one plane. Supports Rgb, Rgb16, uint8_t, uint16_t and float. Encoding
// is lossless by default; set TileEncodeOptions::quality to 1..100 for lossy.
template <typename pixeltype>
bool jxl_encode ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	std::string *error = nullptr, const TileEncodeOptions &options = {} );

}
