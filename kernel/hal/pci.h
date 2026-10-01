/*
 * pci.h — PCI configuration space access and device discovery
 *
 * Uses configuration mechanism #1 (ports 0xCF8/0xCFC), which every PC
 * chipset and QEMU's i440fx/q35 machines support.
 */

#pragma once

#include "../include/types.h"

typedef struct {
    UINT8  bus, dev, func;
    UINT16 vendor, device;
    UINT8  class_code, subclass, prog_if;
    UINT8  irq_line;
} PciDevice;

UINT32 PciRead32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off);
void   PciWrite32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT32 val);
UINT16 PciRead16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off);
void   PciWrite16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT16 val);

/* Enumerate all functions; logs each one.  Call once at boot. */
void PciInitialize(void);

/* Find the first device with this vendor and one of `ids` (count n). */
bool PciFind(UINT16 vendor, const UINT16 *ids, int n, PciDevice *out);

/* The @index'th device (0 = first) of a class, e.g. 01/06/01 for AHCI. */
bool PciFindClass(UINT8 class_code, UINT8 subclass, UINT8 prog_if, int index, PciDevice *out);

/* Physical base address of a memory BAR (handles 64-bit BARs). */
UINT64 PciBarAddress(const PciDevice *d, int bar);

/* Kernel address of a memory BAR's registers.  BARs inside the 64 GiB
 * physmap are reached through it; firmware often places 64-bit BARs far
 * above that (OVMF: at 512 GiB and up), so those get mapped, uncached,
 * into a window after the physmap.  NULL for an I/O or unassigned BAR. */
volatile void *PciMapBar(const PciDevice *d, int bar);

/* Turn on memory-space decoding and bus mastering (for DMA). */
/* Map @size bytes of physical memory or MMIO at @pa (uncached above the
 * physical-memory map); NULL if it can't.  Never unmapped. */
volatile void *PciMapPhysical(UINT64 pa, UINT64 size);

void PciEnableDevice(const PciDevice *d);

/* S3: save every function's configuration header, and put it back on wake */
void PciSaveAll(void);
void PciRestoreAll(void);
