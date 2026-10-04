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
    WCHAR w[4] = { 0 };
    for (int i = 0; root && i < 3 && root[i]; i++) w[i] = (BYTE)root[i];
    return GetDiskFreeSpaceW(root ? w : 0, spc, bps, fc, tc);
}

/* "C:\", "D:\", ... each NUL-terminated, then a NUL; the length without that last NUL */
K32 DWORD WINAPI GetLogicalDriveStringsW(DWORD n, LPWSTR buf)
{
    DWORD map = GetLogicalDrives(), need = 0;
    for (int i = 0; i < 26; i++) if (map & (1u << i)) need += 4;
    if (n < need + 1) return need + 1;
    for (int i = 0; i < 26; i++)
        if (map & (1u << i)) { *buf++ = (WCHAR)('A' + i); *buf++ = ':'; *buf++ = '\\'; *buf++ = 0; }
    *buf = 0;
    return need;
}

K32 DWORD WINAPI GetLogicalDriveStringsA(DWORD n, LPSTR buf)
{
    DWORD map = GetLogicalDrives(), need = 0;
    for (int i = 0; i < 26; i++) if (map & (1u << i)) need += 4;
    if (n < need + 1) return need + 1;
    for (int i = 0; i < 26; i++)
        if (map & (1u << i)) { *buf++ = (char)('A' + i); *buf++ = ':'; *buf++ = '\\'; *buf++ = 0; }
    *buf = 0;
    return need;
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

/* No reparse points on drive C: (hard links: CreateHardLink in extra.c) */
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
/* The kernel keeps the CPU number in TSC_AUX (RDTSCP); a processor
 * without RDTSCP (QEMU's default model) gives its initial APIC ID, which is
 * the CPU number on the machines NovaOS starts */
K32 DWORD WINAPI GetCurrentProcessorNumber(void)
{
    static volatile int has_rdtscp = -1;
    unsigned a, b, c, d;
    if (has_rdtscp < 0) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
        has_rdtscp = (d >> 27) & 1;
    }
    if (has_rdtscp) {
        unsigned aux;
        __builtin_ia32_rdtscp(&aux);
        return aux & 0xFFF;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0));
    DWORD id = b >> 24;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return id < si.dwNumberOfProcessors ? id : 0;
}
K32 VOID WINAPI GetCurrentProcessorNumberEx(PVOID p) { WORD *w = p; w[0] = 0; ((BYTE *)p)[2] = (BYTE)GetCurrentProcessorNumber(); ((BYTE *)p)[3] = 0; }
/* 32-bit programs run under WoW64 (as far as they can tell) */
K32 BOOL WINAPI IsWow64Process(HANDLE h, PBOOL wow) { (void)h; *wow = sizeof(void *) == 4; return TRUE; }
/* File system redirection (System32 -> SysWOW64 for 32-bit programs): the
 * kernel reads this thread's switch from its 32-bit TEB */
#define TEB32_NO_REDIRECT 0xFF8
K32 BOOL WINAPI Wow64DisableWow64FsRedirection(PVOID *old)
{
#ifndef _WIN64
    DWORD *f = (DWORD *)(NtCurrentTebBytes() + TEB32_NO_REDIRECT);
    if (old) *old = (PVOID)(ULONG_PTR)*f;
    *f = 1;
#else
    if (old) *old = 0;
#endif
    return TRUE;
}
K32 BOOL WINAPI Wow64RevertWow64FsRedirection(PVOID old)
{
#ifndef _WIN64
    *(DWORD *)(NtCurrentTebBytes() + TEB32_NO_REDIRECT) = (DWORD)(ULONG_PTR)old;
#else
    (void)old;
#endif
    return TRUE;
}
K32 BOOLEAN WINAPI Wow64EnableWow64FsRedirection(BOOLEAN enable)
{
#ifndef _WIN64
    *(DWORD *)(NtCurrentTebBytes() + TEB32_NO_REDIRECT) = !enable;
#else
    (void)enable;
#endif
    return TRUE;
}
K32 BOOL WINAPI IsWow64Process2(HANDLE h, USHORT *proc, USHORT *native)
{
    (void)h;
    if (proc) *proc = sizeof(void *) == 4 ? 0x014C : 0;      /* IMAGE_FILE_MACHINE_I386, or not WoW64 */
    if (native) *native = 0x8664;
    return TRUE;
}

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
    if (!s) return;
    int n = w2u(s, -1, buf, sizeof(buf) - 1);
    buf[n < 0 ? (int)sizeof(buf) - 1 : n] = 0;
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
K32 BOOL WINAPI Module32First(HANDLE h, LPVOID me) { (void)h; (void)me; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
K32 BOOL WINAPI Module32Next(HANDLE h, LPVOID me) { (void)h; (void)me; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }

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
    volatile LONG gone;                 /* unregistered by its own callback */
} WaitReg;

static void wait_free(WaitReg *r)
{
    CloseHandle(r->thread);
    CloseHandle(r->stop);
    if (r->obj) CloseHandle(r->obj);
    r->magic = 0;
    zfree(r);
}

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
        if (r->gone) { wait_free(r); return 0; }        /* (the callback unregistered it) */
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
    if (GetCurrentThreadId() == GetThreadId(r->thread)) {   /* from its callback: the thread frees it */
        r->magic = 0;
        r->gone = 1;
        if (done && done != INVALID_HANDLE_VALUE) SetEvent(done);
        return TRUE;
    }
    if (done == INVALID_HANDLE_VALUE) WaitForSingleObject(r->thread, INFINITE);
    else if (done) { WaitForSingleObject(r->thread, INFINITE); SetEvent(done); }
    wait_free(r);
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

/* Threadpool timers and timer queue timers: a worker thread per timer
 * waits on a kernel waitable timer (to the TSC, not the 10 ms tick;
 * periodic ones on their own grid, so they don't drift) and calls the
 * callback.  A freed timer's worker waits for the next one: a new thread
 * would start late on a busy machine, and its first callback with it. */
typedef struct Worker {
    struct Worker *next;            /* (the idle list) */
    HANDLE timer, thread;
    SRWLOCK lock;                   /* the callback below */
    void (*call)(void *a, void *b, void *c);   /* NULL: no timer uses it */
    void *a, *b, *c;
    volatile LONG busy;             /* in a callback */
    DWORD tid;
} Worker;
static Worker *g_idle_workers;
static SRWLOCK g_worker_lock;

static DWORD WINAPI worker_thread(LPVOID p)
{
    Worker *w = p;
    w->tid = GetCurrentThreadId();
    for (;;) {
        if (WaitForSingleObject(w->timer, INFINITE) != WAIT_OBJECT_0) { Sleep(10); continue; }
        AcquireSRWLockExclusive(&w->lock);
        void (*call)(void *, void *, void *) = w->call;
        void *a = w->a, *b = w->b, *c = w->c;
        if (call) w->busy = 1;
        ReleaseSRWLockExclusive(&w->lock);
        if (!call) continue;
        call(a, b, c);
        w->busy = 0;
    }
}

static Worker *worker_get(void (*call)(void *, void *, void *), void *a, void *b, void *c)
{
    AcquireSRWLockExclusive(&g_worker_lock);
    Worker *w = g_idle_workers;
    if (w) g_idle_workers = w->next;
    ReleaseSRWLockExclusive(&g_worker_lock);
    if (!w) {
        w = zalloc(sizeof(*w));
        if (!w) return 0;
        w->timer = CreateWaitableTimerW(0, FALSE, 0);
        w->thread = w->timer ? CreateThread(0, 64 * 1024, worker_thread, w, 0, 0) : 0;
        if (!w->thread) { if (w->timer) CloseHandle(w->timer); zfree(w); return 0; }
    }
    AcquireSRWLockExclusive(&w->lock);
    w->call = call; w->a = a; w->b = b; w->c = c;
    ReleaseSRWLockExclusive(&w->lock);
    return w;
}

/* @due: 100 ns units, negative relative (as SetWaitableTimer); @period ms */
static void worker_set(Worker *w, LONGLONG due, DWORD period)
{
    LARGE_INTEGER d;
    d.QuadPart = due;
    SetWaitableTimer(w->timer, &d, (LONG)period, 0, 0, FALSE);
}

/* No more callbacks; when @wait, a running one has finished too (unless
 * it is the caller) */
static void worker_quiet(Worker *w, BOOL wait)
{
    CancelWaitableTimer(w->timer);
    AcquireSRWLockExclusive(&w->lock);
    w->call = 0;
    ReleaseSRWLockExclusive(&w->lock);
    if (wait && w->tid != GetCurrentThreadId()) while (w->busy) Sleep(1);
}

static void worker_put(Worker *w, BOOL wait)
{
    worker_quiet(w, wait);
    AcquireSRWLockExclusive(&g_worker_lock);
    w->next = g_idle_workers;
    g_idle_workers = w;
    ReleaseSRWLockExclusive(&g_worker_lock);
}

typedef VOID (WINAPI *TpTimerFn)(PVOID instance, PVOID ctx, PVOID timer);
typedef struct { DWORD magic; TpTimerFn fn; PVOID ctx; Worker *w; BOOL set; } TpTimer;

static void tp_timer_call(void *fn, void *ctx, void *t) { ((TpTimerFn)fn)(0, ctx, t); }

K32 PVOID WINAPI CreateThreadpoolTimer(TpTimerFn fn, PVOID ctx, PVOID env)
{
    (void)env;
    TpTimer *t = zalloc(sizeof(*t));
    if (!t) return 0;
    t->magic = 0x54505449; t->fn = fn; t->ctx = ctx;
    t->w = worker_get(tp_timer_call, (void *)fn, ctx, t);
    if (!t->w) { zfree(t); return 0; }
    return t;
}

K32 VOID WINAPI SetThreadpoolTimer(PVOID p, PFILETIME due, DWORD period, DWORD window)
{
    (void)window;
    TpTimer *t = p;
    if (!due) { CancelWaitableTimer(t->w->timer); t->set = FALSE; return; }
    LONGLONG d = ft(due);
    worker_set(t->w, d ? d : -1, period);           /* (0 is "now"; as an absolute time it is long gone) */
    t->set = TRUE;
}
K32 BOOL WINAPI IsThreadpoolTimerSet(PVOID p) { return ((TpTimer *)p)->set; }
K32 VOID WINAPI WaitForThreadpoolTimerCallbacks(PVOID p, BOOL cancel)
{
    TpTimer *t = p;
    if (cancel) SetThreadpoolTimer(t, 0, 0, 0);
    if (t->w->tid != GetCurrentThreadId()) while (t->w->busy) Sleep(1);
}
K32 VOID WINAPI CloseThreadpoolTimer(PVOID p)
{
    TpTimer *t = p;
    worker_put(t->w, FALSE);
    t->magic = 0;
    zfree(t);
}

/* Timer queues (CreateTimerQueueTimer): the timers above, in a list per
 * queue (NULL: the default queue) */
typedef struct QueueTimer { struct QueueTimer *next; Worker *w; struct TimerQueue *q; } QueueTimer;
typedef struct TimerQueue { DWORD magic; QueueTimer *timers; } TimerQueue;
static TimerQueue g_default_queue;
static SRWLOCK g_queue_lock;

static void queue_timer_call(void *fn, void *param, void *unused) { (void)unused; ((WAITORTIMERCALLBACK)fn)(param, TRUE); }

K32 HANDLE WINAPI CreateTimerQueue(void)
{
    TimerQueue *q = zalloc(sizeof(*q));
    if (!q) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    q->magic = 0x51524D54;
    return q;
}

K32 BOOL WINAPI CreateTimerQueueTimer(PHANDLE out, HANDLE queue, WAITORTIMERCALLBACK fn, PVOID param,
                                      DWORD due, DWORD period, ULONG flags)
{
    TimerQueue *q = queue ? queue : &g_default_queue;
    QueueTimer *t = zalloc(sizeof(*t));
    if (t) t->w = worker_get(queue_timer_call, (void *)fn, param, 0);
    if (!t || !t->w) { zfree(t); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    t->q = q;
    AcquireSRWLockExclusive(&g_queue_lock);
    t->next = q->timers;
    q->timers = t;
    ReleaseSRWLockExclusive(&g_queue_lock);
    worker_set(t->w, -(LONGLONG)due * 10000, flags & WT_EXECUTEONLYONCE ? 0 : period);
    *out = t;
    return TRUE;
}

K32 BOOL WINAPI ChangeTimerQueueTimer(HANDLE queue, HANDLE timer, ULONG due, ULONG period)
{
    (void)queue;
    if (!timer) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    worker_set(((QueueTimer *)timer)->w, -(LONGLONG)due * 10000, period);
    return TRUE;
}

/* @completion: INVALID_HANDLE_VALUE waits for a running callback; an
 * event is set once it has finished; NULL returns at once */
static void queue_timer_free(QueueTimer *t, HANDLE completion)
{
    worker_put(t->w, completion != 0);
    if (completion && completion != INVALID_HANDLE_VALUE) SetEvent(completion);
    zfree(t);
}

K32 BOOL WINAPI DeleteTimerQueueTimer(HANDLE queue, HANDLE timer, HANDLE completion)
{
    QueueTimer *t = timer;
    if (!t) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    (void)queue;
    AcquireSRWLockExclusive(&g_queue_lock);
    for (QueueTimer **pp = &t->q->timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    ReleaseSRWLockExclusive(&g_queue_lock);
    queue_timer_free(t, completion);
    return TRUE;
}

K32 BOOL WINAPI DeleteTimerQueueEx(HANDLE queue, HANDLE completion)
{
    TimerQueue *q = queue ? queue : &g_default_queue;
    AcquireSRWLockExclusive(&g_queue_lock);
    QueueTimer *list = q->timers;
    q->timers = 0;
    ReleaseSRWLockExclusive(&g_queue_lock);
    for (QueueTimer *t = list, *n; t; t = n) { n = t->next; queue_timer_free(t, completion ? INVALID_HANDLE_VALUE : 0); }
    if (completion && completion != INVALID_HANDLE_VALUE) SetEvent(completion);
    if (q != &g_default_queue) { q->magic = 0; zfree(q); }
    return TRUE;
}
K32 BOOL WINAPI DeleteTimerQueue(HANDLE queue) { return DeleteTimerQueueEx(queue, 0); }

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
 * National language support (GetDateFormat and the other formatting
 * functions are in nlsformat.c)
 * ----------------------------------------------------------------------- */
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

/* -----------------------------------------------------------------------
 * NUMA (one node holding every processor), volumes (one: C:), odds
 * ----------------------------------------------------------------------- */
K32 BOOL WINAPI GetNumaHighestNodeNumber(PULONG node) { *node = 0; return TRUE; }
K32 BOOL WINAPI GetNumaNodeProcessorMask(UCHAR node, PULONGLONG mask)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    *mask = node ? 0 : (si.dwNumberOfProcessors >= 64 ? ~0ULL : (1ULL << si.dwNumberOfProcessors) - 1);
    return TRUE;
}
K32 BOOL WINAPI GetNumaProcessorNode(UCHAR cpu, PUCHAR node) { (void)cpu; *node = 0; return TRUE; }
/* PROCESSOR_NUMBER { WORD Group; BYTE Number, Reserved } -> USHORT node */
K32 BOOL WINAPI GetNumaProcessorNodeEx(PVOID cpu, USHORT *node) { (void)cpu; *node = 0; return TRUE; }
/* GROUP_AFFINITY { KAFFINITY Mask; WORD Group; WORD Reserved[3] } */
K32 BOOL WINAPI GetNumaNodeProcessorMaskEx(USHORT node, PVOID aff)
{
    ULONGLONG m = 0;
    if (node) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GetNumaNodeProcessorMask(0, &m);
    memset(aff, 0, 16);
    *(ULONG_PTR *)aff = (ULONG_PTR)m;
    return TRUE;
}
K32 BOOL WINAPI GetNumaAvailableMemoryNode(UCHAR node, PULONGLONG bytes)
{
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    *bytes = node ? 0 : m.ullAvailPhys;
    return TRUE;
}

static const WCHAR g_volume[] = L"\\\\?\\Volume{4e4f5641-0000-0000-0000-000000000001}\\";
K32 HANDLE WINAPI FindFirstVolumeW(LPWSTR name, DWORD n)
{
    DWORD len = sizeof(g_volume) / sizeof(WCHAR);
    if (n < len) { SetLastError(206 /* ERROR_FILENAME_EXCED_RANGE */); return INVALID_HANDLE_VALUE; }
    for (DWORD i = 0; i < len; i++) name[i] = g_volume[i];
    return (HANDLE)(ULONG_PTR)0x4E56;
}
K32 HANDLE WINAPI FindFirstVolumeA(LPSTR name, DWORD n)
{
    DWORD len = sizeof(g_volume) / sizeof(WCHAR);
    if (n < len) { SetLastError(206 /* ERROR_FILENAME_EXCED_RANGE */); return INVALID_HANDLE_VALUE; }
    for (DWORD i = 0; i < len; i++) name[i] = (char)g_volume[i];
    return (HANDLE)(ULONG_PTR)0x4E56;
}
K32 BOOL WINAPI FindNextVolumeW(HANDLE h, LPWSTR name, DWORD n) { (void)h; (void)name; (void)n; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
K32 BOOL WINAPI FindNextVolumeA(HANDLE h, LPSTR name, DWORD n) { (void)h; (void)name; (void)n; SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
K32 BOOL WINAPI FindVolumeClose(HANDLE h) { (void)h; return TRUE; }

K32 BOOL WINAPI GetSystemTimeAdjustment(PDWORD adj, PDWORD inc, PBOOL disabled)
{
    *adj = 156250; *inc = 156250; *disabled = TRUE;         /* 15.625 ms ticks, no adjustment */
    return TRUE;
}

/* Console input records: the Terminal delivers lines, not key events */

/* Threads in this process only */
K32 HANDLE WINAPI CreateRemoteThread(HANDLE p, LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn,
                                     LPVOID arg, DWORD flags, LPDWORD tid)
{
    if (p != GetCurrentProcess() && GetProcessId(p) != GetCurrentProcessId()) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    return CreateThread(sa, stack, fn, arg, flags, tid);
}
K32 HANDLE WINAPI CreateRemoteThreadEx(HANDLE p, LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn,
                                       LPVOID arg, DWORD flags, LPPROC_THREAD_ATTRIBUTE_LIST attrs, LPDWORD tid)
{
    (void)attrs;
    return CreateRemoteThread(p, sa, stack, fn, arg, flags, tid);
}

/* -----------------------------------------------------------------------
 * More of what POSIX layers (Cygwin/MSYS2) import
 * ----------------------------------------------------------------------- */
typedef ULONG_PTR NOVA_KAFFINITY;

K32 PVOID WINAPI AddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) { return RtlAddVectoredContinueHandler(first, h); }
K32 ULONG WINAPI RemoveVectoredContinueHandler(PVOID h) { return RtlRemoveVectoredContinueHandler(h); }

K32 BOOL WINAPI CreateDirectoryExA(LPCSTR tmpl, LPCSTR dir, LPSECURITY_ATTRIBUTES sa) { (void)tmpl; return CreateDirectoryA(dir, sa); }
K32 BOOL WINAPI CreateDirectoryExW(LPCWSTR tmpl, LPCWSTR dir, LPSECURITY_ATTRIBUTES sa) { (void)tmpl; return CreateDirectoryW(dir, sa); }

K32 BOOL WINAPI IsBadStringPtrA(LPCSTR s, UINT_PTR max)
{
    MEMORY_BASIC_INFORMATION mbi;
    for (UINT_PTR i = 0; i < max; i++) {
        if (!(((ULONG_PTR)s + i) & 0xFFF) || !i) {
            if (!VirtualQuery(s + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | 0x100 /* PAGE_GUARD */)))
                return TRUE;
        }
        if (!s[i]) return FALSE;
    }
    return FALSE;
}

/* Other processes' memory (through a process handle) */
K32 BOOL WINAPI ReadProcessMemory(HANDLE p, LPCVOID base, LPVOID buf, SIZE_T n, SIZE_T *done)
{
    SIZE_T got = 0;
    NTSTATUS s = NtReadVirtualMemory(p, (PVOID)base, buf, n, &got);
    if (done) *done = got;
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}
K32 BOOL WINAPI WriteProcessMemory(HANDLE p, LPVOID base, LPCVOID buf, SIZE_T n, SIZE_T *done)
{
    SIZE_T got = 0;
    NTSTATUS s = NtWriteVirtualMemory(p, base, (PVOID)buf, n, &got);
    if (done) *done = got;
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}
K32 SIZE_T WINAPI VirtualQueryEx(HANDLE p, LPCVOID a, PMEMORY_BASIC_INFORMATION mbi, SIZE_T n)
{
    SIZE_T got = 0;
    NTSTATUS s = NtQueryVirtualMemory(p, (PVOID)a, 0, mbi, n, &got);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return got;
}
K32 BOOL WINAPI VirtualProtectEx(HANDLE p, LPVOID a, SIZE_T size, DWORD prot, LPDWORD old)
{
    PVOID base = a;
    ULONG o = 0;
    NTSTATUS s = NtProtectVirtualMemory(p, &base, &size, prot, &o);
    if (old) *old = o;
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}
K32 LPVOID WINAPI VirtualAllocEx(HANDLE p, LPVOID a, SIZE_T size, DWORD type, DWORD prot)
{
    PVOID base = a;
    NTSTATUS s = NtAllocateVirtualMemory(p, &base, 0, &size, type, prot);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return base;
}
K32 BOOL WINAPI VirtualFreeEx(HANDLE p, LPVOID a, SIZE_T size, DWORD type)
{
    PVOID base = a;
    NTSTATUS s = NtFreeVirtualMemory(p, &base, &size, type);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}
K32 BOOL WINAPI PrefetchVirtualMemory(HANDLE p, ULONG_PTR n, PVOID ranges, ULONG flags) { (void)p; (void)n; (void)ranges; (void)flags; return TRUE; }

K32 HANDLE WINAPI OpenThread(DWORD access, BOOL inherit, DWORD tid)
{
    CLIENT_ID cid = { 0, (HANDLE)(ULONG_PTR)tid };
    OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, 0, inherit ? OBJ_INHERIT : 0, 0, 0 };
    HANDLE h = 0;
    NTSTATUS s = NtOpenThread(&h, access, &oa, &cid);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return h;
}

/* Working sets and processor groups: one group, nothing trimmed */
K32 BOOL WINAPI GetProcessWorkingSetSize(HANDLE p, PSIZE_T mn, PSIZE_T mx) { (void)p; *mn = 200 * 4096; *mx = 1380 * 4096; return TRUE; }
K32 BOOL WINAPI SetProcessWorkingSetSize(HANDLE p, SIZE_T mn, SIZE_T mx) { (void)p; (void)mn; (void)mx; return TRUE; }
typedef struct { NOVA_KAFFINITY Mask; WORD Group, Reserved[3]; } NOVA_GROUP_AFFINITY;
static NOVA_KAFFINITY all_cpus(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors >= 8 * sizeof(NOVA_KAFFINITY) ? ~(NOVA_KAFFINITY)0 : ((NOVA_KAFFINITY)1 << si.dwNumberOfProcessors) - 1;
}
K32 BOOL WINAPI GetThreadGroupAffinity(HANDLE t, NOVA_GROUP_AFFINITY *a) { (void)t; memset(a, 0, sizeof(*a)); a->Mask = all_cpus(); return TRUE; }
K32 BOOL WINAPI SetThreadGroupAffinity(HANDLE t, const NOVA_GROUP_AFFINITY *a, NOVA_GROUP_AFFINITY *prev)
{
    (void)t; (void)a;
    if (prev) { memset(prev, 0, sizeof(*prev)); prev->Mask = all_cpus(); }
    return TRUE;
}
K32 BOOL WINAPI GetProcessGroupAffinity(HANDLE p, USHORT *count, USHORT *groups)
{
    (void)p;
    if (*count < 1) { *count = 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *count = 1;
    groups[0] = 0;
    return TRUE;
}

/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: one core per CPU in one package,
 * NUMA node and group (RelationAll or one relation) */
K32 BOOL WINAPI GetLogicalProcessorInformationEx(int rel, PVOID buf, PDWORD len)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD n = si.dwNumberOfProcessors;
    enum { CORE = 0, NUMA = 1, CACHE = 2, PACKAGE = 3, GROUP = 4, ALL = 0xFFFF };
    const DWORD proc_size = 48, numa_size = 48, group_size = 80;   /* PROCESSOR_RELATIONSHIP (1 group), NUMA_NODE, GROUP_RELATIONSHIP (1) */
    DWORD need = 0;
    if (rel == CORE || rel == ALL) need += n * proc_size;
    if (rel == PACKAGE || rel == ALL) need += proc_size;
    if (rel == NUMA || rel == ALL) need += numa_size;
    if (rel == GROUP || rel == ALL) need += group_size;
    if (*len < need) { *len = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *len = need;
    BYTE *p = buf;
    memset(p, 0, need);
    NOVA_KAFFINITY mask = all_cpus();
    if (rel == CORE || rel == ALL)
        for (DWORD i = 0; i < n; i++, p += proc_size) {
            *(DWORD *)p = CORE; *(DWORD *)(p + 4) = proc_size;
            *(WORD *)(p + 30) = 1;                                   /* GroupCount */
            *(NOVA_KAFFINITY *)(p + 32) = (NOVA_KAFFINITY)1 << i;              /* GroupMask[0].Mask */
        }
    if (rel == PACKAGE || rel == ALL) {
        *(DWORD *)p = PACKAGE; *(DWORD *)(p + 4) = proc_size;
        *(WORD *)(p + 30) = 1;
        *(NOVA_KAFFINITY *)(p + 32) = mask;
        p += proc_size;
    }
    if (rel == NUMA || rel == ALL) {
        *(DWORD *)p = NUMA; *(DWORD *)(p + 4) = numa_size;
        *(NOVA_KAFFINITY *)(p + 32) = mask;                               /* GroupMask.Mask */
        p += numa_size;
    }
    if (rel == GROUP || rel == ALL) {
        *(DWORD *)p = GROUP; *(DWORD *)(p + 4) = group_size;
        *(WORD *)(p + 8) = 1; *(WORD *)(p + 10) = 1;                 /* MaximumGroupCount, ActiveGroupCount */
        *(BYTE *)(p + 32) = (BYTE)n; *(BYTE *)(p + 33) = (BYTE)n;    /* GroupInfo[0]: Maximum/ActiveProcessorCount */
        *(NOVA_KAFFINITY *)(p + 72) = mask;                               /* ActiveProcessorMask */
    }
    return TRUE;
}

/* Devices and volumes: C: is the one volume; there are no MS-DOS devices
 * but the drive, and no serial ports or tapes */
K32 DWORD WINAPI QueryDosDeviceW(LPCWSTR name, LPWSTR buf, DWORD n)
{
    static const WCHAR c_dev[] = L"\\Device\\HarddiskVolume1";
    const WCHAR *v = 0;
    if (!name) {                                                     /* every name: "C:\0\0" */
        if (n < 4) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        buf[0] = 'C'; buf[1] = ':'; buf[2] = 0; buf[3] = 0;
        return 4;
    }
    if ((name[0] == 'C' || name[0] == 'c') && name[1] == ':' && !name[2]) v = c_dev;
    if (!v) { SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
    DWORD k = (DWORD)wlen(v);
    if (n < k + 2) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, v, 2 * k);
    buf[k] = 0; buf[k + 1] = 0;
    return k + 2;
}
K32 BOOL WINAPI GetVolumeNameForVolumeMountPointW(LPCWSTR mount, LPWSTR buf, DWORD n)
{
    static const WCHAR vol[] = L"\\\\?\\Volume{4e6f7661-0000-0000-0000-000000000001}\\";
    if (!mount || (mount[0] != 'C' && mount[0] != 'c') || mount[1] != ':') { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD k = (DWORD)wlen(vol);
    if (n <= k) { SetLastError(206 /* ERROR_FILENAME_EXCED_RANGE */); return FALSE; }
    memcpy(buf, vol, 2 * (k + 1));
    return TRUE;
}
K32 BOOL WINAPI GetVolumePathNamesForVolumeNameW(LPCWSTR vol, LPWSTR buf, DWORD n, PDWORD ret)
{
    (void)vol;
    if (ret) *ret = 5;
    if (n < 5) { SetLastError(ERROR_MORE_DATA); return FALSE; }
    buf[0] = 'C'; buf[1] = ':'; buf[2] = '\\'; buf[3] = 0; buf[4] = 0;
    return TRUE;
}
K32 BOOL WINAPI SetComputerNameExW(int type, LPCWSTR name) { (void)type; (void)name; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

#define NO_DEVICE() do { SetLastError(ERROR_INVALID_HANDLE); return FALSE; } while (0)
K32 BOOL WINAPI GetCommState(HANDLE h, PVOID dcb) { (void)h; (void)dcb; NO_DEVICE(); }
K32 BOOL WINAPI SetCommState(HANDLE h, PVOID dcb) { (void)h; (void)dcb; NO_DEVICE(); }
K32 BOOL WINAPI SetCommTimeouts(HANDLE h, PVOID t) { (void)h; (void)t; NO_DEVICE(); }
K32 BOOL WINAPI SetCommMask(HANDLE h, DWORD m) { (void)h; (void)m; NO_DEVICE(); }
K32 BOOL WINAPI WaitCommEvent(HANDLE h, LPDWORD m, LPOVERLAPPED o) { (void)h; (void)m; (void)o; NO_DEVICE(); }
K32 BOOL WINAPI ClearCommError(HANDLE h, LPDWORD e, PVOID st) { (void)h; (void)e; (void)st; NO_DEVICE(); }
K32 BOOL WINAPI ClearCommBreak(HANDLE h) { (void)h; NO_DEVICE(); }
K32 BOOL WINAPI SetCommBreak(HANDLE h) { (void)h; NO_DEVICE(); }
K32 BOOL WINAPI EscapeCommFunction(HANDLE h, DWORD f) { (void)h; (void)f; NO_DEVICE(); }
K32 BOOL WINAPI GetCommModemStatus(HANDLE h, LPDWORD s) { (void)h; (void)s; NO_DEVICE(); }
K32 BOOL WINAPI PurgeComm(HANDLE h, DWORD f) { (void)h; (void)f; NO_DEVICE(); }
K32 BOOL WINAPI TransmitCommChar(HANDLE h, char c) { (void)h; (void)c; NO_DEVICE(); }
K32 DWORD WINAPI GetTapeParameters(HANDLE h, DWORD op, LPDWORD n, LPVOID info) { (void)h; (void)op; (void)n; (void)info; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI SetTapeParameters(HANDLE h, DWORD op, LPVOID info) { (void)h; (void)op; (void)info; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI GetTapePosition(HANDLE h, DWORD t, LPDWORD p, LPDWORD lo, LPDWORD hi) { (void)h; (void)t; (void)p; (void)lo; (void)hi; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI SetTapePosition(HANDLE h, DWORD m, DWORD p, DWORD lo, DWORD hi, BOOL imm) { (void)h; (void)m; (void)p; (void)lo; (void)hi; (void)imm; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI GetTapeStatus(HANDLE h) { (void)h; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI PrepareTape(HANDLE h, DWORD op, BOOL imm) { (void)h; (void)op; (void)imm; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI EraseTape(HANDLE h, DWORD t, BOOL imm) { (void)h; (void)t; (void)imm; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI CreateTapePartition(HANDLE h, DWORD m, DWORD c, DWORD s) { (void)h; (void)m; (void)c; (void)s; return ERROR_INVALID_HANDLE; }
K32 DWORD WINAPI WriteTapemark(HANDLE h, DWORD t, DWORD c, BOOL imm) { (void)h; (void)t; (void)c; (void)imm; return ERROR_INVALID_HANDLE; }

/* Is @name a program, and what kind */
K32 BOOL WINAPI GetBinaryTypeW(LPCWSTR name, LPDWORD type)
{
    HANDLE f = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BYTE h[512];
    DWORD got = 0;
    BOOL ok = ReadFile(f, h, sizeof(h), &got, 0);
    CloseHandle(f);
    if (!ok || got < 64 || h[0] != 'M' || h[1] != 'Z') { SetLastError(193 /* ERROR_BAD_EXE_FORMAT */); return FALSE; }
    DWORD pe = *(DWORD *)(h + 0x3C);
    if (pe + 24 > got || h[pe] != 'P' || h[pe + 1] != 'E' || h[pe + 2] || h[pe + 3]) { *type = 1; /* SCS_DOS_BINARY */ return TRUE; }
    WORD machine = *(WORD *)(h + pe + 4);
    WORD chars = *(WORD *)(h + pe + 22);
    if (chars & 0x2000) { SetLastError(193 /* ERROR_BAD_EXE_FORMAT */); return FALSE; }   /* a DLL */
    *type = machine == 0x8664 ? 6 /* SCS_64BIT_BINARY */ : 0 /* SCS_32BIT_BINARY */;
    return TRUE;
}

/* National language: only simple folding; IDN names pass through when ASCII */
K32 int WINAPI FoldStringW(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap)
{
    (void)flags;
    if (n < 0) n = wlen(src) + 1;
    if (!cap) return n;
    if (cap < n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) {
        WCHAR c = src[i];
        if (c >= 0xFF01 && c <= 0xFF5E) c = (WCHAR)(c - 0xFF01 + 0x21);   /* MAP_FOLDCZONE: fullwidth ASCII */
        dst[i] = c;
    }
    return n;
}
static int idn_copy(LPCWSTR src, int n, LPWSTR dst, int cap)
{
    if (n < 0) n = wlen(src) + 1;
    for (int i = 0; i < n; i++) if (src[i] > 0x7F) { SetLastError(ERROR_INVALID_NAME); return 0; }
    if (!cap) return n;
    if (cap < n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, src, 2 * (SIZE_T)n);
    return n;
}
K32 int WINAPI IdnToAscii(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap) { (void)flags; return idn_copy(src, n, dst, cap); }
K32 int WINAPI IdnToUnicode(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap) { (void)flags; return idn_copy(src, n, dst, cap); }
/* -----------------------------------------------------------------------
 * Console: the terminal is a stream of text; input records are made from
 * the characters typed, the screen buffer is the one the terminal shows
 * ----------------------------------------------------------------------- */
typedef struct {
    WORD EventType;
    struct { BOOL KeyDown; WORD RepeatCount, VirtualKeyCode, VirtualScanCode; WCHAR UnicodeChar; DWORD ControlKeyState; } Key;
} NOVA_INPUT_RECORD;

/* Input records live in the kernel's console (NtNovaConsole): the keys the
 * Terminal sends and what programs add with WriteConsoleInput */
static BOOL con_call(HANDLE h, ULONG op, PVOID buf, ULONG len, DWORD *res)
{
    ULONG r = 0;
    NTSTATUS s = NtNovaConsole(h, op, buf, len, &r);
    if (res) *res = r;
    if (!NT_SUCCESS(s)) { SetLastError(RtlNtStatusToDosError(s)); return FALSE; }
    return TRUE;
}
/* Reads in batches of at most 64 records */
static BOOL con_records(HANDLE h, ULONG op, NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD got)
{
    DWORD k = 0;
    BOOL ok = con_call(h, op, rec, n > 64 ? 64 : n, &k);
    if (got) *got = k;
    return ok;
}
/* The ANSI forms: characters above 0x7F become '?' (no code page but UTF-8) */
static void con_to_ansi(NOVA_INPUT_RECORD *rec, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        if (rec[i].EventType == 1 && rec[i].Key.UnicodeChar > 0x7F) rec[i].Key.UnicodeChar = '?';
}
K32 BOOL WINAPI ReadConsoleInputW(HANDLE h, NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD read) { return con_records(h, 2, rec, n, read); }
K32 BOOL WINAPI PeekConsoleInputW(HANDLE h, NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD read) { return con_records(h, 3, rec, n, read); }
K32 BOOL WINAPI ReadConsoleInputA(HANDLE h, NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD read)
{
    if (!ReadConsoleInputW(h, rec, n, read)) return FALSE;
    con_to_ansi(rec, *read);
    return TRUE;
}
K32 BOOL WINAPI PeekConsoleInputA(HANDLE h, NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD read)
{
    if (!PeekConsoleInputW(h, rec, n, read)) return FALSE;
    con_to_ansi(rec, *read);
    return TRUE;
}
/* ReadConsoleInputEx: flag 2 (CONSOLE_READ_NOWAIT) returns at once */
K32 BOOL WINAPI ReadConsoleInputExW(HANDLE h, NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD read, USHORT flags)
{
    if (flags & 2) {
        DWORD k = 0;
        if (!GetNumberOfConsoleInputEvents(h, &k)) return FALSE;
        if (!k) { if (read) *read = 0; return TRUE; }
    }
    return con_records(h, (flags & 1) ? 3 : 2, rec, n, read);   /* 1: CONSOLE_READ_NOREMOVE */
}
K32 BOOL WINAPI WriteConsoleInputW(HANDLE h, const NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD written)
{
    return con_call(h, 4, (PVOID)rec, n, written);
}
K32 BOOL WINAPI WriteConsoleInputA(HANDLE h, const NOVA_INPUT_RECORD *rec, DWORD n, LPDWORD written)
{
    return WriteConsoleInputW(h, rec, n, written);
}
K32 BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, LPDWORD n) { return con_call(h, 5, 0, 0, n); }
K32 BOOL WINAPI FlushConsoleInputBuffer(HANDLE h) { return con_call(h, 6, 0, 0, 0); }
K32 BOOL WINAPI GetConsoleMode(HANDLE h, LPDWORD mode) { return con_call(h, 0, 0, 0, mode); }
K32 BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode) { return con_call(h, 1, 0, mode, 0); }
K32 BOOL WINAPI GetNumberOfConsoleMouseButtons(LPDWORD n) { *n = 2; return TRUE; }

/* Pseudo consoles (ConPTY): not yet.  The functions exist because programs
 * probe for them to learn they run on a console that understands virtual
 * terminal sequences (Neovim's --embed server then talks to CONIN$/CONOUT$
 * and keeps its RPC on the pipes); creating one fails cleanly. */
K32 LONG WINAPI CreatePseudoConsole(COORD size, HANDLE in, HANDLE out, DWORD flags, PVOID *pc)
{
    (void)size; (void)in; (void)out; (void)flags;
    if (pc) *pc = 0;
    return (LONG)0x80004001;                                /* E_NOTIMPL */
}
K32 LONG WINAPI ResizePseudoConsole(PVOID pc, COORD size) { (void)pc; (void)size; return (LONG)0x80004001; }
K32 VOID WINAPI ClosePseudoConsole(PVOID pc) { (void)pc; }
K32 DWORD WINAPI GetConsoleProcessList(LPDWORD list, DWORD n)
{
    DWORD count = 0;
    if (!list || !n) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!con_call(0, 8, list, n, &count)) return 0;         /* CON_PROCESS_LIST */
    if (!count) SetLastError(ERROR_INVALID_HANDLE);         /* no console */
    return count;
}
K32 BOOL WINAPI GetCurrentConsoleFontEx(HANDLE h, BOOL max, PVOID info)
{
    (void)h; (void)max;
    BYTE *f = info;                                                      /* CONSOLE_FONT_INFOEX */
    DWORD cb = *(DWORD *)f;
    if (cb < 84) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(f + 4, 0, cb - 4);
    *(SHORT *)(f + 8) = 8; *(SHORT *)(f + 10) = 16;                     /* dwFontSize */
    *(UINT *)(f + 12) = 0x36;                                            /* FontFamily: FF_MODERN | TMPF_TRUETYPE|VECTOR */
    *(UINT *)(f + 16) = 400;                                             /* FontWeight */
    static const WCHAR face[] = L"Consolas";
    memcpy(f + 20, face, sizeof(face));
    return TRUE;
}
K32 BOOL WINAPI SetConsoleWindowInfo(HANDLE h, BOOL abs, const SMALL_RECT *r) { (void)h; (void)abs; (void)r; return TRUE; }
K32 BOOL WINAPI SetConsoleScreenBufferSize(HANDLE h, COORD size) { (void)h; (void)size; return TRUE; }
K32 HANDLE WINAPI CreateConsoleScreenBuffer(DWORD access, DWORD share, const SECURITY_ATTRIBUTES *sa, DWORD flags, LPVOID data)
{
    (void)access; (void)share; (void)sa; (void)flags; (void)data;
    HANDLE h = 0;                                                        /* the one screen: another handle to it */
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h ? h : INVALID_HANDLE_VALUE;
}
K32 BOOL WINAPI SetConsoleActiveScreenBuffer(HANDLE h) { (void)h; return TRUE; }
/* Cells: written as text at the cursor's row (a stream has no cells to read back) */
typedef struct { WCHAR Char; WORD Attributes; } NOVA_CHAR_INFO;
K32 BOOL WINAPI WriteConsoleOutputW(HANDLE h, const NOVA_CHAR_INFO *cells, COORD size, COORD at, SMALL_RECT *region)
{
    (void)at;
    for (SHORT y = region->Top; y <= region->Bottom && y - region->Top < size.Y; y++) {
        WCHAR line[512];
        int n = 0;
        for (SHORT x = region->Left; x <= region->Right && x - region->Left < size.X && n < 511; x++)
            line[n++] = cells[(y - region->Top) * size.X + (x - region->Left)].Char;
        DWORD w;
        WriteConsoleW(h, line, (DWORD)n, &w, 0);
    }
    return TRUE;
}
K32 BOOL WINAPI ReadConsoleOutputW(HANDLE h, NOVA_CHAR_INFO *cells, COORD size, COORD at, SMALL_RECT *region)
{
    (void)h; (void)at;
    for (int i = 0; i < size.X * size.Y; i++) { cells[i].Char = ' '; cells[i].Attributes = 7; }
    (void)region;
    return TRUE;
}
K32 BOOL WINAPI ScrollConsoleScreenBufferW(HANDLE h, const SMALL_RECT *r, const SMALL_RECT *clip, COORD dest, const NOVA_CHAR_INFO *fill)
{
    (void)h; (void)r; (void)clip; (void)dest; (void)fill;
    return TRUE;
}
K32 BOOL WINAPI ScrollConsoleScreenBufferA(HANDLE h, const SMALL_RECT *r, const SMALL_RECT *clip, COORD dest, const NOVA_CHAR_INFO *fill)
{
    return ScrollConsoleScreenBufferW(h, r, clip, dest, fill);
}

/* Allocation within an address range (MEM_EXTENDED_PARAMETER address requirements) */
K32 PVOID WINAPI VirtualAlloc2(HANDLE p, PVOID base, SIZE_T size, ULONG type, ULONG prot, PVOID params, ULONG n)
{
    PVOID b = base;
    NTSTATUS s = NtAllocateVirtualMemoryEx(p ? p : GetCurrentProcess(), &b, &size, type, prot, params, n);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return b;
}
K32 PVOID WINAPI VirtualAlloc2FromApp(HANDLE p, PVOID base, SIZE_T size, ULONG type, ULONG prot, PVOID params, ULONG n)
{
    return VirtualAlloc2(p, base, size, type, prot, params, n);
}
K32 PVOID WINAPI MapViewOfFile3(HANDLE sec, HANDLE p, PVOID base, ULONG64 off, SIZE_T size, ULONG type, ULONG prot,
                                PVOID params, ULONG n)
{
    PVOID b = base;
    LARGE_INTEGER o;
    o.QuadPart = (LONGLONG)off;
    NTSTATUS s = NtMapViewOfSectionEx(sec, p ? p : GetCurrentProcess(), &b, &o, &size, type, prot, params, n);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return b;
}

/* Interrupt time (100 ns units since boot) from the performance counter */
static ULONGLONG interrupt_time(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    if (f.QuadPart <= 0) return GetTickCount64() * 10000;
    return (ULONGLONG)c.QuadPart / (ULONGLONG)f.QuadPart * 10000000ULL +
           (ULONGLONG)c.QuadPart % (ULONGLONG)f.QuadPart * 10000000ULL / (ULONGLONG)f.QuadPart;
}
K32 VOID WINAPI QueryInterruptTimePrecise(PULONGLONG t)         { *t = interrupt_time(); }
K32 VOID WINAPI QueryUnbiasedInterruptTimePrecise(PULONGLONG t) { *t = interrupt_time(); }   /* (no sleep to leave out) */

K32 HRESULT WINAPI SetThreadDescription(HANDLE t, PCWSTR d) { (void)t; (void)d; return S_OK; }
K32 HRESULT WINAPI GetThreadDescription(HANDLE t, PWSTR *d)
{
    (void)t;
    *d = LocalAlloc(LMEM_ZEROINIT, 2);
    return *d ? S_OK : E_OUTOFMEMORY;
}
K32 DWORD WINAPI DiscardVirtualMemory(PVOID a, SIZE_T n) { (void)a; (void)n; return ERROR_SUCCESS; }

/* -----------------------------------------------------------------------
 * For the Java runtime (HotSpot) and friends
 * ----------------------------------------------------------------------- */
K32 LPVOID WINAPI VirtualAllocExNuma(HANDLE p, LPVOID addr, SIZE_T size, DWORD type, DWORD prot, DWORD node)
{
    (void)node;                                     /* one NUMA node */
    return VirtualAllocEx(p, addr, size, type, prot);
}
/* Physical-page (AWE) allocation needs SeLockMemoryPrivilege: not held */
K32 BOOL WINAPI AllocateUserPhysicalPages(HANDLE p, PULONG_PTR n, PULONG_PTR pfns)
{ (void)p; (void)n; (void)pfns; SetLastError(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
K32 BOOL WINAPI AllocateUserPhysicalPagesNuma(HANDLE p, PULONG_PTR n, PULONG_PTR pfns, DWORD node)
{ (void)node; return AllocateUserPhysicalPages(p, n, pfns); }
K32 BOOL WINAPI FreeUserPhysicalPages(HANDLE p, PULONG_PTR n, PULONG_PTR pfns)
{ (void)p; (void)n; (void)pfns; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
K32 BOOL WINAPI MapUserPhysicalPages(PVOID va, ULONG_PTR n, PULONG_PTR pfns)
{ (void)va; (void)n; (void)pfns; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

/* RaiseFailFastException: the process ends at once with the code */
K32 VOID WINAPI RaiseFailFastException(PEXCEPTION_RECORD rec, PCONTEXT ctx, DWORD flags)
{
    (void)ctx; (void)flags;
    TerminateProcess(GetCurrentProcess(), rec ? rec->ExceptionCode : 0xC0000602 /* STATUS_FAIL_FAST_EXCEPTION */);
}

/* Geography: the United States (GEOID 244) */
K32 LONG WINAPI GetUserGeoID(DWORD cls) { (void)cls; return 244; }
K32 int WINAPI GetUserDefaultGeoName(LPWSTR buf, int n)
{
    if (!buf || n < 3) return 3;
    buf[0] = 'U'; buf[1] = 'S'; buf[2] = 0;
    return 3;
}
static const char *geo_text(LONG id, DWORD type)
{
    if (id != 244) return 0;
    switch (type) {
    case 4: return "US";                             /* GEO_ISO2 */
    case 5: return "USA";                            /* GEO_ISO3 */
    case 6: return "1";                              /* GEO_RFC1766... (nation) */
    case 7: return "840";                            /* GEO_LCID / ISO_UN_NUMBER */
    case 8: return "840";
    case 9: return "United States";                  /* GEO_FRIENDLYNAME */
    case 10: return "United States";                 /* GEO_OFFICIALNAME */
    default: return 0;
    }
}
K32 int WINAPI GetGeoInfoA(LONG id, DWORD type, LPSTR buf, int n, LANGID lang)
{
    (void)lang;
    const char *t = geo_text(id, type);
    if (!t) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    int need = lstrlenA(t) + 1;
    if (!n) return need;
    if (n < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    lstrcpyA(buf, t);
    return need;
}
K32 int WINAPI GetGeoInfoW(LONG id, DWORD type, LPWSTR buf, int n, LANGID lang)
{
    (void)lang;
    const char *t = geo_text(id, type);
    if (!t) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    int need = lstrlenA(t) + 1;
    if (!n) return need;
    if (n < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < need; i++) buf[i] = (WCHAR)(BYTE)t[i];
    return need;
}

/* win.ini: there is none, so every value is the default */
K32 DWORD WINAPI GetProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR buf, DWORD n)
{
    (void)app; (void)key;
    if (!buf || !n) return 0;
    lstrcpynA(buf, def ? def : "", (int)n);
    return (DWORD)lstrlenA(buf);
}
K32 DWORD WINAPI GetProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR def, LPWSTR buf, DWORD n)
{
    (void)app; (void)key;
    if (!buf || !n) return 0;
    lstrcpynW(buf, def ? def : L"", (int)n);
    return (DWORD)lstrlenW(buf);
}
K32 UINT WINAPI GetProfileIntA(LPCSTR app, LPCSTR key, INT def) { (void)app; (void)key; return (UINT)def; }
K32 UINT WINAPI GetProfileIntW(LPCWSTR app, LPCWSTR key, INT def) { (void)app; (void)key; return (UINT)def; }

/* Every process is in session 1 */
K32 BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *session) { (void)pid; if (!session) return FALSE; *session = 1; return TRUE; }

/* -----------------------------------------------------------------------
 * For the .NET runtime (CoreCLR)
 * ----------------------------------------------------------------------- */
/* Extended processor state: the legacy x87 and SSE state only (no AVX
 * area in CONTEXT) */
K32 DWORD64 WINAPI GetEnabledXStateFeatures(void) { return 3; }
K32 BOOL WINAPI SetXStateFeaturesMask(PCONTEXT ctx, DWORD64 mask) { (void)ctx; return (mask & ~3ULL) == 0; }
K32 BOOL WINAPI GetXStateFeaturesMask(PCONTEXT ctx, PDWORD64 mask) { (void)ctx; *mask = 3; return TRUE; }
K32 PVOID WINAPI LocateXStateFeature(PCONTEXT ctx, DWORD id, PDWORD len)
{
#ifdef _WIN64
    if (id <= 1) { if (len) *len = 512; return (BYTE *)ctx + 0x100; }   /* FltSave (XSAVE_FORMAT) */
#else
    if (id <= 1) { if (len) *len = 512; return (BYTE *)ctx + 0xCC; }    /* ExtendedRegisters */
#endif
    if (len) *len = 0;
    return 0;
}
K32 BOOL WINAPI InitializeContext2(PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len, ULONG64 compaction)
{
    (void)compaction;
    DWORD need = (DWORD)sizeof(CONTEXT) + 15;
    if (!buf || !len || *len < need) {
        if (len) *len = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    CONTEXT *c = (CONTEXT *)(((ULONG_PTR)buf + 15) & ~(ULONG_PTR)15);
    memset(c, 0, sizeof(*c));
    c->ContextFlags = flags;
    *ctx = c;
    return TRUE;
}
K32 BOOL WINAPI InitializeContext(PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len)
{
    return InitializeContext2(buf, flags, ctx, len, 0);
}
K32 BOOL WINAPI CopyContext(PCONTEXT dst, DWORD flags, PCONTEXT src)
{
    if (!dst || !src) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memcpy(dst, src, sizeof(CONTEXT));
    dst->ContextFlags = flags & src->ContextFlags;
    return TRUE;
}

/* Signal one object, then wait for another */
K32 DWORD WINAPI SignalObjectAndWait(HANDLE sig, HANDLE wait, DWORD ms, BOOL alertable)
{
    DWORD err = GetLastError();
    if (!SetEvent(sig) && !ReleaseMutex(sig) && !ReleaseSemaphore(sig, 1, 0)) return WAIT_FAILED;
    SetLastError(err);
    return WaitForSingleObjectEx(wait, ms, alertable);
}

/* Memory is never low or high enough to notify: the event stays unset */
K32 HANDLE WINAPI CreateMemoryResourceNotification(int type) { (void)type; return CreateEventW(0, TRUE, FALSE, 0); }
K32 BOOL WINAPI QueryMemoryResourceNotification(HANDLE h, PBOOL state)
{
    if (!h || !state) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *state = FALSE;
    return TRUE;
}

/* Windows Error Reporting: nothing is reported */
K32 HRESULT WINAPI WerRegisterRuntimeExceptionModule(PCWSTR dll, PVOID ctx) { (void)dll; (void)ctx; return S_OK; }
K32 HRESULT WINAPI WerUnregisterRuntimeExceptionModule(PCWSTR dll, PVOID ctx) { (void)dll; (void)ctx; return S_OK; }
K32 HRESULT WINAPI WerSetFlags(DWORD f) { (void)f; return S_OK; }
K32 HRESULT WINAPI WerGetFlags(HANDLE p, PDWORD f) { (void)p; if (f) *f = 0; return S_OK; }
K32 HRESULT WINAPI WerRegisterMemoryBlock(PVOID p, DWORD n) { (void)p; (void)n; return S_OK; }
K32 HRESULT WINAPI WerUnregisterMemoryBlock(PVOID p) { (void)p; return S_OK; }
K32 HRESULT WINAPI WerRegisterCustomMetadata(PCWSTR key, PCWSTR value) { (void)key; (void)value; return S_OK; }
K32 HRESULT WINAPI WerUnregisterCustomMetadata(PCWSTR key) { (void)key; return S_OK; }
K32 HRESULT WINAPI WerRegisterFile(PCWSTR f, int t, DWORD flags) { (void)f; (void)t; (void)flags; return S_OK; }

/* PROCESSOR_NUMBER { WORD Group; BYTE Number, Reserved } */
K32 BOOL WINAPI SetThreadIdealProcessorEx(HANDLE t, PVOID ideal, PVOID prev)
{
    (void)t; (void)ideal;
    if (prev) memset(prev, 0, 4);
    return TRUE;
}
K32 BOOL WINAPI GetThreadIdealProcessorEx(HANDLE t, PVOID pn) { (void)t; if (pn) memset(pn, 0, 4); return TRUE; }

/* Pages are never paged out: locking always succeeds */
K32 BOOL WINAPI VirtualLock(LPVOID p, SIZE_T n) { (void)p; (void)n; return TRUE; }
K32 BOOL WINAPI VirtualUnlock(LPVOID p, SIZE_T n) { (void)p; (void)n; return TRUE; }

/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION: a core per processor, one package
 * and NUMA node, and caches (32 KiB L1 data and instruction and 1 MiB L2
 * per core, an 8 MiB L3 shared) */
typedef struct {
    ULONG_PTR ProcessorMask;
    int Relationship;
    union {
        BYTE Flags;
        DWORD NodeNumber;
        struct { BYTE Level, Associativity; WORD LineSize; DWORD Size; int Type; } Cache;
        ULONGLONG Reserved[2];
    };
} SLPI;
K32 BOOL WINAPI GetLogicalProcessorInformation(PVOID buf, PDWORD len)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD n = si.dwNumberOfProcessors, count = n * 4 + 3;   /* per core: core, L1d, L1i, L2; + package, NUMA, L3 */
    DWORD need = count * (DWORD)sizeof(SLPI);
    if (!len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!buf || *len < need) { *len = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *len = need;
    SLPI *e = buf;
    memset(e, 0, need);
    ULONG_PTR all = n >= sizeof(ULONG_PTR) * 8 ? ~(ULONG_PTR)0 : ((ULONG_PTR)1 << n) - 1;
    for (DWORD i = 0; i < n; i++) {
        ULONG_PTR m = (ULONG_PTR)1 << i;
        e->ProcessorMask = m; e->Relationship = 0; e->Flags = 0; e++;                  /* RelationProcessorCore */
        e->ProcessorMask = m; e->Relationship = 2;                                      /* RelationCache */
        e->Cache.Level = 1; e->Cache.Associativity = 8; e->Cache.LineSize = 64; e->Cache.Size = 32768; e->Cache.Type = 2; e++;
        e->ProcessorMask = m; e->Relationship = 2;
        e->Cache.Level = 1; e->Cache.Associativity = 8; e->Cache.LineSize = 64; e->Cache.Size = 32768; e->Cache.Type = 1; e++;
        e->ProcessorMask = m; e->Relationship = 2;
        e->Cache.Level = 2; e->Cache.Associativity = 16; e->Cache.LineSize = 64; e->Cache.Size = 1 << 20; e->Cache.Type = 0; e++;
    }
    e->ProcessorMask = all; e->Relationship = 2;
    e->Cache.Level = 3; e->Cache.Associativity = 16; e->Cache.LineSize = 64; e->Cache.Size = 8 << 20; e->Cache.Type = 0; e++;
    e->ProcessorMask = all; e->Relationship = 3; e++;                                   /* RelationProcessorPackage */
    e->ProcessorMask = all; e->Relationship = 1; e->NodeNumber = 0;                     /* RelationNumaNode */
    return TRUE;
}

/* Windows Runtime (api-ms-win-core-winrt): initializing it succeeds;
 * there are no WinRT classes to activate */
K32 HRESULT WINAPI RoInitialize(int type) { (void)type; return S_OK; }
K32 void WINAPI RoUninitialize(void) { }
K32 HRESULT WINAPI RoGetActivationFactory(PVOID cls, REFIID iid, void **f)
{
    (void)cls; (void)iid;
    if (f) *f = 0;
    return 0x80040154;                                    /* REGDB_E_CLASSNOTREG */
}
K32 HRESULT WINAPI RoActivateInstance(PVOID cls, void **inst) { (void)cls; if (inst) *inst = 0; return 0x80040154; }

/* Heaps: one process heap underneath */
K32 SIZE_T WINAPI HeapCompact(HANDLE h, DWORD flags) { (void)h; (void)flags; return 1 << 20; }
K32 BOOL WINAPI HeapValidate(HANDLE h, DWORD flags, LPCVOID p) { (void)h; (void)flags; (void)p; return TRUE; }
K32 BOOL WINAPI HeapSetInformation(HANDLE h, int cls, PVOID info, SIZE_T n) { (void)h; (void)cls; (void)info; (void)n; return TRUE; }
K32 BOOL WINAPI HeapQueryInformation(HANDLE h, int cls, PVOID info, SIZE_T n, PSIZE_T ret)
{
    (void)h;
    if (cls != 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }   /* HeapCompatibilityInformation */
    if (ret) *ret = sizeof(ULONG);
    if (!info || n < sizeof(ULONG)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *(ULONG *)info = 2;                                       /* the low-fragmentation heap */
    return TRUE;
}

/* Preferred UI languages: English (United States).  MUI_LANGUAGE_ID (4)
 * gives "0409", otherwise names ("en-US"); a double-NUL-terminated list */
static BOOL ui_languages(DWORD flags, PULONG count, LPWSTR buf, PULONG len)
{
    static const WCHAR name[] = L"en-US\0", id[] = L"0409\0";
    const WCHAR *l = (flags & 4) ? id : name;
    ULONG need = (flags & 4) ? 6 : 7;
    if (!len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (count) *count = 1;
    if (!buf) { *len = need; return TRUE; }
    if (*len < need) { *len = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, l, need * sizeof(WCHAR));
    *len = need;
    return TRUE;
}
K32 BOOL WINAPI GetUserPreferredUILanguages(DWORD f, PULONG n, LPWSTR b, PULONG l)    { return ui_languages(f, n, b, l); }
K32 BOOL WINAPI GetSystemPreferredUILanguages(DWORD f, PULONG n, LPWSTR b, PULONG l)  { return ui_languages(f, n, b, l); }
K32 BOOL WINAPI GetProcessPreferredUILanguages(DWORD f, PULONG n, LPWSTR b, PULONG l) { return ui_languages(f, n, b, l); }
K32 BOOL WINAPI GetThreadPreferredUILanguages(DWORD f, PULONG n, LPWSTR b, PULONG l)  { return ui_languages(f, n, b, l); }
K32 BOOL WINAPI SetThreadPreferredUILanguages(DWORD f, LPCWSTR b, PULONG n) { (void)f; (void)b; if (n) *n = 1; return TRUE; }
K32 BOOL WINAPI SetProcessPreferredUILanguages(DWORD f, LPCWSTR b, PULONG n) { (void)f; (void)b; if (n) *n = 1; return TRUE; }

/* -----------------------------------------------------------------------
 * For Python (and others)
 * ----------------------------------------------------------------------- */
/* The current thread's stack: [low, high) */
K32 VOID WINAPI GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high)
{
#ifdef _WIN64
    BYTE *t = NtCurrentTebBytes();
    *low = *(ULONG_PTR *)(t + 0x1478);              /* DeallocationStack */
    *high = *(ULONG_PTR *)(t + 0x8);                /* StackBase */
    if (!*low) *low = *(ULONG_PTR *)(t + 0x10);
#else
    BYTE *t = NtCurrentTebBytes();
    *low = *(ULONG_PTR *)(t + 0xE0C);
    *high = *(ULONG_PTR *)(t + 0x4);
    if (!*low) *low = *(ULONG_PTR *)(t + 0x8);
#endif
}

/* SetWaitableTimerEx: the wake context and tolerable delay change nothing */
K32 BOOL WINAPI SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, LPVOID fn, LPVOID arg,
                                   PVOID wake, ULONG delay)
{
    (void)wake; (void)delay;
    return SetWaitableTimer(h, due, period, fn, arg, FALSE);
}

/* CopyFile2: CopyFileW, with an HRESULT; COPY_FILE_FAIL_IF_EXISTS (1) */
typedef struct { DWORD dwSize, dwCopyFlags; BOOL *pfCancel; PVOID pProgressRoutine, pvCallbackContext; } COPYFILE2_PARAMS_;
K32 HRESULT WINAPI CopyFile2(LPCWSTR from, LPCWSTR to, const COPYFILE2_PARAMS_ *p)
{
    BOOL fail_exists = p && (p->dwCopyFlags & 1);
    if (CopyFileW(from, to, fail_exists)) return S_OK;
    return HRESULT_FROM_WIN32(GetLastError());
}

/* Process snapshots (PssCaptureSnapshot): not available */
K32 DWORD WINAPI PssCaptureSnapshot(HANDLE p, DWORD flags, DWORD ctxflags, PVOID *snap) { (void)p; (void)flags; (void)ctxflags; if (snap) *snap = 0; return ERROR_NOT_SUPPORTED; }
K32 DWORD WINAPI PssQuerySnapshot(PVOID snap, int cls, void *buf, DWORD n) { (void)snap; (void)cls; (void)buf; (void)n; return ERROR_NOT_SUPPORTED; }
K32 DWORD WINAPI PssFreeSnapshot(HANDLE p, PVOID snap) { (void)p; (void)snap; return ERROR_SUCCESS; }

/* PathCch (api-ms-win-core-path): paths of up to @cch characters */
static BOOL pcc_sep(WCHAR c) { return c == '\\' || c == '/'; }
/* The length of @p's root: "C:\" 3, "C:" 2, "\\server\share\" ..., "\" 1, none 0 */
static int pcc_root(LPCWSTR p)
{
    if (p[0] && p[1] == ':') return pcc_sep(p[2]) ? 3 : 2;
    if (pcc_sep(p[0]) && pcc_sep(p[1])) {                          /* UNC or \\?\ */
        int i = 2, parts = 0;
        while (p[i] && parts < 2) { if (pcc_sep(p[i])) parts++; i++; }
        return i;
    }
    return pcc_sep(p[0]) ? 1 : 0;
}
K32 HRESULT WINAPI PathCchSkipRoot(LPCWSTR p, LPCWSTR *end)
{
    if (!p || !end) return E_INVALIDARG;
    int r = pcc_root(p);
    if (!r) return E_INVALIDARG;
    *end = p + r;
    return S_OK;
}
/* @b relative to @a (or @b if it is absolute), with "." and ".." resolved */
K32 HRESULT WINAPI PathCchCombineEx(LPWSTR out, SIZE_T cch, LPCWSTR a, LPCWSTR b, ULONG flags)
{
    (void)flags;
    if (!out || !cch) return E_INVALIDARG;
    WCHAR *tmp = HeapAlloc(GetProcessHeap(), 0, 2 * 65536 * sizeof(WCHAR));
    if (!tmp) return E_OUTOFMEMORY;
    WCHAR *res = tmp + 65536;
    SIZE_T n = 0;
    if (b && pcc_root(b) && !(pcc_sep(b[0]) && !pcc_sep(b[1]) && a && a[0] && a[1] == ':')) a = 0;
    if (b && pcc_sep(b[0]) && !pcc_sep(b[1]) && a && a[0] && a[1] == ':') {   /* "\x" on a's drive */
        tmp[n++] = a[0]; tmp[n++] = ':';
        a = 0;
    }
    if (a) for (; *a && n < 32767; a++) tmp[n++] = *a == '/' ? '\\' : *a;
    if (a && n && b && *b && tmp[n - 1] != '\\') tmp[n++] = '\\';
    if (b) for (; *b && n < 65535; b++) tmp[n++] = *b == '/' ? '\\' : *b;
    tmp[n] = 0;
    /* canonicalize after the root */
    int root = pcc_root(tmp);
    SIZE_T o = 0;
    for (int i = 0; i < root; i++) res[o++] = tmp[i];
    SIZE_T seg_start[1024];
    int depth = 0;
    for (SIZE_T i = (SIZE_T)root; i < n;) {
        SIZE_T j = i;
        while (j < n && tmp[j] != '\\') j++;
        SIZE_T len = j - i;
        if (len == 1 && tmp[i] == '.') { }
        else if (len == 2 && tmp[i] == '.' && tmp[i + 1] == '.') {
            if (depth) o = seg_start[--depth];
        } else if (len) {
            if (depth < 1024) seg_start[depth++] = o;
            if (o > (SIZE_T)root && res[o - 1] != '\\') res[o++] = '\\';
            for (SIZE_T k = i; k < j; k++) res[o++] = tmp[k];
        }
        i = j + 1;
    }
    res[o] = 0;
    if (!o) { res[o++] = '\\'; res[o] = 0; }
    HRESULT hr = S_OK;
    if (o + 1 > cch) { out[0] = 0; hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
    else memcpy(out, res, (o + 1) * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, tmp);
    return hr;
}
K32 HRESULT WINAPI PathCchCombine(LPWSTR out, SIZE_T cch, LPCWSTR a, LPCWSTR b) { return PathCchCombineEx(out, cch, a, b, 0); }

/* No thread has I/O that is pending in the kernel on its behalf */
K32 BOOL WINAPI GetThreadIOPendingFlag(HANDLE thread, PBOOL pending)
{
    (void)thread;
    if (!pending) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *pending = FALSE;
    return TRUE;
}

/* The edition: NovaOS answers as Windows 10 Pro */
K32 BOOL WINAPI GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, PDWORD type)
{
    (void)major; (void)minor; (void)sp_major; (void)sp_minor;
    if (!type) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *type = 0x30;                                   /* PRODUCT_PROFESSIONAL */
    return TRUE;
}

/* Walking a heap's blocks: NovaOS's heap does not list them */
K32 BOOL WINAPI HeapWalk(HANDLE heap, LPVOID entry)
{
    (void)heap; (void)entry;
    SetLastError(259 /* ERROR_NO_MORE_ITEMS */);
    return FALSE;
}
K32 BOOL WINAPI HeapLock(HANDLE heap)   { (void)heap; return TRUE; }
K32 BOOL WINAPI HeapUnlock(HANDLE heap) { (void)heap; return TRUE; }

/* -----------------------------------------------------------------------
 * Calls Firefox makes at startup (launcher process, mozglue, xul)
 * ----------------------------------------------------------------------- */
typedef struct {
    DWORD dwSize, dwFileAttributes, dwFileFlags, dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} CREATEFILE2_EXTENDED_PARAMETERS_;
K32 HANDLE WINAPI CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition, CREATEFILE2_EXTENDED_PARAMETERS_ *x)
{
    return CreateFileW(name, access, share, x ? x->lpSecurityAttributes : 0, disposition,
                       x ? x->dwFileAttributes | x->dwFileFlags | x->dwSecurityQosFlags : FILE_ATTRIBUTE_NORMAL,
                       x ? x->hTemplateFile : 0);
}

WINBASEAPI HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
K32 HANDLE WINAPI CreateWaitableTimerExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access)
{
    WCHAR w[MAX_PATH];
    if (name) MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
    return CreateWaitableTimerExW(sa, name ? w : 0, flags, access);
}

WINBASEAPI BOOL WINAPI ReplaceFileW(LPCWSTR repl, LPCWSTR with, LPCWSTR backup, DWORD flags, LPVOID a, LPVOID b);
K32 BOOL WINAPI ReplaceFileA(LPCSTR repl, LPCSTR with, LPCSTR backup, DWORD flags, LPVOID a, LPVOID b)
{
    WCHAR r[MAX_PATH], w[MAX_PATH], k[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, repl, -1, r, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, with, -1, w, MAX_PATH);
    if (backup) MultiByteToWideChar(CP_ACP, 0, backup, -1, k, MAX_PATH);
    return ReplaceFileW(r, w, backup ? k : 0, flags, a, b);
}

/* Packaged (MSIX) apps: NovaOS runs none, so no process has a package */
#define APPMODEL_ERROR_NO_PACKAGE     15700
#define APPMODEL_ERROR_NO_APPLICATION 15703
K32 LONG WINAPI GetCurrentPackageFullName(UINT32 *len, PWSTR name) { (void)name; if (len) *len = 0; return APPMODEL_ERROR_NO_PACKAGE; }
K32 LONG WINAPI GetCurrentPackageId(UINT32 *len, BYTE *buf) { (void)buf; if (len) *len = 0; return APPMODEL_ERROR_NO_PACKAGE; }
K32 LONG WINAPI GetCurrentPackageFamilyName(UINT32 *len, PWSTR name) { (void)name; if (len) *len = 0; return APPMODEL_ERROR_NO_PACKAGE; }
K32 LONG WINAPI GetCurrentPackagePath(UINT32 *len, PWSTR path) { (void)path; if (len) *len = 0; return APPMODEL_ERROR_NO_PACKAGE; }
K32 LONG WINAPI GetCurrentApplicationUserModelId(UINT32 *len, PWSTR id) { (void)id; if (len) *len = 0; return APPMODEL_ERROR_NO_APPLICATION; }
K32 LONG WINAPI GetApplicationUserModelId(HANDLE p, UINT32 *len, PWSTR id) { (void)p; (void)id; if (len) *len = 0; return APPMODEL_ERROR_NO_APPLICATION; }
K32 LONG WINAPI GetPackageFullName(HANDLE p, UINT32 *len, PWSTR name) { (void)p; (void)name; if (len) *len = 0; return APPMODEL_ERROR_NO_PACKAGE; }
K32 LONG WINAPI GetPackageFamilyName(HANDLE p, UINT32 *len, PWSTR name) { (void)p; (void)name; if (len) *len = 0; return APPMODEL_ERROR_NO_PACKAGE; }

/* A package full name is Name_Version_Architecture_ResourceId_PublisherId
 * (the resource id may be empty); its family name is Name_PublisherId */
static int package_parts(PCWSTR full, PCWSTR part[5], int len[5])
{
    int n = 0;
    PCWSTR s = full;
    if (!full) return 0;
    for (PCWSTR p = full;; p++) {
        if (*p == '_' || !*p) {
            if (n == 5) return 0;
            part[n] = s; len[n] = (int)(p - s); n++;
            if (!*p) break;
            s = p + 1;
        }
    }
    if (n != 5 || !len[0] || !len[1] || !len[2] || !len[4]) return 0;
    return 1;
}

K32 LONG WINAPI PackageFamilyNameFromFullName(PCWSTR full, UINT32 *len, PWSTR out)
{
    PCWSTR part[5]; int l[5];
    if (!len || !package_parts(full, part, l)) return ERROR_INVALID_PARAMETER;
    UINT32 need = (UINT32)(l[0] + 1 + l[4] + 1);
    if (!out || *len < need) { *len = need; return ERROR_INSUFFICIENT_BUFFER; }
    memcpy(out, part[0], l[0] * sizeof(WCHAR));
    out[l[0]] = '_';
    memcpy(out + l[0] + 1, part[4], l[4] * sizeof(WCHAR));
    out[need - 1] = 0;
    *len = need;
    return ERROR_SUCCESS;
}

/* PACKAGE_ID, then its strings; the publisher's full name is only known
 * for an installed package (none is), so it is left out */
typedef struct { UINT32 reserved, processorArchitecture; UINT64 version; PWSTR name, publisher, resourceId, publisherId; } PackageId;

K32 LONG WINAPI PackageIdFromFullName(PCWSTR full, UINT32 flags, UINT32 *len, BYTE *buf)
{
    static const struct { const char *s; UINT32 arch; } archs[] = {
        { "x86", 0 }, { "arm", 5 }, { "x64", 9 }, { "neutral", 11 }, { "arm64", 12 }, { "x86a64", 14 } };
    PCWSTR part[5]; int l[5];
    if (!len || !package_parts(full, part, l)) return ERROR_INVALID_PARAMETER;
    if (flags & 0x100 /* PACKAGE_INFORMATION_FULL */) return APPMODEL_ERROR_NO_PACKAGE;
    UINT64 ver = 0; int fields = 0; UINT32 v = 0;
    for (int i = 0; i <= l[1]; i++) {
        WCHAR c = i < l[1] ? part[1][i] : '.';
        if (c == '.') { if (fields == 4 || v > 0xFFFF) return ERROR_INVALID_PARAMETER; ver = ver << 16 | v; v = 0; fields++; }
        else if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        else return ERROR_INVALID_PARAMETER;
    }
    if (fields != 4) return ERROR_INVALID_PARAMETER;
    UINT32 arch = ~0u;
    for (int i = 0; i < 6; i++) {
        int k = 0;
        while (k < l[2] && archs[i].s[k] && (part[2][k] | 32) == archs[i].s[k]) k++;
        if (k == l[2] && !archs[i].s[k]) arch = archs[i].arch;
    }
    if (arch == ~0u) return ERROR_INVALID_PARAMETER;
    UINT32 need = sizeof(PackageId) + (UINT32)(l[0] + 1 + l[3] + 1 + l[4] + 1) * sizeof(WCHAR);
    if (!buf || *len < need) { *len = need; return ERROR_INSUFFICIENT_BUFFER; }
    PackageId *id = (PackageId *)buf;
    WCHAR *w = (WCHAR *)(id + 1);
    memset(id, 0, sizeof *id);
    id->processorArchitecture = arch;
    id->version = ver;
    PWSTR *dst[3] = { &id->name, &id->resourceId, &id->publisherId };
    int which[3] = { 0, 3, 4 };
    for (int i = 0; i < 3; i++) {
        *dst[i] = w;
        memcpy(w, part[which[i]], l[which[i]] * sizeof(WCHAR));
        w[l[which[i]]] = 0;
        w += l[which[i]] + 1;
    }
    *len = need;
    return ERROR_SUCCESS;
}

/* no package is installed, so a family has no members */
K32 LONG WINAPI GetPackagesByPackageFamily(PCWSTR family, UINT32 *count, PWSTR *names, UINT32 *len, WCHAR *buf)
{
    (void)names; (void)buf;
    if (!family || !count || !len) return ERROR_INVALID_PARAMETER;
    *count = 0;
    *len = 0;
    return ERROR_SUCCESS;
}

/* an unpackaged desktop process ends with ExitProcess */
K32 LONG WINAPI AppPolicyGetProcessTerminationMethod(HANDLE token, int *policy)
{
    (void)token;
    if (!policy) return ERROR_INVALID_PARAMETER;
    *policy = 0;                                    /* AppPolicyProcessTerminationMethod_ExitProcess */
    return ERROR_SUCCESS;
}

K32 BOOL WINAPI GetProcessHandleCount(HANDLE p, PDWORD n)
{
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG c = 0;
    NTSTATUS s = NtQueryInformationProcess(p, 20 /* ProcessHandleCount */, &c, sizeof(c), 0);
    if (!NT_SUCCESS(s)) { SetLastError(RtlNtStatusToDosError(s)); return FALSE; }
    *n = c;
    return TRUE;
}

/* The process heap and the (empty) csrss port heap */
ULONG NTAPI RtlGetProcessHeaps(ULONG n, PVOID *heaps);
K32 DWORD WINAPI GetProcessHeaps(DWORD n, PHANDLE heaps) { return RtlGetProcessHeaps(n, heaps); }

K32 DWORD WINAPI GetProcessIdOfThread(HANDLE t)
{
    THREAD_BASIC_INFORMATION tbi;
    if (!NT_SUCCESS(NtQueryInformationThread(t, 0, &tbi, sizeof(tbi), 0))) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return (DWORD)(ULONG_PTR)tbi.ClientId.UniqueProcess;
}

/* Exploit mitigations (DEP, ASLR, CFG, signature and image-load policies):
 * NovaOS enforces none of them, so every policy reads as all-off and
 * setting one is accepted */
K32 BOOL WINAPI GetProcessMitigationPolicy(HANDLE p, int policy, PVOID buf, SIZE_T n)
{
    (void)p; (void)policy;
    if (!buf || !n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(buf, 0, n);
    return TRUE;
}
K32 BOOL WINAPI SetProcessMitigationPolicy(int policy, PVOID buf, SIZE_T n) { (void)policy; (void)buf; (void)n; return TRUE; }
K32 BOOL WINAPI SetProcessInformation(HANDLE p, int cls, LPVOID info, DWORD n) { (void)p; (void)cls; (void)info; (void)n; return TRUE; }
K32 BOOL WINAPI GetProcessInformation(HANDLE p, int cls, LPVOID info, DWORD n)
{
    (void)p; (void)cls;
    if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(info, 0, n);                            /* no memory priority, power throttling or protection */
    return TRUE;
}
/* Control-flow enforcement (shadow stacks) is never on */
K32 BOOL WINAPI IsUserCetAvailableInEnvironment(DWORD ctx) { (void)ctx; return FALSE; }

/* Thread and process information classes (memory priority, power
 * throttling...): scheduling hints NovaOS's scheduler does not take */
K32 BOOL WINAPI SetThreadInformation(HANDLE t, int cls, LPVOID buf, DWORD n) { (void)t; (void)cls; (void)buf; (void)n; return TRUE; }
K32 BOOL WINAPI GetThreadInformation(HANDLE t, int cls, LPVOID buf, DWORD n) { (void)t; (void)cls; if (buf) memset(buf, 0, n); return TRUE; }
K32 BOOL WINAPI SetProcessShutdownParameters(DWORD level, DWORD flags) { (void)level; (void)flags; return TRUE; }
K32 BOOL WINAPI GetProcessShutdownParameters(LPDWORD level, LPDWORD flags)
{
    if (level) *level = 0x280;
    if (flags) *flags = 0;
    return TRUE;
}

/* SYSTEM_CPU_SET_INFORMATION: one CPU set per processor */
typedef struct {
    DWORD Size, Type;
    DWORD Id;
    WORD Group;
    BYTE LogicalProcessorIndex, CoreIndex, LastLevelCacheIndex, NumaNodeIndex, EfficiencyClass, AllFlags;
    DWORD SchedulingClass;
    DWORD64 AllocationTag;
} CPU_SET_INFO_;
K32 BOOL WINAPI GetSystemCpuSetInformation(PVOID info, ULONG len, PULONG ret, HANDLE p, ULONG flags)
{
    (void)p; (void)flags;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONG need = si.dwNumberOfProcessors * (ULONG)sizeof(CPU_SET_INFO_);
    if (ret) *ret = need;
    if (!info || len < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    CPU_SET_INFO_ *c = info;
    memset(c, 0, need);
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
        c[i].Size = sizeof(CPU_SET_INFO_);
        c[i].Type = 0;                              /* CpuSetInformation */
        c[i].Id = 0x100 + i;
        c[i].LogicalProcessorIndex = (BYTE)i;
        c[i].CoreIndex = (BYTE)i;
    }
    return TRUE;
}
K32 BOOL WINAPI GetProcessDefaultCpuSets(HANDLE p, PULONG ids, ULONG n, PULONG need) { (void)p; (void)ids; (void)n; if (need) *need = 0; return TRUE; }
K32 BOOL WINAPI SetProcessDefaultCpuSets(HANDLE p, const ULONG *ids, ULONG n) { (void)p; (void)ids; (void)n; return TRUE; }
K32 BOOL WINAPI SetThreadSelectedCpuSets(HANDLE t, const ULONG *ids, ULONG n) { (void)t; (void)ids; (void)n; return TRUE; }

#ifndef PAGE_WRITECOPY
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE_WRITECOPY 0x80
#endif
#ifndef PAGE_GUARD
#define PAGE_GUARD             0x100
#endif
/* Whether @n bytes at @p are not all readable (committed, not a guard
 * or no-access page) */
static BOOL bad_ptr(const void *p, UINT_PTR n, DWORD ok)
{
    if (!n) return FALSE;
    if (!p) return TRUE;
    const BYTE *a = p, *end = a + n;
    if (end < a) return TRUE;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & ok)) return TRUE;
        a = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return FALSE;
}
K32 BOOL WINAPI IsBadReadPtr(const void *p, UINT_PTR n)
{
    return bad_ptr(p, n, PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}
K32 BOOL WINAPI IsBadWritePtr(void *p, UINT_PTR n)
{
    return bad_ptr(p, n, PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}
K32 BOOL WINAPI IsBadCodePtr(FARPROC p) { return bad_ptr((const void *)p, 1, PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY); }

/* The working set: NovaOS does not track which pages are resident */
K32 BOOL WINAPI K32QueryWorkingSet(HANDLE p, PVOID buf, DWORD n)
{
    (void)p;
    if (!buf || n < sizeof(ULONG_PTR)) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
    *(ULONG_PTR *)buf = 0;
    return TRUE;
}
K32 BOOL WINAPI K32QueryWorkingSetEx(HANDLE p, PVOID buf, DWORD n) { (void)p; (void)buf; (void)n; return TRUE; }

/* Power requests (keep the display or system awake): nothing sleeps on
 * NovaOS by itself, so a request is a handle with nothing behind it */
K32 HANDLE WINAPI PowerCreateRequest(PVOID reason) { (void)reason; return CreateEventW(0, TRUE, FALSE, 0); }
K32 BOOL WINAPI PowerSetRequest(HANDLE h, int type) { (void)type; return h != 0 && h != INVALID_HANDLE_VALUE; }
K32 BOOL WINAPI PowerClearRequest(HANDLE h, int type) { (void)type; return h != 0 && h != INVALID_HANDLE_VALUE; }

/* One session: the console's */
K32 DWORD WINAPI WTSGetActiveConsoleSessionId(void) { return 1; }

/* PathCchCanonicalize(Ex): "." and ".." resolved */
K32 HRESULT WINAPI PathCchCanonicalizeEx(LPWSTR out, SIZE_T cch, LPCWSTR in, ULONG flags)
{
    if (!in) return E_INVALIDARG;
    return PathCchCombineEx(out, cch, in, 0, flags);
}
K32 HRESULT WINAPI PathCchCanonicalize(LPWSTR out, SIZE_T cch, LPCWSTR in) { return PathCchCanonicalizeEx(out, cch, in, 0); }

/* API-set names Firefox imports from kernel32 whose code lives elsewhere */
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:OpenProcessToken=advapi32.OpenProcessToken\"\n\t"
        ".ascii \" /EXPORT:OpenThreadToken=advapi32.OpenThreadToken\"\n\t"
        ".ascii \" /EXPORT:RtlCompareMemory=ntdll.RtlCompareMemory\"\n\t"
        ".ascii \" /EXPORT:GetFileVersionInfoSizeW=version.GetFileVersionInfoSizeW\"\n\t"
        ".ascii \" /EXPORT:GetFileVersionInfoW=version.GetFileVersionInfoW\"\n\t"
        ".ascii \" /EXPORT:GetFileVersionInfoSizeExW=version.GetFileVersionInfoSizeExW\"\n\t"
        ".ascii \" /EXPORT:GetFileVersionInfoExW=version.GetFileVersionInfoExW\"\n\t"
        ".ascii \" /EXPORT:VerQueryValueW=version.VerQueryValueW\"\n\t"
        ".text\n");

/* Where Windows' threads start their routine (ntdll's RtlUserThreadStart
 * calls it); programs look it up to hook thread creation */
K32 VOID WINAPI BaseThreadInitThunk(DWORD unused, LPTHREAD_START_ROUTINE start, LPVOID arg)
{
    (void)unused;
    ExitThread(start(arg));
}

/* DEP is always on for 64-bit processes; 32-bit ones may ask */
K32 BOOL WINAPI SetProcessDEPPolicy(DWORD flags) { (void)flags; return TRUE; }
K32 BOOL WINAPI GetProcessDEPPolicy(HANDLE p, LPDWORD flags, PBOOL permanent)
{
    (void)p;
    if (flags) *flags = 1;                          /* PROCESS_DEP_ENABLE */
    if (permanent) *permanent = TRUE;
    return TRUE;
}

/* AppContainer monikers (kernelbase on Windows; the Chromium sandbox in
 * Firefox binds them with GetProcAddress and stops the browser when one
 * is missing).  NovaOS runs no AppContainers: a registration is kept for
 * this process only, so a moniker registered can be looked up again. */
NTSYSAPI ULONG   NTAPI RtlLengthSid(PSID sid);
NTSYSAPI BOOLEAN NTAPI RtlEqualSid(PSID a, PSID b);
#define AC_MAX 16
static struct { BYTE sid[68]; WCHAR moniker[128]; } g_ac[AC_MAX];
static int g_ac_n;
static CRITICAL_SECTION g_ac_lock;
static INIT_ONCE g_ac_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK ac_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_ac_lock); return TRUE; }

static int ac_find(PSID sid)
{
    for (int i = 0; i < g_ac_n; i++) if (RtlEqualSid(g_ac[i].sid, sid)) return i;
    return -1;
}

K32 HRESULT WINAPI AppContainerRegisterSid(PSID sid, LPCWSTR moniker, LPCWSTR display_name)
{
    (void)display_name;
    ULONG n = sid ? RtlLengthSid(sid) : 0;
    if (!n || n > sizeof(g_ac[0].sid) || !moniker) return E_INVALIDARG;
    InitOnceExecuteOnce(&g_ac_once, ac_init, NULL, NULL);
    EnterCriticalSection(&g_ac_lock);
    int i = ac_find(sid);
    if (i < 0 && g_ac_n < AC_MAX) i = g_ac_n++;
    if (i >= 0) {
        memcpy(g_ac[i].sid, sid, n);
        int k = 0;
        for (; moniker[k] && k < 127; k++) g_ac[i].moniker[k] = moniker[k];
        g_ac[i].moniker[k] = 0;
    }
    LeaveCriticalSection(&g_ac_lock);
    return i >= 0 ? S_OK : E_OUTOFMEMORY;
}

K32 HRESULT WINAPI AppContainerUnregisterSid(PSID sid)
{
    if (!sid) return E_INVALIDARG;
    InitOnceExecuteOnce(&g_ac_once, ac_init, NULL, NULL);
    EnterCriticalSection(&g_ac_lock);
    int i = ac_find(sid);
    if (i >= 0) g_ac[i] = g_ac[--g_ac_n];
    LeaveCriticalSection(&g_ac_lock);
    return S_OK;
}

K32 HRESULT WINAPI AppContainerLookupMoniker(PSID sid, LPWSTR *moniker)
{
    if (!sid || !moniker) return E_INVALIDARG;
    InitOnceExecuteOnce(&g_ac_once, ac_init, NULL, NULL);
    EnterCriticalSection(&g_ac_lock);
    int i = ac_find(sid), k = 0;
    LPWSTR m = i < 0 ? NULL : RtlAllocateHeap(GetProcessHeap(), 0, sizeof(g_ac[0].moniker));
    if (m) { for (; g_ac[i].moniker[k]; k++) m[k] = g_ac[i].moniker[k]; m[k] = 0; }
    LeaveCriticalSection(&g_ac_lock);
    *moniker = m;
    return m ? S_OK : i < 0 ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : E_OUTOFMEMORY;
}

K32 void WINAPI AppContainerFreeMemory(void *p) { if (p) RtlFreeHeap(GetProcessHeap(), 0, p); }

/* kernelbase carries CommandLineToArgvW on Windows 8 and later (programs
 * look for it there before shell32) */
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:CommandLineToArgvW=shell32.CommandLineToArgvW\"\n\t"
        ".text\n");
