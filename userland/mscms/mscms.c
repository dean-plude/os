/*
 * mscms.dll — Windows color management.  NovaOS installs no color
 * profiles: the color directory is reported (programs list the .icc
 * files in it; Inkscape asks for it at start) and holds none, so programs
 * fall back to sRGB, as on a Windows machine without monitor profiles.
 */
#include <windows.h>

#define MSCMSAPI __declspec(dllexport)

static const WCHAR g_dir[] = L"C:\\Windows\\System32\\spool\\drivers\\color";

MSCMSAPI BOOL WINAPI GetColorDirectoryW(LPCWSTR machine, LPWSTR buf, PDWORD size)
{
    DWORD need = sizeof(g_dir);
    if (machine || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!buf || *size < need) {
        *size = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    for (DWORD i = 0; i < need / sizeof(WCHAR); i++) buf[i] = g_dir[i];
    *size = need;
    return TRUE;
}

MSCMSAPI BOOL WINAPI GetColorDirectoryA(LPCSTR machine, LPSTR buf, PDWORD size)
{
    DWORD need = sizeof(g_dir) / sizeof(WCHAR);
    if (machine || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!buf || *size < need) {
        *size = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    for (DWORD i = 0; i < need; i++) buf[i] = (char)g_dir[i];
    *size = need;
    return TRUE;
}

/* No installed profiles: an empty list */
static BOOL no_profiles(PDWORD size, PDWORD count)
{
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *size = 0;
    if (count) *count = 0;
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
MSCMSAPI BOOL WINAPI EnumColorProfilesW(LPCWSTR machine, void *record, PBYTE buf, PDWORD size, PDWORD count)
{ (void)machine; (void)record; (void)buf; return no_profiles(size, count); }
MSCMSAPI BOOL WINAPI EnumColorProfilesA(LPCSTR machine, void *record, PBYTE buf, PDWORD size, PDWORD count)
{ (void)machine; (void)record; (void)buf; return no_profiles(size, count); }
