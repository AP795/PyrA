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
	for (const auto key : { DCM_StudyInstanceUID, DCM_SeriesInstanceUID,
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

}
