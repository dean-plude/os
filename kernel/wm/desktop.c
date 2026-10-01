/*
 * desktop.c — NovaOS desktop shell
 *
 * A software-rendered Windows-11-style desktop:
 *
 *   - wallpaper: a sky gradient with wave layers, in one of a few themes
 *     (Settings > Personalization)
 *   - desktop icons; double-click opens, right-click shows a menu
 *   - the dock (frosted glass): Start, search, pinned apps with running
 *     indicators, a button for each other window, tooltips; and the tray
 *     (network status and clock) as its own block at the right
 *   - the Start menu: type to search apps, programs, settings pages,
 *     folders and files (arrow keys + Enter); otherwise pinned apps,
 *     installed programs, recent apps and documents, and a power menu
 *   - context menus (desktop, icons, dock, title bars)
 *   - Alt+Tab switcher; Win (Start), Win+E, Win+D, Win+arrows (snap)
 *
 * Everything is drawn with the GDI software rasterizer, on the desktop
 * thread, which also owns input.
 */

#include "../um/um.h"
#include "desktop.h"
#include "../hal/display.h"
#include "../fs/setup.h"
#include "wm.h"
#include "input.h"
#include "../gdi/gdi.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../lib/string.h"
#include "../hal/ps2.h"
#include "../hal/rtc.h"
#include "../hal/acpi.h"
#include "../ke/sleep.h"
#include "../arch/x86_64/cpu.h"
#include "../apps/apps.h"
#include "../net/net.h"

/* -----------------------------------------------------------------------
 * Themes
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *name;
    GdiColor sky_top, sky_mid;
    GdiColor wave[6];                  /* back to front */
} Theme;

static const Theme g_themes[] = {
    { "Sunset",
      GDI_C(0x39, 0x0C, 0x3C), GDI_C(0x6E, 0x1E, 0x55),
      { GDI_C(0x86, 0x21, 0x44), GDI_C(0xA8, 0x2E, 0x3A), GDI_C(0xCB, 0x46, 0x2C),
        GDI_C(0xE6, 0x6C, 0x22), GDI_C(0xF2, 0x9C, 0x2F), GDI_C(0xF7, 0xC8, 0x44) } },
    { "Ocean",
      GDI_C(0x06, 0x1E, 0x3C), GDI_C(0x12, 0x4A, 0x7A),
      { GDI_C(0x16, 0x5A, 0x8C), GDI_C(0x1A, 0x72, 0xA0), GDI_C(0x20, 0x8C, 0xB0),
        GDI_C(0x2C, 0xA6, 0xBE), GDI_C(0x5A, 0xC4, 0xCC), GDI_C(0x9C, 0xE0, 0xDA) } },
    { "Twilight",
      GDI_C(0x0C, 0x0A, 0x1E), GDI_C(0x24, 0x1C, 0x46),
      { GDI_C(0x34, 0x26, 0x62), GDI_C(0x46, 0x30, 0x7E), GDI_C(0x5A, 0x3C, 0x98),
        GDI_C(0x72, 0x4A, 0xB0), GDI_C(0x90, 0x5E, 0xC4), GDI_C(0xB4, 0x7C, 0xD6) } },
};
#define N_THEMES ((int)(sizeof(g_themes) / sizeof(g_themes[0])))
static int g_theme;
#define TH (&g_themes[g_theme])

/* The shell's glass (Start menu, dock, tray): neutral dark acrylic that
 * blurs whatever is behind it, so it sits well on any wallpaper */
#define GLASS_TINT   GDI_C(0x1C, 0x1C, 0x20)
#define GLASS_ALPHA  200
#define GLASS_BLUR   18                 /* logical px */
#define EDGE_ALPHA   40                 /* white hairline around glass */
#define SH_TEXT      GDI_C(0xF3, 0xF3, 0xF5)
#define SH_TEXT2     GDI_C(0xB0, 0xB0, 0xB8)
#define SH_LINE      GDI_C(0x4E, 0x4E, 0x56)
#define SH_FIELD     GDI_C(0x16, 0x16, 0x1A)
#define SH_SELECT    GDI_C(0x40, 0x40, 0x48)
#define HOVER_ALPHA  26                 /* white wash under the pointer */
#define TXT_LIGHT    GDI_C(0xFF, 0xFF, 0xFF)
#define ACCENT       UI_ACCENT

static void glass(GdiRect r, int rad)
{
    GdiBackdrop(r, rad, GLASS_BLUR, GLASS_TINT, GLASS_ALPHA);
    GdiRoundBorderAlpha(r, rad, GDI_WHITE, EDGE_ALPHA);
}

/* Dark popups (context menus, tooltips, Alt+Tab), as in Windows 11 */
#define POP_BG       GDI_C(0x2B, 0x2B, 0x2B)
#define POP_LINE     GDI_C(0x40, 0x40, 0x40)
#define POP_HOVER    GDI_C(0x3D, 0x3D, 0x3D)
#define POP_TEXT     GDI_C(0xF2, 0xF2, 0xF2)
#define POP_TEXT2    GDI_C(0xA8, 0xA8, 0xA8)

static bool g_ready;

/* User identity shown on the Start menu bar. */
static const char *USER_NAME = "Dean Plude";

static bool pt_in(GdiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Case-insensitive: does @hay contain @needle?  Returns the offset or -1 */
static int ci_find(const char *hay, const char *needle)
{
    if (!*needle) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && (hay[i + j] | 0x20) == (needle[j] | 0x20)) j++;
        if (!needle[j]) return i;
    }
    return -1;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && ci_find(s + n - m, suffix) == 0;
}

/* Copy @s into @out, shortened with "..." to fit @maxw logical px */
static void fit_text(const char *s, int maxw, char *out, int cap, bool bold)
{
    strncpy(out, s, (size_t)cap - 1);
    out[cap - 1] = '\0';
    int (*width)(const char *) = bold ? GdiTextBoldW : GdiTextW;
    if (width(out) <= maxw) return;
    int n = (int)strlen(out);
    while (n > 0) {
        out[--n] = '\0';
        if (n + 3 < cap) {
            char tmp[128];
            ksnprintf(tmp, sizeof(tmp), "%s...", out);
            if (width(tmp) <= maxw) { strncpy(out, tmp, (size_t)cap - 1); out[cap - 1] = '\0'; return; }
        }
    }
}

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

static void draw_wallpaper_in(const Theme *t, GdiRect r)
{
    GdiGradientV(r, t->sky_top, t->sky_mid);
    static const int base[6]  = { 58, 66, 73, 80, 87, 93 };
    static const int amp[6]   = { 6, 6, 5, 5, 4, 4 };
    static const int phase[6] = { 0, 110, 220, 60, 190, 320 };
    for (int i = 0; i < 6; i++) {
        Wave w = { r.y + (r.h * base[i]) / 100, (r.h * amp[i]) / 100, phase[i],
                   (r.w * 7) / 5 > 0 ? (r.w * 7) / 5 : 1 };
        GdiFillUnderCurve(r, wave_curve, &w, t->wave[i]);
    }
}

/* -----------------------------------------------------------------------
 * Hotspots: clickable areas recorded while drawing
 *
 * Background hotspots (desktop icons) persist with the cached background;
 * overlay hotspots (dock, Start menu, menus) are rebuilt every frame.
 * ----------------------------------------------------------------------- */
typedef enum {
    ACT_NONE, ACT_APP, ACT_START, ACT_SEARCH, ACT_ICON, ACT_TASK, ACT_CLOCK,
    ACT_PROG, ACT_RECENT, ACT_RESULT, ACT_POWER, ACT_MENU, ACT_SWALLOW, ACT_NET,
} ActKind;

typedef struct { GdiRect r; ActKind kind; int arg; } Hot;

#define HOT_MAX 96
static Hot g_hot_bg[HOT_MAX], g_hot_ov[HOT_MAX];
static int g_hot_bg_n, g_hot_ov_n;

static void hot_add(Hot *list, int *n, GdiRect r, ActKind k, int arg)
{
    if (*n < HOT_MAX) list[(*n)++] = (Hot){ r, k, arg };
}
#define HOT_BG(r, k, a)  hot_add(g_hot_bg, &g_hot_bg_n, (r), (k), (a))
#define HOT_OV(r, k, a)  hot_add(g_hot_ov, &g_hot_ov_n, (r), (k), (a))

static const Hot *hot_find(const Hot *list, int n, int x, int y)
{
    for (int i = n - 1; i >= 0; i--)           /* later = drawn on top */
        if (pt_in(list[i].r, x, y)) return &list[i];
    return NULL;
}

/* The overlay hotspot under the pointer (for hover highlights) */
static ActKind g_hover_kind;
static int     g_hover_arg;
static bool hovered(ActKind k, int arg) { return g_hover_kind == k && g_hover_arg == arg; }

/* -----------------------------------------------------------------------
 * Desktop icons
 * ----------------------------------------------------------------------- */
typedef enum { ICON_PC, ICON_DOCS, ICON_FOLDER, ICON_PROJECT, ICON_PHOTOS, ICON_APP } IconKind;

static const struct {
    IconKind    kind;
    const char *label;
    const char *path;          /* folder to open, or NULL for an app */
    int         app;
} g_icons[] = {
    { ICON_PC,      "This PC",   "\\",           -1 },
    { ICON_DOCS,    "Documents", "\\Documents",  -1 },
    { ICON_FOLDER,  "Downloads", "\\Downloads",  -1 },
    { ICON_PHOTOS,  "Pictures",  "\\Pictures",   -1 },
    { ICON_PROJECT, "Projects",  "\\Projects",   -1 },
    { ICON_APP,     "NetSurf",   NULL,           APP_NETSURF },
    { ICON_APP,     "Install NovaOS", NULL,      APP_SETUP },   /* last: only on the installation disc */
};
/* the installer's icon shows when running from the installation disc */
#define N_ICONS ((int)(sizeof(g_icons) / sizeof(g_icons[0])) - (SetupIsLive() ? 0 : 1))
static int g_icon_sel = -1;

static void draw_icon_emblem(int x, int y, int i)
{
    /* 48x48 emblem area at (x,y), in the shared icon style */
    switch (g_icons[i].kind) {
    case ICON_PC:      AppDrawPcIcon(x, y, 48); break;
    case ICON_DOCS:    AppDrawFolderKindIcon(FOLDER_DOCUMENTS, x, y, 48); break;
    case ICON_FOLDER:  AppDrawFolderKindIcon(FOLDER_DOWNLOADS, x, y, 48); break;
    case ICON_PHOTOS:  AppDrawFolderKindIcon(FOLDER_PICTURES, x, y, 48); break;
    case ICON_PROJECT: AppDrawFolderKindIcon(FOLDER_PROJECTS, x, y, 48); break;
    case ICON_APP:     AppDrawIcon((AppId)g_icons[i].app, x + 3, y + 3, 42); break;
    }
}

/* What is in C:\\Desktop (shortcuts installers made, files saved there)
 * follows the built-in icons, rescanned whenever the desktop is drawn */
#define MAX_DESK_FILES 24
static char g_dfile[MAX_DESK_FILES][RAMFS_PATH_MAX];
static int g_ndfiles;

static void scan_desktop_files(void)
{
    g_ndfiles = 0;
    RamNode *d = RamfsResolve(NULL, "\\Desktop");
    for (RamNode *c = d && d->dir ? d->child : NULL; c && g_ndfiles < MAX_DESK_FILES; c = c->next)
        if (c->name[0] != '.' && strcmp(c->name, "desktop.ini"))
            RamfsPath(c, g_dfile[g_ndfiles++], RAMFS_PATH_MAX);
}

static UINT64 desktop_files_signature(void)
{
    UINT64 sig = 1469598103934665603ULL;
    RamNode *d = RamfsResolve(NULL, "\\Desktop");
    for (RamNode *c = d && d->dir ? d->child : NULL; c; c = c->next) {
        sig = (sig ^ (UINT64)(uintptr_t)c) * 1099511628211ULL;
        for (const char *n = c->name; *n; n++) sig = (sig ^ (UINT8)*n) * 1099511628211ULL;
    }
    return sig;
}

static void icon_cell(int i, int *x, int *y)
{
    int rows = (GdiScreenH() - 24 - 120) / 96;          /* clear of the dock */
    if (rows < 1) rows = 1;
    *x = 20 + (i / rows) * 100;
    *y = 24 + (i % rows) * 96;
}

static void draw_desktop_icons(void)
{
    g_hot_bg_n = 0;
    scan_desktop_files();
    for (int i = 0; i < N_ICONS + g_ndfiles; i++) {
        int x, y;
        icon_cell(i, &x, &y);
        GdiRect cell = RECT(x, y - 6, 88, 84);
        if (i == g_icon_sel) GdiRoundAlpha(cell, 6, GDI_WHITE, 38);   /* soft, no outline */
        char label[48];
        if (i < N_ICONS) {
            draw_icon_emblem(x + 20, y, i);
            strncpy(label, g_icons[i].label, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        } else {
            RamNode *n = RamfsResolve(NULL, g_dfile[i - N_ICONS]);
            if (!n) continue;
            AppDrawNodeIcon(n, x + 20, y, 48);
            char name[RAMFS_NAME_MAX];
            strncpy(name, n->name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char *dot = strrchr(name, '.');
            if (dot && ends_with_ci(dot, ".lnk")) *dot = '\0';   /* shortcuts go by their name */
            fit_text(name, 86, label, sizeof(label), false);
        }
        /* a soft blurred shadow keeps labels readable on any wallpaper */
        GdiTextShadowCenter(x, y + 54, 88, label, TXT_LIGHT, 205);
        HOT_BG(cell, ACT_ICON, i);
    }
}

static void open_icon(int i)
{
    if (i >= N_ICONS) {
        RamNode *n = i - N_ICONS < g_ndfiles ? RamfsResolve(NULL, g_dfile[i - N_ICONS]) : NULL;
        if (n && n->dir) AppOpenFolder(n);
        else if (n) AppOpenFile(n);
        return;
    }
    if (g_icons[i].path) AppOpenFolder(RamfsResolve(NULL, g_icons[i].path));
    else AppLaunch((AppId)g_icons[i].app);
}

/* -----------------------------------------------------------------------
 * Context menus
 * ----------------------------------------------------------------------- */
typedef enum {
    MA_NONE, MA_OPEN_ICON, MA_EXPLORER_AT, MA_TERMINAL_AT, MA_SETTINGS_PAGE, MA_APP,
    MA_CLOSE_APP, MA_WIN_CLOSE, MA_WIN_MIN, MA_WIN_MAX, MA_WIN_RESTORE, MA_WIN_SNAP_L,
    MA_WIN_SNAP_R, MA_SHOW_DESKTOP, MA_THEME_NEXT, MA_SLEEP, MA_RESTART, MA_SHUTDOWN,
} MenuAct;

typedef struct {
    char    label[40];
    MenuAct act;
    int     arg;
    char    path[RAMFS_PATH_MAX];
    bool    sep;               /* separator line above */
} MenuItem;

#define MENU_MAX   10
#define MENU_W     232
#define MENU_ROW   32
static struct {
    bool     open;
    int      x, y;
    int      n;
    MenuItem it[MENU_MAX];
} g_menu;
static GdiRect L_menu_box;

static void menu_begin(int x, int y) { g_menu.open = true; g_menu.x = x; g_menu.y = y; g_menu.n = 0; }

static void menu_add(const char *label, MenuAct act, int arg, const char *path, bool sep)
{
    if (g_menu.n >= MENU_MAX) return;
    MenuItem *m = &g_menu.it[g_menu.n++];
    strncpy(m->label, label, sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
    m->act = act;
    m->arg = arg;
    m->path[0] = '\0';
    if (path) { strncpy(m->path, path, sizeof(m->path) - 1); m->path[sizeof(m->path) - 1] = '\0'; }
    m->sep = sep;
}

static void draw_menu(void)
{
    if (!g_menu.open) { L_menu_box = RECT(0, 0, 0, 0); return; }
    int seps = 0;
    for (int i = 0; i < g_menu.n; i++) if (g_menu.it[i].sep) seps++;
    int h = 8 + g_menu.n * MENU_ROW + seps * 9;
    int x = g_menu.x, y = g_menu.y;
    if (x + MENU_W > GdiScreenW() - 4) x = GdiScreenW() - 4 - MENU_W;
    if (y + h > GdiScreenH() - 4) y = y - h;
    if (y < 4) y = 4;
    if (x < 4) x = 4;
    L_menu_box = RECT(x, y, MENU_W, h);
    GdiDropShadow(RECT(x, y + 3, MENU_W, h), 8, 14, 80);
    GdiRoundRect(L_menu_box, 8, POP_BG, POP_LINE);
    HOT_OV(L_menu_box, ACT_SWALLOW, 0);
    int iy = y + 4;
    for (int i = 0; i < g_menu.n; i++) {
        if (g_menu.it[i].sep) {
            GdiFillRect(RECT(x + 10, iy + 4, MENU_W - 20, 1), POP_LINE);
            iy += 9;
        }
        GdiRect row = RECT(x + 4, iy, MENU_W - 8, MENU_ROW);
        if (hovered(ACT_MENU, i)) GdiRoundRect(row, 5, POP_HOVER, GDI_TRANSPARENT);
        GdiTextT(x + 16, iy + (MENU_ROW - GDI_FONT_H) / 2, g_menu.it[i].label, POP_TEXT);
        HOT_OV(row, ACT_MENU, i);
        iy += MENU_ROW;
    }
}

/* -----------------------------------------------------------------------
 * Power: the Start menu, the power button and NtShutdownSystem/ExitWindowsEx
 * all end up here, on the desktop thread, which owns the screen and drive C:
 * ----------------------------------------------------------------------- */
static volatile UINT64 g_desktop_beat;             /* the desktop loop's last pass */
static volatile int    g_power_req;                /* POWER_* from another thread */
static volatile UINT32 g_sleeps;                   /* sleep requests carried out */
static volatile bool   g_slept;                    /* ... and whether the last one slept */
static volatile bool   g_sleeping;                 /* (the watchdog's clock jumps meanwhile) */

static void power_screen(const char *msg)
{
    WmCursorHide();
    GdiFillRect(RECT(0, 0, GdiScreenW(), GdiScreenH()), 0x0B1E3A);
    GdiTextShadowCenter(0, GdiScreenH() / 2 - GDI_FONT_H / 2, GdiScreenW(), msg, 0xFFFFFF, 0);
    GdiPresent();
}

static void power_restart(void)
{
    kprintf("[SHELL] Restarting\n");
    if (g_ready) power_screen("Restarting");
    UmSaveAll();
    AcpiReset();
}

void DesktopRestart(void) { power_restart(); }

static void power_shutdown(void)
{
    kprintf("[SHELL] Shutting down\n");
    if (g_ready) power_screen("Shutting down");
    UmSaveAll();
    AcpiPowerOff();
    kprintf("[SHELL] The machine didn't power off\n");
    if (g_ready) power_screen("It's now safe to turn off your computer");
    cli();
    for (;;) hlt();
}

/* Sleep (S3): drive C: is saved first, in case the machine never wakes */
static bool power_sleep(void)
{
    kprintf("[SHELL] Sleeping\n");
    UmSaveAll();
    g_sleeping = true;
    bool ok = SleepEnter();
    sched_sleep_tick();                           /* the tick count catches up */
    g_desktop_beat = sched_ticks();
    g_sleeping = false;
    if (ok) {
        WmCursorHide();
        WmInvalidateBackground();
        WmInvalidate();
        WmCursorShow(WmCursorX(), WmCursorY());
    } else {
        kprintf("[SHELL] Sleep failed\n");
    }
    return ok;
}

bool DesktopPowerRequest(int what)
{
    if (!g_desktop_beat) {                        /* no desktop loop: do it here */
        DesktopLock();
        if (what == POWER_SLEEP) { bool ok = power_sleep(); DesktopUnlock(); return ok; }
        if (what == POWER_RESTART) power_restart(); else power_shutdown();
    }
    UINT32 n = g_sleeps;
    __atomic_store_n(&g_power_req, what, __ATOMIC_RELEASE);
    if (what != POWER_SLEEP) return true;
    while (g_sleeps == n) sched_sleep_tick();     /* until the machine is awake again */
    return g_slept;
}

/* The desktop loop: a pending request, or the power button (which shuts
 * down, as Windows does by default) */
static void power_poll(void)
{
    if (AcpiPowerButtonPressed()) {
        kprintf("[SHELL] Power button pressed\n");
        power_shutdown();
    }
    int req = __atomic_exchange_n(&g_power_req, POWER_NONE, __ATOMIC_ACQ_REL);
    if (req == POWER_RESTART) power_restart();
    else if (req == POWER_SHUTDOWN) power_shutdown();
    else if (req == POWER_SLEEP) {
        g_slept = power_sleep();
        __atomic_fetch_add(&g_sleeps, 1, __ATOMIC_RELEASE);
    }
}

static void run_menu_item(const MenuItem *m)
{
    WND *w = (m->act >= MA_WIN_CLOSE && m->act <= MA_WIN_SNAP_R) ? WmWindowById(m->arg) : NULL;
    switch (m->act) {
    case MA_OPEN_ICON:      open_icon(m->arg); break;
    case MA_EXPLORER_AT:    AppOpenFolder(RamfsResolve(NULL, m->path)); break;
    case MA_TERMINAL_AT:    TerminalRun(NULL, RamfsResolve(NULL, m->path)); break;
    case MA_SETTINGS_PAGE:  SettingsOpenPage(m->arg); break;
    case MA_APP:            AppLaunch((AppId)m->arg); break;
    case MA_CLOSE_APP: {
        WND *aw = WmFindApp(m->arg);
        if (aw) WmRequestClose(aw);
        break; }
    case MA_WIN_CLOSE:      if (w) WmRequestClose(w); break;
    case MA_WIN_MIN:        if (w) WmMinimize(w); break;
    case MA_WIN_MAX:        if (w) { WmSetActive(w); WmSnap(w, WM_SNAP_MAX); } break;
    case MA_WIN_RESTORE:    if (w) { WmSetActive(w); WmSnap(w, WM_SNAP_RESTORE); } break;
    case MA_WIN_SNAP_L:     if (w) { WmSetActive(w); WmSnap(w, WM_SNAP_LEFT); } break;
    case MA_WIN_SNAP_R:     if (w) { WmSetActive(w); WmSnap(w, WM_SNAP_RIGHT); } break;
    case MA_SHOW_DESKTOP:   WmShowDesktopToggle(); break;
    case MA_THEME_NEXT:     DesktopSetTheme((g_theme + 1) % N_THEMES); break;
    case MA_SLEEP:          power_sleep(); break;
    case MA_RESTART:        power_restart(); break;
    case MA_SHUTDOWN:       power_shutdown(); break;
    default: break;
    }
}

/* -----------------------------------------------------------------------
 * Start menu
 * ----------------------------------------------------------------------- */
static bool g_start_open;
static GdiRect L_start;
static char g_query[48];
static int  g_query_len;
static int  g_sel;                     /* selected search result */

static const AppId g_pinned[] = {
    APP_TERMINAL, APP_EXPLORER, APP_NOTEPAD, APP_SETTINGS, APP_CALENDAR, APP_NETSURF,
    APP_STORE,
};
#define N_PINNED ((int)(sizeof(g_pinned) / sizeof(g_pinned[0])))
static const char *g_pinned_cap[N_PINNED] = {
    "Terminal", "Files", "Notepad", "Settings", "Calendar", "NetSurf", "Store",
};

/* Programs installed in C:\Programs (rescanned when the menu opens) */
#define MAX_PROGS 64
static struct { char name[32]; char path[RAMFS_PATH_MAX]; bool hidden, link; } g_progs[MAX_PROGS];
static int g_nprogs;

/* Shortcuts installers put in the Start menu's Programs folder (and one
 * level of folders below it) */
#define START_MENU_PROGRAMS "\\AppData\\Roaming\\Start Menu\\Programs"
static void scan_links(RamNode *dir, int depth)
{
    for (RamNode *c = dir ? dir->child : NULL; c && g_nprogs < MAX_PROGS; c = c->next) {
        if (c->dir) { if (depth < 1) scan_links(c, depth + 1); continue; }
        if (!ends_with_ci(c->name, ".lnk") || !AppLinkTarget(c)) continue;
        strncpy(g_progs[g_nprogs].name, c->name, sizeof(g_progs[0].name) - 1);
        g_progs[g_nprogs].name[sizeof(g_progs[0].name) - 1] = '\0';
        char *dot = strrchr(g_progs[g_nprogs].name, '.');
        if (dot && ends_with_ci(dot, ".lnk")) *dot = '\0';
        RamfsPath(c, g_progs[g_nprogs].path, sizeof(g_progs[0].path));
        const char *n = g_progs[g_nprogs].name;
        g_progs[g_nprogs].link = true;
        g_progs[g_nprogs].hidden = !strncmp(n, "Uninstall", 9) || !strncmp(n, "uninstall", 9);
        g_nprogs++;
    }
}

static void draw_prog_icon(int i, int x, int y, int s)
{
    RamNode *n = g_progs[i].link ? RamfsResolve(NULL, g_progs[i].path) : NULL;
    if (n) AppDrawNodeIcon(n, x, y, s);
    else AppDrawProgramIcon(g_progs[i].name, x, y, s);
}

static void scan_programs(void)
{
    g_nprogs = 0;
    scan_links(RamfsResolve(NULL, START_MENU_PROGRAMS), 0);  /* installed apps first */
    RamNode *pd = RamfsResolve(NULL, "\\Programs");
    for (RamNode *c = pd ? pd->child : NULL; c && g_nprogs < MAX_PROGS; c = c->next) {
        char exe[RAMFS_NAME_MAX + 8];
        RamNode *node = NULL;
        if (c->dir) {                                    /* C:\Programs\NAME\NAME.exe */
            ksnprintf(exe, sizeof(exe), "%s.exe", c->name);
            node = RamfsFind(c, exe);
        } else if (ends_with_ci(c->name, ".exe")) {
            node = c;
        }
        if (!node || node->dir) continue;
        int app = AppForProgram(node->name);
        if (app >= 0) continue;                          /* has its own tile */
        strncpy(g_progs[g_nprogs].name, c->name, sizeof(g_progs[0].name) - 1);
        g_progs[g_nprogs].name[sizeof(g_progs[0].name) - 1] = '\0';
        char *dot = strrchr(g_progs[g_nprogs].name, '.');
        if (dot && !c->dir) *dot = '\0';
        RamfsPath(node, g_progs[g_nprogs].path, sizeof(g_progs[0].path));
        /* self-tests and fault demos stay out of the grid (search finds them) */
        const char *n = g_progs[g_nprogs].name;
        g_progs[g_nprogs].link = false;
        g_progs[g_nprogs].hidden = ends_with_ci(n, "test") || !strcmp(n, "crash") ||
                                   !strcmp(n, "spin") || !strcmp(n, "threads");
        g_nprogs++;
    }
}

static void launch_program(int i)
{
    if (i < 0 || i >= g_nprogs) return;
    RamNode *exe = RamfsResolve(NULL, g_progs[i].path);
    if (exe && g_progs[i].link) AppOpenFile(exe);        /* a shortcut */
    else if (exe) AppRunProgram(exe, g_progs[i].name);
}

/* ---- search ---- */
typedef enum { R_APP, R_PROG, R_SETTING, R_FOLDER, R_FILE } ResKind;
#define MAX_RESULTS 9
static struct {
    ResKind kind;
    int     arg;                       /* app id / program index / settings page */
    char    name[48];
    char    sub[RAMFS_PATH_MAX];       /* subtitle; the path for folders and files */
    int     score;                     /* lower = better */
} g_res[MAX_RESULTS];
static int g_nres;

static const struct { const char *name; const char *keys; int page; } g_setting_items[] = {
    { "System",          "system about pc processor memory uptime",           SETTINGS_SYSTEM },
    { "Display",         "display screen resolution scale monitor",           SETTINGS_DISPLAY },
    { "Personalization", "personalization wallpaper background theme colors", SETTINGS_PERSONALIZE },
    { "Storage",         "storage disk drive space memory ram",               SETTINGS_STORAGE },
    { "Network",         "network ethernet internet ip dns wifi certificates", SETTINGS_NETWORK },
    { "About",           "about version fonts licence",                       SETTINGS_ABOUT },
};
#define N_SETTING_ITEMS ((int)(sizeof(g_setting_items) / sizeof(g_setting_items[0])))

static void add_result(ResKind kind, int arg, const char *name, const char *sub, int score)
{
    /* keep the best MAX_RESULTS, ordered by score (stable) */
    int pos = g_nres;
    while (pos > 0 && g_res[pos - 1].score > score) pos--;
    if (pos >= MAX_RESULTS) return;
    int last = g_nres < MAX_RESULTS ? g_nres : MAX_RESULTS - 1;
    for (int i = last; i > pos; i--) g_res[i] = g_res[i - 1];
    g_res[pos].kind = kind;
    g_res[pos].arg = arg;
    strncpy(g_res[pos].name, name, sizeof(g_res[0].name) - 1);
    g_res[pos].name[sizeof(g_res[0].name) - 1] = '\0';
    strncpy(g_res[pos].sub, sub, sizeof(g_res[0].sub) - 1);
    g_res[pos].sub[sizeof(g_res[0].sub) - 1] = '\0';
    g_res[pos].score = score;
    if (g_nres < MAX_RESULTS) g_nres++;
}

/* 0: name starts with the query, 1: a word does, 2: anywhere; -1: no */
static int match(const char *name, const char *q)
{
    int at = ci_find(name, q);
    if (at < 0) return -1;
    if (at == 0) return 0;
    char before = name[at - 1];
    return (before == ' ' || before == '-' || before == '_' || before == '.') ? 1 : 2;
}

static void search_tree(RamNode *dir, int depth)
{
    for (RamNode *c = dir->child; c; c = c->next) {
        /* system files and program resources would drown the results */
        if (depth == 0 && (!strcmp(c->name, "Windows") || !strcmp(c->name, "Programs"))) continue;
        int m = match(c->name, g_query);
        if (m >= 0) {
            char path[RAMFS_PATH_MAX];
            RamfsPath(c, path, sizeof(path));
            add_result(c->dir ? R_FOLDER : R_FILE, 0, c->name, path, (c->dir ? 30 : 40) + m);
        }
        if (c->dir && depth < 6) search_tree(c, depth + 1);
    }
}

static void run_search(void)
{
    g_nres = 0;
    g_sel = 0;
    if (!g_query_len) return;
    for (int i = 0; i < APP_COUNT; i++) {
        const AppInfo *a = AppGetInfo((AppId)i);
        if (!a->builtin) continue;                       /* placeholders aren't real apps */
        int m = match(a->name, g_query);
        if (m >= 0) add_result(R_APP, i, a->name, "App", m);
    }
    for (int i = 0; i < g_nprogs; i++) {
        int m = match(g_progs[i].name, g_query);
        if (m >= 0) add_result(R_PROG, i, g_progs[i].name, g_progs[i].path, 10 + m);
    }
    for (int i = 0; i < N_SETTING_ITEMS; i++) {
        int m = match(g_setting_items[i].name, g_query);
        if (m < 0 && ci_find(g_setting_items[i].keys, g_query) >= 0) m = 2;
        if (m >= 0) add_result(R_SETTING, g_setting_items[i].page, g_setting_items[i].name,
                               "Settings", 20 + m);
    }
    search_tree(RamfsRoot(), 0);
}

static void open_result(int i)
{
    if (i < 0 || i >= g_nres) return;
    switch (g_res[i].kind) {
    case R_APP:     AppLaunch((AppId)g_res[i].arg); break;
    case R_PROG:    launch_program(g_res[i].arg); break;
    case R_SETTING: SettingsOpenPage(g_res[i].arg); break;
    case R_FOLDER:  AppOpenFolder(RamfsResolve(NULL, g_res[i].sub)); break;
    case R_FILE:    AppOpenFile(RamfsResolve(NULL, g_res[i].sub)); break;
    }
}

static void result_icon(int i, int x, int y, int s)
{
    switch (g_res[i].kind) {
    case R_APP:     AppDrawIcon((AppId)g_res[i].arg, x, y, s); break;
    case R_PROG:    draw_prog_icon(g_res[i].arg, x, y, s); break;
    case R_SETTING: AppDrawIcon(APP_SETTINGS, x, y, s); break;
    case R_FOLDER:  AppDrawFolderIcon(x, y, s); break;
    case R_FILE:    AppDrawNodeIcon(RamfsResolve(NULL, g_res[i].sub), x, y, s); break;
    }
}

static const char *kind_label(ResKind k)
{
    switch (k) {
    case R_APP: return "App";
    case R_PROG: return "Program";
    case R_SETTING: return "Settings";
    case R_FOLDER: return "Folder";
    default: return "File";
    }
}

/* ---- drawing ---- */
static void section_title(int x, int y, const char *s)
{
    GdiTextBold(x, y, s, SH_TEXT);
}

static void draw_search_box(GdiRect r)
{
    GdiRoundRect(r, r.h / 2, SH_FIELD, SH_LINE);
    AppDrawGlyph(GL_SEARCH, r.x + 12, r.y + (r.h - 16) / 2, 16, SH_TEXT2);
    int tx = r.x + 36, ty = r.y + (r.h - GDI_FONT_H) / 2;
    if (g_query_len) {
        GdiTextT(tx, ty, g_query, SH_TEXT);
        int cw = GdiTextW(g_query);
        GdiFillRect(RECT(tx + cw + 1, ty - 1, 2, GDI_FONT_H + 2), ACCENT);   /* caret */
    } else {
        GdiFillRect(RECT(tx, ty - 1, 2, GDI_FONT_H + 2), ACCENT);
        GdiTextT(tx + 6, ty, "Type to search apps, programs, settings and files", SH_TEXT2);
    }
}

static void draw_results(int x, int y, int w)
{
    if (!g_nres) {
        section_title(x, y, "No results");
        GdiTextT(x, y + 24, "Nothing on this PC matches that name.", SH_TEXT2);
        return;
    }
    section_title(x, y, "Best matches");
    y += 26;
    for (int i = 0; i < g_nres; i++) {
        GdiRect row = RECT(x - 8, y, w + 16, 46);
        if (i == g_sel) GdiRoundRect(row, 8, SH_SELECT, SH_LINE);
        else if (hovered(ACT_RESULT, i)) GdiRoundAlpha(row, 8, GDI_WHITE, HOVER_ALPHA);
        result_icon(i, x, y + 7, 32);
        char buf[96];
        fit_text(g_res[i].name, w - 60, buf, sizeof(buf), true);
        GdiTextBold(x + 44, y + 6, buf, SH_TEXT);
        char sub[RAMFS_PATH_MAX + 16];
        if (g_res[i].kind == R_FOLDER || g_res[i].kind == R_FILE || g_res[i].kind == R_PROG)
            ksnprintf(sub, sizeof(sub), "%s  -  %s", g_res[i].kind == R_FILE
                      ? AppFileTypeName(RamfsResolve(NULL, g_res[i].sub)) : kind_label(g_res[i].kind),
                      g_res[i].sub);
        else
            ksnprintf(sub, sizeof(sub), "%s", kind_label(g_res[i].kind));
        fit_text(sub, w - 60, buf, sizeof(buf), false);
        GdiTextT(x + 44, y + 24, buf, SH_TEXT2);
        HOT_OV(row, ACT_RESULT, i);
        y += 48;
    }
    GdiTextT(x, y + 8, "Enter opens the highlighted item.  Up/Down to choose, Esc to clear.", SH_TEXT2);
}

static void draw_home(int x, int y, int w, int bottom)
{
    /* Pinned */
    section_title(x, y, "Pinned");
    y += 28;
    int col = w / N_PINNED;
    for (int i = 0; i < N_PINNED; i++) {
        int cx = x + i * col;
        GdiRect cell = RECT(cx, y - 4, col, 78);
        if (hovered(ACT_APP, g_pinned[i])) GdiRoundAlpha(cell, 8, GDI_WHITE, HOVER_ALPHA);
        AppDrawIcon(g_pinned[i], cx + (col - 44) / 2, y + 2, 44);
        GdiTextCenter(cx, y + 52, col, g_pinned_cap[i], SH_TEXT);
        HOT_OV(cell, ACT_APP, g_pinned[i]);
    }
    y += 96;

    /* Programs from C:\Programs */
    int shown = 0;
    for (int i = 0; i < g_nprogs; i++) if (!g_progs[i].hidden) shown++;
    if (shown) {
        section_title(x, y, "Programs");
        GdiTextT(x + w - GdiTextW("installed"), y + 1, "installed", SH_TEXT2);
        y += 28;
        int per_row = 6, k = 0;
        for (int i = 0; i < g_nprogs && k < 2 * per_row; i++) {
            if (g_progs[i].hidden) continue;
            int cx = x + (k % per_row) * col, cy = y + (k / per_row) * 72;
            GdiRect cell = RECT(cx, cy - 4, col, 68);
            if (hovered(ACT_PROG, i)) GdiRoundAlpha(cell, 8, GDI_WHITE, HOVER_ALPHA);
            draw_prog_icon(i, cx + (col - 36) / 2, cy + 2, 36);
            char cap[40];
            fit_text(g_progs[i].name, col - 8, cap, sizeof(cap), false);
            GdiTextCenter(cx, cy + 42, col, cap, SH_TEXT);
            HOT_OV(cell, ACT_PROG, i);
            k++;
        }
        y += ((k + per_row - 1) / per_row) * 72 + 14;
    }

    /* Recent: apps you opened and documents/folders */
    section_title(x, y, "Recent");
    y += 28;
    AppId apps[4];
    RamNode *files[6];
    int na = AppRecent(apps, 4), nf = AppRecentFiles(files, 6);
    if (!na && !nf) {
        GdiTextT(x, y, "Apps, folders and documents you open will show up here.", SH_TEXT2);
        return;
    }
    int half = w / 2, k = 0;
    for (int i = 0; i < na + nf; i++) {
        int cx = x + (k % 2) * half, cy = y + (k / 2) * 44;
        if (cy + 40 > bottom) break;
        GdiRect cell = RECT(cx - 6, cy - 4, half - 8, 42);
        char name[48], sub[RAMFS_PATH_MAX];
        if (i < na) {
            if (hovered(ACT_APP, apps[i])) GdiRoundAlpha(cell, 8, GDI_WHITE, HOVER_ALPHA);
            AppDrawIcon(apps[i], cx, cy + 2, 30);
            fit_text(AppGetInfo(apps[i])->name, half - 60, name, sizeof(name), false);
            ksnprintf(sub, sizeof(sub), "App");
            HOT_OV(cell, ACT_APP, apps[i]);
        } else {
            RamNode *f = files[i - na];
            if (hovered(ACT_RECENT, i - na)) GdiRoundAlpha(cell, 8, GDI_WHITE, HOVER_ALPHA);
            AppDrawNodeIcon(f, cx, cy + 2, 30);
            fit_text(f->name, half - 60, name, sizeof(name), false);
            char path[RAMFS_PATH_MAX];
            RamfsPath(f->parent ? f->parent : f, path, sizeof(path));
            fit_text(path, half - 60, sub, sizeof(sub), false);
            HOT_OV(cell, ACT_RECENT, i - na);
        }
        GdiTextT(cx + 40, cy + 2, name, SH_TEXT);
        GdiTextT(cx + 40, cy + 19, sub, SH_TEXT2);
        k++;
    }
}

static void draw_start_menu(void)
{
    if (!g_start_open) { L_start = RECT(0, 0, 0, 0); return; }
    int sw = GdiScreenW();
    GdiRect work = WmWorkArea();
    int mw = 640; if (mw > sw - 40) mw = sw - 40;
    int mh = 620; if (mh > work.h - 24) mh = work.h - 24;
    int mx = (sw - mw) / 2;
    int my = work.y + work.h - mh - 4;
    if (my < 12) my = 12;
    L_start = RECT(mx, my, mw, mh);

    GdiDropShadow(RECT(mx, my + 6, mw, mh), 16, 22, 80);
    glass(L_start, 16);
    HOT_OV(L_start, ACT_SWALLOW, 0);

    int pad = 28, cx = mx + pad, cw = mw - 2 * pad;
    draw_search_box(RECT(cx, my + 22, cw, 38));

    int body_y = my + 22 + 38 + 22, bottom = my + mh - 72;
    if (g_query_len) draw_results(cx, body_y, cw);
    else             draw_home(cx, body_y, cw, bottom);

    /* User / power bar */
    int by = my + mh - 62;
    GdiFillRect(RECT(mx + 1, by, mw - 2, 1), SH_LINE);
    RtcTime t;
    rtc_read(&t);
    const char *greet = t.hour < 12 ? "Good morning" : t.hour < 18 ? "Good afternoon" : "Good evening";
    GdiFillCircle(cx + 18, by + 31, 17, GDI_C(0x6A, 0x4A, 0xC8));
    GdiTextCenter(cx + 1, by + 24, 36, "DP", GDI_WHITE);
    GdiTextT(cx + 46, by + 14, greet, SH_TEXT2);
    GdiTextBold(cx + 46, by + 31, USER_NAME, SH_TEXT);

    /* Files, Settings and power: line glyphs, 20px in 36px targets */
    static const struct { Glyph g; ActKind k; int arg; } foot[] = {
        { GL_FOLDER, ACT_APP, APP_EXPLORER }, { GL_GEAR, ACT_APP, APP_SETTINGS }, { GL_POWER, ACT_POWER, 0 },
    };
    for (int i = 0; i < 3; i++) {
        GdiRect b = RECT(mx + mw - pad - 36 - (2 - i) * 44, by + 13, 36, 36);
        if (hovered(foot[i].k, foot[i].arg)) GdiRoundAlpha(b, 8, GDI_WHITE, HOVER_ALPHA);
        AppDrawGlyph(foot[i].g, b.x + 8, b.y + 8, 20, SH_TEXT);
        HOT_OV(b, foot[i].k, foot[i].arg);
    }
}

static void start_open(bool open)
{
    if (open && !g_start_open) {
        scan_programs();
        g_query_len = 0;
        g_query[0] = '\0';
        g_nres = 0;
        g_sel = 0;
    }
    g_start_open = open;
    WmInvalidate();
}

/* -----------------------------------------------------------------------
 * Dock
 * ----------------------------------------------------------------------- */
typedef enum { DK_START, DK_SEARCH, DK_APP, DK_TASK } DockKind;
typedef struct { DockKind kind; int arg; char tip[WM_TITLE_MAX]; GdiRect r; } DockItem;

static const AppId g_dock_apps[] = {
    APP_TERMINAL, APP_EXPLORER, APP_NOTEPAD, APP_SETTINGS, APP_CALENDAR, APP_NETSURF,
    APP_STORE,
};
#define N_DOCK_APPS ((int)(sizeof(g_dock_apps) / sizeof(g_dock_apps[0])))
#define MAX_TASKS   8
#define DOCK_ITEM   40                   /* hit target */
#define DOCK_ICON   28                   /* app tiles; line glyphs are 22 */
#define DOCK_GLYPH  22
#define DOCK_GAP    6
#define DOCK_PAD    10
#define DOCK_H      56
#define TRAY_W      176

static DockItem g_dock[2 + N_DOCK_APPS + MAX_TASKS];
static int      g_ndock, g_ndock_fixed;
static GdiRect  L_dock, L_tray, L_clock, L_net;
static int      g_dock_hover = -1;       /* index into g_dock, -2 = clock, -3 = network */

/* The dock's items: fixed ones, then a button per program window that
 * has no dock icon of its own */
static void dock_layout(void)
{
    g_ndock = 0;
    g_dock[g_ndock++] = (DockItem){ DK_START, 0, "Start", {0} };
    g_dock[g_ndock++] = (DockItem){ DK_SEARCH, 0, "Search", {0} };
    for (int i = 0; i < N_DOCK_APPS; i++) {
        DockItem *d = &g_dock[g_ndock++];
        d->kind = DK_APP;
        d->arg = g_dock_apps[i];
        strncpy(d->tip, AppGetInfo(g_dock_apps[i])->name, sizeof(d->tip) - 1);
        d->tip[sizeof(d->tip) - 1] = '\0';
    }
    g_ndock_fixed = g_ndock;
    WND *wins[WM_MAX_WINDOWS];
    int n = WmListWindows(wins, WM_MAX_WINDOWS);
    /* stable order: by id (creation) */
    int ids[WM_MAX_WINDOWS], k = 0;
    for (int i = 0; i < n; i++) {             /* windows without a pinned dock app */
        bool pinned = false;
        for (int j = 0; j < N_DOCK_APPS; j++) if (wins[i]->app == (int)g_dock_apps[j]) pinned = true;
        if (!pinned) ids[k++] = wins[i]->id;
    }
    for (int i = 1; i < k; i++) for (int j = i; j > 0 && ids[j - 1] > ids[j]; j--) {
        int t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t;
    }
    for (int i = 0; i < k && i < MAX_TASKS; i++) {
        WND *w = WmWindowById(ids[i]);
        DockItem *d = &g_dock[g_ndock++];
        d->kind = DK_TASK;
        d->arg = ids[i];
        strncpy(d->tip, w->title, sizeof(d->tip) - 1);
        d->tip[sizeof(d->tip) - 1] = '\0';
    }

    /* The dock (launchers and windows) is centred; the tray (network,
     * clock) is its own glass block at the right edge */
    int sw = GdiScreenW(), sh = GdiScreenH();
    int sep = g_ndock > g_ndock_fixed ? 13 : 0;
    int dw = 2 * DOCK_PAD + g_ndock * DOCK_ITEM + (g_ndock - 1) * DOCK_GAP + sep;
    int y = sh - DOCK_H - 12;
    L_tray = RECT(sw - TRAY_W - 12, y, TRAY_W, DOCK_H);
    int dx = (sw - dw) / 2;
    if (dx + dw > L_tray.x - 12) dx = L_tray.x - 12 - dw;   /* narrow screens */
    if (dx < 12) dx = 12;
    L_dock = RECT(dx, y, dw, DOCK_H);
    for (int i = 0; i < g_ndock; i++) {
        int x = L_dock.x + DOCK_PAD + i * (DOCK_ITEM + DOCK_GAP) + (i >= g_ndock_fixed ? sep : 0);
        g_dock[i].r = RECT(x, y + (DOCK_H - DOCK_ITEM) / 2 - 2, DOCK_ITEM, DOCK_ITEM);
    }
    L_net = RECT(L_tray.x + 6, y + 8, 40, DOCK_H - 16);
    L_clock = RECT(L_net.x + L_net.w + 4, y + 6, L_tray.x + L_tray.w - 6 - (L_net.x + L_net.w + 4), DOCK_H - 12);
}

/* Live clock strings, refreshed from the RTC each minute. */
static char g_clock_time[12] = "12:00 PM";
static char g_clock_date[12] = "01/01/2026";
static char g_clock_long[48] = "";

static void draw_tooltip(int cx, int bottom, const char *text)
{
    char buf[WM_TITLE_MAX];
    fit_text(text, 300, buf, sizeof(buf), false);
    int w = GdiTextW(buf) + 20, h = GDI_FONT_H + 12;
    int x = cx - w / 2;
    if (x < 4) x = 4;
    if (x + w > GdiScreenW() - 4) x = GdiScreenW() - 4 - w;
    GdiRect r = RECT(x, bottom - h, w, h);
    GdiDropShadow(RECT(r.x, r.y + 2, r.w, r.h), 6, 8, 60);
    GdiRoundRect(r, 6, POP_BG, POP_LINE);
    GdiTextT(x + 10, r.y + 6, buf, POP_TEXT);
}

static bool net_up(char *tip, int cap)
{
    NetStatus st;
    NetGetStatus(&st);
    if (st.configured) {
        char ip[20];
        NetFormatIp(st.ip, ip, sizeof(ip));
        ksnprintf(tip, cap, "Ethernet: connected (%s)", ip);
        return true;
    }
    ksnprintf(tip, cap, st.present ? (st.link ? "Ethernet: getting an address..." : "Ethernet: cable unplugged")
                                   : "No network adapter");
    return false;
}

static void draw_dock(void)
{
    dock_layout();
    GdiRect d = L_dock;
    GdiDropShadow(RECT(d.x, d.y + 4, d.w, d.h), 16, 14, 60);
    glass(d, 16);

    WND *active = WmActiveWindow();
    for (int i = 0; i < g_ndock; i++) {
        DockItem *it = &g_dock[i];
        GdiRect r = it->r;
        bool lit = i == g_dock_hover || (it->kind == DK_START && g_start_open && !g_query_len) ||
                   (it->kind == DK_SEARCH && g_start_open && g_query_len);
        if (lit) GdiRoundAlpha(r, 8, GDI_WHITE, HOVER_ALPHA + 8);

        int io = (DOCK_ITEM - DOCK_ICON) / 2, go = (DOCK_ITEM - DOCK_GLYPH) / 2;
        WND *w = NULL;
        switch (it->kind) {
        case DK_START:  AppDrawGlyph(GL_NOVA, r.x + go, r.y + go, DOCK_GLYPH, ACCENT); break;
        case DK_SEARCH: AppDrawGlyph(GL_SEARCH, r.x + go + 1, r.y + go + 1, DOCK_GLYPH - 2, SH_TEXT); break;
        case DK_APP:
            AppDrawIcon((AppId)it->arg, r.x + io, r.y + io, DOCK_ICON);
            w = WmFindApp(it->arg);
            break;
        case DK_TASK:
            w = WmWindowById(it->arg);
            if (w) AppDrawWindowIcon(w, r.x + io, r.y + io, DOCK_ICON);
            break;
        }
        /* Running indicator: a pill under the icon, wider when focused */
        if (w) {
            bool focused = (w == active);
            int pw = focused ? 14 : 5;
            GdiRoundRect(RECT(r.x + (r.w - pw) / 2, r.y + r.h + 1, pw, 3), 1,
                         focused ? ACCENT : SH_TEXT2, GDI_TRANSPARENT);
        }
        ActKind k = it->kind == DK_START ? ACT_START : it->kind == DK_SEARCH ? ACT_SEARCH :
                    it->kind == DK_APP ? ACT_APP : ACT_TASK;
        HOT_OV(RECT(r.x - DOCK_GAP / 2, d.y, r.w + DOCK_GAP, d.h), k, it->arg);
        if (i == g_ndock_fixed - 1 && g_ndock > g_ndock_fixed)
            GdiVLine(r.x + r.w + DOCK_GAP / 2 + 6, d.y + 14, d.y + d.h - 14, SH_LINE);
    }

    /* The tray: network status and the clock */
    GdiRect t = L_tray;
    GdiDropShadow(RECT(t.x, t.y + 4, t.w, t.h), 16, 14, 60);
    glass(t, 16);
    char net_tip[64];
    bool up = net_up(net_tip, sizeof(net_tip));
    if (g_dock_hover == -3) GdiRoundAlpha(L_net, 8, GDI_WHITE, HOVER_ALPHA + 8);
    AppDrawGlyph(up ? GL_NETWORK : GL_NETWORK_OFF, L_net.x + (L_net.w - 20) / 2, L_net.y + (L_net.h - 20) / 2,
                 20, up ? SH_TEXT : SH_TEXT2);
    HOT_OV(L_net, ACT_NET, 0);
    if (g_dock_hover == -2) GdiRoundAlpha(L_clock, 8, GDI_WHITE, HOVER_ALPHA + 8);
    int right = L_clock.x + L_clock.w - 10;
    GdiTextBold(right - GdiTextBoldW(g_clock_time), L_clock.y + 5, g_clock_time, SH_TEXT);
    GdiTextT(right - GdiTextW(g_clock_date), L_clock.y + 23, g_clock_date, SH_TEXT2);
    HOT_OV(L_clock, ACT_CLOCK, 0);

    /* Tooltip for the hovered item (not while its menu or Start is up) */
    if (!g_menu.open) {
        if (g_dock_hover >= 0 && g_dock_hover < g_ndock && !(g_start_open && g_dock_hover < 2)) {
            GdiRect r = g_dock[g_dock_hover].r;
            draw_tooltip(r.x + r.w / 2, d.y - 8, g_dock[g_dock_hover].tip);
        } else if (g_dock_hover == -2) {
            draw_tooltip(L_clock.x + L_clock.w / 2, t.y - 8, g_clock_long);
        } else if (g_dock_hover == -3) {
            draw_tooltip(L_net.x + L_net.w / 2, t.y - 8, net_tip);
        }
    }
}

static void dock_task_click(int id)
{
    WND *w = WmWindowById(id);
    if (!w) return;
    if (w->active && !w->minimized) WmMinimize(w);
    else WmSetActive(w);
}

/* -----------------------------------------------------------------------
 * Alt+Tab
 * ----------------------------------------------------------------------- */
static struct { bool on; int n, sel; int ids[WM_MAX_WINDOWS]; } g_switch;

static void switch_begin(bool backwards)
{
    WND *wins[WM_MAX_WINDOWS];
    int n = WmListWindows(wins, WM_MAX_WINDOWS);
    if (!n) return;
    g_switch.n = n;
    for (int i = 0; i < n; i++) g_switch.ids[i] = wins[i]->id;
    g_switch.sel = n > 1 ? (backwards ? n - 1 : 1) : 0;
    g_switch.on = true;
    WmInvalidate();
}

static void switch_step(bool backwards)
{
    if (!g_switch.n) return;
    g_switch.sel = (g_switch.sel + (backwards ? g_switch.n - 1 : 1)) % g_switch.n;
    WmInvalidate();
}

static void switch_end(bool commit)
{
    if (!g_switch.on) return;
    g_switch.on = false;
    if (commit) {
        WND *w = WmWindowById(g_switch.ids[g_switch.sel]);
        if (w) WmSetActive(w);
    }
    WmInvalidate();
}

static void draw_switcher(void)
{
    if (!g_switch.on) return;
    int n = g_switch.n, cw = 168, ch = 132, gap = 12;
    int per_row = (GdiScreenW() - 80) / (cw + gap);
    if (per_row < 1) per_row = 1;
    int cols = n < per_row ? n : per_row, rows = (n + per_row - 1) / per_row;
    int pw = cols * (cw + gap) + gap + 16, ph = rows * (ch + gap) + gap + 16;
    int px = (GdiScreenW() - pw) / 2, py = (GdiScreenH() - ph) / 2 - 40;
    GdiDropShadow(RECT(px, py + 4, pw, ph), 12, 20, 90);
    GdiRoundRect(RECT(px, py, pw, ph), 12, GDI_C(0x20, 0x20, 0x20), POP_LINE);
    for (int i = 0; i < n; i++) {
        WND *w = WmWindowById(g_switch.ids[i]);
        if (!w) continue;
        int x = px + 8 + gap + (i % per_row) * (cw + gap), y = py + 8 + gap + (i / per_row) * (ch + gap);
        GdiRect card = RECT(x, y, cw, ch);
        GdiRoundRect(card, 8, i == g_switch.sel ? GDI_C(0x3A, 0x3A, 0x3A) : GDI_C(0x2B, 0x2B, 0x2B),
                     i == g_switch.sel ? GDI_C(0x60, 0xB8, 0xF8) : POP_LINE);
        if (i == g_switch.sel)
            GdiRoundRect(RECT(x - 2, y - 2, cw + 4, ch + 4), 10, GDI_TRANSPARENT, GDI_C(0x60, 0xB8, 0xF8));
        AppDrawWindowIcon(w, x + (cw - 48) / 2, y + 22, 48);
        char t[64];
        fit_text(w->title, cw - 16, t, sizeof(t), false);
        GdiTextCenter(x, y + 84, cw, t, POP_TEXT);
        if (w->minimized) GdiTextCenter(x, y + 104, cw, "Minimized", POP_TEXT2);
    }
}

/* -----------------------------------------------------------------------
 * Themes (public)
 * ----------------------------------------------------------------------- */
int         DesktopThemeCount(void)      { return N_THEMES; }
const char *DesktopThemeName(int i)      { return (i >= 0 && i < N_THEMES) ? g_themes[i].name : ""; }
int         DesktopTheme(void)           { return g_theme; }

void DesktopSetTheme(int i)
{
    if (i < 0 || i >= N_THEMES || i == g_theme) return;
    g_theme = i;
    WmInvalidateBackground();
}

void DesktopDrawThemePreview(int i, GdiRect r)
{
    if (i < 0 || i >= N_THEMES) return;
    draw_wallpaper_in(&g_themes[i], r);
    /* a tiny dock */
    int dw = r.w / 2, dh = r.h / 10 > 4 ? r.h / 10 : 4;
    GdiRoundAlpha(RECT(r.x + (r.w - dw) / 2, r.y + r.h - dh - 4, dw, dh), dh / 2, GLASS_TINT, 200);
}

/* -----------------------------------------------------------------------
 * Shell layers (registered with the WM)
 * ----------------------------------------------------------------------- */
static void shell_background(void)
{
    draw_wallpaper_in(TH, RECT(0, 0, GdiScreenW(), GdiScreenH()));
    draw_desktop_icons();
}

static void shell_overlay(void)
{
    g_hot_ov_n = 0;
    draw_start_menu();
    draw_dock();
    draw_menu();
    draw_switcher();
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
    dock_layout();
    WmSetWorkArea(RECT(0, 0, GdiScreenW(), L_dock.y - 8));
    WmSetDesktop(shell_background, shell_overlay);
    AppInit();
    kprintf("[SHELL] Desktop shell ready (%dx%d)\n", GdiScreenW(), GdiScreenH());
}

bool DesktopAvailable(void) { return g_ready; }

bool DesktopSetDisplayMode(int w, int h)
{
    if (!g_ready) return false;
    DesktopLock();
    DisplayMode cur = DisplayCurrentMode();
    if (cur.w == w && cur.h == h) { DesktopUnlock(); return true; }
    int ow = GdiScreenW(), oh = GdiScreenH(), os = GdiScale();
    WmCursorHide();
    bool ok = DisplaySetMode(w, h);
    if (!GdiDisplayChanged()) {               /* cannot happen: the old mode is back */
        DesktopUnlock();
        return false;
    }
    /* Lay the shell out for the new size, then refit the windows into it */
    g_start_open = false;
    dock_layout();
    WmSetWorkArea(RECT(0, 0, GdiScreenW(), L_dock.y - 8));
    WmDisplayChanged(ow, oh, os);
    WmInvalidateBackground();
    DesktopUnlock();
    if (ok) {
        DisplayMode m = DisplayCurrentMode();
        UmGuiDisplayChanged(m.w, m.h);        /* WM_DISPLAYCHANGE to programs */
    }
    kprintf("[SHELL] Display mode %dx%d %s (desktop %dx%d)\n", w, h, ok ? "set" : "refused",
            GdiScreenW(), GdiScreenH());
    return ok;
}

void DesktopToggleStart(void) { start_open(!g_start_open); }

/* Refresh the dock clock strings from the RTC. */
static void update_clock(void)
{
    RtcTime t;
    rtc_read(&t);
    static const char *const months[12] = {
        "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December",
    };
    static const char *const days[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    };
    int h24 = t.hour;
    const char *ap = (h24 < 12) ? "AM" : "PM";
    int h12 = h24 % 12; if (h12 == 0) h12 = 12;
    ksnprintf(g_clock_time, sizeof(g_clock_time), "%d:%02u %s", h12, t.minute, ap);
    ksnprintf(g_clock_date, sizeof(g_clock_date), "%02u/%02u/%04u", t.month, t.day, t.year);
    /* day of the week (Sakamoto) */
    static const int off[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = t.year - (t.month < 3);
    int m = t.month >= 1 && t.month <= 12 ? t.month : 1;
    int dow = (y + y / 4 - y / 100 + y / 400 + off[m - 1] + t.day) % 7;
    ksnprintf(g_clock_long, sizeof(g_clock_long), "%s, %s %u, %u", days[dow], months[m - 1],
              t.day, t.year);
}

/* ---- menus for right-clicks ---- */
static void menu_for_icon(int i, int x, int y)
{
    menu_begin(x, y);
    menu_add("Open", MA_OPEN_ICON, i, NULL, false);
    if (i < N_ICONS && g_icons[i].path) menu_add("Open in Terminal", MA_TERMINAL_AT, 0, g_icons[i].path, false);
}

static void menu_for_desktop(int x, int y)
{
    menu_begin(x, y);
    menu_add("Open Terminal", MA_TERMINAL_AT, 0, "\\Documents", false);
    menu_add("Open File Explorer", MA_EXPLORER_AT, 0, "\\", false);
    menu_add("Show desktop", MA_SHOW_DESKTOP, 0, NULL, true);
    menu_add("Next wallpaper", MA_THEME_NEXT, 0, NULL, true);
    menu_add("Personalize", MA_SETTINGS_PAGE, SETTINGS_PERSONALIZE, NULL, false);
    menu_add("Display settings", MA_SETTINGS_PAGE, SETTINGS_DISPLAY, NULL, false);
}

static void menu_for_window(WND *w, int x, int y)
{
    menu_begin(x, y);
    bool tiled = w->maximized || w->snapped;
    bool can_tile = (w->style & WS_MINMAXBTN) && !w->fixed_size;
    if (can_tile) {
        if (tiled) menu_add("Restore", MA_WIN_RESTORE, w->id, NULL, false);
        if (!w->maximized) menu_add("Maximize", MA_WIN_MAX, w->id, NULL, false);
        menu_add("Snap left", MA_WIN_SNAP_L, w->id, NULL, false);
        menu_add("Snap right", MA_WIN_SNAP_R, w->id, NULL, false);
    }
    if (w->style & WS_MINMAXBTN) menu_add("Minimize", MA_WIN_MIN, w->id, NULL, false);
    menu_add("Close", MA_WIN_CLOSE, w->id, NULL, can_tile || (w->style & WS_MINMAXBTN));
}

static void menu_for_dock(const Hot *h, int x, int y)
{
    if (h->kind == ACT_APP) {
        const AppInfo *a = AppGetInfo((AppId)h->arg);
        WND *w = WmFindApp(h->arg);
        menu_begin(x, y);
        char label[48];
        ksnprintf(label, sizeof(label), w && a->multi ? "New %s window" : "Open %s", a->name);
        menu_add(label, MA_APP, h->arg, NULL, false);
        if (w) menu_add("Close window", MA_CLOSE_APP, h->arg, NULL, true);
    } else if (h->kind == ACT_TASK) {
        WND *w = WmWindowById(h->arg);
        if (w) menu_for_window(w, x, y);
    } else if (h->kind == ACT_START) {
        menu_begin(x, y);
        menu_add("Terminal", MA_APP, APP_TERMINAL, NULL, false);
        menu_add("File Explorer", MA_APP, APP_EXPLORER, NULL, false);
        menu_add("Settings", MA_APP, APP_SETTINGS, NULL, false);
        menu_add("Show desktop", MA_SHOW_DESKTOP, 0, NULL, true);
        if (SleepSupported()) menu_add("Sleep", MA_SLEEP, 0, NULL, true);
        menu_add("Restart", MA_RESTART, 0, NULL, !SleepSupported());
        menu_add("Shut down", MA_SHUTDOWN, 0, NULL, false);
    }
}

/* ---- pointer ---- */
static void close_popups(void)
{
    g_menu.open = false;
    WmInvalidate();
}

static void run_action(const Hot *h)
{
    switch (h->kind) {
    case ACT_APP:
        start_open(false);
        AppLaunch((AppId)h->arg);
        break;
    case ACT_PROG:   start_open(false); launch_program(h->arg); break;
    case ACT_RESULT: start_open(false); open_result(h->arg); break;
    case ACT_RECENT: {
        RamNode *files[6];
        int n = AppRecentFiles(files, 6);
        start_open(false);
        if (h->arg < n) {
            if (files[h->arg]->dir) AppOpenFolder(files[h->arg]);
            else AppOpenFile(files[h->arg]);
        }
        break; }
    case ACT_POWER: {
        GdiRect r = h->r;
        menu_begin(r.x, r.y - 4);
        if (SleepSupported()) menu_add("Sleep", MA_SLEEP, 0, NULL, false);
        menu_add("Restart", MA_RESTART, 0, NULL, false);
        menu_add("Shut down", MA_SHUTDOWN, 0, NULL, false);
        /* open upwards from the button */
        g_menu.y = r.y - (8 + g_menu.n * MENU_ROW) - 6;
        break; }
    default: break;
    }
}

/* Handle a left press (or double-click) at (x, y), in stacking order:
 * menus, Start, dock, windows, then the desktop itself. */
static void desktop_press(int x, int y, bool dbl)
{
    if (WmGetCapture()) {                  /* a program is tracking the mouse (its menu is open) */
        WmMouseButton(x, y, dbl ? WM_MOUSE_DBLCLK : WM_MOUSE_DOWN);
        return;
    }
    const Hot *h = hot_find(g_hot_ov, g_hot_ov_n, x, y);

    if (g_menu.open) {
        g_menu.open = false;
        if (h && h->kind == ACT_MENU) run_menu_item(&g_menu.it[h->arg]);
        WmInvalidate();
        return;
    }
    if (g_start_open && pt_in(L_start, x, y)) {
        if (h && h->kind != ACT_SWALLOW) run_action(h);
        WmInvalidate();
        return;
    }
    if (pt_in(L_dock, x, y) || pt_in(L_tray, x, y)) {
        if (h) {
            switch (h->kind) {
            case ACT_START:  start_open(!(g_start_open && !g_query_len)); break;
            case ACT_SEARCH: start_open(true); break;
            case ACT_APP:    start_open(false); AppActivate((AppId)h->arg); break;
            case ACT_TASK:   start_open(false); dock_task_click(h->arg); break;
            case ACT_CLOCK:  start_open(false); AppActivate(APP_CALENDAR); break;
            case ACT_NET:    start_open(false); SettingsOpenPage(SETTINGS_NETWORK); break;
            default: break;
            }
        }
        WmInvalidate();
        return;
    }
    if (g_start_open) {                    /* click-away closes the menu */
        start_open(false);
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

static void desktop_right_press(int x, int y)
{
    const Hot *h = hot_find(g_hot_ov, g_hot_ov_n, x, y);
    g_menu.open = false;
    if (g_start_open && pt_in(L_start, x, y)) { WmInvalidate(); return; }
    if (g_start_open) start_open(false);
    if (pt_in(L_dock, x, y) || pt_in(L_tray, x, y)) {
        if (h) menu_for_dock(h, x, L_dock.y - 6);
        if (g_menu.open) g_menu.y = L_dock.y - 6 - (8 + g_menu.n * MENU_ROW + 20);
        WmInvalidate();
        return;
    }
    bool caption;
    WND *w = WmWindowAt(x, y, &caption);
    if (w) {
        WmSetActive(w);
        if (caption) menu_for_window(w, x, y);
        WmInvalidate();
        return;
    }
    const Hot *ic = hot_find(g_hot_bg, g_hot_bg_n, x, y);
    if (ic) {
        if (g_icon_sel != ic->arg) { g_icon_sel = ic->arg; WmInvalidateBackground(); }
        menu_for_icon(ic->arg, x, y);
    } else {
        menu_for_desktop(x, y);
    }
    WmInvalidate();
}

static void desktop_hover(int x, int y)
{
    int hover = -1;
    if (!WmMouseCaptured() && pt_in(L_dock, x, y)) {
        for (int i = 0; i < g_ndock; i++) {
            GdiRect r = g_dock[i].r;
            if (pt_in(RECT(r.x - DOCK_GAP / 2, L_dock.y, r.w + DOCK_GAP, L_dock.h), x, y)) hover = i;
        }
    }
    if (!WmMouseCaptured() && pt_in(L_clock, x, y)) hover = -2;
    if (!WmMouseCaptured() && pt_in(L_net, x, y)) hover = -3;
    const Hot *h = WmMouseCaptured() ? NULL : hot_find(g_hot_ov, g_hot_ov_n, x, y);
    ActKind hk = h ? h->kind : ACT_NONE;
    int ha = h ? h->arg : 0;
    if (hover != g_dock_hover || hk != g_hover_kind || ha != g_hover_arg) {
        g_dock_hover = hover;
        g_hover_kind = hk;
        g_hover_arg = ha;
        WmInvalidate();
    }
}

/* ---- keyboard ---- */
static bool g_win_down, g_win_used;

static void start_key(const KeyEvent *k)
{
    if (k->scancode == KEY_ESC && !k->extended) {
        if (g_query_len) { g_query_len = 0; g_query[0] = '\0'; run_search(); }
        else start_open(false);
    } else if (k->extended && (k->scancode == KEY_UP || k->scancode == KEY_DOWN)) {
        if (g_nres) g_sel = (g_sel + (k->scancode == KEY_DOWN ? 1 : g_nres - 1)) % g_nres;
    } else if (k->ch == '\n') {
        if (g_query_len && g_nres) { int s = g_sel; start_open(false); open_result(s); }
    } else if (k->ch == '\b') {
        if (g_query_len) { g_query[--g_query_len] = '\0'; run_search(); }
    } else if (k->ch >= ' ' && k->ch <= '~' && !k->ctrl && !k->alt) {
        if (g_query_len < (int)sizeof(g_query) - 1) {
            g_query[g_query_len++] = k->ch;
            g_query[g_query_len] = '\0';
            run_search();
        }
    }
    WmInvalidate();
}

static void desktop_key(const KeyEvent *k)
{
    bool win_key = k->extended && k->scancode == KEY_LWIN;

    if (!k->pressed) {
        if (win_key) {
            g_win_down = false;
            if (!g_win_used) { g_menu.open = false; DesktopToggleStart(); }
            return;
        }
        if (k->scancode == KEY_ALT && g_switch.on) { switch_end(true); return; }
        WmKey(k);                               /* releases: to windows that want them */
        return;
    }

    if (win_key) { g_win_down = true; g_win_used = false; return; }

    /* Alt+Tab (Shift reverses), Esc cancels */
    if (k->alt && k->scancode == KEY_TAB && !k->extended) {
        if (!g_switch.on) { g_menu.open = false; start_open(false); switch_begin(k->shift); }
        else switch_step(k->shift);
        return;
    }
    if (g_switch.on) {
        if (k->scancode == KEY_ESC) switch_end(false);
        return;
    }

    /* Win+key shortcuts */
    if (g_win_down) {
        g_win_used = true;
        WND *a = WmActiveWindow();
        if (k->extended) {
            if (a && k->scancode == KEY_LEFT)  WmSnap(a, a->snapped && a->frame.x > WmWorkArea().x ? WM_SNAP_RESTORE : WM_SNAP_LEFT);
            if (a && k->scancode == KEY_RIGHT) WmSnap(a, a->snapped && a->frame.x == WmWorkArea().x ? WM_SNAP_RESTORE : WM_SNAP_RIGHT);
            if (a && k->scancode == KEY_UP)    WmSnap(a, WM_SNAP_MAX);
            if (a && k->scancode == KEY_DOWN) {
                if (a->maximized || a->snapped) WmSnap(a, WM_SNAP_RESTORE);
                else if (a->style & WS_MINMAXBTN) WmMinimize(a);
            }
        } else if ((k->ch | 0x20) == 'e') {
            start_open(false);
            AppLaunch(APP_EXPLORER);
        } else if ((k->ch | 0x20) == 'd') {
            start_open(false);
            WmShowDesktopToggle();
        } else if ((k->ch | 0x20) == 's') {
            start_open(true);
        }
        WmInvalidate();
        return;
    }

    if (g_menu.open && k->scancode == KEY_ESC) { close_popups(); return; }
    if (g_start_open) { start_key(k); return; }
    WmKey(k);
}

/* Window-manager + shell event loop (runs as the 'desktop' kernel thread).
 * Polls PS/2, moves the cursor with save-under, routes input, and
 * recomposites whenever something changed (or once per minute for the
 * clock). */
/* Watchdog (called from the timer tick): when the desktop loop has not come
 * round for 3 seconds, log where its thread is, once per stall. */
static Thread *volatile g_desktop_kt;

void DesktopWatchdog(UINT64 now)
{
    static UINT64 reported;
    Thread *kt = g_desktop_kt;
    if (!kt || g_sleeping || now - g_desktop_beat < 300 || reported == g_desktop_beat) return;
    reported = g_desktop_beat;
    extern char __text_end[];
    Thread *who[2] = { kt, DesktopLockOwner() };
    for (int i = 0; i < 2; i++) {
        Thread *th = who[i];
        if (!th || (i && th == kt)) continue;
        kprintf("[WATCHDOG] %s: '%s' (TID %llu, state %d); kernel return addresses:\n",
                i ? "the desktop lock is held by" : "the desktop has not responded for 3 s", th->name,
                (unsigned long long)th->tid, th->state);
        UINT64 *sp = (UINT64 *)(uintptr_t)th->context.rsp;
        UINT64 *top = (UINT64 *)((uintptr_t)th->kernel_stack + th->stack_size);
        for (int n = 0; sp < top && n < 24; sp++)
            if (*sp >= 0xffffffff80000000ull && *sp < (UINT64)(uintptr_t)__text_end) { kprintf("  %llx\n", *sp); n++; }
    }
}

void DesktopRun(void *arg)
{
    (void)arg;
    if (!g_ready) return;
    g_desktop_kt = sched_current();

    update_clock();
    /* Started from the installation disc: offer to install */
    if (SetupIsLive()) AppLaunch(APP_SETUP);
    WmComposite();
    WmCursorShow(GdiScreenW() / 2, GdiScreenH() / 2);

    RtcTime t; rtc_read(&t);
    int    last_min  = t.minute;
    bool   prev_left = false, prev_right = false, prev_mid = false;
    UINT64 last_press = 0;
    int    last_px = -100, last_py = -100;

    UINT64 last_desk_check = 0;
    UINT64 desk_sig = 0;
    for (;;) {
        g_desktop_beat = sched_ticks();
        /* Program threads take this lock around file-system access */
        DesktopLock();
        ps2_poll();
        power_poll();
        /* C:\\Desktop changed (an installer made a shortcut)? redraw the icons */
        if (g_desktop_beat - last_desk_check >= 50) {
            last_desk_check = g_desktop_beat;
            UINT64 sig = desktop_files_signature();
            if (sig != desk_sig) { desk_sig = sig; WmInvalidateBackground(); }
        }

        InputEvent ev;
        while (InputPoll(&ev)) {
            if (ev.type == INPUT_MOUSE) {
                if (ev.dx || ev.dy) {
                    WmCursorMoveBy(ev.dx, ev.dy);
                    WmMouseMove(WmCursorX(), WmCursorY());
                    desktop_hover(WmCursorX(), WmCursorY());
                }
                bool left = (ev.buttons & MOUSE_LEFT) != 0;
                bool right = (ev.buttons & MOUSE_RIGHT) != 0;
                bool mid = (ev.buttons & MOUSE_MIDDLE) != 0;
                int  x = WmCursorX(), y = WmCursorY();
                WmSetButtons(ev.buttons & 7);
                if (ev.dz) WmMouseOther(x, y, WM_MOUSE_WHEEL, ev.dz);
                if (mid != prev_mid) WmMouseOther(x, y, mid ? WM_MOUSE_MDOWN : WM_MOUSE_MUP, 0);
                prev_mid = mid;
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
                if (right && !prev_right && !left && !WmMouseOther(x, y, WM_MOUSE_RDOWN, 0)) desktop_right_press(x, y);
                if (!right && prev_right) WmMouseOther(x, y, WM_MOUSE_RUP, 0);
                prev_left = left;
                prev_right = right;
            } else if (ev.type == INPUT_KEY) {
                KeyEvent k;
                if (InputTranslateKey(&ev, &k)) desktop_key(&k);
            }
        }

        WmTick();
        UmPoll();                               /* reclaim exited programs */

        rtc_read(&t);
        if (t.minute != last_min) {
            last_min = t.minute;
            update_clock();
            WmInvalidate();
        }

        if (WmNeedsRedraw()) {
            /* Drawing needs only the desktop lock (built-in apps' painters
             * take the big one back, see WND.paint_lock_free): the other
             * CPUs keep entering the kernel meanwhile */
            bkl_release();
            WmComposite();              /* redraws the pointer too */
            bkl_acquire();
        }
        DesktopUnlock();
        /* Sleep until the next tick (10 ms: input is collected at the
         * tick) instead of spinning.  Sleeping in the scheduler, not
         * halting the CPU, leaves the CPU to its idle thread, which takes
         * work from busy CPUs meanwhile. */
        sched_sleep_tick();
    }
}

void DesktopRender(void)
{
    if (!g_ready) return;
    update_clock();
    WmComposite();
}
