// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Internal to pyra_core: the tile load/decode pipeline that wsi.cpp owns, as
// the render path in wsi_render.cpp uses it. Not part of the library interface.

#include "img_codec.h"
#include "wsi.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace priml {

inline double ElapsedMs ( const std::chrono::steady_clock::time_point &t0 ) {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0 ).count();
}


inline TileSts WaitForTileIdle ( Tile &tile ) {
	while (true) {
		auto sts = tile.sts.load( std::memory_order_acquire );
		if (sts != TileSts::Loading && sts != TileSts::Decoding)
			return sts;
		std::this_thread::yield();
	}
}


struct RenderPlan {
	int level_no = -1;
	Rtlbr<int> request_px = {{0, 0}, {0, 0}};
	Rtlbr<int> render_px = {{0, 0}, {0, 0}};
	Rtlbr<float> render_mrct = {{0, 0}, {0, 0}};
	std::vector<int> tiles;
};


inline void PinRenderPlanTiles ( Ppyr &pyramid,
	const RenderPlan &plan, int delta )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size() || delta == 0)
		return;

// Under both of the level's pool locks, the way eviction selects a victim, so a
// pin cannot slip in between an eviction's scan and its release.
	std::scoped_lock pool_guard( cache_lock( pyramid, false, plan.level_no ),
		cache_lock( pyramid, true, plan.level_no ) );
	auto &level = pyramid.levels[plan.level_no];
	for (auto tile_no : plan.tiles) {
		auto &tile = level.tiles[tile_no];
		if (delta > 0)
			tile.pin_count.fetch_add( delta, std::memory_order_relaxed );
		else
			tile.pin_count.fetch_sub( -delta, std::memory_order_relaxed );
	}
}


class RenderTilePinGuard {
public:
	RenderTilePinGuard ( Ppyr &pyramid, const RenderPlan &plan )
		: pyramid_( pyramid ), plan_( plan ) {
		PinRenderPlanTiles( pyramid_, plan_, +1 );
	}
	~RenderTilePinGuard () { release(); }
	RenderTilePinGuard ( const RenderTilePinGuard & ) = delete;
	RenderTilePinGuard &operator= ( const RenderTilePinGuard & ) = delete;
	void release () {
		if (active_) {
			PinRenderPlanTiles( pyramid_, plan_, -1 );
			active_ = false;
		}
	}
private:
	Ppyr &pyramid_;
	const RenderPlan &plan_;
	bool active_ = true;
};


// Defined in wsi.cpp and instantiated there for every pixel type Generate
// renders.
template <typename pixeltype>
bool EnsureTilesLoaded ( WsiLoader &loader,
	Ppyr &pyramid, const RenderPlan &plan,
	version_t use_stamp, int &load_ok, int &load_fail );

template <typename pixeltype>
bool EnsureTilesDecoded ( WsiLoader &loader,
	Ppyr &pyramid, const RenderPlan &plan,
	int &decode_ok, int &decode_fail );

template <typename pixeltype>
bool DecodeTileToPixels ( const uint8_t *bf, size_t bf_sz,
	ColorHint hint, std::vector<pixeltype> &pixels, Dim2D<int> &dimensions,
	TileDiag &diag, std::atomic<ImgCodec> &unsupported_codec );

unsigned DecodeWorkerCount ( Ppyr &pyramid, size_t task_count );

} // namespace priml
