/* math.h — C99 math (the implementations are musl's, third_party/musl)
 * plus Microsoft's underscore names.  long double is double on Windows. */
#pragma once
#include <_nova.h>
_NOVA_BEGIN
#define M_PI       3.14159265358979323846
#define M_E        2.71828182845904523536
#define M_SQRT2    1.41421356237309504880
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT1_2  0.70710678118654752440
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define HUGE_VAL   __builtin_huge_val()
#define HUGE_VALF  __builtin_huge_valf()
#define HUGE_VALL  __builtin_huge_vall()
#define INFINITY   __builtin_inff()
#define NAN        __builtin_nanf("")
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define FP_ILOGB0    (-2147483647 - 1)
#define FP_ILOGBNAN  (-2147483647 - 1)
#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling 2
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)
typedef float float_t;
typedef double double_t;

_CRTIMP double acos(double x); _CRTIMP float acosf(float x); _CRTIMP long double acosl(long double x);
_CRTIMP double asin(double x); _CRTIMP float asinf(float x); _CRTIMP long double asinl(long double x);
_CRTIMP double atan(double x); _CRTIMP float atanf(float x); _CRTIMP long double atanl(long double x);
_CRTIMP double cos(double x); _CRTIMP float cosf(float x); _CRTIMP long double cosl(long double x);
_CRTIMP double sin(double x); _CRTIMP float sinf(float x); _CRTIMP long double sinl(long double x);
_CRTIMP double tan(double x); _CRTIMP float tanf(float x); _CRTIMP long double tanl(long double x);
_CRTIMP double acosh(double x); _CRTIMP float acoshf(float x); _CRTIMP long double acoshl(long double x);
_CRTIMP double asinh(double x); _CRTIMP float asinhf(float x); _CRTIMP long double asinhl(long double x);
_CRTIMP double atanh(double x); _CRTIMP float atanhf(float x); _CRTIMP long double atanhl(long double x);
_CRTIMP double cosh(double x); _CRTIMP float coshf(float x); _CRTIMP long double coshl(long double x);
_CRTIMP double sinh(double x); _CRTIMP float sinhf(float x); _CRTIMP long double sinhl(long double x);
_CRTIMP double tanh(double x); _CRTIMP float tanhf(float x); _CRTIMP long double tanhl(long double x);
_CRTIMP double exp(double x); _CRTIMP float expf(float x); _CRTIMP long double expl(long double x);
_CRTIMP double exp2(double x); _CRTIMP float exp2f(float x); _CRTIMP long double exp2l(long double x);
_CRTIMP double expm1(double x); _CRTIMP float expm1f(float x); _CRTIMP long double expm1l(long double x);
_CRTIMP double log(double x); _CRTIMP float logf(float x); _CRTIMP long double logl(long double x);
_CRTIMP double log10(double x); _CRTIMP float log10f(float x); _CRTIMP long double log10l(long double x);
_CRTIMP double log1p(double x); _CRTIMP float log1pf(float x); _CRTIMP long double log1pl(long double x);
_CRTIMP double log2(double x); _CRTIMP float log2f(float x); _CRTIMP long double log2l(long double x);
_CRTIMP double logb(double x); _CRTIMP float logbf(float x); _CRTIMP long double logbl(long double x);
_CRTIMP double cbrt(double x); _CRTIMP float cbrtf(float x); _CRTIMP long double cbrtl(long double x);
_CRTIMP double fabs(double x); _CRTIMP float fabsf(float x); _CRTIMP long double fabsl(long double x);
_CRTIMP double sqrt(double x); _CRTIMP float sqrtf(float x); _CRTIMP long double sqrtl(long double x);
_CRTIMP double erf(double x); _CRTIMP float erff(float x); _CRTIMP long double erfl(long double x);
_CRTIMP double erfc(double x); _CRTIMP float erfcf(float x); _CRTIMP long double erfcl(long double x);
_CRTIMP double lgamma(double x); _CRTIMP float lgammaf(float x); _CRTIMP long double lgammal(long double x);
_CRTIMP double tgamma(double x); _CRTIMP float tgammaf(float x); _CRTIMP long double tgammal(long double x);
_CRTIMP double ceil(double x); _CRTIMP float ceilf(float x); _CRTIMP long double ceill(long double x);
_CRTIMP double floor(double x); _CRTIMP float floorf(float x); _CRTIMP long double floorl(long double x);
_CRTIMP double nearbyint(double x); _CRTIMP float nearbyintf(float x); _CRTIMP long double nearbyintl(long double x);
_CRTIMP double rint(double x); _CRTIMP float rintf(float x); _CRTIMP long double rintl(long double x);
_CRTIMP double round(double x); _CRTIMP float roundf(float x); _CRTIMP long double roundl(long double x);
_CRTIMP double trunc(double x); _CRTIMP float truncf(float x); _CRTIMP long double truncl(long double x);
_CRTIMP double atan2(double x, double y); _CRTIMP float atan2f(float x, float y); _CRTIMP long double atan2l(long double x, long double y);
_CRTIMP double fmod(double x, double y); _CRTIMP float fmodf(float x, float y); _CRTIMP long double fmodl(long double x, long double y);
_CRTIMP double pow(double x, double y); _CRTIMP float powf(float x, float y); _CRTIMP long double powl(long double x, long double y);
_CRTIMP double hypot(double x, double y); _CRTIMP float hypotf(float x, float y); _CRTIMP long double hypotl(long double x, long double y);
_CRTIMP double copysign(double x, double y); _CRTIMP float copysignf(float x, float y); _CRTIMP long double copysignl(long double x, long double y);
_CRTIMP double fdim(double x, double y); _CRTIMP float fdimf(float x, float y); _CRTIMP long double fdiml(long double x, long double y);
_CRTIMP double fmax(double x, double y); _CRTIMP float fmaxf(float x, float y); _CRTIMP long double fmaxl(long double x, long double y);
_CRTIMP double fmin(double x, double y); _CRTIMP float fminf(float x, float y); _CRTIMP long double fminl(long double x, long double y);
_CRTIMP double nextafter(double x, double y); _CRTIMP float nextafterf(float x, float y); _CRTIMP long double nextafterl(long double x, long double y);
_CRTIMP double remainder(double x, double y); _CRTIMP float remainderf(float x, float y); _CRTIMP long double remainderl(long double x, long double y);
_CRTIMP double fma(double x, double y, double z); _CRTIMP float fmaf(float x, float y, float z);
_CRTIMP double frexp(double x, int *e); _CRTIMP float frexpf(float x, int *e);
_CRTIMP double ldexp(double x, int e);  _CRTIMP float ldexpf(float x, int e);
_CRTIMP double scalbn(double x, int e); _CRTIMP float scalbnf(float x, int e);
_CRTIMP double scalbln(double x, long e);
_CRTIMP double modf(double x, double *ip); _CRTIMP float modff(float x, float *ip);
_CRTIMP double remquo(double x, double y, int *q);
_CRTIMP int    ilogb(double x);
_CRTIMP long   lround(double x); _CRTIMP long lroundf(float x);
_CRTIMP long long llround(double x); _CRTIMP long long llroundf(float x);
_CRTIMP long   lrint(double x); _CRTIMP long long llrint(double x);
_CRTIMP double nan(const char *s); _CRTIMP float nanf(const char *s);
_CRTIMP double nexttoward(double x, long double y);
_CRTIMP double j0(double x); _CRTIMP double j1(double x); _CRTIMP double jn(int n, double x);
_CRTIMP double y0(double x); _CRTIMP double y1(double x); _CRTIMP double yn(int n, double x);
/* Microsoft names */
_CRTIMP double _hypot(double x, double y);
_CRTIMP double _copysign(double x, double y);
_CRTIMP double _chgsign(double x);
_CRTIMP double _logb(double x);
_CRTIMP double _nextafter(double x, double y);
_CRTIMP double _scalb(double x, long e);
_CRTIMP int    _finite(double x);
_CRTIMP int    _isnan(double x);
_CRTIMP int    _fpclass(double x);
_CRTIMP double _j0(double x); _CRTIMP double _j1(double x); _CRTIMP double _jn(int n, double x);
_CRTIMP double _y0(double x); _CRTIMP double _y1(double x); _CRTIMP double _yn(int n, double x);
_NOVA_END
