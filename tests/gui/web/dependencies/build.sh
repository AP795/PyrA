#!/usr/bin/env bash
# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
dependency_dir=$(cd -- "${1:-$source_dir}" && pwd)
build_dir=${2:-"$source_dir/out"}
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
cmake -S "$source_dir" -B "$build_dir/build" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$build_dir/install" -DCMAKE_INSTALL_LIBDIR=lib \
	-DPYRA_DEPENDENCY_SOURCE_DIR="$dependency_dir" \
	-DPYRA_DEPENDENCY_ZLIB="${PYRA_DEPENDENCY_ZLIB:-ON}"
cmake --build "$build_dir/build" --parallel "${PYRA_BUILD_JOBS:-2}"
ctest --test-dir "$build_dir/build" --output-on-failure
cmake --install "$build_dir/build"
printf '\nCodec SDK installed in %s/install\n' "$build_dir"
