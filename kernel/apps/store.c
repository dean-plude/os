/*
 * store.c — App Store: a catalog of open-source Windows programs that
 * NovaOS downloads to C:\Downloads and runs unmodified.
 *
 * The catalog is built in (name, publisher, download URL, and where the
 * installed program ends up).  "Get" fetches the installer over HTTP(S)
 * with the same asynchronous network operations the Terminal's wget
 * uses; "Install" runs the downloaded installer; "Open" starts the
 * program once its executable exists under C:\Programs.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../net/net.h"
#include "../um/um.h"

/* -----------------------------------------------------------------------
 * Catalog
 * ----------------------------------------------------------------------- */
enum { CAT_ALL, CAT_UTILITIES, CAT_INTERNET, CAT_MEDIA, CAT_GRAPHICS, CAT_OFFICE,
       CAT_DEVELOPER, CAT_INSTALLED, CAT_COUNT };

static const char *g_cat_name[CAT_COUNT] = {
    "All apps", "Utilities", "Internet", "Media", "Graphics", "Office", "Developer",
    "Installed",
};

typedef struct {
    const char *name;
    const char *publisher;
    const char *summary;       /* one line */
    int         category;
    const char *url;           /* the installer (or portable program) */
    const char *file;          /* saved as C:\Downloads\<file> */
    const char *exe;           /* installed program: C:\Programs\<exe> */
    bool        portable;      /* @file is the program itself: no install step */
    unsigned    size_mb;       /* approximate download size (0: under 1 MB) */
    const char *note;          /* NovaOS compatibility note */
    const char *label;         /* 1-3 letter tile label */
    GdiColor    color;         /* tile colour */
} StoreApp;

static const StoreApp g_catalog[] = {
    { "7-Zip", "Igor Pavlov", "File archiver with a high compression ratio (7z, zip, tar, ...)",
      CAT_UTILITIES, "https://www.7-zip.org/a/7z2603-x64.exe", "7z2603-x64.exe",
      "7-Zip\\7zFM.exe", false, 2, "Tested on NovaOS", "7z", GDI_C(0x25, 0x6E, 0xB8) },
    { "VLC media player", "VideoLAN", "Plays most video and audio files, discs and streams",
      CAT_MEDIA, "https://get.videolan.org/vlc/3.0.21/win64/vlc-3.0.21-win64.exe", "vlc-3.0.21-win64.exe",
      "VideoLAN\\VLC\\vlc.exe", false, 42, "Needs audio and video output NovaOS does not have yet", "VLC", GDI_C(0xF0, 0x7E, 0x1A) },
    { "Firefox", "Mozilla", "Fast, private web browser",
      CAT_INTERNET, "https://download.mozilla.org/?product=firefox-latest&os=win64&lang=en-US", "FirefoxSetup.exe",
      "Mozilla Firefox\\firefox.exe", false, 65, "Needs the sandbox and GPU APIs NovaOS does not have yet", "Fx", GDI_C(0xE6, 0x5A, 0x1C) },
    { "Thunderbird", "Mozilla", "Email, calendar and chat client",
      CAT_INTERNET, "https://download.mozilla.org/?product=thunderbird-latest&os=win64&lang=en-US", "ThunderbirdSetup.exe",
      "Mozilla Thunderbird\\thunderbird.exe", false, 70, "Same runtime as Firefox: not expected to start yet", "Tb", GDI_C(0x1D, 0x66, 0xB4) },
    { "Notepad++", "Don Ho", "Source code editor with syntax highlighting and plugins",
      CAT_DEVELOPER, "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.8.3/npp.8.8.3.Installer.x64.exe", "npp.8.8.3.Installer.x64.exe",
      "Notepad++\\notepad++.exe", false, 6, "Uses Scintilla and common controls; untested", "N++", GDI_C(0x8C, 0xC0, 0x44) },
    { "GIMP", "The GIMP Team", "Image editor for photo retouching, composition and authoring",
      CAT_GRAPHICS, "https://download.gimp.org/gimp/v3.0/windows/gimp-3.0.4-setup.exe", "gimp-3.0.4-setup.exe",
      "GIMP 3\\bin\\gimp-3.0.exe", false, 260, "GTK4 application; untested", "G", GDI_C(0x5C, 0x4A, 0x36) },
    { "Inkscape", "Inkscape Project", "Vector graphics editor (SVG)",
      CAT_GRAPHICS, "https://media.inkscape.org/dl/resources/file/inkscape-1.4.2_2025-05-13_f4bb0b0-x64.exe", "inkscape-1.4.2-x64.exe",
      "Inkscape\\bin\\inkscape.exe", false, 120, "GTK application; untested", "Ik", GDI_C(0x2A, 0x2A, 0x2A) },
    { "Krita", "Krita Foundation", "Digital painting and illustration",
      CAT_GRAPHICS, "https://download.kde.org/stable/krita/5.2.9/krita-x64-5.2.9-setup.exe", "krita-x64-5.2.9-setup.exe",
      "Krita (x64)\\bin\\krita.exe", false, 130, "Qt application; untested", "Kr", GDI_C(0x9C, 0x3A, 0x8A) },
    { "Audacity", "Audacity Team", "Multi-track audio editor and recorder",
      CAT_MEDIA, "https://github.com/audacity/audacity/releases/download/Audacity-3.7.4/audacity-win-3.7.4-64bit.exe", "audacity-win-3.7.4-64bit.exe",
      "Audacity\\Audacity.exe", false, 15, "wxWidgets application; needs audio output", "Au", GDI_C(0x1C, 0x1C, 0x60) },
    { "HandBrake", "HandBrake Team", "Video transcoder: convert video to modern formats",
      CAT_MEDIA, "https://github.com/HandBrake/HandBrake/releases/download/1.9.2/HandBrake-1.9.2-x86_64-Win_GUI.exe", "HandBrake-1.9.2-x86_64-Win_GUI.exe",
      "HandBrake\\HandBrake.exe", false, 20, "Needs the .NET runtime, which NovaOS cannot run yet", "HB", GDI_C(0xB8, 0x3A, 0x2E) },
    { "OBS Studio", "OBS Project", "Live streaming and screen recording",
      CAT_MEDIA, "https://github.com/obsproject/obs-studio/releases/download/31.1.2/OBS-Studio-31.1.2-Windows-Installer.exe", "OBS-Studio-31.1.2-Windows-Installer.exe",
      "obs-studio\\bin\\64bit\\obs64.exe", false, 140, "Needs Direct3D; not expected to start yet", "OBS", GDI_C(0x30, 0x30, 0x30) },
    { "LibreOffice", "The Document Foundation", "Writer, Calc, Impress: a full office suite",
      CAT_OFFICE, "https://downloadarchive.documentfoundation.org/libreoffice/old/25.2.5.2/win/x86_64/LibreOffice_25.2.5.2_Win_x86-64.msi", "LibreOffice_25.2.5.2_Win_x86-64.msi",
      "LibreOffice\\program\\soffice.exe", false, 350, "MSI package: Windows Installer is not available on NovaOS yet", "Lo", GDI_C(0x18, 0xA3, 0x03) },
    { "SumatraPDF", "Krzysztof Kowalczyk", "Small, fast PDF, EPUB and comic book reader",
      CAT_OFFICE, "https://www.sumatrapdfreader.org/dl/rel/3.5.2/SumatraPDF-3.5.2-64.exe", "SumatraPDF-3.5.2-64.exe",
      NULL, true, 8, "Portable program: runs straight from Downloads", "Su", GDI_C(0xE8, 0xB0, 0x22) },
    { "KeePass", "Dominik Reichl", "Password manager with an encrypted database",
      CAT_UTILITIES, "https://sourceforge.net/projects/keepass/files/KeePass%202.x/2.58/KeePass-2.58-Setup.exe/download", "KeePass-2.58-Setup.exe",
      "KeePass Password Safe 2\\KeePass.exe", false, 4, "Needs the .NET Framework, which NovaOS cannot run yet", "Kp", GDI_C(0x2C, 0x68, 0xB0) },
    { "qBittorrent", "qBittorrent project", "BitTorrent client without ads",
      CAT_INTERNET, "https://sourceforge.net/projects/qbittorrent/files/qbittorrent-win32/qbittorrent-5.1.2/qbittorrent_5.1.2_x64_setup.exe/download", "qbittorrent_5.1.2_x64_setup.exe",
      "qBittorrent\\qbittorrent.exe", false, 40, "Qt application; untested", "qB", GDI_C(0x2E, 0x7A, 0xC8) },
    { "PuTTY", "Simon Tatham", "SSH and telnet client",
      CAT_INTERNET, "https://the.earth.li/~sgtatham/putty/latest/w64/putty.exe", "putty.exe",
      NULL, true, 2, "Portable program: runs straight from Downloads", "Pu", GDI_C(0x1F, 0x1F, 0x1F) },
    { "WinSCP", "Martin Prikryl", "SFTP, FTP and SCP file transfer client",
      CAT_INTERNET, "https://sourceforge.net/projects/winscp/files/WinSCP/6.5.3/WinSCP-6.5.3-Setup.exe/download", "WinSCP-6.5.3-Setup.exe",
      "WinSCP\\WinSCP.exe", false, 12, "Untested", "Ws", GDI_C(0x2B, 0x90, 0x3C) },
    { "Git for Windows", "Git for Windows", "The git version control system with Git Bash",
      CAT_DEVELOPER, "https://github.com/git-for-windows/git/releases/download/v2.51.0.windows.1/Git-2.51.0-64-bit.exe", "Git-2.51.0-64-bit.exe",
      "Git\\cmd\\git.exe", false, 70, "The installer needs cmd.exe features NovaOS lacks; untested", "git", GDI_C(0xF0, 0x50, 0x32) },
    { "Python", "Python Software Foundation", "The Python 3 programming language",
      CAT_DEVELOPER, "https://www.python.org/ftp/python/3.13.7/python-3.13.7-amd64.exe", "python-3.13.7-amd64.exe",
      "Python313\\python.exe", false, 27, "The installer uses Windows Installer packages; untested", "Py", GDI_C(0x36, 0x71, 0xA6) },
    { "WinMerge", "WinMerge Team", "Compare and merge files and folders",
      CAT_DEVELOPER, "https://github.com/WinMerge/winmerge/releases/download/v2.16.48/WinMerge-2.16.48-x64-Setup.exe", "WinMerge-2.16.48-x64-Setup.exe",
      "WinMerge\\WinMergeU.exe", false, 10, "Untested", "WM", GDI_C(0xE0, 0xB8, 0x30) },
    { "ShareX", "ShareX Team", "Screen capture and file sharing",
      CAT_UTILITIES, "https://github.com/ShareX/ShareX/releases/download/v17.1.0/ShareX-17.1.0-setup.exe", "ShareX-17.1.0-setup.exe",
      "ShareX\\ShareX.exe", false, 8, "Needs the .NET runtime, which NovaOS cannot run yet", "Sx", GDI_C(0x20, 0x90, 0x60) },
};
#define N_APPS ((int)(sizeof(g_catalog) / sizeof(g_catalog[0])))

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
enum { DL_NONE, DL_RESOLVE, DL_FETCH, DL_FAILED };

typedef struct {
    int     cat;                   /* selected category */
    int     scroll;                /* list offset, pixels */
    int     pressed;               /* catalog index whose button is held, or -1 */
    /* the one download in flight */
    int     dl;                    /* catalog index, or -1 */
    int     phase;
    NetOp  *op;
    UINT32  shown;                 /* bytes shown in the progress text */
    char    host[128], path[512];
    UINT16  port;
    bool    https;
    int     redirects;
    char    msg[N_APPS][64];       /* per-app status line ("Failed: ...") */
} Store;

#define SIDE_W   180
#define HEAD_H   56
#define ROW_H    84
#define BTN_W    92
#define BTN_H    30
#define TILE     48

static WND *g_store;               /* the one Store window */

static RamNode *installed_exe(const StoreApp *a)
{
    char path[RAMFS_PATH_MAX];
    if (!a->exe) return NULL;
    ksnprintf(path, sizeof(path), "\\Programs\\%s", a->exe);
    RamNode *n = RamfsResolve(NULL, path);
    return n && !n->dir ? n : NULL;
}

static RamNode *downloaded_file(const StoreApp *a)
{
    char path[RAMFS_PATH_MAX];
    ksnprintf(path, sizeof(path), "\\Downloads\\%s", a->file);
    RamNode *n = RamfsResolve(NULL, path);
    return n && !n->dir ? n : NULL;
}

static bool in_category(const StoreApp *a, int cat)
{
    if (cat == CAT_ALL) return true;
    if (cat == CAT_INSTALLED) return installed_exe(a) || (a->portable && downloaded_file(a));
    return a->category == cat;
}

/* The catalog indices shown for the current category; returns the count */
static int visible(const Store *s, int *out)
{
    int n = 0;
    for (int i = 0; i < N_APPS; i++)
        if (in_category(&g_catalog[i], s->cat)) out[n++] = i;
    return n;
}

static void set_msg(Store *s, int i, const char *m)
{
    strncpy(s->msg[i], m, sizeof(s->msg[i]) - 1);
    s->msg[i][sizeof(s->msg[i]) - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * Downloading
 * ----------------------------------------------------------------------- */
static void dl_fail(Store *s, const char *why)
{
    char m[64];
    ksnprintf(m, sizeof(m), "Failed: %s", why);
    set_msg(s, s->dl, m);
    if (s->op) NetRelease(s->op);
    s->op = NULL;
    s->phase = DL_NONE;
    s->dl = -1;
}

static bool dl_target(Store *s, const char *url)
{
    return NetParseUrl(url, s->host, sizeof(s->host), &s->port, s->path, sizeof(s->path), &s->https);
}

static void dl_resolve(Store *s)
{
    s->op = NetResolve(s->host);
    s->phase = DL_RESOLVE;
    if (!s->op) dl_fail(s, "the network is busy");
}

static void dl_start(Store *s, int i)
{
    if (s->dl >= 0) { set_msg(s, i, "Another download is in progress"); return; }
    if (!NetAvailable()) { set_msg(s, i, "Failed: no network connection"); return; }
    s->dl = i;
    s->redirects = 0;
    s->shown = 0;
    set_msg(s, i, "Connecting...");
    if (!dl_target(s, g_catalog[i].url)) { dl_fail(s, "bad download address"); return; }
    dl_resolve(s);
}

static void dl_cancel(Store *s)
{
    if (s->dl < 0) return;
    set_msg(s, s->dl, "");
    if (s->op) NetRelease(s->op);
    s->op = NULL;
    s->phase = DL_NONE;
    s->dl = -1;
}

static void dl_done(Store *s)
{
    const StoreApp *a = &g_catalog[s->dl];
    const char *body;
    UINT32 blen;
    char loc[512];
    int status = NetHttpParse(s->op, &body, &blen, loc, sizeof(loc));
    if (!status) { dl_fail(s, "unreadable response"); return; }

    if (status >= 300 && status < 400 && loc[0] && s->redirects < 8) {
        NetRelease(s->op);
        s->op = NULL;
        s->redirects++;
        if (loc[0] == '/') {                          /* same host */
            strncpy(s->path, loc, sizeof(s->path) - 1);
            s->path[sizeof(s->path) - 1] = '\0';
        } else if (!dl_target(s, loc)) {
            dl_fail(s, "bad redirect");
            return;
        }
        dl_resolve(s);
        return;
    }
    if (status != 200) {
        char m[48];
        ksnprintf(m, sizeof(m), "server replied %d", status);
        dl_fail(s, m);
        return;
    }
    RamNode *dir = RamfsResolve(NULL, "\\Downloads");
    if (!dir) dir = RamfsCreate(RamfsResolve(NULL, "\\"), "Downloads", true);
    RamNode *f = dir ? RamfsFind(dir, a->file) : NULL;
    if (!f && dir) f = RamfsCreate(dir, a->file, false);
    if (!f || f->dir || !RamfsWrite(f, body, blen)) { dl_fail(s, "could not save the file"); return; }
    char sz[24], m[64];
    AppFormatSize(blen, sz, sizeof(sz));
    ksnprintf(m, sizeof(m), "Downloaded %s to C:\\Downloads", sz);
    set_msg(s, s->dl, m);
    AppNoteRecentFile(f);
    NetRelease(s->op);
    s->op = NULL;
    s->phase = DL_NONE;
    s->dl = -1;
}

static bool store_tick(WND *w)
{
    Store *s = w->user;
    if (s->dl < 0 || !s->op) return false;
    NetOp *op = s->op;
    if (s->phase == DL_RESOLVE) {
        if (op->state == NET_PENDING) return false;
        if (op->state == NET_FAILED) { dl_fail(s, op->error[0] ? op->error : "name not found"); return true; }
        UINT32 ip = op->ip;
        NetRelease(op);
        s->op = NetHttpGet(ip, s->port, s->host, s->path, s->https);
        s->phase = DL_FETCH;
        if (!s->op) { dl_fail(s, "the network is busy"); return true; }
        set_msg(s, s->dl, "Downloading...");
        return true;
    }
    if (s->phase == DL_FETCH) {
        if (op->state == NET_PENDING) {
            /* progress: the bytes received so far, refreshed every 64 KB */
            if (op->len - s->shown < 64 * 1024) return false;
            s->shown = op->len;
            char sz[24], m[64];
            AppFormatSize(op->len, sz, sizeof(sz));
            ksnprintf(m, sizeof(m), "Downloading... %s", sz);
            set_msg(s, s->dl, m);
            return true;
        }
        if (op->state == NET_FAILED) dl_fail(s, op->error[0] ? op->error : "connection lost");
        else dl_done(s);
        return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Running
 * ----------------------------------------------------------------------- */
static void run_exe(Store *s, int i, RamNode *exe)
{
    char err[160];
    UmProcess *p = UmSpawn(exe, exe->name, exe->parent, NULL, err, sizeof(err));
    if (!p) {
        char m[64];
        ksnprintf(m, sizeof(m), "Could not start: %s", err);
        set_msg(s, i, m);
        return;
    }
    UmDetach(p);
    set_msg(s, i, "");
}

/* The row's button: what it says and what it does */
typedef enum { BTN_GET, BTN_CANCEL, BTN_INSTALL, BTN_RUN, BTN_OPEN } BtnKind;

static BtnKind row_button(const Store *s, int i)
{
    const StoreApp *a = &g_catalog[i];
    if (s->dl == i) return BTN_CANCEL;
    if (installed_exe(a)) return BTN_OPEN;
    if (downloaded_file(a)) return a->portable ? BTN_RUN : BTN_INSTALL;
    return BTN_GET;
}

static void press(Store *s, int i)
{
    const StoreApp *a = &g_catalog[i];
    switch (row_button(s, i)) {
    case BTN_GET:     dl_start(s, i); break;
    case BTN_CANCEL:  dl_cancel(s); break;
    case BTN_INSTALL:
    case BTN_RUN:     run_exe(s, i, downloaded_file(a)); break;
    case BTN_OPEN:    run_exe(s, i, installed_exe(a)); break;
    }
}

/* -----------------------------------------------------------------------
 * Layout and painting (client-relative rectangles)
 * ----------------------------------------------------------------------- */
static GdiRect r_list(GdiRect c) { return RECT(SIDE_W, HEAD_H, c.w - SIDE_W, c.h - HEAD_H); }
static GdiRect r_cat(int i)      { return RECT(8, 56 + i * 36, SIDE_W - 16, 32); }
static GdiRect r_btn(GdiRect c, int row_y) { return RECT(c.w - BTN_W - 20, row_y + (ROW_H - BTN_H) / 2, BTN_W, BTN_H); }

static int list_height(const Store *s)
{
    int idx[N_APPS];
    return visible(s, idx) * ROW_H + 12;
}

static void clamp_scroll(Store *s, GdiRect c)
{
    int max = list_height(s) - r_list(c).h;
    if (max < 0) max = 0;
    if (s->scroll > max) s->scroll = max;
    if (s->scroll < 0) s->scroll = 0;
}

static void app_tile(const StoreApp *a, int x, int y, int sz)
{
    GdiRoundGradV(RECT(x, y, sz, sz), sz * 22 / 100, GdiLerp(a->color, GDI_WHITE, 40), a->color);
    GdiTextCenter(x, y + (sz - GDI_FONT_H) / 2, sz, a->label, GDI_WHITE);
}

static void store_paint(WND *w)
{
    Store *s = w->user;
    GdiRect c = WmClientRect(w);
    clamp_scroll(s, c);

    /* Sidebar */
    GdiFillRect(RECT(c.x, c.y, SIDE_W, c.h), UI_PANEL);
    AppDrawIcon(APP_STORE, c.x + 14, c.y + 12, 28);
    GdiTextBold(c.x + 52, c.y + 18, "App Store", UI_TEXT);
    for (int i = 0; i < CAT_COUNT; i++) {
        GdiRect r = r_cat(i);
        r.x += c.x; r.y += c.y;
        if (i == s->cat) {
            GdiRoundRect(r, 4, UI_HOVER, GDI_TRANSPARENT);
            GdiFillRect(RECT(r.x, r.y + 8, 3, r.h - 16), UI_ACCENT);
        }
        GdiTextT(r.x + 14, r.y + (r.h - GDI_FONT_H) / 2, g_cat_name[i], i == s->cat ? UI_TEXT : UI_TEXT2);
    }
    GdiTextT(c.x + 16, c.y + c.h - 42, "Free and open source", UI_TEXT3);
    GdiTextT(c.x + 16, c.y + c.h - 24, "software for NovaOS", UI_TEXT3);

    /* Header */
    GdiTextLarge(c.x + SIDE_W + 24, c.y + 14, g_cat_name[s->cat], UI_TEXT);
    int idx[N_APPS];
    int n = visible(s, idx);
    char cnt[32];
    ksnprintf(cnt, sizeof(cnt), "%d app%s", n, n == 1 ? "" : "s");
    GdiTextT(c.x + c.w - 20 - GdiTextW(cnt), c.y + 22, cnt, UI_TEXT3);
    GdiFillRect(RECT(c.x + SIDE_W, c.y + HEAD_H - 1, c.w - SIDE_W, 1), UI_LINE);

    /* Rows */
    GdiRect lr = r_list(c);
    lr.x += c.x; lr.y += c.y;
    GdiSetClip(lr);
    if (!n) {
        GdiTextT(lr.x + 24, lr.y + 20, s->cat == CAT_INSTALLED ? "Nothing from the store is installed yet."
                                                                : "No apps here.", UI_TEXT2);
    }
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        const StoreApp *a = &g_catalog[i];
        int y = lr.y + 6 + k * ROW_H - s->scroll;
        if (y + ROW_H < lr.y || y > lr.y + lr.h) continue;
        int x = lr.x + 20;
        GdiRoundRect(RECT(x, y, lr.w - 40, ROW_H - 8), 6, UI_CARD, GDI_TRANSPARENT);
        app_tile(a, x + 14, y + (ROW_H - 8 - TILE) / 2, TILE);
        int tx = x + 14 + TILE + 16;
        GdiTextBold(tx, y + 10, a->name, UI_TEXT);
        int nw = GdiTextBoldW(a->name);
        GdiTextT(tx + nw + 10, y + 10, a->publisher, UI_TEXT3);
        /* the description, cut to the room before the button */
        GdiSetClip(RECT(lr.x, lr.y, c.x + c.w - BTN_W - 32 - lr.x, lr.h));
        GdiTextT(tx, y + 30, a->summary, UI_TEXT2);
        /* status: the download's progress or result, else size + note */
        char line[96];
        if (s->msg[i][0]) {
            ksnprintf(line, sizeof(line), "%s", s->msg[i]);
        } else if (installed_exe(a)) {
            ksnprintf(line, sizeof(line), "Installed  -  %s", a->note);
        } else if (a->size_mb) {
            ksnprintf(line, sizeof(line), "%u MB  -  %s", a->size_mb, a->note);
        } else {
            ksnprintf(line, sizeof(line), "%s", a->note);
        }
        bool failed = !strncmp(s->msg[i], "Failed", 6) || !strncmp(s->msg[i], "Could not", 9);
        GdiTextT(tx, y + 50, line, failed ? GDI_C(0xFF, 0x8A, 0x80) : UI_TEXT3);
        GdiSetClip(lr);
        static const char *labels[] = { "Get", "Cancel", "Install", "Run", "Open" };
        BtnKind b = row_button(s, i);
        GdiRect br = r_btn(c, y - c.y);
        br.x += c.x; br.y += c.y;
        if (s->pressed == i) { br.y += 1; }
        UiButton(br, labels[b], b == BTN_GET || b == BTN_INSTALL || b == BTN_OPEN);
    }
    /* scrollbar */
    int total = list_height(s);
    if (total > lr.h) {
        int th = lr.h * lr.h / total;
        if (th < 24) th = 24;
        int ty = lr.y + (lr.h - th) * s->scroll / (total - lr.h);
        GdiRoundRect(RECT(lr.x + lr.w - 8, ty, 4, th), 2, UI_TEXT3, GDI_TRANSPARENT);
    }
    GdiSetClip(c);
}

/* -----------------------------------------------------------------------
 * Input
 * ----------------------------------------------------------------------- */
/* The catalog index whose button is at client point (x, y), or -1 */
static int button_at(const Store *s, GdiRect c, int x, int y)
{
    GdiRect lr = r_list(c);
    if (!UiHit(lr, x, y)) return -1;
    int idx[N_APPS];
    int n = visible(s, idx);
    for (int k = 0; k < n; k++) {
        int ry = lr.y + 6 + k * ROW_H - s->scroll;
        if (UiHit(r_btn(c, ry), x, y)) return idx[k];
    }
    return -1;
}

static void store_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Store *s = w->user;
    GdiRect c = WmClientRect(w);
    switch (msg) {
    case WM_MOUSE_DOWN:
    case WM_MOUSE_DBLCLK:
        for (int i = 0; i < CAT_COUNT; i++)
            if (UiHit(r_cat(i), x, y)) { s->cat = i; s->scroll = 0; return; }
        s->pressed = button_at(s, c, x, y);
        break;
    case WM_MOUSE_UP: {
        int i = button_at(s, c, x, y);
        if (i >= 0 && i == s->pressed) press(s, i);
        s->pressed = -1;
        break; }
    case WM_MOUSE_WHEEL:
        s->scroll -= WmWheelDelta() * ROW_H;
        clamp_scroll(s, c);
        break;
    default:
        break;
    }
}

static void store_key(WND *w, const KeyEvent *k)
{
    Store *s = w->user;
    GdiRect c = WmClientRect(w);
    if (k->scancode == KEY_ESC) { WmDestroyWindow(w); return; }
    if (!k->extended) return;
    switch (k->scancode) {
    case KEY_UP:   s->scroll -= ROW_H; break;
    case KEY_DOWN: s->scroll += ROW_H; break;
    case KEY_PGUP: s->scroll -= r_list(c).h; break;
    case KEY_PGDN: s->scroll += r_list(c).h; break;
    case KEY_HOME: s->scroll = 0; break;
    case KEY_END:  s->scroll = list_height(s); break;
    }
    clamp_scroll(s, c);
}

static void store_close(WND *w)
{
    Store *s = w->user;
    if (s->op) NetRelease(s->op);
    kfree(s);
    w->user = NULL;
    if (g_store == w) g_store = NULL;
}

void StoreOpen(void)
{
    if (g_store) { WmSetActive(g_store); return; }
    Store *s = kzalloc(sizeof(Store));
    if (!s) return;
    s->dl = -1;
    s->pressed = -1;
    WND *w = AppCreateWindow(APP_STORE, "App Store", 820, 540, UI_BG);
    if (!w) { kfree(s); return; }
    w->user     = s;
    w->on_paint = store_paint;
    w->on_mouse = store_mouse;
    w->on_key   = store_key;
    w->on_close = store_close;
    w->on_tick  = store_tick;
    w->rbutton  = true;                     /* the wheel scrolls the list */
    g_store = w;
}
