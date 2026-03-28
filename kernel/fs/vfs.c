/*
 * vfs.c — Virtual File System layer implementation
 *
 * Maintains a flat mount table keyed by device path.  Path resolution
 * handles the common NT/DOS namespace prefixes used by user-mode code:
 *
 *   \??\C:\          → first registered mount ("C: drive")
 *   \??\D:\          → first registered mount (same; only one volume)
 *   \SystemRoot\     → first registered mount
 *   \DosDevices\C:\  → first registered mount
 *   \Device\InitRD\  → direct mount by device path
 *
 * Phase 4 keeps a fixed-size mount table (VFS_MAX_MOUNTS entries).
 */

#include "vfs.h"
#include "../ke/printf.h"
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Global mount table
 * ----------------------------------------------------------------------- */
static VFS_MOUNT vfs_mounts[VFS_MAX_MOUNTS];
static int       vfs_mount_count = 0;

/* -----------------------------------------------------------------------
 * Path alias table — maps NT/DOS device prefixes to device paths
 * The first matching prefix wins.
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *Prefix;
    const char *DevicePath;   /* NULL = use first registered mount */
} VFS_ALIAS;

static const VFS_ALIAS vfs_aliases[] = {
    { "\\??\\C:\\",          NULL },   /* C: drive → first mount */
    { "\\??\\C:",            NULL },
    { "\\??\\D:\\",          NULL },
    { "\\??\\D:",            NULL },
    { "\\DosDevices\\C:\\",  NULL },
    { "\\DosDevices\\C:",    NULL },
    { "\\SystemRoot\\",      NULL },
    { "\\SystemRoot",        NULL },
};
#define VFS_ALIAS_COUNT (sizeof(vfs_aliases) / sizeof(vfs_aliases[0]))

/* -----------------------------------------------------------------------
 * Case-insensitive string helpers for path comparison
 * ----------------------------------------------------------------------- */
static int path_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool path_prefix_eq(const char *str, const char *prefix)
{
    while (*prefix) {
        /* Treat '\\' and '/' as equivalent separators */
        char s = (char)path_tolower((unsigned char)*str);
        char p = (char)path_tolower((unsigned char)*prefix);
        if ((s == '/' ? '\\' : s) != (p == '/' ? '\\' : p))
            return false;
        str++; prefix++;
    }
    return true;
}

static bool path_eq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (char)path_tolower((unsigned char)*a);
        char cb = (char)path_tolower((unsigned char)*b);
        if ((ca == '/' ? '\\' : ca) != (cb == '/' ? '\\' : cb))
            return false;
        a++; b++;
    }
    return *a == *b;
}

/* -----------------------------------------------------------------------
 * VfsInitialize
 * ----------------------------------------------------------------------- */
void VfsInitialize(void)
{
    __builtin_memset(vfs_mounts, 0, sizeof(vfs_mounts));
    vfs_mount_count = 0;
    kprintf("[VFS] Virtual File System initialized (%d mount slots)\n",
            VFS_MAX_MOUNTS);
}

/* -----------------------------------------------------------------------
 * VfsMount
 * ----------------------------------------------------------------------- */
NTSTATUS VfsMount(const char *DevicePath, const VFS_OPS *Ops, void *FsPrivate)
{
    if (vfs_mount_count >= VFS_MAX_MOUNTS) {
        kprintf("[VFS] Mount table full\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!DevicePath || !Ops) return STATUS_INVALID_PARAMETER;

    VFS_MOUNT *m = &vfs_mounts[vfs_mount_count++];
    m->Active    = true;
    m->Ops       = Ops;
    m->FsPrivate = FsPrivate;

    /* Copy device path, truncate if too long */
    UINT32 i = 0;
    while (DevicePath[i] && i < VFS_DEVICE_PATH_MAX - 1) {
        m->DevicePath[i] = DevicePath[i];
        i++;
    }
    m->DevicePath[i] = '\0';

    kprintf("[VFS] Mounted '%s'\n", m->DevicePath);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VfsResolveMount
 *
 * Two-stage resolution:
 *   1. Check explicit device path: does path start with a mounted device?
 *   2. Check alias table: does path start with a known alias prefix?
 * ----------------------------------------------------------------------- */
NTSTATUS VfsResolveMount(const char *FullPath,
                          VFS_MOUNT **MountOut,
                          const char **RelPathOut)
{
    if (!FullPath || !MountOut || !RelPathOut)
        return STATUS_INVALID_PARAMETER;

    /* Stage 1: match against registered device paths */
    for (int i = 0; i < vfs_mount_count; i++) {
        VFS_MOUNT *m = &vfs_mounts[i];
        if (!m->Active) continue;

        size_t dev_len = 0;
        while (m->DevicePath[dev_len]) dev_len++;

        if (path_prefix_eq(FullPath, m->DevicePath)) {
            const char *rel = FullPath + dev_len;
            /* Skip a leading separator if present */
            if (*rel == '\\' || *rel == '/') rel++;
            *MountOut   = m;
            *RelPathOut = rel;
            return STATUS_SUCCESS;
        }
    }

    /* Stage 2: match against alias table */
    for (UINT32 a = 0; a < VFS_ALIAS_COUNT; a++) {
        const VFS_ALIAS *alias = &vfs_aliases[a];
        size_t prefix_len = 0;
        while (alias->Prefix[prefix_len]) prefix_len++;

        if (path_prefix_eq(FullPath, alias->Prefix)) {
            /* Find the target device */
            VFS_MOUNT *target = NULL;
            if (alias->DevicePath) {
                /* Look up specific device */
                for (int i = 0; i < vfs_mount_count; i++) {
                    if (vfs_mounts[i].Active &&
                        path_eq_nocase(vfs_mounts[i].DevicePath, alias->DevicePath)) {
                        target = &vfs_mounts[i];
                        break;
                    }
                }
            } else {
                /* Use first registered mount */
                if (vfs_mount_count > 0 && vfs_mounts[0].Active)
                    target = &vfs_mounts[0];
            }

            if (!target) return STATUS_OBJECT_NAME_NOT_FOUND;

            const char *rel = FullPath + prefix_len;
            /* Skip a leading separator */
            if (*rel == '\\' || *rel == '/') rel++;
            *MountOut   = target;
            *RelPathOut = rel;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* -----------------------------------------------------------------------
 * VfsOpen
 * ----------------------------------------------------------------------- */
NTSTATUS VfsOpen(const char *FullPath, PVFS_NODE *NodeOut)
{
    if (!FullPath || !NodeOut) return STATUS_INVALID_PARAMETER;

    VFS_MOUNT  *mount  = NULL;
    const char *relpath = NULL;

    NTSTATUS s = VfsResolveMount(FullPath, &mount, &relpath);
    if (!NT_SUCCESS(s)) return s;

    return mount->Ops->Open(mount->FsPrivate, relpath, NodeOut);
}

/* -----------------------------------------------------------------------
 * VfsRead / VfsReadDir / VfsStat / VfsClose
 * ----------------------------------------------------------------------- */
NTSTATUS VfsRead(PVFS_NODE node, UINT64 offset,
                  void *buf, UINT32 len, UINT32 *bytes_read)
{
    if (!node || !node->Mount || !node->Mount->Ops->Read)
        return STATUS_INVALID_HANDLE;
    return node->Mount->Ops->Read(node, offset, buf, len, bytes_read);
}

NTSTATUS VfsReadDir(PVFS_NODE node, UINT32 index,
                     char *name, UINT32 name_len, VFS_STAT *stat)
{
    if (!node || !node->Mount || !node->Mount->Ops->ReadDir)
        return STATUS_NOT_IMPLEMENTED;
    return node->Mount->Ops->ReadDir(node, index, name, name_len, stat);
}

NTSTATUS VfsStat(PVFS_NODE node, VFS_STAT *stat)
{
    if (!node || !node->Mount || !node->Mount->Ops->Stat)
        return STATUS_INVALID_HANDLE;
    return node->Mount->Ops->Stat(node, stat);
}

void VfsClose(PVFS_NODE node)
{
    if (!node) return;
    if (node->Mount && node->Mount->Ops->Close)
        node->Mount->Ops->Close(node);
}
