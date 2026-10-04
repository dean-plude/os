/* d3d9test.exe — NovaOS's own d3d9.dll, as programs that import it find it
 * (Qt WebEngine's, and so GOG GALAXY's, among them): it is the system
 * folder's, it has every export Windows' d3d9.dll has, and
 *   d3d9test none   (no DXVK): Direct3DCreate9 returns NULL and
 *                   Direct3DCreate9Ex D3DERR_NOTAVAILABLE, as on a PC with no
 *                   Direct3D 9 driver, and the PIX and debug exports answer as
 *                   they do with no tool attached;
 *   d3d9test dxvk   (DXVK from the App Store, as d3d9_dxvk.dll): the calls
 *                   reach DXVK, whose IDirect3D9 lists an adapter. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define D3D_SDK_VERSION_ 32
#define D3DERR_NOTAVAILABLE_ ((HRESULT)0x8876086AL)

/* IDirect3D9's first methods (IUnknown, RegisterSoftwareDevice, GetAdapterCount) */
typedef struct D3D9 { struct {
    void *qi;
    ULONG (WINAPI *AddRef)(struct D3D9 *);
    ULONG (WINAPI *Release)(struct D3D9 *);
    void *reg;
    UINT (WINAPI *GetAdapterCount)(struct D3D9 *);
} *vtbl; } D3D9;

static const char *const exports[] = {
    "Direct3DCreate9", "Direct3DCreate9Ex", "Direct3DCreate9On12", "Direct3DCreate9On12Ex",
    "Direct3DShaderValidatorCreate9", "Direct3D9EnableMaximizedWindowedModeShim",
    "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame",
    "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion",
    "DebugSetLevel", "DebugSetMute", "PSGPError", "PSGPSampleTexture",
};

static BOOL in_module(void *p, HMODULE m)
{
    HMODULE of = NULL;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)p, &of) && of == m;
}

int main(int argc, char **argv)
{
    BOOL want_dxvk = argc > 1 && !strcmp(argv[1], "dxvk");
    if (argc < 2 || (!want_dxvk && strcmp(argv[1], "none"))) {
        printf("usage: d3d9test none|dxvk\n");
        return 2;
    }

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    CHECK("d3d9.dll loads", d3d9 != NULL);
    if (!d3d9) { printf("d3d9test: %d passed, %d failed\n", pass, fail); return 1; }

    char path[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameA(d3d9, path, MAX_PATH);
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);    /* SysWOW64 in a 32-bit process */
    lstrcatA(sys, "\\d3d9.dll");
    printf("d3d9.dll is %s\n", path);
    CHECK("d3d9.dll is the system folder's", n && !lstrcmpiA(path, sys));

    for (size_t i = 0; i < sizeof(exports) / sizeof(*exports); i++) {
        if (!GetProcAddress(d3d9, exports[i])) printf("missing export %s\n", exports[i]);
        CHECK(exports[i], GetProcAddress(d3d9, exports[i]) != NULL);
    }

    D3D9 *(WINAPI *create)(UINT) = (D3D9 *(WINAPI *)(UINT))GetProcAddress(d3d9, "Direct3DCreate9");
    HRESULT (WINAPI *create_ex)(UINT, D3D9 **) = (HRESULT (WINAPI *)(UINT, D3D9 **))GetProcAddress(d3d9, "Direct3DCreate9Ex");
    D3D9 *(WINAPI *create_12)(UINT, void *, UINT) = (D3D9 *(WINAPI *)(UINT, void *, UINT))GetProcAddress(d3d9, "Direct3DCreate9On12");
    void *(WINAPI *validator)(void) = (void *(WINAPI *)(void))GetProcAddress(d3d9, "Direct3DShaderValidatorCreate9");
    int (WINAPI *begin)(DWORD, const WCHAR *) = (int (WINAPI *)(DWORD, const WCHAR *))GetProcAddress(d3d9, "D3DPERF_BeginEvent");
    int (WINAPI *end)(void) = (int (WINAPI *)(void))GetProcAddress(d3d9, "D3DPERF_EndEvent");
    DWORD (WINAPI *status)(void) = (DWORD (WINAPI *)(void))GetProcAddress(d3d9, "D3DPERF_GetStatus");
    BOOL (WINAPI *repeat)(void) = (BOOL (WINAPI *)(void))GetProcAddress(d3d9, "D3DPERF_QueryRepeatFrame");
    void (WINAPI *marker)(DWORD, const WCHAR *) = (void (WINAPI *)(DWORD, const WCHAR *))GetProcAddress(d3d9, "D3DPERF_SetMarker");
    void (WINAPI *shim)(UINT) = (void (WINAPI *)(UINT))GetProcAddress(d3d9, "Direct3D9EnableMaximizedWindowedModeShim");
    if (fail) { printf("d3d9test: %d passed, %d failed\n", pass, fail); return 1; }

    /* the markers and switches answer the same either way: no profiler attached */
    CHECK("D3DPERF_GetStatus: no profiler", status() == 0);
    CHECK("D3DPERF_QueryRepeatFrame: no", repeat() == FALSE);
    marker(0xFFFF0000, L"d3d9test");
    shim(1);
    int b0 = begin(0xFF00FF00, L"outer"), b1 = begin(0xFF0000FF, L"inner");
    int e1 = end(), e0 = end();
    printf("D3DPERF events %d %d %d %d\n", b0, b1, e1, e0);
    if (!want_dxvk) {
        CHECK("D3DPERF_BeginEvent counts the open events", b0 == 0 && b1 == 1);
        CHECK("D3DPERF_EndEvent counts down", e1 == 1 && e0 == 0);
        CHECK("D3DPERF_EndEvent with none open", end() == -1);
    }

    HMODULE dxvk = GetModuleHandleA("d3d9_dxvk.dll");
    D3D9 *d = create(D3D_SDK_VERSION_);
    D3D9 *ex = (D3D9 *)(ULONG_PTR)1;
    HRESULT hr = create_ex(D3D_SDK_VERSION_, &ex);
    printf("Direct3DCreate9 %p, Direct3DCreate9Ex %08lx %p\n", (void *)d, (unsigned long)hr, (void *)ex);
    if (!want_dxvk) {
        CHECK("no DXVK loaded", dxvk == NULL && GetModuleHandleA("d3d9_dxvk.dll") == NULL);
        CHECK("Direct3DCreate9: NULL without a driver", d == NULL);
        CHECK("Direct3DCreate9Ex: D3DERR_NOTAVAILABLE", hr == D3DERR_NOTAVAILABLE_);
        CHECK("Direct3DCreate9Ex: no object", ex == NULL);
        CHECK("Direct3DCreate9On12: NULL", create_12(D3D_SDK_VERSION_, NULL, 0) == NULL);
        CHECK("Direct3DShaderValidatorCreate9: NULL", validator() == NULL);
    } else {
        dxvk = GetModuleHandleA("d3d9_dxvk.dll");
        CHECK("DXVK's d3d9 (d3d9_dxvk.dll) loaded", dxvk != NULL);
        CHECK("Direct3DCreate9: an object", d != NULL);
        CHECK("Direct3DCreate9Ex: S_OK", hr == S_OK && ex != NULL);
        if (d) {
            CHECK("the object is DXVK's", in_module(d->vtbl->GetAdapterCount, dxvk));
            UINT adapters = d->vtbl->GetAdapterCount(d);
            printf("adapters %u\n", adapters);
            CHECK("an adapter", adapters >= 1);
            d->vtbl->Release(d);
        }
        if (hr == S_OK && ex) ex->vtbl->Release(ex);
    }

    FreeLibrary(d3d9);
    printf("d3d9test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
