/*
 * virtio_input.c — virtio input devices: multi-touch screens, pens and
 * tablets
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
 * end, exactly as from a USB touch screen (usbhid.c).
 *
 * Pens (a device with absolute X/Y and BTN_TOOL_PEN or ABS_PRESSURE, as
 * Linux's evdev pens and QEMU's virtio-input-host passing one through)
 * post an INPUT_PEN event a frame and then the pointer's motion, as a USB pen
 * does: BTN_TOOL_PEN / BTN_TOOL_RUBBER say it is in range (with its tip or
 * its eraser), BTN_TOUCH is the tip, BTN_STYLUS / BTN_STYLUS2 the barrel
 * buttons, ABS_PRESSURE the pressure, ABS_TILT_X / ABS_TILT_Y the tilt
 * (their resolution is units per radian; none: degrees) and ABS_Z the
 * barrel rotation (Wacom's Art Pen: its range is the full turn).  Other
 * devices with absolute X/Y (QEMU's virtio-tablet-pci) are absolute
 * pointers: buttons, the wheels and the position, as a USB tablet.
 * Keyboards and relative mice are only logged: NovaOS uses the PS/2 and
 * USB ones.
 *
 * The PCI setup is virtio_net.c's (capabilities, the handshake, split
 * queues); the event queue asks for no interrupts and is polled by the
 * desktop loop.
 */

#include "virtio_input.h"
#include "../hal/pci.h"
#include "../wm/input.h"
#include "../wm/tablet.h"
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
#define EV_KEY       1
#define EV_REL       2
#define EV_ABS       3
#define SYN_REPORT   0
#define REL_HWHEEL   0x06
#define REL_WHEEL    0x08
#define ABS_X        0x00
#define ABS_Y        0x01
#define ABS_Z        0x02
#define ABS_PRESSURE 0x18
#define ABS_TILT_X   0x1A
#define ABS_TILT_Y   0x1B
#define BTN_LEFT     0x110
#define BTN_RIGHT    0x111
#define BTN_MIDDLE   0x112
#define BTN_SIDE     0x113
#define BTN_EXTRA    0x114
#define BTN_TOOL_PEN 0x140
#define BTN_TOOL_RUBBER 0x141
#define BTN_TOUCH    0x14A
#define BTN_STYLUS   0x14B
#define BTN_STYLUS2  0x14C
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
    enum { VIN_TOUCH, VIN_PEN, VIN_POINTER } kind;
    INT32            xmin, xmax, ymin, ymax;
    /* multi-touch */
    int              slot;
    struct { INT32 x, y; bool down, changed; } c[TOUCH_MAX];
    /* pens and pointers: the state so far (a frame ends at SYN_REPORT) */
    INT32            pmin, pmax, tres, zmin, zmax;  /* pressure, tilt's units per radian, ABS_Z's range */
    int              caps;                          /* TABLET_CAP_* */
    bool             has_tool, has_pressure;        /* BTN_TOOL_PEN, ABS_PRESSURE */
    INT32            x, y, press, tx, ty, z;
    bool             pen, rubber, touch, changed;
    UINT8            buttons;                       /* MOUSE_* (pointers); pens: 2 barrel, 4 second barrel */
    INT32            wheel, hwheel;
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
    v->pen = v->rubber = v->touch = v->changed = false;
    v->buttons = 0;
    v->wheel = v->hwheel = 0;
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    *v->notify = 0;
    return true;
}

/* Is bit @code set in the answer to (CFG_EV_BITS, @type)? */
static bool ev_bit(Vin *v, UINT8 type, int code)
{
    int n = cfg_query(v, CFG_EV_BITS, type);
    return code / 8 < n && (v->dev[CFG_DATA + code / 8] & (1u << (code % 8)));
}

/* An absolute axis's range (and resolution); false if it has none */
static bool abs_info(Vin *v, int code, INT32 *lo, INT32 *hi, INT32 *res)
{
    if (!ev_bit(v, EV_ABS, code) || !cfg_query(v, CFG_ABS_INFO, (UINT8)code)) return false;
    *lo = cfg_le32(v, 0);
    *hi = cfg_le32(v, 4);
    if (res) *res = cfg_le32(v, 16);
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
    /* A multi-touch screen sends ABS_MT_SLOT and the MT positions; a pen
     * or a tablet absolute X and Y */
    bool mt = ev_bit(v, EV_ABS, ABS_MT_SLOT) && ev_bit(v, EV_ABS, ABS_MT_POSITION_X) &&
              ev_bit(v, EV_ABS, ABS_MT_POSITION_Y) && ev_bit(v, EV_ABS, ABS_MT_TRACKING_ID);
    INT32 lo, hi, res;
    int slots = TOUCH_MAX;
    if (mt) {
        v->kind = VIN_TOUCH;
        if (cfg_query(v, CFG_ABS_INFO, ABS_MT_SLOT)) slots = cfg_le32(v, 4) + 1;
        if (cfg_query(v, CFG_ABS_INFO, ABS_MT_POSITION_X)) { v->xmin = cfg_le32(v, 0); v->xmax = cfg_le32(v, 4); }
        if (cfg_query(v, CFG_ABS_INFO, ABS_MT_POSITION_Y)) { v->ymin = cfg_le32(v, 0); v->ymax = cfg_le32(v, 4); }
    } else if (abs_info(v, ABS_X, &v->xmin, &v->xmax, NULL) && abs_info(v, ABS_Y, &v->ymin, &v->ymax, NULL)) {
        v->has_tool = ev_bit(v, EV_KEY, BTN_TOOL_PEN);
        v->has_pressure = abs_info(v, ABS_PRESSURE, &v->pmin, &v->pmax, NULL);
        v->kind = v->has_tool || v->has_pressure ? VIN_PEN : VIN_POINTER;
        if (v->kind == VIN_PEN) {
            if (abs_info(v, ABS_TILT_X, &lo, &hi, &res) && abs_info(v, ABS_TILT_Y, &lo, &hi, &v->tres))
                v->caps |= TABLET_CAP_TILT;
            if (abs_info(v, ABS_Z, &v->zmin, &v->zmax, NULL) && v->zmax > v->zmin) v->caps |= TABLET_CAP_TWIST;
        }
    } else {
        kprintf("[VIRTIO] Input device \"%s\" at %02x:%02x.%x: no absolute position, not used\n",
                v->name, pci->bus, pci->dev, pci->func);
        kfree(v);
        return;
    }
    v->desc  = kernel_alloc_pages(1);
    v->avail = kernel_alloc_pages(1);
    v->used  = kernel_alloc_pages(1);
    v->ev    = kernel_alloc_pages(1);
    if (!v->desc || !v->avail || !v->used || !v->ev || !hw_setup(v)) {
        kprintf("[VIRTIO] Input device \"%s\" refused the setup\n", v->name);
        return;                                              /* (leaked: a failed device stays failed) */
    }
    g_dev[g_ndev++] = v;
    PciClaim(pci, "virtio-input");
    if (v->kind == VIN_TOUCH) {
        if (slots > TOUCH_MAX) slots = TOUCH_MAX;
        InputTouchScreen(slots);
        kprintf("[VIRTIO] Multi-touch screen \"%s\" at %02x:%02x.%x: %d contacts, X %d-%d, Y %d-%d\n",
                v->name, pci->bus, pci->dev, pci->func, slots, v->xmin, v->xmax, v->ymin, v->ymax);
    } else if (v->kind == VIN_PEN) {
        TabletDevice(NULL, 1, v->caps);
        kprintf("[VIRTIO] Pen \"%s\" at %02x:%02x.%x: X %d-%d, Y %d-%d%s%s%s\n",
                v->name, pci->bus, pci->dev, pci->func, v->xmin, v->xmax, v->ymin, v->ymax,
                v->has_pressure ? ", pressure" : "", v->caps & TABLET_CAP_TILT ? ", tilt" : "",
                v->caps & TABLET_CAP_TWIST ? ", rotation" : "");
    } else {
        kprintf("[VIRTIO] Tablet \"%s\" at %02x:%02x.%x: absolute pointer, X %d-%d, Y %d-%d\n",
                v->name, pci->bus, pci->dev, pci->func, v->xmin, v->xmax, v->ymin, v->ymax);
    }
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
        if (!hw_setup(g_dev[i])) kprintf("[VIRTIO] Input device \"%s\" didn't come back after sleep\n", g_dev[i]->name);
}

static INT32 scale(INT32 v, INT32 lo, INT32 hi)
{
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (INT32)((INT64)(v - lo) * 65535 / (hi - lo));
}

/* usbcheck's device: its events are collected instead of posted */
static const Vin  *g_check_vin;
static InputEvent  g_check_ev[16];
static int         g_check_n;

static void post(const Vin *v, const InputEvent *ev)
{
    if (v == g_check_vin) {
        if (g_check_n < 16) g_check_ev[g_check_n++] = *ev;
        return;
    }
    InputPost(ev);
}

/* A tilt in tenths of a degree: @res units a radian (0: degrees) */
static INT32 tilt10(INT32 val, INT32 res)
{
    INT64 t = res > 0 ? ((INT64)val * 5729578 / 10000 + (val < 0 ? -res / 2 : res / 2)) / res : (INT64)val * 10;
    return (INT32)(t < -900 ? -900 : t > 900 ? 900 : t);
}

/* A pen's or a pointer's frame is over */
static void pen_frame(Vin *v)
{
    if (!v->changed) return;
    v->changed = false;
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.absolute = 1;
    ev.dx = scale(v->x, v->xmin, v->xmax);
    ev.dy = scale(v->y, v->ymin, v->ymax);
    if (v->kind == VIN_POINTER) {
        ev.type = INPUT_MOUSE;
        ev.buttons = v->buttons;
        ev.dz = v->wheel;
        ev.dw = v->hwheel;
        v->wheel = v->hwheel = 0;
        post(v, &ev);
        return;
    }
    bool near = v->has_tool ? v->pen || v->rubber : v->touch;   /* (no BTN_TOOL_*: in range while touching) */
    UINT8 pb = (UINT8)((v->touch ? 1 : 0) | (v->buttons & 6));
    ev.type = INPUT_PEN;
    ev.buttons = pb;
    ev.pressure = (UINT16)(!(pb & 1) ? 0 : !v->has_pressure ? TABLET_PRESSURE :
                           v->pmax > v->pmin ? (v->press <= v->pmin ? 0 : v->press >= v->pmax ? TABLET_PRESSURE :
                                                (INT32)((INT64)(v->press - v->pmin) * TABLET_PRESSURE / (v->pmax - v->pmin))) : 0);
    ev.pressed = near || pb;
    ev.extended = v->rubber;
    ev.pen_has = (UINT8)((v->caps & TABLET_CAP_TILT ? PEN_HAS_TILT : 0) | (v->caps & TABLET_CAP_TWIST ? PEN_HAS_TWIST : 0));
    if (v->caps & TABLET_CAP_TILT) {
        ev.tilt_x = (INT16)tilt10(v->tx, v->tres);
        ev.tilt_y = (INT16)tilt10(v->ty, v->tres);
    }
    if (v->caps & TABLET_CAP_TWIST) {                    /* the Z range is a full turn, its middle "north" */
        INT32 t = (INT32)((INT64)(v->z - v->zmin) * 3600 / ((INT64)v->zmax - v->zmin + 1));
        ev.twist = (UINT16)(((t + 1800) % 3600 + 3600) % 3600);
    }
    post(v, &ev);
    if (near || pb) {                                    /* the pointer follows the pen in range */
        InputEvent m;
        memset(&m, 0, sizeof(m));
        m.type = INPUT_MOUSE;
        m.absolute = 1;
        m.dx = ev.dx; m.dy = ev.dy;
        m.buttons = (UINT8)((pb & 1 ? MOUSE_LEFT : 0) | (pb & 2 ? MOUSE_RIGHT : 0));
        m.from_pen = 1;
        post(v, &m);
    }
}

static void pen_event(Vin *v, const Event *e)
{
    INT32 val = (INT32)e->value;
    if (e->type == EV_ABS) {
        switch (e->code) {
        case ABS_X:        v->x = val; break;
        case ABS_Y:        v->y = val; break;
        case ABS_Z:        v->z = val; break;
        case ABS_PRESSURE: v->press = val; break;
        case ABS_TILT_X:   v->tx = val; break;
        case ABS_TILT_Y:   v->ty = val; break;
        default: return;
        }
    } else if (e->type == EV_KEY) {
        UINT8 bit = 0;
        switch (e->code) {
        case BTN_TOOL_PEN:    v->pen = val != 0; break;
        case BTN_TOOL_RUBBER: v->rubber = val != 0; break;
        case BTN_TOUCH:       v->touch = val != 0; break;
        case BTN_LEFT:        if (v->kind == VIN_PEN) v->touch = val != 0; else bit = MOUSE_LEFT; break;
        case BTN_RIGHT:       bit = MOUSE_RIGHT; break;
        case BTN_MIDDLE:      bit = MOUSE_MIDDLE; break;
        case BTN_SIDE:        bit = MOUSE_X1; break;
        case BTN_EXTRA:       bit = MOUSE_X2; break;
        case BTN_STYLUS:      bit = 2; break;           /* (a pen's barrel buttons: TabletPacket's bits) */
        case BTN_STYLUS2:     bit = 4; break;
        default: return;
        }
        if (bit) v->buttons = val ? v->buttons | bit : v->buttons & (UINT8)~bit;
    } else if (e->type == EV_REL) {
        if (e->code == REL_WHEEL) v->wheel += val;
        else if (e->code == REL_HWHEEL) v->hwheel += val;
        else return;
    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        pen_frame(v);
        return;
    } else return;
    v->changed = true;
}

static void event(Vin *v, const Event *e)
{
    if (v->kind != VIN_TOUCH) { pen_event(v, e); return; }
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
            post(v, &ev);
            any = true;
        }
        if (any) {
            InputEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = INPUT_TOUCH;
            ev.contact = TOUCH_FRAME;
            post(v, &ev);
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

/* usbcheck: the pen decoding on canned evdev events (QEMU passes a pen
 * through only from a host's /dev/input) */
static void describe(char *out, int cap)
{
    int n = 0;
    out[0] = '\0';
    for (int i = 0; i < g_check_n && n < cap - 1; i++) {
        const InputEvent *e = &g_check_ev[i];
        if (e->type == INPUT_PEN)
            n += ksnprintf(out + n, cap - n, "%sp%d,%X%s%s/%d,%d,%d @%d,%d", i ? " " : "", e->pressure, e->buttons,
                           e->pressed ? "r" : "", e->extended ? "e" : "", e->tilt_x, e->tilt_y, e->twist,
                           (e->dx * 100 + 32767) / 65535, (e->dy * 100 + 32767) / 65535);
        else
            n += ksnprintf(out + n, cap - n, "%sm%X,%d", i ? " " : "", e->buttons, e->dz);
    }
}

int VirtioInputSelfCheck(void (*say)(void *ctx, const char *line), void *ctx)
{
    #define E(t, c, v) { t, c, (UINT32)(v) }
    /* A Wacom-like pen: X/Y 0-20000, pressure 0-4095, tilt -64..63 at 57
     * units a radian (one a degree), Art Pen rotation -900..899: hover,
     * touch at half pressure leaning 30 right and 20 toward the user
     * turned a quarter, press hard with the barrel button, turn over to
     * the eraser, leave */
    static const Event pen[] = {
        E(EV_KEY, BTN_TOOL_PEN, 1), E(EV_ABS, ABS_X, 5000), E(EV_ABS, ABS_Y, 10000), E(EV_SYN, SYN_REPORT, 0),
        E(EV_KEY, BTN_TOUCH, 1), E(EV_ABS, ABS_PRESSURE, 2048), E(EV_ABS, ABS_TILT_X, 30), E(EV_ABS, ABS_TILT_Y, 20),
        E(EV_ABS, ABS_Z, 450), E(EV_SYN, SYN_REPORT, 0),
        E(EV_ABS, ABS_PRESSURE, 4095), E(EV_KEY, BTN_STYLUS, 1), E(EV_ABS, ABS_TILT_X, -64), E(EV_SYN, SYN_REPORT, 0),
        E(EV_KEY, BTN_TOUCH, 0), E(EV_KEY, BTN_STYLUS, 0), E(EV_KEY, BTN_TOOL_PEN, 0), E(EV_KEY, BTN_TOOL_RUBBER, 1),
        E(EV_ABS, ABS_PRESSURE, 0), E(EV_ABS, ABS_TILT_X, 0), E(EV_ABS, ABS_TILT_Y, 0), E(EV_ABS, ABS_Z, 0), E(EV_SYN, SYN_REPORT, 0),
        E(EV_KEY, BTN_TOOL_RUBBER, 0), E(EV_SYN, SYN_REPORT, 0),
    };
    /* QEMU's virtio-tablet: X/Y 0-32767, buttons, the wheel */
    static const Event tablet[] = {
        E(EV_ABS, ABS_X, 16384), E(EV_ABS, ABS_Y, 8192), E(EV_SYN, SYN_REPORT, 0),
        E(EV_KEY, BTN_LEFT, 1), E(EV_SYN, SYN_REPORT, 0),
        E(EV_KEY, BTN_LEFT, 0), E(EV_REL, REL_WHEEL, -1), E(EV_SYN, SYN_REPORT, 0),
    };
    #undef E
    static const struct { const char *what; int kind; const Event *ev; int n; const char *want; } checks[] = {
        { "virtio pen: pressure, tilt, rotation, eraser", VIN_PEN, pen, sizeof(pen) / sizeof(pen[0]),
          "p0,0r/0,0,0 @25,50 m0,0 p511,1r/302,201,900 @25,50 m1,0 p1023,3r/-643,201,900 @25,50 m3,0 "
          "p0,0re/0,0,0 @25,50 m0,0 p0,0/0,0,0 @25,50" },
        { "virtio tablet: absolute pointer", VIN_POINTER, tablet, sizeof(tablet) / sizeof(tablet[0]),
          "m0,0 m1,0 m0,-1" },
    };
    int failed = 0;
    for (unsigned c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
        Vin *v = kzalloc(sizeof(Vin));
        if (!v) return -1;
        v->kind = checks[c].kind;
        if (v->kind == VIN_PEN) {
            v->xmax = v->ymax = 20000;
            v->has_tool = v->has_pressure = true;
            v->pmax = 4095;
            v->tres = 57;
            v->zmin = -900; v->zmax = 899;
            v->caps = TABLET_CAP_TILT | TABLET_CAP_TWIST;
        } else v->xmax = v->ymax = 32767;
        g_check_n = 0;
        g_check_vin = v;
        for (int i = 0; i < checks[c].n; i++) event(v, &checks[c].ev[i]);
        g_check_vin = NULL;
        char got[320], line[400];
        describe(got, sizeof(got));
        bool pass = strcmp(got, checks[c].want) == 0;
        if (pass) ksnprintf(line, sizeof(line), "ok   %s: %s", checks[c].what, got);
        else      ksnprintf(line, sizeof(line), "FAIL %s: %s (want %s)", checks[c].what, got, checks[c].want);
        say(ctx, line);
        if (!pass) failed++;
        kfree(v);
    }
    return failed;
}
