// NovaOS: the framework's XamlIsland (Microsoft.UI.Xaml.XamlIsland), the
// XAML content of one content island.  Microsoft's open sources ship only
// its declarations (dxaml/lib/XamlIsland_Partial.h and
// winrtgeneratedclasses/XamlIsland.g.h); this is NovaOS's implementation of
// them, written for NovaOS from what the framework's callers expect:
//   DesktopWindowXamlSource::Initialize (dxaml/lib/DesktopWindowXamlSource_
//     Partial.cpp) makes one per window and takes the XAML island root under
//     it (GetXamlIslandRootNoRef), then connects that root's content island
//     to a DesktopChildSiteBridge on the window (ConnectToHwndIslandSite);
//   its Content, SystemBackdrop and ShouldConstrainPopupsToWorkArea forward
//     here, and its Close calls PrepareToClose, then Close.
// An island keeps the thread's XAML core running (WindowsXamlManager, as
// WindowsXamlManager.InitializeForCurrentThread gives it) and makes its root
// as the framework's own CreateIslandRootWithContentBridge does: the
// islands' start-up on the thread's dummy window, a XamlIslandRoot of the
// desktop content bridge kind (CXamlIslandRoot::Initialize with a bridge,
// which connects later, through the site bridge) and the core told of it
// (CCoreServices::AddXamlIslandRoot, which puts it under the main visual
// tree's island collection and gives it a composition target).  Closing
// takes it back out (RemoveXamlIslandRoot) and disposes of the core root.
#include "precomp.h"
#include "XamlIsland.g.h"
#include "WindowsXamlManager_Partial.h"
#include "XamlIslandRoot_Partial.h"
#include "Window_Partial.h"
#include "SystemBackdrop.g.h"
#include "UIElement.g.h"
#include "XamlIslandRoot.h"
#include "ContentRoot.h"

using namespace DirectUI;

// what the island's header has no field for: the composition brush the
// island's host hands it as its system backdrop (ICompositionSupportsSystem
// Backdrop, which a backdrop controller sets) and whether its popups stay
// inside the work area (true by default, as Windows').  An island is used on
// its own thread only, as the framework's objects are.
namespace
{
    struct IslandExtras
    {
        const XamlIsland *island;
        ctl::ComPtr<ABI::Windows::UI::Composition::ICompositionBrush> backdropBrush;
        bool constrainPopups;
        IslandExtras *next;
    };
    thread_local IslandExtras *t_extras;

    IslandExtras *Extras(const XamlIsland *island, bool make)
    {
        for (IslandExtras *e = t_extras; e; e = e->next)
            if (e->island == island)
                return e;
        if (!make)
            return nullptr;
        IslandExtras *e = new (std::nothrow) IslandExtras{island, nullptr, true, t_extras};
        if (e)
            t_extras = e;
        return e;
    }

    void ForgetExtras(const XamlIsland *island)
    {
        for (IslandExtras **p = &t_extras; *p; p = &(*p)->next)
            if ((*p)->island == island)
            {
                IslandExtras *e = *p;
                *p = e->next;
                delete e;
                return;
            }
    }
}

// The generated class (what XamlIsland.g.cpp would hold): the interfaces it
// answers and its properties, each forwarded to the partial class's Impl.

DirectUI::XamlIslandGenerated::XamlIslandGenerated()
{
}

DirectUI::XamlIslandGenerated::~XamlIslandGenerated()
{
}

HRESULT DirectUI::XamlIslandGenerated::QueryInterfaceImpl(_In_ REFIID iid, _Outptr_ void** ppObject)
{
    if (InlineIsEqualGUID(iid, __uuidof(DirectUI::XamlIsland)))
    {
        *ppObject = static_cast<DirectUI::XamlIsland*>(this);
    }
    else if (InlineIsEqualGUID(iid, __uuidof(ABI::Microsoft::UI::Xaml::IXamlIsland)))
    {
        *ppObject = ctl::interface_cast<ABI::Microsoft::UI::Xaml::IXamlIsland>(this);
    }
    else if (InlineIsEqualGUID(iid, __uuidof(ABI::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop)))
    {
        *ppObject = static_cast<ABI::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop*>(this);
    }
    else if (InlineIsEqualGUID(iid, __uuidof(ABI::Windows::Foundation::IClosable)))
    {
        *ppObject = static_cast<ABI::Windows::Foundation::IClosable*>(this);
    }
    else
    {
        RRETURN(ctl::WeakReferenceSource::QueryInterfaceImpl(iid, ppObject));
    }

    AddRefOuter();
    RRETURN(S_OK);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::get_Content(_Outptr_result_maybenull_ ABI::Microsoft::UI::Xaml::IUIElement** ppValue)
{
    ARG_VALIDRETURNPOINTER(ppValue);
    *ppValue = {};
    return static_cast<XamlIsland*>(this)->get_ContentImpl(ppValue);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::put_Content(_In_opt_ ABI::Microsoft::UI::Xaml::IUIElement* pValue)
{
    return static_cast<XamlIsland*>(this)->put_ContentImpl(pValue);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::get_ContentIsland(_Outptr_result_maybenull_ ABI::Microsoft::UI::Content::IContentIsland** ppValue)
{
    ARG_VALIDRETURNPOINTER(ppValue);
    *ppValue = {};
    return static_cast<XamlIsland*>(this)->get_ContentIslandImpl(ppValue);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::get_ShouldConstrainPopupsToWorkArea(_Out_ BOOLEAN* pValue)
{
    ARG_VALIDRETURNPOINTER(pValue);
    *pValue = {};
    return static_cast<XamlIsland*>(this)->get_ShouldConstrainPopupsToWorkAreaImpl(pValue);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::put_ShouldConstrainPopupsToWorkArea(BOOLEAN value)
{
    return static_cast<XamlIsland*>(this)->put_ShouldConstrainPopupsToWorkAreaImpl(value);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::get_SystemBackdrop(_Outptr_result_maybenull_ ABI::Microsoft::UI::Xaml::Media::ISystemBackdrop** ppValue)
{
    ARG_VALIDRETURNPOINTER(ppValue);
    *ppValue = {};
    return static_cast<XamlIsland*>(this)->get_SystemBackdropImpl(ppValue);
}

_Check_return_ HRESULT STDMETHODCALLTYPE DirectUI::XamlIslandGenerated::put_SystemBackdrop(_In_opt_ ABI::Microsoft::UI::Xaml::Media::ISystemBackdrop* pValue)
{
    return static_cast<XamlIsland*>(this)->put_SystemBackdropImpl(pValue);
}

// Its factory: `new XamlIsland()` (not composable: an outer object is
// refused, as the class is sealed).

HRESULT DirectUI::XamlIslandFactory::QueryInterfaceImpl(_In_ REFIID iid, _Outptr_ void** ppObject)
{
    if (InlineIsEqualGUID(iid, __uuidof(ABI::Microsoft::UI::Xaml::IXamlIslandFactory)))
    {
        *ppObject = static_cast<ABI::Microsoft::UI::Xaml::IXamlIslandFactory*>(this);
    }
    else
    {
        RRETURN(ctl::AggregableActivationFactory<DirectUI::XamlIsland>::QueryInterfaceImpl(iid, ppObject));
    }

    AddRefOuter();
    RRETURN(S_OK);
}

_Check_return_ HRESULT DirectUI::XamlIslandFactory::CheckActivationAllowed()
{
    RRETURN(S_OK);
}

IFACEMETHODIMP DirectUI::XamlIslandFactory::CreateInstance(_In_opt_ IInspectable* pOuter, _Outptr_ IInspectable** ppInner, _Outptr_ ABI::Microsoft::UI::Xaml::IXamlIsland** ppInstance)
{
    ARG_VALIDRETURNPOINTER(ppInstance);
    *ppInstance = nullptr;
    if (ppInner)
    {
        *ppInner = nullptr;
    }
    if (pOuter)
    {
        IFC_RETURN(CLASS_E_NOAGGREGATION);
    }
    ctl::ComPtr<XamlIsland> island;
    IFC_RETURN(ctl::make<XamlIsland>(&island));
    IFC_RETURN(ctl::do_query_interface(*ppInstance, island.Get()));
    return S_OK;
}

namespace DirectUI
{
    _Check_return_ IActivationFactory* CreateActivationFactory_XamlIsland()
    {
        RRETURN(ctl::ActivationFactoryCreator<XamlIslandFactory>::CreateActivationFactory());
    }
}

// The partial class.

XamlIsland::XamlIsland()
    : m_xamlIslandRoot(nullptr)
{
}

XamlIsland::~XamlIsland()
{
    IGNOREHR(Close());
    ForgetExtras(this);
}

_Check_return_ HRESULT XamlIsland::Initialize()
{
    IFC_RETURN(XamlIslandGenerated::Initialize());

    // the thread's XAML core, kept running while the island lives
    m_spXamlCore = WindowsXamlManager::GetForCurrentThread();
    if (!m_spXamlCore)
    {
        IFC_RETURN(ctl::make<WindowsXamlManager>(&m_spXamlCore));
    }

    // the islands' start-up (FrameworkApplication::CreateIslandRootWithContentBridgeImpl)
    DXamlCore *dxamlCore = DXamlCore::GetCurrent();
    IFCEXPECT_RETURN(dxamlCore);
    DirectUI::Window *window = dxamlCore->GetDummyWindowNoRef();
    IFCEXPECT_RETURN(window);
    IFC_RETURN(window->EnsureInitializedForIslands());

    // the root, of the desktop content bridge kind: it connects when the
    // site bridge takes its content island
    ctl::ComPtr<XamlIslandRoot> root;
    IFC_RETURN(ctl::make<XamlIslandRoot>(
        static_cast<WUComp::Desktop::IDesktopWindowContentBridgeInterop *>(nullptr), &root));
    IFC_RETURN(root.As(&m_spXamlIsland));
    m_xamlIslandRoot = root.Get();
    m_pXamlIslandCore = static_cast<CXamlIslandRoot *>(root->GetHandle());

    dxamlCore->GetHandle()->AddXamlIslandRoot(m_pXamlIslandCore);
    // its content island is wanted from the start: the site bridge shows it,
    // so the composition tree host hangs the root's visuals on it once a
    // frame has rendered them (DCompTreeHost::EnsureXamlIslandTargetRoots and
    // ConnectXamlIslandTargetRoots look for a requested island)
    m_pXamlIslandCore->SetContentRequested(true);
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::QueryInterfaceImpl(_In_ REFIID iid, _Outptr_ void** ppObject)
{
    return XamlIslandGenerated::QueryInterfaceImpl(iid, ppObject);
}

void XamlIsland::PrepareToClose()
{
    if (m_pXamlIslandCore)
    {
        if (CContentRoot *contentRoot = m_pXamlIslandCore->GetContentRootNoRef())
        {
            contentRoot->PrepareToClose();
        }
    }
}

IFACEMETHODIMP XamlIsland::Close()
{
    if (m_bClosed)
    {
        return S_OK;
    }
    m_bClosed = true;

    if (m_pXamlIslandCore)
    {
        // out of the main visual tree and its composition target, then the
        // core root's content root, input and automation
        if (DXamlCore *dxamlCore = DXamlCore::GetCurrent())
        {
            dxamlCore->GetHandle()->RemoveXamlIslandRoot(m_pXamlIslandCore);
        }
        m_pXamlIslandCore->Dispose();
        m_pXamlIslandCore = nullptr;
    }
    m_xamlIslandRoot = nullptr;
    m_spXamlIsland = nullptr;
    m_systemBackdrop = nullptr;
    if (IslandExtras *extras = Extras(this, false))
    {
        extras->backdropBrush = nullptr;
    }
    m_spXamlCore = nullptr;
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::get_ContentImpl(_Outptr_ xaml::IUIElement** ppValue)
{
    *ppValue = nullptr;
    if (!m_spXamlIsland)
    {
        return RO_E_CLOSED;
    }
    IFC_RETURN(m_spXamlIsland->get_Content(ppValue));
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::put_ContentImpl(_In_opt_ xaml::IUIElement* pValue)
{
    if (!m_spXamlIsland)
    {
        return pValue ? RO_E_CLOSED : S_OK;
    }
    IFC_RETURN(m_spXamlIsland->put_Content(pValue));
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::get_ContentIslandImpl(_Outptr_ ixp::IContentIsland **ppValue)
{
    *ppValue = nullptr;
    if (!m_pXamlIslandCore)
    {
        return RO_E_CLOSED;
    }
    m_pXamlIslandCore->SetContentRequested(true);
    *ppValue = m_pXamlIslandCore->GetContentIsland();
    if (*ppValue)
    {
        (*ppValue)->AddRef();
    }
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::get_ShouldConstrainPopupsToWorkAreaImpl(_Out_ boolean * pValue)
{
    IslandExtras *extras = Extras(this, false);
    *pValue = extras ? extras->constrainPopups : true;
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::put_ShouldConstrainPopupsToWorkAreaImpl(_In_opt_ boolean value)
{
    IslandExtras *extras = Extras(this, true);
    IFCOOMFAILFAST(extras);
    extras->constrainPopups = !!value;
    return S_OK;
}

IFACEMETHODIMP XamlIsland::get_SystemBackdrop(_Outptr_result_maybenull_ ABI::Windows::UI::Composition::ICompositionBrush * *systemBackdropBrush)
{
    ARG_VALIDRETURNPOINTER(systemBackdropBrush);
    *systemBackdropBrush = nullptr;
    if (IslandExtras *extras = Extras(this, false))
    {
        IFC_RETURN(extras->backdropBrush.CopyTo(systemBackdropBrush));
    }
    return S_OK;
}

IFACEMETHODIMP XamlIsland::put_SystemBackdrop(_In_opt_ ABI::Windows::UI::Composition::ICompositionBrush * systemBackdropBrush)
{
    IslandExtras *extras = Extras(this, true);
    IFCOOMFAILFAST(extras);
    extras->backdropBrush = systemBackdropBrush;
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::get_SystemBackdropImpl(_Outptr_result_maybenull_ xaml::Media::ISystemBackdrop * *iSystemBackdrop)
{
    *iSystemBackdrop = nullptr;
    IFC_RETURN(m_systemBackdrop.CopyTo(iSystemBackdrop));
    return S_OK;
}

_Check_return_ HRESULT XamlIsland::put_SystemBackdropImpl(_In_opt_ xaml::Media::ISystemBackdrop * iSystemBackdrop)
{
    m_systemBackdrop = iSystemBackdrop;
    return S_OK;
}

_Check_return_ xaml_hosting::IXamlIslandRoot *XamlIsland::GetXamlIslandRootNoRef()
{
    return m_spXamlIsland.Get();
}
