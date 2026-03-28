/*
 * vmm.h — Kernel Virtual Memory Manager
 *
 * Phase 1 scope:
 *   - A slab-style kernel heap (kmalloc/kfree) backed by PMM pages
 *   - kernel_alloc_pages() / kernel_free_pages() for page-granular
 *     kernel virtual memory allocation
 *   - No user address space management yet (Phase 2)
 *
 * The kernel heap lives in the PHYSMAP window — we allocate physical pages
 * from the PMM and address them via PHYSMAP_BASE + phys.  This means we
 * don't need to set up new page table entries for the heap in Phase 1
 * (the physmap covers all physical RAM).
 *
 * Heap allocator design:
 *   - Power-of-2 slab caches: 8, 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
 *   - Each slab cache manages a list of 4 KiB pages (slabs)
 *   - Each page is divided into fixed-size cells; a free list links cells
 *   - Allocations >2048 bytes (and >PAGE_SIZE/2) are served directly
 *     from the PMM (multi-page allocations)
 *
 * This is modeled loosely on the classic McKusick-Karels allocator.
 */

#pragma once

#include "../include/types.h"

/*
 * Initialize the virtual memory manager.
 * Must be called after pmm_init() and paging_init().
 */
void vmm_init(void);

/*
 * Allocate `size` bytes from the kernel heap.
 * Returns a kernel virtual address, or NULL on OOM.
 * The memory is NOT zeroed.
 */
void *kmalloc(size_t size);

/*
 * Allocate `size` bytes, zeroed.
 */
void *kzalloc(size_t size);

/*
 * Free a pointer returned by kmalloc/kzalloc.
 */
void kfree(void *ptr);

/*
 * Allocate `count` physically contiguous pages.
 * Returns a kernel virtual address (physmap window), or NULL on OOM.
 * Pages are NOT zeroed.
 */
void *kernel_alloc_pages(size_t count);

/*
 * Free pages allocated with kernel_alloc_pages().
 * @ptr must be the address returned by kernel_alloc_pages().
 * @count must match the count passed to the allocator.
 */
void kernel_free_pages(void *ptr, size_t count);
