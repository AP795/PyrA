#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>

namespace pyra::test {

class Viewport : public QWidget {
	Q_OBJECT
public:
	explicit Viewport ( QWidget *parent = nullptr );
	void set_plane ( const QRectF &bounds, double native_mpp );
	void set_frame ( const QImage &image, const QRectF &rect );
	void fit ();
	void native_size ();
	void zoom ( double factor, QPointF anchor );
	void pan ( QPointF pixels );
	void center_on ( QPointF metric );
	QRectF view_rect () const;
	double mpp () const { return mpp_; }

signals:
	void view_changed ();

protected:
	void paintEvent ( QPaintEvent * ) override;
	void resizeEvent ( QResizeEvent * ) override;
	void mousePressEvent ( QMouseEvent *event ) override;
	void mouseMoveEvent ( QMouseEvent *event ) override;
	void mouseReleaseEvent ( QMouseEvent *event ) override;
	void wheelEvent ( QWheelEvent *event ) override;
	void keyPressEvent ( QKeyEvent *event ) override;

private:
	void changed ();
	QRectF bounds_;
	QRectF image_rect_;
	QPointF center_;
	QPointF last_mouse_;
	QImage image_;
	double mpp_ = 1.0;
	double native_mpp_ = 1.0;
	bool dragging_ = false;
};

} // namespace pyra::test
