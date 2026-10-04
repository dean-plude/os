/* msiqtest.exe — Windows Installer queries as bootstrappers (WiX Burn) make
 * them, and where a 32-bit package's files go, with tools/msitest/mkpkg.py's
 * packages in C:\Tests\Msi.  Built 64- and 32-bit; the 32-bit one runs the
 * engine in a 32-bit process, as Burn calls MsiInstallProduct.
 *
 *   wow32.msi (Template "Intel")  its SystemFolder file lands in SysWOW64
 *   wow64.msi (Template "x64")    its SystemFolder file lands in System32
 *   MsiGetProductInfo[Ex], MsiEnumProducts[Ex] (contexts, buffer sizes)
 *   MsiSourceListAddSourceEx, EnumSources, GetInfo, SetInfo, ClearSource
 *   MsiDetermineApplicablePatches, MsiDeterminePatchSequence (a patch file
 *   and applicability XML), MsiEnumPatchesEx and MsiGetPatchInfoEx on a
 *   patched product, and removal taking the registration with it
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdbool.h>
#include <wchar.h>

typedef struct { LPCWSTR szPatchData; int ePatchDataType; DWORD dwOrder; UINT uStatus; } MSIPATCHSEQUENCEINFOW;
__declspec(dllimport) int WINAPI MsiSetInternalUI(int level, HWND *phwnd);
__declspec(dllimport) UINT WINAPI MsiInstallProductW(LPCWSTR package, LPCWSTR cmdline);
__declspec(dllimport) UINT WINAPI MsiConfigureProductW(LPCWSTR product, int level, int state);
__declspec(dllimport) UINT WINAPI MsiApplyPatchW(LPCWSTR patch, LPCWSTR product, int type, LPCWSTR cmdline);
__declspec(dllimport) UINT WINAPI MsiGetProductInfoW(LPCWSTR product, LPCWSTR prop, LPWSTR buf, LPDWORD len);
__declspec(dllimport) UINT WINAPI MsiGetProductInfoExW(LPCWSTR product, LPCWSTR sid, DWORD ctx, LPCWSTR prop, LPWSTR buf, LPDWORD len);
__declspec(dllimport) UINT WINAPI MsiGetProductInfoExA(LPCSTR product, LPCSTR sid, DWORD ctx, LPCSTR prop, LPSTR buf, LPDWORD len);
__declspec(dllimport) UINT WINAPI MsiEnumProductsW(DWORD index, LPWSTR buf);
__declspec(dllimport) UINT WINAPI MsiEnumProductsExW(LPCWSTR product, LPCWSTR sid, DWORD ctx, DWORD index, WCHAR code[39],
                                                     DWORD *got_ctx, LPWSTR got_sid, LPDWORD sid_len);
__declspec(dllimport) UINT WINAPI MsiSourceListAddSourceExW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR source, DWORD index);
__declspec(dllimport) UINT WINAPI MsiSourceListEnumSourcesW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, DWORD index, LPWSTR buf, LPDWORD len);
__declspec(dllimport) UINT WINAPI MsiSourceListGetInfoW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR prop, LPWSTR buf, LPDWORD len);
__declspec(dllimport) UINT WINAPI MsiSourceListSetInfoW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR prop, LPCWSTR value);
__declspec(dllimport) UINT WINAPI MsiSourceListClearSourceW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR source);
__declspec(dllimport) UINT WINAPI MsiDetermineApplicablePatchesW(LPCWSTR package, DWORD n, MSIPATCHSEQUENCEINFOW *pi);
__declspec(dllimport) UINT WINAPI MsiDeterminePatchSequenceW(LPCWSTR product, LPCWSTR sid, DWORD ctx, DWORD n, MSIPATCHSEQUENCEINFOW *pi);
__declspec(dllimport) UINT WINAPI MsiEnumPatchesExW(LPCWSTR product, LPCWSTR sid, DWORD ctx, DWORD filter, DWORD index, WCHAR patch[39],
                                                    WCHAR target[39], DWORD *target_ctx, LPWSTR target_sid, LPDWORD sid_len);
__declspec(dllimport) UINT WINAPI MsiGetPatchInfoExW(LPCWSTR patch, LPCWSTR product, LPCWSTR sid, DWORD ctx, LPCWSTR prop, LPWSTR buf, LPDWORD len);
WINBASEAPI BOOL WINAPI Wow64DisableWow64FsRedirection(PVOID *old);
WINBASEAPI BOOL WINAPI Wow64RevertWow64FsRedirection(PVOID old);

#define PKG     L"C:\\Tests\\Msi\\"
#define WOW32   L"{6E1D0C3A-5A1B-4C2D-8E3F-000000003201}"
#define WOW64   L"{6E1D0C3A-5A1B-4C2D-8E3F-000000006401}"
#define BASE    L"{6E1D0C3A-5A1B-4C2D-8E3F-00000000B001}"
#define PATCH   L"{6E1D0C3A-5A1B-4C2D-8E3F-00000000A001}"
#define MACHINE 4
#define USERUNMANAGED 2
#define ALLCTX  7
#define NET     1
#define URL     2
#define CODE_PATCH 0x40000000

static int pass, fail;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a == _b) pass++; \
    else { fail++; printf("FAIL line %d: %s is %ld, not %ld\n", __LINE__, #a, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { const WCHAR *_a = (a), *_b = (b); if (!wcscmp(_a, _b)) pass++; \
    else { fail++; printf("FAIL line %d: %s is \"%ls\", not \"%ls\"\n", __LINE__, #a, _a, _b); } } while (0)

/* Does the file exist where it really is (no WoW64 redirection)? */
static bool really_exists(const WCHAR *path)
{
    PVOID old = NULL;
    BOOL off = Wow64DisableWow64FsRedirection(&old);
    bool r = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (off) Wow64RevertWow64FsRedirection(old);
    return r;
}

static bool key_exists(const WCHAR *key)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k)) return false;
    RegCloseKey(k);
    return true;
}

/* one product property (by context), "" on failure */
static const WCHAR *info(const WCHAR *code, DWORD ctx, const WCHAR *prop)
{
    static WCHAR buf[512];
    DWORD n = 512;
    if (MsiGetProductInfoExW(code, NULL, ctx, prop, buf, &n)) buf[0] = 0;
    return buf;
}

static const WCHAR *source(const WCHAR *code, DWORD options, DWORD index)
{
    static WCHAR buf[MAX_PATH];
    DWORD n = MAX_PATH;
    UINT r = MsiSourceListEnumSourcesW(code, NULL, MACHINE, options, index, buf, &n);
    if (r) _snwprintf(buf, MAX_PATH, L"(error %u)", r);
    return buf;
}

static const WCHAR *slinfo(const WCHAR *code, const WCHAR *prop)
{
    static WCHAR buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (MsiSourceListGetInfoW(code, NULL, MACHINE, NET, prop, buf, &n)) buf[0] = 0;
    return buf;
}

static void t_placement(void)
{
    CHECK_EQ(MsiInstallProductW(PKG L"wow32.msi", NULL), 0);
    CHECK(really_exists(L"C:\\Windows\\SysWOW64\\novawow32.txt"));
    CHECK(!really_exists(L"C:\\Windows\\System32\\novawow32.txt"));
    CHECK(really_exists(L"C:\\Programs\\NovaWow32\\app.txt"));
    CHECK_EQ(MsiInstallProductW(PKG L"wow64.msi", NULL), 0);
    CHECK(really_exists(L"C:\\Windows\\System32\\novawow64.txt"));
    CHECK(!really_exists(L"C:\\Windows\\SysWOW64\\novawow64.txt"));
}

static void t_product_info(void)
{
    WCHAR buf[64];
    DWORD n;
    CHECK_STR(info(WOW32, MACHINE, L"VersionString"), L"1.0.0");
    CHECK_STR(info(WOW32, MACHINE, L"InstalledProductName"), L"Nova WoW32 Test");
    CHECK_STR(info(WOW32, MACHINE, L"ProductName"), L"Nova WoW32 Test");
    CHECK_STR(info(WOW32, MACHINE, L"State"), L"5");
    CHECK_STR(info(WOW32, MACHINE, L"AssignmentType"), L"1");
    CHECK_STR(info(WOW32, MACHINE, L"PackageName"), L"wow32.msi");
    CHECK_STR(info(WOW32, MACHINE, L"PackageCode"), WOW32);          /* (mkmsi uses the product code) */
    CHECK_STR(info(WOW32, MACHINE, L"Language"), L"1033");
    CHECK(!wcsncmp(info(WOW32, MACHINE, L"LocalPackage"), L"C:\\Windows\\Installer\\", 21));
    /* the length alone, and a buffer too small */
    n = 0;
    CHECK_EQ(MsiGetProductInfoExW(WOW32, NULL, MACHINE, L"VersionString", NULL, &n), 0);
    CHECK_EQ(n, 5);
    n = 3;
    CHECK_EQ(MsiGetProductInfoExW(WOW32, NULL, MACHINE, L"VersionString", buf, &n), ERROR_MORE_DATA);
    CHECK_EQ(n, 5);
    /* the ANSI form */
    char a[64];
    n = sizeof(a);
    CHECK_EQ(MsiGetProductInfoExA("{6E1D0C3A-5A1B-4C2D-8E3F-000000003201}", NULL, MACHINE, "VersionString", a, &n), 0);
    CHECK(!strcmp(a, "1.0.0"));
    /* not in the user's context; not a context; per-machine with a user */
    n = 64;
    CHECK_EQ(MsiGetProductInfoExW(WOW32, NULL, USERUNMANAGED, L"VersionString", buf, &n), 1605);
    CHECK_EQ(MsiGetProductInfoExW(WOW32, NULL, ALLCTX, L"VersionString", buf, &n), ERROR_INVALID_PARAMETER);
    CHECK_EQ(MsiGetProductInfoExW(WOW32, L"S-1-5-18", MACHINE, L"VersionString", buf, &n), ERROR_INVALID_PARAMETER);
    CHECK_EQ(MsiGetProductInfoExW(WOW32, NULL, MACHINE, L"NoSuchProperty", buf, &n), 1608);
    CHECK_EQ(MsiGetProductInfoExW(L"{00000000-0000-0000-0000-000000000000}", NULL, MACHINE, L"VersionString", buf, &n), 1605);
    CHECK_EQ(MsiGetProductInfoExW(L"not a guid", NULL, MACHINE, L"VersionString", buf, &n), ERROR_INVALID_PARAMETER);
    n = 64;
    CHECK_EQ(MsiGetProductInfoW(WOW64, L"VersionString", buf, &n), 0);
    CHECK_STR(buf, L"1.0.0");

    /* enumeration: both products, per-machine, no user SID */
    bool saw32 = false, saw64 = false;
    for (DWORD i = 0; i < 256; i++) {
        WCHAR code[39], sid[64];
        DWORD ctx = 0, sl = 64;
        UINT r = MsiEnumProductsExW(NULL, NULL, ALLCTX, i, code, &ctx, sid, &sl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        CHECK_EQ(r, 0);
        if (r) break;
        if (!wcscmp(code, WOW32)) { saw32 = true; CHECK_EQ(ctx, MACHINE); CHECK_EQ(sl, 0); }
        if (!wcscmp(code, WOW64)) saw64 = true;
    }
    CHECK(saw32 && saw64);
    WCHAR code[39];
    DWORD ctx = 0;
    CHECK_EQ(MsiEnumProductsExW(WOW64, NULL, MACHINE, 0, code, &ctx, NULL, NULL), 0);
    CHECK_STR(code, WOW64);
    CHECK_EQ(MsiEnumProductsExW(WOW64, NULL, MACHINE, 1, code, &ctx, NULL, NULL), ERROR_NO_MORE_ITEMS);
    CHECK_EQ(MsiEnumProductsExW(WOW64, NULL, USERUNMANAGED, 0, code, &ctx, NULL, NULL), ERROR_NO_MORE_ITEMS);
    CHECK_EQ(MsiEnumProductsExW(NULL, NULL, 0, 0, code, &ctx, NULL, NULL), ERROR_INVALID_PARAMETER);
    saw32 = false;
    for (DWORD i = 0; !MsiEnumProductsW(i, code); i++) if (!wcscmp(code, WOW32)) saw32 = true;
    CHECK(saw32);
}

static void t_source_list(void)
{
    /* the installation registered where the package came from */
    CHECK_STR(source(WOW32, NET, 0), PKG);
    CHECK_STR(source(WOW32, NET, 1), L"(error 259)");
    CHECK_STR(slinfo(WOW32, L"PackageName"), L"wow32.msi");
    CHECK_STR(slinfo(WOW32, L"LastUsedSource"), PKG);
    CHECK_STR(slinfo(WOW32, L"LastUsedType"), L"n");
    /* Burn adds its package cache in front (a network source gets its backslash) */
    CHECK_EQ(MsiSourceListAddSourceExW(WOW32, NULL, MACHINE, NET, L"C:\\Cache\\One", 1), 0);
    CHECK_STR(source(WOW32, NET, 0), L"C:\\Cache\\One\\");
    CHECK_STR(source(WOW32, NET, 1), PKG);
    /* index 0 appends; a source already there moves */
    CHECK_EQ(MsiSourceListAddSourceExW(WOW32, NULL, MACHINE, NET, L"C:\\Cache\\Two\\", 0), 0);
    CHECK_EQ(MsiSourceListAddSourceExW(WOW32, NULL, MACHINE, NET, PKG, 1), 0);
    CHECK_STR(source(WOW32, NET, 0), PKG);
    CHECK_STR(source(WOW32, NET, 1), L"C:\\Cache\\One\\");
    CHECK_STR(source(WOW32, NET, 2), L"C:\\Cache\\Two\\");
    CHECK_STR(source(WOW32, NET, 3), L"(error 259)");
    /* URLs come after the network sources */
    CHECK_EQ(MsiSourceListAddSourceExW(WOW32, NULL, MACHINE, URL, L"http://example.com/pkg", 0), 0);
    CHECK_STR(source(WOW32, NET | URL, 3), L"http://example.com/pkg/");
    CHECK_STR(source(WOW32, URL, 0), L"http://example.com/pkg/");
    /* the last used source, and taking one out */
    CHECK_EQ(MsiSourceListSetInfoW(WOW32, NULL, MACHINE, NET, L"LastUsedSource", L"C:\\Cache\\Two"), 0);
    CHECK_STR(slinfo(WOW32, L"LastUsedSource"), L"C:\\Cache\\Two\\");
    CHECK_EQ(MsiSourceListClearSourceW(WOW32, NULL, MACHINE, NET, L"C:\\Cache\\One\\"), 0);
    CHECK_STR(source(WOW32, NET, 1), L"C:\\Cache\\Two\\");
    /* errors */
    CHECK_EQ(MsiSourceListAddSourceExW(L"{00000000-0000-0000-0000-000000000000}", NULL, MACHINE, NET, L"C:\\x", 0), 1605);
    CHECK_EQ(MsiSourceListAddSourceExW(PATCH, NULL, MACHINE, NET | CODE_PATCH, L"C:\\x", 0), 1647);
    CHECK_EQ(MsiSourceListAddSourceExW(WOW32, NULL, MACHINE, 4 /* media */, L"C:\\x", 0), ERROR_INVALID_PARAMETER);
    CHECK_EQ(MsiSourceListAddSourceExW(WOW32, NULL, ALLCTX, NET, L"C:\\x", 0), ERROR_INVALID_PARAMETER);
}

static void t_patches(void)
{
    static const WCHAR xml32[] =
        L"<MsiPatch xmlns=\"http://www.microsoft.com/msi/patch_applicability.xsd\" SchemaVersion=\"1.0.0.0\" "
        L"PatchGUID=\"{6E1D0C3A-5A1B-4C2D-8E3F-00000000A032}\" MinMsiVersion=\"3\">"
        L"<TargetProduct MinMsiVersion=\"200\"><TargetProductCode Validate=\"true\">" WOW32 L"</TargetProductCode>"
        L"</TargetProduct></MsiPatch>";
    MSIPATCHSEQUENCEINFOW pi[3] = {
        { PKG L"patch.msp", 0 /* file */, 0, 0 },
        { xml32, 2 /* XML blob */, 0, 0 },
        { PKG L"missing.msp", 0, 0, 0 },
    };
    /* base.msi: the patch file is for it, the XML is not */
    CHECK_EQ(MsiDetermineApplicablePatchesW(PKG L"base.msi", 3, pi), 0);
    CHECK_EQ(pi[0].dwOrder, 0);
    CHECK_EQ(pi[0].uStatus, 0);
    CHECK_EQ(pi[1].dwOrder, (DWORD)-1);
    CHECK_EQ(pi[1].uStatus, 1642);
    CHECK_EQ(pi[2].dwOrder, (DWORD)-1);
    CHECK_EQ(pi[2].uStatus, 1635);
    /* the installed WoW32 product: the other way round */
    CHECK_EQ(MsiDeterminePatchSequenceW(WOW32, NULL, MACHINE, 2, pi), 0);
    CHECK_EQ(pi[0].dwOrder, (DWORD)-1);
    CHECK_EQ(pi[0].uStatus, 1642);
    CHECK_EQ(pi[1].dwOrder, 0);
    CHECK_EQ(pi[1].uStatus, 0);
    CHECK_EQ(MsiDeterminePatchSequenceW(BASE, NULL, MACHINE, 2, pi), 1605);
    CHECK_EQ(MsiDetermineApplicablePatchesW(PKG L"nothing.msi", 2, pi), 1619);

    /* nothing applied to WoW32 */
    WCHAR p[39], t[39], buf[MAX_PATH];
    DWORD ctx, n = MAX_PATH;
    CHECK_EQ(MsiEnumPatchesExW(WOW32, NULL, MACHINE, 15, 0, p, t, &ctx, NULL, NULL), ERROR_NO_MORE_ITEMS);
    CHECK_EQ(MsiGetPatchInfoExW(PATCH, WOW32, NULL, MACHINE, L"State", buf, &n), 1647);

    /* base.msi with patch.msp applied */
    CHECK_EQ(MsiInstallProductW(PKG L"base.msi", NULL), 0);
    CHECK_EQ(MsiApplyPatchW(PKG L"patch.msp", NULL, 0, NULL), 0);
    CHECK_EQ(MsiEnumPatchesExW(BASE, NULL, MACHINE, 1 /* applied */, 0, p, t, &ctx, NULL, NULL), 0);
    CHECK_STR(p, PATCH);
    CHECK_STR(t, BASE);
    CHECK_EQ(ctx, MACHINE);
    CHECK_EQ(MsiEnumPatchesExW(BASE, NULL, MACHINE, 1, 1, p, t, &ctx, NULL, NULL), ERROR_NO_MORE_ITEMS);
    n = MAX_PATH;
    CHECK_EQ(MsiGetPatchInfoExW(PATCH, BASE, NULL, MACHINE, L"State", buf, &n), 0);
    CHECK_STR(buf, L"1");
    n = MAX_PATH;
    CHECK_EQ(MsiGetPatchInfoExW(PATCH, BASE, NULL, MACHINE, L"LocalPackage", buf, &n), 0);
    CHECK(!wcsncmp(buf, L"C:\\Windows\\Installer\\", 21));
    CHECK(GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES);
    CHECK_EQ(MsiSourceListAddSourceExW(PATCH, NULL, MACHINE, NET | CODE_PATCH, L"C:\\Patches", 0), 0);
    CHECK_STR(source(PATCH, NET | CODE_PATCH, 0), L"C:\\Patches\\");
    CHECK_EQ(MsiConfigureProductW(BASE, 0, 2 /* INSTALLSTATE_ABSENT */), 0);
    CHECK_EQ(MsiEnumPatchesExW(BASE, NULL, MACHINE, 1, 0, p, t, &ctx, NULL, NULL), ERROR_NO_MORE_ITEMS);
}

static void t_remove(void)
{
    CHECK_EQ(MsiConfigureProductW(WOW32, 0, 2), 0);
    CHECK_EQ(MsiConfigureProductW(WOW64, 0, 2), 0);
    CHECK(!really_exists(L"C:\\Windows\\SysWOW64\\novawow32.txt"));
    CHECK(!really_exists(L"C:\\Windows\\System32\\novawow64.txt"));
    WCHAR buf[64];
    DWORD n = 64;
    CHECK_EQ(MsiGetProductInfoExW(WOW32, NULL, MACHINE, L"VersionString", buf, &n), 1605);
    CHECK(!key_exists(L"SOFTWARE\\NovaOS\\Installer\\Products\\" WOW32));
    CHECK(!key_exists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" WOW32));
}

int main(void)
{
    MsiSetInternalUI(2 /* INSTALLUILEVEL_NONE */, NULL);
    printf("msiqtest (%d-bit)\n", (int)sizeof(void *) * 8);
    t_placement();
    t_product_info();
    t_source_list();
    t_patches();
    t_remove();
    printf("msiqtest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
