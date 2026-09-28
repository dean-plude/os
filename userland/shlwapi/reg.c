/*
 * reg.c — shlwapi's registry helpers (SHGetValue and friends), layered on
 * the Reg* API in kernel32.
 */

#define NOVA_BUILD_SHLWAPI
#include <windows.h>

/* SRRF_* flags match RRF_* for the type filters */
LWSTDAPI_(LSTATUS) SHRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, LPVOID data, LPDWORD n)
{
    return RegGetValueW(key, sub, name, flags, type, data, n);
}

LWSTDAPI_(LSTATUS) SHRegGetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, LPVOID data, LPDWORD n)
{
    return RegGetValueA(key, sub, name, flags, type, data, n);
}

LWSTDAPI_(DWORD) SHGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, LPDWORD type, LPVOID data, LPDWORD n)
{
    return RegGetValueW(key, sub, name, RRF_RT_ANY | RRF_NOEXPAND, type, data, n);
}

LWSTDAPI_(DWORD) SHGetValueA(HKEY key, LPCSTR sub, LPCSTR name, LPDWORD type, LPVOID data, LPDWORD n)
{
    return RegGetValueA(key, sub, name, RRF_RT_ANY | RRF_NOEXPAND, type, data, n);
}

LWSTDAPI_(DWORD) SHSetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD type, LPCVOID data, DWORD n)
{
    HKEY k;
    LSTATUS e = RegCreateKeyExW(key, sub ? sub : L"", 0, 0, 0, KEY_SET_VALUE, 0, &k, 0);
    if (e) return e;
    e = RegSetValueExW(k, name, 0, type, data, n);
    RegCloseKey(k);
    return e;
}

LWSTDAPI_(DWORD) SHSetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD type, LPCVOID data, DWORD n)
{
    HKEY k;
    LSTATUS e = RegCreateKeyExA(key, sub ? sub : "", 0, 0, 0, KEY_SET_VALUE, 0, &k, 0);
    if (e) return e;
    e = RegSetValueExA(k, name, 0, type, data, n);
    RegCloseKey(k);
    return e;
}

LWSTDAPI_(DWORD) SHDeleteValueW(HKEY key, LPCWSTR sub, LPCWSTR name)
{
    HKEY k;
    LSTATUS e = RegOpenKeyExW(key, sub, 0, KEY_SET_VALUE, &k);
    if (e) return e;
    e = RegDeleteValueW(k, name);
    RegCloseKey(k);
    return e;
}

LWSTDAPI_(DWORD) SHDeleteValueA(HKEY key, LPCSTR sub, LPCSTR name)
{
    HKEY k;
    LSTATUS e = RegOpenKeyExA(key, sub, 0, KEY_SET_VALUE, &k);
    if (e) return e;
    e = RegDeleteValueA(k, name);
    RegCloseKey(k);
    return e;
}

LWSTDAPI_(DWORD) SHDeleteKeyW(HKEY key, LPCWSTR sub) { return RegDeleteTreeW(key, sub); }
LWSTDAPI_(DWORD) SHDeleteKeyA(HKEY key, LPCSTR sub) { return RegDeleteTreeA(key, sub); }

/* deletes the key only when it has no subkeys and no values */
LWSTDAPI_(DWORD) SHDeleteEmptyKeyW(HKEY key, LPCWSTR sub)
{
    HKEY k;
    DWORD nsub = 0, nval = 0;
    LSTATUS e = RegOpenKeyExW(key, sub, 0, KEY_READ, &k);
    if (e) return e;
    e = RegQueryInfoKeyW(k, 0, 0, 0, &nsub, 0, 0, &nval, 0, 0, 0, 0);
    RegCloseKey(k);
    if (e) return e;
    if (nsub || nval) return ERROR_KEY_HAS_CHILDREN;
    return RegDeleteKeyW(key, sub);
}

LWSTDAPI_(DWORD) SHDeleteEmptyKeyA(HKEY key, LPCSTR sub)
{
    WCHAR w[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, sub ? sub : "", -1, w, 512)) return ERROR_INVALID_PARAMETER;
    return SHDeleteEmptyKeyW(key, w);
}

LWSTDAPI_(DWORD) SHQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPVOID data, LPDWORD n)
{
    return RegGetValueW(key, 0, name, RRF_RT_ANY, type, data, n);
}

LWSTDAPI_(DWORD) SHQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPVOID data, LPDWORD n)
{
    return RegGetValueA(key, 0, name, RRF_RT_ANY, type, data, n);
}

LWSTDAPI_(LONG) SHEnumKeyExW(HKEY key, DWORD i, LPWSTR name, LPDWORD n)
{
    return RegEnumKeyExW(key, i, name, n, 0, 0, 0, 0);
}

LWSTDAPI_(LONG) SHEnumKeyExA(HKEY key, DWORD i, LPSTR name, LPDWORD n)
{
    return RegEnumKeyExA(key, i, name, n, 0, 0, 0, 0);
}

LWSTDAPI_(LONG) SHEnumValueW(HKEY key, DWORD i, LPWSTR name, LPDWORD nn, LPDWORD type, LPVOID data, LPDWORD nd)
{
    return RegEnumValueW(key, i, name, nn, 0, type, data, nd);
}

LWSTDAPI_(LONG) SHEnumValueA(HKEY key, DWORD i, LPSTR name, LPDWORD nn, LPDWORD type, LPVOID data, LPDWORD nd)
{
    return RegEnumValueA(key, i, name, nn, 0, type, data, nd);
}

LWSTDAPI_(LONG) SHQueryInfoKeyW(HKEY key, LPDWORD subkeys, LPDWORD maxsub, LPDWORD values, LPDWORD maxval)
{
    return RegQueryInfoKeyW(key, 0, 0, 0, subkeys, maxsub, 0, values, maxval, 0, 0, 0);
}

LWSTDAPI_(LONG) SHQueryInfoKeyA(HKEY key, LPDWORD subkeys, LPDWORD maxsub, LPDWORD values, LPDWORD maxval)
{
    return RegQueryInfoKeyW(key, 0, 0, 0, subkeys, maxsub, 0, values, maxval, 0, 0, 0);
}

LWSTDAPI_(LSTATUS) SHCopyKeyW(HKEY from, LPCWSTR sub, HKEY to, DWORD reserved)
{
    (void)reserved;
    return RegCopyTreeW(from, sub, to);
}

LWSTDAPI_(HKEY) SHRegDuplicateHKey(HKEY key)
{
    HKEY k = 0;
    return RegOpenKeyExW(key, L"", 0, KEY_ALL_ACCESS, &k) ? 0 : k;
}

/* the SHReg*USValue family: HKCU first, HKLM as the fallback */
LWSTDAPI_(LSTATUS) SHRegGetUSValueW(LPCWSTR sub, LPCWSTR name, LPDWORD type, LPVOID data, LPDWORD n,
                                    BOOL ignore_hkcu, LPVOID def, DWORD ndef)
{
    DWORD cap = n ? *n : 0;
    LSTATUS e = ERROR_FILE_NOT_FOUND;
    if (!ignore_hkcu) e = SHGetValueW(HKEY_CURRENT_USER, sub, name, type, data, n);
    if (e == ERROR_FILE_NOT_FOUND) { if (n) *n = cap; e = SHGetValueW(HKEY_LOCAL_MACHINE, sub, name, type, data, n); }
    if (e && def && ndef && data && n && cap >= ndef) {
        for (DWORD i = 0; i < ndef; i++) ((BYTE *)data)[i] = ((const BYTE *)def)[i];
        *n = ndef;
        e = 0;
    }
    return e;
}

LWSTDAPI_(BOOL) SHRegGetBoolUSValueW(LPCWSTR sub, LPCWSTR name, BOOL ignore_hkcu, BOOL def)
{
    WCHAR buf[16];
    DWORD type, n = sizeof buf;
    if (SHRegGetUSValueW(sub, name, &type, buf, &n, ignore_hkcu, 0, 0)) return def;
    if (type == REG_DWORD) return *(DWORD *)buf != 0;
    if (type == REG_SZ) {
        if ((buf[0] | 32) == 'y' || (buf[0] | 32) == 't' || buf[0] == '1') return TRUE;
        if ((buf[0] | 32) == 'n' || (buf[0] | 32) == 'f' || buf[0] == '0') return FALSE;
    }
    return def;
}

LWSTDAPI_(LSTATUS) SHRegSetUSValueW(LPCWSTR sub, LPCWSTR name, DWORD type, LPCVOID data, DWORD n, DWORD flags)
{
    /* SHREGSET_HKLM 4 / SHREGSET_FORCE_HKLM 8 select the machine hive */
    return SHSetValueW((flags & 0xC) && !(flags & 3) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, sub, name, type, data, n);
}
