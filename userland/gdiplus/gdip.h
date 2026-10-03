/*
 * gdip.h — gdiplus.dll's types and objects, shared by its files:
 *   gdiplus.c   startup, bitmaps (load, save, lock, convert), encoders
 *   graphics.c  graphics objects, brushes, pens, drawing, matrices, regions
 *   path.c      paths (GDI+'s point and type arrays) and path iterators
 *   text.c      fonts, font families, string formats, drawing and measuring text
 */
#pragma once
#include <windows.h>
#include <objbase.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "plutovg.h"

#define GDIPAPI __declspec(dllexport)
#define GDIPCALL WINAPI

typedef float REAL;
typedef DWORD ARGB;
typedef INT PixelFormat;
typedef enum {
    Ok = 0, GenericError = 1, InvalidParameter = 2, OutOfMemory = 3, ObjectBusy = 4, InsufficientBuffer = 5,
    NotImplemented = 6, Win32Error = 7, WrongState = 8, Aborted = 9, FileNotFound = 10, ValueOverflow = 11,
    AccessDenied = 12, UnknownImageFormat = 13, FontFamilyNotFound = 14, FontStyleNotFound = 15,
    NotTrueTypeFont = 16, UnsupportedGdiplusVersion = 17, GdiplusNotInitialized = 18, PropertyNotFound = 19,
    PropertyNotSupported = 20,
} GpStatus;

#define PixelFormat1bppIndexed    0x00030101
#define PixelFormat4bppIndexed    0x00030402
#define PixelFormat8bppIndexed    0x00030803
#define PixelFormat16bppRGB555    0x00021005
#define PixelFormat16bppRGB565    0x00021006
#define PixelFormat24bppRGB       0x00021808
#define PixelFormat32bppRGB       0x00022009
#define PixelFormat32bppARGB      0x0026200A
#define PixelFormat32bppPARGB     0x000E200B

#define ImageLockModeRead         1
#define ImageLockModeWrite        2
#define ImageLockModeUserInputBuf 4

#define SmoothingModeHighQuality  2
#define SmoothingModeAntiAlias    4
#define PixelOffsetModeHighQuality 2
#define PixelOffsetModeHalf        4
#define CompositingModeSourceCopy  1
#define FillModeWinding            1
#define ImageTypeBitmap            1
#define UnitWorld                  0
#define UnitDisplay                1
#define UnitPixel                  2
#define UnitPoint                  3
#define UnitInch                   4
#define UnitDocument               5
#define UnitMillimeter             6
#define MatrixOrderAppend          1

/* path point types */
#define PathPointTypeStart        0
#define PathPointTypeLine         1
#define PathPointTypeBezier       3
#define PathPointTypePathTypeMask 7
#define PathPointTypePathMarker   0x20
#define PathPointTypeCloseSubpath 0x80

typedef struct { INT X, Y; } GpPoint;
typedef struct { REAL X, Y; } GpPointF;
typedef struct { INT X, Y, Width, Height; } GpRect;
typedef struct { REAL X, Y, Width, Height; } GpRectF;

/* ---- objects ---------------------------------------------------------- */

typedef struct GpImage {
    int type;                       /* ImageTypeBitmap */
    plutovg_surface_t *s;           /* premultiplied BGRA */
    PixelFormat fmt;                /* the format the program sees */
    GUID raw;                       /* the file format it came from */
    /* LockBits */
    BOOL locked;
    UINT lock_flags;
    GpRect lock_rect;
    PixelFormat lock_fmt;
    BYTE *lock_buf;                 /* ours, or the caller's (ImageLockModeUserInputBuf) */
    BOOL lock_own;
    INT lock_stride;
} GpImage, GpBitmap;

typedef struct GpPath {
    GpPointF *pts;
    BYTE *types;
    int n, cap;
    int fill_mode;
    BOOL new_figure;                /* the next point starts a figure */
} GpPath;

/* a region: everything, nothing, a rectangle or the inside of a path */
enum { RgnInfinite, RgnEmpty, RgnRect, RgnPath };
typedef struct GpRegion {
    int kind;
    GpRectF r;
    GpPath *path;
} GpRegion;

typedef struct GpGraphics {
    GpImage *img;                   /* drawing on a bitmap, or */
    HDC hdc;                        /* on a device context */
    HWND hwnd;                      /* (from GdipCreateFromHWND: released with the graphics) */
    plutovg_canvas_t *c;            /* the bitmap's canvas */
    int smoothing, pixel_offset, interpolation, compositing, compositing_quality, text_hint, page_unit;
    REAL page_scale;
    plutovg_matrix_t world;
    GpRegion clip;                  /* in world coordinates */
} GpGraphics;

typedef struct { int type; ARGB color; } GpBrush;               /* BrushTypeSolidColor */
typedef struct { ARGB color; REAL width; int line_join, start_cap, end_cap, dash; } GpPen;
typedef plutovg_matrix_t GpMatrix;

static inline void *xalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static inline void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static inline void set_argb(plutovg_canvas_t *c, ARGB a)
{
    plutovg_canvas_set_rgba(c, ((a >> 16) & 255) / 255.f, ((a >> 8) & 255) / 255.f, (a & 255) / 255.f, (a >> 24) / 255.f);
}

/* ---- shared between the files ----------------------------------------- */

/* gdiplus.c */
HBITMAP gdip_dib_section(int w, int h, DWORD **bits);

/* graphics.c: one drawing operation on a graphics object (see there) */
typedef struct {
    GpGraphics *g;
    plutovg_canvas_t *c;
    plutovg_surface_t *s;
    HDC mem;
    HBITMAP bmp, old;
    RECT box;
} GdipOp;
BOOL gdip_op_begin(GdipOp *op, GpGraphics *g, const GpRectF *area, BOOL stroke);
void gdip_op_end(GdipOp *op);
REAL gdip_unit_to_pixels(int unit, REAL v);

/* path.c */
plutovg_path_t *gdip_path_build(const GpPath *p);        /* NULL: out of memory */
GpRectF gdip_path_bounds(const GpPath *p);
void gdip_region_reset(GpRegion *r);
GpStatus gdip_region_copy(GpRegion *dst, const GpRegion *src);

/* the exported calls the files use of each other */
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromScan0(INT w, INT h, INT stride, PixelFormat f, BYTE *scan0, GpBitmap **out);
GDIPAPI GpStatus GDIPCALL GdipGetImageGraphicsContext(GpImage *i, GpGraphics **out);
GDIPAPI GpStatus GDIPCALL GdipDeleteGraphics(GpGraphics *g);
GDIPAPI GpStatus GDIPCALL GdipDrawImageRect(GpGraphics *g, GpImage *i, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipCreatePath(INT fill_mode, GpPath **out);
GDIPAPI GpStatus GDIPCALL GdipDeletePath(GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipClonePath(GpPath *p, GpPath **out);
GDIPAPI GpStatus GDIPCALL GdipAddPathEllipse(GpPath *p, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipAddPathArc(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
GDIPAPI GpStatus GDIPCALL GdipAddPathLine2I(GpPath *p, const GpPoint *pts, INT n);
GDIPAPI GpStatus GDIPCALL GdipAddPathRectangle(GpPath *p, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipClosePathFigure(GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p);
