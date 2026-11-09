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
