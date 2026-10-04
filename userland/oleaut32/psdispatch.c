/*
 * psdispatch.c — IDispatch between processes: the proxy and stub of
 * Automation's proxy/stub class (PSDispatch, {00020420-...}), which
 * ole32 uses for IDispatch itself and which MIDL-made proxies of dual
 * interfaces (Edge Update's psmachine.dll) delegate IDispatch's four
 * methods to.
 *
 * Each method travels as one request in NovaOS's own wire form, with the
 * method's number (3 GetTypeInfoCount, 4 GetTypeInfo, 5 GetIDsOfNames, 6
 * Invoke) as the RPCOLEMESSAGE's iMethod.  Invoke sends its arguments as
 * VARIANTs (usrmarshal.c: BSTRs by value, interface pointers marshaled for
 * the destination, VT_BYREF arguments by what they point to) and brings
 * back the result, the EXCEPINFO of DISP_E_EXCEPTION, the bad argument's
 * index and the new values of the VT_BYREF arguments, which are stored
 * where the caller's point.  GetTypeInfo's ITypeInfo comes back only if
 * ITypeInfo itself can be marshaled (no proxy for it yet, so it fails).
 *
 * PSOAInterface ({00020424-...}, the type library marshaler that
 * RegisterTypeLib names for [oleautomation] and dual interfaces) serves
 * IDispatch the same way; other interfaces it would marshal from their
 * type information are not done (E_NOINTERFACE).
 */

#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>
#include "usrmarshal.h"

static const CLSID CLSID_PSDispatch_ = { 0x00020420, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const CLSID CLSID_PSOAInterface_ = { 0x00020424, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

enum { M_GETTYPEINFOCOUNT = 3, M_GETTYPEINFO, M_GETIDSOFNAMES, M_INVOKE };
#define RPC_X_BAD_STUB_DATA_HR ((HRESULT)0x800706F7L)
#define RPC_E_INVALIDMETHOD_   ((HRESULT)0x80010104L)

/* ---- building a request or reply in two passes -------------------------- */
typedef void (*BuildFn)(WireBuf *w, void *ctx);

/* the message for msg: sized, then written into the channel's buffer */
static HRESULT build_msg(IRpcChannelBuffer *chan, RPCOLEMESSAGE *msg, REFIID iid, BuildFn fn, void *ctx)
{
    DWORD dest = MSHCTX_LOCAL;
    void *pv = 0;
    chan->lpVtbl->GetDestCtx(chan, &dest, &pv);
    WireBuf w;
    ZeroMemory(&w, sizeof w);
    w.ctx = dest;
    fn(&w, ctx);
    if (w.fail) return RPC_X_BAD_STUB_DATA_HR;
    msg->cbBuffer = w.size;
    HRESULT hr = chan->lpVtbl->GetBuffer(chan, msg, iid);
    if (FAILED(hr)) return hr;
    ZeroMemory(&w, sizeof w);
    w.ctx = dest;
    w.buf = w.start = msg->Buffer;
    fn(&w, ctx);
    msg->cbBuffer = (ULONG)(w.buf - w.start);
    return w.fail ? RPC_X_BAD_STUB_DATA_HR : S_OK;
}

static WireBuf reader(RPCOLEMESSAGE *msg)
{
    WireBuf r;
    ZeroMemory(&r, sizeof r);
    r.buf = r.start = msg->Buffer;
    r.end = r.start + msg->cbBuffer;
    return r;
}

/* ---- the calls' arguments ----------------------------------------------- */
typedef struct {
    /* GetTypeInfo */
    UINT index;
    LCID lcid;
    ITypeInfo *ti;
    /* GetIDsOfNames */
    const IID *riid;
    LPOLESTR *names;
    UINT nnames;
    DISPID *ids;
    /* Invoke */
    DISPID dispid;
    WORD flags;
    DISPPARAMS *dp;
    VARIANT *result;
    EXCEPINFO *ei;
    UINT *argerr;
    /* replies */
    HRESULT hr;
    UINT count;
} Call;

static void put_wstr(WireBuf *w, const WCHAR *s)
{
    ULONG n = 0;
    if (s) while (s[n]) n++;
    wb_u32(w, s ? n : 0xffffffffu);
    if (s) wb_put(w, s, n * sizeof(WCHAR));
}
static WCHAR *get_wstr(WireBuf *r)
{
    ULONG n = rb_u32(r);
    if (n == 0xffffffffu || r->fail) return 0;
    if (n > 0x100000 || (ULONG_PTR)(r->end - r->buf) < n * sizeof(WCHAR)) { r->fail = 1; return 0; }
    WCHAR *s = CoTaskMemAlloc((n + 1) * sizeof(WCHAR));
    if (!s) { r->fail = 1; return 0; }
    rb_get(r, s, n * sizeof(WCHAR));
    s[n] = 0;
    return s;
}

static void req_gettypeinfo(WireBuf *w, void *ctx) { Call *c = ctx; wb_u32(w, c->index); wb_u32(w, c->lcid); }
static void req_getids(WireBuf *w, void *ctx)
{
    Call *c = ctx;
    wb_put(w, c->riid, 16);
    wb_u32(w, c->nnames);
    wb_u32(w, c->lcid);
    for (UINT i = 0; i < c->nnames; i++) put_wstr(w, c->names[i]);
}
static void req_invoke(WireBuf *w, void *ctx)
{
    Call *c = ctx;
    DISPPARAMS *dp = c->dp;
    wb_u32(w, (ULONG)c->dispid);
    wb_put(w, c->riid, 16);
    wb_u32(w, c->lcid);
    wb_u32(w, c->flags);
    wb_u32(w, dp ? dp->cArgs : 0);
    wb_u32(w, dp ? dp->cNamedArgs : 0);
    wb_u32(w, (c->result ? 1 : 0) | (c->ei ? 2 : 0) | (c->argerr ? 4 : 0));
    for (UINT i = 0; dp && i < dp->cNamedArgs; i++) wb_u32(w, (ULONG)dp->rgdispidNamedArgs[i]);
    for (UINT i = 0; dp && i < dp->cArgs; i++) wire_variant(w, &dp->rgvarg[i]);
}

/* ---- the proxy ------------------------------------------------------------ */
typedef struct {
    IRpcProxyBuffer buf;
    IDispatch disp;
    LONG refs;
    IUnknown *outer;
    IRpcChannelBuffer *chan;
} DispProxy;
#define PX_FROM_DISP(p) ((DispProxy *)((BYTE *)(p) - offsetof(DispProxy, disp)))

static HRESULT STDMETHODCALLTYPE pb_qi(IRpcProxyBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRpcProxyBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE pb_addref(IRpcProxyBuffer *This) { return (ULONG)InterlockedIncrement(&((DispProxy *)This)->refs); }
static ULONG STDMETHODCALLTYPE pb_release(IRpcProxyBuffer *This)
{
    DispProxy *p = (DispProxy *)This;
    LONG r = InterlockedDecrement(&p->refs);
    if (!r) {
        if (p->chan) p->chan->lpVtbl->Release(p->chan);
        CoTaskMemFree(p);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE pb_connect(IRpcProxyBuffer *This, IRpcChannelBuffer *chan)
{
    DispProxy *p = (DispProxy *)This;
    if (p->chan) return E_UNEXPECTED;
    if (!chan) return E_INVALIDARG;
    chan->lpVtbl->AddRef(chan);
    p->chan = chan;
    return S_OK;
}
static void STDMETHODCALLTYPE pb_disconnect(IRpcProxyBuffer *This)
{
    DispProxy *p = (DispProxy *)This;
    IRpcChannelBuffer *c = (IRpcChannelBuffer *)InterlockedExchangePointer((void **)&p->chan, 0);
    if (c) c->lpVtbl->Release(c);
}
static const IRpcProxyBufferVtbl g_pb_vtbl = { pb_qi, pb_addref, pb_release, pb_connect, pb_disconnect };

static HRESULT STDMETHODCALLTYPE pd_qi(IDispatch *This, REFIID riid, void **ppv) { DispProxy *p = PX_FROM_DISP(This); return p->outer->lpVtbl->QueryInterface(p->outer, riid, ppv); }
static ULONG STDMETHODCALLTYPE pd_addref(IDispatch *This) { DispProxy *p = PX_FROM_DISP(This); return p->outer->lpVtbl->AddRef(p->outer); }
static ULONG STDMETHODCALLTYPE pd_release(IDispatch *This) { DispProxy *p = PX_FROM_DISP(This); return p->outer->lpVtbl->Release(p->outer); }

/* one call: the request, then the reply handed to parse */
static HRESULT px_call(DispProxy *p, ULONG method, BuildFn build, Call *c, HRESULT (*parse)(WireBuf *r, Call *c))
{
    IRpcChannelBuffer *chan = p->chan;
    if (!chan) return CO_E_OBJNOTCONNECTED;
    chan->lpVtbl->AddRef(chan);
    RPCOLEMESSAGE msg;
    ZeroMemory(&msg, sizeof msg);
    msg.iMethod = method;
    HRESULT hr = build_msg(chan, &msg, &IID_IDispatch, build, c);
    if (SUCCEEDED(hr)) {
        ULONG status = 0;
        hr = chan->lpVtbl->SendReceive(chan, &msg, &status);
        if (SUCCEEDED(hr)) {
            WireBuf r = reader(&msg);
            hr = parse(&r, c);
            if (SUCCEEDED(hr) && r.fail) hr = r.hr ? r.hr : RPC_X_BAD_STUB_DATA_HR;
        }
    }
    chan->lpVtbl->FreeBuffer(chan, &msg);
    chan->lpVtbl->Release(chan);
    return hr;
}

static void req_none(WireBuf *w, void *ctx) { (void)w; (void)ctx; }
static HRESULT rep_count(WireBuf *r, Call *c) { c->hr = (HRESULT)rb_u32(r); c->count = rb_u32(r); return S_OK; }
static HRESULT STDMETHODCALLTYPE pd_count(IDispatch *This, UINT *n)
{
    if (!n) return E_INVALIDARG;
    Call c;
    ZeroMemory(&c, sizeof c);
    HRESULT hr = px_call(PX_FROM_DISP(This), M_GETTYPEINFOCOUNT, req_none, &c, rep_count);
    if (FAILED(hr)) return hr;
    *n = c.count;
    return c.hr;
}

static HRESULT rep_typeinfo(WireBuf *r, Call *c)
{
    c->hr = (HRESULT)rb_u32(r);
    if (SUCCEEDED(c->hr)) c->ti = (ITypeInfo *)wire_get_iface(r, &IID_ITypeInfo);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE pd_typeinfo(IDispatch *This, UINT i, LCID lcid, ITypeInfo **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    Call c;
    ZeroMemory(&c, sizeof c);
    c.index = i;
    c.lcid = lcid;
    HRESULT hr = px_call(PX_FROM_DISP(This), M_GETTYPEINFO, req_gettypeinfo, &c, rep_typeinfo);
    if (FAILED(hr)) return hr;
    *out = c.ti;
    return c.hr;
}

static HRESULT rep_getids(WireBuf *r, Call *c)
{
    c->hr = (HRESULT)rb_u32(r);
    for (UINT i = 0; i < c->nnames; i++) c->ids[i] = (DISPID)rb_u32(r);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE pd_getids(IDispatch *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids)
{
    if (!names || !ids || !riid) return E_INVALIDARG;
    Call c;
    ZeroMemory(&c, sizeof c);
    c.riid = riid;
    c.names = names;
    c.nnames = n;
    c.lcid = lcid;
    c.ids = ids;
    HRESULT hr = px_call(PX_FROM_DISP(This), M_GETIDSOFNAMES, req_getids, &c, rep_getids);
    return FAILED(hr) ? hr : c.hr;
}

static ULONG scalar_bytes(VARTYPE vt)
{
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4: case VT_ERROR: case VT_HRESULT: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: return 8;
    default: return 0;
    }
}

/* the callee's new value of a VT_BYREF argument, stored where the
 * caller's argument points (what was there before is released) */
static void store_byref(VARIANT *arg, VARIANT *v)
{
    VARTYPE base = V_VT(arg) & ~VT_BYREF;
    void *p = arg->byref;
    if (!p) { VariantClear(v); return; }
    if (base == VT_VARIANT) { VariantClear((VARIANT *)p); *(VARIANT *)p = *v; V_VT(v) = VT_EMPTY; return; }
    if (V_VT(v) != base) { VariantClear(v); return; }
    if (base & VT_ARRAY) { if (*(SAFEARRAY **)p) SafeArrayDestroy(*(SAFEARRAY **)p); *(SAFEARRAY **)p = v->parray; }
    else if (base == VT_BSTR) { SysFreeString(*(BSTR *)p); *(BSTR *)p = v->bstrVal; }
    else if (base == VT_UNKNOWN || base == VT_DISPATCH) {
        if (*(IUnknown **)p) (*(IUnknown **)p)->lpVtbl->Release(*(IUnknown **)p);
        *(IUnknown **)p = v->punkVal;
    } else if (base == VT_DECIMAL) *(DECIMAL *)p = v->decVal;
    else CopyMemory(p, &v->llVal, scalar_bytes(base));
    V_VT(v) = VT_EMPTY;
}

static HRESULT rep_invoke(WireBuf *r, Call *c)
{
    c->hr = (HRESULT)rb_u32(r);
    UINT argerr = rb_u32(r);
    if (c->argerr) *c->argerr = argerr;
    if (rb_u32(r)) {
        VARIANT v;
        ZeroMemory(&v, sizeof v);
        wire_get_variant(r, &v);
        if (c->result) { VariantClear(c->result); *c->result = v; }
        else VariantClear(&v);
    }
    if (rb_u32(r)) {
        EXCEPINFO e;
        ZeroMemory(&e, sizeof e);
        e.wCode = (WORD)rb_u32(r);
        e.scode = (HRESULT)rb_u32(r);
        e.dwHelpContext = rb_u32(r);
        e.bstrSource = wire_get_bstr(r);
        e.bstrDescription = wire_get_bstr(r);
        e.bstrHelpFile = wire_get_bstr(r);
        if (c->ei) *c->ei = e;
        else { SysFreeString(e.bstrSource); SysFreeString(e.bstrDescription); SysFreeString(e.bstrHelpFile); }
    }
    ULONG n = rb_u32(r);
    for (ULONG i = 0; i < n && !r->fail; i++) {
        ULONG at = rb_u32(r);
        VARIANT v;
        ZeroMemory(&v, sizeof v);
        wire_get_variant(r, &v);
        if (c->dp && at < c->dp->cArgs && (V_VT(&c->dp->rgvarg[at]) & VT_BYREF)) store_byref(&c->dp->rgvarg[at], &v);
        else VariantClear(&v);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE pd_invoke(IDispatch *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *dp,
                                           VARIANT *result, EXCEPINFO *ei, UINT *argerr)
{
    Call c;
    ZeroMemory(&c, sizeof c);
    c.dispid = id;
    c.riid = riid ? riid : &IID_NULL;
    c.lcid = lcid;
    c.flags = flags;
    c.dp = dp;
    c.result = result;
    c.ei = ei;
    c.argerr = argerr;
    if (ei) ZeroMemory(ei, sizeof *ei);
    HRESULT hr = px_call(PX_FROM_DISP(This), M_INVOKE, req_invoke, &c, rep_invoke);
    return FAILED(hr) ? hr : c.hr;
}
static const IDispatchVtbl g_pd_vtbl = { pd_qi, pd_addref, pd_release, pd_count, pd_typeinfo, pd_getids, pd_invoke };

/* ---- the stub ------------------------------------------------------------- */
typedef struct { IRpcStubBuffer iface; LONG refs; IDispatch *server; } DispStub;

static HRESULT STDMETHODCALLTYPE sb_qi(IRpcStubBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRpcStubBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sb_addref(IRpcStubBuffer *This) { return (ULONG)InterlockedIncrement(&((DispStub *)This)->refs); }
static ULONG STDMETHODCALLTYPE sb_release(IRpcStubBuffer *This)
{
    DispStub *s = (DispStub *)This;
    LONG r = InterlockedDecrement(&s->refs);
    if (!r) {
        if (s->server) s->server->lpVtbl->Release(s->server);
        CoTaskMemFree(s);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE sb_connect(IRpcStubBuffer *This, IUnknown *server)
{
    DispStub *s = (DispStub *)This;
    IDispatch *d = 0;
    if (!server) return E_INVALIDARG;
    HRESULT hr = server->lpVtbl->QueryInterface(server, &IID_IDispatch, (void **)&d);
    if (FAILED(hr)) return hr;
    d = (IDispatch *)InterlockedExchangePointer((void **)&s->server, d);
    if (d) d->lpVtbl->Release(d);
    return S_OK;
}
static void STDMETHODCALLTYPE sb_disconnect(IRpcStubBuffer *This)
{
    DispStub *s = (DispStub *)This;
    IDispatch *d = (IDispatch *)InterlockedExchangePointer((void **)&s->server, 0);
    if (d) d->lpVtbl->Release(d);
}

typedef struct {
    Call c;
    VARIANT *args;
    DISPID *named;
    DISPPARAMS dp;
    VARIANT result;
    EXCEPINFO ei;
    UINT argerr;
    ULONG want;
} InvokeReply;

static void rep_out_count(WireBuf *w, void *ctx) { Call *c = ctx; wb_u32(w, (ULONG)c->hr); wb_u32(w, c->count); }
static void rep_out_typeinfo(WireBuf *w, void *ctx)
{
    Call *c = ctx;
    wb_u32(w, (ULONG)c->hr);
    if (SUCCEEDED(c->hr)) wire_iface(w, (IUnknown *)c->ti, &IID_ITypeInfo);
}
static void rep_out_getids(WireBuf *w, void *ctx)
{
    Call *c = ctx;
    wb_u32(w, (ULONG)c->hr);
    for (UINT i = 0; i < c->nnames; i++) wb_u32(w, (ULONG)c->ids[i]);
}
static void rep_out_invoke(WireBuf *w, void *ctx)
{
    InvokeReply *x = ctx;
    wb_u32(w, (ULONG)x->c.hr);
    wb_u32(w, x->argerr);
    wb_u32(w, (x->want & 1) != 0);
    if (x->want & 1) wire_variant(w, &x->result);
    BOOL excep = (x->want & 2) && x->c.hr == DISP_E_EXCEPTION;
    wb_u32(w, excep);
    if (excep) {
        wb_u32(w, x->ei.wCode);
        wb_u32(w, (ULONG)x->ei.scode);
        wb_u32(w, x->ei.dwHelpContext);
        wire_bstr(w, x->ei.bstrSource);
        wire_bstr(w, x->ei.bstrDescription);
        wire_bstr(w, x->ei.bstrHelpFile);
    }
    ULONG n = 0;
    for (UINT i = 0; i < x->dp.cArgs; i++) if ((V_VT(&x->args[i]) & VT_BYREF) && x->args[i].byref) n++;
    wb_u32(w, n);
    for (UINT i = 0; i < x->dp.cArgs; i++) {
        VARIANT *a = &x->args[i];
        if (!(V_VT(a) & VT_BYREF) || !a->byref) continue;
        wb_u32(w, i);
        VARTYPE base = V_VT(a) & ~VT_BYREF;
        if (base == VT_VARIANT) { wire_variant(w, a->pvarVal); continue; }
        VARIANT v;                                  /* the value, not owned */
        ZeroMemory(&v, sizeof v);
        V_VT(&v) = base;
        if (base == VT_DECIMAL) { v.decVal = *a->pdecVal; V_VT(&v) = VT_DECIMAL; }
        else if (base & VT_ARRAY) v.parray = *a->pparray;
        else if (base == VT_BSTR) v.bstrVal = *a->pbstrVal;
        else if (base == VT_UNKNOWN || base == VT_DISPATCH) v.punkVal = *a->ppunkVal;
        else CopyMemory(&v.llVal, a->byref, scalar_bytes(base));
        wire_variant(w, &v);
    }
}

static HRESULT server_invoke(IDispatch *d, InvokeReply *x, VARIANT *res, EXCEPINFO *ei, UINT *ae)
{
    HRESULT hr;
    __try {
        hr = d->lpVtbl->Invoke(d, x->c.dispid, x->c.riid, x->c.lcid, x->c.flags, &x->dp, res, ei, ae);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = RPC_E_SERVERFAULT;
    }
    return hr;
}

static HRESULT stub_invoke(DispStub *s, RPCOLEMESSAGE *msg, IRpcChannelBuffer *chan)
{
    IDispatch *d = s->server;
    if (!d) return CO_E_OBJNOTCONNECTED;
    WireBuf r = reader(msg);
    HRESULT hr = S_OK;
    switch (msg->iMethod) {
    case M_GETTYPEINFOCOUNT: {
        Call c;
        ZeroMemory(&c, sizeof c);
        c.hr = d->lpVtbl->GetTypeInfoCount(d, &c.count);
        return build_msg(chan, msg, &IID_IDispatch, rep_out_count, &c);
    }
    case M_GETTYPEINFO: {
        Call c;
        ZeroMemory(&c, sizeof c);
        c.index = rb_u32(&r);
        c.lcid = rb_u32(&r);
        if (r.fail) return RPC_X_BAD_STUB_DATA_HR;
        c.hr = d->lpVtbl->GetTypeInfo(d, c.index, c.lcid, &c.ti);
        hr = build_msg(chan, msg, &IID_IDispatch, rep_out_typeinfo, &c);
        if (c.ti) c.ti->lpVtbl->Release(c.ti);
        return hr;
    }
    case M_GETIDSOFNAMES: {
        Call c;
        IID iid;
        ZeroMemory(&c, sizeof c);
        rb_get(&r, &iid, 16);
        c.riid = &iid;
        c.nnames = rb_u32(&r);
        c.lcid = rb_u32(&r);
        if (r.fail || c.nnames > 4096) return RPC_X_BAD_STUB_DATA_HR;
        c.names = CoTaskMemAlloc((c.nnames + 1) * sizeof(LPOLESTR));
        c.ids = CoTaskMemAlloc((c.nnames + 1) * sizeof(DISPID));
        if (!c.names || !c.ids) hr = E_OUTOFMEMORY;
        for (UINT i = 0; SUCCEEDED(hr) && i < c.nnames; i++) c.names[i] = get_wstr(&r);
        if (SUCCEEDED(hr) && r.fail) hr = RPC_X_BAD_STUB_DATA_HR;
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < c.nnames; i++) c.ids[i] = DISPID_UNKNOWN;
            c.hr = d->lpVtbl->GetIDsOfNames(d, c.riid, c.names, c.nnames, c.lcid, c.ids);
            hr = build_msg(chan, msg, &IID_IDispatch, rep_out_getids, &c);
        }
        if (c.names) for (UINT i = 0; i < c.nnames; i++) CoTaskMemFree(c.names[i]);
        CoTaskMemFree(c.names);
        CoTaskMemFree(c.ids);
        return hr;
    }
    case M_INVOKE: {
        InvokeReply x;
        IID iid;
        ZeroMemory(&x, sizeof x);
        x.c.dispid = (DISPID)rb_u32(&r);
        rb_get(&r, &iid, 16);
        x.c.riid = &iid;
        x.c.lcid = rb_u32(&r);
        x.c.flags = (WORD)rb_u32(&r);
        UINT nargs = rb_u32(&r), nnamed = rb_u32(&r);
        x.want = rb_u32(&r);
        if (r.fail || nargs > 1024 || nnamed > nargs) return RPC_X_BAD_STUB_DATA_HR;
        x.args = CoTaskMemAlloc((nargs + 1) * sizeof(VARIANT));
        x.named = CoTaskMemAlloc((nnamed + 1) * sizeof(DISPID));
        if (!x.args || !x.named) { CoTaskMemFree(x.args); CoTaskMemFree(x.named); return E_OUTOFMEMORY; }
        ZeroMemory(x.args, (nargs + 1) * sizeof(VARIANT));
        for (UINT i = 0; i < nnamed; i++) x.named[i] = (DISPID)rb_u32(&r);
        UINT got = 0;
        for (; got < nargs && !r.fail; got++) wire_get_variant(&r, &x.args[got]);
        if (r.fail) hr = r.hr ? r.hr : RPC_X_BAD_STUB_DATA_HR;
        if (SUCCEEDED(hr)) {
            x.dp.rgvarg = nargs ? x.args : 0;
            x.dp.rgdispidNamedArgs = nnamed ? x.named : 0;
            x.dp.cArgs = nargs;
            x.dp.cNamedArgs = nnamed;
            x.c.hr = server_invoke(d, &x, (x.want & 1) ? &x.result : 0, (x.want & 2) ? &x.ei : 0, (x.want & 4) ? &x.argerr : 0);
            if (x.c.hr == DISP_E_EXCEPTION && x.ei.pfnDeferredFillIn) { x.ei.pfnDeferredFillIn(&x.ei); x.ei.pfnDeferredFillIn = 0; }
            hr = build_msg(chan, msg, &IID_IDispatch, rep_out_invoke, &x);
        }
        for (UINT i = 0; i < got; i++) wire_free_variant(&x.args[i]);
        VariantClear(&x.result);
        SysFreeString(x.ei.bstrSource);
        SysFreeString(x.ei.bstrDescription);
        SysFreeString(x.ei.bstrHelpFile);
        CoTaskMemFree(x.args);
        CoTaskMemFree(x.named);
        return hr;
    }
    }
    return RPC_E_INVALIDMETHOD_;
}

static HRESULT STDMETHODCALLTYPE sb_invoke(IRpcStubBuffer *This, RPCOLEMESSAGE *msg, IRpcChannelBuffer *chan)
{
    DispStub *s = (DispStub *)This;
    HRESULT hr;
    sb_addref(This);
    hr = stub_invoke(s, msg, chan);
    sb_release(This);
    return hr;
}
static IRpcStubBuffer *STDMETHODCALLTYPE sb_supported(IRpcStubBuffer *This, REFIID riid)
{
    if (!IsEqualIID(riid, &IID_IDispatch)) return 0;
    sb_addref(This);
    return This;
}
static ULONG STDMETHODCALLTYPE sb_countrefs(IRpcStubBuffer *This) { return ((DispStub *)This)->server ? 1 : 0; }
static HRESULT STDMETHODCALLTYPE sb_debugqi(IRpcStubBuffer *This, void **ppv)
{
    DispStub *s = (DispStub *)This;
    *ppv = s->server;
    return s->server ? S_OK : CO_E_OBJNOTCONNECTED;
}
static void STDMETHODCALLTYPE sb_debugrelease(IRpcStubBuffer *This, void *pv) { (void)This; (void)pv; }
static const IRpcStubBufferVtbl g_sb_vtbl = {
    sb_qi, sb_addref, sb_release, sb_connect, sb_disconnect, sb_invoke, sb_supported, sb_countrefs, sb_debugqi, sb_debugrelease,
};

/* ---- the class object: an IPSFactoryBuffer ------------------------------ */
static HRESULT STDMETHODCALLTYPE ps_qi(IPSFactoryBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPSFactoryBuffer)) { *ppv = This; return S_OK; }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ps_addref(IPSFactoryBuffer *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE ps_release(IPSFactoryBuffer *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE ps_proxy(IPSFactoryBuffer *This, IUnknown *outer, REFIID riid, IRpcProxyBuffer **proxy, void **ppv)
{
    (void)This;
    if (!proxy || !ppv) return E_POINTER;
    *proxy = 0;
    *ppv = 0;
    if (!outer) return E_INVALIDARG;
    if (!IsEqualIID(riid, &IID_IDispatch)) return E_NOINTERFACE;
    DispProxy *p = CoTaskMemAlloc(sizeof *p);
    if (!p) return E_OUTOFMEMORY;
    ZeroMemory(p, sizeof *p);
    p->buf.lpVtbl = &g_pb_vtbl;
    p->disp.lpVtbl = &g_pd_vtbl;
    p->refs = 1;
    p->outer = outer;
    outer->lpVtbl->AddRef(outer);                   /* the interface handed out counts on the outer object */
    *proxy = &p->buf;
    *ppv = &p->disp;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ps_stub(IPSFactoryBuffer *This, REFIID riid, IUnknown *server, IRpcStubBuffer **stub)
{
    (void)This;
    if (!stub) return E_POINTER;
    *stub = 0;
    if (!IsEqualIID(riid, &IID_IDispatch)) return E_NOINTERFACE;
    DispStub *s = CoTaskMemAlloc(sizeof *s);
    if (!s) return E_OUTOFMEMORY;
    ZeroMemory(s, sizeof *s);
    s->iface.lpVtbl = &g_sb_vtbl;
    s->refs = 1;
    if (server) {
        HRESULT hr = sb_connect(&s->iface, server);
        if (FAILED(hr)) { CoTaskMemFree(s); return hr; }
    }
    *stub = &s->iface;
    return S_OK;
}
static const IPSFactoryBufferVtbl g_ps_vtbl = { ps_qi, ps_addref, ps_release, ps_proxy, ps_stub };
static IPSFactoryBuffer g_ps = { &g_ps_vtbl };

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (!IsEqualCLSID(clsid, &CLSID_PSDispatch_) && !IsEqualCLSID(clsid, &CLSID_PSOAInterface_)) return CLASS_E_CLASSNOTAVAILABLE;
    return ps_qi(&g_ps, riid, ppv);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
