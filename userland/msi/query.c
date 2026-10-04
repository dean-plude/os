/*
 * query.c — msi.dll: what is installed, as programs and bootstrappers ask
 * it (WiX Burn among them): product information by installation context
 * (MsiGetProductInfo[Ex], MsiEnumProducts[Ex]), the patches applied to a
 * product (MsiEnumPatchesEx, MsiGetPatchInfoEx), which patches fit a
 * product (MsiDetermineApplicablePatches, MsiDeterminePatchSequence) and
 * the product's source list (MsiSourceList*).
 *
 * The engine (install.c) registers each product under
 * HKLM\SOFTWARE\NovaOS\Installer\Products\{ProductCode}: LocalPackage,
 * PackageCode, AssignmentType (1 per-machine, 0 per-user), Patches (the
 * cached .msp files), and the subkey SourceList (PackageName,
 * LastUsedSource "n;1;path", and the sources by type: Net, URL and Media,
 * values "1", "2", ... in order), as Windows keeps them under
 * Installer\Products\<packed code>\SourceList.  The Uninstall key holds the
 * rest (DisplayName, Publisher, ...).
 */
#include "msi.h"
#include "msi_int.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MSIAPI __declspec(dllexport) UINT WINAPI

#define PRODUCTS_KEY  L"SOFTWARE\\NovaOS\\Installer\\Products"
#define UNINSTALL_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"

/* MSIINSTALLCONTEXT */
#define CTX_NONE           0
#define CTX_USERMANAGED    1
#define CTX_USERUNMANAGED  2
#define CTX_MACHINE        4
#define CTX_ALL            (CTX_USERMANAGED | CTX_USERUNMANAGED | CTX_MACHINE)
#define CTX_ALLUSERMANAGED 8

/* MsiSourceList dwOptions */
#define MSISOURCETYPE_NETWORK 0x1
#define MSISOURCETYPE_URL     0x2
#define MSISOURCETYPE_MEDIA   0x4
#define MSICODE_PATCH         0x40000000

/* MSIPATCHDATATYPE */
#define MSIPATCH_DATATYPE_PATCHFILE 0
#define MSIPATCH_DATATYPE_XMLPATH   1
#define MSIPATCH_DATATYPE_XMLBLOB   2

/* MSIPATCHSTATE */
#define MSIPATCHSTATE_APPLIED 1
#define MSIPATCHSTATE_ALL     15

#define E_UNKNOWN_PRODUCT        1605
#define E_UNKNOWN_PROPERTY       1608
#define E_BAD_CONFIGURATION      1610
#define E_PATCH_OPEN_FAILED      1635
#define E_PATCH_INVALID          1636
#define E_PATCH_TARGET_NOT_FOUND 1642
#define E_UNKNOWN_PATCH          1647
#define E_INSTALL_PACKAGE_OPEN   1619
#define E_INSTALL_PACKAGE_INVALID 1620
#define E_BAD_ARGUMENTS          160

typedef struct {
    LPCWSTR szPatchData;
    int     ePatchDataType;
    DWORD   dwOrder;
    UINT    uStatus;
} MSIPATCHSEQUENCEINFOW;

typedef struct {
    LPCSTR szPatchData;
    int    ePatchDataType;
    DWORD  dwOrder;
    UINT   uStatus;
} MSIPATCHSEQUENCEINFOA;

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static WCHAR *a2w(LPCSTR s)
{
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    WCHAR *w = malloc((size_t)(n > 0 ? n : 1) * sizeof(WCHAR));
    if (!w) return NULL;
    if (n <= 0) w[0] = 0; else MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

/* A string result the Windows Installer way: @len counts characters
 * without the terminator, in and out; a NULL buffer asks for the length */
static UINT put_w(const WCHAR *s, LPWSTR buf, LPDWORD len)
{
    DWORD n = (DWORD)wcslen(s);
    if (!buf) { if (len) *len = n; return ERROR_SUCCESS; }
    if (!len) return ERROR_INVALID_PARAMETER;
    if (*len <= n) {
        if (*len) { wcsncpy(buf, s, *len - 1); buf[*len - 1] = 0; }
        *len = n;
        return ERROR_MORE_DATA;
    }
    wcscpy(buf, s);
    *len = n;
    return ERROR_SUCCESS;
}

/* The same for an ANSI caller of a W function's result (@len in chars) */
static UINT put_a(const WCHAR *s, LPSTR buf, LPDWORD len)
{
    int need = WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL);
    DWORD n = need > 0 ? (DWORD)need - 1 : 0;
    if (!buf) { if (len) *len = n; return ERROR_SUCCESS; }
    if (!len) return ERROR_INVALID_PARAMETER;
    if (*len <= n) {
        if (*len) {
            char *tmp = malloc((size_t)n + 1);
            if (tmp) {
                WideCharToMultiByte(CP_ACP, 0, s, -1, tmp, (int)n + 1, NULL, NULL);
                memcpy(buf, tmp, *len - 1);
                buf[*len - 1] = 0;
                free(tmp);
            }
        }
        *len = n;
        return ERROR_MORE_DATA;
    }
    WideCharToMultiByte(CP_ACP, 0, s, -1, buf, (int)*len, NULL, NULL);
    *len = n;
    return ERROR_SUCCESS;
}

static bool is_guid(LPCWSTR s)
{
    if (!s || wcslen(s) != 38 || s[0] != L'{' || s[37] != L'}') return false;
    for (int i = 1; i < 37; i++) {
        WCHAR c = s[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (c != L'-') return false; }
        else if (!((c >= L'0' && c <= L'9') || ((c | 0x20) >= L'a' && (c | 0x20) <= L'f'))) return false;
    }
    return true;
}

/* A REG_SZ (or REG_DWORD, as decimal) value of an open key into @out */
static bool reg_text(HKEY h, LPCWSTR name, WCHAR *out, DWORD cap)
{
    DWORD type, size = (cap - 1) * sizeof(WCHAR);
    BYTE *b = (BYTE *)out;
    if (RegQueryValueExW(h, name, NULL, &type, b, &size)) { out[0] = 0; return false; }
    if (type == REG_DWORD) { DWORD v = *(DWORD *)b; _snwprintf(out, cap, L"%lu", (unsigned long)v); return true; }
    if (type != REG_SZ && type != REG_EXPAND_SZ) { out[0] = 0; return false; }
    out[size / sizeof(WCHAR)] = 0;
    return true;
}

/* The current user's SID as text (per-user registrations belong to it) */
static void user_sid(WCHAR *out, DWORD cap)
{
    wcsncpy(out, L"S-1-5-21-0-0-0-1001", cap);
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    BYTE buf[256];
    DWORD n = 0;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
        LPWSTR s = NULL;
        if (ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &s) && s) {
            wcsncpy(out, s, cap - 1);
            out[cap - 1] = 0;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
}

/* Open a registered product's key (@sub: NULL, or a subkey such as
 * L"SourceList"); its context goes to @ctx */
static bool open_registration(LPCWSTR code, LPCWSTR sub, REGSAM sam, HKEY *out, DWORD *ctx)
{
    WCHAR key[400];
    if (!is_guid(code)) return false;
    _snwprintf(key, 400, L"%s\\%s%s%s", PRODUCTS_KEY, code, sub ? L"\\" : L"", sub ? sub : L"");
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, sam, &h)) return false;
    if (ctx) {
        HKEY p = h;
        if (sub) {
            _snwprintf(key, 400, L"%s\\%s", PRODUCTS_KEY, code);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &p)) p = NULL;
        }
        DWORD v = 1, type, size = sizeof(v);
        if (p && RegQueryValueExW(p, L"AssignmentType", NULL, &type, (BYTE *)&v, &size)) v = 1;
        *ctx = v ? CTX_MACHINE : CTX_USERUNMANAGED;
        if (p && p != h) RegCloseKey(p);
    }
    *out = h;
    return true;
}

/* Is the product registered in one of the contexts @want (for @sid, when
 * a user context is asked for: NULL or "" means the current user, "s-1-1-0"
 * every user)?  Returns the context it is in, or 0 */
static DWORD product_context(LPCWSTR code, LPCWSTR sid, DWORD want)
{
    HKEY h;
    DWORD ctx = 0;
    if (!open_registration(code, NULL, KEY_READ, &h, &ctx)) return 0;
    RegCloseKey(h);
    if (!(ctx & want)) return 0;
    if (ctx != CTX_MACHINE && sid && *sid && _wcsicmp(sid, L"s-1-1-0")) {
        WCHAR me[200];
        user_sid(me, 200);
        if (_wcsicmp(sid, me)) return 0;
    }
    return ctx;
}

/* The arguments every *Ex function checks the same way */
static UINT check_context(LPCWSTR sid, DWORD ctx, bool single)
{
    if (!ctx || (ctx & ~(CTX_ALL | CTX_ALLUSERMANAGED))) return ERROR_INVALID_PARAMETER;
    if (single && ctx != CTX_MACHINE && ctx != CTX_USERMANAGED && ctx != CTX_USERUNMANAGED) return ERROR_INVALID_PARAMETER;
    if (ctx == CTX_MACHINE && sid && *sid) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Product information
 * ----------------------------------------------------------------------- */
/* INSTALLPROPERTY_* of an installed product: where each one is kept */
enum { SRC_UNINSTALL, SRC_PRODUCT, SRC_SOURCELIST, SRC_FIXED };
static const struct { const WCHAR *name, *value; int src; } g_props[] = {
    { L"InstalledProductName", L"DisplayName", SRC_UNINSTALL },
    { L"VersionString", L"DisplayVersion", SRC_UNINSTALL },
    { L"HelpLink", L"HelpLink", SRC_UNINSTALL },
    { L"HelpTelephone", L"HelpTelephone", SRC_UNINSTALL },
    { L"InstallLocation", L"InstallLocation", SRC_UNINSTALL },
    { L"InstallSource", L"InstallSource", SRC_UNINSTALL },
    { L"InstallDate", L"InstallDate", SRC_UNINSTALL },
    { L"Publisher", L"Publisher", SRC_UNINSTALL },
    { L"LocalPackage", L"LocalPackage", SRC_PRODUCT },
    { L"URLInfoAbout", L"URLInfoAbout", SRC_UNINSTALL },
    { L"URLUpdateInfo", L"URLUpdateInfo", SRC_UNINSTALL },
    { L"VersionMinor", L"VersionMinor", SRC_UNINSTALL },
    { L"VersionMajor", L"VersionMajor", SRC_UNINSTALL },
    { L"ProductID", L"ProductID", SRC_UNINSTALL },
    { L"RegCompany", L"RegCompany", SRC_UNINSTALL },
    { L"RegOwner", L"RegOwner", SRC_UNINSTALL },
    { L"Uninstallable", NULL, SRC_FIXED },
    { L"State", NULL, SRC_FIXED },
    { L"InstanceType", NULL, SRC_FIXED },
    { L"Transforms", L"Transforms", SRC_PRODUCT },
    { L"Language", L"Language", SRC_UNINSTALL },
    { L"ProductName", L"ProductName", SRC_PRODUCT },
    { L"AssignmentType", L"AssignmentType", SRC_PRODUCT },
    { L"PackageCode", L"PackageCode", SRC_PRODUCT },
    { L"Version", L"Version", SRC_UNINSTALL },
    { L"ProductIcon", L"ProductIcon", SRC_PRODUCT },
    { L"AuthorizedLUAApp", NULL, SRC_FIXED },
    { L"PackageName", L"PackageName", SRC_SOURCELIST },
    { L"LastUsedSource", L"LastUsedSource", SRC_SOURCELIST },
    { L"LastUsedType", NULL, SRC_SOURCELIST },
    { L"MediaPackagePath", L"MediaPackagePath", SRC_SOURCELIST },
    { L"DiskPrompt", L"DiskPrompt", SRC_SOURCELIST },
};

/* The value of property @prop of registered product @code into @out */
static UINT product_property(LPCWSTR code, LPCWSTR prop, WCHAR *out, DWORD cap)
{
    out[0] = 0;
    int k = -1;
    for (size_t i = 0; i < sizeof(g_props) / sizeof(g_props[0]); i++)
        if (!_wcsicmp(prop, g_props[i].name)) { k = (int)i; break; }
    if (k < 0) return E_UNKNOWN_PROPERTY;
    HKEY h;
    WCHAR key[400];
    switch (g_props[k].src) {
    case SRC_FIXED:
        /* installed (not advertised), a normal instance, removable */
        wcscpy(out, !_wcsicmp(prop, L"State") ? L"5" : !_wcsicmp(prop, L"Uninstallable") ? L"1" :
                    !_wcsicmp(prop, L"InstanceType") ? L"0" : L"");
        return ERROR_SUCCESS;
    case SRC_PRODUCT:
        if (!open_registration(code, NULL, KEY_READ, &h, NULL)) return E_UNKNOWN_PRODUCT;
        reg_text(h, g_props[k].value, out, cap);
        if (!_wcsicmp(prop, L"AssignmentType") && !out[0]) wcscpy(out, L"1");
        RegCloseKey(h);
        return ERROR_SUCCESS;
    case SRC_SOURCELIST:
        if (!open_registration(code, L"SourceList", KEY_READ, &h, NULL)) return ERROR_SUCCESS;
        if (!_wcsicmp(prop, L"LastUsedType")) {
            reg_text(h, L"LastUsedSource", out, cap);
            out[out[0] ? 1 : 0] = 0;                      /* "n;1;path" -> "n" */
        } else reg_text(h, g_props[k].value, out, cap);
        RegCloseKey(h);
        return ERROR_SUCCESS;
    default:
        _snwprintf(key, 400, L"%s\\%s", UNINSTALL_KEY, code);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h)) return ERROR_SUCCESS;
        reg_text(h, g_props[k].value, out, cap);
        RegCloseKey(h);
        return ERROR_SUCCESS;
    }
}

MSIAPI MsiGetProductInfoExW(LPCWSTR product, LPCWSTR sid, DWORD ctx, LPCWSTR prop, LPWSTR buf, LPDWORD len)
{
    if (!is_guid(product) || !prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    UINT r = check_context(sid, ctx, true);
    if (r) return r;
    if (!product_context(product, sid, ctx)) return E_UNKNOWN_PRODUCT;
    WCHAR v[2048];
    r = product_property(product, prop, v, 2048);
    return r ? r : put_w(v, buf, len);
}

MSIAPI MsiGetProductInfoExA(LPCSTR product, LPCSTR sid, DWORD ctx, LPCSTR prop, LPSTR buf, LPDWORD len)
{
    if (!product || !prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    WCHAR *p = a2w(product), *s = a2w(sid), *a = a2w(prop), v[2048];
    DWORD n = 2048;
    UINT r = MsiGetProductInfoExW(p, s, ctx, a, v, &n);
    free(p); free(s); free(a);
    return r ? r : put_a(v, buf, len);
}

MSIAPI MsiGetProductInfoW(LPCWSTR product, LPCWSTR prop, LPWSTR buf, LPDWORD len)
{
    if (!product || !prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    if (!is_guid(product)) return ERROR_INVALID_PARAMETER;
    if (!product_context(product, NULL, CTX_ALL)) return E_UNKNOWN_PRODUCT;
    WCHAR v[2048];
    UINT r = product_property(product, prop, v, 2048);
    return r ? r : put_w(v, buf, len);
}

MSIAPI MsiGetProductInfoA(LPCSTR product, LPCSTR prop, LPSTR buf, LPDWORD len)
{
    if (!product || !prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    WCHAR *p = a2w(product), *a = a2w(prop), v[2048];
    DWORD n = 2048;
    UINT r = MsiGetProductInfoW(p, a, v, &n);
    free(p); free(a);
    return r ? r : put_a(v, buf, len);
}

/* The @index-th registered product (in the order of the registry) that
 * matches; its code into @code */
static UINT nth_product(LPCWSTR only, LPCWSTR sid, DWORD ctx, DWORD index, WCHAR code[39], DWORD *got)
{
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PRODUCTS_KEY, 0, KEY_READ, &h)) return ERROR_NO_MORE_ITEMS;
    DWORD found = 0;
    UINT r = ERROR_NO_MORE_ITEMS;
    for (DWORD i = 0; ; i++) {
        WCHAR name[64];
        DWORD n = 64;
        if (RegEnumKeyExW(h, i, name, &n, NULL, NULL, NULL, NULL)) break;
        if (only && _wcsicmp(only, name)) continue;
        DWORD c = product_context(name, sid, ctx);
        if (!c) continue;
        if (found++ == index) {
            wcsncpy(code, name, 39);
            code[38] = 0;
            *got = c;
            r = ERROR_SUCCESS;
            break;
        }
    }
    RegCloseKey(h);
    return r;
}

MSIAPI MsiEnumProductsExW(LPCWSTR product, LPCWSTR sid, DWORD ctx, DWORD index, WCHAR code[39],
                          DWORD *got_ctx, LPWSTR got_sid, LPDWORD sid_len)
{
    if (product && *product && !is_guid(product)) return ERROR_INVALID_PARAMETER;
    if (got_sid && !sid_len) return ERROR_INVALID_PARAMETER;
    UINT r = check_context(sid, ctx, false);
    if (r) return r;
    WCHAR c[39];
    DWORD in = 0;
    r = nth_product(product && *product ? product : NULL, sid, ctx, index, c, &in);
    if (r) return r;
    if (code) wcscpy(code, c);
    if (got_ctx) *got_ctx = in;
    WCHAR who[200] = L"";
    if (in != CTX_MACHINE) user_sid(who, 200);
    if (sid_len) return put_w(who, got_sid, sid_len);
    return ERROR_SUCCESS;
}

MSIAPI MsiEnumProductsExA(LPCSTR product, LPCSTR sid, DWORD ctx, DWORD index, CHAR code[39],
                          DWORD *got_ctx, LPSTR got_sid, LPDWORD sid_len)
{
    WCHAR *p = a2w(product), *s = a2w(sid), c[39], who[200];
    DWORD n = 200;
    UINT r = MsiEnumProductsExW(p, s, ctx, index, c, got_ctx, who, &n);
    free(p); free(s);
    if (r) return r;
    if (code) WideCharToMultiByte(CP_ACP, 0, c, -1, code, 39, NULL, NULL);
    if (sid_len) return put_a(who, got_sid, sid_len);
    return ERROR_SUCCESS;
}

MSIAPI MsiEnumProductsW(DWORD index, LPWSTR buf)
{
    if (!buf) return ERROR_INVALID_PARAMETER;
    DWORD got;
    return nth_product(NULL, NULL, CTX_ALL, index, buf, &got);
}

MSIAPI MsiEnumProductsA(DWORD index, LPSTR buf)
{
    WCHAR w[39];
    if (!buf) return ERROR_INVALID_PARAMETER;
    UINT r = MsiEnumProductsW(index, w);
    if (!r) WideCharToMultiByte(CP_ACP, 0, w, -1, buf, 39, NULL, NULL);
    return r;
}

/* -----------------------------------------------------------------------
 * Patches
 * ----------------------------------------------------------------------- */
static MsiFile *load_file(LPCWSTR path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    void *data = malloc(sz ? sz : 1);
    if (!data || !ReadFile(h, data, sz, &rd, NULL) || rd != sz) { CloseHandle(h); free(data); return NULL; }
    CloseHandle(h);
    return msifile_load(data, sz);
}

/* A patch's code (Revision Number, first 38 characters), the products it
 * updates (Template, "{a};{b}") and its transforms (Last Author) */
static bool patch_summary(LPCWSTR path, WCHAR code[39], char *targets, int tcap, char *xforms, int xcap)
{
    MsiFile *f = load_file(path);
    if (!f) return false;
    char rev[512] = "";
    if (targets) msi_suminfo_get(msifile_cfb(f), 7 /* PID_TEMPLATE */, targets, tcap, NULL);
    if (xforms) msi_suminfo_get(msifile_cfb(f), 8 /* PID_LASTAUTHOR */, xforms, xcap, NULL);
    msi_suminfo_get(msifile_cfb(f), 9 /* PID_REVNUMBER */, rev, sizeof(rev), NULL);
    msifile_release(f);
    rev[38] = 0;
    if (code) MultiByteToWideChar(CP_UTF8, 0, rev, -1, code, 39);
    return true;
}

/* The @index-th patch applied to product @product: its code and its
 * cached .msp */
static bool nth_patch(LPCWSTR product, DWORD index, WCHAR code[39], WCHAR *local, DWORD cap)
{
    HKEY h;
    if (!open_registration(product, NULL, KEY_READ, &h, NULL)) return false;
    WCHAR list[4096];
    reg_text(h, L"Patches", list, 4096);
    RegCloseKey(h);
    DWORD i = 0;
    WCHAR *save = NULL;
    for (WCHAR *p = wcstok(list, L";", &save); p; p = wcstok(NULL, L";", &save)) {
        if (!*p) continue;
        WCHAR c[39];
        if (!patch_summary(p, c, NULL, 0, NULL, 0)) continue;
        if (i++ != index) continue;
        if (code) wcscpy(code, c);
        if (local) { wcsncpy(local, p, cap - 1); local[cap - 1] = 0; }
        return true;
    }
    return false;
}

/* Product @product's applied patch @patch: its cached .msp */
static bool find_patch(LPCWSTR product, LPCWSTR patch, WCHAR *local, DWORD cap)
{
    WCHAR c[39];
    for (DWORD i = 0; nth_patch(product, i, c, local, cap); i++)
        if (!_wcsicmp(c, patch)) return true;
    return false;
}

MSIAPI MsiEnumPatchesExW(LPCWSTR product, LPCWSTR sid, DWORD ctx, DWORD filter, DWORD index, WCHAR patch[39],
                         WCHAR target[39], DWORD *target_ctx, LPWSTR target_sid, LPDWORD sid_len)
{
    if (product && *product && !is_guid(product)) return ERROR_INVALID_PARAMETER;
    if (!filter || (filter & ~MSIPATCHSTATE_ALL) || (target_sid && !sid_len)) return ERROR_INVALID_PARAMETER;
    UINT r = check_context(sid, ctx, false);
    if (r) return r;
    if (!(filter & MSIPATCHSTATE_APPLIED)) return ERROR_NO_MORE_ITEMS;   /* (none superseded, obsolete or registered only) */
    DWORD seen = 0;
    for (DWORD p = 0; ; p++) {
        WCHAR code[39];
        DWORD in = 0;
        if (nth_product(product && *product ? product : NULL, sid, ctx, p, code, &in)) return ERROR_NO_MORE_ITEMS;
        WCHAR pc[39];
        for (DWORD i = 0; nth_patch(code, i, pc, NULL, 0); i++) {
            if (seen++ != index) continue;
            if (patch) wcscpy(patch, pc);
            if (target) wcscpy(target, code);
            if (target_ctx) *target_ctx = in;
            WCHAR who[200] = L"";
            if (in != CTX_MACHINE) user_sid(who, 200);
            return sid_len ? put_w(who, target_sid, sid_len) : ERROR_SUCCESS;
        }
    }
}

MSIAPI MsiEnumPatchesExA(LPCSTR product, LPCSTR sid, DWORD ctx, DWORD filter, DWORD index, CHAR patch[39],
                         CHAR target[39], DWORD *target_ctx, LPSTR target_sid, LPDWORD sid_len)
{
    WCHAR *p = a2w(product), *s = a2w(sid), pc[39], tc[39], who[200];
    DWORD n = 200;
    UINT r = MsiEnumPatchesExW(p, s, ctx, filter, index, pc, tc, target_ctx, who, &n);
    free(p); free(s);
    if (r) return r;
    if (patch) WideCharToMultiByte(CP_ACP, 0, pc, -1, patch, 39, NULL, NULL);
    if (target) WideCharToMultiByte(CP_ACP, 0, tc, -1, target, 39, NULL, NULL);
    return sid_len ? put_a(who, target_sid, sid_len) : ERROR_SUCCESS;
}

/* INSTALLPROPERTY_* of a patch applied to a product */
MSIAPI MsiGetPatchInfoExW(LPCWSTR patch, LPCWSTR product, LPCWSTR sid, DWORD ctx, LPCWSTR prop, LPWSTR buf, LPDWORD len)
{
    if (!is_guid(patch) || !is_guid(product) || !prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    UINT r = check_context(sid, ctx, true);
    if (r) return r;
    if (!product_context(product, sid, ctx)) return E_UNKNOWN_PRODUCT;
    WCHAR local[MAX_PATH];
    if (!find_patch(product, patch, local, MAX_PATH)) return E_UNKNOWN_PATCH;
    WCHAR v[1024] = L"";
    if (!_wcsicmp(prop, L"LocalPackage")) wcscpy(v, local);
    else if (!_wcsicmp(prop, L"Transforms")) {
        char x[1024] = "";
        patch_summary(local, NULL, NULL, 0, x, sizeof(x));
        MultiByteToWideChar(CP_UTF8, 0, x, -1, v, 1024);
    } else if (!_wcsicmp(prop, L"State")) wcscpy(v, L"1");            /* MSIPATCHSTATE_APPLIED */
    else if (!_wcsicmp(prop, L"Uninstallable")) wcscpy(v, L"1");
    else if (!_wcsicmp(prop, L"InstallDate")) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        SYSTEMTIME st;
        if (GetFileAttributesExW(local, GetFileExInfoStandard, &fa) && FileTimeToSystemTime(&fa.ftCreationTime, &st))
            _snwprintf(v, 1024, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    } else if (!_wcsicmp(prop, L"DisplayName") || !_wcsicmp(prop, L"MoreInfoURL")) {
        /* (from the patch's MsiPatchMetadata table, which NovaOS does not read: empty) */
    } else return E_UNKNOWN_PROPERTY;
    return put_w(v, buf, len);
}

MSIAPI MsiGetPatchInfoExA(LPCSTR patch, LPCSTR product, LPCSTR sid, DWORD ctx, LPCSTR prop, LPSTR buf, LPDWORD len)
{
    if (!patch || !product || !prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    WCHAR *pa = a2w(patch), *pr = a2w(product), *s = a2w(sid), *a = a2w(prop), v[1024];
    DWORD n = 1024;
    UINT r = MsiGetPatchInfoExW(pa, pr, s, ctx, a, v, &n);
    free(pa); free(pr); free(s); free(a);
    return r ? r : put_a(v, buf, len);
}

/* The products a patch's applicability XML (MsiExtractPatchXMLData's
 * <MsiPatch>) targets: the text of each <TargetProductCode>, ";"-joined */
static void xml_targets(const char *xml, char *out, int cap)
{
    out[0] = 0;
    for (const char *p = xml; (p = strstr(p, "<TargetProductCode")); ) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        if (gt[-1] == '/') { p = gt; continue; }
        const char *end = strstr(gt, "</TargetProductCode");
        if (!end) break;
        const char *s = gt + 1;
        while (s < end && (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t')) s++;
        size_t n = strlen(out);
        snprintf(out + n, (size_t)(cap - (int)n), "%s%.*s", n ? ";" : "", (int)(end - s < 38 ? end - s : 38), s);
        p = end;
    }
}

/* Is one patch (as MsiDetermine* take it) for product @code?  0, or the
 * reason it is not (a uStatus) */
static UINT patch_fits(const MSIPATCHSEQUENCEINFOW *pi, const char *code)
{
    char targets[4096] = "";
    if (!pi->szPatchData) return ERROR_INVALID_PARAMETER;
    if (pi->ePatchDataType == MSIPATCH_DATATYPE_PATCHFILE) {
        if (!patch_summary(pi->szPatchData, NULL, targets, sizeof(targets), NULL, 0)) {
            return GetFileAttributesW(pi->szPatchData) == INVALID_FILE_ATTRIBUTES ? E_PATCH_OPEN_FAILED : E_PATCH_INVALID;
        }
    } else if (pi->ePatchDataType == MSIPATCH_DATATYPE_XMLBLOB || pi->ePatchDataType == MSIPATCH_DATATYPE_XMLPATH) {
        char *xml = NULL;
        if (pi->ePatchDataType == MSIPATCH_DATATYPE_XMLBLOB) {
            int n = WideCharToMultiByte(CP_UTF8, 0, pi->szPatchData, -1, NULL, 0, NULL, NULL);
            xml = malloc(n > 0 ? (size_t)n : 1);
            if (xml) WideCharToMultiByte(CP_UTF8, 0, pi->szPatchData, -1, xml, n, NULL, NULL);
        } else {
            HANDLE h = CreateFileW(pi->szPatchData, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) return E_PATCH_OPEN_FAILED;
            DWORD sz = GetFileSize(h, NULL), rd = 0;
            xml = malloc((size_t)sz + 2);
            if (xml && ReadFile(h, xml, sz, &rd, NULL)) {
                xml[rd] = xml[rd + 1 < sz + 2 ? rd + 1 : rd] = 0;
                if (rd >= 2 && (BYTE)xml[0] == 0xFF && (BYTE)xml[1] == 0xFE) {   /* UTF-16 */
                    WCHAR *w = calloc(rd / 2 + 1, sizeof(WCHAR));
                    char *u = malloc((size_t)rd * 2 + 1);
                    if (w && u) {
                        memcpy(w, xml + 2, rd - 2);
                        WideCharToMultiByte(CP_UTF8, 0, w, -1, u, (int)rd * 2 + 1, NULL, NULL);
                        free(xml);
                        xml = u;
                        u = NULL;
                    }
                    free(w); free(u);
                }
            }
            CloseHandle(h);
        }
        if (!xml) return ERROR_OUTOFMEMORY;
        if (!strstr(xml, "<MsiPatch")) { free(xml); return E_PATCH_INVALID; }
        xml_targets(xml, targets, sizeof(targets));
        free(xml);
    } else return ERROR_INVALID_PARAMETER;
    for (char *t = targets; *t; t++) if (*t >= 'a' && *t <= 'f') *t -= 32;
    char c[64];
    snprintf(c, sizeof(c), "%s", code);
    for (char *t = c; *t; t++) if (*t >= 'a' && *t <= 'f') *t -= 32;
    return *c && strstr(targets, c) ? ERROR_SUCCESS : E_PATCH_TARGET_NOT_FOUND;
}

/* Order the patches that fit: the order they are given in (NovaOS does not
 * read MsiPatchSequence; patches that ship whole files do not depend on
 * each other's order).  Those that do not fit get dwOrder -1 and why. */
static void sequence(const char *code, DWORD n, MSIPATCHSEQUENCEINFOW *pi)
{
    DWORD order = 0;
    for (DWORD i = 0; i < n; i++) {
        pi[i].uStatus = patch_fits(&pi[i], code);
        pi[i].dwOrder = pi[i].uStatus ? (DWORD)-1 : order++;
    }
}

/* The ProductCode in a package's Property table */
static UINT package_code(LPCWSTR path, char *code, int cap)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return E_INSTALL_PACKAGE_OPEN;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    void *buf = malloc(sz ? sz : 1);
    bool ok = buf && ReadFile(h, buf, sz, &rd, NULL) && rd == sz;
    CloseHandle(h);
    MsiDb db;
    memset(&db, 0, sizeof(db));
    if (!ok || !msidb_open(&db, buf, sz)) { free(buf); return E_INSTALL_PACKAGE_INVALID; }
    code[0] = 0;
    MsiTable *pt = msidb_table(&db, "Property");
    char b[16];
    int r = pt ? msidb_find(&db, pt, 0, "ProductCode", 0) : -1;
    if (r >= 0) snprintf(code, (size_t)cap, "%s", msidb_str(&db, pt, r, 1, b));
    msidb_close(&db);
    free(buf);
    return code[0] ? ERROR_SUCCESS : E_INSTALL_PACKAGE_INVALID;
}

MSIAPI MsiDetermineApplicablePatchesW(LPCWSTR package, DWORD n, MSIPATCHSEQUENCEINFOW *pi)
{
    if (!package || !*package || !n || !pi) return ERROR_INVALID_PARAMETER;
    char code[64];
    UINT r = package_code(package, code, sizeof(code));
    if (r) return r;
    sequence(code, n, pi);
    return ERROR_SUCCESS;
}

MSIAPI MsiDeterminePatchSequenceW(LPCWSTR product, LPCWSTR sid, DWORD ctx, DWORD n, MSIPATCHSEQUENCEINFOW *pi)
{
    if (!is_guid(product) || !n || !pi) return ERROR_INVALID_PARAMETER;
    UINT r = check_context(sid, ctx, true);
    if (r) return r;
    if (!product_context(product, sid, ctx)) return E_UNKNOWN_PRODUCT;
    char code[64];
    WideCharToMultiByte(CP_UTF8, 0, product, -1, code, sizeof(code), NULL, NULL);
    sequence(code, n, pi);
    return ERROR_SUCCESS;
}

/* The ANSI forms: the same on UTF-16 copies of the patch data */
static UINT determine_a(LPCSTR product_or_package, bool package, LPCSTR sid, DWORD ctx, DWORD n, MSIPATCHSEQUENCEINFOA *pa)
{
    if (!product_or_package || !n || !pa) return ERROR_INVALID_PARAMETER;
    MSIPATCHSEQUENCEINFOW *pw = calloc(n, sizeof(*pw));
    if (!pw) return ERROR_OUTOFMEMORY;
    for (DWORD i = 0; i < n; i++) { pw[i].szPatchData = a2w(pa[i].szPatchData); pw[i].ePatchDataType = pa[i].ePatchDataType; }
    WCHAR *p = a2w(product_or_package), *s = a2w(sid);
    UINT r = package ? MsiDetermineApplicablePatchesW(p, n, pw) : MsiDeterminePatchSequenceW(p, s, ctx, n, pw);
    for (DWORD i = 0; i < n; i++) {
        if (!r) { pa[i].dwOrder = pw[i].dwOrder; pa[i].uStatus = pw[i].uStatus; }
        free((void *)pw[i].szPatchData);
    }
    free(pw); free(p); free(s);
    return r;
}

MSIAPI MsiDetermineApplicablePatchesA(LPCSTR package, DWORD n, MSIPATCHSEQUENCEINFOA *pi)
{
    return determine_a(package, true, NULL, 0, n, pi);
}

MSIAPI MsiDeterminePatchSequenceA(LPCSTR product, LPCSTR sid, DWORD ctx, DWORD n, MSIPATCHSEQUENCEINFOA *pi)
{
    return determine_a(product, false, sid, ctx, n, pi);
}

/* -----------------------------------------------------------------------
 * Source lists
 * ----------------------------------------------------------------------- */
/* Open the source list of a product (or of a patch: the source list of
 * the first product it is applied to) */
static UINT open_sourcelist(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, REGSAM sam, HKEY *out)
{
    if (!is_guid(code)) return ERROR_INVALID_PARAMETER;
    UINT r = check_context(sid, ctx, true);
    if (r) return r;
    WCHAR key[400];
    if (options & MSICODE_PATCH) {
        /* is the patch applied to a product in this context? */
        bool known = false;
        WCHAR pc[39], target[39];
        DWORD in;
        for (DWORD i = 0; !known && !nth_product(NULL, sid, ctx, i, target, &in); i++)
            for (DWORD j = 0; nth_patch(target, j, pc, NULL, 0); j++)
                if (!_wcsicmp(pc, code)) { known = true; break; }
        if (!known) return E_UNKNOWN_PATCH;
        _snwprintf(key, 400, L"SOFTWARE\\NovaOS\\Installer\\Patches\\%s\\SourceList", code);
    } else {
        if (!product_context(code, sid, ctx)) return E_UNKNOWN_PRODUCT;
        _snwprintf(key, 400, L"%s\\%s\\SourceList", PRODUCTS_KEY, code);
    }
    if (sam & KEY_WRITE) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, sam, NULL, out, NULL)) return E_BAD_CONFIGURATION;
    } else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, sam, out)) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, out, NULL)) return E_BAD_CONFIGURATION;
    }
    return ERROR_SUCCESS;
}

static const WCHAR *type_key(DWORD options)
{
    switch (options & 7) {
    case MSISOURCETYPE_NETWORK: return L"Net";
    case MSISOURCETYPE_URL:     return L"URL";
    case MSISOURCETYPE_MEDIA:   return L"Media";
    default:                    return NULL;
    }
}

#define MAX_SOURCES 64

/* The sources of one type, in order */
static int read_sources(HKEY list, const WCHAR *type, WCHAR src[][MAX_PATH])
{
    HKEY h;
    int n = 0;
    if (RegOpenKeyExW(list, type, 0, KEY_READ, &h)) return 0;
    for (int i = 1; n < MAX_SOURCES; i++) {
        WCHAR name[16];
        _snwprintf(name, 16, L"%d", i);
        if (!reg_text(h, name, src[n], MAX_PATH)) break;
        n++;
    }
    RegCloseKey(h);
    return n;
}

static UINT write_sources(HKEY list, const WCHAR *type, WCHAR src[][MAX_PATH], int n)
{
    HKEY h;
    if (RegCreateKeyExW(list, type, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &h, NULL)) return E_BAD_CONFIGURATION;
    for (int i = 1; ; i++) {                                  /* drop the old numbering */
        WCHAR name[16];
        _snwprintf(name, 16, L"%d", i);
        if (RegDeleteValueW(h, name)) break;
    }
    for (int i = 0; i < n; i++) {
        WCHAR name[16];
        _snwprintf(name, 16, L"%d", i + 1);
        RegSetValueExW(h, name, 0, REG_EXPAND_SZ, (const BYTE *)src[i], (DWORD)(wcslen(src[i]) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(h);
    return ERROR_SUCCESS;
}

/* A network source ends in a backslash, as Windows stores it */
static void normalize_source(DWORD options, LPCWSTR in, WCHAR *out)
{
    wcsncpy(out, in, MAX_PATH - 2);
    out[MAX_PATH - 2] = 0;
    size_t n = wcslen(out);
    if ((options & 7) == MSISOURCETYPE_NETWORK && n && out[n - 1] != L'\\') wcscat(out, L"\\");
    if ((options & 7) == MSISOURCETYPE_URL && n && out[n - 1] != L'/') wcscat(out, L"/");
}

/* Add @source at position @index (1-based; 0 or past the end: the end).
 * A source already in the list moves there. */
MSIAPI MsiSourceListAddSourceExW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR source, DWORD index)
{
    const WCHAR *type = type_key(options);
    if (!source || !*source || !type || (options & 7) == MSISOURCETYPE_MEDIA || (options & ~(7 | MSICODE_PATCH)))
        return ERROR_INVALID_PARAMETER;
    HKEY list;
    UINT r = open_sourcelist(code, sid, ctx, options, KEY_ALL_ACCESS, &list);
    if (r) return r;
    static WCHAR src[MAX_SOURCES + 1][MAX_PATH];
    WCHAR s[MAX_PATH];
    normalize_source(options, source, s);
    int n = read_sources(list, type, src);
    for (int i = 0; i < n; i++)
        if (!_wcsicmp(src[i], s)) {                           /* already there: take it out, then place it */
            memmove(src[i], src[i + 1], (size_t)(n - i - 1) * sizeof(src[0]));
            n--;
            break;
        }
    if (n >= MAX_SOURCES) { RegCloseKey(list); return ERROR_OUTOFMEMORY; }
    int at = index && index <= (DWORD)n ? (int)index - 1 : n;
    memmove(src[at + 1], src[at], (size_t)(n - at) * sizeof(src[0]));
    wcscpy(src[at], s);
    r = write_sources(list, type, src, n + 1);
    RegCloseKey(list);
    return r;
}

MSIAPI MsiSourceListAddSourceExA(LPCSTR code, LPCSTR sid, DWORD ctx, DWORD options, LPCSTR source, DWORD index)
{
    WCHAR *c = a2w(code), *s = a2w(sid), *src = a2w(source);
    UINT r = MsiSourceListAddSourceExW(c, s, ctx, options, src, index);
    free(c); free(s); free(src);
    return r;
}

/* (the older form: a network source of a product, per-machine or the
 * current user's) */
MSIAPI MsiSourceListAddSourceW(LPCWSTR product, LPCWSTR user, DWORD reserved, LPCWSTR source)
{
    if (reserved) return ERROR_INVALID_PARAMETER;
    DWORD ctx = product_context(product, NULL, CTX_ALL);
    if (!ctx) return is_guid(product) ? E_UNKNOWN_PRODUCT : ERROR_INVALID_PARAMETER;
    (void)user;
    return MsiSourceListAddSourceExW(product, NULL, ctx, MSISOURCETYPE_NETWORK, source, 0);
}

MSIAPI MsiSourceListAddSourceA(LPCSTR product, LPCSTR user, DWORD reserved, LPCSTR source)
{
    WCHAR *p = a2w(product), *u = a2w(user), *s = a2w(source);
    UINT r = MsiSourceListAddSourceW(p, u, reserved, s);
    free(p); free(u); free(s);
    return r;
}

/* The @index-th (0-based) source of the types in @options (network first,
 * then URL, when both are asked for) */
MSIAPI MsiSourceListEnumSourcesW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, DWORD index, LPWSTR buf, LPDWORD len)
{
    if ((buf && !len) || (options & ~(7 | MSICODE_PATCH)) || (options & MSISOURCETYPE_MEDIA)) return ERROR_INVALID_PARAMETER;
    if (!(options & 3)) return ERROR_INVALID_PARAMETER;
    HKEY list;
    UINT r = open_sourcelist(code, sid, ctx, options, KEY_READ, &list);
    if (r) return r;
    static WCHAR src[MAX_SOURCES][MAX_PATH];
    DWORD seen = 0;
    r = ERROR_NO_MORE_ITEMS;
    for (int t = 0; t < 2 && r == ERROR_NO_MORE_ITEMS; t++) {
        DWORD bit = t ? MSISOURCETYPE_URL : MSISOURCETYPE_NETWORK;
        if (!(options & bit)) continue;
        int n = read_sources(list, type_key(bit), src);
        if (index < seen + (DWORD)n) r = put_w(src[index - seen], buf, len);
        seen += (DWORD)n;
    }
    RegCloseKey(list);
    return r;
}

MSIAPI MsiSourceListEnumSourcesA(LPCSTR code, LPCSTR sid, DWORD ctx, DWORD options, DWORD index, LPSTR buf, LPDWORD len)
{
    WCHAR *c = a2w(code), *s = a2w(sid), v[MAX_PATH];
    DWORD n = MAX_PATH;
    UINT r = MsiSourceListEnumSourcesW(c, s, ctx, options, index, v, &n);
    free(c); free(s);
    return r ? r : put_a(v, buf, len);
}

MSIAPI MsiSourceListGetInfoW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR prop, LPWSTR buf, LPDWORD len)
{
    if (!prop || (buf && !len)) return ERROR_INVALID_PARAMETER;
    HKEY list;
    UINT r = open_sourcelist(code, sid, ctx, options, KEY_READ, &list);
    if (r) return r;
    WCHAR v[MAX_PATH + 8] = L"";
    if (!_wcsicmp(prop, L"LastUsedType")) {
        reg_text(list, L"LastUsedSource", v, MAX_PATH + 8);
        v[v[0] ? 1 : 0] = 0;
    } else if (!_wcsicmp(prop, L"LastUsedSource")) {
        reg_text(list, L"LastUsedSource", v, MAX_PATH + 8);   /* "n;1;path": the path */
        WCHAR *p = wcschr(v, L';');
        p = p ? wcschr(p + 1, L';') : NULL;
        if (p) memmove(v, p + 1, (wcslen(p + 1) + 1) * sizeof(WCHAR));
    } else if (!_wcsicmp(prop, L"PackageName") || !_wcsicmp(prop, L"MediaPackagePath") || !_wcsicmp(prop, L"DiskPrompt")) {
        reg_text(list, prop, v, MAX_PATH + 8);
    } else { RegCloseKey(list); return E_UNKNOWN_PROPERTY; }
    RegCloseKey(list);
    return put_w(v, buf, len);
}

MSIAPI MsiSourceListGetInfoA(LPCSTR code, LPCSTR sid, DWORD ctx, DWORD options, LPCSTR prop, LPSTR buf, LPDWORD len)
{
    WCHAR *c = a2w(code), *s = a2w(sid), *p = a2w(prop), v[MAX_PATH + 8];
    DWORD n = MAX_PATH + 8;
    UINT r = MsiSourceListGetInfoW(c, s, ctx, options, p, v, &n);
    free(c); free(s); free(p);
    return r ? r : put_a(v, buf, len);
}

/* PackageName, MediaPackagePath, DiskPrompt, LastUsedSource (a source of
 * the type @options names, added if it is not in the list yet) */
MSIAPI MsiSourceListSetInfoW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR prop, LPCWSTR value)
{
    if (!prop || !value) return ERROR_INVALID_PARAMETER;
    HKEY list;
    UINT r;
    if (!_wcsicmp(prop, L"LastUsedSource")) {
        const WCHAR *type = type_key(options);
        if (!type || (options & 7) == MSISOURCETYPE_MEDIA) return ERROR_INVALID_PARAMETER;
        r = open_sourcelist(code, sid, ctx, options, KEY_ALL_ACCESS, &list);
        if (r) return r;
        static WCHAR src[MAX_SOURCES + 1][MAX_PATH];
        WCHAR s[MAX_PATH];
        normalize_source(options, value, s);
        int n = read_sources(list, type, src), at = -1;
        for (int i = 0; i < n; i++) if (!_wcsicmp(src[i], s)) at = i;
        RegCloseKey(list);
        if (at < 0) {
            r = MsiSourceListAddSourceExW(code, sid, ctx, options, value, 0);
            if (r) return r;
            at = n;
        }
        r = open_sourcelist(code, sid, ctx, options, KEY_ALL_ACCESS, &list);
        if (r) return r;
        WCHAR v[MAX_PATH + 16];
        _snwprintf(v, MAX_PATH + 16, L"%c;%d;%s", (options & 7) == MSISOURCETYPE_URL ? L'u' : L'n', at + 1, s);
        RegSetValueExW(list, L"LastUsedSource", 0, REG_EXPAND_SZ, (const BYTE *)v, (DWORD)(wcslen(v) + 1) * sizeof(WCHAR));
        RegCloseKey(list);
        return ERROR_SUCCESS;
    }
    if (_wcsicmp(prop, L"PackageName") && _wcsicmp(prop, L"MediaPackagePath") && _wcsicmp(prop, L"DiskPrompt"))
        return E_UNKNOWN_PROPERTY;
    r = open_sourcelist(code, sid, ctx, options, KEY_ALL_ACCESS, &list);
    if (r) return r;
    RegSetValueExW(list, prop, 0, REG_SZ, (const BYTE *)value, (DWORD)(wcslen(value) + 1) * sizeof(WCHAR));
    RegCloseKey(list);
    return ERROR_SUCCESS;
}

MSIAPI MsiSourceListSetInfoA(LPCSTR code, LPCSTR sid, DWORD ctx, DWORD options, LPCSTR prop, LPCSTR value)
{
    WCHAR *c = a2w(code), *s = a2w(sid), *p = a2w(prop), *v = a2w(value);
    UINT r = MsiSourceListSetInfoW(c, s, ctx, options, p, v);
    free(c); free(s); free(p); free(v);
    return r;
}

/* Take one source out of the list */
MSIAPI MsiSourceListClearSourceW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options, LPCWSTR source)
{
    const WCHAR *type = type_key(options);
    if (!source || !*source || !type || (options & 7) == MSISOURCETYPE_MEDIA) return ERROR_INVALID_PARAMETER;
    HKEY list;
    UINT r = open_sourcelist(code, sid, ctx, options, KEY_ALL_ACCESS, &list);
    if (r) return r;
    static WCHAR src[MAX_SOURCES][MAX_PATH];
    WCHAR s[MAX_PATH];
    normalize_source(options, source, s);
    int n = read_sources(list, type, src);
    for (int i = 0; i < n; i++)
        if (!_wcsicmp(src[i], s)) {
            memmove(src[i], src[i + 1], (size_t)(n - i - 1) * sizeof(src[0]));
            r = write_sources(list, type, src, n - 1);
            RegCloseKey(list);
            return r;
        }
    RegCloseKey(list);
    return E_BAD_ARGUMENTS;                               /* (not in the list) */
}

MSIAPI MsiSourceListClearAllExW(LPCWSTR code, LPCWSTR sid, DWORD ctx, DWORD options)
{
    const WCHAR *type = type_key(options);
    if (!type || (options & 7) == MSISOURCETYPE_MEDIA) return ERROR_INVALID_PARAMETER;
    HKEY list;
    UINT r = open_sourcelist(code, sid, ctx, options, KEY_ALL_ACCESS, &list);
    if (r) return r;
    r = write_sources(list, type, NULL, 0);
    RegCloseKey(list);
    return r;
}

MSIAPI MsiSourceListClearAllW(LPCWSTR product, LPCWSTR user, DWORD reserved)
{
    if (reserved) return ERROR_INVALID_PARAMETER;
    (void)user;
    DWORD ctx = product_context(product, NULL, CTX_ALL);
    if (!ctx) return is_guid(product) ? E_UNKNOWN_PRODUCT : ERROR_INVALID_PARAMETER;
    return MsiSourceListClearAllExW(product, NULL, ctx, MSISOURCETYPE_NETWORK);
}
