# PyrA

PyrA is a standalone library with a C interface for reading and rendering
tiled, multiresolution whole-slide images. Its implementation uses C++20;
applications can use C or C++. Its GitHub repository is **PyrA**.

## Features

- SVS, NDPI, generic tiled TIFF (`.bif` and `.ptf`), and image galleries
  (`.glr`). Ordinary `.tif` files are gallery members, not WSI entry points.
- Optional DICOM WSI and Zeiss CZI loaders.
- JPEG decoding, with optional JPEG 2000 and JPEG XL codecs.
- RGB8, RGB16, Gray8, Gray16, and floating-point raster types.
- Region rendering, channel and focal-plane selection, tile caching, and
  parallel decoding.
- Associated images and metadata, and a tiled PTF writer.

ICC profiles are captured but are not applied to rendered pixels.

## Build

Requires CMake 3.20 or newer, a C++20 compiler, libtiff, and
**libjpeg-turbo 3.0 or newer**. An ordinary IJG libjpeg or libjpeg-turbo 2.x
does not provide the required RGB and 12/16-bit JPEG APIs.

```sh
cmake -S . -B build -DPYR_WITH_OPENJPEG=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The default build additionally enables OpenJPEG. Install its development
package, or pass `PYRA_OPENJPEG_INCLUDE_DIR` and `PYRA_OPENJPEG_LIBRARY` if
it is not discoverable through pkg-config or standard search paths.
Use `CMAKE_PREFIX_PATH` or CMake's `TIFF_INCLUDE_DIR`, `TIFF_LIBRARY`,
`JPEG_INCLUDE_DIR`, and `JPEG_LIBRARY` variables for custom installations.

| Option | Default | Additional dependency |
| --- | --- | --- |
| `PYR_WITH_OPENJPEG` | ON | OpenJPEG 2.x |
| `PYR_WITH_JXL` | OFF | libjxl >= 0.9, discoverable through pkg-config |
| `PYR_WITH_DCMTK` | OFF | DCMTK |
| `PYR_WITH_CZI` | OFF | libCZI, linked dynamically |
| `PYRA_WITH_OPENSLIDE` | OFF | OpenSlide >= 4.0.1, linked dynamically |
| `BUILD_TESTING` | ON | None; tests generate their own small images |
| `PYRA_BUILD_GUI_TEST` | OFF | Qt 6 Widgets; viewer and performance evaluator |
| `PYRA_ENABLE_IPO` | ON | Compiler/linker support for Release IPO/LTO |
| `PYRA_ENABLE_AVX2` | OFF | AVX2-capable target CPU for Release binaries |

Release builds use the compiler's normal speed optimization settings and enable
IPO/LTO after a compiler/linker support check. MSVC uses `/GL` and `/LTCG`;
other supported toolchains use CMake's corresponding IPO settings. Pass
`-DPYRA_ENABLE_IPO=OFF` to disable IPO, including for toolchains without support.

Enable `PYRA_ENABLE_AVX2` to add `/arch:AVX2` with MSVC or `-mavx2` with
GCC/Clang in Release. This requires an AVX2-capable CPU and operating system;
there is no runtime fallback for older machines. The default is OFF for
portable distribution. The local `windows-release-local` preset enables both
IPO and AVX2. Set `PYRA_ENABLE_AVX2=OFF` for a build without that requirement.

These options cover Pyra's library and enabled tests/viewer, leave Debug
settings unchanged, and do not modify prebuilt codec SDKs. They are private
build settings and are not exported to applications linking `Pyra::pyra`.

For a Windows static JPEG XL SDK, enable `PYR_WITH_JXL` and provide
`PYRA_JXL_INCLUDE_DIR` (containing `jxl/decode.h`) and
`PYRA_JXL_LIBRARY_DIR` (containing `jxl`, `jxl_cms`, Brotli and Highway archives).
The SDK must be >= 0.9 and match the selected MSVC runtime/configuration.
The local Release preset enables the 0.11 SDK under `C:/c/jxl`. Linux continues
to discover JPEG XL through pkg-config. If a source needs an unavailable codec,
the C interface returns `PYRA_UNSUPPORTED` with the compression name and build
requirement; the viewer shows that error rather than treating it as a cache issue.

libCZI must use a compatible C++ runtime and remain dynamically linked.
Third-party libraries retain their own licenses and distribution requirements.

### Optional OpenSlide backend

The C API can select **PyrA native** or **OpenSlide** per context. Native remains
the default. The existing GUI's **Open WSI** menu offers the same choice; reopen
the same slide with the other reader to reuse its Record / Stop / Replay workload.
See [comparison and cache semantics](docs/c-api.md#reader-backends).

On Windows, fetch the pinned official GitHub binary build 4.0.1.2 (OpenSlide
4.0.1) and its matching complete sources into the build tree. The script checks
both SHA-256 hashes; normal configuration does not access the network.

```powershell
cmake -DPYRA_OPENSLIDE_DEST=C:/tmp/bld/Pyra/deps -P cmake/FetchOpenSlide.cmake
cmake -S . -B C:/tmp/bld/Pyra -DPYRA_WITH_OPENSLIDE=ON `
  -DPYRA_OPENSLIDE_ROOT=C:/tmp/bld/Pyra/deps/openslide-bin-4.0.1.2-windows-x64
cmake --build C:/tmp/bld/Pyra --config Release --parallel 1
ctest --test-dir C:/tmp/bld/Pyra -C Release --output-on-failure
```

Use CMake from Visual Studio 2026 for its VS 18 generator; an older CMake on
PATH may not support it. These commands extend the existing configured tree;
a fresh tree also needs the native codec SDK inputs described above.

On Linux, install OpenSlide >= 4.0.1 and its development files from
[upstream's packages or source](https://openslide.org/download/), then extend the
normal Release configuration with `-DPYRA_WITH_OPENSLIDE=ON`. Discovery uses
`pkg-config openslide`; set `PKG_CONFIG_PATH` for a custom prefix. The system
OpenSlide version may be older than required. For an official binary SDK without
a pkg-config file, set `PYRA_OPENSLIDE_ROOT` to its extracted prefix instead.
Keep the libjpeg-turbo 3 inputs
from the [Linux setup](tests/gui/README.md#ubuntu-2404--wsl-setup).

OpenSlide is LGPL-2.1 and remains a separate shared library. The Windows fetch
includes upstream component notices and complete corresponding sources. Installation
copies the DLL, notices and matching source archive when using that SDK layout;
custom SDK distributors must provide the corresponding notices and sources.
The Linux package's runtime dependencies remain the distributor's responsibility.
See [upstream's redistribution notes](https://openslide.org/docs/windows/#licensing).

### Visual Studio folder mode

Open this source directory with **File > Open > Folder**. The shared
`CMakePresets.json` defines a hidden Visual Studio 2026 x64/v145 Release base.
Put machine-specific SDK paths and the build directory in the Git-ignored
`CMakeUserPresets.json`, inheriting `windows-vs2026-base`. Visual Studio lists
the resulting user preset after reopening the folder. The local development
preset is `windows-release-local`, displayed as **Windows x64 Release - local
SDKs**, and builds under `C:/tmp/bld/Pyra/ide-release` with the GUI enabled.

The same local configuration can be used from PowerShell:

```powershell
cmake --preset windows-release-local
cmake --build --preset windows-release-local
ctest --preset windows-release-local
```

The local `C:/c/btf/md/BTF64.lib` archive uses the Release `/MD` runtime.
The default Visual Studio `x64-Debug` configuration does not supply SDK paths
and uses a different runtime. Use the configured Release preset; a real Debug
build needs compatible Debug SDK libraries. The local JPEG source layout also
needs both its `src` headers and its generated `jconfig.h` parent directory.

## Use from CMake

```cmake
add_subdirectory(path/to/Pyra)
target_link_libraries(my_application PRIVATE Pyra::pyra)
```

`Pyra::pyra` is a shared library exposing `<pyra/pyra.h>`. Its public header
requires no C++ runtime types, Qt headers, codec SDKs, or generated configuration
headers. See [the C interface reference](docs/c-api.md) for ownership, resource
budgets, image layout, errors, and an example. Diagnostics default to stderr.

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

## Linux test GUI

The optional viewer lives in [`tests/gui/`](tests/gui/README.md). It provides
pan/zoom, a navigable minimap, channel/Z selection, metadata, and a
Record / Stop / Replay render performance evaluator. See that directory for Ubuntu 24.04 and WSLg setup,
build commands, and timing semantics. Its reader uses only the C interface;
synthetic fixture generation runs in a separate helper executable. Qt links
only to the test application and test support code.
The viewer reads configuration and saves its last successfully opened image
under `~/.config/pyra_gui_test/`, restoring that image on the next normal start.

## Scope of the initial fork

The WSIA checkout continues to use its original `pyr/` directory. Pyra is
maintained independently; no automatic synchronization is configured.

The standalone tests cover generated images and reader/codec integration.
They do not replace validation against real SVS, NDPI, DICOM, or CZI slides.
Validation covers Windows x64 with MSVC 19.51 and Ubuntu 24.04 x64 with
GCC 13, both in Release with JPEG and OpenJPEG enabled. The optional Qt test
viewer/evaluator also passes its synthetic checks through WSLg in Client.
JPEG XL, DICOM, CZI, and real-slide GUI performance have not been validated
for this fork.

## License

Pyra is licensed under the [MIT License](license.md).
Third-party libraries retain their own licenses and distribution requirements.
