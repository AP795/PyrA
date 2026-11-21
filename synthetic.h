// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Synthetic z-stack slide generator.
//
// Writes a PTF of procedurally drawn bright-field IHC tissue - hematoxylin
// nuclei, DAB-positive nuclei, elongated stromal nuclei, lymphocytes, fibrous
// stroma and white background - as a stack of focal planes, each a full level
// ladder. It exists to give the viewers and their tests a realistic z-stack of
// any size without shipping one.
//
// Every pixel is a pure function of (world position in microns, level pixel
// size, focal plane, seed): nothing is held but the tile being rendered, so the
// image size is bounded by the format, not by memory, and every pyramid level is
// drawn at its own resolution instead of being reduced from the base.
//
// Defocus is modelled analytically, not by convolution: an object's edge is the
// Gaussian-blurred step 0.5*erfc(d / (sqrt(2)*sigma)), sigma growing with the
// distance between the plane and the object's own depth. The in-focus depth
// sits at focus_fraction of the stack; by default a four-plane stack is sharp at
// z1, slightly soft at z2 and blurred at z0 and z3.

#include "geom.h"
#include "img_codec.h"
#include "pwhl.h"

#include <cstdint>
#include <functional>
#include <string>

namespace priml {

inline constexpr int kSynthTileEdgeMin = 64;
inline constexpr int kSynthTileEdgeMax = 2048;
// TIFF tiles must be multiples of 16.
inline constexpr int kSynthTileEdgeStep = 16;
inline constexpr int kSynthImageEdgeMax = 1 << 20;
inline constexpr int kSynthZLevelsMax = 16;
inline constexpr double kSynthMppMin = 0.05;
inline constexpr double kSynthMppMax = 10.0;
// Distance between consecutive focal planes, microns.
inline constexpr double kSynthZStepDefaultUm = 1.0;
inline constexpr double kSynthZStepMinUm = 0.1;
inline constexpr double kSynthZStepMaxUm = 5.0;
// Where in the stack the tissue is in focus, as a fraction of its height.
inline constexpr double kSynthFocusFractionDefault = 5.0 / 12.0;
// Tumour nuclei: mean equivalent-circle diameter 2*sqrt(a*b), microns.
inline constexpr double kSynthNucleusDiameterDefaultUm = 8.0;
inline constexpr double kSynthNucleusDiameterMinUm = 3.0;
inline constexpr double kSynthNucleusDiameterMaxUm = 40.0;
// Tumour nuclei spacing: sqrt(area per nucleus) inside a tumour nest, microns.
inline constexpr double kSynthNucleusSpacingDefaultUm = 12.0;
inline constexpr double kSynthNucleusSpacingMinUm = 4.0;
inline constexpr double kSynthNucleusSpacingMaxUm = 200.0;
// Expected share of DAB-positive tumour nuclei.
inline constexpr double kSynthPositiveFractionDefault = 0.3;
// Expected share of the tissue area inside tumour nests.
inline constexpr double kSynthTumourFractionDefault = 0.4;
// Share of the stromal nuclei that are lymphocytes; the rest are fibroblasts.
inline constexpr double kSynthLymphocyteFractionDefault = 0.4;
inline constexpr uint64_t kSynthDefaultSeed = 0x5EED'CE11'2026ull;


struct SyntheticSlideSpec {
	Dim2D<int> size{ 4096, 4096 };
	int tile_edge = 256;
	int z_levels = 4;
	double mpp = 0.5;
	ImgCodec codec = ImgCodec::Jpeg;
// 1..100 lossy; kImageLosslessQuality for codecs that support lossless.
	int quality = kImageDefaultLossyQuality;
	uint64_t seed = kSynthDefaultSeed;
	double z_step_um = kSynthZStepDefaultUm;
// 0 = in focus at z 0, 1 = at the last plane.
	double focus_fraction = kSynthFocusFractionDefault;
	double nucleus_diameter_um = kSynthNucleusDiameterDefaultUm;
	double nucleus_spacing_um = kSynthNucleusSpacingDefaultUm;
// The three fractions are 0..1.
	double positive_fraction = kSynthPositiveFractionDefault;
	double tumour_fraction = kSynthTumourFractionDefault;
	double lymphocyte_fraction = kSynthLymphocyteFractionDefault;
};


// Nuclei whose centre lies in a base-level rectangle: the slide's ground truth.
struct SyntheticCensus {
	long long tumour = 0;
	long long tumour_positive = 0;
	long long fibroblast = 0;
	long long lymphocyte = 0;
// Mean equivalent-circle diameter of the tumour nuclei, 0 when there are none.
	double tumour_diameter_um = 0.0;
};


// Called after every written tile. Return false to cancel; the partial file is
// then removed and write_synthetic_slide() fails with "cancelled".
using SyntheticProgress = std::function<bool ( long long done, long long total )>;


// Checks every limit above plus codec availability and the codec's lossless
// support. The message names the offending field.
bool synthetic_spec_valid ( const SyntheticSlideSpec &spec, std::string *error );

// Number of levels in each plane's ladder: base, then halved until both edges
// fit one tile.
int synthetic_level_count ( const SyntheticSlideSpec &spec );

// Size of ladder level `level` (0 = base).
Dim2D<int> synthetic_level_size ( const SyntheticSlideSpec &spec, int level );

// Renders `rect` (level pixel coordinates) of ladder level `level` of focal plane
// `z` into `out`, which must be sized to the rectangle.
void render_synthetic_tile ( const SyntheticSlideSpec &spec, int level, int z,
	const Rtlbr<int> &rect, Pwhl<Rgb8> *out );

// Counts the nuclei centred in `rect` (base pixel coordinates, clipped to the
// slide). The spec must be valid.
SyntheticCensus synthetic_census ( const SyntheticSlideSpec &spec, const Rtlbr<int> &rect );

// Writes the whole slide to `filename_utf8`, replacing any existing file.
bool write_synthetic_slide ( const std::string &filename_utf8,
	const SyntheticSlideSpec &spec, const SyntheticProgress &progress,
	std::string *error );

} // namespace priml
