/*
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
*/

/*
Platform switches the portable jxrlib sources (segdec.c, strcodec.c) read. Decided
here by the preprocessor rather than by a configure step, so every build of the
sources - CMake, the WSIA MSBuild project, Emscripten - agrees.
*/

#pragma once

#if defined( __BYTE_ORDER__ ) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define JXRDECODE_ISBIGENDIANHOST 1
#else
#define JXRDECODE_ISBIGENDIANHOST 0
#endif

/* x86 loads unaligned integers; elsewhere (WebAssembly, ARM) go through memcpy. */
#if defined( _M_X64 ) || defined( _M_IX86 ) || defined( __x86_64__ ) || defined( __i386__ )
#define JXRDECODE_SIGBUS_ON_UNALIGNEDINTEGERS 0
#else
#define JXRDECODE_SIGBUS_ON_UNALIGNEDINTEGERS 1
#endif

#if defined( _MSC_VER )
#define JXRDECODE_HAS_BUILTIN_BSWAP32 0
#define JXRDECODE_HAS_BYTESWAP_IN_STDLIB 1
#define JXRDECODE_HAS_ROTL_WITH_INTRIN_H 1
#define JXRDECODE_HAS_ROTL_WITH_X86INTRIN_H 0
#else
#if defined( __has_include )
#if __has_include( <byteswap.h> )
#define JXRDECODE_HAS_BUILTIN_BSWAP32 1
#endif
#endif
#ifndef JXRDECODE_HAS_BUILTIN_BSWAP32
#define JXRDECODE_HAS_BUILTIN_BSWAP32 0
#endif
#define JXRDECODE_HAS_BYTESWAP_IN_STDLIB 0
#define JXRDECODE_HAS_ROTL_WITH_INTRIN_H 0
#if defined( __x86_64__ ) || defined( __i386__ )
#define JXRDECODE_HAS_ROTL_WITH_X86INTRIN_H 1
#else
#define JXRDECODE_HAS_ROTL_WITH_X86INTRIN_H 0
#endif
#endif

#define JXRDECODE_HAS_BSWAP_LONG_IN_SYS_ENDIAN 0
