/*
 * Microsoft.DirectManipulation.dll — the Windows App SDK's lifted
 * DirectManipulation, which the XAML framework loads to pan and zoom
 * ScrollViewers with touch and precision touchpads.  NovaOS has no
 * manipulation engine yet, so its manager class is there but cannot be
 * made: the framework's probe (CWindowsServices::
 * IsDirectManipulationSupported) then reports DirectManipulation as not
 * supported, as on a Windows build without it, and ScrollViewers scroll
 * by their own offsets (mouse wheel, scroll bars, keyboard).
 */
#include <windows.h>
#include <string.h>

#define DMAPI __declspec(dllexport)
#define CLASS_E_NOAGGREGATION_ ((HRESULT)0x80040110L)
#define CLASS_E_CLASSNOTAVAILABLE_ ((HRESULT)0x80040111L)

/* the class factory (IClassFactory's methods, in order) */
typedef struct Factory Factory;
typedef struct {
    HRESULT (__stdcall *QueryInterface)(Factory *, const GUID *, void **);
    ULONG (__stdcall *AddRef)(Factory *);
    ULONG (__stdcall *Release)(Factory *);
    HRESULT (__stdcall *CreateInstance)(Factory *, void *, const GUID *, void **);
    HRESULT (__stdcall *LockServer)(Factory *, BOOL);
} FactoryVtbl;
struct Factory { const FactoryVtbl *lpVtbl; };

/* CLSID_DirectManipulationManager, IID_IUnknown, IID_IClassFactory */
static const GUID CLSID_Manager_ = { 0x54E211B6, 0x3650, 0x4F75, { 0x83, 0x34, 0xFA, 0x35, 0x95, 0x98, 0xE1, 0xC5 } };
static const GUID IID_IUnknown_ = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IClassFactory_ = { 0x00000001, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

static BOOL same(const GUID *a, const GUID *b) { return memcmp(a, b, sizeof(GUID)) == 0; }

static HRESULT __stdcall cf_qi(Factory *cf, const GUID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = same(riid, &IID_IUnknown_) || same(riid, &IID_IClassFactory_) ? cf : NULL;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static ULONG __stdcall cf_addref(Factory *cf) { (void)cf; return 2; }
static ULONG __stdcall cf_release(Factory *cf) { (void)cf; return 1; }
/* (no manager yet: the caller falls back to scrolling without it) */
static HRESULT __stdcall cf_create(Factory *cf, void *outer, const GUID *riid, void **ppv)
{
    (void)cf; (void)riid;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    return outer ? CLASS_E_NOAGGREGATION_ : E_NOTIMPL;
}
static HRESULT __stdcall cf_lock(Factory *cf, BOOL lock) { (void)cf; (void)lock; return S_OK; }
static const FactoryVtbl g_cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_factory = { &g_cf_vtbl };

DMAPI HRESULT WINAPI DllGetClassObject(const GUID *clsid, const GUID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (same(clsid, &CLSID_Manager_)) return cf_qi(&g_factory, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE_;
}

DMAPI HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
