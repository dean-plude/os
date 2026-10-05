/* d3dcompiler_47.dll's own declarations: the D3D blob and the types and
 * flags of d3dcompiler.h and d3dcommon.h it uses */
#pragma once
#include <windows.h>

#define D3DCAPI __declspec(dllexport)
#define DXBC_TAG(a, b, c, d) ((DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24)
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086CL)

#define D3DCOMPILE_DEBUG                          0x00000001
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR          0x00000008
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR       0x00000010
#define D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY 0x00001000
#define D3DCOMPILE_EFFECT_CHILD_EFFECT            0x00000001
#define D3DCOMPILER_STRIP_REFLECTION_DATA         0x00000001
#define D3DCOMPILER_STRIP_DEBUG_INFO              0x00000002
#define D3DCOMPILER_STRIP_TEST_BLOBS              0x00000004
#define D3DCOMPILER_STRIP_PRIVATE_DATA            0x00000008
#define D3DCOMPILER_STRIP_ROOT_SIGNATURE          0x00000010

typedef struct { LPCSTR Name; LPCSTR Definition; } D3D_SHADER_MACRO;

static const GUID IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_ID3DBlob_ = { 0x8ba5fb08, 0x5195, 0x40e2, { 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 } };

/* ID3DBlob (ID3D10Blob) */
typedef struct Blob Blob;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Blob *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(Blob *);
    ULONG   (WINAPI *Release)(Blob *);
    LPVOID  (WINAPI *GetBufferPointer)(Blob *);
    SIZE_T  (WINAPI *GetBufferSize)(Blob *);
} BlobVtbl;
struct Blob { const BlobVtbl *vtbl; LONG refs; SIZE_T size; BYTE data[]; };

HRESULT d3dc_blob(const void *data, SIZE_T n, void **out);
