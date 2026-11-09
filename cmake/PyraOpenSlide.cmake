# Optional C reader backend. Upstream remains a separate shared library.
set(PYRA_OPENSLIDE_ROOT "" CACHE PATH "OpenSlide shared SDK prefix")
set(PYRA_OPENSLIDE_SOURCE_ARCHIVE "" CACHE FILEPATH "Corresponding OpenSlide binary-build sources for deployment")

if(WIN32)
	find_path(PYRA_OPENSLIDE_INCLUDE_DIR openslide.h
		HINTS "${PYRA_OPENSLIDE_ROOT}/include/openslide" REQUIRED)
	find_library(PYRA_OPENSLIDE_LIBRARY NAMES libopenslide openslide
		HINTS "${PYRA_OPENSLIDE_ROOT}/lib" REQUIRED)
	find_file(PYRA_OPENSLIDE_DLL NAMES libopenslide-1.dll
		HINTS "${PYRA_OPENSLIDE_ROOT}/bin" REQUIRED)
	add_library(PyraOpenSlide SHARED IMPORTED)
	set_target_properties(PyraOpenSlide PROPERTIES
		IMPORTED_IMPLIB "${PYRA_OPENSLIDE_LIBRARY}"
		IMPORTED_LOCATION "${PYRA_OPENSLIDE_DLL}"
		INTERFACE_INCLUDE_DIRECTORIES "${PYRA_OPENSLIDE_INCLUDE_DIR}")
	target_link_libraries(pyra PRIVATE PyraOpenSlide)
	add_custom_command(TARGET pyra POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${PYRA_OPENSLIDE_DLL}" $<TARGET_FILE_DIR:pyra>)
	install(FILES "${PYRA_OPENSLIDE_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
	if(EXISTS "${PYRA_OPENSLIDE_ROOT}/licenses")
		install(DIRECTORY "${PYRA_OPENSLIDE_ROOT}/licenses/"
			DESTINATION ${CMAKE_INSTALL_DATADIR}/Pyra/OpenSlide/licenses)
		install(FILES "${PYRA_OPENSLIDE_ROOT}/versions.json" "${PYRA_OPENSLIDE_ROOT}/VERSIONS.md"
			DESTINATION ${CMAKE_INSTALL_DATADIR}/Pyra/OpenSlide)
	endif()
	if(NOT PYRA_OPENSLIDE_SOURCE_ARCHIVE AND
		EXISTS "${PYRA_OPENSLIDE_ROOT}/../openslide-bin-4.0.1.2.tar.gz")
		set(PYRA_OPENSLIDE_SOURCE_ARCHIVE "${PYRA_OPENSLIDE_ROOT}/../openslide-bin-4.0.1.2.tar.gz")
	endif()
	if(PYRA_OPENSLIDE_SOURCE_ARCHIVE)
		install(FILES "${PYRA_OPENSLIDE_SOURCE_ARCHIVE}"
			DESTINATION ${CMAKE_INSTALL_DATADIR}/Pyra/OpenSlide/source)
	endif()
else()
	if(PYRA_OPENSLIDE_ROOT)
		find_path(PYRA_OPENSLIDE_INCLUDE_DIR openslide.h
			HINTS "${PYRA_OPENSLIDE_ROOT}/include/openslide" REQUIRED)
		find_library(PYRA_OPENSLIDE_LIBRARY NAMES openslide
			HINTS "${PYRA_OPENSLIDE_ROOT}/lib" REQUIRED)
		add_library(PyraOpenSlide SHARED IMPORTED)
		set_target_properties(PyraOpenSlide PROPERTIES
			IMPORTED_LOCATION "${PYRA_OPENSLIDE_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${PYRA_OPENSLIDE_INCLUDE_DIR}")
		target_link_libraries(pyra PRIVATE PyraOpenSlide)
	else()
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(PYRA_OPENSLIDE REQUIRED IMPORTED_TARGET openslide>=4.0.1)
		target_link_libraries(pyra PRIVATE PkgConfig::PYRA_OPENSLIDE)
	endif()
endif()
