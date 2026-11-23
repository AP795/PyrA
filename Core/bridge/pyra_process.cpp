// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// pyra_process: Python processing of a DICOM slide into a new channel, and the
// removal of channels it added. See docs/python-bridge.md.

#include "chunk_process.h"
#include "dcm_series.h"
#include "pyr_rt.h"
#include "pyr_win.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kUsage =
	"usage:\n"
	"  pyra_process SLIDE.dcm --script PROCESS.py --channel NAME [options]\n"
	"  pyra_process SLIDE.dcm --remove-channel NAME [--remove-channel NAME ...]\n"
	"  pyra_process SLIDE.dcm --list\n"
	"\n"
	"options:\n"
	"  --type gray8|gray16        output samples (default gray8)\n"
	"  --prepare-script FILE      coarse whole-slide pass: masks and parameters\n"
	"  --prepare-mpp X            prepare resolution, microns per pixel\n"
	"  --source-channel NAME      input plane (default: the slide's default plane)\n"
	"  --z MICRONS                input focal plane (default 0)\n"
	"  --chunk-tiles N            chunk edge in source frames (default ~2048 px)\n"
	"  --halo PX                  context pixels around each chunk (default 0)\n"
	"  --workers N                Python processes (default: half the cores)\n"
	"  --python PATH              interpreter (default $PYRA_PYTHON, else python/python3)\n"
	"  --worker PATH              pyra_worker.py (default: python/ beside this program)\n"
	"  --codec htj2k|jp2k         lossless frame codec\n"
	"  --uid-root ROOT            DICOM UID root for the new instances\n"
	"  --param KEY=VALUE          passed to both scripts; repeatable\n"
	"  --progress-lines           progress as 'progress PHASE DONE TOTAL' lines on stderr\n"
	"  --report-json              print the report as one JSON object\n";


std::filesystem::path executable_directory ()
{
#if defined(_WIN32)
	auto buffer = std::wstring( 32768, L'\0' );
	const auto length = GetModuleFileNameW( nullptr, buffer.data(), (DWORD)buffer.size() );
	buffer.resize( length );
	return std::filesystem::path( buffer ).parent_path();
#else
	auto buffer = std::string( 4096, '\0' );
	const auto length = readlink( "/proc/self/exe", buffer.data(), buffer.size() );
	if (length <= 0)
		return std::filesystem::current_path();
	buffer.resize( (size_t)length );
	return std::filesystem::path( buffer ).parent_path();
#endif
}


std::string path_utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return std::string( text.begin(), text.end() );
}


template <typename T>
bool parse_number ( std::string_view text, T *out )
{
	const auto result = std::from_chars( text.data(), text.data() + text.size(), *out );
	return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}


struct Command {
	std::filesystem::path slide;
	priml::ChunkProcessOptions options;
	std::vector<std::string> remove;
	bool list = false;
	bool progress_lines = false;
	bool report_json = false;
};


bool parse ( int argc, char **argv, Command *command, std::string *error )
{
	auto &options = command->options;
	options.worker_script = executable_directory() / "python" / "pyra_worker.py";
	auto positional = std::vector<std::string>{};
	for (auto i = 1; i < argc; ++i) {
		const auto arg = std::string_view( argv[i] );
		const auto value = [&] ( std::string_view *out ) {
			if (i + 1 >= argc) {
				*error = std::format( "{} needs a value", arg );
				return false;
			}
			*out = argv[++i];
			return true;
		};
		auto v = std::string_view{};
		auto ok = true;
		if (arg == "--list")
			command->list = true;
		else if (arg == "--progress-lines")
			command->progress_lines = true;
		else if (arg == "--report-json")
			command->report_json = true;
		else if (!arg.starts_with( "--" ))
			positional.emplace_back( arg );
		else if (!value( &v ))
			return false;
		else if (arg == "--script")
			options.process_script = pyr::path_from_utf8( v );
		else if (arg == "--prepare-script")
			options.prepare_script = pyr::path_from_utf8( v );
		else if (arg == "--channel")
			options.channel = v;
		else if (arg == "--remove-channel")
			command->remove.emplace_back( v );
		else if (arg == "--type") {
			ok = v == "gray8" || v == "gray16";
			options.output_type = v == "gray16" ? priml::LevelPixelType::Gray16 : priml::LevelPixelType::Gray8;
		} else if (arg == "--codec") {
			ok = v == "htj2k" || v == "jp2k";
			options.codec = v == "jp2k" ? priml::ImgCodec::Jp2k : priml::ImgCodec::Htj2k;
		} else if (arg == "--prepare-mpp")
			ok = parse_number( v, &options.prepare_mpp );
		else if (arg == "--source-channel")
			options.source.channel = v;
		else if (arg == "--z")
			ok = parse_number( v, &options.source.zlevel );
		else if (arg == "--chunk-tiles")
			ok = parse_number( v, &options.chunk_tiles );
		else if (arg == "--halo")
			ok = parse_number( v, &options.halo );
		else if (arg == "--workers")
			ok = parse_number( v, &options.workers );
		else if (arg == "--python")
			options.python = pyr::path_from_utf8( v );
		else if (arg == "--worker")
			options.worker_script = pyr::path_from_utf8( v );
		else if (arg == "--uid-root")
			options.uid_root = v;
		else if (arg == "--param") {
			const auto eq = v.find( '=' );
			ok = eq != std::string_view::npos && eq > 0;
			if (ok)
				options.params.emplace_back( std::string( v.substr( 0, eq ) ), std::string( v.substr( eq + 1 ) ) );
		} else {
			*error = std::format( "unknown option {}", arg );
			return false;
		}
		if (!ok) {
			*error = std::format( "invalid value for {}: {}", arg, v );
			return false;
		}
	}
	if (positional.size() != 1) {
		*error = "exactly one slide is required";
		return false;
	}
	command->slide = pyr::path_from_utf8( positional.front() );
	if (!command->list && command->remove.empty() && ( options.process_script.empty() || options.channel.empty() )) {
		*error = "--script and --channel are required";
		return false;
	}
	return true;
}


int list_channels ( const std::filesystem::path &slide )
{
	auto instances = std::vector<priml::DcmSeriesInstance>{};
	auto error = std::string{};
	if (!priml::dcm_series_instances( slide, &instances, &error )) {
		std::cerr << "pyra_process: " << error << "\n";
		return 1;
	}
	for (const auto &instance : instances)
		std::cout << std::format( "{:<10} {:<24} {:<8} {}\n", instance.role,
			instance.plane_name().empty() ? "(default)" : instance.plane_name(),
			instance.derived_channel() ? "derived" : "", path_utf8( instance.path.filename() ) );
	return 0;
}


int remove_channels ( const std::filesystem::path &slide, const std::vector<std::string> &channels )
{
	for (const auto &channel : channels) {
		auto removed = std::vector<std::filesystem::path>{};
		auto error = std::string{};
		if (!priml::remove_dcm_channel( slide, channel, &removed, &error )) {
			std::cerr << "pyra_process: " << error << "\n";
			return 1;
		}
		std::cout << std::format( "removed '{}': {} instance(s)\n", channel, removed.size() );
	}
	return 0;
}


} // namespace


int main ( int argc, char **argv )
{
	auto command = Command{};
	auto error = std::string{};
	if (argc < 2 || !parse( argc, argv, &command, &error )) {
		if (!error.empty())
			std::cerr << "pyra_process: " << error << "\n\n";
		std::cerr << kUsage;
		return 2;
	}
	if (command.list)
		return list_channels( command.slide );
	if (!command.remove.empty())
		return remove_channels( command.slide, command.remove );
	auto last_phase = std::string{};
	const auto progress = [&] ( std::string_view phase, size_t done, size_t total ) {
//
// Line mode is for a parent process reading stderr line by line (the web
// viewer's Analyse); the default redraws one line for a person at a terminal.
		if (command.progress_lines) {
			std::cerr << std::format( "progress {} {} {}\n", phase, done, total ) << std::flush;
			return;
		}
		if (phase != last_phase && !last_phase.empty())
			std::cerr << "\n";
		last_phase = phase;
		std::cerr << std::format( "\r{} {}/{}", phase, done, total ) << std::flush;
	};
	auto stats = priml::ChunkProcessStats{};
	const auto ok = priml::process_dcm_chunks( command.slide, command.options, &stats, &error, progress );
	std::cerr << "\n";
	if (!ok) {
		std::cerr << "pyra_process: " << error << "\n";
		return 1;
	}
	std::cout << ( command.report_json ? priml::format_chunk_report_json( stats ) + "\n"
		: priml::format_chunk_report( stats ) ) << std::flush;
	return 0;
}
