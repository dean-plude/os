/* stubs.c — (temporary) the big controls as plain windows */
#include "cc.h"
LRESULT CALLBACK TreeViewProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
CC HPROPSHEETPAGE WINAPI CreatePropertySheetPageW(LPCPROPSHEETPAGEW p) { (void)p; return 0; }
CC BOOL WINAPI DestroyPropertySheetPage(HPROPSHEETPAGE p) { (void)p; return TRUE; }
CC INT_PTR WINAPI PropertySheetW(LPCPROPSHEETHEADERW p) { (void)p; return -1; }
