/*
 * d2dtest — checks NovaOS's Direct2D.  Built with MinGW-w64 against its
 * own d2d1.h, so it calls d2d1.dll the way any Windows program does.
 *
 *     d2dtest [SECONDS] [REFERENCE.bmp]
 *
 * Geometry computations are checked against known answers; then a scene
 * of fills, strokes, gradients, a path with arcs and dashes, a bitmap and
 * a transform is drawn into a DC render target and compared with
 * REFERENCE.bmp (default d2dref.bmp next to the program), which
 * tools/d2dtest/reference.py draws with Skia.  The drawing is saved as
 * d2dout.bmp next to the program, and drawn once more in a window through
 * an HWND render target, which stays up for SECONDS (default 0).  Layers
 * and text (when DirectWrite can lay text out) are checked on their own
 * targets.
 *
 * Prints "d2dtest: N passed, M failed".
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 400
#define H 300

static int passed, failed;

/* ID2D1Geometry's methods, called through the vtable: MinGW's C macros for
   several of them drop an argument */
#define GEO(g) ((ID2D1Geometry *)(g))
#define GCALL(g, m, ...) GEO(g)->lpVtbl->m(GEO(g), __VA_ARGS__)
/* the same for ID2D1RenderTarget */
#define RCALL(t, m, ...) (t)->lpVtbl->m(t, __VA_ARGS__)

static void check(int ok, const char *what)
{
    if (ok) passed++;
    else failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static int close_to(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static D2D1_COLOR_F rgba(float r, float g, float b, float a)
{
    D2D1_COLOR_F c = { r, g, b, a };
    return c;
}

static D2D1_POINT_2F pt(float x, float y)
{
    D2D1_POINT_2F p = { x, y };
    return p;
}

static D2D1_RECT_F rc(float l, float t, float r, float b)
{
    D2D1_RECT_F x = { l, t, r, b };
    return x;
}

static ID2D1Factory *factory;

/* ---- geometry ---- */
static ID2D1PathGeometry *combined(ID2D1Geometry *a, ID2D1Geometry *b, D2D1_COMBINE_MODE mode)
{
    ID2D1PathGeometry *p = NULL;
    ID2D1GeometrySink *s = NULL;
    if (FAILED(ID2D1Factory_CreatePathGeometry(factory, &p))) return NULL;
    ID2D1PathGeometry_Open(p, &s);
    GCALL(a, CombineWithGeometry, b, mode, NULL, 0.25f, (ID2D1SimplifiedGeometrySink *)s);
    ID2D1GeometrySink_Close(s);
    ID2D1GeometrySink_Release(s);
    return p;
}

typedef struct {
    ID2D1TessellationSinkVtbl *lpVtbl;
    double area;
} AreaSink;

static HRESULT STDMETHODCALLTYPE as_qi(IUnknown *s, REFIID r, void **o) { (void)s; (void)r; *o = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE as_ref(IUnknown *s) { (void)s; return 1; }
static void STDMETHODCALLTYPE as_add(ID2D1TessellationSink *s, const D2D1_TRIANGLE *t, UINT32 n)
{
    AreaSink *a = (AreaSink *)s;
    for (UINT32 i = 0; i < n; i++)
        a->area += fabs((t[i].point2.x - t[i].point1.x) * (t[i].point3.y - t[i].point1.y) -
                        (t[i].point3.x - t[i].point1.x) * (t[i].point2.y - t[i].point1.y)) / 2;
}
static HRESULT STDMETHODCALLTYPE as_close(ID2D1TessellationSink *s) { (void)s; return S_OK; }
static ID2D1TessellationSinkVtbl as_vtbl = { { as_qi, as_ref, as_ref }, as_add, as_close };

static void geometry_tests(void)
{
    ID2D1EllipseGeometry *e = NULL;
    D2D1_ELLIPSE el = { { 0, 0 }, 10, 5 };
    check(SUCCEEDED(ID2D1Factory_CreateEllipseGeometry(factory, &el, &e)), "CreateEllipseGeometry");
    if (!e) return;
    D2D1_RECT_F b;
    GCALL(e, GetBounds, NULL, &b);
    check(close_to(b.left, -10, 0.01f) && close_to(b.top, -5, 0.01f) && close_to(b.right, 10, 0.01f) && close_to(b.bottom, 5, 0.01f),
          "ellipse bounds");
    BOOL in = FALSE, out = TRUE;
    GCALL(e, FillContainsPoint, pt(0, 0), NULL, 0.25f, &in);
    GCALL(e, FillContainsPoint, pt(9.5f, 4.5f), NULL, 0.25f, &out);
    check(in && !out, "ellipse FillContainsPoint");
    float area = 0;
    GCALL(e, ComputeArea, NULL, 0.01f, &area);
    check(close_to(area, 3.14159265f * 50, 0.5f), "ellipse ComputeArea");

    ID2D1RectangleGeometry *r1 = NULL, *r2 = NULL, *r3 = NULL;
    D2D1_RECT_F a = rc(0, 0, 10, 10), c = rc(5, 5, 15, 15), d = rc(2, 2, 4, 4);
    ID2D1Factory_CreateRectangleGeometry(factory, &a, &r1);
    ID2D1Factory_CreateRectangleGeometry(factory, &c, &r2);
    ID2D1Factory_CreateRectangleGeometry(factory, &d, &r3);
    float len = 0;
    GCALL(r1, ComputeLength, NULL, 0.25f, &len);
    check(close_to(len, 40, 0.01f), "rectangle ComputeLength");
    D2D1_POINT_2F at, tan;
    GCALL(r1, ComputePointAtLength, 15, NULL, 0.25f, &at, &tan);
    check(close_to(at.x, 10, 0.01f) && close_to(at.y, 5, 0.01f) && close_to(tan.y, 1, 0.01f), "ComputePointAtLength");

    ID2D1PathGeometry *u = combined((ID2D1Geometry *)r1, (ID2D1Geometry *)r2, D2D1_COMBINE_MODE_UNION);
    ID2D1PathGeometry *x = combined((ID2D1Geometry *)r1, (ID2D1Geometry *)r2, D2D1_COMBINE_MODE_INTERSECT);
    ID2D1PathGeometry *ex = combined((ID2D1Geometry *)r1, (ID2D1Geometry *)r2, D2D1_COMBINE_MODE_EXCLUDE);
    float au = 0, ax = 0, ae = 0;
    if (u) GCALL(u, ComputeArea, NULL, 0.25f, &au);
    if (x) GCALL(x, ComputeArea, NULL, 0.25f, &ax);
    if (ex) GCALL(ex, ComputeArea, NULL, 0.25f, &ae);
    check(close_to(au, 175, 0.1f) && close_to(ax, 25, 0.1f) && close_to(ae, 75, 0.1f), "CombineWithGeometry areas");
    UINT32 figs = 0, segs = 0;
    if (u) {
        ID2D1PathGeometry_GetFigureCount(u, &figs);
        ID2D1PathGeometry_GetSegmentCount(u, &segs);
    }
    /* 8 corners: 7 segments and the figure's closing line, or 8 segments */
    printf("     union outline: %u figure(s), %u segments\n", figs, segs);
    check(figs == 1 && (segs == 7 || segs == 8), "union outline is one figure with 8 corners");
    if (u) ID2D1PathGeometry_Release(u);
    if (x) ID2D1PathGeometry_Release(x);
    if (ex) ID2D1PathGeometry_Release(ex);

    D2D1_GEOMETRY_RELATION rel = 0, rel2 = 0, rel3 = 0;
    GCALL(r1, CompareWithGeometry, (ID2D1Geometry *)r3, NULL, 0.25f, &rel);
    GCALL(r3, CompareWithGeometry, (ID2D1Geometry *)r1, NULL, 0.25f, &rel2);
    GCALL(r1, CompareWithGeometry, (ID2D1Geometry *)r2, NULL, 0.25f, &rel3);
    check(rel == D2D1_GEOMETRY_RELATION_CONTAINS && rel2 == D2D1_GEOMETRY_RELATION_IS_CONTAINED &&
          rel3 == D2D1_GEOMETRY_RELATION_OVERLAP, "CompareWithGeometry");

    BOOL on = FALSE, off = TRUE;
    GCALL(r1, StrokeContainsPoint, pt(0, 5), 2, NULL, NULL, 0.25f, &on);
    GCALL(r1, StrokeContainsPoint, pt(5, 5), 2, NULL, NULL, 0.25f, &off);
    check(on && !off, "StrokeContainsPoint");
    GCALL(r1, GetWidenedBounds, 2, NULL, NULL, 0.25f, &b);
    check(close_to(b.left, -1, 0.01f) && close_to(b.top, -1, 0.01f) && close_to(b.right, 11, 0.01f) && close_to(b.bottom, 11, 0.01f),
          "GetWidenedBounds");

    AreaSink as = { &as_vtbl, 0 };
    GCALL(e, Tessellate, NULL, 0.01f, (ID2D1TessellationSink *)&as);
    check(fabs(as.area - 3.14159265 * 50) < 0.5, "Tessellate covers the ellipse");

    D2D1_MATRIX_3X2_F m;
    D2D1MakeRotateMatrix(90, pt(0, 0), &m);
    ID2D1TransformedGeometry *t = NULL;
    ID2D1Factory_CreateTransformedGeometry(factory, (ID2D1Geometry *)e, &m, &t);
    if (t) {
        GCALL(t, GetBounds, NULL, &b);
        ID2D1TransformedGeometry_Release(t);
    }
    check(t && close_to(b.left, -5, 0.01f) && close_to(b.top, -10, 0.01f), "transformed geometry bounds");

    ID2D1RectangleGeometry_Release(r1);
    ID2D1RectangleGeometry_Release(r2);
    ID2D1RectangleGeometry_Release(r3);
    ID2D1EllipseGeometry_Release(e);
}

/* ---- the scene (keep in step with reference.py) ---- */
static ID2D1SolidColorBrush *solid(ID2D1RenderTarget *rt, D2D1_COLOR_F c)
{
    ID2D1SolidColorBrush *b = NULL;
    RCALL(rt, CreateSolidColorBrush, &c, NULL, &b);
    return b;
}

static ID2D1PathGeometry *star(float cx, float cy, float ro, float ri)
{
    ID2D1PathGeometry *p = NULL;
    ID2D1GeometrySink *s = NULL;
    ID2D1Factory_CreatePathGeometry(factory, &p);
    ID2D1PathGeometry_Open(p, &s);
    for (int i = 0; i < 10; i++) {
        float r = i % 2 ? ri : ro, a = -3.14159265f / 2 + i * 3.14159265f / 5;
        D2D1_POINT_2F q = pt(cx + r * cosf(a), cy + r * sinf(a));
        if (!i) ID2D1GeometrySink_BeginFigure(s, q, D2D1_FIGURE_BEGIN_FILLED);
        else ID2D1GeometrySink_AddLine(s, q);
    }
    ID2D1GeometrySink_EndFigure(s, D2D1_FIGURE_END_CLOSED);
    ID2D1GeometrySink_Close(s);
    ID2D1GeometrySink_Release(s);
    return p;
}

static void arc(ID2D1GeometrySink *s, float x, float y, float r)
{
    D2D1_ARC_SEGMENT a = { { x, y }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL };
    ID2D1GeometrySink_AddArc(s, &a);
}

static ID2D1PathGeometry *rounded(float l, float t, float r, float b, float rad)
{
    ID2D1PathGeometry *p = NULL;
    ID2D1GeometrySink *s = NULL;
    ID2D1Factory_CreatePathGeometry(factory, &p);
    ID2D1PathGeometry_Open(p, &s);
    ID2D1GeometrySink_BeginFigure(s, pt(l + rad, t), D2D1_FIGURE_BEGIN_HOLLOW);
    ID2D1GeometrySink_AddLine(s, pt(r - rad, t));
    arc(s, r, t + rad, rad);
    ID2D1GeometrySink_AddLine(s, pt(r, b - rad));
    arc(s, r - rad, b, rad);
    ID2D1GeometrySink_AddLine(s, pt(l + rad, b));
    arc(s, l, b - rad, rad);
    ID2D1GeometrySink_AddLine(s, pt(l, t + rad));
    arc(s, l + rad, t, rad);
    ID2D1GeometrySink_EndFigure(s, D2D1_FIGURE_END_CLOSED);
    ID2D1GeometrySink_Close(s);
    ID2D1GeometrySink_Release(s);
    return p;
}

static void scene(ID2D1RenderTarget *rt)
{
    D2D1_COLOR_F white = rgba(1, 1, 1, 1);
    RCALL(rt, Clear, &white);

    ID2D1SolidColorBrush *red = solid(rt, rgba(0.9f, 0.1f, 0.1f, 1)), *blue = solid(rt, rgba(0.1f, 0.2f, 0.9f, 1));
    ID2D1SolidColorBrush *green = solid(rt, rgba(0.1f, 0.7f, 0.2f, 1)), *black = solid(rt, rgba(0, 0, 0, 1));
    ID2D1SolidColorBrush *shade = solid(rt, rgba(0, 0, 0, 0.5f)), *orange = solid(rt, rgba(1, 0.6f, 0, 1));
    ID2D1SolidColorBrush *gray = solid(rt, rgba(0.25f, 0.25f, 0.25f, 1)), *teal = solid(rt, rgba(0, 0.5f, 0.5f, 1));
    D2D1_RECT_F r;

    r = rc(20, 20, 120, 80);
    RCALL(rt, FillRectangle, &r, (ID2D1Brush *)red);
    r = rc(140, 20, 240, 80);
    RCALL(rt, DrawRectangle, &r, (ID2D1Brush *)blue, 4, NULL);
    D2D1_ELLIPSE el = { { 320, 50 }, 60, 30 };
    RCALL(rt, FillEllipse, &el, (ID2D1Brush *)green);

    D2D1_GRADIENT_STOP ls[2] = { { 0, { 1, 0xDD / 255.0f, 0, 1 } }, { 1, { 0, 0x33 / 255.0f, 0xCC / 255.0f, 1 } } };
    ID2D1GradientStopCollection *lsc = NULL;
    RCALL(rt, CreateGradientStopCollection, ls, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &lsc);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = { { 20, 0 }, { 200, 0 } };
    ID2D1LinearGradientBrush *lin = NULL;
    RCALL(rt, CreateLinearGradientBrush, &lp, NULL, lsc, &lin);
    r = rc(20, 100, 200, 160);
    if (lin) RCALL(rt, FillRectangle, &r, (ID2D1Brush *)lin);

    D2D1_GRADIENT_STOP rs[2] = { { 0, { 1, 1, 1, 1 } }, { 1, { 0x80 / 255.0f, 0x20 / 255.0f, 0xA0 / 255.0f, 1 } } };
    ID2D1GradientStopCollection *rsc = NULL;
    RCALL(rt, CreateGradientStopCollection, rs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &rsc);
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp = { { 300, 130 }, { 0, 0 }, 70, 40 };
    ID2D1RadialGradientBrush *rad = NULL;
    RCALL(rt, CreateRadialGradientBrush, &rp, NULL, rsc, &rad);
    D2D1_ELLIPSE e2 = { { 300, 130 }, 70, 40 };
    if (rad) RCALL(rt, FillEllipse, &e2, (ID2D1Brush *)rad);

    D2D1_MATRIX_3X2_F rot, id = { 1, 0, 0, 1, 0, 0 };
    D2D1MakeRotateMatrix(30, pt(110, 130), &rot);
    RCALL(rt, SetTransform, &rot);
    r = rc(80, 120, 140, 140);
    RCALL(rt, FillRectangle, &r, (ID2D1Brush *)shade);
    RCALL(rt, SetTransform, &id);

    ID2D1PathGeometry *st = star(90, 235, 50, 20);
    D2D1_STROKE_STYLE_PROPERTIES rj = { D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0 };
    ID2D1StrokeStyle *round_join = NULL;
    ID2D1Factory_CreateStrokeStyle(factory, &rj, NULL, 0, &round_join);
    RCALL(rt, FillGeometry, (ID2D1Geometry *)st, (ID2D1Brush *)orange, NULL);
    RCALL(rt, DrawGeometry, (ID2D1Geometry *)st, (ID2D1Brush *)black, 3, round_join);

    ID2D1PathGeometry *rr = rounded(200, 185, 300, 280, 15);
    D2D1_STROKE_STYLE_PROPERTIES ds = { D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH, 0 };
    ID2D1StrokeStyle *dash = NULL;
    ID2D1Factory_CreateStrokeStyle(factory, &ds, NULL, 0, &dash);
    RCALL(rt, DrawGeometry, (ID2D1Geometry *)rr, (ID2D1Brush *)gray, 5, dash);

    UINT32 px[16];
    for (int i = 0; i < 16; i++) px[i] = ((i % 4) + (i / 4)) % 2 ? 0xFF2060E0 : 0xFFF0C020;
    D2D1_SIZE_U sz = { 4, 4 };
    D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96 };
    ID2D1Bitmap *bm = NULL;
    RCALL(rt, CreateBitmap, sz, px, 16, &bp, &bm);
    r = rc(320, 190, 380, 250);
    if (bm) RCALL(rt, DrawBitmap, bm, &r, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL);

    D2D1_STROKE_STYLE_PROPERTIES rc_ = { D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_FLAT,
                                         D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_SOLID, 0 };
    ID2D1StrokeStyle *round_cap = NULL;
    ID2D1Factory_CreateStrokeStyle(factory, &rc_, NULL, 0, &round_cap);
    RCALL(rt, DrawLine, pt(20, 292), pt(380, 292), (ID2D1Brush *)teal, 6, round_cap);

    if (bm) ID2D1Bitmap_Release(bm);
    if (round_cap) ID2D1StrokeStyle_Release(round_cap);
    if (dash) ID2D1StrokeStyle_Release(dash);
    if (round_join) ID2D1StrokeStyle_Release(round_join);
    if (rr) ID2D1PathGeometry_Release(rr);
    if (st) ID2D1PathGeometry_Release(st);
    if (rad) ID2D1RadialGradientBrush_Release(rad);
    if (rsc) ID2D1GradientStopCollection_Release(rsc);
    if (lin) ID2D1LinearGradientBrush_Release(lin);
    if (lsc) ID2D1GradientStopCollection_Release(lsc);
    ID2D1SolidColorBrush *all[] = { red, blue, green, black, shade, orange, gray, teal };
    for (int i = 0; i < 8; i++) if (all[i]) ID2D1SolidColorBrush_Release(all[i]);
}

/* ---- a DC render target over a DIB ---- */
typedef struct { HDC dc; HBITMAP bm; HGDIOBJ old; BYTE *bits; int w, h; ID2D1DCRenderTarget *rt; } Canvas;

static int canvas(Canvas *c, int w, int h)
{
    memset(c, 0, sizeof(*c));
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    c->bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&c->bits, NULL, 0);
    c->dc = CreateCompatibleDC(NULL);
    if (!c->bm || !c->dc) return 0;
    c->old = SelectObject(c->dc, c->bm);
    c->w = w;
    c->h = h;
    D2D1_RENDER_TARGET_PROPERTIES p = { D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                        { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE }, 96, 96,
                                        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT };
    if (FAILED(ID2D1Factory_CreateDCRenderTarget(factory, &p, &c->rt))) return 0;
    RECT r = { 0, 0, w, h };
    return SUCCEEDED(ID2D1DCRenderTarget_BindDC(c->rt, c->dc, &r));
}

static void canvas_free(Canvas *c)
{
    if (c->rt) ID2D1DCRenderTarget_Release(c->rt);
    if (c->dc) { SelectObject(c->dc, c->old); DeleteDC(c->dc); }
    if (c->bm) DeleteObject(c->bm);
}

static const BYTE *pixel(const Canvas *c, int x, int y) { return c->bits + ((size_t)y * c->w + x) * 4; }

/* ---- BMP files ---- */
static void path_beside(char *out, size_t n, const char *name)
{
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    if (s) s[1] = 0;
    else out[0] = 0;
    strncat(out, name, n - strlen(out) - 1);
}

static void save_bmp(const char *path, const Canvas *c)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("     could not write %s\n", path);
        return;
    }
    int row = (c->w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + row * c->h), 0, 0, 54 };
    BITMAPINFOHEADER ih = { sizeof(ih), c->w, c->h, 1, 24, BI_RGB, (DWORD)(row * c->h) };
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    BYTE *line = calloc(1, row);
    for (int y = c->h - 1; y >= 0; y--) {
        for (int x = 0; x < c->w; x++) memcpy(line + x * 3, pixel(c, x, y), 3);
        fwrite(line, row, 1, f);
    }
    free(line);
    fclose(f);
    printf("     saved %s\n", path);
}

/* a 24-bit bottom-up BMP, as BGR rows top first */
static BYTE *load_bmp(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fread(&ih, sizeof(ih), 1, f) != 1 || fh.bfType != 0x4D42 ||
        ih.biBitCount != 24 || ih.biCompression != BI_RGB) { fclose(f); return NULL; }
    *w = ih.biWidth;
    *h = abs(ih.biHeight);
    int row = (*w * 3 + 3) & ~3;
    BYTE *px = malloc((size_t)*w * *h * 3), *line = malloc(row);
    fseek(f, fh.bfOffBits, SEEK_SET);
    for (int i = 0; i < *h; i++) {
        if (fread(line, row, 1, f) != 1) break;
        int y = ih.biHeight > 0 ? *h - 1 - i : i;
        memcpy(px + (size_t)y * *w * 3, line, *w * 3);
    }
    free(line);
    fclose(f);
    return px;
}

static void scene_test(const char *ref_path)
{
    Canvas c;
    int ok = canvas(&c, W, H);
    check(ok, "CreateDCRenderTarget and BindDC");
    if (!ok) { canvas_free(&c); return; }
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)c.rt;
    D2D1_SIZE_F size;
    D2D1_SIZE_U psize;
    /* Windows' C++ ABI returns structures from methods through a hidden
       pointer, which MinGW's C declarations leave out */
    ((D2D1_SIZE_F *(STDMETHODCALLTYPE *)(ID2D1RenderTarget *, D2D1_SIZE_F *))rt->lpVtbl->GetSize)(rt, &size);
    ((D2D1_SIZE_U *(STDMETHODCALLTYPE *)(ID2D1RenderTarget *, D2D1_SIZE_U *))rt->lpVtbl->GetPixelSize)(rt, &psize);
    check(close_to(size.width, W, 0.01f) && psize.height == H, "GetSize and GetPixelSize");
    rt->lpVtbl->BeginDraw(rt);
    scene(rt);
    HRESULT hr = RCALL(rt, EndDraw, NULL, NULL);
    check(SUCCEEDED(hr), "EndDraw");
    GdiFlush();
    char out[MAX_PATH];
    path_beside(out, sizeof(out), "d2dout.bmp");
    save_bmp(out, &c);

    int rw, rh;
    BYTE *ref = load_bmp(ref_path, &rw, &rh);
    check(ref && rw == W && rh == H, "reference image loads");
    if (ref && rw == W && rh == H) {
        long bad = 0;
        double sum = 0;
        int worst = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const BYTE *a = pixel(&c, x, y), *b = ref + ((size_t)y * W + x) * 3;
                int d = 0;
                for (int k = 0; k < 3; k++) {
                    int v = abs(a[k] - b[k]);
                    if (v > d) d = v;
                }
                sum += d;
                if (d > worst) worst = d;
                if (d > 64) bad++;
            }
        double mean = sum / (W * H);
        printf("     scene vs reference: mean difference %.2f, %ld of %d pixels differ by more than 64, worst %d\n",
               mean, bad, W * H, worst);
        check(mean < 2.0 && bad < W * H / 200, "scene matches the Skia reference");
    }
    free(ref);
    canvas_free(&c);
}

static void layer_test(void)
{
    Canvas c;
    if (!canvas(&c, 16, 16)) { check(0, "layer canvas"); canvas_free(&c); return; }
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)c.rt;
    ID2D1Layer *layer = NULL;
    RCALL(rt, CreateLayer, NULL, &layer);
    rt->lpVtbl->BeginDraw(rt);
    D2D1_COLOR_F white = rgba(1, 1, 1, 1);
    RCALL(rt, Clear, &white);
    D2D1_LAYER_PARAMETERS lp = { { -1e9f, -1e9f, 1e9f, 1e9f }, NULL, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                 { { { 1, 0, 0, 1, 0, 0 } } }, 0.5f, NULL, D2D1_LAYER_OPTIONS_NONE };
    RCALL(rt, PushLayer, &lp, layer);
    ID2D1SolidColorBrush *red = solid(rt, rgba(1, 0, 0, 1));
    D2D1_RECT_F r = rc(0, 0, 8, 16);
    RCALL(rt, FillRectangle, &r, (ID2D1Brush *)red);
    rt->lpVtbl->PopLayer(rt);
    D2D1_RECT_F clip = rc(8, 0, 12, 16);
    RCALL(rt, PushAxisAlignedClip, &clip, D2D1_ANTIALIAS_MODE_ALIASED);
    r = rc(0, 0, 16, 16);
    RCALL(rt, FillRectangle, &r, (ID2D1Brush *)red);
    rt->lpVtbl->PopAxisAlignedClip(rt);
    HRESULT hr = RCALL(rt, EndDraw, NULL, NULL);
    GdiFlush();
    const BYTE *a = pixel(&c, 4, 8), *b = pixel(&c, 10, 8), *d = pixel(&c, 14, 8);
    check(SUCCEEDED(hr) && a[2] == 255 && abs(a[1] - 128) <= 2 && abs(a[0] - 128) <= 2, "layer at half opacity");
    check(b[2] == 255 && b[1] == 0 && d[1] == 255, "axis-aligned clip");
    if (red) ID2D1SolidColorBrush_Release(red);
    if (layer) ID2D1Layer_Release(layer);
    canvas_free(&c);
}

/* the scene again in a window, through an HWND render target */
static void hwnd_test(int seconds)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d2dtest";
    RegisterClassA(&wc);
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("d2dtest", "d2dtest: Direct2D", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    check(hwnd != NULL, "window");
    if (!hwnd) return;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    D2D1_RENDER_TARGET_PROPERTIES p = { D2D1_RENDER_TARGET_TYPE_DEFAULT, { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN },
                                        96, 96, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT };
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = { hwnd, { W, H }, D2D1_PRESENT_OPTIONS_NONE };
    ID2D1HwndRenderTarget *ht = NULL;
    HRESULT hr = ID2D1Factory_CreateHwndRenderTarget(factory, &p, &hp, &ht);
    check(SUCCEEDED(hr) && ht, "CreateHwndRenderTarget");
    if (ht) {
        ID2D1RenderTarget *rt = (ID2D1RenderTarget *)ht;
        rt->lpVtbl->BeginDraw(rt);
        scene(rt);
        hr = RCALL(rt, EndDraw, NULL, NULL);
        HDC dc = GetDC(hwnd);
        COLORREF a = GetPixel(dc, 70, 50), b = GetPixel(dc, 5, 5);
        ReleaseDC(hwnd, dc);
        printf("     window pixels: %06lx (red rectangle), %06lx (background)\n", (unsigned long)a, (unsigned long)b);
        check(SUCCEEDED(hr) && GetRValue(a) > 200 && GetGValue(a) < 60 && b == RGB(255, 255, 255),
              "HWND render target draws into the window");
        printf("Direct2D scene on screen\n");
        fflush(stdout);
        DWORD end = GetTickCount() + seconds * 1000;
        while (GetTickCount() < end) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
            Sleep(50);
        }
        ID2D1HwndRenderTarget_Release(ht);
    }
    DestroyWindow(hwnd);
}

static const GUID dwrite_factory_iid = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

static void text_test(void)
{
    HMODULE dw = LoadLibraryA("dwrite.dll");
    typedef HRESULT (WINAPI *Create)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);
    Create create = dw ? (Create)GetProcAddress(dw, "DWriteCreateFactory") : NULL;
    IDWriteFactory *f = NULL;
    IDWriteTextFormat *fmt = NULL;
    if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, &dwrite_factory_iid, (IUnknown **)&f)) ||
        FAILED(IDWriteFactory_CreateTextFormat(f, L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 32, L"en-us",
                                               &fmt))) {
        printf("SKIP text: DirectWrite here cannot make a text format yet\n");
        if (f) IDWriteFactory_Release(f);
        return;
    }
    Canvas c;
    if (!canvas(&c, 200, 60)) { check(0, "text canvas"); canvas_free(&c); return; }
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)c.rt;
    ID2D1SolidColorBrush *blue = solid(rt, rgba(0, 0, 1, 1));
    rt->lpVtbl->BeginDraw(rt);
    D2D1_COLOR_F white = rgba(1, 1, 1, 1);
    RCALL(rt, Clear, &white);
    D2D1_RECT_F r = rc(10, 10, 190, 50);
    RCALL(rt, DrawText, L"NovaOS", 6, fmt, &r, (ID2D1Brush *)blue, D2D1_DRAW_TEXT_OPTIONS_NONE,
                               DWRITE_MEASURING_MODE_NATURAL);
    RCALL(rt, EndDraw, NULL, NULL);
    GdiFlush();
    int ink = 0, outside = 0;
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            const BYTE *p = pixel(&c, x, y);
            if (p[0] > 200 && p[2] < 100) {
                if (x >= 8 && x < 192 && y >= 8 && y < 52) ink++;
                else outside++;
            }
        }
    printf("     text: %d blue pixels inside the box, %d outside\n", ink, outside);
    check(ink > 200 && !outside, "DrawText draws DirectWrite text in the brush's colour");
    if (blue) ID2D1SolidColorBrush_Release(blue);
    canvas_free(&c);
    IDWriteTextFormat_Release(fmt);
    IDWriteFactory_Release(f);
}

int main(int argc, char **argv)
{
    char ref[MAX_PATH];
    int seconds = 0;
    path_beside(ref, sizeof(ref), "d2dref.bmp");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] >= '0' && argv[i][0] <= '9') seconds = atoi(argv[i]);
        else snprintf(ref, sizeof(ref), "%s", argv[i]);
    }
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&factory);
    check(SUCCEEDED(hr) && factory, "D2D1CreateFactory");
    if (factory) {
        geometry_tests();
        scene_test(ref);
        layer_test();
        hwnd_test(seconds);
        text_test();
        ID2D1Factory_Release(factory);
    }
    printf("d2dtest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
