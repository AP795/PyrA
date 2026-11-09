#pragma once

#include <pyra/pyra.h>

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstddef>
#include <vector>

namespace pyra::test {

inline constexpr auto kMaxRenderEdge = 4096;
inline constexpr auto kOverviewEdge = 256;
inline constexpr auto kViewOverscanFactor = 1.5;

struct PlaneSelection {
	QString channel;
	double zlevel = 0;

	bool operator== ( const PlaneSelection & ) const = default;
};

struct RenderRequest {
	QRectF xywh;
	double mpp = 1.0;
	PlaneSelection plane;

	bool operator== ( const RenderRequest & ) const = default;
};

// GUI-owned prepared images are bounded by kMaxRenderEdge on both axes.
RenderRequest prepare_view_request ( const RenderRequest &view );
bool view_request_is_covered ( const RenderRequest &prepared, const RenderRequest &view );

struct PlaneInfo {
	PlaneSelection plane;
	QRectF bounds;
	float native_mpp = 1.0f;
	QString label;
	uint32_t index = 0;
	pyra_pixel_format pixel_format = 0;
	double max_sample_value = 1;
};

struct OpenInfo {
	QString error;
	QString metadata;
	std::vector<PlaneInfo> planes;
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
	RenderEvaluator ();
	~RenderEvaluator ();
	RenderEvaluator ( const RenderEvaluator & ) = delete;
	RenderEvaluator &operator= ( const RenderEvaluator & ) = delete;
	OpenInfo open ( const QString &path );
	RenderResult render ( const RenderRequest &request, bool display = true );
	RenderResult overview ( const PlaneSelection &plane );
	void record ();
	std::size_t stop ();
	ReplayReport replay ();
	void cancel_replay () noexcept;
	void prepare_replay () noexcept;
	const std::vector<RenderRequest> &recorded () const { return recorded_; }

private:
	enum class RenderPurpose { Viewport, Overview };
	RenderResult render_frame ( const RenderRequest &request, bool display, RenderPurpose purpose );

	pyra_context *context_ = nullptr;
	pyra_wsi *wsi_ = nullptr;
	std::vector<PlaneInfo> planes_;
	std::vector<RenderRequest> recorded_;
	bool recording_ = false;
	std::atomic<bool> cancel_{ false };
};

} // namespace pyra::test
