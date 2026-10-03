/*
 * The Universal C Runtime's complex numbers beyond C99's (which come from
 * musl, third_party/musl/src/complex): Microsoft's builders and
 * multipliers for _Dcomplex/_Fcomplex/_Lcomplex (structs of two numbers,
 * which have the same calling convention as C's _Complex types), norm and
 * the old _cabs.  Also the helpers the compiler calls for complex
 * multiplication and division, with C11 Annex G's handling of infinities
 * and NaNs (G.5.1, examples 1 and 2).
 */
#include <math.h>

#define CRTEXP __declspec(dllexport)

typedef double _Complex dcomplex;
typedef float _Complex fcomplex;

static dcomplex dmake(double re, double im) { union { dcomplex z; double v[2]; } u = { .v = { re, im } }; return u.z; }
static fcomplex fmake(float re, float im)   { union { fcomplex z; float v[2]; } u = { .v = { re, im } }; return u.z; }

dcomplex __muldc3(double a, double b, double c, double d)
{
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    double x = ac - bd, y = ad + bc;
    if (isnan(x) && isnan(y)) {
        int recalc = 0;
        if (isinf(a) || isinf(b)) {                 /* z is infinite: box it */
            a = copysign(isinf(a) ? 1.0 : 0.0, a);
            b = copysign(isinf(b) ? 1.0 : 0.0, b);
            if (isnan(c)) c = copysign(0.0, c);
            if (isnan(d)) d = copysign(0.0, d);
            recalc = 1;
        }
        if (isinf(c) || isinf(d)) {                 /* w is infinite */
            c = copysign(isinf(c) ? 1.0 : 0.0, c);
            d = copysign(isinf(d) ? 1.0 : 0.0, d);
            if (isnan(a)) a = copysign(0.0, a);
            if (isnan(b)) b = copysign(0.0, b);
            recalc = 1;
        }
        if (!recalc && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
            if (isnan(a)) a = copysign(0.0, a);     /* overflow in the products */
            if (isnan(b)) b = copysign(0.0, b);
            if (isnan(c)) c = copysign(0.0, c);
            if (isnan(d)) d = copysign(0.0, d);
            recalc = 1;
        }
        if (recalc) {
            x = INFINITY * (a * c - b * d);
            y = INFINITY * (a * d + b * c);
        }
    }
    return dmake(x, y);
}

dcomplex __divdc3(double a, double b, double c, double d)
{
    int ilogbw = 0;
    double logbw = logb(fmax(fabs(c), fabs(d)));
    if (isfinite(logbw)) {
        ilogbw = (int)logbw;
        c = scalbn(c, -ilogbw);
        d = scalbn(d, -ilogbw);
    }
    double denom = c * c + d * d;
    double x = scalbn((a * c + b * d) / denom, -ilogbw);
    double y = scalbn((b * c - a * d) / denom, -ilogbw);
    if (isnan(x) && isnan(y)) {
        if (denom == 0.0 && (!isnan(a) || !isnan(b))) {
            x = copysign(INFINITY, c) * a;
            y = copysign(INFINITY, c) * b;
        } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
            a = copysign(isinf(a) ? 1.0 : 0.0, a);
            b = copysign(isinf(b) ? 1.0 : 0.0, b);
            x = INFINITY * (a * c + b * d);
            y = INFINITY * (b * c - a * d);
        } else if (isinf(logbw) && logbw > 0.0 && isfinite(a) && isfinite(b)) {
            c = copysign(isinf(c) ? 1.0 : 0.0, c);
            d = copysign(isinf(d) ? 1.0 : 0.0, d);
            x = 0.0 * (a * c + b * d);
            y = 0.0 * (b * c - a * d);
        }
    }
    return dmake(x, y);
}

/* float: worked in double, which neither overflows nor loses precision */
fcomplex __mulsc3(float a, float b, float c, float d)
{
    dcomplex z = __muldc3(a, b, c, d);
    return fmake((float)__real__ z, (float)__imag__ z);
}

fcomplex __divsc3(float a, float b, float c, float d)
{
    dcomplex z = __divdc3(a, b, c, d);
    return fmake((float)__real__ z, (float)__imag__ z);
}

/* <complex.h>'s Microsoft additions (long double is double here) */
CRTEXP dcomplex _Cbuild(double re, double im)  { return dmake(re, im); }
CRTEXP fcomplex _FCbuild(float re, float im)   { return fmake(re, im); }
CRTEXP dcomplex _LCbuild(long double re, long double im) { return dmake((double)re, (double)im); }
CRTEXP dcomplex _Cmulcc(dcomplex x, dcomplex y) { return __muldc3(__real__ x, __imag__ x, __real__ y, __imag__ y); }
CRTEXP dcomplex _Cmulcr(dcomplex x, double y)   { return dmake(__real__ x * y, __imag__ x * y); }
CRTEXP fcomplex _FCmulcc(fcomplex x, fcomplex y) { return __mulsc3(__real__ x, __imag__ x, __real__ y, __imag__ y); }
CRTEXP fcomplex _FCmulcr(fcomplex x, float y)   { return fmake(__real__ x * y, __imag__ x * y); }
CRTEXP dcomplex _LCmulcc(dcomplex x, dcomplex y) { return _Cmulcc(x, y); }
CRTEXP dcomplex _LCmulcr(dcomplex x, long double y) { return _Cmulcr(x, (double)y); }
CRTEXP double norm(dcomplex z)      { return __real__ z * __real__ z + __imag__ z * __imag__ z; }
CRTEXP float normf(fcomplex z)      { return __real__ z * __real__ z + __imag__ z * __imag__ z; }
CRTEXP long double norml(dcomplex z) { return norm(z); }

/* _cabs(struct _complex { double x, y; }): passed as a complex double is */
CRTEXP double _cabs(dcomplex z) { return hypot(__real__ z, __imag__ z); }
