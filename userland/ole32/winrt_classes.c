/*
 * The few Windows Runtime classes NovaOS answers for.  A class Windows
 * always has must activate even when NovaOS has nothing behind it: a
 * program asks it whether the feature is there, and only then decides not
 * to use it.  Every object here is static and agile (usable from any
 * thread), so reference counts are kept only for show.
 *
 * Windows.Security.Credentials.KeyCredentialManager (Windows Hello):
 * IsSupportedAsync completes at once with false.  KeePassXC asks it from a
 * PPL task when it starts; a failed activation there is an exception no
 * one observes, which ends the program.
 */
#define NOVA_BUILD_OLE32
#include <objbase.h>

typedef struct HSTRING_ *HSTRING;
HRESULT WINAPI WindowsCreateString(const WCHAR *src, UINT32 len, HSTRING *out);     /* winrt.c */

static const GUID IID_IUnknown_w       = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IInspectable_w   = { 0xAF86E2E0, 0xB12D, 0x4C6A, { 0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90 } };
static const GUID IID_IAgileObject_w   = { 0x94EA2B94, 0xE9CC, 0x49E0, { 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90 } };
static const GUID IID_IActivationFactory_w = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IAsyncInfo_w     = { 0x00000036, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
/* IAsyncOperation<Boolean> */
static const GUID IID_IAsyncOpBool_w   = { 0xCDB5EFB3, 0x5788, 0x509D, { 0x9B, 0xE1, 0x71, 0xCC, 0xB8, 0xA3, 0x36, 0x2A } };
static const GUID IID_IKeyCredentialManagerStatics_w = { 0x6AAC468B, 0x0EF1, 0x4CE0, { 0x82, 0x90, 0x41, 0x06, 0xDA, 0x6A, 0x63, 0xB5 } };

#define WRT_E_NOTIMPL ((HRESULT)0x80004001L)
enum { AsyncCompleted = 1 };

static HRESULT STDMETHODCALLTYPE no_iids(void *o, ULONG *n, GUID **iids) { (void)o; *n = 0; *iids = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE base_trust(void *o, int *t) { (void)o; *t = 0; return S_OK; }   /* BaseTrust */
static ULONG STDMETHODCALLTYPE static_ref(void *o) { (void)o; return 2; }

/* ---- IAsyncOperation<bool>, already completed with false ---------------- */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, GUID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
} InspVtbl;

typedef struct { InspVtbl i; HRESULT (STDMETHODCALLTYPE *put_Completed)(void *, IUnknown *);
                 HRESULT (STDMETHODCALLTYPE *get_Completed)(void *, IUnknown **);
                 HRESULT (STDMETHODCALLTYPE *GetResults)(void *, BOOLEAN *); } OpBoolVtbl;
typedef struct { InspVtbl i; HRESULT (STDMETHODCALLTYPE *get_Id)(void *, UINT32 *);
                 HRESULT (STDMETHODCALLTYPE *get_Status)(void *, int *);
                 HRESULT (STDMETHODCALLTYPE *get_ErrorCode)(void *, HRESULT *);
                 HRESULT (STDMETHODCALLTYPE *Cancel)(void *);
                 HRESULT (STDMETHODCALLTYPE *Close)(void *); } AsyncInfoVtbl;

typedef struct { const OpBoolVtbl *op; const AsyncInfoVtbl *info; } FalseOp;
static FalseOp g_false_op;

static HRESULT STDMETHODCALLTYPE op_qi(void *o, REFIID iid, void **out)
{
    (void)o;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_w) || IsEqualGUID(iid, &IID_IInspectable_w) ||
        IsEqualGUID(iid, &IID_IAgileObject_w) || IsEqualGUID(iid, &IID_IAsyncOpBool_w)) { *out = &g_false_op.op; return S_OK; }
    if (IsEqualGUID(iid, &IID_IAsyncInfo_w)) { *out = &g_false_op.info; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE op_name(void *o, HSTRING *n)
{
    (void)o;
    static const WCHAR name[] = L"Windows.Foundation.IAsyncOperation`1<Boolean>";
    return WindowsCreateString(name, (UINT32)(sizeof(name) / sizeof(WCHAR) - 1), n);
}
/* The handler (AsyncOperationCompletedHandler<bool>) is called at once:
 * Invoke(this, AsyncStatus.Completed) is the slot after IUnknown's */
static HRESULT STDMETHODCALLTYPE op_put_completed(void *o, IUnknown *h)
{
    (void)o;
    if (!h) return E_POINTER;
    typedef HRESULT (STDMETHODCALLTYPE *Invoke)(IUnknown *, void *, int);
    Invoke inv = (Invoke)((void **)h->lpVtbl)[3];
    inv(h, &g_false_op.op, AsyncCompleted);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE op_get_completed(void *o, IUnknown **h) { (void)o; if (h) *h = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE op_results(void *o, BOOLEAN *r) { (void)o; if (!r) return E_POINTER; *r = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE info_id(void *o, UINT32 *id) { (void)o; *id = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE info_status(void *o, int *s) { (void)o; *s = AsyncCompleted; return S_OK; }
static HRESULT STDMETHODCALLTYPE info_error(void *o, HRESULT *e) { (void)o; *e = S_OK; return S_OK; }
static HRESULT STDMETHODCALLTYPE info_nop(void *o) { (void)o; return S_OK; }

static const OpBoolVtbl g_op_vtbl = {
    { op_qi, static_ref, static_ref, no_iids, op_name, base_trust },
    op_put_completed, op_get_completed, op_results,
};
static const AsyncInfoVtbl g_info_vtbl = {
    { op_qi, static_ref, static_ref, no_iids, op_name, base_trust },
    info_id, info_status, info_error, info_nop, info_nop,
};
static FalseOp g_false_op = { &g_op_vtbl, &g_info_vtbl };

/* ---- KeyCredentialManager statics -------------------------------------- */
typedef struct { InspVtbl i;
                 HRESULT (STDMETHODCALLTYPE *IsSupportedAsync)(void *, void **);
                 HRESULT (STDMETHODCALLTYPE *RenewAttestationAsync)(void *, void **);
                 HRESULT (STDMETHODCALLTYPE *RequestCreateAsync)(void *, HSTRING, int, void **);
                 HRESULT (STDMETHODCALLTYPE *OpenAsync)(void *, HSTRING, void **);
                 HRESULT (STDMETHODCALLTYPE *DeleteAsync)(void *, HSTRING, void **); } KcmVtbl;
typedef struct { const KcmVtbl *v; } Kcm;
static Kcm g_kcm;

static HRESULT STDMETHODCALLTYPE kcm_qi(void *o, REFIID iid, void **out)
{
    (void)o;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_w) || IsEqualGUID(iid, &IID_IInspectable_w) ||
        IsEqualGUID(iid, &IID_IAgileObject_w) || IsEqualGUID(iid, &IID_IActivationFactory_w) ||
        IsEqualGUID(iid, &IID_IKeyCredentialManagerStatics_w)) { *out = &g_kcm; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE kcm_name(void *o, HSTRING *n)
{
    (void)o;
    static const WCHAR name[] = L"Windows.Security.Credentials.KeyCredentialManager";
    return WindowsCreateString(name, (UINT32)(sizeof(name) / sizeof(WCHAR) - 1), n);
}
static HRESULT STDMETHODCALLTYPE kcm_supported(void *o, void **op) { (void)o; if (!op) return E_POINTER; *op = &g_false_op.op; return S_OK; }
static HRESULT STDMETHODCALLTYPE kcm_renew(void *o, void **op) { (void)o; if (op) *op = 0; return WRT_E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE kcm_create(void *o, HSTRING n, int opt, void **op) { (void)o; (void)n; (void)opt; if (op) *op = 0; return WRT_E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE kcm_open(void *o, HSTRING n, void **op) { (void)o; (void)n; if (op) *op = 0; return WRT_E_NOTIMPL; }
static const KcmVtbl g_kcm_vtbl = {
    { kcm_qi, static_ref, static_ref, no_iids, kcm_name, base_trust },
    kcm_supported, kcm_renew, kcm_create, kcm_open, kcm_open,
};
static Kcm g_kcm = { &g_kcm_vtbl };

/* The activation factory for runtime class @name (@len characters), or
 * REGDB_E_CLASSNOTREG */
HRESULT ole32_winrt_factory(const WCHAR *name, UINT32 len, REFIID iid, void **out)
{
    static const WCHAR kcm[] = L"Windows.Security.Credentials.KeyCredentialManager";
    UINT32 n = (UINT32)(sizeof(kcm) / sizeof(WCHAR) - 1), i = 0;
    if (len == n) while (i < n && name[i] == kcm[i]) i++;
    if (len == n && i == n) return kcm_qi(&g_kcm, iid, out);
    *out = 0;
    return REGDB_E_CLASSNOTREG;
}
