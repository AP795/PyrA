#pragma once

#include "wsi_session.h"

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstddef>
#include <tuple>
#include <vector>

namespace pyra::test {

inline constexpr auto kMaxRenderEdge = 4096;
inline constexpr auto kOverviewEdge = 256;

struct RenderRequest {
	QRectF xywh;
	QSize output;
	float mpp = 1.0f;
	priml::PlaneSel plane;

	bool operator== ( const RenderRequest & ) const = default;
};

struct PlaneInfo {
	priml::PlaneSel plane;
	QRectF bounds;
	float native_mpp = 1.0f;
	QString label;
};

struct OpenInfo {
	QString error;
	QString metadata;
	std::vector<PlaneInfo> planes;
};

struct RenderResult {
	RenderRequest request;
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

struct ReplayReport {
	QString error;
	TimingStats timing;
	std::size_t recorded = 0;
	std::size_t failed = 0;
	std::size_t incomplete = 0;
	bool cancelled = false;
	double wall_ms = 0.0;
	QImage last_image;
	RenderRequest last_request;
	QString text () const;
};

// The GUI dispatches every method except cancel_replay on one worker thread.
// Record/Stop are ordered with render calls on that same thread.
class RenderEvaluator {
public:
	OpenInfo open ( const QString &path );
	RenderResult render ( const RenderRequest &request, bool display = true );
	RenderResult overview ( const priml::PlaneSel &plane );
	void record ();
	std::size_t stop ();
	ReplayReport replay ();
	void cancel_replay () noexcept;
	void prepare_replay () noexcept;
	const std::vector<RenderRequest> &recorded () const { return recorded_; }

private:
	enum class RenderPurpose { Viewport, Overview };
	using RenderCaches = std::tuple<priml::Render<priml::Rgb>, priml::Render<priml::Rgb16>,
		priml::Render<uint8_t>, priml::Render<uint16_t>, priml::Render<float>>;
	RenderResult render_frame ( const RenderRequest &request, bool display, RenderPurpose purpose );

	priml::WsiSession session_;
	RenderCaches caches_;
	RenderCaches overview_caches_;
	std::vector<RenderRequest> recorded_;
	bool recording_ = false;
	std::atomic<bool> cancel_{ false };
};

} // namespace pyra::test
