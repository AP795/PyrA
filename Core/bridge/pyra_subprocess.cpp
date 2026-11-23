// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "pyra_subprocess.h"

#include "pyr_rt.h"
#include "pyr_win.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace priml {
namespace {

bool spawn_fail ( std::string *error, std::string message )
{
	if (error)
		*error = std::move( message );
	return false;
}


[[maybe_unused]] std::string path_utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return std::string( text.begin(), text.end() );
}


// Environment variable names compare case-insensitively on Windows only.
bool same_name ( std::string_view a, std::string_view b )
{
#if defined(_WIN32)
	return a.size() == b.size() && std::equal( a.begin(), a.end(), b.begin(), [] ( char x, char y ) {
		return std::tolower( (unsigned char)x ) == std::tolower( (unsigned char)y );
	} );
#else
	return a == b;
#endif
}


// The parent's environment with the spec's variables added or replaced, as
// NAME=VALUE strings.
std::vector<std::string> merged_environment ( const SubprocessSpec &spec )
{
	auto entries = std::vector<std::string>{};
#if defined(_WIN32)
	if (auto *block = GetEnvironmentStringsW()) {
		for (auto *p = block; *p; p += wcslen( p ) + 1) {
			const auto wide = std::wstring_view( p );
			auto narrow = std::string{};
			const auto bytes = WideCharToMultiByte( CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr );
			narrow.resize( (size_t)std::max( bytes, 0 ) );
			WideCharToMultiByte( CP_UTF8, 0, wide.data(), (int)wide.size(), narrow.data(), bytes, nullptr, nullptr );
			entries.push_back( std::move( narrow ) );
		}
		FreeEnvironmentStringsW( block );
	}
#else
	for (auto **p = environ; p && *p; ++p)
		entries.emplace_back( *p );
#endif
	for (const auto &[name, value] : spec.environment) {
		std::erase_if( entries, [&name] ( const std::string &entry ) {
			const auto eq = entry.find( '=', 1 );
			return eq != std::string::npos && same_name( std::string_view( entry ).substr( 0, eq ), name );
		} );
		entries.push_back( name + "=" + value );
	}
	return entries;
}

} // namespace


struct Subprocess::Impl {
#if defined(_WIN32)
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	HANDLE input = nullptr;
	HANDLE output = nullptr;
	HANDLE errors = nullptr;
#else
	pid_t pid = -1;
	int input = -1;
	int output = -1;
	int errors = -1;
#endif
	std::thread drain;
	mutable std::mutex tail_mutex;
	std::string tail;
	std::function<void ( const std::string & )> line;
	bool started = false;
	bool exited = false;
	int exit_code = -1;

	void drain_errors ();
	void keep_tail ( const char *bytes, size_t length );
	void close_handles ();
};


void Subprocess::Impl::keep_tail ( const char *bytes, size_t length )
{
	auto guard = std::scoped_lock( tail_mutex );
	tail.append( bytes, length );
	if (tail.size() > kSubprocessStderrTailBytes)
		tail.erase( 0, tail.size() - kSubprocessStderrTailBytes );
}


void Subprocess::Impl::drain_errors ()
{
	auto pending = std::string{};
	char buffer[4096];
	for (;;) {
#if defined(_WIN32)
		auto got = DWORD{};
		if (!ReadFile( errors, buffer, sizeof( buffer ), &got, nullptr ) || got == 0)
			break;
#else
		const auto got = ::read( errors, buffer, sizeof( buffer ) );
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
#endif
		keep_tail( buffer, (size_t)got );
		pending.append( buffer, (size_t)got );
		for (auto end = pending.find( '\n' ); end != std::string::npos; end = pending.find( '\n' )) {
			auto text = pending.substr( 0, end );
			pending.erase( 0, end + 1 );
			if (!text.empty() && text.back() == '\r')
				text.pop_back();
			if (line)
				line( text );
		}
	}
	if (!pending.empty() && line)
		line( pending );
}


void Subprocess::Impl::close_handles ()
{
#if defined(_WIN32)
	for (auto *handle : { &input, &output, &errors, &process, &job })
		if (*handle) {
			CloseHandle( *handle );
			*handle = nullptr;
		}
#else
	for (auto *fd : { &input, &output, &errors })
		if (*fd >= 0) {
			::close( *fd );
			*fd = -1;
		}
#endif
}


Subprocess::Subprocess () : impl_( std::make_unique<Impl>() ) {}


Subprocess::~Subprocess ()
{
	if (impl_->started) {
		close_input();
		if (running()) {
//
// A worker exits on end of input; give it a moment before forcing it.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
			while (running() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
			if (running())
				kill();
		}
		wait();
	}
	impl_->close_handles();
}


#if defined(_WIN32)

namespace {

// One argument as CommandLineToArgvW will split it back out.
std::wstring quote_argument ( const std::wstring &argument )
{
	if (!argument.empty() && argument.find_first_of( L" \t\n\v\"" ) == std::wstring::npos)
		return argument;
	auto out = std::wstring( 1, L'"' );
	for (auto it = argument.begin(); ; ++it) {
		auto backslashes = size_t{};
		while (it != argument.end() && *it == L'\\') {
			++it;
			++backslashes;
		}
		if (it == argument.end()) {
			out.append( backslashes * 2, L'\\' );
			break;
		}
		if (*it == L'"') {
			out.append( backslashes * 2 + 1, L'\\' );
			out.push_back( L'"' );
		} else {
			out.append( backslashes, L'\\' );
			out.push_back( *it );
		}
	}
	out.push_back( L'"' );
	return out;
}

} // namespace


bool Subprocess::start ( const SubprocessSpec &spec, std::string *error )
{
	if (impl_->started)
		return spawn_fail( error, "process already started" );
	auto security = SECURITY_ATTRIBUTES{ sizeof( SECURITY_ATTRIBUTES ), nullptr, TRUE };
	HANDLE child_in = nullptr, child_out = nullptr, child_err = nullptr;
	const auto close_child_ends = [&] {
		for (auto *handle : { &child_in, &child_out, &child_err })
			if (*handle) {
				CloseHandle( *handle );
				*handle = nullptr;
			}
	};
	if (!CreatePipe( &child_in, &impl_->input, &security, 0 ) ||
		!CreatePipe( &impl_->output, &child_out, &security, 0 ) ||
		!CreatePipe( &impl_->errors, &child_err, &security, 0 ))
	{
		close_child_ends();
		impl_->close_handles();
		return spawn_fail( error, "cannot create pipes for the child process" );
	}
	for (auto *handle : { impl_->input, impl_->output, impl_->errors })
		SetHandleInformation( handle, HANDLE_FLAG_INHERIT, 0 );
//
// Hand the child exactly its three pipe ends: with several workers started from
// one parent, an inherited end of another worker's pipe would keep that pipe
// open after the parent closed its side, and the worker would never see EOF.
	auto attribute_bytes = SIZE_T{};
	InitializeProcThreadAttributeList( nullptr, 1, 0, &attribute_bytes );
	auto attribute_storage = std::vector<uint8_t>( attribute_bytes );
	auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( attribute_storage.data() );
	HANDLE inherited[] = { child_in, child_out, child_err };
	if (!InitializeProcThreadAttributeList( attributes, 1, 0, &attribute_bytes ) ||
		!UpdateProcThreadAttribute( attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			inherited, sizeof( inherited ), nullptr, nullptr ))
	{
		close_child_ends();
		impl_->close_handles();
		return spawn_fail( error, "cannot prepare the child's handle list" );
	}
	auto startup = STARTUPINFOEXW{};
	startup.StartupInfo.cb = sizeof( startup );
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = child_in;
	startup.StartupInfo.hStdOutput = child_out;
	startup.StartupInfo.hStdError = child_err;
	startup.lpAttributeList = attributes;

	auto command = quote_argument( spec.executable.wstring() );
	for (const auto &argument : spec.arguments)
		command += L" " + quote_argument( pyr::str2wstr( argument ) );
	auto environment = std::wstring{};
	for (const auto &entry : merged_environment( spec )) {
		environment += pyr::str2wstr( entry );
		environment.push_back( L'\0' );
	}
	environment.push_back( L'\0' );

	auto info = PROCESS_INFORMATION{};
	const auto created = CreateProcessW( nullptr, command.data(), nullptr, nullptr, TRUE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
		environment.data(), nullptr, &startup.StartupInfo, &info );
	const auto last_error = GetLastError();
	DeleteProcThreadAttributeList( attributes );
	close_child_ends();
	if (!created) {
		impl_->close_handles();
		return spawn_fail( error, std::format( "cannot start '{}' (Windows error {})",
			path_utf8( spec.executable ), last_error ) );
	}
	impl_->process = info.hProcess;
	impl_->job = CreateJobObjectW( nullptr, nullptr );
	if (impl_->job) {
		auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject( impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof( limits ) );
		AssignProcessToJobObject( impl_->job, impl_->process );
	}
	ResumeThread( info.hThread );
	CloseHandle( info.hThread );
	impl_->line = spec.stderr_line;
	impl_->started = true;
	impl_->drain = std::thread( [impl = impl_.get()] { impl->drain_errors(); } );
	return true;
}


bool Subprocess::write_all ( const void *bytes, size_t length, std::string *error )
{
	const auto *p = static_cast<const char *>( bytes );
	while (length > 0) {
		const auto chunk = (DWORD)std::min<size_t>( length, 1u << 24 );
		auto wrote = DWORD{};
		if (!impl_->input || !WriteFile( impl_->input, p, chunk, &wrote, nullptr ) || wrote == 0)
			return spawn_fail( error, "the child process stopped reading its input" +
				( stderr_tail().empty() ? std::string{} : ":\n" + stderr_tail() ) );
		p += wrote;
		length -= wrote;
	}
	return true;
}


bool Subprocess::read_exact ( void *bytes, size_t length, std::string *error )
{
	auto *p = static_cast<char *>( bytes );
	while (length > 0) {
		const auto chunk = (DWORD)std::min<size_t>( length, 1u << 24 );
		auto got = DWORD{};
		if (!impl_->output || !ReadFile( impl_->output, p, chunk, &got, nullptr ) || got == 0) {
			wait();
			return spawn_fail( error, "the child process ended without answering" +
				( stderr_tail().empty() ? std::string{} : ":\n" + stderr_tail() ) );
		}
		p += got;
		length -= got;
	}
	return true;
}


void Subprocess::read_until_end ( std::string *out )
{
	char buffer[4096];
	for (;;) {
		auto got = DWORD{};
		if (!impl_->output || !ReadFile( impl_->output, buffer, sizeof( buffer ), &got, nullptr ) || got == 0)
			return;
		out->append( buffer, got );
	}
}


void Subprocess::close_input ()
{
	if (impl_->input) {
		CloseHandle( impl_->input );
		impl_->input = nullptr;
	}
}


int Subprocess::wait ()
{
	if (!impl_->started)
		return -1;
	if (!impl_->exited && impl_->process) {
		WaitForSingleObject( impl_->process, INFINITE );
		auto code = DWORD{};
		impl_->exit_code = GetExitCodeProcess( impl_->process, &code ) ? (int)code : -1;
		impl_->exited = true;
	}
	if (impl_->drain.joinable())
		impl_->drain.join();
	return impl_->exit_code;
}


void Subprocess::kill ()
{
	if (impl_->process && running())
		TerminateProcess( impl_->process, 1 );
}


bool Subprocess::running () const
{
	return impl_->process && !impl_->exited && WaitForSingleObject( impl_->process, 0 ) == WAIT_TIMEOUT;
}

#else

bool Subprocess::start ( const SubprocessSpec &spec, std::string *error )
{
	if (impl_->started)
		return spawn_fail( error, "process already started" );
//
// A write to a worker that has died raises SIGPIPE, whose default action ends
// the whole host. Ignore it - write() then fails with EPIPE, which is reported -
// unless the host installed a handler of its own.
	static auto sigpipe_once = std::once_flag{};
	std::call_once( sigpipe_once, [] {
		struct sigaction current{};
		if (sigaction( SIGPIPE, nullptr, &current ) == 0 && current.sa_handler == SIG_DFL)
			signal( SIGPIPE, SIG_IGN );
	} );
	int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 }, err_pipe[2] = { -1, -1 };
	const auto close_all = [&] {
		for (auto *pair : { in_pipe, out_pipe, err_pipe })
			for (auto i = 0; i < 2; ++i)
				if (pair[i] >= 0)
					::close( pair[i] );
	};
//
// Workers start from several threads at once. pipe() and close-on-exec are two
// steps, so a spawn in another thread between them would hand that child these
// pipe ends, and this worker's EOF would never come. Pipes and spawn take turns.
	static auto spawn_mutex = std::mutex{};
	auto spawning = std::unique_lock( spawn_mutex );
	if (pipe( in_pipe ) || pipe( out_pipe ) || pipe( err_pipe )) {
		close_all();
		return spawn_fail( error, std::string( "cannot create pipes: " ) + std::strerror( errno ) );
	}
//
// Close-on-exec on every end: dup2 onto 0/1/2 clears it for the child's own
// copies, and no child started later inherits these pipes.
	for (auto *pair : { in_pipe, out_pipe, err_pipe })
		for (auto i = 0; i < 2; ++i)
			fcntl( pair[i], F_SETFD, FD_CLOEXEC );
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init( &actions );
	posix_spawn_file_actions_adddup2( &actions, in_pipe[0], 0 );
	posix_spawn_file_actions_adddup2( &actions, out_pipe[1], 1 );
	posix_spawn_file_actions_adddup2( &actions, err_pipe[1], 2 );

	const auto executable = spec.executable.string();
	auto argument_storage = std::vector<std::string>{ executable };
	argument_storage.insert( argument_storage.end(), spec.arguments.begin(), spec.arguments.end() );
	auto argv = std::vector<char *>{};
	for (auto &argument : argument_storage)
		argv.push_back( argument.data() );
	argv.push_back( nullptr );
	auto environment_storage = merged_environment( spec );
	auto envp = std::vector<char *>{};
	for (auto &entry : environment_storage)
		envp.push_back( entry.data() );
	envp.push_back( nullptr );

	auto pid = pid_t{};
	const auto status = posix_spawnp( &pid, executable.c_str(), &actions, nullptr, argv.data(), envp.data() );
	spawning.unlock();
	posix_spawn_file_actions_destroy( &actions );
	::close( in_pipe[0] );
	::close( out_pipe[1] );
	::close( err_pipe[1] );
	if (status != 0) {
		::close( in_pipe[1] );
		::close( out_pipe[0] );
		::close( err_pipe[0] );
		return spawn_fail( error, "cannot start '" + executable + "': " + std::strerror( status ) );
	}
	impl_->pid = pid;
	impl_->input = in_pipe[1];
	impl_->output = out_pipe[0];
	impl_->errors = err_pipe[0];
	impl_->line = spec.stderr_line;
	impl_->started = true;
	impl_->drain = std::thread( [impl = impl_.get()] { impl->drain_errors(); } );
	return true;
}


bool Subprocess::write_all ( const void *bytes, size_t length, std::string *error )
{
	const auto *p = static_cast<const char *>( bytes );
	while (length > 0) {
		const auto wrote = impl_->input >= 0 ? ::write( impl_->input, p, length ) : -1;
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return spawn_fail( error, "the child process stopped reading its input" +
				( stderr_tail().empty() ? std::string{} : ":\n" + stderr_tail() ) );
		p += wrote;
		length -= (size_t)wrote;
	}
	return true;
}


bool Subprocess::read_exact ( void *bytes, size_t length, std::string *error )
{
	auto *p = static_cast<char *>( bytes );
	while (length > 0) {
		const auto got = impl_->output >= 0 ? ::read( impl_->output, p, length ) : -1;
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			wait();
			return spawn_fail( error, "the child process ended without answering" +
				( stderr_tail().empty() ? std::string{} : ":\n" + stderr_tail() ) );
		}
		p += got;
		length -= (size_t)got;
	}
	return true;
}


void Subprocess::read_until_end ( std::string *out )
{
	char buffer[4096];
	for (;;) {
		const auto got = impl_->output >= 0 ? ::read( impl_->output, buffer, sizeof( buffer ) ) : -1;
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return;
		out->append( buffer, (size_t)got );
	}
}


void Subprocess::close_input ()
{
	if (impl_->input >= 0) {
		::close( impl_->input );
		impl_->input = -1;
	}
}


int Subprocess::wait ()
{
	if (!impl_->started)
		return -1;
	if (!impl_->exited && impl_->pid > 0) {
		auto status = 0;
		while (waitpid( impl_->pid, &status, 0 ) < 0 && errno == EINTR) {}
		impl_->exit_code = WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
		impl_->exited = true;
	}
	if (impl_->drain.joinable())
		impl_->drain.join();
	return impl_->exit_code;
}


void Subprocess::kill ()
{
	if (impl_->pid > 0 && running())
		::kill( impl_->pid, SIGKILL );
}


bool Subprocess::running () const
{
	if (impl_->pid <= 0 || impl_->exited)
		return false;
	auto status = 0;
	const auto result = waitpid( impl_->pid, &status, WNOHANG );
	if (result == impl_->pid) {
		impl_->exit_code = WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
		impl_->exited = true;
		return false;
	}
	return result == 0;
}

#endif


std::string Subprocess::stderr_tail () const
{
	auto guard = std::scoped_lock( impl_->tail_mutex );
	return impl_->tail;
}

} // namespace priml
