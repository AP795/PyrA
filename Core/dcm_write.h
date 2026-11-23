// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "dcm_identity.h"
#include "img_codec.h"
#include "pyr.h"

#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace priml {

inline constexpr std::string_view kDcmRangeCode = "RANGE_MAX";
inline constexpr int kDcmFrameMaxEdge = std::numeric_limits<uint16_t>::max();
//
// Pixel Spacing written on an associated image whose spacing nobody measured. The
// attribute is mandatory on every Whole Slide Microscopy instance; the instance then
// says, in its acquisition context, that the value is not a measurement.
inline constexpr double kDcmUnmeasuredPixelSpacingUm = 1.0;


// Image pixels made by combining several focal planes (focus stacking): Extended
// Depth of Field YES, with the plane count and their spacing, which the standard
// requires with it.
struct DcmExtendedDepth {
	int focal_planes = 0;
	double spacing_um = 0.0;
};


// What an instance holds besides a pyramid level: the slide's label image, or an
// overview of the whole glass (which, by the standard's definition, includes at
// least part of the label - a view of the tissue area alone is not one).
enum class DcmAssociatedRole { None, Label, Overview };


struct DcmAcquisition {
	std::string uid_root;
	std::string name;
	std::string acquisition_datetime;
	std::string focus_method;
	double imaged_depth_um = 0.0;
	bool fluorescence = false;
	std::string illumination_color;
	std::string patient_id;
	std::string patient_name;
	std::string specimen_id;
	std::string specimen_uid;
	std::string container_identifier;
	std::string series_description = "IPX converted pyramid";
	std::string derivation_description = "IPX field mosaic conversion";
	std::string acquisition_context = "IPX acquisition and processing context";
	std::string manufacturer;
	std::string model;
	std::string serial;
	std::string software;
	std::string study_uid;
	std::string series_uid;
	std::string pyramid_uid;
	std::string frame_of_reference_uid;
	std::optional<DcmExtendedDepth> extended_depth;
};

struct DcmLevelSpec {
	std::string channel;
//
// Optical Path Identifier (SH, at most 16 characters). Empty: the writer numbers
// its own ("OP001", ...). A channel joining an existing series names one of its
// own, since the series' acquired paths already use the writer's numbering.
	std::string optical_path_id;
	std::string mode;
	std::string processing;
	std::optional<bool> fluorescence;
	int level = 0;
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
	int max_pixel_value = 0;
	double mpp = 0.0;
	Pxy<double> origin_um{ 0.0, 0.0 };
	Dim2D<int> size{ 0, 0 };
	Dim2D<int> tile{ 512, 512 };
	ImgCodec codec = ImgCodec::Htj2k;
	TileEncodeOptions encode;
// Empty means dense coverage. Rectangles are half-open in this level's pixels.
	std::vector<Rtlbr<int>> coverage;
	bool sparse = false;
	bool thumbnail = false;
//
// Set by add_associated; a pyramid level leaves both alone.
	DcmAssociatedRole associated = DcmAssociatedRole::None;
	std::string barcode;
	bool spacing_measured = true;
// Explicit focal planes in the same instance, starting at z = 0 µm.
	int focal_planes = 1;
	double focal_step_um = 0.0;
};


struct DcmAssociatedImage {
	DcmAssociatedRole role = DcmAssociatedRole::Label;
//
// Microns per pixel; 0 when not known (kDcmUnmeasuredPixelSpacingUm is written
// and the instance says so).
	double mpp = 0.0;
//
// Label only: the decoded barcode, if any, into the Slide Label module.
	std::string barcode;
};

enum class DcmFetchResult { Pixels, Absent, Error };

template <typename T>
using DcmTileFetch = std::function<DcmFetchResult (
	const Rtlbr<int> &, Pwhl<T> *, std::string * )>;
template <typename T>
using DcmFocalTileFetch = std::function<DcmFetchResult (
	int z, const Rtlbr<int> &, Pwhl<T> *, std::string * )>;
//
// Already encoded frames: a full spec.tile-sized codestream of spec.codec for
// every rectangle in coverage, edge frames padded as add_level pads them.
using DcmEncodedFetch = std::function<DcmFetchResult (
	const Rtlbr<int> &, std::vector<uint8_t> *, std::string * )>;

struct DcmWriteStats {
	int levels_written = 0;
	long long tiles_written = 0;
	long long tiles_empty = 0;
	long long encoded_bytes = 0;
	std::filesystem::path representative;
	std::vector<std::filesystem::path> instances;
};

// Writes complete image instances into a caller-owned staging directory.
// No publication, source archiving, database work, or dense mosaic allocation.
class DcmWriter {
public:
	DcmWriter ();
	~DcmWriter ();
	DcmWriter ( const DcmWriter & ) = delete;
	DcmWriter &operator= ( const DcmWriter & ) = delete;
	bool open ( const std::filesystem::path &directory, const DcmAcquisition &metadata,
		std::string *error );
	template <typename T>
	bool add_level ( const DcmLevelSpec &spec, const DcmTileFetch<T> &fetch,
		std::string *error );
	bool add_focal_level ( const DcmLevelSpec &spec, const DcmFocalTileFetch<Rgb> &fetch,
		std::string *error, const std::function<bool ()> &cancelled );
//
// The same instance from frames encoded elsewhere. Each frame is checked against
// the spec (codec, full frame size, samples, depth) and written as it is, never
// decoded or re-encoded. The fetch is called in frame order, so the caller can
// assemble frames that were produced in any order.
// Supports JPEG 2000 and HTJ2K for a single focal plane.
	bool add_encoded_level ( const DcmLevelSpec &spec, const DcmEncodedFetch &fetch,
		std::string *error );
//
// One single-frame label or overview instance of the same series, losslessly
// coded. After at least one level: the first instance is the series' entry.
	bool add_associated ( const DcmAssociatedImage &spec, const Pwhl<Rgb> &image,
		std::string *error );
	bool close ( std::string *error );
	const DcmWriteStats &stats () const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	template <typename T>
	bool write_pixel_level ( const DcmLevelSpec &spec, const DcmFocalTileFetch<T> &fetch,
		std::string *error, const std::function<bool ()> &cancelled = {} );
	using FrameSource = std::function<bool ( int, const Rtlbr<int> &, std::vector<uint8_t> *,
		std::string * )>;
	bool write_level ( const DcmLevelSpec &spec, const FrameSource &frames,
		std::string *error, const std::function<bool ()> &cancelled = {} );
};

// Uses the caller's UID root; the generic format layer owns no application root.
std::string make_dcm_uid ( std::string_view root );

// DCMTK's instance UID root, for a tool that has no root of its own. Empty
// without DICOM support.
std::string default_dcm_uid_root ();

} // namespace priml
