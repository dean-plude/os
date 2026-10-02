/*
 * ksym.c — the kernel's own function names, for readable backtraces
 *
 * tools/mkksyms.py writes the table (the .ksyms section, between the
 * linker.ld symbols __ksyms_start and __ksyms_end) from a first link of
 * the kernel; the second link embeds it.  A kernel fault or panic prints
 *
 *   Backtrace:
 *     #0 ffffffff80267434  KeCrashTestFault+0x4
 *     #1 ffffffff8026744b  KeCrashTest+0xa
 *
 * walking the RBP chain (the kernel is built with frame pointers).
 */
#include "ksym.h"
#include "printf.h"

typedef struct { UINT64 addr; UINT32 name, size; } KsymEntry;

extern const UINT8 __ksyms_start[], __ksyms_end[];
extern const UINT8 __text_start[], __text_end[];

static const KsymEntry *table(UINT64 *count, const char **names)
{
    if (__ksyms_end - __ksyms_start < 8) { *count = 0; return NULL; }   /* the first link: no table */
    *count = *(const UINT64 *)__ksyms_start;
    const KsymEntry *e = (const KsymEntry *)(__ksyms_start + 8);
    *names = (const char *)(e + *count);
    return e;
}

const char *KsymLookup(UINT64 addr, UINT64 *offset)
{
    UINT64 n;
    const char *names;
    const KsymEntry *e = table(&n, &names);
    if (!n || addr < (UINT64)__text_start || addr >= (UINT64)__text_end) return NULL;
    UINT64 lo = 0, hi = n;                       /* the last entry at or below addr */
    while (hi - lo > 1) {
        UINT64 mid = (lo + hi) / 2;
        if (e[mid].addr <= addr) lo = mid; else hi = mid;
    }
    if (e[lo].addr > addr) return NULL;
    if (offset) *offset = addr - e[lo].addr;
    return names + e[lo].name;
}

static void frame_line(int i, UINT64 pc)
{
    UINT64 off = 0;
    const char *name = KsymLookup(pc, &off);
    if (name) kprintf("  #%d %016lx  %s+0x%lx\n", i, (unsigned long)pc, name, (unsigned long)off);
    else      kprintf("  #%d %016lx  ?\n", i, (unsigned long)pc);
}

void KsymBacktrace(UINT64 rip, UINT64 rbp, UINT64 rsp)
{
    kprintf("Backtrace:\n");
    int i = 0;
    frame_line(i++, rip);
    /* each frame: [rbp] = the caller's rbp, [rbp + 8] = the return address;
     * only frames on this stack (from @rsp up 128 KiB) are read, so a
     * corrupt chain cannot fault in here */
    for (; i < 32; i++) {
        if (rbp < rsp || rbp + 16 > rsp + 0x20000 || (rbp & 7)) break;
        const UINT64 *f = (const UINT64 *)rbp;
        UINT64 ret = f[1], next = f[0];
        if (ret < (UINT64)__text_start || ret >= (UINT64)__text_end) break;
        frame_line(i, ret - 1);                  /* (inside the call instruction) */
        if (next <= rbp) break;
        rbp = next;
    }
}

void KsymBacktraceHere(void)
{
    UINT64 fp = (UINT64)__builtin_frame_address(0);
    KsymBacktrace((UINT64)__builtin_return_address(0), *(const UINT64 *)fp, fp);
}

/* NtNovaBugCheck (crash.exe kernel): a deliberate kernel fault, three calls
 * deep, to show the backtrace.  Only with the magic argument. */
__attribute__((noinline)) void KeCrashTestFault(volatile UINT32 *p)
{
    *p = 0xDEAD;
    __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void KeCrashTest(void)
{
    KeCrashTestFault((volatile UINT32 *)0xFFFF8FFFDEAD0000ULL);
    __asm__ volatile("" ::: "memory");
}
