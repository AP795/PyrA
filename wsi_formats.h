#pragma once

// Which pyramid file formats this build can open - small on purpose, so that a
// header that only classifies paths (priml/slide_kind.h) can ask without pulling
// in the reader.

#include "build_config.h"

#include <array>
#include <string>
#include <string_view>

namespace priml {

enum class WsiFormat { Unknown, Tiff, Ndpi, Svs, Czi, Dicom, Gallery };

struct WsiFormatEntry {
	std::string_view extension;
	WsiFormat format;
};

// Compile-time support. CZI additionally checks DLL availability at construction.
inline constexpr auto kWsiFormats = std::to_array<WsiFormatEntry>( {
	{ ".bif", WsiFormat::Tiff },
	{ ".glr", WsiFormat::Gallery },
	{ ".ndpi", WsiFormat::Ndpi },
	{ ".ptf", WsiFormat::Tiff },
	{ ".svs", WsiFormat::Svs },
#if USE_CZI
	{ ".czi", WsiFormat::Czi },
#endif
#if USE_DCMTK
	{ ".dcm", WsiFormat::Dicom },
#endif
} );

inline constexpr auto kWsiLoaderExtensions = [] {
	auto extensions = std::array<std::string_view, kWsiFormats.size()>{};
	for (auto i = std::size_t{ 0 }; i < kWsiFormats.size(); ++i)
		extensions[i] = kWsiFormats[i].extension;
	return extensions;
}();


constexpr WsiFormat wsi_format_for_extension ( std::string_view extension )
{
	for (const auto &entry : kWsiFormats) {
		if (entry.extension == extension)
			return entry.format;
	}
	return WsiFormat::Unknown;
}


// ASCII lower case, locale-independent: ::tolower depends on the C locale, and an
// extension or a property key must compare the same whatever the host set.
inline std::string lower_ascii_copy ( std::string_view text )
{
	auto out = std::string( text );
	for (auto &c : out) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>( c - 'A' + 'a' );
	}
	return out;
}

} // namespace priml
