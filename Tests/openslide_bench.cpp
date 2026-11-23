// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Render benchmark: native PyrA vs the OpenSlide backend through the C API,
// plus bare openslide_read_region on the same rectangles. The difference
// between the OpenSlide backend and the bare read is the wrapper's own cost.
// Not a CTest: it needs a real slide.
//
// usage: pyra_openslide_bench <slide> [output_dir] [scale] [threads]
//   scale = request mpp / native mpp of the main sequence (default 1 = 1:1)
//   threads = decode_threads of every C API context (default 1); the bare
//   OpenSlide reads stay single-threaded

#include <cstdlib>
#include <string_view>
#include <pyra/pyra.h>

#include <openslide.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kViewWidth = uint32_t{ 1920 };
constexpr auto kViewHeight = uint32_t{ 1080 };
constexpr auto kPanColumns = 6;
constexpr auto kPanRows = 5;
// Each step moves half a viewport, as a pan does: consecutive frames overlap.
constexpr auto kPanStep = 0.5;
constexpr auto kCacheBytes = uint64_t{ 256 } * 1024 * 1024;
constexpr auto kControlScale = 1.6;
// decode_threads of every C API context, from the command line.
auto bench_threads = 1u;

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


struct Request {
	double x_um = 0;
	double y_um = 0;
};


struct Geometry {
	pyra_rect bounds{};
	double native_mpp = 0;
};


struct Timing {
	double total_ms = 0;
	double median_ms = 0;
	double p95_ms = 0;
};


Timing summarize ( std::vector<double> times )
{
	auto timing = Timing{};
	if (times.empty())
		return timing;
	timing.total_ms = std::accumulate( times.begin(), times.end(), 0.0 );
	std::sort( times.begin(), times.end() );
	const auto middle = times.size() / 2;
	timing.median_ms = times.size() % 2 ? times[middle] : ( times[middle - 1] + times[middle] ) / 2;
	timing.p95_ms = times[size_t( std::ceil( 0.95 * double( times.size() ) ) ) - 1];
	return timing;
}


uint64_t fnv1a ( const unsigned char *data, size_t size, uint64_t hash = 14695981039346656037ull )
{
	for (size_t i = 0; i < size; ++i) {
		hash ^= data[i];
		hash *= 1099511628211ull;
	}
	return hash;
}


// Serpentine pan around the plane centre, in the plane's own metric frame.
std::vector<Request> pan_requests ( const Geometry &geometry, double mpp )
{
	const auto view_w = kViewWidth * mpp;
	const auto view_h = kViewHeight * mpp;
	const auto span_w = view_w * ( 1 + kPanStep * ( kPanColumns - 1 ) );
	const auto span_h = view_h * ( 1 + kPanStep * ( kPanRows - 1 ) );
	const auto left = geometry.bounds.x_um + std::max( 0.0, ( geometry.bounds.width_um - span_w ) / 2 );
	const auto top = geometry.bounds.y_um + std::max( 0.0, ( geometry.bounds.height_um - span_h ) / 2 );
	auto requests = std::vector<Request>{};
	for (auto row = 0; row < kPanRows; ++row) {
		for (auto step = 0; step < kPanColumns; ++step) {
			const auto column = row % 2 ? kPanColumns - 1 - step : step;
			requests.push_back( { left + column * kPanStep * view_w, top + row * kPanStep * view_h } );
		}
	}
	return requests;
}


class Context {
public:
	Context ( pyra_backend backend, uint64_t decoded_cache_bytes, const std::string &path ) {
		auto options = pyra_context_options{};
		pyra_context_options_init( &options );
		options.backend = backend;
		options.decoded_cache_bytes = decoded_cache_bytes;
		options.decode_threads = bench_threads;
// PYRA_BENCH_CACHE=level compares the per-level cache layout with the default arena.
		if (const auto *layout = std::getenv( "PYRA_BENCH_CACHE" ); layout && std::string_view( layout ) == "level")
			options.cache_topology = PYRA_CACHE_PER_LEVEL;
		auto error = pyra_error{};
		require( pyra_context_create( &options, &context_, &error ) == PYRA_OK, error.message );
		require( pyra_open( context_, path.c_str(), &wsi_, &error ) == PYRA_OK, error.message );
		auto plane = pyra_plane_info{};
		plane.struct_size = sizeof( plane );
		require( pyra_get_plane_info( wsi_, 0, &plane, &error ) == PYRA_OK, error.message );
		geometry_ = { plane.bounds, plane.native_mpp };
		require( plane.pixel_format == PYRA_PIXEL_RGB8, "Benchmark expects an RGB8 plane" );
	}
	~Context () {
		if (wsi_)
			pyra_close( wsi_, nullptr );
		if (context_)
			pyra_context_destroy( context_, nullptr );
	}
	Context ( const Context & ) = delete;
	Context &operator= ( const Context & ) = delete;

	const Geometry &geometry () const { return geometry_; }

	std::string cache_state () const {
		auto stats = pyra_context_stats{};
		stats.struct_size = sizeof( stats );
		if (pyra_context_get_stats( context_, &stats, nullptr ) != PYRA_OK)
			return "stats unavailable";
		const auto mib = [] ( uint64_t bytes ) {
			return bytes == PYRA_STAT_UNAVAILABLE ? -1.0 : double( bytes ) / ( 1024.0 * 1024.0 );
		};
		return std::format( "decoded {:.1f} MiB, compressed {:.1f} MiB, evictions {}",
			mib( stats.decoded_cache_bytes ), mib( stats.compressed_cache_bytes ), stats.cache_pool_evictions );
	}

// One pass over the requests; returns per-request milliseconds and folds the
// pixels of each frame into hashes.
	std::vector<double> pass ( const std::vector<Request> &requests, double mpp,
		std::vector<uint64_t> *hashes ) {
		auto times = std::vector<double>{};
		for (const auto &r : requests) {
			auto request = pyra_image_request{ sizeof( pyra_image_request ), 0,
				{ r.x_um, r.y_um, kViewWidth * mpp, kViewHeight * mpp }, mpp };
			auto layout = pyra_image_layout{};
			layout.struct_size = sizeof( layout );
			auto error = pyra_error{};
			require( pyra_get_image_layout( wsi_, &request, &layout, &error ) == PYRA_OK, error.message );
			if (buffer_.size() < layout.capacity_bytes)
				buffer_.resize( layout.capacity_bytes );
			auto destination = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
				buffer_.data(), buffer_.size(), layout.row_stride_bytes };
			const auto start = Clock::now();
			const auto status = pyra_get_image( wsi_, &request, &destination, &error );
			times.push_back( elapsed_ms( start ) );
			require( status == PYRA_OK || status == PYRA_INCOMPLETE, error.message );
			if (hashes)
				hashes->push_back( fnv1a( buffer_.data(), layout.capacity_bytes ) );
		}
		return times;
	}

private:
	pyra_context *context_ = nullptr;
	pyra_wsi *wsi_ = nullptr;
	Geometry geometry_;
	std::vector<unsigned char> buffer_;
};


struct SlideCloser {
	void operator() ( openslide_t *slide ) const { openslide_close( slide ); }
};


struct CacheCloser {
	void operator() ( openslide_cache_t *cache ) const { openslide_cache_release( cache ); }
};


// Bare OpenSlide: the same rectangles read at the level the backend selects,
// into a reused ARGB buffer. Only meaningful when the level downsample is 1
// or the request maps 1:1 onto the level.
class BareOpenSlide {
public:
	BareOpenSlide ( const std::string &path, uint64_t cache_bytes )
		: slide_( openslide_open( path.c_str() ) ), cache_( openslide_cache_create( size_t( cache_bytes ) ) ) {
		require( slide_ && !openslide_get_error( slide_.get() ), "openslide_open failed" );
		openslide_set_cache( slide_.get(), cache_.get() );
	}

	std::vector<double> pass ( const std::vector<Request> &requests, const Geometry &geometry, double mpp ) {
		const auto level = openslide_get_best_level_for_downsample( slide_.get(), mpp / geometry.native_mpp );
		const auto downsample = openslide_get_level_downsample( slide_.get(), level );
		const auto width = int64_t( std::ceil( kViewWidth * mpp / ( geometry.native_mpp * downsample ) ) );
		const auto height = int64_t( std::ceil( kViewHeight * mpp / ( geometry.native_mpp * downsample ) ) );
		buffer_.resize( size_t( width * height ) );
		auto times = std::vector<double>{};
		for (const auto &r : requests) {
			const auto x = int64_t( std::floor( ( r.x_um - geometry.bounds.x_um ) / geometry.native_mpp ) );
			const auto y = int64_t( std::floor( ( r.y_um - geometry.bounds.y_um ) / geometry.native_mpp ) );
			const auto start = Clock::now();
			openslide_read_region( slide_.get(), buffer_.data(), x, y, level, width, height );
			times.push_back( elapsed_ms( start ) );
			const auto *error = openslide_get_error( slide_.get() );
			require( !error, error ? error : "" );
		}
		return times;
	}

private:
	std::unique_ptr<openslide_t, SlideCloser> slide_;
	std::unique_ptr<openslide_cache_t, CacheCloser> cache_;
	std::vector<uint32_t> buffer_;
};


void report ( std::ostream &out, const std::string &name, const std::vector<double> &times )
{
	const auto t = summarize( times );
	out << std::format( "{:<44} n={:<3} median {:8.3f} ms  p95 {:8.3f} ms  total {:9.3f} ms\n",
		name, times.size(), t.median_ms, t.p95_ms, t.total_ms );
}


struct Hashes {
	std::vector<uint64_t> native;
	std::vector<uint64_t> openslide;
};


void bench_sequence ( std::ostream &out, const std::string &path, double scale,
	const std::string &label, Hashes &hashes )
{
	out << std::format( "\n== {} (request mpp = {} x native mpp, {}x{} view, {} frames) ==\n",
		label, scale, kViewWidth, kViewHeight, kPanColumns * kPanRows );
	for (const auto backend : { PYRA_BACKEND_NATIVE, PYRA_BACKEND_OPENSLIDE }) {
		const auto name = backend == PYRA_BACKEND_NATIVE ? std::string( "native" ) : std::string( "openslide" );
		auto *sink = backend == PYRA_BACKEND_OPENSLIDE ? &hashes.openslide : &hashes.native;
		{
			auto context = Context( backend, kCacheBytes, path );
			const auto mpp = context.geometry().native_mpp * scale;
			const auto requests = pan_requests( context.geometry(), mpp );
			report( out, name + " 256 MiB pass 1", context.pass( requests, mpp, sink ) );
			out << "  after pass 1: " << context.cache_state() << "\n";
			report( out, name + " 256 MiB pass 2 (replay)", context.pass( requests, mpp, nullptr ) );
			out << "  after pass 2: " << context.cache_state() << "\n";
		}
		{
			auto context = Context( backend, 0, path );
			const auto mpp = context.geometry().native_mpp * scale;
			const auto requests = pan_requests( context.geometry(), mpp );
			report( out, name + " decoded cache 0", context.pass( requests, mpp, nullptr ) );
		}
	}
	auto geometry_source = Context( PYRA_BACKEND_OPENSLIDE, 0, path );
	const auto &geometry = geometry_source.geometry();
	const auto mpp = geometry.native_mpp * scale;
	const auto requests = pan_requests( geometry, mpp );
	{
		auto bare = BareOpenSlide( path, kCacheBytes );
		report( out, "bare read_region 256 MiB pass 1", bare.pass( requests, geometry, mpp ) );
		report( out, "bare read_region 256 MiB pass 2 (replay)", bare.pass( requests, geometry, mpp ) );
	}
	{
		auto bare = BareOpenSlide( path, 0 );
		report( out, "bare read_region cache 0", bare.pass( requests, geometry, mpp ) );
	}
}

} // namespace


int main ( int argc, char **argv )
{
	try {
		require( argc >= 2, "usage: pyra_openslide_bench <slide> [output_dir] [scale] [threads]" );
		const auto path = std::string( argv[1] );
		const auto output = argc >= 3 ? std::filesystem::path( argv[2] ) : std::filesystem::path{};
		const auto scale = argc >= 4 ? std::stod( argv[3] ) : 1.0;
		bench_threads = argc >= 5 ? unsigned( std::stoul( argv[4] ) ) : 1u;
		require( bench_threads > 0, "threads must be positive" );
		std::cout << "slide: " << path << "\nOpenSlide " << openslide_get_version()
			<< "\ndecode_threads: " << bench_threads << "\n";
		{
			auto slide = std::unique_ptr<openslide_t, SlideCloser>( openslide_open( path.c_str() ) );
			require( slide != nullptr, "openslide_open failed" );
			for (auto level = 0; level < openslide_get_level_count( slide.get() ); ++level) {
				auto w = int64_t{};
				auto h = int64_t{};
				openslide_get_level_dimensions( slide.get(), level, &w, &h );
				std::cout << std::format( "  OpenSlide level {}: downsample {:.3f}, {}x{} px\n", level,
					openslide_get_level_downsample( slide.get(), level ), w, h );
			}
		}
// Warm the OS file cache so the first measured case is not penalised.
		{
			auto warm = Context( PYRA_BACKEND_OPENSLIDE, 0, path );
			const auto mpp = warm.geometry().native_mpp * scale;
			(void)warm.pass( pan_requests( warm.geometry(), mpp ), mpp, nullptr );
		}
		auto hashes = Hashes{};
		bench_sequence( std::cout, path, scale, "main", hashes );
		bench_sequence( std::cout, path, scale * kControlScale, "control between levels", hashes );
		for (const auto &[name, list] : { std::pair{ "native", &hashes.native },
			std::pair{ "openslide", &hashes.openslide } }) {
			auto combined = uint64_t{ 14695981039346656037ull };
			for (const auto hash : *list)
				combined = fnv1a( reinterpret_cast<const unsigned char *>( &hash ), sizeof( hash ), combined );
			std::cout << std::format( "\n{} output hash ({} frames): {:016x}", name, list->size(), combined );
			if (!output.empty()) {
				std::filesystem::create_directories( output );
				auto file = std::ofstream( output / std::format( "{}_hashes.txt", name ), std::ios::binary );
				for (const auto hash : *list)
					file << std::format( "{:016x}\n", hash );
			}
		}
		std::cout << "\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
