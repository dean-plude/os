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
    if (strchr(name, '\\')) {                         /* a full path: the program there, else by its name */
        RamNode *f = RamfsResolve(NULL, name);
        GdiIcon *ic = f && !f->dir ? AppFileIcon(f) : NULL;
        if (ic) return ic;
        name = strrchr(name, '\\') + 1;
    }
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
    if (f->dir) { AppDrawFolderKindIcon(AppFolderKind(f), x, y, size); return; }
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
    /* the same flat style as Explorer's command bar: rounded, no hard
     * outline, a faint light edge */
    GdiRoundRect(r, 6, primary ? UI_ACCENT : UI_CARD, GDI_TRANSPARENT);
    if (!primary) GdiRoundBorderAlpha(r, 6, GDI_WHITE, 18);
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

/* -----------------------------------------------------------------------
 * The NovaOS icon style: one design language everywhere
 *
 *   - apps are rounded-square tiles (corner radius 22% of the size) with a
 *     top-to-bottom gradient and a white line glyph;
 *   - folders, documents and "This PC" are flat, two-tone shapes with the
 *     same proportional corner radius;
 *   - UI glyphs (sidebar, tray, toolbars) are single-colour line drawings.
 * Every line is 8% of the icon size thick (at least 1.4 px), with round
 * caps and joins, so icons of any size have the same visual weight.
 * Shapes are given in percent of the icon size.
 * ----------------------------------------------------------------------- */
typedef struct { signed char x, y; } P;

static int stroke_w(int s)                       /* 1/16 logical px */
{
    int w = s * 16 * 8 / 100;
    return w < 22 ? 22 : w;
}

static void poly(const P *p, int n, bool closed, int x, int y, int s, GdiColor c)
{
    int w = stroke_w(s);
    for (int i = 0; i + 1 < n; i++)
        GdiLine(pt(x, y, s, p[i].x, p[i].y), pt(x, y, s, p[i + 1].x, p[i + 1].y), w, c);
    if (closed && n > 2)
        GdiLine(pt(x, y, s, p[n - 1].x, p[n - 1].y), pt(x, y, s, p[0].x, p[0].y), w, c);
}

/* A circle (or ellipse: rx != ry) as a closed 24-sided outline */
static void ring(int x, int y, int s, int cx, int cy, int rx, int ry, GdiColor c)
{
    static const signed char cs[24] = { 100, 97, 87, 71, 50, 26, 0, -26, -50, -71, -87, -97,
                                        -100, -97, -87, -71, -50, -26, 0, 26, 50, 71, 87, 97 };
    int w = stroke_w(s);
    for (int i = 0; i < 24; i++) {
        int j = (i + 1) % 24;
        GdiPoint a = { x * 16 + s * 16 * (cx * 100 + rx * cs[i]) / 10000,
                       y * 16 + s * 16 * (cy * 100 + ry * cs[(i + 18) % 24]) / 10000 };
        GdiPoint b = { x * 16 + s * 16 * (cx * 100 + rx * cs[j]) / 10000,
                       y * 16 + s * 16 * (cy * 100 + ry * cs[(j + 18) % 24]) / 10000 };
        GdiLine(a, b, w, c);
    }
}

static void tile(int x, int y, int s, GdiColor top, GdiColor bot)
{
    GdiRoundGradV(RECT(x, y, s, s), s * 22 / 100, top, bot);
}

static GdiColor lighten(GdiColor c, int t) { return GdiLerp(c, GDI_WHITE, t); }

static void letter_tile(const AppInfo *a, int x, int y, int s)
{
    tile(x, y, s, lighten(a->color, 40), a->color);
    if (a->label[0]) {
        GdiColor fg = (a->color == GDI_WHITE) ? GDI_BLACK : GDI_WHITE;
        GdiTextCenter(x, y + (s - GDI_FONT_H) / 2, s, a->label, fg);
    }
}

static const P G_FOLDER[] = { {18,30}, {18,74}, {82,74}, {82,38}, {50,38}, {43,30} };
static const P G_PAGE[]   = { {28,16}, {28,84}, {72,84}, {72,32}, {56,16} };
static const P G_FOLD[]   = { {56,16}, {56,32}, {72,32} };

void AppDrawGlyph(Glyph g, int x, int y, int s, GdiColor c)
{
    switch (g) {
    case GL_PC: {
        static const P scr[] = { {12,20}, {88,20}, {88,66}, {12,66} };
        static const P stand[] = { {50,66}, {50,80} }, base[] = { {32,82}, {68,82} };
        poly(scr, 4, true, x, y, s, c); poly(stand, 2, false, x, y, s, c); poly(base, 2, false, x, y, s, c);
        break; }
    case GL_DOCUMENTS: case GL_FILE: {
        static const P l1[] = { {40,52}, {60,52} }, l2[] = { {40,66}, {60,66} };
        poly(G_PAGE, 5, true, x, y, s, c); poly(G_FOLD, 3, false, x, y, s, c);
        if (g == GL_DOCUMENTS) { poly(l1, 2, false, x, y, s, c); poly(l2, 2, false, x, y, s, c); }
        break; }
    case GL_DOWNLOADS: {
        static const P shaft[] = { {50,16}, {50,62} }, head[] = { {32,46}, {50,64}, {68,46} };
        static const P tray[] = { {20,82}, {80,82} };
        poly(shaft, 2, false, x, y, s, c); poly(head, 3, false, x, y, s, c); poly(tray, 2, false, x, y, s, c);
        break; }
    case GL_PICTURES: {
        static const P fr[] = { {14,22}, {86,22}, {86,78}, {14,78} };
        static const P mt[] = { {20,72}, {40,50}, {54,64}, {64,55}, {80,72} };
        poly(fr, 4, true, x, y, s, c); poly(mt, 5, false, x, y, s, c);
        ring(x, y, s, 66, 38, 6, 6, c);
        break; }
    case GL_PERSON: {
        static const P sh[] = { {20,84}, {26,68}, {38,60}, {62,60}, {74,68}, {80,84} };
        ring(x, y, s, 50, 34, 14, 14, c); poly(sh, 6, false, x, y, s, c);
        break; }
    case GL_CODE: {
        static const P l[] = { {36,28}, {16,50}, {36,72} }, r[] = { {64,28}, {84,50}, {64,72} };
        static const P sl[] = { {56,22}, {44,78} };
        poly(l, 3, false, x, y, s, c); poly(r, 3, false, x, y, s, c); poly(sl, 2, false, x, y, s, c);
        break; }
    case GL_WINDOWS: {
        static const P fr[] = { {14,22}, {86,22}, {86,78}, {14,78} }, bar[] = { {14,38}, {86,38} };
        poly(fr, 4, true, x, y, s, c); poly(bar, 2, false, x, y, s, c);
        break; }
    case GL_FOLDER:
        poly(G_FOLDER, 6, true, x, y, s, c);
        break;
    case GL_PLUS: {
        static const P v[] = { {50,20}, {50,80} }, h[] = { {20,50}, {80,50} };
        poly(v, 2, false, x, y, s, c); poly(h, 2, false, x, y, s, c);
        break; }
    case GL_SEARCH: {
        static const P hd[] = { {62,62}, {84,84} };
        ring(x, y, s, 44, 44, 26, 26, c); poly(hd, 2, false, x, y, s, c);
        break; }
    case GL_NETWORK: case GL_NETWORK_OFF: {
        static const P eq[] = { {16,50}, {84,50} };
        ring(x, y, s, 50, 50, 34, 34, c); ring(x, y, s, 50, 50, 15, 34, c);
        poly(eq, 2, false, x, y, s, c);
        if (g == GL_NETWORK_OFF) {
            static const P sl[] = { {14,86}, {86,14} };
            poly(sl, 2, false, x, y, s, c);
        }
        break; }
    case GL_CHEVRON: {
        static const P v[] = { {40,24}, {64,50}, {40,76} };
        poly(v, 3, false, x, y, s, c);
        break; }
    case GL_BACK: {
        static const P sh[] = { {80,50}, {22,50} }, hd[] = { {44,28}, {22,50}, {44,72} };
        poly(sh, 2, false, x, y, s, c); poly(hd, 3, false, x, y, s, c);
        break; }
    case GL_UP: {
        static const P sh[] = { {50,80}, {50,22} }, hd[] = { {28,44}, {50,22}, {72,44} };
        poly(sh, 2, false, x, y, s, c); poly(hd, 3, false, x, y, s, c);
        break; }
    case GL_GEAR: {
        static const signed char t[8][4] = { {50,10,50,22}, {78,22,70,30}, {90,50,78,50}, {78,78,70,70},
                                             {50,90,50,78}, {22,78,30,70}, {10,50,22,50}, {22,22,30,30} };
        ring(x, y, s, 50, 50, 28, 28, c); ring(x, y, s, 50, 50, 10, 10, c);
        for (int i = 0; i < 8; i++) {
            P l[2] = { { t[i][0], t[i][1] }, { t[i][2], t[i][3] } };
            poly(l, 2, false, x, y, s, c);
        }
        break; }
    case GL_POWER: {                            /* a ring open at the top, and a stroke */
        static const signed char cs[24] = { 100, 97, 87, 71, 50, 26, 0, -26, -50, -71, -87, -97,
                                            -100, -97, -87, -71, -50, -26, 0, 26, 50, 71, 87, 97 };
        for (int i = 20; i < 24 + 16; i++) {    /* from 300 to 240 degrees, the long way */
            int a = i % 24, b2 = (i + 1) % 24;
            GdiPoint p0 = { x * 16 + s * 16 * (5000 + 34 * cs[a]) / 10000, y * 16 + s * 16 * (5400 + 34 * cs[(a + 18) % 24]) / 10000 };
            GdiPoint p1 = { x * 16 + s * 16 * (5000 + 34 * cs[b2]) / 10000, y * 16 + s * 16 * (5400 + 34 * cs[(b2 + 18) % 24]) / 10000 };
            GdiLine(p0, p1, stroke_w(s), c);
        }
        static const P st[] = { {50,10}, {50,48} };
        poly(st, 2, false, x, y, s, c);
        break; }
    case GL_NOVA: {                             /* NovaOS: a four-pointed star */
        GdiPoint st[8] = { pt(x, y, s, 50, 4), pt(x, y, s, 60, 40), pt(x, y, s, 96, 50), pt(x, y, s, 60, 60),
                           pt(x, y, s, 50, 96), pt(x, y, s, 40, 60), pt(x, y, s, 4, 50), pt(x, y, s, 40, 40) };
        GdiFillPolygon(st, 8, c);
        break; }
    }
}

void AppDrawIcon(AppId id, int x, int y, int s)
{
    GdiColor w = GDI_WHITE;
    switch (id) {
    case APP_TERMINAL: {
        static const P chev[] = { {28,34}, {44,50}, {28,66} }, cur[] = { {52,68}, {72,68} };
        tile(x, y, s, GDI_C(0x4A, 0x50, 0x5E), GDI_C(0x1E, 0x21, 0x28));
        poly(chev, 3, false, x, y, s, w);
        poly(cur, 2, false, x, y, s, GDI_C(0x5C, 0xC8, 0xFF));
        break; }
    case APP_EXPLORER:
        tile(x, y, s, GDI_C(0xFF, 0xCF, 0x4D), GDI_C(0xF0, 0x98, 0x1C));
        poly(G_FOLDER, 6, true, x, y, s, w);
        break;
    case APP_NOTEPAD: {
        static const P l1[] = { {40,52}, {60,52} }, l2[] = { {40,66}, {60,66} };
        tile(x, y, s, GDI_C(0x62, 0xA4, 0xFF), GDI_C(0x2A, 0x64, 0xE0));
        poly(G_PAGE, 5, true, x, y, s, w); poly(G_FOLD, 3, false, x, y, s, w);
        poly(l1, 2, false, x, y, s, w); poly(l2, 2, false, x, y, s, w);
        break; }
    case APP_SETTINGS: {
        GdiColor top = GDI_C(0x8A, 0x93, 0xA4), bot = GDI_C(0x4A, 0x52, 0x62);
        tile(x, y, s, top, bot);
        int cx = x + s / 2, cy = y + s / 2;
        static const signed char dir[8][2] = { {0,-30},{21,-21},{30,0},{21,21},{0,30},{-21,21},{-30,0},{-21,-21} };
        for (int i = 0; i < 8; i++)
            GdiLine(pt(cx, cy, s, 0, 0), pt(cx, cy, s, dir[i][0], dir[i][1]), s * 16 * 14 / 100, w);
        GdiFillCircle(cx, cy, s * 24 / 100, w);
        GdiFillCircle(cx, cy, s * 10 / 100, GdiLerp(top, bot, 128));
        break; }
    case APP_CALENDAR: {
        static const P body[] = { {20,26}, {80,26}, {80,80}, {20,80} }, hdr[] = { {20,42}, {80,42} };
        static const P r1[] = { {36,18}, {36,32} }, r2[] = { {64,18}, {64,32} };
        tile(x, y, s, GDI_C(0xFF, 0x7A, 0x6E), GDI_C(0xE0, 0x3C, 0x3C));
        poly(body, 4, true, x, y, s, w); poly(hdr, 2, false, x, y, s, w);
        poly(r1, 2, false, x, y, s, w); poly(r2, 2, false, x, y, s, w);
        if (s >= 36) GdiTextCenter(x, y + s * 61 / 100 - GDI_FONT_H / 2, s, "31", w);
        else {
            for (int i = 0; i < 3; i++)
                GdiFillCircle(x + s * (34 + 16 * i) / 100, y + s * 62 / 100, s > 20 ? 2 : 1, w);
        }
        break; }
    case APP_NETSURF: {
        static const P eq[] = { {18,50}, {82,50} };
        tile(x, y, s, GDI_C(0x5E, 0x8E, 0xFF), GDI_C(0x2A, 0x4C, 0xD6));
        ring(x, y, s, 50, 50, 32, 32, w); ring(x, y, s, 50, 50, 14, 32, w);
        poly(eq, 2, false, x, y, s, w);
        break; }
    case APP_PHOTOS: {
        static const P mt[] = { {18,74}, {40,48}, {56,64}, {66,55}, {82,74} };
        static const P base[] = { {18,74}, {82,74} };
        tile(x, y, s, GDI_C(0x4F, 0xDB, 0xC8), GDI_C(0x16, 0x94, 0xA8));
        poly(mt, 5, false, x, y, s, w); poly(base, 2, false, x, y, s, w);
        GdiFillCircle(x + s * 66 / 100, y + s * 32 / 100, s * 8 / 100 + 1, w);
        break; }
    default:
        letter_tile(AppGetInfo(id), x, y, s);
        break;
    }
}

/* Flat folder: a darker back with its tab, a lighter front */
void AppDrawFolderKindIcon(FolderKind k, int x, int y, int s)
{
    int r = s * 8 / 100 + 1;
    GdiRoundRect(RECT(x + s * 6 / 100, y + s * 16 / 100, s * 40 / 100, s * 24 / 100), r,
                 GDI_C(0xE3, 0x9E, 0x21), GDI_TRANSPARENT);
    GdiRoundRect(RECT(x + s * 6 / 100, y + s * 24 / 100, s * 88 / 100, s * 60 / 100), r,
                 GDI_C(0xE3, 0x9E, 0x21), GDI_TRANSPARENT);
    GdiRoundGradV(RECT(x + s * 6 / 100, y + s * 32 / 100, s * 88 / 100, s * 52 / 100), r,
                  GDI_C(0xFF, 0xD6, 0x62), GDI_C(0xF7, 0xB9, 0x3A));
    if (k == FOLDER_PLAIN || s < 28) return;
    /* the folder's purpose, as a glyph on its front */
    static const Glyph gl[] = { GL_FOLDER, GL_DOCUMENTS, GL_DOWNLOADS, GL_PICTURES, GL_CODE };
    int e = s * 36 / 100;
    AppDrawGlyph(gl[k], x + (s - e) / 2, y + s * 40 / 100, e, GDI_C(0xB0, 0x6E, 0x0C));
}

void AppDrawFolderIcon(int x, int y, int s) { AppDrawFolderKindIcon(FOLDER_PLAIN, x, y, s); }

/* Flat document: a white page with a folded corner */
void AppDrawFileIcon(int x, int y, int s)
{
    GdiPoint page[5] = { pt(x, y, s, 20, 6), pt(x, y, s, 60, 6), pt(x, y, s, 80, 26),
                         pt(x, y, s, 80, 94), pt(x, y, s, 20, 94) };
    GdiFillPolygon(page, 5, GDI_C(0xC4, 0xCA, 0xD4));
    int in = s >= 24 ? 16 * 1 : 8;               /* 1 px (1/2 px when tiny) border */
    GdiPoint inner[5] = { { page[0].x + in, page[0].y + in }, { page[1].x - in / 2, page[1].y + in },
                          { page[2].x - in, page[2].y + in / 2 }, { page[3].x - in, page[3].y - in },
                          { page[4].x + in, page[4].y - in } };
    GdiFillPolygon(inner, 5, GDI_C(0xFA, 0xFB, 0xFD));
    GdiPoint fold[3] = { pt(x, y, s, 60, 6), pt(x, y, s, 60, 26), pt(x, y, s, 80, 26) };
    GdiFillPolygon(fold, 3, GDI_C(0xDD, 0xE2, 0xEA));
    if (s >= 20)
        for (int i = 0; i < 3; i++)
            GdiLine(pt(x, y, s, 32, 46 + i * 14), pt(x, y, s, i == 2 ? 56 : 68, 46 + i * 14),
                    s * 16 * 5 / 100 < 16 ? 16 : s * 16 * 5 / 100, GDI_C(0x9A, 0xA6, 0xB8));
}

/* Flat monitor: "This PC" */
void AppDrawPcIcon(int x, int y, int s)
{
    int r = s * 8 / 100 + 1;
    GdiRoundRect(RECT(x + s * 6 / 100, y + s * 12 / 100, s * 88 / 100, s * 60 / 100), r,
                 GDI_C(0x2A, 0x30, 0x3C), GDI_TRANSPARENT);
    GdiRoundGradV(RECT(x + s * 10 / 100, y + s * 16 / 100, s * 80 / 100, s * 52 / 100), r > 1 ? r - 1 : 1,
                  GDI_C(0x6C, 0xB6, 0xFF), GDI_C(0x2F, 0x6B, 0xE8));
    GdiFillRect(RECT(x + s * 44 / 100, y + s * 72 / 100, s * 12 / 100, s * 10 / 100), GDI_C(0x8A, 0x94, 0xA6));
    GdiRoundRect(RECT(x + s * 28 / 100, y + s * 80 / 100, s * 44 / 100, s * 7 / 100 + 1), s * 3 / 100 + 1,
                 GDI_C(0xB4, 0xBC, 0xC8), GDI_TRANSPARENT);
}

FolderKind AppFolderKind(const RamNode *d)
{
    if (!d || !d->dir || !d->parent || d->parent != RamfsRoot()) return FOLDER_PLAIN;
    static const struct { const char *name; FolderKind k; } known[] = {
        { "Documents", FOLDER_DOCUMENTS }, { "Downloads", FOLDER_DOWNLOADS },
        { "Pictures", FOLDER_PICTURES }, { "Projects", FOLDER_PROJECTS },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (name_is(d->name, known[i].name)) return known[i].k;
    return FOLDER_PLAIN;
}

/* A window with a coloured title strip and the program's initial */
void AppDrawProgramIcon(const char *name, int x, int y, int s)
{
    if (IconDraw(AppProgramIcon(name), x, y, s)) return;
    /* A program without an icon: a tile in a colour picked from its name,
     * with its initial (or, when tiny, a window glyph) */
    static const GdiColor tint[] = {
        GDI_C(0x3A, 0x7B, 0xD5), GDI_C(0x2E, 0xA0, 0x6A), GDI_C(0xC8, 0x5A, 0x3A),
        GDI_C(0x8A, 0x4A, 0xC8), GDI_C(0x1E, 0x9A, 0xA8), GDI_C(0xB8, 0x8A, 0x1E),
    };
    unsigned h = 0;
    const char *nm = name && strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    for (const char *p = nm; p && *p && *p != '.'; p++) h = h * 31 + (unsigned char)(*p | 0x20);
    GdiColor c = tint[h % (sizeof(tint) / sizeof(tint[0]))];
    tile(x, y, s, lighten(c, 50), c);
    if (s >= 24) {
        const char *base = name && strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
        char initial[2] = { base && base[0] ? (char)(base[0] & ~0x20) : '?', 0 };
        GdiTextBold(x + (s - GdiTextBoldW(initial)) / 2, y + (s - GDI_FONT_H) / 2, initial, GDI_WHITE);
    } else {
        AppDrawGlyph(GL_WINDOWS, x + s / 6, y + s / 6, s - s / 3, GDI_WHITE);
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
