// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "viewer_logging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QtGlobal>

#include <cerrno>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pyra::test {
namespace {

bool redirect_stream ( FILE *stream, const QString &path, QString *error )
{
#ifdef _WIN32
	auto *redirected = static_cast<FILE *>( nullptr );
	const auto code = _wfreopen_s( &redirected, path.toStdWString().c_str(), L"wb", stream );
#else
	const auto *redirected = std::freopen( QFile::encodeName( path ).constData(), "wb", stream );
	const auto code = redirected ? 0 : errno;
#endif
	if (!redirected || code != 0) {
		if (error)
			*error = QStringLiteral( "Cannot open log file '%1': %2" ).arg( path,
				QString::fromLocal8Bit( std::error_code( code ? code : EIO,
					std::generic_category() ).message().c_str() ) );
		return false;
	}
	if (std::setvbuf( stream, nullptr, _IONBF, 0 ) != 0) {
		if (error)
			*error = QStringLiteral( "Cannot enable immediate writes to log file '%1'." ).arg( path );
		return false;
	}
	return true;
}


void write_qt_message ( QtMsgType type, const QMessageLogContext &context, const QString &message )
{
	const auto record = qFormatLogMessage( type, context, message ).toUtf8() + '\n';
// One stdio write keeps concurrent Qt records together. The unbuffered stream
// also captures diagnostics emitted directly by the reader DLL's shared CRT.
	(void)std::fwrite( record.constData(), 1, size_t( record.size() ), stderr );
	(void)std::fflush( stderr );
}

} // namespace


bool start_viewer_log ( const QString &directory, QString *path, QString *error )
{
	if (path)
		path->clear();
	if (error)
		error->clear();
	const auto folder = QDir( directory );
	if (!folder.mkpath( QStringLiteral( "." ) )) {
		if (error)
			*error = QStringLiteral( "Cannot create log directory '%1'." ).arg( folder.absolutePath() );
		return false;
	}
	const auto filename = QStringLiteral( "pyra-gui-%1-%2.log" )
		.arg( QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd-HHmmss-zzz" ) ) )
		.arg( QCoreApplication::applicationPid() );
	const auto target = folder.absoluteFilePath( filename );
	auto log = QFile( target );
	if (!log.open( QIODevice::WriteOnly | QIODevice::Truncate )) {
		if (error)
			*error = QStringLiteral( "Cannot open log file '%1': %2" ).arg( target, log.errorString() );
		return false;
	}
	const auto descriptor = log.handle();
	if (descriptor < 0) {
		if (error)
			*error = QStringLiteral( "Cannot obtain a file descriptor for log '%1'." ).arg( target );
		return false;
	}
#ifdef _WIN32
	const auto null_device = QStringLiteral( "NUL" );
#else
	const auto null_device = QStringLiteral( "/dev/null" );
#endif
// QFile permits live readers. Its descriptor is shared with both standard
// streams, which may initially be invalid in a Windows GUI process.
	for (auto *stream : { stderr, stdout }) {
		if (!redirect_stream( stream, null_device, error ))
			return false;
#ifdef _WIN32
		const auto duplicated = _dup2( descriptor, _fileno( stream ) );
#else
		const auto duplicated = dup2( descriptor, fileno( stream ) );
#endif
		if (duplicated < 0) {
			if (error)
				*error = QStringLiteral( "Cannot share log output for '%1': %2" ).arg( target,
					QString::fromLocal8Bit( std::error_code( errno, std::generic_category() ).message().c_str() ) );
			return false;
		}
	}
	qSetMessagePattern( QStringLiteral( "%{time yyyy-MM-dd HH:mm:ss.zzz} [%{type}] %{message}" ) );
	qInstallMessageHandler( write_qt_message );
	if (path)
		*path = target;
	return true;
}

} // namespace pyra::test
