/*
 * initrd.h — In-memory initial ramdisk (CPIO newc format)
 *
 * The bootloader loads a CPIO archive into physical memory and passes its
 * base/size in the BootInfo struct.  The kernel mounts this as a read-only
 * VFS filesystem at "\Device\InitRD".
 *
 * CPIO newc format (magic "070701"):
 *   Each entry: 110-byte ASCII header + filename (namesize bytes, padded
 *   to 4-byte boundary) + file data (filesize bytes, padded to 4-byte).
 *   The archive ends with an entry named "TRAILER!!!".
 *
 * Phase 4 provides:
 *   - Parsing of the CPIO archive into an in-memory file table.
 *   - VFS driver registered at "\Device\InitRD".
 *   - IO Manager integration: InitRD device handles IRP_MJ_CREATE and
 *     IRP_MJ_READ so that NtCreateFile / NtReadFile can access files.
 *   - NT namespace aliases: "\??\C:\", "\SystemRoot\" → "\Device\InitRD"
 *     are handled by the VFS layer.
 *
 * Constraints:
 *   - Read-only (no write support).
 *   - Maximum INITRD_MAX_FILES files.
 *   - File paths are stored as lowercase for case-insensitive matching.
 */

#pragma once

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Limits
 * ----------------------------------------------------------------------- */
#define INITRD_MAX_FILES    512
#define INITRD_PATH_MAX     256

/* -----------------------------------------------------------------------
 * CPIO newc header (110 bytes, ASCII hex fields)
 * ----------------------------------------------------------------------- */
#define CPIO_NEWC_MAGIC     "070701"
#define CPIO_NEWC_MAGIC_CRC "070702"
#define CPIO_TRAILER_NAME   "TRAILER!!!"
#define CPIO_HEADER_SIZE    110

/* -----------------------------------------------------------------------
 * INITRD_FILE — one parsed file entry
 * ----------------------------------------------------------------------- */
typedef struct {
    char    Path[INITRD_PATH_MAX];  /* Relative path, '\'-separated, lowercase */
    void   *Data;                   /* Pointer into the physmap-mapped initrd */
    UINT64  Size;                   /* File data size in bytes */
    bool    IsDir;
} INITRD_FILE, *PINITRD_FILE;

/* -----------------------------------------------------------------------
 * INITRD_CONTEXT — private state for the InitRD VFS driver
 * ----------------------------------------------------------------------- */
typedef struct {
    INITRD_FILE Files[INITRD_MAX_FILES];
    UINT32      FileCount;
} INITRD_CONTEXT, *PINITRD_CONTEXT;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * Parse the CPIO archive at @Data (size @Size bytes) and register it as
 * the VFS driver at "\Device\InitRD".  Also creates the IO Manager device
 * so that NtCreateFile can open files.
 *
 * @Data: kernel virtual address of the initrd buffer (physmap-mapped).
 * @Size: size of the buffer in bytes.
 *
 * Called from KiSystemStartup after VfsInitialize().
 * Safe to call with Size == 0 (logs a warning and returns).
 */
void InitrdMount(void *Data, UINT64 Size);

/*
 * Look up a file by path (case-insensitive, '\\' or '/' separators).
 * Returns a pointer to the static INITRD_FILE entry, or NULL if not found.
 * The returned pointer is valid for the lifetime of the kernel.
 */
PINITRD_FILE InitrdLookup(const char *RelPath);
