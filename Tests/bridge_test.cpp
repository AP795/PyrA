// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// The DICOM channel helpers and the Python processing bridge. The DICOM tests
// need a DCMTK build; the worker tests need Python with numpy (PYRA_PYTHON,
// else python/python3 on PATH); the bridge tests need both. Whatever cannot run
// is reported as skipped, and exit code 77 says so when nothing failed.

#include "bridge_worker.h"
#include "chunk_process.h"
#include "dcm_series.h"
#include "dcm_write.h"
#include "pyra_subprocess.h"
#include "wsi.h"
#include "wsi_session.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr auto kSlideSize = priml::Dim2D<int>{ 700, 450 };
constexpr auto kFrame = priml::Dim2D<int>{ 128, 128 };
constexpr auto kMpp = 0.5;
constexpr auto kSkipped = 77;


void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


[[maybe_unused]] priml::Rgb source_rgb ( int x, int y )
{
	return { (uint8_t)( ( x * 7 + y ) % 256 ), (uint8_t)( ( x + 3 * y ) % 256 ), (uint8_t)( ( x ^ y ) % 256 ) };
}


[[maybe_unused]] priml::DcmAcquisition scanner_metadata ()
{
	auto meta = priml::DcmAcquisition{};
	meta.uid_root = "1.2.826.0.1.3680043.10.987";
	meta.name = "slide";
	meta.acquisition_datetime = "20261122101500+0000";
	meta.focus_method = "AUTO";
	meta.imaged_depth_um = 1.5;
	meta.illumination_color = "WHITE";
//
// Not the writer identity: the slide's own channel must read as acquired.
	meta.manufacturer = "Test scanner";
	meta.model = "Synthetic";
	meta.serial = "T1";
	meta.software = "1";
	return meta;
}


// An RGB slide published as PyrA lays one out: slide.dcm plus _slide_/.
[[maybe_unused]] fs::path make_slide ( const fs::path &directory )
{
	fs::remove_all( directory );
	fs::create_directories( directory );
	auto writer = priml::DcmWriter{};
	auto error = std::string{};
	require( writer.open( directory / "stage", scanner_metadata(), &error ), error );
	auto spec = priml::DcmLevelSpec{};
	spec.pixel_type = priml::LevelPixelType::Rgb8;
	spec.max_pixel_value = 255;
	spec.mpp = kMpp;
	spec.size = kSlideSize;
	spec.tile = kFrame;
	spec.codec = priml::kBridgeDefaultCodec;
	spec.encode.quality = priml::kImageLosslessQuality;
	const auto fetch = priml::DcmTileFetch<priml::Rgb>( [] ( const auto &rect, auto *out, std::string * ) {
		for (auto y = rect.tl.y; y < rect.br.y; ++y)
			for (auto x = rect.tl.x; x < rect.br.x; ++x)
				out->rowp( y - rect.tl.y )[x - rect.tl.x] = source_rgb( x, y );
		return priml::DcmFetchResult::Pixels;
	} );
	require( writer.add_level<priml::Rgb>( spec, fetch, &error ) && writer.close( &error ), error );
	auto stats = writer.stats();
	const auto entry = directory / "slide.dcm";
	require( priml::publish_dcm_pyramid( entry, &stats, &error ), error );
	fs::remove_all( directory / "stage" );
	return entry;
}


// A whole plane at level 0, row-major; empty when the plane is missing.
template <typename P>
std::vector<P> read_plane ( const fs::path &entry, const std::string &name, size_t *ladder = nullptr )
{
	auto session = priml::WsiSession{};
	require( session.open( entry ), "slide does not reopen" );
	const auto *stack = priml::find_plane( session.pyramid(), { name, 0.0f } );
	if (!stack)
		return {};
	if (ladder)
		*ladder = stack->ladder.size();
	const auto geometry = session.native_geometry( { name, 0.0f } );
	require( geometry.has_value(), "plane has no geometry" );
	const auto dims = priml::Dim2D<int>{ width( geometry->pixel_bounds ), height( geometry->pixel_bounds ) };
	const auto image = session.loader()->Generate<P>( session.pyramid(), nullptr, geometry->render_mpp,
		geometry->metric_bounds, &dims, { name, 0.0f } );
	require( image.p && image.w == dims.w && image.h == dims.h, "plane '" + name + "' does not render" );
	auto pixels = std::vector<P>{};
	for (auto y = 0; y < image.h; ++y)
		pixels.insert( pixels.end(), image.rowp( y ), image.rowp( y ) + image.w );
	return pixels;
}


// One level of a plane, rendered 1:1 at that level's resolution.
template <typename P>
std::vector<P> read_level ( const fs::path &entry, const std::string &name, int level, priml::Dim2D<int> *size )
{
	auto session = priml::WsiSession{};
	require( session.open( entry ), "slide does not reopen" );
	const auto geometry = session.native_geometry( { name, 0.0f } );
	require( geometry.has_value(), "plane has no geometry" );
	*size = kSlideSize;
	for (auto n = 0; n < level; ++n)
		*size = { 1 + ( size->w - 1 ) / 2, 1 + ( size->h - 1 ) / 2 };
	const auto mpp = geometry->render_mpp * (float)( 1 << level );
	const auto metric = priml::Rtlbr<float>{ geometry->metric_bounds.tl,
		{ geometry->metric_bounds.tl.x + size->w * mpp, geometry->metric_bounds.tl.y + size->h * mpp } };
	const auto image = session.loader()->Generate<P>( session.pyramid(), nullptr, mpp, metric, size, { name, 0.0f } );
	require( image.p && image.w == size->w && image.h == size->h, std::format( "level {} of '{}' does not render", level, name ) );
	auto pixels = std::vector<P>{};
	for (auto y = 0; y < image.h; ++y)
		pixels.insert( pixels.end(), image.rowp( y ), image.rowp( y ) + image.w );
	return pixels;
}


// The 2x2 mean the workers build, from a level given as a function.
template <typename F>
std::vector<uint32_t> mean_level ( priml::Dim2D<int> size, F &&at, priml::Dim2D<int> *halved )
{
	*halved = { 1 + ( size.w - 1 ) / 2, 1 + ( size.h - 1 ) / 2 };
	auto out = std::vector<uint32_t>( (size_t)halved->w * halved->h );
	for (auto y = 0; y < halved->h; ++y)
		for (auto x = 0; x < halved->w; ++x) {
			auto sum = 0u, count = 0u;
			for (auto dy = 0; dy < 2; ++dy)
				for (auto dx = 0; dx < 2; ++dx)
					if (2 * x + dx < size.w && 2 * y + dy < size.h) {
						sum += at( 2 * x + dx, 2 * y + dy );
						++count;
					}
			out[(size_t)y * halved->w + x] = ( sum + count / 2 ) / count;
		}
	return out;
}


[[maybe_unused]] std::vector<std::string> plane_names ( const fs::path &entry )
{
	auto session = priml::WsiSession{};
	require( session.open( entry ), "slide does not reopen" );
	auto names = std::vector<std::string>{};
	for (const auto &stack : session.pyramid().planes)
		names.push_back( stack.plane.channel );
	std::sort( names.begin(), names.end() );
	return names;
}


// Every file under the slide's folder with its size: a failed run must leave it as it was.
[[maybe_unused]] std::map<std::string, uintmax_t> snapshot ( const fs::path &directory )
{
	auto files = std::map<std::string, uintmax_t>{};
	for (const auto &item : fs::recursive_directory_iterator( directory ))
		files[item.path().lexically_relative( directory ).generic_string()] =
			item.is_regular_file() ? item.file_size() : 0;
	return files;
}


fs::path write_script ( const fs::path &directory, const std::string &name, const std::string &body )
{
	const auto path = directory / name;
	auto out = std::ofstream( path, std::ios::binary );
	out << body;
	require( (bool)out, "cannot write " + path.string() );
	return path;
}


[[maybe_unused]] priml::ChunkProcessOptions bridge_options ( const fs::path &process_script, const std::string &channel )
{
	auto options = priml::ChunkProcessOptions{};
	options.process_script = process_script;
	options.worker_script = PYRA_BRIDGE_WORKER;
	options.channel = channel;
	options.workers = 2;
	options.uid_root = "1.2.826.0.1.3680043.10.987";
	return options;
}


bool python_with_numpy ()
{
	auto process = priml::Subprocess{};
	auto spec = priml::SubprocessSpec{};
	spec.executable = priml::default_bridge_python();
	spec.arguments = { "-c", "import numpy" };
	auto error = std::string{};
	if (!process.start( spec, &error ))
		return false;
	process.close_input();
	return process.wait() == 0;
}


// --- Worker protocol (no DICOM needed) ----------------------------------------

void test_bridge_worker_round_trip ( const fs::path &directory )
{
	fs::create_directories( directory );
	const auto script = write_script( directory, "triple.py",
		"def setup(params):\n"
		"    global k\n"
		"    k = int(params['k'])\n"
		"def process(chunk, ctx):\n"
		"    assert (ctx.x0, ctx.y0, ctx.halo) == (10, 20, (1, 1, 1, 1)), (ctx.x0, ctx.y0, ctx.halo)\n"
		"    return (chunk * k).astype(chunk.dtype)\n" );
	auto worker = priml::BridgeWorker{};
	auto error = std::string{};
	require( worker.start( { priml::default_bridge_python(), PYRA_BRIDGE_WORKER, "process", script, 0, true }, &error ), error );
	require( worker.hello( "sha256" ).size() == 64, "Hello carries no script hash" );
	auto answer = priml::BridgeMessage{};
	require( worker.send( priml::BridgeKind::Setup, {}, "k=3\n__dtype=uint8\n", {}, &error ) &&
		worker.receive( &answer, &error ) && (priml::BridgeKind)answer.header.kind == priml::BridgeKind::Ready, error );
	auto header = priml::BridgeHeader{};
	header.id = 7;
	header.x0 = 10;
	header.y0 = 20;
	header.width = 5;
	header.height = 4;
	header.sample = (uint8_t)priml::BridgeSample::U8;
	header.channels = 1;
	header.halo_left = header.halo_top = header.halo_right = header.halo_bottom = 1;
	header.value = kMpp;
	auto pixels = std::vector<uint8_t>( 20 );
	std::iota( pixels.begin(), pixels.end(), uint8_t{} );
	require( worker.send( priml::BridgeKind::Chunk, header, {}, { std::span<const uint8_t>( pixels ) }, &error ) &&
		worker.receive( &answer, &error ), error );
	require( (priml::BridgeKind)answer.header.kind == priml::BridgeKind::Result && answer.header.id == 7 &&
		answer.header.width == 3 && answer.header.height == 2 && answer.payload.size() == 6, "unexpected result message" );
	for (auto y = 0; y < 2; ++y)
		for (auto x = 0; x < 3; ++x)
			require( answer.payload[(size_t)y * 3 + x] == ( ( y + 1 ) * 5 + x + 1 ) * 3, "halo not cropped or wrong values" );
	worker.stop();
}


void test_bridge_worker_reports_exception ( const fs::path &directory )
{
	fs::create_directories( directory );
	const auto script = write_script( directory, "broken.py", "def process(chunk, ctx):\n    raise KeyError('missing input')\n" );
	auto worker = priml::BridgeWorker{};
	auto error = std::string{};
	require( worker.start( { priml::default_bridge_python(), PYRA_BRIDGE_WORKER, "process", script, 0, true }, &error ), error );
	auto answer = priml::BridgeMessage{};
	require( worker.send( priml::BridgeKind::Setup, {}, "__dtype=uint8\n", {}, &error ) && worker.receive( &answer, &error ), error );
	auto header = priml::BridgeHeader{};
	header.width = header.height = 2;
	header.sample = (uint8_t)priml::BridgeSample::U8;
	header.channels = 1;
	const auto pixels = std::vector<uint8_t>( 4 );
	require( worker.send( priml::BridgeKind::Chunk, header, {}, { std::span<const uint8_t>( pixels ) }, &error ), error );
	require( !worker.receive( &answer, &error ) && error.find( "missing input" ) != std::string::npos,
		"the worker's exception did not come back: " + error );
	worker.stop();
}


void test_bridge_worker_import_error ( const fs::path &directory )
{
	fs::create_directories( directory );
	const auto script = write_script( directory, "bad_import.py", "import no_such_module_for_pyra\n" );
	auto worker = priml::BridgeWorker{};
	auto error = std::string{};
	require( !worker.start( { priml::default_bridge_python(), PYRA_BRIDGE_WORKER, "process", script, 0, true }, &error ) &&
		error.find( "no_such_module_for_pyra" ) != std::string::npos, "an import failure was not reported: " + error );
}


#if USE_DCMTK

// --- DICOM channel helpers ----------------------------------------------------

// A gray channel joined to the slide from frames encoded here, in the first and
// last frame columns only; the frames between stay absent.
void add_encoded_channel ( const fs::path &entry, const fs::path &stage, const std::string &name,
	const std::string &processing )
{
	auto meta = priml::DcmAcquisition{};
	auto origin = priml::Pxy<double>{};
	auto error = std::string{};
	require( priml::dcm_slide_acquisition( entry, &meta, &origin, &error ), error );
	meta.uid_root = "1.2.826.0.1.3680043.10.987";
	meta.name = name;
	meta.manufacturer = priml::dcm_writer_identity().manufacturer;
	meta.model = priml::dcm_writer_identity().model;
	meta.serial = "1";
	meta.software = "test";
	auto writer = priml::DcmWriter{};
	require( writer.open( stage, meta, &error ), error );
	auto spec = priml::DcmLevelSpec{};
	spec.channel = name;
	spec.optical_path_id = name;
	spec.processing = processing;
	spec.pixel_type = priml::LevelPixelType::Gray8;
	spec.max_pixel_value = 255;
	spec.mpp = kMpp;
	spec.size = kSlideSize;
	spec.tile = kFrame;
	spec.codec = priml::kBridgeDefaultCodec;
	spec.encode.quality = priml::kImageLosslessQuality;
	spec.sparse = true;
//
// Frames of the first and last frame column only; the rest stays absent.
	spec.coverage = { { { 0, 0 }, { kFrame.w, kSlideSize.h } }, { { 5 * kFrame.w, 0 }, { kSlideSize.w, kSlideSize.h } } };
	const auto fetch = priml::DcmEncodedFetch( [&] ( const priml::Rtlbr<int> &rect, std::vector<uint8_t> *bytes, std::string *e ) {
		auto frame = priml::Pwhl<uint8_t>( kFrame );
		for (auto y = rect.tl.y; y < rect.br.y; ++y)
			for (auto x = rect.tl.x; x < rect.br.x; ++x)
				frame.rowp( y - rect.tl.y )[x - rect.tl.x] = (uint8_t)( 1 + ( x + y ) % 200 );
		auto encode = priml::TileEncodeOptions{};
		encode.quality = priml::kImageLosslessQuality;
		return priml::encode_tile( spec.codec, frame, bytes, encode, e ) ? priml::DcmFetchResult::Pixels : priml::DcmFetchResult::Error;
	} );
	require( writer.add_encoded_level( spec, fetch, &error ) && writer.close( &error ), error );
	auto stats = writer.stats();
	require( priml::publish_dcm_companions( entry, &stats, &error ), error );
	fs::remove_all( stage );
}


void test_dcm_encoded_level_round_trip ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	add_encoded_channel( entry, directory / "stage", "ENC", std::string( priml::kDcmDerivedChannelMarker ) + " test" );
	const auto pixels = read_plane<uint8_t>( entry, "ENC" );
	require( pixels.size() == (size_t)kSlideSize.w * kSlideSize.h, "encoded plane missing" );
	for (auto y = 0; y < kSlideSize.h; ++y)
		for (auto x = 0; x < kSlideSize.w; ++x) {
			const auto covered = x < kFrame.w || x >= 5 * kFrame.w;
			const auto expected = covered ? (uint8_t)( 1 + ( x + y ) % 200 ) : uint8_t{ 0 };
			require( pixels[(size_t)y * kSlideSize.w + x] == expected, std::format( "pixel ({},{}) wrong", x, y ) );
		}
}


// The writer identity and a PROCESSING item do not make a channel removable: a
// converter records its own processing on acquired channels. Only the marker does.
void test_dcm_remove_channel_guarded ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	add_encoded_channel( entry, directory / "stage", "ENC", std::string( priml::kDcmDerivedChannelMarker ) + " test" );
	add_encoded_channel( entry, directory / "stage", "ACQ", "flat-field ref=W coef=[1]" );
	require( plane_names( entry ) == std::vector<std::string>{ "", "ACQ", "ENC" }, "channels did not join the slide" );
	auto removed = std::vector<fs::path>{};
	auto error = std::string{};
	require( !priml::remove_dcm_channel( entry, "ACQ", &removed, &error ) &&
		error.find( "not a derived channel" ) != std::string::npos, "an acquired channel was not protected" );
	require( !priml::remove_dcm_channel( directory / "_slide_" / "ENC.dcm", "ENC", &removed, &error ) &&
		error.find( "opened through" ) != std::string::npos, "the file the slide was opened through was removable" );
	require( priml::remove_dcm_channel( entry, "ENC", &removed, &error ) && removed.size() == 1, error );
	require( plane_names( entry ) == std::vector<std::string>{ "", "ACQ" }, "derived plane survived its removal" );
	require( !priml::remove_dcm_channel( entry, "ENC", &removed, &error ), "a missing channel was removed" );
}


// --- Python bridge ------------------------------------------------------------

void test_bridge_identity_gray8 ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "red.py", "def process(chunk, ctx):\n    return chunk[..., 0].copy()\n" );
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, bridge_options( script, "R" ), &stats, &error ), error );
	const auto red = read_plane<uint8_t>( entry, "R" );
	require( red.size() == (size_t)kSlideSize.w * kSlideSize.h, "result plane missing" );
	for (auto y = 0; y < kSlideSize.h; ++y)
		for (auto x = 0; x < kSlideSize.w; ++x)
			require( red[(size_t)y * kSlideSize.w + x] == source_rgb( x, y ).r, std::format( "pixel ({},{}) differs", x, y ) );
	const auto rgb = read_plane<priml::Rgb>( entry, "" );
	require( rgb.size() == red.size() && rgb[12345].g == source_rgb( 12345 % kSlideSize.w, 12345 / kSlideSize.w ).g,
		"the RGB plane changed" );
}


void test_bridge_halo_placement ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "coords.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    h, w = chunk.shape[:2]\n"
		"    y, x = np.mgrid[0:h, 0:w]\n"
		"    return ((ctx.x0 + x + 2 * (ctx.y0 + y)) % 251).astype(np.uint8)\n" );
	auto options = bridge_options( script, "XY" );
	options.halo = 37;
	options.chunk_tiles = 1;
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, &stats, &error ), error );
	require( stats.chunks_processed == 24, std::format( "{} chunks, expected 24", stats.chunks_processed ) );
	const auto pixels = read_plane<uint8_t>( entry, "XY" );
	for (auto y = 0; y < kSlideSize.h; ++y)
		for (auto x = 0; x < kSlideSize.w; ++x)
			require( pixels[(size_t)y * kSlideSize.w + x] == ( x + 2 * y ) % 251, std::format( "pixel ({},{}) misplaced", x, y ) );
}


void test_bridge_prepare_params_reach_workers ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "prep.py", "def prepare(image, ctx):\n    return {}, {'answer': '42'}\n" );
	const auto script = write_script( directory, "fill.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    return np.full(chunk.shape[:2], int(ctx.params['answer']) + int(ctx.params['base']), np.uint8)\n" );
	auto options = bridge_options( script, "P" );
	options.prepare_script = prepare;
	options.params = { { "base", "1" } };
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, &stats, &error ), error );
	const auto pixels = read_plane<uint8_t>( entry, "P" );
	require( !pixels.empty() && std::all_of( pixels.begin(), pixels.end(), [] ( uint8_t v ) { return v == 43; } ),
		"prepare parameters did not reach every chunk" );
}


void test_bridge_roi_skips_chunks ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "left.py",
		"import numpy as np\n"
		"def prepare(image, ctx):\n"
		"    roi = np.zeros(image.shape[:2], bool)\n"
		"    roi[:, : image.shape[1] // 4] = True\n"
		"    return {'roi': roi}, {}\n" );
	const auto script = write_script( directory, "flat.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    return np.full(chunk.shape[:2], 200, np.uint8)\n" );
	auto options = bridge_options( script, "ROI" );
	options.prepare_script = prepare;
	options.chunk_tiles = 1;
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, &stats, &error ), error );
	require( stats.chunks_skipped == 24 - 8 && stats.chunks_processed == 8,
		std::format( "{} skipped, {} processed", stats.chunks_skipped, stats.chunks_processed ) );
	const auto pixels = read_plane<uint8_t>( entry, "ROI" );
	require( pixels[10] == 200 && pixels[(size_t)300 * kSlideSize.w + 650] == 0, "skipped chunks were written" );
}


void test_bridge_mask_window_nearest ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "checker.py",
		"import numpy as np\n"
		"def prepare(image, ctx):\n"
		"    y, x = np.mgrid[0:image.shape[0], 0:image.shape[1]]\n"
		"    return {'m': ((x + y) % 2 * 255).astype(np.uint8)}, {}\n" );
	const auto script = write_script( directory, "mask.py", "def process(chunk, ctx):\n    return ctx.masks['m'].copy()\n" );
	auto options = bridge_options( script, "M" );
	options.prepare_script = prepare;
	options.prepare_mpp = kMpp * 3.3;
	options.halo = 5;
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, &stats, &error ), error );
	const auto coarse = priml::Dim2D<int>{ (int)std::ceil( kSlideSize.w / 3.3 ), (int)std::ceil( kSlideSize.h / 3.3 ) };
	const auto pixels = read_plane<uint8_t>( entry, "M" );
	for (auto y = 0; y < kSlideSize.h; ++y)
		for (auto x = 0; x < kSlideSize.w; ++x) {
			const auto cx = std::min( coarse.w - 1, (int)std::floor( ( x + 0.5 ) * coarse.w / kSlideSize.w ) );
			const auto cy = std::min( coarse.h - 1, (int)std::floor( ( y + 0.5 ) * coarse.h / kSlideSize.h ) );
			require( pixels[(size_t)y * kSlideSize.w + x] == ( cx + cy ) % 2 * 255, std::format( "mask pixel ({},{}) wrong", x, y ) );
		}
}


void test_bridge_store_raster_level0 ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "keep.py",
		"import numpy as np\n"
		"def prepare(image, ctx):\n"
		"    roi = np.zeros(image.shape[:2], bool)\n"
		"    roi[: image.shape[0] // 2] = True\n"
		"    return {'roi': roi}, {'store': 'roi'}\n" );
	const auto script = write_script( directory, "one.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    return np.ones(chunk.shape[:2], np.uint8)\n" );
	auto options = bridge_options( script, "S" );
	options.prepare_script = prepare;
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, &stats, &error ), error );
	require( stats.channels == std::vector<std::string>{ "S", "S.roi" }, "stored raster not reported" );
	auto ladder = size_t{};
	const auto roi = read_plane<uint8_t>( entry, "S.roi", &ladder );
	require( !roi.empty() && ladder >= 2, "stored raster plane or its ladder is missing" );
	require( roi[5] == 255 && roi[(size_t)( kSlideSize.h - 3 ) * kSlideSize.w + 5] == 0, "stored raster pixels wrong" );
}


void test_bridge_param_collision_refused ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "prep.py", "def prepare(image, ctx):\n    return {}, {'answer': '42'}\n" );
	const auto script = write_script( directory, "id.py", "def process(chunk, ctx):\n    return chunk[..., 0].copy()\n" );
	auto options = bridge_options( script, "C" );
	options.prepare_script = prepare;
	options.params = { { "answer", "1" } };
	auto error = std::string{};
	require( !priml::process_dcm_chunks( entry, options, nullptr, &error ) && error.find( "both" ) != std::string::npos,
		"a parameter collision was accepted: " + error );
}


void test_bridge_failure_leaves_slide ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "boom.py", "def process(chunk, ctx):\n    return 1 / 0\n" );
	const auto before = snapshot( directory );
	auto error = std::string{};
	require( !priml::process_dcm_chunks( entry, bridge_options( script, "B" ), nullptr, &error ) &&
		error.find( "ZeroDivisionError" ) != std::string::npos, "the script's failure was not reported: " + error );
	require( snapshot( directory ) == before, "a failed run changed the slide folder" );
}


void test_bridge_existing_channel_refused ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "id.py", "def process(chunk, ctx):\n    return chunk[..., 1].copy()\n" );
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, bridge_options( script, "G" ), nullptr, &error ), error );
	require( !priml::process_dcm_chunks( entry, bridge_options( script, "G" ), nullptr, &error ) &&
		error.find( "already has" ) != std::string::npos, "a second run over an existing channel was accepted" );
}


void test_bridge_stdout_print ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "chatty.py",
		"def process(chunk, ctx):\n    print('chunk', ctx.chunk_id, chunk.shape)\n    return chunk[..., 2].copy()\n" );
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, bridge_options( script, "Chatty" ), nullptr, &error ), error );
	require( !read_plane<uint8_t>( entry, "Chatty" ).empty(), "a printing script broke the run" );
}


void test_bridge_gray16_ladder ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "wide.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    h, w = chunk.shape[:2]\n"
		"    y, x = np.mgrid[0:h, 0:w]\n"
		"    return ((ctx.x0 + x) * 90 + (ctx.y0 + y)).astype(np.uint16)\n" );
	auto options = bridge_options( script, "Wide" );
	options.output_type = priml::LevelPixelType::Gray16;
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, nullptr, &error ), error );
	auto ladder = size_t{};
	const auto pixels = read_plane<uint16_t>( entry, "Wide", &ladder );
	require( ladder >= 3, std::format( "gray16 ladder has {} levels", ladder ) );
	for (auto y = 0; y < kSlideSize.h; y += 7)
		for (auto x = 0; x < kSlideSize.w; x += 5)
			require( pixels[(size_t)y * kSlideSize.w + x] == x * 90 + y, std::format( "gray16 pixel ({},{}) wrong", x, y ) );
}


// Two-frame chunks hold whole frames of level 1 only: level 1 is the workers'
// 2x2 mean, exactly; levels 2 and 3 are rendered from it afterwards.
void test_bridge_reduced_levels_mean ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "ramp.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    h, w = chunk.shape[:2]\n"
		"    y, x = np.mgrid[0:h, 0:w]\n"
		"    return ((ctx.x0 + x) * 10 + (ctx.y0 + y) * 7).astype(np.uint16)\n" );
	auto options = bridge_options( script, "Ramp" );
	options.output_type = priml::LevelPixelType::Gray16;
	options.chunk_tiles = 2;
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, nullptr, &error ), error );
	auto ladder = size_t{};
	read_plane<uint16_t>( entry, "Ramp", &ladder );
	require( ladder == 4, std::format( "ladder has {} levels, expected 4", ladder ) );
	auto halved = priml::Dim2D<int>{};
	const auto expected = mean_level( kSlideSize, [] ( int x, int y ) { return (uint32_t)( x * 10 + y * 7 ); }, &halved );
	auto size = priml::Dim2D<int>{};
	const auto level1 = read_level<uint16_t>( entry, "Ramp", 1, &size );
	require( size.w == halved.w && size.h == halved.h, "level 1 has the wrong size" );
	for (auto i = size_t{}; i < level1.size(); ++i)
		require( level1[i] == expected[i], std::format( "level 1 pixel {} is {}, expected {}", i, level1[i], expected[i] ) );
	auto quarter = priml::Dim2D<int>{};
	const auto expected2 = mean_level( halved, [&] ( int x, int y ) { return expected[(size_t)y * halved.w + x]; }, &quarter );
	const auto level2 = read_level<uint16_t>( entry, "Ramp", 2, &size );
	for (auto y = 4; y < quarter.h - 4; y += 9)
		for (auto x = 4; x < quarter.w - 4; x += 11) {
			const auto got = (int)level2[(size_t)y * quarter.w + x], want = (int)expected2[(size_t)y * quarter.w + x];
			require( std::abs( got - want ) <= 20, std::format( "level 2 pixel ({},{}) is {}, expected about {}", x, y, got, want ) );
		}
}


// A chunk skipped for roi is absent at the in-chunk levels too.
void test_bridge_reduced_levels_sparse ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "left.py",
		"import numpy as np\n"
		"def prepare(image, ctx):\n"
		"    roi = np.zeros(image.shape[:2], bool)\n"
		"    roi[:, : image.shape[1] // 4] = True\n"
		"    return {'roi': roi}, {}\n" );
	const auto script = write_script( directory, "flat.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    return np.full(chunk.shape[:2], 200, np.uint8)\n" );
	auto options = bridge_options( script, "Sparse" );
	options.prepare_script = prepare;
	options.chunk_tiles = 2;
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, nullptr, &error ), error );
	auto size = priml::Dim2D<int>{};
	const auto level1 = read_level<uint8_t>( entry, "Sparse", 1, &size );
	require( level1[(size_t)10 * size.w + 10] == 200, "a processed chunk is missing at level 1" );
	require( level1[(size_t)( size.h - 10 ) * size.w + size.w - 10] == 0, "a skipped chunk was written at level 1" );
}


// A stored raster non-zero where roi is zero is still complete: those chunks run
// as raster-only jobs, without Python.
void test_bridge_store_raster_skipped_chunk ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto prepare = write_script( directory, "band.py",
		"import numpy as np\n"
		"def prepare(image, ctx):\n"
		"    roi = np.zeros(image.shape[:2], bool)\n"
		"    roi[:, : image.shape[1] // 4] = True\n"
		"    band = np.zeros(image.shape[:2], bool)\n"
		"    band[-20:] = True\n"
		"    return {'roi': roi, 'band': band}, {'store': 'band'}\n" );
	const auto script = write_script( directory, "one.py",
		"import numpy as np\n"
		"def process(chunk, ctx):\n"
		"    return np.ones(chunk.shape[:2], np.uint8)\n" );
	auto options = bridge_options( script, "Band" );
	options.prepare_script = prepare;
	options.chunk_tiles = 2;
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, options, &stats, &error ), error );
	require( stats.chunks_raster_only > 0, "no raster-only job ran" );
	const auto band = read_plane<uint8_t>( entry, "Band.band" );
	require( band[(size_t)( kSlideSize.h - 5 ) * kSlideSize.w + kSlideSize.w - 5] == 255 &&
		band[(size_t)( kSlideSize.h - 5 ) * kSlideSize.w + 5] == 255 && band[5] == 0,
		"the stored raster is incomplete over skipped chunks" );
	auto size = priml::Dim2D<int>{};
	const auto level1 = read_level<uint8_t>( entry, "Band.band", 1, &size );
	require( level1[(size_t)( size.h - 3 ) * size.w + size.w - 3] == 255, "the stored raster's level 1 is incomplete" );
}


void test_bridge_remove_channel ( const fs::path &directory )
{
	const auto entry = make_slide( directory );
	const auto script = write_script( directory, "id.py", "def process(chunk, ctx):\n    return chunk[..., 0].copy()\n" );
	const auto before = snapshot( directory );
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	require( priml::process_dcm_chunks( entry, bridge_options( script, "Gone" ), &stats, &error ), error );
	auto removed = std::vector<fs::path>{};
	require( priml::remove_dcm_channel( entry, "Gone", &removed, &error ), error );
	require( removed.size() == stats.published.size(), "not every instance of the channel was removed" );
	require( snapshot( directory ) == before, "removal did not restore the slide folder" );
}

#endif // USE_DCMTK

} // namespace


int main ( int argc, char **argv )
{
	if (argc < 2) {
		std::cerr << "usage: pyra_bridge_test OUTPUT_DIR\n";
		return 2;
	}
	const auto run = fs::path( argv[1] ) / std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
	auto passed = true;
	const auto test = [&] ( const char *name, const auto &function ) {
		const auto started = std::chrono::steady_clock::now();
		try {
			function( run / name );
			std::cout << name << ": PASS";
		} catch (const std::exception &exception) {
			passed = false;
			std::cout << name << ": FAIL: " << exception.what();
		}
		std::cout << " (" << std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - started ).count() << " us)" << std::endl;
	};
	auto skipped = false;
#if USE_DCMTK
	test( "test_dcm_encoded_level_round_trip", test_dcm_encoded_level_round_trip );
	test( "test_dcm_remove_channel_guarded", test_dcm_remove_channel_guarded );
#else
	std::cout << "DICOM tests: SKIP (built without PYR_WITH_DCMTK)" << std::endl;
	skipped = true;
#endif
	if (!python_with_numpy()) {
		std::cout << "worker and bridge tests: SKIP (no Python with numpy; set PYRA_PYTHON)" << std::endl;
		return passed ? kSkipped : 1;
	}
	test( "test_bridge_worker_round_trip", test_bridge_worker_round_trip );
	test( "test_bridge_worker_reports_exception", test_bridge_worker_reports_exception );
	test( "test_bridge_worker_import_error", test_bridge_worker_import_error );
#if !USE_DCMTK
	std::cout << "bridge tests: SKIP (built without PYR_WITH_DCMTK)" << std::endl;
	return passed ? ( skipped ? kSkipped : 0 ) : 1;
#else
	test( "test_bridge_identity_gray8", test_bridge_identity_gray8 );
	test( "test_bridge_halo_placement", test_bridge_halo_placement );
	test( "test_bridge_prepare_params_reach_workers", test_bridge_prepare_params_reach_workers );
	test( "test_bridge_roi_skips_chunks", test_bridge_roi_skips_chunks );
	test( "test_bridge_mask_window_nearest", test_bridge_mask_window_nearest );
	test( "test_bridge_store_raster_level0", test_bridge_store_raster_level0 );
	test( "test_bridge_param_collision_refused", test_bridge_param_collision_refused );
	test( "test_bridge_failure_leaves_slide", test_bridge_failure_leaves_slide );
	test( "test_bridge_existing_channel_refused", test_bridge_existing_channel_refused );
	test( "test_bridge_stdout_print", test_bridge_stdout_print );
	test( "test_bridge_gray16_ladder", test_bridge_gray16_ladder );
	test( "test_bridge_reduced_levels_mean", test_bridge_reduced_levels_mean );
	test( "test_bridge_reduced_levels_sparse", test_bridge_reduced_levels_sparse );
	test( "test_bridge_store_raster_skipped_chunk", test_bridge_store_raster_skipped_chunk );
	test( "test_bridge_remove_channel", test_bridge_remove_channel );
	return passed ? ( skipped ? kSkipped : 0 ) : 1;
#endif
}
