/* NovaOS shim for <fenv.h> as musl's math sources use it; the functions
 * are the C runtime's (userland/msvcrt/fenv.c) */
#pragma once
#define FE_INVALID    1
#define FE_DIVBYZERO  4
#define FE_OVERFLOW   8
#define FE_UNDERFLOW  16
#define FE_INEXACT    32
#define FE_ALL_EXCEPT 61
#define FE_TONEAREST  0
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00
int feclearexcept(int);
int feraiseexcept(int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
