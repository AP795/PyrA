// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "dcm_color.h"
#include "dcm_identity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace priml {
namespace {

void put32 ( std::vector<uint8_t> &out, size_t offset, uint32_t value )
{
	for (auto i = 0; i < 4; ++i)
		out[offset + i] = (uint8_t)( value >> ( 24 - 8 * i ) );
}


void signature ( std::vector<uint8_t> &out, size_t offset, const char *text )
{
	std::memcpy( out.data() + offset, text, 4 );
}


std::vector<uint8_t> xyz ( double x, double y, double z )
{
	auto out = std::vector<uint8_t>( 20 );
	signature( out, 0, "XYZ " );
	const auto values = std::array{ x, y, z };
	for (auto i = size_t{}; i < values.size(); ++i)
		put32( out, 8 + 4 * i, (uint32_t)(int32_t)std::lround( values[i] * 65536.0 ) );
	return out;
}


std::vector<uint8_t> description ()
{
	const auto text = dcm_writer_identity().product +
		" native-range RGB presentation; assumed sRGB; not scanner calibration";
	const auto count = text.size() + 1;
	auto out = std::vector<uint8_t>( 12 + count + 8 + 70 );
	signature( out, 0, "desc" );
	put32( out, 8, (uint32_t)count );
	std::memcpy( out.data() + 12, text.c_str(), count );
	return out;
}


// The identity's ICC signature as exactly four characters.
std::string icc_signature_local ()
{
	auto text = dcm_writer_identity().icc_signature.substr( 0, 4 );
	text.resize( 4, ' ' );
	return text;
}

} // namespace


bool make_dcm_rgb_profile ( int container_max, int effective_max,
	std::vector<uint8_t> *out, std::string *error )
{
	if (!out || ( container_max != 255 && container_max != 65535 ) ||
		effective_max <= 0 || effective_max > container_max)
	{
		if (error)
			*error = "invalid RGB presentation profile range";
		return false;
	}
// ICC v2 input-device matrix profile. The D50-adapted sRGB matrix is the
// standard sRGB matrix used by LittleCMS's built-in sRGB profile. One curve
// sampled curve retains the native-range mapping with sub-display-code error.
// Keep the table within common CMM limits (LittleCMS rejects >65530 nodes).
	constexpr auto entries = 16385u;
	auto curve = std::vector<uint8_t>( 12 + 2 * entries );
	signature( curve, 0, "curv" );
	put32( curve, 8, entries );
	for (auto i = 0u; i < entries; ++i) {
		const auto encoded = std::min( 1.0,
			(double)i * container_max / ( ( entries - 1.0 ) * effective_max ) );
		const auto linear = encoded <= 0.04045 ? encoded / 12.92
			: std::pow( ( encoded + 0.055 ) / 1.055, 2.4 );
		const auto value = (uint16_t)std::lround( linear * 65535.0 );
		curve[12 + 2 * i] = (uint8_t)( value >> 8 );
		curve[13 + 2 * i] = (uint8_t)value;
	}
	struct Tag { const char *name; std::vector<uint8_t> bytes; };
	auto copyright = std::vector<uint8_t>( 8 );
	signature( copyright, 0, "text" );
	const auto notice = dcm_writer_identity().product +
		" presentation assumption; no scanner calibration asserted.";
	copyright.insert( copyright.end(), notice.c_str(), notice.c_str() + notice.size() + 1 );
	auto tags = std::array{
		Tag{ "desc", description() }, Tag{ "cprt", std::move( copyright ) },
		Tag{ "wtpt", xyz( 0.9642, 1.0, 0.8249 ) },
		Tag{ "rXYZ", xyz( 0.4360747, 0.2225045, 0.0139322 ) },
		Tag{ "gXYZ", xyz( 0.3850649, 0.7168786, 0.0971045 ) },
		Tag{ "bXYZ", xyz( 0.1430804, 0.0606169, 0.7141733 ) },
		Tag{ "rTRC", std::move( curve ) }, Tag{ "gTRC", {} }, Tag{ "bTRC", {} } };
	out->assign( 132 + 12 * tags.size(), 0 );
	const auto creator = icc_signature_local();
	signature( *out, 4, creator.c_str() );
	put32( *out, 8, 0x02100000 );
	signature( *out, 12, "scnr" );
	signature( *out, 16, "RGB " );
	signature( *out, 20, "XYZ " );
// Reproducible profile-definition date, unrelated to acquisition timestamps.
	(*out)[24] = 0x07;
	(*out)[25] = 0xea;
	(*out)[27] = 1;
	(*out)[29] = 1;
	signature( *out, 36, "acsp" );
	signature( *out, 48, creator.c_str() );
	put32( *out, 68, (uint32_t)std::lround( 0.9642 * 65536.0 ) );
	put32( *out, 72, 65536 );
	put32( *out, 76, (uint32_t)std::lround( 0.8249 * 65536.0 ) );
	signature( *out, 80, creator.c_str() );
	put32( *out, 128, (uint32_t)tags.size() );
	auto curve_offset = uint32_t{};
	auto curve_length = uint32_t{};
	for (auto i = size_t{}; i < tags.size(); ++i) {
		const auto &tag = tags[i];
		signature( *out, 132 + 12 * i, tag.name );
		auto offset = (uint32_t)out->size();
		auto length = (uint32_t)tag.bytes.size();
		if (i >= tags.size() - 2) {
			offset = curve_offset;
			length = curve_length;
		} else {
			out->insert( out->end(), tag.bytes.begin(), tag.bytes.end() );
			while (out->size() % 4)
				out->push_back( 0 );
		}
		if (i == tags.size() - 3) {
			curve_offset = offset;
			curve_length = length;
		}
		put32( *out, 136 + 12 * i, offset );
		put32( *out, 140 + 12 * i, length );
	}
	put32( *out, 0, (uint32_t)out->size() );
	return true;
}

} // namespace priml
