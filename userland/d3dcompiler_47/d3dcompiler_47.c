/*
 * d3dcompiler_47.dll — the HLSL compiler.  No permissively licensed HLSL
 * compiler exists to build on (Wine's and vkd3d-shader are LGPL), and
 * DXVK takes compiled bytecode, not HLSL, so NovaOS ships the blob API
 * and a compiler that reports in its error blob that it cannot compile:
 * callers fall back to their precompiled shaders or software paths.
 */
#include <windows.h>
#include <string.h>

#define D3DCAPI __declspec(dllexport)

static const GUID IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_ID3DBlob_ = { 0x8ba5fb08, 0x5195, 0x40e2, { 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 } };

/* ---- ID3DBlob (ID3D10Blob) ---- */
typedef struct Blob Blob;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Blob *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(Blob *);
    ULONG   (WINAPI *Release)(Blob *);
    LPVOID  (WINAPI *GetBufferPointer)(Blob *);
    SIZE_T  (WINAPI *GetBufferSize)(Blob *);
} BlobVtbl;
struct Blob { const BlobVtbl *vtbl; LONG refs; SIZE_T size; BYTE data[]; };

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
    if (!out) return E_INVALIDARG;
    Blob *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Blob) + size);
    if (!b) { *out = NULL; return E_OUTOFMEMORY; }
    b->vtbl = &g_blob;
    b->refs = 1;
    b->size = size;
    *out = b;
    return S_OK;
}

/* an error blob saying why: a NUL-terminated message, as the real one's */
static HRESULT cannot(void **code, void **errors)
{
    static const char msg[] = "NovaOS: this d3dcompiler_47.dll has no HLSL compiler\n";
    if (code) *code = NULL;
    if (errors && SUCCEEDED(D3DCreateBlob(sizeof(msg), errors)))
        memcpy(((Blob *)*errors)->data, msg, sizeof(msg));
    return E_FAIL;
}

D3DCAPI HRESULT WINAPI D3DCompile(LPCVOID src, SIZE_T n, LPCSTR name, const void *defines, void *include, LPCSTR entry,
                                  LPCSTR target, UINT flags1, UINT flags2, void **code, void **errors)
{
    (void)src; (void)n; (void)name; (void)defines; (void)include; (void)entry; (void)target; (void)flags1; (void)flags2;
    return cannot(code, errors);
}
D3DCAPI HRESULT WINAPI D3DCompile2(LPCVOID src, SIZE_T n, LPCSTR name, const void *defines, void *include, LPCSTR entry,
                                   LPCSTR target, UINT flags1, UINT flags2, UINT secdata_flags, LPCVOID secdata,
                                   SIZE_T secdata_size, void **code, void **errors)
{
    (void)secdata_flags; (void)secdata; (void)secdata_size;
    return D3DCompile(src, n, name, defines, include, entry, target, flags1, flags2, code, errors);
}
D3DCAPI HRESULT WINAPI D3DCompileFromFile(LPCWSTR file, const void *defines, void *include, LPCSTR entry, LPCSTR target,
                                          UINT flags1, UINT flags2, void **code, void **errors)
{
    (void)file; (void)defines; (void)include; (void)entry; (void)target; (void)flags1; (void)flags2;
    return cannot(code, errors);
}
D3DCAPI HRESULT WINAPI D3DPreprocess(LPCVOID src, SIZE_T n, LPCSTR name, const void *defines, void *include,
                                     void **out, void **errors)
{
    (void)src; (void)n; (void)name; (void)defines; (void)include;
    return cannot(out, errors);
}
D3DCAPI HRESULT WINAPI D3DReflect(LPCVOID data, SIZE_T n, const GUID *iid, void **out)
{
    (void)data; (void)n; (void)iid;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DDisassemble(LPCVOID data, SIZE_T n, UINT flags, LPCSTR comments, void **out)
{
    (void)data; (void)n; (void)flags; (void)comments;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DGetBlobPart(LPCVOID data, SIZE_T n, UINT part, UINT flags, void **out)
{
    (void)data; (void)n; (void)part; (void)flags;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
D3DCAPI HRESULT WINAPI D3DStripShader(LPCVOID data, SIZE_T n, UINT flags, void **out)
{
    (void)flags;
    if (!data || !out) return E_INVALIDARG;
    HRESULT hr = D3DCreateBlob(n, out);            /* (nothing stripped: the shader as given) */
    if (SUCCEEDED(hr)) memcpy(((Blob *)*out)->data, data, n);
    return hr;
}
D3DCAPI HRESULT WINAPI D3DGetInputSignatureBlob(LPCVOID data, SIZE_T n, void **out) { return D3DGetBlobPart(data, n, 0, 0, out); }
D3DCAPI HRESULT WINAPI D3DGetOutputSignatureBlob(LPCVOID data, SIZE_T n, void **out) { return D3DGetBlobPart(data, n, 1, 0, out); }
