/*
 * wv2host.exe — a small WebView2 host: the way Roblox, the GOG and Epic
 * launchers and many other programs show web pages.  It loads Microsoft's
 * WebView2Loader.dll (from the WebView2 SDK; not part of NovaOS), which
 * finds the installed Edge WebView2 runtime and starts its browser process
 * (msedgewebview2.exe), puts a WebView in a window, shows a page and runs
 * a script in it.
 *
 *   wv2host [LOADER] [URL] [HOLD]
 *                            LOADER: WebView2Loader.dll's path (default:
 *                            found on the DLL search path); URL: the page
 *                            (default: a page of its own; "-" for it);
 *                            HOLD: seconds to keep the page shown after
 *                            "done" (for a screenshot)
 *
 * It prints each step as it gets there, so a run that stops says where:
 *   wv2host: runtime VERSION                 the loader found the runtime
 *   wv2host: environment                     the browser process answered
 *   wv2host: controller                      a WebView is in the window
 *   wv2host: navigation ok|failed (STATUS)   the page loaded
 *   wv2host: script RESULT                   a script ran in the page
 *   wv2host: done                            (exit code 0)
 * A browser or page process that fails is reported as it happens
 * ("process failed KIND"); the host gives up after ten minutes.  The
 * interfaces are declared here by vtable slot, from the SDK's
 * WebView2.idl (BSD licence).
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <objbase.h>

__declspec(dllimport) LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR cmd, int *argc);

typedef HRESULT (WINAPI *CreateEnvFn)(PCWSTR, PCWSTR, void *, void *);
typedef HRESULT (WINAPI *VersionFn)(PCWSTR, LPWSTR *);

static const GUID IID_EnvHandler = { 0x4e8a3389, 0xc9d8, 0x4bd2, { 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d } };
static const GUID IID_ControllerHandler = { 0x6c4819f3, 0xc9b7, 0x4260, { 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c } };
static const GUID IID_NavHandler = { 0xd33a35bf, 0x1c49, 0x4f98, { 0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c } };
static const GUID IID_ScriptHandler = { 0x49511172, 0xcc67, 0x4bca, { 0x99, 0x23, 0x13, 0x71, 0x12, 0xf4, 0xcc, 0x4c } };
static const GUID IID_FailedHandler = { 0x79e0aea4, 0x990b, 0x42d9, { 0xa1, 0xd0, 0x0f, 0xcc, 0x2e, 0x5b, 0xc7, 0xf1 } };

/* vtable slots (IUnknown takes 0-2) */
typedef struct { void **vtbl; } Com;
#define SLOT(o, i, T) ((T)((Com *)(o))->vtbl[i])
enum {
    ENV_CREATE_CONTROLLER = 3,
    CTL_PUT_BOUNDS = 6, CTL_GET_COREWEBVIEW2 = 25,
    WV_NAVIGATE = 5, WV_NAVIGATE_TO_STRING = 6, WV_ADD_NAVIGATION_COMPLETED = 15,
    WV_ADD_PROCESS_FAILED = 25, WV_EXECUTE_SCRIPT = 29, WV_BROWSER_PROCESS_ID = 37,
    NAV_IS_SUCCESS = 3, NAV_WEB_ERROR_STATUS = 4,
    FAILED_KIND = 3,
};
typedef ULONG (STDMETHODCALLTYPE *ReleaseFn)(void *);
typedef HRESULT (STDMETHODCALLTYPE *ObjOutFn)(void *, void *, void *);
typedef HRESULT (STDMETHODCALLTYPE *OutFn)(void *, void *);
typedef HRESULT (STDMETHODCALLTYPE *RectFn)(void *, RECT);
typedef HRESULT (STDMETHODCALLTYPE *StrFn)(void *, LPCWSTR);
typedef HRESULT (STDMETHODCALLTYPE *StrObjFn)(void *, LPCWSTR, void *);
typedef HRESULT (STDMETHODCALLTYPE *AddFn)(void *, void *, INT64 *);

static HWND g_window;
static void *g_controller, *g_webview;
static int g_exit = 1;
static int g_hold;                  /* seconds the page stays up after the script */

static void finish(int code)
{
    g_exit = code;
    PostQuitMessage(code);
}

/* One handler type for every callback: @iid says which interface it is,
 * @invoke what it does.  Invoke is slot 3 of each. */
typedef struct Handler {
    void **vtbl;
    LONG refs;
    const GUID *iid;
    HRESULT (*invoke)(HRESULT, void *);
} Handler;

static HRESULT STDMETHODCALLTYPE h_query(Handler *h, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, h->iid)) {
        *out = h;
        InterlockedIncrement(&h->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(Handler *h) { return InterlockedIncrement(&h->refs); }
static ULONG STDMETHODCALLTYPE h_release(Handler *h) { return InterlockedDecrement(&h->refs); }  /* static objects */
/* Completed handlers get (HRESULT, object); event handlers (sender, args) */
static HRESULT STDMETHODCALLTYPE h_invoke(Handler *h, void *a, void *b)
{
    if (h->iid == &IID_NavHandler || h->iid == &IID_FailedHandler)
        return h->invoke(S_OK, b);
    return h->invoke((HRESULT)(INT_PTR)a, b);
}
static void *h_vtbl[] = { h_query, h_addref, h_release, h_invoke };

static HRESULT on_script(HRESULT hr, void *result)
{
    if (FAILED(hr)) {
        printf("wv2host: script failed %08lx\n", hr);
        finish(5);
        return S_OK;
    }
    printf("wv2host: script %ls\n", (LPCWSTR)result);
    printf("wv2host: done\n");
    if (g_hold > 0) {
        g_exit = 0;
        SetTimer(g_window, 2, g_hold * 1000, NULL);
    } else
        finish(0);
    return S_OK;
}
static Handler g_script = { h_vtbl, 1, &IID_ScriptHandler, on_script };

static HRESULT on_navigation(HRESULT hr, void *args)
{
    BOOL ok = FALSE;
    int status = 0;
    (void)hr;
    SLOT(args, NAV_IS_SUCCESS, OutFn)(args, &ok);
    SLOT(args, NAV_WEB_ERROR_STATUS, OutFn)(args, &status);
    if (!ok) {
        printf("wv2host: navigation failed (%d)\n", status);
        finish(4);
        return S_OK;
    }
    printf("wv2host: navigation ok\n");
    SLOT(g_webview, WV_EXECUTE_SCRIPT, StrObjFn)(g_webview, L"document.title + ' / ' + (6 * 7)", &g_script);
    return S_OK;
}
static Handler g_navigation = { h_vtbl, 1, &IID_NavHandler, on_navigation };

static HRESULT on_failed(HRESULT hr, void *args)
{
    int kind = -1;
    (void)hr;
    SLOT(args, FAILED_KIND, OutFn)(args, &kind);
    printf("wv2host: process failed %d\n", kind);   /* 0: browser, 1: page, 2: page unresponsive, ... */
    if (kind == 0) finish(6);
    return S_OK;
}
static Handler g_failed = { h_vtbl, 1, &IID_FailedHandler, on_failed };

static LPCWSTR g_url;

static HRESULT on_controller(HRESULT hr, void *controller)
{
    RECT r;
    UINT32 pid = 0;
    INT64 token;
    if (FAILED(hr) || !controller) {
        printf("wv2host: controller failed %08lx\n", hr);
        finish(3);
        return S_OK;
    }
    g_controller = controller;
    SLOT(controller, 1, ReleaseFn)(controller);    /* AddRef: keep it */
    GetClientRect(g_window, &r);
    SLOT(controller, CTL_PUT_BOUNDS, RectFn)(controller, r);
    SLOT(controller, CTL_GET_COREWEBVIEW2, OutFn)(controller, &g_webview);
    SLOT(g_webview, WV_BROWSER_PROCESS_ID, OutFn)(g_webview, &pid);
    printf("wv2host: controller (browser process %u)\n", pid);
    SLOT(g_webview, WV_ADD_NAVIGATION_COMPLETED, AddFn)(g_webview, &g_navigation, &token);
    SLOT(g_webview, WV_ADD_PROCESS_FAILED, AddFn)(g_webview, &g_failed, &token);
    if (g_url)
        hr = SLOT(g_webview, WV_NAVIGATE, StrFn)(g_webview, g_url);
    else
        hr = SLOT(g_webview, WV_NAVIGATE_TO_STRING, StrFn)(g_webview,
            L"<!doctype html><title>NovaOS WebView2</title>"
            L"<body style='font:24px sans-serif'><h1>Hello from WebView2 on NovaOS</h1></body>");
    if (FAILED(hr)) {
        printf("wv2host: navigate failed %08lx\n", hr);
        finish(4);
    }
    return S_OK;
}
static Handler g_controller_done = { h_vtbl, 1, &IID_ControllerHandler, on_controller };

static HRESULT on_environment(HRESULT hr, void *env)
{
    if (FAILED(hr) || !env) {
        printf("wv2host: environment failed %08lx\n", hr);
        finish(2);
        return S_OK;
    }
    printf("wv2host: environment\n");
    hr = SLOT(env, ENV_CREATE_CONTROLLER, ObjOutFn)(env, g_window, &g_controller_done);
    if (FAILED(hr)) {
        printf("wv2host: CreateCoreWebView2Controller %08lx\n", hr);
        finish(3);
    }
    return S_OK;
}
static Handler g_env_done = { h_vtbl, 1, &IID_EnvHandler, on_environment };

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE && g_controller) {
        RECT r;
        GetClientRect(w, &r);
        SLOT(g_controller, CTL_PUT_BOUNDS, RectFn)(g_controller, r);
    }
    if (msg == WM_TIMER && wp == 2) {
        finish(0);
        return 0;
    }
    if (msg == WM_TIMER) {
        printf("wv2host: timed out\n");
        finish(7);
    }
    if (msg == WM_DESTROY && g_exit == 1)
        finish(8);
    return DefWindowProcW(w, msg, wp, lp);
}

int main(void)
{
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    LPCWSTR loader = argc > 1 ? argv[1] : L"WebView2Loader.dll";
    WCHAR data[MAX_PATH];
    LPWSTR version = NULL;
    MSG m;
    g_url = argc > 2 && wcscmp(argv[2], L"-") ? argv[2] : NULL;
    g_hold = argc > 3 ? _wtoi(argv[3]) : 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    HMODULE dll = LoadLibraryW(loader);
    if (!dll) {
        printf("wv2host: cannot load %ls (error %lu)\n", loader, GetLastError());
        return 9;
    }
    VersionFn get_version = (VersionFn)GetProcAddress(dll, "GetAvailableCoreWebView2BrowserVersionString");
    CreateEnvFn create = (CreateEnvFn)GetProcAddress(dll, "CreateCoreWebView2EnvironmentWithOptions");
    if (!get_version || !create) {
        printf("wv2host: %ls lacks the loader's functions\n", loader);
        return 9;
    }
    HRESULT hr = get_version(NULL, &version);
    if (FAILED(hr) || !version) {
        printf("wv2host: no runtime found (%08lx)\n", hr);
        return 10;
    }
    printf("wv2host: runtime %ls\n", version);
    CoTaskMemFree(version);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"wv2host";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, L"wv2host", L"WebView2 on NovaOS", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               40, 40, 800, 560, NULL, NULL, wc.hInstance, NULL);
    SetTimer(g_window, 1, 600 * 1000, NULL);

    /* the user data folder beside the profile's other app data, as an
     * installed program would have it */
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", data, MAX_PATH - 32))
        GetTempPathW(MAX_PATH - 32, data);
    wcscat(data, L"\\wv2host");
    hr = create(NULL, data, NULL, &g_env_done);
    if (FAILED(hr)) {
        printf("wv2host: CreateCoreWebView2EnvironmentWithOptions %08lx\n", hr);
        return 2;
    }
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_controller)
        SLOT(g_controller, 2, ReleaseFn)(g_controller);
    return g_exit;
}
