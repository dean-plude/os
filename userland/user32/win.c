/*
 * win.c — window classes, the window tree, creating and destroying
 * windows, positions, visibility, focus and activation
 */
#include "u32.h"

/* -----------------------------------------------------------------------
 * The lock (recursive; never held while a window procedure runs)
 * ----------------------------------------------------------------------- */
static SRWLOCK g_srw;
static volatile DWORD g_owner;
static int g_depth;

void u32_lock(void)
{
    DWORD t = GetCurrentThreadId();
    if (g_owner == t) { g_depth++; return; }
    AcquireSRWLockExclusive(&g_srw);
    g_owner = t;
    g_depth = 1;
}

void u32_unlock(void)
{
    if (--g_depth == 0) { g_owner = 0; ReleaseSRWLockExclusive(&g_srw); }
}

/* -----------------------------------------------------------------------
 * Strings
 * ----------------------------------------------------------------------- */
int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

WCHAR *wstrdup(const WCHAR *s)
{
    int n = wlen(s);
    WCHAR *d = malloc(2 * ((size_t)n + 1));
    if (d) { if (n) memcpy(d, s, 2 * (size_t)n); d[n] = 0; }
    return d;
}

WCHAR *a2w(const char *s, int n)
{
    if (!s) return NULL;
    if (n < 0) n = (int)strlen(s);
    int k = MultiByteToWideChar(CP_ACP, 0, s, n, NULL, 0);
    WCHAR *w = malloc(2 * ((size_t)k + 1));
    if (!w) return NULL;
    if (k) MultiByteToWideChar(CP_ACP, 0, s, n, w, k);
    w[k] = 0;
    return w;
}

char *w2a(const WCHAR *s, int n)
{
    if (!s) return NULL;
    if (n < 0) n = wlen(s);
    int k = WideCharToMultiByte(CP_ACP, 0, s, n, NULL, 0, NULL, NULL);
    char *a = malloc((size_t)k + 1);
    if (!a) return NULL;
    if (k) WideCharToMultiByte(CP_ACP, 0, s, n, a, k, NULL, NULL);
    a[k] = 0;
    return a;
}

static WCHAR fold(WCHAR c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int wcsicmp_(const WCHAR *a, const WCHAR *b)
{
    while (*a && fold(*a) == fold(*b)) { a++; b++; }
    return (int)fold(*a) - (int)fold(*b);
}

/* -----------------------------------------------------------------------
 * The window table.  A handle is
 *   (generation << 25) | (tag << 14) | ((slot + 16) << 1)
 * where the tag is the kernel's for this process (CTL_HWND_TAG), unique
 * among running processes and at least 4: handles never collide between
 * processes (a GPU process can ask about its parent's window, as on
 * Windows), never fall below 0x10000, so they cannot be mistaken for
 * HWND_BROADCAST and friends, and a stale handle to a reused slot is
 * rejected.
 * ----------------------------------------------------------------------- */
#define MAX_WND 4096
#define HWND_TAG_SHIFT 14
#define HWND_TAG_MASK  0x7FF
static Wnd *g_wnds[MAX_WND];
static UINT g_gen = 1;
static UINT g_tag;

static UINT hwnd_tag(void)
{
    if (!g_tag) {
        UINT t = (UINT)NtNovaGuiCtl(0, CTL_HWND_TAG, 0, NULL);
        g_tag = t >= 4 && t <= HWND_TAG_MASK ? t : 4;
    }
    return g_tag;
}

/* another process's window handle (as far as the layout tells) */
int hwnd_foreign(HWND h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    return v >= 0x10000 && v <= 0x7FFFFFFF && v != 0x10010 &&
           ((v >> HWND_TAG_SHIFT) & HWND_TAG_MASK) != hwnd_tag();
}

/* What the kernel knows of another process's window: 2 a desktop
 * (top-level) window with @f filled, 1 a window of a running process (only
 * the pid), 0 none.  f: { pid, thread, state, client x y w h, frame x y w h } */
int foreign_info(HWND h, INT32 f[11])
{
    memset(f, 0, 11 * sizeof(INT32));
    if (!hwnd_foreign(h)) return 0;
    return (int)NtNovaGuiCtl(0, CTL_FOREIGN, (ULONG_PTR)h, f);
}
static Wnd g_desktop_wnd;
HWND g_focus, g_active, g_capture;
int  g_capture_nc;
DWORD g_main_tid;

static Wnd *desktop(void);

Wnd *W_quiet(HWND h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v == 0x10010) return desktop();
    if (v < 0x10000 || v > 0x7FFFFFFF) return NULL;
    int slot = (int)((v >> 1) & 0x1FFF) - 16;
    if (slot < 0 || slot >= MAX_WND) return NULL;
    Wnd *w = g_wnds[slot];
    if (!w || !w->used || w->h != h) return NULL;
    return w;
}

Wnd *W(HWND h)
{
    Wnd *w = W_quiet(h);
    if (!w) SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return w;
}

static Wnd *alloc_wnd(void)
{
    LOCK();
    Wnd *w = NULL;
    for (int i = 0; i < MAX_WND; i++) {
        if (g_wnds[i] && g_wnds[i]->used) continue;
        if (!g_wnds[i]) g_wnds[i] = calloc(1, sizeof(Wnd));
        if (!g_wnds[i]) break;
        w = g_wnds[i];
        memset(w, 0, sizeof(*w));
        w->used = 1;
        w->gen = g_gen++ & 0x3F;
        w->h = (HWND)(ULONG_PTR)(((ULONG_PTR)w->gen << 25) | ((ULONG_PTR)hwnd_tag() << HWND_TAG_SHIFT) |
                                 ((ULONG_PTR)(i + 16) << 1));
        break;
    }
    UNLOCK();
    return w;
}

Wnd *top_of(Wnd *w)
{
    while (w && w->parent) w = w->parent;
    return w;
}

int is_child_of(Wnd *parent, Wnd *w)
{
    for (w = w ? w->parent : NULL; w; w = w->parent) if (w == parent) return 1;
    return 0;
}

static Wnd *container(Wnd *w) { return w->parent ? w->parent : desktop(); }

/* Z order: link @w among its siblings after @after (NULL: topmost; (Wnd*)1: bottom) */
static void unlink_wnd(Wnd *w)
{
    Wnd *c = container(w);
    if (w->prev) w->prev->next = w->next; else if (c->child == w) c->child = w->next;
    if (w->next) w->next->prev = w->prev;
    w->next = w->prev = NULL;
}

static void link_wnd(Wnd *w, Wnd *after)
{
    Wnd *c = container(w);
    if (after == (Wnd *)1) {                                /* bottom */
        Wnd *last = c->child;
        if (!last) { c->child = w; return; }
        while (last->next) last = last->next;
        last->next = w; w->prev = last;
        return;
    }
    if (!after) {                                           /* top */
        w->next = c->child;
        if (c->child) c->child->prev = w;
        c->child = w;
        return;
    }
    w->next = after->next; w->prev = after;
    if (after->next) after->next->prev = w;
    after->next = w;
}

/* -----------------------------------------------------------------------
 * The desktop window (GetDesktopWindow): the parent of every top-level
 * window, as big as the screen
 * ----------------------------------------------------------------------- */
static Wnd *desktop(void)
{
    Wnd *d = &g_desktop_wnd;
    if (!d->used) {
        d->used = 1;
        d->h = (HWND)(ULONG_PTR)0x10010;
        ULONG sw = 0, sh = 0;
        NtNovaGuiScreenSize(&sw, &sh);
        SetRect(&d->rect, 0, 0, (int)sw, (int)sh);
        d->client = d->rect;
        d->style = WS_VISIBLE | WS_CLIPCHILDREN | WS_POPUP;
        d->wide = 1;
        d->proc = DesktopProc;
    }
    return d;
}

LRESULT CALLBACK DesktopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, m, wp, lp); }

/* -----------------------------------------------------------------------
 * Classes
 * ----------------------------------------------------------------------- */
#define MAX_CLASSES 256
static WClass g_class[MAX_CLASSES];
static int g_builtins_done;

static void ensure_builtins(void)
{
    if (g_builtins_done) return;
    g_builtins_done = 1;
    if (!g_main_tid) g_main_tid = GetCurrentThreadId();
    register_builtin_classes();
}

static WClass *find_class_raw(LPCWSTR name)
{
    WClass *sys = NULL;
    for (int i = 0; i < MAX_CLASSES; i++) {
        WClass *c = &g_class[i];
        if (!c->used) continue;
        int match = (ULONG_PTR)name < 0x10000 ? c->atom == (ATOM)(ULONG_PTR)name : !wcsicmp_(c->name, name);
        if (!match) continue;
        if (!c->system) return c;                           /* the program's own first */
        if (!sys) sys = c;
    }
    return sys;
}

/* the common controls' class names: comctl32 registers them when it loads */
static int is_comctl_class(LPCWSTR n)
{
    static const WCHAR *const pre[] = { L"Sys", L"msctls_", L"ToolbarWindow32", L"ReBarWindow32",
                                        L"tooltips_class32", L"ComboBoxEx32", L"NativeFontCtl" };
    for (unsigned i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        int k = 0;
        while (pre[i][k] && (n[k] | 0x20) == (pre[i][k] | 0x20)) k++;
        if (!pre[i][k]) return 1;
    }
    return 0;
}

WClass *find_class_w(LPCWSTR name, HINSTANCE inst)
{
    static int loaded_cc;
    (void)inst;
    ensure_builtins();
    if (!name) return NULL;
    WClass *c = find_class_raw(name);
    /* a program that uses a common control without linking comctl32
     * (Windows loads it through shell32 or the manifest): load it now */
    if (!c && (ULONG_PTR)name >= 0x10000 && !loaded_cc && is_comctl_class(name)) {
        loaded_cc = 1;
        if (LoadLibraryW(L"comctl32.dll")) c = find_class_raw(name);
    }
    return c;
}

static WClass *find_class_a(LPCSTR name, HINSTANCE inst)
{
    if ((ULONG_PTR)name < 0x10000) return find_class_w((LPCWSTR)name, inst);
    WCHAR w[64];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, 64);
    w[63] = 0;
    return find_class_w(w, inst);
}

typedef struct {
    UINT style; WNDPROC proc; int cls_extra, wnd_extra; HINSTANCE inst; HICON icon; HCURSOR cursor;
    HBRUSH brush; LPCWSTR menu; LPCWSTR name; HICON icon_sm;
} ClassDef;

static ATOM register_class(const ClassDef *d, int wide, int system)
{
    ensure_builtins();
    if (!d->name || (ULONG_PTR)d->name < 0x10000) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    LOCK();
    for (int i = 0; i < MAX_CLASSES; i++)
        if (g_class[i].used && g_class[i].system == system && !wcsicmp_(g_class[i].name, d->name)) {
            UNLOCK();
            SetLastError(1410 /* ERROR_CLASS_ALREADY_EXISTS */);
            return 0;
        }
    WClass *c = NULL;
    int slot = 0;
    for (int i = 0; i < MAX_CLASSES; i++) if (!g_class[i].used) { c = &g_class[i]; slot = i; break; }
    if (!c) { UNLOCK(); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    int n = 0;
    for (; d->name[n] && n < 256; n++) c->name[n] = d->name[n];
    c->name[n] = 0;
    c->proc = d->proc; c->wide = wide; c->style = d->style;
    c->extra = d->wnd_extra < 0 ? 0 : d->wnd_extra;
    c->cls_extra = d->cls_extra < 0 ? 0 : d->cls_extra;
    if (c->cls_extra) c->cls_data = calloc(1, (size_t)c->cls_extra);
    c->inst = d->inst; c->icon = d->icon; c->icon_sm = d->icon_sm; c->cursor = d->cursor; c->brush = d->brush;
    c->menu = (ULONG_PTR)d->menu < 0x10000 ? d->menu : wstrdup(d->menu);
    c->system = system;
    /* the system's own atoms for the dialog, menu and desktop classes */
    if (!wcscmp(c->name, L"#32770")) c->atom = 32770;
    else if (!wcscmp(c->name, L"#32768")) c->atom = 32768;
    else if (!wcscmp(c->name, L"#32769")) c->atom = 32769;
    else c->atom = (ATOM)(0xC000 + slot);
    UNLOCK();
    return c->atom;
}

ATOM register_system_class(LPCWSTR name, WNDPROC proc, UINT style, int extra, HBRUSH brush, HCURSOR cursor)
{
    ClassDef d;
    memset(&d, 0, sizeof(d));
    d.name = name; d.proc = proc; d.style = style | CS_GLOBALCLASS; d.wnd_extra = extra; d.brush = brush;
    d.cursor = cursor;
    return register_class(&d, 1, 1);
}

USERAPI ATOM RegisterClassExW(const WNDCLASSEXW *wc)
{
    ClassDef d = { wc->style, wc->lpfnWndProc, wc->cbClsExtra, wc->cbWndExtra, wc->hInstance, wc->hIcon, wc->hCursor,
                   wc->hbrBackground, wc->lpszMenuName, wc->lpszClassName, wc->hIconSm };
    return register_class(&d, 1, 0);
}

USERAPI ATOM RegisterClassW(const WNDCLASSW *wc)
{
    ClassDef d = { wc->style, wc->lpfnWndProc, wc->cbClsExtra, wc->cbWndExtra, wc->hInstance, wc->hIcon, wc->hCursor,
                   wc->hbrBackground, wc->lpszMenuName, wc->lpszClassName, 0 };
    return register_class(&d, 1, 0);
}

static ATOM register_class_a(UINT style, WNDPROC proc, int ce, int we, HINSTANCE inst, HICON icon, HCURSOR cur,
                             HBRUSH br, LPCSTR menu, LPCSTR name, HICON sm)
{
    WCHAR *wn = (ULONG_PTR)name < 0x10000 ? NULL : a2w(name, -1);
    WCHAR *wm = (ULONG_PTR)menu < 0x10000 ? (WCHAR *)menu : a2w(menu, -1);
    ClassDef d = { style, proc, ce, we, inst, icon, cur, br, wm, wn ? wn : (LPCWSTR)name, sm };
    ATOM a = register_class(&d, 0, 0);
    free(wn);
    if ((ULONG_PTR)menu >= 0x10000) free(wm);
    return a;
}

USERAPI ATOM RegisterClassExA(const WNDCLASSEXA *wc)
{
    return register_class_a(wc->style, wc->lpfnWndProc, wc->cbClsExtra, wc->cbWndExtra, wc->hInstance, wc->hIcon,
                            wc->hCursor, wc->hbrBackground, wc->lpszMenuName, wc->lpszClassName, wc->hIconSm);
}

USERAPI ATOM RegisterClassA(const WNDCLASSA *wc)
{
    return register_class_a(wc->style, wc->lpfnWndProc, wc->cbClsExtra, wc->cbWndExtra, wc->hInstance, wc->hIcon,
                            wc->hCursor, wc->hbrBackground, wc->lpszMenuName, wc->lpszClassName, 0);
}

USERAPI BOOL UnregisterClassW(LPCWSTR name, HINSTANCE inst)
{
    WClass *c = find_class_w(name, inst);
    if (!c || c->system) { SetLastError(1411 /* ERROR_CLASS_DOES_NOT_EXIST */); return FALSE; }
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnds[i] && g_wnds[i]->used && g_wnds[i]->cls == c) { SetLastError(1412 /* ERROR_CLASS_HAS_WINDOWS */); return FALSE; }
    c->used = 0;
    return TRUE;
}

USERAPI BOOL UnregisterClassA(LPCSTR name, HINSTANCE inst)
{
    if ((ULONG_PTR)name < 0x10000) return UnregisterClassW((LPCWSTR)name, inst);
    WCHAR *w = a2w(name, -1);
    BOOL r = UnregisterClassW(w, inst);
    free(w);
    return r;
}

static void fill_class_w(WClass *c, WNDCLASSEXW *wc, LPCWSTR name)
{
    wc->style = c->style & ~CS_GLOBALCLASS; wc->lpfnWndProc = c->proc; wc->cbClsExtra = c->cls_extra;
    wc->cbWndExtra = c->extra; wc->hInstance = c->inst; wc->hIcon = c->icon; wc->hCursor = c->cursor;
    wc->hbrBackground = c->brush; wc->lpszMenuName = c->menu; wc->lpszClassName = name;
}

USERAPI BOOL GetClassInfoExW(HINSTANCE inst, LPCWSTR name, WNDCLASSEXW *wc)
{
    WClass *c = find_class_w(name, inst);
    if (!c) { SetLastError(1411); return FALSE; }
    fill_class_w(c, wc, name);
    if (wc->cbSize >= sizeof(WNDCLASSEXW)) wc->hIconSm = c->icon_sm;
    return c->atom;
}

USERAPI BOOL GetClassInfoW(HINSTANCE inst, LPCWSTR name, WNDCLASSW *wc)
{
    WNDCLASSEXW x;
    x.cbSize = sizeof(x);
    BOOL r = GetClassInfoExW(inst, name, &x);
    if (r) {
        /* field by field: in 64-bit code WNDCLASS has padding after style where WNDCLASSEX has none */
        wc->style = x.style; wc->lpfnWndProc = x.lpfnWndProc; wc->cbClsExtra = x.cbClsExtra; wc->cbWndExtra = x.cbWndExtra;
        wc->hInstance = x.hInstance; wc->hIcon = x.hIcon; wc->hCursor = x.hCursor; wc->hbrBackground = x.hbrBackground;
        wc->lpszMenuName = x.lpszMenuName; wc->lpszClassName = x.lpszClassName;
    }
    return r;
}

USERAPI BOOL GetClassInfoExA(HINSTANCE inst, LPCSTR name, WNDCLASSEXA *wc)
{
    WClass *c = find_class_a(name, inst);
    if (!c) { SetLastError(1411); return FALSE; }
    WNDCLASSEXW x;
    fill_class_w(c, &x, 0);
    wc->style = x.style; wc->lpfnWndProc = x.lpfnWndProc; wc->cbClsExtra = x.cbClsExtra; wc->cbWndExtra = x.cbWndExtra;
    wc->hInstance = x.hInstance; wc->hIcon = x.hIcon; wc->hCursor = x.hCursor; wc->hbrBackground = x.hbrBackground;
    wc->lpszMenuName = (LPCSTR)x.lpszMenuName; wc->lpszClassName = name;
    if (wc->cbSize >= sizeof(WNDCLASSEXA)) wc->hIconSm = c->icon_sm;
    return c->atom;
}

USERAPI BOOL GetClassInfoA(HINSTANCE inst, LPCSTR name, WNDCLASSA *wc)
{
    WNDCLASSEXA x;
    x.cbSize = sizeof(x);
    BOOL r = GetClassInfoExA(inst, name, &x);
    if (r) {
        /* field by field: in 64-bit code WNDCLASS has padding after style where WNDCLASSEX has none */
        wc->style = x.style; wc->lpfnWndProc = x.lpfnWndProc; wc->cbClsExtra = x.cbClsExtra; wc->cbWndExtra = x.cbWndExtra;
        wc->hInstance = x.hInstance; wc->hIcon = x.hIcon; wc->hCursor = x.hCursor; wc->hbrBackground = x.hbrBackground;
        wc->lpszMenuName = x.lpszMenuName; wc->lpszClassName = x.lpszClassName;
    }
    return r;
}

USERAPI int GetClassNameW(HWND h, LPWSTR buf, int n)
{
    Wnd *w = W(h);
    if (!w || n <= 0) return 0;
    const WCHAR *s = w->cls ? w->cls->name : L"#32769";
    int k = 0;
    for (; s[k] && k < n - 1; k++) buf[k] = s[k];
    buf[k] = 0;
    return k;
}

USERAPI int GetClassNameA(HWND h, LPSTR buf, int n)
{
    WCHAR w[257];
    int k = GetClassNameW(h, w, 257);
    if (!k || n <= 0) { if (n > 0) buf[0] = 0; return 0; }
    int m = WideCharToMultiByte(CP_ACP, 0, w, k, buf, n - 1, NULL, NULL);
    buf[m] = 0;
    return m;
}

USERAPI UINT RealGetWindowClassW(HWND h, LPWSTR buf, UINT n) { return (UINT)GetClassNameW(h, buf, (int)n); }
USERAPI UINT RealGetWindowClassA(HWND h, LPSTR buf, UINT n) { return (UINT)GetClassNameA(h, buf, (int)n); }

USERAPI ULONG_PTR GetClassLongPtrW(HWND h, int i)
{
    Wnd *w = W(h);
    if (!w || !w->cls) return 0;
    WClass *c = w->cls;
    switch (i) {
    case GCLP_WNDPROC:       return (ULONG_PTR)c->proc;
    case GCLP_HBRBACKGROUND: return (ULONG_PTR)c->brush;
    case GCLP_HCURSOR:       return (ULONG_PTR)c->cursor;
    case GCLP_HICON:         return (ULONG_PTR)c->icon;
    case GCLP_HICONSM:       return (ULONG_PTR)c->icon_sm;
    case GCLP_HMODULE:       return (ULONG_PTR)c->inst;
    case GCLP_MENUNAME:      return (ULONG_PTR)c->menu;
    case GCL_STYLE:          return c->style;
    case GCL_CBWNDEXTRA:     return (ULONG_PTR)c->extra;
    case GCL_CBCLSEXTRA:     return (ULONG_PTR)c->cls_extra;
    case GCW_ATOM:           return c->atom;
    }
    if (i >= 0 && i + 4 <= c->cls_extra) {
        ULONG_PTR v = 0;
        memcpy(&v, c->cls_data + i, i + 8 <= c->cls_extra ? 8 : 4);
        return v;
    }
    SetLastError(1413 /* ERROR_INVALID_INDEX */);
    return 0;
}

USERAPI ULONG_PTR SetClassLongPtrW(HWND h, int i, LONG_PTR v)
{
    Wnd *w = W(h);
    if (!w || !w->cls) return 0;
    WClass *c = w->cls;
    ULONG_PTR old = GetClassLongPtrW(h, i);
    switch (i) {
    case GCLP_WNDPROC:       c->proc = (WNDPROC)v; break;
    case GCLP_HBRBACKGROUND: c->brush = (HBRUSH)v; break;
    case GCLP_HCURSOR:       c->cursor = (HCURSOR)v; break;
    case GCLP_HICON:         c->icon = (HICON)v; break;
    case GCLP_HICONSM:       c->icon_sm = (HICON)v; break;
    case GCL_STYLE:          c->style = (UINT)v; break;
    default:
        if (i >= 0 && i + 4 <= c->cls_extra) memcpy(c->cls_data + i, &v, i + 8 <= c->cls_extra ? 8 : 4);
    }
    return old;
}

USERAPI ULONG_PTR GetClassLongPtrA(HWND h, int i) { return GetClassLongPtrW(h, i); }
USERAPI ULONG_PTR SetClassLongPtrA(HWND h, int i, LONG_PTR v) { return SetClassLongPtrW(h, i, v); }
USERAPI DWORD GetClassLongW(HWND h, int i) { return (DWORD)GetClassLongPtrW(h, i); }
USERAPI DWORD GetClassLongA(HWND h, int i) { return (DWORD)GetClassLongPtrW(h, i); }
USERAPI DWORD SetClassLongW(HWND h, int i, LONG v) { return (DWORD)SetClassLongPtrW(h, i, v); }
USERAPI DWORD SetClassLongA(HWND h, int i, LONG v) { return (DWORD)SetClassLongPtrW(h, i, v); }
USERAPI WORD GetClassWord(HWND h, int i) { return (WORD)GetClassLongPtrW(h, i); }

/* -----------------------------------------------------------------------
 * Geometry
 * ----------------------------------------------------------------------- */
/* Top-level windows the desktop frames (a title bar and a border) */
static int framed(Wnd *w)
{
    if (w->parent) return 0;
    if (w->flags & WF_MENU_TRACK) return 0;
    return (w->style & WS_CAPTION) == WS_CAPTION || !(w->style & WS_POPUP);
}

static void kernel_insets(Wnd *w, RECT *r)
{
    if (!framed(w)) { SetRectEmpty(r); return; }
    int b = w->maximized ? 0 : FRAME_BORDER;
    SetRect(r, b, FRAME_TITLE, b, b);
}

/* The window's own border, drawn by user32 (children; frameless top-level) */
static int border_width(Wnd *w)
{
    int b = 0;
    if (!framed(w)) {
        if (w->style & WS_THICKFRAME) b += w->parent ? 3 : 1;
        else if ((w->style & WS_CAPTION) == WS_DLGFRAME || (w->exstyle & WS_EX_DLGMODALFRAME)) b += w->parent ? 3 : 1;
        else if (w->style & WS_BORDER) b += 1;
    }
    if (w->exstyle & WS_EX_CLIENTEDGE) b += 2;
    if (w->exstyle & WS_EX_STATICEDGE) b += 1;
    return b;
}

static int has_menu_bar(Wnd *w)
{
    return !w->parent && w->menu && !(w->style & WS_CHILD);
}

/* DefWindowProc's WM_NCCALCSIZE: the client area of window rectangle @r */
void default_nc_calc(Wnd *w, RECT *r)
{
    RECT k;
    kernel_insets(w, &k);
    r->left += k.left; r->top += k.top; r->right -= k.right; r->bottom -= k.bottom;
    int b = border_width(w);
    r->left += b; r->top += b; r->right -= b; r->bottom -= b;
    if (has_menu_bar(w)) r->top += menu_bar_height(w, r->right - r->left);
    if ((w->style & WS_VSCROLL) && r->right - r->left > sb_width()) r->right -= sb_width();
    if ((w->style & WS_HSCROLL) && r->bottom - r->top > sb_width()) r->bottom -= sb_width();
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
}

/* A program's client area never reaches over the frame the desktop draws
 * (the title bar and borders of a framed top-level window).  Programs that
 * draw their own title bar (SumatraPDF's tab bar) take the whole window as
 * client area in WM_NCCALCSIZE, which Windows' DWM allows; here their title
 * bar goes under the desktop's. */
static void bitmap_rect(Wnd *w, RECT *b);
static void clamp_client(Wnd *w)
{
    if (w->parent) return;
    RECT b;
    bitmap_rect(w, &b);
    if (w->client.left < b.left) w->client.left = b.left;
    if (w->client.top < b.top) w->client.top = b.top;
    if (w->client.right > b.right) w->client.right = b.right;
    if (w->client.bottom > b.bottom) w->client.bottom = b.bottom;
    if (w->client.right < w->client.left) w->client.right = w->client.left;
    if (w->client.bottom < w->client.top) w->client.bottom = w->client.top;
}

/* Tell the kernel a desktop window's client area, for other processes'
 * GetClientRect (CTL_FOREIGN) */
static void publish_client(Wnd *w)
{
    if (!w->kid || w->parent) return;
    INT32 uc[4] = { w->client.left - w->rect.left, w->client.top - w->rect.top,
                    w->client.right - w->client.left, w->client.bottom - w->client.top };
    NtNovaGuiCtl(w->kid, CTL_SET_HWND, (ULONG_PTR)w->h, uc);
}

void wnd_calc_client(Wnd *w)
{
    NCCALCSIZE_PARAMS p;
    WINDOWPOS pos = { w->h, 0, w->rect.left, w->rect.top, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top,
                      SWP_NOZORDER | SWP_NOACTIVATE };
    memset(&p, 0, sizeof(p));
    p.rgrc[0] = w->rect;
    p.rgrc[1] = w->rect;
    p.rgrc[2] = w->client;
    p.lppos = &pos;                                         /* always there when wParam is TRUE */
    if (w->proc && (w->flags & WF_CREATED)) send_msg(w, WM_NCCALCSIZE, TRUE, (LPARAM)&p);
    else default_nc_calc(w, &p.rgrc[0]);
    w->client = p.rgrc[0];
    clamp_client(w);
    publish_client(w);
}

void wnd_screen_origin(Wnd *w, int client, POINT *p)
{
    p->x = client ? w->client.left : w->rect.left;
    p->y = client ? w->client.top : w->rect.top;
    for (Wnd *a = w->parent; a; a = a->parent) { p->x += a->client.left; p->y += a->client.top; }
}

void wnd_to_bitmap(Wnd *w, int client, POINT *p)
{
    wnd_screen_origin(w, client, p);
    Wnd *t = top_of(w);
    p->x -= t->bmp.x;
    p->y -= t->bmp.y;
}

int wnd_visible(Wnd *w)
{
    for (; w; w = w->parent) if (!(w->style & WS_VISIBLE) || w->minimized) return 0;
    return 1;
}

/* Where the top-level window's bitmap is on screen, from its window rectangle */
static void bitmap_rect(Wnd *w, RECT *b)
{
    RECT k;
    kernel_insets(w, &k);
    *b = w->rect;
    b->left += k.left; b->top += k.top; b->right -= k.right; b->bottom -= k.bottom;
    if (b->right <= b->left) b->right = b->left + 1;
    if (b->bottom <= b->top) b->bottom = b->top + 1;
}

/* Tell the desktop the window's new position and size */
void update_kernel_rect(Wnd *w)
{
    RECT b;
    bitmap_rect(w, &b);
    int nw = b.right - b.left, nh = b.bottom - b.top;
    if (w->maxw && nw > w->maxw) nw = w->maxw;
    if (w->maxh && nh > w->maxh) nh = w->maxh;
    int resized = nw != w->bw || nh != w->bh;
    w->bmp.x = b.left; w->bmp.y = b.top;
    w->bw = nw; w->bh = nh;
    if (w->kid) {
        INT32 in[4] = { b.left, b.top, nw, nh };
        NtNovaGuiCtl(w->kid, CTL_SET_RECT, 3, in);
    }
    if (resized) top_resized(w);
}

/* The desktop moved, resized, minimized or maximized the window */
void top_sync_from_kernel(Wnd *w, int sized)
{
    INT32 r[9];
    if (!w->kid || !NtNovaGuiCtl(w->kid, CTL_GET_RECT, 0, r)) return;
    RECT old = w->rect, oldc = w->client;
    int was_min = w->minimized, was_max = w->maximized;
    w->minimized = (r[8] & 4) != 0;
    w->maximized = (r[8] & 8) != 0;
    SetRect(&w->rect, r[4], r[5], r[4] + r[6], r[5] + r[7]);
    w->bmp.x = r[0]; w->bmp.y = r[1];
    int resized = r[2] != w->bw || r[3] != w->bh;
    w->bw = r[2]; w->bh = r[3];
    wnd_calc_client(w);
    if (resized) top_resized(w);
    if (!EqualRect(&old, &w->rect) || !EqualRect(&oldc, &w->client) || was_min != w->minimized || was_max != w->maximized || sized) {
        WINDOWPOS p = { w->h, 0, w->rect.left, w->rect.top, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE };
        if (oldc.left == w->client.left && oldc.top == w->client.top) p.flags |= 0x0800;    /* SWP_NOCLIENTMOVE */
        if (oldc.right - oldc.left == w->client.right - w->client.left &&
            oldc.bottom - oldc.top == w->client.bottom - w->client.top && was_min == w->minimized && was_max == w->maximized)
            p.flags |= 0x1000;                                                                /* SWP_NOCLIENTSIZE */
        send_msg(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&p);
    }
}

/* The heart of SetWindowPos */
void wnd_set_pos(Wnd *w, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (!w || w == desktop()) return;
    WINDOWPOS p = { w->h, after, x, y, cx, cy, flags };
    if (flags & SWP_NOMOVE) { p.x = w->rect.left; p.y = w->rect.top; }
    if (flags & SWP_NOSIZE) { p.cx = w->rect.right - w->rect.left; p.cy = w->rect.bottom - w->rect.top; }
    if (p.cx < 0) p.cx = 0;
    if (p.cy < 0) p.cy = 0;
    if (!(flags & SWP_NOSENDCHANGING)) send_msg(w, WM_WINDOWPOSCHANGING, 0, (LPARAM)&p);
    if (!W_quiet(p.hwnd)) return;
    flags = p.flags;
    RECT old = w->rect, oldc = w->client;
    RECT nr = { p.x, p.y, p.x + p.cx, p.y + p.cy };
    int moved = nr.left != old.left || nr.top != old.top;
    int sized = (nr.right - nr.left) != (old.right - old.left) || (nr.bottom - nr.top) != (old.bottom - old.top);
    int was_visible = wnd_visible(w);

    if (moved || sized || (flags & SWP_FRAMECHANGED)) {
        w->rect = nr;
        NCCALCSIZE_PARAMS np;
        memset(&np, 0, sizeof(np));
        np.rgrc[0] = nr; np.rgrc[1] = old; np.rgrc[2] = oldc; np.lppos = &p;
        send_msg(w, WM_NCCALCSIZE, TRUE, (LPARAM)&np);
        w->client = np.rgrc[0];
        clamp_client(w);
    }
    if (!(flags & SWP_NOZORDER) && w->parent) {
        Wnd *a = NULL;
        if (after == HWND_BOTTOM) a = (Wnd *)1;
        else if (after == HWND_TOP || after == HWND_TOPMOST || after == HWND_NOTOPMOST) a = NULL;
        else a = W_quiet(after);
        if (a != w && (a == (Wnd *)1 || !a || a->parent == w->parent)) { unlink_wnd(w); link_wnd(w, a); }
    }
    if (flags & SWP_SHOWWINDOW) w->style |= WS_VISIBLE;
    if (flags & SWP_HIDEWINDOW) w->style &= ~WS_VISIBLE;

    if (!w->parent) {
        if (moved || sized || (flags & SWP_FRAMECHANGED)) update_kernel_rect(w);
        if (flags & SWP_SHOWWINDOW) ShowWindow(w->h, (flags & SWP_NOACTIVATE) ? SW_SHOWNA : SW_SHOW);
        if (flags & SWP_HIDEWINDOW) ShowWindow(w->h, SW_HIDE);
        if (!(flags & SWP_NOZORDER) && !(flags & SWP_NOACTIVATE) && w->kid && (w->style & WS_VISIBLE))
            NtNovaGuiCtl(w->kid, CTL_ACTIVATE, 0, NULL);
        if (flags & SWP_FRAMECHANGED) invalidate_nc(w);
    } else if (!(flags & SWP_NOREDRAW)) {
        int vis = wnd_visible(w);
        if (was_visible && (moved || sized || !vis)) invalidate(w->parent, &old, TRUE, 1);   /* uncover what it covered */
        if (vis && (moved || sized || !was_visible || (flags & SWP_FRAMECHANGED))) {
            invalidate_nc(w);
            invalidate(w, NULL, TRUE, 1);
        }
    }
    if (moved || sized || (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED)) || !(flags & SWP_NOZORDER)) {
        p.x = w->rect.left; p.y = w->rect.top;
        p.cx = w->rect.right - w->rect.left; p.cy = w->rect.bottom - w->rect.top;
        if (oldc.left == w->client.left && oldc.top == w->client.top) p.flags |= 0x0800;
        if (oldc.right - oldc.left == w->client.right - w->client.left && oldc.bottom - oldc.top == w->client.bottom - w->client.top)
            p.flags |= 0x1000;
        if (w->flags & WF_CREATED) send_msg(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&p);
    }
}

/* -----------------------------------------------------------------------
 * The desktop's window for a top-level window: made when it is first
 * shown (hidden helper windows never need one)
 * ----------------------------------------------------------------------- */
static int kernel_window(Wnd *w)
{
    if (w->kid) return 1;
    GuiCreate gc;
    memset(&gc, 0, sizeof(gc));
    RECT b;
    bitmap_rect(w, &b);
    gc.x = b.left; gc.y = b.top; gc.w = b.right - b.left; gc.h = b.bottom - b.top;
    gc.title = (UINT64)(ULONG_PTR)(w->text ? w->text : L"");
    gc.flags = GUI_HIDDEN | GUI_HOVER;
    gc.style = w->tid;                                      /* its thread gets its input */
    int popup_like = (w->flags & WF_MENU_TRACK) || (w->exstyle & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE));
    if (!framed(w)) gc.flags |= popup_like ? GUI_POPUP : GUI_NOFRAME;
    else {
        if (w->style & WS_THICKFRAME) gc.flags |= GUI_RESIZABLE;
        if (!(w->style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX))) gc.flags |= GUI_NOMINMAX;
        if (!(w->style & WS_SYSMENU)) gc.flags |= GUI_NOCLOSE;
    }
    if (w->exstyle & WS_EX_NOACTIVATE) gc.flags |= GUI_NOACTIVATE;
    Wnd *o = w->owner ? top_of(w->owner) : NULL;
    if (o && !o->kid && o != w) kernel_window(o);
    gc.owner = o ? o->kid : 0;
    if (!NtNovaGuiCreate(&gc) || !gc.hwnd) return 0;
    w->kid = (UINT32)gc.hwnd;
    publish_client(w);
    if (w->drop_accept) NtNovaGuiCtl(w->kid, CTL_ACCEPT_DROPS, (w->drop_accept | (w->drop_accept >> 2)) & 3, NULL);
    w->front = (DWORD *)(ULONG_PTR)gc.bitmap;
    w->stride = (int)gc.stride / 4;
    w->maxw = w->stride;
    ULONG sw = 0, sh = 0;
    NtNovaGuiScreenSize(&sw, &sh);
    w->maxh = (int)sh;
    if (!w->back) {
        w->back = VirtualAlloc(NULL, (SIZE_T)w->stride * w->maxh * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    top_sync_from_kernel(w, 0);
    /* the whole bitmap is new */
    RECT all = { 0, 0, w->bw, w->bh };
    mark_dirty(w, &all);
    return 1;
}

/* A back buffer to draw into (before the window is shown too) */
int ensure_back(Wnd *t)
{
    if (t->back) return 1;
    ULONG sw = 0, sh = 0;
    NtNovaGuiScreenSize(&sw, &sh);
    if (!t->stride) { t->stride = t->maxw = sw > 2560 ? 2560 : (int)sw; t->maxh = (int)sh; }
    t->back = VirtualAlloc(NULL, (SIZE_T)t->stride * t->maxh * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t->bw) {
        RECT b;
        bitmap_rect(t, &b);
        t->bmp.x = b.left; t->bmp.y = b.top;
        t->bw = MIN(b.right - b.left, t->maxw);
        t->bh = MIN(b.bottom - b.top, t->maxh);
    }
    return t->back != NULL;
}

/* -----------------------------------------------------------------------
 * Creating windows
 * ----------------------------------------------------------------------- */
static void work_area(RECT *r)
{
    INT32 wa[4];
    if (NtNovaGuiCtl(0, CTL_WORKAREA, 0, wa)) SetRect(r, wa[0], wa[1], wa[0] + wa[2], wa[1] + wa[3]);
    else *r = desktop()->rect;
}

/* the kernel log gets why a window was not created: the usual first sign
 * of a program failing on a missing or unfinished control */
static void create_failed(LPCVOID cls_arg, int wide, const char *why)
{
    char b[160], name[64];
    int n = 0;
    if ((ULONG_PTR)cls_arg < 0x10000) {
        ULONG_PTR a = (ULONG_PTR)cls_arg;
        name[n++] = '#';
        char t[8]; int k = 0;
        do { t[k++] = (char)('0' + a % 10); a /= 10; } while (a);
        while (k) name[n++] = t[--k];
    } else if (wide) {
        for (const WCHAR *c = cls_arg; *c && n < 63; c++) name[n++] = *c < 128 ? (char)*c : '?';
    } else {
        for (const char *c = cls_arg; *c && n < 63; c++) name[n++] = *c;
    }
    name[n] = 0;
    char *o = b;
    for (const char *c = "user32: window of class "; *c; ) *o++ = *c++;
    for (const char *c = name; *c; ) *o++ = *c++;
    *o++ = ' ';
    for (const char *c = why; *c && o < b + 157; ) *o++ = *c++;
    *o++ = '\n'; *o = 0;
    OutputDebugStringA(b);
}

static HWND create_window(DWORD ex, WClass *cls, LPCWSTR title, DWORD style, int x, int y, int cx, int cy,
                          HWND hparent, HMENU menu, HINSTANCE inst, LPVOID param, int caller_wide,
                          LPCVOID cls_arg, LPCVOID title_arg)
{
    ensure_builtins();
    if (!cls) { create_failed(cls_arg, caller_wide, "not created: no such class"); SetLastError(1407 /* ERROR_CANNOT_FIND_WND_CLASS */); return 0; }
    Wnd *parent = NULL, *owner = NULL;
    int message_only = hparent == HWND_MESSAGE;
    if (hparent && !message_only) {
        Wnd *p = W(hparent);
        if (!p) return 0;
        if (style & WS_CHILD) parent = p == desktop() ? NULL : p;
        else owner = p == desktop() ? NULL : top_of(p);
    } else if ((style & WS_CHILD) && !message_only) {
        SetLastError(1406 /* ERROR_TLW_WITH_WSCHILD */);
        return 0;
    }
    if (!parent) style &= ~WS_CHILD;

    Wnd *w = alloc_wnd();
    if (!w) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    w->cls = cls;
    w->proc = cls->proc;
    w->wide = cls->wide;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = ex;
    w->inst = inst;
    w->tid = GetCurrentThreadId();
    w->parent = parent;
    w->owner = owner;
    if (cls->extra) w->extra = calloc(1, (size_t)cls->extra + 8);
    w->text = wstrdup(title ? title : L"");
    if (parent) w->id = (LONG_PTR)menu;
    if (!g_main_tid) g_main_tid = w->tid;

    /* Where it goes */
    if (!parent) {
        if (!(style & WS_POPUP)) {                          /* overlapped windows get the caption */
            w->style |= WS_CAPTION | WS_CLIPSIBLINGS;
        }
        RECT wa;
        work_area(&wa);
        int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
        if (cx == CW_USEDEFAULT || (x == CW_USEDEFAULT && cx <= 0)) { cx = ww * 3 / 4; cy = wh * 3 / 4; }
        if (cy == CW_USEDEFAULT) cy = wh * 3 / 4;
        if (x == CW_USEDEFAULT) {
            static int cascade;
            x = wa.left + (ww - cx) / 2 + cascade;
            y = wa.top + (wh - cy) / 2 + cascade;
            cascade = (cascade + 26) % 130;
            if (y < wa.top) y = wa.top;
            if (x < wa.left) x = wa.left;
        }
        if (message_only) w->flags |= WF_MAPPED;            /* never shown */
        if (!wcscmp(cls->name, L"#32768") || !wcscmp(cls->name, L"ComboLBox") || !wcscmp(cls->name, L"tooltips_class32"))
            w->flags |= WF_MENU_TRACK;                      /* a popup: no frame, no focus */
    } else {
        if (x == CW_USEDEFAULT) x = y = 0;
        if (cx == CW_USEDEFAULT) cx = cy = 0;
    }
    SetRect(&w->rect, x, y, x + cx, y + cy);
    LOCK();
    link_wnd(w, parent ? (Wnd *)1 : NULL);
    UNLOCK();

    /* The menu bar */
    if (!parent && !message_only) {
        if (menu) w->menu = menu;
        else if (cls->menu) w->menu = LoadMenuW(cls->inst ? cls->inst : inst, cls->menu);
        w->id = (LONG_PTR)w->menu;
    }
    if (!parent) {
        w->client = w->rect;
        default_nc_calc(w, &w->client);
        RECT b;
        bitmap_rect(w, &b);
        w->bmp.x = b.left; w->bmp.y = b.top;
        w->bw = b.right - b.left; w->bh = b.bottom - b.top;
    }

    /* WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE */
    HWND h = w->h;
    CREATESTRUCTW cs;
    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = param; cs.hInstance = inst; cs.hMenu = menu; cs.hwndParent = hparent;
    cs.cx = cx; cs.cy = cy; cs.x = x; cs.y = y; cs.style = (LONG)style; cs.dwExStyle = ex;
    cs.lpszName = (LPCWSTR)title_arg;
    cs.lpszClass = (LPCWSTR)cls_arg;
    {
        struct { CREATESTRUCTW *lpcs; HWND hwndInsertAfter; } cbt = { &cs, HWND_TOP };   /* CBT_CREATEWNDW */
        if (cbt_hook(3 /* HCBT_CREATEWND */, (WPARAM)h, (LPARAM)&cbt)) {
            create_failed(cls_arg, caller_wide, "refused by a WH_CBT hook");
            if (W_quiet(h)) DestroyWindow(h);
            return 0;
        }
        if (!W_quiet(h)) return 0;
    }
    w->flags |= WF_CREATED;                                 /* messages flow from here */
    int ok = (int)call_proc(w, w->proc, w->wide, h, WM_NCCREATE, 0, (LPARAM)&cs, caller_wide);
    if (!W_quiet(h)) return 0;
    if (!ok) { create_failed(cls_arg, caller_wide, "refused WM_NCCREATE"); DestroyWindow(h); SetLastError(ERROR_CANNOT_FIND_WND_CLASS - 1407 + 1400 + 7); return 0; }
    wnd_calc_client(w);
    if (!w->parent && ensure_back(w)) {}
    if (call_proc(w, w->proc, w->wide, h, WM_CREATE, 0, (LPARAM)&cs, caller_wide) == -1) {
        create_failed(cls_arg, caller_wide, "refused WM_CREATE");
        if (W_quiet(h)) DestroyWindow(h);
        return 0;
    }
    if (!W_quiet(h)) return 0;
    if (w->parent && !(w->exstyle & WS_EX_NOPARENTNOTIFY)) {
        for (Wnd *p = w->parent, *c = w; p && !(c->exstyle & WS_EX_NOPARENTNOTIFY); c = p, p = p->parent) {
            send_msg(p, WM_PARENTNOTIFY, MAKEWPARAM(WM_CREATE, (WORD)w->id), (LPARAM)h);
            if (!(p->style & WS_CHILD)) break;
        }
        if (!W_quiet(h)) return 0;
    }
    /* WM_SIZE and WM_MOVE, as the first SetWindowPos would; an overlapped
     * window gets them when it is first shown, as on Windows (programs
     * create the window before the state its WM_SIZE handler needs) */
    if (style & (WS_CHILD | WS_POPUP)) {
        send_msg(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->client.right - w->client.left, w->client.bottom - w->client.top));
        if (!W_quiet(h)) return 0;
        send_msg(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
        if (!W_quiet(h)) return 0;
    } else w->flags |= WF_NEED_SIZE;
    if (style & WS_VISIBLE) {
        int cmd = (style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED : (style & WS_MINIMIZE) ? SW_SHOWMINIMIZED : SW_SHOW;
        if (w->flags & WF_MENU_TRACK) cmd = SW_SHOWNA;
        ShowWindow(h, cmd);
    }
    return W_quiet(h) ? h : 0;
}

USERAPI HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                             HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r = create_window(ex, find_class_w(cls, inst), title, style, x, y, w, h, parent, menu, inst, param, 1, cls, title);
    return r;
}

USERAPI HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                             HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    WCHAR *wt = (ULONG_PTR)title < 0x10000 ? NULL : a2w(title, -1);
    HWND r = create_window(ex, find_class_a(cls, inst), wt, style, x, y, w, h, parent, menu, inst, param, 0, cls, title);
    free(wt);
    return r;
}

/* -----------------------------------------------------------------------
 * Destroying windows
 * ----------------------------------------------------------------------- */
static void free_wnd(Wnd *w)
{
    paint_drop_kept(w);
    if (w->kid) { NtNovaGuiDestroy(w->kid); w->kid = 0; }
    if (w->back) { VirtualFree(w->back, 0, MEM_RELEASE); w->back = NULL; }
    for (Prop *p = w->props, *n; p; p = n) { n = p->next; free(p->name); free(p); }
    free(w->text); free(w->extra);
    if (w->menu && !w->parent) DestroyMenu(w->menu);
    if (w->sysmenu) DestroyMenu(w->sysmenu);
    LOCK();
    w->used = 0;
    w->h = 0;
    UNLOCK();
}

static void destroy_tree(Wnd *w)
{
    HWND h = w->h;
    w->flags |= WF_DESTROYING;
    send_msg(w, WM_DESTROY, 0, 0);
    if (!W_quiet(h)) return;
    while (w->child) {
        Wnd *c = w->child;
        if (c->flags & WF_DESTROYING) { LOCK(); unlink_wnd(c); UNLOCK(); continue; }
        destroy_tree(c);
    }
    kill_window_timers(h);
    remove_window_messages(h);
    if (g_focus == h) g_focus = 0;
    if (g_capture == h) { g_capture = 0; g_capture_nc = 0; }
    if (g_active == h) g_active = 0;
    send_msg(w, WM_NCDESTROY, 0, 0);
    LOCK();
    unlink_wnd(w);
    UNLOCK();
    w->flags |= WF_DESTROYED;
    free_wnd(w);
}

USERAPI BOOL DestroyWindow(HWND h)
{
    Wnd *w = W(h);
    if (!w || w == desktop()) return FALSE;
    if (cbt_hook(4 /* HCBT_DESTROYWND */, (WPARAM)h, 0)) return FALSE;
    if (w->flags & WF_DESTROYING) return TRUE;
    if (g_menu_owner == w) menu_cancel();
    /* windows it owns go first */
    if (!w->parent) {
        for (int i = 0; i < MAX_WND; i++) {
            Wnd *o = g_wnds[i];
            if (o && o->used && o->owner == w && !(o->flags & WF_DESTROYING)) DestroyWindow(o->h);
        }
    }
    int was_active = !w->parent && g_active == h;
    Wnd *owner = w->owner;
    if (wnd_visible(w)) {
        if (w->parent) {
            RECT r = w->rect;
            w->style &= ~WS_VISIBLE;
            invalidate(w->parent, &r, TRUE, 1);
        } else if (w->kid) {
            NtNovaGuiCtl(w->kid, CTL_SHOW, 0, NULL);
            w->style &= ~WS_VISIBLE;
        }
    }
    /* the focus leaves with it */
    Wnd *f = W_quiet(g_focus);
    if (f && (f == w || is_child_of(w, f))) {
        g_focus = 0;
        if (w->parent && wnd_visible(w->parent)) {}
    }
    if (w->parent && !(w->exstyle & WS_EX_NOPARENTNOTIFY)) send_msg(w->parent, WM_PARENTNOTIFY, MAKEWPARAM(WM_DESTROY, (WORD)w->id), (LPARAM)h);
    if (!W_quiet(h)) return TRUE;
    destroy_tree(w);
    if (was_active && owner && W_quiet(owner->h) && owner->kid) {
        g_active = 0;
        NtNovaGuiCtl(owner->kid, CTL_ACTIVATE, 0, NULL);
    }
    return TRUE;
}

void destroy_children(Wnd *w)
{
    while (w->child) {
        Wnd *c = w->child;
        if (c->flags & WF_DESTROYING) { unlink_wnd(c); continue; }
        DestroyWindow(c->h);
    }
}

/* -----------------------------------------------------------------------
 * Showing, hiding, enabling
 * ----------------------------------------------------------------------- */
USERAPI BOOL ShowWindow(HWND h, int cmd)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    BOOL was = (w->style & WS_VISIBLE) != 0;
    int show = cmd != SW_HIDE;
    if (cmd == SW_SHOWDEFAULT) cmd = SW_SHOWNORMAL;
    if (!w->parent) {
        if (w->flags & WF_MAPPED) { if (show) w->style |= WS_VISIBLE; else w->style &= ~WS_VISIBLE; return was; }
        if (show && !was) {
            send_msg(w, WM_SHOWWINDOW, TRUE, 0);
            if (!W_quiet(h)) return was;
        } else if (!show && was) {
            send_msg(w, WM_SHOWWINDOW, FALSE, 0);
            if (!W_quiet(h)) return was;
        }
        if (show && (w->flags & WF_NEED_SIZE)) {
            w->flags &= ~WF_NEED_SIZE;
            WPARAM how = (cmd == SW_SHOWMINIMIZED || cmd == SW_MINIMIZE || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE) ? SIZE_MINIMIZED :
                         cmd == SW_SHOWMAXIMIZED ? SIZE_MAXIMIZED : SIZE_RESTORED;
            send_msg(w, WM_SIZE, how, MAKELPARAM(w->client.right - w->client.left, w->client.bottom - w->client.top));
            if (!W_quiet(h)) return was;
            send_msg(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
            if (!W_quiet(h)) return was;
        }
        if (show) {
            w->style |= WS_VISIBLE;
            if (!kernel_window(w)) return was;
            int op;
            switch (cmd) {
            case SW_SHOWMINIMIZED: case SW_MINIMIZE: case SW_SHOWMINNOACTIVE: case SW_FORCEMINIMIZE: op = 2; break;
            case SW_SHOWMAXIMIZED: op = 3; break;
            case SW_RESTORE: op = 4; break;
            case SW_SHOWNA: case SW_SHOWNOACTIVATE: op = 5; break;
            default: op = (w->flags & WF_MENU_TRACK) ? 5 : 1;
            }
            if (!was || op != 1) {
                present(w);
                NtNovaGuiCtl(w->kid, CTL_SHOW, (ULONG_PTR)op, NULL);
                w->shown_kernel = 1;
                if (op == 2 || op == 3 || op == 4) top_sync_from_kernel(w, 1);
                if (op == 1 || op == 3 || op == 4) {
                    if (!(w->flags & WF_MENU_TRACK)) {
                        /* activate now: the desktop's WM_ACTIVATE follows */
                        HWND old = g_active;
                        if (old != h) {
                            g_active = h;
                            Wnd *o = W_quiet(old);
                            if (o && !o->parent) {
                                send_msg(o, WM_NCACTIVATE, FALSE, 0);
                                send_msg(o, WM_ACTIVATE, WA_INACTIVE, (LPARAM)h);
                            }
                            if (W_quiet(h)) {
                                send_msg(w, WM_NCACTIVATE, TRUE, 0);
                                send_msg(w, WM_ACTIVATE, WA_ACTIVE, (LPARAM)old);
                            }
                        }
                    }
                }
            }
            if (!was) invalidate(w, NULL, TRUE, 1), invalidate_nc(w);
        } else if (was) {
            w->style &= ~WS_VISIBLE;
            if (w->kid) NtNovaGuiCtl(w->kid, CTL_SHOW, 0, NULL);
            w->shown_kernel = 0;
            Wnd *f = W_quiet(g_focus);
            if (f && (f == w || is_child_of(w, f))) g_focus = 0;
            if (g_active == h) {
                g_active = 0;
                send_msg(w, WM_NCACTIVATE, FALSE, 0);
                send_msg(w, WM_ACTIVATE, WA_INACTIVE, 0);
                if (w->owner && W_quiet(w->owner->h) && w->owner->kid && (w->owner->style & WS_VISIBLE))
                    NtNovaGuiCtl(w->owner->kid, CTL_ACTIVATE, 0, NULL);
            }
        }
        return was;
    }
    /* child windows */
    if (show == (int)was) return was;
    send_msg(w, WM_SHOWWINDOW, show, 0);
    if (!W_quiet(h)) return was;
    wnd_set_pos(w, 0, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (!show) {
        Wnd *f = W_quiet(g_focus);
        if (f && (f == w || is_child_of(w, f))) set_focus(w->parent ? w->parent->h : 0);
    }
    return was;
}

USERAPI BOOL ShowWindowAsync(HWND h, int cmd) { return ShowWindow(h, cmd); }
USERAPI BOOL ShowOwnedPopups(HWND h, BOOL show) { (void)h; (void)show; return TRUE; }

USERAPI BOOL IsWindowVisible(HWND h)
{
    Wnd *w = W_quiet(h);
    INT32 f[11];
    if (!w) return foreign_info(h, f) == 2 && (f[2] & 1) && !(f[2] & 4);
    return wnd_visible(w);
}
USERAPI BOOL IsWindow(HWND h) { INT32 f[11]; return W_quiet(h) != NULL || foreign_info(h, f) != 0; }
USERAPI BOOL IsWindowEnabled(HWND h) { Wnd *w = W_quiet(h); return w && !(w->style & WS_DISABLED); }
USERAPI BOOL IsWindowUnicode(HWND h) { Wnd *w = W_quiet(h); return w && w->wide; }
USERAPI BOOL IsIconic(HWND h)
{
    Wnd *w = W_quiet(h);
    INT32 f[11];
    return w ? w->minimized : foreign_info(h, f) == 2 && (f[2] & 4);
}
USERAPI BOOL IsZoomed(HWND h)
{
    Wnd *w = W_quiet(h);
    INT32 f[11];
    return w ? w->maximized : foreign_info(h, f) == 2 && (f[2] & 8);
}

USERAPI BOOL EnableWindow(HWND h, BOOL on)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    BOOL was_disabled = (w->style & WS_DISABLED) != 0;
    if (on && was_disabled) {
        w->style &= ~WS_DISABLED;
        if (w->kid) NtNovaGuiCtl(w->kid, CTL_ENABLE, 1, NULL);
        send_msg(w, WM_ENABLE, TRUE, 0);
    } else if (!on && !was_disabled) {
        send_msg(w, WM_CANCELMODE, 0, 0);
        w->style |= WS_DISABLED;
        if (w->kid) NtNovaGuiCtl(w->kid, CTL_ENABLE, 0, NULL);
        Wnd *f = W_quiet(g_focus);
        if (f && (f == w || is_child_of(w, f))) set_focus(0);
        if (g_capture == h) ReleaseCapture();
        send_msg(w, WM_ENABLE, FALSE, 0);
    }
    return was_disabled;
}

/* -----------------------------------------------------------------------
 * Focus and activation
 * ----------------------------------------------------------------------- */
HWND set_focus(HWND h)
{
    HWND old = g_focus;
    if (h == old) return old;
    Wnd *w = h ? W_quiet(h) : NULL;
    if (h && !w) return 0;
    if (w && (w->style & WS_DISABLED)) return 0;
    Wnd *o = W_quiet(old);
    g_focus = h;
    if (o) send_msg(o, WM_KILLFOCUS, (WPARAM)h, 0);
    if (g_focus != h) return old;                           /* it moved the focus itself */
    if (w) {
        Wnd *t = top_of(w);
        if (t && t->h != g_active && !(t->flags & WF_MENU_TRACK) && t->kid && (t->style & WS_VISIBLE)) {
            HWND prev = g_active;
            g_active = t->h;
            NtNovaGuiCtl(t->kid, CTL_ACTIVATE, 0, NULL);
            Wnd *pa = W_quiet(prev);
            if (pa) { send_msg(pa, WM_NCACTIVATE, FALSE, 0); send_msg(pa, WM_ACTIVATE, WA_INACTIVE, (LPARAM)t->h); }
            send_msg(t, WM_NCACTIVATE, TRUE, 0);
            send_msg(t, WM_ACTIVATE, WA_ACTIVE, (LPARAM)prev);
            if (g_focus != h) return old;
        }
        send_msg(w, WM_SETFOCUS, (WPARAM)old, 0);
    }
    return old;
}

USERAPI HWND SetFocus(HWND h)
{
    if (h && !W(h)) return 0;
    HWND old = g_focus;
    set_focus(h);
    return W_quiet(old) ? old : 0;
}

USERAPI HWND GetFocus(void) { return W_quiet(g_focus) ? g_focus : 0; }
USERAPI HWND GetActiveWindow(void) { return W_quiet(g_active) ? g_active : 0; }
USERAPI HWND GetForegroundWindow(void) { return W_quiet(g_active) ? g_active : 0; }

/* The desktop activated or deactivated a top-level window */
void top_activated(Wnd *w, int active)
{
    HWND h = w->h;
    if (active) {
        if (g_active == h) return;
        HWND prev = g_active;
        g_active = h;
        Wnd *p = W_quiet(prev);
        if (p && p != w) { send_msg(p, WM_NCACTIVATE, FALSE, 0); send_msg(p, WM_ACTIVATE, WA_INACTIVE, (LPARAM)h); }
        if (!W_quiet(h)) return;
        send_msg(w, WM_NCACTIVATE, TRUE, 0);
        send_msg(w, WM_ACTIVATE, WA_CLICKACTIVE, (LPARAM)prev);
    } else {
        if (g_active != h) return;
        Wnd *f = W_quiet(g_focus);
        if (f && (f == w || is_child_of(w, f))) w->focus_save = g_focus;
        send_msg(w, WM_NCACTIVATE, FALSE, 0);
        send_msg(w, WM_ACTIVATE, WA_INACTIVE, 0);
        if (g_active == h) g_active = 0;
        if (W_quiet(h) && f && (f == w || is_child_of(w, f))) {
            g_focus = 0;
            send_msg(f, WM_KILLFOCUS, 0, 0);
        }
    }
}

USERAPI BOOL SetForegroundWindow(HWND h)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    w = top_of(w);
    if (w->kid && (w->style & WS_VISIBLE)) NtNovaGuiCtl(w->kid, CTL_ACTIVATE, 0, NULL);
    top_activated(w, 1);
    return TRUE;
}

USERAPI HWND SetActiveWindow(HWND h)
{
    HWND old = GetActiveWindow();
    if (h) SetForegroundWindow(h);
    return old;
}

USERAPI BOOL BringWindowToTop(HWND h)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    if (w->parent) { wnd_set_pos(w, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); return TRUE; }
    return SetForegroundWindow(h);
}

USERAPI BOOL AllowSetForegroundWindow(DWORD pid) { (void)pid; return TRUE; }
USERAPI BOOL LockSetForegroundWindow(UINT code) { (void)code; return TRUE; }
USERAPI void SwitchToThisWindow(HWND h, BOOL alt) { (void)alt; SetForegroundWindow(h); }

/* -----------------------------------------------------------------------
 * The tree
 * ----------------------------------------------------------------------- */
USERAPI HWND GetDesktopWindow(void) { return desktop()->h; }
USERAPI HWND GetShellWindow(void) { return 0; }

USERAPI HWND GetParent(HWND h)
{
    Wnd *w = W(h);
    if (!w) return 0;
    if (w->style & WS_CHILD) return w->parent ? w->parent->h : 0;
    if (w->style & WS_POPUP) return w->owner ? w->owner->h : 0;
    return 0;
}

USERAPI HWND GetAncestor(HWND h, UINT flags)
{
    Wnd *w = W(h);
    if (!w || w == desktop()) return 0;
    switch (flags) {
    case GA_PARENT: return w->parent ? w->parent->h : desktop()->h;
    case GA_ROOT: return top_of(w)->h;
    case GA_ROOTOWNER: {
        Wnd *t = top_of(w);
        while (t->owner) t = top_of(t->owner);
        return t->h;
    }
    }
    return 0;
}

USERAPI BOOL IsChild(HWND hp, HWND hc)
{
    Wnd *p = W_quiet(hp), *c = W_quiet(hc);
    if (!p || !c) return FALSE;
    for (Wnd *a = c; a && (a->style & WS_CHILD); a = a->parent) if (a->parent == p) return TRUE;
    return FALSE;
}

USERAPI HWND GetWindow(HWND h, UINT cmd)
{
    Wnd *w = W(h);
    if (!w) return 0;
    Wnd *r = NULL;
    switch (cmd) {
    case GW_HWNDFIRST: r = container(w)->child; break;
    case GW_HWNDLAST: for (r = container(w)->child; r && r->next; r = r->next) ; break;
    case GW_HWNDNEXT: r = w->next; break;
    case GW_HWNDPREV: r = w->prev; break;
    case GW_OWNER: r = w->parent ? NULL : w->owner; break;
    case GW_CHILD: r = w->child; break;
    case GW_ENABLEDPOPUP: return 0;
    }
    return r ? r->h : 0;
}

USERAPI HWND GetTopWindow(HWND h)
{
    Wnd *w = h ? W(h) : desktop();
    return w && w->child ? w->child->h : 0;
}

USERAPI HWND GetNextWindow(HWND h, UINT cmd) { return GetWindow(h, cmd); }
USERAPI HWND GetLastActivePopup(HWND h) { return h; }

USERAPI HWND SetParent(HWND h, HWND hp)
{
    Wnd *w = W(h);
    if (!w || w == desktop()) return 0;
    Wnd *np = hp ? W(hp) : NULL;
    if (hp && !np) return 0;
    if (np == desktop()) np = NULL;
    HWND old = w->parent ? w->parent->h : desktop()->h;
    int vis = (w->style & WS_VISIBLE) != 0;
    if (vis) ShowWindow(h, SW_HIDE);
    POINT o;
    wnd_screen_origin(w, 0, &o);
    int cw = w->rect.right - w->rect.left, ch = w->rect.bottom - w->rect.top;
    LOCK();
    unlink_wnd(w);
    Wnd *oldp = w->parent;
    w->parent = np;
    link_wnd(w, NULL);
    UNLOCK();
    if (!np) {
        w->style &= ~WS_CHILD;
        w->style |= WS_POPUP;
        SetRect(&w->rect, o.x, o.y, o.x + cw, o.y + ch);
    } else {
        if (!oldp && w->kid) { NtNovaGuiDestroy(w->kid); w->kid = 0; }
        POINT po;
        wnd_screen_origin(np, 1, &po);
        SetRect(&w->rect, o.x - po.x, o.y - po.y, o.x - po.x + cw, o.y - po.y + ch);
    }
    wnd_calc_client(w);
    if (vis) ShowWindow(h, SW_SHOWNA);
    return old;
}

typedef BOOL (CALLBACK *ENUMPROC_)(HWND, LPARAM);

static BOOL enum_children(Wnd *p, WNDENUMPROC fn, LPARAM lp)
{
    /* snapshot, so the callback may destroy windows */
    HWND list[512];
    int n = 0;
    for (Wnd *c = p->child; c && n < 512; c = c->next) list[n++] = c->h;
    for (int i = 0; i < n; i++) {
        Wnd *c = W_quiet(list[i]);
        if (!c) continue;
        if (!fn(list[i], lp)) return FALSE;
        c = W_quiet(list[i]);
        if (c && c->child && !enum_children(c, fn, lp)) return FALSE;
    }
    return TRUE;
}

USERAPI BOOL EnumChildWindows(HWND hp, WNDENUMPROC fn, LPARAM lp)
{
    Wnd *p = hp ? W(hp) : desktop();
    if (!p) return FALSE;
    enum_children(p, fn, lp);
    return TRUE;
}

USERAPI BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp)
{
    HWND list[512];
    int n = 0;
    for (Wnd *c = desktop()->child; c && n < 512; c = c->next) list[n++] = c->h;
    for (int i = 0; i < n; i++) if (W_quiet(list[i]) && !fn(list[i], lp)) break;
    return TRUE;
}

USERAPI BOOL EnumThreadWindows(DWORD tid, WNDENUMPROC fn, LPARAM lp)
{
    HWND list[512];
    int n = 0;
    for (Wnd *c = desktop()->child; c && n < 512; c = c->next) if (c->tid == tid) list[n++] = c->h;
    for (int i = 0; i < n; i++) if (W_quiet(list[i]) && !fn(list[i], lp)) break;
    return TRUE;
}

USERAPI BOOL EnumDesktopWindows(HANDLE desk, WNDENUMPROC fn, LPARAM lp) { (void)desk; return EnumWindows(fn, lp); }

static int text_matches(Wnd *w, LPCWSTR title) { return !title || !wcscmp(w->text ? w->text : L"", title); }
static int class_matches(Wnd *w, LPCWSTR cls)
{
    if (!cls) return 1;
    if ((ULONG_PTR)cls < 0x10000) return w->cls && w->cls->atom == (ATOM)(ULONG_PTR)cls;
    return w->cls && !wcsicmp_(w->cls->name, cls);
}

USERAPI HWND FindWindowExW(HWND hp, HWND after, LPCWSTR cls, LPCWSTR title)
{
    Wnd *p = hp && hp != HWND_MESSAGE ? W(hp) : desktop();
    if (!p) return 0;
    Wnd *c = after ? W_quiet(after) : NULL;
    c = c ? c->next : p->child;
    for (; c; c = c->next) if (class_matches(c, cls) && text_matches(c, title)) return c->h;
    return 0;
}

USERAPI HWND FindWindowW(LPCWSTR cls, LPCWSTR title) { return FindWindowExW(0, 0, cls, title); }

USERAPI HWND FindWindowExA(HWND hp, HWND after, LPCSTR cls, LPCSTR title)
{
    WCHAR *wc = (ULONG_PTR)cls < 0x10000 ? (WCHAR *)cls : a2w(cls, -1);
    WCHAR *wt = title ? a2w(title, -1) : NULL;
    HWND r = FindWindowExW(hp, after, wc, wt);
    if ((ULONG_PTR)cls >= 0x10000) free(wc);
    free(wt);
    return r;
}

USERAPI HWND FindWindowA(LPCSTR cls, LPCSTR title) { return FindWindowExA(0, 0, cls, title); }

USERAPI DWORD GetWindowThreadProcessId(HWND h, LPDWORD pid)
{
    Wnd *w = W_quiet(h);
    if (!w) {
        INT32 f[11];
        int k = foreign_info(h, f);
        if (pid) *pid = k ? (DWORD)f[0] : 0;
        if (!k) SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return k == 2 ? (DWORD)f[1] : 0;                 /* a child window's thread is not known */
    }
    if (pid) *pid = GetCurrentProcessId();
    return w == desktop() ? g_main_tid : w->tid;
}

/* -----------------------------------------------------------------------
 * Rectangles and coordinates
 * ----------------------------------------------------------------------- */
/* another process's desktop window: its rectangles from the kernel */
static BOOL foreign_rect(HWND h, int client, LPRECT r)
{
    INT32 f[11];
    if (!r || foreign_info(h, f) != 2) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if (client) SetRect(r, f[3], f[4], f[3] + f[5], f[4] + f[6]);
    else SetRect(r, f[7], f[8], f[7] + f[9], f[8] + f[10]);
    return TRUE;
}

USERAPI BOOL GetWindowRect(HWND h, LPRECT r)
{
    Wnd *w = W_quiet(h);
    if (!w) return foreign_rect(h, 0, r);
    if (!r) return FALSE;
    POINT o;
    wnd_screen_origin(w, 0, &o);
    SetRect(r, o.x, o.y, o.x + w->rect.right - w->rect.left, o.y + w->rect.bottom - w->rect.top);
    return TRUE;
}

USERAPI BOOL GetClientRect(HWND h, LPRECT r)
{
    Wnd *w = W_quiet(h);
    if (!w) {
        if (!foreign_rect(h, 1, r)) return FALSE;
        OffsetRect(r, -r->left, -r->top);
        return TRUE;
    }
    if (!r) return FALSE;
    SetRect(r, 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top);
    return TRUE;
}

USERAPI BOOL ClientToScreen(HWND h, LPPOINT p)
{
    Wnd *w = W_quiet(h);
    RECT fr;
    if (!w) {
        if (!foreign_rect(h, 1, &fr)) return FALSE;
        p->x += fr.left; p->y += fr.top;
        return TRUE;
    }
    POINT o;
    wnd_screen_origin(w, 1, &o);
    p->x += o.x; p->y += o.y;
    return TRUE;
}

USERAPI BOOL ScreenToClient(HWND h, LPPOINT p)
{
    Wnd *w = W_quiet(h);
    RECT fr;
    if (!w) {
        if (!foreign_rect(h, 1, &fr)) return FALSE;
        p->x -= fr.left; p->y -= fr.top;
        return TRUE;
    }
    POINT o;
    wnd_screen_origin(w, 1, &o);
    p->x -= o.x; p->y -= o.y;
    return TRUE;
}

USERAPI int MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n)
{
    POINT a = { 0, 0 }, b = { 0, 0 };
    Wnd *f = from ? W_quiet(from) : NULL, *t = to ? W_quiet(to) : NULL;
    if (f && f != desktop()) wnd_screen_origin(f, 1, &a);
    if (t && t != desktop()) wnd_screen_origin(t, 1, &b);
    int dx = a.x - b.x, dy = a.y - b.y;
    for (UINT i = 0; i < n; i++) { p[i].x += dx; p[i].y += dy; }
    SetLastError(0);
    return MAKELONG((WORD)(SHORT)dx, (WORD)(SHORT)dy);
}

USERAPI BOOL GetWindowInfo(HWND h, PWINDOWINFO wi)
{
    Wnd *w = W(h);
    if (!w || !wi) return FALSE;
    GetWindowRect(h, &wi->rcWindow);
    POINT o;
    wnd_screen_origin(w, 1, &o);
    SetRect(&wi->rcClient, o.x, o.y, o.x + w->client.right - w->client.left, o.y + w->client.bottom - w->client.top);
    wi->dwStyle = w->style; wi->dwExStyle = w->exstyle;
    wi->dwWindowStatus = g_active == h ? 1 : 0;
    wi->cxWindowBorders = wi->cyWindowBorders = (UINT)border_width(w);
    wi->atomWindowType = w->cls ? w->cls->atom : 0;
    wi->wCreatorVersion = 0x0A00;
    return TRUE;
}

USERAPI BOOL AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex)
{
    Wnd tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.style = style; tmp.exstyle = ex;
    if (style & WS_CHILD) tmp.parent = desktop();          /* only "is it a child" matters */
    RECT k = { 0, 0, 0, 0 };
    if ((style & WS_CAPTION) == WS_CAPTION || (!(style & WS_POPUP) && !(style & WS_CHILD))) {
        if (!(style & WS_CHILD)) SetRect(&k, FRAME_BORDER, FRAME_TITLE, FRAME_BORDER, FRAME_BORDER);
    }
    int b = 0;
    if (!k.top) {
        if (style & WS_THICKFRAME) b += (style & WS_CHILD) ? 3 : 1;
        else if ((style & WS_CAPTION) == WS_DLGFRAME || (ex & WS_EX_DLGMODALFRAME)) b += (style & WS_CHILD) ? 3 : 1;
        else if (style & WS_BORDER) b += 1;
    }
    if (ex & WS_EX_CLIENTEDGE) b += 2;
    if (ex & WS_EX_STATICEDGE) b += 1;
    r->left -= k.left + b; r->top -= k.top + b; r->right += k.right + b; r->bottom += k.bottom + b;
    if (menu) r->top -= GetSystemMetrics(SM_CYMENU);
    return TRUE;
}

USERAPI BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) { return AdjustWindowRectEx(r, style, menu, 0); }
USERAPI BOOL AdjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD ex, UINT dpi) { (void)dpi; return AdjustWindowRectEx(r, style, menu, ex); }

USERAPI BOOL SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    if (!w->parent && !(flags & SWP_NOMOVE)) {
        /* x, y are screen coordinates */
    }
    wnd_set_pos(w, after, x, y, cx, cy, flags);
    return TRUE;
}

USERAPI BOOL MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint)
{
    return SetWindowPos(h, 0, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));
}

typedef struct { int n; struct { HWND h, after; int x, y, cx, cy; UINT f; } e[64]; } DeferPos;
USERAPI HANDLE BeginDeferWindowPos(int n) { (void)n; return calloc(1, sizeof(DeferPos)); }
USERAPI HANDLE DeferWindowPos(HANDLE hd, HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    DeferPos *d = hd;
    if (!d) return 0;
    if (d->n < 64) { d->e[d->n].h = h; d->e[d->n].after = after; d->e[d->n].x = x; d->e[d->n].y = y; d->e[d->n].cx = cx; d->e[d->n].cy = cy; d->e[d->n].f = f; d->n++; }
    else SetWindowPos(h, after, x, y, cx, cy, f);
    return hd;
}
USERAPI BOOL EndDeferWindowPos(HANDLE hd)
{
    DeferPos *d = hd;
    if (!d) return FALSE;
    for (int i = 0; i < d->n; i++) SetWindowPos(d->e[i].h, d->e[i].after, d->e[i].x, d->e[i].y, d->e[i].cx, d->e[i].cy, d->e[i].f);
    free(d);
    return TRUE;
}

USERAPI BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *p)
{
    Wnd *w = W(h);
    if (!w || !p) return FALSE;
    p->flags = 0;
    p->showCmd = w->minimized ? SW_SHOWMINIMIZED : w->maximized ? SW_SHOWMAXIMIZED : (w->style & WS_VISIBLE) ? SW_SHOWNORMAL : SW_HIDE;
    p->ptMinPosition.x = p->ptMinPosition.y = -1;
    p->ptMaxPosition.x = p->ptMaxPosition.y = -1;
    if (w->maximized && !IsRectEmpty(&w->normal)) p->rcNormalPosition = w->normal;
    else {
        GetWindowRect(h, &p->rcNormalPosition);
        if ((w->style & WS_CHILD) && w->parent) MapWindowPoints(NULL, w->parent->h, (POINT *)&p->rcNormalPosition, 2);   /* a child's: in its parent's client area */
    }
    return TRUE;
}

USERAPI BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *p)
{
    Wnd *w = W(h);
    if (!w || !p) return FALSE;
    const RECT *r = &p->rcNormalPosition;
    if (r->right > r->left && r->bottom > r->top) {
        if (w->maximized || w->minimized) w->normal = *r;       /* where a restore goes */
        else SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!W_quiet(h)) return FALSE;
    ShowWindow(h, (int)p->showCmd);                         /* shows a hidden window too, as on Windows */
    return TRUE;
}

USERAPI BOOL CloseWindow(HWND h) { return ShowWindow(h, SW_MINIMIZE), TRUE; }
USERAPI BOOL OpenIcon(HWND h) { return ShowWindow(h, SW_RESTORE), TRUE; }

/* Child windows at a point */
static Wnd *child_at(Wnd *p, POINT pt, UINT skip)           /* pt: p's client coordinates */
{
    for (Wnd *c = p->child; c; c = c->next) {
        if ((skip & CWP_SKIPINVISIBLE) && !(c->style & WS_VISIBLE)) continue;
        if ((skip & CWP_SKIPDISABLED) && (c->style & WS_DISABLED)) continue;
        if ((skip & CWP_SKIPTRANSPARENT) && (c->exstyle & WS_EX_TRANSPARENT)) continue;
        if (PtInRect(&c->rect, pt)) return c;
    }
    return NULL;
}

USERAPI HWND ChildWindowFromPointEx(HWND h, POINT pt, UINT flags)
{
    Wnd *w = W(h);
    if (!w) return 0;
    RECT cr = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    if (!PtInRect(&cr, pt)) return 0;
    Wnd *c = child_at(w, pt, flags);
    return c ? c->h : h;
}

USERAPI HWND ChildWindowFromPoint(HWND h, POINT pt) { return ChildWindowFromPointEx(h, pt, 0); }

USERAPI HWND RealChildWindowFromPoint(HWND h, POINT pt) { return ChildWindowFromPointEx(h, pt, CWP_SKIPINVISIBLE); }

/* The deepest visible window at a screen point */
Wnd *window_at(POINT pt)
{
    Wnd *t = NULL;
    for (Wnd *c = desktop()->child; c; c = c->next) {
        if (!(c->style & WS_VISIBLE) || !c->kid || c->minimized) continue;
        RECT b = { c->bmp.x, c->bmp.y, c->bmp.x + c->bw, c->bmp.y + c->bh };
        if (PtInRect(&b, pt)) { t = c; break; }
    }
    if (!t) return NULL;
    Wnd *w = t;
    for (;;) {
        POINT o;
        wnd_screen_origin(w, 1, &o);
        POINT lp = { pt.x - o.x, pt.y - o.y };
        RECT cr = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
        if (!PtInRect(&cr, lp)) break;
        Wnd *c = child_at(w, lp, CWP_SKIPINVISIBLE);
        if (!c) break;
        w = c;
    }
    return w;
}

USERAPI HWND WindowFromPoint(POINT pt)
{
    Wnd *w = window_at(pt);
    return w ? w->h : 0;
}

USERAPI HWND WindowFromPhysicalPoint(POINT pt) { return WindowFromPoint(pt); }

/* -----------------------------------------------------------------------
 * Text
 * ----------------------------------------------------------------------- */
void set_text(Wnd *w, const WCHAR *s)
{
    WCHAR *n = wstrdup(s ? s : L"");
    free(w->text);
    w->text = n;
    if (!w->parent && w->kid) NtNovaGuiSetText(w->kid, w->text);
}

USERAPI BOOL SetWindowTextW(HWND h, LPCWSTR s)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    return (BOOL)SendMessageW(h, WM_SETTEXT, 0, (LPARAM)s);
}

USERAPI BOOL SetWindowTextA(HWND h, LPCSTR s)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    return (BOOL)SendMessageA(h, WM_SETTEXT, 0, (LPARAM)s);
}

USERAPI int GetWindowTextW(HWND h, LPWSTR s, int max)
{
    Wnd *w = W(h);
    if (!w || !s || max <= 0) return 0;
    s[0] = 0;
    if (w->tid != GetCurrentThreadId() && !w->ctl) {        /* another thread's window: no message */
        int n = MIN(wlen(w->text), max - 1);
        memcpy(s, w->text, 2 * (size_t)n);
        s[n] = 0;
        return n;
    }
    return (int)SendMessageW(h, WM_GETTEXT, (WPARAM)max, (LPARAM)s);
}

USERAPI int GetWindowTextA(HWND h, LPSTR s, int max)
{
    Wnd *w = W(h);
    if (!w || !s || max <= 0) return 0;
    s[0] = 0;
    return (int)SendMessageA(h, WM_GETTEXT, (WPARAM)max, (LPARAM)s);
}

USERAPI int GetWindowTextLengthW(HWND h) { return W(h) ? (int)SendMessageW(h, WM_GETTEXTLENGTH, 0, 0) : 0; }
USERAPI int GetWindowTextLengthA(HWND h) { return W(h) ? (int)SendMessageA(h, WM_GETTEXTLENGTH, 0, 0) : 0; }
USERAPI int InternalGetWindowText(HWND h, LPWSTR s, int max)
{
    Wnd *w = W(h);
    if (!w || max <= 0) return 0;
    int n = MIN(wlen(w->text), max - 1);
    memcpy(s, w->text, 2 * (size_t)n);
    s[n] = 0;
    return n;
}

/* -----------------------------------------------------------------------
 * Window data
 * ----------------------------------------------------------------------- */
static LONG_PTR get_long(HWND h, int i, int size, int wide)
{
    Wnd *w = W(h);
    if (!w) return 0;
    switch (i) {
    case GWLP_WNDPROC:    (void)wide; return (LONG_PTR)w->proc;
    case GWLP_HINSTANCE:  return (LONG_PTR)w->inst;
    case GWLP_HWNDPARENT: return (LONG_PTR)(w->parent ? w->parent->h : w->owner ? w->owner->h : 0);
    case GWLP_ID:         return w->id;
    case GWL_STYLE:       return (LONG)w->style;
    case GWL_EXSTYLE:     return (LONG)w->exstyle;
    case GWLP_USERDATA:   return w->userdata;
    }
    int extra = w->cls ? w->cls->extra : 0;
    if (i >= 0 && i + size <= extra) {
        LONG_PTR v = 0;
        memcpy(&v, w->extra + i, (size_t)size);
        return size == 4 ? (LONG_PTR)(LONG)v : v;
    }
    SetLastError(1413 /* ERROR_INVALID_INDEX */);
    return 0;
}

static LONG_PTR set_long(HWND h, int i, LONG_PTR v, int size, int wide)
{
    Wnd *w = W(h);
    if (!w) return 0;
    LONG_PTR old = get_long(h, i, size, wide);
    switch (i) {
    case GWLP_WNDPROC:
        w->proc = (WNDPROC)v;
        w->wide = wide;
        return old;
    case GWLP_HINSTANCE: w->inst = (HINSTANCE)v; return old;
    case GWLP_HWNDPARENT:
        if (w->parent) return (LONG_PTR)SetParent(h, (HWND)v);
        w->owner = v ? W_quiet((HWND)v) : NULL;
        if (w->owner) w->owner = top_of(w->owner);
        return old;
    case GWLP_ID: w->id = v; return old;
    case GWL_STYLE: case GWL_EXSTYLE: {
        STYLESTRUCT ss = { (DWORD)old, (DWORD)v };
        send_msg(w, WM_STYLECHANGING, (WPARAM)i, (LPARAM)&ss);
        if (!W_quiet(h)) return old;
        DWORD nv = ss.styleNew;
        if (i == GWL_STYLE) {
            int vis_change = (nv ^ w->style) & WS_VISIBLE;
            w->style = (nv & ~WS_VISIBLE) | (w->style & WS_VISIBLE);
            if (vis_change) {
                if (w->parent) { w->style ^= WS_VISIBLE; invalidate(w->parent, &w->rect, TRUE, 1); }
                else ShowWindow(h, (nv & WS_VISIBLE) ? SW_SHOWNA : SW_HIDE);
            }
            if ((nv ^ (DWORD)old) & WS_DISABLED) {
                if (w->kid) NtNovaGuiCtl(w->kid, CTL_ENABLE, (nv & WS_DISABLED) ? 0 : 1, NULL);
            }
        } else w->exstyle = nv;
        send_msg(w, WM_STYLECHANGED, (WPARAM)i, (LPARAM)&ss);
        if (w->parent && wnd_visible(w)) invalidate(w, NULL, TRUE, 0);
        return old;
    }
    case GWLP_USERDATA: w->userdata = v; return old;
    }
    int extra = w->cls ? w->cls->extra : 0;
    if (i >= 0 && i + size <= extra) { memcpy(w->extra + i, &v, (size_t)size); return old; }
    SetLastError(1413);
    return 0;
}

USERAPI LONG_PTR GetWindowLongPtrW(HWND h, int i) { return get_long(h, i, (int)sizeof(LONG_PTR), 1); }
USERAPI LONG_PTR GetWindowLongPtrA(HWND h, int i) { return get_long(h, i, (int)sizeof(LONG_PTR), 0); }
USERAPI LONG_PTR SetWindowLongPtrW(HWND h, int i, LONG_PTR v) { return set_long(h, i, v, (int)sizeof(LONG_PTR), 1); }
USERAPI LONG_PTR SetWindowLongPtrA(HWND h, int i, LONG_PTR v) { return set_long(h, i, v, (int)sizeof(LONG_PTR), 0); }
USERAPI LONG GetWindowLongW(HWND h, int i) { return (LONG)get_long(h, i, 4, 1); }
USERAPI LONG GetWindowLongA(HWND h, int i) { return (LONG)get_long(h, i, 4, 0); }
USERAPI LONG SetWindowLongW(HWND h, int i, LONG v) { return (LONG)set_long(h, i, (LONG_PTR)v, 4, 1); }
USERAPI LONG SetWindowLongA(HWND h, int i, LONG v) { return (LONG)set_long(h, i, (LONG_PTR)v, 4, 0); }
USERAPI WORD GetWindowWord(HWND h, int i) { return (WORD)get_long(h, i, 2, 1); }
USERAPI WORD SetWindowWord(HWND h, int i, WORD v) { return (WORD)set_long(h, i, v, 2, 1); }

USERAPI int GetDlgCtrlID(HWND h) { Wnd *w = W(h); return w && w->parent ? (int)w->id : 0; }
USERAPI int SetDlgCtrlID(HWND h, int id) { Wnd *w = W(h); if (!w) return 0; int o = (int)w->id; w->id = id; return o; }

/* -----------------------------------------------------------------------
 * Properties
 * ----------------------------------------------------------------------- */
static Prop *find_prop(Wnd *w, LPCWSTR name)
{
    for (Prop *p = w->props; p; p = p->next) {
        if ((ULONG_PTR)name < 0x10000) { if (p->atom == (ATOM)(ULONG_PTR)name) return p; }
        else if (p->name && !wcsicmp_(p->name, name)) return p;
        else if (!p->name && p->atom) {
            WCHAR buf[64];
            if (GlobalGetAtomNameW(p->atom, buf, 64) && !wcsicmp_(buf, name)) return p;
        }
    }
    return NULL;
}

USERAPI BOOL SetPropW(HWND h, LPCWSTR name, HANDLE data)
{
    Wnd *w = W(h);
    if (!w || !name) return FALSE;
    Prop *p = find_prop(w, name);
    if (!p) {
        p = calloc(1, sizeof(Prop));
        if (!p) return FALSE;
        if ((ULONG_PTR)name < 0x10000) p->atom = (ATOM)(ULONG_PTR)name;
        else p->name = wstrdup(name);
        p->next = w->props;
        w->props = p;
    }
    p->data = data;
    return TRUE;
}

USERAPI HANDLE GetPropW(HWND h, LPCWSTR name)
{
    Wnd *w = W_quiet(h);
    if (!w || !name) return 0;
    Prop *p = find_prop(w, name);
    return p ? p->data : 0;
}

USERAPI HANDLE RemovePropW(HWND h, LPCWSTR name)
{
    Wnd *w = W_quiet(h);
    if (!w || !name) return 0;
    for (Prop **pp = &w->props; *pp; pp = &(*pp)->next) {
        Prop *p = *pp;
        if (p != find_prop(w, name)) continue;
        HANDLE d = p->data;
        *pp = p->next;
        free(p->name);
        free(p);
        return d;
    }
    return 0;
}

USERAPI BOOL SetPropA(HWND h, LPCSTR name, HANDLE data)
{
    if ((ULONG_PTR)name < 0x10000) return SetPropW(h, (LPCWSTR)name, data);
    WCHAR *w = a2w(name, -1);
    BOOL r = SetPropW(h, w, data);
    free(w);
    return r;
}

USERAPI HANDLE GetPropA(HWND h, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return GetPropW(h, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HANDLE r = GetPropW(h, w);
    free(w);
    return r;
}

USERAPI HANDLE RemovePropA(HWND h, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return RemovePropW(h, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HANDLE r = RemovePropW(h, w);
    free(w);
    return r;
}

typedef BOOL (CALLBACK *PROPENUMPROCEXW_)(HWND, LPWSTR, HANDLE, ULONG_PTR);
USERAPI int EnumPropsExW(HWND h, PROPENUMPROCEXW_ fn, LPARAM lp)
{
    Wnd *w = W(h);
    if (!w) return -1;
    int r = -1;
    for (Prop *p = w->props; p; p = p->next) {
        r = fn(h, p->name ? p->name : (LPWSTR)(ULONG_PTR)p->atom, p->data, (ULONG_PTR)lp);
        if (!r) break;
    }
    return r;
}

/* -----------------------------------------------------------------------
 * Odds and ends
 * ----------------------------------------------------------------------- */
USERAPI BOOL FlashWindow(HWND h, BOOL invert) { (void)invert; return W_quiet(h) != NULL; }
USERAPI BOOL FlashWindowEx(PFLASHWINFO fi) { (void)fi; return TRUE; }
USERAPI BOOL IsHungAppWindow(HWND h) { (void)h; return FALSE; }
USERAPI BOOL SetLayeredWindowAttributes(HWND h, COLORREF key, BYTE alpha, DWORD f) { (void)key; (void)alpha; (void)f; return W_quiet(h) != NULL; }
USERAPI BOOL GetLayeredWindowAttributes(HWND h, COLORREF *key, BYTE *alpha, DWORD *f)
{ if (key) *key = 0; if (alpha) *alpha = 255; if (f) *f = 2; return W_quiet(h) != NULL; }
USERAPI BOOL UpdateLayeredWindow(HWND h, HDC d, POINT *p, SIZE *s, HDC src, POINT *sp, COLORREF k, void *bf, DWORD f)
{ (void)d; (void)p; (void)s; (void)src; (void)sp; (void)k; (void)bf; (void)f; return W_quiet(h) != NULL; }
USERAPI BOOL SetWindowDisplayAffinity(HWND h, DWORD a) { (void)a; return W_quiet(h) != NULL; }
USERAPI BOOL GetWindowDisplayAffinity(HWND h, DWORD *a) { if (a) *a = 0; return W_quiet(h) != NULL; }
USERAPI HWND GetProgmanWindow(void) { return 0; }
USERAPI BOOL SetWindowContextHelpId(HWND h, DWORD id) { (void)h; (void)id; return TRUE; }
USERAPI DWORD GetWindowContextHelpId(HWND h) { (void)h; return 0; }
USERAPI BOOL IsWindowArranged(HWND h) { (void)h; return FALSE; }
USERAPI UINT ArrangeIconicWindows(HWND h) { (void)h; return 0; }
USERAPI BOOL LogicalToPhysicalPoint(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
USERAPI BOOL PhysicalToLogicalPoint(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
USERAPI BOOL LogicalToPhysicalPointForPerMonitorDPI(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
USERAPI BOOL PhysicalToLogicalPointForPerMonitorDPI(HWND h, LPPOINT p) { (void)h; (void)p; return TRUE; }
USERAPI BOOL GetTitleBarInfo(HWND h, void *ti) { (void)h; (void)ti; return FALSE; }
USERAPI BOOL DragDetect(HWND h, POINT pt) { (void)h; (void)pt; return FALSE; }
USERAPI HWND GetTopLevelWindow_(HWND h) { return GetAncestor(h, GA_ROOT); }
USERAPI BOOL AnimateWindow(HWND h, DWORD t, DWORD f)
{
    (void)t;
    return ShowWindow(h, (f & 0x10000 /* AW_HIDE */) ? SW_HIDE : (f & 0x20000 /* AW_ACTIVATE */) ? SW_SHOW : SW_SHOWNA), TRUE;
}
USERAPI HWND GetWindowRgnBox_(HWND h) { return h; }
USERAPI int SetWindowRgn(HWND h, HRGN r, BOOL redraw) { (void)r; (void)redraw; return W_quiet(h) != NULL; }
USERAPI int GetWindowRgn(HWND h, HRGN r) { (void)h; (void)r; return 0; /* ERROR: no region */ }
USERAPI int GetWindowRgnBox(HWND h, LPRECT r) { (void)h; (void)r; return 0; }

/* -----------------------------------------------------------------------
 * Live object handles: an open-addressing set of pointers
 * ----------------------------------------------------------------------- */
static void **g_hset;
static int g_hcap, g_hcount, g_hused;

static unsigned hslot(const void *p, int cap) { unsigned long long v = (ULONG_PTR)p; v ^= v >> 17; v *= 0x9E3779B97F4A7C15ull; return (unsigned)(v >> 32) & (unsigned)(cap - 1); }

static void hset_put(void **set, int cap, void *p)
{
    unsigned i = hslot(p, cap);
    while (set[i] && set[i] != (void *)1) i = (i + 1) & (unsigned)(cap - 1);
    set[i] = p;
}

void handle_add(void *p)
{
    LOCK();
    if ((g_hused + 1) * 2 > g_hcap) {                       /* grow (and drop the tombstones) */
        int nc = g_hcap ? g_hcap : 1024;
        while ((g_hcount + 1) * 3 > nc) nc *= 2;
        void **n = calloc((size_t)nc, sizeof(void *));
        if (!n) { UNLOCK(); return; }
        for (int i = 0; i < g_hcap; i++) if (g_hset[i] && g_hset[i] != (void *)1) hset_put(n, nc, g_hset[i]);
        free(g_hset);
        g_hset = n; g_hcap = nc; g_hused = g_hcount;
    }
    hset_put(g_hset, g_hcap, p);
    g_hcount++; g_hused++;
    UNLOCK();
}

static int hfind(const void *p)
{
    if (!g_hcap) return -1;
    unsigned i = hslot(p, g_hcap);
    for (int k = 0; k < g_hcap; k++, i = (i + 1) & (unsigned)(g_hcap - 1)) {
        if (!g_hset[i]) return -1;
        if (g_hset[i] == p) return (int)i;
    }
    return -1;
}

void handle_remove(void *p)
{
    LOCK();
    int i = hfind(p);
    if (i >= 0) { g_hset[i] = (void *)1; g_hcount--; }
    UNLOCK();
}

int handle_live(const void *p)
{
    LOCK();
    int r = hfind(p) >= 0;
    UNLOCK();
    return r;
}
