// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Python processing of a whole DICOM slide into a new channel.
//
// 1. Prepare (optional): one Python process receives the whole source plane at a
//    coarse resolution and returns named rasters (masks) and name=value
//    parameters.
// 2. Chunks: the level-0 plane is cut into chunks of whole source frames. Worker
//    threads, each driving one Python process, send a chunk plus halo and the
//    mask windows, take back the processed core, cut it into frames and encode
//    them losslessly. The main thread spools the frames as they arrive.
//    Each worker also halves its block into the reduced levels the chunk holds
//    whole frames of, and cuts and encodes the stored rasters' windows the same
//    way.
// 3. The spooled frames become the new instances, plane by plane and level by
//    level; the levels coarser than a chunk are rendered from the coarsest
//    spooled one. All of it is published into the slide's series beside the
//    acquisition. Nothing existing is rewritten; a failure before publication
//    leaves the slide untouched.

#include "img_codec.h"
#include "pyr.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace priml {

// Longest edge of the prepare image when no prepare resolution is given, px.
inline constexpr int kPrepareMaxEdge = 4096;
// Target chunk edge, px. The default chunk is the power-of-two number of source
// frames nearest to it: a chunk of 2^K frames holds whole frames of K reduced
// levels, which its worker then builds and encodes itself.
inline constexpr int kChunkTargetEdge = 2048;
// A prepare raster of this name gates the chunks: one it leaves all zero is
// skipped, and its frames stay absent.
inline constexpr std::string_view kBridgeRoiRaster = "roi";
// A prepare parameter of this name lists the rasters kept as channels
// "<channel>.<raster>", comma separated. The others are discarded after the run.
inline constexpr std::string_view kBridgeStoreParam = "store";

#if USE_OPENJPH
inline constexpr ImgCodec kBridgeDefaultCodec = ImgCodec::Htj2k;
#else
inline constexpr ImgCodec kBridgeDefaultCodec = ImgCodec::Jp2k;
#endif


struct ChunkProcessOptions {
	std::filesystem::path process_script;
	std::filesystem::path prepare_script;		// optional
	std::filesystem::path python;				// empty: default_bridge_python()
	std::filesystem::path worker_script;		// pyra_worker.py
	PlaneSel source;
	std::string channel;						// new plane name; must not exist yet
	LevelPixelType output_type = LevelPixelType::Gray8;	// Gray8 or Gray16
	double prepare_mpp = 0.0;					// 0: from kPrepareMaxEdge
	int chunk_tiles = 0;						// 0: power of two nearest kChunkTargetEdge
	int halo = 0;
	int workers = 0;							// 0: half the hardware threads
	ImgCodec codec = kBridgeDefaultCodec;		// Jp2k or Htj2k, always lossless
	std::string uid_root;						// empty: DCMTK's instance root
	std::vector<std::pair<std::string, std::string>> params;
};


struct ChunkProcessStats {
	int workers = 0;
	long long chunks_total = 0;
	long long chunks_processed = 0;
	long long chunks_skipped = 0;				// no Python call
	long long chunks_raster_only = 0;			// of those, still encoded for a stored raster
	long long frames_written = 0;
	long long encoded_bytes = 0;
	std::vector<std::string> channels;			// planes added
	std::vector<std::filesystem::path> published;
// Wall time of each phase, and summed per-worker time of the chunk steps.
	double prepare_s = 0.0;
	double chunks_s = 0.0;
	double read_s = 0.0;
	double python_s = 0.0;
	double transfer_s = 0.0;
	double encode_s = 0.0;						// result frames, every in-chunk level
	double rasters_s = 0.0;						// stored-raster frames, every in-chunk level
	double assemble_s = 0.0;
	double reduce_s = 0.0;
	double publish_s = 0.0;
	double total_s = 0.0;
};

using ChunkProcessProgress = std::function<void ( std::string_view phase, size_t done, size_t total )>;

bool process_dcm_chunks ( const std::filesystem::path &slide, const ChunkProcessOptions &options,
	ChunkProcessStats *stats, std::string *error, const ChunkProcessProgress &progress = {} );

// $PYRA_PYTHON, else "python" on Windows and "python3" elsewhere.
std::filesystem::path default_bridge_python ();

// The run's report as pyra_process prints it: what was added, then a table of
// wall time per phase and one of the work summed over the Python workers.
std::string format_chunk_report ( const ChunkProcessStats &stats );
//
// The same numbers as one JSON object, for a program that tabulates them itself
// (pyra_process --report-json; the web viewer's Analyse).
std::string format_chunk_report_json ( const ChunkProcessStats &stats );

} // namespace priml
