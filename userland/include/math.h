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
#define M_SQRT1_2  0.70710678118654752440
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define HUGE_VAL   __builtin_huge_val()
#define INFINITY   __builtin_inff()
#define NAN        __builtin_nanf("")
#define isnan(x)   __builtin_isnan(x)
#define isinf(x)   __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
_CRTIMP double sqrt(double x);
_CRTIMP double fabs(double x);
_CRTIMP double floor(double x);
_CRTIMP double ceil(double x);
_CRTIMP double round(double x);
_CRTIMP double trunc(double x);
_CRTIMP double fmod(double x, double y);
_CRTIMP double modf(double x, double *ip);
_CRTIMP double frexp(double x, int *e);
_CRTIMP double ldexp(double x, int e);
_CRTIMP double exp(double x);
_CRTIMP double log(double x);
_CRTIMP double log10(double x);
_CRTIMP double log2(double x);
_CRTIMP double pow(double x, double y);
_CRTIMP double sin(double x);
_CRTIMP double cos(double x);
_CRTIMP double tan(double x);
_CRTIMP double asin(double x);
_CRTIMP double acos(double x);
_CRTIMP double atan(double x);
_CRTIMP double atan2(double y, double x);
_CRTIMP double sinh(double x);
_CRTIMP double cosh(double x);
_CRTIMP double tanh(double x);
_CRTIMP double hypot(double x, double y);
_CRTIMP float  sqrtf(float x);
_CRTIMP float  fabsf(float x);
_CRTIMP float  floorf(float x);
_CRTIMP float  ceilf(float x);
_CRTIMP float  roundf(float x);
_CRTIMP long   lroundf(float x);
_CRTIMP float  truncf(float x);
_CRTIMP float  fmodf(float x, float y);
_CRTIMP float  sinf(float x);
_CRTIMP float  cosf(float x);
_CRTIMP float  powf(float x, float y);
_CRTIMP long   lround(double x);
_NOVA_END
