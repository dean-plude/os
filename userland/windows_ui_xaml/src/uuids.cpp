// NovaOS: the interface and class IDs XAML's sources name as data
// (IID_IUnknown, IID_IInspectable, ...), which a Windows program takes from
// the Windows SDK's uuid.lib; NovaOS's import libraries carry functions
// only.  Each is its interface's own __uuidof, as the headers declare it,
// or (a class or pixel format ID, which has no interface) the value the
// SDK's header gives it.  They are data, so the stand-ins for the
// functions not built yet (build.py unbuilt()) must not answer for them: a
// comparison with an IID that is a thunk's code never matches (a peer
// then refuses QueryInterface for IUnknown).
#include <windows.h>
#include <unknwn.h>
#include <inspectable.h>
#include <activation.h>
#include <objidl.h>
#include <oaidl.h>
#include <urlmon.h>

extern "C" {
extern const IID IID_IUnknown = __uuidof(IUnknown);
extern const IID IID_IInspectable = __uuidof(IInspectable);
extern const IID IID_IActivationFactory = __uuidof(IActivationFactory);
extern const IID IID_IAgileObject = __uuidof(IAgileObject);
extern const IID IID_IMarshal = __uuidof(IMarshal);
extern const IID IID_IGlobalInterfaceTable = __uuidof(IGlobalInterfaceTable);
extern const IID IID_IErrorInfo = __uuidof(IErrorInfo);
extern const IID IID_IAuthenticate = __uuidof(IAuthenticate);
extern const IID IID_IBindStatusCallback = __uuidof(IBindStatusCallback);
extern const IID IID_IHttpNegotiate = __uuidof(IHttpNegotiate);
extern const IID IID_IInternetBindInfo = __uuidof(IInternetBindInfo);
// (cguid.h, wincodec.h)
extern const CLSID CLSID_StdGlobalInterfaceTable = {0x00000323, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// (d2d1effects_2.h: the cross-fade effect a connected animation describes)
extern const CLSID CLSID_D2D1CrossFade = {0x12f575e8, 0x4db1, 0x485f, {0x9a, 0x84, 0x03, 0xa0, 0x7d, 0xd3, 0x82, 0x9f}};
extern const GUID GUID_WICPixelFormat8bppAlpha = {0xe6cd0116, 0xeeba, 0x4161, {0xaa, 0x85, 0x27, 0xdd, 0x9f, 0xb3, 0xa8, 0x95}};
extern const GUID GUID_WICPixelFormat8bppGray = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x08}};
extern const GUID GUID_WICPixelFormat32bppBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
extern const CLSID CLSID_WICImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
}
