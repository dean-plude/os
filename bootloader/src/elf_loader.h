/*
 * elf_loader.h — ELF64 loader interface for the UEFI bootloader
 */

#pragma once

#include "../include/efi.h"

/*
 * Load a 64-bit ELF kernel image from an open EFI file handle.
 *
 * Parameters:
 *   file          — open EFI_FILE_PROTOCOL handle positioned at byte 0
 *   file_size     — total file size in bytes
 *   entry_out     — receives the kernel virtual entry point address
 *   phys_base_out — receives the lowest physical address allocated for the kernel
 *   virt_base_out — receives the lowest virtual (linked) address
 *   size_out      — receives total byte size of all allocated kernel pages
 *   bs            — EFI boot services pointer (used for AllocatePages)
 *
 * On success returns EFI_SUCCESS and fills the output parameters.
 * On failure returns an EFI error code; partial allocations are NOT freed
 * (the bootloader will simply fail and the system resets).
 */
EFI_STATUS elf_load(
    EFI_FILE_PROTOCOL *file,
    UINTN              file_size,
    UINT64            *entry_out,
    UINT64            *phys_base_out,
    UINT64            *virt_base_out,
    UINT64            *size_out,
    EFI_BOOT_SERVICES *bs);
