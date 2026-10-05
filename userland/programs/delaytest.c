/* delaytest.exe — the DLLs Firefox's xul.dll delay-loads: d3d11, urlmon,
 * credui, winspool.drv, dhcpcsvc and d3dcompiler_47, loaded by name the
 * way the delay-load helper does */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <oleauto.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d, error %lu)\n", what, __LINE__, GetLastError()); } } while (0)

static FARPROC fn(const char *dll, const char *name)
{
    HMODULE m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : 0;
}

/* IUri, as far as the test reads it */
typedef struct Uri Uri;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Uri *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(Uri *);
    ULONG   (WINAPI *Release)(Uri *);
    HRESULT (WINAPI *GetPropertyBSTR)(Uri *, DWORD, BSTR *, DWORD);
    HRESULT (WINAPI *GetPropertyLength)(Uri *, DWORD, DWORD *, DWORD);
    HRESULT (WINAPI *GetPropertyDWORD)(Uri *, DWORD, DWORD *, DWORD);
    HRESULT (WINAPI *HasProperty)(Uri *, DWORD, BOOL *);
    HRESULT (WINAPI *rest[16])(Uri *, BSTR *);      /* GetAbsoluteUri ... GetUserName, GetHostType */
    HRESULT (WINAPI *GetPort)(Uri *, DWORD *);
} UriVtbl;
struct Uri { const UriVtbl *vtbl; };

static int bstr_is(BSTR b, const WCHAR *s) { return b && !wcscmp(b, s); }

static void test_urlmon(void)
{
    HRESULT (WINAPI *create)(LPCWSTR, DWORD, DWORD_PTR, Uri **) = (void *)fn("urlmon.dll", "CreateUri");
    HRESULT (WINAPI *parse)(LPCWSTR, DWORD, DWORD, LPWSTR, DWORD, DWORD *, DWORD) = (void *)fn("urlmon.dll", "CoInternetParseUrl");
    void (WINAPI *sysfree)(BSTR) = (void *)fn("oleaut32.dll", "SysFreeString");
    CHECK("urlmon exports", create && parse && sysfree);
    if (!create || !parse || !sysfree) return;
    Uri *u = 0;
    CHECK("CreateUri", create(L"https://user:pw@example.org:8443/a/b.html?q=1#top", 0, 0, &u) == S_OK && u);
    if (u) {
        static const struct { DWORD prop; const WCHAR *want; } props[] = {
            { 12, L"https" }, { 6, L"example.org" }, { 8, L"/a/b.html" }, { 10, L"?q=1" }, { 5, L"#top" },
            { 14, L"user" }, { 7, L"pw" }, { 4, L".html" }, { 9, L"/a/b.html?q=1" }, { 1, L"user:pw@example.org:8443" },
        };
        for (int i = 0; i < (int)(sizeof(props) / sizeof(props[0])); i++) {
            BSTR b = 0;
            u->vtbl->GetPropertyBSTR(u, props[i].prop, &b, 0);
            CHECK("IUri string property", bstr_is(b, props[i].want));
            sysfree(b);
        }
        DWORD v = 0;
        CHECK("IUri port", u->vtbl->GetPort(u, &v) == S_OK && v == 8443);
        CHECK("IUri scheme", u->vtbl->GetPropertyDWORD(u, 17, &v, 0) == S_OK && v == 11);   /* URL_SCHEME_HTTPS */
        CHECK("IUri host type", u->vtbl->GetPropertyDWORD(u, 15, &v, 0) == S_OK && v == 1); /* Uri_HOST_DNS */
        u->vtbl->Release(u);
    }
    u = 0;
    CHECK("CreateUri default port", create(L"http://10.0.2.2/", 0, 0, &u) == S_OK && u);
    if (u) {
        DWORD v = 0;
        CHECK("IUri default port", u->vtbl->GetPort(u, &v) == S_OK && v == 80);
        CHECK("IUri IPv4 host", u->vtbl->GetPropertyDWORD(u, 15, &v, 0) == S_OK && v == 2);
        u->vtbl->Release(u);
    }
    CHECK("CreateUri rejects a relative URI", create(L"just/a/path", 0, 0, &u) == E_INVALIDARG);
    WCHAR out[64]; DWORD n = 0;
    CHECK("CoInternetParseUrl schema", parse(L"https://example.org/x", 13, 0, out, 64, &n, 0) == S_OK && !wcscmp(out, L"https"));
    CHECK("CoInternetParseUrl domain", parse(L"https://example.org/x", 15, 0, out, 64, &n, 0) == S_OK && !wcscmp(out, L"example.org"));
}

static void test_winspool(void)
{
    HMODULE m = LoadLibraryA("WINSPOOL.DRV");
    CHECK("winspool.drv loads", m != 0);
    if (!m) return;
    BOOL (WINAPI *enum_w)(DWORD, LPWSTR, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD) = (void *)GetProcAddress(m, "EnumPrintersW");
    BOOL (WINAPI *def_w)(LPWSTR, LPDWORD) = (void *)GetProcAddress(m, MAKEINTRESOURCEA(203));
    BOOL (WINAPI *open_w)(LPWSTR, LPHANDLE, LPVOID) = (void *)GetProcAddress(m, "OpenPrinterW");
    CHECK("winspool exports", enum_w && def_w && open_w && def_w == (void *)GetProcAddress(m, "GetDefaultPrinterW"));
    if (!enum_w || !def_w || !open_w) return;
    DWORD need = 1, count = 1;
    CHECK("EnumPrintersW: none", enum_w(6, 0, 4, 0, 0, &need, &count) && count == 0);
    WCHAR name[64]; DWORD n = 64;
    CHECK("GetDefaultPrinterW: none", !def_w(name, &n) && GetLastError() == ERROR_FILE_NOT_FOUND);
    HANDLE h = (HANDLE)1;
    CHECK("OpenPrinterW fails", !open_w(L"Microsoft Print to PDF", &h, 0) && !h);
}

static void test_others(void)
{
    DWORD (WINAPI *prompt)(void *, DWORD, ULONG *, LPCVOID, ULONG, LPVOID *, ULONG *, BOOL *, DWORD) =
        (void *)fn("credui.dll", "CredUIPromptForWindowsCredentialsW");
    ULONG pkg = 0; LPVOID out = (LPVOID)1; ULONG nout = 1; BOOL save = TRUE;
    CHECK("CredUIPromptForWindowsCredentialsW: cancelled",
          prompt && prompt(0, 0, &pkg, 0, 0, &out, &nout, &save, 0) == 1223 && !out && !nout);

    typedef struct { ULONG n; void *p; } Arr;
    DWORD (WINAPI *dhcp)(DWORD, LPVOID, LPWSTR, void *, Arr, Arr, LPBYTE, LPDWORD, LPWSTR) =
        (void *)fn("dhcpcsvc.DLL", "DhcpRequestParams");
    Arr none = { 0, 0 }; DWORD size = 99;
    CHECK("DhcpRequestParams: no options", dhcp && dhcp(1, 0, L"{0}", 0, none, none, 0, &size, 0) != 0 && size == 0);

    typedef struct Blob { const struct { void *qi; ULONG (WINAPI *AddRef)(void *); ULONG (WINAPI *Release)(void *);
                                         LPVOID (WINAPI *Ptr)(void *); SIZE_T (WINAPI *Size)(void *); } *vtbl; } Blob;
    HRESULT (WINAPI *blob)(SIZE_T, Blob **) = (void *)fn("D3DCOMPILER_47.dll", "D3DCreateBlob");
    HRESULT (WINAPI *compile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, Blob **, Blob **) =
        (void *)fn("D3DCOMPILER_47.dll", "D3DCompile");
    Blob *b = 0;
    CHECK("D3DCreateBlob", blob && blob(16, &b) == S_OK && b && b->vtbl->Size(b) == 16 && b->vtbl->Ptr(b));
    if (b) b->vtbl->Release(b);
    Blob *code = 0, *err = 0;
    const char *src = "float4 main() : SV_Target { return 1; }";
    CHECK("D3DCompile compiles HLSL to DXBC",
          compile && SUCCEEDED(compile(src, strlen(src), 0, 0, 0, "main", "ps_4_0", 0, 0, &code, &err)) && code &&
          code->vtbl->Size(code) > 32 && !memcmp(code->vtbl->Ptr(code), "DXBC", 4));
    if (code) code->vtbl->Release(code);
    if (err) err->vtbl->Release(err);

    /* without DXVK installed: no Direct3D 11 adapter, as on a PC with no driver */
    HRESULT (WINAPI *create)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, void **, UINT *, void **) =
        (void *)fn("d3d11.dll", "D3D11CreateDevice");
    void *dev = (void *)1, *ctx = (void *)1;
    HRESULT hr = create ? create(0, 1, 0, 0, 0, 0, 7, &dev, 0, &ctx) : E_FAIL;
    BOOL dxvk = GetModuleHandleA("d3d11_dxvk.dll") != 0;
    CHECK("D3D11CreateDevice", create && (dxvk ? SUCCEEDED(hr) : hr == (HRESULT)0x887A0004L && !dev && !ctx));
    if (dxvk && SUCCEEDED(hr)) {
        typedef ULONG (WINAPI *release_t)(void *);
        (*(release_t **)ctx)[2](ctx);               /* IUnknown::Release */
        (*(release_t **)dev)[2](dev);
    }
}

int main(void)
{
    test_urlmon();
    test_winspool();
    test_others();
    printf("delaytest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
