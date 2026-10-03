/*
 * xapo.c — XAPO effects between programs and FAudio (see xapo.h)
 */

#include "xapo.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* IXAPO / IXAPOParameters: XAudio2 2.8 and later, and the DirectX SDK's */
static const GUID IID_IXAPO            = { 0xA410B984, 0x9839, 0x4819, { 0xA0, 0xBE, 0x28, 0x56, 0xAE, 0x6B, 0x3A, 0xDB } };
static const GUID IID_IXAPOParameters  = { 0x26D95C66, 0x80F2, 0x499A, { 0xAD, 0x54, 0x5A, 0xE7, 0xF0, 0x1C, 0x6D, 0x98 } };
static const GUID IID_IXAPO27          = { 0xA90BC001, 0xE897, 0xE897, { 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00 } };
static const GUID IID_IXAPOParameters27 = { 0xA90BC001, 0xE897, 0xE897, { 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x01 } };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetRegistrationProperties)(void *, FAPORegistrationProperties **);
    HRESULT (STDMETHODCALLTYPE *IsInputFormatSupported)(void *, const FAudioWaveFormatEx *, const FAudioWaveFormatEx *, FAudioWaveFormatEx **);
    HRESULT (STDMETHODCALLTYPE *IsOutputFormatSupported)(void *, const FAudioWaveFormatEx *, const FAudioWaveFormatEx *, FAudioWaveFormatEx **);
    HRESULT (STDMETHODCALLTYPE *Initialize)(void *, const void *, UINT32);
    void    (STDMETHODCALLTYPE *Reset)(void *);
    HRESULT (STDMETHODCALLTYPE *LockForProcess)(void *, UINT32, const FAPOLockForProcessBufferParameters *, UINT32,
                                                const FAPOLockForProcessBufferParameters *);
    void    (STDMETHODCALLTYPE *UnlockForProcess)(void *);
    void    (STDMETHODCALLTYPE *Process)(void *, UINT32, const FAPOProcessBufferParameters *, UINT32,
                                         FAPOProcessBufferParameters *, BOOL);
    UINT32  (STDMETHODCALLTYPE *CalcInputFrames)(void *, UINT32);
    UINT32  (STDMETHODCALLTYPE *CalcOutputFrames)(void *, UINT32);
} IXAPOVtbl;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    void    (STDMETHODCALLTYPE *SetParameters)(void *, const void *, UINT32);
    void    (STDMETHODCALLTYPE *GetParameters)(void *, void *, UINT32);
} IXAPOParametersVtbl;
typedef struct { const IXAPOVtbl *v; } IXAPO;
typedef struct { const IXAPOParametersVtbl *v; } IXAPOParameters;

/* -----------------------------------------------------------------------
 * A program's XAPO as an FAPO
 * ----------------------------------------------------------------------- */
typedef struct {
    FAPO             f;
    LONG             refs;
    IXAPO           *x;
    IXAPOParameters *p;
} AppFapo;

static int32_t FAPOCALL af_addref(void *f) { return InterlockedIncrement(&((AppFapo *)f)->refs); }
static int32_t FAPOCALL af_release(void *f)
{
    AppFapo *a = f;
    LONG r = InterlockedDecrement(&a->refs);
    if (!r) {
        if (a->p) a->p->v->Release(a->p);
        a->x->v->Release(a->x);
        CoTaskMemFree(a);
    }
    return r;
}
static uint32_t FAPOCALL af_props(void *f, FAPORegistrationProperties **pp)
{ AppFapo *a = f; return a->x->v->GetRegistrationProperties(a->x, pp); }
static uint32_t FAPOCALL af_in(void *f, const FAudioWaveFormatEx *o, const FAudioWaveFormatEx *r, FAudioWaveFormatEx **s)
{ AppFapo *a = f; return a->x->v->IsInputFormatSupported(a->x, o, r, s); }
static uint32_t FAPOCALL af_out(void *f, const FAudioWaveFormatEx *i, const FAudioWaveFormatEx *r, FAudioWaveFormatEx **s)
{ AppFapo *a = f; return a->x->v->IsOutputFormatSupported(a->x, i, r, s); }
static uint32_t FAPOCALL af_init(void *f, const void *d, uint32_t n) { AppFapo *a = f; return a->x->v->Initialize(a->x, d, n); }
static void FAPOCALL af_reset(void *f) { AppFapo *a = f; a->x->v->Reset(a->x); }
static uint32_t FAPOCALL af_lock(void *f, uint32_t ni, const FAPOLockForProcessBufferParameters *i, uint32_t no,
                                 const FAPOLockForProcessBufferParameters *o)
{ AppFapo *a = f; return a->x->v->LockForProcess(a->x, ni, i, no, o); }
static void FAPOCALL af_unlock(void *f) { AppFapo *a = f; a->x->v->UnlockForProcess(a->x); }
static void FAPOCALL af_process(void *f, uint32_t ni, const FAPOProcessBufferParameters *i, uint32_t no,
                                FAPOProcessBufferParameters *o, int32_t on)
{ AppFapo *a = f; a->x->v->Process(a->x, ni, i, no, o, on); }
static uint32_t FAPOCALL af_calc_in(void *f, uint32_t n) { AppFapo *a = f; return a->x->v->CalcInputFrames(a->x, n); }
static uint32_t FAPOCALL af_calc_out(void *f, uint32_t n) { AppFapo *a = f; return a->x->v->CalcOutputFrames(a->x, n); }
static void FAPOCALL af_set(void *f, const void *p, uint32_t n) { AppFapo *a = f; if (a->p) a->p->v->SetParameters(a->p, p, n); }
static void FAPOCALL af_get(void *f, void *p, uint32_t n) { AppFapo *a = f; if (a->p) a->p->v->GetParameters(a->p, p, n); }

/* -----------------------------------------------------------------------
 * A built-in FAPO as an XAPO
 * ----------------------------------------------------------------------- */
typedef struct {
    const IXAPOVtbl           *v;
    const IXAPOParametersVtbl *pv;
    LONG                       refs;
    FAPO                      *f;
} OurXapo;

#define FROM_P(p) ((OurXapo *)((char *)(p) - __builtin_offsetof(OurXapo, pv)))

static HRESULT STDMETHODCALLTYPE ox_qi(void *o, REFIID riid, void **ppv)
{
    OurXapo *x = o;
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IXAPO) || IsEqualIID(riid, &IID_IXAPO27)) *ppv = x;
    else if (IsEqualIID(riid, &IID_IXAPOParameters) || IsEqualIID(riid, &IID_IXAPOParameters27)) *ppv = &x->pv;
    else { *ppv = 0; return E_NOINTERFACE; }
    InterlockedIncrement(&x->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE ox_addref(void *o) { return InterlockedIncrement(&((OurXapo *)o)->refs); }
static ULONG STDMETHODCALLTYPE ox_release(void *o)
{
    OurXapo *x = o;
    LONG r = InterlockedDecrement(&x->refs);
    if (!r) {
        x->f->Release(x->f);
        CoTaskMemFree(x);
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE ox_props(void *o, FAPORegistrationProperties **pp)
{ FAPO *f = ((OurXapo *)o)->f; return f->GetRegistrationProperties(f, pp); }
static HRESULT STDMETHODCALLTYPE ox_in(void *o, const FAudioWaveFormatEx *a, const FAudioWaveFormatEx *b, FAudioWaveFormatEx **c)
{ FAPO *f = ((OurXapo *)o)->f; return f->IsInputFormatSupported(f, a, b, c); }
static HRESULT STDMETHODCALLTYPE ox_out(void *o, const FAudioWaveFormatEx *a, const FAudioWaveFormatEx *b, FAudioWaveFormatEx **c)
{ FAPO *f = ((OurXapo *)o)->f; return f->IsOutputFormatSupported(f, a, b, c); }
static HRESULT STDMETHODCALLTYPE ox_init(void *o, const void *d, UINT32 n) { FAPO *f = ((OurXapo *)o)->f; return f->Initialize(f, d, n); }
static void STDMETHODCALLTYPE ox_reset(void *o) { FAPO *f = ((OurXapo *)o)->f; f->Reset(f); }
static HRESULT STDMETHODCALLTYPE ox_lock(void *o, UINT32 ni, const FAPOLockForProcessBufferParameters *i, UINT32 no,
                                         const FAPOLockForProcessBufferParameters *op)
{ FAPO *f = ((OurXapo *)o)->f; return f->LockForProcess(f, ni, i, no, op); }
static void STDMETHODCALLTYPE ox_unlock(void *o) { FAPO *f = ((OurXapo *)o)->f; f->UnlockForProcess(f); }
static void STDMETHODCALLTYPE ox_process(void *o, UINT32 ni, const FAPOProcessBufferParameters *i, UINT32 no,
                                         FAPOProcessBufferParameters *op, BOOL on)
{ FAPO *f = ((OurXapo *)o)->f; f->Process(f, ni, i, no, op, on); }
static UINT32 STDMETHODCALLTYPE ox_calc_in(void *o, UINT32 n) { FAPO *f = ((OurXapo *)o)->f; return f->CalcInputFrames(f, n); }
static UINT32 STDMETHODCALLTYPE ox_calc_out(void *o, UINT32 n) { FAPO *f = ((OurXapo *)o)->f; return f->CalcOutputFrames(f, n); }
static const IXAPOVtbl ox_vtbl = {
    ox_qi, ox_addref, ox_release, ox_props, ox_in, ox_out, ox_init, ox_reset, ox_lock, ox_unlock, ox_process,
    ox_calc_in, ox_calc_out,
};

static HRESULT STDMETHODCALLTYPE op_qi(void *p, REFIID riid, void **ppv) { return ox_qi(FROM_P(p), riid, ppv); }
static ULONG STDMETHODCALLTYPE op_addref(void *p) { return ox_addref(FROM_P(p)); }
static ULONG STDMETHODCALLTYPE op_release(void *p) { return ox_release(FROM_P(p)); }
static void STDMETHODCALLTYPE op_set(void *p, const void *d, UINT32 n) { FAPO *f = FROM_P(p)->f; f->SetParameters(f, d, n); }
static void STDMETHODCALLTYPE op_get(void *p, void *d, UINT32 n) { FAPO *f = FROM_P(p)->f; f->GetParameters(f, d, n); }
static const IXAPOParametersVtbl op_vtbl = { op_qi, op_addref, op_release, op_set, op_get };

HRESULT fapo_to_xapo(FAPO *f, IUnknown **out)
{
    OurXapo *x = CoTaskMemAlloc(sizeof(*x));
    if (!x) { f->Release(f); return E_OUTOFMEMORY; }
    x->v = &ox_vtbl;
    x->pv = &op_vtbl;
    x->refs = 1;
    x->f = f;
    *out = (IUnknown *)x;
    return S_OK;
}

FAPO *xapo_to_fapo(IUnknown *u)
{
    if (!u) return 0;
    if (*(const void **)u == &ox_vtbl) {
        FAPO *f = ((OurXapo *)u)->f;
        f->AddRef(f);
        return f;
    }
    IXAPO *x = 0;
    if (FAILED(u->lpVtbl->QueryInterface(u, &IID_IXAPO, (void **)&x)) &&
        FAILED(u->lpVtbl->QueryInterface(u, &IID_IXAPO27, (void **)&x)))
        return 0;
    AppFapo *a = CoTaskMemAlloc(sizeof(*a));
    if (!a) { x->v->Release(x); return 0; }
    memset(a, 0, sizeof(*a));
    a->f = (FAPO){ af_addref, af_release, af_props, af_in, af_out, af_init, af_reset, af_lock, af_unlock, af_process,
                   af_calc_in, af_calc_out, af_set, af_get };
    a->refs = 1;
    a->x = x;
    if (FAILED(u->lpVtbl->QueryInterface(u, &IID_IXAPOParameters, (void **)&a->p)))
        u->lpVtbl->QueryInterface(u, &IID_IXAPOParameters27, (void **)&a->p);
    return &a->f;
}
