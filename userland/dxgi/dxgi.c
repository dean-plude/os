/*
 * dxgi.dll — DirectX Graphics Infrastructure, NovaOS's own.
 *
 * NovaOS has no display adapter driver, so its own factory lists no
 * adapters and creates no swap chains: programs that import dxgi.dll start,
 * and find no Direct3D hardware.
 *
 * When DXVK's dxgi is installed (as dxgi_dxvk.dll beside this one), the
 * factory functions hand programs DXVK's factory instead, so Direct3D 10
 * and 11 work system-wide.  The Khronos Vulkan loader (which programs
 * often carry beside their .exe, in place of NovaOS's vulkan-1.dll) still
 * gets NovaOS's: it loads System32\dxgi.dll and needs a factory before it
 * reads its drivers, and DXVK's factory starts Vulkan, so giving it DXVK's
 * would have DXVK and the loader start each other until DXVK's own lock
 * deadlocks.
 */
#include <windows.h>

#define DXGIAPI __declspec(dllexport)
#define DXGI_ERROR_NOT_FOUND_         ((HRESULT)0x887A0002L)
#define DXGI_ERROR_UNSUPPORTED_       ((HRESULT)0x887A0004L)

static const GUID IID_IUnknown_      = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IDXGIObject_   = { 0xaec22fb8, 0x76f3, 0x4639, { 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e } };
static const GUID IID_IDXGIFactory_  = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const GUID IID_IDXGIFactory1_ = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };

static int same(const GUID *a, const GUID *b) { return !__builtin_memcmp(a, b, sizeof(GUID)); }

/* ---- NovaOS's factory: IDXGIFactory1 with no adapters -------------------- */
typedef struct Factory Factory;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Factory *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(Factory *);
    ULONG   (WINAPI *Release)(Factory *);
    HRESULT (WINAPI *SetPrivateData)(Factory *, const GUID *, UINT, const void *);
    HRESULT (WINAPI *SetPrivateDataInterface)(Factory *, const GUID *, void *);
    HRESULT (WINAPI *GetPrivateData)(Factory *, const GUID *, UINT *, void *);
    HRESULT (WINAPI *GetParent)(Factory *, const GUID *, void **);
    HRESULT (WINAPI *EnumAdapters)(Factory *, UINT, void **);
    HRESULT (WINAPI *MakeWindowAssociation)(Factory *, HWND, UINT);
    HRESULT (WINAPI *GetWindowAssociation)(Factory *, HWND *);
    HRESULT (WINAPI *CreateSwapChain)(Factory *, void *, void *, void **);
    HRESULT (WINAPI *CreateSoftwareAdapter)(Factory *, HMODULE, void **);
    HRESULT (WINAPI *EnumAdapters1)(Factory *, UINT, void **);
    BOOL    (WINAPI *IsCurrent)(Factory *);
} FactoryVtbl;
struct Factory { const FactoryVtbl *vtbl; LONG refs; HWND assoc; };

static HRESULT WINAPI f_qi(Factory *f, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (same(iid, &IID_IUnknown_) || same(iid, &IID_IDXGIObject_) || same(iid, &IID_IDXGIFactory_) ||
        same(iid, &IID_IDXGIFactory1_)) {
        InterlockedIncrement(&f->refs);
        *out = f;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI f_addref(Factory *f) { return (ULONG)InterlockedIncrement(&f->refs); }
static ULONG WINAPI f_release(Factory *f)
{
    LONG r = InterlockedDecrement(&f->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, f);
    return (ULONG)r;
}
static HRESULT WINAPI f_setpriv(Factory *f, const GUID *g, UINT n, const void *d) { (void)f; (void)g; (void)n; (void)d; return S_OK; }
static HRESULT WINAPI f_setprivif(Factory *f, const GUID *g, void *u) { (void)f; (void)g; (void)u; return S_OK; }
static HRESULT WINAPI f_getpriv(Factory *f, const GUID *g, UINT *n, void *d)
{ (void)f; (void)g; (void)d; if (n) *n = 0; return DXGI_ERROR_NOT_FOUND_; }
static HRESULT WINAPI f_getparent(Factory *f, const GUID *g, void **out) { (void)f; (void)g; if (out) *out = NULL; return E_NOINTERFACE; }
static HRESULT WINAPI f_enum(Factory *f, UINT i, void **out) { (void)f; (void)i; if (out) *out = NULL; return DXGI_ERROR_NOT_FOUND_; }
static HRESULT WINAPI f_assoc(Factory *f, HWND h, UINT flags) { (void)flags; f->assoc = h; return S_OK; }
static HRESULT WINAPI f_getassoc(Factory *f, HWND *h) { if (!h) return E_POINTER; *h = f->assoc; return S_OK; }
static HRESULT WINAPI f_swapchain(Factory *f, void *dev, void *desc, void **out)
{ (void)f; (void)dev; (void)desc; if (out) *out = NULL; return DXGI_ERROR_UNSUPPORTED_; }
static HRESULT WINAPI f_soft(Factory *f, HMODULE m, void **out) { (void)f; (void)m; if (out) *out = NULL; return DXGI_ERROR_UNSUPPORTED_; }
static BOOL WINAPI f_current(Factory *f) { (void)f; return TRUE; }

static const FactoryVtbl g_factory_vtbl = {
    f_qi, f_addref, f_release, f_setpriv, f_setprivif, f_getpriv, f_getparent, f_enum,
    f_assoc, f_getassoc, f_swapchain, f_soft, f_enum, f_current,
};

static HRESULT own_factory(const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    Factory *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->vtbl = &g_factory_vtbl;
    f->refs = 1;
    HRESULT hr = f_qi(f, iid, out);
    f_release(f);
    return hr;
}

/* ---- DXVK's, when installed ---------------------------------------------- */
static HMODULE dxvk(void)
{
    static HMODULE m;
    static LONG looked;
    if (!InterlockedCompareExchange(&looked, 1, 0)) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
        if (n && n < MAX_PATH - 16) {
            lstrcatA(path, "\\dxgi_dxvk.dll");
            m = LoadLibraryA(path);
        }
        InterlockedExchange(&looked, 2);
    }
    while (looked == 1) Sleep(0);
    return m;
}

/* Is @addr (a caller's return address) in the Vulkan loader? */
static BOOL from_vulkan_loader(void *addr)
{
    HMODULE m = NULL, v = GetModuleHandleA("vulkan-1.dll");
    return v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &m) && m == v;
}

typedef HRESULT (WINAPI *CreateFn)(const GUID *, void **);
typedef HRESULT (WINAPI *Create2Fn)(UINT, const GUID *, void **);

static FARPROC dxvk_proc(void *caller, const char *name)
{
    if (from_vulkan_loader(caller)) return NULL;
    HMODULE m = dxvk();
    return m ? GetProcAddress(m, name) : NULL;
}

DXGIAPI HRESULT WINAPI CreateDXGIFactory(const GUID *iid, void **out)
{
    CreateFn fn = (CreateFn)dxvk_proc(__builtin_return_address(0), "CreateDXGIFactory");
    return fn ? fn(iid, out) : own_factory(iid, out);
}

DXGIAPI HRESULT WINAPI CreateDXGIFactory1(const GUID *iid, void **out)
{
    CreateFn fn = (CreateFn)dxvk_proc(__builtin_return_address(0), "CreateDXGIFactory1");
    return fn ? fn(iid, out) : own_factory(iid, out);
}

DXGIAPI HRESULT WINAPI CreateDXGIFactory2(UINT flags, const GUID *iid, void **out)
{
    Create2Fn fn = (Create2Fn)dxvk_proc(__builtin_return_address(0), "CreateDXGIFactory2");
    return fn ? fn(flags, iid, out) : own_factory(iid, out);
}

DXGIAPI HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, const GUID *iid, void **out)
{
    Create2Fn fn = (Create2Fn)dxvk_proc(__builtin_return_address(0), "DXGIGetDebugInterface1");
    if (fn) return fn(flags, iid, out);
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

DXGIAPI HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void)
{
    typedef HRESULT (WINAPI *Fn)(void);
    Fn fn = (Fn)dxvk_proc(__builtin_return_address(0), "DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : S_OK;
}
