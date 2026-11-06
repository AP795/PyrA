# PyrA

PyrA is a standalone C++20 library for reading and rendering tiled,
multiresolution whole-slide images. Its GitHub repository is **PyrA**.

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
| `BUILD_TESTING` | ON | None; tests generate their own small images |

libCZI must use a compatible C++ runtime and remain dynamically linked.
Third-party libraries retain their own licenses and distribution requirements.

## Use from CMake

```cmake
add_subdirectory(path/to/Pyra)
target_link_libraries(my_application PRIVATE Pyra::pyra)
```

The target supplies its include directories, C++20 requirement, feature
configuration, and link dependencies. Include `wsi.h` for the reader,
`img_codec.h` for codecs, and `ptf_write.h` for the PTF writer.
Logging can be redirected with `pyr::set_log_sink`; otherwise it writes
to stderr. No host-defined service symbols are required.

The standalone tests cover generated images and reader/codec integration.
They do not replace validation against real SVS, NDPI, DICOM, or CZI slides.
Initial validation uses Windows x64, MSVC 19.51, Release, with JPEG and
OpenJPEG enabled. 
No source-code license is selected by this initial fork.
