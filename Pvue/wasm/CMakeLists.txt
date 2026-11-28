# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# Pvue's browser client: PyrA's reader without loaders plus Pvue's display rule,
# compiled to WebAssembly. Configured with emcmake against the Emscripten codec
# SDK (Deps/, see Pvue/README.md); a viewer embeds or serves its output (ViewW:
# PYRA_WEB_CLIENT_DIR names this build's directory).

# Decode threads the client may use; this many workers are started with the
# module, since a thread cannot be created on demand without yielding to the
# event loop. The page asks for min(hardware threads, this).
set(PYRA_CLIENT_MAX_THREADS 8)

add_executable(pvue_client pvue_client.cpp)
target_link_libraries(pvue_client PRIVATE pyra_pvue pyra)
target_compile_definitions(pvue_client PRIVATE PYRA_CLIENT_MAX_THREADS=${PYRA_CLIENT_MAX_THREADS})
set(pvue_client_exports _malloc _free _pvue_client_open _pvue_client_close _pvue_client_plane_count
	_pvue_client_recipe _pvue_client_recipe_planes _pvue_client_recipe_count _pvue_client_put
	_pvue_client_take_missing _pvue_client_missing _pvue_client_render _pvue_client_render_display
	_pvue_client_decode _pvue_client_pixels _pvue_client_width _pvue_client_height _pvue_client_error
	_pvue_client_cache)
list(JOIN pvue_client_exports "," pvue_client_export_list)
target_link_options(pvue_client PRIVATE
	-sMODULARIZE=1
	-sEXPORT_NAME=createPvueClient
	-sENVIRONMENT=worker,node
	-sALLOW_MEMORY_GROWTH=1
	-sMAXIMUM_MEMORY=2GB
	-sSTACK_SIZE=2MB
	-sPTHREAD_POOL_SIZE=${PYRA_CLIENT_MAX_THREADS}
	"-sEXPORTED_FUNCTIONS=${pvue_client_export_list}"
	-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,HEAPF64,UTF8ToString)

# The notices of the bundled libjxl (Deps/libjxl, built into the codec SDK) and
# of the libraries it bundles travel with the client in third-party/, laid out
# as the native install's notices, which the viewer installs them beside.
if(PYR_WITH_JXL)
	set(pyra_jxl_source "${PROJECT_SOURCE_DIR}/Deps/libjxl")
	set(pyra_jxl_notices libjxl:LICENSE libjxl:PATENTS libjxl:SOURCE-NOTICES.txt
		highway:third_party/highway/LICENSE highway:third_party/highway/LICENSE-BSD3
		brotli:third_party/brotli/LICENSE skcms:third_party/skcms/LICENSE)
	foreach(notice IN LISTS pyra_jxl_notices)
		string(REPLACE ":" ";" notice "${notice}")
		list(GET notice 0 component)
		list(GET notice 1 file)
		get_filename_component(name "${file}" NAME)
		configure_file("${pyra_jxl_source}/${file}" "${CMAKE_CURRENT_BINARY_DIR}/third-party/${component}/${name}" COPYONLY)
	endforeach()
endif()
