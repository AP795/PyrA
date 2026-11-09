#include "viewer_window.h"

#include "minimap.h"
#include "test_fixture.h"
#include "viewport.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>

namespace pyra::test {

ViewerWindow::ViewerWindow ( const QString &image_directory )
	: evaluator_( std::make_shared<RenderEvaluator>() ),
	  image_directory_( image_directory.isEmpty() ? QDir::home().filePath( QStringLiteral( "images" ) )
		: image_directory )
{
	setWindowTitle( tr( "Pyra — test viewer" ) );
	resize( 1180, 820 );
	viewport_ = new Viewport( this );
	setCentralWidget( viewport_ );
	auto *toolbar = addToolBar( tr( "Viewer and performance" ) );
	toolbar->setMovable( false );
	open_ = toolbar->addAction( tr( "Open" ) );
	open_->setShortcut( QKeySequence::Open );
	fit_ = toolbar->addAction( tr( "Fit" ) );
	native_ = toolbar->addAction( tr( "1:1" ) );
	toolbar->addSeparator();
	planes_ = new QComboBox( toolbar );
	planes_->setAccessibleName( tr( "Channel and focal plane" ) );
	planes_->setToolTip( tr( "Channel, focal-plane Z, and available pyramid levels" ) );
	toolbar->addWidget( planes_ );
	toolbar->addSeparator();
	record_ = toolbar->addAction( tr( "Record" ) );
	record_->setToolTip( tr( "Start a new in-memory recording of every actual render call" ) );
	stop_ = toolbar->addAction( tr( "Stop" ) );
	stop_->setShortcut( QKeySequence( Qt::Key_Escape ) );
	stop_->setToolTip( tr( "Stop recording, or cancel replay after the current render" ) );
	replay_ = toolbar->addAction( tr( "Replay" ) );
	replay_->setToolTip( tr( "Replay exact recorded requests, preserving existing cache state" ) );
	auto *dock = new QDockWidget( tr( "Inspector / performance" ), this );
	auto *splitter = new QSplitter( Qt::Vertical, dock );
	minimap_ = new Minimap( splitter );
	metadata_ = new QPlainTextEdit( splitter );
	report_ = new QPlainTextEdit( splitter );
	metadata_->setReadOnly( true );
	report_->setReadOnly( true );
	metadata_->setAccessibleName( tr( "Slide metadata" ) );
	report_->setAccessibleName( tr( "Performance report" ) );
	report_->setPlainText( tr( "Record → navigate → Stop → Replay\n\n"
		"Recordings stay in memory and are cleared when another slide is opened.\n"
		"Replay retains current caches and excludes painting from render timing." ) );
	splitter->setSizes( { fontMetrics().lineSpacing() * 10,
		fontMetrics().lineSpacing() * 11, fontMetrics().lineSpacing() * 14 } );
	dock->setWidget( splitter );
	addDockWidget( Qt::RightDockWidgetArea, dock );
	resizeDocks( { dock }, { fontMetrics().averageCharWidth() * 42 }, Qt::Horizontal );
	toolbar->addSeparator();
	toolbar->addAction( dock->toggleViewAction() );
	busy_ = new QProgressBar( this );
	busy_->setRange( 0, 0 );
	busy_->setTextVisible( false );
	busy_->setAccessibleName( tr( "Background work in progress" ) );
	statusBar()->addPermanentWidget( busy_ );
	debounce_.setSingleShot( true );
	debounce_.setInterval( 0 );
	worker_ = new QObject;
	worker_->moveToThread( &thread_ );
	connect( &thread_, &QThread::finished, worker_, &QObject::deleteLater );
	thread_.start();
	connect( &debounce_, &QTimer::timeout, this, &ViewerWindow::schedule_render );
	connect( viewport_, &Viewport::view_changed, this, [this] {
		minimap_->set_view( viewport_->view_rect() );
		if (!debounce_.isActive())
			debounce_.start();
	} );
	connect( minimap_, &Minimap::center_requested, viewport_, &Viewport::center_on );
	connect( open_, &QAction::triggered, this, [this] {
		const auto path = QFileDialog::getOpenFileName( this, tr( "Open slide" ), image_directory_,
			tr( "Whole-slide images (*.svs *.ndpi *.bif *.ptf *.glr *.dcm *.czi);;All files (*)" ) );
		if (!path.isEmpty())
			open( path );
	} );
	connect( fit_, &QAction::triggered, viewport_, &Viewport::fit );
	connect( native_, &QAction::triggered, viewport_, &Viewport::native_size );
	connect( planes_, &QComboBox::currentIndexChanged, this, &ViewerWindow::select_plane );
	connect( record_, &QAction::triggered, this, &ViewerWindow::record );
	connect( stop_, &QAction::triggered, this, &ViewerWindow::stop );
	connect( replay_, &QAction::triggered, this, &ViewerWindow::replay );
	statusBar()->showMessage( tr( "Open a slide. Drag to pan; wheel to zoom; F to fit; 1 for native size." ) );
	update_controls();
}


ViewerWindow::~ViewerWindow ()
{
	evaluator_->cancel_replay();
	thread_.quit();
	thread_.wait();
}


void ViewerWindow::report_message ( const QString &message )
{
	report_->appendPlainText( message );
	statusBar()->showMessage( message );
}


void ViewerWindow::update_controls ()
{
	const auto ready = !plane_info_.empty() && !opening_ && !replaying_ && !stopping_;
	open_->setEnabled( !opening_ && !recording_ && !replaying_ && !stopping_ );
	fit_->setEnabled( ready );
	native_->setEnabled( ready );
	planes_->setEnabled( ready );
	viewport_->setEnabled( ready );
	minimap_->setEnabled( ready );
	record_->setEnabled( ready && !recording_ );
	stop_->setEnabled( recording_ || replaying_ );
	replay_->setEnabled( ready && !recording_ && recorded_count_ > 0 );
	busy_->setVisible( opening_ || overview_loading_ || rendering_ || replaying_ || stopping_ );
}


void ViewerWindow::select_plane ( int index )
{
	if (index < 0 || std::size_t( index ) >= plane_info_.size())
		return;
	const auto generation = ++generation_;
	overview_generation_.store( generation, std::memory_order_relaxed );
	pending_.reset();
	debounce_.stop();
	const auto plane = plane_info_[index];
	displayed_request_.reset();
	active_request_.reset();
	latest_view_.reset();
	overscan_failed_mpp_.reset();
	minimap_->set_plane( plane.bounds );
	viewport_->set_plane( plane.bounds, plane.native_mpp );
	overview_loading_ = true;
	update_controls();
	QMetaObject::invokeMethod( worker_, [this, plane, generation] {
		if (generation != overview_generation_.load( std::memory_order_relaxed ))
			return;
		auto overview = evaluator_->overview( plane.plane );
		QMetaObject::invokeMethod( this, [this, overview = std::move( overview ), generation] () mutable {
			if (generation != generation_)
				return;
			overview_loading_ = false;
			if (overview.incomplete) {
				overview.image = {};
				if (overview.error.isEmpty())
					overview.error = tr( "Incomplete minimap overview." );
			}
			minimap_->set_overview( overview.image, overview.error );
			if (!overview.error.isEmpty()) {
				metadata_->appendPlainText( tr( "Minimap: %1" ).arg( overview.error ) );
				report_->setPlainText( overview.error );
				statusBar()->showMessage( overview.error );
			}
			update_controls();
		} );
	} );
}


void ViewerWindow::open ( const QString &path )
{
	if (opening_ || recording_ || replaying_ || stopping_)
		return;
	opening_ = true;
	displayed_request_.reset();
	active_request_.reset();
	latest_view_.reset();
	overscan_failed_mpp_.reset();
	overview_loading_ = false;
	pending_.reset();
	debounce_.stop();
	const auto generation = ++generation_;
	overview_generation_.store( generation, std::memory_order_relaxed );
	recorded_count_ = 0;
	plane_info_.clear();
	viewport_->set_plane( {}, 1.0 );
	minimap_->set_plane( {} );
	statusBar()->showMessage( tr( "Opening %1…" ).arg( path ) );
	update_controls();
	QMetaObject::invokeMethod( worker_, [this, path, generation] {
		auto info = evaluator_->open( path );
		QMetaObject::invokeMethod( this, [this, info = std::move( info ), generation, path] {
			if (generation != generation_)
				return;
			opening_ = false;
			metadata_->setPlainText( info.metadata );
			{
				const auto blocker = QSignalBlocker( planes_ );
				planes_->clear();
				for (const auto &plane : info.planes)
					planes_->addItem( plane.label );
			}
			if (!info.error.isEmpty()) {
				statusBar()->showMessage( info.error );
				report_->setPlainText( info.error );
				if (smoke_step_ >= 0)
					smoke_finish( 1 );
			} else {
				last_loaded_image_ = QFileInfo( path ).absoluteFilePath();
				plane_info_ = info.planes;
				setWindowTitle( tr( "Pyra — %1" ).arg( path ) );
				select_plane( planes_->currentIndex() );
			}
			update_controls();
		} );
	} );
}


void ViewerWindow::schedule_render ()
{
	const auto index = planes_->currentIndex();
	if (opening_ || replaying_ || stopping_ || index < 0 ||
		std::size_t( index ) >= plane_info_.size())
		return;
	auto output = viewport_->size();
	if (output.width() > kMaxRenderEdge || output.height() > kMaxRenderEdge)
		output.scale( kMaxRenderEdge, kMaxRenderEdge, Qt::KeepAspectRatio );
	const auto mpp = viewport_->mpp() * std::max(
		double( viewport_->width() ) / output.width(), double( viewport_->height() ) / output.height() );
	const auto view = RenderRequest{ viewport_->view_rect(), mpp, plane_info_[index].plane };
	latest_view_ = view;
	if (( displayed_request_ && view_request_is_covered( *displayed_request_, view ) ) ||
		( active_request_ && view_request_is_covered( *active_request_, view ) )) {
		pending_.reset();
		if (smoke_step_ >= 0)
			QTimer::singleShot( 0, this, &ViewerWindow::smoke_frame );
		return;
	}
	pending_ = overscan_failed_mpp_ && *overscan_failed_mpp_ == view.mpp
		? view : prepare_view_request( view );
	dispatch_render();
}


void ViewerWindow::dispatch_render ()
{
	if (rendering_ || !pending_ || opening_ || replaying_ || stopping_)
		return;
	const auto request = *pending_;
	pending_.reset();
	rendering_ = true;
	active_request_ = request;
	update_controls();
	const auto generation = generation_;
	statusBar()->showMessage( tr( "Rendering…%1" ).arg( recording_ ? tr( " recording" ) : QString{} ) );
	QMetaObject::invokeMethod( worker_, [this, request, generation] {
		auto frame = evaluator_->render( request );
		const auto count = evaluator_->recorded().size();
		QMetaObject::invokeMethod( this, [this, frame = std::move( frame ), count, generation] {
			rendering_ = false;
			active_request_.reset();
			if (generation == generation_ && !opening_ && !replaying_) {
				recorded_count_ = count;
				if (frame.incomplete && latest_view_ && frame.request.plane == latest_view_->plane &&
					frame.request.mpp == latest_view_->mpp && frame.request.xywh != latest_view_->xywh) {
// Prefetch must not make a renderable visible view fail its working-set budget.
// Retry the exact latest view once and retain that choice at this resolution.
					overscan_failed_mpp_ = latest_view_->mpp;
					pending_ = *latest_view_;
					update_controls();
					dispatch_render();
					return;
				}
				const auto keep_displayed = displayed_request_ && latest_view_ &&
					view_request_is_covered( *displayed_request_, *latest_view_ ) &&
					!view_request_is_covered( frame.request, *latest_view_ );
				if (!frame.image.isNull() && !keep_displayed) {
					viewport_->set_frame( frame.image, frame.request.xywh );
					if (!frame.incomplete && frame.error.isEmpty())
						displayed_request_ = frame.request;
				}
				statusBar()->showMessage( tr( "%1 ms | %2 × %3 px | %4 µm/px | recorded: %5%6%7" )
					.arg( frame.render_ms, 0, 'f', 3 ).arg( frame.output.width() )
					.arg( frame.output.height() ).arg( frame.request.mpp )
					.arg( qulonglong( count ) ).arg( recording_ ? tr( " — RECORDING" ) : QString{} )
					.arg( frame.incomplete ? tr( " — INCOMPLETE" ) : QString{} ) );
				if (!frame.error.isEmpty()) {
					report_->setPlainText( frame.error );
					statusBar()->showMessage( frame.error );
					displayed_request_.reset();
				}
				if (frame.incomplete)
					displayed_request_.reset();
				if (smoke_step_ >= 0) {
					if (!frame.error.isEmpty() || frame.incomplete)
						smoke_finish( 1 );
					else
						smoke_frame();
				}
			}
			update_controls();
			dispatch_render();
		} );
	} );
}


void ViewerWindow::record ()
{
	recording_ = true;
	displayed_request_.reset();
	active_request_.reset();
	recorded_count_ = 0;
	QMetaObject::invokeMethod( worker_, [engine = evaluator_] { engine->record(); } );
	report_->setPlainText( tr( "Recording actual render calls.\nNavigate, then press Stop.\n"
		"Every call retains metric xywh, mpp, channel and Z; dimensions follow from mpp." ) );
	update_controls();
	schedule_render();
}


void ViewerWindow::stop ()
{
	if (replaying_) {
		evaluator_->cancel_replay();
		statusBar()->showMessage( tr( "Stopping after the current render…" ) );
		return;
	}
	if (!recording_)
		return;
	recording_ = false;
	stopping_ = true;
	pending_.reset();
	debounce_.stop();
	update_controls();
	QMetaObject::invokeMethod( worker_, [this] {
		const auto count = evaluator_->stop();
		QMetaObject::invokeMethod( this, [this, count] {
			recorded_count_ = count;
			stopping_ = false;
			report_->setPlainText( tr( "Stopped: %1 render requests recorded in memory.\n"
				"Replay repeats them in order with their original sizes and planes.\n"
				"Existing caches are retained; this is not a cold-cache benchmark." )
				.arg( qulonglong( count ) ) );
			update_controls();
			if (smoke_step_ == 7)
				replay_->trigger();
		} );
	} );
}


void ViewerWindow::replay ()
{
	replaying_ = true;
	pending_.reset();
	debounce_.stop();
	evaluator_->prepare_replay();
	update_controls();
	statusBar()->showMessage( tr( "Replaying %1 requests… Stop cancels after the current render." )
		.arg( qulonglong( recorded_count_ ) ) );
	QMetaObject::invokeMethod( worker_, [this] {
		auto report = evaluator_->replay();
		QMetaObject::invokeMethod( this, [this, report = std::move( report )] {
			replaying_ = false;
			report_->setPlainText( report.text() );
			statusBar()->showMessage( tr( "Replay finished: %1 calls, %2 ms total Pyra render time." )
				.arg( qulonglong( report.timing.count ) ).arg( report.timing.total_ms, 0, 'f', 3 ) );
			update_controls();
			if (smoke_step_ >= 0) {
				std::cout << report.text().toStdString() << std::endl;
				const auto ok = report.error.isEmpty() && report.recorded >= 4 &&
					report.timing.count == report.recorded && report.failed == 0 &&
					report.incomplete == 0 && !report.cancelled;
				QTimer::singleShot( 150, this, [this, ok] { smoke_finish( ok ? 0 : 1 ); } );
			}
		} );
	} );
}


void ViewerWindow::smoke_test ( const QString &output_directory )
{
	smoke_output_ = output_directory;
	smoke_step_ = 0;
	try {
		open( write_test_wsi( output_directory ) );
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		smoke_finish( 1 );
	}
	QTimer::singleShot( 30000, this, [this] {
		if (smoke_step_ >= 0) {
			std::cerr << "GUI smoke test timed out\n";
			smoke_finish( 1 );
		}
	} );
}


void ViewerWindow::smoke_frame ()
{
	switch (smoke_step_) {
	case 0:
		if (!minimap_->has_overview()) { smoke_finish( 1 ); return; }
		smoke_step_ = 1;
		record_->trigger();
		break;
	case 1:
		smoke_step_ = 2;
		if (!smoke_minimap( false )) smoke_finish( 1 );
		break;
	case 2:
		smoke_step_ = 3;
		viewport_->zoom( 0.5, viewport_->rect().center() );
		break;
	case 3:
		smoke_step_ = 4;
		if (!smoke_minimap( true )) smoke_finish( 1 );
		break;
	case 4:
		smoke_step_ = 5;
		planes_->setCurrentIndex( 1 );
		break;
	case 5:
		if (!minimap_->has_overview()) { smoke_finish( 1 ); return; }
		smoke_step_ = 6;
		viewport_->zoom( 0.5, viewport_->rect().center() );
		break;
	case 6:
		smoke_step_ = 7;
		stop_->trigger();
		break;
	default: break;
	}
}


bool ViewerWindow::smoke_minimap ( bool drag )
{
	const auto transform = minimap_->mapping();
	const auto mpp = viewport_->mpp();
	const auto first = transform.image.center();
	const auto final = drag ? transform.image.topLeft() +
		QPointF( transform.image.width() * 0.723, transform.image.height() * 0.637 ) : first;
	const auto send = [this] ( QEvent::Type type, QPointF position, Qt::MouseButton button,
		Qt::MouseButtons buttons ) {
		auto event = QMouseEvent( type, position, position, button, buttons, Qt::NoModifier );
		QApplication::sendEvent( minimap_, &event );
	};
	send( QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton );
	if (drag)
		send( QEvent::MouseMove, transform.image.bottomRight() + QPointF( 20, 20 ),
			Qt::NoButton, Qt::LeftButton );
	send( QEvent::MouseButtonRelease, final, Qt::LeftButton, Qt::NoButton );
	const auto delta = viewport_->view_rect().center() - transform.metric_at( final );
	return std::abs( delta.x() ) < 1e-9 && std::abs( delta.y() ) < 1e-9 &&
		viewport_->mpp() == mpp;
}


void ViewerWindow::smoke_finish ( int code )
{
	if (smoke_step_ < 0)
		return;
	if (!grab().save( smoke_output_ + QStringLiteral( "/gui-smoke.png" ) ))
		code = 1;
	smoke_step_ = -1;
	std::cout << "GUI smoke: " << ( code == 0 ? "PASS" : "FAIL" ) << std::endl;
	QApplication::exit( code );
}

} // namespace pyra::test
