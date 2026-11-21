// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace priml {

// Explicit presentation assumption, not a measured scanner calibration.
// The profile maps native/effective_max through assumed sRGB to D50 PCS.
bool make_dcm_rgb_profile ( int container_max, int effective_max,
	std::vector<uint8_t> *out, std::string *error );

} // namespace priml
