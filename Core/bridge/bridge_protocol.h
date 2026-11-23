// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Wire format between the bridge and its Python workers (python/pyra_worker.py).
//
// Every message is one fixed 64-byte little-endian header, then `text_bytes` of
// UTF-8 "key=value" lines, then `payload_bytes` of samples. The Python side
// spells the same header as struct "<4sHHIiiiiBB2xiiiiIId"; change the two
// together and bump kBridgeProtocolVersion.

#include <bit>
#include <cstdint>

namespace priml {

inline constexpr char kBridgeMagic[4] = { 'P', 'Y', 'R', 'A' };
inline constexpr uint16_t kBridgeProtocolVersion = 1;

enum class BridgeKind : uint16_t {
	Hello = 1,		// worker -> host: script imported; versions and script hash
	Setup = 2,		// host -> chunk worker: parameters, output sample type
	Ready = 3,		// worker -> host: setup() done
	Prepare = 4,	// host -> prepare worker: the coarse whole-slide image
	Rasters = 5,	// prepare worker -> host: rasters and parameters
	Chunk = 6,		// host -> chunk worker: samples plus halo, mask windows
	Result = 7,		// chunk worker -> host: the chunk core, halo cropped
	Shutdown = 8,	// host -> worker: exit
	Error = 9,		// worker -> host: message and traceback
};

enum class BridgeSample : uint8_t { None = 0, U8 = 1, U16 = 2 };


struct BridgeHeader {
	char magic[4];
	uint16_t version;
	uint16_t kind;
	uint32_t id;					// chunk id
	int32_t x0;						// level-0 pixel of sample [0, 0], halo included
	int32_t y0;
	int32_t width;					// samples per row of the payload image
	int32_t height;
	uint8_t sample;					// BridgeSample
	uint8_t channels;				// 1 or 3
	uint8_t reserved[2];
	int32_t halo_left;
	int32_t halo_top;
	int32_t halo_right;
	int32_t halo_bottom;
	uint32_t text_bytes;
	uint32_t payload_bytes;
	double value;					// requests: microns per pixel; Result: Python seconds
};

static_assert( sizeof( BridgeHeader ) == 64, "bridge header must stay 64 bytes" );
static_assert( std::endian::native == std::endian::little, "the bridge wire format is little-endian" );

} // namespace priml
