// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "build_config.h"

#if USE_DCMTK
#include "pyr.h"

#include <dcmtk/dcmdata/dctk.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace priml {

struct DcmPixelLocation {
	uint64_t offset = 0;
	uint32_t length = 0;
};

struct DcmSceneFrame {
	DcmPixelLocation pixels;
	Dim2D<int> size{ 0, 0 };
	LevelPixelType pixel_type = LevelPixelType::Gray8;
	double mpp = 0.0;
	Pxy<double> origin_um{ 0.0, 0.0 };
};

// A whole scene in one encapsulated frame. Keeps offsets only, never payloads.
bool index_dcm_scene ( const std::string &path, DcmSceneFrame *frame,
	std::string *error = nullptr );

// Parses the dataset but stops before reading native or encapsulated pixels.
bool read_dcm_header ( const std::string &path, std::unique_ptr<DcmFileFormat> &file,
	uint64_t *pixel_offset = nullptr, uint32_t *pixel_length = nullptr,
	std::string *error = nullptr );

// One fragment per frame. All offsets are checked against the original file.
bool index_dcm_frames ( const std::string &path, DcmDataset &dataset,
	uint64_t pixel_offset, uint32_t pixel_length, uint32_t expected,
	std::vector<DcmPixelLocation> *frames, std::string *error );

} // namespace priml
#endif
