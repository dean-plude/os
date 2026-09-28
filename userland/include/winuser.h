/* winuser.h — the USER32 subset NovaOS's user32.dll implements */
#pragma once
#include <_nova.h>
#include <wingdi.h>
_NOVA_BEGIN

#ifdef NOVA_BUILD_USER32
#define USERAPI __declspec(dllexport) __stdcall
#else
#define USERAPI __declspec(dllimport) __stdcall
#endif

typedef LRESULT (__stdcall *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagMSG {
    HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt;
} MSG, *LPMSG, *PMSG;

typedef struct tagWNDCLASSA {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCSTR lpszMenuName, lpszClassName;
} WNDCLASSA, *LPWNDCLASSA;
typedef struct tagWNDCLASSEXA {
    UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCSTR lpszMenuName, lpszClassName; HICON hIconSm;
} WNDCLASSEXA, *LPWNDCLASSEXA;
#define WNDCLASS WNDCLASSA
#define WNDCLASSEX WNDCLASSEXA

typedef struct tagPAINTSTRUCT {
    HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct tagCREATESTRUCTA {
    LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent;
    int cy, cx, y, x; LONG style; LPCSTR lpszName, lpszClass; DWORD dwExStyle;
} CREATESTRUCTA, *LPCREATESTRUCTA;

/* Window messages */
#define WM_CREATE       0x0001
#define WM_DESTROY      0x0002
#define WM_MOVE         0x0003
#define WM_SIZE         0x0005
#define WM_ACTIVATE     0x0006
#define WM_PAINT        0x000F
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_ERASEBKGND   0x0014
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_CHAR         0x0102
#define WM_COMMAND      0x0111
#define WM_TIMER        0x0113
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN  0x0204
#define WM_USER         0x0400

/* Window styles */
#define WS_OVERLAPPED    0x00000000
#define WS_CAPTION       0x00C00000
#define WS_SYSMENU       0x00080000
#define WS_THICKFRAME    0x00040000
#define WS_MINIMIZEBOX   0x00020000
#define WS_MAXIMIZEBOX   0x00010000
#define WS_VISIBLE       0x10000000
#define WS_CHILD         0x40000000
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX)
#define CW_USEDEFAULT    ((int)0x80000000)

/* ShowWindow */
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOW 5

/* PeekMessage */
#define PM_NOREMOVE 0
#define PM_REMOVE   1

/* MessageBox */
#define MB_OK 0x0
#define MB_OKCANCEL 0x1
#define MB_ICONINFORMATION 0x40
#define MB_ICONERROR 0x10
#define IDOK 1
#define IDCANCEL 2

/* Virtual keys (a subset; scancode-based on NovaOS) */
#define VK_BACK 0x0E
#define VK_TAB  0x0F
#define VK_RETURN 0x1C
#define VK_ESCAPE 0x01
#define VK_SPACE 0x39
#define VK_LEFT  0x4B
#define VK_RIGHT 0x4D
#define VK_UP    0x48
#define VK_DOWN  0x50

/* DrawText */
#define DT_LEFT 0x0
#define DT_CENTER 0x1
#define DT_RIGHT 0x2
#define DT_VCENTER 0x4
#define DT_SINGLELINE 0x20
#define DT_WORDBREAK 0x10

typedef HANDLE HINSTANCE;

typedef WORD ATOM;

USERAPI ATOM     RegisterClassA(const WNDCLASSA *wc);
USERAPI ATOM     RegisterClassExA(const WNDCLASSEXA *wc);
USERAPI HWND     CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
                                 int x, int y, int w, int h, HWND parent, HMENU menu,
                                 HINSTANCE inst, LPVOID param);
USERAPI BOOL     DestroyWindow(HWND h);
USERAPI BOOL     ShowWindow(HWND h, int cmd);
USERAPI BOOL     UpdateWindow(HWND h);
USERAPI BOOL     GetMessageA(LPMSG m, HWND h, UINT min, UINT max);
USERAPI BOOL     PeekMessageA(LPMSG m, HWND h, UINT min, UINT max, UINT remove);
USERAPI BOOL     TranslateMessage(const MSG *m);
USERAPI LRESULT  DispatchMessageA(const MSG *m);
USERAPI LRESULT  DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI VOID     PostQuitMessage(int code);
USERAPI BOOL     PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT  SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI HDC      BeginPaint(HWND h, LPPAINTSTRUCT ps);
USERAPI BOOL     EndPaint(HWND h, const PAINTSTRUCT *ps);
USERAPI HDC      GetDC(HWND h);
USERAPI int      ReleaseDC(HWND h, HDC dc);
USERAPI BOOL     GetClientRect(HWND h, LPRECT r);
USERAPI BOOL     GetWindowRect(HWND h, LPRECT r);
USERAPI BOOL     InvalidateRect(HWND h, const RECT *r, BOOL erase);
USERAPI BOOL     SetWindowTextA(HWND h, LPCSTR s);
USERAPI int      GetWindowTextA(HWND h, LPSTR s, int max);
USERAPI UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *fn);
USERAPI BOOL     KillTimer(HWND h, UINT_PTR id);
USERAPI int      MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type);
USERAPI int      FillRect(HDC dc, const RECT *r, HBRUSH br);
USERAPI int      FrameRect(HDC dc, const RECT *r, HBRUSH br);
USERAPI BOOL     InflateRect(LPRECT r, int dx, int dy);
USERAPI int      DrawTextA(HDC dc, LPCSTR s, int len, LPRECT r, UINT fmt);
USERAPI HCURSOR  LoadCursorA(HINSTANCE inst, LPCSTR name);
USERAPI HICON    LoadIconA(HINSTANCE inst, LPCSTR name);
USERAPI int      GetSystemMetrics(int index);
USERAPI HWND     GetDesktopWindow(void);
USERAPI BOOL     UpdateWindow(HWND h);

#define RegisterClass RegisterClassA
#define RegisterClassEx RegisterClassExA
#define CreateWindowExA CreateWindowExA
#define CreateWindowA(cls,t,st,x,y,w,h,par,menu,inst,p) CreateWindowExA(0,cls,t,st,x,y,w,h,par,menu,inst,p)
#define CreateWindow CreateWindowA
#define CreateWindowEx CreateWindowExA
#define GetMessage GetMessageA
#define PeekMessage PeekMessageA
#define DispatchMessage DispatchMessageA
#define DefWindowProc DefWindowProcA
#define PostMessage PostMessageA
#define SendMessage SendMessageA
#define SetWindowText SetWindowTextA
#define GetWindowText GetWindowTextA
#define MessageBox MessageBoxA
#define DrawText DrawTextA
#define LoadCursor LoadCursorA
#define LoadIcon LoadIconA
#define IDC_ARROW ((LPCSTR)32512)
#define IDI_APPLICATION ((LPCSTR)32512)
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

_NOVA_END
