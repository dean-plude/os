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
#include "../gdi/gdi.h"
#include "../ke/printf.h"
#include "../lib/string.h"

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

static bool g_start_open = true;
static bool g_ready;

/* User identity shown on the Start menu bar. */
static const char *USER_NAME  = "Dean Plude";
static const char *USER_GREET = "Good Morning!";
static const char *CLOCK_TIME = "12:00 PM";
static const char *CLOCK_DATE = "05/29/2026";

/* -----------------------------------------------------------------------
 * Math helpers
 * ----------------------------------------------------------------------- */

/* Integer sine, Bhaskara I approximation. Returns -1000..+1000.
 * sin(t) ≈ 4·t·(180−t) / (40500 − t·(180−t))  for t in degrees [0,180]. */
static int isin_milli(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    int sign = 1;
    if (deg > 180) { deg -= 180; sign = -1; }
    int t   = deg * (180 - deg);          /* 0 .. 8100 */
    int den = 40500 - t;                  /* 32400 .. 40500 */
    if (den == 0) den = 1;
    return sign * (4 * t * 1000) / den;   /* fits comfortably in int32 */
}

/* Filled right-pointing triangle (media "play" glyph): vertical left
 * edge, slanted edges converging to a tip at the vertical centre. */
static void play_glyph(int x, int y, int size, GdiColor c)
{
    int half = size / 2;
    if (half < 1) half = 1;
    for (int row = 0; row < size; row++) {
        int d = row < half ? half - row : row - half;
        int w = ((half - d) * size) / half;
        if (w > 0) GdiFillRect(RECT(x, y + row, w, 1), c);
    }
}

/* -----------------------------------------------------------------------
 * Tile helpers
 * ----------------------------------------------------------------------- */

/* Rounded, solid-color app tile with a centered short label. */
static void tile(int x, int y, int s, GdiColor bg, const char *label,
                 GdiColor fg)
{
    GdiRoundRect(RECT(x, y, s, s), s / 4, bg, GDI_TRANSPARENT);
    if (label && *label) {
        int ty = y + (s - GDI_FONT_H) / 2;
        GdiTextCenter(x, ty, s, label, fg);
    }
}

/* Rounded tile with a vertical gradient (used for the peek tiles). */
static void tile_grad(int x, int y, int w, int h, int rad,
                      GdiColor a, GdiColor b)
{
    GdiRoundGradV(RECT(x, y, w, h), rad, a, b);
}

/* -----------------------------------------------------------------------
 * Wallpaper
 * ----------------------------------------------------------------------- */
static void wave_layer(int sw, int sh, int base_pct, int amp_pct,
                       int phase, int period, GdiColor col)
{
    int base = (sh * base_pct) / 100;
    int amp  = (sh * amp_pct) / 100;
    for (int x = 0; x < sw; x++) {
        int ang = phase + (x * 360) / (period > 0 ? period : 1);
        int top = base + (amp * isin_milli(ang)) / 1000;
        if (top < 0) top = 0;
        if (top < sh)
            GdiFillRect(RECT(x, top, 1, sh - top), col);
    }
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

static void draw_desktop_icon(int cellx, int celly, IconKind k,
                              const char *label)
{
    draw_icon_emblem(cellx + 16, celly, k);
    GdiTextCenter(cellx, celly + 52, 80, label, TXT_LIGHT);
}

static void draw_desktop_icons(void)
{
    int x = 28;
    int y = 36;
    int dy = 96;
    draw_desktop_icon(x, y + dy * 0, ICON_PC,      "My Pc");
    draw_desktop_icon(x, y + dy * 1, ICON_DOCS,    "Documents");
    draw_desktop_icon(x, y + dy * 2, ICON_FOLDER,  "Personal");
    draw_desktop_icon(x, y + dy * 3, ICON_PROJECT, "Proyect");
    draw_desktop_icon(x, y + dy * 4, ICON_DRIVE,   "Files");
    draw_desktop_icon(x, y + dy * 5, ICON_PHOTOS,  "");
}

/* -----------------------------------------------------------------------
 * Start menu
 * ----------------------------------------------------------------------- */
static void draw_pinned_grid(int x, int y, int cell, int gap)
{
    struct { GdiColor bg; const char *l; GdiColor fg; const char *cap; } apps[8] = {
        { C_PHOTOSHOP, "Ps", GDI_C(0x3A,0xC8,0xF0), "Adobe" },
        { C_ILLUSTR,   "Ai", GDI_C(0xF0,0x8A,0x2A), "Adobe" },
        { C_PHOTOS,    "",   GDI_WHITE,             "Photos" },
        { C_CLIPCHAMP, "",   GDI_WHITE,             "ClipChamp" },
        { C_STORE,     "",   GDI_WHITE,             "Store" },
        { C_CALENDAR,  "11", GDI_C(0xD0,0x40,0x40), "Calendar" },
        { C_VS,        "VS", GDI_WHITE,             "VStudio" },
        { GDI_C(0xC8,0x5A,0xE0), "", GDI_WHITE,     "Visual" },
    };
    for (int i = 0; i < 8; i++) {
        int cx = x + (i % 4) * (cell + gap);
        int cy = y + (i / 4) * (cell + gap + 14);
        tile(cx, cy, cell, apps[i].bg, apps[i].l, apps[i].fg);
        GdiTextCenter(cx - 4, cy + cell + 2, cell + 8, apps[i].cap, TXT_DARK);
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

    /* Games + to-do row */
    int gy = py + 78;
    GdiRoundRect(RECT(x, gy, 56, 56), rad, GDI_C(0x12, 0x6A, 0x3A),
                 GDI_TRANSPARENT);
    GdiTextCenter(x, gy + 20, 56, "Solit.", GDI_WHITE);
    GdiRoundRect(RECT(x + 64, gy, 56, 56), rad, C_EDGE, GDI_TRANSPARENT);
    GdiTextCenter(x + 64, gy + 20, 56, "e", GDI_WHITE);
    GdiRoundRect(RECT(x + 128, gy, w - 128, 56), rad, GDI_C(0xF2, 0xDC, 0xCE),
                 GDI_TRANSPARENT);
    GdiTextBold(x + 138, gy + 6, "To do", C_EDGE);
    GdiTextT(x + 138, gy + 22, "Call mom", TXT_MUTED);
    GdiTextT(x + 138, gy + 36, "Buy milk", TXT_MUTED);
}

static void draw_recent_list(int x, int y, int w)
{
    struct { GdiColor c; const char *l; const char *n; } items[9] = {
        { C_PHOTOSHOP, "Ps", "Adobe Photoshop 2025" },
        { C_EDGE,      "e",  "Microsoft Edge" },
        { C_PAINT,     "",   "Paint" },
        { GDI_C(0xF2,0xD0,0x3A), "",  "Microsoft Tips" },
        { C_PPT,       "P",  "PowerPoint" },
        { C_SKYPE,     "S",  "Skype" },
        { C_BLENDER,   "",   "Blender" },
        { C_BING,      "b",  "Bing Search" },
        { C_ILLUSTR,   "Ai", "Adobe Illustrator 2025" },
    };
    GdiTextBold(x, y, "Recently Used", TXT_DARK);
    int iy = y + 22;
    for (int i = 0; i < 9; i++) {
        tile(x, iy, 26, items[i].c, items[i].l, GDI_WHITE);
        GdiTextT(x + 36, iy + 6, items[i].n, TXT_DARK);
        iy += 34;
    }
}

static void draw_start_menu(void)
{
    if (!g_start_open) return;
    int sw = GdiScreenW(), sh = GdiScreenH();

    int mw = 540; if (mw > sw - 60) mw = sw - 60;
    int mh = 600; if (mh > sh - 150) mh = sh - 150;
    int mx = (sw - mw) / 2;
    int my = (sh - mh) / 2 - 24;
    if (my < 40) my = 40;

    /* Two app tiles peeking above the menu top. */
    int pk_w = 84, pk_h = 96;
    tile_grad(mx + mw / 2 - pk_w - 8, my - pk_h + 28, pk_w, pk_h, 12,
              GDI_C(0x3A, 0x8A, 0xF0), GDI_C(0x2A, 0xC8, 0xC8));
    tile_grad(mx + mw / 2 + 8, my - pk_h + 28, pk_w, pk_h, 12,
              GDI_C(0xE0, 0x4A, 0xE0), GDI_C(0xF0, 0x9A, 0xD8));

    /* Frosted panel body with a soft drop shadow. */
    for (int s = 10; s >= 1; s--)
        GdiRoundAlpha(RECT(mx - s, my - s + 6, mw + 2 * s, mh + 2 * s),
                      24, GDI_C(0, 0, 0), 6);
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

    /* Live tiles below the grid */
    draw_live_tiles(left_x, hy + 22 + 2 * (46 + 14) + 16, left_w);

    /* Recently used list (right column) */
    draw_recent_list(right_x, hy, right_w);

    /* User / power bar */
    int by = my + mh - 52;
    GdiFillRect(RECT(mx + 16, by - 6, mw - 32, 1), GDI_C(0xD8, 0x9A, 0x88));
    GdiFillCircle(cx + 18, by + 18, 16, GDI_C(0x6A, 0x4A, 0xC8));
    GdiTextCenter(cx + 2, by + 11, 34, "DP", GDI_WHITE);
    GdiTextT(cx + 44, by + 6, USER_GREET, TXT_MUTED);
    GdiTextBold(cx + 44, by + 22, USER_NAME, TXT_DARK);

    /* power / help / add icons on the right */
    int ix = mx + mw - pad - 4;
    const char *gl[5] = { "(?)", "+", "[]", "#", "()" };
    for (int i = 0; i < 5; i++) {
        ix -= 30;
        GdiRoundRect(RECT(ix, by + 12, 24, 24), 6, GDI_C(0xF4, 0xCB, 0xAE),
                     GDI_TRANSPARENT);
        GdiTextCenter(ix, by + 16, 24, gl[i], TXT_DARK);
    }
}

/* -----------------------------------------------------------------------
 * Taskbar / dock
 * ----------------------------------------------------------------------- */
static void draw_dock(void)
{
    int sw = GdiScreenW(), sh = GdiScreenH();

    struct { GdiColor c; const char *l; } left[15] = {
        { GDI_C(0x6A,0x4A,0xC8), "DP" },
        { C_STORE,   "" },
        { GDI_C(0x5A,0x5A,0x64), "" },   /* settings */
        { C_EDGE,    "e" },
        { GDI_C(0x10,0x7C,0x10), "X" },  /* xbox */
        { C_PHOTOS,  "" },
        { GDI_WHITE, "11" },             /* calendar */
        { C_SKYPE,   "S" },
        { GDI_C(0xF6,0xCE,0x52), "" },   /* folder */
        { GDI_C(0xE8,0xC0,0xA0), "Q" },  /* search */
        { GDI_C(0x2A,0x8A,0xE0), "#" },  /* windows */
        { GDI_C(0x3A,0x6A,0xE0), "=" },  /* tasks */
        { GDI_C(0x9A,0xA2,0xB0), "U" },  /* usb */
        { GDI_C(0x6A,0xB0,0xE0), "@" },  /* cloud */
        { GDI_C(0xC8,0x9A,0x6A), "W" },  /* trash */
    };

    int n = 15;
    int icon = 36, gap = 8;
    int clock_w = 150;
    int dw = n * (icon + gap) + clock_w + 24;
    if (dw > sw - 24) dw = sw - 24;
    int dh = 56;
    int dx = (sw - dw) / 2;
    int dy = sh - dh - 14;

    /* Floating frosted dock */
    for (int s = 8; s >= 1; s--)
        GdiRoundAlpha(RECT(dx - s, dy - s + 4, dw + 2 * s, dh + 2 * s),
                      dh / 2, GDI_C(0, 0, 0), 7);
    GdiRoundGradV(RECT(dx, dy, dw, dh), dh / 2,
                  GDI_C(0xF0, 0xB8, 0x7A), DOCK_TINT);

    int x = dx + 14;
    int iy = dy + (dh - icon) / 2;
    for (int i = 0; i < n; i++) {
        tile(x, iy, icon, left[i].c, left[i].l,
             (left[i].c == GDI_WHITE) ? TXT_DARK : GDI_WHITE);
        x += icon + gap;
    }

    /* Right side: status + clock */
    int clock_x = dx + dw - clock_w - 8;
    GdiVLine(clock_x - 6, dy + 10, dy + dh - 10, GDI_C(0xD0, 0x90, 0x66));
    GdiTextT(clock_x, iy + 2, "^ <))", TXT_DARK);
    GdiTextBold(clock_x + 56, iy - 2, CLOCK_TIME, TXT_DARK);
    GdiTextT(clock_x + 56, iy + 14, CLOCK_DATE, TXT_DARK);
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
    g_start_open = true;
    WmSetDesktop(shell_background, shell_overlay);
    kprintf("[SHELL] Desktop shell ready (%dx%d), Start menu open\n",
            GdiScreenW(), GdiScreenH());
}

bool DesktopAvailable(void) { return g_ready; }

void DesktopToggleStart(void) { g_start_open = !g_start_open; }

void DesktopRender(void)
{
    if (!g_ready) return;
    WmComposite();
}
