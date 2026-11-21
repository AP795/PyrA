// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace priml {

struct DcmWriteStats;

std::filesystem::path dcm_pyramid_aux_directory ( const std::filesystem::path &entry );

// Colocated objects plus the named companion directory (or its parent entry).
// Consumers still select objects by DICOM identity, never by filenames alone.
std::vector<std::filesystem::path> dcm_series_paths ( const std::filesystem::path &opened );

// Move verified, caller-owned staged objects into their final layout. Companions
// publish first, the visible entry last. Existing files are never replaced;
// failure/cancellation removes only this invocation's published files.
bool publish_dcm_pyramid ( const std::filesystem::path &entry, DcmWriteStats *stats,
	std::string *error, const std::function<bool ()> &cancelled = {} );

// Canonical entry for a pyramid member written under the current writer identity
// (dcm_identity.h). Returns empty for other
// DICOM layouts. Directory spelling locates the candidate; DICOM identities and
// producer metadata establish membership.
std::filesystem::path dcm_pyramid_entry ( const std::filesystem::path &member );

}
