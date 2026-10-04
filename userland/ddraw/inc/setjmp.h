/*
 * NovaOS: cnc-ddraw does not use setjmp, but MinGW-w64's headers pull
 * <setjmp.h> in, and its x86 declaration of _setjmp (two arguments)
 * disagrees with clang's <intrin.h> (MSVC's, one).  This gives the MSVC
 * one alone.
 */
#pragma once
#ifndef _WIN64
typedef int jmp_buf[16];
#else
typedef __declspec(align(16)) struct { unsigned __int64 Part[2]; } _JBTYPE;
typedef _JBTYPE jmp_buf[16];
#endif
int __cdecl _setjmp(jmp_buf);
#define setjmp _setjmp
__declspec(noreturn) void __cdecl longjmp(jmp_buf, int);
