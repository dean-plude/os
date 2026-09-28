/*
 * icon.h — Windows icons (.ico files and icons in .exe/.dll resources)
 *
 * An icon holds several images of the same picture at different sizes and
 * colour depths; drawing picks the one that best fits the target size at
 * device resolution and scales it with alpha blending.  Images may be
 * BMP-style DIBs (1, 4, 8, 16, 24 or 32 bits per pixel, with the AND mask)
 * or PNG (as Windows Vista+ icons use for 256x256).  Images are decoded
 * lazily, the first time a size is drawn.
 */

#pragma once

#include "../include/types.h"
#include "gdi.h"

#define ICON_MAX_IMAGES 24

typedef struct {
    int     w, h;          /* from the directory (0 in the file means 256) */
    int     bpp;           /* colour depth (32 for PNG images) */
    bool    png;
    UINT32  off, len;      /* image data within GdiIcon.data */
    UINT32 *px;            /* decoded 0xAARRGGBB, straight alpha (lazily) */
    int     pw, ph;        /* decoded size */
    bool    bad;           /* failed to decode */
} IconImage;

typedef struct GdiIcon {
    UINT8     *data;       /* a private copy of the .ico bytes */
    UINT32     len;
    int        n;
    IconImage  img[ICON_MAX_IMAGES];
} GdiIcon;

/* Parse an .ico (or .cur) file; NULL if it is not a valid icon. */
GdiIcon *IconLoad(const void *data, size_t len);

/* The first icon group (the program's own icon, as Explorer shows it) in
 * the resources of a PE image (.exe/.dll) file; NULL if it has none. */
GdiIcon *IconFromPe(const void *pe, size_t len);

/* A PNG picture wrapped as a one-image icon (thumbnails, Photos). */
GdiIcon *IconFromPng(const void *png, size_t len);

void IconFree(GdiIcon *ic);

/* Draw at logical (x, y) in a size x size box (centred, keeping the aspect
 * ratio); false if nothing could be decoded (the caller then draws a
 * fallback). */
bool IconDraw(GdiIcon *ic, int x, int y, int size);

/* Draw image @i at its own size in device pixels (1:1) at logical (x, y). */
bool IconDrawImage(GdiIcon *ic, int i, int x, int y);

/* Draw image @i centred in @box: at 1:1 device pixels if it fits, else
 * shrunk to fit (keeping the aspect ratio).  False if it cannot be decoded. */
bool IconDrawFit(GdiIcon *ic, int i, GdiRect box);

/* Decode image @i if needed; false if it cannot be decoded. */
bool IconDecode(GdiIcon *ic, int i);
