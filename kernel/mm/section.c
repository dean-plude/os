/*
 * section.c — NT Section Object implementation (Phase 3)
 *
 * Anonymous sections (SEC_COMMIT, no file):
 *   - Physical pages are allocated from PMM at creation time.
 *   - Pages are stored in Section->Pages[] array.
 *   - NtMapViewOfSection maps those pages into the target VA space.
 *
 * Image sections (SEC_IMAGE):
 *   - Backed by a PE buffer (already in kernel memory).
 *   - MmMapImageView() calls the PE loader to map sections.
 *   - The PE buffer is NOT freed at section deletion (caller manages it).
 *
 * Phase 3 simplification: sections are not file-backed.  File-backed
 * sections require a working file system (Phase 4).
 */

#include "section.h"
#include "vma.h"
#include "vmm.h"
#include "pmm.h"
#include "../ob/ob.h"
#include "../ke/printf.h"
#include "../include/types.h"
#include "../arch/x86_64/paging.h"

/* -----------------------------------------------------------------------
 * Section object type
 * ----------------------------------------------------------------------- */
static void section_delete(void *obj)
{
    PSECTION_OBJECT sec = (PSECTION_OBJECT)obj;
    if (!sec->IsImage && sec->Pages) {
        for (UINT32 i = 0; i < sec->PageCount; i++) {
            if (sec->Pages[i]) pmm_free_page(sec->Pages[i]);
        }
        kfree(sec->Pages);
        sec->Pages = NULL;
    }
    /* ImageBase not freed — callee manages PE buffers */
}

static OBJECT_TYPE mm_section_type_storage = {
    .Name            = "Section",
    .DefaultBodySize = sizeof(SECTION_OBJECT),
    .GenericAll      = SECTION_ALL_ACCESS,
    .Operations      = { .Delete = section_delete },
};

void MmInitializeSections(void)
{
    ObpSectionType = &mm_section_type_storage;
    ObCreateObjectType(ObpSectionType);
    kprintf("[MM] Section object type registered\n");
}

/* -----------------------------------------------------------------------
 * NtCreateSection
 * ----------------------------------------------------------------------- */
NTSTATUS NtCreateSection(
    HANDLE             *SectionHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes,
    UINT64             *MaximumSize,
    UINT32              SectionPageProtection,
    UINT32              AllocationAttributes,
    HANDLE              FileHandle)
{
    (void)FileHandle;  /* File-backed sections: Phase 4 */

    if (AllocationAttributes & SEC_IMAGE) {
        /* Image sections must go through MmCreateImageSection */
        return STATUS_NOT_IMPLEMENTED;
    }

    UINT64 size = MaximumSize ? *MaximumSize : 0;
    if (!size) return STATUS_INVALID_PARAMETER;

    /* Round up to page boundary */
    size = (size + PAGE_SIZE - 1) & ~((UINT64)PAGE_SIZE - 1);
    UINT32 npages = (UINT32)(size / PAGE_SIZE);

    /* Create the object */
    OBJECT_ATTRIBUTES default_attr;
    if (!ObjectAttributes) {
        __builtin_memset(&default_attr, 0, sizeof(default_attr));
        default_attr.Length = sizeof(OBJECT_ATTRIBUTES);
        ObjectAttributes = &default_attr;
    }

    void *obj;
    NTSTATUS s = ObCreateObject(ObpSectionType, ObjectAttributes, 0, &obj);
    if (!NT_SUCCESS(s)) return s;

    PSECTION_OBJECT sec = (PSECTION_OBJECT)obj;
    sec->SectionFlags = AllocationAttributes;
    sec->MaximumSize  = size;
    sec->PageProtect  = SectionPageProtection;
    sec->IsImage      = false;

    if (AllocationAttributes & SEC_COMMIT) {
        /* Allocate physical pages now */
        sec->Pages = kzalloc(sizeof(PADDR) * npages);
        if (!sec->Pages) {
            ObDereferenceObject(obj);
            return STATUS_NO_MEMORY;
        }
        sec->PageCount = npages;

        for (UINT32 i = 0; i < npages; i++) {
            sec->Pages[i] = pmm_alloc_page();
            if (!sec->Pages[i]) {
                /* Roll back */
                for (UINT32 j = 0; j < i; j++) pmm_free_page(sec->Pages[j]);
                kfree(sec->Pages);
                sec->Pages = NULL;
                ObDereferenceObject(obj);
                return STATUS_NO_MEMORY;
            }
            /* Zero the page via physmap */
            __builtin_memset((void *)(PHYSMAP_BASE + sec->Pages[i]), 0, PAGE_SIZE);
        }
    }

    HANDLE h = 0;
    s = ObInsertObject(obj, NULL, DesiredAccess, 0, NULL, &h);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(obj); return s; }

    if (SectionHandle) *SectionHandle = h;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtMapViewOfSection
 * ----------------------------------------------------------------------- */
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
    UINT32   Win32Protect)
{
    (void)ZeroBits; (void)CommitSize; (void)InheritDisposition;
    (void)AllocationType;

    /* Resolve section handle */
    void *sec_obj;
    NTSTATUS s = ObReferenceObjectByHandle(SectionHandle, SECTION_MAP_READ,
                                            ObpSectionType, NULL, &sec_obj, NULL);
    if (!NT_SUCCESS(s)) return s;
    PSECTION_OBJECT sec = (PSECTION_OBJECT)sec_obj;

    /* Resolve process handle → VMA_SPACE
     * Phase 3: only support current process (-1) */
    extern PEPROCESS PsGetCurrentProcess(void);
    PEPROCESS proc;
    if ((INT64)ProcessHandle == -1) {
        proc = PsGetCurrentProcess();
    } else {
        void *proc_obj;
        s = ObReferenceObjectByHandle(ProcessHandle, PROCESS_ALL_ACCESS,
                                       ObpProcessType, NULL, &proc_obj, NULL);
        if (!NT_SUCCESS(s)) { ObDereferenceObject(sec_obj); return s; }
        proc = (PEPROCESS)proc_obj;
    }

    if (!proc) { ObDereferenceObject(sec_obj); return STATUS_INVALID_HANDLE; }

    PVMA_SPACE vma = &proc->VmaSpace;

    /* Determine view size */
    UINT64 view_size = ViewSize ? *ViewSize : 0;
    if (!view_size || view_size > sec->MaximumSize)
        view_size = sec->MaximumSize;
    view_size = (view_size + PAGE_SIZE - 1) & ~((UINT64)PAGE_SIZE - 1);

    UINT64 sec_off = SectionOffset ? *SectionOffset : 0;
    UINT32 page_off = (UINT32)(sec_off / PAGE_SIZE);

    /* Determine the base VA */
    UINT64 base_va = BaseAddress ? (UINT64)(uintptr_t)*BaseAddress : 0;

    if (!base_va) {
        /* Find a free VA region */
        base_va = 0;
        UINT64 tmp_base = 0;
        UINT64 tmp_size = view_size;
        s = VmaAllocate(vma, &tmp_base, &tmp_size, MEM_RESERVE, Win32Protect);
        if (!NT_SUCCESS(s)) { ObDereferenceObject(sec_obj); return s; }
        /* Free the reservation — we'll re-use the address with our own entry */
        UINT64 free_base = tmp_base, free_size = tmp_size;
        VmaFree(vma, &free_base, &free_size, MEM_RELEASE);
        base_va = tmp_base;
    }

    /* Map the physical pages */
    uint32_t map_flags = MAP_USER;
    if (Win32Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
        map_flags |= MAP_WRITABLE;
    if (!(Win32Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        map_flags |= MAP_NO_EXEC;

    UINT32 pages_to_map = (UINT32)(view_size / PAGE_SIZE);
    if (!sec->IsImage && sec->Pages) {
        for (UINT32 i = 0; i < pages_to_map; i++) {
            UINT32 pi = page_off + i;
            if (pi >= sec->PageCount) break;
            UINT64 va = base_va + (UINT64)i * PAGE_SIZE;
            s = paging_map((VADDR)va, sec->Pages[pi], map_flags);
            if (!NT_SUCCESS(s)) {
                /* Unmap already-mapped */
                for (UINT32 j = 0; j < i; j++)
                    paging_unmap((VADDR)(base_va + (UINT64)j * PAGE_SIZE));
                ObDereferenceObject(sec_obj);
                return s;
            }
        }
    }
    /* Image sections are handled by MmMapImageView, not here */

    /* Create a VMA entry */
    PVMA_ENTRY e = kzalloc(sizeof(VMA_ENTRY));
    if (!e) {
        for (UINT32 i = 0; i < pages_to_map; i++)
            paging_unmap((VADDR)(base_va + (UINT64)i * PAGE_SIZE));
        ObDereferenceObject(sec_obj);
        return STATUS_NO_MEMORY;
    }

    e->BaseAddress    = base_va;
    e->RegionSize     = view_size;
    e->State          = MEM_COMMIT;
    e->Protect        = Win32Protect;
    e->Type           = VMA_TYPE_MAPPED;
    e->SectionObject  = sec_obj;
    e->SectionOffset  = sec_off;

    s = VmaMap(vma, e);
    if (!NT_SUCCESS(s)) {
        for (UINT32 i = 0; i < pages_to_map; i++)
            paging_unmap((VADDR)(base_va + (UINT64)i * PAGE_SIZE));
        kfree(e);
        ObDereferenceObject(sec_obj);
        return s;
    }

    if (BaseAddress) *BaseAddress = (void *)(uintptr_t)base_va;
    if (ViewSize)    *ViewSize    = view_size;

    sec->IsMapped = true;
    /* Keep sec_obj referenced via VMA_ENTRY->SectionObject */
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * NtUnmapViewOfSection
 * ----------------------------------------------------------------------- */
NTSTATUS NtUnmapViewOfSection(HANDLE ProcessHandle, void *BaseAddress)
{
    extern PEPROCESS PsGetCurrentProcess(void);
    PEPROCESS proc;
    NTSTATUS s;

    if ((INT64)ProcessHandle == -1) {
        proc = PsGetCurrentProcess();
    } else {
        void *proc_obj;
        s = ObReferenceObjectByHandle(ProcessHandle, PROCESS_ALL_ACCESS,
                                       ObpProcessType, NULL, &proc_obj, NULL);
        if (!NT_SUCCESS(s)) return s;
        proc = (PEPROCESS)proc_obj;
    }

    if (!proc) return STATUS_INVALID_HANDLE;

    UINT64 base = (UINT64)(uintptr_t)BaseAddress;
    PVMA_SPACE vma = &proc->VmaSpace;

    /* Find the entry */
    vma_lock(vma);   /* Access internal lock directly via inline in vma.h path */
    PVMA_ENTRY e = VmaFind(vma, base);
    if (!e || e->BaseAddress != base || e->Type != VMA_TYPE_MAPPED) {
        vma_unlock(vma);
        return STATUS_NOT_MAPPED_VIEW;
    }

    UINT64 region_size = e->RegionSize;
    void  *sec_obj     = e->SectionObject;

    /* Unmap physical pages */
    UINT32 pages = (UINT32)(region_size / PAGE_SIZE);
    for (UINT32 i = 0; i < pages; i++)
        paging_unmap((VADDR)(base + (UINT64)i * PAGE_SIZE));

    vma_unlock(vma);
    VmaUnmap(vma, base, region_size);

    if (sec_obj) ObDereferenceObject(sec_obj);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * MmCreateImageSection — build a SECTION_OBJECT from a PE buffer
 * ----------------------------------------------------------------------- */
NTSTATUS MmCreateImageSection(void *PeBuffer, UINT64 PeSize,
                                PSECTION_OBJECT *SectionOut)
{
    /* Peek at DOS + NT headers to get ImageBase and ImageSize */
    if (PeSize < 64) return STATUS_INVALID_IMAGE_FORMAT;

    UINT8 *buf = (UINT8 *)PeBuffer;

    /* DOS header: e_magic = 'MZ', e_lfanew at offset 60 */
    if (buf[0] != 'M' || buf[1] != 'Z') return STATUS_INVALID_IMAGE_FORMAT;
    UINT32 nt_off = *(UINT32 *)(buf + 60);
    if (nt_off + 264 > PeSize) return STATUS_INVALID_IMAGE_FORMAT;

    /* NT signature */
    UINT8 *nth = buf + nt_off;
    if (nth[0] != 'P' || nth[1] != 'E' || nth[2] != 0 || nth[3] != 0)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* File header at NT+4 */
    UINT16 machine       = *(UINT16 *)(nth + 4);
    UINT16 num_sections  = *(UINT16 *)(nth + 6);
    UINT16 opt_hdr_size  = *(UINT16 *)(nth + 20);
    (void)num_sections;

    /* Optional header at NT+24 */
    UINT8 *opt = nth + 24;
    UINT16 magic = *(UINT16 *)opt;
    if (magic != 0x020B) return STATUS_INVALID_IMAGE_FORMAT; /* not PE32+ */

    UINT64 image_base    = *(UINT64 *)(opt + 24);
    UINT32 image_size    = *(UINT32 *)(opt + 56);
    UINT32 ep_rva        = *(UINT32 *)(opt + 16);
    (void)opt_hdr_size;

    /* Create the object */
    OBJECT_ATTRIBUTES attr;
    __builtin_memset(&attr, 0, sizeof(attr));
    attr.Length = sizeof(OBJECT_ATTRIBUTES);

    void *obj;
    NTSTATUS s = ObCreateObject(ObpSectionType, &attr, 0, &obj);
    if (!NT_SUCCESS(s)) return s;

    PSECTION_OBJECT sec = (PSECTION_OBJECT)obj;
    sec->SectionFlags   = SEC_IMAGE | SEC_COMMIT;
    sec->MaximumSize    = image_size;
    sec->PageProtect    = PAGE_EXECUTE_READ;
    sec->IsImage        = true;
    sec->ImageBase      = PeBuffer;
    sec->ImageSize      = image_size;
    sec->PreferredBase  = image_base;
    sec->EntryPoint     = ep_rva;
    sec->Machine        = machine;

    if (SectionOut) *SectionOut = sec;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * MmMapImageView — map all PE sections into a process VA space
 *
 * This is the core of PE loading:
 *   1. Determine load base (use PreferredBase if the range is free,
 *      otherwise find a free range and apply base relocation).
 *   2. For each PE section header:
 *      a. Allocate physical pages.
 *      b. Map them into the process VA space at RVA + load_base.
 *      c. Copy raw data from the PE buffer.
 *   3. Return the actual load base in *BaseVA.
 *   4. The caller (LDR) then applies imports and relocations.
 * ----------------------------------------------------------------------- */
NTSTATUS MmMapImageView(PSECTION_OBJECT Section,
                         PVMA_SPACE       VmaSpace,
                         UINT64          *BaseVA)
{
    UINT8 *buf   = (UINT8 *)Section->ImageBase;
    UINT32 nt_off = *(UINT32 *)(buf + 60);
    UINT8 *nth    = buf + nt_off;
    UINT8 *opt    = nth + 24;

    UINT16 num_sections = *(UINT16 *)(nth + 6);
    UINT16 opt_hdr_size = *(UINT16 *)(nth + 20);
    UINT32 size_of_hdrs = *(UINT32 *)(opt + 60);
    UINT32 image_size   = *(UINT32 *)(opt + 56);
    UINT64 pref_base    = *(UINT64 *)(opt + 24);

    /* Section headers start at NT+24+OptHdrSize */
    UINT8 *sec_hdrs = nth + 24 + opt_hdr_size;

    /* Try preferred base first */
    UINT64 load_base = pref_base;

    /* Check if preferred base range is free */
    PVMA_ENTRY existing = VmaFind(VmaSpace, load_base);
    if (existing) {
        /* Need to relocate — find a free range */
        UINT64 size64 = (UINT64)image_size;
        UINT64 free_base = 0;
        UINT64 tmp_size  = size64;
        NTSTATUS s = VmaAllocate(VmaSpace, &free_base, &tmp_size,
                                  MEM_RESERVE, PAGE_NOACCESS);
        if (!NT_SUCCESS(s)) return s;
        /* Release the reservation — we'll map manually */
        UINT64 fb = free_base;
        VmaFree(VmaSpace, &fb, &tmp_size, MEM_RELEASE);
        load_base = free_base;
    }

    *BaseVA = load_base;

    /* Helper: allocate pages and map a range into VmaSpace */
    /* We map the entire image_size range as private pages */
    UINT64 total_pages = ((UINT64)image_size + PAGE_SIZE - 1) / PAGE_SIZE;
    PADDR *page_array  = kzalloc(sizeof(PADDR) * (size_t)total_pages);
    if (!page_array) return STATUS_NO_MEMORY;

    for (UINT64 i = 0; i < total_pages; i++) {
        page_array[i] = pmm_alloc_page();
        if (!page_array[i]) {
            for (UINT64 j = 0; j < i; j++) pmm_free_page(page_array[j]);
            kfree(page_array);
            return STATUS_NO_MEMORY;
        }
        /* Zero each page via physmap */
        __builtin_memset((void *)(PHYSMAP_BASE + page_array[i]), 0, PAGE_SIZE);
    }

    /* Map all pages RW+X initially (PE loader will re-protect per section) */
    for (UINT64 i = 0; i < total_pages; i++) {
        UINT64 va = load_base + i * PAGE_SIZE;
        NTSTATUS s = paging_map((VADDR)va, page_array[i],
                                MAP_USER | MAP_WRITABLE);
        if (!NT_SUCCESS(s)) {
            /* Unmap already-mapped pages */
            for (UINT64 j = 0; j < i; j++)
                paging_unmap((VADDR)(load_base + j * PAGE_SIZE));
            for (UINT64 j = 0; j < total_pages; j++)
                pmm_free_page(page_array[j]);
            kfree(page_array);
            return s;
        }
    }

    kfree(page_array);   /* page_array only needed to track PMM allocs; pages are now in paging */

    /* Copy PE headers (SizeOfHeaders bytes) */
    if (size_of_hdrs > image_size) size_of_hdrs = image_size;
    __builtin_memcpy((void *)(uintptr_t)load_base, buf, size_of_hdrs);

    /* Copy each PE section */
    for (UINT16 i = 0; i < num_sections; i++) {
        UINT8 *sh       = sec_hdrs + (size_t)i * 40;  /* IMAGE_SECTION_HEADER is 40 bytes */
        UINT32 virt_addr = *(UINT32 *)(sh + 12);       /* VirtualAddress */
        UINT32 virt_size = *(UINT32 *)(sh + 16);       /* Misc.VirtualSize */
        UINT32 raw_off   = *(UINT32 *)(sh + 20);       /* PointerToRawData */
        UINT32 raw_size  = *(UINT32 *)(sh + 16 + 4);   /* SizeOfRawData */

        if (!virt_addr || !virt_size) continue;

        UINT64 dst = load_base + virt_addr;
        UINT32 copy_size = (raw_size < virt_size) ? raw_size : virt_size;

        if (raw_off && copy_size) {
            if (raw_off + copy_size <= (UINT32)Section->MaximumSize)
                __builtin_memcpy((void *)(uintptr_t)dst, buf + raw_off, copy_size);
        }
        /* Remaining bytes are already zero (we zeroed pages above) */
    }

    /* Create a single VMA_ENTRY covering the whole image */
    PVMA_ENTRY e = kzalloc(sizeof(VMA_ENTRY));
    if (!e) return STATUS_NO_MEMORY;

    e->BaseAddress   = load_base;
    e->RegionSize    = (UINT64)image_size;
    e->State         = MEM_COMMIT;
    e->Protect       = PAGE_EXECUTE_WRITECOPY;
    e->Type          = VMA_TYPE_IMAGE;
    e->SectionObject = Section;

    NTSTATUS s = VmaMap(VmaSpace, e);
    if (!NT_SUCCESS(s)) { kfree(e); return s; }

    Section->IsMapped = true;
    return STATUS_SUCCESS;
}
