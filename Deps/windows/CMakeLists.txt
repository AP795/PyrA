# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(PyraBtf LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "The BTF64 SDK requires x64 MSVC.")
endif()
set(PYRA_BTF_RUNTIME "md" CACHE STRING "MSVC runtime for the BTF64 SDK: md or mt")
set_property(CACHE PYRA_BTF_RUNTIME PROPERTY STRINGS md mt)
if(PYRA_BTF_RUNTIME STREQUAL "md")
	set(runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
elseif(PYRA_BTF_RUNTIME STREQUAL "mt")
	set(runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
	message(FATAL_ERROR "PYRA_BTF_RUNTIME must be md or mt.")
endif()

set(deps "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(jpeg "${deps}/jpegturbo3_1_3")
set(tiff "${deps}/libtiff47")
set(openjpeg "${deps}/openjp25")
set(zlib "${deps}/zlib132")

set(jpeg_sources
	jcapimin.c jcarith.c jchuff.c jcicc.c jcinit.c jclhuff.c jcmarker.c jcmaster.c
	jcomapi.c jcparam.c jcphuff.c jctrans.c jdapimin.c jdarith.c jdatadst.c
	jdatasrc.c jdhuff.c jdicc.c jdinput.c jdlhuff.c jdmarker.c jdmaster.c jdphuff.c
	jdtrans.c jerror.c jfdctflt.c jmemmgr.c jmemnobs.c jpeg_nbits.c jaricom.c)
list(TRANSFORM jpeg_sources PREPEND "${jpeg}/src/")
file(GLOB jpeg_wrappers CONFIGURE_DEPENDS "${jpeg}/src/wrapper/*.c")

set(simd "${jpeg}/simd/x86_64")
find_program(PYRA_NASM NAMES nasm HINTS "$ENV{ProgramFiles}/NASM" REQUIRED)
file(GLOB simd_includes CONFIGURE_DEPENDS "${jpeg}/simd/nasm/*.inc" "${simd}/*.inc")
set(simd_sources
	jsimdcpu jfdctflt-sse jccolor-sse2 jcgray-sse2 jchuff-sse2 jcphuff-sse2
	jcsample-sse2 jdcolor-sse2 jdmerge-sse2 jdsample-sse2 jfdctfst-sse2 jfdctint-sse2
	jidctflt-sse2 jidctfst-sse2 jidctint-sse2 jidctred-sse2 jquantf-sse2 jquanti-sse2
	jccolor-avx2 jcgray-avx2 jcsample-avx2 jdcolor-avx2 jdmerge-avx2 jdsample-avx2
	jfdctint-avx2 jidctint-avx2 jquanti-avx2)
set(simd_objects)
foreach(unit IN LISTS simd_sources)
	set(source "${simd}/${unit}.asm")
	set(object "${CMAKE_CURRENT_BINARY_DIR}/simd/${unit}.obj")
	add_custom_command(OUTPUT "${object}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/simd"
		COMMAND "${PYRA_NASM}" -fwin64 -DWIN64 -D__x86_64__
			"-I${jpeg}/simd/nasm/" "-I${simd}/" -o "${object}" "${source}"
		DEPENDS "${source}" ${simd_includes}
		VERBATIM)
	list(APPEND simd_objects "${object}")
endforeach()
set_source_files_properties(${simd_objects} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

file(GLOB tiff_sources CONFIGURE_DEPENDS "${tiff}/tif_*.c")
list(FILTER tiff_sources EXCLUDE REGEX "/tif_unix\\.c$")
list(APPEND tiff_sources "${tiff}/tif_stream.cxx")
file(GLOB zlib_sources CONFIGURE_DEPENDS "${zlib}/*.c")
set(openjpeg_sources
	bio.c cio.c dwt.c event.c function_list.c ht_dec.c image.c invert.c j2k.c
	jp2.c mct.c mqc.c openjpeg.c opj_clock.c opj_malloc.c pi.c sparse_array.c
	t1.c t2.c tcd.c tgt.c thread.c)
list(TRANSFORM openjpeg_sources PREPEND "${openjpeg}/")

add_library(BTF64 STATIC
	${jpeg_sources} ${jpeg_wrappers} "${simd}/jsimd.c" ${simd_objects}
	${tiff_sources} ${openjpeg_sources} ${zlib_sources})
set_property(TARGET BTF64 PROPERTY MSVC_RUNTIME_LIBRARY "${runtime}")
target_compile_features(BTF64 PRIVATE cxx_std_20)
target_compile_definitions(BTF64 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_include_directories(BTF64 PRIVATE
	"${jpeg}" "${jpeg}/src" "${jpeg}/simd" "${tiff}" "${openjpeg}" "${zlib}")

install(TARGETS BTF64 ARCHIVE DESTINATION "lib/$<CONFIG>")
include(../../cmake/PyraDependencyNotices.cmake)
pyra_install_dependency_notices(share/pyra-dependencies libjpeg-turbo libtiff openjpeg zlib)
