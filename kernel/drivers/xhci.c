/*
 * xhci.c — USB 3 (xHCI) host controller driver
 *
 * Follows the eXtensible Host Controller Interface 1.2 specification.
 * One command ring, one event ring (interrupter 0, never enabled: the
 * timer tick polls it), and per device a control ring for endpoint 0 plus
 * one interrupt-IN ring for the HID boot interface we bind.
 *
 * Enumeration (section 4.3): reset the root port, Enable Slot, Address
 * Device with an input context describing endpoint 0, read the device and
 * configuration descriptors, then Configure Endpoint for the interrupt
 * endpoint, SET_CONFIGURATION and SET_PROTOCOL(boot).  A Normal TRB is
 * kept queued on the interrupt endpoint; each completion is one report.
 *
 * Commands and control transfers are only issued by XhciInit (boot, with
 * interrupts off) and afterwards by the "usb" thread that handles
 * hot-plug, so they need no queueing of their own: their completions are
 * picked up by whichever caller drains the event ring next.
 */

#include "xhci.h"
#include "usbhid.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/spinlock.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"

/* Capability registers */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* Operational registers */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(p)    (0x400 + 0x10 * ((p) - 1))

#define CMD_RS          (1u << 0)
#define CMD_HCRST       (1u << 1)
#define STS_HCH         (1u << 0)
#define STS_CNR         (1u << 11)

/* PORTSC */
#define PORT_CCS        (1u << 0)
#define PORT_PED        (1u << 1)
#define PORT_PR         (1u << 4)
#define PORT_PP         (1u << 9)
#define PORT_SPEED(v)   (((v) >> 10) & 0xF)
#define PORT_PRC        (1u << 21)
#define PORT_CHANGES    0x00FE0000u       /* CSC PEC WRC OCC PRC PLC CEC (write 1 to clear) */
#define PORT_PRESERVE   0x0E00C3E0u       /* read-write bits that keep their value when written back */

/* Interrupter 0 (runtime registers + 0x20) */
#define IR_IMAN         0x20
#define IR_ERSTSZ       0x28
#define IR_ERSTBA       0x30
#define IR_ERDP         0x38
#define ERDP_EHB        (1u << 3)

/* TRB types and control-field bits */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_EV_TRANSFER 32
#define TRB_EV_CMD      33
#define TRB_EV_PORT     34

#define TRB_CYCLE       (1u << 0)
#define TRB_TC          (1u << 1)         /* link: toggle cycle */
#define TRB_ISP         (1u << 2)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_DIR_IN      (1u << 16)
#define TRB_TYPE(t)     ((UINT32)(t) << 10)
#define TRB_SLOT(s)     ((UINT32)(s) << 24)

#define CC_SUCCESS      1
#define CC_SHORT_PACKET 13

/* Endpoint types (endpoint context dword 1) */
#define EP_CONTROL      4
#define EP_INTERRUPT_IN 7

/* Port speeds */
#define SPEED_FULL      1
#define SPEED_LOW       2
#define SPEED_HIGH      3
#define SPEED_SUPER     4

#define RING_TRBS       256               /* one page; the last is the link */
#define MAX_PORTS       64
#define MAX_DEVS        16
#define SPIN_LONG       20000000

typedef struct __attribute__((packed)) {
    UINT64 param;
    UINT32 status;
    UINT32 control;
} Trb;

typedef struct {
    volatile Trb *trbs;
    int           enq;
    UINT32        cycle;
} Ring;

typedef struct {
    bool              used;
    UINT8             slot, port, speed;
    UINT8             iface, kind;        /* kind: 1 keyboard, 2 mouse */
    UINT8             dci;                /* the interrupt endpoint's context index */
    UINT16            mps;                /* its max packet size */
    UINT8            *in_ctx, *out_ctx;
    Ring              ep0, intr;
    UINT8            *buf;                /* control transfers */
    UINT8            *report;             /* interrupt transfers */
    volatile bool     ctl_done;
    volatile UINT8    ctl_code;
    volatile UINT32   ctl_residual;
    UsbHidKbd         kbd;
} UsbDev;

static volatile UINT8 *g_cap, *g_op, *g_rt, *g_db;
static int      g_ctx;                    /* context size: 32 or 64 bytes */
static int      g_ports;
static UINT64  *g_dcbaa;
static Ring     g_cmd;
static volatile Trb *g_evt;
static int      g_evt_deq;
static UINT32   g_evt_ccs;
static UsbDev   g_devs[MAX_DEVS];
static UsbDev  *g_by_slot[256];
static volatile bool g_ready;
static volatile UINT32 g_port_changed;    /* the hot-plug thread has work */
static volatile bool   g_port_pending[MAX_PORTS + 1];

static volatile bool   g_cmd_done;
static volatile UINT8  g_cmd_code, g_cmd_slot;
static volatile UINT64 g_cmd_trb;

static KSpinLock g_evt_lock = KSPINLOCK_INIT;

static inline UINT64 phys(const volatile void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }
static inline UINT32 rd32(volatile UINT8 *b, UINT32 r)          { return *(volatile UINT32 *)(b + r); }
static inline void   wr32(volatile UINT8 *b, UINT32 r, UINT32 v) { *(volatile UINT32 *)(b + r) = v; }
static inline void   wr64(volatile UINT8 *b, UINT32 r, UINT64 v)
{
    wr32(b, r, (UINT32)v);
    wr32(b, r + 4, (UINT32)(v >> 32));
}

static void *alloc_zero(int pages)
{
    void *p = kernel_alloc_pages((size_t)pages);
    if (p) memset(p, 0, (size_t)pages * PAGE_SIZE);
    return p;
}

static bool ring_init(Ring *r)
{
    r->trbs = alloc_zero(1);
    r->enq = 0;
    r->cycle = 1;
    return r->trbs != NULL;
}

/* Queue one TRB; the cycle bit goes in last so the controller never sees
 * a half-written TRB.  Returns its physical address. */
static UINT64 ring_push(Ring *r, UINT64 param, UINT32 status, UINT32 control)
{
    volatile Trb *t = &r->trbs[r->enq];
    UINT64 at = phys(t);
    t->param = param;
    t->status = status;
    __asm__ volatile ("" ::: "memory");
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    if (++r->enq == RING_TRBS - 1) {
        volatile Trb *link = &r->trbs[RING_TRBS - 1];
        link->param = phys(r->trbs);
        link->status = 0;
        __asm__ volatile ("" ::: "memory");
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static inline void doorbell(int slot, UINT32 target)
{
    mfence();
    wr32(g_db, (UINT32)slot * 4, target);
}

/* ---- events ---- */

static void queue_report(UsbDev *d)
{
    ring_push(&d->intr, phys(d->report), d->mps, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(d->slot, d->dci);
}

static void on_transfer(const volatile Trb *e)
{
    UINT8  code = (UINT8)(e->status >> 24);
    UINT32 residual = e->status & 0xFFFFFF;
    UINT8  ep = (UINT8)((e->control >> 16) & 0x1F);
    UsbDev *d = g_by_slot[e->control >> 24];
    if (!d || !d->used) return;
    if (ep == 1) {
        d->ctl_code = code;
        d->ctl_residual = residual;
        d->ctl_done = true;
        return;
    }
    if (ep != d->dci) return;
    if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
        /* The endpoint halted; leave it (the device is likely going away) */
        kprintf("[USB] slot %d: interrupt transfer failed (code %d)\n", d->slot, code);
        return;
    }
    int len = (int)d->mps - (int)residual;
    if (d->kind == 1)      UsbHidKeyboardReport(&d->kbd, d->report, len, sched_ticks());
    else if (d->kind == 2) UsbHidMouseReport(d->report, len);
    else                   return;               /* being unplugged */
    queue_report(d);
}

static void on_port(const volatile Trb *e)
{
    int port = (int)(e->param >> 24) & 0xFF;
    if (port < 1 || port > g_ports) return;
    UINT32 sc = rd32(g_op, OP_PORTSC(port));
    /* Reset completions are the enumerator's to see; the rest we clear here */
    wr32(g_op, OP_PORTSC(port), (sc & PORT_PRESERVE) | (sc & PORT_CHANGES & ~PORT_PRC));
    if (sc & (1u << 17)) {                /* connect status change: plugged or unplugged */
        g_port_pending[port] = true;
        g_port_changed = 1;
    }
}

/* Drain the event ring.  Callers hold g_evt_lock. */
static void process_events(void)
{
    bool any = false;
    for (;;) {
        volatile Trb *e = &g_evt[g_evt_deq];
        UINT32 ctl = e->control;
        if ((ctl & TRB_CYCLE) != g_evt_ccs) break;
        __asm__ volatile ("" ::: "memory");
        switch ((ctl >> 10) & 0x3F) {
        case TRB_EV_TRANSFER: on_transfer(e); break;
        case TRB_EV_CMD:
            g_cmd_trb  = e->param;
            g_cmd_code = (UINT8)(e->status >> 24);
            g_cmd_slot = (UINT8)(ctl >> 24);
            g_cmd_done = true;
            break;
        case TRB_EV_PORT: on_port(e); break;
        default: break;
        }
        any = true;
        if (++g_evt_deq == RING_TRBS) { g_evt_deq = 0; g_evt_ccs ^= 1; }
    }
    if (any) wr64(g_rt, IR_ERDP, phys(&g_evt[g_evt_deq]) | ERDP_EHB);
}

static void events_once(void)
{
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    process_events();
    spin_unlock_irqrestore(&g_evt_lock, s);
}

void XhciPoll(void)
{
    if (!g_ready) return;
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    process_events();
    UINT64 now = sched_ticks();
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].kind == 1) UsbHidTick(&g_devs[i].kbd, now);
    spin_unlock_irqrestore(&g_evt_lock, s);
}

/* ---- commands and control transfers ---- */

static UINT8 command(UINT64 param, UINT32 control, UINT8 *slot_out)
{
    g_cmd_done = false;
    UINT64 at = ring_push(&g_cmd, param, 0, control);
    doorbell(0, 0);
    for (int spins = SPIN_LONG; spins > 0; spins--) {
        events_once();
        if (g_cmd_done && g_cmd_trb == at) {
            if (slot_out) *slot_out = g_cmd_slot;
            return g_cmd_code;
        }
        pause_cpu();
    }
    kprintf("[USB] command %d timed out\n", (control >> 10) & 0x3F);
    return 0;
}

/* One control transfer on endpoint 0; IN data lands in d->buf.  Returns
 * the number of bytes moved, or -1. */
static int control(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len)
{
    bool in = (type & 0x80) != 0;
    UINT64 setup = type | ((UINT64)req << 8) | ((UINT64)value << 16) |
                   ((UINT64)index << 32) | ((UINT64)len << 48);
    UINT32 trt = len ? (in ? 3u : 2u) : 0u;
    d->ctl_done = false;
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len)
        ring_push(&d->ep0, phys(d->buf), len, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN));
    doorbell(d->slot, 1);
    for (int spins = SPIN_LONG; spins > 0; spins--) {
        events_once();
        if (d->ctl_done) {
            if (d->ctl_code != CC_SUCCESS && d->ctl_code != CC_SHORT_PACKET) return -1;
            /* The status stage reports no residual; short IN data shows in the descriptor itself */
            return len;
        }
        pause_cpu();
    }
    kprintf("[USB] slot %d: control request %02x timed out\n", d->slot, req);
    return -1;
}

static inline UINT32 *in_slot(UsbDev *d)       { return (UINT32 *)(d->in_ctx + g_ctx); }
static inline UINT32 *in_ep(UsbDev *d, int dci) { return (UINT32 *)(d->in_ctx + (dci + 1) * g_ctx); }

/* ---- enumeration ---- */

static bool port_reset(int port)
{
    UINT32 sc = rd32(g_op, OP_PORTSC(port));
    if (!(sc & PORT_CCS)) return false;
    /* USB 3 ports enable themselves after link training; USB 2 ports need a reset */
    if (!(sc & PORT_PED)) {
        wr32(g_op, OP_PORTSC(port), (sc & PORT_PRESERVE) | PORT_PR);
        for (int spins = SPIN_LONG; spins > 0; spins--) {
            sc = rd32(g_op, OP_PORTSC(port));
            if ((sc & PORT_PRC) && !(sc & PORT_PR)) break;
            pause_cpu();
        }
        wr32(g_op, OP_PORTSC(port), (sc & PORT_PRESERVE) | (sc & PORT_CHANGES));
    }
    sc = rd32(g_op, OP_PORTSC(port));
    return (sc & (PORT_CCS | PORT_PED)) == (PORT_CCS | PORT_PED);
}

static void free_dev(UsbDev *d)
{
    if (d->slot) g_by_slot[d->slot] = NULL;
    d->used = false;
    if (d->slot) {
        command(0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), NULL);
        g_dcbaa[d->slot] = 0;
    }
    /* Its memory is kept for the next device in this entry */
}

/* xHCI interval exponent (2^n x 125 us) for an interrupt endpoint */
static UINT32 ep_interval(UINT8 speed, UINT8 b_interval)
{
    if (speed == SPEED_HIGH || speed == SPEED_SUPER)
        return b_interval ? (UINT32)(b_interval > 16 ? 15 : b_interval - 1) : 0;
    UINT32 frames = b_interval ? b_interval : 1, n = 3;     /* 1 ms frames: 8 x 125 us */
    while (n < 10 && (1u << (n + 1)) <= frames * 8) n++;
    return n;
}

static bool attach(int port)
{
    UsbDev *d = NULL;
    for (int i = 0; i < MAX_DEVS && !d; i++)
        if (!g_devs[i].used) d = &g_devs[i];
    if (!d) return false;
    if (!port_reset(port)) return false;

    UINT8 speed = (UINT8)PORT_SPEED(rd32(g_op, OP_PORTSC(port)));
    UINT8 slot = 0;
    if (command(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot) {
        kprintf("[USB] port %d: no device slot\n", port);
        return false;
    }

    if (!d->in_ctx) {                     /* first use of this entry */
        UINT8 *mem = alloc_zero(4);
        if (!mem || !ring_init(&d->ep0) || !ring_init(&d->intr)) return false;
        d->in_ctx = mem;
        d->out_ctx = mem + PAGE_SIZE;
        d->buf = mem + 2 * PAGE_SIZE;
        d->report = mem + 3 * PAGE_SIZE;
    }
    memset(d->in_ctx, 0, PAGE_SIZE);
    memset(d->out_ctx, 0, PAGE_SIZE);
    memset((void *)d->ep0.trbs, 0, PAGE_SIZE);
    memset((void *)d->intr.trbs, 0, PAGE_SIZE);
    d->ep0.enq = d->intr.enq = 0;
    d->ep0.cycle = d->intr.cycle = 1;
    memset(&d->kbd, 0, sizeof(d->kbd));
    d->slot = slot; d->port = (UINT8)port; d->speed = speed;
    d->kind = 0; d->dci = 0;
    d->used = true;
    g_by_slot[slot] = d;
    g_dcbaa[slot] = phys(d->out_ctx);

    /* Address Device: slot context + endpoint 0 */
    UINT16 mps0 = speed == SPEED_SUPER ? 512 : speed == SPEED_HIGH ? 64 : 8;
    ((UINT32 *)d->in_ctx)[1] = 0x3;                       /* add slot + EP0 */
    in_slot(d)[0] = (1u << 27) | ((UINT32)speed << 20);    /* one context entry */
    in_slot(d)[1] = (UINT32)port << 16;
    UINT32 *ep0 = in_ep(d, 1);
    ep0[1] = (3u << 1) | (EP_CONTROL << 3) | ((UINT32)mps0 << 16);
    *(UINT64 *)&ep0[2] = phys(d->ep0.trbs) | 1;
    ep0[4] = 8;
    if (command(phys(d->in_ctx), TRB_TYPE(TRB_ADDRESS_DEV) | TRB_SLOT(slot), NULL) != CC_SUCCESS) {
        kprintf("[USB] port %d: Address Device failed\n", port);
        goto fail;
    }

    /* Endpoint 0's real packet size (full speed devices use 8 to 64) */
    if (control(d, 0x80, 6, 0x0100, 0, 8) < 0) goto fail;
    if (speed == SPEED_FULL && d->buf[7] && d->buf[7] != mps0) {
        memset(d->in_ctx, 0, (size_t)3 * g_ctx);
        ((UINT32 *)d->in_ctx)[1] = 0x2;                    /* evaluate EP0 */
        ep0 = in_ep(d, 1);
        memset(ep0, 0, (size_t)g_ctx);
        ep0[1] = (3u << 1) | (EP_CONTROL << 3) | ((UINT32)d->buf[7] << 16);
        if (command(phys(d->in_ctx), TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(slot), NULL) != CC_SUCCESS)
            goto fail;
    }
    if (control(d, 0x80, 6, 0x0100, 0, 18) < 0) goto fail;
    UINT16 vid = (UINT16)(d->buf[8] | d->buf[9] << 8), pid = (UINT16)(d->buf[10] | d->buf[11] << 8);

    /* Configuration descriptor: header first, then the whole thing */
    if (control(d, 0x80, 6, 0x0200, 0, 9) < 0) goto fail;
    UINT16 total = (UINT16)(d->buf[2] | d->buf[3] << 8);
    if (total > 2048) total = 2048;
    if (control(d, 0x80, 6, 0x0200, 0, total) < 0) goto fail;
    UINT8 config = d->buf[5];

    /* The first HID boot keyboard or mouse interface and its interrupt-IN endpoint */
    UINT8 cls = 0, sub = 0, proto = 0, iface = 0, ep_addr = 0, interval = 0;
    UINT16 ep_mps = 0;
    for (int off = 0; off + 2 <= total && d->buf[off] >= 2; off += d->buf[off]) {
        const UINT8 *p = &d->buf[off];
        if (p[1] == 4 && p[0] >= 9) {                       /* interface */
            iface = p[2]; cls = p[5]; sub = p[6]; proto = p[7];
        } else if (p[1] == 5 && p[0] >= 7 && cls == 3 && sub == 1 &&
                   (proto == 1 || proto == 2) && (p[2] & 0x80) && (p[3] & 3) == 3) {
            ep_addr = p[2];
            ep_mps = (UINT16)((p[4] | p[5] << 8) & 0x7FF);
            interval = p[6];
            d->kind = proto;
            d->iface = iface;
            break;
        }
    }
    if (!d->kind) {
        kprintf("[USB] port %d: device %04x:%04x is not a boot keyboard or mouse, not used\n",
                port, vid, pid);
        goto fail;
    }
    if (ep_mps > 64) ep_mps = 64;   /* boot reports are 8 bytes at most */
    d->mps = ep_mps;
    d->dci = (UINT8)((ep_addr & 0xF) * 2 + 1);

    /* Configure Endpoint: the interrupt endpoint joins the slot */
    memset(d->in_ctx, 0, (size_t)(d->dci + 2) * g_ctx);
    ((UINT32 *)d->in_ctx)[1] = 1u | (1u << d->dci);
    memcpy(in_slot(d), d->out_ctx, (size_t)g_ctx);
    in_slot(d)[0] = (in_slot(d)[0] & ~(0x1Fu << 27)) | ((UINT32)d->dci << 27);
    in_slot(d)[3] = 0;                                     /* (output-only fields) */
    UINT32 *ep = in_ep(d, d->dci);
    ep[0] = ep_interval(speed, interval) << 16;
    ep[1] = (3u << 1) | (EP_INTERRUPT_IN << 3) | ((UINT32)d->mps << 16);
    *(UINT64 *)&ep[2] = phys(d->intr.trbs) | 1;
    ep[4] = (UINT32)d->mps | ((UINT32)d->mps << 16);         /* average TRB length, max ESIT payload */
    if (command(phys(d->in_ctx), TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(slot), NULL) != CC_SUCCESS) {
        kprintf("[USB] port %d: Configure Endpoint failed\n", port);
        goto fail;
    }
    if (control(d, 0x00, 9, config, 0, 0) < 0) goto fail;            /* SET_CONFIGURATION */
    control(d, 0x21, 0x0B, 0, d->iface, 0);                          /* SET_PROTOCOL(boot) */
    if (d->kind == 1) control(d, 0x21, 0x0A, 0, d->iface, 0);        /* SET_IDLE(0): reports on change only */

    kprintf("[USB] port %d: %s %04x:%04x (%s speed, slot %d)\n", port,
            d->kind == 1 ? "keyboard" : "mouse", vid, pid,
            speed == SPEED_LOW ? "low" : speed == SPEED_FULL ? "full" :
            speed == SPEED_HIGH ? "high" : "super", slot);
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    queue_report(d);
    spin_unlock_irqrestore(&g_evt_lock, s);
    return true;

fail:
    free_dev(d);
    return false;
}

static void detach(int port)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = &g_devs[i];
        if (!d->used || d->port != port) continue;
        kprintf("[USB] port %d: %s unplugged\n", port, d->kind == 1 ? "keyboard" : "mouse");
        IrqState s = spin_lock_irqsave(&g_evt_lock);
        if (d->kind == 1) UsbHidKeyboardGone(&d->kbd);
        d->kind = 0;
        spin_unlock_irqrestore(&g_evt_lock, s);
        free_dev(d);
    }
}

/* Hot-plug: the port-change events only set flags; the (slow, waiting)
 * enumeration runs here */
static void usb_thread(void *arg)
{
    for (;;) {
        sched_sleep_until(NULL, sched_ticks() + 10);
        if (!g_port_changed) continue;
        g_port_changed = 0;
        for (int p = 1; p <= g_ports; p++) {
            if (!g_port_pending[p]) continue;
            g_port_pending[p] = false;
            bool present = false;
            for (int i = 0; i < MAX_DEVS; i++)
                if (g_devs[i].used && g_devs[i].port == p) present = true;
            UINT32 sc = rd32(g_op, OP_PORTSC(p));
            if (present) detach(p);
            if (sc & PORT_CCS) attach(p);
        }
    }
}

/* ---- controller bring-up ---- */

/* Take the controller from the firmware (USB legacy support capability) */
static void bios_handoff(void)
{
    UINT32 off = (rd32(g_cap, CAP_HCCPARAMS1) >> 16) * 4;
    for (int guard = 0; off && guard < 64; guard++) {
        UINT32 v = rd32(g_cap, off);
        if ((v & 0xFF) == 1) {
            wr32(g_cap, off, v | (1u << 24));               /* OS owned */
            for (int spins = SPIN_LONG; spins > 0 && (rd32(g_cap, off) & (1u << 16)); spins--)
                pause_cpu();
            wr32(g_cap, off + 4, rd32(g_cap, off + 4) & ~0x1Fu);   /* no SMIs */
            return;
        }
        UINT32 next = (v >> 8) & 0xFF;
        if (!next) return;
        off += next * 4;
    }
}

static bool wait_sts(UINT32 bits, bool set)
{
    for (int spins = SPIN_LONG; spins > 0; spins--) {
        if (((rd32(g_op, OP_USBSTS) & bits) != 0) == set) return true;
        pause_cpu();
    }
    return false;
}

static bool controller_start(const PciDevice *pci)
{
    g_cap = PciMapBar(pci, 0);
    if (!g_cap) return false;
    PciEnableDevice(pci);
    g_op  = g_cap + (rd32(g_cap, CAP_CAPLENGTH) & 0xFF);
    g_rt  = g_cap + (rd32(g_cap, CAP_RTSOFF) & ~0x1Fu);
    g_db  = g_cap + (rd32(g_cap, CAP_DBOFF) & ~0x3u);
    UINT32 hcs1 = rd32(g_cap, CAP_HCSPARAMS1), hcs2 = rd32(g_cap, CAP_HCSPARAMS2);
    g_ctx = (rd32(g_cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;
    g_ports = (int)(hcs1 >> 24);
    if (g_ports > MAX_PORTS) g_ports = MAX_PORTS;
    int slots = (int)(hcs1 & 0xFF);
    if (slots > 255) slots = 255;

    bios_handoff();
    wr32(g_op, OP_USBCMD, rd32(g_op, OP_USBCMD) & ~CMD_RS);
    if (!wait_sts(STS_HCH, true)) return false;
    wr32(g_op, OP_USBCMD, CMD_HCRST);
    for (int spins = SPIN_LONG; spins > 0 && (rd32(g_op, OP_USBCMD) & CMD_HCRST); spins--)
        pause_cpu();
    if (!wait_sts(STS_CNR, false)) {
        kprintf("[USB] controller did not come out of reset\n");
        return false;
    }

    wr32(g_op, OP_CONFIG, (UINT32)slots);

    /* Device context base array, with the scratchpad array in slot 0 */
    g_dcbaa = alloc_zero(1);
    if (!g_dcbaa) return false;
    int scratch = (int)(((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F));
    if (scratch) {
        UINT64 *arr = alloc_zero(1);
        if (!arr) return false;
        for (int i = 0; i < scratch && i < 512; i++) {
            void *pg = alloc_zero(1);
            if (!pg) return false;
            arr[i] = phys(pg);
        }
        g_dcbaa[0] = phys(arr);
    }
    wr64(g_op, OP_DCBAAP, phys(g_dcbaa));

    if (!ring_init(&g_cmd)) return false;
    wr64(g_op, OP_CRCR, phys(g_cmd.trbs) | 1);

    /* One-segment event ring */
    UINT64 *erst = alloc_zero(1);
    g_evt = alloc_zero(1);
    if (!erst || !g_evt) return false;
    erst[0] = phys(g_evt);
    erst[1] = RING_TRBS;
    g_evt_deq = 0;
    g_evt_ccs = 1;
    wr32(g_rt, IR_ERSTSZ, 1);
    wr64(g_rt, IR_ERDP, phys(g_evt));
    wr64(g_rt, IR_ERSTBA, phys(erst));
    wr32(g_rt, IR_IMAN, 1);                                  /* clear pending; interrupts stay off */

    wr32(g_op, OP_USBCMD, CMD_RS);
    if (!wait_sts(STS_HCH, false)) return false;

    kprintf("[USB] xHCI %02x:%02x.%d: %d ports, %d slots, %d-byte contexts\n",
            pci->bus, pci->dev, pci->func, g_ports, slots, g_ctx);
    return true;
}

int XhciInit(void)
{
    PciDevice pci;
    if (!PciFindClass(0x0C, 0x03, 0x30, 0, &pci)) return 0;
    /* (One controller: the first.  Machines with more put their extra
     * ports on the same class of device; supporting them means moving the
     * globals above into a per-controller structure.) */
    if (!controller_start(&pci)) {
        kprintf("[USB] xHCI controller failed to start\n");
        return 0;
    }

    /* Power the ports and let devices connect */
    for (int p = 1; p <= g_ports; p++) {
        UINT32 sc = rd32(g_op, OP_PORTSC(p));
        if (!(sc & PORT_PP)) wr32(g_op, OP_PORTSC(p), (sc & PORT_PRESERVE) | PORT_PP);
    }
    for (int spins = 2000000; spins > 0; spins--) pause_cpu();

    int bound = 0;
    for (int p = 1; p <= g_ports; p++) {
        g_port_pending[p] = false;
        if (rd32(g_op, OP_PORTSC(p)) & PORT_CCS)
            bound += attach(p);
    }
    events_once();                       /* (connect events for the ports just handled) */
    for (int p = 1; p <= g_ports; p++) g_port_pending[p] = false;
    g_port_changed = 0;
    g_ready = true;
    sched_create_thread("usb", usb_thread, NULL, 8);
    kprintf("[USB] %d HID device(s) ready\n", bound);
    return bound;
}
