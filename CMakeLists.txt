# Standalone build for Pyra, the pyramid WSI reader.
#
# The solution builds these sources directly from each .vcxproj; this file is
# not part of that. It exists so the library can be built on its own, and in
# particular on Linux, which is the only real test of the claim that pyr/ is
# platform-independent. Windows-specific code here sits behind _WIN32 (or
# _MSC_VER for compiler pragmas): pyr_win.h, the UTF-16 branch of str2wstr in
# pyr_rt.cpp, and in wsi_czi.cpp the delay-load probe and the Win32 ReadFile arm
# of the read-cost measurement.
#
# Requires libtiff, libjpeg-turbo >= 3.0 and, by default, OpenJPEG 2.x; with
# PYR_WITH_JXL, libjxl >= 0.9.
#
# Host services are injected through pyr_rt.h rather than linked by name: the
# log sink (set_log_sink) and the verbose trace file (set_debug_file). An
# executable linking this library needs to define nothing; both default to off
# or stderr.

cmake_minimum_required(VERSION 3.20)
project(Pyra VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optional loaders and codecs. The core - TIFF-family loaders over baseline JPEG
# - needs only libtiff and libjpeg, so a bare configure builds something useful
# on any machine. Everything else is opt-in because it needs a library the host
# may not have.
option(PYR_WITH_OPENJPEG "JPEG 2000 tiles (Aperio 33003/33005, DICOM .90/.91)" ON)
option(PYR_WITH_JXL      "JPEG XL tile and image codec"                        OFF)
option(PYR_WITH_DCMTK    "DICOM VL Whole Slide Microscopy loader"              OFF)
option(PYR_WITH_CZI      "Zeiss CZI loader (libCZI, LGPL - link dynamically)"  OFF)

find_package(TIFF REQUIRED)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)

# libjpeg-turbo 3.0 or newer, specifically - not any libjpeg.
#
# The shared JPEG decoder writes straight into an RGB surface using the
# JCS_EXT_* colour spaces, which are a libjpeg-turbo extension; IJG's libjpeg
# does not define them. JPEGs deeper than 8 bits (12-bit lossy, DICOM Process 4;
# lossless up to 16) go through J12SAMPLE / J16SAMPLE and jpeg12_ / jpeg16_
# read_scanlines, which first appeared in libjpeg-turbo 3.0 - 2.x has JCS_EXT_*
# but not these. Both are checked here, with a link, so an unsuitable libjpeg is
# a one-line configure failure rather than a compile error two files in. A
# failed result is dropped from the cache so the check re-runs once the library
# is fixed.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${JPEG_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES JPEG::JPEG)
check_cxx_source_compiles([[
	#include <stdio.h>
	#include <jpeglib.h>
	int main () { return (int)JCS_EXT_RGB + (int)JCS_EXT_RGBA; }
]] PYR_HAVE_JPEG_TURBO_EXT)
if(PYR_HAVE_JPEG_TURBO_EXT)
	check_cxx_source_compiles([[
		#include <stdio.h>
		#include <jpeglib.h>
		int main () {
			jpeg_decompress_struct cinfo{};
			J12SAMPARRAY rows12 = nullptr;
			J16SAMPARRAY rows16 = nullptr;
			if (sizeof( J12SAMPLE ) == 0 || sizeof( J16SAMPLE ) == 0)
				return 1;
			return (int)jpeg12_read_scanlines( &cinfo, rows12, 0 ) +
				(int)jpeg16_read_scanlines( &cinfo, rows16, 0 );
		}
	]] PYR_HAVE_JPEG_TURBO_3)
endif()
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT PYR_HAVE_JPEG_TURBO_EXT)
	unset(PYR_HAVE_JPEG_TURBO_EXT CACHE)
	message(FATAL_ERROR
		"pyr needs libjpeg-turbo >= 3.0: the JPEG found at ${JPEG_INCLUDE_DIRS} does "
		"not define the JCS_EXT_* colour spaces the SVS and TIFF loaders decode with.")
endif()
if(NOT PYR_HAVE_JPEG_TURBO_3)
	unset(PYR_HAVE_JPEG_TURBO_3 CACHE)
	message(FATAL_ERROR
		"pyr needs libjpeg-turbo >= 3.0: the libjpeg-turbo found at "
		"${JPEG_INCLUDE_DIRS} (${JPEG_LIBRARIES}) predates the 12/16-bit API "
		"(J12SAMPLE, jpeg12_read_scanlines, jpeg16_read_scanlines) the deep JPEG "
		"path decodes with; 2.x, which is what Ubuntu 24.04's libjpeg-turbo8-dev "
		"installs, is too old. Install 3.x and point CMake at it with "
		"-DJPEG_INCLUDE_DIR=<prefix>/include -DJPEG_LIBRARY=<prefix>/lib/libjpeg.so.")
endif()

set(PYR_SOURCES
	gallery.cpp
	img_codec.cpp
	img_tiff.cpp
	ptf_write.cpp
	pyr_rt.cpp
	wsi.cpp
	wsi_decode_pool.cpp
	wsi_buffers.cpp
	wsi_tif_aux.cpp
	wsi_ndpi.cpp
	wsi_svs.cpp
	wsi_glr.cpp
	wsi_tif.cpp)

set(PYR_HEADERS
	build_config.h
	gallery.h
	geom.h
	img_codec.h
	img_tiff.h
	ptf_tags.h
	ptf_write.h
	pwhl.h
	pyr.h
	pyr_barcode.h
	pyr_rt.h
	pyr_win.h
	tiff_inc.h
	wsi.h
	wsi_diag.h
	wsi_formats.h
	wsi_session.h
	wsi_session_registry.h
	wsi_tif_aux.h)

if(PYR_WITH_DCMTK)
	list(APPEND PYR_SOURCES wsi_dcm.cpp)
endif()
if(PYR_WITH_CZI)
	list(APPEND PYR_SOURCES wsi_czi.cpp)
endif()

add_library(pyra STATIC ${PYR_SOURCES} ${PYR_HEADERS})
add_library(Pyra::pyra ALIAS pyra)
target_compile_features(pyra PUBLIC cxx_std_20)
set(USE_OPENJPEG ${PYR_WITH_OPENJPEG})
set(USE_JXL ${PYR_WITH_JXL})
set(USE_DCMTK ${PYR_WITH_DCMTK})
set(USE_CZI ${PYR_WITH_CZI})
configure_file(
	build_config_generated.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/pyr_build_config_generated.h)

target_include_directories(pyra PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(pyra PUBLIC TIFF::TIFF JPEG::JPEG Threads::Threads)

target_compile_definitions(pyra PUBLIC PYR_USE_GENERATED_BUILD_CONFIG=1)

if(MSVC)
	target_compile_options(pyra PRIVATE /W4 /permissive- /utf-8)
	target_compile_definitions(pyra PUBLIC UNICODE _UNICODE NOMINMAX)
else()
	target_compile_options(pyra PRIVATE -Wall -Wextra)
endif()

if(PYR_WITH_OPENJPEG)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(OPENJPEG QUIET IMPORTED_TARGET libopenjp2)
	endif()
	if(OPENJPEG_FOUND)
		target_link_libraries(pyra PRIVATE PkgConfig::OPENJPEG)
	else()
		find_path(PYRA_OPENJPEG_INCLUDE_DIR openjpeg.h
			PATH_SUFFIXES openjpeg-2.5 openjpeg-2.4 openjpeg-2.3 REQUIRED)
		find_library(PYRA_OPENJPEG_LIBRARY NAMES openjp2 openjpeg REQUIRED)
		target_include_directories(pyra PRIVATE ${PYRA_OPENJPEG_INCLUDE_DIR})
		target_link_libraries(pyra PRIVATE ${PYRA_OPENJPEG_LIBRARY})
	endif()
endif()

if(PYR_WITH_JXL)
#
# libjxl >= 0.9, not any libjxl. The codec calls JxlEncoderDistanceFromQuality
# and JxlDecoderSetOutputColorProfile, both 0.9.0 (0.8 only declared the
# latter), and the 0.8 bit-depth API. 0.7, which Ubuntu 24.04's libjxl-dev
# installs, otherwise passes configure and fails in img_codec.cpp. The solution
# ships 0.11.0 (WSIA-SOUP).
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(JXL QUIET libjxl>=0.9)
	if(NOT JXL_FOUND)
		pkg_check_modules(PYR_JXL_ANY QUIET libjxl)
		if(PYR_JXL_ANY_FOUND)
			set(PYR_JXL_SEEN "found libjxl ${PYR_JXL_ANY_VERSION}")
		else()
			set(PYR_JXL_SEEN "found no libjxl")
		endif()
		message(FATAL_ERROR
			"PYR_WITH_JXL is ON and needs libjxl >= 0.9 (JxlEncoderDistanceFromQuality, "
			"JxlDecoderSetOutputColorProfile), but pkg-config ${PYR_JXL_SEEN}. Install "
			"0.9 or newer and point PKG_CONFIG_PATH at its lib/pkgconfig, or configure "
			"with -DPYR_WITH_JXL=OFF.")
	endif()
	target_include_directories(pyra PRIVATE ${JXL_INCLUDE_DIRS})
	target_link_libraries(pyra PRIVATE ${JXL_LIBRARIES})
endif()

include(CTest)
if(BUILD_TESTING)
	add_executable(pyra_standalone_test tests/standalone_test.cpp)
	target_link_libraries(pyra_standalone_test PRIVATE Pyra::pyra)
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_out")
	add_test(NAME pyra_standalone COMMAND pyra_standalone_test
		"${CMAKE_CURRENT_BINARY_DIR}/test_out")
endif()

if(PYR_WITH_DCMTK)
	find_package(DCMTK REQUIRED)
	target_link_libraries(pyra PRIVATE DCMTK::DCMTK)
endif()

if(PYR_WITH_CZI)
#
# libCZI is LGPL-3.0-or-later: link it dynamically. See build_config.h.
	find_library(LIBCZI_LIBRARY NAMES CZI libCZI REQUIRED)
	find_path(LIBCZI_INCLUDE_DIR NAMES libCZI.h REQUIRED)
	target_include_directories(pyra PRIVATE ${LIBCZI_INCLUDE_DIR})
	target_link_libraries(pyra PRIVATE ${LIBCZI_LIBRARY})
endif()
