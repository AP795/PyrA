// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "remote_pyramid.h"

#include "img_codec.h"

#include "pyr_rt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace priml {

namespace {

static_assert( std::endian::native == std::endian::little,
	"The remote geometry blob is written in host byte order, which must be little-endian" );

constexpr auto kGeometryMagic = std::array<uint8_t, 4>{ 'P', 'Y', 'R', 'G' };

// How a level's tile list is stored: every rectangle, or a row-major grid whose
// rectangles follow from origin, tile size and envelope (see GridLayout).
enum class TileListKind : uint8_t { Explicit = 0, Grid = 1 };

// Ceiling on the strings and counts a blob may declare, so a corrupt length
// cannot ask for gigabytes before the reader runs out of input.
constexpr uint32_t kMaxBlobCount = 1u << 28;


// A row-major grid: tile (col, row) covers [origin + index * tile, origin +
// (index + 1) * tile), clipped to `clip`, which is the grid's envelope.
struct GridLayout {
	int origin_x = 0;
	int origin_y = 0;
	int tile_w = 0;
	int tile_h = 0;
	int cols = 0;
	int rows = 0;
	int clip_x = 0;
	int clip_y = 0;

	[[nodiscard]] size_t count () const { return (size_t)cols * (size_t)rows; }

	[[nodiscard]] Rtlbr<int> rect ( size_t index ) const {
		const auto col = (int)( index % (size_t)cols );
		const auto row = (int)( index / (size_t)cols );
		const auto left = origin_x + col * tile_w;
		const auto top = origin_y + row * tile_h;
		return { { left, top }, { std::min( left + tile_w, clip_x ), std::min( top + tile_h, clip_y ) } };
	}
};


// The grid a synthetic level is offered as: virtual tiles over its one raster.
GridLayout synthetic_grid ( const Level &level )
{
	const auto &source = level.tiles.front().lr;
	auto grid = GridLayout{};
	grid.origin_x = source.tl.x;
	grid.origin_y = source.tl.y;
	grid.tile_w = std::min( kRemoteSyntheticTileEdge, width( source ) );
	grid.tile_h = std::min( kRemoteSyntheticTileEdge, height( source ) );
	grid.cols = ( width( source ) + grid.tile_w - 1 ) / grid.tile_w;
	grid.rows = ( height( source ) + grid.tile_h - 1 ) / grid.tile_h;
	grid.clip_x = source.br.x;
	grid.clip_y = source.br.y;
	return grid;
}


bool is_remote_synthetic ( const Level &level )
{
	return level.synthetic && level.tiles.size() == 1 && level.tiles.front().owned_no >= 0 &&
		!level.tiles.front().lr.is_empty() && level.pix;
}


// The stored tiles of `level` as a grid, when they are exactly one.
bool detect_grid ( const Level &level, GridLayout *grid )
{
	const auto &tiles = level.tiles;
	if (tiles.empty() || level.tile_w <= 0 || level.tile_h <= 0)
		return false;
	auto layout = GridLayout{};
	layout.origin_x = tiles.front().lr.tl.x;
	layout.origin_y = tiles.front().lr.tl.y;
	layout.tile_w = level.tile_w;
	layout.tile_h = level.tile_h;
	while (layout.cols < (int)tiles.size() && tiles[layout.cols].lr.tl.y == layout.origin_y)
		++layout.cols;
	if (layout.cols == 0 || tiles.size() % (size_t)layout.cols != 0)
		return false;
	layout.rows = (int)( tiles.size() / (size_t)layout.cols );
	layout.clip_x = std::numeric_limits<int>::min();
	layout.clip_y = std::numeric_limits<int>::min();
	for (const auto &tile : tiles) {
		layout.clip_x = std::max( layout.clip_x, tile.lr.br.x );
		layout.clip_y = std::max( layout.clip_y, tile.lr.br.y );
	}
	for (size_t i = 0; i < tiles.size(); ++i)
		if (!rct_are_eq( tiles[i].lr, layout.rect( i ) ))
			return false;
	*grid = layout;
	return true;
}


size_t remote_pixel_bytes ( LevelPixelType type )
{
	return pixel_size_bytes( type );
}


// Encoded size bound of a synthetic virtual tile: its raw-sample form, which the
// encoder falls back to whenever a codec would not be smaller.
size_t synthetic_slot_bytes ( const Level &level, const GridLayout &grid )
{
	return (size_t)kRawTileHeaderBytes + (size_t)grid.tile_w * (size_t)grid.tile_h *
		remote_pixel_bytes( level.pixel_type );
}


class BlobWriter {
public:
	explicit BlobWriter ( std::vector<uint8_t> &out ) : out_( out ) {}

	template <typename T>
	void put ( T value ) {
		static_assert( std::is_trivially_copyable_v<T> );
		const auto *bytes = reinterpret_cast<const uint8_t *>( &value );
		out_.insert( out_.end(), bytes, bytes + sizeof( T ) );
	}

	void put_rect ( const Rtlbr<int> &rect ) {
		put( rect.tl.x ); put( rect.tl.y ); put( rect.br.x ); put( rect.br.y );
	}

	void put_string ( std::string_view text ) {
		put( (uint32_t)text.size() );
		out_.insert( out_.end(), text.begin(), text.end() );
	}

private:
	std::vector<uint8_t> &out_;
};


class BlobReader {
public:
	BlobReader ( const uint8_t *data, size_t size ) : data_( data ), size_( size ) {}

	template <typename T>
	T get () {
		static_assert( std::is_trivially_copyable_v<T> );
		auto value = T{};
		if (!ok_ || size_ - pos_ < sizeof( T )) {
			ok_ = false;
			return value;
		}
		std::memcpy( &value, data_ + pos_, sizeof( T ) );
		pos_ += sizeof( T );
		return value;
	}

	Rtlbr<int> get_rect () {
		auto rect = Rtlbr<int>{};
		rect.tl.x = get<int32_t>(); rect.tl.y = get<int32_t>();
		rect.br.x = get<int32_t>(); rect.br.y = get<int32_t>();
		return rect;
	}

	std::string get_string () {
		const auto length = get<uint32_t>();
		if (!ok_ || length > kMaxBlobCount || size_ - pos_ < length) {
			ok_ = false;
			return {};
		}
		auto text = std::string( reinterpret_cast<const char *>( data_ + pos_ ), length );
		pos_ += length;
		return text;
	}

	uint32_t get_count () {
		const auto count = get<uint32_t>();
		if (count > kMaxBlobCount)
			ok_ = false;
		return ok_ ? count : 0;
	}

	const uint8_t *take ( size_t length ) {
		if (!ok_ || size_ - pos_ < length) {
			ok_ = false;
			return nullptr;
		}
		const auto *span = data_ + pos_;
		pos_ += length;
		return span;
	}

	[[nodiscard]] bool ok () const { return ok_; }
	[[nodiscard]] bool done () const { return ok_ && pos_ == size_; }

private:
	const uint8_t *data_ = nullptr;
	size_t size_ = 0;
	size_t pos_ = 0;
	bool ok_ = true;
};


bool fail ( std::string *error, std::string_view message )
{
	if (error)
		*error = message;
	return false;
}


void write_grid ( BlobWriter &writer, const GridLayout &grid )
{
	writer.put( (int32_t)grid.origin_x ); writer.put( (int32_t)grid.origin_y );
	writer.put( (int32_t)grid.tile_w ); writer.put( (int32_t)grid.tile_h );
	writer.put( (int32_t)grid.cols ); writer.put( (int32_t)grid.rows );
	writer.put( (int32_t)grid.clip_x ); writer.put( (int32_t)grid.clip_y );
}


// One bit per tile, set when the tile holds stored bytes.
void write_presence ( BlobWriter &writer, size_t count, auto &&present )
{
	auto byte = uint8_t{ 0 };
	for (size_t i = 0; i < count; ++i) {
		if (present( i ))
			byte |= uint8_t( 1u << ( i % 8 ) );
		if (i % 8 == 7 || i + 1 == count) {
			writer.put( byte );
			byte = 0;
		}
	}
}


void write_level ( BlobWriter &writer, const Level &level )
{
	const auto synthetic = is_remote_synthetic( level );
	auto grid = GridLayout{};
	const auto gridded = synthetic ? ( grid = synthetic_grid( level ), true ) : detect_grid( level, &grid );
	writer.put( level.metric );
	writer.put( (uint8_t)level.pixel_type );
	writer.put( (uint8_t)( level.pixel_type_declared ? 1 : 0 ) );
	writer.put( (int32_t)level.max_pixel_value );
	writer.put_string( level.plane.channel );
	writer.put( level.plane.zlevel );
	writer.put( (int32_t)( synthetic ? grid.tile_w : level.tile_w ) );
	writer.put( (int32_t)( synthetic ? grid.tile_h : level.tile_h ) );
	writer.put_rect( level.bounds );
	writer.put_rect( level.declared_bounds );
	writer.put( (uint8_t)( level.reject_whole_wsi_black ? 1 : 0 ) );
	writer.put( (uint8_t)( synthetic ? 1 : 0 ) );
	writer.put( (uint8_t)level.color_hint );
	writer.put( (uint64_t)( synthetic ? synthetic_slot_bytes( level, grid ) : level.tile_buffer_bytes ) );
	writer.put( (uint8_t)( gridded ? TileListKind::Grid : TileListKind::Explicit ) );
	if (gridded) {
		write_grid( writer, grid );
		write_presence( writer, grid.count(), [&] ( size_t i ) {
			return synthetic || level.tiles[i].fsz > 0;
		} );
		return;
	}
	writer.put( (uint32_t)level.tiles.size() );
	for (const auto &tile : level.tiles) {
		writer.put_rect( tile.lr );
		writer.put( (uint8_t)( tile.fsz > 0 ? 1 : 0 ) );
	}
}


bool read_level ( BlobReader &reader, Level &level )
{
	level.metric = reader.get<float>();
	level.pixel_type = (LevelPixelType)reader.get<uint8_t>();
	level.pixel_type_declared = reader.get<uint8_t>() != 0;
	level.max_pixel_value = reader.get<int32_t>();
	level.plane.channel = reader.get_string();
	level.plane.zlevel = reader.get<float>();
	level.tile_w = reader.get<int32_t>();
	level.tile_h = reader.get<int32_t>();
	const auto bounds = reader.get_rect();
	level.declared_bounds = reader.get_rect();
	level.reject_whole_wsi_black = reader.get<uint8_t>() != 0;
// A synthetic level stays one on the client: its tiles are decoded as they are
// put and owned by the level, outside the caches, as on the server.
	level.synthetic = reader.get<uint8_t>() != 0;
	level.color_hint = (ColorHint)reader.get<uint8_t>();
	level.tile_buffer_bytes = (size_t)reader.get<uint64_t>();
	const auto kind = (TileListKind)reader.get<uint8_t>();
	if (!reader.ok() || (uint8_t)level.pixel_type > (uint8_t)LevelPixelType::Rgb16 ||
		(uint8_t)level.color_hint > (uint8_t)ColorHint::Rgb || level.tile_w < 0 || level.tile_h < 0)
		return false;
	const auto add_tile = [&] ( const Rtlbr<int> &rect, bool present ) {
		auto tile = Tile{};
		tile.lr = rect;
// Only zero versus non-zero matters on the client: zero is a sparse tile.
		tile.fsz = present ? 1 : 0;
		level.tiles.push_back( std::move( tile ) );
	};
	if (kind == TileListKind::Grid) {
		auto grid = GridLayout{};
		grid.origin_x = reader.get<int32_t>(); grid.origin_y = reader.get<int32_t>();
		grid.tile_w = reader.get<int32_t>(); grid.tile_h = reader.get<int32_t>();
		grid.cols = reader.get<int32_t>(); grid.rows = reader.get<int32_t>();
		grid.clip_x = reader.get<int32_t>(); grid.clip_y = reader.get<int32_t>();
		if (!reader.ok() || grid.tile_w <= 0 || grid.tile_h <= 0 || grid.cols <= 0 || grid.rows <= 0 ||
			grid.count() > kMaxBlobCount)
			return false;
		const auto *presence = reader.take( ( grid.count() + 7 ) / 8 );
		if (!presence)
			return false;
		level.tiles.reserve( grid.count() );
		for (size_t i = 0; i < grid.count(); ++i)
			add_tile( grid.rect( i ), ( presence[i / 8] >> ( i % 8 ) ) & 1u );
	}
	else if (kind == TileListKind::Explicit) {
		const auto count = reader.get_count();
		level.tiles.reserve( count );
		for (uint32_t i = 0; i < count && reader.ok(); ++i) {
			const auto rect = reader.get_rect();
			add_tile( rect, reader.get<uint8_t>() != 0 );
		}
	}
	else
		return false;
	if (!reader.ok())
		return false;
	set_level_bounds( level );
	level.bounds = bounds;
	return true;
}


// The losslessly encoded form of `image` no larger than `bound`: the first
// lossless codec this build has that fits, else the raw samples themselves.
template <typename pixeltype>
bool encode_lossless_tile ( const Pwhl<pixeltype> &image, size_t bound,
	std::vector<uint8_t> *out, std::string *error )
{
	auto options = TileEncodeOptions{};
	options.quality = kImageLosslessQuality;
	for (const auto codec : { ImgCodec::Htj2k, ImgCodec::Jxl, ImgCodec::Jp2k }) {
		if (!img_codec_can_encode( codec ))
			continue;
		auto ignored = std::string{};
		if (encode_tile( codec, image, out, options, &ignored ) && !out->empty() && out->size() <= bound)
			return true;
	}
	const auto layout = raw_sample_layout_of<pixeltype>();
	const auto bytes = (size_t)kRawTileHeaderBytes + (size_t)image.w * (size_t)image.h * sizeof( pixeltype );
	if (bytes > bound)
		return fail( error, "Synthetic tile exceeds its slot" );
	out->assign( bytes, 0 );
	write_raw_tile_header( out->data(), layout, image.w, image.h );
	auto *payload = out->data() + kRawTileHeaderBytes;
	for (auto y = 0; y < image.h; ++y)
		std::memcpy( payload + (size_t)y * image.w * sizeof( pixeltype ), image.rowp( y ),
			(size_t)image.w * sizeof( pixeltype ) );
	return true;
}


bool read_synthetic_tile ( const Ppyr &pyramid, const Level &level, int tile_no,
	std::vector<uint8_t> *bytes, std::string *error )
{
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	if (!is_remote_synthetic( level ))
		return fail( error, "Synthetic level holds no raster" );
	const auto grid = synthetic_grid( level );
	if (tile_no < 0 || (size_t)tile_no >= grid.count())
		return fail( error, "Tile index is out of range" );
	const auto rect = grid.rect( (size_t)tile_no );
	const auto &source = level.tiles.front();
	const auto bound = synthetic_slot_bytes( level, grid );
	return visit_level_pixel_type( level.pixel_type, [&] ( auto pixel ) {
		using pixeltype = decltype( pixel );
		const auto *pix = static_cast<const LevelPix<pixeltype> *>( level.pix.get() );
		if (source.owned_no >= (int)pix->owned.size())
			return fail( error, "Synthetic level raster is missing" );
		const auto &raster = pix->owned[source.owned_no];
		auto cut = Pwhl<pixeltype>( Dim2D<int>{ width( rect ), height( rect ) }, uninit );
		const auto x = rect.tl.x - source.lr.tl.x;
		const auto y = rect.tl.y - source.lr.tl.y;
		for (auto row = 0; row < cut.h; ++row)
			std::copy_n( raster.rowp( y + row ) + x, cut.w, cut.rowp( row ) );
		return encode_lossless_tile( cut, bound, bytes, error );
	} );
}

} // namespace


bool serialize_remote_geometry ( const Ppyr &pyramid, std::vector<uint8_t> *out,
	std::string *error )
{
	if (!out)
		return fail( error, "Geometry output is null" );
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	out->clear();
	auto writer = BlobWriter( *out );
	for (auto byte : kGeometryMagic)
		writer.put( byte );
	writer.put( kRemoteGeometryVersion );
	writer.put( pyramid.mrct.tl.x ); writer.put( pyramid.mrct.tl.y );
	writer.put( pyramid.mrct.br.x ); writer.put( pyramid.mrct.br.y );
	writer.put( pyramid.source_pixel_metric.x );
	writer.put( pyramid.source_pixel_metric.y );
	writer.put( (uint32_t)pyramid.properties.size() );
	for (const auto &property : pyramid.properties) {
		writer.put_string( property.key );
		writer.put_string( property.value );
	}
	writer.put( (uint32_t)pyramid.levels.size() );
	for (const auto &level : pyramid.levels) {
		if (level.synthetic && !is_remote_synthetic( level ))
			return fail( error, "A synthetic level has no raster to offer" );
		write_level( writer, level );
	}
	return true;
}


bool read_remote_tile ( WsiLoader &loader, Ppyr &pyramid, int level_no,
	int tile_no, std::vector<uint8_t> *bytes, std::string *error )
{
	if (!bytes || level_no < 0 || level_no >= (int)pyramid.levels.size())
		return fail( error, "Invalid remote tile request" );
	bytes->clear();
	const auto &level = pyramid.levels[level_no];
	if (level.synthetic)
		return read_synthetic_tile( pyramid, level, tile_no, bytes, error );
	return loader.ReadEncodedTile( pyramid, level_no, tile_no, bytes, error );
}


void RemoteLoader::Close ( Ppyr &pyramid )
{
	pyramid.levels.clear();
	pyramid.planes.clear();
	pyramid.tbf = {};
	pyramid.pxb = {};
	auto guard = std::scoped_lock( missing_mtx_ );
	missing_.clear();
}


bool RemoteLoader::LoadTile ( Ppyr &pyramid, int level_no, int tile_no, unsigned )
{
	if (level_no < 0 || level_no >= (int)pyramid.levels.size() ||
		tile_no < 0 || tile_no >= (int)pyramid.levels[level_no].tiles.size())
		return false;
	auto &tile = pyramid.levels[level_no].tiles[tile_no];
	if (tile.fsz == 0) {
		tile.sts.store( TileSts::Ready, std::memory_order_release );
		return true;
	}
	auto guard = std::scoped_lock( missing_mtx_ );
	missing_.push_back( { level_no, tile_no } );
	return false;
}


bool RemoteLoader::OpenRemote ( const uint8_t *data, size_t size, Ppyr &pyramid,
	std::string *error )
{
	auto reader = BlobReader( data, size );
	auto magic = std::array<uint8_t, 4>{};
	for (auto &byte : magic)
		byte = reader.get<uint8_t>();
	if (!reader.ok() || magic != kGeometryMagic)
		return fail( error, "Not a PyrA geometry blob" );
	if (reader.get<uint32_t>() != kRemoteGeometryVersion)
		return fail( error, "Unsupported PyrA geometry version" );
	pyramid.levels.clear();
	pyramid.planes.clear();
	pyramid.properties.clear();
	pyramid.mrct.tl.x = reader.get<float>(); pyramid.mrct.tl.y = reader.get<float>();
	pyramid.mrct.br.x = reader.get<float>(); pyramid.mrct.br.y = reader.get<float>();
	pyramid.source_pixel_metric.x = reader.get<float>();
	pyramid.source_pixel_metric.y = reader.get<float>();
	const auto properties = reader.get_count();
	for (uint32_t i = 0; i < properties && reader.ok(); ++i) {
		auto key = reader.get_string();
		auto value = reader.get_string();
		pyramid.properties.push_back( { std::move( key ), std::move( value ) } );
	}
	const auto levels = reader.get_count();
	pyramid.levels.reserve( levels );
	for (uint32_t i = 0; i < levels && reader.ok(); ++i) {
		pyramid.levels.emplace_back();
		if (!read_level( reader, pyramid.levels.back() ))
			return fail( error, "Malformed level in PyrA geometry blob" );
	}
	if (!reader.done() || pyramid.levels.empty())
		return fail( error, "Malformed PyrA geometry blob" );
	for (auto &level : pyramid.levels)
		visit_level_pixel_type( level.pixel_type, [&] ( auto pixel ) {
			(void)level_pix<decltype( pixel )>( level );
		} );
	rebuild_plane_index( pyramid );
	pyramid.tbf = {};
	pyramid.pxb = {};
	prepare_cache_pools( pyramid );
	pyramid.deferred_tiles = true;
	return true;
}


bool RemoteLoader::PutTile ( Ppyr &pyramid, int level_no, int tile_no,
	const uint8_t *data, size_t size, std::string *error )
{
	const auto operation = std::scoped_lock( *pyramid.render_mutex_ );
	if (level_no < 0 || level_no >= (int)pyramid.levels.size() ||
		tile_no < 0 || tile_no >= (int)pyramid.levels[level_no].tiles.size())
		return fail( error, "Tile index is out of range" );
	const auto &level = pyramid.levels[level_no];
	if (level.tiles[tile_no].fsz == 0)
		return true;
	if (!data || size == 0 || size > level.tile_buffer_bytes)
		return fail( error, "Tile bytes are empty or exceed the level's tile slot" );
	if (level.synthetic)
		return PutSyntheticTile( pyramid, level_no, tile_no, data, size, error );
	if (!LoadCompressedTile( pyramid, level_no, tile_no, 0,
		[data, size] ( const Level &, const Tile &, uint8_t *dest, size_t &pos, unsigned ) {
			std::memcpy( dest, data, size );
			pos = size;
			return true;
		} ))
		return fail( error, "No compressed-cache slot is available for the tile" );
	return true;
}


// A synthetic level's tile is decoded now and owned by the level, as the server
// owns its raster: it never takes a cache slot, so a coarse view needs no more
// cache on the client than on the server.
bool RemoteLoader::PutSyntheticTile ( Ppyr &pyramid, int level_no, int tile_no,
	const uint8_t *data, size_t size, std::string *error )
{
	auto &level = pyramid.levels[level_no];
	auto &tile = level.tiles[tile_no];
	if (tile.sts.load( std::memory_order_acquire ) == TileSts::Ready)
		return true;
	return visit_level_pixel_type( level.pixel_type, [&] ( auto pixel ) {
		using pixeltype = decltype( pixel );
		auto image = Pwhl<pixeltype>{};
		if (!decode_to_image( data, size, image ) || image.w != width( tile.lr ) || image.h != height( tile.lr ))
			return fail( error, "Synthetic tile does not decode to its rectangle" );
		auto &pix = level_pix<pixeltype>( level );
		tile.owned_no = (int)pix.owned.size();
		tile.pbd_w = image.w;
		tile.pbd_h = image.h;
		tile.pbd_l = image.l;
		pix.owned.push_back( std::move( image ) );
		tile.sts.store( TileSts::Ready, std::memory_order_release );
		return true;
	} );
}


std::vector<RemoteTileId> RemoteLoader::TakeMissing ( const Ppyr &pyramid, size_t limit )
{
	auto guard = std::scoped_lock( missing_mtx_ );
	std::sort( missing_.begin(), missing_.end() );
	missing_.erase( std::unique( missing_.begin(), missing_.end() ), missing_.end() );
	std::erase_if( missing_, [&pyramid] ( const RemoteTileId &id ) {
		return id.level_no >= (int)pyramid.levels.size() ||
			id.tile_no >= (int)pyramid.levels[id.level_no].tiles.size() ||
			pyramid.levels[id.level_no].tiles[id.tile_no].sts.load( std::memory_order_acquire ) != TileSts::Empty;
	} );
	const auto count = std::min( limit, missing_.size() );
	auto taken = std::vector<RemoteTileId>( missing_.begin(), missing_.begin() + (ptrdiff_t)count );
	missing_.erase( missing_.begin(), missing_.begin() + (ptrdiff_t)count );
	return taken;
}

} // namespace priml
