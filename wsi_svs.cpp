
#include "wsi.h"

#include "tiff_inc.h"
#include "wsi_formats.h"
#include "wsi_tif_aux.h"

#include "pyr_rt.h"

#include <string>


#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace priml {

// Aperio's private TIFF compression codes for JPEG 2000 tiles. Not in tiff.h -
// libtiff has no codec for them, which is why the tile bytes are handed to the
// codec layer as a raw codestream instead of going through TIFFReadEncodedTile.
constexpr uint16_t kAperioJ2kYCbCr = 33003;
constexpr uint16_t kAperioJ2kRgb = 33005;


// Every "Key = Value" Aperio wrote into ImageDescription, as aperio.<Key>.
//
// The description is a header line, then a run of pipe-separated pairs:
//
//   Aperio Image Library ...<CRLF>46000x32914 [0,100 ...] (256x256) JPEG/RGB Q=30
//   |AppMag = 20|MPP = 0.4990|ScanScope ID = ...|Filename = ...
//
// Kept verbatim beside the normalised readings: openslide.mpp-x says what we
// concluded, aperio.MPP says what Aperio wrote. Where they disagree, the raw one
// is the evidence.
static void RecordAperioDescription ( const char *desc, Ppyr &pyramid )
{
	if (!desc || !*desc)
		return;

	auto text = std::string( desc );
	auto trim = [] ( std::string v ) {
		auto first = v.find_first_not_of( " \t\r\n" );
		if (first == std::string::npos)
			return std::string{};
		auto last = v.find_last_not_of( " \t\r\n" );
		return v.substr( first, last - first + 1 );
	};

	auto at = text.find( '|' );
	while (at != std::string::npos) {
		auto next = text.find( '|', at + 1 );
		auto field = text.substr( at + 1,
			next == std::string::npos ? std::string::npos : next - at - 1 );
		auto eq = field.find( '=' );
		if (eq != std::string::npos) {
			auto key = trim( field.substr( 0, eq ) );
			auto value = trim( field.substr( eq + 1 ) );
			if (!key.empty() && !value.empty())
				set_property( pyramid, "aperio." + key, value );
		}
		at = next;
	}

//
// The two Aperio keys with a normalised counterpart. Objective power has no
// other source in an SVS; mpp is cross-checked against Level::metric by the
// property test rather than trusted blindly.
	if (const auto *mag = find_property( pyramid, "aperio.AppMag" ))
		set_property( pyramid, "openslide.objective-power", *mag );
}


// Parse "MPP = X.XXXX" from SVS ImageDescription
static float ParseMppFromDescription ( const char *desc ) {
	if (!desc) return 0.0f;
	auto p = strstr( desc, "MPP = " );
	if (!p) p = strstr( desc, "MPP=" );
	if (!p) return 0.0f;
	p = strchr( p, '=' );
	if (!p) return 0.0f;
	p++;
	while (*p == ' ') p++;
	return (float)atof( p );
}


// Which auxiliary role, if any, an SVS IFD carries.
enum class SvsAux { None, Label, Macro };


// Aperio writes the IFD's role as the first token of ImageDescription's SECOND
// line ("Aperio Image Library v11.2.1 \r\nlabel 387x463"). Matching the whole
// description instead would misread a slide whose *name* contains "label" or
// "macro" - IFD 1 carries the slide name in its description.
static SvsAux ClassifySvsIfd ( const char *desc ) {
	if (!desc)
		return SvsAux::None;

	auto *line = strchr( desc, '\n' );
	if (!line)
		return SvsAux::None;
	++line;
	while (*line == ' ' || *line == '\t' || *line == '\r')
		++line;

//
// ASCII case-insensitive prefix match; `name` is given in lower case.
	auto token_is = [line] ( const char *name ) {
		auto len = strlen( name );
		if (lower_ascii_copy( std::string_view( line ).substr( 0, len ) ) != name)
			return false;
//
// Require a boundary so "labelling" does not read as "label".
		auto next = line[len];
		return next == '\0' || !isalnum( (unsigned char)next );
	};
	if (token_is( "label" ))
		return SvsAux::Label;
	if (token_is( "macro" ))
		return SvsAux::Macro;
	return SvsAux::None;
}


struct SvsBaseInfo {
	float mpp = 0.0f;
	PixelMetric source_metric;
	uint32_t width = 0;
	uint32_t height = 0;
};


static SvsBaseInfo ReadSvsBaseInfo ( TIFF *tif )
{
	auto info = SvsBaseInfo{};
	char *desc = nullptr;
	if (TIFFGetField( tif, TIFFTAG_IMAGEDESCRIPTION, &desc ) && desc)
		info.mpp = ParseMppFromDescription( desc );
	auto tiff_metric = ReadTiffPixelMetric( tif );
	TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &info.width );
	TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &info.height );
	if (info.mpp <= 0.0f)
		info.mpp = tiff_metric.x;
	if (info.mpp <= 0.0f)
		info.mpp = 1.0f;
	info.source_metric = tiff_metric.x > 0.0f && tiff_metric.y > 0.0f
		? tiff_metric : PixelMetric{ info.mpp, info.mpp };
	return info;
}


static bool AppendSvsIfd ( TIFF *tif,
	int ifd_index,
	const SvsBaseInfo &base,
	Ppyr *pyramid )
{
	uint32_t width = 0;
	uint32_t height = 0;
	TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &width );
	TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &height );
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(), "SVS: IFD %d: %ux%u", ifd_index, width, height );
		fflush( pyr::debug_file() );
	}
	char *desc = nullptr;
	TIFFGetField( tif, TIFFTAG_IMAGEDESCRIPTION, &desc );
	if (ifd_index > 0) {
		auto aux = ClassifySvsIfd( desc );
//
// The label is the frosted end of the slide - the barcode lives there. The
// macro is the slide overview. Decode both auxiliary images while keeping their
// IFDs out of the diagnostic pyramid level list.
		if (aux == SvsAux::Label) {
			auto loaded = LoadStandardIFD( tif, aux_slot( *pyramid, kAuxFrost ) );
			if (pyr::debug_file())
				fprintf( pyr::debug_file(), " [label%s]\n",
					loaded ? "" : ", decode failed" );
			return false;
		}
		if (aux == SvsAux::Macro) {
			auto loaded = LoadStandardIFD( tif, aux_slot( *pyramid, kAuxMacro ) );
			if (pyr::debug_file())
				fprintf( pyr::debug_file(), " [macro%s]\n",
					loaded ? "" : ", decode failed" );
			return false;
		}
	}
	uint32_t tile_width = 0;
	uint32_t tile_height = 0;
	auto is_tiled = TIFFIsTiled( tif );
	if (is_tiled) {
		TIFFGetField( tif, TIFFTAG_TILEWIDTH, &tile_width );
		TIFFGetField( tif, TIFFTAG_TILELENGTH, &tile_height );
	}
	if (!is_tiled || tile_width == 0 || tile_height == 0) {
		if (pyr::debug_file())
			fprintf( pyr::debug_file(), " [not tiled, skipped]\n" );
		return false;
	}
	uint16_t compression = 0;
	TIFFGetField( tif, TIFFTAG_COMPRESSION, &compression );
	if (compression != COMPRESSION_JPEG && compression != COMPRESSION_JXL &&
		compression != kAperioJ2kYCbCr && compression != kAperioJ2kRgb)
	{
		if (pyr::debug_file())
			fprintf( pyr::debug_file(), " [compression=%u, unsupported]\n", compression );
		return false;
	}
	if (pyr::debug_file())
		fprintf( pyr::debug_file(), " [tiled %ux%u, comp=%u]\n",
			tile_width, tile_height, compression );

	auto level = Level{};
	level.tile_w = (int)tile_width;
	level.tile_h = (int)tile_height;
// 33003 stores YCbCr with the JPEG 2000 MCT switched off, so the decoder cannot
// tell from the codestream and the TIFF tag is the only evidence. 33005 carries
// a real MCT (ICT), which the decoder inverts itself.
	if (compression == kAperioJ2kYCbCr)
		level.color_hint = ColorHint::YCbCr;
	level.metric = ifd_index == 0
		? base.mpp
		: base.mpp * (float)base.width / (float)width;
	void *table_data = nullptr;
	uint32_t table_length = 0;
	if (TIFFGetField( tif, TIFFTAG_JPEGTABLES, &table_length, &table_data ) &&
		table_data && table_length > 4)
	{
		level.hdrbf.assign( (uint8_t *)table_data,
			(uint8_t *)table_data + table_length - 2 );
	}
	auto max_bytes = size_t{ 0 };
	if (!BuildTiffTileTable( tif, level, { width, height },
		{ tile_width, tile_height }, max_bytes ))
		return false;
	level.tbf.init( (int)( level.hdrbf.size() + max_bytes ),
		(int)level.tiles.size() );
	pyramid->levels.push_back( std::move( level ) );
	if (pyr::debug_file())
		fflush( pyr::debug_file() );
	return true;
}


static void FinishSvsPyramid ( Ppyr *pyramid )
{
	std::sort( pyramid->levels.begin(), pyramid->levels.end(),
		[] ( const Level &a, const Level &b ) {
			return a.metric < b.metric;
		} );
	if (!pyramid->levels.empty()) {
		auto &finest = pyramid->levels[0];
		auto width = finest.tiles.back().lr.br.x * finest.metric;
		auto height = finest.tiles.back().lr.br.y * finest.metric;
		pyramid->mrct = {{0, 0}, {width, height}};
	}
	if (!pyr::debug_file())
		return;
	fprintf( pyr::debug_file(), "SVS: %zu pyramid levels loaded\n",
		pyramid->levels.size() );
	for (auto i = 0; i < (int)pyramid->levels.size(); ++i) {
		auto &level = pyramid->levels[i];
		fprintf( pyr::debug_file(),
			"SVS: L%d: metric=%f tiles=%zu tile_dim=%dx%d\n",
			i, level.metric, level.tiles.size(), level.tile_w, level.tile_h );
	}
	fflush( pyr::debug_file() );
}


bool SvsLoader::Open ( Ppyr &pyramid ) {
	if (!tif_.open( this->filename_ ))
		return false;
	auto *tif = static_cast<TIFF *>( tif_.get() );
//
// The base IFD's own tags, and its ICC profile if it carries one. Read here,
// while the current directory is still IFD 0.
	TIFFSetDirectory( tif, 0 );
	ReadTiffProperties( tif, pyramid );
	ReadTiffIccProfile( tif, pyramid );
	{
		const char *base_desc = nullptr;
		if (TIFFGetField( tif, TIFFTAG_IMAGEDESCRIPTION, &base_desc ))
			RecordAperioDescription( base_desc, pyramid );
	}

	auto base = ReadSvsBaseInfo( tif );
	pyramid.source_pixel_metric = base.source_metric;
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(), "SVS: base MPP=%f, base dims=%ux%u\n",
			base.mpp, base.width, base.height );
		fflush( pyr::debug_file() );
	}

// Iterate IFDs and build pyramid levels
	int ifd_idx = 0;
	do {
		AppendSvsIfd( tif, ifd_idx, base, &pyramid );
		++ifd_idx;
	} while (TIFFReadDirectory( tif ));

	FinishSvsPyramid( &pyramid );
	return !pyramid.levels.empty();
}


void SvsLoader::Close ( Ppyr &pyramid ) {
	tif_.close();
	this->CloseFile();
	pyramid.levels.clear();
}


bool SvsLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this] ( auto &level, auto &tile, uint8_t *dest, size_t &pos, unsigned worker ) {
			if (!level.hdrbf.empty()) {
				memcpy( dest, level.hdrbf.data(), level.hdrbf.size() );
				pos = level.hdrbf.size();
			}

			size_t skip = 0;
			if (!level.hdrbf.empty() && tile.fsz >= 2) skip = 2;
			auto read_sz = tile.fsz - skip;

			if (!this->ReadFromFile( dest + pos, tile.foff + skip, read_sz, worker,
				level.filename ))
				return false;
			pos += read_sz;
			return true;
		} );
}



} // namespace priml
