
#include "wsi.h"

#include "tiff_inc.h"
#include "wsi_tif_aux.h"

#include "pyr_rt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace priml {

constexpr uint32_t NDPI_SOURCELENS = 65421;
constexpr uint32_t NDPI_STRIPOFFSETS = 65426;
constexpr uint32_t NDPI_REFERENCE = 65427;	// operator/scanner slide reference (barcode text)


// Read an NDPI ASCII tag (e.g. Reference, 65427) from the current TIFF
// directory. NDPI custom tags are anonymous fields, so we resolve the field
// info first and handle both count-prefixed and plain ASCII forms. Returns ""
// when the tag is absent or empty. This is the NDPI analog of reading the
// barcode from DICOM data elements - no image decoding involved.
static std::string ReadNdpiAscii ( TIFF *tif, uint32_t tag ) {
	auto *fld = TIFFFindField( tif, tag, TIFF_ANY );
	if (!fld || TIFFFieldDataType( fld ) != TIFF_ASCII)
		return {};

	const char *s = nullptr;
	if (TIFFFieldPassCount( fld )) {
		auto count_size = TIFFFieldSetGetCountSize( fld );
		if (count_size == 2) {
			uint16_t cnt = 0;
			if (!TIFFGetField( tif, tag, &cnt, &s ) || !s)
				return {};
		} else {
			uint32_t cnt = 0;
			if (!TIFFGetField( tif, tag, &cnt, &s ) || !s)
				return {};
		}
	} else if (!TIFFGetField( tif, tag, &s ) || !s) {
		return {};
	}

	std::string out( s );
	while (!out.empty() &&
		(out.back() == '\0' || out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
		out.pop_back();
	return out;
}


static bool ParseJpegHeaderInfo ( const uint8_t *data, int size,
	int &restart_interval, Dim2D<int> &mcu_dimensions )
{
	restart_interval = 0;
	mcu_dimensions = { 8, 8 };
	int max_h = 1, max_v = 1;

	int pos = 0;
	while (pos < size - 1) {
		if (data[pos] != 0xFF) { pos++; continue; }
		auto marker = data[pos + 1];
		pos += 2;
		if (marker == 0xFF || marker == 0x00) continue;
		if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7))
			continue;
		if (marker == 0xDA) break;
		if (pos + 2 > size) break;
		int seg_len = (data[pos] << 8) | data[pos + 1];
		if (seg_len < 2) break;

		if (marker == 0xDD && seg_len >= 4 && pos + seg_len <= size)
			restart_interval = (data[pos + 2] << 8) | data[pos + 3];

		if (marker == 0xC0 && seg_len >= 8 && pos + seg_len <= size) {
			int ncomp = data[pos + 7];
			for (int c = 0; c < ncomp && pos + 8 + c * 3 + 1 < size; ++c) {
				int hv = data[pos + 8 + c * 3 + 1];
				int hf = (hv >> 4) & 0xF;
				int vf = hv & 0xF;
				if (hf > max_h) max_h = hf;
				if (vf > max_v) max_v = vf;
			}
		}

		pos += seg_len;
	}

	mcu_dimensions = { max_h * 8, max_v * 8 };
	return true;
}


// Patch NDPI JPEG in-place: fix DRI/SOF markers for tile fragment
static void PatchNdpiJpeg ( uint8_t *data, size_t size ) {
	auto *start = data;
	auto *end = start + size;

	std::vector<uint8_t> dri_pattern = {0xFF, 0xDD};
	auto *dri_pos = std::search( start, end, dri_pattern.begin(), dri_pattern.end() );

	std::vector<uint8_t> sof_pattern = {0xFF, 0xC0};
	auto *sof_pos = std::search( start, end, sof_pattern.begin(), sof_pattern.end() );

	if (dri_pos != end && sof_pos != end) {
		uint16_t frag_width = 8 * (256 * dri_pos[4] + dri_pos[5]);
		sof_pos[5] = 0;
		sof_pos[6] = 8;
		sof_pos[7] = *((uint8_t *)&frag_width + 1);
		sof_pos[8] = *((uint8_t *)&frag_width);
	}
}


template <typename pixeltype>
static uint64_t AuxArea ( const Pwhl<pixeltype> &img ) {
	return (uint64_t)img.w * (uint64_t)img.h;
}


template <typename pixeltype>
static float AspectRatio ( const Pwhl<pixeltype> &img ) {
	return img.h > 0 ? (float)img.w / (float)img.h : 0.0f;
}


static bool HasWsiLikeAspect ( float img_aspect, float wsi_aspect ) {
	if (img_aspect <= 0.0f || wsi_aspect <= 0.0f)
		return false;

	auto ratio = img_aspect / wsi_aspect;
	if (ratio < 1.0f)
		ratio = 1.0f / ratio;
	return ratio <= 1.2f;
}


template <typename pixeltype>
static void AssignAuxImage ( Ppyr &pyramid, Pwhl<pixeltype> &&img,
	float wsi_aspect )
{
	auto area = AuxArea( img );
	if (area == 0)
		return;

	auto img_aspect = AspectRatio( img );
	auto wsi_like = HasWsiLikeAspect( img_aspect, wsi_aspect );

//
// A slide-shaped aux IFD is the MACRO, not a tissue-only overview: an NDPI's is
// a composite of two captures - the printed label panel at one end (reflected
// light) and the slide itself (transmitted light) - so it shows the whole glass.
// Anything not slide-shaped is a genuine label.
	if (wsi_like) {
		auto &macro = aux_slot( pyramid, kAuxMacro );
		if (area > AuxArea( macro ))
			macro = std::move( img );
	} else {
		auto &frost = aux_slot( pyramid, kAuxFrost );
		if (area > AuxArea( frost ))
			frost = std::move( img );
	}
}


template <typename pixeltype>
static void LogAuxAssignment ( const Pwhl<pixeltype> &img, float wsi_aspect )
{
	auto img_aspect = AspectRatio( img );
	auto wsi_like = HasWsiLikeAspect( img_aspect, wsi_aspect );
	pyr::plog( "NDPI aux IFD: %dx%d aspect=%.3f -> %s\n",
		img.w, img.h, img_aspect,
		wsi_like ? "macro-candidate" : "label-candidate" );
}


static bool IsLensOverview ( float lens_val ) {
	return lens_val < -0.5f && lens_val > -1.5f;
}


static bool IsLensMap ( float lens_val ) {
	return lens_val < -1.5f && lens_val > -2.5f;
}


// Returns the source-lens magnification, and reports through `has_lens` whether
// tag 65421 was there at all - the positional IFD fallbacks in ProcessNdpiIfd
// must not override a lens value the scanner actually recorded.
static float ReadNdpiLensValue ( TIFF *tif, bool *has_lens = nullptr )
{
	auto lens_value = 1.0f;
	if (has_lens)
		*has_lens = false;
	auto *field = TIFFFindField( tif, NDPI_SOURCELENS, TIFF_ANY );
	if (!field)
		return lens_value;
	if (has_lens)
		*has_lens = true;
	auto assign_value = [&] ( const void *raw ) {
		if (!raw)
			return;
		auto data_type = TIFFFieldDataType( field );
		auto value_size = TIFFFieldSetGetSize( field );
		if (data_type == TIFF_FLOAT)
			lens_value = *(const float *)raw;
		else if (data_type == TIFF_SLONG)
			lens_value = (float)*(const int32_t *)raw;
		else if (data_type == TIFF_RATIONAL || data_type == TIFF_SRATIONAL)
			lens_value = value_size == 8
				? (float)*(const double *)raw : *(const float *)raw;
	};
	if (TIFFFieldPassCount( field )) {
		void *values = nullptr;
		auto count_size = TIFFFieldSetGetCountSize( field );
		if (count_size == 2) {
			uint16_t count = 0;
			if (TIFFGetField( tif, NDPI_SOURCELENS, &count, &values ) && count >= 1)
				assign_value( values );
		}
		else if (count_size == 4) {
			uint32_t count = 0;
			if (TIFFGetField( tif, NDPI_SOURCELENS, &count, &values ) && count >= 1)
				assign_value( values );
		}
	}
	else if (TIFFFieldDataType( field ) == TIFF_FLOAT) {
		TIFFGetField( tif, NDPI_SOURCELENS, &lens_value );
	}
	else if (TIFFFieldDataType( field ) == TIFF_SLONG) {
		auto value = int32_t{ 0 };
		TIFFGetField( tif, NDPI_SOURCELENS, &value );
		lens_value = (float)value;
	}
	return lens_value;
}


static void ReadNdpiBlockMap ( TIFF *tif, Level *level )
{
	void *data = nullptr;
	uint32_t count = 0;
	if (!TIFFGetField( tif, 65426, &count, &data ) || !data)
		return;
	auto *values = (uint32_t *)data;
	auto elements = count;
	if (values[0] < 100 || values[0] > 5000) {
		auto alternative = count / 4;
		if (alternative >= 2 && values[0] >= 100 && values[0] <= 5000)
			elements = alternative;
	}
	level->block_sizes.assign( values, values + elements );
}


static void ReadNdpiJpegHeader ( const std::string &filename,
	uint64_t strip_offset,
	Level *level )
{
	if (level->block_sizes.empty() || level->block_sizes[0] < 100 ||
		level->block_sizes[0] > 2000 || strip_offset == 0)
	{
		return;
	}
	auto header_size = level->block_sizes[0];
	auto stream = std::ifstream( pyr::path_from_utf8( filename ), std::ios::binary );
	if (!stream)
		return;
	level->hdrbf.resize( header_size );
	stream.seekg( strip_offset );
	stream.read( (char *)level->hdrbf.data(), header_size );
}


static void AppendNdpiTiles ( const Dim2D<uint32_t> &dimensions,
	uint64_t strip_offset,
	uint64_t strip_bytes,
	Level *level )
{
	auto nx = ((int)dimensions.w + level->tile_w - 1) / level->tile_w;
	auto ny = ((int)dimensions.h + level->tile_h - 1) / level->tile_h;
//
// Tag 65426 holds one cumulative in-strip offset per tile, so a tile's size is the
// distance to its successor. The last tile has none: its end is the end of the
// strip, which only the strip byte count knows. Without it that tile kept
// foff=fsz=0, loaded nothing and failed to decode — one lost tile per level, in
// every NDPI. A file that reports no byte count keeps the old behaviour.
	auto block_count = (int)level->block_sizes.size();
	auto strip_end = strip_bytes > 0 ? strip_offset + strip_bytes : uint64_t{ 0 };
	auto max_fsz = size_t{ 0 };
	constexpr auto kMaxTileFileSize = size_t{ 10 * 1024 * 1024 };
	for (auto y = 0; y < ny; ++y) {
		for (auto x = 0; x < nx; ++x) {
			auto tile = Tile{};
			tile.lr = grid_tile_rect( { (int)dimensions.w, (int)dimensions.h },
				{ level->tile_w, level->tile_h }, x, y );
			auto block_index = y * nx + x;
			if (block_index < block_count) {
				tile.foff = strip_offset + level->block_sizes[block_index];
				auto next_start = block_index + 1 < block_count
					? strip_offset + level->block_sizes[block_index + 1]
					: strip_end;
				tile.fsz = next_start > tile.foff
					? (size_t)( next_start - tile.foff ) : 0;
			}
			if (tile.fsz > 0 && tile.fsz < kMaxTileFileSize)
				max_fsz = std::max( max_fsz, tile.fsz );
			level->tiles.push_back( std::move( tile ) );
		}
	}
	if (max_fsz == 0)
		max_fsz = 64 * 1024;
	set_level_bounds( *level );
	level->tbf.init( (int)(level->hdrbf.size() + max_fsz + 2),
		(int)level->tiles.size() );
}


static void ProcessNdpiIfd ( TIFF *tif,
	const std::string &filename,
	int ifd_index,
	float wsi_aspect,
	Ppyr *pyramid )
{
	auto level = Level{};
	uint32_t width = 0, height = 0;
	TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &width );
	TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &height );
	auto source_metric = ReadTiffPixelMetric( tif );
	level.metric = source_metric.x > 0.0f ? source_metric.x : 1.0f;
	auto has_lens = false;
	auto lens_value = ReadNdpiLensValue( tif, &has_lens );
//
// IFD position is only a fallback for files with no source-lens tag. NDP.scan
// output has one, and there it lies: on a C9600 slide IFD 5 is a genuine 0.625x
// level and the macro sits at IFD 6.
	if (IsLensOverview( lens_value ) || ( !has_lens && ifd_index == 5 )) {
		LoadStandardIFD( tif, aux_slot( *pyramid, kAuxMacro ) );
		return;
	}
	if (IsLensMap( lens_value ) || ( !has_lens && ifd_index == 6 )) {
		pyr::plog( "NDPI map IFD: %ux%u lens=%g (dropped)\n",
			width, height, lens_value );
		return;
	}
	ReadNdpiBlockMap( tif, &level );
	auto strip_offset = TIFFGetStrileOffset( tif, 0 );
//
// The strip byte count is the only end bound for the level's last tile; tag 65426
// gives starts only. Zero (tag absent) leaves that tile sized as it was before.
	auto strip_bytes = TIFFGetStrileByteCount( tif, 0 );
	ReadNdpiJpegHeader( filename, strip_offset, &level );
	if (level.block_sizes.empty()) {
		auto auxiliary = Pwhl<Rgb>{};
		if (LoadStandardIFD( tif, auxiliary )) {
			LogAuxAssignment( auxiliary, wsi_aspect );
			AssignAuxImage( *pyramid, std::move( auxiliary ), wsi_aspect );
		}
		else
			pyr::plog( "NDPI aux IFD: %ux%u not loadable, skipped\n", width, height );
		return;
	}
	if (!( pyramid->source_pixel_metric.x > 0.0f ))
		pyramid->source_pixel_metric = source_metric;
	if (!level.hdrbf.empty()) {
		auto restart_interval = 0;
		auto mcu_dimensions = Dim2D<int>{};
		ParseJpegHeaderInfo( level.hdrbf.data(), (int)level.hdrbf.size(),
			restart_interval, mcu_dimensions );
		if (level.block_sizes.size() > 1 && restart_interval > 0 &&
			mcu_dimensions.w > 0)
		{
			level.tile_w = restart_interval * mcu_dimensions.w;
			level.tile_h = 8;
			level.tile_w = ((level.tile_w + mcu_dimensions.w - 1) /
				mcu_dimensions.w) * mcu_dimensions.w;
		}
	}
	if (level.tile_w <= 0 || level.tile_h <= 0) {
		level.tile_w = (int)width;
		level.tile_h = (int)height;
	}
	AppendNdpiTiles( { width, height }, strip_offset, strip_bytes, &level );
	pyramid->levels.push_back( std::move( level ) );
}


bool NdpiLoader::Open ( Ppyr &pyramid ) {
	if (!tif_.open( this->filename_ ))
		return false;
	auto *tif = static_cast<TIFF *>( tif_.get() );
//
// The base IFD's own tags, and its ICC profile if it carries one. Read here,
// while the current directory is still IFD 0.
	TIFFSetDirectory( tif, 0 );
	ReadTiffProperties( tif, pyramid );
	ReadTiffIccProfile( tif, pyramid );
//
// The two NDPI private tags worth keeping: the source lens the scanner recorded,
// and the operator/scanner slide reference - which is where a barcode comes from
// when the scanner read one, no image decoding involved.
	{
		auto has_lens = false;
		auto lens = ReadNdpiLensValue( tif, &has_lens );
		if (has_lens)
			set_property( pyramid, "hamamatsu.SourceLens", std::to_string( lens ) );
		set_property( pyramid, "hamamatsu.Reference",
			ReadNdpiAscii( tif, NDPI_REFERENCE ) );
		if (lens > 0.0f)
			set_property( pyramid, "pyra.objective-power",
				std::to_string( lens ) );
	}


// Slide reference / barcode text, stored by NDP.scan in tag 65427 on the main
// directory. Read it straight from metadata (the NDPI analog of the DICOM tag
// path); empty when the operator/scanner recorded no reference.
	if (pyramid.barcode.empty()) {
		pyramid.barcode = ReadNdpiAscii( tif, NDPI_REFERENCE );
		pyramid.barcode_from_metadata = !pyramid.barcode.empty();
	}

	uint32_t wsi_w = 0, wsi_h = 0;
	TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &wsi_w );
	TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &wsi_h );
	auto wsi_aspect = wsi_h > 0 ? (float)wsi_w / (float)wsi_h : 0.0f;

	int ifd_idx = 0;
	do {
		ProcessNdpiIfd(
			tif, this->filename_, ifd_idx, wsi_aspect, &pyramid );

		++ifd_idx;
	} while (TIFFReadDirectory( tif ));


	std::sort( pyramid.levels.begin(), pyramid.levels.end(),
		[] ( const Level &a, const Level &b ) {
			return a.metric < b.metric;
		});

	if (!pyramid.levels.empty() && !pyramid.levels[0].tiles.empty()) {
		auto mw = pyramid.levels[0].tiles.back().lr.br.x * pyramid.levels[0].metric;
		auto mh = pyramid.levels[0].tiles.back().lr.br.y * pyramid.levels[0].metric;
		pyramid.mrct = {{0, 0}, {mw, mh}};
	}

	return !pyramid.levels.empty();
}


void NdpiLoader::Close ( Ppyr &pyramid ) {
	tif_.close();
	this->CloseFile();
	pyramid.levels.clear();
}


bool NdpiLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this] ( auto &level, auto &tile, uint8_t *dest, size_t &pos, unsigned worker ) {
			if (!level.hdrbf.empty()) {
				memcpy( dest, level.hdrbf.data(), level.hdrbf.size() );
				pos = level.hdrbf.size();
			}

			if (!this->ReadFromFile( dest + pos, tile.foff, tile.fsz, worker,
				level.filename ))
				return false;
			pos += tile.fsz;

			dest[pos++] = 0xFF;
			dest[pos++] = 0xD9;

			PatchNdpiJpeg( dest, pos );
			return true;
		} );
}



} // namespace priml
