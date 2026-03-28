/*
 * paging.c — kernel virtual memory / page table management
 *
 * At this point the bootloader has given us:
 *   - A PML4 whose physical address is in CR3
 *   - Identity mapping for the first 4 GiB
 *   - A 64 GiB physmap at PHYSMAP_BASE
 *   - The kernel image mapped at KERNEL_VIRT_BASE
 *
 * We take ownership, enable NX, and provide the mapping API.
 * The PMM is the source of physical pages for new page table entries.
 */

#include "paging.h"
#include "cpu.h"
#include "../../mm/pmm.h"
#include "../../ke/printf.h"
#include "../../include/types.h"

#define PAGE_1GB (1024UL * 1024 * 1024)

/* Current kernel PML4 (virtual address, via physmap) */
static pte_t *kernel_pml4;

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Convert a physical PTE address to a virtual pointer via physmap */
static pte_t *pte_phys_to_virt(uintptr_t phys)
{
    return (pte_t *)(PHYSMAP_BASE + phys);
}

/* Allocate a zeroed page table page from the PMM */
static pte_t *alloc_table(void)
{
    uintptr_t pa = pmm_alloc_page();
    if (!pa) return NULL;
    pte_t *t = pte_phys_to_virt(pa);
    __builtin_memset(t, 0, PAGE_SIZE);
    return t;
}

/* Physical address of a page table from its virtual address */
static uintptr_t table_phys(pte_t *t)
{
    return (uintptr_t)t - PHYSMAP_BASE;
}

/* Get (or create) the next-level page table pointer from a PTE entry. */
static pte_t *get_or_create_table(pte_t *parent, int idx, bool create)
{
    pte_t entry = parent[idx];
    if (entry & PTE_PRESENT) {
        /* If the existing entry is a huge page, we can't descend into it */
        if (entry & PTE_HUGE) return NULL;
        return pte_phys_to_virt(entry & PTE_ADDR_MASK);
    }
    if (!create) return NULL;

    pte_t *child = alloc_table();
    if (!child) return NULL;

    parent[idx] = table_phys(child) | PTE_PRESENT | PTE_WRITE;
    return child;
}

/* Convert MapFlags to PTE flags */
static uint64_t map_flags_to_pte(MapFlags mf)
{
    uint64_t pte = PTE_PRESENT;
    if (mf & MAP_WRITABLE)  pte |= PTE_WRITE;
    if (mf & MAP_USER)      pte |= PTE_USER;
    if (mf & MAP_NO_CACHE)  pte |= PTE_CD;
    if (mf & MAP_NO_EXEC)   pte |= PTE_NX;
    if (mf & MAP_GLOBAL)    pte |= PTE_GLOBAL;
    return pte;
}

/* -----------------------------------------------------------------------
 * Map a single 4 KiB page
 * ----------------------------------------------------------------------- */
static NTSTATUS map_page_4k(uintptr_t va, uintptr_t pa, MapFlags flags)
{
    pte_t *pdpt = get_or_create_table(kernel_pml4, PML4_IDX(va), true);
    if (!pdpt) return STATUS_NO_MEMORY;

    pte_t *pd = get_or_create_table(pdpt, PDPT_IDX(va), true);
    if (!pd) return STATUS_NO_MEMORY;

    pte_t *pt = get_or_create_table(pd, PD_IDX(va), true);
    if (!pt) return STATUS_NO_MEMORY;

    pt[PT_IDX(va)] = (pa & PTE_ADDR_MASK) | map_flags_to_pte(flags);
    invlpg(va);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Map a single 2 MiB huge page
 * ----------------------------------------------------------------------- */
static NTSTATUS map_page_2mb(uintptr_t va, uintptr_t pa, MapFlags flags)
{
    pte_t *pdpt = get_or_create_table(kernel_pml4, PML4_IDX(va), true);
    if (!pdpt) return STATUS_NO_MEMORY;

    pte_t *pd = get_or_create_table(pdpt, PDPT_IDX(va), true);
    if (!pd) return STATUS_NO_MEMORY;

    pd[PD_IDX(va)] = (pa & ~(HUGE_PAGE_SIZE - 1)) | map_flags_to_pte(flags) | PTE_HUGE;
    invlpg(va);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * paging_map — public API
 * ----------------------------------------------------------------------- */
NTSTATUS paging_map(uintptr_t va, uintptr_t pa, size_t size, MapFlags flags)
{
    bool use_huge = !!(flags & MAP_HUGE);
    size_t page_size = use_huge ? HUGE_PAGE_SIZE : PAGE_SIZE;

    /* Align inputs */
    uintptr_t va_aligned = ALIGN_DOWN(va, page_size);
    uintptr_t pa_aligned = ALIGN_DOWN(pa, page_size);
    size_t    aligned_sz = ALIGN_UP(va + size, page_size) - va_aligned;

    for (size_t offset = 0; offset < aligned_sz; offset += page_size) {
        NTSTATUS s;
        if (use_huge) {
            s = map_page_2mb(va_aligned + offset, pa_aligned + offset, flags);
        } else {
            s = map_page_4k(va_aligned + offset, pa_aligned + offset, flags);
        }
        if (!NT_SUCCESS(s)) return s;
    }
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * paging_unmap
 * ----------------------------------------------------------------------- */
void paging_unmap(uintptr_t va, size_t size)
{
    size_t aligned_sz = ALIGN_UP(va + size, PAGE_SIZE) - ALIGN_DOWN(va, PAGE_SIZE);
    uintptr_t va_cur  = ALIGN_DOWN(va, PAGE_SIZE);

    for (size_t offset = 0; offset < aligned_sz; offset += PAGE_SIZE) {
        uintptr_t cur = va_cur + offset;

        pte_t *pdpt = get_or_create_table(kernel_pml4, PML4_IDX(cur), false);
        if (!pdpt) { cur += PAGE_SIZE; continue; }

        pte_t *pd = get_or_create_table(pdpt, PDPT_IDX(cur), false);
        if (!pd) { cur += PAGE_SIZE; continue; }

        pte_t *pt = get_or_create_table(pd, PD_IDX(cur), false);
        if (!pt) { cur += PAGE_SIZE; continue; }

        pt[PT_IDX(cur)] = 0;
        invlpg(cur);
    }
}

/* -----------------------------------------------------------------------
 * paging_virt_to_phys
 * ----------------------------------------------------------------------- */
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    pte_t *pdpt = get_or_create_table(kernel_pml4, PML4_IDX(va), false);
    if (!pdpt) return 0;

    pte_t pdpt_e = pdpt[PDPT_IDX(va)];
    if (!(pdpt_e & PTE_PRESENT)) return 0;
    if (pdpt_e & PTE_HUGE)  /* 1 GiB page */
        return (pdpt_e & PTE_ADDR_MASK) + (va & (PAGE_1GB - 1));

    pte_t *pd = pte_phys_to_virt(pdpt_e & PTE_ADDR_MASK);
    pte_t pd_e = pd[PD_IDX(va)];
    if (!(pd_e & PTE_PRESENT)) return 0;
    if (pd_e & PTE_HUGE)  /* 2 MiB page */
        return (pd_e & PTE_ADDR_MASK) + (va & (HUGE_PAGE_SIZE - 1));

    pte_t *pt = pte_phys_to_virt(pd_e & PTE_ADDR_MASK);
    pte_t pt_e = pt[PT_IDX(va)];
    if (!(pt_e & PTE_PRESENT)) return 0;
    return (pt_e & PTE_ADDR_MASK) + (va & (PAGE_SIZE - 1));
}

/* -----------------------------------------------------------------------
 * paging_init
 * ----------------------------------------------------------------------- */
void paging_init(void)
{
    /* Take ownership of the bootloader's PML4 via the physmap */
    uintptr_t cr3_phys = read_cr3() & PTE_ADDR_MASK;
    kernel_pml4 = pte_phys_to_virt(cr3_phys);

    kprintf("[PAGING] Kernel PML4 at phys=0x%lx, virt=%p\n",
            cr3_phys, (void *)kernel_pml4);

    /* Enable NX (No-Execute) via EFER.NXE */
    uint64_t efer = rdmsr(MSR_IA32_EFER);
    if (!(efer & EFER_NXE)) {
        wrmsr(MSR_IA32_EFER, efer | EFER_NXE);
        kprintf("[PAGING] NX enabled (EFER.NXE set)\n");
    } else {
        kprintf("[PAGING] NX already enabled\n");
    }

    /* Enable CR4.PGE (Page Global Enable) — kernel pages with PTE_GLOBAL
     * won't be flushed from TLB on CR3 switches (user context switches). */
    uint64_t cr4 = read_cr4();
    write_cr4(cr4 | CR4_PGE);
    kprintf("[PAGING] CR4.PGE enabled\n");

    /* Mark kernel pages as global by retroactively setting PTE_GLOBAL in
     * all PT entries covering the kernel image.
     * This is optional for Phase 1 but good practice. */
    /* TODO: walk kernel VA range and set PTE_GLOBAL */

    kprintf("[PAGING] Virtual memory initialized\n");
}
