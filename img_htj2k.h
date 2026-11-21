// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "img_codec.h"

namespace priml {

// Raw HTJ2K codestreams, or bounded JP2/JPH containers with simple RGB/grey
// colour declarations. Profiles, palettes and channel remapping are rejected.
// Detection is available even when the OpenJPH feature is disabled.
bool is_htj2k_codestream ( const uint8_t *bf, size_t len ) noexcept;

bool htj2k_read_info ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error = nullptr );

template <typename pixeltype>
bool htj2k_decode ( const uint8_t *bf, size_t len, ColorHint hint,
	const TileDst<pixeltype> &dst, std::string *error = nullptr );

template <typename pixeltype>
bool htj2k_encode ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	const TileEncodeOptions &options, std::string *error = nullptr );

}
