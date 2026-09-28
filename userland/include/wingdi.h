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
    /* gdi32's own state (user32 leaves these zero) */
    int      fmt;          /* pixel format: 0 COLORREF (0x00BBGGRR), 1 DIB BGRA (0xAARRGGBB) */
    int      flip;         /* rows bottom-up (a DIB with positive height) */
    void    *bitmap, *font, *pen, *brush;   /* selected objects (0: defaults) */
    UINT     text_align;
    int      org_x, org_y; /* viewport origin */
    int      mem;          /* a memory DC (CreateCompatibleDC) */
    struct NOVA_DC *saved; /* SaveDC stack */
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

typedef void *HRGN, *HPALETTE;
typedef struct tagLOGBRUSH { UINT lbStyle; COLORREF lbColor; ULONG_PTR lbHatch; } LOGBRUSH;
typedef struct tagLOGPEN { UINT lopnStyle; POINT lopnWidth; COLORREF lopnColor; } LOGPEN;
typedef struct tagLOGFONTW {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONTW, *LPLOGFONTW;
typedef struct tagLOGFONTA {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    CHAR lfFaceName[32];
} LOGFONTA, *LPLOGFONTA;
typedef struct tagTEXTMETRICW {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth,
         tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} TEXTMETRICW, *LPTEXTMETRICW;
typedef struct tagTEXTMETRICA {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth,
         tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    BYTE tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} TEXTMETRICA, *LPTEXTMETRICA;
typedef struct tagRGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER, *LPBITMAPINFOHEADER;
typedef struct tagBITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO, *LPBITMAPINFO;
typedef struct tagBITMAP { LONG bmType, bmWidth, bmHeight, bmWidthBytes; WORD bmPlanes, bmBitsPixel; LPVOID bmBits; } BITMAP;
typedef struct tagDIBSECTION { BITMAP dsBm; BITMAPINFOHEADER dsBmih; DWORD dsBitfields[3]; HANDLE dshSection; DWORD dsOffset; } DIBSECTION;
typedef struct _BLENDFUNCTION { BYTE BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; } BLENDFUNCTION;
typedef struct _ABC { int abcA; UINT abcB; int abcC; } ABC;
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define AC_SRC_OVER 0
#define AC_SRC_ALPHA 1
#define FW_NORMAL 400
#define FW_BOLD 700
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_TOP 0
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define ETO_OPAQUE 2
#define ETO_CLIPPED 4
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define SRCPAINT 0x00EE0086
#define SRCAND 0x008800C6

GDIAPI COLORREF SetPixel(HDC dc, int x, int y, COLORREF c);
GDIAPI HDC      CreateCompatibleDC(HDC dc);
GDIAPI BOOL     DeleteDC(HDC dc);
GDIAPI HBITMAP  CreateCompatibleBitmap(HDC dc, int w, int h);
GDIAPI HBITMAP  CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits);
GDIAPI HBITMAP  CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset);
GDIAPI BOOL     StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop);
GDIAPI int      StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits,
                              const BITMAPINFO *bi, UINT usage, DWORD rop);
GDIAPI int      SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines,
                                  const void *bits, const BITMAPINFO *bi, UINT usage);
GDIAPI int      GetDIBits(HDC dc, HBITMAP bmp, UINT start, UINT lines, void *bits, BITMAPINFO *bi, UINT usage);
GDIAPI BOOL     AlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf);
GDIAPI HFONT    CreateFontIndirectW(const LOGFONTW *lf);
GDIAPI HFONT    CreateFontIndirectA(const LOGFONTA *lf);
GDIAPI HFONT    CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                            DWORD charset, DWORD outprec, DWORD clip, DWORD quality, DWORD pitch, LPCWSTR face);
GDIAPI BOOL     ExtTextOutW(HDC dc, int x, int y, UINT opts, const RECT *rc, LPCWSTR s, UINT n, const INT *dx);
GDIAPI BOOL     GetTextExtentPoint32W(HDC dc, LPCWSTR s, int n, LPSIZE sz);
GDIAPI BOOL     GetTextMetricsW(HDC dc, TEXTMETRICW *tm);
GDIAPI BOOL     GetTextMetricsA(HDC dc, TEXTMETRICA *tm);
GDIAPI UINT     SetTextAlign(HDC dc, UINT align);
GDIAPI BOOL     Polygon(HDC dc, const POINT *pt, int n);
GDIAPI BOOL     Polyline(HDC dc, const POINT *pt, int n);
GDIAPI BOOL     RoundRect(HDC dc, int l, int t, int r, int b, int w, int h);
GDIAPI int      SaveDC(HDC dc);
GDIAPI BOOL     RestoreDC(HDC dc, int which);
GDIAPI int      GetDeviceCaps(HDC dc, int index);
GDIAPI int      GetObjectW(HGDIOBJ h, int n, LPVOID out);
GDIAPI int      GetObjectA(HGDIOBJ h, int n, LPVOID out);
GDIAPI HGDIOBJ  GetCurrentObject(HDC dc, UINT type);
GDIAPI HRGN     CreateRectRgn(int l, int t, int r, int b);
GDIAPI int      SelectClipRgn(HDC dc, HRGN r);
GDIAPI BOOL     SetViewportOrgEx(HDC dc, int x, int y, LPPOINT old);
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
