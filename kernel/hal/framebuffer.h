/*
 * framebuffer.h — GOP linear framebuffer console
 *
 * Phase 1 provides a simple text console rendered directly into the
 * UEFI GOP framebuffer.  We use an embedded 8×16 bitmap font (PC BIOS
 * VGA font) to render ASCII characters.
 *
 * Capabilities:
 *   - Print text at arbitrary screen positions
 *   - Newline/scroll (full-screen vertical scroll by copying rows)
 *   - Foreground + background color
 *   - No cursor blink (Phase 5 will add a proper window manager)
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

/* 32-bit packed BGRX color (matches GOP PixelBlueGreenRedReserved format) */
typedef uint32_t FbColor;

#define FB_COLOR(r, g, b)  ((FbColor)(((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (r)))

/* Standard terminal colors */
#define FB_BLACK    FB_COLOR(0x00, 0x00, 0x00)
#define FB_WHITE    FB_COLOR(0xFF, 0xFF, 0xFF)
#define FB_RED      FB_COLOR(0xFF, 0x40, 0x40)
#define FB_GREEN    FB_COLOR(0x40, 0xFF, 0x40)
#define FB_BLUE     FB_COLOR(0x40, 0x80, 0xFF)
#define FB_YELLOW   FB_COLOR(0xFF, 0xFF, 0x00)
#define FB_CYAN     FB_COLOR(0x00, 0xFF, 0xFF)
#define FB_GRAY     FB_COLOR(0xC0, 0xC0, 0xC0)
#define FB_DARK_GRAY FB_COLOR(0x40, 0x40, 0x40)

/* Boot screen colors */
#define FB_BOOT_BG  FB_COLOR(0x0D, 0x0D, 0x0D)   /* Near black */
#define FB_BOOT_FG  FB_COLOR(0xC8, 0xFF, 0xC8)   /* Light green */

/*
 * Initialize the framebuffer console.
 * @fb: framebuffer descriptor from the boot info
 */
void fb_init(const BootFramebuffer *fb);

/*
 * Clear the screen with the given background color.
 */
void fb_clear(FbColor bg);

/*
 * Print a character at the current cursor position.
 * Handles '\n', '\r', '\t', and '\b'.
 */
void fb_putc(char c);

/*
 * Print a null-terminated string.
 */
void fb_puts(const char *s);

/*
 * Set text foreground and background colors.
 */
void fb_set_colors(FbColor fg, FbColor bg);

/*
 * Draw a single character at the given pixel coordinates.
 */
void fb_draw_char(int x, int y, char c, FbColor fg, FbColor bg);

/*
 * Fill a rectangle with a solid color.
 */
void fb_fill_rect(int x, int y, int w, int h, FbColor color);

/*
 * Returns true if the framebuffer has been successfully initialized.
 */
bool fb_available(void);

/*
 * Raw framebuffer surface — for the GDI subsystem.
 * Exposes the underlying VRAM pointer and geometry so that GDI can
 * write pixels directly without going through the console abstraction.
 */
typedef struct {
    uint32_t *vram;     /* Kernel VA of the linear framebuffer */
    int       width;    /* Horizontal resolution in pixels */
    int       height;   /* Vertical resolution in pixels */
    int       stride;   /* Pixels per scanline (may be > width) */
    bool      bgr;      /* true = pixel_format 0 (BGR), false = RGB */
} FbRawSurface;

void fb_get_raw(FbRawSurface *out);

/*
 * Move the console to another surface (the display driver: a mode change
 * or a page flip).  Keeps the text cursor in range; draws nothing.
 */
void fb_set_surface(uint64_t phys, uint32_t width, uint32_t height,
                    uint32_t stride, bool bgr);

/*
 * Draw a null-terminated string at pixel coordinates with a solid
 * background fill (bg).
 */
void fb_draw_string(int x, int y, const char *s, FbColor fg, FbColor bg);

/*
 * Draw a null-terminated string at pixel coordinates with a
 * transparent background (only foreground pixels are written).
 */
void fb_draw_string_trans(int x, int y, const char *s, FbColor fg);
