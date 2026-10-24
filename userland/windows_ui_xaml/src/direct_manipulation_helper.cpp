// NovaOS: DirectManipulationHelper's class factories (the Windows App SDK's
// FrameworkUdk ships it closed; inc/DirectManipulationHelper.h): the lifted
// DirectManipulation DLL's own classes, from its DllGetClassObject, which
// the framework's DirectManipulation probe and service make their manager
// and compositor with.
#include <windows.h>
#include "DirectManipulationHelper.h"

namespace
{
    // the lifted DirectManipulation's manager and its DComp manipulation
    // compositor (the classes Windows' directmanipulation.h names)
    const CLSID CLSID_Manager = { 0x54E211B6, 0x3650, 0x4F75, { 0x83, 0x34, 0xFA, 0x35, 0x95, 0x98, 0xE1, 0xC5 } };
    const CLSID CLSID_DCompCompositor = { 0x79DEA627, 0xA08A, 0x43AC, { 0x8E, 0xF5, 0x69, 0x00, 0xB9, 0x29, 0x91, 0x26 } };

    HRESULT GetFactory(HMODULE dmanip, REFCLSID clsid, IClassFactory **factory)
    {
        *factory = nullptr;
        if (!dmanip) return E_INVALIDARG;
        auto get = reinterpret_cast<HRESULT (WINAPI *)(REFCLSID, REFIID, void **)>(GetProcAddress(dmanip, "DllGetClassObject"));
        if (!get) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        return get(clsid, __uuidof(IClassFactory), reinterpret_cast<void **>(factory));
    }
}

HRESULT DirectManipulationHelper::GetDirectManipulationManagerFactory(HMODULE dmanip, IClassFactory **factory)
{
    return GetFactory(dmanip, CLSID_Manager, factory);
}

HRESULT DirectManipulationHelper::GetDCompManipulationCompositorFactory(HMODULE dmanip, IClassFactory **factory)
{
    return GetFactory(dmanip, CLSID_DCompCompositor, factory);
}

// NovaOS's DirectManipulation hands out no manager (Microsoft.DirectManipulation.dll:
// the framework treats manipulations as unsupported), so no viewport has an
// interaction to chain to another process: there is nothing to set on the visual
HRESULT DirectManipulationHelper::SetInteraction(ABI::Microsoft::UI::Composition::IVisual *visual, IInspectable *interaction)
{
    (void)visual;
    (void)interaction;
    return S_OK;
}
