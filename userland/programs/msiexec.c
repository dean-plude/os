/*
 * msiexec.exe — Windows Installer command line
 *
 *   msiexec /i PACKAGE.msi [PROPERTY=value ...]   install
 *   msiexec /x PACKAGE.msi | /x {ProductCode}     uninstall
 *   msiexec /p PATCH.msp [PROPERTY=value ...]     patch the product it is for
 *   msiexec /uninstall PATCH.msp                  take a patch off again
 *   msiexec PACKAGE.msi                           install
 *   TRANSFORMS=a.mst;b.mst  PATCH=a.msp           with /i: transforms, patches
 *   /qn  no UI    /qb  progress only    /passive  progress only
 *   /l*v FILE  or  /log FILE            write a log
 *
 * The work is in msi.dll (MsiRunInstall).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "../msi/msi.h"

__declspec(dllimport) LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR cmdline, int *argc);
__declspec(dllimport) int WINAPI MsiNovaCaServer(HANDLE req, HANDLE rep, unsigned session, LPCWSTR dll, LPCSTR entry);

static void usage(bool gui)
{
    const WCHAR *text =
        L"Windows Installer (NovaOS)\n\n"
        L"msiexec /i package.msi [PROPERTY=value ...]\n"
        L"msiexec /x package.msi | /x {ProductCode}\n"
        L"msiexec /p patch.msp   msiexec /uninstall patch.msp\n\n"
        L"Options: /qn (no UI)  /qb, /passive (progress only)  /l*v file (log)";
    if (gui) MessageBoxW(NULL, text, L"Windows Installer", MB_OK | MB_ICONINFORMATION);
    else wprintf(L"%s\n", text);
}

int main(void)
{
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    /* msiexec /novaca REQ REP SESSION ENTRY DLL: the custom-action server the
     * installer starts to run a DLL custom action (see msi/api.c) */
    if (argc == 7 && !_wcsicmp(argv[1], L"/novaca")) {
        char entry[128];
        WideCharToMultiByte(CP_UTF8, 0, argv[5], -1, entry, sizeof(entry), NULL, NULL);
        return MsiNovaCaServer((HANDLE)(ULONG_PTR)wcstoul(argv[2], NULL, 10), (HANDLE)(ULONG_PTR)wcstoul(argv[3], NULL, 10),
                               (unsigned)wcstoul(argv[4], NULL, 10), argv[6], entry);
    }
    MsiRequest req;
    memset(&req, 0, sizeof(req));
    req.ui_level = MSIUI_FULL;
    static WCHAR props[4096], logfile[MAX_PATH];
    const WCHAR *package = NULL;
    bool help = false;
    for (int i = 1; i < argc; i++) {
        const WCHAR *a = argv[i];
        if (a[0] == L'/' || a[0] == L'-') {
            WCHAR opt = (WCHAR)towlower(a[1]);
            if (!_wcsicmp(a + 1, L"update") || (opt == L'p' && !iswalpha(a[2]))) {
                if (opt == L'p' && a[2]) req.patch = a + 2; else if (i + 1 < argc) req.patch = argv[++i];
            } else if (!_wcsicmp(a + 1, L"package")) {
                if (i + 1 < argc) package = argv[++i];
            } else if (!_wcsicmp(a + 1, L"uninstall")) {
                req.remove = true;
                if (i + 1 < argc) package = argv[++i];
            } else if (opt == L'i' || opt == L'f') {
                if (a[2]) package = a + 2; else if (i + 1 < argc) package = argv[++i];
            } else if (opt == L'x') {
                req.remove = true;
                if (a[2]) package = a + 2; else if (i + 1 < argc) package = argv[++i];
            } else if (opt == L'q') {
                WCHAR m = (WCHAR)towlower(a[2]);
                req.ui_level = (m == L'n' || !m) ? MSIUI_NONE : (m == L'b' || m == L'r') ? MSIUI_BASIC : MSIUI_FULL;
            } else if (!_wcsicmp(a + 1, L"passive")) {
                req.ui_level = MSIUI_BASIC;
            } else if (!_wcsicmp(a + 1, L"quiet")) {
                req.ui_level = MSIUI_NONE;
            } else if (opt == L'l') {
                if (!_wcsicmp(a + 1, L"log") || !a[2]) { if (i + 1 < argc) wcsncpy(logfile, argv[++i], MAX_PATH - 1); }
                else if (i + 1 < argc) wcsncpy(logfile, argv[++i], MAX_PATH - 1);   /* /l*v FILE */
            } else if (opt == L'n' || opt == L'a' || opt == L'j' || opt == L'y' || opt == L'z' || opt == L'c') {
                /* /norestart, /a admin install, /j advertise, /y, /z: accepted, ignored */
                if ((opt == L'a' || opt == L'j') && i + 1 < argc) i++;
            } else if (opt == L'?' || opt == L'h') {
                help = true;
            } else {
                /* unknown switch: ignored, as msiexec mostly does */
            }
        } else if (wcschr(a, L'=')) {
            if (props[0]) wcscat(props, L" ");
            /* keep quotes around values with spaces */
            const WCHAR *eq = wcschr(a, L'=');
            size_t n = wcslen(props);
            _snwprintf(props + n, 4096 - n, L"%.*s=\"%s\"", (int)(eq - a), a, eq + 1);
        } else if (!package) {
            package = a;
        }
    }
    /* /uninstall PATCH.msp: the patch comes off its product */
    size_t plen = package ? wcslen(package) : 0;
    if (req.remove && plen > 4 && !_wcsicmp(package + plen - 4, L".msp")) { req.patch = package; package = NULL; }
    if (help || (!package && !req.patch)) { usage(req.ui_level > MSIUI_NONE); return help ? 0 : 1639; }
    if (package && package[0] == L'{') req.product_code = package; else req.package = package;
    if (req.patch && package && !req.remove) {      /* /i PACKAGE /p PATCH: install with the patch */
        size_t n = wcslen(props);
        _snwprintf(props + n, 4096 - n, L"%sPATCH=\"%s\"", n ? L" " : L"", req.patch);
        req.patch = NULL;
    }

    req.properties = props;
    req.logfile = logfile[0] ? logfile : NULL;
    char err[256];
    int r = MsiRunInstall(&req, err, sizeof(err));
    if (r && err[0]) fprintf(stderr, "msiexec: %s (error %d)\n", err, r);
    else if (!r) printf("msiexec: done\n");
    return r;
}
