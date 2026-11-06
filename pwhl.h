#pragma once

#include "build_config.h"
#include "geom.h"
#include "pyr_rt.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>


namespace priml {

// Allocator whose no-argument construct() default-initialises instead of
// value-initialising, so vector::resize(n) leaves trivial elements untouched.
// resize(n, T{}) still fills, so a zeroing path stays available.
template <typename T>
struct default_init_allocator : std::allocator<T> {
	using std::allocator<T>::allocator;

	template <typename U> struct rebind { using other = default_init_allocator<U>; };

	template <typename U>
	void construct ( U *ptr ) noexcept( std::is_nothrow_default_constructible_v<U> ) {
		::new (static_cast<void *>( ptr )) U;
	}

	template <typename U, typename... Args>
	void construct ( U *ptr, Args &&...args ) {
		::new (static_cast<void *>( ptr )) U( std::forward<Args>( args )... );
	}
};


// Tag selecting the uninitialised Pwhl constructor. Only for buffers whose every
// pixel the caller then writes: zero-filling a 16 MB plane that is immediately
// overwritten costs more than the kernel producing it.
struct uninit_t { explicit uninit_t () = default; };
inline constexpr uninit_t uninit{};

struct Rgb8 {
	uint8_t r, g, b;
};

struct Rgb16 {
	uint16_t r, g, b;
};

using Rgb = Rgb8;

// Maximum pixel value for 16-bit images (12-bit sensor data).
constexpr int pixel_max_16 = 4095;


// Channel type: the scalar type underlying each pixel type.
template <typename T> struct channel_type_of { using type = T; };
template <> struct channel_type_of<Rgb8>  { using type = uint8_t; };
template <> struct channel_type_of<Rgb16> { using type = uint16_t; };
template <typename T> using channel_type = typename channel_type_of<T>::type;

// Whether a pixel type carries three colour components, of either width.
template <typename T>
inline constexpr bool is_rgb_pixel_v = std::is_same_v<T, Rgb8> || std::is_same_v<T, Rgb16>;

template <typename pixeltype> inline pixeltype RgbToLuminance ( pixeltype r, pixeltype g, pixeltype b ) {
	return (pixeltype)(0.299f * r + 0.587f * g + 0.114f * b);
}

// Tile-decode widening of an 8-bit sample: VALUE-preserving (200 stays 200), so
// a Gray8 plane read as 16-bit keeps its numbers. Auxiliary images use the other
// contract, full-range (x257), on purpose - see AuxiliaryPixelFromRgb.
inline uint16_t WidenByteToWord ( uint8_t value ) {
	return (uint16_t)value;
}

template <typename pixeltype>
inline pixeltype PixelFromGrayByte ( uint8_t value ) {
	if constexpr (std::is_same_v<pixeltype, Rgb8>)
		return Rgb8{value, value, value};
	else if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return Rgb16{WidenByteToWord(value), WidenByteToWord(value), WidenByteToWord(value)};
	else if constexpr (std::is_same_v<pixeltype, uint8_t>)
		return value;
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return WidenByteToWord( value );
	else
		return (pixeltype)(value / 255.0f);
}

// 16-bit grey to the target pixel type. Distinct from PixelFromGrayByte because
// the 8-bit targets have to lose the low byte, and doing that at the call site
// is how a fluorescence plane ends up looking black.
template <typename pixeltype>
inline pixeltype PixelFromGrayWord ( uint16_t value ) {
	if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return Rgb16{value, value, value};
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return value;
	else if constexpr (std::is_same_v<pixeltype, float>)
		return (pixeltype)(value / 65535.0f);
	else
		return PixelFromGrayByte<pixeltype>( (uint8_t)( value >> 8 ) );
}


template <typename pixeltype>
inline pixeltype PixelFromRgbBytes ( uint8_t r, uint8_t g, uint8_t b ) {
	if constexpr (std::is_same_v<pixeltype, Rgb8>)
		return Rgb8{r, g, b};
	else if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return Rgb16{WidenByteToWord(r), WidenByteToWord(g), WidenByteToWord(b)};
	else
		return PixelFromGrayByte<pixeltype>( RgbToLuminance<uint8_t>( r, g, b ) );
}


// 16-bit colour to the target pixel type - the colour twin of PixelFromGrayWord,
// with the same narrowing rule for the 8-bit targets (the low byte is dropped).
template <typename pixeltype>
inline pixeltype PixelFromRgbWords ( uint16_t r, uint16_t g, uint16_t b ) {
	if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return Rgb16{r, g, b};
	else if constexpr (std::is_same_v<pixeltype, Rgb8>)
		return Rgb8{(uint8_t)( r >> 8 ), (uint8_t)( g >> 8 ), (uint8_t)( b >> 8 )};
	else
		return PixelFromGrayWord<pixeltype>( RgbToLuminance<uint16_t>( r, g, b ) );
}

struct PwhlHead {
  int w = 0;
  int h = 0;
  int l = 0;

  bool is_set () const { return w > 0 && h > 0; }
};


// Lightweight non-owning view over a Pwhl-shaped raster. Instantiating with a
// const pixel type provides a read-only view without copying or const_cast.
template <typename pixeltype>
struct PwhlView : PwhlHead {
	using pixel_type = std::remove_const_t<pixeltype>;

	pixeltype *p = nullptr;

	PwhlView () = default;

	PwhlView ( const Dim2D<int> &dimensions, int line_pitch, pixeltype *pixels )
		: PwhlHead{ dimensions.w, dimensions.h,
			line_pitch > 0 ? line_pitch : dimensions.w },
		p( pixels )
	{
	}

	pixeltype *rowp ( int y ) const { return p + static_cast<std::size_t>( y ) * l; }
};


template <typename pixeltype> struct Pwhl : PwhlHead {
  using pixel_type = pixeltype;

  Pwhl *prn = nullptr;
  pixeltype *p = nullptr;
  std::vector<pixeltype, default_init_allocator<pixeltype>> buf;

  // Default constructor
  Pwhl() = default;

  // Constructor allocating own buffer, zero-filled.
  Pwhl(const Dim2D<int> &dimensions, int line_pitch = 0)
	  : PwhlHead{dimensions.w, dimensions.h,
		line_pitch > 0 ? line_pitch : dimensions.w},
		prn(nullptr) {
	buf.resize(l * h, pixeltype{});
	p = buf.data();
	stamp_pixels();
  }

  // Constructor allocating own buffer WITHOUT initialising it. Every pixel must
  // be written before it is read; see uninit_t.
  Pwhl(const Dim2D<int> &dimensions, uninit_t)
	  : Pwhl(dimensions, 0, uninit) {}

  Pwhl(const Dim2D<int> &dimensions, int line_pitch, uninit_t)
	  : PwhlHead{dimensions.w, dimensions.h,
		line_pitch > 0 ? line_pitch : dimensions.w},
		prn(nullptr) {
	buf.resize(l * h);
	p = buf.data();
	stamp_pixels();
  }

  // Constructor wrapping an external pointer (non-owning)
  Pwhl(const Dim2D<int> &dimensions, int line_pitch, pixeltype *ptr)
	  : PwhlHead{dimensions.w, dimensions.h,
		line_pitch > 0 ? line_pitch : dimensions.w},
		p(ptr) {
	stamp_pixels();
  }

  // Delete copy semantics
  Pwhl(const Pwhl &) = delete;
  Pwhl &operator=(const Pwhl &) = delete;

  // Move semantics — transfer cached lo/hi state.
  Pwhl(Pwhl &&other) noexcept
	  : PwhlHead(other), prn(other.prn),
		p(other.p), buf(std::move(other.buf)),
		pixels_ver_(other.pixels_ver_), lohi_ver_(other.lohi_ver_),
		lo_at_(other.lo_at_), hi_at_(other.hi_at_) {

	// If other owned its pixels, update p to point to our newly moved buffer
	if (!prn && other.p == other.buf.data()) {
	  p = buf.data();
	}

	other.w = 0;
	other.h = 0;
	other.l = 0;
	other.prn = nullptr;
	other.p = nullptr;
  }

  Pwhl &operator=(Pwhl &&other) noexcept {
	if (this != &other) {
	  w = other.w;
	  h = other.h;
	  l = other.l;
	  prn = other.prn;
	  p = other.p;
	  buf = std::move(other.buf);
	  pixels_ver_ = other.pixels_ver_;
	  lohi_ver_ = other.lohi_ver_;
	  lo_at_ = other.lo_at_;
	  hi_at_ = other.hi_at_;

	  if (!prn && other.p == other.buf.data()) {
		p = buf.data();
	  }

	  other.w = 0;
	  other.h = 0;
	  other.l = 0;
	  other.prn = nullptr;
	  other.p = nullptr;
	}
	return *this;
  }

  // Pixel access — non-const versions stamp pixel version.
  auto *rowp ( int y ) { stamp_pixels(); return p + y * l; }
  const auto *rowp ( int y ) const { return p + y * l; }

  template <typename Pt>
  auto *pxlp ( const Pt &pt ) { stamp_pixels(); return p + pt.y * l + pt.x; }
  template <typename Pt>
  const auto *pxlp ( const Pt &pt ) const { return p + pt.y * l + pt.x; }

  // Clone method (deep copy)
  Pwhl clone() const {
	Pwhl result({ w, h }, l);
	if (p) {
	  for (int y = 0; y < h; ++y)
		std::copy(rowp(y), rowp(y) + w, result.p + y * result.l);
	}
	return result;
  }

  // Span method (returns subrectangle pointing into this Pwhl).
  // Returns empty Pwhl if the rectangle is out of bounds.
  Pwhl span ( const Rtlbr<int> &r ) {
	Pwhl result;
	if (!p || r.is_empty() || !r.is_inside( Dim2D<int>{ w, h } ))
	  return result;

	result.w = r.br.x - r.tl.x;
	result.h = r.br.y - r.tl.y;
	result.l = l;
	result.p = p + r.tl.y * l + r.tl.x;
	result.prn = prn ? prn : this;

	return result;
  }

  // Mark pixels as modified — invalidates cached lo/hi.
  void stamp_pixels () const { pixels_ver_ = pyr::next_version(); }

  // Return cached (lo, hi) pixel positions; recompute if stale.
  // For scalar types: compares pixel values directly.
  // For RGB types: compares by channel sum (r+g+b).
  std::pair<Pxy<int>, Pxy<int>> get_lohi () const {
	if (!is_set())
	  return {{0, 0}, {0, 0}};
	if (lohi_ver_ > pixels_ver_)
	  return {lo_at_, hi_at_};

	Pxy<int> lp{0, 0}, hp{0, 0};

	if constexpr (std::is_same_v<pixeltype, Rgb8> || std::is_same_v<pixeltype, Rgb16>) {
	  auto key = [] ( const pixeltype &px ) {
		return static_cast<int>(px.r) + static_cast<int>(px.g) + static_cast<int>(px.b);
	  };
	  int lo_k = key(rowp(0)[0]);
	  int hi_k = lo_k;
	  for (int y = 0; y < h; ++y) {
		const auto *row = rowp(y);
		for (int x = 0; x < w; ++x) {
		  int k = key(row[x]);
		  if (k < lo_k) { lo_k = k; lp = {x, y}; }
		  if (k > hi_k) { hi_k = k; hp = {x, y}; }
		}
	  }
	} else {
	  auto lo_v = rowp(0)[0];
	  auto hi_v = lo_v;
	  for (int y = 0; y < h; ++y) {
		const auto *row = rowp(y);
		for (int x = 0; x < w; ++x) {
		  if (row[x] < lo_v) { lo_v = row[x]; lp = {x, y}; }
		  if (row[x] > hi_v) { hi_v = row[x]; hp = {x, y}; }
		}
	  }
	}

	lo_at_ = lp;
	hi_at_ = hp;
	lohi_ver_ = pyr::next_version();
	return {lo_at_, hi_at_};
  }

private:
  mutable pyr::version_t pixels_ver_ = 0;
  mutable pyr::version_t lohi_ver_   = 0;
  mutable Pxy<int>       lo_at_      = {0, 0};
  mutable Pxy<int>       hi_at_      = {0, 0};
};


// Extract one channel (0=r, 1=g, 2=b) of an RGB image as a scalar plane.
template <typename RgbT>
Pwhl<channel_type<RgbT>> extract_rgb_channel_plane ( const Pwhl<RgbT> &src, int channel )
{
	if (!src.is_set())
		return {};

	auto out = Pwhl<channel_type<RgbT>>({  src.w, src.h  });
	for (auto y = 0; y < src.h; ++y) {
		auto *dst = out.rowp( y );
		auto *row = src.rowp( y );
		for (auto x = 0; x < src.w; ++x) {
			auto &px = row[x];
			dst[x] = channel == 0 ? px.r : channel == 1 ? px.g : px.b;
		}
	}
	return out;
}


template <typename P>
auto *pwhl_row ( Pwhl<P> &img, int y ) { return img.rowp(y); }

template <typename P>
const auto *pwhl_row ( const Pwhl<P> &img, int y ) { return img.rowp(y); }


// Coarse-level point decimation by `factor`, THE rule for building a coarser
// pyramid level (pyr's synthetic top-up and the PTF converter): coarse pixel d
// takes the value of base pixel d*factor, the top-left of its block. Copied,
// never averaged, so no level holds a value the acquisition did not record; the
// cost is a sub-pixel offset of the coarse grid ((factor-1)/2 base pixels).
//
// `src` (anything with w, h and rowp) holds base pixels starting at base
// coordinate `src_origin`; the coarse pixels of `dst_rect` (coarse coordinates)
// whose sample lands in `src` are written to `dst`, which starts at coarse
// coordinate `dst_origin`. Returns whether any pixel was written.
template <typename Src, typename P>
bool decimate_into ( const Src &src, const Pxy<int> &src_origin, int factor,
	const Rtlbr<int> &dst_rect, Pwhl<P> &dst, const Pxy<int> &dst_origin )
{
	if (factor <= 0 || src.w <= 0 || src.h <= 0)
		return false;
	auto first = [factor] ( int base ) {
		return base >= 0 ? ( base + factor - 1 ) / factor : -( -base / factor );
	};
	const auto dx0 = std::max( dst_rect.tl.x, first( src_origin.x ) );
	const auto dy0 = std::max( dst_rect.tl.y, first( src_origin.y ) );
	const auto dx1 = std::min( dst_rect.br.x, first( src_origin.x + src.w ) );
	const auto dy1 = std::min( dst_rect.br.y, first( src_origin.y + src.h ) );
	auto written = false;
	for (auto dy = dy0; dy < dy1; ++dy) {
		const auto *src_row = src.rowp( dy * factor - src_origin.y );
		auto *dst_row = dst.rowp( dy - dst_origin.y );
		for (auto dx = dx0; dx < dx1; ++dx)
			dst_row[dx - dst_origin.x] = src_row[dx * factor - src_origin.x];
		written = written || dx1 > dx0;
	}
	return written;
}

// --- deep colour ------------------------------------------------------------
//
// Rgb16 holds samples at their native value, so a 12-bit plane spans 0..4095:
// everything that shows or measures one needs its range, stated or inferred.

// A sample scaled from its plane's range to 8 bits - THE display narrowing rule,
// shared by the channel composition and the deep-colour view so the two cannot
// show the same data at different brightness.
inline uint8_t narrow_to_8bit ( int value, int range )
{
	return (uint8_t)std::clamp( value * 255 / std::max( 1, range ), 0, 255 );
}


// The smallest full scale of the form 2^n - 1 covering `max_value` - the range a
// raster of unstated depth is shown in, so 12-bit data peaking at 3000 is shown
// against 4095 rather than 65535. 1 for an all-zero raster.
inline int bit_depth_full_scale ( int max_value )
{
	auto full_scale = 1;
	while (full_scale < max_value && full_scale < 0xffff)
		full_scale = ( full_scale << 1 ) | 1;
	return full_scale;
}


// Largest stored component of a deep-colour raster.
inline int rgb16_observed_max ( const Pwhl<Rgb16> &deep )
{
	auto max_value = 0;
	for (auto y = 0; y < deep.h; ++y) {
		const auto *src = deep.rowp( y );
		for (auto x = 0; x < deep.w; ++x)
			max_value = std::max( { max_value, (int)src[x].r, (int)src[x].g, (int)src[x].b } );
	}
	return max_value;
}


// bit_depth_full_scale over every component of a deep-colour raster.
inline int infer_rgb16_range ( const Pwhl<Rgb16> &deep )
{
	return bit_depth_full_scale( rgb16_observed_max( deep ) );
}


// The 8-bit view of a deep-colour (Rgb16) raster whose full scale is `range`.
// Unset for an unset input.
inline Pwhl<Rgb> narrow_rgb16_to_rgb8 ( const Pwhl<Rgb16> &deep, int range )
{
	if (!deep.is_set() || !deep.p)
		return {};
//
// Through a table: a view is millions of samples and a range at most 65536 wide,
// so one division per possible value is cheaper than one per sample.
	range = std::clamp( range, 1, 0xffff );
	auto table = std::vector<uint8_t>( (size_t)range + 1 );
	for (auto v = 0; v <= range; ++v)
		table[(size_t)v] = narrow_to_8bit( v, range );
	auto out = Pwhl<Rgb>( Dim2D<int>{ deep.w, deep.h } );
	for (auto y = 0; y < deep.h; ++y) {
		const auto *src = deep.rowp( y );
		auto *dst = out.rowp( y );
		for (auto x = 0; x < deep.w; ++x) {
			dst[x] = Rgb{ table[std::min<int>( src[x].r, range )],
				table[std::min<int>( src[x].g, range )],
				table[std::min<int>( src[x].b, range )] };
		}
	}
	return out;
}

} // namespace priml
