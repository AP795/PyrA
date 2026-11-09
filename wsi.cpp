#include "img_codec.h"
#include "wsi.h"
#include "wsi_formats.h"

#include "pyr_rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace priml {

const char *JpegDecoderName () {
	return "jpeglib";
}


void LogPixelMetricInequality ( const std::string &filename,
	const Ppyr &pyramid )
{
	const auto &metric = pyramid.source_pixel_metric;
	if (!( metric.x > 0.0f ) || !( metric.y > 0.0f ) || metric.x == metric.y)
		return;

	auto difference_pct = 100.0 * std::abs(
		(double)metric.y / (double)metric.x - 1.0 );
	pyr::plog_err(
		"wsi loader: %s has non-square pixels: x=%.9g um/px, "
		"y=%.9g um/px (difference %.6f%%)\n",
		filename.c_str(), (double)metric.x, (double)metric.y,
		difference_pct );
}


bool ResolveWsiPixelTypes ( Ppyr &pyramid, LevelPixelType fallback )
{
// Integer monochrome and integer colour opens each retain both of their storage
// widths. Across families the typed-open contract holds: an RGB request cannot
// open a gray-only slide.
	std::erase_if( pyramid.levels, [&] ( const Level &level ) {
		return level.pixel_type_declared && level.pixel_type != fallback &&
			!( is_integer_gray( fallback ) && is_integer_gray( level.pixel_type ) ) &&
			!( is_integer_rgb( fallback ) && is_integer_rgb( level.pixel_type ) );
	} );
	for (auto &level : pyramid.levels)
		if (!level.pixel_type_declared)
			level.pixel_type = fallback;
	pyramid.planes.clear();
	return !pyramid.levels.empty();
}


static double ElapsedMs ( const std::chrono::steady_clock::time_point &t0 ) {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0 ).count();
}

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


static TileSts WaitForTileIdle ( Tile &tile ) {
	while (true) {
		auto sts = tile.sts.load( std::memory_order_acquire );
		if (sts != TileSts::Loading && sts != TileSts::Decoding)
			return sts;
		std::this_thread::yield();
	}
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


static const char *TileStsName ( TileSts sts ) noexcept
{
	switch (sts) {
	case TileSts::Empty: return "Empty";
	case TileSts::Loading: return "Loading";
	case TileSts::Loaded: return "Loaded";
	case TileSts::Decoding: return "Decoding";
	case TileSts::Ready: return "Ready";
	}
	return "Unknown";
}


static void LogTileFailure ( const char *stage, int level_no, int tile_no,
	const Tile &tile, const char *reason )
{
	auto sts = tile.sts.load( std::memory_order_relaxed );
	auto pins = tile.pin_count.load( std::memory_order_relaxed );
	pyr::plog(
		"wsi tile fail: stage=%s lvl=%d tile=%d sts=%s rect=[%d,%d]-[%d,%d] "
		"foff=%zu fsz=%zu bf_sz=%zu ninbuf=%d nixbuf=%d pins=%d reason=%s\n",
		stage ? stage : "unknown",
		level_no, tile_no, TileStsName( sts ),
		tile.lr.tl.x, tile.lr.tl.y, tile.lr.br.x, tile.lr.br.y,
		tile.foff, tile.fsz, tile.bf_sz, tile.ninbuf, tile.nixbuf, pins,
		(reason && *reason) ? reason : "unknown" );
}


template <typename pixeltype>
struct TilePoolFailureLogLocal {
	const char *stage = nullptr;
	int level_no = -1;
	int tile_no = -1;
	const Tile &tile;
	const Ppyr &pyramid;
	size_t pool_alloc = 0;
	int used_slots = 0;
	int num_slots = 0;
	size_t cap_bytes = 0;
	const char *reason = nullptr;
};


template <typename pixeltype>
static void LogTilePoolFailure (
	const TilePoolFailureLogLocal<pixeltype> &failure )
{
	auto total_alloc = size_t{0};
	for (const auto &pyr_level : failure.pyramid.levels) {
		total_alloc += pyr_level.tbf.allocated_bytes();
		if (pyr_level.pix)
			total_alloc += pyr_level.pix->allocated_bytes();
	}

	pyr::plog(
		"wsi tile fail: stage=%s lvl=%d tile=%d pool_used=%d/%d pool_alloc=%zu "
		"pool_cap=%zu ram_cap=%zu total_alloc=%zu levels=%zu reason=%s\n",
		failure.stage ? failure.stage : "unknown",
		failure.level_no, failure.tile_no,
		failure.used_slots, failure.num_slots, failure.pool_alloc,
		failure.cap_bytes, failure.pyramid.ram_cap_bytes,
		total_alloc, failure.pyramid.levels.size(),
		(failure.reason && *failure.reason) ? failure.reason : "unknown" );
//
// Where the budget actually went, per level. A whole-pyramid cap spent by
// per-level pools is impossible to reason about from one total: a multi-plane
// slide has a level ladder per channel, all competing for the same bytes, and
// which of them is holding them is the entire diagnosis.
	for (auto li = 0; li < (int)failure.pyramid.levels.size(); ++li) {
		const auto &pyr_level = failure.pyramid.levels[li];
		auto tbf = pyr_level.tbf.allocated_bytes();
		auto pxb = pyr_level.pix ? pyr_level.pix->allocated_bytes() : size_t{ 0 };
		if (tbf == 0 && pxb == 0)
			continue;
		pyr::plog( "wsi tile fail:   L%d plane='%s' tbf=%zu pxb=%zu "
			"pxb_slots=%d/%d\n",
			li, pyr_level.plane.channel.c_str(), tbf, pxb,
			pyr_level.pix ? pyr_level.pix->used_slots() : 0,
			pyr_level.pix ? pyr_level.pix->num_slots() : 0 );
	}
	LogTileFailure( failure.stage, failure.level_no, failure.tile_no,
		failure.tile, failure.reason );
}


static void LogReadFailure ( size_t file_offset, size_t count,
	unsigned worker_idx, const char *reason )
{
	pyr::plog(
		"wsi read fail: worker=%u offset=%zu bytes=%zu reason=%s\n",
		worker_idx, file_offset, count,
		(reason && *reason) ? reason : "unknown" );
}

struct RenderPlan {
	int level_no = -1;
	Rtlbr<int> request_px = {{0, 0}, {0, 0}};
	Rtlbr<int> render_px = {{0, 0}, {0, 0}};
	Rtlbr<float> render_mrct = {{0, 0}, {0, 0}};
	std::vector<int> tiles;
};


// Sample the requested metric rectangle, not the clipped source extent. Missing
// source pixels stay background; clipping must never stretch the imaged part.
template <typename pixeltype>
static Pwhl<pixeltype> SampleRequestedRaster ( const Render<pixeltype> &render,
	const Level &level, const Rtlbr<float> &subrect, const Dim2D<int> &dimensions )
{
	auto dst = Pwhl<pixeltype>( dimensions );
	const auto mapping = RasterMapping( subrect, dimensions );
	const auto source = IntersectRect( render.lr, level.bounds );
	const auto scale = level.metric > 0.0f ? level.metric : 1.0f;
	auto columns = std::vector<int>( dimensions.w );
	for (int x = 0; x < dimensions.w; ++x)
		columns[x] = mapping.x.source_pixel( x, scale );
	for (int y = 0; y < dimensions.h; ++y) {
		const auto sy = mapping.y.source_pixel( y, scale );
		if (sy < source.tl.y || sy >= source.br.y)
			continue;
		const auto *sp = render.image.rowp( sy - render.lr.tl.y );
		auto *dp = dst.rowp( y );
		for (int x = 0; x < dimensions.w; ++x) {
			const auto sx = columns[x];
			if (sx >= source.tl.x && sx < source.br.x)
				dp[x] = sp[sx - render.lr.tl.x];
		}
	}
	return dst;
}


static RenderPlan BuildRenderPlan ( const Ppyr &pyramid,
	int level_no, const Rtlbr<float> &subrect )
{
	RenderPlan plan;
	if (level_no < 0 || level_no >= (int)pyramid.levels.size())
		return plan;

	const auto &level = pyramid.levels[level_no];
	const auto scale = level.metric > 0.0f ? level.metric : 1.0f;
	plan.level_no = level_no;
	plan.request_px = bbox_from_metric( subrect, scale );

	if (level.tiles.empty())
		return plan;

// GenerateSpan exposes source coverage only. Generate maps that coverage into
// the original requested rectangle, preserving its origin and any overhang.
	plan.request_px = IntersectRect( plan.request_px, level.bounds );
	if (plan.request_px.is_empty())
		return plan;

	select_tiles_for_rect( level, plan.request_px, plan.tiles, plan.render_px );
	if (plan.tiles.empty())
		return plan;
//
// The frame covers the request too, not only the tiles found for it. On a sparse
// level - a gallery's gutters, a CZI mosaic's unscanned stage - a request reaching
// past its tiles into area no tile stores would otherwise be cut from a frame that
// does not hold it, and come back empty rather than as tissue plus background.
// On a full grid the tiles already cover the request and nothing changes.
	plan.render_px = UnionRect( plan.render_px, plan.request_px );

	plan.render_mrct = {
		{pyramid.mrct.tl.x + plan.render_px.tl.x * scale,
		 pyramid.mrct.tl.y + plan.render_px.tl.y * scale},
		{pyramid.mrct.tl.x + plan.render_px.br.x * scale,
		 pyramid.mrct.tl.y + plan.render_px.br.y * scale}
	};
	return plan;
}


template <typename pixeltype>
static bool EnsureTilesLoaded ( WsiLoader &loader,
	Ppyr &pyramid, const RenderPlan &plan,
	version_t use_stamp, int &load_ok, int &load_fail )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size())
		return false;

	auto &level = pyramid.levels[plan.level_no];

// Collect tiles that need loading, stamp all tiles
	std::vector<int> to_load;
	to_load.reserve( plan.tiles.size() );
	for (auto tile_no : plan.tiles) {
		auto &tile = level.tiles[tile_no];
		tile.last_use.store( use_stamp, std::memory_order_relaxed );
		auto sts = tile.sts.load( std::memory_order_acquire );
		if (sts == TileSts::Loading || sts == TileSts::Decoding)
			sts = WaitForTileIdle( tile );
		if (sts == TileSts::Empty)
			to_load.push_back( tile_no );
	}

	if (to_load.empty())
		return true;

// Single tile — load directly without pool overhead
	if (to_load.size() == 1) {
		auto load_t0 = std::chrono::steady_clock::now();
		auto ok = loader.LoadTile( pyramid, plan.level_no, to_load[0] );
		pyramid.stats.tile_load_ms += ElapsedMs( load_t0 );
		if (ok) { pyramid.stats.tile_loads++; load_ok++; }
		else load_fail++;
		return true;
	}

// Serial load — device-level I/O serialises parallel reads anyway
	auto load_t0 = std::chrono::steady_clock::now();
	for (auto tile_no : to_load) {
		auto ok = loader.LoadTile( pyramid, plan.level_no, tile_no );
		if (ok) { pyramid.stats.tile_loads++; load_ok++; }
		else load_fail++;
	}
	pyramid.stats.tile_load_ms += ElapsedMs( load_t0 );
	return true;
}


static void PinRenderPlanTiles ( Ppyr &pyramid,
	const RenderPlan &plan, int delta )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size() || delta == 0)
		return;

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


struct DecodeTask {
	int tile_no = -1;
	int pix_slot = -1;
};


template <typename pixeltype>
static bool DecodeReservedTile ( WsiLoader &/*loader*/,
	Ppyr &pyramid, int level_no, const DecodeTask &task,
	double &decode_ms )
{
	auto &level = pyramid.levels[level_no];
	auto &tile = level.tiles[task.tile_no];
	auto *bf = level.tbf.slot_ptr( tile.ninbuf );
	auto bf_sz = tile.bf_sz;
	auto *pix_ptr = level_pix<pixeltype>( level ).pxb.slot_ptr( task.pix_slot );

	tile.nixbuf = task.pix_slot;

	auto dst = TileDst<pixeltype>{};
// The slot is already sized for this level's tiles, so the codec writes its
// pixels directly into it - no intermediate surface.
	dst.reserve = [&tile, pix_ptr] ( int w, int h, int &stride ) -> pixeltype * {
		tile.pbd_w = w;
		tile.pbd_h = h;
		tile.pbd_l = w;
		stride = w;
		return pix_ptr;
	};

	auto diag = TileDiag{ level_no, task.tile_no, 0.0 };
	auto ok = decode_tile( bf, bf_sz, level.color_hint, dst, diag );
	if (diag.unsupported_codec != ImgCodec::None)
		pyramid.unsupported_codec_->store( diag.unsupported_codec );
	decode_ms = diag.decode_ms;
	if (!ok) {
		tile.pbd_w = 0;
		tile.pbd_h = 0;
		tile.pbd_l = 0;
		return false;
	}

	tile.sts.store( TileSts::Ready, std::memory_order_release );
	return true;
}


template <typename pixeltype>
static bool DecodeTileToPixels ( const uint8_t *bf, size_t bf_sz,
	ColorHint hint, int level_no, int tile_no, std::vector<pixeltype> &pixels,
	Dim2D<int> &dimensions, double &decode_ms, std::atomic<ImgCodec> &unsupported_codec )
{
	dimensions = {};
	pixels.clear();

	auto dst = TileDst<pixeltype>{};
	dst.reserve = [&pixels, &dimensions] ( int w, int h, int &stride ) -> pixeltype * {
		if (w <= 0 || h <= 0)
			return nullptr;
		dimensions.w = w;
		dimensions.h = h;
		pixels.resize( (size_t)w * (size_t)h );
		stride = w;
		return pixels.data();
	};

	auto diag = TileDiag{ level_no, tile_no, 0.0 };
	auto ok = decode_tile( bf, bf_sz, hint, dst, diag );
	if (diag.unsupported_codec != ImgCodec::None)
		unsupported_codec.store( diag.unsupported_codec );
	decode_ms = diag.decode_ms;
	return ok;
}


template <typename pixeltype>
static std::vector<DecodeTask> CollectDecodeTasksLocal (
	WsiLoader &loader,
	Ppyr &pyramid,
	const RenderPlan &plan,
	int &decode_fail )
{
	auto &level = pyramid.levels[plan.level_no];
	auto &pix = level_pix<pixeltype>( level );
	auto tasks = std::vector<DecodeTask>{};
	tasks.reserve( plan.tiles.size() );
	for (auto tile_no : plan.tiles) {
		auto &tile = level.tiles[tile_no];
		while (true) {
			auto status = tile.sts.load( std::memory_order_acquire );
			if (status == TileSts::Ready)
				break;
			if (status == TileSts::Loading || status == TileSts::Decoding) {
				status = WaitForTileIdle( tile );
				if (status == TileSts::Ready || status == TileSts::Empty) {
					if (status == TileSts::Empty) {
						LogTileFailure( "decode-missing-load", plan.level_no,
							tile_no, tile, "tile returned to Empty before decode" );
						++decode_fail;
					}
					break;
				}
			}
			if (status != TileSts::Loaded) {
				if (status == TileSts::Empty) {
					LogTileFailure( "decode-missing-load", plan.level_no,
						tile_no, tile, "tile is Empty when decode was requested" );
					++decode_fail;
				}
				break;
			}
			auto expected = TileSts::Loaded;
			if (!tile.sts.compare_exchange_weak( expected, TileSts::Decoding,
				std::memory_order_acq_rel, std::memory_order_acquire ))
			{
				continue;
			}

			auto slot = loader.AcquirePixBfSlot<pixeltype>(
				pyramid, plan.level_no, tile_no );
			if (slot < 0) {
				LogTilePoolFailure( TilePoolFailureLogLocal<pixeltype>{
					.stage = "pixbf-alloc",
					.level_no = plan.level_no,
					.tile_no = tile_no,
					.tile = tile,
					.pyramid = pyramid,
					.pool_alloc = pix.pxb.allocated_bytes(),
					.used_slots = pix.pxb.used_slots,
					.num_slots = pix.pxb.num_slots,
					.cap_bytes = pyramid.pxb_cap_bytes,
					.reason = "failed to reserve decoded pixel buffer slot" } );
				if (pyr::debug_file()) {
					fprintf( pyr::debug_file(),
						"PIXBF-ALLOC-FAIL: lvl=%d tile=%d reserved=%d used=%d alloc=%zu cap=%zu\n",
						plan.level_no, tile_no, pix.pxb.num_slots,
						pix.pxb.used_slots, pix.pxb.allocated_bytes(),
						pyramid.ram_cap_bytes );
					fflush( pyr::debug_file() );
				}
				tile.sts.store( TileSts::Loaded, std::memory_order_release );
				++decode_fail;
				break;
			}
			tile.nixbuf = slot;
			tasks.push_back( { tile_no, slot } );
			break;
		}
	}
	return tasks;
}


static unsigned DecodeWorkerCount ( Ppyr &pyramid, size_t task_count )
{
	auto worker_count = std::thread::hardware_concurrency();
	if (pyramid.decode_threads > 0)
		worker_count = (unsigned)pyramid.decode_threads;
	if (worker_count == 0)
		worker_count = 4;
	worker_count = std::min<unsigned>( worker_count, (unsigned)task_count );
	if (worker_count == 0)
		worker_count = 1;
	pyramid.stats.peak_decode_workers = std::max<uint32_t>(
		pyramid.stats.peak_decode_workers, (uint32_t)worker_count );
	return worker_count;
}


template <typename pixeltype>
static bool EnsureTilesDecoded ( WsiLoader &loader,
	Ppyr &pyramid, const RenderPlan &plan,
	int &decode_ok, int &decode_fail )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size())
		return false;

	auto &level = pyramid.levels[plan.level_no];
	auto tasks = CollectDecodeTasksLocal<pixeltype>(
		loader, pyramid, plan, decode_fail );

	if (tasks.empty())
		return decode_fail == 0;

	std::shared_lock tbf_guard( *level.tbf_mtx_ );
	std::shared_lock pxb_guard( *level.pxb_mtx_ );

	struct DecodeResult {
		int ok = 0;
		int fail = 0;
		double decode_ms = 0.0;
		std::vector<int> failed_tiles;
	};

	const auto worker_count = DecodeWorkerCount( pyramid, tasks.size() );

	std::atomic<size_t> next_task{0};
	std::vector<DecodeResult> results( worker_count );

	loader.decode_pool_.run_batch( worker_count, [&] ( unsigned worker_idx ) {
		auto &result = results[worker_idx];
		while (true) {
			auto idx = next_task.fetch_add( 1, std::memory_order_relaxed );
			if (idx >= tasks.size())
				break;

			double decode_ms = 0.0;
			auto ok = DecodeReservedTile<pixeltype>( loader, pyramid, plan.level_no,
				tasks[idx], decode_ms );
			result.decode_ms += decode_ms;
			if (ok) {
				result.ok++;
			} else {
				result.fail++;
				result.failed_tiles.push_back( tasks[idx].tile_no );
			}
		}
	} );

	tbf_guard.unlock();
	pxb_guard.unlock();

	for (const auto &result : results) {
		decode_ok += result.ok;
		decode_fail += result.fail;
		pyramid.stats.tile_decode_ms += result.decode_ms;
		pyramid.stats.decoded_tiles += (uint64_t)result.ok;
		for (auto tile_no : result.failed_tiles) {
			auto &tile = level.tiles[tile_no];
			loader.ReleasePixBfSlot( pyramid, plan.level_no, tile.nixbuf );
			tile.nixbuf = -1;
			tile.pbd_w = 0;
			tile.pbd_h = 0;
			tile.pbd_l = 0;
			tile.sts.store( TileSts::Loaded, std::memory_order_release );
		}
	}
	return decode_fail == 0;
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


template <typename pixeltype>
static bool InitializeRender ( const RenderPlan &plan, Render<pixeltype> &render )
{
	render.clear();
	render.level_no = plan.level_no;
	render.lr = plan.render_px;
	render.mrct = plan.render_mrct;
	render.tiles = plan.tiles;
	render.image = Pwhl<pixeltype>( {
		plan.render_px.br.x - plan.render_px.tl.x,
		plan.render_px.br.y - plan.render_px.tl.y } );
	return render.image.p != nullptr;
}


template <typename pixeltype>
static int CompositeLoadedTilesDirectToRender ( WsiLoader &loader,
	Ppyr &pyramid, const RenderPlan &plan,
	Render<pixeltype> &render, int &decode_ok, int &decode_fail )
{
	if (plan.level_no < 0 || plan.level_no >= (int)pyramid.levels.size() ||
		!InitializeRender( plan, render ))
		return 0;

	auto &level = pyramid.levels[plan.level_no];
	for (auto tile_no : plan.tiles)
		WaitForTileIdle( level.tiles[tile_no] );
// Keep both compressed and existing decoded views stable through the batch.
	std::shared_lock tbf_guard( *level.tbf_mtx_ );
	std::shared_lock pxb_guard( *level.pxb_mtx_ );
	const auto workers = DecodeWorkerCount( pyramid, plan.tiles.size() );
	struct DecodedTile {
		std::vector<pixeltype> pixels;
		Dim2D<int> dimensions{};
		bool ready = false;
		bool decoded = false;
		bool failed = false;
		double decode_ms = 0.0;
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
			result.decode_ms = 0.0;
			auto tile_no = plan.tiles[begin + worker];
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
				level.tbf.slot_ptr( tile.ninbuf ), tile.bf_sz, level.color_hint,
				plan.level_no, tile_no, result.pixels, result.dimensions,
				result.decode_ms, *pyramid.unsupported_codec_ );
			result.failed = !result.decoded;
		} );
		for (unsigned i = 0; i < count; ++i) {
			auto &result = batch[i];
			auto &tile = level.tiles[plan.tiles[begin + i]];
			if (result.ready || result.decoded) {
				auto view = result.ready ? tile_pixels<pixeltype>( tile, level )
					: PwhlView<pixeltype>( result.dimensions,
						result.dimensions.w, result.pixels.data() );
				BlitTileIntoRender( tile.lr, view, render );
				++rendered_tiles;
				++decode_ok;
			}
			decode_fail += result.failed ? 1 : 0;
			pyramid.stats.decoded_tiles += result.decoded ? 1 : 0;
			pyramid.stats.tile_decode_ms += result.decode_ms;
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
	std::shared_lock pxb_guard( *level.pxb_mtx_ );
	if (!InitializeRender( plan, render ))
		return false;
	for (auto tile_no : plan.tiles) {
		auto &tile = level.tiles[tile_no];
		if (tile.sts.load() == TileSts::Ready && tile_has_pixels( tile ))
			BlitTileIntoRender( tile.lr, tile_pixels<pixeltype>( tile, level ), render );
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

bool WsiLoader::ReadFromFile ( uint8_t *dest, size_t file_offset,
	size_t count, unsigned worker_idx, const std::string &path )
{
	if (worker_idx > 0) {
		auto &fh = decode_pool_.worker_file( worker_idx - 1, path );
		fh.clear();
		fh.seekg( file_offset );
		if (!fh) {
			LogReadFailure( file_offset, count, worker_idx, "seek failed" );
			return false;
		}
		fh.read( (char *)dest, count );
		if (!fh.good()) {
			LogReadFailure( file_offset, count, worker_idx, "read failed" );
			return false;
		}
		return true;
	}
	std::unique_lock file_guard( file_mtx_ );
//
// Worker 0 is the calling thread, and several application threads can be inside
// Generate at once (see wsi_generate_in_flight), so this handle stays guarded.
// A level naming its own file borrows a pool row rather than growing a second
// guarded handle per path.
	auto &fh = path.empty()
		? file_
		: decode_pool_.worker_file( 0, path );
	fh.clear();
	fh.seekg( file_offset );
	if (!fh) {
		LogReadFailure( file_offset, count, worker_idx, "seek failed" );
		return false;
	}
	fh.read( (char *)dest, count );
	if (!fh.good()) {
		LogReadFailure( file_offset, count, worker_idx, "read failed" );
		return false;
	}
	return true;
}


// --- LRU eviction ---

// --- DecodeTile (shared JPEG decompression) ---

template <typename pixeltype>
bool WsiLoader::DecodeTile ( Ppyr &pyramid, int level_no, int tile_no ) {
	if (level_no < 0 || level_no >= (int)pyramid.levels.size())
		return false;

	auto &level = pyramid.levels[level_no];
	if (level.pixel_type != pixel_type_of<pixeltype>::value) {
		pyr::plog_err(
			"wsi decode: level %d holds %s, asked for %s\n",
			level_no, pixel_type_name( level.pixel_type ),
			pixel_type_name( pixel_type_of<pixeltype>::value ) );
		return false;
	}
	if (tile_no < 0 || tile_no >= (int)level.tiles.size())
		return false;
	if (WaitForTileIdle( level.tiles[tile_no] ) == TileSts::Ready)
		return true;

	RenderPlan plan;
	plan.level_no = level_no;
	plan.tiles.push_back( tile_no );

	int decode_ok = 0;
	int decode_fail = 0;
	if (!EnsureTilesDecoded<pixeltype>( *this, pyramid, plan, decode_ok, decode_fail ))
		return false;
	return WaitForTileIdle( level.tiles[tile_no] ) == TileSts::Ready;
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
	const PlaneSel &sel )
{
	auto in_flight_guard = WsiGenerateInFlightGuard{};
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

	Render<pixeltype> local_render;
	auto &active_render = render ? *render : local_render;
	auto span = GenerateSpan( pyramid, active_render, microns_per_pixel, subrect,
		sel );
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
	auto out = SampleRequestedRaster( active_render,
		pyramid.levels[source_level], subrect, { target_w, target_h } );
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"GENERATE-EXIT: span=%dx%d out=%dx%d mpp=%f rect=[%f,%f]-[%f,%f]\n",
			span.w, span.h, out.w, out.h, microns_per_pixel,
			subrect.tl.x, subrect.tl.y, subrect.br.x, subrect.br.y );
		fflush( pyr::debug_file() );
	}
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
	return request.br.x > request.tl.x && request.br.y > request.tl.y &&
		!plan.tiles.empty();
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
	const PlaneSel &sel )
{
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
		auto plan = BuildRenderPlan( pyramid, best_lvl_idx, subrect );
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
			} else {
				pyr::plog_err(
					"wsi generate INCOMPLETE: %llu of %zu tiles missing at level %d "
					"(mpp=%.4f) - the frame has unpainted areas; "
					"raise Ppyr::ram_cap_bytes or use a coarser level\n",
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


// --- Synthetic coarse levels ---
//
// A slide whose file stores no reduced-resolution layer - a CZI mosaic without a
// pyramid, a single-IFD TIFF - can only be shown zoomed out by reading its full
// resolution tiles, all of them at once. That does not fit the pool budget, so
// the surplus tiles used to be dropped and the frame came back partly unpainted:
// silently, and in an application that measures what it renders.
//
// So after a loader has described what the file contains, the pyramid is topped
// up here with levels the file does not have, decimated from the coarsest one it
// does. Format-agnostic on purpose: it works off Level geometry alone, so every
// loader gets it without knowing.
//
// Each synthetic level is one tile owning its pixels and already Ready, so the
// load/decode path never sees them.

// The thresholds are Ppyr fields (coarse_level_bytes, coarse_first_step_bytes,
// coarse_max_src_tiles) so a caller can tune or disable this per slide; see
// pyr.h for what each one governs.

// What a level costs once decoded — the quantity that decides both whether a
// whole-slide render can hold it and how long it takes to draw.
static size_t LevelDecodedBytes ( const Level &level )
{
	auto w = (size_t)width( level.bounds );
	auto h = (size_t)height( level.bounds );
	return w * h * pixel_size_bytes( level.pixel_type );
}


template <typename pixeltype>
static Pwhl<pixeltype> DecimateLevelIntoImage ( WsiLoader &loader,
	Ppyr &pyramid, int src_level_no, int factor,
	const Dim2D<int> &dst_dimensions )
{
	auto dst = Pwhl<pixeltype>( dst_dimensions );
	if (!dst.p)
		return {};

	auto &src = pyramid.levels[src_level_no];
	auto origin = src.bounds.tl;

//
// Tiles go through in batches decoded on the decode pool - the render path's own
// load and decode - instead of one at a time: a gallery's members are whole
// images, and decoding 300 of them serially took 14 s on every open. Each batch
// is pinned while it is decimated and handed back after, so the pools hold one
// batch, never the level; the batch is sized to a quarter of the pyramid's RAM
// cap so pinning it cannot starve the pools. Decimation itself stays in tile
// order, so where tiles overlap the later one still wins and the result is the
// one the serial pass produced.
	const auto slot_bytes = src.tbf.slot_bytes() +
		(size_t)src.tile_w * (size_t)src.tile_h * pixel_size_bytes( src.pixel_type );
	const auto workers = DecodeWorkerCount( pyramid, src.tiles.size() );
	const auto fit = slot_bytes > 0
		? std::max<size_t>( 1, pyramid.ram_cap_bytes / 4 / slot_bytes ) : (size_t)1;
	const auto batch_size = std::min<size_t>( (size_t)workers * 2, fit );

	auto batch = RenderPlan{};
	batch.level_no = src_level_no;
	auto was_ready = std::vector<char>{};
	auto complete = true;
	auto flush = [&] {
		if (batch.tiles.empty())
			return;
		auto pins = RenderTilePinGuard{ pyramid, batch };
		auto load_ok = 0;
		auto load_fail = 0;
		auto decode_ok = 0;
		auto decode_fail = 0;
		EnsureTilesLoaded<pixeltype>( loader, pyramid, batch, pyr::next_version(),
			load_ok, load_fail );
		EnsureTilesDecoded<pixeltype>( loader, pyramid, batch, decode_ok, decode_fail );
		complete = complete && load_fail == 0 && decode_fail == 0;
		for (auto tile_no : batch.tiles) {
			auto &tile = src.tiles[tile_no];
			if (tile.sts.load( std::memory_order_acquire ) != TileSts::Ready)
				continue;
			auto pbd = tile_pixels<pixeltype>( tile, src );
			if (pbd.p && pbd.w > 0 && pbd.h > 0) {
				(void)decimate_into( pbd,
					{ tile.lr.tl.x - origin.x, tile.lr.tl.y - origin.y }, factor,
					{ { 0, 0 }, { dst.w, dst.h } }, dst, { 0, 0 } );
			}
		}
		pins.release();
//
// Hand the buffers straight back: this pass exists precisely because the whole
// level does not fit in the pools at once.
		for (size_t i = 0; i < batch.tiles.size(); ++i) {
			if (was_ready[i])
				continue;
			auto &tile = src.tiles[batch.tiles[i]];
			if (tile.nixbuf >= 0) {
				loader.ReleasePixBfSlot( pyramid, src_level_no, tile.nixbuf );
				tile.nixbuf = -1;
				tile.pbd_w = tile.pbd_h = tile.pbd_l = 0;
			}
			if (tile.ninbuf >= 0) {
				loader.ReleaseTileBfSlot( pyramid, src_level_no, tile.ninbuf );
				tile.ninbuf = -1;
			}
			tile.sts.store( TileSts::Empty, std::memory_order_release );
		}
		batch.tiles.clear();
		was_ready.clear();
	};

	for (auto tile_no = 0; tile_no < (int)src.tiles.size(); ++tile_no) {
		const auto &tile = src.tiles[tile_no];
		const auto ready = tile.sts.load() == TileSts::Ready;
		if (tile.fsz == 0 && !ready)
			continue;
		batch.tiles.push_back( tile_no );
		was_ready.push_back( ready ? 1 : 0 );
		if (batch.tiles.size() >= batch_size) {
			flush();
			if (!complete)
				return {};
		}
	}
	flush();
	return complete ? std::move( dst ) : Pwhl<pixeltype>{};
}


template <typename pixeltype>
static void BuildCoarseLevelsForPlane ( WsiLoader &loader, Ppyr &pyramid,
	int plane_no );


template <typename pixeltype>
static void BuildCoarseLevels ( WsiLoader &loader,
	Ppyr &pyramid )
{
	if (pyramid.levels.empty() || pyramid.coarse_level_bytes == 0 ||
		pyramid.coarse_first_step_bytes == 0)
	{
		return;
	}

	for (auto plane_no = 0; plane_no < (int)pyramid.planes.size(); ++plane_no) {
		visit_level_pixel_type( pyramid.planes[plane_no].pixel_type,
			[&] ( auto pixel ) {
				BuildCoarseLevelsForPlane<decltype( pixel )>( loader, pyramid, plane_no );
			} );
	}
//
// Ladders gained levels; regroup so they are ordered finest-first again.
	rebuild_plane_index( pyramid );
}


// Top one plane's ladder up. Its coarsest level is the ladder's back(), never
// the level store's - the store interleaves planes.
template <typename pixeltype>
static void BuildCoarseLevelsForPlane ( WsiLoader &loader, Ppyr &pyramid,
	int plane_no )
{
	if (pyramid.planes[plane_no].ladder.empty())
		return;

	const auto plane = pyramid.planes[plane_no].plane;
	auto coarsest = pyramid.planes[plane_no].ladder.back();
	auto width = pyramid.levels[coarsest].bounds.br.x -
		pyramid.levels[coarsest].bounds.tl.x;
	auto height = pyramid.levels[coarsest].bounds.br.y -
		pyramid.levels[coarsest].bounds.tl.y;
	if (width <= 0 || height <= 0 ||
		LevelDecodedBytes( pyramid.levels[coarsest] ) <= pyramid.coarse_level_bytes)
	{
		return;
	}

	if (pyramid.levels[coarsest].tiles.size() > pyramid.coarse_max_src_tiles) {
		pyr::plog_err(
			"wsi pyramid: %dx%d coarsest level has %zu tiles - too many to build an "
			"overview while opening; zoomed-out views will read it in full\n",
			width, height, pyramid.levels[coarsest].tiles.size() );
		return;
	}

//
// First step goes straight to a factor that fits the ceiling, so a slide with a
// huge base level does not get an enormous half-size level on the way down.
	auto factor = 2;
	while ( (size_t)( ( width + factor - 1 ) / factor ) *
		(size_t)( ( height + factor - 1 ) / factor ) *
		sizeof( pixeltype ) > pyramid.coarse_first_step_bytes)
	{
		factor *= 2;
	}

	auto built = 0;
	auto src_no = coarsest;
	while (true) {
		const auto &src = pyramid.levels[src_no];
		auto src_w = src.bounds.br.x - src.bounds.tl.x;
		auto src_h = src.bounds.br.y - src.bounds.tl.y;
		if (src_w <= 1 || src_h <= 1 ||
			LevelDecodedBytes( src ) <= pyramid.coarse_level_bytes)
		{
			break;
		}

		auto dims = Dim2D<int>{
			std::max( 1, ( src_w + factor - 1 ) / factor ),
			std::max( 1, ( src_h + factor - 1 ) / factor ) };
		auto image = DecimateLevelIntoImage<pixeltype>(
			loader, pyramid, src_no, factor, dims );
		if (!image.is_set())
			break;

		auto level = Level{};
		level.plane = plane;
		level.pixel_type = src.pixel_type;
		level.pixel_type_declared = src.pixel_type_declared;
		level.max_pixel_value = src.max_pixel_value;
		level.metric = src.metric * (float)factor;
		level.tile_w = image.w;
		level.tile_h = image.h;
		level.synthetic = true;

		auto tile = Tile{};
		tile.lr = { { 0, 0 }, { image.w, image.h } };
//
// Nothing on disk backs this raster, so it cannot live in PixBf - that pool
// evicts on the assumption a tile can be re-read. The level owns it instead,
// and the tile just points at it.
		tile.pbd_w = image.w;
		tile.pbd_h = image.h;
		tile.pbd_l = image.l;
		auto &pix = level_pix<pixeltype>( level );
		tile.owned_no = (int)pix.owned.size();
		pix.owned.push_back( std::move( image ) );
		tile.sts.store( TileSts::Ready, std::memory_order_release );
		level.tiles.push_back( std::move( tile ) );
		set_level_bounds( level );

		pyramid.levels.push_back( std::move( level ) );
		src_no = (int)pyramid.levels.size() - 1;
		pyramid.planes[plane_no].ladder.push_back( src_no );
		++built;
//
// After the first step the source is a synthetic level already in memory, so
// plain halving is cheap from here on.
		factor = 2;
		if (built > 16)
			break;
	}

	if (built > 0) {
		pyr::plog( "wsi pyramid: added %d synthetic level(s); coarsest is now %dx%d\n",
			built,
			pyramid.levels.back().bounds.br.x - pyramid.levels.back().bounds.tl.x,
			pyramid.levels.back().bounds.br.y - pyramid.levels.back().bounds.tl.y );
	}
}


template <typename pixeltype>
void WsiLoader::TopUpCoarseLevels ( Ppyr &pyramid )
{
	BuildCoarseLevels<pixeltype>( *this, pyramid );
}


// --- Factory ---

std::unique_ptr<WsiLoader> WsiLoader::Create (
	const std::string &filename )
{
	auto dot = filename.rfind( '.' );
	if (dot == std::string::npos)
		return nullptr;

	const auto extension = lower_ascii_copy( std::string_view( filename ).substr( dot ) );
	switch (wsi_format_for_extension( extension )) {
		case WsiFormat::Ndpi:
			return std::make_unique<NdpiLoader>();
		case WsiFormat::Svs:
			return std::make_unique<SvsLoader>();
		case WsiFormat::Tiff:
			return std::make_unique<WsiTifLoader>();
		case WsiFormat::Gallery:
			return std::make_unique<GalleryLoader>();
		case WsiFormat::Dicom:
#if USE_DCMTK
			return std::make_unique<DcmLoader>();
#else
			return nullptr;
#endif
		case WsiFormat::Czi:
#if USE_CZI
// Constructing the delay-loaded CZI loader requires its DLL.
			return czi_runtime_available() ? std::make_unique<CziLoader>() : nullptr;
#else
			return nullptr;
#endif
		case WsiFormat::Unknown:
			return nullptr;
	}

	return nullptr;
}


//
// The class is no longer a template - only the members that touch decoded
// pixels are. Those are what get instantiated here, once per pixel type the
// application renders with.

template bool WsiLoader::OpenFile<Rgb> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<Rgb> ( Ppyr & );
template bool WsiLoader::DecodeTile<Rgb> ( Ppyr &, int, int );
template Pwhl<Rgb> WsiLoader::Generate<Rgb> ( Ppyr &, Render<Rgb> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel & );
template Pwhl<Rgb> WsiLoader::GenerateSpan<Rgb> ( Ppyr &, Render<Rgb> &, float,
	const Rtlbr<float> &, const PlaneSel & );

template bool WsiLoader::OpenFile<uint8_t> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<uint8_t> ( Ppyr & );
template bool WsiLoader::DecodeTile<uint8_t> ( Ppyr &, int, int );
template Pwhl<uint8_t> WsiLoader::Generate<uint8_t> ( Ppyr &, Render<uint8_t> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel & );
template Pwhl<uint8_t> WsiLoader::GenerateSpan<uint8_t> ( Ppyr &, Render<uint8_t> &, float,
	const Rtlbr<float> &, const PlaneSel & );

template bool WsiLoader::OpenFile<uint16_t> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<uint16_t> ( Ppyr & );
template bool WsiLoader::DecodeTile<uint16_t> ( Ppyr &, int, int );
template Pwhl<uint16_t> WsiLoader::Generate<uint16_t> ( Ppyr &, Render<uint16_t> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel & );
template Pwhl<uint16_t> WsiLoader::GenerateSpan<uint16_t> ( Ppyr &, Render<uint16_t> &, float,
	const Rtlbr<float> &, const PlaneSel & );

template bool WsiLoader::OpenFile<float> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<float> ( Ppyr & );
template bool WsiLoader::DecodeTile<float> ( Ppyr &, int, int );
template Pwhl<float> WsiLoader::Generate<float> ( Ppyr &, Render<float> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel & );
template Pwhl<float> WsiLoader::GenerateSpan<float> ( Ppyr &, Render<float> &, float,
	const Rtlbr<float> &, const PlaneSel & );

template bool WsiLoader::OpenFile<Rgb16> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<Rgb16> ( Ppyr & );
template bool WsiLoader::DecodeTile<Rgb16> ( Ppyr &, int, int );
template Pwhl<Rgb16> WsiLoader::Generate<Rgb16> ( Ppyr &, Render<Rgb16> *, float,
	const Rtlbr<float> &, const Dim2D<int> *, const PlaneSel & );
template Pwhl<Rgb16> WsiLoader::GenerateSpan<Rgb16> ( Ppyr &, Render<Rgb16> &, float,
	const Rtlbr<float> &, const PlaneSel & );

} // namespace priml
