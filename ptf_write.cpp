#include "ptf_write.h"

#include "pyr_rt.h"
#include "tiff_inc.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace priml {

namespace {

bool set_error_local ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	pyr::plog_err( "ptf write: %s\n", message.c_str() );
	return false;
}


uint16_t bits_per_sample_local ( LevelPixelType type ) noexcept
{
	switch (type) {
	case LevelPixelType::Rgb8: return 8;
	case LevelPixelType::Gray8: return 8;
	case LevelPixelType::Gray16: return 16;
	case LevelPixelType::Float32: return 32;
	case LevelPixelType::Rgb16: return 16;
	}
	return 8;
}


uint16_t samples_per_pixel_local ( LevelPixelType type ) noexcept
{
	return is_integer_rgb( type ) ? uint16_t{ 3 } : uint16_t{ 1 };
}


uint16_t sample_format_local ( LevelPixelType type ) noexcept
{
	return type == LevelPixelType::Float32
		? uint16_t{ SAMPLEFORMAT_IEEEFP } : uint16_t{ SAMPLEFORMAT_UINT };
}


uint16_t photometric_local ( LevelPixelType type ) noexcept
{
	return is_integer_rgb( type )
		? uint16_t{ PHOTOMETRIC_RGB } : uint16_t{ PHOTOMETRIC_MINISBLACK };
}


bool compression_tag_local ( ImgCodec codec, uint16_t *tag ) noexcept
{
	if (!tag)
		return false;
	switch (codec) {
	case ImgCodec::Jpeg:
		*tag = COMPRESSION_JPEG;
		return true;
	case ImgCodec::Jp2k:
		*tag = COMPRESSION_JP2000;
		return true;
	case ImgCodec::Jxl:
		*tag = COMPRESSION_JXL;
		return true;
	default:
		return false;
	}
}


// Resolution tags, so a plain TIFF viewer and ReadTiffPixelMetric agree with the
// mpp in the description block. pyr derives Level::metric from the ladder, but the
// base level's metric comes from exactly these tags, so they are not decoration.
void set_resolution_local ( TIFF *tif, double mpp )
{
	if (!( mpp > 0.0 ))
		return;
	TIFFSetField( tif, TIFFTAG_RESOLUTIONUNIT, (uint16_t)RESUNIT_CENTIMETER );
	auto per_cm = (float)( 10000.0 / mpp );
	TIFFSetField( tif, TIFFTAG_XRESOLUTION, per_cm );
	TIFFSetField( tif, TIFFTAG_YRESOLUTION, per_cm );
}

} // namespace


struct PtfWriter::Impl {
	TIFF *tif = nullptr;
	std::string filename;
	bool directory_open = false;
	PtfWriteStats stats;
};


PtfWriter::PtfWriter ()
	: impl_( std::make_unique<Impl>() )
{
}


PtfWriter::~PtfWriter ()
{
	if (impl_ && impl_->tif)
		close( nullptr );
}


const PtfWriteStats &PtfWriter::stats () const
{
	return impl_->stats;
}


bool PtfWriter::open ( const std::string &filename, std::string *error )
{
	if (error)
		error->clear();
	if (impl_->tif)
		return set_error_local( error, "writer is already open" );
	if (filename.empty())
		return set_error_local( error, "output filename is empty" );

	TIFFSetErrorHandler( nullptr );
	TIFFSetWarningHandler( nullptr );

	impl_->tif = TIFFOpen( filename.c_str(), "w8" );
	if (!impl_->tif)
		return set_error_local( error, "cannot create '" + filename + "'" );
	impl_->filename = filename;
	return true;
}


bool PtfWriter::begin_level ( const PtfLevelSpec &spec, std::string *error )
{
	auto *tif = impl_->tif;
	if (!tif)
		return set_error_local( error, "writer is not open" );
	if (!spec.size.is_set())
		return set_error_local( error, "level extent is empty" );
	if (spec.tile.w <= 0 || spec.tile.h <= 0 ||
		spec.tile.w % 16 != 0 || spec.tile.h % 16 != 0)
	{
		return set_error_local( error,
			"tile size must be a positive multiple of 16" );
	}

	const auto type = spec.tags.pixel_type;
	auto compression = uint16_t{};
	if (!compression_tag_local( spec.codec, &compression ))
		return set_error_local( error,
			std::string( "unsupported PTF codec " ) + img_codec_name( spec.codec ) );
	if (spec.codec == ImgCodec::Jpeg &&
		type != LevelPixelType::Rgb8 && type != LevelPixelType::Gray8)
	{
		return set_error_local( error,
			"JPEG PTF levels support only RGB8 and Gray8 pixels" );
	}
	if (spec.codec == ImgCodec::Jpeg &&
		spec.encode.quality == kImageLosslessQuality)
	{
		return set_error_local( error, "JPEG has no lossless encoding mode" );
	}
	if (spec.codec == ImgCodec::Jp2k && type == LevelPixelType::Float32)
		return set_error_local( error,
			"JPEG 2000 PTF levels do not support Float32 pixels" );
	TIFFSetField( tif, TIFFTAG_IMAGEWIDTH, (uint32_t)spec.size.w );
	TIFFSetField( tif, TIFFTAG_IMAGELENGTH, (uint32_t)spec.size.h );
	TIFFSetField( tif, TIFFTAG_TILEWIDTH, (uint32_t)spec.tile.w );
	TIFFSetField( tif, TIFFTAG_TILELENGTH, (uint32_t)spec.tile.h );
	TIFFSetField( tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample_local( type ) );
	TIFFSetField( tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel_local( type ) );
	TIFFSetField( tif, TIFFTAG_SAMPLEFORMAT, sample_format_local( type ) );
	TIFFSetField( tif, TIFFTAG_PHOTOMETRIC, photometric_local( type ) );
	TIFFSetField( tif, TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG );
	TIFFSetField( tif, TIFFTAG_ORIENTATION, (uint16_t)ORIENTATION_TOPLEFT );
//
// Every tile goes out through TIFFWriteRawTile, and on the way back in the
// payload is identified by its own magic bytes. The tag still names the actual
// payload so metadata readers do not report a different codec.
	TIFFSetField( tif, TIFFTAG_COMPRESSION, compression );
	set_resolution_local( tif, spec.tags.mpp );

	auto description = format_ptf_tags( spec.tags );
	TIFFSetField( tif, TIFFTAG_IMAGEDESCRIPTION, description.c_str() );

	impl_->directory_open = true;
	return true;
}


bool PtfWriter::write_raw_tile ( int tile_index, const void *bytes, size_t len,
	std::string *error )
{
	auto written = TIFFWriteRawTile( impl_->tif, (uint32_t)tile_index,
		const_cast<void *>( bytes ), (tmsize_t)len );
	if (written < 0 || (size_t)written != len) {
		return set_error_local( error,
			"TIFFWriteRawTile failed for tile " + std::to_string( tile_index ) );
	}
	impl_->stats.tiles_written += 1;
	impl_->stats.encoded_bytes += (long long)len;
	return true;
}


bool PtfWriter::end_directory ( std::string *error )
{
	if (!TIFFWriteDirectory( impl_->tif ))
		return set_error_local( error, "TIFFWriteDirectory failed" );
	impl_->directory_open = false;
	return true;
}


template <typename pixeltype>
bool PtfWriter::add_level ( const PtfLevelSpec &spec,
	const PtfTileFetch<pixeltype> &fetch, std::string *error )
{
	if (error)
		error->clear();
	if (!fetch)
		return set_error_local( error, "tile fetch callback is null" );
	if (spec.tags.pixel_type != pixel_type_of<pixeltype>::value) {
		return set_error_local( error,
			std::string( "level declares " ) +
			std::string( ptf_pixel_type_name( spec.tags.pixel_type ) ) +
			" but is being written as " +
			std::string( ptf_pixel_type_name( pixel_type_of<pixeltype>::value ) ) );
	}
	if (!begin_level( spec, error ))
		return false;

	const auto nx = ( spec.size.w + spec.tile.w - 1 ) / spec.tile.w;
	const auto ny = ( spec.size.h + spec.tile.h - 1 ) / spec.tile.h;
	auto encoded = std::vector<uint8_t>{};
	auto tile = Pwhl<pixeltype>{};

	for (auto ty = 0; ty < ny; ++ty) {
		for (auto tx = 0; tx < nx; ++tx) {
			const auto rect = grid_tile_rect( spec.size, spec.tile, tx, ty );
			const auto dims = Dim2D<int>{ width( rect ), height( rect ) };

			if (tile.w != dims.w || tile.h != dims.h) {
				tile = Pwhl<pixeltype>( dims );
			} else {
//
// Cleared, not merely reused. A fetch is allowed to fill only the part of the
// tile it has pixels for - which on a sparse mosaic is most edge tiles - and
// whatever it leaves alone must read as background, not as the previous tile's
// pixels. Reusing the allocation is worth keeping; reusing its contents is a bug
// that shows up as smeared copies of neighbouring fields in the gaps.
				std::fill( tile.p, tile.p + (size_t)tile.l * tile.h, pixeltype{} );
			}

			if (!fetch( rect, &tile )) {
//
// Declined, not failed. The tile index is simply never written, so its entry in
// the offset and byte-count tables stays zero - which is what an empty region of
// a sparse mosaic is, both here and in the loader.
				impl_->stats.tiles_empty += 1;
				continue;
			}
			if (tile.w != dims.w || tile.h != dims.h) {
				return set_error_local( error,
					"tile fetch returned " + std::to_string( tile.w ) + "x" +
					std::to_string( tile.h ) + ", expected " +
					std::to_string( dims.w ) + "x" + std::to_string( dims.h ) );
			}

			auto codec_error = std::string{};
			if (!encode_tile( spec.codec, tile, &encoded, spec.encode, &codec_error ))
				return set_error_local( error, "tile encode failed: " + codec_error );
			if (encoded.empty())
				return set_error_local( error, "tile encode produced no bytes" );
			if (!write_raw_tile( ty * nx + tx, encoded.data(), encoded.size(), error ))
				return false;
		}
	}

	if (!end_directory( error ))
		return false;
	impl_->stats.levels_written += 1;
	return true;
}


bool PtfWriter::add_aux ( const std::string &name, const Pwhl<Rgb> &image,
	std::string *error )
{
	if (error)
		error->clear();
	auto *tif = impl_->tif;
	if (!tif)
		return set_error_local( error, "writer is not open" );
	if (name.empty())
		return set_error_local( error, "auxiliary image name is empty" );
	if (!image.is_set() || !image.p)
		return set_error_local( error, "auxiliary image '" + name + "' is empty" );

//
// Stripped and Deflate-compressed rather than tiled and JXL: these are read by
// LoadStandardIFD through libtiff's own decoders, not through the magic-byte tile
// path, so they have to stay within what libtiff can decompress on its own.
	TIFFSetField( tif, TIFFTAG_IMAGEWIDTH, (uint32_t)image.w );
	TIFFSetField( tif, TIFFTAG_IMAGELENGTH, (uint32_t)image.h );
	TIFFSetField( tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)8 );
	TIFFSetField( tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3 );
	TIFFSetField( tif, TIFFTAG_SAMPLEFORMAT, (uint16_t)SAMPLEFORMAT_UINT );
	TIFFSetField( tif, TIFFTAG_PHOTOMETRIC, (uint16_t)PHOTOMETRIC_RGB );
	TIFFSetField( tif, TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG );
	TIFFSetField( tif, TIFFTAG_ORIENTATION, (uint16_t)ORIENTATION_TOPLEFT );
	TIFFSetField( tif, TIFFTAG_COMPRESSION, (uint16_t)COMPRESSION_ADOBE_DEFLATE );
	TIFFSetField( tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)image.h );
//
// SUBFILETYPE_REDUCEDIMAGE is what keeps a reader that ignores our description
// block from mistaking the label for a pyramid level.
	TIFFSetField( tif, TIFFTAG_SUBFILETYPE, (uint32_t)FILETYPE_REDUCEDIMAGE );

	auto tags = PtfPlaneTags{};
	tags.valid = true;
	tags.role = std::string( kPtfRoleAux );
	tags.aux_name = name;
	tags.pixel_type = LevelPixelType::Rgb8;
	auto description = format_ptf_tags( tags );
	TIFFSetField( tif, TIFFTAG_IMAGEDESCRIPTION, description.c_str() );

	auto row = std::vector<uint8_t>( (size_t)image.w * 3 );
	for (auto y = 0; y < image.h; ++y) {
		const auto *src = image.rowp( y );
		for (auto x = 0; x < image.w; ++x) {
			row[(size_t)x * 3 + 0] = src[x].r;
			row[(size_t)x * 3 + 1] = src[x].g;
			row[(size_t)x * 3 + 2] = src[x].b;
		}
		if (TIFFWriteScanline( tif, row.data(), (uint32_t)y, 0 ) < 0) {
			return set_error_local( error,
				"TIFFWriteScanline failed on auxiliary image '" + name + "'" );
		}
	}

	if (!end_directory( error ))
		return false;
	impl_->stats.aux_written += 1;
	return true;
}


bool PtfWriter::close ( std::string *error )
{
	if (error)
		error->clear();
	if (!impl_->tif)
		return true;
	if (impl_->directory_open) {
//
// A directory left open means an add_level bailed out midway. Closing here would
// append a truncated IFD and produce a file that opens and is wrong, which is
// worse than one that does not open.
		TIFFClose( impl_->tif );
		impl_->tif = nullptr;
		impl_->directory_open = false;
		return set_error_local( error,
			"closing with an unfinished directory - '" + impl_->filename +
			"' is incomplete" );
	}

	TIFFClose( impl_->tif );
	impl_->tif = nullptr;
	pyr::plog(
		"ptf write: '%s' closed - %d level(s), %d aux, %lld tile(s) written, "
		"%lld empty, %lld encoded bytes\n",
		impl_->filename.c_str(), impl_->stats.levels_written,
		impl_->stats.aux_written, impl_->stats.tiles_written,
		impl_->stats.tiles_empty, impl_->stats.encoded_bytes );
	return true;
}


template bool PtfWriter::add_level<Rgb> ( const PtfLevelSpec &,
	const PtfTileFetch<Rgb> &, std::string * );
template bool PtfWriter::add_level<uint8_t> ( const PtfLevelSpec &,
	const PtfTileFetch<uint8_t> &, std::string * );
template bool PtfWriter::add_level<uint16_t> ( const PtfLevelSpec &,
	const PtfTileFetch<uint16_t> &, std::string * );
template bool PtfWriter::add_level<float> ( const PtfLevelSpec &,
	const PtfTileFetch<float> &, std::string * );
template bool PtfWriter::add_level<Rgb16> ( const PtfLevelSpec &,
	const PtfTileFetch<Rgb16> &, std::string * );

} // namespace priml
