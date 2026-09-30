/*
 * printf.c — kernel formatted output
 *
 * This is a freestanding implementation — no libc.
 * We use __builtin_va_list (compiler-provided even in -ffreestanding mode).
 */

#include "spinlock.h"
#include "printf.h"
#include "../hal/serial.h"
#include "../hal/framebuffer.h"

/* -----------------------------------------------------------------------
 * Internal: format a number into a character buffer
 * Returns pointer to start of string (buffer is filled from the END)
 * ----------------------------------------------------------------------- */

#define NUM_BUF_SIZE 72   /* Enough for 64-bit binary (64 digits) + prefix + NUL */

static const char hex_lower[] = "0123456789abcdef";
static const char hex_upper[] = "0123456789ABCDEF";

typedef enum { BASE_DEC = 10, BASE_HEX = 16, BASE_OCT = 8, BASE_BIN = 2 } NumBase;

static char *format_uint(char *end, uint64_t val, NumBase base,
                          const char *digits, bool uppercase)
{
    (void)uppercase;  /* digits array already reflects case */
    *--end = '\0';
    if (val == 0) {
        *--end = '0';
        return end;
    }
    while (val > 0) {
        *--end = digits[val % (uint64_t)base];
        val   /= (uint64_t)base;
    }
    return end;
}

/* -----------------------------------------------------------------------
 * kvsnprintf — core formatter
 * ----------------------------------------------------------------------- */
int kvsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap)
{
    size_t written = 0;

/* (the character is evaluated exactly once: PUTS passes *_s++) */
#define PUTC(c) do {                          \
    char _c = (c);                            \
    if (n > 0 && written < n - 1) {           \
        buf[written] = _c;                    \
    }                                         \
    written++;                                \
} while (0)

#define PUTS(s) do {                          \
    const char *_s = (s);                     \
    while (*_s) { PUTC(*_s++); }             \
} while (0)

    while (*fmt) {
        if (*fmt != '%') {
            PUTC(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        /* Flags */
        bool zero_pad = false;
        bool alt_form = false;
        while (*fmt == '0' || *fmt == '#') {
            if (*fmt == '0') zero_pad = true;
            if (*fmt == '#') alt_form = true;
            fmt++;
        }

        /* Width */
        int width = 0;
        while (*fmt >= '1' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }

        /* Length modifier */
        int len = 0;  /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'l') {
            len = 1; fmt++;
            if (*fmt == 'l') { len = 2; fmt++; }
        } else if (*fmt == 'z') {
            len = 3; fmt++;
        }

        char        num_buf[NUM_BUF_SIZE];
        char       *num_start;
        const char *s;
        char        c;
        uint64_t    u;
        int64_t     d;
        bool        negative = false;

        switch (*fmt) {
        case 'd':
        case 'i':
            if      (len == 2) d = (int64_t)__builtin_va_arg(ap, long long);
            else if (len == 1) d = (int64_t)__builtin_va_arg(ap, long);
            else if (len == 3) d = (int64_t)__builtin_va_arg(ap, size_t);
            else               d = (int64_t)__builtin_va_arg(ap, int);
            if (d < 0) { negative = true; u = 0 - (uint64_t)d; }  /* safe for INT64_MIN */
            else                          { u = (uint64_t)d; }
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_DEC, hex_lower, false);
            goto print_num;

        case 'u':
            if      (len == 2) u = (uint64_t)__builtin_va_arg(ap, unsigned long long);
            else if (len == 1) u = (uint64_t)__builtin_va_arg(ap, unsigned long);
            else if (len == 3) u = (uint64_t)__builtin_va_arg(ap, size_t);
            else               u = (uint64_t)__builtin_va_arg(ap, unsigned int);
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_DEC, hex_lower, false);
            goto print_num;

        case 'x':
            if      (len == 2) u = (uint64_t)__builtin_va_arg(ap, unsigned long long);
            else if (len == 1) u = (uint64_t)__builtin_va_arg(ap, unsigned long);
            else if (len == 3) u = (uint64_t)__builtin_va_arg(ap, size_t);
            else               u = (uint64_t)__builtin_va_arg(ap, unsigned int);
            if (alt_form) { PUTC('0'); PUTC('x'); }
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_HEX, hex_lower, false);
            goto print_num;

        case 'X':
            if      (len == 2) u = (uint64_t)__builtin_va_arg(ap, unsigned long long);
            else if (len == 1) u = (uint64_t)__builtin_va_arg(ap, unsigned long);
            else if (len == 3) u = (uint64_t)__builtin_va_arg(ap, size_t);
            else               u = (uint64_t)__builtin_va_arg(ap, unsigned int);
            if (alt_form) { PUTC('0'); PUTC('X'); }
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_HEX, hex_upper, true);
            goto print_num;

        case 'o':
            if      (len == 2) u = (uint64_t)__builtin_va_arg(ap, unsigned long long);
            else if (len == 1) u = (uint64_t)__builtin_va_arg(ap, unsigned long);
            else if (len == 3) u = (uint64_t)__builtin_va_arg(ap, size_t);
            else               u = (uint64_t)__builtin_va_arg(ap, unsigned int);
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_OCT, hex_lower, false);
            goto print_num;

        case 'b':  /* binary — non-standard extension */
            if      (len == 2) u = (uint64_t)__builtin_va_arg(ap, unsigned long long);
            else if (len == 1) u = (uint64_t)__builtin_va_arg(ap, unsigned long);
            else if (len == 3) u = (uint64_t)__builtin_va_arg(ap, size_t);
            else               u = (uint64_t)__builtin_va_arg(ap, unsigned int);
            if (alt_form) { PUTC('0'); PUTC('b'); }
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_BIN, hex_lower, false);
            goto print_num;

        case 'p':
            u = (uint64_t)(uintptr_t)__builtin_va_arg(ap, void *);
            PUTC('0'); PUTC('x');
            /* Pointers always printed as 16 hex digits */
            zero_pad = true;
            width    = 16;
            num_start = format_uint(num_buf + NUM_BUF_SIZE, u, BASE_HEX, hex_lower, false);
            goto print_num;

        print_num: {
            int num_len  = (int)((num_buf + NUM_BUF_SIZE - 1) - num_start);
            int pad      = width - num_len - (negative ? 1 : 0);
            if (negative) PUTC('-');
            if (zero_pad) { while (pad-- > 0) PUTC('0'); }
            else          { while (pad-- > 0) PUTC(' '); }
            PUTS(num_start);
            break;
        }

        case 's':
            s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            PUTS(s);
            break;

        case 'c':
            c = (char)__builtin_va_arg(ap, int);
            PUTC(c);
            break;

        case '%':
            PUTC('%');
            break;

        default:
            PUTC('%');
            PUTC(*fmt);
            break;
        }
        fmt++;
    }

    /* NUL-terminate if there's room */
    if (n > 0) {
        buf[written < n ? written : n - 1] = '\0';
    }
    return (int)written;

#undef PUTC
#undef PUTS
}

/* -----------------------------------------------------------------------
 * Public APIs
 * ----------------------------------------------------------------------- */

/* When false, kprintf output is suppressed on the framebuffer (serial
 * still receives everything).  The desktop shell disables it once it has
 * painted the screen so boot-log text can't corrupt the rendered UI. */
static bool g_fb_output = true;

void kprintf_set_fb_enabled(bool enabled)
{
    g_fb_output = enabled;
}

/* Last KLOG_SIZE bytes of kernel output, for the Terminal's `dmesg` */
#define KLOG_SIZE 16384
static char   g_klog[KLOG_SIZE];
static size_t g_klog_total;          /* bytes ever written */

/* One message at a time, from any CPU (and whole: not interleaved) */
static KSpinLock g_print_lock = KSPINLOCK_INIT;

static void klog_append(const char *s)
{
    for (; *s; s++) g_klog[g_klog_total++ % KLOG_SIZE] = *s;
}

size_t klog_read(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    IrqState irq = spin_lock_irqsave(&g_print_lock);
    size_t avail = g_klog_total < KLOG_SIZE ? g_klog_total : KLOG_SIZE;
    size_t n = avail < cap - 1 ? avail : cap - 1;
    size_t start = g_klog_total - n;
    for (size_t i = 0; i < n; i++) out[i] = g_klog[(start + i) % KLOG_SIZE];
    out[n] = '\0';
    spin_unlock_irqrestore(&g_print_lock, irq);
    return n;
}

void kvprintf(const char *fmt, __builtin_va_list ap)
{
    char buf[1024];
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    IrqState irq = spin_lock_irqsave(&g_print_lock);
    klog_append(buf);
    serial_puts(buf);
    if (g_fb_output)
        fb_puts(buf);
    spin_unlock_irqrestore(&g_print_lock, irq);
}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

void early_printf(const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    serial_puts(buf);
}
