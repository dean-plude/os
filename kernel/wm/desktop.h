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
/* At boot, before the desktop starts: switch every display to its saved
 * mode if the adapter has it, and place the other monitors where they
 * were arranged */
void DesktopRestoreDisplayMode(void);

/* The same for display @head (hal/display.h; 0 is the primary) */
bool DesktopSetHeadMode(int head, int w, int h);
void DesktopSaveHeadMode(int head, int w, int h);
/* Arrange: put monitor @i (>= 1) with its top left at (x, y) on the
 * virtual desktop (logical px; gdi.h GdiSetMonitorOrigin) and lay
 * everything out again; @save keeps it in the registry
 * (...\Video\{NovaOS-Display}\000N, Attach.RelativeX/Y).  False if it
 * had to go elsewhere (it would overlap, or touch no other monitor). */
bool DesktopSetMonitorOrigin(int i, int x, int y, bool save);
/* The DPI DPI-aware programs see on display @head: 96 or 192 (gdi.h
 * GdiSetMonitorDpi); programs get WM_NOVA_DPI, and user32 sends
 * WM_DPICHANGED to the windows whose DPI changed.  @save keeps it in the
 * registry (...\Video\{NovaOS-Display}\000N, LogPixels) for the next boot. */
void DesktopSetMonitorDpi(int head, int dpi, bool save);

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
