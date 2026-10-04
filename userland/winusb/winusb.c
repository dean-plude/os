/*
 * winusb.dll — WinUSB, the user-mode USB device interface.  A program
 * opens a device's interface path with CreateFile and hands the handle to
 * WinUsb_Initialize; NovaOS lists no WinUSB device interfaces (setupapi),
 * so there is never a device handle to take and no interface handle to
 * use: every call fails with ERROR_INVALID_HANDLE, as Windows does for a
 * handle that is not a WinUSB device's.
 */
#include <windows.h>

#define WINUSBAPI __declspec(dllexport)

static BOOL no_device(void) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

WINUSBAPI BOOL WINAPI WinUsb_Initialize(HANDLE dev, PVOID *iface) { (void)dev; if (iface) *iface = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_Free(PVOID iface) { (void)iface; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_GetAssociatedInterface(PVOID iface, UCHAR i, PVOID *out) { (void)iface; (void)i; if (out) *out = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_GetDescriptor(PVOID iface, UCHAR type, UCHAR i, USHORT lang, PUCHAR buf, ULONG n, PULONG got)
{ (void)iface; (void)type; (void)i; (void)lang; (void)buf; (void)n; if (got) *got = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_QueryInterfaceSettings(PVOID iface, UCHAR alt, PVOID desc) { (void)iface; (void)alt; (void)desc; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_QueryDeviceInformation(PVOID iface, ULONG type, PULONG n, PVOID buf) { (void)iface; (void)type; (void)n; (void)buf; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_SetCurrentAlternateSetting(PVOID iface, UCHAR alt) { (void)iface; (void)alt; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_GetCurrentAlternateSetting(PVOID iface, PUCHAR alt) { (void)iface; (void)alt; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_QueryPipe(PVOID iface, UCHAR alt, UCHAR pipe, PVOID info) { (void)iface; (void)alt; (void)pipe; (void)info; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_QueryPipeEx(PVOID iface, UCHAR alt, UCHAR pipe, PVOID info) { (void)iface; (void)alt; (void)pipe; (void)info; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_SetPipePolicy(PVOID iface, UCHAR pipe, ULONG type, ULONG n, PVOID v) { (void)iface; (void)pipe; (void)type; (void)n; (void)v; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_GetPipePolicy(PVOID iface, UCHAR pipe, ULONG type, PULONG n, PVOID v) { (void)iface; (void)pipe; (void)type; (void)n; (void)v; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_SetPowerPolicy(PVOID iface, ULONG type, ULONG n, PVOID v) { (void)iface; (void)type; (void)n; (void)v; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_GetPowerPolicy(PVOID iface, ULONG type, PULONG n, PVOID v) { (void)iface; (void)type; (void)n; (void)v; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_ReadPipe(PVOID iface, UCHAR pipe, PUCHAR buf, ULONG n, PULONG got, LPOVERLAPPED o)
{ (void)iface; (void)pipe; (void)buf; (void)n; (void)o; if (got) *got = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_WritePipe(PVOID iface, UCHAR pipe, PUCHAR buf, ULONG n, PULONG sent, LPOVERLAPPED o)
{ (void)iface; (void)pipe; (void)buf; (void)n; (void)o; if (sent) *sent = 0; return no_device(); }
/* (WINUSB_SETUP_PACKET, 8 bytes, is passed by value) */
WINUSBAPI BOOL WINAPI WinUsb_ControlTransfer(PVOID iface, ULONGLONG setup, PUCHAR buf, ULONG n, PULONG got, LPOVERLAPPED o)
{ (void)iface; (void)setup; (void)buf; (void)n; (void)o; if (got) *got = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_ResetPipe(PVOID iface, UCHAR pipe) { (void)iface; (void)pipe; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_AbortPipe(PVOID iface, UCHAR pipe) { (void)iface; (void)pipe; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_FlushPipe(PVOID iface, UCHAR pipe) { (void)iface; (void)pipe; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_GetOverlappedResult(PVOID iface, LPOVERLAPPED o, LPDWORD n, BOOL wait)
{ (void)iface; (void)o; (void)wait; if (n) *n = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_ReadIsochPipe(PVOID buf, ULONG off, ULONG n, PULONG frame, ULONG npk, PVOID pk, LPOVERLAPPED o)
{ (void)buf; (void)off; (void)n; (void)frame; (void)npk; (void)pk; (void)o; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_WriteIsochPipe(PVOID buf, ULONG off, ULONG n, PULONG frame, LPOVERLAPPED o)
{ (void)buf; (void)off; (void)n; (void)frame; (void)o; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_RegisterIsochBuffer(PVOID iface, UCHAR pipe, PUCHAR buf, ULONG n, PVOID *h)
{ (void)iface; (void)pipe; (void)buf; (void)n; if (h) *h = 0; return no_device(); }
WINUSBAPI BOOL WINAPI WinUsb_UnregisterIsochBuffer(PVOID h) { (void)h; return no_device(); }
