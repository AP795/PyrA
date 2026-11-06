#include "wsi.h"

#include "gallery.h"
#include "img_codec.h"
#include "img_tiff.h"
#include "pyr_rt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace priml {

// Stale members named in the refusal before it is cut short; the rest are counted.
inline constexpr int kGalleryStaleMembersNamed = 5;


struct GalleryState {
	GalleryManifest manifest;
	std::filesystem::path directory;
//
// Metric of the thumbnail level, 0 when the gallery has none. LoadTile tells a
// thumbnail tile from a member tile by its level's metric: level indices move
// when OpenFile filters and regroups levels, metrics do not.
	float thumbnail_metric = 0.0f;
};


namespace {

std::filesystem::path relative_path_local ( const GalleryState &state,
	const std::string &file )
{
	return state.directory / std::filesystem::path( std::u8string(
		reinterpret_cast<const char8_t *>( file.data() ), file.size() ) );
}


std::filesystem::path member_path_local ( const GalleryState &state,
	const GalleryMember &member )
{
	return relative_path_local( state, member.file );
}


// The members of `channel`, by zone. A zone missing here is one this channel did
// not acquire.
std::unordered_map<std::string, size_t> channel_members_local (
	const GalleryManifest &manifest, const std::string &channel )
{
	auto members = std::unordered_map<std::string, size_t>{};
	for (auto i = size_t{ 0 }; i < manifest.members.size(); ++i) {
		if (manifest.members[i].channel == channel)
			members.emplace( manifest.members[i].zone, i );
	}
	return members;
}


// A level's identity: everything but its tiles and its metric.
Level empty_level_local ( const GalleryManifest &manifest, const std::string &channel )
{
	auto level = Level{};
	level.pixel_type = manifest.pixel_type;
	level.pixel_type_declared = true;
	level.max_pixel_value = manifest.max_pixel_value;
	if (const auto scale = manifest.channel_max_pixel_values.find( channel );
		scale != manifest.channel_max_pixel_values.end())
	{
		level.max_pixel_value = scale->second;
	}
	level.plane = PlaneSel{ channel, 0.0f };
	return level;
}


// Tiles a channel does not have stay in the table with fsz 0, which pyr reads as a
// legitimately empty region: a request over such a zone then renders blank for this
// plane instead of selecting no tile and coming back empty, and every channel's
// level keeps the same extent.
void finish_level_local ( Level *level, size_t max_fsz )
{
	set_level_bounds( *level );
	level->tbf.init( (int)max_fsz, (int)level->tiles.size() );
}


// The full-size level of `channel`. False when a member file is too large to
// hold in one tile buffer.
bool member_level_local ( const GalleryManifest &manifest, const std::string &channel,
	const std::vector<GalleryZone> &zones, Level *out, size_t *largest )
{
	auto level = empty_level_local( manifest, channel );
	level.metric = (float)manifest.mpp;
	const auto members = channel_members_local( manifest, channel );
	auto max_fsz = size_t{ 0 };
	for (const auto &zone : zones) {
		auto tile = Tile{};
		tile.lr = zone.rect;
		if (auto member = members.find( zone.name ); member != members.end()) {
//
// foff is the member index. set_level_bounds reorders the tiles, and each one
// carries its own index through that.
			tile.foff = member->second;
//
// The ENCODED size: the slot holds the member's file as it is on disk, and
// the decode pool turns it into pixels (img_tiff.h).
			tile.fsz = (size_t)manifest.members[member->second].file_bytes;
			max_fsz = std::max( max_fsz, tile.fsz );
			level.tile_w = std::max( level.tile_w, width( zone.rect ) );
			level.tile_h = std::max( level.tile_h, height( zone.rect ) );
		}
		level.tiles.push_back( std::move( tile ) );
	}
	*largest = max_fsz;
	if (max_fsz == 0 || max_fsz > (size_t)std::numeric_limits<int>::max())
		return false;
	finish_level_local( &level, max_fsz );
	*out = std::move( level );
	return true;
}


// The common scale of the members' stored thumbnails, when every member of every
// channel has a usable one (gallery_thumbnail_scale) and none changed on disk.
// Otherwise nullopt, and the log says why: the overview is then derived from the
// members themselves.
std::optional<double> usable_thumbnail_scale_local ( const GalleryState &state,
	const std::string &filename )
{
	auto why = std::string{};
	const auto scale = gallery_thumbnail_scale( state.manifest, &why );
	if (!scale) {
		pyr::plog( "wsi gallery: %s: overview derived from the members - %s\n",
			filename.c_str(), why.c_str() );
		return std::nullopt;
	}
	for (const auto &member : state.manifest.members) {
		auto reason = gallery_thumbnail_stale_reason( state.directory, member );
		if (!reason.empty()) {
			pyr::plog( "wsi gallery: %s: thumbnails ignored, '%s' %s\n", filename.c_str(),
				member.thumbnail->file.c_str(), reason.c_str() );
			return std::nullopt;
		}
	}
	return scale;
}


// The thumbnail level of `channel`: each tile one thumbnail, at its zone's
// rectangle divided by `scale`.
Level thumbnail_level_local ( const GalleryManifest &manifest, const std::string &channel,
	const std::vector<GalleryZone> &zones, double scale )
{
	auto level = empty_level_local( manifest, channel );
	level.metric = (float)( manifest.mpp * scale );
	const auto members = channel_members_local( manifest, channel );
	auto max_fsz = size_t{ 0 };
	for (const auto &zone : zones) {
		auto tile = Tile{};
		const auto x = (int)std::lround( zone.rect.tl.x / scale );
		const auto y = (int)std::lround( zone.rect.tl.y / scale );
		auto size = Dim2D<int>{ (int)std::lround( width( zone.rect ) / scale ),
			(int)std::lround( height( zone.rect ) / scale ) };
		if (auto member = members.find( zone.name ); member != members.end()) {
			size = manifest.members[member->second].thumbnail->size;
			tile.foff = member->second;
//
// Decoded size: a thumbnail is decoded by the loader, a page at a time, and
// handed on as raw samples (see LoadTile).
			tile.fsz = (size_t)kRawTileHeaderBytes +
				(size_t)size.w * (size_t)size.h * pixel_size_bytes( manifest.pixel_type );
			max_fsz = std::max( max_fsz, tile.fsz );
			level.tile_w = std::max( level.tile_w, size.w );
			level.tile_h = std::max( level.tile_h, size.h );
		}
		tile.lr = { { x, y }, { x + size.w, y + size.h } };
		level.tiles.push_back( std::move( tile ) );
	}
	finish_level_local( &level, max_fsz );
	return level;
}


// Decode `member`'s thumbnail into a raw-sample payload at `dest`.
bool load_thumbnail_local ( const GalleryState &state, const GalleryMember &member,
	LevelPixelType type, uint8_t *dest, size_t capacity, size_t *pos )
{
	const auto &thumbnail = *member.thumbnail;
	const auto path = relative_path_local( state, thumbnail.file );
	return visit_level_pixel_type( type, [&] ( auto pixel ) {
		using pixeltype = decltype( pixel );
		if constexpr (std::is_same_v<pixeltype, float>) {
			return false;
		} else {
			auto size = Dim2D<int>{ 0, 0 };
			auto dst = TileDst<pixeltype>{};
			dst.reserve = [&] ( int w, int h, int &stride ) -> pixeltype * {
				const auto bytes = (size_t)w * (size_t)h * sizeof( pixeltype );
				if (bytes + (size_t)kRawTileHeaderBytes > capacity)
					return nullptr;
				size = { w, h };
				stride = w;
				return reinterpret_cast<pixeltype *>( dest + kRawTileHeaderBytes );
			};
			auto error = std::string{};
			if (!tiff_image_read_page( path, thumbnail.page, dst, &error )) {
				pyr::plog_err( "wsi gallery: thumbnail '%s' page %d: %s\n",
					thumbnail.file.c_str(), thumbnail.page, error.c_str() );
				return false;
			}
			write_raw_tile_header( dest, raw_sample_layout_of<pixeltype>(), size.w, size.h );
			*pos = (size_t)kRawTileHeaderBytes +
				(size_t)size.w * (size_t)size.h * sizeof( pixeltype );
			return true;
		}
	} );
}

} // namespace


GalleryLoader::GalleryLoader ()
	: state_( std::make_unique<GalleryState>() )
{
}


GalleryLoader::~GalleryLoader () = default;


bool ProbeGalleryPixelType ( const std::string &filename, LevelPixelType *out )
{
	if (!out)
		return false;
	auto manifest = GalleryManifest{};
	if (!read_gallery_manifest( std::filesystem::path( filename ), &manifest ))
		return false;
	*out = manifest.pixel_type;
	return true;
}


bool GalleryLoader::Open ( Ppyr &pyramid )
{
	const auto manifest_path = std::filesystem::path( this->filename_ );
	auto error = std::string{};
	auto &state = *state_;
	if (!read_gallery_manifest( manifest_path, &state.manifest, &error )) {
		pyr::plog_err( "wsi gallery: %s: %s\n", this->filename_.c_str(), error.c_str() );
		return false;
	}
	state.directory = manifest_path.parent_path();
	const auto &manifest = state.manifest;

//
// Every member must still be the file the manifest recorded. The zones and every
// stored rectangle were derived from this layout, so a changed member is not
// something to render around - the gallery has to be made again.
	auto stale = 0;
	auto named = std::string{};
	for (const auto &member : manifest.members) {
		auto reason = gallery_member_stale_reason( state.directory, member );
		if (reason.empty())
			continue;
		if (stale < kGalleryStaleMembersNamed)
			named += ( named.empty() ? "" : ", " ) + member.file + " (" + reason + ")";
		++stale;
	}
	if (stale > 0) {
		pyr::plog_err( "wsi gallery: %s: %d member(s) changed since the gallery was "
			"made - %s%s; make the gallery again\n", this->filename_.c_str(), stale,
			named.c_str(), stale > kGalleryStaleMembersNamed ? ", ..." : "" );
		return false;
	}

	const auto zones = gallery_zones( manifest );
	const auto channels = gallery_channels( manifest );
	const auto thumbnail_scale = usable_thumbnail_scale_local( state, this->filename_ );
	auto extent = Rtlbr<int>{ { 0, 0 }, { 0, 0 } };
	for (const auto &channel : channels) {
		auto level = Level{};
		auto largest = size_t{ 0 };
		if (!member_level_local( manifest, channel, zones, &level, &largest )) {
			pyr::plog_err( "wsi gallery: %s: member file size %zu out of range\n",
				this->filename_.c_str(), largest );
			return false;
		}
		extent = level.bounds;
		pyramid.levels.push_back( std::move( level ) );
		if (thumbnail_scale) {
			auto thumbnails = thumbnail_level_local( manifest, channel, zones, *thumbnail_scale );
			state.thumbnail_metric = thumbnails.metric;
			pyramid.levels.push_back( std::move( thumbnails ) );
		}
	}
	if (thumbnail_scale) {
		pyr::plog( "wsi gallery: %s: overview from the stored thumbnails, 1/%.3f\n",
			this->filename_.c_str(), *thumbnail_scale );
	}
	pyramid.mrct = { { 0.0f, 0.0f },
		{ (float)( extent.br.x * manifest.mpp ), (float)( extent.br.y * manifest.mpp ) } };
	pyramid.source_pixel_metric = { (float)manifest.mpp, (float)manifest.mpp };
//
// Every render of a member decodes a whole file, so keep decoded members around
// rather than paying that again on each repaint.
	pyramid.prefer_pixbf = true;

	set_property( pyramid, "gallery.members", std::to_string( manifest.members.size() ) );
	set_property( pyramid, "gallery.mpp-source", manifest.mpp_source );
	auto channel_list = std::string{};
	for (const auto &channel : channels)
		channel_list += ( channel_list.empty() ? "" : "," ) + channel;
	if (!channel_list.empty())
		set_property( pyramid, "gallery.channels", channel_list );

	pyr::plog( "wsi gallery: %s: %zu zone(s), %zu member file(s), mosaic %dx%d px, "
		"%.6g um/px, %s%s%s, largest member %dx%d\n", this->filename_.c_str(), zones.size(),
		manifest.members.size(), extent.br.x, extent.br.y, manifest.mpp,
		pixel_type_name( manifest.pixel_type ), channel_list.empty() ? "" : " channels ",
		channel_list.c_str(), pyramid.levels.front().tile_w, pyramid.levels.front().tile_h );
	return true;
}


void GalleryLoader::Close ( Ppyr &pyramid )
{
	state_->manifest = {};
	state_->directory.clear();
	state_->thumbnail_metric = 0.0f;
	this->CloseFile();
	pyramid.levels.clear();
}


bool GalleryLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this] ( auto &level, auto &tile, uint8_t *dest, size_t &pos, unsigned /*worker*/ ) {
			const auto &members = state_->manifest.members;
			if (tile.foff >= members.size())
				return false;
			if (state_->thumbnail_metric > 0.0f && level.metric == state_->thumbnail_metric)
				return load_thumbnail_local( *state_, members[tile.foff], level.pixel_type,
					dest, level.tbf.slot_bytes(), &pos );
//
// Opened per load rather than through the decode pool's cached handles: those
// are kept per path per worker, and hundreds of members would hold hundreds of
// handles open per thread for a read that is one sequential pass.
			const auto path = member_path_local( *state_, members[tile.foff] );
			auto in = std::ifstream( path, std::ios::binary );
			if (!in)
				return false;
			in.read( reinterpret_cast<char *>( dest ), (std::streamsize)tile.fsz );
			if ((size_t)in.gcount() != tile.fsz)
				return false;
			pos = tile.fsz;
			return true;
		} );
}

} // namespace priml
