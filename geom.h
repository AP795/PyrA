#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace priml {

template <typename T>
struct Pxy {
	T x;
	T y;
};


template <typename T>
using Point = Pxy<T>;


template <typename T>
struct Dim2D {
	T w;
	T h;

	bool is_set () const { return w > T{ 0 } && h > T{ 0 }; }
};


//
// Axis-aligned rectangle, HALF-OPEN for every T: tl is inside, br is not.
//   width  = br.x - tl.x,  height = br.y - tl.y
//   empty  <=> tl.x >= br.x || tl.y >= br.y
// Rtlbr<int> (pixels): pixel (x, y) is inside iff tl.x <= x < br.x and
//   tl.y <= y < br.y, so width/height are pixel counts and {tl, tl} is empty.
//   The last pixel inside is br - 1.
// Rtlbr<float> (continuous coordinates, pixels or um): pixel i covers
//   [i, i + 1), so a pixel rectangle converts to float by value: no +1/-1.
// Rectangles that tile or abut share the edge: a.br.x == b.tl.x.
//
template <typename T>
struct Rtlbr {
	Pxy<T> tl;
	Pxy<T> br;

	bool is_empty () const { return tl.x >= br.x || tl.y >= br.y; }

	bool contains ( const Pxy<T> &p ) const {
		return p.x >= tl.x && p.x < br.x && p.y >= tl.y && p.y < br.y;
	}

	bool is_inside ( const Rtlbr &outer ) const {
		return tl.x >= outer.tl.x && tl.y >= outer.tl.y
			&& br.x <= outer.br.x && br.y <= outer.br.y;
	}

	bool is_inside ( const Dim2D<T> &dimensions ) const {
		return tl.x >= T{0} && tl.y >= T{0} &&
			br.x <= dimensions.w && br.y <= dimensions.h;
	}
};


template <typename T>
inline Rtlbr<T> IntersectRect ( const Rtlbr<T> &a, const Rtlbr<T> &b )
{
	return {
		{std::max( a.tl.x, b.tl.x ), std::max( a.tl.y, b.tl.y )},
		{std::min( a.br.x, b.br.x ), std::min( a.br.y, b.br.y )}
	};
}


// The smallest rectangle holding both a and b (their envelope). Callers start an
// envelope from the first member, never from an empty sentinel.
template <typename T>
inline Rtlbr<T> UnionRect ( const Rtlbr<T> &a, const Rtlbr<T> &b )
{
	return {
		{std::min( a.tl.x, b.tl.x ), std::min( a.tl.y, b.tl.y )},
		{std::max( a.br.x, b.br.x ), std::max( a.br.y, b.br.y )}
	};
}


// True when a and b share area: abutting rectangles (a.br.x == b.tl.x) do not.
template <typename T>
inline bool overlaps ( const Rtlbr<T> &a, const Rtlbr<T> &b )
{
	return !IntersectRect( a, b ).is_empty();
}


// Generic width/height for any type with .w / .h members.
// Overload for types that compute dimensions differently (e.g. Rtlbr).
template <typename T>
inline auto width ( const T &x ) -> decltype(x.w) { return x.w; }

template <typename T>
inline auto height ( const T &x ) -> decltype(x.h) { return x.h; }

template <typename T>
inline T width ( const Rtlbr<T> &r ) {
	return (r.br.x > r.tl.x) ? (r.br.x - r.tl.x) : T{0};
}

template <typename T>
inline T height ( const Rtlbr<T> &r ) {
	return (r.br.y > r.tl.y) ? (r.br.y - r.tl.y) : T{0};
}


// Check that two spatial objects have matching dimensions.
template <typename A, typename B>
bool are_wh_eq ( const A &a, const B &b ) {
	return width(a) == width(b) && height(a) == height(b);
}


// How far from a whole number a computed pixel count may land and still be taken
// as that whole number.
//
// A metric extent is nearly always built as pixels*mpp, so dividing it back ought
// to give a whole number. The multiply and the subtraction that produced it are
// float, and their error grows with the COORDINATE, not with the extent: a
// 1541-pixel field at x=0 divides back to exactly 1541, while the same field at
// x=28833 gives 1541.00146.
//
// Derived, not picked. The relative error of a float product is 2^-24; two of
// them subtract, so the ratio error is bounded by 2 * 2^-24 * coordinate. At the
// largest coordinate this library admits - 200000 level pixels - that is 0.024.
//
// The tolerance is absolute rather than relative to the extent because the error
// scales with POSITION, not with size: a small zone far from the origin carries
// the same noise as a large one, and this function is given only the extent.
//
// It is also kept as small as the job allows, because every 0.01 of slack is
// 0.01 px of a genuinely fractional extent snapped away instead of covered.
// Measured, both ways: the case this exists to fix - a 1541-pixel field at
// x=28833 - carries 0.0015, and 0.02 covers coordinates to 166000, past any real
// slide (41 mm at 0.25 um/px). Going wider starts changing requests that were
// never noisy: at 0.1 it repositioned a quarter of the render sweep, and at 0.03
// it turned a genuinely fractional 117.03-pixel extent into 117 where 118 covers.
//
// A slide beyond that coordinate would reintroduce the defect rather than
// silently mis-snap, and test_render_pixel_rect_round_trip is what would catch it.
inline constexpr float kPixelSnapTolerance = 0.02f;


// Canonical metric-extent -> pixel-span reduction for zone rasters.
// Every place that turns a physical extent (microns) at a given resolution
// (microns/pixel) into an integer pixel count MUST use this one rule so the
// image plane, its zone mask, and the display scene size agree exactly — a
// disagreement of even one pixel forces a mask reframe on the next edit and
// silently desynchronizes sidecars from the zone raster.
//
// The span is snapped to the nearest integer when the ratio is within
// kPixelSnapTolerance of a whole number; otherwise it covers the fractional
// extent (ceil). This is a pure function of its inputs, so it is stable across
// contexts as long as those inputs are — the historical zone-raster drift came
// from the zone bbox differing between Generate Zones and per-zone processing (a
// floor/ceil metric round-trip in slide storage), not from this rule; that is
// fixed at the source in bbox_wsi_from_metric_box_local.
//
// The tolerance used to be 1e-3, which is smaller than the noise it exists to
// absorb. A 1541-pixel field starting at x=28833 came back as 1542 - measured on
// 9 of 56 fields of one slide - and a zone raster one pixel larger than its own
// mask is what forces a reframe on the next edit.
inline int raster_span_from_metric ( float metric, float mpp )
{
	auto safe_mpp = mpp > 0.0f ? mpp : 1.0f;
	auto ratio = metric / safe_mpp;
	auto snapped = std::round( ratio );
	if (std::fabs( ratio - snapped ) < kPixelSnapTolerance)
		return std::max( 1, static_cast<int>( snapped ) );
	return std::max( 1, static_cast<int>( std::ceil( ratio ) ) );
}


// The metric sub-rectangle of a (half-open) pixel bounding box at a given
// resolution, built as {tl*mpp, br*mpp}. This exact construction (and its float
// evaluation order) is what raster_span_from_metric is tuned against, so every
// zone raster derivation shares it rather than re-inlining the multiplies.
inline Rtlbr<float> subrect_metric_from_bbox ( const Rtlbr<int> &bbox, float mpp )
{
	return Rtlbr<float>{
		{
			static_cast<float>( bbox.tl.x ) * mpp,
			static_cast<float>( bbox.tl.y ) * mpp
		},
		{
			static_cast<float>( bbox.br.x ) * mpp,
			static_cast<float>( bbox.br.y ) * mpp
		}
	};
}


// The pixel rectangle of tile (tx, ty) of a regular grid of `tile`-sized tiles
// over a `size` raster, the last row and column cut at the raster edge. Every
// loader that stores a level as a grid (TIFF, NDPI, DICOM TILED_FULL, the PTF
// writer) builds its tile rectangles through this; tiles are then addressed by
// that rectangle alone. Products in 64 bits, so a large grid cannot overflow.
inline Rtlbr<int> grid_tile_rect ( const Dim2D<int> &size, const Dim2D<int> &tile,
	int tx, int ty )
{
	const auto x0 = (long long)tx * tile.w;
	const auto y0 = (long long)ty * tile.h;
	return {
		{ (int)x0, (int)y0 },
		{ (int)std::min<long long>( size.w, x0 + tile.w ),
		  (int)std::min<long long>( size.h, y0 + tile.h ) } };
}


// A rectangle edge in metric units -> pixel index at `scale` metric/px.
//
// floor/ceil alone turn float noise into a one-pixel change of span. A rectangle
// built as pixel*metric - which is how a zone, a tile-aligned request and every
// caller that starts from pixels builds one - divides back to k plus or minus a
// few ulp, and at whole-slide magnitudes an ulp of that product is around 0.004
// of a pixel. Landing on k-0.004 makes floor say k-1 and the span comes back one
// wider than asked for. Measured on one converted slide before this: 9 of 56
// zone-sized rectangles came back the wrong size.
//
// So an edge within NOISE of a whole pixel is that whole pixel, "within noise"
// measured against the value's own precision: reconstruct the metric an
// exactly-k-pixel edge would have had and compare in metric units. A fixed
// tolerance cannot do this job - sized for the largest coordinate it snaps
// requests that genuinely fall between pixels (0.1 px repositioned a quarter of
// the render sweep). A request that really falls between pixels still gets the
// conservative cover: floor for a lower edge, ceil for an upper one.
inline int metric_edge_to_pixel ( float metric, float scale, bool upper )
{
	const auto value = metric / scale;
	const auto snapped = std::round( value );
	const auto exact = (float)( (double)snapped * (double)scale );
	const auto ulp = std::fabs(
		std::nextafter( metric, std::numeric_limits<float>::infinity() ) - metric );
	if (std::fabs( metric - exact ) <= 4.0f * ulp)
		return (int)snapped;
	return (int)( upper ? std::ceil( value ) : std::floor( value ) );
}


// The pixel a metric point falls in: pixel i covers [i, i+1), so this is a floor
// - a point at 9.6 px is in pixel 9. Labels, containment and Generate Zones all
// map a micron position to a pixel through here, so one label cannot be tested
// in two different pixels. A point within float noise of a pixel's left/top edge
// belongs to that pixel (the metric_edge_to_pixel snap): 5*mpp/mpp coming back as
// 4.9999997 is pixel 5, not 4.
inline Pxy<int> pixel_at_metric ( const Pxy<float> &metric, const Pxy<float> &origin,
	float mpp )
{
	const auto step = mpp > 0.0f ? mpp : 1.0f;
	return {
		metric_edge_to_pixel( metric.x - origin.x, step, false ),
		metric_edge_to_pixel( metric.y - origin.y, step, false ) };
}


// The (half-open) pixel box covering a metric rectangle at `mpp` - the inverse of
// subrect_metric_from_bbox, exact on its output.
inline Rtlbr<int> bbox_from_metric ( const Rtlbr<float> &metric, float mpp )
{
	const auto scale = mpp > 0.0f ? mpp : 1.0f;
	return {
		{ metric_edge_to_pixel( metric.tl.x, scale, false ),
		  metric_edge_to_pixel( metric.tl.y, scale, false ) },
		{ metric_edge_to_pixel( metric.br.x, scale, true ),
		  metric_edge_to_pixel( metric.br.y, scale, true ) } };
}


// A destination axis covers the complete requested metric interval, even when
// only part of it is imaged. Use its centres for both sampling and containment.
// Double intermediates avoid losing the subpixel offset at large coordinates.
struct RasterAxis {
	double origin;
	double step;
	int size;

	RasterAxis ( float first, float last, int count )
		: origin( first ), step( count > 0 ? ( (double)last - first ) / count : 0.0 ),
		  size( std::max( 0, count ) ) {}

	double centre ( int pixel ) const { return origin + ( pixel + 0.5 ) * step; }

	int source_pixel ( int pixel, float source_mpp ) const {
		return (int)std::floor( centre( pixel ) / source_mpp );
	}

// Binary search the same centre expression used by sampling. Inverting it with
// ceil can disagree at an exactly-on-centre edge after floating-point rounding.
	int first_at_or_after ( double edge ) const {
		auto lo = 0;
		auto hi = size;
		while (lo < hi) {
			const auto mid = lo + ( hi - lo ) / 2;
			if (centre( mid ) < edge)
				lo = mid + 1;
			else
				hi = mid;
		}
		return lo;
	}
};


struct RasterMapping {
	RasterAxis x;
	RasterAxis y;

	RasterMapping ( const Rtlbr<float> &metric, const Dim2D<int> &raster )
		: x( metric.tl.x, metric.br.x, raster.w ),
		  y( metric.tl.y, metric.br.y, raster.h ) {}

	Rtlbr<int> clip ( const Rtlbr<float> &keep ) const {
		const auto left = x.first_at_or_after( keep.tl.x );
		const auto top = y.first_at_or_after( keep.tl.y );
		return { { left, top },
			{ std::max( left, x.first_at_or_after( keep.br.x ) ),
			  std::max( top, y.first_at_or_after( keep.br.y ) ) } };
	}
};


// Half-open range whose pixel centres lie in keep, clamped to the raster.
inline Rtlbr<int> raster_range_from_metric_clip ( const Rtlbr<float> &raster_metric,
	const Rtlbr<float> &keep, const Dim2D<int> &raster )
{
	return RasterMapping( raster_metric, raster ).clip( keep );
}


} // namespace priml
