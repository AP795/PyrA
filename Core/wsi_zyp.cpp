// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT


// ZYP loader (Win300 scanners). There is no public description of this format:
// everything below was established from the files themselves, and what could not
// be established is said so. docs/wsia_tech/Pyr.md holds the layout and how each
// fact was measured.
//
// The trailer is an MFC CArchive stream, so its strings are CStrings: FF FE FF, a
// length (one byte; FF escapes to 16 bits, FFFF to 32), then that many UTF-16LE
// units. Integers are little-endian.
//
//   0              tile JPEGs, concatenated in the writer's order
//   tiles end      the Preview, OriginalPreview and Barcode JPEGs
//   dictionary     groups up to the tile index: key, u32 n, n x (name, value)
//   tile index     two digests, then one group per tile, "0,z,level,x,y", holding
//                  StartPosition and DataLength as decimal strings
//   FOV table      two digests, then the acquisition fields (not read)
//   two sections   two digests each
//   footer         u64 x 4 (the sections above), CString version, u64 version offset
//
// A tile's key addresses a uniform 256-pixel grid over the whole stage, so level k
// is exactly level 0 reduced by 2^k. The scanned rectangle (ROI) is stated in
// Preview pixels, each 512 level-0 pixels wide.

#include "wsi.h"

#include "img_codec.h"
#include "pyr_rt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace priml {

namespace {

constexpr int kZypTileSize = 256;
constexpr int kZypRoiUnitPx = 512;
//
// The ROI is a multiple of 512, so its origin lands on a whole pixel down to level
// 9, the coarsest level a file stores. The cap only guards the shifts below against
// a corrupt key.
constexpr int kZypMaxLevel = 20;
constexpr int kZypKeyFields = 5;
constexpr int kZypDigestsPerSection = 2;
constexpr int kZypFooterOffsets = 4;
constexpr size_t kZypFooterOffsetBytes = kZypFooterOffsets * sizeof( uint64_t );
//
// Sanity limits that keep a corrupt offset from turning into a huge allocation. A
// 5-plane slide's index is 30 MB and its footer 20 bytes.
constexpr uint64_t kZypMaxSectionBytes = 1ull << 30;
constexpr uint64_t kZypMaxFooterBytes = 4096;
//
// Every sample's smallest tile is a pure 255 JPEG. The floor only allows for what
// a coarser JPEG quantisation might do to it.
constexpr int kZypWhiteFloor = 250;

constexpr std::string_view kZypKnownVersion = "2.01";
constexpr std::string_view kZypFirstGroup = "Preview";
constexpr const char *kZypAuxOriginalPreview = "original-preview";


// A read of `count` bytes at `offset`, into `dest`.
using ZypReadFn = std::function<bool( uint64_t offset, size_t count, uint8_t *dest )>;


uint32_t get_le32 ( const uint8_t *p )
{
	auto value = uint32_t{ 0 };
	for (auto i = 3; i >= 0; --i)
		value = ( value << 8 ) | p[i];
	return value;
}


uint64_t get_le64 ( const uint8_t *p )
{
	auto value = uint64_t{ 0 };
	for (auto i = 7; i >= 0; --i)
		value = ( value << 8 ) | p[i];
	return value;
}


void append_utf8 ( std::string &out, uint32_t cp )
{
	if (cp < 0x80) {
		out += (char)cp;
	}
	else if (cp < 0x800) {
		out += (char)( 0xC0 | ( cp >> 6 ) );
		out += (char)( 0x80 | ( cp & 0x3F ) );
	}
	else if (cp < 0x10000) {
		out += (char)( 0xE0 | ( cp >> 12 ) );
		out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		out += (char)( 0x80 | ( cp & 0x3F ) );
	}
	else {
		out += (char)( 0xF0 | ( cp >> 18 ) );
		out += (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
		out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		out += (char)( 0x80 | ( cp & 0x3F ) );
	}
}


// Bounds-checked reader over one section. Every read reports whether it fitted, so
// a truncated or foreign file ends in `false` rather than past the buffer.
class ZypCursor {
public:
	ZypCursor ( const uint8_t *data, size_t size )
		: data_( data ), size_( size ) {}

	bool at_end () const { return pos_ == size_; }

	bool read_u8 ( uint8_t &out ) {
		if (remaining() < 1)
			return false;
		out = data_[pos_++];
		return true;
	}

	bool read_u16 ( uint16_t &out ) {
		if (remaining() < 2)
			return false;
		out = (uint16_t)( data_[pos_] | ( data_[pos_ + 1] << 8 ) );
		pos_ += 2;
		return true;
	}

	bool read_u32 ( uint32_t &out ) {
		if (remaining() < 4)
			return false;
		out = get_le32( data_ + pos_ );
		pos_ += 4;
		return true;
	}

	bool read_string ( std::string &out );

// Whether `count` (name, value) pairs could fit in what is left. A pair is at least
// two empty strings, so a corrupt count cannot become a huge allocation.
	bool can_hold_entries ( uint32_t count ) const {
		return remaining() / kEmptyPairBytes >= count;
	}

private:
	static constexpr size_t kEmptyPairBytes = 8;

	size_t remaining () const { return size_ - pos_; }
	bool read_length ( uint32_t &units );

	const uint8_t *data_;
	size_t size_;
	size_t pos_ = 0;
};


bool ZypCursor::read_length ( uint32_t &units )
{
	auto one = uint8_t{ 0 };
	if (!read_u8( one ))
		return false;
	units = one;
	if (one != 0xFF)
		return true;

	auto two = uint16_t{ 0 };
	if (!read_u16( two ))
		return false;
	units = two;
	return two != 0xFFFF || read_u32( units );
}


bool ZypCursor::read_string ( std::string &out )
{
	constexpr std::array<uint8_t, 3> kUnicodeMark{ 0xFF, 0xFE, 0xFF };
	if (remaining() < kUnicodeMark.size() ||
		!std::equal( kUnicodeMark.begin(), kUnicodeMark.end(), data_ + pos_ ))
	{
		return false;
	}
	pos_ += kUnicodeMark.size();

	auto units = uint32_t{ 0 };
	if (!read_length( units ) || remaining() / 2 < units)
		return false;

	out.clear();
	const auto *p = data_ + pos_;
	pos_ += (size_t)units * 2;
	for (auto i = uint32_t{ 0 }; i < units; ++i, p += 2) {
		auto cp = (uint32_t)p[0] | ( (uint32_t)p[1] << 8 );
		if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units) {
			auto low = (uint32_t)p[2] | ( (uint32_t)p[3] << 8 );
			if (low >= 0xDC00 && low < 0xE000) {
				cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( low - 0xDC00 );
				++i;
				p += 2;
			}
		}
		append_utf8( out, cp );
	}
	return true;
}


using ZypEntries = std::vector<std::pair<std::string, std::string>>;

struct ZypGroup {
	std::string key;
	ZypEntries entries;
};


const std::string *find_entry ( const ZypEntries &entries, std::string_view name )
{
	for (const auto &[key, value] : entries)
		if (key == name)
			return &value;
	return nullptr;
}


// The dictionary and the tile index share one shape. `group` is reused between
// calls, which matters on the index: a z-stack has a quarter of a million of them.
template <typename OnGroup>
bool read_groups ( ZypCursor &cursor, OnGroup &&on_group )
{
	auto group = ZypGroup{};
	while (!cursor.at_end()) {
		auto count = uint32_t{ 0 };
		if (!cursor.read_string( group.key ) || !cursor.read_u32( count ))
			return false;
		if (!cursor.can_hold_entries( count ))
			return false;
		group.entries.resize( count );
		for (auto &[name, value] : group.entries) {
			if (!cursor.read_string( name ) || !cursor.read_string( value ))
				return false;
		}
		if (!on_group( group ))
			return false;
	}
	return true;
}


template <typename Number>
bool parse_number ( std::string_view text, Number &out )
{
	const auto *end = text.data() + text.size();
	auto [ptr, ec] = std::from_chars( text.data(), end, out );
	return ec == std::errc{} && ptr == end;
}


struct ZypFooter {
	uint64_t index_offset = 0;
	uint64_t fov_offset = 0;
	std::string version;
};


bool read_footer ( const ZypReadFn &read, uint64_t file_size, ZypFooter &footer )
{
	if (file_size < sizeof( uint64_t ))
		return false;
	auto tail = std::array<uint8_t, sizeof( uint64_t )>{};
	if (!read( file_size - tail.size(), tail.size(), tail.data() ))
		return false;

	const auto version_offset = get_le64( tail.data() );
	if (version_offset < kZypFooterOffsetBytes ||
		version_offset > file_size - tail.size() ||
		file_size - version_offset > kZypMaxFooterBytes)
	{
		return false;
	}

	const auto start = version_offset - kZypFooterOffsetBytes;
	auto bytes = std::vector<uint8_t>( (size_t)( file_size - tail.size() - start ) );
	if (!read( start, bytes.size(), bytes.data() ))
		return false;

	auto offsets = std::array<uint64_t, kZypFooterOffsets>{};
	auto previous = uint64_t{ 0 };
	for (auto i = 0; i < kZypFooterOffsets; ++i) {
		offsets[i] = get_le64( bytes.data() + i * sizeof( uint64_t ) );
		if (offsets[i] <= previous || offsets[i] >= start)
			return false;
		previous = offsets[i];
	}
	footer.index_offset = offsets[0];
	footer.fov_offset = offsets[1];

	auto cursor = ZypCursor( bytes.data() + kZypFooterOffsetBytes,
		bytes.size() - kZypFooterOffsetBytes );
	return cursor.read_string( footer.version ) && cursor.at_end();
}


bool read_section ( const ZypReadFn &read, uint64_t begin, uint64_t end,
	std::vector<uint8_t> &bytes )
{
	if (end <= begin || end - begin > kZypMaxSectionBytes)
		return false;
	bytes.resize( (size_t)( end - begin ) );
	return read( begin, bytes.size(), bytes.data() );
}


// One stored tile, as the index states it. The fields of the key: the first is
// always 0 in every sample (a channel or scene, presumably), the second is the
// focal plane.
struct ZypTileEntry {
	int reserved = 0;
	int plane = 0;
	int level = 0;
	int x = 0;
	int y = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
};


bool parse_tile_group ( const ZypGroup &group, ZypTileEntry &tile )
{
	auto fields = std::array<int *, kZypKeyFields>{
		&tile.reserved, &tile.plane, &tile.level, &tile.x, &tile.y };
	auto rest = std::string_view( group.key );
	for (auto i = 0; i < kZypKeyFields; ++i) {
		auto comma = rest.find( ',' );
		if (( i + 1 < kZypKeyFields ) == ( comma == std::string_view::npos ))
			return false;
		if (!parse_number( rest.substr( 0, comma ), *fields[i] ))
			return false;
		rest.remove_prefix( comma == std::string_view::npos ? rest.size() : comma + 1 );
	}

	const auto *offset = find_entry( group.entries, "StartPosition" );
	const auto *size = find_entry( group.entries, "DataLength" );
	return offset && size &&
		parse_number( *offset, tile.offset ) && parse_number( *size, tile.size ) &&
		tile.level >= 0 && tile.level <= kZypMaxLevel && tile.x >= 0 && tile.y >= 0;
}


bool read_tile_index ( const ZypReadFn &read, const ZypFooter &footer,
	std::vector<ZypTileEntry> &tiles )
{
	auto bytes = std::vector<uint8_t>{};
	if (!read_section( read, footer.index_offset, footer.fov_offset, bytes ))
		return false;

	auto cursor = ZypCursor( bytes.data(), bytes.size() );
	auto digest = std::string{};
	for (auto i = 0; i < kZypDigestsPerSection; ++i) {
		if (!cursor.read_string( digest ))
			return false;
	}
	return read_groups( cursor, [&] ( const ZypGroup &group ) {
		auto tile = ZypTileEntry{};
		if (!parse_tile_group( group, tile ) ||
			tile.size == 0 || tile.offset >= footer.index_offset ||
			tile.size > footer.index_offset - tile.offset ||
			tile.size > (uint64_t)INT32_MAX)
		{
			return false;
		}
		tiles.push_back( tile );
		return true;
	} );
}


using ZypDictionary = std::map<std::string, ZypEntries>;


// Nothing points at the dictionary. It starts after the auxiliary JPEGs, whose
// extents only the dictionary itself states, so it is found by its first key: the
// range to search is small, and a JPEG cannot hold that byte run.
bool read_dictionary ( const ZypReadFn &read, const ZypFooter &footer,
	const std::vector<ZypTileEntry> &tiles, ZypDictionary &dictionary )
{
	auto tiles_end = uint64_t{ 0 };
	for (const auto &tile : tiles)
		tiles_end = std::max( tiles_end, tile.offset + tile.size );

	auto bytes = std::vector<uint8_t>{};
	if (!read_section( read, tiles_end, footer.index_offset, bytes ))
		return false;

	auto signature = std::vector<uint8_t>{ 0xFF, 0xFE, 0xFF,
		(uint8_t)kZypFirstGroup.size() };
	for (auto c : kZypFirstGroup) {
		signature.push_back( (uint8_t)c );
		signature.push_back( 0 );
	}
	auto start = std::search( bytes.begin(), bytes.end(),
		signature.begin(), signature.end() );
	if (start == bytes.end())
		return false;

	const auto skip = (size_t)( start - bytes.begin() );
	auto cursor = ZypCursor( bytes.data() + skip, bytes.size() - skip );
	return read_groups( cursor, [&] ( const ZypGroup &group ) {
		dictionary[group.key] = group.entries;
		return true;
	} );
}


struct ZypGeometry {
	float mpp = 0.0f;
	Rtlbr<int> roi = {{0, 0}, {0, 0}};	// level-0 pixels of the stage canvas
};


bool read_int_entry ( const ZypEntries &entries, std::string_view name, int &out )
{
	const auto *text = find_entry( entries, name );
	return text && parse_number( *text, out );
}


bool read_geometry ( const ZypEntries &slice_info, ZypGeometry &geometry )
{
	auto left = 0;
	auto top = 0;
	auto right = 0;
	auto bottom = 0;
	const auto *mpp = find_entry( slice_info, "MicrometersPerPixel" );
	if (!mpp || !parse_number( *mpp, geometry.mpp ) || !( geometry.mpp > 0.0f ) ||
		!read_int_entry( slice_info, "ROILeft", left ) ||
		!read_int_entry( slice_info, "ROITop", top ) ||
		!read_int_entry( slice_info, "ROIRight", right ) ||
		!read_int_entry( slice_info, "ROIBottom", bottom ) ||
		left < 0 || top < 0 || right <= left || bottom <= top ||
		right > INT32_MAX / kZypRoiUnitPx || bottom > INT32_MAX / kZypRoiUnitPx)
	{
		return false;
	}
	geometry.roi = { { left * kZypRoiUnitPx, top * kZypRoiUnitPx },
		{ right * kZypRoiUnitPx, bottom * kZypRoiUnitPx } };
	return true;
}


// Stored focal-plane identifiers, in acquisition order. Their physical spacing
// is unverified; keep them as identifiers rather than inventing micrometres.
std::vector<int> focal_planes ( const std::vector<ZypTileEntry> &tiles )
{
	auto planes = std::vector<int>{};
	for (const auto &tile : tiles) {
		if (std::find( planes.begin(), planes.end(), tile.plane ) == planes.end())
			planes.push_back( tile.plane );
	}
	std::sort( planes.begin(), planes.end() );
	return planes;
}


struct ZypStoredTile {
	uint64_t offset = 0;
	uint64_t size = 0;
};


// Rejects a slide whose tiles are not the 256-pixel grid everything here assumes,
// and finds the tile to repeat where the index stores none: the smallest one, once
// decoding confirms it is white. Size 0 when there is no such tile; the cells then
// stay background, which is black.
bool find_white_tile ( const ZypReadFn &read, const std::vector<ZypTileEntry> &tiles,
	int plane, ZypStoredTile &white )
{
	const ZypTileEntry *smallest = nullptr;
	for (const auto &tile : tiles) {
		if (tile.plane == plane && ( !smallest || tile.size < smallest->size ))
			smallest = &tile;
	}
	if (!smallest)
		return false;

	auto bytes = std::vector<uint8_t>( (size_t)smallest->size );
	auto pixels = Pwhl<Rgb>{};
	if (!read( smallest->offset, bytes.size(), bytes.data() ) ||
		!decode_to_image<Rgb>( bytes.data(), bytes.size(), pixels ) ||
		pixels.w != kZypTileSize || pixels.h != kZypTileSize)
	{
		return false;
	}

	for (auto y = 0; y < pixels.h; ++y) {
		const auto *row = pixels.rowp( y );
		for (auto x = 0; x < pixels.w; ++x) {
			if (row[x].r < kZypWhiteFloor || row[x].g < kZypWhiteFloor ||
				row[x].b < kZypWhiteFloor)
			{
				return true;
			}
		}
	}
	white = { smallest->offset, smallest->size };
	return true;
}


// Which tiles of one level reach the ROI, as a grid of key cells.
struct ZypCellGrid {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	int origin_x = 0;	// the ROI corner in this level's stage pixels
	int origin_y = 0;

	ZypCellGrid ( const Rtlbr<int> &roi, int level_no ) {
		const auto ceil_shift = [level_no] ( int px ) {
			return (int)( ( (int64_t)px + ( 1ll << level_no ) - 1 ) >> level_no );
		};
		origin_x = roi.tl.x >> level_no;
		origin_y = roi.tl.y >> level_no;
		x0 = origin_x / kZypTileSize;
		y0 = origin_y / kZypTileSize;
		x1 = ( ceil_shift( roi.br.x ) + kZypTileSize - 1 ) / kZypTileSize;
		y1 = ( ceil_shift( roi.br.y ) + kZypTileSize - 1 ) / kZypTileSize;
	}

	int width () const { return x1 - x0; }
	int height () const { return y1 - y0; }
	bool holds ( int x, int y ) const {
		return x >= x0 && x < x1 && y >= y0 && y < y1;
	}
	size_t cell ( int x, int y ) const {
		return (size_t)( y - y0 ) * width() + (size_t)( x - x0 );
	}
};


void append_tile ( Level &level, const ZypCellGrid &grid, int x, int y,
	const ZypStoredTile &stored )
{
	auto tile = Tile{};
//
// Shifted so the ROI starts at the origin, which is why a coarse tile straddling
// the ROI corner has a negative rectangle: it cannot be cropped without decoding.
	const auto left = x * kZypTileSize - grid.origin_x;
	const auto top = y * kZypTileSize - grid.origin_y;
	tile.lr = { { left, top }, { left + kZypTileSize, top + kZypTileSize } };
	tile.foff = (size_t)stored.offset;
	tile.fsz = (size_t)stored.size;
	level.tiles.push_back( std::move( tile ) );
}


// One pyramid level from the tiles of one key level. Tiles wholly outside the ROI
// are dropped, and every ROI cell the index does not store becomes the white tile:
// the frame is allocated zeroed, and black on a brightfield slide reads as tissue
// at saturated OD. The vendor's own coarse levels store exactly such tiles.
Level build_level ( const std::vector<const ZypTileEntry *> &stored, int level_no,
	const ZypGeometry &geometry, const ZypStoredTile &white, int &filled )
{
	const auto grid = ZypCellGrid( geometry.roi, level_no );
	auto level = Level{};
	level.metric = geometry.mpp * (float)( 1ll << level_no );
	level.tile_w = kZypTileSize;
	level.tile_h = kZypTileSize;

	auto occupied = std::vector<bool>( (size_t)grid.width() * grid.height(), false );
	auto max_size = white.size;
	for (const auto *entry : stored) {
		if (!grid.holds( entry->x, entry->y ) || occupied[grid.cell( entry->x, entry->y )])
			continue;
		occupied[grid.cell( entry->x, entry->y )] = true;
		max_size = std::max( max_size, entry->size );
		append_tile( level, grid, entry->x, entry->y, { entry->offset, entry->size } );
	}
	for (auto y = grid.y0; y < grid.y1; ++y) {
		for (auto x = grid.x0; x < grid.x1; ++x) {
			if (occupied[grid.cell( x, y )])
				continue;
			append_tile( level, grid, x, y, white );
			++filled;
		}
	}

	set_level_bounds( level );
	level.tile_buffer_bytes = max_size;
	return level;
}


bool build_levels ( const std::vector<ZypTileEntry> &tiles, int plane,
	const ZypGeometry &geometry, const ZypStoredTile &white, Ppyr &pyramid, int &filled )
{
	auto by_level = std::map<int, std::vector<const ZypTileEntry *>>{};
	for (const auto &tile : tiles) {
		if (tile.plane == plane)
			by_level[tile.level].push_back( &tile );
	}
	if (by_level.empty() || by_level.begin()->first != 0)
		return false;

	for (const auto &[level_no, stored] : by_level) {
		auto level = build_level( stored, level_no, geometry, white, filled );
		level.plane.zlevel = float( plane );
		pyramid.levels.push_back( std::move( level ) );
	}
	return true;
}


void read_aux_image ( const ZypReadFn &read, const ZypDictionary &dictionary,
	const char *group, const char *slot, uint64_t limit, Ppyr &pyramid )
{
	auto entries = dictionary.find( group );
	if (entries == dictionary.end())
		return;

	const auto *offset_text = find_entry( entries->second, "StartPosition" );
	const auto *size_text = find_entry( entries->second, "DataLength" );
	auto offset = uint64_t{ 0 };
	auto size = uint64_t{ 0 };
	auto bytes = std::vector<uint8_t>{};
	if (!offset_text || !size_text || !parse_number( *offset_text, offset ) ||
		!parse_number( *size_text, size ) || size == 0 || offset >= limit ||
		size > limit - offset || size > kZypMaxSectionBytes)
	{
		pyr::plog( "wsi zyp: %s has no usable extent\n", group );
		return;
	}

	bytes.resize( (size_t)size );
	auto pixels = Pwhl<Rgb>{};
	if (!read( offset, bytes.size(), bytes.data() ) ||
		!decode_to_image<Rgb>( bytes.data(), bytes.size(), pixels ))
	{
		pyr::plog( "wsi zyp: %s did not decode\n", group );
		return;
	}
	aux_slot( pyramid, slot ) = std::move( pixels );
}


void record_properties ( const ZypDictionary &dictionary, const ZypFooter &footer,
	Ppyr &pyramid )
{
	set_property( pyramid, "zyp.Version", footer.version );
	const auto slice_info = dictionary.find( "SliceInfo" );
	if (slice_info == dictionary.end())
		return;
	for (const auto &[name, value] : slice_info->second)
		set_property( pyramid, "zyp." + name, value );
//
// ScanZoom, not ObjectiveLens: the scanner reaches 40x through a 20x objective, and
// the magnification of the image is what the normalised key means.
	if (const auto *zoom = find_property( pyramid, "zyp.ScanZoom" ))
		set_property( pyramid, "pyra.objective-power", *zoom );
}


// Fields of the format this reader has never seen set, reported so that a slide
// which uses them does not silently show part of itself.
void log_unknown_usage ( const std::vector<ZypTileEntry> &tiles,
	const ZypDictionary &dictionary, const char *filename )
{
	auto reserved = size_t{ 0 };
	for (const auto &tile : tiles)
		reserved += tile.reserved != 0 ? 1 : 0;
	if (reserved)
		pyr::plog( "wsi zyp: %s: %zu tiles have a non-zero first key field; shown anyway, "
			"meaning unknown\n", filename, reserved );

	auto fluorescence = dictionary.find( "Fluorescence" );
	auto channels = 0;
	if (fluorescence != dictionary.end() &&
		read_int_entry( fluorescence->second, "FluorescenceChannelNum", channels ) &&
		channels > 0)
	{
		pyr::plog( "wsi zyp: %s states %d fluorescence channels; not supported, "
			"reading it as brightfield\n", filename, channels );
	}
}

} // namespace


bool ZypLoader::Open ( Ppyr &pyramid )
{
	auto fail = [this] ( const char *stage ) {
		pyr::plog_err( "wsi zyp: %s: %s\n", this->filename_.c_str(), stage );
		return false;
	};

	auto ec = std::error_code{};
	const auto file_size = (uint64_t)std::filesystem::file_size( this->filename_, ec );
	if (ec)
		return fail( "cannot read the file size" );

	const auto read = ZypReadFn( [this] ( uint64_t offset, size_t count, uint8_t *dest ) {
		return this->ReadFromFile( dest, (size_t)offset, count );
	} );

	auto footer = ZypFooter{};
	if (!read_footer( read, file_size, footer ))
		return fail( "footer not recognised" );
	if (footer.version != kZypKnownVersion)
		pyr::plog( "wsi zyp: %s: version %s, only %s has been seen\n",
			this->filename_.c_str(), footer.version.c_str(), kZypKnownVersion.data() );

	auto tiles = std::vector<ZypTileEntry>{};
	if (!read_tile_index( read, footer, tiles ) || tiles.empty())
		return fail( "tile index not recognised" );

	auto dictionary = ZypDictionary{};
	if (!read_dictionary( read, footer, tiles, dictionary ))
		return fail( "metadata dictionary not recognised" );

	const auto slice_info = dictionary.find( "SliceInfo" );
	auto geometry = ZypGeometry{};
	if (slice_info == dictionary.end() || !read_geometry( slice_info->second, geometry ))
		return fail( "metadata states no pixel size or ROI" );

	const auto planes = focal_planes( tiles );
	auto filled = 0;
	for (const auto plane : planes) {
		auto white = ZypStoredTile{};
		if (!find_white_tile( read, tiles, plane, white ))
			return fail( "tiles are not 256x256 JPEGs" );
		auto plane_filled = 0;
		if (!build_levels( tiles, plane, geometry, white, pyramid, plane_filled ))
			return fail( "focal plane has no base level" );
		if (white.size == 0 && plane_filled > 0)
			pyr::plog( "wsi zyp: %s: plane %d has no white tile, %d unstored cells stay black\n",
				this->filename_.c_str(), plane, plane_filled );
		filled += plane_filled;
	}
	set_property( pyramid, "pyra.z-unit", "index" );
	set_property( pyramid, "pyra.default-plane-index", std::to_string( ( planes.size() - 1 ) / 2 ) );

	log_unknown_usage( tiles, dictionary, this->filename_.c_str() );

	pyramid.source_pixel_metric = PixelMetric{ geometry.mpp, geometry.mpp };
	pyramid.mrct = { { 0, 0 },
		{ width( geometry.roi ) * geometry.mpp, height( geometry.roi ) * geometry.mpp } };
	pyramid.prefer_pixbf = false;
	record_properties( dictionary, footer, pyramid );
	read_aux_image( read, dictionary, "Barcode", kAuxFrost, footer.index_offset, pyramid );
	read_aux_image( read, dictionary, "Preview", kAuxOverview, footer.index_offset, pyramid );
	read_aux_image( read, dictionary, "OriginalPreview", kZypAuxOriginalPreview,
		footer.index_offset, pyramid );

	pyr::plog( "wsi zyp: %s: %zu levels, %dx%d px, %.4f um/px, %zu tiles stored, "
		"%d white cells, %zu focal planes\n",
		this->filename_.c_str(), pyramid.levels.size(),
		width( geometry.roi ), height( geometry.roi ), geometry.mpp, tiles.size(),
		filled, planes.size() );
	return true;
}


void ZypLoader::Close ( Ppyr &pyramid )
{
	this->CloseFile();
	pyramid.levels.clear();
	pyramid.tbf = {};
	pyramid.pxb = {};
}


bool ZypLoader::LoadTile ( Ppyr &pyramid, int level_no, int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this] ( const auto &level, const auto &tile, uint8_t *dest, size_t &pos, unsigned worker ) {
			return AssembleTile( level, tile, dest, pos, worker );
		} );
}


bool ZypLoader::AssembleTile ( const Level &level, const Tile &tile, uint8_t *dest,
	size_t &pos, unsigned worker )
{
	if (!this->ReadFromFile( dest, tile.foff, tile.fsz, worker, level.filename ))
		return false;
	pos = tile.fsz;
	return true;
}

} // namespace priml
