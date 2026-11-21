# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

set(PYRA_HTTPLIB_DIR "${PROJECT_SOURCE_DIR}/../httplib" CACHE PATH
	"Directory containing the shared cpp-httplib header and LICENSE")
foreach(httplib_file httplib.h LICENSE)
	if(NOT EXISTS "${PYRA_HTTPLIB_DIR}/${httplib_file}")
		message(FATAL_ERROR
			"Missing ${httplib_file} in ${PYRA_HTTPLIB_DIR}. Set PYRA_HTTPLIB_DIR to the cpp-httplib directory.")
	endif()
endforeach()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS index.html viewer.css viewer.js)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/index.html" PYRA_WEB_HTML)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/viewer.css" PYRA_WEB_CSS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/viewer.js" PYRA_WEB_JS)
configure_file(web_assets.h.in "${CMAKE_CURRENT_BINARY_DIR}/web_assets.h" @ONLY)

add_library(pyra_web_support STATIC web_server.cpp)
target_link_libraries(pyra_web_support PUBLIC Pyra::pyra Threads::Threads)
target_include_directories(pyra_web_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
	PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(pyra_web_support SYSTEM PRIVATE "${PYRA_HTTPLIB_DIR}")
target_compile_definitions(pyra_web_support PUBLIC UNICODE _UNICODE NOMINMAX)
add_executable(pyra_web_test main.cpp)
target_link_libraries(pyra_web_test PRIVATE pyra_web_support)
target_compile_definitions(pyra_web_test PRIVATE
	PYRA_WEB_IMAGE_DIRECTORY="${PROJECT_SOURCE_DIR}/tst_in"
	PYRA_WEB_LOG_DIRECTORY="${PROJECT_BINARY_DIR}/logs")
if(MSVC)
	target_compile_options(pyra_web_support PRIVATE /W4 /permissive- /utf-8)
	target_compile_options(pyra_web_test PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(pyra_web_support PRIVATE -Wall -Wextra)
	target_compile_options(pyra_web_test PRIVATE -Wall -Wextra)
endif()
set_target_properties(pyra_web_test PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS pyra_web_test RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${PYRA_HTTPLIB_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/Pyra RENAME httplib-LICENSE)
if(BUILD_TESTING)
	find_program(PYRA_NODE_EXECUTABLE NAMES node)
	if(PYRA_NODE_EXECUTABLE)
		add_test(NAME pyra_web_navigation COMMAND "${PYRA_NODE_EXECUTABLE}" --test
			"${CMAKE_CURRENT_SOURCE_DIR}/viewer_test.cjs")
	endif()
	add_executable(pyra_web_server_test web_server_test.cpp)
	target_link_libraries(pyra_web_server_test PRIVATE pyra_web_support)
	target_include_directories(pyra_web_server_test SYSTEM PRIVATE "${PYRA_HTTPLIB_DIR}")
	add_test(NAME pyra_web_server COMMAND pyra_web_server_test "${CMAKE_BINARY_DIR}/test_out")
	set_tests_properties(pyra_web_server PROPERTIES FIXTURES_REQUIRED "pyra_wsi_fixture;pyra_image_fixture;pyra_zyp_fixture" TIMEOUT 40)
endif()

# Windows has no rpath: put the reader library (and OpenSlide, when built) beside
# the server and its test, as the Qt viewer targets do.
if(WIN32)
	foreach(web_target pyra_web_test pyra_web_server_test)
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
