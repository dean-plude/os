/*
 * apps.c — app registry, launching, icons, shared UI helpers, and the
 * "not available yet" dialog for placeholder apps.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"

static const AppInfo g_apps[APP_COUNT] = {
    [APP_TERMINAL]    = { "Terminal",               ">_", GDI_C(0x1E,0x1E,0x1E), true,  true  },
    [APP_EXPLORER]    = { "File Explorer",          "",   GDI_C(0xF6,0xCE,0x52), true,  true  },
    [APP_NOTEPAD]     = { "Notepad",                "",   GDI_C(0x4A,0x7B,0xD0), true,  true  },
    [APP_SETTINGS]    = { "Settings",               "",   GDI_C(0x5A,0x5A,0x64), true,  false },
    [APP_CALENDAR]    = { "Calendar",               "",   GDI_C(0xD0,0x40,0x40), true,  false },
    [APP_EDGE]        = { "Microsoft Edge",         "e",  GDI_C(0x1A,0x8A,0xC8), false, false },
    [APP_STORE]       = { "Microsoft Store",        "S",  GDI_C(0x18,0x6A,0xD8), false, false },
    [APP_PHOTOS]      = { "Photos",                 "P",  GDI_C(0x2E,0xA0,0x8A), false, false },
    [APP_XBOX]        = { "Xbox",                   "X",  GDI_C(0x10,0x7C,0x10), false, false },
    [APP_SKYPE]       = { "Skype",                  "S",  GDI_C(0x1E,0x9A,0xE0), false, false },
    [APP_PHOTOSHOP]   = { "Adobe Photoshop 2025",   "Ps", GDI_C(0x05,0x1A,0x2E), false, false },
    [APP_ILLUSTRATOR] = { "Adobe Illustrator 2025", "Ai", GDI_C(0x2A,0x12,0x00), false, false },
    [APP_CLIPCHAMP]   = { "Clipchamp",              "C",  GDI_C(0x7A,0x3C,0xE0), false, false },
    [APP_VSTUDIO]     = { "Visual Studio",          "VS", GDI_C(0x6A,0x2A,0xC8), false, false },
    [APP_PAINT]       = { "Paint",                  "",   GDI_C(0xF2,0xC8,0x3A), false, false },
    [APP_TIPS]        = { "Microsoft Tips",         "",   GDI_C(0xF2,0xD0,0x3A), false, false },
    [APP_POWERPOINT]  = { "PowerPoint",             "P",  GDI_C(0xD0,0x4A,0x28), false, false },
    [APP_BLENDER]     = { "Blender",                "",   GDI_C(0xE8,0x7A,0x22), false, false },
    [APP_BING]        = { "Bing Search",            "b",  GDI_C(0x10,0xA0,0x88), false, false },
    [APP_SOLITAIRE]   = { "Solitaire",              "Sol",GDI_C(0x12,0x6A,0x3A), false, false },
    [APP_TODO]        = { "Microsoft To Do",        "",   GDI_C(0x2A,0x6A,0xE0), false, false },
};

const AppInfo *AppGetInfo(AppId id)
{
    return (id >= 0 && id < APP_COUNT) ? &g_apps[id] : &g_apps[APP_TERMINAL];
}

/* -----------------------------------------------------------------------
 * Launching
 * ----------------------------------------------------------------------- */
#define RECENT_MAX 9
static AppId g_recent[RECENT_MAX];
static int   g_recent_n;

static void note_recent(AppId id)
{
    int i = 0;
    while (i < g_recent_n && g_recent[i] != id) i++;
    if (i == g_recent_n && g_recent_n < RECENT_MAX) g_recent_n++;
    if (i == RECENT_MAX) i = RECENT_MAX - 1;
    for (; i > 0; i--) g_recent[i] = g_recent[i - 1];
    g_recent[0] = id;
}

int AppRecent(AppId *out, int max)
{
    int n = g_recent_n < max ? g_recent_n : max;
    for (int i = 0; i < n; i++) out[i] = g_recent[i];
    return n;
}

void AppLaunch(AppId id)
{
    const AppInfo *a = AppGetInfo(id);
    note_recent(id);
    if (!a->multi) {
        WND *w = WmFindApp(id);
        if (w) { WmSetActive(w); return; }
    }
    switch (id) {
    case APP_TERMINAL: TerminalOpen(); break;
    case APP_EXPLORER: ExplorerOpen(NULL); break;
    case APP_NOTEPAD:  NotepadOpen(NULL); break;
    case APP_SETTINGS: SettingsOpen(); break;
    case APP_CALENDAR: CalendarOpen(); break;
    default:           PlaceholderOpen(id); break;
    }
}

void AppActivate(AppId id)
{
    WND *w = WmFindApp(id);
    if (!w)                                   AppLaunch(id);
    else if (w->active && !w->minimized)      WmMinimize(w);
    else                                      WmSetActive(w);
}

bool AppByName(const char *name, AppId *out)
{
    static const struct { const char *cmd; AppId id; } names[] = {
        { "terminal", APP_TERMINAL }, { "cmd", APP_TERMINAL },
        { "explorer", APP_EXPLORER }, { "files", APP_EXPLORER },
        { "notepad",  APP_NOTEPAD  },
        { "settings", APP_SETTINGS }, { "control", APP_SETTINGS },
        { "calendar", APP_CALENDAR }, { "clock", APP_CALENDAR },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *a = names[i].cmd, *b = name;
        while (*a && (*b | 0x20) == *a) { a++; b++; }
        if (!*a && !*b) { *out = names[i].id; return true; }
    }
    return false;
}

static void title_icon(int app, int x, int y, int size)
{
    AppDrawIcon((AppId)app, x, y, size);
}

void AppOpenFolder(RamNode *dir) { ExplorerOpen(dir); }
void AppOpenFile(RamNode *file)  { NotepadOpen(file); }

WND *AppCreateWindow(AppId id, const char *title, int client_w, int client_h,
                     GdiColor client_bg)
{
    static int cascade;
    GdiRect work = WmWorkArea();
    int w = client_w + 2, h = client_h + WM_TITLEBAR_H + 1;
    if (w > work.w - 40) w = work.w - 40;
    if (h > work.h - 40) h = work.h - 40;
    int x = work.x + (work.w - w) / 2 - 120 + (cascade % 8) * 32;
    int y = work.y + 30 + (cascade % 8) * 28;
    if (x < work.x + 10) x = work.x + 10;
    if (x + w > work.x + work.w) x = work.x + work.w - w;
    if (y + h > work.y + work.h) y = work.y + work.h - h;
    cascade++;

    const AppInfo *a = AppGetInfo(id);
    UINT32 style = (id == APP_CALENDAR || !a->builtin) ? WS_TOOLWINDOW : WS_OVERLAPPED;
    if (id == APP_CALENDAR) style |= WS_MINMAXBTN;
    WmSetIconPainter(title_icon);
    WND *wnd = WmCreateWindow(title, RECT(x, y, w, h), style, client_bg,
                              a->color, NULL, NULL);
    if (wnd) wnd->app = id;
    return wnd;
}

/* -----------------------------------------------------------------------
 * UI helpers
 * ----------------------------------------------------------------------- */
bool UiHit(GdiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void UiButton(GdiRect r, const char *label, bool primary)
{
    GdiRoundRect(r, 4, primary ? UI_ACCENT : UI_CARD,
                 primary ? GDI_TRANSPARENT : UI_LINE);
    GdiTextCenter(r.x, r.y + (r.h - GDI_FONT_H) / 2, r.w, label,
                  primary ? GDI_BLACK : UI_TEXT);
}

void AppFormatSize(UINT64 bytes, char *buf, int cap)
{
    if (bytes < 1024) {
        ksnprintf(buf, (size_t)cap, "%u bytes", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        unsigned tenths = (unsigned)((bytes * 10 + 512) / 1024);
        ksnprintf(buf, (size_t)cap, "%u.%u KB", tenths / 10, tenths % 10);
    } else {
        unsigned tenths = (unsigned)((bytes * 10 + 512 * 1024) / (1024 * 1024));
        ksnprintf(buf, (size_t)cap, "%u.%u MB", tenths / 10, tenths % 10);
    }
}

void AppCpuName(char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    buf[0] = '\0';
    if (cpuid(0x80000000u, 0).eax < 0x80000004u) {
        ksnprintf(buf, (size_t)cap, "x86-64 processor");
        return;
    }
    char brand[49];
    for (UINT32 i = 0; i < 3; i++) {
        CpuidResult r = cpuid(0x80000002u + i, 0);
        memcpy(brand + i * 16 + 0,  &r.eax, 4);
        memcpy(brand + i * 16 + 4,  &r.ebx, 4);
        memcpy(brand + i * 16 + 8,  &r.ecx, 4);
        memcpy(brand + i * 16 + 12, &r.edx, 4);
    }
    brand[48] = '\0';
    const char *p = brand;
    while (*p == ' ') p++;                       /* brand is left-padded */
    ksnprintf(buf, (size_t)cap, "%s", p);
}

void AppUptime(char *buf, int cap)
{
    UINT64 s = sched_ticks() / 100;
    unsigned h = (unsigned)(s / 3600), m = (unsigned)((s / 60) % 60), sec = (unsigned)(s % 60);
    if (h) ksnprintf(buf, (size_t)cap, "%uh %02um %02us", h, m, sec);
    else   ksnprintf(buf, (size_t)cap, "%um %02us", m, sec);
}

/* -----------------------------------------------------------------------
 * Icons
 * ----------------------------------------------------------------------- */
/* Point at (x + s*px/100, y + s*py/100) in 1/16 logical px */
static GdiPoint pt(int x, int y, int s, int px, int py)
{
    return (GdiPoint){ x * 16 + s * 16 * px / 100, y * 16 + s * 16 * py / 100 };
}

static void letter_tile(const AppInfo *a, int x, int y, int s)
{
    GdiRoundRect(RECT(x, y, s, s), s / 4, a->color, GDI_TRANSPARENT);
    if (a->label[0]) {
        GdiColor fg = (a->color == GDI_WHITE) ? GDI_BLACK : GDI_WHITE;
        GdiTextCenter(x, y + (s - GDI_FONT_H) / 2, s, a->label, fg);
    }
}

void AppDrawIcon(AppId id, int x, int y, int s)
{
    int u = s / 16 > 1 ? s / 16 : 1;         /* line unit */
    switch (id) {
    case APP_TERMINAL:
        GdiRoundRect(RECT(x, y, s, s), s / 5, GDI_C(0x1E, 0x1E, 0x1E), GDI_C(0x55, 0x55, 0x55));
        GdiLine(pt(x, y, s, 26, 34), pt(x, y, s, 44, 50), s * 16 / 11, GDI_C(0xE8, 0xE8, 0xE8));
        GdiLine(pt(x, y, s, 44, 50), pt(x, y, s, 26, 66), s * 16 / 11, GDI_C(0xE8, 0xE8, 0xE8));
        GdiFillRect(RECT(x + s * 50 / 100, y + s * 62 / 100, s * 24 / 100, u + 1), UI_ACCENT);
        break;
    case APP_EXPLORER:
        GdiRoundRect(RECT(x + s * 8 / 100, y + s * 18 / 100, s * 40 / 100, s * 24 / 100),
                     s / 16, GDI_C(0xE0, 0xA8, 0x2E), GDI_TRANSPARENT);
        GdiRoundGradV(RECT(x + s * 8 / 100, y + s * 28 / 100, s * 84 / 100, s * 56 / 100),
                      s / 12, GDI_C(0xFF, 0xD8, 0x6B), GDI_C(0xF0, 0xB0, 0x30));
        break;
    case APP_NOTEPAD:
        GdiRoundRect(RECT(x + s * 18 / 100, y + s * 8 / 100, s * 64 / 100, s * 84 / 100),
                     s / 12, GDI_C(0xF5, 0xF5, 0xF5), GDI_C(0xC8, 0xC8, 0xC8));
        for (int i = 0; i < 4; i++)
            GdiFillRect(RECT(x + s * 28 / 100, y + s * 26 / 100 + i * s * 14 / 100,
                             s * (i == 3 ? 26 : 44) / 100, u), GDI_C(0x4A, 0x7B, 0xD0));
        break;
    case APP_SETTINGS: {
        GdiRoundRect(RECT(x, y, s, s), s / 5, GDI_C(0x3A, 0x3E, 0x46), GDI_TRANSPARENT);
        GdiColor g = GDI_C(0xC8, 0xCE, 0xD6);
        int cx = x + s / 2, cy = y + s / 2;
        static const int dir[8][2] = { {0,-40},{28,-28},{40,0},{28,28},{0,40},{-28,28},{-40,0},{-28,-28} };
        for (int i = 0; i < 8; i++)
            GdiLine(pt(cx, cy, s, 0, 0), pt(cx, cy, s, dir[i][0], dir[i][1]), s * 16 / 7, g);
        GdiFillCircle(cx, cy, s * 28 / 100, g);
        GdiFillCircle(cx, cy, s * 12 / 100, GDI_C(0x3A, 0x3E, 0x46));
        break; }
    case APP_CALENDAR: {
        int r = s / 5;
        GdiRoundRect(RECT(x, y, s, s), r, GDI_WHITE, GDI_TRANSPARENT);
        GdiRoundRect(RECT(x, y, s, s * 30 / 100 + r), r, GDI_C(0xD0, 0x40, 0x40), GDI_TRANSPARENT);
        GdiFillRect(RECT(x, y + s * 30 / 100, s, r), GDI_WHITE);
        GdiTextCenter(x, y + s * 30 / 100 + (s * 70 / 100 - GDI_FONT_H) / 2, s, "31",
                      GDI_C(0x30, 0x30, 0x30));
        break; }
    default:
        letter_tile(AppGetInfo(id), x, y, s);
        break;
    }
}

/* -----------------------------------------------------------------------
 * "Not available yet" dialog for placeholder apps
 * ----------------------------------------------------------------------- */
#define PH_W 440
#define PH_H 170

static GdiRect ph_ok(const WND *w)
{
    GdiRect c = WmClientRect(w);
    return RECT(c.x + c.w - 112, c.y + c.h - 48, 96, 32);
}

static void ph_paint(WND *w)
{
    GdiRect c = WmClientRect(w);
    AppId id = (AppId)w->app;
    AppDrawIcon(id, c.x + 24, c.y + 24, 48);
    GdiTextBold(c.x + 92, c.y + 26, AppGetInfo(id)->name, UI_TEXT);
    GdiTextT(c.x + 92, c.y + 48, "isn't available on NovaOS yet.", UI_TEXT2);
    GdiTextT(c.x + 92, c.y + 70, "Windows apps will run once NovaOS can load", UI_TEXT3);
    GdiTextT(c.x + 92, c.y + 88, "real .exe files in user mode.", UI_TEXT3);
    UiButton(ph_ok(w), "OK", true);
}

static void ph_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    GdiRect c = WmClientRect(w);
    if (msg == WM_MOUSE_UP && UiHit(ph_ok(w), x + c.x, y + c.y))
        WmDestroyWindow(w);
}

static void ph_key(WND *w, const KeyEvent *k)
{
    if (k->scancode == KEY_ENTER || k->scancode == KEY_ESC)
        WmDestroyWindow(w);
}

void PlaceholderOpen(AppId id)
{
    WND *w = AppCreateWindow(id, AppGetInfo(id)->name, PH_W, PH_H, UI_BG);
    if (!w) return;
    w->on_paint = ph_paint;
    w->on_mouse = ph_mouse;
    w->on_key   = ph_key;
}
