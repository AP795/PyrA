<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# PyrA

PyrA is a standalone library, written in C++20 with a C interface, for reading
and rendering tiled, multiresolution whole-slide images (WSIs). Its goal is to
provide a foundation for efficient browsing, processing and analysis of multi-gigabyte
WSIs.

The two widely used alternatives, Bio-Formats and OpenSlide, are of limited
use for this purpose. Bio-Formats is prohibitively slow for some applications.
OpenSlide, positioned as a fast, lightweight alternative, limits pixels to
8 bits per channel (RGB or grayscale) and does not support multi-channel
images or z-stacks.

PyrA aims to be at least as fast as OpenSlide. The test viewer can
replay one recorded workload through either reader to compare them.

The project includes a Qt 6 desktop viewer for testing on Windows and Linux,
and an optional [localhost browser viewer](tests/gui/web/README.md) whose
server builds without a GUI toolkit. The longer-term goal is to grow it into
a WSI viewing and analysis platform.

The GitHub repository of the project is **PyrA**. The CMake package, targets
and identifiers are spelled `Pyra` / `pyra`.

## Features

- Formats: SVS, NDPI, generic tiled TIFF (`.bif` and `.ptf`), Win300 ZYP
  (`.zyp`), and image galleries (`.glr`, a directory of independent images
  presented as one slide).
- Ordinary TIFF images and legacy IPX file-sets open as galleries:
  `pyra_make_gallery` (C API, command-line tool, and the web viewer's **Make
  gallery** button) lays a directory out and writes its `.glr` manifest.
- Optional DICOM WSI and Zeiss CZI loaders (both require third-party
  libraries), and a DICOM WSI writer whose producer identity a host sets.
- JPEG decoding, with optional JPEG 2000, HTJ2K (OpenJPH) and JPEG XL codecs.
- Tile caches in one of two layouts, chosen per context: one arena per cache
  for the whole slide, or one pool per level (see the C interface's
  `cache_topology`).
- RGB8, RGB16, Gray8, Gray16, and floating-point raster types.
- Region rendering, channel and focal-plane selection, tile caching, and
  parallel decoding.
- Associated images and metadata.
- A writer for PTF, PyrA's tiled TIFF variant, which records the channel and
  focal plane of each level and may hold tiles encoded with JPEG2000, HTJ2K 
  and JPEG XL.
- A synthetic slide generator: `pyra_make_synthetic` writes a bright-field
  IHC z-stack PTF of any size, tile size, plane count, plane spacing, focus
  position, pixel size and compression. Tumour nucleus size, spacing and
  positive fraction, tumour area and the lymphocyte share are also
  adjustable, and `pyra_synthetic_count_nuclei` returns the matching ground
  truth (C API, command-line tool, and both test viewers' **Generate**
  dialog).

ICC profiles are captured but for the moment are not applied to rendered pixels.

## Build

Requires CMake 3.20 or newer, a C++20 compiler (GCC/G++13+) and CMake 3.20+

| Option | Default | Requirement |
| --- | --- | --- |
| `PYR_WITH_OPENJPEG` | ON | OpenJPEG 2.x |
| `PYR_WITH_OPENJPH` | OFF | OpenJPH >= 0.33 (HTJ2K) |
| `PYR_WITH_JXL` | OFF | libjxl >= 0.9 |
| `PYR_WITH_DCMTK` | OFF | DCMTK >= 3.7.0 (HTJ2K transfer syntaxes) |
| `PYR_WITH_CZI` | OFF | libCZI, linked dynamically |
| `PYRA_WITH_OPENSLIDE` | OFF | OpenSlide >= 4.0.1, linked dynamically |
| `BUILD_TESTING` | ON | None; tests generate their own small images |
| `PYRA_BUILD_GUI_TEST` | OFF | Qt 6 Widgets; viewer and performance evaluator |
| `PYRA_BUILD_WEB_TEST` | OFF | Localhost browser viewer with cpp-httplib |
| `PYRA_ENABLE_IPO` | ON | Compiler and linker support for IPO/LTO |
| `PYRA_ENABLE_AVX2` | OFF | AVX2-capable target CPU |

The five codec and loader options use the `PYR_` prefix; all other options use
`PYRA_`.

If a slide needs a codec that was not built in, the C interface returns
`PYRA_UNSUPPORTED` with the compression name and the build requirement, and
the viewer displays that error.

libCZI must be built with a compatible C++ runtime and must remain dynamically
linked.

### Optimization

Release builds use the compiler's normal speed optimization and IPO/LTO
(`/GL` and `/LTCG` with MSVC, CMake's IPO settings elsewhere). Configuration
fails on a toolchain without IPO support; pass `-DPYRA_ENABLE_IPO=OFF` there,
or to disable IPO.

`PYRA_ENABLE_AVX2` adds `/arch:AVX2` with MSVC or `-mavx2` with GCC/Clang in
Release. The binaries then require an AVX2-capable CPU and operating system,
with no runtime fallback, so the option is OFF by default for portable
distribution.

Both options apply to the PyrA library and the enabled tests and viewer. They
leave Debug settings unchanged, do not modify prebuilt codec SDKs, and are not
exported to applications linking `Pyra::pyra`.

### JPEG XL

On Linux, `PYR_WITH_JXL` discovers libjxl through pkg-config. For a static SDK
on Windows, also provide `PYRA_JXL_INCLUDE_DIR` (containing `jxl/decode.h`)
and `PYRA_JXL_LIBRARY_DIR` (containing the `jxl`, `jxl_cms`, Brotli and
Highway archives). The SDK must match the selected MSVC runtime and
configuration.

### Optional OpenSlide backend

The C API selects **PyrA native** (the default) or **OpenSlide** per context.
The viewer's **Open WSI** menu offers the same choice; reopen a slide with the
other reader to reuse its Record / Stop / Replay workload.
See [comparison and cache semantics](docs/c-api.md#reader-backends).

On Windows, fetch the pinned official GitHub binary build 4.0.1.2 (OpenSlide
4.0.1) and its matching complete sources into the build tree. The script checks
both SHA-256 hashes; normal configuration does not access the network.

```powershell
cmake -DPYRA_OPENSLIDE_DEST=out/deps -P cmake/FetchOpenSlide.cmake
cmake --preset windows-release-local `
  -DPYRA_WITH_OPENSLIDE=ON `
  -DPYRA_OPENSLIDE_ROOT="$PWD/out/deps/openslide-bin-4.0.1.2-windows-x64" `
  -DPYRA_OPENSLIDE_SOURCE_ARCHIVE="$PWD/out/deps/openslide-bin-4.0.1.2.tar.gz"
cmake --build --preset windows-release-local
ctest --preset windows-release-local
```

`windows-release-local` is a user preset that you create, as described under
[Visual Studio folder mode](#visual-studio-folder-mode); it supplies the codec
SDK paths. Use the CMake shipped with Visual Studio 2026 for its VS 18
generator; an older CMake on PATH may not support it.

On Linux, install OpenSlide >= 4.0.1 and its development files from
[upstream's packages or source](https://openslide.org/download/), then add
`-DPYRA_WITH_OPENSLIDE=ON` to the Release configuration. Discovery uses
`pkg-config openslide`; set `PKG_CONFIG_PATH` for a custom prefix. The
distribution's OpenSlide package may be older than required. For an official
binary SDK without a pkg-config file, set `PYRA_OPENSLIDE_ROOT` to its
extracted prefix instead. Keep the libjpeg-turbo 3 inputs from the
[Linux setup](tests/gui/README.md#ubuntu-2404--wsl-setup).

OpenSlide is LGPL-2.1 and remains a separate shared library. The Windows fetch
includes upstream component notices and complete corresponding sources.
Installation copies the DLL, notices and matching source archive when using
that SDK layout; distributors of a custom SDK must provide the corresponding
notices and sources. The runtime dependencies of a Linux package are the
distributor's responsibility.
See [upstream's redistribution notes](https://openslide.org/docs/windows/#licensing).

### Visual Studio folder mode

Open this source directory with **File > Open > Folder**. The shared
`CMakePresets.json` defines a hidden base preset, `windows-vs2026-base`:
Visual Studio 2026, x64, v145 with 64-bit host tools, Release, with the viewer
enabled. It derives the build and install directories from the preset name,
`out/build/<preset>` and `out/install/<preset>`, so executables, libraries,
intermediates, generated files and test output stay in the build tree.

The explicit host architecture keeps Visual Studio and command-line builds
compatible with the same incremental build state. Automated Windows launchers
must preserve `PROCESSOR_ARCHITECTURE` (`AMD64` on a native x64 OS); MSBuild
falls back to x86 host tools if that value is absent.

Put machine-specific SDK paths in the Git-ignored `CMakeUserPresets.json`:
a configure preset that inherits `windows-vs2026-base` and sets the dependency
variables described above, plus a build and a test preset that refer to it.
This README names all three `windows-release-local`. Visual Studio lists the
user preset after the folder is reopened, and the viewer is then
`out/build/windows-release-local/tests/gui/Release/pyra_gui_test.exe`.

The same preset can be used from PowerShell:

```powershell
cmake --preset windows-release-local
cmake --build --preset windows-release-local
ctest --preset windows-release-local
```

SDK libraries must match the runtime of the configuration being built; the
Release preset uses `/MD`. Visual Studio's default `x64-Debug` configuration
supplies no SDK paths and uses a different runtime, so a Debug build needs
Debug-compatible SDK libraries.

## Use from CMake

```cmake
add_subdirectory(path/to/Pyra)
target_link_libraries(my_application PRIVATE Pyra::pyra)
```

`Pyra::pyra` is a shared library exposing `<pyra/pyra.h>`. Its public header
requires no C++ runtime types, Qt headers, codec SDKs, or generated
configuration headers. See [the C interface reference](docs/c-api.md) for
ownership, resource budgets, image layout, errors, and an example.
Diagnostics default to stderr.

To install and consume an independently built package:

```sh
cmake --install build --config Release --prefix /path/to/pyra-install
```

```cmake
find_package(Pyra CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE Pyra::pyra)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix. Deploy the shared library
and the runtime libraries required by the enabled codecs alongside your
application or through the platform loader's search path. The install exports
the C reader interface; codec and PTF writer C++ headers remain internal and
are used by the source-tree tests through `pyra_core`.

## Test viewers

The optional Qt viewer lives in [`tests/gui/`](tests/gui/README.md) and runs on
Windows and Linux. It provides pan/zoom, a navigable minimap, channel/Z
selection, metadata, and a Record / Stop / Replay render performance
evaluator. See that directory for Ubuntu 24.04 and WSLg setup, build commands,
and timing semantics.

Its reader uses only the C interface; synthetic fixture generation runs in a
separate helper executable. Qt is linked only into the test application and
its support code. The viewer reads its configuration and saves its last
successfully opened image under Qt's user configuration location
(`~/.config/pyra_gui_test/` on Linux), restoring that image on the next normal
start.

The optional `pyra_web_test` viewer in `tests/gui/web/` runs without Qt. A local
C++ server renders slides through the PyrA C API, and the browser provides pan,
zoom, a minimap, metadata, and channel/Z selection. The image directory defaults
to the source checkout's `tst_in/`. From the PyrA source root on Linux, build
with the extracted dependency bundle and launch with `--root` to browse a
different directory. See the [web viewer setup guide](tests/gui/web/README.md)
for CMake, WSL, and launch options.

```sh
bash tests/gui/web/build_linux.sh /path/to/Pyra_dep_linux "$PWD/out"
"$PWD/out/install/bin/pyra_web_test" --root /path/to/images
```

Open `http://127.0.0.1:8765/` in your browser.

## Status and validation

PyrA was forked from `pyr`, the slide reader of the WSIA whole-slide analysis
software. The two are maintained independently, with no automatic
synchronization at present.

The automated tests cover generated images and reader/codec integration.
They do not replace validation against real SVS, NDPI, DICOM, or CZI slides.
Validation covers Windows x64 with MSVC 19.51 and Ubuntu 24.04 x64 with
GCC 13, both in Release with JPEG and OpenJPEG enabled. The viewer also passes
its synthetic checks under WSLg. JPEG XL, DICOM, CZI, and real-slide viewer
performance have not been validated for this fork.

## License

PyrA is licensed under the [MIT License](license.md).
Third-party libraries retain their own licenses and distribution requirements.
