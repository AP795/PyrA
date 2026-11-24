// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "img_codec.h"
#include "wsi.h"
#include "wsi_tile_pipeline.h"

#include "pyr_rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace priml {

// The render path of WsiLoader: level selection, render planning, compositing
// tiles into a frame, sampling a frame or the cached tiles to the requested
// raster, and Generate/GenerateSpan. Tiles are loaded and decoded through the
// pipeline in wsi.cpp (wsi_tile_pipeline.h).

template <typename pixeltype>
static bool IsWholeWsiRequest ( const Rtlbr<float> &subrect,
	const Rtlbr<float> &mrct )
{
	constexpr float EPS = 0.5f;
	return std::fabs( subrect.tl.x - mrct.tl.x ) <= EPS &&
		std::fabs( subrect.tl.y - mrct.tl.y ) <= EPS &&
		std::fabs( subrect.br.x - mrct.br.x ) <= EPS &&
		std::fabs( subrect.br.y - mrct.br.y ) <= EPS;
}


template <typename pixeltype>
static bool IsAllBlack ( const Pwhl<pixeltype> &pwhl ) {
	if (!pwhl.p || pwhl.w <= 0 || pwhl.h <= 0)
		return true;

	for (int y = 0; y < pwhl.h; ++y) {
		auto *row = pwhl.p + y * pwhl.l;
		for (int x = 0; x < pwhl.w; ++x) {
			if constexpr (is_rgb_pixel_v<pixeltype>) {
				const auto &px = row[x];
				if (px.r != 0 || px.g != 0 || px.b != 0)
					return false;
			} else if constexpr (std::is_same_v<pixeltype, uint8_t>) {
				if (row[x] != 0)
					return false;
			} else {
				if (row[x] > 0)
					return false;
			}
		}
	}

	return true;
}


template <typename pixeltype>
static bool IsSparseDarkWholeWsiFrame ( const Pwhl<pixeltype> &pwhl,
	int max_pixel_value = 0 ) {
	if (!pwhl.p || pwhl.w <= 0 || pwhl.h <= 0)
		return false;

	constexpr int kDarkThreshold = 8;
	constexpr double kDarkRatioThreshold = 0.75;
	constexpr double kMaxContentWidthRatio = 0.25;
	constexpr double kMaxContentHeightRatio = 0.25;
	constexpr double kMaxContentAreaRatio = 0.15;

	auto dark_pixels = 0ull;
	auto content_pixels = 0ull;
	auto min_x = pwhl.w;
	auto min_y = pwhl.h;
	auto max_x = -1;
	auto max_y = -1;
	for (auto y = 0; y < pwhl.h; ++y) {
		auto *row = pwhl.p + y * pwhl.l;
		for (auto x = 0; x < pwhl.w; ++x) {
			auto lum = 0;
			if constexpr (std::is_same_v<pixeltype, Rgb>) {
				const auto &px = row[x];
				lum = (int(px.r) + int(px.g) + int(px.b)) / 3;
			} else if constexpr (std::is_same_v<pixeltype, Rgb16>) {
				const auto &px = row[x];
				const auto native = (int(px.r) + int(px.g) + int(px.b)) / 3;
				const auto range = max_pixel_value > 0
					? std::min( max_pixel_value, 0xffff ) : 0xffff;
				lum = std::min( 255, native * 255 / range );
			} else if constexpr (std::is_same_v<pixeltype, uint8_t>) {
				lum = int(row[x]);
			} else {
				lum = row[x] > 255 ? 255 : int(row[x]);
			}
			if (lum <= kDarkThreshold) {
				++dark_pixels;
				continue;
			}
			++content_pixels;
			min_x = std::min( min_x, x );
			min_y = std::min( min_y, y );
			max_x = std::max( max_x, x );
			max_y = std::max( max_y, y );
		}
	}

	auto total_pixels = static_cast<double>(pwhl.w) * static_cast<double>(pwhl.h);
	if (!( total_pixels > 0.0 ))
		return false;
	auto dark_ratio = static_cast<double>(dark_pixels) / total_pixels;
	if (dark_ratio < kDarkRatioThreshold || content_pixels == 0 || max_x < min_x || max_y < min_y)
		return false;

	auto content_w_ratio = static_cast<double>(max_x - min_x + 1) / static_cast<double>(pwhl.w);
	auto content_h_ratio = static_cast<double>(max_y - min_y + 1) / static_cast<double>(pwhl.h);
	auto content_area_ratio = content_w_ratio * content_h_ratio;
	return content_area_ratio <= kMaxContentAreaRatio ||
		( content_w_ratio <= kMaxContentWidthRatio &&
		  content_h_ratio <= kMaxContentHeightRatio );
}


// The finest level of `sel`'s ladder that is still no finer than the request.
//
// Scans one plane's ladder, never the whole level store: on a slide holding
// several planes the store interleaves them, and picking by metric alone would
// happily return another channel's level.
static int SelectBestLevel ( const Ppyr &pyramid, const PlaneSel &sel,
	float microns_per_pixel )
{
	int finest_available = -1;
	int best_lvl_idx = -1;

	const auto *stack = find_plane( pyramid, sel );
	if (!stack)
		return -1;

	for (auto i : stack->ladder) {
		const auto &level = pyramid.levels[i];
		if (level.reject_whole_wsi_black && i != stack->ladder.front())
			continue;
		if (finest_available < 0)
			finest_available = i;
		if (level.metric <= microns_per_pixel)
			best_lvl_idx = i;
	}

	if (best_lvl_idx >= 0)
		return best_lvl_idx;

	return finest_available;
}


// Sample the requested metric rectangle, not the clipped source extent. Missing
// source pixels stay background; clipping must never stretch the imaged part.
template <typename pixeltype>
static Pwhl<pixeltype> SampleRequestedRaster ( const Render<pixeltype> &render,
	const Level &level, const Rtlbr<float> &subrect, const Dim2D<int> &dimensions )
{
// Every pixel below is either sampled or set to background, so the buffer
// needs no zero-fill of its own.
	auto dst = Pwhl<pixeltype>( dimensions, uninit );
	const auto mapping = RasterMapping( subrect, dimensions );
	const auto source = IntersectRect( render.lr, level.bounds );
	const auto scale = level.metric > 0.0f ? level.metric : 1.0f;
	auto columns = std::vector<int>( dimensions.w );
	for (int x = 0; x < dimensions.w; ++x)
		columns[x] = mapping.x.source_pixel( x, scale ) - render.lr.tl.x;
//
// Sample centres rise with x, so their source columns never decrease and the
// ones inside the frame form a single run [first, last). Consecutive columns -
// any 1:1 mapping - copy as whole rows.
	const auto left = source.tl.x - render.lr.tl.x;
	const auto right = source.br.x - render.lr.tl.x;
	const auto first = (int)( std::lower_bound( columns.begin(), columns.end(), left ) - columns.begin() );
	const auto last = std::max( first,
		(int)( std::lower_bound( columns.begin(), columns.end(), right ) - columns.begin() ) );
	auto consecutive = true;
	for (auto x = first; x < last && consecutive; ++x)
		consecutive = columns[x] == columns[first] + ( x - first );
	const auto background = pixeltype{};
	for (int y = 0; y < dimensions.h; ++y) {
		auto *dp = dst.rowp( y );
		const auto sy = mapping.y.source_pixel( y, scale );
		if (sy < source.tl.y || sy >= source.br.y || first == last) {
			std::fill_n( dp, dimensions.w, background );
			continue;
		}
		std::fill_n( dp, first, background );
		std::fill_n( dp + last, dimensions.w - last, background );
		const auto *sp = render.image.rowp( sy - render.lr.tl.y );
		if (consecutive) {
			std::copy_n( sp + columns[first], last - first, dp + first );
			continue;
		}
		for (auto x = first; x < last; ++x)
			dp[x] = sp[columns[x]];
	}
	return dst;
}


// The source pixels a sampling of `subrect` at `raster` reads: centres map to
// floor(centre / scale) on each axis, rising with the raster index, so the
// first and last sample bound them. Same expression as SampleRequestedRaster.
static Rtlbr<int> SampledSourceBox ( const Rtlbr<float> &subrect,
	const Dim2D<int> &raster, float scale )
{
	if (raster.w <= 0 || raster.h <= 0)
		return {{0, 0}, {0, 0}};
	const auto mapping = RasterMapping( subrect, raster );
	return {
		{ mapping.x.source_pixel( 0, scale ), mapping.y.source_pixel( 0, scale ) },
		{ mapping.x.source_pixel( raster.w - 1, scale ) + 1,
		  mapping.y.source_pixel( raster.h - 1, scale ) + 1 } };
}


// True when sampling `subrect` at `raster` from the frame would copy it
// unchanged: same size, consecutive rows and columns, all inside the level.
template <typename pixeltype>
static bool IsIdentitySample ( const Render<pixeltype> &render, const Level &level,
	const Rtlbr<float> &subrect, const Dim2D<int> &raster )
{
	if (!render.image.p || !priml::are_wh_eq( render.image, raster ) ||
		!are_wh_eq( render.lr, raster ))
		return false;
	const auto inside = IntersectRect( render.lr, level.bounds );
	if (!priml::rct_are_eq( inside, render.lr ))
		return false;
	const auto mapping = RasterMapping( subrect, raster );
	const auto scale = level.metric > 0.0f ? level.metric : 1.0f;
	for (int x = 0; x < raster.w; ++x)
		if (mapping.x.source_pixel( x, scale ) != render.lr.tl.x + x)
			return false;
	for (int y = 0; y < raster.h; ++y)
		if (mapping.y.source_pixel( y, scale ) != render.lr.tl.y + y)
			return false;
	return true;
}


// One source-pixel interpolation neighbour on each side of a source grid.
constexpr int kSourceGridInterpolationBorder = 1;


// The source pixels covering `subrect`, widened by `border` on each side and
// clipped to the level.
static Rtlbr<int> SourceGridBox ( const Level &level, const Rtlbr<float> &subrect,
	int border )
{
	auto box = bbox_from_metric( subrect, level.metric > 0.0f ? level.metric : 1.0f );
	box.tl.x -= border;
	box.tl.y -= border;
	box.br.x += border;
	box.br.y += border;
	return IntersectRect( box, level.bounds );
}


// `sampled`, when given, is the raster the caller resamples the frame to at
// once, keeping nothing: the frame is then exactly the source pixels that
// sampling reads, instead of the envelope of whole tiles a persistent Render
// reuses while panning. With tiles as wide as the view (NDPI restart rows),
// that envelope is twice the view. `source_border` widens an unsampled request
// by interpolation neighbours (GenerateSourceGrid).
static RenderPlan BuildRenderPlan ( const Ppyr &pyramid,
	int level_no, const Rtlbr<float> &subrect, const Dim2D<int> *sampled,
	int source_border = 0 )
{
	RenderPlan plan;
	if (level_no < 0 || level_no >= (int)pyramid.levels.size())
		return plan;

	const auto &level = pyramid.levels[level_no];
	const auto scale = level.metric > 0.0f ? level.metric : 1.0f;
	plan.level_no = level_no;
	plan.request_px = sampled ? SampledSourceBox( subrect, *sampled, scale )
		: SourceGridBox( level, subrect, source_border );

	if (level.tiles.empty())
		return plan;

// GenerateSpan exposes source coverage only. Generate maps that coverage into
// the original requested rectangle, preserving its origin and any overhang.
	plan.request_px = IntersectRect( plan.request_px, level.bounds );
	if (plan.request_px.is_empty())
		return plan;

	select_tiles_for_rect( level, plan.request_px, plan.tiles, plan.render_px );
//
// The frame covers the request too, not only the tiles found for it. On a sparse
// level - a gallery's gutters, a CZI mosaic's unscanned stage - a request reaching
// past its tiles into area no tile stores would otherwise be cut from a frame that
// does not hold it, and come back empty rather than as tissue plus background.
// On a full grid the tiles already cover the request and nothing changes.
// A request with no tiles at all - an omitted sparse frame inside the declared
// matrix - is a frame of background.
	plan.render_px = sampled || plan.tiles.empty() ? plan.request_px
		: UnionRect( plan.render_px, plan.request_px );

	plan.render_mrct = {
		{pyramid.mrct.tl.x + plan.render_px.tl.x * scale,
		 pyramid.mrct.tl.y + plan.render_px.tl.y * scale},
		{pyramid.mrct.tl.x + plan.render_px.br.x * scale,
		 pyramid.mrct.tl.y + plan.render_px.br.y * scale}
	};
	return plan;
}


// One placement rule for cached, synthetic and freshly decoded tiles.
// Clip both to the declared tile rectangle and to the decoded surface.
template <typename pixeltype>
static void BlitTileIntoRender ( const Rtlbr<int> &tile_rect,
	const PwhlView<pixeltype> &pixels, Render<pixeltype> &render )
{
	if (!pixels.p || !render.image.p)
		return;
	auto source_rect = tile_rect;
	source_rect.br.x = source_rect.tl.x + std::min(
		tile_rect.br.x - tile_rect.tl.x, pixels.w );
	source_rect.br.y = source_rect.tl.y + std::min(
		tile_rect.br.y - tile_rect.tl.y, pixels.h );
	auto intersection = IntersectRect( render.lr, source_rect );
	if (intersection.is_empty())
		return;

	auto copy_w = intersection.br.x - intersection.tl.x;
	auto src_x = intersection.tl.x - tile_rect.tl.x;
	auto src_y = intersection.tl.y - tile_rect.tl.y;
	auto dst_x = intersection.tl.x - render.lr.tl.x;
	auto dst_y = intersection.tl.y - render.lr.tl.y;
	for (auto y = 0; y < intersection.br.y - intersection.tl.y; ++y) {
		auto *src = pixels.p + (size_t)( src_y + y ) * pixels.l + src_x;
		auto *dst = render.image.rowp( dst_y + y ) + dst_x;
		std::copy_n( src, copy_w, dst );
	}
}


// Above this many tiles the partition test below costs more than the zero-fill
// it would save; such a frame is simply zero-filled.
constexpr size_t kMaxPartitionTestTiles = 4096;


// True when the plan's tiles, clipped to the frame, cover every frame pixel
// exactly once - a regular grid does; a mosaic with overlap or gaps does not.
// Each frame pixel then belongs to one tile, so painting or clearing every
// tile writes the whole frame and the frame needs no zero-fill of its own.
// Bands between consecutive tile top/bottom edges hold a constant tile set;
// in each, the clipped spans must abut from the frame's left to its right edge.
static bool TilesPartitionFrame ( const Level &level, const RenderPlan &plan )
{
	const auto &frame = plan.render_px;
	if (frame.is_empty() || plan.tiles.empty() || plan.tiles.size() > kMaxPartitionTestTiles)
		return false;
	auto edges = std::vector<int>{ frame.tl.y, frame.br.y };
	for (auto tile_no : plan.tiles) {
		const auto &lr = level.tiles[tile_no].lr;
		edges.push_back( std::clamp( lr.tl.y, frame.tl.y, frame.br.y ) );
		edges.push_back( std::clamp( lr.br.y, frame.tl.y, frame.br.y ) );
	}
	std::sort( edges.begin(), edges.end() );
	edges.erase( std::unique( edges.begin(), edges.end() ), edges.end() );
	auto spans = std::vector<std::pair<int, int>>{};
	for (size_t band = 0; band + 1 < edges.size(); ++band) {
		const auto top = edges[band];
		const auto bottom = edges[band + 1];
		spans.clear();
		for (auto tile_no : plan.tiles) {
			const auto &lr = level.tiles[tile_no].lr;
			if (lr.tl.y > top || lr.br.y < bottom)
				continue;
			const auto left = std::max( lr.tl.x, frame.tl.x );
			const auto right = std::min( lr.br.x, frame.br.x );
			if (left < right)
				spans.emplace_back( left, right );
		}
		std::sort( spans.begin(), spans.end() );
		auto reached = frame.tl.x;
		for (const auto &[left, right] : spans) {
			if (left != reached)
				return false;
			reached = right;
		}
		if (reached != frame.br.x)
			return false;
	}
	return true;
}


template <typename pixeltype>
static bool InitializeRender ( const RenderPlan &plan, Render<pixeltype> &render,
	bool zero_fill )
{
	render.clear();
	render.level_no = plan.level_no;
	render.lr = plan.render_px;
	render.mrct = plan.render_mrct;
	render.tiles = plan.tiles;
	const auto dimensions = Dim2D<int>{
		plan.render_px.br.x - plan.render_px.tl.x,
		plan.render_px.br.y - plan.render_px.tl.y };
	render.image = zero_fill ? Pwhl<pixeltype>( dimensions ) : Pwhl<pixeltype>( dimensions, uninit );
	return render.image.p != nullptr;
}


template <typename pixeltype>
static void ClearTileArea ( const Rtlbr<int> &tile_rect, Render<pixeltype> &render )
{
	const auto area = IntersectRect( render.lr, tile_rect );
	if (area.is_empty() || !render.image.p)
		return;
	for (auto y = area.tl.y; y < area.br.y; ++y)
		std::fill_n( render.image.rowp( y - render.lr.tl.y ) + ( area.tl.x - render.lr.tl.x ),
			area.br.x - area.tl.x, pixeltype{} );
}


// Paint a tile, or leave its area background. On a frame that was not
// zero-filled (`clear` - its tiles partition it) the background is written
// here: for a tile with no pixels, and under one decoded smaller than its rect.
template <typename pixeltype>
static void PaintTile ( const Rtlbr<int> &tile_rect, const PwhlView<pixeltype> *pixels,
	Render<pixeltype> &render, bool clear )
{
	const auto painted = pixels && pixels->p;
	if (clear && ( !painted || pixels->w < width( tile_rect ) || pixels->h < height( tile_rect ) ))
		ClearTileArea( tile_rect, render );
	if (painted)
		BlitTileIntoRender( tile_rect, *pixels, render );
}


template <typename pixeltype>
static int CompositeLoadedTilesDirectToRender ( WsiLoader &loader,
	Ppyr &pyramid, const RenderPlan &plan,
	Render<pixeltype> &render, int &decode_ok, int &decode_fail )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size())
		return 0;
	auto &level = pyramid.levels[plan.level_no];
	const auto clear = TilesPartitionFrame( level, plan );
	if (!InitializeRender( plan, render, !clear ))
		return 0;

	for (auto tile_no : plan.tiles)
		WaitForTileIdle( level.tiles[tile_no] );
// Keep both compressed and existing decoded views stable through the batch.
	std::shared_lock tbf_guard( cache_lock( pyramid, false, plan.level_no ) );
	std::shared_lock pxb_guard( cache_lock( pyramid, true, plan.level_no ) );
	auto &compressed = cache_pool( pyramid, false, plan.level_no );
	auto &decoded_pool = cache_pool( pyramid, true, plan.level_no );
	const auto workers = DecodeWorkerCount( pyramid, plan.tiles.size() );
	struct DecodedTile {
		std::vector<pixeltype> pixels;
		Dim2D<int> dimensions{};
		bool ready = false;
		bool decoded = false;
		bool failed = false;
		TileDiag diag;
	};
// At most one scratch tile per worker, reused across batches. Painting stays
// in plan order, including already-resident tiles interspersed with new ones.
	auto batch = std::vector<DecodedTile>( workers );
	auto rendered_tiles = 0;
	for (size_t begin = 0; begin < plan.tiles.size(); begin += workers) {
		auto count = (unsigned)std::min<size_t>( workers, plan.tiles.size() - begin );
		loader.decode_pool_.run_batch( count, [&] ( unsigned worker ) {
			auto &result = batch[worker];
			result.ready = result.decoded = result.failed = false;
			auto tile_no = plan.tiles[begin + worker];
			result.diag = TileDiag{ plan.level_no, tile_no, 0.0 };
			auto &tile = level.tiles[tile_no];
			auto status = tile.sts.load( std::memory_order_acquire );
			if (status == TileSts::Ready && tile_has_pixels( tile )) {
				result.ready = true;
				return;
			}
// A zero-byte tile is empty space, not a failed decode.
			if (tile.fsz == 0)
				return;
			if (status == TileSts::Empty || tile.ninbuf < 0 || tile.bf_sz == 0) {
				result.failed = true;
				return;
			}
			result.decoded = DecodeTileToPixels(
				compressed.slot_ptr( tile.ninbuf ), tile.bf_sz, level.color_hint,
				result.pixels, result.dimensions, result.diag,
				*pyramid.unsupported_codec_ );
			result.failed = !result.decoded;
		} );
		for (unsigned i = 0; i < count; ++i) {
			auto &result = batch[i];
			auto &tile = level.tiles[plan.tiles[begin + i]];
			if (result.ready || result.decoded) {
				auto view = result.ready ? tile_pixels<pixeltype>( tile, level, decoded_pool )
					: PwhlView<pixeltype>( result.dimensions,
						result.dimensions.w, result.pixels.data() );
				PaintTile( tile.lr, &view, render, clear );
				++rendered_tiles;
				++decode_ok;
			} else {
				PaintTile<pixeltype>( tile.lr, nullptr, render, clear );
			}
			decode_fail += result.failed ? 1 : 0;
			pyramid.stats.decoded_tiles += result.decoded ? 1 : 0;
			pyramid.stats.tile_decode_ms += result.diag.decode_ms;
			if (result.diag.codec != ImgCodec::None)
				level.tile_codec = result.diag.codec;
		}
	}
	return rendered_tiles;
}


template <typename pixeltype>
static bool CompositeReadyTilesToRender ( Ppyr &pyramid,
	const RenderPlan &plan, Render<pixeltype> &render )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size())
		return false;
	auto &level = pyramid.levels[plan.level_no];
	std::shared_lock pxb_guard( cache_lock( pyramid, true, plan.level_no ) );
	auto &decoded_pool = cache_pool( pyramid, true, plan.level_no );
	const auto clear = TilesPartitionFrame( level, plan );
	if (!InitializeRender( plan, render, !clear ))
		return false;
	for (auto tile_no : plan.tiles) {
		auto &tile = level.tiles[tile_no];
		if (tile.sts.load() == TileSts::Ready && tile_has_pixels( tile )) {
			const auto view = tile_pixels<pixeltype>( tile, level, decoded_pool );
			PaintTile( tile.lr, &view, render, clear );
		} else {
			PaintTile<pixeltype>( tile.lr, nullptr, render, clear );
		}
	}
	return true;
}


// Tiles of the plan the file does not store at all - the unimaged parts of a
// sparse pyramid. They are not failures and never will be painted; their area is
// the render's background.
static int CountEmptyTiles ( const Ppyr &pyramid, const RenderPlan &plan )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size())
		return 0;

	const auto &level = pyramid.levels[plan.level_no];
	auto empty = 0;
	for (auto tile_no : plan.tiles) {
		if (level.tiles[tile_no].fsz == 0)
			empty++;
	}
	return empty;
}


static int CountReadyTiles ( const Ppyr &pyramid,
	const RenderPlan &plan )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size())
		return 0;

	const auto &level = pyramid.levels[plan.level_no];
	auto ready = 0;
	for (auto tile_no : plan.tiles) {
		const auto &tile = level.tiles[tile_no];
		if (tile.sts.load( std::memory_order_acquire ) == TileSts::Ready &&
			tile_has_pixels( tile ))
			ready++;
	}
	return ready;
}


// --- Sampling straight from cached tiles ---
//
// A caller that keeps no Render samples the frame once and drops it. When the
// level's tiles partition the sampled source box and all of them are cached
// decoded, each output pixel can be read from its one tile, and the frame is
// never built: at 2.5x on 960x8 tiles it was 3.2 Mpx written, then read again.
//
// Everything else - decoded caching off, overlap or gaps, a tile that fails or
// decodes short, a whole-slide request (its dark-level test reads a frame) -
// returns an empty raster and takes the frame path, which owns the
// incomplete-render accounting. Pixels are those the frame path produces.

static bool IsRenderPlanReasonableLocal ( const RenderPlan &plan );


template <typename pixeltype>
struct TileSource {
	const pixeltype *pixels = nullptr;
	int pitch = 0;
	Rtlbr<int> lr = {{0, 0}, {0, 0}};
};


// Output columns [begin, end) whose source pixels lie in one tile.
struct ColumnSegment {
	int begin = 0;
	int end = 0;
	size_t source = 0;
	bool consecutive = false;
};


// The tiles holding source row `sy` and the output columns each serves. In a
// partition those tiles span the frame's width, so they are the only tiles of
// every row in the intersection of their row ranges: that band is returned.
// Sources are in plan order, sorted by top edge, and rows only move down: the
// scan starts past the leading tiles that already ended (`scan_from`) and stops
// at the first tile starting below the row.
template <typename pixeltype>
static void BuildBandSegments ( const std::vector<TileSource<pixeltype>> &sources,
	const Rtlbr<int> &frame, const std::vector<int> &columns, int first, int last,
	int sy, std::vector<ColumnSegment> &segments, int &band_top, int &band_bottom,
	size_t &scan_from )
{
	segments.clear();
	band_top = frame.tl.y;
	band_bottom = frame.br.y;
	while (scan_from < sources.size() && sources[scan_from].lr.br.y <= sy)
		++scan_from;
	for (auto i = scan_from; i < sources.size(); ++i) {
		const auto &lr = sources[i].lr;
		if (lr.tl.y > sy)
			break;
		if (lr.br.y <= sy)
			continue;
		band_top = std::max( band_top, lr.tl.y );
		band_bottom = std::min( band_bottom, lr.br.y );
		const auto begin = (int)( std::lower_bound( columns.begin() + first, columns.begin() + last,
			std::max( lr.tl.x, frame.tl.x ) ) - columns.begin() );
		const auto end = (int)( std::lower_bound( columns.begin() + first, columns.begin() + last,
			std::min( lr.br.x, frame.br.x ) ) - columns.begin() );
		if (begin >= end)
			continue;
		auto consecutive = true;
		for (auto x = begin; x < end && consecutive; ++x)
			consecutive = columns[x] == columns[begin] + ( x - begin );
		segments.push_back( { begin, end, i, consecutive } );
	}
}


// What every row range of one tile-direct sampling shares.
template <typename pixeltype>
struct TileSampleJob {
	const std::vector<TileSource<pixeltype>> &sources;
	const Rtlbr<int> &frame;
	const std::vector<int> &columns;
	int first = 0;	// output columns [first, last) have their source in the frame
	int last = 0;
	const RasterMapping &mapping;
	float scale = 1.0f;
	Pwhl<pixeltype> &out;
};


// Sample output rows [begin, end). The tile band state is local, so disjoint
// row ranges can run on different workers; every pixel is a copy from its one
// tile, so the split never changes the result. `segments` is the worker's own
// list, reserved for every source tile before any pixel is written: nothing
// here allocates, so an output that is the caller's buffer is never left half
// written by a throw.
template <typename pixeltype>
static void SampleTileRows ( const TileSampleJob<pixeltype> &job, int begin, int end,
	std::vector<ColumnSegment> &segments )
{
	const auto &frame = job.frame;
	const auto &columns = job.columns;
	const auto width = job.out.w;
	const auto background = pixeltype{};
	auto band_top = 0;
	auto band_bottom = 0;
	auto scan_from = size_t{ 0 };
	for (auto y = begin; y < end; ++y) {
		auto *dp = job.out.rowp( y );
		const auto sy = job.mapping.y.source_pixel( y, job.scale );
		if (sy < frame.tl.y || sy >= frame.br.y || job.first == job.last) {
			std::fill_n( dp, width, background );
			continue;
		}
		std::fill_n( dp, job.first, background );
		std::fill_n( dp + job.last, width - job.last, background );
		if (sy < band_top || sy >= band_bottom)
			BuildBandSegments( job.sources, frame, columns, job.first, job.last, sy, segments,
				band_top, band_bottom, scan_from );
		for (const auto &segment : segments) {
			const auto &source = job.sources[segment.source];
			const auto *row = source.pixels + (size_t)( sy - source.lr.tl.y ) * (size_t)source.pitch;
			const auto offset = source.lr.tl.x;
			if (segment.consecutive) {
				std::copy_n( row + ( columns[segment.begin] - offset ), segment.end - segment.begin,
					dp + segment.begin );
				continue;
			}
			for (auto x = segment.begin; x < segment.end; ++x)
				dp[x] = row[columns[x] - offset];
		}
	}
}


// Output rows handed to one worker at a time, and the request size below which
// waking the pool costs more than the copy it shares.
constexpr auto kSampleBandRows = 64;
constexpr auto kParallelSamplePixels = size_t{ 256 } * 1024;


// `destination`, when it has the target's size, receives the pixels and the
// result is a non-owning raster over it. Every reason to decline is settled
// before the first pixel is written, so a declined render leaves it untouched.
template <typename pixeltype>
static Pwhl<pixeltype> SampleFromCachedTiles ( WsiLoader &loader, Ppyr &pyramid,
	const PlaneSel &sel, float microns_per_pixel, const Rtlbr<float> &subrect,
	const Dim2D<int> &target, Pwhl<pixeltype> *destination )
{
	if (!pyramid.use_pixbf || target.w <= 0 || target.h <= 0)
		return {};
	const auto level_no = SelectBestLevel( pyramid, sel, microns_per_pixel );
	if (level_no < 0)
		return {};
	auto &level = pyramid.levels[level_no];
	if (level.pixel_type != pixel_type_of<pixeltype>::value)
		return {};
	const auto *stack = find_plane( pyramid, sel );
	const auto is_finest = stack && !stack->ladder.empty() && level_no == stack->ladder.front();
	if (!is_finest && IsWholeWsiRequest<pixeltype>( subrect, pyramid.mrct ))
		return {};
	const auto render_t0 = std::chrono::steady_clock::now();
	const auto plan = BuildRenderPlan( pyramid, level_no, subrect, &target );
	if (!IsRenderPlanReasonableLocal( plan ) || plan.render_px.is_empty() ||
		!TilesPartitionFrame( level, plan ))
		return {};

	const auto pins = RenderTilePinGuard{ pyramid, plan };
	auto ok = 0;
	auto failed = 0;
	EnsureTilesLoaded<pixeltype>( loader, pyramid, plan, pyr::next_version(), ok, failed );
	if (failed > 0)
		return {};
	const auto decode_t0 = std::chrono::steady_clock::now();
	const auto decoded = EnsureTilesDecoded<pixeltype>( loader, pyramid, plan, ok, failed );
	pyramid.stats.decode_batch_ms += ElapsedMs( decode_t0 );
	if (!decoded || failed > 0)
		return {};

	std::shared_lock pxb_guard( cache_lock( pyramid, true, level_no ) );
	auto &decoded_pool = cache_pool( pyramid, true, level_no );
	auto sources = std::vector<TileSource<pixeltype>>( plan.tiles.size() );
	for (size_t i = 0; i < plan.tiles.size(); ++i) {
		const auto &tile = level.tiles[plan.tiles[i]];
		if (tile.sts.load( std::memory_order_acquire ) != TileSts::Ready || !tile_has_pixels( tile ))
			return {};
		const auto view = tile_pixels<pixeltype>( tile, level, decoded_pool );
		if (!view.p || view.w < width( tile.lr ) || view.h < height( tile.lr ))
			return {};
		sources[i] = { view.p, view.l, tile.lr };
	}

	const auto into = destination && destination->p &&
		destination->w == target.w && destination->h == target.h;
	auto out = into ? Pwhl<pixeltype>( target, destination->l, destination->p )
		: Pwhl<pixeltype>( target, uninit );
	const auto mapping = RasterMapping( subrect, target );
	const auto scale = level.metric > 0.0f ? level.metric : 1.0f;
	const auto &frame = plan.render_px;
	auto columns = std::vector<int>( target.w );
	for (int x = 0; x < target.w; ++x)
		columns[x] = mapping.x.source_pixel( x, scale );
	const auto first = (int)( std::lower_bound( columns.begin(), columns.end(), frame.tl.x ) - columns.begin() );
	const auto last = std::max( first,
		(int)( std::lower_bound( columns.begin(), columns.end(), frame.br.x ) - columns.begin() ) );
	const auto job = TileSampleJob<pixeltype>{ sources, frame, columns, first, last, mapping, scale, out };
//
// Row bands on the decode pool, decode_threads workers at most (the caller is
// one); a small request stays on the caller.
	const auto bands = ( target.h + kSampleBandRows - 1 ) / kSampleBandRows;
	const auto workers = (size_t)target.w * (size_t)target.h >= kParallelSamplePixels
		? DecodeWorkerCount( pyramid, (size_t)bands ) : 1u;
	auto segments = std::vector<std::vector<ColumnSegment>>( workers );
	for (auto &list : segments)
		list.reserve( sources.size() );
	auto next = std::atomic<int>{ 0 };
	loader.decode_pool_.run_batch( workers, [&] ( unsigned worker ) {
		for (auto band = next.fetch_add( 1 ); band < bands; band = next.fetch_add( 1 ))
			SampleTileRows( job, band * kSampleBandRows,
				std::min( target.h, ( band + 1 ) * kSampleBandRows ), segments[worker] );
	} );
	pyramid.stats.renders++;
	pyramid.stats.requested_tiles += (uint64_t)plan.tiles.size();
	pyramid.stats.last_render_level = level_no;
	pyramid.stats.render_ms += ElapsedMs( render_t0 );
	return out;
}


// --- Generate ---

// Diagnostics: count of in-flight Generate calls across all threads (see
// wsi_generate_in_flight in wsi.h). A RAII guard keeps it correct across the
// multiple return points below.
namespace {
std::atomic<int> g_wsi_generate_in_flight{ 0 };
struct WsiGenerateInFlightGuard {
	WsiGenerateInFlightGuard () {
		g_wsi_generate_in_flight.fetch_add( 1, std::memory_order_relaxed );
	}
	~WsiGenerateInFlightGuard () {
		g_wsi_generate_in_flight.fetch_sub( 1, std::memory_order_relaxed );
	}
};
} // namespace


int wsi_generate_in_flight () noexcept
{
	return g_wsi_generate_in_flight.load( std::memory_order_relaxed );
}


template <typename pixeltype>
Pwhl<pixeltype> WsiLoader::Generate (
	Ppyr &pyramid,
	Render<pixeltype> *render,
	float microns_per_pixel,
	const Rtlbr<float> &subrect,
	const Dim2D<int> *target_size,
	const PlaneSel &sel,
	Pwhl<pixeltype> *destination )
{
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	auto in_flight_guard = WsiGenerateInFlightGuard{};
	pyramid.stats.last_render_level = -1;
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"GENERATE-ENTRY: mpp=%f rect=[%f,%f]-[%f,%f] whole=%d render=%p levels=%d\n",
			microns_per_pixel,
			subrect.tl.x, subrect.tl.y, subrect.br.x, subrect.br.y,
			IsWholeWsiRequest<pixeltype>( subrect, pyramid.mrct ) ? 1 : 0,
			static_cast<void *>( render ),
			static_cast<int>( pyramid.levels.size() ) );
		fflush( pyr::debug_file() );
	}

	if (subrect.is_empty())
		return {};
	const auto safe_mpp = microns_per_pixel > 0.0f ? microns_per_pixel : 1.0f;
	const auto target_w = ( target_size && target_size->w > 0 ) ? target_size->w
		: raster_span_from_metric( subrect.br.x - subrect.tl.x, safe_mpp );
	const auto target_h = ( target_size && target_size->h > 0 ) ? target_size->h
		: raster_span_from_metric( subrect.br.y - subrect.tl.y, safe_mpp );
	const auto selected = SelectBestLevel( pyramid, sel, microns_per_pixel );
	if (selected < 0)
		return {};
	const auto &selected_level = pyramid.levels[selected];
	const auto scale = selected_level.metric > 0.0f ? selected_level.metric : 1.0f;
	const auto bounds = Rtlbr<float>{
		{ selected_level.bounds.tl.x * scale, selected_level.bounds.tl.y * scale },
		{ selected_level.bounds.br.x * scale, selected_level.bounds.br.y * scale } };
	if (IntersectRect( subrect, bounds ).is_empty())
		return Pwhl<pixeltype>( { target_w, target_h } );
	const auto target = Dim2D<int>{ target_w, target_h };
	if (!render) {
		auto direct = SampleFromCachedTiles<pixeltype>( *this, pyramid, sel,
			microns_per_pixel, subrect, target, destination );
		if (direct.p)
			return direct;
	}

	Render<pixeltype> local_render;
	auto &active_render = render ? *render : local_render;
// Without a caller's Render nothing reuses the frame: size it to the samples.
	auto span = GenerateSpan( pyramid, active_render, microns_per_pixel, subrect,
		sel, render ? nullptr : &target );
	if (!span.p || span.w <= 0 || span.h <= 0) {
		if (pyr::debug_file()) {
			fprintf( pyr::debug_file(),
				"GENERATE-EXIT: empty result mpp=%f rect=[%f,%f]-[%f,%f]\n",
				microns_per_pixel,
				subrect.tl.x, subrect.tl.y, subrect.br.x, subrect.br.y );
			fflush( pyr::debug_file() );
		}
		return Pwhl<pixeltype>();
	}

// A partial decode invalidates the cache key (level_no = -1), but still returns
// its available pixels. Whole-slide fallback may also have selected a new level.
	const auto source_level = active_render.level_no >= 0 ? active_render.level_no
		: SelectBestLevel( pyramid, sel, microns_per_pixel );
	if (source_level < 0)
		return {};
// A 1:1 request on the frame's own level: the frame already is the answer.
	if (!render && active_render.level_no == source_level &&
		IsIdentitySample( active_render, pyramid.levels[source_level], subrect, target ))
		return std::move( local_render.image );
	auto out = SampleRequestedRaster( active_render,
		pyramid.levels[source_level], subrect, target );
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"GENERATE-EXIT: span=%dx%d out=%dx%d mpp=%f rect=[%f,%f]-[%f,%f]\n",
			span.w, span.h, out.w, out.h, microns_per_pixel,
			subrect.tl.x, subrect.tl.y, subrect.br.x, subrect.br.y );
		fflush( pyr::debug_file() );
	}
	return out;
}


// Owned source-grid pixels of the level Generate would select, widened by one
// interpolation neighbour on each side and clipped to the level. `coverage`
// receives the metric rectangle the raster actually spans after level selection
// and whole-slide fallback; nothing is resampled.
template <typename pixeltype>
Pwhl<pixeltype> WsiLoader::GenerateSourceGrid ( Ppyr &pyramid, float microns_per_pixel,
	const Rtlbr<float> &subrect, Rtlbr<float> *coverage, const PlaneSel &sel )
{
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	if (!coverage)
		return {};
	*coverage = {{0, 0}, {0, 0}};
	auto in_flight_guard = WsiGenerateInFlightGuard{};
	if (subrect.is_empty())
		return {};
	const auto selected = SelectBestLevel( pyramid, sel, microns_per_pixel );
	if (selected < 0)
		return {};
	const auto &selected_level = pyramid.levels[selected];
	const auto scale = selected_level.metric > 0.0f ? selected_level.metric : 1.0f;
	const auto bounds = Rtlbr<float>{
		{ selected_level.bounds.tl.x * scale, selected_level.bounds.tl.y * scale },
		{ selected_level.bounds.br.x * scale, selected_level.bounds.br.y * scale } };
	if (IntersectRect( subrect, bounds ).is_empty()) {
		const auto box = bbox_from_metric( subrect, scale );
		*coverage = { { box.tl.x * scale, box.tl.y * scale },
			{ box.br.x * scale, box.br.y * scale } };
		return Pwhl<pixeltype>( { box.br.x - box.tl.x, box.br.y - box.tl.y } );
	}

	Render<pixeltype> render;
	auto span = GenerateSpan( pyramid, render, microns_per_pixel, subrect, sel,
		nullptr, kSourceGridInterpolationBorder );
	if (!span.p || span.w <= 0 || span.h <= 0)
		return {};
	const auto source_level = render.level_no >= 0 ? render.level_no
		: SelectBestLevel( pyramid, sel, microns_per_pixel );
	if (source_level < 0)
		return {};
	const auto &level = pyramid.levels[source_level];
	const auto box = SourceGridBox( level, subrect, kSourceGridInterpolationBorder );
	if (box.is_empty())
		return {};
	auto out = Pwhl<pixeltype>( { box.br.x - box.tl.x, box.br.y - box.tl.y } );
	const auto available = IntersectRect( box, render.lr );
	for (auto y = available.tl.y; y < available.br.y; ++y)
		std::copy_n( render.image.rowp( y - render.lr.tl.y ) +
			available.tl.x - render.lr.tl.x, available.br.x - available.tl.x,
			out.rowp( y - box.tl.y ) + available.tl.x - box.tl.x );
	const auto actual_scale = level.metric > 0.0f ? level.metric : 1.0f;
	*coverage = { { box.tl.x * actual_scale, box.tl.y * actual_scale },
		{ box.br.x * actual_scale, box.br.y * actual_scale } };
	return out;
}


template <typename pixeltype>
static void LogGenerateSpanEntry ( const Render<pixeltype> &render,
	float microns_per_pixel,
	const Rtlbr<float> &subrect,
	bool whole_wsi_request )
{
	if (!pyr::debug_file())
		return;
	fprintf( pyr::debug_file(),
		"GENERATE-SPAN-ENTRY: mpp=%f rect=[%f,%f]-[%f,%f] whole=%d render_image=%dx%d bound=%p\n",
		microns_per_pixel, subrect.tl.x, subrect.tl.y,
		subrect.br.x, subrect.br.y, whole_wsi_request ? 1 : 0,
		render.image.w, render.image.h, static_cast<void *>( render.pyr ) );
	fflush( pyr::debug_file() );
}


static void LogGenerateFailures ( int level_index,
	float microns_per_pixel,
	const RenderPlan &plan,
	int load_ok,
	int load_fail,
	int decode_ok,
	int decode_fail,
	int ready_tiles,
	bool decoded )
{
	if (load_fail <= 0 && decode_fail <= 0)
		return;
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"GEN-FAIL: lvl=%d mpp=%f tiles=%zu load_ok=%d load_fail=%d decode_ok=%d decode_fail=%d ready=%d decoded=%d\n",
			level_index, microns_per_pixel, plan.tiles.size(), load_ok,
			load_fail, decode_ok, decode_fail, ready_tiles, decoded ? 1 : 0 );
		fprintf( pyr::debug_file(),
			"GEN-FAIL: request_px=[%d,%d]-[%d,%d] render_px=[%d,%d]-[%d,%d]\n",
			plan.request_px.tl.x, plan.request_px.tl.y,
			plan.request_px.br.x, plan.request_px.br.y,
			plan.render_px.tl.x, plan.render_px.tl.y,
			plan.render_px.br.x, plan.render_px.br.y );
		fflush( pyr::debug_file() );
	}
	pyr::plog(
		"wsi generate fail: lvl=%d mpp=%.6f tiles=%zu load_ok=%d load_fail=%d decode_ok=%d decode_fail=%d ready=%d decoded=%d request_px=[%d,%d]-[%d,%d] render_px=[%d,%d]-[%d,%d]\n",
		level_index, microns_per_pixel, plan.tiles.size(), load_ok,
		load_fail, decode_ok, decode_fail, ready_tiles, decoded ? 1 : 0,
		plan.request_px.tl.x, plan.request_px.tl.y,
		plan.request_px.br.x, plan.request_px.br.y,
		plan.render_px.tl.x, plan.render_px.tl.y,
		plan.render_px.br.x, plan.render_px.br.y );
}


struct GenerateTileStatsLocal {
	int load_ok = 0;
	int load_fail = 0;
	int decode_ok = 0;
	int decode_fail = 0;
};


static bool IsRenderPlanReasonableLocal ( const RenderPlan &plan )
{
	constexpr auto kMaxReasonableCoordinate = 200000;
	const auto &request = plan.request_px;
	if (request.tl.x < -kMaxReasonableCoordinate ||
		request.tl.y < -kMaxReasonableCoordinate ||
		request.br.x > kMaxReasonableCoordinate ||
		request.br.y > kMaxReasonableCoordinate)
	{
		if (pyr::debug_file()) {
			fprintf( pyr::debug_file(),
				"ERROR: Unreasonable pixel coordinates: [%d,%d]-[%d,%d]\n",
				request.tl.x, request.tl.y, request.br.x, request.br.y );
			fflush( pyr::debug_file() );
		}
		return false;
	}
	return request.br.x > request.tl.x && request.br.y > request.tl.y;
}


template <typename pixeltype>
static bool PrepareRenderTilesLocal ( WsiLoader &loader,
	Ppyr &pyramid,
	Render<pixeltype> &render,
	const RenderPlan &plan,
	int level_index,
	float microns_per_pixel,
	const PlaneSel &sel,
	GenerateTileStatsLocal *stats )
{
	auto need_decode = !render.matches(
		sel, level_index, plan.render_px, plan.tiles );
	if (!need_decode)
		return true;

// Sparse formats omit unacquired frames. A valid request inside the declared
// matrix can therefore have no tiles at all, and still has a black raster.
	if (plan.tiles.empty())
		return InitializeRender( plan, render, true );

	const auto pins = RenderTilePinGuard{ pyramid, plan };
	EnsureTilesLoaded<pixeltype>( loader, pyramid, plan, pyr::next_version(),
		stats->load_ok, stats->load_fail );

	auto decode_batch_t0 = std::chrono::steady_clock::now();
	auto decode_complete = true;
	auto decoded = false;
	auto ready_tiles = 0;
	if (pyramid.use_pixbf) {
		decode_complete = EnsureTilesDecoded<pixeltype>( loader, pyramid, plan,
			stats->decode_ok, stats->decode_fail );
		pyramid.stats.decode_batch_ms += ElapsedMs( decode_batch_t0 );
		decoded = CompositeReadyTilesToRender( pyramid, plan, render );
		ready_tiles = CountReadyTiles( pyramid, plan );
	}
	else {
		ready_tiles = CompositeLoadedTilesDirectToRender(
			loader, pyramid, plan, render,
			stats->decode_ok, stats->decode_fail );
		pyramid.stats.decode_batch_ms += ElapsedMs( decode_batch_t0 );
		decode_complete = stats->decode_fail == 0;
		decoded = ready_tiles > 0;
	}
	if (!pyramid.deferred_tiles || stats->decode_fail > stats->load_fail)
		LogGenerateFailures( level_index, microns_per_pixel, plan,
			stats->load_ok, stats->load_fail,
			stats->decode_ok, stats->decode_fail,
			ready_tiles, decoded );
	if (decoded && ( decode_complete || ready_tiles > 0 ))
		return true;

//
// Every tile of this plan is one the file does not store: a view over unimaged
// stage, which on a sparse mosaic - under a fifth of a converted acquisition is
// imaged - is an ordinary place to be, not an error. Nothing was painted and
// nothing failed; the frame IS the background it was allocated with.
//
// Judged by `decoded` alone this reads as a failed render, so the level is
// rejected, every coarser one is tried and rejected in turn, and the caller is
// handed an empty raster - a black viewport over perfectly normal empty slide.
	if (decode_complete && !plan.tiles.empty() &&
		CountEmptyTiles( pyramid, plan ) == (int)plan.tiles.size() &&
		render.image.is_set())
	{
		return true;
	}

	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"GEN-NULL: lvl=%d mpp=%f tiles=%zu decode_complete=%d ready=%d\n",
			level_index, microns_per_pixel, plan.tiles.size(),
			decode_complete ? 1 : 0, ready_tiles );
		fflush( pyr::debug_file() );
	}
	return false;
}


template <typename pixeltype>
Pwhl<pixeltype> WsiLoader::GenerateSpan (
	Ppyr &pyramid,
	Render<pixeltype> &render,
	float microns_per_pixel,
	const Rtlbr<float> &subrect,
	const PlaneSel &sel,
	const Dim2D<int> *sampled,
	int source_border )
{
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	auto render_t0 = std::chrono::steady_clock::now();
	pyramid.stats.renders++;
	const auto whole_wsi_request = IsWholeWsiRequest<pixeltype>( subrect, pyramid.mrct );
	LogGenerateSpanEntry(
		render, microns_per_pixel, subrect, whole_wsi_request );
	auto *active_render = &render;
	active_render->bind( &pyramid );
	active_render->plane = sel;

	while (true) {
		auto best_lvl_idx = SelectBestLevel( pyramid, sel, microns_per_pixel );
		if (best_lvl_idx < 0) {
			pyramid.stats.render_ms += ElapsedMs( render_t0 );
			return Pwhl<pixeltype>();
		}

		if (pyr::debug_file()) {
			fprintf( pyr::debug_file(), "GENERATE: mpp=%f, levels=%d\n", microns_per_pixel, (int)pyramid.levels.size() );
			fprintf( pyr::debug_file(), "GENERATE: Selected level %d with metric %f\n",
				best_lvl_idx, pyramid.levels[best_lvl_idx].metric );
			fflush( pyr::debug_file() );
		}

		auto &level = pyramid.levels[best_lvl_idx];
		const auto *stack = find_plane( pyramid, sel );
// Each plane's finest level is its final fallback, wherever it is in the store.
		const auto is_finest = stack && !stack->ladder.empty() && best_lvl_idx == stack->ladder.front();
		if (level.pixel_type != pixel_type_of<pixeltype>::value) {
			pyr::plog_err(
				"wsi generate: plane '%s' level %d holds %s, asked for %s\n",
				sel.channel.c_str(), best_lvl_idx,
				pixel_type_name( level.pixel_type ),
				pixel_type_name( pixel_type_of<pixeltype>::value ) );
			pyramid.stats.render_ms += ElapsedMs( render_t0 );
			return Pwhl<pixeltype>();
		}

		auto plan_t0 = std::chrono::steady_clock::now();
		auto plan = BuildRenderPlan( pyramid, best_lvl_idx, subrect, sampled, source_border );
		pyramid.stats.render_plan_ms += ElapsedMs( plan_t0 );
		auto px_l = plan.request_px.tl.x;
		auto px_t = plan.request_px.tl.y;
		auto px_r = plan.request_px.br.x;
		auto px_b = plan.request_px.br.y;

		auto out_w = px_r - px_l;
		auto out_h = px_b - px_t;
		if (!IsRenderPlanReasonableLocal( plan )) {
			pyramid.stats.render_ms += ElapsedMs( render_t0 );
			return Pwhl<pixeltype>();
		}
		pyramid.stats.requested_tiles += (uint64_t)plan.tiles.size();
		pyramid.stats.last_render_level = best_lvl_idx;

		static auto genDbg = std::atomic<int>{ 0 };
		auto dbg = (genDbg.fetch_add( 1, std::memory_order_relaxed ) < 10);
		if (dbg && pyr::debug_file()) {
			fprintf( pyr::debug_file(), "GEN: lvl=%d metric=%f mpp=%f out=%dx%d\n",
				best_lvl_idx, level.metric, microns_per_pixel, out_w, out_h );
			fprintf( pyr::debug_file(), "GEN: px=[%d,%d]-[%d,%d]\n", px_l, px_t, px_r, px_b );
			fprintf( pyr::debug_file(), "GEN: render_px=[%d,%d]-[%d,%d]\n",
				plan.render_px.tl.x, plan.render_px.tl.y,
				plan.render_px.br.x, plan.render_px.br.y );
			fprintf( pyr::debug_file(), "GEN: tiles=%zu tile_w=%d tile_h=%d\n",
				plan.tiles.size(), level.tile_w, level.tile_h );
			fflush( pyr::debug_file() );
		}

		auto tile_stats = GenerateTileStatsLocal{};
		if (!PrepareRenderTilesLocal( *this, pyramid, *active_render,
				plan, best_lvl_idx, microns_per_pixel, sel, &tile_stats ))
		{
			if (( tile_stats.load_fail == 0 && tile_stats.decode_fail == 0 ) ||
				!active_render->image.is_set()) {
				pyramid.stats.render_ms += ElapsedMs( render_t0 );
				return Pwhl<pixeltype>();
			}
		}

		if (dbg && pyr::debug_file()) {
			fprintf( pyr::debug_file(), "GEN: LOAD ok=%d fail=%d | DECODE ok=%d fail=%d\n",
				tile_stats.load_ok, tile_stats.load_fail,
				tile_stats.decode_ok, tile_stats.decode_fail );
			fflush( pyr::debug_file() );
		}

		if (tile_stats.load_fail > 0 || tile_stats.decode_fail > 0) {
			active_render->level_no = -1;
//
// Tiles that never arrived leave their area unpainted, and the frame is returned
// regardless - which for an application that measures what it renders is a wrong
// answer, not a slow one. Count it and say so at error level: this used to be
// visible only as a per-tile line buried in the log.
			auto missing = (uint64_t)( tile_stats.load_fail +
				tile_stats.decode_fail );
			pyramid.stats.dropped_tiles += missing;
			pyramid.stats.incomplete_renders++;
			if (const auto *reason = img_codec_unavailable_reason( pyramid.unsupported_codec_->load() )) {
				pyr::plog_err( "wsi generate INCOMPLETE: %s\n", reason );
			} else if (!pyramid.deferred_tiles || tile_stats.decode_fail > tile_stats.load_fail) {
				pyr::plog_err(
					"wsi generate INCOMPLETE: %llu of %zu tiles missing at level %d "
					"(mpp=%.4f) - the frame has unpainted areas; "
					"raise the tile/pixel cache caps or use a coarser level\n",
					(unsigned long long)missing, plan.tiles.size(),
					best_lvl_idx, microns_per_pixel );
			}
		}

		if (whole_wsi_request && !is_finest &&
			( IsAllBlack( active_render->image ) ||
			  IsSparseDarkWholeWsiFrame(
				  active_render->image, level.max_pixel_value ) )) {
			level.reject_whole_wsi_black = true;
			active_render->clear();
			if (pyr::debug_file()) {
				fprintf( pyr::debug_file(), "GEN: rejecting whole-slide sparse/dark level %d (metric=%f)\n",
					best_lvl_idx, level.metric );
				fflush( pyr::debug_file() );
			}
			continue;
		}

		auto span = active_render->image.span( {
			{px_l - active_render->lr.tl.x, px_t - active_render->lr.tl.y},
			{px_r - active_render->lr.tl.x, px_b - active_render->lr.tl.y}
		} );
		if (!whole_wsi_request || is_finest ||
			( !IsAllBlack( span ) &&
			  !IsSparseDarkWholeWsiFrame( span, level.max_pixel_value ) )) {
			pyramid.stats.render_ms += ElapsedMs( render_t0 );
			return span;
		}

		level.reject_whole_wsi_black = true;
		active_render->clear();
		if (pyr::debug_file()) {
			fprintf( pyr::debug_file(), "GEN: rejecting whole-slide sparse/dark level %d (metric=%f)\n",
				best_lvl_idx, level.metric );
			fflush( pyr::debug_file() );
		}
	}
}


// Instantiated once per pixel type the application renders with, as in wsi.cpp.

template Pwhl<Rgb> WsiLoader::Generate<Rgb> ( Ppyr &, Render<Rgb> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel &, Pwhl<Rgb> * );
template Pwhl<Rgb> WsiLoader::GenerateSpan<Rgb> ( Ppyr &, Render<Rgb> &, float,
	const Rtlbr<float> &, const PlaneSel &, const Dim2D<int> *, int );
template Pwhl<Rgb> WsiLoader::GenerateSourceGrid<Rgb> ( Ppyr &, float,
	const Rtlbr<float> &, Rtlbr<float> *, const PlaneSel & );
template Pwhl<uint8_t> WsiLoader::Generate<uint8_t> ( Ppyr &, Render<uint8_t> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel &, Pwhl<uint8_t> * );
template Pwhl<uint8_t> WsiLoader::GenerateSpan<uint8_t> ( Ppyr &, Render<uint8_t> &, float,
	const Rtlbr<float> &, const PlaneSel &, const Dim2D<int> *, int );
template Pwhl<uint8_t> WsiLoader::GenerateSourceGrid<uint8_t> ( Ppyr &, float,
	const Rtlbr<float> &, Rtlbr<float> *, const PlaneSel & );
template Pwhl<uint16_t> WsiLoader::Generate<uint16_t> ( Ppyr &, Render<uint16_t> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel &, Pwhl<uint16_t> * );
template Pwhl<uint16_t> WsiLoader::GenerateSpan<uint16_t> ( Ppyr &, Render<uint16_t> &, float,
	const Rtlbr<float> &, const PlaneSel &, const Dim2D<int> *, int );
template Pwhl<uint16_t> WsiLoader::GenerateSourceGrid<uint16_t> ( Ppyr &, float,
	const Rtlbr<float> &, Rtlbr<float> *, const PlaneSel & );
template Pwhl<float> WsiLoader::Generate<float> ( Ppyr &, Render<float> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel &, Pwhl<float> * );
template Pwhl<float> WsiLoader::GenerateSpan<float> ( Ppyr &, Render<float> &, float,
	const Rtlbr<float> &, const PlaneSel &, const Dim2D<int> *, int );
template Pwhl<float> WsiLoader::GenerateSourceGrid<float> ( Ppyr &, float,
	const Rtlbr<float> &, Rtlbr<float> *, const PlaneSel & );
template Pwhl<Rgb16> WsiLoader::Generate<Rgb16> ( Ppyr &, Render<Rgb16> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel &, Pwhl<Rgb16> * );
template Pwhl<Rgb16> WsiLoader::GenerateSpan<Rgb16> ( Ppyr &, Render<Rgb16> &, float,
	const Rtlbr<float> &, const PlaneSel &, const Dim2D<int> *, int );
template Pwhl<Rgb16> WsiLoader::GenerateSourceGrid<Rgb16> ( Ppyr &, float,
	const Rtlbr<float> &, Rtlbr<float> *, const PlaneSel & );

} // namespace priml
