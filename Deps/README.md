<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# Bundled dependencies

This directory contains the dependency sources from the previous Linux bundle:
libjpeg-turbo 3.1.3, libtiff 4.7.0, OpenJPEG 2.5.0, zlib 1.3.2, OpenJPH 0.33.0,
DCMTK 3.7.0, and cpp-httplib 0.36.0, plus libjxl 0.11.0 with its Brotli,
Highway 1.2.0 and skcms copies (`libjxl/`, trimmed to what the libraries need;
`libjxl/SOURCE-NOTICES.txt` lists the revisions and what was removed). The source directories retain their
upstream notices. The libtiff, OpenJPEG, and zlib notices are available in
their respective source directories. LibTIFF, OpenJPEG, and OpenJPH also have
`SOURCE-NOTICES.txt` with copyright and license text from their source headers.
PyrA installs notices for the bundled
codecs it uses and the web viewer's cpp-httplib license under
`share/Pyra/third-party/` by default. This software is based in part on the
work of the Independent JPEG Group.

To build the codec and DCMTK SDK on 64-bit little-endian Linux, run from the
PyrA root:

```sh
bash Deps/build.sh
```

Prerequisites are GCC/G++ (or Clang), CMake >= 3.20, Make, Bash and the standard
C/C++ development files. No root access, network, package manager, Python,
assembler or preinstalled codec SDK is used. The default build generates
position-independent static libraries, tests them, and installs the SDK under
`out/build/linux-deps/install/`. Set `PYRA_BUILD_JOBS` to adjust parallelism or
pass a build directory as the only argument.

For the full offline Linux build, including DCMTK and the web viewer, use
[`bash ViewW/build_linux.sh`](../ViewW/README.md#build-and-run-on-linux).

Configure consumers with `-DPYRA_LINUX_SDK=/path/to/install`. The adapter
copies JPEG, TIFF and OpenJPEG into its build tree before
generating Linux platform configuration headers. It compiles JPEG's 8/12/16-bit
modules, selects Linux TIFF I/O and pthreads, and uses OpenJPH's build system.
The web viewer uses the bundled header-only cpp-httplib on Linux and Windows.

The Linux build uses portable JPEG code and needs no assembler. OpenJPH retains its own runtime
SIMD dispatch. TIFF supports its internal codecs (including LZW and PackBits)
and JPEG. Deflate and PixarLog use the bundled zlib by default. To disable
these two TIFF codecs:

```sh
PYRA_DEPENDENCY_ZLIB=OFF bash Deps/build.sh
```

JBIG, LERC, LZMA, WebP and Zstd are omitted from TIFF. These are codec
limitations of this minimal upload, not missing requirements of the browser viewer.

`jxrlib/` (jxrlib 1.1, Microsoft, BSD; the portable copy libCZI carries, see
`jxrlib/LICENSE`) and `zstd/` (zstd 1.5.7 decompressor, BSD) decode JPEG XR and
zstd CZI sub-blocks (`Core/img_czi.cpp`, `PYR_WITH_JXR` / `PYR_WITH_ZSTD`, on by
default); jxrlib's encoder also makes JPEG XR a PTF tile codec (`Core/img_jxr.cpp`). PyrA compiles them directly; no SDK step is needed. libCZI itself is
used only by the CZI loader, so a build without it, such as the browser client,
still decodes CZI tiles. `jxrlib/pyra/JxrDecode_Config.h` replaces the
configure step of libCZI's build with preprocessor tests.

The same adapter also accepts an Emscripten configure (`emcmake cmake -S Deps`),
which builds the codecs of the web viewer's browser client for wasm32 with
WebAssembly exception handling, threads and SIMD (`-msimd128`). See [the viewer README](../ViewW/README.md#building-the-client).

libjxl is built by its own CMake build with the options of
`libjxl_options.cmake` (static libraries only: no tools, tests, jpegli or
transcoding; skcms as colour engine), in this adapter for Linux and Emscripten
(`PYRA_DEPENDENCY_JXL`, ON by default) and by the `JXL` component of
`build_windows.ps1`. Highway selects the CPU's SIMD at run time natively.

The codec test checks lossless JPEG 8/12/16-bit round trips, TIFF LZW and
Deflate round trips, an OpenJPEG lossless round trip, and OpenJPH and httplib
linkage/initialization.
It does not benchmark codecs or establish full upstream regression coverage.

## Windows x64

Use Visual Studio 2026 with C++ tools, CMake with the VS 18 generator, and NASM.
From the PyrA root in PowerShell, run:

```powershell
.\Deps\build_windows.ps1
```

The script builds Release and Debug SDKs under `out/Deps/windows/` (components
`Btf`, `OpenJPH`, `DCMTK` and `JXL`, all by default; `-Components` selects some). For the
nested WSIA workspace, pass `-BuildRoot ../out/Deps`. The BTF64 archive includes
the bundled TIFF, JPEG, OpenJPEG and zlib sources with JPEG SIMD enabled.
It is built with both MSVC runtimes used by the host projects.
