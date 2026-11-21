// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "ipx_layout.h"

#include "ipx_imd.h"
#include "ipx_names.h"
#include "pyr_rt.h"
#include "pyr_text.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <utility>

namespace priml {

namespace {

bool set_error_local ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	return false;
}


bool has_extension_local ( const std::filesystem::path &path, const wchar_t *ext )
{
	auto have = path.extension().wstring();
	if (have.size() != std::wcslen( ext ))
		return false;
	for (size_t i = 0; i < have.size(); ++i) {
		if (std::towlower( have[i] ) != std::towlower( ext[i] ))
			return false;
	}
	return true;
}


// R, then G, then B, then everything else alphabetically. The order planes are
// already presented in everywhere else; keeping it here means a converted slide's
// channels come out in the same order the legacy path produced them.
int channel_rank_local ( const std::string &name )
{
	auto lowered = lower_ascii_copy( name );
	if (lowered == "r") return 0;
	if (lowered == "g") return 1;
	if (lowered == "b") return 2;
	return 3;
}


bool channel_less_local ( const std::string &a, const std::string &b )
{
	auto ra = channel_rank_local( a );
	auto rb = channel_rank_local( b );
	if (ra != rb)
		return ra < rb;
	return lower_ascii_copy( a ) < lower_ascii_copy( b );
}


int direct_rgb_axis_local ( const std::array<std::uint8_t, 3> &coefficients )
{
	constexpr auto basis = std::array{
		std::array<std::uint8_t, 3>{ 255, 0, 0 },
		std::array<std::uint8_t, 3>{ 0, 255, 0 },
		std::array<std::uint8_t, 3>{ 0, 0, 255 } };
	for (auto axis = 0; axis < (int)basis.size(); ++axis)
		if (coefficients == basis[axis])
			return axis;
	return -1;
}


std::array<std::string, 3> direct_rgb_components_local (
	const std::vector<IpxField> &fields, const std::vector<std::string> &channels,
	const std::map<std::string, std::array<std::uint8_t, 3>> &coefficients,
	std::string_view metadata_mismatch, std::string *reason )
{
	auto reject = [reason] ( std::string why ) {
		if (reason)
			*reason = std::move( why );
		return std::array<std::string, 3>{};
	};
	if (!metadata_mismatch.empty())
		return reject( std::string( metadata_mismatch ) );
	if (channels.size() != 3)
		return reject( std::format( "{} channels, expected exactly 3", channels.size() ) );

	auto components = std::array<std::string, 3>{};
	for (const auto &channel : channels) {
		auto it = coefficients.find( channel );
		if (it == coefficients.end())
			return reject( "missing RGB coefficients for modality '" + channel + "'" );
		auto axis = direct_rgb_axis_local( it->second );
		if (axis < 0) {
			return reject( std::format( "modality '{}' has coefficients ({},{},{})",
				channel, it->second[0], it->second[1], it->second[2] ) +
				"; expected one of (255,0,0), (0,255,0), (0,0,255)" );
		}
		if (!components[axis].empty())
			return reject( std::format( "modalities '{}' and '{}' both map to component {}",
				components[axis], channel, "RGB"[axis] ) );
		components[axis] = channel;
	}
	for (const auto &field : fields) {
		for (const auto &component : components) {
			if (component.empty() || !field.planes.contains( component )) {
				return reject( std::format(
					"field {} ('{}') is missing RGB source channel '{}'",
					field.scene_id, field.imd_zone_name, component ) );
			}
		}
	}
	if (reason)
		reason->clear();
	return components;
}

} // namespace


void sort_ipx_fields ( std::vector<IpxField> *fields )
{
	std::sort( fields->begin(), fields->end(),
		[] ( const IpxField &a, const IpxField &b ) {
			if (a.rect.tl.y != b.rect.tl.y)
				return a.rect.tl.y < b.rect.tl.y;
			if (a.rect.tl.x != b.rect.tl.x)
				return a.rect.tl.x < b.rect.tl.x;
			return a.scene_id < b.scene_id;
		} );
}


bool scan_ipx_file_set ( const std::filesystem::path &directory,
	std::string_view slide_name, IpxLayout *out, std::string *error )
{
	if (error)
		error->clear();
	if (!out)
		return set_error_local( error, "layout output is null" );
	*out = IpxLayout{};

	auto ec = std::error_code{};
	auto members = std::vector<std::filesystem::path>{};
	auto source_directory = directory;
//
// The slide's own directory first, then the subdirectory the members are moved
// into once a conversion has been verified. Without the second look, moving the
// sources aside - which is the whole point of converting - would leave the slide
// unable to rebuild its own footprint or labels ever again.
	auto candidates = std::vector<std::filesystem::path>{ directory };
	for (auto name : kIpxSourceSubdirectories)
		candidates.push_back( directory / std::filesystem::path( name ) );
	for (const auto &candidate : candidates) {
		for (const auto &entry :
			std::filesystem::directory_iterator( candidate, ec ))
		{
			if (ec)
				break;
			if (entry.is_regular_file() &&
				has_extension_local( entry.path(), L".ipx" ))
			{
				members.push_back( entry.path() );
			}
		}
		ec.clear();
		if (!members.empty()) {
			source_directory = candidate;
			break;
		}
	}
	if (members.empty())
		return set_error_local( error, "no .IPX members in " + directory.string() );
	out->members_present = true;

	auto by_scene = std::map<std::uint32_t, size_t>{};
	auto fields = std::vector<IpxField>{};
	auto channels = std::vector<std::string>{};
	auto rgb_coefficients =
		std::map<std::string, std::array<std::uint8_t, 3>>{};
	auto rgb_metadata_mismatch = std::string{};
	auto rgb_metadata_mismatch_count = size_t{};
	const auto note_rgb_mismatch = [&] ( std::string detail ) {
		++rgb_metadata_mismatch_count;
		if (rgb_metadata_mismatch.empty())
			rgb_metadata_mismatch = std::move( detail );
	};
	auto mpp = 0.0;
	auto resolved_name = std::string( slide_name );

	for (const auto &path : members) {
		auto imd = IpxImdMetadata{};
		if (!load_ipx_imd_metadata( path.wstring(), &imd ) || !imd.loaded)
			continue;
		if (!slide_name.empty() &&
			!equals_ignore_ascii_case( imd.slide_name, slide_name ))
		{
			continue;
		}
		if (resolved_name.empty())
			resolved_name = imd.slide_name;

//
// The gate. Sharing one raster means sharing one pixel size; a set that mixes
// them would have to be resampled, and resampling is exactly what this conversion
// promises not to do. Such a set is refused by name rather than silently
// approximated.
		if (mpp == 0.0) {
			mpp = imd.metric_per_pixel;
		} else if (imd.metric_per_pixel != mpp) {
			return set_error_local( error, std::format(
				"field '{}' is {:.10g} um/px but the set is {:.10g} um/px - "
				"a mixed-resolution file-set cannot be converted without resampling",
				path.filename().string(), imd.metric_per_pixel, mpp ) );
		}


		auto it = by_scene.find( imd.scene_id );
		if (it == by_scene.end()) {
			it = by_scene.emplace( imd.scene_id, fields.size() ).first;
			auto field = IpxField{};
			field.scene_id = imd.scene_id;
			field.imd_zone_name = imd.zone_name;
			field.origin_x_um = imd.origin_x_um;
			field.origin_y_um = imd.origin_y_um;
			field.width = imd.image_width;
			field.height = imd.image_height;
			fields.push_back( std::move( field ) );
		}

		auto &field = fields[it->second];
		auto channel = canonical_ipx_plane_name( imd.plane_name, imd.rgb_channel );
		const auto reference_channel = field.planes.empty() ? channel : field.planes.begin()->first;
		const auto reference_file = field.planes.empty() ? path.filename().string()
			: field.planes.begin()->second.filename().string();
		if (field.width != imd.image_width || field.height != imd.image_height)
			note_rgb_mismatch( std::format(
				"field {} channel '{}' file '{}': dimensions {}x{}, expected {}x{} "
				"from channel '{}' file '{}'", imd.scene_id, channel, path.filename().string(),
				imd.image_width, imd.image_height, field.width, field.height,
				reference_channel, reference_file ) );
		if (field.origin_x_um != imd.origin_x_um || field.origin_y_um != imd.origin_y_um)
			note_rgb_mismatch( std::format(
				"field {} channel '{}' file '{}': IMD image-origin XY=({:.17g},{:.17g}) um, "
				"expected ({:.17g},{:.17g}) um from channel '{}' file '{}'; "
				"delta=({:.17g},{:.17g}) um = ({:.17g},{:.17g}) pixels at {:.17g} um/px",
				imd.scene_id, channel, path.filename().string(), imd.origin_x_um, imd.origin_y_um,
				field.origin_x_um, field.origin_y_um, reference_channel, reference_file,
				imd.origin_x_um - field.origin_x_um, imd.origin_y_um - field.origin_y_um,
				(imd.origin_x_um - field.origin_x_um) / mpp,
				(imd.origin_y_um - field.origin_y_um) / mpp, mpp ) );
		if (const auto previous = field.planes.find( channel ); previous != field.planes.end())
			note_rgb_mismatch( std::format(
				"field {} repeats channel '{}': files '{}' and '{}'",
				imd.scene_id, channel, previous->second.filename().string(), path.filename().string() ) );
		if (auto coefficient = rgb_coefficients.find( channel );
			coefficient == rgb_coefficients.end())
		{
			rgb_coefficients.emplace( channel, imd.rgb_coefficients );
		} else if (coefficient->second != imd.rgb_coefficients) {
			note_rgb_mismatch( std::format(
				"field {} channel '{}' file '{}': RGB coefficients ({},{},{}), "
				"expected ({},{},{}) from earlier records for that channel",
				imd.scene_id, channel, path.filename().string(),
				imd.rgb_coefficients[0], imd.rgb_coefficients[1], imd.rgb_coefficients[2],
				coefficient->second[0], coefficient->second[1], coefficient->second[2] ) );
		}
		field.planes[channel] = path;
		if (std::find( channels.begin(), channels.end(), channel ) == channels.end())
			channels.push_back( channel );
	}

	if (fields.empty()) {
		return set_error_local( error, slide_name.empty()
			? std::string( "no readable .IMD records in " ) + directory.string()
			: std::string( "no fields of slide '" ) + std::string( slide_name ) +
				"' in " + directory.string() );
	}
	if (!( mpp > 0.0 ))
		return set_error_local( error, "the file-set records no pixel size" );

	auto min_x = fields.front().origin_x_um;
	auto min_y = fields.front().origin_y_um;
	for (const auto &field : fields) {
		min_x = (std::min)( min_x, field.origin_x_um );
		min_y = (std::min)( min_y, field.origin_y_um );
	}

//
// Rounded to whole mosaic pixels, which is what makes the conversion a copy: a
// field's samples go in unchanged and only its origin moves, by at most half a
// pixel. Placing at a fractional offset would mean interpolating, and every
// measurement downstream would shift with it.
	auto size = Dim2D<int>{ 0, 0 };
	for (auto &field : fields) {
		auto left = (int)std::lround( ( field.origin_x_um - min_x ) / mpp );
		auto top = (int)std::lround( ( field.origin_y_um - min_y ) / mpp );
		field.rect = { { left, top },
			{ left + field.width, top + field.height } };
		size.w = (std::max)( size.w, field.rect.br.x );
		size.h = (std::max)( size.h, field.rect.br.y );
	}

	sort_ipx_fields( &fields );
	std::sort( channels.begin(), channels.end(), channel_less_local );

	out->valid = true;
	out->slide_name = resolved_name;
	out->directory = directory;
	out->source_directory = source_directory;
	out->mpp = mpp;
	out->origin_x_um = min_x;
	out->origin_y_um = min_y;
	out->size = size;
	out->max_pixel_value = pixel_max_16;
	for (const auto &field : fields)
		for (const auto &[channel, path] : field.planes) {
			auto metadata = IpxImdMetadata{};
			if (load_ipx_imd_metadata( path, &metadata )) {
				if (out->acquisition_datetime.empty() || ( !metadata.acquisition_datetime.empty() &&
					metadata.acquisition_datetime < out->acquisition_datetime ))
					out->acquisition_datetime = metadata.acquisition_datetime;
				out->fluorescence = metadata.fluorescence;
				const auto [entry, inserted] = out->channel_fluorescence.try_emplace( channel, metadata.fluorescence );
				if (!inserted && entry->second != metadata.fluorescence)
					return set_error_local( error, "inconsistent fluorescence metadata within channel: " + channel );
			}
		}
	out->channels = std::move( channels );
	out->fields = std::move( fields );
	auto rgb_reason = std::string{};
	if (rgb_metadata_mismatch_count)
		rgb_metadata_mismatch = std::format( "{} metadata mismatch(es); first: {}",
			rgb_metadata_mismatch_count, rgb_metadata_mismatch );
	out->rgb_components = direct_rgb_components_local( out->fields, out->channels,
		rgb_coefficients, rgb_metadata_mismatch, &rgb_reason );
	if (out->produces_rgb16())
		for (const auto &[channel, fluorescence] : out->channel_fluorescence)
			if (fluorescence != out->fluorescence) {
				rgb_reason = std::format( "channel '{}' has fluorescence={}, but the set's "
					"reference imaging type has fluorescence={}; RGB components must share an imaging type",
					channel, fluorescence, out->fluorescence );
				out->rgb_components = {};
			}

	auto covered = 0.0;
	for (const auto &field : out->fields)
		covered += (double)field.width * field.height;
	pyr::plog( "ipx conversion: scanned '%s' - %zu field(s), %zu channel(s), "
		"%.8g um/px, mosaic %dx%d px, %.1f%% covered, range 0..%d, output=%s\n",
		out->slide_name.c_str(), out->fields.size(), out->channels.size(),
		out->mpp, out->size.w, out->size.h,
		100.0 * covered / ( (double)out->size.w * out->size.h ),
		out->max_pixel_value, out->produces_rgb16() ? "RGB16" : "separate planes" );
	if (!out->produces_rgb16())
		pyr::plog( "ipx conversion: RGB16 gate declined: source IMD metadata "
			"(before stitching/co-registration): %s\n", rgb_reason.c_str() );
	return true;
}

} // namespace priml
