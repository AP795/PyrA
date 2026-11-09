#include "wsi.h"

#include "gallery.h"
#include "ptf_tags.h"
#include "tiff_inc.h"
#include "wsi_formats.h"
#include "wsi_tif_aux.h"

#include "pyr_rt.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace priml {

namespace {

struct PendingTifLevel {
	Level level;
	uint32_t width = 0;
	uint32_t height = 0;
	PixelMetric tag_metric;
};

// Derive every level's metric from the largest level of ITS OWN plane.
//
// Per plane, not per file: a multi-channel PTF holds N base levels of identical
// size, and one file-wide base would still be arithmetically right for them but
// wrong the moment two planes are scanned at different resolutions. Grouping by
// plane makes the rule say what it means. For a single-plane file - every vendor
// format - this is exactly the previous computation.
void assign_level_metrics ( std::vector<PendingTifLevel> *pending )
{
	if (!pending || pending->empty())
		return;

	auto planes = std::vector<PlaneSel>{};
	for (const auto &entry : *pending) {
		if (std::find( planes.begin(), planes.end(), entry.level.plane ) ==
			planes.end())
		{
			planes.push_back( entry.level.plane );
		}
	}

	for (const auto &plane : planes) {
		auto base_width = uint32_t{ 0 };
		auto base_area = uint64_t{ 0 };
		auto base_mpp = 0.0f;
		for (const auto &entry : *pending) {
			if (!( entry.level.plane == plane ))
				continue;
			const auto area = static_cast<uint64_t>( entry.width ) *
				static_cast<uint64_t>( entry.height );
			if (area > base_area) {
				base_area = area;
				base_width = entry.width;
				base_mpp = entry.tag_metric.x;
			}
		}
		if (base_mpp <= 0.0f)
			base_mpp = 1.0f;
		base_width = std::max<uint32_t>( base_width, 1 );

		for (auto &entry : *pending) {
			if (!( entry.level.plane == plane ))
				continue;
			const auto ratio = entry.width > 0
				? static_cast<float>( base_width ) / static_cast<float>( entry.width )
				: 1.0f;
			entry.level.metric = base_mpp * std::max( ratio, 1.0f );
		}
	}
}


// Apply a PTF plane-identity block to the level being built.
//
// Only what the block actually states is applied: an IFD without one leaves every
// field at its default, which is how a vendor .ptf, .bif or any other generic
// tiled TIFF keeps reading exactly as it did before this format existed.
void apply_ptf_tags_local ( const PtfPlaneTags &tags, Level *level )
{
	if (!tags.valid || !level)
		return;
	level->plane = PlaneSel{ tags.channel, tags.zlevel };
	level->pixel_type = tags.pixel_type;
	level->pixel_type_declared = true;
	level->max_pixel_value = tags.max_pixel_value;
}


PtfPlaneTags read_ptf_tags_local ( TIFF *tif )
{
	const char *description = nullptr;
	if (!TIFFGetField( tif, TIFFTAG_IMAGEDESCRIPTION, &description ) || !description)
		return {};
	return parse_ptf_tags( description );
}


} // namespace


bool ProbeWsiPixelType ( const std::string &filename, LevelPixelType *out )
{
	if (!out)
		return false;
//
// Only the formats that can carry the block are opened for it. Every slide open
// would otherwise pay a second TIFFOpen and a walk of every IFD - a vendor NDPI
// has thousands - to learn what it already knows: that the file says nothing.
	auto dot = filename.rfind( '.' );
	if (dot == std::string::npos)
		return false;
	const auto ext = lower_ascii_copy( std::string_view( filename ).substr( dot ) );
#if USE_DCMTK
	if (ext == ".dcm")
		return ProbeDcmPixelType( filename, out );
#endif
	if (wsi_format_for_extension( ext ) == WsiFormat::Gallery)
		return ProbeGalleryPixelType( filename, out );
	if (ext != ".ptf" && ext != ".bif")
		return false;

	auto tif_handle = TiffHandle{};
	if (!tif_handle.open( filename ))
		return false;
	auto *tif = static_cast<TIFF *>( tif_handle.get() );

	auto found = false;
	do {
		auto tags = read_ptf_tags_local( tif );
		if (!ptf_is_level( tags ) || ptf_level_type_unknown( tags ))
			continue;
		*out = tags.pixel_type;
		found = true;
		break;
//
// The first level IFD settles it: every plane of one file shares a pixel type as
// far as a single open is concerned, and a caller that wants a different one asks
// for it explicitly.
	} while (TIFFReadDirectory( tif ));

	return found;
}


std::vector<std::string> ProbeWsiChannels ( const std::string &filename )
{
	auto channels = std::vector<std::string>{};
	auto dot = filename.rfind( '.' );
	if (dot == std::string::npos)
		return channels;
	const auto ext = lower_ascii_copy( std::string_view( filename ).substr( dot ) );
	if (wsi_format_for_extension( ext ) == WsiFormat::Gallery) {
		auto manifest = GalleryManifest{};
		if (read_gallery_manifest( std::filesystem::path( filename ), &manifest ) &&
			is_integer_gray( manifest.pixel_type ))
		{
			channels = gallery_channels( manifest );
		}
		return channels;
	}
	if (ext != ".ptf" && ext != ".bif")
		return channels;

	auto tif_handle = TiffHandle{};
	if (!tif_handle.open( filename ))
		return channels;
	auto *tif = static_cast<TIFF *>( tif_handle.get() );
	do {
		auto tags = read_ptf_tags_local( tif );
		if (!ptf_is_level( tags ) || ptf_level_type_unknown( tags ) || tags.channel.empty() ||
			!is_integer_gray( tags.pixel_type ))
		{
			continue;
		}
		if (std::find( channels.begin(), channels.end(), tags.channel ) == channels.end())
			channels.push_back( tags.channel );
	} while (TIFFReadDirectory( tif ));
	return channels;
}


bool WsiTifLoader::Open ( Ppyr &pyramid )
{
	if (!tif_.open( this->filename_ ))
		return false;
	auto *tif = static_cast<TIFF *>( tif_.get() );
//
// The base IFD's own tags, and its ICC profile if it carries one. Read here,
// while the current directory is still IFD 0.
	TIFFSetDirectory( tif, 0 );
	ReadTiffProperties( tif, pyramid );
	ReadTiffIccProfile( tif, pyramid );


	auto pending = std::vector<PendingTifLevel>{};
	auto ifd_idx = 0;
	do {
		uint32_t w = 0;
		uint32_t h = 0;
		TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &w );
		TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &h );

		const auto ptf_tags = read_ptf_tags_local( tif );
//
// An IFD our own writer marked as an associated image. Decoded here rather than
// left to the level walk below, which would otherwise reject it as "not a valid
// tiled level" and lose the slide's overview or label.
		if (ptf_tags.valid && !ptf_is_level( ptf_tags )) {
			auto name = ptf_tags.aux_name.empty()
				? std::string( kAuxMacro ) : ptf_tags.aux_name;
			auto image = Pwhl<Rgb>{};
			if (LoadStandardIFD<Rgb>( tif, image ) && image.is_set())
				aux_slot( pyramid, name ) = std::move( image );
			else
				pyr::plog_err( "WSI-TIF: IFD %d aux '%s' failed to decode\n",
					ifd_idx, name.c_str() );
			++ifd_idx;
			continue;
		}
		if (ptf_level_type_unknown( ptf_tags )) {
			pyr::plog_err( "WSI-TIF: IFD %d skipped, unknown PTF pixel-type '%s'\n",
				ifd_idx, ptf_tags.unknown_pixel_type.c_str() );
			++ifd_idx;
			continue;
		}

		uint32_t tw = 0;
		uint32_t th = 0;
		const auto is_tiled = TIFFIsTiled( tif ) != 0;
		if (is_tiled) {
			TIFFGetField( tif, TIFFTAG_TILEWIDTH, &tw );
			TIFFGetField( tif, TIFFTAG_TILELENGTH, &th );
		}

		if (!is_tiled || w == 0 || h == 0 || tw == 0 || th == 0) {
			if (pyr::debug_file()) {
				fprintf( pyr::debug_file(), "WSI-TIF: IFD %d skipped, not a valid tiled level\n",
					ifd_idx );
				fflush( pyr::debug_file() );
			}
			++ifd_idx;
			continue;
		}
		auto compression = uint16_t{};
		TIFFGetFieldDefaulted( tif, TIFFTAG_COMPRESSION, &compression );
		const auto codec = compression == COMPRESSION_JXL ? ImgCodec::Jxl
			: compression == COMPRESSION_JP2000 ? ImgCodec::Jp2k : ImgCodec::None;
		if (img_codec_unavailable_reason( codec )) {
			pyramid.unsupported_codec_->store( codec );
			return false;
		}

		auto nx = static_cast<int>( ( w + tw - 1 ) / tw );
		auto ny = static_cast<int>( ( h + th - 1 ) / th );
		if (nx <= 0 || ny <= 0) {
			++ifd_idx;
			continue;
		}

		auto level = Level{};
		apply_ptf_tags_local( ptf_tags, &level );
		auto max_fsz = size_t{ 0 };
		if (!BuildTiffTileTable( tif, level, { w, h }, { tw, th }, max_fsz )) {
			++ifd_idx;
			continue;
		}

		if (max_fsz == 0 || max_fsz > static_cast<size_t>( std::numeric_limits<int>::max() )) {
			if (pyr::debug_file()) {
				fprintf( pyr::debug_file(),
					"WSI-TIF: IFD %d skipped, invalid max tile byte count=%llu\n",
					ifd_idx, static_cast<unsigned long long>( max_fsz ) );
				fflush( pyr::debug_file() );
			}
			++ifd_idx;
			continue;
		}

		level.tbf.init( static_cast<int>( max_fsz ), static_cast<int>( level.tiles.size() ) );

		if (pyr::debug_file()) {
			fprintf( pyr::debug_file(),
				"WSI-TIF: IFD %d accepted %ux%u tiled %ux%u grid=%dx%d max_tile=%llu\n",
				ifd_idx, w, h, tw, th, nx, ny,
				static_cast<unsigned long long>( max_fsz ) );
			fflush( pyr::debug_file() );
		}

		pending.push_back( PendingTifLevel{
			std::move( level ), w, h, ReadTiffPixelMetric( tif ) } );
		++ifd_idx;
	} while (TIFFReadDirectory( tif ));

	assign_level_metrics( &pending );
	if (!pending.empty()) {
		auto base = std::max_element( pending.begin(), pending.end(),
			[] ( const PendingTifLevel &a, const PendingTifLevel &b ) {
				return static_cast<uint64_t>( a.width ) * a.height <
					static_cast<uint64_t>( b.width ) * b.height;
			} );
		pyramid.source_pixel_metric = base->tag_metric;
	}
	std::sort( pending.begin(), pending.end(),
		[] ( const PendingTifLevel &a, const PendingTifLevel &b ) {
			return a.level.metric < b.level.metric;
		} );

	pyramid.levels.reserve( pending.size() );
	for (auto &entry : pending)
		pyramid.levels.push_back( std::move( entry.level ) );

	if (!pyramid.levels.empty() && !pyramid.levels[0].tiles.empty()) {
//
// From `bounds`, which set_level_bounds derived, rather than from the last tile:
// on a row-major grid the two agree, but bounds is the level's stated extent and
// the last tile only happens to reach it.
		auto mw = pyramid.levels[0].bounds.br.x * pyramid.levels[0].metric;
		auto mh = pyramid.levels[0].bounds.br.y * pyramid.levels[0].metric;
		pyramid.mrct = {{0, 0}, {mw, mh}};
	}

	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(), "WSI-TIF: %zu pyramid levels loaded\n",
			pyramid.levels.size() );
		for (auto i = 0; i < static_cast<int>( pyramid.levels.size() ); ++i) {
			const auto &level = pyramid.levels[i];
			fprintf( pyr::debug_file(), "WSI-TIF: L%d metric=%f tiles=%zu tile_dim=%dx%d\n",
				i, level.metric, level.tiles.size(), level.tile_w, level.tile_h );
		}
		fflush( pyr::debug_file() );
	}

	return !pyramid.levels.empty();
}


void WsiTifLoader::Close ( Ppyr &pyramid )
{
	tif_.close();
	this->CloseFile();
	pyramid.levels.clear();
}


bool WsiTifLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this] ( auto &level, auto &tile, uint8_t *dest, size_t &pos, unsigned worker ) {
			if (!this->ReadFromFile( dest, tile.foff, tile.fsz, worker,
				level.filename ))
				return false;
			pos = tile.fsz;
			return true;
		} );
}


} // namespace priml
