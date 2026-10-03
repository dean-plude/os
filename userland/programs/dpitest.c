/*
 * dpitest.exe — per-monitor DPI: the manifest's dpiAwareness, GetDpiForMonitor,
 *               GetDpiForWindow/System, WM_DPICHANGED with its suggested
 *               rectangle, and the coordinates DPI-aware and unaware
 *               programs see
 *
 *   dpitest              the test (the core self-tests' boot: one 2560x1600
 *                        monitor at scale 2); with a second monitor at 96
 *                        DPI it also moves its window there and back
 *   dpitest child MODE W H   (run by the test with __COMPAT_LAYER set):
 *                        MODE unaware or system, W x H the primary's
 *                        logical size; the exit code is the failures
 *
 * Its manifest makes it per-monitor aware (v2).  It sets the primary to
 * 192 DPI with NtNovaGuiCtl CTL_SET_DPI (what Settings would), checks its
 * window gets WM_DPICHANGED and twice the pixels while an unaware child
 * keeps seeing 96 DPI and logical pixels and a system-aware one sees 192
 * everywhere, then sets 96 again.
 */
#include <windows.h>
#include <winternl.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTL_SET_DPI 30

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static void checkv(long got, long want, const char *what)
{
    char b[160];
    snprintf(b, sizeof(b), "%s: %ld, expected %ld", what, got, want);
    check(got == want, b);
}

static void check_rect(RECT got, RECT want, const char *what)
{
    char b[200];
    snprintf(b, sizeof(b), "%s: (%ld, %ld)-(%ld, %ld), expected (%ld, %ld)-(%ld, %ld)", what,
             got.left, got.top, got.right, got.bottom, want.left, want.top, want.right, want.bottom);
    check(EqualRect(&got, &want), b);
}

static LONG set_dpi(int head, int dpi)
{
    INT32 in[3] = { head, dpi, 0 };
    return (LONG)NtNovaGuiCtl(0, CTL_SET_DPI, 0, in);
}

static HMONITOR primary(void)
{
    POINT o = { 0, 0 };
    return MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
}

static RECT monitor_rect(HMONITOR m)
{
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(m, &mi);
    return mi.rcMonitor;
}

static UINT monitor_dpi(HMONITOR m)
{
    UINT x = 0, y = 0;
    if (GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y) != S_OK) return 0;
    return x == y ? x : 0;
}

/* -----------------------------------------------------------------------
 * The window
 * ----------------------------------------------------------------------- */
static int  g_dpichanged;              /* WM_DPICHANGED seen */
static WPARAM g_dpi_wp;
static RECT g_dpi_rect;
static int  g_size_w, g_size_h;        /* the last WM_SIZE */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DPICHANGED) {
        const RECT *r = (const RECT *)lp;
        g_dpichanged++;
        g_dpi_wp = wp;
        g_dpi_rect = *r;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (m == WM_SIZE) { g_size_w = LOWORD(lp); g_size_h = HIWORD(lp); return 0; }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(0x20, 0x60, 0xC0));
        FillRect(dc, &r, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        char s[96];
        snprintf(s, sizeof(s), "dpitest: %u DPI, client %ld x %ld", GetDpiForWindow(h), r.right, r.bottom);
        TextOutA(dc, 16, 16, s, (int)strlen(s));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static void pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(20);
    }
}

/* Pump until WM_DPICHANGED (up to 5 s) */
static int wait_dpichanged(int before)
{
    DWORD end = GetTickCount() + 5000;
    MSG msg;
    while (g_dpichanged == before && (LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(20);
    }
    pump(200);                              /* the WM_SIZE and painting after it */
    return g_dpichanged > before;
}

static HWND make_window(int x, int y, int w, int h)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dpitest";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    HWND hw = CreateWindowExA(0, "dpitest", "dpitest", WS_OVERLAPPEDWINDOW, x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
    pump(300);
    return hw;
}

/* The window's rectangles at @k times 96 DPI: frame at (x, y) w x h; the
 * desktop's frame is a 32-pixel title bar and a 1-pixel border (k times) */
static void check_window(HWND hw, int k, int x, int y, int w, int h, const char *when)
{
    char what[96];
    RECT r, c;
    GetWindowRect(hw, &r);
    GetClientRect(hw, &c);
    snprintf(what, sizeof(what), "%s: GetWindowRect", when);
    check_rect(r, (RECT){ x, y, x + w, y + h }, what);
    snprintf(what, sizeof(what), "%s: GetClientRect", when);
    check_rect(c, (RECT){ 0, 0, w - 2 * k, h - 33 * k }, what);
    POINT p = { 0, 0 };
    ClientToScreen(hw, &p);
    snprintf(what, sizeof(what), "%s: ClientToScreen x", when);
    checkv(p.x, x + k, what);
    snprintf(what, sizeof(what), "%s: ClientToScreen y", when);
    checkv(p.y, y + 32 * k, what);
    snprintf(what, sizeof(what), "%s: GetDpiForWindow", when);
    checkv((long)GetDpiForWindow(hw), 96 * k, what);
}

/* -----------------------------------------------------------------------
 * The children: an unaware and a system-aware process, by __COMPAT_LAYER
 * ----------------------------------------------------------------------- */
static int child(const char *mode, int mw, int mh)
{
    int k = !strcmp(mode, "system") ? 2 : 1;
    char what[64];
    snprintf(what, sizeof(what), "%s child: awareness", mode);
    checkv(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()), k == 2 ? 1 : 0, what);
    snprintf(what, sizeof(what), "%s child: GetDpiForSystem", mode);
    checkv((long)GetDpiForSystem(), 96 * k, what);
    snprintf(what, sizeof(what), "%s child: GetDpiForMonitor", mode);
    checkv((long)monitor_dpi(primary()), 96 * k, what);
    snprintf(what, sizeof(what), "%s child: rcMonitor", mode);
    check_rect(monitor_rect(primary()), (RECT){ 0, 0, mw * k, mh * k }, what);
    snprintf(what, sizeof(what), "%s child: SM_CXSCREEN", mode);
    checkv(GetSystemMetrics(SM_CXSCREEN), mw * k, what);
    HDC dc = GetDC(NULL);
    snprintf(what, sizeof(what), "%s child: LOGPIXELSX", mode);
    checkv(GetDeviceCaps(dc, LOGPIXELSX), 96 * k, what);
    ReleaseDC(NULL, dc);
    /* the same logical place and size as the parent's window */
    HWND hw = make_window(100 * k, 100 * k, 400 * k, 300 * k);
    snprintf(what, sizeof(what), "%s child", mode);
    check_window(hw, k, 100 * k, 100 * k, 400 * k, 300 * k, what);
    DestroyWindow(hw);
    printf("dpitest %s child: %d passed, %d failed\n", mode, g_pass, g_fail);
    return g_fail;
}

static void run_child(const char *mode, const char *layer, int mw, int mh)
{
    char self[MAX_PATH], cl[MAX_PATH + 64];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cl, sizeof(cl), "\"%s\" child %s %d %d", self, mode, mw, mh);
    SetEnvironmentVariableA("__COMPAT_LAYER", layer);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessA(self, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("__COMPAT_LAYER", NULL);
    char what[64];
    snprintf(what, sizeof(what), "%s child started", mode);
    check(ok, what);
    if (!ok) return;
    DWORD code = 99;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    snprintf(what, sizeof(what), "%s child's failures", mode);
    checkv((long)code, 0, what);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* -----------------------------------------------------------------------
 * The test
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "child")) return child(argv[2], atoi(argv[3]), atoi(argv[4]));

    /* the manifest: per-monitor aware v2, which a call can't change */
    HANDLE ctx = GetThreadDpiAwarenessContext();
    check(AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "the manifest makes it per-monitor aware v2");
    check(IsProcessDPIAware(), "IsProcessDPIAware");
    check(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "setting the same awareness again");
    check(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) && GetLastError() == ERROR_ACCESS_DENIED,
          "the manifest's awareness can't be changed");
    PROCESS_DPI_AWARENESS pa = PROCESS_DPI_UNAWARE;
    check(GetProcessDpiAwareness(NULL, &pa) == S_OK && pa == PROCESS_PER_MONITOR_DPI_AWARE, "GetProcessDpiAwareness");
    /* a thread's context */
    HANDLE old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    check(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) == DPI_AWARENESS_UNAWARE, "SetThreadDpiAwarenessContext");
    SetThreadDpiAwarenessContext(old);
    check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "the thread's context restored");

    /* At 96 DPI (the default) aware and unaware programs see the same */
    checkv(set_dpi(0, 96), DISP_CHANGE_SUCCESSFUL, "CTL_SET_DPI 96");
    pump(200);
    HMONITOR mon = primary();
    RECT m = monitor_rect(mon);
    int mw = m.right - m.left, mh = m.bottom - m.top;
    printf("primary monitor: %d x %d logical pixels\n", mw, mh);
    checkv((long)monitor_dpi(mon), 96, "GetDpiForMonitor at 96 DPI");
    checkv((long)GetDpiForSystem(), 96, "GetDpiForSystem");
    HWND hw = make_window(100, 100, 400, 300);
    check_window(hw, 1, 100, 100, 400, 300, "at 96 DPI");
    check(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(hw), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
          "GetWindowDpiAwarenessContext");

    /* The primary at 192 DPI: twice the pixels for this process */
    int before = g_dpichanged;
    checkv(set_dpi(0, 192), DISP_CHANGE_SUCCESSFUL, "CTL_SET_DPI 192");
    check(wait_dpichanged(before), "WM_DPICHANGED when the monitor went to 192 DPI");
    checkv((long)g_dpi_wp, (long)MAKEWPARAM(192, 192), "WM_DPICHANGED wParam");
    check_rect(g_dpi_rect, (RECT){ 200, 200, 1000, 800 }, "WM_DPICHANGED suggested rectangle");
    check_window(hw, 2, 200, 200, 800, 600, "at 192 DPI");
    checkv(g_size_w, 796, "WM_SIZE width at 192 DPI");
    checkv(g_size_h, 534, "WM_SIZE height at 192 DPI");
    checkv((long)monitor_dpi(primary()), 192, "GetDpiForMonitor at 192 DPI");
    check_rect(monitor_rect(primary()), (RECT){ 0, 0, 2 * mw, 2 * mh }, "rcMonitor at 192 DPI");
    checkv(GetSystemMetrics(SM_CXSCREEN), 2 * mw, "SM_CXSCREEN at 192 DPI");
    checkv((long)GetDpiForSystem(), 96, "GetDpiForSystem stays what it was at the start");
    check(MonitorFromWindow(hw, MONITOR_DEFAULTTONULL) == primary(), "MonitorFromWindow at 192 DPI");
    RECT a = { 0, 0, 400, 300 };
    AdjustWindowRectExForDpi(&a, WS_OVERLAPPEDWINDOW, FALSE, 0, 192);
    check_rect(a, (RECT){ -2, -64, 402, 302 }, "AdjustWindowRectExForDpi at 192");
    checkv(GetSystemMetricsForDpi(SM_CXVSCROLL, 192), 2 * GetSystemMetrics(SM_CXVSCROLL), "GetSystemMetricsForDpi");

    /* other processes: unaware keeps 96 DPI and logical pixels; system
     * aware (started now) sees the primary's 192 everywhere */
    run_child("unaware", "DpiUnaware", mw, mh);
    run_child("system", "HighDpiAware", mw, mh);

    /* A second monitor at 96 DPI: moving there is another WM_DPICHANGED */
    RECT m2;
    int two = 0;
    for (int i = 1; i < 4 && !two; i++) {
        DISPLAY_DEVICEA dd;
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, (DWORD)i, &dd, 0)) break;
        POINT far = { 2 * mw + 10, 10 };
        HMONITOR mm = MonitorFromPoint(far, MONITOR_DEFAULTTONULL);
        if (mm && mm != primary()) { m2 = monitor_rect(mm); two = 1; }
    }
    if (two) {
        checkv((long)monitor_dpi(MonitorFromPoint((POINT){ m2.left, m2.top }, MONITOR_DEFAULTTONULL)), 96, "the second monitor's DPI");
        before = g_dpichanged;
        SetWindowPos(hw, NULL, m2.left + 50, m2.top + 50, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
        check(wait_dpichanged(before), "WM_DPICHANGED on the 96 DPI monitor");
        checkv((long)g_dpi_wp, (long)MAKEWPARAM(96, 96), "WM_DPICHANGED wParam there");
        checkv((long)GetDpiForWindow(hw), 96, "GetDpiForWindow there");
        before = g_dpichanged;
        SetWindowPos(hw, NULL, 200, 200, 400, 300, SWP_NOZORDER | SWP_NOACTIVATE);
        check(wait_dpichanged(before), "WM_DPICHANGED back on the 192 DPI monitor");
        checkv((long)GetDpiForWindow(hw), 192, "GetDpiForWindow back");
        SetWindowPos(hw, NULL, 200, 200, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
        pump(200);
    } else printf("one monitor: moving between monitors not tested\n");

    /* Back to 96 DPI */
    before = g_dpichanged;
    checkv(set_dpi(0, 96), DISP_CHANGE_SUCCESSFUL, "CTL_SET_DPI 96 again");
    check(wait_dpichanged(before), "WM_DPICHANGED back at 96 DPI");
    checkv((long)g_dpi_wp, (long)MAKEWPARAM(96, 96), "WM_DPICHANGED wParam at 96");
    check_rect(g_dpi_rect, (RECT){ 100, 100, 500, 400 }, "the suggested rectangle at 96");
    check_window(hw, 1, 100, 100, 400, 300, "back at 96 DPI");
    checkv((long)monitor_dpi(primary()), 96, "GetDpiForMonitor back at 96");
    DestroyWindow(hw);

    printf("dpitest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
