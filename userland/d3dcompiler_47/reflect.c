/*
 * D3DReflect: ID3D11ShaderReflection (and ID3D12ShaderReflection, whose
 * methods are the same) on a DXBC shader, read from its sections: RDEF
 * (constant buffers, variables, types, bound resources), ISGN/OSGN/PCSG and
 * their 5.0/5.1 forms (signatures), STAT (instruction counts), SHEX/SHDR
 * (version, thread group, sample-rate inputs) and SFI0 (required features).
 * The constant buffer, variable and type objects belong to the reflection
 * object and live as long as it; out-of-range lookups give the null objects
 * whose GetDesc fails, as Windows' do.
 */
#include <windows.h>
#include <string.h>
#include "d3dcompiler.h"

/* ID3D11ShaderReflection's IID changed between SDKs (the newer ones only
 * add methods at the end); every one of them, and ID3D12ShaderReflection's */
static const GUID g_iids[] = {
    { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } },
    { 0x17f27486, 0xa342, 0x4d10, { 0x88, 0x42, 0xab, 0x08, 0x74, 0xe7, 0xf6, 0x70 } },
    { 0x0a233719, 0x3960, 0x4578, { 0x9d, 0x7c, 0x20, 0x3b, 0x8b, 0x1d, 0x9c, 0xc1 } },
    { 0x5a58797d, 0xa72c, 0x478d, { 0x8b, 0xa2, 0xef, 0xc6, 0xb0, 0xef, 0xe8, 0x8e } },
};
static BOOL reflection_iid(REFIID iid)
{
    for (unsigned i = 0; i < sizeof(g_iids) / sizeof(*g_iids); i++)
        if (!memcmp(iid, &g_iids[i], sizeof(GUID))) return TRUE;
    return FALSE;
}

typedef struct {
    UINT Version; LPCSTR Creator; UINT Flags; UINT ConstantBuffers, BoundResources, InputParameters, OutputParameters;
    UINT InstructionCount, TempRegisterCount, TempArrayCount, DefCount, DclCount, TextureNormalInstructions,
         TextureLoadInstructions, TextureCompInstructions, TextureBiasInstructions, TextureGradientInstructions,
         FloatInstructionCount, IntInstructionCount, UintInstructionCount, StaticFlowControlCount,
         DynamicFlowControlCount, MacroInstructionCount, ArrayInstructionCount, CutInstructionCount,
         EmitInstructionCount, GSOutputTopology, GSMaxOutputVertexCount, InputPrimitive, PatchConstantParameters,
         cGSInstanceCount, cControlPoints, HSOutputPrimitive, HSPartitioning, TessellatorDomain,
         cBarrierInstructions, cInterlockedInstructions, cTextureStoreInstructions;
} SHADER_DESC;
typedef struct { LPCSTR Name; UINT Type, Variables, Size, uFlags; } BUFFER_DESC;
typedef struct { LPCSTR Name; UINT StartOffset, Size, uFlags; LPVOID DefaultValue;
                 UINT StartTexture, TextureSize, StartSampler, SamplerSize; } VARIABLE_DESC;
typedef struct { UINT Class, Type, Rows, Columns, Elements, Members, Offset; LPCSTR Name; } TYPE_DESC;
typedef struct { LPCSTR Name; UINT Type, BindPoint, BindCount, uFlags, ReturnType, Dimension, NumSamples; } BIND_DESC;
typedef struct { LPCSTR SemanticName; UINT SemanticIndex, Register, SystemValueType, ComponentType;
                 BYTE Mask, ReadWriteMask; UINT Stream, MinPrecision; } PARAM_DESC;

typedef struct Refl Refl;
typedef struct Type Type;
typedef struct Var Var;
typedef struct CBuf CBuf;

/* ---- ID3D11ShaderReflectionType ---- */
typedef struct {
    HRESULT (WINAPI *GetDesc)(Type *, TYPE_DESC *);
    Type *(WINAPI *GetMemberTypeByIndex)(Type *, UINT);
    Type *(WINAPI *GetMemberTypeByName)(Type *, LPCSTR);
    LPCSTR (WINAPI *GetMemberTypeName)(Type *, UINT);
    HRESULT (WINAPI *IsEqual)(Type *, Type *);
    Type *(WINAPI *GetSubType)(Type *);
    Type *(WINAPI *GetBaseClass)(Type *);
    UINT (WINAPI *GetNumInterfaces)(Type *);
    Type *(WINAPI *GetInterfaceByIndex)(Type *, UINT);
    HRESULT (WINAPI *IsOfType)(Type *, Type *);
    HRESULT (WINAPI *ImplementsInterface)(Type *, Type *);
} TypeVtbl;
struct Type {
    const TypeVtbl *vtbl;
    DWORD at;                       /* offset in RDEF (types are shared by offset, so IsEqual compares objects) */
    TYPE_DESC desc;
    Type **members;
    LPCSTR *member_names;
    Type *next;
};

/* ---- ID3D11ShaderReflectionVariable ---- */
typedef struct {
    HRESULT (WINAPI *GetDesc)(Var *, VARIABLE_DESC *);
    Type *(WINAPI *GetType)(Var *);
    CBuf *(WINAPI *GetBuffer)(Var *);
    UINT (WINAPI *GetInterfaceSlot)(Var *, UINT);
} VarVtbl;
struct Var { const VarVtbl *vtbl; VARIABLE_DESC desc; Type *type; CBuf *buf; };

/* ---- ID3D11ShaderReflectionConstantBuffer ---- */
typedef struct {
    HRESULT (WINAPI *GetDesc)(CBuf *, BUFFER_DESC *);
    Var *(WINAPI *GetVariableByIndex)(CBuf *, UINT);
    Var *(WINAPI *GetVariableByName)(CBuf *, LPCSTR);
} CBufVtbl;
struct CBuf { const CBufVtbl *vtbl; BUFFER_DESC desc; Var *vars; };

/* ---- ID3D11ShaderReflection ---- */
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Refl *, REFIID, void **);
    ULONG (WINAPI *AddRef)(Refl *);
    ULONG (WINAPI *Release)(Refl *);
    HRESULT (WINAPI *GetDesc)(Refl *, SHADER_DESC *);
    CBuf *(WINAPI *GetConstantBufferByIndex)(Refl *, UINT);
    CBuf *(WINAPI *GetConstantBufferByName)(Refl *, LPCSTR);
    HRESULT (WINAPI *GetResourceBindingDesc)(Refl *, UINT, BIND_DESC *);
    HRESULT (WINAPI *GetInputParameterDesc)(Refl *, UINT, PARAM_DESC *);
    HRESULT (WINAPI *GetOutputParameterDesc)(Refl *, UINT, PARAM_DESC *);
    HRESULT (WINAPI *GetPatchConstantParameterDesc)(Refl *, UINT, PARAM_DESC *);
    Var *(WINAPI *GetVariableByName)(Refl *, LPCSTR);
    HRESULT (WINAPI *GetResourceBindingDescByName)(Refl *, LPCSTR, BIND_DESC *);
    UINT (WINAPI *GetMovInstructionCount)(Refl *);
    UINT (WINAPI *GetMovcInstructionCount)(Refl *);
    UINT (WINAPI *GetConversionInstructionCount)(Refl *);
    UINT (WINAPI *GetBitwiseInstructionCount)(Refl *);
    UINT (WINAPI *GetGSInputPrimitive)(Refl *);
    BOOL (WINAPI *IsSampleFrequencyShader)(Refl *);
    UINT (WINAPI *GetNumInterfaceSlots)(Refl *);
    HRESULT (WINAPI *GetMinFeatureLevel)(Refl *, UINT *);
    UINT (WINAPI *GetThreadGroupSize)(Refl *, UINT *, UINT *, UINT *);
    UINT64 (WINAPI *GetRequiresFlags)(Refl *);
} ReflVtbl;

typedef struct { PARAM_DESC *p; UINT n; } Sig;
struct Refl {
    const ReflVtbl *vtbl;
    LONG refs;
    BYTE *data;                     /* the shader, copied: names and default values point into it */
    SIZE_T size;
    SHADER_DESC desc;
    CBuf *cbufs;
    BIND_DESC *binds;
    Sig in, out, pc;
    Type *types;
    UINT mov, conversion, slots, tgx, tgy, tgz, level;
    BOOL sample_rate;
    UINT64 requires;
};

/* ---- the null objects ---- */
static HRESULT WINAPI nt_desc(Type *t, TYPE_DESC *d) { (void)t; (void)d; return E_FAIL; }
static Type *WINAPI t_member(Type *t, UINT i);
static Type *WINAPI t_member_name(Type *t, LPCSTR name);
static LPCSTR WINAPI t_member_tname(Type *t, UINT i);
static HRESULT WINAPI t_equal(Type *t, Type *o);
static Type *WINAPI t_none(Type *t);
static UINT WINAPI t_nifaces(Type *t) { (void)t; return 0; }
static Type *WINAPI t_iface(Type *t, UINT i);
static HRESULT WINAPI t_oftype(Type *t, Type *o);
static HRESULT WINAPI t_implements(Type *t, Type *o) { (void)t; (void)o; return S_FALSE; }
static const TypeVtbl g_null_type_vtbl = { nt_desc, t_member, t_member_name, t_member_tname, t_equal, t_none, t_none,
                                           t_nifaces, t_iface, t_oftype, t_implements };
static Type g_null_type = { &g_null_type_vtbl };

static HRESULT WINAPI nv_desc(Var *v, VARIABLE_DESC *d) { (void)v; (void)d; return E_FAIL; }
static Type *WINAPI v_type(Var *v);
static CBuf *WINAPI v_buf(Var *v);
static UINT WINAPI v_slot(Var *v, UINT i) { (void)v; (void)i; return ~0u; }
static const VarVtbl g_null_var_vtbl = { nv_desc, v_type, v_buf, v_slot };
static HRESULT WINAPI nc_desc(CBuf *c, BUFFER_DESC *d) { (void)c; (void)d; return E_FAIL; }
static Var *WINAPI c_var(CBuf *c, UINT i);
static Var *WINAPI c_var_name(CBuf *c, LPCSTR name);
static const CBufVtbl g_null_cbuf_vtbl = { nc_desc, c_var, c_var_name };
static CBuf g_null_cbuf = { &g_null_cbuf_vtbl };
static Var g_null_var = { &g_null_var_vtbl, { 0 }, &g_null_type, &g_null_cbuf };

/* ---- types ---- */
static HRESULT WINAPI t_desc(Type *t, TYPE_DESC *d)
{
    if (!d) return E_FAIL;
    *d = t->desc;
    return S_OK;
}
static Type *WINAPI t_member(Type *t, UINT i) { return t->members && i < t->desc.Members ? t->members[i] : &g_null_type; }
static Type *WINAPI t_member_name(Type *t, LPCSTR name)
{
    if (name && t->members)
        for (UINT i = 0; i < t->desc.Members; i++)
            if (t->member_names[i] && !strcmp(t->member_names[i], name)) return t->members[i];
    return &g_null_type;
}
static LPCSTR WINAPI t_member_tname(Type *t, UINT i) { return t->members && i < t->desc.Members ? t->member_names[i] : NULL; }
static HRESULT WINAPI t_equal(Type *t, Type *o) { return o && t == o && t != &g_null_type ? S_OK : S_FALSE; }
static Type *WINAPI t_none(Type *t) { (void)t; return NULL; }
static Type *WINAPI t_iface(Type *t, UINT i) { (void)t; (void)i; return NULL; }
static HRESULT WINAPI t_oftype(Type *t, Type *o) { return t_equal(t, o); }
static const TypeVtbl g_type_vtbl = { t_desc, t_member, t_member_name, t_member_tname, t_equal, t_none, t_none,
                                      t_nifaces, t_iface, t_oftype, t_implements };

/* ---- variables and constant buffers ---- */
static HRESULT WINAPI v_desc(Var *v, VARIABLE_DESC *d)
{
    if (!d) return E_FAIL;
    *d = v->desc;
    return S_OK;
}
static Type *WINAPI v_type(Var *v) { return v->type ? v->type : &g_null_type; }
static CBuf *WINAPI v_buf(Var *v) { return v->buf; }
static const VarVtbl g_var_vtbl = { v_desc, v_type, v_buf, v_slot };

static HRESULT WINAPI c_desc(CBuf *c, BUFFER_DESC *d)
{
    if (!d) return E_FAIL;
    *d = c->desc;
    return S_OK;
}
static Var *WINAPI c_var(CBuf *c, UINT i) { return c->vars && i < c->desc.Variables ? &c->vars[i] : &g_null_var; }
static Var *WINAPI c_var_name(CBuf *c, LPCSTR name)
{
    if (name && c->vars)
        for (UINT i = 0; i < c->desc.Variables; i++)
            if (!strcmp(c->vars[i].desc.Name, name)) return &c->vars[i];
    return &g_null_var;
}
static const CBufVtbl g_cbuf_vtbl = { c_desc, c_var, c_var_name };

/* ---- the reflection object ---- */
static void *zalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void zfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static HRESULT WINAPI r_qi(Refl *r, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(iid, &IID_IUnknown_, sizeof(GUID)) || reflection_iid(iid)) {
        InterlockedIncrement(&r->refs);
        *out = r;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI r_addref(Refl *r) { return InterlockedIncrement(&r->refs); }
static ULONG WINAPI r_release(Refl *r)
{
    LONG n = InterlockedDecrement(&r->refs);
    if (n) return n;
    for (UINT i = 0; r->cbufs && i < r->desc.ConstantBuffers; i++) zfree(r->cbufs[i].vars);
    zfree(r->cbufs);
    zfree(r->binds);
    zfree(r->in.p);
    zfree(r->out.p);
    zfree(r->pc.p);
    while (r->types) {
        Type *t = r->types;
        r->types = t->next;
        zfree(t->members);
        zfree(t->member_names);
        zfree(t);
    }
    zfree(r->data);
    zfree(r);
    return 0;
}
static HRESULT WINAPI r_desc(Refl *r, SHADER_DESC *d)
{
    if (!d) return E_FAIL;
    *d = r->desc;
    return S_OK;
}
static CBuf *WINAPI r_cbuf(Refl *r, UINT i) { return i < r->desc.ConstantBuffers ? &r->cbufs[i] : &g_null_cbuf; }
static CBuf *WINAPI r_cbuf_name(Refl *r, LPCSTR name)
{
    if (name)
        for (UINT i = 0; i < r->desc.ConstantBuffers; i++)
            if (!strcmp(r->cbufs[i].desc.Name, name)) return &r->cbufs[i];
    return &g_null_cbuf;
}
static HRESULT WINAPI r_bind(Refl *r, UINT i, BIND_DESC *d)
{
    if (!d || i >= r->desc.BoundResources) return E_INVALIDARG;
    *d = r->binds[i];
    return S_OK;
}
static HRESULT sig_get(Sig *s, UINT i, PARAM_DESC *d)
{
    if (!d || i >= s->n) return E_INVALIDARG;
    *d = s->p[i];
    return S_OK;
}
static HRESULT WINAPI r_in(Refl *r, UINT i, PARAM_DESC *d) { return sig_get(&r->in, i, d); }
static HRESULT WINAPI r_out(Refl *r, UINT i, PARAM_DESC *d) { return sig_get(&r->out, i, d); }
static HRESULT WINAPI r_pc(Refl *r, UINT i, PARAM_DESC *d) { return sig_get(&r->pc, i, d); }
static Var *WINAPI r_var_name(Refl *r, LPCSTR name)
{
    for (UINT i = 0; name && i < r->desc.ConstantBuffers; i++) {
        Var *v = c_var_name(&r->cbufs[i], name);
        if (v != &g_null_var) return v;
    }
    return &g_null_var;
}
static HRESULT WINAPI r_bind_name(Refl *r, LPCSTR name, BIND_DESC *d)
{
    if (!d || !name) return E_INVALIDARG;
    for (UINT i = 0; i < r->desc.BoundResources; i++)
        if (!strcmp(r->binds[i].Name, name)) { *d = r->binds[i]; return S_OK; }
    return E_INVALIDARG;
}
static UINT WINAPI r_mov(Refl *r) { return r->mov; }
static UINT WINAPI r_movc(Refl *r) { (void)r; return 0; }
static UINT WINAPI r_conversion(Refl *r) { return r->conversion; }
static UINT WINAPI r_bitwise(Refl *r) { (void)r; return 0; }
static UINT WINAPI r_gs_input(Refl *r) { return r->desc.InputPrimitive; }
static BOOL WINAPI r_sample_rate(Refl *r) { return r->sample_rate; }
static UINT WINAPI r_slots(Refl *r) { return r->slots; }
static HRESULT WINAPI r_level(Refl *r, UINT *level)
{
    if (!level) return E_INVALIDARG;
    *level = r->level;
    return S_OK;
}
static UINT WINAPI r_threads(Refl *r, UINT *x, UINT *y, UINT *z)
{
    if (x) *x = r->tgx;
    if (y) *y = r->tgy;
    if (z) *z = r->tgz;
    return r->tgx * r->tgy * r->tgz;
}
static UINT64 WINAPI r_requires(Refl *r) { return r->requires; }
static const ReflVtbl g_refl_vtbl = {
    r_qi, r_addref, r_release, r_desc, r_cbuf, r_cbuf_name, r_bind, r_in, r_out, r_pc, r_var_name, r_bind_name,
    r_mov, r_movc, r_conversion, r_bitwise, r_gs_input, r_sample_rate, r_slots, r_level, r_threads, r_requires,
};

/* ---- parsing ---- */
typedef struct { const BYTE *p; DWORD n; } Chunk;

static DWORD rd32(const BYTE *p) { DWORD v; memcpy(&v, p, 4); return v; }
static WORD rd16(const BYTE *p) { WORD v; memcpy(&v, p, 2); return v; }

/* a NUL-terminated string at @off in @c, or "" */
static LPCSTR str_at(const Chunk *c, DWORD off)
{
    if (off >= c->n || !memchr(c->p + off, 0, c->n - off)) return "";
    return (LPCSTR)(c->p + off);
}

static BOOL find_chunk(const BYTE *data, SIZE_T size, DWORD tag, Chunk *c)
{
    if (size < 32) return FALSE;
    DWORD count = rd32(data + 28);
    if (count > (size - 32) / 4) return FALSE;
    for (DWORD i = 0; i < count; i++) {
        DWORD off = rd32(data + 32 + 4 * i);
        if (off > size - 8) return FALSE;
        DWORD len = rd32(data + off + 4);
        if (len > size - off - 8) return FALSE;
        if (rd32(data + off) == tag) { c->p = data + off + 8; c->n = len; return TRUE; }
    }
    return FALSE;
}

static Type *parse_type(Refl *r, const Chunk *c, DWORD at, BOOL sm5, int depth)
{
    for (Type *t = r->types; t; t = t->next)
        if (t->at == at) return t;
    DWORD size = sm5 ? 36 : 16;
    if (at > c->n || c->n - at < size || depth > 32) return NULL;
    Type *t = zalloc(sizeof(Type));
    if (!t) return NULL;
    const BYTE *p = c->p + at;
    t->vtbl = &g_type_vtbl;
    t->at = at;
    t->desc.Class = rd16(p);
    t->desc.Type = rd16(p + 2);
    t->desc.Rows = rd16(p + 4);
    t->desc.Columns = rd16(p + 6);
    t->desc.Elements = rd16(p + 8);
    t->desc.Members = rd16(p + 10);
    DWORD members = rd32(p + 12);
    t->desc.Name = sm5 ? str_at(c, rd32(p + 32)) : NULL;
    if (sm5 && !*t->desc.Name) t->desc.Name = NULL;
    t->next = r->types;
    r->types = t;
    if (t->desc.Members) {
        t->members = zalloc(t->desc.Members * sizeof(Type *));
        t->member_names = zalloc(t->desc.Members * sizeof(LPCSTR));
        if (!t->members || !t->member_names || members > c->n || (c->n - members) / 12 < t->desc.Members) {
            t->desc.Members = 0;
            return t;
        }
        for (UINT i = 0; i < t->desc.Members; i++) {
            const BYTE *m = c->p + members + 12 * i;
            t->member_names[i] = str_at(c, rd32(m));
            Type *mt = parse_type(r, c, rd32(m + 4), sm5, depth + 1);
            if (mt && mt->desc.Offset == 0 && rd32(m + 8)) {
                /* a member type's Offset is its place in the struct; a type
                 * used at two places gets an object per place */
                Type *copy = zalloc(sizeof(Type));
                if (copy) {
                    *copy = *mt;
                    copy->at = ~0u;                 /* not found again by offset */
                    copy->members = NULL;
                    copy->member_names = NULL;
                    if (mt->desc.Members) {
                        copy->members = zalloc(mt->desc.Members * sizeof(Type *));
                        copy->member_names = zalloc(mt->desc.Members * sizeof(LPCSTR));
                        if (copy->members && copy->member_names) {
                            memcpy(copy->members, mt->members, mt->desc.Members * sizeof(Type *));
                            memcpy(copy->member_names, mt->member_names, mt->desc.Members * sizeof(LPCSTR));
                        } else {
                            copy->desc.Members = 0;
                        }
                    }
                    copy->desc.Offset = rd32(m + 8);
                    copy->next = r->types;
                    r->types = copy;
                    mt = copy;
                }
            }
            t->members[i] = mt ? mt : &g_null_type;
        }
    }
    return t;
}

static BOOL parse_rdef(Refl *r, const Chunk *c)
{
    if (c->n < 28) return FALSE;
    DWORD ncb = rd32(c->p), cboff = rd32(c->p + 4), nbind = rd32(c->p + 8), bindoff = rd32(c->p + 12);
    DWORD target = rd32(c->p + 16);
    BOOL sm5 = (target >> 8 & 0xff) >= 5;
    DWORD cbsize = 24, bindsize = 32, varsize = sm5 ? 40 : 24;
    if (sm5 && c->n >= 60 && rd32(c->p + 28) == DXBC_TAG('R', 'D', '1', '1')) {
        cbsize = rd32(c->p + 36);
        bindsize = rd32(c->p + 40);
        varsize = rd32(c->p + 44);
        r->slots = rd32(c->p + 56);
        if (cbsize < 24 || bindsize < 32 || varsize < 24) return FALSE;
    }
    r->desc.Flags = rd32(c->p + 20);
    r->desc.Creator = str_at(c, rd32(c->p + 24));

    if (nbind) {
        if (bindoff > c->n || (c->n - bindoff) / bindsize < nbind) return FALSE;
        if (!(r->binds = zalloc(nbind * sizeof(BIND_DESC)))) return FALSE;
        for (DWORD i = 0; i < nbind; i++) {
            const BYTE *p = c->p + bindoff + bindsize * i;
            BIND_DESC *b = &r->binds[i];
            b->Name = str_at(c, rd32(p));
            b->Type = rd32(p + 4);
            b->ReturnType = rd32(p + 8);
            b->Dimension = rd32(p + 12);
            b->NumSamples = rd32(p + 16);
            b->BindPoint = rd32(p + 20);
            b->BindCount = rd32(p + 24);
            b->uFlags = rd32(p + 28);
        }
        r->desc.BoundResources = nbind;
    }
    if (ncb) {
        if (cboff > c->n || (c->n - cboff) / cbsize < ncb) return FALSE;
        if (!(r->cbufs = zalloc(ncb * sizeof(CBuf)))) return FALSE;
        r->desc.ConstantBuffers = ncb;
        for (DWORD i = 0; i < ncb; i++) {
            const BYTE *p = c->p + cboff + cbsize * i;
            CBuf *cb = &r->cbufs[i];
            cb->vtbl = &g_cbuf_vtbl;
            cb->desc.Name = str_at(c, rd32(p));
            DWORD nvar = rd32(p + 4), varoff = rd32(p + 8);
            cb->desc.Size = rd32(p + 12);
            cb->desc.uFlags = rd32(p + 16);
            cb->desc.Type = rd32(p + 20);
            if (!nvar) continue;
            if (varoff > c->n || (c->n - varoff) / varsize < nvar) return FALSE;
            if (!(cb->vars = zalloc(nvar * sizeof(Var)))) return FALSE;
            cb->desc.Variables = nvar;
            for (DWORD j = 0; j < nvar; j++) {
                const BYTE *v = c->p + varoff + varsize * j;
                Var *var = &cb->vars[j];
                var->vtbl = &g_var_vtbl;
                var->buf = cb;
                var->desc.Name = str_at(c, rd32(v));
                var->desc.StartOffset = rd32(v + 4);
                var->desc.Size = rd32(v + 8);
                var->desc.uFlags = rd32(v + 12);
                DWORD def = rd32(v + 20);
                var->desc.DefaultValue = def && def <= c->n && c->n - def >= var->desc.Size ? (LPVOID)(c->p + def) : NULL;
                if (varsize >= 40) {
                    var->desc.StartTexture = rd32(v + 24);
                    var->desc.TextureSize = rd32(v + 28);
                    var->desc.StartSampler = rd32(v + 32);
                    var->desc.SamplerSize = rd32(v + 36);
                } else {
                    var->desc.StartTexture = var->desc.StartSampler = ~0u;
                }
                var->type = parse_type(r, c, rd32(v + 16), sm5, 0);
            }
        }
    }
    return TRUE;
}

/* a pixel shader's outputs: Windows reports the system value of SV_Target and SV_Depth* */
static UINT ps_output_name(LPCSTR name, UINT sysval)
{
    if (sysval) return sysval;
    if (!_strnicmp(name, "SV_Target", 9) || !_stricmp(name, "COLOR")) return 64;
    if (!_stricmp(name, "SV_Depth") || !_stricmp(name, "DEPTH")) return 65;
    if (!_stricmp(name, "SV_Coverage")) return 66;
    if (!_stricmp(name, "SV_DepthGreaterEqual")) return 67;
    if (!_stricmp(name, "SV_DepthLessEqual")) return 68;
    return sysval;
}

static BOOL parse_sig(Sig *s, const Chunk *c, DWORD tag, BOOL ps_out)
{
    if (c->n < 8) return FALSE;
    DWORD n = rd32(c->p), size = 24;
    BOOL stream = FALSE, prec = FALSE;
    if (tag == DXBC_TAG('O','S','G','5')) { size = 28; stream = TRUE; }
    else if (tag == DXBC_TAG('I','S','G','1') || tag == DXBC_TAG('O','S','G','1') || tag == DXBC_TAG('P','S','G','1')) {
        size = 32; stream = prec = TRUE;
    }
    if ((c->n - 8) / size < n) return FALSE;
    if (n && !(s->p = zalloc(n * sizeof(PARAM_DESC)))) return FALSE;
    s->n = n;
    for (DWORD i = 0; i < n; i++) {
        const BYTE *p = c->p + 8 + size * i;
        PARAM_DESC *d = &s->p[i];
        if (stream) { d->Stream = rd32(p); p += 4; }
        d->SemanticName = str_at(c, rd32(p));
        d->SemanticIndex = rd32(p + 4);
        d->SystemValueType = rd32(p + 8);
        d->ComponentType = rd32(p + 12);
        d->Register = rd32(p + 16);
        d->Mask = p[20];
        d->ReadWriteMask = p[21];
        if (prec) d->MinPrecision = rd32(p + 24);
        if (ps_out) d->SystemValueType = ps_output_name(d->SemanticName, d->SystemValueType);
    }
    return TRUE;
}

static void parse_stat(Refl *r, const Chunk *c)
{
    DWORD v[37] = { 0 };
    DWORD n = c->n / 4 < 37 ? c->n / 4 : 37;
    for (DWORD i = 0; i < n; i++) v[i] = rd32(c->p + 4 * i);
    SHADER_DESC *d = &r->desc;
    d->InstructionCount = v[0];
    d->TempRegisterCount = v[1];
    d->DefCount = v[2];
    d->DclCount = v[3];
    d->FloatInstructionCount = v[4];
    d->IntInstructionCount = v[5];
    d->UintInstructionCount = v[6];
    d->StaticFlowControlCount = v[7];
    d->DynamicFlowControlCount = v[8];
    d->MacroInstructionCount = v[9];
    d->TempArrayCount = v[10];
    d->ArrayInstructionCount = v[11];
    d->CutInstructionCount = v[12];
    d->EmitInstructionCount = v[13];
    d->TextureNormalInstructions = v[14];
    d->TextureLoadInstructions = v[15];
    d->TextureCompInstructions = v[16];
    d->TextureBiasInstructions = v[17];
    d->TextureGradientInstructions = v[18];
    r->mov = v[19];
    r->conversion = v[21];
    d->InputPrimitive = v[23];
    d->GSOutputTopology = v[24];
    d->GSMaxOutputVertexCount = v[25];
    if (n >= 37) {
        d->cGSInstanceCount = v[29];
        d->cControlPoints = v[30];
        d->HSOutputPrimitive = v[31];
        d->HSPartitioning = v[32];
        d->TessellatorDomain = v[33];
        d->cBarrierInstructions = v[34];
        d->cInterlockedInstructions = v[35];
        d->cTextureStoreInstructions = v[36];
    }
}

/* the program: version, thread group, sample-rate inputs, early depth */
static BOOL parse_shex(Refl *r, const Chunk *c)
{
    if (c->n < 8) return FALSE;
    DWORD version = rd32(c->p), len = rd32(c->p + 4);
    if (len > c->n / 4) len = c->n / 4;
    r->desc.Version = version;
    UINT major = version >> 4 & 0xf, minor = version & 0xf;
    r->level = major >= 5 ? 0xb000 : minor ? 0xa100 : 0xa000;
    for (DWORD i = 2; i < len;) {
        DWORD tok = rd32(c->p + 4 * i), op = tok & 0x7ff, n = tok >> 24 & 0x7f;
        if (op == 0x35) n = i + 1 < len ? rd32(c->p + 4 * (i + 1)) : 0;       /* customdata */
        if (!n) break;
        if (op == 0x9b && n >= 4 && i + 3 < len) {                             /* dcl_thread_group */
            r->tgx = rd32(c->p + 4 * (i + 1));
            r->tgy = rd32(c->p + 4 * (i + 2));
            r->tgz = rd32(c->p + 4 * (i + 3));
        } else if (op == 0x62 || op == 0x63 || op == 0x64) {                   /* dcl_input_ps(_sgv/_siv) */
            UINT mode = tok >> 11 & 0xf;
            if (mode == 6 || mode == 7) r->sample_rate = TRUE;
        } else if (op == 0x6a && (tok >> 11 & 4)) {                            /* dcl_globalFlags forceEarlyDepthStencil */
            r->requires |= 2;
        }
        i += n;
    }
    return TRUE;
}

D3DCAPI HRESULT WINAPI D3DReflect(LPCVOID data, SIZE_T size, REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!data || size < 32 || rd32(data) != DXBC_TAG('D', 'X', 'B', 'C') || rd32((const BYTE *)data + 24) > size)
        return E_FAIL;
    if (!iid || !reflection_iid(iid)) return E_NOINTERFACE;
    Refl *r = zalloc(sizeof(Refl));
    if (!r) return E_OUTOFMEMORY;
    r->vtbl = &g_refl_vtbl;
    r->refs = 1;
    r->size = rd32((const BYTE *)data + 24);
    if (!(r->data = zalloc(r->size))) { zfree(r); return E_OUTOFMEMORY; }
    memcpy(r->data, data, r->size);

    Chunk c;
    BOOL ok = TRUE;
    if (find_chunk(r->data, r->size, DXBC_TAG('S','H','E','X'), &c) || find_chunk(r->data, r->size, DXBC_TAG('S','H','D','R'), &c))
        ok = parse_shex(r, &c);
    if (find_chunk(r->data, r->size, DXBC_TAG('A','o','n','9'), &c)) r->level = 0x9100;
    BOOL ps = (r->desc.Version >> 16) == 0;
    static const DWORD in_tags[] = { DXBC_TAG('I','S','G','1'), DXBC_TAG('I','S','G','N') };
    static const DWORD out_tags[] = { DXBC_TAG('O','S','G','1'), DXBC_TAG('O','S','G','5'), DXBC_TAG('O','S','G','N') };
    static const DWORD pc_tags[] = { DXBC_TAG('P','S','G','1'), DXBC_TAG('P','C','S','G') };
    for (unsigned i = 0; ok && i < 2; i++)
        if (find_chunk(r->data, r->size, in_tags[i], &c)) { ok = parse_sig(&r->in, &c, in_tags[i], FALSE); break; }
    for (unsigned i = 0; ok && i < 3; i++)
        if (find_chunk(r->data, r->size, out_tags[i], &c)) { ok = parse_sig(&r->out, &c, out_tags[i], ps); break; }
    for (unsigned i = 0; ok && i < 2; i++)
        if (find_chunk(r->data, r->size, pc_tags[i], &c)) { ok = parse_sig(&r->pc, &c, pc_tags[i], FALSE); break; }
    r->desc.InputParameters = r->in.n;
    r->desc.OutputParameters = r->out.n;
    r->desc.PatchConstantParameters = r->pc.n;
    for (UINT i = 0; ps && i < r->in.n; i++)
        if (r->in.p[i].SystemValueType == 10) r->sample_rate = TRUE;          /* SV_SampleIndex */
    if (ok && find_chunk(r->data, r->size, DXBC_TAG('R','D','E','F'), &c)) ok = parse_rdef(r, &c);
    if (ok && find_chunk(r->data, r->size, DXBC_TAG('S','T','A','T'), &c)) parse_stat(r, &c);
    if (ok && find_chunk(r->data, r->size, DXBC_TAG('S','F','I','0'), &c) && c.n >= 4)
        r->requires |= rd32(c.p) & ~2u;          /* the feature bits are the D3D_SHADER_REQUIRES_ ones, but bit 1 */
    if (!ok) {
        r_release(r);
        return E_FAIL;
    }
    *out = r;
    return S_OK;
}
