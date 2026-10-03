/*
 * dibcolor.c — DIB colour tables.  gdi32 draws on 32-bit pixels; a DIB
 * section of 1, 4 or 8 bits per pixel keeps the program's indices and its
 * colour table beside them (gdi32.c, dib24_sync).  These read and change
 * the table of the section selected into a memory DC; any other bitmap
 * has none, as on Windows for a bitmap of more than 8 bits per pixel.
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include "gdi_int.h"

static GObj *palette_section(HDC h)
{
    NOVA_DC *d = dc_of(h);
    GObj *o = d && d->mem ? bitmap_of((HGDIOBJ)d->bitmap) : NULL;   /* synced: the recolour sees the program's pixels */
    return o && o->kind == K_BITMAP && o->pal ? o : NULL;
}

GDIAPI UINT GetDIBColorTable(HDC h, UINT start, UINT n, RGBQUAD *colors)
{
    GObj *o = palette_section(h);
    if (!o || !colors || start >= (UINT)o->npal) return 0;
    if (n > (UINT)o->npal - start) n = (UINT)o->npal - start;
    memcpy(colors, o->pal + start, n * sizeof(RGBQUAD));
    return n;
}

GDIAPI UINT SetDIBColorTable(HDC h, UINT start, UINT n, const RGBQUAD *colors)
{
    GObj *o = palette_section(h);
    if (!o || !colors || start >= (UINT)o->npal) return 0;
    if (n > (UINT)o->npal - start) n = (UINT)o->npal - start;
    memcpy(o->pal + start, colors, n * sizeof(RGBQUAD));
    dib_recolor(o);
    return n;
}
