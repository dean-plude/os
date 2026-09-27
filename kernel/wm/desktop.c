/*
 * desktop.c — NovaOS desktop shell
 *
 * A software-rendered reproduction of a modern Windows-style desktop:
 * gradient wallpaper with warm wave layers, left-column desktop icons,
 * a centered Start menu (search, pinned apps, live tiles, recently-used
 * list, user/power bar), and a floating rounded dock with a clock.
 *
 * Everything is drawn with the GDI software rasterizer.  Because we have
 * no image decoder yet, application "icons" are rendered as rounded,
 * brand-colored tiles bearing a short label — a faithful stand-in for the
 * real artwork.
 */

#include "desktop.h"
#include "wm.h"
#include "input.h"
#include "../gdi/gdi.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"
#include "../hal/ps2.h"
#include "../hal/rtc.h"
#include "../apps/apps.h"

/* -----------------------------------------------------------------------
 * Palette (sampled from the design mock)
 * ----------------------------------------------------------------------- */
#define SKY_TOP      GDI_C(0x39, 0x0C, 0x3C)
#define SKY_MID      GDI_C(0x6E, 0x1E, 0x55)
#define WAVE_1       GDI_C(0x86, 0x21, 0x44)   /* back maroon */
#define WAVE_2       GDI_C(0xA8, 0x2E, 0x3A)
#define WAVE_3       GDI_C(0xCB, 0x46, 0x2C)
#define WAVE_4       GDI_C(0xE6, 0x6C, 0x22)
#define WAVE_5       GDI_C(0xF2, 0x9C, 0x2F)
#define WAVE_6       GDI_C(0xF7, 0xC8, 0x44)   /* front gold */

#define GLASS_LIGHT  GDI_C(0xF2, 0xC9, 0xB0)   /* warm frosted panel */
#define GLASS_DARK   GDI_C(0xC8, 0x6A, 0x55)
#define DOCK_TINT    GDI_C(0xE8, 0xA9, 0x6B)
#define TXT_DARK     GDI_C(0x2A, 0x12, 0x20)
#define TXT_LIGHT    GDI_C(0xFB, 0xF2, 0xEC)
#define TXT_MUTED    GDI_C(0x6A, 0x3A, 0x40)

/* Brand-ish tile colors */
#define C_PHOTOSHOP  GDI_C(0x05, 0x1A, 0x2E)
#define C_ILLUSTR    GDI_C(0x2A, 0x12, 0x00)
#define C_PHOTOS     GDI_C(0x2E, 0xA0, 0x8A)
#define C_CLIPCHAMP  GDI_C(0x7A, 0x3C, 0xE0)
#define C_STORE      GDI_C(0x18, 0x6A, 0xD8)
#define C_CALENDAR   GDI_C(0xFF, 0xFF, 0xFF)
#define C_VS         GDI_C(0x6A, 0x2A, 0xC8)
#define C_EDGE       GDI_C(0x1A, 0x8A, 0xC8)
#define C_PAINT      GDI_C(0xF2, 0xC8, 0x3A)
#define C_PPT        GDI_C(0xD0, 0x4A, 0x28)
#define C_SKYPE      GDI_C(0x1E, 0x9A, 0xE0)
#define C_BLENDER    GDI_C(0xE8, 0x7A, 0x22)
#define C_BING       GDI_C(0x10, 0xA0, 0x88)

static bool g_start_open;         /* Start menu shown */
static bool g_ready;

/* Layout rectangles captured during the last composite (hit-testing) */
static GdiRect L_dock, L_menu;

/* Live clock strings, refreshed from the RTC each minute. */
static char g_clock_time[12] = "12:00 PM";
static char g_clock_date[12] = "01/01/2026";

static bool pt_in(GdiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* User identity shown on the Start menu bar. */
static const char *USER_NAME  = "Dean Plude";
static const char *USER_GREET = "Good Morning!";

/* -----------------------------------------------------------------------
 * Math helpers
 * ----------------------------------------------------------------------- */

/* Sine with millidegree input for smooth curves.  Returns sin * 2^16.
 * Bhaskara I approximation evaluated in 64-bit fixed point. */
static int isin_q16(INT64 mdeg)
{
    mdeg %= 360000;
    if (mdeg < 0) mdeg += 360000;
    int sign = 1;
    if (mdeg > 180000) { mdeg -= 180000; sign = -1; }
    INT64 t   = mdeg * (180000 - mdeg);            /* 0 .. 8.1e9 */
    INT64 den = (INT64)40500 * 1000000 - t;
    return sign * (int)((4 * t * 65536) / den);
}

/* Filled right-pointing triangle (media "play" glyph) */
static void play_glyph(int x, int y, int size, GdiColor c)
{
    GdiPoint tri[3] = {
        GDI_PT(x, y), GDI_PT(x, y + size), GDI_PT(x + size * 7 / 8, y + size / 2),
    };
    GdiFillPolygon(tri, 3, c);
}

/* -----------------------------------------------------------------------
 * Tile helpers
 * ----------------------------------------------------------------------- */

/* Rounded tile with a vertical gradient (used for the peek tiles). */
static void tile_grad(int x, int y, int w, int h, int rad,
                      GdiColor a, GdiColor b)
{
    GdiRoundGradV(RECT(x, y, w, h), rad, a, b);
}

/* -----------------------------------------------------------------------
 * Wallpaper
 * ----------------------------------------------------------------------- */
typedef struct { int base, amp, phase, period; } Wave;   /* logical px, degrees */

/* y of the wave's top edge at x; both in 1/256 logical px */
static int wave_curve(int x_256, void *ctx)
{
    const Wave *w = ctx;
    INT64 mdeg = (INT64)w->phase * 1000 +
                 ((INT64)x_256 * 360000) / ((INT64)w->period * 256);
    return w->base * 256 + (int)(((INT64)w->amp * 256 * isin_q16(mdeg)) >> 16);
}

static void wave_layer(int sw, int sh, int base_pct, int amp_pct,
                       int phase, int period, GdiColor col)
{
    Wave w = { (sh * base_pct) / 100, (sh * amp_pct) / 100, phase,
               period > 0 ? period : 1 };
    GdiFillUnderCurve(RECT(0, 0, sw, sh), wave_curve, &w, col);
}

static void draw_wallpaper(void)
{
    int sw = GdiScreenW(), sh = GdiScreenH();

    /* Sky gradient (top two-thirds) */
    GdiGradientV(RECT(0, 0, sw, sh), SKY_TOP, SKY_MID);

    /* Warm wave layers, back to front. */
    int period = (sw * 7) / 5;
    wave_layer(sw, sh, 58,  6,   0, period, WAVE_1);
    wave_layer(sw, sh, 66,  6, 110, period, WAVE_2);
    wave_layer(sw, sh, 73,  5, 220, period, WAVE_3);
    wave_layer(sw, sh, 80,  5,  60, period, WAVE_4);
    wave_layer(sw, sh, 87,  4, 190, period, WAVE_5);
    wave_layer(sw, sh, 93,  4, 320, period, WAVE_6);
}

/* -----------------------------------------------------------------------
 * Desktop icons (left column)
 * ----------------------------------------------------------------------- */
typedef enum {
    ICON_PC, ICON_DOCS, ICON_FOLDER, ICON_PROJECT, ICON_DRIVE, ICON_PHOTOS
} IconKind;

static void draw_icon_emblem(int x, int y, IconKind k)
{
    /* 48x48 emblem area at (x,y) */
    switch (k) {
    case ICON_PC: {
        /* monitor */
        GdiRoundGradV(RECT(x + 4, y + 2, 40, 28), 4,
                      GDI_C(0x9E, 0xD8, 0xF0), GDI_C(0x3A, 0x6A, 0xC8));
        GdiRoundRect(RECT(x + 4, y + 2, 40, 28), 4, GDI_TRANSPARENT,
                     GDI_C(0xC8, 0xD0, 0xE0));
        GdiFillRect(RECT(x + 18, y + 30, 12, 6), GDI_C(0x9A, 0xA2, 0xB0));
        GdiFillRect(RECT(x + 12, y + 36, 24, 4), GDI_C(0xC0, 0xC6, 0xD2));
        break; }
    case ICON_DOCS: {
        GdiRoundRect(RECT(x + 8, y + 2, 30, 40), 4,
                     GDI_C(0xF2, 0xF4, 0xF8), GDI_C(0xC0, 0xC4, 0xCC));
        for (int i = 0; i < 4; i++)
            GdiFillRect(RECT(x + 13, y + 10 + i * 7, 20, 2),
                        GDI_C(0x5A, 0x7A, 0xC8));
        break; }
    case ICON_FOLDER: {
        GdiFillRect(RECT(x + 4, y + 8, 18, 6), GDI_C(0xE0, 0xB0, 0x3A));
        GdiRoundGradV(RECT(x + 4, y + 12, 40, 28), 4,
                      GDI_C(0xF6, 0xCE, 0x52), GDI_C(0xE0, 0xA8, 0x2E));
        break; }
    case ICON_PROJECT: {
        GdiRoundRect(RECT(x + 2, y + 2, 44, 44), 8,
                     GDI_C(0x16, 0x12, 0x1C), GDI_TRANSPARENT);
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 2; c++)
                GdiRoundRect(RECT(x + 8 + c * 18, y + 8 + r * 18, 14, 12), 2,
                             GDI_C(0xF2, 0xC0, 0x44), GDI_TRANSPARENT);
        break; }
    case ICON_DRIVE: {
        GdiRoundGradV(RECT(x + 4, y + 6, 40, 34), 5,
                      GDI_C(0x8A, 0x92, 0xA0), GDI_C(0x5A, 0x62, 0x70));
        /* tiny window logo */
        GdiFillRect(RECT(x + 12, y + 16, 8, 8), GDI_C(0x4A, 0xC0, 0xF0));
        GdiFillRect(RECT(x + 22, y + 16, 8, 8), GDI_C(0x4A, 0xC0, 0xF0));
        GdiFillRect(RECT(x + 12, y + 26, 8, 8), GDI_C(0x4A, 0xC0, 0xF0));
        GdiFillRect(RECT(x + 22, y + 26, 8, 8), GDI_C(0x4A, 0xC0, 0xF0));
        break; }
    case ICON_PHOTOS: {
        GdiRoundRect(RECT(x + 4, y + 8, 22, 18), 2,
                     GDI_C(0xF0, 0xF0, 0xF0), GDI_TRANSPARENT);
        GdiRoundGradV(RECT(x + 12, y + 4, 28, 22), 2,
                      GDI_C(0x6A, 0xA0, 0xE0), GDI_C(0xE0, 0x9A, 0x6A));
        break; }
    }
}

/* -----------------------------------------------------------------------
 * Hotspots: clickable areas recorded while drawing
 *
 * Background hotspots (desktop icons) persist with the cached background;
 * overlay hotspots (dock, Start menu) are rebuilt every frame.
 * ----------------------------------------------------------------------- */
typedef enum {
    ACT_NONE, ACT_APP, ACT_START, ACT_ICON, ACT_FOLDER,
} ActKind;

typedef struct { GdiRect r; ActKind kind; int arg; const char *path; } Hot;

#define HOT_MAX 64
static Hot g_hot_bg[HOT_MAX], g_hot_ov[HOT_MAX];
static int g_hot_bg_n, g_hot_ov_n;

static void hot_add(Hot *list, int *n, GdiRect r, ActKind k, int arg, const char *path)
{
    if (*n < HOT_MAX) list[(*n)++] = (Hot){ r, k, arg, path };
}
#define HOT_BG(r, k, a, p)  hot_add(g_hot_bg, &g_hot_bg_n, (r), (k), (a), (p))
#define HOT_OV(r, k, a, p)  hot_add(g_hot_ov, &g_hot_ov_n, (r), (k), (a), (p))

static const Hot *hot_find(const Hot *list, int n, int x, int y)
{
    for (int i = n - 1; i >= 0; i--)           /* later = drawn on top */
        if (pt_in(list[i].r, x, y)) return &list[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * Desktop icons (left column)
 * ----------------------------------------------------------------------- */
static const struct {
    IconKind    kind;
    const char *label;
    ActKind     act;
    int         arg;
    const char *path;
} g_icons[] = {
    { ICON_PC,      "This PC",   ACT_APP,    APP_SETTINGS, NULL },
    { ICON_DOCS,    "Documents", ACT_FOLDER, 0, "\\Documents" },
    { ICON_FOLDER,  "Personal",  ACT_FOLDER, 0, "\\Personal" },
    { ICON_PROJECT, "Projects",  ACT_FOLDER, 0, "\\Projects" },
    { ICON_DRIVE,   "Files",     ACT_FOLDER, 0, "\\" },
    { ICON_PHOTOS,  "Pictures",  ACT_FOLDER, 0, "\\Pictures" },
};
#define N_ICONS ((int)(sizeof(g_icons) / sizeof(g_icons[0])))
static int g_icon_sel = -1;

static void draw_desktop_icons(void)
{
    g_hot_bg_n = 0;
    for (int i = 0; i < N_ICONS; i++) {
        int x = 28, y = 30 + i * 96;
        GdiRect cell = RECT(x, y - 4, 80, 80);
        if (i == g_icon_sel)
            GdiRoundAlpha(cell, 6, GDI_WHITE, 55);
        draw_icon_emblem(x + 16, y, g_icons[i].kind);
        GdiTextCenter(x, y + 54, 80, g_icons[i].label, TXT_LIGHT);
        HOT_BG(cell, ACT_ICON, i, NULL);
    }
}

static void open_icon(int i)
{
    if (g_icons[i].act == ACT_APP) AppLaunch((AppId)g_icons[i].arg);
    else AppOpenFolder(RamfsResolve(NULL, g_icons[i].path));
}

/* -----------------------------------------------------------------------
 * Start menu
 * ----------------------------------------------------------------------- */
static const AppId g_pinned[8] = {
    APP_TERMINAL, APP_EXPLORER, APP_NOTEPAD, APP_SETTINGS,
    APP_CALENDAR, APP_EDGE, APP_STORE, APP_PHOTOS,
};
static const char *g_pinned_cap[8] = {
    "Terminal", "Files", "Notepad", "Settings", "Calendar", "Edge", "Store", "Photos",
};

static void draw_pinned_grid(int x, int y, int cell, int gap)
{
    for (int i = 0; i < 8; i++) {
        int cx = x + (i % 4) * (cell + gap);
        /* row pitch = tile + caption line (2 + GDI_FONT_H) + 6px gap */
        int cy = y + (i / 4) * (cell + 2 + GDI_FONT_H + 6);
        AppDrawIcon(g_pinned[i], cx, cy, cell);
        GdiTextCenter(cx - 8, cy + cell + 2, cell + 16, g_pinned_cap[i], TXT_DARK);
        HOT_OV(RECT(cx - 6, cy - 4, cell + 12, cell + GDI_FONT_H + 8), ACT_APP, g_pinned[i], NULL);
    }
}

static void draw_live_tiles(int x, int y, int w)
{
    int rad = 10;

    /* Today / calendar */
    GdiRoundRect(RECT(x, y, w, 56), rad, GDI_C(0xF6, 0xDA, 0xCB),
                 GDI_TRANSPARENT);
    GdiRoundRect(RECT(x + 8, y + 8, 40, 40), 6, C_STORE, GDI_TRANSPARENT);
    GdiTextCenter(x + 8, y + 18, 40, "29", GDI_WHITE);
    GdiTextBold(x + 56, y + 8, "Today", TXT_DARK);
    GdiTextT(x + 56, y + 24, "Video call - 1:30 PM", TXT_MUTED);
    GdiTextT(x + 56, y + 38, "Birthday party - 2:00 PM", TXT_MUTED);
    HOT_OV(RECT(x, y, w, 56), ACT_APP, APP_CALENDAR, NULL);

    /* Weather + media row */
    int ry = y + 64;
    GdiRoundGradV(RECT(x, ry, 80, 48), rad,
                  GDI_C(0x7A, 0xC0, 0xF0), GDI_C(0x4A, 0x8A, 0xD8));
    GdiTextBold(x + 12, ry + 14, "19", GDI_WHITE);
    GdiFillCircle(x + 34, ry + 16, 3, GDI_WHITE);   /* degree dot */
    GdiTextT(x + 12, ry + 30, "Sunny", GDI_WHITE);

    GdiRoundRect(RECT(x + 88, ry, w - 88, 48), rad, GDI_C(0x2A, 0x1A, 0x22),
                 GDI_TRANSPARENT);
    GdiFillCircle(x + 88 + 24, ry + 24, 14, GDI_C(0xE0, 0x5A, 0x4A));
    play_glyph(x + 88 + 19, ry + 16, 16, GDI_WHITE);
    GdiTextT(x + 88 + 48, ry + 18, "Now Playing", TXT_LIGHT);

    /* Cloud storage */
    int cy = ry + 56;
    int ch = 52;
    GdiRoundRect(RECT(x, cy, w, ch), rad, GDI_C(0xF2, 0xCF, 0xBE),
                 GDI_TRANSPARENT);
    GdiFillCircle(x + 22, cy + 18, 10, GDI_C(0x6A, 0xB0, 0xE0));
    GdiTextBold(x + 40, cy + 6, "Cloud Storage", TXT_DARK);
    GdiRoundRect(RECT(x + 40, cy + 24, w - 60, 8), 4, GDI_C(0xD0, 0xA0, 0x90),
                 GDI_TRANSPARENT);
    GdiRoundRect(RECT(x + 40, cy + 24, (w - 60) * 64 / 100, 8), 4,
                 C_STORE, GDI_TRANSPARENT);
    GdiTextT(x + 40, cy + 36, "640 GB / 1 TB", TXT_MUTED);
    HOT_OV(RECT(x, cy, w, ch), ACT_APP, APP_SETTINGS, NULL);

    /* Photos strip */
    int py = cy + ch + 8;
    GdiRoundRect(RECT(x, py, w, 70), rad, GDI_C(0x20, 0x16, 0x1C),
                 GDI_TRANSPARENT);
    GdiTextT(x + 10, py + 6, "Photos", TXT_LIGHT);
    GdiRoundGradV(RECT(x + 10,        py + 22, (w - 50) / 3, 40), 4,
                  GDI_C(0x5A, 0xC0, 0xF0), GDI_C(0x2A, 0x6A, 0xC0));
    GdiRoundGradV(RECT(x + 20 + (w - 50) / 3, py + 22, (w - 50) / 3, 40), 4,
                  GDI_C(0x8A, 0xC8, 0x6A), GDI_C(0x3A, 0x8A, 0x4A));
    GdiRoundGradV(RECT(x + 30 + 2 * (w - 50) / 3, py + 22, (w - 50) / 3, 40), 4,
                  GDI_C(0xF0, 0xB0, 0x6A), GDI_C(0xC0, 0x6A, 0x3A));
    HOT_OV(RECT(x, py, w, 70), ACT_APP, APP_PHOTOS, NULL);

    /* Games + to-do row */
    int gy = py + 78;
    GdiRoundRect(RECT(x, gy, 56, 56), rad, GDI_C(0x12, 0x6A, 0x3A),
                 GDI_TRANSPARENT);
    GdiTextCenter(x, gy + 20, 56, "Solit.", GDI_WHITE);
    HOT_OV(RECT(x, gy, 56, 56), ACT_APP, APP_SOLITAIRE, NULL);
    GdiRoundRect(RECT(x + 64, gy, 56, 56), rad, C_EDGE, GDI_TRANSPARENT);
    GdiTextCenter(x + 64, gy + 20, 56, "e", GDI_WHITE);
    HOT_OV(RECT(x + 64, gy, 56, 56), ACT_APP, APP_EDGE, NULL);
    GdiRoundRect(RECT(x + 128, gy, w - 128, 56), rad, GDI_C(0xF2, 0xDC, 0xCE),
                 GDI_TRANSPARENT);
    GdiTextBold(x + 138, gy + 6, "To do", C_EDGE);
    GdiTextT(x + 138, gy + 22, "Call mom", TXT_MUTED);
    GdiTextT(x + 138, gy + 36, "Buy milk", TXT_MUTED);
    HOT_OV(RECT(x + 128, gy, w - 128, 56), ACT_APP, APP_TODO, NULL);
}

static void draw_recent_list(int x, int y, int w)
{
    /* Apps you opened, newest first, then suggestions */
    static const AppId suggest[9] = {
        APP_PHOTOSHOP, APP_EDGE, APP_PAINT, APP_TIPS, APP_POWERPOINT,
        APP_SKYPE, APP_BLENDER, APP_BING, APP_ILLUSTRATOR,
    };
    AppId items[9];
    int n = AppRecent(items, 9);
    for (int i = 0; i < 9 && n < 9; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++) if (items[j] == suggest[i]) dup = true;
        if (!dup) items[n++] = suggest[i];
    }

    GdiTextBold(x, y, "Recently Used", TXT_DARK);
    int iy = y + 22;
    for (int i = 0; i < n; i++) {
        AppDrawIcon(items[i], x, iy, 26);
        GdiTextT(x + 36, iy + 6, AppGetInfo(items[i])->name, TXT_DARK);
        HOT_OV(RECT(x - 4, iy - 3, w + 8, 32), ACT_APP, items[i], NULL);
        iy += 34;
    }
}

static void draw_start_menu(void)
{
    if (!g_start_open) { L_menu = RECT(0, 0, 0, 0); return; }
    int sw = GdiScreenW(), sh = GdiScreenH();

    int mw = 540; if (mw > sw - 60) mw = sw - 60;
    int mh = 612; if (mh > sh - 150) mh = sh - 150;
    int mx = (sw - mw) / 2;
    int my = (sh - mh) / 2 - 24;
    if (my < 40) my = 40;
    L_menu = RECT(mx, my, mw, mh);

    /* Two app tiles peeking above the menu top. */
    int pk_w = 84, pk_h = 96;
    tile_grad(mx + mw / 2 - pk_w - 8, my - pk_h + 28, pk_w, pk_h, 12,
              GDI_C(0x3A, 0x8A, 0xF0), GDI_C(0x2A, 0xC8, 0xC8));
    tile_grad(mx + mw / 2 + 8, my - pk_h + 28, pk_w, pk_h, 12,
              GDI_C(0xE0, 0x4A, 0xE0), GDI_C(0xF0, 0x9A, 0xD8));

    /* Frosted panel body with a soft drop shadow. */
    GdiDropShadow(RECT(mx, my + 6, mw, mh), 22, 18, 70);
    GdiRoundGradV(RECT(mx, my, mw, mh), 22, GLASS_LIGHT, GLASS_DARK);

    int pad = 18;
    int cx  = mx + pad;
    int cw  = mw - 2 * pad;

    /* Search box */
    GdiRoundRect(RECT(cx, my + pad, cw, 30), 15, GDI_C(0xFA, 0xEC, 0xE4),
                 GDI_TRANSPARENT);
    GdiFillCircle(cx + 16, my + pad + 15, 5, TXT_MUTED);
    GdiTextT(cx + 30, my + pad + 8, "Search apps, files and settings",
             TXT_MUTED);

    /* Column split */
    int left_x  = cx;
    int left_w  = (cw * 56) / 100;
    int right_x = cx + (cw * 60) / 100;
    int right_w = mw - pad - right_x + mx;

    /* Pinned header + see-all pill */
    int hy = my + pad + 44;
    GdiTextBold(left_x, hy, "Pinned Apps", TXT_DARK);
    GdiRoundRect(RECT(left_x + left_w - 52, hy - 3, 52, 18), 9,
                 GDI_C(0xF4, 0xCB, 0x8A), GDI_TRANSPARENT);
    GdiTextT(left_x + left_w - 44, hy, "see all", TXT_DARK);

    /* Pinned grid */
    draw_pinned_grid(left_x, hy + 22, 46, 18);

    /* Live tiles below the grid: two rows of 46px tiles + captions */
    draw_live_tiles(left_x, hy + 22 + 2 * (46 + 2 + GDI_FONT_H + 6), left_w);

    /* Recently used list (right column) */
    draw_recent_list(right_x, hy, right_w);

    /* User / power bar */
    int by = my + mh - 52;
    GdiFillRect(RECT(mx + 16, by - 6, mw - 32, 1), GDI_C(0xD8, 0x9A, 0x88));
    GdiFillCircle(cx + 18, by + 18, 16, GDI_C(0x6A, 0x4A, 0xC8));
    GdiTextCenter(cx + 2, by + 11, 34, "DP", GDI_WHITE);
    GdiTextT(cx + 44, by + 6, USER_GREET, TXT_MUTED);
    GdiTextBold(cx + 44, by + 22, USER_NAME, TXT_DARK);
    HOT_OV(RECT(cx, by, 200, 40), ACT_APP, APP_SETTINGS, NULL);

    /* Quick actions on the right: settings, files, terminal */
    static const AppId quick[3] = { APP_SETTINGS, APP_EXPLORER, APP_TERMINAL };
    int ix = mx + mw - pad - 4;
    for (int i = 0; i < 3; i++) {
        ix -= 34;
        GdiRoundRect(RECT(ix, by + 8, 28, 28), 7, GDI_C(0xF4, 0xCB, 0xAE),
                     GDI_TRANSPARENT);
        AppDrawIcon(quick[i], ix + 4, by + 12, 20);
        HOT_OV(RECT(ix, by + 8, 28, 28), ACT_APP, quick[i], NULL);
    }
}

/* -----------------------------------------------------------------------
 * Dock
 * ----------------------------------------------------------------------- */
#define DOCK_START   (-1)
#define DOCK_SEARCH  (-2)
static const int g_dock_items[] = {
    DOCK_START, DOCK_SEARCH, APP_TERMINAL, APP_EXPLORER, APP_NOTEPAD,
    APP_SETTINGS, APP_CALENDAR, APP_EDGE, APP_STORE, APP_PHOTOS, APP_XBOX,
    APP_SKYPE,
};
#define N_DOCK ((int)(sizeof(g_dock_items) / sizeof(g_dock_items[0])))
#define DOCK_ICON  36
#define DOCK_GAP   12
#define DOCK_CLOCK 132
#define DOCK_H     58

static int g_dock_hover = -1;

static GdiRect dock_rect(void)
{
    int sw = GdiScreenW(), sh = GdiScreenH();
    int dw = N_DOCK * (DOCK_ICON + DOCK_GAP) + DOCK_CLOCK + 28;
    if (dw > sw - 24) dw = sw - 24;
    return RECT((sw - dw) / 2, sh - DOCK_H - 12, dw, DOCK_H);
}

static GdiRect dock_item_rect(int i)
{
    GdiRect d = dock_rect();
    return RECT(d.x + 16 + i * (DOCK_ICON + DOCK_GAP), d.y + (DOCK_H - DOCK_ICON) / 2 - 3,
                DOCK_ICON, DOCK_ICON);
}

static void start_logo(int x, int y, int s)
{
    int q = (s - 3) / 2;
    GdiColor a = GDI_C(0x2A, 0x8A, 0xE8), b = GDI_C(0x5A, 0xC8, 0xF8);
    GdiRoundGradV(RECT(x, y, q, q), 2, b, a);
    GdiRoundGradV(RECT(x + q + 3, y, q, q), 2, b, a);
    GdiRoundGradV(RECT(x, y + q + 3, q, q), 2, b, a);
    GdiRoundGradV(RECT(x + q + 3, y + q + 3, q, q), 2, b, a);
}

static void search_glyph(int x, int y, int s, GdiColor c)
{
    int r = s * 30 / 100, cx = x + s * 44 / 100, cy = y + s * 44 / 100;
    GdiFillCircle(cx, cy, r, c);
    GdiFillCircle(cx, cy, r - 2, GDI_C(0xF3, 0xC0, 0x8A));
    GdiLine((GdiPoint){ (cx + r - 1) * 16, (cy + r - 1) * 16 },
            (GdiPoint){ (x + s * 86 / 100) * 16, (y + s * 86 / 100) * 16 }, 40, c);
}

static void draw_dock(void)
{
    GdiRect d = dock_rect();
    L_dock = d;

    GdiDropShadow(RECT(d.x, d.y + 4, d.w, d.h), d.h / 2, 12, 55);
    GdiRoundGradV(d, d.h / 2, GDI_C(0xF0, 0xB8, 0x7A), DOCK_TINT);

    WND *active = WmActiveWindow();
    for (int i = 0; i < N_DOCK; i++) {
        GdiRect r = dock_item_rect(i);
        int it = g_dock_items[i];
        bool start_on = (it == DOCK_START && g_start_open);
        if (i == g_dock_hover || start_on)
            GdiRoundAlpha(RECT(r.x - 5, r.y - 5, r.w + 10, r.h + 10), 8, GDI_WHITE, 80);

        if (it == DOCK_START)       start_logo(r.x + 7, r.y + 7, r.w - 14);
        else if (it == DOCK_SEARCH) search_glyph(r.x + 4, r.y + 4, r.w - 8, TXT_DARK);
        else                        AppDrawIcon((AppId)it, r.x + 2, r.y + 2, r.w - 4);

        /* Running indicator: a pill under the icon, wider when focused */
        if (it >= 0) {
            WND *w = WmFindApp(it);
            if (w) {
                bool focused = (w == active);
                int pw = focused ? 16 : 6;
                GdiRoundRect(RECT(r.x + (r.w - pw) / 2, r.y + r.h + 5, pw, 4), 2,
                             focused ? GDI_C(0x1A, 0x5A, 0xB8) : GDI_C(0x7A, 0x4A, 0x3A),
                             GDI_TRANSPARENT);
            }
        }
        HOT_OV(RECT(r.x - 6, d.y, r.w + 12, d.h), it == DOCK_START || it == DOCK_SEARCH
               ? ACT_START : ACT_APP, it, NULL);
    }

    /* Clock */
    int clock_x = d.x + d.w - DOCK_CLOCK - 8;
    GdiVLine(clock_x - 8, d.y + 12, d.y + d.h - 12, GDI_C(0xD0, 0x90, 0x66));
    GdiTextBold(clock_x + 8, d.y + 12, g_clock_time, TXT_DARK);
    GdiTextT(clock_x + 8, d.y + 30, g_clock_date, TXT_DARK);
    HOT_OV(RECT(clock_x, d.y, DOCK_CLOCK, d.h), ACT_APP, APP_CALENDAR, NULL);
}

/* -----------------------------------------------------------------------
 * Shell layers (registered with the WM)
 * ----------------------------------------------------------------------- */
static void shell_background(void)
{
    draw_wallpaper();
    draw_desktop_icons();
}

static void shell_overlay(void)
{
    g_hot_ov_n = 0;
    draw_start_menu();
    draw_dock();
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void DesktopInitialize(void)
{
    if (GdiScreenW() <= 0 || GdiScreenH() <= 0) {
        g_ready = false;
        kprintf("[SHELL] No framebuffer; desktop shell disabled\n");
        return;
    }
    g_ready = true;
    g_start_open = false;
    GdiRect d = dock_rect();
    WmSetWorkArea(RECT(0, 0, GdiScreenW(), d.y - 8));
    WmSetDesktop(shell_background, shell_overlay);
    kprintf("[SHELL] Desktop shell ready (%dx%d)\n", GdiScreenW(), GdiScreenH());
}

bool DesktopAvailable(void) { return g_ready; }

void DesktopToggleStart(void) { g_start_open = !g_start_open; WmInvalidate(); }

/* Refresh the dock clock strings from the RTC. */
static void update_clock(void)
{
    RtcTime t;
    rtc_read(&t);

    int h24 = t.hour;
    const char *ap = (h24 < 12) ? "AM" : "PM";
    int h12 = h24 % 12; if (h12 == 0) h12 = 12;
    ksnprintf(g_clock_time, sizeof(g_clock_time), "%d:%02u %s", h12, t.minute, ap);
    ksnprintf(g_clock_date, sizeof(g_clock_date), "%02u/%02u/%04u",
              t.month, t.day, t.year);
}

static void run_action(const Hot *h)
{
    switch (h->kind) {
    case ACT_APP:    AppLaunch((AppId)h->arg); break;
    case ACT_FOLDER: AppOpenFolder(RamfsResolve(NULL, h->path)); break;
    default: break;
    }
}

/* Dock clicks: Start/Search toggle the menu; apps focus, minimize or launch */
static void dock_click(const Hot *h)
{
    if (h->kind == ACT_START) { DesktopToggleStart(); return; }
    g_start_open = false;
    AppActivate((AppId)h->arg);
}

/* Handle a left press (or double-click) at (x, y), in stacking order:
 * Start menu, dock, windows, then the desktop itself. */
static void desktop_press(int x, int y, bool dbl)
{
    const Hot *h = hot_find(g_hot_ov, g_hot_ov_n, x, y);

    if (g_start_open && pt_in(L_menu, x, y)) {
        if (h) { g_start_open = false; run_action(h); }
        WmInvalidate();
        return;
    }
    if (pt_in(L_dock, x, y)) {
        if (h) dock_click(h);
        WmInvalidate();
        return;
    }
    if (g_start_open) {                    /* click-away closes the menu */
        g_start_open = false;
        WmInvalidate();
        return;
    }
    if (WmMouseButton(x, y, dbl ? WM_MOUSE_DBLCLK : WM_MOUSE_DOWN))
        return;

    const Hot *ic = hot_find(g_hot_bg, g_hot_bg_n, x, y);
    int sel = ic ? ic->arg : -1;
    if (sel != g_icon_sel) {
        g_icon_sel = sel;
        WmInvalidateBackground();
    }
    if (ic && dbl) open_icon(ic->arg);
}

static void desktop_hover(int x, int y)
{
    int hover = -1;
    if (!WmMouseCaptured() && pt_in(L_dock, x, y))
        for (int i = 0; i < N_DOCK; i++) {
            GdiRect r = dock_item_rect(i);
            if (pt_in(RECT(r.x - 6, L_dock.y, r.w + 12, L_dock.h), x, y)) hover = i;
        }
    if (hover != g_dock_hover) {
        g_dock_hover = hover;
        WmInvalidate();
    }
}

static void desktop_key(const KeyEvent *k)
{
    if (!k->pressed) return;
    if (k->extended && k->scancode == KEY_LWIN) { DesktopToggleStart(); return; }
    if (g_start_open) {
        if (k->scancode == KEY_ESC) { g_start_open = false; WmInvalidate(); }
        return;                             /* menu has no keyboard UI yet */
    }
    WmKey(k);
}

/* Window-manager + shell event loop (runs as the 'desktop' kernel thread).
 * Polls PS/2, moves the cursor with save-under, routes input, and
 * recomposites whenever something changed (or once per minute for the
 * clock). */
void DesktopRun(void *arg)
{
    (void)arg;
    if (!g_ready) return;

    update_clock();
    WmComposite();
    WmCursorShow(GdiScreenW() / 2, GdiScreenH() / 2);

    RtcTime t; rtc_read(&t);
    int    last_min  = t.minute;
    bool   prev_left = false;
    UINT64 last_press = 0;
    int    last_px = -100, last_py = -100;

    for (;;) {
        ps2_poll();

        InputEvent ev;
        while (InputPoll(&ev)) {
            if (ev.type == INPUT_MOUSE) {
                if (ev.dx || ev.dy) {
                    WmCursorMoveBy(ev.dx, ev.dy);
                    WmMouseMove(WmCursorX(), WmCursorY());
                    desktop_hover(WmCursorX(), WmCursorY());
                }
                bool left = (ev.buttons & MOUSE_LEFT) != 0;
                int  x = WmCursorX(), y = WmCursorY();
                if (left && !prev_left) {
                    UINT64 now = sched_ticks();
                    bool dbl = now - last_press <= 45 &&
                               x - last_px <= 4 && last_px - x <= 4 &&
                               y - last_py <= 4 && last_py - y <= 4;
                    last_press = dbl ? 0 : now;     /* no triple-click */
                    last_px = x; last_py = y;
                    desktop_press(x, y, dbl);
                } else if (!left && prev_left) {
                    WmMouseButton(x, y, WM_MOUSE_UP);
                }
                prev_left = left;
            } else if (ev.type == INPUT_KEY) {
                KeyEvent k;
                if (InputTranslateKey(&ev, &k)) desktop_key(&k);
            }
        }

        rtc_read(&t);
        if (t.minute != last_min) {
            last_min = t.minute;
            update_clock();
            WmInvalidate();
        }

        if (WmNeedsRedraw()) {
            WmComposite();
            WmCursorReshow();
        }
        sched_yield();
    }
}

void DesktopRender(void)
{
    if (!g_ready) return;
    update_clock();
    WmComposite();
}
