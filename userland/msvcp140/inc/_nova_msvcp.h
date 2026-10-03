/*
 * NovaOS: included first in every msvcp140 source (-include): settings
 * and declarations the UCRT's headers would give the STL's sources, over
 * MinGW-w64's.
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

/* Windows 10's API set for the headers; the DLL itself keeps the
 * Windows XP-era exports (and looks newer functions up at run time), as
 * Microsoft builds it for ABI compatibility */
#define _STL_WIN32_WINNT 0x0501
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

/* (MinGW-w64's <minwinbase.h> lists these only for _WIN32_WINNT >= 0x0A000002) */
#define FileDispositionInfoEx ((FILE_INFO_BY_HANDLE_CLASS)21)
#define FileRenameInfoEx ((FILE_INFO_BY_HANDLE_CLASS)22)

/* the UCRT exports the STL calls that MinGW-w64's headers do not declare */
#include <stdio.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
wchar_t **__cdecl ___lc_locale_name_func(void);
unsigned int __cdecl ___lc_collate_cp_func(void);
void __cdecl _lock_locales(void);
void __cdecl _unlock_locales(void);
errno_t __cdecl rand_s(unsigned int *);
__declspec(dllimport) unsigned long __stdcall GetTempPath2W(unsigned long, wchar_t *);
size_t __cdecl __strncnt(const char *, size_t);
size_t __cdecl __wcsncnt(const wchar_t *, size_t);
errno_t __cdecl _get_stream_buffer_pointers(FILE *, char ***, char ***, int **);
__declspec(noreturn) void __cdecl _invoke_watson(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t);
#ifdef __cplusplus
}
#endif
