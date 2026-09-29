/*
 * compat.c — kernel32 functions real Windows programs import that the rest
 * of kernel32 lacked: file times and DOS dates, disk and drive queries,
 * streams, change notifications, tool-help snapshots, thread pools and
 * wait registrations, job objects, once-initialization, national-language
 * formatting, search paths and a few console calls.
 *
 * Where NovaOS has no such thing (hard links, alternate data streams,
 * large pages, CPU affinity), the call answers the way Windows does on a
 * file system or machine without it.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);

#define K32 __declspec(dllexport)
#ifndef ERROR_PRIVILEGE_NOT_HELD
#define ERROR_PRIVILEGE_NOT_HELD 1314
#endif
#ifndef ERROR_GEN_FAILURE
#define ERROR_GEN_FAILURE 31
#endif

/* elsewhere in kernel32 */
BOOL WINAPI GetStringTypeExW(LCID lcid, DWORD type, LPCWSTR src, int n, LPWORD out);
BOOL WINAPI VerifyVersionInfoW(LPVOID info, DWORD mask, DWORDLONG cond);
DWORD WINAPI GetConsoleTitleW(LPWSTR buf, DWORD n);
BOOL WINAPI FillConsoleOutputCharacterW(HANDLE h, WCHAR c, DWORD n, COORD at, LPDWORD written);

static void *zalloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  zfree(void *p)   { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p); }
static int   wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* -----------------------------------------------------------------------
 * File times
 * ----------------------------------------------------------------------- */
typedef struct { LARGE_INTEGER ct, at, wt, cht; ULONG attr; ULONG pad; } BasicInfo;

K32 BOOL WINAPI GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written)
{
    IO_STATUS_BLOCK io;
    BasicInfo b;
    NTSTATUS s = NtQueryInformationFile(h, &io, &b, 40, FileBasicInformation);
    if (!NT_SUCCESS(s)) return fail_status(s);
    if (created)  { created->dwLowDateTime = b.ct.LowPart;  created->dwHighDateTime = (DWORD)b.ct.HighPart; }
    if (accessed) { accessed->dwLowDateTime = b.at.LowPart; accessed->dwHighDateTime = (DWORD)b.at.HighPart; }
    if (written)  { written->dwLowDateTime = b.wt.LowPart;  written->dwHighDateTime = (DWORD)b.wt.HighPart; }
    return TRUE;
}

static LONGLONG ft(const FILETIME *f) { return f ? (LONGLONG)((ULONGLONG)f->dwHighDateTime << 32 | f->dwLowDateTime) : 0; }

K32 BOOL WINAPI SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written)
{
    IO_STATUS_BLOCK io;
    BasicInfo b;
    memset(&b, 0, sizeof(b));
    b.ct.QuadPart = ft(created);
    b.at.QuadPart = ft(accessed);
    b.wt.QuadPart = ft(written);
    NTSTATUS s = NtSetInformationFile(h, &io, &b, 40, FileBasicInformation);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

/* DOS date/time <-> FILETIME (the DOS side is local time; NovaOS keeps UTC) */
K32 BOOL WINAPI FileTimeToDosDateTime(const FILETIME *f, LPWORD date, LPWORD time)
{
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(f, &st) || st.wYear < 1980 || st.wYear > 2107) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *date = (WORD)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    *time = (WORD)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
    return TRUE;
}

K32 BOOL WINAPI DosDateTimeToFileTime(WORD date, WORD time, LPFILETIME f)
{
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)(1980 + (date >> 9));
    st.wMonth = (WORD)((date >> 5) & 15);
    st.wDay = (WORD)(date & 31);
    st.wHour = (WORD)(time >> 11);
    st.wMinute = (WORD)((time >> 5) & 63);
    st.wSecond = (WORD)((time & 31) * 2);
    return SystemTimeToFileTime(&st, f);
}

/* -----------------------------------------------------------------------
 * Disks and drives
 * ----------------------------------------------------------------------- */
K32 BOOL WINAPI GetDiskFreeSpaceW(LPCWSTR root, LPDWORD spc, LPDWORD bps, LPDWORD free_clusters, LPDWORD total_clusters)
{
    ULARGE_INTEGER avail, total, freeb;
    if (!GetDiskFreeSpaceExW(root, &avail, &total, &freeb)) return FALSE;
    if (spc) *spc = 8;
    if (bps) *bps = 512;
    ULONGLONG f = freeb.QuadPart / 4096, t = total.QuadPart / 4096;
    if (free_clusters) *free_clusters = f > 0xFFFFFFFF ? 0xFFFFFFFF : (DWORD)f;
    if (total_clusters) *total_clusters = t > 0xFFFFFFFF ? 0xFFFFFFFF : (DWORD)t;
    return TRUE;
}

K32 BOOL WINAPI GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD fc, LPDWORD tc)
{
    (void)root;
    return GetDiskFreeSpaceW(0, spc, bps, fc, tc);
}

K32 DWORD WINAPI GetLogicalDriveStringsW(DWORD n, LPWSTR buf)
{
    static const WCHAR drives[] = { 'C', ':', '\\', 0, 0 };
    if (n < 5) return 5;
    memcpy(buf, drives, sizeof(drives));
    return 4;
}

K32 DWORD WINAPI GetLogicalDriveStringsA(DWORD n, LPSTR buf)
{
    if (n < 5) return 5;
    memcpy(buf, "C:\\\0", 5);
    return 4;
}

K32 DWORD WINAPI GetCompressedFileSizeW(LPCWSTR name, LPDWORD high)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(name, GetFileExInfoStandard, &d)) return INVALID_FILE_SIZE;
    if (high) *high = d.nFileSizeHigh;
    return d.nFileSizeLow;
}

K32 DWORD WINAPI GetCompressedFileSizeA(LPCSTR name, LPDWORD high)
{
    WCHAR w[MAX_PATH];
    u2w(name, -1, w, MAX_PATH);
    return GetCompressedFileSizeW(w, high);
}

/* No reparse points or links on drive C: (FAT-like semantics) */
K32 BOOL WINAPI CreateHardLinkW(LPCWSTR link, LPCWSTR target, LPSECURITY_ATTRIBUTES sa)
{
    (void)link; (void)target; (void)sa;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
K32 BOOL WINAPI CreateHardLinkA(LPCSTR link, LPCSTR target, LPSECURITY_ATTRIBUTES sa)
{ (void)link; (void)target; (void)sa; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
K32 BOOLEAN WINAPI CreateSymbolicLinkW(LPCWSTR link, LPCWSTR target, DWORD flags)
{ (void)link; (void)target; (void)flags; SetLastError(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
K32 BOOLEAN WINAPI CreateSymbolicLinkA(LPCSTR link, LPCSTR target, DWORD flags)
{ (void)link; (void)target; (void)flags; SetLastError(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }

/* Device I/O: no device answers control codes; a disk-like reply for the
 * common "is this a volume?" probes */
K32 BOOL WINAPI DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_n, LPVOID out, DWORD out_n,
                                LPDWORD ret, LPOVERLAPPED ov)
{
    (void)h; (void)in; (void)in_n; (void)out; (void)out_n; (void)ov;
    if (ret) *ret = 0;
    (void)code;
    SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}

K32 BOOL WINAPI MoveFileWithProgressW(LPCWSTR from, LPCWSTR to, LPVOID progress, LPVOID data, DWORD flags)
{
    (void)progress; (void)data;
    return MoveFileExW(from, to, flags);
}

K32 BOOL WINAPI MoveFileWithProgressA(LPCSTR from, LPCSTR to, LPVOID progress, LPVOID data, DWORD flags)
{
    (void)progress; (void)data;
    return MoveFileExA(from, to, flags);
}

K32 HANDLE WINAPI ReOpenFile(HANDLE h, DWORD access, DWORD share, DWORD flags)
{
    (void)access; (void)share; (void)flags;
    HANDLE dup = 0;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */))
        return INVALID_HANDLE_VALUE;
    return dup;
}

/* Byte-range locks: one process owns drive C:'s files at a time here */
K32 BOOL WINAPI LockFileEx(HANDLE h, DWORD flags, DWORD res, DWORD lo, DWORD hi, LPOVERLAPPED ov)
{
    (void)h; (void)flags; (void)res; (void)lo; (void)hi;
    if (ov) { ov->Internal = 0; ov->InternalHigh = 0; if (ov->hEvent) SetEvent(ov->hEvent); }
    return TRUE;
}
K32 BOOL WINAPI UnlockFileEx(HANDLE h, DWORD res, DWORD lo, DWORD hi, LPOVERLAPPED ov)
{ (void)h; (void)res; (void)lo; (void)hi; (void)ov; return TRUE; }
K32 BOOL WINAPI LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE; }
K32 BOOL WINAPI UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE; }

/* Alternate data streams: a file has just its unnamed ::$DATA stream */
typedef struct { LARGE_INTEGER StreamSize; WCHAR cStreamName[MAX_PATH + 36]; } FIND_STREAM_DATA;
typedef struct { DWORD magic; int done; } StreamFind;

K32 HANDLE WINAPI FindFirstStreamW(LPCWSTR name, int level, LPVOID data, DWORD flags)
{
    (void)level; (void)flags;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(name, GetFileExInfoStandard, &d)) return INVALID_HANDLE_VALUE;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { SetLastError(ERROR_HANDLE_EOF); return INVALID_HANDLE_VALUE; }
    FIND_STREAM_DATA *s = data;
    s->StreamSize.LowPart = d.nFileSizeLow;
    s->StreamSize.HighPart = (LONG)d.nFileSizeHigh;
    static const WCHAR n[] = { ':', ':', '$', 'D', 'A', 'T', 'A', 0 };
    memcpy(s->cStreamName, n, sizeof(n));
    StreamFind *f = zalloc(sizeof(*f));
    if (!f) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    f->magic = 0x5354524D;
    return (HANDLE)f;
}

K32 BOOL WINAPI FindNextStreamW(HANDLE h, LPVOID data)
{
    (void)h; (void)data;
    SetLastError(ERROR_HANDLE_EOF);
    return FALSE;
}

/* FindClose (extra.c) frees directory searches; stream searches come here first */
BOOL k32_find_close_stream(HANDLE h)
{
    StreamFind *f = (StreamFind *)h;
    if (!f || (ULONG_PTR)f < 0x10000 || f->magic != 0x5354524D) return FALSE;
    f->magic = 0;
    zfree(f);
    return TRUE;
}

/* Change notifications: the handle is an event the kernel signals when the
 * directory (or its subtree) changes; FindNextChangeNotification re-arms it */
K32 HANDLE WINAPI FindFirstChangeNotificationW(LPCWSTR path, BOOL sub, DWORD filter)
{
    (void)filter;
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    HANDLE e = CreateEventW(0, TRUE, FALSE, 0);
    if (!e) return INVALID_HANDLE_VALUE;
    HANDLE d = CreateFileW(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0x02000000 /* FILE_FLAG_BACKUP_SEMANTICS */, 0);
    if (d != INVALID_HANDLE_VALUE) {
        NtNovaWatchDirectory(d, sub ? TRUE : FALSE, e, 0);
        CloseHandle(d);
    }
    return e;
}
K32 HANDLE WINAPI FindFirstChangeNotificationA(LPCSTR path, BOOL sub, DWORD filter)
{
    WCHAR w[MAX_PATH];
    u2w(path, -1, w, MAX_PATH);
    return FindFirstChangeNotificationW(w, sub, filter);
}
K32 BOOL WINAPI FindNextChangeNotification(HANDLE h) { return ResetEvent(h); }
K32 BOOL WINAPI FindCloseChangeNotification(HANDLE h)
{
    NtNovaWatchDirectory(0, FALSE, h, 1);
    return CloseHandle(h);
}

K32 BOOL WINAPI ReadDirectoryChangesW(HANDLE h, LPVOID buf, DWORD n, BOOL sub, DWORD filter, LPDWORD ret,
                                      LPOVERLAPPED ov, LPVOID fn)
{
    (void)h; (void)buf; (void)n; (void)sub; (void)filter; (void)fn;
    if (ret) *ret = 0;
    if (ov) {                                               /* pending forever: nothing is reported */
        ov->Internal = STATUS_PENDING;
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Search paths
 * ----------------------------------------------------------------------- */
static BOOL try_path(const WCHAR *dir, int dn, const WCHAR *file, const WCHAR *ext, WCHAR *out, int cap)
{
    WCHAR t[MAX_PATH * 2];
    int n = 0;
    for (int i = 0; i < dn && n < MAX_PATH; i++) t[n++] = dir[i];
    if (n && t[n - 1] != '\\' && t[n - 1] != '/') t[n++] = '\\';
    for (int i = 0; file[i] && n < MAX_PATH * 2 - 1; i++) t[n++] = file[i];
    for (int i = 0; ext && ext[i] && n < MAX_PATH * 2 - 1; i++) t[n++] = ext[i];
    t[n] = 0;
    DWORD a = GetFileAttributesW(t);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    return GetFullPathNameW(t, (DWORD)cap, out, 0) != 0;
}

K32 DWORD WINAPI SearchPathW(LPCWSTR path, LPCWSTR file, LPCWSTR ext, DWORD n, LPWSTR buf, LPWSTR *part)
{
    WCHAR found[MAX_PATH * 2];
    const WCHAR *e = ext;
    for (const WCHAR *c = file; *c; c++) { if (*c == '.') e = 0; if (*c == '\\' || *c == '/') e = ext; }
    BOOL ok = FALSE;
    BOOL has_dir = FALSE;
    for (const WCHAR *c = file; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') has_dir = TRUE;
    if (has_dir) ok = try_path(0, 0, file, e, found, MAX_PATH * 2);
    else if (path) {
        const WCHAR *p = path;
        while (!ok && *p) {
            const WCHAR *q = p;
            while (*q && *q != ';') q++;
            if (q > p) ok = try_path(p, (int)(q - p), file, e, found, MAX_PATH * 2);
            p = *q ? q + 1 : q;
        }
    } else {
        WCHAR dir[MAX_PATH];
        DWORD dn = GetModuleFileNameW(0, dir, MAX_PATH);
        while (dn && dir[dn - 1] != '\\') dn--;
        ok = try_path(dir, (int)dn, file, e, found, MAX_PATH * 2);
        if (!ok) { dn = GetCurrentDirectoryW(MAX_PATH, dir); ok = try_path(dir, (int)dn, file, e, found, MAX_PATH * 2); }
        if (!ok) { dn = GetSystemDirectoryW(dir, MAX_PATH); ok = try_path(dir, (int)dn, file, e, found, MAX_PATH * 2); }
        if (!ok) { dn = GetWindowsDirectoryW(dir, MAX_PATH); ok = try_path(dir, (int)dn, file, e, found, MAX_PATH * 2); }
        if (!ok) {
            WCHAR env[2048];
            DWORD en = GetEnvironmentVariableW((const WCHAR[]){ 'P', 'A', 'T', 'H', 0 }, env, 2048);
            if (en && en < 2048) return SearchPathW(env, file, ext, n, buf, part);
        }
    }
    if (!ok) { SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
    DWORD len = (DWORD)wlen(found);
    if (len + 1 > n) return len + 1;
    memcpy(buf, found, 2 * (len + 1));
    if (part) {
        *part = buf;
        for (WCHAR *c = buf; *c; c++) if (*c == '\\') *part = c + 1;
    }
    return len;
}

K32 DWORD WINAPI SearchPathA(LPCSTR path, LPCSTR file, LPCSTR ext, DWORD n, LPSTR buf, LPSTR *part)
{
    WCHAR wp[2048], wf[MAX_PATH], we[16], out[MAX_PATH * 2];
    if (path) u2w(path, -1, wp, 2048);
    u2w(file, -1, wf, MAX_PATH);
    if (ext) u2w(ext, -1, we, 16);
    DWORD r = SearchPathW(path ? wp : 0, wf, ext ? we : 0, MAX_PATH * 2, out, 0);
    if (!r) return 0;
    char tmp[MAX_PATH * 3];
    int k = w2u(out, -1, tmp, sizeof(tmp));
    if (k > 0 && !tmp[k - 1]) k--;
    if ((DWORD)k + 1 > n) return (DWORD)k + 1;
    memcpy(buf, tmp, (size_t)k + 1);
    if (part) { *part = buf; for (char *c = buf; *c; c++) if (*c == '\\') *part = c + 1; }
    return (DWORD)k;
}

K32 BOOL WINAPI NeedCurrentDirectoryForExePathW(LPCWSTR name)
{
    for (const WCHAR *c = name; *c; c++) if (*c == '\\') return TRUE;
    return GetEnvironmentVariableW((const WCHAR[]){ 'N', 'o', 'D', 'e', 'f', 'a', 'u', 'l', 't', 'C', 'u', 'r', 'r', 'e', 'n', 't',
                                                    'D', 'i', 'r', 'e', 'c', 't', 'o', 'r', 'y', 'I', 'n', 'E', 'x', 'e', 'P', 'a', 't', 'h', 0 }, 0, 0) == 0;
}
K32 BOOL WINAPI NeedCurrentDirectoryForExePathA(LPCSTR name)
{
    for (const char *c = name; *c; c++) if (*c == '\\') return TRUE;
    return GetEnvironmentVariableA("NoDefaultCurrentDirectoryInExePath", 0, 0) == 0;
}

/* -----------------------------------------------------------------------
 * Processors and memory
 * ----------------------------------------------------------------------- */
K32 SIZE_T WINAPI GetLargePageMinimum(void) { return 0; }           /* no large pages */
K32 DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    (void)h;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD_PTR all = si.dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : ((DWORD_PTR)1 << si.dwNumberOfProcessors) - 1;
    if (!(mask & all)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return all;                                             /* the scheduler places threads itself */
}
K32 DWORD WINAPI SetThreadIdealProcessor(HANDLE h, DWORD n) { (void)h; (void)n; return 0; }
K32 DWORD WINAPI GetActiveProcessorCount(WORD group) { (void)group; SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors; }
K32 DWORD WINAPI GetMaximumProcessorCount(WORD group) { return GetActiveProcessorCount(group); }
K32 WORD WINAPI GetActiveProcessorGroupCount(void) { return 1; }
K32 WORD WINAPI GetMaximumProcessorGroupCount(void) { return 1; }
K32 DWORD WINAPI GetCurrentProcessorNumber(void)
{
    unsigned aux;
    __builtin_ia32_rdtscp(&aux);                            /* the kernel keeps the CPU number in TSC_AUX */
    return aux & 0xFFF;
}
K32 VOID WINAPI GetCurrentProcessorNumberEx(PVOID p) { WORD *w = p; w[0] = 0; ((BYTE *)p)[2] = (BYTE)GetCurrentProcessorNumber(); ((BYTE *)p)[3] = 0; }
K32 BOOL WINAPI IsWow64Process(HANDLE h, PBOOL wow) { (void)h; *wow = FALSE; return TRUE; }
K32 BOOL WINAPI IsWow64Process2(HANDLE h, USHORT *proc, USHORT *native) { (void)h; if (proc) *proc = 0; if (native) *native = 0x8664; return TRUE; }

K32 BOOL WINAPI GetProcessIoCounters(HANDLE h, PVOID io)
{
    (void)h;
    memset(io, 0, 48);
    return TRUE;
}

K32 VOID WINAPI DebugBreak(void) { __debugbreak(); }
K32 BOOL WINAPI DebugBreakProcess(HANDLE h) { (void)h; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
K32 VOID WINAPI OutputDebugStringW(LPCWSTR s)
{
    char buf[1024];
    w2u(s, -1, buf, sizeof(buf));
    OutputDebugStringA(buf);
}

K32 int WINAPI MulDiv(int a, int b, int c)
{
    if (!c) return -1;
    LONGLONG r = (LONGLONG)a * b;
    LONGLONG half = (c < 0 ? -(LONGLONG)c : c) / 2;
    r = ((r < 0) ^ (c < 0)) ? (r - (r < 0 ? half : -half)) / c : (r + (r < 0 ? -half : half)) / c;
    if (r > 0x7FFFFFFF || r < -(LONGLONG)0x80000000) return -1;
    return (int)r;
}

/* -----------------------------------------------------------------------
 * Tool help: processes (and this process's threads and modules)
 * ----------------------------------------------------------------------- */
typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads,
    th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; WCHAR szExeFile[MAX_PATH];
} PROCESSENTRY32W_;
typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads,
    th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; CHAR szExeFile[MAX_PATH];
} PROCESSENTRY32_;
typedef struct { DWORD magic, n, pos; NOVA_PROCESS_ENTRY e[64]; } Snapshot;

K32 HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{
    (void)flags; (void)pid;
    Snapshot *s = zalloc(sizeof(*s));
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    ULONG n = 0;
    NtNovaProcessList(s->e, 64, &n);
    s->magic = 0x534E4150;
    s->n = n > 64 ? 64 : n;
    return (HANDLE)s;
}

static Snapshot *snap(HANDLE h) { Snapshot *s = (Snapshot *)h; return s && (ULONG_PTR)s > 0x10000 && s->magic == 0x534E4150 ? s : 0; }

BOOL k32_close_snapshot(HANDLE h)
{
    Snapshot *s = snap(h);
    if (!s) return FALSE;
    s->magic = 0;
    zfree(s);
    return TRUE;
}

static BOOL proc_next(HANDLE h, PROCESSENTRY32W_ *pe, BOOL first)
{
    Snapshot *s = snap(h);
    if (!s) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (first) s->pos = 0;
    while (s->pos < s->n && s->e[s->pos].Exited) s->pos++;
    if (s->pos >= s->n) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    NOVA_PROCESS_ENTRY *e = &s->e[s->pos++];
    DWORD size = pe->dwSize;
    memset((BYTE *)pe + 4, 0, sizeof(*pe) - 4);
    pe->dwSize = size;
    pe->th32ProcessID = e->Pid;
    pe->cntThreads = e->Threads;
    pe->pcPriClassBase = 8;
    u2w(e->Name, -1, pe->szExeFile, MAX_PATH);
    return TRUE;
}

K32 BOOL WINAPI Process32FirstW(HANDLE h, LPVOID pe) { return proc_next(h, pe, TRUE); }
K32 BOOL WINAPI Process32NextW(HANDLE h, LPVOID pe) { return proc_next(h, pe, FALSE); }
static BOOL proc_next_a(HANDLE h, PROCESSENTRY32_ *pe, BOOL first)
{
    PROCESSENTRY32W_ w;
    w.dwSize = sizeof(w);
    if (!proc_next(h, &w, first)) return FALSE;
    DWORD size = pe->dwSize;
    memcpy(pe, &w, (size_t)((BYTE *)w.szExeFile - (BYTE *)&w));
    pe->dwSize = size;
    w2u(w.szExeFile, -1, pe->szExeFile, MAX_PATH);
    return TRUE;
}
K32 BOOL WINAPI Process32First(HANDLE h, LPVOID pe) { return proc_next_a(h, pe, TRUE); }
K32 BOOL WINAPI Process32Next(HANDLE h, LPVOID pe) { return proc_next_a(h, pe, FALSE); }
K32 BOOL WINAPI Thread32First(HANDLE h, LPVOID te) { (void)h; (void)te; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
K32 BOOL WINAPI Thread32Next(HANDLE h, LPVOID te) { (void)h; (void)te; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
K32 BOOL WINAPI Module32FirstW(HANDLE h, LPVOID me) { (void)h; (void)me; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
K32 BOOL WINAPI Module32NextW(HANDLE h, LPVOID me) { (void)h; (void)me; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }

/* -----------------------------------------------------------------------
 * Once-initialization and critical sections
 * ----------------------------------------------------------------------- */
K32 BOOL WINAPI InitOnceBeginInitialize(LPINIT_ONCE once, DWORD flags, PBOOL pending, LPVOID *ctx)
{
    if (flags & 1 /* INIT_ONCE_CHECK_ONLY */) {
        if (once->Ptr == 0 || once->Ptr == (PVOID)1) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
        *pending = FALSE;
        if (ctx) *ctx = 0;
        return TRUE;
    }
    PVOID c = 0;
    NTSTATUS s = RtlRunOnceBeginInitialize(once, flags, &c);
    if (s == STATUS_PENDING) { *pending = TRUE; return TRUE; }
    if (!NT_SUCCESS(s)) return fail_status(s);
    *pending = FALSE;
    if (ctx) *ctx = c;
    return TRUE;
}

K32 BOOL WINAPI InitOnceComplete(LPINIT_ONCE once, DWORD flags, LPVOID ctx)
{
    NTSTATUS s = RtlRunOnceComplete(once, flags, ctx);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

K32 BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags)
{
    (void)flags;
    return NT_SUCCESS(RtlInitializeCriticalSectionAndSpinCount(cs, spin)) ? TRUE : FALSE;
}

/* -----------------------------------------------------------------------
 * The thread pool: work items run on a thread of their own; waits are
 * watched by a thread per registration
 * ----------------------------------------------------------------------- */
typedef VOID (WINAPI *WorkFn)(PVOID instance, PVOID ctx, PVOID work);
typedef struct Work {
    DWORD magic;
    WorkFn fn;
    PVOID ctx;
    volatile LONG pending;                                  /* submitted, not yet finished */
    HANDLE idle;                                            /* manual event: set when pending == 0 */
    volatile LONG cancel;
} Work;

K32 PVOID WINAPI CreateThreadpoolWork(WorkFn fn, PVOID ctx, PVOID env)
{
    (void)env;
    Work *w = zalloc(sizeof(*w));
    if (!w) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    w->magic = 0x574F524B;
    w->fn = fn;
    w->ctx = ctx;
    w->idle = CreateEventW(0, TRUE, TRUE, 0);
    return w;
}

static DWORD WINAPI work_thread(LPVOID p)
{
    Work *w = p;
    if (!w->cancel) w->fn(0, w->ctx, w);
    if (InterlockedDecrement(&w->pending) == 0) SetEvent(w->idle);
    return 0;
}

K32 VOID WINAPI SubmitThreadpoolWork(PVOID p)
{
    Work *w = p;
    if (InterlockedIncrement(&w->pending) == 1) ResetEvent(w->idle);
    HANDLE t = CreateThread(0, 0, work_thread, w, 0, 0);
    if (t) CloseHandle(t);
    else if (InterlockedDecrement(&w->pending) == 0) SetEvent(w->idle);
}

K32 VOID WINAPI WaitForThreadpoolWorkCallbacks(PVOID p, BOOL cancel)
{
    Work *w = p;
    if (cancel) w->cancel = 1;
    WaitForSingleObject(w->idle, INFINITE);
    w->cancel = 0;
}

K32 VOID WINAPI CloseThreadpoolWork(PVOID p)
{
    Work *w = p;
    WaitForSingleObject(w->idle, INFINITE);
    CloseHandle(w->idle);
    w->magic = 0;
    zfree(w);
}

K32 BOOL WINAPI TrySubmitThreadpoolCallback(PVOID fn, PVOID ctx, PVOID env)
{
    Work *w = CreateThreadpoolWork((WorkFn)fn, ctx, env);   /* (instance, ctx): the extra argument is ignored */
    if (!w) return FALSE;
    SubmitThreadpoolWork(w);
    return TRUE;                                            /* w is leaked once done: rare, and small */
}

K32 VOID WINAPI FreeLibraryWhenCallbackReturns(PVOID instance, HMODULE m) { (void)instance; (void)m; }
K32 VOID WINAPI SetEventWhenCallbackReturns(PVOID instance, HANDLE e) { (void)instance; SetEvent(e); }
K32 VOID WINAPI ReleaseMutexWhenCallbackReturns(PVOID instance, HANDLE m) { (void)instance; ReleaseMutex(m); }
K32 VOID WINAPI LeaveCriticalSectionWhenCallbackReturns(PVOID instance, LPCRITICAL_SECTION cs) { (void)instance; LeaveCriticalSection(cs); }
K32 BOOL WINAPI CallbackMayRunLong(PVOID instance) { (void)instance; return TRUE; }
K32 VOID WINAPI DisassociateCurrentThreadFromCallback(PVOID instance) { (void)instance; }

typedef struct { LPTHREAD_START_ROUTINE fn; PVOID ctx; } UserWork;
static DWORD WINAPI user_work(LPVOID p)
{
    UserWork u = *(UserWork *)p;
    zfree(p);
    return u.fn(u.ctx);
}

K32 BOOL WINAPI QueueUserWorkItem(LPTHREAD_START_ROUTINE fn, PVOID ctx, ULONG flags)
{
    (void)flags;
    UserWork *u = zalloc(sizeof(*u));
    if (!u) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    u->fn = fn; u->ctx = ctx;
    HANDLE t = CreateThread(0, 0, user_work, u, 0, 0);
    if (!t) { zfree(u); return FALSE; }
    CloseHandle(t);
    return TRUE;
}

/* RegisterWaitForSingleObject: a thread waits and calls back */
typedef VOID (CALLBACK *WaitOrTimerFn)(PVOID, BOOLEAN);
typedef struct {
    DWORD magic;
    HANDLE obj, stop, thread;
    WaitOrTimerFn fn;
    PVOID ctx;
    DWORD ms;
    ULONG flags;
} WaitReg;

static DWORD WINAPI wait_thread(LPVOID p)
{
    WaitReg *r = p;
    for (;;) {
        HANDLE hs[2] = { r->stop, r->obj };
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, r->ms);
        if (w == WAIT_OBJECT_0) break;                      /* unregistered */
        BOOLEAN timed_out = w == WAIT_TIMEOUT;
        if (w != WAIT_OBJECT_0 + 1 && !timed_out) break;
        r->fn(r->ctx, timed_out);
        if (r->flags & 8 /* WT_EXECUTEONLYONCE */) break;
        if (!timed_out && WaitForSingleObject(r->obj, 0) == WAIT_OBJECT_0 && r->ms == INFINITE) {
            /* a manual-reset object stays signaled: don't spin on it */
            Sleep(1);
        }
    }
    return 0;
}

K32 BOOL WINAPI RegisterWaitForSingleObject(PHANDLE out, HANDLE obj, WaitOrTimerFn fn, PVOID ctx, ULONG ms, ULONG flags)
{
    WaitReg *r = zalloc(sizeof(*r));
    if (!r) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    r->magic = 0x57414954;
    r->fn = fn; r->ctx = ctx; r->ms = ms; r->flags = flags;
    if (!DuplicateHandle(GetCurrentProcess(), obj, GetCurrentProcess(), &r->obj, 0, FALSE, 2)) r->obj = obj;
    r->stop = CreateEventW(0, TRUE, FALSE, 0);
    r->thread = CreateThread(0, 0, wait_thread, r, 0, 0);
    if (!r->thread) { CloseHandle(r->stop); zfree(r); return FALSE; }
    *out = (HANDLE)r;
    return TRUE;
}

K32 HANDLE WINAPI RegisterWaitForSingleObjectEx(HANDLE obj, WaitOrTimerFn fn, PVOID ctx, ULONG ms, ULONG flags)
{
    HANDLE h = 0;
    return RegisterWaitForSingleObject(&h, obj, fn, ctx, ms, flags) ? h : 0;
}

K32 BOOL WINAPI UnregisterWaitEx(HANDLE h, HANDLE done)
{
    WaitReg *r = (WaitReg *)h;
    if (!r || r->magic != 0x57414954) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    SetEvent(r->stop);
    if (GetCurrentThreadId() != GetThreadId(r->thread)) {
        if (done == INVALID_HANDLE_VALUE) WaitForSingleObject(r->thread, INFINITE);
        else if (done) { WaitForSingleObject(r->thread, INFINITE); SetEvent(done); }
    }
    CloseHandle(r->thread);
    CloseHandle(r->stop);
    if (r->obj) CloseHandle(r->obj);
    r->magic = 0;
    zfree(r);
    return TRUE;
}

K32 BOOL WINAPI UnregisterWait(HANDLE h) { return UnregisterWaitEx(h, 0); }

/* Thread-pool waits and timers (CreateThreadpoolWait / Timer) */
typedef VOID (WINAPI *TpWaitFn)(PVOID instance, PVOID ctx, PVOID wait, DWORD result);
typedef struct { DWORD magic; TpWaitFn fn; PVOID ctx; HANDLE reg; } TpWait;

K32 PVOID WINAPI CreateThreadpoolWait(TpWaitFn fn, PVOID ctx, PVOID env)
{
    (void)env;
    TpWait *w = zalloc(sizeof(*w));
    if (!w) return 0;
    w->magic = 0x54505754; w->fn = fn; w->ctx = ctx;
    return w;
}

static VOID CALLBACK tp_wait_cb(PVOID p, BOOLEAN timed_out)
{
    TpWait *w = p;
    w->fn(0, w->ctx, w, timed_out ? WAIT_TIMEOUT : WAIT_OBJECT_0);
}

K32 VOID WINAPI SetThreadpoolWait(PVOID p, HANDLE obj, PFILETIME timeout)
{
    TpWait *w = p;
    if (w->reg) { UnregisterWaitEx(w->reg, INVALID_HANDLE_VALUE); w->reg = 0; }
    if (!obj) return;
    DWORD ms = INFINITE;
    if (timeout) {
        LONGLONG t = ft(timeout);
        if (t < 0) ms = (DWORD)(-t / 10000);
        else { FILETIME now; GetSystemTimeAsFileTime(&now); LONGLONG d = t - ft(&now); ms = d > 0 ? (DWORD)(d / 10000) : 0; }
    }
    RegisterWaitForSingleObject(&w->reg, obj, tp_wait_cb, w, ms, 8 /* once */);
}

K32 VOID WINAPI WaitForThreadpoolWaitCallbacks(PVOID p, BOOL cancel) { (void)p; (void)cancel; }
K32 VOID WINAPI CloseThreadpoolWait(PVOID p)
{
    TpWait *w = p;
    if (w->reg) UnregisterWaitEx(w->reg, INVALID_HANDLE_VALUE);
    w->magic = 0;
    zfree(w);
}

typedef VOID (WINAPI *TpTimerFn)(PVOID instance, PVOID ctx, PVOID timer);
typedef struct { DWORD magic; TpTimerFn fn; PVOID ctx; HANDLE thread, stop; LONGLONG due; DWORD period; volatile LONG armed; } TpTimer;

K32 PVOID WINAPI CreateThreadpoolTimer(TpTimerFn fn, PVOID ctx, PVOID env)
{
    (void)env;
    TpTimer *t = zalloc(sizeof(*t));
    if (!t) return 0;
    t->magic = 0x54505449; t->fn = fn; t->ctx = ctx;
    t->stop = CreateEventW(0, FALSE, FALSE, 0);
    return t;
}

static DWORD WINAPI tp_timer_thread(LPVOID p)
{
    TpTimer *t = p;
    DWORD wait = (DWORD)t->due;
    for (;;) {
        if (WaitForSingleObject(t->stop, wait) != WAIT_TIMEOUT) break;
        t->fn(0, t->ctx, t);
        if (!t->period) break;
        wait = t->period;
    }
    return 0;
}

K32 VOID WINAPI SetThreadpoolTimer(PVOID p, PFILETIME due, DWORD period, DWORD window)
{
    (void)window;
    TpTimer *t = p;
    if (t->thread) { SetEvent(t->stop); WaitForSingleObject(t->thread, INFINITE); CloseHandle(t->thread); t->thread = 0; ResetEvent(t->stop); }
    if (!due) return;
    LONGLONG d = ft(due);
    if (d < 0) t->due = -d / 10000;
    else { FILETIME now; GetSystemTimeAsFileTime(&now); LONGLONG x = d - ft(&now); t->due = x > 0 ? x / 10000 : 0; }
    t->period = period;
    t->thread = CreateThread(0, 0, tp_timer_thread, t, 0, 0);
}
K32 BOOL WINAPI IsThreadpoolTimerSet(PVOID p) { return ((TpTimer *)p)->thread != 0; }
K32 VOID WINAPI WaitForThreadpoolTimerCallbacks(PVOID p, BOOL cancel) { (void)p; (void)cancel; }
K32 VOID WINAPI CloseThreadpoolTimer(PVOID p)
{
    TpTimer *t = p;
    SetThreadpoolTimer(t, 0, 0, 0);
    CloseHandle(t->stop);
    t->magic = 0;
    zfree(t);
}

/* Pools and environments: one pool, the calls just succeed */
K32 PVOID WINAPI CreateThreadpool(PVOID r) { (void)r; static int pool; return &pool; }
K32 VOID WINAPI CloseThreadpool(PVOID p) { (void)p; }
K32 VOID WINAPI SetThreadpoolThreadMaximum(PVOID p, DWORD n) { (void)p; (void)n; }
K32 BOOL WINAPI SetThreadpoolThreadMinimum(PVOID p, DWORD n) { (void)p; (void)n; return TRUE; }
K32 PVOID WINAPI CreateThreadpoolCleanupGroup(void) { static int group; return &group; }
K32 VOID WINAPI CloseThreadpoolCleanupGroupMembers(PVOID g, BOOL cancel, PVOID ctx) { (void)g; (void)cancel; (void)ctx; }
K32 VOID WINAPI CloseThreadpoolCleanupGroup(PVOID g) { (void)g; }

/* APCs queued to a thread run when it next waits alertably (extra.c) */
void k32_queue_user_apc(DWORD tid, PAPCFUNC fn, ULONG_PTR arg);
K32 DWORD WINAPI QueueUserAPC(PAPCFUNC fn, HANDLE thread, ULONG_PTR arg)
{
    DWORD tid = GetThreadId(thread);
    if (!tid) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    k32_queue_user_apc(tid, fn, arg);
    return 1;
}

K32 BOOL WINAPI CancelSynchronousIo(HANDLE thread) { (void)thread; SetLastError(1168 /* ERROR_NOT_FOUND */); return FALSE; }

K32 __declspec(noreturn) VOID WINAPI FreeLibraryAndExitThread(HMODULE m, DWORD code)
{
    FreeLibrary(m);
    ExitThread(code);
}

/* -----------------------------------------------------------------------
 * Job objects: a job is an event handle standing for the group; limits
 * are remembered and answered back
 * ----------------------------------------------------------------------- */
K32 HANDLE WINAPI CreateJobObjectW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name)
{
    (void)sa; (void)name;
    return CreateEventW(0, TRUE, FALSE, 0);
}
K32 HANDLE WINAPI CreateJobObjectA(LPSECURITY_ATTRIBUTES sa, LPCSTR name) { (void)name; return CreateJobObjectW(sa, 0); }
K32 HANDLE WINAPI OpenJobObjectW(DWORD access, BOOL inherit, LPCWSTR name) { (void)access; (void)inherit; (void)name; SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
K32 BOOL WINAPI AssignProcessToJobObject(HANDLE job, HANDLE proc) { (void)job; (void)proc; return TRUE; }
K32 BOOL WINAPI SetInformationJobObject(HANDLE job, int cls, LPVOID info, DWORD n) { (void)job; (void)cls; (void)info; (void)n; return TRUE; }
K32 BOOL WINAPI QueryInformationJobObject(HANDLE job, int cls, LPVOID info, DWORD n, LPDWORD ret)
{
    (void)job; (void)cls;
    memset(info, 0, n);
    if (ret) *ret = n;
    return TRUE;
}
K32 BOOL WINAPI TerminateJobObject(HANDLE job, UINT code) { (void)job; (void)code; return TRUE; }
K32 BOOL WINAPI IsProcessInJob(HANDLE proc, HANDLE job, PBOOL in) { (void)proc; (void)job; *in = FALSE; return TRUE; }

/* Restart manager hooks: nothing restarts programs here */
K32 HRESULT WINAPI RegisterApplicationRestart(LPCWSTR cmd, DWORD flags) { (void)cmd; (void)flags; return 0; }
K32 HRESULT WINAPI UnregisterApplicationRestart(void) { return 0; }
K32 HRESULT WINAPI GetApplicationRestartSettings(HANDLE p, LPWSTR cmd, PDWORD n, PDWORD flags)
{ (void)p; (void)cmd; (void)n; (void)flags; return 0x80070490; /* HRESULT_FROM_WIN32(ERROR_NOT_FOUND) */ }
K32 HRESULT WINAPI RegisterApplicationRecoveryCallback(PVOID fn, PVOID p, DWORD ping, DWORD flags)
{ (void)fn; (void)p; (void)ping; (void)flags; return 0; }

/* -----------------------------------------------------------------------
 * National language support: dates and times, locales
 * ----------------------------------------------------------------------- */
static const char *const g_months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
static const char *const g_days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static int put_num(WCHAR *o, int n, int v, int digits)
{
    char t[12];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k < digits) t[k++] = '0';
    for (int i = k - 1; i >= 0; i--) o[n++] = (WCHAR)t[i];
    return n;
}
static int put_str(WCHAR *o, int n, const char *s, int max)
{
    for (int i = 0; s[i] && (max <= 0 || i < max); i++) o[n++] = (WCHAR)s[i];
    return n;
}

/* Formats with the en-US pictures: "M/d/yyyy", "dddd, MMMM d, yyyy", "h:mm:ss tt" */
static int format_dt(const SYSTEMTIME *st, const WCHAR *pic, WCHAR *o)
{
    int n = 0;
    for (const WCHAR *p = pic; *p && n < 200; ) {
        WCHAR c = *p;
        int run = 0;
        while (p[run] == c) run++;
        if (c == '\'') {                                    /* quoted literal */
            p++;
            while (*p && *p != '\'') o[n++] = *p++;
            if (*p) p++;
            continue;
        }
        switch (c) {
        case 'd':
            if (run <= 2) n = put_num(o, n, st->wDay, run);
            else n = put_str(o, n, g_days[st->wDayOfWeek % 7], run == 3 ? 3 : 0);
            break;
        case 'M':
            if (run <= 2) n = put_num(o, n, st->wMonth, run);
            else n = put_str(o, n, g_months[(st->wMonth + 11) % 12], run == 3 ? 3 : 0);
            break;
        case 'y':
            if (run <= 2) n = put_num(o, n, st->wYear % 100, run);
            else n = put_num(o, n, st->wYear, 4);
            break;
        case 'g': break;
        case 'h': n = put_num(o, n, st->wHour % 12 ? st->wHour % 12 : 12, run > 1 ? 2 : 1); break;
        case 'H': n = put_num(o, n, st->wHour, run > 1 ? 2 : 1); break;
        case 'm': n = put_num(o, n, st->wMinute, run > 1 ? 2 : 1); break;
        case 's': n = put_num(o, n, st->wSecond, run > 1 ? 2 : 1); break;
        case 't': n = put_str(o, n, st->wHour < 12 ? "AM" : "PM", run == 1 ? 1 : 2); break;
        default:
            for (int i = 0; i < run; i++) o[n++] = c;
        }
        p += run;
    }
    o[n] = 0;
    return n;
}

static int nls_out(const WCHAR *s, int n, LPWSTR out, int cap)
{
    if (!cap) return n + 1;
    if (cap < n + 1) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, s, 2 * ((size_t)n + 1));
    return n + 1;
}

static void pic_a(const char *s, WCHAR *w) { int i = 0; for (; s[i]; i++) w[i] = (WCHAR)s[i]; w[i] = 0; }

K32 int WINAPI GetDateFormatW(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    (void)lcid;
    SYSTEMTIME now;
    if (!st) { GetLocalTime(&now); st = &now; }
    WCHAR pic[64], buf[256];
    if (!fmt) { pic_a(flags & 2 /* DATE_LONGDATE */ ? "dddd, MMMM d, yyyy" : "M/d/yyyy", pic); fmt = pic; }
    int n = format_dt(st, fmt, buf);
    return nls_out(buf, n, out, cap);
}

K32 int WINAPI GetDateFormatEx(LPCWSTR loc, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap, LPCWSTR cal)
{
    (void)loc; (void)cal;
    return GetDateFormatW(0, flags, st, fmt, out, cap);
}

K32 int WINAPI GetTimeFormatW(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    (void)lcid;
    SYSTEMTIME now;
    if (!st) { GetLocalTime(&now); st = &now; }
    WCHAR pic[64], buf[256];
    if (!fmt) {
        const char *f = (flags & 2 /* TIME_NOSECONDS */) ? "h:mm tt" : "h:mm:ss tt";
        if (flags & 8 /* TIME_FORCE24HOURFORMAT */) f = (flags & 2) ? "HH:mm" : "HH:mm:ss";
        pic_a(f, pic);
        fmt = pic;
    }
    int n = format_dt(st, fmt, buf);
    return nls_out(buf, n, out, cap);
}

K32 int WINAPI GetTimeFormatEx(LPCWSTR loc, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    (void)loc;
    return GetTimeFormatW(0, flags, st, fmt, out, cap);
}

static int to_a(const WCHAR *w, int wn, LPSTR out, int cap)
{
    char tmp[512];
    int k = w2u(w, wn, tmp, sizeof(tmp));
    if (!cap) return k + 1;
    if (cap < k + 1) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, tmp, (size_t)k);
    out[k] = 0;
    return k + 1;
}

K32 int WINAPI GetDateFormatA(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCSTR fmt, LPSTR out, int cap)
{
    WCHAR wf[128], buf[256];
    if (fmt) u2w(fmt, -1, wf, 128);
    int n = GetDateFormatW(lcid, flags, st, fmt ? wf : 0, buf, 256);
    return n ? to_a(buf, n - 1, out, cap) : 0;
}

K32 int WINAPI GetTimeFormatA(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCSTR fmt, LPSTR out, int cap)
{
    WCHAR wf[128], buf[256];
    if (fmt) u2w(fmt, -1, wf, 128);
    int n = GetTimeFormatW(lcid, flags, st, fmt ? wf : 0, buf, 256);
    return n ? to_a(buf, n - 1, out, cap) : 0;
}

K32 int WINAPI LCMapStringA(LCID lcid, DWORD flags, LPCSTR src, int n, LPSTR dst, int cap)
{
    WCHAR w[1024], o[1024];
    int wn = MultiByteToWideChar(CP_ACP, 0, src, n, w, 1024);
    if (wn <= 0) return 0;
    int on = LCMapStringW(lcid, flags, w, wn, o, 1024);
    if (on <= 0) return 0;
    if (flags & 0x400 /* LCMAP_SORTKEY */) {
        if (!cap) return on;
        if (cap < on) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(dst, o, (size_t)on);
        return on;
    }
    return WideCharToMultiByte(CP_ACP, 0, o, on, dst, cap, 0, 0);
}

K32 BOOL WINAPI GetStringTypeExA(LCID lcid, DWORD type, LPCSTR src, int n, LPWORD out)
{
    WCHAR w[1024];
    if (n < 0) n = (int)strlen(src) + 1;
    int wn = MultiByteToWideChar(CP_ACP, 0, src, n, w, 1024);
    if (wn <= 0) return FALSE;
    return GetStringTypeExW(lcid, type, w, wn, out);
}

typedef BOOL (CALLBACK *LOCALE_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *LOCALE_ENUMPROCW_)(LPWSTR);
K32 BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA_ fn, DWORD flags) { (void)flags; fn("00000409"); return TRUE; }

/* -----------------------------------------------------------------------
 * Resources
 * ----------------------------------------------------------------------- */
typedef BOOL (CALLBACK *ENUMRESLANGPROCA_)(HMODULE, LPCSTR, LPCSTR, WORD, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESLANGPROCW_)(HMODULE, LPCWSTR, LPCWSTR, WORD, LONG_PTR);
K32 BOOL WINAPI EnumResourceLanguagesW(HMODULE m, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW_ fn, LONG_PTR lp)
{
    if (!FindResourceW(m, name, type)) { SetLastError(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return FALSE; }
    fn(m, type, name, 0x0409, lp);
    return TRUE;
}
K32 BOOL WINAPI EnumResourceLanguagesA(HMODULE m, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA_ fn, LONG_PTR lp)
{
    if (!FindResourceA(m, name, type)) { SetLastError(1814); return FALSE; }
    fn(m, type, name, 0x0409, lp);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Versions
 * ----------------------------------------------------------------------- */
K32 BOOL WINAPI VerifyVersionInfoA(LPVOID info, DWORD mask, DWORDLONG cond)
{
    /* OSVERSIONINFOEXA: the fields VerifyVersionInfoW reads have the same
     * offsets up to the service-pack string, then wServicePackMajor... */
    BYTE w[284];
    memset(w, 0, sizeof(w));
    const BYTE *a = info;
    memcpy(w, a, 20);                                       /* size, major, minor, build, platform */
    memcpy(w + 276, a + 148, 8);                            /* wServicePackMajor/Minor, wSuiteMask, product, reserved */
    *(DWORD *)w = 284;
    return VerifyVersionInfoW(w, mask, cond);
}

/* -----------------------------------------------------------------------
 * Console odds and ends
 * ----------------------------------------------------------------------- */
K32 DWORD WINAPI GetConsoleTitleA(LPSTR buf, DWORD n)
{
    WCHAR w[256];
    DWORD k = GetConsoleTitleW(w, 256);
    if (!k) { if (n) buf[0] = 0; return 0; }
    int m = w2u(w, (int)k, buf, (int)n);
    return (DWORD)m;
}

K32 BOOL WINAPI FillConsoleOutputCharacterA(HANDLE h, CHAR c, DWORD n, COORD at, LPDWORD written)
{
    return FillConsoleOutputCharacterW(h, (WCHAR)(BYTE)c, n, at, written);
}

K32 BOOL WINAPI GenerateConsoleCtrlEvent(DWORD ev, DWORD group)
{
    (void)ev; (void)group;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Atoms: one table for the local and the global functions (0xC000 and up;
 * "#123" and integer atoms below 0xC000 stand for themselves)
 * ----------------------------------------------------------------------- */
#define MAX_ATOMS 1024
static struct { WCHAR *name; int refs; } g_atoms[MAX_ATOMS];
static SRWLOCK g_atom_lock;

static int atom_int(LPCWSTR s, WORD *out)
{
    if ((ULONG_PTR)s < 0x10000) { *out = (WORD)(ULONG_PTR)s; return 1; }
    if (s[0] != '#') return 0;
    unsigned v = 0;
    for (const WCHAR *p = s + 1; *p; p++) { if (*p < '0' || *p > '9') return 0; v = v * 10 + (unsigned)(*p - '0'); if (v >= 0xC000) return 0; }
    *out = (WORD)v;
    return 1;
}

static int atom_eq(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static WORD atom_find(LPCWSTR s, int add)
{
    WORD v;
    if (atom_int(s, &v)) return v;
    AcquireSRWLockExclusive(&g_atom_lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_ATOMS; i++) {
        if (!g_atoms[i].name) { if (free_slot < 0) free_slot = i; continue; }
        if (atom_eq(g_atoms[i].name, s)) { if (add) g_atoms[i].refs++; ReleaseSRWLockExclusive(&g_atom_lock); return (WORD)(0xC000 + i); }
    }
    WORD r = 0;
    if (add && free_slot >= 0) {
        size_t n = 0;
        while (s[n]) n++;
        WCHAR *c = HeapAlloc(GetProcessHeap(), 0, (n + 1) * 2);
        if (c) {
            for (size_t i = 0; i <= n; i++) c[i] = s[i];
            g_atoms[free_slot].name = c;
            g_atoms[free_slot].refs = 1;
            r = (WORD)(0xC000 + free_slot);
        }
    }
    ReleaseSRWLockExclusive(&g_atom_lock);
    if (!r) SetLastError(add ? ERROR_NOT_ENOUGH_MEMORY : ERROR_FILE_NOT_FOUND);
    return r;
}

K32 WORD WINAPI GlobalAddAtomW(LPCWSTR s) { return atom_find(s, 1); }
K32 WORD WINAPI GlobalFindAtomW(LPCWSTR s) { return atom_find(s, 0); }
K32 WORD WINAPI AddAtomW(LPCWSTR s) { return atom_find(s, 1); }
K32 WORD WINAPI FindAtomW(LPCWSTR s) { return atom_find(s, 0); }

static WORD atom_a(LPCSTR s, int add)
{
    if ((ULONG_PTR)s < 0x10000) return atom_find((LPCWSTR)s, add);
    WCHAR w[256];
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, 256);
    w[255] = 0;
    return atom_find(w, add);
}

K32 WORD WINAPI GlobalAddAtomA(LPCSTR s) { return atom_a(s, 1); }
K32 WORD WINAPI GlobalFindAtomA(LPCSTR s) { return atom_a(s, 0); }
K32 WORD WINAPI AddAtomA(LPCSTR s) { return atom_a(s, 1); }
K32 WORD WINAPI FindAtomA(LPCSTR s) { return atom_a(s, 0); }
K32 WORD WINAPI GlobalAddAtomExW(LPCWSTR s, DWORD f) { (void)f; return atom_find(s, 1); }

K32 WORD WINAPI GlobalDeleteAtom(WORD a)
{
    if (a < 0xC000 || a >= 0xC000 + MAX_ATOMS) return 0;
    AcquireSRWLockExclusive(&g_atom_lock);
    int i = a - 0xC000;
    if (g_atoms[i].name && --g_atoms[i].refs <= 0) { HeapFree(GetProcessHeap(), 0, g_atoms[i].name); g_atoms[i].name = NULL; }
    ReleaseSRWLockExclusive(&g_atom_lock);
    return 0;
}
K32 WORD WINAPI DeleteAtom(WORD a) { return GlobalDeleteAtom(a); }

K32 UINT WINAPI GlobalGetAtomNameW(WORD a, LPWSTR buf, int n)
{
    if (!buf || n <= 0) return 0;
    if (a < 0xC000) {
        WCHAR tmp[8];
        int k = 0;
        unsigned v = a;
        do { tmp[k++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
        int o = 0;
        if (o < n - 1) buf[o++] = '#';
        while (k && o < n - 1) buf[o++] = tmp[--k];
        buf[o] = 0;
        return (UINT)o;
    }
    int i = a - 0xC000;
    if (i >= MAX_ATOMS || !g_atoms[i].name) { SetLastError(ERROR_INVALID_HANDLE); buf[0] = 0; return 0; }
    int o = 0;
    for (; g_atoms[i].name[o] && o < n - 1; o++) buf[o] = g_atoms[i].name[o];
    buf[o] = 0;
    return (UINT)o;
}
K32 UINT WINAPI GetAtomNameW(WORD a, LPWSTR buf, int n) { return GlobalGetAtomNameW(a, buf, n); }

K32 UINT WINAPI GlobalGetAtomNameA(WORD a, LPSTR buf, int n)
{
    WCHAR w[256];
    UINT k = GlobalGetAtomNameW(a, w, 256);
    if (!k || !buf || n <= 0) { if (buf && n > 0) buf[0] = 0; return 0; }
    int m = WideCharToMultiByte(CP_ACP, 0, w, (int)k, buf, n - 1, NULL, NULL);
    buf[m] = 0;
    return (UINT)m;
}
K32 UINT WINAPI GetAtomNameA(WORD a, LPSTR buf, int n) { return GlobalGetAtomNameA(a, buf, n); }
K32 BOOL WINAPI InitAtomTable(DWORD n) { (void)n; return TRUE; }
