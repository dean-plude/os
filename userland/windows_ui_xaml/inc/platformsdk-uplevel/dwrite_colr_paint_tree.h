/* NovaOS: the Windows SDK's uplevel DirectWrite colour declarations, which
 * core/native/text's D2DTextDrawingContext.cpp includes for glyphs drawn
 * from a font's COLRv1 paint tree: the COLR_PAINT_TREE glyph image format,
 * the flag operators the Windows SDK gives DWRITE_GLYPH_IMAGE_FORMATS (the
 * drawing context combines formats so), the paint feature level (d2d1_3.h,
 * which ID2D1DeviceContext7 answers it with) and IDWriteFactory8, its
 * IID and vtable as the Windows SDK's dwrite_3.h declares them.  NovaOS's
 * DirectWrite answers IDWriteFactory7 at most, so XAML asks only for the
 * formats before COLRv1 (GetDWriteGlyphImageFormats). */
#pragma once
#include <dwrite_3.h>
#include <d2d1_3.h>

#define DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE ((DWRITE_GLYPH_IMAGE_FORMATS)0x00000100)
constexpr DWRITE_GLYPH_IMAGE_FORMATS operator|(DWRITE_GLYPH_IMAGE_FORMATS a, DWRITE_GLYPH_IMAGE_FORMATS b)
{
    return DWRITE_GLYPH_IMAGE_FORMATS((int)a | (int)b);
}
constexpr DWRITE_GLYPH_IMAGE_FORMATS operator&(DWRITE_GLYPH_IMAGE_FORMATS a, DWRITE_GLYPH_IMAGE_FORMATS b)
{
    return DWRITE_GLYPH_IMAGE_FORMATS((int)a & (int)b);
}
constexpr DWRITE_GLYPH_IMAGE_FORMATS operator~(DWRITE_GLYPH_IMAGE_FORMATS a) { return DWRITE_GLYPH_IMAGE_FORMATS(~(int)a); }
inline DWRITE_GLYPH_IMAGE_FORMATS &operator|=(DWRITE_GLYPH_IMAGE_FORMATS &a, DWRITE_GLYPH_IMAGE_FORMATS b) { return a = a | b; }
inline DWRITE_GLYPH_IMAGE_FORMATS &operator&=(DWRITE_GLYPH_IMAGE_FORMATS &a, DWRITE_GLYPH_IMAGE_FORMATS b) { return a = a & b; }

struct __declspec(uuid("ee0a7fb5-def4-4c23-a454-c9c7dc878398")) __declspec(novtable) IDWriteFactory8 : public IDWriteFactory7
{
    virtual HRESULT STDMETHODCALLTYPE TranslateColorGlyphRun(D2D1_POINT_2F baselineOrigin, DWRITE_GLYPH_RUN const *glyphRun,
                                                             DWRITE_GLYPH_RUN_DESCRIPTION const *glyphRunDescription,
                                                             DWRITE_GLYPH_IMAGE_FORMATS desiredGlyphImageFormats,
                                                             DWRITE_PAINT_FEATURE_LEVEL paintFeatureLevel,
                                                             DWRITE_MEASURING_MODE measuringMode,
                                                             DWRITE_MATRIX const *worldAndDpiTransform,
                                                             UINT32 colorPaletteIndex,
                                                             IDWriteColorGlyphRunEnumerator1 **colorEnumerator) = 0;
};
