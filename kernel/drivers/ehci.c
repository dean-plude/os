/*
 * ehci.c — USB 2 (EHCI) host controller driver
 *
 * Follows the Enhanced Host Controller Interface 1.0 specification.  Each
 * endpoint has a queue head; control and bulk queue heads sit on the
 * asynchronous schedule (a ring behind a dummy head), interrupt ones on
 * the periodic schedule, where every one of the 1024 frames points at the
 * same list, so each interrupt endpoint is polled once a millisecond.
 *
 * A transfer is a short chain of qTDs (up to 20 KiB each) handed to an
 * idle queue head by writing its overlay's next pointer.  A short packet
 * on a bulk or interrupt IN ends the transfer: the qTDs' alternate pointer
 * leads to an inactive "stop" qTD, which leaves the queue head idle.  The
 * controller keeps bulk and interrupt data toggles in the queue head;
 * control transfers carry theirs in the qTDs.
 *
 * Full- and low-speed devices on a root port belong to the companion
 * controller (UHCI or OHCI) that shares the port: they are recognised at
 * reset and the port is passed over.  Behind a high-speed hub they are
 * reached with split transactions through the hub's transaction
 * translator.
 *
 * Polled: the controller's interrupts stay off; poll() looks at the
 * interrupt qTDs every tick, and a waited-for transfer is spun on.
 */

#include "usb_hc.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* Capability registers */
#define CAP_LENGTH      0x00
#define CAP_HCSPARAMS   0x04
#define CAP_HCCPARAMS   0x08

/* Operational registers */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_USBINTR      0x08
#define OP_FRINDEX      0x0C
#define OP_CTRLDSSEG    0x10
#define OP_PERIODIC     0x14
#define OP_ASYNC        0x18
#define OP_CONFIGFLAG   0x40
#define OP_PORTSC(p)    (0x44 + 4 * ((p) - 1))

#define CMD_RS          (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_PSE         (1u << 4)
#define CMD_ASE         (1u << 5)
#define CMD_IAAD        (1u << 6)
#define CMD_ITC_8       (8u << 16)
#define STS_IAA         (1u << 5)
#define STS_HCHALTED    (1u << 12)

/* PORTSC */
#define PORT_CCS        (1u << 0)
#define PORT_CSC        (1u << 1)
#define PORT_PE         (1u << 2)
#define PORT_PEC        (1u << 3)
#define PORT_OCC        (1u << 5)
#define PORT_PR         (1u << 8)
#define PORT_LS(v)      (((v) >> 10) & 3)
#define PORT_PP         (1u << 12)
#define PORT_OWNER      (1u << 13)
#define PORT_W1C        (PORT_CSC | PORT_PEC | PORT_OCC)

/* Link pointers */
#define LP_T            1u
#define LP_QH           (1u << 1)

/* qTD token */
#define TOK_ACTIVE      (1u << 7)
#define TOK_HALTED      (1u << 6)
#define TOK_BUFERR      (1u << 5)
#define TOK_BABBLE      (1u << 4)
#define TOK_XACT        (1u << 3)
#define TOK_PID_OUT     (0u << 8)
#define TOK_PID_IN      (1u << 8)
#define TOK_PID_SETUP   (2u << 8)
#define TOK_CERR3       (3u << 10)
#define TOK_IOC         (1u << 15)
#define TOK_BYTES(t)    (((t) >> 16) & 0x7FFF)
#define TOK_TOGGLE      (1u << 31)

/* Queue head dword 1 (endpoint characteristics) and 2 (capabilities) */
#define QH_EPS_FULL     (0u << 12)
#define QH_EPS_LOW      (1u << 12)
#define QH_EPS_HIGH     (2u << 12)
#define QH_DTC          (1u << 14)
#define QH_HEAD         (1u << 15)
#define QH_CONTROL      (1u << 27)
#define QH_MULT1        (1u << 30)

#define QTD_BYTES       0x5000u           /* 5 pages per qTD, page-aligned buffers */
#define QTD_SLOTS       60                /* qTDs in a queue's page */
#define MAX_LISTEN      64                /* interrupt IN pipes per controller */

typedef struct __attribute__((packed, aligned(32))) {
    UINT32 next, alt, token, buf[5], ext[5];
    UINT32 pad[3];
} Qtd;                                    /* 64 bytes */

typedef struct __attribute__((packed, aligned(32))) {
    UINT32 link, ep_char, ep_caps, cur;
    struct __attribute__((packed)) {
        UINT32 next, alt, token, buf[5], ext[5];
    } ov;                                 /* the overlay: a qTD's fields */
} Qh;

/* One page per queue: the queue head, a stop qTD, a setup packet and the qTDs */
typedef struct {
    Qh          qh;                       /* (at offset 0, 96 bytes) */
    UINT8       pad[32];
    Qtd         stop;
    UINT8       setup[64];
    Qtd         td[QTD_SLOTS];
} QPage;

typedef struct {
    QPage      *q;
    bool        periodic;
    bool        linked;
    int         ntd;                      /* qTDs of the transfer in flight (interrupt) */
} EQueue;

typedef struct {
    UsbHc            *hc;
    volatile UINT8   *cap, *op;
    int               ports;
    bool              dma64;
    UINT32           *frames;             /* the periodic frame list */
    QPage            *async_head;         /* the asynchronous ring's dummy head */
    QPage            *intr_head;          /* the periodic list's dummy head */
    UsbPipe          *listening[MAX_LISTEN];
    PciDevice         pci;
} Ehci;

_Static_assert(sizeof(QPage) <= 4096, "EHCI queue page");

static inline UINT32 rd32(volatile UINT8 *b, UINT32 r)          { return *(volatile UINT32 *)(b + r); }
static inline void   wr32(volatile UINT8 *b, UINT32 r, UINT32 v) { *(volatile UINT32 *)(b + r) = v; }
static inline UINT32 p32(const volatile void *va) { return (UINT32)UsbPhys(va); }

static inline Ehci *E(UsbHc *hc) { return hc->priv; }

/* ---- queues ---- */

static QPage *queue_page(void)
{
    QPage *q = UsbDmaAlloc(1);
    if (!q) return NULL;
    q->qh.link = LP_T;
    q->qh.ov.next = LP_T;
    q->qh.ov.alt = LP_T;
    q->stop.next = LP_T;
    q->stop.alt = LP_T;
    q->stop.token = 0;                    /* inactive */
    return q;
}

/* Wait for the controller to let go of anything it read from the
 * asynchronous schedule (after taking a queue head off it) */
static void async_advance(Ehci *e)
{
    if (rd32(e->op, OP_USBSTS) & STS_HCHALTED) return;
    wr32(e->op, OP_USBSTS, STS_IAA);
    wr32(e->op, OP_USBCMD, rd32(e->op, OP_USBCMD) | CMD_IAAD);
    UINT64 until = UsbDeadline(20);
    while (!(rd32(e->op, OP_USBSTS) & STS_IAA) && !UsbPast(until)) pause_cpu();
    wr32(e->op, OP_USBSTS, STS_IAA);
}

static void queue_link(Ehci *e, EQueue *eq)
{
    QPage *head = eq->periodic ? e->intr_head : e->async_head;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    eq->q->qh.link = head->qh.link;
    mfence();
    head->qh.link = p32(&eq->q->qh) | LP_QH;
    eq->linked = true;
    spin_unlock_irqrestore(&g_usb_lock, s);
}

static void queue_unlink(Ehci *e, EQueue *eq)
{
    if (!eq->linked) return;
    QPage *head = eq->periodic ? e->intr_head : e->async_head;
    UINT32 me = p32(&eq->q->qh) | LP_QH;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    eq->linked = false;
    /* (the asynchronous ring comes back round to its head; the periodic list ends) */
    Qh *prev = &head->qh;
    for (int guard = 0; guard < 1024; guard++) {
        if (prev->link == me) { prev->link = eq->q->qh.link; break; }
        if ((prev->link & LP_T) || (prev->link & ~0x1Fu) == p32(&head->qh)) break;
        prev = UsbVirt(prev->link & ~0x1Fu);
    }
    spin_unlock_irqrestore(&g_usb_lock, s);
    if (eq->periodic) UsbDelay(2);        /* (a frame or two: the controller is past it) */
    else async_advance(e);
}

/* Characteristics of a queue head for endpoint @ep of @d */
static void queue_chars(UsbDev *d, UINT8 ep, UINT16 mps, bool control, bool periodic, UINT32 *ch, UINT32 *caps)
{
    UINT32 eps = d->speed == USB_SPEED_HIGH ? QH_EPS_HIGH : d->speed == USB_SPEED_LOW ? QH_EPS_LOW : QH_EPS_FULL;
    *ch = (UINT32)d->addr | ((UINT32)(ep & 0xF) << 8) | eps | ((UINT32)mps << 16) |
          (control ? QH_DTC : 0) | (control && d->speed != USB_SPEED_HIGH ? QH_CONTROL : 0);
    *caps = QH_MULT1;
    if (d->speed != USB_SPEED_HIGH && d->tt_hub)            /* split transactions */
        *caps |= ((UINT32)d->tt_hub->addr << 16) | ((UINT32)d->tt_port << 23);
    if (periodic)
        *caps |= d->speed == USB_SPEED_HIGH ? 0x01u : (0x01u | (0x1Cu << 8));   /* S-mask, C-mask */
}

static EQueue *queue_new(Ehci *e, UsbDev *d, UINT8 ep, UINT16 mps, bool control, bool periodic)
{
    EQueue *eq = kzalloc(sizeof(EQueue));
    if (!eq) return NULL;
    eq->q = queue_page();
    if (!eq->q) { kfree(eq); return NULL; }
    eq->periodic = periodic;
    queue_chars(d, ep, mps, control, periodic, &eq->q->qh.ep_char, &eq->q->qh.ep_caps);
    queue_link(e, eq);
    return eq;
}

static void queue_free(Ehci *e, EQueue *eq)
{
    if (!eq) return;
    queue_unlink(e, eq);
    UsbDmaFree(eq->q, 1);
    kfree(eq);
}

/* Fill @td for @len bytes at physical @pa (page-aligned unless short) */
static void qtd_fill(Qtd *td, UINT32 next, UINT32 alt, UINT32 token, UINT64 pa, UINT32 len)
{
    td->next = next;
    td->alt = alt;
    td->buf[0] = (UINT32)pa;
    for (int i = 1; i < 5; i++) td->buf[i] = (UINT32)((pa & ~0xFFFull) + (UINT64)i * PAGE_SIZE);
    for (int i = 0; i < 5; i++) td->ext[i] = 0;
    td->token = token | ((UINT32)len << 16) | TOK_CERR3 | TOK_ACTIVE;
}

/* Hand the qTD chain at @first to the idle queue */
static void queue_start(EQueue *eq, Qtd *first)
{
    Qh *qh = &eq->q->qh;
    qh->ov.next = p32(first);
    qh->ov.alt = LP_T;
    mfence();
    qh->ov.token &= TOK_TOGGLE;           /* inactive, not halted: the controller follows next */
    mfence();
}

/* Stop a queue that is still busy (a timeout): take it off the schedule,
 * clear it and put it back */
static void queue_cancel(Ehci *e, EQueue *eq, int ntd)
{
    for (int i = 0; i < ntd; i++) eq->q->td[i].token &= ~TOK_ACTIVE;
    queue_unlink(e, eq);
    eq->q->qh.ov.next = LP_T;
    eq->q->qh.ov.token &= TOK_TOGGLE;
    queue_link(e, eq);
}

typedef enum { Q_BUSY, Q_DONE, Q_FAILED, Q_STALLED } QState;

/* How the transfer in qTDs 0..@n-1 stands; *@moved: bytes so far */
static QState queue_state(EQueue *eq, int n, const UINT32 *lens, bool short_ends, UINT32 *moved)
{
    UINT32 m = 0;
    for (int i = 0; i < n; i++) {
        UINT32 t = eq->q->td[i].token;
        if (t & TOK_HALTED) {
            *moved = m;
            return (t & (TOK_BUFERR | TOK_BABBLE | TOK_XACT)) ? Q_FAILED : Q_STALLED;
        }
        if (t & TOK_ACTIVE) { *moved = m; return Q_BUSY; }
        UINT32 left = TOK_BYTES(t);
        m += lens[i] > left ? lens[i] - left : 0;
        if (left && short_ends) { *moved = m; return Q_DONE; }
    }
    *moved = m;
    return Q_DONE;
}

/* ---- devices and transfers ---- */

typedef struct {
    EQueue *ep0;
    UINT8   addr;
    UINT16  mps;
} EDev;

static EDev *edev(Ehci *e, UsbDev *d)
{
    EDev *ed = d->hcd;
    if (ed) return ed;
    ed = kzalloc(sizeof(EDev));
    if (!ed) return NULL;
    ed->ep0 = queue_new(e, d, 0, d->ep0_mps, true, false);
    if (!ed->ep0) { kfree(ed); return NULL; }
    ed->addr = d->addr;
    ed->mps = d->ep0_mps;
    d->hcd = ed;
    return ed;
}

static int ehci_control(UsbHc *hc, UsbDev *d, const UsbSetup *s, bool *stalled)
{
    Ehci *e = E(hc);
    EDev *ed = edev(e, d);
    if (!ed || d->gone) return -1;
    EQueue *eq = ed->ep0;
    if (ed->addr != d->addr || ed->mps != d->ep0_mps) {     /* (after SET_ADDRESS) */
        queue_unlink(e, eq);
        queue_chars(d, 0, d->ep0_mps, true, false, &eq->q->qh.ep_char, &eq->q->qh.ep_caps);
        queue_link(e, eq);
        ed->addr = d->addr;
        ed->mps = d->ep0_mps;
    }
    QPage *q = eq->q;
    bool in = (s->type & 0x80) != 0;
    memcpy(q->setup, s, 8);
    int n = 0;
    UINT32 lens[3];
    Qtd *status = &q->td[s->len ? 2 : 1];
    if (s->len) {
        qtd_fill(&q->td[1], p32(status), LP_T, (in ? TOK_PID_IN : TOK_PID_OUT) | TOK_TOGGLE, UsbPhys(d->buf), s->len);
    }
    qtd_fill(&q->td[0], p32(&q->td[1]), LP_T, TOK_PID_SETUP, UsbPhys(q->setup), 8);
    lens[n++] = 8;
    if (s->len) lens[n++] = s->len;
    qtd_fill(status, LP_T, LP_T, (in && s->len ? TOK_PID_OUT : TOK_PID_IN) | TOK_TOGGLE | TOK_IOC, 0, 0);
    lens[n++] = 0;
    queue_start(eq, &q->td[0]);

    UINT64 until = UsbDeadline(5000);
    UINT32 moved = 0;
    for (;;) {
        QState st = queue_state(eq, n, lens, false, &moved);
        if (st == Q_DONE) return in ? (int)(moved - 8) : s->len;
        if (st == Q_STALLED) { *stalled = true; return -1; }
        if (st == Q_FAILED) return -1;
        if (d->gone || UsbPast(until)) {
            if (!d->gone) kprintf("[USB] %s: control request %02x timed out\n", d->name, s->req);
            queue_cancel(e, eq, n);
            return -1;
        }
        pause_cpu();
    }
}

static bool ehci_pipe_add(UsbHc *hc, UsbPipe *p)
{
    EQueue *eq = queue_new(E(hc), p->dev, p->addr, p->mps, false, p->xfer == 3);
    if (!eq) return false;
    p->hcd = eq;
    return true;
}

static int ehci_bulk(UsbHc *hc, UsbPipe *p, UINT32 len, UINT32 timeout_ms, bool *stalled)
{
    Ehci *e = E(hc);
    EQueue *eq = p->hcd;
    if (!eq) return -1;
    QPage *q = eq->q;
    UINT32 lens[QTD_SLOTS];
    int n = 0;
    UINT32 stop = p32(&q->stop);
    for (UINT32 off = 0; off < len && n < QTD_SLOTS; n++) {
        UINT32 chunk = len - off > QTD_BYTES ? QTD_BYTES : len - off;
        lens[n] = chunk;
        off += chunk;
        bool last = off >= len || n + 1 == QTD_SLOTS;
        qtd_fill(&q->td[n], last ? LP_T : p32(&q->td[n + 1]), p->in ? stop : LP_T,
                 (p->in ? TOK_PID_IN : TOK_PID_OUT) | (last ? TOK_IOC : 0), UsbPhys(p->dma) + off - chunk, chunk);
    }
    queue_start(eq, &q->td[0]);

    UINT64 until = UsbDeadline(timeout_ms);
    UINT32 moved = 0;
    for (;;) {
        QState st = queue_state(eq, n, lens, p->in, &moved);
        if (st == Q_DONE) return (int)moved;
        if (st == Q_STALLED) { *stalled = true; return -1; }
        if (st == Q_FAILED) return -1;
        if (p->dev->gone || UsbPast(until)) {
            if (!p->dev->gone) kprintf("[USB] %s: bulk transfer timed out\n", p->dev->name);
            queue_cancel(e, eq, n);
            return -1;
        }
        pause_cpu();
    }
}

static bool ehci_listen(UsbHc *hc, UsbPipe *p)
{
    Ehci *e = E(hc);
    EQueue *eq = p->hcd;
    if (!eq) return false;
    int free = -1;
    for (int i = 0; i < MAX_LISTEN; i++) {
        if (e->listening[i] == p) { free = i; break; }
        if (!e->listening[i] && free < 0) free = i;
    }
    if (free < 0) return false;
    e->listening[free] = p;
    QPage *q = eq->q;
    int len = p->listen_len > (int)QTD_BYTES ? (int)QTD_BYTES : p->listen_len;
    qtd_fill(&q->td[0], LP_T, p32(&q->stop), TOK_PID_IN, UsbPhys(p->dma), (UINT32)len);
    eq->ntd = 1;
    queue_start(eq, &q->td[0]);
    return true;
}

/* Interrupt IN transfers that finished */
static void ehci_poll(UsbHc *hc)
{
    Ehci *e = E(hc);
    for (int i = 0; i < MAX_LISTEN; i++) {
        UsbPipe *p = e->listening[i];
        if (!p || !p->cb || !p->hcd) continue;
        EQueue *eq = p->hcd;
        if (!eq->ntd) continue;
        UINT32 t = eq->q->td[0].token;
        if (t & TOK_ACTIVE) continue;
        eq->ntd = 0;
        if (t & TOK_HALTED) { UsbPipeListenFailed(p, (int)(t & 0xFF)); continue; }
        int len = p->listen_len > (int)QTD_BYTES ? (int)QTD_BYTES : p->listen_len;
        if (UsbPipeDeliver(p, len - (int)TOK_BYTES(t))) ehci_listen(hc, p);
    }
}

static bool ehci_pipe_reset(UsbHc *hc, UsbPipe *p)
{
    (void)hc;
    EQueue *eq = p->hcd;
    if (!eq) return false;
    eq->q->qh.ov.next = LP_T;
    eq->q->qh.ov.token = 0;               /* not halted, DATA0 */
    return true;
}

static void ehci_dev_remove(UsbHc *hc, UsbDev *d)
{
    Ehci *e = E(hc);
    for (int i = 0; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (!p || !p->hcd) continue;
        EQueue *eq = p->hcd;
        IrqState s = spin_lock_irqsave(&g_usb_lock);
        for (int k = 0; k < MAX_LISTEN; k++) if (e->listening[k] == p) e->listening[k] = NULL;
        p->hcd = NULL;
        spin_unlock_irqrestore(&g_usb_lock, s);
        queue_free(e, eq);
    }
    EDev *ed = d->hcd;
    if (ed) {
        queue_free(e, ed->ep0);
        kfree(ed);
        d->hcd = NULL;
    }
}

/* ---- root ports ---- */

static UINT32 ehci_port_status(UsbHc *hc, int port)
{
    Ehci *e = E(hc);
    UINT32 sc = rd32(e->op, OP_PORTSC(port));
    UINT32 st = 0;
    if (sc & PORT_CSC) {
        wr32(e->op, OP_PORTSC(port), (sc & ~PORT_W1C) | PORT_CSC);
        st |= USB_PORT_CHANGED;
    }
    if ((sc & PORT_CCS) && !(sc & PORT_OWNER)) st |= USB_PORT_CONNECTED;
    return st;
}

static void pass_to_companion(Ehci *e, int port, const char *why)
{
    UINT32 sc = rd32(e->op, OP_PORTSC(port));
    wr32(e->op, OP_PORTSC(port), (sc & ~PORT_W1C) | PORT_OWNER);
    char name[32];
    UsbPortName(e->hc, port, name, sizeof(name));
    kprintf("[USB] %s: %s device, passed to the companion controller\n", name, why);
}

static UINT8 ehci_port_reset(UsbHc *hc, int port)
{
    Ehci *e = E(hc);
    UINT32 sc = rd32(e->op, OP_PORTSC(port));
    if (!(sc & PORT_CCS) || (sc & PORT_OWNER)) return 0;
    if (PORT_LS(sc) == 1) {               /* K state: a low-speed device */
        pass_to_companion(e, port, "low-speed");
        return 0;
    }
    wr32(e->op, OP_PORTSC(port), (sc & ~(PORT_W1C | PORT_PE)) | PORT_PR);
    UsbDelay(50);
    sc = rd32(e->op, OP_PORTSC(port));
    wr32(e->op, OP_PORTSC(port), sc & ~(PORT_W1C | PORT_PR));
    UINT64 until = UsbDeadline(20);
    while ((rd32(e->op, OP_PORTSC(port)) & PORT_PR) && !UsbPast(until)) pause_cpu();
    UsbDelay(2);
    sc = rd32(e->op, OP_PORTSC(port));
    if (!(sc & PORT_CCS)) return 0;
    if (!(sc & PORT_PE)) {                /* not high speed: a full-speed device */
        pass_to_companion(e, port, "full-speed");
        return 0;
    }
    UsbDelay(10);                          /* reset recovery */
    return USB_SPEED_HIGH;
}

/* ---- controller bring-up ---- */

/* Take the controller from the firmware (USB legacy support, in PCI
 * configuration space at EECP) */
static void bios_handoff(Ehci *e)
{
    const PciDevice *p = &e->pci;
    UINT32 eecp = (rd32(e->cap, CAP_HCCPARAMS) >> 8) & 0xFF;
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        UINT32 v = PciRead32(p->bus, p->dev, p->func, (UINT8)eecp);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                PciWrite32(p->bus, p->dev, p->func, (UINT8)eecp, v | (1u << 24));     /* OS owned */
                UINT64 until = UsbDeadline(1000);
                while ((PciRead32(p->bus, p->dev, p->func, (UINT8)eecp) & (1u << 16)) && !UsbPast(until))
                    pause_cpu();
            }
            PciWrite32(p->bus, p->dev, p->func, (UINT8)(eecp + 4), 0);            /* no SMIs */
            return;
        }
        eecp = (v >> 8) & 0xFF;
    }
}

static bool controller_program(Ehci *e)
{
    bios_handoff(e);
    wr32(e->op, OP_USBINTR, 0);
    wr32(e->op, OP_USBCMD, rd32(e->op, OP_USBCMD) & ~CMD_RS);
    UINT64 until = UsbDeadline(50);
    while (!(rd32(e->op, OP_USBSTS) & STS_HCHALTED) && !UsbPast(until)) pause_cpu();
    wr32(e->op, OP_USBCMD, CMD_HCRESET);
    until = UsbDeadline(250);
    while ((rd32(e->op, OP_USBCMD) & CMD_HCRESET) && !UsbPast(until)) pause_cpu();
    if (rd32(e->op, OP_USBCMD) & CMD_HCRESET) return false;

    /* The schedules: an empty asynchronous ring, every frame at the
     * (empty) interrupt list */
    e->async_head->qh.link = p32(&e->async_head->qh) | LP_QH;
    e->async_head->qh.ep_char = QH_HEAD | QH_EPS_HIGH;
    e->async_head->qh.ov.token = TOK_HALTED;
    e->intr_head->qh.link = LP_T;
    e->intr_head->qh.ep_char = QH_EPS_HIGH;
    e->intr_head->qh.ep_caps = QH_MULT1 | 0x01;
    e->intr_head->qh.ov.token = TOK_HALTED;
    for (int i = 0; i < 1024; i++) e->frames[i] = p32(&e->intr_head->qh) | LP_QH;

    if (e->dma64) wr32(e->op, OP_CTRLDSSEG, 0);
    wr32(e->op, OP_FRINDEX, 0);
    wr32(e->op, OP_PERIODIC, p32(e->frames));
    wr32(e->op, OP_ASYNC, p32(&e->async_head->qh));
    wr32(e->op, OP_USBSTS, 0x3F);
    wr32(e->op, OP_USBCMD, CMD_ITC_8 | CMD_PSE | CMD_ASE | CMD_RS);
    wr32(e->op, OP_CONFIGFLAG, 1);        /* every port to EHCI (high-speed devices stay) */
    until = UsbDeadline(50);
    while ((rd32(e->op, OP_USBSTS) & STS_HCHALTED) && !UsbPast(until)) pause_cpu();
    if (rd32(e->op, OP_USBSTS) & STS_HCHALTED) return false;

    if (rd32(e->cap, CAP_HCSPARAMS) & (1u << 4))           /* port power control */
        for (int p = 1; p <= e->ports; p++) {
            UINT32 sc = rd32(e->op, OP_PORTSC(p));
            wr32(e->op, OP_PORTSC(p), (sc & ~PORT_W1C) | PORT_PP);
        }
    return true;
}

static bool ehci_resume(UsbHc *hc)
{
    return controller_program(E(hc));
}

static const UsbHcOps g_ehci_ops = {
    .kind        = "EHCI",
    .port_status = ehci_port_status,
    .port_reset  = ehci_port_reset,
    .pipe_add    = ehci_pipe_add,
    .dev_remove  = ehci_dev_remove,
    .control     = ehci_control,
    .bulk        = ehci_bulk,
    .listen      = ehci_listen,
    .pipe_reset  = ehci_pipe_reset,
    .poll        = ehci_poll,
    .resume      = ehci_resume,
};

bool EhciProbe(const PciDevice *pci)
{
    Ehci *e = kzalloc(sizeof(Ehci));
    if (!e) return false;
    e->pci = *pci;
    e->cap = PciMapBar(pci, 0);
    if (!e->cap) { kfree(e); return false; }
    PciEnableDevice(pci);
    e->op = e->cap + (rd32(e->cap, CAP_LENGTH) & 0xFF);
    UINT32 hcs = rd32(e->cap, CAP_HCSPARAMS);
    e->ports = (int)(hcs & 0xF);
    e->dma64 = (rd32(e->cap, CAP_HCCPARAMS) & 1) != 0;
    e->frames = UsbDmaAlloc(1);
    e->async_head = queue_page();
    e->intr_head = queue_page();
    if (!e->frames || !e->async_head || !e->intr_head || !controller_program(e)) {
        kprintf("[USB] EHCI controller %02x:%02x.%d failed to start\n", pci->bus, pci->dev, pci->func);
        return false;          /* (its memory stays: the controller may still point at it) */
    }
    e->hc = UsbAddController(&g_ehci_ops, e, e->ports);
    if (!e->hc) return false;
    kprintf("[USB] bus %d: EHCI %02x:%02x.%d, %d ports, %d companion controller(s)\n",
            e->hc->num, pci->bus, pci->dev, pci->func, e->ports, (int)((hcs >> 12) & 0xF));
    return true;
}
