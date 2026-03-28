/*
 * vma.c — Virtual Memory Area (VAD) manager
 *
 * Tracks the user-mode virtual address space of a process as a sorted
 * linked list of VMA_ENTRY nodes.
 *
 * All VA range arithmetic is done in bytes with page-aligned values.
 * The user address space runs from USER_ADDRESS_MIN to USER_ADDRESS_MAX
 * (64 KB → 127.5 TB on x86_64).
 *
 * NtAllocateVirtualMemory flow:
 *   - If BaseAddress == 0: scan for a free hole ≥ RegionSize (bottom-up,
 *     starting at USER_ADDRESS_MIN; MEM_TOP_DOWN starts at USER_ADDRESS_MAX).
 *   - If BaseAddress != 0: check the range is completely free.
 *   - Allocate physical pages (PMM) if MEM_COMMIT.
 *   - Map the pages in the process page tables with appropriate permissions.
 *   - Insert a VMA_ENTRY.
 *
 * Phase 3 simplification: we use the kernel's own page tables for user
 * allocations.  Phase 4 will give each process its own CR3.
 */

#include "vma.h"
#include "vmm.h"
#include "pmm.h"
#include "../ke/printf.h"
#include "../include/types.h"
#include "../arch/x86_64/paging.h"

/* -----------------------------------------------------------------------
 * Lock helpers
 * ----------------------------------------------------------------------- */
static void vma_lock(PVMA_SPACE s)
{
    UINT32 t = __atomic_fetch_add(&s->LockNext, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s->LockOwner, __ATOMIC_ACQUIRE) != t)
        __asm__ volatile("pause");
}
static void vma_unlock(PVMA_SPACE s)
{
    __atomic_fetch_add(&s->LockOwner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * PAGE_* → paging MapFlags translation
 * ----------------------------------------------------------------------- */
static uint32_t vma_prot_to_map_flags(UINT32 protect)
{
    uint32_t flags = MAP_USER;
    if (protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        flags |= MAP_WRITABLE;
    if (!(protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        flags |= MAP_NO_EXEC;
    if (protect & PAGE_NOCACHE)
        flags |= MAP_NO_CACHE;
    return flags;
}

/* -----------------------------------------------------------------------
 * Alignment helpers
 * ----------------------------------------------------------------------- */
#define ALLOC_GRAN  0x10000ULL  /* 64 KB allocation granularity (Windows) */

static UINT64 align_up_gran(UINT64 v)   { return (v + ALLOC_GRAN - 1) & ~(ALLOC_GRAN - 1); }
static UINT64 align_up_page(UINT64 v)   { return (v + PAGE_SIZE - 1) & ~((UINT64)PAGE_SIZE - 1); }

/* -----------------------------------------------------------------------
 * VmaInitSpace
 * ----------------------------------------------------------------------- */
void VmaInitSpace(PVMA_SPACE Space)
{
    __builtin_memset(Space, 0, sizeof(VMA_SPACE));
}

/* -----------------------------------------------------------------------
 * VmaDestroySpace — free all VMA entries (and back physical pages)
 * ----------------------------------------------------------------------- */
void VmaDestroySpace(PVMA_SPACE Space)
{
    PVMA_ENTRY e = Space->Head;
    while (e) {
        PVMA_ENTRY next = e->Next;
        if (e->State & MEM_COMMIT && e->Type == VMA_TYPE_PRIVATE) {
            /* Unmap and free the physical pages */
            UINT64 pages = e->RegionSize / PAGE_SIZE;
            for (UINT64 i = 0; i < pages; i++) {
                UINT64 va = e->BaseAddress + i * PAGE_SIZE;
                PADDR phys = paging_virt_to_phys((VADDR)va);
                if (phys) {
                    paging_unmap((VADDR)va);
                    pmm_free_page(phys);
                }
            }
        }
        kfree(e);
        e = next;
    }
    Space->Head  = NULL;
    Space->Count = 0;
}

/* -----------------------------------------------------------------------
 * VmaFind — find the entry containing Address (NULL if free region)
 * ----------------------------------------------------------------------- */
PVMA_ENTRY VmaFind(PVMA_SPACE Space, UINT64 Address)
{
    PVMA_ENTRY e = Space->Head;
    while (e) {
        if (Address >= e->BaseAddress && Address < e->BaseAddress + e->RegionSize)
            return e;
        if (e->BaseAddress > Address) break;  /* sorted: no later entry can match */
        e = e->Next;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * vma_find_free_range — find a free hole of at least 'size' bytes.
 * Returns the base VA, or 0 on failure.
 * ----------------------------------------------------------------------- */
static UINT64 vma_find_free_range(PVMA_SPACE Space, UINT64 size,
                                   bool top_down)
{
    if (!top_down) {
        UINT64 candidate = align_up_gran(USER_ADDRESS_MIN);
        PVMA_ENTRY e = Space->Head;
        while (candidate + size <= USER_ADDRESS_MAX) {
            /* Check if [candidate, candidate+size) is free */
            bool overlap = false;
            PVMA_ENTRY scan = e;
            while (scan) {
                UINT64 eend = scan->BaseAddress + scan->RegionSize;
                if (candidate < eend && candidate + size > scan->BaseAddress) {
                    /* Overlaps — advance candidate past this entry */
                    candidate = align_up_gran(eend);
                    overlap = true;
                    e = scan->Next ? scan->Next : scan;
                    break;
                }
                if (scan->BaseAddress >= candidate + size) break;
                scan = scan->Next;
            }
            if (!overlap) return candidate;
        }
        return 0;  /* Out of space */
    } else {
        /* Top-down: start from max and work down */
        UINT64 candidate = (USER_ADDRESS_MAX - size) & ~(ALLOC_GRAN - 1);
        while (candidate >= USER_ADDRESS_MIN) {
            bool overlap = false;
            PVMA_ENTRY scan = Space->Head;
            while (scan) {
                UINT64 eend = scan->BaseAddress + scan->RegionSize;
                if (candidate < eend && candidate + size > scan->BaseAddress) {
                    /* Overlaps: go below this entry */
                    if (scan->BaseAddress < size) { candidate = 0; break; }
                    candidate = (scan->BaseAddress - size) & ~(ALLOC_GRAN - 1);
                    overlap = true;
                    break;
                }
                scan = scan->Next;
            }
            if (!overlap) return candidate;
            if (candidate == 0) break;
        }
        return 0;
    }
}

/* -----------------------------------------------------------------------
 * vma_insert — insert a new entry into the sorted list (lock must be held)
 * ----------------------------------------------------------------------- */
static void vma_insert(PVMA_SPACE Space, PVMA_ENTRY New)
{
    PVMA_ENTRY *pp = &Space->Head;
    while (*pp && (*pp)->BaseAddress < New->BaseAddress)
        pp = &(*pp)->Next;
    New->Next = *pp;
    *pp = New;
    Space->Count++;
}

/* -----------------------------------------------------------------------
 * vma_commit_pages — allocate and map physical pages for a region
 * ----------------------------------------------------------------------- */
static NTSTATUS vma_commit_pages(UINT64 base, UINT64 size, UINT32 protect)
{
    uint32_t flags = vma_prot_to_map_flags(protect);
    UINT64 pages = size / PAGE_SIZE;

    for (UINT64 i = 0; i < pages; i++) {
        PADDR phys = pmm_alloc_page();
        if (!phys) {
            /* Roll back already-mapped pages */
            for (UINT64 j = 0; j < i; j++) {
                UINT64 va = base + j * PAGE_SIZE;
                PADDR p   = paging_virt_to_phys((VADDR)va);
                if (p) { paging_unmap((VADDR)va); pmm_free_page(p); }
            }
            return STATUS_NO_MEMORY;
        }
        /* Zero the page */
        void *kva = (void *)(PHYSMAP_BASE + phys);
        __builtin_memset(kva, 0, PAGE_SIZE);
        /* Map in user VA space */
        NTSTATUS s = paging_map((VADDR)(base + i * PAGE_SIZE), phys, flags);
        if (!NT_SUCCESS(s)) {
            pmm_free_page(phys);
            for (UINT64 j = 0; j < i; j++) {
                UINT64 va = base + j * PAGE_SIZE;
                PADDR p   = paging_virt_to_phys((VADDR)va);
                if (p) { paging_unmap((VADDR)va); pmm_free_page(p); }
            }
            return s;
        }
    }
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VmaAllocate
 * ----------------------------------------------------------------------- */
NTSTATUS VmaAllocate(PVMA_SPACE  Space,
                      UINT64     *BaseAddress,
                      UINT64     *RegionSize,
                      UINT32      AllocType,
                      UINT32      Protect)
{
    if (!BaseAddress || !RegionSize || !*RegionSize)
        return STATUS_INVALID_PARAMETER;

    if (!(AllocType & (MEM_COMMIT | MEM_RESERVE)))
        return STATUS_INVALID_PARAMETER;

    UINT64 size = align_up_page(*RegionSize);
    bool   top_down = (AllocType & MEM_TOP_DOWN) != 0;

    vma_lock(Space);

    UINT64 base;
    if (*BaseAddress) {
        /* Caller specified a base — align to granularity */
        base = *BaseAddress & ~(ALLOC_GRAN - 1);
        /* Verify the range is free */
        UINT64 end = base + size;
        PVMA_ENTRY scan = Space->Head;
        while (scan) {
            if (scan->BaseAddress < end && scan->BaseAddress + scan->RegionSize > base) {
                vma_unlock(Space);
                return STATUS_CONFLICTING_ADDRESSES;
            }
            scan = scan->Next;
        }
    } else {
        base = vma_find_free_range(Space, size, top_down);
        if (!base) {
            vma_unlock(Space);
            return STATUS_NO_MEMORY;
        }
    }

    /* Validate range is in user space */
    if (base < USER_ADDRESS_MIN || base + size > USER_ADDRESS_MAX) {
        vma_unlock(Space);
        return STATUS_INVALID_PARAMETER;
    }

    /* Commit physical pages if MEM_COMMIT */
    NTSTATUS s = STATUS_SUCCESS;
    if (AllocType & MEM_COMMIT) {
        s = vma_commit_pages(base, size, Protect);
        if (!NT_SUCCESS(s)) { vma_unlock(Space); return s; }
    }

    /* Create the VMA entry */
    PVMA_ENTRY e = kzalloc(sizeof(VMA_ENTRY));
    if (!e) {
        if (AllocType & MEM_COMMIT) {
            /* Unmap already-committed pages */
            for (UINT64 i = 0; i < size / PAGE_SIZE; i++) {
                UINT64 va = base + i * PAGE_SIZE;
                PADDR p   = paging_virt_to_phys((VADDR)va);
                if (p) { paging_unmap((VADDR)va); pmm_free_page(p); }
            }
        }
        vma_unlock(Space);
        return STATUS_NO_MEMORY;
    }

    e->BaseAddress = base;
    e->RegionSize  = size;
    e->State       = (AllocType & MEM_COMMIT) ? MEM_COMMIT : MEM_RESERVE;
    e->Protect     = Protect;
    e->Type        = VMA_TYPE_PRIVATE;

    vma_insert(Space, e);
    vma_unlock(Space);

    *BaseAddress = base;
    *RegionSize  = size;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VmaFree
 * ----------------------------------------------------------------------- */
NTSTATUS VmaFree(PVMA_SPACE  Space,
                  UINT64     *BaseAddress,
                  UINT64     *RegionSize,
                  UINT32      FreeType)
{
    if (!BaseAddress) return STATUS_INVALID_PARAMETER;

    UINT64 base = *BaseAddress;
    UINT64 size = RegionSize ? *RegionSize : 0;

    if (FreeType == MEM_RELEASE) size = 0;  /* release whole region */

    vma_lock(Space);

    PVMA_ENTRY *pp = &Space->Head;
    while (*pp) {
        PVMA_ENTRY e = *pp;
        if (e->BaseAddress == base ||
            (base >= e->BaseAddress && base < e->BaseAddress + e->RegionSize))
        {
            UINT64 release_size = size ? size : e->RegionSize;
            release_size = align_up_page(release_size);

            if (FreeType == MEM_RELEASE) {
                /* Unmap all physical pages and remove the entry */
                if (e->State & MEM_COMMIT && e->Type == VMA_TYPE_PRIVATE) {
                    for (UINT64 i = 0; i < e->RegionSize / PAGE_SIZE; i++) {
                        UINT64 va = e->BaseAddress + i * PAGE_SIZE;
                        PADDR p   = paging_virt_to_phys((VADDR)va);
                        if (p) { paging_unmap((VADDR)va); pmm_free_page(p); }
                    }
                }
                *pp = e->Next;
                Space->Count--;
                kfree(e);
            } else {
                /* MEM_DECOMMIT: unmap pages but keep reservation */
                if (e->State & MEM_COMMIT) {
                    UINT64 off   = base - e->BaseAddress;
                    UINT64 pages = release_size / PAGE_SIZE;
                    for (UINT64 i = 0; i < pages; i++) {
                        UINT64 va = e->BaseAddress + off + i * PAGE_SIZE;
                        PADDR p   = paging_virt_to_phys((VADDR)va);
                        if (p) { paging_unmap((VADDR)va); pmm_free_page(p); }
                    }
                    if (release_size >= e->RegionSize)
                        e->State = MEM_RESERVE;
                }
            }

            vma_unlock(Space);
            if (RegionSize) *RegionSize = release_size;
            return STATUS_SUCCESS;
        }
        pp = &(*pp)->Next;
    }

    vma_unlock(Space);
    return STATUS_MEMORY_NOT_ALLOCATED;
}

/* -----------------------------------------------------------------------
 * VmaProtect
 * ----------------------------------------------------------------------- */
NTSTATUS VmaProtect(PVMA_SPACE  Space,
                     UINT64     *BaseAddress,
                     UINT64     *RegionSize,
                     UINT32      NewProtect,
                     UINT32     *OldProtect)
{
    if (!BaseAddress || !RegionSize) return STATUS_INVALID_PARAMETER;

    UINT64 base = *BaseAddress & ~((UINT64)PAGE_SIZE - 1);
    UINT64 size = align_up_page(*RegionSize);

    vma_lock(Space);
    PVMA_ENTRY e = VmaFind(Space, base);
    if (!e || !(e->State & MEM_COMMIT)) {
        vma_unlock(Space);
        return STATUS_MEMORY_NOT_ALLOCATED;
    }

    if (OldProtect) *OldProtect = e->Protect;
    e->Protect = NewProtect;

    /* Re-map pages with new protection */
    uint32_t flags = vma_prot_to_map_flags(NewProtect);
    UINT64 pages = size / PAGE_SIZE;
    for (UINT64 i = 0; i < pages; i++) {
        UINT64 va   = base + i * PAGE_SIZE;
        PADDR  phys = paging_virt_to_phys((VADDR)va);
        if (phys) {
            paging_unmap((VADDR)va);
            paging_map((VADDR)va, phys, flags);
        }
    }

    vma_unlock(Space);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VmaQuery
 * ----------------------------------------------------------------------- */
NTSTATUS VmaQuery(PVMA_SPACE               Space,
                   UINT64                   Address,
                   MEMORY_BASIC_INFORMATION *Info,
                   UINT64                  *ReturnLength)
{
    if (!Info) return STATUS_INVALID_PARAMETER;

    vma_lock(Space);
    PVMA_ENTRY e = VmaFind(Space, Address);

    if (!e) {
        /* Free region — find the size of the free hole */
        UINT64 base = Address & ~(ALLOC_GRAN - 1);
        UINT64 end  = USER_ADDRESS_MAX;
        PVMA_ENTRY scan = Space->Head;
        while (scan) {
            if (scan->BaseAddress > base) { end = scan->BaseAddress; break; }
            scan = scan->Next;
        }
        vma_unlock(Space);
        Info->BaseAddress       = (void *)(uintptr_t)base;
        Info->AllocationBase    = NULL;
        Info->AllocationProtect = 0;
        Info->RegionSize        = end - base;
        Info->State             = MEM_FREE;
        Info->Protect           = PAGE_NOACCESS;
        Info->Type              = 0;
    } else {
        vma_unlock(Space);
        Info->BaseAddress       = (void *)(uintptr_t)e->BaseAddress;
        Info->AllocationBase    = (void *)(uintptr_t)e->BaseAddress;
        Info->AllocationProtect = e->Protect;
        Info->RegionSize        = e->RegionSize;
        Info->State             = e->State;
        Info->Protect           = e->Protect;
        Info->Type              = e->Type == VMA_TYPE_PRIVATE ? MEM_PRIVATE
                                : e->Type == VMA_TYPE_IMAGE   ? MEM_IMAGE
                                : MEM_MAPPED;
    }

    if (ReturnLength) *ReturnLength = sizeof(MEMORY_BASIC_INFORMATION);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VmaMap — insert a pre-built entry (called by NtMapViewOfSection)
 * ----------------------------------------------------------------------- */
NTSTATUS VmaMap(PVMA_SPACE Space, PVMA_ENTRY Entry)
{
    vma_lock(Space);

    /* Check for overlap */
    UINT64 ebase = Entry->BaseAddress;
    UINT64 eend  = ebase + Entry->RegionSize;
    PVMA_ENTRY scan = Space->Head;
    while (scan) {
        if (scan->BaseAddress < eend && scan->BaseAddress + scan->RegionSize > ebase) {
            vma_unlock(Space);
            return STATUS_CONFLICTING_ADDRESSES;
        }
        scan = scan->Next;
    }

    vma_insert(Space, Entry);
    vma_unlock(Space);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VmaUnmap — remove entry covering exactly [Base, Base+Size)
 * ----------------------------------------------------------------------- */
NTSTATUS VmaUnmap(PVMA_SPACE Space, UINT64 Base, UINT64 Size)
{
    vma_lock(Space);
    PVMA_ENTRY *pp = &Space->Head;
    while (*pp) {
        PVMA_ENTRY e = *pp;
        if (e->BaseAddress == Base && e->RegionSize == Size) {
            *pp = e->Next;
            Space->Count--;
            kfree(e);
            vma_unlock(Space);
            return STATUS_SUCCESS;
        }
        pp = &(*pp)->Next;
    }
    vma_unlock(Space);
    return STATUS_NOT_FOUND;
}
