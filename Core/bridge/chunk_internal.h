// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Shared between the two halves of process_dcm_chunks: chunk_process.cpp (the
// source, the Python passes, the worker threads) and chunk_assemble.cpp (the
// DICOM instances built from their output).

#include "chunk_process.h"
#include "dcm_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace priml::bridge_detail {

// The level-0 plane being processed. Pixel coordinates are 0-based on its grid.
struct SourcePlane {
	PlaneSel plane;
	LevelPixelType type = LevelPixelType::Rgb8;
	float mpp = 0.0f;
	Dim2D<int> size{ 0, 0 };
	Dim2D<int> tile{ 0, 0 };
	Pxy<float> metric_origin{ 0.0f, 0.0f };
	int across = 0;
	int down = 0;
	std::vector<uint8_t> occupied;			// per source frame, row-major

	[[nodiscard]] bool frame_present ( int tx, int ty ) const {
		return occupied[(size_t)ty * across + tx] != 0;
	}
	[[nodiscard]] Rtlbr<float> metric ( const Rtlbr<int> &rect ) const {
		return { { metric_origin.x + rect.tl.x * mpp, metric_origin.y + rect.tl.y * mpp },
			{ metric_origin.x + rect.br.x * mpp, metric_origin.y + rect.br.y * mpp } };
	}
};


// A prepare raster, at the prepare resolution, sampled nearest-neighbour onto
// level-0 pixels: level-0 x falls in coarse column floor((x + 0.5) * scale_x).
struct Raster {
	std::string name;
	std::vector<uint8_t> pixels;
};

struct Prepared {
	Dim2D<int> size{ 0, 0 };
	double scale_x = 0.0;
	double scale_y = 0.0;
	double mpp = 0.0;
	std::vector<Raster> rasters;
	std::vector<std::string> stored;			// raster names kept as channels
	std::string script;
	std::string sha256;

	[[nodiscard]] int column ( int x ) const {
		return std::min( size.w - 1, std::max( 0, (int)std::floor( ( x + 0.5 ) * scale_x ) ) );
	}
	[[nodiscard]] int row ( int y ) const {
		return std::min( size.h - 1, std::max( 0, (int)std::floor( ( y + 0.5 ) * scale_y ) ) );
	}
	[[nodiscard]] const Raster *find ( std::string_view name ) const {
		for (const auto &raster : rasters)
			if (raster.name == name)
				return &raster;
		return nullptr;
	}
//
// True when the raster samples any non-zero pixel over the level-0 rectangle.
	[[nodiscard]] bool any ( const Raster &raster, const Rtlbr<int> &rect ) const;
//
// The raster sampled onto a level-0 rectangle, row-major.
	[[nodiscard]] std::vector<uint8_t> window ( const Raster &raster, const Rtlbr<int> &rect ) const;
};


// Where each encoded frame sits in the spool file.
struct SpoolEntry {
	uint64_t offset = 0;
	uint32_t length = 0;
};

// Spooled frames, keyed by plane (0 the processed channel, 1.. the stored rasters
// in Prepared::stored order), level and row-major frame index within the level.
using SpoolIndex = std::unordered_map<int64_t, SpoolEntry>;

inline constexpr int kSpoolFrameBits = 40;
inline constexpr int kSpoolLevelBits = 8;

inline int64_t spool_key ( int plane, int level, int64_t frame )
{
	return ( (int64_t)plane << ( kSpoolFrameBits + kSpoolLevelBits ) ) |
		( (int64_t)level << kSpoolFrameBits ) | frame;
}


// What the assembly half needs to write and publish the new instances.
struct AssemblyInput {
	std::filesystem::path slide;
	std::filesystem::path staging;
	std::filesystem::path spool;
	const ChunkProcessOptions *options = nullptr;
	const SourcePlane *source = nullptr;
	const Prepared *prepared = nullptr;
	const SpoolIndex *index = nullptr;
	std::string process_script;
	std::string process_sha256;
	std::vector<std::pair<std::string, std::string>> params;	// as the chunk workers saw them
	Dim2D<int> chunk{ 0, 0 };									// core edge, px
	int chunk_levels = 0;										// reduced levels the workers spooled
	uint32_t result_max = 0;									// largest sample the script returned
	std::vector<std::string> optical_ids_in_use;				// the slide's own
};

// Writes every plane (the processed channel, then each stored raster) as new
// instances in input.staging: levels 0..chunk_levels straight from the spool,
// the coarser ones rendered from the coarsest spooled level. Then publishes them
// into the slide. Fills stats->channels, published, frames_written,
// encoded_bytes and the reduce/publish timings.
bool assemble_dcm_channels ( const AssemblyInput &input, ChunkProcessStats *stats, std::string *error );

} // namespace priml::bridge_detail
