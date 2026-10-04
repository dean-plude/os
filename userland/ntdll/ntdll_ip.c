/*
 * ntdll_ip.c — IP address text to binary: RtlIpv4StringToAddress(Ex) and
 * RtlIpv6StringToAddress(Ex), A and W (Chromium's network code calls the
 * Ex forms).
 *
 * IPv4: strict text is four decimal numbers; otherwise inet_aton's forms
 * are taken too (one to four parts, octal with a leading 0, hex with 0x,
 * the last part filling the bytes left).  IPv6: RFC 4291 2.2 text (eight
 * groups, one "::", an IPv4 tail).  The plain forms stop at the first
 * character that does not belong and say where (the terminator); the Ex
 * forms take the whole string, with a port (v4 "a.b.c.d:port", v6
 * "[addr%scope]:port") and a v6 scope id; ports come back in network
 * order, 0 when there is none.
 */
#define NOVA_BUILD_NTDLL
#include <windows.h>
#include <winternl.h>

#define BAD STATUS_INVALID_PARAMETER

static int digit(WCHAR c, int base)
{
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    return v < base ? v : -1;
}

/* IPv4 at @s; the end in *@end */
static NTSTATUS v4(const WCHAR *s, BOOLEAN strict, const WCHAR **end, UCHAR out[4])
{
    ULONG part[4];
    int n = 0;
    const WCHAR *p = s;
    for (;;) {
        int base = 10;
        if (!strict && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (!strict && p[0] == '0' && digit(p[1], 10) >= 0) { base = 8; p++; }
        if (digit(*p, base) < 0) {
            if (base != 16 || n == 4) { *end = p; return BAD; }
        }
        ULONGLONG v = 0;
        int digits = 0;
        while (digit(*p, base) >= 0) {
            v = v * (ULONG)base + (ULONG)digit(*p++, base);
            if (v > 0xFFFFFFFFULL) { *end = p; return BAD; }
            digits++;
        }
        if (strict && (digits > 3 || v > 255)) { *end = p; return BAD; }
        if (n == 4) { *end = p; return BAD; }
        part[n++] = (ULONG)v;
        if (*p == '.' && n < 4 && digit(p[1], 10) >= 0) { p++; continue; }
        break;
    }
    *end = p;
    if (strict && n != 4) return BAD;
    ULONG a;
    switch (n) {
    case 1: a = part[0]; break;
    case 2: if (part[0] > 255 || part[1] > 0xFFFFFF) return BAD; a = part[0] << 24 | part[1]; break;
    case 3: if (part[0] > 255 || part[1] > 255 || part[2] > 0xFFFF) return BAD;
            a = part[0] << 24 | part[1] << 16 | part[2]; break;
    default: for (int i = 0; i < 4; i++) if (part[i] > 255) return BAD;
             a = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
    }
    out[0] = (UCHAR)(a >> 24); out[1] = (UCHAR)(a >> 16); out[2] = (UCHAR)(a >> 8); out[3] = (UCHAR)a;
    return STATUS_SUCCESS;
}

/* IPv6 at @s; the end in *@end */
static NTSTATUS v6(const WCHAR *s, const WCHAR **end, UCHAR out[16])
{
    USHORT g[8];
    int n = 0, gap = -1;
    const WCHAR *p = s;
    if (p[0] == ':' && p[1] == ':') {
        gap = 0;
        p += 2;
        if (digit(*p, 16) < 0) goto done;
    }
    for (;;) {
        const WCHAR *q = p;                     /* an IPv4 tail: digits, then a dot */
        while (digit(*q, 10) >= 0) q++;
        if (*q == '.' && q > p) {
            UCHAR b[4];
            const WCHAR *e;
            if (n > 6 || v4(p, TRUE, &e, b)) { *end = p; return BAD; }
            g[n++] = (USHORT)(b[0] << 8 | b[1]);
            g[n++] = (USHORT)(b[2] << 8 | b[3]);
            p = e;
            break;
        }
        int digits = 0;
        ULONG v = 0;
        while (digit(*p, 16) >= 0) { v = v << 4 | (ULONG)digit(*p++, 16); if (++digits > 4) { *end = p; return BAD; } }
        if (!digits || n == 8) { *end = p; return BAD; }
        g[n++] = (USHORT)v;
        if (p[0] == ':' && p[1] == ':') {
            if (gap >= 0) { *end = p; return BAD; }
            gap = n;
            p += 2;
            if (digit(*p, 16) < 0) break;
            continue;
        }
        if (p[0] == ':' && digit(p[1], 16) >= 0 && n < 8) { p++; continue; }
        break;
    }
done:
    *end = p;
    if (gap < 0 ? n != 8 : n > 7) return BAD;
    int fill = 8 - n, o = 0;
    for (int i = 0; i <= n; i++) {
        if (i == gap) for (int k = 0; k < fill; k++) { out[o++] = 0; out[o++] = 0; }
        if (i < n) { out[o++] = (UCHAR)(g[i] >> 8); out[o++] = (UCHAR)g[i]; }
    }
    return STATUS_SUCCESS;
}

/* a decimal (or 0x hex) number of at most @max, the whole of what is
 * left */
static int number(const WCHAR *p, ULONG max, ULONG *v)
{
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (digit(*p, base) < 0) return 0;
    ULONGLONG x = 0;
    while (digit(*p, base) >= 0) { x = x * (ULONG)base + (ULONG)digit(*p++, base); if (x > max) return 0; }
    *v = (ULONG)x;
    return !*p;
}

static USHORT net16(ULONG v) { return (USHORT)((v & 0xFF) << 8 | (v >> 8 & 0xFF)); }

static NTSTATUS v4ex(const WCHAR *s, BOOLEAN strict, UCHAR out[4], USHORT *port)
{
    const WCHAR *e;
    ULONG pv = 0;
    if (!s || !out || !port) return BAD;
    if (v4(s, strict, &e, out)) return BAD;
    if (*e == ':') { if (!number(e + 1, 0xFFFF, &pv) || !pv) return BAD; }
    else if (*e) return BAD;
    *port = net16(pv);
    return STATUS_SUCCESS;
}

static NTSTATUS v6ex(const WCHAR *s, UCHAR out[16], ULONG *scope, USHORT *port)
{
    const WCHAR *e;
    ULONG sc = 0, pv = 0;
    int bracket = 0;
    if (!s || !out || !scope || !port) return BAD;
    if (*s == '[') { bracket = 1; s++; }
    if (v6(s, &e, out)) return BAD;
    if (*e == '%') {
        const WCHAR *q = ++e;
        while (digit(*e, 10) >= 0) e++;
        if (e == q) return BAD;
        WCHAR t[16];
        int n = 0;
        for (; q < e && n < 15; q++) t[n++] = *q;
        t[n] = 0;
        if (q < e || !number(t, 0xFFFFFFFF, &sc)) return BAD;
    }
    if (bracket) {
        if (*e++ != ']') return BAD;
        if (*e == ':') { if (!number(e + 1, 0xFFFF, &pv) || !pv) return BAD; }
        else if (*e) return BAD;
    } else if (*e) return BAD;
    *scope = sc;
    *port = net16(pv);
    return STATUS_SUCCESS;
}

/* The A forms on a wide copy (address text is ASCII) */
#define WIDE(a, w) WCHAR w[128]; { int i_ = 0; if (!(a)) return BAD; for (; (a)[i_] && i_ < 127; i_++) w[i_] = (UCHAR)(a)[i_]; w[i_] = 0; }

NTSYSAPI NTSTATUS NTAPI RtlIpv4StringToAddressW(PCWSTR s, BOOLEAN strict, PCWSTR *term, void *addr)
{
    const WCHAR *e = s;
    if (!s || !term || !addr) return BAD;
    NTSTATUS r = v4(s, strict, &e, addr);
    *term = e;
    return r;
}

NTSYSAPI NTSTATUS NTAPI RtlIpv4StringToAddressA(const char * s, BOOLEAN strict, const char **term, void *addr)
{
    WIDE(s, w);
    const WCHAR *e = w;
    if (!term || !addr) return BAD;
    NTSTATUS r = v4(w, strict, &e, addr);
    *term = s + (e - w);
    return r;
}

NTSYSAPI NTSTATUS NTAPI RtlIpv4StringToAddressExW(PCWSTR s, BOOLEAN strict, void *addr, USHORT * port)
{
    return v4ex(s, strict, addr, port);
}

NTSYSAPI NTSTATUS NTAPI RtlIpv4StringToAddressExA(const char * s, BOOLEAN strict, void *addr, USHORT * port)
{
    WIDE(s, w);
    return v4ex(w, strict, addr, port);
}

NTSYSAPI NTSTATUS NTAPI RtlIpv6StringToAddressW(PCWSTR s, PCWSTR *term, void *addr)
{
    const WCHAR *e = s;
    if (!s || !term || !addr) return BAD;
    NTSTATUS r = v6(s, &e, addr);
    *term = e;
    return r;
}

NTSYSAPI NTSTATUS NTAPI RtlIpv6StringToAddressA(const char * s, const char **term, void *addr)
{
    WIDE(s, w);
    const WCHAR *e = w;
    if (!term || !addr) return BAD;
    NTSTATUS r = v6(w, &e, addr);
    *term = s + (e - w);
    return r;
}

NTSYSAPI NTSTATUS NTAPI RtlIpv6StringToAddressExW(PCWSTR s, void *addr, ULONG * scope, USHORT * port)
{
    return v6ex(s, addr, scope, port);
}

NTSYSAPI NTSTATUS NTAPI RtlIpv6StringToAddressExA(const char * s, void *addr, ULONG * scope, USHORT * port)
{
    WIDE(s, w);
    return v6ex(w, addr, scope, port);
}
