/*
 * uxtheme.dll — visual styles.  NovaOS draws its controls in one built-in
 * look and has no theme files, so the classic path is what programs take:
 * no theme is active, OpenThemeData finds no theme data, and drawing with
 * a theme fails, which sends programs to their own (GDI) drawing.
 *
 * Buffered painting is real: BeginBufferedPaint hands out a memory DC the
 * size of the target rectangle and EndBufferedPaint copies it to the
 * target, as programs that paint without flicker expect.  Buffered
 * animations never run (programs then paint the final state).
 */
#include <windows.h>

#define UXAPI __declspec(dllexport)
#define E_NOTIMPL_   ((HRESULT)0x80004001L)
#define E_HANDLE_    ((HRESULT)0x80070006L)
#define E_INVALIDARG_ ((HRESULT)0x80070057L)
#define ERROR_NOT_FOUND_ 1168

typedef HANDLE HTHEME;

#define PRF_CLIENT_ 4
#define OBJ_FONT_   6
BOOL WINAPI OffsetViewportOrgEx(HDC h, int x, int y, LPPOINT old);

UXAPI BOOL WINAPI IsThemeActive(void) { return FALSE; }
UXAPI BOOL WINAPI IsAppThemed(void) { return FALSE; }
UXAPI BOOL WINAPI IsCompositionActive(void) { return FALSE; }
UXAPI DWORD WINAPI GetThemeAppProperties(void) { return 0; }
UXAPI void WINAPI SetThemeAppProperties(DWORD flags) { (void)flags; }
UXAPI BOOL WINAPI IsThemePartDefined(HTHEME t, int part, int state) { (void)t; (void)part; (void)state; return FALSE; }
UXAPI BOOL WINAPI IsThemeDialogTextureEnabled(HWND w) { (void)w; return FALSE; }
UXAPI BOOL WINAPI IsThemeBackgroundPartiallyTransparent(HTHEME t, int part, int state)
{ (void)t; (void)part; (void)state; return FALSE; }

UXAPI HTHEME WINAPI OpenThemeData(HWND w, LPCWSTR classes)
{
    (void)w; (void)classes;
    SetLastError(ERROR_NOT_FOUND_);
    return 0;
}
UXAPI HTHEME WINAPI OpenThemeDataEx(HWND w, LPCWSTR classes, DWORD flags) { (void)flags; return OpenThemeData(w, classes); }
UXAPI HTHEME WINAPI OpenThemeDataForDpi(HWND w, LPCWSTR classes, UINT dpi) { (void)dpi; return OpenThemeData(w, classes); }
UXAPI HTHEME WINAPI GetWindowTheme(HWND w) { (void)w; return 0; }
UXAPI HRESULT WINAPI CloseThemeData(HTHEME t) { (void)t; return S_OK; }

UXAPI HRESULT WINAPI SetWindowTheme(HWND w, LPCWSTR app, LPCWSTR idlist) { (void)w; (void)app; (void)idlist; return S_OK; }
UXAPI HRESULT WINAPI SetWindowThemeAttribute(HWND w, int type, PVOID attr, DWORD n)
{ (void)w; (void)type; (void)attr; (void)n; return S_OK; }
UXAPI HRESULT WINAPI EnableThemeDialogTexture(HWND w, DWORD flags) { (void)w; (void)flags; return S_OK; }
UXAPI HRESULT WINAPI EnableTheming(BOOL on) { (void)on; return S_OK; }

/* Drawing with a theme: there is none */
UXAPI HRESULT WINAPI DrawThemeBackground(HTHEME t, HDC dc, int part, int state, const RECT *r, const RECT *clip)
{ (void)t; (void)dc; (void)part; (void)state; (void)r; (void)clip; return E_HANDLE_; }
UXAPI HRESULT WINAPI DrawThemeBackgroundEx(HTHEME t, HDC dc, int part, int state, const RECT *r, const void *opts)
{ (void)t; (void)dc; (void)part; (void)state; (void)r; (void)opts; return E_HANDLE_; }
UXAPI HRESULT WINAPI DrawThemeEdge(HTHEME t, HDC dc, int part, int state, const RECT *r, UINT edge, UINT flags, RECT *content)
{ (void)t; (void)part; (void)state; (void)content; return DrawEdge(dc, (RECT *)r, edge, flags) ? S_OK : E_HANDLE_; }
UXAPI HRESULT WINAPI DrawThemeIcon(HTHEME t, HDC dc, int part, int state, const RECT *r, HANDLE il, int i)
{ (void)t; (void)dc; (void)part; (void)state; (void)r; (void)il; (void)i; return E_HANDLE_; }
UXAPI HRESULT WINAPI DrawThemeText(HTHEME t, HDC dc, int part, int state, LPCWSTR s, int n, DWORD flags, DWORD flags2, const RECT *r)
{ (void)t; (void)dc; (void)part; (void)state; (void)s; (void)n; (void)flags; (void)flags2; (void)r; return E_HANDLE_; }
UXAPI HRESULT WINAPI DrawThemeTextEx(HTHEME t, HDC dc, int part, int state, LPCWSTR s, int n, DWORD flags, RECT *r, const void *opts)
{ (void)t; (void)dc; (void)part; (void)state; (void)s; (void)n; (void)flags; (void)r; (void)opts; return E_HANDLE_; }

/* The parent's background behind a control: ask the parent to paint it */
UXAPI HRESULT WINAPI DrawThemeParentBackground(HWND w, HDC dc, const RECT *r)
{
    (void)r;
    HWND parent = GetParent(w);
    if (!parent) return S_OK;
    POINT pt = { 0, 0 };
    MapWindowPoints(w, parent, &pt, 1);
    POINT old;
    OffsetViewportOrgEx(dc, -pt.x, -pt.y, &old);
    SendMessageW(parent, WM_ERASEBKGND, (WPARAM)dc, 0);
    SendMessageW(parent, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT_);
    SetViewportOrgEx(dc, old.x, old.y, NULL);
    return S_OK;
}
UXAPI HRESULT WINAPI DrawThemeParentBackgroundEx(HWND w, HDC dc, DWORD flags, const RECT *r)
{ (void)flags; return DrawThemeParentBackground(w, dc, r); }

/* Theme metrics and properties: not found */
UXAPI HRESULT WINAPI GetThemeBackgroundContentRect(HTHEME t, HDC dc, int part, int state, const RECT *bounds, RECT *content)
{ (void)t; (void)dc; (void)part; (void)state; if (content && bounds) *content = *bounds; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeBackgroundExtent(HTHEME t, HDC dc, int part, int state, const RECT *content, RECT *extent)
{ (void)t; (void)dc; (void)part; (void)state; if (content && extent) *extent = *content; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeBackgroundRegion(HTHEME t, HDC dc, int part, int state, const RECT *r, HRGN *rgn)
{ (void)t; (void)dc; (void)part; (void)state; (void)r; if (rgn) *rgn = 0; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemePartSize(HTHEME t, HDC dc, int part, int state, const RECT *r, int esize, SIZE *sz)
{ (void)t; (void)dc; (void)part; (void)state; (void)r; (void)esize; if (sz) sz->cx = sz->cy = 0; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeTextExtent(HTHEME t, HDC dc, int part, int state, LPCWSTR s, int n, DWORD flags, const RECT *bound, RECT *ext)
{ (void)t; (void)dc; (void)part; (void)state; (void)s; (void)n; (void)flags; (void)bound; (void)ext; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeTextMetrics(HTHEME t, HDC dc, int part, int state, TEXTMETRICW *tm)
{ (void)t; (void)dc; (void)part; (void)state; (void)tm; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeFont(HTHEME t, HDC dc, int part, int state, int prop, LOGFONTW *lf)
{ (void)t; (void)dc; (void)part; (void)state; (void)prop; (void)lf; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeSysFont(HTHEME t, int id, LOGFONTW *lf)
{
    (void)t; (void)id;
    if (!lf) return E_INVALIDARG_;
    if (!GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(*lf), lf)) return E_HANDLE_;
    return S_OK;
}
UXAPI HRESULT WINAPI GetThemeColor(HTHEME t, int part, int state, int prop, COLORREF *c)
{ (void)t; (void)part; (void)state; (void)prop; (void)c; return E_HANDLE_; }
UXAPI COLORREF WINAPI GetThemeSysColor(HTHEME t, int id) { (void)t; return GetSysColor(id); }
UXAPI HBRUSH WINAPI GetThemeSysColorBrush(HTHEME t, int id) { (void)t; return GetSysColorBrush(id); }
UXAPI int WINAPI GetThemeSysSize(HTHEME t, int id) { (void)t; return GetSystemMetrics(id); }
UXAPI BOOL WINAPI GetThemeSysBool(HTHEME t, int id) { (void)t; (void)id; return FALSE; }
UXAPI HRESULT WINAPI GetThemeSysInt(HTHEME t, int id, int *v) { (void)t; (void)id; (void)v; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeInt(HTHEME t, int part, int state, int prop, int *v)
{ (void)t; (void)part; (void)state; (void)prop; (void)v; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeBool(HTHEME t, int part, int state, int prop, BOOL *v)
{ (void)t; (void)part; (void)state; (void)prop; (void)v; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeEnumValue(HTHEME t, int part, int state, int prop, int *v)
{ (void)t; (void)part; (void)state; (void)prop; (void)v; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeMargins(HTHEME t, HDC dc, int part, int state, int prop, const RECT *r, void *m)
{ (void)t; (void)dc; (void)part; (void)state; (void)prop; (void)r; (void)m; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeMetric(HTHEME t, HDC dc, int part, int state, int prop, int *v)
{ (void)t; (void)dc; (void)part; (void)state; (void)prop; (void)v; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemePosition(HTHEME t, int part, int state, int prop, POINT *p)
{ (void)t; (void)part; (void)state; (void)prop; (void)p; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeRect(HTHEME t, int part, int state, int prop, RECT *r)
{ (void)t; (void)part; (void)state; (void)prop; (void)r; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeString(HTHEME t, int part, int state, int prop, LPWSTR s, int n)
{ (void)t; (void)part; (void)state; (void)prop; if (s && n) s[0] = 0; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemePropertyOrigin(HTHEME t, int part, int state, int prop, int *o)
{ (void)t; (void)part; (void)state; (void)prop; (void)o; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetThemeTransitionDuration(HTHEME t, int part, int from, int to, int prop, DWORD *ms)
{ (void)t; (void)part; (void)from; (void)to; (void)prop; if (ms) *ms = 0; return E_HANDLE_; }
UXAPI HRESULT WINAPI GetCurrentThemeName(LPWSTR file, int nf, LPWSTR color, int nc, LPWSTR size, int ns)
{
    if (file && nf) file[0] = 0;
    if (color && nc) color[0] = 0;
    if (size && ns) size[0] = 0;
    return E_NOTIMPL_;
}
UXAPI HRESULT WINAPI HitTestThemeBackground(HTHEME t, HDC dc, int part, int state, DWORD opts, const RECT *r, HRGN rgn, POINT pt, WORD *code)
{ (void)t; (void)dc; (void)part; (void)state; (void)opts; (void)r; (void)rgn; (void)pt; if (code) *code = 0; return E_HANDLE_; }

/* -----------------------------------------------------------------------
 * Buffered painting
 * ----------------------------------------------------------------------- */
typedef struct {
    DWORD magic;
    HDC target, mem;
    HBITMAP bmp, old;
    RECT r;
} Buffered;
#define BP_MAGIC 0x42504E54

UXAPI HRESULT WINAPI BufferedPaintInit(void) { return S_OK; }
UXAPI HRESULT WINAPI BufferedPaintUnInit(void) { return S_OK; }

UXAPI HANDLE WINAPI BeginBufferedPaint(HDC target, const RECT *r, int format, void *params, HDC *out)
{
    (void)format; (void)params;
    if (out) *out = 0;
    if (!target || !r || r->right <= r->left || r->bottom <= r->top) return 0;
    Buffered *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b));
    if (!b) return 0;
    b->magic = BP_MAGIC;
    b->target = target;
    b->r = *r;
    b->mem = CreateCompatibleDC(target);
    b->bmp = CreateCompatibleBitmap(target, r->right - r->left, r->bottom - r->top);
    if (!b->mem || !b->bmp) {
        if (b->mem) DeleteDC(b->mem);
        if (b->bmp) DeleteObject(b->bmp);
        HeapFree(GetProcessHeap(), 0, b);
        return 0;
    }
    b->old = SelectObject(b->mem, b->bmp);
    SetViewportOrgEx(b->mem, -r->left, -r->top, NULL);   /* the program draws in target coordinates */
    SelectObject(b->mem, GetCurrentObject(target, OBJ_FONT_));
    SetTextColor(b->mem, GetTextColor(target));
    SetBkColor(b->mem, GetBkColor(target));
    SetBkMode(b->mem, GetBkMode(target));
    if (out) *out = b->mem;
    return b;
}

static Buffered *buffered(HANDLE h) { Buffered *b = h; return b && b->magic == BP_MAGIC ? b : 0; }

UXAPI HRESULT WINAPI EndBufferedPaint(HANDLE h, BOOL update)
{
    Buffered *b = buffered(h);
    if (!b) return E_INVALIDARG_;
    SetViewportOrgEx(b->mem, 0, 0, NULL);
    if (update) BitBlt(b->target, b->r.left, b->r.top, b->r.right - b->r.left, b->r.bottom - b->r.top, b->mem, 0, 0, SRCCOPY);
    SelectObject(b->mem, b->old);
    DeleteObject(b->bmp);
    DeleteDC(b->mem);
    b->magic = 0;
    HeapFree(GetProcessHeap(), 0, b);
    return S_OK;
}

UXAPI HDC WINAPI GetBufferedPaintDC(HANDLE h) { Buffered *b = buffered(h); return b ? b->mem : 0; }
UXAPI HDC WINAPI GetBufferedPaintTargetDC(HANDLE h) { Buffered *b = buffered(h); return b ? b->target : 0; }
UXAPI HRESULT WINAPI GetBufferedPaintTargetRect(HANDLE h, RECT *r)
{ Buffered *b = buffered(h); if (!b || !r) return E_INVALIDARG_; *r = b->r; return S_OK; }
UXAPI HRESULT WINAPI GetBufferedPaintBits(HANDLE h, void **bits, int *row)
{ (void)h; if (bits) *bits = 0; if (row) *row = 0; return E_NOTIMPL_; }

/* Fill the buffer (or part of it) with @alpha: there is no alpha channel
 * in a compatible bitmap, so only clearing to black is meaningful */
UXAPI HRESULT WINAPI BufferedPaintClear(HANDLE h, const RECT *r)
{
    Buffered *b = buffered(h);
    if (!b) return E_INVALIDARG_;
    RECT c = r ? *r : b->r;
    FillRect(b->mem, &c, GetStockObject(BLACK_BRUSH));
    return S_OK;
}
UXAPI HRESULT WINAPI BufferedPaintSetAlpha(HANDLE h, const RECT *r, BYTE alpha)
{ (void)r; (void)alpha; return buffered(h) ? S_OK : E_INVALIDARG_; }

/* Animations: none run, so programs paint the end state directly */
UXAPI HANDLE WINAPI BeginBufferedAnimation(HWND w, HDC target, const RECT *r, int format, void *params,
                                            void *anim, HDC *from, HDC *to)
{
    (void)w; (void)target; (void)r; (void)format; (void)params; (void)anim;
    if (from) *from = 0;
    if (to) *to = 0;
    return 0;
}
UXAPI HRESULT WINAPI EndBufferedAnimation(HANDLE h, BOOL update) { (void)h; (void)update; return S_OK; }
UXAPI BOOL WINAPI BufferedPaintRenderAnimation(HWND w, HDC dc) { (void)w; (void)dc; return FALSE; }
UXAPI HRESULT WINAPI BufferedPaintStopAllAnimations(HWND w) { (void)w; return S_OK; }
