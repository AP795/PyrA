// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "render_evaluator.h"
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
	int view_halo_percent () const { return view_halo_percent_; }
	void set_view_halo_percent ( int percent );
	bool save_status ( QString *error = nullptr ) const;

private:
	QString directory_;
	QString image_directory_;
	QString last_image_;
	ViewerRecording recording_;
	int view_halo_percent_ = kViewHaloDefaultPercent;
};

} // namespace pyra::test
