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
 * Isochronous endpoints of high-speed devices use iTDs, which go straight
 * into the frame list as UHCI's isochronous TDs do: the iTD for frame f
 * goes in front of whatever entry f holds (other iTDs, then the interrupt
 * list) and comes out again once the frame has passed.  An iTD carries up
 * to eight transactions, one per microframe, so a pipe polled every
 * microframe puts eight packets in each, one polled every 2^n frames one.
 * Each pipe has an iTD per frame of the core's transfer ring, at a fixed
 * place.  Full-speed isochronous endpoints behind a high-speed hub would
 * need siTDs, which are not written: their pipes are refused (on a root
 * port such a device goes to the companion controller, which streams).
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
#define LP_TYPE         (3u << 1)         /* 0: an iTD */

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

/* iTD transaction status and control, buffer pointer fields */
#define ITD_ACTIVE      (1u << 31)
#define ITD_ERRORS      (7u << 28)        /* data buffer error, babble, transaction error */
#define ITD_LEN(t)      (((t) >> 16) & 0xFFF)
#define ITD_IN          (1u << 11)

#define QTD_BYTES       0x5000u           /* 5 pages per qTD, page-aligned buffers */
#define QTD_SLOTS       60                /* qTDs in a queue's page */
#define MAX_LISTEN      64                /* interrupt IN pipes per controller */
#define MAX_ISO         16                /* isochronous pipes streaming per controller */
#define ITD_SLOT        128               /* bytes per iTD (64, then the 64-bit buffer pointers) */
#define ISO_ITDS        128               /* iTDs per pipe: the core's 128 packets at most */
#define ISO_PAGES       (ISO_ITDS * ITD_SLOT / 4096)

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

/* An isochronous transfer descriptor: one frame's transactions */
typedef struct __attribute__((packed, aligned(32))) {
    UINT32 next, status[8], buf[7], ext[7];
} Itd;

/* An isochronous pipe: an iTD per frame of the transfer ring (transfer k's
 * at k * frames), and the transfers in flight, oldest first */
typedef struct {
    UINT8  *itd;                          /* ITD_SLOT bytes each, ISO_ITDS of them */
    UINT16  at[ISO_ITDS];                 /* the frame (11 bits) each iTD was put in */
    int     frames;                       /* iTDs per transfer */
    int     per;                          /* packets per iTD (1, 2, 4 or 8) */
    UINT16  step;                         /* frames from one iTD to the next */
    UINT16  frame;                        /* where the next transfer's first iTD goes */
    int     fifo[128], head, count;
} EIso;

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
    UsbPipe          *streaming[MAX_ISO];
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

static bool iso_add(UsbPipe *p);

static bool ehci_pipe_add(UsbHc *hc, UsbPipe *p)
{
    if (p->xfer == 1) return iso_add(p);
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

/* ---- isochronous ---- */

static inline Itd *itd_at(EIso *ei, int i) { return (Itd *)(ei->itd + (size_t)i * ITD_SLOT); }
static inline UINT16 frame_now(Ehci *e) { return (UINT16)((rd32(e->op, OP_FRINDEX) >> 3) & 0x7FF); }
/* Frame @f (11 bits) is over */
static inline bool frame_past(UINT16 now, UINT16 f) { UINT16 d = (UINT16)((now - f) & 0x7FF); return d && d < 0x400; }

/* An isochronous pipe's state: high speed only (siTDs are not written) */
static bool iso_add(UsbPipe *p)
{
    if (p->dev->speed != USB_SPEED_HIGH) return false;
    EIso *ei = kzalloc(sizeof(EIso));
    if (!ei) return false;
    ei->itd = UsbDmaAlloc(ISO_PAGES);
    if (!ei->itd) { kfree(ei); return false; }
    UINT32 period = UsbPipePeriod(p);
    ei->per = period >= 8 ? 1 : (int)(8 / period);
    ei->step = (UINT16)(period >= 8 ? period / 8 : 1);
    p->hcd = ei;
    return true;
}

/* Take an iTD out of its frame's list (lock held) */
static void itd_unlink(Ehci *e, EIso *ei, int i)
{
    Itd *td = itd_at(ei, i);
    UINT32 me = p32(td);
    volatile UINT32 *at = &e->frames[ei->at[i] & 1023];
    for (int guard = 0; guard < 256 && !(*at & (LP_T | LP_TYPE)); guard++) {
        if ((*at & ~0x1Fu) == me) { *at = td->next; break; }
        at = &((Itd *)UsbVirt(*at & ~0x1Fu))->next;
    }
    for (int u = 0; u < 8; u++) td->status[u] &= ~ITD_ACTIVE;
}

/* Put transfer @k's iTDs in the frames after the last one's (lock held) */
static void iso_submit(Ehci *e, UsbPipe *p, int k)
{
    EIso *ei = p->hcd;
    UsbDev *d = p->dev;
    UINT16 now = frame_now(e);
    UINT16 ahead = (UINT16)((ei->frame - now) & 0x7FF);
    if (ahead < 2 || ahead >= 0x400) ei->frame = (UINT16)((now + 3) & 0x7FF);   /* (fell behind: catch up) */
    UINT32 uf = 8 / (UINT32)ei->per;                       /* microframes between packets in an iTD */
    for (int f = 0; f < ei->frames; f++) {
        int i = k * ei->frames + f, pk = k * p->iso_packets + f * ei->per;
        Itd *td = itd_at(ei, i);
        UINT64 pa = UsbPhys(p->dma) + (UINT64)pk * p->iso_psize, page0 = pa & ~0xFFFull;
        td->buf[0] = (UINT32)page0 | ((UINT32)(p->addr & 0xF) << 8) | d->addr;
        td->buf[1] = (UINT32)(page0 + PAGE_SIZE) | (p->in ? ITD_IN : 0) | p->mps;
        td->buf[2] = (UINT32)(page0 + 2 * PAGE_SIZE) | (1u + p->mult);
        for (int j = 3; j < 7; j++) td->buf[j] = (UINT32)(page0 + (UINT64)j * PAGE_SIZE);
        for (int j = 0; j < 7; j++) td->ext[j] = 0;
        for (int u = 0; u < 8; u++) td->status[u] = 0;
        for (int n = 0; n < ei->per; n++) {
            UINT64 at = UsbPhys(p->dma) + (UINT64)(pk + n) * p->iso_psize;
            td->status[n * uf] = ITD_ACTIVE | ((UINT32)p->iso_len[pk + n] << 16) |
                                 ((UINT32)((at - page0) >> 12) << 12) | (UINT32)(at & 0xFFF);
        }
        ei->at[i] = ei->frame;
        volatile UINT32 *slot = &e->frames[ei->frame & 1023];
        td->next = *slot;
        mfence();
        *slot = p32(td);
        ei->frame = (UINT16)((ei->frame + ei->step) & 0x7FF);
    }
    ei->fifo[(ei->head + ei->count++) % 128] = k;
}

static bool ehci_iso_start(UsbHc *hc, UsbPipe *p)
{
    Ehci *e = E(hc);
    EIso *ei = p->hcd;
    if (!ei || (p->iso_packets % ei->per)) return false;  /* (a transfer is whole iTDs) */
    int frames = p->iso_packets / ei->per;
    if (p->iso_xfers * frames > ISO_ITDS) return false;
    int slot = -1;
    for (int i = 0; i < MAX_ISO && slot < 0; i++) if (!e->streaming[i]) slot = i;
    if (slot < 0) return false;
    ei->frames = frames;
    ei->head = ei->count = 0;
    ei->frame = (UINT16)((frame_now(e) + 3) & 0x7FF);
    for (int k = 0; k < p->iso_xfers; k++) iso_submit(e, p, k);
    e->streaming[slot] = p;
    return true;
}

/* Out of the frame list with everything in flight (lock held) */
static void iso_unlink_all(Ehci *e, UsbPipe *p)
{
    EIso *ei = p->hcd;
    for (int i = 0; i < MAX_ISO; i++) if (e->streaming[i] == p) e->streaming[i] = NULL;
    for (; ei->count; ei->count--, ei->head = (ei->head + 1) % 128) {
        int k = ei->fifo[ei->head];
        for (int f = 0; f < ei->frames; f++) itd_unlink(e, ei, k * ei->frames + f);
    }
}

static void ehci_iso_stop(UsbHc *hc, UsbPipe *p)
{
    if (!p->hcd) return;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    iso_unlink_all(E(hc), p);
    spin_unlock_irqrestore(&g_usb_lock, s);
    UsbDelay(2);                          /* (the controller is past any frame that had them) */
}

/* Transfers whose frames have all passed (lock held) */
static void iso_poll(Ehci *e)
{
    UINT16 now = frame_now(e);
    for (int s = 0; s < MAX_ISO; s++) {
        UsbPipe *p = e->streaming[s];
        if (!p || !p->iso_cb || !p->hcd) continue;
        EIso *ei = p->hcd;
        UINT32 uf = 8 / (UINT32)ei->per;
        while (ei->count) {
            int k = ei->fifo[ei->head];
            if (!frame_past(now, ei->at[k * ei->frames + ei->frames - 1])) break;
            for (int f = 0; f < ei->frames; f++) {
                int i = k * ei->frames + f;
                Itd *td = itd_at(ei, i);
                UINT32 st[8];
                for (int u = 0; u < 8; u++) st[u] = td->status[u];
                itd_unlink(e, ei, i);
                if (p->in)
                    for (int n = 0; n < ei->per; n++) {
                        UINT32 t = st[n * uf];
                        p->iso_len[k * p->iso_packets + f * ei->per + n] =
                            (UINT16)(t & (ITD_ACTIVE | ITD_ERRORS) ? 0 : ITD_LEN(t));
                    }
            }
            ei->head = (ei->head + 1) % 128;
            ei->count--;
            if (UsbIsoDone(p, k)) iso_submit(e, p, k);
        }
    }
}

static void iso_free(Ehci *e, UsbPipe *p)
{
    EIso *ei = p->hcd;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    iso_unlink_all(e, p);
    p->hcd = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    UsbDelay(2);
    UsbDmaFree(ei->itd, ISO_PAGES);
    kfree(ei);
}

/* Interrupt IN and isochronous transfers that finished */
static void ehci_poll(UsbHc *hc)
{
    Ehci *e = E(hc);
    iso_poll(e);
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
        if (p->xfer == 1) { iso_free(e, p); continue; }
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

/* A pipe of an alternate setting being left */
static void ehci_pipe_drop(UsbHc *hc, UsbPipe *p)
{
    Ehci *e = E(hc);
    if (p->xfer == 1) { if (p->hcd) iso_free(e, p); return; }
    EQueue *eq = p->hcd;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int k = 0; k < MAX_LISTEN; k++) if (e->listening[k] == p) e->listening[k] = NULL;
    p->hcd = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    if (eq) queue_free(e, eq);
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
    Ehci *e = E(hc);
    for (int i = 0; i < MAX_LISTEN; i++) e->listening[i] = NULL;
    for (int i = 0; i < MAX_ISO; i++) e->streaming[i] = NULL;
    return controller_program(e);
}

static const UsbHcOps g_ehci_ops = {
    .kind        = "EHCI",
    .port_status = ehci_port_status,
    .port_reset  = ehci_port_reset,
    .pipe_add    = ehci_pipe_add,
    .pipe_drop   = ehci_pipe_drop,
    .dev_remove  = ehci_dev_remove,
    .control     = ehci_control,
    .bulk        = ehci_bulk,
    .listen      = ehci_listen,
    .iso_start   = ehci_iso_start,
    .iso_stop    = ehci_iso_stop,
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
