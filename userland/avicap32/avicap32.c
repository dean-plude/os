/*
 * avicap32.dll — Video for Windows capture.  NovaOS has no capture
 * drivers: none is listed and no capture window can be made.
 */
#include <windows.h>

#define AVICAP __declspec(dllexport)

AVICAP BOOL WINAPI capGetDriverDescriptionW(UINT i, LPWSTR name, int nn, LPWSTR ver, int nv)
{
    (void)i;
    if (name && nn > 0) name[0] = 0;
    if (ver && nv > 0) ver[0] = 0;
    return FALSE;
}
AVICAP BOOL WINAPI capGetDriverDescriptionA(UINT i, LPSTR name, int nn, LPSTR ver, int nv)
{
    (void)i;
    if (name && nn > 0) name[0] = 0;
    if (ver && nv > 0) ver[0] = 0;
    return FALSE;
}
AVICAP HWND WINAPI capCreateCaptureWindowW(LPCWSTR title, DWORD style, int x, int y, int w, int h, HWND parent, int id)
{
    (void)title; (void)style; (void)x; (void)y; (void)w; (void)h; (void)parent; (void)id;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
AVICAP HWND WINAPI capCreateCaptureWindowA(LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, int id)
{
    (void)title; (void)style; (void)x; (void)y; (void)w; (void)h; (void)parent; (void)id;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
