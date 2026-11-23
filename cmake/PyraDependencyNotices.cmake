# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

function(pyra_install_dependency_notices destination)
	set(deps "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../Deps")
	set(libjpeg-turbo jpegturbo3_1_3/LICENSE.md jpegturbo3_1_3/README.ijg IJG-ACKNOWLEDGMENT.txt)
	set(libtiff libtiff47/LICENSE.md libtiff47/SOURCE-NOTICES.txt)
	set(openjpeg openjp25/LICENSE openjp25/SOURCE-NOTICES.txt)
	set(zlib zlib132/LICENSE)
	set(openjph OpenJPH/LICENSE OpenJPH/SOURCE-NOTICES.txt)
	set(dcmtk dcmtk370/COPYRIGHT dcmtk370/oflog/docs/LICENSE)
	set(cpp-httplib httplib/LICENSE)
	foreach(dependency IN LISTS ARGN)
		set(notices ${${dependency}})
		if(NOT notices)
			message(FATAL_ERROR "Unknown dependency notice: ${dependency}")
		endif()
		list(TRANSFORM notices PREPEND "${deps}/")
		install(FILES ${notices} DESTINATION "${destination}/${dependency}")
	endforeach()
endfunction()
