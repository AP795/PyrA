#include "viewport.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace pyra::test {

Viewport::Viewport ( QWidget *parent ) : QWidget( parent )
{
	setFocusPolicy( Qt::StrongFocus );
	setMinimumSize( fontMetrics().horizontalAdvance( QLatin1Char( 'M' ) ) * 24,
		fontMetrics().lineSpacing() * 12 );
	setAccessibleName( tr( "Slide viewport" ) );
	setToolTip( tr( "Drag to pan; wheel or +/- to zoom; F to fit; 1 for native size." ) );
}


void Viewport::set_plane ( const QRectF &bounds, double native_mpp )
{
	bounds_ = bounds;
	native_mpp_ = native_mpp;
	image_ = {};
	fit();
	update();
}


void Viewport::set_frame ( const QImage &image, const QRectF &rect )
{
	image_ = image;
	image_rect_ = rect;
	update();
}


QRectF Viewport::view_rect () const
{
	const auto extent = QSizeF( width() * mpp_, height() * mpp_ );
	return QRectF( center_ - QPointF( extent.width() / 2, extent.height() / 2 ), extent );
}


void Viewport::changed ()
{
	if (bounds_.isEmpty())
		return;
	center_.setX( std::clamp( center_.x(), bounds_.left(), bounds_.right() ) );
	center_.setY( std::clamp( center_.y(), bounds_.top(), bounds_.bottom() ) );
	update();
	emit view_changed();
}


void Viewport::fit ()
{
	if (bounds_.isEmpty())
		return;
	center_ = bounds_.center();
	mpp_ = std::max( bounds_.width() / std::max( 1, width() ),
		bounds_.height() / std::max( 1, height() ) );
	changed();
}


void Viewport::native_size ()
{
	mpp_ = native_mpp_;
	changed();
}


void Viewport::zoom ( double factor, QPointF anchor )
{
	if (bounds_.isEmpty())
		return;
	const auto offset = anchor - QPointF( width() / 2.0, height() / 2.0 );
	const auto world = center_ + offset * mpp_;
	const auto limit = std::max( native_mpp_, std::max( bounds_.width(), bounds_.height() ) );
	mpp_ = std::clamp( mpp_ * factor, native_mpp_ / 32.0, limit );
	center_ = world - offset * mpp_;
	changed();
}


void Viewport::pan ( QPointF pixels )
{
	center_ -= pixels * mpp_;
	changed();
}


void Viewport::center_on ( QPointF metric )
{
	center_ = metric;
	changed();
}


void Viewport::paintEvent ( QPaintEvent * )
{
	auto painter = QPainter( this );
	painter.fillRect( rect(), palette().dark() );
	if (image_.isNull()) {
		painter.setPen( palette().brightText().color() );
		painter.drawText( rect(), Qt::AlignCenter, tr( "Open a slide to begin" ) );
		return;
	}
	const auto world = view_rect();
	const auto target = QRectF( ( image_rect_.topLeft() - world.topLeft() ) / mpp_,
		QSizeF( image_rect_.width() / mpp_, image_rect_.height() / mpp_ ) );
	painter.setRenderHint( QPainter::SmoothPixmapTransform );
	painter.drawImage( target, image_ );
}


void Viewport::resizeEvent ( QResizeEvent * )
{
	changed();
}


void Viewport::mousePressEvent ( QMouseEvent *event )
{
	if (event->button() == Qt::LeftButton) {
		dragging_ = true;
		last_mouse_ = event->position();
		setCursor( Qt::ClosedHandCursor );
		setFocus();
	}
}


void Viewport::mouseMoveEvent ( QMouseEvent *event )
{
	if (dragging_) {
		pan( event->position() - last_mouse_ );
		last_mouse_ = event->position();
	}
}


void Viewport::mouseReleaseEvent ( QMouseEvent *event )
{
	if (event->button() == Qt::LeftButton) {
		dragging_ = false;
		unsetCursor();
		changed();
	}
}


void Viewport::wheelEvent ( QWheelEvent *event )
{
	zoom( std::pow( 1.2, -event->angleDelta().y() / 120.0 ), event->position() );
	event->accept();
}


void Viewport::keyPressEvent ( QKeyEvent *event )
{
	switch (event->key()) {
	case Qt::Key_F: fit(); break;
	case Qt::Key_1: native_size(); break;
	case Qt::Key_Plus: case Qt::Key_Equal: zoom( 1.0 / 1.2, rect().center() ); break;
	case Qt::Key_Minus: zoom( 1.2, rect().center() ); break;
	case Qt::Key_Left: pan( { width() / 4.0, 0 } ); break;
	case Qt::Key_Right: pan( { -width() / 4.0, 0 } ); break;
	case Qt::Key_Up: pan( { 0, height() / 4.0 } ); break;
	case Qt::Key_Down: pan( { 0, -height() / 4.0 } ); break;
	default: QWidget::keyPressEvent( event ); return;
	}
	event->accept();
}

} // namespace pyra::test
