/*
 * msimg32.dll — the blending calls.  They live in gdi32, as on Windows,
 * where msimg32 forwards to gdi32's Gdi* functions.
 */
#include <windows.h>

#define MSIMGAPI __declspec(dllexport)

BOOL WINAPI GdiGradientFill(HDC h, PVOID v, ULONG nv, PVOID mesh, ULONG nm, ULONG mode);
BOOL WINAPI TransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, UINT key);

/* exported under gdi32's names, which this file imports */
#ifdef _WIN64
#pragma comment(linker, "/export:AlphaBlend=msimg_AlphaBlend")
#pragma comment(linker, "/export:TransparentBlt=msimg_TransparentBlt")
#else
#pragma comment(linker, "/export:AlphaBlend=_msimg_AlphaBlend@44")
#pragma comment(linker, "/export:TransparentBlt=_msimg_TransparentBlt@44")
#endif

BOOL WINAPI msimg_AlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh,
                                      BLENDFUNCTION bf)
{ return AlphaBlend(dst, x, y, w, h, src, sx, sy, sw, sh, bf); }
BOOL WINAPI msimg_TransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh,
                                          UINT key)
{ return TransparentBlt(dst, x, y, w, h, src, sx, sy, sw, sh, key); }
MSIMGAPI BOOL WINAPI GradientFill(HDC h, PVOID v, ULONG nv, PVOID mesh, ULONG nm, ULONG mode)
{ return GdiGradientFill(h, v, nv, mesh, nm, mode); }
MSIMGAPI BOOL WINAPI vSetDdrawflag(void) { return FALSE; }
