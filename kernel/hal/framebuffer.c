/*
 * framebuffer.c — GOP framebuffer text console
 *
 * Font: we embed a subset of the standard PC VGA 8×16 font (256 glyphs,
 * 16 bytes per glyph, each byte is one row of 8 pixels).
 * Only ASCII 0x20–0x7E (printable characters) are included.
 * The font data below is derived from the classic IBM PC BIOS font,
 * which is in the public domain.
 */

#include "framebuffer.h"
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * 8×16 VGA bitmap font (ASCII 0x20 – 0x7E, 95 characters)
 *
 * Source: classical CP437 VGA font, public domain
 * Format: 16 bytes per character, MSB of each byte is leftmost pixel
 * ----------------------------------------------------------------------- */
static const uint8_t font8x16[95][16] = {
    /* 0x20 space */ { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    /* 0x21 ! */     { 0,0,24,24,24,24,24,24,0,0,24,24,0,0,0,0 },
    /* 0x22 " */     { 0,0,54,54,54,0,0,0,0,0,0,0,0,0,0,0 },
    /* 0x23 # */     { 0,0,54,54,126,54,54,126,54,54,0,0,0,0,0,0 },
    /* 0x24 $ */     { 0,24,126,219,216,216,126,27,27,219,126,24,0,0,0,0 },
    /* 0x25 % */     { 0,0,97,102,12,24,48,102,67,0,0,0,0,0,0,0 },
    /* 0x26 & */     { 0,28,54,54,28,59,102,102,59,0,0,0,0,0,0,0 },
    /* 0x27 ' */     { 0,0,24,24,24,0,0,0,0,0,0,0,0,0,0,0 },
    /* 0x28 ( */     { 0,0,12,24,48,48,48,48,48,24,12,0,0,0,0,0 },
    /* 0x29 ) */     { 0,0,48,24,12,12,12,12,12,24,48,0,0,0,0,0 },
    /* 0x2A * */     { 0,0,0,102,60,255,60,102,0,0,0,0,0,0,0,0 },
    /* 0x2B + */     { 0,0,0,24,24,126,24,24,0,0,0,0,0,0,0,0 },
    /* 0x2C , */     { 0,0,0,0,0,0,0,0,0,24,24,48,0,0,0,0 },
    /* 0x2D - */     { 0,0,0,0,0,126,0,0,0,0,0,0,0,0,0,0 },
    /* 0x2E . */     { 0,0,0,0,0,0,0,0,0,24,24,0,0,0,0,0 },
    /* 0x2F / */     { 0,3,6,12,24,48,96,192,0,0,0,0,0,0,0,0 },
    /* 0x30 0 */     { 0,0,60,102,110,118,102,102,60,0,0,0,0,0,0,0 },
    /* 0x31 1 */     { 0,0,24,56,24,24,24,24,126,0,0,0,0,0,0,0 },
    /* 0x32 2 */     { 0,0,60,102,6,12,48,96,126,0,0,0,0,0,0,0 },
    /* 0x33 3 */     { 0,0,60,102,6,28,6,102,60,0,0,0,0,0,0,0 },
    /* 0x34 4 */     { 0,0,14,30,54,102,127,6,6,0,0,0,0,0,0,0 },
    /* 0x35 5 */     { 0,0,126,96,124,6,6,102,60,0,0,0,0,0,0,0 },
    /* 0x36 6 */     { 0,0,28,48,96,124,102,102,60,0,0,0,0,0,0,0 },
    /* 0x37 7 */     { 0,0,126,6,12,24,48,48,48,0,0,0,0,0,0,0 },
    /* 0x38 8 */     { 0,0,60,102,102,60,102,102,60,0,0,0,0,0,0,0 },
    /* 0x39 9 */     { 0,0,60,102,102,62,6,12,56,0,0,0,0,0,0,0 },
    /* 0x3A : */     { 0,0,0,24,24,0,0,24,24,0,0,0,0,0,0,0 },
    /* 0x3B ; */     { 0,0,0,24,24,0,0,24,24,48,0,0,0,0,0,0 },
    /* 0x3C < */     { 0,6,12,24,48,96,48,24,12,6,0,0,0,0,0,0 },
    /* 0x3D = */     { 0,0,0,0,126,0,0,126,0,0,0,0,0,0,0,0 },
    /* 0x3E > */     { 0,96,48,24,12,6,12,24,48,96,0,0,0,0,0,0 },
    /* 0x3F ? */     { 0,0,60,102,6,12,24,0,24,0,0,0,0,0,0,0 },
    /* 0x40 @ */     { 0,62,99,111,111,111,110,96,62,0,0,0,0,0,0,0 },
    /* 0x41 A */     { 0,0,24,60,102,102,126,102,102,0,0,0,0,0,0,0 },
    /* 0x42 B */     { 0,0,124,102,102,124,102,102,124,0,0,0,0,0,0,0 },
    /* 0x43 C */     { 0,0,60,102,96,96,96,102,60,0,0,0,0,0,0,0 },
    /* 0x44 D */     { 0,0,120,108,102,102,102,108,120,0,0,0,0,0,0,0 },
    /* 0x45 E */     { 0,0,126,96,96,120,96,96,126,0,0,0,0,0,0,0 },
    /* 0x46 F */     { 0,0,126,96,96,120,96,96,96,0,0,0,0,0,0,0 },
    /* 0x47 G */     { 0,0,60,102,96,110,102,102,60,0,0,0,0,0,0,0 },
    /* 0x48 H */     { 0,0,102,102,102,126,102,102,102,0,0,0,0,0,0,0 },
    /* 0x49 I */     { 0,0,60,24,24,24,24,24,60,0,0,0,0,0,0,0 },
    /* 0x4A J */     { 0,0,30,12,12,12,108,108,56,0,0,0,0,0,0,0 },
    /* 0x4B K */     { 0,0,102,108,120,112,120,108,102,0,0,0,0,0,0,0 },
    /* 0x4C L */     { 0,0,96,96,96,96,96,96,126,0,0,0,0,0,0,0 },
    /* 0x4D M */     { 0,0,99,119,127,107,99,99,99,0,0,0,0,0,0,0 },
    /* 0x4E N */     { 0,0,102,118,126,126,110,102,102,0,0,0,0,0,0,0 },
    /* 0x4F O */     { 0,0,60,102,102,102,102,102,60,0,0,0,0,0,0,0 },
    /* 0x50 P */     { 0,0,124,102,102,124,96,96,96,0,0,0,0,0,0,0 },
    /* 0x51 Q */     { 0,0,60,102,102,102,102,60,14,0,0,0,0,0,0,0 },
    /* 0x52 R */     { 0,0,124,102,102,124,120,108,102,0,0,0,0,0,0,0 },
    /* 0x53 S */     { 0,0,60,102,96,60,6,102,60,0,0,0,0,0,0,0 },
    /* 0x54 T */     { 0,0,126,24,24,24,24,24,24,0,0,0,0,0,0,0 },
    /* 0x55 U */     { 0,0,102,102,102,102,102,102,60,0,0,0,0,0,0,0 },
    /* 0x56 V */     { 0,0,102,102,102,102,60,60,24,0,0,0,0,0,0,0 },
    /* 0x57 W */     { 0,0,99,99,99,107,127,119,99,0,0,0,0,0,0,0 },
    /* 0x58 X */     { 0,0,102,102,60,24,60,102,102,0,0,0,0,0,0,0 },
    /* 0x59 Y */     { 0,0,102,102,102,60,24,24,24,0,0,0,0,0,0,0 },
    /* 0x5A Z */     { 0,0,126,6,12,24,48,96,126,0,0,0,0,0,0,0 },
    /* 0x5B [ */     { 0,0,60,48,48,48,48,48,60,0,0,0,0,0,0,0 },
    /* 0x5C \ */     { 0,192,96,48,24,12,6,3,0,0,0,0,0,0,0,0 },
    /* 0x5D ] */     { 0,0,60,12,12,12,12,12,60,0,0,0,0,0,0,0 },
    /* 0x5E ^ */     { 0,24,60,102,0,0,0,0,0,0,0,0,0,0,0,0 },
    /* 0x5F _ */     { 0,0,0,0,0,0,0,0,0,126,0,0,0,0,0,0 },
    /* 0x60 ` */     { 0,48,24,12,0,0,0,0,0,0,0,0,0,0,0,0 },
    /* 0x61 a */     { 0,0,0,0,60,6,62,102,62,0,0,0,0,0,0,0 },
    /* 0x62 b */     { 0,96,96,96,124,102,102,102,124,0,0,0,0,0,0,0 },
    /* 0x63 c */     { 0,0,0,0,60,96,96,96,60,0,0,0,0,0,0,0 },
    /* 0x64 d */     { 0,6,6,6,62,102,102,102,62,0,0,0,0,0,0,0 },
    /* 0x65 e */     { 0,0,0,0,60,102,126,96,60,0,0,0,0,0,0,0 },
    /* 0x66 f */     { 0,28,54,48,120,48,48,48,48,0,0,0,0,0,0,0 },
    /* 0x67 g */     { 0,0,0,0,62,102,102,62,6,124,0,0,0,0,0,0 },
    /* 0x68 h */     { 0,96,96,96,124,102,102,102,102,0,0,0,0,0,0,0 },
    /* 0x69 i */     { 0,0,24,0,24,24,24,24,24,0,0,0,0,0,0,0 },
    /* 0x6A j */     { 0,0,6,0,6,6,6,6,6,108,56,0,0,0,0,0 },
    /* 0x6B k */     { 0,96,96,102,108,120,108,102,102,0,0,0,0,0,0,0 },
    /* 0x6C l */     { 0,24,24,24,24,24,24,24,12,0,0,0,0,0,0,0 },
    /* 0x6D m */     { 0,0,0,0,102,127,127,107,99,0,0,0,0,0,0,0 },
    /* 0x6E n */     { 0,0,0,0,124,102,102,102,102,0,0,0,0,0,0,0 },
    /* 0x6F o */     { 0,0,0,0,60,102,102,102,60,0,0,0,0,0,0,0 },
    /* 0x70 p */     { 0,0,0,0,124,102,102,124,96,96,0,0,0,0,0,0 },
    /* 0x71 q */     { 0,0,0,0,62,102,102,62,6,6,0,0,0,0,0,0 },
    /* 0x72 r */     { 0,0,0,0,108,118,96,96,96,0,0,0,0,0,0,0 },
    /* 0x73 s */     { 0,0,0,0,60,96,60,6,124,0,0,0,0,0,0,0 },
    /* 0x74 t */     { 0,48,48,126,48,48,48,54,28,0,0,0,0,0,0,0 },
    /* 0x75 u */     { 0,0,0,0,102,102,102,102,60,0,0,0,0,0,0,0 },
    /* 0x76 v */     { 0,0,0,0,102,102,60,60,24,0,0,0,0,0,0,0 },
    /* 0x77 w */     { 0,0,0,0,99,107,127,62,54,0,0,0,0,0,0,0 },
    /* 0x78 x */     { 0,0,0,0,102,60,24,60,102,0,0,0,0,0,0,0 },
    /* 0x79 y */     { 0,0,0,0,102,102,62,6,60,0,0,0,0,0,0,0 },
    /* 0x7A z */     { 0,0,0,0,126,12,24,48,126,0,0,0,0,0,0,0 },
    /* 0x7B { */     { 0,14,24,24,112,24,24,24,14,0,0,0,0,0,0,0 },
    /* 0x7C | */     { 0,24,24,24,24,24,24,24,24,0,0,0,0,0,0,0 },
    /* 0x7D } */     { 0,112,24,24,14,24,24,24,112,0,0,0,0,0,0,0 },
    /* 0x7E ~ */     { 0,118,220,0,0,0,0,0,0,0,0,0,0,0,0,0 },
};

/* -----------------------------------------------------------------------
 * Framebuffer state
 * ----------------------------------------------------------------------- */

#define FONT_W  8
#define FONT_H  16

static struct {
    uint32_t       *base;           /* Kernel virtual address */
    uint32_t        width;
    uint32_t        height;
    uint32_t        stride;         /* pixels per scanline */
    bool            bgr;            /* true = BGRX, false = RGBX */
    bool            ready;

    /* Text cursor */
    int             cursor_col;
    int             cursor_row;
    int             cols;           /* screen width in characters */
    int             rows;           /* screen height in characters */

    FbColor         fg;
    FbColor         bg;
} fb;

/* -----------------------------------------------------------------------
 * Color adjustment for pixel format
 * ----------------------------------------------------------------------- */
static uint32_t fb_color_pixel(FbColor c)
{
    if (!fb.bgr) {
        /* RGB → swap R and B */
        uint32_t b = (c >> 16) & 0xFF;
        uint32_t g = (c >>  8) & 0xFF;
        uint32_t r = (c >>  0) & 0xFF;
        return (b << 0) | (g << 8) | (r << 16);
    }
    return c;  /* BGR — already correct */
}

/* -----------------------------------------------------------------------
 * fb_init
 * ----------------------------------------------------------------------- */
void fb_init(const BootFramebuffer *bfb)
{
    if (!bfb || !bfb->base) {
        fb.ready = false;
        return;
    }

    fb.base   = (uint32_t *)(PHYSMAP_BASE + bfb->base);
    fb.width  = bfb->width;
    fb.height = bfb->height;
    fb.stride = bfb->pixels_per_scanline;
    fb.bgr    = (bfb->pixel_format == 0);  /* 0 = BGR */
    fb.ready  = true;
    fb.cols   = (int)(bfb->width  / FONT_W);
    fb.rows   = (int)(bfb->height / FONT_H);
    fb.fg     = FB_BOOT_FG;
    fb.bg     = FB_BOOT_BG;
    fb.cursor_col = 0;
    fb.cursor_row = 0;

    fb_clear(fb.bg);
}

bool fb_available(void) { return fb.ready; }

/* -----------------------------------------------------------------------
 * fb_get_raw — expose raw VRAM surface to GDI subsystem
 * ----------------------------------------------------------------------- */
void fb_get_raw(FbRawSurface *out)
{
    if (!out) return;
    out->vram   = fb.base;
    out->width  = (int)fb.width;
    out->height = (int)fb.height;
    out->stride = (int)fb.stride;
    out->bgr    = fb.bgr;
}

/* -----------------------------------------------------------------------
 * fb_draw_string — draw string with solid background
 * ----------------------------------------------------------------------- */
void fb_draw_string(int x, int y, const char *s, FbColor fg, FbColor bg)
{
    if (!fb.ready || !s) return;
    for (; *s; s++, x += FONT_W)
        fb_draw_char(x, y, *s, fg, bg);
}

/* -----------------------------------------------------------------------
 * fb_draw_string_trans — draw string, transparent background.
 * Only foreground (set) pixels are written; the background is preserved.
 * ----------------------------------------------------------------------- */
void fb_draw_string_trans(int x, int y, const char *s, FbColor fg)
{
    if (!fb.ready || !s) return;
    uint32_t px = fb_color_pixel(fg);
    for (; *s; s++, x += FONT_W) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const uint8_t *glyph = font8x16[c - 0x20];
        for (int row = 0; row < FONT_H; row++) {
            int py = y + row;
            if (py < 0 || py >= (int)fb.height) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                if (!(bits & (0x80u >> col))) continue;
                int px_x = x + col;
                if (px_x < 0 || px_x >= (int)fb.width) continue;
                fb.base[py * fb.stride + px_x] = px;
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * fb_fill_rect
 * ----------------------------------------------------------------------- */
void fb_fill_rect(int x, int y, int w, int h, FbColor color)
{
    if (!fb.ready) return;
    uint32_t px = fb_color_pixel(color);
    for (int row = y; row < y + h && row < (int)fb.height; row++) {
        uint32_t *line = fb.base + (uint32_t)row * fb.stride + (uint32_t)x;
        for (int col = 0; col < w && x + col < (int)fb.width; col++) {
            line[col] = px;
        }
    }
}

/* -----------------------------------------------------------------------
 * fb_clear
 * ----------------------------------------------------------------------- */
void fb_clear(FbColor bg)
{
    if (!fb.ready) return;
    uint32_t px = fb_color_pixel(bg);
    for (uint32_t row = 0; row < fb.height; row++) {
        uint32_t *line = fb.base + row * fb.stride;
        for (uint32_t col = 0; col < fb.width; col++) {
            line[col] = px;
        }
    }
    fb.cursor_col = 0;
    fb.cursor_row = 0;
    fb.bg = bg;
}

/* -----------------------------------------------------------------------
 * fb_draw_char — render one character at pixel coordinates
 * ----------------------------------------------------------------------- */
void fb_draw_char(int px, int py, char c, FbColor fg, FbColor bg)
{
    if (!fb.ready) return;

    int glyph_idx = (int)(unsigned char)c - 0x20;
    if (glyph_idx < 0 || glyph_idx >= 95) glyph_idx = 0;  /* space */

    const uint8_t *glyph = font8x16[glyph_idx];
    uint32_t fg_px = fb_color_pixel(fg);
    uint32_t bg_px = fb_color_pixel(bg);

    for (int row = 0; row < FONT_H; row++) {
        int y = py + row;
        if (y < 0 || y >= (int)fb.height) continue;
        uint32_t *line = fb.base + (uint32_t)y * fb.stride;
        uint8_t  bits  = glyph[row];
        for (int bit = 0; bit < FONT_W; bit++) {
            int x = px + (FONT_W - 1 - bit);
            if (x < 0 || x >= (int)fb.width) continue;
            line[x] = (bits & (1u << bit)) ? fg_px : bg_px;
        }
    }
}

/* -----------------------------------------------------------------------
 * Scroll up by one line
 * ----------------------------------------------------------------------- */
static void fb_scroll(void)
{
    if (!fb.ready) return;
    /* Move all rows up by FONT_H pixels */
    uint32_t line_bytes = fb.stride * FONT_H * sizeof(uint32_t);
    uint32_t total_lines = (uint32_t)(fb.rows - 1);
    uint8_t *dst = (uint8_t *)fb.base;
    uint8_t *src = dst + line_bytes;
    __builtin_memmove(dst, src, total_lines * line_bytes);
    /* Clear the last row */
    fb_fill_rect(0, (int)((fb.rows - 1) * FONT_H), (int)fb.width, FONT_H, fb.bg);
}

/* -----------------------------------------------------------------------
 * fb_putc
 * ----------------------------------------------------------------------- */
void fb_putc(char c)
{
    if (!fb.ready) return;

    if (c == '\n') {
        fb.cursor_col = 0;
        fb.cursor_row++;
    } else if (c == '\r') {
        fb.cursor_col = 0;
    } else if (c == '\t') {
        /* Advance to next 8-column tab stop */
        fb.cursor_col = (fb.cursor_col + 8) & ~7;
    } else if (c == '\b') {
        if (fb.cursor_col > 0) {
            fb.cursor_col--;
            fb_draw_char(fb.cursor_col * FONT_W, fb.cursor_row * FONT_H,
                         ' ', fb.fg, fb.bg);
        }
    } else if (c >= 0x20 && c < 0x7F) {
        fb_draw_char(fb.cursor_col * FONT_W, fb.cursor_row * FONT_H,
                     c, fb.fg, fb.bg);
        fb.cursor_col++;
    }

    if (fb.cursor_col >= fb.cols) {
        fb.cursor_col = 0;
        fb.cursor_row++;
    }
    if (fb.cursor_row >= fb.rows) {
        fb_scroll();
        fb.cursor_row = fb.rows - 1;
    }
}

void fb_puts(const char *s)
{
    while (*s) fb_putc(*s++);
}

void fb_set_colors(FbColor fg, FbColor bg)
{
    fb.fg = fg;
    fb.bg = bg;
}
