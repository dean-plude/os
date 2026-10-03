/*
 * virtio_input.c — virtio input devices: multi-touch screens
 *
 * A virtio input device (virtio 1.2, section 5.8) passes Linux evdev
 * events: (type, code, value) triples in 8-byte buffers on its event
 * queue.  Its configuration space answers questions put by writing
 * `select` and `subsel`: the name, which event codes it sends (EV_BITS)
 * and each absolute axis's range (ABS_INFO).
 *
 * Multi-touch screens (QEMU's virtio-multitouch-pci, and those of other
 * hypervisors) send the evdev multi-touch protocol B: ABS_MT_SLOT picks a
 * contact, ABS_MT_TRACKING_ID starts it (an id) or ends it (-1),
 * ABS_MT_POSITION_X/Y move it, and SYN_REPORT ends a frame.  Every contact
 * that changed in a frame becomes an INPUT_TOUCH event, then the frame's
 * end, exactly as from a USB touch screen (usbhid.c).  Other virtio input
 * devices (keyboards, mice, tablets) are only logged: NovaOS uses the
 * PS/2 and USB ones.
 *
 * The PCI setup is virtio_net.c's (capabilities, the handshake, split
 * queues); the event queue asks for no interrupts and is polled by the
 * desktop loop.
 */

#include "virtio_input.h"
#include "../hal/pci.h"
#include "../wm/input.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../arch/x86_64/cpu.h"

#define CAP_COMMON   1
#define CAP_NOTIFY   2
#define CAP_DEVICE   4

/* Common configuration (4.1.4.3) */
#define C_DFSELECT   0x00
#define C_DF         0x04
#define C_GFSELECT   0x08
#define C_GF         0x0C
#define C_STATUS     0x14
#define C_QSELECT    0x16
#define C_QSIZE      0x18
#define C_QMSIX      0x1A
#define C_QENABLE    0x1C
#define C_QNOTIFY    0x1E
#define C_QDESC      0x20
#define C_QDRIVER    0x28
#define C_QDEVICE    0x30

#define S_ACK        1
#define S_DRIVER     2
#define S_DRIVER_OK  4
#define S_FEATURES_OK 8
#define S_FAILED     0x80
#define F_VERSION_1_HI 1u                  /* feature bit 32 */

/* Device configuration (5.8.4) */
#define CFG_SELECT   0
#define CFG_SUBSEL   1
#define CFG_SIZE     2
#define CFG_DATA     8
#define CFG_ID_NAME  0x01
#define CFG_EV_BITS  0x11
#define CFG_ABS_INFO 0x12

/* evdev */
#define EV_SYN       0
#define EV_ABS       3
#define SYN_REPORT   0
#define ABS_MT_SLOT  0x2F
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39

#define N_DESC       64
#define DESC_F_WRITE 2
#define AVAIL_F_NO_INTERRUPT 1
#define MAX_DEV      4

typedef struct __attribute__((packed)) { UINT64 addr; UINT32 len; UINT16 flags, next; } Desc;
typedef struct __attribute__((packed)) { UINT16 flags, idx, ring[N_DESC], used_event; } Avail;
typedef struct __attribute__((packed)) { UINT32 id, len; } UsedElem;
typedef struct __attribute__((packed)) { UINT16 flags, idx; UsedElem ring[N_DESC]; UINT16 avail_event; } Used;
typedef struct __attribute__((packed)) { UINT16 type, code; UINT32 value; } Event;

typedef struct {
    PciDevice        pci;
    volatile UINT8  *common, *notify_base, *dev;
    UINT32           notify_mul;
    Desc            *desc;
    volatile Avail  *avail;
    volatile Used   *used;
    Event           *ev;                  /* N_DESC event buffers */
    UINT16           size, last_used;
    volatile UINT16 *notify;
    char             name[64];
    /* multi-touch */
    INT32            xmin, xmax, ymin, ymax;
    int              slot;
    struct { INT32 x, y; bool down, changed; } c[TOUCH_MAX];
} Vin;

static Vin *g_dev[MAX_DEV];
static int  g_ndev;

static inline UINT64 phys(const void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }
static inline void   c8(Vin *v, UINT32 o, UINT8 x)   { *(volatile UINT8 *)(v->common + o) = x; }
static inline void   c16(Vin *v, UINT32 o, UINT16 x) { *(volatile UINT16 *)(v->common + o) = x; }
static inline void   c32(Vin *v, UINT32 o, UINT32 x) { *(volatile UINT32 *)(v->common + o) = x; }
static inline void   c64(Vin *v, UINT32 o, UINT64 x) { c32(v, o, (UINT32)x); c32(v, o + 4, (UINT32)(x >> 32)); }
static inline UINT8  r8(Vin *v, UINT32 o)  { return *(volatile UINT8 *)(v->common + o); }
static inline UINT16 r16(Vin *v, UINT32 o) { return *(volatile UINT16 *)(v->common + o); }
static inline UINT32 r32(Vin *v, UINT32 o) { return *(volatile UINT32 *)(v->common + o); }

static bool find_caps(Vin *v)
{
    const PciDevice *d = &v->pci;
    if (!(PciRead16(d->bus, d->dev, d->func, 0x06) & (1u << 4))) return false;
    UINT8 off = (UINT8)(PciRead32(d->bus, d->dev, d->func, 0x34) & 0xFC);
    for (int guard = 0; off && guard < 48; guard++) {
        UINT32 h = PciRead32(d->bus, d->dev, d->func, off);
        UINT8 id = (UINT8)h, next = (UINT8)(h >> 8), type = (UINT8)(h >> 24);
        if (id == 0x09) {
            UINT8 bar = (UINT8)PciRead32(d->bus, d->dev, d->func, off + 4);
            UINT32 boff = PciRead32(d->bus, d->dev, d->func, off + 8);
            volatile UINT8 *base = bar < 6 ? PciMapBar(d, bar) : NULL;
            if (base) {
                volatile UINT8 *p = base + boff;
                if (type == CAP_COMMON && !v->common) v->common = p;
                else if (type == CAP_NOTIFY && !v->notify_base) {
                    v->notify_base = p;
                    v->notify_mul = PciRead32(d->bus, d->dev, d->func, off + 16);
                } else if (type == CAP_DEVICE && !v->dev) v->dev = p;
            }
        }
        off = next & 0xFC;
    }
    return v->common && v->notify_base && v->dev;
}

/* Ask the configuration space: returns the answer's size (0: none) */
static int cfg_query(Vin *v, UINT8 select, UINT8 subsel)
{
    v->dev[CFG_SELECT] = select;
    v->dev[CFG_SUBSEL] = subsel;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return v->dev[CFG_SIZE];
}

static INT32 cfg_le32(Vin *v, int off)
{
    UINT32 x = 0;
    for (int i = 0; i < 4; i++) x |= (UINT32)v->dev[CFG_DATA + off + i] << (8 * i);
    return (INT32)x;
}

/* The event queue: every buffer given to the device */
static bool queue_setup(Vin *v)
{
    c16(v, C_QSELECT, 0);
    UINT16 max = r16(v, C_QSIZE);
    if (!max) return false;
    v->size = max < N_DESC ? max : N_DESC;
    c16(v, C_QSIZE, v->size);
    c16(v, C_QMSIX, 0xFFFF);
    memset(v->desc, 0, PAGE_SIZE);
    memset((void *)v->avail, 0, PAGE_SIZE);
    memset((void *)v->used, 0, PAGE_SIZE);
    v->avail->flags = AVAIL_F_NO_INTERRUPT;
    v->last_used = 0;
    for (UINT16 k = 0; k < v->size; k++) {
        v->desc[k].addr = phys(&v->ev[k]);
        v->desc[k].len = sizeof(Event);
        v->desc[k].flags = DESC_F_WRITE;
        v->avail->ring[k] = k;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->avail->idx = v->size;
    c64(v, C_QDESC, phys(v->desc));
    c64(v, C_QDRIVER, phys((const void *)v->avail));
    c64(v, C_QDEVICE, phys((const void *)v->used));
    v->notify = (volatile UINT16 *)(v->notify_base + (UINT32)r16(v, C_QNOTIFY) * v->notify_mul);
    c16(v, C_QENABLE, 1);
    return true;
}

static bool hw_setup(Vin *v)
{
    c8(v, C_STATUS, 0);
    for (int i = 0; i < 100000 && r8(v, C_STATUS); i++) pause_cpu();
    c8(v, C_STATUS, S_ACK);
    c8(v, C_STATUS, S_ACK | S_DRIVER);
    c32(v, C_DFSELECT, 1);
    if (!(r32(v, C_DF) & F_VERSION_1_HI)) { c8(v, C_STATUS, S_FAILED); return false; }
    c32(v, C_GFSELECT, 0); c32(v, C_GF, 0);
    c32(v, C_GFSELECT, 1); c32(v, C_GF, F_VERSION_1_HI);
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    if (!(r8(v, C_STATUS) & S_FEATURES_OK)) { c8(v, C_STATUS, S_FAILED); return false; }
    if (!queue_setup(v)) { c8(v, C_STATUS, S_FAILED); return false; }
    for (int s = 0; s < TOUCH_MAX; s++) v->c[s].down = v->c[s].changed = false;
    v->slot = 0;
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    *v->notify = 0;
    return true;
}

static void probe(const PciDevice *pci)
{
    Vin *v = kzalloc(sizeof(Vin));
    if (!v) return;
    v->pci = *pci;
    PciEnableDevice(&v->pci);
    if (!find_caps(v)) { kfree(v); return; }
    int n = cfg_query(v, CFG_ID_NAME, 0);
    for (int i = 0; i < n && i < (int)sizeof(v->name) - 1; i++) v->name[i] = (char)v->dev[CFG_DATA + i];
    /* A multi-touch screen sends ABS_MT_SLOT and the MT positions */
    bool mt = false;
    if (cfg_query(v, CFG_EV_BITS, EV_ABS) > ABS_MT_TRACKING_ID / 8) {
        UINT8 b5 = v->dev[CFG_DATA + 5], b6 = v->dev[CFG_DATA + 6], b7 = v->dev[CFG_DATA + 7];
        mt = (b5 & (1u << (ABS_MT_SLOT % 8))) && (b6 & (1u << (ABS_MT_POSITION_X % 8))) &&
             (b6 & (1u << (ABS_MT_POSITION_Y % 8))) && (b7 & (1u << (ABS_MT_TRACKING_ID % 8)));
    }
    if (!mt) {
        kprintf("[VIRTIO] Input device \"%s\" at %02x:%02x.%x: not a touch screen, not used\n",
                v->name, pci->bus, pci->dev, pci->func);
        kfree(v);
        return;
    }
    int slots = TOUCH_MAX;
    if (cfg_query(v, CFG_ABS_INFO, ABS_MT_SLOT)) slots = cfg_le32(v, 4) + 1;
    if (cfg_query(v, CFG_ABS_INFO, ABS_MT_POSITION_X)) { v->xmin = cfg_le32(v, 0); v->xmax = cfg_le32(v, 4); }
    if (cfg_query(v, CFG_ABS_INFO, ABS_MT_POSITION_Y)) { v->ymin = cfg_le32(v, 0); v->ymax = cfg_le32(v, 4); }
    v->desc  = kernel_alloc_pages(1);
    v->avail = kernel_alloc_pages(1);
    v->used  = kernel_alloc_pages(1);
    v->ev    = kernel_alloc_pages(1);
    if (!v->desc || !v->avail || !v->used || !v->ev || !hw_setup(v)) {
        kprintf("[VIRTIO] Touch screen \"%s\" refused the setup\n", v->name);
        return;                                              /* (leaked: a failed device stays failed) */
    }
    if (slots > TOUCH_MAX) slots = TOUCH_MAX;
    InputTouchScreen(slots);
    g_dev[g_ndev++] = v;
    kprintf("[VIRTIO] Multi-touch screen \"%s\" at %02x:%02x.%x: %d contacts, X %d-%d, Y %d-%d\n",
            v->name, pci->bus, pci->dev, pci->func, slots, v->xmin, v->xmax, v->ymin, v->ymax);
}

void VirtioInputInit(void)
{
    PciDevice d;
    for (int i = 0; g_ndev < MAX_DEV && PciFindClass(0x09, 0x80, 0, i, &d); i++)   /* "other input" */
        if (d.vendor == 0x1AF4 && d.device == 0x1052) probe(&d);
}

void VirtioInputResume(void)
{
    for (int i = 0; i < g_ndev; i++)
        if (!hw_setup(g_dev[i])) kprintf("[VIRTIO] Touch screen \"%s\" didn't come back after sleep\n", g_dev[i]->name);
}

static INT32 scale(INT32 v, INT32 lo, INT32 hi)
{
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (INT32)((INT64)(v - lo) * 65535 / (hi - lo));
}

static void event(Vin *v, const Event *e)
{
    if (e->type == EV_ABS) {
        INT32 val = (INT32)e->value;
        switch (e->code) {
        case ABS_MT_SLOT:        v->slot = val >= 0 && val < TOUCH_MAX ? val : -1; break;
        case ABS_MT_TRACKING_ID:
            if (v->slot < 0) break;
            v->c[v->slot].down = val >= 0;
            v->c[v->slot].changed = true;
            break;
        case ABS_MT_POSITION_X:  if (v->slot >= 0) { v->c[v->slot].x = scale(val, v->xmin, v->xmax); v->c[v->slot].changed = true; } break;
        case ABS_MT_POSITION_Y:  if (v->slot >= 0) { v->c[v->slot].y = scale(val, v->ymin, v->ymax); v->c[v->slot].changed = true; } break;
        }
    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        bool any = false;
        for (int s = 0; s < TOUCH_MAX; s++) {
            if (!v->c[s].changed) continue;
            v->c[s].changed = false;
            InputEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = INPUT_TOUCH;
            ev.contact = (UINT8)s;
            ev.pressed = v->c[s].down;
            ev.absolute = 1;
            ev.dx = v->c[s].x;
            ev.dy = v->c[s].y;
            InputPost(&ev);
            any = true;
        }
        if (any) {
            InputEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = INPUT_TOUCH;
            ev.contact = TOUCH_FRAME;
            InputPost(&ev);
        }
    }
}

void VirtioInputPoll(void)
{
    for (int i = 0; i < g_ndev; i++) {
        Vin *v = g_dev[i];
        bool gave = false;
        while (v->last_used != v->used->idx) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            UINT32 id = v->used->ring[v->last_used % v->size].id;
            v->last_used++;
            if (id >= v->size) continue;
            Event e = v->ev[id];
            event(v, &e);
            UINT16 idx = v->avail->idx;                     /* the buffer goes straight back */
            v->avail->ring[idx % v->size] = (UINT16)id;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            v->avail->idx = (UINT16)(idx + 1);
            gave = true;
        }
        if (gave) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            *v->notify = 0;
        }
    }
}
