/*
 * NovaOS: MinGW-w64's <math.h>, made to look like the UCRT's to C++: the
 * classification macros become the functions the STL's <cmath> expects,
 * with the UCRT's FP_* values.
 */
#pragma once
#include_next <math.h>

#ifdef __cplusplus
#undef fpclassify
#undef signbit
#undef isfinite
#undef isinf
#undef isnan
#undef isnormal
#undef isgreater
#undef isgreaterequal
#undef isless
#undef islessequal
#undef islessgreater
#undef isunordered
#undef FP_INFINITE
#undef FP_NAN
#undef FP_NORMAL
#undef FP_SUBNORMAL
#undef FP_ZERO
#define FP_INFINITE  1
#define FP_NAN       2
#define FP_NORMAL    (-1)
#define FP_SUBNORMAL (-2)
#define FP_ZERO      0

extern "C++" {
#define _NOVA_FPCLASSIFY(T)                                                                        \
    inline int fpclassify(T _X) noexcept {                                                       \
        return __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, _X); \
    }                                                                                            \
    inline bool signbit(T _X) noexcept { return __builtin_signbit(_X) != 0; }
_NOVA_FPCLASSIFY(float)
_NOVA_FPCLASSIFY(double)
_NOVA_FPCLASSIFY(long double)
#undef _NOVA_FPCLASSIFY
}
#endif

/* the UCRT's IEEE layout constants and classification helpers (its
 * <math.h> has them; the STL's own math code uses them) */
#define _DENORM  (-2)
#define _FINITE  (-1)
#define _INFCODE 1
#define _NANCODE 2
#define _FE_DIVBYZERO 0x04
#define _DBIAS 0x3fe
#define _DOFF  4
#define _FBIAS 0x7e
#define _FOFF  7
#define _FRND  1
#define _DFRAC ((unsigned short)((1 << _DOFF) - 1))
#define _DMASK ((unsigned short)(0x7fff & ~_DFRAC))
#define _DMAX  ((unsigned short)((1 << (15 - _DOFF)) - 1))
#define _DSIGN ((unsigned short)0x8000)
#define _FFRAC ((unsigned short)((1 << _FOFF) - 1))
#define _FMASK ((unsigned short)(0x7fff & ~_FFRAC))
#define _FMAX  ((unsigned short)((1 << (15 - _FOFF)) - 1))
#define _FSIGN ((unsigned short)0x8000)
#define _LBIAS 0x3fe
#define _LOFF  4
#define _LFRAC ((unsigned short)(-1))
#define _LMASK ((unsigned short)0x7fff)
#define _LMAX  ((unsigned short)0x7fff)
#define _LSIGN ((unsigned short)0x8000)
#ifdef __cplusplus
extern "C" {
#endif
short __cdecl _dtest(double *);
short __cdecl _fdtest(float *);
short __cdecl _ldtest(long double *);
short __cdecl _dclass(double);
short __cdecl _fdclass(float);
short __cdecl _ldclass(long double);
int __cdecl _dsign(double);
int __cdecl _fdsign(float);
int __cdecl _ldsign(long double);
short __cdecl _dexp(double *, double, long);
short __cdecl _fdexp(float *, float, long);
short __cdecl _dnorm(unsigned short *);
short __cdecl _fdnorm(unsigned short *);
short __cdecl _dscale(double *, long);
short __cdecl _fdscale(float *, long);
short __cdecl _dunscale(short *, double *);
short __cdecl _fdunscale(short *, float *);
double __cdecl _dpoly(double, double const *, int);
#ifdef __cplusplus
}
#endif
