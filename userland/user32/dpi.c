/*
 * dpi.c — DPI awareness: SetProcessDpiAwareness(Context), the manifest's
 *         dpiAware/dpiAwareness, GetDpiForWindow/System/Monitor,
 *         WM_DPICHANGED, and the coordinates each kind of program sees
 *
 * The desktop works in logical pixels (96 DPI) and draws them at each
 * monitor's scale.  A monitor can also be set to show DPI-aware programs
 * its real density, 192 DPI on a 2x screen (NtNovaGuiCtl CTL_SET_DPI; 96,
 * the default, changes nothing for anyone).  Then:
 *
 *   unaware programs (the default, as on Windows) see 96 DPI and logical
 *       pixels everywhere; the desktop scales their windows up;
 *   system-aware ones (SetProcessDPIAware, dpiAware "true") see the
 *       primary's DPI as it was when they started, everywhere: all their
 *       coordinates are logical pixels times that scale;
 *   per-monitor-aware ones see each monitor's own DPI, and their windows
 *       get WM_DPICHANGED when they go to a monitor of another DPI or the
 *       monitor's DPI changes.
 *
 * An aware process's screen ("physical") coordinates: monitor i at logical
 * origin O with scale d (DPI / 96) starts at O * D, where D is the largest
 * d, and its pixels count d per logical pixel from there; so monitors
 * never overlap, and with every monitor at 96 DPI the two spaces are the
 * same.  A top-level window keeps its scale k (its monitor's d) in
 * Wnd.dpi_k: its bitmap holds k x k pixels per logical pixel (the kernel's
 * CTL_SET_SCALE), its frame insets are k times the desktop's, and the
 * mouse positions the desktop sends are multiplied by k.  Child windows
 * use their top-level's scale.  One process has one awareness (a thread's
 * SetThreadDpiAwarenessContext is remembered and reported, and a
 * window keeps the context it was created under, but coordinates follow
 * the process's).
 */
#include "u32.h"

#define WM_DPICHANGED_            0x02E0
#define WM_DPICHANGED_BEFOREPARENT_ 0x02E2
#define WM_DPICHANGED_AFTERPARENT_  0x02E3
#define WM_GETDPISCALEDSIZE_      0x02E4

/* DPI_AWARENESS_CONTEXT values (pseudo handles, as on Windows) */
#define CTX_UNAWARE   ((HANDLE)(LONG_PTR)-1)
#define CTX_SYSTEM    ((HANDLE)(LONG_PTR)-2)
#define CTX_PERMON    ((HANDLE)(LONG_PTR)-3)
#define CTX_PERMON_V2 ((HANDLE)(LONG_PTR)-4)
#define CTX_GDISCALED ((HANDLE)(LONG_PTR)-5)

static int   g_mode = -1;           /* DPI_* (PROCESS_DPI_AWARENESS); -1: not decided yet */
static int   g_v2;                  /* per-monitor aware v2 */
static int   g_fixed;               /* set by the manifest, the compatibility layer or a call */
static int   g_sys_k = 1;           /* the system DPI's scale, taken when the mode is decided */
static DWORD g_tls = TLS_OUT_OF_INDEXES;   /* a thread's own context (SetThreadDpiAwarenessContext) */

/* -----------------------------------------------------------------------
 * The monitors, as the desktop has them (logical px)
 * ----------------------------------------------------------------------- */
#define DPI_MON 8
static struct { RECT r; int d; } g_mon[DPI_MON];
static int g_nmon, g_dmax = 1, g_have;

static int read_mon(int i, INT32 v[11])
{
    return i >= 0 && NtNovaGuiCtl(0, CTL_MONITOR, (ULONG_PTR)i, v) ? v[0] : 0;
}

int dpi_monitor_raw(int i, RECT *r, RECT *work, int *scale, int *dpi)
{
    INT32 v[11];
    if (!read_mon(i, v)) return 0;
    if (r) SetRect(r, v[1], v[2], v[1] + v[3], v[2] + v[4]);
    if (work) SetRect(work, v[5], v[6], v[5] + v[7], v[6] + v[8]);
    if (scale) *scale = v[9];
    if (dpi) *dpi = v[10] >= 96 ? v[10] : 96;
    if (i < DPI_MON && g_have) {                            /* (keeps the cache current) */
        SetRect(&g_mon[i].r, v[1], v[2], v[1] + v[3], v[2] + v[4]);
        g_mon[i].d = v[10] >= 192 ? 2 : 1;
        if (v[0] != g_nmon) g_have = 0;                     /* plugged in or out: read them all */
        g_dmax = 1;
        for (int j = 0; j < g_nmon; j++) if (g_mon[j].d > g_dmax) g_dmax = g_mon[j].d;
    }
    return v[0];
}

void dpi_refresh(void)
{
    int n = 0;
    for (int i = 0; i < DPI_MON; i++) {
        INT32 v[11];
        int c = read_mon(i, v);
        if (!c) break;
        SetRect(&g_mon[i].r, v[1], v[2], v[1] + v[3], v[2] + v[4]);
        g_mon[i].d = v[10] >= 192 ? 2 : 1;
        n = i + 1;
        if (n >= c) break;
    }
    if (!n) {                                               /* (no desktop: one screen) */
        ULONG sw = 0, sh = 0;
        NtNovaGuiScreenSize(&sw, &sh);
        SetRect(&g_mon[0].r, 0, 0, (int)sw, (int)sh);
        g_mon[0].d = 1;
        n = 1;
    }
    g_nmon = n;
    g_dmax = 1;
    for (int i = 0; i < n; i++) if (g_mon[i].d > g_dmax) g_dmax = g_mon[i].d;
    g_have = 1;
}

static void have(void) { if (!g_have) dpi_refresh(); }

static LONGLONG dist2(const RECT *r, POINT p)
{
    LONGLONG dx = p.x < r->left ? r->left - p.x : p.x >= r->right ? p.x - r->right + 1 : 0;
    LONGLONG dy = p.y < r->top ? r->top - p.y : p.y >= r->bottom ? p.y - r->bottom + 1 : 0;
    return dx * dx + dy * dy;
}

/* Monitor i in this process's coordinates */
static void proc_rect(int i, RECT *o)
{
    const RECT *r = &g_mon[i].r;
    int d = g_mode == DPI_SYSTEM_AWARE ? g_sys_k : g_mon[i].d;
    int D = g_mode == DPI_SYSTEM_AWARE ? g_sys_k : g_dmax;
    o->left = r->left * D; o->top = r->top * D;
    o->right = o->left + (r->right - r->left) * d;
    o->bottom = o->top + (r->bottom - r->top) * d;
}

/* The monitor holding a point (logical, or this process's), else the nearest */
static int mon_at(POINT p, int proc)
{
    int best = 0;
    LONGLONG bd = -1;
    for (int i = 0; i < g_nmon; i++) {
        RECT r;
        if (proc) proc_rect(i, &r); else r = g_mon[i].r;
        LONGLONG d = dist2(&r, p);
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

/* The monitor a logical rectangle overlaps most, else the nearest */
static int mon_of_rect(const RECT *lr)
{
    int best = -1;
    LONGLONG area = 0;
    for (int i = 0; i < g_nmon; i++) {
        RECT x;
        if (!IntersectRect(&x, &g_mon[i].r, lr)) continue;
        LONGLONG a = (LONGLONG)(x.right - x.left) * (x.bottom - x.top);
        if (best < 0 || a > area) { best = i; area = a; }
    }
    if (best >= 0) return best;
    POINT c = { (lr->left + lr->right) / 2, (lr->top + lr->bottom) / 2 };
    return mon_at(c, 0);
}

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* -----------------------------------------------------------------------
 * The process's awareness
 * ----------------------------------------------------------------------- */
static int wmatch(const WCHAR *s, int n, const char *word)
{
    int i = 0;
    for (; i < n && word[i]; i++) {
        WCHAR c = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
        if (c != (WCHAR)word[i]) return 0;
    }
    return i == n && !word[i];
}

/* The text of the manifest element @tag (any namespace prefix), or NULL */
static const char *xml_value(const char *x, DWORD n, const char *tag, int *len)
{
    int tl = (int)strlen(tag);
    for (DWORD i = 0; i + tl + 1 < n; i++) {
        if (x[i] != '<' || x[i + 1] == '/') continue;
        DWORD j = i + 1;
        while (j < n && x[j] != '>' && x[j] != ' ' && x[j] != ':' && x[j] != '/') j++;
        if (j < n && x[j] == ':') j++; else j = i + 1;      /* a prefix: asmv3:dpiAware */
        if (j + tl > n || _strnicmp(x + j, tag, tl) || (x[j + tl] != '>' && x[j + tl] != ' ')) continue;
        while (j < n && x[j] != '>') j++;
        if (j >= n || x[j - 1] == '/') continue;
        DWORD s = j + 1, e = s;
        while (e < n && x[e] != '<') e++;
        *len = (int)(e - s);
        return x + s;
    }
    return NULL;
}

/* One comma-separated item of a manifest value, as UTF-16, trimmed */
static int next_item(const char **p, const char *end, WCHAR *out, int cap)
{
    while (*p < end && (**p == ' ' || **p == ',' || **p == '\t' || **p == '\r' || **p == '\n')) (*p)++;
    int n = 0;
    while (*p < end && **p != ',' && n < cap) out[n++] = (WCHAR)(unsigned char)*(*p)++;
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r' || out[n - 1] == '\n')) n--;
    return n;
}

/* dpiAwareness (a list: the first value known wins) over dpiAware */
static int from_manifest(int *v2)
{
    DWORD size = 0;
    const char *x = find_res(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24), &size);
    if (!x || !size) return -1;
    int len;
    const char *v = xml_value(x, size, "dpiAwareness", &len);
    if (v) {
        const char *p = v, *end = v + len;
        WCHAR w[40];
        int n;
        while ((n = next_item(&p, end, w, 40)) > 0) {
            if (wmatch(w, n, "permonitorv2")) { *v2 = 1; return DPI_PER_MONITOR_AWARE; }
            if (wmatch(w, n, "permonitor")) return DPI_PER_MONITOR_AWARE;
            if (wmatch(w, n, "system")) return DPI_SYSTEM_AWARE;
            if (wmatch(w, n, "unaware") || wmatch(w, n, "unaware_gdiscaled")) return DPI_UNAWARE;
        }
    }
    v = xml_value(x, size, "dpiAware", &len);
    if (v) {
        const char *p = v, *end = v + len;
        WCHAR w[40];
        int n = next_item(&p, end, w, 40);
        if (wmatch(w, n, "true/pm") || wmatch(w, n, "per monitor")) return DPI_PER_MONITOR_AWARE;
        if (wmatch(w, n, "true")) return DPI_SYSTEM_AWARE;
        if (wmatch(w, n, "false")) return DPI_UNAWARE;
    }
    return -1;
}

/* The compatibility layer (__COMPAT_LAYER, as Windows' "Override high DPI
 * scaling" setting writes it): DpiUnaware, GdiDpiScaling, HighDpiAware */
static int from_compat_layer(void)
{
    char v[256];
    DWORD n = GetEnvironmentVariableA("__COMPAT_LAYER", v, sizeof(v));
    if (!n || n >= sizeof(v)) return -1;
    for (char *c = v; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    if (strstr(v, "DPIUNAWARE") || strstr(v, "GDIDPISCALING")) return DPI_UNAWARE;
    if (strstr(v, "PERPROCESSSYSTEMDPIFORCEON") || strstr(v, "HIGHDPIAWARE")) return DPI_SYSTEM_AWARE;
    return -1;
}

static void decide(int mode, int v2)
{
    g_mode = mode;
    g_v2 = mode == DPI_PER_MONITOR_AWARE && v2;
    g_sys_k = 1;
    if (mode == DPI_UNAWARE) return;                        /* (nothing to convert: no monitors needed) */
    dpi_refresh();
    g_sys_k = g_mon[0].d;                                   /* the primary's, from now on */
}

int dpi_mode(void)
{
    if (g_mode >= 0) return g_mode;
    int v2 = 0, m = from_compat_layer();
    if (m < 0) m = from_manifest(&v2);
    if (m >= 0) g_fixed = 1;
    decide(m < 0 ? DPI_UNAWARE : m, v2);
    return g_mode;
}

static int aware(void) { return dpi_mode() != DPI_UNAWARE; }

/* Any window made yet (the awareness can't change after that) */
static int has_windows(void)
{
    Wnd *d = W_quiet(GetDesktopWindow());
    return d && d->child;
}

static BOOL set_mode(int mode, int v2)
{
    dpi_mode();
    if (g_fixed || has_windows()) {
        if (g_mode == mode && g_v2 == v2) return TRUE;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    g_fixed = 1;
    decide(mode, v2);
    return TRUE;
}

static HANDLE ctx_of(int mode, int v2)
{
    return mode == DPI_PER_MONITOR_AWARE ? (v2 ? CTX_PERMON_V2 : CTX_PERMON) :
           mode == DPI_SYSTEM_AWARE ? CTX_SYSTEM : CTX_UNAWARE;
}

/* A context's awareness (-1: not a context); *v2: per-monitor v2 */
static int ctx_mode(HANDLE c, int *v2)
{
    *v2 = 0;
    if (c == CTX_UNAWARE || c == CTX_GDISCALED) return DPI_UNAWARE;
    if (c == CTX_SYSTEM) return DPI_SYSTEM_AWARE;
    if (c == CTX_PERMON) return DPI_PER_MONITOR_AWARE;
    if (c == CTX_PERMON_V2) { *v2 = 1; return DPI_PER_MONITOR_AWARE; }
    /* the values GetThreadDpiAwarenessContext returns on Windows are
     * other numbers; accept their awareness bits */
    ULONG_PTR v = (ULONG_PTR)c;
    if (v >= 0x6000 && v < 0x80000) {
        int a = (int)(v & 3);
        if (a == 2 && (v & 0x20)) *v2 = 1;
        return a <= 2 ? a : -1;
    }
    return -1;
}

HANDLE dpi_thread_context(void)
{
    dpi_mode();
    HANDLE t = g_tls != TLS_OUT_OF_INDEXES ? TlsGetValue(g_tls) : NULL;
    return t ? t : ctx_of(g_mode, g_v2);
}

/* -----------------------------------------------------------------------
 * Coordinates
 * ----------------------------------------------------------------------- */
int dpi_aware(void) { return aware(); }

int dpi_k(Wnd *w)
{
    if (!w || !aware()) return 1;
    Wnd *t = top_of(w);
    return t && t->dpi_k > 1 ? t->dpi_k : 1;
}

int dpi_sys_k(void) { return aware() ? g_sys_k : 1; }

/* The scale a top-level window at logical frame @lr gets */
static int k_for_logical(const RECT *lr)
{
    if (g_mode == DPI_SYSTEM_AWARE) return g_sys_k;
    if (g_mode != DPI_PER_MONITOR_AWARE) return 1;
    have();
    return g_mon[mon_of_rect(lr)].d;
}

/* The scale a new top-level window at @r (this process's coordinates) gets */
int dpi_k_for_proc_rect(const RECT *r)
{
    if (!aware()) return 1;
    POINT a = { r->left, r->top }, b = { r->right, r->bottom };
    dpi_to_logical(&a);
    dpi_to_logical(&b);
    RECT lr = { a.x, a.y, b.x > a.x ? b.x : a.x + 1, b.y > a.y ? b.y : a.y + 1 };
    return k_for_logical(&lr);
}

void dpi_to_proc(POINT *p)
{
    if (!aware()) return;
    if (g_mode == DPI_SYSTEM_AWARE) { p->x *= g_sys_k; p->y *= g_sys_k; return; }
    have();
    int i = mon_at(*p, 0), d = g_mon[i].d;
    p->x = g_mon[i].r.left * g_dmax + (p->x - g_mon[i].r.left) * d;
    p->y = g_mon[i].r.top * g_dmax + (p->y - g_mon[i].r.top) * d;
}

void dpi_to_logical(POINT *p)
{
    if (!aware()) return;
    if (g_mode == DPI_SYSTEM_AWARE) { p->x = floordiv(p->x, g_sys_k); p->y = floordiv(p->y, g_sys_k); return; }
    have();
    int i = mon_at(*p, 1), d = g_mon[i].d;
    p->x = g_mon[i].r.left + floordiv(p->x - g_mon[i].r.left * g_dmax, d);
    p->y = g_mon[i].r.top + floordiv(p->y - g_mon[i].r.top * g_dmax, d);
}

/* A rectangle on one monitor (its top left decides which), corner by corner */
void dpi_rect_to_proc(RECT *r)
{
    if (!aware()) return;
    POINT a = { r->left, r->top };
    int w = r->right - r->left, h = r->bottom - r->top, k;
    if (g_mode == DPI_SYSTEM_AWARE) k = g_sys_k;
    else { have(); k = g_mon[mon_at(a, 0)].d; }
    dpi_to_proc(&a);
    SetRect(r, a.x, a.y, a.x + w * k, a.y + h * k);
}

/* Monitor @i's rectangle and work area for this process (display.c) */
void dpi_monitor_to_proc(int i, RECT *r, RECT *work)
{
    if (!aware()) return;
    have();
    if (i < 0 || i >= g_nmon) return;
    int d = g_mode == DPI_SYSTEM_AWARE ? g_sys_k : g_mon[i].d;
    RECT m = g_mon[i].r, pr;
    proc_rect(i, &pr);
    if (work) {
        work->left = pr.left + (work->left - m.left) * d;
        work->top = pr.top + (work->top - m.top) * d;
        work->right = pr.left + (work->right - m.left) * d;
        work->bottom = pr.top + (work->bottom - m.top) * d;
    }
    if (r) *r = pr;
}

int dpi_monitor_dpi(int i)
{
    if (!aware()) return 96;
    have();
    if (i < 0 || i >= g_nmon) return 96;
    if (g_mode == DPI_SYSTEM_AWARE) return 96 * g_sys_k;
    return 96 * g_mon[i].d;
}

/* -----------------------------------------------------------------------
 * Top-level windows and the desktop
 * ----------------------------------------------------------------------- */
/* The desktop's frame, client and bitmap (CTL_GET_RECT, logical) in this
 * process's coordinates for a window of scale @k; values within a pixel of
 * what the window has keep it (logical pixels lose odd ones) */
static int near_(LONG a, LONG b, int k) { return a - b < k && b - a < k; }

void dpi_from_kernel(Wnd *w, const INT32 r[9], int k, RECT *rect, POINT *bmp, int *bw, int *bh)
{
    if (!aware()) {
        SetRect(rect, r[4], r[5], r[4] + r[6], r[5] + r[7]);
        bmp->x = r[0]; bmp->y = r[1]; *bw = r[2]; *bh = r[3];
        return;
    }
    POINT o = { r[4], r[5] };
    dpi_to_proc(&o);
    if (near_(o.x, w->rect.left, k)) o.x = w->rect.left;
    if (near_(o.y, w->rect.top, k)) o.y = w->rect.top;
    int fw = r[6] * k, fh = r[7] * k;
    if (near_(fw, w->rect.right - w->rect.left, k)) fw = w->rect.right - w->rect.left;
    if (near_(fh, w->rect.bottom - w->rect.top, k)) fh = w->rect.bottom - w->rect.top;
    SetRect(rect, o.x, o.y, o.x + fw, o.y + fh);
    bmp->x = o.x + (r[0] - r[4]) * k;
    bmp->y = o.y + (r[1] - r[5]) * k;
    *bw = near_(r[2] * k, w->bw, k) ? w->bw : r[2] * k;
    *bh = near_(r[3] * k, w->bh, k) ? w->bh : r[3] * k;
}

/* The bitmap rectangle @b (this process's coordinates) for the desktop */
void dpi_to_kernel(Wnd *w, const RECT *b, INT32 out[4])
{
    POINT o = { b->left, b->top };
    int k = dpi_k(w);
    dpi_to_logical(&o);
    out[0] = o.x; out[1] = o.y;
    out[2] = (b->right - b->left + k - 1) / k;
    out[3] = (b->bottom - b->top + k - 1) / k;
}

/* The desktop window's rectangle: the primary monitor */
void dpi_desktop_rect(RECT *r)
{
    if (!aware()) return;
    have();
    proc_rect(0, r);
}

/* The pointer's position (logical) in this process's coordinates */
void dpi_cursor(POINT *p) { dpi_to_proc(p); }

static int g_in_change;

/* The desktop's bitmap for the window at scale @k: it may have grown and
 * moved (and the back buffer goes with it); everything is drawn anew */
int dpi_apply_scale(Wnd *w, int k)
{
    UINT32 geo[4] = { 0 };
    if (!w->kid) return k;
    int got = (int)NtNovaGuiCtl(w->kid, CTL_SET_SCALE, (ULONG_PTR)k, geo);
    if (got < 1) got = 1;
    DWORD *front = (DWORD *)(ULONG_PTR)((UINT64)geo[0] | (UINT64)geo[1] << 32);
    int stride = (int)geo[2] / 4, rows = (int)geo[3];
    if (front && stride > 0 && rows > 0 && (front != w->front || stride != w->stride || rows != w->maxh)) {
        DWORD *nb = VirtualAlloc(NULL, (SIZE_T)stride * rows * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (nb) {
            if (w->back) VirtualFree(w->back, 0, MEM_RELEASE);
            w->back = nb;
            w->front = front;
            w->stride = w->maxw = stride;
            w->maxh = rows;
            w->has_dirty = 0;
        }
    }
    return got;
}

static void children_notify(Wnd *w, UINT msg)
{
    for (Wnd *c = w->child; c; c = c->next) {
        HWND h = c->h;
        if (msg == WM_DPICHANGED_BEFOREPARENT_) send_msg(c, msg, 0, 0);
        if (!W_quiet(h)) continue;
        children_notify(c, msg);
        if (msg == WM_DPICHANGED_AFTERPARENT_ && W_quiet(h)) send_msg(c, msg, 0, 0);
    }
}

/* The window's scale is now @nk: WM_DPICHANGED with the rectangle the same
 * logical frame has at the new scale (DefWindowProc leaves it: then the
 * window is put there, keeping its size on the screen) */
static void dpi_changed(Wnd *w, int nk, const INT32 r[9])
{
    HWND h = w->h;
    POINT o = { r[4], r[5] };
    dpi_to_proc(&o);
    RECT sug = { o.x, o.y, o.x + r[6] * nk, o.y + r[7] * nk };
    g_in_change++;
    if (g_v2) {                                             /* the program may pick the size */
        SIZE sz = { sug.right - sug.left, sug.bottom - sug.top };
        if (send_msg(w, WM_GETDPISCALEDSIZE_, 96 * nk, (LPARAM)&sz) && W_quiet(h) && sz.cx > 0 && sz.cy > 0) {
            sug.right = sug.left + sz.cx;
            sug.bottom = sug.top + sz.cy;
        }
        if (W_quiet(h)) children_notify(w, WM_DPICHANGED_BEFOREPARENT_);
    }
    if (!W_quiet(h)) { g_in_change--; return; }
    int got = dpi_apply_scale(w, nk);
    if (got != nk) {                                        /* (no bitmap that size: stays as it is) */
        if (w->kid) dpi_apply_scale(w, w->dpi_k > 1 ? w->dpi_k : 1);
        g_in_change--;
        return;
    }
    w->dpi_k = nk;
    RECT before = w->rect;
    RECT pass = sug;
    send_msg(w, WM_DPICHANGED_, MAKEWPARAM(96 * nk, 96 * nk), (LPARAM)&pass);
    if (W_quiet(h) && EqualRect(&w->rect, &before))
        wnd_set_pos(w, 0, sug.left, sug.top, sug.right - sug.left, sug.bottom - sug.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (W_quiet(h)) {
        top_resized(w);                                     /* every pixel is drawn anew */
        if (g_v2) children_notify(w, WM_DPICHANGED_AFTERPARENT_);
    }
    g_in_change--;
}

/* After the desktop moved the window, or a monitor's DPI changed: is it
 * at another DPI now?  1 if it got WM_DPICHANGED (and is in place). */
int dpi_check(Wnd *w, const INT32 *kr)
{
    if (g_in_change || !w || w->parent || !w->kid || dpi_mode() != DPI_PER_MONITOR_AWARE) return 0;
    INT32 r[9];
    if (!kr) {
        if (!NtNovaGuiCtl(w->kid, CTL_GET_RECT, 0, r)) return 0;
        kr = r;
    }
    if (kr[8] & 4) return 0;                                /* minimized: not on a monitor */
    RECT lr = { kr[4], kr[5], kr[4] + kr[6], kr[5] + kr[7] };
    int nk = k_for_logical(&lr), ok = w->dpi_k > 1 ? w->dpi_k : 1;
    if (nk == ok) return 0;
    dpi_changed(w, nk, kr);
    return 1;
}

/* WM_NOVA_DPI / WM_DISPLAYCHANGE from the desktop: the monitors changed */
void dpi_monitors_changed(Wnd *top)
{
    dpi_refresh();
    if (top) dpi_check(top, NULL);
}

/* A new desktop window: its scale and GuiCreate's flag */
int dpi_new_window(Wnd *w, const RECT *b)
{
    if (!aware()) return 0;
    w->dpi_k = dpi_k_for_proc_rect(b);
    return 1;
}

/* -----------------------------------------------------------------------
 * The API
 * ----------------------------------------------------------------------- */
USERAPI BOOL SetProcessDpiAwarenessContext(HANDLE ctx)
{
    int v2, m = ctx_mode(ctx, &v2);
    if (m < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return set_mode(m, v2);
}

USERAPI BOOL SetProcessDPIAware(void) { dpi_mode(); return g_mode != DPI_UNAWARE ? TRUE : set_mode(DPI_SYSTEM_AWARE, 0); }
USERAPI BOOL IsProcessDPIAware(void) { return aware(); }

/* shcore's SetProcessDpiAwareness / GetProcessDpiAwareness (shlwapi) */
USERAPI HRESULT SetProcessDpiAwarenessInternal(int v)
{
    if (v < 0 || v > 2) return E_INVALIDARG;
    return set_mode(v, 0) ? S_OK : E_ACCESSDENIED;
}

USERAPI BOOL GetProcessDpiAwarenessInternal(HANDLE proc, int *v)
{
    if (!v) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    (void)proc;                                             /* (another process's: as our own) */
    *v = dpi_mode();
    return TRUE;
}

USERAPI HANDLE GetThreadDpiAwarenessContext(void) { return dpi_thread_context(); }

USERAPI HANDLE SetThreadDpiAwarenessContext(HANDLE ctx)
{
    int v2, m = ctx_mode(ctx, &v2);
    if (m < 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    HANDLE old = dpi_thread_context();
    if (g_tls == TLS_OUT_OF_INDEXES) {
        LOCK();
        if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc();
        UNLOCK();
    }
    if (g_tls != TLS_OUT_OF_INDEXES) TlsSetValue(g_tls, ctx_of(m, v2));
    return old;
}

USERAPI HANDLE GetWindowDpiAwarenessContext(HWND h)
{
    Wnd *w = W(h);
    if (!w) return NULL;
    return w->dpi_ctx ? w->dpi_ctx : ctx_of(dpi_mode(), g_v2);
}

USERAPI int GetAwarenessFromDpiAwarenessContext(HANDLE ctx) { int v2; return ctx_mode(ctx, &v2); }
USERAPI UINT GetDpiFromDpiAwarenessContext(HANDLE ctx)
{
    int v2, m = ctx_mode(ctx, &v2);
    dpi_mode();
    return m == DPI_SYSTEM_AWARE ? 96 * (unsigned)g_sys_k : m == DPI_UNAWARE ? 96 : 0;
}
USERAPI BOOL AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b)
{
    int va, vb, ma = ctx_mode(a, &va), mb = ctx_mode(b, &vb);
    return ma >= 0 && ma == mb && va == vb;
}
USERAPI BOOL IsValidDpiAwarenessContext(HANDLE ctx) { int v2; return ctx_mode(ctx, &v2) >= 0; }
USERAPI HANDLE GetDpiAwarenessContextForProcess(HANDLE p) { (void)p; return ctx_of(dpi_mode(), g_v2); }
USERAPI BOOL EnableNonClientDpiScaling(HWND h) { return W(h) != NULL; }

USERAPI UINT GetDpiForWindow(HWND h)
{
    Wnd *w = W(h);
    if (!w) return 0;
    return 96 * (UINT)dpi_k(w);
}

USERAPI UINT GetDpiForSystem(void) { return 96 * (UINT)dpi_sys_k(); }
USERAPI UINT GetSystemDpiForProcess(HANDLE p) { (void)p; return GetDpiForSystem(); }

/* shcore's GetDpiForMonitor (shlwapi): MDT_EFFECTIVE_DPI, MDT_ANGULAR_DPI
 * and MDT_RAW_DPI are all the monitor's DPI as this process sees it */
USERAPI BOOL GetDpiForMonitorInternal(HMONITOR m, UINT type, UINT *x, UINT *y)
{
    int i = (int)((ULONG_PTR)m - 0x10001);
    if (!x || !y || type > 2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (i < 0 || !dpi_monitor_raw(i, NULL, NULL, NULL, NULL)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *x = *y = (UINT)dpi_monitor_dpi(i);
    return TRUE;
}

USERAPI BOOL LogicalToPhysicalPoint(HWND h, LPPOINT p) { (void)h; return p != NULL; }
USERAPI BOOL PhysicalToLogicalPoint(HWND h, LPPOINT p) { (void)h; return p != NULL; }
USERAPI BOOL LogicalToPhysicalPointForPerMonitorDPI(HWND h, LPPOINT p) { (void)h; return p != NULL; }
USERAPI BOOL PhysicalToLogicalPointForPerMonitorDPI(HWND h, LPPOINT p) { (void)h; return p != NULL; }

USERAPI BOOL AdjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD ex, UINT dpi)
{
    return adjust_window_rect(r, style, menu, ex, dpi >= 144 ? 2 : 1);
}
