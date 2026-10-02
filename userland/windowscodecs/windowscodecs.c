/*
 * windowscodecs.dll — the Windows Imaging Component.  NovaOS has no WIC
 * codecs, so its imaging factory class is not registered
 * (CoCreateInstance fails, and programs fall back to their own image
 * code) and the helpers that need a bitmap source cannot have one.
 */
#include <objbase.h>

#define WINCODEC_ERR_COMPONENTNOTFOUND ((HRESULT)0x88982F50L)

__declspec(dllexport) HRESULT WINAPI WICConvertBitmapSource(REFGUID fmt, IUnknown *src, IUnknown **out)
{
    (void)fmt; (void)src;
    if (out) *out = 0;
    return WINCODEC_ERR_COMPONENTNOTFOUND;
}
__declspec(dllexport) HRESULT WINAPI WICCreateImagingFactory_Proxy(UINT version, IUnknown **out)
{
    (void)version;
    if (out) *out = 0;
    return WINCODEC_ERR_COMPONENTNOTFOUND;
}
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    (void)clsid; (void)iid;
    if (out) *out = 0;
    return CLASS_E_CLASSNOTAVAILABLE;
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_OK; }
