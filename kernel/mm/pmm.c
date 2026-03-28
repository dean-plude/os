/*
 * pmm.c — Physical Memory Manager (bitmap allocator)
 *
 * Implementation notes:
 *
 * We store the bitmap in the physical memory itself — specifically in the
 * first suitable BOOT_MEM_CONVENTIONAL region large enough to hold it.
 * The bootloader placed its own data in EfiLoaderData, which shows up as
 * BOOT_MEM_LOADER_DATA in our map; we treat that as used too.
 *
 * Bitmap layout:
 *   - One uint64_t word covers 64 pages (256 KiB of RAM).
 *   - Bit N of word W corresponds to physical page (W*64 + N).
 *   - Bit = 1 means page is USED; bit = 0 means FREE.
 *
 * We detect the highest physical address from the memory map and size
 * the bitmap accordingly.  For a 4 GiB machine: 4G/4K = 1M pages,
 * 1M/8 = 128 KiB bitmap.  For 64 GiB: 2 MiB bitmap.  Both fit easily.
 *
 * The bitmap itself is accessed via the physmap (PHYSMAP_BASE + phys_addr)
 * because by the time pmm_init() is called, the kernel is running at its
 * high virtual address and the physmap is live.
 */

#include "pmm.h"
#include "../ke/printf.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"

/* -----------------------------------------------------------------------
 * Spinlock (simple ticket lock for Phase 1 / single CPU)
 * ----------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t next;
    volatile uint32_t owner;
} TicketLock;

static void lock_acquire(TicketLock *l)
{
    uint32_t ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket)
        pause_cpu();
}

static void lock_release(TicketLock *l)
{
    __atomic_fetch_add(&l->owner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * PMM state
 * ----------------------------------------------------------------------- */
static struct {
    uint64_t *bitmap;         /* Virtual address of bitmap (via physmap) */
    uint64_t  bitmap_phys;    /* Physical address of bitmap */
    size_t    total_pages;    /* Total number of physical pages tracked */
    size_t    free_pages;     /* Current free page count */
    size_t    bitmap_words;   /* Number of uint64_t words in bitmap */
    uintptr_t highest_phys;   /* Highest physical address + 1 */
    TicketLock lock;
} pmm;

/* -----------------------------------------------------------------------
 * Bitmap helpers (page index → bit position)
 * ----------------------------------------------------------------------- */

static void bitmap_set(size_t page_idx)   /* mark used */
{
    pmm.bitmap[page_idx / 64] |= (UINT64_C(1) << (page_idx % 64));
}

static void bitmap_clear(size_t page_idx) /* mark free */
{
    pmm.bitmap[page_idx / 64] &= ~(UINT64_C(1) << (page_idx % 64));
}

static bool bitmap_test(size_t page_idx)  /* 1 = used */
{
    return !!(pmm.bitmap[page_idx / 64] & (UINT64_C(1) << (page_idx % 64)));
}

/* -----------------------------------------------------------------------
 * pmm_init
 * ----------------------------------------------------------------------- */
void pmm_init(const BootInfo *info)
{
    const BootMemDescriptor *map =
        (const BootMemDescriptor *)(PHYSMAP_BASE + info->mem_map);
    uint32_t count = info->mem_map_count;

    kprintf("[PMM] Memory map: %u entries\n", (uint32_t)count);

    /* 1. Find the highest physical address to size the bitmap */
    uintptr_t highest = 0;
    uint64_t  total_conventional = 0;

    for (uint32_t i = 0; i < count; i++) {
        const BootMemDescriptor *d = &map[i];
        uintptr_t end = d->physical_base + d->num_pages * PAGE_SIZE;
        if (end > highest) highest = end;

        if (d->type == BOOT_MEM_CONVENTIONAL) {
            total_conventional += d->num_pages * PAGE_SIZE;
        }

        /* Print each region */
        const char *type_names[] = {
            "RESERVED", "LOADER_CODE", "LOADER_DATA", "BOOT_SERVICES",
            "RUNTIME",  "CONVENTIONAL", "UNUSABLE",    "ACPI_RECLAIM",
            "ACPI_NVS", "MMIO",         "KERNEL",      "INITRD"
        };
        const char *type_name = (d->type < 12) ? type_names[d->type] : "UNKNOWN";
        kprintf("  [%02u] 0x%012lx - 0x%012lx  %s  (%lu pages)\n",
                i,
                d->physical_base,
                d->physical_base + d->num_pages * PAGE_SIZE,
                type_name,
                (unsigned long)d->num_pages);
    }

    pmm.highest_phys = highest;
    pmm.total_pages  = highest / PAGE_SIZE;
    pmm.bitmap_words = (pmm.total_pages + 63) / 64;
    size_t bitmap_size = pmm.bitmap_words * sizeof(uint64_t);

    kprintf("[PMM] Highest phys: 0x%lx (%zu pages)\n", highest, pmm.total_pages);
    kprintf("[PMM] Conventional RAM: %lu MiB\n",
            (unsigned long)(total_conventional / (1024 * 1024)));
    kprintf("[PMM] Bitmap: %zu KiB\n", bitmap_size / 1024);

    /* 2. Find a suitable physical region for the bitmap.
     *    We need bitmap_size bytes of conventional memory that doesn't
     *    overlap the kernel image. */
    uintptr_t bitmap_phys = 0;
    for (uint32_t i = 0; i < count; i++) {
        const BootMemDescriptor *d = &map[i];
        if (d->type != BOOT_MEM_CONVENTIONAL) continue;

        uintptr_t region_start = d->physical_base;
        uintptr_t region_end   = d->physical_base + d->num_pages * PAGE_SIZE;

        /* Skip the first 1 MiB (firmware/BIOS shadow) */
        if (region_start < 0x100000) region_start = 0x100000;
        if (region_start >= region_end) continue;

        /* Skip if it overlaps the kernel */
        uintptr_t kstart = info->kernel_physical_base;
        uintptr_t kend   = kstart + info->kernel_size;
        if (region_end <= kstart || region_start >= kend) {
            /* No overlap — check if there's room */
            if (region_end - region_start >= bitmap_size) {
                bitmap_phys = region_start;
                break;
            }
        }
    }

    if (!bitmap_phys) {
        kprintf("[PMM] FATAL: no memory for bitmap!\n");
        for (;;) {}
    }

    pmm.bitmap_phys = bitmap_phys;
    pmm.bitmap = (uint64_t *)(PHYSMAP_BASE + bitmap_phys);

    /* 3. Mark everything as USED initially */
    __builtin_memset(pmm.bitmap, 0xFF, bitmap_size);
    pmm.free_pages = 0;

    /* 4. Mark CONVENTIONAL regions as FREE */
    for (uint32_t i = 0; i < count; i++) {
        const BootMemDescriptor *d = &map[i];
        if (d->type == BOOT_MEM_CONVENTIONAL) {
            uintptr_t start = d->physical_base;
            uintptr_t end   = start + d->num_pages * PAGE_SIZE;
            for (uintptr_t p = start; p < end; p += PAGE_SIZE) {
                size_t idx = p / PAGE_SIZE;
                if (idx < pmm.total_pages && bitmap_test(idx)) {
                    bitmap_clear(idx);
                    pmm.free_pages++;
                }
            }
        }
    }

    /* 5. Re-mark the kernel image as USED */
    {
        uintptr_t kstart = info->kernel_physical_base;
        uintptr_t kend   = ALIGN_UP(kstart + info->kernel_size, PAGE_SIZE);
        for (uintptr_t p = kstart; p < kend; p += PAGE_SIZE) {
            size_t idx = p / PAGE_SIZE;
            if (idx < pmm.total_pages && !bitmap_test(idx)) {
                bitmap_set(idx);
                pmm.free_pages--;
            }
        }
        kprintf("[PMM] Kernel: phys 0x%lx - 0x%lx (%lu pages)\n",
                kstart, kend, (unsigned long)((kend - kstart) / PAGE_SIZE));
    }

    /* 6. Re-mark the bitmap itself as USED */
    {
        uintptr_t bstart = bitmap_phys;
        uintptr_t bend   = ALIGN_UP(bstart + bitmap_size, PAGE_SIZE);
        for (uintptr_t p = bstart; p < bend; p += PAGE_SIZE) {
            size_t idx = p / PAGE_SIZE;
            if (idx < pmm.total_pages && !bitmap_test(idx)) {
                bitmap_set(idx);
                pmm.free_pages--;
            }
        }
    }

    /* 7. Re-mark first 1 MiB as reserved (BIOS, SMM, VGA, etc.) */
    for (uintptr_t p = 0; p < 0x100000; p += PAGE_SIZE) {
        size_t idx = p / PAGE_SIZE;
        if (idx < pmm.total_pages && !bitmap_test(idx)) {
            bitmap_set(idx);
            pmm.free_pages--;
        }
    }

    kprintf("[PMM] Free: %zu MiB / %zu MiB\n",
            (pmm.free_pages * PAGE_SIZE) / (1024 * 1024),
            (pmm.total_pages * PAGE_SIZE) / (1024 * 1024));
}

/* -----------------------------------------------------------------------
 * pmm_alloc_page
 * ----------------------------------------------------------------------- */
uintptr_t pmm_alloc_page(void)
{
    lock_acquire(&pmm.lock);

    if (pmm.free_pages == 0) {
        lock_release(&pmm.lock);
        return 0;
    }

    /* Walk bitmap words looking for a word with a clear bit (free page) */
    for (size_t w = 0; w < pmm.bitmap_words; w++) {
        if (pmm.bitmap[w] == UINT64_C(0xFFFFFFFFFFFFFFFF))
            continue;  /* all used in this group */

        /* Find the lowest clear bit using compiler builtin */
        int bit = __builtin_ctzll(~pmm.bitmap[w]);
        size_t page_idx = w * 64 + (size_t)bit;

        if (page_idx >= pmm.total_pages) break;

        bitmap_set(page_idx);
        pmm.free_pages--;

        lock_release(&pmm.lock);
        return (uintptr_t)(page_idx * PAGE_SIZE);
    }

    lock_release(&pmm.lock);
    return 0;
}

/* -----------------------------------------------------------------------
 * pmm_alloc_pages — physically contiguous allocation
 * ----------------------------------------------------------------------- */
uintptr_t pmm_alloc_pages(size_t count)
{
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    lock_acquire(&pmm.lock);

    /* Linear scan for `count` consecutive free pages */
    size_t run = 0;
    size_t run_start = 0;

    for (size_t i = 0; i < pmm.total_pages; i++) {
        if (!bitmap_test(i)) {
            if (run == 0) run_start = i;
            run++;
            if (run == count) {
                /* Found a contiguous run — mark all as used */
                for (size_t j = run_start; j < run_start + count; j++) {
                    bitmap_set(j);
                    pmm.free_pages--;
                }
                lock_release(&pmm.lock);
                return (uintptr_t)(run_start * PAGE_SIZE);
            }
        } else {
            run = 0;
        }
    }

    lock_release(&pmm.lock);
    return 0;
}

/* -----------------------------------------------------------------------
 * pmm_free_page
 * ----------------------------------------------------------------------- */
void pmm_free_page(uintptr_t pa)
{
    if (!IS_ALIGNED(pa, PAGE_SIZE)) {
        kprintf("[PMM] BUG: free_page(0x%lx) not page-aligned!\n", pa);
        return;
    }

    size_t idx = pa / PAGE_SIZE;
    if (idx >= pmm.total_pages) {
        kprintf("[PMM] BUG: free_page(0x%lx) out of range!\n", pa);
        return;
    }

    lock_acquire(&pmm.lock);

    if (!bitmap_test(idx)) {
        kprintf("[PMM] BUG: double free of page 0x%lx!\n", pa);
        lock_release(&pmm.lock);
        return;
    }

    bitmap_clear(idx);
    pmm.free_pages++;
    lock_release(&pmm.lock);
}

/* -----------------------------------------------------------------------
 * pmm_free_pages
 * ----------------------------------------------------------------------- */
void pmm_free_pages(uintptr_t pa, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        pmm_free_page(pa + i * PAGE_SIZE);
    }
}

/* -----------------------------------------------------------------------
 * pmm_mark_used / pmm_mark_free
 * ----------------------------------------------------------------------- */
void pmm_mark_used(uintptr_t pa, size_t size)
{
    uintptr_t start = ALIGN_DOWN(pa, PAGE_SIZE);
    uintptr_t end   = ALIGN_UP(pa + size, PAGE_SIZE);

    lock_acquire(&pmm.lock);
    for (uintptr_t p = start; p < end; p += PAGE_SIZE) {
        size_t idx = p / PAGE_SIZE;
        if (idx < pmm.total_pages && !bitmap_test(idx)) {
            bitmap_set(idx);
            pmm.free_pages--;
        }
    }
    lock_release(&pmm.lock);
}

void pmm_mark_free(uintptr_t pa, size_t size)
{
    uintptr_t start = ALIGN_DOWN(pa, PAGE_SIZE);
    uintptr_t end   = ALIGN_UP(pa + size, PAGE_SIZE);

    lock_acquire(&pmm.lock);
    for (uintptr_t p = start; p < end; p += PAGE_SIZE) {
        size_t idx = p / PAGE_SIZE;
        if (idx < pmm.total_pages && bitmap_test(idx)) {
            bitmap_clear(idx);
            pmm.free_pages++;
        }
    }
    lock_release(&pmm.lock);
}

/* -----------------------------------------------------------------------
 * pmm_stats
 * ----------------------------------------------------------------------- */
void pmm_stats(uint64_t *total_out, uint64_t *free_out, uint64_t *used_out)
{
    lock_acquire(&pmm.lock);
    if (total_out) *total_out = (uint64_t)pmm.total_pages;
    if (free_out)  *free_out  = (uint64_t)pmm.free_pages;
    if (used_out)  *used_out  = (uint64_t)(pmm.total_pages - pmm.free_pages);
    lock_release(&pmm.lock);
}
