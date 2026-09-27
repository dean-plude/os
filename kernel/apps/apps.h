/*
 * apps.h — built-in desktop applications and the app registry
 *
 * Apps are window callbacks running on the desktop thread (NovaOS cannot
 * run user-mode GUI programs yet).  The registry also lists the "pinned"
 * third-party apps shown in the dock and Start menu; those open a short
 * "not available yet" dialog.
 */

#pragma once

#include "../include/types.h"
#include "../gdi/gdi.h"
#include "../wm/wm.h"
#include "../fs/ramfs.h"

typedef enum {
    /* Built in */
    APP_TERMINAL, APP_EXPLORER, APP_NOTEPAD, APP_SETTINGS, APP_CALENDAR,
    /* Placeholders for apps NovaOS cannot run yet */
    APP_EDGE, APP_STORE, APP_PHOTOS, APP_XBOX, APP_SKYPE, APP_PHOTOSHOP,
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

/* Open specific content */
void AppOpenFolder(RamNode *dir);    /* File Explorer */
void AppOpenFile(RamNode *file);     /* Notepad */

/* Draw the app's icon in a size x size box */
void AppDrawIcon(AppId id, int x, int y, int size);

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
void ExplorerOpen(RamNode *dir);
void NotepadOpen(RamNode *file);
void SettingsOpen(void);
void CalendarOpen(void);
void PlaceholderOpen(AppId id);
