/*
 * dibcolor.c — DIB colour tables.  gdi32 keeps every bitmap as 32-bit
 * pixels (palette DIBs are expanded when their bits are set), so no
 * selected bitmap has a colour table to read or change: both calls report
 * no entries, as Windows does for a bitmap of more than 8 bits per pixel.
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include "gdi_int.h"

GDIAPI UINT GetDIBColorTable(HDC h, UINT start, UINT n, RGBQUAD *colors)
{
    (void)h; (void)start; (void)n; (void)colors;
    return 0;
}

GDIAPI UINT SetDIBColorTable(HDC h, UINT start, UINT n, const RGBQUAD *colors)
{
    (void)h; (void)start; (void)n; (void)colors;
    return 0;
}
