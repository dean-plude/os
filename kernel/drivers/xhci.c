/*
 * xhci.c — USB 3 (xHCI) host controller driver and USB core
 *
 * Follows the eXtensible Host Controller Interface 1.2 specification.
 * One command ring, one event ring (interrupter 0, never enabled: the
 * timer tick polls it), and per device a control ring for endpoint 0 plus
 * one ring per endpoint of its active configuration.
 *
 * Enumeration (section 4.3): reset the port (a root port here, a hub's
 * port in usbhub.c), Enable Slot, Address Device with a slot context that
 * carries the route string through any hubs, read the device and
 * configuration descriptors, then one Configure Endpoint for every
 * endpoint of the configuration, SET_CONFIGURATION, and offer each
 * interface to the class drivers (usb.h).
 *
 * Commands are issued by XhciInit (boot, interrupts off), the "usb" thread
 * that handles hot-plug and, to recover a halted endpoint, by whichever
 * thread was using it; a busy flag keeps them one at a time.  Their
 * completions are picked up by whichever caller drains the event ring
 * next.
 */

#include "xhci.h"
#include "usb.h"
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
#define MAX_PORTS       64
#define MAX_DEVS        64
#define MAX_BINDS       4
#define CTL_BUF         4096
#define SPIN_LONG       20000000
#define MAX_TD_TRBS     16                /* a bulk transfer: 16 x 64 KiB at most */
#define MAX_PIPE_BUF    (MAX_TD_TRBS - 1) * 0x10000u

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

struct UsbPipe {
    UsbDev           *dev;
    UINT8             dci, type, addr;
    UINT16            mps;
    UINT8             interval;           /* the endpoint descriptor's bInterval */
    Ring              ring;
    UINT8            *dma;                /* bounce buffer */
    UINT32            dma_size;
    volatile int      busy;               /* a bulk transfer is in progress */
    /* the transfer in flight (bulk) */
    UINT64            trb_at[MAX_TD_TRBS];      /* its TRBs' physical addresses */
    UINT32            trb_off[MAX_TD_TRBS + 1]; /* and where each one's data starts */
    int               ntrb;
    volatile bool     done;
    volatile UINT8    code;
    volatile UINT32   moved;
    /* interrupt IN listening */
    UsbInCallback     cb;
    void             *cb_ctx;
    int               listen_len;
    bool              dead;               /* the device left: never touched again */
};

struct UsbDev {
    bool              used;
    volatile bool     gone;
    UINT8             slot, root_port, speed, depth;
    UINT32            route;
    UsbDev           *parent;             /* hub, or NULL on a root port */
    UINT8             parent_port;
    UINT8             tt_slot, tt_port;
    bool              mtt;
    bool              is_hub;
    UINT16            vid, pid;
    char              name[24];
    UINT8            *in_ctx, *out_ctx;
    UINT8            *buf;                /* control transfers (CTL_BUF bytes) */
    Ring              ep0;
    volatile int      ep0_busy;
    volatile bool     ctl_done;
    volatile UINT8    ctl_code;
    volatile int      users;              /* transfers in progress */
    UsbPipe          *pipes[32];          /* by DCI */
    struct { void *inst; void (*gone)(void *inst); } bind[MAX_BINDS];
    int               nbind;
};

static volatile UINT8 *g_cap, *g_op, *g_rt, *g_db;
static int      g_ctx;                    /* context size: 32 or 64 bytes */
static int      g_ports;
static UINT64  *g_dcbaa;
static Ring     g_cmd;
static volatile Trb *g_evt;
static int      g_evt_deq;
static UINT32   g_evt_ccs;
static UsbDev  *g_devs[MAX_DEVS];
static UsbDev  *g_by_slot[256];
static volatile bool g_ready;
static volatile UINT32 g_port_changed;    /* the usb thread has work */
static volatile bool   g_port_pending[MAX_PORTS + 1];
static volatile bool   g_hubs_pending;
static volatile bool   g_thread_up;
static volatile bool   g_forget_all;     /* woke from S3: drop every device */
static volatile int    g_work;           /* the usb thread is enumerating or removing */
static volatile bool   g_sleeping;       /* between XhciPrepareSleep and XhciResume */
static bool            g_work_held;      /* XhciPrepareSleep took g_work */

static volatile int    g_cmd_busy;
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

/* Wait @ms: sleeping once the usb thread runs, else (boot) spinning */
static void usb_delay(int ms)
{
    if (g_thread_up && interrupts_enabled() && sched_current())
        sched_sleep_until(NULL, sched_ticks() + (UINT64)(ms + 9) / 10 + 1);
    else
        udelay((UINT64)ms * 1000);
}

/* A one-at-a-time flag that waits by yielding */
static void flag_take(volatile int *f)
{
    while (__atomic_exchange_n(f, 1, __ATOMIC_ACQUIRE)) {
        if (interrupts_enabled()) sched_yield();
        else pause_cpu();
    }
}
static void flag_drop(volatile int *f) { __atomic_store_n(f, 0, __ATOMIC_RELEASE); }

static bool ring_init(Ring *r)
{
    r->trbs = alloc_zero(1);
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

static inline void doorbell(int slot, UINT32 target)
{
    mfence();
    wr32(g_db, (UINT32)slot * 4, target);
}

/* ---- device facts ---- */

UINT8  UsbDevSpeed(const UsbDev *d)   { return d->speed; }
UINT8  UsbDevDepth(const UsbDev *d)   { return d->depth; }
UINT16 UsbDevVendor(const UsbDev *d)  { return d->vid; }
UINT16 UsbDevProduct(const UsbDev *d) { return d->pid; }
bool   UsbDevGone(const UsbDev *d)    { return d->gone; }
const char *UsbDevName(const UsbDev *d) { return d->name; }
UINT16 UsbPipeMaxPacket(const UsbPipe *p) { return p->mps; }

const UINT8 *UsbIfaceFind(const UsbIface *f, UINT8 type, int *off)
{
    int o = *off;
    if (o == 0) o = f->desc[0];                         /* skip the interface descriptor */
    while (o + 2 <= f->len && f->desc[o] >= 2) {
        const UINT8 *p = f->desc + o;
        o += p[0];
        if (p[1] == type) { *off = o; return p; }
    }
    *off = o;
    return NULL;
}

void UsbBind(UsbDev *d, void *inst, void (*gone)(void *inst))
{
    if (d->nbind >= MAX_BINDS) return;
    d->bind[d->nbind].inst = inst;
    d->bind[d->nbind].gone = gone;
    d->nbind++;
}

/* ---- events ---- */

static void queue_listen(UsbPipe *p)
{
    ring_push(&p->ring, phys(p->dma), (UINT32)p->listen_len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(p->dev->slot, p->dci);
}

static void on_transfer(const volatile Trb *e)
{
    UINT8  code = (UINT8)(e->status >> 24);
    UINT32 residual = e->status & 0xFFFFFF;
    UINT8  ep = (UINT8)((e->control >> 16) & 0x1F);
    UsbDev *d = g_by_slot[e->control >> 24];
    if (!d || !d->used || d->gone) return;
    if (ep == 1) {
        d->ctl_code = code;
        d->ctl_done = true;
        return;
    }
    UsbPipe *p = d->pipes[ep];
    if (!p || p->dead) return;
    if (p->cb) {                                       /* interrupt IN listener */
        if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
            kprintf("[USB] %s: interrupt transfer failed (code %d)\n", d->name, code);
            p->cb = NULL;
            return;
        }
        int len = p->listen_len - (int)residual;
        if (p->cb(p, p->dma, len, p->cb_ctx)) queue_listen(p);
        else p->cb = NULL;
        return;
    }
    /* A bulk transfer someone waits for: which of its TRBs finished (the
     * last, or the one a short packet ended) */
    int idx = -1;
    for (int i = 0; i < p->ntrb; i++)
        if (p->trb_at[i] == e->param) { idx = i; break; }
    if (idx < 0) return;                               /* stale */
    UINT32 trb_len = p->trb_off[idx + 1] - p->trb_off[idx];
    p->moved = p->trb_off[idx] + (trb_len > residual ? trb_len - residual : 0);
    p->code = code;
    p->done = true;
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
    UsbHidTickAll(sched_ticks());
    spin_unlock_irqrestore(&g_evt_lock, s);
}

void UsbHubSignal(void *hub_inst)
{
    (void)hub_inst;
    g_hubs_pending = true;
    g_port_changed = 1;
}

/* ---- commands and control transfers ---- */

static UINT8 command(UINT64 param, UINT32 control, UINT8 *slot_out)
{
    flag_take(&g_cmd_busy);
    g_cmd_done = false;
    UINT64 at = ring_push(&g_cmd, param, 0, control);
    doorbell(0, 0);
    UINT8 code = 0;
    int spins;
    for (spins = SPIN_LONG; spins > 0; spins--) {
        events_once();
        if (g_cmd_done && g_cmd_trb == at) {
            if (slot_out) *slot_out = g_cmd_slot;
            code = g_cmd_code;
            break;
        }
        pause_cpu();
    }
    flag_drop(&g_cmd_busy);
    if (!spins) kprintf("[USB] command %d timed out\n", (control >> 10) & 0x3F);
    return code;
}

/* A control transfer; the caller holds d->ep0_busy.  IN data lands in d->buf. */
static int control_locked(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len)
{
    if (d->gone) return -1;
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
            if (d->ctl_code == CC_STALL) {
                /* A stall on endpoint 0 is a request the device refused; the
                 * endpoint must be reset before the next one */
                command(0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(d->slot) | TRB_EP(1), NULL);
                UINT64 deq = phys(&d->ep0.trbs[d->ep0.enq]) | d->ep0.cycle;
                command(deq, TRB_TYPE(TRB_SET_DEQ) | TRB_SLOT(d->slot) | TRB_EP(1), NULL);
                return -1;
            }
            if (d->ctl_code != CC_SUCCESS && d->ctl_code != CC_SHORT_PACKET) return -1;
            /* The status stage reports no residual; short IN data shows in the descriptor itself */
            return len;
        }
        if (d->gone) return -1;
        pause_cpu();
    }
    kprintf("[USB] %s: control request %02x timed out\n", d->name, req);
    return -1;
}

int UsbControl(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len, void *data)
{
    if (len > CTL_BUF) return -1;
    __atomic_add_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    int n = -1;
    if (!d->gone) {
        flag_take(&d->ep0_busy);
        if (!(type & 0x80) && len && data) memcpy(d->buf, data, len);
        n = control_locked(d, type, req, value, index, len);
        if (n > 0 && (type & 0x80) && data) memcpy(data, d->buf, (size_t)n);
        flag_drop(&d->ep0_busy);
    }
    __atomic_sub_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    return n;
}

static inline UINT32 *in_slot(UsbDev *d)       { return (UINT32 *)(d->in_ctx + g_ctx); }
static inline UINT32 *in_ep(UsbDev *d, int dci) { return (UINT32 *)(d->in_ctx + (dci + 1) * g_ctx); }

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

UsbPipe *UsbOpenPipe(UsbDev *d, const UINT8 *ep_desc, UINT32 buf_bytes)
{
    UINT8 addr = ep_desc[2];
    UINT8 dci = (UINT8)((addr & 0xF) * 2 + ((addr & 0x80) ? 1 : 0));
    UsbPipe *p = d->pipes[dci];
    if (!p || p->dead) return NULL;
    if (!p->dma) {
        if (buf_bytes < p->mps) buf_bytes = p->mps;
        if (buf_bytes > MAX_PIPE_BUF) buf_bytes = MAX_PIPE_BUF;
        int pages = (int)((buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        p->dma = alloc_zero(pages);
        if (!p->dma) return NULL;
        p->dma_size = (UINT32)pages * PAGE_SIZE;
    }
    return p;
}

bool UsbPipeListen(UsbPipe *p, int len, UsbInCallback cb, void *ctx)
{
    if (p->dead || len <= 0 || (UINT32)len > p->dma_size) return false;
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    p->listen_len = len;
    p->cb_ctx = ctx;
    p->cb = cb;
    queue_listen(p);
    spin_unlock_irqrestore(&g_evt_lock, s);
    return true;
}

int UsbBulk(UsbPipe *p, void *buf, UINT32 len, UINT32 timeout_ms, bool *stalled)
{
    if (stalled) *stalled = false;
    if (p->dead) return -1;
    UsbDev *d = p->dev;
    __atomic_add_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    if (d->gone || p->dead || len > p->dma_size || !len) {
        __atomic_sub_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
        return -1;
    }
    flag_take(&p->busy);
    bool in = (p->addr & 0x80) != 0;
    if (!in && len) memcpy(p->dma, buf, len);

    /* One TD: a TRB per 64 KiB boundary crossed */
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    p->done = false;
    UINT32 off = 0;
    int n = 0;
    do {
        UINT64 at = phys(p->dma) + off;
        UINT32 chunk = 0x10000u - (UINT32)(at & 0xFFFF);
        if (chunk > len - off) chunk = len - off;
        bool last = off + chunk >= len;
        UINT32 left = (UINT32)(MAX_TD_TRBS - n - 1);      /* TD size: TRBs still to come (max 31) */
        p->trb_off[n] = off;
        UINT32 ctl = TRB_TYPE(TRB_NORMAL) | TRB_ISP | (last ? TRB_IOC : TRB_CHAIN);
        p->trb_at[n] = ring_push(&p->ring, at, chunk | ((last ? 0 : (left > 31 ? 31 : left)) << 17), ctl);
        off += chunk;
        n++;
    } while (off < len && n < MAX_TD_TRBS);
    p->ntrb = n;
    p->trb_off[n] = len;
    doorbell(d->slot, p->dci);
    spin_unlock_irqrestore(&g_evt_lock, s);

    UINT64 deadline = rdtsc() + g_tsc_per_tick * (UINT64)timeout_ms / 10;
    int result = -1;
    for (;;) {
        events_once();
        if (p->done) {
            if (p->code == CC_SUCCESS || p->code == CC_SHORT_PACKET) result = (int)p->moved;
            else if (p->code == CC_STALL && stalled) *stalled = true;
            break;
        }
        if (d->gone || rdtsc() > deadline) {
            if (!d->gone) kprintf("[USB] %s: bulk transfer timed out\n", d->name);
            break;
        }
        pause_cpu();
    }
    if (result > 0 && in) memcpy(buf, p->dma, (size_t)result);
    flag_drop(&p->busy);
    __atomic_sub_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    return result;
}

bool UsbPipeReset(UsbPipe *p)
{
    UsbDev *d = p->dev;
    if (p->dead || d->gone) return false;
    if (command(0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(d->slot) | TRB_EP(p->dci), NULL) != CC_SUCCESS)
        return false;
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    UINT64 deq = phys(&p->ring.trbs[p->ring.enq]) | p->ring.cycle;
    spin_unlock_irqrestore(&g_evt_lock, s);
    command(deq, TRB_TYPE(TRB_SET_DEQ) | TRB_SLOT(d->slot) | TRB_EP(p->dci), NULL);
    return UsbControl(d, 0x02, 1, 0, p->addr, 0, NULL) >= 0;       /* CLEAR_FEATURE(ENDPOINT_HALT) */
}

/* ---- enumeration ---- */

static bool root_port_reset(int port)
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

static void free_pages_of(UsbDev *d)
{
    for (int i = 0; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (!p) continue;
        p->dead = true;                    /* (the struct stays: a driver may still hold it) */
        if (p->ring.trbs) kernel_free_pages((void *)p->ring.trbs, 1);
        if (p->dma) kernel_free_pages(p->dma, p->dma_size / PAGE_SIZE);
        p->ring.trbs = NULL;
        p->dma = NULL;
        d->pipes[i] = NULL;
    }
    if (d->ep0.trbs) kernel_free_pages((void *)d->ep0.trbs, 1);
    if (d->in_ctx) kernel_free_pages(d->in_ctx, 3);
    d->ep0.trbs = NULL;
    d->in_ctx = d->out_ctx = d->buf = NULL;
}

/* Take a device off the bus: everything behind it first, then its
 * drivers, then its slot.  Its memory goes once no transfer uses it. */
static void remove_dev(UsbDev *d)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i] && g_devs[i]->used && g_devs[i]->parent == d) remove_dev(g_devs[i]);
    d->gone = true;
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    for (int i = 0; i < 32; i++)
        if (d->pipes[i]) d->pipes[i]->cb = NULL;
    spin_unlock_irqrestore(&g_evt_lock, s);
    for (int i = d->nbind - 1; i >= 0; i--)
        if (d->bind[i].gone) d->bind[i].gone(d->bind[i].inst);
    d->nbind = 0;
    while (__atomic_load_n(&d->users, __ATOMIC_ACQUIRE)) usb_delay(1);
    if (d->slot) {
        s = spin_lock_irqsave(&g_evt_lock);
        g_by_slot[d->slot] = NULL;
        spin_unlock_irqrestore(&g_evt_lock, s);
        command(0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), NULL);
        g_dcbaa[d->slot] = 0;
    }
    free_pages_of(d);
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i] == d) g_devs[i] = NULL;
    /* The UsbDev itself stays allocated: class drivers may still hold
     * pointers to it and its pipes, which now fail every transfer */
}

/* Parse the configuration in d->buf (@total bytes): make a pipe for every
 * endpoint of each interface's first alternate setting */
static int make_pipes(UsbDev *d, const UINT8 *cfg, int total, UINT32 *add_mask, int *max_dci)
{
    int n = 0;
    bool alt0 = false;
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) alt0 = p[3] == 0;
        if (p[1] != USB_DT_ENDPOINT || p[0] < 7 || !alt0) continue;
        UINT8 xfer = p[3] & 3;
        if (xfer == 0 || xfer == 1) continue;           /* (control, isochronous: not supported) */
        UINT8 addr = p[2];
        UINT8 dci = (UINT8)((addr & 0xF) * 2 + ((addr & 0x80) ? 1 : 0));
        if (dci < 2 || dci > 31 || d->pipes[dci]) continue;
        UsbPipe *pp = kzalloc(sizeof(UsbPipe));
        if (!pp) continue;
        if (!ring_init(&pp->ring)) { kfree(pp); continue; }
        pp->dev = d;
        pp->dci = dci;
        pp->addr = addr;
        pp->mps = (UINT16)((p[4] | p[5] << 8) & 0x7FF);
        pp->interval = p[6];
        bool in = (addr & 0x80) != 0;
        pp->type = xfer == 2 ? (in ? EP_BULK_IN : EP_BULK_OUT) : (in ? EP_INTR_IN : EP_INTR_OUT);
        d->pipes[dci] = pp;
        *add_mask |= 1u << dci;
        if (dci > *max_dci) *max_dci = dci;
        n++;
    }
    return n;
}

static bool configure_endpoints(UsbDev *d, UINT32 add, int max_dci)
{
    memset(d->in_ctx, 0, PAGE_SIZE);
    ((UINT32 *)d->in_ctx)[1] = 1u | add;
    memcpy(in_slot(d), d->out_ctx, (size_t)g_ctx);
    in_slot(d)[0] = (in_slot(d)[0] & ~(0x1Fu << 27)) | ((UINT32)max_dci << 27);
    in_slot(d)[3] = 0;                                     /* (output-only fields) */
    for (int dci = 2; dci <= max_dci; dci++) {
        if (!(add & (1u << dci))) continue;
        UsbPipe *p = d->pipes[dci];
        UINT32 *ep = in_ep(d, dci);
        bool intr = p->type == EP_INTR_IN || p->type == EP_INTR_OUT;
        ep[0] = intr ? ep_interval(d->speed, p->interval) << 16 : 0;
        ep[1] = (3u << 1) | ((UINT32)p->type << 3) | ((UINT32)p->mps << 16);
        *(UINT64 *)&ep[2] = phys(p->ring.trbs) | 1;
        ep[4] = intr ? ((UINT32)p->mps | ((UINT32)p->mps << 16)) : 3072;
    }
    return command(phys(d->in_ctx), TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), NULL) == CC_SUCCESS;
}

bool UsbSetHub(UsbDev *d, UINT8 ports, bool mtt, UINT8 think_time)
{
    memset(d->in_ctx, 0, PAGE_SIZE);
    ((UINT32 *)d->in_ctx)[1] = 1u;                         /* the slot context only */
    memcpy(in_slot(d), d->out_ctx, (size_t)g_ctx);
    UINT32 *s = in_slot(d);
    s[0] |= 1u << 26;                                      /* Hub */
    if (mtt && d->speed == USB_SPEED_HIGH) s[0] |= 1u << 25;
    s[1] = (s[1] & 0x00FFFFFFu) | ((UINT32)ports << 24);
    if (d->speed == USB_SPEED_HIGH) s[2] = (s[2] & ~(3u << 16)) | ((UINT32)(think_time & 3) << 16);
    s[3] = 0;
    d->is_hub = true;
    d->mtt = mtt;
    return command(phys(d->in_ctx), TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), NULL) == CC_SUCCESS;
}

static const char *speed_name(UINT8 s)
{
    return s == USB_SPEED_LOW ? "low" : s == USB_SPEED_FULL ? "full" : s == USB_SPEED_HIGH ? "high" : "super";
}

/* Enumerate the device just reset on root port @root (@parent NULL) or
 * on port @hub_port of hub @parent */
static UsbDev *enumerate(UsbDev *parent, UINT8 hub_port, UINT8 root, UINT8 speed)
{
    int idx = -1;
    for (int i = 0; i < MAX_DEVS && idx < 0; i++)
        if (!g_devs[i]) idx = i;
    if (idx < 0) return NULL;
    UsbDev *d = kzalloc(sizeof(UsbDev));
    if (!d) return NULL;
    UINT8 *mem = alloc_zero(3);
    if (!mem || !ring_init(&d->ep0)) {
        if (mem) kernel_free_pages(mem, 3);
        kfree(d);
        return NULL;
    }
    d->in_ctx = mem;
    d->out_ctx = mem + PAGE_SIZE;
    d->buf = mem + 2 * PAGE_SIZE;
    d->speed = speed;
    d->parent = parent;
    d->parent_port = hub_port;
    if (parent) {
        d->root_port = parent->root_port;
        d->depth = (UINT8)(parent->depth + 1);
        UINT32 p = hub_port > 15 ? 15 : hub_port;
        d->route = parent->route | (p << (4 * parent->depth));
        if (parent->speed == USB_SPEED_HIGH && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
            d->tt_slot = parent->slot;
            d->tt_port = hub_port;
            d->mtt = parent->mtt;
        } else {
            d->tt_slot = parent->tt_slot;
            d->tt_port = parent->tt_port;
            d->mtt = parent->mtt;
        }
        ksnprintf(d->name, sizeof(d->name), "%s.%d", parent->name, hub_port);
    } else {
        d->root_port = root;
        ksnprintf(d->name, sizeof(d->name), "port %d", root);
    }
    d->used = true;
    g_devs[idx] = d;

    UINT8 slot = 0;
    if (command(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot) {
        kprintf("[USB] %s: no device slot\n", d->name);
        goto fail;
    }
    d->slot = slot;
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    g_by_slot[slot] = d;
    spin_unlock_irqrestore(&g_evt_lock, s);
    g_dcbaa[slot] = phys(d->out_ctx);

    /* Address Device: slot context + endpoint 0 */
    UINT16 mps0 = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    ((UINT32 *)d->in_ctx)[1] = 0x3;                       /* add slot + EP0 */
    in_slot(d)[0] = (1u << 27) | ((UINT32)speed << 20) | (d->route & 0xFFFFF) | (d->tt_slot && d->mtt ? 1u << 25 : 0);
    in_slot(d)[1] = (UINT32)d->root_port << 16;
    in_slot(d)[2] = (UINT32)d->tt_slot | ((UINT32)d->tt_port << 8);
    UINT32 *ep0 = in_ep(d, 1);
    ep0[1] = (3u << 1) | (EP_CONTROL << 3) | ((UINT32)mps0 << 16);
    *(UINT64 *)&ep0[2] = phys(d->ep0.trbs) | 1;
    ep0[4] = 8;
    if (command(phys(d->in_ctx), TRB_TYPE(TRB_ADDRESS_DEV) | TRB_SLOT(slot), NULL) != CC_SUCCESS) {
        kprintf("[USB] %s: Address Device failed\n", d->name);
        goto fail;
    }

    /* Endpoint 0's real packet size (full speed devices use 8 to 64) */
    if (control_locked(d, 0x80, 6, 0x0100, 0, 8) < 0) goto fail;
    if (speed == USB_SPEED_FULL && d->buf[7] && d->buf[7] != mps0) {
        memset(d->in_ctx, 0, (size_t)3 * g_ctx);
        ((UINT32 *)d->in_ctx)[1] = 0x2;                    /* evaluate EP0 */
        ep0 = in_ep(d, 1);
        memset(ep0, 0, (size_t)g_ctx);
        ep0[1] = (3u << 1) | (EP_CONTROL << 3) | ((UINT32)d->buf[7] << 16);
        if (command(phys(d->in_ctx), TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(slot), NULL) != CC_SUCCESS)
            goto fail;
    }
    if (control_locked(d, 0x80, 6, 0x0100, 0, 18) < 0) goto fail;
    d->vid = (UINT16)(d->buf[8] | d->buf[9] << 8);
    d->pid = (UINT16)(d->buf[10] | d->buf[11] << 8);
    UINT8 dev_class = d->buf[4];

    /* Configuration descriptor: header first, then the whole thing */
    if (control_locked(d, 0x80, 6, 0x0200, 0, 9) < 0) goto fail;
    int total = d->buf[2] | d->buf[3] << 8;
    if (total > CTL_BUF) total = CTL_BUF;
    if (total < 9 || control_locked(d, 0x80, 6, 0x0200, 0, (UINT16)total) < 0) goto fail;
    UINT8 *cfg = kmalloc((size_t)total);
    if (!cfg) goto fail;
    memcpy(cfg, d->buf, (size_t)total);
    UINT8 config = cfg[5];

    UINT32 add = 0;
    int max_dci = 1;
    if (make_pipes(d, cfg, total, &add, &max_dci) && !configure_endpoints(d, add, max_dci)) {
        kprintf("[USB] %s: Configure Endpoint failed\n", d->name);
        kfree(cfg);
        goto fail;
    }
    if (control_locked(d, 0x00, 9, config, 0, 0) < 0) { kfree(cfg); goto fail; }   /* SET_CONFIGURATION */

    kprintf("[USB] %s: device %04x:%04x class %02x (%s speed, slot %d)\n",
            d->name, d->vid, d->pid, dev_class, speed_name(speed), slot);

    /* Offer each interface (first alternate setting) to the drivers */
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] != USB_DT_INTERFACE || p[0] < 9 || p[3] != 0) continue;
        UsbIface f;
        f.number = p[2]; f.alt = p[3];
        f.cls = p[5]; f.sub = p[6]; f.proto = p[7];
        f.desc = p;
        int end = off + p[0];
        while (end + 2 <= total && cfg[end] >= 2 && cfg[end + 1] != USB_DT_INTERFACE) end += cfg[end];
        f.len = end - off;
        void *inst = NULL;
        if (f.cls == 9)       inst = UsbHubProbe(d, &f);
        else if (f.cls == 3)  inst = UsbHidProbe(d, &f);
        else if (f.cls == 8)  inst = UsbMscProbe(d, &f);
        if (!inst)
            kprintf("[USB] %s: interface %d (class %02x/%02x/%02x) not used\n",
                    d->name, f.number, f.cls, f.sub, f.proto);
    }
    kfree(cfg);
    return d;

fail:
    remove_dev(d);
    return NULL;
}

UsbDev *UsbAttachChild(UsbDev *hub, UINT8 port, UINT8 speed)
{
    if (hub->depth >= 5) {
        kprintf("[USB] %s: hubs nested too deep\n", hub->name);
        return NULL;
    }
    return enumerate(hub, port, 0, speed);
}

void UsbDetachChild(UsbDev *hub, UINT8 port)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = g_devs[i];
        if (d && d->used && d->parent == hub && d->parent_port == port) {
            kprintf("[USB] %s: unplugged\n", d->name);
            remove_dev(d);
        }
    }
}

static void attach_root(int port)
{
    if (!root_port_reset(port)) return;
    UINT8 speed = (UINT8)PORT_SPEED(rd32(g_op, OP_PORTSC(port)));
    enumerate(NULL, 0, (UINT8)port, speed);
}

static void detach_root(int port)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = g_devs[i];
        if (d && d->used && !d->parent && d->root_port == port) {
            kprintf("[USB] %s: unplugged\n", d->name);
            remove_dev(d);
        }
    }
}

/* Hot-plug: the port-change events only set flags; the (slow, waiting)
 * enumeration runs here */
static void usb_thread(void *arg)
{
    g_thread_up = true;
    for (;;) {
        sched_sleep_until(NULL, sched_ticks() + 2);
        if (!g_port_changed || g_sleeping) continue;
        /* (sleep waits for this batch to end, so no command is cut off
         * by S3 and none runs while XhciResume resets the controller) */
        flag_take(&g_work);
        if (g_sleeping) { flag_drop(&g_work); continue; }
        g_port_changed = 0;
        if (g_forget_all) {                /* after S3: the old devices are gone */
            g_forget_all = false;
            for (int i = 0; i < MAX_DEVS; i++)
                if (g_devs[i] && g_devs[i]->used && !g_devs[i]->parent) remove_dev(g_devs[i]);
        }
        for (int p = 1; p <= g_ports; p++) {
            if (!g_port_pending[p]) continue;
            g_port_pending[p] = false;
            bool present = false;
            for (int i = 0; i < MAX_DEVS; i++)
                if (g_devs[i] && g_devs[i]->used && !g_devs[i]->parent && g_devs[i]->root_port == p) present = true;
            UINT32 sc = rd32(g_op, OP_PORTSC(p));
            if (present) detach_root(p);
            if (sc & PORT_CCS) attach_root(p);
        }
        if (g_hubs_pending) {
            g_hubs_pending = false;
            UsbHubServiceAll();
        }
        flag_drop(&g_work);
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

static int      g_slots;
static UINT64  *g_erst;

/* Halt, reset and program the controller: device contexts, the command
 * ring and the event ring (allocated already), then run.  Boot and wake. */
static bool controller_program(void)
{
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

    wr32(g_op, OP_CONFIG, (UINT32)g_slots);
    wr64(g_op, OP_DCBAAP, phys(g_dcbaa));

    memset((void *)g_cmd.trbs, 0, PAGE_SIZE);
    g_cmd.enq = 0;
    g_cmd.cycle = 1;
    wr64(g_op, OP_CRCR, phys(g_cmd.trbs) | 1);

    memset((void *)g_evt, 0, PAGE_SIZE);
    g_evt_deq = 0;
    g_evt_ccs = 1;
    wr32(g_rt, IR_ERSTSZ, 1);
    wr64(g_rt, IR_ERDP, phys(g_evt));
    wr64(g_rt, IR_ERSTBA, phys(g_erst));
    wr32(g_rt, IR_IMAN, 1);                                  /* clear pending; interrupts stay off */

    wr32(g_op, OP_USBCMD, CMD_RS);
    return wait_sts(STS_HCH, false);
}

static PciDevice g_pci;

static bool controller_start(const PciDevice *pci)
{
    g_pci = *pci;
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
    g_slots = (int)(hcs1 & 0xFF);
    if (g_slots > 255) g_slots = 255;

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

    if (!ring_init(&g_cmd)) return false;

    /* One-segment event ring */
    g_erst = alloc_zero(1);
    g_evt = alloc_zero(1);
    if (!g_erst || !g_evt) return false;
    g_erst[0] = phys(g_evt);
    g_erst[1] = RING_TRBS;

    if (!controller_program()) return false;
    kprintf("[USB] xHCI %02x:%02x.%d: %d ports, %d slots, %d-byte contexts\n",
            pci->bus, pci->dev, pci->func, g_ports, g_slots, g_ctx);
    return true;
}

static void power_ports(void)
{
    for (int p = 1; p <= g_ports; p++) {
        UINT32 sc = rd32(g_op, OP_PORTSC(p));
        if (!(sc & PORT_PP)) wr32(g_op, OP_PORTSC(p), (sc & PORT_PRESERVE) | PORT_PP);
    }
}

static int count_devs(void)
{
    int n = 0;
    for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i] && g_devs[i]->used) n++;
    return n;
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
    power_ports();
    udelay(20000);

    g_ready = true;                       /* (hubs' interrupt endpoints need the poll) */
    for (int p = 1; p <= g_ports; p++) {
        g_port_pending[p] = false;
        if (rd32(g_op, OP_PORTSC(p)) & PORT_CCS)
            attach_root(p);
    }
    events_once();                       /* (connect events for the ports just handled) */
    for (int p = 1; p <= g_ports; p++) g_port_pending[p] = false;
    g_port_changed = 0;
    /* Devices behind hubs: the hubs found their ports' devices at once */
    while (g_hubs_pending) { g_hubs_pending = false; UsbHubServiceAll(); }
    sched_create_thread("usb", usb_thread, NULL, 8);
    int n = count_devs();
    kprintf("[USB] %d device(s) ready\n", n);
    return n;
}

/* After S3 the controller has lost its state, and the devices theirs: start
 * it again, forget the devices and let the hot-plug thread enumerate what
 * is plugged in (interrupts are off here; enumeration waits). */
static void work_release(void)
{
    g_sleeping = false;
    if (g_work_held) { g_work_held = false; flag_drop(&g_work); }
}

void XhciResume(void)
{
    if (!g_ready) { work_release(); return; }
    g_ready = false;
    IrqState s = spin_lock_irqsave(&g_evt_lock);
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = g_devs[i];
        if (!d || !d->used) continue;
        d->gone = true;                   /* the usb thread lets the drivers go */
        for (int k = 0; k < 32; k++) if (d->pipes[k]) d->pipes[k]->cb = NULL;
        if (d->slot) { g_by_slot[d->slot] = NULL; g_dcbaa[d->slot] = 0; }
        d->slot = 0;                      /* (no Disable Slot: the reset below drops them all) */
    }
    spin_unlock_irqrestore(&g_evt_lock, s);
    if (!controller_program()) {
        kprintf("[USB] xHCI controller didn't restart after sleep\n");
        work_release();
        return;
    }
    power_ports();
    g_forget_all = true;
    for (int p = 1; p <= g_ports; p++) g_port_pending[p] = true;
    g_port_changed = 1;
    g_ready = true;
    work_release();
}

/* Before S3: the root ports may wake the machine (a device connecting or
 * leaving, over-current, a suspended device's remote wakeup), the ports
 * with a device are suspended (U3) and the controller signals PME# */
void XhciPrepareSleep(void)
{
    if (!g_ready) return;
    /* The usb thread finishes what it is doing and waits until the wake
     * (bounded: a stuck enumeration must not keep the machine awake) */
    g_sleeping = true;
    for (UINT64 end = sched_ticks() + 300; !g_work_held && sched_ticks() < end;) {
        if (!__atomic_exchange_n(&g_work, 1, __ATOMIC_ACQUIRE)) g_work_held = true;
        else sched_yield();
    }
    if (!g_work_held) kprintf("[USB] still enumerating: sleeping anyway\n");
    for (int p = 1; p <= g_ports; p++) {
        UINT32 sc = rd32(g_op, OP_PORTSC(p));
        UINT32 w = (sc & PORT_PRESERVE) | (7u << 25);                    /* WCE WDE WOE */
        wr32(g_op, OP_PORTSC(p), w);
        if ((sc & PORT_CCS) && (sc & PORT_PED))
            wr32(g_op, OP_PORTSC(p), (w & ~(0xFu << 5)) | (3u << 5) | (1u << 16));   /* PLS U3, LWS */
    }
    for (UINT8 cap = (UINT8)PciRead32(g_pci.bus, g_pci.dev, g_pci.func, 0x34) & 0xFC, n = 0; cap && n < 48; n++) {
        UINT32 hdr = PciRead32(g_pci.bus, g_pci.dev, g_pci.func, cap);
        if ((hdr & 0xFF) == 1) {                                        /* power management: PME_En, clear PME_Status */
            UINT32 csr = PciRead32(g_pci.bus, g_pci.dev, g_pci.func, cap + 4);
            PciWrite32(g_pci.bus, g_pci.dev, g_pci.func, cap + 4, (csr & ~3u) | (1u << 8) | (1u << 15));
            break;
        }
        cap = (UINT8)(hdr >> 8) & 0xFC;
    }
}
