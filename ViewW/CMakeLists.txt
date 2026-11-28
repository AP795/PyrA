# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

set(PYRA_HTTPLIB_DIR "${PROJECT_SOURCE_DIR}/Deps/httplib" CACHE PATH
	"Directory containing the cpp-httplib header and LICENSE")
foreach(httplib_file httplib.h LICENSE)
	if(NOT EXISTS "${PYRA_HTTPLIB_DIR}/${httplib_file}")
		message(FATAL_ERROR
			"Missing ${httplib_file} in ${PYRA_HTTPLIB_DIR}. Set PYRA_HTTPLIB_DIR to the cpp-httplib directory.")
	endif()
endforeach()

set(pyra_pvue_web "${PROJECT_SOURCE_DIR}/Pvue/web")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS index.html viewer.css viewer.js
	"${pyra_pvue_web}/pvue_view.js" "${pyra_pvue_web}/pvue_worker.js")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/index.html" PYRA_WEB_HTML)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/viewer.css" PYRA_WEB_CSS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/viewer.js" PYRA_WEB_JS)
file(READ "${pyra_pvue_web}/pvue_view.js" PYRA_WEB_PVUE_JS)
file(READ "${pyra_pvue_web}/pvue_worker.js" PYRA_WEB_WORKER_JS)
configure_file(web_assets.h.in "${CMAKE_CURRENT_BINARY_DIR}/web_assets.h" @ONLY)

add_library(pyra_web_support STATIC web_server.cpp web_analyse.cpp)

# The browser client: the output directory of an Emscripten build of Pvue/wasm
# (see Pvue/README.md). When set, the viewer embeds it and renders in the browser
# from stored tiles; otherwise every view is rendered by the server.
set(PYRA_WEB_CLIENT_DIR "" CACHE PATH "Directory holding pvue_client.js and pvue_client.wasm to embed")
if(PYRA_WEB_CLIENT_DIR)
	foreach(client_file pvue_client.js pvue_client.wasm)
		if(NOT EXISTS "${PYRA_WEB_CLIENT_DIR}/${client_file}")
			message(FATAL_ERROR "Missing ${client_file} in PYRA_WEB_CLIENT_DIR=${PYRA_WEB_CLIENT_DIR}")
		endif()
	endforeach()
	set(pvue_client_source "${CMAKE_CURRENT_BINARY_DIR}/web_client_assets.cpp")
	add_custom_command(OUTPUT "${pvue_client_source}"
		COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${pvue_client_source}" "-DJS=${PYRA_WEB_CLIENT_DIR}/pvue_client.js"
			"-DWASM=${PYRA_WEB_CLIENT_DIR}/pvue_client.wasm" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_client.cmake"
		DEPENDS "${PYRA_WEB_CLIENT_DIR}/pvue_client.js" "${PYRA_WEB_CLIENT_DIR}/pvue_client.wasm"
			"${CMAKE_CURRENT_SOURCE_DIR}/embed_client.cmake"
		COMMENT "Embedding the browser client")
	target_sources(pyra_web_support PRIVATE "${pvue_client_source}")
	target_compile_definitions(pyra_web_support PRIVATE PYRA_WEB_WITH_CLIENT=1)
# No native build rebuilds the client: every build warns when it is older than
# the sources it is built from (ViewW/check_client.cmake).
	add_custom_target(pyra_web_client_check
		COMMAND "${CMAKE_COMMAND}" "-DCLIENT_DIR=${PYRA_WEB_CLIENT_DIR}" "-DSOURCE_DIR=${Pyra_SOURCE_DIR}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/check_client.cmake"
		COMMENT "Checking the browser client is current"
		VERBATIM)
	add_dependencies(pyra_web_support pyra_web_client_check)
# Notices of what the client bundles beyond this build's own codecs (JPEG XL).
	if(IS_DIRECTORY "${PYRA_WEB_CLIENT_DIR}/third-party")
		install(DIRECTORY "${PYRA_WEB_CLIENT_DIR}/third-party/" DESTINATION "${pyra_notice_destination}")
	endif()
else()
	target_compile_definitions(pyra_web_support PRIVATE PYRA_WEB_WITH_CLIENT=0)
endif()
target_link_libraries(pyra_web_support PUBLIC pyra_pvue Pyra::pyra Threads::Threads)
target_include_directories(pyra_web_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
	PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(pyra_web_support SYSTEM PRIVATE "${PYRA_HTTPLIB_DIR}")
target_compile_definitions(pyra_web_support PUBLIC UNICODE _UNICODE NOMINMAX)
add_executable(PyrA_Web_Viewer main.cpp)
target_link_libraries(PyrA_Web_Viewer PRIVATE pyra_web_support)
target_compile_definitions(PyrA_Web_Viewer PRIVATE
	PYRA_WEB_IMAGE_DIRECTORY="${PYRA_VIEWER_IMAGE_DIRECTORY}"
	PYRA_WEB_LOG_DIRECTORY="${PROJECT_BINARY_DIR}/logs")

# Analyse runs the Python bridge. It is C++ on pyra_core, so the viewer then
# holds its own static copy of the reader beside pyra.dll; without the bridge
# the button stays disabled and says why. The build-time paths are fallbacks
# for running from the build tree; an install finds both beside the server.
if(TARGET pyra_bridge)
	target_link_libraries(pyra_web_support PUBLIC pyra_bridge)
	target_compile_definitions(pyra_web_support PUBLIC PYRA_WEB_WITH_BRIDGE=1
		PYRA_WEB_BRIDGE_CLI="$<TARGET_FILE:pyra_process>"
		PYRA_WEB_BRIDGE_PYTHON="${PROJECT_SOURCE_DIR}/Core/bridge/python")
	add_dependencies(PyrA_Web_Viewer pyra_process)
else()
	target_compile_definitions(pyra_web_support PUBLIC PYRA_WEB_WITH_BRIDGE=0)
endif()
if(MSVC)
	target_compile_options(pyra_web_support PRIVATE /W4 /permissive- /utf-8)
	target_compile_options(PyrA_Web_Viewer PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(pyra_web_support PRIVATE -Wall -Wextra)
	target_compile_options(PyrA_Web_Viewer PRIVATE -Wall -Wextra)
endif()
set_target_properties(PyrA_Web_Viewer PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS PyrA_Web_Viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${PYRA_HTTPLIB_DIR}/LICENSE" DESTINATION "${pyra_notice_destination}/cpp-httplib")
if(BUILD_TESTING)
	find_program(PYRA_NODE_EXECUTABLE NAMES node)
	if(PYRA_NODE_EXECUTABLE)
		add_test(NAME pyra_web_navigation COMMAND "${PYRA_NODE_EXECUTABLE}" --test
			"${CMAKE_CURRENT_SOURCE_DIR}/viewer_test.cjs")
	endif()
	add_executable(pyra_web_server_test web_server_test.cpp)
	target_link_libraries(pyra_web_server_test PRIVATE pyra_web_support)
	target_include_directories(pyra_web_server_test SYSTEM PRIVATE "${PYRA_HTTPLIB_DIR}")
	if(TARGET pyra_bridge)
		add_dependencies(pyra_web_server_test pyra_process)
	endif()
	add_test(NAME pyra_web_server COMMAND pyra_web_server_test "${PYRA_TEST_OUTPUT_DIRECTORY}")
	set_tests_properties(pyra_web_server PROPERTIES FIXTURES_REQUIRED "pyra_wsi_fixture;pyra_image_fixture;pyra_zyp_fixture" TIMEOUT 180)
endif()

# Windows has no rpath: put the reader library (and OpenSlide, when built) beside
# the server and its test, as the Qt viewer targets do.
if(WIN32)
	foreach(web_target PyrA_Web_Viewer pyra_web_server_test)
		if(TARGET ${web_target})
			add_custom_command(TARGET ${web_target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_RUNTIME_DLLS:${web_target}> $<TARGET_FILE_DIR:${web_target}>
				COMMAND_EXPAND_LISTS)
			if(PYRA_WITH_OPENSLIDE)
				add_custom_command(TARGET ${web_target} POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${PYRA_OPENSLIDE_DLL}" $<TARGET_FILE_DIR:${web_target}>)
			endif()
		endif()
	endforeach()
endif()
