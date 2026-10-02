/*
 * registry.c — the registry API (Reg*) over the kernel's key services.
 *
 * Windows moved these into kernelbase; here kernel32 has them and
 * advapi32 forwards.  The predefined keys map to \Registry paths:
 *   HKEY_LOCAL_MACHINE  \Registry\Machine
 *   HKEY_USERS          \Registry\User
 *   HKEY_CURRENT_USER   \Registry\User\<the user's SID>
 *   HKEY_CLASSES_ROOT   \Registry\Machine\SOFTWARE\Classes
 *   HKEY_CURRENT_CONFIG \Registry\Machine\SYSTEM\CurrentControlSet\Hardware Profiles\Current
 * and are opened once per process.  A/W: values are stored as UTF-16;
 * the A functions convert strings (the ANSI code page is UTF-8).
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include <winreg.h>
#include "k32.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);

#define REG_USER_SID "S-1-5-21-1000-2000-3000-1001"

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void *ralloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), 0, n); }
static void rfree(void *p) { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p); }

static LONG err(NTSTATUS s)
{
    switch ((DWORD)s) {
    case 0: return ERROR_SUCCESS;
    case 0x80000005: return ERROR_MORE_DATA;
    case 0x8000001A: return ERROR_NO_MORE_ITEMS;
    case 0xC0000008: return ERROR_INVALID_HANDLE;
    case 0xC0000023: return ERROR_MORE_DATA;
    case 0xC0000033: return ERROR_INVALID_NAME;
    case 0xC0000034: return ERROR_FILE_NOT_FOUND;
    case 0xC0000035: return ERROR_ALREADY_EXISTS;
    case 0xC000003A: return ERROR_PATH_NOT_FOUND;
    case 0xC0000121: return ERROR_ACCESS_DENIED;
    case 0xC000017C: return ERROR_KEY_DELETED;
    case 0xC0000017: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC000011F: return ERROR_TOO_MANY_OPEN_FILES;
    }
    return (LONG)RtlNtStatusToDosError(s);
}

/* -----------------------------------------------------------------------
 * Predefined keys and opening
 * ----------------------------------------------------------------------- */
static HANDLE g_predef[6];

/* a predefined key's number (0x80000000..): on x64 Windows headers make
 * them sign-extended (0xFFFFFFFF80000002), older code zero-extended */
static ULONG_PTR predef_num(HKEY key)
{
    ULONG_PTR k = (ULONG_PTR)key;
    if (((unsigned long long)k >> 32) == 0xFFFFFFFFu) k &= 0xFFFFFFFFu;
    return k;
}

static const char *predef_path(ULONG_PTR k)
{
    switch (k) {
    case 0x80000000: return "\\Registry\\Machine\\SOFTWARE\\Classes";
    case 0x80000001: return "\\Registry\\User\\" REG_USER_SID;
    case 0x80000002: return "\\Registry\\Machine";
    case 0x80000003: return "\\Registry\\User";
    case 0x80000005: return "\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current";
    }
    return 0;
}

static NTSTATUS open_abs(const char *path, BOOL create, HANDLE *out)
{
    WCHAR w[256];
    int n = u2w(path, -1, w, 255);
    UNICODE_STRING us = { (USHORT)(2 * n), (USHORT)(2 * n), w };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    return create ? NtCreateKey(out, KEY_ALL_ACCESS, &oa, 0, 0, 0, 0) : NtOpenKey(out, KEY_ALL_ACCESS, &oa);
}

/* The kernel handle for @key (predefined ones opened on first use) */
static NTSTATUS handle_of(HKEY key, HANDLE *out)
{
    ULONG_PTR k = predef_num(key);
    if (k < 0x80000000u || k > 0x80000005u) { *out = key; return key ? 0 : (NTSTATUS)0xC0000008; }
    int i = (int)(k - 0x80000000u);
    if (!g_predef[i]) {
        const char *path = predef_path(k);
        if (!path) return (NTSTATUS)0xC0000034;
        HANDLE h;
        NTSTATUS s = open_abs(path, i == 5 || i == 0, &h);      /* HKCR and HKCC exist on demand */
        if (s) return s;
        if (__sync_val_compare_and_swap(&g_predef[i], (HANDLE)0, h)) NtClose(h);
    }
    *out = g_predef[i];
    return 0;
}

static NTSTATUS open_sub(HKEY parent, LPCWSTR sub, BOOL create, DWORD options, HKEY *out, LPDWORD disp)
{
    HANDLE root;
    NTSTATUS s = handle_of(parent, &root);
    if (s) return s;
    int n = sub ? wlen(sub) : 0;
    if (n > 32767) return (NTSTATUS)0xC0000033;
    UNICODE_STRING us = { (USHORT)(2 * n), (USHORT)(2 * n), (WCHAR *)sub };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.RootDirectory = root;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    HANDLE h;
    ULONG d = 0;
    s = create ? NtCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, 0, options, &d) : NtOpenKey(&h, KEY_ALL_ACCESS, &oa);
    if (s) return s;
    *out = h;
    if (disp) *disp = d;
    return 0;
}

static BOOL is_predef(HKEY k) { return predef_num(k) >= 0x80000000u && predef_num(k) <= 0x80000005u; }

/* A UTF-8 name as UTF-16 (heap); *ok = FALSE on failure */
static WCHAR *a2w(LPCSTR s)
{
    if (!s) return 0;
    int n = u2w(s, -1, 0, 0);
    WCHAR *w = ralloc(2 * ((SIZE_T)n + 1));
    if (w) { u2w(s, -1, w, n); w[n] = 0; }
    return w;
}

WINBASEAPI LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD options, REGSAM sam, PHKEY out)
{
    (void)options; (void)sam;
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = 0;
    if ((!sub || !*sub) && is_predef(key)) { HANDLE h; NTSTATUS s = handle_of(key, &h); if (s) return err(s); *out = key; return 0; }
    return err(open_sub(key, sub, FALSE, 0, out, 0));
}

WINBASEAPI LSTATUS WINAPI RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD options, REGSAM sam, PHKEY out)
{
    WCHAR *w = a2w(sub);
    LSTATUS r = RegOpenKeyExW(key, w, options, sam, out);
    rfree(w);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegOpenKeyW(HKEY key, LPCWSTR sub, PHKEY out) { return RegOpenKeyExW(key, sub, 0, KEY_ALL_ACCESS, out); }
WINBASEAPI LSTATUS WINAPI RegOpenKeyA(HKEY key, LPCSTR sub, PHKEY out) { return RegOpenKeyExA(key, sub, 0, KEY_ALL_ACCESS, out); }

WINBASEAPI LSTATUS WINAPI RegCreateKeyExW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR cls, DWORD options, REGSAM sam,
                                          LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disposition)
{
    (void)reserved; (void)cls; (void)sam; (void)sa;
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = 0;
    return err(open_sub(key, sub, TRUE, options, out, disposition));
}

WINBASEAPI LSTATUS WINAPI RegCreateKeyExA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD options, REGSAM sam,
                                          LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disposition)
{
    (void)cls;
    WCHAR *w = a2w(sub);
    LSTATUS r = RegCreateKeyExW(key, w, reserved, 0, options, sam, sa, out, disposition);
    rfree(w);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegCreateKeyW(HKEY key, LPCWSTR sub, PHKEY out) { return RegCreateKeyExW(key, sub, 0, 0, 0, KEY_ALL_ACCESS, 0, out, 0); }
WINBASEAPI LSTATUS WINAPI RegCreateKeyA(HKEY key, LPCSTR sub, PHKEY out) { return RegCreateKeyExA(key, sub, 0, 0, 0, KEY_ALL_ACCESS, 0, out, 0); }

WINBASEAPI LSTATUS WINAPI RegCloseKey(HKEY key)
{
    if (is_predef(key)) return ERROR_SUCCESS;               /* predefined keys stay open */
    if (!key) return ERROR_INVALID_HANDLE;
    return err(NtClose(key));
}

WINBASEAPI LSTATUS WINAPI RegOpenCurrentUser(REGSAM sam, PHKEY out) { return RegOpenKeyExW(HKEY_CURRENT_USER, 0, 0, sam, out); }
WINBASEAPI LSTATUS WINAPI RegOpenUserClassesRoot(HANDLE token, DWORD o, REGSAM sam, PHKEY out) { (void)token; (void)o; return RegOpenKeyExW(HKEY_CLASSES_ROOT, 0, 0, sam, out); }
WINBASEAPI LSTATUS WINAPI RegConnectRegistryW(LPCWSTR machine, HKEY key, PHKEY out)
{
    if (machine && *machine) return 53;                     /* ERROR_BAD_NETPATH: no remote registry */
    *out = key;
    return ERROR_SUCCESS;
}
WINBASEAPI LSTATUS WINAPI RegOverridePredefKey(HKEY key, HKEY with) { (void)key; (void)with; return ERROR_SUCCESS; }
WINBASEAPI LSTATUS WINAPI RegDisablePredefinedCache(void) { return ERROR_SUCCESS; }
WINBASEAPI LSTATUS WINAPI RegDisablePredefinedCacheEx(void) { return ERROR_SUCCESS; }
WINBASEAPI LSTATUS WINAPI RegFlushKey(HKEY key) { HANDLE h; NTSTATUS s = handle_of(key, &h); return err(s ? s : NtFlushKey(h)); }

/* The key @sub below @key, opened (or @key itself when @sub is empty); *close says whether to close it */
static LSTATUS with_sub(HKEY key, LPCWSTR sub, HANDLE *h, BOOL *close)
{
    *close = FALSE;
    if (sub && *sub) {
        HKEY k;
        LSTATUS r = err(open_sub(key, sub, FALSE, 0, &k, 0));
        if (r) return r;
        *h = k;
        *close = TRUE;
        return 0;
    }
    return err(handle_of(key, h));
}

/* -----------------------------------------------------------------------
 * Values
 * ----------------------------------------------------------------------- */
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } PartialInfo;

/* Query @name into a heap buffer: type, data, size */
static LSTATUS query(HANDLE h, LPCWSTR name, DWORD *type, BYTE **data, DWORD *size)
{
    int n = name ? wlen(name) : 0;
    UNICODE_STRING us = { (USHORT)(2 * n), (USHORT)(2 * n), (WCHAR *)name };
    ULONG need = 0, cap = 256;
    for (int tries = 0; tries < 4; tries++) {
        PartialInfo *pi = ralloc(cap);
        if (!pi) return ERROR_NOT_ENOUGH_MEMORY;
        NTSTATUS s = NtQueryValueKey(h, &us, 2, pi, cap, &need);
        if (!s) {
            *type = pi->Type;
            *size = pi->DataLength;
            *data = ralloc(pi->DataLength + 2);
            if (!*data) { rfree(pi); return ERROR_NOT_ENOUGH_MEMORY; }
            memcpy(*data, pi->Data, pi->DataLength);
            (*data)[pi->DataLength] = (*data)[pi->DataLength + 1] = 0;
            rfree(pi);
            return ERROR_SUCCESS;
        }
        rfree(pi);
        if (s != (NTSTATUS)0x80000005 && s != (NTSTATUS)0xC0000023) return err(s);
        cap = need + 16;
    }
    return ERROR_MORE_DATA;
}

static BOOL is_string(DWORD t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }

WINBASEAPI LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD n)
{
    (void)reserved;
    if (data && !n) return ERROR_INVALID_PARAMETER;
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    DWORD t, size;
    BYTE *d;
    LSTATUS r = query(h, name, &t, &d, &size);
    if (r) return r;
    if (type) *type = t;
    if (data && *n < size) { *n = size; rfree(d); return ERROR_MORE_DATA; }
    if (data) memcpy(data, d, size);
    if (n) *n = size;
    rfree(d);
    return ERROR_SUCCESS;
}

/* UTF-16 string data -> UTF-8 (multi-strings keep their NULs) */
static BYTE *to_utf8(const BYTE *d, DWORD size, DWORD *out)
{
    int chars = (int)(size / 2);
    int k = w2u((const WCHAR *)d, chars, 0, 0);
    BYTE *a = ralloc((SIZE_T)k + 2);
    if (!a) return 0;
    w2u((const WCHAR *)d, chars, (char *)a, k);
    a[k] = a[k + 1] = 0;
    *out = (DWORD)k;
    return a;
}

WINBASEAPI LSTATUS WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD n)
{
    (void)reserved;
    if (data && !n) return ERROR_INVALID_PARAMETER;
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    WCHAR *w = a2w(name);
    DWORD t, size;
    BYTE *d;
    LSTATUS r = query(h, w, &t, &d, &size);
    rfree(w);
    if (r) return r;
    if (is_string(t)) {
        DWORD k;
        BYTE *a = to_utf8(d, size, &k);
        rfree(d);
        if (!a) return ERROR_NOT_ENOUGH_MEMORY;
        d = a;
        size = k;
    }
    if (type) *type = t;
    if (data && *n < size) { *n = size; rfree(d); return ERROR_MORE_DATA; }
    if (data) memcpy(data, d, size);
    if (n) *n = size;
    rfree(d);
    return ERROR_SUCCESS;
}

WINBASEAPI LSTATUS WINAPI RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD n)
{
    (void)reserved;
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    int nl = name ? wlen(name) : 0;
    UNICODE_STRING us = { (USHORT)(2 * nl), (USHORT)(2 * nl), (WCHAR *)name };
    return err(NtSetValueKey(h, &us, 0, type, (PVOID)data, data ? n : 0));
}

WINBASEAPI LSTATUS WINAPI RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD n)
{
    WCHAR *w = a2w(name);
    LSTATUS r;
    if (is_string(type) && data) {
        /* the length counts the terminating NUL(s); convert all of it */
        int k = u2w((const char *)data, (int)n, 0, 0);
        WCHAR *v = ralloc(2 * ((SIZE_T)k + 1));
        if (!v) { rfree(w); return ERROR_NOT_ENOUGH_MEMORY; }
        u2w((const char *)data, (int)n, v, k);
        r = RegSetValueExW(key, w, reserved, type, (const BYTE *)v, 2 * (DWORD)k);
        rfree(v);
    } else {
        r = RegSetValueExW(key, w, reserved, type, data, n);
    }
    rfree(w);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegSetKeyValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD type, LPCVOID data, DWORD n)
{
    HKEY k = key;
    if (sub && *sub) { LSTATUS r = RegCreateKeyExW(key, sub, 0, 0, 0, KEY_SET_VALUE, 0, &k, 0); if (r) return r; }
    LSTATUS r = RegSetValueExW(k, name, 0, type, data, n);
    if (k != key) RegCloseKey(k);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegSetKeyValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD type, LPCVOID data, DWORD n)
{
    HKEY k = key;
    if (sub && *sub) { LSTATUS r = RegCreateKeyExA(key, sub, 0, 0, 0, KEY_SET_VALUE, 0, &k, 0); if (r) return r; }
    LSTATUS r = RegSetValueExA(k, name, 0, type, data, n);
    if (k != key) RegCloseKey(k);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegDeleteValueW(HKEY key, LPCWSTR name)
{
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    int nl = name ? wlen(name) : 0;
    UNICODE_STRING us = { (USHORT)(2 * nl), (USHORT)(2 * nl), (WCHAR *)name };
    return err(NtDeleteValueKey(h, &us));
}

WINBASEAPI LSTATUS WINAPI RegDeleteValueA(HKEY key, LPCSTR name) { WCHAR *w = a2w(name); LSTATUS r = RegDeleteValueW(key, w); rfree(w); return r; }

WINBASEAPI LSTATUS WINAPI RegDeleteKeyValueW(HKEY key, LPCWSTR sub, LPCWSTR name)
{
    HANDLE h;
    BOOL close;
    LSTATUS r = with_sub(key, sub, &h, &close);
    if (r) return r;
    r = RegDeleteValueW(h, name);
    if (close) NtClose(h);
    return r;
}

/* RegGetValue: type filtering, string termination, REG_EXPAND_SZ expansion */
static BOOL type_allowed(DWORD t, DWORD flags)
{
    DWORD bit = t == REG_NONE ? RRF_RT_REG_NONE : t == REG_SZ ? RRF_RT_REG_SZ : t == REG_EXPAND_SZ ? RRF_RT_REG_EXPAND_SZ :
                t == REG_BINARY ? RRF_RT_REG_BINARY : t == REG_DWORD ? RRF_RT_REG_DWORD : t == REG_MULTI_SZ ? RRF_RT_REG_MULTI_SZ :
                t == REG_QWORD ? RRF_RT_REG_QWORD : 0;
    return (flags & bit) != 0 || (flags & 0xFFFF) == RRF_RT_ANY;
}

WINBASEAPI LSTATUS WINAPI RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD n)
{
    HANDLE h;
    BOOL close;
    LSTATUS r = with_sub(key, sub, &h, &close);
    if (r) return r;
    DWORD t, size;
    BYTE *d;
    r = query(h, name, &t, &d, &size);
    if (close) NtClose(h);
    if (r) return r;
    if (t == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND)) {
        DWORD need = ExpandEnvironmentStringsW((LPCWSTR)d, 0, 0);
        WCHAR *x = ralloc(2 * (SIZE_T)need + 2);
        if (!x) { rfree(d); return ERROR_NOT_ENOUGH_MEMORY; }
        ExpandEnvironmentStringsW((LPCWSTR)d, x, need);
        rfree(d);
        d = (BYTE *)x;
        size = 2 * need;
        t = REG_SZ;
        if (!(flags & RRF_RT_REG_SZ) && (flags & RRF_RT_REG_EXPAND_SZ)) t = REG_EXPAND_SZ;
    }
    if (!type_allowed(t, flags)) { rfree(d); if (data && (flags & RRF_ZEROONFAILURE) && n) memset(data, 0, *n); return 1630 /* ERROR_UNSUPPORTED_TYPE */; }
    if ((t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ) && (size < 2 || ((WCHAR *)d)[size / 2 - 1])) size += 2;   /* add the NUL */
    if (type) *type = t;
    if (data && (!n || *n < size)) { if (n) *n = size; rfree(d); if (flags & RRF_ZEROONFAILURE) memset(data, 0, 0); return ERROR_MORE_DATA; }
    if (data) memcpy(data, d, size);
    if (n) *n = size;
    rfree(d);
    return ERROR_SUCCESS;
}

WINBASEAPI LSTATUS WINAPI RegGetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD n)
{
    WCHAR *ws = a2w(sub), *wn = a2w(name);
    DWORD t, size = 0;
    LSTATUS r = RegGetValueW(key, ws, wn, flags, &t, 0, &size);
    BYTE *d = r ? 0 : ralloc(size + 2);
    if (!r && !d) r = ERROR_NOT_ENOUGH_MEMORY;
    if (!r) r = RegGetValueW(key, ws, wn, flags, &t, d, &size);
    rfree(ws); rfree(wn);
    if (r) { rfree(d); return r; }
    if (is_string(t)) { DWORD k; BYTE *a = to_utf8(d, size, &k); rfree(d); if (!a) return ERROR_NOT_ENOUGH_MEMORY; d = a; size = k + (k && a[k - 1] ? 1 : 0); }
    if (type) *type = t;
    if (data && (!n || *n < size)) { if (n) *n = size; rfree(d); return ERROR_MORE_DATA; }
    if (data) memcpy(data, d, size);
    if (n) *n = size;
    rfree(d);
    return ERROR_SUCCESS;
}

/* the old default-value functions */
WINBASEAPI LSTATUS WINAPI RegQueryValueW(HKEY key, LPCWSTR sub, LPWSTR data, PLONG n)
{
    DWORD size = n ? (DWORD)*n : 0;
    LSTATUS r = RegGetValueW(key, sub, 0, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, 0, data, &size);
    if (r == ERROR_FILE_NOT_FOUND && data && n && *n >= 2) { data[0] = 0; *n = 2; return 0; }
    if (n) *n = (LONG)size;
    return r;
}
WINBASEAPI LSTATUS WINAPI RegSetValueW(HKEY key, LPCWSTR sub, DWORD type, LPCWSTR data, DWORD n)
{
    (void)n;
    return RegSetKeyValueW(key, sub, 0, type, data, 2 * ((DWORD)wlen(data) + 1));
}

/* -----------------------------------------------------------------------
 * Keys: enumeration, information, deletion
 * ----------------------------------------------------------------------- */
typedef struct { LARGE_INTEGER LastWriteTime; ULONG TitleIndex, NameLength; WCHAR Name[1]; } BasicKeyInfo;
typedef struct { ULONG TitleIndex, Type, NameLength; WCHAR Name[1]; } BasicValueInfo;
typedef struct { LARGE_INTEGER LastWriteTime; ULONG TitleIndex, ClassOffset, ClassLength, SubKeys, MaxNameLen, MaxClassLen, Values,
                 MaxValueNameLen, MaxValueDataLen; } FullKeyInfo;

WINBASEAPI LSTATUS WINAPI RegEnumKeyExW(HKEY key, DWORD i, LPWSTR name, LPDWORD n, LPDWORD reserved, LPWSTR cls, LPDWORD ncls, PFILETIME t)
{
    (void)reserved;
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    BYTE buf[16 + 2 * 256];
    ULONG got;
    s = NtEnumerateKey(h, i, 0, buf, sizeof(buf), &got);
    if (s) return err(s);
    BasicKeyInfo *b = (BasicKeyInfo *)buf;
    DWORD len = b->NameLength / 2;
    if (!name || *n <= len) { if (n) *n = len + 1; return ERROR_MORE_DATA; }
    memcpy(name, b->Name, 2 * (SIZE_T)len);
    name[len] = 0;
    *n = len;
    if (cls && ncls && *ncls) cls[0] = 0;
    if (ncls) *ncls = 0;
    if (t) { t->dwLowDateTime = b->LastWriteTime.LowPart; t->dwHighDateTime = (DWORD)b->LastWriteTime.HighPart; }
    return ERROR_SUCCESS;
}

WINBASEAPI LSTATUS WINAPI RegEnumKeyExA(HKEY key, DWORD i, LPSTR name, LPDWORD n, LPDWORD reserved, LPSTR cls, LPDWORD ncls, PFILETIME t)
{
    WCHAR w[256];
    DWORD wn = 256;
    LSTATUS r = RegEnumKeyExW(key, i, w, &wn, reserved, 0, 0, t);
    if (r) return r;
    int k = w2u(w, (int)wn, 0, 0);
    if (!name || *n <= (DWORD)k) { *n = (DWORD)k + 1; return ERROR_MORE_DATA; }
    w2u(w, (int)wn, name, k);
    name[k] = 0;
    *n = (DWORD)k;
    if (cls && ncls && *ncls) cls[0] = 0;
    if (ncls) *ncls = 0;
    return ERROR_SUCCESS;
}

WINBASEAPI LSTATUS WINAPI RegEnumKeyW(HKEY key, DWORD i, LPWSTR name, DWORD n) { return RegEnumKeyExW(key, i, name, &n, 0, 0, 0, 0); }
WINBASEAPI LSTATUS WINAPI RegEnumKeyA(HKEY key, DWORD i, LPSTR name, DWORD n) { return RegEnumKeyExA(key, i, name, &n, 0, 0, 0, 0); }

WINBASEAPI LSTATUS WINAPI RegEnumValueW(HKEY key, DWORD i, LPWSTR name, LPDWORD n, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD nd)
{
    (void)reserved;
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    ULONG cap = 12 + 2 * 16384, got;
    BasicValueInfo *b = ralloc(cap);
    if (!b) return ERROR_NOT_ENOUGH_MEMORY;
    s = NtEnumerateValueKey(h, i, 0, b, cap, &got);
    if (s) { rfree(b); return err(s); }
    DWORD len = b->NameLength / 2;
    if (!name || *n <= len) { *n = len + 1; rfree(b); return ERROR_MORE_DATA; }
    memcpy(name, b->Name, 2 * (SIZE_T)len);
    name[len] = 0;
    *n = len;
    LSTATUS r = ERROR_SUCCESS;
    if (type || data || nd) r = RegQueryValueExW(key, name, 0, type, data, nd);
    rfree(b);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegEnumValueA(HKEY key, DWORD i, LPSTR name, LPDWORD n, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD nd)
{
    WCHAR *w = ralloc(2 * 16384);
    if (!w) return ERROR_NOT_ENOUGH_MEMORY;
    DWORD wn = 16384;
    LSTATUS r = RegEnumValueW(key, i, w, &wn, reserved, 0, 0, 0);
    if (r) { rfree(w); return r; }
    int k = w2u(w, (int)wn, 0, 0);
    if (!name || *n <= (DWORD)k) { *n = (DWORD)k + 1; rfree(w); return ERROR_MORE_DATA; }
    w2u(w, (int)wn, name, k);
    name[k] = 0;
    *n = (DWORD)k;
    rfree(w);
    if (type || data || nd) r = RegQueryValueExA(key, name, 0, type, data, nd);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegQueryInfoKeyW(HKEY key, LPWSTR cls, LPDWORD ncls, LPDWORD reserved, LPDWORD subkeys, LPDWORD maxsub,
                                           LPDWORD maxcls, LPDWORD values, LPDWORD maxvname, LPDWORD maxvdata, LPDWORD sd, PFILETIME t)
{
    (void)reserved;
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    FullKeyInfo fi;
    ULONG got;
    s = NtQueryKey(h, 2, &fi, sizeof(fi), &got);
    if (s) return err(s);
    if (cls && ncls && *ncls) cls[0] = 0;
    if (ncls) *ncls = 0;
    if (subkeys) *subkeys = fi.SubKeys;
    if (maxsub) *maxsub = fi.MaxNameLen / 2;
    if (maxcls) *maxcls = 0;
    if (values) *values = fi.Values;
    if (maxvname) *maxvname = fi.MaxValueNameLen / 2;
    if (maxvdata) *maxvdata = fi.MaxValueDataLen;
    if (sd) *sd = 0;
    if (t) { t->dwLowDateTime = fi.LastWriteTime.LowPart; t->dwHighDateTime = (DWORD)fi.LastWriteTime.HighPart; }
    return ERROR_SUCCESS;
}

WINBASEAPI LSTATUS WINAPI RegQueryInfoKeyA(HKEY key, LPSTR cls, LPDWORD ncls, LPDWORD reserved, LPDWORD subkeys, LPDWORD maxsub,
                                           LPDWORD maxcls, LPDWORD values, LPDWORD maxvname, LPDWORD maxvdata, LPDWORD sd, PFILETIME t)
{
    if (cls && ncls && *ncls) cls[0] = 0;
    return RegQueryInfoKeyW(key, 0, ncls, reserved, subkeys, maxsub, maxcls, values, maxvname, maxvdata, sd, t);
}

WINBASEAPI LSTATUS WINAPI RegDeleteKeyExW(HKEY key, LPCWSTR sub, REGSAM sam, DWORD reserved)
{
    (void)sam; (void)reserved;
    if (!sub) return ERROR_INVALID_PARAMETER;
    HKEY k;
    LSTATUS r = err(open_sub(key, sub, FALSE, 0, &k, 0));
    if (r) return r;
    r = err(NtDeleteKey(k));
    NtClose(k);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegDeleteKeyW(HKEY key, LPCWSTR sub) { return RegDeleteKeyExW(key, sub, 0, 0); }
WINBASEAPI LSTATUS WINAPI RegDeleteKeyA(HKEY key, LPCSTR sub) { WCHAR *w = a2w(sub); LSTATUS r = RegDeleteKeyExW(key, w, 0, 0); rfree(w); return r; }
WINBASEAPI LSTATUS WINAPI RegDeleteKeyExA(HKEY key, LPCSTR sub, REGSAM sam, DWORD reserved) { WCHAR *w = a2w(sub); LSTATUS r = RegDeleteKeyExW(key, w, sam, reserved); rfree(w); return r; }

/* Delete the subkeys and values of @sub (and @sub itself when named) */
WINBASEAPI LSTATUS WINAPI RegDeleteTreeW(HKEY key, LPCWSTR sub)
{
    HANDLE h;
    BOOL close;
    LSTATUS r = with_sub(key, sub, &h, &close);
    if (r) return r;
    WCHAR name[256];
    for (;;) {
        DWORD n = 256;
        r = RegEnumKeyExW(h, 0, name, &n, 0, 0, 0, 0);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r) goto out;
        r = RegDeleteTreeW(h, name);            /* removes the subkey itself too */
        if (r) goto out;
    }
    for (;;) {
        DWORD n = 256;
        r = RegEnumValueW(h, 0, name, &n, 0, 0, 0, 0);
        if (r == ERROR_NO_MORE_ITEMS) { r = 0; break; }
        if (r) goto out;
        r = RegDeleteValueW(h, name);
        if (r) goto out;
    }
out:
    if (close) {
        NtClose(h);
        if (!r) r = RegDeleteKeyW(key, sub);
    }
    return r;
}

WINBASEAPI LSTATUS WINAPI RegDeleteTreeA(HKEY key, LPCSTR sub) { WCHAR *w = a2w(sub); LSTATUS r = RegDeleteTreeW(key, w); rfree(w); return r; }

/* Copy @sub of @from (values and subkeys) into @to */
WINBASEAPI LSTATUS WINAPI RegCopyTreeW(HKEY from, LPCWSTR sub, HKEY to)
{
    HANDLE h;
    BOOL close;
    LSTATUS r = with_sub(from, sub, &h, &close);
    if (r) return r;
    WCHAR *name = ralloc(2 * 16384);
    if (!name) { if (close) NtClose(h); return ERROR_NOT_ENOUGH_MEMORY; }
    for (DWORD i = 0;; i++) {
        DWORD n = 16384, t, size = 0;
        r = RegEnumValueW(h, i, name, &n, 0, &t, 0, &size);
        if (r == ERROR_NO_MORE_ITEMS) { r = 0; break; }
        if (r) break;
        BYTE *d = ralloc(size + 2);
        if (!d) { r = ERROR_NOT_ENOUGH_MEMORY; break; }
        r = RegQueryValueExW(h, name, 0, &t, d, &size);
        if (!r) r = RegSetValueExW(to, name, 0, t, d, size);
        rfree(d);
        if (r) break;
    }
    for (DWORD i = 0; !r; i++) {
        DWORD n = 256;
        r = RegEnumKeyExW(h, i, name, &n, 0, 0, 0, 0);
        if (r == ERROR_NO_MORE_ITEMS) { r = 0; break; }
        if (r) break;
        HKEY dst;
        r = RegCreateKeyExW(to, name, 0, 0, 0, KEY_ALL_ACCESS, 0, &dst, 0);
        if (r) break;
        r = RegCopyTreeW(h, name, dst);
        RegCloseKey(dst);
    }
    rfree(name);
    if (close) NtClose(h);
    return r;
}

WINBASEAPI LSTATUS WINAPI RegRenameKey(HKEY key, LPCWSTR sub, LPCWSTR newname)
{
    HANDLE h;
    BOOL close;
    LSTATUS r = with_sub(key, sub, &h, &close);
    if (r) return r;
    int n = wlen(newname);
    UNICODE_STRING us = { (USHORT)(2 * n), (USHORT)(2 * n), (WCHAR *)newname };
    r = err(NtRenameKey(h, &us));
    if (close) NtClose(h);
    return r;
}

/* Change notification: @ev is signalled once, when @filter's kind of
 * change happens to the key (or with @subtree below it); without @async
 * the call returns only then */
NTSYSAPI NTSTATUS NTAPI NtNotifyChangeKey(HANDLE key, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, ULONG filter,
                                          BOOLEAN tree, PVOID buf, ULONG len, BOOLEAN async);
WINBASEAPI LSTATUS WINAPI RegNotifyChangeKeyValue(HKEY key, BOOL subtree, DWORD filter, HANDLE ev, BOOL async)
{
    HANDLE h;
    NTSTATUS s = handle_of(key, &h);
    if (s) return err(s);
    if (async && !ev) return ERROR_INVALID_PARAMETER;
    IO_STATUS_BLOCK io;
    s = NtNotifyChangeKey(h, async ? ev : 0, 0, 0, &io, filter, (BOOLEAN)(subtree != 0), 0, 0, (BOOLEAN)(async != 0));
    return s == 0x103 /* STATUS_PENDING */ ? ERROR_SUCCESS : err(s);
}

WINBASEAPI LSTATUS WINAPI RegGetKeySecurity(HKEY key, DWORD si, PVOID sd, LPDWORD n)
{
    (void)key; (void)si;
    if (!sd || *n < 20) { *n = 20; return ERROR_INSUFFICIENT_BUFFER; }
    memset(sd, 0, 20);
    ((BYTE *)sd)[0] = 1;                                    /* self-relative, no owner, no DACL */
    ((BYTE *)sd)[3] = 0x80;
    *n = 20;
    return ERROR_SUCCESS;
}
WINBASEAPI LSTATUS WINAPI RegSetKeySecurity(HKEY key, DWORD si, PVOID sd) { (void)key; (void)si; (void)sd; return ERROR_SUCCESS; }
WINBASEAPI LSTATUS WINAPI RegLoadKeyW(HKEY key, LPCWSTR sub, LPCWSTR file) { (void)key; (void)sub; (void)file; return 1314; /* ERROR_PRIVILEGE_NOT_HELD */ }
WINBASEAPI LSTATUS WINAPI RegUnLoadKeyW(HKEY key, LPCWSTR sub) { (void)key; (void)sub; return 1314; }
WINBASEAPI LSTATUS WINAPI RegSaveKeyW(HKEY key, LPCWSTR file, LPSECURITY_ATTRIBUTES sa) { (void)key; (void)file; (void)sa; return 1314; }
WINBASEAPI LSTATUS WINAPI RegRestoreKeyW(HKEY key, LPCWSTR file, DWORD flags) { (void)key; (void)file; (void)flags; return 1314; }
