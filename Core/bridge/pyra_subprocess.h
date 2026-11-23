// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// A child process with its three standard streams on pipes.
//
// Built for the Python bridge: binary messages go out on the child's stdin and
// come back on its stdout, while its stderr is drained on a thread of its own
// and handed over line by line. Draining is not optional - a child that writes
// more stderr than the pipe buffer holds blocks until someone reads it, and the
// parent waiting on its stdout would then wait forever.
//
// On Windows the child is placed in a job that kills it when the parent goes
// away. On POSIX it is not killed, but every bridge worker exits when its stdin
// reaches end of file, which is what the parent's death produces.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace priml {

// Bytes of the child's stderr kept for error messages when it dies unannounced.
inline constexpr size_t kSubprocessStderrTailBytes = 8192;


struct SubprocessSpec {
	std::filesystem::path executable;		// searched on PATH when it has no directory
	std::vector<std::string> arguments;		// UTF-8, without the executable itself
	std::vector<std::pair<std::string, std::string>> environment;	// added to or replacing the parent's
// Receives each complete stderr line, without its line ending, on the drain thread.
	std::function<void ( const std::string & )> stderr_line;
};


class Subprocess {
public:
	Subprocess ();
	~Subprocess ();
	Subprocess ( const Subprocess & ) = delete;
	Subprocess &operator= ( const Subprocess & ) = delete;

	bool start ( const SubprocessSpec &spec, std::string *error );
	bool write_all ( const void *bytes, size_t length, std::string *error );
//
// False on end of file or a read error; the error then names the child's last
// stderr output, which is usually why it stopped talking.
	bool read_exact ( void *bytes, size_t length, std::string *error );
//
// Everything the child writes to stdout until it closes it (usually: exits).
	void read_until_end ( std::string *out );
//
// Closes the child's stdin, so a worker reading it sees end of file.
	void close_input ();
// Waits for the child to exit; returns its exit code, or -1.
	int wait ();
	void kill ();
	[[nodiscard]] bool running () const;
	[[nodiscard]] std::string stderr_tail () const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace priml
