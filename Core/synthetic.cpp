// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "synthetic.h"

#include "dcm_series.h"
#include "dcm_write.h"
#include "ptf_write.h"
#include "pyr_rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

namespace priml {

namespace {

// Stain absorbance vectors, unit length, per unit concentration. DAB is Ruifrok &
// Johnston's; hematoxylin is fitted to the blue nuclei of tst_in/tissue1.TIF,
// which absorb more red than green - greyer than Ruifrok's violet.
struct Od3 {
	float r, g, b;
};
constexpr Od3 kHematoxylin{ 0.750f, 0.600f, 0.280f };
constexpr Od3 kDab{ 0.268f, 0.570f, 0.776f };
// Illumination of an empty field, per channel.
constexpr float kLight[3] = { 250.0f, 249.0f, 247.0f };
// Amplitude of the per-pixel sensor noise, grey levels.
constexpr float kSensorNoise = 1.5f;

// Defocus blur sigma per micron of focus error, and the spread of the cells' own
// depths around the in-focus depth.
constexpr double kDefocusSigmaPerUm = 1.4;
constexpr double kCellDepthSpreadUm = 0.5;
// Defocus blur stops growing here: an object this blurred is a faint smudge
// already, and the cap bounds the per-tile cost of deep stacks and wide steps.
constexpr double kBlurSigmaMaxUm = 6.0;
// Gaussian stand-in for the pixel's own footprint, in level pixels.
constexpr double kPixelSigma = 0.5;
// Optical blur of an in-focus object, microns: nothing in a real slide is a step.
constexpr double kOpticalSigmaUm = 0.35;

// Tissue layout, microns.
constexpr double kTissueFeatureUm = 500.0;
constexpr double kNestFeatureUm = 140.0;
constexpr double kClusterFeatureUm = 90.0;
constexpr double kFlowFeatureUm = 250.0;
constexpr double kFibreAlongUm = 14.0;
constexpr double kFibreAcrossUm = 1.6;
constexpr double kChromatinUm = 0.9;
// Low-frequency fields are sampled no further apart than this and interpolated.
constexpr double kFieldSampleUm = 16.0;
// Half-width of the soft edge of tumour nests, in nest-field units.
constexpr double kNestEdge = 0.06;
// Positive-cell clustering: contrast window of the cluster field, symmetric
// about 0.5 so the positive rate averages to the requested fraction.
constexpr double kClusterContrast = 0.15;

// Share of the grid squares holding a nucleus inside tumour nests and in stroma.
// The grid pitch is spacing * sqrt(kTumourOccupancy), so a nest holds exactly
// one nucleus per spacing^2.
constexpr double kTumourOccupancy = 0.85;
constexpr double kStromaOccupancy = 0.4;
// Tumour nuclei: semi-major axis spread around its mean, and minor/major ratio.
constexpr double kTumourSizeJitter = 0.18;
constexpr double kTumourAspectMin = 0.7;
// Background positivity of the stromal nuclei.
constexpr double kFibroPositiveRate = 0.04;
constexpr double kLymphoPositiveRate = 0.08;

// Tumour cytoplasm halo radius relative to its nucleus, and its own softness.
constexpr double kHaloScale = 1.9;
constexpr double kHaloSoftUm = 0.8;
// Largest fibroblast semi-axis, microns.
constexpr double kFibroLengthMaxUm = 7.0;

// Field salts, so independent fields do not share a lattice.
enum Salt : uint64_t {
	kSaltTissue = 1, kSaltNest, kSaltCluster, kSaltFlow, kSaltFibre, kSaltChromatin,
	kSaltCell, kSaltSensor
};


uint64_t mix64 ( uint64_t x ) noexcept
{
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}


uint64_t hash3 ( uint64_t seed, uint64_t salt, int64_t a, int64_t b ) noexcept
{
	auto h = mix64( seed ^ ( salt * 0x9E3779B97F4A7C15ull ) );
	h = mix64( h ^ ( static_cast<uint64_t>( a ) * 0xD6E8FEB86659FD93ull ) );
	return mix64( h ^ ( static_cast<uint64_t>( b ) * 0xA0761D6478BD642Full ) );
}


double unit ( uint64_t h ) noexcept
{
	return static_cast<double>( h >> 11 ) * ( 1.0 / 9007199254740992.0 );
}


// Smooth value noise in [0, 1) with unit lattice spacing.
double value_noise ( uint64_t seed, uint64_t salt, double x, double y ) noexcept
{
	const auto fx = std::floor( x );
	const auto fy = std::floor( y );
	const auto ix = static_cast<int64_t>( fx );
	const auto iy = static_cast<int64_t>( fy );
	auto tx = x - fx;
	auto ty = y - fy;
	tx = tx * tx * ( 3.0 - 2.0 * tx );
	ty = ty * ty * ( 3.0 - 2.0 * ty );
	const auto v00 = unit( hash3( seed, salt, ix, iy ) );
	const auto v10 = unit( hash3( seed, salt, ix + 1, iy ) );
	const auto v01 = unit( hash3( seed, salt, ix, iy + 1 ) );
	const auto v11 = unit( hash3( seed, salt, ix + 1, iy + 1 ) );
	const auto top = v00 + ( v10 - v00 ) * tx;
	const auto bottom = v01 + ( v11 - v01 ) * tx;
	return top + ( bottom - top ) * ty;
}


double fbm ( uint64_t seed, uint64_t salt, double x, double y, int octaves ) noexcept
{
	auto sum = 0.0;
	auto weight = 0.0;
	auto amplitude = 1.0;
	for (auto i = 0; i < octaves; ++i) {
		sum += amplitude * value_noise( seed, salt + 100 * i, x, y );
		weight += amplitude;
		amplitude *= 0.5;
		x *= 2.03;
		y *= 2.03;
	}
	return sum / weight;
}


double smoothstep ( double lo, double hi, double x ) noexcept
{
	const auto t = std::clamp( ( x - lo ) / ( hi - lo ), 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}


// Quantile of the two-octave fbm used by the nest field. Its distribution does
// not depend on the seed or salt, so one sorted sample serves every slide.
double nest_field_quantile ( double p )
{
	static const auto sorted = [] {
		constexpr auto kSamples = 1 << 16;
		constexpr auto kSpan = 4096.0;
		auto values = std::vector<double>( kSamples );
		for (auto i = 0; i < kSamples; ++i) {
			const auto x = kSpan * unit( hash3( kSynthDefaultSeed, 7, i, 0 ) );
			const auto y = kSpan * unit( hash3( kSynthDefaultSeed, 7, i, 1 ) );
			values[i] = fbm( kSynthDefaultSeed, 11, x, y, 2 );
		}
		std::sort( values.begin(), values.end() );
		return values;
	}();
	const auto at = std::clamp( p, 0.0, 1.0 ) * static_cast<double>( sorted.size() - 1 );
	return sorted[static_cast<size_t>( std::lround( at ) )];
}


// Everything that depends only on the slide, not on the tile.
struct SlideFrame {
	uint64_t seed = 0;
	double centre_x_um = 0.0;
	double centre_y_um = 0.0;
	double half_diagonal_um = 1.0;
	double focus_um = 0.0;
	double pitch_um = 1.0;
	double tumour_axis_um = 1.0;		// mean tumour semi-major axis
	double reach_um = 0.0;				// largest object extent from its centre
	double positive_fraction = 0.0;
	double tumour_fraction = 0.0;
	double nest_threshold = 0.5;
	double lymphocyte_fraction = 0.0;
};


SlideFrame make_frame ( const SyntheticSlideSpec &spec )
{
	auto frame = SlideFrame{};
	frame.seed = spec.seed;
	frame.centre_x_um = 0.5 * spec.size.w * spec.mpp;
	frame.centre_y_um = 0.5 * spec.size.h * spec.mpp;
	frame.half_diagonal_um = std::max( 1.0, std::hypot( frame.centre_x_um, frame.centre_y_um ) );
	frame.focus_um = spec.focus_fraction * ( spec.z_levels - 1 ) * spec.z_step_um;
	frame.pitch_um = spec.nucleus_spacing_um * std::sqrt( kTumourOccupancy );
// Equivalent diameter 2*sqrt(a*b) with b = a*w, w uniform in [kTumourAspectMin, 1]:
// its mean is 2*a_mean*E[sqrt(w)].
	const auto mean_sqrt_aspect = ( 1.0 - std::pow( kTumourAspectMin, 1.5 ) ) /
		( 1.5 * ( 1.0 - kTumourAspectMin ) );
	frame.tumour_axis_um = 0.5 * spec.nucleus_diameter_um / mean_sqrt_aspect;
	frame.reach_um = std::max( kHaloScale * frame.tumour_axis_um * ( 1.0 + kTumourSizeJitter ),
		kFibroLengthMaxUm );
	frame.positive_fraction = spec.positive_fraction;
	frame.tumour_fraction = spec.tumour_fraction;
	frame.nest_threshold = nest_field_quantile( 1.0 - spec.tumour_fraction );
	frame.lymphocyte_fraction = spec.lymphocyte_fraction;
	return frame;
}


// Tissue presence in [0, 1]: a noisy blob biased towards the slide centre, so
// even a small slide shows tissue and a large one has white margins.
double tissue_at ( const SlideFrame &frame, double u, double v ) noexcept
{
	const auto radial = std::hypot( u - frame.centre_x_um, v - frame.centre_y_um ) /
		frame.half_diagonal_um;
	const auto n = fbm( frame.seed, kSaltTissue, u / kTissueFeatureUm, v / kTissueFeatureUm, 3 ) +
		0.35 * ( 0.55 - radial );
	return smoothstep( 0.36, 0.42, n );
}


// Tumour nest membership in [0, 1]; its mean over the slide is tumour_fraction.
double nest_at ( const SlideFrame &frame, double u, double v ) noexcept
{
	if (frame.tumour_fraction <= 0.0 || frame.tumour_fraction >= 1.0)
		return frame.tumour_fraction <= 0.0 ? 0.0 : 1.0;
	return smoothstep( frame.nest_threshold - kNestEdge, frame.nest_threshold + kNestEdge,
		fbm( frame.seed, kSaltNest, u / kNestFeatureUm, v / kNestFeatureUm, 2 ) );
}


// Positive rate of the tumour nuclei at (u, v): positive_fraction modulated by
// a cluster field symmetric about 0.5, so it stays in [0, 1] and averages to
// positive_fraction while the positive cells still gather in patches.
double positive_rate_at ( const SlideFrame &frame, double u, double v ) noexcept
{
	const auto p = frame.positive_fraction;
	const auto s = smoothstep( 0.5 - kClusterContrast, 0.5 + kClusterContrast,
		fbm( frame.seed, kSaltCluster, u / kClusterFeatureUm, v / kClusterFeatureUm, 2 ) );
	return p + std::min( p, 1.0 - p ) * ( 2.0 * s - 1.0 );
}


double flow_at ( const SlideFrame &frame, double u, double v ) noexcept
{
	return 2.0 * std::numbers::pi *
		fbm( frame.seed, kSaltFlow, u / kFlowFeatureUm, v / kFlowFeatureUm, 2 );
}


enum class CellKind { Tumour, Fibroblast, Lymphocyte };


struct Cell {
	CellKind kind = CellKind::Tumour;
	bool positive = false;
	double x = 0.0, y = 0.0;		// centre, microns
	double a = 0.0, b = 0.0;		// semi-axes, microns
	double cos_t = 1.0, sin_t = 0.0;
	float hematoxylin = 0.0f;
	float dab = 0.0f;
	float halo_hematoxylin = 0.0f;
	float halo_dab = 0.0f;
	double depth_um = 0.0;
};


// The cell owned by grid square (gx, gy), if any. Pure function of the seed.
bool cell_at ( const SlideFrame &frame, int64_t gx, int64_t gy, Cell *cell )
{
	auto r = [&, n = uint64_t{ 0 }] () mutable {
		return unit( hash3( frame.seed, kSaltCell + 1000 * ++n, gx, gy ) );
	};
	*cell = Cell{};
	cell->x = ( static_cast<double>( gx ) + 0.1 + 0.8 * r() ) * frame.pitch_um;
	cell->y = ( static_cast<double>( gy ) + 0.1 + 0.8 * r() ) * frame.pitch_um;
	const auto tissue = tissue_at( frame, cell->x, cell->y );
	const auto nest = nest_at( frame, cell->x, cell->y );
	const auto tumour = r() < nest;
	const auto density = tumour ? kTumourOccupancy : kStromaOccupancy;
	if (r() >= tissue * density)
		return false;

	auto angle = 0.0;
	auto positive_rate = 0.0;
	if (tumour) {
		cell->a = frame.tumour_axis_um * ( 1.0 + kTumourSizeJitter * ( 2.0 * r() - 1.0 ) );
		cell->b = cell->a * ( kTumourAspectMin + ( 1.0 - kTumourAspectMin ) * r() );
		angle = std::numbers::pi * r();
		cell->hematoxylin = static_cast<float>( 0.38 + 0.3 * r() );
		positive_rate = positive_rate_at( frame, cell->x, cell->y );
	} else if (r() >= frame.lymphocyte_fraction) {
		cell->kind = CellKind::Fibroblast;
		cell->a = 3.5 + ( kFibroLengthMaxUm - 3.5 ) * r();
		cell->b = 1.2 + 0.8 * r();
		angle = flow_at( frame, cell->x, cell->y ) + 0.8 * ( r() + r() + r() - 1.5 );
		cell->hematoxylin = static_cast<float>( 0.55 + 0.35 * r() );
		positive_rate = kFibroPositiveRate;
	} else {
		cell->kind = CellKind::Lymphocyte;
		cell->a = 2.2 + 0.7 * r();
		cell->b = cell->a * ( 0.85 + 0.15 * r() );
		angle = std::numbers::pi * r();
		cell->hematoxylin = static_cast<float>( 0.95 + 0.35 * r() );
		positive_rate = kLymphoPositiveRate;
	}
	cell->positive = r() < positive_rate;
	if (cell->positive) {
		cell->dab = static_cast<float>( 0.9 + 0.9 * r() );
		cell->hematoxylin *= 0.5f;
	}
	if (tumour) {
		cell->halo_hematoxylin = 0.03f;
		cell->halo_dab = cell->dab > 0.0f ? 0.12f : 0.06f;
	}
	cell->cos_t = std::cos( angle );
	cell->sin_t = std::sin( angle );
	cell->depth_um = frame.focus_um + kCellDepthSpreadUm * ( r() - 0.5 );
	return true;
}


// One tile's working state: absorbance accumulators plus where it sits.
struct TileCanvas {
	int x0 = 0, y0 = 0, w = 0, h = 0;
	double metric = 1.0;		// microns per level pixel
	double z_um = 0.0;
	std::vector<float> hematoxylin;
	std::vector<float> dab;

	double u ( int x ) const noexcept { return ( x0 + x + 0.5 ) * metric; }
	double v ( int y ) const noexcept { return ( y0 + y + 0.5 ) * metric; }
};


// Blur sigma for an object at `depth_um`, in microns, including the pixel's own
// footprint.
double blur_sigma_um ( const TileCanvas &canvas, double depth_um ) noexcept
{
	const auto defocus = std::min( kBlurSigmaMaxUm,
		kDefocusSigmaPerUm * std::abs( canvas.z_um - depth_um ) );
	const auto pixel = kPixelSigma * canvas.metric;
	return std::sqrt( defocus * defocus + pixel * pixel + kOpticalSigmaUm * kOpticalSigmaUm );
}


// Stroma: pale absorbance with fibres along the flow field. Low-frequency
// fields come from a coarse grid, interpolated.
void paint_background ( const SlideFrame &frame, TileCanvas *canvas )
{
	const auto step = std::max( 1, static_cast<int>( kFieldSampleUm / canvas->metric ) );
	const auto gw = canvas->w / step + 2;
	const auto gh = canvas->h / step + 2;
	auto tissue = std::vector<double>( static_cast<size_t>( gw ) * gh );
	auto nest = std::vector<double>( tissue.size() );
	auto flow_c = std::vector<double>( tissue.size() );
	auto flow_s = std::vector<double>( tissue.size() );
	for (auto gy = 0; gy < gh; ++gy) {
		for (auto gx = 0; gx < gw; ++gx) {
			const auto u = canvas->u( gx * step );
			const auto v = canvas->v( gy * step );
			const auto i = static_cast<size_t>( gy ) * gw + gx;
			tissue[i] = tissue_at( frame, u, v );
			nest[i] = nest_at( frame, u, v );
			const auto flow = flow_at( frame, u, v );
			flow_c[i] = std::cos( flow );
			flow_s[i] = std::sin( flow );
		}
	}
	const auto sigma = blur_sigma_um( *canvas, frame.focus_um );
	const auto fibre_gain = 0.8 * std::exp( -( sigma * sigma ) / ( kFibreAcrossUm * kFibreAcrossUm ) );
	for (auto y = 0; y < canvas->h; ++y) {
		const auto gy = y / step;
		const auto fy = static_cast<double>( y % step ) / step;
		for (auto x = 0; x < canvas->w; ++x) {
			const auto gx = x / step;
			const auto fx = static_cast<double>( x % step ) / step;
			auto lerp = [&] ( const std::vector<double> &f ) {
				const auto i = static_cast<size_t>( gy ) * gw + gx;
				const auto top = f[i] + ( f[i + 1] - f[i] ) * fx;
				const auto bottom = f[i + gw] + ( f[i + gw + 1] - f[i + gw] ) * fx;
				return top + ( bottom - top ) * fy;
			};
			const auto t = lerp( tissue );
			if (t <= 0.0)
				continue;
			const auto u = canvas->u( x );
			const auto v = canvas->v( y );
			const auto c = lerp( flow_c );
			const auto s = lerp( flow_s );
			const auto along = ( u * c + v * s ) / kFibreAlongUm;
			const auto across = ( -u * s + v * c ) / kFibreAcrossUm;
			const auto fibre = 1.0 + fibre_gain *
				( 2.0 * fbm( frame.seed, kSaltFibre, along, across, 2 ) - 1.0 );
			const auto n = lerp( nest );
			const auto i = static_cast<size_t>( y ) * canvas->w + x;
			canvas->hematoxylin[i] += static_cast<float>( t * 0.07 * fibre * ( 1.0 - 0.4 * n ) );
			canvas->dab[i] += static_cast<float>( t * ( 0.1 * fibre * ( 1.0 - n ) + 0.1 * n ) );
		}
	}
}


// Gaussian-blurred ellipse, accumulated into the canvas as absorbance.
struct Blob {
	double x, y, a, b, cos_t, sin_t, sigma_um;
	float hematoxylin, dab;
	bool textured;
};


void splat ( const SlideFrame &frame, const Blob &blob, TileCanvas *canvas )
{
	const auto m = canvas->metric;
	const auto reach = ( blob.a + 3.0 * blob.sigma_um ) / m + 1.0;
	const auto cx = blob.x / m - canvas->x0 - 0.5;
	const auto cy = blob.y / m - canvas->y0 - 0.5;
	const auto xa = std::max( 0, static_cast<int>( std::floor( cx - reach ) ) );
	const auto xb = std::min( canvas->w, static_cast<int>( std::ceil( cx + reach ) ) + 1 );
	const auto ya = std::max( 0, static_cast<int>( std::floor( cy - reach ) ) );
	const auto yb = std::min( canvas->h, static_cast<int>( std::ceil( cy + reach ) ) + 1 );
	if (xa >= xb || ya >= yb)
		return;
	const auto edge_scale = 1.0 / ( std::numbers::sqrt2 * blob.sigma_um );
	const auto peak = std::erf( blob.a * edge_scale ) * std::erf( blob.b * edge_scale );
	const auto texture_gain = blob.textured
		? 0.45 * std::exp( -( blob.sigma_um * blob.sigma_um ) / ( kChromatinUm * kChromatinUm ) )
		: 0.0;
	for (auto y = ya; y < yb; ++y) {
		const auto dy = canvas->v( y ) - blob.y;
		for (auto x = xa; x < xb; ++x) {
			const auto dx = canvas->u( x ) - blob.x;
			const auto p = dx * blob.cos_t + dy * blob.sin_t;
			const auto q = -dx * blob.sin_t + dy * blob.cos_t;
			const auto radial = std::hypot( p, q );
			const auto scaled = std::hypot( p / blob.a, q / blob.b );
			const auto distance = scaled > 1e-9 ? radial * ( 1.0 - 1.0 / scaled ) : -blob.b;
			auto cover = 0.5 * std::erfc( distance * edge_scale ) * peak;
			if (cover < 1e-3)
				continue;
			if (texture_gain > 0.0) {
				cover *= 1.0 + texture_gain * ( 2.0 * value_noise( frame.seed, kSaltChromatin,
					canvas->u( x ) / kChromatinUm, canvas->v( y ) / kChromatinUm ) - 1.0 );
			}
			const auto i = static_cast<size_t>( y ) * canvas->w + x;
			canvas->hematoxylin[i] += static_cast<float>( cover * blob.hematoxylin );
			canvas->dab[i] += static_cast<float>( cover * blob.dab );
		}
	}
}


void paint_cells ( const SlideFrame &frame, TileCanvas *canvas )
{
	const auto max_defocus = std::min( kBlurSigmaMaxUm, kDefocusSigmaPerUm *
		( std::abs( canvas->z_um - frame.focus_um ) + kCellDepthSpreadUm ) );
	const auto reach_um = frame.reach_um + 3.0 * ( std::hypot( max_defocus, kOpticalSigmaUm ) +
		kHaloSoftUm + kPixelSigma * canvas->metric ) + frame.pitch_um;
	const auto u0 = canvas->x0 * canvas->metric - reach_um;
	const auto u1 = ( canvas->x0 + canvas->w ) * canvas->metric + reach_um;
	const auto v0 = canvas->y0 * canvas->metric - reach_um;
	const auto v1 = ( canvas->y0 + canvas->h ) * canvas->metric + reach_um;
	const auto gx0 = static_cast<int64_t>( std::floor( u0 / frame.pitch_um ) );
	const auto gx1 = static_cast<int64_t>( std::floor( u1 / frame.pitch_um ) );
	const auto gy0 = static_cast<int64_t>( std::floor( v0 / frame.pitch_um ) );
	const auto gy1 = static_cast<int64_t>( std::floor( v1 / frame.pitch_um ) );
	auto cell = Cell{};
	for (auto gy = gy0; gy <= gy1; ++gy) {
		for (auto gx = gx0; gx <= gx1; ++gx) {
			if (!cell_at( frame, gx, gy, &cell ))
				continue;
			const auto sigma = blur_sigma_um( *canvas, cell.depth_um );
			if (cell.halo_dab > 0.0f) {
				const auto halo_sigma = std::hypot( sigma, kHaloSoftUm );
				splat( frame, Blob{ cell.x, cell.y, cell.a * kHaloScale, cell.b * kHaloScale,
					cell.cos_t, cell.sin_t, halo_sigma, cell.halo_hematoxylin, cell.halo_dab, false },
					canvas );
			}
			splat( frame, Blob{ cell.x, cell.y, cell.a, cell.b, cell.cos_t, cell.sin_t, sigma,
				cell.hematoxylin, cell.dab, true }, canvas );
		}
	}
}


uint8_t to_byte ( float value ) noexcept
{
	return static_cast<uint8_t>( std::clamp( std::lround( value ), 0l, 255l ) );
}


void develop ( const SlideFrame &frame, const TileCanvas &canvas, int level, int z,
	Pwhl<Rgb8> *out )
{
	const auto salt = kSaltSensor + 64 * static_cast<uint64_t>( level ) + static_cast<uint64_t>( z );
	for (auto y = 0; y < canvas.h; ++y) {
		auto *row = out->p + static_cast<size_t>( y ) * out->l;
		for (auto x = 0; x < canvas.w; ++x) {
			const auto i = static_cast<size_t>( y ) * canvas.w + x;
			const auto h = canvas.hematoxylin[i];
			const auto d = canvas.dab[i];
			const auto noise = kSensorNoise * static_cast<float>(
				2.0 * unit( hash3( frame.seed, salt, canvas.x0 + x, canvas.y0 + y ) ) - 1.0 );
			row[x].r = to_byte( kLight[0] * std::exp( -( kHematoxylin.r * h + kDab.r * d ) ) + noise );
			row[x].g = to_byte( kLight[1] * std::exp( -( kHematoxylin.g * h + kDab.g * d ) ) + noise );
			row[x].b = to_byte( kLight[2] * std::exp( -( kHematoxylin.b * h + kDab.b * d ) ) + noise );
		}
	}
}


bool fail ( std::string *error, std::string message )
{
	if (error)
		*error = std::move( message );
	return false;
}


long long tile_count ( const Dim2D<int> &size, const Dim2D<int> &cell ) noexcept
{
	const auto tx = ( static_cast<long long>( size.w ) + cell.w - 1 ) / cell.w;
	const auto ty = ( static_cast<long long>( size.h ) + cell.h - 1 ) / cell.h;
	return tx * ty;
}


Dim2D<int> square_cell ( const SyntheticSlideSpec &spec ) noexcept
{
	return { spec.tile_edge, spec.tile_edge };
}


long long stack_tile_count ( const SyntheticSlideSpec &spec, int levels )
{
	auto total = 0ll;
	for (auto level = 0; level < levels; ++level)
		total += tile_count( synthetic_level_size( spec, level ), square_cell( spec ) );
	return total * spec.z_levels;
}


// Upper bound on the rendered-ahead tiles held at once.
constexpr size_t kBatchBytes = size_t{ 256 } << 20;


// The writers pull one cell - a tile, or a strip of an untiled image - at a time
// in row-major order. Rendering is the cost, so the cells about to be asked for
// are rendered a batch at a time on every core and handed out from there. Pixels
// do not depend on the batching.
class TileBatcher {
public:
	TileBatcher ( const SyntheticSlideSpec &spec, int level, int z, const Dim2D<int> &cell )
		: spec_( spec ), level_( level ), z_( z ), cell_( cell ),
		size_( synthetic_level_size( spec, level ) ),
		columns_( ( size_.w + cell.w - 1 ) / cell.w ),
		count_( tile_count( size_, cell ) ),
		threads_( static_cast<int>( std::max( 1u, std::thread::hardware_concurrency() ) ) )
	{
		const auto tile_bytes = static_cast<size_t>( cell.w ) * cell.h * sizeof( Rgb8 );
		batch_ = static_cast<int>( std::clamp<size_t>( kBatchBytes / tile_bytes, 1,
			static_cast<size_t>( 4 * threads_ ) ) );
	}

	void fetch ( const Rtlbr<int> &rect, Pwhl<Rgb8> *out ) {
		const auto index = static_cast<long long>( rect.tl.y / cell_.h ) * columns_ +
			rect.tl.x / cell_.w;
		if (index < first_ || index >= first_ + static_cast<long long>( tiles_.size() ))
			render_batch( index );
		const auto &tile = tiles_[static_cast<size_t>( index - first_ )];
		for (auto y = 0; y < tile.h; ++y)
			std::copy_n( tile.p + static_cast<size_t>( y ) * tile.l, tile.w,
				out->p + static_cast<size_t>( y ) * out->l );
	}

private:
	Rtlbr<int> rect_of ( long long index ) const {
		const auto tx = static_cast<int>( index % columns_ );
		const auto ty = static_cast<int>( index / columns_ );
		const auto x0 = tx * cell_.w;
		const auto y0 = ty * cell_.h;
		return { { x0, y0 }, { std::min( x0 + cell_.w, size_.w ),
			std::min( y0 + cell_.h, size_.h ) } };
	}

	void render_batch ( long long first ) {
		first_ = first;
		const auto n = static_cast<int>( std::min<long long>( batch_, count_ - first ) );
		tiles_.clear();
		tiles_.reserve( n );
		for (auto i = 0; i < n; ++i) {
			const auto rect = rect_of( first + i );
			tiles_.emplace_back( Dim2D<int>{ width( rect ), height( rect ) } );
		}
		auto next = std::atomic<int>{ 0 };
		auto work = [&] {
			for (auto i = next++; i < n; i = next++)
				render_synthetic_tile( spec_, level_, z_, rect_of( first + i ), &tiles_[i] );
		};
		auto workers = std::vector<std::jthread>{};
		for (auto t = 1; t < std::min( threads_, n ); ++t)
			workers.emplace_back( work );
		work();
	}

	const SyntheticSlideSpec &spec_;
	int level_, z_;
	Dim2D<int> cell_;
	Dim2D<int> size_;
	int columns_;
	long long count_;
	int threads_;
	int batch_ = 1;
	long long first_ = 0;
	std::vector<Pwhl<Rgb8>> tiles_;
};


// The base level of plane 0 as an untiled TIFF, strip by strip.
bool write_synthetic_image ( const std::string &filename_utf8, const SyntheticSlideSpec &spec,
	const SyntheticProgress &progress, std::string *error )
{
	auto strips = TiffStripSpec{};
	strips.size = spec.size;
	strips.rows_per_strip = kSynthStripRows;
	strips.codec = spec.codec == ImgCodec::Lzw ? TiffStripCodec::Lzw : TiffStripCodec::Jpeg;
	strips.quality = spec.quality;
	strips.mpp = spec.mpp;
	strips.software = "PyrA synthetic slide";
	const auto cell = Dim2D<int>{ spec.size.w, kSynthStripRows };
	const auto total = tile_count( spec.size, cell );
	auto batcher = TileBatcher( spec, 0, 0, cell );
	auto done = 0ll;
	const auto fetch = TiffStripFetch( [&] ( const Rtlbr<int> &rect, Pwhl<Rgb> *strip ) {
		batcher.fetch( rect, strip );
		++done;
		return !progress || progress( done, total );
	} );
	return tiff_image_write_strips( pyr::path_from_utf8( filename_utf8 ), strips, fetch, error );
}

} // namespace


bool synthetic_codec_allowed ( ImgCodec codec, bool untiled ) noexcept
{
	if (untiled)
		return codec == ImgCodec::Lzw || codec == ImgCodec::Jpeg;
	return codec == ImgCodec::Jpeg || codec == ImgCodec::Jp2k || codec == ImgCodec::Htj2k ||
		codec == ImgCodec::Jxl || codec == ImgCodec::JpegXr;
}


// libtiff always codes LZW and JPEG strips; tiles go through encode_tile.
bool synthetic_can_encode ( ImgCodec codec, bool untiled ) noexcept
{
	return synthetic_codec_allowed( codec, untiled ) &&
		( codec == ImgCodec::Lzw || img_codec_can_encode( codec ) );
}


bool synthetic_spec_valid ( const SyntheticSlideSpec &spec, std::string *error )
{
	if (spec.size.w < 1 || spec.size.h < 1 || spec.size.w > kSynthImageEdgeMax ||
		spec.size.h > kSynthImageEdgeMax)
	{
		return fail( error, std::format( "Image size must be 1..{} pixels per edge",
			kSynthImageEdgeMax ) );
	}
	const auto untiled = synthetic_untiled( spec );
	if (!untiled && ( spec.tile_edge < kSynthTileEdgeMin || spec.tile_edge > kSynthTileEdgeMax ||
		spec.tile_edge % kSynthTileEdgeStep != 0 ))
	{
		return fail( error, std::format( "Tile edge must be {} (untiled) or a multiple of {} in {}..{}",
			kSynthUntiledTileEdge, kSynthTileEdgeStep, kSynthTileEdgeMin, kSynthTileEdgeMax ) );
	}
	if (untiled && ( spec.size.w > kSynthUntiledEdgeMax || spec.size.h > kSynthUntiledEdgeMax )) {
		return fail( error, std::format( "An untiled image must be 1..{} pixels per edge",
			kSynthUntiledEdgeMax ) );
	}
	if (spec.z_levels < 1 || spec.z_levels > kSynthZLevelsMax)
		return fail( error, std::format( "Z levels must be 1..{}", kSynthZLevelsMax ) );
	if (untiled && spec.z_levels != 1)
		return fail( error, "An untiled image holds one focal plane: Z levels must be 1" );
	if (!std::isfinite( spec.mpp ) || spec.mpp < kSynthMppMin || spec.mpp > kSynthMppMax)
		return fail( error, std::format( "Microns per pixel must be {}..{}", kSynthMppMin, kSynthMppMax ) );
	if (!synthetic_codec_allowed( spec.codec, untiled )) {
		return fail( error, untiled ? "Compression of an untiled image must be LZW or JPEG"
			: spec.codec == ImgCodec::Lzw ? "LZW is for untiled images only (tile size 0)"
			: "Compression must be JPEG, JPEG 2000, HTJ2K, JPEG XL or JPEG XR" );
	}
	if (!synthetic_can_encode( spec.codec, untiled )) {
		const auto *option = spec.codec == ImgCodec::Jp2k ? "USE_OPENJPEG"
			: spec.codec == ImgCodec::Htj2k ? "USE_OPENJPH"
			: spec.codec == ImgCodec::JpegXr ? "USE_JXR" : "USE_JXL";
		return fail( error, std::format( "{} compression is unavailable: built without {}",
			img_codec_name( spec.codec ), option ) );
	}
	if (spec.codec == ImgCodec::Lzw) {
// Always lossless; the quality is not used.
	} else if (spec.quality == kImageLosslessQuality) {
		if (!img_codec_supports_lossless( spec.codec ))
			return fail( error, std::format( "{} has no lossless mode", img_codec_name( spec.codec ) ) );
	} else if (spec.quality < kImageQualityMin || spec.quality > kImageLossyQualityMax) {
		return fail( error, std::format( "Quality must be {}..{} or lossless",
			kImageQualityMin, kImageLossyQualityMax ) );
	}
	auto in_range = [] ( double value, double lo, double hi ) {
		return std::isfinite( value ) && value >= lo && value <= hi;
	};
	if (!in_range( spec.z_step_um, kSynthZStepMinUm, kSynthZStepMaxUm )) {
		return fail( error, std::format( "Focal plane step must be {}..{} um",
			kSynthZStepMinUm, kSynthZStepMaxUm ) );
	}
	if (!in_range( spec.focus_fraction, 0.0, 1.0 ))
		return fail( error, "Focus position must be 0..1 of the stack height" );
	if (!in_range( spec.nucleus_diameter_um, kSynthNucleusDiameterMinUm, kSynthNucleusDiameterMaxUm )) {
		return fail( error, std::format( "Nucleus diameter must be {}..{} um",
			kSynthNucleusDiameterMinUm, kSynthNucleusDiameterMaxUm ) );
	}
	if (!in_range( spec.nucleus_spacing_um, kSynthNucleusSpacingMinUm, kSynthNucleusSpacingMaxUm )) {
		return fail( error, std::format( "Nucleus spacing must be {}..{} um",
			kSynthNucleusSpacingMinUm, kSynthNucleusSpacingMaxUm ) );
	}
	if (!in_range( spec.positive_fraction, 0.0, 1.0 ))
		return fail( error, "Positive fraction must be 0..1" );
	if (!in_range( spec.tumour_fraction, 0.0, 1.0 ))
		return fail( error, "Tumour fraction must be 0..1" );
	if (!in_range( spec.lymphocyte_fraction, 0.0, 1.0 ))
		return fail( error, "Lymphocyte fraction must be 0..1" );
	return true;
}


int synthetic_level_count ( const SyntheticSlideSpec &spec )
{
	auto count = 1;
	if (synthetic_untiled( spec ))
		return count;
	auto size = spec.size;
	while (size.w > spec.tile_edge || size.h > spec.tile_edge) {
		size = { ( size.w + 1 ) / 2, ( size.h + 1 ) / 2 };
		++count;
	}
	return count;
}


Dim2D<int> synthetic_level_size ( const SyntheticSlideSpec &spec, int level )
{
	auto size = spec.size;
	for (auto i = 0; i < level; ++i)
		size = { ( size.w + 1 ) / 2, ( size.h + 1 ) / 2 };
	return size;
}


void render_synthetic_tile ( const SyntheticSlideSpec &spec, int level, int z,
	const Rtlbr<int> &rect, Pwhl<Rgb8> *out )
{
	const auto frame = make_frame( spec );
	auto canvas = TileCanvas{};
	canvas.x0 = rect.tl.x;
	canvas.y0 = rect.tl.y;
	canvas.w = width( rect );
	canvas.h = height( rect );
	canvas.metric = spec.mpp * std::ldexp( 1.0, level );
	canvas.z_um = z * spec.z_step_um;
	const auto pixels = static_cast<size_t>( canvas.w ) * canvas.h;
	canvas.hematoxylin.assign( pixels, 0.0f );
	canvas.dab.assign( pixels, 0.0f );
	paint_background( frame, &canvas );
	paint_cells( frame, &canvas );
	develop( frame, canvas, level, z, out );
}


SyntheticCensus synthetic_census ( const SyntheticSlideSpec &spec, const Rtlbr<int> &rect )
{
	auto census = SyntheticCensus{};
	const auto x0 = std::max( 0, rect.tl.x ) * spec.mpp;
	const auto y0 = std::max( 0, rect.tl.y ) * spec.mpp;
	const auto x1 = std::min( spec.size.w, rect.br.x ) * spec.mpp;
	const auto y1 = std::min( spec.size.h, rect.br.y ) * spec.mpp;
	if (x0 >= x1 || y0 >= y1)
		return census;
	const auto frame = make_frame( spec );
	const auto gx0 = static_cast<int64_t>( std::floor( x0 / frame.pitch_um ) );
	const auto gx1 = static_cast<int64_t>( std::floor( x1 / frame.pitch_um ) );
	const auto gy0 = static_cast<int64_t>( std::floor( y0 / frame.pitch_um ) );
	const auto gy1 = static_cast<int64_t>( std::floor( y1 / frame.pitch_um ) );
	auto diameter_sum = 0.0;
	auto cell = Cell{};
	for (auto gy = gy0; gy <= gy1; ++gy) {
		for (auto gx = gx0; gx <= gx1; ++gx) {
			if (!cell_at( frame, gx, gy, &cell ) || cell.x < x0 || cell.x >= x1 ||
				cell.y < y0 || cell.y >= y1)
			{
				continue;
			}
			switch (cell.kind) {
				case CellKind::Tumour:
					++census.tumour;
					census.tumour_positive += cell.positive ? 1 : 0;
					diameter_sum += 2.0 * std::sqrt( cell.a * cell.b );
					break;
				case CellKind::Fibroblast:
					++census.fibroblast;
					break;
				case CellKind::Lymphocyte:
					++census.lymphocyte;
					break;
			}
		}
	}
	if (census.tumour > 0)
		census.tumour_diameter_um = diameter_sum / static_cast<double>( census.tumour );
	return census;
}


bool write_synthetic_slide ( const std::string &filename_utf8,
	const SyntheticSlideSpec &spec, const SyntheticProgress &progress,
	std::string *error )
{
	if (!synthetic_spec_valid( spec, error ))
		return false;
	if (synthetic_untiled( spec ))
		return write_synthetic_image( filename_utf8, spec, progress, error );
	const auto levels = synthetic_level_count( spec );
	const auto total = stack_tile_count( spec, levels );

	auto writer = PtfWriter{};
	if (!writer.open( filename_utf8, error ))
		return false;
	auto done = 0ll;
	auto cancelled = false;
	auto remove_partial = [&] {
		writer.close( nullptr );
		auto ignored = std::error_code{};
		std::filesystem::remove( pyr::path_from_utf8( filename_utf8 ), ignored );
	};
	for (auto z = 0; z < spec.z_levels && !cancelled; ++z) {
		for (auto level = 0; level < levels && !cancelled; ++level) {
			auto level_spec = PtfLevelSpec{};
			level_spec.size = synthetic_level_size( spec, level );
			level_spec.tile = { spec.tile_edge, spec.tile_edge };
			level_spec.codec = spec.codec;
			level_spec.encode.quality = spec.quality;
			level_spec.tags.valid = true;
			level_spec.tags.pixel_type = LevelPixelType::Rgb8;
			level_spec.tags.zlevel = static_cast<float>( z * spec.z_step_um );
			level_spec.tags.mpp = spec.mpp * std::ldexp( 1.0, level );
			level_spec.tags.max_pixel_value = 255;
			auto batcher = TileBatcher( spec, level, z, square_cell( spec ) );
			auto fetch = PtfTileFetch<Rgb8>(
				[&] ( const Rtlbr<int> &rect, Pwhl<Rgb8> *tile ) {
					if (cancelled)
						return false;
					batcher.fetch( rect, tile );
					++done;
					if (progress && !progress( done, total ))
						cancelled = true;
					return true;
				} );
			if (!writer.add_level( level_spec, fetch, error )) {
				remove_partial();
				return false;
			}
		}
	}
	if (cancelled) {
		remove_partial();
		return fail( error, "cancelled" );
	}
	if (!writer.close( error )) {
		remove_partial();
		return false;
	}
	return true;
}


bool synthetic_dicom_can_encode ( ImgCodec codec ) noexcept
{
	return USE_DCMTK && ( codec == ImgCodec::Jpeg || codec == ImgCodec::Jp2k || codec == ImgCodec::Htj2k )
		&& img_codec_can_encode( codec );
}


bool write_synthetic_dicom ( const std::filesystem::path &entry, const SyntheticSlideSpec &spec,
	const SyntheticProgress &progress, std::string *error )
{
	if (!synthetic_dicom_can_encode( spec.codec ))
		return fail( error, "DICOM generation needs DCMTK and an enabled JPEG, JPEG 2000 or HTJ2K encoder" );
	if (synthetic_untiled( spec ))
		return fail( error, "A DICOM WSI needs a tile size; tile 0 (untiled) is TIFF only" );
	if (!synthetic_spec_valid( spec, error ))
		return false;
	const auto auxiliary = dcm_pyramid_aux_directory( entry );
	const auto destinations_are_free = [&] {
		for (const auto &path : { entry, auxiliary }) {
			auto ec = std::error_code{};
			const auto status = std::filesystem::symlink_status( path, ec );
			if (status.type() != std::filesystem::file_type::not_found) {
				const auto name = path.filename().u8string();
				return fail( error, "DICOM output already exists or cannot be inspected: " +
					std::string( name.begin(), name.end() ) );
			}
		}
		return true;
	};
	if (!destinations_are_free())
		return false;

	auto meta = DcmAcquisition{};
	meta.uid_root = default_dcm_uid_root();
	meta.name = "synthetic";
	const auto now = std::chrono::floor<std::chrono::seconds>( std::chrono::system_clock::now() );
	meta.acquisition_datetime = std::format( "{:%Y%m%d%H%M%S}+0000", now );
	meta.focus_method = "AUTO";
	meta.imaged_depth_um = spec.z_levels * spec.z_step_um;
	meta.illumination_color = "WHITE";
	meta.series_description = "Synthetic bright-field IHC";
	meta.derivation_description = "Procedurally generated tissue and focal planes";
	meta.acquisition_context = "Synthetic slide generation";
	const auto &identity = dcm_writer_identity();
	meta.manufacturer = identity.manufacturer;
	meta.model = identity.model;
	meta.serial = "SYNTHETIC";
	meta.software = identity.product;

	const auto stage_path = entry.parent_path() / ( ".pyra-generate-" + make_dcm_uid( meta.uid_root ) );
	auto ec = std::error_code{};
	if (!std::filesystem::create_directory( stage_path, ec ))
		return fail( error, "Cannot create DICOM staging directory: " + ( ec ? ec.message() : "already exists" ) );
	struct StagingDirectory {
		const std::filesystem::path &path;
		~StagingDirectory () {
			auto ignored = std::error_code{};
			std::filesystem::remove_all( path, ignored );
		}
	} staging{ stage_path };

	auto writer = DcmWriter{};
	if (!writer.open( staging.path, meta, error ))
		return false;
	const auto levels = synthetic_level_count( spec );
	const auto total = stack_tile_count( spec, levels );
	auto done = 0ll;
	auto stopped = false;
	const auto cancelled = [&] {
		if (!stopped && progress)
			stopped = !progress( done, total );
		return stopped;
	};
	const auto check_cancel = progress ? std::function<bool ()>( cancelled ) : std::function<bool ()>{};
	for (auto level = 0; level < levels; ++level) {
		auto output = DcmLevelSpec{};
		output.level = level;
		output.size = synthetic_level_size( spec, level );
		output.tile = { spec.tile_edge, spec.tile_edge };
		output.max_pixel_value = 255;
		output.mpp = std::ldexp( spec.mpp, level );
		output.codec = spec.codec;
		output.encode.quality = spec.quality;
		output.focal_planes = spec.z_levels;
		output.focal_step_um = spec.z_step_um;
		auto batcher = std::optional<TileBatcher>{};
		auto current_z = -1;
		const auto fetch = DcmFocalTileFetch<Rgb>( [&] ( int z, const auto &rect, auto *tile, auto *tile_error ) {
			if (z != current_z) {
				batcher.emplace( spec, level, z, square_cell( spec ) );
				current_z = z;
			}
			batcher->fetch( rect, tile );
			const auto w = width( rect );
			const auto h = height( rect );
// Extend the edge pixels into the fixed-size DICOM frame for JPEG subsampling.
			for (auto y = 0; y < h; ++y)
				std::fill( tile->rowp( y ) + w, tile->rowp( y ) + tile->w, tile->rowp( y )[w - 1] );
			for (auto y = h; y < tile->h; ++y)
				std::copy_n( tile->rowp( h - 1 ), tile->w, tile->rowp( y ) );
			++done;
			if (cancelled()) {
				if (tile_error)
					*tile_error = "cancelled";
				return DcmFetchResult::Error;
			}
			return DcmFetchResult::Pixels;
		} );
		if (!writer.add_focal_level( output, fetch, error, check_cancel ))
			return false;
	}
	if (!writer.close( error ) || !destinations_are_free())
		return false;
	auto stats = writer.stats();
	if (!publish_dcm_pyramid( entry, &stats, error, check_cancel ))
		return stopped ? fail( error, "cancelled" ) : false;
	return true;
}

} // namespace priml
