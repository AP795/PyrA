# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# Standalone C++20 implementation and shared C reader interface for Pyra.
# Requires libtiff, libjpeg-turbo >= 3.0 and, by default, OpenJPEG 2.x.
# Qt is used only by the optional desktop viewer and its test support in ViewD.

cmake_minimum_required(VERSION 3.20)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
	message(FATAL_ERROR "Use an out-of-source build, for example: cmake -S . -B out/build/local")
endif()
project(Pyra VERSION 0.2.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/out/install" CACHE PATH "Install path prefix" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optional loaders and codecs. The core - TIFF-family loaders over baseline JPEG
# - needs only libtiff and libjpeg, so a bare configure builds something useful
# on any machine. Everything else is opt-in because it needs a library the host
# may not have.
option(PYR_WITH_OPENJPEG "JPEG 2000 tiles (Aperio 33003/33005, DICOM .90/.91)" ON)
option(PYR_WITH_OPENJPH  "HTJ2K tile and image codec (OpenJPH >= 0.33)"         OFF)
option(PYR_WITH_JXL      "JPEG XL tile and image codec"                        OFF)
option(PYR_WITH_DCMTK    "DICOM VL Whole Slide Microscopy loader"              OFF)
option(PYR_WITH_CZI      "Zeiss CZI loader (libCZI, LGPL - link dynamically)"  OFF)
option(PYR_WITH_JXR      "JPEG XR CZI sub-blocks (bundled jxrlib, BSD)"         ON)
option(PYR_WITH_ZSTD     "zstd CZI sub-blocks (bundled zstd decoder, BSD)"      ON)
option(PYRA_BUILD_GUI_TEST "Build the optional Qt Widgets viewer/evaluator" OFF)
option(PYRA_BUILD_WEB_TEST "Build the optional localhost browser viewer (no Qt)" OFF)
option(PYRA_BUILD_BRIDGE "Build pyra_process, the Python processing bridge (writes into DICOM slides with PYR_WITH_DCMTK)" ON)
set(PYRA_VIEWER_IMAGE_DIRECTORY "${PROJECT_SOURCE_DIR}/tst_in" CACHE PATH "Default image directory for the desktop and web viewers")
set(PYRA_TEST_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test_out" CACHE PATH "Directory for generated test artifacts")
set(PYRA_TIFF_EXTRA_LIBRARIES "" CACHE STRING "Additional libraries for a custom static TIFF build")
option(PYRA_WITH_OPENSLIDE "Build the optional dynamically linked OpenSlide C API backend" OFF)
option(PYRA_ENABLE_IPO "Enable interprocedural/link-time optimization in Release" ON)
option(PYRA_ENABLE_AVX2 "Require an AVX2-capable CPU for Release binaries" OFF)

include(cmake/PyraOptimizations.cmake)

set(PYRA_LINUX_SDK "" CACHE PATH "Install prefix produced by Deps/build.sh")
set(PYRA_WINDOWS_SDK "" CACHE PATH "Release SDK produced by Deps/build_windows.ps1")
# Emscripten: C++ exceptions carry every library error to its C status, and
# libjpeg's error exit longjmps; both use WebAssembly's native exception handling.
if(EMSCRIPTEN)
	add_compile_options(-fwasm-exceptions)
	add_link_options(-fwasm-exceptions)
# Shared memory for the decode pool: every object of the client, codecs included
# (Deps/), is built for threads.
	add_compile_options(-pthread)
	add_link_options(-pthread)
endif()
if(PYRA_LINUX_SDK)
	include(cmake/PyraLinuxSdk.cmake)
elseif(PYRA_WINDOWS_SDK)
	include(cmake/PyraWindowsSdk.cmake)
endif()

find_package(TIFF REQUIRED)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)
if(PYRA_JPEG_CONFIG_DIR)
	set_property(TARGET JPEG::JPEG APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${PYRA_JPEG_CONFIG_DIR}")
endif()
set_property(TARGET TIFF::TIFF APPEND PROPERTY INTERFACE_LINK_LIBRARIES JPEG::JPEG ${PYRA_TIFF_EXTRA_LIBRARIES})

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

# The DICOM writer and helpers compile in every build; without DCMTK their
# entry points report that DICOM is unavailable.
set(PYRA_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Core")
set(PYR_SOURCES
	dcm_color.cpp
	dcm_frames.cpp
	dcm_series.cpp
	dcm_write.cpp
	gallery.cpp
	gallery_scan.cpp
	img_codec.cpp
	img_czi.cpp
	img_htj2k.cpp
	img_tiff.cpp
	ipx_imd.cpp
	ipx_layout.cpp
	ipx_names.cpp
	ptf_write.cpp
	ptf_subset.cpp
	pyr_rt.cpp
	remote_pyramid.cpp
	synthetic.cpp
	wsi.cpp
	wsi_decode_pool.cpp
	wsi_buffers.cpp
	wsi_tif_aux.cpp
	wsi_ndpi.cpp
	wsi_svs.cpp
	wsi_glr.cpp
	wsi_render.cpp
	wsi_tif.cpp
	wsi_zyp.cpp)

set(PYR_HEADERS
	build_config.h
	dcm_color.h
	dcm_frames.h
	dcm_identity.h
	dcm_series.h
	dcm_write.h
	gallery.h
	gallery_scan.h
	geom.h
	img_codec.h
	img_czi.h
	img_htj2k.h
	img_htj2k_quantization.h
	img_tiff.h
	ipx_imd.h
	ipx_layout.h
	ipx_names.h
	ptf_tags.h
	ptf_write.h
	ptf_subset.h
	pwhl.h
	pyr.h
	pyr_barcode.h
	pyr_rt.h
	pyr_text.h
	pyr_win.h
	remote_pyramid.h
	synthetic.h
	tiff_inc.h
	wsi.h
	wsi_cache.h
	wsi_diag.h
	wsi_formats.h
	wsi_session.h
	wsi_session_registry.h
	wsi_tif_aux.h
	wsi_tile_pipeline.h)

if(PYR_WITH_DCMTK)
	list(APPEND PYR_SOURCES wsi_dcm.cpp)
endif()
if(PYR_WITH_CZI)
	list(APPEND PYR_SOURCES wsi_czi.cpp)
endif()

list(TRANSFORM PYR_SOURCES PREPEND "${PYRA_CORE_SOURCE_DIR}/")
list(TRANSFORM PYR_HEADERS PREPEND "${PYRA_CORE_SOURCE_DIR}/")
add_library(pyra_core STATIC ${PYR_SOURCES} ${PYR_HEADERS})
set_target_properties(pyra_core PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(pyra_core PUBLIC cxx_std_20)
set(USE_OPENJPEG ${PYR_WITH_OPENJPEG})
set(USE_OPENJPH ${PYR_WITH_OPENJPH})
set(USE_JXL ${PYR_WITH_JXL})
set(USE_DCMTK ${PYR_WITH_DCMTK})
set(USE_CZI ${PYR_WITH_CZI})
set(USE_JXR ${PYR_WITH_JXR})
set(USE_ZSTD ${PYR_WITH_ZSTD})
set(USE_OPENSLIDE ${PYRA_WITH_OPENSLIDE})
configure_file(
	Core/build_config_generated.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/pyr_build_config_generated.h)

target_include_directories(pyra_core PUBLIC
	"${PYRA_CORE_SOURCE_DIR}"
	${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(pyra_core PUBLIC TIFF::TIFF JPEG::JPEG Threads::Threads)

target_compile_definitions(pyra_core PUBLIC PYR_USE_GENERATED_BUILD_CONFIG=1)

if(MSVC)
	target_compile_options(pyra_core PRIVATE /W4 /permissive- /utf-8)
	target_compile_definitions(pyra_core PUBLIC UNICODE _UNICODE NOMINMAX)
else()
	target_compile_options(pyra_core PRIVATE -Wall -Wextra)
endif()

if(PYR_WITH_OPENJPEG)
	if(NOT (PYRA_OPENJPEG_INCLUDE_DIR AND PYRA_OPENJPEG_LIBRARY))
		find_package(PkgConfig QUIET)
		if(PkgConfig_FOUND)
			pkg_check_modules(OPENJPEG QUIET IMPORTED_TARGET libopenjp2)
		endif()
	endif()
	if(TARGET PkgConfig::OPENJPEG AND NOT (PYRA_OPENJPEG_INCLUDE_DIR AND PYRA_OPENJPEG_LIBRARY))
		target_link_libraries(pyra_core PRIVATE PkgConfig::OPENJPEG)
	else()
		find_path(PYRA_OPENJPEG_INCLUDE_DIR openjpeg.h
			PATH_SUFFIXES openjpeg-2.5 openjpeg-2.4 openjpeg-2.3 REQUIRED)
		find_library(PYRA_OPENJPEG_LIBRARY NAMES openjp2 openjpeg REQUIRED)
		target_include_directories(pyra_core PRIVATE ${PYRA_OPENJPEG_INCLUDE_DIR})
		target_link_libraries(pyra_core PRIVATE ${PYRA_OPENJPEG_LIBRARY})
	endif()
endif()

if(PYR_WITH_OPENJPH)
	find_package(openjph 0.33 CONFIG REQUIRED)
	target_link_libraries(pyra_core PRIVATE openjph)
endif()

if(PYR_WITH_JXL)
	include(cmake/PyraJxl.cmake)
endif()

add_library(pyra SHARED Core/pyra_c.cpp Core/pyra_api_internal.h Core/pyra_openslide.cpp
	Core/pyra_openslide.h Core/include/pyra/pyra.h)
add_library(Pyra::pyra ALIAS pyra)
target_compile_features(pyra PRIVATE cxx_std_20)
target_compile_definitions(pyra PRIVATE PYRA_BUILDING_LIBRARY=1)
target_include_directories(pyra PUBLIC
	$<BUILD_INTERFACE:${PYRA_CORE_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:include>)
target_link_libraries(pyra PRIVATE pyra_core)
set_target_properties(pyra PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 4
	CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
	target_link_options(pyra PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/pyra.exports")
	set_property(TARGET pyra APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pyra.exports)
endif()
if(MSVC)
	target_compile_options(pyra PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(pyra PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(PYRA_WITH_OPENSLIDE)
	include(cmake/PyraOpenSlide.cmake)
endif()
install(TARGETS pyra EXPORT PyraTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES Core/include/pyra/pyra.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pyra)
install(FILES license.md DESTINATION ${CMAKE_INSTALL_DATADIR}/Pyra)
set(pyra_notice_destination "${CMAKE_INSTALL_DATADIR}/Pyra/third-party")
# The CZI sub-block decoders are compiled from Deps/ in every build that enables them.
include(cmake/PyraDependencyNotices.cmake)
set(pyra_codec_notices)
if(PYR_WITH_JXR)
	list(APPEND pyra_codec_notices jxrlib)
endif()
if(PYR_WITH_ZSTD)
	list(APPEND pyra_codec_notices zstd)
endif()
if(pyra_codec_notices)
	pyra_install_dependency_notices("${pyra_notice_destination}" ${pyra_codec_notices})
endif()
if(PYRA_WINDOWS_SDK OR PYRA_LINUX_SDK)
	include(cmake/PyraDependencyNotices.cmake)
	set(pyra_bundled_notices libjpeg-turbo libtiff)
	if(PYR_WITH_OPENJPEG)
		list(APPEND pyra_bundled_notices openjpeg)
	endif()
	if(PYRA_WINDOWS_SDK OR EXISTS "${PYRA_LINUX_SDK}/lib/libz.a")
		list(APPEND pyra_bundled_notices zlib)
	endif()
	if(PYR_WITH_OPENJPH)
		list(APPEND pyra_bundled_notices openjph)
	endif()
	if(PYR_WITH_DCMTK)
		list(APPEND pyra_bundled_notices dcmtk)
	endif()
	pyra_install_dependency_notices("${pyra_notice_destination}" ${pyra_bundled_notices})
endif()
# An Emscripten build makes only the browser client: there is no shared library to export.
if(NOT EMSCRIPTEN)
install(EXPORT PyraTargets NAMESPACE Pyra:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pyra)
configure_package_config_file(cmake/PyraConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/PyraConfig.cmake
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pyra)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/PyraConfigVersion.cmake
	VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PyraConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/PyraConfigVersion.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pyra)
endif()

include(CTest)
if(BUILD_TESTING)
	add_executable(pyra_c_api_sample Tests/c_api_sample.cpp)
	target_link_libraries(pyra_c_api_sample PRIVATE Pyra::pyra JPEG::JPEG)
	if(MSVC)
		target_compile_options(pyra_c_api_sample PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(pyra_c_api_sample PRIVATE -Wall -Wextra)
	endif()
	add_executable(pyra_decode_pool_test Tests/decode_pool_test.cpp)
	target_link_libraries(pyra_decode_pool_test PRIVATE pyra_core)
	add_test(NAME pyra_decode_pool COMMAND pyra_decode_pool_test)
	set_tests_properties(pyra_decode_pool PROPERTIES TIMEOUT 20)
	add_executable(pyra_standalone_test Tests/standalone_test.cpp Tests/wsi_buffers_test.cpp Tests/ptf_subset_test.cpp)
	target_link_libraries(pyra_standalone_test PRIVATE pyra_core)
	file(MAKE_DIRECTORY "${PYRA_TEST_OUTPUT_DIRECTORY}")
	add_test(NAME pyra_standalone COMMAND pyra_standalone_test
		"${PYRA_TEST_OUTPUT_DIRECTORY}")
	set_tests_properties(pyra_standalone PROPERTIES FIXTURES_SETUP pyra_wsi_fixture)
# Real-slide stage profile of the native render path; not a CTest.
	add_executable(pyra_native_profile Tests/native_render_profile.cpp)
	target_link_libraries(pyra_native_profile PRIVATE pyra_core)
	add_executable(pyra_c_api_test Tests/c_api_test.cpp)
	add_executable(pyra_zyp_test Tests/wsi_zyp_test.cpp)
	target_link_libraries(pyra_zyp_test PRIVATE Pyra::pyra pyra_core)
	add_test(NAME pyra_zyp COMMAND pyra_zyp_test "${PYRA_TEST_OUTPUT_DIRECTORY}/zyp")
	set_tests_properties(pyra_zyp PROPERTIES FIXTURES_SETUP pyra_zyp_fixture)
	target_link_libraries(pyra_c_api_test PRIVATE Pyra::pyra Threads::Threads)
	target_compile_definitions(pyra_c_api_test PRIVATE PYRA_TEST_WITH_OPENJPEG=$<BOOL:${PYR_WITH_OPENJPEG}>)
	add_test(NAME pyra_c_api COMMAND pyra_c_api_test "${PYRA_TEST_OUTPUT_DIRECTORY}")
	set_tests_properties(pyra_c_api PROPERTIES FIXTURES_REQUIRED pyra_wsi_fixture)
	add_executable(pyra_remote_test Tests/remote_test.cpp)
	target_link_libraries(pyra_remote_test PRIVATE Pyra::pyra pyra_core)
	add_test(NAME pyra_remote COMMAND pyra_remote_test "${PYRA_TEST_OUTPUT_DIRECTORY}/remote")
	set_tests_properties(pyra_remote PROPERTIES TIMEOUT 120)
	add_executable(pyra_make_gallery Tests/make_gallery.cpp)
	target_link_libraries(pyra_make_gallery PRIVATE Pyra::pyra)
	add_executable(pyra_make_synthetic Tests/make_synthetic.cpp)
	target_link_libraries(pyra_make_synthetic PRIVATE Pyra::pyra)
	add_executable(pyra_synthetic_test Tests/synthetic_test.cpp)
	target_link_libraries(pyra_synthetic_test PRIVATE Pyra::pyra)
	add_test(NAME pyra_synthetic COMMAND pyra_synthetic_test "${PYRA_TEST_OUTPUT_DIRECTORY}/synthetic")
	set_tests_properties(pyra_synthetic PROPERTIES TIMEOUT 120)
	add_executable(pyra_c_header_test Tests/c_header_test.c)
	target_link_libraries(pyra_c_header_test PRIVATE Pyra::pyra)
	add_test(NAME pyra_c_header COMMAND pyra_c_header_test)
	add_executable(pyra_jpeg_colour_test Tests/jpeg_colour_test.cpp)
	add_executable(pyra_wsi_gallery_test Tests/wsi_gallery_test.cpp)
	target_link_libraries(pyra_wsi_gallery_test PRIVATE Pyra::pyra pyra_core TIFF::TIFF)
	add_test(NAME pyra_wsi_gallery COMMAND pyra_wsi_gallery_test "${PYRA_TEST_OUTPUT_DIRECTORY}/images")
	set_tests_properties(pyra_wsi_gallery PROPERTIES FIXTURES_SETUP pyra_image_fixture TIMEOUT 60)
	target_include_directories(pyra_jpeg_colour_test PRIVATE "${PYRA_CORE_SOURCE_DIR}")
	target_link_libraries(pyra_jpeg_colour_test PRIVATE Pyra::pyra TIFF::TIFF)
	add_test(NAME pyra_jpeg_colour COMMAND pyra_jpeg_colour_test "${PYRA_TEST_OUTPUT_DIRECTORY}/jpeg_colour")
	if(PYRA_WITH_OPENSLIDE)
		add_executable(pyra_openslide_test Tests/openslide_test.cpp)
		target_include_directories(pyra_openslide_test PRIVATE "${PYRA_CORE_SOURCE_DIR}")
		target_link_libraries(pyra_openslide_test PRIVATE Pyra::pyra TIFF::TIFF)
		add_test(NAME pyra_openslide COMMAND pyra_openslide_test "${PYRA_TEST_OUTPUT_DIRECTORY}/openslide")
		set_tests_properties(pyra_openslide PROPERTIES FIXTURES_SETUP pyra_openslide_fixture)
# Real-slide render benchmark (native vs OpenSlide backend vs bare OpenSlide); not a CTest.
		add_executable(pyra_openslide_bench Tests/openslide_bench.cpp)
		target_link_libraries(pyra_openslide_bench PRIVATE Pyra::pyra ${PYRA_OPENSLIDE_TARGET})
	endif()
endif()

if(PYR_WITH_DCMTK)
	find_package(DCMTK REQUIRED)
# DCMTK 3.7.0 configured with DCMTK_ENABLE_CHARSET_CONVERSION=OFF builds no
# oficonv library, yet its exported ofstd target still names it. Drop the name
# when no such library or target exists, so the link matches the build.
	if(TARGET DCMTK::ofstd AND NOT TARGET oficonv AND NOT TARGET DCMTK::oficonv)
		find_library(PYRA_DCMTK_OFICONV oficonv HINTS "${DCMTK_DIR}/lib"
			PATH_SUFFIXES Release lib NO_DEFAULT_PATH)
		if(NOT PYRA_DCMTK_OFICONV)
			get_target_property(pyra_ofstd_links DCMTK::ofstd INTERFACE_LINK_LIBRARIES)
			if(pyra_ofstd_links)
				list(REMOVE_ITEM pyra_ofstd_links oficonv)
				set_target_properties(DCMTK::ofstd PROPERTIES
					INTERFACE_LINK_LIBRARIES "${pyra_ofstd_links}")
			endif()
		endif()
	endif()
	target_link_libraries(pyra_core PRIVATE DCMTK::DCMTK)
endif()

# CZI sub-block payload decoders, built from the bundled sources (img_czi.cpp).
if(PYR_WITH_ZSTD)
	file(GLOB PYRA_ZSTD_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/Deps/zstd/lib/common/*.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/Deps/zstd/lib/decompress/*.c")
	add_library(pyra_zstd STATIC ${PYRA_ZSTD_SOURCES})
	target_include_directories(pyra_zstd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Deps/zstd/lib")
	target_compile_definitions(pyra_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)
	set_target_properties(pyra_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
	target_link_libraries(pyra_core PRIVATE pyra_zstd)
endif()
if(PYR_WITH_JXR)
	set(PYRA_JXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Deps/jxrlib")
	file(GLOB PYRA_JXR_SOURCES CONFIGURE_DEPENDS
		"${PYRA_JXR_DIR}/common/src/*.c"
		"${PYRA_JXR_DIR}/image/decode/*.c"
		"${PYRA_JXR_DIR}/image/encode/*.c"
		"${PYRA_JXR_DIR}/image/sys/*.c"
		"${PYRA_JXR_DIR}/jxrgluelib/*.c")
	add_library(pyra_jxr STATIC ${PYRA_JXR_SOURCES})
# jxrlib ships its own guiddef.h, which must not shadow the Windows SDK's in
# other translation units: only the jxrlib sources and img_czi.cpp see its headers.
	set(PYRA_JXR_INCLUDES "${PYRA_JXR_DIR}/jxrgluelib" "${PYRA_JXR_DIR}/image/sys" "${PYRA_JXR_DIR}/common/include")
	target_include_directories(pyra_jxr PRIVATE ${PYRA_JXR_INCLUDES} "${PYRA_JXR_DIR}/pyra")
	set_source_files_properties("${PYRA_CORE_SOURCE_DIR}/img_czi.cpp" TARGET_DIRECTORY pyra_core
		PROPERTIES INCLUDE_DIRECTORIES "${PYRA_JXR_INCLUDES}")
	set_target_properties(pyra_jxr PROPERTIES POSITION_INDEPENDENT_CODE ON)
	if(MSVC)
		target_compile_options(pyra_jxr PRIVATE /W0)
	else()
		target_compile_options(pyra_jxr PRIVATE -w)
	endif()
	target_link_libraries(pyra_core PRIVATE pyra_jxr)
endif()

if(PYR_WITH_CZI)
#
# libCZI is LGPL-3.0-or-later: link it dynamically. See build_config.h.
	find_library(LIBCZI_LIBRARY NAMES CZI libCZI REQUIRED)
	find_path(LIBCZI_INCLUDE_DIR NAMES libCZI.h REQUIRED)
	target_include_directories(pyra_core PRIVATE ${LIBCZI_INCLUDE_DIR})
	target_link_libraries(pyra_core PRIVATE ${LIBCZI_LIBRARY})
# PyrA's own sub-block decoders against libCZI's, which only this build has.
	if(BUILD_TESTING AND PYR_WITH_JXR AND PYR_WITH_ZSTD)
		add_executable(pyra_czi_decode_test Tests/czi_decode_test.cpp)
		target_include_directories(pyra_czi_decode_test PRIVATE ${LIBCZI_INCLUDE_DIR})
		target_link_libraries(pyra_czi_decode_test PRIVATE pyra_core ${LIBCZI_LIBRARY})
		add_test(NAME pyra_czi_decode COMMAND pyra_czi_decode_test)
		get_filename_component(PYRA_LIBCZI_BIN "${LIBCZI_LIBRARY}" DIRECTORY)
		set_tests_properties(pyra_czi_decode PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PYRA_LIBCZI_BIN}")
	endif()
endif()

# The bridge first: the web viewer's Analyse links it when it exists.
if(PYRA_BUILD_BRIDGE)
	add_subdirectory(Core/bridge)
endif()
if(PYRA_BUILD_GUI_TEST)
	add_subdirectory(ViewD)
endif()
if(PYRA_BUILD_WEB_TEST)
	add_subdirectory(ViewW)
endif()
# The web viewer's browser client, built by an Emscripten configure of this tree.
if(EMSCRIPTEN)
	add_subdirectory(ViewW/wasm)
endif()
