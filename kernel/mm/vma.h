/*
 * vma.h — Virtual Memory Area (VAD) manager
 *
 * The VAD manager tracks the user-mode virtual address space of each
 * process as a sorted list of non-overlapping VMA_ENTRY regions.
 *
 * NT uses an AVL tree (MM_AVL_TABLE) for this; Phase 3 uses a simpler
 * sorted singly-linked list.  The design is the same: each entry covers
 * [BaseAddress, BaseAddress + RegionSize) and has protection flags.
 *
 * Regions are inserted by NtAllocateVirtualMemory and removed by
 * NtFreeVirtualMemory.  Sections mapped by NtMapViewOfSection also
 * create VMA entries (type = VMA_TYPE_MAPPED).
 *
 * NT MEM_* type constants (from winnt.h):
 *   MEM_COMMIT    0x1000 — pages are backed by physical memory
 *   MEM_RESERVE   0x2000 — VA range is reserved but not committed
 *   MEM_DECOMMIT  0x4000 — NtFreeVirtualMemory: uncommit
 *   MEM_RELEASE   0x8000 — NtFreeVirtualMemory: release
 *   MEM_FREE      0x10000 — query: region is free
 *   MEM_PRIVATE   0x20000 — private (not shared)
 *   MEM_MAPPED    0x40000 — mapped view of section
 *   MEM_IMAGE     0x1000000 — mapped image (PE)
 *
 * Page protection constants (NT PAGE_* from winnt.h):
 *   PAGE_NOACCESS         0x01
 *   PAGE_READONLY         0x02
 *   PAGE_READWRITE        0x04
 *   PAGE_WRITECOPY        0x08
 *   PAGE_EXECUTE          0x10
 *   PAGE_EXECUTE_READ     0x20
 *   PAGE_EXECUTE_READWRITE 0x40
 *   PAGE_EXECUTE_WRITECOPY 0x80
 *   PAGE_GUARD            0x100
 *   PAGE_NOCACHE          0x200
 *   PAGE_WRITECOMBINE     0x400
 */

#pragma once

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * MEM_* allocation type flags
 * ----------------------------------------------------------------------- */
#define MEM_COMMIT          0x00001000
#define MEM_RESERVE         0x00002000
#define MEM_DECOMMIT        0x00004000
#define MEM_RELEASE         0x00008000
#define MEM_FREE            0x00010000
#define MEM_PRIVATE         0x00020000
#define MEM_MAPPED          0x00040000
#define MEM_TOP_DOWN        0x00100000
#define MEM_IMAGE           0x01000000

/* -----------------------------------------------------------------------
 * PAGE_* protection flags
 * ----------------------------------------------------------------------- */
#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_WRITECOPY          0x08
#define PAGE_EXECUTE            0x10
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40
#define PAGE_EXECUTE_WRITECOPY  0x80
#define PAGE_GUARD              0x100
#define PAGE_NOCACHE            0x200
#define PAGE_WRITECOMBINE       0x400

/* -----------------------------------------------------------------------
 * VMA entry types
 * ----------------------------------------------------------------------- */
#define VMA_TYPE_PRIVATE  0   /* Private allocation (NtAllocateVirtualMemory) */
#define VMA_TYPE_MAPPED   1   /* Mapped section view (NtMapViewOfSection) */
#define VMA_TYPE_IMAGE    2   /* PE image section */

/* -----------------------------------------------------------------------
 * VMA_ENTRY — one contiguous VA region
 * ----------------------------------------------------------------------- */
typedef struct _VMA_ENTRY {
    struct _VMA_ENTRY *Next;
    UINT64  BaseAddress;    /* Page-aligned base */
    UINT64  RegionSize;     /* Size in bytes (multiple of PAGE_SIZE) */
    UINT32  State;          /* MEM_COMMIT | MEM_RESERVE */
    UINT32  Protect;        /* PAGE_* flags */
    UINT32  Type;           /* VMA_TYPE_* */
    /* For MAPPED/IMAGE: pointer to backing SECTION object body */
    void   *SectionObject;
    UINT64  SectionOffset;  /* Offset within section */
} VMA_ENTRY, *PVMA_ENTRY;

/* -----------------------------------------------------------------------
 * VMA_SPACE — the full VA space for one process
 * ----------------------------------------------------------------------- */
#define USER_ADDRESS_MIN    0x0000000000010000ULL   /* 64 KB */
#define USER_ADDRESS_MAX    0x00007FFFFFFF0000ULL   /* Just below 128 TB */

typedef struct _VMA_SPACE {
    PVMA_ENTRY  Head;   /* Sorted by BaseAddress, ascending */
    UINT32      Count;
    /* Ticket spinlock */
    volatile UINT32 LockNext, LockOwner;
} VMA_SPACE, *PVMA_SPACE;

/* -----------------------------------------------------------------------
 * MEMORY_BASIC_INFORMATION — returned by NtQueryVirtualMemory
 * ----------------------------------------------------------------------- */
typedef struct _MEMORY_BASIC_INFORMATION {
    void   *BaseAddress;
    void   *AllocationBase;
    UINT32  AllocationProtect;
    UINT64  RegionSize;
    UINT32  State;       /* MEM_COMMIT / MEM_RESERVE / MEM_FREE */
    UINT32  Protect;     /* PAGE_* */
    UINT32  Type;        /* MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE */
} MEMORY_BASIC_INFORMATION;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Internal spinlock helpers — used by section.c to hold the VMA lock across
 * page mapping operations that must be atomic with VMA entry insertion. */
static inline void vma_lock(PVMA_SPACE s)
{
    UINT32 t = __atomic_fetch_add(&s->LockNext, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s->LockOwner, __ATOMIC_ACQUIRE) != t)
        __asm__ volatile("pause");
}
static inline void vma_unlock(PVMA_SPACE s)
{
    __atomic_fetch_add(&s->LockOwner, 1, __ATOMIC_RELEASE);
}

/* Initialize a new, empty VMA space. */
void VmaInitSpace(PVMA_SPACE Space);

/* Free all entries in a VMA space (called when process exits). */
void VmaDestroySpace(PVMA_SPACE Space);

/*
 * Reserve and/or commit a VA range in the given space.
 * Mirrors NtAllocateVirtualMemory semantics.
 *
 * @Space:        Target process VA space.
 * @BaseAddress:  In/out: desired base (0 = OS picks); aligned to 64KB.
 * @RegionSize:   In/out: desired size; aligned to PAGE_SIZE.
 * @AllocType:    MEM_COMMIT | MEM_RESERVE (or both).
 * @Protect:      PAGE_* protection.
 *
 * On success: *BaseAddress and *RegionSize reflect the actual allocation.
 */
NTSTATUS VmaAllocate(PVMA_SPACE   Space,
                      UINT64      *BaseAddress,
                      UINT64      *RegionSize,
                      UINT32       AllocType,
                      UINT32       Protect);

/*
 * Decommit or release a VA range.
 * @FreeType: MEM_DECOMMIT or MEM_RELEASE.
 */
NTSTATUS VmaFree(PVMA_SPACE  Space,
                  UINT64     *BaseAddress,
                  UINT64     *RegionSize,
                  UINT32      FreeType);

/*
 * Change protection of an existing committed region.
 */
NTSTATUS VmaProtect(PVMA_SPACE  Space,
                     UINT64     *BaseAddress,
                     UINT64     *RegionSize,
                     UINT32      NewProtect,
                     UINT32     *OldProtect);

/*
 * Query the state of a VA region containing Address.
 */
NTSTATUS VmaQuery(PVMA_SPACE               Space,
                   UINT64                   Address,
                   MEMORY_BASIC_INFORMATION *Info,
                   UINT64                  *ReturnLength);

/*
 * Map an existing VMA_ENTRY (e.g. from NtMapViewOfSection) directly.
 * The entry is fully initialized by the caller; VmaMap links it in.
 */
NTSTATUS VmaMap(PVMA_SPACE Space, PVMA_ENTRY Entry);

/*
 * Unmap and free the VMA_ENTRY exactly covering [Base, Base+Size).
 */
NTSTATUS VmaUnmap(PVMA_SPACE Space, UINT64 Base, UINT64 Size);

/*
 * Find the VMA entry containing Address (NULL if in a free region).
 * Caller must hold the space lock or be single-threaded.
 */
PVMA_ENTRY VmaFind(PVMA_SPACE Space, UINT64 Address);
