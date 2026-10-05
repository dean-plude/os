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

/* Virtual keys (Windows codes) */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_BACK    0x08
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_MENU    0x12
#define VK_CAPITAL 0x14
#define VK_ESCAPE  0x1B
#define VK_SPACE   0x20
#define VK_PRIOR   0x21
#define VK_NEXT    0x22
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_INSERT  0x2D
#define VK_DELETE  0x2E
#define VK_F1      0x70
#define VK_F12     0x7B

/* DrawText */
#define DT_LEFT 0x0
#define DT_CENTER 0x1
#define DT_RIGHT 0x2
#define DT_VCENTER 0x4
#define DT_SINGLELINE 0x20
#define DT_WORDBREAK 0x10

typedef HANDLE HINSTANCE;

typedef WORD ATOM;
typedef VOID (CALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef BOOL (CALLBACK *WNDENUMPROC)(HWND, LPARAM);
typedef struct tagWNDCLASSW {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCWSTR lpszMenuName, lpszClassName;
} WNDCLASSW, *LPWNDCLASSW;
typedef struct tagWNDCLASSEXW {
    UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCWSTR lpszMenuName, lpszClassName; HICON hIconSm;
} WNDCLASSEXW, *LPWNDCLASSEXW;
#define GWLP_WNDPROC   (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_ID        (-12)
#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWLP_USERDATA  (-21)
#define WM_SETTEXT     0x000C
#define WM_GETTEXT     0x000D
#define WM_SETFOCUS    0x0007
#define WM_KILLFOCUS   0x0008
#define WM_SYSKEYDOWN  0x0104
#define WM_RBUTTONUP   0x0205
#define WM_MOUSEWHEEL  0x020A
#define CF_TEXT        1
#define CF_BITMAP      2
#define CF_METAFILEPICT 3
#define CF_OEMTEXT     7
#define CF_DIB         8
#define CF_ENHMETAFILE 14
#define CF_LOCALE      16
#define CF_DIBV5       17
#define CF_HDROP       15
#define CF_UNICODETEXT 13
#define MB_YESNO       0x4
#define MB_YESNOCANCEL 0x3
#define IDYES 6
#define IDNO  7
#define COLOR_WINDOW   5
#define COLOR_BTNFACE  15

USERAPI ATOM     RegisterClassW(const WNDCLASSW *wc);
USERAPI ATOM     RegisterClassExW(const WNDCLASSEXW *wc);
USERAPI HWND     CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
USERAPI BOOL     GetMessageW(LPMSG m, HWND h, UINT min, UINT max);
USERAPI BOOL     PeekMessageW(LPMSG m, HWND h, UINT min, UINT max, UINT remove);
USERAPI LRESULT  DispatchMessageW(const MSG *m);
USERAPI LRESULT  DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL     PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT  SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT  CallWindowProcW(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL     SetWindowTextW(HWND h, LPCWSTR s);
USERAPI int      GetWindowTextW(HWND h, LPWSTR s, int max);
USERAPI int      MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type);
USERAPI int      DrawTextW(HDC dc, LPCWSTR s, int len, LPRECT r, UINT fmt);
USERAPI HCURSOR  LoadCursorW(HINSTANCE inst, LPCWSTR name);
USERAPI HICON    LoadIconW(HINSTANCE inst, LPCWSTR name);
USERAPI int      LoadStringW(HINSTANCE inst, UINT id, LPWSTR buf, int n);
USERAPI int      LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int n);
USERAPI LONG_PTR GetWindowLongPtrW(HWND h, int i);
USERAPI LONG_PTR SetWindowLongPtrW(HWND h, int i, LONG_PTR v);
USERAPI LONG_PTR GetWindowLongPtrA(HWND h, int i);
USERAPI LONG_PTR SetWindowLongPtrA(HWND h, int i, LONG_PTR v);
USERAPI BOOL     IsWindow(HWND h);
USERAPI BOOL     IsWindowVisible(HWND h);
USERAPI HWND     GetFocus(void);
USERAPI HWND     SetFocus(HWND h);
USERAPI HWND     GetForegroundWindow(void);
USERAPI SHORT    GetKeyState(int vk);
USERAPI SHORT    GetAsyncKeyState(int vk);
USERAPI BOOL     GetCursorPos(LPPOINT p);
USERAPI BOOL     SetRect(LPRECT r, int l, int t, int rr, int b);
USERAPI BOOL     PtInRect(const RECT *r, POINT p);
USERAPI BOOL     OffsetRect(LPRECT r, int dx, int dy);
USERAPI BOOL     IntersectRect(LPRECT d, const RECT *a, const RECT *b);
USERAPI BOOL     UnionRect(LPRECT d, const RECT *a, const RECT *b);
USERAPI BOOL     IsRectEmpty(const RECT *r);
USERAPI DWORD    GetSysColor(int i);
USERAPI HBRUSH   GetSysColorBrush(int i);
USERAPI UINT     RegisterWindowMessageW(LPCWSTR name);
USERAPI BOOL     PostThreadMessageW(DWORD tid, UINT msg, WPARAM wp, LPARAM lp);
USERAPI DWORD    MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD wake);
USERAPI BOOL     OpenClipboard(HWND h);
USERAPI BOOL     CloseClipboard(void);
USERAPI BOOL     EmptyClipboard(void);
USERAPI HANDLE   SetClipboardData(UINT fmt, HANDLE data);
USERAPI HANDLE   GetClipboardData(UINT fmt);
USERAPI LPWSTR   CharUpperW(LPWSTR s);
USERAPI LPWSTR   CharLowerW(LPWSTR s);
USERAPI BOOL     SystemParametersInfoW(UINT action, UINT uparam, PVOID p, UINT winini);
#ifndef NOVA_BUILD_USER32
__declspec(dllimport) int __cdecl wsprintfA(LPSTR buf, LPCSTR fmt, ...);
__declspec(dllimport) int __cdecl wsprintfW(LPWSTR buf, LPCWSTR fmt, ...);
#endif


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
USERAPI UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn);
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
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80

/* -----------------------------------------------------------------------
 * The rest of USER's vocabulary (messages, styles, controls, structures)
 * ----------------------------------------------------------------------- */
#define WM_NULL            0x0000
#define WM_ENABLE          0x000A
#define WM_SETREDRAW       0x000B
#define WM_GETTEXTLENGTH   0x000E
#define WM_QUERYENDSESSION 0x0011
#define WM_SYSCOLORCHANGE  0x0015
#define WM_SHOWWINDOW      0x0018
#define WM_SETTINGCHANGE   0x001A
#define WM_ACTIVATEAPP     0x001C
#define WM_CANCELMODE      0x001F
#define WM_SETCURSOR       0x0020
#define WM_MOUSEACTIVATE   0x0021
#define WM_CHILDACTIVATE   0x0022
#define WM_GETMINMAXINFO   0x0024
#define WM_DRAWITEM        0x002B
#define WM_MEASUREITEM     0x002C
#define WM_DELETEITEM      0x002D
#define WM_VKEYTOITEM      0x002E
#define WM_CHARTOITEM      0x002F
#define WM_SETFONT         0x0030
#define WM_GETFONT         0x0031
#define WM_QUERYDRAGICON   0x0037
#define WM_COMPAREITEM     0x0039
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_COPYDATA        0x004A
#define WM_NOTIFY          0x004E
#define WM_HELP            0x0053
typedef struct tagCOPYDATASTRUCT { ULONG_PTR dwData; DWORD cbData; PVOID lpData; } COPYDATASTRUCT, *PCOPYDATASTRUCT;
#define WM_NOTIFYFORMAT    0x0055
#define WM_CONTEXTMENU     0x007B
#define WM_STYLECHANGING   0x007C
#define WM_STYLECHANGED    0x007D
#define WM_GETICON         0x007F
#define WM_SETICON         0x0080
#define WM_NCCREATE        0x0081
#define WM_NCDESTROY       0x0082
#define WM_NCCALCSIZE      0x0083
#define WM_NCHITTEST       0x0084
#define WM_NCPAINT         0x0085
#define WM_NCACTIVATE      0x0086
#define WM_GETDLGCODE      0x0087
#define WM_NCMOUSEMOVE     0x00A0
#define WM_NCLBUTTONDOWN   0x00A1
#define WM_NCLBUTTONUP     0x00A2
#define WM_NCLBUTTONDBLCLK 0x00A3
#define WM_NCRBUTTONDOWN   0x00A4
#define WM_NCRBUTTONUP     0x00A5
#define WM_KEYFIRST        0x0100
#define WM_DEADCHAR        0x0103
#define WM_SYSKEYUP        0x0105
#define WM_SYSCHAR         0x0106
#define WM_UNICHAR         0x0109
#define WM_KEYLAST         0x0109
#define WM_INITDIALOG      0x0110
#define WM_SYSCOMMAND      0x0112
#define WM_HSCROLL         0x0114
#define WM_VSCROLL         0x0115
#define WM_INITMENU        0x0116
#define WM_INITMENUPOPUP   0x0117
#define WM_MENUSELECT      0x011F
#define WM_MENUCHAR        0x0120
#define WM_ENTERIDLE       0x0121
#define WM_UNINITMENUPOPUP 0x0125
#define WM_CHANGEUISTATE   0x0127
#define WM_UPDATEUISTATE   0x0128
#define WM_QUERYUISTATE    0x0129
#define WM_CTLCOLORMSGBOX  0x0132
#define WM_CTLCOLOREDIT    0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN     0x0135
#define WM_CTLCOLORDLG     0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC  0x0138
#define WM_MOUSEFIRST      0x0200
#define WM_RBUTTONDBLCLK   0x0206
#define WM_MBUTTONDOWN     0x0207
#define WM_MBUTTONUP       0x0208
#define WM_MBUTTONDBLCLK   0x0209
#define WM_XBUTTONDOWN     0x020B
#define WM_XBUTTONUP       0x020C
#define WM_XBUTTONDBLCLK   0x020D
#define WM_MOUSEHWHEEL     0x020E
#define WM_NCXBUTTONDOWN   0x00AB
#define WM_NCXBUTTONUP     0x00AC
#define WM_NCXBUTTONDBLCLK 0x00AD
#define XBUTTON1           0x0001
#define XBUTTON2           0x0002
#define GET_XBUTTON_WPARAM(wp) (HIWORD(wp))
#define WM_MOUSELAST       0x020E
#define WM_PARENTNOTIFY    0x0210
#define WM_ENTERMENULOOP   0x0211
#define WM_EXITMENULOOP    0x0212
#define WM_NEXTMENU        0x0213
#define WM_SIZING          0x0214
#define WM_CAPTURECHANGED  0x0215
#define WM_MOVING          0x0216
#define WM_MDICREATE       0x0220
#define WM_MDIDESTROY      0x0221
#define WM_MDIACTIVATE     0x0222
#define WM_MDIRESTORE      0x0223
#define WM_MDINEXT         0x0224
#define WM_MDIMAXIMIZE     0x0225
#define WM_MDITILE         0x0226
#define WM_MDICASCADE      0x0227
#define WM_MDIICONARRANGE  0x0228
#define WM_MDIGETACTIVE    0x0229
#define WM_MDISETMENU      0x0230
#define WM_MDIREFRESHMENU  0x0234
#define MDIS_ALLCHILDSTYLES 0x0001
#define MDITILE_VERTICAL     0x0000
#define MDITILE_HORIZONTAL   0x0001
#define MDITILE_SKIPDISABLED 0x0002
typedef struct tagMDICREATESTRUCTW {
    LPCWSTR szClass, szTitle;
    HANDLE hOwner;
    int x, y, cx, cy;
    DWORD style;
    LPARAM lParam;
} MDICREATESTRUCTW, *LPMDICREATESTRUCTW;
typedef struct tagMDICREATESTRUCTA {
    LPCSTR szClass, szTitle;
    HANDLE hOwner;
    int x, y, cx, cy;
    DWORD style;
    LPARAM lParam;
} MDICREATESTRUCTA, *LPMDICREATESTRUCTA;
typedef struct tagCLIENTCREATESTRUCT { HANDLE hWindowMenu; UINT idFirstChild; } CLIENTCREATESTRUCT, *LPCLIENTCREATESTRUCT;
#define WM_ENTERSIZEMOVE   0x0231
#define WM_EXITSIZEMOVE    0x0232
#define WM_DROPFILES       0x0233
#define WM_IME_SETCONTEXT  0x0281
#define WM_IME_NOTIFY      0x0282
#define WM_MOUSEHOVER      0x02A1
#define WM_MOUSELEAVE      0x02A3
#define WM_DPICHANGED      0x02E0
#define WM_DPICHANGED_BEFOREPARENT 0x02E2
#define WM_DPICHANGED_AFTERPARENT  0x02E3
#define WM_GETDPISCALEDSIZE 0x02E4
#define WM_CUT             0x0300
#define WM_COPY            0x0301
#define WM_PASTE           0x0302
#define WM_CLEAR           0x0303
#define WM_UNDO            0x0304
#define WM_THEMECHANGED    0x031A
#define WM_PRINTCLIENT     0x0318
#define WM_APP             0x8000

#define WS_POPUP           0x80000000
#define WS_MINIMIZE        0x20000000
#define WS_DISABLED        0x08000000
#define WS_CLIPSIBLINGS    0x04000000
#define WS_CLIPCHILDREN    0x02000000
#define WS_MAXIMIZE        0x01000000
#define WS_BORDER          0x00800000
#define WS_DLGFRAME        0x00400000
#define WS_VSCROLL         0x00200000
#define WS_HSCROLL         0x00100000
#define WS_GROUP           0x00020000
#define WS_TABSTOP         0x00010000
#define WS_POPUPWINDOW     (WS_POPUP|WS_BORDER|WS_SYSMENU)
#define WS_CHILDWINDOW     WS_CHILD
#define WS_TILEDWINDOW     WS_OVERLAPPEDWINDOW
#define WS_EX_DLGMODALFRAME 0x00000001
#define WS_EX_NOPARENTNOTIFY 0x00000004
#define WS_EX_TOPMOST      0x00000008
#define WS_EX_ACCEPTFILES  0x00000010
#define WS_EX_TRANSPARENT  0x00000020
#define WS_EX_MDICHILD     0x00000040
#define WS_EX_TOOLWINDOW   0x00000080
#define WS_EX_WINDOWEDGE   0x00000100
#define WS_EX_CLIENTEDGE   0x00000200
#define WS_EX_CONTEXTHELP  0x00000400
#define WS_EX_RIGHT        0x00001000
#define WS_EX_RTLREADING   0x00002000
#define WS_EX_LEFTSCROLLBAR 0x00004000
#define WS_EX_CONTROLPARENT 0x00010000
#define WS_EX_STATICEDGE   0x00020000
#define WS_EX_APPWINDOW    0x00040000
#define WS_EX_LAYERED      0x00080000
#define WS_EX_NOINHERITLAYOUT 0x00100000
#define WS_EX_LAYOUTRTL    0x00400000
#define WS_EX_COMPOSITED   0x02000000
#define WS_EX_NOACTIVATE   0x08000000
#define WS_EX_OVERLAPPEDWINDOW (WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE)

#define CS_VREDRAW         0x0001
#define CS_HREDRAW         0x0002
#define CS_DBLCLKS         0x0008
#define CS_OWNDC           0x0020
#define CS_CLASSDC         0x0040
#define CS_PARENTDC        0x0080
#define CS_NOCLOSE         0x0200
#define CS_SAVEBITS        0x0800
#define CS_GLOBALCLASS     0x4000
#define CS_DROPSHADOW      0x00020000

#define SW_NORMAL          1
#define SW_SHOWMINIMIZED   2
#define SW_SHOWMAXIMIZED   3
#define SW_MAXIMIZE        3
#define SW_SHOWNOACTIVATE  4
#define SW_MINIMIZE        6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA          8
#define SW_RESTORE         9
#define SW_SHOWDEFAULT     10
#define SW_FORCEMINIMIZE   11

#define SWP_NOSIZE         0x0001
#define SWP_NOMOVE         0x0002
#define SWP_NOZORDER       0x0004
#define SWP_NOREDRAW       0x0008
#define SWP_NOACTIVATE     0x0010
#define SWP_FRAMECHANGED   0x0020
#define SWP_SHOWWINDOW     0x0040
#define SWP_HIDEWINDOW     0x0080
#define SWP_NOCOPYBITS     0x0100
#define SWP_NOOWNERZORDER  0x0200
#define SWP_NOSENDCHANGING 0x0400
#define SWP_DEFERERASE     0x2000
#define SWP_ASYNCWINDOWPOS 0x4000
#define HWND_TOP           ((HWND)0)
#define HWND_BOTTOM        ((HWND)1)
#define HWND_TOPMOST       ((HWND)-1)
#define HWND_NOTOPMOST     ((HWND)-2)
#define HWND_MESSAGE       ((HWND)-3)
#define HWND_BROADCAST     ((HWND)0xFFFF)

#define GW_HWNDFIRST 0
#define GW_HWNDLAST  1
#define GW_HWNDNEXT  2
#define GW_HWNDPREV  3
#define GW_OWNER     4
#define GW_CHILD     5
#define GW_ENABLEDPOPUP 6
#define GA_PARENT    1
#define GA_ROOT      2
#define GA_ROOTOWNER 3
#define GWL_WNDPROC   (-4)
#define GWL_HINSTANCE (-6)
#define GWLP_HWNDPARENT (-8)
#define GWL_ID        (-12)
#define GWL_USERDATA  (-21)
#define DWLP_MSGRESULT 0
#define DWLP_DLGPROC  (DWLP_MSGRESULT + (int)sizeof(LRESULT))
#define DWLP_USER     (DWLP_DLGPROC + (int)sizeof(DLGPROC))
#define DWL_MSGRESULT 0
#define DWL_DLGPROC   4
#define DWL_USER      8
#define GCL_STYLE     (-26)
#define GCLP_HBRBACKGROUND (-10)
#define GCLP_HCURSOR  (-12)
#define GCLP_HICON    (-14)
#define GCLP_HICONSM  (-34)
#define GCLP_WNDPROC  (-24)
#define GCW_ATOM      (-32)

/* hit testing */
#define HTERROR       (-2)
#define HTTRANSPARENT (-1)
#define HTNOWHERE     0
#define HTCLIENT      1
#define HTCAPTION     2
#define HTSYSMENU     3
#define HTMENU        5
#define HTHSCROLL     6
#define HTVSCROLL     7
#define HTMINBUTTON   8
#define HTMAXBUTTON   9
#define HTLEFT        10
#define HTRIGHT       11
#define HTTOP         12
#define HTTOPLEFT     13
#define HTTOPRIGHT    14
#define HTBOTTOM      15
#define HTBOTTOMLEFT  16
#define HTBOTTOMRIGHT 17
#define HTBORDER      18
#define HTCLOSE       20

/* WM_SYSCOMMAND */
#define SC_SIZE     0xF000
#define SC_MOVE     0xF010
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_NEXTWINDOW 0xF040
#define SC_PREVWINDOW 0xF050
#define SC_CLOSE    0xF060
#define SC_VSCROLL  0xF070
#define SC_HSCROLL  0xF080
#define SC_MOUSEMENU 0xF090
#define SC_KEYMENU  0xF100
#define SC_RESTORE  0xF120

/* WM_ACTIVATE */
#define WA_INACTIVE 0
#define WA_ACTIVE   1
#define WA_CLICKACTIVE 2
#define MA_ACTIVATE 1
#define MA_NOACTIVATE 3
/* WM_SIZE */
#define SIZE_RESTORED  0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
/* mouse */
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT   0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define MK_XBUTTON1 0x0020
#define MK_XBUTTON2 0x0040
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BROWSER_BACK        0xA6
#define VK_BROWSER_FORWARD     0xA7
#define VK_BROWSER_REFRESH     0xA8
#define VK_BROWSER_STOP        0xA9
#define VK_BROWSER_SEARCH      0xAA
#define VK_BROWSER_FAVORITES   0xAB
#define VK_BROWSER_HOME        0xAC
#define VK_VOLUME_MUTE         0xAD
#define VK_VOLUME_DOWN         0xAE
#define VK_VOLUME_UP           0xAF
#define VK_MEDIA_NEXT_TRACK    0xB0
#define VK_MEDIA_PREV_TRACK    0xB1
#define VK_MEDIA_STOP          0xB2
#define VK_MEDIA_PLAY_PAUSE    0xB3
#define VK_LAUNCH_MAIL         0xB4
#define VK_LAUNCH_MEDIA_SELECT 0xB5
#define VK_LAUNCH_APP1         0xB6
#define VK_LAUNCH_APP2         0xB7
#define VK_SLEEP               0x5F
#define WHEEL_DELTA 120
#define TME_HOVER  0x00000001
#define TME_LEAVE  0x00000002
#define TME_NONCLIENT 0x00000010
#define TME_CANCEL 0x80000000
#define HOVER_DEFAULT 0xFFFFFFFF
typedef struct tagTRACKMOUSEEVENT { DWORD cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;

#define VK_MBUTTON 0x04
#define VK_CLEAR   0x0C
#define VK_PAUSE   0x13
#define VK_SELECT  0x29
#define VK_SNAPSHOT 0x2C
#define VK_HELP    0x2F
#define VK_LWIN    0x5B
#define VK_RWIN    0x5C
#define VK_APPS    0x5D
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD     0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE  0x6F
#define VK_F2      0x71
#define VK_F3      0x72
#define VK_F4      0x73
#define VK_F5      0x74
#define VK_F6      0x75
#define VK_F7      0x76
#define VK_F8      0x77
#define VK_F9      0x78
#define VK_F10     0x79
#define VK_F11     0x7A
#define VK_NUMLOCK 0x90
#define VK_SCROLL  0x91
#define VK_LSHIFT  0xA0
#define VK_RSHIFT  0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU   0xA4
#define VK_RMENU   0xA5

/* DrawText */
#define DT_TOP          0x00000000
#define DT_BOTTOM       0x00000008
#define DT_EXPANDTABS   0x00000040
#define DT_TABSTOP      0x00000080
#define DT_NOCLIP       0x00000100
#define DT_EXTERNALLEADING 0x00000200
#define DT_CALCRECT     0x00000400
#define DT_NOPREFIX     0x00000800
#define DT_INTERNAL     0x00001000
#define DT_EDITCONTROL  0x00002000
#define DT_PATH_ELLIPSIS 0x00004000
#define DT_END_ELLIPSIS 0x00008000
#define DT_MODIFYSTRING 0x00010000
#define DT_RTLREADING   0x00020000
#define DT_WORD_ELLIPSIS 0x00040000
#define DT_NOFULLWIDTHCHARBREAK 0x00080000
#define DT_HIDEPREFIX   0x00100000
#define DT_PREFIXONLY   0x00200000

/* DrawEdge / DrawFrameControl */
#define BDR_RAISEDOUTER 0x0001
#define BDR_SUNKENOUTER 0x0002
#define BDR_RAISEDINNER 0x0004
#define BDR_SUNKENINNER 0x0008
#define EDGE_RAISED  (BDR_RAISEDOUTER | BDR_RAISEDINNER)
#define EDGE_SUNKEN  (BDR_SUNKENOUTER | BDR_SUNKENINNER)
#define EDGE_ETCHED  (BDR_SUNKENOUTER | BDR_RAISEDINNER)
#define EDGE_BUMP    (BDR_RAISEDOUTER | BDR_SUNKENINNER)
#define BF_LEFT   0x0001
#define BF_TOP    0x0002
#define BF_RIGHT  0x0004
#define BF_BOTTOM 0x0008
#define BF_RECT   (BF_LEFT | BF_TOP | BF_RIGHT | BF_BOTTOM)
#define BF_MIDDLE 0x0800
#define BF_SOFT   0x1000
#define BF_ADJUST 0x2000
#define BF_FLAT   0x4000
#define BF_MONO   0x8000
#define DFC_CAPTION 1
#define DFC_MENU    2
#define DFC_SCROLL  3
#define DFC_BUTTON  4
#define DFCS_BUTTONCHECK 0x0000
#define DFCS_BUTTONRADIO 0x0004
#define DFCS_BUTTONPUSH  0x0010
#define DFCS_SCROLLUP    0x0000
#define DFCS_SCROLLDOWN  0x0001
#define DFCS_SCROLLLEFT  0x0002
#define DFCS_SCROLLRIGHT 0x0003
#define DFCS_SCROLLCOMBOBOX 0x0005
#define DFCS_MENUARROW   0x0000
#define DFCS_MENUCHECK   0x0001
#define DFCS_MENUBULLET  0x0002
#define DFCS_INACTIVE    0x0100
#define DFCS_PUSHED      0x0200
#define DFCS_CHECKED     0x0400
#define DFCS_FLAT        0x4000

/* system colors */
#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_INACTIVECAPTION 3
#define COLOR_MENU 4
#define COLOR_WINDOWFRAME 6
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_CAPTIONTEXT 9
#define COLOR_ACTIVEBORDER 10
#define COLOR_INACTIVEBORDER 11
#define COLOR_APPWORKSPACE 12
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_3DFACE 15
#define COLOR_BTNSHADOW 16
#define COLOR_3DSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_BTNTEXT 18
#define COLOR_INACTIVECAPTIONTEXT 19
#define COLOR_BTNHIGHLIGHT 20
#define COLOR_3DHIGHLIGHT 20
#define COLOR_3DDKSHADOW 21
#define COLOR_3DLIGHT 22
#define COLOR_INFOTEXT 23
#define COLOR_INFOBK 24
#define COLOR_HOTLIGHT 26
#define COLOR_GRADIENTACTIVECAPTION 27
#define COLOR_MENUHILIGHT 29
#define COLOR_MENUBAR 30

/* buttons */
#define BS_PUSHBUTTON      0x0000
#define BS_DEFPUSHBUTTON   0x0001
#define BS_CHECKBOX        0x0002
#define BS_AUTOCHECKBOX    0x0003
#define BS_RADIOBUTTON     0x0004
#define BS_3STATE          0x0005
#define BS_AUTO3STATE      0x0006
#define BS_GROUPBOX        0x0007
#define BS_USERBUTTON      0x0008
#define BS_AUTORADIOBUTTON 0x0009
#define BS_PUSHBOX         0x000A
#define BS_OWNERDRAW       0x000B
#define BS_SPLITBUTTON     0x000C
#define BS_DEFSPLITBUTTON  0x000D
#define BS_COMMANDLINK     0x000E
#define BS_DEFCOMMANDLINK  0x000F
#define BS_TYPEMASK        0x000F
#define BS_LEFTTEXT        0x0020
#define BS_TEXT            0x0000
#define BS_ICON            0x0040
#define BS_BITMAP          0x0080
#define BS_LEFT            0x0100
#define BS_RIGHT           0x0200
#define BS_CENTER          0x0300
#define BS_TOP             0x0400
#define BS_BOTTOM          0x0800
#define BS_VCENTER         0x0C00
#define BS_PUSHLIKE        0x1000
#define BS_MULTILINE       0x2000
#define BS_NOTIFY          0x4000
#define BS_FLAT            0x8000
#define BN_CLICKED   0
#define BN_PAINT     1
#define BN_DISABLE   4
#define BN_DOUBLECLICKED 5
#define BN_SETFOCUS  6
#define BN_KILLFOCUS 7
#define BM_GETCHECK  0x00F0
#define BM_SETCHECK  0x00F1
#define BM_GETSTATE  0x00F2
#define BM_SETSTATE  0x00F3
#define BM_SETSTYLE  0x00F4
#define BM_CLICK     0x00F5
#define BM_GETIMAGE  0x00F6
#define BM_SETIMAGE  0x00F7
#define BM_SETDONTCLICK 0x00F8
#define BST_UNCHECKED 0x0000
#define BST_CHECKED   0x0001
#define BST_INDETERMINATE 0x0002
#define BST_PUSHED    0x0004
#define BST_FOCUS     0x0008
#define BST_HOT       0x0200

/* statics */
#define SS_LEFT        0x0000
#define SS_CENTER      0x0001
#define SS_RIGHT       0x0002
#define SS_ICON        0x0003
#define SS_BLACKRECT   0x0004
#define SS_GRAYRECT    0x0005
#define SS_WHITERECT   0x0006
#define SS_BLACKFRAME  0x0007
#define SS_GRAYFRAME   0x0008
#define SS_WHITEFRAME  0x0009
#define SS_USERITEM    0x000A
#define SS_SIMPLE      0x000B
#define SS_LEFTNOWORDWRAP 0x000C
#define SS_OWNERDRAW   0x000D
#define SS_BITMAP      0x000E
#define SS_ENHMETAFILE 0x000F
#define SS_ETCHEDHORZ  0x0010
#define SS_ETCHEDVERT  0x0011
#define SS_ETCHEDFRAME 0x0012
#define SS_TYPEMASK    0x001F
#define SS_REALSIZECONTROL 0x0040
#define SS_NOPREFIX    0x0080
#define SS_NOTIFY      0x0100
#define SS_CENTERIMAGE 0x0200
#define SS_RIGHTJUST   0x0400
#define SS_REALSIZEIMAGE 0x0800
#define SS_SUNKEN      0x1000
#define SS_EDITCONTROL 0x2000
#define SS_ENDELLIPSIS 0x4000
#define SS_PATHELLIPSIS 0x8000
#define SS_WORDELLIPSIS 0xC000
#define STM_SETICON  0x0170
#define STM_GETICON  0x0171
#define STM_SETIMAGE 0x0172
#define STM_GETIMAGE 0x0173
#define STN_CLICKED  0
#define STN_DBLCLK   1

/* edits */
#define ES_LEFT        0x0000
#define ES_CENTER      0x0001
#define ES_RIGHT       0x0002
#define ES_MULTILINE   0x0004
#define ES_UPPERCASE   0x0008
#define ES_LOWERCASE   0x0010
#define ES_PASSWORD    0x0020
#define ES_AUTOVSCROLL 0x0040
#define ES_AUTOHSCROLL 0x0080
#define ES_NOHIDESEL   0x0100
#define ES_OEMCONVERT  0x0400
#define ES_READONLY    0x0800
#define ES_WANTRETURN  0x1000
#define ES_NUMBER      0x2000
#define EN_SETFOCUS    0x0100
#define EN_KILLFOCUS   0x0200
#define EN_CHANGE      0x0300
#define EN_UPDATE      0x0400
#define EN_ERRSPACE    0x0500
#define EN_MAXTEXT     0x0501
#define EN_HSCROLL     0x0601
#define EN_VSCROLL     0x0602
#define EM_GETSEL      0x00B0
#define EM_SETSEL      0x00B1
#define EM_GETRECT     0x00B2
#define EM_SETRECT     0x00B3
#define EM_SETRECTNP   0x00B4
#define EM_SCROLL      0x00B5
#define EM_LINESCROLL  0x00B6
#define EM_SCROLLCARET 0x00B7
#define EM_GETMODIFY   0x00B8
#define EM_SETMODIFY   0x00B9
#define EM_GETLINECOUNT 0x00BA
#define EM_LINEINDEX   0x00BB
#define EM_SETHANDLE   0x00BC
#define EM_GETHANDLE   0x00BD
#define EM_GETTHUMB    0x00BE
#define EM_LINELENGTH  0x00C1
#define EM_REPLACESEL  0x00C2
#define EM_GETLINE     0x00C4
#define EM_LIMITTEXT   0x00C5
#define EM_SETLIMITTEXT 0x00C5
#define EM_CANUNDO     0x00C6
#define EM_UNDO        0x00C7
#define EM_FMTLINES    0x00C8
#define EM_LINEFROMCHAR 0x00C9
#define EM_SETTABSTOPS 0x00CB
#define EM_SETPASSWORDCHAR 0x00CC
#define EM_EMPTYUNDOBUFFER 0x00CD
#define EM_GETFIRSTVISIBLELINE 0x00CE
#define EM_SETREADONLY 0x00CF
#define EM_SETWORDBREAKPROC 0x00D0
#define EM_GETPASSWORDCHAR 0x00D2
#define EM_SETMARGINS  0x00D3
#define EM_GETMARGINS  0x00D4
#define EM_GETLIMITTEXT 0x00D5
#define EM_POSFROMCHAR 0x00D6
#define EM_CHARFROMPOS 0x00D7
#define EM_SETCUEBANNER 0x1501
#define EM_GETCUEBANNER 0x1502

/* list boxes */
#define LBS_NOTIFY          0x0001
#define LBS_SORT            0x0002
#define LBS_NOREDRAW        0x0004
#define LBS_MULTIPLESEL     0x0008
#define LBS_OWNERDRAWFIXED  0x0010
#define LBS_OWNERDRAWVARIABLE 0x0020
#define LBS_HASSTRINGS      0x0040
#define LBS_USETABSTOPS     0x0080
#define LBS_NOINTEGRALHEIGHT 0x0100
#define LBS_MULTICOLUMN     0x0200
#define LBS_WANTKEYBOARDINPUT 0x0400
#define LBS_EXTENDEDSEL     0x0800
#define LBS_DISABLENOSCROLL 0x1000
#define LBS_NODATA          0x2000
#define LBS_NOSEL           0x4000
#define LBS_COMBOBOX        0x8000
#define LBS_STANDARD        (LBS_NOTIFY | LBS_SORT | WS_VSCROLL | WS_BORDER)
#define LB_ERR   (-1)
#define LB_ERRSPACE (-2)
#define LB_ADDSTRING 0x0180
#define LB_INSERTSTRING 0x0181
#define LB_DELETESTRING 0x0182
#define LB_SELITEMRANGEEX 0x0183
#define LB_RESETCONTENT 0x0184
#define LB_SETSEL 0x0185
#define LB_SETCURSEL 0x0186
#define LB_GETSEL 0x0187
#define LB_GETCURSEL 0x0188
#define LB_GETTEXT 0x0189
#define LB_GETTEXTLEN 0x018A
#define LB_GETCOUNT 0x018B
#define LB_SELECTSTRING 0x018C
#define LB_DIR 0x018D
#define LB_GETTOPINDEX 0x018E
#define LB_FINDSTRING 0x018F
#define LB_GETSELCOUNT 0x0190
#define LB_GETSELITEMS 0x0191
#define LB_SETTABSTOPS 0x0192
#define LB_GETHORIZONTALEXTENT 0x0193
#define LB_SETHORIZONTALEXTENT 0x0194
#define LB_SETCOLUMNWIDTH 0x0195
#define LB_ADDFILE 0x0196
#define LB_SETTOPINDEX 0x0197
#define LB_GETITEMRECT 0x0198
#define LB_GETITEMDATA 0x0199
#define LB_SETITEMDATA 0x019A
#define LB_SELITEMRANGE 0x019B
#define LB_SETANCHORINDEX 0x019C
#define LB_GETANCHORINDEX 0x019D
#define LB_SETCARETINDEX 0x019E
#define LB_GETCARETINDEX 0x019F
#define LB_SETITEMHEIGHT 0x01A0
#define LB_GETITEMHEIGHT 0x01A1
#define LB_FINDSTRINGEXACT 0x01A2
#define LB_SETLOCALE 0x01A5
#define LB_GETLOCALE 0x01A6
#define LB_SETCOUNT 0x01A7
#define LB_INITSTORAGE 0x01A8
#define LB_ITEMFROMPOINT 0x01A9
#define LB_GETLISTBOXINFO 0x01B2
#define LBN_ERRSPACE (-2)
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2
#define LBN_SELCANCEL 3
#define LBN_SETFOCUS 4
#define LBN_KILLFOCUS 5

/* combo boxes */
#define CBS_SIMPLE          0x0001
#define CBS_DROPDOWN        0x0002
#define CBS_DROPDOWNLIST    0x0003
#define CBS_OWNERDRAWFIXED  0x0010
#define CBS_OWNERDRAWVARIABLE 0x0020
#define CBS_AUTOHSCROLL     0x0040
#define CBS_OEMCONVERT      0x0080
#define CBS_SORT            0x0100
#define CBS_HASSTRINGS      0x0200
#define CBS_NOINTEGRALHEIGHT 0x0400
#define CBS_DISABLENOSCROLL 0x0800
#define CBS_UPPERCASE       0x2000
#define CBS_LOWERCASE       0x4000
#define CB_ERR  (-1)
#define CB_ERRSPACE (-2)
#define CB_GETEDITSEL 0x0140
#define CB_LIMITTEXT 0x0141
#define CB_SETEDITSEL 0x0142
#define CB_ADDSTRING 0x0143
#define CB_DELETESTRING 0x0144
#define CB_DIR 0x0145
#define CB_GETCOUNT 0x0146
#define CB_GETCURSEL 0x0147
#define CB_GETLBTEXT 0x0148
#define CB_GETLBTEXTLEN 0x0149
#define CB_INSERTSTRING 0x014A
#define CB_RESETCONTENT 0x014B
#define CB_FINDSTRING 0x014C
#define CB_SELECTSTRING 0x014D
#define CB_SETCURSEL 0x014E
#define CB_SHOWDROPDOWN 0x014F
#define CB_GETITEMDATA 0x0150
#define CB_SETITEMDATA 0x0151
#define CB_GETDROPPEDCONTROLRECT 0x0152
#define CB_SETITEMHEIGHT 0x0153
#define CB_GETITEMHEIGHT 0x0154
#define CB_SETEXTENDEDUI 0x0155
#define CB_GETEXTENDEDUI 0x0156
#define CB_GETDROPPEDSTATE 0x0157
#define CB_FINDSTRINGEXACT 0x0158
#define CB_SETLOCALE 0x0159
#define CB_GETLOCALE 0x015A
#define CB_GETTOPINDEX 0x015B
#define CB_SETTOPINDEX 0x015C
#define CB_GETHORIZONTALEXTENT 0x015D
#define CB_SETHORIZONTALEXTENT 0x015E
#define CB_GETDROPPEDWIDTH 0x015F
#define CB_SETDROPPEDWIDTH 0x0160
#define CB_INITSTORAGE 0x0161
#define CB_GETCOMBOBOXINFO 0x0164
#define CB_SETMINVISIBLE 0x1701
#define CB_GETMINVISIBLE 0x1702
#define CB_SETCUEBANNER 0x1703
#define CBN_ERRSPACE (-1)
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define CBN_EDITUPDATE 6
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define CBN_SELENDOK 9
#define CBN_SELENDCANCEL 10

/* scroll bars */
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL  2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINELEFT 0
#define SB_LINEDOWN 1
#define SB_LINERIGHT 1
#define SB_PAGEUP 2
#define SB_PAGELEFT 2
#define SB_PAGEDOWN 3
#define SB_PAGERIGHT 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_LEFT 6
#define SB_BOTTOM 7
#define SB_RIGHT 7
#define SB_ENDSCROLL 8
#define SBS_HORZ 0x0000
#define SBS_VERT 0x0001
#define SBS_SIZEGRIP 0x0010
#define SBM_SETPOS 0x00E0
#define SBM_GETPOS 0x00E1
#define SBM_SETRANGE 0x00E2
#define SBM_SETRANGEREDRAW 0x00E6
#define SBM_GETRANGE 0x00E3
#define SBM_ENABLE_ARROWS 0x00E4
#define SBM_SETSCROLLINFO 0x00E9
#define SBM_GETSCROLLINFO 0x00EA
#define SBM_GETSCROLLBARINFO 0x00EB
#define SIF_RANGE 0x0001
#define SIF_PAGE 0x0002
#define SIF_POS 0x0004
#define SIF_DISABLENOSCROLL 0x0008
#define SIF_TRACKPOS 0x0010
#define SIF_ALL (SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS)
#define ESB_ENABLE_BOTH 0
#define ESB_DISABLE_BOTH 3
typedef struct tagSCROLLINFO { UINT cbSize, fMask; int nMin, nMax; UINT nPage; int nPos, nTrackPos; } SCROLLINFO, *LPSCROLLINFO;
typedef const SCROLLINFO *LPCSCROLLINFO;

/* dialogs */
#define DS_ABSALIGN 0x01
#define DS_SYSMODAL 0x02
#define DS_LOCALEDIT 0x20
#define DS_SETFONT 0x40
#define DS_MODALFRAME 0x80
#define DS_NOIDLEMSG 0x100
#define DS_SETFOREGROUND 0x200
#define DS_3DLOOK 0x0004
#define DS_FIXEDSYS 0x0008
#define DS_NOFAILCREATE 0x0010
#define DS_CONTROL 0x0400
#define DS_CENTER 0x0800
#define DS_CENTERMOUSE 0x1000
#define DS_CONTEXTHELP 0x2000
#define DS_SHELLFONT (DS_SETFONT | DS_FIXEDSYS)
#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTTAB 0x0002
#define DLGC_WANTALLKEYS 0x0004
#define DLGC_WANTMESSAGE 0x0004
#define DLGC_HASSETSEL 0x0008
#define DLGC_DEFPUSHBUTTON 0x0010
#define DLGC_UNDEFPUSHBUTTON 0x0020
#define DLGC_RADIOBUTTON 0x0040
#define DLGC_WANTCHARS 0x0080
#define DLGC_STATIC 0x0100
#define DLGC_BUTTON 0x2000
#define DM_GETDEFID (WM_USER + 0)
#define DM_SETDEFID (WM_USER + 1)
#define DM_REPOSITION (WM_USER + 2)
#define DC_HASDEFID 0x534B
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDCLOSE 8
#define IDHELP 9
#define IDTRYAGAIN 10
#define IDCONTINUE 11
#define IDTIMEOUT 32000
#define MB_ABORTRETRYIGNORE 0x2
#define MB_RETRYCANCEL 0x5
#define MB_CANCELTRYCONTINUE 0x6
#define MB_ICONHAND 0x10
#define MB_ICONSTOP 0x10
#define MB_ICONQUESTION 0x20
#define MB_ICONEXCLAMATION 0x30
#define MB_ICONWARNING 0x30
#define MB_ICONASTERISK 0x40
#define MB_DEFBUTTON2 0x100
#define MB_DEFBUTTON3 0x200
#define MB_APPLMODAL 0x0
#define MB_SYSTEMMODAL 0x1000
#define MB_TASKMODAL 0x2000
#define MB_SETFOREGROUND 0x10000
#define MB_TOPMOST 0x40000
#pragma pack(push, 2)
typedef struct { DWORD style, dwExtendedStyle; WORD cdit; short x, y, cx, cy; } DLGTEMPLATE, *LPDLGTEMPLATEW, *LPDLGTEMPLATEA;
typedef const DLGTEMPLATE *LPCDLGTEMPLATEW, *LPCDLGTEMPLATEA;
typedef struct { DWORD style, dwExtendedStyle; short x, y, cx, cy; WORD id; } DLGITEMTEMPLATE;
#pragma pack(pop)
typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);

/* menus */
#define MF_INSERT 0x0000
#define MF_CHANGE 0x0080
#define MF_APPEND 0x0100
#define MF_DELETE 0x0200
#define MF_REMOVE 0x1000
#define MF_BYCOMMAND 0x0000
#define MF_BYPOSITION 0x0400
#define MF_SEPARATOR 0x0800
#define MF_ENABLED 0x0000
#define MF_GRAYED 0x0001
#define MF_DISABLED 0x0002
#define MF_UNCHECKED 0x0000
#define MF_CHECKED 0x0008
#define MF_USECHECKBITMAPS 0x0200
#define MF_STRING 0x0000
#define MF_BITMAP 0x0004
#define MF_OWNERDRAW 0x0100
#define MF_POPUP 0x0010
#define MF_MENUBARBREAK 0x0020
#define MF_MENUBREAK 0x0040
#define MF_UNHILITE 0x0000
#define MF_HILITE 0x0080
#define MF_DEFAULT 0x1000
#define MF_SYSMENU 0x2000
#define MF_HELP 0x4000
#define MF_RIGHTJUSTIFY 0x4000
#define MF_MOUSESELECT 0x8000
#define MF_END 0x0080
#define MFT_STRING MF_STRING
#define MFT_BITMAP MF_BITMAP
#define MFT_MENUBARBREAK MF_MENUBARBREAK
#define MFT_MENUBREAK MF_MENUBREAK
#define MFT_OWNERDRAW MF_OWNERDRAW
#define MFT_RADIOCHECK 0x0200
#define MFT_SEPARATOR MF_SEPARATOR
#define MFT_RIGHTORDER 0x2000
#define MFT_RIGHTJUSTIFY MF_RIGHTJUSTIFY
#define MFS_GRAYED 0x0003
#define MFS_DISABLED MFS_GRAYED
#define MFS_CHECKED MF_CHECKED
#define MFS_HILITE MF_HILITE
#define MFS_ENABLED MF_ENABLED
#define MFS_UNCHECKED MF_UNCHECKED
#define MFS_UNHILITE MF_UNHILITE
#define MFS_DEFAULT MF_DEFAULT
#define MIIM_STATE 0x0001
#define MIIM_ID 0x0002
#define MIIM_SUBMENU 0x0004
#define MIIM_CHECKMARKS 0x0008
#define MIIM_TYPE 0x0010
#define MIIM_DATA 0x0020
#define MIIM_STRING 0x0040
#define MIIM_BITMAP 0x0080
#define MIIM_FTYPE 0x0100
#define TPM_LEFTBUTTON 0x0000
#define TPM_RIGHTBUTTON 0x0002
#define TPM_LEFTALIGN 0x0000
#define TPM_CENTERALIGN 0x0004
#define TPM_RIGHTALIGN 0x0008
#define TPM_TOPALIGN 0x0000
#define TPM_VCENTERALIGN 0x0010
#define TPM_BOTTOMALIGN 0x0020
#define TPM_NONOTIFY 0x0080
#define TPM_RETURNCMD 0x0100
#define TPM_RECURSE 0x0001
typedef struct tagMENUITEMINFOW {
    UINT cbSize, fMask, fType, fState, wID; HMENU hSubMenu; HBITMAP hbmpChecked, hbmpUnchecked;
    ULONG_PTR dwItemData; LPWSTR dwTypeData; UINT cch; HBITMAP hbmpItem;
} MENUITEMINFOW, *LPMENUITEMINFOW;
typedef const MENUITEMINFOW *LPCMENUITEMINFOW;
typedef struct tagMENUITEMINFOA {
    UINT cbSize, fMask, fType, fState, wID; HMENU hSubMenu; HBITMAP hbmpChecked, hbmpUnchecked;
    ULONG_PTR dwItemData; LPSTR dwTypeData; UINT cch; HBITMAP hbmpItem;
} MENUITEMINFOA, *LPMENUITEMINFOA;
typedef struct tagTPMPARAMS { UINT cbSize; RECT rcExclude; } TPMPARAMS, *LPTPMPARAMS;
#define FVIRTKEY 0x01
#define FNOINVERT 0x02
#define FSHIFT 0x04
#define FCONTROL 0x08
#define FALT 0x10
typedef struct tagACCEL { BYTE fVirt; WORD key; WORD cmd; } ACCEL, *LPACCEL;

/* owner draw */
#define ODT_MENU 1
#define ODT_LISTBOX 2
#define ODT_COMBOBOX 3
#define ODT_BUTTON 4
#define ODT_STATIC 5
#ifndef ODT_TAB
#define ODT_TAB 101
#endif
#define ODA_DRAWENTIRE 0x0001
#define ODA_SELECT 0x0002
#define ODA_FOCUS 0x0004
#define ODS_SELECTED 0x0001
#define ODS_GRAYED 0x0002
#define ODS_DISABLED 0x0004
#define ODS_CHECKED 0x0008
#define ODS_FOCUS 0x0010
#define ODS_DEFAULT 0x0020
#define ODS_COMBOBOXEDIT 0x1000
#define ODS_HOTLIGHT 0x0040
#define ODS_NOACCEL 0x0100
#define ODS_NOFOCUSRECT 0x0200
typedef struct tagDRAWITEMSTRUCT { UINT CtlType, CtlID, itemID, itemAction, itemState; HWND hwndItem; HDC hDC; RECT rcItem; ULONG_PTR itemData; } DRAWITEMSTRUCT, *LPDRAWITEMSTRUCT;
typedef struct tagMEASUREITEMSTRUCT { UINT CtlType, CtlID, itemID, itemWidth, itemHeight; ULONG_PTR itemData; } MEASUREITEMSTRUCT, *LPMEASUREITEMSTRUCT;
typedef struct tagDELETEITEMSTRUCT { UINT CtlType, CtlID, itemID; HWND hwndItem; ULONG_PTR itemData; } DELETEITEMSTRUCT;
typedef struct tagCOMPAREITEMSTRUCT { UINT CtlType, CtlID; HWND hwndItem; UINT itemID1; ULONG_PTR itemData1; UINT itemID2; ULONG_PTR itemData2; DWORD dwLocaleId; } COMPAREITEMSTRUCT;

/* structures */
typedef struct tagCREATESTRUCTW {
    LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent;
    int cy, cx, y, x; LONG style; LPCWSTR lpszName, lpszClass; DWORD dwExStyle;
} CREATESTRUCTW, *LPCREATESTRUCTW;
typedef struct tagWINDOWPOS { HWND hwnd, hwndInsertAfter; int x, y, cx, cy; UINT flags; } WINDOWPOS, *LPWINDOWPOS, *PWINDOWPOS;
typedef struct tagNCCALCSIZE_PARAMS { RECT rgrc[3]; PWINDOWPOS lppos; } NCCALCSIZE_PARAMS, *LPNCCALCSIZE_PARAMS;
typedef struct tagMINMAXINFO { POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; } MINMAXINFO, *LPMINMAXINFO;
typedef struct tagWINDOWPLACEMENT { UINT length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT, *LPWINDOWPLACEMENT;
typedef struct tagNMHDR { HWND hwndFrom; UINT_PTR idFrom; UINT code; } NMHDR, *LPNMHDR;
typedef struct tagSTYLESTRUCT { DWORD styleOld, styleNew; } STYLESTRUCT;
typedef struct tagHELPINFO { UINT cbSize; int iContextType, iCtrlId; HANDLE hItemHandle; DWORD_PTR dwContextId; POINT MousePos; } HELPINFO;
typedef struct tagCOMBOBOXINFO { DWORD cbSize; RECT rcItem, rcButton; DWORD stateButton; HWND hwndCombo, hwndItem, hwndList; } COMBOBOXINFO, *PCOMBOBOXINFO;
typedef struct tagICONINFO { BOOL fIcon; DWORD xHotspot, yHotspot; HBITMAP hbmMask, hbmColor; } ICONINFO, *PICONINFO;
typedef struct tagMSGBOXPARAMSW { UINT cbSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpszText, lpszCaption; DWORD dwStyle; LPCWSTR lpszIcon; DWORD_PTR dwContextHelpId; void *lpfnMsgBoxCallback; DWORD dwLanguageId; } MSGBOXPARAMSW;

#define MAKEINTRESOURCEW(i) ((LPWSTR)(ULONG_PTR)(WORD)(i))
#define MAKEINTRESOURCEA(i) ((LPSTR)(ULONG_PTR)(WORD)(i))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define IS_INTRESOURCE(r) ((((ULONG_PTR)(r)) >> 16) == 0)
#define RT_CURSOR       MAKEINTRESOURCEW(1)
#define RT_BITMAP       MAKEINTRESOURCEW(2)
#define RT_ICON         MAKEINTRESOURCEW(3)
#define RT_MENU         MAKEINTRESOURCEW(4)
#define RT_DIALOG       MAKEINTRESOURCEW(5)
#define RT_STRING       MAKEINTRESOURCEW(6)
#define RT_ACCELERATOR  MAKEINTRESOURCEW(9)
#define RT_GROUP_CURSOR MAKEINTRESOURCEW(12)
#define RT_GROUP_ICON   MAKEINTRESOURCEW(14)
#define RT_ANICURSOR    MAKEINTRESOURCEW(21)
#define RT_ANIICON      MAKEINTRESOURCEW(22)
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define LR_DEFAULTCOLOR 0x0000
#define LR_LOADFROMFILE 0x0010
#define LR_DEFAULTSIZE 0x0040
#define LR_SHARED 0x8000
#define LR_CREATEDIBSECTION 0x2000
#define DI_MASK 0x0001
#define DI_IMAGE 0x0002
#define DI_NORMAL 0x0003

#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYVTHUMB 9
#define SM_CXHTHUMB 10
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CYMENU 15
#define SM_CXVSCROLL_ 2
#define SM_CYVSCROLL 20
#define SM_CXHSCROLL 21
#define SM_CXEDGE 45
#define SM_CYEDGE 46
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXMIN 28
#define SM_CYMIN 29

USERAPI HWND    CreateDialogParamW(HINSTANCE inst, LPCWSTR tmpl, HWND parent, DLGPROC fn, LPARAM lp);
USERAPI INT_PTR DialogBoxParamW(HINSTANCE inst, LPCWSTR tmpl, HWND parent, DLGPROC fn, LPARAM lp);
USERAPI BOOL    EndDialog(HWND h, INT_PTR r);
USERAPI HWND    GetDlgItem(HWND h, int id);
USERAPI BOOL    SetDlgItemTextW(HWND h, int id, LPCWSTR s);
USERAPI UINT    GetDlgItemTextW(HWND h, int id, LPWSTR s, int n);
USERAPI LRESULT SendDlgItemMessageW(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL    IsDialogMessageW(HWND h, LPMSG m);
USERAPI BOOL    EnableWindow(HWND h, BOOL on);
USERAPI BOOL    MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint);
USERAPI BOOL    SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT flags);
USERAPI HMENU   CreateMenu(void);
USERAPI HMENU   CreatePopupMenu(void);
USERAPI BOOL    AppendMenuW(HMENU h, UINT f, UINT_PTR id, LPCWSTR s);
USERAPI BOOL    SetMenu(HWND h, HMENU m);
USERAPI BOOL    TrackPopupMenu(HMENU h, UINT f, int x, int y, int r, HWND w, const RECT *rc);
USERAPI BOOL    ClientToScreen(HWND h, LPPOINT p);
USERAPI BOOL    ScreenToClient(HWND h, LPPOINT p);
USERAPI HWND    GetParent(HWND h);
USERAPI BOOL    CheckDlgButton(HWND h, int id, UINT st);
USERAPI UINT    IsDlgButtonChecked(HWND h, int id);


/* ---- more of USER: types, constants and prototypes user32 itself uses ---- */
typedef HANDLE HACCEL;
typedef HANDLE HHOOK;
typedef HANDLE HWINEVENTHOOK;
typedef LRESULT (CALLBACK *HOOKPROC)(int, WPARAM, LPARAM);
typedef VOID (CALLBACK *WINEVENTPROC)(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
typedef struct tagWINDOWINFO { DWORD cbSize; RECT rcWindow, rcClient; DWORD dwStyle, dwExStyle, dwWindowStatus; UINT cxWindowBorders, cyWindowBorders; ATOM atomWindowType; WORD wCreatorVersion; } WINDOWINFO, *PWINDOWINFO, *LPWINDOWINFO;
typedef struct tagSCROLLBARINFO { DWORD cbSize; RECT rcScrollBar; int dxyLineButton, xyThumbTop, xyThumbBottom, reserved; DWORD rgstate[6]; } SCROLLBARINFO, *PSCROLLBARINFO, *LPSCROLLBARINFO;
typedef struct tagMENUBARINFO { DWORD cbSize; RECT rcBar; HMENU hMenu; HWND hwndMenu; BOOL fBarFocused:1; BOOL fFocused:1; BOOL fUnused:30; } MENUBARINFO, *PMENUBARINFO, *LPMENUBARINFO;
typedef struct tagGUITHREADINFO { DWORD cbSize, flags; HWND hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; RECT rcCaret; } GUITHREADINFO, *PGUITHREADINFO, *LPGUITHREADINFO;
typedef struct { UINT cbSize; HWND hwnd; DWORD dwFlags; UINT uCount; DWORD dwTimeout; } FLASHWINFO, *PFLASHWINFO;
typedef struct { DWORD cbSize, flags; HCURSOR hCursor; POINT ptScreenPos; } CURSORINFO, *PCURSORINFO, *LPCURSORINFO;
typedef struct tagMENUINFO { DWORD cbSize, fMask, dwStyle; UINT cyMax; HBRUSH hbrBack; DWORD dwContextHelpID; ULONG_PTR dwMenuData; } MENUINFO, *LPMENUINFO;
typedef const MENUINFO *LPCMENUINFO;
typedef struct tagDRAWTEXTPARAMS { UINT cbSize; int iTabLength, iLeftMargin, iRightMargin; UINT uiLengthDrawn; } DRAWTEXTPARAMS, *LPDRAWTEXTPARAMS;

#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)(DWORD)MAKELONG(l, h))
#define DLGWINDOWEXTRA 30
#define WM_SYSTIMER 0x0118
#define WM_NCMOUSELEAVE 0x02A2
#define WM_NCMBUTTONDBLCLK 0x00A9
#define WM_QUERYOPEN 0x0013
#define WM_QUERYNEWPALETTE 0x030F
#define WM_QUERYDROPOBJECT 0x022B
#define WM_DROPOBJECT 0x022A
#define WM_PRINT 0x0317
#ifndef PRF_CLIENT
#define PRF_CHECKVISIBLE 0x01
#define PRF_NONCLIENT    0x02
#define PRF_CLIENT       0x04
#define PRF_ERASEBKGND   0x08
#define PRF_CHILDREN     0x10
#define PRF_OWNED        0x20
#endif
#define WM_NEXTDLGCTL 0x0028
#define WM_ISACTIVEICON 0x0035
#define WM_INPUTLANGCHANGEREQUEST 0x0050
#define WM_GETOBJECT 0x003D
#define WM_DEVMODECHANGE 0x001B
#define WM_APPCOMMAND 0x0319
#define FAPPCOMMAND_MOUSE 0x8000
#define FAPPCOMMAND_KEY   0
#define APPCOMMAND_BROWSER_BACKWARD 1
#define APPCOMMAND_BROWSER_FORWARD  2
#define WH_MSGFILTER (-1)
#define WH_GETMESSAGE 3
#define HC_ACTION 0
#define HC_GETNEXT 1
#define HC_SKIP 2
#define HC_NOREMOVE 3
#define WH_CALLWNDPROC 4
#define WH_CBT 5
#define WH_KEYBOARD 2
#define WH_MOUSE 7
#define WH_KEYBOARD_LL 13
#define WH_MOUSE_LL 14
#define MSGF_DIALOGBOX 0
#define MSGF_MENU 2
#define VK_EXECUTE 0x2B
#define VK_CANCEL 0x03
#define UISF_HIDEFOCUS 1
#define UISF_HIDEACCEL 2
#define TME_QUERY 0x40000000
#define STN_ENABLE 2
#define STN_DISABLE 3
#define STATE_SYSTEM_UNAVAILABLE 0x00000001
#define STATE_SYSTEM_PRESSED 0x00000008
#define STATE_SYSTEM_INVISIBLE 0x00008000
#define SS_ELLIPSISMASK 0x0000C000
#define SM_CXMINTRACK 34
#define SM_CYMINTRACK 35
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_CXDRAG 68
#define SM_CYDRAG 69
#define RDW_INVALIDATE 0x0001
#define RDW_INTERNALPAINT 0x0002
#define RDW_ERASE 0x0004
#define RDW_VALIDATE 0x0008
#define RDW_NOINTERNALPAINT 0x0010
#define RDW_NOERASE 0x0020
#define RDW_NOCHILDREN 0x0040
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
#define RDW_FRAME 0x0400
#define RDW_NOFRAME 0x0800
#define QS_KEY 0x0001
#define QS_MOUSEMOVE 0x0002
#define QS_MOUSEBUTTON 0x0004
#define QS_POSTMESSAGE 0x0008
#define QS_TIMER 0x0010
#define QS_PAINT 0x0020
#define QS_SENDMESSAGE 0x0040
#define QS_HOTKEY 0x0080
#define QS_ALLPOSTMESSAGE 0x0100
#define QS_RAWINPUT 0x0400
#define QS_MOUSE (QS_MOUSEMOVE | QS_MOUSEBUTTON)
#define QS_INPUT (QS_MOUSE | QS_KEY | QS_RAWINPUT)
#define QS_ALLEVENTS (QS_INPUT | QS_POSTMESSAGE | QS_TIMER | QS_PAINT | QS_HOTKEY)
#define QS_ALLINPUT (QS_INPUT | QS_POSTMESSAGE | QS_TIMER | QS_PAINT | QS_HOTKEY | QS_SENDMESSAGE)
#define MWMO_WAITALL 0x0001
#define MWMO_ALERTABLE 0x0002
#define MWMO_INPUTAVAILABLE 0x0004
#define OBJID_WINDOW 0
#define OBJID_CLIENT ((LONG)0xFFFFFFFC)
#define OBJID_VSCROLL ((LONG)0xFFFFFFFB)
#define OBJID_HSCROLL ((LONG)0xFFFFFFFA)
#define OBJID_MENU ((LONG)0xFFFFFFFD)
#define NFR_ANSI 1
#define NFR_UNICODE 2
#define MNC_IGNORE 0
#define MNC_CLOSE 1
#define MNC_EXECUTE 2
#define MNC_SELECT 3
#define MIM_MAXHEIGHT 0x00000001
#define MIM_BACKGROUND 0x00000002
#define MIM_HELPID 0x00000004
#define MIM_MENUDATA 0x00000008
#define MIM_STYLE 0x00000010
#define MIM_APPLYTOSUBMENUS 0x80000000
#define MB_TYPEMASK 0x0000000F
#define MB_ICONMASK 0x000000F0
#define MB_DEFMASK 0x00000F00
#define MB_MODEMASK 0x00003000
#define MB_MISCMASK 0x0000C000
#define MB_HELP 0x00004000
#define IDI_HAND MAKEINTRESOURCEW(32513)
#define IDI_QUESTION MAKEINTRESOURCEW(32514)
#define IDI_EXCLAMATION MAKEINTRESOURCEW(32515)
#define IDI_ASTERISK MAKEINTRESOURCEW(32516)
#define IDI_WINLOGO MAKEINTRESOURCEW(32517)
#define IDI_SHIELD MAKEINTRESOURCEW(32518)
#define IDI_WARNING IDI_EXCLAMATION
#define IDI_ERROR IDI_HAND
#define IDI_INFORMATION IDI_ASTERISK
#define IDC_IBEAM MAKEINTRESOURCEW(32513)
#define IDC_WAIT MAKEINTRESOURCEW(32514)
#define IDC_CROSS MAKEINTRESOURCEW(32515)
#define IDC_UPARROW MAKEINTRESOURCEW(32516)
#define IDC_SIZENWSE MAKEINTRESOURCEW(32642)
#define IDC_SIZENESW MAKEINTRESOURCEW(32643)
#define IDC_SIZEWE MAKEINTRESOURCEW(32644)
#define IDC_SIZENS MAKEINTRESOURCEW(32645)
#define IDC_SIZEALL MAKEINTRESOURCEW(32646)
#define IDC_NO MAKEINTRESOURCEW(32648)
#define IDC_HAND MAKEINTRESOURCEW(32649)
#define IDC_APPSTARTING MAKEINTRESOURCEW(32650)
#define IDC_HELP MAKEINTRESOURCEW(32651)
#define ICON_SMALL 0
#define ICON_BIG 1
#define ICON_SMALL2 2
#define HTSIZE 4
#define GCL_CBWNDEXTRA (-18)
#define GCL_CBCLSEXTRA (-20)
#define GCLP_MENUNAME (-8)
#define GCLP_HMODULE (-16)
#define GCLP_HBRBACKGROUND (-10)
#define GCLP_HCURSOR (-12)
#define GCLP_HICON (-14)
#define GCLP_WNDPROC (-24)
#define GCLP_HICONSM (-34)
#define GCL_STYLE (-26)
#define GCW_ATOM (-32)
#define EM_SHOWBALLOONTIP 0x1503
#define EM_HIDEBALLOONTIP 0x1504
#define EM_SETIMESTATUS 0x00D8
#define EM_GETIMESTATUS 0x00D9
#define EM_GETWORDBREAKPROC 0x00D1
#define EC_LEFTMARGIN 0x0001
#define EC_RIGHTMARGIN 0x0002
#define EC_USEFONTINFO 0xFFFF
#define DST_COMPLEX 0x0000
#define DST_TEXT 0x0001
#define DST_PREFIXTEXT 0x0002
#define DST_ICON 0x0003
#define DST_BITMAP 0x0004
#define DSS_NORMAL 0x0000
#define DSS_UNION 0x0010
#define DSS_DISABLED 0x0020
#define DSS_MONO 0x0080
#define DSS_HIDEPREFIX 0x0200
#define DSS_PREFIXONLY 0x0400
#define DSS_RIGHT 0x8000
#define DFCS_BUTTONRADIOIMAGE 0x0001
#define DFCS_BUTTONRADIOMASK 0x0002
#define DFCS_BUTTON3STATE 0x0008
#define DFCS_SCROLLSIZEGRIP 0x0008
#define DFCS_SCROLLSIZEGRIPRIGHT 0x0010
#define DFCS_CAPTIONCLOSE 0x0000
#define DFCS_CAPTIONMIN 0x0001
#define DFCS_CAPTIONMAX 0x0002
#define DFCS_CAPTIONRESTORE 0x0003
#define DFCS_CAPTIONHELP 0x0004
#define DFCS_TRANSPARENT 0x0800
#define DFCS_HOT 0x1000
#define DCX_WINDOW 0x00000001
#define DCX_CACHE 0x00000002
#define DCX_NORESETATTRS 0x00000004
#define DCX_CLIPCHILDREN 0x00000008
#define DCX_CLIPSIBLINGS 0x00000010
#define DCX_PARENTCLIP 0x00000020
#define DCX_EXCLUDERGN 0x00000040
#define DCX_INTERSECTRGN 0x00000080
#define DCX_LOCKWINDOWUPDATE 0x00000400
#define CWP_ALL 0x0000
#define CWP_SKIPINVISIBLE 0x0001
#define CWP_SKIPDISABLED 0x0002
#define CWP_SKIPTRANSPARENT 0x0004
#define CURSOR_SHOWING 0x00000001
#define CB_SETCUEBANNER 0x1703
#define CB_GETCUEBANNER 0x1704
#define BCM_FIRST 0x1600
#define BCM_GETIDEALSIZE (BCM_FIRST + 0x0001)
#define BCM_SETIMAGELIST (BCM_FIRST + 0x0002)
#define BCM_GETIMAGELIST (BCM_FIRST + 0x0003)
#define BCM_SETTEXTMARGIN (BCM_FIRST + 0x0004)
#define BCM_GETTEXTMARGIN (BCM_FIRST + 0x0005)
#define BCM_SETDROPDOWNSTATE (BCM_FIRST + 0x0006)
#define BCM_SETSPLITINFO (BCM_FIRST + 0x0007)
#define BCM_GETSPLITINFO (BCM_FIRST + 0x0008)
#define BCM_SETNOTE (BCM_FIRST + 0x0009)
#define BCM_GETNOTE (BCM_FIRST + 0x000A)
#define BCM_GETNOTELENGTH (BCM_FIRST + 0x000B)
#define BCM_SETSHIELD (BCM_FIRST + 0x000C)
#define SBS_SIZEBOX 0x0008
#define SW_SCROLLCHILDREN 0x0001
#define SW_INVALIDATE 0x0002
#define SW_ERASE 0x0004
#define SW_SMOOTHSCROLL 0x0010
#define ESB_DISABLE_LTUP 0x0001
#define ESB_DISABLE_RTDN 0x0002
#define LR_COPYDELETEORG 0x00000008
#ifndef DI_DEFAULTSIZE
#define DI_DEFAULTSIZE 0x0008
#endif
#ifndef COLOR_3DHILIGHT
#define COLOR_3DHILIGHT 20
#endif

USERAPI HWND     SetCapture(HWND h);
USERAPI BOOL     ReleaseCapture(void);
USERAPI HWND     GetCapture(void);
USERAPI BOOL     TrackMouseEvent(LPTRACKMOUSEEVENT t);
USERAPI BOOL     SetRectEmpty(LPRECT r);
USERAPI BOOL     EqualRect(const RECT *a, const RECT *b);
USERAPI BOOL     SubtractRect(LPRECT d, const RECT *a, const RECT *b);
USERAPI BOOL     MessageBeep(UINT type);
USERAPI int      SetScrollInfo(HWND h, int bar, LPCSCROLLINFO si, BOOL redraw);
USERAPI BOOL     GetScrollInfo(HWND h, int bar, LPSCROLLINFO si);
USERAPI int      SetScrollPos(HWND h, int bar, int pos, BOOL redraw);
USERAPI int      GetScrollPos(HWND h, int bar);
USERAPI UINT     GetCaretBlinkTime(void);
USERAPI BOOL     CreateCaret(HWND h, HBITMAP b, int w, int hh);
USERAPI BOOL     DestroyCaret(void);
USERAPI BOOL     ShowCaret(HWND h);
USERAPI BOOL     HideCaret(HWND h);
USERAPI BOOL     SetCaretPos(int x, int y);
USERAPI LONG     GetDialogBaseUnits(void);
USERAPI BOOL     DestroyMenu(HMENU h);
USERAPI HMENU    LoadMenuW(HINSTANCE inst, LPCWSTR name);
USERAPI BOOL     CallMsgFilterW(LPMSG m, int code);
USERAPI BOOL     SetForegroundWindow(HWND h);
USERAPI HWND     GetActiveWindow(void);
USERAPI HANDLE   LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags);
USERAPI BOOL     AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex);
USERAPI BOOL     AdjustWindowRect(LPRECT r, DWORD style, BOOL menu);
USERAPI int      ToUnicode(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags);
USERAPI LONG     TabbedTextOutW(HDC dc, int x, int y, LPCWSTR s, int n, int nt, const INT *tabs, int org);
USERAPI BOOL     SetPropW(HWND h, LPCWSTR name, HANDLE data);
USERAPI HANDLE   GetPropW(HWND h, LPCWSTR name);
USERAPI HANDLE   RemovePropW(HWND h, LPCWSTR name);
USERAPI HCURSOR  SetCursor(HCURSOR c);
USERAPI BOOL     IsClipboardFormatAvailable(UINT fmt);
USERAPI BOOL     IsCharAlphaNumericW(WCHAR c);
USERAPI BOOL     IsCharAlphaW(WCHAR c);
USERAPI UINT     GetDoubleClickTime(void);
typedef BOOL (CALLBACK *DRAWSTATEPROC)(HDC, LPARAM, WPARAM, int, int);
USERAPI BOOL     DrawStateW(HDC dc, HBRUSH br, DRAWSTATEPROC fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT flags);
USERAPI BOOL     DrawFrameControl(HDC dc, LPRECT rc, UINT type, UINT state);
USERAPI BOOL     DrawEdge(HDC dc, LPRECT rc, UINT edge, UINT flags);
USERAPI BOOL     DestroyIcon(HICON h);
USERAPI DWORD    CharUpperBuffW(LPWSTR s, DWORD n);
USERAPI DWORD    CharLowerBuffW(LPWSTR s, DWORD n);
USERAPI BOOL     DrawFocusRect(HDC dc, const RECT *r);
USERAPI BOOL     InvalidateRgn(HWND h, HRGN rgn, BOOL erase);
USERAPI BOOL     ValidateRect(HWND h, const RECT *r);
USERAPI BOOL     RedrawWindow(HWND h, const RECT *r, HRGN rgn, UINT flags);
USERAPI HWND     GetWindow(HWND h, UINT cmd);
USERAPI BOOL     IsChild(HWND p, HWND c);
USERAPI HWND     GetAncestor(HWND h, UINT flags);
USERAPI BOOL     IsWindowEnabled(HWND h);
USERAPI HWND     GetNextDlgTabItem(HWND d, HWND c, BOOL prev);
USERAPI HWND     GetNextDlgGroupItem(HWND d, HWND c, BOOL prev);
USERAPI LRESULT  CALLBACK DefDlgProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI int      MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n);
USERAPI BOOL     OpenClipboard(HWND h);
USERAPI HICON    LoadIconW(HINSTANCE inst, LPCWSTR name);
USERAPI BOOL     KillTimer(HWND h, UINT_PTR id);
USERAPI BOOL     GetWindowRect(HWND h, LPRECT r);
USERAPI BOOL     IsDialogMessageA(HWND h, LPMSG m);
USERAPI SHORT    GetKeyState(int vk);


/* ---- every other function user32 exports ---- */
typedef DWORD_PTR *PDWORD_PTR;
USERAPI BOOL CheckRadioButton(HWND h, int first, int last, int check);
USERAPI BOOL GetComboBoxInfo(HWND h, PCOMBOBOXINFO ci);
USERAPI int DlgDirListComboBoxW(HWND h, LPWSTR path, int id, int st, UINT type);
USERAPI LRESULT DefFrameProcW(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT DefFrameProcA(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT DefMDIChildProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT DefMDIChildProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL TranslateMDISysAccel(HWND h, LPMSG m);
USERAPI BOOL MapDialogRect(HWND h, LPRECT r);
USERAPI HWND CreateDialogIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp);
USERAPI HWND CreateDialogIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA t, HWND p, DLGPROC fn, LPARAM lp);
USERAPI HWND CreateDialogIndirectParamAorW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp, DWORD f);
USERAPI HWND CreateDialogParamA(HINSTANCE inst, LPCSTR name, HWND p, DLGPROC fn, LPARAM lp);
USERAPI INT_PTR DialogBoxIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp);
USERAPI INT_PTR DialogBoxIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA t, HWND p, DLGPROC fn, LPARAM lp);
USERAPI INT_PTR DialogBoxIndirectParamAorW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp, DWORD f);
USERAPI INT_PTR DialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND p, DLGPROC fn, LPARAM lp);
USERAPI LRESULT DefDlgProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI LRESULT SendDlgItemMessageA(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL SetDlgItemTextA(HWND h, int id, LPCSTR s);
USERAPI UINT GetDlgItemTextA(HWND h, int id, LPSTR s, int n);
USERAPI BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sign);
USERAPI UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sign);
USERAPI int MessageBoxExW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang);
USERAPI int MessageBoxExA(HWND h, LPCSTR text, LPCSTR caption, UINT type, WORD lang);
USERAPI int MessageBoxIndirectW(const MSGBOXPARAMSW *p);
USERAPI int MessageBoxIndirectA(const void *pa);
USERAPI int MessageBoxTimeoutW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang, DWORD ms);
USERAPI int MessageBoxTimeoutA(HWND h, LPCSTR text, LPCSTR caption, UINT type, WORD lang, DWORD ms);
USERAPI BOOL SetSysColors(int n, const INT *idx, const COLORREF *c);
USERAPI BOOL InvertRect(HDC dc, const RECT *r);
USERAPI int DrawTextExW(HDC dc, LPWSTR s, int len, LPRECT r, UINT fmt, LPDRAWTEXTPARAMS p);
USERAPI int DrawTextExA(HDC dc, LPSTR s, int len, LPRECT r, UINT fmt, LPDRAWTEXTPARAMS p);
USERAPI LONG TabbedTextOutA(HDC dc, int x, int y, LPCSTR s, int n, int nt, const INT *tabs, int org);
USERAPI DWORD GetTabbedTextExtentW(HDC dc, LPCWSTR s, int n, int nt, const INT *tabs);
USERAPI DWORD GetTabbedTextExtentA(HDC dc, LPCSTR s, int n, int nt, const INT *tabs);
USERAPI BOOL DrawStateA(HDC dc, HBRUSH br, DRAWSTATEPROC fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT flags);
USERAPI BOOL DrawCaption(HWND h, HDC dc, const RECT *r, UINT flags);
USERAPI BOOL DrawIconEx(HDC dc, int x, int y, HICON h, int cx, int cy, UINT step, HBRUSH br, UINT flags);
USERAPI BOOL DrawIcon(HDC dc, int x, int y, HICON h);
USERAPI int DlgDirListW(HWND h, LPWSTR path, int lb, int st, UINT type);
USERAPI BOOL DlgDirSelectExW(HWND h, LPWSTR s, int n, int id);
USERAPI DWORD GetListBoxInfo(HWND h);
USERAPI BOOL IsMenu(HMENU h);
USERAPI BOOL InsertMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s);
USERAPI BOOL InsertMenuA(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCSTR s);
USERAPI BOOL AppendMenuA(HMENU h, UINT f, UINT_PTR id, LPCSTR s);
USERAPI BOOL ModifyMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s);
USERAPI BOOL ModifyMenuA(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCSTR s);
USERAPI BOOL RemoveMenu(HMENU h, UINT pos, UINT f);
USERAPI BOOL DeleteMenu(HMENU h, UINT pos, UINT f);
USERAPI int GetMenuItemCount(HMENU h);
USERAPI UINT GetMenuItemID(HMENU h, int pos);
USERAPI HMENU GetSubMenu(HMENU h, int pos);
USERAPI int GetMenuStringW(HMENU h, UINT pos, LPWSTR s, int n, UINT f);
USERAPI int GetMenuStringA(HMENU h, UINT pos, LPSTR s, int n, UINT f);
USERAPI UINT GetMenuState(HMENU h, UINT pos, UINT f);
USERAPI DWORD CheckMenuItem(HMENU h, UINT id, UINT f);
USERAPI BOOL EnableMenuItem(HMENU h, UINT id, UINT f);
USERAPI BOOL CheckMenuRadioItem(HMENU h, UINT first, UINT last, UINT check, UINT f);
USERAPI BOOL HiliteMenuItem(HWND h, HMENU m, UINT pos, UINT f);
USERAPI BOOL SetMenuDefaultItem(HMENU h, UINT item, UINT bypos);
USERAPI UINT GetMenuDefaultItem(HMENU h, UINT bypos, UINT flags);
USERAPI BOOL SetMenuItemBitmaps(HMENU h, UINT pos, UINT f, HBITMAP un, HBITMAP ch);
USERAPI LONG GetMenuCheckMarkDimensions(void);
USERAPI BOOL GetMenuItemInfoW(HMENU h, UINT item, BOOL bypos, LPMENUITEMINFOW mi);
USERAPI BOOL GetMenuItemInfoA(HMENU h, UINT item, BOOL bypos, LPMENUITEMINFOA mi);
USERAPI BOOL SetMenuItemInfoW(HMENU h, UINT item, BOOL bypos, LPCMENUITEMINFOW mi);
USERAPI BOOL SetMenuItemInfoA(HMENU h, UINT item, BOOL bypos, const MENUITEMINFOA *mi);
USERAPI BOOL InsertMenuItemW(HMENU h, UINT item, BOOL bypos, LPCMENUITEMINFOW mi);
USERAPI BOOL InsertMenuItemA(HMENU h, UINT item, BOOL bypos, const MENUITEMINFOA *mi);
USERAPI BOOL SetMenuInfo(HMENU h, LPCMENUINFO mi);
USERAPI BOOL GetMenuInfo(HMENU h, LPMENUINFO mi);
USERAPI BOOL SetMenuContextHelpId(HMENU h, DWORD id);
USERAPI DWORD GetMenuContextHelpId(HMENU h);
USERAPI HMENU GetMenu(HWND h);
USERAPI BOOL DrawMenuBar(HWND h);
USERAPI HMENU GetSystemMenu(HWND h, BOOL revert);
USERAPI BOOL GetMenuItemRect(HWND h, HMENU hm, UINT item, LPRECT r);
USERAPI int MenuItemFromPoint(HWND h, HMENU hm, POINT pt);
USERAPI BOOL GetMenuBarInfo(HWND h, LONG obj, LONG item, PMENUBARINFO mbi);
USERAPI BOOL EndMenu(void);
USERAPI BOOL TrackPopupMenuEx(HMENU hm, UINT flags, int x, int y, HWND h, LPTPMPARAMS p);
USERAPI HMENU LoadMenuIndirectW(const void *tmpl);
USERAPI HMENU LoadMenuIndirectA(const void *tmpl);
USERAPI HMENU LoadMenuA(HINSTANCE inst, LPCSTR name);
USERAPI HACCEL CreateAcceleratorTableW(LPACCEL a, int n);
USERAPI HACCEL CreateAcceleratorTableA(LPACCEL a, int n);
USERAPI BOOL DestroyAcceleratorTable(HACCEL h);
USERAPI int CopyAcceleratorTableW(HACCEL h, LPACCEL a, int n);
USERAPI int CopyAcceleratorTableA(HACCEL h, LPACCEL a, int n);
USERAPI HACCEL LoadAcceleratorsW(HINSTANCE inst, LPCWSTR name);
USERAPI HACCEL LoadAcceleratorsA(HINSTANCE inst, LPCSTR name);
USERAPI int TranslateAcceleratorW(HWND h, HACCEL ha, LPMSG m);
USERAPI int TranslateAcceleratorA(HWND h, HACCEL ha, LPMSG m);
USERAPI BOOL CopyRect(LPRECT d, const RECT *s);
USERAPI BOOL GetKeyboardState(PBYTE keys);
USERAPI BOOL SetKeyboardState(PBYTE keys);
typedef HANDLE HKL;
#define KL_NAMELENGTH 9
#define KLF_ACTIVATE 0x00000001
#define HKL_PREV 0
#define HKL_NEXT 1
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2
#define MAPVK_VSC_TO_VK_EX 3
#define WM_SYSDEADCHAR 0x0107
#define WM_INPUTLANGCHANGE 0x0051
#define SPI_GETDEFAULTINPUTLANG 0x0059
#define SPI_SETDEFAULTINPUTLANG 0x005A
USERAPI HKL GetKeyboardLayout(DWORD tid);
USERAPI int GetKeyboardLayoutList(int n, HKL *list);
USERAPI HKL LoadKeyboardLayoutW(LPCWSTR id, UINT f);
USERAPI HKL LoadKeyboardLayoutA(LPCSTR id, UINT f);
USERAPI BOOL UnloadKeyboardLayout(HKL h);
USERAPI HKL ActivateKeyboardLayout(HKL h, UINT f);
USERAPI BOOL GetKeyboardLayoutNameW(LPWSTR name);
USERAPI BOOL GetKeyboardLayoutNameA(LPSTR name);
USERAPI UINT MapVirtualKeyExA(UINT code, UINT type, HKL hkl);
USERAPI SHORT VkKeyScanExA(CHAR c, HKL hkl);
USERAPI int ToAsciiEx(UINT vk, UINT sc, const BYTE *keys, LPWORD out, UINT flags, HKL hkl);
USERAPI int GetKeyNameTextA(LONG lp, LPSTR buf, int n);
USERAPI int GetKeyboardType(int what);
USERAPI UINT MapVirtualKeyW(UINT code, UINT type);
USERAPI UINT MapVirtualKeyA(UINT code, UINT type);
USERAPI UINT MapVirtualKeyExW(UINT code, UINT type, HANDLE hkl);
USERAPI int ToUnicodeEx(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags, HANDLE hkl);
USERAPI int ToAscii(UINT vk, UINT sc, const BYTE *keys, LPWORD out, UINT flags);
USERAPI SHORT VkKeyScanW(WCHAR c);
USERAPI SHORT VkKeyScanA(CHAR c);
USERAPI SHORT VkKeyScanExW(WCHAR c, HANDLE hkl);
USERAPI int GetKeyNameTextW(LONG lp, LPWSTR buf, int n);
USERAPI UINT SendInput(UINT n, void *inputs, int size);
USERAPI void keybd_event(BYTE vk, BYTE sc, DWORD flags, ULONG_PTR extra);
USERAPI void mouse_event(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra);
USERAPI BOOL BlockInput(BOOL block);
USERAPI BOOL GetLastInputInfo(void *lii);
USERAPI BOOL RegisterHotKey(HWND h, int id, UINT mods, UINT vk);
USERAPI BOOL UnregisterHotKey(HWND h, int id);
USERAPI BOOL SetDoubleClickTime(UINT ms);
USERAPI BOOL SwapMouseButton(BOOL swap);
USERAPI LPSTR CharUpperA(LPSTR s);
USERAPI LPSTR CharLowerA(LPSTR s);
USERAPI DWORD CharUpperBuffA(LPSTR s, DWORD n);
USERAPI DWORD CharLowerBuffA(LPSTR s, DWORD n);
USERAPI LPWSTR CharNextW(LPCWSTR s);
USERAPI LPWSTR CharPrevW(LPCWSTR start, LPCWSTR s);
USERAPI LPSTR CharNextA(LPCSTR s);
USERAPI LPSTR CharPrevA(LPCSTR start, LPCSTR s);
USERAPI LPSTR CharPrevExA(WORD cp, LPCSTR start, LPCSTR s, DWORD flags);
USERAPI LPSTR CharNextExA(WORD cp, LPCSTR s, DWORD flags);
USERAPI BOOL IsCharUpperW(WCHAR c);
USERAPI BOOL IsCharLowerW(WCHAR c);
USERAPI BOOL IsCharAlphaA(CHAR c);
USERAPI BOOL IsCharAlphaNumericA(CHAR c);
USERAPI BOOL IsCharUpperA(CHAR c);
USERAPI BOOL IsCharLowerA(CHAR c);
USERAPI BOOL CharToOemA(LPCSTR s, LPSTR d);
USERAPI BOOL OemToCharA(LPCSTR s, LPSTR d);
USERAPI BOOL CharToOemBuffA(LPCSTR s, LPSTR d, DWORD n);
USERAPI BOOL OemToCharBuffA(LPCSTR s, LPSTR d, DWORD n);
USERAPI BOOL CharToOemW(LPCWSTR s, LPSTR d);
USERAPI BOOL OemToCharW(LPCSTR s, LPWSTR d);
USERAPI int wvsprintfA(LPSTR buf, LPCSTR fmt, va_list ap);
USERAPI int wvsprintfW(LPWSTR buf, LPCWSTR fmt, va_list ap);
USERAPI int GetSystemMetricsForDpi(int index, UINT dpi);
USERAPI BOOL SystemParametersInfoA(UINT action, UINT uparam, PVOID p, UINT winini);
USERAPI BOOL SystemParametersInfoForDpi(UINT action, UINT uparam, PVOID p, UINT winini, UINT dpi);
USERAPI UINT GetDpiForWindow(HWND h);
USERAPI UINT GetDpiForSystem(void);
USERAPI BOOL SetProcessDPIAware(void);
USERAPI BOOL IsProcessDPIAware(void);
USERAPI BOOL SetProcessDpiAwarenessContext(HANDLE ctx);
USERAPI HANDLE SetThreadDpiAwarenessContext(HANDLE ctx);
USERAPI HANDLE GetThreadDpiAwarenessContext(void);
USERAPI HANDLE GetWindowDpiAwarenessContext(HWND h);
USERAPI int GetAwarenessFromDpiAwarenessContext(HANDLE ctx);
USERAPI BOOL AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b);
USERAPI BOOL IsValidDpiAwarenessContext(HANDLE ctx);
USERAPI BOOL EnableNonClientDpiScaling(HWND h);
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_UNAWARE              ((DPI_AWARENESS_CONTEXT)(LONG_PTR)-1)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE         ((DPI_AWARENESS_CONTEXT)(LONG_PTR)-2)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE    ((DPI_AWARENESS_CONTEXT)(LONG_PTR)-3)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(LONG_PTR)-4)
#define DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED    ((DPI_AWARENESS_CONTEXT)(LONG_PTR)-5)
typedef enum { DPI_AWARENESS_INVALID = -1, DPI_AWARENESS_UNAWARE = 0, DPI_AWARENESS_SYSTEM_AWARE = 1,
               DPI_AWARENESS_PER_MONITOR_AWARE = 2 } DPI_AWARENESS;
USERAPI UINT GetDpiFromDpiAwarenessContext(HANDLE ctx);
typedef enum { DPI_HOSTING_BEHAVIOR_INVALID = -1, DPI_HOSTING_BEHAVIOR_DEFAULT = 0, DPI_HOSTING_BEHAVIOR_MIXED = 1 } DPI_HOSTING_BEHAVIOR;
USERAPI int SetThreadDpiHostingBehavior(int v);
USERAPI int GetThreadDpiHostingBehavior(void);
USERAPI int GetWindowDpiHostingBehavior(HWND h);
#define SPI_GETICONTITLELOGFONT 0x001F
#define SPI_GETNONCLIENTMETRICS 0x0029
#define SPI_GETICONMETRICS      0x002D
typedef struct { UINT cbSize; int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight; LOGFONTW lfCaptionFont;
                 int iSmCaptionWidth, iSmCaptionHeight; LOGFONTW lfSmCaptionFont; int iMenuWidth, iMenuHeight;
                 LOGFONTW lfMenuFont, lfStatusFont, lfMessageFont; int iPaddedBorderWidth; } NONCLIENTMETRICSW, *LPNONCLIENTMETRICSW;
typedef struct { UINT cbSize; int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight; LOGFONTA lfCaptionFont;
                 int iSmCaptionWidth, iSmCaptionHeight; LOGFONTA lfSmCaptionFont; int iMenuWidth, iMenuHeight;
                 LOGFONTA lfMenuFont, lfStatusFont, lfMessageFont; int iPaddedBorderWidth; } NONCLIENTMETRICSA, *LPNONCLIENTMETRICSA;
typedef struct { UINT cbSize; int iHorzSpacing, iVertSpacing, iTitleWrap; LOGFONTW lfFont; } ICONMETRICSW, *LPICONMETRICSW;
typedef struct { UINT cbSize; int iHorzSpacing, iVertSpacing, iTitleWrap; LOGFONTA lfFont; } ICONMETRICSA, *LPICONMETRICSA;
USERAPI UINT GetSystemDpiForProcess(HANDLE p);
typedef HANDLE HMONITOR;
typedef BOOL (CALLBACK *MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO, *LPMONITORINFO;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; CHAR szDevice[32]; } MONITORINFOEXA, *LPMONITORINFOEXA;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; WCHAR szDevice[32]; } MONITORINFOEXW, *LPMONITORINFOEXW;
#define MONITORINFOF_PRIMARY      0x00000001
#define MONITOR_DEFAULTTONULL     0x00000000
#define MONITOR_DEFAULTTOPRIMARY  0x00000001
#define MONITOR_DEFAULTTONEAREST  0x00000002
typedef struct { DWORD cb; CHAR DeviceName[32]; CHAR DeviceString[128]; DWORD StateFlags;
                 CHAR DeviceID[128]; CHAR DeviceKey[128]; } DISPLAY_DEVICEA, *PDISPLAY_DEVICEA;
typedef struct { DWORD cb; WCHAR DeviceName[32]; WCHAR DeviceString[128]; DWORD StateFlags;
                 WCHAR DeviceID[128]; WCHAR DeviceKey[128]; } DISPLAY_DEVICEW, *PDISPLAY_DEVICEW;
#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x00000001
#define DISPLAY_DEVICE_PRIMARY_DEVICE      0x00000004
#define DISPLAY_DEVICE_ACTIVE              0x00000001
#define DISPLAY_DEVICE_ATTACHED            0x00000002
USERAPI BOOL EnumDisplayMonitors(HDC dc, LPCRECT clip, MONITORENUMPROC fn, LPARAM lp);
USERAPI BOOL EnumDisplayDevicesA(LPCSTR dev, DWORD i, void *dd, DWORD flags);
USERAPI HANDLE MonitorFromWindow(HWND h, DWORD f);
USERAPI HANDLE MonitorFromPoint(POINT p, DWORD f);
USERAPI HANDLE MonitorFromRect(const RECT *r, DWORD f);
USERAPI BOOL GetMonitorInfoW(HANDLE m, void *mi);
USERAPI BOOL GetMonitorInfoA(HANDLE m, void *mi);
USERAPI BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, void *dm);
USERAPI BOOL EnumDisplaySettingsA(LPCSTR dev, DWORD mode, void *dm);
USERAPI BOOL EnumDisplaySettingsExW(LPCWSTR dev, DWORD mode, void *dm, DWORD flags);
USERAPI BOOL EnumDisplaySettingsExA(LPCSTR dev, DWORD mode, void *dm, DWORD flags);
USERAPI BOOL EnumDisplayDevicesW(LPCWSTR dev, DWORD i, void *dd, DWORD flags);
USERAPI LONG ChangeDisplaySettingsW(void *dm, DWORD f);
USERAPI LONG ChangeDisplaySettingsExW(LPCWSTR d, void *dm, HWND h, DWORD f, void *p);
USERAPI LONG ChangeDisplaySettingsA(void *dm, DWORD f);
USERAPI LONG ChangeDisplaySettingsExA(LPCSTR d, void *dm, HWND h, DWORD f, void *p);
typedef struct {
    WCHAR dmDeviceName[32];
    WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    struct { LONG x, y; } dmPosition;
    DWORD dmDisplayOrientation, dmDisplayFixedOutput;
    short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    WCHAR dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2,
          dmPanningWidth, dmPanningHeight;
} DEVMODEW, *PDEVMODEW, *LPDEVMODEW;
typedef struct {
    BYTE  dmDeviceName[32];
    WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    struct { LONG x, y; } dmPosition;
    DWORD dmDisplayOrientation, dmDisplayFixedOutput;
    short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    BYTE  dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2,
          dmPanningWidth, dmPanningHeight;
} DEVMODEA, *PDEVMODEA, *LPDEVMODEA;
#define DM_BITSPERPEL          0x00040000
#define DM_PELSWIDTH           0x00080000
#define DM_PELSHEIGHT          0x00100000
#define DM_DISPLAYFLAGS        0x00200000
#define DM_DISPLAYFREQUENCY    0x00400000
#define DM_POSITION            0x00000020
#define ENUM_CURRENT_SETTINGS  ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#define CDS_UPDATEREGISTRY     0x00000001
#define CDS_TEST               0x00000002
#define CDS_FULLSCREEN         0x00000004
#define DISP_CHANGE_SUCCESSFUL 0
#define DISP_CHANGE_FAILED     (-1)
#define DISP_CHANGE_BADMODE    (-2)
#define DISP_CHANGE_BADPARAM   (-5)
#ifndef WM_DISPLAYCHANGE
#define WM_DISPLAYCHANGE       0x007E
#endif
USERAPI int CountClipboardFormats(void);
USERAPI UINT EnumClipboardFormats(UINT fmt);
USERAPI HWND GetClipboardOwner(void);
USERAPI HWND GetOpenClipboardWindow(void);
USERAPI DWORD GetClipboardSequenceNumber(void);
USERAPI BOOL AddClipboardFormatListener(HWND h);
USERAPI BOOL RemoveClipboardFormatListener(HWND h);
USERAPI int GetClipboardFormatNameW(UINT fmt, LPWSTR buf, int n);
USERAPI int GetClipboardFormatNameA(UINT fmt, LPSTR buf, int n);
USERAPI HANDLE GetProcessWindowStation(void);
USERAPI HANDLE GetThreadDesktop(DWORD tid);
USERAPI HANDLE OpenInputDesktop(DWORD f, BOOL inherit, DWORD access);
USERAPI HANDLE OpenDesktopW(LPCWSTR name, DWORD f, BOOL inherit, DWORD access);
USERAPI BOOL CloseDesktop(HANDLE h);
USERAPI BOOL SwitchDesktop(HANDLE h);
USERAPI BOOL SetThreadDesktop(HANDLE h);
USERAPI BOOL CloseWindowStation(HANDLE h);
USERAPI BOOL GetUserObjectInformationW(HANDLE h, int index, PVOID p, DWORD n, LPDWORD need);
USERAPI BOOL ExitWindowsEx(UINT flags, DWORD reason);
#define EWX_LOGOFF          0x00000000
#define EWX_SHUTDOWN        0x00000001
#define EWX_REBOOT          0x00000002
#define EWX_FORCE           0x00000004
#define EWX_POWEROFF        0x00000008
#define EWX_FORCEIFHUNG     0x00000010
#define EWX_RESTARTAPPS     0x00000040
#define EWX_HYBRID_SHUTDOWN 0x00400000
USERAPI BOOL LockWorkStation(void);
USERAPI BOOL ChangeWindowMessageFilterEx(HWND h, UINT msg, DWORD action, void *cf);
USERAPI BOOL ChangeWindowMessageFilter(UINT msg, DWORD f);
USERAPI BOOL RegisterTouchWindow(HWND h, ULONG f);
USERAPI HANDLE RegisterDeviceNotificationW(HANDLE r, LPVOID filter, DWORD f);
USERAPI BOOL UnregisterDeviceNotification(HANDLE h);
/* Raw input */
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#define WM_INPUT               0x00FF
#define RIM_INPUT              0
#define RIM_INPUTSINK          1
#define GET_RAWINPUT_CODE_WPARAM(w) ((w) & 0xff)
#define GIDC_ARRIVAL           1
#define GIDC_REMOVAL           2
#define RIM_TYPEMOUSE          0
#define RIM_TYPEKEYBOARD       1
#define RIM_TYPEHID            2
#define RID_INPUT              0x10000003
#define RID_HEADER             0x10000005
#define RIDI_PREPARSEDDATA     0x20000005
#define RIDI_DEVICENAME        0x20000007
#define RIDI_DEVICEINFO        0x2000000b
#define RIDEV_REMOVE           0x00000001
#define RIDEV_EXCLUDE          0x00000010
#define RIDEV_PAGEONLY         0x00000020
#define RIDEV_NOLEGACY         0x00000030
#define RIDEV_INPUTSINK        0x00000100
#define RIDEV_CAPTUREMOUSE     0x00000200
#define RIDEV_NOHOTKEYS        0x00000200
#define RIDEV_APPKEYS          0x00000400
#define RIDEV_EXINPUTSINK      0x00001000
#define RIDEV_DEVNOTIFY        0x00002000
typedef struct HRAWINPUT__ *HRAWINPUT;
typedef struct tagRAWINPUTHEADER { DWORD dwType; DWORD dwSize; HANDLE hDevice; WPARAM wParam; } RAWINPUTHEADER, *PRAWINPUTHEADER, *LPRAWINPUTHEADER;
typedef struct tagRAWMOUSE {
    USHORT usFlags;
    union { ULONG ulButtons; struct { USHORT usButtonFlags; USHORT usButtonData; }; };
    ULONG ulRawButtons;
    LONG lLastX, lLastY;
    ULONG ulExtraInformation;
} RAWMOUSE, *PRAWMOUSE, *LPRAWMOUSE;
#define MOUSE_MOVE_RELATIVE         0x00
#define MOUSE_MOVE_ABSOLUTE         0x01
#define MOUSE_VIRTUAL_DESKTOP       0x02
#define RI_MOUSE_LEFT_BUTTON_DOWN   0x0001
#define RI_MOUSE_LEFT_BUTTON_UP     0x0002
#define RI_MOUSE_RIGHT_BUTTON_DOWN  0x0004
#define RI_MOUSE_RIGHT_BUTTON_UP    0x0008
#define RI_MOUSE_MIDDLE_BUTTON_DOWN 0x0010
#define RI_MOUSE_MIDDLE_BUTTON_UP   0x0020
#define RI_MOUSE_BUTTON_4_DOWN      0x0040
#define RI_MOUSE_BUTTON_4_UP        0x0080
#define RI_MOUSE_BUTTON_5_DOWN      0x0100
#define RI_MOUSE_BUTTON_5_UP        0x0200
#define RI_MOUSE_WHEEL              0x0400
#define RI_MOUSE_HWHEEL             0x0800
#define RI_KEY_MAKE                 0
#define RI_KEY_BREAK                1
#define RI_KEY_E0                   2
#define RI_KEY_E1                   4
typedef struct tagRAWKEYBOARD { USHORT MakeCode, Flags, Reserved, VKey; UINT Message; ULONG ExtraInformation; } RAWKEYBOARD, *PRAWKEYBOARD, *LPRAWKEYBOARD;
typedef struct tagRAWHID { DWORD dwSizeHid; DWORD dwCount; BYTE bRawData[1]; } RAWHID, *PRAWHID, *LPRAWHID;
typedef struct tagRAWINPUT {
    RAWINPUTHEADER header;
    union { RAWMOUSE mouse; RAWKEYBOARD keyboard; RAWHID hid; } data;
} RAWINPUT, *PRAWINPUT, *LPRAWINPUT;
#ifdef _WIN64
#define RAWINPUT_ALIGN(x) (((ULONG_PTR)(x) + 7) & ~(ULONG_PTR)7)
#else
#define RAWINPUT_ALIGN(x) (((ULONG_PTR)(x) + 3) & ~(ULONG_PTR)3)
#endif
#define NEXTRAWINPUTBLOCK(ptr) ((PRAWINPUT)RAWINPUT_ALIGN((ULONG_PTR)((PBYTE)(ptr) + (ptr)->header.dwSize)))
typedef struct tagRID_DEVICE_INFO_MOUSE { DWORD dwId, dwNumberOfButtons, dwSampleRate; BOOL fHasHorizontalWheel; } RID_DEVICE_INFO_MOUSE;
typedef struct tagRID_DEVICE_INFO_KEYBOARD {
    DWORD dwType, dwSubType, dwKeyboardMode, dwNumberOfFunctionKeys, dwNumberOfIndicators, dwNumberOfKeysTotal;
} RID_DEVICE_INFO_KEYBOARD;
typedef struct tagRID_DEVICE_INFO_HID { DWORD dwVendorId, dwProductId, dwVersionNumber; USHORT usUsagePage, usUsage; } RID_DEVICE_INFO_HID;
typedef struct tagRID_DEVICE_INFO {
    DWORD cbSize, dwType;
    union { RID_DEVICE_INFO_MOUSE mouse; RID_DEVICE_INFO_KEYBOARD keyboard; RID_DEVICE_INFO_HID hid; };
} RID_DEVICE_INFO, *PRID_DEVICE_INFO, *LPRID_DEVICE_INFO;
typedef struct tagRAWINPUTDEVICE { USHORT usUsagePage, usUsage; DWORD dwFlags; HWND hwndTarget; } RAWINPUTDEVICE, *PRAWINPUTDEVICE, *LPRAWINPUTDEVICE;
typedef const RAWINPUTDEVICE *PCRAWINPUTDEVICE;
typedef struct tagRAWINPUTDEVICELIST { HANDLE hDevice; DWORD dwType; } RAWINPUTDEVICELIST, *PRAWINPUTDEVICELIST;
USERAPI BOOL RegisterRawInputDevices(PCRAWINPUTDEVICE d, UINT n, UINT cb);
USERAPI UINT GetRegisteredRawInputDevices(PRAWINPUTDEVICE d, PUINT n, UINT cb);
USERAPI UINT GetRawInputDeviceList(PRAWINPUTDEVICELIST list, PUINT n, UINT cb);
USERAPI UINT GetRawInputDeviceInfoW(HANDLE dev, UINT cmd, LPVOID data, PUINT size);
USERAPI UINT GetRawInputDeviceInfoA(HANDLE dev, UINT cmd, LPVOID data, PUINT size);
USERAPI UINT GetRawInputData(HRAWINPUT raw, UINT cmd, LPVOID data, PUINT size, UINT header);
USERAPI UINT GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header);
USERAPI LRESULT DefRawInputProc(PRAWINPUT *raw, INT n, UINT header);
#define GetRawInputDeviceInfo GetRawInputDeviceInfoA
USERAPI HANDLE RegisterPowerSettingNotification(HANDLE r, const GUID *g, DWORD f);
USERAPI BOOL UnregisterPowerSettingNotification(HANDLE h);
USERAPI BOOL GetPhysicalCursorPos(LPPOINT p);
USERAPI BOOL SetCursorPos(int x, int y);
USERAPI BOOL SetPhysicalCursorPos(int x, int y);
USERAPI BOOL ClipCursor(const RECT *r);
USERAPI BOOL GetClipCursor(LPRECT r);
USERAPI HCURSOR GetCursor(void);
USERAPI int ShowCursor(BOOL show);
USERAPI BOOL GetCursorInfo(PCURSORINFO ci);
USERAPI BOOL SetSystemCursor(HCURSOR c, DWORD id);
#define OCR_NORMAL      32512
#define OCR_IBEAM       32513
#define OCR_WAIT        32514
#define OCR_CROSS       32515
#define OCR_UP          32516
#define OCR_SIZENWSE    32642
#define OCR_SIZENESW    32643
#define OCR_SIZEWE      32644
#define OCR_SIZENS      32645
#define OCR_SIZEALL     32646
#define OCR_NO          32648
#define OCR_HAND        32649
#define OCR_APPSTARTING 32650
#define OCR_HELP        32651
#define SPI_SETCURSORS  0x0057
USERAPI LRESULT SendMessageTimeoutW(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT ms, PDWORD_PTR r);
USERAPI LRESULT SendMessageTimeoutA(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT ms, PDWORD_PTR r);
USERAPI BOOL SendNotifyMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL SendNotifyMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI BOOL InSendMessage(void);
USERAPI DWORD InSendMessageEx(LPVOID r);
USERAPI BOOL ReplyMessage(LRESULT r);
USERAPI BOOL PostThreadMessageA(DWORD tid, UINT msg, WPARAM wp, LPARAM lp);
USERAPI UINT_PTR SetCoalescableTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn, ULONG tol);
USERAPI BOOL WaitMessage(void);
USERAPI DWORD GetQueueStatus(UINT flags);
USERAPI BOOL GetInputState(void);
USERAPI LONG GetMessageTime(void);
USERAPI DWORD GetMessagePos(void);
USERAPI LPARAM GetMessageExtraInfo(void);
USERAPI LPARAM SetMessageExtraInfo(LPARAM lp);
USERAPI BOOL SetMessageQueue(int n);
USERAPI DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD wake_mask, DWORD flags);
USERAPI BOOL TranslateMessageEx(const MSG *m, UINT f);
USERAPI LRESULT CallWindowProcA(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp);
USERAPI UINT RegisterWindowMessageA(LPCSTR name);
USERAPI UINT RegisterClipboardFormatW(LPCWSTR name);
USERAPI UINT RegisterClipboardFormatA(LPCSTR name);
USERAPI HHOOK SetWindowsHookExW(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid);
USERAPI HHOOK SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid);
USERAPI HHOOK SetWindowsHookW(int id, HOOKPROC fn);
USERAPI HHOOK SetWindowsHookA(int id, HOOKPROC fn);
USERAPI BOOL UnhookWindowsHookEx(HHOOK h);
USERAPI BOOL UnhookWindowsHook(int id, HOOKPROC fn);
USERAPI LRESULT CallNextHookEx(HHOOK h, int code, WPARAM wp, LPARAM lp);
USERAPI BOOL CallMsgFilterA(LPMSG m, int code);
USERAPI BOOL CallMsgFilter(LPMSG m, int code);
USERAPI HWINEVENTHOOK SetWinEventHook(DWORD a, DWORD b, HMODULE m, WINEVENTPROC fn, DWORD pid, DWORD tid, DWORD f);
USERAPI BOOL UnhookWinEvent(HWINEVENTHOOK h);
USERAPI void NotifyWinEvent(DWORD ev, HWND h, LONG obj, LONG child);
USERAPI BOOL IsWinEventHookInstalled(DWORD ev);
USERAPI BOOL AttachThreadInput(DWORD a, DWORD b, BOOL attach);
USERAPI DWORD WaitForInputIdle(HANDLE p, DWORD ms);
USERAPI BOOL GetGUIThreadInfo(DWORD tid, PGUITHREADINFO gi);
USERAPI BOOL ValidateRgn(HWND h, HRGN rgn);
USERAPI BOOL GetUpdateRect(HWND h, LPRECT r, BOOL erase);
USERAPI int GetUpdateRgn(HWND h, HRGN rgn, BOOL erase);
USERAPI int ExcludeUpdateRgn(HDC dc, HWND h);
USERAPI BOOL LockWindowUpdate(HWND h);
USERAPI HDC GetWindowDC(HWND h);
USERAPI HDC GetDCEx(HWND h, HRGN rgn, DWORD flags);
USERAPI HWND WindowFromDC(HDC dc);
USERAPI int ScrollWindowEx(HWND h, int dx, int dy, const RECT *scroll, const RECT *clip, HRGN rgn, LPRECT upd, UINT flags);
USERAPI BOOL ScrollWindow(HWND h, int dx, int dy, const RECT *r, const RECT *clip);
USERAPI BOOL ScrollDC(HDC dc, int dx, int dy, const RECT *scroll, const RECT *clip, HRGN rgn, LPRECT upd);
USERAPI BOOL GetCaretPos(LPPOINT p);
USERAPI BOOL SetCaretBlinkTime(UINT ms);
USERAPI BOOL PaintDesktop(HDC dc);
USERAPI int LookupIconIdFromDirectoryEx(PBYTE dir, BOOL icon, int cx, int cy, UINT flags);
USERAPI int LookupIconIdFromDirectory(PBYTE dir, BOOL icon);
USERAPI HICON CreateIconFromResourceEx(PBYTE bits, DWORD size, BOOL icon, DWORD ver, int cx, int cy, UINT flags);
USERAPI HICON CreateIconFromResource(PBYTE bits, DWORD size, BOOL icon, DWORD ver);
USERAPI HCURSOR LoadCursorFromFileW(LPCWSTR f);
USERAPI HCURSOR LoadCursorFromFileA(LPCSTR f);
USERAPI HCURSOR GetCursorFrameInfo(HCURSOR h, DWORD reserved, DWORD step, DWORD *rate, DWORD *nsteps);
USERAPI HBITMAP LoadBitmapW(HINSTANCE inst, LPCWSTR name);
USERAPI HBITMAP LoadBitmapA(HINSTANCE inst, LPCSTR name);
USERAPI HANDLE LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags);
USERAPI HICON CreateIconIndirect(PICONINFO ii);
USERAPI HICON CreateIcon(HINSTANCE inst, int w, int h, BYTE planes, BYTE bpp, const BYTE *and_mask, const BYTE *xor_mask);
USERAPI HCURSOR CreateCursor(HINSTANCE inst, int hx, int hy, int w, int h, const void *and_mask, const void *xor_mask);
USERAPI BOOL GetIconInfo(HICON h, PICONINFO ii);
USERAPI BOOL GetIconInfoExW(HICON h, void *ix);
USERAPI HICON CopyIcon(HICON h);
#define CopyCursor(c) ((HCURSOR)CopyIcon((HICON)(c)))
USERAPI HANDLE CopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags);
USERAPI BOOL DestroyCursor(HCURSOR h);
USERAPI UINT PrivateExtractIconsW(LPCWSTR file, int idx, int cx, int cy, HICON *icons, UINT *ids, UINT n, UINT flags);
USERAPI BOOL SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw);
USERAPI BOOL GetScrollRange(HWND h, int bar, LPINT mn, LPINT mx);
USERAPI BOOL ShowScrollBar(HWND h, int bar, BOOL show);
USERAPI BOOL EnableScrollBar(HWND h, UINT bar, UINT arrows);
USERAPI BOOL GetScrollBarInfo(HWND h, LONG id, PSCROLLBARINFO sbi);
USERAPI BOOL UnregisterClassW(LPCWSTR name, HINSTANCE inst);
USERAPI BOOL UnregisterClassA(LPCSTR name, HINSTANCE inst);
USERAPI BOOL GetClassInfoExW(HINSTANCE inst, LPCWSTR name, WNDCLASSEXW *wc);
USERAPI BOOL GetClassInfoW(HINSTANCE inst, LPCWSTR name, WNDCLASSW *wc);
USERAPI BOOL GetClassInfoExA(HINSTANCE inst, LPCSTR name, WNDCLASSEXA *wc);
USERAPI BOOL GetClassInfoA(HINSTANCE inst, LPCSTR name, WNDCLASSA *wc);
USERAPI int GetClassNameW(HWND h, LPWSTR buf, int n);
USERAPI int GetClassNameA(HWND h, LPSTR buf, int n);
USERAPI UINT RealGetWindowClassW(HWND h, LPWSTR buf, UINT n);
USERAPI UINT RealGetWindowClassA(HWND h, LPSTR buf, UINT n);
USERAPI ULONG_PTR GetClassLongPtrW(HWND h, int i);
USERAPI ULONG_PTR SetClassLongPtrW(HWND h, int i, LONG_PTR v);
USERAPI ULONG_PTR GetClassLongPtrA(HWND h, int i);
USERAPI ULONG_PTR SetClassLongPtrA(HWND h, int i, LONG_PTR v);
USERAPI DWORD GetClassLongW(HWND h, int i);
USERAPI DWORD GetClassLongA(HWND h, int i);
USERAPI DWORD SetClassLongW(HWND h, int i, LONG v);
USERAPI DWORD SetClassLongA(HWND h, int i, LONG v);
USERAPI WORD GetClassWord(HWND h, int i);
USERAPI BOOL ShowWindowAsync(HWND h, int cmd);
USERAPI BOOL ShowOwnedPopups(HWND h, BOOL show);
USERAPI BOOL IsWindowUnicode(HWND h);
USERAPI BOOL IsIconic(HWND h);
USERAPI BOOL IsZoomed(HWND h);
USERAPI HWND SetActiveWindow(HWND h);
USERAPI BOOL BringWindowToTop(HWND h);
USERAPI BOOL AllowSetForegroundWindow(DWORD pid);
USERAPI BOOL LockSetForegroundWindow(UINT code);
USERAPI void SwitchToThisWindow(HWND h, BOOL alt);
USERAPI HWND GetShellWindow(void);
USERAPI HWND GetTopWindow(HWND h);
USERAPI HWND GetNextWindow(HWND h, UINT cmd);
USERAPI HWND GetLastActivePopup(HWND h);
USERAPI HWND SetParent(HWND h, HWND hp);
USERAPI BOOL EnumChildWindows(HWND hp, WNDENUMPROC fn, LPARAM lp);
USERAPI BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp);
USERAPI BOOL EnumThreadWindows(DWORD tid, WNDENUMPROC fn, LPARAM lp);
USERAPI BOOL EnumDesktopWindows(HANDLE desk, WNDENUMPROC fn, LPARAM lp);
USERAPI HWND FindWindowExW(HWND hp, HWND after, LPCWSTR cls, LPCWSTR title);
USERAPI HWND FindWindowW(LPCWSTR cls, LPCWSTR title);
USERAPI HWND FindWindowExA(HWND hp, HWND after, LPCSTR cls, LPCSTR title);
USERAPI HWND FindWindowA(LPCSTR cls, LPCSTR title);
USERAPI DWORD GetWindowThreadProcessId(HWND h, LPDWORD pid);
USERAPI BOOL GetWindowInfo(HWND h, PWINDOWINFO wi);
USERAPI BOOL AdjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD ex, UINT dpi);
USERAPI HANDLE BeginDeferWindowPos(int n);
USERAPI HANDLE DeferWindowPos(HANDLE hd, HWND h, HWND after, int x, int y, int cx, int cy, UINT f);
USERAPI BOOL EndDeferWindowPos(HANDLE hd);
USERAPI BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *p);
USERAPI BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *p);
USERAPI BOOL CloseWindow(HWND h);
USERAPI BOOL OpenIcon(HWND h);
USERAPI HWND ChildWindowFromPointEx(HWND h, POINT pt, UINT flags);
USERAPI HWND ChildWindowFromPoint(HWND h, POINT pt);
USERAPI HWND RealChildWindowFromPoint(HWND h, POINT pt);
USERAPI HWND WindowFromPoint(POINT pt);
USERAPI HWND WindowFromPhysicalPoint(POINT pt);
USERAPI int GetWindowTextLengthW(HWND h);
USERAPI int GetWindowTextLengthA(HWND h);
USERAPI int InternalGetWindowText(HWND h, LPWSTR s, int max);
USERAPI LONG GetWindowLongW(HWND h, int i);
USERAPI LONG GetWindowLongA(HWND h, int i);
USERAPI LONG SetWindowLongW(HWND h, int i, LONG v);
USERAPI LONG SetWindowLongA(HWND h, int i, LONG v);
USERAPI WORD GetWindowWord(HWND h, int i);
USERAPI WORD SetWindowWord(HWND h, int i, WORD v);
USERAPI int GetDlgCtrlID(HWND h);
USERAPI int SetDlgCtrlID(HWND h, int id);
USERAPI BOOL SetPropA(HWND h, LPCSTR name, HANDLE data);
USERAPI HANDLE GetPropA(HWND h, LPCSTR name);
USERAPI HANDLE RemovePropA(HWND h, LPCSTR name);
USERAPI BOOL FlashWindow(HWND h, BOOL invert);
USERAPI BOOL FlashWindowEx(PFLASHWINFO fi);
USERAPI BOOL IsHungAppWindow(HWND h);
USERAPI BOOL SetLayeredWindowAttributes(HWND h, COLORREF key, BYTE alpha, DWORD f);
USERAPI BOOL GetLayeredWindowAttributes(HWND h, COLORREF *key, BYTE *alpha, DWORD *f);
USERAPI BOOL UpdateLayeredWindow(HWND h, HDC d, POINT *p, SIZE *s, HDC src, POINT *sp, COLORREF k, void *bf, DWORD f);
USERAPI BOOL SetWindowDisplayAffinity(HWND h, DWORD a);
USERAPI BOOL GetWindowDisplayAffinity(HWND h, DWORD *a);
USERAPI HWND GetProgmanWindow(void);
USERAPI BOOL SetWindowContextHelpId(HWND h, DWORD id);
USERAPI DWORD GetWindowContextHelpId(HWND h);
USERAPI BOOL IsWindowArranged(HWND h);
USERAPI UINT ArrangeIconicWindows(HWND h);
USERAPI BOOL LogicalToPhysicalPoint(HWND h, LPPOINT p);
USERAPI BOOL PhysicalToLogicalPoint(HWND h, LPPOINT p);
USERAPI BOOL LogicalToPhysicalPointForPerMonitorDPI(HWND h, LPPOINT p);
USERAPI BOOL PhysicalToLogicalPointForPerMonitorDPI(HWND h, LPPOINT p);
USERAPI BOOL GetTitleBarInfo(HWND h, void *ti);
USERAPI BOOL DragDetect(HWND h, POINT pt);
USERAPI BOOL AnimateWindow(HWND h, DWORD t, DWORD f);
USERAPI int SetWindowRgn(HWND h, HRGN r, BOOL redraw);
USERAPI int GetWindowRgn(HWND h, HRGN r);
USERAPI int GetWindowRgnBox(HWND h, LPRECT r);

_NOVA_END

/* shellapi.h: a WM_DROPFILES handle's contents */
typedef struct _DROPFILES { DWORD pFiles; POINT pt; BOOL fNC; BOOL fWide; } DROPFILES, *LPDROPFILES;
typedef HANDLE HDROP;
