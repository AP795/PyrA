# Standalone C++20 implementation and shared C reader interface for Pyra.
# Requires libtiff, libjpeg-turbo >= 3.0 and, by default, OpenJPEG 2.x.
# Qt is used only by the optional viewer and its test support under tests/gui.

cmake_minimum_required(VERSION 3.20)
project(Pyra VERSION 0.1.0 LANGUAGES C CXX)

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
option(PYRA_BUILD_GUI_TEST "Build the optional Qt Widgets viewer/evaluator" OFF)
option(PYRA_ENABLE_IPO "Enable interprocedural/link-time optimization in Release" ON)
option(PYRA_ENABLE_AVX2 "Require an AVX2-capable CPU for Release binaries" OFF)

include(cmake/PyraOptimizations.cmake)

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

add_library(pyra_core STATIC ${PYR_SOURCES} ${PYR_HEADERS})
set_target_properties(pyra_core PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(pyra_core PUBLIC cxx_std_20)
set(USE_OPENJPEG ${PYR_WITH_OPENJPEG})
set(USE_JXL ${PYR_WITH_JXL})
set(USE_DCMTK ${PYR_WITH_DCMTK})
set(USE_CZI ${PYR_WITH_CZI})
configure_file(
	build_config_generated.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/pyr_build_config_generated.h)

target_include_directories(pyra_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
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
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(OPENJPEG QUIET IMPORTED_TARGET libopenjp2)
	endif()
	if(OPENJPEG_FOUND)
		target_link_libraries(pyra_core PRIVATE PkgConfig::OPENJPEG)
	else()
		find_path(PYRA_OPENJPEG_INCLUDE_DIR openjpeg.h
			PATH_SUFFIXES openjpeg-2.5 openjpeg-2.4 openjpeg-2.3 REQUIRED)
		find_library(PYRA_OPENJPEG_LIBRARY NAMES openjp2 openjpeg REQUIRED)
		target_include_directories(pyra_core PRIVATE ${PYRA_OPENJPEG_INCLUDE_DIR})
		target_link_libraries(pyra_core PRIVATE ${PYRA_OPENJPEG_LIBRARY})
	endif()
endif()

if(PYR_WITH_JXL)
	include(cmake/PyraJxl.cmake)
endif()

add_library(pyra SHARED pyra_c.cpp include/pyra/pyra.h)
add_library(Pyra::pyra ALIAS pyra)
target_compile_features(pyra PRIVATE cxx_std_20)
target_compile_definitions(pyra PRIVATE PYRA_BUILDING_LIBRARY=1)
target_include_directories(pyra PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:include>)
target_link_libraries(pyra PRIVATE pyra_core)
set_target_properties(pyra PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1
	CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(UNIX AND NOT APPLE)
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
install(TARGETS pyra EXPORT PyraTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/pyra/pyra.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pyra)
install(FILES license.md DESTINATION ${CMAKE_INSTALL_DATADIR}/Pyra)
install(EXPORT PyraTargets NAMESPACE Pyra:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pyra)
configure_package_config_file(cmake/PyraConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/PyraConfig.cmake
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pyra)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/PyraConfigVersion.cmake
	VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PyraConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/PyraConfigVersion.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pyra)

include(CTest)
if(BUILD_TESTING)
	add_executable(pyra_decode_pool_test tests/decode_pool_test.cpp)
	target_link_libraries(pyra_decode_pool_test PRIVATE pyra_core)
	add_test(NAME pyra_decode_pool COMMAND pyra_decode_pool_test)
	set_tests_properties(pyra_decode_pool PROPERTIES TIMEOUT 20)
	add_executable(pyra_standalone_test tests/standalone_test.cpp)
	target_link_libraries(pyra_standalone_test PRIVATE pyra_core)
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_out")
	add_test(NAME pyra_standalone COMMAND pyra_standalone_test
		"${CMAKE_CURRENT_BINARY_DIR}/test_out")
	set_tests_properties(pyra_standalone PROPERTIES FIXTURES_SETUP pyra_slide_fixture)
	add_executable(pyra_c_api_test tests/c_api_test.cpp)
	target_link_libraries(pyra_c_api_test PRIVATE Pyra::pyra Threads::Threads)
	target_compile_definitions(pyra_c_api_test PRIVATE PYRA_TEST_WITH_OPENJPEG=$<BOOL:${PYR_WITH_OPENJPEG}>)
	add_test(NAME pyra_c_api COMMAND pyra_c_api_test "${CMAKE_CURRENT_BINARY_DIR}/test_out")
	set_tests_properties(pyra_c_api PROPERTIES FIXTURES_REQUIRED pyra_slide_fixture)
	add_executable(pyra_c_header_test tests/c_header_test.c)
	target_link_libraries(pyra_c_header_test PRIVATE Pyra::pyra)
	add_test(NAME pyra_c_header COMMAND pyra_c_header_test)
endif()

if(PYR_WITH_DCMTK)
	find_package(DCMTK REQUIRED)
	target_link_libraries(pyra_core PRIVATE DCMTK::DCMTK)
endif()

if(PYR_WITH_CZI)
#
# libCZI is LGPL-3.0-or-later: link it dynamically. See build_config.h.
	find_library(LIBCZI_LIBRARY NAMES CZI libCZI REQUIRED)
	find_path(LIBCZI_INCLUDE_DIR NAMES libCZI.h REQUIRED)
	target_include_directories(pyra_core PRIVATE ${LIBCZI_INCLUDE_DIR})
	target_link_libraries(pyra_core PRIVATE ${LIBCZI_LIBRARY})
endif()

if(PYRA_BUILD_GUI_TEST)
	add_subdirectory(tests/gui)
endif()
