/*
 * mui.c — RegLoadMUIString: a registry string that may be an indirect
 * "@file.dll,-id" reference to a string resource (how Windows stores the
 * display names of time zones, services and shell entries).  NovaOS has
 * one language, so the string comes from the module's own string table.
 */

#define NOVA_BUILD_ADVAPI32
#include <winternl.h>
#include "advapi32.h"

#define ERROR_MORE_DATA_   234
#define ERROR_BADKEY_      1010
#define REG_SZ_            1
#define REG_EXPAND_SZ_     2
#define RT_STRING_         ((LPCWSTR)(ULONG_PTR)6)
#define LOAD_LIBRARY_AS_DATAFILE_ 0x00000002
#define REG_MUI_STRING_TRUNCATE   0x00000001

/* String @id of @mod's string table: 16 counted strings per resource */
static int load_string(HMODULE mod, UINT id, const WCHAR **out)
{
    HRSRC r = FindResourceW(mod, (LPCWSTR)(ULONG_PTR)(id / 16 + 1), RT_STRING_);
    if (!r) return -1;
    const WCHAR *p = LockResource(LoadResource(mod, r));
    if (!p) return -1;
    for (UINT i = 0; i < id % 16; i++) p += 1 + *p;
    *out = p + 1;
    return *p;
}

static LSTATUS put(const WCHAR *s, int n, LPWSTR buf, DWORD cb, LPDWORD need, DWORD flags)
{
    DWORD bytes = (DWORD)(n + 1) * sizeof(WCHAR);
    if (need) *need = bytes;
    if (!buf) return cb ? ERROR_INVALID_PARAMETER : ERROR_MORE_DATA_;
    if (cb < bytes) {
        if (!(flags & REG_MUI_STRING_TRUNCATE) || cb < sizeof(WCHAR)) return ERROR_MORE_DATA_;
        n = (int)(cb / sizeof(WCHAR)) - 1;
    }
    for (int i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = 0;
    return ERROR_SUCCESS;
}

WINADVAPI LSTATUS WINAPI RegLoadMUIStringW(HKEY key, LPCWSTR value, LPWSTR buf, DWORD cb, LPDWORD need, DWORD flags,
                                           LPCWSTR dir)
{
    if (flags & ~REG_MUI_STRING_TRUNCATE) return ERROR_INVALID_PARAMETER;
    if ((flags & REG_MUI_STRING_TRUNCATE) && need) return ERROR_INVALID_PARAMETER;
    WCHAR raw[1024], path[MAX_PATH * 2];
    DWORD type = 0, n = sizeof(raw) - sizeof(WCHAR);
    LSTATUS r = RegQueryValueExW(key, value, 0, &type, (BYTE *)raw, &n);
    if (r) return r;
    if (type != REG_SZ_ && type != REG_EXPAND_SZ_) return ERROR_FILE_NOT_FOUND;
    raw[n / sizeof(WCHAR)] = 0;
    if (raw[0] != '@') return put(raw, lstrlenW(raw), buf, cb, need, flags);

    /* @file,-id (an optional ;comment follows) */
    WCHAR *comma = 0;
    for (WCHAR *p = raw + 1; *p; p++) if (*p == ',') comma = p;
    if (!comma || comma[1] != '-') return ERROR_BADKEY_;
    *comma = 0;
    UINT id = 0;
    for (WCHAR *p = comma + 2; *p >= '0' && *p <= '9'; p++) id = id * 10 + (UINT)(*p - '0');
    WCHAR file[MAX_PATH * 2];
    if (!ExpandEnvironmentStringsW(raw + 1, file, MAX_PATH * 2)) return ERROR_BADKEY_;
    const WCHAR *f = file;
    if (dir && *dir && !(file[0] == '\\' || (file[0] && file[1] == ':'))) {
        int k = 0;
        for (const WCHAR *d = dir; *d && k < MAX_PATH; d++) path[k++] = *d;
        if (k && path[k - 1] != '\\') path[k++] = '\\';
        for (const WCHAR *s = file; *s && k < MAX_PATH * 2 - 1; s++) path[k++] = *s;
        path[k] = 0;
        f = path;
    }
    HMODULE mod = LoadLibraryExW(f, 0, LOAD_LIBRARY_AS_DATAFILE_);
    if (!mod) return ERROR_FILE_NOT_FOUND;
    const WCHAR *s;
    int len = load_string(mod, id, &s);
    r = len < 0 ? ERROR_RESOURCE_NAME_NOT_FOUND : put(s, len, buf, cb, need, flags);
    FreeLibrary(mod);
    return r;
}

/* The ANSI form: Windows has it unimplemented too */
WINADVAPI LSTATUS WINAPI RegLoadMUIStringA(HKEY key, LPCSTR value, LPSTR buf, DWORD cb, LPDWORD need, DWORD flags,
                                           LPCSTR dir)
{
    (void)key; (void)value; (void)buf; (void)cb; (void)need; (void)flags; (void)dir;
    return ERROR_CALL_NOT_IMPLEMENTED;
}
