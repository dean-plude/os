/*
 * invoke.c — calling methods described by a type library: DispCallFunc,
 * ITypeInfo::Invoke (what DispInvoke, CreateStdDispatch and the IDispatch
 * of ATL-style objects come down to) and DispGetIDsOfNames.
 *
 * Invoke turns the DISPPARAMS VARIANTs into the method's own argument
 * types (named arguments, [optional] and [defaultvalue], [in, out] by
 * reference, [retval]), calls through the object's vtable and turns a
 * failing HRESULT into DISP_E_EXCEPTION with the object's error info.
 */
#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>
#include "typelib.h"

#define TLAPI __declspec(dllexport)

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

/* ---------------------------------------------------------------------------
 * DispCallFunc
 * ------------------------------------------------------------------------- */
#ifdef _WIN64
/* DWORD64 nova_call_x64(fn, const DWORD64 *args, n, DWORD64 *xmm0):
 * the first four arguments in RCX/RDX/R8/R9 and XMM0-3 alike (the callee
 * reads whichever its prototype says), the rest on the stack */
DWORD64 nova_call_x64(void *fn, const DWORD64 *args, DWORD64 n, DWORD64 *xmm0);
__asm__(
    ".text\n"
    ".globl nova_call_x64\n"
    ".def nova_call_x64; .scl 2; .type 32; .endef\n"
    ".seh_proc nova_call_x64\n"
    "nova_call_x64:\n\t"
    "pushq %rbp\n\t"
    ".seh_pushreg %rbp\n\t"
    "pushq %rsi\n\t"
    ".seh_pushreg %rsi\n\t"
    "pushq %rdi\n\t"
    ".seh_pushreg %rdi\n\t"
    "subq $48, %rsp\n\t"
    ".seh_stackalloc 48\n\t"
    "movq %rsp, %rbp\n\t"
    ".seh_setframe %rbp, 0\n\t"
    ".seh_endprologue\n\t"
    "movq %r9, 32(%rbp)\n\t"            /* where XMM0 goes */
    "movq %rcx, 40(%rbp)\n\t"           /* the function */
    "movq %rdx, %rsi\n\t"
    "movq %r8, %rax\n\t"
    "cmpq $4, %rax\n\t"
    "jae 1f\n\t"
    "movq $4, %rax\n"
    "1:\n\t"
    "shlq $3, %rax\n\t"
    "addq $15, %rax\n\t"
    "andq $-16, %rax\n\t"
    "subq %rax, %rsp\n\t"
    "movq %rsp, %rdi\n\t"
    "movq %r8, %rcx\n\t"
    "rep movsq\n\t"
    "movq 0(%rsp), %rcx\n\t"
    "movq 8(%rsp), %rdx\n\t"
    "movq 16(%rsp), %r8\n\t"
    "movq 24(%rsp), %r9\n\t"
    "movq %rcx, %xmm0\n\t"
    "movq %rdx, %xmm1\n\t"
    "movq %r8, %xmm2\n\t"
    "movq %r9, %xmm3\n\t"
    "callq *40(%rbp)\n\t"
    "movq 32(%rbp), %rcx\n\t"
    "movq %xmm0, (%rcx)\n\t"
    "leaq 48(%rbp), %rsp\n\t"
    "popq %rdi\n\t"
    "popq %rsi\n\t"
    "popq %rbp\n\t"
    "retq\n\t"
    ".seh_endproc\n");
#else
/* ULONGLONG nova_call_x86(fn, const DWORD *stack, ndwords, fp, double *st0):
 * copies the argument block onto the stack and calls; ESP is restored from
 * EBP afterwards, so both stdcall and cdecl callees work */
ULONGLONG __cdecl nova_call_x86(void *fn, const DWORD *stack, DWORD n, DWORD fp, double *st0);
__asm__(
    ".text\n"
    ".globl _nova_call_x86\n"
    "_nova_call_x86:\n\t"
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "pushl %esi\n\t"
    "pushl %edi\n\t"
    "movl 16(%ebp), %ecx\n\t"
    "movl 12(%ebp), %esi\n\t"
    "leal 0(,%ecx,4), %eax\n\t"
    "subl %eax, %esp\n\t"
    "andl $-16, %esp\n\t"
    "movl %esp, %edi\n\t"
    "rep movsl\n\t"
    "call *8(%ebp)\n\t"
    "cmpl $0, 20(%ebp)\n\t"
    "je 1f\n\t"
    "movl 24(%ebp), %ecx\n\t"
    "fstpl (%ecx)\n"
    "1:\n\t"
    "leal -8(%ebp), %esp\n\t"
    "popl %edi\n\t"
    "popl %esi\n\t"
    "popl %ebp\n\t"
    "ret\n");
#endif

/* how many bytes an argument of @vt takes (x86) */
static int arg_size(VARTYPE vt)
{
    if (vt & (VT_BYREF | VT_ARRAY)) return sizeof(void *);
    switch (vt) {
    case VT_I8: case VT_UI8: case VT_R8: case VT_DATE: case VT_CY: return 8;
    case VT_VARIANT: case VT_DECIMAL: return 16;
    default: return 4;
    }
}

TLAPI HRESULT WINAPI DispCallFunc(void *obj, ULONG_PTR ovft, CALLCONV cc, VARTYPE ret, UINT n, VARTYPE *types,
                                  VARIANTARG **args, VARIANT *result)
{
    if (n && (!types || !args)) return E_INVALIDARG;
    if (cc != CC_STDCALL && cc != CC_CDECL && cc != CC_SYSCALL && cc != CC_FASTCALL) return E_INVALIDARG;
    void *fn = obj ? (*(void ***)obj)[ovft / sizeof(void *)] : (void *)ovft;
    if (!fn) return E_INVALIDARG;
    BOOL hidden = ret == VT_VARIANT || ret == VT_DECIMAL;          /* returned through a pointer */
    VARIANT tmp;
    VariantInit(&tmp);
#ifdef _WIN64
    DWORD64 stackbuf[32], *a = n + 2 <= 32 ? stackbuf : HeapAlloc(GetProcessHeap(), 0, (n + 2) * 8);
    if (!a) return E_OUTOFMEMORY;
    DWORD64 k = 0, xmm0 = 0;
    if (obj) a[k++] = (DWORD64)obj;
    if (hidden) a[k++] = (DWORD64)&tmp;
    for (UINT i = 0; i < n; i++) {
        VARTYPE vt = types[i];
        VARIANT *v = args[i];
        if (vt & (VT_BYREF | VT_ARRAY)) a[k++] = (DWORD64)v->byref;
        else if (vt == VT_VARIANT || vt == VT_DECIMAL) a[k++] = (DWORD64)v;     /* larger than 8 bytes: by address */
        else if (vt == VT_R4) { DWORD64 x = 0; memcpy(&x, &v->fltVal, 4); a[k++] = x; }
        else a[k++] = (DWORD64)v->llVal;
    }
    DWORD64 r = nova_call_x64(fn, a, k, &xmm0);
    if (a != stackbuf) HeapFree(GetProcessHeap(), 0, a);
    double d;
    float f;
    memcpy(&d, &xmm0, 8);
    memcpy(&f, &xmm0, 4);
#else
    DWORD stackbuf[64], *a = n * 4 + 2 <= 64 ? stackbuf : HeapAlloc(GetProcessHeap(), 0, (n * 4 + 2) * 4);
    if (!a) return E_OUTOFMEMORY;
    DWORD k = 0;
    if (obj) a[k++] = (DWORD)obj;
    if (hidden) a[k++] = (DWORD)&tmp;
    for (UINT i = 0; i < n; i++) {
        VARTYPE vt = types[i];
        VARIANT *v = args[i];
        int sz = arg_size(vt);
        if (vt & (VT_BYREF | VT_ARRAY)) a[k++] = (DWORD)v->byref;
        else if (sz == 16) { memcpy(&a[k], v, 16); k += 4; }
        else if (sz == 8) { memcpy(&a[k], &v->llVal, 8); k += 2; }
        else a[k++] = v->lVal;
    }
    double d = 0;
    BOOL fp = ret == VT_R4 || ret == VT_R8 || ret == VT_DATE;
    ULONGLONG r = nova_call_x86(fn, a, k, fp, &d);
    if (a != stackbuf) HeapFree(GetProcessHeap(), 0, a);
    float f = (float)d;
#endif
    if (!result) { VariantClear(&tmp); return S_OK; }
    VariantInit(result);
    switch (ret) {
    case VT_EMPTY: case VT_VOID: break;
    case VT_R4: result->vt = VT_R4; result->fltVal = f; break;
    case VT_R8: case VT_DATE: result->vt = ret; result->dblVal = d; break;
    case VT_VARIANT: *result = tmp; break;
    case VT_DECIMAL: result->decVal = tmp.decVal; result->vt = VT_DECIMAL; break;
    case VT_I8: case VT_UI8: case VT_CY: result->vt = ret; result->llVal = (LONGLONG)r; break;
    case VT_HRESULT: result->vt = VT_HRESULT; result->scode = (SCODE)(DWORD)r; break;
    default:
        result->vt = ret;
        if (arg_size(ret) <= 4 && !(ret & (VT_BYREF | VT_ARRAY))) result->llVal = (LONG)(DWORD)r;
        else result->llVal = (LONGLONG)r;
        if (ret == VT_I2 || ret == VT_BOOL) result->llVal = (SHORT)r;
        else if (ret == VT_UI2) result->llVal = (USHORT)r;
        else if (ret == VT_I1 || ret == VT_UI1) result->llVal = ret == VT_I1 ? (LONGLONG)(signed char)r : (BYTE)r;
    }
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * ITypeInfo::Invoke
 * ------------------------------------------------------------------------- */
enum { K_PLAIN, K_VARIANT, K_IFACE, K_ARRAY, K_BAD };

/* What a type description amounts to for passing a VARIANT: aliases and
 * enums seen through, interfaces with their IID */
typedef struct { int kind; VARTYPE vt; GUID iid; } ArgType;

static void resolve(TInfo *ti, const TYPEDESC *td, ArgType *at, int depth)
{
    at->kind = K_PLAIN;
    at->vt = td->vt;
    if (td->vt == VT_VARIANT) { at->kind = K_VARIANT; return; }
    if (td->vt == VT_SAFEARRAY) {
        at->kind = K_ARRAY;
        ArgType el;
        resolve(ti, td->lptdesc ? td->lptdesc : &(TYPEDESC){ .vt = VT_VARIANT }, &el, depth + 1);
        at->vt = VT_ARRAY | (el.kind == K_VARIANT ? VT_VARIANT : el.vt);
        return;
    }
    if (td->vt == VT_INT) { at->vt = VT_I4; return; }
    if (td->vt == VT_UINT) { at->vt = VT_UI4; return; }
    if (td->vt == VT_HRESULT) { at->vt = VT_ERROR; return; }
    if (td->vt == VT_LPWSTR || td->vt == VT_LPSTR) { at->kind = K_BAD; return; }
    if (td->vt != VT_USERDEFINED) return;
    ITypeInfo *r;
    if (depth > 16 || FAILED(tinfo_ref(ti, td->hreftype, &r))) { at->kind = K_BAD; return; }
    TInfo *t = (TInfo *)r;
    switch (t->attr.typekind) {
    case TKIND_ALIAS:  resolve(t, &t->attr.tdescAlias, at, depth + 1); break;
    case TKIND_ENUM:   at->vt = VT_I4; break;
    case TKIND_INTERFACE: at->kind = K_IFACE; at->vt = VT_UNKNOWN; at->iid = t->attr.guid; break;
    case TKIND_DISPATCH:  at->kind = K_IFACE; at->vt = VT_DISPATCH; at->iid = t->attr.guid; break;
    case TKIND_COCLASS:   at->kind = K_IFACE; at->vt = VT_UNKNOWN; at->iid = IID_IUnknown; break;
    default: at->kind = K_BAD;                       /* records and unions are not passed */
    }
    r->lpVtbl->Release(r);
}

/* the pointee of a VT_PTR (an [out] parameter), or 0 */
static const TYPEDESC *pointee(const TYPEDESC *td) { return td->vt == VT_PTR ? td->lptdesc : 0; }

/* An interface pointer for a parameter typed as @iid */
static HRESULT get_iface(const VARIANT *src, const ArgType *at, IUnknown **out)
{
    *out = 0;
    const VARIANT *v = src->vt == (VT_BYREF | VT_VARIANT) ? src->pvarVal : src;
    IUnknown *u;
    if (v->vt == VT_UNKNOWN || v->vt == VT_DISPATCH) u = v->punkVal;
    else if (v->vt == (VT_BYREF | VT_UNKNOWN) || v->vt == (VT_BYREF | VT_DISPATCH)) u = *v->ppunkVal;
    else if (v->vt == VT_EMPTY || v->vt == VT_NULL) return S_OK;
    else return DISP_E_TYPEMISMATCH;
    if (!u) return S_OK;
    HRESULT hr = u->lpVtbl->QueryInterface(u, &at->iid, (void **)out);
    return FAILED(hr) ? DISP_E_TYPEMISMATCH : S_OK;
}

typedef struct {
    VARIANT val;                /* the converted value (or the storage an [out] pointer points to) */
    VARIANT *byref_src;         /* [in, out] through the caller's own VT_BYREF|VT_VARIANT */
    ArgType out_type;           /* [out] / [retval]: the type stored */
    BOOL out, release_iface;
} Slot;

static void free_slot(Slot *s)
{
    if (s->release_iface) { if (s->val.punkVal) s->val.punkVal->lpVtbl->Release(s->val.punkVal); }
    else VariantClear(&s->val);
}

/* fill @res with an [out] value held in @s: ownership moves to the caller */
static void take_out(Slot *s, VARIANT *res)
{
    VariantInit(res);
    if (s->out_type.kind == K_VARIANT) { *res = s->val; VariantInit(&s->val); return; }
    res->vt = s->out_type.vt;
    res->llVal = s->val.llVal;
    if (s->out_type.vt == VT_DECIMAL) res->decVal = s->val.decVal, res->vt = VT_DECIMAL;
    VariantInit(&s->val);
}

/* clear what an [out] value holds, when nobody takes it */
static void drop_out(Slot *s)
{
    VARIANT v;
    take_out(s, &v);
    VariantClear(&v);
}

static void fill_excepinfo(void *obj, HRESULT hr, EXCEPINFO *ei)
{
    if (!ei) return;
    memset(ei, 0, sizeof(*ei));
    ei->scode = hr;
    IErrorInfo *e = 0;
    (void)obj;
    if (GetErrorInfo(0, &e) == S_OK && e) {
        e->lpVtbl->GetSource(e, &ei->bstrSource);
        e->lpVtbl->GetDescription(e, &ei->bstrDescription);
        e->lpVtbl->GetHelpFile(e, &ei->bstrHelpFile);
        e->lpVtbl->GetHelpContext(e, &ei->dwHelpContext);
        e->lpVtbl->Release(e);
    }
}

/* the argument for parameter @i: positional (rgvarg holds them last to
 * first, after the named ones) or named (by parameter index; the value of
 * a property put is DISPID_PROPERTYPUT) */
static VARIANT *find_arg(DISPPARAMS *dp, int i, int put_index, UINT *pos, UINT *where)
{
    UINT nnamed = dp->cNamedArgs, npos = dp->cArgs - nnamed;
    for (UINT k = 0; k < nnamed; k++) {
        DISPID id = dp->rgdispidNamedArgs[k];
        if (id == i || (id == DISPID_PROPERTYPUT && i == put_index)) { *where = k; return &dp->rgvarg[k]; }
    }
    if (*pos < npos) {
        UINT k = dp->cArgs - 1 - (*pos)++;
        *where = k;
        return &dp->rgvarg[k];
    }
    return 0;
}

static HRESULT invoke_vtable(TInfo *ti, const FUNCDESC *fd, void *obj, WORD flags, DISPPARAMS *dp, VARIANT *res,
                             EXCEPINFO *ei, UINT *argerr)
{
    int n = fd->cParams;
    DISPPARAMS none = { 0, 0, 0, 0 };
    if (!dp) dp = &none;
    if (dp->cArgs < dp->cNamedArgs || (dp->cArgs && !dp->rgvarg) || (dp->cNamedArgs && !dp->rgdispidNamedArgs))
        return E_INVALIDARG;
    Slot sbuf[16], *slots = n <= 16 ? sbuf : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Slot) * n);
    VARIANTARG *pbuf[16], **ptrs = n <= 16 ? pbuf : HeapAlloc(GetProcessHeap(), 0, sizeof(void *) * n);
    VARTYPE tbuf[16], *vts = n <= 16 ? tbuf : HeapAlloc(GetProcessHeap(), 0, sizeof(VARTYPE) * n);
    VARIANT pbox[16], *boxes = n <= 16 ? pbox : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VARIANT) * n);
    if (!slots || !ptrs || !vts || !boxes) return E_OUTOFMEMORY;
    memset(slots, 0, sizeof(Slot) * n);
    HRESULT hr = S_OK;
    int retval = -1, put_index = -1, inputs = 0;
    for (int i = 0; i < n; i++) {
        USHORT pf = fd->lprgelemdescParam[i].paramdesc.wParamFlags;
        if (!(pf & (PARAMFLAG_FRETVAL | PARAMFLAG_FLCID))) { put_index = i; inputs++; }
    }
    if (!(flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))) put_index = -1;
    if (dp->cArgs > (UINT)inputs) { hr = DISP_E_BADPARAMCOUNT; goto out; }

    UINT pos = 0;
    for (int i = 0; i < n && SUCCEEDED(hr); i++) {
        const ELEMDESC *ed = &fd->lprgelemdescParam[i];
        USHORT pf = ed->paramdesc.wParamFlags;
        Slot *s = &slots[i];
        VariantInit(&s->val);
        ptrs[i] = &boxes[i];
        VariantInit(&boxes[i]);
        const TYPEDESC *pt = pointee(&ed->tdesc);
        if (pf & PARAMFLAG_FLCID) { vts[i] = VT_I4; boxes[i].lVal = 0; continue; }
        if (pf & PARAMFLAG_FRETVAL) {
            if (!pt) { hr = DISP_E_BADVARTYPE; break; }
            resolve(ti, pt->vt == VT_PTR && pt->lptdesc && pt->lptdesc->vt == VT_USERDEFINED ? pt->lptdesc : pt,
                    &s->out_type, 0);
            if (s->out_type.kind == K_BAD) { hr = DISP_E_BADVARTYPE; break; }
            s->out = TRUE;
            retval = i;
            vts[i] = VT_BYREF | s->out_type.vt;
            boxes[i].byref = s->out_type.kind == K_VARIANT ? (void *)&s->val : (void *)&s->val.llVal;
            continue;
        }
        UINT where = 0;
        VARIANT *src = find_arg(dp, i, put_index, &pos, &where), defv;
        BOOL missing = !src || (src->vt == VT_ERROR && src->scode == DISP_E_PARAMNOTFOUND);
        if (missing && (pf & PARAMFLAG_FHASDEFAULT) && ed->paramdesc.pparamdescex) {
            defv = ed->paramdesc.pparamdescex->varDefaultValue;     /* (a BSTR stays the library's: copied below) */
            src = &defv;
            missing = FALSE;
        } else if (missing && !src && !(pf & PARAMFLAG_FOPT)) {
            hr = DISP_E_BADPARAMCOUNT;
            break;
        }
        if (missing) {                               /* an omitted [optional] VARIANT */
            VariantInit(&defv);
            defv.vt = VT_ERROR;
            defv.scode = DISP_E_PARAMNOTFOUND;
            src = &defv;
        }

        if (pt) {                                    /* by reference: [out] or [in, out], or an interface */
            ArgType at;
            if (pt->vt == VT_PTR && pt->lptdesc) {   /* IFoo **: an interface pointer by reference */
                resolve(ti, pt->lptdesc, &at, 0);
                at.kind = at.kind == K_IFACE ? K_PLAIN : K_BAD;
            } else {
                resolve(ti, pt, &at, 0);
            }
            if (at.kind == K_IFACE) {                /* IFoo *: an interface pointer by value */
                IUnknown *u;
                if (FAILED(hr = get_iface(src, &at, &u))) { if (argerr) *argerr = where; break; }
                s->val.punkVal = u;
                s->release_iface = TRUE;
                vts[i] = VT_UNKNOWN;
                boxes[i].punkVal = u;
                continue;
            }
            if (at.kind == K_BAD) { hr = DISP_E_BADVARTYPE; break; }
            VARTYPE want = at.kind == K_VARIANT ? VT_VARIANT : at.vt;
            vts[i] = VT_BYREF | want;
            if (src->vt == (VT_BYREF | want) && want != VT_VARIANT) {
                boxes[i].byref = src->byref;           /* the caller's own storage */
                continue;
            }
            if (want == VT_VARIANT) {
                if (src->vt == (VT_BYREF | VT_VARIANT)) { boxes[i].byref = src->pvarVal; continue; }
                hr = VariantCopy(&s->val, src);
                boxes[i].byref = &s->val;
                if (FAILED(hr)) break;
                continue;
            }
            /* a value of another type: converted in, and back out to a VT_BYREF|VT_VARIANT */
            if (src->vt == (VT_BYREF | VT_VARIANT)) s->byref_src = src->pvarVal;
            if (pf & PARAMFLAG_FIN || !(pf & PARAMFLAG_FOUT)) {
                if (FAILED(hr = VariantChangeType(&s->val, src, 0, want))) { if (argerr) *argerr = where; break; }
            } else {
                s->val.vt = want;
            }
            s->out_type = at;
            boxes[i].byref = want == VT_DECIMAL ? (void *)&s->val.decVal : (void *)&s->val.llVal;
            continue;
        }

        ArgType at;
        resolve(ti, &ed->tdesc, &at, 0);
        if (at.kind == K_BAD) { hr = DISP_E_BADVARTYPE; break; }
        if (at.kind == K_VARIANT) {
            hr = VariantCopyInd(&s->val, src);
            vts[i] = VT_VARIANT;
            ptrs[i] = &s->val;
            if (FAILED(hr)) break;
            continue;
        }
        if (at.kind == K_IFACE) {
            IUnknown *u;
            if (FAILED(hr = get_iface(src, &at, &u))) { if (argerr) *argerr = where; break; }
            s->val.punkVal = u;
            s->release_iface = TRUE;
            vts[i] = VT_UNKNOWN;
            boxes[i].punkVal = u;
            continue;
        }
        if (at.kind == K_ARRAY) {
            const VARIANT *v = src->vt == (VT_BYREF | VT_VARIANT) ? src->pvarVal : src;
            SAFEARRAY *sa = (v->vt & VT_ARRAY) ? ((v->vt & VT_BYREF) ? *v->pparray : v->parray) : 0;
            if (!sa && v->vt != VT_EMPTY) { hr = DISP_E_TYPEMISMATCH; if (argerr) *argerr = where; break; }
            vts[i] = VT_INT_PTR;
            boxes[i].byref = sa;
            continue;
        }
        if (FAILED(hr = VariantChangeType(&s->val, src, 0, at.vt))) { if (argerr) *argerr = where; break; }
        vts[i] = at.vt;
        ptrs[i] = &s->val;
    }

    if (SUCCEEDED(hr)) {
        VARIANT r;
        VARTYPE rt = fd->elemdescFunc.tdesc.vt;
        ArgType rat = { K_PLAIN, rt, { 0 } };
        if (rt == VT_USERDEFINED) resolve(ti, &fd->elemdescFunc.tdesc, &rat, 0);
        VARTYPE call_rt = rt == VT_HRESULT ? VT_HRESULT : rat.kind == K_VARIANT ? VT_VARIANT :
                          rat.kind == K_PLAIN || rat.kind == K_IFACE ? rat.vt : VT_INT_PTR;
        if (rt == VT_VOID) call_rt = VT_EMPTY;
        hr = DispCallFunc(obj, (ULONG_PTR)fd->oVft, fd->callconv, call_rt, (UINT)n, vts, ptrs, &r);
        if (SUCCEEDED(hr)) {
            if (rt == VT_HRESULT && FAILED(r.scode)) {
                fill_excepinfo(obj, r.scode, ei);
                hr = DISP_E_EXCEPTION;
            } else {
                /* values written back to VT_BYREF|VT_VARIANT arguments of another type */
                for (int i = 0; i < n; i++) {
                    Slot *s = &slots[i];
                    if (!s->byref_src || i == retval) continue;
                    VARIANT v;
                    take_out(s, &v);
                    VariantClear(s->byref_src);
                    *s->byref_src = v;
                }
                if (res) {
                    if (retval >= 0) take_out(&slots[retval], res);
                    else if (rt != VT_HRESULT && rt != VT_VOID) *res = r;
                    else VariantInit(res);
                } else if (retval >= 0) {
                    drop_out(&slots[retval]);
                }
            }
        }
    }
out:
    for (int i = 0; i < n; i++) {
        if (i == retval) { if (FAILED(hr)) drop_out(&slots[i]); continue; }
        free_slot(&slots[i]);                     /* (values handed out were moved out of their slots) */
    }
    if (slots != sbuf) HeapFree(GetProcessHeap(), 0, slots);
    if (ptrs != pbuf) HeapFree(GetProcessHeap(), 0, ptrs);
    if (vts != tbuf) HeapFree(GetProcessHeap(), 0, vts);
    if (boxes != pbox) HeapFree(GetProcessHeap(), 0, boxes);
    return hr;
}

HRESULT tinfo_invoke(TInfo *ti, void *obj, MEMBERID id, WORD flags, DISPPARAMS *dp, VARIANT *res,
                     EXCEPINFO *ei, UINT *argerr)
{
    if (!obj) return E_INVALIDARG;
    if (res && !(flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))) VariantInit(res);
    TFunc *f = 0;
    TInfo *owner = ti;
    /* a method call may also mean a property get, and the other way round */
    WORD want = flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET | DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF);
    if (FAILED(tinfo_find_func(ti, id, want, &f, &owner))) {
        /* a pure dispinterface's properties: only the object can say */
        for (UINT i = 0; i < ti->attr.cVars; i++)
            if (ti->vars[i].vd.memid == id && ti->vars[i].vd.varkind == VAR_DISPATCH) {
                IDispatch *d = obj;
                return d->lpVtbl->Invoke(d, id, &IID_NULL, 0, flags, dp, res, ei, argerr);
            }
        return DISP_E_MEMBERNOTFOUND;
    }
    if (f->fd.funckind == FUNC_DISPATCH) {           /* a dispinterface: the object's own IDispatch */
        IDispatch *d = obj;
        return d->lpVtbl->Invoke(d, id, &IID_NULL, 0, flags, dp, res, ei, argerr);
    }
    /* vtable methods (and a dual interface's) are called with their own signature */
    return invoke_vtable(owner, &f->fd, obj, flags, dp, res, ei, argerr);
}

/* ---------------------------------------------------------------------------
 * DispInvoke, DispGetIDsOfNames, CreateStdDispatch
 * ------------------------------------------------------------------------- */
TLAPI HRESULT WINAPI DispGetIDsOfNames(ITypeInfo *ti, LPOLESTR *names, UINT n, DISPID *ids)
{
    if (!ti) return E_INVALIDARG;
    return ti->lpVtbl->GetIDsOfNames(ti, names, n, ids);
}

TLAPI HRESULT WINAPI DispInvoke(void *obj, ITypeInfo *ti, DISPID id, WORD flags, DISPPARAMS *p, VARIANT *res,
                                EXCEPINFO *ei, UINT *argerr)
{
    if (!ti) return E_INVALIDARG;
    return ti->lpVtbl->Invoke(ti, obj, id, flags, p, res, ei, argerr);
}

/* The standard dispatch object: IDispatch for @obj from its type info */
typedef struct {
    IDispatch IDispatch_iface;
    IUnknown IUnknown_iface;                 /* the inner unknown when aggregated */
    LONG refs;
    IUnknown *outer;
    void *obj;
    ITypeInfo *ti;
} StdDisp;

#define SD_FROM_DISP(i) ((StdDisp *)(i))
#define SD_FROM_UNK(i) ((StdDisp *)((char *)(i) - __builtin_offsetof(StdDisp, IUnknown_iface)))

static HRESULT STDMETHODCALLTYPE sdu_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    StdDisp *sd = SD_FROM_UNK(iface);
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown)) *ppv = &sd->IUnknown_iface;
    else if (IsEqualIID(riid, &IID_IDispatch)) *ppv = &sd->IDispatch_iface;
    else { *ppv = 0; return E_NOINTERFACE; }
    ((IUnknown *)*ppv)->lpVtbl->AddRef((IUnknown *)*ppv);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE sdu_AddRef(IUnknown *iface) { return (ULONG)InterlockedIncrement(&SD_FROM_UNK(iface)->refs); }
static ULONG STDMETHODCALLTYPE sdu_Release(IUnknown *iface)
{
    StdDisp *sd = SD_FROM_UNK(iface);
    LONG r = InterlockedDecrement(&sd->refs);
    if (!r) {
        sd->ti->lpVtbl->Release(sd->ti);
        HeapFree(GetProcessHeap(), 0, sd);
    }
    return (ULONG)r;
}
static const IUnknownVtbl g_sdu_vtbl = { sdu_QueryInterface, sdu_AddRef, sdu_Release };

static HRESULT STDMETHODCALLTYPE sd_QueryInterface(IDispatch *iface, REFIID riid, void **ppv)
{
    IUnknown *o = SD_FROM_DISP(iface)->outer;
    return o->lpVtbl->QueryInterface(o, riid, ppv);
}
static ULONG STDMETHODCALLTYPE sd_AddRef(IDispatch *iface) { IUnknown *o = SD_FROM_DISP(iface)->outer; return o->lpVtbl->AddRef(o); }
static ULONG STDMETHODCALLTYPE sd_Release(IDispatch *iface) { IUnknown *o = SD_FROM_DISP(iface)->outer; return o->lpVtbl->Release(o); }
static HRESULT STDMETHODCALLTYPE sd_GetTypeInfoCount(IDispatch *iface, UINT *n) { (void)iface; if (!n) return E_INVALIDARG; *n = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE sd_GetTypeInfo(IDispatch *iface, UINT i, LCID lcid, ITypeInfo **out)
{
    (void)lcid;
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (i) return DISP_E_BADINDEX;
    *out = SD_FROM_DISP(iface)->ti;
    (*out)->lpVtbl->AddRef(*out);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sd_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids)
{
    (void)riid; (void)lcid;
    return DispGetIDsOfNames(SD_FROM_DISP(iface)->ti, names, n, ids);
}
static HRESULT STDMETHODCALLTYPE sd_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *p,
                                           VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    (void)lcid;
    if (!IsEqualIID(riid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    StdDisp *sd = SD_FROM_DISP(iface);
    return DispInvoke(sd->obj, sd->ti, id, flags, p, res, ei, argerr);
}
static const IDispatchVtbl g_sd_vtbl = {
    sd_QueryInterface, sd_AddRef, sd_Release, sd_GetTypeInfoCount, sd_GetTypeInfo, sd_GetIDsOfNames, sd_Invoke,
};

TLAPI HRESULT WINAPI CreateStdDispatch(IUnknown *outer, void *obj, ITypeInfo *ti, IUnknown **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (!obj || !ti) return E_INVALIDARG;
    StdDisp *sd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*sd));
    if (!sd) return E_OUTOFMEMORY;
    sd->IDispatch_iface.lpVtbl = &g_sd_vtbl;
    sd->IUnknown_iface.lpVtbl = &g_sdu_vtbl;
    sd->refs = 1;
    sd->outer = outer ? outer : &sd->IUnknown_iface;
    sd->obj = obj;
    sd->ti = ti;
    ti->lpVtbl->AddRef(ti);
    *out = &sd->IUnknown_iface;
    return S_OK;
}
