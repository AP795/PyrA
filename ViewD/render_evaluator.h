// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "viewer_recording.h"

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace pyra::test {

inline constexpr auto kMaxRenderEdge = 4096;
inline constexpr auto kOverviewEdge = 256;
inline constexpr auto kViewOverscanFactor = 1.5;

struct CacheUsage {
	pyra_context_options options = reader_options();
	std::optional<pyra_context_stats> stats;
	std::optional<pyra_cache_usage> occupancy;
	bool image_open = false;
};

// GUI-owned prepared images are bounded by kMaxRenderEdge on both axes.
RenderRequest prepare_view_request ( const RenderRequest &view );
bool view_request_is_covered ( const RenderRequest &prepared, const RenderRequest &view );

struct OpenInfo {
	QString error;
	QString metadata;
	std::vector<PlaneInfo> planes;
	QString backend;
	double open_ms = 0;
	std::size_t recorded = 0;
	bool recording_preserved = false;
	int default_plane_index = 0;
};

struct RenderResult {
	RenderRequest request;
	QSize output;
	QImage image;
	QString error;
	double render_ms = 0.0;
	bool incomplete = false;
};

struct TimingStats {
	std::size_t count = 0;
	double total_ms = 0.0;
	double mean_ms = 0.0;
	double min_ms = 0.0;
	double max_ms = 0.0;
	double median_ms = 0.0;
	double p95_ms = 0.0;
	double renders_per_second = 0.0;
};

TimingStats summarize_timings ( std::vector<double> milliseconds );
QString reader_settings_text ( const pyra_context_options &options );
QString cache_report_text ( const pyra_context_options &options, const pyra_context_stats &stats );

struct ReplayReport {
	QString error;
	pyra_context_options options = reader_options();
	QString backend;
	double open_ms = 0;
	TimingStats timing;
	pyra_context_stats cache = { sizeof( pyra_context_stats ), 0, 0, 0, 0, 0, 0, 0,
		PYRA_STAT_UNAVAILABLE, PYRA_STAT_UNAVAILABLE, PYRA_STAT_UNAVAILABLE, PYRA_STAT_UNAVAILABLE };
	std::size_t recorded = 0;
	std::size_t failed = 0;
	std::size_t incomplete = 0;
	bool cancelled = false;
	double wall_ms = 0.0;
	QImage last_image;
	RenderRequest last_request;
	QSize minimum_output;
	QSize maximum_output;
	QString text () const;
};

// The GUI dispatches every method except cancel_replay on one worker thread.
// Record/Stop are ordered with render calls on that same thread.
class RenderEvaluator {
public:
	explicit RenderEvaluator ( pyra_backend backend = PYRA_BACKEND_NATIVE );
	explicit RenderEvaluator ( const pyra_context_options &options );
	~RenderEvaluator ();
	RenderEvaluator ( const RenderEvaluator & ) = delete;
	RenderEvaluator &operator= ( const RenderEvaluator & ) = delete;
	OpenInfo open ( const QString &path, pyra_backend backend = PYRA_BACKEND_NATIVE );
	OpenInfo open ( const QString &path, const pyra_context_options &options );
	RenderResult render ( const RenderRequest &request, bool display = true );
	RenderResult overview ( const PlaneSelection &plane );
	void record ();
	std::size_t stop ();
	ReplayReport replay ();
	void cancel_replay () noexcept;
	void prepare_replay () noexcept;
	ViewerRecording recording () const;
	CacheUsage cache_usage () const;
	QString restore_recording ( const ViewerRecording &recording );
	const std::vector<RenderRequest> &recorded () const { return recorded_; }

private:
	enum class RenderPurpose { Viewport, Overview };
	OpenInfo load ( const QString &path );
	RenderResult render_frame ( const RenderRequest &request, bool display, RenderPurpose purpose );

	pyra_context *context_ = nullptr;
	pyra_context_options options_ = reader_options();
	pyra_wsi *wsi_ = nullptr;
	std::vector<PlaneInfo> planes_;
	std::vector<RenderRequest> recorded_;
	QString path_;
	qint64 source_size_ = -1;
	qint64 source_modified_ms_ = -1;
	int32_t last_view_level_ = -1;
	QString backend_description_;
	double open_ms_ = 0;
	bool recording_ = false;
	std::atomic<bool> cancel_{ false };
};

} // namespace pyra::test
