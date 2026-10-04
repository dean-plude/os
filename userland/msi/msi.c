/*
 * msi.c — msi.dll: the Windows Installer API used by programs and
 * setup bootstrappers (MsiInstallProduct, MsiConfigureProduct, product
 * queries).  The engine is in install.c.
 */
#include "msi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MSIAPI __declspec(dllexport) UINT WINAPI

#ifndef ERROR_UNKNOWN_PRODUCT
#define ERROR_UNKNOWN_PRODUCT             1605
#endif
#ifndef ERROR_UNKNOWN_PROPERTY
#define ERROR_UNKNOWN_PROPERTY            1608
#endif
#ifndef ERROR_INSTALL_PACKAGE_OPEN_FAILED
#define ERROR_INSTALL_PACKAGE_OPEN_FAILED 1619
#endif
#ifndef ERROR_INSTALL_PACKAGE_INVALID
#define ERROR_INSTALL_PACKAGE_INVALID     1620
#endif

static int   g_ui_level = MSIUI_FULL;
static WCHAR g_logfile[MAX_PATH];

/* INSTALLSTATE */
#define INSTALLSTATE_UNKNOWN  (-1)
#define INSTALLSTATE_ABSENT   2
#define INSTALLSTATE_LOCAL    3
#define INSTALLSTATE_DEFAULT  5

__declspec(dllexport) int WINAPI MsiSetInternalUI(int level, HWND *phwnd)
{
    (void)phwnd;
    int prev = g_ui_level;
    int l = level & 0xFF;
    if (l >= MSIUI_NONE && l <= MSIUI_FULL) g_ui_level = l;
    else if (l == 1 /* DEFAULT */) g_ui_level = MSIUI_FULL;
    return prev;
}

MSIAPI MsiEnableLogW(DWORD mode, LPCWSTR file, DWORD attrs)
{
    (void)mode; (void)attrs;
    if (file) wcsncpy(g_logfile, file, MAX_PATH - 1); else g_logfile[0] = 0;
    return ERROR_SUCCESS;
}

MSIAPI MsiEnableLogA(DWORD mode, LPCSTR file, DWORD attrs)
{
    WCHAR w[MAX_PATH];
    if (file) MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH);
    return MsiEnableLogW(mode, file ? w : NULL, attrs);
}

MSIAPI MsiInstallProductW(LPCWSTR package, LPCWSTR cmdline)
{
    if (!package) return ERROR_INVALID_PARAMETER;
    MsiRequest req;
    memset(&req, 0, sizeof(req));
    req.package = package;
    req.properties = cmdline;
    req.ui_level = g_ui_level;
    req.logfile = g_logfile[0] ? g_logfile : NULL;
    /* REMOVE=ALL on the command line means uninstall */
    if (cmdline && wcsstr(cmdline, L"REMOVE=ALL")) req.remove = true;
    return (UINT)MsiRunInstall(&req, NULL, 0);
}

/* MsiApplyPatch: a patch on the installed product it names (@product, a
 * product code or package, picks nothing here: the patch says) */
MSIAPI MsiApplyPatchW(LPCWSTR patch, LPCWSTR product, int type, LPCWSTR cmdline)
{
    (void)product; (void)type;
    if (!patch) return ERROR_INVALID_PARAMETER;
    MsiRequest req;
    memset(&req, 0, sizeof(req));
    req.patch = patch;
    req.properties = cmdline;
    req.ui_level = g_ui_level;
    req.logfile = g_logfile[0] ? g_logfile : NULL;
    return (UINT)MsiRunInstall(&req, NULL, 0);
}

MSIAPI MsiApplyPatchA(LPCSTR patch, LPCSTR product, int type, LPCSTR cmdline)
{
    WCHAR p[MAX_PATH], c[2048];
    (void)product;
    if (!patch) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_ACP, 0, patch, -1, p, MAX_PATH);
    if (cmdline) MultiByteToWideChar(CP_ACP, 0, cmdline, -1, c, 2048);
    return MsiApplyPatchW(p, NULL, type, cmdline ? c : NULL);
}

MSIAPI MsiInstallProductA(LPCSTR package, LPCSTR cmdline)

{
    WCHAR p[MAX_PATH], c[2048];
    if (!package) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_ACP, 0, package, -1, p, MAX_PATH);
    if (cmdline) MultiByteToWideChar(CP_ACP, 0, cmdline, -1, c, 2048);
    return MsiInstallProductW(p, cmdline ? c : NULL);
}

MSIAPI MsiConfigureProductExW(LPCWSTR product, int level, int state, LPCWSTR cmdline)
{
    (void)level;
    if (!product) return ERROR_INVALID_PARAMETER;
    MsiRequest req;
    memset(&req, 0, sizeof(req));
    req.product_code = product;
    req.properties = cmdline;
    req.remove = state == INSTALLSTATE_ABSENT;
    req.ui_level = g_ui_level;
    req.logfile = g_logfile[0] ? g_logfile : NULL;
    return (UINT)MsiRunInstall(&req, NULL, 0);
}

MSIAPI MsiConfigureProductW(LPCWSTR product, int level, int state)
{
    return MsiConfigureProductExW(product, level, state, NULL);
}

MSIAPI MsiConfigureProductA(LPCSTR product, int level, int state)
{
    WCHAR p[64];
    if (!product) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_ACP, 0, product, -1, p, 64);
    return MsiConfigureProductW(p, level, state);
}

MSIAPI MsiConfigureProductExA(LPCSTR product, int level, int state, LPCSTR cmdline)
{
    WCHAR p[64], c[2048];
    if (!product) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_ACP, 0, product, -1, p, 64);
    if (cmdline) MultiByteToWideChar(CP_ACP, 0, cmdline, -1, c, 2048);
    return MsiConfigureProductExW(p, level, state, cmdline ? c : NULL);
}

static bool open_product(LPCWSTR product, HKEY *out)
{
    WCHAR key[300];
    _snwprintf(key, 300, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", product);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, out) == ERROR_SUCCESS;
}

__declspec(dllexport) int WINAPI MsiQueryProductStateW(LPCWSTR product)
{
    HKEY h;
    if (!product || !open_product(product, &h)) return INSTALLSTATE_UNKNOWN;
    RegCloseKey(h);
    return INSTALLSTATE_DEFAULT;
}

__declspec(dllexport) int WINAPI MsiQueryProductStateA(LPCSTR product)
{
    WCHAR p[64];
    if (!product) return INSTALLSTATE_UNKNOWN;
    MultiByteToWideChar(CP_ACP, 0, product, -1, p, 64);
    return MsiQueryProductStateW(p);
}

MSIAPI MsiVerifyPackageW(LPCWSTR package)
{
    HANDLE h = CreateFileW(package, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return ERROR_INSTALL_PACKAGE_OPEN_FAILED;
    unsigned char sig[8];
    DWORD rd = 0;
    ReadFile(h, sig, 8, &rd, NULL);
    CloseHandle(h);
    static const unsigned char cfb[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    return rd == 8 && !memcmp(sig, cfb, 8) ? ERROR_SUCCESS : ERROR_INSTALL_PACKAGE_INVALID;
}

MSIAPI MsiVerifyPackageA(LPCSTR package)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, package, -1, w, MAX_PATH);
    return MsiVerifyPackageW(w);
}

/* Registered products sharing an upgrade code */
MSIAPI MsiEnumRelatedProductsW(LPCWSTR upgrade, DWORD reserved, DWORD index, LPWSTR buf)
{
    (void)reserved;
    if (!upgrade) return ERROR_INVALID_PARAMETER;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NovaOS\\Installer\\Products", 0, KEY_READ, &h)) return ERROR_NO_MORE_ITEMS;
    DWORD found = 0;
    UINT r = ERROR_NO_MORE_ITEMS;
    for (DWORD i = 0; ; i++) {
        WCHAR name[64], up[64];
        DWORD n = 64, type, size = sizeof(up);
        if (RegEnumKeyExW(h, i, name, &n, NULL, NULL, NULL, NULL)) break;
        HKEY p;
        if (RegOpenKeyExW(h, name, 0, KEY_READ, &p)) continue;
        LONG q = RegQueryValueExW(p, L"UpgradeCode", NULL, &type, (BYTE *)up, &size);
        RegCloseKey(p);
        if (q || _wcsicmp(up, upgrade)) continue;
        if (found++ == index) { if (buf) wcscpy(buf, name); r = ERROR_SUCCESS; break; }
    }
    RegCloseKey(h);
    return r;
}

MSIAPI MsiEnumRelatedProductsA(LPCSTR upgrade, DWORD reserved, DWORD index, LPSTR buf)
{
    WCHAR u[64], w[64];
    if (!upgrade) return ERROR_INVALID_PARAMETER;
    MultiByteToWideChar(CP_ACP, 0, upgrade, -1, u, 64);
    UINT r = MsiEnumRelatedProductsW(u, reserved, index, w);
    if (!r && buf) WideCharToMultiByte(CP_ACP, 0, w, -1, buf, 39, NULL, NULL);
    return r;
}

__declspec(dllexport) int WINAPI MsiSetExternalUIRecord(void *handler, DWORD filter, void *ctx, void **prev)
{
    (void)handler; (void)filter; (void)ctx;
    if (prev) *prev = NULL;
    return ERROR_SUCCESS;
}

__declspec(dllexport) void *WINAPI MsiSetExternalUIW(void *handler, DWORD filter, void *ctx)
{
    (void)handler; (void)filter; (void)ctx;
    return NULL;
}

__declspec(dllexport) void *WINAPI MsiSetExternalUIA(void *handler, DWORD filter, void *ctx)
{
    (void)handler; (void)filter; (void)ctx;
    return NULL;
}
