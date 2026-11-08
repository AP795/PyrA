find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

add_library(pyra_gui_support STATIC render_evaluator.cpp test_fixture.cpp viewer_settings.cpp)
target_link_libraries(pyra_gui_support PUBLIC Pyra::pyra Qt6::Gui)
target_include_directories(pyra_gui_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(pyra_gui_test main.cpp viewer_window.cpp viewer_window.h
	viewport.cpp viewport.h minimap.cpp minimap.h)
set_target_properties(pyra_gui_test PROPERTIES AUTOMOC ON)
target_link_libraries(pyra_gui_test PRIVATE pyra_gui_support Qt6::Widgets)

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
	add_test(NAME pyra_evaluator COMMAND pyra_evaluator_test
		"${CMAKE_CURRENT_BINARY_DIR}/test_out/evaluator")
	add_test(NAME pyra_gui_smoke COMMAND pyra_gui_test --smoke-test
		"${CMAKE_CURRENT_BINARY_DIR}/test_out/gui")
	set_tests_properties(pyra_gui_smoke PROPERTIES
		ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 40)
endif()
