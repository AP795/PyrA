#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 24.04 supplies JPEG 2.x. Build an isolated JPEG 3.x dependency with
# JPEG-8 ABI compatibility so the system libtiff can use this same library.
version=3.1.3
checksum=075920b826834ac4ddf97661cc73491047855859affd671d52079c6867c1c6c0
prefix=${1:-"$HOME/.local/opt/pyra-jpeg"}
work="${XDG_CACHE_HOME:-$HOME/.cache}/pyra-deps/jpeg-$version"
mkdir -p "$work"
archive="$work/libjpeg-turbo-$version.tar.gz"
if [[ ! -f "$archive" ]]; then
	curl --fail --location --retry 3 \
		"https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$version/libjpeg-turbo-$version.tar.gz" \
		--output "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check
if [[ ! -d "$work/libjpeg-turbo-$version" ]]; then
	tar -xzf "$archive" -C "$work"
fi
cmake -S "$work/libjpeg-turbo-$version" -B "$work/build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
	-DCMAKE_INSTALL_LIBDIR=lib -DWITH_JPEG8=ON \
	-DENABLE_SHARED=ON -DENABLE_STATIC=OFF -DWITH_TURBOJPEG=OFF
cmake --build "$work/build" --parallel 2
cmake --install "$work/build"
printf '\nJPEG dependency installed in %s\n' "$prefix"
