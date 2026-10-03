/*
 * i2c_dw.c — Intel LPSS I2C controllers (Synopsys DesignWare cores)
 *
 * The I2C controllers in Intel's chipsets since Skylake (the "LPSS" serial
 * I/O functions, PCI devices 00:15.x and 00:19.x on a Raptor Lake laptop)
 * are DesignWare I2C cores behind a small Intel register block at 0x200.
 * The ACPI namespace says which of them a touchpad hangs off, so no ID
 * table is needed: the function is checked for the DesignWare signature.
 *
 * Polled master transfers: the bytes to write and the read commands go
 * into the transmit FIFO (a repeated start before the first read, a stop
 * after the last command), the received bytes come out of the receive
 * FIFO, never more read commands outstanding than it holds.  A missing
 * acknowledge shows up as a transmit abort.
 *
 * Register layout and bring-up (the LPSS reset register, the idle
 * handshake, the SCL counts from the firmware's FMCN/SSCN or from the
 * input clock) follow FreeBSD's ig4 driver (sys/dev/ichiic, BSD licence,
 * from DragonFly BSD); the clock figures there come from Linux's
 * intel-lpss tables.
 */

#include "i2c.h"
#include "../hal/pci.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

#define DW_CON          0x00
#define DW_TAR          0x04
#define DW_DATA_CMD     0x10
#define DW_SS_HCNT      0x14
#define DW_SS_LCNT      0x18
#define DW_FS_HCNT      0x1C
#define DW_FS_LCNT      0x20
#define DW_INTR_MASK    0x30
#define DW_RAW_INTR     0x34
#define DW_RX_TL        0x38
#define DW_TX_TL        0x3C
#define DW_CLR_INTR     0x40
#define DW_CLR_TX_ABRT  0x54
#define DW_CLR_STOP_DET 0x60
#define DW_ENABLE       0x6C
#define DW_STATUS       0x70
#define DW_TXFLR        0x74
#define DW_RXFLR        0x78
#define DW_SDA_HOLD     0x7C
#define DW_ABRT_SOURCE  0x80
#define DW_ENABLE_STAT  0x9C
#define DW_COMP_TYPE    0xFC
#define LPSS_RESETS     0x204           /* Intel: the core's reset (3 = running) */
#define LPSS_DEVIDLE    0x24C           /* Intel: device idle control */

#define CON_MASTER      0x0001
#define CON_STD         0x0002
#define CON_FAST        0x0004
#define CON_RESTART_EN  0x0020
#define CON_SLAVE_OFF   0x0040
#define CMD_READ        0x0100
#define CMD_STOP        0x0200
#define CMD_RESTART     0x0400
#define INTR_TX_ABRT    0x0040
#define INTR_STOP_DET   0x0200
#define DEVIDLE_IDLE    0x0004
#define DEVIDLE_RESTORE 0x0008

#define COMP_TYPE_DW    0x44570140      /* "DW", component type 0x0140 */

#define MAX_DW 4

typedef struct {
    I2cBus          bus;
    PciDevice       pci;
    volatile UINT8 *mmio;
    UINT32          speed;
    UINT16          fs[3], ss[3];       /* SCL high, SCL low, SDA hold */
    int             txd, rxd;           /* FIFO depths */
    char            name[24];
} Dw;

static Dw  g_dw[MAX_DW];
static int g_ndw;

static inline UINT32 rd(Dw *c, UINT32 r)           { return *(volatile UINT32 *)(c->mmio + r); }
static inline void   wr(Dw *c, UINT32 r, UINT32 v) { *(volatile UINT32 *)(c->mmio + r) = v; }

static bool set_enabled(Dw *c, bool on)
{
    wr(c, DW_ENABLE, on ? 1 : 0);
    for (int i = 0; i < 1000; i++) {
        if ((rd(c, DW_ENABLE_STAT) & 1) == (on ? 1u : 0u)) return true;
        udelay(25);
    }
    return false;
}

/* SCL counts for the input clock (MHz) when the firmware gives none:
 * the I2C specification's tHIGH/tLOW plus the fall times, as ig4 and
 * intel-lpss compute them (Tiger Lake and later: 133 MHz) */
static void clock_counts(UINT32 mhz, bool fast, UINT16 out[3])
{
    UINT32 thigh = fast ? 600 : 4000, tlow = fast ? 1300 : 4700;        /* ns */
    UINT32 sda_fall = 171, scl_fall = 208, hold = 42;
    out[0] = (UINT16)((mhz * (thigh + sda_fall) + 500) / 1000 - 3);
    out[1] = (UINT16)((mhz * (tlow + scl_fall) + 500) / 1000 - 1);
    out[2] = (UINT16)((mhz * hold + 500) / 1000);
}

static int fifo_depth(Dw *c, UINT32 reg)
{
    UINT32 v = rd(c, reg);
    wr(c, reg, 0xFF);                   /* the threshold saturates at the depth - 1 */
    int d = (int)(rd(c, reg) & 0xFF) + 1;
    wr(c, reg, v);
    return d < 2 ? 2 : d;
}

static bool setup(Dw *c)
{
    /* D0 (PCI power management), memory decoding, bus mastering */
    UINT8 cap = (UINT8)(PciRead32(c->pci.bus, c->pci.dev, c->pci.func, 0x34) & 0xFC);
    for (int n = 0; cap && n < 16; n++) {
        UINT32 hdr = PciRead32(c->pci.bus, c->pci.dev, c->pci.func, cap);
        if ((hdr & 0xFF) == 0x01) {
            UINT32 pmcsr = PciRead32(c->pci.bus, c->pci.dev, c->pci.func, (UINT8)(cap + 4));
            if (pmcsr & 3) {
                PciWrite32(c->pci.bus, c->pci.dev, c->pci.func, (UINT8)(cap + 4), pmcsr & ~3u);
                udelay(10000);
            }
            break;
        }
        cap = (UINT8)((hdr >> 8) & 0xFC);
    }
    PciEnableDevice(&c->pci);

    /* Out of the idle state and out of reset; the signature says it's up */
    UINT32 idle = rd(c, LPSS_DEVIDLE);
    if (idle & DEVIDLE_RESTORE) {
        wr(c, LPSS_DEVIDLE, DEVIDLE_IDLE | DEVIDLE_RESTORE);
        wr(c, LPSS_DEVIDLE, 0);
        udelay(1000);
    }
    wr(c, LPSS_RESETS, 0);
    wr(c, LPSS_RESETS, 3);
    int i;
    for (i = 0; i < 100 && rd(c, DW_COMP_TYPE) != COMP_TYPE_DW; i++) udelay(10);
    if (rd(c, DW_COMP_TYPE) != COMP_TYPE_DW) return false;

    if (!set_enabled(c, false)) return false;
    rd(c, DW_CLR_INTR);
    wr(c, DW_INTR_MASK, 0);
    wr(c, DW_SS_HCNT, c->ss[0]);
    wr(c, DW_SS_LCNT, c->ss[1]);
    wr(c, DW_FS_HCNT, c->fs[0]);
    wr(c, DW_FS_LCNT, c->fs[1]);
    bool fast = c->speed >= 400000;
    wr(c, DW_SDA_HOLD, fast ? c->fs[2] : c->ss[2]);
    c->txd = fifo_depth(c, DW_TX_TL);
    c->rxd = fifo_depth(c, DW_RX_TL);
    wr(c, DW_RX_TL, 0);
    wr(c, DW_TX_TL, 0);
    wr(c, DW_CON, CON_MASTER | CON_SLAVE_OFF | CON_RESTART_EN | (fast ? CON_FAST : CON_STD));
    return true;
}

static bool dw_xfer(I2cBus *bus, UINT16 addr, const UINT8 *w, int wn, UINT8 *r, int rn)
{
    Dw *c = bus->ctx;
    int total = wn + rn;
    if (total <= 0) return false;
    if (!set_enabled(c, false)) return false;
    wr(c, DW_TAR, addr & 0x7F);
    rd(c, DW_CLR_INTR);
    if (!set_enabled(c, true)) return false;

    int sent = 0, got = 0;
    UINT64 deadline = sched_ticks() + 5 + (UINT64)total / 16;         /* 50 ms and up */
    bool ok = true;
    while (got < rn || sent < total) {
        if (rd(c, DW_RAW_INTR) & INTR_TX_ABRT) { ok = false; break; }
        while (sent < total && (int)rd(c, DW_TXFLR) < c->txd &&
               (sent < wn || sent - wn - got < c->rxd)) {
            UINT32 cmd = sent < wn ? w[sent] : CMD_READ;
            if (sent == wn && wn && rn) cmd |= CMD_RESTART;            /* the read after the write */
            if (sent == total - 1) cmd |= CMD_STOP;
            wr(c, DW_DATA_CMD, cmd);
            sent++;
        }
        while (got < rn && rd(c, DW_RXFLR)) r[got++] = (UINT8)rd(c, DW_DATA_CMD);
        if (sched_ticks() > deadline) { ok = false; break; }
        pause_cpu();
    }
    /* the stop condition (or the abort's) */
    for (int i = 0; i < 2000 && !(rd(c, DW_RAW_INTR) & (INTR_STOP_DET | INTR_TX_ABRT)); i++) udelay(5);
    if (rd(c, DW_RAW_INTR) & INTR_TX_ABRT) {
        ok = false;
        rd(c, DW_ABRT_SOURCE);
        rd(c, DW_CLR_TX_ABRT);
    }
    rd(c, DW_CLR_STOP_DET);
    set_enabled(c, false);
    return ok;
}

I2cBus *DwI2cOpen(UINT8 dev, UINT8 fn, UINT32 speed, const UINT16 *fmcn, const UINT16 *sscn)
{
    for (int i = 0; i < g_ndw; i++)
        if (g_dw[i].pci.dev == dev && g_dw[i].pci.func == fn) return &g_dw[i].bus;
    if (g_ndw == MAX_DW) return NULL;
    PciDevice d;
    bool found = false;
    for (int i = 0; PciAt(i, &d, NULL); i++)
        if (d.bus == 0 && d.dev == dev && d.func == fn) { found = true; break; }
    if (!found || d.vendor != 0x8086) return NULL;

    Dw *c = &g_dw[g_ndw];
    memset(c, 0, sizeof(*c));
    c->pci = d;
    c->mmio = (volatile UINT8 *)PciMapBar(&d, 0);
    if (!c->mmio) return NULL;
    c->speed = speed ? speed : 400000;
    if (fmcn) memcpy(c->fs, fmcn, sizeof(c->fs)); else clock_counts(133, true, c->fs);
    if (sscn) memcpy(c->ss, sscn, sizeof(c->ss)); else clock_counts(133, false, c->ss);
    if (!setup(c)) {
        kprintf("[I2C] 00:%02x.%x (%04x:%04x) is not a DesignWare I2C controller, or did not leave reset\n",
                dev, fn, d.vendor, d.device);
        return NULL;
    }
    ksnprintf(c->name, sizeof(c->name), "I2C 00:%02x.%x", dev, fn);
    c->bus.name = c->name;
    c->bus.xfer = dw_xfer;
    c->bus.ctx = c;
    g_ndw++;
    PciClaim(&d, "I2C (touchpad)");
    kprintf("[I2C] Intel LPSS I2C controller 00:%02x.%x (%04x:%04x): %u kHz, FIFOs %d/%d, timings %s\n",
            dev, fn, d.vendor, d.device, c->speed / 1000, c->txd, c->rxd, fmcn ? "from ACPI" : "computed");
    return &c->bus;
}

void DwI2cResume(void)
{
    for (int i = 0; i < g_ndw; i++)
        if (!setup(&g_dw[i])) kprintf("[I2C] %s did not come back after sleep\n", g_dw[i].name);
}
