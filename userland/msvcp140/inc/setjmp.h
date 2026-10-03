/*
 * NovaOS: MinGW-w64's <setjmp.h> with the UCRT's spelling of setjmp
 * (an intrinsic of one argument), so it agrees with clang's <intrin.h>.
 */
#pragma once
#include_next <setjmp.h>
#undef _setjmp
#undef setjmp
#ifdef __cplusplus
extern "C"
#endif
int __cdecl _setjmp(jmp_buf);
#define setjmp _setjmp
