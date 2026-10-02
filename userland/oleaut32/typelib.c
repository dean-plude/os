/*
 * typelib.c — COM type libraries: LoadTypeLib and friends, ITypeLib,
 * ITypeInfo, ITypeComp and the registry entries under HKCR\TypeLib.
 *
 * A type library is read from a .tlb file or from the TYPELIB resource of
 * a DLL or EXE ("file.dll\2" names resource 2) in the "MSFT" format MIDL
 * writes (the older SLTG format is not supported).  The whole library is
 * decoded when it is loaded, into the TLib/TInfo structures below, and the
 * descriptions handed out (TYPEATTR, FUNCDESC, VARDESC) point into them, so
 * the Release* calls have nothing to free.
 *
 * Every typeinfo holds a reference on its library; the library goes away
 * with the last reference to it or to any of its typeinfos.  Loading the
 * same file again returns the same library.
 *
 * Dual interfaces are stored as a TKIND_DISPATCH typeinfo whose functions
 * are the interface's own vtable methods.  It is shown two ways, as on
 * Windows: the dispinterface view (what GetTypeInfoOfGuid returns: every
 * method from IUnknown on as FUNC_DISPATCH, [retval] turned into the
 * return value) and the TKIND_INTERFACE view (GetRefTypeOfImplType(-1)).
 *
 * stdole2.tlb (IUnknown, IDispatch, IEnumVARIANT), which nearly every type
 * library imports, is built in.  Invoke and DispCallFunc are in invoke.c.
 */
#define NOVA_BUILD_OLEAUT32
#include <oleauto.h>
#include "typelib.h"

#define TLAPI __declspec(dllexport)

WINBASEAPI DWORD WINAPI SearchPathW(LPCWSTR path, LPCWSTR file, LPCWSTR ext, DWORD n, LPWSTR buf, LPWSTR *part);

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */
static HANDLE heap(void) { return GetProcessHeap(); }
void *tl_alloc(TLib *lib, SIZE_T n)
{
    /* every allocation of a library is chained to it and freed with it */
    SIZE_T *p = HeapAlloc(heap(), HEAP_ZERO_MEMORY, n + 2 * sizeof(SIZE_T));
    if (!p) return 0;
    p[0] = (SIZE_T)lib->blocks;
    lib->blocks = p;
    return p + 2;
}

static BSTR tl_bstr(TLib *lib, const WCHAR *s)
{
    if (!s) return 0;
    BSTR b = SysAllocString(s);
    if (b && lib->nstrs < TL_MAX_STRS) lib->strs[lib->nstrs++] = b;
    else if (b) {                               /* grow the list */
        BSTR *n = tl_alloc(lib, sizeof(BSTR) * (lib->nstrs + 256));
        if (!n) { SysFreeString(b); return 0; }
        for (UINT i = 0; i < lib->nstrs; i++) n[i] = lib->strs[i];
        lib->strs = n;
        lib->strs[lib->nstrs++] = b;
    }
    return b;
}

static int wlen(const WCHAR *s) { int n = 0; while (s && s[n]) n++; return n; }
static void wcopy(WCHAR *d, const WCHAR *s, int max) { int i = 0; for (; s && s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }

static const WCHAR hexd[] = L"0123456789abcdef";
static WCHAR *put_hex(WCHAR *p, DWORD v)
{
    WCHAR t[9];
    int n = 0;
    do { t[n++] = hexd[v & 15]; v >>= 4; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static WCHAR *put_dec(WCHAR *p, DWORD v)
{
    WCHAR t[11];
    int n = 0;
    do { t[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static WCHAR *put_str(WCHAR *p, const WCHAR *s) { while (*s) *p++ = *s++; *p = 0; return p; }

int tl_ptr_size(TLib *lib) { return lib->attr.syskind == SYS_WIN64 ? 8 : 4; }

/* the TYPEDESC of a plain VARTYPE, for VT_PTR/VT_SAFEARRAY elements */
static TYPEDESC g_std_tdesc[VT_LPWSTR + 1 + 8];
static TYPEDESC *std_tdesc(VARTYPE vt)
{
    vt &= VT_TYPEMASK;
    if (vt >= sizeof(g_std_tdesc) / sizeof(g_std_tdesc[0])) vt = VT_VOID;
    g_std_tdesc[vt].vt = vt;
    return &g_std_tdesc[vt];
}

/* ---------------------------------------------------------------------------
 * The MSFT file format
 * ------------------------------------------------------------------------- */
#define MSFT_MAGIC 0x5446534D            /* "MSFT" */
#define SLTG_MAGIC 0x47544C53            /* "SLTG" */
#define HELPDLLFLAG 0x0100

typedef struct { INT offset, length, res08, res0c; } MsftSeg;
enum { SEG_TYPEINFO, SEG_IMPINFO, SEG_IMPFILES, SEG_REFTAB, SEG_GUIDHASH, SEG_GUID, SEG_NAMEHASH, SEG_NAME,
       SEG_STRING, SEG_TYPEDESC, SEG_ARRAYDESC, SEG_CUSTDATA, SEG_CDGUIDS, SEG_RES0E, SEG_RES0F, SEG_COUNT };

typedef struct {
    const BYTE *d;
    DWORD size;
    BOOL bad;                    /* an offset pointed outside the file */
    MsftSeg seg[SEG_COUNT];
    TLib *lib;
    TYPEDESC *tdescs;            /* the type description table, decoded */
    int ntdescs;
} Msft;

static INT rd32(Msft *m, DWORD off)
{
    if (off > m->size || m->size - off < 4) { m->bad = TRUE; return 0; }
    INT v;
    for (int i = 0; i < 4; i++) ((BYTE *)&v)[i] = m->d[off + i];
    return v;
}
static SHORT rd16(Msft *m, DWORD off)
{
    if (off > m->size || m->size - off < 2) { m->bad = TRUE; return 0; }
    return (SHORT)(m->d[off] | (m->d[off + 1] << 8));
}
static DWORD seg_off(Msft *m, int seg, INT off) { return (DWORD)(m->seg[seg].offset + off); }

static GUID msft_guid(Msft *m, INT off)
{
    GUID g = { 0 };
    if (off < 0) return g;
    DWORD o = seg_off(m, SEG_GUID, off);
    if (o > m->size || m->size - o < 16) { m->bad = TRUE; return g; }
    for (int i = 0; i < 16; i++) ((BYTE *)&g)[i] = m->d[o + i];
    return g;
}

static BSTR msft_chars(Msft *m, DWORD o, int n)
{
    if (o > m->size || m->size - o < (DWORD)n) { m->bad = TRUE; return 0; }
    WCHAR buf[512];
    WCHAR *w = n < 511 ? buf : HeapAlloc(heap(), 0, (n + 1) * sizeof(WCHAR));
    if (!w) return 0;
    int k = MultiByteToWideChar(CP_ACP, 0, (const char *)m->d + o, n, w, n);
    w[k > 0 ? k : 0] = 0;
    BSTR b = tl_bstr(m->lib, w);
    if (w != buf) HeapFree(heap(), 0, w);
    return b;
}

/* a name: (hreftype, next hash, length | hash << 8) then the characters */
static BSTR msft_name(Msft *m, INT off)
{
    if (off < 0) return 0;
    DWORD o = seg_off(m, SEG_NAME, off);
    return msft_chars(m, o + 12, rd32(m, o + 8) & 0xFF);
}
/* a string: 16-bit length, then the characters */
static BSTR msft_string(Msft *m, INT off)
{
    if (off < 0) return 0;
    DWORD o = seg_off(m, SEG_STRING, off);
    return msft_chars(m, o + 2, (USHORT)rd16(m, o));
}

/* A constant or default value: packed into the offset itself when the
 * high bit is set (VARTYPE in bits 26-30, value in the low 26 bits), else
 * stored in the custom data segment */
static void msft_value(Msft *m, INT off, VARIANT *v)
{
    VariantInit(v);
    if (off < 0) {
        v->vt = (VARTYPE)((off & 0x7C000000) >> 26);
        v->lVal = off & 0x03FFFFFF;
        if (v->vt == VT_I2 || v->vt == VT_BOOL) v->iVal = (SHORT)v->lVal;
        else if (v->vt == VT_I1 || v->vt == VT_UI1) v->bVal = (BYTE)v->lVal;
        return;
    }
    DWORD o = seg_off(m, SEG_CUSTDATA, off);
    v->vt = (VARTYPE)rd16(m, o);
    switch (v->vt) {
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
        v->llVal = (LONGLONG)(DWORD)rd32(m, o + 2) | ((LONGLONG)rd32(m, o + 6) << 32);
        break;
    case VT_I4: case VT_UI4: case VT_R4: case VT_INT: case VT_UINT: case VT_ERROR: case VT_HRESULT:
        v->lVal = rd32(m, o + 2);
        break;
    case VT_I2: case VT_UI2: case VT_BOOL:
        v->iVal = rd16(m, o + 2);
        break;
    case VT_I1: case VT_UI1:
        v->bVal = (BYTE)rd16(m, o + 2);
        break;
    case VT_BSTR: case VT_LPSTR: case VT_LPWSTR: {
        INT n = rd32(m, o + 2);
        BSTR s = n > 0 ? msft_chars(m, o + 6, n) : tl_bstr(m->lib, L"");
        v->vt = VT_BSTR;
        v->bstrVal = s;                       /* owned by the library: copy before handing out */
        break;
    }
    default:
        v->vt = VT_EMPTY;
    }
}

static void msft_tdesc(Msft *m, INT t, TYPEDESC *out)
{
    if (t < 0) { out->vt = (VARTYPE)(t & VT_TYPEMASK); out->lptdesc = 0; return; }
    int i = t / 8;
    if (i < m->ntdescs) *out = m->tdescs[i];
    else { m->bad = TRUE; out->vt = VT_VOID; }
}

static BOOL msft_typedescs(Msft *m)
{
    MsftSeg *s = &m->seg[SEG_TYPEDESC];
    if (s->length <= 0 || s->offset < 0) return TRUE;
    m->ntdescs = s->length / 8;
    m->tdescs = tl_alloc(m->lib, sizeof(TYPEDESC) * m->ntdescs);
    if (!m->tdescs) return FALSE;
    for (int i = 0; i < m->ntdescs; i++) {
        DWORD o = s->offset + i * 8;
        SHORT t0 = rd16(m, o), t2 = rd16(m, o + 4), t3 = rd16(m, o + 6);
        TYPEDESC *td = &m->tdescs[i];
        td->vt = (VARTYPE)(t0 & VT_TYPEMASK);
        if (td->vt == VT_PTR || td->vt == VT_SAFEARRAY) {
            if (t3 < 0) td->lptdesc = std_tdesc((VARTYPE)t2);
            else if ((USHORT)t2 / 8 < (USHORT)m->ntdescs) td->lptdesc = &m->tdescs[(USHORT)t2 / 8];
            else td->lptdesc = std_tdesc(VT_VOID);
        } else if (td->vt == VT_CARRAY) {
            td->lpadesc = (ARRAYDESC *)(INT_PTR)(USHORT)t2;     /* fixed up below */
        } else if (td->vt == VT_USERDEFINED) {
            td->hreftype = (DWORD)(USHORT)t2 | ((DWORD)(USHORT)t3 << 16);
        }
    }
    for (int i = 0; i < m->ntdescs; i++) {        /* array dimensions */
        TYPEDESC *td = &m->tdescs[i];
        if (td->vt != VT_CARRAY) continue;
        DWORD o = seg_off(m, SEG_ARRAYDESC, (INT)(INT_PTR)td->lpadesc);
        SHORT a0 = rd16(m, o), a1 = rd16(m, o + 2), dims = rd16(m, o + 4);
        if (dims < 0 || dims > 64) dims = 0;
        ARRAYDESC *ad = tl_alloc(m->lib, sizeof(ARRAYDESC) + sizeof(SAFEARRAYBOUND) * (dims ? dims : 1));
        if (!ad) return FALSE;
        if (a1 < 0) ad->tdescElem.vt = (VARTYPE)(a0 & VT_TYPEMASK);
        else if ((USHORT)a0 / 8 < (USHORT)m->ntdescs) ad->tdescElem = m->tdescs[(USHORT)a0 / 8];
        ad->cDims = (USHORT)dims;
        for (int k = 0; k < dims; k++) {
            ad->rgbounds[k].cElements = (ULONG)rd32(m, o + 8 + k * 8);
            ad->rgbounds[k].lLbound = rd32(m, o + 12 + k * 8);
        }
        td->lpadesc = ad;
    }
    return TRUE;
}

/* Imported libraries and the references into them */
static BOOL msft_imports(Msft *m)
{
    TLib *lib = m->lib;
    MsftSeg *s = &m->seg[SEG_IMPFILES];
    if (s->offset >= 0 && s->length > 0) {
        int cap = s->length / 14 + 1;
        lib->imps = tl_alloc(lib, sizeof(TImpLib) * cap);
        if (!lib->imps) return FALSE;
        DWORD o = s->offset, end = s->offset + s->length;
        while (o + 14 <= end && lib->nimps < cap && !m->bad) {
            TImpLib *im = &lib->imps[lib->nimps++];
            im->offset = o - s->offset;
            im->guid = msft_guid(m, rd32(m, o));
            im->lcid = (LCID)rd32(m, o + 4);
            im->major = (WORD)rd16(m, o + 8);
            im->minor = (WORD)rd16(m, o + 10);
            int n = (USHORT)rd16(m, o + 12) >> 2;
            im->name = msft_chars(m, o + 14, n);
            o = (o + 14 + n + 3) & ~3u;
        }
    }
    s = &m->seg[SEG_IMPINFO];
    if (s->offset >= 0 && s->length > 0) {
        lib->nimpinfos = s->length / 12;
        lib->impinfos = tl_alloc(lib, sizeof(TImpInfo) * lib->nimpinfos);
        if (!lib->impinfos) return FALSE;
        for (UINT i = 0; i < lib->nimpinfos; i++) {
            DWORD o = s->offset + i * 12;
            INT flags = rd32(m, o), file = rd32(m, o + 4), g = rd32(m, o + 8);
            TImpInfo *ii = &lib->impinfos[i];
            ii->by_guid = (flags & 0x10000) != 0;
            ii->guid = ii->by_guid ? msft_guid(m, g) : (GUID){ 0 };
            ii->index = ii->by_guid ? -1 : g;
            ii->lib = -1;
            for (UINT k = 0; k < lib->nimps; k++)
                if (lib->imps[k].offset == (DWORD)file) ii->lib = (int)k;
        }
    }
    return TRUE;
}

static int count_params(const FUNCDESC *fd) { return fd->cParams; }

/* the dispinterface form of a vtable method: [retval] becomes the result */
static void make_disp_form(TFunc *f)
{
    f->disp = f->fd;
    f->disp.funckind = FUNC_DISPATCH;
    f->disp.callconv = CC_STDCALL;
    if (f->fd.funckind == FUNC_DISPATCH) return;
    int n = count_params(&f->fd);
    if (n > 0 && (f->fd.lprgelemdescParam[n - 1].paramdesc.wParamFlags & PARAMFLAG_FRETVAL)) {
        const TYPEDESC *t = &f->fd.lprgelemdescParam[n - 1].tdesc;
        f->disp.cParams--;
        f->disp.elemdescFunc.tdesc = t->vt == VT_PTR && t->lptdesc ? *t->lptdesc : *t;
        f->disp.elemdescFunc.paramdesc.wParamFlags = 0;
        f->disp.elemdescFunc.paramdesc.pparamdescex = 0;
        if (f->disp.cParamsOpt > f->disp.cParams) f->disp.cParamsOpt = f->disp.cParams;
    } else if (f->fd.elemdescFunc.tdesc.vt == VT_HRESULT) {
        f->disp.elemdescFunc.tdesc.vt = VT_VOID;
    }
}

static BOOL msft_members(Msft *m, TInfo *ti, INT memoff)
{
    TLib *lib = m->lib;
    int nf = ti->attr.cFuncs, nv = ti->attr.cVars;
    if (!nf && !nv) return TRUE;
    INT infolen = rd32(m, memoff);
    DWORD rec = memoff + 4, tab = memoff + infolen;
    ti->funcs = tl_alloc(lib, sizeof(TFunc) * (nf ? nf : 1));
    ti->vars = tl_alloc(lib, sizeof(TVar) * (nv ? nv : 1));
    if (!ti->funcs || !ti->vars) return FALSE;
    int ptr = tl_ptr_size(lib);

    for (int i = 0; i < nf && !m->bad; i++) {
        TFunc *f = &ti->funcs[i];
        DWORD len = (DWORD)rd32(m, rec) & 0xFFFF;
        INT datatype = rd32(m, rec + 4), flags = rd32(m, rec + 8);
        SHORT vtoff = rd16(m, rec + 12);
        INT fk = rd32(m, rec + 16);
        SHORT nargs = rd16(m, rec + 20), nopt = rd16(m, rec + 22);
        if (len < 24 || nargs < 0 || nargs > 1024) { m->bad = TRUE; break; }
        /* the optional fields' size: the record less the parameters (and their defaults) */
        INT optional = (INT)len - nargs * 12 - ((fk & 0x1000) ? nargs * 4 : 0);
        f->fd.memid = rd32(m, tab + 4 * (i + 1));
        f->fd.funckind = (FUNCKIND)(fk & 7);
        f->fd.invkind = (INVOKEKIND)((fk >> 3) & 15);
        f->fd.callconv = (CALLCONV)((fk >> 8) & 15);
        f->fd.cParams = nargs;
        f->fd.cParamsOpt = nopt;
        f->fd.oVft = f->fd.funckind == FUNC_DISPATCH ? 0 : (SHORT)((vtoff & ~1) * (int)sizeof(void *) / ptr);
        f->fd.wFuncFlags = (WORD)flags;
        msft_tdesc(m, datatype, &f->fd.elemdescFunc.tdesc);
        if (optional > 24) f->helpctx = (DWORD)rd32(m, rec + 24);
        if (optional > 28) f->doc = msft_string(m, rd32(m, rec + 28));
        if (optional > 32) {
            INT e = rd32(m, rec + 32);
            if (fk & 0x2000) f->ordinal = (WORD)e;
            else f->entry = msft_string(m, e);
        }
        if (optional > 44) f->helpstrctx = (DWORD)rd32(m, rec + 44);
        INT name = rd32(m, tab + 4 * (nf + nv + i + 1));
        /* the second half of a property get/put pair may carry no name */
        if (name == -1 && i > 0) f->name = ti->funcs[i - 1].name;
        else f->name = msft_name(m, name);
        if (nargs) {
            f->fd.lprgelemdescParam = tl_alloc(lib, sizeof(ELEMDESC) * nargs);
            f->pnames = tl_alloc(lib, sizeof(BSTR) * nargs);
            if (!f->fd.lprgelemdescParam || !f->pnames) return FALSE;
            DWORD pbase = rec + len - nargs * 12;
            for (int k = 0; k < nargs; k++) {
                ELEMDESC *ed = &f->fd.lprgelemdescParam[k];
                msft_tdesc(m, rd32(m, pbase + k * 12), &ed->tdesc);
                INT pn = rd32(m, pbase + k * 12 + 4);
                f->pnames[k] = pn != -1 ? msft_name(m, pn) : 0;
                ed->paramdesc.wParamFlags = (USHORT)rd32(m, pbase + k * 12 + 8);
                if ((ed->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) && (fk & 0x1000)) {
                    PARAMDESCEX *px = tl_alloc(lib, sizeof(PARAMDESCEX));
                    if (!px) return FALSE;
                    px->cBytes = sizeof(PARAMDESCEX);
                    msft_value(m, rd32(m, rec + len - nargs * 16 + k * 4), &px->varDefaultValue);
                    ed->paramdesc.pparamdescex = px;
                }
            }
        }
        make_disp_form(f);
        rec += len;
    }
    for (int i = 0; i < nv && !m->bad; i++) {
        TVar *v = &ti->vars[i];
        DWORD len = (DWORD)rd32(m, rec) & 0xFF;
        if (len < 20) { m->bad = TRUE; break; }
        v->vd.memid = rd32(m, tab + 4 * (nf + i + 1));
        v->name = msft_name(m, rd32(m, tab + 4 * (2 * nf + nv + i + 1)));
        msft_tdesc(m, rd32(m, rec + 4), &v->vd.elemdescVar.tdesc);
        v->vd.wVarFlags = (WORD)rd32(m, rec + 8);
        v->vd.varkind = (VARKIND)rd16(m, rec + 12);
        INT val = rd32(m, rec + 16);
        if (len > 20) v->helpctx = (DWORD)rd32(m, rec + 20);
        if (len > 24) v->doc = msft_string(m, rd32(m, rec + 24));
        if (v->vd.varkind == VAR_CONST) {
            VARIANT *c = tl_alloc(lib, sizeof(VARIANT));
            if (!c) return FALSE;
            msft_value(m, val, c);
            v->vd.lpvarValue = c;
        } else {
            v->vd.oInst = (ULONG)val;
        }
        rec += len;
    }
    return !m->bad;
}

static HRESULT msft_parse(TLib *lib, const BYTE *d, DWORD size)
{
    Msft m = { d, size, FALSE, { { 0 } }, lib, 0, 0 };
    if (size < 0x54 || rd32(&m, 0) != MSFT_MAGIC) return TYPE_E_CANTLOADLIBRARY;
    INT posguid = rd32(&m, 0x08), varflags = rd32(&m, 0x14), version = rd32(&m, 0x18), flags = rd32(&m, 0x1C);
    INT ntinfos = rd32(&m, 0x20), helpstring = rd32(&m, 0x24), helpctx = rd32(&m, 0x2C);
    INT nameoff = rd32(&m, 0x38), helpfile = rd32(&m, 0x3C), dispatchpos = rd32(&m, 0x4C);
    if (ntinfos < 0 || ntinfos > 0x10000) return TYPE_E_INVDATAREAD;
    DWORD segdir = 0x54 + ((varflags & HELPDLLFLAG) ? 4 : 0) + ntinfos * 4;
    for (int i = 0; i < SEG_COUNT; i++) {
        m.seg[i].offset = rd32(&m, segdir + i * 16);
        m.seg[i].length = rd32(&m, segdir + i * 16 + 4);
    }
    if (m.bad || rd32(&m, segdir + 12) != 0x0F) return TYPE_E_INVDATAREAD;

    lib->attr.guid = msft_guid(&m, posguid);
    lib->attr.lcid = (LCID)rd32(&m, 0x10);      /* the library's own lcid (0x0C is the one its strings use) */
    lib->attr.syskind = (SYSKIND)(varflags & 0x0F);
    lib->attr.wMajorVerNum = (WORD)version;
    lib->attr.wMinorVerNum = (WORD)(version >> 16);
    lib->attr.wLibFlags = (WORD)flags;
    lib->name = msft_name(&m, nameoff);
    lib->doc = msft_string(&m, helpstring);
    lib->helpfile = msft_string(&m, helpfile);
    lib->helpctx = (DWORD)helpctx;
    lib->dispatch_href = dispatchpos >= 0 ? (HREFTYPE)dispatchpos : (HREFTYPE)-1;
    if (!msft_typedescs(&m) || !msft_imports(&m)) return E_OUTOFMEMORY;

    lib->ntinfos = (UINT)ntinfos;
    lib->tinfos = tl_alloc(lib, sizeof(TInfo *) * (ntinfos ? ntinfos : 1));
    if (!lib->tinfos) return E_OUTOFMEMORY;
    for (int i = 0; i < ntinfos && !m.bad; i++) {
        DWORD b = m.seg[SEG_TYPEINFO].offset + i * 0x64;
        TInfo *ti = tinfo_new(lib, (UINT)i);
        if (!ti) return E_OUTOFMEMORY;
        INT kind = rd32(&m, b), memoff = rd32(&m, b + 4), celem = rd32(&m, b + 0x18);
        TYPEATTR *a = &ti->attr;
        a->typekind = (TYPEKIND)(kind & 0xF);
        a->cbAlignment = (WORD)((kind >> 11) & 0x1F);
        a->guid = msft_guid(&m, rd32(&m, b + 0x2C));
        a->wTypeFlags = (WORD)rd32(&m, b + 0x30);
        ti->name = msft_name(&m, rd32(&m, b + 0x34));
        INT ver = rd32(&m, b + 0x38);
        a->wMajorVerNum = (WORD)ver;
        a->wMinorVerNum = (WORD)(ver >> 16);
        ti->doc = msft_string(&m, rd32(&m, b + 0x3C));
        ti->helpstrctx = (DWORD)rd32(&m, b + 0x40);
        ti->helpctx = (DWORD)rd32(&m, b + 0x44);
        a->cImplTypes = (WORD)rd16(&m, b + 0x4C);
        a->cbSizeVft = (WORD)((USHORT)rd16(&m, b + 0x4E) * sizeof(void *) / tl_ptr_size(lib));
        a->cbSizeInstance = (ULONG)rd32(&m, b + 0x50);
        INT dt1 = rd32(&m, b + 0x54);
        a->cFuncs = (WORD)(celem & 0xFFFF);
        a->cVars = (WORD)((DWORD)celem >> 16);
        a->lcid = lib->attr.lcid;
        a->memidConstructor = a->memidDestructor = MEMBERID_NIL;
        if (a->typekind == TKIND_ALIAS) msft_tdesc(&m, dt1, &a->tdescAlias);
        if (a->typekind == TKIND_MODULE) ti->dllname = msft_string(&m, dt1);
        if (!msft_members(&m, ti, memoff)) return m.bad ? TYPE_E_INVDATAREAD : E_OUTOFMEMORY;

        if (a->cImplTypes) {
            ti->impls = tl_alloc(lib, sizeof(TImpl) * a->cImplTypes);
            if (!ti->impls) return E_OUTOFMEMORY;
            if (a->typekind == TKIND_COCLASS) {            /* a chain of reference records */
                INT o = dt1;
                for (int k = 0; k < a->cImplTypes && o >= 0 && !m.bad; k++) {
                    DWORD r = seg_off(&m, SEG_REFTAB, o);
                    ti->impls[k].href = (HREFTYPE)rd32(&m, r);
                    ti->impls[k].flags = rd32(&m, r + 4);
                    o = rd32(&m, r + 12);
                    ti->nimpls++;
                }
            } else {
                ti->impls[0].href = (HREFTYPE)dt1;
                ti->nimpls = 1;
            }
        }
        if (a->typekind == TKIND_DISPATCH) {
            ti->base_href = ti->nimpls ? ti->impls[0].href : (HREFTYPE)-1;
            if (!ti->nimpls) {
                ti->impls = tl_alloc(lib, sizeof(TImpl));
                if (!ti->impls) return E_OUTOFMEMORY;
                ti->nimpls = 1;
            }
            ti->impls[0].href = lib->dispatch_href;      /* every dispinterface derives from IDispatch */
            a->cImplTypes = 1;
        }
        lib->tinfos[i] = ti;
    }
    if (m.bad) return TYPE_E_INVDATAREAD;
    return tlib_finish(lib);
}

/* ---------------------------------------------------------------------------
 * Libraries: loading, the cache, the built-in stdole2
 * ------------------------------------------------------------------------- */
static TLib *g_libs;
static volatile LONG g_lock;
static void lock(void) { while (InterlockedCompareExchange(&g_lock, 1, 0)) Sleep(0); }
static void unlock(void) { InterlockedExchange(&g_lock, 0); }

static const GUID LIBID_stdole2 = { 0x00020430, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

TLib *tlib_new(void)
{
    TLib *lib = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(TLib));
    if (!lib) return 0;
    lib->ITypeLib2_iface.lpVtbl = &g_tlib_vtbl;
    lib->ITypeComp_iface.lpVtbl = &g_tlib_comp_vtbl;
    lib->refs = 1;
    lib->strs = lib->strs0;
    lib->dispatch_href = (HREFTYPE)-1;
    return lib;
}

void tlib_destroy(TLib *lib)
{
    for (UINT i = 0; i < lib->nimps; i++)
        if (lib->imps[i].loaded) ITypeLib_Release(lib->imps[i].loaded);
    for (UINT i = 0; i < lib->ntinfos; i++) {
        TInfo *ti = lib->tinfos[i];
        if (ti && ti->dfuncs_hold) ITypeInfo_Release(ti->dfuncs_hold);
        if (ti && ti->dual && ti->dual->dfuncs_hold) ITypeInfo_Release(ti->dual->dfuncs_hold);
    }
    for (UINT i = 0; i < lib->nstrs; i++) SysFreeString(lib->strs[i]);
    SIZE_T *b = lib->blocks;
    while (b) {
        SIZE_T *next = (SIZE_T *)b[0];
        HeapFree(heap(), 0, b);
        b = next;
    }
    HeapFree(heap(), 0, lib);
}

ULONG tlib_addref(TLib *lib) { return (ULONG)InterlockedIncrement(&lib->refs); }
ULONG tlib_release(TLib *lib)
{
    lock();
    LONG r = InterlockedDecrement(&lib->refs);
    if (!r && !lib->builtin) {                    /* drop it from the cache */
        for (TLib **pp = &g_libs; *pp; pp = &(*pp)->next)
            if (*pp == lib) { *pp = lib->next; break; }
    }
    unlock();
    if (!r && !lib->builtin) tlib_destroy(lib);
    return (ULONG)r;
}

/* Read @path (the resource number @res of an executable) */
static HRESULT read_file(const WCHAR *path, BYTE **out, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return TYPE_E_CANTLOADLIBRARY;
    DWORD n = GetFileSize(f, 0), got = 0;
    BYTE *d = n && n < 0x10000000 ? HeapAlloc(heap(), 0, n) : 0;
    BOOL ok = d && ReadFile(f, d, n, &got, 0) && got == n;
    CloseHandle(f);
    if (!ok) { HeapFree(heap(), 0, d); return d ? TYPE_E_IOERROR : TYPE_E_CANTLOADLIBRARY; }
    *out = d;
    *size = n;
    return S_OK;
}

static DWORD le32(const BYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24); }
static WORD le16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }

/* The TYPELIB resource number @res of a PE file: its offset and size */
static BOOL find_resource(const BYTE *d, DWORD size, int res, DWORD *off, DWORD *len)
{
    if (size < 0x40 || d[0] != 'M' || d[1] != 'Z') return FALSE;
    DWORD pe = le32(d + 0x3C);
    if (pe > size - 24 || le32(d + pe) != 0x00004550) return FALSE;
    WORD nsec = le16(d + pe + 6), optsize = le16(d + pe + 20);
    DWORD opt = pe + 24;
    if (opt + optsize > size || optsize < 2) return FALSE;
    WORD magic = le16(d + opt);
    DWORD dd = opt + (magic == 0x20B ? 112 : 96) + 2 * 8;       /* data directory 2: resources */
    if (dd + 8 > opt + optsize) return FALSE;
    DWORD rsrc = le32(d + dd);
    if (!rsrc) return FALSE;
    DWORD sec = opt + optsize, base = 0, delta = 0;
    BOOL found = FALSE;
    for (WORD i = 0; i < nsec && sec + 40 <= size; i++, sec += 40) {
        DWORD va = le32(d + sec + 12), vsz = le32(d + sec + 8), raw = le32(d + sec + 20), rsz = le32(d + sec + 16);
        if (vsz < rsz) vsz = rsz;
        if (rsrc >= va && rsrc < va + vsz) { base = raw + (rsrc - va); delta = va - raw; found = TRUE; break; }
    }
    if (!found || base >= size) return FALSE;
#define IN(o, n) ((o) <= size && size - (o) >= (n))
    /* level 1: the "TYPELIB" type */
    DWORD dir = base, sub = 0;
    if (!IN(dir, 16)) return FALSE;
    int nnamed = le16(d + dir + 12), nid = le16(d + dir + 14);
    for (int i = 0; i < nnamed + nid; i++) {
        DWORD e = dir + 16 + i * 8;
        if (!IN(e, 8)) return FALSE;
        DWORD name = le32(d + e), to = le32(d + e + 4);
        if (!(name & 0x80000000)) continue;
        DWORD s = base + (name & 0x7FFFFFFF);
        if (!IN(s, 2)) continue;
        WORD n = le16(d + s);
        static const char want[] = "TYPELIB";
        if (n != 7 || !IN(s + 2, 14)) continue;
        BOOL eq = TRUE;
        for (int k = 0; k < 7; k++) {
            WORD c = le16(d + s + 2 + k * 2);
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c != (WORD)want[k]) eq = FALSE;
        }
        if (eq && (to & 0x80000000)) { sub = base + (to & 0x7FFFFFFF); break; }
    }
    if (!sub || !IN(sub, 16)) return FALSE;
    /* level 2: the resource number (the first one if @res is not there) */
    DWORD lang = 0;
    nnamed = le16(d + sub + 12); nid = le16(d + sub + 14);
    for (int i = 0; i < nnamed + nid; i++) {
        DWORD e = sub + 16 + i * 8;
        if (!IN(e, 8)) return FALSE;
        DWORD name = le32(d + e), to = le32(d + e + 4);
        if (!(to & 0x80000000)) continue;
        if (!lang && res == 1 && i == 0) lang = base + (to & 0x7FFFFFFF);
        if (!(name & 0x80000000) && (int)name == res) { lang = base + (to & 0x7FFFFFFF); break; }
    }
    if (!lang || !IN(lang, 24)) return FALSE;
    /* level 3: the first language */
    DWORD to = le32(d + lang + 16 + 4);
    if (to & 0x80000000) return FALSE;
    DWORD ent = base + to;
    if (!IN(ent, 16)) return FALSE;
    DWORD rva = le32(d + ent), n = le32(d + ent + 4);
    if (rva < delta || !IN(rva - delta, n)) return FALSE;
    *off = rva - delta;
    *len = n;
    return TRUE;
#undef IN
}

/* Split "path\N" into the file and resource number; find a bare name the
 * way LoadLibrary would */
static BOOL resolve_path(const WCHAR *file, WCHAR *full, int *res)
{
    WCHAR tmp[MAX_PATH];
    *res = 1;
    wcopy(tmp, file, MAX_PATH);
    for (int pass = 0; pass < 2; pass++) {
        if (GetFileAttributesW(tmp) != INVALID_FILE_ATTRIBUTES && GetFullPathNameW(tmp, MAX_PATH, full, 0)) return TRUE;
        BOOL bare = TRUE;
        for (const WCHAR *p = tmp; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') bare = FALSE;
        if (bare && SearchPathW(0, tmp, 0, MAX_PATH, full, 0)) return TRUE;
        if (pass) break;
        /* a trailing \N: the resource number */
        int n = wlen(tmp), i = n;
        while (i > 0 && tmp[i - 1] >= '0' && tmp[i - 1] <= '9') i--;
        if (i == n || i < 2 || tmp[i - 1] != '\\') break;
        int r = 0;
        for (int k = i; k < n; k++) r = r * 10 + (tmp[k] - '0');
        *res = r;
        tmp[i - 1] = 0;
    }
    return FALSE;
}

static int wieq(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static const WCHAR *leaf(const WCHAR *p)
{
    const WCHAR *l = p;
    for (; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') l = p + 1;
    return l;
}

static HRESULT load_lib(const WCHAR *file, TLib **out)
{
    WCHAR full[MAX_PATH];
    int res = 1;
    *out = 0;
    if (!file) return E_INVALIDARG;
    if (!resolve_path(file, full, &res)) {
        const WCHAR *l = leaf(file);
        if (wieq(l, L"stdole2.tlb") || wieq(l, L"stdole32.tlb")) {
            *out = stdole_lib();
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        return TYPE_E_CANTLOADLIBRARY;
    }

    lock();
    for (TLib *l = g_libs; l; l = l->next)
        if (l->res == res && wieq(l->path, full)) {
            tlib_addref(l);
            unlock();
            *out = l;
            return S_OK;
        }
    unlock();

    BYTE *d;
    DWORD size, off = 0, len;
    HRESULT hr = read_file(full, &d, &size);
    if (FAILED(hr)) return hr;
    len = size;
    if (size >= 2 && d[0] == 'M' && d[1] == 'Z' && !find_resource(d, size, res, &off, &len)) {
        HeapFree(heap(), 0, d);
        return TYPE_E_CANTLOADLIBRARY;
    }
    TLib *lib = tlib_new();
    if (!lib) { HeapFree(heap(), 0, d); return E_OUTOFMEMORY; }
    if (len >= 4 && le32(d + off) == SLTG_MAGIC) hr = TYPE_E_UNSUPFORMAT;
    else hr = msft_parse(lib, d + off, len);
    HeapFree(heap(), 0, d);
    if (FAILED(hr)) { tlib_destroy(lib); return hr; }
    wcopy(lib->path, full, MAX_PATH);
    lib->res = res;

    lock();
    for (TLib *l = g_libs; l; l = l->next)          /* another thread was quicker */
        if (l->res == res && wieq(l->path, full)) {
            tlib_addref(l);
            unlock();
            tlib_destroy(lib);
            *out = l;
            return S_OK;
        }
    lib->next = g_libs;
    g_libs = lib;
    unlock();
    *out = lib;
    return S_OK;
}

/* ---- the built-in stdole2.tlb ---- */
static TLib *g_stdole;

static ELEMDESC *params(TLib *lib, int n, const VARTYPE *vts, const USHORT *flags)
{
    ELEMDESC *e = tl_alloc(lib, sizeof(ELEMDESC) * (n ? n : 1));
    for (int i = 0; e && i < n; i++) {
        VARTYPE vt = vts[i];
        if (vt & VT_BYREF) { e[i].tdesc.vt = VT_PTR; e[i].tdesc.lptdesc = std_tdesc(vt & ~VT_BYREF); }
        else e[i].tdesc.vt = vt;
        e[i].paramdesc.wParamFlags = flags[i];
    }
    return e;
}

static void add_func(TLib *lib, TInfo *ti, const WCHAR *name, MEMBERID id, VARTYPE ret, int n,
                     const VARTYPE *vts, const USHORT *flags, const WCHAR *const *names)
{
    TFunc *f = &ti->funcs[ti->attr.cFuncs];
    f->name = tl_bstr(lib, name);
    f->fd.memid = id;
    f->fd.funckind = FUNC_PUREVIRTUAL;
    f->fd.invkind = INVOKE_FUNC;
    f->fd.callconv = CC_STDCALL;
    f->fd.cParams = (SHORT)n;
    f->fd.oVft = (SHORT)((id & 0xFF) * sizeof(void *) + (id & 0x10000 ? 3 * sizeof(void *) : 0));
    f->fd.wFuncFlags = FUNCFLAG_FRESTRICTED;
    f->fd.elemdescFunc.tdesc.vt = ret;
    f->fd.lprgelemdescParam = params(lib, n, vts, flags);
    f->pnames = tl_alloc(lib, sizeof(BSTR) * (n ? n : 1));
    for (int i = 0; f->pnames && i < n; i++) f->pnames[i] = tl_bstr(lib, names[i]);
    make_disp_form(f);
    ti->attr.cFuncs++;
}

static TInfo *add_iface(TLib *lib, UINT i, const GUID *g, const WCHAR *name, WORD flags, int nfuncs, int vft)
{
    TInfo *ti = tinfo_new(lib, i);
    if (!ti) return 0;
    ti->attr.guid = *g;
    ti->attr.typekind = TKIND_INTERFACE;
    ti->attr.wTypeFlags = flags;
    ti->attr.cbSizeVft = (WORD)(vft * sizeof(void *));
    ti->attr.cbSizeInstance = sizeof(void *);
    ti->attr.cbAlignment = sizeof(void *);
    ti->attr.memidConstructor = ti->attr.memidDestructor = MEMBERID_NIL;
    ti->name = tl_bstr(lib, name);
    ti->funcs = tl_alloc(lib, sizeof(TFunc) * nfuncs);
    lib->tinfos[i] = ti;
    return ti->funcs ? ti : 0;
}

TLib *stdole_lib(void)
{
    lock();
    TLib *lib = g_stdole;
    if (lib) { tlib_addref(lib); unlock(); return lib; }
    lib = tlib_new();
    if (!lib) { unlock(); return 0; }
    lib->builtin = TRUE;
    lib->attr.guid = LIBID_stdole2;
    lib->attr.syskind = sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32;
    lib->attr.wMajorVerNum = 2;
    lib->name = tl_bstr(lib, L"stdole");
    lib->doc = tl_bstr(lib, L"OLE Automation");
    wcopy(lib->path, L"C:\\Windows\\System32\\stdole2.tlb", MAX_PATH);
    lib->ntinfos = 3;
    lib->tinfos = tl_alloc(lib, sizeof(TInfo *) * 3);
    lib->dispatch_href = 1 * 0x64;
    static const VARTYPE qi_t[] = { VT_BYREF | VT_VOID, VT_BYREF | VT_BYREF | VT_VOID };
    static const USHORT in_out[] = { PARAMFLAG_FIN, PARAMFLAG_FOUT }, in[] = { PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FIN },
        gti_f[] = { PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FOUT },
        gid_f[] = { PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FOUT },
        inv_f[] = { PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FOUT,
                    PARAMFLAG_FOUT, PARAMFLAG_FOUT },
        next_f[] = { PARAMFLAG_FIN, PARAMFLAG_FOUT, PARAMFLAG_FOUT }, out1[] = { PARAMFLAG_FOUT };
    static const VARTYPE gtic_t[] = { VT_BYREF | VT_UINT },
        gti_t[] = { VT_UINT, VT_UI4, VT_BYREF | VT_BYREF | VT_VOID },
        gid_t[] = { VT_BYREF | VT_VOID, VT_BYREF | VT_BYREF | VT_I1, VT_UINT, VT_UI4, VT_BYREF | VT_I4 },
        inv_t[] = { VT_I4, VT_BYREF | VT_VOID, VT_UI4, VT_UI2, VT_BYREF | VT_VOID, VT_BYREF | VT_VARIANT,
                    VT_BYREF | VT_VOID, VT_BYREF | VT_UINT },
        next_t[] = { VT_UI4, VT_BYREF | VT_VARIANT, VT_BYREF | VT_UI4 }, skip_t[] = { VT_UI4 },
        clone_t[] = { VT_BYREF | VT_BYREF | VT_VOID };
    static const WCHAR *const qi_n[] = { L"riid", L"ppvObj" }, *const gtic_n[] = { L"pctinfo" },
        *const gti_n[] = { L"itinfo", L"lcid", L"pptinfo" },
        *const gid_n[] = { L"riid", L"rgszNames", L"cNames", L"lcid", L"rgdispid" },
        *const inv_n[] = { L"dispidMember", L"riid", L"lcid", L"wFlags", L"pdispparams", L"pvarResult",
                           L"pexcepinfo", L"puArgErr" },
        *const next_n[] = { L"celt", L"rgvar", L"pceltFetched" }, *const skip_n[] = { L"celt" },
        *const clone_n[] = { L"ppenum" };
    (void)in_out; (void)in;

    TInfo *unk = add_iface(lib, 0, &IID_IUnknown, L"IUnknown", TYPEFLAG_FHIDDEN, 3, 3);
    TInfo *disp = add_iface(lib, 1, &IID_IDispatch, L"IDispatch", TYPEFLAG_FRESTRICTED, 4, 7);
    static const GUID iid_enumvar = { 0x00020404, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    TInfo *ev = add_iface(lib, 2, &iid_enumvar, L"IEnumVARIANT", TYPEFLAG_FHIDDEN, 4, 7);
    if (!unk || !disp || !ev) { tlib_destroy(lib); unlock(); return 0; }
    add_func(lib, unk, L"QueryInterface", 0x60000000, VT_HRESULT, 2, qi_t, in_out, qi_n);
    add_func(lib, unk, L"AddRef", 0x60000001, VT_UI4, 0, 0, 0, 0);
    add_func(lib, unk, L"Release", 0x60000002, VT_UI4, 0, 0, 0, 0);
    add_func(lib, disp, L"GetTypeInfoCount", 0x60010000, VT_HRESULT, 1, gtic_t, out1, gtic_n);
    add_func(lib, disp, L"GetTypeInfo", 0x60010001, VT_HRESULT, 3, gti_t, gti_f, gti_n);
    add_func(lib, disp, L"GetIDsOfNames", 0x60010002, VT_HRESULT, 5, gid_t, gid_f, gid_n);
    add_func(lib, disp, L"Invoke", 0x60010003, VT_HRESULT, 8, inv_t, inv_f, inv_n);
    add_func(lib, ev, L"Next", 0x60010000, VT_HRESULT, 3, next_t, next_f, next_n);
    add_func(lib, ev, L"Skip", 0x60010001, VT_HRESULT, 1, skip_t, in, skip_n);
    add_func(lib, ev, L"Reset", 0x60010002, VT_HRESULT, 0, 0, 0, 0);
    add_func(lib, ev, L"Clone", 0x60010003, VT_HRESULT, 1, clone_t, out1, clone_n);
    for (TInfo **p = (TInfo *[]){ disp, ev }, **e = p + 2; p < e; p++) {
        (*p)->impls = tl_alloc(lib, sizeof(TImpl));
        if ((*p)->impls) { (*p)->impls[0].href = 0; (*p)->nimpls = 1; (*p)->attr.cImplTypes = 1; }
    }
    if (FAILED(tlib_finish(lib))) { tlib_destroy(lib); unlock(); return 0; }
    lib->refs = 2;                                /* ours and the caller's: it stays */
    g_stdole = lib;
    unlock();
    return lib;
}

/* the library an import refers to (loaded on first use) */
HRESULT tlib_import(TLib *lib, int index, ITypeLib **out)
{
    *out = 0;
    if (index < 0 || (UINT)index >= lib->nimps) return TYPE_E_LIBNOTREGISTERED;
    TImpLib *im = &lib->imps[index];
    if (!im->loaded) {
        ITypeLib *tl = 0;
        HRESULT hr = LoadRegTypeLib(&im->guid, im->major, im->minor, im->lcid, &tl);
        if (FAILED(hr) && im->name) hr = LoadTypeLibEx(im->name, REGKIND_NONE, &tl);
        if (FAILED(hr)) return hr;
        if (InterlockedCompareExchangePointer((void **)&im->loaded, tl, 0)) ITypeLib_Release(tl);
    }
    *out = im->loaded;
    ITypeLib_AddRef(*out);
    return S_OK;
}

/* ---------------------------------------------------------------------------
 * The exported API
 * ------------------------------------------------------------------------- */
TLAPI HRESULT WINAPI LoadTypeLibEx(LPCOLESTR file, REGKIND kind, ITypeLib **out)
{
    if (!out) return E_INVALIDARG;
    TLib *lib;
    HRESULT hr = load_lib(file, &lib);
    *out = SUCCEEDED(hr) ? (ITypeLib *)&lib->ITypeLib2_iface : 0;
    if (FAILED(hr)) return hr;
    /* REGKIND_DEFAULT registers libraries not named by a full path */
    BOOL fullpath = (file[0] == '\\' && file[1] == '\\') || (file[0] && file[1] == ':');
    if (kind == REGKIND_REGISTER || (kind == REGKIND_DEFAULT && !fullpath)) {
        if (!lib->builtin) {
            hr = RegisterTypeLib(*out, lib->path, 0);
            if (FAILED(hr)) { ITypeLib_Release(*out); *out = 0; }
        }
    }
    return hr;
}

TLAPI HRESULT WINAPI LoadTypeLib(LPCOLESTR file, ITypeLib **out) { return LoadTypeLibEx(file, REGKIND_DEFAULT, out); }

/* ---- the registry: HKCR\TypeLib\{libid}\maj.min\lcid\win32|win64 ---- */
static WCHAR *put_guid(WCHAR *p, const GUID *g)
{
    StringFromGUID2(g, p, 39);
    return p + 38;
}
static WCHAR *put_ver(WCHAR *p, WORD maj, WORD min)
{
    p = put_hex(p, maj);
    *p++ = '.';
    return put_hex(p, min);
}

static LSTATUS set_value(HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *val)
{
    HKEY k;
    LSTATUS e = RegCreateKeyExW(root, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0);
    if (e) return e;
    e = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(wlen(val) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
    return e;
}

static HRESULT register_lib(HKEY root, ITypeLib *tl, LPCOLESTR path, LPCOLESTR helpdir)
{
    if (!tl || !path) return E_INVALIDARG;
    TLIBATTR *a;
    HRESULT hr = ITypeLib_GetLibAttr(tl, &a);
    if (FAILED(hr)) return hr;
    BSTR name = 0, doc = 0;
    ITypeLib_GetDocumentation(tl, -1, &name, &doc, 0, 0);
    WCHAR key[160], *p = put_str(key, L"TypeLib\\");
    p = put_guid(p, &a->guid);
    *p++ = '\\';
    p = put_ver(p, a->wMajorVerNum, a->wMinorVerNum);
    WCHAR *ver_end = p;
    LSTATUS e = set_value(root, key, 0, doc && doc[0] ? doc : name ? name : L"");
    WCHAR num[16];
    put_dec(num, a->wLibFlags);
    put_str(ver_end, L"\\FLAGS");
    if (!e) e = set_value(root, key, 0, num);
    p = put_str(ver_end, L"\\");
    p = put_hex(p, a->lcid);
    put_str(p, a->syskind == SYS_WIN64 ? L"\\win64" : L"\\win32");
    if (!e) e = set_value(root, key, 0, path);
    WCHAR dir[MAX_PATH];
    if (helpdir) wcopy(dir, helpdir, MAX_PATH);
    else {
        wcopy(dir, path, MAX_PATH);
        WCHAR *l = (WCHAR *)leaf(dir);
        if (l > dir) l[-1] = 0;
    }
    put_str(ver_end, L"\\HELPDIR");
    if (!e) e = set_value(root, key, 0, dir);

    /* interfaces the Automation marshaler serves */
    WCHAR libid[40], vers[16];
    StringFromGUID2(&a->guid, libid, 40);
    put_ver(vers, a->wMajorVerNum, a->wMinorVerNum);
    UINT n = ITypeLib_GetTypeInfoCount(tl);
    for (UINT i = 0; i < n && !e; i++) {
        ITypeInfo *ti;
        if (FAILED(ITypeLib_GetTypeInfo(tl, i, &ti))) continue;
        TYPEATTR *ta;
        if (SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta))) {
            if ((ta->typekind == TKIND_INTERFACE && (ta->wTypeFlags & (TYPEFLAG_FOLEAUTOMATION | TYPEFLAG_FDUAL))) ||
                ta->typekind == TKIND_DISPATCH) {
                BSTR iname = 0;
                ITypeInfo_GetDocumentation(ti, MEMBERID_NIL, &iname, 0, 0, 0);
                WCHAR ik[96], *q = put_str(ik, L"Interface\\");
                q = put_guid(q, &ta->guid);
                const WCHAR *ps = ta->typekind == TKIND_DISPATCH && !(ta->wTypeFlags & TYPEFLAG_FDUAL)
                                  ? L"{00020420-0000-0000-C000-000000000046}"         /* PSDispatch */
                                  : L"{00020424-0000-0000-C000-000000000046}";        /* PSOAInterface */
                e = set_value(root, ik, 0, iname ? iname : L"");
                put_str(q, L"\\ProxyStubClsid");
                if (!e) e = set_value(root, ik, 0, ps);
                put_str(q, L"\\ProxyStubClsid32");
                if (!e) e = set_value(root, ik, 0, ps);
                put_str(q, L"\\TypeLib");
                if (!e) e = set_value(root, ik, 0, libid);
                if (!e) e = set_value(root, ik, L"Version", vers);
                SysFreeString(iname);
            }
            ITypeInfo_ReleaseTypeAttr(ti, ta);
        }
        ITypeInfo_Release(ti);
    }
    SysFreeString(name);
    SysFreeString(doc);
    ITypeLib_ReleaseTLibAttr(tl, a);
    return e ? TYPE_E_REGISTRYACCESS : S_OK;
}

TLAPI HRESULT WINAPI RegisterTypeLib(ITypeLib *tl, LPCOLESTR path, LPCOLESTR helpdir)
{
    return register_lib(HKEY_CLASSES_ROOT, tl, path, helpdir);
}

TLAPI HRESULT WINAPI RegisterTypeLibForUser(ITypeLib *tl, LPOLESTR path, LPOLESTR helpdir)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0))
        return TYPE_E_REGISTRYACCESS;
    HRESULT hr = register_lib(k, tl, path, helpdir);
    RegCloseKey(k);
    return hr;
}

/* "maj.min" (hex) */
static BOOL parse_ver(const WCHAR *s, WORD *maj, WORD *min)
{
    DWORD v[2] = { 0, 0 };
    int part = 0, digits = 0;
    for (; *s; s++) {
        WCHAR c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (c == '.' && !part && digits) { part = 1; digits = 0; continue; }
        if (d < 0) return FALSE;
        v[part] = v[part] * 16 + d;
        digits++;
    }
    if (!part || !digits) return FALSE;
    *maj = (WORD)v[0];
    *min = (WORD)v[1];
    return TRUE;
}

static BOOL query_default(HKEY k, const WCHAR *sub, WCHAR *out, DWORD cch)
{
    HKEY s;
    if (RegOpenKeyExW(k, sub, 0, KEY_READ, &s)) return FALSE;
    DWORD type, n = cch * sizeof(WCHAR);
    LSTATUS e = RegQueryValueExW(s, 0, 0, &type, (BYTE *)out, &n);
    RegCloseKey(s);
    if (e || (type != REG_SZ && type != REG_EXPAND_SZ)) return FALSE;
    out[cch - 1] = 0;
    return TRUE;
}

TLAPI HRESULT WINAPI QueryPathOfRegTypeLib(REFGUID guid, USHORT maj, USHORT min, LCID lcid, BSTR *path)
{
    if (!guid || !path) return E_INVALIDARG;
    *path = 0;
    WCHAR key[64], *p = put_str(key, L"TypeLib\\");
    put_guid(p, guid);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k)) return TYPE_E_LIBNOTREGISTERED;
    /* the same major version with the highest minor >= @min (any version for 0xFFFF) */
    WCHAR best[32] = { 0 }, name[64];
    int best_min = -1, best_maj = -1;
    for (DWORD i = 0;; i++) {
        DWORD n = 64;
        if (RegEnumKeyExW(k, i, name, &n, 0, 0, 0, 0)) break;
        WORD vmaj, vmin;
        if (!parse_ver(name, &vmaj, &vmin)) continue;
        if (maj == 0xFFFF && min == 0xFFFF) {
            if (vmaj > best_maj || (vmaj == best_maj && vmin > best_min)) { best_maj = vmaj; best_min = vmin; wcopy(best, name, 32); }
        } else if (vmaj == maj && vmin >= min && vmin > best_min) {
            best_maj = vmaj; best_min = vmin; wcopy(best, name, 32);
        }
    }
    HRESULT hr = TYPE_E_LIBNOTREGISTERED;
    if (best[0]) {
        LCID tries[3] = { lcid, lcid & 0x3FF, 0 };
        for (int t = 0; t < 3 && FAILED(hr); t++) {
            if (t && tries[t] == tries[t - 1]) continue;
            for (int plat = 0; plat < 2 && FAILED(hr); plat++) {
                WCHAR sub[64], buf[MAX_PATH], *q = put_str(sub, best);
                *q++ = '\\';
                q = put_hex(q, tries[t]);
                put_str(q, (sizeof(void *) == 8) == !plat ? L"\\win64" : L"\\win32");
                if (query_default(k, sub, buf, MAX_PATH)) {
                    *path = SysAllocString(buf);
                    hr = *path ? S_OK : E_OUTOFMEMORY;
                }
            }
        }
    }
    RegCloseKey(k);
    return hr;
}

TLAPI HRESULT WINAPI LoadRegTypeLib(REFGUID guid, WORD maj, WORD min, LCID lcid, ITypeLib **out)
{
    if (!out) return E_INVALIDARG;
    *out = 0;
    BSTR path;
    HRESULT hr = QueryPathOfRegTypeLib(guid, maj, min, lcid, &path);
    if (SUCCEEDED(hr)) {
        hr = LoadTypeLibEx(path, REGKIND_NONE, out);
        SysFreeString(path);
    }
    if (FAILED(hr) && IsEqualGUID(guid, &LIBID_stdole2) && maj <= 2) {
        TLib *lib = stdole_lib();
        if (!lib) return E_OUTOFMEMORY;
        *out = (ITypeLib *)&lib->ITypeLib2_iface;
        hr = S_OK;
    }
    return hr;
}

static HRESULT unregister_lib(HKEY root, REFGUID guid, WORD maj, WORD min, LCID lcid, SYSKIND kind)
{
    if (!guid) return E_INVALIDARG;
    WCHAR key[160], *p = put_str(key, L"TypeLib\\");
    p = put_guid(p, guid);
    WCHAR *lib_end = p;
    *p++ = '\\';
    p = put_ver(p, maj, min);
    WCHAR *ver_end = p;
    *p++ = '\\';
    p = put_hex(p, lcid);
    put_str(p, kind == SYS_WIN64 ? L"\\win64" : L"\\win32");
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k)) return TYPE_E_REGISTRYACCESS;
    RegCloseKey(k);

    /* the interfaces it registered (when the library can still be read) */
    WCHAR path[MAX_PATH];
    ITypeLib *tl = 0;
    if (query_default(root, key, path, MAX_PATH) && SUCCEEDED(LoadTypeLibEx(path, REGKIND_NONE, &tl))) {
        UINT n = ITypeLib_GetTypeInfoCount(tl);
        for (UINT i = 0; i < n; i++) {
            ITypeInfo *ti;
            TYPEATTR *ta;
            if (FAILED(ITypeLib_GetTypeInfo(tl, i, &ti))) continue;
            if (SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta))) {
                if (ta->typekind == TKIND_INTERFACE || ta->typekind == TKIND_DISPATCH) {
                    WCHAR ik[96], *q = put_str(ik, L"Interface\\");
                    put_guid(q, &ta->guid);
                    WCHAR tk[140], cur[40];
                    put_str(put_str(tk, ik), L"\\TypeLib");
                    WCHAR want[40];
                    StringFromGUID2(guid, want, 40);
                    if (query_default(root, tk, cur, 40) && wieq(cur, want)) RegDeleteTreeW(root, ik);
                }
                ITypeInfo_ReleaseTypeAttr(ti, ta);
            }
            ITypeInfo_Release(ti);
        }
        ITypeLib_Release(tl);
    }
    *p = 0;                                       /* ...\lcid */
    RegDeleteTreeW(root, key);
    RegDeleteKeyW(root, key);
    /* the version key goes when only FLAGS and HELPDIR are left */
    *ver_end = 0;
    if (!RegOpenKeyExW(root, key, 0, KEY_READ, &k)) {
        WCHAR name[64];
        BOOL other = FALSE;
        for (DWORD i = 0;; i++) {
            DWORD n = 64;
            if (RegEnumKeyExW(k, i, name, &n, 0, 0, 0, 0)) break;
            if (!wieq(name, L"FLAGS") && !wieq(name, L"HELPDIR")) other = TRUE;
        }
        RegCloseKey(k);
        if (!other) RegDeleteTreeW(root, key), RegDeleteKeyW(root, key);
    }
    *lib_end = 0;
    if (!RegOpenKeyExW(root, key, 0, KEY_READ, &k)) {
        WCHAR name[64];
        DWORD n = 64;
        BOOL empty = RegEnumKeyExW(k, 0, name, &n, 0, 0, 0, 0) != 0;
        RegCloseKey(k);
        if (empty) RegDeleteKeyW(root, key);
    }
    return S_OK;
}

TLAPI HRESULT WINAPI UnRegisterTypeLib(REFGUID guid, WORD maj, WORD min, LCID lcid, SYSKIND kind)
{
    return unregister_lib(HKEY_CLASSES_ROOT, guid, maj, min, lcid, kind);
}

TLAPI HRESULT WINAPI UnRegisterTypeLibForUser(REFGUID guid, WORD maj, WORD min, LCID lcid, SYSKIND kind)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_ALL_ACCESS, &k)) return TYPE_E_REGISTRYACCESS;
    HRESULT hr = unregister_lib(k, guid, maj, min, lcid, kind);
    RegCloseKey(k);
    return hr;
}

/* The name hash ITypeLib::IsName/FindName take (and ignore here): the
 * system kind and locale in the high word, a hash of the name below */
TLAPI ULONG WINAPI LHashValOfNameSys(SYSKIND kind, LCID lcid, LPCOLESTR name)
{
    if (!name) return 0;
    ULONG h = 0x3EB7;
    for (; *name; name++) {
        WCHAR c = *name;
        if (c >= 'a' && c <= 'z') c -= 32;
        h = h * 37 + c;
    }
    return (h % 65599 & 0xFFFF) | ((kind == SYS_WIN16 ? 0 : kind == SYS_WIN64 ? 3 : 1) << 16) | ((lcid & 0xFF) << 24);
}

TLAPI ULONG WINAPI LHashValOfNameSysA(SYSKIND kind, LCID lcid, LPCSTR name)
{
    WCHAR w[256];
    if (!name) return 0;
    int n = MultiByteToWideChar(CP_ACP, 0, name, -1, w, 256);
    if (n <= 0) w[0] = 0;
    w[255] = 0;
    return LHashValOfNameSys(kind, lcid, w);
}
