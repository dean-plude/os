/*
 * paging.h — page table setup for the UEFI bootloader
 *
 * The bootloader creates the initial page tables required for the kernel
 * to start executing at its high virtual address, while the bootloader's
 * own code continues to work via identity mapping.
 */

#pragma once

#include "../include/efi.h"

/*
 * Build the initial 4-level (PML4) page tables:
 *
 *  1. Identity map first 4 GiB at VA 0x0000000000000000
 *     (keeps bootloader code valid after CR3 switch)
 *
 *  2. Map 64 GiB of physical RAM at PHYSMAP_BASE (0xFFFF800000000000)
 *     using 1 GiB huge pages
 *
 *  3. Map kernel [phys_base .. phys_base+kernel_size) to
 *     [virt_base .. virt_base+kernel_size) using 4 KiB pages
 *
 * The new CR3 is returned in cr3_out.
 * Memory for the tables is allocated via bs->AllocatePages(EfiLoaderData).
 */
EFI_STATUS paging_build(
    UINT64             kernel_phys_base,
    UINT64             kernel_virt_base,
    UINT64             kernel_size,
    UINT64            *cr3_out,
    EFI_BOOT_SERVICES *bs);

/* Virtual base for the direct physical map */
#define PHYSMAP_BASE  UINT64_C(0xFFFF800000000000)
/* How many GiB to map (64 GiB is plenty for Phase 1) */
#define PHYSMAP_GIB   64
