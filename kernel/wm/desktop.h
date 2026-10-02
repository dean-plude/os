/*
 * desktop.h — NovaOS desktop shell (Explorer-style)
 *
 * Draws the Windows-11-style desktop on top of the window manager:
 *
 *   - a gradient wallpaper with "wave" layers, in a choice of themes
 *   - desktop icons (This PC, folders, NetSurf) with right-click menus
 *   - a floating dock: Start, search, pinned apps, a button for every
 *     other program window, tooltips and a clock
 *   - a Start menu with live search over apps, programs, settings and
 *     files, pinned apps, installed programs, recent items and power
 *   - an Alt+Tab switcher and Win-key shortcuts
 *
 * The shell registers itself with the WM as the background + overlay
 * layers, so WmComposite() produces the complete scene.
 */

#pragma once

#include "../include/types.h"
#include "../gdi/gdi.h"

/* Initialize the shell and register its layers with the window manager.
 * Must be called after WmInitialize() and GdiInitialize(). */
void DesktopInitialize(void);

/* Render exactly one composited frame (wallpaper + windows + shell). */
void DesktopRender(void);

/* Window-manager + shell event loop; runs as the 'desktop' kernel thread.
 * Polls PS/2 input, drives the cursor, and recomposites on change. */
void DesktopRun(void *arg);

/* Save drive C: and restart the PC */
void DesktopRestart(void);

/* Shut down, restart or sleep from any thread: the desktop loop saves
 * drive C: and does it (NtShutdownSystem, ExitWindowsEx, SetSuspendState).
 * POWER_SLEEP waits until the machine is awake again and returns whether
 * it slept; the others return at once. */
enum { POWER_NONE, POWER_SHUTDOWN, POWER_RESTART, POWER_SLEEP };
bool DesktopPowerRequest(int what);

/* Switch the screen to @w x @h (a mode the display driver lists) and lay
 * the desktop and its windows out again.  False: the mode is not
 * supported (nothing changed).  Takes the desktop lock. */
bool DesktopSetDisplayMode(int w, int h);
/* Make @w x @h the mode NovaOS returns to and boots in (saved in the
 * registry, where Windows keeps it: ...\Control\Video\...\DefaultSettings.*) */
void DesktopSaveDisplayMode(int w, int h);
/* At boot, before the desktop starts: switch to the saved mode if the
 * adapter has it */
void DesktopRestoreDisplayMode(void);

/* Toggle the Start menu open/closed. */
void DesktopToggleStart(void);

/* Returns true if the shell has a usable framebuffer to draw on. */
bool DesktopAvailable(void);

/* Wallpaper / colour themes (Settings > Personalization) */
int         DesktopThemeCount(void);
const char *DesktopThemeName(int i);
int         DesktopTheme(void);
void        DesktopSetTheme(int i);
/* A miniature of theme @i's wallpaper in @r */
void        DesktopDrawThemePreview(int i, GdiRect r);
