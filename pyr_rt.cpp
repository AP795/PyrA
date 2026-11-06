#include "pyr_rt.h"

#include "pyr_win.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace pyr {

namespace {

LogSink g_info_sink = nullptr;
LogSink g_error_sink = nullptr;

// Atomic because decode-pool workers read it while the host thread may set it.
std::atomic<FILE *> g_debug_file{ nullptr };

// Matches the host's error marker so a standalone build's stderr output reads
// the same way as a hosted one's log file.
constexpr const char *kErrorSignature = "##### ";


// Formats into a stack buffer, growing to the heap only when the message does
// not fit. Log lines here are tile diagnostics - almost always short.
void format_and_emit ( LogSink sink, bool error, const char *fmt, va_list args )
{
	char stack_buf[1024];
	va_list retry;
	va_copy( retry, args );
	auto needed = std::vsnprintf( stack_buf, sizeof( stack_buf ), fmt, args );

	if (needed < 0) {
		va_end( retry );
		return;
	}

	const char *msg = stack_buf;
	std::vector<char> heap_buf;
	if ((size_t)needed >= sizeof( stack_buf )) {
		heap_buf.resize( (size_t)needed + 1 );
		std::vsnprintf( heap_buf.data(), heap_buf.size(), fmt, retry );
		msg = heap_buf.data();
	}
	va_end( retry );

	if (sink) {
		sink( msg );
		return;
	}
	std::fputs( error ? kErrorSignature : "", stderr );
	std::fputs( msg, stderr );
}

} // namespace


void set_log_sink ( LogSink info, LogSink error ) noexcept
{
	g_info_sink = info;
	g_error_sink = error;
}


void set_debug_file ( FILE *file ) noexcept
{
	g_debug_file.store( file, std::memory_order_release );
}


FILE *debug_file () noexcept
{
	return g_debug_file.load( std::memory_order_acquire );
}


void plog ( const char *fmt, ... ) noexcept
{
	va_list args;
	va_start( args, fmt );
	format_and_emit( g_info_sink, false, fmt, args );
	va_end( args );
}


void plog_err ( const char *fmt, ... ) noexcept
{
	va_list args;
	va_start( args, fmt );
	format_and_emit( g_error_sink, true, fmt, args );
	va_end( args );
}


std::filesystem::path normalise_path ( const std::filesystem::path &path )
{
	if (path.empty())
		return {};

	auto normalized = path.lexically_normal();
	normalized.make_preferred();
	return normalized;
}


std::wstring str2wstr ( std::string_view s )
{
	if (s.empty())
		return {};
#ifdef _WIN32
	auto size = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(),
		nullptr, 0 );
	std::wstring result( size, 0 );
	MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), &result[0], size );
	return result;
#else
//
// Only the CZI loader needs this, for libCZI's wchar_t stream API. Byte-wise
// widening is right for ASCII; a non-ASCII UTF-8 path would need decoding.
	return std::wstring( s.begin(), s.end() );
#endif
}

} // namespace pyr
