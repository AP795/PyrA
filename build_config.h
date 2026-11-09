#pragma once

#if defined(PYR_USE_GENERATED_BUILD_CONFIG)
#include "pyr_build_config_generated.h"
#endif

#ifndef USE_OPENSLIDE
#define USE_OPENSLIDE 0
#endif

// Central compile-time feature switches for the whole solution.
//
// Lives in pyr/ (the lowest layer) rather than wsia/ so the low-level libraries
// carry no UI-layer dependency. It is pulled in everywhere via pwhl.h; the few
// files that gate at the very top of the translation unit (before any other
// include, e.g. wsi_dcm.cpp) include it explicitly.
//
// Prefer editing the values here over scattering /D flags across .vcxproj files.

// USE_DCMTK: build the DICOM VL Whole Slide Microscopy loader (links DCMTK).
// Per-customer, uniform across wsia and scpa.
#ifndef USE_DCMTK
#define USE_DCMTK 1
#endif

// USE_OPENJPEG: build the JPEG 2000 tile encoder/decoder in img_codec.cpp. OpenJPEG is
// federated into btf64.lib, which every WSI translation unit already links via
// tiff_inc.h, so this costs no new library - only the openjp25 include path.
// Needed for Aperio compression 33003/33005 and DICOM transfer syntaxes
// 1.2.840.10008.1.2.4.90/.91.
#ifndef USE_OPENJPEG
#define USE_OPENJPEG 1
#endif

// USE_JXL: build JPEG XL standalone-image and WSI tile encoding/decoding.
// libjxl is installed in c:\c\jxl; current applications use its /MD build
// selected centrally by prj/Directory.Build.props.
#ifndef USE_JXL
#define USE_JXL 1
#endif

// USE_CZI: build the Zeiss CZI loader (wsi_czi.cpp) and its sub-block decoder
// row in img_codec.cpp. Links libCZI as a DLL - libCZI is LGPL-3.0-or-later, and
// dynamic linking is what keeps that obligation off the rest of the tree, so do
// not switch this to libCZIStatic.lib without checking the licence position.
// Build it with prj/Directory.Build.props' $(LibCziDir)\build_md (a /MD build:
// std::shared_ptr and std::string cross the API boundary, so the CRT must match).
#ifndef USE_CZI
#define USE_CZI 1
#endif
