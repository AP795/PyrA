<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# PyrA codec upload: offline Linux build

This source package retains the folders from Pyra_dep.zip and adds a small
CMake adapter. Build it after extracting the ZIP on 64-bit little-endian Linux:

```sh
cd Pyra_dep_linux
bash build.sh
```

Prerequisites are GCC/G++ (or Clang), CMake >= 3.20, Make, Bash and the standard
C/C++ development files. No root access, network, package manager, Python,
assembler or preinstalled codec SDK is used. The default build generates
position-independent static libraries, tests them, and installs headers and
libraries under `out/install/`. Build artifacts can be removed and regenerated.
`PYRA_BUILD_JOBS=4 bash build.sh` adjusts build parallelism.

The upload contains libjpeg-turbo 3.1.3, libtiff 4.7, OpenJPEG 2.5.0, zlib,
OpenJPH 0.33.0, and cpp-httplib 0.36.0. The latter
is header-only and needs no library build. Source files and per-file license
notices remain intact. The returned ZIP also restores the missing JPEG
README.ijg notice from the official libjpeg-turbo 3.1.3 source distribution.
This software is based in part on the work of the Independent JPEG Group.

The original JPEG/OpenJPEG build scripts are missing, and the supplied JPEG,
TIFF and OpenJPEG configuration headers target Windows. The adapter copies
sources into its build tree, replaces only those platform configs, compiles
JPEG's 8/12/16-bit modules, selects Linux TIFF I/O and pthreads, and uses
OpenJPH's own build system. Uploaded source folders are never edited.

JPEG assembly SIMD is disabled to avoid requiring NASM; this is a functional,
portable build, not a performance baseline. OpenJPH retains its own runtime
SIMD dispatch. TIFF supports its internal codecs (including LZW and PackBits)
and JPEG. Deflate and PixarLog use the uploaded zlib132 sources by default;
no system zlib development files are used. For the older upload without zlib,
explicitly disable these two TIFF codecs:

```sh
PYRA_DEPENDENCY_ZLIB=OFF bash build.sh
```

JBIG, LERC, LZMA, WebP and Zstd are omitted. These are codec limitations
of this minimal upload, not missing requirements of the browser viewer.

To use this adapter from a PyrA source checkout with the dependency ZIP
extracted elsewhere:

```sh
bash tests/gui/web/dependencies/build.sh /path/to/extracted/deps /path/to/build
```

The codec test checks lossless JPEG 8/12/16-bit round trips, TIFF LZW and
Deflate round trips, an OpenJPEG lossless round trip, and OpenJPH and httplib
linkage/initialization.
It does not benchmark codecs or establish full upstream regression coverage.
