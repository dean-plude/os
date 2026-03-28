/*
 * section.h — NT Section Objects (NtCreateSection / NtMapViewOfSection)
 *
 * A Section object represents a region of memory that can be shared
 * between processes, or used to map a file into a process's address space.
 * Sections are the basis of all executable image loading in NT.
 *
 * NT Section terminology:
 *   SEC_COMMIT    0x8000000  — Backing store is committed (physical pages)
 *   SEC_RESERVE   0x4000000  — Backing store is reserved (no physical pages)
 *   SEC_IMAGE     0x1000000  — Section is a PE image (file-backed, mapped at
 *                              image base, import-resolved)
 *   SEC_NOCACHE   0x10000000 — Pages not cached
 *   SEC_WRITECOMBINE 0x40000000
 *   SEC_LARGE_PAGES  0x80000000
 *
 * Phase 3 scope:
 *   - Anonymous committed sections (SEC_COMMIT, no file)
 *   - Image sections (SEC_IMAGE) backed by a PE buffer in memory
 *   - NtCreateSection, NtOpenSection, NtMapViewOfSection, NtUnmapViewOfSection
 *   - No file-backed non-image sections yet (needs NtReadFile / file system)
 *
 * View mapping:
 *   Each call to NtMapViewOfSection creates a VMA_ENTRY in the target
 *   process's VMA_SPACE.  The VMA_ENTRY references the SECTION_OBJECT body.
 *   When the view is unmapped the VMA_ENTRY is removed.
 */

#pragma once

#include "../include/types.h"
#include "../ob/ob.h"
#include "vma.h"

/* -----------------------------------------------------------------------
 * SEC_* flags (CreateSection Attributes)
 * ----------------------------------------------------------------------- */
#define SEC_BASED            0x00200000
#define SEC_NO_CHANGE        0x00400000
#define SEC_IMAGE            0x01000000
#define SEC_VLM              0x02000000
#define SEC_RESERVE          0x04000000
#define SEC_COMMIT           0x08000000
#define SEC_NOCACHE          0x10000000
#define SEC_WRITECOMBINE     0x40000000
#define SEC_LARGE_PAGES      0x80000000
#define SEC_IMAGE_NO_EXECUTE (SEC_IMAGE | SEC_NOCACHE)

/* SECTION_* access rights */
#define SECTION_QUERY        0x0001
#define SECTION_MAP_WRITE    0x0002
#define SECTION_MAP_READ     0x0004
#define SECTION_MAP_EXECUTE  0x0008
#define SECTION_EXTEND_SIZE  0x0010
#define SECTION_MAP_EXECUTE_EXPLICIT 0x0020
#define SECTION_ALL_ACCESS   (STANDARD_RIGHTS_REQUIRED | 0x01F)

/* -----------------------------------------------------------------------
 * SECTION_OBJECT — the object body for Section objects
 *
 * Memory layout:
 *   - If not SEC_IMAGE: a flat array of pages large enough to hold MaximumSize.
 *     Pages are physically allocated and mapped into the physmap.
 *   - If SEC_IMAGE: a pointer to a copy of the PE image buffer.
 *     The PE loader (ldr.c) processes the image when mapping the section.
 * ----------------------------------------------------------------------- */
typedef struct _SECTION_OBJECT {
    UINT32   SectionFlags;      /* SEC_* */
    UINT64   MaximumSize;       /* Maximum section size in bytes */
    UINT32   PageProtect;       /* Initial page protection */

    /* Anonymous section: array of physical page addresses */
    PADDR   *Pages;             /* kmalloc'd array of PADDR */
    UINT32   PageCount;

    /* Image section: pointer to PE image buffer in kernel memory */
    void    *ImageBase;         /* Pointer to the PE image data */
    UINT64   ImageSize;         /* Total virtual size of image */
    UINT64   PreferredBase;     /* PE optional header ImageBase */
    UINT64   EntryPoint;        /* RVA of EP (added to actual base at load) */
    UINT16   Machine;           /* IMAGE_FILE_MACHINE_AMD64 */
    bool     IsImage;
    bool     IsMapped;          /* At least one view exists */
} SECTION_OBJECT, *PSECTION_OBJECT;

/* -----------------------------------------------------------------------
 * NtMapViewOfSection InheritDisposition
 * ----------------------------------------------------------------------- */
#define ViewShare        1
#define ViewUnmap        2

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize the Section subsystem (register Section object type) */
void MmInitializeSections(void);

/*
 * Create a new Section object.
 * @SectionHandle:    Receives the handle.
 * @DesiredAccess:    SECTION_* access rights.
 * @ObjectAttributes: Optional name for the section in the namespace.
 * @MaximumSize:      Maximum size in bytes (NULL for page-file-backed).
 * @SectionPageProtection: PAGE_* initial protection.
 * @AllocationAttributes: SEC_* flags.
 * @FileHandle:       NULL for anonymous; file handle for file-backed.
 *                    For SEC_IMAGE, this must be a handle to a PE image file.
 */
NTSTATUS NtCreateSection(
    HANDLE             *SectionHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes,
    UINT64             *MaximumSize,
    UINT32              SectionPageProtection,
    UINT32              AllocationAttributes,
    HANDLE              FileHandle);

/*
 * Map a view of a section into a process's address space.
 * @SectionHandle:   Handle to the SECTION_OBJECT.
 * @ProcessHandle:   Handle to target process (-1 = current).
 * @BaseAddress:     In/out: desired VA (0 = OS picks).
 * @ZeroBits:        Number of high-order VA bits that must be zero.
 * @CommitSize:      Number of bytes initially committed (for SEC_RESERVE).
 * @SectionOffset:   Offset within section (must be allocation-granularity aligned).
 * @ViewSize:        Size of the view in bytes (0 = map entire section).
 * @InheritDisposition: ViewShare or ViewUnmap.
 * @AllocationType:  0 or MEM_TOP_DOWN.
 * @Win32Protect:    PAGE_* protection for the view.
 */
NTSTATUS NtMapViewOfSection(
    HANDLE   SectionHandle,
    HANDLE   ProcessHandle,
    void   **BaseAddress,
    ULONG_PTR ZeroBits,
    UINT64   CommitSize,
    UINT64  *SectionOffset,
    UINT64  *ViewSize,
    UINT32   InheritDisposition,
    UINT32   AllocationType,
    UINT32   Win32Protect);

/*
 * Unmap a view of a section from a process's address space.
 */
NTSTATUS NtUnmapViewOfSection(HANDLE ProcessHandle, void *BaseAddress);

/*
 * Create an image section from a PE file already loaded in kernel memory.
 * Used by the PE loader to pre-process a PE binary.
 *
 * @PeBuffer:    Pointer to the raw PE image (kernel virtual address).
 * @PeSize:      Size of the buffer in bytes.
 * @SectionOut:  Receives the SECTION_OBJECT body pointer (not a handle).
 */
NTSTATUS MmCreateImageSection(void *PeBuffer, UINT64 PeSize,
                                PSECTION_OBJECT *SectionOut);

/*
 * Map an image section into a process address space (PE loader helper).
 * Maps each PE section at its preferred RVA (or with relocation applied).
 * Returns the actual image base VA.
 */
NTSTATUS MmMapImageView(PSECTION_OBJECT Section,
                         PVMA_SPACE       VmaSpace,
                         UINT64          *BaseVA);

/* ObpSectionType registered by MmInitializeSections */
/* (declared in ob.h already) */
