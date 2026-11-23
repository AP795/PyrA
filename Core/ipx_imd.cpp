// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "ipx_imd.h"

#include "ipx_names.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <format>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct IpxImdReaderLocal {
	std::ifstream in;

	explicit IpxImdReaderLocal ( const std::filesystem::path &path )
		: in( path, std::ios::binary )
	{
	}

	[[nodiscard]] bool ok () const
	{
		return in.good();
	}

	template <typename T>
	bool read ( T *out )
	{
		static_assert( std::is_trivially_copyable_v<T> );
		if (!out)
			return false;
		in.read( reinterpret_cast<char *>( out ), sizeof( *out ) );
		return in.good();
	}

	bool skip ( std::streamoff count )
	{
		if (count < 0)
			return false;
		in.seekg( count, std::ios::cur );
		return in.good();
	}
};


bool path_has_extension_local ( const std::filesystem::path &path,
	const wchar_t *extension )
{
	if (!extension)
		return false;

	auto ext = path.extension().wstring();
	if (ext.size() != std::wcslen( extension ))
		return false;

	for (auto i = std::size_t{ 0 }; i < ext.size(); ++i) {
		auto left = static_cast<wchar_t>( std::towlower( ext[i] ) );
		auto right = static_cast<wchar_t>( std::towlower( extension[i] ) );
		if (left != right)
			return false;
	}
	return true;
}


// The instruments write both extensions in upper case. On a case-sensitive file
// system only the spelling on disk opens, so the upper-case one is tried when the
// lower-case one is not there; on Windows the first test already finds either.
std::filesystem::path build_imd_path_local (
	const std::filesystem::path &ipx_path )
{
	auto imd = ipx_path;
	imd.replace_extension( L".imd" );
	auto ec = std::error_code{};
	if (std::filesystem::exists( imd, ec ))
		return imd;
	auto upper = ipx_path;
	upper.replace_extension( L".IMD" );
	if (std::filesystem::exists( upper, ec ))
		return upper;
	return imd;
}


int infer_ipx_rgb_channel_local ( const std::filesystem::path &ipx_path )
{
	return priml::ipx_triplet_channel( ipx_path.native() );
}


bool read_utf16_string_local ( IpxImdReaderLocal *reader,
	std::string *out )
{
	if (!reader || !out)
		return false;

	auto length = std::int32_t{ 0 };
	if (!reader->read( &length ))
		return false;
	if (length < 0 || length > 4096)
		return false;
	if (length == 0) {
		out->clear();
		return true;
	}

	auto chars = std::vector<char16_t>(
		static_cast<std::size_t>( length ), u'\0' );
	reader->in.read(
		reinterpret_cast<char *>( chars.data() ),
		static_cast<std::streamsize>( chars.size() * sizeof( char16_t ) ) );
	if (!reader->in.good())
		return false;

	while (!chars.empty() && chars.back() == u'\0')
		chars.pop_back();

// UTF-16 to UTF-8; an unpaired surrogate becomes U+FFFD.
	out->clear();
	out->reserve( chars.size() );
	for (auto i = std::size_t{ 0 }; i < chars.size(); ++i) {
		auto code = static_cast<std::uint32_t>( chars[i] );
		if (code >= 0xd800 && code <= 0xdbff && i + 1 < chars.size() &&
			chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff)
		{
			code = 0x10000 + ( ( code - 0xd800 ) << 10 ) + ( chars[++i] - 0xdc00 );
		} else if (code >= 0xd800 && code <= 0xdfff) {
			code = 0xfffd;
		}
		if (code < 0x80) {
			out->push_back( static_cast<char>( code ) );
		} else if (code < 0x800) {
			out->push_back( static_cast<char>( 0xc0 | ( code >> 6 ) ) );
			out->push_back( static_cast<char>( 0x80 | ( code & 0x3f ) ) );
		} else if (code < 0x10000) {
			out->push_back( static_cast<char>( 0xe0 | ( code >> 12 ) ) );
			out->push_back( static_cast<char>( 0x80 | ( ( code >> 6 ) & 0x3f ) ) );
			out->push_back( static_cast<char>( 0x80 | ( code & 0x3f ) ) );
		} else {
			out->push_back( static_cast<char>( 0xf0 | ( code >> 18 ) ) );
			out->push_back( static_cast<char>( 0x80 | ( ( code >> 12 ) & 0x3f ) ) );
			out->push_back( static_cast<char>( 0x80 | ( ( code >> 6 ) & 0x3f ) ) );
			out->push_back( static_cast<char>( 0x80 | ( code & 0x3f ) ) );
		}
	}
	return true;
}


bool read_ipx_imd_geometry_local ( IpxImdReaderLocal *reader,
	priml::IpxImdMetadata *metadata )
{
	constexpr auto timestamp_bytes = std::streamoff{ 8 };
	auto flags = std::uint32_t{};
	if (!reader->skip( 4 + 4 ) || !reader->read( &flags ) ||
		!reader->read( &metadata->rgb_coefficients ) ||
		!reader->skip( timestamp_bytes ))
	{
		return false;
	}
	metadata->fluorescence = ( flags & 1u ) != 0;

	auto ignored = std::array<double, 9>{};
	if (!reader->read( &metadata->metric_per_pixel ) ||
		!reader->read( &ignored[0] ) ||
		!reader->read( &metadata->origin_x_um ) ||
		!reader->read( &metadata->origin_y_um ) ||
		!reader->read( &ignored[1] ) || !reader->read( &ignored[2] ) ||
		!reader->read( &ignored[3] ) || !reader->read( &ignored[4] ) ||
		!reader->read( &ignored[5] ) || !reader->read( &ignored[6] ) ||
		!reader->read( &ignored[7] ) || !reader->read( &ignored[8] ))
	{
		return false;
	}

	constexpr auto padding_before_gamma = std::streamoff{ 4 };
	constexpr auto tail_padding = std::streamoff{ 6 };
	auto lookup_flags = std::uint32_t{ 0 };
	auto lookup_low = std::int32_t{ 0 };
	auto lookup_high = std::int32_t{ 0 };
	auto lookup_gamma = 0.0;
	auto lookup_background = 0.0;
	auto lookup_max = std::uint16_t{ 0 };
	if (!( reader->read( &lookup_flags ) && reader->read( &lookup_low ) &&
		reader->read( &lookup_high ) && reader->skip( padding_before_gamma ) &&
		reader->read( &lookup_gamma ) && reader->read( &lookup_background ) &&
		reader->read( &lookup_max ) && reader->skip( tail_padding ) ))
	{
		return false;
	}

//
// The display lookup's high point is a display hint, not a reliable sensor
// range: legacy files can record 4095 even when their samples fit in a byte.
// `lookup_max` next to it is the lookup's OUTPUT ceiling - 255, an 8-bit display value - and is not the
// pixel range however much its name suggests otherwise.
//
// Bounded by what a 16-bit sample can hold, so a corrupt record cannot produce a
// lookup table sized off a nonsense number.
	if (lookup_high > 0 && lookup_high <= 0xffff)
		metadata->max_pixel_value = lookup_high;
	return true;
}


bool skip_ipx_imd_channel_data_local ( IpxImdReaderLocal *reader,
	std::int32_t version )
{
	constexpr auto vch_zs_1002_bytes = std::streamoff{ 32 };
	constexpr auto vch_acq_bytes = std::streamoff{ 56 };
	constexpr auto vch_mech_bytes = std::streamoff{ 12 };
	constexpr auto vch_stat_1001_bytes = std::streamoff{ 112 };
	if (version <= 1001)
		return reader->skip( vch_stat_1001_bytes );
	return reader->skip( 4 + 4 ) && reader->skip( vch_zs_1002_bytes ) &&
		reader->skip( vch_acq_bytes ) && reader->skip( vch_mech_bytes );
}


bool read_ipx_imd_identity_local ( IpxImdReaderLocal *reader,
	std::int32_t version,
	priml::IpxImdMetadata *metadata )
{
	auto import_version = std::uint16_t{ 0 };
	if (version > 1000 && !reader->read( &import_version ))
		return false;
	auto seconds = std::int64_t{};
	auto year = std::int16_t{};
	auto time_flags = std::uint16_t{};
	if (!reader->read( &metadata->scene_id ) || !reader->read( &seconds ) ||
		!reader->read( &year ) || !reader->read( &time_flags ) || !reader->skip( 4 ) ||
		!read_utf16_string_local( reader, &metadata->plane_name ))
	{
		return false;
	}
// Ir I_INFO::tmi is packed C_TIME: Unix seconds, historical year, DATE/TIME
// flags. I_PRMS::ttmdf above is a modification timestamp, not acquisition time.
	if (( time_flags & 3u ) == 3u && seconds >= 0 && seconds < 32503680000LL && year == -1) {
		const auto acquired = std::chrono::sys_seconds{ std::chrono::seconds{ seconds } };
		metadata->acquisition_datetime = std::format( "{:%Y%m%d%H%M%S}+0000", acquired );
	}
	auto preparation = std::string{};
	auto comment = std::string{};
	auto author = std::string{};
	return read_utf16_string_local( reader, &preparation ) &&
		read_utf16_string_local( reader, &metadata->slide_name ) &&
		read_utf16_string_local( reader, &metadata->zone_name ) &&
		read_utf16_string_local( reader, &comment ) &&
		read_utf16_string_local( reader, &author );
}

} // namespace


bool priml::load_ipx_imd_metadata ( const std::filesystem::path &ipx_path,
	IpxImdMetadata *out )
{
	if (!path_has_extension_local( ipx_path, L".ipx" ))
		return false;
	return load_imd_metadata_beside( ipx_path, out );
}


bool priml::load_imd_metadata_beside ( const std::filesystem::path &raster_path,
	IpxImdMetadata *out )
{
	if (!out)
		return false;

	auto imd_path = build_imd_path_local( raster_path );
	if (!std::filesystem::exists( imd_path ))
		return false;

	auto reader = IpxImdReaderLocal( imd_path );
	if (!reader.ok())
		return false;

	auto version = std::int32_t{ 0 };
	auto width = std::int32_t{ 0 };
	auto height = std::int32_t{ 0 };
	if (!reader.read( &version ) ||
		!reader.read( &width ) ||
		!reader.read( &height ))
	{
		return false;
	}
	if (version < 1000 || version > 1002)
		return false;

	auto metadata = IpxImdMetadata{};
	metadata.image_width = width;
	metadata.image_height = height;
	if (!read_ipx_imd_geometry_local( &reader, &metadata ) ||
		!skip_ipx_imd_channel_data_local( &reader, version ) ||
		!read_ipx_imd_identity_local( &reader, version, &metadata ))
	{
		return false;
	}

	if (!( metadata.metric_per_pixel > 0.0 ) ||
		!std::isfinite( metadata.metric_per_pixel ) ||
		!std::isfinite( metadata.origin_x_um ) ||
		!std::isfinite( metadata.origin_y_um ))
	{
		return false;
	}

	metadata.loaded = true;
	metadata.rgb_channel = infer_ipx_rgb_channel_local( raster_path );
	*out = std::move( metadata );
	return true;
}


bool priml::load_ipx_imd_metadata ( const std::string &ipx_path,
	IpxImdMetadata *out )
{
	return load_ipx_imd_metadata( std::filesystem::path( ipx_path ), out );
}


#ifdef _WIN32
bool priml::load_ipx_imd_metadata ( const std::wstring &ipx_path,
	IpxImdMetadata *out )
{
	return load_ipx_imd_metadata( std::filesystem::path( ipx_path ), out );
}
#endif
