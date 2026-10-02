/*
 * hid.dll — Human Interface Device class support.  NovaOS exposes no HID
 * device nodes (see setupapi), so there is never a device handle to ask.
 */
#include <windows.h>

#define HIDAPI __declspec(dllexport)
#define HIDP_STATUS_INVALID_PREPARSED_DATA ((LONG)0xC0110001)

HIDAPI void WINAPI HidD_GetHidGuid(GUID *g)
{
    static const GUID hid = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    if (g) *g = hid;
}
HIDAPI BOOLEAN WINAPI HidD_GetPreparsedData(HANDLE dev, PVOID *data) { (void)dev; if (data) *data = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
HIDAPI BOOLEAN WINAPI HidD_FreePreparsedData(PVOID data) { (void)data; return TRUE; }
HIDAPI BOOLEAN WINAPI HidD_GetAttributes(HANDLE dev, PVOID attrs) { (void)dev; (void)attrs; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
HIDAPI BOOLEAN WINAPI HidD_GetProductString(HANDLE dev, PVOID buf, ULONG n) { (void)dev; (void)buf; (void)n; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
HIDAPI BOOLEAN WINAPI HidD_GetManufacturerString(HANDLE dev, PVOID buf, ULONG n) { (void)dev; (void)buf; (void)n; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
HIDAPI LONG WINAPI HidP_GetCaps(PVOID data, PVOID caps) { (void)data; (void)caps; return HIDP_STATUS_INVALID_PREPARSED_DATA; }
