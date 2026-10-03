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
       CAT_DEVELOPER, CAT_RUNTIMES, CAT_INSTALLED, CAT_COUNT };

static const char *g_cat_name[CAT_COUNT] = {
    "All apps", "Utilities", "Internet", "Media", "Graphics", "Office", "Developer",
    "Runtimes", "Installed",
};

/* What the downloaded file is, and how it is installed */
enum {
    KIND_SETUP,        /* a 64-bit installer (.exe) or a Windows Installer package (.msi) */
    KIND_PORTABLE,     /* the 64-bit program itself: Run it from Downloads */
    KIND_ARCHIVE,      /* a .zip, .7z or 7-Zip self-extractor: 7-Zip unpacks it into C:\Programs\<dest> */
};

typedef struct {
    const char *name;
    const char *publisher;
    const char *summary;       /* one line */
    int         category;
    const char *url;           /* the download (the official 64-bit package where there is one) */
    const char *file;          /* saved as C:\Downloads\<file> */
    const char *dest;          /* KIND_ARCHIVE: unpacked into C:\Programs\<dest> */
    const char *exe;           /* installed: C:\Programs\<exe>, or an absolute path
                                * starting with '\'; a "**" component stands for
                                * any folders (archives with a versioned top folder) */
    int         kind;          /* KIND_* */
    unsigned    size_mb;       /* approximate download size */
    const char *note;          /* NovaOS compatibility note */
    const char *label;         /* 1-3 letter tile label */
    GdiColor    color;         /* tile colour */
    const char *system;        /* KIND_ARCHIVE: only these files (space-separated) are
                                * unpacked, and they go into the system folders:
                                * from an x86 or x32 folder to SysWOW64, others to
                                * System32; "path>name" renames.  Vulkan driver
                                * manifests (*_icd.*.json) among them are registered */
} StoreApp;

#define GH "https://github.com/"
static const StoreApp g_catalog[] = {
    { "7-Zip", "Igor Pavlov", "File archiver with a high compression ratio (7z, zip, tar, ...)",
      CAT_UTILITIES, "https://www.7-zip.org/a/7z2603-x64.exe", "7z2603-x64.exe", NULL,
      "7-Zip\\7zFM.exe", KIND_SETUP, 2, "Tested on NovaOS; the Store uses it to unpack downloads", "7z", GDI_C(0x25, 0x6E, 0xB8) },
    { "VLC media player", "VideoLAN", "Plays most video and audio files, discs and streams",
      CAT_MEDIA, "https://get.videolan.org/vlc/3.0.21/win64/vlc-3.0.21-win64.zip", "vlc-3.0.21-win64.zip", "VideoLAN",
      "VideoLAN\\**\\vlc.exe", KIND_ARCHIVE, 60, "64-bit zip; needs audio and video output NovaOS lacks", "VLC", GDI_C(0xF0, 0x7E, 0x1A) },
    { "Firefox", "Mozilla", "Fast, private web browser",
      CAT_INTERNET, "https://archive.mozilla.org/pub/firefox/releases/157.0/win64/en-US/Firefox%20Setup%20157.0.exe", "Firefox-Setup-x64.exe", "Mozilla Firefox",
      "Mozilla Firefox\\core\\firefox.exe", KIND_ARCHIVE, 80, "Unpacked from the full installer; loads HTTPS pages on NovaOS (nightly corpus)", "Fx", GDI_C(0xE6, 0x5A, 0x1C) },
    { "Thunderbird", "Mozilla", "Email, calendar and chat client",
      CAT_INTERNET, "https://download.mozilla.org/?product=thunderbird-latest-ssl&os=win64&lang=en-US", "Thunderbird-Setup-x64.exe", "Mozilla Thunderbird",
      "Mozilla Thunderbird\\core\\thunderbird.exe", KIND_ARCHIVE, 75, "Unpacked from the full installer; same runtime as Firefox", "Tb", GDI_C(0x1D, 0x66, 0xB4) },
    { "Notepad++", "Don Ho", "Source code editor with syntax highlighting and plugins",
      CAT_DEVELOPER, GH "notepad-plus-plus/notepad-plus-plus/releases/download/v8.8.3/npp.8.8.3.portable.x64.zip", "npp.8.8.3.portable.x64.zip", "Notepad++",
      "Notepad++\\**\\notepad++.exe", KIND_ARCHIVE, 7, "64-bit portable zip; opens and edits", "N++", GDI_C(0x8C, 0xC0, 0x44) },
    { "GIMP", "The GIMP Team", "Image editor for photo retouching, composition and authoring",
      CAT_GRAPHICS, "https://download.gimp.org/gimp/v3.0/windows/gimp-3.0.4-setup.exe", "gimp-3.0.4-setup.exe", NULL,
      NULL, KIND_SETUP, 260, "32-bit installer (Inno Setup) for 64-bit GIMP; untested", "G", GDI_C(0x5C, 0x4A, 0x36) },
    { "Inkscape", "Inkscape Project", "Vector graphics editor (SVG)",
      CAT_GRAPHICS, "https://inkscape.org/release/inkscape-1.4.2/windows/64-bit/compressed-7z/dl/", "inkscape-1.4.2-x64.7z", "Inkscape",
      "Inkscape\\**\\inkscape.exe", KIND_ARCHIVE, 110, "64-bit 7z archive; GTK application, untested", "Ik", GDI_C(0x2A, 0x2A, 0x2A) },
    { "Krita", "Krita Foundation", "Digital painting and illustration",
      CAT_GRAPHICS, "https://download.kde.org/stable/krita/5.2.9/krita-x64-5.2.9.zip", "krita-x64-5.2.9.zip", "Krita",
      "Krita\\**\\krita.exe", KIND_ARCHIVE, 190, "64-bit portable zip; Qt application, untested", "Kr", GDI_C(0x9C, 0x3A, 0x8A) },
    { "Audacity", "Audacity Team", "Multi-track audio editor and recorder",
      CAT_MEDIA, GH "audacity/audacity/releases/download/Audacity-3.7.4/audacity-win-3.7.4-64bit.zip", "audacity-win-3.7.4-64bit.zip", "Audacity",
      "Audacity\\**\\Audacity.exe", KIND_ARCHIVE, 20, "64-bit zip; needs audio output NovaOS lacks", "Au", GDI_C(0x1C, 0x1C, 0x60) },
    { "HandBrake", "HandBrake Team", "Video transcoder: convert video to modern formats",
      CAT_MEDIA, GH "HandBrake/HandBrake/releases/download/1.9.2/HandBrake-1.9.2-x86_64-Win_GUI.zip", "HandBrake-1.9.2-x86_64-Win_GUI.zip", "HandBrake",
      "HandBrake\\**\\HandBrake.exe", KIND_ARCHIVE, 25, "64-bit zip; needs the .NET runtime (see Runtimes)", "HB", GDI_C(0xB8, 0x3A, 0x2E) },
    { "OBS Studio", "OBS Project", "Live streaming and screen recording",
      CAT_MEDIA, GH "obsproject/obs-studio/releases/download/31.1.2/OBS-Studio-31.1.2-Windows-x64.zip", "OBS-Studio-31.1.2-Windows-x64.zip", "obs-studio",
      "obs-studio\\**\\obs64.exe", KIND_ARCHIVE, 150, "64-bit zip; needs Direct3D, which NovaOS lacks", "OBS", GDI_C(0x30, 0x30, 0x30) },
    { "LibreOffice", "The Document Foundation", "Writer, Calc, Impress: a full office suite",
      CAT_OFFICE, "https://downloadarchive.documentfoundation.org/libreoffice/old/25.2.5.2/win/x86_64/LibreOffice_25.2.5.2_Win_x86-64.msi", "LibreOffice_25.2.5.2_Win_x86-64.msi", NULL,
      "LibreOffice\\program\\soffice.exe", KIND_SETUP, 350, "64-bit Windows Installer package; untested", "Lo", GDI_C(0x18, 0xA3, 0x03) },
    { "SumatraPDF", "Krzysztof Kowalczyk", "Small, fast PDF, EPUB and comic book reader",
      CAT_OFFICE, "https://www.sumatrapdfreader.org/dl/rel/3.5.2/SumatraPDF-3.5.2-64.exe", "SumatraPDF-3.5.2-64.exe", NULL,
      NULL, KIND_PORTABLE, 8, "64-bit portable program: runs straight from Downloads; 3.4.6 opens and renders PDFs on NovaOS (nightly corpus)", "Su", GDI_C(0xE8, 0xB0, 0x22) },
    { "KeePassXC", "KeePassXC Team", "Password manager with an encrypted database",
      CAT_UTILITIES, GH "keepassxreboot/keepassxc/releases/download/2.7.10/KeePassXC-2.7.10-Win64.zip", "KeePassXC-2.7.10-Win64.zip", "KeePassXC",
      "KeePassXC\\**\\KeePassXC.exe", KIND_ARCHIVE, 40, "64-bit portable zip; Qt application, untested", "Kp", GDI_C(0x2C, 0x9A, 0x4C) },
    { "qBittorrent", "qBittorrent project", "BitTorrent client without ads",
      CAT_INTERNET, "https://sourceforge.net/projects/qbittorrent/files/qbittorrent-win32/qbittorrent-5.1.2/qbittorrent_5.1.2_x64_setup.exe/download", "qbittorrent_5.1.2_x64_setup.exe", NULL,
      NULL, KIND_SETUP, 40, "32-bit installer (NSIS); Qt application, untested", "qB", GDI_C(0x2E, 0x7A, 0xC8) },
    { "PuTTY", "Simon Tatham", "SSH and telnet client",
      CAT_INTERNET, "https://the.earth.li/~sgtatham/putty/latest/w64/putty.exe", "putty.exe", NULL,
      NULL, KIND_PORTABLE, 2, "64-bit portable program: runs straight from Downloads; raw connections work on NovaOS (nightly corpus), SSH untested", "Pu", GDI_C(0x1F, 0x1F, 0x1F) },
    { "WinSCP", "Martin Prikryl", "SFTP, FTP and SCP file transfer client",
      CAT_INTERNET, "https://sourceforge.net/projects/winscp/files/WinSCP/6.5.3/WinSCP-6.5.3-Setup.exe/download", "WinSCP-6.5.3-Setup.exe", NULL,
      NULL, KIND_SETUP, 12, "32-bit program and installer (Inno Setup); untested", "Ws", GDI_C(0x2B, 0x90, 0x3C) },
    { "Git", "Git for Windows", "The git version control system (MinGit, command line)",
      CAT_DEVELOPER, GH "git-for-windows/git/releases/download/v2.51.0.windows.1/MinGit-2.51.0-64-bit.zip", "MinGit-2.51.0-64-bit.zip", "Git",
      "Git\\cmd\\git.exe", KIND_ARCHIVE, 40, "64-bit zip; git, its bash and clone/fetch/push run on NovaOS", "git", GDI_C(0xF0, 0x50, 0x32) },
    { "Python", "Python Software Foundation", "The Python 3 programming language (embeddable)",
      CAT_DEVELOPER, "https://www.python.org/ftp/python/3.13.7/python-3.13.7-embed-amd64.zip", "python-3.13.7-embed-amd64.zip", "Python",
      "Python\\python.exe", KIND_ARCHIVE, 11, "64-bit zip; opens in a Terminal; Python runs on NovaOS", "Py", GDI_C(0x36, 0x71, 0xA6) },
    { "WinMerge", "WinMerge Team", "Compare and merge files and folders",
      CAT_DEVELOPER, GH "WinMerge/winmerge/releases/download/v2.16.50/winmerge-2.16.50-x64-exe.zip", "winmerge-2.16.50-x64-exe.zip", "WinMerge",
      "WinMerge\\**\\WinMergeU.exe", KIND_ARCHIVE, 12, "64-bit portable zip; compares files on NovaOS (nightly corpus)", "WM", GDI_C(0xE0, 0xB8, 0x30) },
    { "ShareX", "ShareX Team", "Screen capture and file sharing",
      CAT_UTILITIES, GH "ShareX/ShareX/releases/download/v17.1.0/ShareX-17.1.0-portable.zip", "ShareX-17.1.0-portable.zip", "ShareX",
      "ShareX\\**\\ShareX.exe", KIND_ARCHIVE, 10, "Portable zip; needs the .NET runtime (see Runtimes)", "Sx", GDI_C(0x20, 0x90, 0x60) },
    /* Runtimes */
    { ".NET Desktop Runtime 8", "Microsoft (MIT)", "Runs .NET programs such as HandBrake and ShareX (WinForms, WPF)",
      CAT_RUNTIMES, "https://aka.ms/dotnet/8.0/windowsdesktop-runtime-win-x64.zip", "windowsdesktop-runtime-8.0-win-x64.zip", "dotnet",
      "dotnet\\dotnet.exe", KIND_ARCHIVE, 60, "64-bit zip; .NET console programs run; desktop (WinForms/WPF) apps are untested", ".NET", GDI_C(0x51, 0x2B, 0xD4) },
    { "Visual C++ Redistributable", "Microsoft", "C++ runtime DLLs (msvcp140, vcruntime140, ...) many programs need",
      CAT_RUNTIMES, "https://aka.ms/vs/17/release/vc_redist.x64.exe", "vc_redist.x64.exe", NULL,
      NULL, KIND_SETUP, 25, "32-bit installer; NovaOS also has its own vcruntime140", "VC", GDI_C(0x68, 0x21, 0x7A) },
    { "OpenJDK 21", "Microsoft Build of OpenJDK", "Java runtime and development kit",
      CAT_RUNTIMES, "https://aka.ms/download-jdk/microsoft-jdk-21-windows-x64.zip", "microsoft-jdk-21-windows-x64.zip", "Java",
      "Java\\**\\bin\\java.exe", KIND_ARCHIVE, 190, "64-bit zip; Java (Temurin 21) runs on NovaOS; this build is untested", "Jv", GDI_C(0xE7, 0x6F, 0x00) },
    { "Mesa 3D", "Mesa / mesa-dist-win", "Software OpenGL and Vulkan for programs that need 3D without a GPU driver",
      CAT_RUNTIMES, GH "pal1000/mesa-dist-win/releases/download/24.2.4/mesa3d-24.2.4-release-msvc.7z", "mesa3d-24.2.4-release-msvc.7z", "Mesa3D",
      "\\Windows\\System32\\opengl32.dll", KIND_ARCHIVE, 90,
      "The system OpenGL 4.5 and Vulkan 1.3, drawn on the CPU (llvmpipe, lavapipe), for 64- and 32-bit programs",
      "GL", GDI_C(0x3B, 0x5B, 0xA0),
      "x64\\opengl32.dll x64\\libgallium_wgl.dll x64\\libglapi.dll x64\\vulkan_lvp.dll x64\\lvp_icd.x86_64.json "
      "x86\\opengl32.dll x86\\libgallium_wgl.dll x86\\libglapi.dll x86\\vulkan_lvp.dll x86\\lvp_icd.x86.json" },
    { "DXVK", "Philip Rebohle / DXVK", "Direct3D 8, 9, 10 and 11 on Vulkan, for games and 3D programs",
      CAT_RUNTIMES, GH "doitsujin/dxvk/releases/download/v2.5.3/dxvk-2.5.3.tar.gz", "dxvk-2.5.3.tar.gz", "DXVK",
      "\\Windows\\System32\\d3d11_dxvk.dll", KIND_ARCHIVE, 10,
      "The system Direct3D 8-11 for 64- and 32-bit programs, drawn on the CPU through Mesa's Vulkan: get Mesa 3D first",
      "DX", GDI_C(0x10, 0x7C, 0x10),
      "dxvk-2.5.3\\x64\\d3d8.dll dxvk-2.5.3\\x64\\d3d9.dll dxvk-2.5.3\\x64\\d3d10core.dll dxvk-2.5.3\\x64\\d3d11.dll>d3d11_dxvk.dll "
      "dxvk-2.5.3\\x64\\dxgi.dll>dxgi_dxvk.dll "
      "dxvk-2.5.3\\x32\\d3d8.dll dxvk-2.5.3\\x32\\d3d9.dll dxvk-2.5.3\\x32\\d3d10core.dll dxvk-2.5.3\\x32\\d3d11.dll>d3d11_dxvk.dll "
      "dxvk-2.5.3\\x32\\dxgi.dll>dxgi_dxvk.dll" },
    /* Built by tools/build_venus.py; the CI publishes it beside nova.iso */
    { "Venus", "Mesa / NovaOS", "Vulkan on the host's GPU when NovaOS runs in QEMU with a 3D virtio-gpu",
      CAT_RUNTIMES, GH "dean-plude/os/releases/download/latest/venus.7z", "venus.7z", "Venus",
      "\\Windows\\System32\\vulkan_virtio.dll", KIND_ARCHIVE, 3,
      "Mesa's Venus for 64- and 32-bit programs: Vulkan, and Direct3D through DXVK, run on the host's GPU "
      "(QEMU: -device virtio-vga-gl,venus=on,blob=on,hostmem=1G); without that GPU programs keep using Mesa 3D",
      "VN", GDI_C(0xC0, 0x30, 0x40),
      "x64\\vulkan_virtio.dll x64\\virtio_icd.x86_64.json x86\\vulkan_virtio.dll x86\\virtio_icd.x86.json" },
};
#undef GH
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
    char    msg[N_APPS][72];       /* per-app status line */
    bool    bad[N_APPS];           /* the status line reports a failure */
    /* 7-Zip unpacking an archive */
    UmProcess *unpack;
    int     unpack_i;              /* catalog index, or -1 */
    char    tar[RAMFS_PATH_MAX];   /* a .tar.gz's .tar: unpacked next, then deleted */
    bool    tar_layer;             /* 7-Zip is taking the .gz layer off */
} Store;

#define SIDE_W   180
#define HEAD_H   56
#define ROW_H    84
#define BTN_W    92
#define BTN_H    30
#define TILE     48

static WND *g_store;               /* the one Store window */

/* Walk the '\'-separated @path from @n; a component ending in '*' matches
 * the first folder with that prefix, and a "**" component any folders */
static RamNode *walk(RamNode *n, const char *p, int depth)
{
    while (*p == '\\') p++;
    if (!n) return NULL;
    if (!*p) return n;
    char comp[RAMFS_NAME_MAX];
    int len = 0;
    while (p[len] && p[len] != '\\' && len < (int)sizeof(comp) - 1) { comp[len] = p[len]; len++; }
    comp[len] = '\0';
    const char *rest = p + len;
    if (!strcmp(comp, "**")) {
        RamNode *r = walk(n, rest, depth);                /* no folder at all */
        for (RamNode *c = n->child; !r && c && depth < 6; c = c->next)
            if (c->dir) r = walk(c, p, depth + 1);        /* or one more, then again */
        return r;
    }
    if (len && comp[len - 1] == '*') {
        comp[--len] = '\0';
        for (RamNode *c = n->child; c; c = c->next)
            if (c->dir && !strncmp(c->name, comp, (size_t)len)) {
                RamNode *r = walk(c, rest, depth);
                if (r) return r;
            }
        return NULL;
    }
    return walk(RamfsFind(n, comp), rest, depth);
}

/* The installed program's file (see StoreApp.exe) */
static RamNode *installed_exe(const StoreApp *a)
{
    if (!a->exe) return NULL;
    RamNode *n = walk(a->exe[0] == '\\' ? RamfsRoot() : RamfsResolve(NULL, "\\Programs"), a->exe, 0);
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
    if (cat == CAT_INSTALLED) return installed_exe(a) || (a->kind == KIND_PORTABLE && downloaded_file(a));
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
    s->bad[i] = !strncmp(m, "Failed", 6) || !strncmp(m, "Could not", 9);
    if (s->bad[i] || !strncmp(m, "Installed", 9))       /* the outcome, for the serial log */
        kprintf("[STORE] %s: %s\n", g_catalog[i].name, m);
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
    if ((UINT64)g_catalog[i].size_mb * 1024 * 1024 > NET_HTTP_MAX) {
        char m[64];
        ksnprintf(m, sizeof(m), "Too large for this build (limit %u MB)", (unsigned)(NET_HTTP_MAX >> 20));
        set_msg(s, i, m);
        return;
    }
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

static bool unpack_tick(Store *s);

static bool store_tick(WND *w)
{
    Store *s = w->user;
    bool changed = unpack_tick(s);
    if (s->dl < 0 || !s->op) return changed;
    NetOp *op = s->op;
    if (s->phase == DL_RESOLVE) {
        if (op->state == NET_PENDING) return false;
        if (op->state == NET_FAILED) { dl_fail(s, op->error[0] ? op->error : "name not found"); return true; }
        NetIp ip = op->addr;
        NetRelease(op);
        s->op = NetHttpGetAddr(&ip, s->port, s->host, s->path, s->https);
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
static void failed_msg(Store *s, int i, const char *m)
{
    set_msg(s, i, m);
    s->bad[i] = true;
}

/* The machine a PE file is built for (0x8664 x64, 0x14C x86), 0 if not a PE */
static UINT16 pe_machine(const RamNode *f)
{
    const UINT8 *d = (const UINT8 *)f->data;
    if (!d || f->size < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
    UINT32 pe = (UINT32)(d[0x3C] | d[0x3D] << 8 | d[0x3E] << 16 | (UINT32)d[0x3F] << 24);
    if (pe > f->size - 6 || memcmp(d + pe, "PE\0\0", 4)) return 0;
    return (UINT16)(d[pe + 4] | d[pe + 5] << 8);
}

static void spawn(Store *s, int i, RamNode *exe, const char *cmdline, RamNode *cwd)
{
    char err[160];
    UmProcess *p = UmSpawn(exe, cmdline, cwd, NULL, err, sizeof(err));
    if (!p) {
        char m[96];
        ksnprintf(m, sizeof(m), "Could not start: %s", err);
        failed_msg(s, i, m);
        return;
    }
    UmDetach(p);
    set_msg(s, i, "");
}

static bool ends_with(const char *name, const char *ext)
{
    int n = (int)strlen(name), e = (int)strlen(ext);
    if (n < e) return false;
    for (int i = 0; i < e; i++)
        if ((name[n - e + i] | 0x20) != (ext[i] | 0x20)) return false;
    return true;
}

/* Unpack an archive (or a 7-Zip self-extracting installer) into
 * C:\Programs\<dest> with 7-Zip's command-line program */
static void unpack(Store *s, int i, RamNode *f)
{
    const StoreApp *a = &g_catalog[i];
    if (s->unpack) { set_msg(s, i, "Another download is being unpacked"); return; }
    RamNode *z = RamfsResolve(NULL, "\\Programs\\7-Zip\\7z.exe");
    if (!z) { failed_msg(s, i, "Could not unpack: get 7-Zip first (Utilities)"); return; }
    char path[RAMFS_PATH_MAX], cmd[2 * RAMFS_PATH_MAX + 640], err[160];
    RamfsPath(f, path, sizeof(path));
    s->tar_layer = ends_with(f->name, ".tar.gz") || ends_with(f->name, ".tgz");
    if (s->tar_layer) {
        /* 7-Zip takes one layer at a time: the .tar beside the download first */
        char dir[RAMFS_PATH_MAX];
        RamfsPath(f->parent, dir, sizeof(dir));
        ksnprintf(cmd, sizeof(cmd), "7z x \"%s\" \"-o%s\" -y", path, dir);
        ksnprintf(s->tar, sizeof(s->tar), "%s\\%s", dir + 2, f->name);
        int n = (int)strlen(s->tar);                        /* x.tar.gz -> x.tar, x.tgz -> x.tar */
        if (ends_with(s->tar, ".tgz")) strcpy(s->tar + n - 2, "ar");
        else s->tar[n - 3] = '\0';
    } else {
        int n = ksnprintf(cmd, sizeof(cmd), "7z x \"%s\" \"-oC:\\Programs\\%s\" -y", path, a->dest);
        for (const char *c = a->system; c && *c && n < (int)sizeof(cmd) - 2; ) {   /* the files, less any ">name" */
            if (*c == '>') { while (*c && *c != ' ') c++; continue; }
            if (c == a->system) cmd[n++] = ' ';
            cmd[n++] = *c++;
        }
        cmd[n] = '\0';
    }
    UmProcess *p = UmSpawn(z, cmd, f->parent, NULL, err, sizeof(err));
    if (!p) {
        char m[96];
        ksnprintf(m, sizeof(m), "Could not start 7-Zip: %s", err);
        failed_msg(s, i, m);
        return;
    }
    s->unpack = p;
    s->unpack_i = i;
    set_msg(s, i, "Unpacking with 7-Zip...");
}

/* An archive of system files unpacked: move them into System32 (x64\...)
 * and SysWOW64 (x86\...), replacing older copies, as an installer would */
static void move_system_files(const StoreApp *a)
{
    char list[512];
    strncpy(list, a->system, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';
    for (char *f = list, *next; f && *f; f = next) {
        next = strchr(f, ' ');
        if (next) *next++ = '\0';
        char *as = strchr(f, '>');                          /* "path>name": installed as name */
        if (as) *as++ = '\0';
        char path[RAMFS_PATH_MAX];
        ksnprintf(path, sizeof(path), "\\Programs\\%s\\%s", a->dest, f);
        RamNode *n = RamfsResolve(NULL, path);
        const char *leaf = strrchr(f, '\\');
        bool x86 = !strncmp(f, "x86\\", 4) || !strncmp(f, "x32\\", 4) || strstr(f, "\\x86\\") || strstr(f, "\\x32\\");
        const char *sys = x86 ? "\\Windows\\SysWOW64" : "\\Windows\\System32";
        RamNode *dir = RamfsResolve(NULL, sys);
        if (!n || n->dir || !leaf || !dir) continue;
        const char *name = as ? as : leaf + 1;
        if (!RamfsRename(n, dir, name, true)) {
            kprintf("[STORE] Could not move %s into the system folder\n", path);
            continue;
        }
        if (strstr(name, "_icd.") && ends_with(name, ".json")) {   /* a Vulkan driver, as its installer registers it */
            char reg[RAMFS_PATH_MAX];
            ksnprintf(reg, sizeof(reg), "C:%s\\%s", sys, name);
            um_registry_set_dword("Machine\\SOFTWARE\\Khronos\\Vulkan\\Drivers", reg, 0);
        }
    }
    RamNode *top = RamfsResolve(NULL, "\\Programs");
    RamNode *d = top ? RamfsFind(top, a->dest) : NULL;     /* the emptied folders */
    for (RamNode *c = d ? d->child : NULL, *nx; c; c = nx) {
        nx = c->next;
        for (RamNode *g = c->dir ? c->child : NULL, *gn; g; g = gn) {
            gn = g->next;
            if (g->dir && !g->child) RamfsDelete(g);
        }
        if (c->dir && !c->child) RamfsDelete(c);
    }
    if (d && !d->child) RamfsDelete(d);
}

/* A running unpack finished: did it produce the program? */
static bool unpack_tick(Store *s)
{
    if (!s->unpack) return false;
    UINT32 status = 0;
    char why[96] = "";
    if (!UmHasExited(s->unpack, &status, why, sizeof(why))) return false;
    UmRelease(s->unpack);
    s->unpack = NULL;
    int i = s->unpack_i;
    s->unpack_i = -1;
    const StoreApp *a = &g_catalog[i];
    if (s->tar_layer) {                                     /* the .gz layer is off: now the .tar */
        RamNode *t = status == 0 ? RamfsResolve(NULL, s->tar) : NULL;
        if (t && !t->dir) {
            unpack(s, i, t);
            if (s->unpack) return true;
            RamfsDelete(t);
            s->tar[0] = '\0';
            return true;                                    /* unpack() said why */
        }
    } else if (s->tar[0]) {
        RamNode *t = RamfsResolve(NULL, s->tar);
        if (t && !t->dir) RamfsDelete(t);
    }
    s->tar_layer = false;
    s->tar[0] = '\0';
    if (a->system && status == 0) move_system_files(a);
    if (installed_exe(a) || (!a->exe && status == 0)) {
        char m[96];
        if (a->system) ksnprintf(m, sizeof(m), "Installed in C:\\Windows\\System32");
        else           ksnprintf(m, sizeof(m), "Installed in C:\\Programs\\%s", a->dest);
        set_msg(s, i, m);
    } else {
        char m[96];
        if (why[0]) ksnprintf(m, sizeof(m), "Could not unpack: 7-Zip %s", why);
        else        ksnprintf(m, sizeof(m), "Could not unpack: 7-Zip stopped with code %u", (unsigned)status);
        failed_msg(s, i, m);
    }
    return true;
}

/* Install a downloaded file: 64-bit installers run, .msi packages go to
 * Windows Installer, archives are unpacked */
static void run_file(Store *s, int i, RamNode *f)
{
    if (!f) return;
    const StoreApp *a = &g_catalog[i];
    if (a->kind == KIND_ARCHIVE) { unpack(s, i, f); return; }
    if (ends_with(f->name, ".msi")) {
        if (!AppRunMsi(f)) failed_msg(s, i, "Could not start: Windows Installer is missing");
        else set_msg(s, i, "");
        return;
    }
    UINT16 m = pe_machine(f);                   /* 64-bit, or 32-bit (WoW64) */
    if (m != 0x8664 && m != 0x14C) { failed_msg(s, i, "Could not install: this download is not a Windows program"); return; }
    spawn(s, i, f, f->name, f->parent);
}

/* The row's button: what it says and what it does */
typedef enum { BTN_GET, BTN_CANCEL, BTN_INSTALL, BTN_RUN, BTN_OPEN, BTN_NONE } BtnKind;

static bool is_runtime(const StoreApp *a) { return a->category == CAT_RUNTIMES; }

static BtnKind row_button(const Store *s, int i)
{
    const StoreApp *a = &g_catalog[i];
    if (s->unpack_i == i) return BTN_NONE;
    if (s->dl == i) return BTN_CANCEL;
    if (installed_exe(a)) return is_runtime(a) ? BTN_NONE : BTN_OPEN;
    if (downloaded_file(a)) return a->kind == KIND_PORTABLE ? BTN_RUN : BTN_INSTALL;
    return BTN_GET;
}

/* What stands in the button's place when there is none */
static const char *no_button_text(const Store *s, int i)
{
    if (s->unpack_i == i) return "Unpacking";
    return "Installed";
}

static void press(Store *s, int i)
{
    const StoreApp *a = &g_catalog[i];
    switch (row_button(s, i)) {
    case BTN_GET:     dl_start(s, i); break;
    case BTN_CANCEL:  dl_cancel(s); break;
    case BTN_INSTALL: run_file(s, i, downloaded_file(a)); break;
    case BTN_RUN: {
        RamNode *f = downloaded_file(a);
        UINT16 m = f ? pe_machine(f) : 0;
        if (m == 0x8664 || m == 0x14C) { AppRunProgram(f, f->name); set_msg(s, i, ""); }
        else if (f) failed_msg(s, i, "Could not run: this download is not a Windows program");
        break; }
    case BTN_OPEN: {                         /* console programs get a Terminal */
        RamNode *exe = installed_exe(a);
        if (exe) { AppRunProgram(exe, exe->name); set_msg(s, i, ""); }
        break; }
    case BTN_NONE:    break;
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
        bool failed = s->msg[i][0] && s->bad[i];
        GdiTextT(tx, y + 50, line, failed ? GDI_C(0xFF, 0x8A, 0x80) : UI_TEXT3);
        GdiSetClip(lr);
        static const char *labels[] = { "Get", "Cancel", "Install", "Run", "Open", "" };
        BtnKind b = row_button(s, i);
        GdiRect br = r_btn(c, y - c.y);
        br.x += c.x; br.y += c.y;
        if (s->pressed == i) { br.y += 1; }
        if (b == BTN_NONE) GdiTextCenter(br.x, br.y + (br.h - GDI_FONT_H) / 2, br.w, no_button_text(s, i), UI_TEXT3);
        else UiButton(br, labels[b], b == BTN_GET || b == BTN_INSTALL || b == BTN_OPEN);
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
        if (UiHit(r_btn(c, ry), x, y)) return row_button(s, idx[k]) == BTN_NONE ? -1 : idx[k];
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
    if (s->unpack) UmDetach(s->unpack);          /* let 7-Zip finish on its own */
    kfree(s);
    w->user = NULL;
    if (g_store == w) g_store = NULL;
}

/* "store install NAME" in the Terminal: press the row's button as a click
 * would (Get, or Install once the file is in C:\Downloads).  The Store
 * window opens in the background; the outcome lands in the serial log as
 * "[STORE] NAME: Installed ..." or "... Failed/Could not ...". */
const char *StoreInstall(const char *name)
{
    int i = 0;
    for (; i < N_APPS; i++) {
        const char *a = g_catalog[i].name, *b = name;
        while (*a && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) break;
    }
    if (i == N_APPS) return "There is no such program in the App Store.";
    WND *was = WmActiveWindow();
    StoreOpen();
    if (!g_store) return "Could not open the App Store.";
    if (was && was != g_store) WmSetActive(was);
    Store *s = g_store->user;
    if (installed_exe(&g_catalog[i])) {
        kprintf("[STORE] %s: Installed already\n", g_catalog[i].name);
        return "Installed already.";
    }
    press(s, i);
    return s->msg[i][0] ? s->msg[i] : "Started.";
}

/* "store close" in the Terminal: close the App Store's window (one
 * "store install" opened in the background), as its close button would */
const char *StoreClose(void)
{
    if (!g_store) return "The App Store is not open.";
    WmRequestClose(g_store);
    return "Closed the App Store.";
}

void StoreOpen(void)
{
    if (g_store) { WmSetActive(g_store); return; }
    Store *s = kzalloc(sizeof(Store));
    if (!s) return;
    s->dl = -1;
    s->pressed = -1;
    s->unpack_i = -1;
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
