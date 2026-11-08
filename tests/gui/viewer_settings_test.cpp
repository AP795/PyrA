#include "viewer_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {

void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


void write_text ( const QString &path, const QByteArray &text )
{
	auto file = QFile( path );
	require( file.open( QIODevice::WriteOnly ) && file.write( text ) == text.size(), "Test setup write failed" );
}


QByteArray read_text ( const QString &path )
{
	auto file = QFile( path );
	require( file.open( QIODevice::ReadOnly ), "Test read failed" );
	return file.readAll();
}


void test_viewer_settings_first_start_creates_default_config ()
{
	auto directory = QTemporaryDir{};
	require( directory.isValid(), "Temporary directory failed" );
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Default load failed" );
	require( settings.image_directory() == QDir::home().filePath( QStringLiteral( "images" ) ),
		"Default image directory is incorrect" );
	require( settings.last_image().isEmpty(), "First start restored an image" );
	require( read_text( settings.config_path() ).contains( "image-path=~/images" ),
		"Default configuration was not created" );
	require( !QFile::exists( settings.status_path() ), "Loading created status before exit" );
}


void test_viewer_settings_home_expansion_and_config_preservation ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	const auto config = QByteArray( "; user-maintained\nimage-path=~/images with spaces\ncustom-key=keep\n" );
	write_text( settings.config_path(), config );
	require( settings.load().isEmpty(), "Config read failed" );
	require( settings.image_directory() == QDir::home().filePath( QStringLiteral( "images with spaces" ) ),
		"Home-relative image directory was not expanded" );
	settings.image_loaded( directory.filePath( QStringLiteral( "sample.ptf" ) ) );
	require( settings.save_status(), "Status save failed" );
	require( read_text( settings.config_path() ) == config, "Saving status changed user configuration" );
}


void test_viewer_settings_config_precedes_relative_status ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	write_text( settings.config_path(), "image-path=slides\n" );
	write_text( settings.status_path(), "last-image=sample.ptf\n" );
	require( settings.load().isEmpty(), "Load failed" );
	require( settings.last_image() == directory.filePath( QStringLiteral( "slides/sample.ptf" ) ),
		"Status was not resolved using the configuration read before it" );
}


void test_viewer_settings_unicode_path_survives_restart ()
{
	auto directory = QTemporaryDir{};
	const auto image = directory.filePath( QStringLiteral( "caf\u00e9, sample #1.ptf" ) );
	{
		auto settings = pyra::test::ViewerSettings( directory.path() );
		require( settings.load().isEmpty(), "Initial load failed" );
		settings.image_loaded( image );
		require( settings.save_status(), "Save failed" );
	}
	auto restarted = pyra::test::ViewerSettings( directory.path() );
	require( restarted.load().isEmpty() && restarted.startup_image() == image,
		"Restart did not preserve the Unicode/path punctuation" );
}


void test_viewer_settings_command_line_overrides_saved_image ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Load failed" );
	const auto saved = directory.filePath( QStringLiteral( "saved.ptf" ) );
	const auto explicit_path = directory.filePath( QStringLiteral( "explicit.ptf" ) );
	settings.image_loaded( saved );
	require( settings.startup_image( explicit_path ) == explicit_path && settings.startup_image() == saved,
		"Explicit startup image did not override saved status" );
}


void test_viewer_settings_failed_or_pending_load_retains_previous_image ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Load failed" );
	const auto previous = directory.filePath( QStringLiteral( "previous.ptf" ) );
	settings.image_loaded( previous );
	require( settings.save_status(), "Save failed" );
	auto restarted = pyra::test::ViewerSettings( directory.path() );
	require( restarted.load().isEmpty(), "Restart load failed" );
// The window reports no completed image when a startup load fails or is still pending.
	restarted.image_loaded( {} );
	require( restarted.save_status(), "Exit save failed" );
	auto again = pyra::test::ViewerSettings( directory.path() );
	require( again.load().isEmpty() && again.last_image() == previous, "Failed load replaced saved image" );
}


void test_viewer_settings_missing_status_can_save_empty_session ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty() && settings.save_status(), "Empty session failed" );
	auto restarted = pyra::test::ViewerSettings( directory.path() );
	require( restarted.load().isEmpty() && restarted.last_image().isEmpty(), "Empty session restored an image" );
}


void test_viewer_settings_malformed_config_is_reported_and_preserved ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	const auto broken = QByteArray( "[unclosed section\nimage-path=bad\n" );
	write_text( settings.config_path(), broken );
	const auto errors = settings.load();
	require( !errors.isEmpty() && errors.join( QLatin1Char( '\n' ) ).contains( settings.config_path() ),
		"Malformed config was not reported with its path" );
	require( read_text( settings.config_path() ) == broken, "Malformed config was overwritten" );
	require( settings.image_directory() == QDir::home().filePath( QStringLiteral( "images" ) ),
		"Malformed config did not retain a usable default directory" );
}


void test_viewer_settings_malformed_status_is_reported ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	write_text( settings.status_path(), "[unclosed section\nlast-image=bad\n" );
	const auto errors = settings.load();
	require( !errors.isEmpty() && settings.last_image().isEmpty(), "Malformed status was silently restored" );
}


void test_viewer_settings_status_write_failure_has_context ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Load failed" );
	require( QDir().mkpath( settings.status_path() ), "Setup failed" );
	auto error = QString{};
	require( !settings.save_status( &error ) && error.contains( settings.status_path() ),
		"Write failure did not return its status filename" );
}

} // namespace


int main ( int argc, char **argv )
{
	auto application = QCoreApplication( argc, argv );
	auto failed = 0;
	const auto run = [&failed] ( const char *name, const std::function<void()> &function ) {
		const auto start = std::chrono::steady_clock::now();
		auto passed = true;
		try {
			function();
		} catch (const std::exception &error) {
			std::cerr << name << ": " << error.what() << '\n';
			passed = false;
			++failed;
		}
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start ).count();
		std::cout << name << ": " << ( passed ? "PASS" : "FAIL" ) << " (" << us << " us)\n";
	};
	run( "test_viewer_settings_first_start_creates_default_config", test_viewer_settings_first_start_creates_default_config );
	run( "test_viewer_settings_home_expansion_and_config_preservation", test_viewer_settings_home_expansion_and_config_preservation );
	run( "test_viewer_settings_config_precedes_relative_status", test_viewer_settings_config_precedes_relative_status );
	run( "test_viewer_settings_unicode_path_survives_restart", test_viewer_settings_unicode_path_survives_restart );
	run( "test_viewer_settings_command_line_overrides_saved_image", test_viewer_settings_command_line_overrides_saved_image );
	run( "test_viewer_settings_failed_or_pending_load_retains_previous_image", test_viewer_settings_failed_or_pending_load_retains_previous_image );
	run( "test_viewer_settings_missing_status_can_save_empty_session", test_viewer_settings_missing_status_can_save_empty_session );
	run( "test_viewer_settings_malformed_config_is_reported_and_preserved", test_viewer_settings_malformed_config_is_reported_and_preserved );
	run( "test_viewer_settings_malformed_status_is_reported", test_viewer_settings_malformed_status_is_reported );
	run( "test_viewer_settings_status_write_failure_has_context", test_viewer_settings_status_write_failure_has_context );
	return failed == 0 ? 0 : 1;
}
