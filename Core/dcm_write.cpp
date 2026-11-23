// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "dcm_write.h"

#include "build_config.h"
#include "dcm_color.h"
#include "pyr_rt.h"

#if USE_DCMTK
#include "pyr_win.h"
#include <dcmtk/dcmdata/dcostrmf.h>
#include <dcmtk/dcmdata/dcostrmb.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcuid.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace priml {
namespace {

bool fail ( std::string *error, std::string message )
{
	if (error)
		*error = message;
	pyr::plog( "dcm write: %s\n", message.c_str() );
	return false;
}


#if USE_DCMTK

template <typename Fn>
bool each_tile ( const DcmLevelSpec &spec, Fn &&fn )
{
	auto intervals = std::vector<std::pair<int64_t, int64_t>>{};
	const auto across = ( (int64_t)spec.size.w + spec.tile.w - 1 ) / spec.tile.w;
	for (auto y = int64_t{}; y < spec.size.h; y += spec.tile.h) {
		intervals.clear();
		if (!spec.sparse && spec.coverage.empty())
			intervals.emplace_back( 0, across );
		else
			for (const auto &rect : spec.coverage) {
				if (rect.tl.y >= y + spec.tile.h || rect.br.y <= y || width( rect ) <= 0)
					continue;
				const auto begin = std::max<int64_t>( 0, rect.tl.x / spec.tile.w );
				const auto end = std::min<int64_t>( across, ( (int64_t)rect.br.x + spec.tile.w - 1 ) / spec.tile.w );
				if (end > begin)
					intervals.emplace_back( begin, end );
			}
		std::sort( intervals.begin(), intervals.end() );
		auto previous_end = int64_t{};
		for (const auto &[begin, end] : intervals) {
			for (auto column = std::max( begin, previous_end ); column < end; ++column) {
				const auto x = column * spec.tile.w;
				const auto rect = Rtlbr<int>{ { (int)x, (int)y },
					{ (int)std::min<int64_t>( x + spec.tile.w, spec.size.w ),
					  (int)std::min<int64_t>( y + spec.tile.h, spec.size.h ) } };
				if (!fn( rect ))
					return false;
			}
			previous_end = std::max( previous_end, end );
		}
	}
	return true;
}


template <typename Fn>
bool each_frame ( const DcmLevelSpec &spec, Fn &&fn )
{
	for (auto z = 0; z < spec.focal_planes; ++z)
		if (!each_tile( spec, [&] ( const auto &rect ) { return fn( z, rect ); } ))
			return false;
	return true;
}


// Dimension indices enumerate the occupied coordinate values, independently of
// lattice gaps. Store merged coverage projections, never one record per frame.
std::vector<std::pair<int64_t, int64_t>> dimension_ranges ( const DcmLevelSpec &spec, bool rows )
{
	auto ranges = std::vector<std::pair<int64_t, int64_t>>{};
	const auto edge = rows ? spec.tile.h : spec.tile.w;
	const auto limit = ( (int64_t)( rows ? spec.size.h : spec.size.w ) + edge - 1 ) / edge;
	if (!spec.sparse && spec.coverage.empty())
		ranges.emplace_back( 0, limit );
	else
		for (const auto &rect : spec.coverage) {
			if (rect.is_empty() || rect.br.x <= 0 || rect.br.y <= 0 ||
				rect.tl.x >= spec.size.w || rect.tl.y >= spec.size.h)
				continue;
			const auto first = rows ? rect.tl.y : rect.tl.x;
			const auto last = rows ? rect.br.y : rect.br.x;
			ranges.emplace_back( std::max<int64_t>( 0, first / edge ),
				std::min<int64_t>( limit, ( (int64_t)last + edge - 1 ) / edge ) );
		}
	std::sort( ranges.begin(), ranges.end() );
	auto merged = decltype( ranges ){};
	for (const auto &range : ranges) {
		if (merged.empty() || merged.back().second < range.first)
			merged.push_back( range );
		else
			merged.back().second = std::max( merged.back().second, range.second );
	}
	return merged;
}


uint32_t dimension_ordinal ( const std::vector<std::pair<int64_t, int64_t>> &ranges, int coordinate )
{
	auto ordinal = int64_t{ 1 };
	for (const auto &[first, last] : ranges) {
		if (coordinate >= first && coordinate < last)
			return (uint32_t)( ordinal + coordinate - first );
		ordinal += last - first;
	}
	throw std::runtime_error( "frame coordinate is outside dimension coverage" );
}

void u16 ( std::ostream &out, uint16_t value )
{
	const auto bytes = std::array<char, 2>{ (char)value, (char)( value >> 8 ) };
	out.write( bytes.data(), bytes.size() );
}


void u32 ( std::ostream &out, uint32_t value )
{
	u16( out, (uint16_t)value );
	u16( out, (uint16_t)( value >> 16 ) );
}


void u64 ( std::ostream &out, uint64_t value )
{
	u32( out, (uint32_t)value );
	u32( out, (uint32_t)( value >> 32 ) );
}


void tag ( std::ostream &out, uint16_t group, uint16_t element, uint32_t length )
{
	u16( out, group );
	u16( out, element );
	u32( out, length );
}


void long_header ( std::ostream &out, uint16_t group, uint16_t element,
	const char *vr, uint32_t length )
{
	u16( out, group );
	u16( out, element );
	out.write( vr, 2 );
	u16( out, 0 );
	u32( out, length );
}

DcmItem &item ( DcmItem &parent, const DcmTagKey &key )
{
	auto entry = std::make_unique<DcmItem>();
	auto *result = entry.get();
	if (parent.insertSequenceItem( key, result, -2 ).bad())
		throw std::runtime_error( "cannot create DICOM sequence item" );
	entry.release();
	return *result;
}


void text ( DcmItem &ds, const DcmTagKey &key, const std::string &value )
{
	const auto status = ds.putAndInsertString( key, value.c_str() );
	if (status.bad())
		throw std::runtime_error( status.text() );
}


void code ( DcmItem &ds, const DcmTagKey &key, const std::string &value,
	const std::string &scheme, const std::string &meaning )
{
	auto &entry = item( ds, key );
	text( entry, DCM_CodeValue, value );
	text( entry, DCM_CodingSchemeDesignator, scheme );
	text( entry, DCM_CodeMeaning, meaning );
}


void context ( DcmItem &ds, const std::string &name, const std::string &meaning,
	const std::string &value, bool numeric = false )
{
	auto &entry = item( ds, DCM_AcquisitionContextSequence );
	text( entry, DCM_ValueType, numeric ? "NUMERIC" : "TEXT" );
	code( entry, DCM_ConceptNameCodeSequence, name, dcm_writer_identity().local_scheme, meaning );
	text( entry, numeric ? DCM_NumericValue : DCM_TextValue, value );
	if (numeric)
		code( entry, DCM_MeasurementUnitsCodeSequence, "1", "UCUM", "no units" );
}


E_TransferSyntax transfer ( const DcmLevelSpec &spec )
{
	if (spec.codec == ImgCodec::Jpeg)
		return EXS_JPEGProcess1;
	const auto lossless = spec.encode.quality == kImageLosslessQuality;
	return spec.codec == ImgCodec::Htj2k
		? ( lossless ? EXS_HighThroughputJPEG2000LosslessOnly : EXS_HighThroughputJPEG2000 )
		: ( lossless ? EXS_JPEG2000LosslessOnly : EXS_JPEG2000 );
}


// Image Type, and the Frame Type that repeats it: values 3 and 4 by what the
// instance holds.
std::string image_type ( const DcmLevelSpec &spec )
{
	switch (spec.associated) {
		case DcmAssociatedRole::Label: return "DERIVED\\PRIMARY\\LABEL\\NONE";
		case DcmAssociatedRole::Overview: return "DERIVED\\PRIMARY\\OVERVIEW\\NONE";
		case DcmAssociatedRole::None: break;
	}
	return spec.thumbnail ? "DERIVED\\PRIMARY\\THUMBNAIL\\RESAMPLED"
		: spec.level == 0 ? "DERIVED\\PRIMARY\\VOLUME\\NONE"
		: "DERIVED\\PRIMARY\\VOLUME\\RESAMPLED";
}


void make_metadata ( DcmFileFormat &file, const DcmAcquisition &meta,
	const DcmLevelSpec &spec, const std::string &sop, const std::string &optical,
	const std::string &specimen_uid, uint32_t frames, int instance )
{
	auto &ds = *file.getDataset();
	const auto associated = spec.associated != DcmAssociatedRole::None;
	const auto rgb = spec.pixel_type == LevelPixelType::Rgb8 || spec.pixel_type == LevelPixelType::Rgb16;
	const auto wide = spec.pixel_type == LevelPixelType::Rgb16 || spec.pixel_type == LevelPixelType::Gray16;
	const auto lossless = spec.encode.quality == kImageLosslessQuality;
	text( ds, DCM_SpecificCharacterSet, "ISO_IR 192" );
	text( ds, DCM_SOPClassUID, UID_VLWholeSlideMicroscopyImageStorage );
	text( ds, DCM_SOPInstanceUID, sop );
	text( ds, DCM_StudyInstanceUID, meta.study_uid );
	text( ds, DCM_SeriesInstanceUID, meta.series_uid );
//
// The Multi-Resolution Pyramid module "shall be present only if" Image Type value
// 3 is VOLUME or THUMBNAIL.
	if (!associated)
		text( ds, DCM_PyramidUID, meta.pyramid_uid );
	text( ds, DCM_FrameOfReferenceUID, meta.frame_of_reference_uid );
	text( ds, DCM_Modality, "SM" );
	text( ds, DCM_PatientName, meta.patient_name );
	text( ds, DCM_PatientID, meta.patient_id );
	for (const auto &key : { DCM_PatientBirthDate, DCM_PatientSex, DCM_StudyID,
		DCM_StudyDate, DCM_StudyTime, DCM_ReferringPhysicianName,
		DCM_AccessionNumber, DCM_PositionReferenceIndicator })
		text( ds, key, "" );
	text( ds, DCM_Manufacturer, meta.manufacturer );
	text( ds, DCM_ManufacturerModelName, meta.model );
	text( ds, DCM_DeviceSerialNumber, meta.serial );
	text( ds, DCM_SoftwareVersions, meta.software );
	text( ds, DCM_SeriesNumber, "1" );
	text( ds, DCM_InstanceNumber, std::to_string( instance ) );
	text( ds, DCM_SeriesDescription, meta.series_description );
	const auto created = std::chrono::floor<std::chrono::seconds>( std::chrono::system_clock::now() );
	text( ds, DCM_ContentDate, std::format( "{:%Y%m%d}", created ) );
	text( ds, DCM_ContentTime, std::format( "{:%H%M%S}", created ) );
	text( ds, DCM_TimezoneOffsetFromUTC, "+0000" );
	text( ds, DCM_AcquisitionDateTime, meta.acquisition_datetime );
	text( ds, DCM_ImageType, image_type( spec ) );
	text( ds, DCM_VolumetricProperties, "VOLUME" );
//
// YES on a label or an overview, by the standard. A label can carry anything
// written on the slide, so its annotation is declared burned in as well.
	text( ds, DCM_SpecimenLabelInImage, associated ? "YES" : "NO" );
	text( ds, DCM_BurnedInAnnotation, associated ? "YES" : "NO" );
	text( ds, DCM_FocusMethod, meta.focus_method );
	if (meta.extended_depth) {
		text( ds, DCM_ExtendedDepthOfField, "YES" );
		ds.putAndInsertUint16( DCM_NumberOfFocalPlanes, (Uint16)meta.extended_depth->focal_planes );
		ds.putAndInsertFloat32( DCM_DistanceBetweenFocalPlanes, (Float32)meta.extended_depth->spacing_um );
	} else {
		text( ds, DCM_ExtendedDepthOfField, "NO" );
	}
	if (spec.associated == DcmAssociatedRole::Label) {
		text( ds, DCM_LabelText, "" );
		text( ds, DCM_BarcodeValue, spec.barcode );
	}
	text( ds, DCM_ContainerIdentifier, meta.container_identifier.empty() ? meta.name : meta.container_identifier );
	ds.insertEmptyElement( DCM_IssuerOfTheContainerIdentifierSequence );
	code( ds, DCM_ContainerTypeCodeSequence, "SLIDE", dcm_writer_identity().local_scheme, "Microscope slide" );
	auto &specimen = item( ds, DCM_SpecimenDescriptionSequence );
	text( specimen, DCM_SpecimenIdentifier, meta.specimen_id.empty() ? meta.name : meta.specimen_id );
	text( specimen, DCM_SpecimenUID, specimen_uid );
	specimen.insertEmptyElement( DCM_IssuerOfTheSpecimenIdentifierSequence );
	specimen.insertEmptyElement( DCM_SpecimenPreparationSequence );
	text( ds, DCM_DimensionOrganizationType, "TILED_SPARSE" );
	text( ds, DCM_TilesOverlap, "NONE" );
	ds.putAndInsertUint16( DCM_Rows, (Uint16)spec.tile.h );
	ds.putAndInsertUint16( DCM_Columns, (Uint16)spec.tile.w );
	ds.putAndInsertUint32( DCM_TotalPixelMatrixColumns, (Uint32)spec.size.w );
	ds.putAndInsertUint32( DCM_TotalPixelMatrixRows, (Uint32)spec.size.h );
	ds.putAndInsertUint32( DCM_TotalPixelMatrixFocalPlanes, (Uint32)spec.focal_planes );
	text( ds, DCM_NumberOfFrames, std::to_string( frames ) );
	ds.putAndInsertUint16( DCM_SamplesPerPixel, rgb ? 3 : 1 );
	ds.putAndInsertUint16( DCM_BitsAllocated, wide ? 16 : 8 );
	ds.putAndInsertUint16( DCM_BitsStored, wide ? 16 : 8 );
	ds.putAndInsertUint16( DCM_HighBit, wide ? 15 : 7 );
	ds.putAndInsertUint16( DCM_PixelRepresentation, 0 );
	const auto colour = spec.codec == ImgCodec::Jpeg ? "YBR_FULL_422" : ( lossless ? "YBR_RCT" : "YBR_ICT" );
	text( ds, DCM_PhotometricInterpretation, rgb ? colour : "MONOCHROME2" );
	if (rgb)
		ds.putAndInsertUint16( DCM_PlanarConfiguration, 0 );
	else {
		text( ds, DCM_PresentationLUTShape, "IDENTITY" );
		text( ds, DCM_RescaleIntercept, "0" );
		text( ds, DCM_RescaleSlope, "1" );
	}
//
// Required on a VOLUME, optional otherwise: an extent computed from a nominal
// spacing would state a size nobody measured, so it is left out.
	if (spec.spacing_measured) {
		ds.putAndInsertFloat32( DCM_ImagedVolumeWidth, (float)( spec.size.w * spec.mpp / 1000.0 ) );
		ds.putAndInsertFloat32( DCM_ImagedVolumeHeight, (float)( spec.size.h * spec.mpp / 1000.0 ) );
		ds.putAndInsertFloat32( DCM_ImagedVolumeDepth, (float)meta.imaged_depth_um );
	}
	auto &origin = item( ds, DCM_TotalPixelMatrixOriginSequence );
	text( origin, DCM_XOffsetInSlideCoordinateSystem, std::format( "{:.12g}", spec.origin_um.x / 1000.0 ) );
	text( origin, DCM_YOffsetInSlideCoordinateSystem, std::format( "{:.12g}", spec.origin_um.y / 1000.0 ) );
	text( origin, DCM_ZOffsetInSlideCoordinateSystem, "0" );
	text( ds, DCM_ImageOrientationSlide, "1\\0\\0\\0\\1\\0" );
	text( ds, DCM_LossyImageCompression, lossless ? "00" : "01" );
	if (!lossless) {
// Patched with the measured ratio after streaming, at the reserved DS value.
		text( ds, DCM_LossyImageCompressionRatio, "1.00000000000000" );
		const auto method = spec.codec == ImgCodec::Jpeg ? "ISO_10918_1"
			: spec.codec == ImgCodec::Htj2k ? "ISO_15444_15" : "ISO_15444_1";
		text( ds, DCM_LossyImageCompressionMethod, method );
	}
	text( ds, DCM_DerivationDescription, meta.derivation_description );
	text( ds, DCM_AcquisitionContextDescription, meta.acquisition_context );
	if (!spec.processing.empty())
		context( ds, "PROCESSING", "Applied image processing", spec.processing );
	if (!spec.spacing_measured)
		context( ds, "PIXEL_SPACING", "Pixel spacing", "not measured; nominal value" );
	context( ds, std::string( kDcmRangeCode ), "Effective sample maximum", std::to_string( spec.max_pixel_value ), true );
	if (!spec.channel.empty())
		context( ds, "CHANNEL_NAME", "Acquisition channel name", spec.channel );
	if (!spec.mode.empty())
		context( ds, "SOURCE_MODE", "Source acquisition mode", spec.mode );
	auto &path = item( ds, DCM_OpticalPathSequence );
	text( path, DCM_OpticalPathIdentifier, optical );
	text( path, DCM_OpticalPathDescription, spec.channel.size() <= 64 ? spec.channel : optical );
	const auto fluorescence = spec.fluorescence.value_or( meta.fluorescence );
	code( path, DCM_IlluminationTypeCodeSequence, fluorescence ? "FLUORESCENCE" : "ABSORPTION",
		dcm_writer_identity().local_scheme, fluorescence ? "Fluorescence imaging" : "Absorption imaging" );
	code( path, DCM_IlluminationColorCodeSequence, meta.illumination_color,
		dcm_writer_identity().local_scheme, meta.illumination_color );
	if (rgb) {
		auto profile = std::vector<uint8_t>{};
		auto error = std::string{};
		if (!make_dcm_rgb_profile( wide ? 65535 : 255, spec.max_pixel_value, &profile, &error ))
			throw std::runtime_error( error );
		path.putAndInsertUint8Array( DCM_ICCProfile, profile.data(), (unsigned long)profile.size() );
	}
	auto &shared = item( ds, DCM_SharedFunctionalGroupsSequence );
	auto &measures = item( shared, DCM_PixelMeasuresSequence );
	text( measures, DCM_PixelSpacing, std::format( "{0:.12g}\\{0:.12g}", spec.mpp / 1000.0 ) );
	const auto thickness = spec.focal_planes > 1 ? spec.focal_step_um : meta.imaged_depth_um;
	text( measures, DCM_SliceThickness, std::format( "{:.12g}", thickness / 1000.0 ) );
	auto &type = item( shared, DCM_WholeSlideMicroscopyImageFrameTypeSequence );
	text( type, DCM_FrameType, image_type( spec ) );
	text( type, DCM_VolumetricProperties, "VOLUME" );
	const auto dimension_uid = make_dcm_uid( meta.uid_root );
	text( item( ds, DCM_DimensionOrganizationSequence ), DCM_DimensionOrganizationUID, dimension_uid );
	for (const auto &key : { DCM_RowPositionInTotalImagePixelMatrix, DCM_ColumnPositionInTotalImagePixelMatrix,
		DCM_ZOffsetInSlideCoordinateSystem }) {
		if (key == DCM_ZOffsetInSlideCoordinateSystem && spec.focal_planes == 1)
			break;
		auto &dimension = item( ds, DCM_DimensionIndexSequence );
		text( dimension, DCM_DimensionOrganizationUID, dimension_uid );
		dimension.putAndInsertTagKey( DCM_DimensionIndexPointer, key );
		dimension.putAndInsertTagKey( DCM_FunctionalGroupPointer, DCM_PlanePositionSlideSequence );
	}
}


bool append_frame_group ( std::ostream &out, const DcmLevelSpec &spec,
	const Rtlbr<int> &rect, const std::string &optical,
	Uint32 row, Uint32 column, int z, std::string *error )
{
	auto entry = DcmItem{};
	auto &position = item( entry, DCM_PlanePositionSlideSequence );
	position.putAndInsertSint32( DCM_ColumnPositionInTotalImagePixelMatrix, rect.tl.x + 1 );
	position.putAndInsertSint32( DCM_RowPositionInTotalImagePixelMatrix, rect.tl.y + 1 );
	text( position, DCM_XOffsetInSlideCoordinateSystem,
		std::format( "{:.12g}", ( spec.origin_um.x + rect.tl.x * spec.mpp ) / 1000.0 ) );
	text( position, DCM_YOffsetInSlideCoordinateSystem,
		std::format( "{:.12g}", ( spec.origin_um.y + rect.tl.y * spec.mpp ) / 1000.0 ) );
	text( position, DCM_ZOffsetInSlideCoordinateSystem, std::format( "{:.12g}", z * spec.focal_step_um ) );
	text( item( entry, DCM_OpticalPathIdentificationSequence ), DCM_OpticalPathIdentifier, optical );
	auto &content = item( entry, DCM_FrameContentSequence );
	const auto indices = std::array<Uint32, 3>{ row, column, (Uint32)z + 1 };
	content.putAndInsertUint32Array( DCM_DimensionIndexValues, indices.data(), spec.focal_planes > 1 ? 3 : 2 );
	auto buffer = std::vector<uint8_t>( entry.calcElementLength( EXS_LittleEndianExplicit, EET_UndefinedLength ) );
	auto stream = DcmOutputBufferStream( buffer.data(), (offile_off_t)buffer.size() );
	entry.transferInit();
	const auto status = entry.write( stream, EXS_LittleEndianExplicit, EET_UndefinedLength, nullptr );
	entry.transferEnd();
	if (status.bad())
		return fail( error, status.text() );
	out.write( (const char *)buffer.data(), (std::streamsize)stream.filled() );
	return (bool)out || fail( error, "cannot append frame metadata" );
}

#endif

} // namespace

struct DcmWriter::Impl {
	DcmAcquisition metadata;
	std::filesystem::path directory;
	std::string specimen_uid;
	std::map<std::string, std::string> optical_paths;
	DcmWriteStats stats;
	bool opened = false;
	bool failed = false;
};


DcmWriter::DcmWriter () : impl_( std::make_unique<Impl>() ) {}


DcmWriter::~DcmWriter () = default;


namespace {

DcmWriterIdentity &writer_identity_local ()
{
	static auto identity = DcmWriterIdentity{};
	return identity;
}

} // namespace


void set_dcm_writer_identity ( DcmWriterIdentity identity )
{
	writer_identity_local() = std::move( identity );
}


const DcmWriterIdentity &dcm_writer_identity ()
{
	return writer_identity_local();
}


std::string make_dcm_uid ( std::string_view root )
{
#if USE_DCMTK
	if (root.empty() || root.size() > 29 || root.back() == '.' ||
		!std::all_of( root.begin(), root.end(), [] ( char c ) { return ( c >= '0' && c <= '9' ) || c == '.'; } ))
		return {};
	auto buffer = std::array<char, 100>{};
	auto owned = std::string( root );
	dcmGenerateUniqueIdentifier( buffer.data(), owned.c_str() );
	return buffer.data();
#else
	(void)root;
	return {};
#endif
}


std::string default_dcm_uid_root ()
{
#if USE_DCMTK
	return SITE_INSTANCE_UID_ROOT;
#else
	return {};
#endif
}


bool DcmWriter::open ( const std::filesystem::path &directory,
	const DcmAcquisition &metadata, std::string *error )
{
	if (error)
		error->clear();
#if !USE_DCMTK
	(void)directory;
	(void)metadata;
	return fail( error, "DICOM writing was not included in this build" );
#else
	if (impl_->opened)
		return fail( error, "writer is already open" );
	if (metadata.name.empty() || metadata.name.find_first_of( "/\\" ) != std::string::npos ||
		metadata.name == "." || metadata.name == "..")
		return fail( error, "DICOM entry name is invalid" );
	if (metadata.acquisition_datetime.size() < 14 ||
		DcmDateTime::checkStringValue( metadata.acquisition_datetime.c_str(), "1" ).bad())
		return fail( error, "a valid acquisition date/time is required" );
	if (( metadata.focus_method != "AUTO" && metadata.focus_method != "MANUAL" ) ||
		!std::isfinite( metadata.imaged_depth_um ) || metadata.imaged_depth_um <= 0.0)
		return fail( error, "focus method and positive imaged depth are required" );
	if (metadata.illumination_color.empty() || metadata.illumination_color.size() > 16 ||
		metadata.manufacturer.empty() || metadata.model.empty() || metadata.serial.empty() || metadata.software.empty())
		return fail( error, "illumination and producing-equipment metadata are required" );
	if (metadata.extended_depth &&
		( metadata.extended_depth->focal_planes < 2 ||
		  metadata.extended_depth->focal_planes > std::numeric_limits<Uint16>::max() ||
		  !std::isfinite( metadata.extended_depth->spacing_um ) ||
		  !( metadata.extended_depth->spacing_um > 0.0 ) ))
		return fail( error, "extended depth of field needs at least two focal planes and a positive spacing" );
	*impl_ = Impl{};
	impl_->metadata = metadata;
	auto &meta = impl_->metadata;
	for (auto *uid : { &meta.study_uid, &meta.series_uid, &meta.pyramid_uid, &meta.frame_of_reference_uid }) {
		if (uid->empty())
			*uid = make_dcm_uid( meta.uid_root );
		if (uid->empty() || DcmUniqueIdentifier::checkStringValue( uid->c_str(), "1" ).bad())
			return fail( error, "a valid caller UID root/identity is required" );
	}
	impl_->specimen_uid = meta.specimen_uid.empty() ? make_dcm_uid( meta.uid_root ) : meta.specimen_uid;
	if (impl_->specimen_uid.empty() || DcmUniqueIdentifier::checkStringValue( impl_->specimen_uid.c_str(), "1" ).bad())
		return fail( error, "a valid caller UID root is required" );
	auto ec = std::error_code{};
	std::filesystem::create_directories( directory, ec );
	if (ec)
		return fail( error, "cannot create staging directory: " + ec.message() );
	impl_->directory = directory;
	impl_->opened = true;
	return true;
#endif
}


template <typename T>
bool DcmWriter::add_level ( const DcmLevelSpec &source, const DcmTileFetch<T> &fetch,
	std::string *error )
{
	if (!fetch || source.focal_planes != 1)
		return fail( error, "single-plane DICOM writing requires a tile callback and one focal plane" );
	return write_pixel_level<T>( source, [&] ( int, const Rtlbr<int> &rect, Pwhl<T> *out, std::string *tile_error ) {
		return fetch( rect, out, tile_error );
	}, error );
}


bool DcmWriter::add_focal_level ( const DcmLevelSpec &spec, const DcmFocalTileFetch<Rgb> &fetch,
	std::string *error, const std::function<bool ()> &cancelled )
{
	return write_pixel_level( spec, fetch, error, cancelled );
}


template <typename T>
bool DcmWriter::write_pixel_level ( const DcmLevelSpec &source, const DcmFocalTileFetch<T> &fetch,
	std::string *error, const std::function<bool ()> &cancelled )
{
#if !USE_DCMTK
	(void)source;
	(void)fetch;
	(void)cancelled;
	return fail( error, "DICOM writing was not included in this build" );
#else
	if (!fetch || source.pixel_type != pixel_type_of<T>::value)
		return fail( error, "invalid DICOM level geometry, pixel type, or codec" );
// WSI BitsStored equals BitsAllocated, even when the effective range is 4095.
	auto encode = source.encode;
	encode.bits_per_sample = sizeof( channel_type<T> ) * 8;
	encode.max_pixel_value = source.max_pixel_value;
	encode.jpeg_422 = source.codec == ImgCodec::Jpeg;
	auto pixels = Pwhl<T>{};
	const auto frames = FrameSource( [&] ( int z, const Rtlbr<int> &rect, std::vector<uint8_t> *encoded,
		std::string *frame_error ) {
		if (!pixels.p)
			pixels = Pwhl<T>( source.tile );
		std::memset( pixels.p, 0, (size_t)pixels.h * pixels.l * sizeof( T ) );
		const auto result = fetch( z, rect, &pixels, frame_error );
		if (result != DcmFetchResult::Pixels)
			return fail( frame_error, result == DcmFetchResult::Error && frame_error && !frame_error->empty()
				? *frame_error : "occupied DICOM frame has no source pixels" );
		if (!priml::are_wh_eq( pixels, source.tile ))
			return fail( frame_error, "frame callback changed the fixed DICOM frame dimensions" );
		return encode_tile( source.codec, pixels, encoded, encode, frame_error ) && !encoded->empty();
	} );
	return write_level( source, frames, error, cancelled );
#endif
}


bool DcmWriter::add_encoded_level ( const DcmLevelSpec &spec, const DcmEncodedFetch &fetch,
	std::string *error )
{
#if !USE_DCMTK
	(void)spec;
	(void)fetch;
	return fail( error, "DICOM writing was not included in this build" );
#else
	if (!fetch)
		return fail( error, "encoded-frame fetch callback is null" );
	if (spec.focal_planes != 1 || spec.codec == ImgCodec::Jpeg)
		return fail( error, "encoded DICOM input requires one focal plane and JPEG 2000 or HTJ2K" );
	const auto samples = is_integer_rgb( spec.pixel_type ) ? 3 : 1;
	const auto bits = (int)( pixel_size_bytes( spec.pixel_type ) / samples * 8 );
	const auto frames = FrameSource( [&] ( int, const Rtlbr<int> &rect, std::vector<uint8_t> *encoded,
		std::string *frame_error ) {
		encoded->clear();
		const auto result = fetch( rect, encoded, frame_error );
		if (result != DcmFetchResult::Pixels)
			return fail( frame_error, result == DcmFetchResult::Error && frame_error && !frame_error->empty()
				? *frame_error : "occupied DICOM frame has no encoded frame" );
		auto info = ImgCodecInfo{};
		if (!read_img_codec_info( encoded->data(), encoded->size(), &info, frame_error ))
			return false;
		if (info.codec != spec.codec || info.width != spec.tile.w || info.height != spec.tile.h ||
			info.color_channels != samples || info.bits_per_sample > bits || info.is_float)
			return fail( frame_error, "encoded frame does not match its DICOM geometry, codec or sample type" );
		return true;
	} );
	return write_level( spec, frames, error );
#endif
}


bool DcmWriter::write_level ( const DcmLevelSpec &source, const FrameSource &frame_source,
	std::string *error, const std::function<bool ()> &cancelled )
{
#if !USE_DCMTK
	(void)source;
	(void)frame_source;
	(void)cancelled;
	return fail( error, "DICOM writing was not included in this build" );
#else
	if (!impl_->opened || impl_->failed)
		return fail( error, "DICOM writer is not ready" );
	impl_->failed = true;
	auto spec = source;
	const auto wide = spec.pixel_type == LevelPixelType::Rgb16 || spec.pixel_type == LevelPixelType::Gray16;
	const auto jpeg = spec.codec == ImgCodec::Jpeg;
	if (!spec.size.is_set() || spec.tile.w <= 0 || spec.tile.h <= 0 ||
		spec.tile.w > kDcmFrameMaxEdge || spec.tile.h > kDcmFrameMaxEdge || !std::isfinite( spec.mpp ) || spec.mpp <= 0.0 ||
		!std::isfinite( spec.origin_um.x ) || !std::isfinite( spec.origin_um.y ) ||
		spec.pixel_type == LevelPixelType::Float32 || spec.level < 0 ||
		( !jpeg && spec.codec != ImgCodec::Jp2k && spec.codec != ImgCodec::Htj2k ) ||
		( jpeg && ( wide || spec.encode.quality == kImageLosslessQuality ) ) ||
		spec.encode.quality < kImageQualityMin || spec.encode.quality > kImageLosslessQuality)
	{
		return fail( error, "invalid DICOM level geometry, pixel type, or codec" );
	}
	if (spec.focal_planes <= 0 || !std::isfinite( spec.focal_step_um * ( spec.focal_planes - 1.0 ) ) ||
		( spec.focal_planes > 1 && ( spec.focal_step_um <= 0.0 || impl_->metadata.extended_depth ||
			spec.associated != DcmAssociatedRole::None ) ))
		return fail( error, "invalid DICOM focal plane count or spacing" );
	const auto sample_bytes = pixel_size_bytes( spec.pixel_type );
	const auto container_max = wide ? 65535 : 255;
	if (spec.max_pixel_value <= 0 || spec.max_pixel_value > container_max)
		return fail( error, "effective range does not fit the pixel storage" );
	auto tiles = uint64_t{};
	if (!spec.sparse && spec.coverage.empty())
		tiles = uint64_t( 1 + ( spec.size.w - 1 ) / spec.tile.w ) * ( 1 + ( spec.size.h - 1 ) / spec.tile.h );
	else
		each_tile( spec, [&] ( const auto & ) { ++tiles; return true; } );
	constexpr auto maximum_frames = (uint64_t{ 0xfffffff8 } / sizeof( uint64_t ));
	if (tiles > maximum_frames / spec.focal_planes)
		return fail( error, "DICOM frame count exceeds supported offset-table limits" );
	const auto frames = tiles * spec.focal_planes;
	if (!frames) {
		impl_->failed = false;
		return true;
	}
	if (spec.optical_path_id.size() > 16)
		return fail( error, "optical path identifier exceeds 16 characters: " + spec.optical_path_id );
	auto [entry, inserted] = impl_->optical_paths.try_emplace( spec.channel );
	if (inserted) {
		entry->second = spec.optical_path_id.empty()
			? std::format( "OP{:03}", impl_->optical_paths.size() ) : spec.optical_path_id;
		for (const auto &[channel, id] : impl_->optical_paths)
			if (channel != spec.channel && id == entry->second) {
				impl_->optical_paths.erase( entry );
				return fail( error, "optical path identifier is already used: " + spec.optical_path_id );
			}
	} else if (!spec.optical_path_id.empty() && spec.optical_path_id != entry->second)
		return fail( error, "channel '" + spec.channel + "' already has optical path " + entry->second );
	const auto &optical = entry->second;
	const auto first = impl_->stats.instances.empty();
	const auto associated = spec.associated != DcmAssociatedRole::None;
	if (first && associated)
		return fail( error, "a label or overview cannot be the series' first instance" );
	const auto name = first ? impl_->metadata.name + ".dcm"
		: associated ? std::format( "{}_{}.dcm",
			spec.associated == DcmAssociatedRole::Label ? "LABEL" : "OVERVIEW", optical )
		: std::format( "L{:02}_{}.dcm", spec.level, optical );
	const auto path = impl_->directory / name;
	if (std::filesystem::exists( path ))
		return fail( error, "refusing to overwrite DICOM instance: " + path.string() );
	try {
		auto file = DcmFileFormat{};
		make_metadata( file, impl_->metadata, spec, make_dcm_uid( impl_->metadata.uid_root ),
			optical, impl_->specimen_uid, (uint32_t)frames, (int)impl_->stats.instances.size() + 1 );
		{
			auto stream = DcmOutputFileStream( path.string().c_str() );
			file.transferInit();
			const auto status = file.write( stream, transfer( spec ), EET_UndefinedLength,
				nullptr, EGL_withoutGL, EPD_withoutPadding, 0, 0, 0, EWM_createNewMeta );
			file.transferEnd();
			if (status.bad() || stream.status().bad())
				return fail( error, "cannot write DICOM metadata: " + std::string( status.text() ) );
		}
		auto ratio_position = std::streamoff{ -1 };
		if (spec.encode.quality != kImageLosslessQuality) {
			auto position = (std::streamoff)file.getMetaInfo()->calcElementLength( EXS_LittleEndianExplicit, EET_UndefinedLength );
			for (auto n = 0ul; n < file.getDataset()->card(); ++n) {
				auto *element = file.getDataset()->getElement( n );
				if (element->getTag().getXTag() == DCM_LossyImageCompressionRatio) {
					ratio_position = position + 8;
					break;
				}
				position += element->calcElementLength( transfer( spec ), EET_UndefinedLength );
			}
		}
		auto out = std::fstream( path, std::ios::binary | std::ios::in | std::ios::out );
		out.seekp( 0, std::ios::end );
		long_header( out, 0x5200, 0x9230, "SQ", 0xffffffff );
		auto frame = uint32_t{};
		const auto row_ranges = dimension_ranges( spec, true );
		const auto column_ranges = dimension_ranges( spec, false );
		constexpr auto frame_block = 256u;
		if (!each_frame( spec, [&] ( int z, const auto &rect ) {
			if (cancelled && frame++ % frame_block == 0 && cancelled())
				return fail( error, "cancelled" );
			return append_frame_group( out, spec, rect, optical,
				dimension_ordinal( row_ranges, rect.tl.y / spec.tile.h ),
				dimension_ordinal( column_ranges, rect.tl.x / spec.tile.w ), z, error );
		} ))
			return false;
		tag( out, 0xfffe, 0xe0dd, 0 );
		const auto table_bytes = (uint32_t)( frames * sizeof( uint64_t ) );
		long_header( out, 0x7fe0, 0x0001, "OV", table_bytes );
		const auto offsets_at = out.tellp();
		const auto zeros = std::array<char, 4096>{};
		auto reserve = [&] {
			for (auto remaining = (uint64_t)table_bytes; remaining; ) {
				const auto n = std::min<uint64_t>( remaining, zeros.size() );
				out.write( zeros.data(), (std::streamsize)n );
				remaining -= n;
			}
		};
		reserve();
		long_header( out, 0x7fe0, 0x0002, "OV", table_bytes );
		const auto lengths_at = out.tellp();
		reserve();
		long_header( out, 0x7fe0, 0x0010, "OB", 0xffffffff );
		tag( out, 0xfffe, 0xe000, 0 );
		const auto first_fragment = out.tellp();
		auto encoded = std::vector<uint8_t>{};
		frame = 0;
		auto encoded_bytes = uint64_t{};
		auto offsets = std::array<uint64_t, frame_block>{};
		auto lengths = std::array<uint64_t, frame_block>{};
		auto buffered = size_t{};
		auto flush_tables = [&] {
			const auto next = out.tellp();
			const auto start = (std::streamoff)( ( frame - buffered ) * sizeof( uint64_t ) );
			out.seekp( offsets_at + start );
			for (auto i = size_t{}; i < buffered; ++i)
				u64( out, offsets[i] );
			out.seekp( lengths_at + start );
			for (auto i = size_t{}; i < buffered; ++i)
				u64( out, lengths[i] );
			out.seekp( next );
			buffered = 0;
		};
		const auto written = each_frame( spec, [&] ( int z, const auto &rect ) {
			if (!frame_source( z, rect, &encoded, error ) || encoded.empty())
				return false;
			if (encoded.size() > uint64_t{ 0xfffffffe } - 1)
				return fail( error, "compressed frame exceeds DICOM item length limit" );
			const auto position = out.tellp();
			tag( out, 0xfffe, 0xe000, (uint32_t)( encoded.size() + encoded.size() % 2 ) );
			out.write( (const char *)encoded.data(), (std::streamsize)encoded.size() );
			if (encoded.size() % 2)
				out.put( 0 );
// Bounded blocks avoid three file seeks for every compressed frame.
			offsets[buffered] = (uint64_t)( position - first_fragment );
			lengths[buffered++] = encoded.size();
			++frame;
			if (buffered == offsets.size())
				flush_tables();
			encoded_bytes += encoded.size();
			return (bool)out || fail( error, "cannot write DICOM frame or offset table" );
		} );
		if (!written)
			return false;
		if (buffered)
			flush_tables();
		tag( out, 0xfffe, 0xe0dd, 0 );
		if (ratio_position >= 0) {
			const auto raw_bytes = (double)frames * spec.tile.w * spec.tile.h * sample_bytes;
			auto ratio = std::format( "{:.8g}", raw_bytes / encoded_bytes );
			ratio.resize( 16, ' ' );
			out.seekp( ratio_position );
			out.write( ratio.data(), ratio.size() );
		}
		out.flush();
		if (!out)
			return fail( error, "cannot finish DICOM instance" );
		impl_->stats.instances.push_back( path );
		if (first)
			impl_->stats.representative = path;
		++impl_->stats.levels_written;
		impl_->stats.tiles_written += frame;
		impl_->stats.encoded_bytes += (long long)encoded_bytes;
		impl_->stats.tiles_empty += ( (int64_t)spec.size.w + spec.tile.w - 1 ) / spec.tile.w *
			( ( (int64_t)spec.size.h + spec.tile.h - 1 ) / spec.tile.h ) * spec.focal_planes - frame;
		impl_->failed = false;
		return true;
	} catch (const std::exception &exception) {
		return fail( error, path.string() + ": " + exception.what() );
	}
#endif
}


bool DcmWriter::add_associated ( const DcmAssociatedImage &associated, const Pwhl<Rgb> &image,
	std::string *error )
{
	if (associated.role == DcmAssociatedRole::None)
		return fail( error, "an associated image needs the label or overview role" );
	if (!image.is_set() || !image.p || image.w > kDcmFrameMaxEdge || image.h > kDcmFrameMaxEdge)
		return fail( error, "associated image is empty or exceeds one DICOM frame" );
	if (associated.mpp < 0.0 || !std::isfinite( associated.mpp ))
		return fail( error, "associated image spacing is invalid" );
	auto spec = DcmLevelSpec{};
	spec.associated = associated.role;
	spec.barcode = associated.role == DcmAssociatedRole::Label ? associated.barcode : std::string{};
	spec.spacing_measured = associated.mpp > 0.0;
	spec.mpp = spec.spacing_measured ? associated.mpp : kDcmUnmeasuredPixelSpacingUm;
	spec.pixel_type = LevelPixelType::Rgb8;
	spec.max_pixel_value = 255;
	spec.size = spec.tile = { image.w, image.h };
#if USE_OPENJPH
	spec.codec = ImgCodec::Htj2k;
#else
	spec.codec = ImgCodec::Jp2k;
#endif
	spec.encode.quality = kImageLosslessQuality;
	const auto fetch = DcmTileFetch<Rgb>( [&image] ( const Rtlbr<int> &, Pwhl<Rgb> *out, std::string * ) {
		for (auto y = 0; y < image.h; ++y)
			std::copy( image.rowp( y ), image.rowp( y ) + image.w, out->rowp( y ) );
		return DcmFetchResult::Pixels;
	} );
	return add_level<Rgb>( spec, fetch, error );
}


bool DcmWriter::close ( std::string *error )
{
	if (!impl_->opened)
		return !impl_->failed;
	impl_->opened = false;
	if (impl_->failed || impl_->stats.instances.empty())
		return fail( error, "DICOM series is incomplete" );
	return true;
}


const DcmWriteStats &DcmWriter::stats () const { return impl_->stats; }


template bool DcmWriter::add_level<Rgb> ( const DcmLevelSpec &, const DcmTileFetch<Rgb> &, std::string * );
template bool DcmWriter::add_level<Rgb16> ( const DcmLevelSpec &, const DcmTileFetch<Rgb16> &, std::string * );
template bool DcmWriter::add_level<uint8_t> ( const DcmLevelSpec &, const DcmTileFetch<uint8_t> &, std::string * );
template bool DcmWriter::add_level<uint16_t> ( const DcmLevelSpec &, const DcmTileFetch<uint16_t> &, std::string * );

} // namespace priml
