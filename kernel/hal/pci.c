/*
 * pci.c — PCI configuration space access and device discovery
 */

#include "../ke/spinlock.h"
#include "pci.h"
#include "../arch/x86_64/cpu.h"
#include "../ke/printf.h"
#include "../arch/x86_64/paging.h"

#define PCI_ADDR  0xCF8
#define PCI_DATA  0xCFC

#define PCI_MAX_DEVICES 64
static PciDevice g_devices[PCI_MAX_DEVICES];
static int       g_count;

static UINT32 cfg_addr(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    return 0x80000000u | ((UINT32)bus << 16) | ((UINT32)(dev & 31) << 11) |
           ((UINT32)(func & 7) << 8) | (off & 0xFC);
}

/* The configuration mechanism is an address port and a data port: one
 * access at a time, from any CPU */
static KSpinLock g_cfg_lock = KSPINLOCK_INIT;

UINT32 PciRead32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    IrqState s = spin_lock_irqsave(&g_cfg_lock);
    outl(PCI_ADDR, cfg_addr(bus, dev, func, off));
    UINT32 v = inl(PCI_DATA);
    spin_unlock_irqrestore(&g_cfg_lock, s);
    return v;
}

void PciWrite32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT32 val)
{
    IrqState s = spin_lock_irqsave(&g_cfg_lock);
    outl(PCI_ADDR, cfg_addr(bus, dev, func, off));
    outl(PCI_DATA, val);
    spin_unlock_irqrestore(&g_cfg_lock, s);
}

UINT16 PciRead16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    return (UINT16)(PciRead32(bus, dev, func, off) >> ((off & 2) * 8));
}

void PciWrite16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT16 val)
{
    UINT32 v = PciRead32(bus, dev, func, off);
    int sh = (off & 2) * 8;
    v = (v & ~(0xFFFFu << sh)) | ((UINT32)val << sh);
    PciWrite32(bus, dev, func, off, v);
}

static void probe(UINT8 bus, UINT8 dev, UINT8 func)
{
    UINT32 id = PciRead32(bus, dev, func, 0x00);
    if ((id & 0xFFFF) == 0xFFFF || g_count >= PCI_MAX_DEVICES) return;
    UINT32 cls = PciRead32(bus, dev, func, 0x08);
    PciDevice *d = &g_devices[g_count++];
    d->bus = bus; d->dev = dev; d->func = func;
    d->vendor     = (UINT16)(id & 0xFFFF);
    d->device     = (UINT16)(id >> 16);
    d->class_code = (UINT8)(cls >> 24);
    d->subclass   = (UINT8)(cls >> 16);
    d->prog_if    = (UINT8)(cls >> 8);
    d->irq_line   = (UINT8)PciRead32(bus, dev, func, 0x3C);
    kprintf("[PCI] %02x:%02x.%x %04x:%04x class %02x.%02x\n", bus, dev, func,
            d->vendor, d->device, d->class_code, d->subclass);
}

void PciInitialize(void)
{
    g_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            if ((PciRead32((UINT8)bus, (UINT8)dev, 0, 0) & 0xFFFF) == 0xFFFF) continue;
            probe((UINT8)bus, (UINT8)dev, 0);
            /* multi-function device? (header type bit 7) */
            if (PciRead32((UINT8)bus, (UINT8)dev, 0, 0x0C) & 0x00800000)
                for (int f = 1; f < 8; f++) probe((UINT8)bus, (UINT8)dev, (UINT8)f);
        }
    }
    kprintf("[PCI] %d function(s) found\n", g_count);
}

bool PciFind(UINT16 vendor, const UINT16 *ids, int n, PciDevice *out)
{
    for (int i = 0; i < g_count; i++) {
        if (g_devices[i].vendor != vendor) continue;
        for (int j = 0; j < n; j++) {
            if (g_devices[i].device == ids[j]) {
                if (out) *out = g_devices[i];
                return true;
            }
        }
    }
    return false;
}

UINT64 PciBarAddress(const PciDevice *d, int bar)
{
    UINT8 off = (UINT8)(0x10 + bar * 4);
    UINT32 lo = PciRead32(d->bus, d->dev, d->func, off);
    if (lo & 1) return 0;                          /* I/O BAR */
    UINT64 addr = lo & ~0xFu;
    if (((lo >> 1) & 3) == 2)                      /* 64-bit BAR */
        addr |= (UINT64)PciRead32(d->bus, d->dev, d->func, (UINT8)(off + 4)) << 32;
    return addr;
}

/* Size of a memory BAR: write all ones, see which address bits stick */
static UINT64 bar_size(const PciDevice *d, int bar)
{
    UINT8 off = (UINT8)(0x10 + bar * 4);
    bool is64 = ((PciRead32(d->bus, d->dev, d->func, off) >> 1) & 3) == 2;
    UINT16 cmd = PciRead16(d->bus, d->dev, d->func, 0x04);
    PciWrite16(d->bus, d->dev, d->func, 0x04, (UINT16)(cmd & ~2u));   /* decoding off meanwhile */
    UINT32 lo = PciRead32(d->bus, d->dev, d->func, off), hi = 0;
    PciWrite32(d->bus, d->dev, d->func, off, 0xFFFFFFFFu);
    UINT64 mask = PciRead32(d->bus, d->dev, d->func, off) & ~0xFull;
    PciWrite32(d->bus, d->dev, d->func, off, lo);
    if (is64) {
        hi = PciRead32(d->bus, d->dev, d->func, (UINT8)(off + 4));
        PciWrite32(d->bus, d->dev, d->func, (UINT8)(off + 4), 0xFFFFFFFFu);
        mask |= (UINT64)PciRead32(d->bus, d->dev, d->func, (UINT8)(off + 4)) << 32;
        PciWrite32(d->bus, d->dev, d->func, (UINT8)(off + 4), hi);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }
    PciWrite16(d->bus, d->dev, d->func, 0x04, cmd);
    return mask ? ~mask + 1 : 0;
}

#define PHYSMAP_SIZE (64ull << 30)
/* MMIO window: after the physmap, in the same top-level page table entry,
 * which every address space shares (paging_create_process_pt) */
static UINT64 g_mmio_next = PHYSMAP_BASE + (256ull << 30);
static KSpinLock g_mmio_lock = KSPINLOCK_INIT;

volatile void *PciMapBar(const PciDevice *d, int bar)
{
    UINT64 pa = PciBarAddress(d, bar);
    if (!pa) return NULL;
    UINT64 size = bar_size(d, bar);
    if (size < PAGE_SIZE) size = PAGE_SIZE;
    if (pa + size <= PHYSMAP_SIZE) return (volatile void *)(uintptr_t)(PHYSMAP_BASE + pa);
    IrqState s = spin_lock_irqsave(&g_mmio_lock);
    UINT64 va = g_mmio_next;
    g_mmio_next += (size + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    spin_unlock_irqrestore(&g_mmio_lock, s);
    if (!NT_SUCCESS(paging_map((uintptr_t)va, (uintptr_t)pa, (size_t)size,
                               MAP_WRITABLE | MAP_NO_CACHE | MAP_NO_EXEC))) {
        kprintf("[PCI] cannot map BAR%d at %llx\n", bar, (unsigned long long)pa);
        return NULL;
    }
    return (volatile void *)(uintptr_t)va;
}

void PciEnableDevice(const PciDevice *d)
{
    UINT16 cmd = PciRead16(d->bus, d->dev, d->func, 0x04);
    cmd |= (1u << 1) | (1u << 2);                  /* memory space, bus master */
    cmd |= (1u << 10);          /* INTx off: drivers poll (no IOAPIC routing yet) */
    PciWrite16(d->bus, d->dev, d->func, 0x04, cmd);
}

bool PciFindClass(UINT8 class_code, UINT8 subclass, UINT8 prog_if, int index, PciDevice *out)
{
    for (int i = 0; i < g_count; i++) {
        const PciDevice *d = &g_devices[i];
        if (d->class_code == class_code && d->subclass == subclass && d->prog_if == prog_if && index-- == 0) {
            *out = *d;
            return true;
        }
    }
    return false;
}
