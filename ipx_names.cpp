// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "ipx_names.h"

#include "pyr_text.h"

#include <cstring>

namespace {

template <class CharT>
bool is_hex_char_local ( CharT c )
{
	return (c >= CharT( '0' ) && c <= CharT( '9' )) ||
		(c >= CharT( 'a' ) && c <= CharT( 'f' )) ||
		(c >= CharT( 'A' ) && c <= CharT( 'F' ));
}


template <class CharT>
int channel_from_tag_local ( CharT c )
{
	switch (c) {
		case CharT( 'r' ): case CharT( 'R' ): return 0;
		case CharT( 'g' ): case CharT( 'G' ): return 1;
		case CharT( 'b' ): case CharT( 'B' ): return 2;
		default:
			return -1;
	}
}


// _X_ + 8 hex digits.
constexpr std::size_t kIpxSceneIdLen = 8;
constexpr std::size_t kIpxSuffixLen = 11;


// Splits a triplet member path into [prefix][_X_][scene_id][ext] and returns
// the channel (0/1/2), or -1 when the path is not a triplet member.
template <class CharT>
int parse_triplet_local ( std::basic_string_view<CharT> path,
	std::basic_string<CharT> *prefix,
	std::basic_string<CharT> *scene_id,
	std::basic_string<CharT> *extension )
{
	auto dot = path.find_last_of( CharT( '.' ) );
	if (dot == std::basic_string_view<CharT>::npos)
		return -1;

	auto base = path.substr( 0, dot );
	auto ext = path.substr( dot );
	if (base.size() < kIpxSuffixLen)
		return -1;

	auto scene = base.substr( base.size() - kIpxSceneIdLen );
	for (auto c : scene)
		if (!is_hex_char_local( c ))
			return -1;

	auto tag_pos = base.size() - kIpxSuffixLen;
	if (base[tag_pos] != CharT( '_' ) || base[tag_pos + 2] != CharT( '_' ))
		return -1;

	auto channel = channel_from_tag_local( base[tag_pos + 1] );
	if (channel < 0)
		return -1;

	if (prefix)
		*prefix = std::basic_string<CharT>( base.substr( 0, tag_pos ) );
	if (scene_id)
		*scene_id = std::basic_string<CharT>( scene );
	if (extension)
		*extension = std::basic_string<CharT>( ext );
	return channel;
}


template <class CharT>
std::basic_string<CharT> sibling_path_local (
	const std::basic_string<CharT> &prefix,
	CharT tag,
	const std::basic_string<CharT> &scene_id,
	const std::basic_string<CharT> &extension )
{
	auto out = prefix;
	out.push_back( CharT( '_' ) );
	out.push_back( tag );
	out.push_back( CharT( '_' ) );
	out += scene_id;
	out += extension;
	return out;
}

} // namespace


priml::IpxTripletName priml::parse_ipx_triplet_name ( std::string_view path )
{
	auto name = IpxTripletName{};
	auto channel = parse_triplet_local<char>(
		path, &name.prefix, &name.scene_id, &name.extension );
	if (channel < 0)
		return IpxTripletName{};
	name.valid = true;
	name.channel = channel;
	return name;
}


std::array<std::string, 3> priml::ipx_triplet_paths ( const IpxTripletName &name )
{
	if (!name.valid)
		return {};
	return {
		sibling_path_local<char>( name.prefix, 'R', name.scene_id, name.extension ),
		sibling_path_local<char>( name.prefix, 'G', name.scene_id, name.extension ),
		sibling_path_local<char>( name.prefix, 'B', name.scene_id, name.extension ) };
}


int priml::ipx_triplet_channel ( std::string_view path )
{
	return parse_triplet_local<char>( path, nullptr, nullptr, nullptr );
}


std::string priml::canonical_plane_name ( std::string_view name )
{
	auto trimmed = trim_ascii_copy( name );
	auto lowered = lower_ascii_copy( trimmed );
	if (lowered == "r" || lowered == "img_r" || lowered == "red")
		return "R";
	if (lowered == "g" || lowered == "img_g" || lowered == "green")
		return "G";
	if (lowered == "b" || lowered == "img_b" || lowered == "blue")
		return "B";
	if (lowered == "img")
		return priml::kDefaultImagePlaneName;
	return trimmed;
}


std::string priml::canonical_ipx_plane_name ( std::string_view mode,
	int rgb_channel )
{
	auto trimmed = trim_ascii_copy( mode );
	auto lowered = lower_ascii_copy( trimmed );
	if (lowered == "r" || lowered == "img_r" || lowered.rfind( "red", 0 ) == 0)
		return "R";
	if (lowered == "g" || lowered == "img_g" || lowered.rfind( "green", 0 ) == 0)
		return "G";
	if (lowered == "b" || lowered == "img_b" || lowered.rfind( "blue", 0 ) == 0)
		return "B";
	if (rgb_channel >= 0 &&
		( lowered.empty() || lowered == "img" || lowered == "image" ))
	{
		return rgb_channel == 0 ? "R"
			: rgb_channel == 1 ? "G"
			: "B";
	}
	if (lowered == "img")
		return priml::kDefaultImagePlaneName;
	if (!trimmed.empty())
		return std::string( trimmed );

	if (rgb_channel == 0)
		return "R";
	if (rgb_channel == 1)
		return "G";
	if (rgb_channel == 2)
		return "B";
	return priml::kDefaultImagePlaneName;
}


#ifdef _WIN32

int priml::ipx_triplet_channel ( std::wstring_view path )
{
	return parse_triplet_local<wchar_t>( path, nullptr, nullptr, nullptr );
}

#endif
