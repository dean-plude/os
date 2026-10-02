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

    /* Large allocation */
    return large_alloc(size);
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
