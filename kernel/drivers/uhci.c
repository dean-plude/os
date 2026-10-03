/*
 * uhci.c — USB 1.1 (UHCI) host controller driver
 *
 * Follows the Universal Host Controller Interface design guide, revision
 * 1.1.  The 1024 frame-list entries all point at the same three skeleton
 * queue heads, one after the other: interrupt, control, bulk.  Each
 * endpoint has its own queue head behind the skeleton of its kind, and a
 * transfer is a chain of TDs, one per packet, hung from that queue head's
 * element pointer while it is idle.  Every interrupt endpoint is therefore
 * polled each frame.
 *
 * UHCI leaves the data toggle to software: each pipe remembers the next
 * one.  IN TDs have short-packet detection on, so a short packet stops the
 * queue where it is: that ends a bulk or interrupt transfer, and moves a
 * control transfer on to its status stage.
 *
 * Polled: the controller's interrupts stay off; poll() looks at the
 * interrupt TDs every tick, and a waited-for transfer is spun on.
 */

#include "usb_hc.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* I/O registers */
#define REG_USBCMD      0x00
#define REG_USBSTS      0x02
#define REG_USBINTR     0x04
#define REG_FRNUM       0x06
#define REG_FRBASEADD   0x08
#define REG_SOFMOD      0x0C
#define REG_PORTSC(p)   (0x10 + 2 * ((p) - 1))

#define CMD_RS          (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_GRESET      (1u << 2)
#define CMD_CF          (1u << 6)
#define CMD_MAXP        (1u << 7)
#define STS_HCHALTED    (1u << 5)

/* PORTSC */
#define PORT_CCS        (1u << 0)
#define PORT_CSC        (1u << 1)
#define PORT_PE         (1u << 2)
#define PORT_PEC        (1u << 3)
#define PORT_ALWAYS1    (1u << 7)
#define PORT_LSDA       (1u << 8)
#define PORT_PR         (1u << 9)
#define PORT_W1C        (PORT_CSC | PORT_PEC)

/* Link pointers */
#define LP_T            1u
#define LP_QH           (1u << 1)
#define LP_VF           (1u << 2)          /* depth first: the next TD in the same frame */

/* TD control/status */
#define TD_ACTLEN(v)    (((v) + 1) & 0x7FF)
#define TD_ACTIVE       (1u << 23)
#define TD_STALLED      (1u << 22)
#define TD_ERRORS       (0x7Eu << 16)     /* stalled, buffer, babble, NAK, CRC/timeout, bitstuff */
#define TD_IOC          (1u << 24)
#define TD_LS           (1u << 26)
#define TD_CERR3        (3u << 27)
#define TD_SPD          (1u << 29)

/* TD token */
#define PID_IN          0x69
#define PID_OUT         0xE1
#define PID_SETUP       0x2D
#define TOK_TOGGLE      (1u << 19)

#define TD_SIZE         32
#define MAX_LISTEN      64

typedef struct __attribute__((packed, aligned(16))) {
    UINT32 link, ctl, token, buf;
    UINT32 len, pad[3];                   /* (software) bytes asked for */
} Td;

typedef struct __attribute__((packed, aligned(16))) {
    UINT32 head, elem;
    UINT32 pad[2];
} Qh;

/* A queue: its queue head and setup packet in a page, then the TD pool */
typedef struct {
    Qh      qh;
    UINT8   setup[16];
    Td      td[];                         /* (td[0] starts at offset 32) */
} QPage;

typedef struct {
    QPage  *q;
    int     pages;
    int     slots;                        /* TDs in the pool */
    int     kind;                         /* 0 control, 1 bulk, 2 interrupt */
    bool    linked;
    bool    toggle;                       /* the next packet's data toggle */
    int     n;                            /* TDs of the transfer in flight */
} UQueue;

typedef struct {
    UQueue *ep0;
} UDev;

typedef struct {
    UsbHc            *hc;
    UINT16            io;
    int               ports;
    UINT32           *frames;
    QPage            *skel;               /* skeleton queue heads (interrupt, control, bulk) */
    UsbPipe          *listening[MAX_LISTEN];
    PciDevice         pci;
} Uhci;

static inline UINT32 p32(const volatile void *va) { return (UINT32)UsbPhys(va); }
static inline Uhci *U(UsbHc *hc) { return hc->priv; }
static inline Qh *skel(Uhci *u, int kind) { return (Qh *)((UINT8 *)u->skel + 16 * kind); }

/* ---- queues ---- */

static void queue_link(Uhci *u, UQueue *uq)
{
    Qh *s = skel(u, uq->kind);
    IrqState st = spin_lock_irqsave(&g_usb_lock);
    uq->q->qh.head = s->head;
    mfence();
    s->head = p32(&uq->q->qh) | LP_QH;
    uq->linked = true;
    spin_unlock_irqrestore(&g_usb_lock, st);
}

static void queue_unlink(Uhci *u, UQueue *uq)
{
    if (!uq->linked) return;
    UINT32 me = p32(&uq->q->qh) | LP_QH;
    Qh *s = skel(u, uq->kind);
    UINT32 end = uq->kind < 2 ? (p32(skel(u, uq->kind + 1)) | LP_QH) : LP_T;
    IrqState st = spin_lock_irqsave(&g_usb_lock);
    uq->linked = false;
    Qh *prev = s;
    for (int guard = 0; guard < 1024 && prev->head != end && !(prev->head & LP_T); guard++) {
        if (prev->head == me) { prev->head = uq->q->qh.head; break; }
        prev = UsbVirt(prev->head & ~0xFu);
    }
    spin_unlock_irqrestore(&g_usb_lock, st);
    UsbDelay(2);                          /* (the controller is past any frame that had it) */
}

static UQueue *queue_new(Uhci *u, int kind, int slots)
{
    UQueue *uq = kzalloc(sizeof(UQueue));
    if (!uq) return NULL;
    uq->pages = (32 + slots * TD_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uq->q = UsbDmaAlloc(uq->pages);
    if (!uq->q) { kfree(uq); return NULL; }
    uq->slots = (uq->pages * PAGE_SIZE - 32) / TD_SIZE;
    uq->kind = kind;
    uq->q->qh.elem = LP_T;
    queue_link(u, uq);
    return uq;
}

static void queue_free(Uhci *u, UQueue *uq)
{
    if (!uq) return;
    queue_unlink(u, uq);
    UsbDmaFree(uq->q, uq->pages);
    kfree(uq);
}

/* Fill TD @i of @uq: one packet */
static void td_fill(UQueue *uq, int i, UsbDev *d, UINT8 ep, UINT8 pid, bool toggle, UINT64 pa, UINT32 len, bool last)
{
    Td *td = &uq->q->td[i];
    td->link = last ? LP_T : (p32(&uq->q->td[i + 1]) | LP_VF);
    td->ctl = TD_ACTIVE | TD_CERR3 | (pid == PID_IN ? TD_SPD : 0) | (d->speed == USB_SPEED_LOW ? TD_LS : 0);
    td->token = pid | ((UINT32)d->addr << 8) | ((UINT32)(ep & 0xF) << 15) | (toggle ? TOK_TOGGLE : 0) |
                ((len ? len - 1 : 0x7FFu) << 21);
    td->buf = (UINT32)pa;
    td->len = len;
}

/* Hang the chain at TD @first on the idle queue */
static void queue_start(UQueue *uq, int first)
{
    mfence();
    uq->q->qh.elem = p32(&uq->q->td[first]);
    mfence();
}

/* Take the queue's TDs off it (finished early, or timed out) */
static void queue_stop(UQueue *uq)
{
    for (int i = 0; i < uq->n; i++) uq->q->td[i].ctl &= ~TD_ACTIVE;
    uq->q->qh.elem = LP_T;
    UsbDelay(1);                          /* (a TD the controller was on writes back) */
    uq->q->qh.elem = LP_T;
}

typedef enum { Q_BUSY, Q_DONE, Q_FAILED, Q_STALLED, Q_SHORT } QState;

/* How TDs @from..@to-1 stand; *@moved: bytes; *@last: the last TD done */
static QState queue_state(UQueue *uq, int from, int to, UINT32 *moved, int *last)
{
    UINT32 m = 0;
    *last = from - 1;
    for (int i = from; i < to; i++) {
        Td *td = &uq->q->td[i];
        UINT32 c = td->ctl;
        if (c & TD_ACTIVE) { *moved = m; return Q_BUSY; }
        if (c & TD_STALLED) { *moved = m; return (c & (TD_ERRORS & ~TD_STALLED & ~(1u << 19))) ? Q_FAILED : Q_STALLED; }
        if (c & TD_ERRORS & ~(1u << 19)) { *moved = m; return Q_FAILED; }
        UINT32 got = TD_ACTLEN(c);
        if (got > td->len) got = td->len;
        m += got;
        *last = i;
        if (got < td->len) { *moved = m; return Q_SHORT; }
    }
    *moved = m;
    return Q_DONE;
}

/* The toggle after TD @i went through */
static inline bool toggle_after(UQueue *uq, int i) { return !(uq->q->td[i].token & TOK_TOGGLE); }

/* ---- devices and transfers ---- */

static int uhci_control(UsbHc *hc, UsbDev *d, const UsbSetup *s, bool *stalled)
{
    Uhci *u = U(hc);
    UDev *ud = d->hcd;
    if (!ud) {
        ud = kzalloc(sizeof(UDev));
        if (!ud) return -1;
        ud->ep0 = queue_new(u, 0, USB_CTL_BUF / 8 + 2);
        if (!ud->ep0) { kfree(ud); return -1; }
        d->hcd = ud;
    }
    if (d->gone) return -1;
    UQueue *uq = ud->ep0;
    memcpy(uq->q->setup, s, 8);
    bool in = (s->type & 0x80) != 0;
    UINT32 mps = d->ep0_mps ? d->ep0_mps : 8;
    int n = 0;
    td_fill(uq, n++, d, 0, PID_SETUP, false, UsbPhys(uq->q->setup), 8, false);
    bool tog = true;
    for (UINT32 off = 0; off < s->len; off += mps, tog = !tog) {
        UINT32 chunk = s->len - off > mps ? mps : s->len - off;
        td_fill(uq, n++, d, 0, in ? PID_IN : PID_OUT, tog, UsbPhys(d->buf) + off, chunk, false);
    }
    int status = n;
    td_fill(uq, n++, d, 0, in && s->len ? PID_OUT : PID_IN, true, 0, 0, true);
    uq->q->td[status].ctl |= TD_IOC;
    uq->n = n;
    queue_start(uq, 0);

    UINT64 until = UsbDeadline(5000);
    for (;;) {
        UINT32 moved = 0;
        int last;
        QState st = queue_state(uq, 0, n, &moved, &last);
        if (st == Q_SHORT && last < status) {
            /* A short data packet ends the data stage: on to the status stage */
            if (uq->q->td[status].ctl & TD_ACTIVE) {
                uq->q->qh.elem = p32(&uq->q->td[status]);
                UINT32 m2 = 0;
                int l2;
                for (UINT64 u2 = UsbDeadline(1000); !UsbPast(u2); ) {
                    QState s2 = queue_state(uq, status, n, &m2, &l2);
                    if (s2 == Q_BUSY) { pause_cpu(); continue; }
                    if (s2 != Q_DONE) { queue_stop(uq); return -1; }
                    return (int)(moved - 8);
                }
                queue_stop(uq);
                return -1;
            }
            st = Q_DONE;
        }
        if (st == Q_DONE) return in ? (int)(moved - 8) : s->len;
        if (st == Q_STALLED || st == Q_FAILED) {
            if (st == Q_STALLED) *stalled = true;
            queue_stop(uq);
            return -1;
        }
        if (d->gone || UsbPast(until)) {
            if (!d->gone) kprintf("[USB] %s: control request %02x timed out\n", d->name, s->req);
            queue_stop(uq);
            return -1;
        }
        pause_cpu();
    }
}

static bool uhci_pipe_add(UsbHc *hc, UsbPipe *p)
{
    /* Bulk: a TD per packet of the largest transfer (the pool grows when
     * the pipe's buffer is known); interrupt: one TD */
    UQueue *uq = queue_new(U(hc), p->xfer == 3 ? 2 : 1, p->xfer == 3 ? 4 : 64);
    if (!uq) return false;
    p->hcd = uq;
    return true;
}

static int uhci_bulk(UsbHc *hc, UsbPipe *p, UINT32 len, UINT32 timeout_ms, bool *stalled)
{
    Uhci *u = U(hc);
    UQueue *uq = p->hcd;
    if (!uq) return -1;
    int need = (int)((len + p->mps - 1) / p->mps);
    if (need > uq->slots) {
        /* A bigger pool for this pipe's transfers */
        int want = (int)((p->dma_size + p->mps - 1) / p->mps);
        if (want < need) want = need;
        UQueue *big = queue_new(u, uq->kind, want);
        if (!big) return -1;
        big->toggle = uq->toggle;
        queue_free(u, uq);
        IrqState s = spin_lock_irqsave(&g_usb_lock);
        p->hcd = uq = big;
        spin_unlock_irqrestore(&g_usb_lock, s);
    }
    UsbDev *d = p->dev;
    int n = 0;
    bool tog = uq->toggle;
    for (UINT32 off = 0; off < len; off += p->mps, tog = !tog, n++) {
        UINT32 chunk = len - off > p->mps ? p->mps : len - off;
        td_fill(uq, n, d, p->addr, p->in ? PID_IN : PID_OUT, tog, UsbPhys(p->dma) + off, chunk, off + chunk >= len);
    }
    uq->n = n;
    queue_start(uq, 0);

    UINT64 until = UsbDeadline(timeout_ms);
    for (;;) {
        UINT32 moved = 0;
        int last;
        QState st = queue_state(uq, 0, n, &moved, &last);
        if (st != Q_BUSY) {
            if (last >= 0) uq->toggle = toggle_after(uq, last);
            if (st == Q_SHORT) queue_stop(uq);
            if (st == Q_DONE || st == Q_SHORT) return (int)moved;
            if (st == Q_STALLED) *stalled = true;
            queue_stop(uq);
            return -1;
        }
        if (d->gone || UsbPast(until)) {
            if (!d->gone) kprintf("[USB] %s: bulk transfer timed out\n", d->name);
            queue_stop(uq);
            queue_state(uq, 0, n, &moved, &last);
            if (last >= 0) uq->toggle = toggle_after(uq, last);
            return -1;
        }
        pause_cpu();
    }
}

static bool uhci_listen(UsbHc *hc, UsbPipe *p)
{
    Uhci *u = U(hc);
    UQueue *uq = p->hcd;
    if (!uq) return false;
    int free = -1;
    for (int i = 0; i < MAX_LISTEN; i++) {
        if (u->listening[i] == p) { free = i; break; }
        if (!u->listening[i] && free < 0) free = i;
    }
    if (free < 0) return false;
    u->listening[free] = p;
    UINT32 len = (UINT32)p->listen_len > p->mps ? p->mps : (UINT32)p->listen_len;
    td_fill(uq, 0, p->dev, p->addr, PID_IN, uq->toggle, UsbPhys(p->dma), len, true);
    uq->n = 1;
    queue_start(uq, 0);
    return true;
}

/* Interrupt IN transfers that finished */
static void uhci_poll(UsbHc *hc)
{
    Uhci *u = U(hc);
    for (int i = 0; i < MAX_LISTEN; i++) {
        UsbPipe *p = u->listening[i];
        if (!p || !p->cb || !p->hcd) continue;
        UQueue *uq = p->hcd;
        if (!uq->n) continue;
        UINT32 moved = 0;
        int last;
        QState st = queue_state(uq, 0, 1, &moved, &last);
        if (st == Q_BUSY) continue;
        uq->n = 0;
        uq->q->qh.elem = LP_T;
        if (st != Q_DONE && st != Q_SHORT) {
            UsbPipeListenFailed(p, (int)((uq->q->td[0].ctl >> 16) & 0xFF));
            continue;
        }
        uq->toggle = toggle_after(uq, 0);
        if (UsbPipeDeliver(p, (int)moved)) uhci_listen(hc, p);
    }
}

static bool uhci_pipe_reset(UsbHc *hc, UsbPipe *p)
{
    (void)hc;
    UQueue *uq = p->hcd;
    if (!uq) return false;
    uq->q->qh.elem = LP_T;
    uq->toggle = false;                   /* DATA0 */
    return true;
}

static void uhci_dev_remove(UsbHc *hc, UsbDev *d)
{
    Uhci *u = U(hc);
    for (int i = 0; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (!p || !p->hcd) continue;
        UQueue *uq = p->hcd;
        IrqState s = spin_lock_irqsave(&g_usb_lock);
        for (int k = 0; k < MAX_LISTEN; k++) if (u->listening[k] == p) u->listening[k] = NULL;
        p->hcd = NULL;
        spin_unlock_irqrestore(&g_usb_lock, s);
        queue_free(u, uq);
    }
    UDev *ud = d->hcd;
    if (ud) {
        queue_free(u, ud->ep0);
        kfree(ud);
        d->hcd = NULL;
    }
}

/* ---- root ports ---- */

static UINT32 uhci_port_status(UsbHc *hc, int port)
{
    Uhci *u = U(hc);
    UINT16 v = inw((UINT16)(u->io + REG_PORTSC(port)));
    UINT32 st = (v & PORT_CCS) ? USB_PORT_CONNECTED : 0;
    if (v & PORT_CSC) {
        outw((UINT16)(u->io + REG_PORTSC(port)), (UINT16)((v & ~PORT_W1C) | PORT_CSC));
        st |= USB_PORT_CHANGED;
    }
    return st;
}

static UINT8 uhci_port_reset(UsbHc *hc, int port)
{
    Uhci *u = U(hc);
    UINT16 r = (UINT16)(u->io + REG_PORTSC(port));
    if (!(inw(r) & PORT_CCS)) return 0;
    outw(r, (UINT16)((inw(r) & ~PORT_W1C) | PORT_PR));
    UsbDelay(50);
    outw(r, (UINT16)(inw(r) & ~(PORT_W1C | PORT_PR)));
    udelay(50);
    /* Enable the port; it may take a few tries while the device settles */
    for (int tries = 0; tries < 10; tries++) {
        UINT16 v = inw(r);
        if (!(v & PORT_CCS)) return 0;
        if (v & (PORT_CSC | PORT_PEC)) { outw(r, (UINT16)((v & ~PORT_PR) | PORT_W1C)); continue; }
        if (v & PORT_PE) break;
        outw(r, (UINT16)((v & ~PORT_W1C) | PORT_PE));
        UsbDelay(10);
    }
    UINT16 v = inw(r);
    if ((v & (PORT_CCS | PORT_PE)) != (PORT_CCS | PORT_PE)) return 0;
    UsbDelay(10);                          /* reset recovery */
    return (v & PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* ---- controller bring-up ---- */

static bool controller_program(Uhci *u)
{
    const PciDevice *p = &u->pci;
    /* Legacy keyboard emulation and SMIs off (USBLEGSUP) */
    PciWrite16(p->bus, p->dev, p->func, 0xC0, 0x8F00);

    outw((UINT16)(u->io + REG_USBINTR), 0);
    outw((UINT16)(u->io + REG_USBCMD), 0);
    UsbDelay(2);
    outw((UINT16)(u->io + REG_USBCMD), CMD_GRESET);
    UsbDelay(20);
    outw((UINT16)(u->io + REG_USBCMD), 0);
    UsbDelay(2);
    outw((UINT16)(u->io + REG_USBCMD), CMD_HCRESET);
    UINT64 until = UsbDeadline(50);
    while ((inw((UINT16)(u->io + REG_USBCMD)) & CMD_HCRESET) && !UsbPast(until)) pause_cpu();
    if (inw((UINT16)(u->io + REG_USBCMD)) & CMD_HCRESET) return false;

    /* Skeleton: every frame → interrupt → control → bulk → end */
    Qh *si = skel(u, 0), *sc = skel(u, 1), *sb = skel(u, 2);
    si->head = p32(sc) | LP_QH;
    sc->head = p32(sb) | LP_QH;
    sb->head = LP_T;
    si->elem = sc->elem = sb->elem = LP_T;
    for (int i = 0; i < 1024; i++) u->frames[i] = p32(si) | LP_QH;

    outw((UINT16)(u->io + REG_USBSTS), 0x3F);
    outw((UINT16)(u->io + REG_FRNUM), 0);
    outl((UINT16)(u->io + REG_FRBASEADD), p32(u->frames));
    outb((UINT16)(u->io + REG_SOFMOD), 64);
    outw((UINT16)(u->io + REG_USBCMD), CMD_RS | CMD_CF | CMD_MAXP);
    until = UsbDeadline(20);
    while ((inw((UINT16)(u->io + REG_USBSTS)) & STS_HCHALTED) && !UsbPast(until)) pause_cpu();
    return !(inw((UINT16)(u->io + REG_USBSTS)) & STS_HCHALTED);
}

static bool uhci_resume(UsbHc *hc)
{
    Uhci *u = U(hc);
    for (int i = 0; i < MAX_LISTEN; i++) u->listening[i] = NULL;
    return controller_program(u);
}

static const UsbHcOps g_uhci_ops = {
    .kind        = "UHCI",
    .port_status = uhci_port_status,
    .port_reset  = uhci_port_reset,
    .pipe_add    = uhci_pipe_add,
    .dev_remove  = uhci_dev_remove,
    .control     = uhci_control,
    .bulk        = uhci_bulk,
    .listen      = uhci_listen,
    .pipe_reset  = uhci_pipe_reset,
    .poll        = uhci_poll,
    .resume      = uhci_resume,
};

bool UhciProbe(const PciDevice *pci)
{
    UINT32 bar = PciRead32(pci->bus, pci->dev, pci->func, 0x20);        /* BAR 4: I/O ports */
    if (!(bar & 1) || !(bar & ~3u)) return false;
    Uhci *u = kzalloc(sizeof(Uhci));
    if (!u) return false;
    u->pci = *pci;
    u->io = (UINT16)(bar & ~3u);
    PciWrite16(pci->bus, pci->dev, pci->func, 0x04,
               (UINT16)(PciRead16(pci->bus, pci->dev, pci->func, 0x04) | 0x5));   /* I/O space, bus master */
    /* Ports: as many as answer like ports (bit 7 always reads 1) */
    while (u->ports < 8) {
        UINT16 v = inw((UINT16)(u->io + REG_PORTSC(u->ports + 1)));
        if (!(v & PORT_ALWAYS1) || v == 0xFFFF) break;
        u->ports++;
    }
    u->frames = UsbDmaAlloc(1);
    u->skel = UsbDmaAlloc(1);
    if (!u->ports || !u->frames || !u->skel || !controller_program(u)) {
        kprintf("[USB] UHCI controller %02x:%02x.%d failed to start\n", pci->bus, pci->dev, pci->func);
        return false;          /* (its memory stays: the controller may still point at it) */
    }
    u->hc = UsbAddController(&g_uhci_ops, u, u->ports);
    if (!u->hc) return false;
    kprintf("[USB] bus %d: UHCI %02x:%02x.%d, I/O %04x, %d ports\n",
            u->hc->num, pci->bus, pci->dev, pci->func, u->io, u->ports);
    return true;
}
