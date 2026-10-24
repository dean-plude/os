/* NovaOS: the Windows SDK's d2d1_3.h (Direct2D 1.3), which MinGW-w64 lacks.
 * It holds what XAML's core headers and components call: Direct2D 1.1
 * (core/inc/Timeline.h includes d2d1_3.h for it), the Direct2D 1.2 device,
 * ID2D1Device2 (with ID2D1Device1 under it), whose DXGI device XAML's
 * composition helpers take from a D2D device (components/comptree/
 * DCompTreeHost.cpp), and the device contexts ID2D1DeviceContext1-7, which
 * the platform layer's render target hands out and the native text's
 * drawing context draws colour and plain glyph runs with (core/native/
 * text/Common/D2DTextDrawingContext.cpp).  Each is declared in its vtable
 * order with the Windows SDK's IIDs; the interfaces their methods take
 * and give (ink, gradient meshes, sprite batches, SVG glyph styles and
 * documents) are named only, for the callers that join later.  No
 * using-declarations: under Microsoft's C++ ABI they would move a class's
 * overloads of those names first in its vtable. */
#pragma once
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dxgicommon.h>
#include <dwrite.h>

#ifdef __cplusplus
interface ID2D1GeometryRealization;
interface ID2D1Ink;
interface ID2D1InkStyle;
interface ID2D1GradientMesh;
interface ID2D1ImageSource;
interface ID2D1ImageSourceFromWic;
interface ID2D1TransformedImageSource;
interface ID2D1LookupTable3D;
interface ID2D1SpriteBatch;
interface ID2D1SvgGlyphStyle;
interface ID2D1SvgDocument;
interface ID2D1ColorContext1;
struct D2D1_INK_POINT;
struct D2D1_INK_STYLE_PROPERTIES;
struct D2D1_GRADIENT_MESH_PATCH;
struct D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES;
struct D2D1_SIMPLE_COLOR_PROFILE;
typedef enum D2D1_IMAGE_SOURCE_LOADING_OPTIONS { D2D1_IMAGE_SOURCE_LOADING_OPTIONS_NONE = 0,
    D2D1_IMAGE_SOURCE_LOADING_OPTIONS_RELEASE_SOURCE = 1, D2D1_IMAGE_SOURCE_LOADING_OPTIONS_CACHE_ON_DEMAND = 2,
    D2D1_IMAGE_SOURCE_LOADING_OPTIONS_FORCE_DWORD = 0xffffffff } D2D1_IMAGE_SOURCE_LOADING_OPTIONS;
typedef enum D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS { D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS_NONE = 0,
    D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS_LOW_QUALITY_PRIMARY_CONVERSION = 1,
    D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS_FORCE_DWORD = 0xffffffff } D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS;
typedef enum D2D1_SPRITE_OPTIONS { D2D1_SPRITE_OPTIONS_NONE = 0, D2D1_SPRITE_OPTIONS_CLAMP_TO_SOURCE_RECTANGLE = 1,
                                   D2D1_SPRITE_OPTIONS_FORCE_DWORD = 0xffffffff } D2D1_SPRITE_OPTIONS;
typedef enum D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION { D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT = 0,
    D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DISABLE = 1,
    D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_FORCE_DWORD = 0xffffffff } D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION;
/* (DirectWrite's paint feature level, which the Windows SDK's dwrite_3.h
 * declares and ID2D1DeviceContext7 answers) */
#ifndef NOVA_DWRITE_PAINT_FEATURE_LEVEL
#define NOVA_DWRITE_PAINT_FEATURE_LEVEL
enum DWRITE_PAINT_FEATURE_LEVEL : INT32 { DWRITE_PAINT_FEATURE_LEVEL_NONE = 0, DWRITE_PAINT_FEATURE_LEVEL_COLR_V0 = 1,
                                          DWRITE_PAINT_FEATURE_LEVEL_COLR_V1 = 2 };
#endif

struct __declspec(uuid("d37f57e4-6908-459f-a199-e72f24f79987")) __declspec(novtable) ID2D1DeviceContext1
    : public ID2D1DeviceContext
{
    virtual HRESULT STDMETHODCALLTYPE CreateFilledGeometryRealization(ID2D1Geometry *geometry, FLOAT flatteningTolerance,
                                                                      ID2D1GeometryRealization **geometryRealization) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStrokedGeometryRealization(ID2D1Geometry *geometry, FLOAT flatteningTolerance,
                                                                       FLOAT strokeWidth, ID2D1StrokeStyle *strokeStyle,
                                                                       ID2D1GeometryRealization **geometryRealization) = 0;
    virtual void STDMETHODCALLTYPE DrawGeometryRealization(ID2D1GeometryRealization *geometryRealization,
                                                           ID2D1Brush *brush) = 0;
};
struct __declspec(uuid("394ea6a3-0c34-4321-950b-6ca20f0be6c7")) __declspec(novtable) ID2D1DeviceContext2
    : public ID2D1DeviceContext1
{
    virtual HRESULT STDMETHODCALLTYPE CreateInk(const D2D1_INK_POINT *startPoint, ID2D1Ink **ink) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInkStyle(const D2D1_INK_STYLE_PROPERTIES *inkStyleProperties,
                                                     ID2D1InkStyle **inkStyle) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateGradientMesh(const D2D1_GRADIENT_MESH_PATCH *patches, UINT32 patchesCount,
                                                         ID2D1GradientMesh **gradientMesh) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateImageSourceFromWic(IWICBitmapSource *wicBitmapSource,
                                                               D2D1_IMAGE_SOURCE_LOADING_OPTIONS loadingOptions,
                                                               D2D1_ALPHA_MODE alphaMode,
                                                               ID2D1ImageSourceFromWic **imageSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateLookupTable3D(D2D1_BUFFER_PRECISION precision, const UINT32 *extents,
                                                          const BYTE *data, UINT32 dataCount, const UINT32 *strides,
                                                          ID2D1LookupTable3D **lookupTable) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateImageSourceFromDxgi(IDXGISurface **surfaces, UINT32 surfaceCount,
                                                                DXGI_COLOR_SPACE_TYPE colorSpace,
                                                                D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS options,
                                                                ID2D1ImageSource **imageSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGradientMeshWorldBounds(ID2D1GradientMesh *gradientMesh,
                                                                 D2D1_RECT_F *pBounds) const = 0;
    virtual void STDMETHODCALLTYPE DrawInk(ID2D1Ink *ink, ID2D1Brush *brush, ID2D1InkStyle *inkStyle) = 0;
    virtual void STDMETHODCALLTYPE DrawGradientMesh(ID2D1GradientMesh *gradientMesh) = 0;
    virtual void STDMETHODCALLTYPE DrawGdiMetafile(ID2D1GdiMetafile *gdiMetafile, const D2D1_RECT_F *destinationRectangle,
                                                   const D2D1_RECT_F *sourceRectangle = NULL) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTransformedImageSource(ID2D1ImageSource *imageSource,
                                                                   const D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES *properties,
                                                                   ID2D1TransformedImageSource **transformedImageSource) = 0;
};
struct __declspec(uuid("235a7496-8351-414c-bcd4-6672ab2d8e00")) __declspec(novtable) ID2D1DeviceContext3
    : public ID2D1DeviceContext2
{
    virtual HRESULT STDMETHODCALLTYPE CreateSpriteBatch(ID2D1SpriteBatch **spriteBatch) = 0;
    virtual void STDMETHODCALLTYPE DrawSpriteBatch(ID2D1SpriteBatch *spriteBatch, UINT32 startIndex, UINT32 spriteCount,
                                                   ID2D1Bitmap *bitmap,
                                                   D2D1_BITMAP_INTERPOLATION_MODE interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                                                   D2D1_SPRITE_OPTIONS spriteOptions = D2D1_SPRITE_OPTIONS_NONE) = 0;
};
struct __declspec(uuid("8c427831-3d90-4476-b647-c4fae349e4db")) __declspec(novtable) ID2D1DeviceContext4
    : public ID2D1DeviceContext3
{
    virtual HRESULT STDMETHODCALLTYPE CreateSvgGlyphStyle(ID2D1SvgGlyphStyle **svgGlyphStyle) = 0;
    virtual void STDMETHODCALLTYPE DrawText(const WCHAR *string, UINT32 stringLength, IDWriteTextFormat *textFormat,
                                            const D2D1_RECT_F *layoutRect, ID2D1Brush *defaultFillBrush,
                                            ID2D1SvgGlyphStyle *svgGlyphStyle, UINT32 colorPaletteIndex = 0,
                                            D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                                            DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL) = 0;
    virtual void STDMETHODCALLTYPE DrawTextLayout(D2D1_POINT_2F origin, IDWriteTextLayout *textLayout,
                                                  ID2D1Brush *defaultFillBrush, ID2D1SvgGlyphStyle *svgGlyphStyle,
                                                  UINT32 colorPaletteIndex = 0,
                                                  D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT) = 0;
    virtual void STDMETHODCALLTYPE DrawColorBitmapGlyphRun(DWRITE_GLYPH_IMAGE_FORMATS glyphImageFormat,
                                                           D2D1_POINT_2F baselineOrigin, const DWRITE_GLYPH_RUN *glyphRun,
                                                           DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL,
                                                           D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION bitmapSnapOption =
                                                               D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT) = 0;
    virtual void STDMETHODCALLTYPE DrawSvgGlyphRun(D2D1_POINT_2F baselineOrigin, const DWRITE_GLYPH_RUN *glyphRun,
                                                   ID2D1Brush *defaultFillBrush = NULL,
                                                   ID2D1SvgGlyphStyle *svgGlyphStyle = NULL, UINT32 colorPaletteIndex = 0,
                                                   DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetColorBitmapGlyphImage(DWRITE_GLYPH_IMAGE_FORMATS glyphImageFormat,
                                                               D2D1_POINT_2F glyphOrigin, IDWriteFontFace *fontFace,
                                                               FLOAT fontEmSize, UINT16 glyphIndex, BOOL isSideways,
                                                               const D2D1_MATRIX_3X2_F *worldTransform, FLOAT dpiX,
                                                               FLOAT dpiY, D2D1_MATRIX_3X2_F *glyphTransform,
                                                               ID2D1Image **glyphImage) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSvgGlyphImage(D2D1_POINT_2F glyphOrigin, IDWriteFontFace *fontFace,
                                                       FLOAT fontEmSize, UINT16 glyphIndex, BOOL isSideways,
                                                       const D2D1_MATRIX_3X2_F *worldTransform,
                                                       ID2D1Brush *defaultFillBrush, ID2D1SvgGlyphStyle *svgGlyphStyle,
                                                       UINT32 colorPaletteIndex, D2D1_MATRIX_3X2_F *glyphTransform,
                                                       ID2D1CommandList **glyphImage) = 0;
};
struct __declspec(uuid("7836d248-68cc-4df6-b9e8-de991bf62eb7")) __declspec(novtable) ID2D1DeviceContext5
    : public ID2D1DeviceContext4
{
    virtual HRESULT STDMETHODCALLTYPE CreateSvgDocument(IStream *inputXmlStream, D2D1_SIZE_F viewportSize,
                                                        ID2D1SvgDocument **svgDocument) = 0;
    virtual void STDMETHODCALLTYPE DrawSvgDocument(ID2D1SvgDocument *svgDocument) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateColorContextFromDxgiColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace,
                                                                           ID2D1ColorContext1 **colorContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateColorContextFromSimpleColorProfile(const D2D1_SIMPLE_COLOR_PROFILE *simpleProfile,
                                                                               ID2D1ColorContext1 **colorContext) = 0;
};
struct __declspec(uuid("985f7e37-4ed0-4a19-98a3-15b0edfde306")) __declspec(novtable) ID2D1DeviceContext6
    : public ID2D1DeviceContext5
{
    virtual void STDMETHODCALLTYPE BlendImage(ID2D1Image *image, D2D1_BLEND_MODE blendMode,
                                              const D2D1_POINT_2F *targetOffset = NULL,
                                              const D2D1_RECT_F *imageRectangle = NULL,
                                              D2D1_INTERPOLATION_MODE interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR) = 0;
};
struct __declspec(uuid("ec891cf7-9b69-4851-9def-4e0915771e62")) __declspec(novtable) ID2D1DeviceContext7
    : public ID2D1DeviceContext6
{
    virtual DWRITE_PAINT_FEATURE_LEVEL STDMETHODCALLTYPE GetPaintFeatureLevel() = 0;
    virtual void STDMETHODCALLTYPE DrawPaintGlyphRun(D2D1_POINT_2F baselineOrigin, const DWRITE_GLYPH_RUN *glyphRun,
                                                     ID2D1Brush *defaultFillBrush = NULL, UINT32 colorPaletteIndex = 0,
                                                     DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL) = 0;
    virtual void STDMETHODCALLTYPE DrawGlyphRunWithColorSupport(D2D1_POINT_2F baselineOrigin,
                                                                const DWRITE_GLYPH_RUN *glyphRun,
                                                                const DWRITE_GLYPH_RUN_DESCRIPTION *glyphRunDescription,
                                                                ID2D1Brush *foregroundBrush,
                                                                ID2D1SvgGlyphStyle *svgGlyphStyle,
                                                                UINT32 colorPaletteIndex = 0,
                                                                DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL,
                                                                D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION bitmapSnapOption =
                                                                    D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT) = 0;
};
typedef enum D2D1_RENDERING_PRIORITY { D2D1_RENDERING_PRIORITY_NORMAL = 0, D2D1_RENDERING_PRIORITY_LOW = 1,
                                       D2D1_RENDERING_PRIORITY_FORCE_DWORD = 0xffffffff } D2D1_RENDERING_PRIORITY;
struct __declspec(uuid("d21768e1-23a4-4823-a14b-7c3eba85d658")) __declspec(novtable) ID2D1Device1 : public ID2D1Device
{
    virtual D2D1_RENDERING_PRIORITY STDMETHODCALLTYPE GetRenderingPriority() = 0;
    virtual void STDMETHODCALLTYPE SetRenderingPriority(D2D1_RENDERING_PRIORITY renderingPriority) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext1 **deviceContext1) = 0;
    using ID2D1Device::CreateDeviceContext;
};
struct __declspec(uuid("a44472e1-8dfb-4e60-8492-6e2861c9ca8b")) __declspec(novtable) ID2D1Device2 : public ID2D1Device1
{
    virtual HRESULT STDMETHODCALLTYPE CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext2 **deviceContext2) = 0;
    virtual void STDMETHODCALLTYPE FlushDeviceContexts(ID2D1Bitmap *bitmap) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDxgiDevice(IDXGIDevice **dxgiDevice) = 0;
    using ID2D1Device1::CreateDeviceContext;
};
#endif
