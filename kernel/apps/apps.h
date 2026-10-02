/*
 * apps.h — built-in desktop applications and the app registry
 *
 * Apps are window callbacks running on the desktop thread; NetSurf is a
 * Windows program in C:\Programs.  The registry also lists well-known
 * third-party apps (for search and the "all apps" list); those open a
 * short "not available yet" dialog.
 */

#pragma once

#include "../include/types.h"
#include "../gdi/gdi.h"
#include "../wm/wm.h"
#include "../fs/ramfs.h"
#include "../gdi/icon.h"

typedef enum {
    /* Built in */
    APP_TERMINAL, APP_EXPLORER, APP_NOTEPAD, APP_SETTINGS, APP_CALENDAR,
    /* A Windows program: C:\Programs\NetSurf\netsurf.exe */
    APP_NETSURF,
    /* Photos, and the App Store (downloads open-source Windows programs) */
    APP_STORE, APP_PHOTOS,
    /* The installer: puts NovaOS on a disk */
    APP_SETUP,
    /* Placeholders for apps NovaOS cannot run yet */ APP_XBOX, APP_SKYPE, APP_PHOTOSHOP,
    APP_ILLUSTRATOR, APP_CLIPCHAMP, APP_VSTUDIO, APP_PAINT, APP_TIPS,
    APP_POWERPOINT, APP_BLENDER, APP_BING, APP_SOLITAIRE, APP_TODO,
    APP_COUNT
} AppId;

typedef struct {
    const char *name;        /* display name */
    const char *label;       /* 1-2 letter tile label (placeholders) */
    GdiColor    color;       /* tile / accent colour */
    bool        builtin;
    bool        multi;       /* may have several windows */
} AppInfo;

const AppInfo *AppGetInfo(AppId id);

/* Open the app (a new window for multi-instance apps). */
void AppLaunch(AppId id);
/* Dock behaviour: launch if not running; otherwise focus it, or minimize
 * it if it is already the focused window. */
void AppActivate(AppId id);
/* Most recently launched apps, newest first; returns how many (<= max). */
int  AppRecent(AppId *out, int max);
/* Look an app up by (case-insensitive) command name, e.g. "notepad". */
bool AppByName(const char *name, AppId *out);
/* The app a Windows program's windows belong to (e.g. "netsurf.exe" ->
 * APP_NETSURF), or -1 for programs without a dock entry. */
int  AppForProgram(const char *exe_name);
/* Run a program found by UmFindProgram: GUI programs directly, console
 * programs in a new Terminal window.  @cmdline includes the program name. */
void AppRunProgram(RamNode *exe, const char *cmdline);
/* Install a .msi package with Windows Installer (msiexec /i); false if
 * msiexec.exe is missing */
bool AppRunMsi(RamNode *msi);

/* Recently opened documents and folders (newest first), for the Start menu */
void AppNoteRecentFile(RamNode *node);
int  AppRecentFiles(RamNode **out, int max);

/* Open specific content */
void AppOpenFolder(RamNode *dir);    /* File Explorer */
/* By type: pictures and icons in Photos, programs run, the rest Notepad */
void AppOpenFile(RamNode *file);
/* "Text Document", "Icon", "Application", ... (Explorer's Type column) */
const char *AppFileTypeName(const RamNode *f);

/* Set up the app layer (title-bar icons); called by the desktop shell */
void AppInit(void);

/* Draw the app's icon in a size x size box */
void AppDrawIcon(AppId id, int x, int y, int size);
/* A Windows program's icon: NAME.ico beside NAME.exe, else the icon in the
 * program's resources, else a generic program tile */
void AppDrawProgramIcon(const char *name, int x, int y, int size);
/* A window's icon: its app's, or its program's */
void AppDrawWindowIcon(const WND *w, int x, int y, int size);
/* A file's or folder's icon: .ico files show themselves, programs their
 * own icon, PNG pictures a thumbnail; others a generic icon */
void AppDrawNodeIcon(RamNode *f, int x, int y, int size);
void AppDrawFolderIcon(int x, int y, int size);
void AppDrawFileIcon(int x, int y, int size);
void AppDrawPcIcon(int x, int y, int size);         /* "This PC" */

/* Special folders (C:\Documents, ...) show their purpose on the folder */
typedef enum { FOLDER_PLAIN, FOLDER_DOCUMENTS, FOLDER_DOWNLOADS, FOLDER_PICTURES,
               FOLDER_PROJECTS } FolderKind;
FolderKind AppFolderKind(const RamNode *dir);
void AppDrawFolderKindIcon(FolderKind k, int x, int y, int size);

/* Single-colour line glyphs (sidebars, toolbars, the tray), in the same
 * line weight as the app icons */
typedef enum {
    GL_PC, GL_DOCUMENTS, GL_DOWNLOADS, GL_PICTURES, GL_PERSON, GL_CODE, GL_WINDOWS,
    GL_FOLDER, GL_FILE, GL_PLUS, GL_SEARCH, GL_NETWORK, GL_NETWORK_OFF, GL_CHEVRON,
    GL_BACK, GL_UP, GL_NOVA, GL_GEAR, GL_POWER,
} Glyph;
void AppDrawGlyph(Glyph g, int x, int y, int size, GdiColor c);

/* The decoded icon of an .ico/.cur/.png/.exe/.dll file (cached; NULL if it
 * has none), and of a program by name as for AppDrawProgramIcon */
GdiIcon *AppFileIcon(RamNode *f);
GdiIcon *AppProgramIcon(const char *name);

/* -----------------------------------------------------------------------
 * Shared look (Windows 11 dark) and helpers for app implementations
 * ----------------------------------------------------------------------- */
#define UI_BG        GDI_C(0x27, 0x27, 0x27)   /* window client */
#define UI_PANEL     GDI_C(0x20, 0x20, 0x20)   /* sidebars / toolbars */
#define UI_CARD      GDI_C(0x2F, 0x2F, 0x2F)
#define UI_HOVER     GDI_C(0x3A, 0x3A, 0x3A)
#define UI_SELECT    GDI_C(0x3D, 0x4A, 0x57)
#define UI_LINE      GDI_C(0x3A, 0x3A, 0x3A)
#define UI_TEXT      GDI_C(0xFF, 0xFF, 0xFF)
#define UI_TEXT2     GDI_C(0xC5, 0xC5, 0xC5)
#define UI_TEXT3     GDI_C(0x8A, 0x8A, 0x8A)
#define UI_ACCENT    GDI_C(0x4C, 0xC2, 0xFF)
#define UI_ACCENT_BG GDI_C(0x00, 0x5A, 0x9E)

bool UiHit(GdiRect r, int x, int y);
void UiButton(GdiRect r, const char *label, bool primary);

/* Create an app window at the next cascade position (client-size given). */
WND *AppCreateWindow(AppId id, const char *title, int client_w, int client_h,
                     GdiColor client_bg);

/* Format helpers */
void AppFormatSize(UINT64 bytes, char *buf, int cap);   /* "1.2 KB" */

/* System information shared by Terminal and Settings */
void AppCpuName(char *buf, int cap);                    /* CPUID brand string */
void AppUptime(char *buf, int cap);                     /* "1h 02m 05s" */

/* Implemented by the individual apps */
void TerminalOpen(void);
/* A new Terminal in @cwd (NULL: Documents) that runs @cmd as if typed */
void TerminalRun(const char *cmd, RamNode *cwd);
/* CREATE_NEW_CONSOLE (see terminal.c) */
struct UmConsole; struct UmProcess;
int  TerminalConsoleNew(const char *title, RamNode *cwd, struct UmConsole **con);
bool TerminalConsoleAdopt(int id, struct UmProcess *p);

/* Shortcuts (.lnk files) */
typedef struct {
    char target[RAMFS_PATH_MAX];    /* "C:\\Programs\\App\\app.exe" */
    char args[256];
    char workdir[RAMFS_PATH_MAX];
    char description[128];
} AppLink;
bool     AppLinkRead(const RamNode *lnk, AppLink *out);
RamNode *AppLinkTarget(const RamNode *lnk);          /* NULL: not a shortcut, or a missing target */
void ExplorerOpen(RamNode *dir);
void NotepadOpen(RamNode *file);
void SettingsOpen(void);
/* Settings pages (for SettingsOpenPage) */
enum { SETTINGS_SYSTEM, SETTINGS_DISPLAY, SETTINGS_PERSONALIZE, SETTINGS_STORAGE,
       SETTINGS_NETWORK, SETTINGS_ABOUT };
/* Open Settings (or focus the open window) at page @page */
void SettingsOpenPage(int page);
void CalendarOpen(void);
/* Photos: view a picture or icon (every image of an .ico); NULL shows the
 * pictures in C:\Pictures */
void PhotosOpen(RamNode *file);
void PlaceholderOpen(AppId id);
/* App Store: a catalog of open-source Windows programs to download and run */
void StoreOpen(void);
/* Get or install the App Store program called @name, as its button would;
 * returns what the Store says (the outcome is logged as "[STORE] ...") */
const char *StoreInstall(const char *name);
/* Install NovaOS on a disk */
void SetupOpen(void);
