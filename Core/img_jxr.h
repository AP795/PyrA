// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// JPEG XR (ITU-T T.832) images in their TIFF-like container (.jxr / .wdp), coded
// with the bundled jxrlib (Deps/jxrlib, BSD). A PTF tile codec (TIFF compression
// kTiffCompressionJpegXr) and the payload of CZI sub-blocks (img_czi.cpp).
// Detection is always available; coding needs USE_JXR.

#include "img_codec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace priml {

// TIFF compression code of JPEG XR tiles. Not in Adobe's registry; it is the
// value Bio-Formats reads as JPEG-XR, so its readers open our PTF tiles too.
inline constexpr uint16_t kTiffCompressionJpegXr = 22610;

// The packed sample layout a JPEG XR image decodes to: the codestream's own.
enum class JxrLayout : uint8_t { Gray8, Gray16, Gray32Float, Bgr24, Rgb24, Rgb48 };

size_t jxr_pixel_bytes ( JxrLayout layout ) noexcept;

struct JxrPacked {
	JxrLayout layout = JxrLayout::Gray8;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;		// rows of width * jxr_pixel_bytes(layout)
};

bool jxr_read_info ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error = nullptr );

// Decodes into packed rows in the codestream's own layout.
bool jxr_decode_packed ( const uint8_t *bf, size_t len, JxrPacked *out,
	std::string *error = nullptr );

template <typename pixeltype>
bool jxr_decode ( const uint8_t *bf, size_t len, const TileDst<pixeltype> &dst,
	std::string *error = nullptr );

// Rgb8, Rgb16, uint8_t, uint16_t and float. quality 101 (kImageLosslessQuality)
// is lossless; 1..100 are lossy, mapped to JPEG XR quantisation by
// jxr_quality_to_qp.
template <typename pixeltype>
bool jxr_encode ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	const TileEncodeOptions &options, std::string *error = nullptr );

// PyrA's mapping of a JPEG-style quality to the JPEG XR quantisation-parameter
// index (1 = lossless, larger = coarser). jxrlib's own quality tables live in its
// command-line tool, not the library, so this is a straight line instead.
int jxr_quality_to_qp ( int quality ) noexcept;

} // namespace priml
