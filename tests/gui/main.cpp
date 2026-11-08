#include "viewer_settings.h"
#include "viewer_window.h"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QTimer>

#include <algorithm>
#include <optional>

int main ( int argc, char **argv )
{
	auto application = QApplication( argc, argv );
	QApplication::setApplicationName( QStringLiteral( "Pyra test viewer" ) );
	auto font = application.font();
	font.setPointSizeF( std::max( 12.0, font.pointSizeF() ) );
	application.setFont( font );
	const auto arguments = application.arguments();
	const auto smoke = arguments.size() == 3 && arguments[1] == QStringLiteral( "--smoke-test" );
	auto settings = std::optional<pyra::test::ViewerSettings>{};
	auto messages = QStringList{};
	if (!smoke) {
		settings.emplace();
		messages = settings->load();
	}
	auto window = pyra::test::ViewerWindow( settings ? settings->image_directory() : QString{} );
	window.show();
	for (const auto &message : messages) {
		qWarning().noquote() << message;
		window.report_message( message );
	}
	if (smoke) {
		QTimer::singleShot( 0, &window, [&] { window.smoke_test( arguments[2] ); } );
	} else {
		QObject::connect( &application, &QCoreApplication::aboutToQuit, &window, [&] {
			settings->image_loaded( window.last_loaded_image() );
			auto error = QString{};
			if (!settings->save_status( &error ))
				qWarning().noquote() << error;
		} );
		const auto image = settings->startup_image( arguments.size() == 2 ? arguments[1] : QString{} );
		if (!image.isEmpty())
			QTimer::singleShot( 0, &window, [&window, image] { window.open( image ); } );
	}
	return application.exec();
}
