/*
 * shellscalingapi.h — shcore's per-monitor DPI (shlwapi.dll in NovaOS)
 */
#pragma once
#include <windows.h>

typedef enum { PROCESS_DPI_UNAWARE = 0, PROCESS_SYSTEM_DPI_AWARE = 1, PROCESS_PER_MONITOR_DPI_AWARE = 2 } PROCESS_DPI_AWARENESS;
typedef enum { MDT_EFFECTIVE_DPI = 0, MDT_ANGULAR_DPI = 1, MDT_RAW_DPI = 2, MDT_DEFAULT = 0 } MONITOR_DPI_TYPE;

__declspec(dllimport) HRESULT WINAPI GetDpiForMonitor(HMONITOR mon, MONITOR_DPI_TYPE type, UINT *x, UINT *y);
__declspec(dllimport) HRESULT WINAPI SetProcessDpiAwareness(PROCESS_DPI_AWARENESS v);
__declspec(dllimport) HRESULT WINAPI GetProcessDpiAwareness(HANDLE p, PROCESS_DPI_AWARENESS *v);
