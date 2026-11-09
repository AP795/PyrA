#include "viewer_window.h"
#include "viewport.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QStatusBar>
#include <QThread>

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

constexpr auto kWaitLimitMs = 10000;
constexpr auto kPanEvents = 20;
constexpr auto kPanIntervalMs = 10;

void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


void pump ( int milliseconds )
{
	auto timer = QElapsedTimer{};
	timer.start();
	while (timer.elapsed() < milliseconds) {
		QCoreApplication::processEvents( QEventLoop::AllEvents, 5 );
		QThread::msleep( 1 );
	}
}


template <typename F>
void wait_for ( F &&ready, const char *message )
{
	auto timer = QElapsedTimer{};
	timer.start();
	while (!ready() && timer.elapsed() < kWaitLimitMs)
		pump( 5 );
	require( ready(), message );
}


struct Controls {
	QAction *record_ = nullptr;
	QAction *stop_ = nullptr;
	QAction *native_ = nullptr;
	QPlainTextEdit *report_ = nullptr;
};


Controls controls ( pyra::test::ViewerWindow &window )
{
	auto result = Controls{};
	for (auto *action : window.findChildren<QAction *>()) {
		if (action->text() == QStringLiteral( "Record" )) result.record_ = action;
		if (action->text() == QStringLiteral( "Stop" )) result.stop_ = action;
		if (action->text() == QStringLiteral( "1:1" )) result.native_ = action;
	}
	for (auto *edit : window.findChildren<QPlainTextEdit *>())
		if (edit->accessibleName() == QStringLiteral( "Performance report" )) result.report_ = edit;
	require( result.record_ && result.stop_ && result.native_ && result.report_, "Viewer controls are missing" );
	return result;
}


int record_pan ( pyra::test::ViewerWindow &window, const Controls &ui, double pixels )
{
	auto *viewport = window.findChild<pyra::test::Viewport *>();
	require( viewport != nullptr, "Viewport is missing" );
	ui.record_->trigger();
	wait_for( [&] { return window.statusBar()->currentMessage().contains( QStringLiteral( "recorded: 1" ) ); },
		"Initial recorded render did not finish" );
	for (auto event = 0; event < kPanEvents; ++event) {
		viewport->pan( { pixels, 0 } );
		pump( kPanIntervalMs );
	}
	ui.stop_->trigger();
	wait_for( [&] { return ui.report_->toPlainText().startsWith( QStringLiteral( "Stopped:" ) ); },
		"Stop did not finish" );
	const auto match = QRegularExpression( QStringLiteral( "Stopped: (\\d+)" ) ).match( ui.report_->toPlainText() );
	require( match.hasMatch(), "Recorded call count is missing" );
	return match.captured( 1 ).toInt();
}

} // namespace


int main ( int argc, char **argv )
{
	auto application = QApplication( argc, argv );
	if (argc != 2)
		return 2;
	try {
		auto window = pyra::test::ViewerWindow{};
		window.resize( 1280, 900 );
		window.show();
		const auto ui = controls( window );
		const auto directory = QDir( QString::fromLocal8Bit( argv[1] ) );
		window.open( directory.filePath( QStringLiteral( "coarse.ptf" ) ) );
		wait_for( [&] { return ui.native_->isEnabled(); }, "Synthetic slide did not open" );
		ui.native_->trigger();
		wait_for( [&] { return window.statusBar()->currentMessage().contains( QStringLiteral( " ms |" ) ); },
			"Native-resolution view did not render" );
		pump( 50 );
		const auto reused = record_pan( window, ui, 1 );
		require( reused == 1, "Small pans did not reuse the prepared GUI image" );
		const auto refreshed = record_pan( window, ui, 24 );
		require( refreshed > 1, "Continuous motion starved fresh rendering beyond prepared coverage" );
		window.open( directory.filePath( QStringLiteral( "unsupported.ptf" ) ) );
		wait_for( [&] {
			return ui.report_->toPlainText().contains( QStringLiteral( "Unsupported compression" ) ) &&
				ui.report_->toPlainText().contains( QStringLiteral( "JPEG-LS" ) );
		}, "Unsupported compression was not shown to the user" );
		std::cout << "Prepared reuse=" << reused << " continuous refresh=" << refreshed
			<< "; unsupported compression visible: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
