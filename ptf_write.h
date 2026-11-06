#pragma once

// Tiled PTF writer.
//
// Produces the format pyr already reads: a multi-IFD tiled TIFF whose tile
// payloads are whatever encoding this build can produce, identified on the way
// back in by their magic bytes rather than by the TIFF compression tag. Plane
// identity - which channel, which focal plane, what the samples mean - travels in
// the ImageDescription block defined in ptf_tags.h, shared verbatim with the
// loader.
//
// What this file knows: rasters, tiles, IFDs. What it does not know, and must not
// learn: slides, zones, masks, IPX, or where the pixels came from. The IPX
// conversion that motivated it lives in priml, above this library.
//
// Two properties the design turns on:
//
// * Tiles are PULLED, one at a time, through a callback. A whole-slide base level
//   of a sparse field-set is tens of gigabytes dense and cannot be materialised;
//   the caller renders each tile on demand and the writer never holds more than
//   one.
// * A tile the callback declines is not written at all, leaving its offset and
//   byte count at zero. pyr already treats a zero-byte tile as a legitimately
//   empty region, so a mostly-empty mosaic costs only its own tile table.

#include "img_codec.h"
#include "ptf_tags.h"
#include "pwhl.h"

#include <functional>
#include <memory>
#include <string>

namespace priml {

// Tile edge, in pixels. TIFF requires a multiple of 16; 512 keeps the tile table
// small on a whole-slide mosaic while staying a sensible decode unit.
inline constexpr int kPtfDefaultTileEdge = 512;


struct PtfLevelSpec {
//
// Channel, mode, pixel type, mpp and max-pixel-value. The writer emits this
// verbatim into the IFD; it does not second-guess any of it.
	PtfPlaneTags tags;
	Dim2D<int> size{ 0, 0 };
	Dim2D<int> tile{ kPtfDefaultTileEdge, kPtfDefaultTileEdge };
//
// How to encode the tiles. The writer names a codec, never a codec library,
// matching the way loaders hand encoded bytes to the decoder dispatcher.
	ImgCodec codec = ImgCodec::Jxl;
	TileEncodeOptions encode;
};


// Called once per tile, in row-major order, with the tile's rectangle clipped to
// the level extent. Fill `out` with that rectangle's pixels and return true;
// return false to leave the tile out of the file entirely.
//
// `out` arrives zeroed, and a fetch may fill only part of it - on a sparse mosaic
// most edge tiles are partly covered - so whatever it does not write stays
// background.
template <typename pixeltype>
using PtfTileFetch =
	std::function<bool( const Rtlbr<int> &tile_rect, Pwhl<pixeltype> *out )>;


// Counters a caller reports rather than guesses at. Storage claims about this
// format have to come from here, not from arithmetic on the source files.
struct PtfWriteStats {
	int levels_written = 0;
	int aux_written = 0;
	long long tiles_written = 0;
	long long tiles_empty = 0;
	long long encoded_bytes = 0;
};


class PtfWriter {
public:
	PtfWriter ();
	~PtfWriter ();

	PtfWriter ( const PtfWriter & ) = delete;
	PtfWriter &operator= ( const PtfWriter & ) = delete;

//
// Creates the file, truncating any existing one. Always BigTIFF: a whole-slide
// multi-channel pyramid passes 4 GB without anything unusual happening, and
// libtiff reads both forms transparently, so there is no reason to make the
// caller choose and no reason to discover the limit at level nine of a
// forty-minute conversion.
	bool open ( const std::string &filename, std::string *error = nullptr );

	template <typename pixeltype>
	bool add_level ( const PtfLevelSpec &spec,
		const PtfTileFetch<pixeltype> &fetch, std::string *error = nullptr );

//
// One associated image - "frost", "overview", "macro" - as a stripped IFD. These
// are not levels of any ladder and carry role=aux, so the loader keeps them out
// of the plane index.
	bool add_aux ( const std::string &name, const Pwhl<Rgb> &image,
		std::string *error = nullptr );

//
// Flushes and closes. Call it explicitly and check the result: a TIFF's directory
// is written at close, so a failure here means the file is unusable even though
// every add_level succeeded. The destructor closes too, but cannot report.
	bool close ( std::string *error = nullptr );

	[[nodiscard]] const PtfWriteStats &stats () const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	bool begin_level ( const PtfLevelSpec &spec, std::string *error );
	bool write_raw_tile ( int tile_index, const void *bytes, size_t len,
		std::string *error );
	bool end_directory ( std::string *error );
};

} // namespace priml
