/*
 * user32.dll — windows, message loop and simple drawing helpers
 *
 * A window is created in the desktop's window manager through the kernel's
 * GUI syscalls; user32 keeps the class table and per-window state (the
 * window procedure and the device context that references the window's
 * client bitmap).  The message loop pulls Win32 messages the kernel
 * queued from real desktop input.
 */
#define NOVA_BUILD_USER32
#include <windows.h>
#include <winternl.h>

void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
WINBASEAPI LPVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
WINBASEAPI HANDLE WINAPI GetProcessHeap(void);
/* from gdi32 */
__declspec(dllimport) void NovaGdiFill(HDC, int, int, int, int, COLORREF);
__declspec(dllimport) int  NovaGdiCellW(void);
__declspec(dllimport) int  NovaGdiCellH(void);
__declspec(dllimport) void NovaGdiChar(HDC, int, int, char);

/* Kernel GuiCreate struct (matches um_gui.c) */
typedef struct {
    INT32 x, y, w, h; UINT32 style; UINT64 title;
    UINT64 hwnd, bitmap; UINT32 stride, cw, ch;
} GuiCreate;

#define MAX_CLASSES 32
#define MAX_WINDOWS 32

typedef struct { char name[64]; WNDPROC proc; COLORREF bg; int used; } WClass;
typedef struct {
    int      used;
    UINT32   id;
    WNDPROC  proc;
    NOVA_DC  dc;
    COLORREF bg;
    char     text[128];
    void    *param;
} WInfo;

static WClass g_class[MAX_CLASSES];
static WInfo  g_win[MAX_WINDOWS];
static int    g_quit, g_quit_code;

static int to_utf16(const char *s, unsigned short *w, int cap)
{
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) w[i] = (unsigned char)s[i];
    w[i] = 0;
    return i;
}

static WInfo *win_of(HWND h)
{
    UINT32 id = (UINT32)(ULONG_PTR)h;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].id == id) return &g_win[i];
    return NULL;
}

ATOM RegisterClassA(const WNDCLASSA *wc)
{
    for (int i = 0; i < MAX_CLASSES; i++) if (!g_class[i].used) {
        g_class[i].used = 1;
        g_class[i].proc = wc->lpfnWndProc;
        g_class[i].bg = 0xF3F3F3;
        int n = 0;
        if (wc->lpszClassName) for (; wc->lpszClassName[n] && n < 63; n++) g_class[i].name[n] = wc->lpszClassName[n];
        g_class[i].name[n] = 0;
        return (ATOM)(i + 1);
    }
    return 0;
}
ATOM RegisterClassExA(const WNDCLASSEXA *wc)
{
    WNDCLASSA c;
    memset(&c, 0, sizeof(c));
    c.lpfnWndProc = wc->lpfnWndProc;
    c.lpszClassName = wc->lpszClassName;
    return RegisterClassA(&c);
}

static WClass *find_class(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (!g_class[i].used) continue;
        const char *a = g_class[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return &g_class[i];
    }
    return NULL;
}

HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
                     int x, int y, int w, int h, HWND parent, HMENU menu,
                     HINSTANCE inst, LPVOID param)
{
    (void)ex; (void)parent; (void)menu; (void)inst;
    WClass *wc = find_class(cls);
    if (!wc) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_win[i].used) { slot = i; break; }
    if (slot < 0) return 0;

    unsigned short t16[128];
    to_utf16(title, t16, 128);
    GuiCreate gc;
    memset(&gc, 0, sizeof(gc));
    gc.x = (x == CW_USEDEFAULT) ? 0 : x;
    gc.y = (y == CW_USEDEFAULT) ? 0 : y;
    gc.w = (w == CW_USEDEFAULT || w <= 0) ? 640 : w;
    gc.h = (h == CW_USEDEFAULT || h <= 0) ? 480 : h;
    gc.title = (UINT64)(ULONG_PTR)t16;
    if (!NtNovaGuiCreate(&gc) || !gc.hwnd) return 0;

    WInfo *wi = &g_win[slot];
    memset(wi, 0, sizeof(*wi));
    wi->used = 1;
    wi->id = (UINT32)gc.hwnd;
    wi->proc = wc->proc;
    wi->bg = wc->bg;
    wi->param = param;
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
    if (title) { int n = 0; for (; title[n] && n < 127; n++) wi->text[n] = title[n]; wi->text[n] = 0; }

    HWND hwnd = (HWND)(ULONG_PTR)wi->id;
    /* Paint the background once, then WM_CREATE */
    NovaGdiFill((HDC)&wi->dc, 0, 0, gc.cw, gc.ch, wc->bg);
    if (wi->proc) {
        CREATESTRUCTA cs; memset(&cs, 0, sizeof(cs)); cs.lpCreateParams = param;
        wi->proc(hwnd, WM_CREATE, 0, (LPARAM)&cs);
    }
    return hwnd;
}

BOOL DestroyWindow(HWND h)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    if (wi->proc) wi->proc(h, WM_DESTROY, 0, 0);
    NtNovaGuiDestroy(wi->id);
    wi->used = 0;
    return TRUE;
}

BOOL ShowWindow(HWND h, int cmd) { WInfo *wi = win_of(h); if (!wi) return FALSE; NtNovaGuiShow(wi->id, cmd != SW_HIDE); return TRUE; }
BOOL UpdateWindow(HWND h) { WInfo *wi = win_of(h); if (!wi) return FALSE; if (wi->proc) wi->proc(h, WM_PAINT, 0, 0); NtNovaGuiInvalidate(wi->id); return TRUE; }

BOOL GetMessageA(LPMSG m, HWND h, UINT mn, UINT mx)
{
    (void)mn; (void)mx;
    if (g_quit) return 0;
    long r = NtNovaGuiGetMessage((ULONG_PTR)(h ? (UINT32)(ULONG_PTR)h : 0), m, 1);
    if (r == 0) { return 0; }               /* WM_QUIT */
    return r > 0 ? TRUE : (BOOL)-1;
}

BOOL PeekMessageA(LPMSG m, HWND h, UINT mn, UINT mx, UINT remove)
{
    (void)mn; (void)mx; (void)remove;
    if (g_quit) { m->message = WM_QUIT; return FALSE; }
    long r = NtNovaGuiGetMessage((ULONG_PTR)(h ? (UINT32)(ULONG_PTR)h : 0), m, 0);
    return r == 1 ? TRUE : FALSE;
}

BOOL TranslateMessage(const MSG *m) { (void)m; return FALSE; }

LRESULT DispatchMessageA(const MSG *m)
{
    WInfo *wi = win_of(m->hwnd);
    if (!wi || !wi->proc) return 0;
    return wi->proc(m->hwnd, m->message, m->wParam, m->lParam);
}

LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WInfo *wi = win_of(h);
    switch (msg) {
    case WM_CLOSE:   DestroyWindow(h); return 0;
    case WM_DESTROY: return 0;
    case WM_ERASEBKGND:
        if (wi) NovaGdiFill((HDC)&wi->dc, 0, 0, wi->dc.w, wi->dc.h, wi->bg);
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    }
    (void)wp; (void)lp;
    return 0;
}

VOID PostQuitMessage(int code) { g_quit = 1; g_quit_code = code; }

BOOL PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WInfo *wi = win_of(h);
    if (!wi) { if (msg == WM_QUIT) { g_quit = 1; } return FALSE; }
    NtNovaGuiPostMessage(wi->id, msg, wp, lp);
    return TRUE;
}

LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WInfo *wi = win_of(h);
    if (!wi || !wi->proc) return 0;
    return wi->proc(h, msg, wp, lp);
}

HDC BeginPaint(HWND h, LPPAINTSTRUCT ps)
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

BOOL EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    (void)ps;
    WInfo *wi = win_of(h);
    if (wi) NtNovaGuiInvalidate(wi->id);        /* present the freshly drawn bitmap */
    return TRUE;
}

HDC GetDC(HWND h) { WInfo *wi = win_of(h); return wi ? (HDC)&wi->dc : 0; }
int ReleaseDC(HWND h, HDC dc) { WInfo *wi = win_of(h); if (wi) NtNovaGuiInvalidate(wi->id); (void)dc; return 1; }

BOOL GetClientRect(HWND h, LPRECT r)
{
    WInfo *wi = win_of(h);
    if (!wi || !r) return FALSE;
    r->left = 0; r->top = 0; r->right = wi->dc.w; r->bottom = wi->dc.h;
    return TRUE;
}
BOOL GetWindowRect(HWND h, LPRECT r) { return GetClientRect(h, r); }

BOOL InvalidateRect(HWND h, const RECT *r, BOOL erase)
{
    (void)r;
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    if (erase) NovaGdiFill((HDC)&wi->dc, 0, 0, wi->dc.w, wi->dc.h, wi->bg);
    if (wi->proc) wi->proc(h, WM_PAINT, 0, 0);
    NtNovaGuiInvalidate(wi->id);
    return TRUE;
}

BOOL SetWindowTextA(HWND h, LPCSTR s)
{
    WInfo *wi = win_of(h);
    if (!wi) return FALSE;
    int n = 0; for (; s && s[n] && n < 127; n++) wi->text[n] = s[n]; wi->text[n] = 0;
    unsigned short w[128]; to_utf16(s, w, 128);
    NtNovaGuiSetText(wi->id, w);
    return TRUE;
}
int GetWindowTextA(HWND h, LPSTR s, int max)
{
    WInfo *wi = win_of(h);
    if (!wi || !s || max <= 0) return 0;
    int n = 0; for (; wi->text[n] && n < max - 1; n++) s[n] = wi->text[n]; s[n] = 0;
    return n;
}

UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *fn)
{
    (void)fn;
    WInfo *wi = win_of(h);
    if (!wi) return 0;
    return (UINT_PTR)NtNovaGuiSetTimer(wi->id, id, ms);
}
BOOL KillTimer(HWND h, UINT_PTR id) { WInfo *wi = win_of(h); if (!wi) return FALSE; NtNovaGuiKillTimer(wi->id, id); return TRUE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    unsigned short t[256], c[128];
    to_utf16(text, t, 256);
    to_utf16(caption, c, 128);
    return (int)NtNovaGuiMessageBox(t, c, type);
}

int FillRect(HDC dc, const RECT *r, HBRUSH br)
{
    if (!dc || !r) return 0;
    /* HBRUSH from gdi32 is a {kind,color,width} object; read its color */
    COLORREF c = br ? ((COLORREF *)br)[1] : 0;
    NovaGdiFill(dc, r->left, r->top, r->right, r->bottom, c);
    return 1;
}
int FrameRect(HDC dc, const RECT *r, HBRUSH br)
{
    if (!dc || !r) return 0;
    COLORREF c = br ? ((COLORREF *)br)[1] : 0;
    NovaGdiFill(dc, r->left, r->top, r->right, r->top + 1, c);
    NovaGdiFill(dc, r->left, r->bottom - 1, r->right, r->bottom, c);
    NovaGdiFill(dc, r->left, r->top, r->left + 1, r->bottom, c);
    NovaGdiFill(dc, r->right - 1, r->top, r->right, r->bottom, c);
    return 1;
}
BOOL InflateRect(LPRECT r, int dx, int dy)
{
    if (!r) return FALSE;
    r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy;
    return TRUE;
}

int DrawTextA(HDC dc, LPCSTR s, int len, LPRECT r, UINT fmt)
{
    if (!dc || !s || !r) return 0;
    if (len < 0) { len = 0; while (s[len]) len++; }
    int cw = NovaGdiCellW(), ch = NovaGdiCellH();
    int tw = len * cw;
    int x = r->left, y = r->top;
    if (fmt & DT_CENTER) x = r->left + (r->right - r->left - tw) / 2;
    else if (fmt & DT_RIGHT) x = r->right - tw;
    if (fmt & (DT_VCENTER | DT_SINGLELINE)) y = r->top + (r->bottom - r->top - ch) / 2;
    for (int i = 0; i < len; i++) NovaGdiChar(dc, x + i * cw, y, s[i]);
    return ch;
}

HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name) { (void)inst; (void)name; return (HCURSOR)1; }
HICON   LoadIconA(HINSTANCE inst, LPCSTR name)   { (void)inst; (void)name; return (HICON)1; }
HWND    GetDesktopWindow(void) { return 0; }

int GetSystemMetrics(int index)
{
    ULONG w = 0, h = 0;
    NtNovaGuiScreenSize(&w, &h);
    if (index == SM_CXSCREEN) return (int)w;
    if (index == SM_CYSCREEN) return (int)h;
    return 0;
}
