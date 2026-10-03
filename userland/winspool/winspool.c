/*
 * winspool.drv — the print spooler's client calls.  NovaOS has no printers
 * yet, so the spooler answers as Windows does on a machine without any:
 * the printer list is empty, there is no default printer and no printer
 * name opens.  Programs that link it for their print menus (SumatraPDF,
 * editors) load and run; printing reports that no printer is installed.
 */
#include <windows.h>
#include <string.h>

#define SPLAPI __declspec(dllexport)

#ifndef ERROR_INVALID_PRINTER_NAME
#define ERROR_INVALID_PRINTER_NAME 1801
#endif
#ifndef ERROR_INVALID_HANDLE
#define ERROR_INVALID_HANDLE 6
#endif

static BOOL no_printer(void) { SetLastError(ERROR_INVALID_PRINTER_NAME); return FALSE; }
static BOOL bad_handle(void) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

/* the list of printers: always empty (and a success) */
static BOOL empty_list(DWORD *needed, DWORD *returned)
{
    if (needed) *needed = 0;
    if (returned) *returned = 0;
    return TRUE;
}
SPLAPI BOOL WINAPI EnumPrintersW(DWORD flags, LPWSTR name, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)flags; (void)name; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }
SPLAPI BOOL WINAPI EnumPrintersA(DWORD flags, LPSTR name, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)flags; (void)name; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }
SPLAPI BOOL WINAPI EnumPrinterDriversW(LPWSTR srv, LPWSTR env, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)srv; (void)env; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }
SPLAPI BOOL WINAPI EnumPrinterDriversA(LPSTR srv, LPSTR env, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)srv; (void)env; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }
SPLAPI BOOL WINAPI EnumFormsW(HANDLE h, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)h; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }
SPLAPI BOOL WINAPI EnumJobsW(HANDLE h, DWORD first, DWORD n, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)h; (void)first; (void)n; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }
SPLAPI BOOL WINAPI EnumPortsW(LPWSTR srv, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed, LPDWORD returned)
{ (void)srv; (void)level; (void)buf; (void)cb; return empty_list(needed, returned); }

/* no default printer: GetDefaultPrinter fails with ERROR_FILE_NOT_FOUND */
SPLAPI BOOL WINAPI GetDefaultPrinterW(LPWSTR buf, LPDWORD cch)
{
    (void)buf;
    if (!cch) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *cch = 0;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
}
SPLAPI BOOL WINAPI GetDefaultPrinterA(LPSTR buf, LPDWORD cch) { (void)buf; return GetDefaultPrinterW(NULL, cch); }
SPLAPI BOOL WINAPI SetDefaultPrinterW(LPCWSTR name) { (void)name; return no_printer(); }
SPLAPI BOOL WINAPI SetDefaultPrinterA(LPCSTR name) { (void)name; return no_printer(); }

/* no name opens a printer */
SPLAPI BOOL WINAPI OpenPrinterW(LPWSTR name, LPHANDLE h, LPVOID defaults)
{ (void)name; (void)defaults; if (h) *h = NULL; return no_printer(); }
SPLAPI BOOL WINAPI OpenPrinterA(LPSTR name, LPHANDLE h, LPVOID defaults)
{ (void)name; (void)defaults; if (h) *h = NULL; return no_printer(); }
SPLAPI BOOL WINAPI OpenPrinter2W(LPCWSTR name, LPHANDLE h, LPVOID defaults, LPVOID options)
{ (void)name; (void)defaults; (void)options; if (h) *h = NULL; return no_printer(); }
SPLAPI BOOL WINAPI ClosePrinter(HANDLE h) { return h ? TRUE : bad_handle(); }

/* everything that takes a printer handle: there is none to take */
SPLAPI BOOL WINAPI GetPrinterW(HANDLE h, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{ (void)h; (void)level; (void)buf; (void)cb; if (needed) *needed = 0; return bad_handle(); }
SPLAPI BOOL WINAPI GetPrinterA(HANDLE h, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{ (void)h; (void)level; (void)buf; (void)cb; if (needed) *needed = 0; return bad_handle(); }
SPLAPI BOOL WINAPI SetPrinterW(HANDLE h, DWORD level, LPBYTE buf, DWORD cmd)
{ (void)h; (void)level; (void)buf; (void)cmd; return bad_handle(); }
SPLAPI BOOL WINAPI GetPrinterDriverW(HANDLE h, LPWSTR env, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{ (void)h; (void)env; (void)level; (void)buf; (void)cb; if (needed) *needed = 0; return bad_handle(); }
SPLAPI DWORD WINAPI StartDocPrinterW(HANDLE h, DWORD level, LPBYTE info)
{ (void)h; (void)level; (void)info; bad_handle(); return 0; }
SPLAPI DWORD WINAPI StartDocPrinterA(HANDLE h, DWORD level, LPBYTE info)
{ (void)h; (void)level; (void)info; bad_handle(); return 0; }
SPLAPI BOOL WINAPI EndDocPrinter(HANDLE h) { (void)h; return bad_handle(); }
SPLAPI BOOL WINAPI StartPagePrinter(HANDLE h) { (void)h; return bad_handle(); }
SPLAPI BOOL WINAPI EndPagePrinter(HANDLE h) { (void)h; return bad_handle(); }
SPLAPI BOOL WINAPI WritePrinter(HANDLE h, LPVOID buf, DWORD cb, LPDWORD written)
{ (void)h; (void)buf; (void)cb; if (written) *written = 0; return bad_handle(); }
SPLAPI BOOL WINAPI AbortPrinter(HANDLE h) { (void)h; return bad_handle(); }
SPLAPI DWORD WINAPI GetPrinterDataW(HANDLE h, LPWSTR name, LPDWORD type, LPBYTE buf, DWORD cb, LPDWORD needed)
{ (void)h; (void)name; (void)type; (void)buf; (void)cb; if (needed) *needed = 0; return ERROR_INVALID_HANDLE; }
SPLAPI DWORD WINAPI GetPrinterDataExW(HANDLE h, LPCWSTR key, LPCWSTR name, LPDWORD type, LPBYTE buf, DWORD cb, LPDWORD needed)
{ (void)h; (void)key; (void)name; (void)type; (void)buf; (void)cb; if (needed) *needed = 0; return ERROR_INVALID_HANDLE; }

/* DocumentProperties and DeviceCapabilities report failure as a negative count */
SPLAPI LONG WINAPI DocumentPropertiesW(HWND w, HANDLE h, LPWSTR name, LPVOID out, LPVOID in, DWORD mode)
{ (void)w; (void)h; (void)name; (void)out; (void)in; (void)mode; SetLastError(ERROR_INVALID_PRINTER_NAME); return -1; }
SPLAPI LONG WINAPI DocumentPropertiesA(HWND w, HANDLE h, LPSTR name, LPVOID out, LPVOID in, DWORD mode)
{ (void)w; (void)h; (void)name; (void)out; (void)in; (void)mode; SetLastError(ERROR_INVALID_PRINTER_NAME); return -1; }
SPLAPI LONG WINAPI AdvancedDocumentPropertiesW(HWND w, HANDLE h, LPWSTR name, LPVOID out, LPVOID in)
{ (void)w; (void)h; (void)name; (void)out; (void)in; return 0; }
SPLAPI int WINAPI DeviceCapabilitiesW(LPCWSTR dev, LPCWSTR port, WORD cap, LPWSTR out, const void *mode)
{ (void)dev; (void)port; (void)cap; (void)out; (void)mode; SetLastError(ERROR_INVALID_PRINTER_NAME); return -1; }
SPLAPI int WINAPI DeviceCapabilitiesA(LPCSTR dev, LPCSTR port, WORD cap, LPSTR out, const void *mode)
{ (void)dev; (void)port; (void)cap; (void)out; (void)mode; SetLastError(ERROR_INVALID_PRINTER_NAME); return -1; }
SPLAPI BOOL WINAPI PrinterProperties(HWND w, HANDLE h) { (void)w; (void)h; return bad_handle(); }

/* the spooler's own folder, where drivers would live */
SPLAPI BOOL WINAPI GetPrinterDriverDirectoryW(LPWSTR srv, LPWSTR env, DWORD level, LPBYTE buf, DWORD cb, LPDWORD needed)
{
    static const WCHAR dir[] = L"C:\\Windows\\System32\\spool\\drivers\\x64";
    (void)srv; (void)env; (void)level;
    if (needed) *needed = sizeof dir;
    if (!buf || cb < sizeof dir) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, dir, sizeof dir);
    return TRUE;
}
