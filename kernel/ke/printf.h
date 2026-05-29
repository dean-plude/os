/*
 * printf.h — kernel debug printing
 *
 * kprintf() outputs to both the serial port and the framebuffer console.
 * It's intended for kernel debug/diagnostic output, not user-facing strings.
 *
 * Format specifiers supported:
 *   %d / %i  — signed decimal int (int / long / long long via length modifier)
 *   %u       — unsigned decimal
 *   %x / %X  — lowercase/uppercase hex
 *   %o       — octal
 *   %p       — pointer (= 0x + 16 hex digits)
 *   %s       — null-terminated string
 *   %c       — character
 *   %b       — binary (non-standard, useful for bit flags)
 *   %%       — literal percent
 *
 * Length modifiers:
 *   l   — long
 *   ll  — long long
 *   z   — size_t
 *
 * Flag modifiers:
 *   0   — zero-pad (e.g. %08x)
 *   -   — left-align (not yet implemented, ignored)
 *
 * Width is supported (e.g. %16lx).
 * Precision is NOT supported.
 */

#pragma once

#include "../include/types.h"

/*
 * Print a formatted string to serial + framebuffer.
 */
void kprintf(const char *fmt, ...);

/*
 * Print a formatted string to a fixed-size buffer.
 * Returns the number of characters written (not including NUL terminator).
 * Always NUL-terminates if n > 0.
 */
int ksnprintf(char *buf, size_t n, const char *fmt, ...);

/*
 * Variadic versions.
 */
void kvprintf(const char *fmt, __builtin_va_list ap);
int kvsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap);

/*
 * Early (serial-only) printf for use before the framebuffer is initialized.
 */
void early_printf(const char *fmt, ...);

/*
 * Enable or disable framebuffer output for kprintf.  Serial output is
 * unaffected.  The desktop shell disables it after painting so kernel
 * log text does not overwrite the rendered UI.
 */
void kprintf_set_fb_enabled(bool enabled);

/*
 * Kernel assertion with descriptive panic message.
 */
#define KASSERT(cond)  do {                                          \
    if (__builtin_expect(!(cond), 0)) {                             \
        kprintf("ASSERTION FAILED: %s\n"                            \
                "  at %s:%d in %s()\n",                             \
                #cond, __FILE__, __LINE__, __func__);                \
        for (;;) { __asm__ volatile ("cli; hlt"); }                 \
    }                                                                \
} while (0)

#define KPANIC(fmt, ...) do {                                        \
    kprintf("\n*** KERNEL PANIC ***\n");                             \
    kprintf("  " fmt "\n", ##__VA_ARGS__);                          \
    kprintf("  at %s:%d in %s()\n", __FILE__, __LINE__, __func__);  \
    for (;;) { __asm__ volatile ("cli; hlt"); }                     \
} while (0)
