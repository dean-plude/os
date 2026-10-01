/*
 * display.h — display adapter driver: modes and page flipping
 *
 * Two back ends:
 *   - Bochs/QEMU VBE "DISPI" (QEMU -vga std, -device bochs-display,
 *     VirtualBox's VBoxVGA): any resolution the adapter's video memory
 *     holds, set at run time, and two pages of video memory flipped by
 *     the display start offset, so a frame is never seen half written.
 *   - The UEFI GOP framebuffer the bootloader handed over: the boot mode
 *     only, no flipping (every other adapter).
 *
 * The driver keeps the framebuffer console (framebuffer.h) pointed at the
 * page on screen; the GDI reads its geometry from there.
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

typedef struct {
    int w, h;
} DisplayMode;

#define DISPLAY_MAX_MODES 32

/* Probe the adapter (after PciInitialize).  Never changes the mode. */
void DisplayInit(const BootFramebuffer *boot);

/* "Bochs VBE", or "UEFI GOP framebuffer" */
const char *DisplayDriverName(void);

/* The modes the adapter can show, largest first; 32 bits per pixel. */
int  DisplayModeCount(void);
bool DisplayModeAt(int i, DisplayMode *out);
bool DisplayModeSupported(int w, int h);
DisplayMode DisplayCurrentMode(void);
DisplayMode DisplayBootMode(void);

/* The mode the user chose (Settings, or ChangeDisplaySettings with
 * CDS_UPDATEREGISTRY): what a temporary mode change returns to.  Starts
 * as the boot mode; kept until restart. */
DisplayMode DisplayDefaultMode(void);
void DisplaySetDefaultMode(int w, int h);

/* Switch the hardware to @w x @h (a supported mode).  The framebuffer
 * console follows; the caller re-initialises whatever draws on it. */
bool DisplaySetMode(int w, int h);

/* Page flipping: when DisplayCanFlip(), DisplayBackPage() is the video
 * memory page that is not on screen (same geometry as the visible one),
 * and DisplayFlip() puts it on screen. */
bool    DisplayCanFlip(void);
UINT32 *DisplayBackPage(void);
void    DisplayFlip(void);
