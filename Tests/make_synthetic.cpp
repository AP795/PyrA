// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// pyra_make_synthetic <out.ptf|out.tif> [--size WxH] [--tile N] [--z N] [--mpp X]
//     [--codec jpeg|jp2k|htj2k|jxl|jxr|lzw] [--quality 1..100|lossless] [--seed N]
//     [--z-step UM] [--focus 0..1] [--nucleus UM] [--spacing UM]
//     [--positive 0..1] [--tumour 0..1] [--lymphocytes 0..1]
//
// Writes a synthetic bright-field IHC z-stack through pyra_make_synthetic.
// --tile 0 writes an untiled TIFF instead (one plane: --z 1; LZW or JPEG).
// Exit code 0 on success; the reason on stderr otherwise.

#include <pyra/pyra.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace {

void usage ()
{
	std::cerr << "usage: pyra_make_synthetic <out.ptf|out.tif> [--size WxH] [--tile N] [--z N] [--mpp X]\n"
		"       [--codec jpeg|jp2k|htj2k|jxl|jxr|lzw] [--quality 1..100|lossless] [--seed N]\n"
		"       [--z-step UM] [--focus 0..1] [--nucleus UM] [--spacing UM]\n"
		"       [--positive 0..1] [--tumour 0..1] [--lymphocytes 0..1]\n"
		"       --tile 0 writes an untiled TIFF: one plane (--z 1), LZW or JPEG\n";
}


// The real-valued options, by flag.
double *real_option ( pyra_synthetic_options *options, const std::string &key )
{
	const std::pair<const char *, double *> table[] = {
		{ "--mpp", &options->mpp },
		{ "--z-step", &options->z_step_um },
		{ "--focus", &options->focus_fraction },
		{ "--nucleus", &options->nucleus_diameter_um },
		{ "--spacing", &options->nucleus_spacing_um },
		{ "--positive", &options->positive_fraction },
		{ "--tumour", &options->tumour_fraction },
		{ "--lymphocytes", &options->lymphocyte_fraction },
	};
	for (const auto &[name, field] : table) {
		if (key == name)
			return field;
	}
	return nullptr;
}


bool parse_codec ( const std::string &name, pyra_codec *codec )
{
	if (name == "jpeg") *codec = PYRA_CODEC_JPEG;
	else if (name == "jp2k" || name == "jpeg2000") *codec = PYRA_CODEC_JPEG2000;
	else if (name == "htj2k") *codec = PYRA_CODEC_HTJ2K;
	else if (name == "jxl" || name == "jpegxl") *codec = PYRA_CODEC_JPEGXL;
	else if (name == "jxr" || name == "jpegxr") *codec = PYRA_CODEC_JPEGXR;
	else if (name == "lzw") *codec = PYRA_CODEC_LZW;
	else return false;
	return true;
}


int print_progress ( void *, uint64_t done, uint64_t total )
{
	if (done == total || done % 64 == 0)
		std::fprintf( stderr, "\r%llu / %llu tiles", (unsigned long long)done, (unsigned long long)total );
	if (done == total)
		std::fputc( '\n', stderr );
	return 1;
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc < 2 || argc % 2 != 0) {
		usage();
		return 2;
	}
	auto options = pyra_synthetic_options{};
	pyra_synthetic_options_init( &options );
	options.progress = print_progress;
	for (auto i = 2; i + 1 < argc; i += 2) {
		const auto key = std::string( argv[i] );
		const auto value = std::string( argv[i + 1] );
		if (key == "--size") {
			unsigned w = 0, h = 0;
			if (std::sscanf( value.c_str(), "%ux%u", &w, &h ) != 2) {
				usage();
				return 2;
			}
			options.width = w;
			options.height = h;
		}
		else if (key == "--tile") options.tile_edge = uint32_t( std::strtoul( value.c_str(), nullptr, 10 ) );
		else if (key == "--z") options.z_levels = uint32_t( std::strtoul( value.c_str(), nullptr, 10 ) );
		else if (auto *real = real_option( &options, key )) *real = std::strtod( value.c_str(), nullptr );
		else if (key == "--seed") options.seed = std::strtoull( value.c_str(), nullptr, 0 );
		else if (key == "--quality") {
			options.quality = value == "lossless" ? PYRA_QUALITY_LOSSLESS
				: uint32_t( std::strtoul( value.c_str(), nullptr, 10 ) );
		}
		else if (key != "--codec" || !parse_codec( value, &options.codec )) {
			usage();
			return 2;
		}
	}
	auto error = pyra_error{};
	if (pyra_make_synthetic( &options, argv[1], &error ) != PYRA_OK) {
		std::cerr << "pyra_make_synthetic: " << error.message << '\n';
		return 1;
	}
	std::cout << argv[1] << '\n';
	return 0;
}
