/*
 * paging.h — kernel-side virtual memory / page table management (x86_64)
 *
 * Phase 1 scope:
 *   - Take over the page tables the bootloader set up
 *   - Provide primitives to map/unmap 4KiB and 2MiB pages
 *   - Provide kernel_map() for mapping physical ranges into kernel VA space
 *
 * Phase 2 will add VAD trees, demand paging, and guard pages.
 *
 * Virtual address space layout (must match types.h):
 *
 *   0x0000000000000000 – 0x00007FFFFFFFFFFF   User space (128 TiB)
 *   -- 128 TiB hole (canonical address gap) --
 *   0xFFFF800000000000 – 0xFFFF87FFFFFFFFFF   Physmap (direct map, 64 GiB)
 *   0xFFFFFFFF80000000 – 0xFFFFFFFFFFFFFFFF   Kernel image (-2 GiB)
 *
 * Page table entry flags:
 *   Bit  0: Present
 *   Bit  1: Read/Write (1 = writable)
 *   Bit  2: User/Supervisor (1 = user accessible)
 *   Bit  3: Write-Through
 *   Bit  4: Cache Disable
 *   Bit  5: Accessed
 *   Bit  6: Dirty
 *   Bit  7: Page Size (in PD/PDPT = huge page)
 *   Bit  8: Global (TLB doesn't flush on CR3 write)
 *  Bits 51-12: Physical page number
 *  Bit 63: Execute Disable (NX) — requires EFER.NXE
 */

#pragma once

#include "../../include/types.h"

/* Page table entry flags */
#define PTE_PRESENT   UINT64_C(1 << 0)
#define PTE_WRITE     UINT64_C(1 << 1)
#define PTE_USER      UINT64_C(1 << 2)
#define PTE_WT        UINT64_C(1 << 3)
#define PTE_CD        UINT64_C(1 << 4)
#define PTE_ACCESSED  UINT64_C(1 << 5)
#define PTE_DIRTY     UINT64_C(1 << 6)
#define PTE_HUGE      UINT64_C(1 << 7)
#define PTE_GLOBAL    UINT64_C(1 << 8)
#define PTE_NX        UINT64_C(1ULL << 63)

/* Address mask (strips flag bits from PTE) */
#define PTE_ADDR_MASK UINT64_C(0x000FFFFFFFFFF000)

/* VA decomposition */
#define PML4_IDX(va) (((uintptr_t)(va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((uintptr_t)(va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((uintptr_t)(va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((uintptr_t)(va) >> 12) & 0x1FF)

/* A page table level (all four levels have the same structure) */
typedef uint64_t pte_t;
typedef pte_t    PageTable[512];

/* Map flags passed to kernel paging functions */
typedef uint32_t MapFlags;
#define MAP_WRITABLE    (1u << 0)  /* PTE_WRITE */
#define MAP_USER        (1u << 1)  /* PTE_USER */
#define MAP_NO_CACHE    (1u << 2)  /* PTE_CD */
#define MAP_NO_EXEC     (1u << 3)  /* PTE_NX */
#define MAP_HUGE        (1u << 4)  /* Use 2MiB pages */
#define MAP_GLOBAL      (1u << 5)  /* PTE_GLOBAL (kernel-only, survives CR3 flushes) */

/*
 * Initialize the kernel paging subsystem.
 * Takes ownership of the page tables set up by the bootloader.
 * Enables NX (No-Execute) via EFER.NXE.
 * Must be called after PMM is initialized.
 */
void paging_init(void);

/*
 * Return the physical address of the kernel's PML4.
 * Used by MmCreateProcessPageTable to clone the kernel mappings.
 */
uintptr_t paging_get_kernel_cr3(void);

/*
 * Create a new per-process PML4.
 * Copies all upper-half (kernel) PML4 entries from the kernel page table
 * so the new address space has the same kernel mappings.
 * Returns the physical address of the new PML4 (suitable for CR3).
 * Returns 0 on allocation failure.
 */
uintptr_t paging_create_process_pt(void);

/*
 * Map a single 4 KiB page into a specific process page table.
 *
 * @pt_phys — physical address of the target PML4 (from paging_create_process_pt)
 * @va      — virtual address to map (user-space, < 0x800000000000)
 * @pa      — physical address of the page to map
 * @flags   — combination of MAP_* flags (MAP_USER must be set)
 */
NTSTATUS paging_map_in_pt(uintptr_t pt_phys, uintptr_t va,
                            uintptr_t pa, MapFlags flags);

/*
 * Clone PML4 entries 0–255 (user lower half) from the kernel's PML4
 * into the target process page table.
 * Call after LdrLoadImage has mapped the user-space pages into the kernel PML4
 * so that the process page table reflects the same user-space mappings.
 *
 * @pt_phys — physical address of the target PML4 (from paging_create_process_pt)
 */
void paging_clone_user_mappings(uintptr_t pt_phys);

/*
 * Free all user-mode page table pages in a process PML4.
 * Only frees pages that cover the user address range (VA < PHYSMAP_BASE).
 * The PML4 page itself is also freed.
 * Does NOT free the mapped physical pages — caller must free them.
 */
void paging_destroy_process_pt(uintptr_t pt_phys);

/*
 * Load a page table into CR3 (switch address space).
 * @pt_phys — physical address of PML4 (0 = reload kernel PML4)
 */
static inline void paging_load_cr3(uintptr_t pt_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pt_phys) : "memory");
}

/*
 * Map a virtual address range to a physical address range.
 *
 * @va    — virtual start address (page-aligned)
 * @pa    — physical start address (page-aligned)
 * @size  — byte count (rounded up to page boundary)
 * @flags — combination of MAP_* flags
 *
 * Returns STATUS_SUCCESS or an error code.
 */
NTSTATUS paging_map(uintptr_t va, uintptr_t pa, size_t size, MapFlags flags);

/*
 * Unmap a virtual address range.
 * Does NOT free the physical pages — caller is responsible.
 *
 * @va   — virtual start address (page-aligned)
 * @size — byte count (rounded up)
 */
void paging_unmap(uintptr_t va, size_t size);

/*
 * Translate virtual address to physical.
 * Returns 0 if not mapped.
 */
uintptr_t paging_virt_to_phys(uintptr_t va);

/*
 * Convenience: map a physical range into the kernel's PHYSMAP region.
 * Returns the virtual address in the physmap window.
 */
static inline void *phys_to_virt_map(uintptr_t pa)
{
    return (void *)(PHYSMAP_BASE + pa);
}
