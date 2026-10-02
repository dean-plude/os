/*
 * service.c — the service control manager
 *
 * Services live where Windows keeps them, under
 * HKLM\SYSTEM\CurrentControlSet\Services\<name> (Type, Start, ImagePath,
 * DisplayName, Description...). There is no SCM process: each call works
 * on the registry, and a running service's state sits next to its
 * configuration in Nova* values, written by the service's own
 * SetServiceStatus.
 *
 * StartService runs the service's ImagePath with NOVA_SERVICE naming the
 * service. Its StartServiceCtrlDispatcher sees that, starts ServiceMain
 * on a thread and serves controls on the pipe \\.\pipe\NovaService_<name>,
 * where ControlService sends them. A program started any other way gets
 * ERROR_FAILED_SERVICE_CONTROLLER_CONNECT, as on Windows.
 */
#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"
#include <stdbool.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

#define ERROR_SERVICE_REQUEST_TIMEOUT    1053
#define ERROR_INVALID_SERVICE_CONTROL    1052
#define ERROR_SERVICE_ALREADY_RUNNING    1056
#define ERROR_SERVICE_DISABLED           1058
#define ERROR_SERVICE_DOES_NOT_EXIST     1060
#define ERROR_SERVICE_CANNOT_ACCEPT_CTRL 1061
#define ERROR_SERVICE_NOT_ACTIVE         1062
#define ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 1063
#define ERROR_SERVICE_EXISTS             1073
#ifndef ERROR_INVALID_LEVEL
#define ERROR_INVALID_LEVEL              124
#endif

#define SERVICE_STOPPED          1
#define SERVICE_START_PENDING    2
#define SERVICE_STOP_PENDING     3
#define SERVICE_RUNNING          4
#define SERVICE_CONTROL_STOP     1
#define SERVICE_CONTROL_INTERROGATE 4
#define SERVICE_ACCEPT_STOP      1
#define SERVICE_NO_CHANGE        0xFFFFFFFF
#define SERVICE_DISABLED         4
#define SERVICE_KERNEL_DRIVER    1
#define SERVICE_FILE_SYSTEM_DRIVER 2
#define SERVICE_ACTIVE           1
#define SERVICE_INACTIVE         2

typedef void *SC_HANDLE;
typedef void *SERVICE_STATUS_HANDLE;

typedef struct {
    DWORD dwServiceType, dwCurrentState, dwControlsAccepted, dwWin32ExitCode, dwServiceSpecificExitCode,
          dwCheckPoint, dwWaitHint;
} SERVICE_STATUS;

typedef struct {
    DWORD dwServiceType, dwCurrentState, dwControlsAccepted, dwWin32ExitCode, dwServiceSpecificExitCode,
          dwCheckPoint, dwWaitHint, dwProcessId, dwServiceFlags;
} SERVICE_STATUS_PROCESS;

typedef struct {
    DWORD  dwServiceType, dwStartType, dwErrorControl;
    LPWSTR lpBinaryPathName, lpLoadOrderGroup;
    DWORD  dwTagId;
    LPWSTR lpDependencies, lpServiceStartName, lpDisplayName;
} QUERY_SERVICE_CONFIGW;

typedef struct { LPWSTR lpServiceName, lpDisplayName; SERVICE_STATUS_PROCESS ServiceStatusProcess; } ENUM_SERVICE_STATUS_PROCESSW;
typedef struct { LPWSTR lpServiceName, lpDisplayName; SERVICE_STATUS ServiceStatus; } ENUM_SERVICE_STATUSW;

typedef void (WINAPI *SERVICE_MAIN_W)(DWORD argc, LPWSTR *argv);
typedef void (WINAPI *SERVICE_MAIN_A)(DWORD argc, LPSTR *argv);
typedef struct { LPWSTR name; SERVICE_MAIN_W proc; } SERVICE_TABLE_ENTRYW;
typedef struct { LPSTR name; SERVICE_MAIN_A proc; } SERVICE_TABLE_ENTRYA;
typedef void (WINAPI *HANDLER_FN)(DWORD control);
typedef DWORD (WINAPI *HANDLER_EX_FN)(DWORD control, DWORD type, LPVOID data, LPVOID ctx);

#define SERVICES_KEY L"SYSTEM\\CurrentControlSet\\Services\\"
#define PIPE_PREFIX  L"\\\\.\\pipe\\NovaService_"

enum { K_SCM = 1, K_SVC, K_STATUS };

typedef struct {
    DWORD magic;
    DWORD kind;
    WCHAR name[257];
} SvcH;
#define SVC_MAGIC 0x53564348

/* -----------------------------------------------------------------------
 * Small helpers (advapi32 has no C library)
 * ----------------------------------------------------------------------- */
static size_t wlen(const WCHAR *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }

static void wcopy(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t i = 0;
    if (s) for (; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static void wcat(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t n = wlen(d);
    wcopy(d + n, cap - n, s);
}

static void *halloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void hfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static WCHAR *a2w(const char *s)
{
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    WCHAR *w = halloc((size_t)(n + 1) * sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n + 1);
    return w;
}

/* "a\0b\0\0" from the ANSI form */
static WCHAR *a2w_multi(const char *s)
{
    if (!s) return NULL;
    size_t n = 0;
    while (s[n] || s[n + 1]) n++;
    n += 2;
    WCHAR *w = halloc((n + 1) * sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, (int)n, w, (int)n + 1);
    return w;
}

static SvcH *handle(SC_HANDLE h, DWORD kind)
{
    SvcH *s = h;
    if (!s || s->magic != SVC_MAGIC || (kind && s->kind != kind)) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return s;
}

static SvcH *new_handle(DWORD kind, const WCHAR *name)
{
    SvcH *s = halloc(sizeof(SvcH));
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    s->magic = SVC_MAGIC;
    s->kind = kind;
    wcopy(s->name, 257, name);
    return s;
}

static LSTATUS open_key(const WCHAR *name, bool write, HKEY *k)
{
    WCHAR path[400];
    wcopy(path, 400, SERVICES_KEY);
    wcat(path, 400, name);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, write ? KEY_ALL_ACCESS : KEY_READ, k);
}

static DWORD get_dword(HKEY k, const WCHAR *v, DWORD def)
{
    DWORD d = 0, n = sizeof(d), t = 0;
    if (RegQueryValueExW(k, v, NULL, &t, (BYTE *)&d, &n) || t != REG_DWORD) return def;
    return d;
}

static void set_dword(HKEY k, const WCHAR *v, DWORD d) { RegSetValueExW(k, v, 0, REG_DWORD, (const BYTE *)&d, sizeof(d)); }

static void set_str(HKEY k, const WCHAR *v, DWORD type, const WCHAR *s)
{
    RegSetValueExW(k, v, 0, type, (const BYTE *)s, (DWORD)((wlen(s) + 1) * sizeof(WCHAR)));
}

static void set_multi(HKEY k, const WCHAR *v, const WCHAR *s)
{
    size_t n = 0;
    if (s) { while (s[n] || s[n + 1]) n++; n += 2; }
    else n = 0;
    if (!n) { WCHAR z[2] = { 0, 0 }; RegSetValueExW(k, v, 0, REG_MULTI_SZ, (const BYTE *)z, sizeof(z)); return; }
    RegSetValueExW(k, v, 0, REG_MULTI_SZ, (const BYTE *)s, (DWORD)(n * sizeof(WCHAR)));
}

/* A string value (any string type), "" if none; @cap in WCHARs */
static DWORD get_str(HKEY k, const WCHAR *v, WCHAR *out, DWORD cap)
{
    DWORD n = (cap - 2) * sizeof(WCHAR), t = 0;
    out[0] = out[1] = 0;
    if (RegQueryValueExW(k, v, NULL, &t, (BYTE *)out, &n) || (t != REG_SZ && t != REG_EXPAND_SZ && t != REG_MULTI_SZ)) {
        out[0] = out[1] = 0;
        return 0;
    }
    out[n / sizeof(WCHAR)] = 0;
    out[n / sizeof(WCHAR) + 1] = 0;
    return n / sizeof(WCHAR);
}

static bool exists(const WCHAR *name)
{
    HKEY k;
    if (open_key(name, false, &k)) return false;
    bool ok = get_dword(k, L"Type", 0) != 0;
    RegCloseKey(k);
    return ok;
}

/* The running state; a service whose process has gone is stopped */
static void query_status(const WCHAR *name, SERVICE_STATUS_PROCESS *st)
{
    memset(st, 0, sizeof(*st));
    st->dwCurrentState = SERVICE_STOPPED;
    HKEY k;
    if (open_key(name, true, &k)) return;
    st->dwServiceType = get_dword(k, L"Type", 0x10);
    st->dwCurrentState = get_dword(k, L"NovaState", SERVICE_STOPPED);
    st->dwControlsAccepted = get_dword(k, L"NovaControls", 0);
    st->dwWin32ExitCode = get_dword(k, L"NovaExitCode", 0);
    st->dwServiceSpecificExitCode = get_dword(k, L"NovaSpecificExitCode", 0);
    st->dwCheckPoint = get_dword(k, L"NovaCheckPoint", 0);
    st->dwWaitHint = get_dword(k, L"NovaWaitHint", 0);
    st->dwProcessId = get_dword(k, L"NovaPid", 0);
    if (st->dwCurrentState != SERVICE_STOPPED) {
        bool alive = false;
        HANDLE p = st->dwProcessId ? OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, FALSE, st->dwProcessId) : NULL;
        if (p) {
            DWORD code = 0;
            alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
            CloseHandle(p);
        }
        if (!alive) {
            st->dwCurrentState = SERVICE_STOPPED;
            st->dwControlsAccepted = 0;
            st->dwProcessId = 0;
            set_dword(k, L"NovaState", SERVICE_STOPPED);
            set_dword(k, L"NovaPid", 0);
        }
    }
    if (st->dwCurrentState == SERVICE_STOPPED) { st->dwControlsAccepted = 0; st->dwProcessId = 0; }
    RegCloseKey(k);
}

/* -----------------------------------------------------------------------
 * The control manager's side
 * ----------------------------------------------------------------------- */
WINADVAPI SC_HANDLE WINAPI OpenSCManagerW(LPCWSTR machine, LPCWSTR db, DWORD access)
{
    (void)db; (void)access;
    if (machine && machine[0] && !(machine[0] == '\\' && machine[1] == '\\' && machine[2] == '.' && !machine[3])) {
        SetLastError(ERROR_ACCESS_DENIED);       /* other machines */
        return NULL;
    }
    return new_handle(K_SCM, L"");
}

WINADVAPI SC_HANDLE WINAPI OpenSCManagerA(LPCSTR machine, LPCSTR db, DWORD access)
{
    (void)db;
    if (machine && machine[0]) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    return OpenSCManagerW(NULL, NULL, access);
}

WINADVAPI SC_HANDLE WINAPI OpenServiceW(SC_HANDLE scm, LPCWSTR name, DWORD access)
{
    (void)access;
    if (!handle(scm, K_SCM)) return NULL;
    if (!name || !name[0] || wlen(name) > 256) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    if (!exists(name)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return NULL; }
    return new_handle(K_SVC, name);
}

WINADVAPI SC_HANDLE WINAPI OpenServiceA(SC_HANDLE scm, LPCSTR name, DWORD access)
{
    WCHAR *w = a2w(name);
    SC_HANDLE h = OpenServiceW(scm, w, access);
    hfree(w);
    return h;
}

WINADVAPI SC_HANDLE WINAPI CreateServiceW(SC_HANDLE scm, LPCWSTR name, LPCWSTR display, DWORD access, DWORD type, DWORD start,
                                          DWORD errc, LPCWSTR path, LPCWSTR group, LPDWORD tag, LPCWSTR deps, LPCWSTR user, LPCWSTR pw)
{
    (void)access; (void)pw;
    if (!handle(scm, K_SCM)) return NULL;
    if (!name || !name[0] || wlen(name) > 256 || !path || !path[0]) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (exists(name)) { SetLastError(ERROR_SERVICE_EXISTS); return NULL; }
    WCHAR kp[400];
    wcopy(kp, 400, SERVICES_KEY);
    wcat(kp, 400, name);
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kp, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL)) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    set_dword(k, L"Type", type);
    set_dword(k, L"Start", start);
    set_dword(k, L"ErrorControl", errc);
    set_str(k, L"ImagePath", REG_EXPAND_SZ, path);
    set_str(k, L"DisplayName", REG_SZ, display && display[0] ? display : name);
    if (!(type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)))
        set_str(k, L"ObjectName", REG_SZ, user && user[0] ? user : L"LocalSystem");
    if (group && group[0]) set_str(k, L"Group", REG_SZ, group);
    if (deps && deps[0]) set_multi(k, L"DependOnService", deps);
    set_dword(k, L"NovaState", SERVICE_STOPPED);
    RegCloseKey(k);
    if (tag) *tag = 0;
    return new_handle(K_SVC, name);
}

WINADVAPI SC_HANDLE WINAPI CreateServiceA(SC_HANDLE scm, LPCSTR name, LPCSTR display, DWORD access, DWORD type, DWORD start,
                                          DWORD errc, LPCSTR path, LPCSTR group, LPDWORD tag, LPCSTR deps, LPCSTR user, LPCSTR pw)
{
    WCHAR *wn = a2w(name), *wd = a2w(display), *wp = a2w(path), *wg = a2w(group), *wdep = a2w_multi(deps), *wu = a2w(user);
    (void)pw;
    SC_HANDLE h = CreateServiceW(scm, wn, wd, access, type, start, errc, wp, wg, tag, wdep, wu, NULL);
    hfree(wn); hfree(wd); hfree(wp); hfree(wg); hfree(wdep); hfree(wu);
    return h;
}

WINADVAPI BOOL WINAPI CloseServiceHandle(SC_HANDLE h)
{
    SvcH *s = handle(h, 0);
    if (!s || s->kind == K_STATUS) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    s->magic = 0;
    hfree(s);
    return TRUE;
}

WINADVAPI BOOL WINAPI DeleteService(SC_HANDLE h)
{
    SvcH *s = handle(h, K_SVC);
    if (!s) return FALSE;
    WCHAR kp[400];
    wcopy(kp, 400, SERVICES_KEY);
    wcat(kp, 400, s->name);
    if (RegDeleteTreeW(HKEY_LOCAL_MACHINE, kp)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    return TRUE;
}

WINADVAPI BOOL WINAPI ChangeServiceConfigW(SC_HANDLE h, DWORD type, DWORD start, DWORD errc, LPCWSTR path, LPCWSTR group,
                                           LPDWORD tag, LPCWSTR deps, LPCWSTR user, LPCWSTR pw, LPCWSTR display)
{
    (void)pw;
    SvcH *s = handle(h, K_SVC);
    HKEY k;
    if (!s) return FALSE;
    if (open_key(s->name, true, &k)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    if (type != SERVICE_NO_CHANGE) set_dword(k, L"Type", type);
    if (start != SERVICE_NO_CHANGE) set_dword(k, L"Start", start);
    if (errc != SERVICE_NO_CHANGE) set_dword(k, L"ErrorControl", errc);
    if (path) set_str(k, L"ImagePath", REG_EXPAND_SZ, path);
    if (group) set_str(k, L"Group", REG_SZ, group);
    if (deps) set_multi(k, L"DependOnService", deps);
    if (user) set_str(k, L"ObjectName", REG_SZ, user);
    if (display) set_str(k, L"DisplayName", REG_SZ, display);
    if (tag) *tag = 0;
    RegCloseKey(k);
    return TRUE;
}

WINADVAPI BOOL WINAPI ChangeServiceConfigA(SC_HANDLE h, DWORD type, DWORD start, DWORD errc, LPCSTR path, LPCSTR group,
                                           LPDWORD tag, LPCSTR deps, LPCSTR user, LPCSTR pw, LPCSTR display)
{
    WCHAR *wp = a2w(path), *wg = a2w(group), *wdep = a2w_multi(deps), *wu = a2w(user), *wd = a2w(display);
    (void)pw;
    BOOL r = ChangeServiceConfigW(h, type, start, errc, wp, wg, tag, wdep, wu, NULL, wd);
    hfree(wp); hfree(wg); hfree(wdep); hfree(wu); hfree(wd);
    return r;
}

/* SERVICE_CONFIG_DESCRIPTION (1) and SERVICE_CONFIG_DELAYED_AUTO_START_INFO
 * (3) are kept; failure actions and the rest are accepted and ignored */
WINADVAPI BOOL WINAPI ChangeServiceConfig2W(SC_HANDLE h, DWORD level, LPVOID info)
{
    SvcH *s = handle(h, K_SVC);
    HKEY k;
    if (!s) return FALSE;
    if (open_key(s->name, true, &k)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    if (level == 1 && info) {
        LPCWSTR d = *(LPCWSTR *)info;
        if (d && d[0]) set_str(k, L"Description", REG_SZ, d);
        else if (d) RegDeleteValueW(k, L"Description");
    } else if (level == 3 && info) {
        set_dword(k, L"DelayedAutostart", *(BOOL *)info ? 1 : 0);
    }
    RegCloseKey(k);
    return TRUE;
}

WINADVAPI BOOL WINAPI ChangeServiceConfig2A(SC_HANDLE h, DWORD level, LPVOID info)
{
    if (level == 1 && info) {
        WCHAR *w = a2w(*(LPCSTR *)info);
        BOOL r = ChangeServiceConfig2W(h, level, &w);
        hfree(w);
        return r;
    }
    return ChangeServiceConfig2W(h, level, info);
}

WINADVAPI BOOL WINAPI QueryServiceStatusEx(SC_HANDLE h, int level, LPBYTE buf, DWORD n, LPDWORD need)
{
    SvcH *s = handle(h, K_SVC);
    if (!s) return FALSE;
    if (level != 0) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    if (need) *need = sizeof(SERVICE_STATUS_PROCESS);
    if (!buf || n < sizeof(SERVICE_STATUS_PROCESS)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    query_status(s->name, (SERVICE_STATUS_PROCESS *)buf);
    return TRUE;
}

WINADVAPI BOOL WINAPI QueryServiceStatus(SC_HANDLE h, SERVICE_STATUS *st)
{
    SvcH *s = handle(h, K_SVC);
    SERVICE_STATUS_PROCESS p;
    if (!s) return FALSE;
    if (!st) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    query_status(s->name, &p);
    memcpy(st, &p, sizeof(*st));
    return TRUE;
}

/* Strings packed after a structure: @pos advances, NULL when out of room */
static LPWSTR pack(BYTE *base, DWORD cap, DWORD *pos, const WCHAR *s, size_t nchars)
{
    DWORD bytes = (DWORD)(nchars * sizeof(WCHAR));
    DWORD at = *pos;
    *pos += bytes;
    if (!base || *pos > cap) return NULL;
    memcpy(base + at, s, bytes);
    return (LPWSTR)(base + at);
}

WINADVAPI BOOL WINAPI QueryServiceConfigW(SC_HANDLE h, QUERY_SERVICE_CONFIGW *cfg, DWORD n, LPDWORD need)
{
    SvcH *s = handle(h, K_SVC);
    HKEY k;
    if (!s) return FALSE;
    if (open_key(s->name, false, &k)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    WCHAR path[1024], group[256], deps[1024], user[256], disp[512];
    get_str(k, L"ImagePath", path, 1024);
    get_str(k, L"Group", group, 256);
    DWORD nd = get_str(k, L"DependOnService", deps, 1024);
    get_str(k, L"ObjectName", user, 256);
    get_str(k, L"DisplayName", disp, 512);
    DWORD type = get_dword(k, L"Type", 0x10), start = get_dword(k, L"Start", 3), errc = get_dword(k, L"ErrorControl", 1);
    RegCloseKey(k);
    DWORD pos = sizeof(QUERY_SERVICE_CONFIGW);
    size_t dn = nd ? nd : 0;
    while (dn >= 1 && deps[dn - 1] == 0) dn--;
    dn += 2;                                         /* the list's two terminators */
    DWORD total = pos + (DWORD)((wlen(path) + 1 + wlen(group) + 1 + dn + wlen(user) + 1 + wlen(disp) + 1) * sizeof(WCHAR));
    if (need) *need = total;
    if (!cfg || n < total) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    BYTE *b = (BYTE *)cfg;
    cfg->dwServiceType = type;
    cfg->dwStartType = start;
    cfg->dwErrorControl = errc;
    cfg->dwTagId = 0;
    cfg->lpBinaryPathName = pack(b, n, &pos, path, wlen(path) + 1);
    cfg->lpLoadOrderGroup = pack(b, n, &pos, group, wlen(group) + 1);
    cfg->lpDependencies = pack(b, n, &pos, deps, dn);
    cfg->lpServiceStartName = pack(b, n, &pos, user, wlen(user) + 1);
    cfg->lpDisplayName = pack(b, n, &pos, disp, wlen(disp) + 1);
    return TRUE;
}

WINADVAPI BOOL WINAPI QueryServiceConfig2W(SC_HANDLE h, DWORD level, LPBYTE buf, DWORD n, LPDWORD need)
{
    SvcH *s = handle(h, K_SVC);
    HKEY k;
    if (!s) return FALSE;
    if (open_key(s->name, false, &k)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    if (level == 1) {
        WCHAR desc[1024];
        DWORD len = get_str(k, L"Description", desc, 1024);
        RegCloseKey(k);
        DWORD total = sizeof(LPWSTR) + (len ? (DWORD)((wlen(desc) + 1) * sizeof(WCHAR)) : 0);
        if (need) *need = total;
        if (!buf || n < total) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        DWORD pos = sizeof(LPWSTR);
        *(LPWSTR *)buf = len ? pack(buf, n, &pos, desc, wlen(desc) + 1) : NULL;
        return TRUE;
    }
    if (level == 3) {
        DWORD v = get_dword(k, L"DelayedAutostart", 0);
        RegCloseKey(k);
        if (need) *need = sizeof(BOOL);
        if (!buf || n < sizeof(BOOL)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        *(BOOL *)buf = v != 0;
        return TRUE;
    }
    RegCloseKey(k);
    /* failure actions and the rest: an empty structure */
    if (need) *need = 32;
    if (!buf || n < 32) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(buf, 0, 32);
    return TRUE;
}

WINADVAPI BOOL WINAPI GetServiceDisplayNameW(SC_HANDLE scm, LPCWSTR name, LPWSTR out, LPDWORD n)
{
    HKEY k;
    if (!handle(scm, K_SCM)) return FALSE;
    if (!name || open_key(name, false, &k)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    WCHAR d[512];
    get_str(k, L"DisplayName", d, 512);
    RegCloseKey(k);
    if (!d[0]) wcopy(d, 512, name);
    DWORD len = (DWORD)wlen(d);
    if (!out || *n <= len) { *n = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    wcopy(out, *n, d);
    *n = len;
    return TRUE;
}

/* Services of @type in @state, through @add for each */
static void enum_services(DWORD type, DWORD state, void (*add)(void *ctx, const WCHAR *name, const WCHAR *disp, const SERVICE_STATUS_PROCESS *st), void *ctx)
{
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0, KEY_READ, &root)) return;
    WCHAR name[260];
    for (DWORD i = 0; ; i++) {
        DWORD nn = 260;
        if (RegEnumKeyExW(root, i, name, &nn, NULL, NULL, NULL, NULL)) break;
        HKEY k;
        if (open_key(name, false, &k)) continue;
        DWORD t = get_dword(k, L"Type", 0);
        WCHAR disp[512];
        get_str(k, L"DisplayName", disp, 512);
        RegCloseKey(k);
        if (!(t & type)) continue;
        SERVICE_STATUS_PROCESS st;
        query_status(name, &st);
        bool active = st.dwCurrentState != SERVICE_STOPPED;
        if ((active && !(state & SERVICE_ACTIVE)) || (!active && !(state & SERVICE_INACTIVE))) continue;
        add(ctx, name, disp[0] ? disp : name, &st);
    }
    RegCloseKey(root);
}

typedef struct { BYTE *buf; DWORD cap, count, fixed, strings, index, skip; bool ex; } EnumCtx;

static void enum_add(void *p, const WCHAR *name, const WCHAR *disp, const SERVICE_STATUS_PROCESS *st)
{
    EnumCtx *e = p;
    if (e->index++ < e->skip) return;
    DWORD esz = e->ex ? sizeof(ENUM_SERVICE_STATUS_PROCESSW) : sizeof(ENUM_SERVICE_STATUSW);
    e->fixed += esz;
    e->strings += (DWORD)((wlen(name) + 1 + wlen(disp) + 1) * sizeof(WCHAR));
    if (e->buf && e->fixed + e->strings <= e->cap) e->count++;
}

/* The entries go first, then their strings from the end of the buffer */
typedef struct { BYTE *buf; DWORD cap, n, at, end, index, skip; bool ex; } WriteCtx;

static void enum_write(void *p, const WCHAR *name, const WCHAR *disp, const SERVICE_STATUS_PROCESS *st)
{
    WriteCtx *w = p;
    if (w->index++ < w->skip) return;
    DWORD esz = w->ex ? sizeof(ENUM_SERVICE_STATUS_PROCESSW) : sizeof(ENUM_SERVICE_STATUSW);
    DWORD sn = (DWORD)((wlen(name) + 1) * sizeof(WCHAR)), sd = (DWORD)((wlen(disp) + 1) * sizeof(WCHAR));
    if (w->at + esz > w->end - sn - sd) return;
    w->end -= sn;
    memcpy(w->buf + w->end, name, sn);
    LPWSTR pn = (LPWSTR)(w->buf + w->end);
    w->end -= sd;
    memcpy(w->buf + w->end, disp, sd);
    LPWSTR pd = (LPWSTR)(w->buf + w->end);
    if (w->ex) {
        ENUM_SERVICE_STATUS_PROCESSW *e = (ENUM_SERVICE_STATUS_PROCESSW *)(w->buf + w->at);
        e->lpServiceName = pn;
        e->lpDisplayName = pd;
        e->ServiceStatusProcess = *st;
    } else {
        ENUM_SERVICE_STATUSW *e = (ENUM_SERVICE_STATUSW *)(w->buf + w->at);
        e->lpServiceName = pn;
        e->lpDisplayName = pd;
        memcpy(&e->ServiceStatus, st, sizeof(SERVICE_STATUS));
    }
    w->at += esz;
    w->n++;
}

static BOOL enum_common(SC_HANDLE scm, bool ex, DWORD type, DWORD state, LPBYTE buf, DWORD n, LPDWORD need, LPDWORD count, LPDWORD resume)
{
    if (!handle(scm, K_SCM)) return FALSE;
    EnumCtx e;
    memset(&e, 0, sizeof(e));
    e.ex = ex;
    e.skip = resume ? *resume : 0;
    e.buf = NULL;
    enum_services(type, state, enum_add, &e);
    DWORD total = e.fixed + e.strings;
    if (count) *count = 0;
    if (!buf || n < total) {
        if (need) *need = total;
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    WriteCtx w = { buf, n, 0, 0, n, 0, e.skip, ex };
    enum_services(type, state, enum_write, &w);
    if (need) *need = 0;
    if (count) *count = w.n;
    if (resume) *resume = 0;
    return TRUE;
}

WINADVAPI BOOL WINAPI EnumServicesStatusExW(SC_HANDLE scm, int level, DWORD type, DWORD state, LPBYTE buf, DWORD n, LPDWORD need,
                                            LPDWORD count, LPDWORD resume, LPCWSTR group)
{
    (void)group;
    if (level != 0) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    return enum_common(scm, true, type, state, buf, n, need, count, resume);
}

WINADVAPI BOOL WINAPI EnumServicesStatusW(SC_HANDLE scm, DWORD type, DWORD state, LPBYTE buf, DWORD n, LPDWORD need,
                                          LPDWORD count, LPDWORD resume)
{
    return enum_common(scm, false, type, state, buf, n, need, count, resume);
}

static void pipe_name(const WCHAR *svc, WCHAR *out, size_t cap)
{
    wcopy(out, cap, PIPE_PREFIX);
    wcat(out, cap, svc);
}

WINADVAPI BOOL WINAPI StartServiceW(SC_HANDLE h, DWORD argc, LPCWSTR *argv)
{
    SvcH *s = handle(h, K_SVC);
    HKEY k;
    if (!s) return FALSE;
    SERVICE_STATUS_PROCESS st;
    query_status(s->name, &st);
    if (st.dwCurrentState != SERVICE_STOPPED) { SetLastError(ERROR_SERVICE_ALREADY_RUNNING); return FALSE; }
    if (open_key(s->name, true, &k)) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    DWORD type = get_dword(k, L"Type", 0x10);
    if (get_dword(k, L"Start", 3) == SERVICE_DISABLED) { RegCloseKey(k); SetLastError(ERROR_SERVICE_DISABLED); return FALSE; }
    if (type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)) { RegCloseKey(k); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    WCHAR raw[1024], cmd[2048];
    get_str(k, L"ImagePath", raw, 1024);
    if (!ExpandEnvironmentStringsW(raw, cmd, 2048)) wcopy(cmd, 2048, raw);
    /* the start arguments, for ServiceMain */
    WCHAR args[2048];
    size_t pos = 0;
    for (DWORD i = 0; argv && i < argc; i++) {
        size_t n = wlen(argv[i]);
        if (pos + n + 2 >= 2048) break;
        wcopy(args + pos, 2048 - pos, argv[i]);
        pos += n + 1;
    }
    args[pos] = 0;
    args[pos + 1] = 0;
    if (pos) RegSetValueExW(k, L"NovaArgs", 0, REG_MULTI_SZ, (const BYTE *)args, (DWORD)((pos + 1) * sizeof(WCHAR)));
    else RegDeleteValueW(k, L"NovaArgs");
    set_dword(k, L"NovaState", SERVICE_START_PENDING);
    set_dword(k, L"NovaConnected", 0);
    set_dword(k, L"NovaControls", 0);
    set_dword(k, L"NovaExitCode", 0);

    SetEnvironmentVariableW(L"NOVA_SERVICE", s->name);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD err = GetLastError();
    SetEnvironmentVariableW(L"NOVA_SERVICE", NULL);
    if (!ok) {
        set_dword(k, L"NovaState", SERVICE_STOPPED);
        RegCloseKey(k);
        SetLastError(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? err : ERROR_SERVICE_REQUEST_TIMEOUT);
        return FALSE;
    }
    set_dword(k, L"NovaPid", pi.dwProcessId);
    CloseHandle(pi.hThread);
    /* until the service connects to the control manager (30 s, as Windows) */
    BOOL started = FALSE;
    for (int t = 0; t < 600; t++) {
        if (get_dword(k, L"NovaConnected", 0)) { started = TRUE; break; }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
    }
    if (!started) {
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
        set_dword(k, L"NovaState", SERVICE_STOPPED);
        set_dword(k, L"NovaPid", 0);
    }
    CloseHandle(pi.hProcess);
    RegCloseKey(k);
    if (!started) SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
    return started;
}

WINADVAPI BOOL WINAPI StartServiceA(SC_HANDLE h, DWORD argc, LPCSTR *argv)
{
    LPCWSTR wv[32];
    DWORD n = argc > 32 ? 32 : argc;
    for (DWORD i = 0; i < n; i++) wv[i] = a2w(argv[i]);
    BOOL r = StartServiceW(h, n, argv ? wv : NULL);
    for (DWORD i = 0; i < n; i++) hfree((void *)wv[i]);
    return r;
}

WINADVAPI BOOL WINAPI ControlService(SC_HANDLE h, DWORD control, SERVICE_STATUS *out)
{
    SvcH *s = handle(h, K_SVC);
    if (!s) return FALSE;
    SERVICE_STATUS_PROCESS st;
    query_status(s->name, &st);
    if (st.dwCurrentState == SERVICE_STOPPED) { SetLastError(ERROR_SERVICE_NOT_ACTIVE); return FALSE; }
    if (control == SERVICE_CONTROL_STOP && !(st.dwControlsAccepted & SERVICE_ACCEPT_STOP)) {
        if (out) memcpy(out, &st, sizeof(*out));
        SetLastError(ERROR_INVALID_SERVICE_CONTROL);
        return FALSE;
    }
    WCHAR pn[300];
    pipe_name(s->name, pn, 300);
    HANDLE p = INVALID_HANDLE_VALUE;
    for (int t = 0; t < 100 && p == INVALID_HANDLE_VALUE; t++) {
        p = CreateFileW(pn, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) WaitNamedPipeW(pn, 50);
    }
    if (p == INVALID_HANDLE_VALUE) { SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT); return FALSE; }
    DWORD n = 0, result = ERROR_SERVICE_REQUEST_TIMEOUT;
    if (WriteFile(p, &control, sizeof(control), &n, NULL) && n == sizeof(control)) {
        if (!ReadFile(p, &result, sizeof(result), &n, NULL) || n != sizeof(result)) result = ERROR_SERVICE_REQUEST_TIMEOUT;
    }
    CloseHandle(p);
    query_status(s->name, &st);
    if (out) memcpy(out, &st, sizeof(*out));
    if (result) { SetLastError(result); return FALSE; }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * The service's side
 * ----------------------------------------------------------------------- */
static WCHAR g_name[257];
static HANDLER_FN g_handler;
static HANDLER_EX_FN g_handler_ex;
static void *g_handler_ctx;
static HANDLE g_stopped;
static SvcH g_status_handle;
static SERVICE_MAIN_W g_main_w;
static SERVICE_MAIN_A g_main_a;

static DWORD WINAPI control_loop(void *arg)
{
    (void)arg;
    WCHAR pn[300];
    pipe_name(g_name, pn, 300);
    for (;;) {
        HANDLE p = CreateNamedPipeW(pn, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 64, 64, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
        if (!ConnectNamedPipe(p, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(p); continue; }
        DWORD control = 0, n = 0, result;
        if (ReadFile(p, &control, sizeof(control), &n, NULL) && n == sizeof(control)) {
            if (g_handler_ex) result = g_handler_ex(control, 0, NULL, g_handler_ctx);
            else if (g_handler) { g_handler(control); result = 0; }
            else result = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            if (control == SERVICE_CONTROL_INTERROGATE) result = 0;
            WriteFile(p, &result, sizeof(result), &n, NULL);
            FlushFileBuffers(p);
        }
        CloseHandle(p);
    }
    return 0;
}

/* ServiceMain(argc, argv): the service's name, then the start arguments */
static DWORD WINAPI main_thread(void *arg)
{
    (void)arg;
    static WCHAR args[2048];
    static LPWSTR wv[33];
    static LPSTR av[33];
    DWORD argc = 0;
    wv[argc++] = g_name;
    HKEY k;
    if (!open_key(g_name, false, &k)) {
        DWORD n = get_str(k, L"NovaArgs", args, 2048);
        RegCloseKey(k);
        for (DWORD i = 0; i < n && args[i] && argc < 32; ) {
            wv[argc++] = args + i;
            i += (DWORD)wlen(args + i) + 1;
        }
    }
    if (g_main_w) {
        g_main_w(argc, wv);
    } else if (g_main_a) {
        for (DWORD i = 0; i < argc; i++) {
            int n = WideCharToMultiByte(CP_ACP, 0, wv[i], -1, NULL, 0, NULL, NULL);
            av[i] = halloc((size_t)n + 1);
            if (av[i]) WideCharToMultiByte(CP_ACP, 0, wv[i], -1, av[i], n + 1, NULL, NULL);
        }
        g_main_a(argc, av);
    }
    return 0;
}

static BOOL dispatch(SERVICE_MAIN_W mw, SERVICE_MAIN_A ma)
{
    if (!GetEnvironmentVariableW(L"NOVA_SERVICE", g_name, 257) || !g_name[0]) {
        SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
        return FALSE;
    }
    SetEnvironmentVariableW(L"NOVA_SERVICE", NULL);      /* not for the service's own children */
    if (!mw && !ma) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    g_main_w = mw;
    g_main_a = ma;
    g_stopped = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, control_loop, NULL, 0, NULL);
    if (t) CloseHandle(t);
    HKEY k;
    if (!open_key(g_name, true, &k)) {
        set_dword(k, L"NovaPid", GetCurrentProcessId());
        set_dword(k, L"NovaConnected", 1);
        RegCloseKey(k);
    }
    t = CreateThread(NULL, 0, main_thread, NULL, 0, NULL);
    if (!t) return FALSE;
    CloseHandle(t);
    /* until the service reports itself stopped */
    WaitForSingleObject(g_stopped, INFINITE);
    return TRUE;
}

WINADVAPI BOOL WINAPI StartServiceCtrlDispatcherW(const SERVICE_TABLE_ENTRYW *table)
{
    if (!table) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    /* an own-process service runs the table's first entry, whatever its name */
    return dispatch(table[0].proc, NULL);
}

WINADVAPI BOOL WINAPI StartServiceCtrlDispatcherA(const SERVICE_TABLE_ENTRYA *table)
{
    if (!table) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return dispatch(NULL, table[0].proc);
}

static SERVICE_STATUS_HANDLE register_handler(HANDLER_FN fn, HANDLER_EX_FN fnex, void *ctx)
{
    if (!g_name[0]) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return NULL; }
    g_handler = fn;
    g_handler_ex = fnex;
    g_handler_ctx = ctx;
    g_status_handle.magic = SVC_MAGIC;
    g_status_handle.kind = K_STATUS;
    wcopy(g_status_handle.name, 257, g_name);
    return &g_status_handle;
}

WINADVAPI SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerW(LPCWSTR name, HANDLER_FN fn) { (void)name; return register_handler(fn, NULL, NULL); }
WINADVAPI SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerA(LPCSTR name, HANDLER_FN fn) { (void)name; return register_handler(fn, NULL, NULL); }
WINADVAPI SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerExW(LPCWSTR name, HANDLER_EX_FN fn, LPVOID ctx) { (void)name; return register_handler(NULL, fn, ctx); }
WINADVAPI SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerExA(LPCSTR name, HANDLER_EX_FN fn, LPVOID ctx) { (void)name; return register_handler(NULL, fn, ctx); }

WINADVAPI BOOL WINAPI SetServiceStatus(SERVICE_STATUS_HANDLE h, SERVICE_STATUS *st)
{
    SvcH *s = handle(h, K_STATUS);
    HKEY k;
    if (!s) return FALSE;
    if (!st) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_key(s->name, true, &k)) {
        set_dword(k, L"NovaState", st->dwCurrentState);
        set_dword(k, L"NovaControls", st->dwControlsAccepted);
        set_dword(k, L"NovaExitCode", st->dwWin32ExitCode);
        set_dword(k, L"NovaSpecificExitCode", st->dwServiceSpecificExitCode);
        set_dword(k, L"NovaCheckPoint", st->dwCheckPoint);
        set_dword(k, L"NovaWaitHint", st->dwWaitHint);
        set_dword(k, L"NovaPid", st->dwCurrentState == SERVICE_STOPPED ? 0 : GetCurrentProcessId());
        RegCloseKey(k);
    }
    if (st->dwCurrentState == SERVICE_STOPPED && g_stopped) SetEvent(g_stopped);
    return TRUE;
}
