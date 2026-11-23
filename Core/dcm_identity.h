// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Who the DICOM writer says it is.
//
// The writer stamps its own coded concepts with a private coding scheme, a
// presentation ICC profile with a product name and a four-character signature,
// and its pyramids are later recognised as its own by Manufacturer and
// Manufacturer Model Name. A host that publishes DICOM under its own name sets
// that identity once at start-up, before any writer or reader runs, or states
// it at build time in pyr_host_config.h (see build_config.h); the defaults name
// PyrA. The reader accepts the RANGE_MAX concept only under the
// current scheme, so a host must keep the scheme its existing files carry.

#include "build_config.h"

#include <string>

// Defaults of DcmWriterIdentity; a host states its own in pyr_host_config.h.
#ifndef PYR_DCM_LOCAL_SCHEME
#define PYR_DCM_LOCAL_SCHEME "99PYRA"
#endif
#ifndef PYR_DCM_MANUFACTURER
#define PYR_DCM_MANUFACTURER "PyrA"
#endif
#ifndef PYR_DCM_MODEL
#define PYR_DCM_MODEL "PyrA DICOM writer"
#endif
#ifndef PYR_DCM_PRODUCT
#define PYR_DCM_PRODUCT "PyrA"
#endif
#ifndef PYR_DCM_ICC_SIGNATURE
#define PYR_DCM_ICC_SIGNATURE "PYRA"
#endif

namespace priml {

struct DcmWriterIdentity {
// Private coding scheme designator of the writer's own codes ("99" + name).
	std::string local_scheme = PYR_DCM_LOCAL_SCHEME;
// The producer whose pyramids dcm_pyramid_entry recognises.
	std::string manufacturer = PYR_DCM_MANUFACTURER;
	std::string model = PYR_DCM_MODEL;
// Product named in the presentation ICC profile's description and notice.
	std::string product = PYR_DCM_PRODUCT;
// ICC header signature (CMM type, device manufacturer, profile creator); the
// first four characters are used, padded with spaces.
	std::string icc_signature = PYR_DCM_ICC_SIGNATURE;
};

void set_dcm_writer_identity ( DcmWriterIdentity identity );
[[nodiscard]] const DcmWriterIdentity &dcm_writer_identity ();

} // namespace priml
