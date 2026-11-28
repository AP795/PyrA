# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# Pvue: the display layer viewers share above the reader - which planes make the
# picture and how they become RGBA8 (view_compose), the browser client (wasm/,
# web/) and what a server feeding it agrees on (pvue_http.h). It reads pixels
# through the public C API only; Core's internal headers supply the composition
# rule (compose_rgb_from_channels), header-only.

add_library(pyra_pvue STATIC view_compose.cpp view_compose.h display_rgba.h pvue_http.h)
target_compile_features(pyra_pvue PUBLIC cxx_std_20)
target_link_libraries(pyra_pvue PUBLIC Pyra::pyra)
target_include_directories(pyra_pvue PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
	PRIVATE $<TARGET_PROPERTY:pyra_core,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(pyra_pvue PRIVATE $<TARGET_PROPERTY:pyra_core,INTERFACE_COMPILE_DEFINITIONS>)
set_target_properties(pyra_pvue PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
	target_compile_options(pyra_pvue PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(pyra_pvue PRIVATE -Wall -Wextra)
endif()

if(EMSCRIPTEN)
	add_subdirectory(wasm)
elseif(BUILD_TESTING)
	add_executable(pyra_pvue_test tests/view_compose_test.cpp tests/render_display_test.cpp)
	target_link_libraries(pyra_pvue_test PRIVATE pyra_pvue)
	target_include_directories(pyra_pvue_test PRIVATE $<TARGET_PROPERTY:pyra_core,INTERFACE_INCLUDE_DIRECTORIES>)
	target_compile_definitions(pyra_pvue_test PRIVATE $<TARGET_PROPERTY:pyra_core,INTERFACE_COMPILE_DEFINITIONS>)
	add_test(NAME pyra_pvue COMMAND pyra_pvue_test "${PYRA_TEST_OUTPUT_DIRECTORY}")
	set_tests_properties(pyra_pvue PROPERTIES FIXTURES_REQUIRED pyra_wsi_fixture TIMEOUT 120)
	if(WIN32)
		add_custom_command(TARGET pyra_pvue_test POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:pyra_pvue_test> $<TARGET_FILE_DIR:pyra_pvue_test>
			COMMAND_EXPAND_LISTS)
	endif()
	find_program(PYRA_NODE_EXECUTABLE NAMES node)
	if(PYRA_NODE_EXECUTABLE)
		add_test(NAME pyra_pvue_view COMMAND "${PYRA_NODE_EXECUTABLE}" --test
			"${CMAKE_CURRENT_SOURCE_DIR}/web/pvue_view_test.cjs")
	endif()
endif()
