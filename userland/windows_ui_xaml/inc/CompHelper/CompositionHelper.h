/* NovaOS: the Windows App SDK's CompHelper/CompositionHelper.h (shipped
 * closed with the lifted composition code).  XAML's composition tree host
 * (components/comptree/DCompTreeHost.cpp) keeps a CompositionHelper that
 * owns its composition device and surface factory: it makes the
 * Compositor XAML's visuals live on, the surface factory and surfaces its
 * drawing goes into (wrapped as ICompositionSurface for brushes), offers
 * and reclaims their memory, and reports batch ids and the largest texture
 * size.  DCompTreeHost holds it in a CompositionHelperHolder made when the
 * host initializes, and the helper tells the host through
 * INotifyCompositionDirty when composition needs a commit.  Declared here as XAML calls it; NovaOS defines it on
 * its Windows.UI.Composition (userland/dcomp) when the composition tree
 * builds. */
#pragma once
#include <windows.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <microsoft.ui.composition.h>

class DCompTreeHost;

// what DCompTreeHost answers when composition changed outside XAML's
// frame (it requests a commit of its device)
struct __declspec(novtable) INotifyCompositionDirty
{
    virtual HRESULT NotifyCompositionDirty() = 0;
};

class CompositionHelper
{
public:
    // the composition device (the Compositor XAML's visuals are made on)
    IDCompositionDevice2 *GetMainDevice() const;
    IDCompositionSurfaceFactory *GetSurfaceFactory() const;
    // the surface factory on @renderingDevice (XAML's D2D or D3D11 device)
    _Check_return_ HRESULT CreateSurfaceFactory(_In_ IUnknown *renderingDevice);
    _Check_return_ HRESULT FlushSurfaceFactory();
    _Check_return_ HRESULT CreateSurface(UINT width, UINT height, DXGI_FORMAT format, DXGI_ALPHA_MODE alphaMode,
                                         bool isExclusive, _Outptr_ IDCompositionSurface **surface);
    _Check_return_ HRESULT CreateSurface(_In_ IDCompositionSurfaceFactory *factory, UINT width, UINT height,
                                         DXGI_FORMAT format, DXGI_ALPHA_MODE alphaMode, bool isExclusive,
                                         _Outptr_ IDCompositionSurface **surface);
    // @surface as an ICompositionSurface for a brush (none: an empty one)
    _Check_return_ HRESULT CreateCompositionSurfaceForDCompositionSurface(
        _In_opt_ IDCompositionSurface *surface,
        _Outptr_ ABI::Microsoft::UI::Composition::ICompositionSurface **result);
    _Check_return_ HRESULT OfferSurfaceResources(_In_ IDCompositionSurfaceFactory *factory);
    _Check_return_ HRESULT ReclaimSurfaceResources(_In_ IDCompositionSurfaceFactory *factory, _Out_ bool *discarded);
    _Check_return_ HRESULT HintSize(UINT width, UINT height);
    _Check_return_ HRESULT Flush();
    _Check_return_ HRESULT GetCurrentBatchID(_Out_ ULONG *batchId);
    _Check_return_ HRESULT GetLastConfirmedBatchId(_Out_ ULONG *batchId);
    uint32_t GetMaxTextureSize() const;
    void SetAutoCompleteKeyFrameAnimationsOnScreenOccluded(bool enable);
    void ReleaseResources(bool shouldDeferClosingInteropCompositor);
    void ReleaseGraphicsResources();
    void ReleaseObjectsForClosedDevice();
    void CloseAndReleaseInteropCompositor();
};

// DCompTreeHost's CompositionHelper, made by Initialize
class CompositionHelperHolder
{
public:
    _Check_return_ HRESULT Initialize(_In_ DCompTreeHost *host);
    void Uninitialize();
    bool IsInitialized() const { return m_helper != nullptr; }
    CompositionHelper *Get() const { return m_helper; }
    CompositionHelper *operator->() const { return m_helper; }

private:
    CompositionHelper *m_helper = nullptr;
};
