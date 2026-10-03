/*
 * usb.c — the USB core: enumeration, pipes and hot-plug for every controller
 *
 * Enumeration (USB 2.0 section 9.1.2): reset the port (a root port by the
 * controller driver, a hub's port in usbhub.c), give the device an address
 * (SET_ADDRESS, or xHCI's Address Device), learn endpoint 0's packet size,
 * read the device and configuration descriptors, make a pipe for every
 * endpoint of the configuration (each interface's first alternate
 * setting), SET_CONFIGURATION, and offer each interface to the class
 * drivers (usb.h).  A driver may switch an interface to another
 * alternate setting (UsbSetInterface), whose endpoints then get pipes:
 * audio and video devices keep their isochronous endpoints there.
 *
 * Root ports are watched by a small "usb" kernel thread, which is also
 * the only one that enumerates: a port that changed is looked at again,
 * a device that left is taken away (everything behind it first) and a new
 * one enumerated.  The same thread services hubs whose ports changed and
 * keeps keyboard LEDs in step with the lock keys.
 *
 * The controllers are started in an order that lets an EHCI controller
 * pass its full- and low-speed devices to its companion UHCI or OHCI
 * controllers: xHCI first (on Intel chipsets it takes the shared ports
 * over from EHCI), then EHCI, then OHCI and UHCI.
 */

#include "usb_hc.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

#define MAX_HCS     16
#define MAX_DEVS    128

KSpinLock g_usb_lock = KSPINLOCK_INIT;

static UsbHc   *g_hcs[MAX_HCS];
static int      g_nhc;
static UsbDev  *g_devs[MAX_DEVS];
static volatile bool g_ready;
static volatile bool g_thread_up;
static volatile bool g_hubs_pending;
static volatile bool g_forget_all;       /* woke from S3: drop every device */
static volatile int  g_work;             /* the usb thread is enumerating or removing */
static volatile bool g_sleeping;         /* between UsbPrepareSleep and UsbResume */
static bool          g_work_held;        /* UsbPrepareSleep took g_work */

/* ---- helpers for the controller drivers ---- */

void UsbDelay(int ms)
{
    if (g_thread_up && interrupts_enabled() && sched_current())
        sched_sleep_until(NULL, sched_ticks() + (UINT64)(ms + 9) / 10 + 1);
    else
        udelay((UINT64)ms * 1000);
}

UINT64 UsbDeadline(UINT32 ms) { return rdtsc() + g_tsc_per_tick * (UINT64)ms / 10; }
bool   UsbPast(UINT64 deadline) { return rdtsc() > deadline; }

void UsbFlagTake(volatile int *f)
{
    while (__atomic_exchange_n(f, 1, __ATOMIC_ACQUIRE)) {
        if (interrupts_enabled()) sched_yield();
        else pause_cpu();
    }
}

void UsbFlagDrop(volatile int *f) { __atomic_store_n(f, 0, __ATOMIC_RELEASE); }

void *UsbDmaAlloc(int pages)
{
    void *p = kernel_alloc_pages((size_t)pages);
    if (!p) return NULL;
    if (UsbPhys(p) + (UINT64)pages * PAGE_SIZE > 0x100000000ull) {
        /* (the page allocator hands out low memory first: this only
         * happens once memory below 4 GiB has run out) */
        kernel_free_pages(p, (size_t)pages);
        kprintf("[USB] no DMA memory below 4 GiB\n");
        return NULL;
    }
    memset(p, 0, (size_t)pages * PAGE_SIZE);
    return p;
}

void UsbDmaFree(void *p, int pages)
{
    if (p) kernel_free_pages(p, (size_t)pages);
}

UsbHc *UsbAddController(const UsbHcOps *ops, void *priv, int ports)
{
    if (g_nhc >= MAX_HCS) return NULL;
    UsbHc *hc = kzalloc(sizeof(UsbHc));
    if (!hc) return NULL;
    hc->ops = ops;
    hc->priv = priv;
    hc->ports = ports > USB_MAX_PORTS ? USB_MAX_PORTS : ports;
    hc->num = g_nhc + 1;
    hc->addr_used[0] = 1;                                 /* (address 0: the default address) */
    g_hcs[g_nhc++] = hc;
    return hc;
}

void UsbPortName(UsbHc *hc, int port, char *buf, int size)
{
    if (g_nhc > 1) ksnprintf(buf, (size_t)size, "usb%d port %d", hc->num, port);
    else           ksnprintf(buf, (size_t)size, "port %d", port);
}

/* ---- device facts ---- */

UINT8  UsbDevSpeed(const UsbDev *d)   { return d->speed; }
UINT8  UsbDevDepth(const UsbDev *d)   { return d->depth; }
UINT16 UsbDevVendor(const UsbDev *d)  { return d->vid; }
UINT16 UsbDevProduct(const UsbDev *d) { return d->pid; }
bool   UsbDevGone(const UsbDev *d)    { return d->gone; }
const char *UsbDevName(const UsbDev *d) { return d->name; }
UINT16 UsbPipeMaxPacket(const UsbPipe *p) { return p->mps; }

const UINT8 *UsbDevConfig(const UsbDev *d, int *len)
{
    *len = d->cfg ? d->cfg_len : 0;
    return d->cfg;
}

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
    if (d->nbind >= USB_MAX_BINDS) return;
    d->bind[d->nbind].inst = inst;
    d->bind[d->nbind].gone = gone;
    d->nbind++;
}

/* ---- transfers ---- */

/* A control request on endpoint 0; the caller owns it (d->ep0_busy, or
 * the device is being enumerated).  IN data lands in d->buf. */
static int control_locked(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len)
{
    if (d->gone) return -1;
    UsbSetup s = { type, req, value, index, len };
    bool stalled = false;
    return d->hc->ops->control(d->hc, d, &s, &stalled);
}

int UsbControl(UsbDev *d, UINT8 type, UINT8 req, UINT16 value, UINT16 index, UINT16 len, void *data)
{
    if (len > USB_CTL_BUF) return -1;
    __atomic_add_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    int n = -1;
    if (!d->gone) {
        UsbFlagTake(&d->ep0_busy);
        if (!(type & 0x80) && len && data) memcpy(d->buf, data, len);
        n = control_locked(d, type, req, value, index, len);
        if (n > 0 && (type & 0x80) && data) memcpy(data, d->buf, (size_t)n);
        UsbFlagDrop(&d->ep0_busy);
    }
    __atomic_sub_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    return n;
}

#define MAX_PIPE_BUF  (960u * 1024)

UsbPipe *UsbOpenPipe(UsbDev *d, const UINT8 *ep_desc, UINT32 buf_bytes)
{
    UINT8 addr = ep_desc[2];
    UINT8 idx = (UINT8)((addr & 0xF) * 2 + ((addr & 0x80) ? 1 : 0));
    UsbPipe *p = d->pipes[idx];
    if (!p || p->dead) return NULL;
    if (!p->dma) {
        if (buf_bytes < p->mps) buf_bytes = p->mps;
        if (buf_bytes > MAX_PIPE_BUF) buf_bytes = MAX_PIPE_BUF;
        int pages = (int)((buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        p->dma = UsbDmaAlloc(pages);
        if (!p->dma) return NULL;
        p->dma_size = (UINT32)pages * PAGE_SIZE;
    }
    return p;
}

bool UsbPipeListen(UsbPipe *p, int len, UsbInCallback cb, void *ctx)
{
    if (p->dead || !p->in || len <= 0 || (UINT32)len > p->dma_size) return false;
    UsbDev *d = p->dev;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    bool ok = false;
    if (!d->gone) {
        p->listen_len = len;
        p->cb_ctx = ctx;
        p->cb = cb;
        ok = d->hc->ops->listen(d->hc, p);
        if (!ok) p->cb = NULL;
    }
    spin_unlock_irqrestore(&g_usb_lock, s);
    return ok;
}

bool UsbPipeDeliver(UsbPipe *p, int len)
{
    if (!p->cb || p->dead || p->dev->gone) return false;
    if (p->cb(p, p->dma, len, p->cb_ctx)) return true;
    p->cb = NULL;
    return false;
}

void UsbPipeListenFailed(UsbPipe *p, int code)
{
    if (!p->cb || p->dev->gone) return;
    kprintf("[USB] %s: interrupt transfer failed (code %d)\n", p->dev->name, code);
    p->cb = NULL;
}

UINT32 UsbPipePeriod(const UsbPipe *p)
{
    UINT32 b = p->interval ? p->interval : 1;
    if (p->dev->speed == USB_SPEED_HIGH || p->dev->speed == USB_SPEED_SUPER || p->xfer == 1)
        b = 1u << ((b > 16 ? 16 : b) - 1);             /* 2^(bInterval-1) (micro)frames */
    return p->dev->speed == USB_SPEED_HIGH || p->dev->speed == USB_SPEED_SUPER ? b : b * 8;
}

UINT32 UsbIsoPacketSize(const UsbPipe *p) { return (UINT32)p->mps * (1u + p->mult); }
UINT32 UsbIsoIntervalUs(const UsbPipe *p) { return UsbPipePeriod(p) * 125; }

bool UsbIsoStart(UsbPipe *p, int xfers, int packets, UsbIsoCallback cb, void *ctx)
{
    UsbDev *d = p->dev;
    if (p->dead || d->gone || p->xfer != 1 || p->iso_cb || xfers < 2 || packets < 1 || xfers * packets > 128 ||
        !d->hc->ops->iso_start)
        return false;
    UINT32 psize = UsbIsoPacketSize(p);
    UINT32 need = (UINT32)(xfers * packets) * psize;
    if (need > p->dma_size) {
        int pages = (int)((need + PAGE_SIZE - 1) / PAGE_SIZE);
        UINT8 *buf = UsbDmaAlloc(pages);
        if (!buf) return false;
        UsbDmaFree(p->dma, (int)(p->dma_size / PAGE_SIZE));
        p->dma = buf;
        p->dma_size = (UINT32)pages * PAGE_SIZE;
    }
    kfree(p->iso_len);
    p->iso_len = kzalloc((size_t)(xfers * packets) * sizeof(UINT16));
    if (!p->iso_len) return false;
    p->iso_xfers = xfers;
    p->iso_packets = packets;
    p->iso_psize = psize;
    p->iso_ctx = ctx;
    for (int k = 0; k < xfers; k++) {
        UINT16 *lens = p->iso_len + k * packets;
        if (p->in) for (int i = 0; i < packets; i++) lens[i] = (UINT16)psize;
        else cb(p, p->dma + (UINT32)(k * packets) * psize, lens, packets, ctx);
    }
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    p->iso_cb = cb;
    bool ok = !d->gone && d->hc->ops->iso_start(d->hc, p);
    if (!ok) p->iso_cb = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    return ok;
}

bool UsbIsoDone(UsbPipe *p, int k)
{
    UsbIsoCallback cb = p->iso_cb;
    if (!cb || p->dead || p->dev->gone || k < 0 || k >= p->iso_xfers) return false;
    int n = p->iso_packets;
    UINT16 *lens = p->iso_len + k * n;
    cb(p, p->dma + (UINT32)(k * n) * p->iso_psize, lens, n, p->iso_ctx);
    if (p->in) for (int i = 0; i < n; i++) lens[i] = (UINT16)p->iso_psize;
    return p->iso_cb != NULL;
}

void UsbIsoStop(UsbPipe *p)
{
    UsbDev *d = p->dev;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    bool was = p->iso_cb != NULL;
    p->iso_cb = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    if (was && !p->dead && p->hcd && d->hc->ops->iso_stop) d->hc->ops->iso_stop(d->hc, p);
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
    UsbFlagTake(&p->busy);
    if (!p->in) memcpy(p->dma, buf, len);
    bool st = false;
    int n = d->hc->ops->bulk(d->hc, p, len, timeout_ms, &st);
    if (stalled) *stalled = st;
    if (n > 0 && p->in) memcpy(buf, p->dma, (size_t)n);
    UsbFlagDrop(&p->busy);
    __atomic_sub_fetch(&d->users, 1, __ATOMIC_ACQ_REL);
    return n;
}

bool UsbPipeReset(UsbPipe *p)
{
    UsbDev *d = p->dev;
    if (p->dead || d->gone) return false;
    if (!d->hc->ops->pipe_reset(d->hc, p)) return false;
    return UsbControl(d, 0x02, 1, 0, p->addr, 0, NULL) >= 0;       /* CLEAR_FEATURE(ENDPOINT_HALT) */
}

bool UsbSetHub(UsbDev *d, UINT8 ports, bool mtt, UINT8 think_time)
{
    d->is_hub = true;
    d->mtt = mtt;
    return !d->hc->ops->dev_hub || d->hc->ops->dev_hub(d->hc, d, ports, mtt, think_time);
}

void UsbHubSignal(void *hub_inst)
{
    (void)hub_inst;
    g_hubs_pending = true;
}

/* ---- enumeration ---- */

static int alloc_address(UsbHc *hc)
{
    for (int a = 1; a < 128; a++)
        if (!(hc->addr_used[a >> 3] & (1u << (a & 7)))) {
            hc->addr_used[a >> 3] |= (UINT8)(1u << (a & 7));
            return a;
        }
    return 0;
}

static void free_address(UsbHc *hc, int a)
{
    if (a > 0 && a < 128) hc->addr_used[a >> 3] &= (UINT8)~(1u << (a & 7));
}

/* Take a device off the bus: everything behind it first, then its
 * drivers, then its state on the controller.  The UsbDev and its pipes
 * stay allocated: class drivers may still hold pointers to them, and
 * every transfer on them now fails. */
static void remove_dev(UsbDev *d)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i] && g_devs[i]->parent == d) remove_dev(g_devs[i]);
    d->gone = true;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int i = 0; i < 32; i++)
        if (d->pipes[i]) d->pipes[i]->cb = NULL, d->pipes[i]->iso_cb = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    for (int i = d->nbind - 1; i >= 0; i--)
        if (d->bind[i].gone) d->bind[i].gone(d->bind[i].inst);
    d->nbind = 0;
    while (__atomic_load_n(&d->users, __ATOMIC_ACQUIRE)) UsbDelay(1);
    d->hc->ops->dev_remove(d->hc, d);
    for (int i = 0; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (!p) continue;
        p->dead = true;
        UsbDmaFree(p->dma, (int)(p->dma_size / PAGE_SIZE));
        p->dma = NULL;
        kfree(p->iso_len);
        p->iso_len = NULL;
        d->pipes[i] = NULL;
    }
    UsbDmaFree(d->buf, 1);
    d->buf = NULL;
    kfree(d->cfg);
    d->cfg = NULL;
    free_address(d->hc, d->addr);
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i] == d) g_devs[i] = NULL;
}

/* A pipe for the endpoint descriptor @p of interface @iface, in
 * d->pipes; its idx bit, or 0 */
static UINT32 new_pipe(UsbDev *d, const UINT8 *p, UINT8 iface)
{
    UINT8 xfer = p[3] & 3;
    if (xfer == 0) return 0;                            /* (control: endpoint 0 only) */
    UINT8 addr = p[2];
    UINT8 idx = (UINT8)((addr & 0xF) * 2 + ((addr & 0x80) ? 1 : 0));
    if (idx < 2 || idx > 31 || d->pipes[idx]) return 0;
    UsbPipe *pp = kzalloc(sizeof(UsbPipe));
    if (!pp) return 0;
    UINT16 wmps = (UINT16)(p[4] | p[5] << 8);
    pp->dev = d;
    pp->idx = idx;
    pp->addr = addr;
    pp->xfer = xfer;
    pp->in = (addr & 0x80) != 0;
    pp->mps = (UINT16)(wmps & 0x7FF);
    pp->mult = d->speed == USB_SPEED_HIGH && xfer & 1 ? (UINT8)((wmps >> 11) & 3) : 0;
    if (pp->mult > 2) pp->mult = 2;
    pp->interval = p[6];
    pp->iface = iface;
    if (!pp->mps || !d->hc->ops->pipe_add(d->hc, pp)) {
        if (pp->mps) kprintf("[USB] %s: endpoint %02x not usable\n", d->name, addr);
        kfree(pp);
        return 0;                                       /* (zero-bandwidth: nothing to open) */
    }
    d->pipes[idx] = pp;
    return 1u << idx;
}

/* Make a pipe for every endpoint of each interface's first alternate
 * setting */
static int make_pipes(UsbDev *d, const UINT8 *cfg, int total)
{
    int n = 0;
    bool alt0 = false;
    UINT8 iface = 0;
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) { alt0 = p[3] == 0; iface = p[2]; }
        if (p[1] != USB_DT_ENDPOINT || p[0] < 7 || !alt0) continue;
        if (new_pipe(d, p, iface)) n++;
    }
    return n;
}

/* Close a pipe of an alternate setting being left */
static void drop_pipe(UsbDev *d, UsbPipe *p)
{
    if (p->iso_xfers) UsbIsoStop(p);
    if (p->hcd && d->hc->ops->pipe_drop) d->hc->ops->pipe_drop(d->hc, p);
    p->dead = true;
    UsbDmaFree(p->dma, (int)(p->dma_size / PAGE_SIZE));
    kfree(p->iso_len);
    kfree(p);
}

bool UsbSetInterface(UsbDev *d, UINT8 iface, UINT8 alt)
{
    if (d->gone || !d->cfg) return false;
    const UINT8 *cfg = d->cfg;
    int total = d->cfg_len, at = -1;
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off])
        if (cfg[off + 1] == USB_DT_INTERFACE && cfg[off] >= 9 && cfg[off + 2] == iface && cfg[off + 3] == alt) {
            at = off;
            break;
        }
    if (at < 0) return false;

    /* The old setting's pipes leave d->pipes, the new one's come in; the
     * controller learns of both at once (xHCI's Configure Endpoint) */
    UsbPipe *old[32] = { 0 };
    UINT32 drop = 0, add = 0;
    for (int i = 2; i < 32; i++)
        if (d->pipes[i] && d->pipes[i]->iface == iface && d->pipes[i]->iso_xfers) UsbIsoStop(d->pipes[i]);
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int i = 2; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (!p || p->iface != iface) continue;
        p->cb = NULL;
        old[i] = p;
        d->pipes[i] = NULL;
        drop |= 1u << i;
    }
    spin_unlock_irqrestore(&g_usb_lock, s);
    for (int off = at + cfg[at]; off + 2 <= total && cfg[off] >= 2 && cfg[off + 1] != USB_DT_INTERFACE; off += cfg[off])
        if (cfg[off + 1] == USB_DT_ENDPOINT && cfg[off] >= 7) add |= new_pipe(d, &cfg[off], iface);
    bool ok = !d->hc->ops->iface_config || (!drop && !add) || d->hc->ops->iface_config(d->hc, d, drop, add);
    for (int i = 2; i < 32; i++)
        if (old[i]) drop_pipe(d, old[i]);
    if (!ok) kprintf("[USB] %s: the controller refused interface %d's endpoints\n", d->name, iface);
    if (ok && UsbControl(d, 0x01, 11, alt, iface, 0, NULL) < 0) ok = false;     /* SET_INTERFACE */
    if (!ok)
        for (int i = 2; i < 32; i++)
            if (add & (1u << i)) {
                UsbPipe *p = d->pipes[i];
                d->pipes[i] = NULL;
                drop_pipe(d, p);
            }
    return ok;
}

static const char *speed_name(UINT8 s)
{
    return s == USB_SPEED_LOW ? "low" : s == USB_SPEED_FULL ? "full" : s == USB_SPEED_HIGH ? "high" : "super";
}

/* Give a device on the default address its own address (controllers
 * without dev_address), learning endpoint 0's packet size on the way */
static bool set_address(UsbDev *d)
{
    /* The first 8 bytes of the device descriptor, at address 0, carry
     * bMaxPacketSize0 */
    if (control_locked(d, 0x80, 6, 0x0100, 0, 8) < 8) return false;
    UINT8 mps = d->buf[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) mps = 8;
    d->ep0_mps = d->speed == USB_SPEED_HIGH ? 64 : mps;
    int a = alloc_address(d->hc);
    if (!a) return false;
    if (control_locked(d, 0x00, 5, (UINT16)a, 0, 0) < 0) {    /* SET_ADDRESS */
        free_address(d->hc, a);
        return false;
    }
    d->addr = (UINT8)a;
    UsbDelay(2);                                             /* set-address recovery */
    return true;
}

/* Enumerate the device just reset on root port @root of @hc (@parent
 * NULL) or on port @hub_port of hub @parent */
static UsbDev *enumerate(UsbHc *hc, UsbDev *parent, UINT8 hub_port, UINT8 root, UINT8 speed)
{
    int idx = -1;
    for (int i = 0; i < MAX_DEVS && idx < 0; i++)
        if (!g_devs[i]) idx = i;
    if (idx < 0) return NULL;
    UsbDev *d = kzalloc(sizeof(UsbDev));
    if (!d) return NULL;
    d->buf = UsbDmaAlloc(1);
    if (!d->buf) { kfree(d); return NULL; }
    d->hc = hc;
    d->speed = speed;
    d->parent = parent;
    d->parent_port = hub_port;
    d->ep0_mps = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    if (parent) {
        d->root_port = parent->root_port;
        d->depth = (UINT8)(parent->depth + 1);
        UINT32 p = hub_port > 15 ? 15 : hub_port;
        d->route = parent->route | (p << (4 * parent->depth));
        if (parent->speed == USB_SPEED_HIGH && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
            d->tt_hub = parent;
            d->tt_port = hub_port;
        } else {
            d->tt_hub = parent->tt_hub;
            d->tt_port = parent->tt_port;
        }
        ksnprintf(d->name, sizeof(d->name), "%s.%d", parent->name, hub_port);
    } else {
        d->root_port = root;
        UsbPortName(hc, root, d->name, sizeof(d->name));
    }
    g_devs[idx] = d;

    /* An address, and endpoint 0's packet size */
    if (hc->ops->dev_address) {
        if (!hc->ops->dev_address(hc, d)) goto fail;
        if (control_locked(d, 0x80, 6, 0x0100, 0, 8) < 0) goto fail;
        if (speed == USB_SPEED_FULL && d->buf[7] && d->buf[7] != d->ep0_mps) {
            d->ep0_mps = d->buf[7];
            if (hc->ops->dev_ep0 && !hc->ops->dev_ep0(hc, d)) goto fail;
        }
    } else if (!set_address(d)) {
        kprintf("[USB] %s: no address\n", d->name);
        goto fail;
    }
    if (control_locked(d, 0x80, 6, 0x0100, 0, 18) < 0) goto fail;
    d->vid = (UINT16)(d->buf[8] | d->buf[9] << 8);
    d->pid = (UINT16)(d->buf[10] | d->buf[11] << 8);
    UINT8 dev_class = d->buf[4];

    /* Configuration descriptor: header first, then the whole thing */
    if (control_locked(d, 0x80, 6, 0x0200, 0, 9) < 0) goto fail;
    int total = d->buf[2] | d->buf[3] << 8;
    if (total > USB_CTL_BUF) total = USB_CTL_BUF;
    if (total < 9 || control_locked(d, 0x80, 6, 0x0200, 0, (UINT16)total) < 0) goto fail;
    UINT8 *cfg = kmalloc((size_t)total);
    if (!cfg) goto fail;
    memcpy(cfg, d->buf, (size_t)total);
    UINT8 config = cfg[5];

    d->cfg = cfg;
    d->cfg_len = total;
    if (make_pipes(d, cfg, total) && hc->ops->dev_config && !hc->ops->dev_config(hc, d)) {
        kprintf("[USB] %s: the controller refused the endpoints\n", d->name);
        goto fail;
    }
    if (control_locked(d, 0x00, 9, config, 0, 0) < 0) goto fail;   /* SET_CONFIGURATION */

    kprintf("[USB] %s: device %04x:%04x class %02x (%s speed, %s, address %d)\n",
            d->name, d->vid, d->pid, dev_class, speed_name(speed), hc->ops->kind, d->addr);

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
        else if (f.cls == 1)  inst = UsbAudioProbe(d, &f);
        if (!inst)
            kprintf("[USB] %s: interface %d (class %02x/%02x/%02x) not used\n",
                    d->name, f.number, f.cls, f.sub, f.proto);
    }
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
    return enumerate(hub->hc, hub, port, 0, speed);
}

void UsbDetachChild(UsbDev *hub, UINT8 port)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = g_devs[i];
        if (d && d->parent == hub && d->parent_port == port) {
            kprintf("[USB] %s: unplugged\n", d->name);
            remove_dev(d);
        }
    }
}

/* ---- root ports and hot-plug ---- */

static UsbDev *root_dev(UsbHc *hc, int port)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = g_devs[i];
        if (d && d->hc == hc && !d->parent && d->root_port == port) return d;
    }
    return NULL;
}

/* Look at the root ports of @hc: take away what left, enumerate what
 * arrived (@all: every connected port, as at boot) */
static void scan_ports(UsbHc *hc, bool all)
{
    if (hc->rescan) { hc->rescan = false; all = true; }
    for (int p = 1; p <= hc->ports; p++) {
        UINT32 st = hc->ops->port_status(hc, p);
        bool changed = (st & USB_PORT_CHANGED) != 0;
        if (!changed && !all) continue;
        UsbDev *d = root_dev(hc, p);
        if (d && (changed || !(st & USB_PORT_CONNECTED) || d->gone)) {     /* (gone: from before S3) */
            kprintf("[USB] %s: unplugged\n", d->name);
            remove_dev(d);
            d = NULL;
        }
        if (!(st & USB_PORT_CONNECTED) || d) continue;
        if (!all) UsbDelay(100);                           /* debounce a plug-in */
        UINT8 speed = hc->ops->port_reset(hc, p);
        if (speed) enumerate(hc, NULL, 0, (UINT8)p, speed);
    }
}

static void service_hubs(void)
{
    while (g_hubs_pending) {
        g_hubs_pending = false;
        UsbHubServiceAll();
    }
}

static void usb_thread(void *arg)
{
    (void)arg;
    g_thread_up = true;
    for (;;) {
        sched_sleep_until(NULL, sched_ticks() + 2);
        if (g_sleeping) continue;
        /* (sleep waits for this round to end, so no transfer is cut off by
         * S3 and none runs while UsbResume resets the controllers) */
        UsbFlagTake(&g_work);
        if (g_sleeping) { UsbFlagDrop(&g_work); continue; }
        if (g_forget_all) {                /* after S3: the old devices are gone */
            g_forget_all = false;
            for (int i = 0; i < MAX_DEVS; i++)
                if (g_devs[i] && !g_devs[i]->parent && g_devs[i]->gone) remove_dev(g_devs[i]);
        }
        if (g_ready) {
            for (int i = 0; i < g_nhc; i++) scan_ports(g_hcs[i], false);
            service_hubs();
            UsbHidSyncLeds();
        }
        UsbFlagDrop(&g_work);
    }
}

void UsbPoll(void)
{
    if (!g_ready) return;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int i = 0; i < g_nhc; i++) g_hcs[i]->ops->poll(g_hcs[i]);
    UsbHidTickAll(sched_ticks());
    spin_unlock_irqrestore(&g_usb_lock, s);
}

static int probe_class(UINT8 prog_if, bool (*probe)(const PciDevice *))
{
    int n = 0;
    PciDevice pci;
    for (int i = 0; i < MAX_HCS && PciFindClass(0x0C, 0x03, prog_if, i, &pci); i++)
        if (probe(&pci)) n++;
    return n;
}

int UsbInit(void)
{
    probe_class(0x30, XhciProbe);
    int first_companion_hc = g_nhc;
    probe_class(0x20, EhciProbe);
    int ehcis = g_nhc - first_companion_hc;
    first_companion_hc = g_nhc;
    probe_class(0x10, OhciProbe);
    probe_class(0x00, UhciProbe);
    if (!g_nhc) return 0;

    UsbDelay(20);                          /* (ports were just powered: let devices connect) */
    g_ready = true;                        /* (interrupt transfers need the poll) */
    for (int i = 0; i < first_companion_hc; i++) scan_ports(g_hcs[i], true);
    if (ehcis) UsbDelay(20);               /* (devices EHCI passed on show up on the companions) */
    for (int i = first_companion_hc; i < g_nhc; i++) scan_ports(g_hcs[i], true);
    service_hubs();                        /* devices behind hubs */
    sched_create_thread("usb", usb_thread, NULL, PRIO_DEVICE_IO);   /* (above programs: scheduler.h) */

    int n = 0;
    for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i]) n++;
    kprintf("[USB] %d controller(s), %d device(s) ready\n", g_nhc, n);
    return n;
}

/* After S3 every controller has lost its state, and the devices theirs:
 * start the controllers again, forget the devices and let the usb thread
 * enumerate what is plugged in (interrupts are off here). */
void UsbPrepareSleep(void)
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
    for (int i = 0; i < g_nhc; i++)
        if (g_hcs[i]->ops->prepare_sleep) g_hcs[i]->ops->prepare_sleep(g_hcs[i]);
}

static void work_release(void)
{
    g_sleeping = false;
    if (g_work_held) { g_work_held = false; UsbFlagDrop(&g_work); }
}

void UsbResume(void)
{
    if (!g_ready) { work_release(); return; }
    g_ready = false;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int i = 0; i < MAX_DEVS; i++) {
        UsbDev *d = g_devs[i];
        if (!d) continue;
        d->gone = true;                    /* the usb thread lets the drivers go */
        for (int k = 0; k < 32; k++) if (d->pipes[k]) d->pipes[k]->cb = NULL;
    }
    spin_unlock_irqrestore(&g_usb_lock, s);
    for (int i = 0; i < g_nhc; i++) {
        if (!g_hcs[i]->ops->resume(g_hcs[i]))
            kprintf("[USB] %s controller (bus %d) didn't restart after sleep\n", g_hcs[i]->ops->kind, g_hcs[i]->num);
        g_hcs[i]->rescan = true;
    }
    g_forget_all = true;
    g_ready = true;
    work_release();
}
