#!/usr/bin/env bash
# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

set -euo pipefail

if [[ $# -gt 1 ]]; then
	printf 'Usage: bash ViewW/build_linux.sh [BUILD_DIRECTORY]\n' >&2
	exit 2
fi
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$source_dir/out/build/linux-web"}
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
jobs=${PYRA_BUILD_JOBS:-2}
# Optional browser client: a folder holding pyra_client.js and pyra_client.wasm
# from an Emscripten build (copied from another machine is fine; see README).
client_dir=${PYRA_WEB_CLIENT_DIR:-}
if [[ -n "$client_dir" ]]; then
	client_dir=$(cd -- "$client_dir" && pwd)
fi

bash "$source_dir/Deps/build.sh" "$build_dir/deps"
printf 'PyrA codecs: OpenJPEG=ON OpenJPH=ON DCMTK=ON\n'

cmake -S "$source_dir" -B "$build_dir/pyra" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$build_dir/install" -DCMAKE_INSTALL_LIBDIR=lib \
	-DPYRA_BUILD_GUI_TEST=OFF -DPYRA_BUILD_WEB_TEST=ON \
	-DPYR_WITH_OPENJPEG=ON -DPYR_WITH_JXL=OFF -DPYR_WITH_CZI=OFF \
	-DPYR_WITH_OPENJPH=ON -DPYR_WITH_DCMTK=ON -DPYRA_WITH_OPENSLIDE=OFF \
	-DPYRA_LINUX_SDK="$build_dir/deps/install" \
	-DPYRA_WEB_CLIENT_DIR="$client_dir"
if [[ -n "$client_dir" ]]; then
	printf 'Browser client: embedded from %s\n' "$client_dir"
fi
cmake --build "$build_dir/pyra" --parallel "$jobs"
ctest --test-dir "$build_dir/pyra" --output-on-failure
cmake --install "$build_dir/pyra"
printf '\nRun: %s/bin/pyra_web_test --root /path/to/images\n' "$build_dir/install"
