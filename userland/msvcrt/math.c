/* msvcrt: <math.h> — double-precision routines (range reduction + polynomials) */
#define NOVA_BUILD_MSVCRT
#include <math.h>
#include <stdint.h>
#include <errno.h>

__declspec(dllexport) int _fltused = 0x9875;

static uint64_t bits(double x)      { union { double d; uint64_t u; } v = { x }; return v.u; }
static double   from(uint64_t u)    { union { uint64_t u; double d; } v = { u }; return v.d; }

double fabs(double x)   { return from(bits(x) & ~(1ULL << 63)); }

double sqrt(double x)
{
    if (x < 0) { errno = EDOM; return __builtin_nan(""); }
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double trunc(double x)
{
    if (fabs(x) >= 4503599627370496.0 || x != x) return x;   /* 2^52: already integral */
    return (double)(long long)x;
}

double floor(double x) { double t = trunc(x); return t > x ? t - 1 : t; }
double ceil(double x)  { double t = trunc(x); return t < x ? t + 1 : t; }
double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
long   lround(double x){ return (long)round(x); }

double fmod(double x, double y)
{
    if (y == 0 || x != x || y != y) { errno = EDOM; return __builtin_nan(""); }
    double q = trunc(x / y);
    double r = x - q * y;
    if (fabs(r) >= fabs(y)) r = fmod(r, y);
    return r;
}

double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }

double ldexp(double x, int e)
{
    if (x == 0 || x != x || fabs(x) == __builtin_inf()) return x;
    while (e > 1000)  { x *= 0x1p1000;  e -= 1000; }
    while (e < -1000) { x *= 0x1p-1000; e += 1000; }
    return x * from((uint64_t)(e + 1023) << 52);
}

double frexp(double x, int *e)
{
    if (x == 0 || x != x || fabs(x) == __builtin_inf()) { *e = 0; return x; }
    int k = 0;
    if (fabs(x) < 0x1p-1000) { x *= 0x1p64; k = -64; }       /* subnormals */
    uint64_t u = bits(x);
    int ex = (int)((u >> 52) & 0x7FF);
    *e = ex - 1022 + k;
    return from((u & ~(0x7FFULL << 52)) | (1022ULL << 52));
}

double exp(double x)
{
    if (x != x) return x;
    if (x > 709.78) { errno = ERANGE; return __builtin_inf(); }
    if (x < -745.2) return 0;
    const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10;
    double n = round(x / M_LN2);
    double r = (x - n * ln2_hi) - n * ln2_lo;               /* |r| <= ln2/2 */
    double term = 1, sum = 1;
    for (int i = 1; i < 16; i++) { term *= r / i; sum += term; }
    return ldexp(sum, (int)n);
}

double log(double x)
{
    if (x < 0 || x != x) { errno = EDOM; return __builtin_nan(""); }
    if (x == 0) { errno = ERANGE; return -__builtin_inf(); }
    if (x == __builtin_inf()) return x;
    int e;
    double m = frexp(x, &e);                                 /* [0.5, 1) */
    if (m < M_SQRT2 / 2) { m *= 2; e--; }
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int k = 1; k < 40; k += 2) { sum += term / k; term *= s2; }
    return 2 * sum + e * M_LN2;
}

double log10(double x) { return log(x) / M_LN10; }
double log2(double x)  { return log(x) / M_LN2; }

double pow(double x, double y)
{
    if (y == 0) return 1;
    if (x == 1) return 1;
    if (x != x || y != y) return __builtin_nan("");
    double yi = trunc(y);
    if (yi == y && fabs(y) < 2147483648.0) {                  /* integer power: exact-ish */
        long long n = (long long)y;
        int neg = n < 0;
        if (neg) n = -n;
        double r = 1, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (x < 0) { errno = EDOM; return __builtin_nan(""); }
    if (x == 0) return y > 0 ? 0 : __builtin_inf();
    return exp(y * log(x));
}

/* sin/cos on [-pi/4, pi/4] */
static double sin_k(double x) { double x2 = x * x, t = x, s = x; for (int i = 1; i < 12; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; } return s; }
static double cos_k(double x) { double x2 = x * x, t = 1, s = 1; for (int i = 1; i < 12; i++) { t *= -x2 / ((2 * i - 1) * (2 * i)); s += t; } return s; }

static double reduce(double x, int *q)                        /* x = q*(pi/2) + r */
{
    const double p1 = 1.5707963267341256e+00, p2 = 6.0771005065061922e-11, p3 = 2.0222662487959506e-21;
    double n = round(x / (M_PI / 2));
    *q = (int)((long long)n & 3);
    return ((x - n * p1) - n * p2) - n * p3;
}

double sin(double x)
{
    if (x != x || fabs(x) == __builtin_inf()) return __builtin_nan("");
    int q;
    double r = reduce(x, &q);
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}

double cos(double x)
{
    if (x != x || fabs(x) == __builtin_inf()) return __builtin_nan("");
    int q;
    double r = reduce(x, &q);
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}

double tan(double x) { return sin(x) / cos(x); }

double atan(double x)
{
    if (x != x) return x;
    int neg = x < 0;
    if (neg) x = -x;
    double base = 0;
    if (x > 2.414213562373095) { base = M_PI / 2; x = -1 / x; }
    else if (x > 0.4142135623730950) { base = M_PI / 4; x = (x - 1) / (x + 1); }
    double x2 = x * x, t = x, s = x;
    for (int k = 3; k < 60; k += 2) { t *= -x2; s += t / k; }
    s += base;
    return neg ? -s : s;
}

double atan2(double y, double x)
{
    if (x > 0) return atan(y / x);
    if (x < 0) return y >= 0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    if (y > 0) return M_PI / 2;
    if (y < 0) return -M_PI / 2;
    return 0;
}

double asin(double x)  { if (fabs(x) > 1) { errno = EDOM; return __builtin_nan(""); } return atan2(x, sqrt(1 - x * x)); }
double acos(double x)  { if (fabs(x) > 1) { errno = EDOM; return __builtin_nan(""); } return atan2(sqrt(1 - x * x), x); }
double sinh(double x)  { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x)  { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x)  { if (x > 20) return 1; if (x < -20) return -1; double e = exp(2 * x); return (e - 1) / (e + 1); }

double hypot(double x, double y)
{
    x = fabs(x); y = fabs(y);
    double m = x > y ? x : y;
    if (m == 0) return 0;
    x /= m; y /= m;
    return m * sqrt(x * x + y * y);
}

float sqrtf(float x)          { return (float)sqrt(x); }
float fabsf(float x)          { return (float)fabs(x); }
float floorf(float x)         { return (float)floor(x); }
float ceilf(float x)          { return (float)ceil(x); }
float sinf(float x)           { return (float)sin(x); }
float cosf(float x)           { return (float)cos(x); }
float powf(float x, float y)  { return (float)pow(x, y); }
