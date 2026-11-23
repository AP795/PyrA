// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "ptf_write.h"
#include "wsi_session.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace priml {

struct PtfSubsetOptions {
	ImgCodec reduced_codec = ImgCodec::Jpeg;
	TileEncodeOptions reduced_encode{ .quality = kImageDefaultLossyQuality };
};


struct PtfSubsetResult {
	std::filesystem::path path;
// Source coordinates, in microns. The output's top-left is its own origin.
	Rtlbr<double> source_rect{};
	PtfWriteStats stats;
};


using PtfSubsetProgress = std::function<void ( size_t completed, size_t total )>;

// Appends to the PTF at `path` the reduced levels of every plane in `base` (each
// that plane's level-0 spec: channel, z, pixel type, size, mpp), halving until a
// level fits one tile. Each reduction reopens the file and renders from the level
// before it, in bounded tiles, so a whole-slide base is never held in memory.
// Coarse levels carry no base-level evidence (registration, flat field, focus
// merge). Written tile counts are added to `stats` when given.
bool append_ptf_reduced_levels ( const std::filesystem::path &path,
	const std::vector<PtfLevelSpec> &base, const PtfSubsetOptions &options,
	PtfWriteStats *stats, std::string *error = nullptr,
	const PtfSubsetProgress &progress = {} );


// Full finest-level tiles intersecting the half-open metric rectangle, across
// acquired planes. Encoded samples are copied, never decoded/re-encoded for the
// base. Incompatible TIFF geometry/codecs fail explicitly. Reduced levels are
// generated from the exported base, in bounded tiles. Publishes beside the
// source as sub_<N>_<stem>.ptf without replacing any existing file.
bool save_ptf_subset ( WsiSession &source, const Rtlbr<double> &metric_rect,
	PtfSubsetResult *result, std::string *error = nullptr,
	const PtfSubsetOptions &options = {}, const PtfSubsetProgress &progress = {} );

} // namespace priml
