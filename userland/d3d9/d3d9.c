/*
 * d3d9.dll — Direct3D 9, NovaOS's own front.
 *
 * Programs import d3d9.dll even when they never draw with it (Qt WebEngine,
 * and so GOG GALAXY's client, does), so the base system has one.  NovaOS has
 * no display adapter driver of its own: when DXVK is installed from the App
 * Store (as d3d9_dxvk.dll beside this one), every entry point hands the call
 * to DXVK, so Direct3D 9 runs on Mesa's lavapipe or Venus; without it,
 * Direct3DCreate9 returns NULL and Direct3DCreate9Ex D3DERR_NOTAVAILABLE, as
 * Windows does with no Direct3D 9 driver, and programs use their software
 * paths.  The rest of Windows' exports (the PIX markers, the debug and shim
 * switches) answer as they do there when no tool is attached.
 */
#include <windows.h>

#define D3D9API __declspec(dllexport)
#define D3DERR_NOTAVAILABLE_ ((HRESULT)0x8876086AL)

static HMODULE dxvk(void)
{
    static HMODULE m;
    static LONG looked;
    if (!InterlockedCompareExchange(&looked, 1, 0)) {
        m = LoadLibraryW(L"d3d9_dxvk.dll");
        InterlockedExchange(&looked, 2);
    }
    while (looked == 1) Sleep(0);
    return m;
}

static FARPROC dxvk_proc(const char *name)
{
    HMODULE m = dxvk();
    return m ? GetProcAddress(m, name) : NULL;
}

/* ---- creating Direct3D ----------------------------------------------------- */
typedef void *(WINAPI *Create9_t)(UINT);
typedef HRESULT (WINAPI *Create9Ex_t)(UINT, void **);
typedef void *(WINAPI *Create9On12_t)(UINT, void *, UINT);
typedef HRESULT (WINAPI *Create9On12Ex_t)(UINT, void *, UINT, void **);

D3D9API void *WINAPI Direct3DCreate9(UINT sdk)
{
    Create9_t f = (Create9_t)dxvk_proc("Direct3DCreate9");
    return f ? f(sdk) : NULL;
}

D3D9API HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, void **out)
{
    Create9Ex_t f = (Create9Ex_t)dxvk_proc("Direct3DCreate9Ex");
    if (f) return f(sdk, out);
    if (out) *out = NULL;
    return D3DERR_NOTAVAILABLE_;
}

D3D9API void *WINAPI Direct3DCreate9On12(UINT sdk, void *args, UINT n)
{
    Create9On12_t f = (Create9On12_t)dxvk_proc("Direct3DCreate9On12");
    return f ? f(sdk, args, n) : NULL;
}

D3D9API HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdk, void *args, UINT n, void **out)
{
    Create9On12Ex_t f = (Create9On12Ex_t)dxvk_proc("Direct3DCreate9On12Ex");
    if (f) return f(sdk, args, n, out);
    if (out) *out = NULL;
    return D3DERR_NOTAVAILABLE_;
}

/* (a shader validator object; none without a driver) */
D3D9API void *WINAPI Direct3DShaderValidatorCreate9(void)
{
    void *(WINAPI *f)(void) = (void *(WINAPI *)(void))dxvk_proc("Direct3DShaderValidatorCreate9");
    return f ? f() : NULL;
}

D3D9API void WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT on)
{
    void (WINAPI *f)(UINT) = (void (WINAPI *)(UINT))dxvk_proc("Direct3D9EnableMaximizedWindowedModeShim");
    if (f) f(on);
}

/* ---- PIX markers: no profiler is attached ---------------------------------- */
static LONG g_events;                               /* BeginEvents not yet ended */

D3D9API int WINAPI D3DPERF_BeginEvent(DWORD colour, const WCHAR *name)
{
    int (WINAPI *f)(DWORD, const WCHAR *) = (int (WINAPI *)(DWORD, const WCHAR *))dxvk_proc("D3DPERF_BeginEvent");
    if (f) return f(colour, name);
    return (int)InterlockedIncrement(&g_events) - 1;
}

D3D9API int WINAPI D3DPERF_EndEvent(void)
{
    int (WINAPI *f)(void) = (int (WINAPI *)(void))dxvk_proc("D3DPERF_EndEvent");
    if (f) return f();
    LONG n = g_events;
    while (n > 0) {
        LONG seen = InterlockedCompareExchange(&g_events, n - 1, n);
        if (seen == n) return (int)n - 1;
        n = seen;
    }
    return -1;
}

D3D9API DWORD WINAPI D3DPERF_GetStatus(void)
{
    DWORD (WINAPI *f)(void) = (DWORD (WINAPI *)(void))dxvk_proc("D3DPERF_GetStatus");
    return f ? f() : 0;
}

D3D9API BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    BOOL (WINAPI *f)(void) = (BOOL (WINAPI *)(void))dxvk_proc("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}

D3D9API void WINAPI D3DPERF_SetMarker(DWORD colour, const WCHAR *name)
{
    void (WINAPI *f)(DWORD, const WCHAR *) = (void (WINAPI *)(DWORD, const WCHAR *))dxvk_proc("D3DPERF_SetMarker");
    if (f) f(colour, name);
}

D3D9API void WINAPI D3DPERF_SetOptions(DWORD options)
{
    void (WINAPI *f)(DWORD) = (void (WINAPI *)(DWORD))dxvk_proc("D3DPERF_SetOptions");
    if (f) f(options);
}

D3D9API void WINAPI D3DPERF_SetRegion(DWORD colour, const WCHAR *name)
{
    void (WINAPI *f)(DWORD, const WCHAR *) = (void (WINAPI *)(DWORD, const WCHAR *))dxvk_proc("D3DPERF_SetRegion");
    if (f) f(colour, name);
}

/* ---- the debug runtime's switches (this is the retail one) ----------------- */
D3D9API int WINAPI DebugSetLevel(void)
{
    int (WINAPI *f)(void) = (int (WINAPI *)(void))dxvk_proc("DebugSetLevel");
    return f ? f() : 0;
}

D3D9API void WINAPI DebugSetMute(void)
{
    void (WINAPI *f)(void) = (void (WINAPI *)(void))dxvk_proc("DebugSetMute");
    if (f) f();
}

/* (the processor-specific geometry pipeline's hooks; nothing to report) */
D3D9API void WINAPI PSGPError(void *d3dfe, DWORD error, UINT line)
{
    void (WINAPI *f)(void *, DWORD, UINT) = (void (WINAPI *)(void *, DWORD, UINT))dxvk_proc("PSGPError");
    if (f) f(d3dfe, error, line);
}

D3D9API void WINAPI PSGPSampleTexture(void *d3dfe, UINT stage, float (*uv)[4], UINT count, float (*out)[4])
{
    void (WINAPI *f)(void *, UINT, float (*)[4], UINT, float (*)[4]) =
        (void (WINAPI *)(void *, UINT, float (*)[4], UINT, float (*)[4]))dxvk_proc("PSGPSampleTexture");
    if (f) f(d3dfe, stage, uv, count, out);
}
