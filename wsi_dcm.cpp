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
#include "img_codec.h"
#include "wsi.h"
#include "wsi_formats.h"

#include "pyr_rt.h"

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcpixseq.h"
#include "dcmtk/dcmdata/dcpxitem.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _MSC_VER
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

// Per-level DCMTK state, kept parallel to pyramid.levels (same index = level_no).
// Owns the DcmFileFormat so deferred frame access reads from its retained
// stream; seq points into that file format (non-owning).
struct DcmState {
	struct LevelH {
		std::unique_ptr<DcmFileFormat> ff;
		DcmPixelSequence *seq = nullptr;	// owned by ff
// Fragment pointers indexed by absolute pixel-item index (0 = Basic Offset
// Table, 1.. = frames). Cached once in Open via O(1) sequential iteration so
// per-tile access is O(1); DcmPixelSequence::getItem is O(n) (linked-list seek).
		std::vector<DcmPixelItem *> frags;
		unsigned long num_frames = 0;
		int tiles_across = 0;
		bool bot = true;					// item 0 is the Basic Offset Table
	};
	std::vector<LevelH> levels;
};


// One VOLUME level under construction (before sorting by metric).
struct DcmPendingLevel {
	float metric = 0.0f;
	PixelMetric source_metric;
	int tile_w = 0;
	int tile_h = 0;
	int total_w = 0;
	int total_h = 0;
	unsigned long num_frames = 0;
	int tiles_across = 0;
	int tiles_down = 0;
	std::unique_ptr<DcmFileFormat> ff;
	DcmPixelSequence *seq = nullptr;
	std::vector<DcmPixelItem *> frags;	// see DcmState::LevelH::frags
// Grid-cell -> 0-based frame number, row-major size tiles_across*tiles_down.
// -1 marks a cell with no tile (a hole; rendered blank). For TILED_FULL this is
// the identity mapping; for TILED_SPARSE it is built from each frame's explicit
// PlanePositionSlide position.
	std::vector<long> frame_at_cell;
	bool bot = true;
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
	bool pixel_type_declared = false;
	int max_pixel_value = 0;
};


static std::string ToLower ( std::string s ) {
	return lower_ascii_copy( s );
}


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
		set_property( pyramid, "openslide.objective-power",
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
static bool LoadMeta ( const std::string &path, std::unique_ptr<DcmFileFormat> &out ) {
	auto ff = std::make_unique<DcmFileFormat>();
	auto cond = ff->loadFile( path.c_str(), EXS_Unknown, EGL_noChange, 4096 );
	if (cond.bad())
		return false;
	out = std::move( ff );
	return true;
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
static std::vector<long> BuildFrameCellMap ( DcmDataset *ds, bool is_sparse,
	int cols, int rows, int across, int down, unsigned long num_frames,
	const std::string &path )
{
	std::vector<long> map( (size_t)across * (size_t)down, -1 );

	if (!is_sparse) {
		auto n = std::min<unsigned long>( num_frames, (unsigned long)map.size() );
		for (unsigned long i = 0; i < n; ++i)
			map[i] = (long)i;
		return map;
	}

	DcmSequenceOfItems *pf = nullptr;
	if (ds->findAndGetSequence( DCM_PerFrameFunctionalGroupsSequence, pf ).bad() || !pf) {
		pyr::plog( "wsi dcm: %s TILED_SPARSE without PerFrameFunctionalGroups\n",
			path.c_str() );
		return map;
	}

// One O(n) walk of the per-frame items; per-item lookups are O(small).
	unsigned long f = 0;
	for (DcmObject *o = pf->nextInContainer( nullptr ); o;
		o = pf->nextInContainer( o ), ++f) {
		auto *item = OFstatic_cast( DcmItem *, o );
		DcmItem *pps = nullptr;
		if (item->findAndGetSequenceItem( DCM_PlanePositionSlideSequence, pps, 0 ).bad()
			|| !pps)
			continue;
		Sint32 col = 0, row = 0;
		if (pps->findAndGetSint32( DCM_ColumnPositionInTotalImagePixelMatrix, col ).bad()
			|| pps->findAndGetSint32( DCM_RowPositionInTotalImagePixelMatrix, row ).bad())
			continue;

		auto cx = (int)col - 1, cy = (int)row - 1;	// 1-based -> 0-based pixels
		if (cx < 0 || cy < 0 || cx % cols != 0 || cy % rows != 0) {
			pyr::plog( "wsi dcm: %s frame %lu pos (%d,%d) not tile-aligned, skipping\n",
				path.c_str(), f, (int)col, (int)row );
			continue;
		}
		auto tx = cx / cols, ty = cy / rows;
		if (tx >= across || ty >= down)
			continue;
		map[(size_t)ty * (size_t)across + (size_t)tx] = (long)f;
	}
	return map;
}


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


static std::vector<DcmPendingLevel> CollectDcmLevels (
	const std::string &filename,
	Ppyr *pyramid,
	bool *have_overview,
	bool *have_frost )
{
	auto opened = fs::path{ filename };
	auto dir = opened.parent_path();
	if (dir.empty())
		dir = fs::current_path();
	std::unique_ptr<DcmFileFormat> opened_ff;
	if (!LoadMeta( filename, opened_ff ))
		return {};
	auto series_uid = OFString{};
	opened_ff->getDataset()->findAndGetOFString(
		DCM_SeriesInstanceUID, series_uid );
	auto pending = std::vector<DcmPendingLevel>{};
	auto overview_priority = -1;
	auto ec = std::error_code{};
	for (auto &entry : fs::directory_iterator( dir, ec )) {
		if (!entry.is_regular_file() ||
			ToLower( entry.path().extension().string() ) != ".dcm")
		{
			continue;
		}
		auto path = entry.path().string();
		auto ff = std::unique_ptr<DcmFileFormat>{};
		if (!LoadMeta( path, ff ))
			continue;
		auto *ds = ff->getDataset();
		auto uid = OFString{};
		ds->findAndGetOFString( DCM_SeriesInstanceUID, uid );
		if (!series_uid.empty() && uid != series_uid)
			continue;
		auto role = ImageRole( ds );
		if (HandleDcmAuxiliaryImageLocal( ds, *ff, role, pyramid,
				have_overview, have_frost, &overview_priority ))
		{
			continue;
		}

		Uint16 cols = 0, rows = 0;
		Uint32 total_w = 0, total_h = 0;
		long int num_frames = 0;
		ds->findAndGetUint16( DCM_Columns, cols );
		ds->findAndGetUint16( DCM_Rows, rows );
		ds->findAndGetUint32( DCM_TotalPixelMatrixColumns, total_w );
		ds->findAndGetUint32( DCM_TotalPixelMatrixRows, total_h );
		ds->findAndGetLongInt( DCM_NumberOfFrames, num_frames );
		if (cols == 0 || rows == 0 || total_w == 0 || total_h == 0 ||
			num_frames <= 0)
		{
			continue;
		}
		auto organization = OFString{};
		ds->findAndGetOFString( DCM_DimensionOrganizationType, organization );
		auto is_sparse = organization == "TILED_SPARSE";
		if (!organization.empty() && organization != "TILED_FULL" && !is_sparse) {
			pyr::plog( "wsi dcm: %s uses %s (only TILED_FULL/TILED_SPARSE supported), skipping\n",
				path.c_str(), organization.c_str() );
			continue;
		}
		auto transfer = ds->getOriginalXfer();
// Each frame is one encapsulated fragment regardless of which of these it is;
// the codec layer identifies the actual encoding from the fragment's own magic
// bytes. The Multicomponent syntaxes (.92/.93) apply a transform across more
// than three components and are genuinely not handled.
		if (transfer != EXS_JPEGProcess1 && transfer != EXS_JPEGProcess2_4 &&
			transfer != EXS_JPEG2000LosslessOnly && transfer != EXS_JPEG2000)
		{
			pyr::plog( "wsi dcm: %s transfer syntax %s not yet supported (only JPEG baseline/extended and JPEG 2000), skipping\n",
				path.c_str(), DcmXfer( transfer ).getXferName() );
			continue;
		}
		auto depth_reason = std::string{};
		auto deep_max = 0;
		const auto depth = ReadDcmColourDepth(
			ds, transfer, &deep_max, &depth_reason );
		if (depth == DcmColourDepth::Invalid) {
			pyr::plog( "wsi dcm: %s has invalid colour depth metadata: %s; skipping\n",
				path.c_str(), depth_reason.c_str() );
			continue;
		}
		DcmElement *element = nullptr;
		if (ds->findAndGetElement( DCM_PixelData, element ).bad() || !element)
			continue;
		auto *pixel_data = OFstatic_cast( DcmPixelData *, element );
		DcmPixelSequence *sequence = nullptr;
		const DcmRepresentationParameter *parameter = nullptr;
		if (pixel_data->getEncapsulatedRepresentation(
				transfer, parameter, sequence ).bad() || !sequence)
		{
			pyr::plog( "wsi dcm: %s pixel data is not encapsulated, skipping\n",
				path.c_str() );
			continue;
		}

		auto level = DcmPendingLevel{};
		level.tile_w = (int)cols;
		level.tile_h = (int)rows;
		level.total_w = (int)total_w;
		level.total_h = (int)total_h;
		level.num_frames = (unsigned long)num_frames;
		level.tiles_across = ((int)total_w + (int)cols - 1) / (int)cols;
		level.tiles_down = ((int)total_h + (int)rows - 1) / (int)rows;
		level.source_metric = ReadPixelMetric( ds, (int)total_w, (int)total_h );
		level.metric = level.source_metric.y > 0.0f
			? level.source_metric.y : level.source_metric.x;
		level.bot = true;
		level.seq = sequence;
		level.frags.reserve( sequence->card() );
		for (DcmObject *object = sequence->nextInContainer( nullptr ); object;
			object = sequence->nextInContainer( object ))
		{
			level.frags.push_back( OFstatic_cast( DcmPixelItem *, object ) );
		}
		level.frame_at_cell = BuildFrameCellMap(
			ds, is_sparse, (int)cols, (int)rows, level.tiles_across,
			level.tiles_down, level.num_frames, path );
		if (depth == DcmColourDepth::Rgb16) {
			level.pixel_type = LevelPixelType::Rgb16;
			level.pixel_type_declared = true;
			level.max_pixel_value = deep_max;
		}
		level.ff = std::move( ff );
		pending.push_back( std::move( level ) );
	}
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
		if (ReadDcmColourDepth(
				ds, ds->getOriginalXfer(), nullptr ) != DcmColourDepth::Rgb16)
		{
			return false;
		}
		*out = LevelPixelType::Rgb16;
		return true;
	};
	auto *opened_ds = opened_ff->getDataset();
	if (ImageRole( opened_ds ) == "VOLUME")
		return decide( opened_ds );

	auto series_uid = OFString{};
	opened_ds->findAndGetOFString( DCM_SeriesInstanceUID, series_uid );
	auto dir = fs::path{ filename }.parent_path();
	if (dir.empty())
		dir = fs::current_path();
	auto ec = std::error_code{};
	for (auto &entry : fs::directory_iterator( dir, ec )) {
		if (!entry.is_regular_file() ||
			ToLower( entry.path().extension().string() ) != ".dcm")
		{
			continue;
		}
		auto ff = std::unique_ptr<DcmFileFormat>{};
		if (!LoadMeta( entry.path().string(), ff ))
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


static Level MaterializeDcmLevel (
	DcmPendingLevel *pending )
{
	auto level = Level{};
	level.tile_w = pending->tile_w;
	level.tile_h = pending->tile_h;
	level.metric = pending->metric > 0.0f ? pending->metric : 1.0f;
	level.pixel_type = pending->pixel_type;
	level.pixel_type_declared = pending->pixel_type_declared;
	level.max_pixel_value = pending->max_pixel_value;
	auto expected = (unsigned long)pending->tiles_across *
		(unsigned long)pending->tiles_down;
	if (expected != pending->num_frames) {
		pyr::plog( "wsi dcm: frame count %lu != grid %dx%d (%lu) - using grid\n",
			pending->num_frames, pending->tiles_across,
			pending->tiles_down, expected );
	}
	auto max_fsz = size_t{ 0 };
	auto frag_base = pending->bot ? 1u : 0u;
	for (auto ty = 0; ty < pending->tiles_down; ++ty) {
		for (auto tx = 0; tx < pending->tiles_across; ++tx) {
			auto tile = Tile{};
			tile.lr = grid_tile_rect( { pending->total_w, pending->total_h },
				{ pending->tile_w, pending->tile_h }, tx, ty );
			auto cell = (size_t)ty * (size_t)pending->tiles_across + (size_t)tx;
			auto frame_no = cell < pending->frame_at_cell.size()
				? pending->frame_at_cell[cell] : -1;
			if (frame_no >= 0) {
				auto item_idx = (unsigned long)frag_base +
					(unsigned long)frame_no;
				tile.foff = item_idx;
				tile.fsz = item_idx < pending->frags.size() &&
					pending->frags[item_idx]
					? (size_t)pending->frags[item_idx]->getLength() : 0;
			}
			max_fsz = std::max( max_fsz, tile.fsz );
			level.tiles.push_back( std::move( tile ) );
		}
	}
	set_level_bounds( level );
	level.tbf.init( (int)max_fsz, (int)level.tiles.size() );
	return level;
}


bool DcmLoader::Open ( Ppyr &pyramid ) {
	auto have_overview = false;
	auto have_frost = false;
	auto pending = CollectDcmLevels(
		this->filename_, &pyramid, &have_overview, &have_frost );
	if (pending.empty())
		return false;

// Finest level first (smallest microns-per-pixel). If a level has no metric,
// push it to the back so it never wins level selection by accident.
	std::sort( pending.begin(), pending.end(),
		[] ( const DcmPendingLevel &a, const DcmPendingLevel &b ) {
			auto ma = a.metric > 0.0f ? a.metric : 1e30f;
			auto mb = b.metric > 0.0f ? b.metric : 1e30f;
			return ma < mb;
		} );
	pyramid.source_pixel_metric = pending[0].source_metric;

// Materialize levels + parallel DCMTK state in sorted order.
	state_->levels.clear();
	for (auto &pl : pending) {
		auto level = MaterializeDcmLevel( &pl );
		typename DcmState::LevelH lh;
		lh.ff = std::move( pl.ff );
		lh.seq = pl.seq;
		lh.frags = std::move( pl.frags );
		lh.num_frames = pl.num_frames;
		lh.tiles_across = pl.tiles_across;
		lh.bot = pl.bot;

		pyramid.levels.push_back( std::move( level ) );
		state_->levels.push_back( std::move( lh ) );
	}

	if (!pyramid.levels.empty() && !pyramid.levels[0].tiles.empty()) {
		auto mw = pyramid.levels[0].tiles.back().lr.br.x * pyramid.levels[0].metric;
		auto mh = pyramid.levels[0].tiles.back().lr.br.y * pyramid.levels[0].metric;
		pyramid.mrct = { {0, 0}, {mw, mh} };
	}

	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"DCM: %zu levels, overview=%d frost=%d, mrct=[%.1f,%.1f]\n",
			pyramid.levels.size(), have_overview ? 1 : 0, have_frost ? 1 : 0,
			pyramid.mrct.br.x, pyramid.mrct.br.y );
		for (size_t i = 0; i < pyramid.levels.size(); ++i) {
			auto &lv = pyramid.levels[i];
			fprintf( pyr::debug_file(), "DCM: L%zu metric=%f tiles=%zu tile=%dx%d\n",
				i, lv.metric, lv.tiles.size(), lv.tile_w, lv.tile_h );
		}
		fflush( pyr::debug_file() );
	}

	return true;
}


bool DcmLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
// DCMTK access is single-stream per file; EnsureTilesLoaded calls LoadTile
// serially on worker 0, so the pixel sequence needs no extra locking here.
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this, level_no] ( auto &level, auto &tile, uint8_t *dest, size_t &pos,
			unsigned /*worker*/ ) {
			(void)level;
			if (level_no < 0 || level_no >= (int)state_->levels.size())
				return false;
			auto &lh = state_->levels[level_no];

			auto item_idx = (size_t)tile.foff;	// pixel-item index cached in Open
			if (item_idx >= lh.frags.size() || !lh.frags[item_idx])
				return false;
			auto *frag = lh.frags[item_idx];
			Uint8 *data = nullptr;
			if (frag->getUint8Array( data ).bad() || !data)
				return false;
			auto len = (size_t)frag->getLength();
			if (len == 0 || len > tile.fsz)
				return false;

			memcpy( dest, data, len );
			pos = len;
			return true;
		} );
}


void DcmLoader::Close ( Ppyr &pyramid ) {
	state_->levels.clear();		// frees DcmFileFormats (and their pixel sequences)
	this->CloseFile();
	pyramid.levels.clear();
}



} // namespace priml

#endif
