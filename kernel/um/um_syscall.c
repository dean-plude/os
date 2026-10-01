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
#include "../arch/x86_64/apic.h"
#include "../wm/clipboard.h"

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
#ifndef ST_TOO_MANY_HANDLES
#define ST_TOO_MANY_HANDLES        0xC000011Fu
#endif
#define ST_OBJECT_NAME_COLLISION   0xC0000035u
#define ST_OBJECT_PATH_NOT_FOUND   0xC000003Au
#define ST_DISK_FULL               0xC000007Fu
#define ST_MEMORY_NOT_ALLOCATED    0xC00000A0u
#define ST_FILE_IS_A_DIRECTORY     0xC00000BAu
#define ST_DIRECTORY_NOT_EMPTY     0xC0000101u
#define ST_NOT_A_DIRECTORY         0xC0000103u
#define ST_TOO_MANY_OPENED_FILES   0xC000011Fu
#define ST_CANNOT_DELETE           0xC0000121u

/* A program may commit up to 7/8 of the machine's memory (pages are only
 * taken when first touched, so this bounds promises, not use) */
static UINT64 proc_commit_limit(void)
{
    static UINT64 limit;
    if (!limit) {
        uint64_t total = 0, free = 0, used = 0;
        pmm_stats(&total, &free, &used);
        limit = total - total / 8;
    }
    return limit;
}
#define PROC_MEM_LIMIT_PAGES proc_commit_limit()

static SYSCALL_HANDLER g_um[SYSCALL_MAX];       /* the services open to programs */

bool UmSyscallAllowed(UINT64 num)
{
    return num < SYSCALL_MAX && g_um[num];
}

/* Services that run without the big kernel lock: everything they touch is
 * under locks of their own (see um_lock_free_init for the list and why) */
static bool g_um_free[SYSCALL_MAX];

bool UmSyscallLockFree(UINT64 num)
{
    return num < SYSCALL_MAX && g_um_free[num];
}

void um_lock_free(UINT32 num)
{
    if (num < SYSCALL_MAX) g_um_free[num] = true;
}

/* The services that skip the big kernel lock, and what they rely on:
 *   time and yielding:          the scheduler (sched_lock);
 *   waits, events, mutants, semaphores, suspend counts and handle
 *   closing:                    the handle table (process lock), object
 *                               state (g_um_oblock), destructors take the
 *                               big lock themselves (um_ob_unref);
 *   virtual memory:             the process lock (regions, page tables),
 *                               the PMM and heap spinlocks, TLB shootdowns;
 *   sockets:                    net_lock around the network stack;
 *   windows (NtNovaGui*):       DesktopLock (window manager and message
 *                               queues) and the process lock.
 * User memory is reached through CopyFromUser/CopyToUser, which survive
 * the memory disappearing meanwhile.  Anything else (files, the registry,
 * processes, sections, the console, the loader...) keeps the big lock. */
static void um_lock_free_init(void)
{
    static const UINT32 list[] = {
        SYSCALL_NtQuerySystemTime, SYSCALL_NtQueryPerformanceCounter,
        SYSCALL_NtDelayExecution, SYSCALL_NtYieldExecution,
        SYSCALL_NtWaitForSingleObject, SYSCALL_NtWaitForMultipleObjects,
        SYSCALL_NtCreateEvent, SYSCALL_NtSetEvent, SYSCALL_NtResetEvent, SYSCALL_NtClearEvent,
        SYSCALL_NtOpenEvent, SYSCALL_NtOpenMutant, SYSCALL_NtOpenSemaphore,
        SYSCALL_NtCreateMutant, SYSCALL_NtReleaseMutant,
        SYSCALL_NtCreateSemaphore, SYSCALL_NtReleaseSemaphore,
        SYSCALL_NtSuspendThread, SYSCALL_NtResumeThread, SYSCALL_NtClose,
        SYSCALL_NtAllocateVirtualMemory, SYSCALL_NtFreeVirtualMemory,
        SYSCALL_NtProtectVirtualMemory, SYSCALL_NtQueryVirtualMemory,
        SYSCALL_NtNovaSocket, SYSCALL_NtNovaSockConnect, SYSCALL_NtNovaSockSend,
        SYSCALL_NtNovaSockRecv, SYSCALL_NtNovaSockBind, SYSCALL_NtNovaSockListen,
        SYSCALL_NtNovaSockAccept, SYSCALL_NtNovaSockCtl, SYSCALL_NtNovaSockSendTo,
        SYSCALL_NtNovaSockRecvFrom,
        SYSCALL_NtNovaGuiCreate, SYSCALL_NtNovaGuiGetMessage, SYSCALL_NtNovaGuiInvalidate,
        SYSCALL_NtNovaGuiSetText, SYSCALL_NtNovaGuiShow, SYSCALL_NtNovaGuiDestroy,
        SYSCALL_NtNovaGuiSetTimer, SYSCALL_NtNovaGuiKillTimer, SYSCALL_NtNovaGuiMessageBox,
        SYSCALL_NtNovaGuiScreenSize, SYSCALL_NtNovaGuiPostMessage, SYSCALL_NtNovaGuiCtl,
    };
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) um_lock_free(list[i]);
}

static UINT32 get_path(UmProcess *p, UINT64 oa_ptr, char *out, int cap, RamNode **root);
static UINT32 g_oa_attrs;               /* Attributes of the last get_path (under the big lock) */

/* Terminal "trace NAME": log the failing system calls of programs named
 * NAME (a debugging aid for Windows programs that misbehave) */
static char g_trace[32];
static bool g_trace_all;
void UmSetTrace(const char *name)
{
    int i = 0;
    g_trace_all = false;
    if (name && name[0] == '+') { g_trace_all = true; name++; }   /* "+NAME": every call */
    for (; name && name[i] && i < 31; i++) g_trace[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
    g_trace[i] = 0;
}

UINT64 UmSyscall(UINT64 num, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmThread *t = UmCurrentThread();
    if (t) { t->park = 1; t->last_sys = (UINT16)num; }   /* park: cleared on the way out (UmReturnToUser) */
    UINT64 r = g_um[num](a1, a2, a3, a4);
    if (g_trace[0] && t && (g_trace_all || ((r & 0x80000000u) && (UINT32)r == r))) {
        const char *n = t->proc->name;
        int i = 0;
        while (g_trace[i] && n[i] && (n[i] | 0x20) == (g_trace[i] | 0x20)) i++;
        if (!g_trace[i] && (!n[i] || n[i] == '.')) {
            /* the file name of the calls that take one */
            char path[RAMFS_PATH_MAX] = "";
            RamNode *root;
            UINT64 oa = num == SYSCALL_NtCreateFile || num == SYSCALL_NtOpenFile ? a3 :
                        num == SYSCALL_NtQueryAttributesFile ? a1 : 0;
            if (oa && get_path(t->proc, oa, path, sizeof(path), &root)) path[0] = 0;
            kprintf("[TRACE] %s: syscall %03llx(%llx, %llx, %llx, %llx) -> %08llx%s%s\n", n,
                    (unsigned long long)num, (unsigned long long)a1, (unsigned long long)a2,
                    (unsigned long long)a3, (unsigned long long)a4, (unsigned long long)r,
                    path[0] ? " " : "", path);
        }
    }
    return r;
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

/* An I/O status block; bit 63 of @ptr marks the 32-bit layout (a WoW
 * program's own block, passed through for I/O that may finish later) */
static UINT32 iosb(UINT64 ptr, UINT32 status, UINT64 info)
{
    if (ptr >> 63) {
        UINT32 b32[2] = { status, (UINT32)info };
        ptr &= ~(UINT64_C(1) << 63);
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, b32, sizeof(b32)))) return UM_STATUS_ACCESS_VIOLATION;
        return status;
    }
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

RamNode *um_handle_file(UmProcess *p, UINT64 h)
{
    UmHandle *hd = handle(p, h);
    return hd && hd->kind == H_FILE ? hd->node : NULL;
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
/* UTF-16 (from programs) <-> UTF-8 (drive C: names) */
static int w2u(const UINT16 *w, UINT32 n, char *out, int cap)
{
    int o = 0;
    for (UINT32 i = 0; i < n; i++) {
        UINT32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        int need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + need >= cap) break;
        if (need == 1) out[o++] = (char)c;
        else if (need == 2) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (need == 3) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = '\0';
    return o;
}

/* UTF-8 -> UTF-16 into @out (little-endian bytes); returns code units */
static UINT32 u2w(const char *s, UINT8 *out, UINT32 cap)
{
    UINT32 n = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c && n < cap; ) {
        UINT32 ch = *c++;
        if (ch >= 0xC0) {
            int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            ch &= 0x3F >> more;
            while (more-- && (*c & 0xC0) == 0x80) ch = ch << 6 | (*c++ & 0x3F);
        }
        if (ch >= 0x10000) {
            if (n + 2 > cap) break;
            UINT16 hi = (UINT16)(0xD800 + ((ch - 0x10000) >> 10)), lo = (UINT16)(0xDC00 + ((ch - 0x10000) & 0x3FF));
            memcpy(out + 2 * n, &hi, 2); memcpy(out + 2 * n + 2, &lo, 2);
            n += 2;
        } else {
            UINT16 w = (UINT16)ch;
            memcpy(out + 2 * n, &w, 2);
            n++;
        }
    }
    return n;
}

/* File system redirection, as WoW64 does it: a 32-bit program asking for
 * C:\Windows\System32 gets C:\Windows\SysWOW64 (its own DLLs), unless the
 * thread turned that off (Wow64DisableWow64FsRedirection sets this word
 * in its 32-bit TEB) */
#define TEB32_NO_REDIRECT 0xFF8
static void wow_redirect(UmProcess *p, char *path)
{
    if (!p->wow) return;
    UmThread *t = UmCurrentThread();
    UINT32 off = 0;
    if (t && NT_SUCCESS(CopyFromUser(&off, (const void *)(uintptr_t)(t->teb + TEB32_NO_REDIRECT), 4)) && off) return;
    um_wow_path(p, path);
}

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
    w2u(w, len, out, cap);
    g_oa_attrs = (UINT32)oa[3];
    if (oa[1]) {                                        /* relative to \Device\NamedPipe\ */
        UmHandle *h = handle(p, oa[1]);
        if (h && h->kind == H_NULL && h->npfs) {
            static const char npfs[] = "\\Device\\NamedPipe\\";
            int nl = (int)sizeof(npfs) - 1, ol = (int)strlen(out);
            if (ol + nl >= cap) return ST_OBJECT_NAME_INVALID;
            memmove(out + nl, out, (size_t)ol + 1);
            memcpy(out, npfs, (size_t)nl);
        }
    }
    if (um_pipe_name(out)) { *root = p->cwd; return ST_SUCCESS; }   /* \Device\NamedPipe\X */
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
    wow_redirect(p, out);
    return ST_SUCCESS;
}

UINT32 um_get_path(UmProcess *p, UINT64 oa_ptr, char *out, int cap, UINT32 *attrs)
{
    RamNode *root;
    UINT32 st = get_path(p, oa_ptr, out, cap, &root);
    if (attrs) *attrs = g_oa_attrs;
    return st;
}

static bool is_console_name(const char *s, UmHandleKind *kind)
{
    if (!strcmp(s, "CONIN$") || !strcmp(s, "conin$")) { *kind = H_CON_IN;  return true; }
    if (!strcmp(s, "CONOUT$") || !strcmp(s, "conout$") ||
        !strcmp(s, "CON") || !strcmp(s, "con")) { *kind = H_CON_OUT; return true; }
    if (!strcmp(s, "NUL") || !strcmp(s, "nul")) { *kind = H_NULL; return true; }     /* the null device */
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

    bool inherit = g_oa_attrs & 0x2;                            /* OBJ_INHERIT */
    UmHandleKind ck;
    UmHandle *h;
    if (um_pipe_name(path) && !*um_pipe_name(path)) {           /* the pipe file system's root */
        um_lock(&p->lock);
        UINT64 hv = handle_alloc(p, &h);
        if (hv) { h->kind = H_NULL; h->npfs = true; h->inherit = inherit; }
        um_unlock(&p->lock);
        if (!hv) return iosb(iosb_ptr, ST_TOO_MANY_OPENED_FILES, 0);
        if (!put_u64(handle_ptr, hv)) { um_close_handle(hv); return UM_STATUS_ACCESS_VIOLATION; }
        return iosb(iosb_ptr, ST_SUCCESS, 1);
    }
    if (um_pipe_name(path)) {                                   /* a pipe's client end */
        UmObject *o;
        bool rd, wr;
        UINT32 pst = um_pipe_open(path, access, options, &o, &rd, &wr);
        if (pst) return iosb(iosb_ptr, pst, 0);
        um_lock(&p->lock);
        UINT64 hv = handle_alloc(p, &h);
        if (hv) { h->kind = H_OBJECT; h->obj = o; h->read = rd; h->write = wr; h->inherit = inherit; }
        um_unlock(&p->lock);
        if (!hv) { um_ob_unref(o); return iosb(iosb_ptr, ST_TOO_MANY_OPENED_FILES, 0); }
        if (!put_u64(handle_ptr, hv)) { um_close_handle(hv); return UM_STATUS_ACCESS_VIOLATION; }
        return iosb(iosb_ptr, ST_SUCCESS, 1);
    }
    if (is_console_name(path, &ck)) {
        um_lock(&p->lock);
        UINT64 hv = handle_alloc(p, &h);
        if (hv) { h->kind = ck; h->inherit = inherit; }
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
    h->inherit = inherit;
    h->async = !(options & 0x30);                               /* no FILE_SYNCHRONOUS_IO_* */
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

/* An I/O request's event: the request finished (at once, for files) */
static void set_io_event(UINT64 ev)
{
    if (!ev) return;
    UmObject *o = um_handle_object(UmCurrent(), ev, UO_EVENT);
    if (!o) return;
    IrqState s = ob_lock();
    o->signaled = true;
    um_ob_wake(o);
    ob_unlock(s);
    um_ob_unref(o);
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
    UmObject *po = um_handle_object(p, a1, UO_PIPE);
    if (po) {
        UINT64 info;
        UINT32 st = um_pipe_read(po, a2, iosb_ptr, buf, len, &info);
        um_ob_unref(po);
        return st == ST_PENDING ? st : iosb(iosb_ptr, st, info);
    }
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
    if (h->kind == H_NULL) { DesktopUnlock(); return iosb(iosb_ptr, ST_END_OF_FILE, 0); }
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
    set_io_event(a2);
    return iosb(iosb_ptr, ST_SUCCESS, done);
}

/* NtWriteFile: same arguments as NtReadFile */
static UINT64 sys_write_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 iosb_ptr = um_stack_arg(5), buf = um_stack_arg(6);
    UINT32 len = (UINT32)um_stack_arg(7);
    UmObject *po = um_handle_object(p, a1, UO_PIPE);
    if (po) {
        UINT64 info;
        UINT32 st = um_pipe_write(po, a2, iosb_ptr, buf, len, &info);
        um_ob_unref(po);
        return st == ST_PENDING ? st : iosb(iosb_ptr, st, info);
    }
    DesktopLock();                          /* files: held throughout; console: released */
    UmHandle *h = handle(p, a1);
    UINT32 bad = !h ? ST_INVALID_HANDLE :
                 (h->kind == H_FILE && !h->write) ? ST_ACCESS_DENIED :
                 (h->kind != H_FILE && h->kind != H_CON_OUT && h->kind != H_NULL) ? ST_INVALID_HANDLE : 0;
    bool file = h && h->kind == H_FILE && !bad;
    if (!file) DesktopUnlock();
    if (bad) return h ? iosb(iosb_ptr, bad, 0) : bad;
    if (h->kind == H_NULL) { set_io_event(a2); return iosb(iosb_ptr, ST_SUCCESS, len); }

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
    set_io_event(a2);
    return iosb(iosb_ptr, st, done);
}

static UINT64 g_boot_time;                        /* 100 ns units since 1601, at boot */
static UINT64 g_boot_ticks;

/* FILE_BASIC_INFORMATION: creation, access, write, change times; attributes */
static void basic_info(UINT8 *b, const RamNode *n)
{
    memset(b, 0, 40);
    UINT64 c = n->ctime ? n->ctime : g_boot_time, m = n->mtime ? n->mtime : c;
    memcpy(b, &c, 8); memcpy(b + 8, &m, 8); memcpy(b + 16, &m, 8); memcpy(b + 24, &m, 8);
    UINT32 attr = (n->dir ? 0x10 : 0x20) | (n->attrs & 0x07);  /* DIRECTORY / ARCHIVE, R/H/S */
    memcpy(b + 32, &attr, 4);
}

/* FILE_NAME_INFORMATION for @h at user @dst (@len bytes): the status */
static UINT64 sys_query_info_file_name(UmHandle *h, UINT64 dst, UINT32 len)
{
    if (!(h->kind == H_FILE || h->kind == H_DIR) || len < 4) return ST_INFO_LENGTH_MISMATCH;
    char path[RAMFS_PATH_MAX];
    RamfsPath(h->node, path, sizeof(path));
    UINT16 w[RAMFS_PATH_MAX + 2];
    UINT32 n = 0;
    for (const unsigned char *c = (const unsigned char *)path + 2; *c && n < RAMFS_PATH_MAX; ) {
        UINT32 ch = *c++;
        if (ch >= 0xE0 && c[0] && c[1]) { ch = (ch & 0x0F) << 12 | (UINT32)(c[0] & 0x3F) << 6 | (c[1] & 0x3F); c += 2; }
        else if (ch >= 0xC0 && c[0]) { ch = (ch & 0x1F) << 6 | (c[0] & 0x3F); c++; }
        w[n++] = (UINT16)ch;
    }
    UINT32 bytes = 2 * n;
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)dst, &bytes, 4))) return UM_STATUS_ACCESS_VIOLATION;
    UINT32 room = len - 4 < bytes ? len - 4 : bytes;
    if (room && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)(dst + 4), w, room))) return UM_STATUS_ACCESS_VIOLATION;
    return room < bytes ? 0x80000005u : ST_SUCCESS;
}

/* NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG Length,
 *                        FILE_INFORMATION_CLASS) */
static UINT64 sys_query_info_file_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UmHandle *h = handle(p, a1);
    if (!h) return ST_INVALID_HANDLE;
    UINT32 cls = (UINT32)um_stack_arg(5), len = (UINT32)a4;
    UINT8 b[8 + 2 * RAMFS_PATH_MAX];
    UINT32 need;
    memset(b, 0, 40);
    bool file = h->kind == H_FILE || h->kind == H_DIR;
    switch (cls) {
    case 6: {                                                   /* FileInternalInformation */
        need = 8;
        UINT64 id = (UINT64)(uintptr_t)h->node;
        memcpy(b, &id, 8);
        break;
    }
    case 9: {                                                   /* FileNameInformation */
        if (!file) return iosb(a2, ST_INVALID_PARAMETER, 0);
        char path[RAMFS_PATH_MAX];
        RamfsPath(h->node, path, sizeof(path));
        /* the name without the drive ("\dir\file"), UTF-8 -> UTF-16 */
        UINT32 n = 0;
        for (const unsigned char *c = (const unsigned char *)path + 2; *c && n < RAMFS_PATH_MAX; ) {
            UINT32 ch = *c++;
            if (ch >= 0xC0) {
                int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
                ch &= 0x3F >> more;
                while (more-- && (*c & 0xC0) == 0x80) ch = ch << 6 | (*c++ & 0x3F);
            }
            if (ch >= 0x10000) {
                if (n + 2 > RAMFS_PATH_MAX) break;
                UINT16 hi = (UINT16)(0xD800 + ((ch - 0x10000) >> 10)), lo = (UINT16)(0xDC00 + ((ch - 0x10000) & 0x3FF));
                memcpy(b + 4 + 2 * n, &hi, 2); memcpy(b + 6 + 2 * n, &lo, 2);
                n += 2;
            } else {
                UINT16 w = (UINT16)ch;
                memcpy(b + 4 + 2 * n, &w, 2);
                n++;
            }
        }
        UINT32 bytes = 2 * n;
        memcpy(b, &bytes, 4);
        need = 4 + bytes;
        if (len < need) {                                       /* partial name, like NT */
            if (len < 4) return iosb(a2, ST_INFO_LENGTH_MISMATCH, 0);
            if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, len))) return UM_STATUS_ACCESS_VIOLATION;
            return iosb(a2, 0x80000005u, len);                  /* STATUS_BUFFER_OVERFLOW */
        }
        break;
    }
    case 35: {                                                  /* FileAttributeTagInformation */
        need = 8;
        UINT32 attr = !file ? 0x80 : h->node->dir ? 0x10 : 0x20;
        memcpy(b, &attr, 4);
        break;
    }
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
    case 7:                                                     /* FileEaInformation: no extended attributes */
    case 17:                                                    /* FileAlignmentInformation: byte aligned */
        need = 4;
        break;
    case 8: {                                                   /* FileAccessInformation */
        need = 4;
        UINT32 acc = 0x00120089u | (h->write ? 0x00000116u : 0) | 0x00010000u;   /* read/attrs/sync | write | DELETE */
        memcpy(b, &acc, 4);
        break;
    }
    case 16: {                                                  /* FileModeInformation */
        need = 4;
        UINT32 mode = h->async ? 0 : 0x20;                      /* FILE_SYNCHRONOUS_IO_NONALERT */
        if (h->delete_on_close) mode |= 0x1000;
        memcpy(b, &mode, 4);
        break;
    }
    case 34: {                                                  /* FileNetworkOpenInformation */
        need = 56;
        UINT8 bi[40];
        memset(bi, 0, sizeof(bi));
        if (file) basic_info(bi, h->node);
        memcpy(b, bi, 32);                                      /* the four times */
        UINT64 size = h->kind == H_FILE ? h->node->size : 0, alloc = (size + 4095) & ~4095ULL;
        memcpy(b + 32, &alloc, 8); memcpy(b + 40, &size, 8);
        memcpy(b + 48, bi + 32, 4);                             /* FileAttributes */
        if (!file) { UINT32 a = 0x80; memcpy(b + 48, &a, 4); }
        break;
    }
    case 68: {                                                  /* FileStatInformation */
        need = 72;
        UINT8 bi[40];
        memset(bi, 0, sizeof(bi));
        if (file) basic_info(bi, h->node);
        UINT64 id = (UINT64)(uintptr_t)h->node;
        memcpy(b, &id, 8);                                      /* FileId */
        memcpy(b + 8, bi, 32);                                  /* times */
        UINT64 size = h->kind == H_FILE ? h->node->size : 0, alloc = (size + 4095) & ~4095ULL;
        memcpy(b + 40, &alloc, 8); memcpy(b + 48, &size, 8);
        memcpy(b + 56, bi + 32, 4);                             /* FileAttributes */
        UINT32 links = 1, eff = 0x001F01FF;
        memcpy(b + 64, &links, 4); memcpy(b + 68, &eff, 4);     /* NumberOfLinks, EffectiveAccess */
        if (!file) { UINT32 a = 0x80; memcpy(b + 56, &a, 4); }
        break;
    }
    case 18: {                                                  /* FileAllInformation */
        /* Basic 40, Standard 24, Internal 8, Ea 4, Access 4, Position 8, Mode 4, Alignment 4, Name 4+ */
        if (len < 100) return iosb(a2, ST_INFO_LENGTH_MISMATCH, 0);
        UINT8 all[100];
        memset(all, 0, sizeof(all));
        if (file) basic_info(all, h->node);
        UINT64 size = h->kind == H_FILE ? h->node->size : 0;
        memcpy(all + 40, &size, 8); memcpy(all + 48, &size, 8);
        UINT32 links = 1; memcpy(all + 56, &links, 4);
        all[60] = h->delete_on_close; all[61] = h->kind == H_DIR;
        UINT64 id = (UINT64)(uintptr_t)h->node; memcpy(all + 64, &id, 8);
        UINT32 acc = 0x001F01FF; memcpy(all + 76, &acc, 4);
        memcpy(all + 80, &h->pos, 8);
        UINT32 mode = h->async ? 0 : 0x20; memcpy(all + 88, &mode, 4);
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, all, 96))) return UM_STATUS_ACCESS_VIOLATION;
        /* then the name, as FileNameInformation */
        UINT64 r = sys_query_info_file_name(h, a3 + 96, len - 96);
        UINT32 nl = 0;
        CopyFromUser(&nl, (const void *)(uintptr_t)(a3 + 96), 4);
        return iosb(a2, (UINT32)r, r == ST_SUCCESS || r == 0x80000005u ? (96 + 4 + nl < len ? 96 + 4 + nl : len) : 0);
    }
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
    case 10: {                                                  /* FileRenameInformation */
        /* { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[] } */
        UINT64 hdr[3];
        if (h->kind != H_FILE && h->kind != H_DIR) return iosb(a2, ST_INVALID_PARAMETER, 0);
        if (a4 < 20 || !NT_SUCCESS(CopyFromUser(hdr, (const void *)(uintptr_t)a3, 24)))
            return iosb(a2, ST_INVALID_PARAMETER, 0);
        UINT32 nlen = (UINT32)hdr[2] / 2;
        if (!nlen || nlen >= RAMFS_PATH_MAX) return iosb(a2, ST_OBJECT_NAME_INVALID, 0);
        UINT16 w[RAMFS_PATH_MAX];
        if (!NT_SUCCESS(CopyFromUser(w, (const void *)(uintptr_t)(a3 + 20), 2 * nlen)))
            return iosb(a2, UM_STATUS_ACCESS_VIOLATION, 0);
        char path[RAMFS_PATH_MAX];
        w2u(w, nlen, path, sizeof(path));
        char *s = path;
        if (!strncmp(s, "\\??\\", 4)) s += 4;
        if (((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') && s[1] == ':') {
            if ((s[0] | 0x20) != 'c') return iosb(a2, 0xC00000D4u, 0);   /* NOT_SAME_DEVICE */
            s += 2;
        }
        /* absolute, relative to RootDirectory, a bare name (same directory), or relative to the cwd */
        RamNode *root = strchr(s, '\\') || strchr(s, '/') ? p->cwd : h->node->parent;
        if (hdr[1]) {
            UmHandle *rd = handle(p, hdr[1]);
            if (!rd || rd->kind != H_DIR) return iosb(a2, ST_INVALID_HANDLE, 0);
            root = rd->node;
        }
        const char *leaf;
        RamNode *dir = parent_of(root, s, &leaf);
        if (!dir || !dir->dir) return iosb(a2, ST_OBJECT_PATH_NOT_FOUND, 0);
        RamNode *old = RamfsFind(dir, leaf);
        if (old && old != h->node && !(hdr[0] & 0xFF)) return iosb(a2, 0xC0000035u, 0);   /* NAME_COLLISION */
        if (!RamfsRename(h->node, dir, leaf, hdr[0] & 0xFF))
            return iosb(a2, old ? ST_ACCESS_DENIED : ST_OBJECT_NAME_INVALID, 0);
        return iosb(a2, ST_SUCCESS, 0);
    }
    case 4: {                                                   /* FileBasicInformation */
        UINT8 bi[40];
        if (h->kind != H_FILE && h->kind != H_DIR) return iosb(a2, ST_INVALID_PARAMETER, 0);
        if (a4 < 36 || !NT_SUCCESS(CopyFromUser(bi, (const void *)(uintptr_t)a3, 36)))
            return iosb(a2, ST_INVALID_PARAMETER, 0);
        INT64 ct, wt;
        UINT32 attr;
        memcpy(&ct, bi, 8); memcpy(&wt, bi + 16, 8); memcpy(&attr, bi + 32, 4);
        if (ct > 0) h->node->ctime = (UINT64)ct;            /* 0: unchanged, -1: stop updating */
        if (wt > 0) h->node->mtime = (UINT64)wt;
        if (attr) h->node->attrs = attr & 0x07;
        if (ct > 0 || wt > 0 || attr) RamfsMarkChanged(h->node);
        return iosb(a2, ST_SUCCESS, 0);
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

/* DOS wildcard rules (FindFirstFile's patterns): a trailing ".*" also
 * matches a name with no extension ("*.*" is everything), and a trailing
 * "." matches only names without one */
static bool wild_dos(const char *pat, const char *s)
{
    if (wild(pat, s)) return true;
    UINT32 n = (UINT32)strlen(pat);
    if (strchr(s, '.')) return false;
    char base[RAMFS_NAME_MAX];
    if (n >= 2 && pat[n - 2] == '.' && pat[n - 1] == '*' && n - 2 < sizeof(base)) {
        memcpy(base, pat, n - 2);
        base[n - 2] = 0;
        return wild(base, s);
    }
    if (n >= 1 && pat[n - 1] == '.' && n - 1 < sizeof(base)) {
        memcpy(base, pat, n - 1);
        base[n - 1] = 0;
        return wild(base, s);
    }
    return false;
}

/* NtQueryDirectoryFile(HANDLE, Event, ApcRoutine, ApcContext,
 *   PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length,
 *   FILE_INFORMATION_CLASS, BOOLEAN ReturnSingleEntry,
 *   PUNICODE_STRING FileName, BOOLEAN RestartScan)
 * FileDirectoryInformation (1), FileFullDirectoryInformation (2),
 * FileBothDirectoryInformation (3), FileNamesInformation (12),
 * FileIdBothDirectoryInformation (37), FileIdFullDirectoryInformation (38):
 * as many entries as fit (one if ReturnSingleEntry). */
static UINT32 dir_name_offset(UINT32 cls)
{
    switch (cls) {
    case 1: return 64;
    case 2: return 68;
    case 3: return 94;
    case 12: return 12;
    case 37: return 104;
    case 38: return 80;
    default: return 0;
    }
}

static UINT64 sys_query_directory_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    UINT64 iosb_ptr = um_stack_arg(5), out = um_stack_arg(6);
    UINT32 len = (UINT32)um_stack_arg(7), cls = (UINT32)um_stack_arg(8);
    bool single = um_stack_arg(9) & 0xFF;
    UINT64 name_ptr = um_stack_arg(10);
    bool restart = um_stack_arg(11) & 0xFF;
    UmHandle *h = handle(p, a1);
    if (!h || h->kind != H_DIR) return ST_INVALID_HANDLE;
    UINT32 name_off = dir_name_offset(cls);
    if (!name_off) return iosb(iosb_ptr, ST_INVALID_INFO_CLASS, 0);
    char pat[RAMFS_NAME_MAX] = "*";
    if (name_ptr) {
        UINT64 us[2];
        UINT16 w[RAMFS_NAME_MAX];
        if (NT_SUCCESS(CopyFromUser(us, (const void *)(uintptr_t)name_ptr, 16))) {
            UINT32 n = (UINT32)(us[0] & 0xFFFF) / 2;
            if (n && n < RAMFS_NAME_MAX && NT_SUCCESS(CopyFromUser(w, (const void *)(uintptr_t)us[1], 2 * n))) {
                w2u(w, n, pat, sizeof(pat));
            }
        }
    }
    if (restart) h->pos = 0;
    UINT8 *b = kzalloc(len < 65536 ? len + 8 : 65536 + 8);
    if (!b) return iosb(iosb_ptr, ST_NO_MEMORY, 0);
    UINT32 cap = len < 65536 ? len : 65536, used = 0, last = 0, count = 0;
    UINT32 st = ST_SUCCESS;
    DesktopLock();
    /* h->pos: the next entry, "." and ".." first as Windows lists them in
     * every directory but a drive's root */
    UINT64 dots = h->node->parent ? 2 : 0, k = h->pos;
    RamNode *c = h->node->child;
    for (UINT64 i = dots; c && i < k; i++) c = c->next;
    for (;; k++) {
        RamNode *n = k < dots ? (k == 0 ? h->node : h->node->parent) : c;
        if (!n) break;
        const char *name = k < dots ? (k == 0 ? "." : "..") : c->name;
        if (k >= dots) c = c->next;
        if (!wild_dos(pat, name)) continue;
        UINT32 nl = (UINT32)strlen(name);
        UINT32 need = name_off + 2 * nl;
        UINT32 at = (used + 7) & ~7u;
        if (at + need > cap) {
            if (!count) st = ST_BUFFER_OVERFLOW;
            break;                                           /* (this entry comes next time) */
        }
        UINT8 *e = b + at;
        memset(e, 0, need);
        if (cls == 12) {
            nl = (UINT32)u2w(name, e + 12, nl);
            UINT32 bytes = 2 * nl;
            memcpy(e + 8, &bytes, 4);
        } else {
            UINT8 bi[40];
            basic_info(bi, n);
            memcpy(e + 8, bi, 32);                           /* times */
            UINT64 size = n->dir ? 0 : n->size, alloc = (size + 4095) & ~4095ULL;
            memcpy(e + 40, &size, 8);
            memcpy(e + 48, &alloc, 8);
            memcpy(e + 56, bi + 32, 4);                      /* attributes */
            nl = (UINT32)u2w(name, e + name_off, nl);
            UINT32 bytes = 2 * nl;
            memcpy(e + 60, &bytes, 4);
            if (cls == 37 || cls == 38) {
                UINT64 id = (UINT64)(uintptr_t)n;
                memcpy(e + (cls == 37 ? 96 : 72), &id, 8);
            }
        }
        need = name_off + 2 * nl;
        if (count) { UINT32 next = at - last; memcpy(b + last, &next, 4); }
        last = at;
        used = at + need;
        count++;
        if (single) { k++; break; }
    }
    h->pos = k;
    DesktopUnlock();
    if (!count && st == ST_SUCCESS) st = ST_NO_MORE_FILES;
    if (count && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)out, b, used))) { kfree(b); return UM_STATUS_ACCESS_VIOLATION; }
    kfree(b);
    return iosb(iosb_ptr, st, count ? used : 0);
}

/* NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
 *                              FS_INFORMATION_CLASS): FileFsDeviceInformation */
static UINT64 sys_query_volume_locked(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UmHandle *h = handle(p, a1);
    if (!h) return ST_INVALID_HANDLE;
    UINT32 cls = (UINT32)um_stack_arg(5);
    if (cls != 4) {
        /* the volume of drive C: (a FAT32-like volume: no ACLs, no hard links) */
        if (h->kind != H_FILE && h->kind != H_DIR) return iosb(a2, 0xC0000010u /* INVALID_DEVICE_REQUEST */, 0);
        UINT8 b[64];
        UINT32 need;
        memset(b, 0, sizeof(b));
        UINT64 total = UINT64_C(1) << 20, avail = UINT64_C(1) << 19;   /* 4 KiB units: 4 GiB, 2 GiB free */
        switch (cls) {
        case 1: {                                               /* FileFsVolumeInformation */
            memcpy(b, &g_boot_time, 8);
            UINT32 serial = 0x4E4F5641u, ll = 12;               /* "NOVA", "NovaOS" */
            memcpy(b + 8, &serial, 4); memcpy(b + 12, &ll, 4);
            u2w("NovaOS", b + 18, 6);
            need = 18 + 12;
            break;
        }
        case 3:                                                 /* FileFsSizeInformation */
            memcpy(b, &total, 8); memcpy(b + 8, &avail, 8);
            b[16] = 8; b[21] = 2;                               /* 8 sectors of 512 */
            need = 24;
            break;
        case 5: {                                               /* FileFsAttributeInformation */
            UINT32 attrs = 0x6, maxc = 255, nl = 10;            /* CASE_PRESERVED_NAMES | UNICODE_ON_DISK */
            memcpy(b, &attrs, 4); memcpy(b + 4, &maxc, 4); memcpy(b + 8, &nl, 4);
            u2w("FAT32", b + 12, 5);
            need = 12 + 10;
            break;
        }
        case 7:                                                 /* FileFsFullSizeInformation */
            memcpy(b, &total, 8); memcpy(b + 8, &avail, 8); memcpy(b + 16, &avail, 8);
            b[24] = 8; b[29] = 2;
            need = 32;
            break;
        case 11: {                                              /* FileFsSectorSizeInformation */
            UINT32 v[7] = { 512, 512, 512, 512, 0, 0, 0 };
            memcpy(b, v, 28);
            need = 28;
            break;
        }
        default:
            return iosb(a2, ST_INVALID_INFO_CLASS, 0);
        }
        UINT32 n = a4 < need ? (UINT32)a4 : need;
        if (a4 < 8) return iosb(a2, ST_INFO_LENGTH_MISMATCH, 0);
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, n))) return UM_STATUS_ACCESS_VIOLATION;
        return iosb(a2, n < need ? ST_BUFFER_OVERFLOW : ST_SUCCESS, n);
    }
    if (a4 < 8) return iosb(a2, ST_INFO_LENGTH_MISMATCH, 0);
    UINT32 dev[2] = { (h->kind == H_CON_IN || h->kind == H_CON_OUT) ? 0x50u /* CONSOLE */ :
                      h->kind == H_NULL ? 0x15u /* NULL */ :
                      (h->kind == H_OBJECT && h->obj->type == UO_PIPE) ? 0x11u /* NAMED_PIPE */ :
                      h->kind == H_OBJECT ? 0x22u /* UNKNOWN */ : 0x07u /* DISK */, 0 };
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, dev, 8))) return UM_STATUS_ACCESS_VIOLATION;
    return iosb(a2, ST_SUCCESS, 8);
}

/* NtQueryInformationFile on a pipe */
static UINT64 pipe_query(UmObject *po, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT32 cls = (UINT32)um_stack_arg(5), len = (UINT32)a4, got = 0;
    UINT8 b[256];
    UINT32 st = um_pipe_query(po, cls, b, len < sizeof(b) ? len : sizeof(b), &got);
    um_ob_unref(po);
    if (NT_SUCCESS(st) || st == ST_BUFFER_OVERFLOW)
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, got))) return UM_STATUS_ACCESS_VIOLATION;
    return iosb(a2, st, got);
}

static UINT64 sys_query_info_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *po = um_handle_object(UmCurrent(), a1, UO_PIPE);
    if (po) return pipe_query(po, a2, a3, a4);
    DesktopLock();
    UINT64 r = sys_query_info_file_locked(a1, a2, a3, a4);
    DesktopUnlock();
    return r;
}

static UINT64 sys_set_info_file(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *po = um_handle_object(UmCurrent(), a1, UO_PIPE);
    if (po) {                                       /* FilePipeInformation: read mode, completion mode */
        UINT32 v[2], st;
        if ((UINT32)um_stack_arg(5) != 23) st = ST_INVALID_INFO_CLASS;
        else if (a4 < 8 || !NT_SUCCESS(CopyFromUser(v, (const void *)(uintptr_t)a3, 8))) st = ST_INVALID_PARAMETER;
        else st = um_pipe_set_mode(po, v[0], v[1]);
        um_ob_unref(po);
        return iosb(a2, st, 0);
    }
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
static UINT64 alloc_vm(UmProcess *p, UINT64 a2, UINT64 a4, UINT32 type, UINT32 prot, UINT64 lo, UINT64 hi, UINT64 align);

static UINT64 sys_alloc_vm_locked(UmProcess *p, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3;
    return alloc_vm(p, a2, a4, (UINT32)um_stack_arg(5), (UINT32)um_stack_arg(6), p->lay.alloc_min, p->lay.alloc_max, 0);
}

/* NtAllocateVirtualMemoryEx(HANDLE, PVOID *Base, PSIZE_T Size, ULONG Type, ULONG Protect,
 *                           MEM_EXTENDED_PARAMETER *, ULONG Count): an address range too */
static UINT64 sys_alloc_vm_ex(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmObject *ob;
    UmProcess *p = um_proc_of(UmCurrent(), a1, &ob);
    if (!p) return ST_INVALID_HANDLE;
    UINT64 lo = p->lay.alloc_min, hi = p->lay.alloc_max, align = 0;
    UINT64 r;
    if (!um_addr_requirements(um_stack_arg(6), (UINT32)um_stack_arg(7), &lo, &hi, &align)) r = UM_STATUS_ACCESS_VIOLATION;
    else {
        um_lock(&p->lock);
        r = alloc_vm(p, a2, a3, (UINT32)a4, (UINT32)um_stack_arg(5), lo, hi, align);
        um_unlock(&p->lock);
    }
    if (ob) um_ob_unref(ob);
    return r;
}

/* Reserve and/or commit at *@a2 (0: anywhere in [lo, hi) at @align), *@a4 bytes */
static UINT64 alloc_vm(UmProcess *p, UINT64 a2, UINT64 a4, UINT32 type, UINT32 prot, UINT64 lo, UINT64 hi, UINT64 align)
{
    UINT64 base, size;
    if (!get_u64(a2, &base) || !get_u64(a4, &size)) return UM_STATUS_ACCESS_VIOLATION;
    if (!size || !(type & (MEM_COMMIT | MEM_RESERVE)) || !valid_protect(prot)) return ST_INVALID_PARAMETER;
    if (!base) type |= MEM_RESERVE;                             /* committing at no address reserves too */
    UINT64 end = (base + size + 0xFFF) & ~0xFFFULL;

    if (type & MEM_RESERVE) {
        base &= ~0xFFFFULL;
        size = (end - base + 0xFFF) & ~0xFFFULL;
        if (!base) {
            base = um_find_free_aligned(p, size, lo, hi, align);
            if (!base) return ST_NO_MEMORY;
        } else if (!um_is_free(p, base, size)) {
            return ST_CONFLICTING_ADDRESSES;
        }
        if (!um_region_add(p, base, size, prot, false)) return ST_NO_MEMORY;
    } else {
        base &= ~0xFFFULL;
        size = end - base;
        UmRegion *r = um_region_find(p, base);
        if (!r || base + size > r->base + r->size || r->image || r->section) return ST_MEMORY_NOT_ALLOCATED;
    }
    if (type & MEM_COMMIT) {
        if (p->commit + size / PAGE_SIZE > PROC_MEM_LIMIT_PAGES) {
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
static UINT64 sys_free_vm_locked(UmProcess *p, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT64 base, size;
    if (!get_u64(a2, &base) || !get_u64(a3, &size)) return UM_STATUS_ACCESS_VIOLATION;
    UmRegion *r = um_region_find(p, base);
    if (!r || r->image) return ST_MEMORY_NOT_ALLOCATED;
    if (r->section) return 0xC000001Bu;                     /* STATUS_UNABLE_TO_DELETE_SECTION: a mapped view */
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
static UINT64 sys_protect_vm_locked(UmProcess *p, UINT64 a2, UINT64 a3, UINT64 a4)
{
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

/* NtQueryVirtualMemory(HANDLE, PVOID Address, MEMORY_INFORMATION_CLASS (0: basic),
 *                      PVOID Buffer, SIZE_T Length, PSIZE_T ReturnLength) */
static UINT64 sys_query_vm_locked(UmProcess *p, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UINT64 len = um_stack_arg(5), ret_ptr = um_stack_arg(6);
    if (a3 != 0) return ST_INVALID_INFO_CLASS;
    if (len < 48) return ST_INFO_LENGTH_MISMATCH;
    UINT64 top = p->wow ? UINT64_C(0x80000000) : UM_ALLOC_MAX + UINT64_C(0x2000000000);   /* end of user space */
    if (a2 >= top) return ST_INVALID_PARAMETER;
    struct {
        UINT64 base, alloc_base;
        UINT32 alloc_protect; UINT16 partition, pad;
        UINT64 size;
        UINT32 state, protect, type, pad2;
    } mbi;
    memset(&mbi, 0, sizeof(mbi));
    UINT64 va = a2 & ~0xFFFULL;
    mbi.base = va;
    UmRegion *r = um_region_find(p, va);
    if (!r) {
        /* free: up to the next region */
        UINT64 next = top;
        for (int i = 0; i < p->nregions; i++)
            if (p->regions[i].base > va && p->regions[i].base < next) next = p->regions[i].base;
        mbi.size = next - va;
        mbi.state = 0x10000;                                    /* MEM_FREE */
        mbi.protect = 0x01;
    } else {
        bool c = um_is_committed(p, va);
        UINT64 end = va + PAGE_SIZE, lim = r->base + r->size;
        /* a run of pages in the same state (bounded, so huge reservations stay cheap) */
        for (int k = 0; end < lim && k < 65536 && um_is_committed(p, end) == c; k++) end += PAGE_SIZE;
        if (end < lim && um_is_committed(p, end) == c) end = lim;
        mbi.alloc_base = r->base;
        mbi.alloc_protect = r->image ? 0x80 : r->protect;       /* images: EXECUTE_WRITECOPY */
        mbi.size = end - va;
        mbi.state = c ? 0x1000 : 0x2000;                        /* MEM_COMMIT / MEM_RESERVE */
        mbi.protect = c ? r->protect : 0;
        if (c && r->image) mbi.protect = 0x20;                  /* report images as EXECUTE_READ */
        mbi.type = r->image ? 0x1000000 : r->section ? 0x40000 : 0x20000;   /* MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE */
    }
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, &mbi, 48))) return UM_STATUS_ACCESS_VIOLATION;
    if (ret_ptr) put_u64(ret_ptr, 48);
    return ST_SUCCESS;
}

/* Run a memory service on the process @a1 names (this one or another,
 * through a process handle), under that process's lock */
static UINT64 on_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4,
                         UINT64 (*fn)(UmProcess *, UINT64, UINT64, UINT64))
{
    UmObject *ob;
    UmProcess *p = um_proc_of(UmCurrent(), a1, &ob);
    if (!p) return ST_INVALID_HANDLE;
    um_lock(&p->lock);
    UINT64 r = fn(p, a2, a3, a4);
    um_unlock(&p->lock);
    if (ob) um_ob_unref(ob);
    return r;
}

static UINT64 sys_query_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)   { return on_process(a1, a2, a3, a4, sys_query_vm_locked); }
static UINT64 sys_alloc_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)   { return on_process(a1, a2, a3, a4, sys_alloc_vm_locked); }
static UINT64 sys_free_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)    { return on_process(a1, a2, a3, a4, sys_free_vm_locked); }
static UINT64 sys_protect_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { return on_process(a1, a2, a3, a4, sys_protect_vm_locked); }

/* NtReadVirtualMemory / NtWriteVirtualMemory(HANDLE Process, PVOID Base,
 * PVOID Buffer, SIZE_T Size, PSIZE_T Done): copy between this process and
 * another (or itself), a page-sized bounce at a time */
#define ST_PARTIAL_COPY 0x8000000Du
static UINT64 copy_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4, bool write)
{
    UmObject *ob;
    UmProcess *p = um_proc_of(UmCurrent(), a1, &ob);
    if (!p) return ST_INVALID_HANDLE;
    UINT8 *k = kmalloc(PAGE_SIZE);
    UINT64 done = 0;
    UINT32 st = k ? ST_SUCCESS : ST_NO_MEMORY;
    while (!st && done < a4) {
        UINT64 n = a4 - done;
        UINT64 room = PAGE_SIZE - ((a2 + done) & 0xFFF);
        if (n > room) n = room;
        void *u = (void *)(uintptr_t)(a3 + done);
        if (write) {
            if (!NT_SUCCESS(CopyFromUser(k, u, n))) { st = UM_STATUS_ACCESS_VIOLATION; break; }
            um_lock(&p->lock);
            bool ok = um_region_find(p, a2 + done) && um_write(p, a2 + done, k, n);
            um_unlock(&p->lock);
            if (!ok) { st = ST_PARTIAL_COPY; break; }
        } else {
            um_lock(&p->lock);
            bool ok = um_region_find(p, a2 + done) && um_read(p, a2 + done, k, n);
            um_unlock(&p->lock);
            if (!ok) { st = ST_PARTIAL_COPY; break; }
            if (!NT_SUCCESS(CopyToUser(u, k, n))) { st = UM_STATUS_ACCESS_VIOLATION; break; }
        }
        done += n;
    }
    kfree(k);
    if (ob) um_ob_unref(ob);
    UINT64 dp = um_stack_arg(5);
    if (dp) put_u64(dp, done);
    return st;
}
static UINT64 sys_read_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)  { return copy_vm(a1, a2, a3, a4, false); }
static UINT64 sys_write_vm(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) { return copy_vm(a1, a2, a3, a4, true); }

/* -----------------------------------------------------------------------
 * Process, time
 * ----------------------------------------------------------------------- */
/* A NUL-terminated string from user memory; false if unreadable or too long */
static bool get_str(UINT64 ptr, char *out, int cap)
{
    for (int i = 0; i < cap; i++) {
        if (!NT_SUCCESS(CopyFromUser(&out[i], (const void *)(uintptr_t)(ptr + (UINT64)i), 1))) return false;
        if (!out[i]) return true;
    }
    return false;
}

/* A process created by a program: the handle holds the creator's claim */
static void process_ob_destroy(UmObject *o)
{
    IrqState s = ob_lock();
    UmProcess *c = o->proc;
    if (c) c->exit_ob = NULL;
    ob_unlock(s);
    if (c) UmDetach(c);                                         /* reclaimed once it exits */
}

/* NtNovaCreateProcess(PCSTR Image, PCSTR CommandLine, PCSTR CurrentDirectory,
 *                     NOVA_CREATE_PROCESS *io)
 * UTF-8 strings, full paths ("C:\dir\prog.exe").  io: in: StdHandle[3]
 * (0 = the console), Flags (1: inherit handles, 2: no console),
 * Environment + EnvironmentSize (UTF-8 "NAME=value\0...\0"; NULL: the
 * default), RuntimeData + RuntimeDataSize (STARTUPINFO.lpReserved2);
 * out: Process, Thread, ProcessId, ThreadId.  The new process
 * shares the creator's console. */
#define NCP_INHERIT    1u
#define NCP_NO_CONSOLE 2u
#define NCP_ENV_MAX    (64 * 1024)

static bool std_kind(UmHandleKind k) { return k == H_FILE || k == H_CON_IN || k == H_CON_OUT || k == H_OBJECT || k == H_NULL; }

static UINT64 sys_nova_create_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    char image[RAMFS_PATH_MAX], dir[RAMFS_PATH_MAX], *cmd = NULL;
    UINT64 io[12];
    if (!get_str(a1, image, sizeof(image)) || (a3 && !get_str(a3, dir, sizeof(dir))) ||
        !NT_SUCCESS(CopyFromUser(io, (const void *)(uintptr_t)a4, sizeof(io))))
        return UM_STATUS_ACCESS_VIOLATION;
    if (!a3) dir[0] = 0;
    UINT32 flags = (UINT32)io[7];
    char *env = NULL;
    UINT32 env_len = (UINT32)io[9];
    if (io[8]) {
        if (env_len < 2 || env_len > NCP_ENV_MAX) return ST_INVALID_PARAMETER;
        env = kmalloc(env_len);
        if (!env) return ST_NO_MEMORY;
        if (!NT_SUCCESS(CopyFromUser(env, (const void *)(uintptr_t)io[8], env_len))) { kfree(env); return UM_STATUS_ACCESS_VIOLATION; }
    }
    UINT8 *rt = NULL;
    UINT32 rt_len = (UINT32)io[11];
    if (io[10] && rt_len) {
        if (rt_len > 65535) { kfree(env); return ST_INVALID_PARAMETER; }
        rt = kmalloc(rt_len);
        if (!rt) { kfree(env); return ST_NO_MEMORY; }
        if (!NT_SUCCESS(CopyFromUser(rt, (const void *)(uintptr_t)io[10], rt_len))) { kfree(rt); kfree(env); return UM_STATUS_ACCESS_VIOLATION; }
    }
    cmd = kmalloc(8192);
    if (!cmd) { kfree(rt); kfree(env); return ST_NO_MEMORY; }
    if (a2 && !get_str(a2, cmd, 8192)) { kfree(cmd); kfree(rt); kfree(env); return UM_STATUS_ACCESS_VIOLATION; }
    if (!a2) strncpy(cmd, image, 8191);
    UmHandle *inh = (flags & NCP_INHERIT) ? kzalloc(sizeof(UmHandle) * UM_MAX_HANDLES) : NULL;
    if ((flags & NCP_INHERIT) && !inh) { kfree(cmd); kfree(rt); kfree(env); return ST_NO_MEMORY; }

    const char *ip = image, *dp = dir;
    if ((ip[0] | 0x20) == 'c' && ip[1] == ':') ip += 2;
    if ((dp[0] | 0x20) == 'c' && dp[1] == ':') dp += 2;
    char err[128];
    UINT32 st = ST_SUCCESS;
    UmObject *o = kzalloc(sizeof(*o));
    UmProcess *c = NULL;
    if (!o) st = ST_NO_MEMORY;
    DesktopLock();                                  /* lock order: desktop, then process */
    RamNode *exe = st ? NULL : RamfsResolve(NULL, ip);
    RamNode *cwd = dp[0] ? RamfsResolve(NULL, dp) : p->cwd;
    if (!cwd || !cwd->dir) cwd = p->cwd;
    if (!st && (!exe || exe->dir)) st = ST_OBJECT_NAME_NOT_FOUND;
    /* Map the images first, letting go of the desktop lock meanwhile (the
     * program stays pinned, the folder referenced) */
    bool pinned = false;
    if (!st) {
        RamfsPin(exe);
        RamfsRef(cwd);
        pinned = true;
        c = um_spawn_image(exe, cwd, (flags & NCP_NO_CONSOLE) ? NULL : p->con, true, err, sizeof(err));
        if (!c) {
            kprintf("[UM] %s (PID %u): CreateProcess(%s) failed: %s\n", p->name, p->pid, image, err);
            st = strstr(err, "not found") ? 0xC0000135u : strstr(err, "memory") ? ST_NO_MEMORY : 0xC000007Bu;
        }
    }
    um_lock(&p->lock);                              /* the handles stay put while they are copied */
    UmSpawnOpts opts;
    UmHandle std[3];
    memset(&opts, 0, sizeof(opts));
    memset(std, 0, sizeof(std));
    for (int i = 0; inh && i < UM_MAX_HANDLES; i++)
        if (p->handles[i].inherit && p->handles[i].kind != H_FREE) inh[i] = p->handles[i];
    for (int i = 0; i < 3; i++) {
        UmHandle *h = io[i] ? handle(p, io[i]) : NULL;
        if (!h || !std_kind(h->kind)) continue;
        if (inh && inh[io[i] / 4 - 1].kind != H_FREE) opts.std_value[i] = io[i];   /* the same handle */
        else std[i] = *h;
    }
    opts.std = std;
    opts.inherit = inh;
    opts.env = env;
    opts.env_len = env_len;
    opts.runtime = rt;
    opts.runtime_len = rt ? rt_len : 0;
    if (!st) {
        c = um_spawn_finish(c, exe, cmd, &opts, err, sizeof(err));
        if (!c) {
            kprintf("[UM] %s (PID %u): CreateProcess(%s) failed: %s\n", p->name, p->pid, image, err);
            st = strstr(err, "memory") ? ST_NO_MEMORY : 0xC000007Bu;
        }
    }
    um_unlock(&p->lock);
    if (pinned) { RamfsUnpin(exe); RamfsUnref(cwd); }
    DesktopUnlock();
    kfree(cmd);
    kfree(env);
    kfree(rt);
    kfree(inh);
    if (st) { kfree(o); return st; }

    o->type = UO_PROCESS;
    o->refs = 1;
    o->proc = c;
    o->destroy = process_ob_destroy;
    IrqState s = ob_lock();
    c->exit_ob = o;
    if (c->exited) o->signaled = true;
    ob_unlock(s);
    UINT64 hp = um_handle_new_object(p, o);
    UmThread *t0 = c->threads[0];
    UINT64 ht = t0 ? um_handle_new_object(p, &t0->ob) : 0;
    um_ob_unref(o);                                             /* the handle holds it now */
    if (!hp) return ST_TOO_MANY_HANDLES;
    UINT64 out[4] = { hp, ht, c->pid, t0 ? t0->tid : 0 };
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a4 + 24), out, sizeof(out))) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
}

/* NtNovaProcessInfo(HANDLE Process, ULONG64 Out[3]): process id, exit code
 * (STILL_ACTIVE while it runs), exited */
static UINT64 sys_nova_process_info(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    UINT64 out[3];
    if (a1 == UINT64_C(0xFFFFFFFFFFFFFFFF)) {
        out[0] = p->pid; out[1] = 0x103; out[2] = 0;
    } else {
        UmObject *o = um_handle_object(p, a1, UO_PROCESS);
        if (!o) return ST_INVALID_HANDLE;
        IrqState s = ob_lock();
        UmProcess *c = o->proc;
        out[0] = c ? c->pid : 0;
        out[2] = o->signaled;
        out[1] = !c ? (UINT32)o->count : c->exited ? c->exit_status : 0x103;
        ob_unlock(s);
        um_ob_unref(o);
    }
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, out, sizeof(out))) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
}

/* NtNovaProcessList(NOVA_PROCESS_ENTRY *Buffer, ULONG Max, PULONG Count):
 * { ULONG Pid, MemoryKb, Threads, Exited; CHAR Name[32] } per program */
static UINT64 sys_nova_process_list(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    static UmProcInfo list[UM_MAX_PROCS];                       /* under the lock below */
    static UmLock lk;
    um_lock(&lk);
    int n = UmList(list, UM_MAX_PROCS);
    UINT32 st = ST_SUCCESS;
    for (int i = 0; i < n && (UINT64)i < a2; i++) {
        UINT8 e[48];
        memset(e, 0, sizeof(e));
        memcpy(e, &list[i].pid, 4);
        memcpy(e + 4, &list[i].mem_kb, 4);
        memcpy(e + 8, &list[i].threads, 4);
        UINT32 ex = list[i].exited;
        memcpy(e + 12, &ex, 4);
        memcpy(e + 16, list[i].name, 31);
        if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a1 + 48 * (UINT64)i), e, 48))) { st = UM_STATUS_ACCESS_VIOLATION; break; }
    }
    um_unlock(&lk);
    UINT32 cnt = (UINT32)n;
    if (a3 && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, &cnt, 4))) return UM_STATUS_ACCESS_VIOLATION;
    return st;
}

static UINT64 sys_terminate_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a1 != 0 && a1 != UINT64_C(0xFFFFFFFFFFFFFFFF)) {
        UmObject *o = um_handle_object(p, a1, UO_PROCESS);
        if (!o) return ST_INVALID_HANDLE;
        if (o->proc) UmKill(o->proc, (UINT32)a2);
        um_ob_unref(o);
        return ST_SUCCESS;
    }
    um_exit_process((UINT32)a2);
}

extern long long nova_time(long long *t);         /* net/tls_platform.c: RTC as Unix time */
static UINT64 g_tsc0, g_tsc_hz;

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
        UINT64 hz = g_tsc_per_tick * 100;           /* measured against the PIT at boot */
        if (!hz) {
            UINT64 dt = sched_ticks() - g_boot_ticks;
            if (dt < 50) {                          /* too early: wait for 0.5 s of ticks */
                while (sched_ticks() - g_boot_ticks < 50) sched_yield();
                dt = sched_ticks() - g_boot_ticks;
            }
            hz = (rdtsc() - g_tsc0) / dt * 100;
        }
        __atomic_store_n(&g_tsc_hz, hz, __ATOMIC_RELAXED);
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
    if (!wait_100ns) { sched_yield(); return ST_SUCCESS; }          /* Sleep(0): just yield */
    UINT64 until = sched_ticks() + (wait_100ns + 99999) / 100000;
    do {
        if (um_stopping()) break;
        sched_sleep_tick();
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
/* -----------------------------------------------------------------------
 * Pipes, cancelling, handle flags
 * ----------------------------------------------------------------------- */
/* NtCreateNamedPipeFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
 *   PIO_STATUS_BLOCK, ULONG ShareAccess, ULONG CreateDisposition,
 *   ULONG CreateOptions, ULONG NamedPipeType, ULONG ReadMode,
 *   ULONG CompletionMode, ULONG MaximumInstances, ULONG InboundQuota,
 *   ULONG OutboundQuota, PLARGE_INTEGER DefaultTimeout) */
static UINT64 sys_create_named_pipe(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    char path[RAMFS_PATH_MAX];
    UINT32 attrs;
    UINT32 st = um_get_path(p, a3, path, sizeof(path), &attrs);
    if (st) return iosb(a4, st, 0);
    UmObject *o;
    bool rd, wr;
    st = um_pipe_create(path, (UINT32)a2, (UINT32)um_stack_arg(6), (UINT32)um_stack_arg(7),
                        (UINT32)um_stack_arg(8), (UINT32)um_stack_arg(9), (UINT32)um_stack_arg(10),
                        (UINT32)um_stack_arg(11), (UINT32)um_stack_arg(12), (UINT32)um_stack_arg(13),
                        &o, &rd, &wr);
    if (st) return iosb(a4, st, 0);
    UmHandle *h;
    um_lock(&p->lock);
    UINT64 hv = handle_alloc(p, &h);
    if (hv) { h->kind = H_OBJECT; h->obj = o; h->read = rd; h->write = wr; h->inherit = attrs & 0x2; }
    um_unlock(&p->lock);
    if (!hv) { um_ob_unref(o); return iosb(a4, ST_TOO_MANY_OPENED_FILES, 0); }
    if (!put_u64(a1, hv)) { um_close_handle(hv); return UM_STATUS_ACCESS_VIOLATION; }
    return iosb(a4, ST_SUCCESS, 2);                             /* FILE_CREATED */
}

/* NtFsControlFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
 *   PIO_STATUS_BLOCK, ULONG Code, PVOID In, ULONG InLength, PVOID Out,
 *   ULONG OutLength): the named-pipe controls */
static UINT64 sys_fs_control(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    UINT64 iosb_ptr = um_stack_arg(5);
    UINT32 code = (UINT32)um_stack_arg(6);
    UmObject *po = a1 ? um_handle_object(p, a1, UO_PIPE) : NULL;
    if (!po && code != 0x110018u /* FSCTL_PIPE_WAIT */) {
        if (a1 && !um_handle_object_exists(p, a1)) return ST_INVALID_HANDLE;
        return iosb(iosb_ptr, 0xC0000010u /* STATUS_INVALID_DEVICE_REQUEST */, 0);
    }
    UINT64 info = 0;
    UINT32 st = um_pipe_fsctl(po, a2, iosb_ptr, code, um_stack_arg(7), (UINT32)um_stack_arg(8),
                              um_stack_arg(9), (UINT32)um_stack_arg(10), &info);
    if (po) um_ob_unref(po);
    return st == ST_PENDING ? st : iosb(iosb_ptr, st, info);
}

/* NtCancelIoFile(HANDLE, PIO_STATUS_BLOCK) */
static UINT64 sys_cancel_io(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmObject *po = um_handle_object(UmCurrent(), a1, UO_PIPE);
    UINT32 st = po ? um_pipe_cancel(po, 0) : ST_SUCCESS;
    if (po) um_ob_unref(po);
    return iosb(a2, st, 0);
}

/* NtCancelIoFileEx(HANDLE, PIO_STATUS_BLOCK Request, PIO_STATUS_BLOCK) */
static UINT64 sys_cancel_io_ex(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmObject *po = um_handle_object(UmCurrent(), a1, UO_PIPE);
    UINT32 st = po ? um_pipe_cancel(po, a2) : 0xC0000225u;    /* STATUS_NOT_FOUND */
    if (po) um_ob_unref(po);
    return iosb(a3, st, 0);
}

bool um_handle_object_exists(UmProcess *p, UINT64 hv)
{
    um_lock(&p->lock);
    bool ok = handle(p, hv) != NULL;
    um_unlock(&p->lock);
    return ok;
}

/* NtOpenProcess(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID) */
static UINT64 sys_open_process(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    UmProcess *p = UmCurrent();
    UINT64 cid[2];
    if (!a4 || !NT_SUCCESS(CopyFromUser(cid, (const void *)(uintptr_t)a4, sizeof(cid)))) return UM_STATUS_ACCESS_VIOLATION;
    UmObject *o = um_open_process((UINT32)cid[0]);
    if (!o) return ST_INVALID_PARAMETER;                       /* (as Windows: no such process) */
    UINT64 h = um_handle_new_object(p, o);
    um_ob_unref(o);
    if (!h) return ST_TOO_MANY_HANDLES;
    return put_u64(a1, h) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
}

/* NtOpenThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, CLIENT_ID *) */
static UINT64 sys_open_thread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3;
    UINT64 cid[2];
    if (!a4 || !NT_SUCCESS(CopyFromUser(cid, (const void *)(uintptr_t)a4, sizeof(cid)))) return UM_STATUS_ACCESS_VIOLATION;
    UmObject *o = um_open_thread((UINT32)cid[1]);
    if (!o) return ST_INVALID_PARAMETER;
    UINT64 h = um_handle_new_object(UmCurrent(), o);
    um_ob_unref(o);
    if (!h) return ST_TOO_MANY_HANDLES;
    return put_u64(a1, h) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
}

/* NtSetInformationObject(HANDLE, ObjectHandleFlagInformation (4),
 *   { BOOLEAN Inherit, ProtectFromClose }, ULONG) */
static UINT64 sys_set_info_object(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT8 v[2];
    if (a2 != 4) return ST_INVALID_INFO_CLASS;
    if (a4 < 2) return ST_INFO_LENGTH_MISMATCH;
    if (!NT_SUCCESS(CopyFromUser(v, (const void *)(uintptr_t)a3, 2))) return UM_STATUS_ACCESS_VIOLATION;
    um_lock(&p->lock);
    UmHandle *h = handle(p, a1);
    if (h) h->inherit = v[0] != 0;
    um_unlock(&p->lock);
    return h ? ST_SUCCESS : ST_INVALID_HANDLE;
}

/* NtQueryObject(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG):
 * ObjectHandleFlagInformation (4) */
/* NtQueryObject(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG):
 * 0 basic, 1 name ("\Device\HarddiskVolume1\dir\file", "\Device\NamedPipe\x",
 * "\BaseNamedObjects\x"...), 2 type ("File", "Event"...), 4 handle flags */
static UINT64 sys_query_object(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UINT64 ret = um_stack_arg(5);
    if (a2 != 0 && a2 != 1 && a2 != 2 && a2 != 4) return ST_INVALID_INFO_CLASS;
    char name[RAMFS_PATH_MAX + 64];
    const char *type = "File";
    bool inherit = false;
    name[0] = 0;
    DesktopLock();
    um_lock(&p->lock);
    UmHandle *h = handle(p, a1);
    UmObject *o = NULL;
    if (h) {
        inherit = h->inherit;
        if ((h->kind == H_FILE || h->kind == H_DIR) && h->node) {
            char path[RAMFS_PATH_MAX];
            RamfsPath(h->node, path, sizeof(path));
            ksnprintf(name, sizeof(name), "\\Device\\HarddiskVolume1%s", path + 2);   /* (without "C:") */
            if (h->kind == H_DIR && !strcmp(name, "\\Device\\HarddiskVolume1\\")) name[strlen(name) - 1] = 0;
        } else if (h->kind == H_CON_IN || h->kind == H_CON_OUT) {
            strcpy(name, h->kind == H_CON_IN ? "\\Device\\ConDrv\\Input" : "\\Device\\ConDrv\\Output");
        } else if (h->kind == H_NULL) {
            strcpy(name, "\\Device\\Null");
        } else if (h->kind == H_OBJECT) {
            o = um_ob_ref(h->obj);
        }
    }
    um_unlock(&p->lock);
    DesktopUnlock();
    if (!h) return ST_INVALID_HANDLE;
    if (o) {
        char n[160];
        switch (o->type) {
        case UO_PIPE:
            um_pipe_end_name(o, n, sizeof(n));
            ksnprintf(name, sizeof(name), "\\Device\\NamedPipe\\%s", n);
            break;
        case UO_KEY: type = "Key"; break;
        case UO_PROCESS: type = "Process"; break;
        case UO_THREAD: type = "Thread"; break;
        case UO_SOCKET: strcpy(name, "\\Device\\Afd"); break;
        default:
            type = o->type == UO_EVENT ? "Event" : o->type == UO_MUTANT ? "Mutant" : o->type == UO_SEMAPHORE ? "Semaphore" :
                   o->type == UO_SECTION ? "Section" : o->type == UO_DIRECTORY ? "Directory" :
                   o->type == UO_SYMLINK ? "SymbolicLink" : o->type == UO_TIMER ? "Timer" : o->type == UO_WINDOW ? "Window" : "Unknown";
            um_object_name(o, n, sizeof(n));
            if (n[0]) ksnprintf(name, sizeof(name), "%s%s", n[0] == '\\' ? "" : "\\BaseNamedObjects\\", n);
            break;
        }
        um_ob_unref(o);
    }
    if (a2 == 4) {                                              /* ObjectHandleFlagInformation */
        UINT8 v[2] = { inherit, 0 };
        if (ret) { UINT32 n = 2; if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)ret, &n, 4))) return UM_STATUS_ACCESS_VIOLATION; }
        if (a4 < 2) return ST_INFO_LENGTH_MISMATCH;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, v, 2)) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
    }
    if (a2 == 0) {                                              /* ObjectBasicInformation */
        UINT32 b[14];
        memset(b, 0, sizeof(b));
        b[0] = inherit ? 2 : 0;                                 /* Attributes: OBJ_INHERIT */
        b[1] = 0x1F01FF;                                        /* GrantedAccess */
        b[2] = 1; b[3] = 2;                                     /* HandleCount, PointerCount */
        if (ret) { UINT32 n = 56; CopyToUser((void *)(uintptr_t)ret, &n, 4); }
        if (a4 < 56) return ST_INFO_LENGTH_MISMATCH;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, b, 56)) ? ST_SUCCESS : UM_STATUS_ACCESS_VIOLATION;
    }
    /* a UNICODE_STRING header (the type information's is 0x68 bytes) and the text after it */
    const char *text = a2 == 1 ? name : type;
    UINT32 head = a2 == 1 ? 16 : 0x68, n = 0;
    UINT16 w[RAMFS_PATH_MAX + 64];
    for (const unsigned char *c = (const unsigned char *)text; *c && n < RAMFS_PATH_MAX + 63; ) {
        UINT32 cp = *c++;
        if (cp >= 0xE0 && c[0] && c[1]) { cp = (cp & 0x0F) << 12 | (UINT32)(c[0] & 0x3F) << 6 | (c[1] & 0x3F); c += 2; }
        else if (cp >= 0xC0 && c[0]) { cp = (cp & 0x1F) << 6 | (c[0] & 0x3F); c++; }
        w[n++] = (UINT16)cp;
    }
    w[n] = 0;
    UINT32 need = head + 2 * (n + 1);
    if (ret) CopyToUser((void *)(uintptr_t)ret, &need, 4);
    if (a4 < need) return a4 < head ? ST_INFO_LENGTH_MISMATCH : 0xC0000023u;   /* BUFFER_TOO_SMALL */
    UINT8 hdr[0x68];
    memset(hdr, 0, sizeof(hdr));
    UINT16 len = (UINT16)(2 * n), max = (UINT16)(2 * n + 2);
    UINT64 buf = n || a2 == 2 ? a3 + head : 0;
    memcpy(hdr, &len, 2);
    memcpy(hdr + 2, &max, 2);
    memcpy(hdr + 8, &buf, 8);
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, hdr, head)) ||
        !NT_SUCCESS(CopyToUser((void *)(uintptr_t)(a3 + head), w, 2 * (n + 1)))) return UM_STATUS_ACCESS_VIOLATION;
    return ST_SUCCESS;
}

/* -----------------------------------------------------------------------
 * The clipboard
 * ----------------------------------------------------------------------- */
/* NtNovaClipboard(ULONG Op, ...):
 *   0 EMPTY (HWND owner)
 *   1 SET   (ULONG fmt, PVOID data, ULONG size, PCSTR name)
 *   2 GET   (ULONG fmt, PVOID buf, ULONG cap, PCSTR name) -> the size, or -1
 *   3 LIST  (-, ClipEntry *buf, ULONG max) -> the count
 *   4 SEQUENCE -> the sequence number;  5 OWNER -> the owner window
 * (a result, not a status) */
#define CLIP_MAX_SIZE (32u * 1024 * 1024)

static bool clip_name(UINT64 ptr, char *name)
{
    name[0] = 0;
    return !ptr || get_str(ptr, name, CLIP_NAME_MAX);
}

static UINT64 sys_clipboard(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    char name[CLIP_NAME_MAX];
    switch (a1) {
    case 0: ClipEmpty(a2); return 0;
    case 1: {
        if (a4 > CLIP_MAX_SIZE || !clip_name(um_stack_arg(5), name)) return (UINT64)-1;
        UINT8 *d = kmalloc(a4 ? (UINT32)a4 : 1);
        if (!d) return (UINT64)-1;
        if (a4 && !NT_SUCCESS(CopyFromUser(d, (const void *)(uintptr_t)a3, a4))) { kfree(d); return (UINT64)-1; }
        return ClipPut((UINT32)a2, name, d, (UINT32)a4) ? 0 : (UINT64)-1;
    }
    case 2: {
        if (!clip_name(um_stack_arg(5), name)) return (UINT64)-1;
        int size = ClipGet((UINT32)a2, name, NULL, 0);
        if (size < 0 || !a3) return (UINT64)(INT64)size;
        UINT32 cap = (UINT32)a4 < (UINT32)size ? (UINT32)a4 : (UINT32)size;
        UINT8 *d = kmalloc(size ? (UINT32)size : 1);
        if (!d) return (UINT64)-1;
        int got = ClipGet((UINT32)a2, name, d, (UINT32)size);
        if (got < (int)cap) cap = got < 0 ? 0 : (UINT32)got;
        bool ok = !cap || NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, d, cap));
        kfree(d);
        return ok ? (UINT64)(INT64)got : (UINT64)-1;
    }
    case 3: {
        ClipEntry e[32];
        int n = ClipList(e, a4 < 32 ? (int)a4 : 32);
        if (n && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)a3, e, sizeof(ClipEntry) * (UINT64)n))) return (UINT64)-1;
        return (UINT64)n;
    }
    case 4: return ClipSequence();
    case 5: return ClipOwner();
    }
    return (UINT64)-1;
}

void um_install(UINT32 num, SYSCALL_HANDLER h)
{
    g_um[num] = h;
}

/* -----------------------------------------------------------------------
 * Directory watches: FindFirstChangeNotification's event is signaled when
 * the watched directory (or, with subtree, anything below it) changes.
 * A watch lasts until the program removes it or closes its event.
 * ----------------------------------------------------------------------- */
#define MAX_WATCHES 64
typedef struct { RamNode *dir; bool subtree; UmObject *ev; } Watch;
static Watch g_watch[MAX_WATCHES];
static KSpinLock g_watch_lock = KSPINLOCK_INIT;

static void fs_changed(RamNode *d)
{
    IrqState s = spin_lock_irqsave(&g_watch_lock);
    for (int i = 0; i < MAX_WATCHES; i++) {
        Watch *w = &g_watch[i];
        if (!w->ev) continue;
        bool hit = w->dir == d;
        for (RamNode *a = d ? d->parent : NULL; !hit && w->subtree && a; a = a->parent) if (a == w->dir) hit = true;
        if (!hit) continue;
        IrqState o = ob_lock();
        w->ev->signaled = true;
        um_ob_wake(w->ev);
        ob_unlock(o);
    }
    spin_unlock_irqrestore(&g_watch_lock, s);
}

/* Take out the watches on @ev (every one whose event only we hold if NULL) */
static void unwatch(UmObject *ev)
{
    Watch gone[MAX_WATCHES];
    int n = 0;
    IrqState s = spin_lock_irqsave(&g_watch_lock);
    for (int i = 0; i < MAX_WATCHES; i++) {
        Watch *w = &g_watch[i];
        if (!w->ev) continue;
        if (ev ? w->ev == ev : __atomic_load_n(&w->ev->refs, __ATOMIC_ACQUIRE) <= 1) { gone[n++] = *w; w->ev = NULL; w->dir = NULL; }
    }
    spin_unlock_irqrestore(&g_watch_lock, s);
    if (!n) return;
    DesktopLock();
    for (int i = 0; i < n; i++) RamfsUnref(gone[i].dir);
    DesktopUnlock();
    for (int i = 0; i < n; i++) um_ob_unref(gone[i].ev);
}

/* NtNovaWatchDirectory(HANDLE Directory, BOOLEAN Subtree, HANDLE Event, ULONG Remove) */
static UINT64 sys_watch_dir(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    UmObject *ev = um_handle_object(p, a3, UO_EVENT);
    if (!ev) return ST_INVALID_HANDLE;
    unwatch(NULL);                                          /* forgotten watches */
    if (a4) { unwatch(ev); um_ob_unref(ev); return ST_SUCCESS; }
    DesktopLock();                                          /* lock order: desktop, then process */
    um_lock(&p->lock);
    UmHandle *hd = handle(p, a1);
    RamNode *dir = hd && (hd->kind == H_DIR || (hd->kind == H_FILE && hd->node && hd->node->dir)) ? hd->node : NULL;
    if (dir) RamfsRef(dir);
    um_unlock(&p->lock);
    DesktopUnlock();
    if (!dir) { um_ob_unref(ev); return ST_INVALID_HANDLE; }
    IrqState s = spin_lock_irqsave(&g_watch_lock);
    int slot = -1;
    for (int i = 0; i < MAX_WATCHES && slot < 0; i++) if (!g_watch[i].ev) slot = i;
    if (slot >= 0) { g_watch[slot].dir = dir; g_watch[slot].subtree = (a2 & 0xFF) != 0; g_watch[slot].ev = ev; }
    spin_unlock_irqrestore(&g_watch_lock, s);
    if (slot < 0) { DesktopLock(); RamfsUnref(dir); DesktopUnlock(); um_ob_unref(ev); return ST_NO_MEMORY; }
    return ST_SUCCESS;
}

void um_syscall_init(void)
{
    g_boot_time = ((UINT64)nova_time(NULL) + UINT64_C(11644473600)) * 10000000ULL;
    g_boot_ticks = sched_ticks();
    g_tsc0 = rdtsc();
    RamfsSetClock(um_now_100ns);
    um_lock_free_init();

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
    um_install(SYSCALL_NtNovaWatchDirectory,       sys_watch_dir);
    um_install(SYSCALL_NtCreateNamedPipeFile,      sys_create_named_pipe);
    um_install(SYSCALL_NtFsControlFile,            sys_fs_control);
    um_install(SYSCALL_NtCancelIoFile,             sys_cancel_io);
    um_install(SYSCALL_NtCancelIoFileEx,           sys_cancel_io_ex);
    um_install(SYSCALL_NtSetInformationObject,     sys_set_info_object);
    um_install(SYSCALL_NtQueryObject,              sys_query_object);
    um_install(SYSCALL_NtNovaClipboard,            sys_clipboard);
    um_install(SYSCALL_NtOpenProcess,              sys_open_process);
    RamfsSetChangeHook(fs_changed);
    um_install(SYSCALL_NtFreeVirtualMemory,        sys_free_vm);
    um_install(SYSCALL_NtProtectVirtualMemory,     sys_protect_vm);
    um_install(SYSCALL_NtQueryVirtualMemory,       sys_query_vm);
    um_install(SYSCALL_NtReadVirtualMemory,        sys_read_vm);
    um_install(SYSCALL_NtOpenThread,               sys_open_thread);
    um_install(SYSCALL_NtAllocateVirtualMemoryEx,  sys_alloc_vm_ex);
    um_install(SYSCALL_NtWriteVirtualMemory,       sys_write_vm);
    um_install(SYSCALL_NtTerminateProcess,         sys_terminate_process);
    um_install(SYSCALL_NtNovaCreateProcess,        sys_nova_create_process);
    um_install(SYSCALL_NtNovaProcessInfo,          sys_nova_process_info);
    um_install(SYSCALL_NtNovaProcessList,          sys_nova_process_list);
    um_install(SYSCALL_NtQuerySystemTime,          sys_query_system_time);
    um_install(SYSCALL_NtQueryPerformanceCounter,  sys_query_perf_counter);
    um_install(SYSCALL_NtDelayExecution,           sys_delay);
    um_install(SYSCALL_NtYieldExecution,           sys_yield);
    um_thread_syscalls_init();
    um_exception_syscalls_init();
    um_registry_syscalls_init();
    um_socket_syscalls_init();
    um_gui_syscalls_init();
}
