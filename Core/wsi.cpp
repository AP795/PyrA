// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "img_codec.h"
#include "wsi.h"
#include "wsi_formats.h"
#include "wsi_tile_pipeline.h"

#include "pyr_rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

namespace priml {

bool WsiLoader::RecacheEncodedTile ( Ppyr &pyramid, int level_no, int tile_no )
{
	auto &level = pyramid.levels[level_no];
	auto &tile = level.tiles[tile_no];
	if (level.tile_buffer_bytes == 0)
		return false;
	auto *scratch = compressed_tile_scratch( level.tile_buffer_bytes );
	auto size = size_t{};
	if (!AssembleTile( level, tile, scratch, size, 0 ) || size == 0 || size > level.tile_buffer_bytes)
		return false;
	const auto slot = AcquireTileBfSlot( pyramid, level_no, tile_no, size );
	if (slot < 0)
		return false;
	{
		auto guard = std::shared_lock( cache_lock( pyramid, false, level_no ) );
		std::memcpy( cache_pool( pyramid, false, level_no ).slot_ptr( slot ), scratch, size );
	}
	tile.ninbuf = slot;
	tile.bf_sz = size;
	return true;
}


bool WsiLoader::ReadEncodedTile ( Ppyr &pyramid, int level_no, int tile_no,
	std::vector<uint8_t> *bytes, std::string *error )
{
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	if (error)
		error->clear();
	if (bytes)
		bytes->clear();
	auto fail = [&] ( const std::string &message ) {
		if (error)
			*error = message;
		return false;
	};
	if (!bytes || level_no < 0 || level_no >= (int)pyramid.levels.size())
		return fail( "Invalid encoded-tile request" );
	const auto &level = pyramid.levels[level_no];
	if (level.synthetic || tile_no < 0 || tile_no >= (int)level.tiles.size())
		return fail( "Encoded-tile request requires a stored source tile" );
	auto &tile = pyramid.levels[level_no].tiles[tile_no];
	if (tile.fsz == 0)
		return true;
	if (level.tile_buffer_bytes == 0)
		return fail( "Source tile has no encoded-buffer size" );
	tile.last_use.store( pyr::next_version(), std::memory_order_relaxed );
	auto status = tile.sts.load( std::memory_order_acquire );
	if (status == TileSts::Loading || status == TileSts::Decoding)
		status = WaitForTileIdle( tile );
	auto cached = tile.ninbuf >= 0 && tile.bf_sz > 0;
	++( cached ? pyramid.stats.compressed_cache_hits : pyramid.stats.compressed_cache_misses );
	if (!cached) {
// Empty: the loader's own load path, as a render would take it. Ready: decoded
// pixels outlived the compressed slot, so only the slot is refilled.
		if (status == TileSts::Empty)
			(void)LoadTile( pyramid, level_no, tile_no );
		else if (status == TileSts::Ready)
			(void)RecacheEncodedTile( pyramid, level_no, tile_no );
		cached = tile.ninbuf >= 0 && tile.bf_sz > 0;
		if (cached)
			++pyramid.stats.tile_loads;
	}
	if (cached) {
		auto guard = std::shared_lock( cache_lock( pyramid, false, level_no ) );
		const auto *slot = cache_pool( pyramid, false, level_no ).slot_ptr( tile.ninbuf );
		bytes->assign( slot, slot + tile.bf_sz );
		return true;
	}
// No slot could be had (the budget is smaller than a slot, or taken by tiles in
// use): read past the cache rather than fail the request, and count it - the
// compressed budget is too small for this workflow.
// Logged once per slide; the count says how often.
	if (++pyramid.stats.compressed_cache_bypasses == 1)
		pyr::plog( "wsi tile bypass: lvl=%d tile=%d alloc=%zu used=%d/%d cap=%zu reason=no compressed slot; "
			"encoded tile read past the cache, the compressed budget is too small (later bypasses only counted)\n",
			level_no, tile_no, pyramid.tbf.allocated_bytes(), pyramid.tbf.used_slots(), pyramid.tbf.num_slots(),
			pyramid.tbf_cap_bytes );
	bytes->resize( level.tile_buffer_bytes );
	auto size = size_t{};
	if (!AssembleTile( level, tile, bytes->data(), size, 0 ) ||
		size == 0 || size > bytes->size())
	{
		bytes->clear();
		return fail( "Cannot read compressed source tile without decoding" );
	}
	bytes->resize( size );
	++pyramid.stats.tile_loads;
	return true;
}


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
// The typed-open contract: an RGB request cannot open a gray-only slide. At least
// one level has to belong to the requested family - integer monochrome and integer
// colour each span both of their storage widths.
	const auto in_family = [fallback] ( const Level &level ) {
		return !level.pixel_type_declared || level.pixel_type == fallback ||
			( is_integer_gray( fallback ) && is_integer_gray( level.pixel_type ) ) ||
			( is_integer_rgb( fallback ) && is_integer_rgb( level.pixel_type ) );
	};
	if (std::none_of( pyramid.levels.begin(), pyramid.levels.end(), in_family )) {
		pyramid.levels.clear();
		pyramid.planes.clear();
		return false;
	}
//
// Once that holds, a declared level of another family is kept, not dropped: a
// float32 or gray8 result channel appended to an RGB8 PTF is a plane of its own.
// Every ladder is typed per plane (rebuild_plane_index), decoding and rendering
// dispatch on the level's own type, and Generate<T> refuses a level of another
// type rather than misreading it - so the open's family only names which planes
// a caller of that one type can render.
	for (auto &level : pyramid.levels)
		if (!level.pixel_type_declared)
			level.pixel_type = fallback;
	pyramid.planes.clear();
	return !pyramid.levels.empty();
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
	const auto &pyramid = failure.pyramid;
	pyr::plog(
		"wsi tile fail: stage=%s lvl=%d tile=%d pool_used=%d/%d pool_alloc=%zu "
		"pool_cap=%zu topology=%s total_alloc=%zu ram_cap=%zu reason=%s\n",
		failure.stage ? failure.stage : "unknown",
		failure.level_no, failure.tile_no,
		failure.used_slots, failure.num_slots, failure.pool_alloc,
		failure.cap_bytes, std::string( cache_topology_id( pyramid.cache_topology ) ).c_str(),
		pyramid.tbf.allocated_bytes() + pyramid.pxb.allocated_bytes(),
		pyramid.ram_cap_bytes,
		(failure.reason && *failure.reason) ? failure.reason : "unknown" );
//
// PerLevel: where the budget actually went, per level. A multi-plane slide has
// a level ladder per channel, all competing for the same bytes, and which of
// them holds them is the entire diagnosis.
	if (pyramid.cache_topology == CacheTopology::PerLevel) {
		for (auto li = 0; li < (int)pyramid.tbf.pools.size() && li < (int)pyramid.levels.size(); ++li) {
			const auto tbf = pyramid.tbf.pools[li].allocated_bytes();
			const auto &pxb_pool = pyramid.pxb.pools[li];
			if (tbf == 0 && pxb_pool.allocated_bytes() == 0)
				continue;
			pyr::plog( "wsi tile fail:   L%d plane='%s' tbf=%zu pxb=%zu pxb_slots=%d/%d\n",
				li, pyramid.levels[li].plane.channel.c_str(), tbf, pxb_pool.allocated_bytes(),
				pxb_pool.used_slots, pxb_pool.num_slots );
		}
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

template <typename pixeltype>
bool EnsureTilesLoaded ( WsiLoader &loader,
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
// Ready pixels bypass compressed storage. Synthetic and absent tiles do not
// represent a lookup in either retained tile cache.
		if (!level.synthetic && tile.fsz > 0 && sts != TileSts::Ready) {
			if (sts == TileSts::Loaded && tile.ninbuf >= 0)
				++pyramid.stats.compressed_cache_hits;
			else
				++pyramid.stats.compressed_cache_misses;
		}
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
		if (ok) { pyramid.stats.tile_loads += level.tiles[to_load[0]].fsz > 0; load_ok++; }
		else load_fail++;
		return true;
	}

// Serial load — device-level I/O serialises parallel reads anyway
	auto load_t0 = std::chrono::steady_clock::now();
	for (auto tile_no : to_load) {
		auto ok = loader.LoadTile( pyramid, plan.level_no, tile_no );
		if (ok) { pyramid.stats.tile_loads += level.tiles[tile_no].fsz > 0; load_ok++; }
		else load_fail++;
	}
	pyramid.stats.tile_load_ms += ElapsedMs( load_t0 );
	return true;
}


struct DecodeTask {
	int tile_no = -1;
	int pix_slot = -1;
};


template <typename pixeltype>
static bool DecodeReservedTile ( WsiLoader &/*loader*/,
	Ppyr &pyramid, int level_no, const DecodeTask &task,
	TileDiag &diag )
{
	auto &level = pyramid.levels[level_no];
	auto &tile = level.tiles[task.tile_no];
	auto *bf = cache_pool( pyramid, false, level_no ).slot_ptr( tile.ninbuf );
	auto bf_sz = tile.bf_sz;
	auto *pix_ptr = cache_pool( pyramid, true, level_no ).typed_slot<pixeltype>( task.pix_slot );

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

	diag = TileDiag{ level_no, task.tile_no, 0.0 };
	auto ok = decode_tile( bf, bf_sz, level.color_hint, dst, diag );
	if (diag.unsupported_codec != ImgCodec::None)
		pyramid.unsupported_codec_->store( diag.unsupported_codec );
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
bool DecodeTileToPixels ( const uint8_t *bf, size_t bf_sz,
	ColorHint hint, std::vector<pixeltype> &pixels, Dim2D<int> &dimensions,
	TileDiag &diag, std::atomic<ImgCodec> &unsupported_codec )
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

	auto ok = decode_tile( bf, bf_sz, hint, dst, diag );
	if (diag.unsupported_codec != ImgCodec::None)
		unsupported_codec.store( diag.unsupported_codec );
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
	const auto &pixel_pool = cache_pool( pyramid, true, plan.level_no );
	auto tasks = std::vector<DecodeTask>{};
	tasks.reserve( plan.tiles.size() );
	for (auto tile_no : plan.tiles) {
		auto &tile = level.tiles[tile_no];
// One lookup per tile, before scheduling any decoder work. Temporary pixels
// for overview construction with retained PBF disabled are not cache entries.
		if (pyramid.use_pixbf && !level.synthetic && tile.fsz > 0) {
			if (tile.sts.load( std::memory_order_acquire ) == TileSts::Ready && tile.nixbuf >= 0)
				++pyramid.stats.decoded_cache_hits;
			else
				++pyramid.stats.decoded_cache_misses;
		}
		while (true) {
			auto status = tile.sts.load( std::memory_order_acquire );
			if (status == TileSts::Ready)
				break;
			if (status == TileSts::Loading || status == TileSts::Decoding) {
				status = WaitForTileIdle( tile );
				if (status == TileSts::Ready || status == TileSts::Empty) {
					if (status == TileSts::Empty) {
						if (!pyramid.deferred_tiles)
							LogTileFailure( "decode-missing-load", plan.level_no,
								tile_no, tile, "tile returned to Empty before decode" );
						++decode_fail;
					}
					break;
				}
			}
			if (status != TileSts::Loaded) {
				if (status == TileSts::Empty) {
// A deferred tile not yet put is a pending fetch, already counted as a load failure.
					if (!pyramid.deferred_tiles)
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
					.pool_alloc = pixel_pool.allocated_bytes(),
					.used_slots = pixel_pool.used_slots,
					.num_slots = pixel_pool.num_slots,
					.cap_bytes = pyramid.pxb_cap_bytes,
					.reason = "failed to reserve decoded pixel buffer slot" } );
				if (pyr::debug_file()) {
					fprintf( pyr::debug_file(),
						"PIXBF-ALLOC-FAIL: lvl=%d tile=%d reserved=%d used=%d alloc=%zu cap=%zu\n",
						plan.level_no, tile_no, pixel_pool.num_slots,
						pixel_pool.used_slots, pixel_pool.allocated_bytes(),
						pyramid.pxb_cap_bytes );
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


unsigned DecodeWorkerCount ( Ppyr &pyramid, size_t task_count )
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
bool EnsureTilesDecoded ( WsiLoader &loader,
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

	std::shared_lock tbf_guard( cache_lock( pyramid, false, plan.level_no ) );
	std::shared_lock pxb_guard( cache_lock( pyramid, true, plan.level_no ) );

	struct DecodeResult {
		int ok = 0;
		int fail = 0;
		double decode_ms = 0.0;
		ImgCodec codec = ImgCodec::None;
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

			auto diag = TileDiag{};
			auto ok = DecodeReservedTile<pixeltype>( loader, pyramid, plan.level_no,
				tasks[idx], diag );
			result.decode_ms += diag.decode_ms;
			if (diag.codec != ImgCodec::None)
				result.codec = diag.codec;
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
		if (result.codec != ImgCodec::None)
			level.tile_codec = result.codec;
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
	auto pins = RenderTilePinGuard{ pyramid, plan };
	auto ok = EnsureTilesDecoded<pixeltype>( *this, pyramid, plan, decode_ok, decode_fail );
	pins.release();
	return ok && WaitForTileIdle( level.tiles[tile_no] ) == TileSts::Ready;
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
// is pinned while it is decimated. Keep cached tiles until eviction; disabled
// decoded caching instead reuses temporary pixel slots between batches. Bound
// pinned storage by each enabled cap, leaving headroom for other cached tiles.
// Decimation stays in tile order to preserve overlap painting.
// The most one tile may take in either kind: a compressed entry is at most the
// level's largest assembled tile; a decoded slot under PerPyramid is the arena's,
// which can exceed this level's own tile.
	const auto compressed_slot_bytes = src.tile_buffer_bytes;
	const auto decoded_slot_bytes = std::max( (size_t)src.tile_w * (size_t)src.tile_h *
		pixel_size_bytes( src.pixel_type ), cache_pool( pyramid, true, src_level_no ).slot_bytes() );
	const auto keep_decoded = pyramid.use_pixbf_auto ? pyramid.prefer_pixbf : pyramid.use_pixbf;
	const auto workers = DecodeWorkerCount( pyramid, src.tiles.size() );
	auto batch_size = (size_t)workers * 2;
	for (auto [cap, bytes] : {
		std::pair{ pyramid.tbf_cap_bytes, compressed_slot_bytes },
		std::pair{ pyramid.pxb_cap_bytes, decoded_slot_bytes },
		std::pair{ pyramid.ram_cap_bytes, compressed_slot_bytes + decoded_slot_bytes } })
	{
		if (cap > 0 && bytes > 0)
			batch_size = std::min( batch_size, std::max<size_t>( 1, cap / 4 / bytes ) );
	}

	auto batch = RenderPlan{};
	batch.level_no = src_level_no;
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
		{
			std::shared_lock pixel_guard( cache_lock( pyramid, true, src_level_no ) );
			for (auto tile_no : batch.tiles) {
				auto &tile = src.tiles[tile_no];
				if (tile.sts.load( std::memory_order_acquire ) != TileSts::Ready)
					continue;
				auto pbd = tile_pixels<pixeltype>( tile, src,
					cache_pool( pyramid, true, src_level_no ) );
				if (pbd.p && pbd.w > 0 && pbd.h > 0) {
					(void)decimate_into( pbd,
						{ tile.lr.tl.x - origin.x, tile.lr.tl.y - origin.y }, factor,
						{ { 0, 0 }, { dst.w, dst.h } }, dst, { 0, 0 } );
				}
			}
		}
		pins.release();
//
// Temporary PBF slots are not a cache when decoded retention is disabled.
// Keep their backing allocation for the next batch; the C API releases that
// scratch once opening finishes. Compressed tile entries remain cached.
		if (!keep_decoded) {
			for (auto tile_no : batch.tiles) {
				auto &tile = src.tiles[tile_no];
				if (tile.nixbuf < 0)
					continue;
				loader.ReleasePixBfSlot( pyramid, src_level_no, tile.nixbuf );
				tile.nixbuf = -1;
				tile.pbd_w = tile.pbd_h = tile.pbd_l = 0;
				tile.sts.store( tile.ninbuf >= 0 ? TileSts::Loaded : TileSts::Empty,
					std::memory_order_release );
			}
		}
		batch.tiles.clear();
	};

	for (auto tile_no = 0; tile_no < (int)src.tiles.size(); ++tile_no) {
		const auto &tile = src.tiles[tile_no];
		const auto ready = tile.sts.load() == TileSts::Ready;
		if (tile.fsz == 0 && !ready)
			continue;
		batch.tiles.push_back( tile_no );
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
		prepare_cache_pools( pyramid );
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
// Overview generation needs decoded scratch even when normal rendering bypasses
// the decoded cache. Return that temporary storage once its last slot is free.
	auto keep_decoded = pyramid.use_pixbf_auto ? pyramid.prefer_pixbf : pyramid.use_pixbf;
	if (!keep_decoded)
		release_unused_cache( pyramid, true );
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
		case WsiFormat::Zyp:
			return std::make_unique<ZypLoader>();
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
template bool EnsureTilesLoaded<Rgb> ( WsiLoader &, Ppyr &, const RenderPlan &,
	version_t, int &, int & );
template bool EnsureTilesDecoded<Rgb> ( WsiLoader &, Ppyr &, const RenderPlan &,
	int &, int & );
template bool DecodeTileToPixels<Rgb> ( const uint8_t *, size_t, ColorHint,
	std::vector<Rgb> &, Dim2D<int> &, TileDiag &, std::atomic<ImgCodec> & );

template bool WsiLoader::OpenFile<uint8_t> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<uint8_t> ( Ppyr & );
template bool WsiLoader::DecodeTile<uint8_t> ( Ppyr &, int, int );
template bool EnsureTilesLoaded<uint8_t> ( WsiLoader &, Ppyr &, const RenderPlan &,
	version_t, int &, int & );
template bool EnsureTilesDecoded<uint8_t> ( WsiLoader &, Ppyr &, const RenderPlan &,
	int &, int & );
template bool DecodeTileToPixels<uint8_t> ( const uint8_t *, size_t, ColorHint,
	std::vector<uint8_t> &, Dim2D<int> &, TileDiag &, std::atomic<ImgCodec> & );

template bool WsiLoader::OpenFile<uint16_t> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<uint16_t> ( Ppyr & );
template bool WsiLoader::DecodeTile<uint16_t> ( Ppyr &, int, int );
template bool EnsureTilesLoaded<uint16_t> ( WsiLoader &, Ppyr &, const RenderPlan &,
	version_t, int &, int & );
template bool EnsureTilesDecoded<uint16_t> ( WsiLoader &, Ppyr &, const RenderPlan &,
	int &, int & );
template bool DecodeTileToPixels<uint16_t> ( const uint8_t *, size_t, ColorHint,
	std::vector<uint16_t> &, Dim2D<int> &, TileDiag &, std::atomic<ImgCodec> & );

template bool WsiLoader::OpenFile<float> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<float> ( Ppyr & );
template bool WsiLoader::DecodeTile<float> ( Ppyr &, int, int );
template bool EnsureTilesLoaded<float> ( WsiLoader &, Ppyr &, const RenderPlan &,
	version_t, int &, int & );
template bool EnsureTilesDecoded<float> ( WsiLoader &, Ppyr &, const RenderPlan &,
	int &, int & );
template bool DecodeTileToPixels<float> ( const uint8_t *, size_t, ColorHint,
	std::vector<float> &, Dim2D<int> &, TileDiag &, std::atomic<ImgCodec> & );

template bool WsiLoader::OpenFile<Rgb16> ( const std::string &, Ppyr & );
template void WsiLoader::TopUpCoarseLevels<Rgb16> ( Ppyr & );
template bool WsiLoader::DecodeTile<Rgb16> ( Ppyr &, int, int );
template bool EnsureTilesLoaded<Rgb16> ( WsiLoader &, Ppyr &, const RenderPlan &,
	version_t, int &, int & );
template bool EnsureTilesDecoded<Rgb16> ( WsiLoader &, Ppyr &, const RenderPlan &,
	int &, int & );
template bool DecodeTileToPixels<Rgb16> ( const uint8_t *, size_t, ColorHint,
	std::vector<Rgb16> &, Dim2D<int> &, TileDiag &, std::atomic<ImgCodec> & );

} // namespace priml
