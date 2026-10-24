// NovaOS: TextAnalysis_GetContentReadingDirection (the Windows App SDK's
// FrameworkUdk ships it closed; inc/TextAnalysis.h): the reading direction
// a text block's text sets when its TextReadingOrder or TextAlignment is
// DetectFromContent (the default), from its first strong character, as
// the Unicode bidirectional algorithm's paragraph level (rules P2 and P3):
// a Hebrew, Arabic, Syriac, Thaana, N'Ko or Samaritan letter reads right to
// left, any other letter left to right, and text with no letter is
// ambiguous (the element's flow direction stays).
#include <windows.h>
#include <wctype.h>
#include "TextAnalysis.h"

namespace
{
    // 1: right to left, 0: left to right, -1: not a strong character
    int strong_direction(wchar_t c)
    {
        if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF))
            return (c >= 0x0600 && c <= 0x0605) || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9) ? -1 : 1;
        return iswalpha(c) ? 0 : -1;
    }
}

HRESULT TextAnalysis_GetContentReadingDirection(IDWriteTextAnalyzer *analyzer, IDWriteTextAnalysisSource *source,
                                                UINT32 textPosition, UINT32 textLength,
                                                DWRITE_READING_DIRECTION *readingDirection, BOOL *isAmbiguous)
{
    (void)analyzer;
    if (!source || !readingDirection || !isAmbiguous) return E_INVALIDARG;
    *readingDirection = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    *isAmbiguous = TRUE;
    UINT32 end = textPosition + textLength;
    for (UINT32 at = textPosition; at < end;)
    {
        const WCHAR *text = nullptr;
        UINT32 n = 0;
        HRESULT hr = source->GetTextAtPosition(at, &text, &n);
        if (FAILED(hr)) return hr;
        if (!text || !n) break;
        if (n > end - at) n = end - at;
        for (UINT32 i = 0; i < n; i++)
        {
            int d = strong_direction(text[i]);
            if (d >= 0)
            {
                *readingDirection = d ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
                *isAmbiguous = FALSE;
                return S_OK;
            }
        }
        at += n;
    }
    return S_OK;
}
