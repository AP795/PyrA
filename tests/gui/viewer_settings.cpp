// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "viewer_settings.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <utility>

namespace pyra::test {
namespace {

constexpr auto kConfigFile = "pyra_gui_test.cfg";
constexpr auto kStatusFile = "pyra_gui_test.sts";
constexpr auto kImagePath = "image-path";
constexpr auto kLastImage = "last-image";
constexpr auto kRecording = "recording";

QString resolve_path ( QString value, const QString &base )
{
	if (value.isEmpty())
		return {};
	if (value == QStringLiteral( "~" ))
		value = QDir::homePath();
	else if (value.startsWith( QStringLiteral( "~/" ) ))
		value = QDir::home().filePath( value.mid( 2 ) );
	return QDir::cleanPath( QDir::isAbsolutePath( value ) ? value : QDir( base ).absoluteFilePath( value ) );
}


QString settings_error ( const QString &path, QSettings::Status status )
{
	return status == QSettings::FormatError
		? QStringLiteral( "Invalid configuration/status syntax: %1" ).arg( path )
		: QStringLiteral( "Unable to read or atomically write settings: %1" ).arg( path );
}

} // namespace


ViewerSettings::ViewerSettings ( const QString &directory )
	: directory_( QFileInfo( directory ).absoluteFilePath() )
{
}


QString ViewerSettings::default_directory ()
{
	return QDir( QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) )
		.filePath( QStringLiteral( "pyra_gui_test" ) );
}


QString ViewerSettings::default_image_directory ()
{
	return QString::fromUtf8( PYRA_GUI_IMAGE_DIRECTORY );
}


QString ViewerSettings::config_path () const
{
	return QDir( directory_ ).filePath( QLatin1String( kConfigFile ) );
}


QString ViewerSettings::status_path () const
{
	return QDir( directory_ ).filePath( QLatin1String( kStatusFile ) );
}


QStringList ViewerSettings::load ()
{
	auto errors = QStringList{};
	image_directory_ = default_image_directory();
	last_image_.clear();
	recording_ = {};
	if (!QDir().mkpath( directory_ ))
		errors.append( QStringLiteral( "Unable to create settings directory: %1" ).arg( directory_ ) );
	{
		auto config = QSettings( config_path(), QSettings::IniFormat );
		config.setAtomicSyncRequired( true );
		if (!QFileInfo::exists( config_path() )) {
			config.setValue( QLatin1String( kImagePath ), default_image_directory() );
			config.sync();
		}
		const auto value = config.value( QLatin1String( kImagePath ), default_image_directory() ).toString();
		if (config.status() != QSettings::NoError)
			errors.append( settings_error( config_path(), config.status() ) );
		else if (!value.isEmpty() && value != QStringLiteral( "~/images" ))
			image_directory_ = resolve_path( value, directory_ );
	}
// Status is read after config, so relative saved paths use the configured image directory.
	if (QFileInfo::exists( status_path() )) {
		auto status = QSettings( status_path(), QSettings::IniFormat );
		const auto value = status.value( QLatin1String( kLastImage ) ).toString();
		if (status.status() != QSettings::NoError)
			errors.append( settings_error( status_path(), status.status() ) );
		else {
			last_image_ = resolve_path( value, image_directory_ );
			if (status.contains( QLatin1String( kRecording ) )) {
				auto error = QString{};
				if (!decode_recording( status.value( QLatin1String( kRecording ) ).toByteArray(), &recording_, &error ))
					errors.append( QStringLiteral( "%1: %2" ).arg( status_path(), error ) );
			}
		}
	}
	return errors;
}


QString ViewerSettings::startup_image ( const QString &explicit_image ) const
{
	return explicit_image.isEmpty() ? last_image_ : resolve_path( explicit_image, QDir::currentPath() );
}


void ViewerSettings::image_loaded ( const QString &path )
{
	if (!path.isEmpty()) {
		const auto resolved = resolve_path( path, QDir::currentPath() );
		if (last_image_ != resolved)
			recording_ = {};
		last_image_ = resolved;
	}
}


void ViewerSettings::session_saved ( ViewerRecording recording )
{
// An unsuccessful startup leaves the previous on-disk session available.
	if (recording.source.isEmpty())
		return;
	image_loaded( recording.source );
	recording_ = std::move( recording );
}


bool ViewerSettings::save_status ( QString *error ) const
{
	if (!QDir().mkpath( directory_ )) {
		if (error)
			*error = QStringLiteral( "Unable to create status directory: %1" ).arg( directory_ );
		return false;
	}
	auto status = QSettings( status_path(), QSettings::IniFormat );
	status.setAtomicSyncRequired( true );
	status.setValue( QLatin1String( kLastImage ), last_image_ );
	if (recording_.requests.empty())
		status.remove( QLatin1String( kRecording ) );
	else
		status.setValue( QLatin1String( kRecording ), encode_recording( recording_ ) );
	status.sync();
	if (status.status() != QSettings::NoError) {
		if (error)
			*error = settings_error( status_path(), status.status() );
		return false;
	}
	return true;
}

} // namespace pyra::test
