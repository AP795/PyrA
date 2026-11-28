// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// What a reader's tile caches hold of their budgets, for a viewer to show: the
// server's reader and the browser client's alike.

#include <pyra/pyra.h>

#include <cstdint>

namespace pyra::pvue {

// Bytes holding tiles and budget of each cache; PYRA_STAT_UNAVAILABLE bytes where the
// backend does not say (OpenSlide's compressed cache). compressed_bypasses
// counts encoded-tile reads that found no room in the compressed cache (budget
// too small);
// source_tile_reads the stored tiles read from the file (or, for a remote
// slide, put into it). The hits and misses are each cache's cumulative tile
// lookups (pyra_context_stats), PYRA_STAT_UNAVAILABLE where the backend does
// not say.
struct CacheUse {
	uint64_t compressed_bytes = 0;
	uint64_t compressed_cap = 0;
	uint64_t compressed_bypasses = 0;
	uint64_t compressed_hits = 0;
	uint64_t compressed_misses = 0;
	uint64_t decoded_bytes = 0;
	uint64_t decoded_cap = 0;
	uint64_t decoded_hits = 0;
	uint64_t decoded_misses = 0;
	uint64_t source_tile_reads = 0;
// Tiles each cache holds (PYRA_STAT_UNAVAILABLE for OpenSlide).
	uint64_t compressed_tiles = 0;
	uint64_t decoded_tiles = 0;
};

// The caches of `context`, which holds the one open slide: the bytes holding a
// tile (pyra_context_stats *_used_bytes) - a compressed tile at its own size, a
// decoded one in its slot - exact in either topology. Tiles of levels a server synthesized are held outside the caches
// and not counted.
inline bool cache_use ( pyra_context *context, uint64_t compressed_cap, uint64_t decoded_cap, CacheUse *use )
{
	auto stats = pyra_context_stats{};
	stats.struct_size = sizeof( stats );
	if (pyra_context_get_stats( context, &stats, nullptr ) != PYRA_OK)
		return false;
	*use = { stats.compressed_cache_used_bytes, compressed_cap, stats.compressed_cache_bypasses, stats.compressed_cache_hits,
		stats.compressed_cache_misses, stats.decoded_cache_used_bytes, decoded_cap, stats.decoded_cache_hits,
		stats.decoded_cache_misses, stats.source_tile_reads, stats.compressed_cache_tiles, stats.decoded_cache_tiles };
	return true;
}

} // namespace pyra::pvue
