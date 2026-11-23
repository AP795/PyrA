// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "cache_usage_widget.h"
#include "generate_wsi_dialog.h"
#include "minimap.h"
#include "open_wsi_dialog.h"
#include "test_fixture.h"
#include "viewer_settings.h"
#include "viewer_window.h"
#include "viewport.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTextCursor>

#include <cmath>
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
	QProgressBar *tbf_ = nullptr;
	QProgressBar *pbf_ = nullptr;
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
	result.tbf_ = window.findChild<QProgressBar *>( QStringLiteral( "tbfCacheAllocation" ) );
	result.pbf_ = window.findChild<QProgressBar *>( QStringLiteral( "pbfCacheAllocation" ) );
	require( result.tbf_ && result.pbf_, "Cache allocation bars are missing" );
	require( result.record_ && result.stop_ && result.native_ && result.replay_ && result.report_, "Viewer controls are missing" );
	return result;
}


void test_cache_usage_known_allocations_and_colors ( const QDir &directory )
{
	auto widget = pyra::test::CacheUsageWidget{};
	auto usage = pyra::test::CacheUsage{};
	usage.image_open = true;
	usage.options.compressed_cache_bytes = 100 * pyra::test::kBytesPerMiB;
	usage.options.decoded_cache_bytes = 200 * pyra::test::kBytesPerMiB;
	usage.stats = pyra_context_stats{ sizeof( pyra_context_stats ) };
	usage.occupancy = pyra_cache_usage{ sizeof( pyra_cache_usage ) };
	usage.occupancy->topology = PYRA_CACHE_ARENA;
	usage.occupancy->compressed_used_slots = 25;
	usage.occupancy->compressed_allocated_slots = 100;
	usage.occupancy->decoded_used_slots = 100;
	usage.occupancy->decoded_allocated_slots = 200;
	widget.set_usage( usage );
	const auto *tbf = widget.findChild<QProgressBar *>( QStringLiteral( "tbfCacheAllocation" ) );
	const auto *pbf = widget.findChild<QProgressBar *>( QStringLiteral( "pbfCacheAllocation" ) );
	require( tbf && pbf && tbf->value() * 4 == tbf->maximum() && pbf->value() * 2 == pbf->maximum(),
		"Cache occupancy fractions differ from occupied/allocated slots" );
	require( tbf->accessibleDescription().contains( QStringLiteral( "25 / 100 slots used" ) ) &&
		pbf->accessibleDescription().contains( QStringLiteral( "100 / 200 slots used" ) ) &&
		tbf->accessibleDescription().contains( QStringLiteral( "Arena" ) ),
		"Cache bars omitted slot counts or scope" );
	widget.resize( 440, widget.sizeHint().height() );
	widget.show();
	pump( 10 );
	const auto image = widget.grab().toImage();
	auto blue = 0;
	auto green = 0;
	for (auto y = 0; y < image.height(); ++y)
		for (auto x = 0; x < image.width(); ++x) {
			const auto pixel = image.pixelColor( x, y );
			blue += pixel == QColor( "#1976d2" );
			green += pixel == QColor( "#2e7d32" );
		}
	require( blue > 0 && green > 0, "TBF blue and PBF green were not painted" );
	require( image.save( directory.filePath( QStringLiteral( "images/cache_usage.png" ) ) ),
		"Cannot save cache usage widget screenshot" );
// Changing byte budgets must not change the fraction of allocated slots in use.
	usage.options.compressed_cache_bytes = uint64_t{ 8 } * 1024 * pyra::test::kBytesPerMiB;
	usage.stats->compressed_cache_bytes = uint64_t{ 6 } * 1024 * pyra::test::kBytesPerMiB;
	widget.set_usage( usage );
	require( tbf->value() * 4 == tbf->maximum(), "Occupancy changed with the byte allocation/cap" );
	usage.occupancy->compressed_used_slots = usage.occupancy->compressed_allocated_slots;
	usage.occupancy->decoded_used_slots = usage.occupancy->decoded_allocated_slots;
	widget.set_usage( usage );
	pump( 10 );
	for (const auto *bar : { tbf, pbf })
		require( bar->value() == bar->maximum() && bar->geometry().left() > 0 &&
			bar->geometry().right() < widget.width() - 1,
			"Full cache bar has no visible inset from the window edge" );
	require( widget.grab().save( directory.filePath( QStringLiteral( "images/cache_usage_full.png" ) ) ),
		"Cannot save full-cache bar screenshot" );
}


void test_cache_usage_empty_disabled_and_unavailable ()
{
	auto widget = pyra::test::CacheUsageWidget{};
	const auto *tbf = widget.findChild<QProgressBar *>( QStringLiteral( "tbfCacheAllocation" ) );
	const auto *pbf = widget.findChild<QProgressBar *>( QStringLiteral( "pbfCacheAllocation" ) );
	require( tbf && pbf && tbf->accessibleDescription().contains( QStringLiteral( "no image" ) ),
		"Empty viewer displayed stale allocation" );
	auto usage = pyra::test::CacheUsage{};
	usage.image_open = true;
	usage.options.decoded_cache_bytes = 0;
	widget.set_usage( usage );
	require( pbf->value() == 0 && pbf->accessibleDescription().contains( QStringLiteral( "disabled" ) ) &&
		tbf->accessibleDescription().contains( QStringLiteral( "unavailable" ) ),
		"Disabled PBF or missing statistics were shown as measured allocation" );
	usage.options.backend = PYRA_BACKEND_OPENSLIDE;
	usage.options.decoded_cache_bytes = 200 * pyra::test::kBytesPerMiB;
	usage.stats = pyra_context_stats{ sizeof( pyra_context_stats ) };
	usage.stats->decoded_cache_bytes = PYRA_STAT_UNAVAILABLE;
	widget.set_usage( usage );
	require( tbf->value() == 0 && pbf->value() == 0 &&
		tbf->accessibleDescription().contains( QStringLiteral( "unavailable (OpenSlide)" ) ) &&
		pbf->accessibleDescription().contains( QStringLiteral( "200 MiB capacity (OpenSlide)" ) ) &&
		pbf->accessibleDescription().contains( QStringLiteral( "occupancy unavailable" ) ),
		"OpenSlide unknown allocation was displayed as an empty measured cache" );
	usage.options.backend = PYRA_BACKEND_NATIVE;
	usage.occupancy = pyra_cache_usage{ sizeof( pyra_cache_usage ) };
	usage.occupancy->topology = PYRA_CACHE_ARENA;
	usage.occupancy->compressed_used_slots = usage.occupancy->compressed_allocated_slots = 100;
	widget.set_usage( usage );
	require( tbf->value() == tbf->maximum() && pbf->value() == 0 &&
		!pbf->accessibleDescription().contains( QStringLiteral( "unavailable" ) ),
		"Switching back to native did not clear unavailable state" );
	usage.occupancy->topology = PYRA_CACHE_PER_LEVEL;
	usage.occupancy->level_index = -1;
	widget.set_usage( usage );
	require( tbf->accessibleDescription().contains( QStringLiteral( "awaiting first render" ) ),
		"Level mode invented a selected pool before any render" );
	usage.occupancy->level_index = 7;
	usage.occupancy->synthetic_level = 1;
	widget.set_usage( usage );
	require( tbf->value() == 0 && tbf->accessibleDescription().contains( QStringLiteral( "Level 7" ) ) &&
		tbf->accessibleDescription().contains( QStringLiteral( "synthetic level" ) ),
		"Synthetic level was shown as an allocated tile cache" );
	usage.occupancy->synthetic_level = 0;
	usage.occupancy->compressed_used_slots = 3;
	usage.occupancy->compressed_allocated_slots = 12;
	widget.set_usage( usage );
	require( tbf->value() * 4 == tbf->maximum() &&
		tbf->accessibleDescription().contains( QStringLiteral( "Level 7" ) ) &&
		pbf->accessibleDescription().contains( QStringLiteral( "not allocated" ) ),
		"Selected-level occupancy or unallocated-pool state is incorrect" );
}


void test_open_dialog_cache_selection ()
{
	auto options = pyra::test::reader_options();
	options.cache_topology = PYRA_CACHE_PER_LEVEL;
	auto dialog = pyra::test::OpenWsiDialog( {}, QStringLiteral( "test.ptf" ), options );
	auto *cache = dialog.findChild<QComboBox *>( QStringLiteral( "cacheTopology" ) );
	auto *backend = dialog.findChild<QComboBox *>( QStringLiteral( "readerBackend" ) );
	require( cache && backend && cache->isEnabled() && cache->count() == 2 &&
		dialog.options().cache_topology == PYRA_CACHE_PER_LEVEL, "Open WSI did not restore Level cache selection" );
	cache->setCurrentIndex( cache->findData( uint32_t( PYRA_CACHE_ARENA ) ) );
	require( dialog.options().cache_topology == PYRA_CACHE_ARENA, "Arena selection did not reach context options" );
	cache->setCurrentIndex( cache->findData( uint32_t( PYRA_CACHE_PER_LEVEL ) ) );
	const auto openslide = backend->findData( uint32_t( PYRA_BACKEND_OPENSLIDE ) );
	if (openslide >= 0) {
		backend->setCurrentIndex( openslide );
		require( !cache->isEnabled(), "OpenSlide offered native cache topology selection" );
		backend->setCurrentIndex( backend->findData( uint32_t( PYRA_BACKEND_NATIVE ) ) );
		require( cache->isEnabled() && dialog.options().cache_topology == PYRA_CACHE_PER_LEVEL,
			"Switching readers lost the native topology selection" );
	}
}


void test_generate_dialog_options_and_lossless ()
{
	auto dialog = pyra::test::GenerateWsiDialog( QStringLiteral( "out" ) );
	auto *codec = dialog.findChild<QComboBox *>( QStringLiteral( "synthCodec" ) );
	auto *lossless = dialog.findChild<QCheckBox *>( QStringLiteral( "synthLossless" ) );
	auto *quality = dialog.findChild<QSpinBox *>( QStringLiteral( "synthQuality" ) );
	auto *tile = dialog.findChild<QSpinBox *>( QStringLiteral( "synthTile" ) );
	auto *z = dialog.findChild<QSpinBox *>( QStringLiteral( "synthZ" ) );
	require( codec && lossless && quality && tile && z, "Generate dialog controls are missing" );
	auto defaults = pyra_synthetic_options{};
	pyra_synthetic_options_init( &defaults );
	auto options = dialog.options();
	require( options.width == defaults.width && options.tile_edge == defaults.tile_edge &&
		options.z_levels == defaults.z_levels && options.mpp == defaults.mpp &&
		options.codec == PYRA_CODEC_JPEG && options.quality == defaults.quality &&
		dialog.path().endsWith( QStringLiteral( "synthetic_cells.ptf" ) ),
		"Generate dialog does not start from the library defaults" );
	require( !lossless->isEnabled(), "JPEG offered lossless compression" );
	require( tile->singleStep() == int( PYRA_SYNTHETIC_TILE_STEP ) && tile->minimum() == int( PYRA_SYNTHETIC_TILE_MIN ) &&
		z->maximum() == int( PYRA_SYNTHETIC_Z_MAX ), "Generate dialog limits differ from the library" );
	auto *nucleus = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthNucleus" ) );
	auto *spacing = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthSpacing" ) );
	auto *positive = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthPositive" ) );
	auto *z_step = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthZStep" ) );
	auto *focus = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthFocus" ) );
	auto *tumour = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthTumour" ) );
	auto *lymphocytes = dialog.findChild<QDoubleSpinBox *>( QStringLiteral( "synthLymphocytes" ) );
	require( nucleus && spacing && positive && z_step && focus && tumour && lymphocytes,
		"Generate dialog focus or tissue controls are missing" );
	require( nucleus->minimum() == PYRA_SYNTHETIC_NUCLEUS_MIN_UM && nucleus->maximum() == PYRA_SYNTHETIC_NUCLEUS_MAX_UM &&
		spacing->minimum() == PYRA_SYNTHETIC_SPACING_MIN_UM && spacing->maximum() == PYRA_SYNTHETIC_SPACING_MAX_UM &&
		z_step->minimum() == PYRA_SYNTHETIC_Z_STEP_MIN_UM && z_step->maximum() == PYRA_SYNTHETIC_Z_STEP_MAX_UM,
		"Generate dialog tissue limits differ from the library" );
	const auto near = [] ( double a, double b ) { return std::abs( a - b ) < 1e-3; };
	require( near( options.nucleus_diameter_um, defaults.nucleus_diameter_um ) &&
		near( options.nucleus_spacing_um, defaults.nucleus_spacing_um ) &&
		near( options.positive_fraction, defaults.positive_fraction ) &&
		near( options.z_step_um, defaults.z_step_um ) && near( options.focus_fraction, defaults.focus_fraction ) &&
		near( options.tumour_fraction, defaults.tumour_fraction ) &&
		near( options.lymphocyte_fraction, defaults.lymphocyte_fraction ),
		"Generate dialog focus or tissue defaults differ from the library" );
	positive->setValue( 75.0 );
	focus->setValue( 100.0 );
	require( near( dialog.options().positive_fraction, 0.75 ) && near( dialog.options().focus_fraction, 1.0 ),
		"Percent fields did not reach the options as fractions" );
	for (auto i = 0; i < codec->count(); ++i)
		require( pyra_codec_can_encode( codec->itemData( i ).toUInt() ), "Generate dialog listed an unavailable codec" );
	const auto lossless_codec = codec->findData( uint32_t( PYRA_CODEC_JPEGXL ) ) >= 0
		? codec->findData( uint32_t( PYRA_CODEC_JPEGXL ) ) : codec->findData( uint32_t( PYRA_CODEC_JPEG2000 ) );
	if (lossless_codec >= 0) {
		codec->setCurrentIndex( lossless_codec );
		lossless->setChecked( true );
		require( lossless->isEnabled() && !quality->isEnabled() &&
			dialog.options().quality == PYRA_QUALITY_LOSSLESS, "Lossless choice did not reach the options" );
		codec->setCurrentIndex( codec->findData( uint32_t( PYRA_CODEC_JPEG ) ) );
		require( dialog.options().quality == uint32_t( quality->value() ),
			"JPEG kept the lossless quality of the previous codec" );
	}
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

void test_viewer_z_slider_selects_available_positions ( const QDir &directory )
{
	auto temporary = QTemporaryDir( directory.filePath( QStringLiteral( "z-slider-XXXXXX" ) ) );
	require( temporary.isValid(), "Cannot create Z navigation fixture directory" );
	const auto path = pyra::test::write_test_wsi( temporary.path() );
	auto timer = QElapsedTimer{};
	timer.start();
	auto window = pyra::test::ViewerWindow{};
	window.resize( 1280, 900 );
	window.show();
	const auto ui = controls( window );
	auto *slider = window.findChild<QSlider *>( QStringLiteral( "zSlider" ) );
	auto *label = window.findChild<QLabel *>( QStringLiteral( "zPosition" ) );
	auto *planes = window.findChild<QComboBox *>();
	auto *viewport = window.findChild<pyra::test::Viewport *>();
	auto *minimap = window.findChild<pyra::test::Minimap *>();
	require( slider && label && planes && viewport && minimap, "Z navigation controls are missing" );
	require( !slider->isVisible(), "Z slider is visible before opening an image" );
	window.open( path );
	wait_for( [&] { return ui.native_->isEnabled() && minimap->has_overview(); }, "Z fixture did not open" );
	require( slider->isVisible() && slider->maximum() == 1 && slider->value() == 0,
		"Z slider did not restrict steps to the two available focal positions" );
	const auto slider_origin = slider->mapTo( &window, QPoint{} );
	const auto minimap_corner = minimap->mapTo( &window, minimap->rect().topRight() );
	require( slider_origin.x() > minimap_corner.x(), "Z slider is not right of the minimap" );
	ui.native_->trigger();
	viewport->zoom( 0.5, viewport->rect().center() );
	viewport->pan( { 12, 8 } );
	pump( 100 );
	const auto bounds = viewport->view_rect();
	const auto mpp = viewport->mpp();
	const auto before = viewport->grab().toImage();
	ui.record_->trigger();
	wait_for( [&] { return window.statusBar()->currentMessage().contains( QStringLiteral( "recorded: 1" ) ); },
		"Z fixture recording did not start" );
	slider->setValue( 1 );
	wait_for( [&] { return minimap->has_overview() &&
		window.statusBar()->currentMessage().contains( QStringLiteral( "recorded: 2" ) ); },
		"Z slider selection did not render" );
	require( planes->currentIndex() == 1 && label->text() == QStringLiteral( "Z 2 µm" ),
		"Z step did not select the sparse position at 2 micrometres" );
	require( viewport->mpp() == mpp && viewport->view_rect() == bounds,
		"Z slider changed the view position or zoom" );
	require( viewport->grab().toImage() != before, "Z slider did not display the selected plane's pixels" );
	planes->setCurrentIndex( 0 );
	require( slider->value() == 0 && viewport->view_rect() == bounds,
		"Combined selector did not synchronize Z while retaining the view" );
	if (planes->count() > 2) {
		planes->setCurrentIndex( 2 );
		require( !slider->isVisible(), "Single-Z channel retained the Z slider" );
		planes->setCurrentIndex( 1 );
		require( slider->isVisible() && slider->value() == 1, "Returning to a stack did not restore Z selection" );
	}
	const auto recording = window.session_for_exit();
	require( recording.requests.size() >= 2 && recording.requests[1].plane.zlevel == 2 &&
		recording.requests[1].plane.channel == QStringLiteral( "G" ), "Z navigation rendered the wrong channel or focal plane" );
	std::cout << "test_viewer_z_slider_selects_available_positions: PASS (" << timer.nsecsElapsed() / 1000 << " us)\n";
}


void test_viewer_zyp_stack_shows_slider ( const QDir &directory )
{
	auto window = pyra::test::ViewerWindow{};
	window.resize( 1280, 900 );
	window.show();
	const auto ui = controls( window );
	auto *slider = window.findChild<QSlider *>( QStringLiteral( "zSlider" ) );
	auto *label = window.findChild<QLabel *>( QStringLiteral( "zPosition" ) );
	auto *viewport = window.findChild<pyra::test::Viewport *>();
	auto *minimap = window.findChild<pyra::test::Minimap *>();
	require( slider && label && viewport && minimap, "ZYP controls are missing" );
	window.open( directory.filePath( QStringLiteral( "zyp/five-plane.zyp" ) ) );
	wait_for( [&] { return ui.native_->isEnabled() && minimap->has_overview(); }, "Five-plane ZYP did not open" );
	require( slider->isVisible() && slider->maximum() == 4 && slider->value() == 2 &&
		label->text() == QStringLiteral( "Z plane 3" ), "ZYP slider is hidden or central plane was not selected" );
	ui.native_->trigger();
	const auto mpp = viewport->mpp();
	const auto center = viewport->view_rect().center();
	slider->setValue( 4 );
	require( label->text() == QStringLiteral( "Z plane 5" ) && viewport->mpp() == mpp &&
		viewport->view_rect().center() == center, "ZYP selection lost the view or claimed calibrated physical Z" );
}


void test_viewer_recording_survives_restart ( const QDir &directory )
{
	auto config = QTemporaryDir{};
	auto settings = pyra::test::ViewerSettings( config.path() );
	require( settings.load().isEmpty(), "Cannot initialize isolated restart settings" );
	auto saved = pyra::test::ViewerRecording{};
	{
		auto first = pyra::test::ViewerWindow{};
		first.show();
		const auto ui = controls( first );
		first.open( directory.filePath( QStringLiteral( "coarse.ptf" ) ) );
		wait_for( [&] { return ui.native_->isEnabled() &&
			first.statusBar()->currentMessage().contains( QStringLiteral( " ms |" ) ); },
			"Restart fixture did not finish opening/rendering" );
		ui.record_->trigger();
// Exercise normal last-window exit with the same aboutToQuit save path as main.
// Do not press Stop: shutdown must drain the queued Record/render.
		auto saved_ok = false;
		const auto connection = QObject::connect( qApp, &QCoreApplication::aboutToQuit, &first, [&] {
			saved = first.session_for_exit();
			settings.session_saved( saved );
			saved_ok = settings.save_status();
		} );
		QTimer::singleShot( 0, &first, [&first] { first.close(); } );
		QApplication::exec();
		QObject::disconnect( connection );
		require( !saved.requests.empty(), "Exit snapshot lost queued active recording requests" );
		require( saved_ok, "Normal window exit did not save recorded positions" );
	}
	auto restarted = pyra::test::ViewerSettings( config.path() );
	require( restarted.load().isEmpty(), "Cannot reload recorded positions" );
	{
		auto second = pyra::test::ViewerWindow{};
		second.show();
		const auto ui = controls( second );
		second.open( restarted.startup_image(), pyra::test::reader_options(), restarted.recording() );
		wait_for( [&] { return ui.replay_->isEnabled(); }, "Restored positions did not enable Replay" );
		require( ui.report_->toPlainText().contains( QStringLiteral( "Restored %1 recorded requests" )
			.arg( qulonglong( saved.requests.size() ) ) ), "Recording restoration was not reported" );
		ui.replay_->trigger();
		wait_for( [&] { return ui.report_->toPlainText().contains( QStringLiteral( "Attempted: %1" )
			.arg( qulonglong( saved.requests.size() ) ) ); }, "Restored workload did not replay" );
		require( second.session_for_exit().requests == saved.requests, "Replay changed restored requests" );
	}
	{
		auto other = pyra::test::ViewerWindow{};
		other.show();
		const auto ui = controls( other );
		other.open( directory.filePath( QStringLiteral( "images/gui_raw/gui_raw.glr" ) ),
			pyra::test::reader_options(), restarted.recording() );
		wait_for( [&] { return ui.native_->isEnabled(); }, "Explicit alternate startup image did not open" );
		require( !ui.replay_->isEnabled() &&
			ui.report_->toPlainText().contains( QStringLiteral( "Saved recording was not restored" ) ),
			"Recording was attached to a different startup image" );
	}
}


} // namespace


int main ( int argc, char **argv )
{
	auto application = QApplication( argc, argv );
	if (argc != 2)
		return 2;
	try {
		test_cache_usage_known_allocations_and_colors( QDir( QString::fromLocal8Bit( argv[1] ) ) );
		test_cache_usage_empty_disabled_and_unavailable();
		test_open_dialog_cache_selection();
		test_generate_dialog_options_and_lossless();
		test_viewer_z_slider_selects_available_positions( QDir( QString::fromLocal8Bit( argv[1] ) ) );
		test_viewer_zyp_stack_shows_slider( QDir( QString::fromLocal8Bit( argv[1] ) ) );
		test_viewer_recording_survives_restart( QDir( QString::fromLocal8Bit( argv[1] ) ) );
		auto window = pyra::test::ViewerWindow{};
		window.resize( 1280, 900 );
		window.show();
		const auto ui = controls( window );
		const auto directory = QDir( QString::fromLocal8Bit( argv[1] ) );
		auto initial_options = pyra::test::reader_options();
		initial_options.cache_topology = PYRA_CACHE_PER_LEVEL;
		initial_options.decode_threads = 2;
		window.open( directory.filePath( QStringLiteral( "coarse.ptf" ) ), initial_options );
		wait_for( [&] { return ui.native_->isEnabled(); }, "Synthetic slide did not open" );
		require( !ui.tbf_->accessibleDescription().contains( QStringLiteral( "unavailable" ) ) &&
			!ui.pbf_->accessibleDescription().contains( QStringLiteral( "unavailable" ) ),
			"Open did not deliver cache allocation from the worker" );
		const auto *minimap = window.findChild<pyra::test::Minimap *>();
		const auto *usage_widget = window.findChild<pyra::test::CacheUsageWidget *>();
		require( minimap && usage_widget && minimap->parentWidget() == usage_widget->parentWidget() &&
			usage_widget->geometry().top() > minimap->geometry().bottom(),
			"Cache bars are not directly below the minimap" );
		require( window.findChild<QSplitter *>()->count() == 3, "Cache display changed the inspector splitter layout" );
		require( ui.report_->toPlainText().contains( QStringLiteral( "Cache type: Level" ) ) &&
			ui.report_->toPlainText().contains( QStringLiteral( "decoder threads: 2" ) ),
			"Open report omitted the selected cache type or thread count" );
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
			require( ui.pbf_->accessibleDescription().contains( QStringLiteral( "disabled" ) ),
				"Rendered native PBF=0 did not update the cache display" );
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
				alternate_dialog.findChild<QSpinBox *>( QStringLiteral( "decodeThreads" ) )->isEnabled() &&
				alternate_dialog.findChild<QSpinBox *>( QStringLiteral( "pbfCacheMiB" ) )->isEnabled(),
				"OpenSlide exposes unsupported resource controls" );
			window.open( path, PYRA_BACKEND_OPENSLIDE );
			wait_for( [&] { return ui.replay_->isEnabled() && window.windowTitle().contains( QStringLiteral( "OpenSlide" ) ); },
				"Opening with OpenSlide lost the recording" );
			ui.replay_->trigger();
			wait_for( [&] { return ui.report_->toPlainText().contains( QStringLiteral( "Backend: OpenSlide" ) ); },
				"OpenSlide replay report missing" );
			require( ui.tbf_->accessibleDescription().contains( QStringLiteral( "unavailable (OpenSlide)" ) ) &&
				ui.pbf_->accessibleDescription().contains( QStringLiteral( "disabled" ) ),
				"OpenSlide replay did not update the cache display" );
			require( ui.report_->toPlainText().contains( QStringLiteral( "Backend: PyrA native" ) ) &&
				ui.report_->toPlainText().contains( QStringLiteral( "Recorded: %1" ).arg( count ) ),
				"Backend open/replay discarded the previous report or changed the workload" );
			window.open( path + QStringLiteral( ".missing" ), PYRA_BACKEND_OPENSLIDE );
			wait_for( [&] { return ui.replay_->isEnabled(); }, "Failed open lost the active recording" );
			require( window.windowTitle().contains( QStringLiteral( "OpenSlide" ) ), "Failed open replaced the current slide" );
		}
		window.open( directory.filePath( QStringLiteral( "images/gui_raw/gui_raw.glr" ) ), PYRA_BACKEND_NATIVE );
		wait_for( [&] { return ui.native_->isEnabled() && window.windowTitle().contains( QStringLiteral( "gui_raw.glr" ) ); },
			"Regular IPX did not open in the viewer" );
		ui.native_->trigger();
		wait_for( [&] { return window.statusBar()->currentMessage().contains( QStringLiteral( " ms |" ) ); },
			"Regular IPX native viewport did not render" );
		require( window.grab().save( directory.filePath( QStringLiteral( "images/viewer_raw.png" ) ) ),
			"Cannot save regular-image viewer screenshot" );
		std::cout << "Prepared reuse=" << reused << " continuous refresh=" << refreshed
			<< "; unsupported compression visible: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
