// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "viewer_logging.h"
#include "viewer_settings.h"
#include "viewer_window.h"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QIcon>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <optional>

int main ( int argc, char **argv )
{
	auto log_path = QString{};
	auto log_error = QString{};
	const auto logging = pyra::test::start_viewer_log(
		QString::fromUtf8( PYRA_GUI_LOG_DIRECTORY ), &log_path, &log_error );
	auto application = QApplication( argc, argv );
	QApplication::setApplicationName( QStringLiteral( "Pyra test viewer" ) );
	application.setWindowIcon( QIcon( QStringLiteral( ":/pyra/pyra.ico" ) ) );
	if (!logging) {
		QMessageBox::critical( nullptr, QStringLiteral( "Pyra logging error" ), log_error );
		return 1;
	}
	qInfo().noquote() << "Viewer started. Log:" << log_path;
	auto font = application.font();
	font.setPointSizeF( std::max( 12.0, font.pointSizeF() ) );
	application.setFont( font );
	const auto arguments = application.arguments();
	const auto smoke = arguments.size() == 3 && arguments[1] == QStringLiteral( "--smoke-test" );
	if (smoke && application.windowIcon().pixmap( 32, 32 ).isNull()) {
		qCritical() << "Unable to decode the embedded Pyra application icon";
		return 1;
	}
	auto settings = std::optional<pyra::test::ViewerSettings>{};
	auto messages = QStringList{};
	if (!smoke) {
		settings.emplace();
		messages = settings->load();
	}
	auto window = pyra::test::ViewerWindow( settings ? settings->image_directory() : QString{} );
	if (settings)
		window.set_view_halo_percent( settings->view_halo_percent() );
	window.show();
	for (const auto &message : messages) {
		qWarning().noquote() << message;
		window.report_message( message );
	}
	if (smoke) {
		QTimer::singleShot( 0, &window, [&] { window.smoke_test( arguments[2] ); } );
	} else {
		QObject::connect( &application, &QCoreApplication::aboutToQuit, &window, [&] {
			settings->session_saved( window.session_for_exit() );
			settings->set_view_halo_percent( window.view_halo_percent() );
			auto error = QString{};
			if (!settings->save_status( &error ))
				qWarning().noquote() << error;
		} );
		const auto image = settings->startup_image( arguments.size() == 2 ? arguments[1] : QString{} );
		if (!image.isEmpty())
			QTimer::singleShot( 0, &window, [&window, image, saved = settings->recording()] {
				window.open( image, pyra::test::reader_options(), saved );
			} );
	}
	const auto code = application.exec();
	qInfo() << "Viewer exiting with code" << code;
	return code;
}
