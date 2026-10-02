/*
 * dwrite_int.h — NovaOS DirectWrite: shared types and internal API
 *
 * The COM objects are plain C structs whose first member is a vtable of
 * function pointers in the documented slot order.  Fonts are parsed and
 * rasterized with stb_truetype (font.c); dwrite.c holds the factory,
 * files, collections and fonts; face.c the font faces, glyph run outlines
 * and glyph run analysis.
 */
#pragma once
#include <objbase.h>
#include <string.h>

#define DWAPI __declspec(dllexport)

/* ---- results ---- */
#define DWRITE_E_FILEFORMAT        ((HRESULT)0x88985000L)
#define DWRITE_E_UNEXPECTED        ((HRESULT)0x88985001L)
#define DWRITE_E_NOFONT            ((HRESULT)0x88985002L)
#define DWRITE_E_FILENOTFOUND      ((HRESULT)0x88985003L)
#define DWRITE_E_ALREADYREGISTERED ((HRESULT)0x88985006L)
#define E_NOT_SUFFICIENT_BUFFER_   ((HRESULT)0x8007007AL)

/* ---- interface ids ---- */
DEFINE_GUID(IID_IDWriteFactory,           0xb859ee5a, 0xd838, 0x4b5b, 0xa2,0xe8, 0x1a,0xdc,0x7d,0x93,0xdb,0x48);
DEFINE_GUID(IID_IDWriteFactory1,          0x30572f99, 0xdac6, 0x41db, 0xa1,0x6e, 0x04,0x86,0x30,0x7e,0x60,0x6a);
DEFINE_GUID(IID_IDWriteFontFileStream,    0x6d4865fe, 0x0ab8, 0x4d91, 0x8f,0x62, 0x5d,0xd6,0xbe,0x34,0xa3,0xe0);
DEFINE_GUID(IID_IDWriteFontFileLoader,    0x727cad4e, 0xd6af, 0x4c9e, 0x8a,0x08, 0xd6,0x95,0xb1,0x1c,0xaa,0x49);
DEFINE_GUID(IID_IDWriteLocalFontFileLoader, 0xb2d9f3ec, 0xc9fe, 0x4a11, 0xa2,0xec, 0xd8,0x62,0x08,0xf7,0xc0,0xa2);
DEFINE_GUID(IID_IDWriteFontFile,          0x739d886a, 0xcef5, 0x47dc, 0x87,0x69, 0x1a,0x8b,0x41,0xbe,0xbb,0xb0);
DEFINE_GUID(IID_IDWriteLocalizedStrings,  0x08256209, 0x099a, 0x4b34, 0xb8,0x6d, 0xc2,0x2b,0x11,0x0e,0x77,0x71);
DEFINE_GUID(IID_IDWriteRenderingParams,   0x2f0da53a, 0x2add, 0x47cd, 0x82,0xee, 0xd9,0xec,0x34,0x68,0x8e,0x75);
DEFINE_GUID(IID_IDWriteRenderingParams1,  0x94413cf4, 0xa6fc, 0x4248, 0x8b,0x50, 0x66,0x74,0x34,0x8f,0xca,0xd3);
DEFINE_GUID(IID_IDWriteFontFace,          0x5f49804d, 0x7024, 0x4d43, 0xbf,0xa9, 0xd2,0x59,0x84,0xf5,0x38,0x49);
DEFINE_GUID(IID_IDWriteFontFace1,         0xa71efdb4, 0x9fdb, 0x4838, 0xad,0x90, 0xcf,0xc3,0xbe,0x8c,0x3d,0xaf);
DEFINE_GUID(IID_IDWriteFont,              0xacd16696, 0x8c14, 0x4f5d, 0x87,0x7e, 0xfe,0x3f,0xc1,0xd3,0x27,0x37);
DEFINE_GUID(IID_IDWriteFont1,             0xacd16696, 0x8c14, 0x4f5d, 0x87,0x7e, 0xfe,0x3f,0xc1,0xd3,0x27,0x38);
DEFINE_GUID(IID_IDWriteFontList,          0x1a0d8438, 0x1d97, 0x4ec1, 0xae,0xf9, 0xa2,0xfb,0x86,0xed,0x6a,0xcb);
DEFINE_GUID(IID_IDWriteFontFamily,        0xda20d8ef, 0x812a, 0x4c43, 0x98,0x02, 0x62,0xec,0x4a,0xbd,0x7a,0xdd);
DEFINE_GUID(IID_IDWriteFontCollection,    0xa84cee02, 0x3eea, 0x4eee, 0xa8,0x27, 0x87,0xc1,0xa0,0x2a,0x0f,0xcc);
DEFINE_GUID(IID_IDWriteGdiInterop,        0x1edd9491, 0x9853, 0x4299, 0x89,0x8f, 0x64,0x32,0x98,0x3b,0x6f,0x3a);
DEFINE_GUID(IID_IDWriteGlyphRunAnalysis,  0x7d97dbf7, 0xe085, 0x42d4, 0x81,0xe3, 0x6a,0x88,0x3b,0xde,0xd1,0x18);

/* ---- enumerations (all passed as 32-bit values) ---- */
enum { FACE_CFF = 0, FACE_TRUETYPE = 1, FACE_COLLECTION = 2, FACE_UNKNOWN = 6 };
enum { FILE_UNKNOWN = 0, FILE_CFF = 1, FILE_TRUETYPE = 2, FILE_COLLECTION = 3 };
enum { SIM_BOLD = 1, SIM_OBLIQUE = 2 };
enum { STYLE_NORMAL = 0, STYLE_OBLIQUE = 1, STYLE_ITALIC = 2 };
enum { RMODE_DEFAULT = 0, RMODE_ALIASED = 1, RMODE_GDI_CLASSIC = 2, RMODE_GDI_NATURAL = 3,
       RMODE_NATURAL = 4, RMODE_NATURAL_SYMMETRIC = 5, RMODE_OUTLINE = 6 };
enum { TEX_ALIASED_1x1 = 0, TEX_CLEARTYPE_3x1 = 1 };

/* ---- structures ---- */
typedef struct {
    UINT16 designUnitsPerEm, ascent, descent;
    INT16  lineGap;
    UINT16 capHeight, xHeight;
    INT16  underlinePosition;
    UINT16 underlineThickness;
    INT16  strikethroughPosition;
    UINT16 strikethroughThickness;
} DW_FONT_METRICS;

typedef struct {
    DW_FONT_METRICS m;
    INT16 glyphBoxLeft, glyphBoxTop, glyphBoxRight, glyphBoxBottom;
    INT16 subscriptPositionX, subscriptPositionY, subscriptSizeX, subscriptSizeY;
    INT16 superscriptPositionX, superscriptPositionY, superscriptSizeX, superscriptSizeY;
    BOOL  hasTypographicMetrics;
} DW_FONT_METRICS1;

typedef struct {
    INT32 leftSideBearing; UINT32 advanceWidth; INT32 rightSideBearing;
    INT32 topSideBearing;  UINT32 advanceHeight; INT32 bottomSideBearing;
    INT32 verticalOriginY;
} DW_GLYPH_METRICS;

typedef struct { float advanceOffset, ascenderOffset; } DW_GLYPH_OFFSET;
typedef struct { float m11, m12, m21, m22, dx, dy; } DW_MATRIX;
typedef struct { INT16 slopeRise, slopeRun, offset; } DW_CARET_METRICS;
typedef struct { UINT32 first, last; } DW_UNICODE_RANGE;

typedef struct {
    void                  *fontFace;      /* IDWriteFontFace */
    float                  fontEmSize;
    UINT32                 glyphCount;
    const UINT16          *glyphIndices;
    const float           *glyphAdvances;
    const DW_GLYPH_OFFSET *glyphOffsets;
    BOOL                   isSideways;
    UINT32                 bidiLevel;
} DW_GLYPH_RUN;

typedef struct { float x, y; } D2_POINT;
typedef struct { D2_POINT p1, p2, p3; } D2_BEZIER;

/* ---- calling methods of objects that programs implement ---- */
#define VT(obj)               (*(void ***)(obj))
#define COM_ADDREF(o)         ((ULONG (STDMETHODCALLTYPE *)(void *))VT(o)[1])(o)
#define COM_RELEASE(o)        ((ULONG (STDMETHODCALLTYPE *)(void *))VT(o)[2])(o)
#define COM_QI(o, iid, out)   ((HRESULT (STDMETHODCALLTYPE *)(void *, REFIID, void **))VT(o)[0])(o, iid, out)

/* ---- fonts (font.c) ---- */
typedef struct FontData FontData;   /* the bytes of one font file */
typedef struct FaceData FaceData;   /* one face in a font file */

struct FontData {
    LONG           ref;
    BYTE          *bytes;
    UINT32         size;
    UINT32         file_type;      /* FILE_* */
    UINT32         face_type;      /* FACE_* of its faces */
    UINT32         num_faces;
    FaceData     **faces;          /* created on first use */
    SRWLOCK        lock;
};

struct FaceData {
    FontData      *file;
    UINT32         index;
    UINT32         offset;         /* of the face's table directory */
    void          *info;           /* stbtt_fontinfo */
    UINT16         upem, num_glyphs;
    DW_FONT_METRICS1 metrics;
    DW_CARET_METRICS caret;
    UINT16         weight, stretch, style;
    BOOL           mono, symbol, has_kerning;
    BYTE           panose[10];
    INT16          hhea_ascent, hhea_descent;
};

FontData *font_data_load_path(const WCHAR *path);
FontData *font_data_from_bytes(BYTE *bytes, UINT32 size);   /* takes ownership */
void      font_data_addref(FontData *d);
void      font_data_release(FontData *d);
FaceData *font_face_data(FontData *d, UINT32 index);         /* NULL if not a face */

const BYTE *sfnt_table(const FaceData *f, const char tag[4], UINT32 *size);
/* one name from the name table (platform 3, English preferred), NUL-terminated; 0 if absent */
int  sfnt_name(const FaceData *f, int id, WCHAR *out, int cap);
UINT16 face_glyph_index(const FaceData *f, UINT32 cp);
void   face_glyph_metrics(const FaceData *f, UINT16 glyph, DW_GLYPH_METRICS *m);
int    face_kern(const FaceData *f, UINT16 a, UINT16 b);
UINT32 face_unicode_ranges(const FaceData *f, DW_UNICODE_RANGE *out, UINT32 cap);

/* A glyph outline: moves, lines, quadratic and cubic curves in font units (y up) */
typedef struct { short x, y, cx, cy, cx1, cy1; unsigned char type, pad; } GlyphVertex;
enum { GV_MOVE = 1, GV_LINE = 2, GV_QUAD = 3, GV_CUBIC = 4 };
int  face_glyph_shape(const FaceData *f, UINT16 glyph, GlyphVertex **verts);
void face_free_shape(GlyphVertex *verts);

/* Rasterize one glyph: font units → device pixels through m (x' = x*m11 + y*m21 + dx,
 * y' = x*m12 + y*m22 + dy, y down).  Coverage is added (saturating) into the
 * w*h buffer whose top-left pixel is (ox, oy). */
void face_raster_glyph(const FaceData *f, UINT16 glyph, const DW_MATRIX *m,
                       BYTE *buf, int w, int h, int ox, int oy);
/* Device bounds of the same glyph (empty → r[0] >= r[2]) */
void face_glyph_bounds(const FaceData *f, UINT16 glyph, const DW_MATRIX *m, int r[4]);

/* ---- helpers (dwrite.c) ---- */
void *dw_alloc(SIZE_T n);
void *dw_zalloc(SIZE_T n);
void  dw_free(void *p);
void  dw_log(const char *fmt, ...);
WCHAR *dw_wcsdup(const WCHAR *s);
int    dw_wcsieq(const WCHAR *a, const WCHAR *b);
double dw_floor(double x);
double dw_ceil(double x);

/* ---- COM objects shared between dwrite.c and face.c ---- */
typedef struct FontFile {
    const void *const *vtbl;
    LONG        ref;
    void       *loader;            /* IDWriteFontFileLoader (ours for local files) */
    BYTE       *key;
    UINT32      key_size;
    FontData   *data;              /* loaded on first use */
    SRWLOCK     lock;
} FontFile;

typedef struct FontFace {
    const void *const *vtbl;
    LONG        ref;
    FontFile   *file;
    FaceData   *face;
    UINT32      index, sims;
} FontFace;

HRESULT font_file_data(FontFile *f, FontData **out);
HRESULT font_face_create(FontFile *file, UINT32 index, UINT32 sims, FontFace **out);
FontFace *font_face_from(void *iface);   /* NULL unless one of ours */
/* layout.c */
HRESULT text_format_create(const WCHAR *family, void *coll, UINT32 weight, UINT32 style, UINT32 stretch, float size,
                           const WCHAR *locale, void **out);
HRESULT text_layout_create(const WCHAR *s, UINT32 n, void *fmt, float w, float h, void **out);
HRESULT glyph_run_analysis_create(const DW_GLYPH_RUN *run, float ppd, const DW_MATRIX *m, UINT32 mode,
                                  float ox, float oy, void **out);
