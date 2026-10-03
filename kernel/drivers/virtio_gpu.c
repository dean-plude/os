/*
 * virtio_gpu.c — virtio GPU, 2D: several monitors on one card
 *
 * A virtio GPU (virtio 1.2, section 5.7) takes commands on its control
 * queue.  For 2D, a "resource" is a picture the card holds, backed by
 * pages of guest memory: RESOURCE_CREATE_2D makes one, ATTACH_BACKING
 * hands it the memory, SET_SCANOUT shows it on an output, and after the
 * guest draws, TRANSFER_TO_HOST_2D copies the changed rectangle into the
 * card's copy and RESOURCE_FLUSH puts it on the output.  GET_DISPLAY_INFO
 * says which of the card's outputs (up to 16) have a monitor, and the size
 * each monitor prefers; when that changes the card sets
 * VIRTIO_GPU_EVENT_DISPLAY in its configuration space.
 *
 * Each picture is one physically contiguous block, so the GDI and the
 * framebuffer console can draw on it like on video memory.  Commands are
 * synchronous: the control queue asks for no interrupts and the driver
 * waits for the card's answer (a copy in QEMU's main loop).  The PCI setup
 * is virtio_input.c's (capabilities, the handshake, split queues).
 *
 * virtio-vga is also VGA-compatible: until the first SET_SCANOUT, output
 * 0 shows its VGA/DISPI framebuffer (what the GOP and display.c's DISPI
 * driver use).  Once any output is set it shows only resources, so
 * display.c moves the boot display onto a resource too (DRV_VIRTIO) before
 * it uses the card's other outputs.
 */

#include "virtio_gpu.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/spinlock.h"
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

/* Device configuration (5.7.4) */
#define CFG_EVENTS_READ  0
#define CFG_EVENTS_CLEAR 4
#define CFG_NUM_SCANOUTS 8
#define EVENT_DISPLAY    1

/* Commands and answers (5.7.6.7) */
#define CMD_GET_DISPLAY_INFO     0x0100
#define CMD_RESOURCE_CREATE_2D   0x0101
#define CMD_RESOURCE_UNREF       0x0102
#define CMD_SET_SCANOUT          0x0103
#define CMD_RESOURCE_FLUSH       0x0104
#define CMD_TRANSFER_TO_HOST_2D  0x0105
#define CMD_ATTACH_BACKING       0x0106
#define CMD_DETACH_BACKING       0x0107
#define RESP_OK_NODATA           0x1100
#define RESP_OK_DISPLAY_INFO     0x1101
#define FORMAT_B8G8R8X8          2         /* bytes B, G, R, X: little-endian XRGB */

#define N_DESC       8
#define DESC_F_NEXT  1
#define DESC_F_WRITE 2
#define AVAIL_F_NO_INTERRUPT 1

typedef struct __attribute__((packed)) { UINT64 addr; UINT32 len; UINT16 flags, next; } Desc;
typedef struct __attribute__((packed)) { UINT16 flags, idx, ring[N_DESC], used_event; } Avail;
typedef struct __attribute__((packed)) { UINT32 id, len; } UsedElem;
typedef struct __attribute__((packed)) { UINT16 flags, idx; UsedElem ring[N_DESC]; UINT16 avail_event; } Used;

typedef struct __attribute__((packed)) {
    UINT32 type, flags;
    UINT64 fence;
    UINT32 ctx;
    UINT8  ring, pad[3];
} Hdr;
typedef struct __attribute__((packed)) { UINT32 x, y, w, h; } Rect;
typedef struct __attribute__((packed)) { Hdr h; struct { Rect r; UINT32 enabled, flags; } mode[16]; } DisplayInfo;
typedef struct __attribute__((packed)) { Hdr h; UINT32 id, format, w, h2; } Create2D;
typedef struct __attribute__((packed)) { Hdr h; UINT32 id, pad; } Unref;
typedef struct __attribute__((packed)) { Hdr h; Rect r; UINT32 scanout, id; } SetScanout;
typedef struct __attribute__((packed)) { Hdr h; Rect r; UINT32 id, pad; } Flush;
typedef struct __attribute__((packed)) { Hdr h; Rect r; UINT64 offset; UINT32 id, pad; } Transfer;
typedef struct __attribute__((packed)) {
    Hdr h; UINT32 id, n;
    struct { UINT64 addr; UINT32 len, pad; } e[1];
} Attach;

typedef struct {
    UINT32  *px;                          /* the picture (NULL: output off) */
    int      w, h;
    UINT32   id;                          /* its resource */
    size_t   pages;
} Out;

typedef struct {
    PciDevice        pci;
    const char      *name;
    UINT64           fb;                  /* BAR0 (virtio-vga's VGA framebuffer) */
    volatile UINT8  *common, *notify_base, *dev;
    UINT32           notify_mul;
    Desc            *desc;
    volatile Avail  *avail;
    volatile Used   *used;
    UINT8           *buf;                 /* two requests and their answers */
    UINT16           size, last_used;
    volatile UINT16 *notify;
    int              nscan;
    UINT32           next_id;
    Out              out[VGPU_MAX_SCANOUTS];
} Vgpu;

static Vgpu      *g_dev[VGPU_MAX_DEVICES];
static int        g_ndev;
static KSpinLock  g_lock = KSPINLOCK_INIT;

static inline UINT64 phys(const void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }
static inline void   c8(Vgpu *v, UINT32 o, UINT8 x)   { *(volatile UINT8 *)(v->common + o) = x; }
static inline void   c16(Vgpu *v, UINT32 o, UINT16 x) { *(volatile UINT16 *)(v->common + o) = x; }
static inline void   c32(Vgpu *v, UINT32 o, UINT32 x) { *(volatile UINT32 *)(v->common + o) = x; }
static inline void   c64(Vgpu *v, UINT32 o, UINT64 x) { c32(v, o, (UINT32)x); c32(v, o + 4, (UINT32)(x >> 32)); }
static inline UINT8  r8(Vgpu *v, UINT32 o)  { return *(volatile UINT8 *)(v->common + o); }
static inline UINT16 r16(Vgpu *v, UINT32 o) { return *(volatile UINT16 *)(v->common + o); }
static inline UINT32 r32(Vgpu *v, UINT32 o) { return *(volatile UINT32 *)(v->common + o); }
static inline UINT32 dev32(Vgpu *v, UINT32 o) { return *(volatile UINT32 *)(v->dev + o); }

static bool find_caps(Vgpu *v)
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

/* The control queue (0); the cursor queue (1) stays off */
static bool queue_setup(Vgpu *v)
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
    c64(v, C_QDESC, phys(v->desc));
    c64(v, C_QDRIVER, phys((const void *)v->avail));
    c64(v, C_QDEVICE, phys((const void *)v->used));
    v->notify = (volatile UINT16 *)(v->notify_base + (UINT32)r16(v, C_QNOTIFY) * v->notify_mul);
    c16(v, C_QENABLE, 1);
    return true;
}

static bool hw_setup(Vgpu *v)
{
    c8(v, C_STATUS, 0);
    for (int i = 0; i < 100000 && r8(v, C_STATUS); i++) pause_cpu();
    c8(v, C_STATUS, S_ACK);
    c8(v, C_STATUS, S_ACK | S_DRIVER);
    c32(v, C_DFSELECT, 1);
    if (!(r32(v, C_DF) & F_VERSION_1_HI)) { c8(v, C_STATUS, S_FAILED); return false; }
    c32(v, C_GFSELECT, 0); c32(v, C_GF, 0);                  /* no VIRGL, no EDID: plain 2D */
    c32(v, C_GFSELECT, 1); c32(v, C_GF, F_VERSION_1_HI);
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    if (!(r8(v, C_STATUS) & S_FEATURES_OK)) { c8(v, C_STATUS, S_FAILED); return false; }
    if (!queue_setup(v)) { c8(v, C_STATUS, S_FAILED); return false; }
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    return true;
}

/* Send @n requests (request i at buf + i * 1024, its answer at
 * buf + 512 + i * 1024, @rlen[i] bytes) and wait for the answers; under
 * g_lock.  True when every answer is one of the OK ones. */
static bool submit(Vgpu *v, int n, const UINT32 *qlen, const UINT32 *rlen)
{
    UINT16 idx = v->avail->idx;
    for (int i = 0; i < n; i++) {
        int d = 2 * i;
        v->desc[d].addr = phys(v->buf + i * 1024);
        v->desc[d].len = qlen[i];
        v->desc[d].flags = DESC_F_NEXT;
        v->desc[d].next = (UINT16)(d + 1);
        v->desc[d + 1].addr = phys(v->buf + 512 + i * 1024);
        v->desc[d + 1].len = rlen[i];
        v->desc[d + 1].flags = DESC_F_WRITE;
        v->desc[d + 1].next = 0;
        v->avail->ring[(UINT16)(idx + i) % v->size] = (UINT16)d;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->avail->idx = (UINT16)(idx + n);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *v->notify = 0;
    UINT16 want = (UINT16)(v->last_used + n);
    for (UINT64 spin = 0; v->used->idx != want; spin++) {
        if (spin > 400000000ull) {
            kprintf("[VGPU] %s: no answer to command %x\n", v->name, ((Hdr *)v->buf)->type);
            v->last_used = v->used->idx;
            return false;
        }
        pause_cpu();
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->last_used = want;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        UINT32 t = ((Hdr *)(v->buf + 512 + i * 1024))->type;
        if (t != RESP_OK_NODATA && t != RESP_OK_DISPLAY_INFO) {
            kprintf("[VGPU] %s: command %x answered %x\n", v->name, ((Hdr *)(v->buf + i * 1024))->type, t);
            ok = false;
        }
    }
    return ok;
}

static void *req(Vgpu *v, int i, UINT32 type, UINT32 len)
{
    void *p = v->buf + i * 1024;
    memset(p, 0, len);
    ((Hdr *)p)->type = type;
    return p;
}

static bool cmd(Vgpu *v, UINT32 len)
{
    UINT32 rl = sizeof(Hdr);
    return submit(v, 1, &len, &rl);
}

/* GET_DISPLAY_INFO into buf + 512; under g_lock */
static const DisplayInfo *display_info(Vgpu *v)
{
    req(v, 0, CMD_GET_DISPLAY_INFO, sizeof(Hdr));
    UINT32 ql = sizeof(Hdr), rl = sizeof(DisplayInfo);
    if (!submit(v, 1, &ql, &rl)) return NULL;
    return (const DisplayInfo *)(v->buf + 512);
}

/* Make a resource for @o's picture and show it on @scanout; under g_lock */
static bool show_locked(Vgpu *v, int scanout, Out *o)
{
    o->id = ++v->next_id;
    Create2D *c = req(v, 0, CMD_RESOURCE_CREATE_2D, sizeof(Create2D));
    c->id = o->id; c->format = FORMAT_B8G8R8X8; c->w = (UINT32)o->w; c->h2 = (UINT32)o->h;
    if (!cmd(v, sizeof(Create2D))) return false;
    Attach *a = req(v, 0, CMD_ATTACH_BACKING, sizeof(Attach));
    a->id = o->id; a->n = 1;
    a->e[0].addr = phys(o->px); a->e[0].len = (UINT32)((size_t)o->w * o->h * 4);
    if (!cmd(v, sizeof(Attach))) return false;
    SetScanout *s = req(v, 0, CMD_SET_SCANOUT, sizeof(SetScanout));
    s->r = (Rect){ 0, 0, (UINT32)o->w, (UINT32)o->h }; s->scanout = (UINT32)scanout; s->id = o->id;
    return cmd(v, sizeof(SetScanout));
}

static void unref_locked(Vgpu *v, UINT32 id)
{
    if (!id) return;
    Unref *u = req(v, 0, CMD_DETACH_BACKING, sizeof(Unref));
    u->id = id;
    cmd(v, sizeof(Unref));
    u = req(v, 0, CMD_RESOURCE_UNREF, sizeof(Unref));
    u->id = id;
    cmd(v, sizeof(Unref));
}

static void flush_locked(Vgpu *v, int scanout, int x, int y, int w, int h)
{
    Out *o = &v->out[scanout];
    if (!o->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > o->w) w = o->w - x;
    if (y + h > o->h) h = o->h - y;
    if (w <= 0 || h <= 0) return;
    Rect r = { (UINT32)x, (UINT32)y, (UINT32)w, (UINT32)h };
    Transfer *t = req(v, 0, CMD_TRANSFER_TO_HOST_2D, sizeof(Transfer));
    t->r = r; t->offset = ((UINT64)y * o->w + x) * 4; t->id = o->id;
    Flush *f = req(v, 1, CMD_RESOURCE_FLUSH, sizeof(Flush));
    f->r = r; f->id = o->id;
    UINT32 ql[2] = { sizeof(Transfer), sizeof(Flush) }, rl[2] = { sizeof(Hdr), sizeof(Hdr) };
    submit(v, 2, ql, rl);
}

static void probe(const PciDevice *pci)
{
    Vgpu *v = kzalloc(sizeof(Vgpu));
    if (!v) return;
    v->pci = *pci;
    bool vga = pci->class_code == 0x03 && pci->subclass == 0x00;
    v->name = vga ? "QEMU virtio-vga" : "QEMU virtio-gpu";
    v->fb = vga ? PciBarAddress(pci, 0) : 0;
    PciEnableDevice(&v->pci);
    if (!find_caps(v)) { kfree(v); return; }
    v->desc  = kernel_alloc_pages(1);
    v->avail = kernel_alloc_pages(1);
    v->used  = kernel_alloc_pages(1);
    v->buf   = kernel_alloc_pages(1);
    if (!v->desc || !v->avail || !v->used || !v->buf || !hw_setup(v)) {
        kprintf("[VGPU] %s at %02x:%02x.%x refused the setup\n", v->name, pci->bus, pci->dev, pci->func);
        return;                                              /* (leaked: a failed device stays failed) */
    }
    v->nscan = (int)dev32(v, CFG_NUM_SCANOUTS);
    if (v->nscan > VGPU_MAX_SCANOUTS) v->nscan = VGPU_MAX_SCANOUTS;
    if (v->nscan < 1) v->nscan = 1;
    g_dev[g_ndev++] = v;
    kprintf("[VGPU] %s at %02x:%02x.%x: %d output(s)\n", v->name, pci->bus, pci->dev, pci->func, v->nscan);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
int VgpuInit(void)
{
    static const UINT16 ids[] = { 0x1050 };
    PciDevice pci;
    for (int i = 0; g_ndev < VGPU_MAX_DEVICES && PciFindNth(0x1AF4, ids, 1, i, &pci); i++) probe(&pci);
    return g_ndev;
}

int VgpuCount(void) { return g_ndev; }

static Vgpu *dev_of(int dev) { return dev >= 0 && dev < g_ndev ? g_dev[dev] : NULL; }

const char *VgpuName(int dev) { Vgpu *v = dev_of(dev); return v ? v->name : NULL; }
int  VgpuScanouts(int dev)    { Vgpu *v = dev_of(dev); return v ? v->nscan : 0; }
bool VgpuIsAt(int dev, UINT64 fb) { Vgpu *v = dev_of(dev); return v && v->fb && v->fb == fb; }

UINT32 VgpuConnected(int dev, int *w, int *h)
{
    Vgpu *v = dev_of(dev);
    if (!v) return 0;
    UINT32 mask = 0;
    IrqState s = spin_lock_irqsave(&g_lock);
    const DisplayInfo *di = display_info(v);
    for (int i = 0; di && i < v->nscan; i++) {
        if (!di->mode[i].enabled) continue;
        mask |= 1u << i;
        if (w) w[i] = (int)di->mode[i].r.w;
        if (h) h[i] = (int)di->mode[i].r.h;
    }
    spin_unlock_irqrestore(&g_lock, s);
    return mask;
}

bool VgpuChanged(int dev)
{
    Vgpu *v = dev_of(dev);
    if (!v || !(dev32(v, CFG_EVENTS_READ) & EVENT_DISPLAY)) return false;
    *(volatile UINT32 *)(v->dev + CFG_EVENTS_CLEAR) = EVENT_DISPLAY;
    return true;
}

bool VgpuShow(int dev, int scanout, int w, int h, bool keep, UINT32 **pixels, int *stride)
{
    Vgpu *v = dev_of(dev);
    if (!v || scanout < 0 || scanout >= v->nscan) return false;
    Out *o = &v->out[scanout], old = *o, nu = { 0 };
    if (w > 0 && h > 0) {
        nu.w = w; nu.h = h;
        nu.pages = ((size_t)w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
        nu.px = kernel_alloc_pages(nu.pages);
        if (!nu.px) {
            kprintf("[VGPU] %s: no memory for a %dx%d picture on output %d\n", v->name, w, h, scanout + 1);
            return false;
        }
        memset(nu.px, 0, nu.pages * PAGE_SIZE);
        if (keep && old.px)
            for (int y = 0; y < h && y < old.h; y++)
                memcpy(nu.px + (size_t)y * w, old.px + (size_t)y * old.w, (size_t)(w < old.w ? w : old.w) * 4);
    }
    IrqState s = spin_lock_irqsave(&g_lock);
    bool ok;
    if (nu.px) {
        ok = show_locked(v, scanout, &nu);
        if (ok) flush_locked(v, scanout, 0, 0, w, h);
    } else {
        SetScanout *so = req(v, 0, CMD_SET_SCANOUT, sizeof(SetScanout));
        so->scanout = (UINT32)scanout;                       /* resource 0: off */
        ok = cmd(v, sizeof(SetScanout));
    }
    if (ok) { unref_locked(v, old.id); *o = nu; }
    else unref_locked(v, nu.id);
    spin_unlock_irqrestore(&g_lock, s);
    if (!ok) {
        if (nu.px) kernel_free_pages(nu.px, nu.pages);
        return false;
    }
    if (old.px) kernel_free_pages(old.px, old.pages);
    if (pixels) *pixels = nu.px;
    if (stride) *stride = nu.w;
    return true;
}

void VgpuFlush(int dev, int scanout, int x, int y, int w, int h)
{
    Vgpu *v = dev_of(dev);
    if (!v || scanout < 0 || scanout >= v->nscan || !v->out[scanout].px) return;
    IrqState s = spin_lock_irqsave(&g_lock);
    flush_locked(v, scanout, x, y, w, h);
    spin_unlock_irqrestore(&g_lock, s);
}

void VgpuResume(void)
{
    for (int d = 0; d < g_ndev; d++) {
        Vgpu *v = g_dev[d];
        IrqState s = spin_lock_irqsave(&g_lock);
        bool ok = hw_setup(v);
        for (int i = 0; ok && i < v->nscan; i++) {
            Out *o = &v->out[i];
            if (!o->px) continue;
            if (show_locked(v, i, o)) flush_locked(v, i, 0, 0, o->w, o->h);
        }
        spin_unlock_irqrestore(&g_lock, s);
        kprintf("[VGPU] %s %s after sleep\n", v->name, ok ? "set up again" : "didn't come back");
    }
}
