#!/usr/bin/env bash
# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

set -euo pipefail

if [[ $# -gt 1 ]]; then
	printf 'Usage: bash Deps/build.sh [BUILD_DIRECTORY]\n' >&2
	exit 2
fi

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_dir=${1:-"$source_dir/../out/build/linux-deps"}
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
prefix="$build_dir/install"
jobs=${PYRA_BUILD_JOBS:-2}

cmake -S "$source_dir" -B "$build_dir/build" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
	-DPYRA_DEPENDENCY_ZLIB="${PYRA_DEPENDENCY_ZLIB:-ON}"
cmake --build "$build_dir/build" --parallel "$jobs"
ctest --test-dir "$build_dir/build" --output-on-failure
cmake --install "$build_dir/build"

# Build the shared DICOM subset without system codec dependencies.
cmake -S "$source_dir/dcmtk370" -B "$build_dir/dcmtk-build" -DCMAKE_BUILD_TYPE=Release \
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
printf '\nBundled Linux SDK: %s\n' "$prefix"
