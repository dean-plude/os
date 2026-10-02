/*
 * UIAutomationCore.dll — UI Automation.  As with Active Accessibility
 * (oleacc), no client ever listens on NovaOS, so providers are never
 * asked for and events have nobody to reach.
 */
#include <objbase.h>
#include <oleauto.h>

#define UIAAPI __declspec(dllexport)

UIAAPI BOOL WINAPI UiaClientsAreListening(void) { return FALSE; }
UIAAPI LRESULT WINAPI UiaReturnRawElementProvider(HWND h, WPARAM wp, LPARAM lp, IUnknown *el) { (void)h; (void)wp; (void)lp; (void)el; return 0; }
UIAAPI HRESULT WINAPI UiaHostProviderFromHwnd(HWND h, IUnknown **out) { (void)h; if (out) *out = 0; return E_NOTIMPL; }
UIAAPI HRESULT WINAPI UiaRaiseAutomationEvent(IUnknown *p, int id) { (void)p; (void)id; return S_OK; }
UIAAPI HRESULT WINAPI UiaRaiseAutomationPropertyChangedEvent(IUnknown *p, int id, VARIANT oldv, VARIANT newv) { (void)p; (void)id; (void)oldv; (void)newv; return S_OK; }
UIAAPI HRESULT WINAPI UiaRaiseStructureChangedEvent(IUnknown *p, int type, int *id, int n) { (void)p; (void)type; (void)id; (void)n; return S_OK; }
UIAAPI HRESULT WINAPI UiaRaiseNotificationEvent(IUnknown *p, int kind, int proc, BSTR text, BSTR act) { (void)p; (void)kind; (void)proc; (void)text; (void)act; return S_OK; }
UIAAPI HRESULT WINAPI UiaDisconnectProvider(IUnknown *p) { (void)p; return S_OK; }
UIAAPI HRESULT WINAPI UiaDisconnectAllProviders(void) { return S_OK; }
/* The reserved sentinel objects: one static IUnknown each */
static HRESULT STDMETHODCALLTYPE s_qi(IUnknown *u, REFIID iid, void **out) { (void)iid; *out = u; return S_OK; }
static ULONG STDMETHODCALLTYPE s_ref(IUnknown *u) { (void)u; return 1; }
static const IUnknownVtbl g_sentinel_vtbl = { s_qi, s_ref, s_ref };
static IUnknown g_not_supported = { &g_sentinel_vtbl }, g_mixed = { &g_sentinel_vtbl };
UIAAPI HRESULT WINAPI UiaGetReservedNotSupportedValue(IUnknown **out) { if (!out) return E_INVALIDARG; *out = &g_not_supported; return S_OK; }
UIAAPI HRESULT WINAPI UiaGetReservedMixedAttributeValue(IUnknown **out) { if (!out) return E_INVALIDARG; *out = &g_mixed; return S_OK; }
