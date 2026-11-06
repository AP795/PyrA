#pragma once

// Decoding of the non-pyramidal IFDs a WSI carries alongside its tile pyramid:
// the slide label (frost), the macro/overview photograph, and thumbnails.
//
// Every vendor stores these differently — Hamamatsu writes single-strip JPEG,
// Aperio writes multi-strip LZW — so the one entry point below covers all of
// them and the loaders only have to decide which IFD is which.

#include "pyr.h"
#include "pwhl.h"
#include "tiff_inc.h"

namespace priml {

// Shared row-major TIFF tile geometry and byte ranges. The caller owns
// format-specific headers and decides whether an entirely empty level is valid.
bool BuildTiffTileTable ( TIFF *tif, Level &level,
	const Dim2D<uint32_t> &dimensions, const Dim2D<uint32_t> &tile_dimensions,
	size_t &max_tile_bytes );


// X/Y microns-per-pixel from the CURRENT directory's TIFF resolution tags.
// A zero component means the corresponding tag or physical unit was absent.
PixelMetric ReadTiffPixelMetric ( TIFF *tif );

// Decode the TIFF's CURRENT directory into an owning Pwhl. Handles 8-bit
// grayscale strips scanline-wise, single-strip JPEG through libjpeg, and
// everything else through TIFFReadRGBAImage. The current directory is left
// unchanged, so this is safe to call from inside an IFD walk.
// Returns false when the IFD cannot be decoded; `out` is then untouched.
template <typename pixeltype>
bool LoadStandardIFD ( TIFF *tif, Pwhl<pixeltype> &out );


// Record the baseline TIFF tags of the CURRENT directory as `tiff.*` properties.
//
// The raw tag beside the normalised reading: openslide.mpp-x says what we
// concluded, tiff.XResolution says what the file actually held. Call it on the
// base IFD once the pyramid is otherwise built.
void ReadTiffProperties ( TIFF *tif, Ppyr &pyramid );


// Capture the embedded ICC profile of the CURRENT directory, if any.
//
// Recorded, never applied. Colour-managing the pixels would move every
// OD-derived measure in every script at once; what this buys is the ability to
// say afterwards whether a run was colour-managed at all.
void ReadTiffIccProfile ( TIFF *tif, Ppyr &pyramid );

} // namespace priml
