/* msvcrt: Microsoft's names for math functions (the C99 ones are musl's)
 * and the floating-point environment (SSE MXCSR + x87 control word) */
#define NOVA_BUILD_MSVCRT
#include <math.h>
#include <float.h>
#include <stdint.h>
#include "ptd.h"

__declspec(dllexport) int _fltused = 0x9875;   /* floating point in use (the compiler references it) */

double _hypot(double x, double y)     { return hypot(x, y); }
__declspec(dllexport) float _hypotf(float x, float y)      { return hypotf(x, y); }
double _copysign(double x, double y)  { return copysign(x, y); }
float  _copysignf(float x, float y)   { return copysignf(x, y); }
double _chgsign(double x)             { return -x; }
double _logb(double x)                { return logb(x); }
float  _logbf(float x)                { return logbf(x); }
double _nextafter(double x, double y) { return nextafter(x, y); }
float  _nextafterf(float x, float y)  { return nextafterf(x, y); }
double _scalb(double x, long e)       { return scalbln(x, e); }
float  _scalbf(float x, long e)       { return scalbnf(x, (int)e); }
int    _finite(double x)              { return isfinite(x); }
int    _finitef(float x)              { return isfinite(x); }
int    _isnan(double x)               { return isnan(x); }
int    _isnanf(float x)               { return isnan(x); }
double _j0(double x) { return j0(x); }
double _j1(double x) { return j1(x); }
double _jn(int n, double x) { return jn(n, x); }
double _y0(double x) { return y0(x); }
double _y1(double x) { return y1(x); }
double _yn(int n, double x) { return yn(n, x); }

/* _fpclass bits */
int _fpclass(double x)
{
    int neg = signbit(x) != 0;
    switch (fpclassify(x)) {
    case FP_NAN:       return 0x0002;                        /* _FPCLASS_QNAN */
    case FP_INFINITE:  return neg ? 0x0004 : 0x0200;
    case FP_ZERO:      return neg ? 0x0020 : 0x0040;
    case FP_SUBNORMAL: return neg ? 0x0010 : 0x0080;
    default:           return neg ? 0x0008 : 0x0100;
    }
}

/* ucrt's classification helpers used by <math.h> inlines */
typedef union { double d; uint64_t u; } DU;
typedef union { float f; uint32_t u; } FU;
/* Microsoft's FP_ values: INFINITE 1, NAN 2, NORMAL -1, SUBNORMAL -2, ZERO 0 */
static short ms_class(int c)
{
    switch (c) {
    case FP_INFINITE:  return 1;
    case FP_NAN:       return 2;
    case FP_NORMAL:    return -1;
    case FP_SUBNORMAL: return -2;
    default:           return 0;
    }
}
__declspec(dllexport) short _dclass(double x)   { return ms_class(fpclassify(x)); }
__declspec(dllexport) short _fdclass(float x)   { return ms_class(fpclassify(x)); }
__declspec(dllexport) short _ldclass(long double x) { return ms_class(fpclassify((double)x)); }
__declspec(dllexport) int _dsign(double x)    { return signbit(x) != 0; }
__declspec(dllexport) int _fdsign(float x)    { return signbit(x) != 0; }
__declspec(dllexport) int _ldsign(long double x) { return signbit((double)x) != 0; }
__declspec(dllexport) int _dtest(double *x)   { return ms_class(fpclassify(*x)); }
__declspec(dllexport) int _fdtest(float *x)   { return ms_class(fpclassify(*x)); }
__declspec(dllexport) int _dpcomp(double x, double y) { return isunordered(x, y) ? 0 : x < y ? 1 : x == y ? 2 : 4; }
__declspec(dllexport) int _fdpcomp(float x, float y)  { return isunordered(x, y) ? 0 : x < y ? 1 : x == y ? 2 : 4; }

/* -----------------------------------------------------------------------
 * <fenv.h> (exported: NumPy calls them): exceptions and rounding live in MXCSR (SSE) and the x87
 * status/control words; kept in step so either unit behaves the same
 * ----------------------------------------------------------------------- */
#define FE_ALL 0x3D
static unsigned mxcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }

__declspec(dllexport) int feclearexcept(int e)
{
    e &= FE_ALL;
    set_mxcsr(mxcsr() & ~(unsigned)e);
    if (e) __asm__ volatile("fnclex");
    return 0;
}
__declspec(dllexport) int fetestexcept(int e)
{
    unsigned short sw;
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    return (int)((mxcsr() | sw) & (unsigned)e & FE_ALL);
}
__declspec(dllexport) int feraiseexcept(int e) { set_mxcsr(mxcsr() | ((unsigned)e & FE_ALL)); return 0; }
__declspec(dllexport) int fegetround(void) { return (int)((mxcsr() >> 3) & 0xC00); }
__declspec(dllexport) int fesetround(int r)
{
    if (r & ~0xC00) return -1;
    set_mxcsr((mxcsr() & ~0x6000u) | ((unsigned)r << 3));
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0xC00) | r);
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return 0;
}
typedef struct { unsigned long control, status; } fenv_t;
typedef unsigned long fexcept_t;
__declspec(dllexport) int fegetenv(fenv_t *e) { e->control = mxcsr() & ~0x3Fu; e->status = mxcsr() & 0x3F; return 0; }
__declspec(dllexport) int fesetenv(const fenv_t *e)
{
    if (e == (const fenv_t *)0) { set_mxcsr(0x1F80); return 0; }        /* FE_DFL_ENV */
    set_mxcsr((unsigned)(e->control | e->status));
    return 0;
}
__declspec(dllexport) int feholdexcept(fenv_t *e) { fegetenv(e); set_mxcsr((mxcsr() | 0x1F80) & ~0x3Fu); return 0; }
__declspec(dllexport) int feupdateenv(const fenv_t *e) { unsigned ex = mxcsr() & 0x3F; fesetenv(e); feraiseexcept((int)ex); return 0; }
__declspec(dllexport) int fegetexceptflag(fexcept_t *f, int e) { *f = (fexcept_t)fetestexcept(e); return 0; }
__declspec(dllexport) int fesetexceptflag(const fexcept_t *f, int e) { set_mxcsr((mxcsr() & ~((unsigned)e & FE_ALL)) | ((unsigned)*f & (unsigned)e & FE_ALL)); return 0; }

/* The exported <fenv.h>: Microsoft numbers the rounding modes as its
 * _RC_* bits (near 0, down 0x100, up 0x200, chop 0x300); the exception
 * bits match MXCSR's.  The functions above keep musl's x87 numbering
 * for the math library. */
static int ms_round(int r) { return r == 0x400 ? 0x100 : r == 0x800 ? 0x200 : r == 0xC00 ? 0x300 : 0; }
static int x87_round(int r) { return r == 0x100 ? 0x400 : r == 0x200 ? 0x800 : r == 0x300 ? 0xC00 : 0; }
int nova_ms_fegetround(void) { return ms_round(fegetround()); }
int nova_ms_fesetround(int r) { return r & ~0x300 ? 1 : fesetround(x87_round(r)) ? 1 : 0; }
#ifdef _WIN64
#define FE_SYM(n) #n
#else
#define FE_SYM(n) "_" #n                    /* x86: C names carry an underscore */
#endif
#define FE_EXPORT(name, sym) ".ascii \" /EXPORT:" #name "=" FE_SYM(sym) "\"\n\t"
__asm__(".section .drectve,\"yn\"\n\t"
        FE_EXPORT(fegetround, nova_ms_fegetround) FE_EXPORT(fesetround, nova_ms_fesetround)
        FE_EXPORT(fegetenv, fegetenv) FE_EXPORT(fesetenv, fesetenv) FE_EXPORT(feholdexcept, feholdexcept)
        FE_EXPORT(feupdateenv, feupdateenv) FE_EXPORT(feclearexcept, feclearexcept)
        FE_EXPORT(fetestexcept, fetestexcept) FE_EXPORT(feraiseexcept, feraiseexcept)
        FE_EXPORT(fegetexceptflag, fegetexceptflag) FE_EXPORT(fesetexceptflag, fesetexceptflag)
        ".text\n");

/* Microsoft's float control API over MXCSR */
__declspec(dllexport) unsigned int _clearfp(void)  { unsigned s = mxcsr() & 0x3F; set_mxcsr(mxcsr() & ~0x3Fu); __asm__ volatile("fnclex"); return s; }
__declspec(dllexport) unsigned int _statusfp(void) { return mxcsr() & 0x3F; }
__declspec(dllexport) unsigned int _controlfp(unsigned int newv, unsigned int mask) { (void)newv; (void)mask; return 0x0009001F; }
__declspec(dllexport) int _controlfp_s(unsigned int *cur, unsigned int newv, unsigned int mask) { if (cur) *cur = _controlfp(newv, mask); return 0; }
__declspec(dllexport) unsigned int _control87(unsigned int newv, unsigned int mask) { return _controlfp(newv, mask); }
/* x86: both units' control words at once */
__declspec(dllexport) int __control87_2(unsigned int newv, unsigned int mask, unsigned int *x87, unsigned int *sse)
{
    if (x87) *x87 = _controlfp(newv, mask);
    if (sse) *sse = _controlfp(newv, mask);
    return 1;
}
/* _CW_DEFAULT: every exception masked, round to nearest, 53-bit x87 precision */
__declspec(dllexport) void _fpreset(void) { unsigned short cw = 0x27F; set_mxcsr(0x1F80); __asm__ volatile("fninit\n\tfldcw %0" : : "m"(cw)); }
__declspec(dllexport) int *__fpecode(void) { return &__nova_ptd()->fpecode; }

#ifndef _WIN64
/* x86: the UCRT's math for code built with /arch:SSE2 (MSVC's x86
 * default): _libm_sse2_NAME_precise takes its argument in xmm0 (pow: xmm0
 * and xmm1) and returns in xmm0; the C function underneath returns on the
 * x87 stack, as x86 cdecl does (lld-link takes one underscore off an
 * x86 export name, hence two) */
#define SSE2_MATH1(n) ".globl __libm_sse2_" #n "_precise\n__libm_sse2_" #n "_precise:\n\t" \
    "subl $8, %esp\n\tmovq %xmm0, (%esp)\n\tcalll _" #n "\n\t"                            \
    "fstpl (%esp)\n\tmovq (%esp), %xmm0\n\taddl $8, %esp\n\tretl\n"
__asm__(".section .text$libm_sse2,\"xr\"\n"
        SSE2_MATH1(acos) SSE2_MATH1(asin) SSE2_MATH1(atan) SSE2_MATH1(cos) SSE2_MATH1(exp)
        SSE2_MATH1(log) SSE2_MATH1(log10) SSE2_MATH1(sin) SSE2_MATH1(sqrt) SSE2_MATH1(tan)
        ".globl __libm_sse2_pow_precise\n__libm_sse2_pow_precise:\n\t"
        "subl $16, %esp\n\tmovq %xmm0, (%esp)\n\tmovq %xmm1, 8(%esp)\n\tcalll _pow\n\t"
        "fstpl (%esp)\n\tmovq (%esp), %xmm0\n\taddl $16, %esp\n\tretl\n"
        ".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:__libm_sse2_acos_precise=__libm_sse2_acos_precise"
        " /EXPORT:__libm_sse2_asin_precise=__libm_sse2_asin_precise"
        " /EXPORT:__libm_sse2_atan_precise=__libm_sse2_atan_precise"
        " /EXPORT:__libm_sse2_cos_precise=__libm_sse2_cos_precise"
        " /EXPORT:__libm_sse2_exp_precise=__libm_sse2_exp_precise"
        " /EXPORT:__libm_sse2_log_precise=__libm_sse2_log_precise"
        " /EXPORT:__libm_sse2_log10_precise=__libm_sse2_log10_precise"
        " /EXPORT:__libm_sse2_pow_precise=__libm_sse2_pow_precise"
        " /EXPORT:__libm_sse2_sin_precise=__libm_sse2_sin_precise"
        " /EXPORT:__libm_sse2_sqrt_precise=__libm_sse2_sqrt_precise"
        " /EXPORT:__libm_sse2_tan_precise=__libm_sse2_tan_precise\"\n\t"
        ".text\n");

/* x86: the older x87 forms (_CINAME) take their arguments on the x87
 * stack (two: the first in st(1), the second in st(0)) and return in st(0) */
#define CI_MATH1(n) ".globl __CI" #n "\n__CI" #n ":\n\t"                                  \
    "subl $8, %esp\n\tfstpl (%esp)\n\tcalll _" #n "\n\taddl $8, %esp\n\tretl\n"
#define CI_MATH2(n) ".globl __CI" #n "\n__CI" #n ":\n\t"                                  \
    "subl $16, %esp\n\tfstpl 8(%esp)\n\tfstpl (%esp)\n\tcalll _" #n "\n\taddl $16, %esp\n\tretl\n"
#define CI_EXPORT(n) " /EXPORT:__CI" #n "=__CI" #n
__asm__(".section .text$ci_math,\"xr\"\n"
        CI_MATH1(acos) CI_MATH1(asin) CI_MATH1(atan) CI_MATH1(cos) CI_MATH1(cosh) CI_MATH1(exp)
        CI_MATH1(log) CI_MATH1(log10) CI_MATH1(sin) CI_MATH1(sinh) CI_MATH1(sqrt) CI_MATH1(tan)
        CI_MATH1(tanh) CI_MATH2(atan2) CI_MATH2(fmod) CI_MATH2(pow)
        ".section .drectve,\"yn\"\n\t"
        ".ascii \"" CI_EXPORT(acos) CI_EXPORT(asin) CI_EXPORT(atan) CI_EXPORT(cos) CI_EXPORT(cosh)
        CI_EXPORT(exp) CI_EXPORT(log) CI_EXPORT(log10) CI_EXPORT(sin) CI_EXPORT(sinh) CI_EXPORT(sqrt)
        CI_EXPORT(tan) CI_EXPORT(tanh) CI_EXPORT(atan2) CI_EXPORT(fmod) CI_EXPORT(pow) "\"\n\t"
        ".text\n");
#endif
