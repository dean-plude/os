/*
 * xhci.c — USB 3 (xHCI) host controller driver
 *
 * Follows the eXtensible Host Controller Interface 1.2 specification.
 * Per controller: one command ring, one event ring (interrupter 0, never
 * enabled: the timer tick polls it), and per device a control ring for
 * endpoint 0 plus one ring per endpoint of its active configuration.  Any
 * number of controllers can run side by side; each keeps its state in an
 * Xhci structure.
 *
 * The USB core (usb.c) enumerates; this driver supplies what xHCI does
 * its own way: Enable Slot and Address Device instead of SET_ADDRESS (with
 * a slot context carrying the route string through any hubs), Evaluate
 * Context when endpoint 0's packet size is learnt, one Configure Endpoint
 * for every endpoint of the configuration, and a hub's slot context.
 *
 * Commands are issued by the boot-time enumeration (interrupts off), the
 * "usb" thread that handles hot-plug and, to recover a halted endpoint, by
 * whichever thread was using it; a busy flag keeps them one at a time.
 * Their completions are picked up by whichever caller drains the event
 * ring next.
 */

#include "usb_hc.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/spinlock.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

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
#define PORT_CSC        (1u << 17)
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
#define TRB_RESET_EP    14
#define TRB_SET_DEQ     16
#define TRB_EV_TRANSFER 32
#define TRB_EV_CMD      33
#define TRB_EV_PORT     34

#define TRB_CYCLE       (1u << 0)
#define TRB_TC          (1u << 1)         /* link: toggle cycle */
#define TRB_ISP         (1u << 2)
#define TRB_CHAIN       (1u << 4)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_DIR_IN      (1u << 16)
#define TRB_TYPE(t)     ((UINT32)(t) << 10)
#define TRB_SLOT(s)     ((UINT32)(s) << 24)
#define TRB_EP(e)       ((UINT32)(e) << 16)

#define CC_SUCCESS      1
#define CC_STALL        6
#define CC_SHORT_PACKET 13

/* Endpoint types (endpoint context dword 1) */
#define EP_ISOCH_OUT    1
#define EP_BULK_OUT     2
#define EP_INTR_OUT     3
#define EP_CONTROL      4
#define EP_ISOCH_IN     5
#define EP_BULK_IN      6
#define EP_INTR_IN      7

#define RING_TRBS       256               /* one page; the last is the link */
#define SPIN_LONG       20000000
#define MAX_TD_TRBS     16                /* a bulk transfer: 16 x 64 KiB at most */

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

typedef struct Xhci Xhci;

/* Per endpoint */
typedef struct {
    UINT8             type;               /* EP_* */
    Ring              ring;
    /* the bulk transfer in flight */
    UINT64            trb_at[MAX_TD_TRBS];      /* its TRBs' physical addresses */
    UINT32            trb_off[MAX_TD_TRBS + 1]; /* and where each one's data starts */
    int               ntrb;
    volatile bool     done;
    volatile UINT8    code;
    volatile UINT32   moved;
} XPipe;

/* Per device */
typedef struct {
    UINT8             slot;
    UINT8            *in_ctx, *out_ctx;
    Ring              ep0;
    volatile bool     ctl_done;
    volatile UINT8    ctl_code;
} XDev;

struct Xhci {
    UsbHc            *hc;
    volatile UINT8   *cap, *op, *rt, *db;
    int               ctx;                /* context size: 32 or 64 bytes */
    int               ports, slots;
    UINT64           *dcbaa;
    UINT64           *erst;
    Ring              cmd;
    volatile Trb     *evt;
    int               evt_deq;
    UINT32            evt_ccs;
    UsbDev           *by_slot[256];
    volatile bool     port_changed[USB_MAX_PORTS + 1];
    volatile int      cmd_busy;
    volatile bool     cmd_done;
    volatile UINT8    cmd_code, cmd_slot;
    volatile UINT64   cmd_trb;
    PciDevice         pci;
};

static inline UINT32 rd32(volatile UINT8 *b, UINT32 r)          { return *(volatile UINT32 *)(b + r); }
static inline void   wr32(volatile UINT8 *b, UINT32 r, UINT32 v) { *(volatile UINT32 *)(b + r) = v; }
static inline void   wr64(volatile UINT8 *b, UINT32 r, UINT64 v)
{
    wr32(b, r, (UINT32)v);
    wr32(b, r + 4, (UINT32)(v >> 32));
}
#define phys UsbPhys

static inline Xhci  *X(UsbHc *hc)       { return hc->priv; }
static inline XDev  *XD(UsbDev *d)      { return d->hcd; }
static inline XPipe *XP(UsbPipe *p)     { return p->hcd; }

static bool ring_init(Ring *r)
{
    r->trbs = UsbDmaAlloc(1);
    r->enq = 0;
    r->cycle = 1;
    return r->trbs != NULL;
}

/* Queue one TRB; the cycle bit goes in last so the controller never sees
 * a half-written TRB.  Returns its physical address.  A chained TD may
 * span the link TRB, which then carries the chain bit too. */
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
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CHAIN) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static inline void doorbell(Xhci *x, int slot, UINT32 target)
{
    mfence();
    wr32(x->db, (UINT32)slot * 4, target);
}

/* ---- events ---- */

static void queue_listen(Xhci *x, UsbPipe *p)
{
    ring_push(&XP(p)->ring, phys(p->dma), (UINT32)p->listen_len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(x, XD(p->dev)->slot, p->idx);
}

static void on_transfer(Xhci *x, const volatile Trb *e)
{
    UINT8  code = (UINT8)(e->status >> 24);
    UINT32 residual = e->status & 0xFFFFFF;
    UINT8  ep = (UINT8)((e->control >> 16) & 0x1F);
    UsbDev *d = x->by_slot[e->control >> 24];
    if (!d || d->gone) return;
    if (ep == 1) {
        XD(d)->ctl_code = code;
        XD(d)->ctl_done = true;
        return;
    }
    UsbPipe *p = d->pipes[ep];
    if (!p || p->dead || !p->hcd) return;
    XPipe *xp = XP(p);
    if (p->cb) {                                       /* interrupt IN listener */
        if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
            UsbPipeListenFailed(p, code);
            return;
        }
        if (UsbPipeDeliver(p, p->listen_len - (int)residual)) queue_listen(x, p);
        return;
    }
    /* A bulk transfer someone waits for: which of its TRBs finished (the
     * last, or the one a short packet ended) */
    int idx = -1;
    for (int i = 0; i < xp->ntrb; i++)
        if (xp->trb_at[i] == e->param) { idx = i; break; }
    if (idx < 0) return;                               /* stale */
    UINT32 trb_len = xp->trb_off[idx + 1] - xp->trb_off[idx];
    xp->moved = xp->trb_off[idx] + (trb_len > residual ? trb_len - residual : 0);
    xp->code = code;
    xp->done = true;
}

static void on_port(Xhci *x, const volatile Trb *e)
{
    int port = (int)(e->param >> 24) & 0xFF;
    if (port < 1 || port > x->ports) return;
    UINT32 sc = rd32(x->op, OP_PORTSC(port));
    /* Reset completions are the enumerator's to see; the rest we clear here */
    wr32(x->op, OP_PORTSC(port), (sc & PORT_PRESERVE) | (sc & PORT_CHANGES & ~PORT_PRC));
    if (sc & PORT_CSC) x->port_changed[port] = true;   /* plugged or unplugged */
}

/* Drain the event ring.  Callers hold g_usb_lock. */
static void process_events(Xhci *x)
{
    bool any = false;
    for (;;) {
        volatile Trb *e = &x->evt[x->evt_deq];
        UINT32 ctl = e->control;
        if ((ctl & TRB_CYCLE) != x->evt_ccs) break;
        __asm__ volatile ("" ::: "memory");
        switch ((ctl >> 10) & 0x3F) {
        case TRB_EV_TRANSFER: on_transfer(x, e); break;
        case TRB_EV_CMD:
            x->cmd_trb  = e->param;
            x->cmd_code = (UINT8)(e->status >> 24);
            x->cmd_slot = (UINT8)(ctl >> 24);
            x->cmd_done = true;
            break;
        case TRB_EV_PORT: on_port(x, e); break;
        default: break;
        }
        any = true;
        if (++x->evt_deq == RING_TRBS) { x->evt_deq = 0; x->evt_ccs ^= 1; }
    }
    if (any) wr64(x->rt, IR_ERDP, phys(&x->evt[x->evt_deq]) | ERDP_EHB);
}

static void events_once(Xhci *x)
{
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    process_events(x);
    spin_unlock_irqrestore(&g_usb_lock, s);
}

static void xhci_poll(UsbHc *hc)
{
    process_events(X(hc));
}

/* ---- commands and control transfers ---- */

static UINT8 command(Xhci *x, UINT64 param, UINT32 control, UINT8 *slot_out)
{
    UsbFlagTake(&x->cmd_busy);
    x->cmd_done = false;
    UINT64 at = ring_push(&x->cmd, param, 0, control);
    doorbell(x, 0, 0);
    UINT8 code = 0;
    int spins;
    for (spins = SPIN_LONG; spins > 0; spins--) {
        events_once(x);
        if (x->cmd_done && x->cmd_trb == at) {
            if (slot_out) *slot_out = x->cmd_slot;
            code = x->cmd_code;
            break;
        }
        pause_cpu();
    }
    UsbFlagDrop(&x->cmd_busy);
    if (!spins) kprintf("[USB] command %d timed out\n", (control >> 10) & 0x3F);
    return code;
}

static int xhci_control(UsbHc *hc, UsbDev *d, const UsbSetup *s, bool *stalled)
{
    Xhci *x = X(hc);
    XDev *xd = XD(d);
    if (d->gone || !xd || !xd->slot) return -1;
    UINT16 len = s->len;
    bool in = (s->type & 0x80) != 0;
    UINT64 setup = s->type | ((UINT64)s->req << 8) | ((UINT64)s->value << 16) |
                   ((UINT64)s->index << 32) | ((UINT64)len << 48);
    UINT32 trt = len ? (in ? 3u : 2u) : 0u;
    xd->ctl_done = false;
    ring_push(&xd->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len)
        ring_push(&xd->ep0, phys(d->buf), len, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    ring_push(&xd->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN));
    doorbell(x, xd->slot, 1);
    for (int spins = SPIN_LONG; spins > 0; spins--) {
        events_once(x);
        if (xd->ctl_done) {
            if (xd->ctl_code == CC_STALL) {
                /* A stall on endpoint 0 is a request the device refused; the
                 * endpoint must be reset before the next one */
                *stalled = true;
                command(x, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(xd->slot) | TRB_EP(1), NULL);
                UINT64 deq = phys(&xd->ep0.trbs[xd->ep0.enq]) | xd->ep0.cycle;
                command(x, deq, TRB_TYPE(TRB_SET_DEQ) | TRB_SLOT(xd->slot) | TRB_EP(1), NULL);
                return -1;
            }
            if (xd->ctl_code != CC_SUCCESS && xd->ctl_code != CC_SHORT_PACKET) return -1;
            /* The status stage reports no residual; short IN data shows in the descriptor itself */
            return len;
        }
        if (d->gone) return -1;
        pause_cpu();
    }
    kprintf("[USB] %s: control request %02x timed out\n", d->name, s->req);
    return -1;
}

static inline UINT32 *in_slot(Xhci *x, XDev *xd)        { return (UINT32 *)(xd->in_ctx + x->ctx); }
static inline UINT32 *in_ep(Xhci *x, XDev *xd, int dci) { return (UINT32 *)(xd->in_ctx + (dci + 1) * x->ctx); }

/* ---- pipes ---- */

/* xHCI interval exponent (2^n x 125 us) for an interrupt endpoint */
static UINT32 ep_interval(UINT8 speed, UINT8 b_interval)
{
    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER)
        return b_interval ? (UINT32)(b_interval > 16 ? 15 : b_interval - 1) : 0;
    UINT32 frames = b_interval ? b_interval : 1, n = 3;     /* 1 ms frames: 8 x 125 us */
    while (n < 10 && (1u << (n + 1)) <= frames * 8) n++;
    return n;
}

static bool xhci_pipe_add(UsbHc *hc, UsbPipe *p)
{
    (void)hc;
    XPipe *xp = kzalloc(sizeof(XPipe));
    if (!xp) return false;
    if (!ring_init(&xp->ring)) { kfree(xp); return false; }
    xp->type = p->xfer == 2 ? (p->in ? EP_BULK_IN : EP_BULK_OUT) : (p->in ? EP_INTR_IN : EP_INTR_OUT);
    p->hcd = xp;
    return true;
}

static bool xhci_listen(UsbHc *hc, UsbPipe *p)
{
    if (!p->hcd || !XD(p->dev)->slot) return false;
    queue_listen(X(hc), p);
    return true;
}

static int xhci_bulk(UsbHc *hc, UsbPipe *p, UINT32 len, UINT32 timeout_ms, bool *stalled)
{
    Xhci *x = X(hc);
    UsbDev *d = p->dev;
    XPipe *xp = XP(p);
    if (!xp || !XD(d)->slot) return -1;

    /* One TD: a TRB per 64 KiB boundary crossed */
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    xp->done = false;
    UINT32 off = 0;
    int n = 0;
    do {
        UINT64 at = phys(p->dma) + off;
        UINT32 chunk = 0x10000u - (UINT32)(at & 0xFFFF);
        if (chunk > len - off) chunk = len - off;
        bool last = off + chunk >= len;
        UINT32 left = (UINT32)(MAX_TD_TRBS - n - 1);      /* TD size: TRBs still to come (max 31) */
        xp->trb_off[n] = off;
        UINT32 ctl = TRB_TYPE(TRB_NORMAL) | TRB_ISP | (last ? TRB_IOC : TRB_CHAIN);
        xp->trb_at[n] = ring_push(&xp->ring, at, chunk | ((last ? 0 : (left > 31 ? 31 : left)) << 17), ctl);
        off += chunk;
        n++;
    } while (off < len && n < MAX_TD_TRBS);
    xp->ntrb = n;
    xp->trb_off[n] = len;
    doorbell(x, XD(d)->slot, p->idx);
    spin_unlock_irqrestore(&g_usb_lock, s);

    UINT64 deadline = rdtsc() + g_tsc_per_tick * (UINT64)timeout_ms / 10;
    for (;;) {
        events_once(x);
        if (xp->done) {
            if (xp->code == CC_SUCCESS || xp->code == CC_SHORT_PACKET) return (int)xp->moved;
            if (xp->code == CC_STALL) *stalled = true;
            return -1;
        }
        if (d->gone || rdtsc() > deadline) {
            if (!d->gone) kprintf("[USB] %s: bulk transfer timed out\n", d->name);
            return -1;
        }
        pause_cpu();
    }
}

static bool xhci_pipe_reset(UsbHc *hc, UsbPipe *p)
{
    Xhci *x = X(hc);
    XDev *xd = XD(p->dev);
    XPipe *xp = XP(p);
    if (!xp || !xd->slot) return false;
    if (command(x, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(xd->slot) | TRB_EP(p->idx), NULL) != CC_SUCCESS)
        return false;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    UINT64 deq = phys(&xp->ring.trbs[xp->ring.enq]) | xp->ring.cycle;
    spin_unlock_irqrestore(&g_usb_lock, s);
    command(x, deq, TRB_TYPE(TRB_SET_DEQ) | TRB_SLOT(xd->slot) | TRB_EP(p->idx), NULL);
    return true;
}

/* ---- devices ---- */

static void xhci_dev_remove(UsbHc *hc, UsbDev *d)
{
    Xhci *x = X(hc);
    XDev *xd = XD(d);
    if (!xd) return;
    if (xd->slot) {
        IrqState s = spin_lock_irqsave(&g_usb_lock);
        x->by_slot[xd->slot] = NULL;
        spin_unlock_irqrestore(&g_usb_lock, s);
        command(x, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(xd->slot), NULL);
        x->dcbaa[xd->slot] = 0;
    }
    for (int i = 0; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (!p || !p->hcd) continue;
        XPipe *xp = XP(p);
        p->hcd = NULL;
        UsbDmaFree((void *)xp->ring.trbs, 1);
        kfree(xp);
    }
    UsbDmaFree((void *)xd->ep0.trbs, 1);
    UsbDmaFree(xd->in_ctx, 2);
    d->hcd = NULL;
    kfree(xd);
}

static bool xhci_dev_address(UsbHc *hc, UsbDev *d)
{
    Xhci *x = X(hc);
    XDev *xd = kzalloc(sizeof(XDev));
    if (!xd) return false;
    UINT8 *mem = UsbDmaAlloc(2);
    if (!mem || !ring_init(&xd->ep0)) {
        UsbDmaFree(mem, 2);
        kfree(xd);
        return false;
    }
    xd->in_ctx = mem;
    xd->out_ctx = mem + PAGE_SIZE;
    d->hcd = xd;

    UINT8 slot = 0;
    if (command(x, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot) {
        kprintf("[USB] %s: no device slot\n", d->name);
        return false;
    }
    xd->slot = slot;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    x->by_slot[slot] = d;
    spin_unlock_irqrestore(&g_usb_lock, s);
    x->dcbaa[slot] = phys(xd->out_ctx);

    /* The hub whose transaction translator serves a low/full speed device */
    UINT8 tt_slot = d->tt_hub && XD(d->tt_hub) ? XD(d->tt_hub)->slot : 0;
    bool mtt = d->tt_hub && d->tt_hub->mtt;

    /* Address Device: slot context + endpoint 0 */
    ((UINT32 *)xd->in_ctx)[1] = 0x3;                      /* add slot + EP0 */
    in_slot(x, xd)[0] = (1u << 27) | ((UINT32)d->speed << 20) | (d->route & 0xFFFFF) | (tt_slot && mtt ? 1u << 25 : 0);
    in_slot(x, xd)[1] = (UINT32)d->root_port << 16;
    in_slot(x, xd)[2] = (UINT32)tt_slot | ((UINT32)(tt_slot ? d->tt_port : 0) << 8);
    UINT32 *ep0 = in_ep(x, xd, 1);
    ep0[1] = (3u << 1) | (EP_CONTROL << 3) | ((UINT32)d->ep0_mps << 16);
    *(UINT64 *)&ep0[2] = phys(xd->ep0.trbs) | 1;
    ep0[4] = 8;
    if (command(x, phys(xd->in_ctx), TRB_TYPE(TRB_ADDRESS_DEV) | TRB_SLOT(slot), NULL) != CC_SUCCESS) {
        kprintf("[USB] %s: Address Device failed\n", d->name);
        return false;
    }
    d->addr = (UINT8)(((UINT32 *)xd->out_ctx)[3] & 0xFF);    /* (the address the controller chose) */
    return true;
}

/* Endpoint 0's real packet size (full speed devices use 8 to 64) */
static bool xhci_dev_ep0(UsbHc *hc, UsbDev *d)
{
    Xhci *x = X(hc);
    XDev *xd = XD(d);
    memset(xd->in_ctx, 0, (size_t)3 * x->ctx);
    ((UINT32 *)xd->in_ctx)[1] = 0x2;                       /* evaluate EP0 */
    UINT32 *ep0 = in_ep(x, xd, 1);
    memset(ep0, 0, (size_t)x->ctx);
    ep0[1] = (3u << 1) | (EP_CONTROL << 3) | ((UINT32)d->ep0_mps << 16);
    return command(x, phys(xd->in_ctx), TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(xd->slot), NULL) == CC_SUCCESS;
}

/* One Configure Endpoint for every pipe of the configuration */
static bool xhci_dev_config(UsbHc *hc, UsbDev *d)
{
    Xhci *x = X(hc);
    XDev *xd = XD(d);
    UINT32 add = 0;
    int max_dci = 1;
    for (int dci = 2; dci < 32; dci++)
        if (d->pipes[dci] && d->pipes[dci]->hcd) { add |= 1u << dci; max_dci = dci; }
    if (!add) return true;
    memset(xd->in_ctx, 0, PAGE_SIZE);
    ((UINT32 *)xd->in_ctx)[1] = 1u | add;
    memcpy(in_slot(x, xd), xd->out_ctx, (size_t)x->ctx);
    in_slot(x, xd)[0] = (in_slot(x, xd)[0] & ~(0x1Fu << 27)) | ((UINT32)max_dci << 27);
    in_slot(x, xd)[3] = 0;                                 /* (output-only fields) */
    for (int dci = 2; dci <= max_dci; dci++) {
        if (!(add & (1u << dci))) continue;
        UsbPipe *p = d->pipes[dci];
        XPipe *xp = XP(p);
        UINT32 *ep = in_ep(x, xd, dci);
        bool intr = p->xfer == 3;
        ep[0] = intr ? ep_interval(d->speed, p->interval) << 16 : 0;
        ep[1] = (3u << 1) | ((UINT32)xp->type << 3) | ((UINT32)p->mps << 16);
        *(UINT64 *)&ep[2] = phys(xp->ring.trbs) | 1;
        ep[4] = intr ? ((UINT32)p->mps | ((UINT32)p->mps << 16)) : 3072;
    }
    return command(x, phys(xd->in_ctx), TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(xd->slot), NULL) == CC_SUCCESS;
}

static bool xhci_dev_hub(UsbHc *hc, UsbDev *d, UINT8 ports, bool mtt, UINT8 think_time)
{
    Xhci *x = X(hc);
    XDev *xd = XD(d);
    memset(xd->in_ctx, 0, PAGE_SIZE);
    ((UINT32 *)xd->in_ctx)[1] = 1u;                        /* the slot context only */
    memcpy(in_slot(x, xd), xd->out_ctx, (size_t)x->ctx);
    UINT32 *s = in_slot(x, xd);
    s[0] |= 1u << 26;                                      /* Hub */
    if (mtt && d->speed == USB_SPEED_HIGH) s[0] |= 1u << 25;
    s[1] = (s[1] & 0x00FFFFFFu) | ((UINT32)ports << 24);
    if (d->speed == USB_SPEED_HIGH) s[2] = (s[2] & ~(3u << 16)) | ((UINT32)(think_time & 3) << 16);
    s[3] = 0;
    return command(x, phys(xd->in_ctx), TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(xd->slot), NULL) == CC_SUCCESS;
}

/* ---- root ports ---- */

static UINT32 xhci_port_status(UsbHc *hc, int port)
{
    Xhci *x = X(hc);
    UINT32 st = (rd32(x->op, OP_PORTSC(port)) & PORT_CCS) ? USB_PORT_CONNECTED : 0;
    if (x->port_changed[port]) {
        x->port_changed[port] = false;
        st |= USB_PORT_CHANGED;
    }
    return st;
}

static UINT8 xhci_port_reset(UsbHc *hc, int port)
{
    Xhci *x = X(hc);
    UINT32 sc = rd32(x->op, OP_PORTSC(port));
    if (!(sc & PORT_CCS)) return 0;
    /* USB 3 ports enable themselves after link training; USB 2 ports need a reset */
    if (!(sc & PORT_PED)) {
        wr32(x->op, OP_PORTSC(port), (sc & PORT_PRESERVE) | PORT_PR);
        for (int spins = SPIN_LONG; spins > 0; spins--) {
            sc = rd32(x->op, OP_PORTSC(port));
            if ((sc & PORT_PRC) && !(sc & PORT_PR)) break;
            pause_cpu();
        }
        wr32(x->op, OP_PORTSC(port), (sc & PORT_PRESERVE) | (sc & PORT_CHANGES));
    }
    sc = rd32(x->op, OP_PORTSC(port));
    if ((sc & (PORT_CCS | PORT_PED)) != (PORT_CCS | PORT_PED)) return 0;
    return (UINT8)PORT_SPEED(sc);
}

/* ---- controller bring-up ---- */

/* Take the controller from the firmware (USB legacy support capability) */
static void bios_handoff(Xhci *x)
{
    UINT32 off = (rd32(x->cap, CAP_HCCPARAMS1) >> 16) * 4;
    for (int guard = 0; off && guard < 64; guard++) {
        UINT32 v = rd32(x->cap, off);
        if ((v & 0xFF) == 1) {
            wr32(x->cap, off, v | (1u << 24));               /* OS owned */
            for (int spins = SPIN_LONG; spins > 0 && (rd32(x->cap, off) & (1u << 16)); spins--)
                pause_cpu();
            wr32(x->cap, off + 4, rd32(x->cap, off + 4) & ~0x1Fu);   /* no SMIs */
            return;
        }
        UINT32 next = (v >> 8) & 0xFF;
        if (!next) return;
        off += next * 4;
    }
}

static bool wait_sts(Xhci *x, UINT32 bits, bool set)
{
    for (int spins = SPIN_LONG; spins > 0; spins--) {
        if (((rd32(x->op, OP_USBSTS) & bits) != 0) == set) return true;
        pause_cpu();
    }
    return false;
}

/* Halt, reset and program the controller: device contexts, the command
 * ring and the event ring (allocated already), then run.  Boot and wake. */
static bool controller_program(Xhci *x)
{
    bios_handoff(x);
    wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~CMD_RS);
    if (!wait_sts(x, STS_HCH, true)) return false;
    wr32(x->op, OP_USBCMD, CMD_HCRST);
    for (int spins = SPIN_LONG; spins > 0 && (rd32(x->op, OP_USBCMD) & CMD_HCRST); spins--)
        pause_cpu();
    if (!wait_sts(x, STS_CNR, false)) {
        kprintf("[USB] xHCI controller did not come out of reset\n");
        return false;
    }

    wr32(x->op, OP_CONFIG, (UINT32)x->slots);
    wr64(x->op, OP_DCBAAP, phys(x->dcbaa));

    memset((void *)x->cmd.trbs, 0, PAGE_SIZE);
    x->cmd.enq = 0;
    x->cmd.cycle = 1;
    wr64(x->op, OP_CRCR, phys(x->cmd.trbs) | 1);

    memset((void *)x->evt, 0, PAGE_SIZE);
    x->evt_deq = 0;
    x->evt_ccs = 1;
    wr32(x->rt, IR_ERSTSZ, 1);
    wr64(x->rt, IR_ERDP, phys(x->evt));
    wr64(x->rt, IR_ERSTBA, phys(x->erst));
    wr32(x->rt, IR_IMAN, 1);                                  /* clear pending; interrupts stay off */

    wr32(x->op, OP_USBCMD, CMD_RS);
    return wait_sts(x, STS_HCH, false);
}

static void power_ports(Xhci *x)
{
    for (int p = 1; p <= x->ports; p++) {
        UINT32 sc = rd32(x->op, OP_PORTSC(p));
        if (!(sc & PORT_PP)) wr32(x->op, OP_PORTSC(p), (sc & PORT_PRESERVE) | PORT_PP);
    }
}

/* Intel 7- to 9-series chipsets wire their USB 2 ports to EHCI until the
 * xHCI controller asks for them (and its USB 3 pins stay off): take every
 * port the firmware allows, as Windows does. */
static void intel_take_ports(const PciDevice *pci)
{
    static const UINT16 ids[] = { 0x1E31, 0x8C31, 0x9C31, 0x8CB1, 0x9CB1, 0x8D31 };
    if (pci->vendor != 0x8086) return;
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (pci->device != ids[i]) continue;
        PciWrite32(pci->bus, pci->dev, pci->func, 0xD8, PciRead32(pci->bus, pci->dev, pci->func, 0xDC));  /* USB3_PSSEN */
        PciWrite32(pci->bus, pci->dev, pci->func, 0xD0, PciRead32(pci->bus, pci->dev, pci->func, 0xD4));  /* XUSB2PR */
        kprintf("[USB] xHCI: took the chipset's USB 2 ports over from EHCI\n");
        return;
    }
}

static bool xhci_resume(UsbHc *hc)
{
    Xhci *x = X(hc);
    /* The slots went with the reset below: no Disable Slot for the old devices */
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int i = 0; i < 256; i++) {
        UsbDev *d = x->by_slot[i];
        if (d && XD(d)) XD(d)->slot = 0;
        x->by_slot[i] = NULL;
        if (i) x->dcbaa[i] = 0;
    }
    spin_unlock_irqrestore(&g_usb_lock, s);
    intel_take_ports(&x->pci);
    if (!controller_program(x)) return false;
    power_ports(x);
    return true;
}

/* Before S3: the root ports may wake the machine (a device connecting or
 * leaving, over-current, a suspended device's remote wakeup), the ports
 * with a device are suspended (U3) and the controller signals PME# */
static void xhci_prepare_sleep(UsbHc *hc)
{
    Xhci *x = X(hc);
    const PciDevice *pci = &x->pci;
    for (int p = 1; p <= x->ports; p++) {
        UINT32 sc = rd32(x->op, OP_PORTSC(p));
        UINT32 w = (sc & PORT_PRESERVE) | (7u << 25);                    /* WCE WDE WOE */
        wr32(x->op, OP_PORTSC(p), w);
        if ((sc & PORT_CCS) && (sc & PORT_PED))
            wr32(x->op, OP_PORTSC(p), (w & ~(0xFu << 5)) | (3u << 5) | (1u << 16));   /* PLS U3, LWS */
    }
    for (UINT8 cap = (UINT8)PciRead32(pci->bus, pci->dev, pci->func, 0x34) & 0xFC, n = 0; cap && n < 48; n++) {
        UINT32 hdr = PciRead32(pci->bus, pci->dev, pci->func, cap);
        if ((hdr & 0xFF) == 1) {                                        /* power management: PME_En, clear PME_Status */
            UINT32 csr = PciRead32(pci->bus, pci->dev, pci->func, cap + 4);
            PciWrite32(pci->bus, pci->dev, pci->func, cap + 4, (csr & ~3u) | (1u << 8) | (1u << 15));
            break;
        }
        cap = (UINT8)(hdr >> 8) & 0xFC;
    }
}

static const UsbHcOps g_xhci_ops = {
    .kind        = "xHCI",
    .port_status = xhci_port_status,
    .port_reset  = xhci_port_reset,
    .dev_address = xhci_dev_address,
    .dev_ep0     = xhci_dev_ep0,
    .pipe_add    = xhci_pipe_add,
    .dev_config  = xhci_dev_config,
    .dev_hub     = xhci_dev_hub,
    .dev_remove  = xhci_dev_remove,
    .control     = xhci_control,
    .bulk        = xhci_bulk,
    .listen      = xhci_listen,
    .pipe_reset  = xhci_pipe_reset,
    .poll        = xhci_poll,
    .prepare_sleep = xhci_prepare_sleep,
    .resume      = xhci_resume,
};

bool XhciProbe(const PciDevice *pci)
{
    Xhci *x = kzalloc(sizeof(Xhci));
    if (!x) return false;
    x->pci = *pci;
    x->cap = PciMapBar(pci, 0);
    if (!x->cap) { kfree(x); return false; }
    PciEnableDevice(pci);
    intel_take_ports(pci);
    x->op  = x->cap + (rd32(x->cap, CAP_CAPLENGTH) & 0xFF);
    x->rt  = x->cap + (rd32(x->cap, CAP_RTSOFF) & ~0x1Fu);
    x->db  = x->cap + (rd32(x->cap, CAP_DBOFF) & ~0x3u);
    UINT32 hcs1 = rd32(x->cap, CAP_HCSPARAMS1), hcs2 = rd32(x->cap, CAP_HCSPARAMS2);
    x->ctx = (rd32(x->cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;
    x->ports = (int)(hcs1 >> 24);
    if (x->ports > USB_MAX_PORTS) x->ports = USB_MAX_PORTS;
    x->slots = (int)(hcs1 & 0xFF);

    /* Device context base array, with the scratchpad array in slot 0 */
    x->dcbaa = UsbDmaAlloc(1);
    if (!x->dcbaa) goto fail;
    int scratch = (int)(((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F));
    if (scratch) {
        UINT64 *arr = UsbDmaAlloc(1);
        if (!arr) goto fail;
        for (int i = 0; i < scratch && i < 512; i++) {
            void *pg = UsbDmaAlloc(1);
            if (!pg) goto fail;
            arr[i] = phys(pg);
        }
        x->dcbaa[0] = phys(arr);
    }
    if (!ring_init(&x->cmd)) goto fail;

    /* One-segment event ring */
    x->erst = UsbDmaAlloc(1);
    x->evt = UsbDmaAlloc(1);
    if (!x->erst || !x->evt) goto fail;
    x->erst[0] = phys(x->evt);
    x->erst[1] = RING_TRBS;

    if (!controller_program(x)) goto fail;
    power_ports(x);
    x->hc = UsbAddController(&g_xhci_ops, x, x->ports);
    if (!x->hc) goto fail;
    kprintf("[USB] bus %d: xHCI %02x:%02x.%d, %d ports, %d slots, %d-byte contexts\n",
            x->hc->num, pci->bus, pci->dev, pci->func, x->ports, x->slots, x->ctx);
    return true;

fail:
    kprintf("[USB] xHCI controller %02x:%02x.%d failed to start\n", pci->bus, pci->dev, pci->func);
    return false;              /* (its memory stays: the controller may still point at it) */
}
