// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Decoders for the payloads of Zeiss CZI sub-blocks, without libCZI.
//
// A CZI sub-block stores its pixels uncompressed, as JPEG, as JPEG XR or as zstd.
// The CZI loader (wsi_czi.cpp, the only libCZI user) wraps each payload in the
// CZSB descriptor of img_codec.h; decoding it needs only these functions, so a
// build without the loader - a remote client - still renders CZI tiles. JPEG XR
// comes from the bundled jxrlib (BSD) and zstd from the bundled zstd (BSD).

#include "build_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace priml {

// The values the CZI file format stores, which the CZSB descriptor carries as is.
enum class CziCompression : uint8_t { Uncompressed = 0, Jpeg = 1, JpegXr = 4, Zstd0 = 5, Zstd1 = 6 };
enum class CziPixelType : uint8_t { Gray8 = 0, Gray16 = 1, Gray32Float = 2, Bgr24 = 3, Bgr48 = 4 };

// Bytes one pixel of `type` occupies; 0 for a type no decoder here handles.
size_t czi_pixel_bytes ( CziPixelType type ) noexcept;

// Decodes a JPEG XR codestream of a `width` x `height` sub-block into packed rows
// in CZI sample order (blue first for colour). False when this build has no
// JPEG XR decoder or the stream does not hold such an image.
bool czi_decode_jpeg_xr ( const uint8_t *data, size_t size, CziPixelType type,
	int width, int height, std::vector<uint8_t> *pixels, std::string *error = nullptr );

// Decodes a zstd sub-block into packed rows. Zstd1 carries a small header whose
// one defined flag says the 16-bit samples were split into all low bytes, then
// all high bytes, before compression.
bool czi_decode_zstd ( const uint8_t *data, size_t size, bool zstd1, CziPixelType type,
	int width, int height, std::vector<uint8_t> *pixels, std::string *error = nullptr );

} // namespace priml
