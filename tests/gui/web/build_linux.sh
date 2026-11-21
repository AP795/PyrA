#!/usr/bin/env bash
# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
	printf 'Usage: bash tests/gui/web/build_linux.sh EXTRACTED_DEPENDENCIES [BUILD_DIRECTORY]\n' >&2
	exit 2
fi
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
dependency_dir=$(cd -- "$1" && pwd)
build_dir=${2:-"$source_dir/out/build/linux-web"}
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
prefix="$build_dir/dependency-prefix"
zlib=${PYRA_DEPENDENCY_ZLIB:-ON}
jobs=${PYRA_BUILD_JOBS:-2}

cmake -S "$source_dir/tests/gui/web/dependencies" -B "$build_dir/dependency-build" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
	-DPYRA_DEPENDENCY_SOURCE_DIR="$dependency_dir" -DPYRA_DEPENDENCY_ZLIB="$zlib"
cmake --build "$build_dir/dependency-build" --parallel "$jobs"
cmake --install "$build_dir/dependency-build"

# OpenJPH is part of the adapter build; its install provides the CMake package.
openjph_dir="$prefix/lib/cmake/openjph"
openjph=OFF
[[ -f "$openjph_dir/openjph-config.cmake" ]] && openjph=ON

# Optional DCMTK 3.7.0 subset (the modules WSIA builds), as a separate CMake
# project: static, position-independent, no apps/tests, no external libraries.
dcmtk_source="$dependency_dir/dcmtk370"
dcmtk_dir="$prefix/lib/cmake/dcmtk"
dicom=OFF
if [[ -f "$dcmtk_source/CMakeLists.txt" ]]; then
	cmake -S "$dcmtk_source" -B "$build_dir/dcmtk-build" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
		-DDCMTK_MODULES="ofstd;oflog;dcmdata;dcmimgle;dcmimage;dcmsr;dcmiod;dcmfg;dcmseg" \
		-DBUILD_APPS=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DDCMTK_DEFAULT_DICT=builtin -DDCMTK_ENABLE_CHARSET_CONVERSION=OFF -DDCMTK_ENABLE_STL=OFF \
		-DDCMTK_WITH_DOXYGEN=OFF -DDCMTK_WITH_ICONV=OFF -DDCMTK_WITH_ICU=OFF \
		-DDCMTK_WITH_OPENJPEG=OFF -DDCMTK_WITH_OPENSSL=OFF -DDCMTK_WITH_PNG=OFF \
		-DDCMTK_WITH_SNDFILE=OFF -DDCMTK_WITH_TIFF=OFF -DDCMTK_WITH_XML=OFF -DDCMTK_WITH_ZLIB=OFF \
		-DDCMTK_WITH_THREADS=ON
	cmake --build "$build_dir/dcmtk-build" --parallel "$jobs"
	cmake --install "$build_dir/dcmtk-build"
	dicom=ON
fi
printf 'PyrA codecs: OpenJPEG=ON OpenJPH=%s DCMTK=%s\n' "$openjph" "$dicom"

cmake -S "$source_dir" -B "$build_dir/pyra" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$build_dir/install" -DCMAKE_INSTALL_LIBDIR=lib \
	-DPYRA_BUILD_GUI_TEST=OFF -DPYRA_BUILD_WEB_TEST=ON \
	-DPYRA_HTTPLIB_DIR="$dependency_dir/httplib" \
	-DPYR_WITH_OPENJPEG=ON -DPYR_WITH_JXL=OFF -DPYR_WITH_CZI=OFF \
	-DPYR_WITH_OPENJPH="$openjph" -Dopenjph_DIR="$openjph_dir" \
	-DPYR_WITH_DCMTK="$dicom" -DDCMTK_DIR="$dcmtk_dir" \
	-DPYRA_WITH_OPENSLIDE=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
	-DTIFF_INCLUDE_DIR="$prefix/include" -DTIFF_LIBRARY="$prefix/lib/libtiff.a" \
	-DJPEG_INCLUDE_DIR="$prefix/include" -DJPEG_LIBRARY="$prefix/lib/libjpeg.a" \
	-DPYRA_OPENJPEG_INCLUDE_DIR="$prefix/include" -DPYRA_OPENJPEG_LIBRARY="$prefix/lib/libopenjp2.a" \
	-DPYRA_TIFF_EXTRA_LIBRARIES="$([[ $zlib == ON ]] && printf '%s' "$prefix/lib/libz.a" || true)"
cmake --build "$build_dir/pyra" --parallel "$jobs"
ctest --test-dir "$build_dir/pyra" --output-on-failure
cmake --install "$build_dir/pyra"
printf '\nRun: %s/bin/pyra_web_test --root /path/to/images\n' "$build_dir/install"
