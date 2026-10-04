/*
 * gpio.c — Intel GPIO controllers (the chipset's pin controller)
 *
 * Laptop touchpads raise a GPIO pin when they have a report; the ACPI
 * namespace names it in the touchpad's GpioInt resource as a controller
 * (\_SB.GPI0, _HID INTC1055 on Alder and Raptor Lake laptops such as the
 * ThinkPad T14 Gen 4) and a pin number.  The controller is the chipset's
 * pin controller: a few "communities", each a block of memory-mapped
 * registers (one Memory32Fixed resource apiece), holding a configuration
 * register per pad and, for each group of up to 32 pads, an interrupt
 * status register (GPI_IS, write 1 to clear) and an enable register
 * (GPI_IE).  All the pins' interrupts share the controller's one
 * interrupt (its Interrupt resource, IRQ 14 on these chipsets).
 *
 * The ACPI pin number counts in groups starting at 32-pin boundaries
 * (the group's "GPIO base"), which differ from the pads' own numbering;
 * the tables below map one to the other for each chipset.  A connected
 * pin's pad is set up as an input in GPIO mode that interrupts on the
 * level or edge the resource asks for (RXINV inverts an active-low
 * line), with its other routes (SCI, SMI, NMI, I/O APIC) off.  A
 * level-triggered pin stays masked from its interrupt until its driver
 * has read what the device had (GpioIrqUnmask), as Linux's level IRQ
 * flow does.
 *
 * The register layout, the pad groups of each chipset and the save and
 * restore around sleep come from OpenBSD's pchgpio(4) (ISC licence,
 * Mark Kettenis and James Hastings); the pad configuration bits not used
 * there (the pad mode, the other interrupt routes) are Intel's documented
 * PAD_CFG_DW0 layout.
 *
 * _HID NOVA1055 is the self-tests' controller (tests/acpi/i2c-touchpad.asl):
 * a Tiger Lake-LP layout whose registers are memory here instead of
 * MMIO.  A modelled device drives its pins (GpioModelSetLine), and it
 * raises its interrupt by sending its vector to the CPU itself, as the
 * real controller's level-triggered line would through the I/O APIC.
 *
 * The parts from pchgpio.c carry its licence:
 *
 *   Copyright (c) 2020 Mark Kettenis
 *   Copyright (c) 2020 James Hastings
 *
 *   Permission to use, copy, modify, and distribute this software for any
 *   purpose with or without fee is hereby granted, provided that the above
 *   copyright notice and this permission notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "gpio.h"
#include "ioapic.h"
#include "pci.h"
#include "../ke/printf.h"
#include "../ke/spinlock.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/idt.h"

#include <uacpi/uacpi.h>
#include <uacpi/resources.h>
#include <uacpi/utilities.h>
#include <uacpi/namespace.h>

#define PADBAR          0x00C           /* a community's pad configuration registers start here */

#define CFG_RXSTATE     0x00000002      /* PAD_CFG_DW0: the input's level */
#define CFG_TXDIS       0x00000100
#define CFG_RXDIS       0x00000200
#define CFG_PMODE       0x00003C00      /*   pad mode: 0 GPIO, else a native function */
#define CFG_ROUTE       0x001E0000      /*   NMI, SMI, SCI, I/O APIC routes */
#define CFG_RXINV       0x00800000
#define CFG_RXEV_EDGE   0x02000000      /*   RXEVCFG: 0 level, 1 edge, 2 off, 3 both edges */
#define CFG_RXEV_ZERO   0x04000000
#define CFG_RXEV_MASK   0x06000000

#define MAX_COMM  5
#define MAX_CTRL  2
#define MAX_LINES 8
#define MODEL_REGS 0x1000               /* bytes of registers per modelled community */
#define MODEL_PADBAR 0x700

typedef struct { UINT8 bar, bank; UINT16 base, limit; INT16 gpiobase; const char *name; } Group;
typedef struct { const char *what; UINT16 pad_size, gpi_is, gpi_ie; const Group *groups; int ngroups; } Layout;

/* (community, register bank, first and last pad, GPIO base) */
static const Group g_tgl_lp[] = {
    { 0, 0,   0,  25,   0, "GPP_B" }, { 0, 1,  26,  41,  32, "GPP_T" }, { 0, 2,  42,  66,  64, "GPP_A" },
    { 1, 0,  67,  74,  96, "GPP_S" }, { 1, 1,  75,  98, 128, "GPP_H" }, { 1, 2,  99, 119, 160, "GPP_D" },
    { 1, 3, 120, 143, 192, "GPP_U" },
    { 2, 0, 171, 194, 256, "GPP_C" }, { 2, 1, 195, 219, 288, "GPP_F" }, { 2, 3, 226, 250, 320, "GPP_E" },
    { 3, 0, 260, 267, 352, "GPP_R" },
};
static const Group g_tgl_h[] = {
    { 0, 0,   0,  24,   0, "GPP_A" }, { 0, 1,  25,  44,  32, "GPP_R" }, { 0, 2,  45,  70,  64, "GPP_B" },
    { 1, 0,  79, 104, 128, "GPP_D" }, { 1, 1, 105, 128, 160, "GPP_C" }, { 1, 2, 129, 136, 192, "GPP_S" },
    { 1, 3, 137, 153, 224, "GPP_G" },
    { 2, 0, 181, 193, 288, "GPP_E" }, { 2, 1, 194, 217, 320, "GPP_F" },
    { 2, 0, 218, 241, 352, "GPP_H" }, { 2, 1, 242, 251, 384, "GPP_J" }, { 2, 2, 252, 266, 416, "GPP_K" },
    { 3, 0, 267, 281, 448, "GPP_I" },
};
static const Group g_adl_s[] = {
    { 0, 0,   0,  24,   0, "GPP_I" }, { 0, 1,  25,  47,  32, "GPP_R" }, { 0, 2,  48,  59,  64, "GPP_J" },
    { 1, 0,  95, 118, 160, "GPP_B" }, { 1, 1, 119, 126, 192, "GPP_G" }, { 1, 2, 127, 150, 224, "GPP_H" },
    { 2, 1, 160, 175, 256, "GPP_A" }, { 2, 2, 176, 199, 288, "GPP_C" },
    { 3, 0, 200, 207, 320, "GPP_S" }, { 3, 1, 208, 230, 352, "GPP_E" }, { 3, 2, 231, 245, 384, "GPP_K" },
    { 3, 3, 246, 269, 416, "GPP_F" },
    { 4, 0, 270, 294, 448, "GPP_D" },
};
static const Group g_adl_n[] = {
    { 0, 0,   0,  25,   0, "GPP_B" }, { 0, 1,  26,  41,  32, "GPP_T" }, { 0, 2,  42,  66,  64, "GPP_A" },
    { 1, 0,  67,  74,  96, "GPP_S" }, { 1, 1,  75,  94, 128, "GPP_I" }, { 1, 2,  95, 118, 160, "GPP_H" },
    { 1, 3, 119, 139, 192, "GPP_D" },
    { 2, 0, 169, 192, 256, "GPP_C" }, { 2, 1, 193, 217, 288, "GPP_F" }, { 2, 3, 224, 248, 320, "GPP_E" },
    { 3, 0, 249, 256, 352, "GPP_R" },
};
static const Group g_mtl_p[] = {
    { 0, 1,   5,  28,  32, "GPP_V" }, { 0, 2,  29,  52,  64, "GPP_C" },
    { 1, 0,  53,  77,  96, "GPP_A" }, { 1, 1,  78, 102, 128, "GPP_E" },
    { 2, 0, 103, 128, 160, "GPP_H" }, { 2, 1, 129, 154, 192, "GPP_F" },
    { 3, 0, 184, 191, 288, "GPP_S" },
    { 4, 0, 204, 228, 352, "GPP_B" }, { 4, 1, 229, 253, 384, "GPP_D" },
};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))
static const Layout g_lay_tgl_lp = { "Tiger Lake-LP", 16, 0x100, 0x120, g_tgl_lp, N(g_tgl_lp) };
static const Layout g_lay_tgl_h  = { "Tiger Lake-H",  16, 0x100, 0x120, g_tgl_h,  N(g_tgl_h) };
static const Layout g_lay_adl_s  = { "Alder Lake-S",  16, 0x200, 0x220, g_adl_s,  N(g_adl_s) };
static const Layout g_lay_adl_n  = { "Alder Lake-N",  16, 0x100, 0x120, g_adl_n,  N(g_adl_n) };
static const Layout g_lay_mtl_p  = { "Meteor Lake-P", 16, 0x200, 0x210, g_mtl_p,  N(g_mtl_p) };

static const struct { const char *hid; const Layout *lay; } g_match[] = {
    { "INT34C5",  &g_lay_tgl_lp },
    { "INT34C6",  &g_lay_tgl_h },
    { "INTC1055", &g_lay_tgl_lp },     /* Alder Lake-P, Raptor Lake-P/U (the T14 Gen 4) */
    { "INTC1056", &g_lay_adl_s },
    { "INTC1057", &g_lay_adl_n },
    { "INTC1085", &g_lay_adl_s },      /* Raptor Lake-S */
    { "INTC1083", &g_lay_mtl_p },
    { "INTC105E", &g_lay_mtl_p },
    { "NOVA1055", &g_lay_tgl_lp },     /* the self-tests' model */
};

typedef struct Ctrl Ctrl;

struct GpioIrq {
    Ctrl        *c;
    const Group *grp;
    bool         used, level, low, both;
    UINT16       pin, pad;
    UINT32       bit;
    UINT32       cfg;                   /* the pad's configuration as set up (restored after S3) */
    void       (*fn)(void *ctx);
    void        *ctx;
    volatile UINT32 count;
    char         name[64];
};

struct Ctrl {
    char            path[48], hid[12];
    const Layout   *lay;
    bool            model;
    int             nbar;
    volatile UINT8 *mmio[MAX_COMM];
    UINT32         *regs[MAX_COMM];     /* the model's registers */
    UINT32          padbar[MAX_COMM];
    UINT16          padbase[MAX_COMM];
    UINT32          irq, gsi;
    bool            has_irq, level, low, routed;
    UINT8           vector;
    KSpinLock       lock;
    GpioIrq         lines[MAX_LINES];
    UINT32          stray;              /* interrupts of pins nobody connected (masked) */
};

static Ctrl g_ctrl[MAX_CTRL];
static int  g_nctrl;

static void model_update(Ctrl *c);

static UINT32 rd(Ctrl *c, int bar, UINT32 off)
{
    if (c->model) return c->regs[bar][off / 4];
    return *(volatile UINT32 *)(c->mmio[bar] + off);
}

static void wr(Ctrl *c, int bar, UINT32 off, UINT32 v)
{
    if (!c->model) { *(volatile UINT32 *)(c->mmio[bar] + off) = v; return; }
    if (off >= c->lay->gpi_is && off < c->lay->gpi_is + 0x10) c->regs[bar][off / 4] &= ~v;   /* write 1 to clear */
    else c->regs[bar][off / 4] = v;
    model_update(c);
}

static UINT32 cfg_reg(Ctrl *c, int bar, UINT16 pad) { return c->padbar[bar] + (UINT32)pad * c->lay->pad_size; }
static UINT32 is_reg(Ctrl *c, const Group *g) { return c->lay->gpi_is + g->bank * 4u; }
static UINT32 ie_reg(Ctrl *c, const Group *g) { return c->lay->gpi_ie + g->bank * 4u; }

/* -----------------------------------------------------------------------
 * The self-tests' controller: what the hardware does with the registers
 * ----------------------------------------------------------------------- */

/* Level-triggered pads whose input is active set their status bit (again,
 * after a clear) as the real controller's do; a status bit whose enable is
 * set raises the controller's interrupt */
static void model_update(Ctrl *c)
{
    bool raise = false;
    for (int i = 0; i < c->lay->ngroups; i++) {
        const Group *g = &c->lay->groups[i];
        UINT32 *is = &c->regs[g->bar][is_reg(c, g) / 4];
        for (int b = 0; b <= g->limit - g->base; b++) {
            UINT32 cfg = c->regs[g->bar][cfg_reg(c, g->bar, (UINT16)(g->base + b - c->padbase[g->bar])) / 4];
            bool active = !!(cfg & CFG_RXSTATE) != !!(cfg & CFG_RXINV);
            if ((cfg & CFG_RXEV_MASK) == 0 && active) *is |= 1u << b;
        }
        if (*is & c->regs[g->bar][ie_reg(c, g) / 4]) raise = true;
    }
    if (raise && c->routed) apic_send_ipi(apic_id(), APIC_IPI_FIXED | c->vector);
}

static void model_reset(Ctrl *c)
{
    for (int i = 0; i < c->nbar; i++) {
        memset(c->regs[i], 0, MODEL_REGS);
        c->regs[i][PADBAR / 4] = c->padbar[i] = MODEL_PADBAR;
        c->regs[i][0] = 0x00940000;                         /* REVID */
    }
    /* as firmware leaves them: inputs in GPIO mode, pulled up (idle
     * high), interrupting on level; GPP_C0 in its native function (SMBus
     * clock) */
    for (int i = 0; i < c->lay->ngroups; i++) {
        const Group *g = &c->lay->groups[i];
        for (int b = 0; b <= g->limit - g->base; b++) {
            UINT16 pad = (UINT16)(g->base + b - c->padbase[g->bar]);
            UINT32 v = CFG_TXDIS | CFG_RXSTATE;
            if (g->gpiobase == 256 && b == 0) v |= 1u << 10;
            c->regs[g->bar][cfg_reg(c, g->bar, pad) / 4] = v;
        }
    }
}

void GpioModelSetLine(GpioIrq *g, bool asserted)
{
    if (!g || !g->c->model) return;
    Ctrl *c = g->c;
    IrqState s = spin_lock_irqsave(&c->lock);
    UINT32 *cfg = &c->regs[g->grp->bar][cfg_reg(c, g->grp->bar, g->pad) / 4];
    bool high = asserted != g->low;
    bool was = !!(*cfg & CFG_RXSTATE) != !!(*cfg & CFG_RXINV);
    *cfg = high ? *cfg | CFG_RXSTATE : *cfg & ~CFG_RXSTATE;
    bool now = !!(*cfg & CFG_RXSTATE) != !!(*cfg & CFG_RXINV);
    UINT32 ev = *cfg & CFG_RXEV_MASK;
    if ((ev == CFG_RXEV_EDGE && !was && now) || (ev == CFG_RXEV_MASK && was != now))
        c->regs[g->grp->bar][is_reg(c, g->grp) / 4] |= g->bit;
    model_update(c);
    spin_unlock_irqrestore(&c->lock, s);
}

bool GpioIrqIsModel(GpioIrq *g) { return g && g->c->model; }

static Ctrl *find_ctrl(const char *path);
bool GpioControllerIsModel(const char *ctrl) { Ctrl *c = find_ctrl(ctrl); return c && c->model; }

/* -----------------------------------------------------------------------
 * The interrupt
 * ----------------------------------------------------------------------- */
static void gpio_interrupt(void *ctx)
{
    Ctrl *c = ctx;
    GpioIrq *call[MAX_LINES];
    int ncall = 0;
    spin_lock(&c->lock);                 /* (interrupts are off) */
    for (int i = 0; i < c->lay->ngroups; i++) {
        const Group *g = &c->lay->groups[i];
        UINT32 is = rd(c, g->bar, is_reg(c, g));
        if (!is) continue;
        UINT32 ie = rd(c, g->bar, ie_reg(c, g)), pend = is & ie, keep = ie;
        if (!pend) continue;
        for (int b = 0; b < 32; b++) {
            if (!(pend & (1u << b))) continue;
            GpioIrq *l = NULL;
            for (int k = 0; k < MAX_LINES; k++)
                if (c->lines[k].used && c->lines[k].grp == g && c->lines[k].bit == 1u << b) l = &c->lines[k];
            if (!l || l->level) keep &= ~(1u << b);             /* masked until serviced; or nobody's */
            if (!l) c->stray++;
            else if (ncall < MAX_LINES) call[ncall++] = l;
        }
        if (keep != ie) wr(c, g->bar, ie_reg(c, g), keep);
        wr(c, g->bar, is_reg(c, g), pend);
    }
    spin_unlock(&c->lock);
    for (int i = 0; i < ncall; i++) {
        __atomic_fetch_add(&call[i]->count, 1, __ATOMIC_RELAXED);
        call[i]->fn(call[i]->ctx);
    }
}

/* -----------------------------------------------------------------------
 * Pins
 * ----------------------------------------------------------------------- */
static Ctrl *find_ctrl(const char *path)
{
    for (int i = 0; i < g_nctrl; i++) if (strcmp(g_ctrl[i].path, path) == 0) return &g_ctrl[i];
    return NULL;
}

GpioIrq *GpioIrqConnect(const char *ctrl, UINT16 pin, bool level, bool low, bool both,
                        void (*fn)(void *ctx), void *ctx, char *why, int whycap)
{
    Ctrl *c = find_ctrl(ctrl);
    if (!c) { ksnprintf(why, whycap, "no GPIO controller driver for %s", ctrl); return NULL; }
    if (!c->routed) { ksnprintf(why, whycap, "%s has no interrupt routed", ctrl); return NULL; }
    const Group *grp = NULL;
    for (int i = 0; i < c->lay->ngroups && !grp; i++) {
        const Group *g = &c->lay->groups[i];
        if (pin >= g->gpiobase && pin <= g->gpiobase + (g->limit - g->base)) grp = g;
    }
    if (!grp) { ksnprintf(why, whycap, "pin %u is in no pad group of %s", pin, ctrl); return NULL; }
    UINT16 pad = (UINT16)(grp->base + (pin - grp->gpiobase) - c->padbase[grp->bar]);
    UINT32 bit = 1u << (pin - grp->gpiobase);
    GpioIrq *g = NULL;
    IrqState s = spin_lock_irqsave(&c->lock);
    for (int i = 0; i < MAX_LINES; i++) {
        if (c->lines[i].used && c->lines[i].grp == grp && c->lines[i].bit == bit) {
            spin_unlock_irqrestore(&c->lock, s);
            ksnprintf(why, whycap, "pin %u of %s is in use", pin, ctrl);
            return NULL;
        }
        if (!c->lines[i].used && !g) g = &c->lines[i];
    }
    if (!g) {
        spin_unlock_irqrestore(&c->lock, s);
        ksnprintf(why, whycap, "%s: every line in use", ctrl);
        return NULL;
    }
    UINT32 reg = cfg_reg(c, grp->bar, pad), was = rd(c, grp->bar, reg);
    if (was & CFG_PMODE) {
        spin_unlock_irqrestore(&c->lock, s);
        ksnprintf(why, whycap, "pad %s%u of %s is in native function %u, not GPIO", grp->name,
                  pin - grp->gpiobase, ctrl, (was & CFG_PMODE) >> 10);
        return NULL;
    }
    memset(g, 0, sizeof(*g));
    g->c = c; g->grp = grp; g->pin = pin; g->pad = pad; g->bit = bit;
    g->level = level && !both; g->low = low; g->both = both;
    g->fn = fn; g->ctx = ctx;
    UINT32 v = (was & ~(CFG_RXEV_MASK | CFG_RXINV | CFG_ROUTE | CFG_RXDIS)) | CFG_TXDIS;
    if (both) v |= CFG_RXEV_EDGE | CFG_RXEV_ZERO;
    else {
        if (!level) v |= CFG_RXEV_EDGE;
        if (low) v |= CFG_RXINV;
    }
    wr(c, grp->bar, reg, v);
    g->cfg = rd(c, grp->bar, reg);
    g->used = true;
    wr(c, grp->bar, is_reg(c, grp), bit);
    wr(c, grp->bar, ie_reg(c, grp), rd(c, grp->bar, ie_reg(c, grp)) | bit);
    spin_unlock_irqrestore(&c->lock, s);
    ksnprintf(g->name, sizeof(g->name), "%s%u on %s", grp->name, pin - grp->gpiobase, c->path);
    kprintf("[GPIO] pin %u = pad %s: %s, %s; PADCFG0 %08x -> %08x%s\n", pin, g->name,
            both ? "both edges" : level ? "level" : "edge", both ? "" : low ? "active low" : "active high",
            was, g->cfg, g->cfg != v ? " (the firmware locked it: kept)" : "");
    return g;
}

void GpioIrqDisconnect(GpioIrq *g)
{
    if (!g) return;
    Ctrl *c = g->c;
    IrqState s = spin_lock_irqsave(&c->lock);
    wr(c, g->grp->bar, ie_reg(c, g->grp), rd(c, g->grp->bar, ie_reg(c, g->grp)) & ~g->bit);
    wr(c, g->grp->bar, is_reg(c, g->grp), g->bit);
    g->used = false;
    spin_unlock_irqrestore(&c->lock, s);
}

void GpioIrqUnmask(GpioIrq *g)
{
    if (!g) return;
    Ctrl *c = g->c;
    IrqState s = spin_lock_irqsave(&c->lock);
    if (g->level) wr(c, g->grp->bar, is_reg(c, g->grp), g->bit);     /* (set again if still asserted) */
    wr(c, g->grp->bar, ie_reg(c, g->grp), rd(c, g->grp->bar, ie_reg(c, g->grp)) | g->bit);
    spin_unlock_irqrestore(&c->lock, s);
}

const char *GpioIrqName(GpioIrq *g) { return g ? g->name : "?"; }
UINT32      GpioIrqCount(GpioIrq *g) { return g ? __atomic_load_n(&g->count, __ATOMIC_RELAXED) : 0; }

bool GpioIrqAsserted(GpioIrq *g)
{
    if (!g) return false;
    bool high = !!(rd(g->c, g->grp->bar, cfg_reg(g->c, g->grp->bar, g->pad)) & CFG_RXSTATE);
    return high != g->low;
}

void GpioIrqState(GpioIrq *g, UINT32 *padcfg0, bool *enabled, bool *status)
{
    Ctrl *c = g->c;
    *padcfg0 = rd(c, g->grp->bar, cfg_reg(c, g->grp->bar, g->pad));
    *enabled = !!(rd(c, g->grp->bar, ie_reg(c, g->grp)) & g->bit);
    *status = !!(rd(c, g->grp->bar, is_reg(c, g->grp)) & g->bit);
}

/* -----------------------------------------------------------------------
 * Finding the controllers
 * ----------------------------------------------------------------------- */
static void mask_all(Ctrl *c)
{
    for (int i = 0; i < c->lay->ngroups; i++) {
        const Group *g = &c->lay->groups[i];
        wr(c, g->bar, ie_reg(c, g), 0);
        wr(c, g->bar, is_reg(c, g), 0xFFFFFFFF);
    }
}

typedef struct { Ctrl *c; UINT64 addr[MAX_COMM]; UINT32 len[MAX_COMM]; } Scan;

static uacpi_iteration_decision gpio_resource(void *user, uacpi_resource *r)
{
    Scan *sc = user;
    Ctrl *c = sc->c;
    if (r->type == UACPI_RESOURCE_TYPE_FIXED_MEMORY32 && c->nbar < MAX_COMM) {
        sc->addr[c->nbar] = r->fixed_memory32.address;
        sc->len[c->nbar++] = r->fixed_memory32.length;
    } else if (r->type == UACPI_RESOURCE_TYPE_MEMORY32 && c->nbar < MAX_COMM) {
        sc->addr[c->nbar] = r->memory32.minimum;
        sc->len[c->nbar++] = r->memory32.length;
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs && !c->has_irq) {
        c->has_irq = true;
        c->irq = r->extended_irq.irqs[0];
        c->level = r->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        c->low = r->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
    } else if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs && !c->has_irq) {
        c->has_irq = true;
        c->irq = r->irq.irqs[0];
        c->level = r->irq.triggering == UACPI_TRIGGERING_LEVEL;
        c->low = r->irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_gpio(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user; (void)depth;
    if (g_nctrl == MAX_CTRL) return UACPI_ITERATION_DECISION_BREAK;
    Ctrl *c = &g_ctrl[g_nctrl];
    memset(c, 0, sizeof(*c));
    const uacpi_char *path = uacpi_namespace_node_generate_absolute_path(node);
    strncpy(c->path, path ? path : "?", sizeof(c->path) - 1);
    uacpi_free_absolute_path(path);
    for (int i = 0; i < g_nctrl; i++)
        if (strcmp(g_ctrl[i].path, c->path) == 0) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_u32 sta = 0;
    if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK || !(sta & 1)) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(node, &hid) == UACPI_STATUS_OK && hid) {
        strncpy(c->hid, hid->value, sizeof(c->hid) - 1);
        uacpi_free_id_string(hid);
    }
    for (int i = 0; i < N(g_match); i++) if (strcmp(g_match[i].hid, c->hid) == 0) c->lay = g_match[i].lay;
    if (!c->lay) return UACPI_ITERATION_DECISION_CONTINUE;      /* (matched by a _CID: not a layout known) */
    c->model = strcmp(c->hid, "NOVA1055") == 0;
    Scan sc = { c, { 0 }, { 0 } };
    uacpi_for_each_device_resource(node, "_CRS", gpio_resource, &sc);
    int need = 0;
    for (int i = 0; i < c->lay->ngroups; i++) if (c->lay->groups[i].bar + 1 > need) need = c->lay->groups[i].bar + 1;
    if (c->nbar < need) {
        kprintf("[GPIO] %s (%s): %d memory resources, its %s layout has %d communities\n",
                c->path, c->hid, c->nbar, c->lay->what, need);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    for (int i = 0; i < c->nbar; i++) {
        if (c->model) {
            if (!(c->regs[i] = kzalloc(MODEL_REGS))) return UACPI_ITERATION_DECISION_CONTINUE;
            continue;
        }
        c->mmio[i] = PciMapPhysical(sc.addr[i], sc.len[i] ? sc.len[i] : 0x10000);
        if (!c->mmio[i]) {
            kprintf("[GPIO] %s (%s): community %d at %llx not mapped\n", c->path, c->hid, i,
                    (unsigned long long)sc.addr[i]);
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }
    /* the first pad of each community */
    int bar = -1;
    for (int i = 0; i < c->lay->ngroups; i++)
        if (c->lay->groups[i].bar != bar) { bar = c->lay->groups[i].bar; c->padbase[bar] = c->lay->groups[i].base; }
    if (c->model) model_reset(c);
    for (int i = 0; i < c->nbar; i++) c->padbar[i] = rd(c, i, PADBAR);
    mask_all(c);
    c->vector = (UINT8)(IRQ_GPIO + g_nctrl);
    g_nctrl++;
    char irq[48] = "no interrupt resource: drivers poll";
    if (c->has_irq && IoApicPresent()) {
        if (IrqInstall(c->vector, gpio_interrupt, c) &&
            IoApicRouteIrq(c->irq, c->level, c->low, c->vector, &c->gsi)) {
            c->routed = true;
            ksnprintf(irq, sizeof(irq), "IRQ %u (GSI %u)", c->irq, c->gsi);
        } else {
            IrqInstall(c->vector, NULL, NULL);
            ksnprintf(irq, sizeof(irq), "IRQ %u not routed: drivers poll", c->irq);
        }
    } else if (c->has_irq) {
        ksnprintf(irq, sizeof(irq), "IRQ %u, no I/O APIC: drivers poll", c->irq);
    }
    kprintf("[GPIO] %s (%s): %s, %s layout, %d communities, %d pad groups, %s\n", c->path, c->hid,
            c->model ? "the self-tests' GPIO controller (a model, no registers)" : "Intel GPIO controller",
            c->lay->what, c->nbar, c->lay->ngroups, irq);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void GpioProbe(void)
{
    static const uacpi_char *ids[N(g_match) + 1];
    for (int i = 0; i < N(g_match); i++) ids[i] = g_match[i].hid;
    ids[N(g_match)] = NULL;
    uacpi_find_devices_at(uacpi_namespace_root(), ids, found_gpio, NULL);
}

void GpioResume(void)
{
    for (int i = 0; i < g_nctrl; i++) {
        Ctrl *c = &g_ctrl[i];
        IrqState s = spin_lock_irqsave(&c->lock);
        mask_all(c);
        for (int k = 0; k < MAX_LINES; k++) {
            GpioIrq *g = &c->lines[k];
            if (!g->used) continue;
            wr(c, g->grp->bar, cfg_reg(c, g->grp->bar, g->pad), g->cfg);
            wr(c, g->grp->bar, ie_reg(c, g->grp), rd(c, g->grp->bar, ie_reg(c, g->grp)) | g->bit);
        }
        spin_unlock_irqrestore(&c->lock, s);
    }
}

const char *GpioStatus(void)
{
    static char buf[160];
    if (!g_nctrl) return "no Intel GPIO controller found";
    Ctrl *c = &g_ctrl[0];
    int used = 0;
    for (int k = 0; k < MAX_LINES; k++) used += c->lines[k].used;
    ksnprintf(buf, sizeof(buf), "%s (%s), %s layout, %s, %d pin%s connected", c->path, c->hid, c->lay->what,
              c->routed ? "interrupt routed" : "no interrupt", used, used == 1 ? "" : "s");
    return buf;
}
