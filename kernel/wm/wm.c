/*
 * wm.c — NovaOS Window Manager implementation
 *
 * Windows live in a fixed static pool.  Each frame the compositor restores
 * the cached desktop background, draws visible windows in ascending
 * z-order (each client clipped to its client area), draws the shell
 * overlay (dock, Start menu), then presents the back buffer.
 *
 * Frames follow Windows 11 dark mode: rounded corners, a soft shadow that
 * is stronger on the focused window, and 46x32 caption buttons with hover
 * highlights (red for close).
 */

#include "../ke/smp.h"
#include "wm.h"
#include "../gdi/gdi.h"
#include "../lib/string.h"
#include "../ke/printf.h"

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static WND        g_windows[WM_MAX_WINDOWS];
static bool       g_used[WM_MAX_WINDOWS];
static int        g_next_id = 1;
static int        g_next_z  = 1;
static WmLayerFn  g_background;
static WmLayerFn  g_overlay;
static bool       g_ready;
/* Redraw needed while the two differ: invalidations bump g_dirty_gen (from
 * any thread, even while a frame is being drawn: that frame may predate
 * them), a finished frame records the generation it started from */
static volatile UINT32 g_dirty_gen = 1, g_drawn_gen;
static inline void mark_dirty(void) { __atomic_add_fetch(&g_dirty_gen, 1, __ATOMIC_RELEASE); }
static struct WND *by_id(int id);
static GdiRect    g_work;
static WmIconFn   g_icon_fn;

/* Hit-test parts */
enum { HT_NONE, HT_CLIENT, HT_CAPTION, HT_MIN, HT_MAX, HT_CLOSE, HT_RESIZE };
/* Resize edges (HT_RESIZE) */
enum { EDGE_L = 1, EDGE_R = 2, EDGE_T = 4, EDGE_B = 8 };
#define EDGE_OUT    4               /* resize band outside the frame */
#define EDGE_GRAB   6               /* px inside the frame that grab an edge */
#define MIN_W       360
#define MIN_H       220

static WND *g_drag;                 /* window being dragged by its title */
static int  g_drag_dx, g_drag_dy;   /* cursor offset from frame origin */
static int  g_snap_zone;            /* WM_SNAP_* the drag would tile to (0: none) */
static WND *g_resize;               /* window being resized by an edge */
static int  g_resize_edges;
static GdiRect g_resize_start;
static int  g_resize_x, g_resize_y;
static int  g_hit_edges;            /* edges under the pointer (set by hit) */
static WND *g_capture;              /* client that received the press */
static WND *g_press;                /* caption button being pressed */
static int  g_press_part;
static WND *g_hover;                /* caption button under the pointer */
static int  g_hover_part;
static WND *g_grab;                 /* explicit capture (WmSetCapture) */
static WND *g_rcapture;             /* client that received a right/middle press */
static WND *g_hover_client;         /* hover window the pointer is over */
static int  g_wheel;
static UINT32 g_buttons;

/* Windows 11 dark palette */
/* The title bar is a step darker than toolbars (0x20) and content (0x27),
 * with a hairline under it, so the frame reads apart from the app */
#define TITLE_ACTIVE    GDI_C(0x1A, 0x1A, 0x1C)
#define TITLE_INACTIVE  GDI_C(0x26, 0x26, 0x28)
#define TEXT_ACTIVE     GDI_C(0xFF, 0xFF, 0xFF)
#define TEXT_INACTIVE   GDI_C(0x9A, 0x9A, 0x9A)
#define BTN_HOVER       GDI_C(0x3A, 0x3A, 0x3A)
#define CLOSE_HOVER     GDI_C(0xC4, 0x2B, 0x1C)

#define CORNER   8
#define BTN_W    46

static bool pt_in(GdiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void WmInitialize(void)
{
    memset(g_windows, 0, sizeof(g_windows));
    memset(g_used,    0, sizeof(g_used));
    g_next_id = 1;
    g_next_z  = 1;
    g_background = NULL;
    g_overlay    = NULL;
    g_work  = RECT(0, 0, GdiScreenW(), GdiScreenH());
    g_ready = true;
    mark_dirty();
    kprintf("[WM] Window manager initialized (%d window slots)\n",
            WM_MAX_WINDOWS);
}

void WmSetIconPainter(WmIconFn fn) { g_icon_fn = fn; }
void WmSetWorkArea(GdiRect r) { g_work = r; }
GdiRect WmWorkArea(void)      { return g_work; }

void WmInvalidate(void)           { mark_dirty(); }
void WmInvalidateBackground(void) { GdiCacheInvalidate(); mark_dirty(); }
bool WmNeedsRedraw(void)          { return __atomic_load_n(&g_dirty_gen, __ATOMIC_ACQUIRE) != g_drawn_gen; }

/* -----------------------------------------------------------------------
 * Window list
 * ----------------------------------------------------------------------- */
static bool shown(const WND *w)
{
    int i = (int)(w - g_windows);
    return g_used[i] && w->visible && !w->minimized;
}

/* Stacking: popups above every normal window, then by z */
static long long zkey(const WND *w) { return (w->popup ? (1LL << 40) : 0) + w->z; }

/* Topmost shown window, optionally excluding one */
static WND *topmost(const WND *except)
{
    WND *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (w == except || !shown(w) || w->popup) continue;
        if (!best || w->z > best->z) best = w;
    }
    return best;
}

WND *WmCreateWindow(const char *title, GdiRect frame, UINT32 style,
                    GdiColor client_bg, GdiColor accent,
                    WndPaintFn on_paint, void *user)
{
    return WmCreateWindowEx(title, frame, style, client_bg, accent, on_paint, user, true);
}

WND *WmCreateWindowEx(const char *title, GdiRect frame, UINT32 style,
                      GdiColor client_bg, GdiColor accent,
                      WndPaintFn on_paint, void *user, bool activate)
{
    if (!g_ready) return NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_used[i]) continue;
        WND *w = &g_windows[i];
        memset(w, 0, sizeof(*w));
        w->id        = g_next_id++;
        w->frame     = frame;
        w->restore   = frame;
        w->style     = style;
        w->client_bg = client_bg;
        w->accent    = accent;
        w->visible   = true;
        w->app       = -1;
        w->on_paint  = on_paint;
        w->user      = user;
        WmSetTitle(w, title);
        g_used[i] = true;
        if (activate) WmSetActive(w);
        else { w->z = g_next_z++; mark_dirty(); }
        return w;
    }
    kprintf("[WM] WARNING: window pool exhausted\n");
    return NULL;
}

void WmSetTitle(WND *w, const char *title)
{
    if (!w) return;
    w->title[0] = '\0';
    if (title) {
        strncpy(w->title, title, WM_TITLE_MAX - 1);
        w->title[WM_TITLE_MAX - 1] = '\0';
    }
    mark_dirty();
}

void WmDestroyWindow(WND *w)
{
    if (!w) return;
    int i = (int)(w - g_windows);
    if (i < 0 || i >= WM_MAX_WINDOWS || !g_used[i]) return;
    bool was_active = w->active;
    int owner = w->owner;
    if (w->on_close) w->on_close(w);
    if (g_drag == w)    g_drag = NULL;
    if (g_resize == w)  g_resize = NULL;
    if (g_capture == w) g_capture = NULL;
    if (g_press == w)   g_press = NULL;
    if (g_hover == w)   g_hover = NULL;
    if (g_grab == w)    g_grab = NULL;
    if (g_rcapture == w) g_rcapture = NULL;
    if (g_hover_client == w) g_hover_client = NULL;
    g_used[i] = false;
    memset(w, 0, sizeof(*w));
    if (was_active) {
        WND *o = owner ? by_id(owner) : NULL;       /* a closed dialog gives the focus back to its owner */
        WND *next = o && shown(o) && !o->disabled ? o : topmost(NULL);
        if (next) WmSetActive(next);
    }
    mark_dirty();
}

void WmShowWindow(WND *w, bool visible)
{
    if (w) { w->visible = visible; mark_dirty(); }
}

/* Raise the windows @w owns (and theirs), keeping their order */
static void raise_owned(const WND *w, int depth)
{
    if (depth > 8) return;
    for (;;) {
        WND *low = NULL;                    /* the lowest owned one not yet above @w */
        for (int j = 0; j < WM_MAX_WINDOWS; j++) {
            WND *o = &g_windows[j];
            if (!g_used[j] || o->owner != w->id || o == w || o->z > w->z) continue;
            if (!low || o->z < low->z) low = o;
        }
        if (!low) return;
        low->z = g_next_z++;
        raise_owned(low, depth + 1);
    }
}

static WND *by_id(int id)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (g_used[i] && g_windows[i].id == id) return &g_windows[i];
    return NULL;
}

void WmSetActive(WND *w)
{
    if (!w) return;
    if (w->popup) { w->z = g_next_z++; mark_dirty(); return; }   /* popups never take the focus */
    for (int j = 0; j < WM_MAX_WINDOWS; j++)
        if (g_used[j]) g_windows[j].active = false;
    /* an owned window comes up with its owner(s) under it */
    WND *chain[8];
    int nc = 0;
    for (WND *o = w->owner ? by_id(w->owner) : NULL; o && nc < 8; o = o->owner ? by_id(o->owner) : NULL) chain[nc++] = o;
    while (nc) { WND *o = chain[--nc]; o->minimized = false; o->visible = true; o->z = g_next_z++; }
    w->minimized = false;
    w->visible   = true;
    w->active    = true;
    w->z = g_next_z++;
    raise_owned(w, 0);
    mark_dirty();
}

/* The window that takes input for @w: @w itself, or while it is disabled
 * (a modal dialog is up) the topmost enabled window it owns */
static WND *input_target(WND *w)
{
    for (int depth = 0; w && w->disabled && depth < 8; depth++) {
        WND *best = NULL;
        for (int j = 0; j < WM_MAX_WINDOWS; j++) {
            WND *o = &g_windows[j];
            if (g_used[j] && o->owner == w->id && shown(o) && !o->popup && (!best || o->z > best->z)) best = o;
        }
        if (!best) return NULL;
        w = best;
    }
    return w;
}

void WmSetFrame(WND *w, GdiRect frame)
{
    if (!w) return;
    w->frame = frame;
    if (!w->maximized && !w->snapped) w->restore = frame;
    mark_dirty();
}

void WmSetCapture(WND *w) { g_grab = w; }
WND *WmGetCapture(void)   { return g_grab; }
int  WmWheelDelta(void)   { return g_wheel; }
void WmSetButtons(UINT32 b) { g_buttons = b; }
UINT32 WmButtons(void)    { return g_buttons; }

WND *WmActiveWindow(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_used[i] && g_windows[i].active && shown(&g_windows[i]))
            return &g_windows[i];
    return NULL;
}

void WmRequestClose(WND *w)
{
    if (!w) return;
    if (w->on_close_request) { w->on_close_request(w); mark_dirty(); }
    else WmDestroyWindow(w);
}

void WmMinimize(WND *w)
{
    if (!w) return;
    w->minimized = true;
    w->active    = false;
    WND *next = topmost(w);
    if (next) WmSetActive(next);
    mark_dirty();
}

static bool tileable(const WND *w)
{
    return w && (w->style & WS_MINMAXBTN) && !w->fixed_size;
}

static void clamp_to_work(WND *w);

void WmSnap(WND *w, int where)
{
    if (!tileable(w)) return;
    bool tiled = w->maximized || w->snapped;
    if (where == WM_SNAP_RESTORE) {
        if (tiled) w->frame = w->restore;
        w->maximized = w->snapped = false;
        clamp_to_work(w);
        mark_dirty();
        return;
    }
    if (!tiled) w->restore = w->frame;
    GdiRect a = g_work;
    if (where == WM_SNAP_MAX) {
        w->frame = a;
        w->maximized = true;
        w->snapped = false;
    } else {
        int hw = a.w / 2;
        w->frame = where == WM_SNAP_LEFT ? RECT(a.x, a.y, hw, a.h) : RECT(a.x + hw, a.y, a.w - hw, a.h);
        w->snapped = true;
        w->maximized = false;
    }
    mark_dirty();
}

void WmToggleMaximize(WND *w)
{
    if (!tileable(w)) return;
    WmSnap(w, w->maximized ? WM_SNAP_RESTORE : WM_SNAP_MAX);
}

int WmListWindows(WND **out, int max)
{
    WND *all[WM_MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (!g_used[i] || !w->visible || w->popup || w->owner) continue;   /* the dock and Alt+Tab list top windows */
        int j = n++;
        while (j > 0 && all[j - 1]->z < w->z) { all[j] = all[j - 1]; j--; }
        all[j] = w;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = all[i];
    return n;
}

/* Win+D: windows minimized by the last "show desktop", by id */
static int g_desk_ids[WM_MAX_WINDOWS];
static int g_desk_n;

void WmShowDesktopToggle(void)
{
    bool any = false;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (shown(&g_windows[i])) any = true;
    if (any) {
        WND *order[WM_MAX_WINDOWS];
        int n = WmListWindows(order, WM_MAX_WINDOWS);
        g_desk_n = 0;
        for (int i = n - 1; i >= 0; i--) {      /* bottom first, so restore keeps the order */
            if (!shown(order[i])) continue;
            g_desk_ids[g_desk_n++] = order[i]->id;
            order[i]->minimized = true;
            order[i]->active = false;
        }
    } else {
        for (int k = 0; k < g_desk_n; k++)
            for (int i = 0; i < WM_MAX_WINDOWS; i++)
                if (g_used[i] && g_windows[i].id == g_desk_ids[k]) WmSetActive(&g_windows[i]);
        g_desk_n = 0;
    }
    mark_dirty();
}

GdiRect WmClientRect(const WND *w)
{
    GdiRect r = w->frame;
    int top = (w->style & WS_TITLEBAR) ? WM_TITLEBAR_H : 0;
    int b   = (w->style & WS_BORDER) && !w->maximized ? 1 : 0;
    return RECT(r.x + b, r.y + top, r.w - 2 * b, r.h - top - b);
}

WND *WmFindApp(int app)
{
    WND *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (!g_used[i] || w->app != app) continue;
        if (!best || w->z > best->z) best = w;
    }
    return best;
}

int WmWindowCount(void)
{
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (g_used[i]) n++;
    return n;
}

void WmSetDesktop(WmLayerFn background, WmLayerFn overlay)
{
    g_background = background;
    g_overlay    = overlay;
    WmInvalidateBackground();
}

/* -----------------------------------------------------------------------
 * Hit testing
 * ----------------------------------------------------------------------- */
static bool has_button(const WND *w, int part)
{
    if (!(w->style & WS_TITLEBAR)) return false;
    if (part == HT_CLOSE) return (w->style & WS_CLOSEBTN) != 0;
    if (part == HT_MAX && w->fixed_size) return false;
    return (w->style & WS_MINMAXBTN) != 0;
}

static GdiRect button_rect(const WND *w, int part)
{
    GdiRect f = w->frame;
    int slot = (part == HT_CLOSE) ? 1 : (part == HT_MAX) ? 2 : has_button(w, HT_MAX) ? 3 : 2;
    return RECT(f.x + f.w - slot * BTN_W, f.y, BTN_W, WM_TITLEBAR_H);
}

static bool resizable(const WND *w)
{
    return tileable(w) && !w->maximized;
}

static WND *hit(int x, int y, int *part)
{
    WND *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (!shown(w)) continue;
        GdiRect f = w->frame;
        if (resizable(w))                   /* grab band reaches outside the frame */
            f = RECT(f.x - EDGE_OUT, f.y - EDGE_OUT, f.w + 2 * EDGE_OUT, f.h + 2 * EDGE_OUT);
        if (!pt_in(f, x, y)) continue;
        if (!best || zkey(w) > zkey(best)) best = w;
    }
    *part = HT_NONE;
    g_hit_edges = 0;
    if (!best) return NULL;
    if (resizable(best)) {
        GdiRect f = best->frame;
        int e = 0;
        if (x < f.x + EDGE_GRAB)            e |= EDGE_L;
        if (x >= f.x + f.w - EDGE_GRAB)     e |= EDGE_R;
        if (y < f.y + 4)                    e |= EDGE_T;
        if (y >= f.y + f.h - EDGE_GRAB)     e |= EDGE_B;
        if (e) {
            g_hit_edges = e;
            *part = HT_RESIZE;
            return best;
        }
    }
    if ((best->style & WS_TITLEBAR) && y < best->frame.y + WM_TITLEBAR_H) {
        static const int btns[3] = { HT_CLOSE, HT_MAX, HT_MIN };
        for (int b = 0; b < 3; b++) {
            if (has_button(best, btns[b]) && pt_in(button_rect(best, btns[b]), x, y)) {
                *part = btns[b];
                return best;
            }
        }
        *part = HT_CAPTION;
    } else {
        *part = HT_CLIENT;
    }
    return best;
}

WND *WmWindowAt(int x, int y, bool *caption)
{
    int part;
    WND *w = hit(x, y, &part);
    if (caption) *caption = w && part != HT_CLIENT && part != HT_RESIZE;
    return w;
}

WND *WmWindowById(int id)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_used[i] && g_windows[i].id == id) return &g_windows[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * Input routing
 * ----------------------------------------------------------------------- */
static void to_client(WND *w, WmMouseMsg msg, int x, int y)
{
    if (!w->on_mouse) return;
    GdiRect c = WmClientRect(w);
    w->on_mouse(w, msg, x - c.x, y - c.y);
    mark_dirty();
}

static void clamp_to_work(WND *w)
{
    GdiRect *f = &w->frame;
    int keep = 120;                                   /* stays grabbable */
    if (f->y < g_work.y) f->y = g_work.y;
    if (f->y > g_work.y + g_work.h - WM_TITLEBAR_H) f->y = g_work.y + g_work.h - WM_TITLEBAR_H;
    if (f->x + f->w < g_work.x + keep) f->x = g_work.x + keep - f->w;
    if (f->x > g_work.x + g_work.w - keep) f->x = g_work.x + g_work.w - keep;
}

bool WmMouseButton(int x, int y, WmMouseMsg msg)
{
    if (g_grab) {                               /* everything to the capturing window */
        to_client(g_grab, msg, x, y);
        return true;
    }
    if (msg == WM_MOUSE_UP) {
        if (g_drag) {
            if (g_snap_zone) WmSnap(g_drag, g_snap_zone);
            g_drag = NULL;
            g_snap_zone = 0;
            mark_dirty();
            return true;
        }
        if (g_resize) { g_resize = NULL; return true; }
        if (g_press) {
            int part;
            WND *w = hit(x, y, &part);
            WND *p = g_press;
            int pp = g_press_part;
            g_press = NULL;
            mark_dirty();
            if (w == p && part == pp) {
                if (pp == HT_CLOSE)      WmRequestClose(p);
                else if (pp == HT_MAX)   WmToggleMaximize(p);
                else if (pp == HT_MIN)   WmMinimize(p);
            }
            return true;
        }
        if (g_capture) {
            WND *c = g_capture;
            g_capture = NULL;
            to_client(c, WM_MOUSE_UP, x, y);
            return true;
        }
        int part;
        return hit(x, y, &part) != NULL;
    }

    /* Press or double-click */
    int part;
    WND *w = hit(x, y, &part);
    if (!w) return false;
    if (w->disabled) {                          /* a modal dialog is up: bring it forward */
        WND *t = input_target(w);
        if (t) WmSetActive(t);
        mark_dirty();
        return true;
    }
    if (!w->no_activate && (!w->active || topmost(NULL) != w)) WmSetActive(w);

    switch (part) {
    case HT_CAPTION:
        if (msg == WM_MOUSE_DBLCLK && tileable(w)) {
            WmToggleMaximize(w);
            break;
        }
        if (w->maximized || w->snapped) {
            /* Pull the window out of maximize/snap, keeping the cursor at
             * the same relative spot along the title bar. */
            int rel = ((x - w->frame.x) * w->restore.w) / (w->frame.w > 0 ? w->frame.w : 1);
            w->maximized = w->snapped = false;
            w->frame = RECT(x - rel, w->frame.y, w->restore.w, w->restore.h);
        }
        g_drag    = w;
        g_drag_dx = x - w->frame.x;
        g_drag_dy = y - w->frame.y;
        g_snap_zone = 0;
        break;
    case HT_RESIZE:
        g_resize = w;
        g_resize_edges = g_hit_edges;
        g_resize_start = w->frame;
        g_resize_x = x;
        g_resize_y = y;
        w->snapped = false;                 /* a resized tile is a normal window */
        break;
    case HT_MIN: case HT_MAX: case HT_CLOSE:
        g_press = w;
        g_press_part = part;
        break;
    case HT_CLIENT:
        g_capture = w;
        to_client(w, msg, x, y);
        break;
    }
    mark_dirty();
    return true;
}

void WmMouseMove(int x, int y)
{
    if (g_drag) {
        g_drag->frame.x = x - g_drag_dx;
        g_drag->frame.y = y - g_drag_dy;
        clamp_to_work(g_drag);
        /* Dragging to an edge offers to tile the window there */
        int zone = 0;
        if (tileable(g_drag)) {
            if (y <= g_work.y + 1)                      zone = WM_SNAP_MAX;
            else if (x <= g_work.x + 1)                 zone = WM_SNAP_LEFT;
            else if (x >= g_work.x + g_work.w - 2)      zone = WM_SNAP_RIGHT;
        }
        g_snap_zone = zone;
        mark_dirty();
        return;
    }
    if (g_resize) {
        GdiRect f = g_resize_start;
        int dx = x - g_resize_x, dy = y - g_resize_y;
        if (g_resize_edges & EDGE_L) {
            int nw = f.w - dx;
            if (nw < MIN_W) nw = MIN_W;
            f.x += f.w - nw;
            f.w = nw;
        }
        if (g_resize_edges & EDGE_R) { f.w += dx; if (f.w < MIN_W) f.w = MIN_W; }
        if (g_resize_edges & EDGE_T) {
            int nh = f.h - dy;
            if (nh < MIN_H) nh = MIN_H;
            if (f.y + f.h - nh < g_work.y) nh = f.y + f.h - g_work.y;
            f.y += f.h - nh;
            f.h = nh;
        }
        if (g_resize_edges & EDGE_B) { f.h += dy; if (f.h < MIN_H) f.h = MIN_H; }
        if (f.y + f.h > g_work.y + g_work.h) f.h = g_work.y + g_work.h - f.y;
        g_resize->frame = f;
        mark_dirty();
        return;
    }
    if (g_grab) { to_client(g_grab, WM_MOUSE_MOVE, x, y); return; }
    if (g_capture) to_client(g_capture, WM_MOUSE_MOVE, x, y);
    else if (g_rcapture) to_client(g_rcapture, WM_MOUSE_MOVE, x, y);

    /* Caption-button hover highlight */
    int part;
    WND *w = hit(x, y, &part);
    /* windows that follow the pointer (buttons light up, menus track) */
    WND *hc = w && part == HT_CLIENT && w->hover && !w->disabled ? w : NULL;
    if (hc != g_hover_client) {
        if (g_hover_client && g_hover_client->on_mouse && !g_capture) g_hover_client->on_mouse(g_hover_client, WM_MOUSE_LEAVE, 0, 0);
        g_hover_client = hc;
    }
    if (hc && !g_capture && !g_rcapture) to_client(hc, WM_MOUSE_MOVE, x, y);
    if (part != HT_MIN && part != HT_MAX && part != HT_CLOSE) { w = NULL; part = HT_NONE; }
    if (w != g_hover || part != g_hover_part) {
        g_hover = w;
        g_hover_part = part;
        mark_dirty();
    }
}

bool WmMouseCaptured(void) { return g_drag || g_resize || g_capture || g_press || g_grab || g_rcapture; }

bool WmMouseOther(int x, int y, WmMouseMsg msg, int dz)
{
    g_wheel = dz;
    if (g_grab) { to_client(g_grab, msg, x, y); return true; }
    if ((msg == WM_MOUSE_RUP || msg == WM_MOUSE_MUP) && g_rcapture) {
        WND *c = g_rcapture;
        g_rcapture = NULL;
        to_client(c, msg, x, y);
        return true;
    }
    int part;
    WND *w = hit(x, y, &part);
    if (!w || part != HT_CLIENT || !w->rbutton) return false;
    if (w->disabled) {
        if (msg != WM_MOUSE_WHEEL) { WND *t = input_target(w); if (t) WmSetActive(t); }
        return true;
    }
    if ((msg == WM_MOUSE_RDOWN || msg == WM_MOUSE_MDOWN) && !w->no_activate && (!w->active || topmost(NULL) != w))
        WmSetActive(w);
    if (msg == WM_MOUSE_RDOWN || msg == WM_MOUSE_MDOWN) g_rcapture = w;
    to_client(w, msg, x, y);
    return true;
}

void WmTick(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (g_used[i] && w->on_tick && w->on_tick(w))
            mark_dirty();
    }
}

bool WmKey(const KeyEvent *k)
{
    if (!k) return false;
    WND *w = WmActiveWindow();
    if (!w) return false;
    if (w->disabled) { WND *t = input_target(w); if (t && t != w) WmSetActive(t); return true; }
    if (!k->pressed) {
        /* only windows that track held keys (program windows) see releases */
        if (!w->key_releases || !w->on_key) return false;
        w->on_key(w, k);
        return true;
    }
    if (k->alt && !k->extended && k->scancode == KEY_F4) {
        WmRequestClose(w);
        return true;
    }
    if (w->on_key) {
        w->on_key(w, k);
        mark_dirty();
    }
    return true;
}

/* -----------------------------------------------------------------------
 * Frame drawing
 * ----------------------------------------------------------------------- */
static void draw_button(const WND *w, int part, GdiColor title_bg, GdiColor fg)
{
    GdiRect b = button_rect(w, part);
    bool hot = (g_hover == w && g_hover_part == part);
    GdiColor bg = title_bg;
    if (hot) {
        bg = (part == HT_CLOSE) ? CLOSE_HOVER : BTN_HOVER;
        GdiRect hr = b;
        if (part == HT_CLOSE && !w->maximized) {
            /* follow the window's rounded top-right corner */
            GdiRoundRect(RECT(hr.x, hr.y, hr.w, hr.h + CORNER), CORNER, bg, GDI_TRANSPARENT);
            GdiFillRect(RECT(hr.x, hr.y + CORNER, CORNER, hr.h - CORNER), bg);
            GdiFillRect(RECT(hr.x, hr.y, CORNER, CORNER), bg);
        } else {
            GdiFillRect(hr, bg);
        }
        if (part == HT_CLOSE) fg = GDI_WHITE;
    }
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;

    switch (part) {
    case HT_MIN:
        GdiFillRect(RECT(cx - 5, cy, 10, 1), fg);
        break;
    case HT_MAX:
        if (w->maximized) {         /* "restore": two overlapping squares */
            GdiRoundRect(RECT(cx - 3, cy - 7, 10, 10), 2, GDI_TRANSPARENT, fg);
            GdiRoundRect(RECT(cx - 5, cy - 5, 10, 10), 2, bg, fg);
        } else {
            GdiRoundRect(RECT(cx - 5, cy - 5, 10, 10), 2, GDI_TRANSPARENT, fg);
        }
        break;
    case HT_CLOSE: {
        int x0 = (cx - 5) * 16, y0 = (cy - 5) * 16, x1 = (cx + 5) * 16, y1 = (cy + 5) * 16;
        GdiLine((GdiPoint){ x0, y0 }, (GdiPoint){ x1, y1 }, 18, fg);
        GdiLine((GdiPoint){ x0, y1 }, (GdiPoint){ x1, y0 }, 18, fg);
        break; }
    }
}

static void draw_window(WND *w)
{
    GdiRect f   = w->frame;
    int     rad = w->maximized || w->popup ? 0 : CORNER;
    GdiColor title_bg = w->active ? TITLE_ACTIVE : TITLE_INACTIVE;
    GdiColor title_fg = w->active ? TEXT_ACTIVE  : TEXT_INACTIVE;

    if ((w->style & WS_SHADOW) && !w->maximized)       /* under the frame, drawn next */
        GdiDropShadowAround(RECT(f.x, f.y + 3, f.w, f.h), rad,
                            w->active ? 18 : 10, w->active ? 90 : 45,
                            w->client_bg == GDI_TRANSPARENT ? RECT(0, 0, 0, 0) : f, rad);

    GdiRoundRect(f, rad, w->client_bg, GDI_TRANSPARENT);

    if (w->style & WS_TITLEBAR) {
        /* Title strip with rounded top corners and a square bottom */
        GdiRoundRect(RECT(f.x, f.y, f.w, WM_TITLEBAR_H + rad), rad, title_bg,
                     GDI_TRANSPARENT);
        GdiFillRect(RECT(f.x, f.y + WM_TITLEBAR_H, f.w, rad), w->client_bg);

        int btns = 0;
        if (has_button(w, HT_CLOSE)) btns++;
        if (has_button(w, HT_MAX))   btns++;
        if (has_button(w, HT_MIN))   btns++;

        /* App mark + title, clipped so it never runs under the buttons */
        GdiSetClip(RECT(f.x, f.y, f.w - btns * BTN_W, WM_TITLEBAR_H));
        if (!g_icon_fn || !g_icon_fn(w, f.x + 10, f.y + 8, 16))
            GdiFillCircle(f.x + 17, f.y + 15, 4, w->accent);
        GdiTextT(f.x + 34, f.y + (WM_TITLEBAR_H - GDI_FONT_H) / 2, w->title, title_fg);
        GdiResetClip();

        if (w->style & WS_CLOSEBTN) draw_button(w, HT_CLOSE, title_bg, title_fg);
        if (has_button(w, HT_MAX)) draw_button(w, HT_MAX, title_bg, title_fg);
        if (has_button(w, HT_MIN)) draw_button(w, HT_MIN, title_bg, title_fg);
        GdiAlphaFill(RECT(f.x, f.y + WM_TITLEBAR_H - 1, f.w, 1), GDI_WHITE, 20);   /* ~8% white */
    }

    /* A light hairline edge: separates the window from dark wallpapers
     * and from windows behind it */
    if ((w->style & WS_BORDER) && !w->maximized)
        GdiRoundBorderAlpha(f, rad, GDI_WHITE, w->active ? 40 : 24);

    if (w->on_paint) {
        GdiSetClip(WmClientRect(w));
        /* the desktop draws without the big kernel lock; the built-in apps'
         * painters read state that other code changes under it */
        if (!w->paint_lock_free) bkl_acquire();
        w->on_paint(w);
        if (!w->paint_lock_free) bkl_release();
        GdiResetClip();
    }
}

/* -----------------------------------------------------------------------
 * Compositor
 * ----------------------------------------------------------------------- */
static void cursor_after_present(void);

void WmComposite(void)
{
    if (!g_ready) return;
    UINT32 gen = __atomic_load_n(&g_dirty_gen, __ATOMIC_ACQUIRE);
    GdiResetClip();

    /* 1. Desktop background (wallpaper + icons), cached between frames */
    if (!GdiCacheRestore()) {
        if (g_background) g_background();
        GdiCacheSave();
    }

    /* 2. Windows, ascending z-order */
    WND *order[WM_MAX_WINDOWS];
    int  n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!shown(&g_windows[i])) continue;
        WND *w = &g_windows[i];
        int j = n++;
        while (j > 0 && zkey(order[j - 1]) > zkey(w)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = w;
    }
    for (int i = 0; i < n; i++)
        draw_window(order[i]);

    /* Where a window being dragged to a screen edge would be tiled */
    if (g_drag && g_snap_zone) {
        GdiRect a = g_work, p = a;
        if (g_snap_zone == WM_SNAP_LEFT)  p = RECT(a.x, a.y, a.w / 2, a.h);
        if (g_snap_zone == WM_SNAP_RIGHT) p = RECT(a.x + a.w / 2, a.y, a.w - a.w / 2, a.h);
        p = RECT(p.x + 8, p.y + 8, p.w - 16, p.h - 16);
        GdiRoundAlpha(p, 10, GDI_C(0x9A, 0xC8, 0xF0), 70);
        GdiRoundRect(p, 10, GDI_TRANSPARENT, GDI_C(0xC8, 0xE4, 0xFF));
    }

    /* 3. Overlay (dock, Start menu) */
    GdiResetClip();
    if (g_overlay) g_overlay();

    /* 4. Show the finished frame, with the pointer drawn over it */
    GdiPresent();
    cursor_after_present();
    GdiFlip();
    g_drawn_gen = gen;
}

/* -----------------------------------------------------------------------
 * Software mouse cursor
 *
 * The pointer is an anti-aliased vector arrow drawn by the GDI straight to
 * the screen (over the presented frame) with its own save-under, so moving
 * it never requires recompositing.  Its position is kept in DEVICE pixels
 * so it is drawn at native resolution; WmCursorX/Y report logical ones.
 * ----------------------------------------------------------------------- */
static int  g_cx, g_cy;            /* device pixels */
static bool g_cursor_shown;
static bool g_cursor_started;      /* shown once: every frame redraws it */

int WmCursorX(void) { return g_cx / GdiScale(); }
int WmCursorY(void) { return g_cy / GdiScale(); }

static void cursor_show_dev(int dx, int dy)
{
    int s = GdiScale();
    int maxx = GdiScreenW() * s - 1, maxy = GdiScreenH() * s - 1;
    if (dx < 0) dx = 0;
    if (dx > maxx) dx = maxx;
    if (dy < 0) dy = 0;
    if (dy > maxy) dy = maxy;
    g_cx = dx;
    g_cy = dy;
    GdiCursorDraw(g_cx, g_cy);
    g_cursor_shown = true;
    g_cursor_started = true;
}

void WmCursorShow(int x, int y)
{
    int s = GdiScale();
    cursor_show_dev(x * s, y * s);
}

void WmCursorHide(void)
{
    if (!g_cursor_shown) return;
    GdiCursorErase(g_cx, g_cy);
    g_cursor_shown = false;
}

void WmCursorMove(int x, int y)
{
    WmCursorHide();
    WmCursorShow(x, y);
}

void WmCursorMoveBy(int dx, int dy)
{
    /* One mouse count moves one logical pixel, as before scaling existed */
    int s = GdiScale();
    WmCursorHide();
    cursor_show_dev(g_cx + dx * s, g_cy + dy * s);
}

void WmCursorMoveAbs(int nx, int ny)
{
    int s = GdiScale();
    int w = GdiScreenW() * s, h = GdiScreenH() * s;
    WmCursorHide();
    cursor_show_dev((int)((INT64)nx * (w - 1) / 65535), (int)((INT64)ny * (h - 1) / 65535));
}

void WmCursorReshow(void)
{
    /* A new frame was presented: the pointer and its save-under are gone.
     * Grab a fresh save-under and redraw at the current position. */
    g_cursor_shown = false;
    cursor_show_dev(g_cx, g_cy);
}

/* WmComposite: the frame is on the surface the pointer is drawn on (with
 * page flipping, the page about to be shown) */
static void cursor_after_present(void)
{
    if (g_cursor_started) WmCursorReshow();
}

/* -----------------------------------------------------------------------
 * Display mode changes
 * ----------------------------------------------------------------------- */
void WmDisplayChanged(int old_w, int old_h, int old_s)
{
    /* The pointer keeps its place in proportion; its save-under belonged
     * to the old surface */
    int nw = GdiScreenW(), nh = GdiScreenH(), s = GdiScale();
    if (old_w > 0 && old_h > 0 && old_s > 0) {
        g_cx = (int)((INT64)(g_cx / old_s) * nw / old_w) * s;
        g_cy = (int)((INT64)(g_cy / old_s) * nh / old_h) * s;
    }
    if (g_cx > nw * s - 1) g_cx = nw * s - 1;
    if (g_cy > nh * s - 1) g_cy = nh * s - 1;
    g_cursor_shown = false;

    /* Windows: maximized ones fill the new work area, tiled ones go back
     * to their own size, and everything is kept on screen.  A window an
     * earlier, smaller mode shrank or pushed aside grows back towards the
     * frame it had (unless it was moved or resized since). */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_used[i]) continue;
        WND *w = &g_windows[i];
        if (w->maximized) { w->frame = g_work; continue; }
        if (w->snapped) { w->frame = w->restore; w->snapped = false; }
        if (w->wanted.w) {
            if (memcmp(&w->frame, &w->shrunk, sizeof(GdiRect))) w->wanted.w = 0;   /* moved since */
            else w->frame = w->wanted;
        }
        GdiRect before = w->frame;
        if (!w->fixed_size && !w->popup) {
            if (w->frame.w > g_work.w) w->frame.w = g_work.w;
            if (w->frame.h > g_work.h) w->frame.h = g_work.h;
        }
        if (w->frame.x + w->frame.w > nw) w->frame.x = nw - w->frame.w;
        if (w->frame.x < 0) w->frame.x = 0;
        if (!w->popup) clamp_to_work(w);
        if (memcmp(&w->frame, &before, sizeof(GdiRect))) {
            if (!w->wanted.w) w->wanted = before;
            w->shrunk = w->frame;
        } else {
            w->wanted.w = 0;                  /* it has its own frame again */
        }
    }
    GdiCacheInvalidate();
    mark_dirty();
}
