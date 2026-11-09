#include "render_evaluator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

namespace pyra::test {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms ( Clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
}


template <typename T>
int display_sample ( T value, double range )
{
	const auto scaled = double( value ) * 255.0 / range;
	return std::isfinite( scaled ) ? int( std::clamp( scaled, 0.0, 255.0 ) ) : 0;
}


QImage display_image ( const std::vector<unsigned char> &pixels,
	const pyra_image_layout &layout, double range )
{
	if (range == 255 && ( layout.pixel_format == PYRA_PIXEL_RGB8 || layout.pixel_format == PYRA_PIXEL_GRAY8 )) {
		const auto rgb = layout.pixel_format == PYRA_PIXEL_RGB8;
		auto image = QImage( int( layout.width ), int( layout.height ),
			rgb ? QImage::Format_RGB888 : QImage::Format_Grayscale8 );
		if (image.isNull())
			return {};
		const auto row_bytes = size_t( layout.width ) * ( rgb ? 3 : 1 );
		for (uint32_t y = 0; y < layout.height; ++y)
			std::memcpy( image.scanLine( int( y ) ), pixels.data() + size_t( y ) * layout.row_stride_bytes, row_bytes );
		return image;
	}
	auto image = QImage( int( layout.width ), int( layout.height ), QImage::Format_RGB32 );
	if (image.isNull())
		return {};
	for (uint32_t y = 0; y < layout.height; ++y) {
		auto *out = reinterpret_cast<QRgb *>( image.scanLine( int( y ) ) );
		const auto *row = pixels.data() + size_t( y ) * layout.row_stride_bytes;
		for (uint32_t x = 0; x < layout.width; ++x) {
			const auto rgb = layout.pixel_format == PYRA_PIXEL_RGB8 || layout.pixel_format == PYRA_PIXEL_RGB16;
			int samples[3] = {};
			for (auto c = 0; c < ( rgb ? 3 : 1 ); ++c) {
				const auto index = size_t( x ) * ( rgb ? 3 : 1 ) + c;
				if (layout.pixel_format == PYRA_PIXEL_GRAY16 || layout.pixel_format == PYRA_PIXEL_RGB16) {
					uint16_t value;
					std::memcpy( &value, row + index * sizeof( value ), sizeof( value ) );
					samples[c] = display_sample( value, range );
				} else if (layout.pixel_format == PYRA_PIXEL_FLOAT32) {
					float value;
					std::memcpy( &value, row + index * sizeof( value ), sizeof( value ) );
					samples[c] = display_sample( value, range );
				} else {
					samples[c] = display_sample( row[index], range );
				}
			}
			out[x] = rgb ? qRgb( samples[0], samples[1], samples[2] ) : qRgb( samples[0], samples[0], samples[0] );
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
		request.xywh.width() > 0 && request.xywh.height() > 0;
}

} // namespace


RenderRequest prepare_view_request ( const RenderRequest &view )
{
	if (!valid_request( view ))
		return view;
	const auto width = std::ceil( view.xywh.width() / view.mpp );
	const auto height = std::ceil( view.xywh.height() / view.mpp );
	if (width > kMaxRenderEdge || height > kMaxRenderEdge)
		return view;
	const auto factor = std::min( { kViewOverscanFactor, kMaxRenderEdge / width, kMaxRenderEdge / height } );
	const auto raster_width = std::min( double( kMaxRenderEdge ), std::ceil( width * factor ) );
	const auto raster_height = std::min( double( kMaxRenderEdge ), std::ceil( height * factor ) );
	const auto center = view.xywh.center();
	auto prepared = view;
	const auto size = QSizeF( raster_width * view.mpp, raster_height * view.mpp );
	prepared.xywh = QRectF( center - QPointF( size.width() / 2, size.height() / 2 ), size );
	return prepared;
}


bool view_request_is_covered ( const RenderRequest &prepared, const RenderRequest &view )
{
	return valid_request( prepared ) && valid_request( view ) &&
		prepared.plane == view.plane && prepared.mpp == view.mpp && prepared.xywh.contains( view.xywh );
}


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
		"C API timing includes the output copy; excludes Qt conversion and painting.\n%14" )
		.arg( qulonglong( recorded ) ).arg( qulonglong( timing.count ) )
		.arg( qulonglong( failed ) ).arg( qulonglong( incomplete ) )
		.arg( cancelled ? QStringLiteral( "yes" ) : QStringLiteral( "no" ) )
		.arg( timing.total_ms, 0, 'f', 3 ).arg( timing.mean_ms, 0, 'f', 3 )
		.arg( timing.median_ms, 0, 'f', 3 ).arg( timing.p95_ms, 0, 'f', 3 )
		.arg( timing.min_ms, 0, 'f', 3 ).arg( timing.max_ms, 0, 'f', 3 )
		.arg( timing.renders_per_second, 0, 'f', 2 ).arg( wall_ms, 0, 'f', 3 ).arg( error );
}


RenderEvaluator::RenderEvaluator ()
{
	auto error = pyra_error{};
	if (pyra_context_create( nullptr, &context_, &error ) != PYRA_OK)
		throw std::runtime_error( error.message );
}


RenderEvaluator::~RenderEvaluator ()
{
	if (slide_)
		pyra_close( slide_, nullptr );
	pyra_context_destroy( context_, nullptr );
}


OpenInfo RenderEvaluator::open ( const QString &path )
{
	recording_ = false;
	recorded_.clear();
	planes_.clear();
	if (slide_)
		pyra_close( std::exchange( slide_, nullptr ), nullptr );
	auto info = OpenInfo{};
	auto error = pyra_error{};
	const auto utf8 = path.toUtf8();
	if (pyra_open( context_, utf8.constData(), &slide_, &error ) != PYRA_OK) {
		info.error = QString::fromUtf8( error.message );
		return info;
	}
	auto count = uint32_t{};
	if (pyra_get_plane_count( slide_, &count, &error ) != PYRA_OK) {
		info.error = QString::fromUtf8( error.message );
		return info;
	}
	for (uint32_t i = 0; i < count; ++i) {
		auto plane = pyra_plane_info{};
		plane.struct_size = sizeof( plane );
		if (pyra_get_plane_info( slide_, i, &plane, &error ) != PYRA_OK)
			continue;
		const auto name = QString::fromUtf8( plane.channel );
		const auto label = QStringLiteral( "%1 | z=%2 | %3 levels" )
			.arg( name.isEmpty() ? QStringLiteral( "Default" ) : name )
			.arg( plane.z_um ).arg( plane.level_count );
		info.planes.push_back( { { name, plane.z_um },
			QRectF( plane.bounds.x_um, plane.bounds.y_um, plane.bounds.width_um, plane.bounds.height_um ),
			float( plane.native_mpp ), label, i, plane.pixel_format, plane.max_sample_value } );
	}
	info.metadata = QStringLiteral( "%1\n\n" ).arg( path );
	if (pyra_get_property_count( slide_, &count, &error ) != PYRA_OK) {
		info.error = QString::fromUtf8( error.message );
		return info;
	}
	for (uint32_t i = 0; i < count; ++i) {
		auto property = pyra_property{};
		if (pyra_get_property( slide_, i, &property, &error ) == PYRA_OK)
			info.metadata += QString::fromUtf8( property.key ) + QStringLiteral( " = " ) +
				QString::fromUtf8( property.value ) + QLatin1Char( '\n' );
	}
	planes_ = info.planes;
	if (info.planes.empty())
		info.error = QStringLiteral( "No renderable image planes were found." );
	return info;
}


RenderResult RenderEvaluator::render ( const RenderRequest &request, bool display )
{
	return render_frame( request, display, RenderPurpose::Viewport );
}


RenderResult RenderEvaluator::overview ( const PlaneSelection &plane )
{
	const auto found = std::find_if( planes_.begin(), planes_.end(), [&] ( const auto &info ) {
		return info.plane == plane;
	} );
	if (!slide_ || found == planes_.end()) {
		auto result = RenderResult{};
		result.error = QStringLiteral( "No overview geometry is available for this plane." );
		return result;
	}
	const auto &rect = found->bounds;
	const auto mpp = std::max( rect.width(), rect.height() ) / kOverviewEdge;
	return render_frame( { rect, mpp, plane }, true, RenderPurpose::Overview );
}


RenderResult RenderEvaluator::render_frame ( const RenderRequest &request,
	bool display, RenderPurpose purpose )
{
	auto result = RenderResult{};
	result.request = request;
	if (!slide_ || !valid_request( request )) {
		result.error = QStringLiteral( "No open slide or invalid render rectangle/mpp." );
		return result;
	}
	const auto plane = std::find_if( planes_.begin(), planes_.end(), [&] ( const auto &info ) {
		return info.plane == request.plane;
	} );
	if (plane == planes_.end()) {
		result.error = QStringLiteral( "The requested channel/Z plane does not exist." );
		return result;
	}
	auto native = pyra_image_request{ sizeof( pyra_image_request ), plane->index,
		{ request.xywh.x(), request.xywh.y(), request.xywh.width(), request.xywh.height() }, request.mpp };
	auto layout = pyra_image_layout{};
	layout.struct_size = sizeof( layout );
	auto error = pyra_error{};
	if (pyra_get_image_layout( slide_, &native, &layout, &error ) != PYRA_OK) {
		result.error = QString::fromUtf8( error.message );
		return result;
	}
	if (layout.width > kMaxRenderEdge || layout.height > kMaxRenderEdge) {
		result.error = QStringLiteral( "Requested raster exceeds the viewer limit." );
		return result;
	}
	result.output = QSize( int( layout.width ), int( layout.height ) );
	if (recording_ && purpose == RenderPurpose::Viewport)
		recorded_.push_back( request );
	try {
		auto pixels = std::vector<unsigned char>( layout.capacity_bytes );
		auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
			pixels.data(), pixels.size(), layout.row_stride_bytes };
		const auto start = Clock::now();
		const auto status = pyra_get_image( slide_, &native, &buffer, &error );
		result.render_ms = elapsed_ms( start );
		result.incomplete = status == PYRA_INCOMPLETE;
		if (status != PYRA_OK && status != PYRA_INCOMPLETE) {
			result.error = QString::fromUtf8( error.message );
		} else if (display) {
			result.image = display_image( pixels, layout, plane->max_sample_value );
			if (result.image.isNull())
				result.error = QStringLiteral( "Unable to allocate the display image." );
		}
	} catch (const std::exception &exception) {
		result.error = QString::fromUtf8( exception.what() );
	}
	return result;
}


void RenderEvaluator::record ()
{
	recorded_.clear();
	recording_ = slide_ != nullptr;
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
	if (!slide_ || recorded_.empty()) {
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
