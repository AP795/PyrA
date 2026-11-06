#include "gallery.h"

#include "ptf_tags.h"
#include "wsi_formats.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace priml {

bool gallery_plane_is_acquired ( const Ppyr &pyramid,
	const PlaneStack &plane, const Rtlbr<float> &region )
{
	if (!find_property( pyramid, "gallery.members" ))
		return true;
	if (plane.ladder.empty())
		return false;
	const auto &level = pyramid.levels[plane.ladder.front()];
	const auto request = bbox_from_metric( region, level.metric );
	if (request.is_empty())
		return false;
	const auto [first, last] = bracket_tiles_by_top( level, request );
	return std::any_of( level.tiles.begin() + first, level.tiles.begin() + last, [&] ( const auto &tile ) {
		return tile.fsz > 0 && !IntersectRect( region,
			subrect_metric_from_bbox( tile.lr, level.metric ) ).is_empty();
	} );
}


namespace {

bool set_error_local ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	return false;
}


std::string utf8_of_local ( const std::filesystem::path &path )
{
	const auto u8 = path.generic_u8string();
	return std::string( reinterpret_cast<const char *>( u8.data() ), u8.size() );
}


std::filesystem::path path_of_utf8_local ( std::string_view text )
{
	return std::filesystem::path( std::u8string(
		reinterpret_cast<const char8_t *>( text.data() ), text.size() ) );
}


bool has_line_break_or_tab_local ( std::string_view text )
{
	return text.find_first_of( "\t\r\n" ) != std::string_view::npos;
}


template <typename T>
bool parse_number_local ( std::string_view text, T *out )
{
	text = ptf_detail::trim( text );
	if (text.empty())
		return false;
	auto value = T{};
	const auto *last = text.data() + text.size();
	auto result = std::from_chars( text.data(), last, value );
	if (result.ec != std::errc{} || result.ptr != last)
		return false;
	*out = value;
	return true;
}


std::vector<std::string_view> split_tabs_local ( std::string_view text )
{
	auto fields = std::vector<std::string_view>{};
	while (true) {
		auto tab = text.find( '\t' );
		fields.push_back( text.substr( 0, tab ) );
		if (tab == std::string_view::npos)
			break;
		text.remove_prefix( tab + 1 );
	}
	return fields;
}


std::string format_double_local ( double value )
{
	char buffer[64];
	auto result = std::to_chars( buffer, buffer + sizeof( buffer ), value );
	return std::string( buffer, result.ptr );
}


bool parse_member_local ( std::string_view value, GalleryMember *out,
	std::string *error )
{
	constexpr auto kMemberFields = std::size_t{ 10 };
	auto fields = split_tabs_local( value );
	if (fields.size() != kMemberFields)
		return set_error_local( error, "member line has " +
			std::to_string( fields.size() ) + " fields, expected " +
			std::to_string( kMemberFields ) );

	auto member = GalleryMember{};
	auto x = 0;
	auto y = 0;
	auto w = 0;
	auto h = 0;
	if (!parse_number_local( fields[0], &x ) || !parse_number_local( fields[1], &y ) ||
		!parse_number_local( fields[2], &w ) || !parse_number_local( fields[3], &h ))
	{
		return set_error_local( error, "member line has an unreadable rectangle" );
	}
	member.rect = { { x, y }, { x + w, y + h } };
	if (!parse_number_local( fields[4], &member.file_bytes ) ||
		!parse_number_local( fields[5], &member.file_mtime ))
	{
		return set_error_local( error, "member line has an unreadable size or mtime" );
	}
	if (ptf_detail::trim( fields[6] ) != "-" || ptf_detail::trim( fields[7] ) != "-") {
		auto stage = Pxy<double>{};
		if (!parse_number_local( fields[6], &stage.x ) ||
			!parse_number_local( fields[7], &stage.y ))
		{
			return set_error_local( error, "member line has an unreadable stage origin" );
		}
		member.stage_um = stage;
	}
	member.zone.assign( fields[8] );
	member.file.assign( fields[9] );
	*out = std::move( member );
	return true;
}


// "thumbnail=" fields: the member it belongs to, then the thumbnail itself.
bool parse_thumbnail_local ( std::string_view value, std::string *member_file,
	GalleryThumbnail *out, std::string *error )
{
	constexpr auto kThumbnailFields = std::size_t{ 7 };
	auto fields = split_tabs_local( value );
	if (fields.size() != kThumbnailFields)
		return set_error_local( error, "thumbnail line has " +
			std::to_string( fields.size() ) + " fields, expected " +
			std::to_string( kThumbnailFields ) );
	auto thumbnail = GalleryThumbnail{};
	if (!parse_number_local( fields[1], &thumbnail.page ) ||
		!parse_number_local( fields[2], &thumbnail.size.w ) ||
		!parse_number_local( fields[3], &thumbnail.size.h ) ||
		!parse_number_local( fields[4], &thumbnail.file_bytes ) ||
		!parse_number_local( fields[5], &thumbnail.file_mtime ))
	{
		return set_error_local( error, "thumbnail line has an unreadable number" );
	}
	member_file->assign( fields[0] );
	thumbnail.file.assign( fields[6] );
	*out = std::move( thumbnail );
	return true;
}

} // namespace


bool is_gallery_path ( const std::filesystem::path &path )
{
	return lower_ascii_copy( path.extension().string() ) == kGalleryExtension;
}


bool gallery_pixel_type_supported ( LevelPixelType type ) noexcept
{
	return is_integer_gray( type ) || is_integer_rgb( type );
}


std::vector<GalleryZone> gallery_zones ( const GalleryManifest &manifest )
{
	auto zones = std::vector<GalleryZone>{};
	auto seen = std::set<std::string>{};
	for (const auto &member : manifest.members) {
		if (seen.insert( member.zone ).second)
			zones.push_back( { member.zone, member.rect } );
	}
	return zones;
}


std::vector<std::string> gallery_channels ( const GalleryManifest &manifest )
{
	auto channels = std::vector<std::string>{};
	auto seen = std::set<std::string>{};
	for (const auto &member : manifest.members) {
		if (seen.insert( member.channel ).second)
			channels.push_back( member.channel );
	}
	return channels;
}


Rtlbr<int> gallery_extent ( const GalleryManifest &manifest )
{
	if (manifest.members.empty())
		return { { 0, 0 }, { 0, 0 } };
	auto extent = manifest.members.front().rect;
	for (const auto &member : manifest.members)
		extent = UnionRect( extent, member.rect );
	return extent;
}


std::optional<std::int64_t> gallery_file_mtime ( const std::filesystem::path &path )
{
	auto ec = std::error_code{};
	const auto written = std::filesystem::last_write_time( path, ec );
	if (ec)
		return std::nullopt;
	const auto system = std::chrono::clock_cast<std::chrono::system_clock>( written );
	return std::chrono::duration_cast<std::chrono::seconds>(
		system.time_since_epoch() ).count();
}


bool validate_gallery_manifest ( const GalleryManifest &manifest,
	std::string *error )
{
	if (!gallery_pixel_type_supported( manifest.pixel_type ))
		return set_error_local( error, std::string( "unsupported pixel type " ) +
			pixel_type_name( manifest.pixel_type ) );
	if (has_line_break_or_tab_local( manifest.mpp_source ))
		return set_error_local( error, "header value contains a tab or line break" );
	if (!( manifest.mpp > 0.0 ))
		return set_error_local( error, "microns per pixel must be positive" );
	if (manifest.members.empty())
		return set_error_local( error, "no members" );
	const auto channels = gallery_channels( manifest );
	for (const auto &[channel, scale] : manifest.channel_max_pixel_values) {
		if (std::find( channels.begin(), channels.end(), channel ) == channels.end() ||
			scale <= 0 || scale > ( manifest.pixel_type == LevelPixelType::Gray8 ||
				manifest.pixel_type == LevelPixelType::Rgb8
				? std::numeric_limits<uint8_t>::max() : std::numeric_limits<uint16_t>::max() ))
		{
			return set_error_local( error, "unusable range for channel '" + channel + "'" );
		}
	}

	auto files = std::set<std::string>{};
	auto zone_channels = std::set<std::pair<std::string, std::string>>{};
	auto zone_rects = std::map<std::string, Rtlbr<int>>{};
	for (const auto &member : manifest.members) {
		if (member.file.empty() || member.zone.empty())
			return set_error_local( error, "member with an empty file or zone name" );
		if (is_integer_gray( manifest.pixel_type ) && member.channel.empty())
			return set_error_local( error, "grey member '" + member.file +
				"' needs a channel name" );
		if (is_integer_rgb( manifest.pixel_type ) && !member.channel.empty())
			return set_error_local( error, "RGB member '" + member.file +
				"' has a channel name" );
		if (has_line_break_or_tab_local( member.file ) ||
			has_line_break_or_tab_local( member.zone ) ||
			has_line_break_or_tab_local( member.channel ))
		{
			return set_error_local( error, "member '" + member.file +
				"' has a tab or line break in its file, zone or channel name" );
		}
		if (member.rect.is_empty() || member.rect.tl.x < 0 || member.rect.tl.y < 0)
			return set_error_local( error, "member '" + member.file +
				"' has an empty or negative rectangle" );
		if (!files.insert( member.file ).second)
			return set_error_local( error, "member file '" + member.file +
				"' is listed twice" );
		if (!zone_channels.emplace( member.zone, member.channel ).second)
			return set_error_local( error, "zone '" + member.zone +
				"' has channel '" + member.channel + "' twice" );
		const auto [known, added] = zone_rects.try_emplace( member.zone, member.rect );
		if (!added && ( known->second.tl.x != member.rect.tl.x ||
			known->second.tl.y != member.rect.tl.y ||
			known->second.br.x != member.rect.br.x ||
			known->second.br.y != member.rect.br.y ))
		{
			return set_error_local( error, "the members of zone '" + member.zone +
				"' differ in position or size" );
		}
		if (member.thumbnail) {
			const auto &thumbnail = *member.thumbnail;
			if (thumbnail.file.empty() || has_line_break_or_tab_local( thumbnail.file ) ||
				thumbnail.page < 0 || !thumbnail.size.is_set())
			{
				return set_error_local( error, "member '" + member.file +
					"' has an unusable thumbnail" );
			}
		}
	}
	const auto zones = gallery_zones( manifest );
	for (auto i = std::size_t{ 0 }; i < zones.size(); ++i) {
		for (auto j = i + 1; j < zones.size(); ++j) {
			if (overlaps( zones[i].rect, zones[j].rect ))
				return set_error_local( error, "zones '" + zones[i].name +
					"' and '" + zones[j].name + "' overlap" );
		}
	}
//
// The coarse levels pyr synthesises are decimated from the level's extent with
// their origin at (0,0); an extent starting elsewhere would shift them against
// the base level.
	const auto extent = gallery_extent( manifest );
	if (extent.tl.x != 0 || extent.tl.y != 0)
		return set_error_local( error, "the members do not start at the mosaic origin" );
	return true;
}


bool write_gallery_manifest ( const std::filesystem::path &path,
	const GalleryManifest &manifest, std::string *error )
{
	if (!validate_gallery_manifest( manifest, error ))
		return false;

	auto text = std::ostringstream{};
	text << kGalleryMarker << '\n';
	text << "version=" << kGalleryVersion << '\n';
	text << "pixel-type=" << ptf_pixel_type_name( manifest.pixel_type ) << '\n';
	text << "max-pixel-value=" << manifest.max_pixel_value << '\n';
	text << "mpp=" << format_double_local( manifest.mpp ) << '\n';
	text << "mpp-source=" << manifest.mpp_source << '\n';
	for (const auto &name : gallery_channels( manifest )) {
		if (const auto scale = manifest.channel_max_pixel_values.find( name );
			scale != manifest.channel_max_pixel_values.end())
		{
			text << "channel-range=" << name << '\t' << scale->second << '\n';
		}
	}
	auto channel = std::optional<std::string>{};
	for (const auto &member : manifest.members) {
		if (!channel || *channel != member.channel) {
			channel = member.channel;
			text << "channel=" << *channel << '\n';
		}
		text << "member=" << member.rect.tl.x << '\t' << member.rect.tl.y << '\t'
			<< width( member.rect ) << '\t' << height( member.rect ) << '\t'
			<< member.file_bytes << '\t' << member.file_mtime << '\t';
		if (member.stage_um)
			text << format_double_local( member.stage_um->x ) << '\t'
				<< format_double_local( member.stage_um->y ) << '\t';
		else
			text << "-\t-\t";
		text << member.zone << '\t' << member.file << '\n';
	}
	for (const auto &member : manifest.members) {
		if (!member.thumbnail)
			continue;
		const auto &thumbnail = *member.thumbnail;
		text << "thumbnail=" << member.file << '\t' << thumbnail.page << '\t'
			<< thumbnail.size.w << '\t' << thumbnail.size.h << '\t'
			<< thumbnail.file_bytes << '\t' << thumbnail.file_mtime << '\t'
			<< thumbnail.file << '\n';
	}

//
// Written aside and renamed into place, so a failed write never leaves a
// truncated manifest where a slide used to be.
	auto temp = path;
	temp += ".tmp";
	{
		auto out = std::ofstream( temp, std::ios::binary | std::ios::trunc );
		if (!out)
			return set_error_local( error, "cannot create '" + utf8_of_local( temp ) + "'" );
		const auto bytes = text.str();
		out.write( bytes.data(), (std::streamsize)bytes.size() );
		out.close();
		if (!out)
			return set_error_local( error, "cannot write '" + utf8_of_local( temp ) + "'" );
	}
	auto ec = std::error_code{};
	std::filesystem::rename( temp, path, ec );
	if (ec) {
		std::filesystem::remove( temp, ec );
		return set_error_local( error, "cannot replace '" + utf8_of_local( path ) + "'" );
	}
	return true;
}


bool read_gallery_manifest ( const std::filesystem::path &path,
	GalleryManifest *out, std::string *error )
{
	if (!out)
		return set_error_local( error, "manifest output is null" );
	auto in = std::ifstream( path, std::ios::binary );
	if (!in)
		return set_error_local( error, "cannot open '" + utf8_of_local( path ) + "'" );

	auto manifest = GalleryManifest{};
	auto channel = std::string{};
	auto thumbnails = std::vector<std::pair<std::string, GalleryThumbnail>>{};
	auto saw_marker = false;
	auto line_no = 0;
	auto line = std::string{};
	while (std::getline( in, line )) {
		++line_no;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto view = std::string_view( line );
		if (!saw_marker) {
//
// A UTF-8 byte-order mark is tolerated on the marker line: an editor may add one.
			if (view.starts_with( "\xEF\xBB\xBF" ))
				view.remove_prefix( 3 );
			if (ptf_detail::trim( view ) != kGalleryMarker)
				return set_error_local( error, "'" + utf8_of_local( path ) +
					"' is not a gallery manifest" );
			saw_marker = true;
			continue;
		}
		if (ptf_detail::trim( view ).empty() || view.front() == '#')
			continue;
		const auto eq = view.find( '=' );
		if (eq == std::string_view::npos)
			return set_error_local( error, "line " + std::to_string( line_no ) +
				" has no '='" );
		const auto key = ptf_detail::trim( view.substr( 0, eq ) );
		const auto value = view.substr( eq + 1 );

		auto bad = false;
		if (key == "version") {
			bad = !parse_number_local( value, &manifest.version );
			if (!bad && manifest.version > kGalleryVersion)
				return set_error_local( error, "manifest version " +
					std::to_string( manifest.version ) + " is newer than this reader" );
		}
		else if (key == "pixel-type")
			bad = !ptf_pixel_type_from_name( ptf_detail::trim( value ),
				&manifest.pixel_type );
		else if (key == "channel")
			channel.assign( ptf_detail::trim( value ) );
		else if (key == "max-pixel-value")
			bad = !parse_number_local( value, &manifest.max_pixel_value );
		else if (key == "channel-range") {
			const auto tab = value.find( '\t' );
			auto scale = 0;
			bad = tab == std::string_view::npos ||
				!parse_number_local( value.substr( tab + 1 ), &scale );
			if (!bad)
				bad = !manifest.channel_max_pixel_values.emplace(
					std::string( value.substr( 0, tab ) ), scale ).second;
		}
		else if (key == "mpp")
			bad = !parse_number_local( value, &manifest.mpp );
		else if (key == "mpp-source")
			manifest.mpp_source.assign( ptf_detail::trim( value ) );
		else if (key == "member") {
			auto member = GalleryMember{};
			auto member_error = std::string{};
			if (!parse_member_local( value, &member, &member_error ))
				return set_error_local( error, "line " + std::to_string( line_no ) +
					": " + member_error );
			member.channel = channel;
			manifest.members.push_back( std::move( member ) );
		}
		else if (key == "thumbnail") {
			auto member_file = std::string{};
			auto thumbnail = GalleryThumbnail{};
			auto thumbnail_error = std::string{};
			if (!parse_thumbnail_local( value, &member_file, &thumbnail, &thumbnail_error ))
				return set_error_local( error, "line " + std::to_string( line_no ) +
					": " + thumbnail_error );
			thumbnails.emplace_back( std::move( member_file ), std::move( thumbnail ) );
		}
//
// Unknown keys are ignored, so adding one is not a format change.
		if (bad)
			return set_error_local( error, "line " + std::to_string( line_no ) +
				" has an unreadable '" + std::string( key ) + "'" );
	}
	if (!saw_marker)
		return set_error_local( error, "'" + utf8_of_local( path ) + "' is empty" );
	for (auto &[member_file, thumbnail] : thumbnails) {
		auto member = std::find_if( manifest.members.begin(), manifest.members.end(),
			[&] ( const GalleryMember &m ) { return m.file == member_file; } );
		if (member == manifest.members.end())
			return set_error_local( error, "thumbnail for unknown member '" +
				member_file + "'" );
		member->thumbnail = std::move( thumbnail );
	}
	if (!validate_gallery_manifest( manifest, error ))
		return false;
	*out = std::move( manifest );
	return true;
}


std::optional<double> gallery_thumbnail_scale ( const GalleryManifest &manifest,
	std::string *why )
{
	auto refuse = [why] ( const std::string &reason ) -> std::optional<double> {
		if (why)
			*why = reason;
		return std::nullopt;
	};
	if (manifest.members.empty())
		return refuse( "no members" );
	auto scale = 0.0;
	for (const auto &member : manifest.members) {
		if (!member.thumbnail)
			return refuse( "'" + member.file + "' has no thumbnail" );
		const auto &size = member.thumbnail->size;
		const auto sx = (double)width( member.rect ) / size.w;
		const auto sy = (double)height( member.rect ) / size.h;
		if (std::abs( sx - sy ) > kGalleryThumbnailScaleTolerance * sx)
			return refuse( "'" + member.file + "' thumbnail is not to scale on both axes" );
		if (scale == 0.0)
			scale = sx;
		else if (std::abs( sx - scale ) > kGalleryThumbnailScaleTolerance * scale)
			return refuse( "'" + member.file + "' thumbnail scale " + format_double_local( sx ) +
				" differs from " + format_double_local( scale ) );
	}
	if (scale < kGalleryThumbnailMinScale)
		return refuse( "thumbnails are barely smaller than their members" );
	return scale;
}


std::string gallery_thumbnail_stale_reason ( const std::filesystem::path &directory,
	const GalleryMember &member )
{
	if (!member.thumbnail || member.thumbnail->file == member.file)
		return {};
	auto as_member = GalleryMember{};
	as_member.file = member.thumbnail->file;
	as_member.file_bytes = member.thumbnail->file_bytes;
	as_member.file_mtime = member.thumbnail->file_mtime;
	return gallery_member_stale_reason( directory, as_member );
}


std::string gallery_member_stale_reason ( const std::filesystem::path &directory,
	const GalleryMember &member )
{
	const auto path = directory / path_of_utf8_local( member.file );
	auto ec = std::error_code{};
	const auto bytes = std::filesystem::file_size( path, ec );
	if (ec)
		return "missing";
	if (bytes != member.file_bytes)
		return "size changed";
	const auto mtime = gallery_file_mtime( path );
	if (!mtime || *mtime != member.file_mtime)
		return "modified";
	return {};
}


} // namespace priml
