// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

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
	require( QDir::isAbsolutePath( settings.image_directory() )
		&& settings.image_directory() == QString::fromUtf8( PYRA_GUI_IMAGE_DIRECTORY ),
		"Default image directory is incorrect" );
	require( settings.last_image().isEmpty(), "First start restored an image" );
	require( read_text( settings.config_path() ).contains( settings.image_directory().toUtf8() ),
		"Default configuration was not created" );
	require( !QFile::exists( settings.status_path() ), "Loading created status before exit" );
}


void test_viewer_settings_legacy_default_uses_configured_directory ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	write_text( settings.config_path(), "image-path=~/images\n" );
	require( settings.load().isEmpty(), "Legacy config load failed" );
	require( settings.image_directory() == pyra::test::ViewerSettings::default_image_directory(),
		"Legacy default did not use the configured image directory" );
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
	require( settings.image_directory() == pyra::test::ViewerSettings::default_image_directory(),
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

pyra::test::ViewerRecording sample_recording ( const QString &source )
{
	auto result = pyra::test::ViewerRecording{};
	result.source = source;
	result.source_size = 123456;
	result.source_modified_ms = 1794830400123;
	const auto plane = pyra::test::PlaneSelection{ QStringLiteral( "DAPI \u03b1" ), -1.25 };
	result.planes.push_back( { plane, { -100.25, 20, 1000, 800 }, 0.25f, {}, 0, PYRA_PIXEL_GRAY16, 4095 } );
	const auto request = pyra::test::RenderRequest{ { -3.123456789012345, 7.123456789012345, 31.25, 29.5 },
		0.123456789012345, plane };
	result.requests = { request, request };
	result.requests.push_back( { { 21.5, -19.25, 400, 200 }, 2.75, plane } );
	return result;
}


void test_viewer_settings_recording_round_trip ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Load failed" );
	const auto original = sample_recording( directory.filePath( QStringLiteral( "caf\u00e9 #1.ptf" ) ) );
	settings.session_saved( original );
	require( settings.save_status(), "Recording save failed" );
	auto restarted = pyra::test::ViewerSettings( directory.path() );
	require( restarted.load().isEmpty(), "Recording reload failed" );
	const auto &saved = restarted.recording();
	require( saved.requests == original.requests && saved.source == original.source &&
		saved.source_size == original.source_size && saved.source_modified_ms == original.source_modified_ms,
		"Status round trip changed coordinates, MPP, planes, order, duplicates or source identity" );
	require( saved.planes.size() == 1 && saved.planes[0].bounds == original.planes[0].bounds &&
		saved.planes[0].pixel_format == PYRA_PIXEL_GRAY16, "Status lost saved plane geometry" );
}


void test_viewer_settings_empty_recording_clears_previous_workload ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Load failed" );
	auto session = sample_recording( directory.filePath( QStringLiteral( "sample.ptf" ) ) );
	settings.session_saved( session );
	require( settings.save_status(), "Initial recording save failed" );
	session.requests.clear();
	settings.session_saved( session );
	require( settings.save_status(), "Empty recording save failed" );
	auto restarted = pyra::test::ViewerSettings( directory.path() );
	require( restarted.load().isEmpty() && restarted.recording().requests.empty() &&
		restarted.last_image() == session.source, "Cleared recording reappeared after restart" );
}


void test_viewer_settings_failed_startup_retains_recording ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	require( settings.load().isEmpty(), "Load failed" );
	const auto original = sample_recording( directory.filePath( QStringLiteral( "sample.ptf" ) ) );
	settings.session_saved( original );
	require( settings.save_status(), "Initial recording save failed" );
	auto restarted = pyra::test::ViewerSettings( directory.path() );
	require( restarted.load().isEmpty(), "Restart failed" );
	restarted.session_saved( {} );
	require( restarted.save_status(), "Failed-startup exit save failed" );
	auto again = pyra::test::ViewerSettings( directory.path() );
	require( again.load().isEmpty() && again.recording().requests == original.requests,
		"Failed startup discarded the previous recording" );
}


void test_viewer_settings_invalid_recording_is_reported ()
{
	auto directory = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( directory.path() );
	write_text( settings.status_path(), "last-image=sample.ptf\nrecording=not-json\n" );
	const auto errors = settings.load();
	require( !errors.isEmpty() && errors.join( QLatin1Char( '\n' ) ).contains( settings.status_path() ) &&
		settings.recording().requests.empty() && !settings.last_image().isEmpty(),
		"Invalid recording did not preserve image startup and report a contextual error" );
	auto invalid = sample_recording( QStringLiteral( "source.ptf" ) );
	invalid.requests[0].mpp = 0;
	auto decoded = pyra::test::ViewerRecording{};
	auto error = QString{};
	require( !pyra::test::decode_recording( pyra::test::encode_recording( invalid ), &decoded, &error ),
		"Invalid recorded MPP was accepted" );
	invalid.requests[0].mpp = 1;
	invalid.requests[0].plane.channel = QStringLiteral( "missing" );
	require( !pyra::test::decode_recording( pyra::test::encode_recording( invalid ), &decoded, &error ),
		"A recording referring to an unsaved plane was accepted" );
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
	run( "test_viewer_settings_legacy_default_uses_configured_directory", test_viewer_settings_legacy_default_uses_configured_directory );
	run( "test_viewer_settings_home_expansion_and_config_preservation", test_viewer_settings_home_expansion_and_config_preservation );
	run( "test_viewer_settings_config_precedes_relative_status", test_viewer_settings_config_precedes_relative_status );
	run( "test_viewer_settings_unicode_path_survives_restart", test_viewer_settings_unicode_path_survives_restart );
	run( "test_viewer_settings_command_line_overrides_saved_image", test_viewer_settings_command_line_overrides_saved_image );
	run( "test_viewer_settings_failed_or_pending_load_retains_previous_image", test_viewer_settings_failed_or_pending_load_retains_previous_image );
	run( "test_viewer_settings_missing_status_can_save_empty_session", test_viewer_settings_missing_status_can_save_empty_session );
	run( "test_viewer_settings_malformed_config_is_reported_and_preserved", test_viewer_settings_malformed_config_is_reported_and_preserved );
	run( "test_viewer_settings_malformed_status_is_reported", test_viewer_settings_malformed_status_is_reported );
	run( "test_viewer_settings_status_write_failure_has_context", test_viewer_settings_status_write_failure_has_context );
	run( "test_viewer_settings_recording_round_trip", test_viewer_settings_recording_round_trip );
	run( "test_viewer_settings_empty_recording_clears_previous_workload", test_viewer_settings_empty_recording_clears_previous_workload );
	run( "test_viewer_settings_failed_startup_retains_recording", test_viewer_settings_failed_startup_retains_recording );
	run( "test_viewer_settings_invalid_recording_is_reported", test_viewer_settings_invalid_recording_is_reported );
	return failed == 0 ? 0 : 1;
}
