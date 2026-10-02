/* typelib.h — oleaut32's type library internals (typelib.c, typeinfo.c, invoke.c) */
#pragma once
#include <oleauto.h>

typedef struct TLib TLib;
typedef struct TInfo TInfo;

typedef struct {
    FUNCDESC fd;                /* as stored: vtable methods with their [retval] */
    FUNCDESC disp;              /* the dispinterface form (make_disp_form) */
    BSTR name, doc;
    DWORD helpctx, helpstrctx;
    BSTR *pnames;               /* fd.cParams names (0 for unnamed) */
    BSTR entry;                 /* TKIND_MODULE: the DLL entry point name, or */
    WORD ordinal;               /* its ordinal */
} TFunc;

typedef struct { VARDESC vd; BSTR name, doc; DWORD helpctx; } TVar;
typedef struct { HREFTYPE href; INT flags; } TImpl;

/* a reference into another library: HREFTYPE (offset | 1) / 12 */
typedef struct { BOOL by_guid; GUID guid; INT index; int lib; } TImpInfo;
typedef struct {
    DWORD offset;               /* in the import file segment */
    GUID guid; LCID lcid; WORD major, minor;
    BSTR name;
    ITypeLib *loaded;
} TImpLib;

struct TInfo {
    ITypeInfo2 ITypeInfo2_iface;
    ITypeComp ITypeComp_iface;
    TLib *lib;
    UINT index;
    HREFTYPE href;              /* how this typeinfo is referred to */
    TYPEATTR attr;
    BSTR name, doc, dllname;
    DWORD helpctx, helpstrctx;
    TFunc *funcs;               /* attr.cFuncs of them (own methods), except in a dual's dispatch view */
    TVar *vars;
    TImpl *impls;
    UINT nimpls;
    HREFTYPE base_href;         /* TKIND_DISPATCH: the interface it was declared on, or -1 */
    TInfo *dual;                /* a dual interface: the other view */
    BOOL dispview;              /* the dispinterface view of a dual interface */
    TFunc **dfuncs;             /* dispview: inherited methods first, then own (built on first use) */
    UINT ndfuncs, nown;
    ITypeInfo *dfuncs_hold;     /* keeps the base interface (another library) alive */
};

#define TL_MAX_STRS 64
struct TLib {
    ITypeLib2 ITypeLib2_iface;
    ITypeComp ITypeComp_iface;
    LONG refs;
    TLib *next;                 /* the cache of loaded libraries */
    BOOL builtin;               /* stdole2: never freed */
    WCHAR path[MAX_PATH];
    int res;
    TLIBATTR attr;
    BSTR name, doc, helpfile;
    DWORD helpctx;
    HREFTYPE dispatch_href;     /* IDispatch, which dispinterfaces derive from */
    TInfo **tinfos;
    UINT ntinfos;
    TImpLib *imps;
    UINT nimps;
    TImpInfo *impinfos;
    UINT nimpinfos;
    SIZE_T *blocks;             /* every allocation, freed with the library */
    BSTR *strs, strs0[TL_MAX_STRS];
    UINT nstrs;
};

extern const ITypeLib2Vtbl g_tlib_vtbl;
extern const ITypeCompVtbl g_tlib_comp_vtbl;

void *tl_alloc(TLib *lib, SIZE_T n);
int tl_ptr_size(TLib *lib);
TLib *tlib_new(void);
void tlib_destroy(TLib *lib);
ULONG tlib_addref(TLib *lib);
ULONG tlib_release(TLib *lib);
HRESULT tlib_finish(TLib *lib);
HRESULT tlib_import(TLib *lib, int index, ITypeLib **out);
TLib *stdole_lib(void);
TInfo *tinfo_new(TLib *lib, UINT index);

static inline TInfo *impl_from_ITypeInfo(ITypeInfo *iface) { return (TInfo *)iface; }

/* typeinfo.c */
HRESULT tinfo_ref(TInfo *ti, HREFTYPE href, ITypeInfo **out);
UINT tinfo_nfuncs(TInfo *ti);
TFunc *tinfo_func(TInfo *ti, UINT i, const FUNCDESC **fd);
HRESULT tinfo_find_func(TInfo *ti, MEMBERID id, WORD flags, TFunc **f, TInfo **owner);

/* invoke.c */
HRESULT tinfo_invoke(TInfo *ti, void *obj, MEMBERID id, WORD flags, DISPPARAMS *dp, VARIANT *res,
                     EXCEPINFO *ei, UINT *argerr);
