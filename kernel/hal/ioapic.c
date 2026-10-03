/*
 * ioapic.c — I/O APICs: routing a device interrupt to a CPU
 */

#include "ioapic.h"
#include "acpi.h"
#include "pci.h"
#include "../ke/kpcr.h"
#include "../ke/printf.h"
#include "../lib/string.h"

#define MAX_IOAPICS 8
#define MAX_ISOS    16
#define MAX_ROUTES  8

typedef struct {
    volatile UINT32 *regs;
    UINT32 gsi_base, inputs;
} IoApic;

typedef struct { UINT8 irq; UINT32 gsi; UINT16 flags; } Override;
typedef struct { UINT32 gsi; UINT64 entry; } Route;

static IoApic   g_io[MAX_IOAPICS];
static int      g_nio;
static Override g_iso[MAX_ISOS];
static int      g_niso;
static Route    g_route[MAX_ROUTES];
static int      g_nroute;

static UINT32 io_read(IoApic *a, UINT32 reg)            { a->regs[0] = reg; return a->regs[4]; }
static void   io_write(IoApic *a, UINT32 reg, UINT32 v) { a->regs[0] = reg; a->regs[4] = v; }

static IoApic *owner(UINT32 gsi)
{
    for (int i = 0; i < g_nio; i++)
        if (gsi >= g_io[i].gsi_base && gsi < g_io[i].gsi_base + g_io[i].inputs) return &g_io[i];
    return NULL;
}

static void set_entry(UINT32 gsi, UINT64 e)
{
    IoApic *a = owner(gsi);
    if (!a) return;
    UINT32 n = gsi - a->gsi_base;
    io_write(a, 0x10 + 2 * n, (UINT32)e | (1u << 16));    /* masked while it changes */
    io_write(a, 0x11 + 2 * n, (UINT32)(e >> 32));
    io_write(a, 0x10 + 2 * n, (UINT32)e);
}

static void mask_all(void)
{
    for (int i = 0; i < g_nio; i++)
        for (UINT32 n = 0; n < g_io[i].inputs; n++) io_write(&g_io[i], 0x10 + 2 * n, 1u << 16);
}

bool IoApicInit(void)
{
    const UINT8 *m = AcpiFindTable("APIC");
    if (!m) return false;
    UINT32 len = *(const UINT32 *)(m + 4);
    for (UINT32 off = 44; off + 2 <= len && m[off + 1] >= 2; off += m[off + 1]) {
        const UINT8 *e = m + off;
        if (e[0] == 1 && e[1] >= 12 && g_nio < MAX_IOAPICS) {
            UINT32 addr, base;
            memcpy(&addr, e + 4, 4);
            memcpy(&base, e + 8, 4);
            IoApic *a = &g_io[g_nio];
            a->regs = (volatile UINT32 *)PciMapPhysical(addr, 0x20);
            if (!a->regs) continue;
            a->gsi_base = base;
            a->inputs = ((io_read(a, 1) >> 16) & 0xFF) + 1;
            g_nio++;
        } else if (e[0] == 2 && e[1] >= 10 && g_niso < MAX_ISOS) {
            Override *o = &g_iso[g_niso++];
            o->irq = e[3];
            memcpy(&o->gsi, e + 4, 4);
            memcpy(&o->flags, e + 8, 2);
        }
    }
    mask_all();
    if (g_nio) kprintf("[IOAPIC] %d I/O APIC(s), %u inputs from GSI %u\n", g_nio, g_io[0].inputs, g_io[0].gsi_base);
    return g_nio > 0;
}

bool IoApicPresent(void) { return g_nio > 0; }

bool IoApicRouteIrq(UINT32 irq, bool level, bool low, UINT8 vector, UINT32 *gsi_out)
{
    UINT32 gsi = irq;
    for (int i = 0; i < g_niso; i++) {
        if (g_iso[i].irq != irq) continue;
        gsi = g_iso[i].gsi;
        UINT16 pol = g_iso[i].flags & 3, trig = (g_iso[i].flags >> 2) & 3;
        if (pol == 1) low = false; else if (pol == 3) low = true;
        if (trig == 1) level = false; else if (trig == 3) level = true;
    }
    if (!owner(gsi) || g_nroute == MAX_ROUTES) return false;
    UINT64 e = vector | (low ? 1u << 13 : 0) | (level ? 1u << 15 : 0) | ((UINT64)g_kpcr[0].ApicId << 56);
    g_route[g_nroute++] = (Route){ gsi, e };
    set_entry(gsi, e);
    if (gsi_out) *gsi_out = gsi;
    return true;
}

void IoApicMask(UINT32 gsi, bool mask)
{
    for (int i = 0; i < g_nroute; i++)
        if (g_route[i].gsi == gsi) set_entry(gsi, g_route[i].entry | (mask ? 1u << 16 : 0));
}

void IoApicResume(void)
{
    mask_all();
    for (int i = 0; i < g_nroute; i++) set_entry(g_route[i].gsi, g_route[i].entry);
}

/* -----------------------------------------------------------------------
 * Handlers
 * ----------------------------------------------------------------------- */
static IrqHandler g_fn[256];
static void      *g_ctx[256];

bool IrqInstall(UINT8 vector, IrqHandler fn, void *ctx)
{
    if (g_fn[vector] && fn) return false;
    g_ctx[vector] = ctx;
    __atomic_store_n(&g_fn[vector], fn, __ATOMIC_RELEASE);
    return true;
}

bool IrqDispatch(UINT8 vector)
{
    IrqHandler fn = __atomic_load_n(&g_fn[vector], __ATOMIC_ACQUIRE);
    if (!fn) return false;
    fn(g_ctx[vector]);
    return true;
}
