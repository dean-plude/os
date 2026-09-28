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
#include "../um/um.h"
#include "../gdi/icon.h"

static const AppInfo g_apps[APP_COUNT] = {
    [APP_TERMINAL]    = { "Terminal",               ">_", GDI_C(0x1E,0x1E,0x1E), true,  true  },
    [APP_EXPLORER]    = { "File Explorer",          "",   GDI_C(0xF6,0xCE,0x52), true,  true  },
    [APP_NOTEPAD]     = { "Notepad",                "",   GDI_C(0x4A,0x7B,0xD0), true,  true  },
    [APP_SETTINGS]    = { "Settings",               "",   GDI_C(0x5A,0x5A,0x64), true,  false },
    [APP_CALENDAR]    = { "Calendar",               "",   GDI_C(0xD0,0x40,0x40), true,  false },
    [APP_NETSURF]     = { "NetSurf",                "",   GDI_C(0x3A,0x6E,0xF0), true,  true  },
    [APP_STORE]       = { "Microsoft Store",        "S",  GDI_C(0x18,0x6A,0xD8), false, false },
    [APP_PHOTOS]      = { "Photos",                 "",   GDI_C(0x2E,0xA0,0x8A), true,  true  },
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

/* NetSurf is a Windows program (tools/build_netsurf.py); started from the
 * desktop it has no console and runs on its own until its window closes */
static void netsurf_launch(void)
{
    RamNode *exe = RamfsResolve(NULL, "\\Programs\\NetSurf\\netsurf.exe");
    char err[160] = "not installed";
    UmProcess *p = exe ? UmSpawn(exe, "netsurf", exe->parent, NULL, err, sizeof(err)) : NULL;
    if (p) UmDetach(p);
    else   kprintf("[APPS] Cannot start NetSurf: %s\n", err);
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
    case APP_NETSURF:  netsurf_launch(); break;
    case APP_PHOTOS:   PhotosOpen(NULL); break;
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
        { "browser",  APP_NETSURF  },    /* "netsurf" itself runs the program */
        { "photos",   APP_PHOTOS   }, { "pictures", APP_PHOTOS },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *a = names[i].cmd, *b = name;
        while (*a && (*b | 0x20) == *a) { a++; b++; }
        if (!*a && !*b) { *out = names[i].id; return true; }
    }
    return false;
}

static bool title_icon(const WND *w, int x, int y, int size)
{
    if (w->app < 0 && !w->program[0]) return false;
    AppDrawWindowIcon(w, x, y, size);
    return true;
}

void AppInit(void)
{
    WmSetIconPainter(title_icon);
}

/* -----------------------------------------------------------------------
 * Recent documents and folders (kept as paths: nodes may be deleted)
 * ----------------------------------------------------------------------- */
#define RECENT_FILES 8
static char g_recent_files[RECENT_FILES][RAMFS_PATH_MAX];
static int  g_recent_files_n;

void AppNoteRecentFile(RamNode *node)
{
    if (!node || node == RamfsRoot()) return;
    char path[RAMFS_PATH_MAX];
    RamfsPath(node, path, sizeof(path));
    int i = 0;
    while (i < g_recent_files_n && strcmp(g_recent_files[i], path)) i++;
    if (i == g_recent_files_n && g_recent_files_n < RECENT_FILES) g_recent_files_n++;
    if (i == RECENT_FILES) i = RECENT_FILES - 1;
    for (; i > 0; i--) memcpy(g_recent_files[i], g_recent_files[i - 1], RAMFS_PATH_MAX);
    memcpy(g_recent_files[0], path, RAMFS_PATH_MAX);
}

int AppRecentFiles(RamNode **out, int max)
{
    int n = 0;
    for (int i = 0; i < g_recent_files_n && n < max; i++) {
        RamNode *node = RamfsResolve(NULL, g_recent_files[i]);
        if (node) out[n++] = node;
    }
    return n;
}

void AppOpenFolder(RamNode *dir)
{
    if (dir) AppNoteRecentFile(dir);
    ExplorerOpen(dir);
}

static bool has_ext(const char *name, const char *ext)     /* case-insensitive */
{
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++)
        if ((name[n - e + i] | 0x20) != (ext[i] | 0x20)) return false;
    return true;
}

typedef enum { FT_OTHER, FT_TEXT, FT_ICON, FT_PNG, FT_EXE, FT_DLL, FT_CURSOR } FileType;

static FileType file_type(const RamNode *f)
{
    if (!f || f->dir) return FT_OTHER;
    if (has_ext(f->name, ".ico")) return FT_ICON;
    if (has_ext(f->name, ".cur")) return FT_CURSOR;
    if (has_ext(f->name, ".png")) return FT_PNG;
    if (has_ext(f->name, ".exe")) return FT_EXE;
    if (has_ext(f->name, ".dll")) return FT_DLL;
    if (has_ext(f->name, ".txt") || has_ext(f->name, ".md") || has_ext(f->name, ".log") ||
        has_ext(f->name, ".ini") || has_ext(f->name, ".c") || has_ext(f->name, ".h"))
        return FT_TEXT;
    return FT_OTHER;
}

const char *AppFileTypeName(const RamNode *f)
{
    if (!f) return "";
    if (f->dir) return "File folder";
    switch (file_type(f)) {
    case FT_ICON:   return "Icon";
    case FT_CURSOR: return "Cursor";
    case FT_PNG:    return "PNG image";
    case FT_EXE:    return "Application";
    case FT_DLL:    return "Application extension";
    case FT_TEXT:   return "Text Document";
    default:        return "File";
    }
}

void AppOpenFile(RamNode *file)
{
    if (!file) { NotepadOpen(NULL); return; }
    AppNoteRecentFile(file);
    switch (file_type(file)) {
    case FT_ICON: case FT_CURSOR: case FT_PNG:
        PhotosOpen(file);
        break;
    case FT_EXE:
        AppRunProgram(file, file->name);
        break;
    default:
        NotepadOpen(file);
        break;
    }
}

/* -----------------------------------------------------------------------
 * File and program icons (.ico files, icons in .exe/.dll resources, PNG
 * thumbnails), decoded once and cached by file.  Only the desktop thread
 * uses the cache, and program threads change files only under the desktop
 * lock, so nothing changes under a drawing call.
 * ----------------------------------------------------------------------- */
#define ICON_CACHE 48
static struct {
    const RamNode *node;          /* compared, never dereferenced */
    const char    *data;          /* the file's contents when decoded */
    UINT32         size;
    GdiIcon       *icon;          /* NULL: the file has no usable icon */
    UINT32         used;
} g_icache[ICON_CACHE];
static UINT32 g_icache_clock;

GdiIcon *AppFileIcon(RamNode *f)
{
    FileType t = file_type(f);
    if (t != FT_ICON && t != FT_CURSOR && t != FT_PNG && t != FT_EXE && t != FT_DLL) return NULL;
    int slot = -1, lru = 0;
    for (int i = 0; i < ICON_CACHE; i++) {
        if (g_icache[i].node == f) { slot = i; break; }
        if (g_icache[i].used < g_icache[lru].used) lru = i;
    }
    if (slot >= 0 && g_icache[slot].data == f->data && g_icache[slot].size == f->size) {
        g_icache[slot].used = ++g_icache_clock;
        return g_icache[slot].icon;
    }
    if (slot < 0) slot = lru;                 /* evict the least recently used */
    IconFree(g_icache[slot].icon);
    GdiIcon *ic = NULL;
    if (f->data && f->size) {
        if (t == FT_PNG)                   ic = IconFromPng(f->data, f->size);
        else if (t == FT_EXE || t == FT_DLL) ic = IconFromPe(f->data, f->size);
        else                               ic = IconLoad(f->data, f->size);
    }
    g_icache[slot].node = f;
    g_icache[slot].data = f->data;
    g_icache[slot].size = f->size;
    g_icache[slot].icon = ic;
    g_icache[slot].used = ++g_icache_clock;
    return ic;
}

/* C:\Programs\NAME\FILE or C:\Programs\FILE */
static RamNode *program_file(const char *base, const char *ext)
{
    char path[RAMFS_PATH_MAX];
    ksnprintf(path, sizeof(path), "\\Programs\\%s\\%s%s", base, base, ext);
    RamNode *n = RamfsResolve(NULL, path);
    if (!n) {
        ksnprintf(path, sizeof(path), "\\Programs\\%s%s", base, ext);
        n = RamfsResolve(NULL, path);
    }
    return n && !n->dir ? n : NULL;
}

GdiIcon *AppProgramIcon(const char *name)
{
    char base[RAMFS_NAME_MAX];
    if (!name || !*name) return NULL;
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    size_t n = strlen(base);
    if (n > 4 && has_ext(base, ".exe")) base[n - 4] = '\0';
    GdiIcon *ic = AppFileIcon(program_file(base, ".ico"));   /* NAME.ico beside it */
    if (!ic) ic = AppFileIcon(program_file(base, ".exe"));   /* else the program's own */
    return ic;
}

void AppDrawWindowIcon(const WND *w, int x, int y, int size)
{
    if (w->app >= 0) { AppDrawIcon((AppId)w->app, x, y, size); return; }
    AppDrawProgramIcon(w->program[0] ? w->program : w->title, x, y, size);
}

void AppDrawNodeIcon(RamNode *f, int x, int y, int size)
{
    if (!f) return;
    if (f->dir) { AppDrawFolderIcon(x, y, size); return; }
    if (IconDraw(AppFileIcon(f), x, y, size)) return;
    if (file_type(f) == FT_EXE) AppDrawProgramIcon(f->name, x, y, size);
    else                        AppDrawFileIcon(x, y, size);
}

/* -----------------------------------------------------------------------
 * Windows programs
 * ----------------------------------------------------------------------- */
static bool name_is(const char *a, const char *b)       /* case-insensitive */
{
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return !*a && !*b;
}

int AppForProgram(const char *exe_name)
{
    if (exe_name && (name_is(exe_name, "netsurf.exe") || name_is(exe_name, "netsurf")))
        return APP_NETSURF;
    return -1;
}

/* Programs in C:\Programs are found by name from anywhere; others run
 * from their own folder */
static bool in_programs(const RamNode *n)
{
    RamNode *progs = RamfsResolve(NULL, "\\Programs");
    for (const RamNode *p = n->parent; p; p = p->parent)
        if (p == progs) return true;
    return false;
}

void AppRunProgram(RamNode *exe, const char *cmdline)
{
    if (!exe) return;
    int app = AppForProgram(exe->name);
    if (app >= 0) { AppLaunch((AppId)app); return; }
    /* In a Terminal: console programs need one, and GUI programs report
     * their exit status there */
    TerminalRun(cmdline && *cmdline ? cmdline : exe->name, in_programs(exe) ? NULL : exe->parent);
}

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
    case APP_NETSURF: {                         /* a globe on NetSurf blue */
        GdiRoundGradV(RECT(x, y, s, s), s / 5, GDI_C(0x4C, 0x8C, 0xF8), GDI_C(0x24, 0x52, 0xD8));
        int cx = x + s / 2, cy = y + s / 2, r = s * 32 / 100;
        GdiColor ink = GDI_C(0x16, 0x2A, 0x5C), sea = GDI_C(0x8C, 0xD4, 0xFF);
        GdiFillCircle(cx, cy, r, ink);
        GdiFillCircle(cx, cy, r - u - 1, sea);
        GdiLine(pt(cx, cy, s, -30, 0), pt(cx, cy, s, 30, 0), s * 16 / 22, ink);      /* equator */
        GdiLine(pt(cx, cy, s, 0, -30), pt(cx, cy, s, 0, 30), s * 16 / 22, ink);      /* meridian */
        GdiLine(pt(cx, cy, s, -26, -14), pt(cx, cy, s, 26, -14), s * 16 / 28, ink);  /* latitudes */
        GdiLine(pt(cx, cy, s, -26, 14), pt(cx, cy, s, 26, 14), s * 16 / 28, ink);
        break; }
    case APP_PHOTOS: {                          /* a landscape: sky, sun, hills */
        GdiRoundGradV(RECT(x, y, s, s), s / 5, GDI_C(0x5A, 0xC8, 0xF0), GDI_C(0x2E, 0x8C, 0xD8));
        GdiFillCircle(x + s * 68 / 100, y + s * 32 / 100, s * 11 / 100, GDI_C(0xFF, 0xE0, 0x6A));
        GdiPoint hill[3] = { pt(x, y, s, 8, 80), pt(x, y, s, 40, 40), pt(x, y, s, 72, 80) };
        GdiFillPolygon(hill, 3, GDI_C(0x2E, 0xA0, 0x6A));
        GdiPoint hill2[3] = { pt(x, y, s, 44, 80), pt(x, y, s, 66, 54), pt(x, y, s, 92, 80) };
        GdiFillPolygon(hill2, 3, GDI_C(0x1E, 0x7A, 0x50));
        GdiFillRect(RECT(x + s * 8 / 100, y + s * 80 / 100 - 1, s * 84 / 100, u + 1), GDI_C(0x1E, 0x7A, 0x50));
        break; }
    default:
        letter_tile(AppGetInfo(id), x, y, s);
        break;
    }
}

void AppDrawFolderIcon(int x, int y, int s)
{
    GdiRoundRect(RECT(x + s * 8 / 100, y + s * 18 / 100, s * 40 / 100, s * 24 / 100),
                 s / 16 + 1, GDI_C(0xE0, 0xA8, 0x2E), GDI_TRANSPARENT);
    GdiRoundGradV(RECT(x + s * 8 / 100, y + s * 28 / 100, s * 84 / 100, s * 56 / 100),
                  s / 12 + 1, GDI_C(0xFF, 0xD8, 0x6B), GDI_C(0xF0, 0xB0, 0x30));
}

void AppDrawFileIcon(int x, int y, int s)
{
    int u = s / 16 > 1 ? s / 16 : 1;
    GdiRoundRect(RECT(x + s * 20 / 100, y + s * 8 / 100, s * 60 / 100, s * 84 / 100),
                 s / 12 + 1, GDI_C(0xF5, 0xF5, 0xF5), GDI_C(0xB8, 0xB8, 0xB8));
    for (int i = 0; i < 3; i++)
        GdiFillRect(RECT(x + s * 30 / 100, y + s * 30 / 100 + i * s * 16 / 100,
                         s * (i == 2 ? 24 : 40) / 100, u), GDI_C(0x8A, 0x8A, 0x8A));
}

/* A window with a coloured title strip and the program's initial */
void AppDrawProgramIcon(const char *name, int x, int y, int s)
{
    if (IconDraw(AppProgramIcon(name), x, y, s)) return;
    static const GdiColor tint[] = {
        GDI_C(0x3A, 0x7B, 0xD5), GDI_C(0x2E, 0xA0, 0x6A), GDI_C(0xC8, 0x5A, 0x3A),
        GDI_C(0x8A, 0x4A, 0xC8), GDI_C(0x1E, 0x9A, 0xA8), GDI_C(0xB8, 0x8A, 0x1E),
    };
    unsigned h = 0;
    for (const char *p = name; p && *p && *p != '.'; p++) h = h * 31 + (unsigned char)(*p | 0x20);
    GdiColor c = tint[h % (sizeof(tint) / sizeof(tint[0]))];
    GdiRoundRect(RECT(x + s / 16, y + s / 8, s - s / 8, s - s / 4), s / 8 + 1,
                 GDI_C(0xF2, 0xF2, 0xF2), GDI_C(0xB0, 0xB0, 0xB0));
    GdiRoundRect(RECT(x + s / 16, y + s / 8, s - s / 8, s / 4), s / 8 + 1, c, GDI_TRANSPARENT);
    GdiFillRect(RECT(x + s / 16, y + s / 8 + s / 8, s - s / 8, s / 8), c);
    char initial[2] = { name && name[0] ? (char)(name[0] & ~0x20) : '?', 0 };
    if (s >= 20) GdiTextCenter(x, y + s / 8 + s / 4 + (s - s / 2 - GDI_FONT_H) / 2, s, initial, c);
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
    GdiTextT(c.x + 92, c.y + 70, "NovaOS runs Windows programs from C:\\Programs,", UI_TEXT3);
    GdiTextT(c.x + 92, c.y + 88, "but this one isn't installed.", UI_TEXT3);
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
