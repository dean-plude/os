/*
 * wm.c — NovaOS Window Manager implementation
 *
 * A compact compositing WM.  Windows live in a fixed static pool; the
 * compositor walks them in ascending z-order so higher-z windows paint
 * last (on top).  Each frame is drawn straight to the framebuffer via
 * GDI — there is no off-screen back buffer yet (Phase 8 will add
 * double-buffering once a second mapping of VRAM is available).
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

/* Windows-11-ish frame palette */
#define TITLE_ACTIVE_BG    GDI_C(0x2B, 0x2B, 0x33)
#define TITLE_INACTIVE_BG  GDI_C(0x3A, 0x3A, 0x42)
#define TITLE_TEXT         GDI_C(0xF0, 0xF0, 0xF4)
#define BORDER_COLOR       GDI_C(0x50, 0x50, 0x5A)
#define CLOSE_HOT          GDI_C(0xE0, 0x40, 0x40)

void WmInitialize(void)
{
    memset(g_windows, 0, sizeof(g_windows));
    memset(g_used,    0, sizeof(g_used));
    g_next_id = 1;
    g_next_z  = 1;
    g_background = NULL;
    g_overlay    = NULL;
    g_ready = true;
    kprintf("[WM] Window manager initialized (%d window slots)\n",
            WM_MAX_WINDOWS);
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
        w->style     = style;
        w->client_bg = client_bg;
        w->accent    = accent;
        w->visible   = true;
        w->active    = true;
        w->z         = g_next_z++;
        w->on_paint  = on_paint;
        w->user      = user;
        if (title) {
            strncpy(w->title, title, WM_TITLE_MAX - 1);
            w->title[WM_TITLE_MAX - 1] = '\0';
        }
        /* New window becomes the sole active one. */
        for (int j = 0; j < WM_MAX_WINDOWS; j++)
            if (g_used[j]) g_windows[j].active = false;
        w->active = true;
        g_used[i] = true;
        return w;
    }
    kprintf("[WM] WARNING: window pool exhausted\n");
    return NULL;
}

void WmDestroyWindow(WND *w)
{
    if (!w) return;
    int i = (int)(w - g_windows);
    if (i < 0 || i >= WM_MAX_WINDOWS) return;
    g_used[i] = false;
    memset(w, 0, sizeof(*w));
}

void WmShowWindow(WND *w, bool visible)
{
    if (w) w->visible = visible;
}

void WmSetActive(WND *w)
{
    if (!w) return;
    for (int j = 0; j < WM_MAX_WINDOWS; j++)
        if (g_used[j]) g_windows[j].active = false;
    w->active = true;
    w->z = g_next_z++;
}

GdiRect WmClientRect(const WND *w)
{
    GdiRect r = w->frame;
    int top = (w->style & WS_TITLEBAR) ? WM_TITLEBAR_H : 0;
    int b   = (w->style & WS_BORDER) ? 1 : 0;
    return RECT(r.x + b, r.y + top, r.w - 2 * b, r.h - top - b);
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
}

/* -----------------------------------------------------------------------
 * Frame decoration
 * ----------------------------------------------------------------------- */
static void draw_window(WND *w)
{
    if (!w->visible) return;
    GdiRect f = w->frame;

    /* Drop shadow */
    if (w->style & WS_SHADOW) {
        for (int s = 6; s >= 1; s--) {
            int a = 10 + (6 - s) * 6;
            GdiRoundAlpha(RECT(f.x - s, f.y - s + 3, f.w + 2 * s, f.h + 2 * s),
                          12 + s, GDI_C(0, 0, 0), a);
        }
    }

    /* Client background */
    GdiRoundRect(f, 10, w->client_bg, GDI_TRANSPARENT);

    /* Title bar */
    if (w->style & WS_TITLEBAR) {
        GdiColor bg = w->active ? TITLE_ACTIVE_BG : TITLE_INACTIVE_BG;
        /* rounded top only: approximate by a rounded rect clipped to top */
        GdiRoundRect(RECT(f.x, f.y, f.w, WM_TITLEBAR_H + 10), 10, bg,
                     GDI_TRANSPARENT);
        GdiFillRect(RECT(f.x, f.y + WM_TITLEBAR_H, f.w, 1), BORDER_COLOR);

        /* Accent stripe on the left of the title */
        GdiFillRect(RECT(f.x, f.y, 4, WM_TITLEBAR_H), w->accent);

        GdiTextT(f.x + 12, f.y + (WM_TITLEBAR_H - GDI_FONT_H) / 2 + 1,
                 w->title, TITLE_TEXT);

        /* Caption buttons (close / min / max) as simple glyphs */
        int cy = f.y + (WM_TITLEBAR_H - GDI_FONT_H) / 2 + 1;
        int rx = f.x + f.w - 20;
        if (w->style & WS_CLOSEBTN) {
            GdiTextBold(rx, cy, "x",
                        w->active ? GDI_C(0xF0, 0xA0, 0xA0) : GDI_GRAY);
        }
        if (w->style & WS_MINMAXBTN) {
            GdiTextT(rx - 44, cy, "_", TITLE_TEXT);
            GdiTextT(rx - 22, cy, "[", TITLE_TEXT);
        }
    }

    /* Border */
    if (w->style & WS_BORDER) {
        GdiRoundRect(f, 10, GDI_TRANSPARENT, BORDER_COLOR);
    }

    /* Client content */
    if (w->on_paint) w->on_paint(w);
}

/* -----------------------------------------------------------------------
 * Compositor
 * ----------------------------------------------------------------------- */
void WmComposite(void)
{
    if (!g_ready) return;

    /* 1. Desktop background (wallpaper + icons) */
    if (g_background) g_background();

    /* 2. Windows, ascending z-order (simple insertion pass). */
    for (int pass_z = 1; pass_z < g_next_z; pass_z++) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (g_used[i] && g_windows[i].z == pass_z)
                draw_window(&g_windows[i]);
        }
    }

    /* 3. Overlay (taskbar, start menu) */
    if (g_overlay) g_overlay();
}

/* -----------------------------------------------------------------------
 * Software mouse cursor
 * ----------------------------------------------------------------------- */
#define CUR_W 12
#define CUR_H 19

/* Classic arrow.  'X' = black outline, '.' = white fill, ' ' = transparent. */
static const char *const g_cursor_bmp[CUR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..X X..X   ",
    "X.X  X..X   ",
    "XX    X..X  ",
    "X     X..X  ",
    "       X..X ",
    "       XXXX ",
};

static int    g_cx, g_cy;
static bool   g_cursor_shown;
static UINT32 g_cursor_under[CUR_W * CUR_H];

int WmCursorX(void) { return g_cx; }
int WmCursorY(void) { return g_cy; }

static void cursor_paint(void)
{
    for (int row = 0; row < CUR_H; row++) {
        const char *line = g_cursor_bmp[row];
        for (int col = 0; col < CUR_W; col++) {
            char p = line[col];
            if (p == 'X') GdiPutPixel(g_cx + col, g_cy + row, GDI_BLACK);
            else if (p == '.') GdiPutPixel(g_cx + col, g_cy + row, GDI_WHITE);
        }
    }
}

void WmCursorShow(int x, int y)
{
    g_cx = x;
    g_cy = y;
    GdiBlitSave(RECT(g_cx, g_cy, CUR_W, CUR_H), g_cursor_under);
    cursor_paint();
    g_cursor_shown = true;
}

void WmCursorHide(void)
{
    if (!g_cursor_shown) return;
    GdiBlitRestore(RECT(g_cx, g_cy, CUR_W, CUR_H), g_cursor_under);
    g_cursor_shown = false;
}

void WmCursorMove(int x, int y)
{
    int sw = GdiScreenW(), sh = GdiScreenH();
    if (x < 0) x = 0; if (x > sw - 1) x = sw - 1;
    if (y < 0) y = 0; if (y > sh - 1) y = sh - 1;
    WmCursorHide();
    WmCursorShow(x, y);
}

void WmCursorReshow(void)
{
    /* The scene was fully redrawn, so the previous save-under is stale and
     * the cursor was wiped.  Re-grab and redraw at the current position. */
    g_cursor_shown = false;
    WmCursorShow(g_cx, g_cy);
}
