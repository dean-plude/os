/*
 * nvme.c — NVM Express disk driver
 *
 * Follows the NVM Express Base Specification 2.0 (NVM command set).  Each
 * controller gets an admin queue pair and one I/O queue pair; commands
 * are issued one at a time per controller and completed by polling the
 * completion queue's phase bit, so no interrupts are routed.  Every
 * active namespace with 512-byte blocks becomes a block device (block.h);
 * transfers go through a bounce buffer described by a PRP list.
 */

#include "nvme.h"
#include "../fs/block.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* Controller registers */
#define REG_CAP     0x00
#define REG_VS      0x08
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28
#define REG_ACQ     0x30

#define CC_EN       (1u << 0)
#define CSTS_RDY    (1u << 0)
#define CSTS_CFS    (1u << 1)

/* Admin opcodes */
#define ADM_CREATE_SQ   0x01
#define ADM_CREATE_CQ   0x05
#define ADM_IDENTIFY    0x06
/* NVM opcodes */
#define NVM_FLUSH       0x00
#define NVM_WRITE       0x01
#define NVM_READ        0x02

#define QSIZE           64              /* entries per queue */
#define XFER_PAGES      32              /* 128 KiB per command */
#define MAX_CTRL        4
#define MAX_NS          8

typedef struct __attribute__((packed)) {
    UINT8  opc, flags;
    UINT16 cid;
    UINT32 nsid;
    UINT64 rsvd;
    UINT64 mptr;
    UINT64 prp1, prp2;
    UINT32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} Sqe;

typedef struct __attribute__((packed)) {
    UINT32 dw0, dw1;
    UINT16 sq_head, sq_id;
    UINT16 cid;
    UINT16 status;                      /* bit 0: phase */
} Cqe;

typedef struct {
    Sqe          *sq;
    volatile Cqe *cq;
    UINT16        sq_tail, cq_head;
    UINT8         phase;
    UINT16        qid;
} Queue;

typedef struct Ctrl Ctrl;

typedef struct {
    BlockDev dev;
    Ctrl    *c;
    UINT32   nsid;
} Ns;

struct Ctrl {
    volatile UINT8 *regs;
    UINT32          dstrd;              /* doorbell stride, bytes */
    UINT32          timeout_ms;
    Queue           admin, io;
    UINT8          *buf;                /* bounce buffer, XFER_PAGES pages */
    UINT64         *prp_list;           /* one page */
    UINT16          cid;
    volatile int    busy;
    Ns              ns[MAX_NS];
    int             nns;
    PciDevice       pci;
};

static Ctrl g_ctrl[MAX_CTRL];
static int  g_nctrl, g_ndisks;

static inline UINT64 phys(const volatile void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }
static inline UINT32 rd32(Ctrl *c, UINT32 r)          { return *(volatile UINT32 *)(c->regs + r); }
static inline void   wr32(Ctrl *c, UINT32 r, UINT32 v) { *(volatile UINT32 *)(c->regs + r) = v; }
static inline UINT64 rd64(Ctrl *c, UINT32 r)          { return rd32(c, r) | (UINT64)rd32(c, r + 4) << 32; }
static inline void   wr64(Ctrl *c, UINT32 r, UINT64 v) { wr32(c, r, (UINT32)v); wr32(c, r + 4, (UINT32)(v >> 32)); }

static void *alloc_zero(int pages)
{
    void *p = kernel_alloc_pages((size_t)pages);
    if (p) memset(p, 0, (size_t)pages * PAGE_SIZE);
    return p;
}

static void take(volatile int *f)
{
    while (__atomic_exchange_n(f, 1, __ATOMIC_ACQUIRE)) {
        if (interrupts_enabled()) sched_yield();
        else pause_cpu();
    }
}
static void drop(volatile int *f) { __atomic_store_n(f, 0, __ATOMIC_RELEASE); }

static bool queue_alloc(Queue *q, UINT16 qid)
{
    if (!q->sq) {
        q->sq = alloc_zero(1);
        q->cq = alloc_zero(1);
        if (!q->sq || !q->cq) return false;
    }
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->qid = qid;
    return true;
}

/* Submit @cmd on @q and wait for its completion.  Returns the status
 * field (0 = success), or -1 on a timeout. */
static int submit(Ctrl *c, Queue *q, Sqe *cmd, UINT32 *dw0)
{
    cmd->cid = ++c->cid;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (UINT16)((q->sq_tail + 1) % QSIZE);
    mfence();
    wr32(c, 0x1000 + (2 * q->qid) * c->dstrd, q->sq_tail);
    UINT64 deadline = rdtsc() + g_tsc_per_tick * (UINT64)c->timeout_ms / 10;
    for (;;) {
        volatile Cqe *e = &q->cq[q->cq_head];
        UINT16 st = e->status;
        if ((st & 1) == q->phase) {
            __asm__ volatile ("" ::: "memory");
            UINT16 cid = e->cid;
            if (dw0) *dw0 = e->dw0;
            if (++q->cq_head == QSIZE) { q->cq_head = 0; q->phase ^= 1; }
            wr32(c, 0x1000 + (2 * q->qid + 1) * c->dstrd, q->cq_head);
            if (cid != cmd->cid) continue;               /* (a late completion of an earlier command) */
            return st >> 1;
        }
        if (rdtsc() > deadline || (rd32(c, REG_CSTS) & CSTS_CFS)) {
            kprintf("[NVME] command %02x timed out (CSTS %08x)\n", cmd->opc, rd32(c, REG_CSTS));
            return -1;
        }
        pause_cpu();
    }
}

/* Describe @bytes of the bounce buffer in cmd's PRP entries */
static void set_prps(Ctrl *c, Sqe *cmd, UINT32 bytes)
{
    UINT32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    cmd->prp1 = phys(c->buf);
    if (pages <= 1) cmd->prp2 = 0;
    else if (pages == 2) cmd->prp2 = phys(c->buf + PAGE_SIZE);
    else {
        for (UINT32 i = 1; i < pages; i++) c->prp_list[i - 1] = phys(c->buf + (size_t)i * PAGE_SIZE);
        cmd->prp2 = phys(c->prp_list);
    }
}

static bool rw(Ns *n, bool write, UINT64 lba, UINT32 count, UINT8 *data)
{
    Ctrl *c = n->c;
    UINT32 per = XFER_PAGES * PAGE_SIZE / BLOCK_SECTOR;
    while (count) {
        UINT32 k = count < per ? count : per;
        UINT32 bytes = k * BLOCK_SECTOR;
        if (write) memcpy(c->buf, data, bytes);
        Sqe cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opc = write ? NVM_WRITE : NVM_READ;
        cmd.nsid = n->nsid;
        set_prps(c, &cmd, bytes);
        cmd.cdw10 = (UINT32)lba;
        cmd.cdw11 = (UINT32)(lba >> 32);
        cmd.cdw12 = k - 1;
        int st = submit(c, &c->io, &cmd, NULL);
        if (st != 0) {
            kprintf("[NVME] %s: %s at LBA %llu failed (status %x)\n", n->dev.name, write ? "write" : "read",
                    (unsigned long long)lba, st);
            return false;
        }
        if (!write) memcpy(data, c->buf, bytes);
        data += bytes; lba += k; count -= k;
    }
    return true;
}

static bool nvme_read(BlockDev *bd, UINT64 lba, UINT32 count, void *buf)
{
    Ns *n = bd->ctx;
    if (lba + count > bd->sectors) return false;
    take(&n->c->busy);
    bool ok = rw(n, false, lba, count, buf);
    drop(&n->c->busy);
    return ok;
}

static bool nvme_write(BlockDev *bd, UINT64 lba, UINT32 count, const void *buf)
{
    Ns *n = bd->ctx;
    if (lba + count > bd->sectors) return false;
    take(&n->c->busy);
    bool ok = rw(n, true, lba, count, (UINT8 *)buf);
    drop(&n->c->busy);
    return ok;
}

static bool nvme_flush(BlockDev *bd)
{
    Ns *n = bd->ctx;
    take(&n->c->busy);
    Sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = NVM_FLUSH;
    cmd.nsid = n->nsid;
    int st = submit(n->c, &n->c->io, &cmd, NULL);
    drop(&n->c->busy);
    return st == 0;
}

static bool wait_ready(Ctrl *c, bool ready)
{
    UINT64 deadline = rdtsc() + g_tsc_per_tick * (UINT64)c->timeout_ms / 10;
    while (((rd32(c, REG_CSTS) & CSTS_RDY) != 0) != ready) {
        if (rdtsc() > deadline) return false;
        pause_cpu();
    }
    return true;
}

/* Reset the controller and set up its queues (boot and wake) */
static bool ctrl_program(Ctrl *c)
{
    if (rd32(c, REG_CC) & CC_EN) {
        wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
        if (!wait_ready(c, false)) { kprintf("[NVME] controller did not stop\n"); return false; }
    }
    if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1)) return false;
    wr32(c, REG_AQA, (UINT32)(QSIZE - 1) << 16 | (QSIZE - 1));
    wr64(c, REG_ASQ, phys(c->admin.sq));
    wr64(c, REG_ACQ, phys(c->admin.cq));
    /* NVM command set, 4 KiB pages, round robin, 64-byte SQ and 16-byte CQ entries */
    wr32(c, REG_CC, CC_EN | (6u << 16) | (4u << 20));
    if (!wait_ready(c, true)) { kprintf("[NVME] controller did not become ready\n"); return false; }

    Sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = ADM_CREATE_CQ;
    cmd.prp1 = phys(c->io.cq);
    cmd.cdw10 = (UINT32)(QSIZE - 1) << 16 | 1;
    cmd.cdw11 = 1;                                     /* physically contiguous, no interrupts */
    if (submit(c, &c->admin, &cmd, NULL) != 0) { kprintf("[NVME] Create I/O CQ failed\n"); return false; }
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = ADM_CREATE_SQ;
    cmd.prp1 = phys(c->io.sq);
    cmd.cdw10 = (UINT32)(QSIZE - 1) << 16 | 1;
    cmd.cdw11 = 1u << 16 | 1;                          /* completes to CQ 1, contiguous */
    if (submit(c, &c->admin, &cmd, NULL) != 0) { kprintf("[NVME] Create I/O SQ failed\n"); return false; }
    return true;
}

static bool identify(Ctrl *c, UINT32 cns, UINT32 nsid)
{
    Sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = ADM_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = phys(c->buf);
    cmd.cdw10 = cns;
    return submit(c, &c->admin, &cmd, NULL) == 0;
}

static void trim(char *s, int n)
{
    s[n] = 0;
    while (n > 0 && (s[n - 1] == ' ' || !s[n - 1])) s[--n] = 0;
}

static void probe(const PciDevice *pci)
{
    if (g_nctrl >= MAX_CTRL) return;
    Ctrl *c = &g_ctrl[g_nctrl];
    memset(c, 0, sizeof(*c));
    c->pci = *pci;
    c->regs = PciMapBar(pci, 0);
    if (!c->regs) return;
    PciEnableDevice(pci);
    UINT64 cap = rd64(c, REG_CAP);
    c->dstrd = 4u << ((cap >> 32) & 0xF);
    c->timeout_ms = (UINT32)((cap >> 24) & 0xFF) * 500 + 1000;
    if (((cap >> 48) & 0xF) > 0) { kprintf("[NVME] controller needs pages over 4 KiB\n"); return; }
    if (!((cap >> 37) & 1)) { kprintf("[NVME] controller lacks the NVM command set\n"); return; }
    c->buf = alloc_zero(XFER_PAGES);
    c->prp_list = alloc_zero(1);
    if (!c->buf || !c->prp_list) return;
    if (!ctrl_program(c)) return;

    if (!identify(c, 1, 0)) { kprintf("[NVME] Identify Controller failed\n"); return; }
    char model[41];
    memcpy(model, c->buf + 24, 40);
    trim(model, 40);
    UINT32 nn = *(UINT32 *)(c->buf + 516);
    UINT32 vs = rd32(c, REG_VS);
    kprintf("[NVME] %02x:%02x.%d: \"%s\", NVMe %u.%u, %u namespace(s)\n", pci->bus, pci->dev, pci->func,
            model, vs >> 16, (vs >> 8) & 0xFF, nn);
    g_nctrl++;
    PciClaim(pci, "NVMe");

    /* The active namespaces (NVMe 1.1+), or every ID up to NN */
    UINT32 ids[MAX_NS], nids = 0;
    if (vs >= 0x10100 && identify(c, 2, 0)) {
        for (int i = 0; i < 1024 && nids < MAX_NS; i++) {
            UINT32 id = ((UINT32 *)c->buf)[i];
            if (!id) break;
            ids[nids++] = id;
        }
    } else {
        for (UINT32 id = 1; id <= nn && nids < MAX_NS; id++) ids[nids++] = id;
    }
    for (UINT32 k = 0; k < nids && c->nns < MAX_NS; k++) {
        UINT32 id = ids[k];
        if (!identify(c, 0, id)) continue;
        UINT64 nsze = *(UINT64 *)c->buf;
        if (!nsze) continue;                           /* not active */
        UINT8 flbas = c->buf[26] & 0xF;
        UINT32 lbaf = *(UINT32 *)(c->buf + 128 + 4 * flbas);
        UINT32 bsize = 1u << ((lbaf >> 16) & 0xFF);
        if (bsize != BLOCK_SECTOR || (lbaf & 0xFFFF)) {
            kprintf("[NVME] namespace %u: %u-byte blocks%s, not supported\n", id, bsize,
                    (lbaf & 0xFFFF) ? " with metadata" : "");
            continue;
        }
        Ns *n = &c->ns[c->nns++];
        n->c = c;
        n->nsid = id;
        ksnprintf(n->dev.name, sizeof(n->dev.name), "nvme%dn%u", g_nctrl - 1, id);
        g_ndisks++;
        strncpy(n->dev.model, model, sizeof(n->dev.model) - 1);
        n->dev.sectors = nsze;
        n->dev.read = nvme_read;
        n->dev.write = nvme_write;
        n->dev.flush = nvme_flush;
        n->dev.ctx = n;
        BlockRegister(&n->dev);
    }
}

int NvmeInit(void)
{
    PciDevice pci;
    for (int i = 0; PciFindClass(0x01, 0x08, 0x02, i, &pci); i++) probe(&pci);
    return g_ndisks;
}

void NvmeResume(void)
{
    for (int i = 0; i < g_nctrl; i++) {
        Ctrl *c = &g_ctrl[i];
        PciEnableDevice(&c->pci);
        if (!ctrl_program(c)) kprintf("[NVME] controller %d did not restart after sleep\n", i);
    }
}
