/*
 * display.h — display adapter driver: modes and page flipping
 *
 * Three back ends:
 *   - Bochs/QEMU VBE "DISPI" (QEMU -vga std, qxl, virtio and vmware,
 *     -device bochs-display, VirtualBox's VBoxVGA): any resolution the
 *     adapter's video memory holds, set at run time, and two pages of
 *     video memory flipped by the display start offset, so a frame is
 *     never seen half written.
 *   - Cirrus Logic GD5446 (QEMU -vga cirrus): 800x600 and 640x480.
 *   - The UEFI GOP framebuffer the bootloader handed over: the boot mode
 *     only, no flipping (every other adapter).
 *
 * The driver keeps the framebuffer console (framebuffer.h) pointed at the
 * page on screen; the GDI reads its geometry from there.
 *
 * More monitors: each further Bochs/QEMU DISPI adapter that is not the boot
 * display (QEMU -device secondary-vga or bochs-display) is another head,
 * with its own modes, set through the same registers (BAR2 MMIO).  Head 0
 * is the boot display (the functions without "Head" in the name); the GDI
 * composes one desktop across all of them (gdi.h, GdiMonitor*).
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

typedef struct {
    int w, h;
} DisplayMode;

#define DISPLAY_MAX_MODES 32
#define DISPLAY_MAX_HEADS 4

/* Probe the adapter (after PciInitialize).  Never changes the mode. */
void DisplayInit(const BootFramebuffer *boot);

/* "Bochs VBE", "Cirrus Logic" or "UEFI GOP framebuffer" */
const char *DisplayDriverName(void);

/* The adapter the driver found ("QEMU QXL", "VMware SVGA II", ...);
 * NULL on the GOP framebuffer */
const char *DisplayAdapterName(void);

/* The modes the adapter can show, largest first; 32 bits per pixel. */
int  DisplayModeCount(void);
bool DisplayModeAt(int i, DisplayMode *out);
bool DisplayModeSupported(int w, int h);
DisplayMode DisplayCurrentMode(void);
DisplayMode DisplayBootMode(void);

/* The mode the user chose (Settings, or ChangeDisplaySettings with
 * CDS_UPDATEREGISTRY): what a temporary mode change returns to.  Starts
 * as the boot mode; DesktopSaveDisplayMode also keeps it in the registry
 * for the next boot. */
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

/* After S3: the adapter lost its mode while powered off; set the current
 * one again (video memory is not cleared; the caller redraws anyway, as
 * real hardware may not keep it).  Nothing on the GOP framebuffer, which
 * shows whatever the firmware's wake path set up. */
void DisplayResume(void);

/* -----------------------------------------------------------------------
 * Heads (monitors).  Head 0 is the boot display above; heads 1.. are the
 * further DISPI adapters, single-buffered (no page flipping).
 * ----------------------------------------------------------------------- */
int  DisplayHeadCount(void);              /* 1 + further heads; 0 without a display */
/* The adapter's name ("QEMU secondary-vga", ...; head 0: DisplayAdapterName
 * or the driver's name) */
const char *DisplayHeadName(int head);
int  DisplayHeadModeCount(int head);
bool DisplayHeadModeAt(int head, int i, DisplayMode *out);
bool DisplayHeadModeSupported(int head, int w, int h);
DisplayMode DisplayHeadMode(int head);
DisplayMode DisplayHeadDefaultMode(int head);
void DisplayHeadSetDefaultMode(int head, int w, int h);
bool DisplayHeadSetMode(int head, int w, int h);
/* Heads 1..: the visible video memory and its pixels per scanline (the
 * pixels are little-endian XRGB, as on head 0) */
UINT32 *DisplayHeadSurface(int head, int *stride);
