# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# libjxl options of every bundled SDK: the static decoder/encoder libraries
# with the Brotli, Highway and skcms copies in libjxl/third_party, nothing else.
# Deps/CMakeLists.txt includes this file; build_windows.ps1 preloads it (-C).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_INSTALL_LIBDIR lib CACHE STRING "" FORCE)
foreach(feature TOOLS DEVTOOLS JPEGLI BENCHMARK EXAMPLES SJPEG OPENEXR MANPAGES DOXYGEN
	JNI PLUGINS TRANSCODE_JPEG VIEWERS FUZZERS TCMALLOC)
	set(JPEGXL_ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "" FORCE)
set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
# Emscripten only: every object of the threaded browser client uses shared memory.
set(JPEGXL_ENABLE_WASM_THREADS ON CACHE BOOL "" FORCE)
