// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "dcm_series.h"

#include "build_config.h"
#include "dcm_write.h"

#if USE_DCMTK
#include "pyr_win.h"
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcuid.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <system_error>

namespace priml {

std::filesystem::path dcm_pyramid_aux_directory ( const std::filesystem::path &entry )
{
	auto name = std::filesystem::path( "_" );
	name += entry.stem();
	name += "_";
	return entry.parent_path() / name;
}


std::vector<std::filesystem::path> dcm_series_paths ( const std::filesystem::path &input )
{
	const auto opened = std::filesystem::absolute( input ).lexically_normal();
	auto result = std::vector<std::filesystem::path>{ opened };
	const auto append = [&]( const std::filesystem::path &directory ) {
		auto ec = std::error_code{};
		for (const auto &file : std::filesystem::directory_iterator( directory, ec )) {
			auto extension = file.path().extension().string();
			std::transform( extension.begin(), extension.end(), extension.begin(),
				[]( unsigned char c ) { return (char)std::tolower( c ); } );
			if (file.is_regular_file( ec ) && extension == ".dcm" && file.path() != opened)
				result.push_back( file.path() );
		}
	};
	append( opened.parent_path() );
	append( dcm_pyramid_aux_directory( opened ) );
	const auto directory = opened.parent_path();
	const auto name = directory.filename().string();
	if (name.size() > 2 && name.front() == '_' && name.back() == '_') {
		const auto entry = directory.parent_path() / (name.substr( 1, name.size() - 2 ) + ".dcm");
		auto ec = std::error_code{};
		if (std::filesystem::is_regular_file( entry, ec )) result.push_back( entry );
	}
	std::sort( result.begin() + 1, result.end() );
	result.erase( std::unique( result.begin(), result.end() ), result.end() );
	return result;
}


bool publish_dcm_pyramid ( const std::filesystem::path &input, DcmWriteStats *stats,
	std::string *error, const std::function<bool ()> &cancelled )
{
	const auto fail = [&]( const std::string &message ) { if (error) *error = message; return false; };
	if (!stats || stats->instances.empty() || stats->representative != stats->instances.front())
		return fail( "DICOM publication requires a verified staged representative" );
	const auto entry = std::filesystem::absolute( input ).lexically_normal();
	const auto auxiliary = dcm_pyramid_aux_directory( entry );
	auto destinations = std::vector<std::filesystem::path>{ entry };
	for (auto i = size_t{ 1 }; i < stats->instances.size(); ++i)
		destinations.push_back( auxiliary / stats->instances[i].filename() );
	auto ec = std::error_code{};
	for (auto i = size_t{}; i < destinations.size(); ++i) {
		if (stats->instances[i].parent_path() != stats->representative.parent_path() ||
			!std::filesystem::is_regular_file( stats->instances[i], ec ))
			return fail( "missing or inconsistent staged DICOM object" );
		if (std::filesystem::exists( destinations[i], ec ) || ec)
			return fail( "DICOM output already exists or cannot be inspected: " + destinations[i].string() );
	}
	struct Publication {
		const std::vector<std::filesystem::path> &destinations;
		std::filesystem::path auxiliary;
		std::filesystem::path lock;
		std::vector<size_t> placed;
		bool made_directory = false;
		bool owns_lock = false;
		bool committed = false;
		~Publication () {
			auto ignored = std::error_code{};
			if (!committed)
				for (auto i = placed.rbegin(); i != placed.rend(); ++i) std::filesystem::remove( destinations[*i], ignored );
			if (owns_lock) std::filesystem::remove( lock, ignored );
			if (made_directory && !committed) std::filesystem::remove( auxiliary, ignored );
		}
	};
	auto publication = Publication{ destinations, {}, {}, {} };
	publication.auxiliary = auxiliary;
	publication.lock = auxiliary / ".dicom-publish";
	publication.placed.reserve( destinations.size() );
	publication.made_directory = std::filesystem::create_directory( auxiliary, ec );
	if (ec) return fail( "cannot create DICOM auxiliary directory: " + ec.message() );
	publication.owns_lock = std::filesystem::create_directory( publication.lock, ec );
	if (!publication.owns_lock || ec) return fail( "DICOM publication directory is busy or unavailable" );
	const auto move = [&]( size_t i ) {
		if (cancelled && cancelled()) return fail( "DICOM publication cancelled" );
		if (std::filesystem::exists( destinations[i], ec ) || ec)
			return fail( "refusing to replace a DICOM output: " + destinations[i].string() );
		std::filesystem::rename( stats->instances[i], destinations[i], ec );
		if (ec) return fail( "cannot publish DICOM object: " + ec.message() );
		publication.placed.push_back( i );
		return true;
	};
	for (auto i = size_t{ 1 }; i < destinations.size(); ++i)
		if (!move( i )) return false;
	if (!move( 0 )) return false;
	stats->instances = std::move( destinations );
	stats->representative = entry;
	publication.committed = true;
	return true;
}

std::filesystem::path dcm_pyramid_entry ( const std::filesystem::path &member )
{
#if USE_DCMTK
	const auto directory = member.parent_path();
	const auto name = directory.filename().string();
	const auto entry = name.size() > 2 && name.front() == '_' && name.back() == '_'
		? directory.parent_path() / (name.substr( 1, name.size() - 2 ) + ".dcm") : member;
	auto ec = std::error_code{};
	if (!std::filesystem::is_regular_file( member, ec ) ||
		!std::filesystem::is_regular_file( entry, ec ))
		return {};
	auto load = [] ( const auto &path, auto &file ) {
// Stop before optical paths and functional groups; tree enumeration needs no
// frame metadata, compressed image samples, or ICC profiles.
		if (file.loadFileUntilTag( path.string().c_str(), EXS_Unknown, EGL_noChange,
			4096, ERM_autoDetect, DCM_OpticalPathSequence ).bad())
			return false;
		auto *ds = file.getDataset();
		auto manufacturer = OFString{}, model = OFString{}, sop = OFString{};
		ds->findAndGetOFString( DCM_Manufacturer, manufacturer );
		ds->findAndGetOFString( DCM_ManufacturerModelName, model );
		ds->findAndGetOFString( DCM_SOPClassUID, sop );
		const auto &identity = dcm_writer_identity();
		return manufacturer == identity.manufacturer.c_str() && model == identity.model.c_str() &&
			sop == UID_VLWholeSlideMicroscopyImageStorage;
	};
	auto selected = DcmFileFormat{}, representative = DcmFileFormat{};
	if (!load( member, selected ))
		return {};
	if (member == entry) return entry;
	if (!load( entry, representative )) return {};
	for (const auto &key : { DCM_StudyInstanceUID, DCM_SeriesInstanceUID,
		DCM_PyramidUID, DCM_FrameOfReferenceUID }) {
		auto a = OFString{}, b = OFString{};
		selected.getDataset()->findAndGetOFString( key, a );
		representative.getDataset()->findAndGetOFString( key, b );
//
// A label or overview carries no Pyramid UID - the standard forbids it outside
// VOLUME and THUMBNAIL - and belongs to the series all the same.
		if (key == DCM_PyramidUID && a.empty())
			continue;
		if (a.empty() || a != b)
			return {};
	}
	return entry;
#else
	(void)member;
	return {};
#endif
}


namespace {

#if USE_DCMTK
std::string dcm_text ( DcmItem *item, const DcmTagKey &key, unsigned long position = 0 )
{
	auto value = OFString{};
	if (!item || item->findAndGetOFString( key, value, position ).bad())
		return {};
	return value.c_str();
}


std::string context_text ( DcmItem *ds, const char *concept_value )
{
	DcmSequenceOfItems *sequence = nullptr;
	if (ds->findAndGetSequence( DCM_AcquisitionContextSequence, sequence ).bad() || !sequence)
		return {};
	for (auto *object = sequence->nextInContainer( nullptr ); object; object = sequence->nextInContainer( object )) {
		auto *entry = OFstatic_cast( DcmItem *, object );
		DcmItem *name = nullptr;
		if (entry->findAndGetSequenceItem( DCM_ConceptNameCodeSequence, name ).good() &&
			dcm_text( name, DCM_CodeValue ) == concept_value)
			return dcm_text( entry, DCM_TextValue );
	}
	return {};
}


// Header and module attributes only: stop before the functional groups, whose
// per-frame sequence alone can hold tens of thousands of items.
bool load_instance_header ( const std::filesystem::path &path, DcmFileFormat &file )
{
	return file.loadFileUntilTag( path.string().c_str(), EXS_Unknown, EGL_noChange,
		4096, ERM_autoDetect, DCM_SharedFunctionalGroupsSequence ).good();
}
#endif


bool series_fail ( std::string *error, std::string message )
{
	if (error)
		*error = std::move( message );
	return false;
}

} // namespace


// The loader's rule (CollectDcmLevels in wsi_dcm.cpp), so a name listed or
// removed here is the name the reader shows.
std::string DcmSeriesInstance::plane_name () const
{
	const auto single = optical_ids.size() <= 1;
	if (samples == 3 && single)
		return {};
	if (samples == 1 && single && !channel_name.empty())
		return channel_name;
	if (!optical_descriptions.empty() && !optical_descriptions.front().empty())
		return optical_descriptions.front();
	return optical_ids.empty() ? std::string{} : optical_ids.front();
}


bool DcmSeriesInstance::derived_channel () const
{
	const auto &identity = dcm_writer_identity();
	return manufacturer == identity.manufacturer && model == identity.model &&
		processing.starts_with( kDcmDerivedChannelMarker );
}


bool dcm_series_instances ( const std::filesystem::path &entry,
	std::vector<DcmSeriesInstance> *instances, std::string *error )
{
	if (!instances)
		return series_fail( error, "instance list is null" );
	instances->clear();
#if !USE_DCMTK
	(void)entry;
	return series_fail( error, "DICOM support was not included in this build" );
#else
	auto series_uid = std::string{};
	const auto paths = dcm_series_paths( entry );
	for (const auto &path : paths) {
		auto file = DcmFileFormat{};
		if (!load_instance_header( path, file )) {
			if (path == paths.front())
				return series_fail( error, "cannot read DICOM instance: " + path.string() );
			continue;
		}
		auto *ds = file.getDataset();
		const auto uid = dcm_text( ds, DCM_SeriesInstanceUID );
		if (series_uid.empty())
			series_uid = uid;
		if (uid.empty() || uid != series_uid)
			continue;
		auto instance = DcmSeriesInstance{};
		instance.path = path;
		instance.role = dcm_text( ds, DCM_ImageType, 2 );
		auto samples = Uint16{};
		instance.samples = ds->findAndGetUint16( DCM_SamplesPerPixel, samples ).good() ? samples : 0;
		instance.manufacturer = dcm_text( ds, DCM_Manufacturer );
		instance.model = dcm_text( ds, DCM_ManufacturerModelName );
		instance.channel_name = context_text( ds, "CHANNEL_NAME" );
		instance.processing = context_text( ds, "PROCESSING" );
		DcmSequenceOfItems *optical = nullptr;
		if (ds->findAndGetSequence( DCM_OpticalPathSequence, optical ).good() && optical)
			for (auto *object = optical->nextInContainer( nullptr ); object; object = optical->nextInContainer( object )) {
				auto *item = OFstatic_cast( DcmItem *, object );
				instance.optical_ids.push_back( dcm_text( item, DCM_OpticalPathIdentifier ) );
				instance.optical_descriptions.push_back( dcm_text( item, DCM_OpticalPathDescription ) );
			}
		instances->push_back( std::move( instance ) );
	}
	if (series_uid.empty())
		return series_fail( error, "not a DICOM slide with a Series Instance UID: " + entry.string() );
	return true;
#endif
}


bool dcm_slide_acquisition ( const std::filesystem::path &entry, DcmAcquisition *meta,
	Pxy<double> *origin_um, std::string *error )
{
	if (!meta || !origin_um)
		return series_fail( error, "acquisition output is null" );
#if !USE_DCMTK
	(void)entry;
	return series_fail( error, "DICOM support was not included in this build" );
#else
	auto file = DcmFileFormat{};
	if (!load_instance_header( entry, file ))
		return series_fail( error, "cannot read DICOM instance: " + entry.string() );
	auto *ds = file.getDataset();
	auto result = DcmAcquisition{};
	result.study_uid = dcm_text( ds, DCM_StudyInstanceUID );
	result.series_uid = dcm_text( ds, DCM_SeriesInstanceUID );
	result.pyramid_uid = dcm_text( ds, DCM_PyramidUID );
	result.frame_of_reference_uid = dcm_text( ds, DCM_FrameOfReferenceUID );
	if (result.study_uid.empty() || result.series_uid.empty() || result.frame_of_reference_uid.empty())
		return series_fail( error, "slide lacks a study, series or frame-of-reference UID: " + entry.string() );
	result.patient_name = dcm_text( ds, DCM_PatientName );
	result.patient_id = dcm_text( ds, DCM_PatientID );
	result.container_identifier = dcm_text( ds, DCM_ContainerIdentifier );
	result.series_description = dcm_text( ds, DCM_SeriesDescription );
	DcmItem *specimen = nullptr;
	if (ds->findAndGetSequenceItem( DCM_SpecimenDescriptionSequence, specimen ).good() && specimen) {
		result.specimen_id = dcm_text( specimen, DCM_SpecimenIdentifier );
		result.specimen_uid = dcm_text( specimen, DCM_SpecimenUID );
	}
//
// Acquisition date/time is required by the writer; fall back to the content
// date and time, then to now, so a vendor file that omits it can still be joined.
	result.acquisition_datetime = dcm_text( ds, DCM_AcquisitionDateTime );
	if (result.acquisition_datetime.size() < 14) {
		auto time = dcm_text( ds, DCM_ContentTime );
		time = time.substr( 0, std::min<size_t>( time.size(), 6 ) );
		result.acquisition_datetime = dcm_text( ds, DCM_ContentDate ) + time;
	}
	if (result.acquisition_datetime.size() < 14 ||
		DcmDateTime::checkStringValue( result.acquisition_datetime.c_str(), "1" ).bad())
	{
		const auto now = std::chrono::floor<std::chrono::seconds>( std::chrono::system_clock::now() );
		result.acquisition_datetime = std::format( "{:%Y%m%d%H%M%S}", now );
	}
	result.focus_method = dcm_text( ds, DCM_FocusMethod );
	if (result.focus_method != "AUTO" && result.focus_method != "MANUAL")
		result.focus_method = "AUTO";
	auto depth = Float32{};
	result.imaged_depth_um = ds->findAndGetFloat32( DCM_ImagedVolumeDepth, depth ).good() &&
		std::isfinite( depth ) && depth > 0 ? depth : 1.0;
	if (dcm_text( ds, DCM_ExtendedDepthOfField ) == "YES") {
		auto planes = Uint16{};
		auto spacing = Float32{};
		if (ds->findAndGetUint16( DCM_NumberOfFocalPlanes, planes ).good() && planes >= 2 &&
			ds->findAndGetFloat32( DCM_DistanceBetweenFocalPlanes, spacing ).good() && spacing > 0)
			result.extended_depth = DcmExtendedDepth{ planes, spacing };
	}
	result.illumination_color = "WHITE";
	DcmItem *optical = nullptr;
	if (ds->findAndGetSequenceItem( DCM_OpticalPathSequence, optical ).good() && optical) {
		DcmItem *code = nullptr;
		if (optical->findAndGetSequenceItem( DCM_IlluminationColorCodeSequence, code ).good() && code) {
			const auto color = dcm_text( code, DCM_CodeValue );
			if (!color.empty() && color.size() <= 16)
				result.illumination_color = color;
		}
		if (optical->findAndGetSequenceItem( DCM_IlluminationTypeCodeSequence, code ).good() && code) {
			auto meaning = dcm_text( code, DCM_CodeMeaning );
			std::transform( meaning.begin(), meaning.end(), meaning.begin(),
				[]( unsigned char c ) { return (char)std::tolower( c ); } );
			result.fluorescence = dcm_text( code, DCM_CodeValue ) == "FLUORESCENCE" ||
				meaning.find( "fluoresc" ) != std::string::npos;
		}
	}
	*origin_um = { 0.0, 0.0 };
	DcmItem *origin = nullptr;
	if (ds->findAndGetSequenceItem( DCM_TotalPixelMatrixOriginSequence, origin ).good() && origin) {
		auto x = Float64{}, y = Float64{};
		if (origin->findAndGetFloat64( DCM_XOffsetInSlideCoordinateSystem, x ).good() &&
			origin->findAndGetFloat64( DCM_YOffsetInSlideCoordinateSystem, y ).good() &&
			std::isfinite( x ) && std::isfinite( y ))
			*origin_um = { x * 1000.0, y * 1000.0 };
	}
	*meta = std::move( result );
	return true;
#endif
}


bool publish_dcm_companions ( const std::filesystem::path &input, DcmWriteStats *stats,
	std::string *error )
{
	if (!stats || stats->instances.empty())
		return series_fail( error, "nothing staged to publish" );
	const auto entry = std::filesystem::absolute( input ).lexically_normal();
	auto ec = std::error_code{};
	const auto auxiliary = dcm_pyramid_aux_directory( entry );
	const auto directory = std::filesystem::is_directory( auxiliary, ec ) ? auxiliary : entry.parent_path();
	auto destinations = std::vector<std::filesystem::path>{};
	for (const auto &staged : stats->instances) {
		if (!std::filesystem::is_regular_file( staged, ec ))
			return series_fail( error, "missing staged DICOM object: " + staged.string() );
		destinations.push_back( directory / staged.filename() );
		if (std::filesystem::exists( destinations.back(), ec ) || ec)
			return series_fail( error, "DICOM output already exists or cannot be inspected: " +
				destinations.back().string() );
	}
	struct Publication {
		std::vector<std::filesystem::path> placed;
		std::filesystem::path lock;
		bool owns_lock = false;
		bool committed = false;
		~Publication () {
			auto ignored = std::error_code{};
			if (!committed)
				for (auto i = placed.rbegin(); i != placed.rend(); ++i)
					std::filesystem::remove( *i, ignored );
			if (owns_lock)
				std::filesystem::remove( lock, ignored );
		}
	};
	auto publication = Publication{};
	publication.lock = directory / ".dicom-publish";
	publication.owns_lock = std::filesystem::create_directory( publication.lock, ec );
	if (!publication.owns_lock || ec)
		return series_fail( error, "DICOM publication directory is busy or unavailable" );
	for (auto i = size_t{}; i < destinations.size(); ++i) {
		if (std::filesystem::exists( destinations[i], ec ) || ec)
			return series_fail( error, "refusing to replace a DICOM output: " + destinations[i].string() );
		std::filesystem::rename( stats->instances[i], destinations[i], ec );
		if (ec)
			return series_fail( error, "cannot publish DICOM object: " + ec.message() );
		publication.placed.push_back( destinations[i] );
	}
	stats->instances = std::move( destinations );
	stats->representative = stats->instances.front();
	publication.committed = true;
	return true;
}


bool remove_dcm_channel ( const std::filesystem::path &input, const std::string &channel,
	std::vector<std::filesystem::path> *removed, std::string *error )
{
	if (removed)
		removed->clear();
	if (channel.empty())
		return series_fail( error, "a channel name is required" );
	const auto entry = std::filesystem::absolute( input ).lexically_normal();
	auto instances = std::vector<DcmSeriesInstance>{};
	if (!dcm_series_instances( entry, &instances, error ))
		return false;
	auto doomed = std::vector<std::filesystem::path>{};
	for (const auto &instance : instances) {
		if (instance.role != "VOLUME" || instance.plane_name() != channel)
			continue;
		if (instance.path == entry)
			return series_fail( error, "channel '" + channel + "' holds " + instance.path.filename().string() +
				", the file the slide was opened through; open it through another instance. Nothing was removed" );
		if (!instance.derived_channel())
			return series_fail( error, "channel '" + channel + "' holds " + instance.path.filename().string() +
				", which is not a derived channel added by this writer; nothing was removed" );
		doomed.push_back( instance.path );
	}
	if (doomed.empty())
		return series_fail( error, "the slide has no channel '" + channel + "'" );
	auto ec = std::error_code{};
	for (const auto &path : doomed) {
		if (!std::filesystem::remove( path, ec ) || ec)
			return series_fail( error, "cannot remove " + path.string() + ": " + ec.message() );
		if (removed)
			removed->push_back( path );
	}
	return true;
}

}
