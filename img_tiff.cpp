#include "img_tiff.h"

#include "tiff_inc.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace priml {

namespace {

bool set_error_local ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	return false;
}


// How a TIFF's samples are laid out, as far as copying them out needs to know.
struct TiffSampleFormat {
	LevelPixelType type = LevelPixelType::Rgb8;
	Dim2D<int> size{ 0, 0 };
	int samples_per_pixel = 0;		// as stored, extra samples included
	int bytes_per_sample = 0;
	bool min_is_white = false;
};


bool classify_local ( TIFF *tif, TiffSampleFormat *out, std::string *error )
{
	uint32_t w = 0;
	uint32_t h = 0;
	if (!TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &w ) ||
		!TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &h ) || w == 0 || h == 0 ||
		w > (uint32_t)std::numeric_limits<int>::max() ||
		h > (uint32_t)std::numeric_limits<int>::max())
	{
		return set_error_local( error, "no usable image dimensions" );
	}

	uint16_t bits = 0;
	uint16_t spp = 0;
	uint16_t sample_format = 0;
	uint16_t planar = 0;
	uint16_t photometric = 0;
	uint16_t compression = 0;
	TIFFGetFieldDefaulted( tif, TIFFTAG_BITSPERSAMPLE, &bits );
	TIFFGetFieldDefaulted( tif, TIFFTAG_SAMPLESPERPIXEL, &spp );
	TIFFGetFieldDefaulted( tif, TIFFTAG_SAMPLEFORMAT, &sample_format );
	TIFFGetFieldDefaulted( tif, TIFFTAG_PLANARCONFIG, &planar );
	TIFFGetFieldDefaulted( tif, TIFFTAG_COMPRESSION, &compression );
	if (!TIFFGetField( tif, TIFFTAG_PHOTOMETRIC, &photometric ))
		return set_error_local( error, "no photometric interpretation" );

	if (bits != 8 && bits != 16)
		return set_error_local( error, std::to_string( bits ) +
			" bits per sample (8 or 16 supported)" );
	if (sample_format != SAMPLEFORMAT_UINT)
		return set_error_local( error, "samples are not unsigned integers" );
	if (spp > 1 && planar != PLANARCONFIG_CONTIG)
		return set_error_local( error, "planar-separate sample storage" );
	if (spp < 1)
		return set_error_local( error, "no samples per pixel" );

	auto format = TiffSampleFormat{};
	format.size = { (int)w, (int)h };
	format.samples_per_pixel = spp;
	format.bytes_per_sample = bits / 8;
	switch (photometric) {
		case PHOTOMETRIC_MINISWHITE:
			format.min_is_white = true;
			[[fallthrough]];
		case PHOTOMETRIC_MINISBLACK:
			format.type = bits == 8 ? LevelPixelType::Gray8 : LevelPixelType::Gray16;
			break;
		case PHOTOMETRIC_YCBCR:
//
// libjpeg converts JPEG-in-TIFF YCbCr itself when asked; raw subsampled YCbCr
// would need an upsampler nothing here has.
			if (compression != COMPRESSION_JPEG || bits != 8)
				return set_error_local( error, "YCbCr storage other than JPEG" );
			TIFFSetField( tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB );
			[[fallthrough]];
		case PHOTOMETRIC_RGB:
			if (spp < 3)
				return set_error_local( error, "RGB with fewer than 3 samples" );
			format.type = bits == 8 ? LevelPixelType::Rgb8 : LevelPixelType::Rgb16;
			break;
		case PHOTOMETRIC_PALETTE:
			return set_error_local( error, "palette colour" );
		default:
			return set_error_local( error, "photometric interpretation " +
				std::to_string( photometric ) );
	}
	*out = format;
	return true;
}


// libtiff over a read-only buffer.
struct MemoryStream {
	const uint8_t *data = nullptr;
	toff_t size = 0;
	toff_t pos = 0;
};


tmsize_t memory_read_local ( thandle_t handle, void *buffer, tmsize_t count )
{
	auto *stream = static_cast<MemoryStream *>( handle );
	if (count <= 0 || stream->pos >= stream->size)
		return 0;
	const auto n = std::min<toff_t>( (toff_t)count, stream->size - stream->pos );
	std::memcpy( buffer, stream->data + stream->pos, (size_t)n );
	stream->pos += n;
	return (tmsize_t)n;
}


tmsize_t memory_write_local ( thandle_t, void *, tmsize_t )
{
	return 0;
}


toff_t memory_seek_local ( thandle_t handle, toff_t offset, int whence )
{
	auto *stream = static_cast<MemoryStream *>( handle );
	switch (whence) {
		case SEEK_SET: stream->pos = offset; break;
		case SEEK_CUR: stream->pos += offset; break;
		case SEEK_END: stream->pos = stream->size + offset; break;
		default: return (toff_t)-1;
	}
	return stream->pos;
}


int memory_close_local ( thandle_t )
{
	return 0;
}


toff_t memory_size_local ( thandle_t handle )
{
	return static_cast<MemoryStream *>( handle )->size;
}


int memory_map_local ( thandle_t, void **, toff_t * )
{
	return 0;
}


void memory_unmap_local ( thandle_t, void *, toff_t )
{
}


struct TiffCloser {
	void operator() ( TIFF *tif ) const noexcept
	{
		if (tif)
			TIFFClose( tif );
	}
};

using TiffPtr = std::unique_ptr<TIFF, TiffCloser>;


void silence_libtiff_local ()
{
	TIFFSetErrorHandler( nullptr );
	TIFFSetWarningHandler( nullptr );
}


template <typename pixeltype>
pixeltype convert_sample_local ( const TiffSampleFormat &format, const uint8_t *s )
{
	if (format.bytes_per_sample == 1) {
		auto sample = [&] ( int i ) {
			return format.min_is_white ? (uint8_t)( 0xFF - s[i] ) : s[i];
		};
		if (is_integer_rgb( format.type ))
			return PixelFromRgbBytes<pixeltype>( sample( 0 ), sample( 1 ), sample( 2 ) );
		return PixelFromGrayByte<pixeltype>( sample( 0 ) );
	}
	auto sample = [&] ( int i ) {
		auto v = uint16_t{ 0 };
		std::memcpy( &v, s + (size_t)i * 2, sizeof( v ) );
		return format.min_is_white ? (uint16_t)( 0xFFFF - v ) : v;
	};
	if (is_integer_rgb( format.type ))
		return PixelFromRgbWords<pixeltype>( sample( 0 ), sample( 1 ), sample( 2 ) );
	return PixelFromGrayWord<pixeltype>( sample( 0 ) );
}


// One decoded chunk (strip or tile) of `chunk_w` x `rows` pixels into the
// destination at (x0, y0), clipped to the image.
template <typename pixeltype>
void copy_chunk_local ( const TiffSampleFormat &format, const uint8_t *src,
	int chunk_w, int rows, int x0, int y0, pixeltype *out, int stride )
{
	const auto src_pixel = (size_t)format.samples_per_pixel *
		(size_t)format.bytes_per_sample;
	const auto copy_w = std::min( chunk_w, format.size.w - x0 );
	const auto copy_rows = std::min( rows, format.size.h - y0 );
	for (auto r = 0; r < copy_rows; ++r) {
		const auto *s = src + (size_t)r * (size_t)chunk_w * src_pixel;
		auto *d = out + (size_t)( y0 + r ) * (size_t)stride + x0;
		for (auto x = 0; x < copy_w; ++x, s += src_pixel)
			d[x] = convert_sample_local<pixeltype>( format, s );
	}
}


// The one strip of a codec-strip TIFF: the codestream, as tiff_image_write_lossless
// stored it. Its samples are the image's, so there is nothing to convert or invert.
template <typename pixeltype>
bool decode_codec_strip_local ( TIFF *tif, const TiffSampleFormat &format,
	const TileDst<pixeltype> &dst, std::string *error )
{
	if (TIFFIsTiled( tif ) || TIFFNumberOfStrips( tif ) != 1 || format.min_is_white)
		return set_error_local( error, "unsupported layout of a codestream in a TIFF" );
	const auto size = TIFFRawStripSize64( tif, 0 );
	if (size == 0 || size == (uint64_t)-1 ||
		size > (uint64_t)std::numeric_limits<tmsize_t>::max())
	{
		return set_error_local( error, "unusable strip size" );
	}
	auto bytes = std::vector<uint8_t>( (size_t)size );
	if (TIFFReadRawStrip( tif, 0, bytes.data(), (tmsize_t)size ) != (tmsize_t)size)
		return set_error_local( error, "the strip could not be read" );
	uint16_t compression = 0;
	TIFFGetFieldDefaulted( tif, TIFFTAG_COMPRESSION, &compression );
	const auto codec = compression == COMPRESSION_JXL ? ImgCodec::Jxl : ImgCodec::Jp2k;
	if (sniff_img_codec( bytes.data(), bytes.size() ) != codec)
		return set_error_local( error, "the strip does not contain the declared codec" );
	auto diag = TileDiag{};
	auto checked_dst = TileDst<pixeltype>{};
	checked_dst.reserve = [&] ( int w, int h, int &stride ) -> pixeltype * {
		if (w != format.size.w || h != format.size.h)
			return nullptr;
		return dst.reserve( w, h, stride );
	};
	if (!decode_tile( bytes.data(), bytes.size(), ColorHint::FromStream, checked_dst, diag ))
		return set_error_local( error, "the codestream could not be decoded at the TIFF dimensions" );
	return true;
}


// The current directory of `tif`, decoded into `dst`. Shared by the in-memory
// decode and the page reader so a page decodes exactly as a whole file does.
template <typename pixeltype>
bool decode_current_directory_local ( TIFF *tif, const TileDst<pixeltype> &dst,
	std::string *error )
{
	auto format = TiffSampleFormat{};
	if (!classify_local( tif, &format, error ))
		return false;
	uint16_t compression = 0;
	TIFFGetFieldDefaulted( tif, TIFFTAG_COMPRESSION, &compression );
	if (tiff_compression_is_codec_strip( compression ))
		return decode_codec_strip_local( tif, format, dst, error );

	auto stride = 0;
	auto *out = dst.reserve( format.size.w, format.size.h, stride );
	if (!out)
		return set_error_local( error, "no destination for the decoded image" );

	if (TIFFIsTiled( tif )) {
		uint32_t tw = 0;
		uint32_t th = 0;
		TIFFGetField( tif, TIFFTAG_TILEWIDTH, &tw );
		TIFFGetField( tif, TIFFTAG_TILELENGTH, &th );
		const auto tile_bytes = TIFFTileSize( tif );
		if (tw == 0 || th == 0 || tile_bytes <= 0)
			return set_error_local( error, "unusable tile geometry" );
		auto buffer = std::vector<uint8_t>( (size_t)tile_bytes );
		for (auto y = 0; y < format.size.h; y += (int)th) {
			for (auto x = 0; x < format.size.w; x += (int)tw) {
				const auto tile = TIFFComputeTile( tif, (uint32_t)x, (uint32_t)y, 0, 0 );
				if (TIFFReadEncodedTile( tif, tile, buffer.data(), tile_bytes ) < 0)
					return set_error_local( error, "tile " + std::to_string( tile ) +
						" failed to decode" );
				copy_chunk_local( format, buffer.data(), (int)tw, (int)th, x, y,
					out, stride );
			}
		}
		return true;
	}

	uint32_t rows_per_strip = 0;
	TIFFGetFieldDefaulted( tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip );
	rows_per_strip = std::min<uint32_t>( rows_per_strip, (uint32_t)format.size.h );
	const auto strip_bytes = TIFFStripSize( tif );
	if (rows_per_strip == 0 || strip_bytes <= 0)
		return set_error_local( error, "unusable strip geometry" );
	auto buffer = std::vector<uint8_t>( (size_t)strip_bytes );
	const auto strips = TIFFNumberOfStrips( tif );
	for (auto strip = uint32_t{ 0 }; strip < strips; ++strip) {
		const auto y = (int)( strip * rows_per_strip );
		if (y >= format.size.h)
			break;
		if (TIFFReadEncodedStrip( tif, strip, buffer.data(), strip_bytes ) < 0)
			return set_error_local( error, "strip " + std::to_string( strip ) +
				" failed to decode" );
		copy_chunk_local( format, buffer.data(), format.size.w, (int)rows_per_strip,
			0, y, out, stride );
	}
	return true;
}


TiffPtr open_file_local ( const std::filesystem::path &path, const char *mode = "r" )
{
	silence_libtiff_local();
#ifdef _WIN32
	return TiffPtr( TIFFOpenW( path.wstring().c_str(), mode ) );
#else
	return TiffPtr( TIFFOpen( path.string().c_str(), mode ) );
#endif
}


// The text tags a rewrite carries over. With the geometry, the resolution and the
// orientation they are everything it keeps, so a file with any other optional tag is
// not rewritable (tiff_image_info).
constexpr auto kCarriedTextTags = std::array<uint32_t, 9>{
	TIFFTAG_DOCUMENTNAME, TIFFTAG_IMAGEDESCRIPTION, TIFFTAG_MAKE, TIFFTAG_MODEL,
	TIFFTAG_PAGENAME, TIFFTAG_SOFTWARE, TIFFTAG_DATETIME, TIFFTAG_ARTIST,
	TIFFTAG_HOSTCOMPUTER
};


// Whether the first directory holds anything a rewrite would drop. libtiff lists the
// tags it keeps as custom values, unknown and private ones among them; the few tags
// it stores in fields of its own have to be asked for by name.
bool has_uncarried_tags_local ( TIFF *tif )
{
	for (auto i = 0; i < TIFFGetTagListCount( tif ); ++i) {
		const auto tag = TIFFGetTagListEntry( tif, i );
		if (std::find( kCarriedTextTags.begin(), kCarriedTextTags.end(), tag ) ==
			kCarriedTextTags.end())
		{
			return true;
		}
	}
	auto position = 0.0f;
	uint16_t subifds = 0;
	uint64_t *subifd_offsets = nullptr;
	uint16_t short_value = 0;
	uint16_t second_short = 0;
	uint32_t long_value = 0;
	auto sample_value = 0.0;
	uint16_t *red = nullptr;
	uint16_t *green = nullptr;
	uint16_t *blue = nullptr;
	float *reference = nullptr;
	char *names = nullptr;

// TIFFGetTagListCount enumerates custom values only. These tags live in
// libtiff's dedicated fields and are not copied by the rewrite either.
	return TIFFGetField( tif, TIFFTAG_XPOSITION, &position ) ||
		TIFFGetField( tif, TIFFTAG_YPOSITION, &position ) ||
		TIFFGetField( tif, TIFFTAG_SUBIFD, &subifds, &subifd_offsets ) ||
		TIFFGetField( tif, TIFFTAG_SUBFILETYPE, &long_value ) ||
		TIFFGetField( tif, TIFFTAG_THRESHHOLDING, &short_value ) ||
		TIFFGetField( tif, TIFFTAG_MINSAMPLEVALUE, &short_value ) ||
		TIFFGetField( tif, TIFFTAG_MAXSAMPLEVALUE, &short_value ) ||
		TIFFGetField( tif, TIFFTAG_SMINSAMPLEVALUE, &sample_value ) ||
		TIFFGetField( tif, TIFFTAG_SMAXSAMPLEVALUE, &sample_value ) ||
		TIFFGetField( tif, TIFFTAG_PAGENUMBER, &short_value, &second_short ) ||
		TIFFGetField( tif, TIFFTAG_IMAGEDEPTH, &long_value ) ||
		TIFFGetField( tif, TIFFTAG_TILEDEPTH, &long_value ) ||
		TIFFGetField( tif, TIFFTAG_HALFTONEHINTS, &short_value, &second_short ) ||
		TIFFGetField( tif, TIFFTAG_YCBCRSUBSAMPLING, &short_value, &second_short ) ||
		TIFFGetField( tif, TIFFTAG_YCBCRPOSITIONING, &short_value ) ||
		TIFFGetField( tif, TIFFTAG_REFERENCEBLACKWHITE, &reference ) ||
		TIFFGetField( tif, TIFFTAG_TRANSFERFUNCTION, &red, &green, &blue ) ||
		TIFFGetField( tif, TIFFTAG_COLORMAP, &red, &green, &blue ) ||
		TIFFGetField( tif, TIFFTAG_EXTRASAMPLES, &short_value, &red ) ||
		TIFFGetField( tif, TIFFTAG_INKNAMES, &names ) ||
		TIFFGetField( tif, TIFFTAG_NUMBEROFINKS, &short_value );
}


void copy_carried_tags_local ( TIFF *from, TIFF *to )
{
	for (const auto tag : kCarriedTextTags) {
		const char *text = nullptr;
		if (TIFFGetField( from, tag, &text ) && text)
			TIFFSetField( to, tag, text );
	}
	uint16_t orientation = 0;
	if (TIFFGetField( from, TIFFTAG_ORIENTATION, &orientation ))
		TIFFSetField( to, TIFFTAG_ORIENTATION, orientation );
	uint16_t unit = 0;
	if (TIFFGetField( from, TIFFTAG_RESOLUTIONUNIT, &unit ))
		TIFFSetField( to, TIFFTAG_RESOLUTIONUNIT, unit );
	auto resolution = 0.0f;
	if (TIFFGetField( from, TIFFTAG_XRESOLUTION, &resolution ))
		TIFFSetField( to, TIFFTAG_XRESOLUTION, resolution );
	if (TIFFGetField( from, TIFFTAG_YRESOLUTION, &resolution ))
		TIFFSetField( to, TIFFTAG_YRESOLUTION, resolution );
}


// Row by row through a copy: libtiff's horizontal predictor works in the buffer it
// is given, and `image` is what the caller goes on to compare against.
template <typename pixeltype>
bool write_lzw_local ( TIFF *tif, const Pwhl<pixeltype> &image, std::string *error )
{
	TIFFSetField( tif, TIFFTAG_COMPRESSION, (uint16_t)COMPRESSION_LZW );
	TIFFSetField( tif, TIFFTAG_PREDICTOR, (uint16_t)PREDICTOR_HORIZONTAL );
	auto row = std::vector<pixeltype>( (size_t)image.w );
	for (auto y = 0; y < image.h; ++y) {
		std::memcpy( row.data(), image.rowp( y ), (size_t)image.w * sizeof( pixeltype ) );
		if (TIFFWriteScanline( tif, row.data(), (uint32_t)y, 0 ) < 0)
			return set_error_local( error, "row " + std::to_string( y ) + " could not be written" );
	}
	return true;
}


template <typename pixeltype>
bool write_codec_strip_local ( TIFF *tif, const Pwhl<pixeltype> &image,
	TiffLosslessCodec codec, std::string *error )
{
	const auto is_jxl = codec == TiffLosslessCodec::Jxl;
	auto options = TileEncodeOptions{};
	options.quality = kImageLosslessQuality;
	auto bytes = std::vector<uint8_t>{};
	if (!encode_tile( is_jxl ? ImgCodec::Jxl : ImgCodec::Jp2k, image, &bytes, options, error ))
		return false;
	TIFFSetField( tif, TIFFTAG_COMPRESSION,
		(uint16_t)( is_jxl ? COMPRESSION_JXL : COMPRESSION_JP2000 ) );
	if (TIFFWriteRawStrip( tif, 0, bytes.data(), (tmsize_t)bytes.size() ) < 0)
		return set_error_local( error, "the strip could not be written" );
	return true;
}

} // namespace


bool tiff_compression_is_codec_strip ( uint16_t compression ) noexcept
{
	return compression == COMPRESSION_JXL || compression == COMPRESSION_JP2000;
}


bool tiff_image_info ( const std::filesystem::path &path, TiffImageInfo *out,
	std::string *error )
{
	if (!out)
		return set_error_local( error, "info output is null" );
	auto tif = open_file_local( path );
	if (!tif)
		return set_error_local( error, "not a readable TIFF" );

	auto info = TiffImageInfo{};
	info.pages = (int)TIFFNumberOfDirectories( tif.get() );
	TIFFSetDirectory( tif.get(), 0 );
	auto format = TiffSampleFormat{};
	if (!classify_local( tif.get(), &format, error ))
		return false;
	info.size = format.size;
	info.pixel_type = format.type;
	uint16_t compression = 0;
	TIFFGetFieldDefaulted( tif.get(), TIFFTAG_COMPRESSION, &compression );
	info.rewritable = compression == COMPRESSION_NONE && info.pages == 1 &&
		format.samples_per_pixel == ( is_integer_rgb( format.type ) ? 3 : 1 ) &&
		!has_uncarried_tags_local( tif.get() );

	uint16_t unit = 0;
	float xres = 0.0f;
	float yres = 0.0f;
	if (TIFFGetField( tif.get(), TIFFTAG_RESOLUTIONUNIT, &unit ))
		info.resolution_unit = unit;
	if (TIFFGetField( tif.get(), TIFFTAG_XRESOLUTION, &xres ))
		info.x_resolution = xres;
	if (TIFFGetField( tif.get(), TIFFTAG_YRESOLUTION, &yres ))
		info.y_resolution = yres;
//
// Microns per unit. A unit of "none" states an aspect ratio, not a size.
	const auto per_unit = unit == RESUNIT_CENTIMETER ? 10000.0
		: unit == RESUNIT_INCH ? 25400.0 : 0.0;
	if (per_unit > 0.0 && info.x_resolution > 0.0)
		info.tag_mpp_x = per_unit / info.x_resolution;
	if (per_unit > 0.0 && info.y_resolution > 0.0)
		info.tag_mpp_y = per_unit / info.y_resolution;

//
// The other pages: reduced-resolution images of this one (thumbnails), or
// something else. Headers only.
	for (auto page = 1; page < info.pages; ++page) {
		if (!TIFFSetDirectory( tif.get(), (tdir_t)page )) {
			++info.other_pages;
			continue;
		}
		uint32_t subfile = 0;
		TIFFGetFieldDefaulted( tif.get(), TIFFTAG_SUBFILETYPE, &subfile );
		auto page_format = TiffSampleFormat{};
		if (( subfile & FILETYPE_REDUCEDIMAGE ) == 0 ||
			!classify_local( tif.get(), &page_format, nullptr ) ||
			page_format.size.w >= format.size.w || page_format.size.h >= format.size.h)
		{
			++info.other_pages;
			continue;
		}
		info.reduced.push_back( { page, page_format.size, page_format.type } );
	}
	*out = info;
	return true;
}


template <typename pixeltype>
bool tiff_image_decode ( const uint8_t *bf, size_t len,
	const TileDst<pixeltype> &dst, std::string *error )
{
	if (!bf || len == 0 || !dst.reserve)
		return set_error_local( error, "empty TIFF buffer" );
	silence_libtiff_local();
	auto stream = MemoryStream{ bf, (toff_t)len, 0 };
	auto tif = TiffPtr( TIFFClientOpen( "gallery-member", "rm", &stream,
		memory_read_local, memory_write_local, memory_seek_local,
		memory_close_local, memory_size_local, memory_map_local,
		memory_unmap_local ) );
	if (!tif)
		return set_error_local( error, "libtiff could not open the buffer" );
	return decode_current_directory_local( tif.get(), dst, error );
}


template <typename pixeltype>
bool tiff_image_read_page ( const std::filesystem::path &path, int page,
	const TileDst<pixeltype> &dst, std::string *error )
{
	if (!dst.reserve || page < 0)
		return set_error_local( error, "no destination or a negative page" );
	auto tif = open_file_local( path );
	if (!tif)
		return set_error_local( error, "not a readable TIFF" );
	if (!TIFFSetDirectory( tif.get(), (tdir_t)page ))
		return set_error_local( error, "page " + std::to_string( page ) + " is missing" );
	return decode_current_directory_local( tif.get(), dst, error );
}


template <typename pixeltype>
bool tiff_handle_decode ( ::tiff *tif, const TileDst<pixeltype> &dst, std::string *error )
{
	if (!tif || !dst.reserve)
		return set_error_local( error, "no TIFF or no destination" );
	return decode_current_directory_local( tif, dst, error );
}


template <typename pixeltype>
bool tiff_image_write_lossless ( const std::filesystem::path &path,
	const Pwhl<pixeltype> &image, TiffLosslessCodec codec,
	const std::filesystem::path &tags_from, std::string *error )
{
	if (!image.p || image.w <= 0 || image.h <= 0)
		return set_error_local( error, "empty image" );
	auto source = open_file_local( tags_from );
	if (!source)
		return set_error_local( error, "the original is not a readable TIFF" );
	auto tif = open_file_local( path, "w" );
	if (!tif)
		return set_error_local( error, "the file could not be created" );

	constexpr auto rgb = is_rgb_pixel_v<pixeltype>;
	constexpr auto samples = rgb ? 3 : 1;
	constexpr auto bits = (int)( sizeof( pixeltype ) / samples ) * 8;
	TIFFSetField( tif.get(), TIFFTAG_IMAGEWIDTH, (uint32_t)image.w );
	TIFFSetField( tif.get(), TIFFTAG_IMAGELENGTH, (uint32_t)image.h );
	TIFFSetField( tif.get(), TIFFTAG_SAMPLESPERPIXEL, (uint16_t)samples );
	TIFFSetField( tif.get(), TIFFTAG_BITSPERSAMPLE, (uint16_t)bits );
	TIFFSetField( tif.get(), TIFFTAG_SAMPLEFORMAT, (uint16_t)SAMPLEFORMAT_UINT );
	TIFFSetField( tif.get(), TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG );
	TIFFSetField( tif.get(), TIFFTAG_PHOTOMETRIC,
		(uint16_t)( rgb ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK ) );
	TIFFSetField( tif.get(), TIFFTAG_ROWSPERSTRIP, (uint32_t)image.h );
	copy_carried_tags_local( source.get(), tif.get() );

	const auto written = codec == TiffLosslessCodec::Lzw
		? write_lzw_local( tif.get(), image, error )
		: write_codec_strip_local( tif.get(), image, codec, error );
	if (!written)
		return false;
//
// Close, not just destroy: a full disk first shows up here, and the caller is about
// to replace an original with this file.
	if (!TIFFFlush( tif.get() ))
		return set_error_local( error, "the file could not be completed" );
	return true;
}


template bool tiff_image_decode<Rgb> ( const uint8_t *, size_t,
	const TileDst<Rgb> &, std::string * );
template bool tiff_image_decode<Rgb16> ( const uint8_t *, size_t,
	const TileDst<Rgb16> &, std::string * );
template bool tiff_image_decode<uint8_t> ( const uint8_t *, size_t,
	const TileDst<uint8_t> &, std::string * );
template bool tiff_image_decode<uint16_t> ( const uint8_t *, size_t,
	const TileDst<uint16_t> &, std::string * );
template bool tiff_image_decode<float> ( const uint8_t *, size_t,
	const TileDst<float> &, std::string * );

template bool tiff_image_read_page<Rgb> ( const std::filesystem::path &, int,
	const TileDst<Rgb> &, std::string * );
template bool tiff_image_read_page<Rgb16> ( const std::filesystem::path &, int,
	const TileDst<Rgb16> &, std::string * );
template bool tiff_image_read_page<uint8_t> ( const std::filesystem::path &, int,
	const TileDst<uint8_t> &, std::string * );
template bool tiff_image_read_page<uint16_t> ( const std::filesystem::path &, int,
	const TileDst<uint16_t> &, std::string * );
template bool tiff_image_read_page<float> ( const std::filesystem::path &, int,
	const TileDst<float> &, std::string * );

template bool tiff_handle_decode<Rgb> ( ::tiff *, const TileDst<Rgb> &, std::string * );
template bool tiff_handle_decode<Rgb16> ( ::tiff *, const TileDst<Rgb16> &, std::string * );
template bool tiff_handle_decode<uint8_t> ( ::tiff *, const TileDst<uint8_t> &, std::string * );
template bool tiff_handle_decode<uint16_t> ( ::tiff *, const TileDst<uint16_t> &, std::string * );
template bool tiff_handle_decode<float> ( ::tiff *, const TileDst<float> &, std::string * );

template bool tiff_image_write_lossless<Rgb> ( const std::filesystem::path &,
	const Pwhl<Rgb> &, TiffLosslessCodec, const std::filesystem::path &, std::string * );
template bool tiff_image_write_lossless<Rgb16> ( const std::filesystem::path &,
	const Pwhl<Rgb16> &, TiffLosslessCodec, const std::filesystem::path &, std::string * );
template bool tiff_image_write_lossless<uint8_t> ( const std::filesystem::path &,
	const Pwhl<uint8_t> &, TiffLosslessCodec, const std::filesystem::path &, std::string * );
template bool tiff_image_write_lossless<uint16_t> ( const std::filesystem::path &,
	const Pwhl<uint16_t> &, TiffLosslessCodec, const std::filesystem::path &, std::string * );

} // namespace priml
