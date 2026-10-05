/*
 * dcomp.dll — DirectComposition, NovaOS's own.
 *
 * NovaOS has no compositor engine for DirectComposition's visual trees.
 * What it provides is what Chromium's software compositor (the GPU
 * process of WebView2 and of Steam's and Galaxy's browsers) uses: a device,
 * a target on a window, visuals with offsets and children, and a visual
 * whose content is a swap chain of dxgi.dll's software device (novadx.h).
 * Each present of such a swap chain (and each Commit) draws its frame into
 * the target's window with GDI, at the visual's offset.  Surfaces,
 * transforms, effects, clips and animations are not implemented.
 *
 * Only DCompositionCreateDevice is exported: Chromium turns on its
 * hardware DirectComposition path (overlays, swap chains on the GPU) only
 * when DCompositionCreateDevice3 exists, and that path needs a real GPU.
 */
#include <windows.h>
#include <novadx.h>

#define DCOMPAPI __declspec(dllexport)
#define MAX_TARGETS 64
#define MAX_KIDS    64

int _fltused = 0x9875;                  /* (offsets are floats; no C runtime here) */

typedef struct DcDevice DcDevice;
typedef struct Target Target;
typedef struct Visual Visual;

struct Visual {
    const struct VisualVtbl *vtbl;
    LONG refs;
    IUnknown *content;
    INovaSwapChain *chain;              /* (the content as a swap chain) */
    Target *target;                     /* (the root of this target's tree; not counted) */
    Visual *parent;                     /* (not counted) */
    Visual *kids[MAX_KIDS];             /* (counted) */
    int nkids;
    float ox, oy;
};

struct Target {
    const struct TargetVtbl *vtbl;
    LONG refs;
    HWND hwnd;
    Visual *root;                       /* (counted) */
};

/* The targets of every device, for Commit (a visual tree is the same
 * whichever device made it) */
static Target *g_targets[MAX_TARGETS];
static CRITICAL_SECTION g_lock;

/* ---- Drawing ------------------------------------------------------------- */
/* @v's place in its target's window, and the target (NULL: not shown) */
static Target *visual_place(Visual *v, int *x, int *y)
{
    float fx = 0, fy = 0;
    for (int depth = 0; v && depth < 64; depth++) {
        fx += v->ox; fy += v->oy;
        if (v->target) { *x = (int)fx; *y = (int)fy; return v->target; }
        v = v->parent;
    }
    return NULL;
}

static void draw_visual(Visual *v, const RECT *dirty)
{
    int x = 0, y = 0;
    Target *t = visual_place(v, &x, &y);
    if (!t || !v->chain || !IsWindow(t->hwnd)) return;
    UINT pitch = 0, w = 0, h = 0;
    BYTE *bits = v->chain->lpVtbl->Frame(v->chain, &pitch, &w, &h);
    if (!bits) return;
    HDC dc = GetDC(t->hwnd);
    if (!dc) return;
    novadx_blit(dc, x, y, bits, pitch, w, h, dirty);
    ReleaseDC(t->hwnd, dc);
}

static void draw_tree(Visual *v, int depth)
{
    if (!v || depth > 64) return;
    draw_visual(v, NULL);
    for (int i = 0; i < v->nkids; i++) draw_tree(v->kids[i], depth + 1);
}

/* a present of the swap chain a visual shows */
static void STDMETHODCALLTYPE visual_sink(void *ctx, const RECT *dirty)
{
    EnterCriticalSection(&g_lock);
    draw_visual(ctx, dirty);
    LeaveCriticalSection(&g_lock);
}

/* ---- Visuals ------------------------------------------------------------- */
typedef struct VisualVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Visual *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Visual *);
    ULONG   (STDMETHODCALLTYPE *Release)(Visual *);
    HRESULT (STDMETHODCALLTYPE *SetOffsetXAnim)(Visual *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *SetOffsetX)(Visual *, float);
    HRESULT (STDMETHODCALLTYPE *SetOffsetYAnim)(Visual *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *SetOffsetY)(Visual *, float);
    HRESULT (STDMETHODCALLTYPE *SetTransformObj)(Visual *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *SetTransform)(Visual *, const void *);
    HRESULT (STDMETHODCALLTYPE *SetTransformParent)(Visual *, Visual *);
    HRESULT (STDMETHODCALLTYPE *SetEffect)(Visual *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *SetBitmapInterpolationMode)(Visual *, UINT);
    HRESULT (STDMETHODCALLTYPE *SetBorderMode)(Visual *, UINT);
    HRESULT (STDMETHODCALLTYPE *SetClipObj)(Visual *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *SetClip)(Visual *, const void *);
    HRESULT (STDMETHODCALLTYPE *SetContent)(Visual *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *AddVisual)(Visual *, Visual *, BOOL, Visual *);
    HRESULT (STDMETHODCALLTYPE *RemoveVisual)(Visual *, Visual *);
    HRESULT (STDMETHODCALLTYPE *RemoveAllVisuals)(Visual *);
    HRESULT (STDMETHODCALLTYPE *SetCompositeMode)(Visual *, UINT);
} VisualVtbl;
static const VisualVtbl g_visual_vtbl;

static Visual *visual_of(void *p) { Visual *v = p; return v && v->vtbl == &g_visual_vtbl ? v : NULL; }

static void visual_set_content(Visual *v, IUnknown *content)
{
    if (v->chain) {
        v->chain->lpVtbl->SetSink(v->chain, NULL, NULL);
        v->chain->lpVtbl->Release(v->chain);
        v->chain = NULL;
    }
    if (v->content) v->content->lpVtbl->Release(v->content);
    v->content = content;
    if (!content) return;
    content->lpVtbl->AddRef(content);
    void *c = NULL;
    if (SUCCEEDED(content->lpVtbl->QueryInterface(content, &NIID_INovaSwapChain, &c))) {
        v->chain = c;
        v->chain->lpVtbl->SetSink(v->chain, visual_sink, v);
    }
}

static HRESULT STDMETHODCALLTYPE v_qi(Visual *v, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_IDCompositionVisual)) {
        InterlockedIncrement(&v->refs);
        *out = v;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE v_addref(Visual *v) { return (ULONG)InterlockedIncrement(&v->refs); }
static HRESULT STDMETHODCALLTYPE v_removeall(Visual *v);
static ULONG STDMETHODCALLTYPE v_release(Visual *v)
{
    LONG r = InterlockedDecrement(&v->refs);
    if (!r) {
        EnterCriticalSection(&g_lock);
        visual_set_content(v, NULL);
        v_removeall(v);
        LeaveCriticalSection(&g_lock);
        HeapFree(GetProcessHeap(), 0, v);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE v_ignore(Visual *v, IUnknown *u) { (void)v; (void)u; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_setx(Visual *v, float x) { v->ox = x; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_sety(Visual *v, float y) { v->oy = y; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_ignoreptr(Visual *v, const void *p) { (void)v; (void)p; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_ignorevis(Visual *v, Visual *p) { (void)v; (void)p; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_ignoremode(Visual *v, UINT m) { (void)v; (void)m; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_setcontent(Visual *v, IUnknown *content)
{
    EnterCriticalSection(&g_lock);
    visual_set_content(v, content);
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE v_add(Visual *v, Visual *kid, BOOL above, Visual *ref)
{
    (void)above; (void)ref;
    kid = visual_of(kid);
    if (!kid || kid->parent || kid->target || kid == v) return E_INVALIDARG;
    EnterCriticalSection(&g_lock);
    HRESULT hr = E_OUTOFMEMORY;
    if (v->nkids < MAX_KIDS) {
        v_addref(kid);
        kid->parent = v;
        v->kids[v->nkids++] = kid;
        hr = S_OK;
    }
    LeaveCriticalSection(&g_lock);
    return hr;
}
static HRESULT STDMETHODCALLTYPE v_remove(Visual *v, Visual *kid)
{
    EnterCriticalSection(&g_lock);
    HRESULT hr = E_INVALIDARG;
    for (int i = 0; i < v->nkids; i++) {
        if (v->kids[i] != kid) continue;
        for (int j = i; j + 1 < v->nkids; j++) v->kids[j] = v->kids[j + 1];
        v->nkids--;
        kid->parent = NULL;
        hr = S_OK;
        break;
    }
    LeaveCriticalSection(&g_lock);
    if (hr == S_OK) v_release(kid);
    return hr;
}
static HRESULT STDMETHODCALLTYPE v_removeall(Visual *v)
{
    while (v->nkids) v_remove(v, v->kids[v->nkids - 1]);
    return S_OK;
}

static const VisualVtbl g_visual_vtbl = {
    v_qi, v_addref, v_release, v_ignore, v_setx, v_ignore, v_sety, v_ignore, v_ignoreptr, v_ignorevis, v_ignore,
    v_ignoremode, v_ignoremode, v_ignore, v_ignoreptr, v_setcontent, v_add, v_remove, v_removeall, v_ignoremode,
};

/* ---- Targets ------------------------------------------------------------- */
typedef struct TargetVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Target *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(Target *);
    ULONG   (STDMETHODCALLTYPE *Release)(Target *);
    HRESULT (STDMETHODCALLTYPE *SetRoot)(Target *, Visual *);
} TargetVtbl;

static HRESULT STDMETHODCALLTYPE t_qi(Target *t, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_IDCompositionTarget)) {
        InterlockedIncrement(&t->refs);
        *out = t;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE t_addref(Target *t) { return (ULONG)InterlockedIncrement(&t->refs); }
static HRESULT STDMETHODCALLTYPE t_setroot(Target *t, Visual *v);
static ULONG STDMETHODCALLTYPE t_release(Target *t)
{
    LONG r = InterlockedDecrement(&t->refs);
    if (!r) {
        t_setroot(t, NULL);
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i] == t) g_targets[i] = NULL;
        LeaveCriticalSection(&g_lock);
        HeapFree(GetProcessHeap(), 0, t);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE t_setroot(Target *t, Visual *v)
{
    if (v && (!visual_of(v) || v->parent || (v->target && v->target != t))) return E_INVALIDARG;
    EnterCriticalSection(&g_lock);
    Visual *old = t->root;
    if (v) { v_addref(v); v->target = t; }
    t->root = v;
    if (old && old != v) old->target = NULL;
    LeaveCriticalSection(&g_lock);
    if (old) v_release(old);
    return S_OK;
}
static const TargetVtbl g_target_vtbl = { t_qi, t_addref, t_release, t_setroot };

/* ---- The device ---------------------------------------------------------- */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DcDevice *, const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DcDevice *);
    ULONG   (STDMETHODCALLTYPE *Release)(DcDevice *);
    HRESULT (STDMETHODCALLTYPE *Commit)(DcDevice *);
    HRESULT (STDMETHODCALLTYPE *WaitForCommitCompletion)(DcDevice *);
    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(DcDevice *, void *);
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(DcDevice *, HWND, BOOL, void **);
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSurface)(DcDevice *, UINT, UINT, UINT, UINT, void **);
    HRESULT (STDMETHODCALLTYPE *CreateVirtualSurface)(DcDevice *, UINT, UINT, UINT, UINT, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSurfaceFromHandle)(DcDevice *, HANDLE, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSurfaceFromHwnd)(DcDevice *, HWND, void **);
    HRESULT (STDMETHODCALLTYPE *CreateTranslateTransform)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateScaleTransform)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateRotateTransform)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSkewTransform)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateMatrixTransform)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateTransformGroup)(DcDevice *, void **, UINT, void **);
    HRESULT (STDMETHODCALLTYPE *CreateTranslateTransform3D)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateScaleTransform3D)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateRotateTransform3D)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateMatrixTransform3D)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateTransform3DGroup)(DcDevice *, void **, UINT, void **);
    HRESULT (STDMETHODCALLTYPE *CreateEffectGroup)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateRectangleClip)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateAnimation)(DcDevice *, void **);
    HRESULT (STDMETHODCALLTYPE *CheckDeviceState)(DcDevice *, BOOL *);
} DcDeviceVtbl;
struct DcDevice { const DcDeviceVtbl *vtbl; LONG refs; };

static HRESULT STDMETHODCALLTYPE d_qi(DcDevice *d, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (novadx_same(iid, &NIID_IUnknown) || novadx_same(iid, &NIID_IDCompositionDevice)) {
        InterlockedIncrement(&d->refs);
        *out = d;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE d_addref(DcDevice *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG STDMETHODCALLTYPE d_release(DcDevice *d)
{
    LONG r = InterlockedDecrement(&d->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, d);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE d_commit(DcDevice *d)
{
    (void)d;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i]) draw_tree(g_targets[i]->root, 0);
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_wait(DcDevice *d) { (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE d_stats(DcDevice *d, void *out)
{
    (void)d;
    if (!out) return E_INVALIDARG;
    struct { LARGE_INTEGER last; UINT num, den; LARGE_INTEGER now, freq, next; } *s = out;   /* DCOMPOSITION_FRAME_STATISTICS */
    QueryPerformanceCounter(&s->now);
    QueryPerformanceFrequency(&s->freq);
    s->num = 60; s->den = 1;
    s->last = s->now;
    s->next.QuadPart = s->now.QuadPart + s->freq.QuadPart / 60;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_target(DcDevice *d, HWND h, BOOL topmost, void **out)
{
    (void)d; (void)topmost;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsWindow(h)) return E_INVALIDARG;
    Target *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
    if (!t) return E_OUTOFMEMORY;
    t->vtbl = &g_target_vtbl;
    t->refs = 1;
    t->hwnd = h;
    EnterCriticalSection(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_TARGETS && slot < 0; i++) if (!g_targets[i]) slot = i;
    if (slot >= 0) g_targets[slot] = t;
    LeaveCriticalSection(&g_lock);
    if (slot < 0) { HeapFree(GetProcessHeap(), 0, t); return E_OUTOFMEMORY; }
    *out = t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_visual(DcDevice *d, void **out)
{
    (void)d;
    if (!out) return E_POINTER;
    Visual *v = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*v));
    if (!v) { *out = NULL; return E_OUTOFMEMORY; }
    v->vtbl = &g_visual_vtbl;
    v->refs = 1;
    *out = v;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_none1(DcDevice *d, void **out) { (void)d; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_none2(DcDevice *d, HANDLE h, void **out) { (void)d; (void)h; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_none2w(DcDevice *d, HWND h, void **out) { (void)d; (void)h; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_none3(DcDevice *d, void **a, UINT n, void **out) { (void)d; (void)a; (void)n; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_none5(DcDevice *d, UINT w, UINT h, UINT f, UINT a, void **out)
{ (void)d; (void)w; (void)h; (void)f; (void)a; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_state(DcDevice *d, BOOL *ok) { (void)d; if (!ok) return E_INVALIDARG; *ok = TRUE; return S_OK; }

static const DcDeviceVtbl g_device_vtbl = {
    d_qi, d_addref, d_release, d_commit, d_wait, d_stats, d_target, d_visual, d_none5, d_none5, d_none2, d_none2w,
    d_none1, d_none1, d_none1, d_none1, d_none1, d_none3, d_none1, d_none1, d_none1, d_none1, d_none3, d_none1, d_none1,
    d_none1, d_state,
};

/* @dxgi: the rendering device (unused: content is drawn with GDI) */
DCOMPAPI HRESULT WINAPI DCompositionCreateDevice(IUnknown *dxgi, const GUID *iid, void **out)
{
    (void)dxgi;
    if (!out) return E_POINTER;
    *out = NULL;
    DcDevice *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return E_OUTOFMEMORY;
    d->vtbl = &g_device_vtbl;
    d->refs = 1;
    HRESULT hr = d_qi(d, iid, out);
    d_release(d);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_lock);
    return TRUE;
}
