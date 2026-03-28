/*
 * console.c — UEFI console helpers for the bootloader
 *
 * Provides printf-style output via EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.
 * UCS-2 conversion is intentionally minimal (ASCII only) — the bootloader
 * only ever prints ASCII text.
 */

#include "../include/efi.h"

/* The system table pointer is set by efi_main before any console calls. */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_conout;

void console_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *conout)
{
    g_conout = conout;
    conout->ClearScreen(conout);
}

/* Print a UCS-2 (wide) string directly. */
void console_puts_w(CHAR16 *s)
{
    g_conout->OutputString(g_conout, s);
}

/* Print an ASCII string by converting one char at a time.
 * We use a small stack buffer to batch characters for efficiency. */
void console_puts(const char *s)
{
    CHAR16 buf[128];
    int    i = 0;

    while (*s) {
        buf[i++] = (CHAR16)(unsigned char)*s++;
        if (i == 126 || *s == '\0') {
            buf[i] = 0;
            g_conout->OutputString(g_conout, buf);
            i = 0;
        }
    }
}

/* Minimal hex formatter — no heap needed. */
static void print_hex64(UINT64 val)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];  /* "0x" + 16 hex digits + NUL */
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(val >> (60 - i * 4)) & 0xF];
    }
    buf[18] = '\0';
    console_puts(buf);
}

static void print_dec64(UINT64 val)
{
    if (val == 0) { console_puts("0"); return; }
    char buf[21];
    int  i = 20;
    buf[i] = '\0';
    while (val && i > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    console_puts(buf + i);
}

/*
 * console_printf — supports %s, %S (wide), %d, %u, %x/%X (64-bit), %p, %c
 * Not a full printf — only what the bootloader needs.
 */
void console_printf(const char *fmt, ...)
{
    /* We implement our own va_arg to avoid including <stdarg.h> in UEFI env.
     * Actually stdarg.h is fine in freestanding — the compiler provides it. */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            char tmp[2] = { *fmt, '\0' };
            console_puts(tmp);
            fmt++;
            continue;
        }
        fmt++;  /* skip '%' */
        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            console_puts(s ? s : "(null)");
            break;
        }
        case 'S': {
            CHAR16 *ws = __builtin_va_arg(ap, CHAR16 *);
            if (ws) console_puts_w(ws);
            break;
        }
        case 'd': {
            INT64 v = __builtin_va_arg(ap, INT64);
            if (v < 0) { console_puts("-"); v = -v; }
            print_dec64((UINT64)v);
            break;
        }
        case 'u': {
            UINT64 v = __builtin_va_arg(ap, UINT64);
            print_dec64(v);
            break;
        }
        case 'x':
        case 'X':
        case 'p': {
            UINT64 v = __builtin_va_arg(ap, UINT64);
            print_hex64(v);
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            char tmp[2] = { c, '\0' };
            console_puts(tmp);
            break;
        }
        case '%':
            console_puts("%");
            break;
        default:
            console_puts("%?");
            break;
        }
        fmt++;
    }

    __builtin_va_end(ap);
}
