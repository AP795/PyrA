// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "dcm_frames.h"

#if USE_DCMTK
#include "pyr_rt.h"

#include <dcmtk/dcmdata/dcistrmf.h>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>

namespace priml {
namespace {

bool fail ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	pyr::plog( "wsi dcm: %s\n", message.c_str() );
	return false;
}


uint16_t le16 ( const uint8_t *p )
{
	return (uint16_t)( p[0] | (uint16_t)p[1] << 8 );
}


uint32_t le32 ( const uint8_t *p )
{
	return (uint32_t)le16( p ) | (uint32_t)le16( p + 2 ) << 16;
}


bool item_header ( std::ifstream &in, uint16_t *element, uint32_t *length )
{
	auto bytes = std::array<uint8_t, 8>{};
	if (!in.read( (char *)bytes.data(), bytes.size() ) || le16( bytes.data() ) != 0xfffe)
		return false;
	*element = le16( bytes.data() + 2 );
	*length = le32( bytes.data() + 4 );
	return true;
}

} // namespace


bool read_dcm_header ( const std::string &path, std::unique_ptr<DcmFileFormat> &file,
	uint64_t *pixel_offset, uint32_t *pixel_length, std::string *error )
{
	auto stream = DcmInputFileStream( path.c_str() );
	if (stream.status().bad())
		return fail( error, path + ": " + stream.status().text() );
	auto loaded = std::make_unique<DcmFileFormat>();
	loaded->transferInit();
	const auto status = loaded->readUntilTag( stream, EXS_Unknown, EGL_noChange, 4096, DCM_PixelData );
	loaded->transferEnd();
	if (status.bad())
		return fail( error, path + ": " + status.text() );
	file = std::move( loaded );
	if (!pixel_offset && !pixel_length)
		return true;
// For supported encapsulated transfers the dataset is explicit little endian.
// readUntilTag stops after the matching tag header, or after a higher tag's
// header if PixelData is absent. Check the actual bytes before trusting tell().
	const auto position = stream.tell();
	if (position < 12)
		return fail( error, path + ": missing pixel-data header" );
	auto in = std::ifstream( path, std::ios::binary );
	in.seekg( position - 12 );
	auto bytes = std::array<uint8_t, 12>{};
	if (!in.read( (char *)bytes.data(), bytes.size() ) ||
		le16( bytes.data() ) != 0x7fe0 || le16( bytes.data() + 2 ) != 0x0010 ||
		bytes[4] != 'O' || ( bytes[5] != 'B' && bytes[5] != 'W' ) || bytes[6] || bytes[7])
	{
		return fail( error, path + ": missing supported explicit pixel-data header" );
	}
	if (pixel_offset)
		*pixel_offset = (uint64_t)position;
	if (pixel_length)
		*pixel_length = le32( bytes.data() + 8 );
	return true;
}


bool index_dcm_frames ( const std::string &path, DcmDataset &dataset,
	uint64_t pixel_offset, uint32_t pixel_length, uint32_t expected,
	std::vector<DcmPixelLocation> *frames, std::string *error )
{
	if (!frames || !expected || pixel_length != 0xffffffff)
		return fail( error, path + ": invalid encapsulated pixel data" );
	auto in = std::ifstream( path, std::ios::binary | std::ios::ate );
	const auto end = in.tellg();
	if (end < 0 || pixel_offset > (uint64_t)end)
		return fail( error, path + ": pixel-data offset exceeds file" );
	in.seekg( (std::streamoff)pixel_offset );
	auto element = uint16_t{};
	auto length = uint32_t{};
	if (!item_header( in, &element, &length ) || element != 0xe000 || length % 4 ||
		( length && length / 4 != expected ))
		return fail( error, path + ": invalid Basic Offset Table" );
	auto basic = std::vector<uint32_t>{};
	if (length) {
		if ((uint64_t)in.tellg() + length > (uint64_t)end)
			return fail( error, path + ": truncated Basic Offset Table" );
		basic.resize( expected );
		for (auto &offset : basic) {
			auto bytes = std::array<uint8_t, 4>{};
			in.read( (char *)bytes.data(), bytes.size() );
			offset = le32( bytes.data() );
		}
	}
	const Uint64 *offsets = nullptr;
	const Uint64 *lengths = nullptr;
	unsigned long count_offsets = 0, count_lengths = 0;
	const auto have_offsets = dataset.tagExists( DCM_ExtendedOffsetTable );
	const auto have_lengths = dataset.tagExists( DCM_ExtendedOffsetTableLengths );
	if (have_offsets != have_lengths || ( have_offsets &&
		( !basic.empty() || dataset.findAndGetUint64Array( DCM_ExtendedOffsetTable, offsets, &count_offsets ).bad() ||
		  dataset.findAndGetUint64Array( DCM_ExtendedOffsetTableLengths, lengths, &count_lengths ).bad() ||
		  !offsets || !lengths || count_offsets != expected || count_lengths != expected ) ))
	{
		return fail( error, path + ": inconsistent Extended Offset Tables" );
	}
	frames->clear();
	frames->reserve( expected );
	const auto first = (uint64_t)in.tellg();
	for (auto frame = 0u; frame < expected; ++frame) {
		const auto item_at = (uint64_t)in.tellg();
		if (!item_header( in, &element, &length ) || element != 0xe000 || !length ||
			length == 0xffffffff || length % 2 || (uint64_t)in.tellg() + length > (uint64_t)end)
		{
			return fail( error, path + ": invalid or truncated frame item" );
		}
		if ((!basic.empty() && basic[frame] != item_at - first) ||
			( offsets && ( offsets[frame] != item_at - first || !lengths[frame] ||
			  lengths[frame] > length || length - lengths[frame] > 1 ) ))
		{
			return fail( error, path + ": frame offset/length table mismatch" );
		}
		frames->push_back( { (uint64_t)in.tellg(), offsets ? (uint32_t)lengths[frame] : length } );
		in.seekg( (std::streamoff)length, std::ios::cur );
	}
	if (!item_header( in, &element, &length ) || element != 0xe0dd || length)
		return fail( error, path + ": unsupported fragmented frame sequence or missing delimiter" );
	return true;
}

bool index_dcm_scene ( const std::string &path, DcmSceneFrame *frame, std::string *error )
{
	if (!frame)
		return fail( error, "null scene frame" );
	auto file = std::unique_ptr<DcmFileFormat>{};
	auto offset = uint64_t{};
	auto length = uint32_t{};
	if (!read_dcm_header( path, file, &offset, &length, error ))
		return false;
	auto &ds = *file->getDataset();
	auto sop = OFString{};
	auto transfer = OFString{};
	file->getMetaInfo()->findAndGetOFString( DCM_TransferSyntaxUID, transfer );
	ds.findAndGetOFString( DCM_SOPClassUID, sop );
	if (sop != UID_VLWholeSlideMicroscopyImageStorage ||
		(transfer != "1.2.840.10008.1.2.4.90" && transfer != "1.2.840.10008.1.2.4.91" &&
		 transfer != "1.2.840.10008.1.2.4.201" && transfer != "1.2.840.10008.1.2.4.202" &&
		 transfer != "1.2.840.10008.1.2.4.203"))
		return fail( error, path + ": unsupported Gallery DICOM SOP class or transfer syntax" );
	auto rows = Uint16{}, columns = Uint16{}, bits = Uint16{}, stored = Uint16{}, samples = Uint16{}, sign = Uint16{};
	auto matrix_w = Uint32{}, matrix_h = Uint32{};
	auto count = Sint32{};
	auto photo = OFString{};
	if (ds.findAndGetUint16( DCM_Rows, rows ).bad() || ds.findAndGetUint16( DCM_Columns, columns ).bad() ||
		ds.findAndGetUint16( DCM_BitsAllocated, bits ).bad() || ds.findAndGetUint16( DCM_BitsStored, stored ).bad() ||
		ds.findAndGetUint16( DCM_SamplesPerPixel, samples ).bad() || ds.findAndGetUint16( DCM_PixelRepresentation, sign ).bad() ||
		ds.findAndGetUint32( DCM_TotalPixelMatrixColumns, matrix_w ).bad() ||
		ds.findAndGetUint32( DCM_TotalPixelMatrixRows, matrix_h ).bad() ||
		ds.findAndGetSint32( DCM_NumberOfFrames, count ).bad() || count != 1 || !rows || !columns ||
		matrix_w != columns || matrix_h != rows || (bits != 8 && bits != 16) || stored != bits || sign ||
		(samples != 1 && samples != 3))
		return fail( error, path + ": Gallery DICOM must contain one unsigned 8/16-bit scene frame" );
	ds.findAndGetOFString( DCM_PhotometricInterpretation, photo );
	if ((samples == 1 && photo != "MONOCHROME2") ||
		(samples == 3 && photo != "YBR_RCT" && photo != "YBR_ICT" && photo != "RGB"))
		return fail( error, path + ": unsupported Gallery DICOM photometric interpretation" );
	auto frames = std::vector<DcmPixelLocation>{};
	if (!index_dcm_frames( path, ds, offset, length, 1, &frames, error ))
		return false;
	auto shared = static_cast<DcmItem *>( nullptr );
	auto measures = static_cast<DcmItem *>( nullptr );
	auto origin = static_cast<DcmItem *>( nullptr );
	auto x = Float64{}, y = Float64{}, sx = Float64{}, sy = Float64{};
	if (ds.findAndGetSequenceItem( DCM_SharedFunctionalGroupsSequence, shared ).bad() ||
		shared->findAndGetSequenceItem( DCM_PixelMeasuresSequence, measures ).bad() ||
		measures->findAndGetFloat64( DCM_PixelSpacing, sy, 0 ).bad() ||
		measures->findAndGetFloat64( DCM_PixelSpacing, sx, 1 ).bad() ||
		ds.findAndGetSequenceItem( DCM_TotalPixelMatrixOriginSequence, origin ).bad() ||
		origin->findAndGetFloat64( DCM_XOffsetInSlideCoordinateSystem, x ).bad() ||
		origin->findAndGetFloat64( DCM_YOffsetInSlideCoordinateSystem, y ).bad() ||
		!std::isfinite( sx ) || sx <= 0 || !std::isfinite( sy ) || std::abs( sx - sy ) > sx * 1e-6 ||
		!std::isfinite( x ) || !std::isfinite( y ))
		return fail( error, path + ": invalid Gallery DICOM calibration" );
	frame->pixels = frames.front();
	frame->size = { columns, rows };
	frame->pixel_type = samples == 1 ? (bits == 8 ? LevelPixelType::Gray8 : LevelPixelType::Gray16)
		: (bits == 8 ? LevelPixelType::Rgb8 : LevelPixelType::Rgb16);
	frame->mpp = sx * 1000.0;
	frame->origin_um = { x * 1000.0, y * 1000.0 };
	return true;
}

} // namespace priml
#endif
