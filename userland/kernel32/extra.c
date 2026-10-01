/*
 * extra.c — the rest of kernel32: completion ports and overlapped I/O,
 * file mapping, waitable timers, WaitOnAddress, processes, file
 * information, national language support, console, time zones,
 * interlocked lists and error messages.
 *
 * NovaOS emulates a few kernel objects here, in the process:
 *   - I/O is synchronous.  An OVERLAPPED request completes before ReadFile/
 *     WriteFile return: its status is stored, its event set and (unless
 *     the handle skips it) a packet queued on the bound completion port.
 *   - A completion port is a semaphore (the handle) plus a packet queue.
 *   - A file mapping is a kernel section (shared by name between
 *     processes); a file-backed one is a copy of the file the kernel
 *     writes back when a view is unmapped or flushed and when the last
 *     handle closes.
 *   - A waitable timer is an event that a helper thread sets when due.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);

static void *zalloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  zfree(void *p)   { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p); }

static SRWLOCK g_lock;                          /* the tables below */
static void lock(void)   { AcquireSRWLockExclusive(&g_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&g_lock); }

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* Copy a UTF-16 string out Win32-style: returns chars written (no NUL), or
 * the size needed including the NUL if @cap is too small. */
static DWORD put_w(const WCHAR *s, int n, LPWSTR out, DWORD cap)
{
    if (!out || cap <= (DWORD)n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return (DWORD)n + 1; }
    memcpy(out, s, 2 * (SIZE_T)n);
    out[n] = 0;
    return (DWORD)n;
}

static DWORD put_a(const char *s, LPSTR out, DWORD cap)
{
    DWORD n = (DWORD)strlen(s);
    if (!out || cap <= n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return n + 1; }
    memcpy(out, s, n + 1);
    return n;
}

static DWORD put_utf8_as_w(const char *s, LPWSTR out, DWORD cap)
{
    WCHAR tmp[MAX_PATH * 2];
    int n = u2w(s, -1, tmp, MAX_PATH * 2 - 1);
    if (n < 0) { SetLastError(ERROR_INVALID_NAME); return 0; }
    return put_w(tmp, n, out, cap);
}

/* -----------------------------------------------------------------------
 * Completion ports and overlapped I/O
 * ----------------------------------------------------------------------- */
typedef struct Packet { struct Packet *next; DWORD bytes; ULONG_PTR key; LPOVERLAPPED ov; NTSTATUS status; } Packet;
typedef struct Port { struct Port *next; HANDLE h; Packet *head, *tail; } Port;
typedef struct FileInfo { struct FileInfo *next; HANDLE h; Port *port; ULONG_PTR key; UCHAR modes; } FileInfo;

static Port     *g_ports;
static FileInfo *g_files;

static Port *find_port(HANDLE h)
{
    for (Port *p = g_ports; p; p = p->next) if (p->h == h) return p;
    return 0;
}

static FileInfo *file_info(HANDLE h, BOOL create)
{
    for (FileInfo *f = g_files; f; f = f->next) if (f->h == h) return f;
    if (!create) return 0;
    FileInfo *f = zalloc(sizeof(*f));
    if (f) { f->h = h; f->next = g_files; g_files = f; }
    return f;
}

static BOOL post(Port *p, DWORD bytes, ULONG_PTR key, LPOVERLAPPED ov, NTSTATUS status)
{
    Packet *k = zalloc(sizeof(*k));
    if (!k) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    k->bytes = bytes; k->key = key; k->ov = ov; k->status = status;
    if (p->tail) p->tail->next = k; else p->head = k;
    p->tail = k;
    return TRUE;
}

WINBASEAPI HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads)
{
    (void)threads;
    Port *p = 0;
    lock();
    if (existing) {
        p = find_port(existing);
        if (!p) { unlock(); SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    } else {
        HANDLE sem = CreateSemaphoreW(0, 0, 0x7FFFFFFF, 0);
        p = sem ? zalloc(sizeof(*p)) : 0;
        if (!p) { unlock(); if (sem) CloseHandle(sem); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        p->h = sem;
        p->next = g_ports;
        g_ports = p;
    }
    if (file && file != INVALID_HANDLE_VALUE) {
        FileInfo *f = file_info(file, TRUE);
        if (f) { f->port = p; f->key = key; }
    }
    unlock();
    return p->h;
}

WINBASEAPI BOOL WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED ov)
{
    lock();
    Port *p = find_port(port);
    BOOL ok = p && post(p, bytes, key, ov, 0);
    unlock();
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (ok) ReleaseSemaphore(port, 1, 0);
    return ok;
}

static Packet *take(HANDLE port)
{
    lock();
    Port *p = find_port(port);
    Packet *k = p ? p->head : 0;
    if (k) { p->head = k->next; if (!p->head) p->tail = 0; }
    unlock();
    return k;
}

WINBASEAPI BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED *ov, DWORD ms)
{
    *ov = 0;
    DWORD w = WaitForSingleObject(port, ms);
    if (w == WAIT_TIMEOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    if (w != WAIT_OBJECT_0) { SetLastError(ERROR_ABANDONED_WAIT_0); return FALSE; }
    Packet *k = take(port);
    if (!k) { SetLastError(ERROR_ABANDONED_WAIT_0); return FALSE; }
    *bytes = k->bytes; *key = k->key; *ov = k->ov;
    NTSTATUS s = k->status;
    zfree(k);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE port, LPOVERLAPPED_ENTRY e, ULONG n, PULONG got, DWORD ms, BOOL alertable)
{
    (void)alertable;
    *got = 0;
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD w = WaitForSingleObject(port, ms);
    if (w == WAIT_TIMEOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    if (w != WAIT_OBJECT_0) { SetLastError(ERROR_ABANDONED_WAIT_0); return FALSE; }
    ULONG i = 0;
    for (;;) {
        Packet *k = take(port);
        if (!k) break;
        e[i].lpCompletionKey = k->key;
        e[i].lpOverlapped = k->ov;
        e[i].Internal = (ULONG_PTR)k->status;
        e[i].dwNumberOfBytesTransferred = k->bytes;
        zfree(k);
        if (++i == n || WaitForSingleObject(port, 0) != WAIT_OBJECT_0) break;   /* more without waiting */
    }
    *got = i;
    return TRUE;
}

/* Called by ReadFile/WriteFile once an OVERLAPPED request has completed */
void k32_io_done(HANDLE h, OVERLAPPED *o, NTSTATUS s, DWORD bytes)
{
    o->Internal = (ULONG_PTR)s;
    o->InternalHigh = bytes;
    ULONG_PTR ev = (ULONG_PTR)o->hEvent;
    if (ev & ~(ULONG_PTR)1) SetEvent((HANDLE)(ev & ~(ULONG_PTR)1));
    if (ev & 1) return;                                     /* low bit: no completion packet */
    lock();
    FileInfo *f = file_info(h, FALSE);
    Port *p = f ? f->port : 0;
    BOOL queued = p && !(NT_SUCCESS(s) && (f->modes & 1)) && post(p, bytes, f->key, o, s);
    unlock();
    if (queued) ReleaseSemaphore(p->h, 1, 0);
}

/* For ws2_32: finishes an overlapped operation on @h the way file I/O does
 * (the event, then a completion packet if @h is bound to a port). */
__declspec(dllexport) void WINAPI NovaIoComplete(HANDLE h, OVERLAPPED *o, LONG status, DWORD bytes)
{
    k32_io_done(h, o, status, bytes);
}

WINBASEAPI BOOL WINAPI SetFileCompletionNotificationModes(HANDLE h, UCHAR flags)
{
    lock();
    FileInfo *f = file_info(h, TRUE);
    if (f) f->modes = flags;
    unlock();
    return f != 0;
}

/* Completion routines (ReadFileEx, WriteFileEx, timer APCs) run on the
 * issuing thread when it waits alertably (SleepEx) */
typedef struct Apc {
    struct Apc *next;
    DWORD tid;
    int timer;
    void *fn;
    ULONG_PTR a, b, c;
} Apc;
static Apc *g_apcs;
static BOOL run_apcs(void);

static void queue_apc(DWORD tid, int timer, void *fn, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c)
{
    Apc *x = zalloc(sizeof(*x));
    if (!x) return;
    x->tid = tid; x->timer = timer; x->fn = fn; x->a = a; x->b = b; x->c = c;
    lock();
    Apc **pp = &g_apcs;
    while (*pp) pp = &(*pp)->next;
    *pp = x;
    unlock();
    RtlNovaSetApcRunner(run_apcs);                   /* (ntdll's NtTestAlert runs them too) */
}

void k32_queue_user_apc(DWORD tid, PAPCFUNC fn, ULONG_PTR arg) { queue_apc(tid, 2, (void *)fn, arg, 0, 0); }

BOOL k32_run_apcs(void) { return run_apcs(); }

static BOOL run_apcs(void)
{
    DWORD me = GetCurrentThreadId();
    BOOL ran = FALSE;
    for (;;) {
        lock();
        Apc **pp = &g_apcs, *x = 0;
        while (*pp && (*pp)->tid != me) pp = &(*pp)->next;
        if (*pp) { x = *pp; *pp = x->next; }
        unlock();
        if (!x) return ran;
        if (x->timer == 2) ((PAPCFUNC)x->fn)(x->a);            /* QueueUserAPC */
        else if (x->timer) ((VOID (WINAPI *)(LPVOID, DWORD, DWORD))x->fn)((LPVOID)x->a, (DWORD)x->b, (DWORD)x->c);
        else ((LPOVERLAPPED_COMPLETION_ROUTINE)x->fn)((DWORD)x->a, (DWORD)x->b, (LPOVERLAPPED)x->c);
        zfree(x);
        ran = TRUE;
    }
}

/* -----------------------------------------------------------------------
 * Overlapped requests the kernel leaves pending (pipes)
 *
 * The kernel finishes them into the OVERLAPPED (its Internal and
 * InternalHigh are the I/O status block) and sets the event it was given.
 * When the handle is bound to a completion port, or a completion routine
 * waits (ReadFileEx), a helper thread watches a private event instead and
 * then does what k32_io_done does at once for other requests.
 * ----------------------------------------------------------------------- */
typedef struct Watch { struct Watch *next; HANDLE ev, h; OVERLAPPED *o; void *fn; DWORD tid; } Watch;
static Watch *g_watch;
static HANDLE g_watch_wake;

static DWORD apc_error(NTSTATUS s)
{
    return NT_SUCCESS(s) ? 0 : s == STATUS_END_OF_FILE ? ERROR_HANDLE_EOF : RtlNtStatusToDosError(s);
}

static void watch_done(Watch *w)
{
    NTSTATUS s = (NTSTATUS)w->o->Internal;
    DWORD bytes = (DWORD)w->o->InternalHigh;
    if (w->fn) queue_apc(w->tid, 0, w->fn, apc_error(s), bytes, (ULONG_PTR)w->o);
    else k32_io_done(w->h, w->o, s, bytes);
    CloseHandle(w->ev);
    zfree(w);
}

static DWORD WINAPI watcher(LPVOID arg)
{
    (void)arg;
    for (;;) {
        HANDLE hs[MAXIMUM_WAIT_OBJECTS];
        Watch *ws[MAXIMUM_WAIT_OBJECTS];
        DWORD n = 0;
        hs[n++] = g_watch_wake;
        lock();
        for (Watch *w = g_watch; w && n < MAXIMUM_WAIT_OBJECTS; w = w->next) { ws[n] = w; hs[n++] = w->ev; }
        unlock();
        DWORD r = WaitForMultipleObjects(n, hs, FALSE, n == 1 ? INFINITE : 100);
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT || r >= WAIT_OBJECT_0 + n) continue;
        Watch *w = ws[r - WAIT_OBJECT_0];
        lock();
        for (Watch **pp = &g_watch; *pp; pp = &(*pp)->next) if (*pp == w) { *pp = w->next; break; }
        unlock();
        watch_done(w);
    }
}

/* A private event for a request someone must finish in user mode, or 0 */
static Watch *watch_new(HANDLE h, OVERLAPPED *o, void *fn)
{
    lock();
    FileInfo *f = file_info(h, FALSE);
    BOOL port = f && f->port && !((ULONG_PTR)o->hEvent & 1);
    unlock();
    if (!port && !fn) return 0;
    Watch *w = zalloc(sizeof(*w));
    if (!w) return 0;
    w->ev = CreateEventW(0, TRUE, FALSE, 0);
    if (!w->ev) { zfree(w); return 0; }
    w->h = h; w->o = o; w->fn = fn; w->tid = GetCurrentThreadId();
    return w;
}

static void watch_start(Watch *w)
{
    static LONG started;
    if (!InterlockedCompareExchange(&started, 1, 0)) {     /* (not under the lock: CreateThread takes it) */
        HANDLE ev = CreateEventW(0, FALSE, FALSE, 0);
        lock();
        g_watch_wake = ev;
        unlock();
        HANDLE t = CreateThread(0, 64 * 1024, watcher, 0, 0, 0);
        if (t) CloseHandle(t);
    }
    while (!*(HANDLE volatile *)&g_watch_wake) Sleep(0);
    lock();
    w->next = g_watch;
    g_watch = w;
    unlock();
    SetEvent(g_watch_wake);
}

/* Finish a request that did not stay pending */
static void finished_now(HANDLE h, OVERLAPPED *o, Watch *w, void *fn, NTSTATUS s)
{
    if (w) { CloseHandle(w->ev); zfree(w); }
    if (fn) queue_apc(GetCurrentThreadId(), 0, fn, apc_error(s), (DWORD)o->InternalHigh, (ULONG_PTR)o);
    else k32_io_done(h, o, s, (DWORD)o->InternalHigh);
}

BOOL k32_overlapped(HANDLE h, OVERLAPPED *o, int op, PVOID buf, DWORD n, LPDWORD done, PVOID fn)
{
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)o->Offset | (LONGLONG)o->OffsetHigh << 32;
    Watch *w = watch_new(h, o, fn);
    HANDLE ev = w ? w->ev : (HANDLE)((ULONG_PTR)o->hEvent & ~(ULONG_PTR)1);
    o->Internal = STATUS_PENDING;
    o->InternalHigh = 0;
    NTSTATUS s = op ? NtWriteFile(h, ev, 0, 0, (PIO_STATUS_BLOCK)o, buf, n, &off, 0)
                    : NtReadFile(h, ev, 0, 0, (PIO_STATUS_BLOCK)o, buf, n, &off, 0);
    if (done) *done = 0;
    if (s == STATUS_PENDING) {
        if (w) watch_start(w);
        if (fn) { SetLastError(0); return TRUE; }          /* ReadFileEx: queued */
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }
    o->Internal = (ULONG_PTR)s;
    if (!NT_SUCCESS(s) && s != STATUS_END_OF_FILE && s != STATUS_BUFFER_OVERFLOW) o->InternalHigh = 0;
    finished_now(h, o, w, fn, s);
    if (fn) { SetLastError(0); return NT_SUCCESS(s) || s == STATUS_END_OF_FILE || s == STATUS_BUFFER_OVERFLOW; }
    if (done) *done = (DWORD)o->InternalHigh;
    if (s == STATUS_END_OF_FILE) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

NTSTATUS k32_overlapped_fsctl(HANDLE h, OVERLAPPED *o, ULONG code, PVOID in, DWORD in_len, PVOID out, DWORD out_len)
{
    Watch *w = watch_new(h, o, 0);
    HANDLE ev = w ? w->ev : (HANDLE)((ULONG_PTR)o->hEvent & ~(ULONG_PTR)1);
    o->Internal = STATUS_PENDING;
    o->InternalHigh = 0;
    NTSTATUS s = NtFsControlFile(h, ev, 0, 0, (PIO_STATUS_BLOCK)o, code, in, in_len, out, out_len);
    if (s == STATUS_PENDING) { if (w) watch_start(w); return s; }
    o->Internal = (ULONG_PTR)s;
    /* ConnectNamedPipe finding its client already there completes nothing */
    if (s == (NTSTATUS)0xC00000B2) { if (w) { CloseHandle(w->ev); zfree(w); } return s; }
    finished_now(h, o, w, 0, s);
    return s;
}

WINBASEAPI BOOL WINAPI GetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, DWORD ms, BOOL alertable)
{
    (void)h;
    if ((NTSTATUS)ov->Internal == STATUS_PENDING) {
        if (!ms) { SetLastError(996 /* ERROR_IO_INCOMPLETE */); return FALSE; }
        HANDLE ev = (HANDLE)((ULONG_PTR)ov->hEvent & ~(ULONG_PTR)1);
        ULONGLONG until = ms == INFINITE ? ~0ULL : GetTickCount64() + ms;
        while ((NTSTATUS)*(volatile ULONG_PTR *)&ov->Internal == STATUS_PENDING) {
            ULONGLONG now = GetTickCount64();
            if (now >= until) { SetLastError(WAIT_TIMEOUT); return FALSE; }
            DWORD step = ev ? (until - now > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)(until - now)) : 1;
            DWORD r = ev ? WaitForSingleObjectEx(ev, step, alertable) : SleepEx(1, alertable);
            if (r == WAIT_IO_COMPLETION) { SetLastError(WAIT_IO_COMPLETION); return FALSE; }
            if (ev && r == WAIT_OBJECT_0 && (NTSTATUS)ov->Internal == STATUS_PENDING) Sleep(1);
        }
    }
    *bytes = (DWORD)ov->InternalHigh;
    NTSTATUS s = (NTSTATUS)ov->Internal;
    if (s == STATUS_END_OF_FILE) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD bytes, BOOL wait)
{
    return GetOverlappedResultEx(h, ov, bytes, wait ? INFINITE : 0, FALSE);
}

WINBASEAPI BOOL WINAPI CancelIo(HANDLE h)
{
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtCancelIoFile(h, &io);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI CancelIoEx(HANDLE h, LPOVERLAPPED ov)
{
    IO_STATUS_BLOCK io;
    NTSTATUS s = ov ? NtCancelIoFileEx(h, (PIO_STATUS_BLOCK)ov, &io) : NtCancelIoFile(h, &io);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}


WINBASEAPI BOOL WINAPI ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE fn)
{ return k32_overlapped(h, ov, 0, buf, n, 0, (PVOID)fn); }
WINBASEAPI BOOL WINAPI WriteFileEx(HANDLE h, LPCVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE fn)
{ return k32_overlapped(h, ov, 1, (PVOID)buf, n, 0, (PVOID)fn); }

WINBASEAPI DWORD WINAPI SleepEx(DWORD ms, BOOL alertable)
{
    if (!alertable) { Sleep(ms); return 0; }
    ULONGLONG until = ms == INFINITE ? ~0ULL : GetTickCount64() + ms;
    for (;;) {
        if (run_apcs()) return WAIT_IO_COMPLETION;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return 0;
        ULONGLONG left = until - now;
        Sleep(left > 10 ? 10 : (DWORD)left);
    }
}

/* -----------------------------------------------------------------------
 * File mapping: kernel sections. A memory-backed mapping is shared by
 * name between processes; a file-backed one holds a copy of the file that
 * goes back to it on flush, unmap and when the last handle closes.
 * ----------------------------------------------------------------------- */
typedef struct { UNICODE_STRING us; OBJECT_ATTRIBUTES oa; WCHAR buf[260]; } SecName;

static POBJECT_ATTRIBUTES sec_name(SecName *n, LPCWSTR name)
{
    if (!name || !name[0]) return 0;
    int k = 0;
    for (; name[k] && k < 259; k++) n->buf[k] = name[k];
    n->buf[k] = 0;
    RtlInitUnicodeString(&n->us, n->buf);
    memset(&n->oa, 0, sizeof(n->oa));
    n->oa.Length = sizeof(n->oa);
    n->oa.ObjectName = &n->us;
    return &n->oa;
}

/* @oa with OBJ_INHERIT when @inherit (an unnamed section gets one) */
static POBJECT_ATTRIBUTES sec_inherit(SecName *n, POBJECT_ATTRIBUTES oa, BOOL inherit)
{
    if (!inherit) return oa;
    if (!oa) {
        memset(&n->oa, 0, sizeof(n->oa));
        n->oa.Length = sizeof(n->oa);
        oa = &n->oa;
    }
    oa->Attributes |= OBJ_INHERIT;
    return oa;
}

static BOOL writable(DWORD protect) { return (protect & 0xFF) == PAGE_READWRITE || (protect & 0xFF) == PAGE_EXECUTE_READWRITE; }

WINBASEAPI HANDLE WINAPI CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
{
    ULONGLONG size = (ULONGLONG)hi << 32 | lo;
    if (file == INVALID_HANDLE_VALUE) file = 0;
    if (file) {
        LARGE_INTEGER fs;
        if (!GetFileSizeEx(file, &fs)) return 0;
        if (!size) size = (ULONGLONG)fs.QuadPart;
        if (!size) { SetLastError(1006 /* ERROR_FILE_INVALID */); return 0; }
        if (size > (ULONGLONG)fs.QuadPart) {                /* a larger mapping extends the file */
            if (!writable(protect)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
            FILE_END_OF_FILE_INFO e;
            e.EndOfFile.QuadPart = (LONGLONG)size;
            if (!SetFileInformationByHandle(file, FileEndOfFileInfo, &e, sizeof(e))) return 0;
        }
    } else if (!size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    SecName n;
    LARGE_INTEGER max;
    max.QuadPart = (LONGLONG)size;
    HANDLE h = 0;
    NTSTATUS s = NtCreateSection(&h, 0xF001F /* SECTION_ALL_ACCESS */, sec_inherit(&n, sec_name(&n, name), sa && sa->bInheritHandle), &max, protect & 0xFF,
                                 0x8000000 /* SEC_COMMIT */, file);
    if (!NT_SUCCESS(s)) return fail_status(s), (HANDLE)0;
    SetLastError(s == 0x40000000 /* STATUS_OBJECT_NAME_EXISTS */ ? ERROR_ALREADY_EXISTS : 0);
    return h;
}

WINBASEAPI HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name)
{
    WCHAR w[260];
    if (name) { MultiByteToWideChar(CP_ACP, 0, name, -1, w, 260); w[259] = 0; }
    return CreateFileMappingW(file, sa, protect, hi, lo, name ? w : 0);
}

WINBASEAPI HANDLE WINAPI OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name)
{
    (void)access;
    SecName n;
    POBJECT_ATTRIBUTES oa = sec_name(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = sec_inherit(&n, oa, inherit);
    HANDLE h = 0;
    NTSTATUS s = NtOpenSection(&h, 0xF001F, oa);
    if (!NT_SUCCESS(s)) {
        if (s == (NTSTATUS)0xC0000034) SetLastError(ERROR_FILE_NOT_FOUND);
        else fail_status(s);
        return 0;
    }
    return h;
}

WINBASEAPI HANDLE WINAPI OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name)
{
    WCHAR w[260];
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, 260);
    w[259] = 0;
    return OpenFileMappingW(access, inherit, w);
}

WINBASEAPI LPVOID WINAPI MapViewOfFileEx(HANDLE map, DWORD access, DWORD hi, DWORD lo, SIZE_T n, LPVOID base)
{
    PVOID at = base;
    SIZE_T view = n;
    LARGE_INTEGER o;
    o.QuadPart = (LONGLONG)((ULONGLONG)hi << 32 | lo);
    ULONG prot = (access & (FILE_MAP_WRITE | FILE_MAP_COPY)) || access == FILE_MAP_ALL_ACCESS ? PAGE_READWRITE : PAGE_READONLY;
    NTSTATUS s = NtMapViewOfSection(map, (HANDLE)(LONG_PTR)-1, &at, 0, 0, &o, &view, 1 /* ViewShare */, 0, prot);
    if (!NT_SUCCESS(s)) {
        if (s == (NTSTATUS)0xC0000008 || s == (NTSTATUS)0xC0000024) SetLastError(ERROR_INVALID_HANDLE);
        else if (s == (NTSTATUS)0xC0000018) SetLastError(ERROR_INVALID_ADDRESS);
        else fail_status(s);
        return 0;
    }
    return at;
}

WINBASEAPI LPVOID WINAPI MapViewOfFile(HANDLE map, DWORD access, DWORD hi, DWORD lo, SIZE_T n)
{
    return MapViewOfFileEx(map, access, hi, lo, n, 0);
}

WINBASEAPI BOOL WINAPI FlushViewOfFile(LPCVOID p, SIZE_T n)
{
    (void)n;
    NtNovaFlushView((PVOID)p);
    return TRUE;
}

WINBASEAPI BOOL WINAPI UnmapViewOfFile(LPCVOID p)
{
    NTSTATUS s = NtUnmapViewOfSection((HANDLE)(LONG_PTR)-1, (PVOID)p);
    if (!NT_SUCCESS(s)) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Waitable timers
 * ----------------------------------------------------------------------- */
typedef struct Timer {
    struct Timer *next;
    HANDLE h;                       /* the event the waiters see */
    BOOL armed;
    ULONGLONG due;                  /* GetTickCount64 time */
    LONG period;
    void *fn; LPVOID arg; DWORD tid;
} Timer;
static Timer *g_timers;
static HANDLE g_timer_wake, g_timer_thread;

static DWORD WINAPI timer_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        ULONGLONG now = GetTickCount64(), next = ~0ULL;
        lock();
        for (Timer *t = g_timers; t; t = t->next) {
            if (!t->armed) continue;
            if (t->due <= now) {
                SetEvent(t->h);
                if (t->fn) {
                    FILETIME ft;
                    GetSystemTimeAsFileTime(&ft);
                    queue_apc(t->tid, 1, t->fn, (ULONG_PTR)t->arg, ft.dwLowDateTime, ft.dwHighDateTime);
                }
                if (t->period > 0) {
                    t->due += (ULONGLONG)t->period;
                    if (t->due <= now) t->due = now + (ULONGLONG)t->period;
                } else {
                    t->armed = FALSE;
                    continue;
                }
            }
            if (t->due < next) next = t->due;
        }
        unlock();
        DWORD wait = next == ~0ULL ? INFINITE : (DWORD)(next - now > 0x7FFFFFFF ? 0x7FFFFFFF : next - now);
        WaitForSingleObject(g_timer_wake, wait);
    }
}

WINBASEAPI HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{
    (void)access; (void)name;
    Timer *t = zalloc(sizeof(*t));
    HANDLE h = t ? CreateEventW(sa, flags & 1 /* MANUAL_RESET */, FALSE, 0) : 0;
    if (!h) { zfree(t); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    t->h = h;
    lock();
    t->next = g_timers;
    g_timers = t;
    unlock();
    return h;
}

WINBASEAPI HANDLE WINAPI CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name)
{
    return CreateWaitableTimerExW(sa, name, manual ? 1 : 0, 0);
}

WINBASEAPI HANDLE WINAPI CreateWaitableTimerA(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCSTR name)
{
    (void)name;
    return CreateWaitableTimerExW(sa, 0, manual ? 1 : 0, 0);
}

WINBASEAPI BOOL WINAPI SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, LPVOID fn, LPVOID arg, BOOL resume)
{
    (void)resume;
    ULONGLONG now = GetTickCount64(), at;
    if (due->QuadPart < 0) {
        at = now + (ULONGLONG)(-due->QuadPart + 9999) / 10000;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG t = (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
        at = now + ((ULONGLONG)due->QuadPart > t ? ((ULONGLONG)due->QuadPart - t + 9999) / 10000 : 0);
    }
    lock();
    Timer *t = g_timers;
    while (t && t->h != h) t = t->next;
    if (t) {
        ResetEvent(h);
        t->due = at; t->period = period; t->armed = TRUE;
        t->fn = fn; t->arg = arg; t->tid = GetCurrentThreadId();
        if (!g_timer_wake) g_timer_wake = CreateEventW(0, FALSE, FALSE, 0);
        if (g_timer_wake && !g_timer_thread) g_timer_thread = CreateThread(0, 64 * 1024, timer_thread, 0, 0, 0);
    }
    unlock();
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (g_timer_wake) SetEvent(g_timer_wake);
    return TRUE;
}

WINBASEAPI BOOL WINAPI CancelWaitableTimer(HANDLE h)
{
    lock();
    Timer *t = g_timers;
    while (t && t->h != h) t = t->next;
    if (t) t->armed = FALSE;
    unlock();
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

/* The emulated objects above go away with their handle */
void k32_forget_handle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return;
    Port *port = 0;
    Timer *timer = 0;
    lock();
    for (FileInfo **pp = &g_files; *pp; pp = &(*pp)->next)
        if ((*pp)->h == h) { FileInfo *f = *pp; *pp = f->next; zfree(f); break; }
    for (Port **pp = &g_ports; *pp; pp = &(*pp)->next)
        if ((*pp)->h == h) {
            port = *pp;
            *pp = port->next;
            for (FileInfo *f = g_files; f; f = f->next) if (f->port == port) f->port = 0;
            break;
        }
    for (Timer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if ((*pp)->h == h) { timer = *pp; *pp = timer->next; break; }
    unlock();
    if (port) {
        for (Packet *k = port->head, *n; k; k = n) { n = k->next; zfree(k); }
        zfree(port);
    }
    zfree(timer);
}

/* -----------------------------------------------------------------------
 * WaitOnAddress: hashed wait queues (a lock and a condition variable)
 * ----------------------------------------------------------------------- */
typedef struct { SRWLOCK l; CONDITION_VARIABLE cv; } AddrQueue;
static AddrQueue g_addr[64];

static AddrQueue *addr_queue(const volatile void *a) { return &g_addr[((ULONG_PTR)a >> 3) % 64]; }

static BOOL same(const volatile void *a, const void *b, SIZE_T n)
{
    switch (n) {
    case 1: return *(const volatile BYTE *)a == *(const BYTE *)b;
    case 2: return *(const volatile WORD *)a == *(const WORD *)b;
    case 4: return *(const volatile DWORD *)a == *(const DWORD *)b;
    case 8: return *(const volatile ULONGLONG *)a == *(const ULONGLONG *)b;
    }
    return FALSE;
}

WINBASEAPI BOOL WINAPI WaitOnAddress(volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    AddrQueue *q = addr_queue(addr);
    AcquireSRWLockExclusive(&q->l);
    BOOL ok = TRUE;
    if (same(addr, cmp, size)) ok = SleepConditionVariableSRW(&q->cv, &q->l, ms, 0);
    ReleaseSRWLockExclusive(&q->l);
    if (!ok) SetLastError(1460 /* ERROR_TIMEOUT */);
    return ok;
}

WINBASEAPI VOID WINAPI WakeByAddressAll(PVOID addr)
{
    AddrQueue *q = addr_queue(addr);
    AcquireSRWLockExclusive(&q->l);
    WakeAllConditionVariable(&q->cv);
    ReleaseSRWLockExclusive(&q->l);
}

/* waiters on the same bucket re-check their own value, so waking all is safe */
WINBASEAPI VOID WINAPI WakeByAddressSingle(PVOID addr) { WakeByAddressAll(addr); }

/* -----------------------------------------------------------------------
 * Interlocked singly linked lists (a spin lock keeps them simple)
 * ----------------------------------------------------------------------- */
static volatile LONG g_slist_lock;
static void sl_lock(void)   { while (__atomic_exchange_n(&g_slist_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void sl_unlock(void) { __atomic_store_n(&g_slist_lock, 0, __ATOMIC_RELEASE); }

/* The list's first entry, and its depth and sequence (x64: Alignment and
 * the low and high bits of Region; x86: one 8-byte header) */
#ifdef _WIN64
#define SL_FIRST(h)      ((PSLIST_ENTRY)(h)->Alignment)
#define SL_SET_FIRST(h, e) ((h)->Alignment = (ULONGLONG)(e))
#define SL_DEPTH(h)      ((USHORT)((h)->Region & 0xFFFF))
#define SL_BUMP(h, d)    ((h)->Region = (((h)->Region + (d)) & 0xFFFF) | (((h)->Region + 0x10000) & ~0xFFFFULL))
#else
#define SL_FIRST(h)      ((h)->Next.Next)
#define SL_SET_FIRST(h, e) ((h)->Next.Next = (e))
#define SL_DEPTH(h)      ((h)->Depth)
#define SL_BUMP(h, d)    ((h)->Depth += (d), (h)->Sequence++)
#endif

WINBASEAPI VOID WINAPI InitializeSListHead(PSLIST_HEADER h) { memset(h, 0, sizeof(*h)); }

WINBASEAPI PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER h, PSLIST_ENTRY e)
{
    sl_lock();
    PSLIST_ENTRY old = SL_FIRST(h);
    e->Next = old;
    SL_SET_FIRST(h, e);
    SL_BUMP(h, 1);
    sl_unlock();
    return old;
}

WINBASEAPI PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER h)
{
    sl_lock();
    PSLIST_ENTRY e = SL_FIRST(h);
    if (e) {
        SL_SET_FIRST(h, e->Next);
        SL_BUMP(h, -1);
    }
    sl_unlock();
    return e;
}

WINBASEAPI PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER h)
{
    sl_lock();
    PSLIST_ENTRY e = SL_FIRST(h);
    SL_SET_FIRST(h, NULL);
    SL_BUMP(h, -(int)SL_DEPTH(h));
    sl_unlock();
    return e;
}

WINBASEAPI USHORT WINAPI QueryDepthSList(PSLIST_HEADER h) { return SL_DEPTH(h); }

/* -----------------------------------------------------------------------
 * Processes
 * ----------------------------------------------------------------------- */
static BOOL exists_file(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Find a program like CreateProcess does: the name as given (".exe" added
 * when it has no extension), then the program's own directory, the current
 * directory, System32, Windows and PATH. */
static BOOL find_program(const char *name, char *out, int cap)
{
    char tmp[MAX_PATH];
    int n = (int)strlen(name);
    if (n >= MAX_PATH - 5) return FALSE;
    memcpy(tmp, name, (SIZE_T)n + 1);
    const char *base = name;
    for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
    BOOL has_ext = FALSE;
    for (const char *c = base; *c; c++) if (*c == '.') has_ext = TRUE;
    if (!has_ext) memcpy(tmp + n, ".exe", 5);
    BOOL pathy = base != name || (name[0] && name[1] == ':');
    if (pathy) return exists_file(tmp) && full_path(tmp, out, cap);

    char dirs[8][MAX_PATH];
    int nd = 0;
    DWORD k = GetModuleFileNameA(0, dirs[nd], MAX_PATH);
    if (k) { char *s = dirs[nd]; for (char *c = dirs[nd]; *c; c++) if (*c == '\\') s = c; *s = 0; nd++; }
    GetCurrentDirectoryA(MAX_PATH, dirs[nd++]);
    memcpy(dirs[nd++], "C:\\Windows\\System32", 20);
    memcpy(dirs[nd++], "C:\\Windows", 11);
    memcpy(dirs[nd++], "C:\\Programs", 12);
    char path[1024];
    DWORD pl = GetEnvironmentVariableA("PATH", path, sizeof(path));
    char *pp = pl && pl < sizeof(path) ? path : 0;
    for (int i = 0;; i++) {
        char cand[MAX_PATH * 2];
        const char *d;
        char pdir[MAX_PATH];
        if (i < nd) d = dirs[i];
        else {
            if (!pp || !*pp) break;
            int j = 0;
            while (*pp && *pp != ';' && j < MAX_PATH - 1) pdir[j++] = *pp++;
            while (*pp == ';') pp++;
            pdir[j] = 0;
            if (!j) continue;
            d = pdir;
        }
        int dl = (int)strlen(d), tl = (int)strlen(tmp);
        if (dl + tl + 2 > (int)sizeof(cand)) continue;
        memcpy(cand, d, (SIZE_T)dl);
        if (dl && cand[dl - 1] != '\\') cand[dl++] = '\\';
        memcpy(cand + dl, tmp, (SIZE_T)tl + 1);
        if (exists_file(cand)) return full_path(cand, out, cap) != 0;
    }
    return FALSE;
}

/* The environment block for a new process, as UTF-8 (heap) */
static char *env_utf8(LPVOID env, BOOL unicode, SIZE_T *len)
{
    if (!env) return k32_env_block(0, len);
    SIZE_T n = 0;
    if (unicode) {
        const WCHAR *w = env;
        while (w[n] || w[n + 1]) n++;
        n += 2;                                         /* both NULs */
        int need = w2u(w, (int)n, 0, 0);
        char *b = zalloc((SIZE_T)need + 2);
        if (!b) return 0;
        w2u(w, (int)n, b, need + 1);
        *len = (SIZE_T)need;
        return b;
    }
    const char *a = env;
    while (a[n] || a[n + 1]) n++;
    n += 2;
    char *b = zalloc(n);
    if (b) memcpy(b, a, n);
    *len = n;
    return b;
}

static BOOL create_process(const char *app, char *cmd, const char *dir, HANDLE std[3], BOOL inherit,
                           DWORD flags, LPVOID env, const void *rt, WORD rt_len, LPPROCESS_INFORMATION pi)
{
    char name[MAX_PATH], image[MAX_PATH], cwdbuf[MAX_PATH];
    if (app) {
        int n = (int)strlen(app);
        if (n >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
        memcpy(name, app, (SIZE_T)n + 1);
    } else {
        /* the program is the first token of the command line */
        const char *c = cmd;
        while (*c == ' ' || *c == '\t') c++;
        int n = 0;
        if (*c == '"') { c++; while (*c && *c != '"' && n < MAX_PATH - 1) name[n++] = *c++; }
        else while (*c && *c != ' ' && *c != '\t' && n < MAX_PATH - 1) name[n++] = *c++;
        name[n] = 0;
    }
    if (!name[0] || !find_program(name, image, MAX_PATH)) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (dir && !full_path(dir, cwdbuf, MAX_PATH)) { SetLastError(ERROR_DIRECTORY); return FALSE; }
    /* a batch file runs in the command interpreter */
    int il = (int)strlen(image);
    char *batch = 0;
    if (il > 4 && (ieq(image + il - 4, ".bat") || ieq(image + il - 4, ".cmd"))) {
        const char *rest = cmd ? cmd : image;
        SIZE_T bl = strlen(rest) + 64;
        batch = zalloc(bl + (SIZE_T)il);
        if (!batch) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        const char *pre = "C:\\Windows\\System32\\cmd.exe /c ";
        memcpy(batch, pre, strlen(pre));
        memcpy(batch + strlen(pre), rest, strlen(rest) + 1);
        memcpy(image, "C:\\Windows\\System32\\cmd.exe", 28);
        cmd = batch;
    }
    SIZE_T env_len = 0;
    char *envb = env_utf8(env, (flags & CREATE_UNICODE_ENVIRONMENT) != 0, &env_len);
    if (!envb) { zfree(batch); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    NOVA_CREATE_PROCESS io;
    memset(&io, 0, sizeof(io));
    for (int i = 0; i < 3; i++) io.StdHandle[i] = std[i];
    io.Flags = (inherit ? 1 : 0) | ((flags & (DETACHED_PROCESS | CREATE_NO_WINDOW)) ? 2 : 0);
    io.Environment = envb;
    io.EnvironmentSize = env_len;
    if (rt && rt_len) { io.RuntimeData = rt; io.RuntimeDataSize = rt_len; }
    NTSTATUS s = NtNovaCreateProcess(image, cmd ? cmd : image, dir ? cwdbuf : cwd(), &io);
    zfree(envb);
    zfree(batch);
    if (!NT_SUCCESS(s)) return fail_status(s);
    pi->hProcess = io.Process;
    pi->hThread = io.Thread;
    pi->dwProcessId = (DWORD)io.ProcessId;
    pi->dwThreadId = (DWORD)io.ThreadId;
    return TRUE;
}

/* The new process's standard handles: STARTUPINFO's when it says so,
 * else the creator's own (a console program's output goes where ours does) */
static void std_handles(DWORD flags, HANDLE si_in, HANDLE si_out, HANDLE si_err, HANDLE std[3])
{
    if (flags & STARTF_USESTDHANDLES) { std[0] = si_in; std[1] = si_out; std[2] = si_err; return; }
    std[0] = GetStdHandle(STD_INPUT_HANDLE);
    std[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    std[2] = GetStdHandle(STD_ERROR_HANDLE);
    for (int i = 0; i < 3; i++) if (std[i] == INVALID_HANDLE_VALUE) std[i] = 0;
}

WINBASEAPI BOOL WINAPI CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                      DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    (void)pa; (void)ta;
    HANDLE std[3];
    if (!app && !cmd) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    std_handles(si ? si->dwFlags : 0, si ? si->hStdInput : 0, si ? si->hStdOutput : 0, si ? si->hStdError : 0, std);
    return create_process(app, cmd, dir, std, inherit, flags, env,
                          si ? si->lpReserved2 : 0, si ? si->cbReserved2 : 0, pi);
}

WINBASEAPI BOOL WINAPI CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                      DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    (void)pa; (void)ta;
    char a[MAX_PATH * 3], d[MAX_PATH * 3], *c = 0;
    if (!app && !cmd) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (cmd) {
        int n = w2u(cmd, -1, 0, 0);
        c = zalloc((SIZE_T)n + 1);
        if (!c) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        w2u(cmd, -1, c, n);
    }
    HANDLE std[3];
    std_handles(si ? si->dwFlags : 0, si ? si->hStdInput : 0, si ? si->hStdOutput : 0, si ? si->hStdError : 0, std);
    BOOL ok = create_process(app ? wide_to_temp(app, a, sizeof(a)) : 0, c, dir ? wide_to_temp(dir, d, sizeof(d)) : 0,
                             std, inherit, flags, env, si ? si->lpReserved2 : 0, si ? si->cbReserved2 : 0, pi);
    zfree(c);
    return ok;
}

WINBASEAPI HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    (void)access;
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.Attributes = inherit ? OBJ_INHERIT : 0;
    CLIENT_ID cid;
    cid.UniqueProcess = (HANDLE)(ULONG_PTR)pid;
    cid.UniqueThread = 0;
    HANDLE h = 0;
    NTSTATUS s = NtOpenProcess(&h, access, &oa, &cid);
    if (!NT_SUCCESS(s)) {
        if (pid == GetCurrentProcessId()) return GetCurrentProcess();   /* (this process has no exit object) */
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return h;
}

WINBASEAPI DWORD WINAPI GetProcessId(HANDLE h)
{
    ULONG64 info[3];
    if (!NT_SUCCESS(NtNovaProcessInfo(h, info))) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return (DWORD)info[0];
}

WINBASEAPI BOOL WINAPI InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST l, DWORD n, DWORD flags, PSIZE_T size)
{
    (void)flags;
    SIZE_T need = 48 + (SIZE_T)n * 24;
    if (!l || *size < need) { *size = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(l, 0, need);
    return TRUE;
}

WINBASEAPI BOOL WINAPI UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST l, DWORD flags, DWORD_PTR attr, PVOID v, SIZE_T n,
                                                 PVOID prev, PSIZE_T ret)
{
    (void)l; (void)flags; (void)attr; (void)v; (void)n; (void)prev; (void)ret;
    return TRUE;                            /* attributes (handle lists, pseudo consoles) are ignored */
}

WINBASEAPI VOID WINAPI DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST l) { (void)l; }

WINBASEAPI VOID WINAPI GetStartupInfoW(LPSTARTUPINFOW si)
{
    STARTUPINFOA a;
    GetStartupInfoA(&a);
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
    si->hStdInput = a.hStdInput;
    si->hStdOutput = a.hStdOutput;
    si->hStdError = a.hStdError;
    si->cbReserved2 = a.cbReserved2;
    si->lpReserved2 = a.lpReserved2;
}

WINBASEAPI BOOL WINAPI GetProcessAffinityMask(HANDLE p, PDWORD_PTR proc, PDWORD_PTR sys)
{
    (void)p;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD_PTR mask = si.dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : ((DWORD_PTR)1 << si.dwNumberOfProcessors) - 1;
    *proc = mask;
    *sys = mask;
    return TRUE;
}

WINBASEAPI BOOL WINAPI SetProcessAffinityMask(HANDLE p, DWORD_PTR mask) { (void)p; return mask != 0; }
WINBASEAPI BOOL WINAPI SetProcessPriorityBoost(HANDLE p, BOOL off)      { (void)p; (void)off; return TRUE; }
WINBASEAPI int  WINAPI GetThreadPriority(HANDLE t)                     { (void)t; return 0; }
WINBASEAPI BOOL WINAPI SetThreadPriority(HANDLE t, int prio)           { (void)t; (void)prio; return TRUE; }
WINBASEAPI BOOL WINAPI SetThreadStackGuarantee(PULONG size)            { if (size) *size = 0; return TRUE; }
WINBASEAPI DWORD WINAPI GetPriorityClass(HANDLE p)                     { (void)p; return 0x20; /* NORMAL */ }
WINBASEAPI BOOL WINAPI SetPriorityClass(HANDLE p, DWORD c)             { (void)p; (void)c; return TRUE; }

static UINT g_error_mode;
WINBASEAPI UINT WINAPI SetErrorMode(UINT mode) { UINT old = g_error_mode; g_error_mode = mode; return old; }
WINBASEAPI UINT WINAPI GetErrorMode(void)      { return g_error_mode; }
WINBASEAPI BOOL WINAPI SetThreadErrorMode(DWORD mode, LPDWORD old) { if (old) *old = g_error_mode; g_error_mode = mode; return TRUE; }
WINBASEAPI DWORD WINAPI GetThreadErrorMode(void) { return g_error_mode; }

WINBASEAPI BOOL WINAPI IsProcessorFeaturePresent(DWORD f)
{
    unsigned a, b, c, d, b7 = 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    { unsigned x, y, z; __asm__("cpuid" : "=a"(x), "=b"(b7), "=c"(y), "=d"(z) : "a"(7), "c"(0)); }
    switch (f) {
    case 2:  return TRUE;                   /* PF_COMPARE_EXCHANGE_DOUBLE */
    case 3:  return (d >> 23) & 1;          /* PF_MMX_INSTRUCTIONS_AVAILABLE */
    case 6:  return (d >> 25) & 1;          /* PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE) */
    case 8:  return TRUE;                   /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    case 10: return (d >> 26) & 1;          /* PF_XMMI64 (SSE2) */
    case 12: return TRUE;                   /* PF_NX_ENABLED */
    case 13: return c & 1;                  /* PF_SSE3 */
    case 14: return (c >> 13) & 1;          /* PF_COMPARE_EXCHANGE128 */
    case 17: return TRUE;                   /* PF_XSAVE: FXSAVE-based here */
    case 20: return TRUE;                   /* PF_SECOND_LEVEL_ADDRESS_TRANSLATION */
    case 36: return (c >> 9) & 1;           /* PF_SSSE3 */
    case 37: return (c >> 19) & 1;          /* PF_SSE4_1 */
    case 38: return (c >> 20) & 1;          /* PF_SSE4_2 */
    case 39: return 0;                      /* PF_AVX: the kernel saves SSE state only */
    case 40: return 0;                      /* PF_AVX2 */
    case 41: return 0;                      /* PF_AVX512F */
    case 28: return (c >> 30) & 1 ? TRUE : FALSE; /* PF_RDRAND */
    case 29: return 0;                      /* PF_ARM_* and others */
    default: (void)b7; return FALSE;
    }
}

WINBASEAPI PVOID WINAPI EncodePointer(PVOID p)       { return p; }
WINBASEAPI PVOID WINAPI DecodePointer(PVOID p)       { return p; }
WINBASEAPI PVOID WINAPI EncodeSystemPointer(PVOID p) { return p; }
WINBASEAPI PVOID WINAPI DecodeSystemPointer(PVOID p) { return p; }

WINBASEAPI BOOL WINAPI GetThreadContext(HANDLE t, LPCONTEXT c)
{
    if (t == GetCurrentThread() || GetThreadId(t) == GetCurrentThreadId()) {
        DWORD flags = c->ContextFlags;
        RtlCaptureContext(c);
        c->ContextFlags = flags;
        return TRUE;
    }
    NTSTATUS s = NtGetContextThread(t, c);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI SetThreadContext(HANDLE t, const CONTEXT *c)
{
    if (t == GetCurrentThread() || GetThreadId(t) == GetCurrentThreadId()) {
        NtContinue((PCONTEXT)c, FALSE);
        return FALSE;
    }
    NTSTATUS s = NtSetContextThread(t, c);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

/* ntdll's unwinder and friends, under their kernel32 names */
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:RtlCaptureContext=ntdll.RtlCaptureContext\"\n\t"
        ".ascii \" /EXPORT:RtlLookupFunctionEntry=ntdll.RtlLookupFunctionEntry\"\n\t"
        ".ascii \" /EXPORT:RtlVirtualUnwind=ntdll.RtlVirtualUnwind\"\n\t"
        ".ascii \" /EXPORT:RtlUnwindEx=ntdll.RtlUnwindEx\"\n\t"
        ".ascii \" /EXPORT:RtlUnwind=ntdll.RtlUnwind\"\n\t"
        ".ascii \" /EXPORT:RtlPcToFileHeader=ntdll.RtlPcToFileHeader\"\n\t"
        ".ascii \" /EXPORT:RtlRestoreContext=ntdll.RtlRestoreContext\"\n\t"
        ".ascii \" /EXPORT:RtlCaptureStackBackTrace=ntdll.RtlCaptureStackBackTrace\"\n\t"
        ".ascii \" /EXPORT:RtlRaiseException=ntdll.RtlRaiseException\"\n\t"
        ".ascii \" /EXPORT:RtlAddFunctionTable=ntdll.RtlAddFunctionTable\"\n\t"
        ".ascii \" /EXPORT:RtlDeleteFunctionTable=ntdll.RtlDeleteFunctionTable\"\n\t"
        ".ascii \" /EXPORT:RtlInstallFunctionTableCallback=ntdll.RtlInstallFunctionTableCallback\"\n\t"
        ".text\n");
#ifdef _WIN64
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:__C_specific_handler=ntdll.__C_specific_handler\"\n\t"
        ".text\n");
#endif

/* -----------------------------------------------------------------------
 * Files and paths
 * ----------------------------------------------------------------------- */
#define VOLUME_SERIAL 0x4E4F5641u           /* "NOVA" */

static void ft_from(LARGE_INTEGER t, FILETIME *ft) { ft->dwLowDateTime = t.LowPart; ft->dwHighDateTime = (DWORD)t.HighPart; }

WINBASEAPI BOOL WINAPI GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    IO_STATUS_BLOCK io;
    FILE_BASIC_INFORMATION b;
    FILE_STANDARD_INFORMATION s;
    ULONGLONG id = 0;
    NTSTATUS st = NtQueryInformationFile(h, &io, &b, sizeof(b), FileBasicInformation);
    if (NT_SUCCESS(st)) st = NtQueryInformationFile(h, &io, &s, sizeof(s), FileStandardInformation);
    if (!NT_SUCCESS(st)) return fail_status(st);
    NtQueryInformationFile(h, &io, &id, sizeof(id), 6 /* FileInternalInformation */);
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = b.FileAttributes;
    ft_from(b.CreationTime, &info->ftCreationTime);
    ft_from(b.LastAccessTime, &info->ftLastAccessTime);
    ft_from(b.LastWriteTime, &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = VOLUME_SERIAL;
    info->nFileSizeHigh = (DWORD)(s.EndOfFile.QuadPart >> 32);
    info->nFileSizeLow = (DWORD)s.EndOfFile.QuadPart;
    info->nNumberOfLinks = s.NumberOfLinks;
    info->nFileIndexHigh = (DWORD)(id >> 32);
    info->nFileIndexLow = (DWORD)id;
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID buf, DWORD n)
{
    IO_STATUS_BLOCK io;
    NTSTATUS s;
    switch (c) {
    case FileBasicInfo:
        if (n < sizeof(FILE_BASIC_INFO)) break;
        s = NtQueryInformationFile(h, &io, buf, sizeof(FILE_BASIC_INFO), FileBasicInformation);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    case FileStandardInfo:
        if (n < sizeof(FILE_STANDARD_INFO)) break;
        s = NtQueryInformationFile(h, &io, buf, sizeof(FILE_STANDARD_INFO), FileStandardInformation);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    case FileNameInfo:
        if (n < 4) break;
        s = NtQueryInformationFile(h, &io, buf, n, 9 /* FileNameInformation */);
        if (s == (NTSTATUS)0x80000005) { SetLastError(ERROR_MORE_DATA); return FALSE; }
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    case FileAttributeTagInfo:
        if (n < sizeof(FILE_ATTRIBUTE_TAG_INFO)) break;
        s = NtQueryInformationFile(h, &io, buf, sizeof(FILE_ATTRIBUTE_TAG_INFO), 35 /* FileAttributeTagInformation */);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    case FileIdInfo: {
        if (n < sizeof(FILE_ID_INFO)) break;
        FILE_ID_INFO *fi = buf;
        ULONGLONG id = 0;
        s = NtQueryInformationFile(h, &io, &id, sizeof(id), 6);
        if (!NT_SUCCESS(s)) return fail_status(s);
        memset(fi, 0, sizeof(*fi));
        fi->VolumeSerialNumber = VOLUME_SERIAL;
        memcpy(fi->FileId.Identifier, &id, sizeof(id));
        return TRUE;
    }
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(ERROR_BAD_LENGTH);
    return FALSE;
}

/* FILE_RENAME_INFORMATION for the kernel: { BOOLEAN; HANDLE; ULONG; WCHAR[] } */
static NTSTATUS rename_handle(HANDLE h, const char *to, BOOL replace)
{
    NtPath p;
    if (!nt_path(to, &p)) return STATUS_OBJECT_NAME_INVALID;
    typedef struct { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[1]; } RenameInfo;
    BYTE buf[sizeof(RenameInfo) + 2 * (MAX_PATH + 8)];
    RenameInfo *ri = (RenameInfo *)buf;
    memset(buf, 0, sizeof(RenameInfo));
    ri->ReplaceIfExists = (BOOLEAN)(replace ? 1 : 0);
    ULONG len = p.us.Length;
    ri->FileNameLength = len;
    memcpy(ri->FileName, p.buf, len);
    IO_STATUS_BLOCK io;
    return NtSetInformationFile(h, &io, buf, (ULONG)__builtin_offsetof(RenameInfo, FileName) + len,
                                10 /* FileRenameInformation */);
}

WINBASEAPI BOOL WINAPI SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID buf, DWORD n)
{
    IO_STATUS_BLOCK io;
    NTSTATUS s;
    switch (c) {
    case FileBasicInfo:
        return TRUE;                        /* times and attributes are not stored */
    case FileEndOfFileInfo:
    case FileAllocationInfo:
        if (n < 8) break;
        s = NtSetInformationFile(h, &io, buf, 8, c == FileEndOfFileInfo ? FileEndOfFileInformation : FileEndOfFileInformation);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    case FileDispositionInfo:
        if (n < 1) break;
        s = NtSetInformationFile(h, &io, buf, 1, FileDispositionInformation);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    case FileDispositionInfoEx: {
        if (n < 4) break;
        BOOLEAN del = (*(DWORD *)buf & 1) != 0;
        s = NtSetInformationFile(h, &io, &del, 1, FileDispositionInformation);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    }
    case FileRenameInfo:
    case FileRenameInfoEx: {
        if (n < 24) break;
        BYTE *r = buf;
        DWORD flags = c == FileRenameInfoEx ? *(DWORD *)r : r[0];
        DWORD len = *(DWORD *)(r + 16);
        char to[MAX_PATH * 3];
        int k = w2u((WCHAR *)(r + 20), (int)len / 2, to, sizeof(to) - 1);
        if (k < 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
        to[k] = 0;
        s = rename_handle(h, to, (flags & 1) != 0);
        return NT_SUCCESS(s) ? TRUE : fail_status(s);
    }
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(ERROR_BAD_LENGTH);
    return FALSE;
}

WINBASEAPI DWORD WINAPI GetFinalPathNameByHandleW(HANDLE h, LPWSTR buf, DWORD n, DWORD flags)
{
    BYTE info[4 + 2 * (MAX_PATH + 8)];
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtQueryInformationFile(h, &io, info, sizeof(info), 9);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    DWORD len = *(DWORD *)info / 2;
    WCHAR out[MAX_PATH + 40];
    int o = 0;
    const char *pre = (flags & 0xF) == 2 ? "\\Device\\HarddiskVolume1" :     /* VOLUME_NAME_NT */
                      (flags & 0xF) == 4 ? "" : "\\\\?\\C:";                  /* NONE : DOS */
    for (const char *c = pre; *c; c++) out[o++] = (WCHAR)*c;
    for (DWORD i = 0; i < len && o < MAX_PATH + 38; i++) out[o++] = ((WCHAR *)(info + 4))[i];
    if (o == 6 && (flags & 0xF) == 0) out[o++] = '\\';                      /* the root: "\\?\C:\" */
    return put_w(out, o, buf, n);
}

WINBASEAPI DWORD WINAPI GetFinalPathNameByHandleA(HANDLE h, LPSTR buf, DWORD n, DWORD flags)
{
    WCHAR w[MAX_PATH + 40];
    DWORD k = GetFinalPathNameByHandleW(h, w, MAX_PATH + 40, flags);
    if (!k || k >= MAX_PATH + 40) return 0;
    char a[MAX_PATH * 3];
    int m = w2u(w, (int)k, a, sizeof(a) - 1);
    if (m < 0) return 0;
    a[m] = 0;
    return put_a(a, buf, n);
}

WINBASEAPI DWORD WINAPI GetFullPathNameW(LPCWSTR name, DWORD n, LPWSTR buf, LPWSTR *part)
{
    char a[MAX_PATH * 3], full[MAX_PATH];
    if (!wide_to_temp(name, a, sizeof(a)) || !full_path(a, full, MAX_PATH)) { SetLastError(ERROR_INVALID_NAME); return 0; }
    /* keep a trailing separator, as Windows does */
    int al = (int)strlen(a), fl = (int)strlen(full);
    if (al && (a[al - 1] == '\\' || a[al - 1] == '/') && full[fl - 1] != '\\' && fl < MAX_PATH - 1) { full[fl++] = '\\'; full[fl] = 0; }
    DWORD r = put_utf8_as_w(full, buf, n);
    if (part && buf && r < n) {
        LPWSTR last = buf;
        for (LPWSTR c = buf; *c; c++) if (*c == '\\') last = c + 1;
        *part = *last ? last : 0;
    }
    return r;
}

static BOOL attr_data(const char *name, WIN32_FILE_ATTRIBUTE_DATA *d)
{
    memset(d, 0, sizeof(*d));
    DWORD a = GetFileAttributesA(name);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    d->dwFileAttributes = a;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        HANDLE h = CreateFileA(name, 0, 7, 0, OPEN_EXISTING, 0, 0);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION i;
            if (GetFileInformationByHandle(h, &i)) {
                d->ftCreationTime = i.ftCreationTime;
                d->ftLastAccessTime = i.ftLastAccessTime;
                d->ftLastWriteTime = i.ftLastWriteTime;
                d->nFileSizeHigh = i.nFileSizeHigh;
                d->nFileSizeLow = i.nFileSizeLow;
            }
            CloseHandle(h);
        }
    }
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS l, LPVOID info)
{
    (void)l;
    return attr_data(name, info);
}

WINBASEAPI BOOL WINAPI GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS l, LPVOID info)
{
    (void)l;
    char a[MAX_PATH * 3];
    if (!wide_to_temp(name, a, sizeof(a))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return attr_data(a, info);
}

WINBASEAPI BOOL WINAPI SetFileAttributesA(LPCSTR name, DWORD attr)
{
    (void)attr;                             /* attributes are not stored */
    return GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES;
}

WINBASEAPI BOOL WINAPI SetFileAttributesW(LPCWSTR name, DWORD attr)
{
    (void)attr;
    return GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES;
}

WINBASEAPI HANDLE WINAPI FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS l, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
    (void)l; (void)op; (void)filter; (void)flags;
    return FindFirstFileW(name, data);
}

WINBASEAPI HANDLE WINAPI FindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS l, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
    (void)l; (void)op; (void)filter; (void)flags;
    return FindFirstFileA(name, data);
}

WINBASEAPI BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    HANDLE in = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (in == INVALID_HANDLE_VALUE) return FALSE;
    HANDLE out = CreateFileA(to, GENERIC_WRITE, 0, 0, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS, 0, 0);
    if (out == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); CloseHandle(in); SetLastError(e); return FALSE; }
    static BYTE buf[64 * 1024];             /* callers are rarely concurrent; keep stacks small */
    BOOL ok = TRUE;
    lock();
    for (;;) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buf, sizeof(buf), &got, 0)) { ok = FALSE; break; }
        if (!got) break;
        if (!WriteFile(out, buf, got, &put, 0) || put != got) { ok = FALSE; break; }
    }
    unlock();
    DWORD e = GetLastError();
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) { DeleteFileA(to); SetLastError(e); }
    return ok;
}

WINBASEAPI BOOL WINAPI CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    char a[MAX_PATH * 3], b[MAX_PATH * 3];
    if (!wide_to_temp(from, a, sizeof(a)) || !wide_to_temp(to, b, sizeof(b))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return CopyFileA(a, b, fail_if_exists);
}

WINBASEAPI BOOL WINAPI CopyFileExW(LPCWSTR from, LPCWSTR to, LPVOID progress, LPVOID data, LPBOOL cancel, DWORD flags)
{
    (void)progress; (void)data; (void)cancel;
    return CopyFileW(from, to, (flags & 1) != 0);   /* COPY_FILE_FAIL_IF_EXISTS */
}

/* MOVEFILE_DELAY_UNTIL_REBOOT: as Windows, the operation is only written
 * down, in Session Manager's PendingFileRenameOperations (pairs of
 * "\\??\\source", "\\??\\target" or "" for a delete); installers use it
 * for files they cannot remove while they run */
static BOOL pending_file_op(LPCSTR from, LPCSTR to)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, 0, 0,
                        KEY_ALL_ACCESS, 0, &k, 0)) return FALSE;
    DWORD type = 0, n = 0;
    RegQueryValueExW(k, L"PendingFileRenameOperations", 0, &type, 0, &n);
    if (type != REG_MULTI_SZ) n = 0;
    DWORD cap = n + 2 * (2 * MAX_PATH + 16) * sizeof(WCHAR);
    WCHAR *buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap);
    if (!buf) { RegCloseKey(k); return FALSE; }
    if (n) RegQueryValueExW(k, L"PendingFileRenameOperations", 0, &type, (BYTE *)buf, &n);
    DWORD w = n / sizeof(WCHAR);
    if (w && !buf[w - 1]) w--;                       /* drop the list's final terminator */
    const char *items[2] = { from, to };
    for (int i = 0; i < 2; i++) {
        if (items[i] && *items[i]) {
            buf[w++] = '\\'; buf[w++] = '?'; buf[w++] = '?'; buf[w++] = '\\';
            int m = MultiByteToWideChar(CP_UTF8, 0, items[i], -1, buf + w, 2 * MAX_PATH);
            w += m > 0 ? (DWORD)m : 1;
        } else buf[w++] = 0;
    }
    buf[w++] = 0;
    LSTATUS r = RegSetValueExW(k, L"PendingFileRenameOperations", 0, REG_MULTI_SZ, (BYTE *)buf, w * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, buf);
    RegCloseKey(k);
    return r == 0;
}

WINBASEAPI BOOL WINAPI MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    if (flags & 4) return pending_file_op(from, to);       /* MOVEFILE_DELAY_UNTIL_REBOOT */
    if (!to) return DeleteFileA(from);
    HANDLE h = CreateFileA(from, DELETE, 7, 0, OPEN_EXISTING, 0x02000000 /* BACKUP_SEMANTICS */, 0);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    NTSTATUS s = rename_handle(h, to, (flags & 1) != 0);
    CloseHandle(h);
    if (s == (NTSTATUS)0xC00000D4 && (flags & 2)) {  /* another drive: MOVEFILE_COPY_ALLOWED */
        if (!CopyFileA(from, to, !(flags & 1))) return FALSE;
        return DeleteFileA(from);
    }
    if (s == (NTSTATUS)0xC0000035) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    char a[MAX_PATH * 3], b[MAX_PATH * 3];
    if (!wide_to_temp(from, a, sizeof(a))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    if (to && !wide_to_temp(to, b, sizeof(b))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return MoveFileExA(a, to ? b : 0, flags);
}

WINBASEAPI BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to)   { return MoveFileExA(from, to, 2); }
WINBASEAPI BOOL WINAPI MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, 2); }
WINBASEAPI BOOL WINAPI ReplaceFileW(LPCWSTR repl, LPCWSTR with, LPCWSTR backup, DWORD flags, LPVOID a, LPVOID b)
{
    (void)backup; (void)flags; (void)a; (void)b;
    return MoveFileExW(with, repl, 1);
}

WINBASEAPI BOOL WINAPI RemoveDirectoryW(LPCWSTR name)
{
    char a[MAX_PATH * 3];
    return wide_to_temp(name, a, sizeof(a)) ? RemoveDirectoryA(a) : FALSE;
}

WINBASEAPI BOOL WINAPI SetCurrentDirectoryW(LPCWSTR path)
{
    char a[MAX_PATH * 3];
    if (!wide_to_temp(path, a, sizeof(a))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return SetCurrentDirectoryA(a);
}

#ifdef _WIN64
#define SYSTEM_DIR "C:\\Windows\\System32"
#else
#define SYSTEM_DIR "C:\\Windows\\SysWOW64"                  /* 32-bit programs' system folder */
#endif
WINBASEAPI UINT WINAPI GetSystemDirectoryA(LPSTR buf, UINT n)  { return put_a(SYSTEM_DIR, buf, n); }
WINBASEAPI UINT WINAPI GetWindowsDirectoryA(LPSTR buf, UINT n) { return put_a("C:\\Windows", buf, n); }
WINBASEAPI UINT WINAPI GetSystemWindowsDirectoryA(LPSTR buf, UINT n) { return put_a("C:\\Windows", buf, n); }
WINBASEAPI UINT WINAPI GetSystemDirectoryW(LPWSTR buf, UINT n) { return put_utf8_as_w(SYSTEM_DIR, buf, n); }
WINBASEAPI UINT WINAPI GetSystemWow64DirectoryA(LPSTR buf, UINT n)  { return put_a("C:\\Windows\\SysWOW64", buf, n); }
WINBASEAPI UINT WINAPI GetSystemWow64DirectoryW(LPWSTR buf, UINT n) { return put_utf8_as_w("C:\\Windows\\SysWOW64", buf, n); }
WINBASEAPI UINT WINAPI GetWindowsDirectoryW(LPWSTR buf, UINT n){ return put_utf8_as_w("C:\\Windows", buf, n); }
WINBASEAPI UINT WINAPI GetSystemWindowsDirectoryW(LPWSTR buf, UINT n) { return put_utf8_as_w("C:\\Windows", buf, n); }

/* %TMP%, %TEMP%, else C:\Temp (created on demand); always ends in '\' */
static void temp_dir(char *out)
{
    DWORD n = GetEnvironmentVariableA("TMP", out, MAX_PATH - 2);
    if (!n || n >= MAX_PATH - 2) n = GetEnvironmentVariableA("TEMP", out, MAX_PATH - 2);
    if (!n || n >= MAX_PATH - 2) { memcpy(out, "C:\\Temp", 8); n = 7; CreateDirectoryA(out, 0); }
    if (out[n - 1] != '\\') { out[n++] = '\\'; out[n] = 0; }
}

WINBASEAPI DWORD WINAPI GetTempPathA(DWORD n, LPSTR buf) { char t[MAX_PATH]; temp_dir(t); return put_a(t, buf, n); }
WINBASEAPI DWORD WINAPI GetTempPathW(DWORD n, LPWSTR buf) { char t[MAX_PATH]; temp_dir(t); return put_utf8_as_w(t, buf, n); }
WINBASEAPI DWORD WINAPI GetTempPath2W(DWORD n, LPWSTR buf) { return GetTempPathW(n, buf); }

WINBASEAPI UINT WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out)
{
    static const char hex[] = "0123456789ABCDEF";
    int dl = (int)strlen(dir);
    if (dl > MAX_PATH - 15) { SetLastError(ERROR_BUFFER_OVERFLOW); return 0; }
    UINT u = unique ? unique : (GetTickCount() ^ GetCurrentProcessId() << 8) & 0xFFFF;
    for (int tries = 0; tries < 0x10000; tries++, u = (u + 1) & 0xFFFF) {
        if (!u) continue;
        int o = 0;
        memcpy(out, dir, (SIZE_T)dl);
        o = dl;
        if (o && out[o - 1] != '\\') out[o++] = '\\';
        for (int i = 0; i < 3 && prefix && prefix[i]; i++) out[o++] = prefix[i];
        for (int s = 12; s >= 0; s -= 4) out[o++] = hex[(u >> s) & 15];
        memcpy(out + o, ".TMP", 5);
        if (unique) return unique;
        HANDLE h = CreateFileA(out, GENERIC_WRITE, 0, 0, CREATE_NEW, 0, 0);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return u; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    return 0;
}

WINBASEAPI UINT WINAPI GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR out)
{
    char d[MAX_PATH * 3], p[16], o[MAX_PATH];
    if (!wide_to_temp(dir, d, sizeof(d))) { SetLastError(ERROR_INVALID_NAME); return 0; }
    if (!prefix || !wide_to_temp(prefix, p, sizeof(p))) p[0] = 0;
    UINT r = GetTempFileNameA(d, p, unique, o);
    if (r) u2w(o, -1, out, MAX_PATH), out[u2w(o, -1, 0, 0)] = 0;
    return r;
}

WINBASEAPI DWORD WINAPI GetLongPathNameW(LPCWSTR s, LPWSTR l, DWORD n)  { return put_w(s, wlen(s), l, n); }
WINBASEAPI DWORD WINAPI GetShortPathNameW(LPCWSTR s, LPWSTR l, DWORD n) { return put_w(s, wlen(s), l, n); }
WINBASEAPI DWORD WINAPI GetLongPathNameA(LPCSTR s, LPSTR l, DWORD n)    { return put_a(s, l, n); }
WINBASEAPI DWORD WINAPI GetShortPathNameA(LPCSTR s, LPSTR l, DWORD n)   { return put_a(s, l, n); }


WINBASEAPI BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free)
{
    (void)dir;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    ULONGLONG f = GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : 256ULL << 20;   /* drive C: lives in RAM */
    if (avail) avail->QuadPart = f;
    if (free) free->QuadPart = f;
    if (total) total->QuadPart = GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 512ULL << 20;
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free)
{
    (void)dir;
    return GetDiskFreeSpaceExW(0, avail, total, free);
}

WINBASEAPI BOOL WINAPI GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD nn, LPDWORD serial, LPDWORD maxlen, LPDWORD flags,
                                             LPWSTR fs, DWORD nfs)
{
    (void)root;
    if (name && nn) put_utf8_as_w("NovaOS", name, nn);
    if (serial) *serial = VOLUME_SERIAL;
    if (maxlen) *maxlen = 47;
    if (flags) *flags = 0x2 | 0x4;          /* CASE_PRESERVED_NAMES | UNICODE_ON_DISK */
    if (fs && nfs) put_utf8_as_w("RAMFS", fs, nfs);
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD nn, LPDWORD serial, LPDWORD maxlen, LPDWORD flags,
                                             LPSTR fs, DWORD nfs)
{
    (void)root;
    if (name && nn) put_a("NovaOS", name, nn);
    if (fs && nfs) put_a("RAMFS", fs, nfs);
    return GetVolumeInformationW(0, 0, 0, serial, maxlen, flags, 0, 0);
}

WINBASEAPI BOOL WINAPI GetVolumePathNameW(LPCWSTR name, LPWSTR out, DWORD n) { (void)name; return put_utf8_as_w("C:\\", out, n) < n; }

WINBASEAPI UINT WINAPI GetDriveTypeW(LPCWSTR root)
{
    return !root || ((root[0] | 0x20) == 'c' && root[1] == ':') ? 3 /* DRIVE_FIXED */ : 1 /* DRIVE_NO_ROOT_DIR */;
}

WINBASEAPI UINT WINAPI GetDriveTypeA(LPCSTR root)
{
    return !root || ((root[0] | 0x20) == 'c' && root[1] == ':') ? 3 : 1;
}

WINBASEAPI DWORD WINAPI GetLogicalDrives(void) { return 1u << 2; }
WINBASEAPI BOOL  WINAPI AreFileApisANSI(void)   { return TRUE; }
WINBASEAPI VOID  WINAPI SetFileApisToANSI(void) { }
WINBASEAPI VOID  WINAPI SetFileApisToOEM(void)  { }

/* -----------------------------------------------------------------------
 * Modules and memory
 * ----------------------------------------------------------------------- */
WINBASEAPI HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags)
{
    char n[MAX_PATH * 3];
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof(n), 0, 0)) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return 0; }
    return LoadLibraryExA(n, f, flags);
}

WINBASEAPI BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
{
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        PVOID base = 0;
        RtlPcToFileHeader((PVOID)name, &base);
        *out = base;
    } else {
        *out = GetModuleHandleW(name);               /* (PIN, UNCHANGED_REFCOUNT: modules stay loaded) */
    }
    if (!*out) { SetLastError(ERROR_MOD_NOT_FOUND); return FALSE; }
    return TRUE;
}

WINBASEAPI BOOL WINAPI SetDllDirectoryW(LPCWSTR dir) { (void)dir; return TRUE; }
WINBASEAPI BOOL WINAPI SetDllDirectoryA(LPCSTR dir)  { (void)dir; return TRUE; }
WINBASEAPI BOOL WINAPI SetDefaultDllDirectories(DWORD f) { (void)f; return TRUE; }
WINBASEAPI PVOID WINAPI AddDllDirectory(LPCWSTR dir) { (void)dir; return (PVOID)1; }
WINBASEAPI BOOL WINAPI RemoveDllDirectory(PVOID cookie) { (void)cookie; return TRUE; }
WINBASEAPI BOOL WINAPI DisableThreadLibraryCalls(HMODULE m) { (void)m; return TRUE; }

WINBASEAPI SIZE_T WINAPI VirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION mbi, SIZE_T n)
{
    SIZE_T got = 0;
    NTSTATUS s = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)p, 0, mbi, n, &got);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return got;
}

WINBASEAPI BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ms->dwMemoryLoad = 30;
    ms->ullTotalPhys = 512ULL << 20;
    ms->ullAvailPhys = 256ULL << 20;
    ms->ullTotalPageFile = ms->ullTotalPhys;
    ms->ullAvailPageFile = ms->ullAvailPhys;
    ms->ullTotalVirtual = sizeof(void *) == 4 ? 0x7FFE0000ULL : 0x7FFE0000000ULL;
    ms->ullAvailVirtual = sizeof(void *) == 4 ? 0x70000000ULL : 0x7F000000000ULL;
    ms->ullAvailExtendedVirtual = 0;
    return TRUE;
}

WINBASEAPI BOOL WINAPI FlushInstructionCache(HANDLE p, LPCVOID a, SIZE_T n) { (void)p; (void)a; (void)n; return TRUE; }
WINBASEAPI VOID WINAPI FlushProcessWriteBuffers(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* -----------------------------------------------------------------------
 * Environment and system
 * ----------------------------------------------------------------------- */
WINBASEAPI BOOL WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
    char n[256], *v = 0;
    if (!wide_to_temp(name, n, sizeof(n))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (value) {
        int k = w2u(value, -1, 0, 0);
        v = zalloc((SIZE_T)k + 1);
        if (!v) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        w2u(value, -1, v, k);
    }
    BOOL ok = SetEnvironmentVariableA(n, v);
    zfree(v);
    return ok;
}

WINBASEAPI DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR s, LPWSTR out, DWORD n)
{
    DWORD o = 0;
    WCHAR val[1024];
    for (const WCHAR *c = s; *c; ) {
        if (*c == '%') {
            const WCHAR *e = c + 1;
            while (*e && *e != '%') e++;
            if (*e == '%' && e > c + 1 && e - c < 256) {
                WCHAR name[256];
                int k = (int)(e - c - 1);
                memcpy(name, c + 1, 2 * (SIZE_T)k);
                name[k] = 0;
                DWORD vl = GetEnvironmentVariableW(name, val, 1024);
                if (vl && vl < 1024) {
                    for (DWORD i = 0; i < vl; i++, o++) if (out && o < n) out[o] = val[i];
                    c = e + 1;
                    continue;
                }
            }
        }
        if (out && o < n) out[o] = *c;
        o++;
        c++;
    }
    if (out && o < n) out[o] = 0;
    return o + 1;
}

WINBASEAPI DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR s, LPSTR out, DWORD n)
{
    int k = u2w(s, -1, 0, 0);
    WCHAR *w = zalloc(2 * ((SIZE_T)k + 1));
    if (!w) return 0;
    u2w(s, -1, w, k);
    DWORD need = ExpandEnvironmentStringsW(w, 0, 0);
    WCHAR *x = zalloc(2 * (SIZE_T)need);
    DWORD r = 0;
    if (x) {
        ExpandEnvironmentStringsW(w, x, need);
        int m = w2u(x, -1, 0, 0);
        r = (DWORD)m + 1;
        if (out && n >= r) { w2u(x, -1, out, m); out[m] = 0; }
    }
    zfree(w);
    zfree(x);
    return r;
}

WINBASEAPI BOOL WINAPI GetComputerNameW(LPWSTR buf, LPDWORD n)
{
    char a[64];
    DWORD k = sizeof(a);
    if (!GetComputerNameA(a, &k)) return FALSE;
    WCHAR w[64];
    int m = u2w(a, -1, w, 63);
    if (*n <= (DWORD)m) { *n = (DWORD)m + 1; SetLastError(ERROR_BUFFER_OVERFLOW); return FALSE; }
    memcpy(buf, w, 2 * (SIZE_T)m);
    buf[m] = 0;
    *n = (DWORD)m;
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetComputerNameExW(int type, LPWSTR buf, LPDWORD n)
{
    if (type == 2 || type == 6) {           /* DNS domain names: none */
        if (*n < 1) { *n = 1; SetLastError(ERROR_MORE_DATA); return FALSE; }
        buf[0] = 0;
        *n = 0;
        return TRUE;
    }
    if (!buf) { *n = 64; SetLastError(ERROR_MORE_DATA); return FALSE; }
    BOOL ok = GetComputerNameW(buf, n);
    if (!ok && GetLastError() == ERROR_BUFFER_OVERFLOW) SetLastError(ERROR_MORE_DATA);
    return ok;
}

WINBASEAPI BOOL WINAPI GetComputerNameExA(int type, LPSTR buf, LPDWORD n)
{
    if (type == 2 || type == 6) { if (*n < 1) { *n = 1; SetLastError(ERROR_MORE_DATA); return FALSE; } buf[0] = 0; *n = 0; return TRUE; }
    return GetComputerNameA(buf, n);
}

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD wServicePackMajor, wServicePackMinor, wSuiteMask;
    BYTE wProductType, wReserved;
} OSVERSIONINFOEXW_;

WINBASEAPI BOOL WINAPI GetVersionExW(LPVOID vi)
{
    OSVERSIONINFOEXW_ *v = vi;
    DWORD size = v->dwOSVersionInfoSize;
    if (size != 276 && size != sizeof(OSVERSIONINFOEXW_)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset((BYTE *)v + 4, 0, size - 4);
    DWORD ver = GetVersion();
    v->dwMajorVersion = ver & 0xFF;
    v->dwMinorVersion = (ver >> 8) & 0xFF;
    v->dwBuildNumber = ver >> 16;
    v->dwPlatformId = 2;                    /* VER_PLATFORM_WIN32_NT */
    if (size == sizeof(OSVERSIONINFOEXW_)) v->wProductType = 1;   /* VER_NT_WORKSTATION */
    return TRUE;
}

WINBASEAPI BOOL WINAPI VerifyVersionInfoW(LPVOID vi, DWORD mask, DWORDLONG cond) { (void)vi; (void)mask; (void)cond; return TRUE; }
WINBASEAPI ULONGLONG WINAPI VerSetConditionMask(ULONGLONG cond, DWORD mask, BYTE op)
{
    for (int i = 0; i < 8; i++) if (mask & (1u << i)) cond |= (ULONGLONG)(op & 7) << (3 * i);
    return cond;
}

/* -----------------------------------------------------------------------
 * FormatMessage
 * ----------------------------------------------------------------------- */
static const struct { DWORD code; const char *text; } g_msgs[] = {
    { 0, "The operation completed successfully." },
    { 1, "Incorrect function." },
    { 2, "The system cannot find the file specified." },
    { 3, "The system cannot find the path specified." },
    { 4, "The system cannot open the file." },
    { 5, "Access is denied." },
    { 6, "The handle is invalid." },
    { 8, "Not enough memory resources are available to process this command." },
    { 13, "The data is invalid." },
    { 14, "Not enough memory resources are available to complete this operation." },
    { 15, "The system cannot find the drive specified." },
    { 17, "The system cannot move the file to a different disk drive." },
    { 18, "There are no more files." },
    { 21, "The device is not ready." },
    { 32, "The process cannot access the file because it is being used by another process." },
    { 38, "Reached the end of the file." },
    { 50, "The request is not supported." },
    { 80, "The file exists." },
    { 87, "The parameter is incorrect." },
    { 109, "The pipe has been ended." },
    { 111, "The file name is too long." },
    { 112, "There is not enough space on the disk." },
    { 120, "This function is not supported on this system." },
    { 122, "The data area passed to a system call is too small." },
    { 123, "The filename, directory name, or volume label syntax is incorrect." },
    { 126, "The specified module could not be found." },
    { 127, "The specified procedure could not be found." },
    { 145, "The directory is not empty." },
    { 183, "Cannot create a file when that file already exists." },
    { 203, "The system could not find the environment option that was entered." },
    { 234, "More data is available." },
    { 258, "The wait operation timed out." },
    { 267, "The directory name is invalid." },
    { 487, "Attempt to access invalid address." },
    { 995, "The I/O operation has been aborted because of either a thread exit or an application request." },
    { 996, "Overlapped I/O event is not in a signaled state." },
    { 997, "Overlapped I/O operation is in progress." },
    { 1168, "Element not found." },
    { 1460, "This operation returned because the timeout period expired." },
    { 10013, "An attempt was made to access a socket in a way forbidden by its access permissions." },
    { 10035, "A non-blocking socket operation could not be completed immediately." },
    { 10048, "Only one usage of each socket address (protocol/network address/port) is normally permitted." },
    { 10049, "The requested address is not valid in its context." },
    { 10051, "A socket operation was attempted to an unreachable network." },
    { 10053, "An established connection was aborted by the software in your host machine." },
    { 10054, "An existing connection was forcibly closed by the remote host." },
    { 10057, "A request to send or receive data was disallowed because the socket is not connected." },
    { 10060, "A connection attempt failed because the connected party did not properly respond after a period of time." },
    { 10061, "No connection could be made because the target machine actively refused it." },
    { 11001, "No such host is known." },
};

/* A growable UTF-16 buffer */
typedef struct { WCHAR *p; DWORD n, cap; } WBuf;
static void wb_put(WBuf *b, WCHAR c)
{
    if (b->n + 1 >= b->cap) {
        DWORD cap = b->cap ? b->cap * 2 : 256;
        WCHAR *q = zalloc(2 * (SIZE_T)cap);
        if (!q) return;
        if (b->p) { memcpy(q, b->p, 2 * (SIZE_T)b->n); zfree(b->p); }
        b->p = q; b->cap = cap;
    }
    b->p[b->n++] = c;
}
static void wb_str(WBuf *b, const char *s) { WCHAR w[2]; while (*s) { int k = 0; const char *c = s; (void)k; int n = 1; while ((c[n] & 0xC0) == 0x80) n++; int m = u2w(s, n, w, 2); for (int i = 0; i < m; i++) wb_put(b, w[i]); s += n; } }
static void wb_wstr(WBuf *b, const WCHAR *s) { while (*s) wb_put(b, *s++); }

static void wb_num(WBuf *b, ULONGLONG v, int base, BOOL neg, BOOL upper)
{
    char t[24];
    int k = 0;
    do { int d = (int)(v % (ULONGLONG)base); t[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= (ULONGLONG)base; } while (v);
    if (neg) wb_put(b, '-');
    while (k) wb_put(b, (WCHAR)t[--k]);
}

/* One insert (%n or %n!fmt!) */
static void wb_insert(WBuf *b, const WCHAR *fmt, int flen, ULONG_PTR arg, BOOL wide)
{
    int longs = 0;
    WCHAR conv = 's';
    for (int i = 0; i < flen; i++) {
        WCHAR c = fmt[i];
        if (c == 'l') longs++;
        else if (c == 'I' && i + 2 < flen && fmt[i + 1] == '6' && fmt[i + 2] == '4') { longs = 2; i += 2; }
        else if (c == 'h') longs = -1;
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) conv = c;
    }
    switch (conv) {
    case 's':
        if (!arg) wb_str(b, "(null)");
        else if (wide ? longs >= 0 : longs > 0) wb_wstr(b, (const WCHAR *)arg);
        else wb_str(b, (const char *)arg);
        break;
    case 'S':
        if (!arg) wb_str(b, "(null)");
        else if (wide) wb_str(b, (const char *)arg);
        else wb_wstr(b, (const WCHAR *)arg);
        break;
    case 'c': wb_put(b, (WCHAR)arg); break;
    case 'd': case 'i': {
        LONGLONG v = longs == 2 ? (LONGLONG)arg : (LONGLONG)(int)arg;
        wb_num(b, v < 0 ? (ULONGLONG)-v : (ULONGLONG)v, 10, v < 0, FALSE);
        break;
    }
    case 'u': wb_num(b, longs == 2 ? arg : (DWORD)arg, 10, FALSE, FALSE); break;
    case 'x': wb_num(b, longs == 2 ? arg : (DWORD)arg, 16, FALSE, FALSE); break;
    case 'X': wb_num(b, longs == 2 ? arg : (DWORD)arg, 16, FALSE, TRUE); break;
    case 'p': wb_num(b, arg, 16, FALSE, TRUE); break;
    default: wb_str(b, "?"); break;
    }
}

/* The message as UTF-16 in a heap buffer, or 0 (last error set) */
static WCHAR *format_message(DWORD flags, LPCVOID src, DWORD id, va_list *args, BOOL wide, DWORD *len)
{
    WCHAR *pattern = 0;
    if (flags & FORMAT_MESSAGE_FROM_STRING) {
        if (wide) {
            int n = wlen(src);
            pattern = zalloc(2 * ((SIZE_T)n + 1));
            if (pattern) memcpy(pattern, src, 2 * (SIZE_T)n);
        } else {
            int n = u2w(src, -1, 0, 0);
            pattern = zalloc(2 * ((SIZE_T)n + 1));
            if (pattern) u2w(src, -1, pattern, n);
        }
    } else if (flags & FORMAT_MESSAGE_FROM_SYSTEM) {
        const char *t = 0;
        for (unsigned i = 0; i < sizeof(g_msgs) / sizeof(g_msgs[0]); i++) if (g_msgs[i].code == id) { t = g_msgs[i].text; break; }
        if (!t && (flags & FORMAT_MESSAGE_FROM_HMODULE)) t = 0;
        if (!t) { SetLastError(317 /* ERROR_MR_MID_NOT_FOUND */); return 0; }
        int n = u2w(t, -1, 0, 0);
        pattern = zalloc(2 * ((SIZE_T)n + 3));
        if (pattern) { u2w(t, -1, pattern, n); pattern[n] = '\r'; pattern[n + 1] = '\n'; }
        flags |= FORMAT_MESSAGE_IGNORE_INSERTS;             /* system texts here have none */
    } else {
        SetLastError(317);
        return 0;
    }
    if (!pattern) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }

    WBuf b = { 0, 0, 0 };
    DWORD width = flags & FORMAT_MESSAGE_MAX_WIDTH_MASK;
    ULONG_PTR *array = (flags & FORMAT_MESSAGE_ARGUMENT_ARRAY) ? (ULONG_PTR *)args : 0;
    ULONG_PTR seen[100];
    int nseen = 0;
    for (const WCHAR *c = pattern; *c; c++) {
        if (*c != '%' || (flags & FORMAT_MESSAGE_IGNORE_INSERTS && !(c[1] == '0' || c[1] == 'n' || c[1] == '%'
                                                                     || c[1] == '\\' || c[1] == '.' || c[1] == '!' || c[1] == ' ') )) {
            if ((flags & FORMAT_MESSAGE_IGNORE_INSERTS) && *c == '%') { wb_put(&b, *c); continue; }
            if (width == FORMAT_MESSAGE_MAX_WIDTH_MASK && (*c == '\r' || *c == '\n')) { if (*c == '\n' || c[1] != '\n') wb_put(&b, ' '); continue; }
            wb_put(&b, *c);
            continue;
        }
        c++;
        if (*c == '0') break;                               /* end, no newline */
        if (*c == 'n') { wb_put(&b, '\r'); wb_put(&b, '\n'); continue; }
        if (*c == 'r') { wb_put(&b, '\r'); continue; }
        if (*c == 't') { wb_put(&b, '\t'); continue; }
        if (*c == '%' || *c == '\\' || *c == '.' || *c == '!' || *c == ' ') { wb_put(&b, *c); continue; }
        if (*c >= '1' && *c <= '9') {
            int k = *c - '0';
            if (c[1] >= '0' && c[1] <= '9') k = k * 10 + (*++c - '0');
            const WCHAR *fmt = 0;
            int flen = 0;
            if (c[1] == '!') {
                fmt = c + 2;
                while (fmt[flen] && fmt[flen] != '!') flen++;
                c = fmt + flen;
                if (!*c) c--;
            }
            ULONG_PTR arg = 0;
            if (array) arg = array[k - 1];
            else if (args) {
                while (nseen < k && nseen < 100) seen[nseen++] = va_arg(*args, ULONG_PTR);
                arg = k <= nseen ? seen[k - 1] : 0;
            }
            wb_insert(&b, fmt, flen, arg, wide);
            continue;
        }
        wb_put(&b, *c);
    }
    zfree(pattern);
    wb_put(&b, 0);
    if (!b.p) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    *len = b.n - 1;
    return b.p;
}

WINBASEAPI DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPWSTR buf, DWORD n, va_list *args)
{
    (void)lang;
    DWORD len;
    WCHAR *m = format_message(flags, src, id, args, TRUE, &len);
    if (!m) return 0;
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        DWORD cap = len + 1 > n ? len + 1 : n;
        LPWSTR out = LocalAlloc(0, 2 * (SIZE_T)cap);
        if (!out) { zfree(m); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        memcpy(out, m, 2 * ((SIZE_T)len + 1));
        *(LPWSTR *)buf = out;
    } else {
        if (len + 1 > n) { zfree(m); SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(buf, m, 2 * ((SIZE_T)len + 1));
    }
    zfree(m);
    return len;
}

WINBASEAPI DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPSTR buf, DWORD n, va_list *args)
{
    (void)lang;
    DWORD len;
    WCHAR *m = format_message(flags, src, id, args, FALSE, &len);
    if (!m) return 0;
    int k = w2u(m, (int)len, 0, 0);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        DWORD cap = (DWORD)k + 1 > n ? (DWORD)k + 1 : n;
        LPSTR out = LocalAlloc(0, cap);
        if (!out) { zfree(m); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        w2u(m, (int)len, out, k);
        out[k] = 0;
        *(LPSTR *)buf = out;
    } else {
        if ((DWORD)k + 1 > n) { zfree(m); SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        w2u(m, (int)len, buf, k);
        buf[k] = 0;
    }
    zfree(m);
    return (DWORD)k;
}

/* -----------------------------------------------------------------------
 * National language support: one locale (en-US), UTF-8 code pages
 * ----------------------------------------------------------------------- */
WCHAR k32_upper(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 0x80) return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

WCHAR k32_lower(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

#define LING_IGNORECASE 0x10                                /* LINGUISTIC_IGNORECASE */

static int compare(const WCHAR *a, int na, const WCHAR *b, int nb, BOOL fold)
{
    if (na < 0) na = wlen(a);
    if (nb < 0) nb = wlen(b);
    for (int i = 0; i < na && i < nb; i++) {
        WCHAR x = fold ? k32_upper(a[i]) : a[i], y = fold ? k32_upper(b[i]) : b[i];
        if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    return na == nb ? CSTR_EQUAL : na < nb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

WINBASEAPI int WINAPI CompareStringOrdinal(LPCWSTR a, int na, LPCWSTR b, int nb, BOOL ignore_case)
{
    return compare(a, na, b, nb, ignore_case);
}

WINBASEAPI int WINAPI CompareStringEx(LPCWSTR loc, DWORD flags, LPCWSTR a, int na, LPCWSTR b, int nb, LPVOID v, LPVOID r, LONG_PTR p)
{
    (void)loc; (void)v; (void)r; (void)p;
    return compare(a, na, b, nb, (flags & (NORM_IGNORECASE | LING_IGNORECASE)) != 0);
}

WINBASEAPI int WINAPI CompareStringW(DWORD lcid, DWORD flags, LPCWSTR a, int na, LPCWSTR b, int nb)
{
    (void)lcid;
    return CompareStringEx(0, flags, a, na, b, nb, 0, 0, 0);
}

WINBASEAPI int WINAPI CompareStringA(DWORD lcid, DWORD flags, LPCSTR a, int na, LPCSTR b, int nb)
{
    (void)lcid;
    int la = na < 0 ? (int)strlen(a) : na, lb = nb < 0 ? (int)strlen(b) : nb;
    WCHAR *wa = zalloc(2 * ((SIZE_T)la + 1)), *wb = zalloc(2 * ((SIZE_T)lb + 1));
    int r = 0;
    if (wa && wb) {
        int ka = u2w(a, la, wa, la), kb = u2w(b, lb, wb, lb);
        r = compare(wa, ka, wb, kb, (flags & NORM_IGNORECASE) != 0);
    }
    zfree(wa);
    zfree(wb);
    return r;
}

/* Find @value in @src: from the start or end, or only as a prefix or suffix
 * (FIND_* flags), optionally ignoring case.  Returns the index, or -1. */
static int find_str(const WCHAR *src, int ns, const WCHAR *val, int nv, DWORD flags, BOOL fold, int *found)
{
    if (ns < 0) ns = wlen(src);
    if (nv < 0) nv = wlen(val);
    int first = 0, last = ns - nv;
    if (flags & 0x00100000) last = first;                   /* FIND_STARTSWITH */
    if (flags & 0x00200000) first = last;                   /* FIND_ENDSWITH */
    BOOL back = (flags & (0x00800000 | 0x00200000)) != 0;   /* FIND_FROMEND */
    for (int k = 0; k <= last - first; k++) {
        int i = back ? last - k : first + k;
        if (i < 0) break;
        int j = 0;
        for (; j < nv; j++) {
            WCHAR x = fold ? k32_upper(src[i + j]) : src[i + j], y = fold ? k32_upper(val[j]) : val[j];
            if (x != y) break;
        }
        if (j == nv) { if (found) *found = nv; return i; }
    }
    SetLastError(ERROR_SUCCESS);
    return -1;
}

WINBASEAPI int WINAPI FindNLSStringEx(LPCWSTR loc, DWORD flags, LPCWSTR src, int ns, LPCWSTR val, int nv,
                                      LPINT found, LPVOID ver, LPVOID r, LPARAM h)
{
    (void)loc; (void)ver; (void)r; (void)h;
    if (!src || !val || ns < -1 || nv < -1) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    return find_str(src, ns, val, nv, flags, (flags & (NORM_IGNORECASE | LING_IGNORECASE)) != 0, found);
}

WINBASEAPI int WINAPI FindNLSString(LCID lcid, DWORD flags, LPCWSTR src, int ns, LPCWSTR val, int nv, LPINT found)
{
    (void)lcid;
    return FindNLSStringEx(0, flags, src, ns, val, nv, found, 0, 0, 0);
}

WINBASEAPI int WINAPI FindStringOrdinal(DWORD flags, LPCWSTR src, int ns, LPCWSTR val, int nv, BOOL ignore_case)
{
    if (!src || !val) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    return find_str(src, ns, val, nv, flags, ignore_case, 0);
}

WINBASEAPI int WINAPI LCMapStringEx(LPCWSTR loc, DWORD flags, LPCWSTR s, int n, LPWSTR out, int cap, LPVOID v, LPVOID r, LONG_PTR p)
{
    (void)loc; (void)v; (void)r; (void)p;
    int len = n < 0 ? wlen(s) + 1 : n;
    if (flags & 0x400) {                                    /* LCMAP_SORTKEY: upper-cased code units, big endian */
        int need = 2 * len + 1;
        if (!cap) return need;
        if (cap < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        BYTE *o = (BYTE *)out;
        for (int i = 0; i < len; i++) { WCHAR c = (flags & (NORM_IGNORECASE | LING_IGNORECASE)) ? k32_upper(s[i]) : s[i]; o[2 * i] = (BYTE)(c >> 8); o[2 * i + 1] = (BYTE)c; }
        o[2 * len] = 0;
        return need;
    }
    if (!cap) return len;
    if (cap < len) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < len; i++)
        out[i] = (flags & LCMAP_UPPERCASE) ? k32_upper(s[i]) : (flags & LCMAP_LOWERCASE) ? k32_lower(s[i]) : s[i];
    return len;
}

WINBASEAPI int WINAPI LCMapStringW(DWORD lcid, DWORD flags, LPCWSTR s, int n, LPWSTR out, int cap)
{
    (void)lcid;
    return LCMapStringEx(0, flags, s, n, out, cap, 0, 0, 0);
}

static WORD ctype1(WCHAR c)
{
    WORD t = 0x200;                                         /* C1_DEFINED */
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') t |= 0x1 | 0x100;
        if (c >= 'a' && c <= 'z') t |= 0x2 | 0x100;
        if (c >= '0' && c <= '9') t |= 0x4;
        if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8;
        if (c == ' ' || c == '\t') t |= 0x40;
        if (c < 32 || c == 127) t |= 0x20;
        if (c > 32 && c < 127 && !(t & (0x100 | 0x4))) t |= 0x10;
        if ((c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f')) t |= 0x80;
        return t;
    }
    if (c < 0xA0) return t | 0x20;
    if (c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return t | 0x8 | 0x40;
    if (c < 0xC0 || c == 0xD7 || c == 0xF7 || (c >= 0x2010 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F)) return t | 0x10;
    t |= 0x100;
    if (k32_lower(c) != c) t |= 0x1;
    else if (k32_upper(c) != c) t |= 0x2;
    return t;
}

WINBASEAPI BOOL WINAPI GetStringTypeW(DWORD type, LPCWSTR s, int n, LPWORD out)
{
    if (n < 0) n = wlen(s) + 1;
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (type == CT_CTYPE1) out[i] = ctype1(c);
        else if (type == CT_CTYPE2) out[i] = c < 0x80 ? ((c >= '0' && c <= '9') ? 3 : ((c | 32) >= 'a' && (c | 32) <= 'z') ? 1 : c == ' ' ? 10 : 11) : 1;
        else if (type == CT_CTYPE3) out[i] = (ctype1(c) & 0x100) ? 0x8000 | 0x100 : 0;
        else { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    }
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetStringTypeExW(DWORD lcid, DWORD type, LPCWSTR s, int n, LPWORD out) { (void)lcid; return GetStringTypeW(type, s, n, out); }

WINBASEAPI BOOL WINAPI GetStringTypeA(DWORD lcid, DWORD type, LPCSTR s, int n, LPWORD out)
{
    (void)lcid;
    if (n < 0) n = (int)strlen(s) + 1;
    for (int i = 0; i < n; i++) { WCHAR c = (BYTE)s[i]; GetStringTypeW(type, &c, 1, &out[i]); }
    return TRUE;
}

WINBASEAPI BOOL WINAPI IsValidCodePage(UINT cp)
{
    return cp == 65001 || cp == 65000 || cp == 1252 || cp == 437 || cp == 850 || cp == 20127 || cp == 28591 || cp == 1200 || cp == 1201;
}

WINBASEAPI BOOL WINAPI GetCPInfo(UINT cp, LPCPINFO info)
{
    if (cp == CP_ACP || cp == CP_OEMCP || cp == 3 /* CP_THREAD_ACP */) cp = CP_UTF8;
    if (!IsValidCodePage(cp)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(info, 0, sizeof(*info));
    info->MaxCharSize = cp == CP_UTF8 ? 4 : cp == 65000 ? 5 : 1;
    info->DefaultChar[0] = '?';
    return TRUE;
}

typedef struct { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; WCHAR UnicodeDefaultChar; UINT CodePage; WCHAR CodePageName[260]; } CPINFOEXW_;
WINBASEAPI BOOL WINAPI GetCPInfoExW(UINT cp, DWORD flags, LPVOID out)
{
    (void)flags;
    CPINFOEXW_ *e = out;
    CPINFO ci;
    if (!GetCPInfo(cp, &ci)) return FALSE;
    memset(e, 0, sizeof(*e));
    e->MaxCharSize = ci.MaxCharSize;
    e->DefaultChar[0] = '?';
    e->UnicodeDefaultChar = '?';
    e->CodePage = cp == CP_ACP || cp == CP_OEMCP ? CP_UTF8 : cp;
    u2w(e->CodePage == CP_UTF8 ? "65001 (UTF-8)" : "Code page", -1, e->CodePageName, 259);
    return TRUE;
}

WINBASEAPI UINT WINAPI GetOEMCP(void)                  { return CP_UTF8; }
WINBASEAPI BOOL WINAPI IsDBCSLeadByte(BYTE c)          { (void)c; return FALSE; }
WINBASEAPI BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE c) { (void)cp; (void)c; return FALSE; }

WINBASEAPI DWORD WINAPI GetUserDefaultLCID(void)       { return 0x409; }
WINBASEAPI DWORD WINAPI GetSystemDefaultLCID(void)     { return 0x409; }
WINBASEAPI DWORD WINAPI GetThreadLocale(void)          { return 0x409; }
WINBASEAPI BOOL  WINAPI SetThreadLocale(DWORD lcid)    { (void)lcid; return TRUE; }
WINBASEAPI WORD  WINAPI GetUserDefaultLangID(void)     { return 0x409; }
WINBASEAPI WORD  WINAPI GetSystemDefaultLangID(void)   { return 0x409; }
WINBASEAPI WORD  WINAPI GetUserDefaultUILanguage(void) { return 0x409; }
WINBASEAPI WORD  WINAPI GetSystemDefaultUILanguage(void) { return 0x409; }
WINBASEAPI BOOL  WINAPI IsValidLocale(DWORD lcid, DWORD f) { (void)f; return lcid == 0x409 || lcid == 0x400 || lcid == 0x800 || lcid == 0x7F; }
#define LOC_UNKNOWN_ 3
static int locale_of(LPCWSTR loc);
WINBASEAPI BOOL  WINAPI IsValidLocaleName(LPCWSTR n)   { return locale_of(n) != LOC_UNKNOWN_; }
WINBASEAPI DWORD WINAPI LocaleNameToLCID(LPCWSTR n, DWORD f)
{
    (void)f;
    switch (locale_of(n)) {
    case 0: return 0x409;
    case 1: return 0x09;
    case 2: return 0x7F;
    default: SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
}
WINBASEAPI int   WINAPI LCIDToLocaleName(DWORD lcid, LPWSTR n, int cap, DWORD f) { (void)lcid; (void)f; return cap ? (int)put_utf8_as_w("en-US", n, (DWORD)cap) + 1 : 6; }
WINBASEAPI int   WINAPI GetUserDefaultLocaleName(LPWSTR n, int cap)   { return (int)put_utf8_as_w("en-US", n, (DWORD)cap) + 1; }
WINBASEAPI int   WINAPI GetSystemDefaultLocaleName(LPWSTR n, int cap) { return (int)put_utf8_as_w("en-US", n, (DWORD)cap) + 1; }

/* Locales: English (United States), its neutral parent "en", and the
 * invariant locale "" (what .NET's CultureInfo.InvariantCulture asks for);
 * any other name is unknown (ERROR_INVALID_PARAMETER) */
enum { LOC_EN_US, LOC_EN, LOC_INVARIANT, LOC_UNKNOWN };
static int locale_of(LPCWSTR loc)
{
    static const WCHAR sysdef[] = L"!x-sys-default-locale";
    if (!loc) return LOC_EN_US;                                  /* LOCALE_NAME_USER_DEFAULT */
    if (!loc[0]) return LOC_INVARIANT;
    if (compare(loc, -1, sysdef, -1, TRUE) == CSTR_EQUAL) return LOC_EN_US;
    if (compare(loc, -1, (const WCHAR *)L"en-US", -1, TRUE) == CSTR_EQUAL) return LOC_EN_US;
    if (compare(loc, -1, (const WCHAR *)L"en_US", -1, TRUE) == CSTR_EQUAL) return LOC_EN_US;
    if (compare(loc, -1, (const WCHAR *)L"en", -1, TRUE) == CSTR_EQUAL) return LOC_EN;
    return LOC_UNKNOWN;
}

static const char *locale_value(int loc, DWORD type)
{
    DWORD t = type & 0xFFFF;
    BOOL inv = loc == LOC_INVARIANT, neutral = loc == LOC_EN;
    switch (t) {                                                 /* what differs by locale */
    case 0x01: return inv ? "007F" : neutral ? "0009" : "0409";  /* LOCALE_ILANGUAGE */
    case 0x02: case 0x72: return inv ? "Invariant Language (Invariant Country)" : neutral ? "English" : "English (United States)";
    case 0x73: return inv ? "Invariant Language (Invariant Country)" : neutral ? "English" : "English (United States)";
    case 0x5C: return inv ? "" : neutral ? "en" : "en-US";       /* LOCALE_SNAME */
    case 0x6D: return neutral || inv ? "" : "en";                /* LOCALE_SPARENT */
    case 0x71: return neutral ? "1" : "0";                       /* LOCALE_INEUTRAL */
    case 0x5A: return inv ? "IV" : neutral ? "" : "US";          /* LOCALE_SISO3166CTRYNAME */
    case 0x68: return inv ? "IVC" : neutral ? "" : "USA";        /* LOCALE_SISO3166CTRYNAME2 */
    case 0x59: return inv ? "iv" : "en";                         /* LOCALE_SISO639LANGNAME */
    case 0x67: return inv ? "ivl" : "eng";                       /* LOCALE_SISO639LANGNAME2 */
    case 0x06: case 0x08: case 0x1002: return inv ? "Invariant Country" : neutral ? "" : "United States";
    case 0x5B: return inv ? "39070" : "244";                     /* LOCALE_IGEOID */
    case 0x14: return inv ? "\xC2\xA4" : "$";                    /* LOCALE_SCURRENCY */
    case 0x15: return inv ? "XDR" : "USD";                       /* LOCALE_SINTLSYMBOL */
    case 0x1007: return inv ? "International Monetary Fund" : "US Dollar";   /* LOCALE_SENGCURRNAME */
    case 0x1008: return inv ? "International Monetary Fund" : "US Dollar";   /* LOCALE_SNATIVECURRNAME */
    case 0x1F: return inv ? "MM/dd/yyyy" : "M/d/yyyy";           /* LOCALE_SSHORTDATE */
    case 0x20: return inv ? "dddd, dd MMMM yyyy" : "dddd, MMMM d, yyyy";      /* LOCALE_SLONGDATE */
    case 0x1003: return inv ? "HH:mm:ss" : "h:mm:ss tt";         /* LOCALE_STIMEFORMAT */
    case 0x79: return inv ? "HH:mm" : "h:mm tt";                 /* LOCALE_SSHORTTIME */
    case 0x23: return inv ? "1" : "0";                           /* LOCALE_ITIME (24-hour) */
    case 0x100C: return inv ? "0" : "6";                         /* LOCALE_IFIRSTDAYOFWEEK: Monday 0 ... Sunday 6 */
    case 0x0D: return inv ? "0" : "1";                           /* LOCALE_IMEASURE: metric 0, US 1 */
    case 0x100A: return inv ? "9" : "1";                         /* LOCALE_IPAPERSIZE: A4 9, letter 1 */
    }
    switch (t) {                                                 /* the same for all three */
    case 0x03: return "ENU";                                     /* LOCALE_SABBREVLANGNAME */
    case 0x04: case 0x1001: case 0x6F: return "English";         /* native / English / localized language name */
    case 0x05: return "1";                                       /* LOCALE_ICOUNTRY */
    case 0x07: return "USA";                                     /* LOCALE_SABBREVCTRYNAME */
    case 0x0B: return "437";                                     /* LOCALE_IDEFAULTCODEPAGE (OEM) */
    case 0x1004: return "1252";                                  /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x1011: return "10000";                                 /* LOCALE_IDEFAULTMACCODEPAGE */
    case 0x1012: return "037";                                   /* LOCALE_IDEFAULTEBCDICCODEPAGE */
    case 0x0C: return ",";                                       /* LOCALE_SLIST */
    case 0x0E: return ".";                                       /* LOCALE_SDECIMAL */
    case 0x0F: return ",";                                       /* LOCALE_STHOUSAND */
    case 0x10: return "3;0";                                     /* LOCALE_SGROUPING */
    case 0x11: return "2";                                       /* LOCALE_IDIGITS */
    case 0x12: return "1";                                       /* LOCALE_ILZERO */
    case 0x13: return "0123456789";                              /* LOCALE_SNATIVEDIGITS */
    case 0x16: return ".";                                       /* LOCALE_SMONDECIMALSEP */
    case 0x17: return ",";                                       /* LOCALE_SMONTHOUSANDSEP */
    case 0x18: return "3;0";                                     /* LOCALE_SMONGROUPING */
    case 0x19: return "2";                                       /* LOCALE_ICURRDIGITS */
    case 0x1A: return "2";                                       /* LOCALE_IINTLCURRDIGITS */
    case 0x1B: return "0";                                       /* LOCALE_ICURRENCY: $1.1 */
    case 0x1C: return "1";                                       /* LOCALE_INEGCURR: -$1.1 */
    case 0x1D: return "/";                                       /* LOCALE_SDATE */
    case 0x1E: return ":";                                       /* LOCALE_STIME */
    case 0x21: return "0";                                       /* LOCALE_IDATE: M-D-Y */
    case 0x22: return "0";                                       /* LOCALE_ILDATE */
    case 0x24: return "1";                                       /* LOCALE_ICENTURY */
    case 0x25: return "0";                                       /* LOCALE_ITLZERO */
    case 0x26: return "0";                                       /* LOCALE_IDAYLZERO */
    case 0x27: return "0";                                       /* LOCALE_IMONLZERO */
    case 0x28: return "AM";                                      /* LOCALE_S1159 */
    case 0x29: return "PM";                                      /* LOCALE_S2359 */
    case 0x50: return "";                                        /* LOCALE_SPOSITIVESIGN */
    case 0x51: return "-";                                       /* LOCALE_SNEGATIVESIGN */
    case 0x52: return "3";                                       /* LOCALE_IPOSSIGNPOSN */
    case 0x53: return "0";                                       /* LOCALE_INEGSIGNPOSN */
    case 0x5D: return "h:mm:ss";                                 /* LOCALE_SDURATION */
    case 0x69: return "NaN";                                     /* LOCALE_SNAN */
    case 0x6A: return "Infinity";                                /* LOCALE_SPOSINFINITY */
    case 0x6B: return "-Infinity";                               /* LOCALE_SNEGINFINITY */
    case 0x6C: return "Latn;";                                   /* LOCALE_SSCRIPTS */
    case 0x6E: return "en-US";                                   /* LOCALE_SCONSOLEFALLBACKNAME */
    case 0x70: return "0";                                       /* LOCALE_IREADINGLAYOUT: left to right */
    case 0x74: return "1";                                       /* LOCALE_INEGATIVEPERCENT: -n % */
    case 0x75: return "1";                                       /* LOCALE_IPOSITIVEPERCENT: n % */
    case 0x76: return "%";                                       /* LOCALE_SPERCENT */
    case 0x77: return "\xE2\x80\xB0";                            /* LOCALE_SPERMILLE */
    case 0x78: return "MMMM d";                                  /* LOCALE_SMONTHDAY */
    case 0x7A: return "ENU";                                     /* LOCALE_SOPENTYPELANGUAGETAG */
    case 0x7B: return "";                                        /* LOCALE_SSORTLOCALE */
    case 0x1006: return "MMMM yyyy";                             /* LOCALE_SYEARMONTH */
    case 0x1009: return "1";                                     /* LOCALE_ICALENDARTYPE: Gregorian */
    case 0x100B: return "0";                                     /* LOCALE_IOPTIONALCALENDAR */
    case 0x100D: return "0";                                     /* LOCALE_IFIRSTWEEKOFYEAR */
    case 0x100E: case 0x100F: return "";                         /* LOCALE_SMONTHNAME13, ABBREV13 */
    case 0x1010: return "1";                                     /* LOCALE_INEGNUMBER: -1.1 */
    case 0x1014: return "1";                                     /* LOCALE_IDIGITSUBSTITUTION: none */
    case 0x1016: return "en-US";                                 /* LOCALE_SNATIVELANGUAGE... (unused) */
    }
    if (t >= 0x2A && t <= 0x30) {                                /* LOCALE_SDAYNAME1..7 (Monday first) */
        static const char *d[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
        return d[t - 0x2A];
    }
    if (t >= 0x31 && t <= 0x37) {
        static const char *d[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
        return d[t - 0x31];
    }
    if (t >= 0x60 && t <= 0x66) {                                /* LOCALE_SSHORTESTDAYNAME1..7 */
        static const char *d[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
        return d[t - 0x60];
    }
    if (t >= 0x38 && t <= 0x43) {
        static const char *m[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December" };
        return m[t - 0x38];
    }
    if (t >= 0x44 && t <= 0x4F) {
        static const char *m[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        return m[t - 0x44];
    }
    return 0;
}

WINBASEAPI int WINAPI GetLocaleInfoEx(LPCWSTR name, DWORD type, LPWSTR buf, int n)
{
    int loc = locale_of(name);
    if (loc == LOC_UNKNOWN) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const char *v = locale_value(loc, type);
    if (!v) { SetLastError(1004 /* ERROR_INVALID_FLAGS */); return 0; }
    if (type & 0x20000000) {                                 /* LOCALE_RETURN_NUMBER */
        DWORD num = 0;
        DWORD t = type & 0xFFFF;
        if (t == 0x01) num = loc == LOC_INVARIANT ? 0x7F : loc == LOC_EN ? 0x09 : 0x409;   /* (hex) */
        else for (const char *c = v; *c >= '0' && *c <= '9'; c++) num = num * 10 + (DWORD)(*c - '0');
        if (!n) return 2;
        if (n < 2) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(buf, &num, 4);
        return 2;
    }
    WCHAR w[64];
    int k = u2w(v, -1, w, 63);
    if (!n) return k + 1;
    if (n <= k) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, w, 2 * (SIZE_T)k);
    buf[k] = 0;
    return k + 1;
}

static LPCWSTR lcid_name(DWORD lcid)
{
    if (lcid == 0x7F) return (const WCHAR *)L"";
    if (lcid == 0x09) return (const WCHAR *)L"en";
    return 0;                                                    /* 0x409, the defaults: en-US */
}
WINBASEAPI int WINAPI GetLocaleInfoW(DWORD lcid, DWORD type, LPWSTR buf, int n) { return GetLocaleInfoEx(lcid_name(lcid), type, buf, n); }

WINBASEAPI int WINAPI GetLocaleInfoA(DWORD lcid, DWORD type, LPSTR buf, int n)
{
    LPCWSTR nm = lcid_name(lcid);
    const char *v = locale_value(locale_of(nm), type);
    if (!v) { SetLastError(1004); return 0; }
    int k = (int)strlen(v);
    if (!n) return k + 1;
    if (n <= k) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, v, (SIZE_T)k + 1);
    return k + 1;
}

WINBASEAPI BOOL WINAPI EnumSystemLocalesW(BOOL (WINAPI *fn)(LPWSTR), DWORD f) { (void)f; WCHAR s[] = { '0','0','0','0','0','4','0','9',0 }; fn(s); return TRUE; }
WINBASEAPI BOOL WINAPI EnumSystemLocalesEx(BOOL (WINAPI *fn)(LPWSTR, DWORD, LPARAM), DWORD f, LPARAM p, LPVOID r)
{
    (void)f; (void)r;
    WCHAR s[] = { 'e','n','-','U','S',0 };
    fn(s, 1, p);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Console
 * ----------------------------------------------------------------------- */
WINBASEAPI BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info)
{
    if (GetFileType(h) != FILE_TYPE_CHAR) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    memset(info, 0, sizeof(*info));
    info->dwSize.X = 80; info->dwSize.Y = 300;
    info->wAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    info->srWindow.Right = 79; info->srWindow.Bottom = 24;
    info->dwMaximumWindowSize.X = 80; info->dwMaximumWindowSize.Y = 25;
    return TRUE;
}

WINBASEAPI BOOL WINAPI SetConsoleTextAttribute(HANDLE h, WORD attr)
{
    (void)attr;                             /* the Terminal draws plain text */
    if (GetFileType(h) != FILE_TYPE_CHAR) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

static PHANDLER_ROUTINE g_ctrl[8];
WINBASEAPI BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE fn, BOOL add)
{
    if (!fn) return TRUE;                   /* ignore/restore Ctrl+C for this process */
    lock();
    BOOL ok = FALSE;
    for (int i = 0; i < 8; i++) {
        if (add && !g_ctrl[i]) { g_ctrl[i] = fn; ok = TRUE; break; }
        if (!add && g_ctrl[i] == fn) { g_ctrl[i] = 0; ok = TRUE; break; }
    }
    unlock();
    if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
    return ok;
}

WINBASEAPI BOOL WINAPI ReadConsoleW(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID c)
{
    (void)c;
    char tmp[1024];
    DWORD want = n > 256 ? 256 : n, got = 0;
    if (!ReadFile(h, tmp, want, &got, 0)) return FALSE;
    /* finish a UTF-8 sequence cut off at the end */
    while (got && got < sizeof(tmp)) {
        int i = (int)got - 1, back = 0;
        while (i > 0 && ((BYTE)tmp[i] & 0xC0) == 0x80 && back < 3) { i--; back++; }
        BYTE lead = (BYTE)tmp[i];
        int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (back + 1 >= need) break;
        DWORD more = 0;
        if (!ReadFile(h, tmp + got, (DWORD)(need - back - 1), &more, 0) || !more) break;
        got += more;
    }
    int k = u2w(tmp, (int)got, buf, (int)n);
    if (read) *read = k < 0 ? 0 : (DWORD)k;
    return TRUE;
}

WINBASEAPI BOOL WINAPI SetConsoleCP(UINT cp)             { (void)cp; return TRUE; }
WINBASEAPI BOOL WINAPI SetConsoleTitleW(LPCWSTR t)       { (void)t; return TRUE; }
WINBASEAPI DWORD WINAPI GetConsoleTitleW(LPWSTR t, DWORD n) { return put_utf8_as_w("Terminal", t, n); }
/* (console input handles only, as on Windows: callers use these to tell a console from a file) */
/* A console input handle (not an output one, nor a file): what programs
 * test with GetNumberOfConsoleInputEvents to tell the two apart */
static BOOL console_in(HANDLE h)
{
    DWORD m;
    if (GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &m)) {
        union { UNICODE_STRING us; BYTE b[256]; } name;
        ULONG got = 0;
        if (NT_SUCCESS(NtQueryObject(h, 1 /* ObjectNameInformation */, &name, sizeof(name), &got)) && name.us.Buffer) {
            int n = name.us.Length / 2;
            if (n >= 5 && name.us.Buffer[n - 5] == 'I' && name.us.Buffer[n - 1] == 't') return TRUE;   /* ...\Input */
        }
    }
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
WINBASEAPI BOOL WINAPI FlushConsoleInputBuffer(HANDLE h) { return console_in(h); }
WINBASEAPI BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, LPDWORD n) { *n = 0; return console_in(h); }
WINBASEAPI BOOL WINAPI AllocConsole(void)                { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
WINBASEAPI BOOL WINAPI FreeConsole(void)                 { return TRUE; }
WINBASEAPI BOOL WINAPI AttachConsole(DWORD pid)          { (void)pid; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
/* The Terminal window a console program shows in stands in as its console
 * window (the same value in every program on a console; NULL without one) */
WINBASEAPI HANDLE WINAPI GetConsoleWindow(void)
{
    DWORD m;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), in = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(out, &m) && !GetConsoleMode(in, &m)) {
        HANDLE c = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, 3, 0, OPEN_EXISTING, 0, 0);
        if (c == INVALID_HANDLE_VALUE) return 0;
        CloseHandle(c);
    }
    return (HANDLE)(ULONG_PTR)0x000C0501;
}
WINBASEAPI BOOL WINAPI SetConsoleCursorPosition(HANDLE h, COORD c) { (void)h; (void)c; return TRUE; }
WINBASEAPI BOOL WINAPI GetConsoleCursorInfo(HANDLE h, LPVOID i) { (void)h; memset(i, 0, 8); ((DWORD *)i)[0] = 25; ((DWORD *)i)[1] = 1; return TRUE; }
WINBASEAPI BOOL WINAPI SetConsoleCursorInfo(HANDLE h, LPCVOID i) { (void)h; (void)i; return TRUE; }
WINBASEAPI BOOL WINAPI FillConsoleOutputCharacterW(HANDLE h, WCHAR c, DWORD n, COORD at, LPDWORD done) { (void)h; (void)c; (void)at; if (done) *done = n; return TRUE; }
WINBASEAPI BOOL WINAPI FillConsoleOutputAttribute(HANDLE h, WORD a, DWORD n, COORD at, LPDWORD done) { (void)h; (void)a; (void)at; if (done) *done = n; return TRUE; }

/* -----------------------------------------------------------------------
 * Time zones (NovaOS keeps UTC)
 * ----------------------------------------------------------------------- */
static void utc_zone(LPTIME_ZONE_INFORMATION tz)
{
    memset(tz, 0, sizeof(*tz));
    u2w("Coordinated Universal Time", -1, tz->StandardName, 31);
    u2w("Coordinated Universal Time", -1, tz->DaylightName, 31);
}

WINBASEAPI DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz) { utc_zone(tz); return TIME_ZONE_ID_UNKNOWN; }

WINBASEAPI BOOL WINAPI GetTimeZoneInformationForYear(USHORT year, DYNAMIC_TIME_ZONE_INFORMATION *d, LPTIME_ZONE_INFORMATION tz)
{
    (void)year; (void)d;
    utc_zone(tz);
    return TRUE;
}

WINBASEAPI DWORD WINAPI GetDynamicTimeZoneInformation(DYNAMIC_TIME_ZONE_INFORMATION *d)
{
    TIME_ZONE_INFORMATION tz;
    utc_zone(&tz);
    memset(d, 0, sizeof(*d));
    memcpy(d, &tz, sizeof(tz));
    u2w("UTC", -1, d->TimeZoneKeyName, 127);
    return TIME_ZONE_ID_UNKNOWN;
}

WINBASEAPI BOOL WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *u, LPSYSTEMTIME l)
{
    (void)tz;
    *l = *u;
    return TRUE;
}

WINBASEAPI BOOL WINAPI TzSpecificLocalTimeToSystemTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *l, LPSYSTEMTIME u)
{
    (void)tz;
    *u = *l;
    return TRUE;
}

WINBASEAPI BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *u, LPFILETIME l) { *l = *u; return TRUE; }
WINBASEAPI BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *l, LPFILETIME u) { *u = *l; return TRUE; }
WINBASEAPI VOID WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME ft)           { GetSystemTimeAsFileTime(ft); }
WINBASEAPI BOOL WINAPI SetLocalTime(const SYSTEMTIME *st)                     { (void)st; SetLastError(1314 /* PRIVILEGE_NOT_HELD */); return FALSE; }
WINBASEAPI BOOL WINAPI SetSystemTime(const SYSTEMTIME *st)                    { (void)st; SetLastError(1314); return FALSE; }

WINBASEAPI LONG WINAPI CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    ULONGLONG x = (ULONGLONG)a->dwHighDateTime << 32 | a->dwLowDateTime;
    ULONGLONG y = (ULONGLONG)b->dwHighDateTime << 32 | b->dwLowDateTime;
    return x < y ? -1 : x > y;
}

WINBASEAPI BOOL WINAPI GetSystemTimes(LPFILETIME idle, LPFILETIME kernel, LPFILETIME user)
{
    if (idle) memset(idle, 0, sizeof(*idle));
    if (kernel) memset(kernel, 0, sizeof(*kernel));
    if (user) memset(user, 0, sizeof(*user));
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetProcessTimes(HANDLE p, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel, LPFILETIME user)
{
    (void)p;
    GetSystemTimeAsFileTime(created);
    memset(exited, 0, sizeof(*exited));
    memset(kernel, 0, sizeof(*kernel));
    ULONGLONG t = GetTickCount64() * 10000;             /* run time as user time: an estimate */
    user->dwLowDateTime = (DWORD)t;
    user->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetThreadTimes(HANDLE t, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel, LPFILETIME user)
{
    (void)t;
    return GetProcessTimes(0, created, exited, kernel, user);
}

WINBASEAPI BOOL WINAPI QueryProcessCycleTime(HANDLE p, PULONG64 cycles) { (void)p; *cycles = __builtin_ia32_rdtsc(); return TRUE; }
WINBASEAPI BOOL WINAPI QueryThreadCycleTime(HANDLE t, PULONG64 cycles)  { (void)t; *cycles = __builtin_ia32_rdtsc(); return TRUE; }
WINBASEAPI VOID WINAPI QueryUnbiasedInterruptTime(PULONGLONG t)        { *t = GetTickCount64() * 10000; }
WINBASEAPI VOID WINAPI QueryInterruptTime(PULONGLONG t)                { *t = GetTickCount64() * 10000; }

/* -----------------------------------------------------------------------
 * lstr* (the rest of them)
 * ----------------------------------------------------------------------- */
WINBASEAPI LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s) { LPWSTR r = d; while ((*d++ = *s++)) ; return r; }
WINBASEAPI LPSTR  WINAPI lstrcatA(LPSTR d, LPCSTR s)   { lstrcpyA(d + strlen(d), s); return d; }
WINBASEAPI LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s) { lstrcpyW(d + wlen(d), s); return d; }

WINBASEAPI LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n)
{
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
    return d;
}

WINBASEAPI LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n)
{
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
    return d;
}

WINBASEAPI int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b)  { return compare(a, -1, b, -1, FALSE) - 2; }
WINBASEAPI int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b) { return compare(a, -1, b, -1, TRUE) - 2; }

/* -----------------------------------------------------------------------
 * Process status (psapi.dll forwards here, as on Windows 7 and later)
 * ----------------------------------------------------------------------- */
typedef struct { LPVOID lpBaseOfDll; DWORD SizeOfImage; LPVOID EntryPoint; } MODULEINFO;
typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
           QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;

static BOOL self_process(HANDLE p)
{
    if (p == GetCurrentProcess()) return TRUE;
    ULONG64 info[3];
    return NT_SUCCESS(NtNovaProcessInfo(p, info)) && info[0] == GetCurrentProcessId();
}

WINBASEAPI BOOL WINAPI K32EnumProcesses(DWORD *pids, DWORD cb, DWORD *needed)
{
    NOVA_PROCESS_ENTRY list[64];
    ULONG n = 0;
    NTSTATUS s = NtNovaProcessList(list, 64, &n);
    if (!NT_SUCCESS(s)) return fail_status(s);
    DWORD k = 0;
    for (ULONG i = 0; i < n && i < 64 && (k + 1) * 4 <= cb; i++)
        if (!list[i].Exited) pids[k++] = list[i].Pid;
    *needed = k * 4;
    return TRUE;
}

WINBASEAPI BOOL WINAPI K32EnumProcessModulesEx(HANDLE p, HMODULE *mods, DWORD cb, LPDWORD needed, DWORD filter)
{
    (void)filter;
    if (!self_process(p)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    NOVA_LDR_INFO *li = NOVA_LDR_INFO_ADDRESS;
    DWORD n = 0;
    HMODULE image = RtlGetCurrentPeb()->ImageBaseAddress;
    if (cb >= sizeof(HMODULE)) mods[0] = image;             /* the program first, as on Windows */
    n = 1;
    for (ULONG i = 0; i < li->Count && i < 64; i++) {
        HMODULE m = (HMODULE)(ULONG_PTR)li->Modules[i].Base;
        if (m == image) continue;
        if ((n + 1) * sizeof(HMODULE) <= cb) mods[n] = m;
        n++;
    }
    *needed = n * (DWORD)sizeof(HMODULE);
    return TRUE;
}

WINBASEAPI BOOL WINAPI K32EnumProcessModules(HANDLE p, HMODULE *mods, DWORD cb, LPDWORD needed)
{
    return K32EnumProcessModulesEx(p, mods, cb, needed, 3);
}

static DWORD module_name(HANDLE p, HMODULE m, char *out, BOOL base_only)
{
    if (p && !self_process(p)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    char tmp[MAX_PATH];
    const char *path = k32_module_path(m, tmp);
    if (!path) { SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
    const char *s = path;
    if (base_only) for (const char *c = path; *c; c++) if (*c == '\\') s = c + 1;
    DWORD n = (DWORD)strlen(s);
    memcpy(out, s, n + 1);
    return n;
}

WINBASEAPI DWORD WINAPI K32GetModuleBaseNameA(HANDLE p, HMODULE m, LPSTR buf, DWORD n)
{
    char t[MAX_PATH];
    DWORD k = module_name(p, m, t, TRUE);
    if (!k || !n) return 0;
    if (k >= n) k = n - 1;
    memcpy(buf, t, k);
    buf[k] = 0;
    return k;
}

WINBASEAPI DWORD WINAPI K32GetModuleBaseNameW(HANDLE p, HMODULE m, LPWSTR buf, DWORD n)
{
    char t[MAX_PATH];
    if (!module_name(p, m, t, TRUE) || !n) return 0;
    int k = u2w(t, -1, buf, (int)n - 1);
    if (k < 0) k = (int)n - 1;
    buf[k] = 0;
    return (DWORD)k;
}

WINBASEAPI DWORD WINAPI K32GetModuleFileNameExA(HANDLE p, HMODULE m, LPSTR buf, DWORD n)
{
    char t[MAX_PATH];
    DWORD k = module_name(p, m, t, FALSE);
    if (!k || !n) return 0;
    if (k >= n) k = n - 1;
    memcpy(buf, t, k);
    buf[k] = 0;
    return k;
}

WINBASEAPI DWORD WINAPI K32GetModuleFileNameExW(HANDLE p, HMODULE m, LPWSTR buf, DWORD n)
{
    char t[MAX_PATH];
    if (!module_name(p, m, t, FALSE) || !n) return 0;
    int k = u2w(t, -1, buf, (int)n - 1);
    if (k < 0) k = (int)n - 1;
    buf[k] = 0;
    return (DWORD)k;
}

WINBASEAPI DWORD WINAPI K32GetProcessImageFileNameW(HANDLE p, LPWSTR buf, DWORD n)
{
    return K32GetModuleFileNameExW(p, 0, buf, n);
}

WINBASEAPI BOOL WINAPI QueryFullProcessImageNameW(HANDLE p, DWORD flags, LPWSTR buf, PDWORD n)
{
    (void)flags;
    DWORD k = K32GetModuleFileNameExW(p, 0, buf, *n);
    if (!k) return FALSE;
    *n = k;
    return TRUE;
}

WINBASEAPI BOOL WINAPI QueryFullProcessImageNameA(HANDLE p, DWORD flags, LPSTR buf, PDWORD n)
{
    (void)flags;
    DWORD k = K32GetModuleFileNameExA(p, 0, buf, *n);
    if (!k) return FALSE;
    *n = k;
    return TRUE;
}

WINBASEAPI BOOL WINAPI K32GetModuleInformation(HANDLE p, HMODULE m, MODULEINFO *mi, DWORD cb)
{
    if (!self_process(p) || cb < sizeof(*mi)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    BYTE *b = (BYTE *)m;
    if (!b) b = RtlGetCurrentPeb()->ImageBaseAddress;
    if (b[0] != 'M' || b[1] != 'Z') { SetLastError(ERROR_MOD_NOT_FOUND); return FALSE; }
    BYTE *nt = b + *(DWORD *)(b + 0x3C);
    mi->lpBaseOfDll = b;
    mi->SizeOfImage = *(DWORD *)(nt + 24 + 56);
    DWORD entry = *(DWORD *)(nt + 24 + 16);
    mi->EntryPoint = entry ? b + entry : 0;
    return TRUE;
}

WINBASEAPI BOOL WINAPI K32GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS_EX *pmc, DWORD cb)
{
    if (cb < 72) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    ULONG64 info[3];
    DWORD pid = p == GetCurrentProcess() ? GetCurrentProcessId() : (NT_SUCCESS(NtNovaProcessInfo(p, info)) ? (DWORD)info[0] : 0);
    NOVA_PROCESS_ENTRY list[64];
    ULONG n = 0;
    NtNovaProcessList(list, 64, &n);
    SIZE_T bytes = 0;
    for (ULONG i = 0; i < n && i < 64; i++) if (list[i].Pid == pid) bytes = (SIZE_T)list[i].MemoryKb * 1024;
    memset(pmc, 0, cb < sizeof(*pmc) ? cb : sizeof(*pmc));
    pmc->cb = cb;
    pmc->WorkingSetSize = pmc->PeakWorkingSetSize = bytes;
    pmc->PagefileUsage = pmc->PeakPagefileUsage = bytes;
    if (cb >= sizeof(*pmc)) pmc->PrivateUsage = bytes;
    return TRUE;
}

WINBASEAPI BOOL WINAPI K32EmptyWorkingSet(HANDLE p) { (void)p; return TRUE; }
WINBASEAPI BOOL WINAPI K32InitializeProcessForWsWatch(HANDLE p) { (void)p; return TRUE; }
WINBASEAPI DWORD WINAPI K32GetMappedFileNameW(HANDLE p, LPVOID a, LPWSTR buf, DWORD n)
{
    (void)p; (void)a; (void)buf; (void)n;
    SetLastError(1006 /* ERROR_FILE_INVALID */);
    return 0;
}

typedef struct {
    DWORD cb; SIZE_T CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable, SystemCache,
    KernelTotal, KernelPaged, KernelNonpaged, PageSize; DWORD HandleCount, ProcessCount, ThreadCount;
} PERFORMANCE_INFORMATION;

WINBASEAPI BOOL WINAPI K32GetPerformanceInfo(PERFORMANCE_INFORMATION *pi, DWORD cb)
{
    if (cb < sizeof(*pi)) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    NOVA_PROCESS_ENTRY list[64];
    ULONG n = 0;
    NtNovaProcessList(list, 64, &n);
    memset(pi, 0, sizeof(*pi));
    pi->cb = sizeof(*pi);
    pi->PageSize = 4096;
    pi->PhysicalTotal = (SIZE_T)(ms.ullTotalPhys / 4096);
    pi->PhysicalAvailable = (SIZE_T)(ms.ullAvailPhys / 4096);
    pi->CommitLimit = pi->PhysicalTotal;
    pi->CommitTotal = pi->PhysicalTotal - pi->PhysicalAvailable;
    pi->CommitPeak = pi->CommitTotal;
    pi->ProcessCount = n;
    for (ULONG i = 0; i < n && i < 64; i++) pi->ThreadCount += list[i].Threads;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Resources of a loaded module (the image's .rsrc directory)
 * ----------------------------------------------------------------------- */
static const BYTE *res_root(HMODULE m, DWORD *size)
{
    const BYTE *b = m ? (const BYTE *)m : RtlGetCurrentPeb()->ImageBaseAddress;
    if (!b || b[0] != 'M' || b[1] != 'Z') return 0;
    const BYTE *nt = b + *(const DWORD *)(b + 0x3C);
    DWORD dd = *(const WORD *)(nt + 24) == 0x10B ? 96 : 112;  /* PE32 / PE32+ */
    DWORD rva = *(const DWORD *)(nt + 24 + dd + 8 * 2);
    if (size) *size = *(const DWORD *)(nt + 24 + dd + 4 + 8 * 2);
    return rva ? b + rva : 0;
}

static int res_name_eq(const BYTE *root, DWORD name_off, LPCWSTR want)
{
    const WORD *s = (const WORD *)(root + (name_off & 0x7FFFFFFF));
    WORD n = s[0];
    for (WORD i = 0; i < n; i++) {
        WCHAR a = s[1 + i], b = want[i];
        if (!b) return 0;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return want[n] == 0;
}

/* One directory level: the entry for @id (an integer or a name; "#123" too), or the first when @id is 0 */
static DWORD res_find(const BYTE *root, DWORD dir, LPCWSTR id)
{
    const BYTE *d = root + dir;
    WORD named = *(const WORD *)(d + 12), ids = *(const WORD *)(d + 14);
    const DWORD *e = (const DWORD *)(d + 16);
    ULONG_PTR key = (ULONG_PTR)id;
    if (key >= 0x10000 && id[0] == '#') {
        key = 0;
        for (const WCHAR *c = id + 1; *c >= '0' && *c <= '9'; c++) key = key * 10 + (ULONG_PTR)(*c - '0');
    }
    for (int i = 0; i < named + ids; i++, e += 2) {
        if (!id) return e[1];
        if (key < 0x10000) { if (i >= named && e[0] == key) return e[1]; }
        else if (i < named && res_name_eq(root, e[0], id)) return e[1];
    }
    return 0xFFFFFFFF;
}

WINBASEAPI HANDLE WINAPI FindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang)
{
    const BYTE *root = res_root(m, 0);
    if (!root) { SetLastError(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */); return 0; }
    DWORD t = res_find(root, 0, type);
    if (t == 0xFFFFFFFF || !(t & 0x80000000)) { SetLastError(1813 /* ERROR_RESOURCE_TYPE_NOT_FOUND */); return 0; }
    DWORD n = res_find(root, t & 0x7FFFFFFF, name);
    if (n == 0xFFFFFFFF || !(n & 0x80000000)) { SetLastError(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return 0; }
    DWORD l = lang ? res_find(root, n & 0x7FFFFFFF, (LPCWSTR)(ULONG_PTR)lang) : 0xFFFFFFFF;
    if (l == 0xFFFFFFFF) l = res_find(root, n & 0x7FFFFFFF, 0);
    if (l == 0xFFFFFFFF || (l & 0x80000000)) { SetLastError(1815 /* ERROR_RESOURCE_LANG_NOT_FOUND */); return 0; }
    return (HANDLE)(root + l);                              /* IMAGE_RESOURCE_DATA_ENTRY */
}

WINBASEAPI HANDLE WINAPI FindResourceW(HMODULE m, LPCWSTR name, LPCWSTR type) { return FindResourceExW(m, type, name, 0); }

static LPCWSTR res_id_a2w(LPCSTR s, WCHAR *buf)
{
    if ((ULONG_PTR)s < 0x10000) return (LPCWSTR)s;
    u2w(s, -1, buf, 63);
    buf[63] = 0;
    int n = u2w(s, -1, 0, 0);
    buf[n < 63 ? n : 63] = 0;
    return buf;
}

WINBASEAPI HANDLE WINAPI FindResourceA(HMODULE m, LPCSTR name, LPCSTR type)
{
    WCHAR a[64], b[64];
    return FindResourceExW(m, res_id_a2w(type, b), res_id_a2w(name, a), 0);
}

WINBASEAPI HANDLE WINAPI FindResourceExA(HMODULE m, LPCSTR type, LPCSTR name, WORD lang)
{
    WCHAR a[64], b[64];
    return FindResourceExW(m, res_id_a2w(type, b), res_id_a2w(name, a), lang);
}

WINBASEAPI HGLOBAL WINAPI LoadResource(HMODULE m, HANDLE r)
{
    if (!r) return 0;
    const BYTE *b = m ? (const BYTE *)m : RtlGetCurrentPeb()->ImageBaseAddress;
    return (HGLOBAL)(b + *(const DWORD *)r);
}

WINBASEAPI LPVOID WINAPI LockResource(HGLOBAL h) { return h; }
WINBASEAPI BOOL WINAPI FreeResource(HGLOBAL h) { (void)h; return FALSE; }
WINBASEAPI DWORD WINAPI SizeofResource(HMODULE m, HANDLE r) { (void)m; return r ? ((const DWORD *)r)[1] : 0; }

typedef BOOL (CALLBACK *ENUMRESNAMEPROCW)(HMODULE, LPCWSTR, LPWSTR, LONG_PTR);
WINBASEAPI BOOL WINAPI EnumResourceNamesW(HMODULE m, LPCWSTR type, ENUMRESNAMEPROCW fn, LONG_PTR lp)
{
    const BYTE *root = res_root(m, 0);
    if (!root) { SetLastError(1812); return FALSE; }
    DWORD t = res_find(root, 0, type);
    if (t == 0xFFFFFFFF || !(t & 0x80000000)) { SetLastError(1813); return FALSE; }
    const BYTE *d = root + (t & 0x7FFFFFFF);
    WORD named = *(const WORD *)(d + 12), ids = *(const WORD *)(d + 14);
    const DWORD *e = (const DWORD *)(d + 16);
    for (int i = 0; i < named + ids; i++, e += 2) {
        WCHAR buf[128];
        LPWSTR name;
        if (i < named) {
            const WORD *s = (const WORD *)(root + (e[0] & 0x7FFFFFFF));
            int n = s[0] < 127 ? s[0] : 127;
            memcpy(buf, s + 1, 2 * (SIZE_T)n);
            buf[n] = 0;
            name = buf;
        } else name = (LPWSTR)(ULONG_PTR)e[0];
        if (!fn(m, type, name, lp)) break;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Global/Local memory: handles are the pointers (fixed memory)
 * ----------------------------------------------------------------------- */
WINBASEAPI LPVOID  WINAPI GlobalLock(HGLOBAL h)   { return h; }
WINBASEAPI BOOL    WINAPI GlobalUnlock(HGLOBAL h) { (void)h; SetLastError(0); return FALSE; }   /* no longer locked */
WINBASEAPI SIZE_T  WINAPI GlobalSize(HGLOBAL h)   { return h ? HeapSize(GetProcessHeap(), 0, h) : 0; }
WINBASEAPI HGLOBAL WINAPI GlobalHandle(LPCVOID p) { return (HGLOBAL)p; }
WINBASEAPI UINT    WINAPI GlobalFlags(HGLOBAL h)  { (void)h; return 0; }
WINBASEAPI HGLOBAL WINAPI GlobalReAlloc(HGLOBAL h, SIZE_T n, UINT flags)
{
    if (flags & 0x80 /* GMEM_MODIFY */) return h;
    return HeapReAlloc(GetProcessHeap(), (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, h, n ? n : 1);
}
WINBASEAPI LPVOID  WINAPI LocalLock(HLOCAL h)     { return h; }
WINBASEAPI BOOL    WINAPI LocalUnlock(HLOCAL h)   { (void)h; SetLastError(0); return FALSE; }
WINBASEAPI SIZE_T  WINAPI LocalSize(HLOCAL h)     { return h ? HeapSize(GetProcessHeap(), 0, h) : 0; }
WINBASEAPI HLOCAL  WINAPI LocalHandle(LPCVOID p)  { return (HLOCAL)p; }
WINBASEAPI UINT    WINAPI LocalFlags(HLOCAL h)    { (void)h; return 0; }
WINBASEAPI HLOCAL  WINAPI LocalReAlloc(HLOCAL h, SIZE_T n, UINT flags)
{
    if (flags & 0x80 /* LMEM_MODIFY */) return h;
    return HeapReAlloc(GetProcessHeap(), (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, h, n ? n : 1);
}
WINBASEAPI VOID    WINAPI GlobalMemoryStatus(LPVOID p)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    DWORD *o = p;                                           /* MEMORYSTATUS: SIZE_Ts after two DWORDs */
    o[0] = 56; o[1] = ms.dwMemoryLoad;
    SIZE_T *s = (SIZE_T *)(o + 2);
    s[0] = (SIZE_T)ms.ullTotalPhys; s[1] = (SIZE_T)ms.ullAvailPhys; s[2] = (SIZE_T)ms.ullTotalPageFile;
    s[3] = (SIZE_T)ms.ullAvailPageFile; s[4] = (SIZE_T)ms.ullTotalVirtual; s[5] = (SIZE_T)ms.ullAvailVirtual;
}
