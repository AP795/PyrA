# Run explicitly; normal configuration never downloads dependencies.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PYRA_OPENSLIDE_DEST)
	message(FATAL_ERROR "Set PYRA_OPENSLIDE_DEST to an out-of-source dependency directory")
endif()
file(MAKE_DIRECTORY "${PYRA_OPENSLIDE_DEST}")
set(base_url "https://github.com/openslide/openslide-bin/releases/download/v4.0.1.2")

function(download_checked name sha256)
	set(destination "${PYRA_OPENSLIDE_DEST}/${name}")
	if(EXISTS "${destination}")
		file(SHA256 "${destination}" actual_hash)
		if(NOT actual_hash STREQUAL sha256)
			message(FATAL_ERROR "Checksum mismatch: ${destination}")
		endif()
	else()
		file(DOWNLOAD "${base_url}/${name}" "${destination}"
			EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON STATUS result)
		list(GET result 0 code)
		if(NOT code EQUAL 0)
			message(FATAL_ERROR "OpenSlide download failed: ${result}")
		endif()
	endif()
endfunction()

download_checked(openslide-bin-4.0.1.2-windows-x64.zip
	841bef094e1504a831caaaf767a969be436e30d84bc16a33f4d941953409c150)
# Complete matching source package, including the bundled dependency sources.
download_checked(openslide-bin-4.0.1.2.tar.gz
	83c9a4a598f3a5fe1dea83e123745e48b94ce7eab99833f04902bdf4fa0af1db)
file(ARCHIVE_EXTRACT
	INPUT "${PYRA_OPENSLIDE_DEST}/openslide-bin-4.0.1.2-windows-x64.zip"
	DESTINATION "${PYRA_OPENSLIDE_DEST}")
message(STATUS "Set PYRA_OPENSLIDE_ROOT=${PYRA_OPENSLIDE_DEST}/openslide-bin-4.0.1.2-windows-x64")
