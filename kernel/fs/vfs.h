/*
 * vfs.h — Virtual File System (VFS) layer
 *
 * The VFS provides a uniform interface for kernel code and the IO Manager
 * to access files regardless of the underlying filesystem.
 *
 * Architecture overview:
 *
 *   NtCreateFile("\??\C:\Windows\notepad.exe")
 *       ↓ IoCreateFile (io.c)
 *       ↓ VfsResolveDevice("\??\C:")  → "\Device\InitRD"
 *       ↓ IRP_MJ_CREATE to \Device\InitRD driver
 *       ↓ InitRD driver: VfsOpen("\Windows\notepad.exe")
 *       ↓ Returns INITRD_FILE* in FILE_OBJECT.FsContext
 *   NtReadFile(handle, ...)
 *       ↓ IoReadFile → IRP_MJ_READ → InitRD driver
 *       ↓ Copies bytes from INITRD_FILE.Data
 *
 * Path namespace aliases (resolved in IoCreateFile):
 *   \??\C:        → \Device\InitRD
 *   \??\D:        → \Device\InitRD
 *   \SystemRoot   → \Device\InitRD
 *   \DosDevices\C:→ \Device\InitRD
 *
 * Phase 4 scope:
 *   - Read-only filesystems only.
 *   - Single mount point backed by InitRD (CPIO newc archive).
 *   - No directory creation, no write support.
 *   - No ACLs on VFS nodes (SE access checks happen at IoCreateFile level).
 */

#pragma once

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * VFS node types
 * ----------------------------------------------------------------------- */
#define VFS_TYPE_FILE   0
#define VFS_TYPE_DIR    1

/* -----------------------------------------------------------------------
 * VFS_STAT — metadata for a file or directory node
 * ----------------------------------------------------------------------- */
typedef struct _VFS_STAT {
    UINT32  Type;       /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    UINT64  Size;       /* File size in bytes (0 for dirs) */
} VFS_STAT, *PVFS_STAT;

/* -----------------------------------------------------------------------
 * VFS_NODE — an open file or directory handle
 *
 * Created by VFS_OPS.Open(), released by VFS_OPS.Close().
 * Embedded or referenced from FILE_OBJECT.FsContext.
 * ----------------------------------------------------------------------- */
typedef struct _VFS_NODE VFS_NODE, *PVFS_NODE;

/* -----------------------------------------------------------------------
 * VFS_OPS — filesystem operations table (registered by each FS driver)
 * ----------------------------------------------------------------------- */
typedef struct _VFS_OPS {
    /*
     * Open a path relative to this mount.
     * @path: null-terminated, uses '\' or '/' as separator.
     * Returns STATUS_SUCCESS and sets *out on success;
     * STATUS_OBJECT_NAME_NOT_FOUND if not found.
     */
    NTSTATUS (*Open)(void *FsPrivate, const char *path, PVFS_NODE *out);

    /*
     * Read bytes from an open file node.
     * @offset: byte offset within file.
     * @buf: kernel buffer to read into.
     * @len: number of bytes requested.
     * @bytes_read: receives actual bytes transferred.
     */
    NTSTATUS (*Read)(PVFS_NODE node, UINT64 offset,
                     void *buf, UINT32 len, UINT32 *bytes_read);

    /*
     * Enumerate directory entries.
     * @index: 0-based entry index.
     * @name: output buffer for entry name (null-terminated).
     * @name_len: capacity of name buffer.
     * @stat: receives metadata for this entry.
     * Returns STATUS_NO_MORE_ENTRIES when index is out of range.
     */
    NTSTATUS (*ReadDir)(PVFS_NODE node, UINT32 index,
                        char *name, UINT32 name_len, VFS_STAT *stat);

    /* Stat an open node */
    NTSTATUS (*Stat)(PVFS_NODE node, VFS_STAT *stat);

    /* Release an open node */
    void     (*Close)(PVFS_NODE node);
} VFS_OPS, *PVFS_OPS;

/* -----------------------------------------------------------------------
 * VFS_MOUNT — a registered filesystem mount
 * ----------------------------------------------------------------------- */
#define VFS_MAX_MOUNTS      8
#define VFS_DEVICE_PATH_MAX 64

typedef struct _VFS_MOUNT {
    bool        Active;
    char        DevicePath[VFS_DEVICE_PATH_MAX];  /* e.g. "\Device\InitRD" */
    const VFS_OPS *Ops;
    void       *FsPrivate;  /* FS driver private context */
} VFS_MOUNT, *PVFS_MOUNT;

/* -----------------------------------------------------------------------
 * VFS_NODE definition
 * Allocated by the FS driver (typically kzalloc), freed on Close.
 * ----------------------------------------------------------------------- */
struct _VFS_NODE {
    UINT32       Type;      /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    UINT64       Size;      /* File size in bytes */
    void        *FsData;    /* FS-private per-node data */
    VFS_MOUNT   *Mount;     /* Back-pointer to owning mount */
};

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize the VFS layer (empty mount table) */
void VfsInitialize(void);

/*
 * Register a filesystem at a device path.
 * @DevicePath: NT device path, e.g. "\Device\InitRD".
 * @Ops:        Filesystem operations.
 * @FsPrivate:  Filesystem driver context (passed to all Ops calls).
 */
NTSTATUS VfsMount(const char *DevicePath, const VFS_OPS *Ops, void *FsPrivate);

/*
 * Resolve a DOS/NT path prefix to a VFS mount device path.
 * Handles:
 *   "\??\C:\"     → "\Device\InitRD"
 *   "\SystemRoot\" → "\Device\InitRD"
 *   "\Device\*"    → pass-through (no translation)
 *
 * @FullPath:    Input NT/DOS path.
 * @DeviceOut:   Output: pointer to device path within the mount table.
 * @RelPathOut:  Output: pointer into FullPath after the device prefix.
 * Returns STATUS_SUCCESS if a mount was found, STATUS_OBJECT_NAME_NOT_FOUND otherwise.
 */
NTSTATUS VfsResolveMount(const char *FullPath,
                          VFS_MOUNT **MountOut,
                          const char **RelPathOut);

/*
 * Open a file by full NT/DOS path.
 * Resolves the device prefix, then calls VFS_OPS.Open with the remainder.
 */
NTSTATUS VfsOpen(const char *FullPath, PVFS_NODE *NodeOut);

/* Wrappers forwarding to node's VFS_OPS */
NTSTATUS VfsRead(PVFS_NODE node, UINT64 offset,
                  void *buf, UINT32 len, UINT32 *bytes_read);
NTSTATUS VfsReadDir(PVFS_NODE node, UINT32 index,
                     char *name, UINT32 name_len, VFS_STAT *stat);
NTSTATUS VfsStat(PVFS_NODE node, VFS_STAT *stat);
void     VfsClose(PVFS_NODE node);
