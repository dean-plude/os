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
static bool       g_dirty = true;
static GdiRect    g_work;
static WmIconFn   g_icon_fn;

/* Hit-test parts */
enum { HT_NONE, HT_CLIENT, HT_CAPTION, HT_MIN, HT_MAX, HT_CLOSE };

static WND *g_drag;                 /* window being dragged by its title */
static int  g_drag_dx, g_drag_dy;   /* cursor offset from frame origin */
static WND *g_capture;              /* client that received the press */
static WND *g_press;                /* caption button being pressed */
static int  g_press_part;
static WND *g_hover;                /* caption button under the pointer */
static int  g_hover_part;

/* Windows 11 dark palette */
#define TITLE_ACTIVE    GDI_C(0x20, 0x20, 0x20)
#define TITLE_INACTIVE  GDI_C(0x2B, 0x2B, 0x2B)
#define TEXT_ACTIVE     GDI_C(0xFF, 0xFF, 0xFF)
#define TEXT_INACTIVE   GDI_C(0x9A, 0x9A, 0x9A)
#define BORDER_ACTIVE   GDI_C(0x4A, 0x4A, 0x4A)
#define BORDER_INACTIVE GDI_C(0x3A, 0x3A, 0x3A)
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
    g_dirty = true;
    kprintf("[WM] Window manager initialized (%d window slots)\n",
            WM_MAX_WINDOWS);
}

void WmSetIconPainter(WmIconFn fn) { g_icon_fn = fn; }
void WmSetWorkArea(GdiRect r) { g_work = r; }
GdiRect WmWorkArea(void)      { return g_work; }

void WmInvalidate(void)           { g_dirty = true; }
void WmInvalidateBackground(void) { GdiCacheInvalidate(); g_dirty = true; }
bool WmNeedsRedraw(void)          { return g_dirty; }

/* -----------------------------------------------------------------------
 * Window list
 * ----------------------------------------------------------------------- */
static bool shown(const WND *w)
{
    int i = (int)(w - g_windows);
    return g_used[i] && w->visible && !w->minimized;
}

/* Topmost shown window, optionally excluding one */
static WND *topmost(const WND *except)
{
    WND *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (w == except || !shown(w)) continue;
        if (!best || w->z > best->z) best = w;
    }
    return best;
}

WND *WmCreateWindow(const char *title, GdiRect frame, UINT32 style,
                    GdiColor client_bg, GdiColor accent,
                    WndPaintFn on_paint, void *user)
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
        WmSetActive(w);
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
    g_dirty = true;
}

void WmDestroyWindow(WND *w)
{
    if (!w) return;
    int i = (int)(w - g_windows);
    if (i < 0 || i >= WM_MAX_WINDOWS || !g_used[i]) return;
    bool was_active = w->active;
    if (w->on_close) w->on_close(w);
    if (g_drag == w)    g_drag = NULL;
    if (g_capture == w) g_capture = NULL;
    if (g_press == w)   g_press = NULL;
    if (g_hover == w)   g_hover = NULL;
    g_used[i] = false;
    memset(w, 0, sizeof(*w));
    if (was_active) {
        WND *next = topmost(NULL);
        if (next) WmSetActive(next);
    }
    g_dirty = true;
}

void WmShowWindow(WND *w, bool visible)
{
    if (w) { w->visible = visible; g_dirty = true; }
}

void WmSetActive(WND *w)
{
    if (!w) return;
    for (int j = 0; j < WM_MAX_WINDOWS; j++)
        if (g_used[j]) g_windows[j].active = false;
    w->minimized = false;
    w->visible   = true;
    w->active    = true;
    w->z = g_next_z++;
    g_dirty = true;
}

WND *WmActiveWindow(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_used[i] && g_windows[i].active && shown(&g_windows[i]))
            return &g_windows[i];
    return NULL;
}

void WmMinimize(WND *w)
{
    if (!w) return;
    w->minimized = true;
    w->active    = false;
    WND *next = topmost(w);
    if (next) WmSetActive(next);
    g_dirty = true;
}

void WmToggleMaximize(WND *w)
{
    if (!w || !(w->style & WS_MINMAXBTN)) return;
    if (w->maximized) {
        w->frame = w->restore;
        w->maximized = false;
    } else {
        w->restore = w->frame;
        w->frame = g_work;
        w->maximized = true;
    }
    g_dirty = true;
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
static GdiRect button_rect(const WND *w, int part)
{
    GdiRect f = w->frame;
    int slot = (part == HT_CLOSE) ? 1 : (part == HT_MAX) ? 2 : 3;
    return RECT(f.x + f.w - slot * BTN_W, f.y, BTN_W, WM_TITLEBAR_H);
}

static bool has_button(const WND *w, int part)
{
    if (!(w->style & WS_TITLEBAR)) return false;
    if (part == HT_CLOSE) return (w->style & WS_CLOSEBTN) != 0;
    return (w->style & WS_MINMAXBTN) != 0;
}

static WND *hit(int x, int y, int *part)
{
    WND *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (!shown(w) || !pt_in(w->frame, x, y)) continue;
        if (!best || w->z > best->z) best = w;
    }
    *part = HT_NONE;
    if (!best) return NULL;
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

/* -----------------------------------------------------------------------
 * Input routing
 * ----------------------------------------------------------------------- */
static void to_client(WND *w, WmMouseMsg msg, int x, int y)
{
    if (!w->on_mouse) return;
    GdiRect c = WmClientRect(w);
    w->on_mouse(w, msg, x - c.x, y - c.y);
    g_dirty = true;
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
    if (msg == WM_MOUSE_UP) {
        if (g_drag) { g_drag = NULL; return true; }
        if (g_press) {
            int part;
            WND *w = hit(x, y, &part);
            WND *p = g_press;
            int pp = g_press_part;
            g_press = NULL;
            g_dirty = true;
            if (w == p && part == pp) {
                if (pp == HT_CLOSE)      WmDestroyWindow(p);
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
    if (!w->active || topmost(NULL) != w) WmSetActive(w);

    switch (part) {
    case HT_CAPTION:
        if (msg == WM_MOUSE_DBLCLK && (w->style & WS_MINMAXBTN)) {
            WmToggleMaximize(w);
            break;
        }
        if (w->maximized) {
            /* Pull the window out of maximize, keeping the cursor at the
             * same relative spot along the title bar. */
            int rel = ((x - w->frame.x) * w->restore.w) / (w->frame.w > 0 ? w->frame.w : 1);
            w->maximized = false;
            w->frame = RECT(x - rel, w->frame.y, w->restore.w, w->restore.h);
        }
        g_drag    = w;
        g_drag_dx = x - w->frame.x;
        g_drag_dy = y - w->frame.y;
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
    g_dirty = true;
    return true;
}

void WmMouseMove(int x, int y)
{
    if (g_drag) {
        g_drag->frame.x = x - g_drag_dx;
        g_drag->frame.y = y - g_drag_dy;
        clamp_to_work(g_drag);
        g_dirty = true;
        return;
    }
    if (g_capture) to_client(g_capture, WM_MOUSE_MOVE, x, y);

    /* Caption-button hover highlight */
    int part;
    WND *w = hit(x, y, &part);
    if (part != HT_MIN && part != HT_MAX && part != HT_CLOSE) { w = NULL; part = HT_NONE; }
    if (w != g_hover || part != g_hover_part) {
        g_hover = w;
        g_hover_part = part;
        g_dirty = true;
    }
}

bool WmMouseCaptured(void) { return g_drag || g_capture || g_press; }

void WmTick(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WND *w = &g_windows[i];
        if (g_used[i] && w->on_tick && w->on_tick(w))
            g_dirty = true;
    }
}

bool WmKey(const KeyEvent *k)
{
    if (!k) return false;
    WND *w = WmActiveWindow();
    if (!w) return false;
    if (!k->pressed) {
        /* only windows that track held keys (program windows) see releases */
        if (!w->key_releases || !w->on_key) return false;
        w->on_key(w, k);
        return true;
    }
    if (k->alt && !k->extended && k->scancode == KEY_F4) {
        WmDestroyWindow(w);
        return true;
    }
    if (w->on_key) {
        w->on_key(w, k);
        g_dirty = true;
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
    int     rad = w->maximized ? 0 : CORNER;
    GdiColor title_bg = w->active ? TITLE_ACTIVE : TITLE_INACTIVE;
    GdiColor title_fg = w->active ? TEXT_ACTIVE  : TEXT_INACTIVE;

    if ((w->style & WS_SHADOW) && !w->maximized)
        GdiDropShadow(RECT(f.x, f.y + 3, f.w, f.h), rad,
                      w->active ? 18 : 10, w->active ? 90 : 45);

    GdiRoundRect(f, rad, w->client_bg, GDI_TRANSPARENT);

    if (w->style & WS_TITLEBAR) {
        /* Title strip with rounded top corners and a square bottom */
        GdiRoundRect(RECT(f.x, f.y, f.w, WM_TITLEBAR_H + rad), rad, title_bg,
                     GDI_TRANSPARENT);
        GdiFillRect(RECT(f.x, f.y + WM_TITLEBAR_H, f.w, rad), w->client_bg);

        int btns = 0;
        if (w->style & WS_CLOSEBTN)  btns++;
        if (w->style & WS_MINMAXBTN) btns += 2;

        /* App mark + title, clipped so it never runs under the buttons */
        GdiSetClip(RECT(f.x, f.y, f.w - btns * BTN_W, WM_TITLEBAR_H));
        if (w->app >= 0 && g_icon_fn)
            g_icon_fn(w->app, f.x + 10, f.y + 8, 16);
        else
            GdiFillCircle(f.x + 17, f.y + 15, 4, w->accent);
        GdiTextT(f.x + 34, f.y + (WM_TITLEBAR_H - GDI_FONT_H) / 2, w->title, title_fg);
        GdiResetClip();

        if (w->style & WS_CLOSEBTN) draw_button(w, HT_CLOSE, title_bg, title_fg);
        if (w->style & WS_MINMAXBTN) {
            draw_button(w, HT_MAX, title_bg, title_fg);
            draw_button(w, HT_MIN, title_bg, title_fg);
        }
    }

    if ((w->style & WS_BORDER) && !w->maximized)
        GdiRoundRect(f, rad, GDI_TRANSPARENT, w->active ? BORDER_ACTIVE : BORDER_INACTIVE);

    if (w->on_paint) {
        GdiSetClip(WmClientRect(w));
        w->on_paint(w);
        GdiResetClip();
    }
}

/* -----------------------------------------------------------------------
 * Compositor
 * ----------------------------------------------------------------------- */
void WmComposite(void)
{
    if (!g_ready) return;
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
        while (j > 0 && order[j - 1]->z > w->z) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = w;
    }
    for (int i = 0; i < n; i++)
        draw_window(order[i]);

    /* 3. Overlay (dock, Start menu) */
    GdiResetClip();
    if (g_overlay) g_overlay();

    /* 4. Show the finished frame */
    GdiPresent();
    g_dirty = false;
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

void WmCursorReshow(void)
{
    /* A new frame was presented: the pointer and its save-under are gone.
     * Grab a fresh save-under and redraw at the current position. */
    g_cursor_shown = false;
    cursor_show_dev(g_cx, g_cy);
}
