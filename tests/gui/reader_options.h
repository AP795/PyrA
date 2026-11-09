#pragma once

#include <pyra/pyra.h>

namespace pyra::test {

inline constexpr auto kBytesPerMiB = uint64_t{ 1024 } * 1024;

inline pyra_context_options reader_options ( pyra_backend backend = PYRA_BACKEND_NATIVE )
{
	auto options = pyra_context_options{};
	pyra_context_options_init( &options );
	options.backend = backend;
	return options;
}

} // namespace pyra::test
