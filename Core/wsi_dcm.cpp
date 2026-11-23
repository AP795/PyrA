// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "build_config.h"

#if USE_DCMTK

// DICOM VL Whole Slide Microscopy loader.
//
// A DICOM slide is a *folder* of .dcm files that share one SeriesInstanceUID.
// Each file is a separate object; its role is given by ImageType[2] (0008,0008):
//   VOLUME      -> one pyramid level (a TILED_FULL or TILED_SPARSE grid of
//                  compressed frames)
//   LABEL       -> slide label image   (aux "frost")
//   OVERVIEW    -> whole slide          (aux "macro")
//   REGIONLOCALIZER -> overview fallback
//
// Frames are stored as one fragment each in the encapsulated PixelData. We use
// DCMTK only to parse the structure and hand the raw fragment bytes to the
// shared codec layer (img_codec.*), which identifies the encoding from its own
// magic bytes - the same path SVS/NDPI/TIFF tiles take. DCMTK's codecs are not
// linked. A new tile encoding is therefore added in img_codec.cpp; only the
// transfer-syntax gate below has to learn its UID.

#include "pyr_win.h"		// winsock2 + windows in the order DCMTK needs
#include "dcm_frames.h"
#include "dcm_series.h"
#include "dcm_write.h"
#include "img_codec.h"
#include "wsi.h"
#include "wsi_formats.h"

#include "pyr_rt.h"

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcpixseq.h"
#include "dcmtk/dcmdata/dcpxitem.h"

#if PACKAGE_VERSION_NUMBER < 370
#error "DICOM HTJ2K support requires DCMTK 3.7.0 or newer"
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#if PYR_MSVC_AUTOLINK
#pragma comment(lib, "dcmdata.lib")
#pragma comment(lib, "oflog.lib")
#pragma comment(lib, "ofstd.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace priml {

namespace fs = std::filesystem;

// Immutable per-level codestream expectations. Pixel payloads remain in the file.
struct DcmState {
	struct LevelH {
		bool htj2k = false;
		unsigned channels = 0;
		unsigned bits = 0;
	};
	std::map<std::string, LevelH> levels;
};


struct DcmPendingLevel {
	Level level;
	PixelMetric source_metric;
	DcmState::LevelH decoding;
};


// Identifying data elements, as dicom.<Keyword>.
//
// The general-equipment and container attributes only - not a dump of the whole
// dataset, which for a WSI object runs to thousands of per-frame items.
static void RecordDcmProperties ( DcmItem *ds, Ppyr &pyramid )
{
	if (!ds)
		return;

	struct Wanted {
		const DcmTagKey &tag;
		const char *key;
	};
	const Wanted wanted[] = {
		{ DCM_Manufacturer, "dicom.Manufacturer" },
		{ DCM_ManufacturerModelName, "dicom.ManufacturerModelName" },
		{ DCM_DeviceSerialNumber, "dicom.DeviceSerialNumber" },
		{ DCM_SoftwareVersions, "dicom.SoftwareVersions" },
		{ DCM_ContainerIdentifier, "dicom.ContainerIdentifier" },
		{ DCM_StudyInstanceUID, "dicom.StudyInstanceUID" },
		{ DCM_SeriesInstanceUID, "dicom.SeriesInstanceUID" },
		{ DCM_AcquisitionDateTime, "dicom.AcquisitionDateTime" },
		{ DCM_Modality, "dicom.Modality" },
	};
	for (const auto &item : wanted) {
		OFString value;
		if (ds->findAndGetOFString( item.tag, value ).good() && !value.empty())
			set_property( pyramid, item.key, std::string( value.c_str() ) );
	}

	OFString objective;
	DcmItem *optical = nullptr;
	if (ds->findAndGetSequenceItem( DCM_OpticalPathSequence, optical, 0 ).good() &&
		optical &&
		optical->findAndGetOFString( DCM_ObjectiveLensPower, objective ).good() &&
		!objective.empty())
	{
		set_property( pyramid, "pyra.objective-power",
			std::string( objective.c_str() ) );
	}
}


// The ICC profile, which for a WSI object lives in the optical path rather than
// at the top level - one per optical path, and we take the first.
//
// Recorded, never applied; see record_icc_properties.
static void ReadDcmIccProfile ( DcmItem *ds, Ppyr &pyramid )
{
	if (!ds || !pyramid.icc_profile.empty())
		return;

	DcmItem *optical = nullptr;
	if (!ds->findAndGetSequenceItem( DCM_OpticalPathSequence, optical, 0 ).good() ||
		!optical)
	{
		return;
	}

	const Uint8 *data = nullptr;
	unsigned long count = 0;
	if (!optical->findAndGetUint8Array( DCM_ICCProfile, data, &count ).good() ||
		!data || count == 0)
	{
		return;
	}

	pyramid.icc_profile.assign( data, data + count );
	record_icc_properties( pyramid );
}


// Slide-label barcode text read straight from DICOM data elements (no image
// decode). Tried in order of reliability; first non-empty value wins. Most WSI
// scanners write the barcode into ContainerIdentifier (0040,0512).
static std::string ReadBarcodeTags ( DcmItem *ds ) {
	OFString v;
	if (ds->findAndGetOFString( DCM_ContainerIdentifier, v ).good() && !v.empty())
		return std::string( v.c_str() );
	if (ds->findAndGetOFString( DCM_BarcodeValue, v ).good() && !v.empty())
		return std::string( v.c_str() );

	DcmItem *spec = nullptr;
	if (ds->findAndGetSequenceItem( DCM_SpecimenDescriptionSequence, spec, 0 ).good() && spec
		&& spec->findAndGetOFString( DCM_SpecimenIdentifier, v ).good() && !v.empty())
		return std::string( v.c_str() );

	if (ds->findAndGetOFString( DCM_SpecimenShortDescription, v ).good() && !v.empty())
		return std::string( v.c_str() );

	return {};
}


// ImageType[2] role for a WSI object: "VOLUME", "LABEL", "OVERVIEW", ...
static std::string ImageRole ( DcmItem *ds ) {
	OFString role;
	if (ds->findAndGetOFString( DCM_ImageType, role, 2 ).good())
		return std::string( role.c_str() );
	return {};
}


static bool IsHtj2kTransfer ( E_TransferSyntax transfer ) noexcept
{
	return transfer == EXS_HighThroughputJPEG2000LosslessOnly ||
		transfer == EXS_HighThroughputJPEG2000withRPCLOptionsLosslessOnly ||
		transfer == EXS_HighThroughputJPEG2000;
}


static bool ReadHtj2kPixelType ( DcmItem *ds, LevelPixelType *type,
	int *maximum, unsigned *channels, unsigned *precision, std::string *error, bool strict_htj2k = true )
{
	auto invalid = [&] ( const char *message ) {
		if (error)
			*error = message;
		return false;
	};
	Uint16 spp = 0, allocated = 0, stored = 0, high = 0, representation = 0, planar = 0;
	OFString photometric;
	if (ds->findAndGetUint16( DCM_SamplesPerPixel, spp ).bad() ||
		ds->findAndGetUint16( DCM_BitsAllocated, allocated ).bad() ||
		ds->findAndGetUint16( DCM_BitsStored, stored ).bad() ||
		ds->findAndGetUint16( DCM_HighBit, high ).bad() ||
		ds->findAndGetUint16( DCM_PixelRepresentation, representation ).bad() ||
		ds->findAndGetOFString( DCM_PhotometricInterpretation, photometric ).bad())
		return invalid( "HTJ2K pixel metadata is incomplete" );
	if ((spp != 1 && spp != 3) || (allocated != 8 && allocated != 16) ||
		stored == 0 || stored > allocated || high + 1 != stored || representation != 0)
		return invalid( "HTJ2K requires unsigned 8/16-bit gray or RGB samples with consistent bit metadata" );
	if (spp == 1 && photometric != "MONOCHROME2")
		return invalid( "HTJ2K gray input requires MONOCHROME2" );
	if (spp == 3 && (photometric != "RGB" && photometric != "YBR_RCT" && photometric != "YBR_ICT" &&
		( strict_htj2k || ( photometric != "YBR_FULL" && photometric != "YBR_FULL_422" ) )))
		return invalid( "HTJ2K supports RGB or an in-stream YBR_RCT/YBR_ICT transform" );
	if (spp == 3 && (ds->findAndGetUint16( DCM_PlanarConfiguration, planar ).bad() || planar != 0))
		return invalid( "HTJ2K colour input requires PlanarConfiguration=0" );
	*type = spp == 1 ? (stored <= 8 ? LevelPixelType::Gray8 : LevelPixelType::Gray16)
		: (stored <= 8 ? LevelPixelType::Rgb8 : LevelPixelType::Rgb16);
	*maximum = (1 << stored) - 1;
	*channels = spp;
	*precision = stored;
	return true;
}


// HTJ2K mandates one fragment per frame (PS3.5 A.4.4). Validate offset tables
// against fragment lengths without materializing compressed frame bodies.
static bool ValidateHtj2kFrameTable ( DcmItem *ds,
	const std::vector<DcmPixelItem *> &frags, size_t count )
{
	if (frags.size() < 2 || count == 0 || count != frags.size() - 1)
		return false;
	const Uint64 *extended = nullptr, *lengths = nullptr;
	unsigned long extended_count = 0, lengths_count = 0;
	const auto has_extended = ds->tagExists( DCM_ExtendedOffsetTable );
	Uint8 *basic = nullptr;
	if (has_extended) {
		if (ds->findAndGetUint64Array( DCM_ExtendedOffsetTable, extended, &extended_count ).bad() ||
			!extended || extended_count != count || frags[0]->getLength() != 0 ||
			ds->findAndGetUint64Array( DCM_ExtendedOffsetTableLengths, lengths, &lengths_count ).bad() ||
			!lengths || lengths_count != count)
			return false;
	} else if (frags[0]->getLength() != 0) {
		if (count > std::numeric_limits<size_t>::max() / 4 ||
			frags[0]->getLength() != count * 4 || frags[0]->getUint8Array( basic ).bad() || !basic)
			return false;
	}
	auto offset = uint64_t{};
	for (auto i = size_t{}; i < count; ++i) {
		const auto length = (uint64_t)frags[i + 1]->getLength();
		if (!length)
			return false;
		if (has_extended) {
			if (extended[i] != offset || !lengths[i] || lengths[i] > length || length - lengths[i] > 1)
				return false;
		} else if (basic) {
			const auto *p = basic + i * 4;
			const auto value = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
			if (value != offset)
				return false;
		}
		offset += 8ull + length;
	}
	return true;
}

enum class DcmColourDepth { Rgb8, Rgb16, Invalid };


// What a VOLUME's frames decode to. Deep metadata is accepted only when its
// container fields agree with one another and the transfer syntax can carry
// that precision; otherwise an apparently valid RGB16 level would be scaled by
// a range different from the codestream's.
static DcmColourDepth ReadDcmColourDepth ( DcmItem *ds,
	E_TransferSyntax transfer, int *max_pixel_value, std::string *reason = nullptr )
{
	Uint16 spp = 0, bits_allocated = 0, bits_stored = 0, high_bit = 0;
	ds->findAndGetUint16( DCM_SamplesPerPixel, spp );
	ds->findAndGetUint16( DCM_BitsStored, bits_stored );
	if (spp != 3 || bits_stored <= 8)
		return DcmColourDepth::Rgb8;
	auto invalid = [&] ( const char *message ) {
		if (reason)
			*reason = message;
		return DcmColourDepth::Invalid;
	};
	if (bits_stored > 16 ||
		ds->findAndGetUint16( DCM_BitsAllocated, bits_allocated ).bad() ||
		ds->findAndGetUint16( DCM_HighBit, high_bit ).bad())
	{
		return invalid( "invalid or incomplete deep-colour bit metadata" );
	}
	if (bits_allocated != 16 || high_bit + 1 != bits_stored)
		return invalid( "BitsAllocated/BitsStored/HighBit are inconsistent" );
	if (transfer == EXS_JPEGProcess1)
		return invalid( "baseline JPEG cannot carry samples deeper than 8 bits" );
	if (transfer == EXS_JPEGProcess2_4 && bits_stored > 12)
		return invalid( "JPEG Extended cannot carry samples deeper than 12 bits" );
	if (transfer != EXS_JPEGProcess2_4 &&
		transfer != EXS_JPEG2000LosslessOnly && transfer != EXS_JPEG2000)
	{
		return invalid( "transfer syntax cannot carry supported deep colour" );
	}
	if (max_pixel_value)
		*max_pixel_value = ( 1 << bits_stored ) - 1;
	return DcmColourDepth::Rgb16;
}


// X/Y microns-per-pixel from PixelSpacing (mm), tried in two locations:
//   1. SharedFunctionalGroups > PixelMeasures > PixelSpacing (3dhistech style)
//   2. top-level PixelSpacing element (Leica style)
// PixelSpacing order is row (Y), column (X). Falls back to the imaged-volume
// dimensions divided by the total pixel matrix dimensions.
static PixelMetric ReadPixelMetric ( DcmDataset *ds, int total_w, int total_h ) {
	auto read_spacing = [] ( DcmItem *item ) {
		auto metric = PixelMetric{};
		auto row = OFString{};
		auto column = OFString{};
		if (item && item->findAndGetOFString( DCM_PixelSpacing, row, 0 ).good() &&
			item->findAndGetOFString( DCM_PixelSpacing, column, 1 ).good())
		{
			auto row_mm = (float)atof( row.c_str() );
			auto column_mm = (float)atof( column.c_str() );
			if (row_mm > 0.0f)
				metric.y = row_mm * 1000.0f;
			if (column_mm > 0.0f)
				metric.x = column_mm * 1000.0f;
		}
		return metric;
	};

	DcmItem *shared = nullptr;
	if (ds->findAndGetSequenceItem( DCM_SharedFunctionalGroupsSequence, shared, 0 ).good()
		&& shared) {
		DcmItem *pm = nullptr;
		if (shared->findAndGetSequenceItem( DCM_PixelMeasuresSequence, pm, 0 ).good()) {
			auto metric = read_spacing( pm );
			if (metric.x > 0.0f || metric.y > 0.0f)
				return metric;
		}
	}

	auto metric = read_spacing( ds );
	if (metric.x > 0.0f || metric.y > 0.0f)
		return metric;

	Float32 vol_w = 0.0f;
	if (total_w > 0 && ds->findAndGetFloat32( DCM_ImagedVolumeWidth, vol_w ).good()
		&& vol_w > 0.0f)
		metric.x = (vol_w / (float)total_w) * 1000.0f;
	Float32 vol_h = 0.0f;
	if (total_h > 0 && ds->findAndGetFloat32( DCM_ImagedVolumeHeight, vol_h ).good()
		&& vol_h > 0.0f)
		metric.y = (vol_h / (float)total_h) * 1000.0f;

	return metric;
}


// Open a .dcm metadata-only (PixelData deferred via small maxReadLength).
static bool LoadMeta ( const std::string &path, std::unique_ptr<DcmFileFormat> &out )
{
	return read_dcm_header( path, out );
}


// Decode native (uncompressed) 8-bit pixel data into an owning image. Handles
// the common single-frame aux-image cases: RGB (3 samples, interleaved or
// planar) and grayscale (1 sample). Used for aux images stored in a
// non-encapsulated transfer syntax (e.g. Leica's LittleEndian LABEL).
template <typename pixeltype>
static bool DecodeNativeImage ( DcmDataset *ds, DcmElement *elem,
	Pwhl<pixeltype> &out ) {
	Uint16 rows = 0, cols = 0, spp = 1, bits = 0, planar = 0;
	ds->findAndGetUint16( DCM_Rows, rows );
	ds->findAndGetUint16( DCM_Columns, cols );
	ds->findAndGetUint16( DCM_SamplesPerPixel, spp );
	ds->findAndGetUint16( DCM_BitsAllocated, bits );
	ds->findAndGetUint16( DCM_PlanarConfiguration, planar );
	if (rows == 0 || cols == 0 || bits != 8 || (spp != 1 && spp != 3))
		return false;

	Uint8 *data = nullptr;
	if (elem->getUint8Array( data ).bad() || !data)
		return false;
	auto len = (size_t)elem->getLength();
	auto npix = (size_t)rows * (size_t)cols;
	if (len < npix * (size_t)spp)
		return false;

	out = Pwhl<pixeltype>({  (int)cols, (int)rows  });
	if (!out.p)
		return false;

	for (int y = 0; y < (int)rows; ++y) {
		auto *dst = out.p + (size_t)y * out.l;
		for (int x = 0; x < (int)cols; ++x) {
			auto i = (size_t)y * (size_t)cols + (size_t)x;
			uint8_t r, g, b;
			if (spp == 1) {
				r = g = b = data[i];
			} else if (planar == 0) {		// R0G0B0 R1G1B1 ... (interleaved)
				r = data[i * 3 + 0];
				g = data[i * 3 + 1];
				b = data[i * 3 + 2];
			} else {						// RR..GG..BB.. (planar)
				r = data[i];
				g = data[npix + i];
				b = data[2 * npix + i];
			}
			dst[x] = PixelFromRgbBytes<pixeltype>( r, g, b );
		}
	}
	return true;
}


// Decode a single-frame aux image (LABEL/OVERVIEW) into an owning image.
// Encapsulated pixel data -> JPEG fragment via the shared jpegturbo path;
// native (uncompressed) pixel data -> direct copy.
template <typename pixeltype>
static bool DecodeAuxImage ( DcmFileFormat &ff, Pwhl<pixeltype> &out ) {
	auto *ds = ff.getDataset();
	DcmElement *elem = nullptr;
	if (ds->findAndGetElement( DCM_PixelData, elem ).bad() || !elem)
		return false;
	auto *pixel_data = OFstatic_cast( DcmPixelData *, elem );
	DcmPixelSequence *seq = nullptr;
	const DcmRepresentationParameter *param = nullptr;
	if (pixel_data->getEncapsulatedRepresentation(
			ds->getOriginalXfer(), param, seq ).bad() || !seq)
		return DecodeNativeImage<pixeltype>( ds, elem, out );	// uncompressed
	if (IsHtj2kTransfer( ds->getOriginalXfer() )) {
		auto type = LevelPixelType::Rgb8;
		auto maximum = 0;
		auto channels = 0u, precision = 0u;
		auto error = std::string{};
		if (!ReadHtj2kPixelType( ds, &type, &maximum, &channels, &precision, &error ))
			return false;
		long frame_count = 1;
		ds->findAndGetLongInt( DCM_NumberOfFrames, frame_count );
		if (frame_count != 1)
			return false;
		auto frags = std::vector<DcmPixelItem *>{};
		for (auto *object = seq->nextInContainer( nullptr ); object; object = seq->nextInContainer( object ))
			frags.push_back( OFstatic_cast( DcmPixelItem *, object ) );
		if (!ValidateHtj2kFrameTable( ds, frags, 1 ))
			return false;
		Uint8 *data = nullptr;
		if (frags[1]->getUint8Array( data ).bad() || !data)
			return false;
		const auto len = (size_t)frags[1]->getLength();
		if (len < 4 || data[0] != 0xff || data[1] != 0x4f)
			return false;
		Uint16 width = 0, height = 0;
		ds->findAndGetUint16( DCM_Columns, width );
		ds->findAndGetUint16( DCM_Rows, height );
		auto info = ImgCodecInfo{};
		if (!read_img_codec_info( data, len, &info, &error ) ||
			info.codec != ImgCodec::Htj2k || info.width != width || info.height != height ||
			info.color_channels != (int)channels || info.bits_per_sample != (int)precision)
			return false;
		return decode_to_image<pixeltype>( data, len, out );
	}

	if (seq->card() < 2)
		return false;

	DcmPixelItem *frag = nullptr;
	if (seq->getItem( frag, 1 ).bad() || !frag)		// item 0 = BOT, 1 = first frame
		return false;
	Uint8 *data = nullptr;
	if (frag->getUint8Array( data ).bad() || !data)
		return false;
	auto len = (size_t)frag->getLength();
	if (len == 0)
		return false;

	return decode_to_image<pixeltype>( data, len, out );
}


// Build the grid-cell -> 0-based-frame map for one VOLUME level (row-major,
// size across*down; -1 = hole).
//   TILED_FULL:   implicit row-major order (cell i holds frame i).
//   TILED_SPARSE: each frame's PlanePositionSlide carries its 1-based top-left
//                 pixel in the total matrix; convert to a grid cell. Positions
//                 are expected tile-aligned (every known WSI writer does this);
//                 a non-aligned frame is logged and skipped, never misplaced.
DcmLoader::DcmLoader () : state_( std::make_unique<DcmState>() ) {}


DcmLoader::~DcmLoader () = default;


static bool HandleDcmAuxiliaryImageLocal ( DcmDataset *dataset,
	DcmFileFormat &file,
	const std::string &role,
	Ppyr *pyramid,
	bool *have_overview,
	bool *have_frost,
	int *overview_priority )
{
//
// Metadata comes off whichever instance we happen to see first; set_property
// keeps the first non-empty value, and every instance of one series carries the
// same equipment and container attributes.
	RecordDcmProperties( dataset, *pyramid );
	ReadDcmIccProfile( dataset, *pyramid );

	if (pyramid->barcode.empty()) {
		auto barcode = ReadBarcodeTags( dataset );
		if (!barcode.empty()) {
			pyramid->barcode = std::move( barcode );
			pyramid->barcode_from_metadata = true;
		}
	}
	if (role == "LABEL") {
		if (!*have_frost && DecodeAuxImage( file, aux_slot( *pyramid, kAuxFrost ) ))
			*have_frost = true;
		return true;
	}
	if (role == "OVERVIEW" || role == "REGIONLOCALIZER") {
		auto priority = role == "OVERVIEW" ? 2 : 1;
		if (priority > *overview_priority) {
			auto image = Pwhl<Rgb>{};
			if (DecodeAuxImage( file, image )) {
				aux_slot( *pyramid, kAuxMacro ) = std::move( image );
				*overview_priority = priority;
				*have_overview = true;
			}
		}
		return true;
	}
	return role != "VOLUME";
}


static std::string DcmText ( DcmItem *ds, const DcmTagKey &key )
{
	auto value = OFString{};
	return ds && ds->findAndGetOFStringArray( key, value ).good() ? value.c_str() : "";
}


static std::string ContextText ( DcmItem *ds, const char *name )
{
	DcmSequenceOfItems *sequence = nullptr;
	if (ds->findAndGetSequence( DCM_AcquisitionContextSequence, sequence ).bad() || !sequence)
		return {};
	for (auto *object = sequence->nextInContainer( nullptr ); object; object = sequence->nextInContainer( object )) {
		auto *entry = OFstatic_cast( DcmItem *, object );
		DcmItem *concept_item = nullptr;
		if (entry->findAndGetSequenceItem( DCM_ConceptNameCodeSequence, concept_item ).good() && concept_item &&
			DcmText( concept_item, DCM_CodingSchemeDesignator ) == dcm_writer_identity().local_scheme &&
			DcmText( concept_item, DCM_CodeValue ) == name)
		{
			return DcmText( entry, DCM_TextValue );
		}
	}
	return {};
}


static bool EffectiveDcmRange ( DcmItem *ds, int stored_max, int *out )
{
	*out = stored_max;
	DcmSequenceOfItems *sequence = nullptr;
	if (ds->findAndGetSequence( DCM_AcquisitionContextSequence, sequence ).bad() || !sequence)
		return true;
	auto found = false;
	for (auto *object = sequence->nextInContainer( nullptr ); object; object = sequence->nextInContainer( object )) {
		auto *entry = OFstatic_cast( DcmItem *, object );
		DcmItem *concept_item = nullptr;
		if (entry->findAndGetSequenceItem( DCM_ConceptNameCodeSequence, concept_item ).bad() || !concept_item ||
			DcmText( concept_item, DCM_CodingSchemeDesignator ) != dcm_writer_identity().local_scheme ||
			DcmText( concept_item, DCM_CodeValue ) != kDcmRangeCode)
			continue;
		Float64 value = 0;
		if (found || DcmText( entry, DCM_ValueType ) != "NUMERIC" ||
			entry->findAndGetFloat64( DCM_NumericValue, value ).bad() ||
			!std::isfinite( value ) || value < 1 || value > stored_max || value != std::floor( value ))
			return false;
		*out = (int)value;
		found = true;
	}
	return true;
}


static std::vector<DcmPendingLevel> CollectDcmLevels (
	const std::string &filename, Ppyr *pyramid, bool *have_overview, bool *have_frost )
{
	auto opened = fs::absolute( fs::path{ filename } ).lexically_normal();
	auto opened_ff = std::unique_ptr<DcmFileFormat>{};
	auto opened_offset = uint64_t{};
	auto opened_length = uint32_t{};
	if (!read_dcm_header( filename, opened_ff, &opened_offset, &opened_length ))
		return {};
	const auto series_uid = DcmText( opened_ff->getDataset(), DCM_SeriesInstanceUID );
	const auto pyramid_uid = DcmText( opened_ff->getDataset(), DCM_PyramidUID );
	if (series_uid.empty())
		return {};
	const auto paths = dcm_series_paths( opened );
	auto pending = std::vector<DcmPendingLevel>{};
	auto overview_priority = -1;
	for (const auto &path : paths) {
		auto ff = std::unique_ptr<DcmFileFormat>{};
		auto pixel_offset = opened_offset;
		auto pixel_length = opened_length;
		if (path == opened)
			ff = std::move( opened_ff );
		else if (!read_dcm_header( path.string(), ff, &pixel_offset, &pixel_length ))
			continue;
		auto *ds = ff->getDataset();
		if (DcmText( ds, DCM_SeriesInstanceUID ) != series_uid)
			continue;
		const auto role = ImageRole( ds );
		if (role != "VOLUME") {
			if (role == "LABEL" || role == "OVERVIEW" || role == "REGIONLOCALIZER") {
				if (ff->loadFile( path.string().c_str(), EXS_Unknown, EGL_noChange, 4096 ).bad())
					continue;
				ds = ff->getDataset();
			}
			HandleDcmAuxiliaryImageLocal( ds, *ff, role, pyramid, have_overview, have_frost, &overview_priority );
			continue;
		}
		if (!pyramid_uid.empty() && DcmText( ds, DCM_PyramidUID ) != pyramid_uid)
			continue;
		RecordDcmProperties( ds, *pyramid );
		ReadDcmIccProfile( ds, *pyramid );
		if (pyramid->barcode.empty()) {
			pyramid->barcode = ReadBarcodeTags( ds );
			pyramid->barcode_from_metadata = !pyramid->barcode.empty();
		}
		Uint16 cols = 0, rows = 0;
		Uint32 total_w = 0, total_h = 0, z_count = 1;
		long frame_count = 0;
		ds->findAndGetUint16( DCM_Columns, cols );
		ds->findAndGetUint16( DCM_Rows, rows );
		ds->findAndGetUint32( DCM_TotalPixelMatrixColumns, total_w );
		ds->findAndGetUint32( DCM_TotalPixelMatrixRows, total_h );
		ds->findAndGetLongInt( DCM_NumberOfFrames, frame_count );
		ds->findAndGetUint32( DCM_TotalPixelMatrixFocalPlanes, z_count );
		if (!cols || !rows || !total_w || !total_h || frame_count <= 0 ||
			total_w > (Uint32)std::numeric_limits<int>::max() || total_h > (Uint32)std::numeric_limits<int>::max())
			return {};
		const auto organization = DcmText( ds, DCM_DimensionOrganizationType );
		const auto sparse = organization == "TILED_SPARSE";
		if (!organization.empty() && organization != "TILED_FULL" && !sparse)
			return {};
		const auto transfer = ds->getOriginalXfer();
		const auto htj2k = IsHtj2kTransfer( transfer );
		if (transfer != EXS_JPEGProcess1 && transfer != EXS_JPEGProcess2_4 &&
			transfer != EXS_JPEG2000LosslessOnly && transfer != EXS_JPEG2000 && !htj2k) {
			pyr::plog( "wsi dcm: unsupported transfer syntax %s\n", DcmXfer( transfer ).getXferName() );
			continue;
		}
		auto type = LevelPixelType::Rgb8;
		auto stored_max = 0;
		auto channels = 0u, bits = 0u;
		auto reason = std::string{};
		if (!ReadHtj2kPixelType( ds, &type, &stored_max, &channels, &bits, &reason, htj2k ) ||
			( !htj2k && ReadDcmColourDepth( ds, transfer, nullptr, &reason ) == DcmColourDepth::Invalid ))
		{
			pyr::plog( "wsi dcm: invalid pixel metadata: %s\n", reason.c_str() );
			return {};
		}
		auto effective_max = stored_max;
		if (!EffectiveDcmRange( ds, stored_max, &effective_max ))
			return {};
		auto frames = std::vector<DcmPixelLocation>{};
		if (!index_dcm_frames( path.string(), *ds, pixel_offset, pixel_length,
			(uint32_t)frame_count, &frames, &reason ))
		{
			pyr::plog( "wsi dcm: %s: cannot index frames: %s\n", path.string().c_str(), reason.c_str() );
			return {};
		}
		const auto metric = ReadPixelMetric( ds, (int)total_w, (int)total_h );
		if (!( metric.x > 0.0f ) || !( metric.y > 0.0f ))
			return {};
		auto optical_names = std::map<std::string, std::string>{};
		auto optical_order = std::vector<std::string>{};
		DcmSequenceOfItems *optical = nullptr;
		if (ds->findAndGetSequence( DCM_OpticalPathSequence, optical ).good() && optical) {
			for (auto *object = optical->nextInContainer( nullptr ); object; object = optical->nextInContainer( object )) {
				auto *entry = OFstatic_cast( DcmItem *, object );
				const auto id = DcmText( entry, DCM_OpticalPathIdentifier );
				auto name = DcmText( entry, DCM_OpticalPathDescription );
				if (name.empty())
					name = id;
				optical_names[id] = channels == 3 && optical->card() == 1 ? std::string{} : name;
				optical_order.push_back( id );
			}
		}
		if (optical_order.empty()) {
			optical_order.push_back( "" );
			optical_names[""] = "";
		}
		if (channels == 1 && optical_order.size() == 1)
			if (auto name = ContextText( ds, "CHANNEL_NAME" ); !name.empty())
				optical_names[optical_order.front()] = std::move( name );
		DcmSequenceOfItems *per_frame = nullptr;
		ds->findAndGetSequence( DCM_PerFrameFunctionalGroupsSequence, per_frame );
		if (sparse && ( !per_frame || per_frame->card() != frames.size() ))
			return {};
		auto group_indices = std::map<std::pair<std::string, float>, size_t>{};
		const auto across = (uint64_t)( 1 + ( total_w - 1 ) / cols );
		const auto down = (uint64_t)( 1 + ( total_h - 1 ) / rows );
		const auto cells = across * down;
		auto seen = std::set<std::tuple<std::string, float, int, int>>{};
		DcmObject *frame_object = per_frame ? per_frame->nextInContainer( nullptr ) : nullptr;
		for (auto f = size_t{}; f < frames.size(); ++f) {
			auto x = (int)( f % across ) * cols;
			auto y = (int)( ( f / across ) % down ) * rows;
			auto z = (float)( ( f / cells ) % std::max<Uint32>( z_count, 1 ) );
			const auto op_no = f / cells / std::max<Uint32>( z_count, 1 );
			auto id = optical_order[std::min( op_no, optical_order.size() - 1 )];
			if (sparse) {
				auto *entry = OFstatic_cast( DcmItem *, frame_object );
				DcmItem *position = nullptr, *identification = nullptr;
				Sint32 column = 0, row = 0;
				Float64 z_offset = 0;
				if (!entry || entry->findAndGetSequenceItem( DCM_PlanePositionSlideSequence, position ).bad() || !position ||
					position->findAndGetSint32( DCM_ColumnPositionInTotalImagePixelMatrix, column ).bad() ||
					position->findAndGetSint32( DCM_RowPositionInTotalImagePixelMatrix, row ).bad() || column <= 0 || row <= 0)
					return {};
				x = column - 1;
				y = row - 1;
				position->findAndGetFloat64( DCM_ZOffsetInSlideCoordinateSystem, z_offset );
				if (!std::isfinite( z_offset ))
					return {};
				z = (float)z_offset;
				if (entry->findAndGetSequenceItem( DCM_OpticalPathIdentificationSequence, identification ).good() && identification)
					id = DcmText( identification, DCM_OpticalPathIdentifier );
				frame_object = per_frame->nextInContainer( frame_object );
			}
			if (x >= (int)total_w || y >= (int)total_h || !optical_names.contains( id ) ||
				!seen.emplace( id, z, x, y ).second)
				return {};
			const auto key = std::pair{ optical_names.at( id ), z };
			auto [group, inserted] = group_indices.try_emplace( key, pending.size() );
			if (inserted) {
				auto next = DcmPendingLevel{};
				next.source_metric = metric;
				next.decoding = { htj2k, channels, bits };
				auto &level = next.level;
				level.filename = path.string();
				level.metric = metric.x;
				level.pixel_type = type;
				level.pixel_type_declared = true;
				level.max_pixel_value = effective_max;
				level.plane = { key.first, key.second };
				level.tile_w = cols;
				level.tile_h = rows;
				level.declared_bounds = { { 0, 0 }, { (int)total_w, (int)total_h } };
				pending.push_back( std::move( next ) );
			}
			auto &level = pending[group->second].level;
			auto tile = Tile{};
			tile.lr = { { x, y }, { (int)std::min<int64_t>( (int64_t)x + cols, total_w ),
				(int)std::min<int64_t>( (int64_t)y + rows, total_h ) } };
			tile.foff = (size_t)frames[f].offset;
			tile.fsz = frames[f].length;
			level.tile_buffer_bytes = std::max( level.tile_buffer_bytes, tile.fsz );
			level.tiles.push_back( std::move( tile ) );
		}
	}
	for (auto &entry : pending)
		set_level_bounds( entry.level );
	return pending;
}


// ProbeWsiPixelType's DICOM half: the opened file decides when it is a VOLUME,
// otherwise the first VOLUME of its series does. Metadata-only, and true only
// when the slide declares a type other than the 8-bit RGB default.
bool ProbeDcmPixelType ( const std::string &filename, LevelPixelType *out )
{
	if (!out)
		return false;
	auto opened_ff = std::unique_ptr<DcmFileFormat>{};
	if (!LoadMeta( filename, opened_ff ))
		return false;
	auto decide = [out] ( DcmDataset *ds ) {
		auto maximum = 0;
		auto channels = 0u, bits = 0u;
		return ReadHtj2kPixelType( ds, out, &maximum, &channels, &bits, nullptr, IsHtj2kTransfer( ds->getOriginalXfer() ) ) &&
			( IsHtj2kTransfer( ds->getOriginalXfer() ) || ReadDcmColourDepth( ds, ds->getOriginalXfer(), nullptr ) != DcmColourDepth::Invalid );
	};
	auto *opened_ds = opened_ff->getDataset();
	if (ImageRole( opened_ds ) == "VOLUME")
		return decide( opened_ds );

	auto series_uid = OFString{};
	opened_ds->findAndGetOFString( DCM_SeriesInstanceUID, series_uid );
	for (const auto &path : dcm_series_paths( fs::path{ filename } )) {
		auto ff = std::unique_ptr<DcmFileFormat>{};
		if (!LoadMeta( path.string(), ff ))
			continue;
		auto *ds = ff->getDataset();
		auto uid = OFString{};
		ds->findAndGetOFString( DCM_SeriesInstanceUID, uid );
		if ((!series_uid.empty() && uid != series_uid) || ImageRole( ds ) != "VOLUME")
			continue;
		return decide( ds );
	}
	return false;
}


bool DcmLoader::Open ( Ppyr &pyramid )
{
	auto have_overview = false;
	auto have_frost = false;
	auto pending = CollectDcmLevels( filename_, &pyramid, &have_overview, &have_frost );
	if (pending.empty())
		return false;
	std::stable_sort( pending.begin(), pending.end(), [] ( const auto &a, const auto &b ) {
		return a.level.metric < b.level.metric;
	} );
	pyramid.source_pixel_metric = pending.front().source_metric;
	state_->levels.clear();
	for (auto &entry : pending) {
		state_->levels[entry.level.filename] = entry.decoding;
		pyramid.levels.push_back( std::move( entry.level ) );
	}
	return true;
}


bool DcmLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this] ( const auto &level, const auto &tile, uint8_t *dest, size_t &pos, unsigned worker ) {
			return AssembleTile( level, tile, dest, pos, worker );
		} );
}


bool DcmLoader::AssembleTile ( const Level &level, const Tile &tile, uint8_t *dest,
	size_t &pos, unsigned worker )
{
	const auto found = state_->levels.find( level.filename );
	if (found == state_->levels.end() ||
		!ReadFromFile( dest, tile.foff, tile.fsz, worker, level.filename ))
		return false;
	const auto &expected = found->second;
	if (expected.htj2k) {
		auto info = ImgCodecInfo{};
		auto error = std::string{};
		if (!read_img_codec_info( dest, tile.fsz, &info, &error ) || info.codec != ImgCodec::Htj2k ||
			info.width != level.tile_w || info.height != level.tile_h ||
			info.color_channels != (int)expected.channels || info.bits_per_sample != (int)expected.bits)
			return false;
	}
	pos = tile.fsz;
	return true;
}


void DcmLoader::Close ( Ppyr &pyramid ) {
	state_->levels.clear();
	this->CloseFile();
	pyramid.levels.clear();
	pyramid.tbf = {};
	pyramid.pxb = {};
}



} // namespace priml

#endif
