# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

if(NOT MSVC)
	message(FATAL_ERROR "The bundled Windows SDK uses MSVC libraries")
endif()

set(pyra_btf_release "${PYRA_WINDOWS_SDK}/btf/md/lib/Release/BTF64.lib")
if(NOT EXISTS "${pyra_btf_release}")
	message(FATAL_ERROR "Missing ${pyra_btf_release}. Build the SDK with Deps/build_windows.ps1.")
endif()
set(TIFF_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/Deps/libtiff47")
set(JPEG_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/Deps/jpegturbo3_1_3/src")
set(PYRA_JPEG_CONFIG_DIR "${PROJECT_SOURCE_DIR}/Deps/jpegturbo3_1_3")
set(TIFF_LIBRARY "${pyra_btf_release}")
set(JPEG_LIBRARY "${pyra_btf_release}")
set(PYRA_OPENJPEG_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/Deps/openjp25")
set(PYRA_OPENJPEG_LIBRARY "${pyra_btf_release}")
set(openjph_DIR "${PYRA_WINDOWS_SDK}/openjph/lib/cmake/openjph")
set(DCMTK_DIR "${PYRA_WINDOWS_SDK}/dcmtk/Release/cmake")
