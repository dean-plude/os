/*
 * d3d12.dll — Direct3D 12, NovaOS's own front.
 *
 * NovaOS has no Direct3D 12 driver (DXVK, its Direct3D from the App
 * Store, covers versions 8 to 11), so it answers as Windows does on a PC
 * whose display adapter has none: no device can be made, the debug layer
 * (a Graphics Tools component) is not installed, and experimental
 * features cannot be turned on.  Programs that import d3d12.dll load and
 * take their Direct3D 11 or software paths.  Root signatures are only
 * created on a device, so serializing one is not provided.
 */
#include <windows.h>

#define D3D12API __declspec(dllexport)
#define DXGI_ERROR_UNSUPPORTED_            ((HRESULT)0x887A0004L)
#define DXGI_ERROR_SDK_COMPONENT_MISSING_  ((HRESULT)0x887A002DL)

D3D12API HRESULT WINAPI D3D12CreateDevice(void *adapter, UINT level, REFIID iid, void **device)
{
    (void)adapter; (void)level; (void)iid;
    if (device) *device = NULL;
    return DXGI_ERROR_UNSUPPORTED_;
}

D3D12API HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
    (void)iid;
    if (debug) *debug = NULL;
    return DXGI_ERROR_SDK_COMPONENT_MISSING_;
}

D3D12API HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT n, const IID *iids, void *config, UINT *sizes)
{
    (void)n; (void)iids; (void)config; (void)sizes;
    return E_NOINTERFACE;
}

D3D12API HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **out)
{
    (void)clsid; (void)iid;
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT no_root_signature(void **blob, void **error)
{
    if (blob) *blob = NULL;
    if (error) *error = NULL;
    return E_NOTIMPL;
}
D3D12API HRESULT WINAPI D3D12SerializeRootSignature(const void *desc, UINT version, void **blob, void **error)
{ (void)desc; (void)version; return no_root_signature(blob, error); }
D3D12API HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void *desc, void **blob, void **error)
{ (void)desc; return no_root_signature(blob, error); }
D3D12API HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T n, REFIID iid, void **out)
{ (void)data; (void)n; (void)iid; if (out) *out = NULL; return E_NOTIMPL; }
D3D12API HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T n, REFIID iid, void **out)
{ (void)data; (void)n; (void)iid; if (out) *out = NULL; return E_NOTIMPL; }
