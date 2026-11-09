# JPEG XL discovery. Windows SDK archives must match the consumer's CRT.
if(WIN32)
	find_path(PYRA_JXL_INCLUDE_DIR jxl/decode.h REQUIRED)
	set(PYRA_JXL_LIBRARY_DIR "" CACHE PATH "JPEG XL static SDK library directory matching the configured MSVC runtime")
	set(pyra_jxl_libraries)
	foreach(component jxl jxl_cms brotlicommon brotlidec brotlienc hwy)
		find_library(PYRA_JXL_${component}_LIBRARY NAMES ${component}
			HINTS "${PYRA_JXL_LIBRARY_DIR}" REQUIRED)
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
