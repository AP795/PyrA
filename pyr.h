#pragma once

#include "geom.h"
#include "img_codec.h"
#include "pwhl.h"

#include "pyr_rt.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace priml {

using c_name = std::string;
using version_t = uint64_t;

enum class TileSts : uint8_t { Empty, Loading, Loaded, Decoding, Ready };


// What a level's tiles decode to.
//
// Distinct from priml::PixelType in img.h on purpose: that one names a plane of
// the image model, which sits ABOVE this library and which this library must not
// depend on. This one names what a level's tiles decode to.
//
// A runtime tag rather than a template parameter, because one slide may hold
// several: RGB brightfield planes and uint16 mono fluorescence planes are
// levels of the same pyramid. It names what the level DECODES TO - the buffer
// type - which for a given (channel, z) ladder is the same at every resolution.
//
// Rgb16 holds colour samples of more than 8 bits at their NATIVE value - 12-bit
// data spans 0..4095, not 0..65535 - with the real range in max_pixel_value,
// the same contract as Gray16. Appended last so the existing values stay put.
enum class LevelPixelType : uint8_t { Rgb8, Gray8, Gray16, Float32, Rgb16 };


template <typename pixeltype> struct pixel_type_of;
template <> struct pixel_type_of<Rgb8> {
	static constexpr LevelPixelType value = LevelPixelType::Rgb8; };
template <> struct pixel_type_of<Rgb16> {
	static constexpr LevelPixelType value = LevelPixelType::Rgb16; };
template <> struct pixel_type_of<uint8_t> {
	static constexpr LevelPixelType value = LevelPixelType::Gray8; };
template <> struct pixel_type_of<uint16_t> {
	static constexpr LevelPixelType value = LevelPixelType::Gray16; };
template <> struct pixel_type_of<float> {
	static constexpr LevelPixelType value = LevelPixelType::Float32; };


// Bytes one decoded pixel occupies. Reads the runtime tag, so it stays correct
// on a pyramid whose levels are not all the same type.
inline size_t pixel_size_bytes ( LevelPixelType type ) noexcept
{
	switch (type) {
	case LevelPixelType::Rgb8: return sizeof( Rgb8 );
	case LevelPixelType::Gray8: return sizeof( uint8_t );
	case LevelPixelType::Gray16: return sizeof( uint16_t );
	case LevelPixelType::Float32: return sizeof( float );
	case LevelPixelType::Rgb16: return sizeof( Rgb16 );
	}
	return sizeof( Rgb8 );
}


// Integer monochrome storage: the two widths one open serves together.
inline constexpr bool is_integer_gray ( LevelPixelType type ) noexcept
{
	return type == LevelPixelType::Gray8 || type == LevelPixelType::Gray16;
}


// Integer colour storage: the two widths one open serves together, so a slide
// stored as Rgb16 still opens for a caller that asks for 8-bit Rgb.
inline constexpr bool is_integer_rgb ( LevelPixelType type ) noexcept
{
	return type == LevelPixelType::Rgb8 || type == LevelPixelType::Rgb16;
}


// Call `f` with a value of the C++ pixel type `type` decodes to - the one
// runtime-tag -> template dispatch (Rgb8 for anything else). Every branch of `f`
// must return the same type.
template <typename F>
decltype( auto ) visit_level_pixel_type ( LevelPixelType type, F &&f )
{
	switch (type) {
	case LevelPixelType::Gray8: return f( uint8_t{} );
	case LevelPixelType::Gray16: return f( uint16_t{} );
	case LevelPixelType::Float32: return f( float{} );
	case LevelPixelType::Rgb16: return f( Rgb16{} );
	default: return f( Rgb8{} );
	}
}


inline const char *pixel_type_name ( LevelPixelType type ) noexcept
{
	switch (type) {
	case LevelPixelType::Rgb8: return "Rgb8";
	case LevelPixelType::Gray8: return "Gray8";
	case LevelPixelType::Gray16: return "Gray16";
	case LevelPixelType::Float32: return "Float32";
	case LevelPixelType::Rgb16: return "Rgb16";
	}
	return "Unknown";
}

// Which plane of a slide to address.
//
// A slide may hold more than one: fluorescence channels, and focal planes within
// each. Defaulted throughout, so every existing caller keeps addressing the
// single unnamed plane an ordinary brightfield slide has.
struct PlaneSel {
	std::string channel;			// "" = the slide's only / default channel
	float zlevel = 0.0f;			// microns, the frame of Level::metric and Ppyr::mrct

	bool operator== ( const PlaneSel &other ) const {
		return channel == other.channel && zlevel == other.zlevel;
	}
};


struct Tile;


// Shared slot bookkeeping for the per-level buffer pools (TileBf, PixBf):
// a flat element array carved into equal-size slots, tpos[] mapping slot ->
// occupying tile (-1 = free) and a free-list stack for O(1) acquire.
template <typename ElemT>
struct SlotPool {
	int max_slots = 0;				// configured upper bound for allocated slots
	int num_slots = 0;				// number of allocated slots
	int used_slots = 0;				// currently occupied slots
	int peak_used_slots = 0;		// high-water mark of occupied slots
	size_t slot_elems_ = 0;			// elements per slot
	std::vector<ElemT> buf;
	std::vector<int> tpos;			// tpos[slot] = tile index occupying that slot, -1 = free
	std::vector<int> free_list_;	// stack of free slot indices for O(1) acquire

	void reset_pool ( size_t elems_per_slot, int slot_count ) {
		slot_elems_ = elems_per_slot;
		max_slots = slot_count;
		num_slots = 0;
		used_slots = 0;
		peak_used_slots = 0;
		buf.clear();
		tpos.clear();
		free_list_.clear();
	}

	ElemT *slot_ptr ( int slot ) { return buf.data() + (size_t)slot * slot_elems_; }

	bool can_grow () const { return num_slots < max_slots; }

	int grow_slots ( int slot_count, size_t max_extra_bytes = SIZE_MAX ) {
		auto add = std::min( slot_count, max_slots - num_slots );
		if (add <= 0)
			return 0;
		const auto new_size = ((size_t)num_slots + (size_t)add) * slot_elems_;
		if (new_size > buf.capacity()) {
// Construct an exact-sized replacement rather than vector::resize's implicit
// over-allocation. Account retained capacity, not just occupied slots.
			auto grown = std::vector<ElemT>( new_size );
			const auto extra = ( grown.capacity() - buf.capacity() ) * sizeof( ElemT );
			if (extra > max_extra_bytes)
				return 0;
			std::copy( buf.begin(), buf.end(), grown.begin() );
			buf.swap( grown );
		} else {
			buf.resize( new_size );
		}
		tpos.resize( num_slots + add, -1 );
		for (int i = num_slots + add - 1; i >= num_slots; --i)
			free_list_.push_back( i );
		num_slots += add;
		return add;
	}

	void trim_free_tail () {
		auto old_slots = num_slots;
		while (num_slots > 0 && !tpos.empty() && tpos.back() < 0) {
			tpos.pop_back();
			--num_slots;
		}
		buf.resize( (size_t)num_slots * slot_elems_ );
		if (num_slots != old_slots) {
			std::erase_if( free_list_, [this]( int s ) { return s >= num_slots; } );
			if (num_slots == 0 || (size_t)num_slots * 2 < tpos.capacity()) {
				tpos.shrink_to_fit();
				buf.shrink_to_fit();
			}
		}
	}

	int acquire ( int tile_idx ) {
		if (free_list_.empty())
			return -1;
		int slot = free_list_.back();
		free_list_.pop_back();
		tpos[slot] = tile_idx;
		++used_slots;
		peak_used_slots = std::max( peak_used_slots, used_slots );
		return slot;
	}

	void release ( int slot ) {
		if (slot >= 0 && slot < num_slots) {
			if (tpos[slot] >= 0) {
				tpos[slot] = -1;
				if (used_slots > 0)
					--used_slots;
				free_list_.push_back( slot );
				trim_free_tail();
			}
		}
	}
};


// Pool of fixed-size compressed-tile buffers, one per Level.
// Each slot holds the assembled JPEG (header + raw data) ready for decoding.
struct TileBf : SlotPool<uint8_t> {
	void init ( int item_len, int slot_count ) {
		reset_pool( (size_t)item_len, slot_count );
	}

	size_t slot_bytes () const { return slot_elems_; }
	size_t allocated_bytes () const { return buf.capacity(); }

	int grow ( int slot_count, size_t max_extra_bytes = SIZE_MAX ) {
		return grow_slots( slot_count, max_extra_bytes );
	}
};


// Pool of fixed-size decoded pixel buffers, one per Level.
// Each slot holds tile_w * tile_h pixels of decoded tile data.
template <typename pixeltype>
struct PixBf : SlotPool<pixeltype> {
	using SlotPool<pixeltype>::buf;
	using SlotPool<pixeltype>::tpos;
	using SlotPool<pixeltype>::num_slots;
	using SlotPool<pixeltype>::slot_elems_;
	using SlotPool<pixeltype>::slot_ptr;
	using SlotPool<pixeltype>::grow_slots;

	int slot_w = 0;					// tile width
	int slot_h = 0;					// tile height

	void init ( const Dim2D<int> &tile_dimensions, int slot_count ) {
		slot_w = tile_dimensions.w;
		slot_h = tile_dimensions.h;
		this->reset_pool(
			(size_t)tile_dimensions.w * (size_t)tile_dimensions.h, slot_count );
	}

	size_t slot_pixels () const { return slot_elems_; }
	size_t slot_bytes () const {
		return slot_elems_ * sizeof( pixeltype );
	}
	size_t allocated_bytes () const { return buf.capacity() * sizeof( pixeltype ); }

	int grow ( int slot_count, size_t max_extra_bytes = SIZE_MAX ) {
		return grow_slots( slot_count, max_extra_bytes );
	}
};

// A tile carries no pixels and no pixel type - only where its pixels are.
//
// `nixbuf` names a PixBf slot and `owned_no` a slot in LevelPix::owned (a
// synthetic level owns its raster outright, since nothing on disk backs it).
// The Pwhl view is rebuilt from those on demand by tile_pixels(); it is not
// stored, which is what keeps Tile free of the template parameter and lets a
// growing PixBf move its buffer without re-pointing every resident tile.
//
// pbd_w/h/l are still needed because a decoded tile is not always the size of
// its slot: an edge tile is smaller, and the stride is the codec's, not the
// slot's.
struct Tile {
	Rtlbr<int> lr = {{0, 0}, {0, 0}};	// rectangle in 'level pixels'
	int ninbuf = -1;			// slot index in Level::tbf
	int nixbuf = -1;			// slot index in Level::pxb
	int owned_no = -1;			// index in LevelPix::owned, -1 = not owned
	int pbd_w = 0;				// decoded extent, <= the slot's
	int pbd_h = 0;
	int pbd_l = 0;				// decoded stride
	size_t bf_sz = 0;			// actual assembled JPEG size in TileBf slot
	size_t foff = 0;			// offset in file
	size_t fsz = 0;			// size in bytes in the file
	std::atomic<TileSts> sts{TileSts::Empty};
	std::atomic<int> pin_count{0};
	std::atomic<version_t> last_use{0};

	Tile() = default;
	Tile ( const Tile & ) = delete;
	Tile &operator= ( const Tile & ) = delete;

	Tile ( Tile &&other ) noexcept
		: lr(other.lr), ninbuf(other.ninbuf),
		  nixbuf(other.nixbuf), owned_no(other.owned_no),
		  pbd_w(other.pbd_w), pbd_h(other.pbd_h), pbd_l(other.pbd_l),
		  bf_sz(other.bf_sz), foff(other.foff), fsz(other.fsz),
		  last_use(other.last_use.load( std::memory_order_relaxed )) {
		sts.store( other.sts.load() );
		pin_count.store( other.pin_count.load() );
		other.ninbuf = -1;
		other.nixbuf = -1;
		other.owned_no = -1;
		other.pbd_w = other.pbd_h = other.pbd_l = 0;
		other.bf_sz = 0;
		other.foff = 0;
		other.fsz = 0;
		other.sts.store( TileSts::Empty );
		other.pin_count.store( 0 );
	}

	Tile &operator= ( Tile &&other ) noexcept {
		if (this != &other) {
			lr = other.lr;
			ninbuf = other.ninbuf;
			nixbuf = other.nixbuf;
			owned_no = other.owned_no;
			pbd_w = other.pbd_w;
			pbd_h = other.pbd_h;
			pbd_l = other.pbd_l;
			bf_sz = other.bf_sz;
			foff = other.foff;
			fsz = other.fsz;
			sts.store( other.sts.load() );
			pin_count.store( other.pin_count.load() );
			last_use.store( other.last_use.load( std::memory_order_relaxed ),
				std::memory_order_relaxed );

			other.ninbuf = -1;
			other.nixbuf = -1;
			other.owned_no = -1;
			other.pbd_w = other.pbd_h = other.pbd_l = 0;
			other.bf_sz = 0;
			other.foff = 0;
			other.fsz = 0;
			other.sts.store( TileSts::Empty );
			other.pin_count.store( 0 );
		}
		return *this;
	}
};

// The decoded-pixel storage of one level.
//
// Split off from Level so that Level itself carries no pixel type: the pyramid
// holds levels of mixed types, and only the code that actually touches pixels -
// decode, blit, Generate - needs to know which. Reached through level_pix<T>(),
// which is a static_cast, so everything past that point is monomorphic.
struct LevelPixBase {
	virtual ~LevelPixBase () = default;
//
// Pool accounting, needed by the byte-budget and eviction bookkeeping, which is
// type-agnostic. Virtual because it is asked per level per budget check, never
// per pixel.
	virtual size_t allocated_bytes () const = 0;
	virtual size_t slot_bytes () const = 0;
	virtual int used_slots () const = 0;
	virtual int num_slots () const = 0;
	virtual int peak_used_slots () const = 0;
//
// Eviction bookkeeping. Choosing a victim and handing its slot back is pure
// slot arithmetic - no pixel is read or written - so it does not need the type.
// That is what lets the tile LOAD path stay type-agnostic even though it may
// have to evict decoded pixels to stay inside the RAM cap.
	virtual int slot_tile ( int slot ) const = 0;
	virtual void release_slot ( int slot ) = 0;
};


template <typename pixeltype>
struct LevelPix : LevelPixBase {
	PixBf<pixeltype> pxb;			// decoded pixel buffer pool
//
// Pixels owned outright by this level's tiles. Only a synthetic level uses it:
// nothing on disk backs those rasters, so they cannot live in PixBf, which is
// an eviction pool for things that can be re-read.
	std::vector<Pwhl<pixeltype>> owned;

	size_t allocated_bytes () const override { return pxb.allocated_bytes(); }
	size_t slot_bytes () const override { return pxb.slot_bytes(); }
	int slot_tile ( int slot ) const override {
		return ( slot >= 0 && slot < (int)pxb.tpos.size() ) ? pxb.tpos[slot] : -1;
	}
	void release_slot ( int slot ) override { pxb.release( slot ); }
	int used_slots () const override { return pxb.used_slots; }
	int num_slots () const override { return pxb.num_slots; }
	int peak_used_slots () const override { return pxb.peak_used_slots; }
};


struct Level {
	float metric = 0.0f;				// width in microns of one pixel
//
// The file this level's tiles are read from; empty means the pyramid's own
// file, which is the case for every level of an ordinary single-file slide.
//
// A pyramid may span files. A DICOM slide already does - it is a folder of .dcm
// files, one VOLUME per level - and appending synthetic levels to a companion
// file, rather than to a read-only vendor slide, needs the same thing.
	std::string filename;
// What this level's tiles decode to. Every level of one (channel, z) ladder
// must agree; the loader states it in Open.
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
// Whether the loader actually STATED that type, as opposed to leaving the
// default standing.
//
// The distinction matters because level_pix<T> overwrites pixel_type on first
// typed access, so for every format that does not declare one the field is a
// placeholder rather than a fact - and filtering a pyramid on a placeholder would
// discard the whole slide. Only a level whose type is declared may be judged
// against the type an open asked for.
	bool pixel_type_declared = false;
// Largest value a sample of this level actually takes - 4095 for 12-bit sensor
// data in a 16-bit container, 0 when the file did not say. It sizes a display
// lookup table, and a 12-bit plane read as a 16-bit one is uniformly black.
	int max_pixel_value = 0;
// Which plane this level belongs to. Levels sharing a plane form a resolution
// ladder - see Ppyr::planes.
	PlaneSel plane;
// Index into Ppyr::planes, stamped by rebuild_plane_index. -1 until then.
//
// The pool allocator needs a level's plane on every growth decision, and
// searching `planes` by PlaneSel for that would put a string compare in the
// allocation path.
	int plane_no = -1;
// Tile dimensions. These size the TileBf / PixBf slots; they are NOT how tiles
// are located - see `bounds` and BuildRenderPlan. A loader whose tiles are not
// a uniform grid sets these to the largest tile it will produce.
	int tile_w = 0;
	int tile_h = 0;
// The level's extent in level pixels. Tiles are addressed by their `lr`
// rectangles alone, so the extent cannot be inferred from the last tile except
// on a row-major grid; every loader states it explicitly via set_level_bounds.
	Rtlbr<int> bounds = {{0, 0}, {0, 0}};
// Tallest tile on the level, in level pixels. Tile selection binary-searches the
// (top, left) ordering, and a tile's bottom edge is not part of that key, so this
// is how far back the search has to reach to be exact. Derived in
// set_level_bounds - never set it by hand.
	int max_tile_span_y = 0;
// Built by decimating a finer level rather than read from the file, because the
// file stores no layer this coarse (see BuildCoarseLevels in wsi.cpp). Its tiles
// own their pixels and are already Ready, so the load/decode path skips them.
	bool synthetic = false;
	bool reject_whole_slide_black = false;
// Only for encodings that cannot describe their own colour space: Aperio
// compression 33003 stores YCbCr with the JPEG 2000 MCT off, so the container
// tag is the sole source. FromStream (the default) is correct everywhere else.
	ColorHint color_hint = ColorHint::FromStream;
	std::vector<uint8_t> hdrbf;			// JPEG header (NDPI: full header; SVS: JPEGTABLES minus EOI)
	std::vector<uint32_t> block_sizes;	// From NDPI tag 65426
	std::vector<uint64_t> strip_offsets;
	std::vector<Tile> tiles;
	TileBf tbf;							// compressed tile buffer pool
// Decoded pixels, typed. Null until a loader calls level_pix<T> in Open.
	std::shared_ptr<LevelPixBase> pix;
	mutable std::unique_ptr<std::shared_mutex> tbf_mtx_ =
		std::make_unique<std::shared_mutex>();
	mutable std::unique_ptr<std::shared_mutex> pxb_mtx_ =
		std::make_unique<std::shared_mutex>();
};


// The typed pixel storage of `level`, created on first use.
//
// The one place the runtime pixel type meets the compile-time one. A mismatch
// is a loader bug - it would hand a level's pixels to code expecting a different
// buffer type - so it is reported rather than silently reinterpreted.
template <typename pixeltype>
LevelPix<pixeltype> &level_pix ( Level &level )
{
	if (!level.pix) {
		level.pixel_type = pixel_type_of<pixeltype>::value;
		level.pix = std::make_shared<LevelPix<pixeltype>>();
	}
	else if (level.pixel_type != pixel_type_of<pixeltype>::value) {
		pyr::plog_err(
			"pyr level: pixel type mismatch - level holds %s, asked for %s\n",
			pixel_type_name( level.pixel_type ),
			pixel_type_name( pixel_type_of<pixeltype>::value ) );
	}
	return *static_cast<LevelPix<pixeltype> *>( level.pix.get() );
}



// The pixels of `tile`, or an empty view when it holds none.
//
// The single place a tile is resolved to a raster: a synthetic level's own
// buffer, otherwise a window onto its PixBf slot at the extent the codec
// actually decoded.
template <typename pixeltype>
PwhlView<pixeltype> tile_pixels ( const Tile &tile, Level &level )
{
	auto &pix = level_pix<pixeltype>( level );
	if (tile.owned_no >= 0 && tile.owned_no < (int)pix.owned.size()) {
		auto &own = pix.owned[tile.owned_no];
		return PwhlView<pixeltype>( Dim2D<int>{ own.w, own.h }, own.l, own.p );
	}
	if (tile.nixbuf < 0 || tile.pbd_w <= 0 || tile.pbd_h <= 0)
		return PwhlView<pixeltype>();

	return PwhlView<pixeltype>( Dim2D<int>{ tile.pbd_w, tile.pbd_h }, tile.pbd_l,
		pix.pxb.slot_ptr( tile.nixbuf ) );
}


// True when `tile` currently holds decoded pixels.
inline bool tile_has_pixels ( const Tile &tile )
{
	return ( tile.owned_no >= 0 ) ||
		( tile.nixbuf >= 0 && tile.pbd_w > 0 && tile.pbd_h > 0 );
}


// Prepare a level for rendering once its tiles are built: order the tiles by
// (top, left) and set `bounds` to their envelope.
//
// BuildRenderPlan locates tiles by binary-searching that order, so it is a
// precondition, not a convenience. Loaders that emit a row-major grid already
// satisfy it and the sort is a no-op for them; one that emits tiles in an
// arbitrary order (a mosaic addressed by stage position) is made conformant here
// rather than having to remember the rule itself.
inline void set_level_bounds ( Level &level )
{
	if (level.tiles.empty()) {
		level.bounds = {{0, 0}, {0, 0}};
		level.max_tile_span_y = 0;
		return;
	}

	std::stable_sort( level.tiles.begin(), level.tiles.end(),
		[] ( const Tile &a, const Tile &b ) {
			if (a.lr.tl.y != b.lr.tl.y)
				return a.lr.tl.y < b.lr.tl.y;
			return a.lr.tl.x < b.lr.tl.x;
		} );

	auto bounds = level.tiles.front().lr;
	auto span_y = 0;
	for (const auto &tile : level.tiles) {
		bounds = UnionRect( bounds, tile.lr );
		span_y = std::max( span_y, height( tile.lr ) );
	}
	level.bounds = bounds;
	level.max_tile_span_y = span_y;
}


// The half-open index range of `level.tiles` that can intersect `request`.
//
// Tiles are ordered by (top, left), so the last candidate is the first tile
// starting at or below the request. The first candidate cannot be found the same
// way, because a tile's bottom edge is not part of the sort key: a tall tile
// starting well above the request may still reach into it. `max_tile_span_y`
// bounds how far above to start, which makes the bracket exact without scanning.
inline std::pair<int, int> bracket_tiles_by_top ( const Level &level,
	const Rtlbr<int> &request )
{
	auto by_top = [] ( const Tile &tile, int y ) {
		return tile.lr.tl.y < y;
	};

	auto reach = std::max( 0, level.max_tile_span_y );
	auto first = (int)( std::lower_bound( level.tiles.begin(), level.tiles.end(),
		request.tl.y - reach + 1, by_top ) - level.tiles.begin() );
	auto last = (int)( std::lower_bound( level.tiles.begin(), level.tiles.end(),
		request.br.y, by_top ) - level.tiles.begin() );
	return { first, last };
}


// The tiles covering `request`, in paint order, and the rectangle they actually
// cover.
//
// Tiles are addressed by their `lr` rectangles alone - never by grid arithmetic -
// so they may be any size and may overlap. Where they do, the later tile in the
// (top, left) ordering paints last and wins, which is what lets a mosaic with
// acquisition overlap composite without seams. `covered` is the envelope of the
// selected tiles: it can exceed the request (partial tiles at the edges) or fall
// short of it (the request ran past the imaged area).
//
// Modelled on hz_get_piclst in \usr\src\ir\hz_base.cpp.
inline void select_tiles_for_rect ( const Level &level,
	const Rtlbr<int> &request, std::vector<int> &selected, Rtlbr<int> &covered )
{
	selected.clear();
	covered = {{0, 0}, {0, 0}};
	if (level.tiles.empty() || request.is_empty())
		return;

	auto [first, last] = bracket_tiles_by_top( level, request );
	selected.reserve( (size_t)std::max( 0, last - first ) );

	for (auto tile_no = first; tile_no < last; ++tile_no) {
		const auto &lr = level.tiles[tile_no].lr;
		if (IntersectRect( lr, request ).is_empty())
			continue;

		covered = selected.empty() ? lr : UnionRect( covered, lr );
		selected.push_back( tile_no );
	}
}

struct Ppyr;

template <typename pixeltype> struct Render {
	Ppyr *pyr = nullptr;
//
// Which plane the cached frame came from. Without it `matches` compares only the
// level index, rect and tile list - all of which a different plane can reproduce
// exactly - and the cache would hand back another channel's pixels.
	PlaneSel plane;
	int level_no = -1;
	Rtlbr<int> lr = {{0, 0}, {0, 0}};	// covered rectangle in level pixels
	Rtlbr<float> mrct = {{0, 0}, {0, 0}};	// covered rectangle in metric units
	std::vector<int> tiles;
	Pwhl<pixeltype> image;

	void bind ( Ppyr *parent ) {
		if (pyr != parent) {
			clear();
			pyr = parent;
		}
	}

	void clear () {
		plane = PlaneSel{};
		level_no = -1;
		lr = {{0, 0}, {0, 0}};
		mrct = {{0, 0}, {0, 0}};
		tiles.clear();
		image = Pwhl<pixeltype>();
	}

	bool matches ( const PlaneSel &sel, int lvl_no, const Rtlbr<int> &bounds,
		const std::vector<int> &tile_list ) const
	{
		return plane == sel &&
			level_no == lvl_no &&
			lr.tl.x == bounds.tl.x && lr.tl.y == bounds.tl.y &&
			lr.br.x == bounds.br.x && lr.br.y == bounds.br.y &&
			tiles == tile_list &&
			image.p != nullptr;
	}
};


// One plane's resolution ladder.
//
// `ladder` indexes Ppyr::levels, finest first - exactly the ordering the render
// path has always assumed of the whole vector. The plane key selects a ladder
// BEFORE level selection runs, which is what keeps SelectBestLevel,
// BuildCoarseLevels and base_mpp correct once a slide holds several planes.
struct PlaneStack {
	PlaneSel plane;
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
// Carried up from the ladder's levels so a consumer asking "what is this
// channel's dynamic range" has one typed place to ask, rather than reaching into
// a level or re-deriving it from the pixels. 0 when the file did not say.
	int max_pixel_value = 0;
// Bytes of tile and pixel pool this plane's levels may hold between them.
//
// 0 means "an equal share of Ppyr::ram_cap_bytes". A budget is per plane rather
// than per pyramid because the pools are per level and a plane is a ladder of
// them: with one shared pot the first channel touched grows until the pot is
// empty and the other channels never allocate at all. Measured before this
// existed: one level of one channel held 419 MB of a 512 MB cap, and a
// three-channel slide could not render its own base level.
//
// Set it explicitly through set_plane_ram_cap when a caller knows the shape of
// its work - an analysis pass that reads one channel hard and the others barely
// should say so rather than let an equal split starve it.
	size_t ram_cap_bytes = 0;
	std::vector<int> ladder;
};


// The sample value that means "full scale" for a plane: its declared
// max_pixel_value, else the full range of its storage (255 for the 8-bit types,
// 0xffff otherwise). The one place display and OD scaling read a channel's range
// from.
inline int plane_value_range ( const PlaneStack &stack )
{
	if (stack.max_pixel_value > 0)
		return stack.max_pixel_value;
	const auto narrow = stack.pixel_type == LevelPixelType::Gray8 ||
		stack.pixel_type == LevelPixelType::Rgb8;
	return narrow ? 255 : 0xffff;
}


// One named channel to compose: its name, pixels (widened to 16 bits) and the
// sample value that maps to full intensity.
struct NamedChannelPixels {
	std::string name;
	const Pwhl<uint16_t> *pixels = nullptr;
	int range = 0xffff;
};

// The RGB view of a channel slide, THE composition rule for every such picture
// (the analysis colour view, the converter's thumbnail): when channels named R, G
// and B (ASCII case-insensitively) all exist they drive their colour component,
// scaled from their range to 8 bits, and any other channel is ignored. A slide
// without all three shows its first channel as grey, honest about there being no
// colour: a green acquisition alone is a grey image, not a green one. Channels of
// another size than the first usable one are skipped. Unset when no channel has
// pixels.
inline Pwhl<Rgb> compose_rgb_from_channels ( const std::vector<NamedChannelPixels> &channels )
{
	auto slot_of = [] ( const std::string &name ) {
		if (name.size() != 1)
			return -1;
		const auto c = name[0] >= 'a' && name[0] <= 'z' ? char( name[0] - 'a' + 'A' ) : name[0];
		return c == 'R' ? 0 : c == 'G' ? 1 : c == 'B' ? 2 : -1;
	};

//	Not "slots": Qt defines it as a keyword macro, and Qt units include this.
	const NamedChannelPixels *first = nullptr;
	const NamedChannelPixels *components[3] = { nullptr, nullptr, nullptr };
	for (const auto &channel : channels) {
		if (!channel.pixels || !channel.pixels->is_set())
			continue;
		if (!first)
			first = &channel;
		else if (channel.pixels->w != first->pixels->w || channel.pixels->h != first->pixels->h)
			continue;
		if (const auto slot = slot_of( channel.name ); slot >= 0 && !components[slot])
			components[slot] = &channel;
	}
	if (!first)
		return {};
	if (!components[0] || !components[1] || !components[2])
		components[0] = components[1] = components[2] = first;

	auto out = Pwhl<Rgb>( Dim2D<int>{ first->pixels->w, first->pixels->h } );
	for (auto y = 0; y < out.h; ++y) {
		auto *row = out.rowp( y );
		for (auto x = 0; x < out.w; ++x) {
			uint8_t rgb[3] = { 0, 0, 0 };
			for (auto i = 0; i < 3; ++i) {
				if (!components[i])
					continue;
				rgb[i] = narrow_to_8bit( (int)components[i]->pixels->rowp( y )[x],
					components[i]->range );
			}
			row[x] = Rgb{ rgb[0], rgb[1], rgb[2] };
		}
	}
	return out;
}


// One metadata item. Normalised keys follow OpenSlide's vocabulary
// ("openslide.mpp-x", "openslide.objective-power", ...) so the two are directly
// comparable; the raw tag a normalised value came from is kept alongside under
// its own per-format prefix ("tiff.XResolution", "aperio.MPP", ...), because the
// receipt is worth as much as the reading.
struct Property {
	std::string key;
	std::string value;
};


// The three canonical auxiliary images. A format supplies whichever it has.
//
//   frost     the label area alone
//   overview  the tissue area alone
//   macro     the whole slide, label AND tissue in one image
//
// Most scanners give a label plus a whole-slide macro; a few give a tissue-only
// overview. Ask through label_image() / slide_overview_image() rather than
// naming a slot, so a caller does not have to know which kind of file it has.
inline constexpr const char *kAuxFrost = "frost";
inline constexpr const char *kAuxOverview = "overview";
inline constexpr const char *kAuxMacro = "macro";


struct PpyrStats {
	double open_ms = 0.0;
	uint64_t requested_tiles = 0;
	uint64_t tile_loads = 0;
	uint64_t decoded_tiles = 0;
	uint64_t renders = 0;
	uint32_t peak_decode_workers = 0;
	uint32_t peak_tilebf_used_slots = 0;
	uint32_t peak_pixbf_used_slots = 0;
// Renders that returned a frame with tiles missing, and how many tiles those
// were. Non-zero means pixels the caller asked for were not delivered — a
// measurement taken over such a frame is wrong, so this is worth surfacing
// rather than leaving to the log.
	uint64_t incomplete_renders = 0;
	uint64_t dropped_tiles = 0;
	double render_plan_ms = 0.0;
	double tile_load_ms = 0.0;
	double tile_decode_ms = 0.0;
	double decode_batch_ms = 0.0;
	double render_ms = 0.0;
};


// Native base-level pixel size reported by the file, in microns per pixel.
//
// Rendering still uses Level::metric, which is deliberately scalar. This pair
// preserves both source values so the common loader boundary can diagnose the
// rare file whose physical pixels are not square without changing geometry.
struct PixelMetric {
	float x = 0.0f;
	float y = 0.0f;
};


struct Ppyr {
	Rtlbr<float> mrct;				// metric coordinates of top-left and bottom-right
	PixelMetric source_pixel_metric;	// native X/Y values, for load diagnostics
//
// Every level of the slide, of every plane. This is the owning store and nothing
// more: it is NOT itself a resolution ladder once a slide holds more than one
// plane, so do not scan it looking for "the coarsest level". Go through `planes`,
// which groups it into ladders.
	std::vector<Level> levels;
//
// The levels of `levels`, grouped into per-plane resolution ladders. Rebuilt by
// rebuild_plane_index after a loader has filled `levels` in.
	std::vector<PlaneStack> planes;
	size_t tbf_init_bytes = 100ull * 1024ull * 1024ull;	// initial TileBf growth budget
	size_t tbf_cap_bytes = 0;				// 0 = no TileBf-specific cap
	size_t pxb_init_bytes = 100ull * 1024ull * 1024ull;	// initial PixBf growth budget
	size_t pxb_cap_bytes = 0;				// 0 = no PixBf-specific cap
	size_t ram_cap_bytes = 512ull * 1024ull * 1024ull;	// pyramid-wide pool cap
	int decode_threads = 0;			// 0 = auto, >0 = fixed worker count
// Optional context owner: bounds growth across slides and reclaims idle pools.
// Called under the owner's serialization; true selects decoded storage.
	std::function<size_t( bool, size_t )> cache_growth_budget;
//
// Synthetic coarse levels (BuildCoarseLevels in wsi.cpp), for slides whose file
// stores no reduced-resolution layer. Set before OpenFile.
//
// The criterion is the DECODED SIZE of the coarsest level, not its pixel
// dimensions: that is what decides both whether a whole-slide render can hold
// all its tiles at once and how long it takes to draw. Levels are added while
// the coarsest exceeds coarse_level_bytes, which at the default is a couple of
// megabytes - small enough to decode instantly at any zoom, and independent of
// aspect ratio and pixel type in a way a "longest side" test is not.
//
//   coarse_level_bytes      keep adding levels while the coarsest is larger than
//                           this; 0 disables the whole mechanism.
//   coarse_first_step_bytes ceiling on the FIRST synthetic level, so a slide with
//                           an enormous base does not get a huge half-size level
//                           on the way down. Later steps just halve, which is
//                           cheap because the source is already in memory.
//   coarse_max_src_tiles    building a level reads every tile of its source once;
//                           past this many, skip it rather than stall the open.
	size_t coarse_level_bytes = 3ull * 1024ull * 1024ull;
	size_t coarse_first_step_bytes = 128ull * 1024ull * 1024ull;
	size_t coarse_max_src_tiles = 8192;
	bool use_pixbf = true;				// false = decode directly into render buffers
//
// Whether to let the loader decide use_pixbf, and what it decided.
//
// The two settings suit opposite codecs, and which is right is a property of the
// file rather than the installation. With cheap tiles - JPEG at tens of
// microseconds - the same RAM buys far more *encoded* tiles than decoded ones
// (roughly 10x), so bypassing the decoded cache raises the hit rate and
// re-decoding costs almost nothing. With JPEG XR at ~119 ms for a 5 Mpx Zeiss
// sub-block, re-decoding on every repaint dominates and caching decoded pixels
// wins despite costing ~34x the RAM per tile.
//
// So a loader states what it stores via prefer_pixbf, and OpenFile applies it
// when the caller asked for auto. A caller that sets use_pixbf explicitly is
// obeyed; auto is opt-in.
	bool use_pixbf_auto = false;
	bool prefer_pixbf = false;		// set by the loader in Open, read only if auto
//
// Auxiliary images the file carries beside the pyramid, by name.
//
// A map rather than three fixed members so nothing a loader parsed is thrown
// away - vendors ship thumbnails and more besides the three canonical names.
// Always RGB, whatever the levels decode to: these are photographs of the glass,
// and a fluorescence scan does not change what the label camera recorded.
	std::unordered_map<std::string, Pwhl<Rgb>> aux_images;
	std::vector<std::string> aux_order;		// stable iteration order
//
// The raw ICC profile, when the file carries one. Recorded, never applied - see
// icc_summary() and the note there.
	std::vector<uint8_t> icc_profile;
//
// Normalised and raw metadata, in the order the loader produced it.
	std::vector<Property> properties;
	std::string barcode;				// slide-label barcode ("" = none/unknown yet)
	bool barcode_attempted = false;		// guards a one-shot image decode of the frost
//
// The barcode was recorded by the scanner as metadata (DICOM ContainerIdentifier
// and friends, NDPI tag 65427) rather than decoded from the label pixels. Once
// barcode_attempted latches the two are otherwise indistinguishable, and they
// mean different things to a user: a DICOM may carry a perfectly good barcode
// and no label image at all.
	bool barcode_from_metadata = false;
	PpyrStats stats;
// Decoder workers publish unavailable codecs here; the serialized C operation
// clears it before a request and reports it through its error output.
	std::unique_ptr<std::atomic<ImgCodec>> unsupported_codec_ =
		std::make_unique<std::atomic<ImgCodec>>( ImgCodec::None );
};


// Group `levels` into per-plane resolution ladders, finest first.
//
// Called once a loader has filled `levels` in, and again after synthetic coarse
// levels are appended. Also the place the "one pixel type per ladder" invariant
// is checked: a level disagreeing with its plane is a loader bug, and silently
// reinterpreting its buffer would be far worse than saying so.
inline void rebuild_plane_index ( Ppyr &pyramid )
{
	pyramid.planes.clear();
	for (auto li = 0; li < (int)pyramid.levels.size(); ++li) {
		const auto &level = pyramid.levels[li];
		auto it = std::find_if( pyramid.planes.begin(), pyramid.planes.end(),
			[&] ( const PlaneStack &st ) { return st.plane == level.plane; } );
		if (it == pyramid.planes.end()) {
			auto stack = PlaneStack{};
			stack.plane = level.plane;
			stack.pixel_type = level.pixel_type;
			stack.max_pixel_value = level.max_pixel_value;
			stack.ladder.push_back( li );
			pyramid.planes.push_back( std::move( stack ) );
			continue;
		}
//
// The coarse levels a top-up appends do not restate the range, so the first level
// that names one settles it for the ladder rather than the last.
		if (it->max_pixel_value == 0)
			it->max_pixel_value = level.max_pixel_value;
		if (it->pixel_type != level.pixel_type) {
			pyr::plog_err(
				"pyr plane: channel '%s' z=%.3f mixes pixel types %s and %s - "
				"every level of one ladder must agree\n",
				level.plane.channel.c_str(), (double)level.plane.zlevel,
				pixel_type_name( it->pixel_type ),
				pixel_type_name( level.pixel_type ) );
		}
		it->ladder.push_back( li );
	}

	for (auto pi = 0; pi < (int)pyramid.planes.size(); ++pi) {
		auto &stack = pyramid.planes[pi];
		std::stable_sort( stack.ladder.begin(), stack.ladder.end(),
			[&] ( int a, int b ) {
				return pyramid.levels[a].metric < pyramid.levels[b].metric;
			} );
		for (auto li : stack.ladder)
			pyramid.levels[li].plane_no = pi;
	}
}


inline const PlaneStack *find_plane ( const Ppyr &pyramid, const PlaneSel &sel )
{
	for (const auto &stack : pyramid.planes)
		if (stack.plane == sel)
			return &stack;
	return nullptr;
}


inline int find_plane_no ( const Ppyr &pyramid, const PlaneSel &sel )
{
	for (auto pi = 0; pi < (int)pyramid.planes.size(); ++pi)
		if (pyramid.planes[pi].plane == sel)
			return pi;
	return -1;
}


// Canonical native geometry for one plane. `levels` owns every level of every
// plane and has no resolution-order contract; the selected plane's ladder is
// the only authoritative way to find its finest level.
//
// Rendering deliberately remains scalar-metric. `source_pixel_metric` retains
// the independent values reported by the file for metadata and diagnostics,
// but `render_mpp` and `metric_bounds` use Level::metric on both axes.
struct WsiNativeGeometry {
	PlaneSel plane;
	int native_level_index = -1;
	Rtlbr<int> pixel_bounds = {{0, 0}, {0, 0}};
	float render_mpp = 0.0f;
	Rtlbr<float> metric_bounds = {{0, 0}, {0, 0}};
	PixelMetric source_pixel_metric;

	[[nodiscard]] bool valid () const {
		return native_level_index >= 0 && render_mpp > 0.0f &&
			!pixel_bounds.is_empty() && !metric_bounds.is_empty();
	}
};


inline std::optional<WsiNativeGeometry> native_geometry_for_plane (
	const Ppyr &pyramid, const PlaneSel &sel )
{
	const auto *stack = find_plane( pyramid, sel );
	if (!stack || stack->ladder.empty())
		return std::nullopt;

	const auto level_index = stack->ladder.front();
	if (level_index < 0 || level_index >= (int)pyramid.levels.size())
		return std::nullopt;
	const auto &level = pyramid.levels[level_index];
	if (!( level.metric > 0.0f ) || level.bounds.is_empty())
		return std::nullopt;

	auto geometry = WsiNativeGeometry{};
	geometry.plane = stack->plane;
	geometry.native_level_index = level_index;
	geometry.pixel_bounds = level.bounds;
	geometry.render_mpp = level.metric;
	geometry.metric_bounds = {
		{ level.bounds.tl.x * level.metric, level.bounds.tl.y * level.metric },
		{ level.bounds.br.x * level.metric, level.bounds.br.y * level.metric } };
	geometry.source_pixel_metric = pyramid.source_pixel_metric;
	return geometry;
}


// The ordinary brightfield plane is the display default. A named-channel slide
// has no such plane, so retain its registration order and use its first stack;
// resolution selection still goes through that stack's sorted ladder.
inline std::optional<WsiNativeGeometry> display_native_geometry (
	const Ppyr &pyramid )
{
	if (auto geometry = native_geometry_for_plane( pyramid, PlaneSel{} ))
		return geometry;
	if (pyramid.planes.empty())
		return std::nullopt;
	return native_geometry_for_plane( pyramid, pyramid.planes.front().plane );
}


// Bytes of pool `plane_no` may hold: its own budget if one was set, otherwise an
// equal share of the pyramid-wide cap. 0 means unlimited, which is what a
// pyramid-wide cap of 0 already means.
inline size_t plane_ram_cap ( const Ppyr &pyramid, int plane_no )
{
	if (plane_no < 0 || plane_no >= (int)pyramid.planes.size())
		return pyramid.ram_cap_bytes;
	if (auto own = pyramid.planes[plane_no].ram_cap_bytes; own > 0)
		return own;
	if (pyramid.ram_cap_bytes == 0)
		return 0;
	return pyramid.ram_cap_bytes /
		std::max<size_t>( size_t{ 1 }, pyramid.planes.size() );
}


// Give one plane its own pool budget; 0 restores the equal share.
//
// For a caller whose access is lopsided - an analysis pass that walks one channel
// tile by tile while barely touching the rest. An equal split is the right
// default and the wrong answer there: the busy plane thrashes its pool against a
// third of the budget while two idle planes sit on the rest.
//
// The pyramid-wide cap still applies on top, so raising one plane does not let
// the total exceed Ppyr::ram_cap_bytes. Returns false when no such plane exists.
inline bool set_plane_ram_cap ( Ppyr &pyramid, const PlaneSel &sel, size_t bytes )
{
	auto plane_no = find_plane_no( pyramid, sel );
	if (plane_no < 0) {
		pyr::plog_err( "pyr plane: no plane '%s' z=%.3f to budget\n",
			sel.channel.c_str(), (double)sel.zlevel );
		return false;
	}
	pyramid.planes[plane_no].ram_cap_bytes = bytes;
	pyr::plog( "pyr plane: channel '%s' z=%.3f pool budget = %zu bytes%s\n",
		sel.channel.c_str(), (double)sel.zlevel, bytes,
		bytes == 0 ? " (equal share)" : "" );
	return true;
}


// The resolution ladder of one plane, finest first.
//
// `pixeltype` states what the caller expects to render; a mismatch against the
// plane's own type is reported and yields nothing, rather than handing back
// levels whose buffers would be reinterpreted as the wrong type.
template <typename pixeltype>
std::vector<Level *> get_levels_for ( Ppyr &pyramid, float zlevel,
	const std::string &channel = {} )
{
	auto out = std::vector<Level *>{};
	const auto *stack = find_plane( pyramid, PlaneSel{ channel, zlevel } );
	if (!stack)
		return out;
	if (stack->pixel_type != pixel_type_of<pixeltype>::value) {
		pyr::plog_err(
			"pyr plane: channel '%s' z=%.3f holds %s, asked for %s\n",
			channel.c_str(), (double)zlevel,
			pixel_type_name( stack->pixel_type ),
			pixel_type_name( pixel_type_of<pixeltype>::value ) );
		return out;
	}
	out.reserve( stack->ladder.size() );
	for (auto li : stack->ladder)
		out.push_back( &pyramid.levels[li] );
	return out;
}



// ---- auxiliary images ------------------------------------------------------

// The slot named `name`, created empty if the file had none. For writing.
inline Pwhl<Rgb> &aux_slot ( Ppyr &pyramid, const std::string &name )
{
	auto it = pyramid.aux_images.find( name );
	if (it == pyramid.aux_images.end()) {
		pyramid.aux_order.push_back( name );
		it = pyramid.aux_images.emplace( name, Pwhl<Rgb>{} ).first;
	}
	return it->second;
}


// The image named `name`, or an empty one. Querying never creates a slot, so
// `aux_order` stays a record of what the file actually carried.
inline const Pwhl<Rgb> &aux_image ( const Ppyr &pyramid, const std::string &name )
{
	static const Pwhl<Rgb> none;
	auto it = pyramid.aux_images.find( name );
	return it == pyramid.aux_images.end() ? none : it->second;
}


inline const Pwhl<Rgb> &frost_image ( const Ppyr &pyramid )
{
	return aux_image( pyramid, kAuxFrost );
}


inline const Pwhl<Rgb> &macro_image ( const Ppyr &pyramid )
{
	return aux_image( pyramid, kAuxMacro );
}


// What to show as the slide label, and what to read a barcode off.
//
// The dedicated label capture when the file has one, otherwise the whole-slide
// macro - which contains the label panel, just uncropped. Callers ask for the
// PURPOSE and are spared knowing which kind of file they have; that knowledge
// used to be duplicated as an ad-hoc fallback at each call site.
inline const Pwhl<Rgb> &label_image ( const Ppyr &pyramid )
{
// Borrow the stored raster explicitly: GCC 13 otherwise warns about the
// temporary string used as the lookup key, whose lifetime is unrelated.
	const auto *frost = &aux_image( pyramid, kAuxFrost );
	return frost->is_set() ? *frost : aux_image( pyramid, kAuxMacro );
}


// What to show as the whole-slide view: the tissue-only overview when the file
// has one, otherwise the macro.
inline const Pwhl<Rgb> &slide_overview_image ( const Ppyr &pyramid )
{
	const auto *overview = &aux_image( pyramid, kAuxOverview );
	return overview->is_set() ? *overview : aux_image( pyramid, kAuxMacro );
}


// ---- properties ------------------------------------------------------------

inline void set_property ( Ppyr &pyramid, const std::string &key,
	const std::string &value )
{
	if (value.empty())
		return;
	for (auto &prop : pyramid.properties) {
		if (prop.key == key) {
			prop.value = value;
			return;
		}
	}
	pyramid.properties.push_back( Property{ key, value } );
}


inline const std::string *find_property ( const Ppyr &pyramid,
	const std::string &key )
{
	for (const auto &prop : pyramid.properties)
		if (prop.key == key)
			return &prop.value;
	return nullptr;
}


// Record what the ICC profile is, without applying it.
//
// Presence, size and a digest - enough for a run to state afterwards whether the
// slide was colour-managed, and to tell two slides' profiles apart. Deliberately
// NOT applied: OD is only meaningful on linear transmittance, so linearising
// would shift every OD-derived measure in every script at once. That is a
// separate, explicit decision with its own retuning pass.
inline void record_icc_properties ( Ppyr &pyramid )
{
	if (pyramid.icc_profile.empty())
		return;

	set_property( pyramid, "openslide.icc-size",
		std::to_string( pyramid.icc_profile.size() ) );

//
// FNV-1a, for telling profiles apart in a log - not a cryptographic claim.
	auto hash = uint64_t{ 14695981039346656037ull };
	for (auto byte : pyramid.icc_profile) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	char digest[24];
	std::snprintf( digest, sizeof( digest ), "%016llx",
		(unsigned long long)hash );
	set_property( pyramid, "pyr.icc-fnv1a", digest );

//
// The profile's own description, at its documented offset in the `desc` tag, if
// the tag table is intact. Best effort: a malformed profile just yields nothing.
	const auto &bytes = pyramid.icc_profile;
	if (bytes.size() > 132) {
		auto be32 = [&bytes] ( size_t at ) {
			return ( (uint32_t)bytes[at] << 24 ) | ( (uint32_t)bytes[at + 1] << 16 ) |
				( (uint32_t)bytes[at + 2] << 8 ) | (uint32_t)bytes[at + 3];
		};
		auto count = be32( 128 );
		for (auto i = uint32_t{ 0 }; i < count && 132 + i * 12 + 12 <= bytes.size(); ++i) {
			auto at = size_t{ 132 } + i * 12;
			if (be32( at ) != 0x64657363u)			// 'desc'
				continue;
			auto offset = be32( at + 4 );
			auto size = be32( at + 8 );
			if (offset + 12 >= bytes.size() || size < 12)
				break;
			auto text_at = offset + 12;
			auto text_len = size_t{ 0 };
			while (text_at + text_len < bytes.size() && text_len < 128 &&
				bytes[text_at + text_len] != 0)
			{
				++text_len;
			}
			if (text_len)
				set_property( pyramid, "pyr.icc-desc",
					std::string( (const char *)&bytes[text_at], text_len ) );
			break;
		}
	}
}


// Fill in the metadata that is derivable from the pyramid itself.
//
// Central rather than per loader: level geometry and the auxiliary image list
// are the same questions whatever the format, and five copies of the answer
// would drift. Loaders add what only they know - the raw vendor tags.
inline void record_derived_properties ( Ppyr &pyramid )
{
	set_property( pyramid, "openslide.level-count",
		std::to_string( pyramid.levels.size() ) );

	auto display_geometry = display_native_geometry( pyramid );
	auto base_metric = display_geometry ? display_geometry->render_mpp : 0.0f;
	if (base_metric > 0.0f) {
		const auto source_x = pyramid.source_pixel_metric.x > 0.0f
			? pyramid.source_pixel_metric.x : base_metric;
		const auto source_y = pyramid.source_pixel_metric.y > 0.0f
			? pyramid.source_pixel_metric.y : base_metric;
		set_property( pyramid, "openslide.mpp-x", std::to_string( source_x ) );
		set_property( pyramid, "openslide.mpp-y", std::to_string( source_y ) );
	}

	for (auto i = 0; i < (int)pyramid.levels.size(); ++i) {
		const auto &level = pyramid.levels[i];
		auto prefix = "openslide.level[" + std::to_string( i ) + "].";
		set_property( pyramid, prefix + "width",
			std::to_string( level.bounds.br.x - level.bounds.tl.x ) );
		set_property( pyramid, prefix + "height",
			std::to_string( level.bounds.br.y - level.bounds.tl.y ) );
		auto plane_base_metric = 0.0f;
		if (auto geometry = native_geometry_for_plane( pyramid, level.plane ))
			plane_base_metric = geometry->render_mpp;
		if (plane_base_metric > 0.0f && level.metric > 0.0f)
			set_property( pyramid, prefix + "downsample",
				std::to_string( level.metric / plane_base_metric ) );
		set_property( pyramid, prefix + "tile-width", std::to_string( level.tile_w ) );
		set_property( pyramid, prefix + "tile-height", std::to_string( level.tile_h ) );
	}

	for (const auto &name : pyramid.aux_order) {
		const auto &image = aux_image( pyramid, name );
		if (!image.is_set())
			continue;
		auto prefix = "openslide.associated[" + name + "].";
		set_property( pyramid, prefix + "width", std::to_string( image.w ) );
		set_property( pyramid, prefix + "height", std::to_string( image.h ) );
	}
}

} // namespace priml
