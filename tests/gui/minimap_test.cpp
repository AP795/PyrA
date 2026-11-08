#include "minimap.h"
#include "viewport.h"

#include <QApplication>
#include <QMouseEvent>

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


void near ( double actual, double expected, double tolerance = 1e-9 )
{
	require( std::abs( actual - expected ) <= tolerance, "Coordinate mapping mismatch" );
}


void send_mouse ( QWidget &widget, QEvent::Type type, QPointF position,
	Qt::MouseButton button, Qt::MouseButtons buttons )
{
	auto event = QMouseEvent( type, position, position, button, buttons, Qt::NoModifier );
	QApplication::sendEvent( &widget, &event );
}


void test_minimap_wide_letterbox_and_outline ()
{
	const auto map = pyra::test::make_minimap_mapping( { 100, -50, 400, 100 }, { 10, 20, 300, 200 } );
	require( map.valid(), "Mapping is invalid" );
	near( map.image.left(), 10 );
	near( map.image.top(), 82.5 );
	near( map.image.width(), 300 );
	near( map.image.height(), 75 );
	const auto outline = map.position_rect( { 200, -25, 100, 50 } );
	near( outline.left(), 85 );
	near( outline.top(), 101.25 );
	near( outline.width(), 75 );
	near( outline.height(), 37.5 );
}


void test_minimap_tall_negative_origin_and_clamping ()
{
	const auto map = pyra::test::make_minimap_mapping( { -40, 125, 100, 400 }, { 0, 0, 300, 200 } );
	near( map.image.left(), 125 );
	near( map.image.width(), 50 );
	const auto first = map.metric_at( { -100, -100 } );
	near( first.x(), -40 );
	near( first.y(), 125 );
	const auto last = map.metric_at( { 400, 300 } );
	near( last.x(), 60 );
	near( last.y(), 525 );
}


void test_minimap_fractional_large_extent_round_trip ()
{
	const auto map = pyra::test::make_minimap_mapping(
		{ -250000, 150000, 1e8, 2e8 }, { 0, 0, 256, 256 } );
	const auto position = QPointF( 151.237, 97.819 );
	const auto metric = map.metric_at( position );
	const auto round_trip = map.position_at( metric );
	near( round_trip.x(), position.x() );
	near( round_trip.y(), position.y() );
	near( metric.x(), -250000 + ( position.x() - 64 ) / 0.00000128, 1e-7 );
	near( metric.y(), 150000 + position.y() / 0.00000128, 1e-7 );
}


void test_minimap_empty_geometry ()
{
	require( !pyra::test::make_minimap_mapping( {}, { 0, 0, 100, 100 } ).valid(),
		"Empty slide has a valid map" );
	require( !pyra::test::make_minimap_mapping( { 1, 2, 3, 4 }, {} ).valid(),
		"Empty widget has a valid map" );
}


void test_minimap_mouse_release_centers_exactly_preserving_zoom ()
{
	auto minimap = pyra::test::Minimap{};
	auto viewport = pyra::test::Viewport{};
	minimap.resize( 300, 240 );
	viewport.resize( 700, 400 );
	const auto bounds = QRectF( -450, 120, 2000, 1000 );
	minimap.set_plane( bounds );
	viewport.set_plane( bounds, 1.0 );
	viewport.native_size();
	QObject::connect( &minimap, &pyra::test::Minimap::center_requested,
		&viewport, &pyra::test::Viewport::center_on );
	const auto transform = minimap.mapping();
	send_mouse( minimap, QEvent::MouseButtonPress, transform.image.center(), Qt::LeftButton, Qt::LeftButton );
	send_mouse( minimap, QEvent::MouseMove, transform.image.bottomRight() + QPointF( 50, 50 ),
		Qt::NoButton, Qt::LeftButton );
	const auto final = transform.image.topLeft() + QPointF(
		transform.image.width() * 0.7543, transform.image.height() * 0.6147 );
	send_mouse( minimap, QEvent::MouseButtonRelease, final, Qt::LeftButton, Qt::NoButton );
	const auto center = viewport.view_rect().center();
	near( center.x(), bounds.left() + bounds.width() * 0.7543 );
	near( center.y(), bounds.top() + bounds.height() * 0.6147 );
	near( viewport.mpp(), 1.0 );
}


void test_minimap_letterbox_press_does_not_navigate ()
{
	auto minimap = pyra::test::Minimap{};
	minimap.resize( 400, 240 );
	minimap.set_plane( { 0, 0, 100, 1000 } );
	auto requests = 0;
	QObject::connect( &minimap, &pyra::test::Minimap::center_requested,
		[&requests] ( QPointF ) { ++requests; } );
	send_mouse( minimap, QEvent::MouseButtonPress, { 1, 1 }, Qt::LeftButton, Qt::LeftButton );
	send_mouse( minimap, QEvent::MouseMove, minimap.mapping().image.center(), Qt::NoButton, Qt::LeftButton );
	send_mouse( minimap, QEvent::MouseButtonRelease, minimap.mapping().image.center(), Qt::LeftButton, Qt::NoButton );
	require( requests == 0, "Letterbox started navigation" );
}


void test_minimap_plane_change_clears_image_and_disabled_drag ()
{
	auto minimap = pyra::test::Minimap{};
	minimap.set_plane( { 0, 0, 100, 100 } );
	minimap.set_overview( QImage( 8, 8, QImage::Format_RGB32 ) );
	require( minimap.has_overview(), "Overview was not installed" );
	auto requests = 0;
	QObject::connect( &minimap, &pyra::test::Minimap::center_requested,
		[&requests] ( QPointF ) { ++requests; } );
	send_mouse( minimap, QEvent::MouseButtonPress, minimap.mapping().image.center(), Qt::LeftButton, Qt::LeftButton );
	require( requests == 1, "Press did not navigate" );
	minimap.setEnabled( false );
	minimap.setEnabled( true );
	send_mouse( minimap, QEvent::MouseButtonRelease, minimap.mapping().image.center(), Qt::LeftButton, Qt::NoButton );
	require( requests == 1, "Disabled drag retained navigation state" );
	minimap.set_plane( { 50, 100, 500, 1000 } );
	require( !minimap.has_overview(), "Changing planes retained the old thumbnail" );
}

} // namespace


int main ( int argc, char **argv )
{
	auto application = QApplication( argc, argv );
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
	run( "test_minimap_wide_letterbox_and_outline", test_minimap_wide_letterbox_and_outline );
	run( "test_minimap_tall_negative_origin_and_clamping", test_minimap_tall_negative_origin_and_clamping );
	run( "test_minimap_fractional_large_extent_round_trip", test_minimap_fractional_large_extent_round_trip );
	run( "test_minimap_empty_geometry", test_minimap_empty_geometry );
	run( "test_minimap_mouse_release_centers_exactly_preserving_zoom", test_minimap_mouse_release_centers_exactly_preserving_zoom );
	run( "test_minimap_letterbox_press_does_not_navigate", test_minimap_letterbox_press_does_not_navigate );
	run( "test_minimap_plane_change_clears_image_and_disabled_drag", test_minimap_plane_change_clears_image_and_disabled_drag );
	return failed == 0 ? 0 : 1;
}
