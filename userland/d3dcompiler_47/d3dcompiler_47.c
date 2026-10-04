/*
 * d3dcompiler_47.dll — the HLSL compiler.  Compiling, preprocessing and
 * disassembling go to vkd3d-shader (third_party/vkd3d-shader, Wine's HLSL
 * compiler, LGPL-2.1), compiled into this DLL and called only through its
 * public API (vkd3d_shader.h); this file is NovaOS's own (MIT): the D3D
 * blob, the d3dcompiler entry points on top of vkd3d-shader, and the DXBC
 * container parts (signatures, stripping, private data).  Reflection is in
 * reflect.c.  Profiles: vs/ps/gs/hs/ds/cs 4_0 to 5_1 compile to DXBC (TPF),
 * 1_x to 3_0 to Direct3D 9 bytecode, fx_2_0 to fx_5_0 to effects.
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <vkd3d_shader.h>
#include "d3dcompiler.h"

int _fltused = 0x9875;                  /* floating point in use (the compiler references it) */

/* ---- ID3DBlob (ID3D10Blob) ---- */
static HRESULT WINAPI b_qi(Blob *b, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (!memcmp(iid, &IID_IUnknown_, sizeof(GUID)) || !memcmp(iid, &IID_ID3DBlob_, sizeof(GUID))) {
        InterlockedIncrement(&b->refs);
        *out = b;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI b_addref(Blob *b) { return InterlockedIncrement(&b->refs); }
static ULONG WINAPI b_release(Blob *b)
{
    LONG n = InterlockedDecrement(&b->refs);
    if (!n) HeapFree(GetProcessHeap(), 0, b);
    return n;
}
static LPVOID WINAPI b_ptr(Blob *b) { return b->data; }
static SIZE_T WINAPI b_size(Blob *b) { return b->size; }
static const BlobVtbl g_blob = { b_qi, b_addref, b_release, b_ptr, b_size };

D3DCAPI HRESULT WINAPI D3DCreateBlob(SIZE_T size, void **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    Blob *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Blob) + size);
    if (!b) { *out = NULL; return E_OUTOFMEMORY; }
    b->vtbl = &g_blob;
    b->refs = 1;
    b->size = size;
    *out = b;
    return S_OK;
}

/* a blob holding @n bytes of @data */
HRESULT d3dc_blob(const void *data, SIZE_T n, void **out)
{
    HRESULT hr = D3DCreateBlob(n, out);
    if (SUCCEEDED(hr) && n) memcpy(((Blob *)*out)->data, data, n);
    return hr;
}

static HRESULT from_vkd3d(int ret)
{
    switch (ret) {
    case VKD3D_OK: return S_OK;
    case VKD3D_ERROR_OUT_OF_MEMORY: return E_OUTOFMEMORY;
    case VKD3D_ERROR_INVALID_ARGUMENT: return E_INVALIDARG;
    case VKD3D_ERROR_NOT_IMPLEMENTED: return E_NOTIMPL;
    default: return E_FAIL;
    }
}

/* the compiler's messages as an error blob: a NUL-terminated string, as the real one's */
static void messages_blob(char *messages, void **errors)
{
    if (errors) {
        *errors = NULL;
        if (messages && *messages) d3dc_blob(messages, strlen(messages) + 1, errors);
    }
    vkd3d_shader_free_messages(messages);
}

/* ---- #include: an ID3DInclude, or D3D_COMPILE_STANDARD_FILE_INCLUDE ---- */
typedef struct ID3DInclude ID3DInclude;
typedef struct {
    HRESULT (WINAPI *Open)(ID3DInclude *, int type, LPCSTR name, LPCVOID parent, LPCVOID *data, UINT *bytes);
    HRESULT (WINAPI *Close)(ID3DInclude *, LPCVOID data);
} ID3DIncludeVtbl;
struct ID3DInclude { const ID3DIncludeVtbl *lpVtbl; };
#define STANDARD_FILE_INCLUDE ((ID3DInclude *)(UINT_PTR)1)

/* the standard include handler: a file's includes are looked for beside it
 * (the first file's beside @dir, the source file's folder or the current one) */
typedef struct Opened { struct Opened *next; char *data; char dir[MAX_PATH]; } Opened;
typedef struct { ID3DInclude *iface; char dir[MAX_PATH]; Opened *open; CRITICAL_SECTION lock; } Includes;

static void dir_of(const char *path, char *dir)
{
    const char *s = path, *slash = NULL;
    for (; *s; s++) if (*s == '\\' || *s == '/') slash = s;
    size_t n = slash ? (size_t)(slash - path) + 1 : 0;
    if (n >= MAX_PATH) n = 0;
    memcpy(dir, path, n);
    dir[n] = 0;
}

static int std_open(Includes *inc, const char *name, const char *parent, struct vkd3d_shader_code *code)
{
    char path[MAX_PATH * 2];
    const char *base = inc->dir;
    EnterCriticalSection(&inc->lock);
    for (Opened *o = inc->open; o; o = o->next)
        if (parent && o->data == parent) base = o->dir;
    BOOL absolute = name[0] == '\\' || name[0] == '/' || (name[0] && name[1] == ':');
    snprintf(path, sizeof(path), "%s%s", absolute ? "" : base, name);
    LeaveCriticalSection(&inc->lock);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return VKD3D_ERROR;
    DWORD size = GetFileSize(f, NULL), got = 0;
    Opened *o = HeapAlloc(GetProcessHeap(), 0, sizeof(Opened));
    char *data = o && size != INVALID_FILE_SIZE ? HeapAlloc(GetProcessHeap(), 0, size + 1) : NULL;
    if (!data || !ReadFile(f, data, size, &got, NULL) || got != size) {
        CloseHandle(f);
        if (data) HeapFree(GetProcessHeap(), 0, data);
        if (o) HeapFree(GetProcessHeap(), 0, o);
        return VKD3D_ERROR;
    }
    CloseHandle(f);
    data[size] = 0;
    o->data = data;
    dir_of(path, o->dir);
    EnterCriticalSection(&inc->lock);
    o->next = inc->open;
    inc->open = o;
    LeaveCriticalSection(&inc->lock);
    code->code = data;
    code->size = size;
    return VKD3D_OK;
}

static void std_close(Includes *inc, const void *data)
{
    EnterCriticalSection(&inc->lock);
    for (Opened **p = &inc->open; *p; p = &(*p)->next)
        if ((*p)->data == data) {
            Opened *o = *p;
            *p = o->next;
            HeapFree(GetProcessHeap(), 0, o->data);
            HeapFree(GetProcessHeap(), 0, o);
            break;
        }
    LeaveCriticalSection(&inc->lock);
}

static int open_include(const char *name, bool local, const char *parent, void *context, struct vkd3d_shader_code *code)
{
    Includes *inc = context;
    memset(code, 0, sizeof(*code));
    if (!inc->iface) return VKD3D_ERROR;                 /* no handler: #include fails, as on Windows */
    if (inc->iface == STANDARD_FILE_INCLUDE) return std_open(inc, name, parent, code);
    LPCVOID data = NULL;
    UINT size = 0;
    if (FAILED(inc->iface->lpVtbl->Open(inc->iface, local ? 0 : 1, name, parent, &data, &size)))
        return VKD3D_ERROR;
    code->code = data;
    code->size = size;
    return VKD3D_OK;
}

static void close_include(const struct vkd3d_shader_code *code, void *context)
{
    Includes *inc = context;
    if (inc->iface == STANDARD_FILE_INCLUDE) std_close(inc, code->code);
    else if (inc->iface) inc->iface->lpVtbl->Close(inc->iface, code->code);
}

static void includes_init(Includes *inc, ID3DInclude *iface, const char *file)
{
    inc->iface = iface;
    inc->open = NULL;
    inc->dir[0] = 0;
    if (file) dir_of(file, inc->dir);
    InitializeCriticalSection(&inc->lock);
}

static void includes_done(Includes *inc)
{
    while (inc->open) std_close(inc, inc->open->data);
    DeleteCriticalSection(&inc->lock);
}

static void preprocess_info(struct vkd3d_shader_preprocess_info *pp, const D3D_SHADER_MACRO *defines, Includes *inc)
{
    memset(pp, 0, sizeof(*pp));
    pp->type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    pp->macros = (const struct vkd3d_shader_macro *)defines;   /* the same layout: two string pointers */
    if (defines) while (defines[pp->macro_count].Name) pp->macro_count++;
    pp->pfn_open_include = open_include;
    pp->pfn_close_include = close_include;
    pp->include_context = inc;
}

/* ---- compiling ---- */
static enum vkd3d_shader_target_type target_of(const char *profile)
{
    static const char *const d3dbc[] = { "vs_1_", "vs_2_", "vs_3_", "ps_1_", "ps_2_", "ps_3_", "tx_1_",
                                         "vs.1.", "vs.2.", "vs.3.", "ps.1.", "ps.2.", "ps.3." };
    for (unsigned i = 0; i < sizeof(d3dbc) / sizeof(*d3dbc); i++)
        if (!strncmp(profile, d3dbc[i], 5)) return VKD3D_SHADER_TARGET_D3D_BYTECODE;
    if (!strncmp(profile, "fx_", 3)) return VKD3D_SHADER_TARGET_FX;
    return VKD3D_SHADER_TARGET_DXBC_TPF;
}

static HRESULT compile(LPCVOID src, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                       const char *file, LPCSTR entry, LPCSTR profile, UINT flags1, UINT flags2,
                       LPCVOID secdata, SIZE_T secdata_size, void **code, void **errors)
{
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    if (!src || !profile) return E_INVALIDARG;

    Includes inc;
    includes_init(&inc, include, file);
    struct vkd3d_shader_preprocess_info pp;
    preprocess_info(&pp, defines, &inc);
    struct vkd3d_shader_hlsl_source_info hlsl = { VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO };
    hlsl.profile = profile;
    hlsl.entry_point = entry ? entry : "main";
    hlsl.secondary_code.code = secdata;
    hlsl.secondary_code.size = secdata_size;
    pp.next = &hlsl;

    struct vkd3d_shader_compile_option opts[8];
    unsigned nopts = 0;
    opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_API_VERSION,
                                                          VKD3D_SHADER_API_VERSION_1_19 };
    if (!(flags1 & D3DCOMPILE_DEBUG))
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1 };
    if (flags1 & (D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR))
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER,
            flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR ? VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR
                                                      : VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR };
    if (flags1 & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY,
                                                              VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES };
    if (flags2 & D3DCOMPILE_EFFECT_CHILD_EFFECT)
        opts[nopts++] = (struct vkd3d_shader_compile_option){ VKD3D_SHADER_COMPILE_OPTION_CHILD_EFFECT, 1 };

    struct vkd3d_shader_compile_info info = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO };
    info.next = &pp;
    info.source.code = src;
    info.source.size = n;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = target_of(profile);
    info.options = opts;
    info.option_count = nopts;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = name;

    struct vkd3d_shader_code out = { 0 };
    char *messages = NULL;
    int ret = vkd3d_shader_compile(&info, &out, &messages);
    includes_done(&inc);
    messages_blob(messages, errors);
    if (ret < 0) return from_vkd3d(ret);

    HRESULT hr;
    if (!strcmp(profile, "fx_4_0") || !strcmp(profile, "fx_4_1")) {
        /* fx_4_x effects come in a DXBC container (FX10), unlike fx_2_0 and fx_5_0 */
        struct vkd3d_shader_dxbc_section_desc fx = { DXBC_TAG('F', 'X', '1', '0'), out };
        struct vkd3d_shader_code dxbc;
        ret = vkd3d_shader_serialize_dxbc(1, &fx, &dxbc, NULL);
        vkd3d_shader_free_shader_code(&out);
        if (ret < 0) return from_vkd3d(ret);
        out = dxbc;
    }
    hr = code ? d3dc_blob(out.code, out.size, code) : S_OK;
    vkd3d_shader_free_shader_code(&out);
    return hr;
}

D3DCAPI HRESULT WINAPI D3DCompile2(LPCVOID src, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO *defines, void *include,
                                   LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, UINT secdata_flags,
                                   LPCVOID secdata, SIZE_T secdata_size, void **code, void **errors)
{
    (void)secdata_flags;
    return compile(src, n, name, defines, include, name, entry, target, flags1, flags2, secdata, secdata_size,
                   code, errors);
}

D3DCAPI HRESULT WINAPI D3DCompile(LPCVOID src, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO *defines, void *include,
                                  LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, void **code, void **errors)
{
    return compile(src, n, name, defines, include, name, entry, target, flags1, flags2, NULL, 0, code, errors);
}

/* a whole file, NUL-terminated; NULL if it cannot be read */
static char *read_file(LPCWSTR file, DWORD *size)
{
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(f, NULL), got = 0;
    char *data = n != INVALID_FILE_SIZE ? HeapAlloc(GetProcessHeap(), 0, n + 1) : NULL;
    if (data && (!ReadFile(f, data, n, &got, NULL) || got != n)) {
        HeapFree(GetProcessHeap(), 0, data);
        data = NULL;
    }
    CloseHandle(f);
    if (data) { data[n] = 0; *size = n; }
    return data;
}

D3DCAPI HRESULT WINAPI D3DCompileFromFile(LPCWSTR file, const D3D_SHADER_MACRO *defines, void *include, LPCSTR entry,
                                          LPCSTR target, UINT flags1, UINT flags2, void **code, void **errors)
{
    char name[MAX_PATH * 3];
    DWORD size = 0;
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    if (!file) return E_INVALIDARG;
    char *src = read_file(file, &size);
    if (!src) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_FILE_NOT_FOUND);
    if (!WideCharToMultiByte(CP_ACP, 0, file, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
    HRESULT hr = compile(src, size, name, defines, include, name, entry, target, flags1, flags2, NULL, 0,
                         code, errors);
    HeapFree(GetProcessHeap(), 0, src);
    return hr;
}

D3DCAPI HRESULT WINAPI D3DPreprocess(LPCVOID src, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO *defines,
                                     void *include, void **out, void **errors)
{
    if (out) *out = NULL;
    if (errors) *errors = NULL;
    if (!src) return E_INVALIDARG;
    Includes inc;
    includes_init(&inc, include, name);
    struct vkd3d_shader_preprocess_info pp;
    preprocess_info(&pp, defines, &inc);
    static const struct vkd3d_shader_compile_option opt = { VKD3D_SHADER_COMPILE_OPTION_API_VERSION,
                                                            VKD3D_SHADER_API_VERSION_1_19 };
    struct vkd3d_shader_compile_info info = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO };
    info.next = &pp;
    info.source.code = src;
    info.source.size = n;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_NONE;
    info.options = &opt;
    info.option_count = 1;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = name;
    struct vkd3d_shader_code text = { 0 };
    char *messages = NULL;
    int ret = vkd3d_shader_preprocess(&info, &text, &messages);
    includes_done(&inc);
    messages_blob(messages, errors);
    if (ret < 0) return from_vkd3d(ret);
    HRESULT hr = out ? d3dc_blob(text.code, text.size + 1, out) : S_OK;   /* with the NUL vkd3d-shader leaves after it */
    vkd3d_shader_free_shader_code(&text);
    return hr;
}

D3DCAPI HRESULT WINAPI D3DDisassemble(LPCVOID data, SIZE_T n, UINT flags, LPCSTR comments, void **out)
{
    (void)flags;
    if (out) *out = NULL;
    if (!data || !n || !out) return E_INVALIDARG;
    static const struct vkd3d_shader_compile_option opt = { VKD3D_SHADER_COMPILE_OPTION_API_VERSION,
                                                            VKD3D_SHADER_API_VERSION_1_19 };
    struct vkd3d_shader_compile_info info = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO };
    info.source.code = data;
    info.source.size = n;
    info.source_type = n >= 4 && *(const DWORD *)data == DXBC_TAG('D', 'X', 'B', 'C')
                       ? VKD3D_SHADER_SOURCE_DXBC_TPF : VKD3D_SHADER_SOURCE_D3D_BYTECODE;
    info.target_type = VKD3D_SHADER_TARGET_D3D_ASM;
    info.options = &opt;
    info.option_count = 1;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    struct vkd3d_shader_code text = { 0 };
    char *messages = NULL;
    int ret = vkd3d_shader_compile(&info, &text, &messages);
    vkd3d_shader_free_messages(messages);
    if (ret < 0) return from_vkd3d(ret);
    size_t pre = comments ? strlen(comments) + 1 : 0;      /* the caller's comment line first, as "//" lines do */
    HRESULT hr = D3DCreateBlob(pre + text.size + 1, out);
    if (SUCCEEDED(hr)) {
        char *p = (char *)((Blob *)*out)->data;
        if (pre) { memcpy(p, comments, pre - 1); p[pre - 1] = '\n'; }
        memcpy(p + pre, text.code, text.size);
        p[pre + text.size] = 0;
    }
    vkd3d_shader_free_shader_code(&text);
    return hr;
}

/* ---- DXBC parts ---- */
enum {
    PART_INPUT_SIGNATURE, PART_OUTPUT_SIGNATURE, PART_INPUT_AND_OUTPUT_SIGNATURE, PART_PATCH_CONSTANT_SIGNATURE,
    PART_ALL_SIGNATURE, PART_DEBUG_INFO, PART_LEGACY_SHADER, PART_XNA_PREPASS_SHADER, PART_XNA_SHADER, PART_PDB,
    PART_PRIVATE_DATA, PART_ROOT_SIGNATURE, PART_DEBUG_NAME,
};
#define T(a, b, c, d) DXBC_TAG(a, b, c, d)

/* which of a part's sections @tag is: 0 none, else 1 + its place */
static int part_slot(UINT part, DWORD tag)
{
    BOOL in = tag == T('I','S','G','N') || tag == T('I','S','G','1');
    BOOL out = tag == T('O','S','G','N') || tag == T('O','S','G','5') || tag == T('O','S','G','1');
    BOOL pc = tag == T('P','C','S','G') || tag == T('P','S','G','1');
    switch (part) {
    case PART_INPUT_SIGNATURE: return in;
    case PART_OUTPUT_SIGNATURE: return out;
    case PART_INPUT_AND_OUTPUT_SIGNATURE: return in ? 1 : out ? 2 : 0;
    case PART_PATCH_CONSTANT_SIGNATURE: return pc;
    case PART_ALL_SIGNATURE: return in ? 1 : out ? 2 : pc ? 3 : 0;
    case PART_DEBUG_INFO: return tag == T('S','D','B','G');
    case PART_LEGACY_SHADER: return tag == T('A','o','n','9');
    case PART_XNA_PREPASS_SHADER: return tag == T('X','N','A','P');
    case PART_XNA_SHADER: return tag == T('X','N','A','S');
    case PART_PRIVATE_DATA: return tag == T('P','R','I','V');
    case PART_ROOT_SIGNATURE: return tag == T('R','T','S','0');
    case PART_DEBUG_NAME: return tag == T('I','L','D','N');
    }
    return 0;
}

D3DCAPI HRESULT WINAPI D3DGetBlobPart(LPCVOID data, SIZE_T n, UINT part, UINT flags, void **out)
{
    if (out) *out = NULL;
    if (!data || !n || flags || !out || part > PART_DEBUG_NAME) return D3DERR_INVALIDCALL;
    struct vkd3d_shader_code src = { data, n };
    struct vkd3d_shader_dxbc_desc dxbc;
    if (vkd3d_shader_parse_dxbc(&src, 0, &dxbc, NULL) < 0) return D3DERR_INVALIDCALL;
    struct vkd3d_shader_dxbc_section_desc picked[3];
    unsigned want = part == PART_ALL_SIGNATURE ? 3 : part == PART_INPUT_AND_OUTPUT_SIGNATURE ? 2 : 1, got = 0;
    for (unsigned s = 1; s <= want; s++)                          /* in their usual order: in, out, patch constant */
        for (size_t i = 0; i < dxbc.section_count; i++)
            if (part_slot(part, dxbc.sections[i].tag) == (int)s) { picked[got++] = dxbc.sections[i]; break; }
    HRESULT hr = E_FAIL;
    if (got == want) {
        if (part >= PART_DEBUG_INFO) {                             /* these parts are the bare section */
            hr = d3dc_blob(picked[0].data.code, picked[0].data.size, out);
        } else {
            struct vkd3d_shader_code res;
            int ret = vkd3d_shader_serialize_dxbc(got, picked, &res, NULL);
            hr = ret < 0 ? from_vkd3d(ret) : d3dc_blob(res.code, res.size, out);
            if (ret >= 0) vkd3d_shader_free_shader_code(&res);
        }
    }
    vkd3d_shader_free_dxbc(&dxbc);
    return hr;
}

D3DCAPI HRESULT WINAPI D3DGetInputSignatureBlob(LPCVOID data, SIZE_T n, void **out)
{
    return D3DGetBlobPart(data, n, PART_INPUT_SIGNATURE, 0, out);
}
D3DCAPI HRESULT WINAPI D3DGetOutputSignatureBlob(LPCVOID data, SIZE_T n, void **out)
{
    return D3DGetBlobPart(data, n, PART_OUTPUT_SIGNATURE, 0, out);
}
D3DCAPI HRESULT WINAPI D3DGetInputAndOutputSignatureBlob(LPCVOID data, SIZE_T n, void **out)
{
    return D3DGetBlobPart(data, n, PART_INPUT_AND_OUTPUT_SIGNATURE, 0, out);
}
D3DCAPI HRESULT WINAPI D3DGetDebugInfo(LPCVOID data, SIZE_T n, void **out)
{
    return D3DGetBlobPart(data, n, PART_DEBUG_INFO, 0, out);
}

/* the container again with the sections @keep says yes to, and @extra after them */
static HRESULT rebuild(LPCVOID data, SIZE_T n, BOOL (*keep)(DWORD tag, UINT arg), UINT arg,
                       const struct vkd3d_shader_dxbc_section_desc *extra, void **out)
{
    struct vkd3d_shader_code src = { data, n };
    struct vkd3d_shader_dxbc_desc dxbc;
    if (vkd3d_shader_parse_dxbc(&src, 0, &dxbc, NULL) < 0) return D3DERR_INVALIDCALL;
    struct vkd3d_shader_dxbc_section_desc *s = HeapAlloc(GetProcessHeap(), 0, (dxbc.section_count + 1) * sizeof(*s));
    HRESULT hr = E_OUTOFMEMORY;
    if (s) {
        size_t k = 0;
        for (size_t i = 0; i < dxbc.section_count; i++)
            if (keep(dxbc.sections[i].tag, arg)) s[k++] = dxbc.sections[i];
        if (extra) s[k++] = *extra;
        struct vkd3d_shader_code res;
        int ret = vkd3d_shader_serialize_dxbc(k, s, &res, NULL);
        hr = ret < 0 ? from_vkd3d(ret) : d3dc_blob(res.code, res.size, out);
        if (ret >= 0) vkd3d_shader_free_shader_code(&res);
        HeapFree(GetProcessHeap(), 0, s);
    }
    vkd3d_shader_free_dxbc(&dxbc);
    return hr;
}

static BOOL strip_keeps(DWORD tag, UINT flags)
{
    if ((flags & D3DCOMPILER_STRIP_REFLECTION_DATA) && (tag == T('R','D','E','F') || tag == T('S','T','A','T')))
        return FALSE;
    if ((flags & D3DCOMPILER_STRIP_DEBUG_INFO) && (tag == T('S','D','B','G') || tag == T('S','P','D','B') ||
                                                  tag == T('I','L','D','B') || tag == T('I','L','D','N')))
        return FALSE;
    if ((flags & D3DCOMPILER_STRIP_PRIVATE_DATA) && tag == T('P','R','I','V')) return FALSE;
    if ((flags & D3DCOMPILER_STRIP_ROOT_SIGNATURE) && tag == T('R','T','S','0')) return FALSE;
    return TRUE;
}

D3DCAPI HRESULT WINAPI D3DStripShader(LPCVOID data, SIZE_T n, UINT flags, void **out)
{
    if (!out) return E_FAIL;
    *out = NULL;
    if (!data || !n) return D3DERR_INVALIDCALL;
    return rebuild(data, n, strip_keeps, flags, NULL, out);
}

static BOOL not_priv(DWORD tag, UINT arg) { (void)arg; return tag != T('P','R','I','V'); }

D3DCAPI HRESULT WINAPI D3DSetBlobPart(LPCVOID data, SIZE_T n, UINT part, UINT flags, LPCVOID partdata,
                                      SIZE_T partsize, void **out)
{
    if (out) *out = NULL;
    if (!data || !n || !out || flags || part != PART_PRIVATE_DATA || (!partdata && partsize))
        return D3DERR_INVALIDCALL;
    struct vkd3d_shader_dxbc_section_desc priv = { T('P','R','I','V'), { partdata, partsize } };
    return rebuild(data, n, not_priv, 0, &priv, out);
}

/* ---- files ---- */
D3DCAPI HRESULT WINAPI D3DReadFileToBlob(LPCWSTR file, void **out)
{
    DWORD size = 0;
    if (!file || !out) return E_INVALIDARG;
    *out = NULL;
    char *data = read_file(file, &size);
    if (!data) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_FILE_NOT_FOUND);
    HRESULT hr = d3dc_blob(data, size, out);
    HeapFree(GetProcessHeap(), 0, data);
    return hr;
}

D3DCAPI HRESULT WINAPI D3DWriteBlobToFile(void *blob, LPCWSTR file, BOOL overwrite)
{
    Blob *b = blob;
    if (!b || !file) return E_INVALIDARG;
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, overwrite ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    void *p = b->vtbl->GetBufferPointer(b);
    DWORD size = (DWORD)b->vtbl->GetBufferSize(b), done = 0;
    BOOL ok = WriteFile(f, p, size, &done, NULL) && done == size;
    DWORD err = GetLastError();
    CloseHandle(f);
    return ok ? S_OK : HRESULT_FROM_WIN32(err);
}

/* ---- what vkd3d-shader has no counterpart for ---- */
D3DCAPI HRESULT WINAPI D3DAssemble(LPCVOID data, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO *defines, void *include,
                                   UINT flags, void **code, void **errors)
{
    (void)data; (void)n; (void)name; (void)defines; (void)include; (void)flags;
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DCompressShaders(UINT count, void *shaders, UINT flags, void **out)
{
    (void)count; (void)shaders; (void)flags;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DDecompressShaders(LPCVOID data, SIZE_T n, UINT count, UINT start, UINT *indices, UINT flags,
                                            void **shaders, UINT *total)
{
    (void)data; (void)n; (void)count; (void)start; (void)indices; (void)flags; (void)shaders;
    if (total) *total = 0;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DCreateFunctionLinkingGraph(UINT flags, void **out)
{
    (void)flags;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DCreateLinker(void **out)
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DLoadModule(LPCVOID data, SIZE_T n, void **out)
{
    (void)data; (void)n;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DReflectLibrary(LPCVOID data, SIZE_T n, REFIID iid, void **out)
{
    (void)data; (void)n; (void)iid;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DDisassemble10Effect(void *effect, UINT flags, void **out)
{
    (void)effect; (void)flags;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DDisassemble11Trace(LPCVOID data, SIZE_T n, void *trace, UINT start, UINT count, UINT flags,
                                             void **out)
{
    (void)data; (void)n; (void)trace; (void)start; (void)count; (void)flags;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DDisassembleRegion(LPCVOID data, SIZE_T n, UINT flags, LPCSTR comments, SIZE_T start,
                                            SIZE_T count, SIZE_T *finish, void **out)
{
    (void)data; (void)n; (void)flags; (void)comments; (void)start; (void)count;
    if (finish) *finish = 0;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DGetTraceInstructionOffsets(LPCVOID data, SIZE_T n, UINT flags, SIZE_T start, SIZE_T count,
                                                     SIZE_T *offsets, SIZE_T *total)
{
    (void)data; (void)n; (void)flags; (void)start; (void)count; (void)offsets;
    if (total) *total = 0;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DReturnFailure1(UINT a, UINT b, UINT c)
{
    (void)a; (void)b; (void)c;
    return E_FAIL;
}
D3DCAPI HRESULT WINAPI DebugSetMute(void)
{
    return S_OK;
}
