/*
 * NovaOS: included first in every cnc-ddraw source (-include).  cnc-ddraw
 * is written against the Windows SDK's (or MinGW-w64's) headers; NovaOS
 * compiles it with clang in MSVC mode over MinGW-w64's headers, as it does
 * Microsoft's STL (userland/msvcp140/build.py).
 *
 * Clang's <intrin.h> already defines these string intrinsics inline; tell
 * MinGW-w64's <winnt.h> not to define them again.
 */
#define __INTRINSIC_DEFINED___stosb
#define __INTRINSIC_DEFINED___stosw
#define __INTRINSIC_DEFINED___stosd
#define __INTRINSIC_DEFINED___stosq
#define __INTRINSIC_DEFINED___movsb
#define __INTRINSIC_DEFINED___movsw
#define __INTRINSIC_DEFINED___movsd
#define __INTRINSIC_DEFINED___movsq
#include <intrin.h>   /* before any Windows header defines its macros */
#include <_mingw.h>
#ifndef __MINGW_IMPORT   /* (<_mingw.h> leaves it undefined for clang in MSVC mode) */
#define __MINGW_IMPORT __declspec(dllimport)
#endif
