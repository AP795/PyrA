// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "pyra/pyra.h"

#include <string>

namespace pyra::detail {

struct ApiFailure {
	pyra_status status;
	std::string message;
};

inline void require ( bool condition, const char *message,
	pyra_status status = PYRA_INVALID_ARGUMENT )
{
	if (!condition)
		throw ApiFailure{ status, message };
}

} // namespace pyra::detail
