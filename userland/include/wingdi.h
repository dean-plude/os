/* wingdi.h — the GDI subset NovaOS's gdi32.dll implements */
#pragma once
#include <_nova.h>
_NOVA_BEGIN

#ifdef NOVA_BUILD_GDI32
#define GDIAPI __declspec(dllexport) __stdcall
#else
#define GDIAPI __declspec(dllimport) __stdcall
#endif

/* The device context a program draws through — shared with user32, which
 * fills it from a window's client bitmap.  gdi32 rasterizes into `bits`. */
typedef struct NOVA_DC {
    DWORD  *bits;          /* COLORREF pixels, top-down */
    int     stride;        /* pixels per row */
    int     w, h;          /* client size */
    COLORREF text_color;
    COLORREF bk_color;
    int      bk_mode;      /* 1 = TRANSPARENT, 2 = OPAQUE */
    COLORREF pen_color;    int pen_width; int has_pen;
    COLORREF brush_color;  int has_brush;
    int      cx, cy;       /* current position (MoveToEx/LineTo) */
    void    *hwnd;         /* owning window handle */
} NOVA_DC;

#define TRANSPARENT 1
#define OPAQUE      2

#define WHITE_BRUSH  0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH   2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH  4
#define NULL_BRUSH   5
#define HOLLOW_BRUSH NULL_BRUSH
#define WHITE_PEN    6
#define BLACK_PEN    7
#define NULL_PEN     8
#define OEM_FIXED_FONT 10
#define ANSI_FIXED_FONT 11
#define SYSTEM_FONT    13
#define DEFAULT_GUI_FONT 17

#define PS_SOLID 0
#define PS_NULL  5

/* BitBlt raster ops (only SRCCOPY is meaningful here) */
#define SRCCOPY     0x00CC0020
#define BLACKNESS   0x00000042
#define WHITENESS   0x00FF0062
#define PATCOPY     0x00F00021

GDIAPI COLORREF SetPixel(HDC dc, int x, int y, COLORREF c);
GDIAPI COLORREF GetPixel(HDC dc, int x, int y);
GDIAPI COLORREF SetTextColor(HDC dc, COLORREF c);
GDIAPI COLORREF SetBkColor(HDC dc, COLORREF c);
GDIAPI int      SetBkMode(HDC dc, int mode);
GDIAPI BOOL     MoveToEx(HDC dc, int x, int y, LPPOINT old);
GDIAPI BOOL     LineTo(HDC dc, int x, int y);
GDIAPI BOOL     Rectangle(HDC dc, int l, int t, int r, int b);
GDIAPI BOOL     Ellipse(HDC dc, int l, int t, int r, int b);
GDIAPI BOOL     PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop);
GDIAPI BOOL     BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
GDIAPI BOOL     TextOutA(HDC dc, int x, int y, LPCSTR s, int len);
GDIAPI BOOL     TextOutW(HDC dc, int x, int y, LPCWSTR s, int len);
GDIAPI BOOL     GetTextExtentPoint32A(HDC dc, LPCSTR s, int len, LPSIZE sz);
GDIAPI HGDIOBJ  GetStockObject(int obj);
GDIAPI HBRUSH   CreateSolidBrush(COLORREF c);
GDIAPI HPEN     CreatePen(int style, int width, COLORREF c);
GDIAPI HGDIOBJ  SelectObject(HDC dc, HGDIOBJ obj);
GDIAPI BOOL     DeleteObject(HGDIOBJ obj);
#define TextOut TextOutA
#define GetTextExtentPoint32 GetTextExtentPoint32A

_NOVA_END
