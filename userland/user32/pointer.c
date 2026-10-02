/*
 * pointer.c — pointer (touch and pen), gesture and raw input: NovaOS has
 * a mouse and a keyboard only, so there are no touch or pen pointers, no
 * gestures, no raw input devices and no auto-rotation sensor.
 */
#include "u32.h"

#define PT_MOUSE 4
USERAPI BOOL GetPointerType(UINT32 id, DWORD *type)
{
    (void)id;
    if (!type) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *type = PT_MOUSE;
    return TRUE;
}
/* No pointer messages are sent, so no pointer id is ever valid */
static BOOL no_pointer(void) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
USERAPI BOOL GetPointerInfo(UINT32 id, void *info) { (void)id; (void)info; return no_pointer(); }
USERAPI BOOL GetPointerPenInfo(UINT32 id, void *info) { (void)id; (void)info; return no_pointer(); }
USERAPI BOOL GetPointerTouchInfo(UINT32 id, void *info) { (void)id; (void)info; return no_pointer(); }
USERAPI BOOL GetPointerFrameTouchInfo(UINT32 id, UINT32 *n, void *info) { (void)id; (void)n; (void)info; return no_pointer(); }
USERAPI BOOL GetPointerFramePenInfo(UINT32 id, UINT32 *n, void *info) { (void)id; (void)n; (void)info; return no_pointer(); }
USERAPI BOOL GetTouchInputInfo(HANDLE h, UINT n, void *info, int size) { (void)h; (void)n; (void)info; (void)size; return no_pointer(); }
USERAPI BOOL CloseTouchInputHandle(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL UnregisterTouchWindow(HWND h) { (void)h; return TRUE; }
USERAPI BOOL IsTouchWindow(HWND h, PULONG flags) { (void)h; if (flags) *flags = 0; return FALSE; }
USERAPI BOOL EnableMouseInPointer(BOOL on) { (void)on; return TRUE; }
USERAPI BOOL IsMouseInPointerEnabled(void) { return FALSE; }
USERAPI BOOL GetGestureInfo(HANDLE h, void *info) { (void)h; (void)info; return no_pointer(); }
USERAPI BOOL CloseGestureInfoHandle(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SetGestureConfig(HWND h, DWORD r, UINT n, void *cfg, UINT size) { (void)h; (void)r; (void)n; (void)cfg; (void)size; return TRUE; }
USERAPI BOOL GetGestureConfig(HWND h, DWORD r, DWORD f, PUINT n, void *cfg, UINT size) { (void)h; (void)r; (void)f; (void)cfg; (void)size; if (n) *n = 0; return TRUE; }
USERAPI HANDLE CreateSyntheticPointerDevice(DWORD type, ULONG max, DWORD mode)
{
    (void)type; (void)max; (void)mode;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
USERAPI BOOL InjectSyntheticPointerInput(HANDLE dev, const void *info, UINT32 n) { (void)dev; (void)info; (void)n; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
USERAPI void DestroySyntheticPointerDevice(HANDLE dev) { (void)dev; }

#define AR_NOSENSOR 0x10
USERAPI BOOL GetAutoRotationState(DWORD *state) { if (!state) return FALSE; *state = AR_NOSENSOR; return TRUE; }

/* Raw input: no devices to list or read */
USERAPI UINT GetRawInputDeviceList(void *list, PUINT n, UINT size)
{
    (void)list; (void)size;
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return (UINT)-1; }
    *n = 0;
    return 0;
}
USERAPI UINT GetRawInputDeviceInfoW(HANDLE dev, UINT cmd, LPVOID data, PUINT size)
{
    (void)dev; (void)cmd; (void)data; (void)size;
    SetLastError(ERROR_INVALID_HANDLE);
    return (UINT)-1;
}
USERAPI UINT GetRawInputDeviceInfoA(HANDLE dev, UINT cmd, LPVOID data, PUINT size) { return GetRawInputDeviceInfoW(dev, cmd, data, size); }
USERAPI UINT GetRawInputData(HANDLE raw, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    (void)raw; (void)cmd; (void)data; (void)size; (void)header;
    SetLastError(ERROR_INVALID_HANDLE);
    return (UINT)-1;
}

/* Every thread can own windows */
USERAPI BOOL IsGUIThread(BOOL convert) { (void)convert; return TRUE; }

/* Keyboard layouts, ANSI forms: US English only */
USERAPI BOOL GetKeyboardLayoutNameA(LPSTR name)
{
    if (!name) return FALSE;
    const char *s = "00000409";
    for (int i = 0; i < 9; i++) name[i] = s[i];
    return TRUE;
}
USERAPI HANDLE LoadKeyboardLayoutA(LPCSTR id, UINT f) { (void)id; (void)f; return (HANDLE)(ULONG_PTR)0x04090409; }
USERAPI BOOL UnloadKeyboardLayout(HANDLE h) { (void)h; return TRUE; }

/* PrintWindow: ask the window to paint itself into @hdc */
USERAPI BOOL PrintWindow(HWND h, HDC hdc, UINT flags)
{
    (void)flags;
    if (!IsWindow(h) || !hdc) return FALSE;
    SendMessageW(h, WM_PRINT, (WPARAM)hdc, 0x02 | 0x04 | 0x08 | 0x10);  /* PRF_NONCLIENT, CLIENT, ERASEBKGND, CHILDREN */
    return TRUE;
}

/* BroadcastSystemMessage: to every top-level window (posted, so no
 * recipient can block the sender) */
USERAPI long BroadcastSystemMessageW(DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)flags; (void)recipients;
    PostMessageW(HWND_BROADCAST, msg, wp, lp);
    return 1;
}
USERAPI long BroadcastSystemMessageA(DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp)
{
    return BroadcastSystemMessageW(flags, recipients, msg, wp, lp);
}
