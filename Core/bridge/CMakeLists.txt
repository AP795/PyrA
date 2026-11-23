# Python processing bridge: pyra_process and its library. Kept out of pyra_core
# (and out of the sources vendored into hosts): it starts processes.

add_library(pyra_bridge STATIC
	bridge_protocol.h
	bridge_worker.cpp bridge_worker.h
	chunk_assemble.cpp chunk_internal.h chunk_pyramid.h
	chunk_process.cpp chunk_process.h
	pyra_subprocess.cpp pyra_subprocess.h)
target_include_directories(pyra_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pyra_bridge PUBLIC pyra_core Threads::Threads)
target_compile_features(pyra_bridge PUBLIC cxx_std_20)

add_executable(pyra_process pyra_process.cpp)
target_link_libraries(pyra_process PRIVATE pyra_bridge)

foreach(target pyra_bridge pyra_process)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
	endif()
endforeach()

# The worker and the examples travel with the program: python/ beside it. A
# target of its own, run on every build: a change to a script alone relinks
# nothing, so a post-build step would leave a stale copy.
add_custom_target(pyra_process_python ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/python
		$<TARGET_FILE_DIR:pyra_process>/python
	COMMENT "Copying the bridge's Python files beside pyra_process")
add_dependencies(pyra_process_python pyra_process)
install(TARGETS pyra_process RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY python/ DESTINATION ${CMAKE_INSTALL_BINDIR}/python
	PATTERN "__pycache__" EXCLUDE)

if(BUILD_TESTING)
	add_executable(pyra_bridge_test "${PROJECT_SOURCE_DIR}/Tests/bridge_test.cpp")
	target_link_libraries(pyra_bridge_test PRIVATE pyra_bridge)
	target_compile_definitions(pyra_bridge_test PRIVATE
		PYRA_BRIDGE_WORKER="${CMAKE_CURRENT_SOURCE_DIR}/python/pyra_worker.py")
	if(MSVC)
		target_compile_options(pyra_bridge_test PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(pyra_bridge_test PRIVATE -Wall -Wextra)
	endif()
	add_test(NAME pyra_bridge COMMAND pyra_bridge_test "${PYRA_TEST_OUTPUT_DIRECTORY}/bridge")
	set_tests_properties(pyra_bridge PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endif()
