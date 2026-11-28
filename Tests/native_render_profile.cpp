// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Stage profile of the native warm-cache render path, on a real slide.
// Renders the same panned viewport sequence as pyra_openslide_bench through
// WsiLoader::Generate (what pyra_get_image calls) and splits each frame with
// the core's own PpyrStats timers. Not a CTest.
//
// usage: pyra_native_profile <slide> [scale] [tbf_mib] [pxb_mib]
//
// A small tbf_mib/pxb_mib forces steady-state eviction: the churn case where
// cache bookkeeping, not decode, can dominate.

#include "wsi.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kViewWidth = 1920;
constexpr auto kViewHeight = 1080;
constexpr auto kPanColumns = 6;
constexpr auto kPanRows = 5;
constexpr auto kPanStep = 0.5;
constexpr auto kCacheBytes = size_t{ 256 } * 1024 * 1024;

using Clock = std::chrono::steady_clock;


double elapsed_ms ( Clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
}


void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


double median ( std::vector<double> values )
{
	std::sort( values.begin(), values.end() );
	const auto middle = values.size() / 2;
	return values.size() % 2 ? values[middle] : ( values[middle - 1] + values[middle] ) / 2;
}


struct Frame {
	double total = 0;	// Generate, as pyra_get_image calls it
	double span = 0;	// GenerateSpan: plan + tile preparation + composite
	double plan = 0;
	double load = 0;
	double decode = 0;	// decode batch (tile decode on a miss)
	double copy_out = 0;	// the C API's row copy into the caller's buffer
	int tiles = 0;
};


std::vector<priml::Rtlbr<float>> pan ( const priml::Rtlbr<float> &bounds, float mpp )
{
	const auto view_w = kViewWidth * mpp;
	const auto view_h = kViewHeight * mpp;
	const auto span_w = view_w * float( 1 + kPanStep * ( kPanColumns - 1 ) );
	const auto span_h = view_h * float( 1 + kPanStep * ( kPanRows - 1 ) );
	const auto left = bounds.tl.x + std::max( 0.0f, ( bounds.br.x - bounds.tl.x - span_w ) / 2 );
	const auto top = bounds.tl.y + std::max( 0.0f, ( bounds.br.y - bounds.tl.y - span_h ) / 2 );
	auto rects = std::vector<priml::Rtlbr<float>>{};
	for (auto row = 0; row < kPanRows; ++row) {
		for (auto step = 0; step < kPanColumns; ++step) {
			const auto column = row % 2 ? kPanColumns - 1 - step : step;
			const auto x = left + float( column * kPanStep ) * view_w;
			const auto y = top + float( row * kPanStep ) * view_h;
			rects.push_back( { { x, y }, { x + view_w, y + view_h } } );
		}
	}
	return rects;
}


std::vector<Frame> pass ( priml::WsiLoader &loader, priml::Ppyr &pyramid,
	const priml::PlaneSel &plane, float mpp, const std::vector<priml::Rtlbr<float>> &rects )
{
	auto frames = std::vector<Frame>{};
	auto destination = std::vector<unsigned char>( size_t( kViewWidth ) * kViewHeight * 3 );
	const auto size = priml::Dim2D<int>{ kViewWidth, kViewHeight };
	for (const auto &rect : rects) {
		const auto before = pyramid.stats;
		const auto start = Clock::now();
		auto pixels = loader.Generate<priml::Rgb>( pyramid, nullptr, mpp, rect, &size, plane );
		auto frame = Frame{};
		frame.total = elapsed_ms( start );
		require( pixels.p && pixels.w == kViewWidth && pixels.h == kViewHeight, "Render failed" );
		const auto copy_start = Clock::now();
		for (auto row = 0; row < pixels.h; ++row)
			std::memcpy( destination.data() + size_t( row ) * kViewWidth * 3, pixels.rowp( row ), size_t( kViewWidth ) * 3 );
		frame.copy_out = elapsed_ms( copy_start );
		const auto &after = pyramid.stats;
		frame.span = after.render_ms - before.render_ms;
		frame.plan = after.render_plan_ms - before.render_plan_ms;
		frame.load = after.tile_load_ms - before.tile_load_ms;
		frame.decode = after.decode_batch_ms - before.decode_batch_ms;
		frame.tiles = int( after.requested_tiles - before.requested_tiles );
		frames.push_back( frame );
	}
	return frames;
}


void report ( const std::string &name, const std::vector<Frame> &frames,
	const priml::Ppyr &pyramid, const priml::PpyrStats &before )
{
	auto pick = [&] ( double Frame::*field ) {
		auto values = std::vector<double>{};
		for (const auto &frame : frames)
			values.push_back( frame.*field );
		return median( values );
	};
	const auto total = pick( &Frame::total );
	const auto span = pick( &Frame::span );
	const auto plan = pick( &Frame::plan );
	const auto load = pick( &Frame::load );
	const auto decode = pick( &Frame::decode );
	std::cout << std::format( "\n{} (medians, ms)\n", name );
	std::cout << std::format( "  Generate total            {:8.3f}\n", total );
	std::cout << std::format( "    GenerateSpan            {:8.3f}\n", span );
	std::cout << std::format( "      render plan           {:8.3f}\n", plan );
	std::cout << std::format( "      tile load (I/O)       {:8.3f}\n", load );
	std::cout << std::format( "      decode batch          {:8.3f}\n", decode );
	std::cout << std::format( "      rest: pin/stamp/composite {:8.3f}\n", span - plan - load - decode );
	std::cout << std::format( "    sample to request (rest) {:8.3f}\n", total - span );
	std::cout << std::format( "  C API row copy-out        {:8.3f}\n", pick( &Frame::copy_out ) );
	std::cout << std::format( "  tiles/frame {} for a {}x{} view\n", frames.front().tiles, kViewWidth, kViewHeight );
	auto pass_ms = 0.0;
	for (const auto &frame : frames)
		pass_ms += frame.total;
	const auto &after = pyramid.stats;
	const auto held = pyramid.tbf.used_slots();
	const auto held_bytes = pyramid.tbf.used_bytes();
	constexpr auto kMiB = 1024.0 * 1024.0;
	std::cout << std::format( "  pass total {:.1f} ms; source tile reads {}; TBF hits {} misses {}; PBF hits {} misses {}; incomplete {}\n",
		pass_ms, after.tile_loads - before.tile_loads,
		after.compressed_cache_hits - before.compressed_cache_hits,
		after.compressed_cache_misses - before.compressed_cache_misses,
		after.decoded_cache_hits - before.decoded_cache_hits,
		after.decoded_cache_misses - before.decoded_cache_misses,
		after.incomplete_renders - before.incomplete_renders );
	std::cout << std::format( "  TBF holds {} tiles in {:.2f} MiB ({:.1f} KiB each), allocated {:.2f} MiB\n",
		held, held_bytes / kMiB, held ? held_bytes / 1024.0 / held : 0.0, pyramid.tbf.allocated_bytes() / kMiB );
}


// What a frame allocation alone costs: a fresh zero-filled Pwhl, as
// InitializeRender makes one per call.
double allocation_ms ( int w, int h )
{
	auto times = std::vector<double>{};
	for (auto i = 0; i < 30; ++i) {
		const auto start = Clock::now();
		auto image = priml::Pwhl<priml::Rgb>( { w, h } );
		times.push_back( elapsed_ms( start ) );
		require( image.p != nullptr, "Allocation failed" );
	}
	return median( times );
}

} // namespace


int main ( int argc, char **argv )
{
	try {
		require( argc >= 2, "usage: pyra_native_profile <slide> [scale] [tbf_mib] [pxb_mib]" );
		const auto path = std::string( argv[1] );
		const auto scale = argc >= 3 ? std::stof( argv[2] ) : 1.0f;
		auto mib_arg = [&] ( int index ) {
			return argc > index ? (size_t)std::stoull( argv[index] ) * 1024 * 1024 : kCacheBytes;
		};
		auto pyramid = priml::Ppyr{};
// The C API's settings for a 256 MiB / 256 MiB / one-thread context.
		pyramid.decode_threads = 1;
		pyramid.ram_cap_bytes = 0;
		pyramid.tbf_cap_bytes = mib_arg( 3 );
		pyramid.pxb_cap_bytes = mib_arg( 4 );
		pyramid.use_pixbf = true;
		pyramid.cache_growth_budget = [] ( bool, size_t wanted ) { return wanted; };
		auto loader = priml::WsiLoader::Create( path );
		require( loader && loader->OpenFile<priml::Rgb>( path, pyramid ), "Open failed" );
		require( !pyramid.planes.empty(), "No planes" );
		const auto plane = pyramid.planes.front().plane;
		const auto geometry = priml::native_geometry_for_plane( pyramid, plane );
		require( geometry && geometry->valid(), "No native geometry" );
		const auto mpp = geometry->render_mpp * scale;
		const auto rects = pan( geometry->metric_bounds, mpp );
		const auto &level = pyramid.levels[geometry->native_level_index];
		std::cout << std::format( "slide: {}\nlevel 0: {}x{} px, tile {}x{}, {} tiles\n", path,
			level.bounds.br.x - level.bounds.tl.x, level.bounds.br.y - level.bounds.tl.y,
			level.tile_w, level.tile_h, level.tiles.size() );
		for (const auto index : pyramid.planes.front().ladder) {
			const auto &rung = pyramid.levels[index];
			std::cout << std::format( "  level {}: downsample {:.3f}, {}x{} px, tile {}x{}, {} tiles\n", index,
				rung.metric / level.metric, rung.bounds.br.x - rung.bounds.tl.x,
				rung.bounds.br.y - rung.bounds.tl.y, rung.tile_w, rung.tile_h, rung.tiles.size() );
		}
		std::cout << std::format( "caches: TBF {} MiB, PBF {} MiB\n", pyramid.tbf_cap_bytes >> 20, pyramid.pxb_cap_bytes >> 20 );
		auto before = pyramid.stats;
		const auto cold = pass( *loader, pyramid, plane, mpp, rects );
		report( "pass 1 (cold decoded cache)", cold, pyramid, before );
		before = pyramid.stats;
		const auto warm = pass( *loader, pyramid, plane, mpp, rects );
		report( "pass 2 (warm, as the replay)", warm, pyramid, before );
		std::cout << std::format( "\nreference: fresh zero-filled Pwhl, 2x view {:8.3f} ms, view {:8.3f} ms\n",
			allocation_ms( 2 * kViewWidth, kViewHeight ), allocation_ms( kViewWidth, kViewHeight ) );
// The frame this scale reads: first touch of fresh memory against a rewrite of
// a buffer already mapped, to separate page faults from bandwidth.
		const auto frame_w = int( std::ceil( kViewWidth * scale / 2 ) ) * ( scale >= 2 ? 1 : 2 );
		const auto frame_h = int( std::ceil( kViewHeight * scale / 2 ) ) * ( scale >= 2 ? 1 : 2 );
		const auto bytes = size_t( frame_w ) * frame_h * 3;
		auto fresh = std::vector<double>{};
		auto reused = std::vector<double>{};
		auto mapped = std::vector<unsigned char>( bytes );
		for (auto i = 0; i < 30; ++i) {
			auto start = Clock::now();
			{
				auto buffer = std::make_unique_for_overwrite<unsigned char[]>( bytes );
				std::memset( buffer.get(), i, bytes );
				fresh.push_back( elapsed_ms( start ) );
			}
			start = Clock::now();
			std::memset( mapped.data(), i, bytes );
			reused.push_back( elapsed_ms( start ) );
		}
		std::cout << std::format( "reference: write {}x{} RGB frame - fresh allocation {:8.3f} ms, reused buffer {:8.3f} ms\n",
			frame_w, frame_h, median( fresh ), median( reused ) );
		loader->Close( pyramid );
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
