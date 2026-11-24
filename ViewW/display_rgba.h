// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// The viewer's display conversion: a rendered raster in any plane format to
// top-down RGBA8. One definition for the server's /api/render frames and the
// browser client's own renders (wasm/pyra_client.cpp), so both show the same
// pixels. Native RGB8/Gray8 samples keep their values; wider samples are scaled
// by the plane's advertised maximum sample value.

#include <pyra/pyra.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pyra::web {

inline uint8_t display_sample ( const unsigned char *row, size_t index, pyra_pixel_format format, double range )
{
	auto sample = 0.0;
	if (format == PYRA_PIXEL_RGB16 || format == PYRA_PIXEL_GRAY16) {
		auto value = uint16_t{};
		std::memcpy( &value, row + index * sizeof( value ), sizeof( value ) );
		sample = value;
	} else if (format == PYRA_PIXEL_FLOAT32) {
		auto value = float{};
		std::memcpy( &value, row + index * sizeof( value ), sizeof( value ) );
		sample = value;
	} else {
		sample = row[index];
	}
	const auto scaled = sample * 255.0 / range;
	return std::isfinite( scaled ) ? uint8_t( std::clamp( scaled, 0.0, 255.0 ) ) : 0;
}


// Writes width x height RGBA8 pixels to `out` from `source` laid out as `layout`.
inline void display_rgba ( const unsigned char *source, const pyra_image_layout &layout, double range,
	unsigned char *out )
{
	const auto rgb = layout.pixel_format == PYRA_PIXEL_RGB8 || layout.pixel_format == PYRA_PIXEL_RGB16;
	if (!std::isfinite( range ) || range <= 0)
		range = 1;
//
// 8-bit samples at full range: sample * 255.0 / 255.0 is the sample exactly, so
// the general path would copy them; copying directly gives the same bytes at a
// fraction of the cost, which matters once per rendered frame.
	if (range == 255.0 && ( layout.pixel_format == PYRA_PIXEL_RGB8 || layout.pixel_format == PYRA_PIXEL_GRAY8 )) {
		for (auto y = 0u; y < layout.height; ++y) {
			const auto *row = source + size_t( y ) * layout.row_stride_bytes;
			auto *pixel = out + size_t( y ) * layout.width * 4;
			for (auto x = 0u; x < layout.width; ++x, pixel += 4) {
				const auto *sample = rgb ? row + size_t( x ) * 3 : row + x;
				pixel[0] = sample[0];
				pixel[1] = sample[rgb ? 1 : 0];
				pixel[2] = sample[rgb ? 2 : 0];
				pixel[3] = 255;
			}
		}
		return;
	}
	for (auto y = 0u; y < layout.height; ++y) {
		const auto *row = source + size_t( y ) * layout.row_stride_bytes;
		auto *pixel = out + size_t( y ) * layout.width * 4;
		for (auto x = 0u; x < layout.width; ++x) {
			for (auto c = 0u; c < 3; ++c)
				pixel[x * 4 + c] = display_sample( row, rgb ? size_t( x ) * 3 + c : x, layout.pixel_format, range );
			pixel[x * 4 + 3] = 255;
		}
	}
}

} // namespace pyra::web
