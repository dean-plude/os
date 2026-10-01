/*
 * dwmapi.dll — the Desktop Window Manager.  NovaOS's window manager does
 * not composite with glass or blur, so composition reports off; window
 * attributes are accepted (dark title bars, corner preferences...) and the
 * frame bounds are the window rectangle.
 */
#include <windows.h>

#define DWMAPI __declspec(dllexport)
#define E_INVALIDARG_ ((HRESULT)0x80070057L)
#define DWM_E_COMPOSITIONDISABLED_ ((HRESULT)0x80263001L)

enum { DWMWA_NCRENDERING_ENABLED_ = 1, DWMWA_CAPTION_BUTTON_BOUNDS_ = 5, DWMWA_EXTENDED_FRAME_BOUNDS_ = 9,
       DWMWA_CLOAKED_ = 14 };

DWMAPI HRESULT WINAPI DwmIsCompositionEnabled(BOOL *on)
{
    if (!on) return E_INVALIDARG_;
    *on = FALSE;
    return S_OK;
}
DWMAPI HRESULT WINAPI DwmEnableComposition(UINT action) { (void)action; return S_OK; }
DWMAPI HRESULT WINAPI DwmSetWindowAttribute(HWND w, DWORD attr, LPCVOID data, DWORD n)
{
    (void)attr; (void)data; (void)n;
    return IsWindow(w) ? S_OK : E_INVALIDARG_;
}
DWMAPI HRESULT WINAPI DwmGetWindowAttribute(HWND w, DWORD attr, PVOID data, DWORD n)
{
    if (!IsWindow(w) || !data) return E_INVALIDARG_;
    switch (attr) {
    case DWMWA_EXTENDED_FRAME_BOUNDS_:
    case DWMWA_CAPTION_BUTTON_BOUNDS_:
        if (n < sizeof(RECT)) return E_INVALIDARG_;
        GetWindowRect(w, (RECT *)data);
        return S_OK;
    case DWMWA_NCRENDERING_ENABLED_:
    case DWMWA_CLOAKED_:
        if (n < sizeof(DWORD)) return E_INVALIDARG_;
        *(DWORD *)data = 0;
        return S_OK;
    default:
        return E_INVALIDARG_;
    }
}
DWMAPI HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND w, const void *margins)
{ (void)w; (void)margins; return DWM_E_COMPOSITIONDISABLED_; }
DWMAPI HRESULT WINAPI DwmEnableBlurBehindWindow(HWND w, const void *bb)
{ (void)w; (void)bb; return DWM_E_COMPOSITIONDISABLED_; }
DWMAPI HRESULT WINAPI DwmGetColorizationColor(DWORD *color, BOOL *opaque)
{
    if (!color || !opaque) return E_INVALIDARG_;
    *color = 0xFF0078D7;                          /* accent blue, opaque */
    *opaque = TRUE;
    return S_OK;
}
DWMAPI HRESULT WINAPI DwmFlush(void) { return S_OK; }
DWMAPI BOOL WINAPI DwmDefWindowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *r)
{ (void)w; (void)msg; (void)wp; (void)lp; if (r) *r = 0; return FALSE; }
DWMAPI HRESULT WINAPI DwmSetIconicThumbnail(HWND w, HBITMAP b, DWORD f) { (void)w; (void)b; (void)f; return S_OK; }
DWMAPI HRESULT WINAPI DwmSetIconicLivePreviewBitmap(HWND w, HBITMAP b, POINT *p, DWORD f)
{ (void)w; (void)b; (void)p; (void)f; return S_OK; }
DWMAPI HRESULT WINAPI DwmInvalidateIconicBitmaps(HWND w) { (void)w; return S_OK; }
DWMAPI HRESULT WINAPI DwmGetCompositionTimingInfo(HWND w, void *info) { (void)w; (void)info; return DWM_E_COMPOSITIONDISABLED_; }
