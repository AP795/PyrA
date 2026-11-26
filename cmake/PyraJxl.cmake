# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# JPEG XL discovery from a static SDK. The bundled SDKs build Deps/libjxl:
# Deps/build_windows.ps1 (MSVC, archives matching the consumer's CRT) and the
# Linux/Emscripten adapter Deps/CMakeLists.txt; PyraWindowsSdk.cmake and
# PyraLinuxSdk.cmake point pyra_jxl_sdk_include/lib at them. PYRA_JXL_INCLUDE_DIR
# and PYRA_JXL_LIBRARY_DIR select another SDK. Emscripten's sysroot does not
# contain the SDK, so its paths are searched as given, not re-rooted. Linux
# without a bundled SDK uses the system libjxl through pkg-config.
if(WIN32 OR EMSCRIPTEN OR PYRA_LINUX_SDK)
	find_path(PYRA_JXL_INCLUDE_DIR jxl/decode.h HINTS "${pyra_jxl_sdk_include}" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
	set(PYRA_JXL_LIBRARY_DIR "" CACHE PATH "JPEG XL static SDK library directory (MSVC: matching the configured runtime)")
	set(pyra_jxl_libraries)
#	Dependency order, for linkers that resolve static archives in one pass (GNU ld).
	foreach(component jxl jxl_cms brotlidec brotlienc brotlicommon hwy)
		find_library(PYRA_JXL_${component}_LIBRARY NAMES ${component}
			HINTS "${PYRA_JXL_LIBRARY_DIR}" "${pyra_jxl_sdk_lib}" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
		list(APPEND pyra_jxl_libraries "${PYRA_JXL_${component}_LIBRARY}")
	endforeach()
	include(CMakePushCheckState)
	cmake_push_check_state(RESET)
	set(CMAKE_REQUIRED_INCLUDES "${PYRA_JXL_INCLUDE_DIR}")
	set(CMAKE_REQUIRED_LIBRARIES ${pyra_jxl_libraries})
	set(CMAKE_REQUIRED_DEFINITIONS -DJXL_STATIC_DEFINE -DJXL_CMS_STATIC_DEFINE)
	check_cxx_source_compiles([[
		#include <jxl/decode.h>
		#include <jxl/encode.h>
		#include <jxl/version.h>
		#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0,9,0)
		#error JPEG XL 0.9 or newer is required
		#endif
		int main () {
			auto volatile encoder = &JxlEncoderDistanceFromQuality;
			auto volatile decoder = &JxlDecoderSetOutputColorProfile;
			return encoder == nullptr || decoder == nullptr;
		}
	]] PYRA_HAVE_JXL_API)
	cmake_pop_check_state()
	if(NOT PYRA_HAVE_JXL_API)
		unset(PYRA_HAVE_JXL_API CACHE)
		message(FATAL_ERROR "JPEG XL >= 0.9 headers and compatible static SDK libraries are required; check PYRA_JXL_INCLUDE_DIR and PYRA_JXL_LIBRARY_DIR.")
	endif()
	target_compile_definitions(pyra_core PRIVATE JXL_STATIC_DEFINE JXL_CMS_STATIC_DEFINE)
	target_include_directories(pyra_core PRIVATE "${PYRA_JXL_INCLUDE_DIR}")
	target_link_libraries(pyra_core PRIVATE ${pyra_jxl_libraries})
else()
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(JXL REQUIRED IMPORTED_TARGET libjxl>=0.9)
	target_link_libraries(pyra_core PRIVATE PkgConfig::JXL)
endif()
