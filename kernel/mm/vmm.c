/*
 * vmm.c — Kernel heap allocator (slab/zone allocator)
 *
 * Design:
 *
 * Small allocations (≤ 2048 bytes) use slab caches.  Each cache has a
 * fixed object size (power of two from 8 to 2048).  A slab is a single
 * 4 KiB page divided into equal-sized objects linked via a free list
 * embedded in the object itself (when free, the first 8 bytes hold a
 * next-pointer).
 *
 * Large allocations (> 2048 bytes) are served directly from the PMM and
 * use a small header page to record the allocation size so kfree() works.
 *
 * All allocations are done in the physmap VA window:
 *   va = PHYSMAP_BASE + physical_address
 *
 * Memory overhead:
 *   - One page per active slab (4 KiB minimum granule)
 *   - One header page per large allocation (4 KiB wasted per allocation)
 *
 * This is adequate for Phase 1.  A future phase will replace this with
 * a proper slab allocator (like Linux's SLUB or SLOB).
 */

#include "../ke/spinlock.h"
#include "vmm.h"
#include "pmm.h"
#include "../ke/printf.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"
#include "../ke/kpcr.h"
#include "../ke/smp.h"
#include "../arch/x86_64/paging.h"

/* -----------------------------------------------------------------------
 * Slab cache
 * ----------------------------------------------------------------------- */

#define SLAB_SIZES_COUNT  9
static const size_t slab_sizes[SLAB_SIZES_COUNT] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

/* A free object — when free, its first 8 bytes hold a pointer to the
 * next free object in the same slab page. */
typedef struct FreeObj { struct FreeObj *next; } FreeObj;

/* One slab: a single 4 KiB page subdivided into objects of a fixed size. */
typedef struct Slab {
    struct Slab *next;      /* Next slab in the cache's list */
    size_t       obj_size;  /* Object size for this slab */
    size_t       free_count;/* Number of free objects */
    FreeObj     *free_list; /* Head of free object list */
} Slab;

typedef struct {
    size_t  obj_size;
    Slab   *slabs;      /* The slabs with free objects (full ones are on no list) */
    KSpinLock lock;
} SlabCache;

static SlabCache caches[SLAB_SIZES_COUNT];

/* Each CPU keeps a few freed objects of each size for itself (a magazine):
 * most kmalloc/kfree pairs then touch no shared lock or line.  On once
 * CPU 0's KPCR is set (vmm_percpu_ready). */
#define MAG_SIZE 32
typedef struct {
    int   n;
    void *obj[MAG_SIZE];
} Magazine;
static Magazine g_mag[MAX_CPUS][SLAB_SIZES_COUNT];
static bool     g_mag_on;

void vmm_percpu_ready(void) { g_mag_on = true; }

static Magazine *mag_of(SlabCache *c)
{
    return &g_mag[KiGetCurrentKpcr()->CpuNumber % MAX_CPUS][c - caches];
}

/* Each cache's spinlock (interrupts off while held) */
static IrqState cache_lock(SlabCache *c)            { return spin_lock_irqsave(&c->lock); }
static void     cache_unlock(SlabCache *c, IrqState s) { spin_unlock_irqrestore(&c->lock, s); }

/* -----------------------------------------------------------------------
 * Slab creation
 * ----------------------------------------------------------------------- */

static Slab *slab_create(size_t obj_size)
{
    /* Allocate one physical page for the slab */
    uintptr_t pa = pmm_alloc_page();
    if (!pa) return NULL;

    /* The slab header lives at the START of the page.
     * Objects follow immediately after. */
    Slab *slab   = (Slab *)(PHYSMAP_BASE + pa);
    slab->next   = NULL;
    slab->obj_size  = obj_size;
    slab->free_list = NULL;
    slab->free_count = 0;

    /* Divide the rest of the page into objects and build the free list.
     * Object region starts after the Slab header (rounded up to obj_size). */
    uintptr_t obj_start = ALIGN_UP((uintptr_t)slab + sizeof(Slab), obj_size);
    uintptr_t page_end  = (uintptr_t)slab + PAGE_SIZE;

    /* Build free list in reverse order (simpler) */
    FreeObj *prev = NULL;
    for (uintptr_t addr = obj_start; addr + obj_size <= page_end; addr += obj_size) {
        FreeObj *obj = (FreeObj *)addr;
        obj->next    = prev;
        prev         = obj;
        slab->free_count++;
    }
    slab->free_list = prev;

    return slab;
}

/* -----------------------------------------------------------------------
 * Slab allocation / deallocation
 * ----------------------------------------------------------------------- */

static void *slab_alloc(SlabCache *c)
{
    if (g_mag_on) {
        IrqState is = irq_save();                   /* (stay on this CPU) */
        Magazine *m = mag_of(c);
        void *o = m->n ? m->obj[--m->n] : NULL;
        irq_restore(is);
        if (o) return o;
    }
    IrqState s = cache_lock(c);

    /* The first slab with free objects (all on the list have some) */
    Slab *slab = c->slabs;

    if (!slab) {
        /* No slab with free objects — create a new one */
        cache_unlock(c, s);
        slab = slab_create(c->obj_size);
        if (!slab) return NULL;
        s = cache_lock(c);
        slab->next = c->slabs;
        c->slabs   = slab;
    }

    /* Pop from free list */
    FreeObj *obj    = slab->free_list;
    slab->free_list = obj->next;
    if (--slab->free_count == 0) c->slabs = slab->next;    /* full: off the list until a free */

    cache_unlock(c, s);
    return (void *)obj;
}

static void slab_free(void *ptr, size_t obj_size)
{
    /* Find the slab this object belongs to (it's on the same page) */
    uintptr_t page_base = ALIGN_DOWN((uintptr_t)ptr, PAGE_SIZE);
    Slab     *slab      = (Slab *)page_base;

    /* Validate: slab's obj_size should match */
    if (slab->obj_size != obj_size) {
        kprintf("[VMM] BUG: slab_free(%p) size mismatch (slab=%zu, given=%zu)\n",
                ptr, slab->obj_size, obj_size);
        return;
    }

    /* Find the cache for this size */
    SlabCache *c = NULL;
    for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
        if (caches[i].obj_size == obj_size) { c = &caches[i]; break; }
    }
    if (!c) return;

    if (g_mag_on) {
        IrqState is = irq_save();
        Magazine *m = mag_of(c);
        bool kept = m->n < MAG_SIZE;
        if (kept) m->obj[m->n++] = ptr;
        irq_restore(is);
        if (kept) return;
    }
    IrqState s = cache_lock(c);
    FreeObj *obj    = (FreeObj *)ptr;
    obj->next       = slab->free_list;
    slab->free_list = obj;
    if (slab->free_count++ == 0) {                        /* was full: back on the list */
        slab->next = c->slabs;
        c->slabs   = slab;
    }
    cache_unlock(c, s);
}

/* -----------------------------------------------------------------------
 * Large allocation header
 *
 * For allocations > 2048 bytes, we allocate ceil(size/PAGE_SIZE) + 1 pages:
 *   - Page 0: LargeAllocHeader (metadata)
 *   - Pages 1..N: actual data
 * ----------------------------------------------------------------------- */

typedef struct {
    uint64_t magic;     /* sanity check */
    size_t   pages;     /* number of DATA pages (not counting header) */
} LargeAllocHeader;

#define LARGE_ALLOC_MAGIC UINT64_C(0x4C41524748445200)  /* "LARGHDR\0" */

static void *large_alloc(size_t size)
{
    size_t data_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t total_pages = 1 + data_pages;  /* +1 for header */

    uintptr_t pa = pmm_alloc_pages(total_pages);
    if (!pa) return NULL;

    LargeAllocHeader *hdr = (LargeAllocHeader *)(PHYSMAP_BASE + pa);
    hdr->magic = LARGE_ALLOC_MAGIC;
    hdr->pages = data_pages;

    return (void *)(PHYSMAP_BASE + pa + PAGE_SIZE);
}

static void large_free(void *ptr)
{
    /* Header is one page before the data */
    uintptr_t data_va  = (uintptr_t)ptr;
    uintptr_t hdr_va   = data_va - PAGE_SIZE;
    LargeAllocHeader *hdr = (LargeAllocHeader *)hdr_va;

    if (hdr->magic != LARGE_ALLOC_MAGIC) {
        kprintf("[VMM] BUG: large_free(%p): bad magic 0x%lx\n", ptr, hdr->magic);
        return;
    }

    size_t total_pages = 1 + hdr->pages;
    uintptr_t pa = hdr_va - PHYSMAP_BASE;

    /* Wipe the magic: otherwise a stale header left in a freed page could
     * make kfree() treat a slab object on the following page as a large
     * allocation and free the wrong pages. */
    hdr->magic = 0;
    pmm_free_pages(pa, total_pages);
}

/* -----------------------------------------------------------------------
 * Mapped blocks: large blocks out of pages from anywhere
 *
 * A large block from large_alloc is one physically contiguous run, which a
 * machine whose memory is in use in many small pieces may not have even
 * with hundreds of megabytes free (drive C:'s files are blocks like this:
 * Firefox's 164 MB xul.dll could not be unpacked with 735 MB free).  A
 * block of MAPPED_MIN or more, or a smaller one when no run is free, is
 * instead single pages mapped one after another into a window of kernel
 * addresses after the physmap and the MMIO window, in the top-level entry
 * every address space shares.  The window is in 2 MiB slots (one page
 * table each, so blocks never share one); a block takes whole slots, with
 * a header page first as large blocks have.
 *
 * Freed slots are stale until every CPU has flushed its TLB (other CPUs
 * may still hold their old translations), and are only reused after one
 * shootdown for all of them, when no fresh slots are left.
 * ----------------------------------------------------------------------- */

#define MAPPED_BASE   (PHYSMAP_BASE + (384ull << 30))
#define MAPPED_SIZE   (128ull << 30)
#define MAPPED_SLOT   HUGE_PAGE_SIZE
#define MAPPED_SLOTS  (MAPPED_SIZE / MAPPED_SLOT)
#define MAPPED_MIN    (4ull << 20)          /* blocks this large are always mapped */
#define MAPPED_MAGIC  UINT64_C(0x4D41505045444844)  /* "MAPPEDHD" */
#define SLOT_PAGES    (MAPPED_SLOT / PAGE_SIZE)

typedef struct {
    uint64_t magic;
    size_t   pages;     /* data pages mapped now */
    size_t   mapped;    /* data pages ever mapped (past pages: stale in other CPUs' TLBs) */
    size_t   slots;     /* slots taken, header included */
} MappedHeader;

static uint64_t  g_slot_used[MAPPED_SLOTS / 64];
static uint64_t  g_slot_stale[MAPPED_SLOTS / 64];
static size_t    g_slots_stale;
static size_t    g_slot_hint;               /* no free slot below this one */
static bool      g_mapped_ready;
static KSpinLock g_slot_lock = KSPINLOCK_INIT;

/* Every CPU's TLB, this one's too (smp_tlb_flush asks only the others) */
static void flush_everywhere(void)
{
    smp_tlb_flush(0);
    write_cr3(read_cr3());
}

static bool slot_busy(size_t i)
{
    return ((g_slot_used[i / 64] | g_slot_stale[i / 64]) >> (i % 64)) & 1;
}

static void slots_mark(uint64_t *map, size_t first, size_t n, bool on)
{
    for (size_t i = first; i < first + n; i++) {
        if (on) map[i / 64] |= UINT64_C(1) << (i % 64);
        else    map[i / 64] &= ~(UINT64_C(1) << (i % 64));
    }
}

/* @n slots in a row (the slot lock held), or MAPPED_SLOTS */
static size_t slots_find(size_t n)
{
    for (int pass = 0; pass < 2; pass++) {
        size_t run = 0;
        for (size_t i = g_slot_hint; i < MAPPED_SLOTS; i++) {
            if (!(i % 64) && (g_slot_used[i / 64] | g_slot_stale[i / 64]) == ~UINT64_C(0)) {
                run = 0;
                i += 63;
                continue;
            }
            run = slot_busy(i) ? 0 : run + 1;
            if (run == n) return i + 1 - n;
        }
        if (!g_slots_stale) break;
        flush_everywhere();                 /* the stale slots are clean everywhere now */
        __builtin_memset(g_slot_stale, 0, sizeof(g_slot_stale));
        g_slots_stale = 0;
        g_slot_hint = 0;
    }
    return MAPPED_SLOTS;
}

static void slots_free(size_t first, size_t n)
{
    IrqState s = spin_lock_irqsave(&g_slot_lock);
    slots_mark(g_slot_used, first, n, false);
    slots_mark(g_slot_stale, first, n, true);
    g_slots_stale += n;
    spin_unlock_irqrestore(&g_slot_lock, s);
}

/* Map fresh pages at [va, va + n pages); false (none left mapped) when
 * memory runs out */
static bool map_fresh(uintptr_t va, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uintptr_t pa = pmm_alloc_page();
        if (pa && NT_SUCCESS(paging_map(va + i * PAGE_SIZE, pa, PAGE_SIZE, MAP_WRITABLE | MAP_NO_EXEC)))
            continue;
        if (pa) pmm_free_page(pa);
        for (size_t k = 0; k < i; k++) {
            uintptr_t at = va + k * PAGE_SIZE;
            uintptr_t p = paging_virt_to_phys(at);
            paging_unmap(at, PAGE_SIZE);
            pmm_free_page(p);
        }
        return false;
    }
    return true;
}

static void unmap_pages(uintptr_t va, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uintptr_t at = va + i * PAGE_SIZE;
        uintptr_t p = paging_virt_to_phys(at);
        paging_unmap(at, PAGE_SIZE);
        if (p) pmm_free_page(p);
    }
}

static bool is_mapped(const void *ptr)
{
    return (uintptr_t)ptr >= MAPPED_BASE && (uintptr_t)ptr < MAPPED_BASE + MAPPED_SIZE;
}

static MappedHeader *mapped_header(const void *ptr)
{
    MappedHeader *h = (MappedHeader *)(ALIGN_DOWN((uintptr_t)ptr, MAPPED_SLOT));
    return h->magic == MAPPED_MAGIC ? h : NULL;
}

static void *mapped_alloc(size_t size)
{
    if (!g_mapped_ready) return NULL;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t slots = (pages + 1 + SLOT_PAGES - 1) / SLOT_PAGES;
    IrqState s = spin_lock_irqsave(&g_slot_lock);
    size_t first = slots_find(slots);
    if (first < MAPPED_SLOTS) {
        slots_mark(g_slot_used, first, slots, true);
        if (first == g_slot_hint) g_slot_hint = first + slots;
    }
    spin_unlock_irqrestore(&g_slot_lock, s);
    if (first >= MAPPED_SLOTS) return NULL;
    uintptr_t va = MAPPED_BASE + first * MAPPED_SLOT;
    if (!map_fresh(va, 1 + pages)) { slots_free(first, slots); return NULL; }
    MappedHeader *h = (MappedHeader *)va;
    h->magic = MAPPED_MAGIC;
    h->pages = h->mapped = pages;
    h->slots = slots;
    return (void *)(va + PAGE_SIZE);
}

static void mapped_free(void *ptr)
{
    MappedHeader *h = mapped_header(ptr);
    if (!h || (uintptr_t)ptr != (uintptr_t)h + PAGE_SIZE) {
        kprintf("[VMM] BUG: kfree(%p): not a mapped block\n", ptr);
        return;
    }
    size_t pages = h->pages, slots = h->slots;
    uintptr_t va = (uintptr_t)h;
    h->magic = 0;
    unmap_pages(va, 1 + pages);
    slots_free((va - MAPPED_BASE) / MAPPED_SLOT, slots);
}

static bool mapped_resize(void *ptr, size_t size)
{
    MappedHeader *h = mapped_header(ptr);
    if (!h || !size) return false;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t data = (uintptr_t)h + PAGE_SIZE;
    if (pages < h->pages) {
        unmap_pages(data + pages * PAGE_SIZE, h->pages - pages);
        h->pages = pages;
        return true;
    }
    if (pages == h->pages) return true;
    size_t slots = (pages + 1 + SLOT_PAGES - 1) / SLOT_PAGES;
    size_t first = ((uintptr_t)h - MAPPED_BASE) / MAPPED_SLOT;
    if (slots > h->slots) {                  /* the slots after it, when free */
        bool ok = first + slots <= MAPPED_SLOTS;
        IrqState s = spin_lock_irqsave(&g_slot_lock);
        for (size_t i = first + h->slots; ok && i < first + slots; i++) ok = !slot_busy(i);
        if (ok) slots_mark(g_slot_used, first + h->slots, slots - h->slots, true);
        spin_unlock_irqrestore(&g_slot_lock, s);
        if (!ok) return false;
        h->slots = slots;
    }
    if (h->pages < h->mapped) flush_everywhere();   /* (pages it gave back: old translations) */
    if (!map_fresh(data + h->pages * PAGE_SIZE, pages - h->pages)) return false;
    h->pages = pages;
    if (pages > h->mapped) h->mapped = pages;
    return true;
}

static LargeAllocHeader *large_header(const void *ptr)
{
    uintptr_t page_before = ALIGN_DOWN((uintptr_t)ptr, PAGE_SIZE) - PAGE_SIZE;
    if (page_before < PHYSMAP_BASE) return NULL;
    LargeAllocHeader *hdr = (LargeAllocHeader *)page_before;
    return hdr->magic == LARGE_ALLOC_MAGIC ? hdr : NULL;
}

size_t ksize(const void *ptr)
{
    if (!ptr) return 0;
    if (is_mapped(ptr)) {
        MappedHeader *m = mapped_header(ptr);
        return m ? m->pages * PAGE_SIZE : 0;
    }
    LargeAllocHeader *hdr = large_header(ptr);
    if (hdr) return hdr->pages * PAGE_SIZE;
    return ((Slab *)ALIGN_DOWN((uintptr_t)ptr, PAGE_SIZE))->obj_size;
}

bool kresize(void *ptr, size_t size)
{
    if (is_mapped(ptr)) return mapped_resize(ptr, size);
    LargeAllocHeader *hdr = ptr ? large_header(ptr) : NULL;
    if (!hdr || !size) return false;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t end = (uintptr_t)ptr - PHYSMAP_BASE + hdr->pages * PAGE_SIZE;
    if (pages < hdr->pages) {
        pmm_free_pages(end - (hdr->pages - pages) * PAGE_SIZE, hdr->pages - pages);
    } else if (pages > hdr->pages) {
        if (!pmm_claim_pages(end, pages - hdr->pages)) return false;
    }
    hdr->pages = pages;
    return true;
}

/* -----------------------------------------------------------------------
 * vmm_init
 * ----------------------------------------------------------------------- */
void vmm_init(void)
{
    for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
        caches[i].obj_size   = slab_sizes[i];
        caches[i].slabs      = NULL;
        caches[i].lock       = (KSpinLock)KSPINLOCK_INIT;
    }
    kprintf("[VMM] Slab caches initialized: %d size classes (8B–2KB)\n",
            SLAB_SIZES_COUNT);
    g_mapped_ready = NT_SUCCESS(paging_prepare(MAPPED_BASE, MAPPED_SIZE));
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;

    /* Find the smallest slab size that fits */
    for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
        if (size <= slab_sizes[i]) {
            return slab_alloc(&caches[i]);
        }
    }

    /* Large allocation: one run of pages, or mapped pages from anywhere
     * (always for big blocks, which would use up the long runs) */
    void *p = size < MAPPED_MIN ? large_alloc(size) : NULL;
    return p ? p : mapped_alloc(size);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) __builtin_memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    if (is_mapped(ptr)) { mapped_free(ptr); return; }

    /* Check if this is a large allocation by reading the header page.
     * For slab objects the "header" is the slab descriptor, which has
     * a different magic pattern. We distinguish by checking the magic. */
    uintptr_t page_before = ALIGN_DOWN((uintptr_t)ptr, PAGE_SIZE) - PAGE_SIZE;
    /* Only check if page_before is in physmap range */
    if (page_before >= PHYSMAP_BASE) {
        LargeAllocHeader *hdr = (LargeAllocHeader *)page_before;
        if (hdr->magic == LARGE_ALLOC_MAGIC) {
            large_free(ptr);
            return;
        }
    }

    /* Slab object — find the object size from the slab header */
    uintptr_t page_base = ALIGN_DOWN((uintptr_t)ptr, PAGE_SIZE);
    Slab *slab = (Slab *)page_base;
    slab_free(ptr, slab->obj_size);
}

void *kernel_alloc_pages(size_t count)
{
    uintptr_t pa = pmm_alloc_pages(count);
    if (!pa) return NULL;
    return (void *)(PHYSMAP_BASE + pa);
}

void kernel_free_pages(void *ptr, size_t count)
{
    uintptr_t pa = (uintptr_t)ptr - PHYSMAP_BASE;
    pmm_free_pages(pa, count);
}
