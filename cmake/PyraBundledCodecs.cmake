# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# The reader always needs the codec adapters, including their disabled stubs.
function(pyra_add_bundled_tile_codecs reader_target)
	set(pyra_source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
	set(jxr_adapter "${pyra_source_dir}/Core/img_jxr.cpp")
	target_sources(${reader_target} PRIVATE
		"${pyra_source_dir}/Core/img_czi.cpp" "${jxr_adapter}")

	if(PYR_WITH_ZSTD)
		set(zstd_dir "${pyra_source_dir}/Deps/zstd/lib")
		file(GLOB zstd_sources CONFIGURE_DEPENDS
			"${zstd_dir}/common/*.c" "${zstd_dir}/decompress/*.c")
		add_library(pyra_zstd STATIC ${zstd_sources})
		target_include_directories(pyra_zstd PUBLIC "${zstd_dir}")
		target_compile_definitions(pyra_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)
		set_target_properties(pyra_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
		target_link_libraries(${reader_target} PRIVATE pyra_zstd)
	endif()

	if(PYR_WITH_JXR)
		set(jxr_dir "${pyra_source_dir}/Deps/jxrlib")
		file(GLOB jxr_sources CONFIGURE_DEPENDS
			"${jxr_dir}/common/src/*.c" "${jxr_dir}/image/decode/*.c"
			"${jxr_dir}/image/encode/*.c" "${jxr_dir}/image/sys/*.c"
			"${jxr_dir}/jxrgluelib/*.c")
		add_library(pyra_jxr STATIC ${jxr_sources})
# jxrlib ships its own guiddef.h. Only its sources and img_jxr.cpp see it.
		set(jxr_includes "${jxr_dir}/jxrgluelib" "${jxr_dir}/image/sys" "${jxr_dir}/common/include")
		target_include_directories(pyra_jxr PRIVATE ${jxr_includes} "${jxr_dir}/pyra")
		set_source_files_properties("${jxr_adapter}" TARGET_DIRECTORY ${reader_target}
			PROPERTIES INCLUDE_DIRECTORIES "${jxr_includes}")
		set_target_properties(pyra_jxr PROPERTIES POSITION_INDEPENDENT_CODE ON)
		if(MSVC)
			target_compile_options(pyra_jxr PRIVATE /W0)
		else()
			target_compile_options(pyra_jxr PRIVATE -w)
		endif()
		target_link_libraries(${reader_target} PRIVATE pyra_jxr)
	endif()
endfunction()
