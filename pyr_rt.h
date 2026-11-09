#pragma once

// Host services the pyramid library needs but does not want to own.
//
// The library is meant to build and run standalone, so it carries its own
// minimal versions of these and lets an embedding application redirect the ones
// that matter. wsia installs its logging here at startup; a bare build writes to
// stderr and works. Nothing here is a symbol the host has to define.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace pyr {

using version_t = uint64_t;

// Monotonic counter for cache invalidation.
//
// Deliberately private to this library and NOT shared with the host's version
// domain. Every value it produces is compared only against another value from
// this same counter - Tile::last_use for LRU eviction order, and Pwhl's
// pixels_ver_/lohi_ver_ pair for "is the cached lo/hi stale". None of them is
// persisted or compared against a scene version manifest, so a separate counter
// cannot desynchronise anything. Do not wire this to the host's generator on the
// assumption that "versions should match" - they are different domains.
//
// Header-inline, like the host's own generator: Pwhl stamps versions from
// header code, so making this a link-time symbol would force every project that
// merely includes pwhl.h to also compile pyr_rt.cpp.
inline std::atomic<version_t> g_version_counter{ 1 };

inline version_t next_version () noexcept
{
	return g_version_counter.fetch_add( 1, std::memory_order_relaxed ) + 1;
}


// Log sink. Receives a fully formatted message, newline included, exactly as the
// call site wrote it. The error sink is handed the message unprefixed so a host
// can apply its own severity marker.
using LogSink = void ( * ) ( const char *msg );

// Install host logging. Either may be null to keep the stderr default.
void set_log_sink ( LogSink info, LogSink error ) noexcept;

void plog ( const char *fmt, ... ) noexcept;

void plog_err ( const char *fmt, ... ) noexcept;


// Verbose trace file: per-IFD, per-tile and render-selection detail that is too
// heavy for the log sink, written with fprintf and flushed per record. Null, the
// default, turns it off. The host owns the FILE - clear it here before closing
// it. Replaces the host-defined `extern FILE *g_debugFile` the loaders used to
// reach for, which made every executable linking the library define one.
void set_debug_file ( FILE *file ) noexcept;

FILE *debug_file () noexcept;


// Lexically normalised, preferred-separator form of `path`. Used to key the
// session registry so two spellings of one slide resolve to one session.
std::filesystem::path normalise_path ( const std::filesystem::path &path );

inline std::filesystem::path path_from_utf8 ( std::string_view text )
{
	return std::filesystem::path( std::u8string( text.begin(), text.end() ) );
}

// UTF-8 to wide, for libCZI's wchar_t file API and the Win32 calls in the CZI
// loader. UTF-16 on Windows; elsewhere a byte-wise widening, correct only for
// ASCII paths.
std::wstring str2wstr ( std::string_view s );

} // namespace pyr
