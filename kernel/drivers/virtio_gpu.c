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
 * framebuffer console can draw on it like on video memory.  The 2D
 * commands are synchronous: the control queue asks for no interrupts and
 * the driver waits for the card's answer (a copy in QEMU's main loop).  The
 * PCI setup is virtio_input.c's (capabilities, the handshake, split queues).
 *
 * 3D (QEMU's virtio-vga-gl / virtio-gpu-gl with venus=on,blob=on): the
 * card runs a host renderer (virglrenderer) and a program's Vulkan driver
 * (Mesa's Venus, vulkan_virtio.dll) talks to it through a context
 * (CTX_CREATE with the Venus capability set), command streams (SUBMIT_3D)
 * and "blob" resources: host memory the card places in its host-visible
 * shared-memory region (a BAR) on RESOURCE_MAP_BLOB, which the program
 * then maps (um_gpu.c turns it into a section).  A submission carrying a
 * fence comes back only when the host GPU has finished it, so the queue
 * holds several requests at once in slots; whoever waits reaps the
 * answers, and a kick (a queue notification) makes QEMU check its
 * renderer's fences at once rather than on its 10 ms timer.  Timelines
 * ("syncs": a 64-bit counter each) advance when a fenced submission that
 * names them completes; Venus waits on them.
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
#include "../ke/scheduler.h"
#include "../mm/pmm.h"

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
#define CMD_GET_CAPSET_INFO      0x0108
#define CMD_GET_CAPSET           0x0109
#define CMD_RESOURCE_CREATE_BLOB 0x010C
#define CMD_CTX_CREATE           0x0200
#define CMD_CTX_DESTROY          0x0201
#define CMD_CTX_ATTACH_RESOURCE  0x0202
#define CAPSET_VENUS             4
#define CMD_RESOURCE_CREATE_3D   0x0204
#define CMD_TRANSFER_TO_HOST_3D  0x0205
#define CMD_TRANSFER_FROM_HOST_3D 0x0206
#define CMD_SUBMIT_3D            0x0207
#define CMD_RESOURCE_MAP_BLOB    0x0208
#define CMD_RESOURCE_UNMAP_BLOB  0x0209
#define RESP_OK_NODATA           0x1100
#define RESP_OK_DISPLAY_INFO     0x1101
#define RESP_OK_CAPSET_INFO      0x1102
#define RESP_OK_CAPSET           0x1103
#define RESP_OK_MAP_INFO         0x1106
#define FORMAT_B8G8R8X8          2         /* bytes B, G, R, X: little-endian XRGB */
#define FLAG_FENCE               1
#define FLAG_INFO_RING_IDX       2

/* Features (5.7.3) */
#define F_VIRGL          (1u << 0)
#define F_RESOURCE_BLOB  (1u << 3)
#define F_CONTEXT_INIT   (1u << 4)
#define CFG_NUM_CAPSETS  12
#define CAP_SHARED_MEMORY 8
#define SHM_HOST_VISIBLE 1

/* The control queue: N_SLOT requests in flight, each with its own page
 * (the request at 0, the answer at SLOT_ANSWER) and three descriptors
 * (request, an optional payload, answer) */
#define N_DESC       128
#define N_SLOT       (N_DESC / 3)
#define SLOT_ANSWER  2048
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

typedef struct VgpuSync {
    volatile UINT64 value;
    int             refs;                 /* the context's table, and each fence naming it */
} VgpuSync;

/* What a slot's completion does: a fenced submission advances timelines */
typedef struct {
    int        n;
    VgpuSync **sync;
    UINT64    *value;
} Fence;

typedef struct {
    UINT8  state;                         /* SLOT_FREE, _BUSY (on the queue), _DONE */
    bool   autofree;                      /* nobody waits: free it when it comes back */
    UINT32 answer;                        /* the answer's type, when done */
    UINT8 *page;
    void  *ext;                           /* a payload (a command stream), freed with the slot */
    size_t ext_pages;
    Fence *fence;
} Slot;
enum { SLOT_FREE, SLOT_BUSY, SLOT_DONE };

typedef struct {
    PciDevice        pci;
    const char      *name;
    UINT64           fb;                  /* BAR0 (virtio-vga's VGA framebuffer) */
    volatile UINT8  *common, *notify_base, *dev;
    UINT32           notify_mul;
    Desc            *desc;
    volatile Avail  *avail;
    volatile Used   *used;
    UINT8           *buf;                 /* the 2D path's two requests and their answers */
    UINT16           size, last_used;
    volatile UINT16 *notify;
    int              nscan;
    UINT32           next_id;
    Out              out[VGPU_MAX_SCANOUTS];
    Slot             slot[N_SLOT];
    int              nslot;
    /* 3D */
    bool             has3d;               /* VIRGL + RESOURCE_BLOB + CONTEXT_INIT, and host-visible memory */
    int              ncapsets;
    UINT64           shm_pa, shm_size;    /* the host-visible region */
    UINT8           *shm_used;            /* one byte per SHM_CHUNK of it */
    UINT64           fence_seq;
    UINT32           next_ctx;
} Vgpu;

#define SHM_CHUNK  (64u * 1024)

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
        if (id == 0x09 && type == CAP_SHARED_MEMORY) {
            /* virtio_pci_cap64: bar, id (the region), offset and length in two halves */
            UINT32 b = PciRead32(d->bus, d->dev, d->func, off + 4);
            UINT8 bar = (UINT8)b, shmid = (UINT8)(b >> 8);
            UINT64 o = PciRead32(d->bus, d->dev, d->func, off + 8) |
                       (UINT64)PciRead32(d->bus, d->dev, d->func, off + 16) << 32;
            UINT64 len = PciRead32(d->bus, d->dev, d->func, off + 12) |
                         (UINT64)PciRead32(d->bus, d->dev, d->func, off + 20) << 32;
            if (shmid == SHM_HOST_VISIBLE && bar < 6 && PciBarAddress(d, bar) && len) {
                v->shm_pa = PciBarAddress(d, bar) + o;
                v->shm_size = len;
            }
        } else if (id == 0x09) {
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
    v->nslot = v->size / 3;
    for (int i = 0; i < v->nslot; i++) {                     /* (after S3: whatever was in flight is gone) */
        Slot *sl = &v->slot[i];
        if (sl->ext) kernel_free_pages(sl->ext, sl->ext_pages);
        sl->ext = NULL;
        sl->state = SLOT_FREE;
        sl->fence = NULL;                                    /* (its timelines never advance: leaked) */
    }
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
    /* 3D when the card offers it all and has host-visible memory for blobs;
     * otherwise plain 2D (no VIRGL, no EDID) */
    c32(v, C_DFSELECT, 0);
    UINT32 want3d = F_VIRGL | F_RESOURCE_BLOB | F_CONTEXT_INIT;
    v->has3d = (r32(v, C_DF) & want3d) == want3d && v->shm_size >= SHM_CHUNK;
    c32(v, C_GFSELECT, 0); c32(v, C_GF, v->has3d ? want3d : 0);
    c32(v, C_GFSELECT, 1); c32(v, C_GF, F_VERSION_1_HI);
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    if (!(r8(v, C_STATUS) & S_FEATURES_OK)) { c8(v, C_STATUS, S_FAILED); return false; }
    if (!queue_setup(v)) { c8(v, C_STATUS, S_FAILED); return false; }
    c8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    return true;
}

/* Collect the answers the card has given; under g_lock */
static void reap(Vgpu *v)
{
    while (v->last_used != v->used->idx) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        UINT32 id = v->used->ring[v->last_used % v->size].id;
        v->last_used++;
        int i = (int)(id / 3);
        if (i >= v->nslot || v->slot[i].state != SLOT_BUSY) continue;
        Slot *sl = &v->slot[i];
        sl->answer = ((Hdr *)(sl->page + SLOT_ANSWER))->type;
        if (sl->fence) {
            Fence *f = sl->fence;
            for (int k = 0; k < f->n; k++) {
                if (f->value[k] > f->sync[k]->value) f->sync[k]->value = f->value[k];
                if (--f->sync[k]->refs == 0) kfree(f->sync[k]);
            }
            kfree(f);
            sl->fence = NULL;
        }
        if (sl->ext) { kernel_free_pages(sl->ext, sl->ext_pages); sl->ext = NULL; }
        sl->state = sl->autofree ? SLOT_FREE : SLOT_DONE;
    }
}

/* A free slot, or -1 (all busy even after reaping); under g_lock */
static int slot_get(Vgpu *v)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < v->nslot; i++)
            if (v->slot[i].state == SLOT_FREE) {
                if (!v->slot[i].page) v->slot[i].page = kernel_alloc_pages(1);
                if (!v->slot[i].page) return -1;
                v->slot[i].state = SLOT_BUSY;               /* (claimed; queued by slot_send) */
                v->slot[i].autofree = false;
                v->slot[i].fence = NULL;
                v->slot[i].ext = NULL;
                return i;
            }
        reap(v);
    }
    return -1;
}

/* Put slot @i's request (@qlen bytes, then @ext_len bytes of its payload)
 * on the queue, its answer @rlen bytes; under g_lock */
static void slot_send(Vgpu *v, int i, UINT32 qlen, UINT32 ext_len, UINT32 rlen)
{
    Slot *sl = &v->slot[i];
    int d = 3 * i, n = 0;
    v->desc[d].addr = phys(sl->page);
    v->desc[d].len = qlen;
    v->desc[d].flags = DESC_F_NEXT;
    v->desc[d].next = (UINT16)(d + 1);
    if (ext_len) {
        n = 1;
        v->desc[d + 1].addr = phys(sl->ext);
        v->desc[d + 1].len = ext_len;
        v->desc[d + 1].flags = DESC_F_NEXT;
        v->desc[d + 1].next = (UINT16)(d + 2);
    }
    Desc *a = &v->desc[d + 1 + n];
    if (!n) v->desc[d].next = (UINT16)(d + 1);
    a->addr = phys(sl->page + SLOT_ANSWER);
    a->len = rlen;
    a->flags = DESC_F_WRITE;
    a->next = 0;
    memset(sl->page + SLOT_ANSWER, 0, sizeof(Hdr));
    sl->state = SLOT_BUSY;
    UINT16 idx = v->avail->idx;
    v->avail->ring[idx % v->size] = (UINT16)d;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->avail->idx = (UINT16)(idx + 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void kick(Vgpu *v) { *v->notify = 0; }

/* Spin until slot @i is answered; under g_lock (the 2D path: QEMU answers
 * those while it handles the notification).  The answer's type, 0 if none. */
static UINT32 slot_spin(Vgpu *v, int i)
{
    for (UINT64 spin = 0; v->slot[i].state == SLOT_BUSY; spin++) {
        if (spin > 400000000ull) {
            kprintf("[VGPU] %s: no answer to command %x\n", v->name, ((Hdr *)v->slot[i].page)->type);
            v->slot[i].autofree = true;                      /* (if it ever comes) */
            return 0;
        }
        pause_cpu();
        reap(v);
    }
    v->slot[i].state = SLOT_FREE;
    return v->slot[i].answer;
}


/* Send @n requests (request i at buf + i * 1024, its answer at
 * buf + 512 + i * 1024, @rlen[i] bytes) and wait for the answers; under
 * g_lock.  True when every answer is one of the OK ones. */
static bool submit(Vgpu *v, int n, const UINT32 *qlen, const UINT32 *rlen)
{
    int id[2];
    for (int i = 0; i < n; i++) {
        id[i] = slot_get(v);
        for (UINT64 spin = 0; id[i] < 0 && spin < 400000000ull; spin++) { pause_cpu(); id[i] = slot_get(v); }
        if (id[i] < 0) {
            for (int k = 0; k < i; k++) v->slot[id[k]].state = SLOT_FREE;
            kprintf("[VGPU] %s: the queue stays full\n", v->name);
            return false;
        }
        memcpy(v->slot[id[i]].page, v->buf + i * 1024, qlen[i]);
        slot_send(v, id[i], qlen[i], 0, rlen[i]);
    }
    kick(v);
    bool ok = true;
    for (int i = 0; i < n; i++) {
        UINT32 t = slot_spin(v, id[i]);
        memcpy(v->buf + 512 + i * 1024, v->slot[id[i]].page + SLOT_ANSWER, rlen[i] < 512 ? rlen[i] : 512);
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
    if (v->has3d) {
        v->ncapsets = (int)dev32(v, CFG_NUM_CAPSETS);
        v->shm_used = kzalloc((size_t)(v->shm_size / SHM_CHUNK));
        if (!v->shm_used) v->has3d = false;
    }
    g_dev[g_ndev++] = v;
    PciClaim(pci, v->has3d ? "virtio-gpu (3D)" : "virtio-gpu");
    kprintf("[VGPU] %s at %02x:%02x.%x: %d output(s)\n", v->name, pci->bus, pci->dev, pci->func, v->nscan);
    if (v->has3d)
        kprintf("[VGPU] %s: 3D, %d capability set(s), %llu MiB of host-visible memory at %llx\n", v->name,
                v->ncapsets, (unsigned long long)(v->shm_size >> 20), (unsigned long long)v->shm_pa);
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

/* -----------------------------------------------------------------------
 * 3D: contexts, blobs and timelines (for um_gpu.c)
 * ----------------------------------------------------------------------- */
#define MAX_SYNCS 4096

struct VgpuCtx {
    Vgpu      *v;
    UINT32     id;
    VgpuSync **sync;                      /* timeline N at sync[N - 1] */
    int        nsync;
    UINT32     capset;
};

struct VgpuBlob {
    Vgpu   *v;
    int     refs;                         /* its context's, and the section's while mapped */
    UINT32  res;
    UINT64  size;
    INT64   shm_off;                      /* where it is mapped in the host-visible region (-1: not) */
    PADDR  *frames;                       /* a virgl resource's guest memory (NULL: a blob) */
    UINT64  nframes;
};

static Vgpu *dev3d(void)
{
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i]->has3d) return g_dev[i];
    return NULL;
}

bool Vgpu3dPresent(void) { return dev3d() != NULL; }

UINT64 Vgpu3dHostVisibleSize(void) { Vgpu *v = dev3d(); return v ? v->shm_size : 0; }

/* Wait (sleeping, g_lock not held) until slot @i is answered and free it;
 * copy @alen bytes of the answer to @ans.  The answer's type, 0 if none. */
static UINT32 slot_wait(Vgpu *v, int i, void *ans, UINT32 alen)
{
    UINT64 deadline = sched_tsc_after(100000000ull);   /* 10 s */
    for (int n = 0; ; n++) {
        IrqState s = spin_lock_irqsave(&g_lock);
        reap(v);
        if (v->slot[i].state == SLOT_DONE) {
            UINT32 t = v->slot[i].answer;
            if (ans) memcpy(ans, v->slot[i].page + SLOT_ANSWER, alen);
            v->slot[i].state = SLOT_FREE;
            spin_unlock_irqrestore(&g_lock, s);
            return t;
        }
        if (rdtsc() > deadline) {
            v->slot[i].autofree = true;
            spin_unlock_irqrestore(&g_lock, s);
            kprintf("[VGPU] %s: no answer to command %x\n", v->name, ((Hdr *)v->slot[i].page)->type);
            return 0;
        }
        spin_unlock_irqrestore(&g_lock, s);
        kick(v);
        if (n < 20) sched_yield();
        else { static UINT32 never; sched_sleep_until_tsc(&never, sched_tsc_after(2000)); }
    }
}

/* A slot for a 3D request, sleeping while the queue is full (g_lock not
 * held); its page zeroed for @len bytes, the header's type and context set */
static int slot3d(Vgpu *v, UINT32 type, UINT32 ctx, UINT32 len)
{
    for (;;) {
        IrqState s = spin_lock_irqsave(&g_lock);
        int i = slot_get(v);
        spin_unlock_irqrestore(&g_lock, s);
        if (i >= 0) {
            memset(v->slot[i].page, 0, len);
            ((Hdr *)v->slot[i].page)->type = type;
            ((Hdr *)v->slot[i].page)->ctx = ctx;
            return i;
        }
        kick(v);
        static UINT32 never;
        sched_sleep_until_tsc(&never, sched_tsc_after(2000));
    }
}

/* Send slot @i and wait for its answer */
static UINT32 call3d(Vgpu *v, int i, UINT32 qlen, UINT32 ext_len, void *ans, UINT32 alen)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    slot_send(v, i, qlen, ext_len, alen < sizeof(Hdr) ? sizeof(Hdr) : alen);
    spin_unlock_irqrestore(&g_lock, s);
    kick(v);
    return slot_wait(v, i, ans, alen);
}

/* Send slot @i and don't wait (its answer frees it) */
static void post3d(Vgpu *v, int i, UINT32 qlen, UINT32 ext_len)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    v->slot[i].autofree = true;
    slot_send(v, i, qlen, ext_len, sizeof(Hdr));
    spin_unlock_irqrestore(&g_lock, s);
    kick(v);
}

typedef struct __attribute__((packed)) { Hdr h; UINT32 index, pad; } CapsetInfoReq;
typedef struct __attribute__((packed)) { Hdr h; UINT32 id, max_version, max_size, pad; } CapsetInfo;
typedef struct __attribute__((packed)) { Hdr h; UINT32 id, version; } CapsetReq;

int Vgpu3dCapset(UINT32 id, UINT32 version, void *out, UINT32 size)
{
    Vgpu *v = dev3d();
    if (!v) return -1;
    for (int k = 0; k < v->ncapsets; k++) {
        int i = slot3d(v, CMD_GET_CAPSET_INFO, 0, sizeof(CapsetInfoReq));
        ((CapsetInfoReq *)v->slot[i].page)->index = (UINT32)k;
        CapsetInfo ci;
        if (call3d(v, i, sizeof(CapsetInfoReq), 0, &ci, sizeof(ci)) != RESP_OK_CAPSET_INFO) return -1;
        if (ci.id != id) continue;
        if (version > ci.max_version || ci.max_size > SLOT_ANSWER - sizeof(Hdr)) return -1;
        i = slot3d(v, CMD_GET_CAPSET, 0, sizeof(CapsetReq));
        ((CapsetReq *)v->slot[i].page)->id = id;
        ((CapsetReq *)v->slot[i].page)->version = version;
        UINT8 *ans = kmalloc(SLOT_ANSWER);
        if (!ans) return -1;
        int n = -1;
        if (call3d(v, i, sizeof(CapsetReq), 0, ans, sizeof(Hdr) + ci.max_size) == RESP_OK_CAPSET) {
            n = (int)(ci.max_size < size ? ci.max_size : size);
            memcpy(out, ans + sizeof(Hdr), (size_t)n);
        }
        kfree(ans);
        return n;
    }
    return -1;
}

typedef struct __attribute__((packed)) { Hdr h; UINT32 nlen, context_init; char name[64]; } CtxCreate;
typedef struct __attribute__((packed)) { Hdr h; UINT32 size, pad; } Submit3d;

VgpuCtx *VgpuCtxCreate(UINT32 capset, const char *name)
{
    Vgpu *v = dev3d();
    if (!v) return NULL;
    VgpuCtx *c = kzalloc(sizeof(*c));
    if (!c) return NULL;
    c->v = v;
    IrqState s = spin_lock_irqsave(&g_lock);
    c->id = ++v->next_ctx;
    c->capset = capset & 0xFF;
    spin_unlock_irqrestore(&g_lock, s);
    int i = slot3d(v, CMD_CTX_CREATE, c->id, sizeof(CtxCreate));
    CtxCreate *cc = (CtxCreate *)v->slot[i].page;
    cc->context_init = capset & 0xFF;
    for (int k = 0; name && name[k] && k < 63; k++) cc->name[k] = name[k], cc->nlen = (UINT32)k + 1;
    if (call3d(v, i, sizeof(CtxCreate), 0, NULL, sizeof(Hdr)) != RESP_OK_NODATA) {
        kprintf("[VGPU] %s: the card refused a 3D context (capability set %u)\n", v->name, capset);
        kfree(c);
        return NULL;
    }
    return c;
}

static void sync_unref_locked(VgpuSync *y) { if (--y->refs == 0) kfree(y); }

void VgpuCtxDestroy(VgpuCtx *c)
{
    if (!c) return;
    Vgpu *v = c->v;
    int i = slot3d(v, CMD_CTX_DESTROY, c->id, sizeof(Hdr));
    post3d(v, i, sizeof(Hdr), 0);
    IrqState s = spin_lock_irqsave(&g_lock);
    for (int k = 0; k < c->nsync; k++)
        if (c->sync[k]) sync_unref_locked(c->sync[k]);
    spin_unlock_irqrestore(&g_lock, s);
    kfree(c->sync);
    kfree(c);
}

/* Payload pages holding @size bytes of @cs (a kernel copy) */
static void *payload(const void *cs, UINT32 size, size_t *pages)
{
    *pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *p = *pages ? kernel_alloc_pages(*pages) : NULL;
    if (p) memcpy(p, cs, size);
    return p;
}

static void fence_drop(Fence *f)
{
    if (!f) return;
    IrqState s = spin_lock_irqsave(&g_lock);
    for (int j = 0; j < f->n; j++) sync_unref_locked(f->sync[j]);
    spin_unlock_irqrestore(&g_lock, s);
    kfree(f);
}

/* What completing a fenced command does to @c's timelines: @syncs[k]
 * reaches @vals[k] (NULL when n == 0 or a timeline is bad; *ok says which) */
static Fence *fence_make(VgpuCtx *c, int n, const UINT32 *syncs, const UINT64 *vals, bool *ok)
{
    *ok = true;
    if (n <= 0) return NULL;
    Fence *f = kzalloc(sizeof(Fence) + (size_t)n * (sizeof(VgpuSync *) + sizeof(UINT64)));
    if (!f) { *ok = false; return NULL; }
    f->sync = (VgpuSync **)(f + 1);
    f->value = (UINT64 *)(f->sync + n);
    IrqState s = spin_lock_irqsave(&g_lock);
    for (int k = 0; k < n; k++) {
        UINT32 id = syncs[k];
        if (!id || (int)id > c->nsync || !c->sync[id - 1]) {
            for (int j = 0; j < f->n; j++) sync_unref_locked(f->sync[j]);
            spin_unlock_irqrestore(&g_lock, s);
            kfree(f);
            *ok = false;
            return NULL;
        }
        f->sync[k] = c->sync[id - 1];
        f->sync[k]->refs++;
        f->value[k] = vals[k];
        f->n++;
    }
    spin_unlock_irqrestore(&g_lock, s);
    return f;
}

bool VgpuCtxSubmit(VgpuCtx *c, const void *cs, UINT32 size, UINT32 ring, int n, const UINT32 *syncs, const UINT64 *vals)
{
    Vgpu *v = c->v;
    bool ok;
    Fence *f = fence_make(c, n, syncs, vals, &ok);
    if (!ok) return false;
    size_t pages = 0;
    void *p = size ? payload(cs, size, &pages) : NULL;
    if (size && !p) { fence_drop(f); return false; }
    int i = slot3d(v, CMD_SUBMIT_3D, c->id, sizeof(Submit3d));
    Submit3d *q = (Submit3d *)v->slot[i].page;
    q->size = size;
    if (f) {
        q->h.flags = FLAG_FENCE | FLAG_INFO_RING_IDX;
        q->h.ring = (UINT8)ring;
    }
    IrqState s = spin_lock_irqsave(&g_lock);
    if (f) q->h.fence = ++v->fence_seq;
    v->slot[i].ext = p;
    v->slot[i].ext_pages = pages;
    v->slot[i].fence = f;
    v->slot[i].autofree = true;
    slot_send(v, i, sizeof(Submit3d), size, sizeof(Hdr));
    spin_unlock_irqrestore(&g_lock, s);
    kick(v);
    return true;
}

typedef struct __attribute__((packed)) {
    Hdr h; UINT32 res, blob_mem, blob_flags, nr_entries; UINT64 blob_id, size;
} CreateBlob;
typedef struct __attribute__((packed)) { Hdr h; UINT32 res, pad; UINT64 offset; } MapBlob;
typedef struct __attribute__((packed)) { Hdr h; UINT32 res, pad; } CtxResource;
typedef struct __attribute__((packed)) { Hdr h; UINT32 map_info, pad; } MapInfo;

VgpuBlob *VgpuBlobCreate(VgpuCtx *c, UINT32 blob_mem, UINT32 flags, UINT64 blob_id, UINT64 size,
                         const void *cs, UINT32 cs_size)
{
    Vgpu *v = c->v;
    if (!size || ((flags & 1) && size > v->shm_size)) return NULL;     /* (1: mappable) */
    if (cs_size && !VgpuCtxSubmit(c, cs, cs_size, 0, 0, NULL, NULL)) return NULL;
    VgpuBlob *b = kzalloc(sizeof(*b));
    if (!b) return NULL;
    b->v = v;
    b->refs = 1;
    b->size = (size + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    b->shm_off = -1;
    IrqState s = spin_lock_irqsave(&g_lock);
    b->res = ++v->next_id;
    spin_unlock_irqrestore(&g_lock, s);
    int i = slot3d(v, CMD_RESOURCE_CREATE_BLOB, c->id, sizeof(CreateBlob));
    CreateBlob *q = (CreateBlob *)v->slot[i].page;
    q->res = b->res;
    q->blob_mem = blob_mem;
    q->blob_flags = flags;
    q->blob_id = blob_id;
    q->size = b->size;
    UINT32 t = call3d(v, i, sizeof(CreateBlob), 0, NULL, sizeof(Hdr));
    if (t != RESP_OK_NODATA) {
        kprintf("[VGPU] %s: a %llu KiB blob (memory %u, flags %x, id %llu) was refused (%x)\n", v->name,
                (unsigned long long)(size >> 10), blob_mem, flags, (unsigned long long)blob_id, t);
        kfree(b);
        return NULL;
    }
    /* virgl's command streams name the blob by its resource number, so
     * its context must know it.  Not Venus's: there the host would import
     * a second copy of a blob the context made (its ring and reply
     * buffers among them), and programs then hang now and again */
    if (c->capset != CAPSET_VENUS) {
        i = slot3d(v, CMD_CTX_ATTACH_RESOURCE, c->id, sizeof(CtxResource));
        ((CtxResource *)v->slot[i].page)->res = b->res;
        post3d(v, i, sizeof(CtxResource), 0);
    }
    return b;
}

UINT32 VgpuBlobId(VgpuBlob *b) { return b->res; }

typedef struct __attribute__((packed)) {
    Hdr h; UINT32 res, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, pad;
} Create3d;
typedef struct __attribute__((packed)) {
    Hdr h; UINT32 x, y, z, w, h2, d; UINT64 offset; UINT32 res, level, stride, layer_stride;
} Transfer3d;

VgpuBlob *VgpuRes3dCreate(VgpuCtx *c, const UINT32 *p, PADDR *frames, UINT64 nframes)
{
    Vgpu *v = c->v;
    VgpuBlob *b = kzalloc(sizeof(*b));
    UINT64 *ent = NULL;
    if (!b) return NULL;
    b->v = v;
    b->refs = 1;
    b->shm_off = -1;
    b->size = nframes * PAGE_SIZE;
    IrqState s = spin_lock_irqsave(&g_lock);
    b->res = ++v->next_id;
    spin_unlock_irqrestore(&g_lock, s);
    int i = slot3d(v, CMD_RESOURCE_CREATE_3D, c->id, sizeof(Create3d));
    Create3d *q = (Create3d *)v->slot[i].page;
    q->res = b->res;
    memcpy(&q->target, p, 10 * sizeof(UINT32));
    UINT32 t = call3d(v, i, sizeof(Create3d), 0, NULL, sizeof(Hdr));
    if (t != RESP_OK_NODATA) {
        kprintf("[VGPU] %s: a %ux%u resource (format %u, bind %x) was refused (%x)\n", v->name,
                p[3], p[4], p[1], p[2], t);
        kfree(b);
        return NULL;
    }
    if (nframes) {
        /* its memory: one entry per run of consecutive pages */
        UINT32 n = 0;
        ent = kmalloc(nframes * 16);
        if (!ent) goto fail;
        for (UINT64 k = 0; k < nframes; k++) {
            if (n && ent[2 * (n - 1)] + (ent[2 * (n - 1) + 1] & 0xFFFFFFFF) == frames[k] &&
                (ent[2 * (n - 1) + 1] & 0xFFFFFFFF) < (1u << 30)) {
                ent[2 * (n - 1) + 1] += PAGE_SIZE;
                continue;
            }
            ent[2 * n] = frames[k];
            ent[2 * n + 1] = PAGE_SIZE;
            n++;
        }
        size_t pages;
        void *pl = payload(ent, n * 16, &pages);
        kfree(ent);
        ent = NULL;
        if (!pl) goto fail;
        i = slot3d(v, CMD_ATTACH_BACKING, 0, sizeof(Attach));
        Attach *a = (Attach *)v->slot[i].page;
        a->id = b->res;
        a->n = n;
        v->slot[i].ext = pl;
        v->slot[i].ext_pages = pages;
        t = call3d(v, i, sizeof(Hdr) + 8, n * 16, NULL, sizeof(Hdr));
        if (t != RESP_OK_NODATA) {
            kprintf("[VGPU] %s: resource %u refused its memory (%x)\n", v->name, b->res, t);
            goto fail;
        }
        b->frames = frames;
        b->nframes = nframes;
    }
    i = slot3d(v, CMD_CTX_ATTACH_RESOURCE, c->id, sizeof(CtxResource));
    ((CtxResource *)v->slot[i].page)->res = b->res;
    post3d(v, i, sizeof(CtxResource), 0);
    return b;
fail:
    kfree(ent);
    i = slot3d(v, CMD_RESOURCE_UNREF, 0, sizeof(Unref));
    ((Unref *)v->slot[i].page)->id = b->res;
    call3d(v, i, sizeof(Unref), 0, NULL, sizeof(Hdr));
    kfree(b);
    return NULL;
}

bool VgpuBlobFrames(VgpuBlob *b, const PADDR **frames, UINT64 *n)
{
    if (!b->frames) return false;
    *frames = b->frames;
    *n = b->nframes;
    return true;
}

bool VgpuTransfer3d(VgpuCtx *c, VgpuBlob *b, bool to_host, const UINT32 *box, UINT64 offset, UINT32 level,
                    UINT32 stride, UINT32 layer_stride, UINT32 sync, UINT64 value)
{
    Vgpu *v = c->v;
    bool ok;
    Fence *f = fence_make(c, sync ? 1 : 0, &sync, &value, &ok);
    if (!ok) return false;
    int i = slot3d(v, to_host ? CMD_TRANSFER_TO_HOST_3D : CMD_TRANSFER_FROM_HOST_3D, c->id, sizeof(Transfer3d));
    Transfer3d *q = (Transfer3d *)v->slot[i].page;
    memcpy(&q->x, box, 6 * sizeof(UINT32));
    q->offset = offset;
    q->res = b->res;
    q->level = level;
    q->stride = stride;
    q->layer_stride = layer_stride;
    if (f) q->h.flags = FLAG_FENCE | FLAG_INFO_RING_IDX;
    IrqState s = spin_lock_irqsave(&g_lock);
    if (f) q->h.fence = ++v->fence_seq;
    v->slot[i].fence = f;
    v->slot[i].autofree = true;
    slot_send(v, i, sizeof(Transfer3d), 0, sizeof(Hdr));
    spin_unlock_irqrestore(&g_lock, s);
    kick(v);
    return true;
}

/* Map @b into the host-visible region (once); its physical address */
bool VgpuBlobMap(VgpuBlob *b, UINT64 *pa, UINT64 *size)
{
    Vgpu *v = b->v;
    if (b->shm_off < 0) {
        UINT64 chunks = (b->size + SHM_CHUNK - 1) / SHM_CHUNK, total = v->shm_size / SHM_CHUNK, run = 0, at = 0;
        IrqState s = spin_lock_irqsave(&g_lock);
        for (UINT64 k = 0; k < total && run < chunks; k++) {
            if (v->shm_used[k]) { run = 0; continue; }
            if (!run) at = k;
            run++;
        }
        if (run < chunks) {
            spin_unlock_irqrestore(&g_lock, s);
            kprintf("[VGPU] %s: no room in host-visible memory for %llu KiB\n", v->name,
                    (unsigned long long)(b->size >> 10));
            return false;
        }
        memset(v->shm_used + at, 1, (size_t)chunks);
        spin_unlock_irqrestore(&g_lock, s);
        int i = slot3d(v, CMD_RESOURCE_MAP_BLOB, 0, sizeof(MapBlob));
        MapBlob *q = (MapBlob *)v->slot[i].page;
        q->res = b->res;
        q->offset = at * SHM_CHUNK;
        MapInfo mi;
        if (call3d(v, i, sizeof(MapBlob), 0, &mi, sizeof(mi)) != RESP_OK_MAP_INFO) {
            s = spin_lock_irqsave(&g_lock);
            memset(v->shm_used + at, 0, (size_t)chunks);
            spin_unlock_irqrestore(&g_lock, s);
            kprintf("[VGPU] %s: blob %u could not be mapped\n", v->name, b->res);
            return false;
        }
        b->shm_off = (INT64)(at * SHM_CHUNK);
    }
    *pa = v->shm_pa + (UINT64)b->shm_off;
    *size = b->size;
    return true;
}

void VgpuBlobRef(VgpuBlob *b)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    b->refs++;
    spin_unlock_irqrestore(&g_lock, s);
}

/* Drop a reference; the last one unmaps the blob and frees it on the card */
void VgpuBlobUnref(VgpuBlob *b)
{
    if (!b) return;
    Vgpu *v = b->v;
    IrqState s = spin_lock_irqsave(&g_lock);
    bool last = --b->refs == 0;
    spin_unlock_irqrestore(&g_lock, s);
    if (!last) return;
    if (b->shm_off >= 0) {
        int i = slot3d(v, CMD_RESOURCE_UNMAP_BLOB, 0, sizeof(Unref));
        ((Unref *)v->slot[i].page)->id = b->res;
        post3d(v, i, sizeof(Unref), 0);                      /* (the card keeps the queue's order) */
        UINT64 chunks = (b->size + SHM_CHUNK - 1) / SHM_CHUNK;
        s = spin_lock_irqsave(&g_lock);
        memset(v->shm_used + b->shm_off / SHM_CHUNK, 0, (size_t)chunks);
        spin_unlock_irqrestore(&g_lock, s);
    }
    int i = slot3d(v, CMD_RESOURCE_UNREF, 0, sizeof(Unref));
    ((Unref *)v->slot[i].page)->id = b->res;
    if (b->frames) {
        /* (the card is done with the pages once it answers: it runs the
         * queue's commands in order, transfers included) */
        call3d(v, i, sizeof(Unref), 0, NULL, sizeof(Hdr));
        for (UINT64 k = 0; k < b->nframes; k++) pmm_free_page(b->frames[k]);
        kfree(b->frames);
    } else
        post3d(v, i, sizeof(Unref), 0);
    kfree(b);
}

UINT32 VgpuSyncCreate(VgpuCtx *c, UINT64 value)
{
    VgpuSync *y = kzalloc(sizeof(*y));
    if (!y) return 0;
    y->value = value;
    y->refs = 1;
    IrqState s = spin_lock_irqsave(&g_lock);
    int k = 0;
    while (k < c->nsync && c->sync[k]) k++;
    if (k == c->nsync) {
        int n = c->nsync ? c->nsync * 2 : 64;
        VgpuSync **t = n <= MAX_SYNCS ? kzalloc(sizeof(*t) * (size_t)n) : NULL;
        if (!t) { spin_unlock_irqrestore(&g_lock, s); kfree(y); return 0; }
        if (c->nsync) memcpy(t, c->sync, sizeof(*t) * (size_t)c->nsync);
        kfree(c->sync);
        c->sync = t;
        c->nsync = n;
    }
    c->sync[k] = y;
    spin_unlock_irqrestore(&g_lock, s);
    return (UINT32)k + 1;
}

static VgpuSync *sync_of(VgpuCtx *c, UINT32 id)
{
    return id && (int)id <= c->nsync ? c->sync[id - 1] : NULL;
}

void VgpuSyncDestroy(VgpuCtx *c, UINT32 id)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    VgpuSync *y = sync_of(c, id);
    if (y) { c->sync[id - 1] = NULL; sync_unref_locked(y); }
    spin_unlock_irqrestore(&g_lock, s);
}

/* @op 0 read (into *value), 1 write (*value: larger than now), 2 reset to 0 */
bool VgpuSyncAccess(VgpuCtx *c, UINT32 id, int op, UINT64 *value)
{
    IrqState s = spin_lock_irqsave(&g_lock);
    reap(c->v);
    VgpuSync *y = sync_of(c, id);
    if (y) {
        if (op == 0) *value = y->value;
        else if (op == 1) { if (*value > y->value) y->value = *value; }
        else y->value = 0;
    }
    spin_unlock_irqrestore(&g_lock, s);
    return y != NULL;
}

/* The control queue's state on the serial log (Ctrl+Alt+F12, with the programs' threads): the card has
 * taken (avail) and answered (used) how many requests, and which fenced ones are still out, with the
 * timeline value each one will set.  A request out and a card that has gone quiet means the host side
 * never answered; no request out means the guest never sent the one a program waits for.  No lock: it
 * runs when something is already stuck. */
void VgpuDump(void)
{
    for (int d = 0; d < g_ndev; d++) {
        Vgpu *v = g_dev[d];
        if (!v || !v->has3d) continue;
        int busy = 0;
        for (int i = 0; i < v->nslot; i++) busy += v->slot[i].state == SLOT_BUSY;
        kprintf("[VGPU] %s: avail %u, used %u, reaped %u, %d of %d requests out, fence seq %llu\n", v->name,
                (unsigned)v->avail->idx, (unsigned)v->used->idx, (unsigned)v->last_used, busy, v->nslot,
                (unsigned long long)v->fence_seq);
        for (int i = 0; i < v->nslot; i++) {
            Slot *sl = &v->slot[i];
            if (sl->state != SLOT_BUSY || !sl->page) continue;
            const Hdr *h = (const Hdr *)sl->page;
            kprintf("[VGPU]   request %d: type 0x%x ctx %u fence %llu%s\n", i, (unsigned)h->type, (unsigned)h->ctx,
                    (unsigned long long)h->fence, sl->autofree ? " (nobody waits)" : "");
            const Fence *f = sl->fence;
            for (int k = 0; f && k < f->n && k < 4; k++)
                kprintf("[VGPU]     sets a timeline to %llu (now %llu)\n", (unsigned long long)f->value[k],
                        (unsigned long long)f->sync[k]->value);
        }
    }
}

/* Wait until the timelines reach their values (all of them, or @any one);
 * 0 done, 1 timed out (@timeout_ns), -1 a bad timeline */
int VgpuWait(VgpuCtx *c, int n, const UINT32 *ids, const UINT64 *vals, bool any, UINT64 timeout_ns)
{
    Vgpu *v = c->v;
    UINT64 deadline = timeout_ns >= 1000000000000000ull ? UINT64_MAX : sched_tsc_after(timeout_ns / 100);
    for (int iter = 0; ; iter++) {
        IrqState s = spin_lock_irqsave(&g_lock);
        reap(v);
        int reached = 0, bad = 0;
        for (int k = 0; k < n; k++) {
            VgpuSync *y = sync_of(c, ids[k]);
            if (!y) bad = 1;
            else if (y->value >= vals[k]) reached++;
        }
        spin_unlock_irqrestore(&g_lock, s);
        if (bad) return -1;
        if (any ? (reached > 0 || n == 0) : reached == n) return 0;
        if (rdtsc() >= deadline) return 1;
        kick(v);
        if (iter < 50) sched_yield();
        else {
            static UINT32 never;
            UINT64 next = sched_tsc_after(iter < 500 ? 1000 : 5000);   /* 0.1 ms, then 0.5 ms */
            sched_sleep_until_tsc(&never, next < deadline ? next : deadline);
        }
    }
}
