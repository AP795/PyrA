// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "wsi.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace priml {

namespace {

// How much to add to a pool that already has slots.
//
// A proportion of what is there, so the number of reallocations stays
// logarithmic in the final size. Growing by a fixed small step - or by whatever
// the budget happens to leave - makes each growth recopy the whole buffer, which
// is quadratic and stays invisible until a request needs thousands of slots.
// The budget clamps this afterwards; it does not set it.
inline constexpr int kPoolGrowthPercent = 50;
//
// A variable-size (compressed) eviction frees at least this fraction of the
// bytes held, oldest entries first, in one scan. Finding the oldest entry scans
// every entry, and exact-size entries are numerous - tens of thousands of NDPI
// strips in a large budget - so one scan per evicted tile would cost more than
// the tile's decode. Freeing 1/kEvictBatchDivisor at once makes the scan's cost
// per evicted tile constant, for about that much budget left idle.
inline constexpr size_t kEvictBatchDivisor = 32;


size_t decoded_slot_bytes ( const Level &level )
{
	auto bytes = (size_t)level.tile_w * (size_t)level.tile_h *
		pixel_size_bytes( level.pixel_type );
	const auto alignment = alignof( std::max_align_t );
	if (bytes > std::numeric_limits<size_t>::max() - alignment + 1)
		return 0;
	return ( bytes + alignment - 1 ) / alignment * alignment;
}


size_t level_slot_bytes ( const Level &level, bool decoded )
{
	return decoded ? decoded_slot_bytes( level ) : level.tile_buffer_bytes;
}


int clamp_slot_count ( size_t count )
{
	return (int)std::min( count, (size_t)std::numeric_limits<int>::max() );
}


// Size pool `pool_no` of one kind if it is not yet. PerPyramid: the single pool
// gets a slot fitting the largest tile of any backed level. PerLevel: the level's
// own tile. Synthetic levels own their pixels and get no slots. Compressed pools
// are variable-size, and their slot size is only the largest entry accepted.
void ConfigurePool ( const Ppyr &pyramid, BufferPool &pool, bool decoded, int pool_no )
{
	if (pool.slot_bytes() != 0)
		return;
	const auto variable = !decoded;
	if (pyramid.cache_topology == CacheTopology::PerLevel) {
		if (pool_no >= (int)pyramid.levels.size())
			return;
		const auto &level = pyramid.levels[pool_no];
		if (!level.synthetic)
			pool.init( level_slot_bytes( level, decoded ), clamp_slot_count( level.tiles.size() ), variable );
		return;
	}
	auto bytes = size_t{ 0 };
	auto count = size_t{ 0 };
	for (const auto &level : pyramid.levels) {
		if (level.synthetic)
			continue;
		count += level.tiles.size();
		bytes = std::max( bytes, level_slot_bytes( level, decoded ) );
	}
	pool.init( bytes, clamp_slot_count( count ), variable );
}


// The levels whose entries pool `pool_no` of the current topology holds.
template <typename Fn>
void for_pool_levels ( const Ppyr &pyramid, int pool_no, Fn &&fn )
{
	if (pyramid.cache_topology == CacheTopology::PerLevel) {
		if (pool_no < (int)pyramid.levels.size())
			fn( pool_no );
		return;
	}
	for (auto li = 0; li < (int)pyramid.levels.size(); ++li)
		fn( li );
}


void forget_entry ( Tile &tile, bool decoded )
{
	if (decoded) {
		tile.nixbuf = -1;
		tile.pbd_w = tile.pbd_h = tile.pbd_l = 0;
		tile.sts.store( tile.ninbuf >= 0 ? TileSts::Loaded : TileSts::Empty );
	} else {
		tile.ninbuf = -1;
		tile.bf_sz = 0;
		if (tile.sts.load() == TileSts::Loaded)
			tile.sts.store( TileSts::Empty );
	}
}


bool evictable ( const Tile &tile )
{
	auto status = tile.sts.load();
	return status != TileSts::Loading && status != TileSts::Decoding &&
		tile.pin_count.load( std::memory_order_relaxed ) == 0;
}


struct EvictionCandidate {
	int pool_no = -1;
	int slot = -1;
	BufferSlot owner;
	version_t last_use = UINT64_MAX;
};


// Oldest unpinned entry of one kind across every pool. Each pool is scanned
// under both of its locks, the way pinning and the other kind's eviction hold
// them, so a pinned tile cannot be picked from a stale view.
EvictionCandidate FindEvictionCandidate ( Ppyr &pyramid, bool decoded )
{
	auto best = EvictionCandidate{};
	auto &kind = cache_kind( pyramid, decoded );
	for (auto pi = 0; pi < (int)kind.pools.size(); ++pi) {
		std::scoped_lock pool_guard( *pyramid.tbf.locks[pi], *pyramid.pxb.locks[pi] );
		const auto &pool = kind.pools[pi];
		for (auto si = 0; si < pool.num_slots; ++si) {
			const auto &owner = pool.tpos[si];
			if (owner.tile_no < 0)
				continue;
			const auto &tile = pyramid.levels[owner.level_no].tiles[owner.tile_no];
			if (!evictable( tile ))
				continue;
			auto last_use = tile.last_use.load( std::memory_order_relaxed );
			if (last_use < best.last_use)
				best = { pi, si, owner, last_use };
		}
	}
	return best;
}


// Free one entry of `decoded`'s kind, or false when every entry is pinned or in
// flight. The candidate is re-checked under its pool's locks: between the scan
// and the release another thread may have pinned or replaced it.
bool EvictBufferSlot ( Ppyr &pyramid, bool decoded, int reuse_level )
{
	constexpr int kRetries = 4;
	for (auto attempt = 0; attempt < kRetries; ++attempt) {
		const auto candidate = FindEvictionCandidate( pyramid, decoded );
		if (candidate.pool_no < 0)
			return false;
		std::scoped_lock pool_guard( *pyramid.tbf.locks[candidate.pool_no],
			*pyramid.pxb.locks[candidate.pool_no] );
		auto &pool = cache_kind( pyramid, decoded ).pools[candidate.pool_no];
		const auto &owner = pool.tpos[candidate.slot];
		if (owner.level_no != candidate.owner.level_no || owner.tile_no != candidate.owner.tile_no)
			continue;
		auto &tile = pyramid.levels[owner.level_no].tiles[owner.tile_no];
		if (!evictable( tile ))
			continue;
		pool.release( candidate.slot );
		forget_entry( tile, decoded );
//
// PerLevel: a slot freed in another level's pool does not help this level
// acquire, only the bytes do - so give that pool's free tail back.
		if (pyramid.cache_topology == CacheTopology::PerLevel &&
			candidate.pool_no != cache_pool_index( pyramid, reuse_level ))
			pool.trim_free_tail();
		return true;
	}
	return false;
}


// Variable-size kinds: free the oldest unpinned entries until at least
// `bytes_wanted`, and at least 1/kEvictBatchDivisor of what the kind holds, is
// released. False when nothing could be freed. One scan collects candidates;
// each is re-checked under its pool's locks before release, since another
// thread may have pinned, used or replaced it after the scan.
bool EvictOldestBatch ( Ppyr &pyramid, bool decoded, size_t bytes_wanted )
{
	auto &kind = cache_kind( pyramid, decoded );
	auto candidates = std::vector<EvictionCandidate>{};
	for (auto pi = 0; pi < (int)kind.pools.size(); ++pi) {
		std::scoped_lock pool_guard( *pyramid.tbf.locks[pi], *pyramid.pxb.locks[pi] );
		const auto &pool = kind.pools[pi];
		for (auto si = 0; si < pool.num_slots; ++si) {
			const auto &owner = pool.tpos[si];
			if (owner.tile_no < 0)
				continue;
			const auto &tile = pyramid.levels[owner.level_no].tiles[owner.tile_no];
			if (evictable( tile ))
				candidates.push_back( { pi, si, owner, tile.last_use.load( std::memory_order_relaxed ) } );
		}
	}
	const auto target = (std::max)( bytes_wanted, kind.allocated_bytes() / kEvictBatchDivisor );
	std::sort( candidates.begin(), candidates.end(), [] ( const auto &a, const auto &b ) {
		return a.last_use < b.last_use;
	} );
	auto freed = size_t{ 0 };
	for (const auto &candidate : candidates) {
		if (freed >= target)
			break;
		std::scoped_lock pool_guard( *pyramid.tbf.locks[candidate.pool_no],
			*pyramid.pxb.locks[candidate.pool_no] );
		auto &pool = kind.pools[candidate.pool_no];
		if (candidate.slot >= pool.num_slots)
			continue;
		const auto &owner = pool.tpos[candidate.slot];
		if (owner.level_no != candidate.owner.level_no || owner.tile_no != candidate.owner.tile_no)
			continue;
		auto &tile = pyramid.levels[owner.level_no].tiles[owner.tile_no];
		if (!evictable( tile ) || tile.last_use.load( std::memory_order_relaxed ) != candidate.last_use)
			continue;
		freed += pool.entry_bytes( candidate.slot );
		pool.release( candidate.slot );
		forget_entry( tile, decoded );
	}
	return freed > 0;
}


// PerLevel only: release free tail capacity anywhere before evicting a live
// entry. Ordinary releases keep capacity; this runs only under cache pressure.
bool TrimUnusedPoolStorage ( Ppyr &pyramid, bool decoded )
{
	auto &kind = cache_kind( pyramid, decoded );
	for (auto pi = 0; pi < (int)kind.pools.size(); ++pi) {
		std::unique_lock pool_guard( *kind.locks[pi] );
		if (kind.pools[pi].trim_free_tail() > 0)
			return true;
	}
	return false;
}


size_t AllocatedPlanePoolBytes ( const Ppyr &pyramid, int plane_no )
{
	if (plane_no < 0 || plane_no >= (int)pyramid.planes.size())
		return 0;
	auto total = size_t{ 0 };
	for (auto li : pyramid.planes[plane_no].ladder) {
		for (const auto *kind : { &pyramid.tbf, &pyramid.pxb })
			if (li < (int)kind->pools.size())
				total += kind->pools[li].allocated_bytes();
	}
	return total;
}


// How many bytes `level_no` may add to a pool of one kind right now.
//
// A pure query - it never evicts: eviction frees SLOTS, not bytes, so a budget
// check that evicted while waiting for the total to fall could never succeed.
// Growth is bounded here; slot reuse is the answer when the bound is zero.
size_t GrowthBudget ( const Ppyr &pyramid, int level_no, bool decoded, size_t wanted )
{
	auto allowed = wanted;
	auto clamp = [&allowed] ( size_t used, size_t cap ) {
		if (cap == 0)
			return;
		allowed = used >= cap ? size_t{ 0 } : (std::min)( allowed, cap - used );
	};

	clamp( cache_kind( pyramid, decoded ).allocated_bytes(),
		decoded ? pyramid.pxb_cap_bytes : pyramid.tbf_cap_bytes );
	if (pyramid.cache_topology == CacheTopology::PerLevel) {
		const auto plane_no = pyramid.levels[level_no].plane_no;
		clamp( AllocatedPlanePoolBytes( pyramid, plane_no ), plane_ram_cap( pyramid, plane_no ) );
		clamp( pyramid.tbf.allocated_bytes() + pyramid.pxb.allocated_bytes(),
			pyramid.ram_cap_bytes );
	}
	if (allowed > 0 && pyramid.cache_growth_budget)
		allowed = (std::min)( allowed, pyramid.cache_growth_budget( decoded, allowed ) );
	return allowed;
}

} // namespace


std::string_view cache_topology_id ( CacheTopology topology )
{
	return topology == CacheTopology::PerLevel ? "level" : "arena";
}


bool parse_cache_topology ( std::string_view text, CacheTopology *out )
{
	if (text == "arena")
		*out = CacheTopology::PerPyramid;
	else if (text == "level")
		*out = CacheTopology::PerLevel;
	else
		return false;
	return true;
}


void WsiLoader::InitializeBufferPools ( Ppyr &pyramid )
{
	std::scoped_lock alloc_guard( tilebf_alloc_mtx_, pixbf_alloc_mtx_ );
	pyramid.tbf = {};
	pyramid.pxb = {};
	prepare_cache_pools( pyramid );
}


void prepare_cache_pools ( Ppyr &pyramid )
{
	const auto count = pyramid.cache_topology == CacheTopology::PerLevel
		? pyramid.levels.size() : size_t{ 1 };
	for (auto decoded : { false, true }) {
		auto &kind = cache_kind( pyramid, decoded );
		kind.resize( count );
		for (auto pi = 0; pi < (int)kind.pools.size(); ++pi)
			ConfigurePool( pyramid, kind.pools[pi], decoded, pi );
	}
}


bool WsiLoader::EvictTileBf ( Ppyr &pyramid, int reuse_level )
{
	if (pyramid.cache_topology == CacheTopology::PerLevel &&
		TrimUnusedPoolStorage( pyramid, false ))
		return true;
	return EvictBufferSlot( pyramid, false, reuse_level );
}


bool WsiLoader::EvictPixBf ( Ppyr &pyramid, int reuse_level )
{
	if (pyramid.cache_topology == CacheTopology::PerLevel &&
		TrimUnusedPoolStorage( pyramid, true ))
		return true;
	return EvictBufferSlot( pyramid, true, reuse_level );
}


int WsiLoader::AcquireBufferSlot ( Ppyr &pyramid, int level_no,
	int tile_no, bool decoded, size_t bytes )
{
	auto &kind = cache_kind( pyramid, decoded );
	auto initial_bytes = decoded ? pyramid.pxb_init_bytes : pyramid.tbf_init_bytes;
	auto record_peak = [&] {
		auto &peak = decoded ? pyramid.stats.peak_pixbf_used_slots
			: pyramid.stats.peak_tilebf_used_slots;
		peak = std::max( peak, (uint32_t)kind.used_slots() );
	};
	while (true) {
		auto variable = false;
		{
			std::unique_lock pool_guard( cache_lock( pyramid, decoded, level_no ) );
			auto &pool = cache_pool( pyramid, decoded, level_no );
			ConfigurePool( pyramid, pool, decoded, cache_pool_index( pyramid, level_no ) );
			if (pool.slot_bytes() == 0)
				return -1;
//
// Variable size: the entry is exactly `bytes`, so the budget is asked for that
// and nothing more; when it refuses, the oldest entries make room.
			variable = pool.variable();
			if (variable) {
				if (bytes == 0 || bytes > pool.slot_bytes())
					return -1;
				if (GrowthBudget( pyramid, level_no, decoded, bytes ) >= bytes) {
					const auto slot = pool.acquire( level_no, tile_no, bytes );
					if (slot >= 0)
						record_peak();
					return slot;
				}
			} else if (auto slot = pool.acquire( level_no, tile_no ); slot >= 0) {
				if (decoded) {
					const auto &level = pyramid.levels[level_no];
// Begin the new occupant's trivial pixel lifetimes, including when a slot
// previously held another pixel type. Default initialization writes no pixels.
					visit_level_pixel_type( level.pixel_type, [&] ( auto pixel ) {
						using Pixel = decltype( pixel );
						std::uninitialized_default_construct_n( pool.typed_slot<Pixel>( slot ),
							(size_t)level.tile_w * (size_t)level.tile_h );
					} );
				}
				record_peak();
				return slot;
			} else if (pool.can_grow()) {
				auto remaining = (size_t)( pool.max_slots - pool.num_slots );
				auto grow = pool.num_slots == 0
					? std::max<size_t>( 1, initial_bytes / pool.slot_bytes() )
					: std::max<size_t>( 1, (size_t)pool.num_slots * kPoolGrowthPercent / 100 );
				grow = std::min( grow, remaining );
				auto allowed = GrowthBudget( pyramid, level_no, decoded, grow * pool.slot_bytes() );
				grow = std::min( grow, allowed / pool.slot_bytes() );
				if (grow > 0 && pool.grow( (int)grow ) > 0)
					continue;
			}
		}
// Release the pool lock before eviction takes both kinds' locks.
		const auto freed = variable ? EvictOldestBatch( pyramid, decoded, bytes )
			: decoded ? EvictPixBf( pyramid, level_no ) : EvictTileBf( pyramid, level_no );
		if (!freed)
			return -1;
	}
}


int WsiLoader::AcquireTileBfSlot ( Ppyr &pyramid, int level_no, int tile_no, size_t bytes )
{
	std::unique_lock alloc_guard( tilebf_alloc_mtx_ );
	return AcquireBufferSlot( pyramid, level_no, tile_no, false, bytes );
}


uint8_t *compressed_tile_scratch ( size_t bytes )
{
	thread_local auto scratch = std::vector<uint8_t>{};
	if (scratch.size() < bytes)
		scratch.resize( bytes );
	return scratch.data();
}


template <typename pixeltype>
int WsiLoader::AcquirePixBfSlot ( Ppyr &pyramid, int level_no, int tile_no )
{
	std::unique_lock alloc_guard( pixbf_alloc_mtx_ );
	{
		std::unique_lock pool_guard( cache_lock( pyramid, true, level_no ) );
		auto &level = pyramid.levels[level_no];
		(void)level_pix<pixeltype>( level );
		if (level.pixel_type != pixel_type_of<pixeltype>::value)
			return -1;
	}
	return AcquireBufferSlot( pyramid, level_no, tile_no, true );
}


void WsiLoader::ReleaseTileBfSlot ( Ppyr &pyramid, int level_no, int slot )
{
	std::unique_lock alloc_guard( tilebf_alloc_mtx_ );
	std::unique_lock pool_guard( cache_lock( pyramid, false, level_no ) );
	cache_pool( pyramid, false, level_no ).release( slot );
}


void WsiLoader::ReleasePixBfSlot ( Ppyr &pyramid, int level_no, int slot )
{
	std::unique_lock alloc_guard( pixbf_alloc_mtx_ );
	std::unique_lock pool_guard( cache_lock( pyramid, true, level_no ) );
	cache_pool( pyramid, true, level_no ).release( slot );
}


size_t release_unused_cache ( Ppyr &pyramid, bool decoded )
{
	auto &kind = cache_kind( pyramid, decoded );
	auto released = size_t{ 0 };
	for (auto pi = 0; pi < (int)kind.pools.size(); ++pi) {
		std::unique_lock pool_guard( *kind.locks[pi] );
		auto &pool = kind.pools[pi];
		if (pool.used_slots != 0)
			continue;
		released += pool.allocated_bytes();
		pool.clear_storage();
	}
	return released;
}


size_t reclaim_idle_cache ( Ppyr &pyramid, bool decoded )
{
	auto &kind = cache_kind( pyramid, decoded );
	auto released = size_t{ 0 };
	for (auto pi = 0; pi < (int)kind.pools.size(); ++pi) {
		std::scoped_lock pool_guard( *pyramid.tbf.locks[pi], *pyramid.pxb.locks[pi] );
		auto &pool = kind.pools[pi];
		if (pool.num_slots == 0)
			continue;
		auto busy = false;
		for_pool_levels( pyramid, pi, [&] ( int li ) {
			const auto &level = pyramid.levels[li];
			if (!level.synthetic)
				busy = busy || std::any_of( level.tiles.begin(), level.tiles.end(),
					[] ( const Tile &tile ) { return !evictable( tile ); } );
		} );
		if (busy)
			continue;
		for (auto si = 0; si < pool.num_slots; ++si) {
			const auto owner = pool.tpos[si];
			if (owner.tile_no >= 0)
				forget_entry( pyramid.levels[owner.level_no].tiles[owner.tile_no], decoded );
		}
		released += pool.allocated_bytes();
		pool.clear_storage();
	}
	return released;
}


template int WsiLoader::AcquirePixBfSlot<Rgb> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<uint8_t> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<uint16_t> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<float> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<Rgb16> ( Ppyr &, int, int );

} // namespace priml
