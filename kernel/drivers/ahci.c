/*
 * ahci.c — AHCI (SATA) disk driver
 *
 * Follows the Serial ATA AHCI 1.3.1 specification.  Every SATA disk on
 * every AHCI controller becomes a block device (block.h).  Commands are
 * issued one at a time on slot 0 and completed by polling, so no
 * interrupt routing is needed; transfers go through a physically
 * contiguous bounce buffer, so callers may pass any buffer.
 */

#include "ahci.h"
#include "../fs/block.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/apic.h"

/* HBA (generic host control) registers */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_PI      0x0C
#define GHC_AE      (1u << 31)

/* Port registers (0x100 + port * 0x80) */
#define PX_CLB      0x00
#define PX_CLBU     0x04
#define PX_FB       0x08
#define PX_FBU      0x0C
#define PX_IS       0x10
#define PX_IE       0x14
#define PX_CMD      0x18
#define PX_TFD      0x20
#define PX_SIG      0x24
#define PX_SSTS     0x28
#define PX_SERR     0x30
#define PX_CI       0x38

#define CMD_ST      (1u << 0)
#define CMD_SUD     (1u << 1)
#define CMD_POD     (1u << 2)
#define CMD_FRE     (1u << 4)
#define CMD_FR      (1u << 14)
#define CMD_CR      (1u << 15)
#define TFD_ERR     (1u << 0)
#define TFD_DRQ     (1u << 3)
#define TFD_BSY     (1u << 7)
#define IS_TFES     (1u << 30)

#define SIG_ATA     0x00000101u

#define ATA_IDENTIFY        0xEC
#define ATA_READ_DMA_EXT    0x25
#define ATA_WRITE_DMA_EXT   0x35
#define ATA_FLUSH_CACHE_EXT 0xEA

#define BOUNCE_SECTORS 128               /* 64 KiB per command */
#define MAX_DISKS      8

typedef struct __attribute__((packed)) {
    UINT16 flags;                        /* CFL (DWORDs) | W (bit 6) */
    UINT16 prdtl;
    UINT32 prdbc;
    UINT64 ctba;
    UINT32 rsvd[4];
} CmdHeader;

typedef struct __attribute__((packed)) {
    UINT64 dba;
    UINT32 rsvd;
    UINT32 dbc;                          /* byte count - 1 (bits 0-21) */
} Prd;

typedef struct __attribute__((packed)) {
    UINT8 cfis[64];
    UINT8 acmd[16];
    UINT8 rsvd[48];
    Prd   prdt[16];
} CmdTable;

typedef struct {
    BlockDev          dev;
    volatile UINT8   *abar;              /* the controller's registers */
    volatile UINT8   *port;              /* the port's registers */
    CmdHeader        *cl;                /* command list (1 KiB aligned) */
    UINT8            *fis;               /* received FIS area (256 B aligned) */
    CmdTable         *ct;                /* command table for slot 0 */
    UINT8            *bounce;            /* BOUNCE_SECTORS * 512 bytes */
} AhciDisk;

static AhciDisk g_disks[MAX_DISKS];
static int g_ndisks;

static inline UINT32 prd32(AhciDisk *d, UINT32 r)          { return *(volatile UINT32 *)(d->port + r); }
static inline void   pwr32(AhciDisk *d, UINT32 r, UINT32 v) { *(volatile UINT32 *)(d->port + r) = v; }
static inline UINT64 phys(const void *va)                   { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }

static bool wait_clear(AhciDisk *d, UINT32 reg, UINT32 bits, int spins)
{
    while (spins-- > 0) {
        if (!(prd32(d, reg) & bits)) return true;
        pause_cpu();
    }
    return false;
}

static void port_stop(AhciDisk *d)
{
    pwr32(d, PX_CMD, prd32(d, PX_CMD) & ~CMD_ST);
    wait_clear(d, PX_CMD, CMD_CR, 5000000);
    pwr32(d, PX_CMD, prd32(d, PX_CMD) & ~CMD_FRE);
    wait_clear(d, PX_CMD, CMD_FR, 5000000);
}

static void port_start(AhciDisk *d)
{
    wait_clear(d, PX_CMD, CMD_CR, 5000000);
    pwr32(d, PX_CMD, prd32(d, PX_CMD) | CMD_FRE);
    pwr32(d, PX_CMD, prd32(d, PX_CMD) | CMD_ST);
}

/* Issue one command on slot 0 and poll for completion.  @bytes of data
 * move between the device and the bounce buffer. */
static bool issue(AhciDisk *d, UINT8 cmd, UINT64 lba, UINT32 count, UINT32 bytes, bool write)
{
    if (!wait_clear(d, PX_TFD, TFD_BSY | TFD_DRQ, 10000000)) {
        kprintf("[AHCI] %s: port busy\n", d->dev.name);
        return false;
    }
    memset(d->ct, 0, sizeof(CmdTable));
    UINT8 *f = d->ct->cfis;
    f[0] = 0x27;                         /* host to device register FIS */
    f[1] = 0x80;                         /* command (not control) */
    f[2] = cmd;
    f[4] = (UINT8)lba;
    f[5] = (UINT8)(lba >> 8);
    f[6] = (UINT8)(lba >> 16);
    f[7] = 1u << 6;                      /* LBA mode */
    f[8] = (UINT8)(lba >> 24);
    f[9] = (UINT8)(lba >> 32);
    f[10] = (UINT8)(lba >> 40);
    f[12] = (UINT8)count;
    f[13] = (UINT8)(count >> 8);

    int nprd = 0;
    for (UINT32 off = 0; off < bytes; nprd++) {
        UINT32 n = bytes - off > 0x400000u ? 0x400000u : bytes - off;
        d->ct->prdt[nprd].dba = phys(d->bounce + off);
        d->ct->prdt[nprd].dbc = n - 1;
        off += n;
    }
    d->cl[0].flags = (UINT16)(5 | (write ? 1u << 6 : 0));
    d->cl[0].prdtl = (UINT16)nprd;
    d->cl[0].prdbc = 0;
    d->cl[0].ctba = phys(d->ct);

    pwr32(d, PX_IS, 0xFFFFFFFFu);
    __asm__ volatile ("mfence" ::: "memory");
    pwr32(d, PX_CI, 1);
    for (int spins = 50000000; spins > 0; spins--) {
        if (prd32(d, PX_IS) & IS_TFES) break;
        if (!(prd32(d, PX_CI) & 1)) {
            if (prd32(d, PX_TFD) & TFD_ERR) break;
            return true;
        }
        pause_cpu();
    }
    kprintf("[AHCI] %s: command %02x at LBA %llu failed (TFD %08x IS %08x)\n", d->dev.name, cmd,
            (unsigned long long)lba, prd32(d, PX_TFD), prd32(d, PX_IS));
    /* recover: restart the port so the next command can run */
    port_stop(d);
    pwr32(d, PX_SERR, 0xFFFFFFFFu);
    pwr32(d, PX_IS, 0xFFFFFFFFu);
    port_start(d);
    return false;
}

static bool ahci_read(BlockDev *bd, UINT64 lba, UINT32 count, void *buf)
{
    AhciDisk *d = bd->ctx;
    if (lba + count > bd->sectors) return false;
    UINT8 *out = buf;
    while (count) {
        UINT32 n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        if (!issue(d, ATA_READ_DMA_EXT, lba, n, n * BLOCK_SECTOR, false)) return false;
        memcpy(out, d->bounce, n * BLOCK_SECTOR);
        out += n * BLOCK_SECTOR;
        lba += n;
        count -= n;
    }
    return true;
}

static bool ahci_write(BlockDev *bd, UINT64 lba, UINT32 count, const void *buf)
{
    AhciDisk *d = bd->ctx;
    if (lba + count > bd->sectors) return false;
    const UINT8 *in = buf;
    while (count) {
        UINT32 n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        memcpy(d->bounce, in, n * BLOCK_SECTOR);
        if (!issue(d, ATA_WRITE_DMA_EXT, lba, n, n * BLOCK_SECTOR, true)) return false;
        in += n * BLOCK_SECTOR;
        lba += n;
        count -= n;
    }
    return true;
}

static bool ahci_flush(BlockDev *bd)
{
    return issue(bd->ctx, ATA_FLUSH_CACHE_EXT, 0, 0, 0, false);
}

/* Point the port at its command list and FIS area and start it */
static void port_setup(AhciDisk *d)
{
    port_stop(d);
    pwr32(d, PX_CLB, (UINT32)phys(d->cl));
    pwr32(d, PX_CLBU, (UINT32)(phys(d->cl) >> 32));
    pwr32(d, PX_FB, (UINT32)phys(d->fis));
    pwr32(d, PX_FBU, (UINT32)(phys(d->fis) >> 32));
    pwr32(d, PX_IE, 0);                                  /* polled */
    pwr32(d, PX_SERR, 0xFFFFFFFFu);
    pwr32(d, PX_IS, 0xFFFFFFFFu);
    port_start(d);
}

/* IDENTIFY strings are byte-swapped in 16-bit words and space padded */
static void ata_string(const UINT16 *w, int words, char *out)
{
    int n = 0;
    for (int i = 0; i < words; i++) {
        out[n++] = (char)(w[i] >> 8);
        out[n++] = (char)w[i];
    }
    while (n > 0 && (out[n - 1] == ' ' || !out[n - 1])) n--;
    out[n] = '\0';
    int s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, (size_t)(n - s + 1));
}

static void probe_port(volatile UINT8 *abar, int port)
{
    if (g_ndisks >= MAX_DISKS) return;
    AhciDisk *d = &g_disks[g_ndisks];
    memset(d, 0, sizeof(*d));
    d->abar = abar;
    d->port = abar + 0x100 + port * 0x80;
    UINT32 ssts = prd32(d, PX_SSTS);
    if ((ssts & 0xF) != 3) return;                       /* no device, or PHY not up */
    if (prd32(d, PX_SIG) != SIG_ATA) return;             /* ATAPI (CD-ROM), port multiplier... */

    UINT8 *mem = kernel_alloc_pages(2 + BOUNCE_SECTORS * BLOCK_SECTOR / PAGE_SIZE);
    if (!mem) return;
    memset(mem, 0, 2 * PAGE_SIZE);
    d->cl = (CmdHeader *)mem;                            /* 1 KiB */
    d->fis = mem + 1024;                                 /* 256 B */
    d->ct = (CmdTable *)(mem + PAGE_SIZE);
    d->bounce = mem + 2 * PAGE_SIZE;

    port_setup(d);

    if (!issue(d, ATA_IDENTIFY, 0, 0, 512, false)) return;
    const UINT16 *id = (const UINT16 *)d->bounce;
    if (!(id[83] & (1u << 10))) {
        kprintf("[AHCI] port %d: disk without 48-bit LBA, ignored\n", port);
        return;
    }
    d->dev.sectors = *(const UINT64 *)&id[100];
    ata_string(&id[27], 20, d->dev.model);
    ksnprintf(d->dev.name, sizeof(d->dev.name), "sata%d", g_ndisks);
    d->dev.read = ahci_read;
    d->dev.write = ahci_write;
    d->dev.flush = ahci_flush;
    d->dev.ctx = d;
    g_ndisks++;
    BlockRegister(&d->dev);
}

int AhciInit(void)
{
    PciDevice pci;
    for (int c = 0; PciFindClass(0x01, 0x06, 0x01, c, &pci); c++) {
        UINT64 bar = PciBarAddress(&pci, 5);
        if (!bar) continue;
        PciEnableDevice(&pci);
        volatile UINT8 *abar = (volatile UINT8 *)(uintptr_t)(PHYSMAP_BASE + bar);
        *(volatile UINT32 *)(abar + HBA_GHC) |= GHC_AE;
        UINT32 pi = *(volatile UINT32 *)(abar + HBA_PI);
        kprintf("[AHCI] controller %02x:%02x.%d, ports %08x\n", pci.bus, pci.dev, pci.func, pi);
        for (int p = 0; p < 32; p++)
            if (pi & (1u << p)) probe_port(abar, p);
    }
    return g_ndisks;
}

/* After S3: the controllers were reset; set each disk's port up again
 * (once its link is back) with the same command list and buffers */
void AhciResume(void)
{
    for (int i = 0; i < g_ndisks; i++) {
        AhciDisk *d = &g_disks[i];
        *(volatile UINT32 *)(d->abar + HBA_GHC) |= GHC_AE;
        pwr32(d, PX_CMD, prd32(d, PX_CMD) | CMD_SUD | CMD_POD);     /* spin up, power on */
        for (int t = 0; t < 1000 && (prd32(d, PX_SSTS) & 0xF) != 3; t++) udelay(1000);
        port_setup(d);
    }
}
