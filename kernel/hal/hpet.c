/*
 * hpet.c — the High Precision Event Timer (see hpet.h)
 *
 * The ACPI "HPET" table gives the address of the first timer block.  Its
 * capabilities register says how many femtoseconds one count of the main
 * counter lasts and whether the counter is 64 bits wide; setting
 * ENABLE_CNF in the configuration register starts it.  A 32-bit counter
 * wraps every few minutes, so readings are only ever compared over short
 * spans (calibration), modulo its width.
 */

#include "hpet.h"
#include "../arch/x86_64/cpu.h"
#include "../ke/printf.h"
#include "../lib/string.h"

#define HPET_CAP        0x000           /* capabilities and ID */
#define HPET_CONF       0x010           /* general configuration */
#define HPET_COUNTER    0x0F0           /* main counter */

#define CAP_COUNT_64    (1ull << 13)
#define CONF_ENABLE     (1ull << 0)

static volatile UINT8 *g_regs;
static UINT64 g_hz;
static bool   g_wide;                   /* 64-bit main counter */

static UINT64 reg_read(UINT32 off) { return *(volatile UINT64 *)(g_regs + off); }
static void reg_write(UINT32 off, UINT64 v) { *(volatile UINT64 *)(g_regs + off) = v; }

/* The table with signature @sig from the RSDT/XSDT (acpi.c isn't up yet) */
static const UINT8 *early_table(UINT64 rsdp, const char *sig)
{
    if (!rsdp) return NULL;
    const UINT8 *r = PHYS_TO_VIRT(rsdp);
    if (memcmp(r, "RSD PTR ", 8)) return NULL;
    UINT64 xsdt = r[15] >= 2 ? *(const UINT64 *)(r + 24) : 0;
    UINT32 rsdt = *(const UINT32 *)(r + 16);
    UINT64 rootpa = xsdt ? xsdt : rsdt;
    if (!rootpa) return NULL;
    const UINT8 *root = PHYS_TO_VIRT(rootpa);
    UINT32 len = *(const UINT32 *)(root + 4), esize = xsdt ? 8 : 4;
    if (len < 36) return NULL;
    for (UINT32 off = 36; off + esize <= len; off += esize) {
        UINT64 pa = esize == 8 ? *(const UINT64 *)(root + off) : *(const UINT32 *)(root + off);
        if (!pa) continue;
        const UINT8 *h = PHYS_TO_VIRT(pa);
        if (!memcmp(h, sig, 4)) return h;
    }
    return NULL;
}

bool HpetInit(UINT64 rsdp)
{
    const UINT8 *t = early_table(rsdp, "HPET");
    if (!t || *(const UINT32 *)(t + 4) < 56) {
        kprintf("[HPET] None (the timers come from CPUID 0x15 or the PIT)\n");
        return false;
    }
    UINT8 space = t[40];                                /* base address: a GAS at offset 40 */
    UINT64 pa = *(const UINT64 *)(t + 44);
    if (space != 0 || !pa) {
        kprintf("[HPET] Not memory-mapped: not used\n");
        return false;
    }
    g_regs = PHYS_TO_VIRT(pa);
    UINT64 cap = reg_read(HPET_CAP);
    UINT32 fs = (UINT32)(cap >> 32);                    /* femtoseconds per count */
    if (!fs || fs > 100000000u) {                       /* (at most 100 ns, says the spec) */
        kprintf("[HPET] Bad counter period %u fs: not used\n", fs);
        g_regs = NULL;
        return false;
    }
    g_hz = 1000000000000000ull / fs;
    g_wide = (cap & CAP_COUNT_64) != 0;
    reg_write(HPET_CONF, reg_read(HPET_CONF) | CONF_ENABLE);
    kprintf("[HPET] At 0x%llx: %llu Hz, %u-bit main counter, %u comparators\n",
            (unsigned long long)pa, (unsigned long long)g_hz, g_wide ? 64 : 32,
            (unsigned)((cap >> 8) & 0x1F) + 1);
    return true;
}

bool HpetPresent(void) { return g_regs != NULL; }
UINT64 HpetFrequency(void) { return g_hz; }

UINT64 HpetCounter(void)
{
    if (!g_regs) return 0;
    if (g_wide) return reg_read(HPET_COUNTER);
    return *(volatile UINT32 *)(g_regs + HPET_COUNTER);
}

void HpetDelay(UINT64 us)
{
    UINT64 mask = g_wide ? ~0ull : 0xFFFFFFFFull;
    UINT64 start = HpetCounter(), n = g_hz * us / 1000000;
    while (((HpetCounter() - start) & mask) < n) pause_cpu();
}
