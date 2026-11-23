// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "viewer_logging.h"

#include <pyra/pyra.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

constexpr auto kThreadCount = 4;
constexpr auto kRecordsPerThread = 16;


void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


int write_child_log ( const QString &directory, const QString &fixture )
{
	auto path = QString{};
	auto error = QString{};
	if (!pyra::test::start_viewer_log( directory, &path, &error ))
		throw std::runtime_error( error.toStdString() );
	std::fprintf( stdout, "stdout marker\n" );
	std::fprintf( stderr, "stderr marker\n" );
	qWarning().noquote() << QStringLiteral( "Qt warning \u03bc\u03a9" );
	{
		auto reader = QFile( path );
		require( reader.open( QIODevice::ReadOnly ) && reader.readAll().contains( "stderr marker\n" ),
			"Session log cannot be read immediately while logging is active" );
	}
	auto threads = std::vector<std::thread>{};
	for (auto worker = 0; worker < kThreadCount; ++worker) {
		threads.emplace_back( [worker] {
			for (auto index = 0; index < kRecordsPerThread; ++index)
				qInfo().noquote() << QStringLiteral( "record-%1-%2" ).arg( worker ).arg( index );
		} );
	}
	for (auto &thread : threads)
		thread.join();
	auto *raw_context = static_cast<pyra_context *>( nullptr );
	require( pyra_context_create( nullptr, &raw_context, nullptr ) == PYRA_OK,
		"Cannot create the native logging fixture context" );
	const auto context = std::unique_ptr<pyra_context, void (*)(pyra_context *)>( raw_context,
		[] ( pyra_context *value ) { (void)pyra_context_destroy( value, nullptr ); } );
	auto *raw_wsi = static_cast<pyra_wsi *>( nullptr );
	require( pyra_open( context.get(), fixture.toUtf8().constData(), &raw_wsi, nullptr ) == PYRA_OK,
		"Cannot open the native logging fixture" );
	const auto wsi = std::unique_ptr<pyra_wsi, void (*)(pyra_wsi *)>( raw_wsi,
		[] ( pyra_wsi *value ) { (void)pyra_close( value, nullptr ); } );
	return 0;
}


void run_child ( const QStringList &arguments )
{
	auto child = QProcess{};
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.remove( QStringLiteral( "QT_MESSAGE_PATTERN" ) );
	environment.remove( QStringLiteral( "QT_LOGGING_RULES" ) );
	child.setProcessEnvironment( environment );
	child.start( QCoreApplication::applicationFilePath(), arguments );
	if (!child.waitForFinished( 15000 )) {
		child.kill();
		child.waitForFinished();
		throw std::runtime_error( "Logging child timed out" );
	}
	require( child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
		"Logging child failed" );
}


void test_viewer_logging_captures_diagnostics ( const QString &directory, const QString &fixture )
{
	auto temporary = QTemporaryDir( QDir( directory ).filePath( QStringLiteral( "unicode-\u03bc-XXXXXX" ) ) );
	require( temporary.isValid(), "Cannot create the logging test directory" );
	run_child( { QStringLiteral( "--write-log" ), temporary.path(), fixture } );
	const auto files = QDir( temporary.path() ).entryList( { QStringLiteral( "*.log" ) }, QDir::Files );
	require( files.size() == 1, "Expected one session log" );
	auto file = QFile( QDir( temporary.path() ).filePath( files.front() ) );
	require( file.open( QIODevice::ReadOnly ), "Cannot read the session log" );
	const auto content = file.readAll();
	require( content.contains( "stdout marker\n" ) && content.contains( "stderr marker\n" ) &&
		content.contains( QStringLiteral( "[warning] Qt warning \u03bc\u03a9\n" ).toUtf8() ) &&
		content.contains( "wsi pyramid: added 1 synthetic level(s)" ),
		"Session log lost stdout, stderr, Qt or native reader diagnostics" );
	require( !content.contains( '\r' ) && QString::fromUtf8( content ).toUtf8() == content,
		"Session log is not UTF-8 text with LF endings" );
	for (auto worker = 0; worker < kThreadCount; ++worker)
		for (auto index = 0; index < kRecordsPerThread; ++index)
			require( content.count( QStringLiteral( "[info] record-%1-%2\n" ).arg( worker ).arg( index ).toUtf8() ) == 1,
				"Concurrent Qt records were interleaved, lost or duplicated" );
}


void test_viewer_logging_rejects_invalid_directory ( const QString &directory )
{
	const auto path = QDir( directory ).filePath( QStringLiteral( "blocked-directory" ) );
	auto file = QFile( path );
	require( file.open( QIODevice::WriteOnly ), "Cannot prepare the blocked log directory" );
	file.close();
	run_child( { QStringLiteral( "--expect-log-failure" ), path } );
}

} // namespace


int main ( int argc, char **argv )
{
	auto application = QCoreApplication( argc, argv );
	const auto arguments = application.arguments();
	try {
		if (arguments.size() == 4 && arguments[1] == QStringLiteral( "--write-log" ))
			return write_child_log( arguments[2], arguments[3] );
		if (arguments.size() == 3 && arguments[1] == QStringLiteral( "--expect-log-failure" )) {
			auto path = QString{};
			auto error = QString{};
			require( !pyra::test::start_viewer_log( arguments[2], &path, &error ) &&
				!error.isEmpty(), "Invalid log directory was accepted without an error" );
			return 0;
		}
		require( arguments.size() == 3, "Usage: pyra_logging_test output-directory fixture" );
		require( QDir().mkpath( arguments[1] ), "Cannot create the logging test output directory" );
		const auto start = std::chrono::steady_clock::now();
		test_viewer_logging_captures_diagnostics( arguments[1], arguments[2] );
		std::cout << "test_viewer_logging_captures_diagnostics: PASS ("
			<< std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - start ).count() << " us)\n";
		const auto failure_start = std::chrono::steady_clock::now();
		test_viewer_logging_rejects_invalid_directory( arguments[1] );
		std::cout << "test_viewer_logging_rejects_invalid_directory: PASS ("
			<< std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - failure_start ).count() << " us)\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
