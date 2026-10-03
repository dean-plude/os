/*
 * winspool.drv — the print spooler's client, with no spooler behind it:
 * there are no printers, so every printer opens and lookups fail the way
 * they do on a Windows machine without one, and enumerations find none.
 * Programs (wxWidgets, Qt, the common dialogs) import it to offer printing
 * and take these answers for "no printer installed".
 */
#include <windows.h>

#define SPOOLAPI __declspec(dllexport)

#define ERROR_INVALID_PRINTER_NAME 1801

SPOOLAPI BOOL WINAPI OpenPrinterW(LPWSTR name, HANDLE *h, void *defaults)
{
    (void)name; (void)defaults;
    if (h) *h = 0;
    SetLastError(ERROR_INVALID_PRINTER_NAME);
    return FALSE;
}
SPOOLAPI BOOL WINAPI OpenPrinterA(LPSTR name, HANDLE *h, void *defaults) { return OpenPrinterW((LPWSTR)name, h, defaults); }
SPOOLAPI BOOL WINAPI ClosePrinter(HANDLE h) { (void)h; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

SPOOLAPI LONG WINAPI DocumentPropertiesW(HWND owner, HANDLE h, LPWSTR dev, void *out, void *in, DWORD mode)
{
    (void)owner; (void)h; (void)dev; (void)out; (void)in; (void)mode;
    SetLastError(ERROR_INVALID_HANDLE);
    return -1;
}
SPOOLAPI LONG WINAPI DocumentPropertiesA(HWND owner, HANDLE h, LPSTR dev, void *out, void *in, DWORD mode)
{
    return DocumentPropertiesW(owner, h, (LPWSTR)dev, out, in, mode);
}

/* No printers: nothing to list, nothing needed */
SPOOLAPI BOOL WINAPI EnumPrintersW(DWORD flags, LPWSTR name, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{
    (void)flags; (void)name; (void)level; (void)buf; (void)cb;
    if (needed) *needed = 0;
    if (returned) *returned = 0;
    return TRUE;
}
SPOOLAPI BOOL WINAPI EnumPrintersA(DWORD flags, LPSTR name, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{
    return EnumPrintersW(flags, (LPWSTR)name, level, buf, cb, needed, returned);
}

SPOOLAPI BOOL WINAPI GetDefaultPrinterW(LPWSTR buf, LPDWORD size)
{
    (void)buf;
    if (size) *size = 0;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
}
SPOOLAPI BOOL WINAPI GetDefaultPrinterA(LPSTR buf, LPDWORD size) { return GetDefaultPrinterW((LPWSTR)buf, size); }

SPOOLAPI BOOL WINAPI GetPrinterW(HANDLE h, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{
    (void)h; (void)level; (void)buf; (void)cb;
    if (needed) *needed = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
SPOOLAPI BOOL WINAPI GetPrinterA(HANDLE h, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed) { return GetPrinterW(h, level, buf, cb, needed); }
SPOOLAPI BOOL WINAPI GetPrinterDriverW(HANDLE h, LPWSTR env, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{
    (void)env;
    return GetPrinterW(h, level, buf, cb, needed);
}
SPOOLAPI BOOL WINAPI GetPrinterDriverA(HANDLE h, LPSTR env, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{
    (void)env;
    return GetPrinterW(h, level, buf, cb, needed);
}

SPOOLAPI DWORD WINAPI DeviceCapabilitiesW(LPCWSTR dev, LPCWSTR port, WORD cap, LPWSTR out, const void *mode)
{
    (void)dev; (void)port; (void)cap; (void)out; (void)mode;
    SetLastError(ERROR_INVALID_PRINTER_NAME);
    return (DWORD)-1;
}
SPOOLAPI DWORD WINAPI DeviceCapabilitiesA(LPCSTR dev, LPCSTR port, WORD cap, LPSTR out, const void *mode)
{
    return DeviceCapabilitiesW((LPCWSTR)dev, (LPCWSTR)port, cap, (LPWSTR)out, mode);
}

SPOOLAPI DWORD WINAPI StartDocPrinterW(HANDLE h, DWORD level, LPBYTE info) { (void)h; (void)level; (void)info; SetLastError(ERROR_INVALID_HANDLE); return 0; }
SPOOLAPI BOOL WINAPI StartPagePrinter(HANDLE h) { (void)h; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
SPOOLAPI BOOL WINAPI WritePrinter(HANDLE h, LPVOID buf, DWORD cb, LPDWORD written) { (void)h; (void)buf; (void)cb; if (written) *written = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
SPOOLAPI BOOL WINAPI EndPagePrinter(HANDLE h) { (void)h; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
SPOOLAPI BOOL WINAPI EndDocPrinter(HANDLE h) { (void)h; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
SPOOLAPI BOOL WINAPI AbortPrinter(HANDLE h) { (void)h; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
