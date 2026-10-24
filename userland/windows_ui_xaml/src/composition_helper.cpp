// NovaOS: the composition tree host's CompositionHelper
// (inc/CompHelper/CompositionHelper.h), which the Windows App SDK ships
// closed.  DCompTreeHost (components/comptree/DCompTreeHost.cpp) asks it
// for one object that is both its DirectComposition device (Commit,
// GetFrameStatistics, surface factories) and the Microsoft.UI.Composition
// Compositor its visuals are made on (ICompositor, 2, 5, 6 and the swap
// chain interop).  NovaOS's dcomp.dll gives exactly that: its Compositor
// answers IDCompositionDevice, IDCompositionDevice2 and 3 too
// (userland/dcomp/dcomp.c, dcomp_part_new), so the helper makes a
// Compositor and keeps it as its device.  The surfaces XAML draws its
// elements into are that device's surface factory's (a texture on XAML's
// Direct3D 11 device, drawn with its Direct2D device), shown in sprite
// visuals through dcomp.dll's NovaCompositionSurfaceForDComp, and a Commit
// of the device has the compositor draw them again.
//
// Offering and reclaiming surface memory, size hints and flushes have
// nothing to do: NovaOS's surfaces are read back at each EndDraw.
#include "precomp.h"
#include "DCompTreeHost.h"
#include <CompHelper/CompositionSurfaceHelper.h>
#include <roapi.h>
#include <microsoft.ui.composition.h>

namespace
{
    template <typename T> void Drop(T *&p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    typedef HRESULT(WINAPI *SurfaceForDComp)(IUnknown *, IUnknown *, void **);
}

_Check_return_ HRESULT CompositionHelper::Initialize()
{
    wrl::ComPtr<IInspectable> compositor;
    IFC_RETURN(RoActivateInstance(
        wrl_wrappers::HStringReference(RuntimeClass_Microsoft_UI_Composition_Compositor).Get(), &compositor));
    IFC_RETURN(compositor->QueryInterface(__uuidof(IDCompositionDevice2), reinterpret_cast<void **>(&m_device)));
    return S_OK;
}

IDCompositionDevice2 *CompositionHelper::GetMainDevice() const { return m_device; }
IDCompositionSurfaceFactory *CompositionHelper::GetSurfaceFactory() const { return m_factory; }

_Check_return_ HRESULT CompositionHelper::CreateSurfaceFactory(_In_ IUnknown *renderingDevice)
{
    if (!m_device)
        return RO_E_CLOSED;
    IDCompositionSurfaceFactory *factory = nullptr;
    IFC_RETURN(m_device->CreateSurfaceFactory(renderingDevice, &factory));
    Drop(m_factory);
    Drop(m_renderingDevice);
    m_factory = factory;
    m_renderingDevice = renderingDevice;
    m_renderingDevice->AddRef();
    return S_OK;
}

_Check_return_ HRESULT CompositionHelper::FlushSurfaceFactory() { return S_OK; }
_Check_return_ HRESULT CompositionHelper::FlushSurfaceFactory(_In_ IDCompositionSurfaceFactory *) { return S_OK; }

_Check_return_ HRESULT CompositionHelper::CreateSurface(UINT width, UINT height, DXGI_FORMAT format,
                                                        DXGI_ALPHA_MODE alphaMode, bool isExclusive,
                                                        _Outptr_ IDCompositionSurface **surface)
{
    *surface = nullptr;
    if (!m_factory)
        return E_UNEXPECTED;
    return CreateSurface(m_factory, width, height, format, alphaMode, isExclusive, surface);
}

_Check_return_ HRESULT CompositionHelper::CreateSurface(_In_ IDCompositionSurfaceFactory *factory, UINT width,
                                                        UINT height, DXGI_FORMAT format, DXGI_ALPHA_MODE alphaMode,
                                                        bool, _Outptr_ IDCompositionSurface **surface)
{
    *surface = nullptr;
    return factory->CreateSurface(width, height, format, alphaMode, surface);
}

_Check_return_ HRESULT CompositionHelper::CreateCompositionSurfaceForDCompositionSurface(
    _In_opt_ IDCompositionSurface *surface, _Outptr_ ABI::Microsoft::UI::Composition::ICompositionSurface **result)
{
    *result = nullptr;
    if (!m_device)
        return RO_E_CLOSED;
    static SurfaceForDComp make = reinterpret_cast<SurfaceForDComp>(
        GetProcAddress(GetModuleHandleA("dcomp.dll"), "NovaCompositionSurfaceForDComp"));
    if (!make)
        return E_NOTIMPL;
    return make(m_device, surface, reinterpret_cast<void **>(result));
}

_Check_return_ HRESULT CompositionHelper::OfferSurfaceResources(_In_ IDCompositionSurfaceFactory *) { return S_OK; }

_Check_return_ HRESULT CompositionHelper::ReclaimSurfaceResources(_In_ IDCompositionSurfaceFactory *,
                                                                  _Out_ BOOL *discarded)
{
    *discarded = FALSE;
    return S_OK;
}

_Check_return_ HRESULT CompositionHelper::HintSize(UINT, UINT) { return S_OK; }

_Check_return_ HRESULT CompositionHelper::Flush()
{
    m_batch++;
    return S_OK;
}

_Check_return_ HRESULT CompositionHelper::GetCurrentBatchID(_Out_ ULONG *batchId)
{
    *batchId = m_batch;
    return S_OK;
}

// (everything flushed is drawn: dcomp.dll's compositor draws as it changes)
_Check_return_ HRESULT CompositionHelper::GetLastConfirmedBatchId(_Out_ ULONG *batchId)
{
    *batchId = m_batch;
    return S_OK;
}

// (Direct3D 11's feature level 11 limit, as dcomp.dll's software surfaces allow)
uint32_t CompositionHelper::GetMaxTextureSize() const { return 16384; }

void CompositionHelper::SetAutoCompleteKeyFrameAnimationsOnScreenOccluded(bool) {}

void CompositionHelper::ReleaseResources(bool shouldDeferClosingInteropCompositor)
{
    ReleaseGraphicsResources();
    if (!shouldDeferClosingInteropCompositor)
        CloseAndReleaseInteropCompositor();
}

void CompositionHelper::ReleaseGraphicsResources()
{
    Drop(m_factory);
    Drop(m_renderingDevice);
}

void CompositionHelper::ReleaseObjectsForClosedDevice() { ReleaseGraphicsResources(); }

void CompositionHelper::CloseAndReleaseInteropCompositor()
{
    ReleaseGraphicsResources();
    if (m_device)
    {
        wrl::ComPtr<wf::IClosable> closable;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&closable))))
            closable->Close();
    }
    Drop(m_device);
}

_Check_return_ HRESULT CompositionHelperHolder::Initialize(_In_ DCompTreeHost *)
{
    CompositionHelper *helper = new (std::nothrow) CompositionHelper();
    if (!helper)
        return E_OUTOFMEMORY;
    HRESULT hr = helper->Initialize();
    if (FAILED(hr))
    {
        delete helper;
        return hr;
    }
    m_helper = helper;
    return S_OK;
}

void CompositionHelperHolder::Uninitialize()
{
    if (m_helper)
    {
        m_helper->CloseAndReleaseInteropCompositor();
        delete m_helper;
        m_helper = nullptr;
    }
}

// The DirectComposition surface helper (inc/CompHelper/CompositionSurfaceHelper.h,
// also closed) each of XAML's surfaces draws through
// (plat/win/desktop/DCompSurface.cpp).  NovaOS's surfaces need no gutters
// (dcomp.dll samples them without bilinear filtering), so BeginDraw reports
// none and is the surface's own; their pixels are always kept.
_Check_return_ HRESULT CompositionSurfaceHelper::Initialize(_In_opt_ IDCompositionSurface *surface)
{
    if (surface)
        surface->AddRef();
    Drop(m_surface);
    m_surface = surface;
    return S_OK;
}

_Check_return_ HRESULT CompositionSurfaceHelper::ResetWinRTSurface(
    _In_opt_ ABI::Microsoft::UI::Composition::ICompositionSurface *winrtSurface, _In_opt_ IDCompositionSurface *surface)
{
    typedef HRESULT(WINAPI * SetDComp)(IUnknown *, IUnknown *);
    static SetDComp set = reinterpret_cast<SetDComp>(
        GetProcAddress(GetModuleHandleA("dcomp.dll"), "NovaCompositionSurfaceSetDComp"));
    if (!set)
        return E_NOTIMPL;
    return winrtSurface ? set(winrtSurface, surface) : S_OK;
}

void CompositionSurfaceHelper::ReleaseSurface() { Drop(m_surface); }

_Check_return_ HRESULT CompositionSurfaceHelper::HasValidPixels(_Out_ BOOL *valid)
{
    *valid = m_surface != nullptr;
    return S_OK;
}

_Check_return_ HRESULT CompositionSurfaceHelper::Resize(UINT width, UINT height)
{
    if (!m_surface)
        return E_UNEXPECTED;
    wrl::ComPtr<IDCompositionVirtualSurface> virtualSurface;
    IFC_RETURN(m_surface->QueryInterface(IID_PPV_ARGS(&virtualSurface)));
    return virtualSurface->Resize(width, height);
}

// (a copy of a surface's pixels: XAML's surface dumps and render target
// bitmaps read one; not yet)
_Check_return_ HRESULT CompositionSurfaceHelper::CopySurface(_Outptr_ IDXGISurface **copy)
{
    *copy = nullptr;
    return E_NOTIMPL;
}

_Check_return_ HRESULT CompositionSurfaceHelper::BeginDrawWithGutters(_In_opt_ const RECT *updateRect, REFIID iid,
                                                                      _Outptr_ void **updateObject,
                                                                      _Out_ POINT *updateOffset,
                                                                      _Out_ COMPOSITION_HELPER_GUTTERS *gutters)
{
    *updateObject = nullptr;
    *gutters = {};
    if (!m_surface)
        return E_UNEXPECTED;
    return m_surface->BeginDraw(updateRect, iid, updateObject, updateOffset);
}
