// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "wsi.h"

#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr auto kTileEdge = 8;
constexpr auto kSlotBytes = size_t{ kTileEdge * kTileEdge } * sizeof( priml::Rgb );
constexpr auto kTopologies = { priml::CacheTopology::PerPyramid, priml::CacheTopology::PerLevel };


void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


// Two levels of one tile each, every cache capped at one slot.
void prepare_pyramid ( priml::Ppyr &pyramid, priml::CacheTopology topology, int tiles_per_level = 1 )
{
	pyramid.cache_topology = topology;
	pyramid.ram_cap_bytes = 0;
	pyramid.tbf_init_bytes = pyramid.pxb_init_bytes = kSlotBytes;
	pyramid.tbf_cap_bytes = pyramid.pxb_cap_bytes = kSlotBytes;
	pyramid.levels.resize( 2 );
	for (auto index = 0; index < 2; ++index) {
		auto &level = pyramid.levels[index];
		level.metric = float( index + 1 );
		level.pixel_type = priml::LevelPixelType::Rgb8;
		level.tile_w = level.tile_h = kTileEdge;
		level.tile_buffer_bytes = kSlotBytes;
		for (auto ti = 0; ti < tiles_per_level; ++ti) {
			level.tiles.emplace_back();
			level.tiles.back().lr = { { 0, 0 }, { kTileEdge, kTileEdge } };
		}
		priml::set_level_bounds( level );
	}
	priml::rebuild_plane_index( pyramid );
	priml::prepare_cache_pools( pyramid );
}


int acquire ( priml::WsiLoader &loader, priml::Ppyr &pyramid, int level_no, bool decoded,
	int tile_no = 0, size_t compressed_bytes = kSlotBytes )
{
	auto &tile = pyramid.levels[level_no].tiles[tile_no];
	const auto slot = decoded ? loader.AcquirePixBfSlot<priml::Rgb>( pyramid, level_no, tile_no )
		: loader.AcquireTileBfSlot( pyramid, level_no, tile_no, compressed_bytes );
	if (slot >= 0) {
		if (decoded)
			tile.nixbuf = slot;
		else
			tile.ninbuf = slot;
		tile.sts.store( decoded ? priml::TileSts::Ready : priml::TileSts::Loaded );
	}
	return slot;
}


size_t allocated ( priml::Ppyr &pyramid, int level_no, bool decoded )
{
	return priml::cache_pool( pyramid, decoded, level_no ).allocated_bytes();
}


const void *storage ( priml::Ppyr &pyramid, int level_no, bool decoded )
{
	return priml::cache_pool( pyramid, decoded, level_no ).buf.get();
}


void test_wsi_buffers_slot_release_retains_capacity ()
{
	auto pool = priml::BufferPool{};
	pool.init( kSlotBytes, 2 );
	require( pool.grow( 2 ) == 2, "Cannot prepare reusable slot storage" );
	const auto bytes = pool.allocated_bytes();
	const auto *before = pool.buf.get();
	const auto first = pool.acquire( 0, 0 );
	const auto second = pool.acquire( 0, 1 );
	pool.release( first );
	pool.release( second );
	require( pool.used_slots == 0 && pool.num_slots == 2 &&
		pool.allocated_bytes() == bytes && pool.buf.get() == before,
		"Ordinary slot release freed reusable backing storage" );
	require( pool.acquire( 0, 2 ) >= 0 && pool.buf.get() == before,
		"Released storage could not be reused without allocation" );
}


void test_wsi_buffers_trim_keeps_occupied_slots ()
{
	auto pool = priml::BufferPool{};
	pool.init( kSlotBytes, 4 );
	require( pool.grow( 4 ) == 4, "Cannot prepare slot storage" );
	const auto kept = pool.acquire( 0, 0 );
	*pool.slot_ptr( kept ) = 37;
	for (auto i = 1; i < 4; ++i)
		pool.acquire( 0, i );
	for (auto slot = 1; slot < 4; ++slot)
		pool.release( slot );
	require( pool.trim_free_tail() == 3 * kSlotBytes && pool.num_slots == 1 &&
		pool.allocated_bytes() == kSlotBytes && *pool.slot_ptr( kept ) == 37 &&
		pool.tpos[kept].tile_no == 0, "Trimming lost an occupied slot or kept the free tail" );
	require( pool.grow( 1 ) == 1 && pool.acquire( 0, 5 ) == 1,
		"A trimmed pool could not grow again" );
}


// PerLevel: a released decoded slot's storage is kept until another level needs
// the bytes, and only then given back. (A compressed entry frees its bytes at
// release - see test_wsi_buffers_variable_entries_exact_bytes.)
void test_wsi_buffers_per_level_reclaims_unused_capacity ()
{
	for (const auto decoded : { true }) {
		auto pyramid = priml::Ppyr{};
		prepare_pyramid( pyramid, priml::CacheTopology::PerLevel );
		auto loader = priml::SvsLoader{};
		const auto slot = acquire( loader, pyramid, 0, decoded );
		require( slot >= 0, "Cannot allocate initial pool" );
		if (decoded)
			loader.ReleasePixBfSlot( pyramid, 0, slot );
		else
			loader.ReleaseTileBfSlot( pyramid, 0, slot );
		auto &tile = pyramid.levels.front().tiles.front();
		tile.ninbuf = tile.nixbuf = -1;
		tile.sts.store( priml::TileSts::Empty );
		require( allocated( pyramid, 0, decoded ) == kSlotBytes,
			"Backing storage was reclaimed before eviction pressure" );
		require( acquire( loader, pyramid, 1, decoded ) >= 0,
			"Unused backing storage starved another level" );
		require( allocated( pyramid, 0, decoded ) == 0 &&
			allocated( pyramid, 1, decoded ) == kSlotBytes,
			"Eviction did not transfer the bounded allocation between levels" );
	}
}


// Under pressure the older level's entry is evicted. PerLevel gives its bytes
// to the new level; PerPyramid hands the very slot over.
void test_wsi_buffers_eviction_moves_capacity_between_levels ()
{
	for (const auto topology : kTopologies)
		for (const auto decoded : { false, true }) {
			auto pyramid = priml::Ppyr{};
			prepare_pyramid( pyramid, topology );
			auto loader = priml::SvsLoader{};
			const auto first = acquire( loader, pyramid, 0, decoded );
			require( first >= 0, "Cannot populate initial cache" );
			const auto second = acquire( loader, pyramid, 1, decoded );
			require( second >= 0, "Cached backing storage starved another level" );
			require( pyramid.levels[0].tiles.front().sts.load() == priml::TileSts::Empty &&
				priml::cache_kind( pyramid, decoded ).allocated_bytes() == kSlotBytes,
				"Pressure eviction did not invalidate the older cache entry" );
			if (topology == priml::CacheTopology::PerLevel)
				require( allocated( pyramid, 0, decoded ) == 0 &&
					allocated( pyramid, 1, decoded ) == kSlotBytes,
					"PerLevel eviction did not reclaim the older level's storage" );
			else
				require( second == first &&
					priml::cache_pool( pyramid, decoded, 1 ).tpos[second].level_no == 1,
					"PerPyramid eviction did not reuse the slot across levels" );
		}
}


// Decoded slots only: compressed entries have no shared storage to reuse.
void test_wsi_buffers_eviction_reuses_same_level_storage ()
{
	for (const auto topology : kTopologies)
		for (const auto decoded : { true }) {
			auto pyramid = priml::Ppyr{};
			prepare_pyramid( pyramid, topology, 2 );
			auto loader = priml::SvsLoader{};
			const auto first = acquire( loader, pyramid, 0, decoded );
			require( first >= 0, "Cannot allocate the first cached slot" );
			const auto *before = storage( pyramid, 0, decoded );
			const auto second = acquire( loader, pyramid, 0, decoded, 1 );
			require( second == first && storage( pyramid, 0, decoded ) == before &&
				allocated( pyramid, 0, decoded ) == kSlotBytes,
				"Same-level eviction reallocated storage instead of reusing its slot" );
		}
}


void test_wsi_buffers_eviction_preserves_pinned_cache ()
{
	for (const auto topology : kTopologies)
		for (const auto decoded : { false, true }) {
			auto pyramid = priml::Ppyr{};
			prepare_pyramid( pyramid, topology );
			auto loader = priml::SvsLoader{};
			require( acquire( loader, pyramid, 0, decoded ) >= 0, "Cannot populate pinned cache" );
			pyramid.levels[0].tiles.front().pin_count.store( 1 );
			require( acquire( loader, pyramid, 1, decoded ) < 0 &&
				allocated( pyramid, 0, decoded ) == kSlotBytes &&
				pyramid.levels[0].tiles.front().sts.load() != priml::TileSts::Empty,
				"Eviction released storage belonging to a pinned tile" );
		}
}


// Idle reclamation drops every entry and frees the storage; a pinned level keeps
// its pool.
void test_wsi_buffers_reclaim_idle_cache ()
{
	for (const auto topology : kTopologies) {
		auto pyramid = priml::Ppyr{};
		prepare_pyramid( pyramid, topology );
		pyramid.tbf_cap_bytes = 2 * kSlotBytes;
		auto loader = priml::SvsLoader{};
		require( acquire( loader, pyramid, 0, false ) >= 0 && acquire( loader, pyramid, 1, false ) >= 0,
			"Cannot populate the cache" );
		pyramid.levels[1].tiles.front().pin_count.store( 1 );
		const auto freed = priml::reclaim_idle_cache( pyramid, false );
		if (topology == priml::CacheTopology::PerLevel)
			require( freed == kSlotBytes && pyramid.levels[0].tiles.front().ninbuf < 0 &&
				pyramid.levels[1].tiles.front().ninbuf >= 0,
				"PerLevel reclamation did not spare the pinned level" );
		else
			require( freed == 0 && pyramid.levels[0].tiles.front().ninbuf >= 0,
				"PerPyramid reclamation freed a pool holding a pinned entry" );
		pyramid.levels[1].tiles.front().pin_count.store( 0 );
		priml::reclaim_idle_cache( pyramid, false );
		require( pyramid.tbf.allocated_bytes() == 0 &&
			pyramid.levels[1].tiles.front().sts.load() == priml::TileSts::Empty,
			"Idle reclamation left storage or a stale entry" );
	}
}


// A variable pool holds exactly the bytes of its entries and frees them at
// release; its free slots hold nothing, so trimming them frees no bytes.
void test_wsi_buffers_variable_entries_exact_bytes ()
{
	constexpr auto kSmall = size_t{ 1000 };
	constexpr auto kLarge = size_t{ 30000 };
	auto pool = priml::BufferPool{};
	pool.init( kLarge, 4, true );
	const auto small = pool.acquire( 0, 0, kSmall );
	const auto large = pool.acquire( 0, 1, kLarge );
	require( small >= 0 && large >= 0 && pool.slot_ptr( small ) && pool.slot_ptr( large ) &&
		pool.allocated_bytes() == kSmall + kLarge && pool.used_bytes() == kSmall + kLarge &&
		pool.entry_bytes( small ) == kSmall && pool.entry_bytes( large ) == kLarge,
		"Variable entries do not hold exactly their own bytes" );
	require( pool.acquire( 0, 2, kLarge + 1 ) < 0 && pool.acquire( 0, 2 ) < 0,
		"A variable pool accepted an oversized or unsized entry" );
	pool.release( large );
	require( pool.allocated_bytes() == kSmall && pool.used_slots == 1 && pool.trim_free_tail() == 0,
		"Releasing a variable entry did not free its bytes" );
}


// The compressed cache counts bytes, not slots: two half-size entries share a
// one-slot cap at any topology, where fixed slots would have evicted one.
void test_wsi_buffers_variable_entries_share_cap_by_bytes ()
{
	for (const auto topology : kTopologies) {
		auto pyramid = priml::Ppyr{};
		prepare_pyramid( pyramid, topology );
		auto loader = priml::SvsLoader{};
		require( acquire( loader, pyramid, 0, false, 0, kSlotBytes / 2 ) >= 0 &&
			acquire( loader, pyramid, 1, false, 0, kSlotBytes / 2 ) >= 0 &&
			pyramid.levels[0].tiles.front().ninbuf >= 0 &&
			pyramid.tbf.allocated_bytes() == kSlotBytes,
			"Two half-size compressed entries did not share a one-slot cap" );
	}
}


// Under pressure the compressed cache frees the oldest entries in one batch of
// at least 1/32 of what it holds - not one tile per scan - and keeps the rest.
void test_wsi_buffers_variable_eviction_frees_oldest_batch ()
{
	constexpr auto kTiles = 65;
	constexpr auto kEntry = size_t{ 100 };
	auto pyramid = priml::Ppyr{};
	prepare_pyramid( pyramid, priml::CacheTopology::PerPyramid, kTiles );
	pyramid.tbf_init_bytes = pyramid.tbf_cap_bytes = ( kTiles - 1 ) * kEntry;
	auto loader = priml::SvsLoader{};
	auto &tiles = pyramid.levels[0].tiles;
	for (auto ti = 0; ti < kTiles - 1; ++ti) {
		require( acquire( loader, pyramid, 0, false, ti, kEntry ) >= 0, "Cannot fill the compressed cache" );
		tiles[ti].last_use.store( priml::version_t( ti + 1 ) );
	}
	require( acquire( loader, pyramid, 0, false, kTiles - 1, kEntry ) >= 0, "Pressure acquisition failed" );
	require( tiles[0].ninbuf < 0 && tiles[1].ninbuf < 0 && tiles[2].ninbuf >= 0 &&
		tiles[kTiles - 1].ninbuf >= 0 && pyramid.tbf.used_slots() == kTiles - 2 &&
		pyramid.tbf.allocated_bytes() == ( kTiles - 2 ) * kEntry,
		"Batch eviction did not free exactly the two oldest entries" );
}


void test_wsi_buffers_topology_ids ()
{
	auto topology = priml::CacheTopology::PerPyramid;
	require( priml::parse_cache_topology( "level", &topology ) &&
		topology == priml::CacheTopology::PerLevel &&
		priml::cache_topology_id( topology ) == "level" &&
		priml::parse_cache_topology( "arena", &topology ) &&
		topology == priml::CacheTopology::PerPyramid &&
		!priml::parse_cache_topology( "pyramid", &topology ),
		"Cache topology identifiers do not round-trip" );
}

} // namespace


bool run_wsi_buffers_tests ( std::ostream &console, std::ostream *file )
{
	const auto tests = {
		std::pair{ "test_wsi_buffers_slot_release_retains_capacity", &test_wsi_buffers_slot_release_retains_capacity },
		std::pair{ "test_wsi_buffers_trim_keeps_occupied_slots", &test_wsi_buffers_trim_keeps_occupied_slots },
		std::pair{ "test_wsi_buffers_per_level_reclaims_unused_capacity", &test_wsi_buffers_per_level_reclaims_unused_capacity },
		std::pair{ "test_wsi_buffers_eviction_moves_capacity_between_levels", &test_wsi_buffers_eviction_moves_capacity_between_levels },
		std::pair{ "test_wsi_buffers_eviction_reuses_same_level_storage", &test_wsi_buffers_eviction_reuses_same_level_storage },
		std::pair{ "test_wsi_buffers_eviction_preserves_pinned_cache", &test_wsi_buffers_eviction_preserves_pinned_cache },
		std::pair{ "test_wsi_buffers_reclaim_idle_cache", &test_wsi_buffers_reclaim_idle_cache },
		std::pair{ "test_wsi_buffers_variable_entries_exact_bytes", &test_wsi_buffers_variable_entries_exact_bytes },
		std::pair{ "test_wsi_buffers_variable_entries_share_cap_by_bytes", &test_wsi_buffers_variable_entries_share_cap_by_bytes },
		std::pair{ "test_wsi_buffers_variable_eviction_frees_oldest_batch", &test_wsi_buffers_variable_eviction_frees_oldest_batch },
		std::pair{ "test_wsi_buffers_topology_ids", &test_wsi_buffers_topology_ids }
	};
	auto passed = true;
	for (const auto &[name, test] : tests) {
		const auto start = std::chrono::steady_clock::now();
		auto error = std::string{};
		try {
			test();
		} catch (const std::exception &exception) {
			error = exception.what();
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start ).count();
		const auto result = std::format( "{}: {} ({} us){}{}\n", name,
			error.empty() ? "PASS" : "FAIL", elapsed, error.empty() ? "" : ": ", error );
		console << result;
		if (file)
			*file << result;
		passed = passed && error.empty();
	}
	return passed;
}
