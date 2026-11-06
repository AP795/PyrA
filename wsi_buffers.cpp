#include "wsi.h"

#include <algorithm>
#include <utility>

namespace priml {

// How much to add to a pool that already has slots.
//
// A proportion of what is there, so the number of reallocations stays
// logarithmic in the final size. Growing by a fixed small step - or by whatever
// the budget happens to leave - makes each growth recopy the whole buffer, which
// is quadratic and stays invisible until a request needs thousands of slots: a
// zone render is tens of megapixels in one call, where a 512x512 test is one.
//
// The budget clamps this afterwards; it does not set it. Once the clamp bites the
// pool stops growing and reuses slots instead, which is the intended behaviour at
// the cap and costs nothing.
inline constexpr int kPoolGrowthPercent = 50;


static int GeometricGrowSlots ( int num_slots, int max_slots ) {
	auto headroom = max_slots - num_slots;
	if (headroom <= 0)
		return 0;
	auto step = std::max( 1, num_slots * kPoolGrowthPercent / 100 );
	return std::min( step, headroom );
}


static int InitialGrowSlots ( size_t init_bytes, size_t slot_bytes, int max_slots ) {
	if (max_slots <= 0)
		return 0;
	if (slot_bytes == 0)
		return std::max( 1, max_slots );
	auto budget_slots = init_bytes / slot_bytes;
	auto grow_slots = budget_slots > 0 ? budget_slots : 1ull;
	return std::max( 1, std::min( max_slots, (int)grow_slots ) );
}


namespace {

std::pair<int, int> FindEvictionCandidate ( const Ppyr &pyramid, bool decoded )
{
	auto oldest = version_t{ UINT64_MAX };
	auto candidate = std::pair<int, int>{ -1, -1 };
	for (int li = 0; li < (int)pyramid.levels.size(); ++li) {
		const auto &level = pyramid.levels[li];
		auto count = decoded ? ( level.pix ? level.pix->num_slots() : 0 )
			: level.tbf.num_slots;
		for (int si = 0; si < count; ++si) {
			auto ti = decoded ? level.pix->slot_tile( si ) : level.tbf.tpos[si];
			if (ti < 0)
				continue;
			const auto &tile = level.tiles[ti];
			auto status = tile.sts.load();
			if (status == TileSts::Loading || status == TileSts::Decoding ||
				tile.pin_count.load( std::memory_order_relaxed ) > 0)
				continue;
			auto last_use = tile.last_use.load( std::memory_order_relaxed );
			if (last_use < oldest) {
				oldest = last_use;
				candidate = { li, ti };
			}
		}
	}
	return candidate;
}

} // namespace


bool WsiLoader::EvictTileBf ( Ppyr &pyramid ) {
	auto [evict_lvl, evict_tile] = FindEvictionCandidate( pyramid, false );
	if (evict_lvl < 0)
		return false;

	auto &level = pyramid.levels[evict_lvl];
	std::unique_lock level_guard( *level.tbf_mtx_ );
	auto &tile = level.tiles[evict_tile];
	level.tbf.release( tile.ninbuf );
	tile.ninbuf = -1;
	tile.bf_sz = 0;
// If tile was only Loaded (not decoded), reset to Empty
	if (tile.sts.load() == TileSts::Loaded)
		tile.sts.store( TileSts::Empty );
	return true;
}


bool WsiLoader::EvictPixBf ( Ppyr &pyramid ) {
	auto [evict_lvl, evict_tile] = FindEvictionCandidate( pyramid, true );
	if (evict_lvl < 0)
		return false;

	auto &level = pyramid.levels[evict_lvl];
	std::unique_lock level_guard( *level.pxb_mtx_ );
	auto &tile = level.tiles[evict_tile];
	level.pix->release_slot( tile.nixbuf );
	tile.nixbuf = -1;
	tile.pbd_w = 0;
	tile.pbd_h = 0;
	tile.pbd_l = 0;
// Tile still has compressed data in TileBf (if ninbuf >= 0), so go back to Loaded
	if (tile.ninbuf >= 0)
		tile.sts.store( TileSts::Loaded );
	else
		tile.sts.store( TileSts::Empty );
	return true;
}


size_t WsiLoader::AllocatedPoolBytes ( const Ppyr &pyramid ) const {
	size_t total = 0;
	for (const auto &level : pyramid.levels) {
		total += level.tbf.allocated_bytes();
		if (level.pix)
			total += level.pix->allocated_bytes();
	}
	return total;
}


static uint32_t CurrentTileBfUsedSlots ( const Ppyr &pyramid ) {
	uint32_t total = 0;
	for (const auto &level : pyramid.levels)
		total += (uint32_t)std::max( 0, level.tbf.used_slots );
	return total;
}


static uint32_t CurrentPixBfUsedSlots ( const Ppyr &pyramid ) {
	uint32_t total = 0;
	for (const auto &level : pyramid.levels)
		if (level.pix)
			total += (uint32_t)std::max( 0, level.pix->used_slots() );
	return total;
}

static size_t AllocatedTileBfBytes ( const Ppyr &pyramid ) {
	size_t total = 0;
	for (const auto &level : pyramid.levels)
		total += level.tbf.allocated_bytes();
	return total;
}


static size_t AllocatedPixBfBytes ( const Ppyr &pyramid ) {
	size_t total = 0;
	for (const auto &level : pyramid.levels)
		if (level.pix)
			total += level.pix->allocated_bytes();
	return total;
}


static size_t AllocatedPlanePoolBytes ( const Ppyr &pyramid, int plane_no )
{
	if (plane_no < 0 || plane_no >= (int)pyramid.planes.size())
		return 0;
	size_t total = 0;
	for (auto li : pyramid.planes[plane_no].ladder) {
		const auto &level = pyramid.levels[li];
		total += level.tbf.allocated_bytes();
		if (level.pix)
			total += level.pix->allocated_bytes();
	}
	return total;
}


// How many bytes `level_no` may add to a pool right now.
//
// A pure query - it never evicts. That is the point: eviction frees SLOTS, not
// bytes, because a pool's allocated size is its buffer capacity and that only
// shrinks when the freed slots happen to be at the tail. The old code evicted
// inside the budget check waiting for the total to fall, which it could not, then
// reported failure - and the caller gave up without retrying the acquire that
// those very evictions had just made possible. Measured: 392 of 400 slots free
// and the render dropped a tile anyway.
//
// So growth is bounded here and slot REUSE is the answer when the bound is zero,
// which is what the acquire loops now do.
size_t WsiLoader::GrowthBudget ( const Ppyr &pyramid, int level_no,
	size_t wanted, size_t kind_cap, size_t kind_allocated ) const
{
	if (wanted == 0)
		return 0;
	auto allowed = wanted;
	auto clamp = [&allowed] ( size_t used, size_t cap ) {
		if (cap == 0)
			return;
		allowed = used >= cap ? size_t{ 0 }
			: (std::min)( allowed, cap - used );
	};

	clamp( kind_allocated, kind_cap );
	clamp( AllocatedPlanePoolBytes( pyramid, pyramid.levels[level_no].plane_no ),
		plane_ram_cap( pyramid, pyramid.levels[level_no].plane_no ) );
	clamp( AllocatedPoolBytes( pyramid ), pyramid.ram_cap_bytes );
	return allowed;
}


template <bool Decoded, typename Pool>
int WsiLoader::AcquireBufferSlot ( Ppyr &pyramid, int level_no,
	int tile_no, Pool &pool )
{
	auto &level = pyramid.levels[level_no];
	auto &mutex = Decoded ? *level.pxb_mtx_ : *level.tbf_mtx_;
	auto initial_bytes = Decoded ? pyramid.pxb_init_bytes : pyramid.tbf_init_bytes;
	auto cap_bytes = Decoded ? pyramid.pxb_cap_bytes : pyramid.tbf_cap_bytes;
	while (true) {
		{
			std::unique_lock pool_guard( mutex );
			auto slot = pool.acquire( tile_no );
			if (slot >= 0) {
				auto used = Decoded ? CurrentPixBfUsedSlots( pyramid )
					: CurrentTileBfUsedSlots( pyramid );
				auto &peak = Decoded ? pyramid.stats.peak_pixbf_used_slots
					: pyramid.stats.peak_tilebf_used_slots;
				peak = std::max( peak, used );
				return slot;
			}
			if (pool.can_grow()) {
				auto grow = pool.num_slots == 0
					? InitialGrowSlots( initial_bytes, pool.slot_bytes(), pool.max_slots )
					: GeometricGrowSlots( pool.num_slots, pool.max_slots );
				auto allocated = Decoded ? AllocatedPixBfBytes( pyramid )
					: AllocatedTileBfBytes( pyramid );
				auto allowed = GrowthBudget( pyramid, level_no,
					(size_t)grow * pool.slot_bytes(), cap_bytes, allocated );
				grow = (int)std::min( (size_t)grow,
					allowed / std::max( size_t{ 1 }, pool.slot_bytes() ) );
				if (grow > 0 && pool.grow( grow ) > 0)
					continue;
			}
		}
// The allocation mutex remains held, but release the level lock before eviction.
// Reuse freed slots even when buffer capacity (and therefore the budget) stays put.
		if (!( Decoded ? EvictPixBf( pyramid ) : EvictTileBf( pyramid ) ))
			return -1;
	}
}


int WsiLoader::AcquireTileBfSlot ( Ppyr &pyramid, int level_no, int tile_no )
{
	std::unique_lock alloc_guard( tilebf_alloc_mtx_ );
	return AcquireBufferSlot<false>( pyramid, level_no, tile_no,
		pyramid.levels[level_no].tbf );
}


template <typename pixeltype>
int WsiLoader::AcquirePixBfSlot ( Ppyr &pyramid, int level_no, int tile_no )
{
	auto &level = pyramid.levels[level_no];
	std::unique_lock alloc_guard( pixbf_alloc_mtx_ );
	{
		std::unique_lock pool_guard( *level.pxb_mtx_ );
		auto &pool = level_pix<pixeltype>( level ).pxb;
		if (pool.max_slots == 0 && !level.tiles.empty())
			pool.init( { level.tile_w, level.tile_h }, (int)level.tiles.size() );
	}
	return AcquireBufferSlot<true>( pyramid, level_no, tile_no,
		level_pix<pixeltype>( level ).pxb );
}


void WsiLoader::ReleaseTileBfSlot ( Ppyr &pyramid,
	int level_no, int slot )
{
	auto &level = pyramid.levels[level_no];
	std::unique_lock alloc_guard( tilebf_alloc_mtx_ );
	std::unique_lock pool_guard( *level.tbf_mtx_ );
	level.tbf.release( slot );
}


void WsiLoader::ReleasePixBfSlot ( Ppyr &pyramid,
	int level_no, int slot )
{
	auto &level = pyramid.levels[level_no];
	std::unique_lock alloc_guard( pixbf_alloc_mtx_ );
	std::unique_lock pool_guard( *level.pxb_mtx_ );
	if (level.pix)
		level.pix->release_slot( slot );
}



template int WsiLoader::AcquirePixBfSlot<Rgb> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<uint8_t> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<uint16_t> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<float> ( Ppyr &, int, int );
template int WsiLoader::AcquirePixBfSlot<Rgb16> ( Ppyr &, int, int );

} // namespace priml
