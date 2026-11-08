#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>

namespace pyra::test {

// One aspect-preserving mapping shared by painting, hit testing and navigation.
struct MinimapMapping {
	QRectF bounds;
	QRectF image;
	double scale = 0.0;
	bool valid () const { return scale > 0.0; }
	QPointF metric_at ( QPointF position ) const;
	QPointF position_at ( QPointF metric ) const;
	QRectF position_rect ( const QRectF &metric ) const;
};

MinimapMapping make_minimap_mapping ( const QRectF &bounds, const QRectF &available );

class Minimap : public QWidget {
	Q_OBJECT
public:
	explicit Minimap ( QWidget *parent = nullptr );
	void set_plane ( const QRectF &bounds );
	void set_overview ( const QImage &image, const QString &error = {} );
	void set_view ( const QRectF &view );
	MinimapMapping mapping () const;
	QSize sizeHint () const override;
	bool has_overview () const { return !overview_.isNull(); }

signals:
	void center_requested ( QPointF metric );

protected:
	void paintEvent ( QPaintEvent * ) override;
	void mousePressEvent ( QMouseEvent *event ) override;
	void mouseMoveEvent ( QMouseEvent *event ) override;
	void mouseReleaseEvent ( QMouseEvent *event ) override;
	void changeEvent ( QEvent *event ) override;

private:
	void navigate ( QPointF position );
	QRectF bounds_;
	QRectF view_;
	QImage overview_;
	QString error_;
	bool dragging_ = false;
};

} // namespace pyra::test
