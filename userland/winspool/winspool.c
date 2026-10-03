/*
 * winspool.drv — the print spooler's client.  NovaOS has no printers yet:
 * the printer lists are empty, there is no default printer, and opening a
 * printer by name fails, so programs show "no printers" instead of failing
 * to start.  (Exported under Windows' ordinals for the default-printer
 * calls, which import libraries reach by number.)
 */
#include <windows.h>

#define SPOOLAPI __declspec(dllexport)
#define ERROR_INVALID_PRINTER_NAME_ 1801

SPOOLAPI BOOL WINAPI EnumPrintersW(DWORD flags, LPWSTR name, DWORD level, LPBYTE buf, DWORD n, LPDWORD need, LPDWORD count)
{
    (void)flags; (void)name; (void)level; (void)buf; (void)n;
    if (need) *need = 0;
    if (count) *count = 0;
    return TRUE;
}
SPOOLAPI BOOL WINAPI EnumPrintersA(DWORD flags, LPSTR name, DWORD level, LPBYTE buf, DWORD n, LPDWORD need, LPDWORD count)
{
    (void)name;
    return EnumPrintersW(flags, NULL, level, buf, n, need, count);
}

SPOOLAPI BOOL WINAPI GetDefaultPrinterW(LPWSTR buf, LPDWORD n)
{
    (void)buf;
    if (n) *n = 0;
    SetLastError(ERROR_FILE_NOT_FOUND);             /* as Windows with no default printer */
    return FALSE;
}
SPOOLAPI BOOL WINAPI GetDefaultPrinterA(LPSTR buf, LPDWORD n) { (void)buf; return GetDefaultPrinterW(NULL, n); }
SPOOLAPI BOOL WINAPI SetDefaultPrinterW(LPCWSTR name)
{
    (void)name;
    SetLastError(ERROR_INVALID_PRINTER_NAME_);
    return FALSE;
}
SPOOLAPI BOOL WINAPI SetDefaultPrinterA(LPCSTR name) { (void)name; return SetDefaultPrinterW(NULL); }

SPOOLAPI BOOL WINAPI OpenPrinterW(LPWSTR name, LPHANDLE h, LPVOID defaults)
{
    (void)name; (void)defaults;
    if (h) *h = NULL;
    SetLastError(ERROR_INVALID_PRINTER_NAME_);
    return FALSE;
}
SPOOLAPI BOOL WINAPI OpenPrinterA(LPSTR name, LPHANDLE h, LPVOID defaults) { (void)name; return OpenPrinterW(NULL, h, defaults); }
SPOOLAPI BOOL WINAPI OpenPrinter2W(LPCWSTR name, LPHANDLE h, LPVOID defaults, LPVOID options)
{
    (void)options;
    return OpenPrinterW((LPWSTR)name, h, defaults);
}
SPOOLAPI BOOL WINAPI ClosePrinter(HANDLE h)
{
    (void)h;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

/* (both need an open printer, which there never is) */
SPOOLAPI int WINAPI DeviceCapabilitiesW(LPCWSTR device, LPCWSTR port, WORD cap, LPWSTR out, const void *dm)
{
    (void)device; (void)port; (void)cap; (void)out; (void)dm;
    SetLastError(ERROR_INVALID_PRINTER_NAME_);
    return -1;
}
SPOOLAPI int WINAPI DeviceCapabilitiesA(LPCSTR device, LPCSTR port, WORD cap, LPSTR out, const void *dm)
{
    (void)device; (void)port; (void)out;
    return DeviceCapabilitiesW(NULL, NULL, cap, NULL, dm);
}
SPOOLAPI LONG WINAPI DocumentPropertiesW(HWND w, HANDLE h, LPWSTR device, void *out, void *in, DWORD mode)
{
    (void)w; (void)h; (void)device; (void)out; (void)in; (void)mode;
    SetLastError(ERROR_INVALID_HANDLE);
    return -1;
}
SPOOLAPI LONG WINAPI DocumentPropertiesA(HWND w, HANDLE h, LPSTR device, void *out, void *in, DWORD mode)
{
    (void)device;
    return DocumentPropertiesW(w, h, NULL, out, in, mode);
}
SPOOLAPI BOOL WINAPI GetPrinterW(HANDLE h, DWORD level, LPBYTE buf, DWORD n, LPDWORD need)
{
    (void)h; (void)level; (void)buf; (void)n;
    if (need) *need = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
SPOOLAPI BOOL WINAPI GetPrinterDriverW(HANDLE h, LPWSTR env, DWORD level, LPBYTE buf, DWORD n, LPDWORD need)
{
    (void)env;
    return GetPrinterW(h, level, buf, n, need);
}
