#include "minimap.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>

namespace pyra::test {

MinimapMapping make_minimap_mapping ( const QRectF &bounds, const QRectF &available )
{
	if (bounds.isEmpty() || available.isEmpty())
		return {};
	const auto scale = std::min( available.width() / bounds.width(),
		available.height() / bounds.height() );
	const auto size = QSizeF( bounds.width() * scale, bounds.height() * scale );
	return { bounds, QRectF( available.center() - QPointF( size.width() / 2,
		size.height() / 2 ), size ), scale };
}


QPointF MinimapMapping::metric_at ( QPointF position ) const
{
	if (!valid())
		return {};
	return { std::clamp( bounds.left() + ( position.x() - image.left() ) / scale,
		bounds.left(), bounds.right() ),
		std::clamp( bounds.top() + ( position.y() - image.top() ) / scale,
			bounds.top(), bounds.bottom() ) };
}


QPointF MinimapMapping::position_at ( QPointF metric ) const
{
	return image.topLeft() + ( metric - bounds.topLeft() ) * scale;
}


QRectF MinimapMapping::position_rect ( const QRectF &metric ) const
{
	return QRectF( position_at( metric.topLeft() ),
		QSizeF( metric.width() * scale, metric.height() * scale ) );
}


Minimap::Minimap ( QWidget *parent ) : QWidget( parent )
{
	setAccessibleName( tr( "Slide minimap" ) );
	setToolTip( tr( "Click or drag on the overview to center the main view without changing zoom." ) );
	setMinimumHeight( fontMetrics().lineSpacing() * 5 );
	setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
}


QSize Minimap::sizeHint () const
{
	return { fontMetrics().averageCharWidth() * 34, fontMetrics().lineSpacing() * 10 };
}


MinimapMapping Minimap::mapping () const
{
	const auto margin = std::max( 1.0, fontMetrics().horizontalAdvance( QLatin1Char( 'M' ) ) / 4.0 );
	return make_minimap_mapping( bounds_, QRectF( rect() ).adjusted( margin, margin, -margin, -margin ) );
}


void Minimap::set_plane ( const QRectF &bounds )
{
	bounds_ = bounds;
	view_ = {};
	overview_ = {};
	error_.clear();
	dragging_ = false;
	unsetCursor();
	update();
}


void Minimap::set_overview ( const QImage &image, const QString &error )
{
	overview_ = image;
	error_ = error;
	update();
}


void Minimap::set_view ( const QRectF &view )
{
	view_ = view;
	update();
}


void Minimap::paintEvent ( QPaintEvent * )
{
	auto painter = QPainter( this );
	painter.fillRect( rect(), palette().window() );
	const auto transform = mapping();
	if (!transform.valid()) {
		painter.drawText( rect(), Qt::AlignCenter, tr( "Minimap" ) );
		return;
	}
	painter.save();
	painter.setClipRect( transform.image );
	painter.fillRect( transform.image, palette().dark() );
	if (!overview_.isNull()) {
		painter.setRenderHint( QPainter::SmoothPixmapTransform );
		painter.drawImage( transform.image, overview_ );
	} else {
		painter.setPen( palette().brightText().color() );
		painter.drawText( transform.image, Qt::AlignCenter | Qt::TextWordWrap,
			error_.isEmpty() ? tr( "Loading overview…" ) : tr( "Overview unavailable" ) );
	}
	const auto outline = transform.position_rect( view_ );
	auto fill = palette().highlight().color();
	fill.setAlpha( 40 );
	painter.fillRect( outline, fill );
	painter.setPen( QPen( Qt::black, 3 ) );
	painter.drawRect( outline );
	painter.setPen( QPen( Qt::white, 1 ) );
	painter.drawRect( outline );
	painter.restore();
	painter.setPen( palette().mid().color() );
	painter.drawRect( transform.image );
}


void Minimap::navigate ( QPointF position )
{
	const auto transform = mapping();
	if (transform.valid())
		emit center_requested( transform.metric_at( position ) );
}


void Minimap::mousePressEvent ( QMouseEvent *event )
{
	const auto transform = mapping();
	if (event->button() != Qt::LeftButton || !transform.valid() ||
		!transform.image.contains( event->position() ))
		return;
	dragging_ = true;
	setCursor( Qt::ClosedHandCursor );
	navigate( event->position() );
	event->accept();
}


void Minimap::mouseMoveEvent ( QMouseEvent *event )
{
	if (dragging_)
		navigate( event->position() );
}


void Minimap::mouseReleaseEvent ( QMouseEvent *event )
{
	if (event->button() == Qt::LeftButton && dragging_) {
		dragging_ = false;
		unsetCursor();
		navigate( event->position() );
		event->accept();
	}
}


void Minimap::changeEvent ( QEvent *event )
{
	if (event->type() == QEvent::EnabledChange && !isEnabled()) {
		dragging_ = false;
		unsetCursor();
	}
	QWidget::changeEvent( event );
}

} // namespace pyra::test
