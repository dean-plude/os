/* msvcrt: Microsoft's names for math functions (the C99 ones are musl's)
 * and the floating-point environment (SSE MXCSR + x87 control word) */
#define NOVA_BUILD_MSVCRT
#include <math.h>
#include <float.h>
#include <stdint.h>

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
__declspec(dllexport) void _fpreset(void) { set_mxcsr(0x1F80); __asm__ volatile("fninit"); }
__declspec(dllexport) int *__fpecode(void) { static int c; return &c; }
