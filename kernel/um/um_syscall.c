/*
 * um_syscall.c — NT system services for user-mode programs
 *
 * The real NT signatures (Windows 10 1903 numbering, see ke/syscall.h):
 * arguments 1–4 arrive in registers, 5+ on the user stack.  Every user
 * pointer goes through CopyFromUser/CopyToUser.  Services here replace the
 * legacy handlers for the same numbers when called from a kernel/um
 * process; kernel threads still get the legacy ones.
 *
 * Files live on drive C: (ramfs).  Paths may be NT ("\??\C:\x"), DOS
 * ("C:\x") or relative to the process's current directory; "CONIN$",
 * "CONOUT$" and "CON" name the console.
 */

#include "um_internal.h"
#include "../ke/syscall.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"

#define ST_SUCCESS                 0x00000000u
#define ST_PENDING                 0x00000103u
#define ST_BUFFER_OVERFLOW         0x80000005u
#define ST_NO_MORE_FILES           0x80000006u
#define ST_NOT_IMPLEMENTED         0xC0000002u
#define ST_INVALID_INFO_CLASS      0xC0000003u
#define ST_INFO_LENGTH_MISMATCH    0xC0000004u
#define ST_INVALID_HANDLE          0xC0000008u
#define ST_INVALID_PARAMETER       0xC000000Du
#define ST_END_OF_FILE             0xC0000011u
#define ST_NO_MEMORY               0xC0000017u
#define ST_CONFLICTING_ADDRESSES   0xC0000018u
#define ST_ACCESS_DENIED           0xC0000022u
#define ST_OBJECT_NAME_INVALID     0xC0000033u
#define ST_OBJECT_NAME_NOT_FOUND   0xC0000034u
#define ST_OBJECT_NAME_COLLISION   0xC0000035u
#define ST_OBJECT_PATH_NOT_FOUND   0xC000003Au
#define ST_DISK_FULL               0xC000007Fu
#define ST_MEMORY_NOT_ALLOCATED    0xC00000A0u
#define ST_FILE_IS_A_DIRECTORY     0xC00000BAu
#define ST_DIRECTORY_NOT_EMPTY     0xC0000101u
#define ST_NOT_A_DIRECTORY         0xC0000103u
#define ST_TOO_MANY_OPENED_FILES   0xC000011Fu
#define ST_CANNOT_DELETE           0xC0000121u

#define PROC_MEM_LIMIT_PAGES       (256u * 1024u * 1024u / PAGE_SIZE)   /* 256 MiB */

static SYSCALL_HANDLER g_um[SYSCALL_MAX];       /* the services open to programs */

bool UmSyscallAllowed(UINT64 num)
{
    return num < SYSCALL_MAX && g_um[num];
}

UINT64 UmSyscall(UINT64 num, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    return g_um[num](a1, a2, a3, a4);
}

/* -----------------------------------------------------------------------
 * Argument and user-memory helpers
 * ----------------------------------------------------------------------- */
UINT64 um_stack_arg(int n)                         /* n >= 5 */
{
    UINT64 v = 0;
    CopyFromUser(&v, (const void *)(uintptr_t)(sched_current()->user_rsp + 0x28 + 8 * (UINT64)(n - 5)), 8);
    return v;
}

static bool get_u64(UINT64 ptr, UINT64 *v)
{
    return NT_SUCCESS(CopyFromUser(v, (const void *)(uintptr_t)ptr, 8));
}

static bool put_u64(UINT64 ptr, UINT64 v)
{
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &v, 8));
}

static UINT32 iosb(UINT64 ptr, UINT32 status, UINT64 info)
{
    UINT64 blk[2] = { status, info };
    if (ptr && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, blk, sizeof(blk))))
        return UM_STATUS_ACCESS_VIOLATION;
    return status;
}

static UmHandle *handle(UmProcess *p, UINT64 h)
{
    if (h < 4 || (h & 3)) return NULL;
    UINT64 i = h / 4 - 1;
    if (i >= UM_MAX_HANDLES || p->handles[i].kind == H_FREE) return NULL;
    return &p->handles[i];
}

/* Reserve a free slot (kind set by the caller before unlocking). */
static UINT64 handle_alloc(UmProcess *p, UmHandle **out)
{
    for (int i = 0; i < UM_MAX_HANDLES; i++) {
        if (p->handles[i].kind == H_FREE) {
            memset(&p->handles[i], 0, sizeof(UmHandle));
            *out = &p->handles[i];
            return (UINT64)(i + 1) * 4;
        }
    }
    return 0;
}

static void handle_close(UmHandle *h)
{
    if (h->kind == H_FILE || h->kind == H_DIR) {
        DesktopLock();
        RamNode *n = h->node;
        bool del = h->delete_on_close;
        h->kind = H_FREE;
        RamfsUnref(n);
        if (del) RamfsDelete(n);
        DesktopUnlock();
        return;
    }
    if (h->kind == H_OBJECT) {
        UmObject *o = h->obj;
        h->kind = H_FREE;
        h->obj = NULL;
        um_ob_unref(o);
        return;
    }
    h->kind = H_FREE;
}

void um_close_all_handles(UmProcess *p)
{
    for (int i = 0; i < UM_MAX_HANDLES; i++)
        if (p->handles[i].kind != H_FREE) handle_close(&p->handles[i]);
}

UINT64 um_handle_new_object(UmProcess *p, UmObject *o)
{
    UmHandle *h;
    um_lock(&p->lock);
    UINT64 hv = handle_alloc(p, &h);
    if (hv) {
        h->kind = H_OBJECT;
        h->obj = um_ob_ref(o);
    }
    um_unlock(&p->lock);
    return hv;
}

/* The object behind @h (with a reference the caller drops), or NULL.
 * NtCurrentThread() (-2) names the calling thread; type 0 matches any. */
UmObject *um_handle_object(UmProcess *p, UINT64 hv, UmObType type)
{
    UmObject *o = NULL;
    if (hv == UINT64_C(0xFFFFFFFFFFFFFFFE)) {
        o = &UmCurrentThread()->ob;
        return (!type || type == UO_THREAD) ? um_ob_ref(o) : NULL;
    }
    um_lock(&p->lock);
    UmHandle *h = handle(p, hv);
    if (h && h->kind == H_OBJECT && (!type || h->obj->type == type)) o = um_ob_ref(h->obj);
    um_unlock(&p->lock);
    return o;
}

/* Read the path out of OBJECT_ATTRIBUTES (UTF-16 → ASCII, NT prefix
 * removed).  Returns the base directory for relative paths via *root. */
static UINT32 get_path(UmProcess *p, UINT64 oa_ptr, char *out, int cap, RamNode **root)
{
    UINT64 oa[6];
    if (!oa_ptr || !NT_SUCCESS(CopyFromUser(oa, (const void *)(uintptr_t)oa_ptr, sizeof(oa))))
        return UM_STATUS_ACCESS_VIOLATION;
    UINT64 us[2];
    if (!oa[2] || !NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)oa[2], sizeof(us))))
        return ST_OBJECT_NAME_INVALID;
    UINT32 len = (UINT32)(us[0] & 0xFFFF) / 2;
    if (len >= (UINT32)cap) return ST_OBJECT_NAME_INVALID;
    UINT16 w[RAMFS_PATH_MAX];
    if (len > RAMFS_PATH_MAX || !NT_SUCCESS(CopyFromUser(w, (const void *)(uintptr_t)us[1], 2 * len)))
        return UM_STATUS_ACCESS_VIOLATION;
    int n = 0;
    for (UINT32 i = 0; i < len; i++) out[n++] = w[i] < 0x80 ? (char)w[i] : '?';
    out[n] = '\0';
    char *s = out;
    if (!strncmp(s, "\\??\\", 4) || !strncmp(s, "\\\\?\\", 4) || !strncmp(s, "\\\\.\\", 4)) s += 4;
    if (s != out) memmove(out, s, strlen(s) + 1);

    *root = p->cwd;
    if (oa[1]) {                                        /* RootDirectory handle */
        UmHandle *h = handle(p, oa[1]);
        if (!h || h->kind != H_DIR) return ST_INVALID_HANDLE;
        *root = h->node;
    }
    if (((out[0] | 0x20) >= 'a' && (out[0] | 0x20) <= 'z') && out[1] == ':' &&
        (out[0] | 0x20) != 'c')
        return ST_OBJECT_PATH_NOT_FOUND;                /* only drive C: exists */
    return ST_SUCCESS;
}

static bool is_console_name(const char *s, UmHandleKind *kind)
{
    if (!strcmp(s, "CONIN$") || !strcmp(s, "conin$")) { *kind = H_CON_IN;  return true; }
    if (!strcmp(s, "CONOUT$") || !strcmp(s, "conout$") ||
        !strcmp(s, "CON") || !strcmp(s, "con")) { *kind = H_CON_OUT; return true; }
    return false;
}

/* Split "a\b\c.txt" into the parent directory node and the last name. */
static RamNode *parent_of(RamNode *root, char *path, const char **leaf)
{
    char *slash = NULL;
    for (char *c = path; *c; c++) if (*c == '\\' || *c == '/') slash = c;
    if (!slash) { *leaf = path; return root; }
    *leaf = slash + 1;
    if (slash == path || (slash == path + 2 && path[1] == ':')) {
        return RamfsResolve(NULL, "\\");
    }
    *slash = '\0';
    RamNode *dir = RamfsResolve(root, path);
    *slash = '\\';
    return dir;
}

/* -----------------------------------------------------------------------
 * Files
 * ----------------------------------------------------------------------- */
#ifndef GENERIC_READ
#define GENERIC_READ      0x80000000u
#define GENERIC_WRITE     0x40000000u
#define GENERIC_ALL       0x10000000u
#endif
#define FILE_READ_DATA    0x0001u
#define FILE_WRITE_DATA   0x0002u
#define FILE_APPEND_DATA  0x0004u

static UINT32 open_file(UINT64 handle_ptr, UINT32 access, UINT64 oa_ptr, UINT64 iosb_ptr,
                        UINT32 disposition, UINT32 options)
{
    UmProcess *p = UmCurrent();
    char path[RAMFS_PATH_MAX];
    RamNode *root;
    UINT32 st = get_path(p, oa_ptr, path, sizeof(path), &root);
    if (st) return iosb(iosb_ptr, st, 0);

    UmHandleKind ck;
    UmHandle *h;
    if (is_console_name(path, &ck)) {
        um_lock(&p->lock);
        UINT64 hv = handle_alloc(p, &h);
        if (hv) h->kind = ck;
        um_unlock(&p->lock);
        if (!hv) return iosb(iosb_ptr, ST_TOO_MANY_OPENED_FILES, 0);
        if (!put_u64(handle_ptr, hv)) { h->kind = H_FREE; return UM_STATUS_ACCESS_VIOLATION; }
        return iosb(iosb_ptr, ST_SUCCESS, 1);
    }

    bool want_dir = options & 0x1, want_file = options & 0x40;
    bool rd = access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA);
    bool wr = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
    bool append = (access & FILE_APPEND_DATA) && !(access & (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL));

    DesktopLock();
    RamNode *node = path[0] ? RamfsResolve(root, path) : root;
    UINT64 info = 1;                                            /* FILE_OPENED */
    if (node) {
        if (disposition == 2) { DesktopUnlock(); return iosb(iosb_ptr, ST_OBJECT_NAME_COLLISION, 4); }
        if (want_file && node->dir) { DesktopUnlock(); return iosb(iosb_ptr, ST_FILE_IS_A_DIRECTORY, 0); }
        if (want_dir && !node->dir) { DesktopUnlock(); return iosb(iosb_ptr, ST_NOT_A_DIRECTORY, 0); }
        if (!node->dir && (disposition == 0 || disposition == 4 || disposition == 5)) {
            RamfsResize(node, 0);                               /* supersede / overwrite */
            info = disposition == 0 ? 0 : 3;
        }
    } else {
        if (disposition == 1 || disposition == 4) {             /* open / overwrite */
            DesktopUnlock();
            const char *leaf;
            RamNode *dir = parent_of(root, path, &leaf);
            return iosb(iosb_ptr, dir ? ST_OBJECT_NAME_NOT_FOUND : ST_OBJECT_PATH_NOT_FOUND, 5);
        }
        const char *leaf;
        RamNode *dir = parent_of(root, path, &leaf);
        if (!dir || !dir->dir) { DesktopUnlock(); return iosb(iosb_ptr, ST_OBJECT_PATH_NOT_FOUND, 0); }
        if (!*leaf) { DesktopUnlock(); return iosb(iosb_ptr, ST_OBJECT_NAME_INVALID, 0); }
        node = RamfsCreate(dir, leaf, want_dir);
        if (!node) { DesktopUnlock(); return iosb(iosb_ptr, ST_DISK_FULL, 0); }
        info = 2;                                               /* FILE_CREATED */
    }
    um_lock(&p->lock);
    UINT64 hv = handle_alloc(p, &h);
    if (!hv) { um_unlock(&p->lock); DesktopUnlock(); return iosb(iosb_ptr, ST_TOO_MANY_OPENED_FILES, 0); }
    h->kind = node->dir ? H_DIR : H_FILE;
    h->node = node;
    h->read = rd || node->dir;
    h->write = wr && !node->dir;
    h->append = append;
    h->delete_on_close = options & 0x1000;
    RamfsRef(node);
    um_unlock(&p->lock);
    DesktopUnlock();
    if (!put_u64(handle_ptr, hv)) { handle_close(h); return UM_STATUS_ACCESS_VIOLATION; }
    return iosb(iosb_ptr, ST_SUCCESS, info);
}

/* NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
 *              PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
 *              ULONG ShareAccess, ULONG CreateDisposition,
 *              ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength) */
static UINT64 sys_create_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    return open_file(a1, (UINT32)a2, a3, a4, (UINT32)um_stack_arg(8), (UINT32)um_stack_arg(9));
}

/* NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
 *            ULONG ShareAccess, ULONG OpenOptions) */
static UINT64 sys_open_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    return open_file(a1, (UINT32)a2, a3, a4, 1, (UINT32)um_stack_arg(6));
}

static UINT64 sys_close(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return um_close_handle(a1);
}

UINT64 um_close_handle(UINT64 a1)
{
    UmProcess *p = UmCurrent();
    if (a1 == UINT64_C(0xFFFFFFFFFFFFFFFF) || a1 == UINT64_C(0xFFFFFFFFFFFFFFFE)) return ST_SUCCESS;
    DesktopLock();                                  /* lock order: desktop, then process */
    um_lock(&p->lock);
    UmHandle *h = handle(p, a1);
    if (h) handle_close(h);
    um_unlock(&p->lock);
    DesktopUnlock();
    return h ? ST_SUCCESS : ST_INVALID_HANDLE;
}

/* Starting offset: ByteOffset if given (and not "current"), else pos. */
static UINT64 start_offset(UmHandle *h, UINT64 byte_offset_ptr)
{
    UINT64 off;
    if (byte_offset_ptr && get_u64(byte_offset_ptr, &off) &&
        off != UINT64_C(0xFFFFFFFFFFFFFFFE) && off != UINT64_C(0xFFFFFFFFFFFFFFFF))
        return off;
    return h->pos;
}

/* NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
 *            PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length,
 *            PLARGE_INTEGER ByteOffset, PULONG Key) */
static UINT64 sys_read_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 iosb_ptr = um_stack_arg(5), buf = um_stack_arg(6);
    UINT32 len = (UINT32)um_stack_arg(7);
    DesktopLock();                          /* keeps the handle and its file alive */
    UmHandle *h = handle(p, a1);
    if (!h) { DesktopUnlock(); return ST_INVALID_HANDLE; }

    if (h->kind == H_CON_IN) {
        DesktopUnlock();
        char tmp[512];
        int n = um_console_read(p->con, tmp, len < sizeof(tmp) ? (int)len : (int)sizeof(tmp), p);
        if (n < 0) return iosb(iosb_ptr, ST_END_OF_FILE, 0);
        if (n && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)buf, tmp, (size_t)n))) return UM_STATUS_ACCESS_VIOLATION;
        return iosb(iosb_ptr, ST_SUCCESS, (UINT64)n);
    }
    if (h->kind != H_FILE) { DesktopUnlock(); return iosb(iosb_ptr, ST_INVALID_HANDLE, 0); }
    if (!h->read) { DesktopUnlock(); return iosb(iosb_ptr, ST_ACCESS_DENIED, 0); }

    UINT64 off = start_offset(h, um_stack_arg(8)), done = 0;
    UINT32 size = h->node->size;
    if (off >= size) { DesktopUnlock(); return iosb(iosb_ptr, len ? ST_END_OF_FILE : ST_SUCCESS, 0); }
    UINT64 n = size - off < len ? size - off : len;
    while (done < n) {
        UINT64 chunk = n - done < USER_MAX_BOUNCE ? n - done : USER_MAX_BOUNCE;
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)(buf + done), h->node->data + off + done, (size_t)chunk))) {
            DesktopUnlock();
            return UM_STATUS_ACCESS_VIOLATION;
        }
        done += chunk;
    }
    h->pos = off + done;
    DesktopUnlock();
    return iosb(iosb_ptr, ST_SUCCESS, done);
}

/* NtWriteFile: same arguments as NtReadFile */
static UINT64 sys_write_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 iosb_ptr = um_stack_arg(5), buf = um_stack_arg(6);
    UINT32 len = (UINT32)um_stack_arg(7);
    DesktopLock();                          /* files: held throughout; console: released */
    UmHandle *h = handle(p, a1);
    UINT32 bad = !h ? ST_INVALID_HANDLE :
                 (h->kind == H_FILE && !h->write) ? ST_ACCESS_DENIED :
                 (h->kind != H_FILE && h->kind != H_CON_OUT) ? ST_INVALID_HANDLE : 0;
    bool file = h && h->kind == H_FILE;
    if (!file) DesktopUnlock();
    if (bad) return h ? iosb(iosb_ptr, bad, 0) : bad;

    char small[512];
    char *tmp = len <= sizeof(small) ? small : kmalloc(USER_MAX_BOUNCE);
    if (!tmp) { if (file) DesktopUnlock(); return iosb(iosb_ptr, ST_NO_MEMORY, 0); }
    UINT64 off = h->kind == H_FILE ? start_offset(h, um_stack_arg(8)) : 0, done = 0;
    UINT32 st = ST_SUCCESS;
    while (done < len) {
        UINT32 chunk = len - done < USER_MAX_BOUNCE ? (UINT32)(len - done) : USER_MAX_BOUNCE;
        if (!NT_SUCCESS(CopyFromUser(tmp, (const void *)(uintptr_t)(buf + done), chunk))) {
            st = UM_STATUS_ACCESS_VIOLATION;
            break;
        }
        if (h->kind == H_CON_OUT) {
            int w = um_console_write(p->con, tmp, (int)chunk);
            done += (UINT64)w;
            if ((UINT32)w < chunk) break;                        /* killed */
        } else {
            if (h->append) off = h->node->size;
            bool ok = RamfsWriteAt(h->node, (UINT32)(off + done), tmp, chunk);
            if (!ok) { st = ST_DISK_FULL; break; }
            done += chunk;
        }
    }
    if (tmp != small) kfree(tmp);
    if (file) { h->pos = off + done; DesktopUnlock(); }
    return iosb(iosb_ptr, st, done);
}

static void basic_info(UINT8 *b, const RamNode *n)
{
    memset(b, 0, 40);
    UINT32 attr = n->dir ? 0x10 : 0x20;                        /* DIRECTORY / ARCHIVE */
    memcpy(b + 32, &attr, 4);
}

/* NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG Length,
 *                        FILE_INFORMATION_CLASS) */
static UINT64 sys_query_info_file_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UmHandle *h = handle(p, a1);
    if (!h) return ST_INVALID_HANDLE;
    UINT32 cls = (UINT32)um_stack_arg(5), len = (UINT32)a4;
    UINT8 b[40];
    UINT32 need;
    memset(b, 0, sizeof(b));
    bool file = h->kind == H_FILE || h->kind == H_DIR;
    switch (cls) {
    case 4:                                                     /* FileBasicInformation */
        need = 40;
        if (file) basic_info(b, h->node);
        break;
    case 5: {                                                   /* FileStandardInformation */
        need = 24;
        UINT64 size = h->kind == H_FILE ? h->node->size : 0;
        memcpy(b, &size, 8); memcpy(b + 8, &size, 8);
        UINT32 links = 1; memcpy(b + 16, &links, 4);
        b[20] = h->delete_on_close;
        b[21] = h->kind == H_DIR;
        break;
    }
    case 14:                                                    /* FilePositionInformation */
        need = 8;
        memcpy(b, &h->pos, 8);
        break;
    default:
        return iosb(a2, ST_INVALID_INFO_CLASS, 0);
    }
    if (len < need) return iosb(a2, ST_INFO_LENGTH_MISMATCH, 0);
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, need))) return UM_STATUS_ACCESS_VIOLATION;
    return iosb(a2, ST_SUCCESS, need);
}

/* NtSetInformationFile: same arguments */
static UINT64 sys_set_info_file_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UmHandle *h = handle(p, a1);
    if (!h) return ST_INVALID_HANDLE;
    UINT32 cls = (UINT32)um_stack_arg(5);
    UINT64 v = 0;
    UINT8 flag = 0;
    switch (cls) {
    case 14:                                                    /* FilePositionInformation */
        if (a4 < 8 || !get_u64(a3, &v)) return iosb(a2, ST_INVALID_PARAMETER, 0);
        h->pos = v;
        return iosb(a2, ST_SUCCESS, 0);
    case 20: {                                                  /* FileEndOfFileInformation */
        if (h->kind != H_FILE || !h->write) return iosb(a2, ST_ACCESS_DENIED, 0);
        if (a4 < 8 || !get_u64(a3, &v) || v > RAMFS_FILE_MAX) return iosb(a2, ST_INVALID_PARAMETER, 0);
        DesktopLock();
        bool ok = RamfsResize(h->node, (UINT32)v);
        DesktopUnlock();
        return iosb(a2, ok ? ST_SUCCESS : ST_DISK_FULL, 0);
    }
    case 13:                                                    /* FileDispositionInformation */
        if (a4 < 1 || !NT_SUCCESS(CopyFromUser(&flag, (const void *)(uintptr_t)a3, 1)))
            return iosb(a2, ST_INVALID_PARAMETER, 0);
        if (h->kind == H_DIR && flag && RamfsCount(h->node))
            return iosb(a2, ST_DIRECTORY_NOT_EMPTY, 0);
        if (h->kind != H_FILE && h->kind != H_DIR) return iosb(a2, ST_CANNOT_DELETE, 0);
        h->delete_on_close = flag;
        return iosb(a2, ST_SUCCESS, 0);
    }
    return iosb(a2, ST_INVALID_INFO_CLASS, 0);
}

/* NtQueryAttributesFile(POBJECT_ATTRIBUTES, PFILE_BASIC_INFORMATION) */
static UINT64 sys_query_attributes(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    char path[RAMFS_PATH_MAX];
    RamNode *root;
    UINT32 st = get_path(p, a1, path, sizeof(path), &root);
    if (st) return st;
    UINT8 b[40];
    DesktopLock();
    RamNode *n = path[0] ? RamfsResolve(root, path) : root;
    if (n) basic_info(b, n);
    DesktopUnlock();
    if (!n) return ST_OBJECT_NAME_NOT_FOUND;
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, b, 40)) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
}

/* Wildcard match, case-insensitive: '*' any run, '?' one character. */
static bool wild(const char *pat, const char *s)
{
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (;; s++) { if (wild(pat + 1, s)) return true; if (!*s) return false; }
    }
    if (!*s) return false;
    char a = *pat, b = *s;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return (*pat == '?' || a == b) && wild(pat + 1, s + 1);
}

/* NtQueryDirectoryFile(HANDLE, Event, ApcRoutine, ApcContext,
 *   PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length,
 *   FILE_INFORMATION_CLASS, BOOLEAN ReturnSingleEntry,
 *   PUNICODE_STRING FileName, BOOLEAN RestartScan)
 * FileDirectoryInformation (1), one entry per call. */
static UINT64 sys_query_directory_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 iosb_ptr = um_stack_arg(5), out = um_stack_arg(6);
    UINT32 len = (UINT32)um_stack_arg(7), cls = (UINT32)um_stack_arg(8);
    UINT64 name_ptr = um_stack_arg(10);
    bool restart = um_stack_arg(11) & 0xFF;
    UmHandle *h = handle(p, a1);
    if (!h || h->kind != H_DIR) return ST_INVALID_HANDLE;
    if (cls != 1) return iosb(iosb_ptr, ST_INVALID_INFO_CLASS, 0);
    char pat[RAMFS_NAME_MAX] = "*";
    if (name_ptr) {
        UINT64 us[2];
        UINT16 w[RAMFS_NAME_MAX];
        if (NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)name_ptr, 16))) {
            UINT32 n = (UINT32)(us[0] & 0xFFFF) / 2;
            if (n && n < RAMFS_NAME_MAX && NT_SUCCESS(CopyFromUser(w, (const void *)(uintptr_t)us[1], 2 * n))) {
                for (UINT32 i = 0; i < n; i++) pat[i] = w[i] < 0x80 ? (char)w[i] : '?';
                pat[n] = '\0';
            }
        }
    }
    if (restart) h->pos = 0;
    DesktopLock();
    UINT64 idx = 0;
    RamNode *c = h->node->child;
    for (; c; c = c->next) {
        if (!wild(pat, c->name)) continue;
        if (idx++ == h->pos) break;
    }
    if (!c) { DesktopUnlock(); return iosb(iosb_ptr, ST_NO_MORE_FILES, 0); }
    UINT32 nl = (UINT32)strlen(c->name), need = 64 + 2 * nl;
    UINT8 *b = kzalloc(need);
    if (!b) { DesktopUnlock(); return iosb(iosb_ptr, ST_NO_MEMORY, 0); }
    UINT64 size = c->dir ? 0 : c->size;
    memcpy(b + 40, &size, 8);
    memcpy(b + 48, &size, 8);
    UINT32 attr = c->dir ? 0x10 : 0x20;
    memcpy(b + 56, &attr, 4);
    UINT32 bytes = 2 * nl;
    memcpy(b + 60, &bytes, 4);
    for (UINT32 i = 0; i < nl; i++) { b[64 + 2 * i] = (UINT8)c->name[i]; }
    DesktopUnlock();
    if (len < need) { kfree(b); return iosb(iosb_ptr, ST_BUFFER_OVERFLOW, 0); }
    bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)out, b, need));
    kfree(b);
    if (!ok) return UM_STATUS_ACCESS_VIOLATION;
    h->pos++;
    return iosb(iosb_ptr, ST_SUCCESS, need);
}

/* NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
 *                              FS_INFORMATION_CLASS): FileFsDeviceInformation */
static UINT64 sys_query_volume_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UmHandle *h = handle(p, a1);
    if (!h) return ST_INVALID_HANDLE;
    if ((UINT32)um_stack_arg(5) != 4) return iosb(a2, ST_INVALID_INFO_CLASS, 0);
    if (a4 < 8) return iosb(a2, ST_INFO_LENGTH_MISMATCH, 0);
    UINT32 dev[2] = { (h->kind == H_CON_IN || h->kind == H_CON_OUT) ? 0x50u /* CONSOLE */ : 0x07u /* DISK */, 0 };
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, dev, 8))) return UM_STATUS_ACCESS_VIOLATION;
    return iosb(a2, ST_SUCCESS, 8);
}

static UINT64 sys_query_info_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    DesktopLock();
    UINT64 r = sys_query_info_file_locked(a1, a2, a3, a4);
    DesktopUnlock();
    return r;
}

static UINT64 sys_set_info_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    DesktopLock();
    UINT64 r = sys_set_info_file_locked(a1, a2, a3, a4);
    DesktopUnlock();
    return r;
}

static UINT64 sys_query_directory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    DesktopLock();
    UINT64 r = sys_query_directory_locked(a1, a2, a3, a4);
    DesktopUnlock();
    return r;
}

static UINT64 sys_query_volume(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    DesktopLock();
    UINT64 r = sys_query_volume_locked(a1, a2, a3, a4);
    DesktopUnlock();
    return r;
}

/* -----------------------------------------------------------------------
 * Virtual memory
 * ----------------------------------------------------------------------- */
#define MEM_COMMIT     0x1000u
#define MEM_RESERVE    0x2000u
#define MEM_DECOMMIT   0x4000u
#define MEM_RELEASE    0x8000u

static bool valid_protect(UINT32 pr)
{
    pr &= 0xFF;
    return pr == 0x01 || pr == 0x02 || pr == 0x04 || pr == 0x08 ||
           pr == 0x10 || pr == 0x20 || pr == 0x40 || pr == 0x80;
}

/* NtAllocateVirtualMemory(HANDLE Process, PVOID *BaseAddress, ULONG_PTR ZeroBits,
 *                         PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) */
static UINT64 sys_alloc_vm_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a1 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_INVALID_HANDLE;
    UINT32 type = (UINT32)um_stack_arg(5), prot = (UINT32)um_stack_arg(6);
    UINT64 base, size;
    if (!get_u64(a2, &base) || !get_u64(a4, &size)) return UM_STATUS_ACCESS_VIOLATION;
    if (!size || !(type & (MEM_COMMIT | MEM_RESERVE)) || !valid_protect(prot)) return ST_INVALID_PARAMETER;
    UINT64 end = (base + size + 0xFFF) & ~0xFFFULL;

    if (type & MEM_RESERVE) {
        base &= ~0xFFFFULL;
        size = (end - base + 0xFFF) & ~0xFFFULL;
        if (!base) {
            base = um_find_free(p, size, UM_ALLOC_MIN, UM_ALLOC_MAX);
            if (!base) return ST_NO_MEMORY;
        } else if (!um_is_free(p, base, size)) {
            return ST_CONFLICTING_ADDRESSES;
        }
        if (!um_region_add(p, base, size, prot, false)) return ST_NO_MEMORY;
    } else {
        base &= ~0xFFFULL;
        size = end - base;
        UmRegion *r = um_region_find(p, base);
        if (!r || base + size > r->base + r->size || r->image) return ST_MEMORY_NOT_ALLOCATED;
    }
    if (type & MEM_COMMIT) {
        if (p->pages + size / PAGE_SIZE > PROC_MEM_LIMIT_PAGES) {
            if (type & MEM_RESERVE) um_region_remove(p, um_region_find(p, base));
            return ST_NO_MEMORY;
        }
        if (!um_commit(p, base, size, prot)) {
            um_decommit(p, base, size);
            if (type & MEM_RESERVE) um_region_remove(p, um_region_find(p, base));
            return ST_NO_MEMORY;
        }
    }
    put_u64(a2, base);
    put_u64(a4, size);
    return ST_SUCCESS;
}

/* NtFreeVirtualMemory(HANDLE, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType) */
static UINT64 sys_free_vm_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 base, size;
    if (!get_u64(a2, &base) || !get_u64(a3, &size)) return UM_STATUS_ACCESS_VIOLATION;
    UmRegion *r = um_region_find(p, base);
    if (!r || r->image) return ST_MEMORY_NOT_ALLOCATED;
    if (a4 & MEM_RELEASE) {
        if (base != r->base || size) return ST_INVALID_PARAMETER;
        size = r->size;
        um_decommit(p, base, size);
        um_region_remove(p, r);
    } else if (a4 & MEM_DECOMMIT) {
        UINT64 end = (base + (size ? size : r->base + r->size - base) + 0xFFF) & ~0xFFFULL;
        base &= ~0xFFFULL;
        size = end - base;
        um_decommit(p, base, size);
    } else {
        return ST_INVALID_PARAMETER;
    }
    put_u64(a2, base);
    put_u64(a3, size);
    return ST_SUCCESS;
}

/* NtProtectVirtualMemory(HANDLE, PVOID *Base, PSIZE_T Size, ULONG New, PULONG Old) */
static UINT64 sys_protect_vm_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 base, size, old_ptr = um_stack_arg(5);
    if (!get_u64(a2, &base) || !get_u64(a3, &size) || !valid_protect((UINT32)a4)) return ST_INVALID_PARAMETER;
    UINT64 end = (base + size + 0xFFF) & ~0xFFFULL;
    base &= ~0xFFFULL;
    UmRegion *r = um_region_find(p, base);
    if (!r || end > r->base + r->size) return ST_MEMORY_NOT_ALLOCATED;
    for (UINT64 a = base; a < end; a += PAGE_SIZE)
        if (!um_is_committed(p, a)) return ST_MEMORY_NOT_ALLOCATED;
    UINT32 old = r->protect;
    um_commit(p, base, end - base, (UINT32)a4);                 /* re-protect committed pages */
    if (old_ptr) { UINT32 o = old; CopyToUser((void *)(uintptr_t)old_ptr, &o, 4); }
    put_u64(a2, base);
    put_u64(a3, end - base);
    return ST_SUCCESS;
}

static UINT64 sys_alloc_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (p) um_lock(&p->lock);
    UINT64 r = sys_alloc_vm_locked(a1, a2, a3, a4);
    if (p) um_unlock(&p->lock);
    return r;
}

static UINT64 sys_free_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (p) um_lock(&p->lock);
    UINT64 r = sys_free_vm_locked(a1, a2, a3, a4);
    if (p) um_unlock(&p->lock);
    return r;
}

static UINT64 sys_protect_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (p) um_lock(&p->lock);
    UINT64 r = sys_protect_vm_locked(a1, a2, a3, a4);
    if (p) um_unlock(&p->lock);
    return r;
}

/* -----------------------------------------------------------------------
 * Process, time
 * ----------------------------------------------------------------------- */
static UINT64 sys_terminate_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a1 != 0 && a1 != UINT64_C(0xFFFFFFFFFFFFFFFF)) return ST_INVALID_HANDLE;
    um_exit_process((UINT32)a2);
}

extern long long nova_time(long long *t);         /* net/tls_platform.c: RTC as Unix time */
static UINT64 g_boot_time;                        /* 100 ns units since 1601, at boot */
static UINT64 g_boot_ticks, g_tsc0, g_tsc_hz;

UINT64 um_now_100ns(void)
{
    return g_boot_time + (sched_ticks() - g_boot_ticks) * 100000ULL;
}

static UINT64 sys_query_system_time(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    return put_u64(a1, um_now_100ns()) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
}

/* NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency):
 * the TSC, with its frequency calibrated against the 100 Hz timer. */
static UINT64 sys_query_perf_counter(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (!g_tsc_hz) {
        UINT64 dt = sched_ticks() - g_boot_ticks;
        if (dt < 50) {                              /* too early: wait for 0.5 s of ticks */
            while (sched_ticks() - g_boot_ticks < 50) sched_yield();
            dt = sched_ticks() - g_boot_ticks;
        }
        g_tsc_hz = (rdtsc() - g_tsc0) / dt * 100;
    }
    if (a1 && !put_u64(a1, rdtsc())) return UM_STATUS_ACCESS_VIOLATION;
    if (a2 && !put_u64(a2, g_tsc_hz)) return UM_STATUS_ACCESS_VIOLATION;
    return ST_SUCCESS;
}

/* NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval) */
static UINT64 sys_delay(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 v;
    if (!get_u64(a2, &v)) return UM_STATUS_ACCESS_VIOLATION;
    INT64 iv = (INT64)v;
    UINT64 wait_100ns = iv < 0 ? (UINT64)(-iv) : (v > um_now_100ns() ? v - um_now_100ns() : 0);
    UINT64 until = sched_ticks() + (wait_100ns + 99999) / 100000;
    do {
        if (um_stopping()) break;
        sched_yield();
    } while (sched_ticks() < until);
    return ST_SUCCESS;
}

static UINT64 sys_yield(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    sched_yield();
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Registration
 * ----------------------------------------------------------------------- */
void um_install(UINT32 num, SYSCALL_HANDLER h)
{
    g_um[num] = h;
}

void um_syscall_init(void)
{
    g_boot_time = ((UINT64)nova_time(NULL) + UINT64_C(11644473600)) * 10000000ULL;
    g_boot_ticks = sched_ticks();
    g_tsc0 = rdtsc();

    um_install(SYSCALL_NtCreateFile,               sys_create_file);
    um_install(SYSCALL_NtOpenFile,                 sys_open_file);
    um_install(SYSCALL_NtClose,                    sys_close);
    um_install(SYSCALL_NtReadFile,                 sys_read_file);
    um_install(SYSCALL_NtWriteFile,                sys_write_file);
    um_install(SYSCALL_NtQueryInformationFile,     sys_query_info_file);
    um_install(SYSCALL_NtSetInformationFile,       sys_set_info_file);
    um_install(SYSCALL_NtQueryAttributesFile,      sys_query_attributes);
    um_install(SYSCALL_NtQueryDirectoryFile,       sys_query_directory);
    um_install(SYSCALL_NtQueryVolumeInformationFile, sys_query_volume);
    um_install(SYSCALL_NtAllocateVirtualMemory,    sys_alloc_vm);
    um_install(SYSCALL_NtFreeVirtualMemory,        sys_free_vm);
    um_install(SYSCALL_NtProtectVirtualMemory,     sys_protect_vm);
    um_install(SYSCALL_NtTerminateProcess,         sys_terminate_process);
    um_install(SYSCALL_NtQuerySystemTime,          sys_query_system_time);
    um_install(SYSCALL_NtQueryPerformanceCounter,  sys_query_perf_counter);
    um_install(SYSCALL_NtDelayExecution,           sys_delay);
    um_install(SYSCALL_NtYieldExecution,           sys_yield);
    um_thread_syscalls_init();
    um_exception_syscalls_init();
    um_socket_syscalls_init();
    um_gui_syscalls_init();
}
