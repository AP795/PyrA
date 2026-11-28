// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "web_analyse.h"

#include <httplib.h>

#if PYRA_WEB_WITH_BRIDGE
#include "chunk_process.h"
#include "pyr_rt.h"
#include "pyra_subprocess.h"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pyra::web {
namespace {

using Clock = std::chrono::steady_clock;

std::string utf8 ( const std::filesystem::path &path )
{
	const auto bytes = path.u8string();
	return { bytes.begin(), bytes.end() };
}


int hardware_threads ()
{
	return std::max( 1, (int)std::thread::hardware_concurrency() );
}


int request_integer ( const httplib::Request &request, const char *name, int minimum, int maximum )
{
	const auto value = request.get_param_value( name );
	auto number = 0;
	const auto parsed = std::from_chars( value.data(), value.data() + value.size(), number );
	if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
		number < minimum || number > maximum)
		throw std::invalid_argument( std::format( "{} must be a whole number from {} to {}", name, minimum, maximum ) );
	return number;
}


std::string_view trim ( std::string_view text )
{
	while (!text.empty() && ( text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ))
		text.remove_prefix( 1 );
	while (!text.empty() && ( text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ))
		text.remove_suffix( 1 );
	return text;
}


// "key=value" lines; blank lines are skipped, anything else is an error.
std::vector<std::pair<std::string, std::string>> parse_params ( std::string_view text )
{
	auto params = std::vector<std::pair<std::string, std::string>>{};
	while (!text.empty()) {
		const auto end = text.find( '\n' );
		const auto line = trim( text.substr( 0, end ) );
		text = end == std::string_view::npos ? std::string_view{} : text.substr( end + 1 );
		if (line.empty())
			continue;
		const auto eq = line.find( '=' );
		if (eq == std::string_view::npos || trim( line.substr( 0, eq ) ).empty())
			throw std::invalid_argument( std::format( "Parameter line '{}' is not key=value", line ) );
		params.emplace_back( std::string( trim( line.substr( 0, eq ) ) ), std::string( trim( line.substr( eq + 1 ) ) ) );
	}
	return params;
}


struct RunRequest {
	bool cli = false;
	std::filesystem::path process;
	std::filesystem::path prepare;
	std::string channel;
	bool gray16 = false;
	int halo = 0;
	int workers = 1;
	std::vector<std::pair<std::string, std::string>> params;
};

} // namespace


struct Analyser::Impl {
	ServerOptions options;
	mutable std::mutex mutex;
	struct Run {
		uint64_t id = 0;
		bool running = false;
		std::string mode, path, channel, phase, error;
		std::string stats;				// the bridge's JSON report; empty until a run succeeds
		uint64_t done = 0, total = 0;
		int workers = 0;
		double wall_s = 0.0;
	} run;
// Last: its destructor stops (a CLI run is killed) and joins before the state it uses goes.
	std::jthread job;

	[[nodiscard]] std::string unavailable_reason () const;
	[[nodiscard]] std::filesystem::path script ( const std::string &name, bool optional ) const;
	[[nodiscard]] RunRequest parse ( const httplib::Request &request ) const;
	void progress ( std::string_view phase, uint64_t done, uint64_t total );
	void finish ( bool ok, std::string report, std::string error, Clock::time_point started );
	void run_server ( const RunRequest &request, const std::filesystem::path &slide );
	void run_cli ( const RunRequest &request, const std::filesystem::path &slide, std::stop_token stop );
	[[nodiscard]] std::string status_locked () const;
};


std::string Analyser::Impl::unavailable_reason () const
{
#if !PYRA_WEB_WITH_BRIDGE
	return "This viewer was built without the Python bridge (PYRA_BUILD_BRIDGE).";
#elif !USE_DCMTK
	return "This viewer was built without DICOM support (PYR_WITH_DCMTK).";
#else
	auto error = std::error_code{};
	if (options.scripts.empty() || !std::filesystem::is_directory( options.scripts, error ))
		return "No scripts folder: start the viewer with --scripts DIR.";
	if (options.bridge_worker.empty() || !std::filesystem::is_regular_file( options.bridge_worker, error ))
		return "pyra_worker.py was not found: start the viewer with --bridge-worker PATH.";
	return {};
#endif
}


// A plain *.py file name in the scripts folder, nothing else: the dialog may only
// run scripts someone put there.
std::filesystem::path Analyser::Impl::script ( const std::string &name, bool optional ) const
{
	if (name.empty()) {
		if (optional)
			return {};
		throw std::invalid_argument( "Choose a process script" );
	}
	const auto file = utf8_path( name );
	if (file != file.filename() || name.find_first_of( "/\\:" ) != std::string::npos || file.extension() != ".py")
		throw std::invalid_argument( "Scripts are chosen by file name from the scripts folder" );
	auto error = std::error_code{};
	const auto path = std::filesystem::canonical( options.scripts / file, error );
	if (error || !std::filesystem::is_regular_file( path, error ) ||
		path.parent_path() != std::filesystem::canonical( options.scripts, error ))
		throw std::invalid_argument( std::format( "{} is not in the scripts folder", name ) );
	return path;
}


RunRequest Analyser::Impl::parse ( const httplib::Request &request ) const
{
	auto parsed = RunRequest{};
	const auto mode = request.get_param_value( "mode" );
	if (mode != "cli" && mode != "server")
		throw std::invalid_argument( "Mode must be cli or server" );
	parsed.cli = mode == "cli";
	if (parsed.cli && ( options.bridge_cli.empty() || !std::filesystem::is_regular_file( options.bridge_cli ) ))
		throw std::invalid_argument( "pyra_process was not found; start the viewer with --bridge-cli PATH or apply by server" );
	parsed.process = script( request.get_param_value( "process" ), false );
	parsed.prepare = script( request.get_param_value( "prepare" ), true );
	parsed.channel = request.get_param_value( "channel" );
	if (parsed.channel.empty())
		throw std::invalid_argument( "Name the new channel" );
	const auto type = request.get_param_value( "type" );
	if (type != "gray8" && type != "gray16")
		throw std::invalid_argument( "Output type must be gray8 or gray16" );
	parsed.gray16 = type == "gray16";
	parsed.halo = request_integer( request, "halo", 0, kAnalyseMaxHalo );
	parsed.workers = request_integer( request, "workers", 1, hardware_threads() );
	parsed.params = parse_params( request.get_param_value( "params" ) );
	return parsed;
}


void Analyser::Impl::progress ( std::string_view phase, uint64_t done, uint64_t total )
{
	auto lock = std::lock_guard( mutex );
	run.phase = phase;
	run.done = done;
	run.total = total;
}


void Analyser::Impl::finish ( bool ok, std::string report, std::string error, Clock::time_point started )
{
	const auto wall = std::chrono::duration<double>( Clock::now() - started ).count();
	auto lock = std::lock_guard( mutex );
	run.running = false;
	run.wall_s = wall;
	run.stats = ok ? std::move( report ) : std::string{};
	run.error = ok ? std::string{} : ( error.empty() ? std::string( "Analysis failed" ) : std::move( error ) );
	if (options.log)
		options.log( ok ? std::format( "Analyse {} finished in {:.2f} s", run.mode, wall )
			: std::format( "Analyse {} failed after {:.2f} s: {}", run.mode, wall, run.error ) );
}


std::string Analyser::Impl::status_locked () const
{
	return std::format( "{{\"id\":{},\"running\":{},\"mode\":{},\"path\":{},\"channel\":{},\"workers\":{},"
		"\"phase\":{},\"done\":{},\"total\":{},\"error\":{},\"stats\":{},\"wall_s\":{:.3f}}}",
		run.id, run.running, json_string( run.mode ), json_string( run.path ), json_string( run.channel ), run.workers,
		json_string( run.phase ), run.done, run.total, json_string( run.error ), run.stats.empty() ? std::string( "null" ) : run.stats, run.wall_s );
}


#if PYRA_WEB_WITH_BRIDGE

namespace {

// The bridge's own copy of the reader logs through pyr::plog; send it to the
// session log instead of the console. One sink for the process, as plog has.
std::mutex g_sink_mutex;
std::function<void ( const std::string & )> g_sink;

void forward_log ( const char *message )
{
	auto lock = std::lock_guard( g_sink_mutex );
	if (!g_sink)
		return;
	auto text = std::string( message );
	while (!text.empty() && ( text.back() == '\n' || text.back() == '\r' ))
		text.pop_back();
	g_sink( "bridge: " + text );
}

} // namespace


void Analyser::Impl::run_server ( const RunRequest &request, const std::filesystem::path &slide )
{
	const auto started = Clock::now();
	auto options_ = priml::ChunkProcessOptions{};
	options_.process_script = request.process;
	options_.prepare_script = request.prepare;
	options_.python = options.python;
	options_.worker_script = options.bridge_worker;
	options_.channel = request.channel;
	options_.output_type = request.gray16 ? priml::LevelPixelType::Gray16 : priml::LevelPixelType::Gray8;
	options_.halo = request.halo;
	options_.workers = request.workers;
	options_.params = request.params;
	auto stats = priml::ChunkProcessStats{};
	auto error = std::string{};
	const auto ok = priml::process_dcm_chunks( slide, options_, &stats, &error,
		[this] ( std::string_view phase, size_t done, size_t total ) { progress( phase, done, total ); } );
	finish( ok, priml::format_chunk_report_json( stats ), error, started );
}


// pyra_process with --progress-lines --report-json: progress on stderr, the
// report as one JSON object on stdout.
void Analyser::Impl::run_cli ( const RunRequest &request, const std::filesystem::path &slide, std::stop_token stop )
{
	const auto started = Clock::now();
	auto spec = priml::SubprocessSpec{};
	spec.executable = options.bridge_cli;
	spec.arguments = { utf8( slide ), "--script", utf8( request.process ), "--channel", request.channel,
		"--type", request.gray16 ? "gray16" : "gray8", "--halo", std::to_string( request.halo ),
		"--workers", std::to_string( request.workers ), "--worker", utf8( options.bridge_worker ), "--progress-lines", "--report-json" };
	if (!request.prepare.empty())
		spec.arguments.insert( spec.arguments.end(), { "--prepare-script", utf8( request.prepare ) } );
	if (!options.python.empty())
		spec.arguments.insert( spec.arguments.end(), { "--python", utf8( options.python ) } );
	for (const auto &[key, value] : request.params)
		spec.arguments.insert( spec.arguments.end(), { "--param", key + "=" + value } );
	spec.stderr_line = [this] ( const std::string &line ) {
		if (line.starts_with( "progress " )) {
			auto phase = std::string{};
			auto done = uint64_t{}, total = uint64_t{};
			const auto first = line.find( ' ', 9 ), second = first == std::string::npos ? first : line.find( ' ', first + 1 );
			if (second != std::string::npos) {
				phase = line.substr( 9, first - 9 );
				std::from_chars( line.data() + first + 1, line.data() + second, done );
				std::from_chars( line.data() + second + 1, line.data() + line.size(), total );
				progress( phase, done, total );
				return;
			}
		}
		if (options.log)
			options.log( "pyra_process: " + line );
	};
	auto process = priml::Subprocess{};
	auto error = std::string{};
	if (!process.start( spec, &error )) {
		finish( false, {}, error, started );
		return;
	}
	process.close_input();
	const auto kill_on_stop = std::stop_callback( stop, [&process] { process.kill(); } );
	auto report = std::string{};
	process.read_until_end( &report );
//
// One JSON line; text mode on Windows adds a '\r' before its '\n'.
	while (!report.empty() && ( report.back() == '\n' || report.back() == '\r' ))
		report.pop_back();
	const auto code = process.wait();
	if (code != 0) {
		const auto tail = process.stderr_tail();
		const auto marker = std::string( "pyra_process: " );
		const auto at = tail.rfind( marker );
		error = at == std::string::npos ? std::format( "pyra_process exited with code {}", code ) : tail.substr( at + marker.size() );
		while (!error.empty() && ( error.back() == '\n' || error.back() == '\r' ))
			error.pop_back();
	}
//
// Decided before the call: argument evaluation order is unspecified, and the
// report is moved into one of them.
	const auto ok = code == 0 && report.starts_with( '{' );
	if (code == 0 && !ok)
		error = "pyra_process finished without printing its JSON report";
	finish( ok, std::move( report ), std::move( error ), started );
}

#endif


Analyser::Analyser ( const ServerOptions &options ) : impl_( std::make_unique<Impl>() )
{
	impl_->options = options;
#if PYRA_WEB_WITH_BRIDGE
	if (options.log) {
		{
			auto lock = std::lock_guard( g_sink_mutex );
			g_sink = options.log;
		}
		pyr::set_log_sink( forward_log, forward_log );
	}
#endif
}


Analyser::~Analyser ()
{
	if (impl_->job.joinable()) {
		impl_->job.request_stop();
		impl_->job.join();
	}
#if PYRA_WEB_WITH_BRIDGE
	pyr::set_log_sink( nullptr, nullptr );
	auto lock = std::lock_guard( g_sink_mutex );
	g_sink = nullptr;
#endif
}


std::string Analyser::capabilities_json () const
{
	const auto reason = impl_->unavailable_reason();
	const auto cores = hardware_threads();
	auto error = std::error_code{};
	const auto cli = !impl_->options.bridge_cli.empty() && std::filesystem::is_regular_file( impl_->options.bridge_cli, error );
	return std::format( "{{\"available\":{},\"reason\":{},\"cli\":{},\"cores\":{},\"workers\":{},\"max_halo\":{}}}",
		reason.empty(), json_string( reason ), cli, cores, std::max( 1, cores / 2 ), kAnalyseMaxHalo );
}


std::string Analyser::scripts_json () const
{
	auto names = std::vector<std::string>{};
	auto error = std::error_code{};
	if (!impl_->options.scripts.empty() && std::filesystem::is_directory( impl_->options.scripts, error ))
		for (const auto &entry : std::filesystem::directory_iterator( impl_->options.scripts, error ))
			if (entry.is_regular_file( error ) && entry.path().extension() == ".py")
				names.push_back( utf8( entry.path().filename() ) );
	std::sort( names.begin(), names.end() );
	auto list = std::string{};
	for (const auto &name : names)
		list += ( list.empty() ? "" : "," ) + json_string( name );
	return std::format( "{{\"folder\":{},\"scripts\":[{}]}}", json_string( utf8( impl_->options.scripts ) ), list );
}


bool Analyser::running () const
{
	auto lock = std::lock_guard( impl_->mutex );
	return impl_->run.running;
}


bool Analyser::start ( const httplib::Request &request, const std::filesystem::path &slide,
	const std::string &relative, std::string *body )
{
	if (const auto reason = impl_->unavailable_reason(); !reason.empty())
		throw std::invalid_argument( reason );
	auto extension = utf8( slide.extension() );
	std::transform( extension.begin(), extension.end(), extension.begin(),
		[] ( unsigned char c ) { return (char)std::tolower( c ); } );
	if (extension != ".dcm")
		throw std::invalid_argument( "Analyse works on DICOM slides (.dcm) only" );
	const auto run = impl_->parse( request );
	auto lock = std::lock_guard( impl_->mutex );
	if (impl_->run.running) {
		*body = impl_->status_locked();
		return false;
	}
	if (impl_->job.joinable())
		impl_->job.join();
	auto &state = impl_->run;
	state = Impl::Run{ state.id + 1, true, run.cli ? "cli" : "server", relative, run.channel, "starting", {}, {}, 0, 0, run.workers, 0.0 };
	if (impl_->options.log)
		impl_->options.log( std::format( "Analyse {} on {}: {} -> '{}', {} workers", state.mode, relative,
			utf8( run.process.filename() ), run.channel, run.workers ) );
#if PYRA_WEB_WITH_BRIDGE
	impl_->job = std::jthread( [impl = impl_.get(), run, slide] ( std::stop_token stop ) {
		if (run.cli)
			impl->run_cli( run, slide, stop );
		else
			impl->run_server( run, slide );
	} );
#endif
	*body = impl_->status_locked();
	return true;
}


std::string Analyser::status_json () const
{
	auto lock = std::lock_guard( impl_->mutex );
	return impl_->status_locked();
}

} // namespace pyra::web
