#include "render_evaluator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <numeric>
#include <string>
#include <type_traits>

namespace pyra::test {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms ( Clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
}


template <typename T>
int display_sample ( T value, int range )
{
	const auto scaled = std::is_floating_point_v<T>
		? double( value ) * 255.0 : double( value ) * 255.0 / range;
	return std::isfinite( scaled ) ? int( std::clamp( scaled, 0.0, 255.0 ) ) : 0;
}


template <typename T>
QImage display_image ( const priml::Pwhl<T> &pixels, int range )
{
	auto image = QImage( pixels.w, pixels.h, QImage::Format_RGB32 );
	if (image.isNull())
		return {};
	for (auto y = 0; y < pixels.h; ++y) {
		auto *out = reinterpret_cast<QRgb *>( image.scanLine( y ) );
		const auto *in = pixels.rowp( y );
		for (auto x = 0; x < pixels.w; ++x) {
			if constexpr (priml::is_rgb_pixel_v<T>) {
				out[x] = qRgb( display_sample( in[x].r, range ),
					display_sample( in[x].g, range ), display_sample( in[x].b, range ) );
			} else {
				const auto gray = display_sample( in[x], range );
				out[x] = qRgb( gray, gray, gray );
			}
		}
	}
	return image;
}


bool valid_request ( const RenderRequest &request )
{
	return std::isfinite( request.mpp ) && request.mpp > 0.0f &&
		std::isfinite( request.xywh.x() ) && std::isfinite( request.xywh.y() ) &&
		std::isfinite( request.xywh.width() ) && std::isfinite( request.xywh.height() ) &&
		std::isfinite( request.xywh.right() ) && std::isfinite( request.xywh.bottom() ) &&
		request.xywh.width() > 0 && request.xywh.height() > 0 &&
		request.output.width() > 0 && request.output.height() > 0 &&
		request.output.width() <= kMaxRenderEdge && request.output.height() <= kMaxRenderEdge;
}

} // namespace


TimingStats summarize_timings ( std::vector<double> milliseconds )
{
	auto stats = TimingStats{};
	if (milliseconds.empty())
		return stats;
	std::sort( milliseconds.begin(), milliseconds.end() );
	stats.count = milliseconds.size();
	stats.total_ms = std::accumulate( milliseconds.begin(), milliseconds.end(), 0.0 );
	stats.mean_ms = stats.total_ms / stats.count;
	stats.min_ms = milliseconds.front();
	stats.max_ms = milliseconds.back();
	const auto middle = stats.count / 2;
	stats.median_ms = stats.count % 2 ? milliseconds[middle]
		: ( milliseconds[middle - 1] + milliseconds[middle] ) / 2;
	stats.p95_ms = milliseconds[std::size_t( std::ceil( 0.95 * stats.count ) ) - 1];
	stats.renders_per_second = stats.total_ms > 0 ? 1000.0 * stats.count / stats.total_ms : 0.0;
	return stats;
}


QString ReplayReport::text () const
{
	return QStringLiteral(
		"Replay: existing slide/cache state retained\n"
		"Recorded: %1   Attempted: %2\nFailed: %3   Incomplete: %4\n"
		"Cancelled: %5\n\nPyra render calls (all attempts):\n"
		"Total: %6 ms\nMean: %7 ms\nMedian: %8 ms\nP95: %9 ms\n"
		"Min / max: %10 / %11 ms\nThroughput: %12 renders/s\n\n"
		"Replay wall time: %13 ms\n"
		"Render timing excludes Qt conversion, painting, and interaction delays.\n%14" )
		.arg( qulonglong( recorded ) ).arg( qulonglong( timing.count ) )
		.arg( qulonglong( failed ) ).arg( qulonglong( incomplete ) )
		.arg( cancelled ? QStringLiteral( "yes" ) : QStringLiteral( "no" ) )
		.arg( timing.total_ms, 0, 'f', 3 ).arg( timing.mean_ms, 0, 'f', 3 )
		.arg( timing.median_ms, 0, 'f', 3 ).arg( timing.p95_ms, 0, 'f', 3 )
		.arg( timing.min_ms, 0, 'f', 3 ).arg( timing.max_ms, 0, 'f', 3 )
		.arg( timing.renders_per_second, 0, 'f', 2 ).arg( wall_ms, 0, 'f', 3 ).arg( error );
}


OpenInfo RenderEvaluator::open ( const QString &path )
{
	recording_ = false;
	recorded_.clear();
	caches_ = {};
	overview_caches_ = {};
	auto info = OpenInfo{};
	auto opened = false;
	try {
		const auto utf8 = path.toUtf8();
		opened = session_.open( std::filesystem::path( std::u8string( utf8.cbegin(), utf8.cend() ) ) );
	} catch (const std::exception &error) {
		session_.close();
		info.error = QString::fromUtf8( error.what() );
		return info;
	}
	if (!opened) {
		info.error = QStringLiteral( "Unable to open slide: %1" ).arg( path );
		return info;
	}
	const auto &pyramid = session_.pyramid();
	for (const auto &plane : pyramid.planes) {
		const auto geometry = session_.native_geometry( plane.plane );
		if (!geometry)
			continue;
		const auto &bounds = geometry->metric_bounds;
		info.planes.push_back( { plane.plane,
			QRectF( bounds.tl.x, bounds.tl.y, bounds.br.x - bounds.tl.x, bounds.br.y - bounds.tl.y ),
			geometry->render_mpp,
			QStringLiteral( "%1 | z=%2 | %3 levels" )
				.arg( plane.plane.channel.empty() ? QStringLiteral( "Default" )
					: QString::fromStdString( plane.plane.channel ) )
				.arg( plane.plane.zlevel ).arg( qulonglong( plane.ladder.size() ) ) } );
	}
	info.metadata = QStringLiteral( "%1\n\n" ).arg( path );
	for (const auto &property : pyramid.properties)
		info.metadata += QString::fromStdString( property.key ) + QStringLiteral( " = " ) +
			QString::fromStdString( property.value ) + QLatin1Char( '\n' );
	if (info.planes.empty())
		info.error = QStringLiteral( "No renderable image planes were found." );
	return info;
}


RenderResult RenderEvaluator::render ( const RenderRequest &request, bool display )
{
	return render_frame( request, display, RenderPurpose::Viewport );
}


RenderResult RenderEvaluator::overview ( const priml::PlaneSel &plane )
{
	const auto geometry = session_.native_geometry( plane );
	if (!session_.is_open() || !geometry) {
		auto result = RenderResult{};
		result.error = QStringLiteral( "No overview geometry is available for this plane." );
		return result;
	}
	const auto &bounds = geometry->metric_bounds;
	const auto rect = QRectF( bounds.tl.x, bounds.tl.y,
		bounds.br.x - bounds.tl.x, bounds.br.y - bounds.tl.y );
	const auto mpp = std::max( rect.width(), rect.height() ) / kOverviewEdge;
	const auto output = QSize( std::clamp( qRound( rect.width() / mpp ), 1, kOverviewEdge ),
		std::clamp( qRound( rect.height() / mpp ), 1, kOverviewEdge ) );
	return render_frame( { rect, output, float( mpp ), plane }, true, RenderPurpose::Overview );
}


RenderResult RenderEvaluator::render_frame ( const RenderRequest &request,
	bool display, RenderPurpose purpose )
{
	auto result = RenderResult{};
	result.request = request;
	if (!session_.is_open() || !valid_request( request )) {
		result.error = QStringLiteral( "No open slide or invalid render rectangle/dimensions." );
		return result;
	}
	const auto *plane = priml::find_plane( session_.pyramid(), request.plane );
	if (!plane) {
		result.error = QStringLiteral( "The requested channel/Z plane does not exist." );
		return result;
	}
	if (recording_ && purpose == RenderPurpose::Viewport)
		recorded_.push_back( request );
	auto &caches = purpose == RenderPurpose::Viewport ? caches_ : overview_caches_;
	const auto before = session_.pyramid().stats.incomplete_renders;
	auto start = Clock::now();
	auto render_finished = false;
	try {
		priml::visit_level_pixel_type( plane->pixel_type, [&] ( auto pixel ) {
			using Pixel = decltype( pixel );
			const auto rect = priml::Rtlbr<float>{
				{ float( request.xywh.left() ), float( request.xywh.top() ) },
				{ float( request.xywh.right() ), float( request.xywh.bottom() ) } };
			const auto size = priml::Dim2D<int>{ request.output.width(), request.output.height() };
			start = Clock::now();
			auto pixels = session_.loader()->Generate<Pixel>( session_.pyramid(),
				&std::get<priml::Render<Pixel>>( caches ), request.mpp, rect, &size, request.plane );
			result.render_ms = elapsed_ms( start );
			render_finished = true;
			if (!pixels.p || pixels.w != size.w || pixels.h != size.h) {
				result.error = QStringLiteral( "Pyra returned an empty or incorrectly sized frame." );
			} else if (display) {
				result.image = display_image( pixels, priml::plane_value_range( *plane ) );
				if (result.image.isNull())
					result.error = QStringLiteral( "Unable to allocate the display image." );
			}
		} );
	} catch (const std::exception &error) {
		if (!render_finished)
			result.render_ms = elapsed_ms( start );
		result.error = QString::fromUtf8( error.what() );
	}
	result.incomplete = session_.pyramid().stats.incomplete_renders != before;
	return result;
}


void RenderEvaluator::record ()
{
	recorded_.clear();
	recording_ = session_.is_open();
}


std::size_t RenderEvaluator::stop ()
{
	recording_ = false;
	return recorded_.size();
}


ReplayReport RenderEvaluator::replay ()
{
	recording_ = false;
	auto report = ReplayReport{};
	report.recorded = recorded_.size();
	if (!session_.is_open() || recorded_.empty()) {
		report.error = QStringLiteral( "Record at least one render on the currently open slide first." );
		return report;
	}
	auto times = std::vector<double>{};
	times.reserve( recorded_.size() );
	const auto start = Clock::now();
	for (auto i = std::size_t{ 0 }; i < recorded_.size(); ++i) {
		if (cancel_.load( std::memory_order_relaxed )) {
			report.cancelled = true;
			break;
		}
		auto frame = render( recorded_[i], i + 1 == recorded_.size() );
		times.push_back( frame.render_ms );
		report.failed += !frame.error.isEmpty();
		report.incomplete += frame.incomplete;
		if (!frame.error.isEmpty() && report.error.isEmpty())
			report.error = frame.error;
		if (!frame.image.isNull()) {
			report.last_image = std::move( frame.image );
			report.last_request = frame.request;
		}
	}
	report.wall_ms = elapsed_ms( start );
	report.timing = summarize_timings( std::move( times ) );
	return report;
}


void RenderEvaluator::cancel_replay () noexcept
{
	cancel_.store( true, std::memory_order_relaxed );
}


void RenderEvaluator::prepare_replay () noexcept
{
	cancel_.store( false, std::memory_order_relaxed );
}

} // namespace pyra::test
