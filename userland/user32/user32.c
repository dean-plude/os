/*
 * user32.dll — windows, the message loop and the rest of USER
 *
 * A window is created in the desktop's window manager through the kernel's
 * GUI syscalls; user32 keeps the class table and per-window state (window
 * procedure, styles, user data, extra bytes, and the device context that
 * references the window's client bitmap).  The message loop pulls Win32
 * messages the kernel queued from real desktop input; messages posted to
 * the thread (no window) and ones peeked without removal wait in a local
 * queue.
 *
 * Every NovaOS window is top-level: there are no child windows, controls,
 * dialogs or menus in the window system, so those calls fail or do
 * nothing, as documented at each.
 */
#define NOVA_BUILD_USER32
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
/* from gdi32 */
__declspec(dllimport) void NovaGdiFill(HDC, int, int, int, int, COLORREF);
__declspec(dllimport) int  NovaGdiCellW(void);
__declspec(dllimport) int  NovaGdiCellH(void);
__declspec(dllimport) void NovaGdiChar(HDC, int, int, char);

/* Kernel GuiCreate struct (matches um_gui.c) */
typedef struct {
    INT32 x, y, w, h; UINT32 style; UINT64 title;
    UINT64 hwnd, bitmap; UINT32 stride, cw, ch;
    UINT32 flags; UINT64 owner;
} GuiCreate;

#define MAX_CLASSES 64
#define MAX_WINDOWS 32
#define EXTRA_MAX   64

typedef struct {
    char name[64];
    WNDPROC proc;
    COLORREF bg;
    HBRUSH brush;
    HICON icon;
    HCURSOR cursor;
    HINSTANCE inst;
    UINT style;
    int extra;
    int wide;
    int used;
} WClass;

typedef struct {
    int      used;
    UINT32   id;
    WNDPROC  proc;
    NOVA_DC  dc;
    COLORREF bg;
    char     text[256];                 /* UTF-8 */
    void    *param;
    WClass  *cls;
    LONG_PTR userdata, id_menu;
    DWORD    style, exstyle, tid;
    int      wide, visible, enabled;
    BYTE     extra[EXTRA_MAX];
} WInfo;

static WClass g_class[MAX_CLASSES];
static WInfo  g_win[MAX_WINDOWS];
static int    g_quit, g_quit_code;

static int u8_to_w(const char *s, WCHAR *w, int cap) { int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, cap); if (n <= 0) { w[0] = 0; return 0; } return n - 1; }
static int w_to_u8(const WCHAR *w, char *s, int cap) { int n = WideCharToMultiByte(CP_UTF8, 0, w ? w : (const WCHAR[]){ 0 }, -1, s, cap, 0, 0); if (n <= 0) { s[0] = 0; return 0; } return n - 1; }
static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static WInfo *win_of(HWND h)
{
    UINT32 id = (UINT32)(ULONG_PTR)h;
    if (!id) return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].id == id) return &g_win[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * Window classes
 * ----------------------------------------------------------------------- */
static WClass *find_class(const char *name)
{
    if (!name) return NULL;
    if ((ULONG_PTR)name < 0x10000) {                        /* an atom */
        int i = (int)(ULONG_PTR)name - 1;
        return i >= 0 && i < MAX_CLASSES && g_class[i].used ? &g_class[i] : NULL;
    }
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (!g_class[i].used) continue;
        const char *a = g_class[i].name, *b = name;
        while (*a && ((*a | 32) == (*b | 32))) { a++; b++; }
        if (!*a && !*b) return &g_class[i];
    }
    return NULL;
}

static ATOM register_class(const char *name, WNDPROC proc, HBRUSH bg, HICON icon, HCURSOR cur, HINSTANCE inst, UINT style, int extra, int wide)
{
    if (find_class(name)) { SetLastError(1410 /* ERROR_CLASS_ALREADY_EXISTS */); return 0; }
    for (int i = 0; i < MAX_CLASSES; i++) if (!g_class[i].used) {
        WClass *c = &g_class[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->proc = proc;
        c->brush = bg;
        /* a brush handle or COLOR_* + 1; the window's background color */
        c->bg = (ULONG_PTR)bg > 0 && (ULONG_PTR)bg <= 31 ? GetSysColor((int)(ULONG_PTR)bg - 1) :
                bg ? ((COLORREF *)bg)[1] : 0xF3F3F3;
        c->icon = icon; c->cursor = cur; c->inst = inst; c->style = style;
        c->extra = extra < 0 ? 0 : extra > EXTRA_MAX ? EXTRA_MAX : extra;
        c->wide = wide;
        int n = 0;
        if (name && (ULONG_PTR)name >= 0x10000) for (; name[n] && n < 63; n++) c->name[n] = name[n];
        c->name[n] = 0;
        return (ATOM)(i + 1);
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

USERAPI ATOM RegisterClassA(const WNDCLASSA *wc)
{
    return register_class(wc->lpszClassName, wc->lpfnWndProc, wc->hbrBackground, wc->hIcon, wc->hCursor, wc->hInstance, wc->style, wc->cbWndExtra, 0);
}

USERAPI ATOM RegisterClassExA(const WNDCLASSEXA *wc)
{
    return register_class(wc->lpszClassName, wc->lpfnWndProc, wc->hbrBackground, wc->hIcon, wc->hCursor, wc->hInstance, wc->style, wc->cbWndExtra, 0);
}

USERAPI ATOM RegisterClassW(const WNDCLASSW *wc)
{
    char n[64];
    if ((ULONG_PTR)wc->lpszClassName < 0x10000) return 0;
    w_to_u8(wc->lpszClassName, n, sizeof(n));
    return register_class(n, wc->lpfnWndProc, wc->hbrBackground, wc->hIcon, wc->hCursor, wc->hInstance, wc->style, wc->cbWndExtra, 1);
}

USERAPI ATOM RegisterClassExW(const WNDCLASSEXW *wc)
{
    char n[64];
    if ((ULONG_PTR)wc->lpszClassName < 0x10000) return 0;
    w_to_u8(wc->lpszClassName, n, sizeof(n));
    return register_class(n, wc->lpfnWndProc, wc->hbrBackground, wc->hIcon, wc->hCursor, wc->hInstance, wc->style, wc->cbWndExtra, 1);
}

USERAPI BOOL UnregisterClassA(LPCSTR name, HINSTANCE inst)
{
    (void)inst;
    WClass *c = find_class(name);
    if (!c) { SetLastError(1411 /* ERROR_CLASS_DOES_NOT_EXIST */); return FALSE; }
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].cls == c) { SetLastError(1412 /* ERROR_CLASS_HAS_WINDOWS */); return FALSE; }
    c->used = 0;
    return TRUE;
}

USERAPI BOOL UnregisterClassW(LPCWSTR name, HINSTANCE inst)
{
    char n[64];
    if ((ULONG_PTR)name < 0x10000) return UnregisterClassA((LPCSTR)name, inst);
    w_to_u8(name, n, sizeof(n));
    return UnregisterClassA(n, inst);
}

USERAPI BOOL GetClassInfoExA(HINSTANCE inst, LPCSTR name, WNDCLASSEXA *wc)
{
    (void)inst;
    WClass *c = find_class(name);
    if (!c) { SetLastError(1411); return FALSE; }
    memset((BYTE *)wc + 4, 0, sizeof(*wc) - 4);
    wc->style = c->style; wc->lpfnWndProc = c->proc; wc->cbWndExtra = c->extra; wc->hInstance = c->inst;
    wc->hIcon = c->icon; wc->hCursor = c->cursor; wc->hbrBackground = c->brush; wc->lpszClassName = name;
    return TRUE;
}

USERAPI BOOL GetClassInfoExW(HINSTANCE inst, LPCWSTR name, WNDCLASSEXW *wc)
{
    char n[64];
    if ((ULONG_PTR)name >= 0x10000) w_to_u8(name, n, sizeof(n));
    WClass *c = find_class((ULONG_PTR)name < 0x10000 ? (LPCSTR)name : n);
    (void)inst;
    if (!c) { SetLastError(1411); return FALSE; }
    memset((BYTE *)wc + 4, 0, sizeof(*wc) - 4);
    wc->style = c->style; wc->lpfnWndProc = c->proc; wc->cbWndExtra = c->extra; wc->hInstance = c->inst;
    wc->hIcon = c->icon; wc->hCursor = c->cursor; wc->hbrBackground = c->brush; wc->lpszClassName = name;
    return TRUE;
}

USERAPI BOOL GetClassInfoW(HINSTANCE inst, LPCWSTR name, WNDCLASSW *wc)
{
    WNDCLASSEXW x;
    x.cbSize = sizeof(x);
    if (!GetClassInfoExW(inst, name, &x)) return FALSE;
    memcpy(wc, &x.style, sizeof(*wc));
    return TRUE;
}

USERAPI int GetClassNameA(HWND h, LPSTR buf, int n)
{
    WInfo *wi = win_of(h);
    if (!wi || n <= 0) return 0;
    int k = 0;
    for (; wi->cls->name[k] && k < n - 1; k++) buf[k] = wi->cls->name[k];
    buf[k] = 0;
    return k;
}

USERAPI int GetClassNameW(HWND h, LPWSTR buf, int n)
{
    WInfo *wi = win_of(h);
    if (!wi || n <= 0) return 0;
    WCHAR w[64];
    int k = u8_to_w(wi->cls->name, w, 64);
    if (k > n - 1) k = n - 1;
    memcpy(buf, w, 2 * (size_t)k);
    buf[k] = 0;
    return k;
}

/* -----------------------------------------------------------------------
 * Creating and destroying windows
 * ----------------------------------------------------------------------- */
static HWND g_focus, g_capture, g_active;

static HWND create_window(DWORD ex, WClass *wc, const char *title, DWORD style, int x, int y, int w, int h,
                          HMENU menu, HINSTANCE inst, LPVOID param, int wide, const void *wtitle, const void *wclass)
{
    if (!wc) { SetLastError(1407 /* ERROR_CANNOT_FIND_WND_CLASS */); return 0; }
    if (style & WS_CHILD) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }     /* no child windows */
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_win[i].used) { slot = i; break; }
    if (slot < 0) { SetLastError(1400 + 58 /* ERROR_NO_MORE_ITEMS-ish */); return 0; }

    WCHAR t16[256];
    u8_to_w(title, t16, 256);
    GuiCreate gc;
    memset(&gc, 0, sizeof(gc));
    gc.x = (x == CW_USEDEFAULT || x <= 0) ? (INT32)0x80000000 : x;
    gc.y = (y == CW_USEDEFAULT || y <= 0) ? (INT32)0x80000000 : y;
    gc.w = (w == CW_USEDEFAULT || w <= 0) ? 640 : w;
    gc.h = (h == CW_USEDEFAULT || h <= 0) ? 480 : h;
    gc.title = (UINT64)(ULONG_PTR)t16;
    if (!NtNovaGuiCreate(&gc) || !gc.hwnd) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }

    WInfo *wi = &g_win[slot];
    memset(wi, 0, sizeof(*wi));
    wi->used = 1;
    wi->id = (UINT32)gc.hwnd;
    wi->cls = wc;
    wi->proc = wc->proc;
    wi->wide = wide;
    wi->bg = wc->bg;
    wi->param = param;
    wi->style = style;
    wi->exstyle = ex;
    wi->id_menu = (LONG_PTR)menu;
    wi->enabled = !(style & 0x08000000 /* WS_DISABLED */);
    wi->tid = GetCurrentThreadId();
    wi->dc.bits = (DWORD *)(ULONG_PTR)gc.bitmap;
    wi->dc.stride = gc.stride / 4;
    wi->dc.w = gc.cw;
    wi->dc.h = gc.ch;
    wi->dc.text_color = 0x000000;
    wi->dc.bk_color = wc->bg;
    wi->dc.bk_mode = OPAQUE;
    wi->dc.has_pen = 1; wi->dc.pen_color = 0; wi->dc.pen_width = 1;
    wi->dc.has_brush = 0;
    wi->dc.hwnd = (void *)(ULONG_PTR)wi->id;
    if (title) { int n = 0; for (; title[n] && n < 255; n++) wi->text[n] = title[n]; wi->text[n] = 0; }

    HWND hwnd = (HWND)(ULONG_PTR)wi->id;
    if (!g_active) g_active = g_focus = hwnd;
    NovaGdiFill((HDC)&wi->dc, 0, 0, gc.cw, gc.ch, wc->bg);
    /* WM_NCCREATE, WM_CREATE (-1 from WM_CREATE fails the creation) */
    CREATESTRUCTA cs;
    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = param; cs.hInstance = inst; cs.hMenu = menu;
    cs.cx = gc.cw; cs.cy = gc.ch; cs.x = gc.x; cs.y = gc.y; cs.style = (LONG)style; cs.dwExStyle = ex;
    cs.lpszName = wide ? (LPCSTR)wtitle : title;
    cs.lpszClass = wide ? (LPCSTR)wclass : wc->name;
    if (wi->proc) {
        if (!wi->proc(hwnd, 0x0081 /* WM_NCCREATE */, 0, (LPARAM)&cs)) {
            wi = win_of(hwnd);
            if (wi) { NtNovaGuiDestroy(wi->id); wi->used = 0; }
            return 0;
        }
        wi = win_of(hwnd);
        if (!wi) return 0;
        if (wi->proc(hwnd, WM_CREATE, 0, (LPARAM)&cs) == -1) {
            DestroyWindow(hwnd);
            return 0;
        }
        wi = win_of(hwnd);
        if (wi && wi->proc) wi->proc(hwnd, WM_SIZE, 0, MAKELONG(gc.cw, gc.ch));
    }
    if (style & WS_VISIBLE) ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

USERAPI HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                             HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    (void)parent;
    return create_window(ex, find_class(cls), title, style, x, y, w, h, menu, inst, param, 0, 0, 0);
}

USERAPI HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                             HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    (void)parent;
    char c[64], t[256];
    WClass *wc;
    if ((ULONG_PTR)cls < 0x10000) wc = find_class((LPCSTR)cls);
    else { w_to_u8(cls, c, sizeof(c)); wc = find_class(c); }
    w_to_u8(title, t, sizeof(t));
    return create_window(ex, wc, t, style, x, y, w, h, menu, inst, param, wc ? wc->wide || 1 : 1, title, cls);
}

USERAPI BOOL DestroyWindow(HWND h)
{
    WInfo *wi = win_of(h);
    if (!wi) { SetLastError(1400 /* ERROR_INVALID_WINDOW_HANDLE */); return FALSE; }
    if (wi->proc) wi->proc(h, WM_DESTROY, 0, 0);
    wi = win_of(h);
    if (!wi) return TRUE;
    if (wi->proc) wi->proc(h, 0x0082 /* WM_NCDESTROY */, 0, 0);
    wi = win_of(h);
    if (!wi) return TRUE;
    NtNovaGuiDestroy(wi->id);
    wi->used = 0;
    if (g_focus == h) g_focus = 0;
    if (g_active == h) g_active = 0;
    if (g_capture == h) g_capture = 0;
    return TRUE;
}

USERAPI BOOL ShowWindow(HWND h, int cmd)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    BOOL was = wi->visible;
    wi->visible = cmd != SW_HIDE;
    NtNovaGuiShow(wi->id, cmd != SW_HIDE);
    if (wi->visible) wi->style |= WS_VISIBLE; else wi->style &= ~WS_VISIBLE;
    if (wi->visible && !was && wi->proc) wi->proc(h, 0x0018 /* WM_SHOWWINDOW */, TRUE, 0);
    return was;
}

USERAPI BOOL ShowWindowAsync(HWND h, int cmd) { return ShowWindow(h, cmd); }

USERAPI BOOL UpdateWindow(HWND h)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    if (wi->proc) wi->proc(h, WM_PAINT, 0, 0);
    NtNovaGuiInvalidate(wi->id);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * The message loop
 * ----------------------------------------------------------------------- */
#define LOCALQ 128
static MSG g_localq[LOCALQ];
static int g_lq_head, g_lq_count;
static SRWLOCK g_lq_lock;
static BYTE g_keys[256];                    /* key state: 0x80 down, 0x01 toggled */
static POINT g_cursor;
static DWORD g_msg_time;
static POINT g_msg_pt;

static BOOL local_push(const MSG *m, BOOL front)
{
    AcquireSRWLockExclusive(&g_lq_lock);
    BOOL ok = g_lq_count < LOCALQ;
    if (ok) {
        if (front) { g_lq_head = (g_lq_head + LOCALQ - 1) % LOCALQ; g_localq[g_lq_head] = *m; }
        else g_localq[(g_lq_head + g_lq_count) % LOCALQ] = *m;
        g_lq_count++;
    }
    ReleaseSRWLockExclusive(&g_lq_lock);
    return ok;
}

/* The first local message for @h (0: any) in [mn, mx] (both 0: any) */
static BOOL local_take(LPMSG out, HWND h, UINT mn, UINT mx, BOOL remove)
{
    BOOL found = FALSE;
    AcquireSRWLockExclusive(&g_lq_lock);
    for (int i = 0; i < g_lq_count; i++) {
        MSG *m = &g_localq[(g_lq_head + i) % LOCALQ];
        if (h && m->hwnd != h && m->hwnd) continue;
        if ((mn || mx) && (m->message < mn || m->message > mx)) continue;
        *out = *m;
        found = TRUE;
        if (remove) {
            for (int k = i; k + 1 < g_lq_count; k++) g_localq[(g_lq_head + k) % LOCALQ] = g_localq[(g_lq_head + k + 1) % LOCALQ];
            g_lq_count--;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_lq_lock);
    return found;
}

static void track(const MSG *m)
{
    g_msg_time = m->time ? m->time : GetTickCount();
    switch (m->message) {
    case WM_KEYDOWN: case 0x0104 /* WM_SYSKEYDOWN */: {
        BYTE vk = (BYTE)m->wParam;
        if (!(g_keys[vk] & 0x80)) g_keys[vk] ^= 1;          /* toggle on each press */
        g_keys[vk] |= 0x80;
        if (vk == 0x10 || vk == 0x11 || vk == 0x12) g_keys[vk == 0x10 ? 0xA0 : vk == 0x11 ? 0xA2 : 0xA4] |= 0x80;
        break;
    }
    case WM_KEYUP: case 0x0105 /* WM_SYSKEYUP */: {
        BYTE vk = (BYTE)m->wParam;
        g_keys[vk] &= ~0x80;
        if (vk == 0x10 || vk == 0x11 || vk == 0x12) g_keys[vk == 0x10 ? 0xA0 : vk == 0x11 ? 0xA2 : 0xA4] &= ~0x80;
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: g_keys[1] |= 0x80; break;
    case WM_LBUTTONUP: g_keys[1] &= ~0x80; break;
    case WM_RBUTTONDOWN: g_keys[2] |= 0x80; break;
    case 0x0205 /* WM_RBUTTONUP */: g_keys[2] &= ~0x80; break;
    }
    if ((m->message >= 0x0200 && m->message <= 0x020E) || m->message == WM_MOUSEMOVE) {
        g_cursor.x = (short)LOWORD(m->lParam);
        g_cursor.y = (short)HIWORD(m->lParam);
    }
    g_msg_pt = g_cursor;
    if (m->hwnd && (m->message == WM_LBUTTONDOWN || m->message == WM_KEYDOWN)) g_active = g_focus = m->hwnd;
}

USERAPI BOOL GetMessageA(LPMSG m, HWND h, UINT mn, UINT mx)
{
    if (g_quit && !g_lq_count) { memset(m, 0, sizeof(*m)); m->message = WM_QUIT; m->wParam = (WPARAM)g_quit_code; return 0; }
    for (;;) {
        if (local_take(m, h, mn, mx, TRUE)) break;
        if (g_quit) { memset(m, 0, sizeof(*m)); m->message = WM_QUIT; m->wParam = (WPARAM)g_quit_code; return 0; }
        long r = NtNovaGuiGetMessage((ULONG_PTR)(h ? (UINT32)(ULONG_PTR)h : 0), m, 1);
        if (r == 0) { m->message = WM_QUIT; return 0; }
        if (r < 0) return (BOOL)-1;
        if ((mn || mx) && (m->message < mn || m->message > mx)) { local_push(m, FALSE); Sleep(1); continue; }
        break;
    }
    track(m);
    return m->message != WM_QUIT;
}

USERAPI BOOL GetMessageW(LPMSG m, HWND h, UINT mn, UINT mx) { return GetMessageA(m, h, mn, mx); }

USERAPI BOOL PeekMessageA(LPMSG m, HWND h, UINT mn, UINT mx, UINT remove)
{
    BOOL rm = (remove & PM_REMOVE) != 0;
    if (local_take(m, h, mn, mx, rm)) { if (rm) track(m); return TRUE; }
    if (g_quit) {
        memset(m, 0, sizeof(*m));
        m->message = WM_QUIT; m->wParam = (WPARAM)g_quit_code;
        if (rm) g_quit = 2;                                 /* delivered once */
        return g_quit == 1 || rm;
    }
    MSG t;
    long r = NtNovaGuiGetMessage((ULONG_PTR)(h ? (UINT32)(ULONG_PTR)h : 0), &t, 0);
    if (r != 1) return FALSE;
    if ((mn || mx) && (t.message < mn || t.message > mx)) { local_push(&t, FALSE); return FALSE; }
    *m = t;
    if (!rm) local_push(&t, TRUE);                          /* keep it for the next Get/Peek */
    else track(m);
    return TRUE;
}

USERAPI BOOL PeekMessageW(LPMSG m, HWND h, UINT mn, UINT mx, UINT remove) { return PeekMessageA(m, h, mn, mx, remove); }

USERAPI BOOL WaitMessage(void)
{
    MSG m;
    if (g_lq_count || g_quit) return TRUE;
    long r = NtNovaGuiGetMessage(0, &m, 1);
    if (r == 1) local_push(&m, TRUE);
    else if (r == 0) g_quit = 1;
    return TRUE;
}

USERAPI DWORD GetQueueStatus(UINT flags)
{
    MSG m;
    BOOL any = PeekMessageA(&m, 0, 0, 0, PM_NOREMOVE);
    return any ? (flags & 0xFFFF) | ((flags & 0xFFFF) << 16) : 0;
}

USERAPI BOOL GetInputState(void) { return GetQueueStatus(0x04FF) != 0; }
USERAPI LONG GetMessageTime(void) { return (LONG)g_msg_time; }
USERAPI DWORD GetMessagePos(void) { return (DWORD)MAKELONG(g_msg_pt.x, g_msg_pt.y); }
USERAPI LPARAM GetMessageExtraInfo(void) { return 0; }

/* Wait for handles or input: poll both, 10 ms at a time */
USERAPI DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD wake, DWORD flags)
{
    ULONGLONG until = ms == INFINITE ? ~0ULL : GetTickCount64() + ms;
    for (;;) {
        DWORD r = n ? WaitForMultipleObjectsEx(n, hs, (flags & 1) != 0, 0, (flags & 2) != 0) : WAIT_TIMEOUT;
        if (r != WAIT_TIMEOUT) return r;
        MSG m;
        if (wake && PeekMessageA(&m, 0, 0, 0, PM_NOREMOVE)) return WAIT_OBJECT_0 + n;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return WAIT_TIMEOUT;
        if (n) {
            DWORD slice = until - now > 10 ? 10 : (DWORD)(until - now);
            r = WaitForMultipleObjectsEx(n, hs, (flags & 1) != 0, slice, (flags & 2) != 0);
            if (r != WAIT_TIMEOUT) return r;
        } else {
            Sleep(until - now > 10 ? 10 : (DWORD)(until - now));
        }
    }
}

USERAPI DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD wake)
{
    return MsgWaitForMultipleObjectsEx(n, hs, ms, wake, all ? 1 : 0);
}

USERAPI BOOL TranslateMessage(const MSG *m) { (void)m; return FALSE; }   /* the kernel already sends WM_CHAR */

/* SetTimer callbacks, keyed by (window, id) */
typedef struct { HWND h; UINT_PTR id; TIMERPROC fn; } TimerFn;
static TimerFn g_timer_fns[64];

static LRESULT dispatch(const MSG *m, BOOL wide)
{
    (void)wide;
    if (m->message == WM_TIMER && m->lParam) {
        ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, m->wParam, GetTickCount());
        return 0;
    }
    if (m->message == WM_TIMER) {
        for (int i = 0; i < 64; i++)
            if (g_timer_fns[i].fn && g_timer_fns[i].h == m->hwnd && g_timer_fns[i].id == m->wParam) {
                g_timer_fns[i].fn(m->hwnd, WM_TIMER, m->wParam, GetTickCount());
                return 0;
            }
    }
    WInfo *wi = win_of(m->hwnd);
    if (!wi || !wi->proc) return 0;
    return wi->proc(m->hwnd, m->message, m->wParam, m->lParam);
}

USERAPI LRESULT DispatchMessageA(const MSG *m) { return dispatch(m, FALSE); }
USERAPI LRESULT DispatchMessageW(const MSG *m) { return dispatch(m, TRUE); }

/* -----------------------------------------------------------------------
 * Default window procedure
 * ----------------------------------------------------------------------- */
static LRESULT def_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, BOOL wide)
{
    WInfo *wi = win_of(h);
    switch (msg) {
    case 0x0081: return TRUE;                               /* WM_NCCREATE */
    case WM_CLOSE:   DestroyWindow(h); return 0;
    case WM_DESTROY: return 0;
    case WM_ERASEBKGND:
        if (wi) NovaGdiFill((HDC)&wi->dc, 0, 0, wi->dc.w, wi->dc.h, wi->bg);
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    case 0x000C:                                            /* WM_SETTEXT */
        if (!wi) return FALSE;
        if (wide) w_to_u8((LPCWSTR)lp, wi->text, sizeof(wi->text));
        else { int n = 0; const char *s = (const char *)lp; for (; s && s[n] && n < 255; n++) wi->text[n] = s[n]; wi->text[n] = 0; }
        { WCHAR w[256]; u8_to_w(wi->text, w, 256); NtNovaGuiSetText(wi->id, w); }
        return TRUE;
    case 0x000D:                                            /* WM_GETTEXT */
        if (!wi || !wp) return 0;
        if (wide) {
            WCHAR w[256];
            int n = u8_to_w(wi->text, w, 256);
            if (n > (int)wp - 1) n = (int)wp - 1;
            memcpy((void *)lp, w, 2 * (size_t)n);
            ((WCHAR *)lp)[n] = 0;
            return n;
        } else {
            int n = 0;
            for (; wi->text[n] && n < (int)wp - 1; n++) ((char *)lp)[n] = wi->text[n];
            ((char *)lp)[n] = 0;
            return n;
        }
    case 0x000E:                                            /* WM_GETTEXTLENGTH */
        if (!wi) return 0;
        if (wide) { WCHAR w[256]; return u8_to_w(wi->text, w, 256); }
        return (LRESULT)strlen(wi->text);
    case 0x0112:                                            /* WM_SYSCOMMAND */
        if ((wp & 0xFFF0) == 0xF060 /* SC_CLOSE */) { SendMessageA(h, WM_CLOSE, 0, 0); return 0; }
        return 0;
    case 0x0020:                                            /* WM_SETCURSOR */
        return FALSE;
    case 0x0084: return 1;                                  /* WM_NCHITTEST: HTCLIENT */
    case 0x0024: return 0;                                  /* WM_GETMINMAXINFO */
    case 0x007F: return 0;                                  /* WM_GETICON */
    }
    return 0;
}

USERAPI LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return def_proc(h, msg, wp, lp, FALSE); }
USERAPI LRESULT DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return def_proc(h, msg, wp, lp, TRUE); }

USERAPI LRESULT CallWindowProcA(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp) { return fn ? fn(h, msg, wp, lp) : 0; }
USERAPI LRESULT CallWindowProcW(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp) { return fn ? fn(h, msg, wp, lp) : 0; }

/* -----------------------------------------------------------------------
 * Posting and sending
 * ----------------------------------------------------------------------- */
USERAPI VOID PostQuitMessage(int code) { g_quit = 1; g_quit_code = code; }

static BOOL post(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WInfo *wi = win_of(h);
    if (!h || !wi) {
        if (h && h != (HWND)(LONG_PTR)-1 && h != (HWND)0xFFFF) { SetLastError(1400); return FALSE; }
        MSG m;
        memset(&m, 0, sizeof(m));
        m.hwnd = 0; m.message = msg; m.wParam = wp; m.lParam = lp; m.time = GetTickCount();
        if (msg == WM_QUIT) { g_quit = 1; g_quit_code = (int)wp; return TRUE; }
        return local_push(&m, FALSE);
    }
    if (wi->tid != GetCurrentThreadId() || msg >= 0xC000 || msg == WM_QUIT) {
        NtNovaGuiPostMessage(wi->id, msg, wp, lp);
        return TRUE;
    }
    MSG m;
    memset(&m, 0, sizeof(m));
    m.hwnd = h; m.message = msg; m.wParam = wp; m.lParam = lp; m.time = GetTickCount();
    NtNovaGuiPostMessage(wi->id, msg, wp, lp);
    return TRUE;
}

USERAPI BOOL PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return post(h, msg, wp, lp); }
USERAPI BOOL PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return post(h, msg, wp, lp); }
USERAPI BOOL PostThreadMessageA(DWORD tid, UINT msg, WPARAM wp, LPARAM lp)
{
    if (tid != GetCurrentThreadId()) {
        /* another thread of this process: its windows share the process queue here */
        MSG m;
        memset(&m, 0, sizeof(m));
        m.message = msg; m.wParam = wp; m.lParam = lp; m.time = GetTickCount();
        return local_push(&m, FALSE);
    }
    return post(0, msg, wp, lp);
}
USERAPI BOOL PostThreadMessageW(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { return PostThreadMessageA(tid, msg, wp, lp); }

USERAPI LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WInfo *wi = win_of(h);
    if (!wi) return 0;
    if (!wi->proc) return def_proc(h, msg, wp, lp, FALSE);
    return wi->proc(h, msg, wp, lp);
}

USERAPI LRESULT SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WInfo *wi = win_of(h);
    if (!wi) return 0;
    if (!wi->proc) return def_proc(h, msg, wp, lp, TRUE);
    return wi->proc(h, msg, wp, lp);
}

USERAPI LRESULT SendMessageTimeoutW(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT ms, PDWORD_PTR r)
{
    (void)flags; (void)ms;
    if (h == (HWND)0xFFFF) { if (r) *r = 0; return TRUE; }  /* HWND_BROADCAST: nobody else listens */
    LRESULT v = SendMessageW(h, msg, wp, lp);
    if (r) *r = (DWORD_PTR)v;
    return win_of(h) != 0 || !h;
}
USERAPI LRESULT SendMessageTimeoutA(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT ms, PDWORD_PTR r) { return SendMessageTimeoutW(h, msg, wp, lp, flags, ms, r); }
USERAPI BOOL SendNotifyMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { if (h == (HWND)0xFFFF) return TRUE; SendMessageW(h, msg, wp, lp); return TRUE; }
USERAPI BOOL SendNotifyMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return SendNotifyMessageW(h, msg, wp, lp); }
USERAPI BOOL ReplyMessage(LRESULT r) { (void)r; return FALSE; }
USERAPI BOOL InSendMessage(void) { return FALSE; }

/* Messages by name: 0xC000 and up */
static char g_msg_names[64][64];
static UINT register_message(const char *name)
{
    for (int i = 0; i < 64; i++) {
        if (!g_msg_names[i][0]) {
            int k = 0;
            for (; name[k] && k < 63; k++) g_msg_names[i][k] = name[k];
            return 0xC000 + (UINT)i;
        }
        const char *a = g_msg_names[i], *b = name;
        while (*a && (*a | 32) == (*b | 32)) { a++; b++; }
        if (!*a && !*b) return 0xC000 + (UINT)i;
    }
    return 0;
}
USERAPI UINT RegisterWindowMessageA(LPCSTR name) { return name ? register_message(name) : 0; }
USERAPI UINT RegisterWindowMessageW(LPCWSTR name) { char a[64]; w_to_u8(name, a, 64); return register_message(a); }
USERAPI UINT RegisterClipboardFormatA(LPCSTR name) { return RegisterWindowMessageA(name); }
USERAPI UINT RegisterClipboardFormatW(LPCWSTR name) { return RegisterWindowMessageW(name); }

/* -----------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */
USERAPI HDC BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    WInfo *wi = win_of(h);
    if (!wi) return 0;
    if (ps) {
        memset(ps, 0, sizeof(*ps));
        ps->hdc = (HDC)&wi->dc;
        ps->rcPaint.right = wi->dc.w;
        ps->rcPaint.bottom = wi->dc.h;
    }
    return (HDC)&wi->dc;
}

USERAPI BOOL EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    (void)ps;
    WInfo *wi = win_of(h);
    if (wi) NtNovaGuiInvalidate(wi->id);                    /* present the freshly drawn bitmap */
    return TRUE;
}

/* GetDC(NULL): a DC on a small offscreen surface (for measuring text) */
static NOVA_DC g_screen_dc;
static DWORD g_screen_px[64 * 64];
static HDC screen_dc(void)
{
    if (!g_screen_dc.bits) {
        g_screen_dc.bits = g_screen_px; g_screen_dc.w = g_screen_dc.h = g_screen_dc.stride = 64;
        g_screen_dc.bk_color = 0xFFFFFF; g_screen_dc.bk_mode = OPAQUE; g_screen_dc.has_pen = 1; g_screen_dc.pen_width = 1;
    }
    return (HDC)&g_screen_dc;
}

USERAPI HDC GetDC(HWND h) { if (!h) return screen_dc(); WInfo *wi = win_of(h); return wi ? (HDC)&wi->dc : 0; }
USERAPI HDC GetWindowDC(HWND h) { return GetDC(h); }
USERAPI HDC GetDCEx(HWND h, HANDLE rgn, DWORD flags) { (void)rgn; (void)flags; return GetDC(h); }
USERAPI int ReleaseDC(HWND h, HDC dc) { WInfo *wi = win_of(h); if (wi) NtNovaGuiInvalidate(wi->id); (void)dc; return 1; }

USERAPI BOOL GetClientRect(HWND h, LPRECT r)
{
    WInfo *wi = win_of(h);
    if (!r) return FALSE;
    if (!wi) {
        if (h == GetDesktopWindow()) { r->left = r->top = 0; r->right = GetSystemMetrics(0); r->bottom = GetSystemMetrics(1); return TRUE; }
        SetLastError(1400);
        return FALSE;
    }
    r->left = 0; r->top = 0; r->right = wi->dc.w; r->bottom = wi->dc.h;
    return TRUE;
}
USERAPI BOOL GetWindowRect(HWND h, LPRECT r) { return GetClientRect(h, r); }

USERAPI BOOL InvalidateRect(HWND h, const RECT *r, BOOL erase)
{
    (void)r;
    WInfo *wi = win_of(h);
    if (!wi) return h == 0;
    if (erase) NovaGdiFill((HDC)&wi->dc, 0, 0, wi->dc.w, wi->dc.h, wi->bg);
    if (wi->proc) wi->proc(h, WM_PAINT, 0, 0);
    NtNovaGuiInvalidate(wi->id);
    return TRUE;
}

USERAPI BOOL InvalidateRgn(HWND h, HANDLE rgn, BOOL erase) { (void)rgn; return InvalidateRect(h, 0, erase); }
USERAPI BOOL ValidateRect(HWND h, const RECT *r) { (void)h; (void)r; return TRUE; }
USERAPI BOOL ValidateRgn(HWND h, HANDLE rgn) { (void)h; (void)rgn; return TRUE; }
USERAPI BOOL GetUpdateRect(HWND h, LPRECT r, BOOL erase) { (void)h; (void)erase; if (r) memset(r, 0, sizeof(*r)); return FALSE; }
USERAPI int GetUpdateRgn(HWND h, HANDLE rgn, BOOL erase) { (void)h; (void)rgn; (void)erase; return 1; /* NULLREGION */ }
USERAPI BOOL RedrawWindow(HWND h, const RECT *r, HANDLE rgn, UINT flags)
{
    (void)rgn;
    if (flags & 0x0001 /* RDW_INVALIDATE */) return InvalidateRect(h, r, (flags & 0x0004 /* RDW_ERASE */) != 0);
    return TRUE;
}
USERAPI BOOL ScrollWindow(HWND h, int dx, int dy, const RECT *r, const RECT *clip) { (void)dx; (void)dy; (void)r; (void)clip; return InvalidateRect(h, 0, TRUE); }
USERAPI int ScrollWindowEx(HWND h, int dx, int dy, const RECT *r, const RECT *clip, HANDLE rgn, LPRECT upd, UINT flags)
{
    (void)dx; (void)dy; (void)r; (void)clip; (void)rgn; (void)upd; (void)flags;
    InvalidateRect(h, 0, TRUE);
    return 2;                                               /* SIMPLEREGION */
}

/* -----------------------------------------------------------------------
 * Window text, data and state
 * ----------------------------------------------------------------------- */
USERAPI BOOL SetWindowTextA(HWND h, LPCSTR s)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    return (BOOL)SendMessageA(h, 0x000C, 0, (LPARAM)s);
}
USERAPI BOOL SetWindowTextW(HWND h, LPCWSTR s)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    return (BOOL)SendMessageW(h, 0x000C, 0, (LPARAM)s);
}
USERAPI int GetWindowTextA(HWND h, LPSTR s, int max)
{
    WInfo *wi = win_of(h);
    if (!wi || !s || max <= 0) return 0;
    int n = 0; for (; wi->text[n] && n < max - 1; n++) s[n] = wi->text[n]; s[n] = 0;
    return n;
}
USERAPI int GetWindowTextW(HWND h, LPWSTR s, int max)
{
    WInfo *wi = win_of(h);
    if (!wi || !s || max <= 0) return 0;
    WCHAR w[256];
    int n = u8_to_w(wi->text, w, 256);
    if (n > max - 1) n = max - 1;
    memcpy(s, w, 2 * (size_t)n);
    s[n] = 0;
    return n;
}
USERAPI int GetWindowTextLengthA(HWND h) { WInfo *wi = win_of(h); return wi ? (int)strlen(wi->text) : 0; }
USERAPI int GetWindowTextLengthW(HWND h) { WInfo *wi = win_of(h); WCHAR w[256]; return wi ? u8_to_w(wi->text, w, 256) : 0; }

/* GWL_*: WNDPROC -4, HINSTANCE -6, HWNDPARENT -8, ID -12, STYLE -16, EXSTYLE -20, USERDATA -21 */
static LONG_PTR get_long(HWND h, int i)
{
    WInfo *wi = win_of(h);
    if (!wi) { SetLastError(1400); return 0; }
    switch (i) {
    case -4:  return (LONG_PTR)wi->proc;
    case -6:  return (LONG_PTR)wi->cls->inst;
    case -8:  return 0;
    case -12: return wi->id_menu;
    case -16: return (LONG_PTR)wi->style;
    case -20: return (LONG_PTR)wi->exstyle;
    case -21: return wi->userdata;
    }
    if (i >= 0 && i + (int)sizeof(LONG_PTR) <= wi->cls->extra) { LONG_PTR v; memcpy(&v, wi->extra + i, sizeof(v)); return v; }
    if (i >= 0 && i + 4 <= wi->cls->extra) { LONG v; memcpy(&v, wi->extra + i, 4); return v; }
    SetLastError(1413 /* ERROR_INVALID_INDEX */);
    return 0;
}

static LONG_PTR set_long(HWND h, int i, LONG_PTR v, int size)
{
    WInfo *wi = win_of(h);
    if (!wi) { SetLastError(1400); return 0; }
    LONG_PTR old = get_long(h, i);
    switch (i) {
    case -4:  wi->proc = (WNDPROC)v; return old;
    case -12: wi->id_menu = v; return old;
    case -16: wi->style = (DWORD)v; return old;
    case -20: wi->exstyle = (DWORD)v; return old;
    case -21: wi->userdata = v; return old;
    case -6: case -8: return old;
    }
    if (i >= 0 && i + size <= wi->cls->extra) { memcpy(wi->extra + i, &v, (size_t)size); return old; }
    SetLastError(1413);
    return 0;
}

USERAPI LONG_PTR GetWindowLongPtrA(HWND h, int i) { return get_long(h, i); }
USERAPI LONG_PTR GetWindowLongPtrW(HWND h, int i) { return get_long(h, i); }
USERAPI LONG_PTR SetWindowLongPtrA(HWND h, int i, LONG_PTR v) { return set_long(h, i, v, sizeof(LONG_PTR)); }
USERAPI LONG_PTR SetWindowLongPtrW(HWND h, int i, LONG_PTR v) { return set_long(h, i, v, sizeof(LONG_PTR)); }
USERAPI LONG GetWindowLongA(HWND h, int i) { return (LONG)get_long(h, i); }
USERAPI LONG GetWindowLongW(HWND h, int i) { return (LONG)get_long(h, i); }
USERAPI LONG SetWindowLongA(HWND h, int i, LONG v) { return (LONG)set_long(h, i, v, 4); }
USERAPI LONG SetWindowLongW(HWND h, int i, LONG v) { return (LONG)set_long(h, i, v, 4); }
USERAPI ULONG_PTR GetClassLongPtrW(HWND h, int i)
{
    WInfo *wi = win_of(h);
    if (!wi) return 0;
    switch (i) {
    case -24: return (ULONG_PTR)wi->cls->proc;              /* GCLP_WNDPROC */
    case -10: return (ULONG_PTR)wi->cls->brush;             /* GCLP_HBRBACKGROUND */
    case -12: return (ULONG_PTR)wi->cls->cursor;            /* GCLP_HCURSOR */
    case -14: return (ULONG_PTR)wi->cls->icon;              /* GCLP_HICON */
    case -26: return wi->cls->style;                        /* GCL_STYLE */
    }
    return 0;
}
USERAPI ULONG_PTR GetClassLongPtrA(HWND h, int i) { return GetClassLongPtrW(h, i); }
USERAPI ULONG_PTR SetClassLongPtrW(HWND h, int i, LONG_PTR v)
{
    WInfo *wi = win_of(h);
    if (!wi) return 0;
    ULONG_PTR old = GetClassLongPtrW(h, i);
    if (i == -12) wi->cls->cursor = (HCURSOR)v;
    else if (i == -14) wi->cls->icon = (HICON)v;
    else if (i == -10) wi->cls->brush = (HBRUSH)v;
    return old;
}
USERAPI ULONG_PTR SetClassLongPtrA(HWND h, int i, LONG_PTR v) { return SetClassLongPtrW(h, i, v); }
USERAPI DWORD GetClassLongW(HWND h, int i) { return (DWORD)GetClassLongPtrW(h, i); }

USERAPI BOOL IsWindow(HWND h) { return win_of(h) != 0; }
USERAPI BOOL IsWindowVisible(HWND h) { WInfo *wi = win_of(h); return wi && wi->visible; }
USERAPI BOOL IsWindowEnabled(HWND h) { WInfo *wi = win_of(h); return wi && wi->enabled; }
USERAPI BOOL EnableWindow(HWND h, BOOL on) { WInfo *wi = win_of(h); if (!wi) return FALSE; BOOL was = !wi->enabled; wi->enabled = on; return was; }
USERAPI BOOL IsWindowUnicode(HWND h) { WInfo *wi = win_of(h); return wi && wi->wide; }
USERAPI BOOL IsIconic(HWND h) { (void)h; return FALSE; }
USERAPI BOOL IsZoomed(HWND h) { (void)h; return FALSE; }
USERAPI BOOL IsChild(HWND p, HWND c) { (void)p; (void)c; return FALSE; }
USERAPI HWND GetParent(HWND h) { (void)h; return 0; }
USERAPI HWND SetParent(HWND h, HWND p) { (void)h; (void)p; return 0; }
USERAPI HWND GetAncestor(HWND h, UINT flags) { (void)flags; return win_of(h) ? h : 0; }
USERAPI HWND GetWindow(HWND h, UINT cmd) { (void)h; (void)cmd; return 0; }
USERAPI HWND GetTopWindow(HWND h) { (void)h; return 0; }
USERAPI HWND GetDesktopWindow(void) { return (HWND)(ULONG_PTR)0x10010; }
USERAPI HWND GetShellWindow(void) { return 0; }
USERAPI HWND GetForegroundWindow(void) { return win_of(g_active) ? g_active : 0; }
USERAPI HWND GetActiveWindow(void) { return win_of(g_active) ? g_active : 0; }
USERAPI HWND GetFocus(void) { return win_of(g_focus) ? g_focus : 0; }
USERAPI BOOL SetForegroundWindow(HWND h) { if (!win_of(h)) return FALSE; g_active = g_focus = h; return TRUE; }
USERAPI HWND SetActiveWindow(HWND h) { HWND o = g_active; if (win_of(h)) g_active = h; return o; }
USERAPI HWND SetFocus(HWND h)
{
    HWND o = g_focus;
    if (h && !win_of(h)) return 0;
    g_focus = h;
    if (o && o != h && win_of(o)) SendMessageW(o, 0x0008 /* WM_KILLFOCUS */, (WPARAM)h, 0);
    if (h && o != h) SendMessageW(h, 0x0007 /* WM_SETFOCUS */, (WPARAM)o, 0);
    return o;
}
USERAPI BOOL BringWindowToTop(HWND h) { return SetForegroundWindow(h); }
USERAPI BOOL AllowSetForegroundWindow(DWORD pid) { (void)pid; return TRUE; }
USERAPI BOOL LockSetForegroundWindow(UINT code) { (void)code; return TRUE; }
USERAPI BOOL IsHungAppWindow(HWND h) { (void)h; return FALSE; }
USERAPI HWND SetCapture(HWND h) { HWND o = g_capture; g_capture = h; return o; }
USERAPI BOOL ReleaseCapture(void) { g_capture = 0; return TRUE; }
USERAPI HWND GetCapture(void) { return g_capture; }

USERAPI DWORD GetWindowThreadProcessId(HWND h, LPDWORD pid)
{
    WInfo *wi = win_of(h);
    if (!wi) { if (pid) *pid = 0; return 0; }
    if (pid) *pid = GetCurrentProcessId();
    return wi->tid;
}

USERAPI HWND FindWindowA(LPCSTR cls, LPCSTR title)
{
    WClass *c = cls ? find_class(cls) : 0;
    if (cls && !c) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WInfo *wi = &g_win[i];
        if (!wi->used || (c && wi->cls != c)) continue;
        if (title) {
            const char *a = wi->text, *b = title;
            while (*a && *a == *b) { a++; b++; }
            if (*a || *b) continue;
        }
        return (HWND)(ULONG_PTR)wi->id;
    }
    return 0;
}

USERAPI HWND FindWindowW(LPCWSTR cls, LPCWSTR title)
{
    char c[64], t[256];
    if (cls && (ULONG_PTR)cls >= 0x10000) w_to_u8(cls, c, sizeof(c));
    if (title) w_to_u8(title, t, sizeof(t));
    return FindWindowA(!cls ? 0 : (ULONG_PTR)cls < 0x10000 ? (LPCSTR)cls : c, title ? t : 0);
}

USERAPI HWND FindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR title)
{
    if (parent || after) return 0;
    return FindWindowW(cls, title);
}

USERAPI BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && !fn((HWND)(ULONG_PTR)g_win[i].id, lp)) break;
    return TRUE;
}
USERAPI BOOL EnumThreadWindows(DWORD tid, WNDENUMPROC fn, LPARAM lp)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].tid == tid && !fn((HWND)(ULONG_PTR)g_win[i].id, lp)) break;
    return TRUE;
}
USERAPI BOOL EnumChildWindows(HWND parent, WNDENUMPROC fn, LPARAM lp) { (void)parent; (void)fn; (void)lp; return FALSE; }

/* Windows can't be moved or resized from a program: the user does it */
USERAPI BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint) { (void)x; (void)y; (void)w; (void)hh; if (repaint) InvalidateRect(h, 0, FALSE); return win_of(h) != 0; }
USERAPI BOOL SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT flags)
{
    (void)after; (void)x; (void)y; (void)w; (void)hh;
    if (flags & 0x0040 /* SWP_SHOWWINDOW */) ShowWindow(h, SW_SHOW);
    if (flags & 0x0080 /* SWP_HIDEWINDOW */) ShowWindow(h, SW_HIDE);
    return win_of(h) != 0;
}
USERAPI BOOL GetWindowPlacement(HWND h, void *wp)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    DWORD *p = wp;                                          /* length, flags, showCmd, ptMin, ptMax, rcNormal */
    p[1] = 0; p[2] = wi->visible ? 1 : 0;
    p[3] = p[4] = p[5] = p[6] = 0;
    p[7] = 0; p[8] = 0; p[9] = (DWORD)wi->dc.w; p[10] = (DWORD)wi->dc.h;
    return TRUE;
}
USERAPI BOOL SetWindowPlacement(HWND h, const void *wp) { (void)wp; return win_of(h) != 0; }
USERAPI BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) { (void)r; (void)style; (void)menu; return TRUE; }   /* client == window */
USERAPI BOOL AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex) { (void)ex; return AdjustWindowRect(r, style, menu); }
USERAPI BOOL AdjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD ex, UINT dpi) { (void)dpi; return AdjustWindowRectEx(r, style, menu, ex); }
USERAPI BOOL ClientToScreen(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
USERAPI BOOL ScreenToClient(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
USERAPI int MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n) { (void)from; (void)to; (void)p; (void)n; return 0; }
USERAPI HWND WindowFromPoint(POINT p) { (void)p; return GetForegroundWindow(); }
USERAPI HWND ChildWindowFromPoint(HWND h, POINT p) { (void)p; return h; }
USERAPI HWND ChildWindowFromPointEx(HWND h, POINT p, UINT f) { (void)p; (void)f; return h; }
USERAPI BOOL FlashWindow(HWND h, BOOL invert) { (void)invert; return win_of(h) != 0; }
USERAPI BOOL FlashWindowEx(const void *fi) { (void)fi; return TRUE; }
USERAPI BOOL CloseWindow(HWND h) { return win_of(h) != 0; }
USERAPI BOOL OpenIcon(HWND h) { return win_of(h) != 0; }

/* -----------------------------------------------------------------------
 * Timers
 * ----------------------------------------------------------------------- */
USERAPI UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn)
{
    WInfo *wi = win_of(h);
    if (!wi) {
        if (h) return 0;
        /* a thread timer: no window to deliver to; the first window of the process carries it */
        for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used) { wi = &g_win[i]; break; }
        if (!wi) return 0;
        static UINT_PTR next = 0x7F00;
        id = ++next;
    }
    UINT_PTR r = (UINT_PTR)NtNovaGuiSetTimer(wi->id, id, ms);
    if (r && fn) {
        for (int i = 0; i < 64; i++)
            if (!g_timer_fns[i].fn || (g_timer_fns[i].h == h && g_timer_fns[i].id == id)) {
                g_timer_fns[i].h = h ? h : (HWND)(ULONG_PTR)wi->id; g_timer_fns[i].id = id; g_timer_fns[i].fn = fn;
                break;
            }
    }
    return h ? r : (r ? id : 0);
}

USERAPI BOOL KillTimer(HWND h, UINT_PTR id)
{
    WInfo *wi = win_of(h);
    for (int i = 0; i < 64; i++) if (g_timer_fns[i].fn && g_timer_fns[i].id == id && (!h || g_timer_fns[i].h == h)) g_timer_fns[i].fn = 0;
    if (!wi) return h == 0;
    NtNovaGuiKillTimer(wi->id, id);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Message boxes
 * ----------------------------------------------------------------------- */
USERAPI int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type)
{
    (void)h;
    static const WCHAR err[] = { 'E', 'r', 'r', 'o', 'r', 0 };
    return (int)NtNovaGuiMessageBox(text ? (void *)text : (void *)(const WCHAR[]){ 0 }, caption ? (void *)caption : (void *)err, type);
}

USERAPI int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    WCHAR t[1024], c[256];
    u8_to_w(text, t, 1024);
    u8_to_w(caption ? caption : "Error", c, 256);
    return MessageBoxW(h, t, c, type);
}

USERAPI int MessageBoxExW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang) { (void)lang; return MessageBoxW(h, text, caption, type); }
USERAPI int MessageBoxExA(HWND h, LPCSTR text, LPCSTR caption, UINT type, WORD lang) { (void)lang; return MessageBoxA(h, text, caption, type); }
USERAPI int MessageBoxIndirectW(const void *p)
{
    const BYTE *b = p;                                      /* cbSize, hwndOwner, hInstance, lpszText, lpszCaption, dwStyle */
    return MessageBoxW(*(HWND *)(b + 8), *(LPCWSTR *)(b + 24), *(LPCWSTR *)(b + 32), *(DWORD *)(b + 40));
}
USERAPI BOOL MessageBeep(UINT type) { (void)type; return TRUE; }

/* -----------------------------------------------------------------------
 * Rectangles
 * ----------------------------------------------------------------------- */
USERAPI BOOL SetRect(LPRECT r, int l, int t, int rr, int b) { if (!r) return FALSE; r->left = l; r->top = t; r->right = rr; r->bottom = b; return TRUE; }
USERAPI BOOL SetRectEmpty(LPRECT r) { return SetRect(r, 0, 0, 0, 0); }
USERAPI BOOL CopyRect(LPRECT d, const RECT *s) { if (!d || !s) return FALSE; *d = *s; return TRUE; }
USERAPI BOOL IsRectEmpty(const RECT *r) { return !r || r->right <= r->left || r->bottom <= r->top; }
USERAPI BOOL PtInRect(const RECT *r, POINT p) { return r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
USERAPI BOOL OffsetRect(LPRECT r, int dx, int dy) { if (!r) return FALSE; r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return TRUE; }
USERAPI BOOL InflateRect(LPRECT r, int dx, int dy) { if (!r) return FALSE; r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy; return TRUE; }
USERAPI BOOL EqualRect(const RECT *a, const RECT *b) { return a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }
USERAPI BOOL IntersectRect(LPRECT d, const RECT *a, const RECT *b)
{
    RECT r = { a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top,
               a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom };
    if (IsRectEmpty(&r)) { SetRectEmpty(d); return FALSE; }
    *d = r;
    return TRUE;
}
USERAPI BOOL UnionRect(LPRECT d, const RECT *a, const RECT *b)
{
    if (IsRectEmpty(a)) { if (IsRectEmpty(b)) { SetRectEmpty(d); return FALSE; } *d = *b; return TRUE; }
    if (IsRectEmpty(b)) { *d = *a; return TRUE; }
    RECT r = { a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top,
               a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom };
    *d = r;
    return TRUE;
}
USERAPI BOOL SubtractRect(LPRECT d, const RECT *a, const RECT *b)
{
    RECT i;
    *d = *a;
    if (!IntersectRect(&i, a, b)) return !IsRectEmpty(d);
    if (i.top == a->top && i.bottom == a->bottom) {
        if (i.left == a->left) d->left = i.right; else if (i.right == a->right) d->right = i.left;
    } else if (i.left == a->left && i.right == a->right) {
        if (i.top == a->top) d->top = i.bottom; else if (i.bottom == a->bottom) d->bottom = i.top;
    }
    return !IsRectEmpty(d);
}

/* -----------------------------------------------------------------------
 * Drawing helpers
 * ----------------------------------------------------------------------- */
static COLORREF brush_color(HBRUSH br)
{
    if ((ULONG_PTR)br > 0 && (ULONG_PTR)br <= 31) return GetSysColor((int)(ULONG_PTR)br - 1);   /* COLOR_* + 1 */
    return br ? ((COLORREF *)br)[1] : 0;
}

USERAPI int FillRect(HDC dc, const RECT *r, HBRUSH br)
{
    if (!dc || !r) return 0;
    NovaGdiFill(dc, r->left, r->top, r->right, r->bottom, brush_color(br));
    return 1;
}

USERAPI int FrameRect(HDC dc, const RECT *r, HBRUSH br)
{
    if (!dc || !r) return 0;
    COLORREF c = brush_color(br);
    NovaGdiFill(dc, r->left, r->top, r->right, r->top + 1, c);
    NovaGdiFill(dc, r->left, r->bottom - 1, r->right, r->bottom, c);
    NovaGdiFill(dc, r->left, r->top, r->left + 1, r->bottom, c);
    NovaGdiFill(dc, r->right - 1, r->top, r->right, r->bottom, c);
    return 1;
}

USERAPI BOOL InvertRect(HDC dc, const RECT *r)
{
    for (int y = r->top; y < r->bottom; y++)
        for (int x = r->left; x < r->right; x++) {
            COLORREF c = GetPixel(dc, x, y);
            if (c != 0xFFFFFFFF) SetPixel(dc, x, y, ~c & 0xFFFFFF);
        }
    return TRUE;
}

USERAPI BOOL DrawFocusRect(HDC dc, const RECT *r) { return FrameRect(dc, r, (HBRUSH)(ULONG_PTR)(16 + 1)); }
USERAPI BOOL DrawEdge(HDC dc, LPRECT r, UINT edge, UINT flags)
{
    (void)edge;
    if (flags & 0x0800 /* BF_MIDDLE */) FillRect(dc, r, (HBRUSH)(ULONG_PTR)(15 + 1));
    FrameRect(dc, r, (HBRUSH)(ULONG_PTR)(16 + 1));
    return TRUE;
}

/* Single-line layout on the built-in font: alignment, ellipsis-free clipping */
static int draw_text(HDC dc, const char *s, int len, LPRECT r, UINT fmt)
{
    int cw = NovaGdiCellW(), ch = NovaGdiCellH();
    int lines = 1;
    for (int i = 0; i < len; i++) if (s[i] == '\n' && !(fmt & DT_SINGLELINE)) lines++;
    if (fmt & 0x0400 /* DT_CALCRECT */) {
        int maxw = 0, cur = 0;
        for (int i = 0; i < len; i++) { if (s[i] == '\n' && !(fmt & DT_SINGLELINE)) { if (cur > maxw) maxw = cur; cur = 0; } else cur++; }
        if (cur > maxw) maxw = cur;
        r->right = r->left + maxw * cw;
        r->bottom = r->top + lines * ch;
        return lines * ch;
    }
    int y = r->top;
    if (fmt & (DT_VCENTER)) y = r->top + (r->bottom - r->top - lines * ch) / 2;
    else if (fmt & 0x8 /* DT_BOTTOM */) y = r->bottom - lines * ch;
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i < len && !(s[i] == '\n' && !(fmt & DT_SINGLELINE))) continue;
        int n = i - start;
        if (n && s[start + n - 1] == '\r') n--;
        int tw = n * cw, x = r->left;
        if (fmt & DT_CENTER) x = r->left + (r->right - r->left - tw) / 2;
        else if (fmt & DT_RIGHT) x = r->right - tw;
        for (int k = 0; k < n; k++) {
            if (!(fmt & 0x100 /* DT_NOCLIP */) && x + (k + 1) * cw > r->right) break;
            char c = s[start + k] == '\t' ? ' ' : s[start + k];
            if (c == '&' && !(fmt & 0x800 /* DT_NOPREFIX */) && k + 1 < n) { c = s[start + ++k]; }
            NovaGdiChar(dc, x + k * cw, y, c);
        }
        y += ch;
        start = i + 1;
    }
    return lines * ch;
}

USERAPI int DrawTextA(HDC dc, LPCSTR s, int len, LPRECT r, UINT fmt)
{
    if (!dc || !s || !r) return 0;
    if (len < 0) len = (int)strlen(s);
    return draw_text(dc, s, len, r, fmt);
}

USERAPI int DrawTextW(HDC dc, LPCWSTR s, int len, LPRECT r, UINT fmt)
{
    if (!dc || !s || !r) return 0;
    if (len < 0) len = wlen(s);
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, buf, sizeof(buf) - 1, 0, 0);
    if (n < 0) n = 0;
    /* one cell per character: collapse multi-byte sequences to '?' */
    int o = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) buf[o++] = (char)c;
        else if ((c & 0xC0) == 0xC0) buf[o++] = '?';
    }
    return draw_text(dc, buf, o, r, fmt);
}

USERAPI int DrawTextExW(HDC dc, LPWSTR s, int len, LPRECT r, UINT fmt, void *params) { (void)params; return DrawTextW(dc, s, len, r, fmt); }
USERAPI int DrawTextExA(HDC dc, LPSTR s, int len, LPRECT r, UINT fmt, void *params) { (void)params; return DrawTextA(dc, s, len, r, fmt); }
USERAPI LONG TabbedTextOutW(HDC dc, int x, int y, LPCWSTR s, int n, int nt, const INT *tabs, int org)
{
    (void)nt; (void)tabs; (void)org;
    TextOutW(dc, x, y, s, n);
    return MAKELONG(n * NovaGdiCellW(), NovaGdiCellH());
}
USERAPI BOOL GrayStringW(HDC dc, HBRUSH br, void *fn, LPARAM lp, int n, int x, int y, int w, int h)
{
    (void)br; (void)fn; (void)w; (void)h;
    return TextOutW(dc, x, y, (LPCWSTR)lp, n);
}

/* -----------------------------------------------------------------------
 * Icons, cursors, images (all the built-in arrow and a placeholder icon)
 * ----------------------------------------------------------------------- */
USERAPI HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name) { (void)inst; (void)name; return (HCURSOR)1; }
USERAPI HCURSOR LoadCursorW(HINSTANCE inst, LPCWSTR name) { (void)inst; (void)name; return (HCURSOR)1; }
USERAPI HCURSOR LoadCursorFromFileW(LPCWSTR f) { (void)f; return (HCURSOR)1; }
USERAPI HICON LoadIconA(HINSTANCE inst, LPCSTR name) { (void)inst; (void)name; return (HICON)1; }
USERAPI HICON LoadIconW(HINSTANCE inst, LPCWSTR name) { (void)inst; (void)name; return (HICON)1; }
USERAPI HANDLE LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags)
{
    (void)inst; (void)name; (void)cx; (void)cy; (void)flags;
    if (type == 0 /* IMAGE_BITMAP */) { SetLastError(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return 0; }
    return (HANDLE)1;                                       /* icons and cursors: the built-in ones */
}
USERAPI HANDLE LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags) { return LoadImageW(inst, (LPCWSTR)name, type, cx, cy, flags); }
USERAPI HBITMAP LoadBitmapW(HINSTANCE inst, LPCWSTR name) { (void)inst; (void)name; SetLastError(1814); return 0; }
USERAPI HBITMAP LoadBitmapA(HINSTANCE inst, LPCSTR name) { (void)inst; (void)name; SetLastError(1814); return 0; }
USERAPI HICON CreateIconIndirect(void *ii) { (void)ii; return (HICON)1; }
USERAPI HICON CreateIconFromResourceEx(BYTE *b, DWORD n, BOOL icon, DWORD ver, int cx, int cy, UINT f) { (void)b; (void)n; (void)icon; (void)ver; (void)cx; (void)cy; (void)f; return (HICON)1; }
USERAPI HICON CopyIcon(HICON h) { return h; }
USERAPI HANDLE CopyImage(HANDLE h, UINT type, int cx, int cy, UINT f) { (void)type; (void)cx; (void)cy; (void)f; return h; }
USERAPI BOOL DestroyIcon(HICON h) { (void)h; return TRUE; }
USERAPI BOOL DestroyCursor(HCURSOR h) { (void)h; return TRUE; }
USERAPI BOOL DrawIcon(HDC dc, int x, int y, HICON h) { (void)dc; (void)x; (void)y; (void)h; return TRUE; }
USERAPI BOOL DrawIconEx(HDC dc, int x, int y, HICON h, int cx, int cy, UINT step, HBRUSH br, UINT flags)
{ (void)dc; (void)x; (void)y; (void)h; (void)cx; (void)cy; (void)step; (void)br; (void)flags; return TRUE; }
USERAPI BOOL GetIconInfo(HICON h, void *ii) { (void)h; memset(ii, 0, 32); return FALSE; }
static HCURSOR g_cursor_handle = (HCURSOR)1;
static int g_cursor_count;
USERAPI HCURSOR SetCursor(HCURSOR c) { HCURSOR o = g_cursor_handle; g_cursor_handle = c; return o; }
USERAPI HCURSOR GetCursor(void) { return g_cursor_handle; }
USERAPI int ShowCursor(BOOL show) { return show ? ++g_cursor_count : --g_cursor_count; }
USERAPI BOOL GetCursorPos(LPPOINT p) { if (!p) return FALSE; *p = g_cursor; return TRUE; }
USERAPI BOOL SetCursorPos(int x, int y) { (void)x; (void)y; return FALSE; }   /* the pointer is the user's */
USERAPI BOOL ClipCursor(const RECT *r) { (void)r; return TRUE; }
USERAPI BOOL GetCursorInfo(void *ci) { DWORD *p = ci; p[1] = 1; *(HCURSOR *)(p + 2) = g_cursor_handle; ((POINT *)(p + 4))->x = g_cursor.x; ((POINT *)(p + 4))->y = g_cursor.y; return TRUE; }

/* Carets: drawn by the program's own text rendering here */
USERAPI BOOL CreateCaret(HWND h, HBITMAP b, int w, int hh) { (void)h; (void)b; (void)w; (void)hh; return TRUE; }
USERAPI BOOL DestroyCaret(void) { return TRUE; }
USERAPI BOOL ShowCaret(HWND h) { (void)h; return TRUE; }
USERAPI BOOL HideCaret(HWND h) { (void)h; return TRUE; }
USERAPI BOOL SetCaretPos(int x, int y) { (void)x; (void)y; return TRUE; }
USERAPI BOOL GetCaretPos(LPPOINT p) { p->x = p->y = 0; return TRUE; }
USERAPI UINT GetCaretBlinkTime(void) { return 530; }
USERAPI BOOL SetCaretBlinkTime(UINT ms) { (void)ms; return TRUE; }

/* -----------------------------------------------------------------------
 * Keyboard
 * ----------------------------------------------------------------------- */
USERAPI SHORT GetKeyState(int vk) { BYTE s = g_keys[vk & 0xFF]; return (SHORT)((s & 0x80 ? 0x8000 : 0) | (s & 1)); }
USERAPI SHORT GetAsyncKeyState(int vk) { return (SHORT)(g_keys[vk & 0xFF] & 0x80 ? 0x8000 : 0); }
USERAPI BOOL GetKeyboardState(PBYTE keys) { memcpy(keys, g_keys, 256); return TRUE; }
USERAPI BOOL SetKeyboardState(PBYTE keys) { memcpy(g_keys, keys, 256); return TRUE; }
USERAPI HANDLE GetKeyboardLayout(DWORD tid) { (void)tid; return (HANDLE)(ULONG_PTR)0x04090409; }
USERAPI int GetKeyboardLayoutList(int n, HANDLE *list) { if (n >= 1 && list) list[0] = (HANDLE)(ULONG_PTR)0x04090409; return 1; }
USERAPI HANDLE LoadKeyboardLayoutW(LPCWSTR id, UINT f) { (void)id; (void)f; return (HANDLE)(ULONG_PTR)0x04090409; }
USERAPI HANDLE ActivateKeyboardLayout(HANDLE h, UINT f) { (void)f; return h; }
USERAPI BOOL GetKeyboardLayoutNameW(LPWSTR name) { const char *s = "00000409"; for (int i = 0; i < 9; i++) name[i] = (WCHAR)s[i]; return TRUE; }
USERAPI int GetKeyboardType(int what) { return what == 0 ? 4 : what == 2 ? 12 : 0; }

/* US layout: VK <-> scan code, and the character a key makes */
static const BYTE g_vk_to_sc[256] = {
    [0x1B] = 0x01, ['1'] = 0x02, ['2'] = 0x03, ['3'] = 0x04, ['4'] = 0x05, ['5'] = 0x06, ['6'] = 0x07, ['7'] = 0x08,
    ['8'] = 0x09, ['9'] = 0x0A, ['0'] = 0x0B, [0xBD] = 0x0C, [0xBB] = 0x0D, [0x08] = 0x0E, [0x09] = 0x0F,
    ['Q'] = 0x10, ['W'] = 0x11, ['E'] = 0x12, ['R'] = 0x13, ['T'] = 0x14, ['Y'] = 0x15, ['U'] = 0x16, ['I'] = 0x17,
    ['O'] = 0x18, ['P'] = 0x19, [0xDB] = 0x1A, [0xDD] = 0x1B, [0x0D] = 0x1C, [0x11] = 0x1D, ['A'] = 0x1E, ['S'] = 0x1F,
    ['D'] = 0x20, ['F'] = 0x21, ['G'] = 0x22, ['H'] = 0x23, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, [0xBA] = 0x27,
    [0xDE] = 0x28, [0xC0] = 0x29, [0x10] = 0x2A, [0xDC] = 0x2B, ['Z'] = 0x2C, ['X'] = 0x2D, ['C'] = 0x2E, ['V'] = 0x2F,
    ['B'] = 0x30, ['N'] = 0x31, ['M'] = 0x32, [0xBC] = 0x33, [0xBE] = 0x34, [0xBF] = 0x35, [0x6A] = 0x37, [0x12] = 0x38,
    [0x20] = 0x39, [0x14] = 0x3A, [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E, [0x74] = 0x3F, [0x75] = 0x40,
    [0x76] = 0x41, [0x77] = 0x42, [0x78] = 0x43, [0x79] = 0x44, [0x90] = 0x45, [0x91] = 0x46, [0x24] = 0x47, [0x26] = 0x48,
    [0x21] = 0x49, [0x6D] = 0x4A, [0x25] = 0x4B, [0x0C] = 0x4C, [0x27] = 0x4D, [0x6B] = 0x4E, [0x23] = 0x4F, [0x28] = 0x50,
    [0x22] = 0x51, [0x2D] = 0x52, [0x2E] = 0x53, [0x7A] = 0x57, [0x7B] = 0x58, [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D,
    [0xA3] = 0x1D, [0xA4] = 0x38, [0xA5] = 0x38, [0x5B] = 0x5B, [0x5C] = 0x5C, [0x5D] = 0x5D,
};

static WCHAR vk_char(UINT vk, BOOL shift, BOOL caps)
{
    static const char plain[] = "0123456789", shifted[] = ")!@#$%^&*(";
    if (vk >= 'A' && vk <= 'Z') return (WCHAR)((shift ^ caps) ? vk : vk + 32);
    if (vk >= '0' && vk <= '9') return (WCHAR)(shift ? shifted[vk - '0'] : plain[vk - '0']);
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x09: return '\t';
    case 0x08: return '\b';
    case 0x1B: return 0x1B;
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6F: return '/';
    }
    if (vk >= 0x60 && vk <= 0x69) return (WCHAR)('0' + vk - 0x60);
    return 0;
}

USERAPI UINT MapVirtualKeyW(UINT code, UINT type)
{
    switch (type) {
    case 0: return g_vk_to_sc[code & 0xFF];                 /* MAPVK_VK_TO_VSC */
    case 1: case 3:                                         /* MAPVK_VSC_TO_VK(_EX) */
        for (UINT vk = 1; vk < 256; vk++) if (g_vk_to_sc[vk] == (code & 0xFF)) return vk;
        return 0;
    case 2: { WCHAR c = vk_char(code, FALSE, FALSE); return c >= 'a' && c <= 'z' ? (UINT)c - 32 : c; }   /* MAPVK_VK_TO_CHAR */
    }
    return 0;
}
USERAPI UINT MapVirtualKeyA(UINT code, UINT type) { return MapVirtualKeyW(code, type); }
USERAPI UINT MapVirtualKeyExW(UINT code, UINT type, HANDLE hkl) { (void)hkl; return MapVirtualKeyW(code, type); }

USERAPI int ToUnicodeEx(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags, HANDLE hkl)
{
    (void)sc; (void)flags; (void)hkl;
    if (n < 1) return 0;
    BOOL shift = keys && (keys[0x10] & 0x80), caps = keys && (keys[0x14] & 1), ctrl = keys && (keys[0x11] & 0x80);
    WCHAR c = vk_char(vk, shift, caps);
    if (ctrl && vk >= 'A' && vk <= 'Z') c = (WCHAR)(vk - 'A' + 1);
    if (!c) return 0;
    out[0] = c;
    if (n > 1) out[1] = 0;
    return 1;
}
USERAPI int ToUnicode(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags) { return ToUnicodeEx(vk, sc, keys, out, n, flags, 0); }
USERAPI int ToAscii(UINT vk, UINT sc, const BYTE *keys, LPWORD out, UINT flags)
{
    WCHAR w[2];
    int r = ToUnicodeEx(vk, sc, keys, w, 2, flags, 0);
    if (r) *out = w[0];
    return r;
}
USERAPI SHORT VkKeyScanW(WCHAR c)
{
    for (UINT vk = 1; vk < 256; vk++) {
        if (vk_char(vk, FALSE, FALSE) == c) return (SHORT)vk;
        if (vk_char(vk, TRUE, FALSE) == c) return (SHORT)(vk | 0x100);
    }
    return -1;
}
USERAPI SHORT VkKeyScanA(CHAR c) { return VkKeyScanW((WCHAR)(BYTE)c); }
USERAPI SHORT VkKeyScanExW(WCHAR c, HANDLE hkl) { (void)hkl; return VkKeyScanW(c); }
USERAPI int GetKeyNameTextW(LONG lp, LPWSTR buf, int n)
{
    UINT sc = (UINT)(lp >> 16) & 0xFF;
    UINT vk = MapVirtualKeyW(sc, 1);
    char name[16];
    WCHAR c = vk_char(vk, TRUE, FALSE);
    if (vk == 0x20) memcpy(name, "Space", 6);
    else if (vk == 0x0D) memcpy(name, "Enter", 6);
    else if (vk == 0x1B) memcpy(name, "Esc", 4);
    else if (vk == 0x10) memcpy(name, "Shift", 6);
    else if (vk == 0x11) memcpy(name, "Ctrl", 5);
    else if (vk == 0x12) memcpy(name, "Alt", 4);
    else if (vk >= 0x70 && vk <= 0x7B) { name[0] = 'F'; int f = (int)vk - 0x6F; if (f >= 10) { name[1] = '1'; name[2] = (char)('0' + f - 10); name[3] = 0; } else { name[1] = (char)('0' + f); name[2] = 0; } }
    else if (c > ' ' && c < 0x7F) { name[0] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c); name[1] = 0; }
    else return 0;
    return u8_to_w(name, buf, n);
}

USERAPI UINT SendInput(UINT n, void *inputs, int size) { (void)n; (void)inputs; (void)size; SetLastError(ERROR_ACCESS_DENIED); return 0; }
USERAPI void keybd_event(BYTE vk, BYTE sc, DWORD flags, ULONG_PTR extra) { (void)vk; (void)sc; (void)flags; (void)extra; }
USERAPI void mouse_event(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra) { (void)flags; (void)dx; (void)dy; (void)data; (void)extra; }
USERAPI BOOL BlockInput(BOOL block) { (void)block; return FALSE; }
USERAPI BOOL GetLastInputInfo(void *lii) { DWORD *p = lii; p[1] = g_msg_time ? g_msg_time : GetTickCount(); return TRUE; }
USERAPI BOOL RegisterHotKey(HWND h, int id, UINT mods, UINT vk) { (void)h; (void)id; (void)mods; (void)vk; SetLastError(1409 /* ERROR_HOTKEY_ALREADY_REGISTERED */); return FALSE; }
USERAPI BOOL UnregisterHotKey(HWND h, int id) { (void)h; (void)id; return TRUE; }
USERAPI BOOL TrackMouseEvent(void *tme) { (void)tme; return TRUE; }
USERAPI UINT GetDoubleClickTime(void) { return 500; }
USERAPI BOOL SetDoubleClickTime(UINT ms) { (void)ms; return TRUE; }
USERAPI BOOL SwapMouseButton(BOOL swap) { (void)swap; return FALSE; }

/* -----------------------------------------------------------------------
 * Characters (UTF-16 case mapping: ASCII, Latin-1, Greek, Cyrillic)
 * ----------------------------------------------------------------------- */
static WCHAR up(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) || (c >= 0x430 && c <= 0x44F)) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
static WCHAR low(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) || (c >= 0x410 && c <= 0x42F)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

USERAPI LPWSTR CharUpperW(LPWSTR s)
{
    if ((ULONG_PTR)s < 0x10000) return (LPWSTR)(ULONG_PTR)up((WCHAR)(ULONG_PTR)s);
    for (WCHAR *p = s; *p; p++) *p = up(*p);
    return s;
}
USERAPI LPWSTR CharLowerW(LPWSTR s)
{
    if ((ULONG_PTR)s < 0x10000) return (LPWSTR)(ULONG_PTR)low((WCHAR)(ULONG_PTR)s);
    for (WCHAR *p = s; *p; p++) *p = low(*p);
    return s;
}
USERAPI LPSTR CharUpperA(LPSTR s)
{
    if ((ULONG_PTR)s < 0x10000) { char c = (char)(ULONG_PTR)s; return (LPSTR)(ULONG_PTR)(BYTE)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}
USERAPI LPSTR CharLowerA(LPSTR s)
{
    if ((ULONG_PTR)s < 0x10000) { char c = (char)(ULONG_PTR)s; return (LPSTR)(ULONG_PTR)(BYTE)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    return s;
}
USERAPI DWORD CharUpperBuffW(LPWSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = up(s[i]); return n; }
USERAPI DWORD CharLowerBuffW(LPWSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = low(s[i]); return n; }
USERAPI DWORD CharUpperBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32; return n; }
USERAPI DWORD CharLowerBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32; return n; }
USERAPI LPWSTR CharNextW(LPCWSTR s) { return (LPWSTR)(*s ? s + 1 : s); }
USERAPI LPWSTR CharPrevW(LPCWSTR start, LPCWSTR s) { return (LPWSTR)(s > start ? s - 1 : start); }
USERAPI LPSTR CharNextA(LPCSTR s)
{
    if (!*s) return (LPSTR)s;
    s++;
    while ((*s & 0xC0) == 0x80) s++;                        /* the ANSI code page is UTF-8 */
    return (LPSTR)s;
}
USERAPI LPSTR CharPrevA(LPCSTR start, LPCSTR s)
{
    if (s <= start) return (LPSTR)start;
    s--;
    while (s > start && (*s & 0xC0) == 0x80) s--;
    return (LPSTR)s;
}
/* The Ex forms take a code page; every ANSI page is UTF-8 here */
USERAPI LPSTR CharPrevExA(WORD cp, LPCSTR start, LPCSTR s, DWORD flags) { (void)cp; (void)flags; return CharPrevA(start, s); }
USERAPI LPSTR CharNextExA(WORD cp, LPCSTR s, DWORD flags) { (void)cp; (void)flags; return CharNextA(s); }
USERAPI BOOL IsCharAlphaW(WCHAR c) { return up(c) != low(c) || (c >= 0x4E00 && c <= 0x9FFF); }
USERAPI BOOL IsCharAlphaNumericW(WCHAR c) { return IsCharAlphaW(c) || (c >= '0' && c <= '9'); }
USERAPI BOOL IsCharUpperW(WCHAR c) { return low(c) != c; }
USERAPI BOOL IsCharLowerW(WCHAR c) { return up(c) != c; }
USERAPI BOOL IsCharAlphaA(CHAR c) { return IsCharAlphaW((WCHAR)(BYTE)c); }
USERAPI BOOL IsCharAlphaNumericA(CHAR c) { return IsCharAlphaNumericW((WCHAR)(BYTE)c); }
USERAPI BOOL IsCharUpperA(CHAR c) { return c >= 'A' && c <= 'Z'; }
USERAPI BOOL IsCharLowerA(CHAR c) { return c >= 'a' && c <= 'z'; }
USERAPI BOOL CharToOemA(LPCSTR s, LPSTR d) { if (s != d) while ((*d++ = *s++)) ; return TRUE; }   /* OEM is UTF-8 too */
USERAPI BOOL OemToCharA(LPCSTR s, LPSTR d) { if (s != d) while ((*d++ = *s++)) ; return TRUE; }
USERAPI BOOL CharToOemBuffA(LPCSTR s, LPSTR d, DWORD n) { if (s != d) for (DWORD i = 0; i < n; i++) d[i] = s[i]; return TRUE; }
USERAPI BOOL OemToCharBuffA(LPCSTR s, LPSTR d, DWORD n) { if (s != d) for (DWORD i = 0; i < n; i++) d[i] = s[i]; return TRUE; }
USERAPI BOOL CharToOemW(LPCWSTR s, LPSTR d) { WideCharToMultiByte(CP_UTF8, 0, s, -1, d, 0x7FFFFFFF, 0, 0); return TRUE; }
USERAPI BOOL OemToCharW(LPCSTR s, LPWSTR d) { MultiByteToWideChar(CP_UTF8, 0, s, -1, d, 0x7FFFFFFF); return TRUE; }

/* wsprintf: the C runtime's formatting, capped at 1024 characters as on Windows */
__declspec(dllimport) int _vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
__declspec(dllimport) int _vsnwprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
USERAPI int wvsprintfA(LPSTR buf, LPCSTR fmt, va_list ap) { int r = _vsnprintf(buf, 1024, fmt, ap); if (r < 0 || r >= 1024) { buf[1023] = 0; r = 1023; } return r; }
USERAPI int wvsprintfW(LPWSTR buf, LPCWSTR fmt, va_list ap) { int r = _vsnwprintf((wchar_t *)buf, 1024, (const wchar_t *)fmt, ap); if (r < 0 || r >= 1024) { buf[1023] = 0; r = 1023; } return r; }
__declspec(dllexport) int __cdecl wsprintfA(LPSTR buf, LPCSTR fmt, ...) { va_list a; va_start(a, fmt); int r = wvsprintfA(buf, fmt, a); va_end(a); return r; }
__declspec(dllexport) int __cdecl wsprintfW(LPWSTR buf, LPCWSTR fmt, ...) { va_list a; va_start(a, fmt); int r = wvsprintfW(buf, fmt, a); va_end(a); return r; }

/* -----------------------------------------------------------------------
 * Resources: strings from the module's string table
 * ----------------------------------------------------------------------- */
USERAPI int LoadStringW(HINSTANCE inst, UINT id, LPWSTR buf, int n)
{
    HRSRC r = FindResourceW(inst, (LPCWSTR)(ULONG_PTR)((id >> 4) + 1), (LPCWSTR)(ULONG_PTR)6 /* RT_STRING */);
    if (!r) { if (buf && n) buf[0] = 0; return 0; }
    const WCHAR *p = LockResource(LoadResource(inst, r));
    if (!p) { if (buf && n) buf[0] = 0; return 0; }
    for (UINT i = 0; i < (id & 15); i++) p += 1 + *p;       /* length-prefixed entries */
    int len = *p;
    if (!n) { *(const WCHAR **)buf = p + 1; return len; }   /* a pointer to the resource itself */
    int k = len < n - 1 ? len : n - 1;
    memcpy(buf, p + 1, 2 * (size_t)k);
    buf[k] = 0;
    return k;
}

USERAPI int LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int n)
{
    WCHAR w[1024];
    int k = LoadStringW(inst, id, w, 1024);
    if (!n) return 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, w, k, buf, n - 1, 0, 0);
    if (m < 0) m = 0;
    buf[m] = 0;
    return m;
}

/* -----------------------------------------------------------------------
 * System colors, metrics and parameters
 * ----------------------------------------------------------------------- */
USERAPI DWORD GetSysColor(int i)
{
    static const COLORREF c[31] = {
        0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
        0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x544E43,
        0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xD77800, 0xF0F0F0 };
    return i >= 0 && i < 31 ? c[i] : 0;
}
USERAPI HBRUSH GetSysColorBrush(int i)
{
    static HBRUSH b[31];
    if (i < 0 || i >= 31) return 0;
    if (!b[i]) b[i] = CreateSolidBrush(GetSysColor(i));
    return b[i];
}
USERAPI BOOL SetSysColors(int n, const INT *idx, const COLORREF *c) { (void)n; (void)idx; (void)c; return FALSE; }

USERAPI int GetSystemMetrics(int index)
{
    ULONG w = 0, h = 0;
    NtNovaGuiScreenSize(&w, &h);
    switch (index) {
    case 0: case 16: case 78: case 61: return (int)w;       /* CXSCREEN, CXFULLSCREEN, CXVIRTUALSCREEN, CXMAXIMIZED */
    case 1: case 17: case 79: case 62: return (int)h;
    case 76: case 77: return 0;                             /* XVIRTUALSCREEN, YVIRTUALSCREEN */
    case 2: case 3: case 20: case 21: return 17;            /* scroll bars */
    case 4: return 23;                                      /* CYCAPTION */
    case 5: case 6: return 1;                               /* CXBORDER, CYBORDER */
    case 7: case 8: case 32: case 33: return 4;             /* frames */
    case 9: case 10: return 8;                              /* CYVTHUMB, CXHTHUMB */
    case 11: case 12: case 13: case 14: return 32;          /* icons, cursors */
    case 15: return 20;                                     /* CYMENU */
    case 19: return 1;                                      /* MOUSEPRESENT */
    case 23: return 0;                                      /* SWAPBUTTON */
    case 28: case 29: return 136;                           /* CXMIN, CYMIN */
    case 30: case 31: return 22;                            /* CXSIZE, CYSIZE */
    case 36: case 37: return 4;                             /* double-click rectangle */
    case 43: return 3;                                      /* CMOUSEBUTTONS */
    case 45: case 46: return 2;                             /* CXEDGE, CYEDGE */
    case 49: case 50: return 16;                            /* small icons */
    case 67: return 0;                                      /* CLEANBOOT */
    case 68: case 69: return 4;                             /* CXDRAG, CYDRAG */
    case 75: return 1;                                      /* MOUSEWHEELPRESENT */
    case 80: return 1;                                      /* CMONITORS */
    case 81: return 1;                                      /* SAMEDISPLAYFORMAT */
    case 0x1000: return 0;                                  /* REMOTESESSION */
    }
    return 0;
}
USERAPI int GetSystemMetricsForDpi(int index, UINT dpi) { (void)dpi; return GetSystemMetrics(index); }

typedef struct { UINT cbSize; int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight; LOGFONTW lfCaptionFont;
                 int iSmCaptionWidth, iSmCaptionHeight; LOGFONTW lfSmCaptionFont; int iMenuWidth, iMenuHeight;
                 LOGFONTW lfMenuFont, lfStatusFont, lfMessageFont; int iPaddedBorderWidth; } NONCLIENTMETRICSW_;

static void ui_font(LOGFONTW *lf)
{
    memset(lf, 0, sizeof(*lf));
    lf->lfHeight = -12;
    lf->lfWeight = 400;
    u8_to_w("Segoe UI", lf->lfFaceName, 32);
}

USERAPI BOOL SystemParametersInfoW(UINT action, UINT uparam, PVOID p, UINT winini)
{
    (void)uparam; (void)winini;
    switch (action) {
    case 0x0030: {                                          /* SPI_GETWORKAREA */
        RECT *r = p;
        r->left = r->top = 0; r->right = GetSystemMetrics(0); r->bottom = GetSystemMetrics(1);
        return TRUE;
    }
    case 0x0029: {                                          /* SPI_GETNONCLIENTMETRICS */
        NONCLIENTMETRICSW_ *m = p;
        UINT size = m->cbSize;
        memset(m, 0, size < sizeof(*m) ? size : sizeof(*m));
        m->cbSize = size;
        m->iBorderWidth = 1; m->iScrollWidth = m->iScrollHeight = 17; m->iCaptionWidth = m->iCaptionHeight = 22;
        m->iSmCaptionWidth = m->iSmCaptionHeight = 22; m->iMenuWidth = m->iMenuHeight = 19;
        ui_font(&m->lfCaptionFont); ui_font(&m->lfSmCaptionFont); ui_font(&m->lfMenuFont);
        ui_font(&m->lfStatusFont); ui_font(&m->lfMessageFont);
        return TRUE;
    }
    case 0x001F: ui_font(p); return TRUE;                   /* SPI_GETICONTITLELOGFONT */
    case 0x004A: case 0x000A: case 0x0044: case 0x0046: case 0x0048: case 0x001B: case 0x005F:
        *(BOOL *)p = FALSE; return TRUE;                    /* screen reader, beep, key prefs, ... */
    case 0x004A + 0x1000: *(BOOL *)p = TRUE; return TRUE;
    case 0x0068: *(UINT *)p = 3; return TRUE;               /* SPI_GETWHEELSCROLLLINES */
    case 0x006C: *(UINT *)p = 3; return TRUE;               /* SPI_GETWHEELSCROLLCHARS */
    case 0x0016: *(int *)p = 500; return TRUE;              /* SPI_GETKEYBOARDDELAY-ish */
    case 0x004B: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETFONTSMOOTHING */
    case 0x200C: *(UINT *)p = 2; return TRUE;               /* SPI_GETFONTSMOOTHINGTYPE: ClearType */
    case 0x1042: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETCLIENTAREAANIMATION */
    case 0x1012: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETGRADIENTCAPTIONS */
    case 0x0070: *(UINT *)p = 10; return TRUE;              /* SPI_GETMOUSESPEED */
    case 0x0008: *(UINT *)p = 1; return TRUE;               /* SPI_GETBORDER */
    case 0x2014: *(UINT *)p = 0; return TRUE;               /* SPI_GETCARETWIDTH-ish */
    case 0x1024: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETDROPSHADOW */
    case 0x1002: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETMENUANIMATION */
    }
    if (p && action >= 0x1000 && action < 0x2000 && !(action & 1)) { *(BOOL *)p = FALSE; return TRUE; }   /* other SPI_GET* booleans */
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

USERAPI BOOL SystemParametersInfoA(UINT action, UINT uparam, PVOID p, UINT winini)
{
    if (action == 0x0029) {                                 /* the A metrics hold LOGFONTA */
        BYTE *m = p;
        UINT size = *(UINT *)m;
        memset(m, 0, size);
        *(UINT *)m = size;
        return TRUE;
    }
    return SystemParametersInfoW(action, uparam, p, winini);
}
USERAPI BOOL SystemParametersInfoForDpi(UINT action, UINT uparam, PVOID p, UINT winini, UINT dpi) { (void)dpi; return SystemParametersInfoW(action, uparam, p, winini); }

/* DPI: everything is at 96 (the desktop scales logical pixels itself) */
USERAPI UINT GetDpiForWindow(HWND h) { (void)h; return 96; }
USERAPI UINT GetDpiForSystem(void) { return 96; }
USERAPI BOOL SetProcessDPIAware(void) { return TRUE; }
USERAPI BOOL IsProcessDPIAware(void) { return TRUE; }
USERAPI BOOL SetProcessDpiAwarenessContext(HANDLE ctx) { (void)ctx; return TRUE; }
USERAPI HANDLE SetThreadDpiAwarenessContext(HANDLE ctx) { (void)ctx; return (HANDLE)(LONG_PTR)-4; }
USERAPI HANDLE GetThreadDpiAwarenessContext(void) { return (HANDLE)(LONG_PTR)-4; }
USERAPI HANDLE GetWindowDpiAwarenessContext(HWND h) { (void)h; return (HANDLE)(LONG_PTR)-4; }
USERAPI int GetAwarenessFromDpiAwarenessContext(HANDLE ctx) { (void)ctx; return 2; }
USERAPI BOOL AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b) { return a == b; }
USERAPI BOOL IsValidDpiAwarenessContext(HANDLE ctx) { return ctx != 0; }
USERAPI BOOL EnableNonClientDpiScaling(HWND h) { (void)h; return TRUE; }

/* Monitors: one */
#define THE_MONITOR ((HANDLE)(ULONG_PTR)0x10001)
USERAPI HANDLE MonitorFromWindow(HWND h, DWORD f) { (void)h; (void)f; return THE_MONITOR; }
USERAPI HANDLE MonitorFromPoint(POINT p, DWORD f) { (void)p; (void)f; return THE_MONITOR; }
USERAPI HANDLE MonitorFromRect(const RECT *r, DWORD f) { (void)r; (void)f; return THE_MONITOR; }
USERAPI BOOL GetMonitorInfoW(HANDLE m, void *mi)
{
    if (m != THE_MONITOR) return FALSE;
    DWORD *p = mi;                                          /* cbSize, rcMonitor, rcWork, dwFlags, [szDevice] */
    RECT *mon = (RECT *)(p + 1), *work = (RECT *)(p + 5);
    SetRect(mon, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1));
    *work = *mon;
    p[9] = 1;                                               /* MONITORINFOF_PRIMARY */
    if (p[0] >= 104) u8_to_w("\\\\.\\DISPLAY1", (WCHAR *)(p + 10), 32);
    return TRUE;
}
USERAPI BOOL GetMonitorInfoA(HANDLE m, void *mi)
{
    DWORD *p = mi;
    DWORD size = p[0];
    p[0] = 40;
    BOOL ok = GetMonitorInfoW(m, mi);
    p[0] = size;
    if (ok && size >= 72) memcpy(p + 10, "\\\\.\\DISPLAY1", 13);
    return ok;
}
typedef BOOL (CALLBACK *MONITORENUMPROC)(HANDLE, HDC, LPRECT, LPARAM);
USERAPI BOOL EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC fn, LPARAM lp)
{
    (void)clip;
    RECT r;
    SetRect(&r, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1));
    fn(THE_MONITOR, dc, &r, lp);
    return TRUE;
}
USERAPI BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, void *dm)
{
    (void)dev;
    if (mode != (DWORD)-1 && mode != (DWORD)-2 && mode != 0) return FALSE;
    BYTE *b = dm;                                           /* DEVMODEW: dmSize at 68; fields after the name */
    WORD size = *(WORD *)(b + 68), extra = *(WORD *)(b + 70);
    memset(b, 0, (size_t)size + extra);
    *(WORD *)(b + 68) = size;
    u8_to_w("NovaOS Display", (WCHAR *)b, 32);
    *(DWORD *)(b + 72) = 0x00580000;                        /* dmFields: BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY */
    *(DWORD *)(b + 168) = 32;                               /* dmBitsPerPel */
    *(DWORD *)(b + 172) = (DWORD)GetSystemMetrics(0);       /* dmPelsWidth */
    *(DWORD *)(b + 176) = (DWORD)GetSystemMetrics(1);       /* dmPelsHeight */
    *(DWORD *)(b + 184) = 60;                               /* dmDisplayFrequency */
    return TRUE;
}
USERAPI BOOL EnumDisplayDevicesW(LPCWSTR dev, DWORD i, void *dd, DWORD flags)
{
    (void)dev; (void)flags;
    if (i) return FALSE;
    BYTE *b = dd;                                           /* cb, DeviceName[32], DeviceString[128], StateFlags, ... */
    DWORD cb = *(DWORD *)b;
    memset(b + 4, 0, cb - 4);
    u8_to_w("\\\\.\\DISPLAY1", (WCHAR *)(b + 4), 32);
    u8_to_w("NovaOS Display Adapter", (WCHAR *)(b + 68), 128);
    *(DWORD *)(b + 324) = 0x5;                              /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    return TRUE;
}
USERAPI LONG ChangeDisplaySettingsW(void *dm, DWORD f) { (void)dm; (void)f; return -2; /* DISP_CHANGE_BADMODE */ }
USERAPI LONG ChangeDisplaySettingsExW(LPCWSTR d, void *dm, HWND h, DWORD f, void *p) { (void)d; (void)dm; (void)h; (void)f; (void)p; return -2; }

/* -----------------------------------------------------------------------
 * Clipboard (this process's own)
 * ----------------------------------------------------------------------- */
typedef struct { UINT fmt; HANDLE data; } ClipItem;
static ClipItem g_clip[16];
static int g_clip_n, g_clip_open;
static DWORD g_clip_seq = 1;
static HWND g_clip_owner;

USERAPI BOOL OpenClipboard(HWND h) { if (g_clip_open) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } g_clip_open = 1; (void)h; return TRUE; }
USERAPI BOOL CloseClipboard(void) { g_clip_open = 0; return TRUE; }
USERAPI BOOL EmptyClipboard(void)
{
    for (int i = 0; i < g_clip_n; i++) if (g_clip[i].data) GlobalFree(g_clip[i].data);
    g_clip_n = 0;
    g_clip_seq++;
    return TRUE;
}
USERAPI HANDLE SetClipboardData(UINT fmt, HANDLE data)
{
    for (int i = 0; i < g_clip_n; i++) if (g_clip[i].fmt == fmt) { if (g_clip[i].data != data) GlobalFree(g_clip[i].data); g_clip[i].data = data; g_clip_seq++; return data; }
    if (g_clip_n >= 16) return 0;
    g_clip[g_clip_n].fmt = fmt;
    g_clip[g_clip_n++].data = data;
    g_clip_seq++;
    return data;
}
USERAPI HANDLE GetClipboardData(UINT fmt)
{
    for (int i = 0; i < g_clip_n; i++) if (g_clip[i].fmt == fmt) return g_clip[i].data;
    /* CF_TEXT (1) <-> CF_UNICODETEXT (13) */
    UINT other = fmt == 1 ? 13 : fmt == 13 ? 1 : 0;
    for (int i = 0; other && i < g_clip_n; i++) if (g_clip[i].fmt == other && g_clip[i].data) {
        const void *src = GlobalLock(g_clip[i].data);
        HANDLE h;
        if (fmt == 13) {
            int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, 0, 0);
            h = GlobalAlloc(0, 2 * (SIZE_T)n);
            if (h) MultiByteToWideChar(CP_UTF8, 0, src, -1, GlobalLock(h), n);
        } else {
            int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            h = GlobalAlloc(0, (SIZE_T)n);
            if (h) WideCharToMultiByte(CP_UTF8, 0, src, -1, GlobalLock(h), n, 0, 0);
        }
        if (h) SetClipboardData(fmt, h);
        return h;
    }
    return 0;
}
USERAPI BOOL IsClipboardFormatAvailable(UINT fmt)
{
    for (int i = 0; i < g_clip_n; i++) if (g_clip[i].fmt == fmt || (fmt == 1 && g_clip[i].fmt == 13) || (fmt == 13 && g_clip[i].fmt == 1)) return TRUE;
    return FALSE;
}
USERAPI int CountClipboardFormats(void) { return g_clip_n; }
USERAPI UINT EnumClipboardFormats(UINT fmt)
{
    if (!fmt) return g_clip_n ? g_clip[0].fmt : 0;
    for (int i = 0; i + 1 < g_clip_n; i++) if (g_clip[i].fmt == fmt) return g_clip[i + 1].fmt;
    return 0;
}
USERAPI HWND GetClipboardOwner(void) { return g_clip_owner; }
USERAPI HWND GetOpenClipboardWindow(void) { return 0; }
USERAPI DWORD GetClipboardSequenceNumber(void) { return g_clip_seq; }
USERAPI BOOL AddClipboardFormatListener(HWND h) { (void)h; return TRUE; }
USERAPI BOOL RemoveClipboardFormatListener(HWND h) { (void)h; return TRUE; }
USERAPI int GetClipboardFormatNameW(UINT fmt, LPWSTR buf, int n)
{
    if (fmt < 0xC000 || fmt >= 0xC000 + 64 || !g_msg_names[fmt - 0xC000][0]) return 0;
    return u8_to_w(g_msg_names[fmt - 0xC000], buf, n);
}

/* -----------------------------------------------------------------------
 * Dialogs, menus, hooks, accelerators, scroll bars: not in NovaOS's
 * window system.  Dialogs and dialog-item calls fail; menus are handles
 * with no bar or popup; hooks are accepted and never called.
 * ----------------------------------------------------------------------- */
USERAPI INT_PTR DialogBoxParamW(HINSTANCE i, LPCWSTR t, HWND p, DLGPROC fn, LPARAM lp) { (void)i; (void)t; (void)p; (void)fn; (void)lp; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return -1; }
USERAPI INT_PTR DialogBoxParamA(HINSTANCE i, LPCSTR t, HWND p, DLGPROC fn, LPARAM lp) { (void)i; (void)t; (void)p; (void)fn; (void)lp; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return -1; }
USERAPI INT_PTR DialogBoxIndirectParamW(HINSTANCE i, const void *t, HWND p, DLGPROC fn, LPARAM lp) { (void)i; (void)t; (void)p; (void)fn; (void)lp; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return -1; }
USERAPI HWND CreateDialogParamW(HINSTANCE i, LPCWSTR t, HWND p, DLGPROC fn, LPARAM lp) { (void)i; (void)t; (void)p; (void)fn; (void)lp; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return 0; }
USERAPI HWND CreateDialogParamA(HINSTANCE i, LPCSTR t, HWND p, DLGPROC fn, LPARAM lp) { (void)i; (void)t; (void)p; (void)fn; (void)lp; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return 0; }
USERAPI HWND CreateDialogIndirectParamW(HINSTANCE i, const void *t, HWND p, DLGPROC fn, LPARAM lp) { (void)i; (void)t; (void)p; (void)fn; (void)lp; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return 0; }
USERAPI BOOL EndDialog(HWND h, INT_PTR r) { (void)r; return DestroyWindow(h); }
USERAPI HWND GetDlgItem(HWND h, int id) { (void)h; (void)id; return 0; }
USERAPI int GetDlgCtrlID(HWND h) { WInfo *wi = win_of(h); return wi ? (int)wi->id_menu : 0; }
USERAPI BOOL SetDlgItemTextW(HWND h, int id, LPCWSTR s) { (void)h; (void)id; (void)s; return FALSE; }
USERAPI BOOL SetDlgItemTextA(HWND h, int id, LPCSTR s) { (void)h; (void)id; (void)s; return FALSE; }
USERAPI UINT GetDlgItemTextW(HWND h, int id, LPWSTR s, int n) { (void)h; (void)id; if (s && n) s[0] = 0; return 0; }
USERAPI UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sign) { (void)h; (void)id; (void)sign; if (ok) *ok = FALSE; return 0; }
USERAPI BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sign) { (void)h; (void)id; (void)v; (void)sign; return FALSE; }
USERAPI LRESULT SendDlgItemMessageW(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp) { (void)h; (void)id; (void)msg; (void)wp; (void)lp; return 0; }
USERAPI LRESULT SendDlgItemMessageA(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp) { (void)h; (void)id; (void)msg; (void)wp; (void)lp; return 0; }
USERAPI BOOL CheckDlgButton(HWND h, int id, UINT st) { (void)h; (void)id; (void)st; return FALSE; }
USERAPI UINT IsDlgButtonChecked(HWND h, int id) { (void)h; (void)id; return 0; }
USERAPI BOOL IsDialogMessageW(HWND h, LPMSG m) { (void)h; (void)m; return FALSE; }
USERAPI BOOL IsDialogMessageA(HWND h, LPMSG m) { (void)h; (void)m; return FALSE; }
USERAPI LRESULT DefDlgProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, msg, wp, lp); }
USERAPI LONG GetDialogBaseUnits(void) { return MAKELONG(8, 16); }
USERAPI BOOL MapDialogRect(HWND h, LPRECT r) { (void)h; r->left *= 2; r->right *= 2; r->top *= 2; r->bottom *= 2; return TRUE; }

typedef struct { DWORD magic; int n; } Menu;
static HMENU new_menu(void) { Menu *m = LocalAlloc(LMEM_ZEROINIT, sizeof(Menu)); if (m) m->magic = 0x4D454E55; return (HMENU)m; }
static Menu *menu_of(HMENU h) { Menu *m = (Menu *)h; return m && m->magic == 0x4D454E55 ? m : 0; }
USERAPI HMENU CreateMenu(void) { return new_menu(); }
USERAPI HMENU CreatePopupMenu(void) { return new_menu(); }
USERAPI BOOL DestroyMenu(HMENU h) { Menu *m = menu_of(h); if (!m) return FALSE; m->magic = 0; LocalFree(m); return TRUE; }
USERAPI BOOL AppendMenuW(HMENU h, UINT f, UINT_PTR id, LPCWSTR s) { (void)f; (void)id; (void)s; Menu *m = menu_of(h); if (!m) return FALSE; m->n++; return TRUE; }
USERAPI BOOL AppendMenuA(HMENU h, UINT f, UINT_PTR id, LPCSTR s) { (void)s; return AppendMenuW(h, f, id, 0); }
USERAPI BOOL InsertMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s) { (void)pos; return AppendMenuW(h, f, id, s); }
USERAPI BOOL InsertMenuItemW(HMENU h, UINT item, BOOL bypos, const void *mii) { (void)item; (void)bypos; (void)mii; return AppendMenuW(h, 0, 0, 0); }
USERAPI BOOL ModifyMenuW(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCWSTR s) { (void)pos; (void)f; (void)id; (void)s; return menu_of(h) != 0; }
USERAPI BOOL RemoveMenu(HMENU h, UINT pos, UINT f) { (void)pos; (void)f; Menu *m = menu_of(h); if (!m) return FALSE; if (m->n) m->n--; return TRUE; }
USERAPI BOOL DeleteMenu(HMENU h, UINT pos, UINT f) { return RemoveMenu(h, pos, f); }
USERAPI int GetMenuItemCount(HMENU h) { Menu *m = menu_of(h); return m ? m->n : -1; }
USERAPI BOOL SetMenuItemInfoW(HMENU h, UINT item, BOOL bypos, const void *mii) { (void)item; (void)bypos; (void)mii; return menu_of(h) != 0; }
USERAPI BOOL GetMenuItemInfoW(HMENU h, UINT item, BOOL bypos, void *mii) { (void)h; (void)item; (void)bypos; (void)mii; return FALSE; }
USERAPI DWORD CheckMenuItem(HMENU h, UINT id, UINT f) { (void)h; (void)id; (void)f; return 0; }
USERAPI BOOL EnableMenuItem(HMENU h, UINT id, UINT f) { (void)h; (void)id; (void)f; return 0; }
USERAPI BOOL CheckMenuRadioItem(HMENU h, UINT a, UINT b, UINT c, UINT f) { (void)h; (void)a; (void)b; (void)c; (void)f; return TRUE; }
USERAPI HMENU GetSubMenu(HMENU h, int pos) { (void)h; (void)pos; return 0; }
USERAPI HMENU GetMenu(HWND h) { (void)h; return 0; }
USERAPI BOOL SetMenu(HWND h, HMENU m) { (void)m; return win_of(h) != 0; }
USERAPI HMENU GetSystemMenu(HWND h, BOOL revert) { (void)h; (void)revert; return 0; }
USERAPI BOOL DrawMenuBar(HWND h) { return win_of(h) != 0; }
USERAPI HMENU LoadMenuW(HINSTANCE i, LPCWSTR name) { (void)i; (void)name; return new_menu(); }
USERAPI HMENU LoadMenuA(HINSTANCE i, LPCSTR name) { (void)i; (void)name; return new_menu(); }
USERAPI BOOL TrackPopupMenu(HMENU h, UINT f, int x, int y, int r, HWND w, const RECT *rc) { (void)h; (void)f; (void)x; (void)y; (void)r; (void)w; (void)rc; return FALSE; }
USERAPI BOOL TrackPopupMenuEx(HMENU h, UINT f, int x, int y, HWND w, void *p) { (void)h; (void)f; (void)x; (void)y; (void)w; (void)p; return FALSE; }
USERAPI UINT GetMenuState(HMENU h, UINT id, UINT f) { (void)h; (void)id; (void)f; return (UINT)-1; }
USERAPI BOOL SetMenuInfo(HMENU h, const void *mi) { (void)mi; return menu_of(h) != 0; }

USERAPI HANDLE SetWindowsHookExW(int id, void *fn, HINSTANCE mod, DWORD tid) { (void)id; (void)fn; (void)mod; (void)tid; return (HANDLE)(ULONG_PTR)0x48004; }
USERAPI HANDLE SetWindowsHookExA(int id, void *fn, HINSTANCE mod, DWORD tid) { return SetWindowsHookExW(id, fn, mod, tid); }
USERAPI BOOL UnhookWindowsHookEx(HANDLE h) { (void)h; return TRUE; }
USERAPI LRESULT CallNextHookEx(HANDLE h, int code, WPARAM wp, LPARAM lp) { (void)h; (void)code; (void)wp; (void)lp; return 0; }
USERAPI HANDLE SetWinEventHook(DWORD a, DWORD b, HMODULE m, void *fn, DWORD pid, DWORD tid, DWORD f) { (void)a; (void)b; (void)m; (void)fn; (void)pid; (void)tid; (void)f; return (HANDLE)(ULONG_PTR)0x48005; }
USERAPI BOOL UnhookWinEvent(HANDLE h) { (void)h; return TRUE; }
USERAPI void NotifyWinEvent(DWORD ev, HWND h, LONG obj, LONG child) { (void)ev; (void)h; (void)obj; (void)child; }

USERAPI HANDLE LoadAcceleratorsW(HINSTANCE i, LPCWSTR name) { (void)i; (void)name; return (HANDLE)(ULONG_PTR)0xACC1; }
USERAPI HANDLE LoadAcceleratorsA(HINSTANCE i, LPCSTR name) { (void)i; (void)name; return (HANDLE)(ULONG_PTR)0xACC1; }
USERAPI HANDLE CreateAcceleratorTableW(void *a, int n) { (void)a; (void)n; return (HANDLE)(ULONG_PTR)0xACC1; }
USERAPI BOOL DestroyAcceleratorTable(HANDLE h) { (void)h; return TRUE; }
USERAPI int TranslateAcceleratorW(HWND h, HANDLE a, LPMSG m) { (void)h; (void)a; (void)m; return 0; }
USERAPI int TranslateAcceleratorA(HWND h, HANDLE a, LPMSG m) { (void)h; (void)a; (void)m; return 0; }

USERAPI int SetScrollInfo(HWND h, int bar, const void *si, BOOL redraw) { (void)h; (void)bar; (void)si; (void)redraw; return 0; }
USERAPI BOOL GetScrollInfo(HWND h, int bar, void *si) { (void)h; (void)bar; (void)si; return FALSE; }
USERAPI int SetScrollPos(HWND h, int bar, int pos, BOOL redraw) { (void)h; (void)bar; (void)pos; (void)redraw; return 0; }
USERAPI int GetScrollPos(HWND h, int bar) { (void)h; (void)bar; return 0; }
USERAPI BOOL SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw) { (void)h; (void)bar; (void)mn; (void)mx; (void)redraw; return TRUE; }
USERAPI BOOL ShowScrollBar(HWND h, int bar, BOOL show) { (void)h; (void)bar; (void)show; return TRUE; }
USERAPI BOOL EnableScrollBar(HWND h, UINT bar, UINT arrows) { (void)h; (void)bar; (void)arrows; return TRUE; }

/* -----------------------------------------------------------------------
 * Window stations, desktops, session
 * ----------------------------------------------------------------------- */
#define WINSTA ((HANDLE)(ULONG_PTR)0x57530001)
#define DESKTOP ((HANDLE)(ULONG_PTR)0x44540001)
USERAPI HANDLE GetProcessWindowStation(void) { return WINSTA; }
USERAPI HANDLE GetThreadDesktop(DWORD tid) { (void)tid; return DESKTOP; }
USERAPI HANDLE OpenInputDesktop(DWORD f, BOOL inherit, DWORD access) { (void)f; (void)inherit; (void)access; return DESKTOP; }
USERAPI HANDLE OpenDesktopW(LPCWSTR name, DWORD f, BOOL inherit, DWORD access) { (void)name; (void)f; (void)inherit; (void)access; return DESKTOP; }
USERAPI BOOL CloseDesktop(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SwitchDesktop(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SetThreadDesktop(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL CloseWindowStation(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL GetUserObjectInformationW(HANDLE h, int index, PVOID p, DWORD n, LPDWORD need)
{
    if (index == 1) {                                       /* UOI_FLAGS: fInherit, fReserved, dwFlags */
        if (need) *need = 12;
        if (n < 12) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        DWORD *f = p;
        f[0] = 0; f[1] = 0; f[2] = 1;                       /* WSF_VISIBLE */
        return TRUE;
    }
    if (index == 2) {                                       /* UOI_NAME */
        const char *name = h == WINSTA ? "WinSta0" : "Default";
        WCHAR w[16];
        int k = u8_to_w(name, w, 16);
        DWORD bytes = 2 * ((DWORD)k + 1);
        if (need) *need = bytes;
        if (n < bytes) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy(p, w, bytes);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}
USERAPI BOOL ExitWindowsEx(UINT flags, DWORD reason) { (void)flags; (void)reason; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
USERAPI BOOL LockWorkStation(void) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
USERAPI BOOL GetGUIThreadInfo(DWORD tid, void *info)
{
    (void)tid;
    DWORD *p = info;
    DWORD cb = p[0];
    memset(p + 1, 0, cb - 4);
    *(HWND *)(p + 2) = GetActiveWindow();
    *(HWND *)(p + 4) = GetFocus();
    return TRUE;
}
USERAPI BOOL AttachThreadInput(DWORD a, DWORD b, BOOL attach) { (void)a; (void)b; (void)attach; return TRUE; }
USERAPI DWORD WaitForInputIdle(HANDLE p, DWORD ms) { (void)p; (void)ms; return 0; }
USERAPI BOOL SetPropW(HWND h, LPCWSTR name, HANDLE data) { (void)h; (void)name; (void)data; return FALSE; }
USERAPI HANDLE GetPropW(HWND h, LPCWSTR name) { (void)h; (void)name; return 0; }
USERAPI HANDLE RemovePropW(HWND h, LPCWSTR name) { (void)h; (void)name; return 0; }
USERAPI BOOL SetLayeredWindowAttributes(HWND h, COLORREF key, BYTE alpha, DWORD f) { (void)key; (void)alpha; (void)f; return win_of(h) != 0; }
USERAPI BOOL UpdateLayeredWindow(HWND h, HDC d, POINT *p, SIZE *s, HDC src, POINT *sp, COLORREF k, void *bf, DWORD f)
{ (void)d; (void)p; (void)s; (void)src; (void)sp; (void)k; (void)bf; (void)f; return win_of(h) != 0; }
USERAPI BOOL SetWindowDisplayAffinity(HWND h, DWORD a) { (void)a; return win_of(h) != 0; }
USERAPI BOOL ChangeWindowMessageFilterEx(HWND h, UINT msg, DWORD action, void *cf) { (void)h; (void)msg; (void)action; (void)cf; return TRUE; }
USERAPI BOOL ChangeWindowMessageFilter(UINT msg, DWORD f) { (void)msg; (void)f; return TRUE; }
USERAPI BOOL RegisterTouchWindow(HWND h, ULONG f) { (void)h; (void)f; return FALSE; }
USERAPI HANDLE RegisterDeviceNotificationW(HANDLE r, LPVOID filter, DWORD f) { (void)r; (void)filter; (void)f; return (HANDLE)(ULONG_PTR)0xDE01; }
USERAPI BOOL UnregisterDeviceNotification(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL RegisterRawInputDevices(const void *d, UINT n, UINT cb) { (void)d; (void)n; (void)cb; return TRUE; }
USERAPI HANDLE RegisterPowerSettingNotification(HANDLE r, const GUID *g, DWORD f) { (void)r; (void)g; (void)f; return (HANDLE)(ULONG_PTR)0xDE02; }
USERAPI BOOL UnregisterPowerSettingNotification(HANDLE h) { (void)h; return TRUE; }
