/*
 * initrd.c — CPIO newc initial ramdisk driver
 *
 * Parses a CPIO newc archive from physical memory, builds an in-memory
 * file table, and registers a VFS driver + IO Manager device so user-mode
 * code can open and read files via NtCreateFile / NtReadFile.
 *
 * CPIO newc record layout:
 *
 *   [0]    6 bytes  magic       "070701" (no checksum)
 *   [6]    8 bytes  ino         ASCII hex
 *   [14]   8 bytes  mode        Unix mode (we use bit 14 to detect dirs)
 *   [22]   8 bytes  uid         ASCII hex
 *   [30]   8 bytes  gid         ASCII hex
 *   [38]   8 bytes  nlink       ASCII hex
 *   [46]   8 bytes  mtime       ASCII hex
 *   [54]   8 bytes  filesize    data size in bytes
 *   [62]   8 bytes  devmajor    ASCII hex
 *   [70]   8 bytes  devminor    ASCII hex
 *   [78]   8 bytes  rdevmajor   ASCII hex
 *   [86]   8 bytes  rdevminor   ASCII hex
 *   [94]   8 bytes  namesize    filename length incl. null terminator
 *   [102]  8 bytes  check       0 for "070701"
 *   [110]           filename    namesize bytes, padded to 4-byte boundary
 *                   data        filesize bytes, padded to 4-byte boundary
 *
 * All values are in ASCII hexadecimal (no leading "0x").
 */

#include "initrd.h"
#include "vfs.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../include/types.h"
#include "../io/io.h"

/* -----------------------------------------------------------------------
 * Global InitRD context (single instance)
 * ----------------------------------------------------------------------- */
static INITRD_CONTEXT g_initrd;

/* -----------------------------------------------------------------------
 * CPIO hex parser — parse N ASCII hex digits into a UINT32
 * ----------------------------------------------------------------------- */
static UINT32 cpio_hex(const char *s, int n)
{
    UINT32 v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        UINT32 d;
        if (c >= '0' && c <= '9')      d = (UINT32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (UINT32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (UINT32)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

/* -----------------------------------------------------------------------
 * Path normalization helpers
 * ----------------------------------------------------------------------- */
static int path_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Normalize a path: lowercase, replace '/' with '\\', strip leading sep */
static void normalize_path(char *dst, const char *src, UINT32 cap)
{
    UINT32 i = 0;
    /* Skip leading './' or '/' or '.\\' */
    while (*src == '/' || *src == '\\') src++;
    if (src[0] == '.' && (src[1] == '/' || src[1] == '\\')) src += 2;

    while (*src && i < cap - 1) {
        char c = *src++;
        if (c == '/') c = '\\';
        else c = (char)path_tolower((unsigned char)c);
        dst[i++] = c;
    }
    dst[i] = '\0';
}

/* Case-insensitive path comparison (treats '/' == '\\') */
static bool path_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (char)path_tolower((unsigned char)*a);
        char cb = (char)path_tolower((unsigned char)*b);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* -----------------------------------------------------------------------
 * CPIO archive parser
 * ----------------------------------------------------------------------- */
static void cpio_parse(const UINT8 *buf, UINT64 size)
{
    const UINT8 *p   = buf;
    const UINT8 *end = buf + size;

    g_initrd.FileCount = 0;

    while (p + CPIO_HEADER_SIZE <= end) {
        const char *hdr = (const char *)p;

        /* Validate magic */
        if (hdr[0] != '0' || hdr[1] != '7' || hdr[2] != '0' ||
            hdr[3] != '7' || hdr[4] != '0' ||
            (hdr[5] != '1' && hdr[5] != '2')) {
            break;
        }

        UINT32 mode     = cpio_hex(hdr + 14, 8);
        UINT32 filesize = cpio_hex(hdr + 54, 8);
        UINT32 namesize = cpio_hex(hdr + 94, 8);

        /* Check trailer */
        const char *name_ptr = (const char *)(p + CPIO_HEADER_SIZE);
        if (p + CPIO_HEADER_SIZE + namesize > end) break;

        bool is_trailer = (namesize >= 11 &&
                           __builtin_memcmp(name_ptr, CPIO_TRAILER_NAME, 10) == 0);
        if (is_trailer) break;

        /* Compute aligned offsets:
         * Name starts at CPIO_HEADER_SIZE, padded to next 4-byte boundary
         * from start of record.
         * Data starts after name, also padded to 4-byte boundary from
         * start of record.
         */
        UINT32 name_end_off  = CPIO_HEADER_SIZE + namesize;
        UINT32 data_start_off = (name_end_off + 3) & ~3u;
        UINT32 data_end_off   = data_start_off + filesize;
        UINT32 record_size    = (data_end_off + 3) & ~3u;

        const UINT8 *data_ptr = p + data_start_off;

        if (p + data_start_off + filesize > end) break;

        /* Store entry */
        if (g_initrd.FileCount < INITRD_MAX_FILES) {
            bool is_dir = ((mode & 0xF000) == 0x4000);
            INITRD_FILE *f = &g_initrd.Files[g_initrd.FileCount++];
            normalize_path(f->Path, name_ptr, INITRD_PATH_MAX);
            f->Data  = (void *)data_ptr;
            f->Size  = is_dir ? 0 : filesize;
            f->IsDir = is_dir;
        }

        p += record_size;
    }

    kprintf("[INITRD] Parsed %u entries\n", g_initrd.FileCount);

    /* Print first few entries for diagnostics */
    UINT32 show = g_initrd.FileCount < 8 ? g_initrd.FileCount : 8;
    for (UINT32 i = 0; i < show; i++) {
        INITRD_FILE *f = &g_initrd.Files[i];
        kprintf("[INITRD]   [%u] %s (%s, %llu bytes)\n",
                i, f->Path[0] ? f->Path : "(root)",
                f->IsDir ? "dir" : "file",
                (unsigned long long)f->Size);
    }
}

/* -----------------------------------------------------------------------
 * InitrdLookup
 * ----------------------------------------------------------------------- */
PINITRD_FILE InitrdLookup(const char *RelPath)
{
    if (!RelPath) return NULL;

    char norm[INITRD_PATH_MAX];
    normalize_path(norm, RelPath, INITRD_PATH_MAX);

    for (UINT32 i = 0; i < g_initrd.FileCount; i++) {
        if (path_eq(g_initrd.Files[i].Path, norm))
            return &g_initrd.Files[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * VFS driver implementation for InitRD
 * ----------------------------------------------------------------------- */

/* VFS_NODE embedded in a small allocation */
typedef struct {
    VFS_NODE    Node;   /* Must be first */
    PINITRD_FILE File;  /* Pointer to INITRD_FILE (NULL for root dir) */
    UINT32      DirIndex; /* For directory enumeration */
} INITRD_VFS_NODE, *PINITRD_VFS_NODE;

static NTSTATUS initrd_vfs_open(void *FsPrivate, const char *path, PVFS_NODE *out)
{
    (void)FsPrivate;

    /* Empty path or "." → root directory */
    bool is_root = (!path || *path == '\0' ||
                    (path[0] == '.' && path[1] == '\0'));

    INITRD_FILE *file = NULL;
    bool         is_dir = false;

    if (is_root) {
        is_dir = true;
    } else {
        file = InitrdLookup(path);
        if (!file) return STATUS_OBJECT_NAME_NOT_FOUND;
        is_dir = file->IsDir;
    }

    PINITRD_VFS_NODE n = kzalloc(sizeof(INITRD_VFS_NODE));
    if (!n) return STATUS_NO_MEMORY;

    n->File          = file;
    n->Node.Type     = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    n->Node.Size     = file ? file->Size : 0;
    n->Node.FsData   = n;
    /* Mount is set by VfsOpen caller — left NULL here for now */

    *out = &n->Node;
    return STATUS_SUCCESS;
}

static NTSTATUS initrd_vfs_read(PVFS_NODE node, UINT64 offset,
                                 void *buf, UINT32 len, UINT32 *bytes_read)
{
    PINITRD_VFS_NODE n = (PINITRD_VFS_NODE)node->FsData;
    if (!n || !n->File || n->File->IsDir) return STATUS_INVALID_PARAMETER;

    UINT64 size = n->File->Size;
    if (offset >= size) {
        *bytes_read = 0;
        return STATUS_END_OF_FILE;
    }

    UINT64 avail = size - offset;
    UINT32 to_copy = (len < (UINT32)avail) ? len : (UINT32)avail;
    __builtin_memcpy(buf, (UINT8 *)n->File->Data + offset, to_copy);
    *bytes_read = to_copy;
    return STATUS_SUCCESS;
}

static NTSTATUS initrd_vfs_readdir(PVFS_NODE node, UINT32 index,
                                    char *name, UINT32 name_len,
                                    VFS_STAT *stat)
{
    (void)node;
    /* Enumerate all files in the archive */
    if (index >= g_initrd.FileCount) return STATUS_NO_MORE_ENTRIES;

    INITRD_FILE *f = &g_initrd.Files[index];

    /* Copy basename */
    const char *src = f->Path;
    /* Find last '\\' */
    const char *last_sep = src;
    for (const char *c = src; *c; c++) {
        if (*c == '\\') last_sep = c + 1;
    }

    UINT32 i = 0;
    while (*last_sep && i < name_len - 1)
        name[i++] = *last_sep++;
    name[i] = '\0';

    if (stat) {
        stat->Type = f->IsDir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        stat->Size = f->Size;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS initrd_vfs_stat(PVFS_NODE node, VFS_STAT *stat)
{
    PINITRD_VFS_NODE n = (PINITRD_VFS_NODE)node->FsData;
    if (!stat) return STATUS_INVALID_PARAMETER;
    stat->Type = node->Type;
    stat->Size = n && n->File ? n->File->Size : 0;
    return STATUS_SUCCESS;
}

static void initrd_vfs_close(PVFS_NODE node)
{
    PINITRD_VFS_NODE n = (PINITRD_VFS_NODE)node->FsData;
    if (n) kfree(n);
}

static const VFS_OPS g_initrd_vfs_ops = {
    .Open    = initrd_vfs_open,
    .Read    = initrd_vfs_read,
    .ReadDir = initrd_vfs_readdir,
    .Stat    = initrd_vfs_stat,
    .Close   = initrd_vfs_close,
};

/* -----------------------------------------------------------------------
 * IO Manager IRP handlers for \Device\InitRD
 * ----------------------------------------------------------------------- */
static NTSTATUS initrd_irp_create(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    PIO_STACK_LOCATION sl = &Irp->StackLocation;

    /* Extract file name from the IRP stack location */
    const char *path = "";
    if (sl->Parameters.Create.FileName) {
        /* FileName is stored as a UNICODE_STRING; in Phase 4 we use
         * ASCII paths internally via the narrow string in Buffer */
        UNICODE_STRING *un = sl->Parameters.Create.FileName;
        if (un && un->Buffer)
            path = (const char *)un->Buffer;
    }

    /* Look up the file in the InitRD */
    PINITRD_FILE file = (*path == '\0') ? NULL : InitrdLookup(path);
    bool is_root = (*path == '\0');

    if (!is_root && !file) {
        Irp->IoStatus.Status      = STATUS_OBJECT_NAME_NOT_FOUND;
        Irp->IoStatus.Information = 0;
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* Store INITRD_FILE* in the FILE_OBJECT's FsContext */
    PFILE_OBJECT fo = sl->Parameters.Create.FileObject;
    if (fo) fo->FsContext = (void *)file;

    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static NTSTATUS initrd_irp_read(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    PIO_STACK_LOCATION sl = &Irp->StackLocation;
    PFILE_OBJECT fo = sl->Parameters.Read.FileObject;

    if (!fo) {
        Irp->IoStatus.Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    PINITRD_FILE file = (PINITRD_FILE)fo->FsContext;
    if (!file || file->IsDir) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    UINT64 offset  = sl->Parameters.Read.ByteOffset;
    UINT32 req_len = sl->Parameters.Read.Length;
    void  *buf     = Irp->SystemBuffer;

    if (!buf || !req_len) {
        Irp->IoStatus.Status      = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    UINT64 size = file->Size;
    if (offset >= size) {
        Irp->IoStatus.Status      = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
        return STATUS_END_OF_FILE;
    }

    UINT64 avail   = size - offset;
    UINT32 to_copy = (req_len < (UINT32)avail) ? req_len : (UINT32)avail;
    __builtin_memcpy(buf, (UINT8 *)file->Data + offset, to_copy);

    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = to_copy;
    return STATUS_SUCCESS;
}

static NTSTATUS initrd_irp_query_info(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    PIO_STACK_LOCATION sl = &Irp->StackLocation;
    PFILE_OBJECT fo = sl->Parameters.QueryFile.FileObject;

    if (!fo) {
        Irp->IoStatus.Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    PINITRD_FILE file = (PINITRD_FILE)fo->FsContext;
    FILE_STANDARD_INFORMATION *info =
        (FILE_STANDARD_INFORMATION *)sl->Parameters.QueryFile.Buffer;

    if (!info || sl->Parameters.QueryFile.Length < sizeof(*info)) {
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        return STATUS_BUFFER_TOO_SMALL;
    }

    __builtin_memset(info, 0, sizeof(*info));
    if (file) {
        info->EndOfFile  = file->Size;
        info->AllocationSize = (file->Size + 511) & ~511ULL;
        info->Directory  = file->IsDir;
    }

    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = sizeof(*info);
    return STATUS_SUCCESS;
}

static NTSTATUS initrd_irp_not_supported(PDEVICE_OBJECT d, PIRP Irp)
{
    (void)d;
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* -----------------------------------------------------------------------
 * InitrdMount — entry point called from KiSystemStartup
 * ----------------------------------------------------------------------- */
void InitrdMount(void *Data, UINT64 Size)
{
    if (!Data || Size < CPIO_HEADER_SIZE) {
        kprintf("[INITRD] No valid ramdisk (size=%llu)\n",
                (unsigned long long)Size);
        return;
    }

    kprintf("[INITRD] Parsing CPIO archive at %p (%llu bytes)\n",
            Data, (unsigned long long)Size);

    __builtin_memset(&g_initrd, 0, sizeof(g_initrd));
    cpio_parse((const UINT8 *)Data, Size);

    /* Register VFS driver */
    VfsMount("\\Device\\InitRD", &g_initrd_vfs_ops, &g_initrd);

    /* Create the IO Manager device \Device\InitRD */
    static DRIVER_OBJECT initrd_driver;
    __builtin_memset(&initrd_driver, 0, sizeof(initrd_driver));

    /* Wire IRP major functions */
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        initrd_driver.MajorFunction[i] = initrd_irp_not_supported;
    initrd_driver.MajorFunction[IRP_MJ_CREATE]                  = initrd_irp_create;
    initrd_driver.MajorFunction[IRP_MJ_READ]                    = initrd_irp_read;
    initrd_driver.MajorFunction[IRP_MJ_QUERY_INFORMATION]       = initrd_irp_query_info;

    /* Device name as proper UNICODE_STRING (UTF-16LE) */
    static WCHAR dev_name_buf[] = L"\\Device\\InitRD";
    static UNICODE_STRING dev_name;
    dev_name.Buffer  = dev_name_buf;
    dev_name.Length  = (USHORT)(sizeof(dev_name_buf) - sizeof(WCHAR));
    dev_name.MaximumLength = (USHORT)sizeof(dev_name_buf);

    PDEVICE_OBJECT initrd_device = NULL;
    NTSTATUS s = IoCreateDevice(&initrd_driver, 0, &dev_name,
                                 0 /* FILE_DEVICE_DISK */,
                                 0, false, &initrd_device);
    if (!NT_SUCCESS(s)) {
        kprintf("[INITRD] IoCreateDevice failed: 0x%08x\n", s);
        return;
    }

    kprintf("[INITRD] Device \\Device\\InitRD created (%u files)\n",
            g_initrd.FileCount);
}
