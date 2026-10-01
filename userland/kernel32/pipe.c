/*
 * pipe.c — anonymous and named pipes, handle information
 *
 * Pipes are the kernel's (NtCreateNamedPipeFile, NtFsControlFile).  An
 * anonymous pipe is a named one with a name nobody else knows, its read
 * end the server and its write end a client, as on Windows.  Reads,
 * writes and ConnectNamedPipe on a handle opened with FILE_FLAG_OVERLAPPED
 * can be left pending; see k32_overlapped in extra.c.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);

#define FSCTL_PIPE_DISCONNECT  0x110004
#define FSCTL_PIPE_LISTEN      0x110008
#define FSCTL_PIPE_PEEK        0x11400C
#define FSCTL_PIPE_WAIT        0x110018
#define FSCTL_PIPE_TRANSCEIVE  0x11C017


static LONG g_seq;

/* \Device\NamedPipe\NAME for NtCreateNamedPipeFile */
static BOOL pipe_path(const char *name, NtPath *p)
{
    char full[MAX_PATH + 16];
    int n = (int)strlen(name);
    if (n > MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    memcpy(full, "\\\\.\\pipe\\", 9);
    memcpy(full + 9, name, (SIZE_T)n + 1);
    return nt_path(full, p);
}

static HANDLE create_named(const char *name, DWORD mode, DWORD pmode, DWORD max, DWORD out, DWORD in,
                           DWORD ms, LPSECURITY_ATTRIBUTES sa)
{
    NtPath p;
    if (!pipe_path(name, &p)) return INVALID_HANDLE_VALUE;
    if (sa && sa->bInheritHandle) p.oa.Attributes |= OBJ_INHERIT;
    ACCESS_MASK access = SYNCHRONIZE;
    if (mode & PIPE_ACCESS_INBOUND) access |= GENERIC_READ;
    if (mode & PIPE_ACCESS_OUTBOUND) access |= GENERIC_WRITE;
    if (!(mode & PIPE_ACCESS_DUPLEX)) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    ULONG options = (mode & FILE_FLAG_OVERLAPPED) ? 0 : FILE_SYNCHRONOUS_IO_NONALERT;
    ULONG disposition = (mode & FILE_FLAG_FIRST_PIPE_INSTANCE) ? FILE_CREATE : FILE_OPEN_IF;
    if (!max || max > PIPE_UNLIMITED_INSTANCES) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -(LONGLONG)(ms ? ms : 50) * 10000;
    HANDLE h;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtCreateNamedPipeFile(&h, access, &p.oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE, disposition, options,
                                       (pmode & PIPE_TYPE_MESSAGE) ? 1 : 0, (pmode & PIPE_READMODE_MESSAGE) ? 1 : 0,
                                       (pmode & PIPE_NOWAIT) ? 1 : 0,
                                       max == PIPE_UNLIMITED_INSTANCES ? 0xFFFFFFFF : max, in, out, &timeout);
    if (!NT_SUCCESS(s)) { fail_status(s); return INVALID_HANDLE_VALUE; }
    SetLastError(0);
    return h;
}

WINBASEAPI HANDLE WINAPI CreateNamedPipeA(LPCSTR name, DWORD mode, DWORD pmode, DWORD max, DWORD out, DWORD in, DWORD ms,
                                          LPSECURITY_ATTRIBUTES sa)
{
    const char *n = k32_pipe_name(name);
    if (!n || !*n) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    return create_named(n, mode, pmode, max, out, in, ms, sa);
}

WINBASEAPI HANDLE WINAPI CreateNamedPipeW(LPCWSTR name, DWORD mode, DWORD pmode, DWORD max, DWORD out, DWORD in, DWORD ms,
                                          LPSECURITY_ATTRIBUTES sa)
{
    char n[MAX_PATH * 3];
    if (!wide_to_temp(name, n, sizeof(n))) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    return CreateNamedPipeA(n, mode, pmode, max, out, in, ms, sa);
}

WINBASEAPI BOOL WINAPI CreatePipe(PHANDLE r, PHANDLE w, LPSECURITY_ATTRIBUTES sa, DWORD size)
{
    char name[64];
    LONG seq = InterlockedIncrement(&g_seq);
    static const char hex[] = "0123456789abcdef";
    memcpy(name, "Win32Pipes.", 11);
    DWORD v[2] = { GetCurrentProcessId(), (DWORD)seq };
    int n = 11;
    for (int k = 0; k < 2; k++) {
        for (int i = 7; i >= 0; i--) name[n++] = hex[(v[k] >> (4 * i)) & 15];
        name[n++] = k ? 0 : '.';
    }
    HANDLE rd = create_named(name, PIPE_ACCESS_INBOUND, 0, 1, size, size, 120000, sa);
    if (rd == INVALID_HANDLE_VALUE) return FALSE;
    char full[96];
    memcpy(full, "\\\\.\\pipe\\", 9);
    memcpy(full + 9, name, strlen(name) + 1);
    HANDLE wr = CreateFileA(full, GENERIC_WRITE | 0x80 /* FILE_READ_ATTRIBUTES */, 0, sa, OPEN_EXISTING, 0, 0);
    if (wr == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); CloseHandle(rd); SetLastError(e); return FALSE; }
    *r = rd;
    *w = wr;
    return TRUE;
}

WINBASEAPI BOOL WINAPI ConnectNamedPipe(HANDLE h, LPOVERLAPPED ov)
{
    NTSTATUS s;
    if (ov) {
        s = k32_overlapped_fsctl(h, ov, FSCTL_PIPE_LISTEN, 0, 0, 0, 0);
        if (s == STATUS_PENDING) { SetLastError(ERROR_IO_PENDING); return FALSE; }
    } else {
        IO_STATUS_BLOCK io;
        s = NtFsControlFile(h, 0, 0, 0, &io, FSCTL_PIPE_LISTEN, 0, 0, 0, 0);
    }
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI DisconnectNamedPipe(HANDLE h)
{
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtFsControlFile(h, 0, 0, 0, &io, FSCTL_PIPE_DISCONNECT, 0, 0, 0, 0);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI PeekNamedPipe(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPDWORD avail, LPDWORD left)
{
    DWORD cap = n + 16;
    BYTE small[272], *b = cap <= sizeof(small) ? small : HeapAlloc(GetProcessHeap(), 0, cap);
    if (!b) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtFsControlFile(h, 0, 0, 0, &io, FSCTL_PIPE_PEEK, 0, 0, b, cap);
    if (!NT_SUCCESS(s) && s != STATUS_BUFFER_OVERFLOW) {
        if (b != small) HeapFree(GetProcessHeap(), 0, b);
        return fail_status(s);
    }
    ULONG hdr[4];
    memcpy(hdr, b, 16);
    DWORD got = (DWORD)io.Information > 16 ? (DWORD)io.Information - 16 : 0;
    if (buf && got) memcpy(buf, b + 16, got);
    if (read) *read = got;
    if (avail) *avail = hdr[1];
    if (left) *left = hdr[2] ? hdr[3] - got : 0;
    if (b != small) HeapFree(GetProcessHeap(), 0, b);
    return TRUE;
}

WINBASEAPI BOOL WINAPI WaitNamedPipeA(LPCSTR name, DWORD ms)
{
    const char *n = k32_pipe_name(name);
    if (!n) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    BYTE b[16 + 2 * MAX_PATH];
    memset(b, 0, sizeof(b));
    WCHAR *w = (WCHAR *)(b + 14);
    int k = u2w(n, -1, w, MAX_PATH);
    if (k < 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    LONGLONG t = ms == NMPWAIT_WAIT_FOREVER ? (LONGLONG)0x8000000000000000ULL : -(LONGLONG)ms * 10000;
    ULONG nl = (ULONG)k * 2;
    memcpy(b, &t, 8);
    memcpy(b + 8, &nl, 4);
    b[12] = ms != NMPWAIT_USE_DEFAULT_WAIT;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtFsControlFile(0, 0, 0, 0, &io, FSCTL_PIPE_WAIT, b, 14 + nl, 0, 0);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI WaitNamedPipeW(LPCWSTR name, DWORD ms)
{
    char n[MAX_PATH * 3];
    if (!wide_to_temp(name, n, sizeof(n))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return WaitNamedPipeA(n, ms);
}

WINBASEAPI BOOL WINAPI TransactNamedPipe(HANDLE h, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD read,
                                         LPOVERLAPPED ov)
{
    NTSTATUS s;
    DWORD got;
    if (ov) {
        s = k32_overlapped_fsctl(h, ov, FSCTL_PIPE_TRANSCEIVE, in, in_len, out, out_len);
        if (s == STATUS_PENDING) { SetLastError(ERROR_IO_PENDING); return FALSE; }
        got = (DWORD)ov->InternalHigh;
    } else {
        IO_STATUS_BLOCK io;
        s = NtFsControlFile(h, 0, 0, 0, &io, FSCTL_PIPE_TRANSCEIVE, in, in_len, out, out_len);
        got = (DWORD)io.Information;
    }
    if (read) *read = NT_SUCCESS(s) || s == STATUS_BUFFER_OVERFLOW ? got : 0;
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI CallNamedPipeA(LPCSTR name, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD read, DWORD ms)
{
    HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeA(name, ms)) return FALSE;
        h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) return FALSE;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    BOOL ok = SetNamedPipeHandleState(h, &mode, 0, 0) && TransactNamedPipe(h, in, in_len, out, out_len, read, 0);
    DWORD e = GetLastError();
    CloseHandle(h);
    SetLastError(e);
    return ok;
}

WINBASEAPI BOOL WINAPI CallNamedPipeW(LPCWSTR name, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD read, DWORD ms)
{
    char n[MAX_PATH * 3];
    if (!wide_to_temp(name, n, sizeof(n))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    return CallNamedPipeA(n, in, in_len, out, out_len, read, ms);
}

/* FilePipeLocalInformation: type, configuration, max instances, current
 * instances, inbound quota, read data available, outbound quota, write
 * quota available, state, end */
static BOOL local_info(HANDLE h, ULONG v[10])
{
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtQueryInformationFile(h, &io, v, 40, 24);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI GetNamedPipeInfo(HANDLE h, LPDWORD flags, LPDWORD out, LPDWORD in, LPDWORD max)
{
    ULONG v[10];
    if (!local_info(h, v)) return FALSE;
    if (flags) *flags = (v[9] ? PIPE_SERVER_END : PIPE_CLIENT_END) | (v[0] ? PIPE_TYPE_MESSAGE : 0);
    if (out) *out = v[6];
    if (in) *in = v[4];
    if (max) *max = v[2] == 0xFFFFFFFF ? PIPE_UNLIMITED_INSTANCES : v[2];
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetNamedPipeHandleStateA(HANDLE h, LPDWORD state, LPDWORD inst, LPDWORD count, LPDWORD ms,
                                                LPSTR user, DWORD user_len)
{
    (void)user_len;
    ULONG v[10], m[2];
    IO_STATUS_BLOCK io;
    if (!local_info(h, v)) return FALSE;
    NTSTATUS s = NtQueryInformationFile(h, &io, m, 8, 23);
    if (!NT_SUCCESS(s)) return fail_status(s);
    if (state) *state = (m[0] ? PIPE_READMODE_MESSAGE : 0) | (m[1] ? PIPE_NOWAIT : 0);
    if (inst) *inst = v[3];
    if (count) *count = 0;
    if (ms) *ms = 0;
    if (user) user[0] = 0;
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetNamedPipeHandleStateW(HANDLE h, LPDWORD state, LPDWORD inst, LPDWORD count, LPDWORD ms,
                                                LPWSTR user, DWORD user_len)
{
    if (user && user_len) user[0] = 0;
    return GetNamedPipeHandleStateA(h, state, inst, count, ms, 0, 0);
}

WINBASEAPI BOOL WINAPI SetNamedPipeHandleState(HANDLE h, LPDWORD mode, LPDWORD count, LPDWORD ms)
{
    (void)count; (void)ms;
    if (!mode) return TRUE;
    ULONG m[2] = { (*mode & PIPE_READMODE_MESSAGE) ? 1u : 0u, (*mode & PIPE_NOWAIT) ? 1u : 0u };
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtSetInformationFile(h, &io, m, 8, 23);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI GetNamedPipeClientProcessId(HANDLE h, PULONG pid)
{
    ULONG v[10];
    if (!local_info(h, v)) return FALSE;
    *pid = 0;                                   /* (not kept) */
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetNamedPipeServerProcessId(HANDLE h, PULONG pid)
{
    ULONG v[10];
    if (!local_info(h, v)) return FALSE;
    *pid = 0;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Handle information: the inherit flag
 * ----------------------------------------------------------------------- */
WINBASEAPI BOOL WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags)
{
    if (!(mask & HANDLE_FLAG_INHERIT)) return TRUE;
    BOOLEAN v[2] = { (flags & HANDLE_FLAG_INHERIT) != 0, 0 };
    NTSTATUS s = NtSetInformationObject(h, 4 /* ObjectHandleFlagInformation */, v, 2);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI GetHandleInformation(HANDLE h, LPDWORD flags)
{
    BOOLEAN v[2] = { 0, 0 };
    NTSTATUS s = NtQueryObject(h, 4, v, 2, 0);
    if (!NT_SUCCESS(s)) return fail_status(s);
    *flags = v[0] ? HANDLE_FLAG_INHERIT : 0;
    return TRUE;
}
