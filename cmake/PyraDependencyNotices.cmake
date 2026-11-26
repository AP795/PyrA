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
	set(jxrlib jxrlib/LICENSE)
	set(zstd zstd/LICENSE)
# libjxl and the copies of its dependencies it bundles (one folder each: all
# four name their license file LICENSE).
	set(libjxl libjxl/LICENSE libjxl/PATENTS libjxl/SOURCE-NOTICES.txt)
	set(highway libjxl/third_party/highway/LICENSE libjxl/third_party/highway/LICENSE-BSD3)
	set(brotli libjxl/third_party/brotli/LICENSE)
	set(skcms libjxl/third_party/skcms/LICENSE)
	foreach(dependency IN LISTS ARGN)
		set(notices ${${dependency}})
		if(NOT notices)
			message(FATAL_ERROR "Unknown dependency notice: ${dependency}")
		endif()
		list(TRANSFORM notices PREPEND "${deps}/")
		install(FILES ${notices} DESTINATION "${destination}/${dependency}")
	endforeach()
endfunction()
