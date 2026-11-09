# Directory-scoped settings cover Pyra and its tests without becoming public
# usage requirements of the exported shared-library target.
if(PYRA_ENABLE_IPO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT pyra_ipo_supported OUTPUT pyra_ipo_error LANGUAGES C CXX)
	if(NOT pyra_ipo_supported)
		message(FATAL_ERROR
			"Release IPO/LTO is unavailable: ${pyra_ipo_error}\n"
			"Configure with -DPYRA_ENABLE_IPO=OFF to disable it.")
	endif()
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ${PYRA_ENABLE_IPO})

if(PYRA_ENABLE_AVX2)
	if(MSVC)
		set(pyra_avx2_flag /arch:AVX2)
	elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
		set(pyra_avx2_flag -mavx2)
	else()
		message(FATAL_ERROR "PYRA_ENABLE_AVX2 requires an x86/x64 MSVC, GCC, or Clang compiler.")
	endif()
	include(CheckCXXSourceCompiles)
	include(CMakePushCheckState)
	cmake_push_check_state(RESET)
	set(CMAKE_REQUIRED_FLAGS "${pyra_avx2_flag}")
	check_cxx_source_compiles([[
		#ifndef __AVX2__
		#error AVX2 code generation is unavailable for this compiler/target
		#endif
		int main () { return 0; }
	]] PYRA_COMPILER_SUPPORTS_AVX2)
	cmake_pop_check_state()
	if(NOT PYRA_COMPILER_SUPPORTS_AVX2)
		message(FATAL_ERROR
			"The compiler/target does not support ${pyra_avx2_flag}; "
			"configure with -DPYRA_ENABLE_AVX2=OFF.")
	endif()
	add_compile_options("$<$<CONFIG:Release>:${pyra_avx2_flag}>")
endif()
