#pragma once

// Windows header shim for the pyramid library.
//
// A copy of tcu/win.h rather than a reference to it: this library is meant to
// build standalone, and the one thing it needs from a Windows host is windows.h
// pulled in with the right guards. If a .cpp needs more than LEAN_AND_MEAN
// provides, include <windows.h> directly in that file.

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif

 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif

 #include <windows.h>
#endif
