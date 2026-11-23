// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "chunk_process.h"

#include "bridge_worker.h"
#include "chunk_internal.h"
#include "chunk_pyramid.h"
#include "dcm_series.h"
#include "pyr_rt.h"
#include "wsi.h"
#include "wsi_session.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <memory>
#include <set>
#include <span>
#include <thread>

namespace priml {

namespace bridge_detail {

bool Prepared::any ( const Raster &raster, const Rtlbr<int> &rect ) const
{
	if (rect.is_empty())
		return false;
	const auto x0 = column( rect.tl.x ), x1 = column( rect.br.x - 1 );
	const auto y0 = row( rect.tl.y ), y1 = row( rect.br.y - 1 );
	for (auto y = y0; y <= y1; ++y) {
		const auto *line = raster.pixels.data() + (size_t)y * size.w;
		if (std::any_of( line + x0, line + x1 + 1, [] ( uint8_t v ) { return v != 0; } ))
			return true;
	}
	return false;
}

std::vector<uint8_t> Prepared::window ( const Raster &raster, const Rtlbr<int> &rect ) const
{
	const auto w = width( rect ), h = height( rect );
	auto columns = std::vector<int>( (size_t)w );
	for (auto x = 0; x < w; ++x)
		columns[x] = column( rect.tl.x + x );
	auto out = std::vector<uint8_t>( (size_t)w * h );
	for (auto y = 0; y < h; ++y) {
		const auto *line = raster.pixels.data() + (size_t)row( rect.tl.y + y ) * size.w;
		auto *target = out.data() + (size_t)y * w;
		for (auto x = 0; x < w; ++x)
			target[x] = line[columns[x]];
	}
	return out;
}

} // namespace bridge_detail


namespace {

using namespace bridge_detail;
using Clock = std::chrono::steady_clock;


bool process_fail ( std::string *error, std::string message )
{
	if (error)
		*error = std::move( message );
	pyr::plog( "pyra process: %s\n", error ? error->c_str() : "" );
	return false;
}


double seconds_since ( Clock::time_point start )
{
	return std::chrono::duration<double>( Clock::now() - start ).count();
}


std::string path_utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return std::string( text.begin(), text.end() );
}


class StagingDirectory {
public:
	std::filesystem::path path;
	~StagingDirectory () {
		if (!path.empty()) {
			auto ignored = std::error_code{};
			std::filesystem::remove_all( path, ignored );
		}
	}
};


// Samples of one source pixel type as the wire carries them.
template <typename T> struct WireSample;
template <> struct WireSample<Rgb8> { static constexpr auto sample = BridgeSample::U8; static constexpr uint8_t channels = 3; };
template <> struct WireSample<Rgb16> { static constexpr auto sample = BridgeSample::U16; static constexpr uint8_t channels = 3; };
template <> struct WireSample<uint8_t> { static constexpr auto sample = BridgeSample::U8; static constexpr uint8_t channels = 1; };
template <> struct WireSample<uint16_t> { static constexpr auto sample = BridgeSample::U16; static constexpr uint8_t channels = 1; };


template <typename T>
std::vector<uint8_t> pack_rows ( const Pwhl<T> &image )
{
	auto bytes = std::vector<uint8_t>( (size_t)image.w * image.h * sizeof( T ) );
	for (auto y = 0; y < image.h; ++y)
		std::memcpy( bytes.data() + (size_t)y * image.w * sizeof( T ), image.rowp( y ), (size_t)image.w * sizeof( T ) );
	return bytes;
}


template <typename T>
Pwhl<T> read_rect ( WsiSession &session, const SourcePlane &source, const Rtlbr<int> &rect,
	float mpp, Dim2D<int> dims )
{
	auto metric = source.metric( rect );
	return session.loader()->Generate<T>( session.pyramid(), nullptr, mpp, metric, &dims, source.plane );
}


bool open_source ( WsiSession &session, const PlaneSel &plane, SourcePlane *source, std::string *error )
{
	const auto geometry = session.native_geometry( plane );
	if (!geometry || !geometry->valid())
		return process_fail( error, "the slide has no plane '" + plane.channel + "' at z=" + std::to_string( plane.zlevel ) );
	const auto &pyramid = session.pyramid();
	const auto &level = pyramid.levels[geometry->native_level_index];
	if (geometry->pixel_bounds.tl.x != 0 || geometry->pixel_bounds.tl.y != 0)
		return process_fail( error, "the level-0 frame grid does not start at the pixel origin" );
	if (level.pixel_type == LevelPixelType::Float32)
		return process_fail( error, "a float32 source plane cannot be sent to the bridge" );
	if (level.tile_w <= 0 || level.tile_h <= 0)
		return process_fail( error, "the source level has no frame size" );
	source->plane = plane;
	source->type = level.pixel_type;
	source->mpp = geometry->render_mpp;
	source->size = { width( geometry->pixel_bounds ), height( geometry->pixel_bounds ) };
	source->tile = { level.tile_w, level.tile_h };
	source->metric_origin = geometry->metric_bounds.tl;
	source->across = 1 + ( source->size.w - 1 ) / source->tile.w;
	source->down = 1 + ( source->size.h - 1 ) / source->tile.h;
	source->occupied.assign( (size_t)source->across * source->down, 0 );
	for (const auto &tile : level.tiles) {
		if (tile.fsz == 0 || tile.lr.is_empty())
			continue;
		const auto tx = tile.lr.tl.x / source->tile.w, ty = tile.lr.tl.y / source->tile.h;
		if (tx >= 0 && ty >= 0 && tx < source->across && ty < source->down)
			source->occupied[(size_t)ty * source->across + tx] = 1;
	}
	return true;
}


// Prepare pass: the whole plane at a coarse resolution through prepare().
template <typename T>
bool run_prepare ( WsiSession &session, const SourcePlane &source, const ChunkProcessOptions &options,
	const std::filesystem::path &python, Prepared *prepared, BridgeFields *params, std::string *error )
{
	const auto longest = (double)std::max( source.size.w, source.size.h );
	const auto mpp = options.prepare_mpp > 0.0 ? options.prepare_mpp
		: source.mpp * std::max( 1.0, longest / kPrepareMaxEdge );
//
// Rounded up, less a rounding allowance: at the default resolution the longest
// edge is exactly kPrepareMaxEdge in theory and a hair above it in floating
// point, which ceil would turn into one pixel too many.
	const auto edge = [&] ( int pixels ) {
		return std::max( 1, (int)std::ceil( pixels * (double)source.mpp / mpp - 1e-6 ) );
	};
	const auto dims = Dim2D<int>{ edge( source.size.w ), edge( source.size.h ) };
	const auto image = read_rect<T>( session, source, { { 0, 0 }, { source.size.w, source.size.h } }, (float)mpp, dims );
	if (!image.p || image.w != dims.w || image.h != dims.h)
		return process_fail( error, "cannot render the prepare image" );
	auto worker = BridgeWorker{};
	if (!worker.start( { python, options.worker_script, "prepare", options.prepare_script, 0, false }, error ))
		return false;
	auto text = std::string{};
	if (!format_bridge_fields( options.params, &text, error ))
		return false;
	auto header = BridgeHeader{};
	header.width = dims.w;
	header.height = dims.h;
	header.sample = (uint8_t)WireSample<T>::sample;
	header.channels = WireSample<T>::channels;
	header.value = mpp;
	const auto pixels = pack_rows( image );
	auto answer = BridgeMessage{};
	if (!worker.send( BridgeKind::Prepare, header, text, { std::span<const uint8_t>( pixels ) }, error ) ||
		!worker.receive( &answer, error ))
		return false;
	worker.stop();
	if ((BridgeKind)answer.header.kind != BridgeKind::Rasters || answer.header.width != dims.w ||
		answer.header.height != dims.h)
		return process_fail( error, "the prepare worker answered with an unexpected message" );
	prepared->size = dims;
	prepared->mpp = mpp;
	prepared->scale_x = (double)dims.w / source.size.w;
	prepared->scale_y = (double)dims.h / source.size.h;
	prepared->script = path_utf8( options.prepare_script.filename() );
	prepared->sha256 = worker.hello( "sha256" );
	const auto plane = (size_t)dims.w * dims.h;
	auto offset = size_t{};
	for (const auto &[key, value] : parse_bridge_fields( answer.text )) {
		if (key == "raster") {
			if (answer.payload.size() < offset + plane || prepared->find( value ))
				return process_fail( error, "prepare raster '" + value + "' is truncated or repeated" );
			prepared->rasters.push_back( { value, std::vector<uint8_t>( answer.payload.begin() + offset,
				answer.payload.begin() + offset + plane ) } );
			offset += plane;
		} else if (key.starts_with( "param:" ))
			params->emplace_back( key.substr( 6 ), value );
	}
	return true;
}


// Prepare parameters join the command-line ones; "store" is consumed here. The
// same name from both sides is an error, never a silent override.
bool merge_prepare_params ( const ChunkProcessOptions &options, BridgeFields prepared_params,
	Prepared *prepared, BridgeFields *merged, std::string *error )
{
	*merged = options.params;
	for (auto &[key, value] : prepared_params) {
		if (key == kBridgeStoreParam) {
			for (auto rest = std::string_view( value ); !rest.empty(); ) {
				const auto comma = rest.find( ',' );
				auto name = std::string( rest.substr( 0, comma ) );
				rest = comma == std::string_view::npos ? std::string_view{} : rest.substr( comma + 1 );
				std::erase( name, ' ' );
				if (name.empty())
					continue;
				if (!prepared->find( name ))
					return process_fail( error, "store= names '" + name + "', which prepare() did not return" );
				prepared->stored.push_back( name );
			}
			continue;
		}
		for (const auto &existing : *merged)
			if (existing.first == key)
				return process_fail( error, "parameter '" + key + "' is set both on the command line and by the prepare script" );
		merged->emplace_back( std::move( key ), std::move( value ) );
	}
	return true;
}


struct FrameSet {
	std::vector<std::pair<int64_t, std::vector<uint8_t>>> frames;
	bool python = false;
	uint32_t result_max = 0;			// largest sample of the chunk's result
};


struct WorkerTimes {
	double read = 0.0;
	double python = 0.0;
	double transfer = 0.0;
	double encode = 0.0;
	double rasters = 0.0;
};


// A chunk to process. Without Python it only carries the stored rasters: the
// chunk was skipped for processing (no source frame, or roi all zero) yet some
// stored raster is non-zero over it.
struct ChunkJob {
	Rtlbr<int> core;
	bool python = true;
};


struct ChunkRun {
	WsiSession *session = nullptr;
	const SourcePlane *source = nullptr;
	const Prepared *prepared = nullptr;
	const ChunkProcessOptions *options = nullptr;
	std::vector<ChunkJob> jobs;
	int chunk_levels = 0;
	std::atomic<size_t> next{ 0 };
	std::atomic<bool> failed{ false };
	std::mutex mutex;
	std::condition_variable ready;
	std::deque<FrameSet> queue;
	int active = 0;
	std::string error;
	uint32_t result_max = 0;			// over every result; main thread only

	void fail ( const std::string &message ) {
		auto guard = std::scoped_lock( mutex );
		if (!failed.exchange( true ))
			error = message;
		ready.notify_all();
	}
};


// Read a chunk plus halo, send it with its mask windows, take back the core.
template <typename T, typename P>
bool run_python ( ChunkRun &run, BridgeWorker &worker, size_t index, WorkerTimes &times,
	std::vector<P> *result, std::string *error )
{
	const auto &source = *run.source;
	const auto &core = run.jobs[index].core;
	const auto halo = run.options->halo;
	const auto rect = Rtlbr<int>{ { std::max( 0, core.tl.x - halo ), std::max( 0, core.tl.y - halo ) },
		{ std::min( source.size.w, core.br.x + halo ), std::min( source.size.h, core.br.y + halo ) } };
	const auto dims = Dim2D<int>{ width( rect ), height( rect ) };
	auto started = Clock::now();
	const auto image = read_rect<T>( *run.session, source, rect, source.mpp, dims );
	if (!image.p || image.w != dims.w || image.h != dims.h)
		return process_fail( error, std::format( "cannot read source pixels at ({}, {})", rect.tl.x, rect.tl.y ) );
	const auto pixels = pack_rows( image );
	auto text = std::string{};
	auto windows = std::vector<std::vector<uint8_t>>{};
	if (run.prepared)
		for (const auto &raster : run.prepared->rasters) {
			text += "mask=" + raster.name + "\n";
			windows.push_back( run.prepared->window( raster, rect ) );
		}
	times.read += seconds_since( started );
	auto header = BridgeHeader{};
	header.id = (uint32_t)index;
	header.x0 = rect.tl.x;
	header.y0 = rect.tl.y;
	header.width = dims.w;
	header.height = dims.h;
	header.sample = (uint8_t)WireSample<T>::sample;
	header.channels = WireSample<T>::channels;
	header.halo_left = core.tl.x - rect.tl.x;
	header.halo_top = core.tl.y - rect.tl.y;
	header.halo_right = rect.br.x - core.br.x;
	header.halo_bottom = rect.br.y - core.br.y;
	header.value = source.mpp;
	auto parts = std::vector<std::span<const uint8_t>>{ std::span<const uint8_t>( pixels ) };
	for (const auto &window : windows)
		parts.emplace_back( window );
	started = Clock::now();
	auto answer = BridgeMessage{};
	if (!worker.send( BridgeKind::Chunk, header, text, parts, error ) || !worker.receive( &answer, error ))
		return false;
	const auto round_trip = seconds_since( started );
	times.python += answer.header.value;
	times.transfer += std::max( 0.0, round_trip - answer.header.value );
	const auto core_dims = Dim2D<int>{ width( core ), height( core ) };
	if ((BridgeKind)answer.header.kind != BridgeKind::Result || answer.header.id != (uint32_t)index ||
		answer.header.width != core_dims.w || answer.header.height != core_dims.h ||
		answer.payload.size() != (size_t)core_dims.w * core_dims.h * sizeof( P ))
		return process_fail( error, "a chunk worker answered with an unexpected message" );
	result->resize( (size_t)core_dims.w * core_dims.h );
	std::memcpy( result->data(), answer.payload.data(), answer.payload.size() );
	return true;
}


// Cut one level of a block into frames and encode them. The block covers whole
// frames of the level, or reaches its edge. With skip_zero, a frame of zeros is
// left absent, as a stored raster's empty regions are.
template <typename P>
bool encode_level_frames ( const ChunkRun &run, int plane, int level, const std::vector<P> &block,
	Dim2D<int> block_size, Pxy<int> origin, bool skip_zero, FrameSet *out, std::string *error )
{
	const auto &source = *run.source;
	const auto tile = source.tile;
	const auto level_size = Dim2D<int>{ level_extent( source.size.w, level ), level_extent( source.size.h, level ) };
	const auto across = 1 + ( level_size.w - 1 ) / tile.w;
	auto encode = TileEncodeOptions{};
	encode.quality = kImageLosslessQuality;
	encode.bits_per_sample = sizeof( P ) * 8;
	encode.max_pixel_value = sizeof( P ) == 1 ? 255 : 65535;
	auto frame = Pwhl<P>( tile );
	for (auto ty = origin.y / tile.h; ty * tile.h < origin.y + block_size.h; ++ty)
		for (auto tx = origin.x / tile.w; tx * tile.w < origin.x + block_size.w; ++tx) {
			std::memset( frame.p, 0, (size_t)frame.h * frame.l * sizeof( P ) );
			const auto x0 = tx * tile.w, y0 = ty * tile.h;
			const auto w = std::min( tile.w, level_size.w - x0 );
			const auto h = std::min( tile.h, level_size.h - y0 );
			auto any = false;
			for (auto y = 0; y < h; ++y) {
				const auto *line = block.data() + (size_t)( y0 - origin.y + y ) * block_size.w + ( x0 - origin.x );
				std::memcpy( frame.rowp( y ), line, (size_t)w * sizeof( P ) );
				any = any || std::any_of( line, line + w, [] ( P v ) { return v != 0; } );
			}
			if (skip_zero && !any)
				continue;
			auto &entry = out->frames.emplace_back( spool_key( plane, level, (int64_t)ty * across + tx ), std::vector<uint8_t>{} );
			if (!encode_tile( run.options->codec, frame, &entry.second, encode, error ))
				return false;
		}
	return true;
}


// A level-0 block and the in-chunk levels halved from it, all cut and encoded.
template <typename P>
bool encode_chunk_pyramid ( const ChunkRun &run, int plane, std::vector<P> block, Rtlbr<int> core,
	bool skip_zero, FrameSet *out, std::string *error )
{
	auto size = Dim2D<int>{ width( core ), height( core ) };
	auto origin = core.tl;
	for (auto level = 0; ; ++level) {
		if (!encode_level_frames( run, plane, level, block, size, origin, skip_zero, out, error ))
			return false;
		if (level == run.chunk_levels)
			return true;
		auto halved = Dim2D<int>{};
		block = halve( block, size, &halved );
		size = halved;
		origin = { origin.x / 2, origin.y / 2 };
	}
}


// One job through one worker: Python (unless raster-only), then the frames of
// every level the chunk holds, for the result and for each stored raster.
template <typename T, typename P>
bool process_chunk ( ChunkRun &run, BridgeWorker &worker, size_t index, WorkerTimes &times,
	FrameSet *out, std::string *error )
{
	const auto &job = run.jobs[index];
	out->python = job.python;
	if (job.python) {
		auto result = std::vector<P>{};
		if (!run_python<T, P>( run, worker, index, times, &result, error ))
			return false;
		if (!result.empty())
			out->result_max = *std::max_element( result.begin(), result.end() );
		const auto started = Clock::now();
		if (!encode_chunk_pyramid<P>( run, 0, std::move( result ), job.core, false, out, error ))
			return false;
		times.encode += seconds_since( started );
	}
	if (!run.prepared)
		return true;
	const auto started = Clock::now();
	for (auto i = size_t{}; i < run.prepared->stored.size(); ++i) {
		const auto *raster = run.prepared->find( run.prepared->stored[i] );
		if (!encode_chunk_pyramid<uint8_t>( run, (int)i + 1, run.prepared->window( *raster, job.core ),
			job.core, true, out, error ))
			return false;
	}
	times.rasters += seconds_since( started );
	return true;
}


template <typename T, typename P>
void worker_loop ( ChunkRun &run, BridgeWorker &worker, WorkerTimes &times )
{
	for (;;) {
		if (run.failed)
			break;
		const auto index = run.next.fetch_add( 1 );
		if (index >= run.jobs.size())
			break;
		auto frames = FrameSet{};
		auto error = std::string{};
		if (!process_chunk<T, P>( run, worker, index, times, &frames, &error )) {
			run.fail( error );
			break;
		}
		auto guard = std::scoped_lock( run.mutex );
		run.queue.push_back( std::move( frames ) );
		run.ready.notify_all();
	}
	auto guard = std::scoped_lock( run.mutex );
	--run.active;
	run.ready.notify_all();
}


template <typename F>
bool visit_source_type ( LevelPixelType type, F &&f )
{
	switch (type) {
		case LevelPixelType::Rgb8: return f( Rgb8{} );
		case LevelPixelType::Rgb16: return f( Rgb16{} );
		case LevelPixelType::Gray8: return f( uint8_t{} );
		case LevelPixelType::Gray16: return f( uint16_t{} );
		case LevelPixelType::Float32: break;
	}
	return false;
}


// Runs the chunk workers and spools their frames as they arrive, in any order.
template <typename T, typename P>
bool run_chunks ( ChunkRun &run, std::vector<std::unique_ptr<BridgeWorker>> &workers, std::ofstream &spool,
	SpoolIndex *index, ChunkProcessStats *stats, const ChunkProcessProgress &progress, std::string *error )
{
	auto times = std::vector<WorkerTimes>( workers.size() );
	auto threads = std::vector<std::thread>{};
	run.active = (int)workers.size();
	for (auto i = size_t{}; i < workers.size(); ++i)
		threads.emplace_back( [&run, &workers, &times, i] { worker_loop<T, P>( run, *workers[i], times[i] ); } );
	auto offset = uint64_t{};
	auto done = size_t{};
	auto killed = false;
	auto assemble = 0.0;
	for (;;) {
		auto lock = std::unique_lock( run.mutex );
		run.ready.wait( lock, [&] { return !run.queue.empty() || run.active == 0 || ( run.failed && !killed ); } );
		if (run.failed && !killed) {
//
// Workers blocked on a Python answer wake only when their process goes away.
			killed = true;
			lock.unlock();
			for (auto &worker : workers)
				worker->kill();
			continue;
		}
		if (run.queue.empty()) {
			if (run.active == 0)
				break;
			continue;
		}
		auto set = std::move( run.queue.front() );
		run.queue.pop_front();
		lock.unlock();
		if (run.failed)
			continue;
		const auto started = Clock::now();
		for (const auto &[key, bytes] : set.frames) {
			spool.write( (const char *)bytes.data(), (std::streamsize)bytes.size() );
			(*index)[key] = { offset, (uint32_t)bytes.size() };
			offset += bytes.size();
		}
		assemble += seconds_since( started );
		if (!spool) {
			run.fail( "cannot write the frame spool" );
			continue;
		}
		++done;
		++( set.python ? stats->chunks_processed : stats->chunks_raster_only );
		run.result_max = std::max( run.result_max, set.result_max );
		if (progress)
			progress( "chunks", (size_t)stats->chunks_skipped + done, (size_t)stats->chunks_total );
	}
	for (auto &thread : threads)
		thread.join();
	for (const auto &t : times) {
		stats->read_s += t.read;
		stats->python_s += t.python;
		stats->transfer_s += t.transfer;
		stats->encode_s += t.encode;
		stats->rasters_s += t.rasters;
	}
	stats->assemble_s += assemble;
	if (run.failed)
		return process_fail( error, run.error );
	return true;
}


bool validate_options ( const std::filesystem::path &slide, const ChunkProcessOptions &options, std::string *error )
{
	auto extension = path_utf8( slide.extension() );
	std::transform( extension.begin(), extension.end(), extension.begin(),
		[] ( unsigned char c ) { return (char)std::tolower( c ); } );
#if !USE_DCMTK
	return process_fail( error, "this build has no DICOM support" );
#endif
	if (extension != ".dcm")
		return process_fail( error, "the bridge writes into DICOM slides only (.dcm)" );
	if (options.channel.empty() || options.channel.size() > 64 ||
		options.channel.find_first_of( "\\\r\n" ) != std::string::npos)
		return process_fail( error, "the channel name must be 1-64 characters without '\\' or line breaks" );
	if (options.output_type != LevelPixelType::Gray8 && options.output_type != LevelPixelType::Gray16)
		return process_fail( error, "DICOM WSI stores unsigned samples only: the output type must be gray8 or gray16" );
	if (( options.codec != ImgCodec::Jp2k && options.codec != ImgCodec::Htj2k ) || !img_codec_can_encode( options.codec ))
		return process_fail( error, std::string( "the codec must be JPEG 2000 or HTJ2K and built in: " ) + img_codec_name( options.codec ) );
	if (options.halo < 0 || options.chunk_tiles < 0 || options.workers < 0 || options.prepare_mpp < 0.0)
		return process_fail( error, "halo, chunk size, worker count and prepare resolution cannot be negative" );
	auto ec = std::error_code{};
	if (!std::filesystem::is_regular_file( options.process_script, ec ))
		return process_fail( error, "process script not found: " + path_utf8( options.process_script ) );
	if (!options.prepare_script.empty() && !std::filesystem::is_regular_file( options.prepare_script, ec ))
		return process_fail( error, "prepare script not found: " + path_utf8( options.prepare_script ) );
	if (!std::filesystem::is_regular_file( options.worker_script, ec ))
		return process_fail( error, "bridge worker not found: " + path_utf8( options.worker_script ) );
	return true;
}


// Default chunk edge in frames: the power of two nearest kChunkTargetEdge, so the
// chunk holds whole frames of as many reduced levels as possible.
int default_chunk_tiles ( Dim2D<int> tile )
{
	const auto ratio = (double)kChunkTargetEdge / std::max( tile.w, tile.h );
	return 1 << std::max( 0, (int)std::lround( std::log2( std::max( 1.0, ratio ) ) ) );
}


// The level-0 chunks, in row-major order, of whole source frames. A chunk with
// a source frame and a non-zero roi window goes through Python; one without but
// with a non-zero stored raster becomes a raster-only job; the rest are skipped.
std::vector<ChunkJob> plan_chunks ( const SourcePlane &source, const ChunkProcessOptions &options,
	const Prepared *prepared, Dim2D<int> *chunk, int *chunk_tiles, ChunkProcessStats *stats )
{
	const auto tiles = options.chunk_tiles > 0 ? options.chunk_tiles : default_chunk_tiles( source.tile );
	*chunk_tiles = tiles;
	*chunk = { tiles * source.tile.w, tiles * source.tile.h };
	const auto *roi = prepared ? prepared->find( kBridgeRoiRaster ) : nullptr;
	auto jobs = std::vector<ChunkJob>{};
	for (auto ty = 0; ty < source.down; ty += tiles)
		for (auto tx = 0; tx < source.across; tx += tiles) {
			++stats->chunks_total;
			auto present = false;
			for (auto y = ty; y < std::min( source.down, ty + tiles ) && !present; ++y)
				for (auto x = tx; x < std::min( source.across, tx + tiles ) && !present; ++x)
					present = source.frame_present( x, y );
			const auto core = Rtlbr<int>{ { tx * source.tile.w, ty * source.tile.h },
				{ std::min( source.size.w, ( tx + tiles ) * source.tile.w ),
				  std::min( source.size.h, ( ty + tiles ) * source.tile.h ) } };
			if (present && ( !roi || prepared->any( *roi, core ) )) {
				jobs.push_back( { core, true } );
				continue;
			}
			++stats->chunks_skipped;
			if (prepared && std::any_of( prepared->stored.begin(), prepared->stored.end(), [&] ( const std::string &name ) {
				return prepared->any( *prepared->find( name ), core );
			} ))
				jobs.push_back( { core, false } );
		}
	return jobs;
}


bool start_chunk_workers ( const ChunkProcessOptions &options, const std::filesystem::path &python,
	const BridgeFields &params, size_t count, std::vector<std::unique_ptr<BridgeWorker>> *workers,
	std::string *error )
{
	auto fields = params;
	fields.emplace_back( "__dtype", options.output_type == LevelPixelType::Gray16 ? "uint16" : "uint8" );
	auto text = std::string{};
	if (!format_bridge_fields( fields, &text, error ))
		return false;
	const auto start = [&] ( BridgeWorker &worker, int index, std::string *message ) {
		auto ready = BridgeMessage{};
		if (!worker.start( { python, options.worker_script, "process", options.process_script, index, true }, message ) ||
			!worker.send( BridgeKind::Setup, {}, text, {}, message ) || !worker.receive( &ready, message ))
			return false;
		if ((BridgeKind)ready.header.kind != BridgeKind::Ready)
			return process_fail( message, "a chunk worker did not acknowledge its setup" );
		return true;
	};
//
// A start waits for Python and the script's imports, a fraction of a second
// each, so the workers start side by side rather than one after another.
	for (auto i = size_t{}; i < count; ++i)
		workers->push_back( std::make_unique<BridgeWorker>() );
	auto started = std::vector<char>( count, 0 );
	auto messages = std::vector<std::string>( count );
	{
		auto threads = std::vector<std::jthread>{};
		for (auto i = size_t{}; i < count; ++i)
			threads.emplace_back( [&, i] { started[i] = start( *( *workers )[i], (int)i, &messages[i] ); } );
	}
	for (auto i = size_t{}; i < count; ++i)
		if (!started[i])
			return process_fail( error, messages[i].empty() ? "a chunk worker failed to start" : messages[i] );
	return true;
}


// Plane names and optical path identifiers the slide already uses.
bool existing_names ( const std::filesystem::path &slide, const WsiSession &session,
	std::set<std::string> *names, std::vector<std::string> *optical_ids, std::string *error )
{
	auto instances = std::vector<DcmSeriesInstance>{};
	if (!dcm_series_instances( slide, &instances, error ))
		return false;
	for (const auto &instance : instances) {
		if (instance.role == "VOLUME")
			names->insert( instance.plane_name() );
		optical_ids->insert( optical_ids->end(), instance.optical_ids.begin(), instance.optical_ids.end() );
	}
	for (const auto &stack : session.pyramid().planes)
		names->insert( stack.plane.channel );
	return true;
}

} // namespace


std::filesystem::path default_bridge_python ()
{
#if defined(_WIN32)
	auto *value = (wchar_t *)nullptr;
	auto length = size_t{};
	if (_wdupenv_s( &value, &length, L"PYRA_PYTHON" ) == 0 && value) {
		auto path = std::filesystem::path( value );
		std::free( value );
		if (!path.empty())
			return path;
	}
	return "python";
#else
	if (const auto *value = std::getenv( "PYRA_PYTHON" ); value && *value)
		return value;
	return "python3";
#endif
}


namespace {

std::string report_json_string ( std::string_view value )
{
	auto out = std::string( "\"" );
	for (const auto ch : value) {
		if (ch == '"' || ch == '\\') {
			out += '\\';
			out += ch;
		} else if ((unsigned char)ch < 0x20)
			out += std::format( "\\u{:04x}", (unsigned)(unsigned char)ch );
		else
			out += ch;
	}
	return out + '"';
}


// Wall time outside the four phases: opening the slide, reading its series and
// checking names before the first one starts.
double opening_seconds ( const ChunkProcessStats &stats )
{
	return std::max( 0.0, stats.total_s - stats.prepare_s - stats.chunks_s - stats.reduce_s - stats.publish_s );
}


// Average number of workers busy during the chunk phase: their summed time
// over its wall time. Below the worker count when they waited (reads take
// turns per slide) or were slowed by contention.
double busy_workers ( const ChunkProcessStats &stats )
{
	const auto work = stats.read_s + stats.python_s + stats.transfer_s + stats.encode_s + stats.rasters_s;
	return stats.chunks_s > 0.0 ? work / stats.chunks_s : 0.0;
}

} // namespace


std::string format_chunk_report ( const ChunkProcessStats &stats )
{
	auto channels = std::string{};
	for (const auto &channel : stats.channels)
		channels += ( channels.empty() ? "" : ", " ) + channel;
	const auto share = [&] ( double seconds ) {
		return stats.total_s > 0.0 ? std::format( "{:5.1f} %", 100.0 * seconds / stats.total_s ) : std::string{};
	};
	auto out = std::format( "Channels added: {}\n", channels );
	out += std::format( "Chunks: {} of {} processed by Python; {} skipped (no source frame, or roi all zero)",
		stats.chunks_processed, stats.chunks_total, stats.chunks_skipped );
	if (stats.chunks_raster_only)
		out += std::format( ", {} of which still written for kept masks", stats.chunks_raster_only );
	out += std::format( "\nOutput: {} frames, {:.1f} MB, in {} new DICOM files (one per channel and resolution level)\n",
		stats.frames_written, stats.encoded_bytes / 1e6, stats.published.size() );
	out += std::format( "\n{:<38}{:>10}{:>10}\n", "Phase (wall time)", "seconds", "share" );
	for (const auto &[name, seconds] : { std::pair{ "Opening and checks", opening_seconds( stats ) },
		std::pair{ "Prepare (coarse whole-slide pass)", stats.prepare_s },
		std::pair{ "Chunks (Python workers)", stats.chunks_s },
		std::pair{ "Assembly and coarser levels", stats.reduce_s },
		std::pair{ "Publish", stats.publish_s } })
		out += std::format( "  {:<36}{:>10.2f}{:>10}\n", name, seconds, share( seconds ) );
	out += std::format( "  {:<36}{:>10.2f}\n", "Total", stats.total_s );
	out += std::format( "\n{:<38}{:>10}\n", "Chunk work (summed over workers)", "seconds" );
	for (const auto &[name, seconds] : { std::pair{ "Read source pixels", stats.read_s },
		std::pair{ "Python", stats.python_s },
		std::pair{ "Transfer to and from Python", stats.transfer_s },
		std::pair{ "Encode result frames", stats.encode_s },
		std::pair{ "Encode kept masks", stats.rasters_s },
		std::pair{ "Spool (main thread)", stats.assemble_s } })
		out += std::format( "  {:<36}{:>10.2f}\n", name, seconds );
	out += std::format( "  {:<36}{:>10.1f}  of {} Python workers\n", "Workers busy on average", busy_workers( stats ), stats.workers );
	return out;
}


std::string format_chunk_report_json ( const ChunkProcessStats &stats )
{
	auto channels = std::string{};
	for (const auto &channel : stats.channels)
		channels += ( channels.empty() ? "" : "," ) + report_json_string( channel );
	return std::format( "{{\"channels\":[{}],\"chunks\":{{\"total\":{},\"processed\":{},\"skipped\":{},\"mask_only\":{}}},"
		"\"workers\":{},\"busy_workers\":{:.2f},\"frames\":{},\"encoded_bytes\":{},\"files\":{},"
		"\"time\":{{\"opening\":{:.3f},\"prepare\":{:.3f},\"chunks\":{:.3f},\"assemble\":{:.3f},\"publish\":{:.3f},\"total\":{:.3f}}},"
		"\"work\":{{\"read\":{:.3f},\"python\":{:.3f},\"transfer\":{:.3f},\"encode\":{:.3f},\"masks\":{:.3f},\"spool\":{:.3f}}}}}",
		channels, stats.chunks_total, stats.chunks_processed, stats.chunks_skipped, stats.chunks_raster_only,
		stats.workers, busy_workers( stats ), stats.frames_written, stats.encoded_bytes, stats.published.size(),
		opening_seconds( stats ), stats.prepare_s, stats.chunks_s, stats.reduce_s, stats.publish_s, stats.total_s,
		stats.read_s, stats.python_s, stats.transfer_s, stats.encode_s, stats.rasters_s, stats.assemble_s );
}


bool process_dcm_chunks ( const std::filesystem::path &input, const ChunkProcessOptions &options,
	ChunkProcessStats *stats, std::string *error, const ChunkProcessProgress &progress )
{
	const auto started = Clock::now();
	auto local = ChunkProcessStats{};
	auto &s = stats ? *stats : local;
	s = {};
	if (error)
		error->clear();
	try {
		const auto slide = std::filesystem::absolute( input ).lexically_normal();
		if (!validate_options( slide, options, error ))
			return false;
		const auto python = options.python.empty() ? default_bridge_python() : options.python;
		auto session = WsiSession{};
		if (!session.open( slide ))
			return process_fail( error, "cannot open the slide: " + path_utf8( slide ) );
		auto source = SourcePlane{};
		auto names = std::set<std::string>{};
		auto optical_ids = std::vector<std::string>{};
		if (!open_source( session, options.source, &source, error ) ||
			!existing_names( slide, session, &names, &optical_ids, error ))
			return false;
		if (names.contains( options.channel ))
			return process_fail( error, "the slide already has a plane named '" + options.channel + "'" );

		auto prepared = Prepared{};
		auto params = options.params;
		const auto has_prepare = !options.prepare_script.empty();
		if (has_prepare) {
			const auto prepare_started = Clock::now();
			if (progress)
				progress( "prepare", 0, 1 );
			auto prepare_params = BridgeFields{};
			const auto ok = visit_source_type( source.type, [&] ( auto pixel ) {
				return run_prepare<decltype( pixel )>( session, source, options, python, &prepared, &prepare_params, error );
			} );
			if (!ok || !merge_prepare_params( options, std::move( prepare_params ), &prepared, &params, error ))
				return false;
			for (const auto &name : prepared.stored)
				if (names.contains( options.channel + "." + name ))
					return process_fail( error, "the slide already has a plane named '" + options.channel + "." + name + "'" );
			s.prepare_s = seconds_since( prepare_started );
		}

		auto chunk = Dim2D<int>{};
		auto chunk_tiles = 0;
		auto run = ChunkRun{};
		run.jobs = plan_chunks( source, options, has_prepare ? &prepared : nullptr, &chunk, &chunk_tiles, &s );
		const auto python_jobs = (size_t)std::count_if( run.jobs.begin(), run.jobs.end(), [] ( const ChunkJob &job ) { return job.python; } );
		if (python_jobs == 0)
			return process_fail( error, "every chunk was skipped: no source frame or roi pixel to process" );
		run.chunk_levels = in_chunk_levels( chunk_tiles, reduced_level_count( source.size, source.tile ) );
		auto staging = StagingDirectory{};
		staging.path = slide.parent_path() / pyr::path_from_utf8( std::format( ".pyra-process-{}",
			Clock::now().time_since_epoch().count() ) );
		std::filesystem::create_directories( staging.path );
		const auto spool_path = staging.path / "frames.spool";
		auto spool = std::ofstream( spool_path, std::ios::binary );
		if (!spool)
			return process_fail( error, "cannot create the frame spool in " + path_utf8( staging.path ) );

		const auto chunks_started = Clock::now();
		const auto hardware = std::max( 2u, std::thread::hardware_concurrency() );
		const auto count = std::min( python_jobs, (size_t)( options.workers > 0 ? options.workers : (int)hardware / 2 ) );
		auto workers = std::vector<std::unique_ptr<BridgeWorker>>{};
		if (!start_chunk_workers( options, python, params, count, &workers, error ))
			return false;
		s.workers = (int)workers.size();
		run.session = &session;
		run.source = &source;
		run.prepared = has_prepare ? &prepared : nullptr;
		run.options = &options;
		auto index = SpoolIndex{};
		const auto ran = visit_source_type( source.type, [&] ( auto pixel ) {
			using T = decltype( pixel );
			return options.output_type == LevelPixelType::Gray16
				? run_chunks<T, uint16_t>( run, workers, spool, &index, &s, progress, error )
				: run_chunks<T, uint8_t>( run, workers, spool, &index, &s, progress, error );
		} );
		const auto process_sha256 = workers.front()->hello( "sha256" );
		for (auto &worker : workers)
			worker->stop();
		spool.close();
		if (!ran)
			return false;
		s.chunks_s = seconds_since( chunks_started );

		auto assembly = AssemblyInput{};
		assembly.slide = slide;
		assembly.staging = staging.path;
		assembly.spool = spool_path;
		assembly.options = &options;
		assembly.source = &source;
		assembly.prepared = has_prepare ? &prepared : nullptr;
		assembly.index = &index;
		assembly.process_script = path_utf8( options.process_script.filename() );
		assembly.process_sha256 = process_sha256;
		assembly.params = params;
		assembly.chunk = chunk;
		assembly.chunk_levels = run.chunk_levels;
		assembly.result_max = run.result_max;
		assembly.optical_ids_in_use = optical_ids;
		if (!assemble_dcm_channels( assembly, &s, error ))
			return false;
		s.total_s = seconds_since( started );
		return true;
	} catch (const std::exception &exception) {
		return process_fail( error, std::string( "unexpected failure: " ) + exception.what() );
	}
}

} // namespace priml
