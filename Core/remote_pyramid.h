// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// A pyramid split across two processes: a SERVER that holds the slide file and
// its loader, and a CLIENT that holds a "sister" Ppyr with the same geometry but
// no file. The server describes its levels and tiles once (the geometry blob) and
// then hands out each tile's stored bytes unchanged; the client decodes and
// renders them with the same codec and render code the server would use.
//
// Tiles reach the client after the render that needs them. RemoteLoader::LoadTile
// does no I/O: it records the tile as missing and reports it absent, so a render
// returns a partial frame plus the list of tiles to fetch. Once the caller has put
// their bytes, the next render of the same view is complete and bit-identical to
// the server's own render of it.
//
// Synthetic coarse levels (BuildCoarseLevels) exist only in the server's memory,
// as one raster per level. They are offered as a grid of kRemoteSyntheticTileEdge
// virtual tiles, each cut from that raster and encoded losslessly on request. The
// client decodes them as they arrive into rasters its level owns, as the server
// does, so they never compete with stored tiles for cache slots.

#include "wsi.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace priml {

inline constexpr uint32_t kRemoteGeometryVersion = 1;

// Edge of the virtual tiles a synthetic level is offered as.
inline constexpr int kRemoteSyntheticTileEdge = 512;


// The geometry blob of an open pyramid: levels, tile rectangles, pixel types,
// colour hints, metric frame and properties. Little-endian, versioned by
// kRemoteGeometryVersion.
bool serialize_remote_geometry ( const Ppyr &pyramid, std::vector<uint8_t> *out,
	std::string *error = nullptr );


// The stored bytes of one tile as the client expects them: the loader's
// self-contained assembled tile, or for a synthetic level the losslessly encoded
// virtual tile. An absent (sparse) tile yields no bytes and true.
bool read_remote_tile ( WsiLoader &loader, const Ppyr &pyramid, int level_no,
	int tile_no, std::vector<uint8_t> *bytes, std::string *error = nullptr );


struct RemoteTileId {
	int level_no = -1;
	int tile_no = -1;

	bool operator== ( const RemoteTileId & ) const = default;
	auto operator<=> ( const RemoteTileId & ) const = default;
};


// The client side: a loader whose tiles are put by the caller rather than read.
class RemoteLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "remote"; }
	void Close ( Ppyr &pyramid ) override;

// Records the tile as missing; never reads. A tile already put is Loaded and is
// not asked for.
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

// Builds the sister pyramid from a geometry blob. The caller sets the cache
// options on `pyramid` beforehand, as for OpenFile.
	bool OpenRemote ( const uint8_t *data, size_t size, Ppyr &pyramid,
		std::string *error = nullptr );

// Stores a tile's bytes in the compressed cache. Bytes larger than the level's
// slot are refused.
	bool PutTile ( Ppyr &pyramid, int level_no, int tile_no,
		const uint8_t *data, size_t size, std::string *error = nullptr );

// The tiles renders asked for since the last call and still without bytes, each
// once, in (level, tile) order; at most `limit`, the rest stay queued.
	std::vector<RemoteTileId> TakeMissing ( const Ppyr &pyramid, size_t limit );

protected:
	bool Open ( Ppyr & ) override { return false; }

private:
	bool PutSyntheticTile ( Ppyr &pyramid, int level_no, int tile_no,
		const uint8_t *data, size_t size, std::string *error );

	std::mutex missing_mtx_;
	std::vector<RemoteTileId> missing_;
};

} // namespace priml
