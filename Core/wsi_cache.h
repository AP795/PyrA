// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Tile cache storage: one slot mechanism, two storage modes, two topologies.
//
// A cache kind (compressed tiles "tbf", decoded pixels "pxb") is a set of
// BufferPools. The two kinds store differently:
//
//   decoded     fixed: one contiguous array of equal slots. Every decoded tile of
//               a level has the same size, so a fixed slot wastes nothing there.
//   compressed  variable: each entry is its own allocation of exactly the
//               assembled tile's bytes. Compressed tiles vary several-fold in
//               size, and a slot sized to the largest one held 4x (NDPI strips)
//               to 13x (CMU-1 SVS) fewer tiles than the budget's bytes allow.
//               Placement is left to the system heap; the pool only accounts the
//               bytes it holds.
//
// The topology decides how many pools and what they share:
//
//   PerPyramid  one pool per kind for the whole pyramid. Fixed slots are sized to
//               the largest tile of any backed level, so an eviction is
//               immediately reusable at any resolution; one LRU spans all levels.
//               Bounded by the per-kind caps only.
//   PerLevel    one pool per kind per level. Fixed slots are sized to the level's
//               own tile, each level has its own lock, and growth is also bounded
//               by the per-plane share of Ppyr::ram_cap_bytes. Free tail capacity
//               is trimmed when an allocation hits cache pressure.
//
// The topology is fixed when a pyramid is opened; changing it means reopening.
// Both honour the per-kind caps, retention until eviction, and the optional
// Ppyr::cache_growth_budget owner hook. Neither knows a pixel type: decoded
// slots are raw aligned bytes, viewed through typed_slot<T> by the level's type.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <vector>

namespace priml {

enum class CacheTopology : uint8_t { PerPyramid, PerLevel };

// Stable identifiers for configuration files: "arena" and "level".
[[nodiscard]] std::string_view cache_topology_id ( CacheTopology topology );
bool parse_cache_topology ( std::string_view text, CacheTopology *out );


// Which level and tile occupy a slot; tile_no -1 = free.
struct BufferSlot {
	int level_no = -1;
	int tile_no = -1;
};


// Slots with a free list, in one of two storage modes (see the top of this file).
//
// Fixed: a contiguous array of equal-size slots. Release keeps the backing
// storage, so a freed slot is reused without reallocation; only trim_free_tail
// gives memory back.
//
// Variable: the slot table holds no bytes; each occupied slot owns one
// allocation of the size it was acquired with, freed at release. slot_bytes ()
// is then the largest entry the pool accepts, and allocated/used bytes are the
// bytes held. Entry storage never moves, so growing the table copies no data.
struct BufferPool {
	int max_slots = 0;
	int num_slots = 0;
	int used_slots = 0;
	int peak_used_slots = 0;
	size_t slot_bytes_ = 0;
	bool variable_ = false;
	size_t held_bytes_ = 0;
	std::unique_ptr<uint8_t[]> buf;
	std::vector<std::unique_ptr<uint8_t[]>> entries_;
	std::vector<size_t> entry_bytes_;
	std::vector<BufferSlot> tpos;
	std::vector<int> free_list_;

	void init ( size_t bytes, int count, bool variable = false ) {
		slot_bytes_ = bytes;
		max_slots = count;
		variable_ = variable;
	}

	bool variable () const { return variable_; }
	size_t slot_bytes () const { return slot_bytes_; }
	size_t allocated_bytes () const {
		return variable_ ? held_bytes_ : (size_t)num_slots * slot_bytes_;
	}
	size_t used_bytes () const {
		return variable_ ? held_bytes_ : (size_t)used_slots * slot_bytes_;
	}
	size_t entry_bytes ( int slot ) const {
		return variable_ ? entry_bytes_[slot] : slot_bytes_;
	}
	bool can_grow () const { return num_slots < max_slots; }
	uint8_t *slot_ptr ( int slot ) {
		return variable_ ? entries_[slot].get() : buf.get() + (size_t)slot * slot_bytes_;
	}

	template <typename T>
	T *typed_slot ( int slot ) {
		static_assert( std::is_trivially_copyable_v<T> );
		static_assert( alignof( T ) <= alignof( std::max_align_t ) );
		return reinterpret_cast<T *>( slot_ptr( slot ) );
	}

	int grow ( int count ) {
		auto add = (std::min)( count, max_slots - num_slots );
		if (add <= 0 || slot_bytes_ == 0 ||
			(size_t)( num_slots + add ) > std::numeric_limits<size_t>::max() / slot_bytes_)
			return 0;
		if (variable_) {
			entries_.resize( num_slots + add );
			entry_bytes_.resize( num_slots + add );
		} else {
			reallocate( num_slots + add );
		}
		free_list_.reserve( num_slots + add );
		tpos.resize( num_slots + add );
		for (auto i = num_slots + add - 1; i >= num_slots; --i)
			free_list_.push_back( i );
		num_slots += add;
		return add;
	}

// Fixed mode: take a free slot of the array.
	int acquire ( int level_no, int tile_no ) {
		if (variable_ || free_list_.empty())
			return -1;
		return take_slot( level_no, tile_no );
	}

// Variable mode: an entry of exactly `bytes`. The table grows by half its size
// when full - entries hold no bytes, so that costs no copy of tile data. The
// caller has already checked `bytes` against the byte budget.
	int acquire ( int level_no, int tile_no, size_t bytes ) {
		if (!variable_ || bytes == 0 || bytes > slot_bytes_)
			return -1;
		if (free_list_.empty() && grow( (std::max)( kVariableTableStep, num_slots / 2 ) ) == 0)
			return -1;
		auto storage = std::unique_ptr<uint8_t[]>{};
		try {
			storage = std::make_unique_for_overwrite<uint8_t[]>( bytes );
		} catch (const std::bad_alloc &) {
			return -1;
		}
		const auto slot = take_slot( level_no, tile_no );
		entries_[slot] = std::move( storage );
		entry_bytes_[slot] = bytes;
		held_bytes_ += bytes;
		return slot;
	}

// Fixed: reuse the existing allocation; only explicit trimming gives capacity
// back. Variable: the entry's bytes are freed now.
	void release ( int slot ) {
		if (slot >= 0 && slot < num_slots && tpos[slot].tile_no >= 0) {
			tpos[slot] = {};
			--used_slots;
			free_list_.push_back( slot );
			if (variable_) {
				held_bytes_ -= entry_bytes_[slot];
				entry_bytes_[slot] = 0;
				entries_[slot].reset();
			}
		}
	}

// Give back the free slots at the end of the array; occupied slots keep their
// index, so tiles referring to them stay valid. Returns the bytes released:
// always 0 in variable mode, whose free slots hold no bytes.
	size_t trim_free_tail () {
		auto keep = num_slots;
		while (keep > 0 && tpos[keep - 1].tile_no < 0)
			--keep;
		if (keep == num_slots)
			return 0;
		const auto released = variable_ ? size_t{ 0 } : (size_t)( num_slots - keep ) * slot_bytes_;
		if (variable_) {
			entries_.resize( keep );
			entry_bytes_.resize( keep );
		} else {
			reallocate( keep );
		}
		tpos.resize( keep );
		std::erase_if( free_list_, [keep] ( int s ) { return s >= keep; } );
		num_slots = keep;
		return released;
	}

// Back to the configured but empty state: slot size, mode and bound kept.
	void clear_storage () {
		buf.reset();
		entries_.clear();
		entry_bytes_.clear();
		held_bytes_ = 0;
		tpos.clear();
		free_list_.clear();
		num_slots = 0;
		used_slots = 0;
	}

private:
	static constexpr int kVariableTableStep = 64;

	int take_slot ( int level_no, int tile_no ) {
		auto slot = free_list_.back();
		free_list_.pop_back();
		tpos[slot] = { level_no, tile_no };
		++used_slots;
		peak_used_slots = (std::max)( peak_used_slots, used_slots );
		return slot;
	}

	void reallocate ( int slot_count ) {
		if (slot_count == 0) {
			buf.reset();
			return;
		}
		auto next = std::make_unique_for_overwrite<uint8_t[]>( (size_t)slot_count * slot_bytes_ );
		const auto keep = (size_t)(std::min)( slot_count, num_slots ) * slot_bytes_;
		if (keep > 0)
			std::memcpy( next.get(), buf.get(), keep );
		buf = std::move( next );
	}
};


// The pools of one cache kind and their locks: one entry under PerPyramid, one
// per level under PerLevel. Locks are separate heap objects so a Ppyr stays
// movable.
struct CachePools {
	std::vector<BufferPool> pools;
	std::vector<std::unique_ptr<std::shared_mutex>> locks;

	CachePools () { resize( 1 ); }

	void resize ( size_t count ) {
		count = (std::max<size_t>)( count, 1 );
		pools.resize( count );
		while (locks.size() < count)
			locks.push_back( std::make_unique<std::shared_mutex>() );
		locks.resize( count );
	}

	size_t allocated_bytes () const {
		auto total = size_t{ 0 };
		for (const auto &pool : pools)
			total += pool.allocated_bytes();
		return total;
	}

	int used_slots () const {
		auto total = 0;
		for (const auto &pool : pools)
			total += pool.used_slots;
		return total;
	}

// Bytes of the slots holding an entry; each pool has its own slot size (one per
// level in per-level topology, one per entry in variable mode), so this is not
// used_slots () times one size.
	size_t used_bytes () const {
		auto total = size_t{ 0 };
		for (const auto &pool : pools)
			total += pool.used_bytes();
		return total;
	}

	int num_slots () const {
		auto total = 0;
		for (const auto &pool : pools)
			total += pool.num_slots;
		return total;
	}

// The largest slot of any pool: what one more entry may cost.
	size_t max_slot_bytes () const {
		auto largest = size_t{ 0 };
		for (const auto &pool : pools)
			largest = (std::max)( largest, pool.slot_bytes() );
		return largest;
	}
};

} // namespace priml
