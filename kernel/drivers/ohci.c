/*
 * ohci.c — USB 1.1 (OHCI) host controller driver
 *
 * Follows the Open Host Controller Interface 1.0a specification.  Each
 * endpoint has an endpoint descriptor (ED) on the control, bulk or
 * interrupt list; the 32 interrupt-table entries of the HCCA all point at
 * one interrupt list, so every interrupt endpoint is polled each frame.
 *
 * Transfers are queued the usual OHCI way: an ED always ends in an empty
 * "dummy" TD; a transfer fills the dummy and the TDs after it, ends in a
 * new dummy, and moves the ED's tail pointer there, so the controller
 * never sees a half-built chain.  The controller keeps the data toggle in
 * the ED.  A short packet before a bulk transfer's last TD halts the ED
 * (data underrun): that ends the transfer, and the ED is restarted at its
 * tail.
 *
 * Isochronous endpoints have an ED of their own (format bit set) at the
 * end of the interrupt list, which the controller walks every frame; each
 * transfer of the core's ring is one or more isochronous TDs of up to
 * eight packets, one packet a frame, starting at the frame after the
 * previous TD's last.
 *
 * Polled: the controller's interrupts stay off; poll() looks at the
 * interrupt TDs every tick, and a waited-for transfer is spun on.  TDs
 * are tracked by their place in the queue's page, since the controller
 * reuses a finished TD's next pointer for its done queue.
 */

#include "usb_hc.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* Registers */
#define HC_REVISION         0x00
#define HC_CONTROL          0x04
#define HC_COMMAND_STATUS   0x08
#define HC_INT_STATUS       0x0C
#define HC_INT_DISABLE      0x14
#define HC_HCCA             0x18
#define HC_CONTROL_HEAD     0x20
#define HC_CONTROL_CURRENT  0x24
#define HC_BULK_HEAD        0x28
#define HC_BULK_CURRENT     0x2C
#define HC_FM_INTERVAL      0x34
#define HC_PERIODIC_START   0x40
#define HC_LS_THRESHOLD     0x44
#define HC_RH_DESC_A        0x48
#define HC_RH_STATUS        0x50
#define HC_RH_PORT(p)       (0x54 + 4 * ((p) - 1))

#define CTL_CBSR_4          3u
#define CTL_PLE             (1u << 2)
#define CTL_IE              (1u << 3)
#define CTL_CLE             (1u << 4)
#define CTL_BLE             (1u << 5)
#define CTL_HCFS_MASK       (3u << 6)
#define CTL_HCFS_RESET      (0u << 6)
#define CTL_HCFS_OPER       (2u << 6)
#define CTL_IR              (1u << 8)
#define CS_HCR              (1u << 0)
#define CS_CLF              (1u << 1)
#define CS_BLF              (1u << 2)
#define CS_OCR              (1u << 3)
#define INT_WDH             (1u << 1)
#define INT_MIE             (1u << 31)

/* Root hub port status (read) and commands (write) */
#define RH_CCS              (1u << 0)
#define RH_PES              (1u << 1)
#define RH_PRS              (1u << 4)
#define RH_PPS              (1u << 8)
#define RH_LSDA             (1u << 9)
#define RH_CSC              (1u << 16)
#define RH_PRSC             (1u << 20)
#define RH_CHANGES          0x001F0000u
#define RH_LPSC             (1u << 16)    /* (HcRhStatus) set global power */

/* ED dword 0 */
#define ED_LOWSPEED         (1u << 13)
#define ED_SKIP             (1u << 14)
#define ED_ISO              (1u << 15)
#define ED_DIR_OUT          (1u << 11)
#define ED_DIR_IN           (2u << 11)
#define ED_HALTED           1u
#define ED_CARRY            2u

/* TD dword 0 */
#define TD_ROUNDING         (1u << 18)
#define TD_DP_SETUP         (0u << 19)
#define TD_DP_OUT           (1u << 19)
#define TD_DP_IN            (2u << 19)
#define TD_NO_INT           (7u << 21)
#define TD_DATA0            (2u << 24)
#define TD_DATA1            (3u << 24)
#define TD_CC(v)            ((v) >> 28)
#define TD_CC_FRESH         (0xFu << 28)  /* "not accessed" */

#define CC_NOERROR          0
#define CC_STALL            4
#define CC_DATAUNDERRUN     9
#define CC_NOT_ACCESSED     14            /* (14 and 15) */

#define TD_BYTES            0x2000u       /* 8 KiB per TD: one page crossing at most */
#define TD_SLOTS            126
#define MAX_LISTEN          64
#define ITD_SLOTS           120
#define MAX_ISO             16
#define HCCA_FRAME          0x80          /* HccaFrameNumber */

typedef struct __attribute__((packed, aligned(16))) {
    UINT32 ctl, tail, head, next;
} Ed;

typedef struct __attribute__((packed, aligned(32))) {
    UINT32 ctl, cbp, next, be;
    /* (software) */
    UINT32 start, len, pad[2];
} Td;

/* One page per queue: the ED, a setup packet and the TD ring */
typedef struct {
    Ed      ed;
    UINT8   setup[16];
    Td      td[TD_SLOTS + 1];
} QPage;

typedef struct {
    QPage  *q;
    int     list;                         /* 0 control, 1 bulk, 2 interrupt */
    int     tail;                         /* the dummy TD's slot */
    int     first, n;                     /* the transfer in flight */
    bool    linked;
} OQueue;

typedef struct {
    OQueue *ep0;
} ODev;

typedef struct {
    UsbHc            *hc;
    volatile UINT8   *r;
    int               ports;
    UINT32            fm_interval;
    UINT8            *hcca;
    QPage            *intr_head;          /* the interrupt list's (skipped) head ED */
    UsbPipe          *listening[MAX_LISTEN];
    UsbPipe          *streaming[MAX_ISO];
    PciDevice         pci;
} Ohci;

/* Isochronous TD: up to 8 packets in two pages from bp0 */
typedef struct __attribute__((packed, aligned(32))) {
    UINT32 ctl;                           /* SF 0-15, DI 21-23, FC 24-26, CC 28-31 */
    UINT32 bp0, next, be;
    UINT16 psw[8];                        /* offset (bit 12: be's page), then CC and size */
} ITd;

typedef struct {
    Ed      ed;
    UINT8   pad[16];
    ITd     td[ITD_SLOTS + 1];
} IPage;

/* An isochronous pipe's controller state: its ED's TD ring and the
 * transfers in flight, oldest first */
typedef struct {
    IPage  *q;
    int     tail;                         /* the dummy TD's slot */
    UINT16  frame;                        /* where the next TD starts */
    bool    linked;
    int     fifo[128], head, count;       /* transfers in flight */
    int     first[128], ntd[128];         /* each one's TDs */
} OIso;

_Static_assert(sizeof(QPage) <= 4096, "OHCI queue page");
_Static_assert(sizeof(IPage) <= 4096, "OHCI isochronous page");

static inline UINT32 rd32(volatile UINT8 *b, UINT32 r)          { return *(volatile UINT32 *)(b + r); }
static inline void   wr32(volatile UINT8 *b, UINT32 r, UINT32 v) { *(volatile UINT32 *)(b + r) = v; }
static inline UINT32 p32(const volatile void *va) { return (UINT32)UsbPhys(va); }
static inline Ohci *O(UsbHc *hc) { return hc->priv; }

/* ---- queues ---- */

static UINT32 ed_ctl(UsbDev *d, UINT8 ep, UINT16 mps)
{
    return (UINT32)d->addr | ((UINT32)(ep & 0xF) << 7) | (d->speed == USB_SPEED_LOW ? ED_LOWSPEED : 0) |
           ((UINT32)mps << 16);           /* direction from the TDs */
}

static void queue_link(Ohci *o, OQueue *oq)
{
    Ed *ed = &oq->q->ed;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    if (oq->list == 2) {
        ed->next = o->intr_head->ed.next;
        mfence();
        o->intr_head->ed.next = p32(ed);
    } else {
        UINT32 reg = oq->list ? HC_BULK_HEAD : HC_CONTROL_HEAD;
        ed->next = rd32(o->r, reg);
        mfence();
        wr32(o->r, reg, p32(ed));
    }
    oq->linked = true;
    spin_unlock_irqrestore(&g_usb_lock, s);
}

static void queue_unlink(Ohci *o, OQueue *oq)
{
    if (!oq->linked) return;
    Ed *ed = &oq->q->ed;
    UINT32 me = p32(ed);
    ed->ctl |= ED_SKIP;
    UsbDelay(2);                          /* (the controller is past any frame that had it) */
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    oq->linked = false;
    if (oq->list == 2) {
        Ed *prev = &o->intr_head->ed;
        for (int guard = 0; prev->next && guard < 1024; guard++) {
            if (prev->next == me) { prev->next = ed->next; break; }
            prev = UsbVirt(prev->next);
        }
    } else {
        UINT32 head = oq->list ? HC_BULK_HEAD : HC_CONTROL_HEAD;
        UINT32 cur = oq->list ? HC_BULK_CURRENT : HC_CONTROL_CURRENT;
        if (rd32(o->r, head) == me) wr32(o->r, head, ed->next);
        else for (UINT32 at = rd32(o->r, head), guard = 0; at && guard < 1024; guard++) {
            Ed *prev = UsbVirt(at);
            if (prev->next == me) { prev->next = ed->next; break; }
            at = prev->next;
        }
        if (rd32(o->r, cur) == me) wr32(o->r, cur, 0);
    }
    spin_unlock_irqrestore(&g_usb_lock, s);
    UsbDelay(2);
}

static OQueue *queue_new(Ohci *o, UsbDev *d, UINT8 ep, UINT16 mps, int list)
{
    OQueue *oq = kzalloc(sizeof(OQueue));
    if (!oq) return NULL;
    oq->q = UsbDmaAlloc(1);
    if (!oq->q) { kfree(oq); return NULL; }
    oq->list = list;
    oq->q->ed.ctl = ed_ctl(d, ep, mps);
    oq->q->ed.head = oq->q->ed.tail = p32(&oq->q->td[0]);    /* empty: the dummy alone */
    queue_link(o, oq);
    return oq;
}

static void queue_free(Ohci *o, OQueue *oq)
{
    if (!oq) return;
    queue_unlink(o, oq);
    UsbDmaFree(oq->q, 1);
    kfree(oq);
}

static inline int slot_after(int i) { return i == TD_SLOTS ? 0 : i + 1; }

/* Queue a transfer of @n TDs (@ctl[i], @pa[i], @len[i]) and tell the
 * controller its list has work */
static void queue_submit(Ohci *o, OQueue *oq, int n, const UINT32 *ctl, const UINT64 *pa, const UINT32 *len)
{
    QPage *q = oq->q;
    int at = oq->tail;
    oq->first = at;
    oq->n = n;
    for (int i = 0; i < n; i++) {
        Td *td = &q->td[at];
        int nx = slot_after(at);
        Td *nt = &q->td[nx];
        nt->ctl = 0; nt->cbp = 0; nt->be = 0; nt->next = 0;       /* the next dummy, or the next TD */
        td->ctl = ctl[i] | TD_NO_INT | TD_CC_FRESH;
        td->cbp = len[i] ? (UINT32)pa[i] : 0;
        td->be = len[i] ? (UINT32)(pa[i] + len[i] - 1) : 0;
        td->start = (UINT32)pa[i];
        td->len = len[i];
        td->next = p32(nt);
        at = nx;
    }
    oq->tail = at;
    mfence();
    q->ed.tail = p32(&q->td[at]);
    mfence();
    if (oq->list == 0) wr32(o->r, HC_COMMAND_STATUS, CS_CLF);
    else if (oq->list == 1) wr32(o->r, HC_COMMAND_STATUS, CS_BLF);
}

typedef enum { Q_BUSY, Q_DONE, Q_FAILED, Q_STALLED } QState;

/* How the transfer in flight stands; *@moved: bytes of TDs from @from on.
 * A short packet with rounding allowed (a control transfer's data stage,
 * a bulk transfer's last TD) is not an error; one before a bulk
 * transfer's last TD halts the ED with a data underrun, which ends it. */
static QState queue_state(OQueue *oq, int from, UINT32 *moved)
{
    QPage *q = oq->q;
    UINT32 head = q->ed.head;
    bool halted = (head & ED_HALTED) != 0;
    UINT32 m = 0;
    QState st = Q_DONE;
    int at = oq->first;
    for (int i = 0; i < oq->n; i++, at = slot_after(at)) {
        Td *td = &q->td[at];
        UINT32 cc = TD_CC(td->ctl);
        if (cc >= CC_NOT_ACCESSED) { st = Q_BUSY; break; }
        UINT32 got = td->cbp ? td->cbp - td->start : td->len;
        if (i >= from) m += got;
        if (cc == CC_DATAUNDERRUN) break;                  /* short: the end */
        if (cc == CC_STALL) { st = Q_STALLED; break; }
        if (cc != CC_NOERROR) { st = Q_FAILED; break; }
    }
    *moved = m;
    if (st == Q_BUSY && !halted) return Q_BUSY;
    if (halted) q->ed.head = q->ed.tail | (head & ED_CARRY);   /* skip the rest, keep the toggle */
    return st == Q_BUSY ? Q_FAILED : st;
}

/* Stop a queue that is still busy (a timeout) */
static void queue_cancel(Ohci *o, OQueue *oq)
{
    queue_unlink(o, oq);
    oq->q->ed.head = oq->q->ed.tail | (oq->q->ed.head & ED_CARRY);
    oq->q->ed.ctl &= ~ED_SKIP;
    queue_link(o, oq);
}

static QState queue_wait(Ohci *o, OQueue *oq, UsbDev *d, UINT32 timeout_ms, int from, UINT32 *moved)
{
    UINT64 until = UsbDeadline(timeout_ms);
    for (;;) {
        wr32(o->r, HC_INT_STATUS, INT_WDH);    /* (let the controller write its done queue) */
        QState st = queue_state(oq, from, moved);
        if (st != Q_BUSY) return st;
        if (d->gone || UsbPast(until)) {
            if (!d->gone) kprintf("[USB] %s: transfer timed out\n", d->name);
            queue_cancel(o, oq);
            return Q_FAILED;
        }
        pause_cpu();
    }
}

/* ---- isochronous ---- */

static inline UINT16 frame_now(Ohci *o) { return *(volatile UINT16 *)(o->hcca + HCCA_FRAME); }

/* Link an isochronous ED at the end of the interrupt list (lock held) */
static void iso_link(Ohci *o, OIso *oi)
{
    Ed *prev = &o->intr_head->ed;
    for (int guard = 0; prev->next && guard < 1024; guard++) prev = UsbVirt(prev->next);
    oi->q->ed.next = 0;
    mfence();
    prev->next = p32(&oi->q->ed);
    oi->linked = true;
}

static void iso_unlink(Ohci *o, OIso *oi)
{
    if (!oi->linked) return;
    UINT32 me = p32(&oi->q->ed);
    oi->q->ed.ctl |= ED_SKIP;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (Ed *prev = &o->intr_head->ed; prev->next; prev = UsbVirt(prev->next))
        if (prev->next == me) { prev->next = oi->q->ed.next; break; }
    oi->linked = false;
    for (int i = 0; i < MAX_ISO; i++)
        if (o->streaming[i] && o->streaming[i]->hcd == oi) o->streaming[i] = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    UsbDelay(2);                          /* (the controller is past any frame that had it) */
}

/* Queue transfer @k: TDs of up to 8 packets within two pages (lock held) */
static bool iso_submit(Ohci *o, UsbPipe *p, int k)
{
    OIso *oi = p->hcd;
    IPage *q = oi->q;
    int n = p->iso_packets, i = 0, ntd = 0;
    INT16 ahead = (INT16)(oi->frame - frame_now(o));
    if (ahead < 2 || ahead > 512) oi->frame = (UINT16)(frame_now(o) + 3);   /* (fell behind: catch up) */
    oi->first[k] = oi->tail;
    while (i < n) {
        UINT64 pa = UsbPhys(p->dma) + (UINT64)(k * n + i) * p->iso_psize;
        UINT32 bp0 = (UINT32)pa & ~0xFFFu;
        int c = 0;
        UINT32 end = (UINT32)pa;
        while (i + c < n && c < 8) {
            UINT32 at = (UINT32)(UsbPhys(p->dma) + (UINT64)(k * n + i + c) * p->iso_psize);
            UINT32 len = p->iso_len[k * n + i + c];
            if (at + len - bp0 > 0x2000) break;
            q->td[oi->tail].psw[c] = (UINT16)((at - bp0) | 0xE000);   /* CC: not accessed */
            end = at + len;
            c++;
        }
        if (!c) return false;                       /* (a packet bigger than a page) */
        ITd *td = &q->td[oi->tail];
        int nx = oi->tail == ITD_SLOTS ? 0 : oi->tail + 1;
        ITd *nt = &q->td[nx];
        memset(nt, 0, sizeof(*nt));
        td->bp0 = bp0;
        td->be = end - 1;
        td->next = p32(nt);
        td->ctl = oi->frame | TD_NO_INT | ((UINT32)(c - 1) << 24) | TD_CC_FRESH;
        oi->frame = (UINT16)(oi->frame + c);
        oi->tail = nx;
        i += c;
        ntd++;
    }
    oi->ntd[k] = ntd;
    oi->fifo[(oi->head + oi->count++) % 128] = k;
    mfence();
    q->ed.tail = p32(&q->td[oi->tail]);
    return true;
}

static bool ohci_iso_start(UsbHc *hc, UsbPipe *p)
{
    Ohci *o = O(hc);
    OIso *oi = p->hcd;
    if (!oi || p->iso_xfers * ((p->iso_packets + 7) / 8 + 1) >= ITD_SLOTS) return false;
    int at = -1;
    for (int i = 0; i < MAX_ISO && at < 0; i++) if (!o->streaming[i]) at = i;
    if (at < 0) return false;
    oi->count = oi->head = 0;
    oi->q->ed.head = oi->q->ed.tail = p32(&oi->q->td[oi->tail]);
    oi->frame = (UINT16)(frame_now(o) + 3);
    for (int k = 0; k < p->iso_xfers; k++)
        if (!iso_submit(o, p, k)) return false;
    oi->q->ed.ctl &= ~ED_SKIP;
    if (!oi->linked) iso_link(o, oi);
    o->streaming[at] = p;
    return true;
}

static void ohci_iso_stop(UsbHc *hc, UsbPipe *p)
{
    OIso *oi = p->hcd;
    if (!oi) return;
    iso_unlink(O(hc), oi);
    oi->q->ed.head = oi->q->ed.tail;
    oi->count = 0;
}

/* Finished isochronous transfers, in order (lock held) */
static void iso_poll(Ohci *o)
{
    for (int s = 0; s < MAX_ISO; s++) {
        UsbPipe *p = o->streaming[s];
        if (!p || !p->iso_cb || !p->hcd) continue;
        OIso *oi = p->hcd;
        while (oi->count) {
            int k = oi->fifo[oi->head], at = oi->first[k];
            bool done = true;
            for (int t = 0; t < oi->ntd[k] && done; t++, at = at == ITD_SLOTS ? 0 : at + 1)
                if (TD_CC(oi->q->td[at].ctl) >= CC_NOT_ACCESSED) done = false;
            if (!done) break;
            if (p->in) {
                int pk = k * p->iso_packets;
                at = oi->first[k];
                for (int t = 0; t < oi->ntd[k]; t++, at = at == ITD_SLOTS ? 0 : at + 1) {
                    ITd *td = &oi->q->td[at];
                    for (UINT32 c = 0; c <= ((td->ctl >> 24) & 7); c++, pk++) {
                        UINT16 w = td->psw[c];
                        UINT32 cc = w >> 12;
                        p->iso_len[pk] = (UINT16)(cc == CC_NOERROR || cc == CC_DATAUNDERRUN ? w & 0x7FF : 0);
                    }
                }
            }
            oi->head = (oi->head + 1) % 128;
            oi->count--;
            if (UsbIsoDone(p, k)) iso_submit(o, p, k);
        }
    }
}

static void iso_free(Ohci *o, UsbPipe *p)
{
    OIso *oi = p->hcd;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    p->hcd = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    iso_unlink(o, oi);
    UsbDmaFree(oi->q, 1);
    kfree(oi);
}

/* ---- devices and transfers ---- */

static int ohci_control(UsbHc *hc, UsbDev *d, const UsbSetup *s, bool *stalled)
{
    Ohci *o = O(hc);
    ODev *od = d->hcd;
    if (!od) {
        od = kzalloc(sizeof(ODev));
        if (!od) return -1;
        od->ep0 = queue_new(o, d, 0, d->ep0_mps, 0);
        if (!od->ep0) { kfree(od); return -1; }
        d->hcd = od;
    }
    if (d->gone) return -1;
    OQueue *oq = od->ep0;
    oq->q->ed.ctl = ed_ctl(d, 0, d->ep0_mps);          /* (idle: address and packet size may have changed) */
    memcpy(oq->q->setup, s, 8);
    bool in = (s->type & 0x80) != 0;
    UINT32 ctl[3], len[3];
    UINT64 pa[3];
    int n = 0;
    ctl[n] = TD_DP_SETUP | TD_DATA0; pa[n] = UsbPhys(oq->q->setup); len[n++] = 8;
    if (s->len) { ctl[n] = (in ? TD_DP_IN : TD_DP_OUT) | TD_DATA1 | TD_ROUNDING; pa[n] = UsbPhys(d->buf); len[n++] = s->len; }
    ctl[n] = (in && s->len ? TD_DP_OUT : TD_DP_IN) | TD_DATA1; pa[n] = 0; len[n++] = 0;
    queue_submit(o, oq, n, ctl, pa, len);
    UINT32 moved = 0;
    QState st = queue_wait(o, oq, d, 5000, 1, &moved);
    if (st == Q_STALLED) *stalled = true;
    if (st != Q_DONE) return -1;
    return in ? (int)moved : s->len;
}

static bool ohci_pipe_add(UsbHc *hc, UsbPipe *p)
{
    if (p->xfer == 1) {
        OIso *oi = kzalloc(sizeof(OIso));
        if (!oi) return false;
        oi->q = UsbDmaAlloc(1);
        if (!oi->q) { kfree(oi); return false; }
        oi->q->ed.ctl = ed_ctl(p->dev, p->addr, p->mps) | ED_ISO | ED_SKIP | (p->in ? ED_DIR_IN : ED_DIR_OUT);
        oi->q->ed.head = oi->q->ed.tail = p32(&oi->q->td[0]);
        p->hcd = oi;
        return true;
    }
    OQueue *oq = queue_new(O(hc), p->dev, p->addr, p->mps, p->xfer == 3 ? 2 : 1);
    if (!oq) return false;
    p->hcd = oq;
    return true;
}

static int ohci_bulk(UsbHc *hc, UsbPipe *p, UINT32 len, UINT32 timeout_ms, bool *stalled)
{
    Ohci *o = O(hc);
    OQueue *oq = p->hcd;
    if (!oq) return -1;
    UINT32 ctl[TD_SLOTS], lens[TD_SLOTS];
    UINT64 pa[TD_SLOTS];
    int n = 0;
    for (UINT32 off = 0; off < len && n < TD_SLOTS - 1; n++) {
        UINT32 chunk = len - off > TD_BYTES ? TD_BYTES : len - off;
        bool last = off + chunk >= len;
        ctl[n] = (p->in ? TD_DP_IN : TD_DP_OUT) | (last ? TD_ROUNDING : 0);
        pa[n] = UsbPhys(p->dma) + off;
        lens[n] = chunk;
        off += chunk;
    }
    queue_submit(o, oq, n, ctl, pa, lens);
    UINT32 moved = 0;
    QState st = queue_wait(o, oq, p->dev, timeout_ms, 0, &moved);
    if (st == Q_STALLED) *stalled = true;
    return st == Q_DONE ? (int)moved : -1;
}

static bool ohci_listen(UsbHc *hc, UsbPipe *p)
{
    Ohci *o = O(hc);
    OQueue *oq = p->hcd;
    if (!oq) return false;
    int free = -1;
    for (int i = 0; i < MAX_LISTEN; i++) {
        if (o->listening[i] == p) { free = i; break; }
        if (!o->listening[i] && free < 0) free = i;
    }
    if (free < 0) return false;
    o->listening[free] = p;
    UINT32 ctl = TD_DP_IN | TD_ROUNDING, len = (UINT32)p->listen_len > TD_BYTES ? TD_BYTES : (UINT32)p->listen_len;
    UINT64 pa = UsbPhys(p->dma);
    queue_submit(o, oq, 1, &ctl, &pa, &len);
    return true;
}

static void ohci_pipe_drop(UsbHc *hc, UsbPipe *p)
{
    Ohci *o = O(hc);
    if (p->xfer == 1) { iso_free(o, p); return; }
    OQueue *oq = p->hcd;
    IrqState s = spin_lock_irqsave(&g_usb_lock);
    for (int k = 0; k < MAX_LISTEN; k++) if (o->listening[k] == p) o->listening[k] = NULL;
    p->hcd = NULL;
    spin_unlock_irqrestore(&g_usb_lock, s);
    queue_free(o, oq);
}

/* Interrupt IN and isochronous transfers that finished */
static void ohci_poll(UsbHc *hc)
{
    Ohci *o = O(hc);
    wr32(o->r, HC_INT_STATUS, INT_WDH);
    iso_poll(o);
    for (int i = 0; i < MAX_LISTEN; i++) {
        UsbPipe *p = o->listening[i];
        if (!p || !p->cb || !p->hcd) continue;
        OQueue *oq = p->hcd;
        if (!oq->n) continue;
        UINT32 moved = 0;
        QState st = queue_state(oq, 0, &moved);
        if (st == Q_BUSY) continue;
        oq->n = 0;
        if (st != Q_DONE) {
            UsbPipeListenFailed(p, (int)TD_CC(oq->q->td[oq->first].ctl));
            continue;
        }
        if (UsbPipeDeliver(p, (int)moved)) ohci_listen(hc, p);
    }
}

static bool ohci_pipe_reset(UsbHc *hc, UsbPipe *p)
{
    (void)hc;
    OQueue *oq = p->hcd;
    if (!oq) return false;
    oq->q->ed.head = oq->q->ed.tail;      /* not halted, DATA0 */
    return true;
}

static void ohci_dev_remove(UsbHc *hc, UsbDev *d)
{
    Ohci *o = O(hc);
    for (int i = 0; i < 32; i++) {
        UsbPipe *p = d->pipes[i];
        if (p && p->hcd) ohci_pipe_drop(hc, p);
    }
    ODev *od = d->hcd;
    if (od) {
        queue_free(o, od->ep0);
        kfree(od);
        d->hcd = NULL;
    }
}

/* ---- root ports ---- */

static UINT32 ohci_port_status(UsbHc *hc, int port)
{
    Ohci *o = O(hc);
    UINT32 v = rd32(o->r, HC_RH_PORT(port));
    UINT32 st = (v & RH_CCS) ? USB_PORT_CONNECTED : 0;
    if (v & RH_CSC) {
        wr32(o->r, HC_RH_PORT(port), RH_CSC);
        st |= USB_PORT_CHANGED;
    }
    return st;
}

static UINT8 ohci_port_reset(UsbHc *hc, int port)
{
    Ohci *o = O(hc);
    if (!(rd32(o->r, HC_RH_PORT(port)) & RH_CCS)) return 0;
    wr32(o->r, HC_RH_PORT(port), RH_PRS);
    UINT64 until = UsbDeadline(100);
    while (!(rd32(o->r, HC_RH_PORT(port)) & RH_PRSC) && !UsbPast(until)) pause_cpu();
    UINT32 v = rd32(o->r, HC_RH_PORT(port));
    wr32(o->r, HC_RH_PORT(port), v & RH_CHANGES & ~RH_CSC);
    if ((v & (RH_CCS | RH_PES)) != (RH_CCS | RH_PES)) return 0;
    UsbDelay(10);                          /* reset recovery */
    return (v & RH_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* ---- controller bring-up ---- */

static bool controller_program(Ohci *o)
{
    /* Take it from the firmware's SMM driver, if that has it */
    if (rd32(o->r, HC_CONTROL) & CTL_IR) {
        wr32(o->r, HC_COMMAND_STATUS, CS_OCR);
        UINT64 until = UsbDeadline(1000);
        while ((rd32(o->r, HC_CONTROL) & CTL_IR) && !UsbPast(until)) pause_cpu();
    }
    wr32(o->r, HC_INT_DISABLE, 0xFFFFFFFFu);
    UINT32 fi = rd32(o->r, HC_FM_INTERVAL) & 0x3FFF;
    if (fi < 0x2000) fi = 0x2EDF;                          /* (11999: 1 ms of bit times) */
    o->fm_interval = fi;

    /* Reset into the USB reset state, then the controller itself */
    wr32(o->r, HC_CONTROL, CTL_HCFS_RESET);
    UsbDelay(50);
    wr32(o->r, HC_COMMAND_STATUS, CS_HCR);
    UINT64 until = UsbDeadline(10);
    while ((rd32(o->r, HC_COMMAND_STATUS) & CS_HCR) && !UsbPast(until)) pause_cpu();
    if (rd32(o->r, HC_COMMAND_STATUS) & CS_HCR) return false;

    /* Now in the suspend state: set up and go operational within 2 ms */
    o->intr_head->ed.ctl = ED_SKIP;
    o->intr_head->ed.next = 0;
    for (int i = 0; i < 32; i++) ((UINT32 *)o->hcca)[i] = p32(&o->intr_head->ed);
    wr32(o->r, HC_HCCA, p32(o->hcca));
    wr32(o->r, HC_CONTROL_HEAD, 0);
    wr32(o->r, HC_BULK_HEAD, 0);
    UINT32 fsmps = (6 * (fi - 210)) / 7;
    UINT32 fit = (rd32(o->r, HC_FM_INTERVAL) & (1u << 31)) ^ (1u << 31);
    wr32(o->r, HC_FM_INTERVAL, fit | (fsmps << 16) | fi);
    wr32(o->r, HC_PERIODIC_START, fi * 9 / 10);
    wr32(o->r, HC_LS_THRESHOLD, 0x628);
    wr32(o->r, HC_CONTROL, CTL_CBSR_4 | CTL_PLE | CTL_IE | CTL_CLE | CTL_BLE | CTL_HCFS_OPER);

    /* Power the ports (all at once, or each one) */
    UINT32 a = rd32(o->r, HC_RH_DESC_A);
    wr32(o->r, HC_RH_STATUS, RH_LPSC);
    for (int p = 1; p <= o->ports; p++) wr32(o->r, HC_RH_PORT(p), RH_PPS);
    UsbDelay((int)((a >> 24) & 0xFF) * 2 + 2);
    return (rd32(o->r, HC_CONTROL) & CTL_HCFS_MASK) == CTL_HCFS_OPER;
}

static bool ohci_resume(UsbHc *hc)
{
    Ohci *o = O(hc);
    for (int i = 0; i < MAX_LISTEN; i++) o->listening[i] = NULL;
    for (int i = 0; i < MAX_ISO; i++) o->streaming[i] = NULL;
    /* (the old devices' EDs are forgotten with the lists: they are freed,
     * unlinked from nothing, when the core takes the devices away) */
    return controller_program(o);
}

static const UsbHcOps g_ohci_ops = {
    .kind        = "OHCI",
    .port_status = ohci_port_status,
    .port_reset  = ohci_port_reset,
    .pipe_add    = ohci_pipe_add,
    .pipe_drop   = ohci_pipe_drop,
    .dev_remove  = ohci_dev_remove,
    .control     = ohci_control,
    .bulk        = ohci_bulk,
    .listen      = ohci_listen,
    .iso_start   = ohci_iso_start,
    .iso_stop    = ohci_iso_stop,
    .pipe_reset  = ohci_pipe_reset,
    .poll        = ohci_poll,
    .resume      = ohci_resume,
};

bool OhciProbe(const PciDevice *pci)
{
    Ohci *o = kzalloc(sizeof(Ohci));
    if (!o) return false;
    o->pci = *pci;
    o->r = PciMapBar(pci, 0);
    if (!o->r) { kfree(o); return false; }
    PciEnableDevice(pci);
    o->ports = (int)(rd32(o->r, HC_RH_DESC_A) & 0xFF);
    if (o->ports > 15) o->ports = 15;
    o->hcca = UsbDmaAlloc(1);              /* (256 bytes, 256-aligned) */
    o->intr_head = UsbDmaAlloc(1);
    if (!o->hcca || !o->intr_head || !controller_program(o)) {
        kprintf("[USB] OHCI controller %02x:%02x.%d failed to start\n", pci->bus, pci->dev, pci->func);
        return false;          /* (its memory stays: the controller may still point at it) */
    }
    o->hc = UsbAddController(&g_ohci_ops, o, o->ports);
    if (!o->hc) return false;
    kprintf("[USB] bus %d: OHCI %02x:%02x.%d, revision %x, %d ports\n",
            o->hc->num, pci->bus, pci->dev, pci->func, rd32(o->r, HC_REVISION) & 0xFF, o->ports);
    return true;
}
