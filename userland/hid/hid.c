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
HIDAPI BOOLEAN WINAPI HidD_GetFeature(HANDLE dev, PVOID buf, ULONG n) { (void)dev; (void)buf; (void)n; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
HIDAPI BOOLEAN WINAPI HidD_SetFeature(HANDLE dev, PVOID buf, ULONG n) { (void)dev; (void)buf; (void)n; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
HIDAPI BOOLEAN WINAPI HidD_GetSerialNumberString(HANDLE dev, PVOID buf, ULONG n) { (void)dev; (void)buf; (void)n; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
/* The report parsers: HidD_GetPreparsedData never hands out parsed report
 * descriptors, so whatever is passed for them is not one */
HIDAPI LONG WINAPI HidP_GetButtonCaps(int type, PVOID caps, USHORT *n, PVOID data)
{ (void)type; (void)caps; (void)data; if (n) *n = 0; return HIDP_STATUS_INVALID_PREPARSED_DATA; }
HIDAPI LONG WINAPI HidP_GetValueCaps(int type, PVOID caps, USHORT *n, PVOID data)
{ (void)type; (void)caps; (void)data; if (n) *n = 0; return HIDP_STATUS_INVALID_PREPARSED_DATA; }
HIDAPI LONG WINAPI HidP_GetUsageValue(int type, USHORT page, USHORT link, USHORT usage, PULONG value, PVOID data, CHAR *report, ULONG len)
{ (void)type; (void)page; (void)link; (void)usage; (void)data; (void)report; (void)len; if (value) *value = 0; return HIDP_STATUS_INVALID_PREPARSED_DATA; }
HIDAPI LONG WINAPI HidP_GetScaledUsageValue(int type, USHORT page, USHORT link, USHORT usage, PLONG value, PVOID data, CHAR *report, ULONG len)
{ (void)type; (void)page; (void)link; (void)usage; (void)data; (void)report; (void)len; if (value) *value = 0; return HIDP_STATUS_INVALID_PREPARSED_DATA; }
HIDAPI LONG WINAPI HidP_GetUsagesEx(int type, USHORT link, PVOID list, PULONG n, PVOID data, CHAR *report, ULONG len)
{ (void)type; (void)link; (void)list; (void)data; (void)report; (void)len; if (n) *n = 0; return HIDP_STATUS_INVALID_PREPARSED_DATA; }
