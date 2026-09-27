/*
 * tls_platform.c — the libc-style services Mbed TLS needs in the kernel
 *
 * Memory (kernel heap), a C99 snprintf (Mbed TLS relies on its "would have
 * written" return value to detect truncation), wall-clock and monotonic
 * time, and the hardware entropy hook (tls.c's entropy pool).
 */

#include "../include/types.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"
#include "../hal/rtc.h"
#include "tls.h"

#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/entropy.h"

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */
void *nova_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;
    size_t total = n * size;
    return kzalloc(total ? total : 1);
}

void nova_free(void *p)
{
    if (p) kfree(p);
}

/* -----------------------------------------------------------------------
 * Formatted output (subset of C99: flags "-0#+ ", width, precision,
 * length hh/h/l/ll/z/j/t, conversions d i u x X o c s p %)
 * ----------------------------------------------------------------------- */
typedef struct { char *s; size_t n, len; } Out;

static void put(Out *o, char c)
{
    if (o->len + 1 < o->n) o->s[o->len] = c;
    o->len++;
}

static void pad(Out *o, char c, int count)
{
    while (count-- > 0) put(o, c);
}

int nova_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    Out o = { s, n, 0 };
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        f++;
        bool left = false, zero = false, alt = false, plus = false, space = false;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') zero = true;
            else if (*f == '#') alt = true;
            else if (*f == '+') plus = true;
            else if (*f == ' ') space = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); f++; if (width < 0) { left = true; width = -width; } }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; if (prec < 0) prec = -1; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;                                   /* 0 int, 1 long, 2 long long/size */
        for (;; f++) {
            if (*f == 'l') lng++;
            else if (*f == 'z' || *f == 'j' || *f == 't') lng = 2;
            else if (*f == 'h') { }
            else break;
        }
        char conv = *f;
        if (!conv) break;

        if (conv == '%') { put(&o, '%'); continue; }
        if (conv == 'c') {
            if (!left) pad(&o, ' ', width - 1);
            put(&o, (char)va_arg(ap, int));
            if (left) pad(&o, ' ', width - 1);
            continue;
        }
        if (conv == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int len = (int)strlen(str);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++) put(&o, str[i]);
            if (left) pad(&o, ' ', width - len);
            continue;
        }

        /* integers */
        UINT64 v;
        bool neg = false;
        int base = 10;
        bool upper = false;
        if (conv == 'd' || conv == 'i') {
            INT64 sv = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            neg = sv < 0;
            v = neg ? (UINT64)(-(sv + 1)) + 1 : (UINT64)sv;
        } else if (conv == 'p') {
            v = (UINT64)(uintptr_t)va_arg(ap, void *);
            base = 16; alt = true;
        } else {
            v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long)
                         : va_arg(ap, unsigned int);
            if (conv == 'x' || conv == 'X') { base = 16; upper = conv == 'X'; }
            else if (conv == 'o') base = 8;
            else if (conv != 'u') continue;                 /* unsupported: skip */
        }
        char digits[24];
        int nd = 0;
        do {
            int d = (int)(v % (UINT64)base);
            digits[nd++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            v /= (UINT64)base;
        } while (v);
        if (prec == 0 && nd == 1 && digits[0] == '0') nd = 0;

        char prefix[3];
        int np = 0;
        if (neg) prefix[np++] = '-';
        else if (plus && (conv == 'd' || conv == 'i')) prefix[np++] = '+';
        else if (space && (conv == 'd' || conv == 'i')) prefix[np++] = ' ';
        if (alt && base == 16 && (nd || conv == 'p')) { prefix[np++] = '0'; prefix[np++] = upper ? 'X' : 'x'; }
        else if (alt && base == 8) prefix[np++] = '0';

        int zeros = prec > nd ? prec - nd : 0;
        int total = np + zeros + nd;
        if (zero && !left && prec < 0 && width > total) { zeros += width - total; total = width; }
        if (!left) pad(&o, ' ', width - total);
        for (int i = 0; i < np; i++) put(&o, prefix[i]);
        pad(&o, '0', zeros);
        while (nd) put(&o, digits[--nd]);
        if (left) pad(&o, ' ', width - total);
    }
    if (o.n) o.s[o.len < o.n ? o.len : o.n - 1] = '\0';
    return (int)o.len;
}

int nova_snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = nova_vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int nova_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int r = nova_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("%s", buf);
    return r;
}

/* No files or processes in this configuration: never called. */
int  nova_fprintf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
void nova_setbuf(void *f, char *buf)             { (void)f; (void)buf; }
void nova_exit(int status)                       { kprintf("[TLS] exit(%d)\n", status); for (;;) sched_yield(); }

/* -----------------------------------------------------------------------
 * Time — the CMOS RTC runs on UTC
 * ----------------------------------------------------------------------- */
static INT64 days_from_civil(int y, int m, int d)          /* days since 1970-01-01 */
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (INT64)era * 146097 + doe - 719468;
}

long long nova_time(long long *t)
{
    RtcTime r;
    rtc_read(&r);
    long long now = days_from_civil(r.year, r.month, r.day) * 86400LL +
                    r.hour * 3600 + r.minute * 60 + r.second;
    if (t) *t = now;
    return now;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm)
{
    INT64 secs = *tt, days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min  = (int)(rem % 3600 / 60);
    tm->tm_sec  = (int)(rem % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);                /* 1970-01-01 was a Thursday */
    /* civil_from_days (H. Hinnant) */
    INT64 z = days + 719468;
    INT64 era = (z >= 0 ? z : z - 146096) / 146097;
    INT64 doe = z - era * 146097;
    INT64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    INT64 y = yoe + era * 400;
    INT64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    INT64 mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon  = m - 1;
    tm->tm_mday = d;
    tm->tm_yday = (int)(days - days_from_civil((int)y, 1, 1));
    tm->tm_isdst = 0;
    return tm;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(sched_ticks() * 10);
}

/* -----------------------------------------------------------------------
 * Entropy (MBEDTLS_ENTROPY_HARDWARE_ALT)
 * ----------------------------------------------------------------------- */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    TlsEntropyOutput(output, len);
    *olen = len;
    return 0;
}
