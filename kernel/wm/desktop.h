/*
 * desktop.h — NovaOS desktop shell (Explorer-style)
 *
 * Phase 7.  Draws the full Windows-11-style desktop experience on top of
 * the window manager:
 *
 *   - Gradient wallpaper with warm "wave" layers
 *   - Left-column desktop icons (My PC, Documents, Personal, ...)
 *   - A floating, rounded taskbar / dock with app glyphs and a clock
 *   - A centered Start menu: search box, pinned-app grid, live tiles,
 *     a "Recently used" list and a user / power bar
 *
 * The shell registers itself with the WM as the background + overlay
 * layers, so WmComposite() produces the complete scene.
 */

#pragma once

#include "../include/types.h"

/* Initialize the shell and register its layers with the window manager.
 * Must be called after WmInitialize() and GdiInitialize(). */
void DesktopInitialize(void);

/* Render exactly one composited frame (wallpaper + windows + shell). */
void DesktopRender(void);

/* Window-manager + shell event loop; runs as the 'desktop' kernel thread.
 * Polls PS/2 input, drives the cursor, and recomposites on change. */
void DesktopRun(void *arg);


/* Toggle the Start menu open/closed (the shell renders it open by
 * default so the boot screen matches the design mock). */
void DesktopToggleStart(void);

/* Returns true if the shell has a usable framebuffer to draw on. */
bool DesktopAvailable(void);
