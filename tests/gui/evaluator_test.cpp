#include "render_evaluator.h"
#include "test_fixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <chrono>
#include <cmath>
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


void test_timings_known_distribution ()
{
	const auto stats = pyra::test::summarize_timings( { 100, 2, 4, 1, 3 } );
	require( stats.count == 5 && stats.total_ms == 110 && stats.mean_ms == 22,
		"Count/total/mean are incorrect" );
	require( stats.min_ms == 1 && stats.max_ms == 100 && stats.median_ms == 3 &&
		stats.p95_ms == 100, "Percentiles/extrema are incorrect" );
	require( std::abs( stats.renders_per_second - 5000.0 / 110 ) < 1e-9,
		"Throughput is incorrect" );
}


void test_timings_empty_and_even_distribution ()
{
	require( pyra::test::summarize_timings( {} ).count == 0, "Empty sample is nonempty" );
	require( pyra::test::summarize_timings( { 2, 4 } ).median_ms == 3,
		"Even-sample median is incorrect" );
}


void test_recording_exact_order_and_stop ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	const auto opened = evaluator.open( path );
	require( opened.error.isEmpty() && opened.planes.size() >= 2, "Cannot open test planes" );
	auto first = pyra::test::RenderRequest{ { 0, 0, 64, 64 }, 1, { "G", 0 } };
	auto second = first;
	second.xywh = { 4, 8, 32, 24 };
	second.mpp = 0.75f;
	second.plane = { "G", 2 };
	evaluator.record();
	for (const auto &request : { first, first, second })
		require( evaluator.render( request ).error.isEmpty(), "Recorded render failed" );
	require( evaluator.stop() == 3, "Duplicate render was lost or Stop count is wrong" );
	const auto &recorded = evaluator.recorded();
	require( recorded[0] == first && recorded[1] == first && recorded[2] == second,
		"Recording did not preserve exact xywh/mpp/plane order" );
	(void)evaluator.render( first );
	require( evaluator.recorded().size() == 3, "Stop did not stop recording" );
	evaluator.prepare_replay();
	const auto report = evaluator.replay();
	require( report.error.isEmpty() && report.recorded == 3 && report.timing.count == 3 &&
		report.failed == 0 && report.incomplete == 0, "Replay omitted or failed calls" );
	require( report.last_request == second && report.last_image.size() == QSize( 43, 32 ),
		"Replay changed the final request dimensions" );
	require( std::abs( qGray( report.last_image.pixel( 15, 13 ) ) - 193 ) <= 3,
		"Replay selected the wrong focal plane or rendered different pixels" );
	require( evaluator.recorded().size() == 3, "Replay recorded itself" );
}


void test_recording_restart_and_wsi_replacement ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	const auto request = pyra::test::RenderRequest{ { 0, 0, 64, 64 }, 1, { "G", 0 } };
	evaluator.record();
	(void)evaluator.render( request );
	evaluator.record();
	require( evaluator.stop() == 0, "Record did not clear the previous workload" );
	evaluator.record();
	(void)evaluator.render( request );
	(void)evaluator.open( path );
	require( evaluator.recorded().empty() && !evaluator.replay().error.isEmpty(),
		"Opening a slide retained an obsolete recording" );
}


void test_replay_cancel_before_first_call ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	evaluator.record();
	(void)evaluator.render( { { 0, 0, 64, 64 }, 1, { "G", 0 } } );
	(void)evaluator.stop();
	evaluator.prepare_replay();
	evaluator.cancel_replay();
	const auto report = evaluator.replay();
	require( report.cancelled && report.timing.count == 0 && report.recorded == 1,
		"Stop requested before replay was ignored" );
}


void test_render_invalid_request_is_rejected ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	evaluator.record();
	const auto request = pyra::test::RenderRequest{
		{ 0, 0, pyra::test::kMaxRenderEdge + 1, 64 }, 1, { "G", 0 } };
	require( !evaluator.render( request ).error.isEmpty() && evaluator.stop() == 0,
		"Invalid output dimensions reached the renderer" );
}


void test_overview_preserves_recording_and_plane_selection ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	const auto request = pyra::test::RenderRequest{ { 0, 0, 64, 64 }, 1, { "G", 0 } };
	evaluator.record();
	require( evaluator.render( request ).error.isEmpty(), "Viewport render failed" );
	const auto first = evaluator.overview( { "G", 0 } );
	const auto second = evaluator.overview( { "G", 2 } );
	require( first.error.isEmpty() && second.error.isEmpty() &&
		!first.incomplete && !second.incomplete, "Overview render failed" );
	require( first.image.width() <= pyra::test::kOverviewEdge &&
		first.image.height() <= pyra::test::kOverviewEdge, "Overview is unbounded" );
	require( std::abs( qGray( first.image.pixel( 32, 32 ) ) - 37 ) <= 3 &&
		std::abs( qGray( second.image.pixel( 32, 32 ) ) - 193 ) <= 3,
		"Overview used the wrong focal plane" );
	require( evaluator.stop() == 1 && evaluator.recorded().front() == request,
		"Overview calls contaminated the recording" );
	evaluator.prepare_replay();
	const auto report = evaluator.replay();
	require( report.timing.count == 1 && report.error.isEmpty() && report.failed == 0,
		"Overview contaminated replay" );
}


void test_overview_rejects_missing_plane ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( !evaluator.overview( { "G", 0 } ).error.isEmpty(), "Closed slide supplied an overview" );
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	evaluator.record();
	require( !evaluator.overview( { "missing", 0 } ).error.isEmpty() && evaluator.stop() == 0,
		"Invalid overview altered recording" );
}


void test_prepared_view_coverage_and_limits ()
{
	const auto view = pyra::test::RenderRequest{ { 100, 200, 512, 256 }, 0.5, { "G", 0 } };
	const auto prepared = pyra::test::prepare_view_request( view );
	require( prepared.xywh.width() > view.xywh.width() &&
		pyra::test::view_request_is_covered( prepared, view ), "Prepared image does not cover its initial view" );
	auto moved = view;
	moved.xywh.translate( 20, 10 );
	require( pyra::test::view_request_is_covered( prepared, moved ), "Small pan lost prepared coverage" );
	moved.xywh.translate( prepared.xywh.width(), 0 );
	require( !pyra::test::view_request_is_covered( prepared, moved ), "Coverage accepted a view beyond its buffer" );
	moved = view;
	moved.mpp *= 2;
	require( !pyra::test::view_request_is_covered( prepared, moved ), "Zoom reused the wrong resolution" );
	moved = view;
	moved.plane.zlevel = 2;
	require( !pyra::test::view_request_is_covered( prepared, moved ), "Z change reused another plane" );
	moved = view;
	moved.plane.channel = "R";
	require( !pyra::test::view_request_is_covered( prepared, moved ), "Channel change reused another plane" );
	const auto large = pyra::test::RenderRequest{
		{ 0, 0, pyra::test::kMaxRenderEdge, pyra::test::kMaxRenderEdge }, 1, {} };
	const auto bounded = pyra::test::prepare_view_request( large );
	require( bounded.xywh.width() <= pyra::test::kMaxRenderEdge &&
		bounded.xywh.height() <= pyra::test::kMaxRenderEdge, "Prepared image exceeded the raster limit" );
}


void test_packed_display_storage_and_lifetime ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	const auto rgb_path = QDir( QFileInfo( path ).absolutePath() ).filePath( QStringLiteral( "gui_rgb_fixture.ptf" ) );
	require( evaluator.open( rgb_path ).error.isEmpty(), "Cannot open RGB display fixture" );
	const auto first = evaluator.render( { { 0, 0, 31, 27 }, 1, {} } );
	require( first.error.isEmpty() && first.image.format() == QImage::Format_RGB888,
		"RGB8 display did not use packed native storage" );
	const auto pixel = first.image.pixel( 15, 13 );
	require( std::abs( qRed( pixel ) - 37 ) <= 3 && std::abs( qGreen( pixel ) - 117 ) <= 3 &&
		std::abs( qBlue( pixel ) - 220 ) <= 3, "Packed RGB rows or channels changed" );
	(void)evaluator.render( { { 100, 100, 31, 27 }, 1, {} } );
	require( first.image.pixel( 15, 13 ) == pixel, "Display image retained temporary caller memory" );
	require( evaluator.open( path ).error.isEmpty(), "Cannot reopen monochrome display fixture" );
	const auto gray = evaluator.render( { { 0, 0, 31, 27 }, 1, { "G", 0 } } );
	require( gray.error.isEmpty() && gray.image.format() == QImage::Format_Grayscale8 &&
		std::abs( qGray( gray.image.pixel( 15, 13 ) ) - 37 ) <= 3, "Packed grayscale display changed" );
}


void test_unsupported_compression_error ( const QString &directory )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( QDir( directory ).filePath( QStringLiteral( "unsupported.ptf" ) ) ).error.isEmpty(),
		"Cannot open unsupported-codec test metadata" );
	const auto frame = evaluator.render( { { 0, 0, 32, 32 }, 1, {} } );
	require( frame.image.isNull() && frame.error.contains( QStringLiteral( "Unsupported compression" ) ) &&
		frame.error.contains( QStringLiteral( "JPEG-LS" ) ), "Viewer hid the unsupported-compression diagnosis" );
}


#if PYRA_TEST_WITH_OPENJPEG
void test_render_gray16_declared_display_range ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	const auto frame = evaluator.render( { { 0, 0, 64, 64 }, 1, { "DAPI", 0 } } );
	require( frame.error.isEmpty() && !frame.image.isNull(), "Gray16 render failed" );
	require( std::abs( qGray( frame.image.pixel( 32, 32 ) ) - 127 ) <= 1,
		"Gray16 display ignored the declared 12-bit sample range" );
}
#endif

} // namespace


int main ( int argc, char **argv )
{
	auto application = QCoreApplication( argc, argv );
	if (argc != 3)
		return 2;
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
	try {
		const auto path = pyra::test::write_test_wsi( QString::fromLocal8Bit( argv[1] ) );
		run( "test_timings_known_distribution", test_timings_known_distribution );
		run( "test_timings_empty_and_even_distribution", test_timings_empty_and_even_distribution );
		run( "test_prepared_view_coverage_and_limits", test_prepared_view_coverage_and_limits );
		run( "test_packed_display_storage_and_lifetime", [&] { test_packed_display_storage_and_lifetime( path ); } );
		run( "test_unsupported_compression_error", [&] {
			test_unsupported_compression_error( QString::fromLocal8Bit( argv[2] ) );
		} );
		run( "test_recording_exact_order_and_stop", [&] { test_recording_exact_order_and_stop( path ); } );
		run( "test_recording_restart_and_wsi_replacement", [&] { test_recording_restart_and_wsi_replacement( path ); } );
		run( "test_replay_cancel_before_first_call", [&] { test_replay_cancel_before_first_call( path ); } );
		run( "test_render_invalid_request_is_rejected", [&] { test_render_invalid_request_is_rejected( path ); } );
		run( "test_overview_preserves_recording_and_plane_selection", [&] { test_overview_preserves_recording_and_plane_selection( path ); } );
		run( "test_overview_rejects_missing_plane", [&] { test_overview_rejects_missing_plane( path ); } );
#if PYRA_TEST_WITH_OPENJPEG
		run( "test_render_gray16_declared_display_range", [&] { test_render_gray16_declared_display_range( path ); } );
#endif
	} catch (const std::exception &error) {
		std::cerr << "Fixture setup: " << error.what() << '\n';
		return 1;
	}
	return failed == 0 ? 0 : 1;
}
