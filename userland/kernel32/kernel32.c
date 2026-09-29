/*
 * kernel32.dll — NovaOS Win32 base API on top of ntdll's native API
 *
 * Paths: kernel32 keeps the current directory itself and always hands the
 * kernel absolute NT paths ("\??\C:\dir\file").  "A" functions take
 * UTF-8; "W" functions take UTF-16 (non-ASCII characters are passed on as
 * UTF-8 bytes in names).  Errors go to the TEB's LastErrorValue.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
void *memcpy(void *d, const void *s, size_t n);          /* from ntdll */
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
static void *kmemcpy(void *d, const void *s, SIZE_T n) { return memcpy(d, s, n); }

static PRTL_USER_PROCESS_PARAMETERS params(void) { return RtlGetCurrentPeb()->ProcessParameters; }

static BYTE *teb(void)
{
    BYTE *t;
    __asm__("movq %%gs:0x30, %0" : "=r"(t));
    return t;
}

WINBASEAPI DWORD WINAPI GetLastError(void)       { return *(DWORD *)(teb() + 0x68); }
WINBASEAPI VOID  WINAPI SetLastError(DWORD err)  { *(DWORD *)(teb() + 0x68) = err; }

BOOL fail_status(NTSTATUS s)
{
    SetLastError(RtlNtStatusToDosError(s));
    return FALSE;
}

int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

/* UTF-16 → UTF-8 (n = -1: NUL-terminated); returns bytes written (no NUL) */
int w2u(const WCHAR *w, int n, char *out, int cap)
{
    int o = 0;
    for (int i = 0; n < 0 ? w[i] : i < n; i++) {
        unsigned c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && (n < 0 || i + 1 < n) && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        }
        char tmp[4];
        int k = 0;
        if (c < 0x80) tmp[k++] = (char)c;
        else if (c < 0x800) { tmp[k++] = (char)(0xC0 | c >> 6); tmp[k++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { tmp[k++] = (char)(0xE0 | c >> 12); tmp[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (c & 0x3F)); }
        else { tmp[k++] = (char)(0xF0 | c >> 18); tmp[k++] = (char)(0x80 | ((c >> 12) & 0x3F)); tmp[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (c & 0x3F)); }
        if (out) { if (o + k > cap) return -1; for (int j = 0; j < k; j++) out[o + j] = tmp[j]; }
        o += k;
    }
    return o;
}

/* UTF-8 → UTF-16 (n = -1: NUL-terminated); returns units written (no NUL) */
int u2w(const char *s, int n, WCHAR *out, int cap)
{
    const unsigned char *p = (const unsigned char *)s;
    int o = 0;
    for (int i = 0; n < 0 ? p[i] : i < n; ) {
        unsigned c = p[i], need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; need = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; need = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; need = 3; }
        else { c = 0xFFFD; }
        i++;
        for (unsigned k = 0; k < need; k++, i++) {
            if ((n >= 0 && i >= n) || (p[i] & 0xC0) != 0x80) { c = 0xFFFD; break; }
            c = c << 6 | (p[i] & 0x3F);
        }
        if (c >= 0x10000) {
            if (out) { if (o + 2 > cap) return -1; out[o] = (WCHAR)(0xD800 + ((c - 0x10000) >> 10)); out[o + 1] = (WCHAR)(0xDC00 + ((c - 0x10000) & 0x3FF)); }
            o += 2;
        } else {
            if (out) { if (o + 1 > cap) return -1; out[o] = (WCHAR)c; }
            o++;
        }
    }
    return o;
}

/* -----------------------------------------------------------------------
 * Current directory and full paths
 * ----------------------------------------------------------------------- */
static char g_cwd[MAX_PATH];                 /* "C:\dir" (no trailing '\' except root) */

const char *cwd(void)
{
    if (!g_cwd[0]) {
        UNICODE_STRING *d = &params()->CurrentDirectory.DosPath;
        int n = w2u(d->Buffer, d->Length / 2, g_cwd, MAX_PATH - 1);
        if (n < 0) n = 0;
        g_cwd[n] = 0;
        if (n > 3 && g_cwd[n - 1] == '\\') g_cwd[n - 1] = 0;
        if (!g_cwd[0]) { g_cwd[0] = 'C'; g_cwd[1] = ':'; g_cwd[2] = '\\'; g_cwd[3] = 0; }
    }
    return g_cwd;
}

/* Absolute, normalized "C:\a\b" for @name; 0 on error */
/* "\\?\C:\x", "\??\C:\x" and "\\.\C:\x" all name C:\x */
static const char *skip_prefix(const char *name)
{
    if ((name[0] == '\\' || name[0] == '/') && (name[1] == '\\' || name[1] == '/' || name[1] == '?') &&
        (name[2] == '?' || name[2] == '.') && (name[3] == '\\' || name[3] == '/'))
        return name + 4;
    return name;
}

int full_path(const char *name, char *out, int cap)
{
    char tmp[MAX_PATH * 2];
    int n = 0;
    name = skip_prefix(name);
    if (((name[0] | 0x20) >= 'a' && (name[0] | 0x20) <= 'z') && name[1] == ':') {
        tmp[n++] = (char)(name[0] & ~0x20); tmp[n++] = ':'; tmp[n++] = '\\';
        name += 2;
    } else if (name[0] == '\\' || name[0] == '/') {
        tmp[n++] = 'C'; tmp[n++] = ':'; tmp[n++] = '\\';
    } else {
        const char *c = cwd();
        while (*c && n < (int)sizeof(tmp) - 2) tmp[n++] = *c++;
        if (tmp[n - 1] != '\\') tmp[n++] = '\\';
    }
    while (*name && n < (int)sizeof(tmp) - 1) { tmp[n++] = *name == '/' ? '\\' : *name; name++; }
    tmp[n] = 0;

    /* normalize: collapse separators, resolve "." and ".." */
    int o = 3;
    out[0] = tmp[0]; out[1] = ':'; out[2] = '\\';
    for (int i = 3; tmp[i]; ) {
        while (tmp[i] == '\\') i++;
        if (!tmp[i]) break;
        int s = i;
        while (tmp[i] && tmp[i] != '\\') i++;
        int len = i - s;
        if (len == 1 && tmp[s] == '.') continue;
        if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            if (o > 3) { o--; while (o > 3 && out[o - 1] != '\\') o--; if (o > 3) o--; }
            continue;
        }
        if (o > 3) { if (o >= cap - 1) return 0; out[o++] = '\\'; }
        if (o + len >= cap) return 0;
        for (int k = 0; k < len; k++) out[o++] = tmp[s + k];
    }
    out[o] = 0;
    return o;
}


BOOL nt_path(const char *name, NtPath *p)
{
    char full[MAX_PATH];
    if (!name || !*name) { SetLastError(ERROR_PATH_NOT_FOUND); return FALSE; }
    name = skip_prefix(name);
    /* devices pass through by name */
    const char *dev = 0;
    if (ieq(name, "CONIN$") || ieq(name, "CONOUT$") || ieq(name, "CON")) dev = name;
    if (dev) { int k = 0; while (dev[k] && k < 16) { full[k] = dev[k]; k++; } full[k] = 0; }
    else if (!full_path(name, full, MAX_PATH)) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    p->buf[0] = '\\'; p->buf[1] = '?'; p->buf[2] = '?'; p->buf[3] = '\\';
    int n = u2w(full, -1, p->buf + 4, MAX_PATH);
    if (n < 0) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    p->buf[4 + n] = 0;
    p->us.Buffer = p->buf;
    p->us.Length = (USHORT)(2 * (4 + n));
    p->us.MaximumLength = p->us.Length + 2;
    memset(&p->oa, 0, sizeof(p->oa));
    p->oa.Length = sizeof(p->oa);
    p->oa.ObjectName = &p->us;
    p->oa.Attributes = OBJ_CASE_INSENSITIVE;
    return TRUE;
}

char *wide_to_temp(LPCWSTR w, char *buf, int cap)
{
    if (!w) return 0;
    int n = w2u(w, -1, buf, cap - 1);
    if (n < 0) return 0;
    buf[n] = 0;
    return buf;
}

WINBASEAPI DWORD WINAPI GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    const char *c = cwd();
    DWORD n = (DWORD)strlen(c);
    if (size <= n) return n + 1;
    kmemcpy(buf, c, n + 1);
    return n;
}

WINBASEAPI DWORD WINAPI GetCurrentDirectoryW(DWORD size, LPWSTR buf)
{
    WCHAR tmp[MAX_PATH];
    int n = u2w(cwd(), -1, tmp, MAX_PATH);
    if ((int)size <= n) return (DWORD)n + 1;
    kmemcpy(buf, tmp, 2 * (SIZE_T)n);
    buf[n] = 0;
    return (DWORD)n;
}

WINBASEAPI DWORD WINAPI GetFileAttributesA(LPCSTR name);

WINBASEAPI BOOL WINAPI SetCurrentDirectoryA(LPCSTR path)
{
    char full[MAX_PATH];
    if (!path || !full_path(path, full, MAX_PATH)) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    DWORD a = GetFileAttributesA(full);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { SetLastError(ERROR_PATH_NOT_FOUND); return FALSE; }
    kmemcpy(g_cwd, full, strlen(full) + 1);
    return TRUE;
}

WINBASEAPI DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buf, LPSTR *filepart)
{
    char full[MAX_PATH];
    int n = full_path(name, full, MAX_PATH);
    if (!n) { SetLastError(ERROR_INVALID_NAME); return 0; }
    if (size <= (DWORD)n) return (DWORD)n + 1;
    kmemcpy(buf, full, (SIZE_T)n + 1);
    if (filepart) {
        char *last = buf;
        for (char *c = buf; *c; c++) if (*c == '\\') last = c + 1;
        *filepart = *last ? last : 0;
    }
    return (DWORD)n;
}

/* -----------------------------------------------------------------------
 * Process and environment
 * ----------------------------------------------------------------------- */
WINBASEAPI VOID WINAPI ExitProcess(UINT code)                { RtlExitUserProcess((NTSTATUS)code); }
WINBASEAPI HANDLE WINAPI GetCurrentProcess(void)             { return NtCurrentProcess(); }
WINBASEAPI DWORD WINAPI GetCurrentProcessId(void)            { return *(DWORD *)(teb() + 0x40); }
WINBASEAPI DWORD WINAPI GetCurrentThreadId(void)             { return *(DWORD *)(teb() + 0x48); }

WINBASEAPI BOOL WINAPI TerminateProcess(HANDLE process, UINT code)
{
    if (process == NtCurrentProcess()) RtlExitUserProcess((NTSTATUS)code);
    NTSTATUS s = NtTerminateProcess(process, (NTSTATUS)code);  /* one this process started */
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

static char *g_cmdline_a;

WINBASEAPI LPWSTR WINAPI GetCommandLineW(void) { return params()->CommandLine.Buffer; }

WINBASEAPI LPSTR WINAPI GetCommandLineA(void)
{
    if (!g_cmdline_a) {
        UNICODE_STRING *c = &params()->CommandLine;
        int n = w2u(c->Buffer, c->Length / 2, 0, 0);
        g_cmdline_a = RtlAllocateHeap(RtlGetProcessHeap(), 0, (SIZE_T)n + 1);
        if (!g_cmdline_a) return (LPSTR)"";
        w2u(c->Buffer, c->Length / 2, g_cmdline_a, n);
        g_cmdline_a[n] = 0;
    }
    return g_cmdline_a;
}

WINBASEAPI HMODULE WINAPI GetModuleHandleA(LPCSTR name)
{
    if (!name) return (HMODULE)RtlGetCurrentPeb()->ImageBaseAddress;
    HMODULE m = (HMODULE)LdrNovaGetModuleA(name);
    if (!m) SetLastError(ERROR_MOD_NOT_FOUND);
    return m;
}

WINBASEAPI HMODULE WINAPI GetModuleHandleW(LPCWSTR name)
{
    if (!name) return (HMODULE)RtlGetCurrentPeb()->ImageBaseAddress;
    char n[128];
    int i = 0;
    for (; i < 127 && name[i]; i++) n[i] = (char)name[i];
    n[i] = 0;
    return GetModuleHandleA(n);
}

/* The full path of a loaded module (UTF-8), or 0 */
const char *k32_module_path(HMODULE m, char *tmp)
{
    if (!m || m == RtlGetCurrentPeb()->ImageBaseAddress) {
        UNICODE_STRING *p = &params()->ImagePathName;
        int n = w2u(p->Buffer, p->Length / 2, tmp, MAX_PATH - 1);
        tmp[n < 0 ? 0 : n] = 0;
        return tmp;
    }
    NOVA_LDR_INFO *li = NOVA_LDR_INFO_ADDRESS;
    for (ULONG i = 0; i < li->Count && i < 64; i++)
        if ((HMODULE)(ULONG_PTR)li->Modules[i].Base == m) return li->Modules[i].Path;
    return 0;
}

WINBASEAPI DWORD WINAPI GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD size)
{
    char tmp[MAX_PATH];
    const char *path = k32_module_path(m, tmp);
    if (!path) { SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
    DWORD n = (DWORD)strlen(path);
    if (!size) return 0;
    DWORD k = n < size - 1 ? n : size - 1;
    kmemcpy(buf, path, k);
    buf[k] = 0;
    if (n >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return k;
}

WINBASEAPI DWORD WINAPI GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD size)
{
    char tmp[MAX_PATH];
    WCHAR w[MAX_PATH];
    const char *path = k32_module_path(m, tmp);
    if (!path) { SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
    int n = u2w(path, -1, w, MAX_PATH - 1);
    if (n < 0) n = 0;
    if (!size) return 0;
    DWORD k = (DWORD)n < size - 1 ? (DWORD)n : size - 1;
    kmemcpy(buf, w, 2 * (SIZE_T)k);
    buf[k] = 0;
    if ((DWORD)n >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return k;
}

/* Exports of a module mapped in this process (the image or a system DLL) */
WINBASEAPI FARPROC WINAPI GetProcAddress(HMODULE m, LPCSTR name)
{
    BYTE *b = (BYTE *)m;
    if (!b || b[0] != 'M' || b[1] != 'Z') { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    BYTE *nt = b + *(DWORD *)(b + 0x3C);
    DWORD exp = *(DWORD *)(nt + 24 + 112), exps = *(DWORD *)(nt + 24 + 116);
    if (!exp) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
    BYTE *ed = b + exp;
    DWORD nnames = *(DWORD *)(ed + 24);
    DWORD *funcs = (DWORD *)(b + *(DWORD *)(ed + 28)), *names = (DWORD *)(b + *(DWORD *)(ed + 32));
    WORD *ords = (WORD *)(b + *(DWORD *)(ed + 36));
    DWORD rva = 0;
    if ((ULONG_PTR)name < 0x10000) {
        DWORD idx = (DWORD)(ULONG_PTR)name - *(DWORD *)(ed + 16);
        if (idx < *(DWORD *)(ed + 20)) rva = funcs[idx];
    } else {
        for (DWORD i = 0; i < nnames; i++) {
            const char *n = (const char *)b + names[i], *s = name;
            while (*n && *n == *s) { n++; s++; }
            if (!*n && !*s) { rva = funcs[ords[i]]; break; }
        }
    }
    if (!rva) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
    if (rva >= exp && rva < exp + exps) {                   /* a forwarder: "DLL.Function" */
        const char *fw = (const char *)b + rva, *dot = fw;
        while (*dot && *dot != '.') dot++;
        char dll[64];
        int n = (int)(dot - fw);
        if (!*dot || n > 55) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
        kmemcpy(dll, fw, (SIZE_T)n);
        kmemcpy(dll + n, ".dll", 5);
        HMODULE target = LoadLibraryA(dll);
        if (!target) return 0;
        return GetProcAddress(target, dot + 1);
    }
    return (FARPROC)(b + rva);
}

WINBASEAPI BOOL WINAPI FreeLibrary(HMODULE m) { (void)m; return TRUE; }   /* modules stay mapped */

/* Environment: the process block (UTF-16), plus variables set at run time */
typedef struct EnvVar { struct EnvVar *next; char *name, *value; } EnvVar;
static EnvVar *g_env_set;

static const WCHAR *env_block(void) { return params()->Environment; }

WINBASEAPI DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char *val = 0;
    char tmp[1024];
    for (EnvVar *e = g_env_set; e; e = e->next) if (ieq(e->name, name)) { val = e->value; break; }
    if (!val) {
        for (const WCHAR *w = env_block(); w && *w; ) {
            int len = 0;
            while (w[len]) len++;
            int n = w2u(w, len, tmp, sizeof(tmp) - 1);
            if (n > 0) {
                tmp[n] = 0;
                char *eq = tmp + 1;
                while (*eq && *eq != '=') eq++;
                if (*eq) { *eq = 0; if (ieq(tmp, name)) { val = eq + 1; break; } }
            }
            w += len + 1;
        }
    }
    if (!val || !*val) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    DWORD n = (DWORD)strlen(val);
    if (size <= n) return n + 1;
    kmemcpy(buf, val, (SIZE_T)n + 1);
    return n;
}

WINBASEAPI DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size)
{
    char n[256], v[1024];
    if (!wide_to_temp(name, n, sizeof(n))) return 0;
    DWORD r = GetEnvironmentVariableA(n, v, sizeof(v));
    if (!r || r >= sizeof(v)) return r;
    int w = u2w(v, -1, 0, 0);
    if ((int)size <= w) return (DWORD)w + 1;
    u2w(v, -1, buf, (int)size);
    buf[w] = 0;
    return (DWORD)w;
}

WINBASEAPI BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    PVOID h = RtlGetProcessHeap();
    for (EnvVar *e = g_env_set; e; e = e->next) {
        if (ieq(e->name, name)) {
            SIZE_T n = value ? strlen(value) : 0;
            char *v = RtlAllocateHeap(h, 0, n + 1);
            if (!v) return FALSE;
            kmemcpy(v, value ? value : "", n + 1);
            RtlFreeHeap(h, 0, e->value);
            e->value = v;
            return TRUE;
        }
    }
    EnvVar *e = RtlAllocateHeap(h, 0, sizeof(*e));
    SIZE_T nl = strlen(name), vl = value ? strlen(value) : 0;
    if (!e || !(e->name = RtlAllocateHeap(h, 0, nl + 1)) || !(e->value = RtlAllocateHeap(h, 0, vl + 1)))
        return FALSE;
    kmemcpy(e->name, name, nl + 1);
    kmemcpy(e->value, value ? value : "", vl + 1);
    e->next = g_env_set;
    g_env_set = e;
    return TRUE;
}

WINBASEAPI LPWSTR WINAPI GetEnvironmentStringsW(void) { return (LPWSTR)env_block(); }
WINBASEAPI BOOL   WINAPI FreeEnvironmentStringsW(LPWSTR env) { (void)env; return TRUE; }

WINBASEAPI VOID WINAPI GetStartupInfoA(LPSTARTUPINFOA si)
{
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
    si->hStdInput = params()->StandardInput;
    si->hStdOutput = params()->StandardOutput;
    si->hStdError = params()->StandardError;
}

WINBASEAPI VOID WINAPI GetSystemInfo(LPSYSTEM_INFO si)
{
    memset(si, 0, sizeof(*si));
    si->wProcessorArchitecture = 9;                          /* AMD64 */
    si->dwPageSize = 4096;
    si->lpMinimumApplicationAddress = (LPVOID)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)0x7FFFFFFEFFFFULL;
    DWORD n = *(volatile DWORD *)(ULONG_PTR)0x7FFE03C0;    /* KUSER_SHARED_DATA.ActiveProcessorCount */
    if (!n) n = 1;
    si->dwActiveProcessorMask = n >= 64 ? ~(DWORD_PTR)0 : ((DWORD_PTR)1 << n) - 1;
    si->dwNumberOfProcessors = n;
    si->dwProcessorType = 8664;
    si->dwAllocationGranularity = 65536;
}

WINBASEAPI DWORD WINAPI GetVersion(void) { return 10 | (0 << 8) | (18362u << 16); }

WINBASEAPI BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA vi)
{
    vi->dwMajorVersion = 10;
    vi->dwMinorVersion = 0;
    vi->dwBuildNumber = 18362;
    vi->dwPlatformId = 2;
    vi->szCSDVersion[0] = 0;
    return TRUE;
}

WINBASEAPI BOOL WINAPI GetComputerNameA(LPSTR buf, LPDWORD size)
{
    static const char name[] = "NOVA-PC";
    if (*size < sizeof(name)) { *size = sizeof(name); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    kmemcpy(buf, name, sizeof(name));
    *size = sizeof(name) - 1;
    return TRUE;
}

WINBASEAPI BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }

WINBASEAPI BOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPVOID ov);
WINBASEAPI HANDLE WINAPI GetStdHandle(DWORD which);

WINBASEAPI VOID WINAPI OutputDebugStringA(LPCSTR s)
{
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)strlen(s), &w, 0);
}

/* -----------------------------------------------------------------------
 * Console and handles
 * ----------------------------------------------------------------------- */
static HANDLE g_std[3];
static int    g_std_set[3];

WINBASEAPI HANDLE WINAPI GetStdHandle(DWORD which)
{
    int i = (int)(STD_INPUT_HANDLE - which);                 /* -10 → 0, -11 → 1, -12 → 2 */
    if (i < 0 || i > 2) { SetLastError(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }
    if (g_std_set[i]) return g_std[i];
    PRTL_USER_PROCESS_PARAMETERS p = params();
    return i == 0 ? p->StandardInput : i == 1 ? p->StandardOutput : p->StandardError;
}

WINBASEAPI BOOL WINAPI SetStdHandle(DWORD which, HANDLE h)
{
    int i = (int)(STD_INPUT_HANDLE - which);
    if (i < 0 || i > 2) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    g_std[i] = h;
    g_std_set[i] = 1;
    return TRUE;
}

/* I/O is synchronous here; an OVERLAPPED request completes at once: its
 * offset is used, its status and event are set, and a completion port the
 * handle is bound to gets a packet (see k32_io_done in extra.c). */
WINBASEAPI BOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPVOID ov)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER off, *po = 0;
    OVERLAPPED *o = ov;
    if (o) { off.QuadPart = (LONGLONG)o->Offset | (LONGLONG)o->OffsetHigh << 32; po = &off; }
    NTSTATUS s = NtWriteFile(h, 0, 0, 0, &io, buf, n, po, 0);
    DWORD done = NT_SUCCESS(s) ? (DWORD)io.Information : 0;
    if (written) *written = done;
    if (o) k32_io_done(h, o, s, done);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID ov)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER off, *po = 0;
    OVERLAPPED *o = ov;
    if (o) { off.QuadPart = (LONGLONG)o->Offset | (LONGLONG)o->OffsetHigh << 32; po = &off; }
    NTSTATUS s = NtReadFile(h, 0, 0, 0, &io, buf, n, po, 0);
    if (s == STATUS_END_OF_FILE) {
        if (read) *read = 0;
        if (o) { k32_io_done(h, o, s, 0); SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        return TRUE;                                        /* Win32: EOF is success */
    }
    DWORD done = NT_SUCCESS(s) ? (DWORD)io.Information : 0;
    if (read) *read = done;
    if (o) k32_io_done(h, o, s, done);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI WriteConsoleA(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID r)
{
    (void)r;
    return WriteFile(h, buf, n, written, 0);
}

WINBASEAPI BOOL WINAPI WriteConsoleW(HANDLE h, const VOID *buf, DWORD n, LPDWORD written, LPVOID r)
{
    (void)r;
    char tmp[1024];
    const WCHAR *w = buf;
    DWORD done = 0;
    while (done < n) {
        DWORD chunk = n - done > 256 ? 256 : n - done;
        int k = w2u(w + done, (int)chunk, tmp, sizeof(tmp));
        DWORD wr;
        if (k < 0 || !WriteFile(h, tmp, (DWORD)k, &wr, 0)) return FALSE;
        done += chunk;
    }
    if (written) *written = n;
    return TRUE;
}

WINBASEAPI BOOL WINAPI ReadConsoleA(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPVOID c)
{
    (void)c;
    return ReadFile(h, buf, n, read, 0);
}

WINBASEAPI DWORD WINAPI GetFileType(HANDLE h)
{
    IO_STATUS_BLOCK io;
    FILE_FS_DEVICE_INFORMATION d;
    if (!NT_SUCCESS(NtQueryVolumeInformationFile(h, &io, &d, sizeof(d), FileFsDeviceInformation))) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FILE_TYPE_UNKNOWN;
    }
    return d.DeviceType == 0x50 ? FILE_TYPE_CHAR : FILE_TYPE_DISK;
}

WINBASEAPI BOOL WINAPI GetConsoleMode(HANDLE h, LPDWORD mode)
{
    if (GetFileType(h) != FILE_TYPE_CHAR) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    *mode = 0x7;                                            /* processed, line input, echo */
    return TRUE;
}

WINBASEAPI BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode) { (void)mode; return GetFileType(h) == FILE_TYPE_CHAR; }
WINBASEAPI UINT WINAPI GetConsoleCP(void)             { return CP_UTF8; }
WINBASEAPI UINT WINAPI GetConsoleOutputCP(void)       { return CP_UTF8; }
WINBASEAPI BOOL WINAPI SetConsoleOutputCP(UINT cp)    { (void)cp; return TRUE; }
WINBASEAPI BOOL WINAPI SetConsoleTitleA(LPCSTR title) { (void)title; return TRUE; }

WINBASEAPI BOOL WINAPI CloseHandle(HANDLE h)
{
    if (h == NtCurrentProcess() || h == (HANDLE)(LONG_PTR)-2) return TRUE;   /* pseudo handles */
    if (k32_close_snapshot(h)) return TRUE;                 /* tool-help snapshots (compat.c) */
    k32_forget_handle(h);                   /* file mappings, ports, timers (extra.c) */
    NTSTATUS s = NtClose(h);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

/* -----------------------------------------------------------------------
 * Files
 * ----------------------------------------------------------------------- */
WINBASEAPI HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disposition, DWORD flags, HANDLE templ)
{
    (void)sa; (void)templ;
    NtPath p;
    if (!nt_path(name, &p)) return INVALID_HANDLE_VALUE;
    static const ULONG disp[6] = { 0, FILE_CREATE, FILE_OVERWRITE_IF, FILE_OPEN, FILE_OPEN_IF, FILE_OVERWRITE };
    if (disposition < 1 || disposition > 5) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    ULONG opts = FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT;
    if (flags & FILE_FLAG_DELETE_ON_CLOSE) opts |= FILE_DELETE_ON_CLOSE;
    if (flags & 0x02000000) opts &= ~FILE_NON_DIRECTORY_FILE;  /* FILE_FLAG_BACKUP_SEMANTICS */
    HANDLE h;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtCreateFile(&h, access | SYNCHRONIZE, &p.oa, &io, 0, flags & 0xFFFF, share,
                              disp[disposition], opts, 0, 0);
    if (!NT_SUCCESS(s)) { fail_status(s); return INVALID_HANDLE_VALUE; }
    /* Win32 reports "already existed" for CREATE_ALWAYS / OPEN_ALWAYS */
    SetLastError((disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) && io.Information != 2
                 ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return h;
}

WINBASEAPI HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disposition, DWORD flags, HANDLE templ)
{
    char n[MAX_PATH * 3];
    if (!wide_to_temp(name, n, sizeof(n))) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    return CreateFileA(n, access, share, sa, disposition, flags, templ);
}

WINBASEAPI BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION fi;
    NTSTATUS s = NtQueryInformationFile(h, &io, &fi, sizeof(fi), FileStandardInformation);
    if (!NT_SUCCESS(s)) return fail_status(s);
    *size = fi.EndOfFile;
    return TRUE;
}

WINBASEAPI DWORD WINAPI GetFileSize(HANDLE h, LPDWORD high)
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)sz.HighPart;
    return sz.LowPart;
}

WINBASEAPI BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method)
{
    IO_STATUS_BLOCK io;
    LONGLONG base = 0;
    if (method == FILE_CURRENT) {
        LARGE_INTEGER cur;
        NTSTATUS s = NtQueryInformationFile(h, &io, &cur, sizeof(cur), FilePositionInformation);
        if (!NT_SUCCESS(s)) return fail_status(s);
        base = cur.QuadPart;
    } else if (method == FILE_END) {
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz)) return FALSE;
        base = sz.QuadPart;
    } else if (method != FILE_BEGIN) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = base + dist.QuadPart;
    if (pos.QuadPart < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    NTSTATUS s = NtSetInformationFile(h, &io, &pos, sizeof(pos), FilePositionInformation);
    if (!NT_SUCCESS(s)) return fail_status(s);
    if (newpos) *newpos = pos;
    return TRUE;
}

WINBASEAPI DWORD WINAPI SetFilePointer(HANDLE h, LONG dist, PLONG high, DWORD method)
{
    LARGE_INTEGER d, np;
    d.QuadPart = high ? ((LONGLONG)*high << 32) | (DWORD)dist : dist;
    if (!SetFilePointerEx(h, d, &np, method)) return INVALID_SET_FILE_POINTER;
    if (high) *high = np.HighPart;
    return np.LowPart;
}

WINBASEAPI BOOL WINAPI SetEndOfFile(HANDLE h)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER cur;
    NTSTATUS s = NtQueryInformationFile(h, &io, &cur, sizeof(cur), FilePositionInformation);
    if (NT_SUCCESS(s)) s = NtSetInformationFile(h, &io, &cur, sizeof(cur), FileEndOfFileInformation);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI FlushFileBuffers(HANDLE h) { (void)h; return TRUE; }

static BOOL delete_path(LPCSTR name, BOOL dir)
{
    NtPath p;
    if (!nt_path(name, &p)) return FALSE;
    HANDLE h;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtOpenFile(&h, DELETE | SYNCHRONIZE, &p.oa, &io, 7,
                            dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(s)) return fail_status(s);
    BOOLEAN del = TRUE;
    s = NtSetInformationFile(h, &io, &del, 1, FileDispositionInformation);
    NtClose(h);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI DeleteFileA(LPCSTR name)      { return delete_path(name, FALSE); }
WINBASEAPI BOOL WINAPI RemoveDirectoryA(LPCSTR name) { return delete_path(name, TRUE); }

WINBASEAPI BOOL WINAPI DeleteFileW(LPCWSTR name)
{
    char n[MAX_PATH * 3];
    return wide_to_temp(name, n, sizeof(n)) ? DeleteFileA(n) : FALSE;
}

WINBASEAPI BOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
{
    (void)sa;
    NtPath p;
    if (!nt_path(name, &p)) return FALSE;
    HANDLE h;
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtCreateFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &p.oa, &io, 0, FILE_ATTRIBUTE_NORMAL,
                              3, FILE_CREATE, FILE_DIRECTORY_FILE, 0, 0);
    if (!NT_SUCCESS(s)) return fail_status(s);
    NtClose(h);
    return TRUE;
}

WINBASEAPI BOOL WINAPI CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa)
{
    char n[MAX_PATH * 3];
    return wide_to_temp(name, n, sizeof(n)) ? CreateDirectoryA(n, sa) : FALSE;
}

WINBASEAPI DWORD WINAPI GetFileAttributesA(LPCSTR name)
{
    NtPath p;
    if (!nt_path(name, &p)) return INVALID_FILE_ATTRIBUTES;
    FILE_BASIC_INFORMATION fi;
    NTSTATUS s = NtQueryAttributesFile(&p.oa, &fi);
    if (!NT_SUCCESS(s)) { fail_status(s); return INVALID_FILE_ATTRIBUTES; }
    return fi.FileAttributes;
}

WINBASEAPI DWORD WINAPI GetFileAttributesW(LPCWSTR name)
{
    char n[MAX_PATH * 3];
    return wide_to_temp(name, n, sizeof(n)) ? GetFileAttributesA(n) : INVALID_FILE_ATTRIBUTES;
}

/* FindFirstFile: open the directory, then one NtQueryDirectoryFile per entry */
typedef struct { HANDLE dir; UNICODE_STRING pattern; WCHAR pat[MAX_PATH]; } FindState;

static BOOL find_next(FindState *f, LPWIN32_FIND_DATAA fd, BOOL restart)
{
    BYTE buf[sizeof(FILE_DIRECTORY_INFORMATION) + 2 * MAX_PATH];
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtQueryDirectoryFile(f->dir, 0, 0, 0, &io, buf, sizeof(buf), FileDirectoryInformation,
                                      TRUE, &f->pattern, restart);
    if (!NT_SUCCESS(s)) return fail_status(s == STATUS_NO_MORE_FILES ? s : s);
    FILE_DIRECTORY_INFORMATION *e = (FILE_DIRECTORY_INFORMATION *)buf;
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = e->FileAttributes;
    fd->nFileSizeLow = (DWORD)e->EndOfFile.QuadPart;
    fd->nFileSizeHigh = (DWORD)(e->EndOfFile.QuadPart >> 32);
    int n = w2u(e->FileName, (int)e->FileNameLength / 2, fd->cFileName, MAX_PATH - 1);
    fd->cFileName[n < 0 ? 0 : n] = 0;
    return TRUE;
}

WINBASEAPI HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd)
{
    char full[MAX_PATH];
    if (!full_path(pattern, full, MAX_PATH)) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    char *slash = full + 2;
    for (char *c = full; *c; c++) if (*c == '\\') slash = c;
    char pat[MAX_PATH];
    int k = 0;
    for (char *c = slash + 1; *c && k < MAX_PATH - 1; c++) pat[k++] = *c;
    pat[k] = 0;
    if (slash == full + 2) slash[1] = 0; else *slash = 0;   /* keep "C:\" for the root */
    NtPath p;
    if (!nt_path(full, &p)) return INVALID_HANDLE_VALUE;
    FindState *f = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
    if (!f) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtOpenFile(&f->dir, FILE_LIST_DIRECTORY | SYNCHRONIZE, &p.oa, &io, 7, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(s)) {
        RtlFreeHeap(RtlGetProcessHeap(), 0, f);
        fail_status(s == (NTSTATUS)0xC0000034 ? (NTSTATUS)0xC000003A : s);
        return INVALID_HANDLE_VALUE;
    }
    int n = u2w(pat[0] ? pat : "*", -1, f->pat, MAX_PATH - 1);
    f->pattern.Buffer = f->pat;
    f->pattern.Length = (USHORT)(2 * (n < 0 ? 0 : n));
    f->pattern.MaximumLength = f->pattern.Length;
    if (!find_next(f, fd, TRUE)) {
        DWORD err = GetLastError();
        NtClose(f->dir);
        RtlFreeHeap(RtlGetProcessHeap(), 0, f);
        SetLastError(err == ERROR_NO_MORE_FILES ? ERROR_FILE_NOT_FOUND : err);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

WINBASEAPI BOOL WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) { return find_next(h, fd, FALSE); }

static void find_data_a2w(const WIN32_FIND_DATAA *a, LPWIN32_FIND_DATAW w)
{
    memset(w, 0, sizeof(*w));
    w->dwFileAttributes = a->dwFileAttributes;
    w->nFileSizeLow = a->nFileSizeLow;
    w->nFileSizeHigh = a->nFileSizeHigh;
    int n = u2w(a->cFileName, -1, w->cFileName, MAX_PATH - 1);
    w->cFileName[n < 0 ? 0 : n] = 0;
}

WINBASEAPI HANDLE WINAPI FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd)
{
    char p[MAX_PATH * 3];
    WIN32_FIND_DATAA a;
    if (!wide_to_temp(pattern, p, sizeof(p))) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    HANDLE h = FindFirstFileA(p, &a);
    if (h != INVALID_HANDLE_VALUE) find_data_a2w(&a, fd);
    return h;
}

WINBASEAPI BOOL WINAPI FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd)
{
    WIN32_FIND_DATAA a;
    if (!FindNextFileA(h, &a)) return FALSE;
    find_data_a2w(&a, fd);
    return TRUE;
}

WINBASEAPI BOOL WINAPI FindClose(HANDLE h)
{
    FindState *f = h;
    if (!f || h == INVALID_HANDLE_VALUE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (k32_find_close_stream(h)) return TRUE;              /* FindFirstStreamW (compat.c) */
    NtClose(f->dir);
    RtlFreeHeap(RtlGetProcessHeap(), 0, f);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */
WINBASEAPI LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    PVOID base = addr;
    NTSTATUS s = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, type, protect);
    if (!NT_SUCCESS(s)) { fail_status(s); return 0; }
    return base;
}

WINBASEAPI BOOL WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    PVOID base = addr;
    NTSTATUS s = NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, type);
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI BOOL WINAPI VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, LPDWORD old)
{
    PVOID base = addr;
    ULONG o = 0;
    NTSTATUS s = NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, protect, &o);
    if (old) *old = o;
    return NT_SUCCESS(s) ? TRUE : fail_status(s);
}

WINBASEAPI HANDLE WINAPI GetProcessHeap(void) { return RtlGetProcessHeap(); }
WINBASEAPI HANDLE WINAPI HeapCreate(DWORD f, SIZE_T i, SIZE_T m) { (void)f; (void)i; (void)m; return RtlGetProcessHeap(); }
WINBASEAPI BOOL   WINAPI HeapDestroy(HANDLE heap) { (void)heap; return TRUE; }

WINBASEAPI LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n)
{
    LPVOID p = RtlAllocateHeap(heap, flags, n);
    if (!p) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

WINBASEAPI LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T n)
{
    LPVOID q = RtlReAllocateHeap(heap, flags, p, n);
    if (!q) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return q;
}

WINBASEAPI BOOL   WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID p) { return RtlFreeHeap(heap, flags, p); }
WINBASEAPI SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID p) { return RtlSizeHeap(heap, flags, p); }
WINBASEAPI HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T n)  { return HeapAlloc(GetProcessHeap(), flags & LMEM_ZEROINIT ? HEAP_ZERO_MEMORY : 0, n); }
WINBASEAPI HLOCAL WINAPI LocalFree(HLOCAL p)               { HeapFree(GetProcessHeap(), 0, p); return 0; }
WINBASEAPI HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T n) { return HeapAlloc(GetProcessHeap(), flags & GMEM_ZEROINIT ? HEAP_ZERO_MEMORY : 0, n); }
WINBASEAPI HGLOBAL WINAPI GlobalFree(HGLOBAL p)            { HeapFree(GetProcessHeap(), 0, p); return 0; }

/* -----------------------------------------------------------------------
 * Time (UTC; NovaOS has no time zones yet)
 * ----------------------------------------------------------------------- */
WINBASEAPI VOID WINAPI Sleep(DWORD ms)
{
    LARGE_INTEGER iv;
    iv.QuadPart = -(LONGLONG)ms * 10000;
    if (ms) NtDelayExecution(FALSE, &iv); else NtYieldExecution();
}

WINBASEAPI BOOL WINAPI QueryPerformanceCounter(PLARGE_INTEGER c)   { return NT_SUCCESS(NtQueryPerformanceCounter(c, 0)); }
WINBASEAPI BOOL WINAPI QueryPerformanceFrequency(PLARGE_INTEGER f)
{
    LARGE_INTEGER c;
    return NT_SUCCESS(NtQueryPerformanceCounter(&c, f));
}

WINBASEAPI ULONGLONG WINAPI GetTickCount64(void)
{
    LARGE_INTEGER c, f;
    NtQueryPerformanceCounter(&c, &f);
    if (!f.QuadPart) return 0;
    return (ULONGLONG)(c.QuadPart / f.QuadPart) * 1000 + (ULONGLONG)(c.QuadPart % f.QuadPart) * 1000 / (ULONGLONG)f.QuadPart;
}

WINBASEAPI DWORD WINAPI GetTickCount(void) { return (DWORD)GetTickCount64(); }

WINBASEAPI VOID WINAPI GetSystemTimeAsFileTime(LPFILETIME ft)
{
    LARGE_INTEGER t;
    NtQuerySystemTime(&t);
    ft->dwLowDateTime = t.LowPart;
    ft->dwHighDateTime = (DWORD)t.HighPart;
}

WINBASEAPI BOOL WINAPI FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG t = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    ULONGLONG secs = t / 10000000ULL;
    long long days = (long long)(secs / 86400) - 134774;      /* 1601 → 1970 */
    ULONGLONG rem = secs % 86400;
    st->wMilliseconds = (WORD)((t / 10000) % 1000);
    st->wHour = (WORD)(rem / 3600);
    st->wMinute = (WORD)(rem % 3600 / 60);
    st->wSecond = (WORD)(rem % 60);
    st->wDayOfWeek = (WORD)(((days % 7) + 11) % 7);           /* 1970-01-01: Thursday */
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    st->wYear = (WORD)(yoe + era * 400 + (m <= 2));
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
    return TRUE;
}

WINBASEAPI BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    int y = st->wYear, m = st->wMonth, d = st->wDay;
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468 + 134774;
    ULONGLONG t = ((ULONGLONG)days * 86400 + st->wHour * 3600ULL + st->wMinute * 60ULL + st->wSecond) * 10000000ULL
                  + st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}

WINBASEAPI VOID WINAPI GetSystemTime(LPSYSTEMTIME st)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    FileTimeToSystemTime(&ft, st);
}

WINBASEAPI VOID WINAPI GetLocalTime(LPSYSTEMTIME st) { GetSystemTime(st); }

/* -----------------------------------------------------------------------
 * Strings and code pages (the ANSI code page is UTF-8)
 * ----------------------------------------------------------------------- */
WINBASEAPI UINT WINAPI GetACP(void) { return CP_UTF8; }

WINBASEAPI int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR out, int cap)
{
    (void)cp; (void)flags;
    int len = n < 0 ? (int)strlen(s) + 1 : n;
    int r = u2w(s, len, cap ? out : 0, cap);
    if (r < 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return r;
}

WINBASEAPI int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR s, int n, LPSTR out, int cap,
                                          LPCSTR defchar, LPBOOL used)
{
    (void)cp; (void)flags; (void)defchar;
    if (used) *used = FALSE;
    int len = n;
    if (n < 0) { len = 0; while (s[len]) len++; len++; }
    int r = w2u(s, len, cap ? out : 0, cap);
    if (r < 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return r;
}

WINBASEAPI int WINAPI lstrlenA(LPCSTR s)  { return s ? (int)strlen(s) : 0; }
WINBASEAPI int WINAPI lstrlenW(LPCWSTR s) { int n = 0; if (s) while (s[n]) n++; return n; }
WINBASEAPI LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s) { kmemcpy(d, s, strlen(s) + 1); return d; }

WINBASEAPI int WINAPI lstrcmpA(LPCSTR a, LPCSTR b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a < (unsigned char)*b ? -1 : (unsigned char)*a > (unsigned char)*b;
}

WINBASEAPI int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x < y ? -1 : x > y;
    }
}
