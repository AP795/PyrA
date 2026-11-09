#include "wsi_tif_aux.h"

#include "img_codec.h"
#include "wsi.h"

#include <algorithm>
#include <climits>
#include <vector>

namespace priml {

TiffHandle::~TiffHandle ()
{
	close();
}


bool TiffHandle::open ( const std::string &filename )
{
	close();
	static auto handlers_once = std::once_flag{};
	std::call_once( handlers_once, [] {
		TIFFSetErrorHandler( nullptr );
		TIFFSetWarningHandler( nullptr );
	} );
#ifdef _WIN32
	tif_ = TIFFOpenW( pyr::path_from_utf8( filename ).c_str(), "r" );
#else
	tif_ = TIFFOpen( filename.c_str(), "r" );
#endif
	return tif_ != nullptr;
}


void TiffHandle::close () noexcept
{
	if (tif_) {
		TIFFClose( static_cast<TIFF *>( tif_ ) );
		tif_ = nullptr;
	}
}


bool BuildTiffTileTable ( TIFF *tif, Level &level,
	const Dim2D<uint32_t> &dimensions, const Dim2D<uint32_t> &tile_dimensions,
	size_t &max_tile_bytes )
{
	max_tile_bytes = 0;
	if (dimensions.w == 0 || dimensions.h == 0 ||
		tile_dimensions.w == 0 || tile_dimensions.h == 0 ||
		dimensions.w > INT_MAX || dimensions.h > INT_MAX ||
		tile_dimensions.w > INT_MAX || tile_dimensions.h > INT_MAX)
		return false;
	auto nx = 1 + ( dimensions.w - 1 ) / tile_dimensions.w;
	auto ny = 1 + ( dimensions.h - 1 ) / tile_dimensions.h;
	if ((uint64_t)nx * ny > INT_MAX)
		return false;
	level.tile_w = (int)tile_dimensions.w;
	level.tile_h = (int)tile_dimensions.h;
	level.tiles.reserve( (size_t)nx * ny );
	for (uint32_t y = 0; y < ny; ++y) {
		for (uint32_t x = 0; x < nx; ++x) {
			auto tile = Tile{};
			tile.lr = grid_tile_rect(
				{ (int)dimensions.w, (int)dimensions.h },
				{ (int)tile_dimensions.w, (int)tile_dimensions.h }, (int)x, (int)y );
			auto index = y * nx + x;
			tile.foff = TIFFGetStrileOffset( tif, index );
			tile.fsz = TIFFGetStrileByteCount( tif, index );
			max_tile_bytes = std::max( max_tile_bytes, tile.fsz );
			if (level.hdrbf.size() > INT_MAX ||
				max_tile_bytes > (size_t)INT_MAX - level.hdrbf.size())
				return false;
			level.tiles.push_back( std::move( tile ) );
		}
	}
	set_level_bounds( level );
	return true;
}


PixelMetric ReadTiffPixelMetric ( TIFF *tif )
{
	auto metric = PixelMetric{};
	float xres = 0.0f;
	float yres = 0.0f;
	uint16_t res_unit = 0;
	if (!TIFFGetField( tif, TIFFTAG_RESOLUTIONUNIT, &res_unit ))
		return metric;

	auto scale = 0.0f;
	if (res_unit == RESUNIT_CENTIMETER)
		scale = 10000.0f;
	else if (res_unit == RESUNIT_INCH)
		scale = 25400.0f;
	if (!( scale > 0.0f ))
		return metric;

	if (TIFFGetField( tif, TIFFTAG_XRESOLUTION, &xres ) && xres > 0.0f)
		metric.x = scale / xres;
	if (TIFFGetField( tif, TIFFTAG_YRESOLUTION, &yres ) && yres > 0.0f)
		metric.y = scale / yres;
	return metric;
}


// ---------------------------------------------------------------------------
// Auxiliary (non-pyramidal) IFD decoding - see wsi_tif_aux.h.
//
// Shared by every TIFF-family loader: Hamamatsu writes its macro as a single
// JPEG strip, Aperio writes its label as multi-strip LZW, so both the libjpeg
// dispatch and the TIFFReadRGBAImage fallback have to live in one place.
// ---------------------------------------------------------------------------

// Use the common codec dispatch for a self-contained compressed strip.
// Decode into RGB so the auxiliary-image conversion contract below stays intact.
static bool TryDecodeSingleStripImage ( TIFF *tif, Pwhl<Rgb> &image )
{
	if (TIFFNumberOfStrips( tif ) != 1)
		return false;
	auto raw_size = TIFFRawStripSize( tif, 0 );
	if (raw_size <= 0)
		return false;
	auto raw = std::vector<uint8_t>( (size_t)raw_size );
	auto got = TIFFReadRawStrip( tif, 0, raw.data(), raw_size );
	return got > 0 && decode_to_image( raw.data(), (size_t)got, image );
}


// Auxiliary images use full-range words and unquantised float luminance.
// Tile decoding deliberately has a different byte-to-word conversion contract.
template <typename pixeltype>
static pixeltype AuxiliaryPixelFromRgb ( uint8_t r, uint8_t g, uint8_t b )
{
	if constexpr (std::is_same_v<pixeltype, Rgb>)
		return Rgb{ r, g, b };
	else if constexpr (std::is_same_v<pixeltype, uint8_t>)
		return RgbToLuminance<uint8_t>( r, g, b );
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return (uint16_t)( 257u * RgbToLuminance<uint8_t>( r, g, b ) );
	else
		return RgbToLuminance<float>( r, g, b ) / 255.0f;
}


template <typename pixeltype>
bool LoadStandardIFD ( TIFF *tif, Pwhl<pixeltype> &out ) {
	uint32_t w = 0, h = 0;
	if (!TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &w ) ||
		!TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &h ) || w == 0 || h == 0 ||
		w > INT_MAX || h > INT_MAX)
		return false;

	uint16_t bits_per_sample = 0;
	uint16_t samples_per_pixel = 0;
	uint16_t compression = 0;
	uint16_t photometric = 0;
	bool has_spp = TIFFGetField( tif, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel ) != 0;
	bool has_bps = TIFFGetField( tif, TIFFTAG_BITSPERSAMPLE, &bits_per_sample ) != 0;
	TIFFGetField( tif, TIFFTAG_COMPRESSION, &compression );
	bool has_photometric = TIFFGetField( tif, TIFFTAG_PHOTOMETRIC, &photometric ) != 0;

	if (!TIFFIsTiled( tif ) && has_bps && bits_per_sample == 8 &&
		has_photometric &&
		(photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE) &&
		(!has_spp || samples_per_pixel <= 1))
	{
		std::vector<uint8_t> row( w );
		auto tmp = Pwhl<pixeltype>({
			static_cast<int>( w ), static_cast<int>( h ) });
		for (uint32_t y = 0; y < h; ++y) {
			if (TIFFReadScanline( tif, row.data(), y, 0 ) < 0)
				return false;

			for (uint32_t x = 0; x < w; ++x) {
				auto v = row[x];
				if (photometric == PHOTOMETRIC_MINISWHITE)
					v = 255 - v;

				if constexpr (std::is_same_v<pixeltype, Rgb>) {
					tmp.p[y * tmp.l + x] = Rgb{v, v, v};
				} else if constexpr (std::is_same_v<pixeltype, uint8_t>) {
					tmp.p[y * tmp.l + x] = v;
				} else if constexpr (std::is_same_v<pixeltype, uint16_t>) {
					tmp.p[y * tmp.l + x] = (uint16_t)(257u * (uint16_t)v);
				} else {
					tmp.p[y * tmp.l + x] = v / 255.0f;
				}
			}
		}
		out = std::move( tmp );
		return true;
	}

	if (compression == COMPRESSION_JPEG || compression == COMPRESSION_JXL) {
		auto image = Pwhl<Rgb>{};
		if (TryDecodeSingleStripImage( tif, image )) {
			if constexpr (std::is_same_v<pixeltype, Rgb>)
				out = std::move( image );
			else {
				auto tmp = Pwhl<pixeltype>( { image.w, image.h } );
				for (int y = 0; y < image.h; ++y) {
					for (int x = 0; x < image.w; ++x) {
						auto pixel = image.rowp( y )[x];
						tmp.rowp( y )[x] = AuxiliaryPixelFromRgb<pixeltype>(
							pixel.r, pixel.g, pixel.b );
					}
				}
				out = std::move( tmp );
			}
			return true;
		}
	}

	auto rgba = std::vector<uint32_t>( (size_t)w * h );
	if (TIFFReadRGBAImage( tif, w, h, rgba.data(), 0 )) {
		auto tmp = Pwhl<pixeltype>({
			static_cast<int>( w ), static_cast<int>( h ) });
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				auto p = rgba[(h - 1 - y) * w + x];
				auto r = (uint8_t)TIFFGetR( p );
				auto g = (uint8_t)TIFFGetG( p );
				auto b = (uint8_t)TIFFGetB( p );

				tmp.p[y * tmp.l + x] = AuxiliaryPixelFromRgb<pixeltype>( r, g, b );
			}
		}
		out = std::move( tmp );
		return true;
	}
	return false;
}



//
namespace {

void SetTiffAscii ( TIFF *tif, uint32_t tag, const char *key, Ppyr &pyramid )
{
	const char *value = nullptr;
	if (TIFFGetField( tif, tag, &value ) && value && *value)
		set_property( pyramid, key, value );
}


void SetTiffFloat ( TIFF *tif, uint32_t tag, const char *key, Ppyr &pyramid )
{
	float value = 0.0f;
	if (TIFFGetField( tif, tag, &value ))
		set_property( pyramid, key, std::to_string( value ) );
}

} // namespace


void ReadTiffProperties ( TIFF *tif, Ppyr &pyramid )
{
	SetTiffAscii( tif, TIFFTAG_MAKE, "tiff.Make", pyramid );
	SetTiffAscii( tif, TIFFTAG_MODEL, "tiff.Model", pyramid );
	SetTiffAscii( tif, TIFFTAG_SOFTWARE, "tiff.Software", pyramid );
	SetTiffAscii( tif, TIFFTAG_DATETIME, "tiff.DateTime", pyramid );
	SetTiffAscii( tif, TIFFTAG_IMAGEDESCRIPTION, "tiff.ImageDescription", pyramid );
	SetTiffAscii( tif, TIFFTAG_ARTIST, "tiff.Artist", pyramid );
	SetTiffFloat( tif, TIFFTAG_XRESOLUTION, "tiff.XResolution", pyramid );
	SetTiffFloat( tif, TIFFTAG_YRESOLUTION, "tiff.YResolution", pyramid );

	uint16_t unit = 0;
	if (TIFFGetField( tif, TIFFTAG_RESOLUTIONUNIT, &unit ))
		set_property( pyramid, "tiff.ResolutionUnit", std::to_string( unit ) );
}


void ReadTiffIccProfile ( TIFF *tif, Ppyr &pyramid )
{
	uint32_t length = 0;
	const void *data = nullptr;
	if (!TIFFGetField( tif, TIFFTAG_ICCPROFILE, &length, &data ) ||
		!data || length == 0)
	{
		return;
	}

	const auto *bytes = static_cast<const uint8_t *>( data );
	pyramid.icc_profile.assign( bytes, bytes + length );
	record_icc_properties( pyramid );
}


// LoadStandardIFD is declared in wsi_tif_aux.h and called from the other
// loaders, so the instantiations it is used with live here with the definition.
template bool LoadStandardIFD<Rgb> ( TIFF *, Pwhl<Rgb> & );
template bool LoadStandardIFD<uint8_t> ( TIFF *, Pwhl<uint8_t> & );
template bool LoadStandardIFD<uint16_t> ( TIFF *, Pwhl<uint16_t> & );
template bool LoadStandardIFD<float> ( TIFF *, Pwhl<float> & );

} // namespace priml
