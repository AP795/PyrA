#include "open_wsi_dialog.h"
#include "viewer_window.h"
#include "viewport.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QThread>
#include <QTextCursor>

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
	QAction *replay_ = nullptr;
	QPlainTextEdit *report_ = nullptr;
};


Controls controls ( pyra::test::ViewerWindow &window )
{
	auto result = Controls{};
	for (auto *action : window.findChildren<QAction *>()) {
		if (action->text() == QStringLiteral( "Record" )) result.record_ = action;
		if (action->text() == QStringLiteral( "Stop" )) result.stop_ = action;
		if (action->text() == QStringLiteral( "1:1" )) result.native_ = action;
		if (action->text() == QStringLiteral( "Replay" )) result.replay_ = action;
	}
	for (auto *edit : window.findChildren<QPlainTextEdit *>())
		if (edit->accessibleName() == QStringLiteral( "Performance report" )) result.report_ = edit;
	require( result.record_ && result.stop_ && result.native_ && result.replay_ && result.report_, "Viewer controls are missing" );
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
		auto backend = pyra_backend_info{ sizeof( pyra_backend_info ) };
		pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &backend, nullptr );
		if (backend.available) {
			const auto path = directory.filePath( QStringLiteral( "openslide/compare.ptf" ) );
			auto dialog = pyra::test::OpenWsiDialog( directory.absolutePath(), path,
				pyra::test::reader_options() );
			auto *tbf = dialog.findChild<QSpinBox *>( QStringLiteral( "tbfCacheMiB" ) );
			auto *pbf = dialog.findChild<QSpinBox *>( QStringLiteral( "pbfCacheMiB" ) );
			auto *threads = dialog.findChild<QSpinBox *>( QStringLiteral( "decodeThreads" ) );
			require( tbf && pbf && threads, "Open WSI resource controls are missing" );
			tbf->setValue( 64 );
			pbf->setValue( 0 );
			threads->setValue( 3 );
			const auto options = dialog.options();
			require( options.compressed_cache_bytes == 64 * pyra::test::kBytesPerMiB &&
				options.decoded_cache_bytes == 0 && options.decode_threads == 3,
				"Open WSI did not translate its resource controls into C API options" );
			window.open( path, options );
			wait_for( [&] { return ui.native_->isEnabled() && window.windowTitle().contains( QStringLiteral( "compare.ptf" ) ); },
				"Native comparison slide did not open" );
			const auto count = record_pan( window, ui, 1 );
			auto *inspector = window.findChild<QDockWidget *>();
			require( inspector != nullptr, "Inspector missing" );
			inspector->hide();
			ui.replay_->trigger();
			wait_for( [&] { return ui.report_->toPlainText().contains( QStringLiteral( "Backend: PyrA native" ) ); },
				"Native replay report missing" );
			require( inspector->isVisible() && ui.report_->isVisible(),
				"Replay did not reveal the timing report" );
			auto timing_cursor = ui.report_->textCursor();
			timing_cursor.setPosition( int( ui.report_->toPlainText().lastIndexOf( QStringLiteral( "Total:" ) ) ) );
			require( ui.report_->viewport()->rect().contains( ui.report_->cursorRect( timing_cursor ).center() ),
				"Replay timing summary is outside the visible report area" );
			require( ui.report_->toPlainText().contains( QStringLiteral( "TBF cache: 64 MiB; PBF cache: 0 MiB; decoder threads: 3" ) ),
				"Replay report did not use the selected C API resource options" );
			auto alternate_options = options;
			alternate_options.backend = PYRA_BACKEND_OPENSLIDE;
			auto alternate_dialog = pyra::test::OpenWsiDialog( directory.absolutePath(), path, alternate_options );
			require( !alternate_dialog.findChild<QSpinBox *>( QStringLiteral( "tbfCacheMiB" ) )->isEnabled() &&
				!alternate_dialog.findChild<QSpinBox *>( QStringLiteral( "decodeThreads" ) )->isEnabled() &&
				alternate_dialog.findChild<QSpinBox *>( QStringLiteral( "pbfCacheMiB" ) )->isEnabled(),
				"OpenSlide exposes unsupported resource controls" );
			window.open( path, PYRA_BACKEND_OPENSLIDE );
			wait_for( [&] { return ui.replay_->isEnabled() && window.windowTitle().contains( QStringLiteral( "OpenSlide" ) ); },
				"Opening with OpenSlide lost the recording" );
			ui.replay_->trigger();
			wait_for( [&] { return ui.report_->toPlainText().contains( QStringLiteral( "Backend: OpenSlide" ) ); },
				"OpenSlide replay report missing" );
			require( ui.report_->toPlainText().contains( QStringLiteral( "Backend: PyrA native" ) ) &&
				ui.report_->toPlainText().contains( QStringLiteral( "Recorded: %1" ).arg( count ) ),
				"Backend open/replay discarded the previous report or changed the workload" );
			window.open( path + QStringLiteral( ".missing" ), PYRA_BACKEND_OPENSLIDE );
			wait_for( [&] { return ui.replay_->isEnabled(); }, "Failed open lost the active recording" );
			require( window.windowTitle().contains( QStringLiteral( "OpenSlide" ) ), "Failed open replaced the current slide" );
		}
		std::cout << "Prepared reuse=" << reused << " continuous refresh=" << refreshed
			<< "; unsupported compression visible: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
