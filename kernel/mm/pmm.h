/*
 * pmm.h — Physical Memory Manager
 *
 * The PMM manages physical page allocation.  Phase 1 uses a simple
 * two-level bitmap allocator:
 *
 *   - All physical RAM is divided into 4 KiB pages.
 *   - A bitmap stores one bit per page: 0 = free, 1 = used.
 *   - A secondary "super-bitmap" has one bit per 64-page group,
 *     set if ALL 64 pages in that group are allocated.  This lets
 *     us skip fully-allocated regions quickly during free-page search.
 *
 * Performance:
 *   - alloc: O(N/64) in the worst case (walks super-bitmap)
 *   - free:  O(1)
 *
 * This is sufficient for Phase 1.  Phase 2 will replace it with a
 * buddy allocator when we need large physically-contiguous allocations
 * for DMA and kernel heap.
 *
 * Physical memory above 4 GiB is fully supported (64-bit physical addresses).
 *
 * Thread safety: All operations take a global spinlock.  SMP will refine
 * this to per-NUMA-node locks in a later phase.
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

/*
 * Initialize the PMM from the boot memory map.
 *
 * Marks all BOOT_MEM_CONVENTIONAL regions as free, everything else as used.
 * Then re-marks the kernel image and PMM bitmap itself as used to prevent
 * them from being handed out.
 *
 * Must be called before any other PMM or VMM function.
 */
void pmm_init(const BootInfo *info);

/*
 * Allocate one physical page (4 KiB).
 * Returns the physical address of the page, or 0 on out-of-memory.
 * The returned page is NOT zeroed.
 */
uintptr_t pmm_alloc_page(void);

/*
 * Allocate `count` physically contiguous pages.
 * Returns the physical address of the first page, or 0 on failure.
 * Pages are NOT zeroed.
 */
uintptr_t pmm_alloc_pages(size_t count);

/*
 * Take the `count` pages starting at @pa, when every one of them is free.
 * False (and nothing taken) otherwise.
 */
bool pmm_claim_pages(uintptr_t pa, size_t count);

/*
 * Free a single physical page previously returned by pmm_alloc_page().
 * @pa must be 4 KiB-aligned.
 */
void pmm_free_page(uintptr_t pa);

/*
 * Free `count` physically contiguous pages starting at @pa.
 */
void pmm_free_pages(uintptr_t pa, size_t count);

/*
 * Query statistics.
 */
void pmm_stats(uint64_t *total_pages_out,
               uint64_t *free_pages_out,
               uint64_t *used_pages_out);

/*
 * The free page count without taking the lock (a snapshot for figures
 * programs see, such as KUSER_SHARED_DATA's; safe with interrupts off).
 */
size_t pmm_free_now(void);

/*
 * The machine's RAM in pages: the firmware's conventional memory (the
 * total pmm_stats gives also counts the holes below the highest address).
 */
size_t pmm_ram_pages(void);

/*
 * Mark a physical range as used (prevents allocation of those pages).
 * Used to protect firmware regions, MMIO, etc.
 */
void pmm_mark_used(uintptr_t pa, size_t size);

/*
 * Mark a physical range as free (makes those pages available).
 * Used after reclaiming bootloader/ACPI memory.
 */
void pmm_mark_free(uintptr_t pa, size_t size);
