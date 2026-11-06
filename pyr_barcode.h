#pragma once

// Where a slide's barcode comes from - one statement of the rule.
//
// The library holds the state and owns the precedence, but does not link a
// barcode decoder: reading one out of label pixels needs a symbology library
// (ZXing), which an embedder may not want and a headless build does not need.
// So the pixel decode is injected. Metadata-sourced barcodes need no decoder at
// all and are filled in by the loaders.

#include "pyr.h"
#include "pyr_rt.h"

#include <string>

namespace priml {

// Decodes a barcode from a label or overview image; "" when none is found.
using BarcodeDecodeFn = std::string ( * ) ( const Pwhl<Rgb> & );

inline BarcodeDecodeFn g_barcode_decoder = nullptr;

inline void set_barcode_decoder ( BarcodeDecodeFn fn ) noexcept
{
	g_barcode_decoder = fn;
}


// Resolve the barcode of an already-loaded pyramid, decoding it from the label
// image on first call if the scanner recorded none.
//
// Precedence: metadata recorded at load time (DICOM ContainerIdentifier &c.,
// NDPI tag 65427) wins, because the scanner read the barcode with better
// evidence than we have; then the frost, the dedicated label image; then the
// whole overview/macro, since the barcode may sit anywhere on it - we feed the
// entire image rather than the square display crop.
//
// One-shot: a failed decode latches barcode_attempted and is never retried, so
// repaints are free. That matters because the decoder is configured to try every
// symbology at every rotation and inversion, and the slow case is precisely the
// slide with no barcode.
inline const std::string &resolve_pyramid_barcode ( Ppyr &pyr )
{
	if (pyr.barcode.empty() && !pyr.barcode_attempted) {
		pyr.barcode_attempted = true;
//
// label_image() already encodes the precedence - the dedicated label capture if
// the file has one, otherwise the whole-slide macro, which contains the label
// panel uncropped. This used to be spelled out here as an ad-hoc fallback.
		const auto &src = label_image( pyr );
		if (src.p) {
			if (g_barcode_decoder) {
				pyr.barcode = g_barcode_decoder( src );
			}
			else {
//
// Loud rather than silent: without an installed decoder every slide whose
// scanner recorded no barcode would simply come back blank, which looks exactly
// like a slide that has none. See install_pyr_host_services in priml.
				pyr::plog_err( "pyr barcode: no decoder installed - the label "
					"image of this slide will not be read\n" );
			}
		}
	}
	return pyr.barcode;
}

} // namespace priml
