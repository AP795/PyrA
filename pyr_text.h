// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// Locale-independent ASCII text helpers shared by the loaders and scanners.

#include "wsi_formats.h"

#include <cctype>
#include <string>
#include <string_view>

namespace priml {

// `text` without leading and trailing white space (the C locale's isspace).
inline std::string trim_ascii_copy ( std::string_view text )
{
	auto begin = std::size_t{ 0 };
	while (begin < text.size() && std::isspace( static_cast<unsigned char>( text[begin] ) ))
		++begin;
	auto end = text.size();
	while (end > begin && std::isspace( static_cast<unsigned char>( text[end - 1] ) ))
		--end;
	return std::string( text.substr( begin, end - begin ) );
}


[[nodiscard]] inline bool equals_ignore_ascii_case ( std::string_view lhs,
	std::string_view rhs ) noexcept
{
	if (lhs.size() != rhs.size())
		return false;
	for (auto i = std::size_t{ 0 }; i < lhs.size(); ++i) {
		auto a = lhs[i] >= 'A' && lhs[i] <= 'Z' ? char( lhs[i] - 'A' + 'a' ) : lhs[i];
		auto b = rhs[i] >= 'A' && rhs[i] <= 'Z' ? char( rhs[i] - 'A' + 'a' ) : rhs[i];
		if (a != b)
			return false;
	}
	return true;
}

} // namespace priml
