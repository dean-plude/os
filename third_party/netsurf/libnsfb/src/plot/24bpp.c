/*
 * Copyright 2009 Vincent Sanders <vince@simtec.co.uk>
 *
 * This file is part of libnsfb, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"

#include "nsfb.h"
#include "plot.h"

/**
 * Get the address of a logical location on the framebuffer
 */
static inline uint8_t *get_xy_loc(nsfb_t *nsfb, int x, int y)
{
        return (uint8_t *)(nsfb->ptr + (y * nsfb->linelen) + (x * 3));
}

#ifdef NSFB_BE_BYTE_ORDER

/**
 * convert a 24bpp big endian pixel value to netsurf colour
 */
static inline nsfb_colour_t pixel_to_colour(uint32_t pixel)
{
        return (pixel >> 8) & ~0xFF000000U;
}

/**
 * convert a colour value to a big endian 24bpp pixel value
 *
 * The resulting value is ready for screen output
 */
static inline uint32_t colour_to_pixel(nsfb_colour_t c)
{
        return (c << 8);
}

#else

/**
 * convert a 24bpp little endian pixel value to netsurf colour
 *
 * \param nsfb The framebuffer
 * \param pixel The pixel values
 * \return The netsurf colour value.
 */
static inline nsfb_colour_t pixel_to_colour(uint32_t pixel)
{
        return ((pixel & 0xFF) << 16) |
                ((pixel & 0xFF00)) |
                ((pixel & 0xFF0000) >> 16);
}

/**
 * convert a colour value to a little endian 24bpp pixel value
 *
 * \param c The netsurf colour
 * \return A pixel value ready for screen output.
 */
static inline uint32_t colour_to_pixel(nsfb_colour_t c)
{
        return ((c & 0xff0000) >> 16) | (c & 0xff00) | ((c & 0xff) << 16);
}

#endif

static inline nsfb_colour_t read_pixel(const uint8_t *pixel)
{
        uint32_t value = 0;

        memcpy(&value, pixel, 3);
        return pixel_to_colour(value);
}

static inline void write_pixel(uint8_t *pixel, nsfb_colour_t colour)
{
        uint32_t value = colour_to_pixel(colour);

        memcpy(pixel, &value, 3);
}

#define SIGN(x)  ((x<0) ?  -1  :  ((x>0) ? 1 : 0))

static bool
line(nsfb_t *nsfb, int linec, nsfb_bbox_t *line, nsfb_plot_pen_t *pen)
{
        int w;
        uint8_t *pvideo;
        int x, y, i;
        int dx, dy, sdy;
        int dxabs, dyabs;

        for (;linec > 0; linec--) {

                if (line->y0 == line->y1) {
                        /* horizontal line special cased */

                        if (!nsfb_plot_clip_ctx(nsfb, line)) {
                                /* line outside clipping */
                                line++;
                                continue;
                        }

                        pvideo = get_xy_loc(nsfb, line->x0, line->y0);

                        w = line->x1 - line->x0;
                        while (w-- > 0)
                                write_pixel(pvideo + w * 3, pen->stroke_colour);

                } else {
                        /* standard bresenham line */

                        if (!nsfb_plot_clip_line_ctx(nsfb, line)) {
                                /* line outside clipping */
                                line++;
                                continue;
                        }

                        /* the horizontal distance of the line */
                        dx = line->x1 - line->x0;
                        dxabs = abs (dx);

                        /* the vertical distance of the line */
                        dy = line->y1 - line->y0;
                        dyabs = abs (dy);

                        sdy = dx ? SIGN(dy) * SIGN(dx) : SIGN(dy);

                        if (dx >= 0)
                                pvideo = get_xy_loc(nsfb, line->x0, line->y0);
                        else
                                pvideo = get_xy_loc(nsfb, line->x1, line->y1);

                        x = dyabs >> 1;
                        y = dxabs >> 1;

                        if (dxabs >= dyabs) {
                                /* the line is more horizontal than vertical */
                                for (i = 0; i < dxabs; i++) {
                                        write_pixel(pvideo, pen->stroke_colour);

                                        pvideo += 3;
                                        y += dyabs;
                                        if (y >= dxabs) {
                                                y -= dxabs;
                                                pvideo += sdy * nsfb->linelen;
                                        }
                                }
                        } else {
                                /* the line is more vertical than horizontal */
                                for (i = 0; i < dyabs; i++) {
                                        write_pixel(pvideo, pen->stroke_colour);
                                        pvideo += sdy * nsfb->linelen;

                                        x += dxabs;
                                        if (x >= dyabs) {
                                                x -= dyabs;
                                                pvideo += 3;
                                        }
                                }
                        }

                }
                line++;
        }
        return true;
}



static bool fill(nsfb_t *nsfb, nsfb_bbox_t *rect, nsfb_colour_t c)
{
        int w;
        uint8_t *pvid;
        uint32_t width;
        uint32_t height;

        if (!nsfb_plot_clip_ctx(nsfb, rect))
                return true; /* fill lies outside current clipping region */

        width = rect->x1 - rect->x0;
        height = rect->y1 - rect->y0;
        pvid = get_xy_loc(nsfb, rect->x0, rect->y0);

        while (height-- > 0) {
                for (w = 0; w < (int)width; w++)
                        write_pixel(pvid + w * 3, c);
                pvid += nsfb->linelen;
        }

        return true;
}




static bool point(nsfb_t *nsfb, int x, int y, nsfb_colour_t c)
{
        uint8_t *pvideo;

        /* check point lies within clipping region */
        if ((x < nsfb->clip.x0) ||
            (x >= nsfb->clip.x1) ||
            (y < nsfb->clip.y0) ||
            (y >= nsfb->clip.y1))
                return true;

        pvideo = get_xy_loc(nsfb, x, y);

        if ((c & 0xFF000000) != 0) {
                if ((c & 0xFF000000) != 0xFF000000) {
                        c = nsfb_plot_ablend(c, read_pixel(pvideo));
                }

                write_pixel(pvideo, c);
        }
        return true;
}

static bool
glyph1(nsfb_t *nsfb,
       nsfb_bbox_t *loc,
       const uint8_t *pixel,
       int pitch,
       nsfb_colour_t c)
{
        uint8_t *pvideo;
        int xloop, yloop;
        int xoff, yoff; /* x and y offset into image */
        int x = loc->x0;
        int y = loc->y0;
        const uint8_t *fntd;
        uint8_t row;

        if (!nsfb_plot_clip_ctx(nsfb, loc))
                return true;

        xoff = loc->x0 - x;
        yoff = loc->y0 - y;

        pvideo = get_xy_loc(nsfb, loc->x0, loc->y0);

        for (yloop = 0; yloop < loc->y1 - loc->y0; yloop++) {
                for (xloop = 0; xloop < loc->x1 - loc->x0; xloop++) {
                        int source_x = xoff + xloop;
                        int source_y = yoff + yloop;

                        fntd = pixel + source_y * (pitch >> 3) +
                                (source_x >> 3);
                        row = (uint8_t)(*fntd << (source_x & 7));
                        if ((row & 0x80) != 0)
                                write_pixel(pvideo + xloop * 3, c);
                }
                pvideo += nsfb->linelen;
        }

        return true;
}

static bool
glyph8(nsfb_t *nsfb,
       nsfb_bbox_t *loc,
       const uint8_t *pixel,
       int pitch,
       nsfb_colour_t c)
{
        uint8_t *pvideo;
        nsfb_colour_t abpixel; /* alphablended pixel */
        int xloop, yloop;
        int xoff, yoff; /* x and y offset into image */
        int x = loc->x0;
        int y = loc->y0;

        if (!nsfb_plot_clip_ctx(nsfb, loc))
                return true;

        xoff = loc->x0 - x;
        yoff = loc->y0 - y;

        pvideo = get_xy_loc(nsfb, loc->x0, loc->y0);

        for (yloop = 0; yloop < loc->y1 - loc->y0; yloop++) {
                for (xloop = 0; xloop < loc->x1 - loc->x0; xloop++) {
                        abpixel = ((nsfb_colour_t)pixel[
                                ((yoff + yloop) * pitch) + xloop + xoff]
                                << 24) | (c & 0xFFFFFF);
                        if ((abpixel & 0xFF000000) != 0) {
                                /* pixel is not transparent */
                                if ((abpixel & 0xFF000000) != 0xFF000000) {
                                        abpixel = nsfb_plot_ablend(abpixel,
                                                                   read_pixel(pvideo + xloop * 3));
                                }

                                write_pixel(pvideo + xloop * 3, abpixel);
                        }
                }
                pvideo += nsfb->linelen;
        }

        return true;
}

static bool
bitmap(nsfb_t *nsfb,
       const nsfb_bbox_t *loc,
       const nsfb_colour_t *pixel,
       int bmp_width,
       int bmp_height,
       int bmp_stride,
       bool alpha)
{
        uint8_t *pvideo;
        nsfb_colour_t abpixel;
        int xloop, yloop;
        int x = loc->x0;
        int y = loc->y0;
        int width = loc->x1 - loc->x0;
        int height = loc->y1 - loc->y0;
        nsfb_bbox_t clipped; /* clipped display */

        if (width <= 0 || height <= 0 || bmp_width <= 0 || bmp_height <= 0 ||
            bmp_stride < bmp_width)
                return true;

        clipped.x0 = x;
        clipped.y0 = y;
        clipped.x1 = loc->x1;
        clipped.y1 = loc->y1;

        if (!nsfb_plot_clip_ctx(nsfb, &clipped))
                return true;

        pvideo = get_xy_loc(nsfb, clipped.x0, clipped.y0);
        for (yloop = 0; yloop < clipped.y1 - clipped.y0; yloop++) {
                int destination_y = clipped.y0 - y + yloop;
                int source_y = (int)(((int64_t)destination_y * bmp_height) /
                                     height);

                for (xloop = 0; xloop < clipped.x1 - clipped.x0; xloop++) {
                        int destination_x = clipped.x0 - x + xloop;
                        int source_x = (int)(((int64_t)destination_x *
                                              bmp_width) / width);

                        abpixel = pixel[(size_t)source_y * bmp_stride +
                                        source_x];
                        if (alpha) {
                                if ((abpixel & 0xFF000000) == 0)
                                        continue;
                                if ((abpixel & 0xFF000000) != 0xFF000000) {
                                        abpixel = nsfb_plot_ablend(
                                                abpixel,
                                                read_pixel(pvideo + xloop * 3));
                                }
                        }
                        write_pixel(pvideo + xloop * 3, abpixel);
                }
                pvideo += nsfb->linelen;
        }
        return true;
}

static bool
bitmap_tiles(nsfb_t *nsfb,
             const nsfb_bbox_t *loc,
             int tiles_x,
             int tiles_y,
             const nsfb_colour_t *pixel,
             int bmp_width,
             int bmp_height,
             int bmp_stride,
             bool alpha)
{
        int width = loc->x1 - loc->x0;
        int height = loc->y1 - loc->y0;
        int tx, ty;
        bool result = true;

        if (width <= 0 || height <= 0 || tiles_x <= 0 || tiles_y <= 0)
                return true;

        for (ty = 0; ty < tiles_y; ty++) {
                for (tx = 0; tx < tiles_x; tx++) {
                        nsfb_bbox_t tile = {
                                .x0 = loc->x0 + tx * width,
                                .y0 = loc->y0 + ty * height,
                                .x1 = loc->x1 + tx * width,
                                .y1 = loc->y1 + ty * height
                        };

                        result &= bitmap(nsfb, &tile, pixel, bmp_width,
                                         bmp_height, bmp_stride, alpha);
                }
        }

        return result;
}

static bool readrect(nsfb_t *nsfb, nsfb_bbox_t *rect, nsfb_colour_t *buffer)
{
        uint8_t *pvideo;
        int xloop, yloop;
        int width;

        if (!nsfb_plot_clip_ctx(nsfb, rect)) {
                return true;
        }

        width = rect->x1 - rect->x0;

        pvideo = get_xy_loc(nsfb, rect->x0, rect->y0);

        for (yloop = rect->y0; yloop < rect->y1; yloop += 1) {
                for (xloop = 0; xloop < width; xloop++) {
                        *buffer = read_pixel(pvideo + xloop * 3);
                        buffer++;
                }
                pvideo += nsfb->linelen;
        }
        return true;
}

const nsfb_plotter_fns_t _nsfb_24bpp_plotters = {
        .line = line,
        .fill = fill,
        .point = point,
        .bitmap = bitmap,
        .bitmap_tiles = bitmap_tiles,
        .glyph8 = glyph8,
        .glyph1 = glyph1,
        .readrect = readrect,
};

/*
 * Local Variables:
 * c-basic-offset:8
 * End:
 */
