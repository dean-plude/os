/*
 * elf_loader.c — ELF64 parser and loader for the UEFI bootloader
 *
 * This module reads an ELF64 executable, allocates physical pages for each
 * PT_LOAD segment, and copies/zeroes them appropriately.
 *
 * Architectural choices:
 *  - We allocate at the LMA (physical load address) stored in the ELF PHDR.
 *    The kernel linker script separates VMA (high) from LMA (low physical).
 *  - We use AllocateAddress to get exactly the right physical pages.
 *    If the address is already in use (shouldn't happen for a fresh boot)
 *    we fall back to AllocateAnyPages and relocate — but for Phase 1 we
 *    keep it simple and require the fixed address to be free.
 *  - File reads are done in chunks to avoid needing large stack buffers.
 */

#include "../include/efi.h"
#include "elf_loader.h"

/* console_printf is defined in console.c and linked in — just declare it */
extern void console_printf(const char *fmt, ...);

/* ------------------------------------------------------------------
 * ELF64 structures (per System V ABI / ELF-64 Object File Format)
 * ------------------------------------------------------------------ */

#define ELFMAG0  0x7F
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1   /* little-endian */
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_NULL     0
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;    /* Program header table offset */
    UINT64 e_shoff;    /* Section header table offset */
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;  /* Offset in file */
    UINT64 p_vaddr;   /* Virtual address */
    UINT64 p_paddr;   /* Physical address (LMA) */
    UINT64 p_filesz;  /* Bytes in file */
    UINT64 p_memsz;   /* Bytes in memory (>= filesz, extra is zeroed = BSS) */
    UINT64 p_align;   /* Alignment */
} Elf64_Phdr;

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

/* EFI file read with explicit offset (SetPosition + Read). */
static EFI_STATUS file_read_at(EFI_FILE_PROTOCOL *file,
                                UINT64 offset, VOID *buf, UINTN size)
{
    EFI_STATUS s = file->SetPosition(file, offset);
    if (EFI_ERROR(s)) return s;
    return file->Read(file, &size, buf);
}

/* Zero a memory range — no libc in UEFI freestanding context. */
static void mem_zero(VOID *ptr, UINTN n)
{
    UINT8 *p = ptr;
    while (n--) *p++ = 0;
}

static void mem_copy(VOID *dst, const VOID *src, UINTN n)
{
    UINT8       *d = dst;
    const UINT8 *s = src;
    while (n--) *d++ = *s++;
}

/* Align a value upwards to the given power-of-2 alignment. */
static UINT64 align_up(UINT64 val, UINT64 align)
{
    return (val + align - 1) & ~(align - 1);
}

/* ------------------------------------------------------------------
 * elf_load — main entry point
 * ------------------------------------------------------------------ */
EFI_STATUS elf_load(
    EFI_FILE_PROTOCOL *file,
    UINTN              file_size,
    UINT64            *entry_out,
    UINT64            *phys_base_out,
    UINT64            *virt_base_out,
    UINT64            *size_out,
    EFI_BOOT_SERVICES *bs)
{
    EFI_STATUS  status;
    Elf64_Ehdr  ehdr;
    (void)file_size;  /* used for validation in a more hardened impl */

    /* -- 1. Read and validate ELF header ----------------------------- */
    status = file_read_at(file, 0, &ehdr, sizeof(ehdr));
    if (EFI_ERROR(status)) {
        console_printf("ELF: failed to read header: %x\r\n", (UINT64)status);
        return status;
    }

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        console_printf("ELF: bad magic\r\n");
        return EFI_LOAD_ERROR;
    }
    if (ehdr.e_ident[4] != ELFCLASS64) {
        console_printf("ELF: not 64-bit\r\n");
        return EFI_LOAD_ERROR;
    }
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        console_printf("ELF: not little-endian\r\n");
        return EFI_LOAD_ERROR;
    }
    if (ehdr.e_machine != EM_X86_64) {
        console_printf("ELF: not x86_64 (e_machine=%d)\r\n", (UINT64)ehdr.e_machine);
        return EFI_LOAD_ERROR;
    }
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        console_printf("ELF: not an executable\r\n");
        return EFI_LOAD_ERROR;
    }
    if (ehdr.e_phentsize < sizeof(Elf64_Phdr)) {
        console_printf("ELF: phentsize too small\r\n");
        return EFI_LOAD_ERROR;
    }

    console_printf("ELF: entry=0x%x, %d segments\r\n",
                   ehdr.e_entry, (UINT64)ehdr.e_phnum);

    /* -- 2. Read all program headers ---------------------------------- */
    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 64) {
        console_printf("ELF: unreasonable phnum=%d\r\n", (UINT64)ehdr.e_phnum);
        return EFI_LOAD_ERROR;
    }

    Elf64_Phdr phdrs[64];
    UINTN phdr_bytes = ehdr.e_phnum * sizeof(Elf64_Phdr);
    status = file_read_at(file, ehdr.e_phoff, phdrs, phdr_bytes);
    if (EFI_ERROR(status)) {
        console_printf("ELF: failed to read phdrs: %x\r\n", (UINT64)status);
        return status;
    }

    /* -- 3. First pass: compute physical address range ---------------- */
    UINT64 phys_min = UINT64_C(0xFFFFFFFFFFFFFFFF);
    UINT64 phys_max = 0;
    UINT64 virt_min = UINT64_C(0xFFFFFFFFFFFFFFFF);

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_memsz == 0) continue;

        UINT64 seg_phys_start = phdrs[i].p_paddr;
        UINT64 seg_phys_end   = align_up(phdrs[i].p_paddr + phdrs[i].p_memsz, 4096);

        if (seg_phys_start < phys_min) phys_min = seg_phys_start;
        if (seg_phys_end   > phys_max) phys_max = seg_phys_end;
        if (phdrs[i].p_vaddr < virt_min) virt_min = phdrs[i].p_vaddr;

        console_printf("  LOAD: paddr=0x%x vaddr=0x%x filesz=0x%x memsz=0x%x\r\n",
                       phdrs[i].p_paddr, phdrs[i].p_vaddr,
                       phdrs[i].p_filesz, phdrs[i].p_memsz);
    }

    if (phys_max == 0) {
        console_printf("ELF: no loadable segments\r\n");
        return EFI_LOAD_ERROR;
    }

    /* Align physical base down to 2MB (huge page boundary) */
    UINT64 phys_base  = phys_min & ~(UINT64)(0x200000 - 1);
    UINT64 total_size = align_up(phys_max - phys_base, 4096);
    UINTN  num_pages  = (UINTN)(total_size / 4096);

    console_printf("ELF: allocating %u pages at phys 0x%x\r\n",
                   (UINT64)num_pages, phys_base);

    /* -- 4. Allocate physical pages at the requested address ---------- */
    UINT64 alloc_addr = phys_base;
    status = bs->AllocatePages(AllocateAddress, EfiLoaderData, num_pages, &alloc_addr);
    if (EFI_ERROR(status)) {
        /* Try any address as fallback — only works if kernel is position-independent */
        console_printf("ELF: AllocateAddress failed (%x), trying AnyPages\r\n",
                       (UINT64)status);
        status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, num_pages, &alloc_addr);
        if (EFI_ERROR(status)) {
            console_printf("ELF: AllocatePages failed: %x\r\n", (UINT64)status);
            return status;
        }
        console_printf("ELF: placed at phys 0x%x (non-ideal)\r\n", alloc_addr);
    }

    /* Zero the entire allocation (handles BSS). */
    mem_zero((VOID *)(UINTN)alloc_addr, (UINTN)total_size);

    /* -- 5. Copy each PT_LOAD segment --------------------------------- */
    /* Read in 64 KiB chunks to avoid large stack buffers. */
    VOID  *chunk_buf = NULL;
    UINTN  chunk_sz  = 65536;
    status = bs->AllocatePool(EfiLoaderData, chunk_sz, &chunk_buf);
    if (EFI_ERROR(status)) {
        console_printf("ELF: AllocatePool failed: %x\r\n", (UINT64)status);
        return status;
    }

    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;

        /* Destination physical address for this segment.
         * Adjust if we allocated at a different base than requested. */
        UINT64 dest_phys = alloc_addr + (ph->p_paddr - phys_base);
        UINT64 file_off  = ph->p_offset;
        UINT64 remaining = ph->p_filesz;

        while (remaining > 0) {
            UINTN  to_read   = (remaining > chunk_sz) ? chunk_sz : (UINTN)remaining;
            UINTN  did_read  = to_read;

            status = file->SetPosition(file, file_off);
            if (EFI_ERROR(status)) goto done;
            status = file->Read(file, &did_read, chunk_buf);
            if (EFI_ERROR(status)) goto done;

            mem_copy((VOID *)(UINTN)dest_phys, chunk_buf, did_read);

            dest_phys += did_read;
            file_off  += did_read;
            remaining -= did_read;
        }
    }

    status = EFI_SUCCESS;

done:
    bs->FreePool(chunk_buf);

    if (!EFI_ERROR(status)) {
        /* Adjust entry point if we moved the image */
        *entry_out     = ehdr.e_entry + (alloc_addr - phys_base);
        *phys_base_out = alloc_addr;
        *virt_base_out = virt_min;
        *size_out      = total_size;
        console_printf("ELF: loaded OK, entry phys=0x%x virt=0x%x\r\n",
                       *entry_out, ehdr.e_entry);
    }

    return status;
}
