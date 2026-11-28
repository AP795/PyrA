# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

# Warns when the browser client the viewer embeds is older than what it is made
# from. The client is a separate Emscripten build that no native build rebuilds,
# so a change to Core or Pvue otherwise reaches the browser only when someone
# remembers to rebuild it. Run with cmake -P, passing CLIENT_DIR (the
# PYRA_WEB_CLIENT_DIR folder) and SOURCE_DIR (this source tree).
#
# Only a client built from this source tree by Ninja is checked: its build tree
# says what the client is made from. A client copied from elsewhere has file
# times that mean nothing, so it is accepted as it is. The check only reads the
# client's build tree, so native trees sharing one client may build at once.
#
# `ninja -n` cannot answer: the tree's glob verification makes every dry run
# stop at regenerating the build files. The inputs are listed instead:
#   -t inputs pvue_client   sources and libraries linked into the client
#   -t deps                 headers the compiler read
#   -t query build.ninja    CMake files whose change regenerates the build

cmake_minimum_required(VERSION 3.20)

set(client_target pvue_client)
set(max_listed 5)
set(max_cache_depth 4)

set(wasm "${CLIENT_DIR}/${client_target}.wasm")
if(NOT EXISTS "${wasm}")
	message(WARNING "No ${client_target}.wasm in ${CLIENT_DIR}")
	return()
endif()

set(tree "")
set(dir "${CLIENT_DIR}")
foreach(depth RANGE ${max_cache_depth})
	if(EXISTS "${dir}/CMakeCache.txt")
		set(tree "${dir}")
		break()
	endif()
	get_filename_component(dir "${dir}" DIRECTORY)
endforeach()
if(NOT tree)
	message(STATUS "Browser client in ${CLIENT_DIR} is prebuilt; not checked")
	return()
endif()

file(STRINGS "${tree}/CMakeCache.txt" cache_lines
	REGEX "^(CMAKE_HOME_DIRECTORY|CMAKE_MAKE_PROGRAM|CMAKE_GENERATOR):")
set(home "")
set(ninja "")
set(generator "")
foreach(line IN LISTS cache_lines)
	if(line MATCHES "^CMAKE_HOME_DIRECTORY:[^=]*=(.*)$")
		set(home "${CMAKE_MATCH_1}")
	elseif(line MATCHES "^CMAKE_MAKE_PROGRAM:[^=]*=(.*)$")
		set(ninja "${CMAKE_MATCH_1}")
	elseif(line MATCHES "^CMAKE_GENERATOR:[^=]*=(.*)$")
		set(generator "${CMAKE_MATCH_1}")
	endif()
endforeach()

file(REAL_PATH "${SOURCE_DIR}" source_real)
set(home_real "")
if(home AND EXISTS "${home}")
	file(REAL_PATH "${home}" home_real)
endif()
if(CMAKE_HOST_WIN32)
	string(TOLOWER "${source_real}" source_real)
	string(TOLOWER "${home_real}" home_real)
endif()
if(NOT home_real STREQUAL source_real OR NOT generator STREQUAL "Ninja" OR NOT EXISTS "${ninja}")
	message(STATUS "Browser client in ${CLIENT_DIR} is not a Ninja build of this source tree; not checked")
	return()
endif()

set(listing "")
foreach(tool_args IN ITEMS "inputs;${client_target}" "deps" "query;build.ninja")
	execute_process(COMMAND "${ninja}" -C "${tree}" -t ${tool_args}
		OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
	if(NOT result EQUAL 0)
		string(REPLACE ";" " " tool_text "${tool_args}")
		message(WARNING "Cannot list the browser client's inputs (ninja -t ${tool_text}): ${error}")
		return()
	endif()
	string(APPEND listing "${output}\n")
endforeach()

# One path per line, indented and prefixed with | or || for implicit and
# order-only inputs; keep absolute paths only (objects and the cache are
# relative to the client's tree, and headers lines end with ": #deps ...").
string(REPLACE ";" "\\;" listing "${listing}")
string(REPLACE "\n" ";" lines "${listing}")
# Every object repeats the headers it shares: deduplicate before matching.
list(REMOVE_DUPLICATES lines)
set(inputs "")
foreach(line IN LISTS lines)
	if(line MATCHES "^[ \t]*(\\|\\|?[ \t]+)?(([A-Za-z]:)?/[^:]*)$")
		list(APPEND inputs "${CMAKE_MATCH_2}")
	endif()
endforeach()
list(REMOVE_DUPLICATES inputs)

set(newer "")
foreach(path IN LISTS inputs)
	if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}" AND "${path}" IS_NEWER_THAN "${wasm}")
		list(APPEND newer "${path}")
	endif()
endforeach()
list(LENGTH newer newer_count)
if(newer_count EQUAL 0)
	return()
endif()

list(SUBLIST newer 0 ${max_listed} listed)
list(JOIN listed "\n  " listed_text)
if(newer_count GREATER max_listed)
	math(EXPR more "${newer_count} - ${max_listed}")
	string(APPEND listed_text "\n  ... and ${more} more")
endif()
message(WARNING "The embedded browser client is older than ${newer_count} of the files it is built from:\n"
	"  ${listed_text}\n"
	"Rebuild it in an emsdk environment (see Pvue/README.md), then build this viewer again to embed it:\n"
	"  cmake --build \"${tree}\" --target ${client_target}")
# The same line in the compiler-message format, so Visual Studio counts it as a warning.
message(NOTICE "${wasm} : warning PYRAWEB1: embedded browser client is older than ${newer_count} of its sources; rebuild ${client_target}")
