/*
 * activeds.dll — Active Directory Service Interfaces. NovaOS joins no
 * directory, so binding to an object fails with E_NOTIMPL; the memory
 * helpers work. (WiX's util custom actions import ADsGetObject.)
 */
#include <windows.h>

#define ADS __declspec(dllexport)
#define E_NOTIMPL_ ((HRESULT)0x80004001L)

ADS HRESULT WINAPI ADsGetObject(LPCWSTR path, REFIID iid, void **out)
{
    (void)path; (void)iid;
    if (out) *out = NULL;
    return E_NOTIMPL_;
}

ADS HRESULT WINAPI ADsOpenObject(LPCWSTR path, LPCWSTR user, LPCWSTR pw, DWORD flags, REFIID iid, void **out)
{
    (void)path; (void)user; (void)pw; (void)flags; (void)iid;
    if (out) *out = NULL;
    return E_NOTIMPL_;
}

ADS HRESULT WINAPI ADsBuildEnumerator(void *container, void **out) { (void)container; if (out) *out = NULL; return E_NOTIMPL_; }
ADS HRESULT WINAPI ADsFreeEnumerator(void *e) { (void)e; return S_OK; }
ADS HRESULT WINAPI ADsEnumerateNext(void *e, ULONG n, void *var, ULONG *got) { (void)e; (void)n; (void)var; if (got) *got = 0; return S_FALSE; }

static DWORD g_err;
ADS void WINAPI ADsSetLastError(DWORD err, LPCWSTR text, LPCWSTR provider) { (void)text; (void)provider; g_err = err; }
ADS HRESULT WINAPI ADsGetLastError(LPDWORD err, LPWSTR buf, DWORD n, LPWSTR prov, DWORD np)
{
    if (err) *err = g_err;
    if (buf && n) buf[0] = 0;
    if (prov && np) prov[0] = 0;
    return S_OK;
}

ADS LPVOID WINAPI AllocADsMem(DWORD n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
ADS BOOL WINAPI FreeADsMem(LPVOID p) { return p ? HeapFree(GetProcessHeap(), 0, p) : TRUE; }
ADS LPVOID WINAPI ReallocADsMem(LPVOID p, DWORD old, DWORD n)
{
    (void)old;
    if (!p) return AllocADsMem(n);
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n ? n : 1);
}
ADS LPWSTR WINAPI AllocADsStr(LPCWSTR s)
{
    if (!s) return NULL;
    DWORD n = 0;
    while (s[n]) n++;
    LPWSTR d = AllocADsMem((n + 1) * sizeof(WCHAR));
    if (d) for (DWORD i = 0; i <= n; i++) d[i] = s[i];
    return d;
}
ADS BOOL WINAPI FreeADsStr(LPWSTR s) { return FreeADsMem(s); }
