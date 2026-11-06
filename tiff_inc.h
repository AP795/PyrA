#pragma once

// Wrapper for libtiff headers.

#if defined(_MSC_VER) && __has_include(<tif_config.h>)
#pragma warning(push, 0)
#endif

extern "C" {
#if defined(_MSC_VER) && __has_include(<tif_config.h>)
//
// Some Windows vendor builds ship an unconfigured tiffconf.h and need this
// internal header first. Installed libtiff packages need only tiffio.h.
#include <tif_config.h>
#endif
#include <tiffio.h>
}

#if defined(_MSC_VER) && __has_include(<tif_config.h>)
#pragma warning(pop)
#endif
