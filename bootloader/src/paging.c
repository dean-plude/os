/*
 * paging.c — UEFI bootloader page table construction
 *
 * We build the initial set of x86_64 4-level page tables here.
 * After ExitBootServices we load CR3 with these tables; the kernel
 * then replaces them with its own once it's initialized VMM.
 *
 * Page table entry flags:
 *   Bit 0: Present
 *   Bit 1: Read/Write
 *   Bit 2: User/Supervisor (0 = supervisor only)
 *   Bit 7: Page Size (1 = huge page — 2MiB in PD, 1GiB in PDPT)
 *
 * We use:
 *   1 GiB pages (PS bit in PDPT entries) for the physical map
 *   2 MiB pages (PS bit in PD entries) for the identity map
 *   4 KiB pages (PT level) for the kernel itself
 *
 * All tables are allocated as EfiLoaderData pages.
 * Helper: one 4096-byte page holds 512 entries × 8 bytes.
 */

#include "paging.h"

extern void console_printf(const char *fmt, ...);

#define PAGE_PRESENT     UINT64_C(1 << 0)
#define PAGE_WRITE       UINT64_C(1 << 1)
#define PAGE_HUGE        UINT64_C(1 << 7)
#define PAGE_MASK        (~(UINT64_C(0xFFF)))   /* Strip flags from phys addr */

#define ENTRIES_PER_TABLE 512
#define PAGE_SIZE         4096ULL
#define PAGE_2MB          (2ULL * 1024 * 1024)
#define PAGE_1GB          (1ULL * 1024 * 1024 * 1024)

/* Virtual address bit field extraction */
#define PML4_IDX(va) (((va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((va) >> 12) & 0x1FF)

typedef UINT64 pte_t;

/* Allocate a zeroed 4096-byte page table. */
static EFI_STATUS alloc_table(EFI_BOOT_SERVICES *bs, UINT64 *phys_out)
{
    UINT64     addr  = 0;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &addr);
    if (EFI_ERROR(status)) return status;

    /* Zero the table */
    pte_t *p = (pte_t *)(UINTN)addr;
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) p[i] = 0;

    *phys_out = addr;
    return EFI_SUCCESS;
}

/* Obtain or create a sub-table at entries[idx], return its physical address. */
static EFI_STATUS get_or_create(EFI_BOOT_SERVICES *bs,
                                 pte_t *entries, int idx,
                                 UINT64 *sub_phys_out)
{
    if (entries[idx] & PAGE_PRESENT) {
        *sub_phys_out = entries[idx] & PAGE_MASK;
        return EFI_SUCCESS;
    }
    UINT64     sub_phys = 0;
    EFI_STATUS s        = alloc_table(bs, &sub_phys);
    if (EFI_ERROR(s)) return s;
    entries[idx] = sub_phys | PAGE_PRESENT | PAGE_WRITE;
    *sub_phys_out = sub_phys;
    return EFI_SUCCESS;
}

/* Map a single 4KiB virtual page → physical page. */
static EFI_STATUS map_4k(EFI_BOOT_SERVICES *bs,
                          pte_t *pml4,
                          UINT64 va, UINT64 pa, UINT64 flags)
{
    EFI_STATUS s;
    UINT64 pdpt_phys, pd_phys, pt_phys;

    s = get_or_create(bs, pml4, PML4_IDX(va), &pdpt_phys);
    if (EFI_ERROR(s)) return s;
    pte_t *pdpt = (pte_t *)(UINTN)pdpt_phys;

    s = get_or_create(bs, pdpt, PDPT_IDX(va), &pd_phys);
    if (EFI_ERROR(s)) return s;
    pte_t *pd = (pte_t *)(UINTN)pd_phys;

    s = get_or_create(bs, pd, PD_IDX(va), &pt_phys);
    if (EFI_ERROR(s)) return s;
    pte_t *pt = (pte_t *)(UINTN)pt_phys;

    if (pt[PT_IDX(va)] & PAGE_PRESENT) {
        /* Already mapped — overwrite silently */
    }
    pt[PT_IDX(va)] = (pa & PAGE_MASK) | flags | PAGE_PRESENT;
    return EFI_SUCCESS;
}

/* Map a range using 2MiB huge pages in a Page Directory.
 * The PD entry is set directly with PAGE_HUGE. */
static EFI_STATUS map_2mb_range(EFI_BOOT_SERVICES *bs,
                                 pte_t *pml4,
                                 UINT64 va_start, UINT64 pa_start,
                                 UINT64 page_count_2mb)
{
    EFI_STATUS s;
    UINT64 pdpt_phys, pd_phys;

    for (UINT64 i = 0; i < page_count_2mb; i++) {
        UINT64 va = va_start + i * PAGE_2MB;
        UINT64 pa = pa_start + i * PAGE_2MB;

        s = get_or_create(bs, pml4, PML4_IDX(va), &pdpt_phys);
        if (EFI_ERROR(s)) return s;
        pte_t *pdpt = (pte_t *)(UINTN)pdpt_phys;

        s = get_or_create(bs, pdpt, PDPT_IDX(va), &pd_phys);
        if (EFI_ERROR(s)) return s;
        pte_t *pd = (pte_t *)(UINTN)pd_phys;

        pd[PD_IDX(va)] = (pa & ~(PAGE_2MB - 1)) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
    }
    return EFI_SUCCESS;
}

/* Map a range using 1 GiB huge pages in a PDPT entry. */
static EFI_STATUS map_1gb_range(EFI_BOOT_SERVICES *bs,
                                 pte_t *pml4,
                                 UINT64 va_start, UINT64 pa_start,
                                 UINT64 page_count_1gb)
{
    EFI_STATUS s;
    UINT64 pdpt_phys;

    for (UINT64 i = 0; i < page_count_1gb; i++) {
        UINT64 va = va_start + i * PAGE_1GB;
        UINT64 pa = pa_start + i * PAGE_1GB;

        s = get_or_create(bs, pml4, PML4_IDX(va), &pdpt_phys);
        if (EFI_ERROR(s)) return s;
        pte_t *pdpt = (pte_t *)(UINTN)pdpt_phys;

        pdpt[PDPT_IDX(va)] = (pa & ~(PAGE_1GB - 1)) | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
    }
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------
 * paging_build — public API
 * ------------------------------------------------------------------ */
EFI_STATUS paging_build(
    UINT64             kernel_phys_base,
    UINT64             kernel_virt_base,
    UINT64             kernel_size,
    UINT64            *cr3_out,
    EFI_BOOT_SERVICES *bs)
{
    EFI_STATUS s;
    UINT64     pml4_phys = 0;

    /* Allocate and zero the PML4. */
    s = alloc_table(bs, &pml4_phys);
    if (EFI_ERROR(s)) {
        console_printf("PAGING: failed to allocate PML4: %x\r\n", (UINT64)s);
        return s;
    }
    pte_t *pml4 = (pte_t *)(UINTN)pml4_phys;
    console_printf("PAGING: PML4 at phys 0x%x\r\n", pml4_phys);

    /* 1. Identity map [0 .. 4GiB) using 2MiB pages. */
    UINT64 identity_pages_2mb = (4ULL * 1024 * 1024 * 1024) / PAGE_2MB;  /* 2048 */
    s = map_2mb_range(bs, pml4, 0, 0, identity_pages_2mb);
    if (EFI_ERROR(s)) {
        console_printf("PAGING: identity map failed: %x\r\n", (UINT64)s);
        return s;
    }
    console_printf("PAGING: identity mapped first 4 GiB\r\n");

    /* 2. Physical map [0 .. PHYSMAP_GIB GiB) at PHYSMAP_BASE using 1 GiB pages. */
    s = map_1gb_range(bs, pml4, PHYSMAP_BASE, 0, PHYSMAP_GIB);
    if (EFI_ERROR(s)) {
        console_printf("PAGING: physmap failed: %x\r\n", (UINT64)s);
        return s;
    }
    console_printf("PAGING: physmap %d GiB at 0x%x\r\n",
                   (UINT64)PHYSMAP_GIB, PHYSMAP_BASE);

    /* 3. Map kernel pages (4 KiB granularity for precise mapping). */
    UINT64 num_kernel_pages = (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (UINT64 i = 0; i < num_kernel_pages; i++) {
        UINT64 pa = kernel_phys_base + i * PAGE_SIZE;
        UINT64 va = kernel_virt_base + i * PAGE_SIZE;
        s = map_4k(bs, pml4, va, pa, PAGE_WRITE);
        if (EFI_ERROR(s)) {
            console_printf("PAGING: kernel map failed at va=0x%x: %x\r\n",
                           va, (UINT64)s);
            return s;
        }
    }
    console_printf("PAGING: kernel mapped %u pages virt=0x%x -> phys=0x%x\r\n",
                   num_kernel_pages, kernel_virt_base, kernel_phys_base);

    *cr3_out = pml4_phys;
    return EFI_SUCCESS;
}
