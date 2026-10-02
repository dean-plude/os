/*
 * propframe.c — OleCreatePropertyFrame: the dialog that hosts an object's
 * property pages (DirectShow filters' settings, ActiveX controls).
 * NovaOS has no property-page frame, so it reports that.
 */
#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>

#define OLEAUTAPI __declspec(dllexport)

OLEAUTAPI HRESULT WINAPI OleCreatePropertyFrame(HWND owner, UINT x, UINT y, LPCOLESTR caption, ULONG nobjs,
                                                IUnknown **objs, ULONG npages, const CLSID *pages, LCID lcid,
                                                DWORD reserved, void *preserved)
{
    (void)owner; (void)x; (void)y; (void)caption; (void)nobjs; (void)objs; (void)npages; (void)pages;
    (void)lcid; (void)reserved; (void)preserved;
    return E_NOTIMPL;
}

OLEAUTAPI HRESULT WINAPI OleCreatePropertyFrameIndirect(void *params)
{
    (void)params;
    return E_NOTIMPL;
}
