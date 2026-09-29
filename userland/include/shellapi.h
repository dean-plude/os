/* shellapi.h — shell32's program-facing API (the parts NovaOS has) */
#pragma once
#include <windows.h>
#ifdef NOVA_BUILD_SHELL32
#define SHAPI __declspec(dllexport)
#else
#define SHAPI __declspec(dllimport)
#endif
SHAPI void  WINAPI DragAcceptFiles(HWND h, BOOL accept);
SHAPI UINT  WINAPI DragQueryFileW(HDROP drop, UINT i, LPWSTR buf, UINT n);
SHAPI UINT  WINAPI DragQueryFileA(HDROP drop, UINT i, LPSTR buf, UINT n);
SHAPI BOOL  WINAPI DragQueryPoint(HDROP drop, POINT *pt);
SHAPI void  WINAPI DragFinish(HDROP drop);
#define DragQueryFile DragQueryFileW
