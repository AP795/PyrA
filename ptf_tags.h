#pragma once

// The PTF plane-identity block.
//
// A ".ptf" is a TIFF whose tiles may carry encodings the TIFF spec does not
// admit - JPEG XL today, JPEG in the older standard-compliant files. TIFF has
// nowhere to record which channel and focal plane an IFD belongs to, so a written
// pyramid would silently lose its plane identity: every IFD would read back as
// the one unnamed plane, and a three-channel slide would collapse into a stack of
// levels that only differ by size.
//
// This block is that missing metadata. It is emitted into each IFD's
// ImageDescription by the writer and parsed back by the loader, and it is defined
// HERE, once, so the two cannot drift - a writer and a parser kept in step by
// hand is a lesson this tree has already paid for.
//
// The grammar is deliberately dull: "key=value", one per line, ASCII, leading
// "PTF" line as the marker. An ImageDescription that does not start with that
// marker is not ours - Aperio writes free text there, and the loader must go on
// treating such a file exactly as it did before this header existed.

#include "pyr.h"

#include <charconv>
#include <cstdint>
#include <system_error>
#include <string>
#include <string_view>

namespace priml {

// First line of an ImageDescription we wrote. Nothing else may start this way.
inline constexpr std::string_view kPtfMarker = "PTF";

// Bumped only for a change a previous reader could misread. Adding a key is not
// such a change: unknown keys are ignored, absent keys keep their default.
inline constexpr int kPtfVersion = 1;

// What an IFD is for. "aux" IFDs are the associated images (label / overview /
// macro) and are never levels of a plane ladder.
inline constexpr std::string_view kPtfRoleAnalysis = "analysis";
inline constexpr std::string_view kPtfRoleAux = "aux";


struct PtfPlaneTags {
//
// False when the description carried no PTF marker. Every other field is then
// meaningless and the caller must fall back to its pre-PTF behaviour.
	bool valid = false;
	int version = 0;
//
// Canonical channel name - "R", "G", "B", "DAPI". Empty is legal and means the
// slide's single unnamed plane, which is what PlaneSel defaults to.
	std::string channel;
//
// The raw acquisition mode string the channel name was derived from ("RED10").
// Kept beside the conclusion as evidence, the same way pyr keeps "aperio.MPP"
// beside "pyra.mpp-x". Never parsed back into a decision.
	std::string mode;
//
// For a channel that was co-registered onto the reference channel at conversion
// (a filter-wheel acquisition's chromatic map): "ref=R a=.. b=.. c=.. d=..
// tx=.. ty=.. tiles=N rms=.." on the base level only. Evidence, like `mode`:
// recorded so a later reader can say what was done to the pixels, never parsed
// back into a decision.
	std::string registration;
//
// For a channel whose illumination shading was divided out at conversion:
// "ref=.. coef=[..] tiles=N rms=.." on the base level only. Evidence, as above.
	std::string flat_field;
//
// Where each acquisition field of a stitched mosaic was placed, on the first
// level IFD only: "<map> | <field id>:<x>,<y> ...". Unlike the two above this one
// IS read back - the converter's caller rebuilds zones from it - because the
// pixels sit where it says and nowhere else records that. Opaque to pyr.
	std::string field_placement;
	float zlevel = 0.0f;
//
// Microns per pixel of THIS IFD, not of the base level. The loader derives
// Level::metric from the ladder anyway; this is the writer stating what it meant.
	double mpp = 0.0;
//
// Largest value a sample actually takes - 4095 for 12-bit sensor data in a 16-bit
// container. 0 means unstated, in which case the full range of the sample type is
// the only thing a reader may assume. This is what sizes a display lookup table;
// without it a 12-bit plane reads as a very dark 16-bit one.
	int max_pixel_value = 0;
	LevelPixelType pixel_type = LevelPixelType::Rgb8;
//
// The pixel-type spelling when this reader does not know it - a newer writer's
// type. Empty otherwise. Such an IFD must not be read as Rgb8: its tiles decode
// to something else, and reading them as 8-bit colour would be silently wrong.
	std::string unknown_pixel_type;
	std::string role{ kPtfRoleAnalysis };
//
// For role=aux only: the canonical associated-image name ("frost", "overview",
// "macro"). Empty for a level.
	std::string aux_name;
};


// Name <-> LevelPixelType. pixel_type_name() in pyr.h is for humans reading a log
// line; this pair is the on-disk spelling and has to round-trip exactly, so it
// does not borrow that one.
inline std::string_view ptf_pixel_type_name ( LevelPixelType type ) noexcept
{
	switch (type) {
	case LevelPixelType::Rgb8: return "rgb8";
	case LevelPixelType::Gray8: return "gray8";
	case LevelPixelType::Gray16: return "gray16";
	case LevelPixelType::Float32: return "float32";
	case LevelPixelType::Rgb16: return "rgb16";
	}
	return "rgb8";
}


inline bool ptf_pixel_type_from_name ( std::string_view name,
	LevelPixelType *out ) noexcept
{
	if (!out)
		return false;
	if (name == "rgb8") { *out = LevelPixelType::Rgb8; return true; }
	if (name == "gray8") { *out = LevelPixelType::Gray8; return true; }
	if (name == "gray16") { *out = LevelPixelType::Gray16; return true; }
	if (name == "float32") { *out = LevelPixelType::Float32; return true; }
	if (name == "rgb16") { *out = LevelPixelType::Rgb16; return true; }
	return false;
}


namespace ptf_detail {

inline std::string_view trim ( std::string_view s ) noexcept
{
	while (!s.empty() && ( s.front() == ' ' || s.front() == '\t' ||
		s.front() == '\r' || s.front() == '\n' ))
	{
		s.remove_prefix( 1 );
	}
	while (!s.empty() && ( s.back() == ' ' || s.back() == '\t' ||
		s.back() == '\r' || s.back() == '\n' ))
	{
		s.remove_suffix( 1 );
	}
	return s;
}


// A value may not contain a newline - that would forge a second key. Writers hand
// us channel and mode names read out of a vendor sidecar, so this is sanitising
// untrusted input, not defending against ourselves.
inline std::string sanitise ( std::string_view value )
{
	auto out = std::string{};
	out.reserve( value.size() );
	for (auto ch : value) {
		if (ch == '\n' || ch == '\r')
			continue;
		out.push_back( ch );
	}
	return ptf_detail::trim( out ).empty() ? std::string{} : out;
}


template <typename T>
bool parse_number ( std::string_view text, T *out ) noexcept
{
	if (!out || text.empty())
		return false;
	auto value = T{};
	const auto *first = text.data();
	const auto *last = first + text.size();
	auto result = std::from_chars( first, last, value );
	if (result.ec != std::errc{} || result.ptr != last)
		return false;
	*out = value;
	return true;
}


template <typename T>
std::string format_number ( T value )
{
	char buffer[48];
	auto result = std::to_chars( buffer, buffer + sizeof( buffer ), value );
	if (result.ec != std::errc{})
		return {};
	return std::string( buffer, result.ptr );
}

} // namespace ptf_detail


// Render the block for one IFD's ImageDescription.
//
// Keys are emitted in a fixed order so two writes of the same plane produce
// byte-identical bytes; a round-trip test that diffs whole files depends on it.
inline std::string format_ptf_tags ( const PtfPlaneTags &tags )
{
	auto out = std::string( kPtfMarker );
	out += '\n';
	out += "version=" + std::to_string( kPtfVersion ) + '\n';
	out += "role=" + ( tags.role.empty()
		? std::string( kPtfRoleAnalysis ) : ptf_detail::sanitise( tags.role ) ) + '\n';
	out += "pixel-type=" + std::string( ptf_pixel_type_name( tags.pixel_type ) ) + '\n';

	auto channel = ptf_detail::sanitise( tags.channel );
	if (!channel.empty())
		out += "channel=" + channel + '\n';

	auto mode = ptf_detail::sanitise( tags.mode );
	if (!mode.empty())
		out += "mode=" + mode + '\n';

	auto aux_name = ptf_detail::sanitise( tags.aux_name );
	if (!aux_name.empty())
		out += "aux-name=" + aux_name + '\n';

	auto registration = ptf_detail::sanitise( tags.registration );
	if (!registration.empty())
		out += "chroma-reg=" + registration + '\n';

	auto flat_field = ptf_detail::sanitise( tags.flat_field );
	if (!flat_field.empty())
		out += "flat-field=" + flat_field + '\n';

	auto field_placement = ptf_detail::sanitise( tags.field_placement );
	if (!field_placement.empty())
		out += "field-placement=" + field_placement + '\n';

	if (tags.zlevel != 0.0f)
		out += "zlevel=" + ptf_detail::format_number( tags.zlevel ) + '\n';
//
// to_chars' shortest form round-trips an IEEE value exactly and, unlike printf,
// is locale-independent - a machine set to a comma decimal separator would
// otherwise write an "mpp=0,735" no reader of ours could parse. The metric is the
// one number here that a later comparison ("do these two planes share a ladder?")
// tests for equality, so it may not lose a bit on the way out.
	if (tags.mpp > 0.0)
		out += "mpp=" + ptf_detail::format_number( tags.mpp ) + '\n';
	if (tags.max_pixel_value > 0)
		out += "max-pixel-value=" + std::to_string( tags.max_pixel_value ) + '\n';
	return out;
}


// Parse an IFD's ImageDescription. Returns valid==false - leaving every other
// field at its default - for any description that is not one of ours, which is
// the case for every file written before this format existed.
inline PtfPlaneTags parse_ptf_tags ( std::string_view description )
{
	auto tags = PtfPlaneTags{};
	auto rest = description;
//
// The marker must be the whole first line. "PTF-something" is not us, and neither
// is a vendor description that merely happens to begin with those three letters.
	auto first_end = rest.find( '\n' );
	auto first_line = ptf_detail::trim(
		first_end == std::string_view::npos ? rest : rest.substr( 0, first_end ) );
	if (first_line != kPtfMarker)
		return tags;
	if (first_end == std::string_view::npos)
		rest = {};
	else
		rest.remove_prefix( first_end + 1 );

	tags.valid = true;
	while (!rest.empty()) {
		auto line_end = rest.find( '\n' );
		auto line = ptf_detail::trim(
			line_end == std::string_view::npos ? rest : rest.substr( 0, line_end ) );
		if (line_end == std::string_view::npos)
			rest = {};
		else
			rest.remove_prefix( line_end + 1 );
		if (line.empty())
			continue;

		auto eq = line.find( '=' );
		if (eq == std::string_view::npos)
			continue;
		auto key = ptf_detail::trim( line.substr( 0, eq ) );
		auto value = ptf_detail::trim( line.substr( eq + 1 ) );

		if (key == "version")
			ptf_detail::parse_number( value, &tags.version );
		else if (key == "role")
			tags.role.assign( value );
		else if (key == "pixel-type") {
			if (!ptf_pixel_type_from_name( value, &tags.pixel_type ))
				tags.unknown_pixel_type.assign( value.empty() ? "(empty)" : value );
		}
		else if (key == "channel")
			tags.channel.assign( value );
		else if (key == "mode")
			tags.mode.assign( value );
		else if (key == "aux-name")
			tags.aux_name.assign( value );
		else if (key == "chroma-reg")
			tags.registration.assign( value );
		else if (key == "flat-field")
			tags.flat_field.assign( value );
		else if (key == "field-placement")
			tags.field_placement.assign( value );
		else if (key == "zlevel")
			ptf_detail::parse_number( value, &tags.zlevel );
		else if (key == "mpp")
			ptf_detail::parse_number( value, &tags.mpp );
		else if (key == "max-pixel-value")
			ptf_detail::parse_number( value, &tags.max_pixel_value );
//
// Anything else is a key a later version added. Ignore it: that is what makes
// adding a key a compatible change.
	}
	return tags;
}


// True when this IFD is a level of a plane ladder rather than an associated image.
inline bool ptf_is_level ( const PtfPlaneTags &tags ) noexcept
{
	return tags.valid && tags.role != kPtfRoleAux;
}


// True when this IFD is a level whose pixel type this reader cannot decode to.
inline bool ptf_level_type_unknown ( const PtfPlaneTags &tags ) noexcept
{
	return ptf_is_level( tags ) && !tags.unknown_pixel_type.empty();
}

} // namespace priml
