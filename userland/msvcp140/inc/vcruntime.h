/*
 * NovaOS: the part of Microsoft's <vcruntime.h> the C++ standard library
 * (third_party/msstl) is written against, over MinGW-w64's C headers.
 * Written for NovaOS from the documented interface.
 */
#pragma once
#define _VCRUNTIME_H

#include <_mingw.h>
#include <sal.h>
#include <vadefs.h>
#include <crtdefs.h>

#ifdef __cplusplus
#define _CRT_BEGIN_C_HEADER __pragma(pack(push, 8)) extern "C" {
#define _CRT_END_C_HEADER } __pragma(pack(pop))
#else
#define _CRT_BEGIN_C_HEADER __pragma(pack(push, 8))
#define _CRT_END_C_HEADER __pragma(pack(pop))
#endif

#ifndef _VCRTIMP
#define _VCRTIMP
#endif
#ifndef _VCRT_NOALIAS
#define _VCRT_NOALIAS __declspec(noalias)
#endif
#ifndef _VCRT_RESTRICT
#define _VCRT_RESTRICT __declspec(restrict)
#endif
#ifndef _VCRT_ALLOCATOR
#define _VCRT_ALLOCATOR __declspec(allocator)
#endif
#define _VCRT_ALIGN(x) __declspec(align(x))

/* (MinGW-w64's <_mingw.h> has its own _CRTIMP2) */
#undef _CRTIMP2
#if defined(CRTDLL2) && defined(_CRTBLD)
#define _CRTIMP2 __declspec(dllexport)
#else
#define _CRTIMP2
#endif
#undef _MRTIMP2
#define _MRTIMP2 _CRTIMP2

/* the STL's satellite DLLs (msvcp140_1, _2, _atomic_wait, _codecvt_ids):
 * exported while one is built, imported (plainly) otherwise */
#ifndef _CRT_SATELLITE_1
#ifdef _BUILDING_SATELLITE_1
#define _CRT_SATELLITE_1 __declspec(dllexport)
#else
#define _CRT_SATELLITE_1
#endif
#endif
#ifndef _CRT_SATELLITE_2
#ifdef _BUILDING_SATELLITE_2
#define _CRT_SATELLITE_2 __declspec(dllexport)
#else
#define _CRT_SATELLITE_2
#endif
#endif
#ifndef _CRT_SATELLITE_CODECVT_IDS
#ifdef _BUILDING_SATELLITE_CODECVT_IDS
#define _CRT_SATELLITE_CODECVT_IDS __declspec(dllexport)
#else
#define _CRT_SATELLITE_CODECVT_IDS
#endif
#endif

#ifndef _CRT_SATELLITE_CODECVT_IDS_NOIMPORT
#ifdef _BUILDING_SATELLITE_CODECVT_IDS
#define _CRT_SATELLITE_CODECVT_IDS_NOIMPORT __declspec(dllexport)
#else
#define _CRT_SATELLITE_CODECVT_IDS_NOIMPORT
#endif
#endif

#ifndef __CLR_OR_THIS_CALL
#define __CLR_OR_THIS_CALL
#endif
#ifndef __CLRCALL_OR_CDECL
#define __CLRCALL_OR_CDECL __cdecl
#endif
#ifndef __CLRCALL_PURE_OR_CDECL
#define __CLRCALL_PURE_OR_CDECL __cdecl
#endif
#ifndef __CRTDECL
#define __CRTDECL __cdecl
#endif

#ifndef _HAS_EXCEPTIONS
#define _HAS_EXCEPTIONS 1
#endif

#define _CRT_STRINGIZE_(x) #x
#define _CRT_STRINGIZE(x) _CRT_STRINGIZE_(x)
#define _CRT_WIDE_(s) L##s
#define _CRT_WIDE(s) _CRT_WIDE_(s)
#define _CRT_CONCATENATE_(a, b) a##b
#define _CRT_CONCATENATE(a, b) _CRT_CONCATENATE_(a, b)
#define _CRT_UNPARENTHESIZE_(...) __VA_ARGS__
#define _CRT_UNPARENTHESIZE(...) _CRT_UNPARENTHESIZE_ __VA_ARGS__

#ifndef _CRT_DEPRECATE_TEXT
#define _CRT_DEPRECATE_TEXT(_Text) __declspec(deprecated(_Text))
#endif
#ifndef _CRT_INSECURE_DEPRECATE
#define _CRT_INSECURE_DEPRECATE(_Replacement)
#endif
#ifndef _CRT_INSECURE_DEPRECATE_MEMORY
#define _CRT_INSECURE_DEPRECATE_MEMORY(_Replacement)
#endif
#define _CRT_GUARDOVERFLOW
#define _CRT_HYBRIDPATCHABLE
#define _CRT_SECURE_CPP_NOTHROW throw()
#ifdef __cplusplus
#define _CRT_HAS_CXX17 1
#else
#define _CRT_HAS_CXX17 0
#endif

#ifndef _CRT_STDIO_INLINE
#define _CRT_STDIO_INLINE __inline
#endif

#ifdef __cplusplus
typedef bool __vcrt_bool;
#else
typedef _Bool __vcrt_bool;
#endif

#ifndef _HAS_NODISCARD
#if defined(__has_cpp_attribute) && __has_cpp_attribute(nodiscard) >= 201603L
#define _HAS_NODISCARD 1
#else
#define _HAS_NODISCARD 0
#endif
#endif
#if _HAS_NODISCARD
#define _NODISCARD [[nodiscard]]
#else
#define _NODISCARD
#endif
#define _VCRT_NODISCARD _NODISCARD
#ifndef _MSVC_CONSTEXPR
#define _MSVC_CONSTEXPR
#endif


/* the C++ dialect switches <vcruntime.h> sets for the STL */
#ifdef _MSVC_LANG
#define _STL_LANG _MSVC_LANG
#elif defined(__cplusplus)
#define _STL_LANG __cplusplus
#else
#define _STL_LANG 0L
#endif
#ifndef _HAS_CXX17
#define _HAS_CXX17 (_STL_LANG > 201402L)
#endif
#ifndef _HAS_CXX20
#define _HAS_CXX20 (_HAS_CXX17 && _STL_LANG > 201703L)
#endif
#ifndef _HAS_CXX23
#define _HAS_CXX23 (_HAS_CXX20 && _STL_LANG > 202002L)
#endif

/* <corecrt.h>'s names MinGW-w64's headers lack */
#ifndef _ACRTIMP
#define _ACRTIMP
#endif
#ifndef _DCRTIMP
#define _DCRTIMP
#endif
#define _CRT_SECURE_INVALID_PARAMETER(expr) ::_invalid_parameter_noinfo_noreturn()

#ifdef __cplusplus
extern "C" {
#endif
__declspec(noreturn) void __cdecl _invalid_parameter_noinfo_noreturn(void);
void __cdecl _invalid_parameter_noinfo(void);
void __cdecl __security_init_cookie(void);
#ifdef _M_IX86
void __fastcall __security_check_cookie(uintptr_t _StackCookie);
#else
void __cdecl __security_check_cookie(uintptr_t _StackCookie);
#endif
extern uintptr_t __security_cookie;
#ifdef __cplusplus
}
#endif

/* (<corecrt.h> declares _Mbstatet, which the STL takes from here) */
#include <wchar.h>
