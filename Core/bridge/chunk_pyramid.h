// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// The reduced levels a chunk worker builds from the block it already holds.
//
// A level-n frame lies inside one chunk as long as the chunk edge, in frames, is
// divisible by 2^n: chunk origins are multiples of the chunk edge, so at level n
// they still fall on frame boundaries. Those levels are built in the workers,
// with no decode of anything already encoded; the levels above them span several
// chunks and are rendered afterwards from the coarsest in-chunk level.

#include "geom.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

namespace priml::bridge_detail {

// Halvings until a level fits one frame: the number of reduced levels.
inline int reduced_level_count ( Dim2D<int> size, Dim2D<int> tile )
{
	auto levels = 0;
	while (size.w > tile.w || size.h > tile.h) {
		size = { 1 + ( size.w - 1 ) / 2, 1 + ( size.h - 1 ) / 2 };
		++levels;
	}
	return levels;
}


// Reduced levels a chunk of chunk_tiles frames holds whole frames of: the number
// of times it divides by two, and no more than the plane has.
inline int in_chunk_levels ( int chunk_tiles, int reduced_levels )
{
	return std::min( reduced_levels, std::countr_zero( (unsigned)std::max( 1, chunk_tiles ) ) );
}


// Level-n extent of a level-0 extent: halved n times, rounding up.
inline int level_extent ( int extent, int level )
{
	for (auto n = 0; n < level; ++n)
		extent = 1 + ( extent - 1 ) / 2;
	return extent;
}


// 2x2 mean, rounded to nearest. A last odd column or row averages only the
// samples that exist, so the slide edge is not darkened by padding.
template <typename P>
std::vector<P> halve ( const std::vector<P> &source, Dim2D<int> size, Dim2D<int> *halved )
{
	*halved = { 1 + ( size.w - 1 ) / 2, 1 + ( size.h - 1 ) / 2 };
	auto out = std::vector<P>( (size_t)halved->w * halved->h );
	for (auto y = 0; y < halved->h; ++y) {
		const auto *row0 = source.data() + (size_t)( 2 * y ) * size.w;
		const auto *row1 = 2 * y + 1 < size.h ? row0 + size.w : nullptr;
		auto *target = out.data() + (size_t)y * halved->w;
		for (auto x = 0; x < halved->w; ++x) {
			const auto x0 = 2 * x;
			const auto pair = x0 + 1 < size.w;
			auto sum = (uint32_t)row0[x0] + ( pair ? row0[x0 + 1] : 0u );
			auto count = pair ? 2u : 1u;
			if (row1) {
				sum += (uint32_t)row1[x0] + ( pair ? row1[x0 + 1] : 0u );
				count *= 2;
			}
			target[x] = (P)( ( sum + count / 2 ) / count );
		}
	}
	return out;
}

} // namespace priml::bridge_detail
