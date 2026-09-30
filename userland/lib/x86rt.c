/* x86rt.c — the 64-bit division helpers 32-bit x86 code calls
 * (_alldiv, _aulldiv, _allrem, _aullrem: what the MSVC runtime's
 * lldiv.asm and friends provide).  Linked into every 32-bit module; they
 * take their operands on the stack, return in EDX:EAX and pop 16 bytes. */
#ifndef _WIN64

typedef unsigned long long u64;
typedef long long s64;

static u64 udivmod(u64 n, u64 d, u64 *rem)
{
    if (!d) {                                   /* the CPU's divide error, as the real helpers */
        volatile unsigned z = 0;
        *rem = 0;
        return (unsigned)n / z;
    }
    if (!(d >> 32)) {                           /* 32-bit divisor: two divl */
        unsigned dv = (unsigned)d, hi = (unsigned)(n >> 32), lo = (unsigned)n;
        unsigned qhi = hi / dv, r = hi % dv, qlo;
        __asm__("divl %4" : "=a"(qlo), "=d"(r) : "a"(lo), "d"(r), "rm"(dv));
        *rem = r;
        return ((u64)qhi << 32) | qlo;
    }
    u64 q = 0, r = 0;                           /* the quotient fits in 32 bits */
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    *rem = r;
    return q;
}

u64 __stdcall nova_aulldiv(u64 a, u64 b) __asm__("__aulldiv");
u64 __stdcall nova_aulldiv(u64 a, u64 b) { u64 r; return udivmod(a, b, &r); }

u64 __stdcall nova_aullrem(u64 a, u64 b) __asm__("__aullrem");
u64 __stdcall nova_aullrem(u64 a, u64 b) { u64 r; udivmod(a, b, &r); return r; }

s64 __stdcall nova_alldiv(s64 a, s64 b) __asm__("__alldiv");
s64 __stdcall nova_alldiv(s64 a, s64 b)
{
    int neg = (a < 0) ^ (b < 0);
    u64 r, q = udivmod(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return neg ? -(s64)q : (s64)q;
}

s64 __stdcall nova_allrem(s64 a, s64 b) __asm__("__allrem");
s64 __stdcall nova_allrem(s64 a, s64 b)
{
    u64 r;
    udivmod(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(s64)r : (s64)r;
}

s64 __stdcall nova_allmul(s64 a, s64 b) __asm__("__allmul");
s64 __stdcall nova_allmul(s64 a, s64 b) { return a * b; }

#endif
