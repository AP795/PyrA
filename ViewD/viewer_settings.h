// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "viewer_recording.h"

#include <QString>
#include <QStringList>

namespace pyra::test {

class ViewerSettings {
public:
	explicit ViewerSettings ( const QString &directory = default_directory() );
	static QString default_directory ();
	static QString default_image_directory ();
	QStringList load ();
	QString config_path () const;
	QString status_path () const;
	const QString &image_directory () const { return image_directory_; }
	const QString &last_image () const { return last_image_; }
	QString startup_image ( const QString &explicit_image = {} ) const;
	void image_loaded ( const QString &path );
	void session_saved ( ViewerRecording recording );
	const ViewerRecording &recording () const { return recording_; }
	bool save_status ( QString *error = nullptr ) const;

private:
	QString directory_;
	QString image_directory_;
	QString last_image_;
	ViewerRecording recording_;
};

} // namespace pyra::test
