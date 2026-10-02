/*
 * d2d_int.h — NovaOS Direct2D: shared types and internal API
 *
 * Direct2D's COM objects are plain C structs whose first member is a
 * vtable in the documented slot order.  Drawing is done in software:
 * geometry is kept as figures of lines and cubic Béziers (geometry.c),
 * flattened, stroked (stroke.c) and filled by an exact-area anti-aliasing
 * scanline rasterizer (raster.c), and painted with solid, gradient and bitmap
 * brushes (brush.c) into a 32-bit premultiplied BGRA surface that render
 * targets (target.c) hand to GDI.  Text goes through DirectWrite's
 * interfaces: glyph outlines from IDWriteFontFace, layouts drawn through
 * IDWriteTextLayout::Draw with our IDWriteTextRenderer (text.c).
 *
 * Methods that return a structure take a hidden pointer after `this` and
 * return it, as MSVC's C++ member functions do.
 */
#pragma once
#include <objbase.h>
#include <string.h>
#include <math.h>

#define D2D __declspec(dllexport)

/* ---- results ---- */
#define D2DERR_WRONG_STATE          ((HRESULT)0x88990001L)
#define D2DERR_NOT_INITIALIZED      ((HRESULT)0x88990002L)
#define D2DERR_UNSUPPORTED_OPERATION ((HRESULT)0x88990003L)
#define D2DERR_BAD_NUMBER           ((HRESULT)0x88990011L)
#define D2DERR_UNSUPPORTED_PIXEL_FORMAT ((HRESULT)0x88982F80L)
#define D2DERR_PUSH_POP_UNBALANCED  ((HRESULT)0x88990016L)

/* ---- interface ids ---- */
#define D2D_GUID(n, l) DEFINE_GUID(n, l, 0x12e2, 0x11dc, 0x9f, 0xed, 0x00, 0x11, 0x43, 0xa0, 0x55, 0xf9)
D2D_GUID(IID_ID2D1Resource, 0x2cd90691);
D2D_GUID(IID_ID2D1RenderTarget, 0x2cd90694);
D2D_GUID(IID_ID2D1BitmapRenderTarget, 0x2cd90695);
D2D_GUID(IID_ID2D1HwndRenderTarget, 0x2cd90698);
D2D_GUID(IID_ID2D1Layer, 0x2cd9069b);
D2D_GUID(IID_ID2D1StrokeStyle, 0x2cd9069d);
D2D_GUID(IID_ID2D1SimplifiedGeometrySink, 0x2cd9069e);
D2D_GUID(IID_ID2D1GeometrySink, 0x2cd9069f);
D2D_GUID(IID_ID2D1Geometry, 0x2cd906a1);
D2D_GUID(IID_ID2D1RectangleGeometry, 0x2cd906a2);
D2D_GUID(IID_ID2D1RoundedRectangleGeometry, 0x2cd906a3);
D2D_GUID(IID_ID2D1EllipseGeometry, 0x2cd906a4);
D2D_GUID(IID_ID2D1PathGeometry, 0x2cd906a5);
D2D_GUID(IID_ID2D1GeometryGroup, 0x2cd906a6);
D2D_GUID(IID_ID2D1GradientStopCollection, 0x2cd906a7);
D2D_GUID(IID_ID2D1Brush, 0x2cd906a8);
D2D_GUID(IID_ID2D1SolidColorBrush, 0x2cd906a9);
D2D_GUID(IID_ID2D1BitmapBrush, 0x2cd906aa);
D2D_GUID(IID_ID2D1LinearGradientBrush, 0x2cd906ab);
D2D_GUID(IID_ID2D1RadialGradientBrush, 0x2cd906ac);
D2D_GUID(IID_ID2D1TransformedGeometry, 0x2cd906bb);
D2D_GUID(IID_ID2D1TessellationSink, 0x2cd906c1);
D2D_GUID(IID_ID2D1Mesh, 0x2cd906c2);
DEFINE_GUID(IID_ID2D1Factory, 0x06152247, 0x6f50, 0x465a, 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07);
DEFINE_GUID(IID_ID2D1DCRenderTarget, 0x1c51bc64, 0xde61, 0x46fd, 0x98, 0x99, 0x63, 0xa5, 0xd8, 0xf0, 0x39, 0x50);
DEFINE_GUID(IID_ID2D1Bitmap, 0xa2296057, 0xea42, 0x4099, 0x98, 0x3b, 0x53, 0x9f, 0xb6, 0x50, 0x54, 0x26);
DEFINE_GUID(IID_ID2D1Image, 0x65019f75, 0x8da2, 0x497c, 0xb3, 0x2c, 0xdf, 0xa3, 0x4e, 0x48, 0xed, 0xe6);
DEFINE_GUID(IID_ID2D1DrawingStateBlock, 0x28506e39, 0xebf6, 0x46a1, 0xbb, 0x47, 0xfd, 0x85, 0x56, 0x5a, 0xb9, 0x57);
DEFINE_GUID(IID_ID2D1GdiInteropRenderTarget, 0xe0db51c3, 0x6f77, 0x4bae, 0xb3, 0xd5, 0xe4, 0x75, 0x09, 0xb3, 0x58, 0x38);
DEFINE_GUID(IID_IDWriteFactory_, 0xb859ee5a, 0xd838, 0x4b5b, 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48);
DEFINE_GUID(IID_IDWritePixelSnapping_, 0xeaf3a2da, 0xecf4, 0x4d24, 0xb6, 0x44, 0xb3, 0x4f, 0x68, 0x42, 0x02, 0x4b);
DEFINE_GUID(IID_IDWriteTextRenderer_, 0xef8a8135, 0x5cc6, 0x45fe, 0x88, 0x25, 0xc5, 0xa0, 0x72, 0x4e, 0xb8, 0x19);

/* ---- structures ---- */
typedef struct { float x, y; } PT;                          /* D2D1_POINT_2F */
typedef struct { UINT32 x, y; } PTU;
typedef struct { float width, height; } SZ;                 /* D2D1_SIZE_F */
typedef struct { UINT32 width, height; } SZU;
typedef struct { float left, top, right, bottom; } RCF;     /* D2D1_RECT_F */
typedef struct { UINT32 left, top, right, bottom; } RCU;
typedef struct { float r, g, b, a; } COLORF;
typedef struct { float m11, m12, m21, m22, dx, dy; } MAT;   /* D2D1_MATRIX_3X2_F */
typedef struct { UINT32 format, alphaMode; } PIXFMT;
typedef struct { PT point; float radiusX, radiusY; } ELLIPSE_;
typedef struct { RCF rect; float radiusX, radiusY; } RRECT;
typedef struct { PT point1, point2, point3; } BEZIER;
typedef struct { PT point1, point2; } QBEZIER;
typedef struct { PT point; SZ size; float rotationAngle; UINT32 sweepDirection, arcSize; } ARCSEG;
typedef struct { float position; COLORF color; } GSTOP;
typedef struct { PIXFMT pixelFormat; float dpiX, dpiY; } BMPPROPS;
typedef struct { UINT32 extendModeX, extendModeY, interpolationMode; } BMPBRUSHPROPS;
typedef struct { float opacity; MAT transform; } BRUSHPROPS;
typedef struct { PT startPoint, endPoint; } LINPROPS;
typedef struct { PT center, gradientOriginOffset; float radiusX, radiusY; } RADPROPS;
typedef struct { UINT32 type; PIXFMT pixelFormat; float dpiX, dpiY; UINT32 usage, minLevel; } RTPROPS;
typedef struct { HWND hwnd; SZU pixelSize; UINT32 presentOptions; } HWNDRTPROPS;
typedef struct { UINT32 startCap, endCap, dashCap, lineJoin; float miterLimit; UINT32 dashStyle; float dashOffset; } STROKEPROPS;
typedef struct { UINT32 antialiasMode, textAntialiasMode; UINT64 tag1, tag2; MAT transform; } STATEDESC;
typedef struct { RCF contentBounds; void *geometricMask; UINT32 maskAntialiasMode; MAT maskTransform; float opacity;
                 void *opacityBrush; UINT32 layerOptions; } LAYERPARAMS;
typedef struct { PT point1, point2, point3; } TRIANGLE;

/* DirectWrite pieces */
typedef struct { float advanceOffset, ascenderOffset; } DW_OFFSET;
typedef struct { void *fontFace; float fontEmSize; UINT32 glyphCount; const UINT16 *glyphIndices;
                 const float *glyphAdvances; const DW_OFFSET *glyphOffsets; BOOL isSideways; UINT32 bidiLevel; } DW_GLYPHRUN;
typedef struct { float width, thickness, offset, runHeight; UINT32 readingDirection, flowDirection;
                 const WCHAR *localeName; UINT32 measuringMode; } DW_UNDERLINE;
typedef struct { float width, thickness, offset; UINT32 readingDirection, flowDirection;
                 const WCHAR *localeName; UINT32 measuringMode; } DW_STRIKETHROUGH;

enum { CAP_FLAT, CAP_SQUARE, CAP_ROUND, CAP_TRIANGLE };
enum { JOIN_MITER, JOIN_BEVEL, JOIN_ROUND, JOIN_MITER_OR_BEVEL };
enum { DASH_SOLID, DASH_DASH, DASH_DOT, DASH_DASH_DOT, DASH_DASH_DOT_DOT, DASH_CUSTOM };
enum { EXTEND_CLAMP, EXTEND_WRAP, EXTEND_MIRROR };
enum { FILL_ALTERNATE, FILL_WINDING };

/* ---- memory and COM helpers ---- */
void *d_alloc(size_t n);            /* zeroed */
void *d_realloc(void *p, size_t n);
void d_free(void *p);
#define COM_ADDREF(p)  ((*(ULONG (STDMETHODCALLTYPE **)(void *))(*(void ***)(p) + 1))(p))
#define COM_RELEASE(p) do { if (p) (*(ULONG (STDMETHODCALLTYPE **)(void *))(*(void ***)(p) + 2))(p); } while (0)
#define COM_QI(p, iid, out) ((*(HRESULT (STDMETHODCALLTYPE **)(void *, REFIID, void **))(*(void ***)(p)))(p, iid, out))
/* the slot @i of @p's vtable, as a function of type @T */
#define VSLOT(p, i, T) ((T)(*(void ***)(p))[i])

/* ---- matrices ---- */
extern const MAT MAT_IDENTITY;
MAT mat_mul(const MAT *a, const MAT *b);         /* a then b */
PT mat_apply(const MAT *m, PT p);
BOOL mat_invert(const MAT *m, MAT *out);
int mat_is_identity(const MAT *m);

/* ---- path data: figures of lines and cubics ---- */
enum { SEG_LINE, SEG_CUBIC };
typedef struct {
    PT start;
    int n;                          /* segments */
    int cap;
    unsigned char *kind;            /* SEG_* per segment */
    PT *pts;                        /* 3 per segment (a line uses the last) */
    int closed, filled;
} Figure;
typedef struct {
    int n, cap;
    Figure *f;
    int fill_mode;
} Path;
void path_init(Path *p);
void path_free(Path *p);
Figure *path_begin(Path *p, PT start, int filled);
void path_line(Path *p, PT to);
void path_cubic(Path *p, PT c1, PT c2, PT to);
void path_quad(Path *p, PT c, PT to);
void path_arc(Path *p, const ARCSEG *a);
void path_end(Path *p, int closed);
void path_copy(Path *dst, const Path *src, const MAT *m);   /* appends @src transformed */
void path_rect(Path *p, const RCF *r);
void path_rrect(Path *p, const RRECT *r);
void path_ellipse(Path *p, const ELLIPSE_ *e);
/* a polyline approximation of every figure (device tolerance @tol) */
typedef struct { int n, cap; PT *pt; int nfig, figcap; int *fig_end; unsigned char *closed, *filled; } Poly;
void path_flatten(const Path *p, const MAT *m, float tol, Poly *out);
void poly_free(Poly *p);
/* exact bounds of @p under @m; FALSE when empty */
BOOL path_bounds(const Path *p, const MAT *m, RCF *r);
/* winding test of a point against the filled figures */
BOOL poly_contains(const Poly *p, int fill_mode, PT pt);
/* emit @p (under @m) into an ID2D1SimplifiedGeometrySink; lines only when @flat */
void path_emit(const Path *p, const MAT *m, int flat, float tol, void *sink);
/* boolean combination of two filled polygon sets, by horizontal slabs */
enum { OP_UNION, OP_INTERSECT, OP_XOR, OP_EXCLUDE, OP_ONLY_A };
typedef void (*trap_fn)(void *ctx, float ya, float yb, float la, float lb, float ra, float rb);
void slabs(const Poly *a, int fill_a, const Poly *b, int fill_b, int op, trap_fn fn, void *ctx);
/* the outline of a slab decomposition, as closed figures in @out */
void slabs_outline(const Poly *a, int fill_a, const Poly *b, int fill_b, int op, Path *out);
float mat_scale(const MAT *m);

/* ---- geometry objects ---- */
typedef struct Geometry Geometry;
struct Geometry {
    const void *vtbl;
    LONG ref;
    void *factory;
    int kind;                       /* G_* */
    Path path;                      /* the figures (in the geometry's own space) */
    int sealed;                     /* a path geometry, after Close */
    /* the descriptions the Get* methods return */
    RCF rect; RRECT rrect; ELLIPSE_ ell;
    Geometry *source; MAT transform;                /* transformed */
    Geometry **children; UINT32 nchildren;          /* group */
    int nsegments;
};
enum { G_RECT, G_RRECT, G_ELLIPSE, G_PATH, G_TRANSFORMED, G_GROUP };
Geometry *geometry_of(void *iface);
HRESULT geometry_create(void *factory, int kind, Geometry **out);
HRESULT geometry_path_sink(Geometry *g, void **sink);
/* @g's figures (with transforms of nested geometries applied) */
const Path *geometry_path(Geometry *g);
void *geometry_sink_new(Path *path);      /* an ID2D1GeometrySink writing into @path (owned by the caller) */

/* ---- stroke styles ---- */
typedef struct {
    const void *vtbl;
    LONG ref;
    void *factory;
    STROKEPROPS p;
    float *dashes;
    UINT32 ndashes;
} StrokeStyle;
StrokeStyle *stroke_style_of(void *iface);
HRESULT stroke_style_create(void *factory, const STROKEPROPS *p, const float *dashes, UINT32 n, void **out);

/* ---- surfaces and paint ---- */
typedef struct {
    UINT32 *px;                     /* premultiplied BGRA */
    int w, h, stride;               /* stride in pixels */
} Surface;

enum { PAINT_SOLID, PAINT_LINEAR, PAINT_RADIAL, PAINT_BITMAP };
typedef struct Paint {
    int kind;
    float opacity;
    UINT32 color;                   /* solid: premultiplied */
    MAT inv;                        /* device -> brush space */
    PT p0, p1;                      /* linear: start, end; radial: centre, origin offset */
    float rx, ry;
    const UINT32 *lut;              /* gradients: 256 premultiplied colours */
    int extend, extend_y;
    const Surface *bmp;             /* bitmap */
    int linear_filter;
    const struct Paint *alpha;      /* an opacity mask: its alpha multiplies coverage */
} Paint;

typedef struct {
    int x0, y0, x1, y1;             /* device clip, half-open */
} Clip;

/* a coverage mask (layers' geometric masks) */
typedef struct { unsigned char *a; int x0, y0, w, h; } Mask;

/* premultiplied colours of @n pixels of row @y from @x */
void paint_span(const Paint *p, int x, int y, int n, UINT32 *out);
/* fill @path (transformed by @m) with @paint */
void raster_fill(Surface *s, const Clip *c, const Mask *mask, const Path *path, const MAT *m, int aliased, const Paint *paint);
/* stroke @path (in its own space, widened there, then transformed by @m) */
void raster_stroke(Surface *s, const Clip *c, const Mask *mask, const Path *path, const MAT *m, float width,
                   const StrokeStyle *st, int aliased, const Paint *paint);
/* the outline of @path's stroke, as figures */
void stroke_to_path(const Path *path, float width, const StrokeStyle *st, float tol, Path *out);
/* coverage of @path into @mask (allocated to @c) */
void raster_mask(Mask *mask, const Clip *c, const Path *path, const MAT *m, int aliased);

/* ---- brushes and bitmaps ---- */
typedef struct {
    const void *vtbl;
    LONG ref;
    void *factory;
    GSTOP *stops;
    UINT32 n;
    UINT32 gamma, extend;
    UINT32 lut[256];
} GradientStops;

typedef struct {
    const void *vtbl;
    LONG ref;
    void *factory;
    Surface s;
    PIXFMT fmt;
    float dpix, dpiy;
    struct Target *owner_rt;        /* a bitmap render target's bitmap */
} Bitmap;
Bitmap *bitmap_of(void *iface);
HRESULT bitmap_create(void *factory, SZU size, const void *src, UINT32 pitch, const BMPPROPS *props, Bitmap **out);

typedef struct {
    const void *vtbl;
    LONG ref;
    void *factory;
    int kind;                       /* PAINT_* */
    float opacity;
    MAT transform;
    COLORF color;
    PT p0, p1;
    float rx, ry;
    GradientStops *stops;
    Bitmap *bitmap;
    UINT32 extend_x, extend_y, interp;
} Brush;
Brush *brush_of(void *iface);
HRESULT brush_create(void *factory, int kind, const BRUSHPROPS *bp, Brush **out);
void brush_set_stops(Brush *b, void *stops);
HRESULT stops_create(void *factory, const GSTOP *stops, UINT32 n, UINT32 gamma, UINT32 extend, void **out);
/* the paint for @b drawn under @world (device from user space) */
int brush_paint(Brush *b, const MAT *world, Paint *p);
UINT32 premul(const COLORF *c, float opacity);

/* ---- the factory ---- */
typedef struct {
    const void *vtbl;
    LONG ref;
    UINT32 type;
} Factory;
void factory_addref(void *f);
void factory_release(void *f);
void state_block_get(void *iface, STATEDESC *d, void **params);
void state_block_set(void *iface, const STATEDESC *d, void *params);

/* ---- render targets ---- */
#define MAX_CLIPS 64
#define MAX_LAYERS 32
typedef struct Layer {
    Surface saved;                  /* the target's pixels under the layer */
    Clip region;                    /* the device rectangle it covers */
    Mask mask;                      /* a geometric mask, or none (a == NULL) */
    float opacity;
    Brush *opbrush;                 /* an opacity brush, or NULL */
    Paint opaint;
    int clip_depth;                 /* the clip stack's depth before the layer */
    void *layer;                    /* the ID2D1Layer, held while pushed */
} LayerState;

typedef struct Target {
    const void *vtbl;
    const void *gdi_vtbl;           /* ID2D1GdiInteropRenderTarget */
    LONG ref;
    void *factory;
    int kind;                       /* RT_* */
    Surface s;
    HBITMAP dib;
    HDC mdc;
    HGDIOBJ old_bm;
    HWND hwnd;
    HDC bound_dc;
    RECT bound_rc;
    PIXFMT fmt;
    float dpix, dpiy;
    MAT transform;
    UINT32 aa, text_aa;
    UINT64 tag1, tag2;
    int drawing;
    HRESULT error;
    Clip clips[MAX_CLIPS];
    int nclips;
    LayerState layers[MAX_LAYERS];
    int nlayers;
    void *text_params;
    Bitmap *bitmap;                 /* a bitmap render target's */
} Target;
enum { TGT_HWND, TGT_DC, TGT_BITMAP };
Target *target_of(void *iface);
HRESULT target_create(void *factory, int kind, const RTPROPS *props, Target **out);
HRESULT target_resize(Target *t, int w, int h);
/* the target's current clip, and its world transform (DPI included) */
Clip target_clip(Target *t);
MAT target_world(Target *t);
/* fill or stroke @path (in user space) with @brush */
void target_fill(Target *t, const Path *path, void *brush, void *opacity_brush);
void target_stroke(Target *t, const Path *path, void *brush, float width, void *style);

/* ---- text ---- */
void text_draw_glyph_run(Target *t, PT origin, const DW_GLYPHRUN *run, void *brush);
void text_draw_layout(Target *t, PT origin, void *layout, void *brush, UINT32 options);
void text_draw_string(Target *t, const WCHAR *s, UINT32 n, void *format, const RCF *rc, void *brush, UINT32 options,
                      UINT32 measuring);

