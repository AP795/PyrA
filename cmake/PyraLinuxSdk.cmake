# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# Emscripten uses the same SDK layout, built by an emcmake configure of Deps/.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
	message(FATAL_ERROR "PYRA_LINUX_SDK is only supported on Linux and Emscripten")
endif()

set(pyra_sdk_include "${PYRA_LINUX_SDK}/include")
set(pyra_sdk_lib "${PYRA_LINUX_SDK}/lib")
foreach(file include/jpeglib.h include/tiffio.h include/openjpeg.h
	lib/libjpeg.a lib/libtiff.a lib/libopenjp2.a)
	if(NOT EXISTS "${PYRA_LINUX_SDK}/${file}")
		message(FATAL_ERROR
			"Missing ${file} in ${PYRA_LINUX_SDK}. Build the SDK with Deps/build.sh.")
	endif()
endforeach()

set(JPEG_INCLUDE_DIR "${pyra_sdk_include}")
set(JPEG_LIBRARY "${pyra_sdk_lib}/libjpeg.a")
set(TIFF_INCLUDE_DIR "${pyra_sdk_include}")
set(TIFF_LIBRARY "${pyra_sdk_lib}/libtiff.a")
set(PYRA_OPENJPEG_INCLUDE_DIR "${pyra_sdk_include}")
set(PYRA_OPENJPEG_LIBRARY "${pyra_sdk_lib}/libopenjp2.a")
set(openjph_DIR "${pyra_sdk_lib}/cmake/openjph")
set(DCMTK_DIR "${pyra_sdk_lib}/cmake/dcmtk")
set(pyra_jxl_sdk_include "${pyra_sdk_include}")
set(pyra_jxl_sdk_lib "${pyra_sdk_lib}")
if(EXISTS "${pyra_sdk_lib}/libz.a")
	set(ZLIB_INCLUDE_DIR "${pyra_sdk_include}")
	set(ZLIB_LIBRARY "${pyra_sdk_lib}/libz.a")
	set(PYRA_TIFF_EXTRA_LIBRARIES "${ZLIB_LIBRARY}")
endif()
