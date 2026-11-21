// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// pyra_make_gallery <directory> [--mpp X]
//
// Lays a directory of ordinary TIFF images (or an IPX file-set) out as one
// gallery slide and writes its ".glr" manifest beside them, for the viewers to
// open. Exit code 0 on success; the reason on stderr otherwise.

#include <pyra/pyra.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main ( int argc, char **argv )
{
	if (argc != 2 && !( argc == 4 && std::strcmp( argv[2], "--mpp" ) == 0 )) {
		std::cerr << "usage: pyra_make_gallery <directory> [--mpp microns_per_pixel]\n";
		return 2;
	}
	const auto mpp = argc == 4 ? std::strtod( argv[3], nullptr ) : 0.0;
	char manifest[4096] = {};
	auto error = pyra_error{};
	if (pyra_make_gallery( argv[1], mpp, manifest, sizeof( manifest ), &error ) != PYRA_OK) {
		std::cerr << "pyra_make_gallery: " << error.message << '\n';
		return 1;
	}
	std::cout << manifest << '\n';
	return 0;
}
