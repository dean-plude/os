/*
 * virtio_net.c — virtio network adapter (see virtio_net.h)
 *
 * Follows the virtio 1.2 specification: the device's PCI vendor
 * capabilities (type 9) locate its common configuration, notification,
 * ISR and device-specific registers in its BARs (section 4.1.4).  Setup
 * is the usual handshake (3.1.1): reset, ACKNOWLEDGE, DRIVER, agree on
 * features (VERSION_1, MAC, STATUS), FEATURES_OK, give each queue its
 * three areas, DRIVER_OK.
 *
 * Queues are split virtqueues (2.7) of N_DESC entries.  Receive: every
 * descriptor points at a 2 KiB device-writable buffer that starts with
 * the 12-byte virtio_net_hdr (5.1.6); used ones are copied out and put
 * straight back.  Transmit: one descriptor per frame, the header (all
 * zero: no offloads) and the frame in one buffer.  Both rings ask for no
 * interrupts; net.c polls.
 */

#include "virtio_net.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../arch/x86_64/cpu.h"

/* PCI capability types (4.1.4) */
#define CAP_COMMON   1
#define CAP_NOTIFY   2
#define CAP_ISR      3
#define CAP_DEVICE   4

/* Common configuration (4.1.4.3) */
#define C_DFSELECT   0x00
#define C_DF         0x04
#define C_GFSELECT   0x08
#define C_GF         0x0C
#define C_MSIX       0x10
#define C_NUMQ       0x12
#define C_STATUS     0x14
#define C_GEN        0x15
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

#define F_MAC        (1ull << 5)
#define F_STATUS     (1ull << 16)
#define F_VERSION_1  (1ull << 32)

#define NET_S_LINK_UP 1
#define HDR_LEN      12             /* virtio_net_hdr with num_buffers (VERSION_1) */

#define N_DESC       64
#define BUF_SZ       2048
#define DESC_F_WRITE 2
#define AVAIL_F_NO_INTERRUPT 1

typedef struct __attribute__((packed)) { UINT64 addr; UINT32 len; UINT16 flags, next; } Desc;
typedef struct __attribute__((packed)) { UINT16 flags, idx, ring[N_DESC], used_event; } Avail;
typedef struct __attribute__((packed)) { UINT32 id, len; } UsedElem;
typedef struct __attribute__((packed)) { UINT16 flags, idx; UsedElem ring[N_DESC]; UINT16 avail_event; } Used;

typedef struct {
    Desc            *desc;
    volatile Avail  *avail;
    volatile Used   *used;
    UINT8           *buf;                 /* N_DESC buffers of BUF_SZ */
    UINT16           size;                /* entries in use (≤ N_DESC) */
    UINT16           last_used;           /* next used entry to look at */
    UINT16           free_tx;             /* (transmit) descriptors not in flight */
    UINT16           next_tx;
    volatile UINT16 *notify;
} Queue;

static struct {
    bool             present;
    PciDevice        pci;
    volatile UINT8  *common, *notify_base, *isr, *dev;
    UINT32           notify_mul;
    UINT64           features;
    Queue            q[2];                /* 0 receive, 1 transmit */
    UINT8            mac[6];
} g;

static inline UINT64 phys(const void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }
static inline void   c8(UINT32 o, UINT8 v)   { *(volatile UINT8 *)(g.common + o) = v; }
static inline void   c16(UINT32 o, UINT16 v) { *(volatile UINT16 *)(g.common + o) = v; }
static inline void   c32(UINT32 o, UINT32 v) { *(volatile UINT32 *)(g.common + o) = v; }
static inline void   c64(UINT32 o, UINT64 v) { c32(o, (UINT32)v); c32(o + 4, (UINT32)(v >> 32)); }
static inline UINT8  r8(UINT32 o)  { return *(volatile UINT8 *)(g.common + o); }
static inline UINT16 r16(UINT32 o) { return *(volatile UINT16 *)(g.common + o); }
static inline UINT32 r32(UINT32 o) { return *(volatile UINT32 *)(g.common + o); }

/* Walk the capability list for the virtio structures */
static bool find_caps(void)
{
    const PciDevice *d = &g.pci;
    if (!(PciRead16(d->bus, d->dev, d->func, 0x06) & (1u << 4))) return false;
    UINT8 off = (UINT8)(PciRead32(d->bus, d->dev, d->func, 0x34) & 0xFC);
    for (int guard = 0; off && guard < 48; guard++) {
        UINT32 h = PciRead32(d->bus, d->dev, d->func, off);
        UINT8 id = (UINT8)h, next = (UINT8)(h >> 8), type = (UINT8)(h >> 24);
        if (id == 0x09) {                                   /* vendor-specific: virtio */
            UINT8 bar = (UINT8)PciRead32(d->bus, d->dev, d->func, off + 4);
            UINT32 boff = PciRead32(d->bus, d->dev, d->func, off + 8);
            volatile UINT8 *base = bar < 6 ? PciMapBar(d, bar) : NULL;
            if (base) {
                volatile UINT8 *p = base + boff;
                if (type == CAP_COMMON && !g.common) g.common = p;
                else if (type == CAP_NOTIFY && !g.notify_base) {
                    g.notify_base = p;
                    g.notify_mul = PciRead32(d->bus, d->dev, d->func, off + 16);
                } else if (type == CAP_ISR && !g.isr) g.isr = p;
                else if (type == CAP_DEVICE && !g.dev) g.dev = p;
            }
        }
        off = next & 0xFC;
    }
    return g.common && g.notify_base && g.dev;
}

static bool queue_setup(int i)
{
    Queue *q = &g.q[i];
    c16(C_QSELECT, (UINT16)i);
    UINT16 max = r16(C_QSIZE);
    if (!max) return false;
    q->size = max < N_DESC ? max : N_DESC;
    c16(C_QSIZE, q->size);
    c16(C_QMSIX, 0xFFFF);                                   /* no vector */
    memset(q->desc, 0, PAGE_SIZE);
    memset((void *)q->avail, 0, PAGE_SIZE);
    memset((void *)q->used, 0, PAGE_SIZE);
    q->avail->flags = AVAIL_F_NO_INTERRUPT;
    q->last_used = 0;
    for (UINT16 k = 0; k < q->size; k++) {
        q->desc[k].addr = phys(q->buf + (UINT64)k * BUF_SZ);
        q->desc[k].len = BUF_SZ;
    }
    if (i == 0) {                                           /* receive: all buffers to the device */
        for (UINT16 k = 0; k < q->size; k++) {
            q->desc[k].flags = DESC_F_WRITE;
            q->avail->ring[k] = k;
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        q->avail->idx = q->size;
    } else {
        q->free_tx = q->size;
        q->next_tx = 0;
    }
    c64(C_QDESC, phys(q->desc));
    c64(C_QDRIVER, phys((const void *)q->avail));
    c64(C_QDEVICE, phys((const void *)q->used));
    q->notify = (volatile UINT16 *)(g.notify_base + (UINT32)r16(C_QNOTIFY) * g.notify_mul);
    c16(C_QENABLE, 1);
    return true;
}

/* The handshake (3.1.1), at start-up and after S3 */
static bool hw_setup(void)
{
    c8(C_STATUS, 0);                                        /* reset */
    for (int i = 0; i < 100000 && r8(C_STATUS); i++) pause_cpu();
    c8(C_STATUS, S_ACK);
    c8(C_STATUS, S_ACK | S_DRIVER);
    c32(C_DFSELECT, 0);
    UINT64 have = r32(C_DF);
    c32(C_DFSELECT, 1);
    have |= (UINT64)r32(C_DF) << 32;
    g.features = have & (F_VERSION_1 | F_MAC | F_STATUS);
    if (!(g.features & F_VERSION_1)) { c8(C_STATUS, S_FAILED); return false; }
    c32(C_GFSELECT, 0); c32(C_GF, (UINT32)g.features);
    c32(C_GFSELECT, 1); c32(C_GF, (UINT32)(g.features >> 32));
    c8(C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    if (!(r8(C_STATUS) & S_FEATURES_OK)) { c8(C_STATUS, S_FAILED); return false; }
    if (!queue_setup(0) || !queue_setup(1)) { c8(C_STATUS, S_FAILED); return false; }
    if (g.features & F_MAC) {
        for (int i = 0; i < 6; i++) g.mac[i] = g.dev[i];
    } else if (!g.mac[0] && !g.mac[1] && !g.mac[2]) {
        static const UINT8 m[6] = { 0x52, 0x54, 0x00, 0x4E, 0x4F, 0x56 };   /* locally administered */
        memcpy(g.mac, m, 6);
    }
    c8(C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    *g.q[0].notify = 0;                                     /* receive buffers are there */
    return true;
}

bool VirtioNetInit(void)
{
    static const UINT16 ids[] = { 0x1041, 0x1000 };         /* modern, transitional */
    if (!PciFind(0x1AF4, ids, 2, &g.pci)) return false;
    PciEnableDevice(&g.pci);
    if (!find_caps()) {
        kprintf("[VIRTIO] Network adapter at %02x:%02x.%x has no virtio 1.0 registers (legacy only): not used\n",
                g.pci.bus, g.pci.dev, g.pci.func);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        Queue *q = &g.q[i];
        q->desc  = kernel_alloc_pages(1);
        q->avail = kernel_alloc_pages(1);
        q->used  = kernel_alloc_pages(1);
        q->buf   = kernel_alloc_pages(N_DESC * BUF_SZ / PAGE_SIZE);
        if (!q->desc || !q->avail || !q->used || !q->buf) {
            kprintf("[VIRTIO] Out of memory for the network queues\n");
            return false;
        }
    }
    if (!hw_setup()) {
        kprintf("[VIRTIO] The network adapter refused the setup\n");
        return false;
    }
    g.present = true;
    PciClaim(&g.pci, "virtio-net");
    kprintf("[VIRTIO] Network adapter at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x, "
            "queues %u/%u, link %s\n", g.pci.bus, g.pci.dev, g.pci.func,
            g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5],
            g.q[0].size, g.q[1].size, VirtioNetLinkUp() ? "up" : "down");
    return true;
}

void VirtioNetResume(void)
{
    if (!g.present) return;
    g.present = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (hw_setup()) g.present = true;
    else kprintf("[VIRTIO] The network adapter didn't come back after sleep\n");
}

bool        VirtioNetPresent(void) { return g.present; }
const char *VirtioNetName(void)    { return "Virtio network adapter"; }
void        VirtioNetMac(UINT8 mac[6]) { memcpy(mac, g.mac, 6); }

bool VirtioNetLinkUp(void)
{
    if (!g.present) return false;
    if (!(g.features & F_STATUS)) return true;
    return (*(volatile UINT16 *)(g.dev + 6) & NET_S_LINK_UP) != 0;
}

/* Take back the transmit descriptors the device has finished with */
static void tx_reclaim(Queue *q)
{
    while (q->last_used != q->used->idx) {
        q->last_used++;
        q->free_tx++;
    }
}

bool VirtioNetTransmit(const void *frame, UINT16 len)
{
    if (!g.present || len + HDR_LEN > BUF_SZ) return false;
    Queue *q = &g.q[1];
    tx_reclaim(q);
    if (!q->free_tx) return false;
    UINT16 k = q->next_tx;                                  /* (finished in order: a ring of buffers) */
    UINT8 *b = q->buf + (UINT64)k * BUF_SZ;
    memset(b, 0, HDR_LEN);
    memcpy(b + HDR_LEN, frame, len);
    q->desc[k].len = (UINT32)len + HDR_LEN;
    q->desc[k].flags = 0;
    q->next_tx = (UINT16)((k + 1) % q->size);
    q->free_tx--;
    UINT16 idx = q->avail->idx;
    q->avail->ring[idx % q->size] = k;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);                /* the entry before the index */
    q->avail->idx = (UINT16)(idx + 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify = 1;
    return true;
}

int VirtioNetReceive(void *buf, int cap)
{
    if (!g.present) return 0;
    Queue *q = &g.q[0];
    if (q->last_used == q->used->idx) return 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    volatile UsedElem *e = &q->used->ring[q->last_used % q->size];
    UINT32 id = e->id, n = e->len;
    q->last_used++;
    int len = 0;
    if (id < q->size && n > HDR_LEN) {
        len = (int)(n - HDR_LEN);
        if (len > cap) len = cap;
        memcpy(buf, q->buf + (UINT64)id * BUF_SZ + HDR_LEN, (size_t)len);
    }
    if (id < q->size) {                                     /* the buffer goes straight back */
        UINT16 idx = q->avail->idx;
        q->avail->ring[idx % q->size] = (UINT16)id;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        q->avail->idx = (UINT16)(idx + 1);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *q->notify = 0;
    }
    return len;
}
