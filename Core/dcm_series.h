// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "dcm_write.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace priml {

//
// Leading words of the PROCESSING acquisition-context text on every instance a
// derived channel adds to an existing slide. remove_dcm_channel deletes only
// instances carrying it: the writer identity and a PROCESSING item alone do not
// tell a derived channel from an acquired one, since a converter records its own
// processing (registration, flat field) on acquired channels too.
inline constexpr std::string_view kDcmDerivedChannelMarker = "pyra-derived-channel";


// One object of a slide's series, read up to its functional groups.
struct DcmSeriesInstance {
	std::filesystem::path path;
	std::string role;								// Image Type value 3: VOLUME, LABEL, ...
	int samples = 0;								// Samples per Pixel: 1 gray, 3 colour
	std::string manufacturer;
	std::string model;
	std::vector<std::string> optical_ids;
	std::vector<std::string> optical_descriptions;
	std::string channel_name;						// CHANNEL_NAME context, if any
	std::string processing;							// PROCESSING context, if any
//
// The plane name the loader gives this instance: empty for a single colour
// optical path; else CHANNEL_NAME for a single gray one; else the first optical
// path's description, else its identifier.
	[[nodiscard]] std::string plane_name () const;
	[[nodiscard]] bool derived_channel () const;
};

// Every object sharing the entry's Series Instance UID, in dcm_series_paths order.
bool dcm_series_instances ( const std::filesystem::path &entry,
	std::vector<DcmSeriesInstance> *instances, std::string *error );

// What a new instance must repeat to join the entry's slide: study, series,
// pyramid and frame-of-reference UIDs, patient, container, specimen, acquisition
// time, focus, imaged depth, illumination, and the total pixel matrix origin.
// Missing optional values get the writer's documented defaults.
bool dcm_slide_acquisition ( const std::filesystem::path &entry, DcmAcquisition *meta,
	Pxy<double> *origin_um, std::string *error );

// Move staged instances that join an existing slide into it: into the entry's
// companion directory when it has one, else beside the entry. Existing files are
// never replaced; a failure removes only this call's published files.
bool publish_dcm_companions ( const std::filesystem::path &entry, DcmWriteStats *stats,
	std::string *error );

// Delete the instances of one derived channel (see kDcmDerivedChannelMarker).
// Refuses when any instance of that plane name is not a derived channel of this
// writer, or is the entry itself.
bool remove_dcm_channel ( const std::filesystem::path &entry, const std::string &channel,
	std::vector<std::filesystem::path> *removed, std::string *error );

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
