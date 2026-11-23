// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "render_evaluator.h"
#include "test_fixture.h"

#ifdef _WIN32
#include "../Core/pyr_win.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <atomic>
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


#ifdef _WIN32
std::atomic<unsigned> statistics_exception_count{ 0 };

LONG CALLBACK count_statistics_exception ( EXCEPTION_POINTERS *exception )
{
	constexpr auto kMsvcCppException = DWORD{ 0xe06d7363 };
	if (exception->ExceptionRecord->ExceptionCode == kMsvcCppException)
		statistics_exception_count.fetch_add( 1, std::memory_order_relaxed );
	return EXCEPTION_CONTINUE_SEARCH;
}


class StatisticsExceptionCounter {
public:
	StatisticsExceptionCounter () {
		statistics_exception_count.store( 0, std::memory_order_relaxed );
		handler_ = AddVectoredExceptionHandler( 1, count_statistics_exception );
		require( handler_ != nullptr, "Cannot install the first-chance exception counter" );
	}
	~StatisticsExceptionCounter () { RemoveVectoredExceptionHandler( handler_ ); }
	StatisticsExceptionCounter ( const StatisticsExceptionCounter & ) = delete;
	StatisticsExceptionCounter &operator= ( const StatisticsExceptionCounter & ) = delete;
	unsigned count () const { return statistics_exception_count.load( std::memory_order_relaxed ); }

private:
	PVOID handler_ = nullptr;
};
#endif


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
	require( report.minimum_output == QSize( 43, 32 ) && report.maximum_output == QSize( 64, 64 ) &&
		report.text().contains( QStringLiteral( "Rendered size: width 43–64 px; height 32–64 px" ) ),
		"Replay report omitted actual raster dimensions for mixed render sizes" );
	require( std::abs( qGray( report.last_image.pixel( 15, 13 ) ) - 193 ) <= 3,
		"Replay selected the wrong focal plane or rendered different pixels" );
	require( evaluator.recorded().size() == 3, "Replay recorded itself" );
}


void test_replay_cache_counters_exclude_previous_renders ( const QString &path )
{
	for (const auto topology : { PYRA_CACHE_ARENA, PYRA_CACHE_PER_LEVEL }) {
		auto options = pyra::test::reader_options();
		options.cache_topology = topology;
		options.decode_threads = 3;
		auto evaluator = pyra::test::RenderEvaluator( options );
		require( evaluator.open( path, options ).error.isEmpty(), "Cache replay fixture failed to open" );
		const auto request = pyra::test::RenderRequest{ { 0, 0, 32, 32 }, 1, { "G", 0 } };
		require( evaluator.render( request ).error.isEmpty(), "Cache warmup failed" );
		evaluator.record();
		for (auto i = 0; i < 2; ++i)
			require( evaluator.render( request ).error.isEmpty(), "Recorded cache render failed" );
		require( evaluator.stop() == 2, "Cache workload count differs" );
		evaluator.prepare_replay();
		const auto report = evaluator.replay();
		require( report.error.isEmpty() && report.cache.decoded_cache_hits == 2 &&
			report.cache.decoded_cache_misses == 0 && report.cache.compressed_cache_hits == 0 &&
			report.cache.compressed_cache_misses == 0, "Replay counters include work before replay" );
		const auto text = report.text();
		require( text.contains( QStringLiteral( "Rendered size: 32 × 32 px" ) ),
			"Replay report omitted the rendered width and height" );
		require( text.contains( QStringLiteral( "decoder threads: 3" ) ) &&
			text.contains( topology == PYRA_CACHE_PER_LEVEL ? QStringLiteral( "Cache type: Level" ) :
				QStringLiteral( "Cache type: Arena" ) ) &&
			text.contains( QStringLiteral( "PBF cache hit ratio: 100.0% (2 hits, 0 misses)" ) ) &&
			text.contains( QStringLiteral( "TBF cache hit ratio: N/A (no lookups)" ) ),
			"Cache replay report omitted selected settings or interval ratios" );
	}
}


void test_cache_usage_reflects_reader_context ( const QString &path )
{
	auto options = pyra::test::reader_options();
	options.decoded_cache_bytes = 0;
	options.decode_threads = 3;
	options.cache_topology = PYRA_CACHE_PER_LEVEL;
	auto evaluator = pyra::test::RenderEvaluator( options );
	require( !evaluator.cache_usage().image_open, "Empty evaluator reported an open image" );
	require( evaluator.open( path, options ).error.isEmpty(), "Cache usage fixture failed to open" );
	require( evaluator.render( { { 0, 0, 32, 32 }, 1, { "G", 0 } } ).error.isEmpty(), "Cache usage render failed" );
	const auto usage = evaluator.cache_usage();
	require( usage.image_open && usage.stats && usage.stats->compressed_cache_bytes > 0 &&
		usage.stats->decoded_cache_bytes == 0 && usage.options.decoded_cache_bytes == 0 &&
		usage.options.cache_topology == PYRA_CACHE_PER_LEVEL && usage.options.decode_threads == 3,
		"Cache usage snapshot differs from the active context allocation or options" );
}


void test_reopening_same_image_reinitializes_both_caches ( const QString &path )
{
	for (const auto topology : { PYRA_CACHE_ARENA, PYRA_CACHE_PER_LEVEL }) {
		auto options = pyra::test::reader_options();
		options.cache_topology = topology;
		auto evaluator = pyra::test::RenderEvaluator( options );
		require( evaluator.open( path, options ).error.isEmpty(), "Cache reset fixture did not open" );
		const auto initial = evaluator.cache_usage().stats.value();
		const auto request = pyra::test::RenderRequest{ { 0, 0, 64, 64 }, 1, { "G", 0 } };
		evaluator.record();
		for (auto repeat = 0; repeat < 2; ++repeat)
			require( evaluator.render( request ).error.isEmpty(), "Cannot warm cache reset fixture" );
		(void)evaluator.stop();
		const auto recorded = evaluator.recorded();
		const auto warm = evaluator.cache_usage().stats.value();
		require( warm.compressed_cache_bytes > initial.compressed_cache_bytes &&
			warm.decoded_cache_bytes > initial.decoded_cache_bytes && warm.render_calls == 2,
			"Fixture did not populate both caches before reopening" );
		const auto reopened = evaluator.open( path, options );
		require( reopened.error.isEmpty() && reopened.recording_preserved && evaluator.recorded() == recorded,
			"Reopening the same image lost its compatible recording" );
		const auto reset = evaluator.cache_usage().stats.value();
		require( reset.render_calls == 0 &&
			reset.compressed_cache_bytes == initial.compressed_cache_bytes &&
			reset.decoded_cache_bytes == initial.decoded_cache_bytes &&
			reset.compressed_cache_hits == initial.compressed_cache_hits &&
			reset.compressed_cache_misses == initial.compressed_cache_misses &&
			reset.decoded_cache_hits == initial.decoded_cache_hits &&
			reset.decoded_cache_misses == initial.decoded_cache_misses,
			"Same-file Open retained old cache allocations or lookup state" );
		require( evaluator.render( request ).error.isEmpty(), "Render after cache initialization failed" );
		const auto first = evaluator.cache_usage().stats.value();
		require( first.compressed_cache_misses > reset.compressed_cache_misses &&
			first.decoded_cache_misses > reset.decoded_cache_misses,
			"First render after reopening reused old compressed or decoded cache entries" );
	}
}


void test_cache_occupancy_scope_and_overview_isolation ( const QString &path )
{
	for (const auto topology : { PYRA_CACHE_ARENA, PYRA_CACHE_PER_LEVEL }) {
		auto options = pyra::test::reader_options();
		options.cache_topology = topology;
		auto evaluator = pyra::test::RenderEvaluator( options );
		require( evaluator.open( path, options ).error.isEmpty(), "Occupancy fixture did not open" );
		const auto first = pyra::test::RenderRequest{ { 0, 0, 32, 32 }, 1, { "G", 0 } };
		auto second = first;
		second.plane.zlevel = 2;
		require( evaluator.render( first ).error.isEmpty(), "First occupancy render failed" );
		const auto a = evaluator.cache_usage().occupancy.value();
		require( a.compressed_used_slots == 1 && a.decoded_used_slots == 1 &&
			a.compressed_allocated_slots > a.compressed_used_slots &&
			a.decoded_allocated_slots > a.decoded_used_slots, "Reservation was confused with occupied slots" );
		require( evaluator.render( second ).error.isEmpty(), "Second occupancy render failed" );
		const auto b = evaluator.cache_usage().occupancy.value();
		require( b.level_index != a.level_index && b.topology == uint32_t( topology ),
			"Occupancy did not track the actual rendered level" );
		const auto expected = topology == PYRA_CACHE_ARENA ? 2u : 1u;
		require( b.compressed_used_slots == expected && b.decoded_used_slots == expected,
			"Arena totals or selected-level slot counts are incorrect" );
		require( evaluator.overview( first.plane ).error.isEmpty(), "Occupancy overview failed" );
		const auto after_overview = evaluator.cache_usage().occupancy.value();
		require( after_overview.level_index == b.level_index, "Minimap replaced the main view's selected cache level" );
		if (topology == PYRA_CACHE_PER_LEVEL)
			require( after_overview.compressed_used_slots == b.compressed_used_slots &&
				after_overview.decoded_used_slots == b.decoded_used_slots,
				"Level bars included a different level used by the minimap" );
		require( evaluator.render( second ).error.isEmpty() &&
			evaluator.cache_usage().occupancy->level_index == b.level_index,
			"Warm direct sampling lost the rendered level" );
		auto outside = second;
		outside.xywh.moveTopLeft( { 1000000, 1000000 } );
		require( evaluator.render( outside ).error.isEmpty() &&
			evaluator.cache_usage().occupancy->level_index == b.level_index,
			"An outside-image view discarded the last level that actually used the cache" );
	}
}


void test_cache_report_unavailable_and_disabled ()
{
	auto report = pyra::test::ReplayReport{};
	report.options.backend = PYRA_BACKEND_OPENSLIDE;
	require( report.text().contains( QStringLiteral( "PBF cache hit ratio: N/A (unavailable)" ) ) &&
		report.text().contains( QStringLiteral( "TBF cache hit ratio: N/A (unavailable)" ) ),
		"Unavailable cache counters were presented as a measured percentage" );
	report.options.backend = PYRA_BACKEND_NATIVE;
	report.options.decoded_cache_bytes = 0;
	require( report.text().contains( QStringLiteral( "PBF cache hit ratio: N/A (disabled)" ) ),
		"Disabled decoded caching was not identified" );
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
	const auto reopened = evaluator.open( path );
	require( reopened.error.isEmpty() && reopened.recording_preserved && evaluator.recorded().size() == 1,
		"Reopening the same slide lost its recording" );
	const auto failed = evaluator.open( path + QStringLiteral( ".missing" ) );
	require( !failed.error.isEmpty() && evaluator.recorded().size() == 1 &&
		evaluator.render( request ).error.isEmpty(), "Failed open lost the slide or recording" );
	auto backend = pyra_backend_info{ sizeof( pyra_backend_info ) };
	pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &backend, nullptr );
	if (backend.available) {
		const auto incompatible = evaluator.open( path, PYRA_BACKEND_OPENSLIDE );
		require( !incompatible.error.isEmpty() && evaluator.recorded().size() == 1 &&
			evaluator.render( request ).error.isEmpty(), "Incompatible backend lost the existing plane/recording" );
	}
}


void test_recording_restore_preserves_exact_workload ( const QString &path )
{
	auto original = pyra::test::RenderEvaluator{};
	require( original.open( path ).error.isEmpty(), "Open failed" );
	const auto first = pyra::test::RenderRequest{ { -2.25, 3.5, 32, 24 }, 0.75, { "G", 0 } };
	const auto second = pyra::test::RenderRequest{ { 7.125, 9.25, 21, 30 }, 1.25, { "G", 2 } };
	original.record();
	for (const auto &request : { first, first, second })
		require( original.render( request ).error.isEmpty(), "Recording render failed" );
	const auto saved = original.recording();
	auto restarted = pyra::test::RenderEvaluator{};
	require( restarted.open( path ).error.isEmpty() && restarted.restore_recording( saved ).isEmpty(),
		"Saved recording did not restore on the matching image" );
	require( restarted.recorded() == saved.requests, "Restoration changed request order or coordinates" );
	(void)restarted.render( first );
	require( restarted.recorded() == saved.requests, "Restoration resumed recording without Record" );
	restarted.prepare_replay();
	const auto report = restarted.replay();
	require( report.error.isEmpty() && report.timing.count == 3 && report.last_request == second,
		"Restored positions could not be replayed" );
}


void test_recording_restore_rejects_changed_source ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	evaluator.record();
	(void)evaluator.render( { { 0, 0, 32, 32 }, 1, { "G", 0 } } );
	(void)evaluator.stop();
	const auto saved = evaluator.recording();
	for (auto field = 0; field < 3; ++field) {
		auto changed = saved;
		if (field == 0) changed.source += QStringLiteral( ".different" );
		if (field == 1) ++changed.source_size;
		if (field == 2) ++changed.source_modified_ms;
		require( !evaluator.restore_recording( changed ).isEmpty() && evaluator.recorded() == saved.requests,
			"Changed source identity restored or discarded the current workload" );
	}
}


void test_recording_restore_rejects_changed_geometry ( const QString &path )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	require( evaluator.open( path ).error.isEmpty(), "Open failed" );
	evaluator.record();
	(void)evaluator.render( { { 0, 0, 32, 32 }, 1, { "G", 0 } } );
	(void)evaluator.stop();
	auto changed = evaluator.recording();
	changed.planes.front().bounds.setWidth( changed.planes.front().bounds.width() + 100 );
	require( !evaluator.restore_recording( changed ).isEmpty(), "Changed plane geometry was accepted" );
}


void test_openslide_statistics_do_not_throw ( const QString &directory )
{
	auto backend = pyra_backend_info{ sizeof( pyra_backend_info ) };
	require( pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &backend, nullptr ) == PYRA_OK, "Backend query failed" );
	if (!backend.available)
		return;
	auto options = pyra::test::reader_options( PYRA_BACKEND_OPENSLIDE );
	options.decoded_cache_bytes = 16 * pyra::test::kBytesPerMiB;
	auto evaluator = pyra::test::RenderEvaluator( options );
	require( evaluator.open( QDir( directory ).filePath( QStringLiteral( "openslide/compare.ptf" ) ), options ).error.isEmpty(),
		"OpenSlide exception fixture failed to open" );
#ifdef _WIN32
	const auto exceptions = StatisticsExceptionCounter{};
#endif
	evaluator.record();
	const auto frame = evaluator.render( { { 0, 0, 16, 16 }, 1, {} } );
	require( frame.error.isEmpty() && !frame.incomplete, "OpenSlide exception fixture did not render" );
	require( evaluator.stop() == 1, "OpenSlide exception fixture did not record" );
	evaluator.prepare_replay();
	const auto report = evaluator.replay();
	const auto usage = evaluator.cache_usage();
	require( report.error.isEmpty() && report.failed == 0 && report.incomplete == 0 && report.timing.count == 1 &&
		!usage.occupancy && usage.options.decoded_cache_bytes == options.decoded_cache_bytes,
		"OpenSlide replay or unknown-occupancy state differs" );
#ifdef _WIN32
	std::cout << "OpenSlide statistics first-chance C++ exceptions: " << exceptions.count() << '\n';
	require( exceptions.count() == 0, "OpenSlide rendering/replay throws for routine cache statistics" );
#endif
}


void test_backend_replay ( const QString &directory )
{
	auto info = pyra_backend_info{ sizeof( pyra_backend_info ) };
	require( pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &info, nullptr ) == PYRA_OK, "Backend query failed" );
	if (!info.available)
		return;
	auto evaluator = pyra::test::RenderEvaluator{};
	const auto source = QDir( directory ).filePath( QStringLiteral( "openslide/compare.ptf" ) );
	auto options = pyra::test::reader_options();
	options.compressed_cache_bytes = 64 * pyra::test::kBytesPerMiB;
	options.decoded_cache_bytes = 0;
	options.decode_threads = 3;
	require( evaluator.open( source, options ).error.isEmpty(), "Native comparison open failed" );
	const auto first = pyra::test::RenderRequest{ { -1, -1, 16, 12 }, 0.5, {} };
	auto second = first;
	second.xywh = { 4, 6, 12, 8 };
	second.mpp = 1;
	evaluator.record();
	require( evaluator.render( first ).error.isEmpty() && evaluator.render( second ).error.isEmpty(), "Recording failed" );
	(void)evaluator.stop();
	evaluator.prepare_replay();
	const auto native = evaluator.replay();
	require( native.options.decode_threads == 3 && native.options.decoded_cache_bytes == 0 &&
		native.text().contains( QStringLiteral( "TBF cache: 64 MiB; PBF cache: 0 MiB; decoder threads: 3" ) ),
		"Replay lost its actual context resource settings" );
	auto invalid = options;
	invalid.decode_threads = 0;
	require( !evaluator.open( source, invalid ).error.isEmpty() && evaluator.recorded().size() == 2,
		"Invalid resource options replaced the active recording" );
	const auto opened = evaluator.open( source, PYRA_BACKEND_OPENSLIDE );
	require( opened.error.isEmpty() && opened.recording_preserved && opened.recorded == 2 &&
		evaluator.recorded()[0] == first && evaluator.recorded()[1] == second,
		"Backend open changed the recorded requests" );
	evaluator.prepare_replay();
	const auto alternate = evaluator.replay();
	require( alternate.error.isEmpty() && alternate.failed == 0 && alternate.timing.count == 2 &&
		alternate.last_request == native.last_request && alternate.last_image == native.last_image &&
		alternate.backend.contains( QStringLiteral( "OpenSlide" ) ) &&
		native.backend.contains( QStringLiteral( "PyrA native" ) ) &&
		alternate.text().contains( QStringLiteral( "output copy" ) ), "Cross-backend replay differs" );
	require( alternate.options.decoded_cache_bytes == 0 && alternate.options.decode_threads == 3,
		"Reopening with another reader lost the selected context settings" );
	require( evaluator.open( source, PYRA_BACKEND_NATIVE ).error.isEmpty() && evaluator.recorded().size() == 2,
		"Switching back lost the recording" );
	require( evaluator.open( QDir( directory ).filePath( QStringLiteral( "standalone.ptf" ) ) ).error.isEmpty() &&
		evaluator.recorded().empty(), "Different slide retained an obsolete recording" );
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

void test_image_ipx_display_and_replay ( const QString &directory )
{
	auto evaluator = pyra::test::RenderEvaluator{};
	const auto opened = evaluator.open( QDir( directory ).filePath( QStringLiteral( "images/gui_raw/gui_raw.glr" ) ) );
	require( opened.error.isEmpty() && opened.planes.size() == 1 &&
		opened.planes.front().pixel_format == PYRA_PIXEL_GRAY16, "Gray16 gallery did not open through the C API" );
	const auto &plane = opened.planes.front();
	auto request = pyra::test::RenderRequest{ plane.bounds, plane.native_mpp, plane.plane };
	evaluator.record();
	const auto native = evaluator.render( request );
	require( native.error.isEmpty() && !native.incomplete && native.image.size() == QSize( 37, 29 ),
		"Gray16 gallery native display failed" );
// A gallery measures its members' range: display scales by the declared maximum.
	const auto full_scale = plane.max_sample_value > 0 ? plane.max_sample_value : 65535.0;
	const auto expected = int( ( ( 17 * 137 + 13 * 511 ) % 65536 ) * 255.0 / full_scale );
	require( std::abs( qGray( native.image.pixel( 17, 13 ) ) - expected ) <= 1,
		"Gray16 gallery display changed sample scaling" );
	request.mpp *= 2;
	const auto zoomed = evaluator.render( request );
	require( zoomed.error.isEmpty() && !zoomed.incomplete, "Regular IPX zoomed rendering failed" );
	require( evaluator.stop() == 2, "Regular IPX recording lost renders" );
	evaluator.prepare_replay();
	const auto report = evaluator.replay();
	require( report.error.isEmpty() && report.failed == 0 && report.incomplete == 0 &&
		report.timing.count == 2 && report.last_request == request, "Regular IPX replay failed" );
}

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
		run( "test_image_ipx_display_and_replay", [&] {
			test_image_ipx_display_and_replay( QString::fromLocal8Bit( argv[2] ) );
		} );
		run( "test_timings_empty_and_even_distribution", test_timings_empty_and_even_distribution );
		run( "test_prepared_view_coverage_and_limits", test_prepared_view_coverage_and_limits );
		run( "test_packed_display_storage_and_lifetime", [&] { test_packed_display_storage_and_lifetime( path ); } );
		run( "test_unsupported_compression_error", [&] {
			test_unsupported_compression_error( QString::fromLocal8Bit( argv[2] ) );
		} );
		run( "test_recording_exact_order_and_stop", [&] { test_recording_exact_order_and_stop( path ); } );
		run( "test_replay_cache_counters_exclude_previous_renders", [&] {
			test_replay_cache_counters_exclude_previous_renders( path );
		} );
		run( "test_cache_report_unavailable_and_disabled", test_cache_report_unavailable_and_disabled );
		run( "test_cache_usage_reflects_reader_context", [&] { test_cache_usage_reflects_reader_context( path ); } );
		run( "test_cache_occupancy_scope_and_overview_isolation", [&] {
			test_cache_occupancy_scope_and_overview_isolation( path );
		} );
		run( "test_reopening_same_image_reinitializes_both_caches", [&] {
			test_reopening_same_image_reinitializes_both_caches( path );
		} );
		run( "test_recording_restart_and_wsi_replacement", [&] { test_recording_restart_and_wsi_replacement( path ); } );
		run( "test_recording_restore_preserves_exact_workload", [&] { test_recording_restore_preserves_exact_workload( path ); } );
		run( "test_recording_restore_rejects_changed_source", [&] { test_recording_restore_rejects_changed_source( path ); } );
		run( "test_recording_restore_rejects_changed_geometry", [&] { test_recording_restore_rejects_changed_geometry( path ); } );
		run( "test_backend_replay", [&] { test_backend_replay( QString::fromLocal8Bit( argv[2] ) ); } );
		run( "test_openslide_statistics_do_not_throw", [&] {
			test_openslide_statistics_do_not_throw( QString::fromLocal8Bit( argv[2] ) );
		} );
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
