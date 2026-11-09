find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

add_library(pyra_gui_support STATIC render_evaluator.cpp fixture_client.cpp viewer_settings.cpp)
target_link_libraries(pyra_gui_support PUBLIC Pyra::pyra Qt6::Gui)
target_include_directories(pyra_gui_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(pyra_fixture_writer fixture_writer_main.cpp test_fixture.cpp)
target_link_libraries(pyra_fixture_writer PRIVATE pyra_core Qt6::Core)

add_executable(pyra_gui_test main.cpp viewer_window.cpp viewer_window.h
	viewport.cpp viewport.h minimap.cpp minimap.h)
set_target_properties(pyra_gui_test PROPERTIES AUTOMOC ON)
target_link_libraries(pyra_gui_test PRIVATE pyra_gui_support Qt6::Widgets)
add_dependencies(pyra_gui_test pyra_fixture_writer)

if(MSVC)
	target_compile_options(pyra_gui_support PRIVATE /W4 /permissive- /utf-8)
	target_compile_options(pyra_gui_test PRIVATE /W4 /permissive- /utf-8)
else()
	target_compile_options(pyra_gui_support PRIVATE -Wall -Wextra)
	target_compile_options(pyra_gui_test PRIVATE -Wall -Wextra)
endif()

if(BUILD_TESTING)
	add_executable(pyra_settings_test viewer_settings_test.cpp)
	target_link_libraries(pyra_settings_test PRIVATE pyra_gui_support)
	add_test(NAME pyra_settings COMMAND pyra_settings_test)
	add_executable(pyra_minimap_test minimap_test.cpp minimap.cpp minimap.h viewport.cpp viewport.h)
	set_target_properties(pyra_minimap_test PROPERTIES AUTOMOC ON)
	target_link_libraries(pyra_minimap_test PRIVATE Qt6::Widgets)
	add_test(NAME pyra_minimap COMMAND pyra_minimap_test)
	set_tests_properties(pyra_minimap PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
	add_executable(pyra_evaluator_test evaluator_test.cpp)
	target_link_libraries(pyra_evaluator_test PRIVATE pyra_gui_support)
	target_compile_definitions(pyra_evaluator_test PRIVATE PYRA_TEST_WITH_OPENJPEG=$<BOOL:${PYR_WITH_OPENJPEG}>)
	add_dependencies(pyra_evaluator_test pyra_fixture_writer)
	add_test(NAME pyra_evaluator COMMAND pyra_evaluator_test
		"${CMAKE_CURRENT_BINARY_DIR}/test_out/evaluator" "${CMAKE_BINARY_DIR}/test_out")
	set_tests_properties(pyra_evaluator PROPERTIES FIXTURES_REQUIRED pyra_wsi_fixture)
	add_test(NAME pyra_gui_smoke COMMAND pyra_gui_test --smoke-test
		"${CMAKE_CURRENT_BINARY_DIR}/test_out/gui")
	set_tests_properties(pyra_gui_smoke PROPERTIES
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 40)
	add_executable(pyra_viewer_behavior_test viewer_behavior_test.cpp viewer_window.cpp
		viewport.cpp viewport.h minimap.cpp minimap.h)
	set_target_properties(pyra_viewer_behavior_test PROPERTIES AUTOMOC ON)
	target_link_libraries(pyra_viewer_behavior_test PRIVATE pyra_gui_support Qt6::Widgets)
	add_dependencies(pyra_viewer_behavior_test pyra_fixture_writer)
	add_test(NAME pyra_viewer_behavior COMMAND pyra_viewer_behavior_test "${CMAKE_BINARY_DIR}/test_out")
	set_tests_properties(pyra_viewer_behavior PROPERTIES FIXTURES_REQUIRED pyra_wsi_fixture
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 40)
endif()

if(WIN32)
	foreach(gui_target pyra_gui_test pyra_fixture_writer pyra_settings_test pyra_minimap_test pyra_evaluator_test pyra_viewer_behavior_test)
		if(TARGET ${gui_target})
			add_custom_command(TARGET ${gui_target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_RUNTIME_DLLS:${gui_target}> $<TARGET_FILE_DIR:${gui_target}>
				COMMAND_EXPAND_LISTS)
			foreach(platform_plugin Qt6::QWindowsIntegrationPlugin Qt6::QOffscreenIntegrationPlugin)
				if(TARGET ${platform_plugin})
					add_custom_command(TARGET ${gui_target} POST_BUILD
						COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${gui_target}>/platforms
						COMMAND ${CMAKE_COMMAND} -E copy_if_different
						$<TARGET_FILE:${platform_plugin}> $<TARGET_FILE_DIR:${gui_target}>/platforms)
				endif()
			endforeach()
		endif()
	endforeach()
endif()
