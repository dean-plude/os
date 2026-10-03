/*
 * gdip.h — gdiplus.dll's types and objects, shared by its files:
 *   gdiplus.c   startup, bitmaps (load, save, lock, convert), encoders
 *   graphics.c  graphics objects, brushes, pens, drawing, matrices, regions
 *   path.c      paths (GDI+'s point and type arrays) and path iterators
 *   text.c      fonts, font families, string formats, drawing and measuring text
 *   more.c      curves, pies, image parallelograms, gradient/hatch/texture brushes,
 *               pen and brush properties, region data, custom caps, metafile stubs
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

/* brushes: BrushTypeSolidColor 0, HatchFill 1, TextureFill 2, PathGradient 3, LinearGradient 4 */
#define BrushTypeSolidColor     0
#define BrushTypeHatchFill      1
#define BrushTypeTextureFill    2
#define BrushTypePathGradient   3
#define BrushTypeLinearGradient 4
#define MAX_BLEND 16
typedef struct {
    int type;
    ARGB color;                     /* solid; hatch foreground; gradient start; path gradient centre */
    ARGB color2;                    /* hatch background; gradient end */
    int hatch;                      /* HatchStyle */
    GpRectF rect;                   /* linear gradient: the rectangle; path gradient: bounds */
    REAL angle;                     /* linear gradient's angle (degrees) */
    BOOL angle_scalable;
    int wrap;                       /* WrapMode: 0 tile, 1 flip x, 2 flip y, 3 flip xy, 4 clamp */
    BOOL gamma;
    plutovg_matrix_t xform;         /* the brush's own transform */
    /* blends: positions 0..1 with factors (blend) or colours (preset) */
    int nblend; REAL blend_f[MAX_BLEND], blend_p[MAX_BLEND];
    int npreset; ARGB preset_c[MAX_BLEND]; REAL preset_p[MAX_BLEND];
    /* path gradient */
    GpPointF centre, focus;
    GpPointF *pts; int npts;
    ARGB *surround; int nsurround;
    /* texture */
    GpImage *img;                   /* a copy of the image's surface, ours */
} GpBrush;
typedef struct {
    ARGB color; REAL width; int line_join, start_cap, end_cap, dash;
    int dash_cap, mode, unit, align;
    REAL miter, dash_offset;
    REAL *dashes; int ndashes;
    REAL *compound; int ncompound;
    plutovg_matrix_t xform;
    GpBrush *brush;                 /* the brush it was made from (owned), else NULL */
    void *custom_start, *custom_end;
} GpPen;
typedef plutovg_matrix_t GpMatrix;

typedef struct {
    GpPath *p;              /* a copy, as GDI+ takes */
    int sub;                /* the next subpath's first point */
    int marker;             /* the next marker section's first point */
} GpPathIterator;

static inline void *xalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static inline void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static inline void set_argb(plutovg_canvas_t *c, ARGB a)
{
    plutovg_canvas_set_rgba(c, ((a >> 16) & 255) / 255.f, ((a >> 8) & 255) / 255.f, (a & 255) / 255.f, (a >> 24) / 255.f);
}

/* ---- shared between the files ----------------------------------------- */

/* gdiplus.c */
HBITMAP gdip_dib_section(int w, int h, DWORD **bits);
GDIPAPI GpStatus GDIPCALL GdipCreateBitmapFromGdiDib(const BITMAPINFO *bi, void *bits, GpBitmap **out);
GDIPAPI GpStatus GDIPCALL GdipCloneBitmapArea(REAL x, REAL y, REAL w, REAL h, PixelFormat f, GpBitmap *src, GpBitmap **out);

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

/* graphics.c */
void gdip_use_brush(plutovg_canvas_t *c, const GpBrush *b);     /* the paint a brush fills with */
void gdip_use_pen(plutovg_canvas_t *c, const GpPen *p);
GpRectF gdip_region_bounds(const GpRegion *r);
GpStatus gdip_brush_free(GpBrush *b);                            /* frees what a brush owns (not the brush) */
GpStatus gdip_brush_copy(GpBrush *dst, const GpBrush *src);
GDIPAPI GpStatus GDIPCALL GdipSaveGraphics(GpGraphics *g, UINT *state);
GDIPAPI GpStatus GDIPCALL GdipRestoreGraphics(GpGraphics *g, UINT state);
GDIPAPI GpStatus GDIPCALL GdipCreateSolidFill(ARGB color, GpBrush **out);
GDIPAPI GpStatus GDIPCALL GdipDrawLines(GpGraphics *g, GpPen *pen, const GpPointF *p, INT n);
GDIPAPI GpStatus GDIPCALL GdipDrawImageRectRect(GpGraphics *g, GpImage *i, REAL dx, REAL dy, REAL dw, REAL dh,
                                                REAL sx, REAL sy, REAL sw, REAL sh, INT unit, const void *attrs, void *cb, void *cbdata);
GDIPAPI GpStatus GDIPCALL GdipGetImageWidth(GpImage *i, UINT *w);
GDIPAPI GpStatus GDIPCALL GdipGetImageHeight(GpImage *i, UINT *h);
GDIPAPI GpStatus GDIPCALL GdipCloneImage(GpImage *i, GpImage **out);
GDIPAPI GpStatus GDIPCALL GdipDisposeImage(GpImage *i);
GDIPAPI GpStatus GDIPCALL GdipCreateRegionRect(const GpRectF *r, GpRegion **out);
GDIPAPI GpStatus GDIPCALL GdipCreateRegionPath(GpPath *p, GpRegion **out);
GDIPAPI GpStatus GDIPCALL GdipCreateRegion(GpRegion **out);
GDIPAPI GpStatus GDIPCALL GdipTransformPath(GpPath *p, GpMatrix *m);
GDIPAPI GpStatus GDIPCALL GdipAddPathBeziers(GpPath *p, const GpPointF *pts, INT n);
GDIPAPI GpStatus GDIPCALL GdipAddPathLine2(GpPath *p, const GpPointF *pts, INT n);
GDIPAPI GpStatus GDIPCALL GdipAddPathPie(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
GDIPAPI GpStatus GDIPCALL GdipFillRectangle(GpGraphics *g, GpBrush *b, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipFillRegion(GpGraphics *g, GpBrush *b, GpRegion *r);
GDIPAPI GpStatus GDIPCALL GdipCreateMatrix2(REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy, GpMatrix **out);
GDIPAPI GpStatus GDIPCALL GdipCloneRegion(GpRegion *r, GpRegion **out);
GDIPAPI GpStatus GDIPCALL GdipIsVisibleRegionPoint(GpRegion *r, REAL x, REAL y, GpGraphics *g, BOOL *out);

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
GDIPAPI GpStatus GDIPCALL GdipCreatePath2(const GpPointF *pts, const BYTE *types, INT n, INT fill_mode, GpPath **out);
GDIPAPI GpStatus GDIPCALL GdipDeletePath(GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipClonePath(GpPath *p, GpPath **out);
GDIPAPI GpStatus GDIPCALL GdipAddPathEllipse(GpPath *p, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipAddPathArc(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
GDIPAPI GpStatus GDIPCALL GdipAddPathLine2I(GpPath *p, const GpPoint *pts, INT n);
GDIPAPI GpStatus GDIPCALL GdipAddPathRectangle(GpPath *p, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipClosePathFigure(GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipStartPathFigure(GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipGetPathLastPoint(GpPath *p, GpPointF *out);
GDIPAPI GpStatus GDIPCALL GdipAddPathPolygon(GpPath *p, const GpPointF *pts, INT n);
GDIPAPI GpStatus GDIPCALL GdipAddPathRectangles(GpPath *p, const GpRectF *r, INT n);
GDIPAPI GpStatus GDIPCALL GdipAddPathPath(GpPath *p, const GpPath *add_p, BOOL connect);
GDIPAPI GpStatus GDIPCALL GdipGetPathWorldBounds(GpPath *p, GpRectF *r, const GpMatrix *m, const GpPen *pen);
GDIPAPI GpStatus GDIPCALL GdipFlattenPath(GpPath *p, GpMatrix *m, REAL flatness);
GDIPAPI GpStatus GDIPCALL GdipCreateMatrix(GpMatrix **out);
GDIPAPI GpStatus GDIPCALL GdipDeleteMatrix(GpMatrix *m);
GDIPAPI GpStatus GDIPCALL GdipMultiplyMatrix(GpMatrix *m, const GpMatrix *t, INT order);
GDIPAPI GpStatus GDIPCALL GdipTranslateMatrix(GpMatrix *m, REAL dx, REAL dy, INT order);
GDIPAPI GpStatus GDIPCALL GdipScaleMatrix(GpMatrix *m, REAL sx, REAL sy, INT order);
GDIPAPI GpStatus GDIPCALL GdipRotateMatrix(GpMatrix *m, REAL deg, INT order);
GDIPAPI GpStatus GDIPCALL GdipTransformMatrixPoints(GpMatrix *m, GpPointF *p, INT n);
GDIPAPI GpStatus GDIPCALL GdipVectorTransformMatrixPoints(GpMatrix *m, GpPointF *p, INT n);
GDIPAPI GpStatus GDIPCALL GdipDrawLinesI(GpGraphics *g, GpPen *pen, const GpPoint *p, INT n);
GDIPAPI GpStatus GDIPCALL GdipDrawRectangle(GpGraphics *g, GpPen *pen, REAL x, REAL y, REAL w, REAL h);
GDIPAPI GpStatus GDIPCALL GdipDrawRectangleI(GpGraphics *g, GpPen *pen, INT x, INT y, INT w, INT h);
GDIPAPI GpStatus GDIPCALL GdipFillRectangleI(GpGraphics *g, GpBrush *b, INT x, INT y, INT w, INT h);
GDIPAPI GpStatus GDIPCALL GdipFillPolygonI(GpGraphics *g, GpBrush *b, const GpPoint *pts, INT n, INT mode);
GDIPAPI GpStatus GDIPCALL GdipDrawImage(GpGraphics *g, GpImage *i, REAL x, REAL y);
GDIPAPI GpStatus GDIPCALL GdipMultiplyWorldTransform(GpGraphics *g, const GpMatrix *m, INT order);
GDIPAPI GpStatus GDIPCALL GdipSetWorldTransform(GpGraphics *g, const GpMatrix *m);
GDIPAPI GpStatus GDIPCALL GdipTranslateWorldTransform(GpGraphics *g, REAL dx, REAL dy, INT order);
GDIPAPI GpStatus GDIPCALL GdipScaleWorldTransform(GpGraphics *g, REAL sx, REAL sy, INT order);
GDIPAPI GpStatus GDIPCALL GdipResetWorldTransform(GpGraphics *g);
GDIPAPI GpStatus GDIPCALL GdipSetPageUnit(GpGraphics *g, INT unit);
GDIPAPI GpStatus GDIPCALL GdipSetPageScale(GpGraphics *g, REAL s);
GDIPAPI GpStatus GDIPCALL GdipDeleteRegion(GpRegion *r);
GDIPAPI GpStatus GDIPCALL GdipCombineRegionRegion(GpRegion *r, GpRegion *src, INT mode);
GDIPAPI GpStatus GDIPCALL GdipGetRegionBounds(GpRegion *r, GpGraphics *g, GpRectF *out);
GDIPAPI GpStatus GDIPCALL GdipIsEmptyRegion(GpRegion *r, GpGraphics *g, BOOL *out);
GDIPAPI GpStatus GDIPCALL GdipIsInfiniteRegion(GpRegion *r, GpGraphics *g, BOOL *out);
GDIPAPI GpStatus GDIPCALL GdipGetVisibleClipBounds(GpGraphics *g, GpRectF *out);
GDIPAPI GpStatus GDIPCALL GdipGetPenColor(GpPen *p, ARGB *c);
GDIPAPI GpStatus GDIPCALL GdipCloneBrush(GpBrush *b, GpBrush **out);
GDIPAPI GpStatus GDIPCALL GdipDeleteBrush(GpBrush *b);
GDIPAPI GpStatus GDIPCALL GdipGetImagePixelFormat(GpImage *i, PixelFormat *f);
GDIPAPI GpStatus GDIPCALL GdipCreatePen1(ARGB color, REAL width, INT unit, GpPen **out);
GDIPAPI GpStatus GDIPCALL GdipClonePen(GpPen *p, GpPen **out);
GDIPAPI GpStatus GDIPCALL GdipDeletePen(GpPen *p);
GDIPAPI GpStatus GDIPCALL GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p);
GDIPAPI GpStatus GDIPCALL GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p);
