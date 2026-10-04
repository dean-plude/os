/* xpwin.exe — windows of different processes working together, as
 * WebView2 needs: its host parents the browser process's window in its own
 * window (SetParent), sizes, moves and shows it, sends it messages, and
 * Chromium's GPU process, a third process, draws into it (GetDC, BitBlt).
 *
 *   xpwin               the host: runs the checks
 *   xpwin child HWND    a process with a hidden top-level window, as the
 *                       browser process's; posts its handle to HWND
 *   xpwin draw HWND     paints a red square into another process's window
 *
 * A watchdog ends the program (exit code 3) if any step hangs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); fflush(stdout); } } while (0)

#define XP_HELLO  (WM_APP + 1)       /* child -> host: wParam = its window */
#define XP_REPORT (WM_APP + 2)       /* host -> child: wParam = the parent it should have, lParam = where it should be */
#define XP_RESULT (WM_APP + 3)       /* child -> host: wParam = checks passed, lParam = failed ones (bits) */
#define XP_PIXEL  (WM_APP + 4)       /* host -> child: is the drawer's red square in my window? */
#define XP_PIXOK  (WM_APP + 5)       /* child -> host: wParam = 1 yes */
#define XP_TWICE  (WM_APP + 6)       /* sent: returns wParam * 2 */

static const char *g_step = "start";

static DWORD WINAPI watchdog(void *arg)
{
    (void)arg;
    Sleep(150000);
    printf("FAIL: hung at: %s\n", g_step);
    fflush(stdout);
    ExitProcess(3);
}

/* -----------------------------------------------------------------------
 * The child: a window another process embeds
 * ----------------------------------------------------------------------- */
static HWND g_host;
static int g_size_w, g_size_h;

static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SIZE: g_size_w = LOWORD(lp); g_size_h = HIWORD(lp); return 0;
    case XP_TWICE: return (LRESULT)(wp * 2);
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        LRESULT sum = (LRESULT)cd->dwData;
        for (DWORD i = 0; i < cd->cbData; i++) sum += ((const BYTE *)cd->lpData)[i];
        return sum;
    }
    case XP_REPORT: {
        int ok = 0, bad = 0, bit = 0;
#define C(cond) do { if (cond) ok++; else bad |= 1 << bit; bit++; } while (0)
        HWND parent = (HWND)wp;
        RECT r;
        C(GetParent(h) == parent);                           /* bit 0: WS_CHILD: its parent */
        C(GetAncestor(h, GA_PARENT) == parent);              /* bit 1 */
        C(GetClientRect(h, &r) && r.right == 200 && r.bottom == 150);   /* bit 2 */
        C(g_size_w == 200 && g_size_h == 150);               /* bit 3: WM_SIZE told it */
        C(IsWindowVisible(h));                               /* bit 4 */
        C(GetWindowLongW(h, GWL_STYLE) & WS_CHILD);          /* bit 5: the host's SetWindowLong */
        C(GetWindowRect(h, &r) && r.left == (short)LOWORD(lp) && r.top == (short)HIWORD(lp));   /* bit 6: where the host moved it */
#undef C
        PostMessageW(g_host, XP_RESULT, (WPARAM)ok, (LPARAM)bad);
        return 0;
    }
    case XP_PIXEL: {
        HDC dc = GetDC(h);
        COLORREF in = GetPixel(dc, 5, 5), out = GetPixel(dc, 80, 80);
        ReleaseDC(h, dc);
        PostMessageW(g_host, XP_PIXOK, in == RGB(255, 0, 0) && out != RGB(255, 0, 0), (LPARAM)in);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int child_main(HWND host)
{
    g_host = host;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = child_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"xpwin_child";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassW(&wc);
    /* hidden, as the browser process's window is until its host shows it */
    HWND h = CreateWindowExW(0, L"xpwin_child", L"xpwin child", WS_POPUP, 600, 500, 120, 90, NULL, NULL, wc.hInstance, NULL);
    if (!h) return 2;
    if (!PostMessageW(host, XP_HELLO, (WPARAM)h, 0)) return 4;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

/* -----------------------------------------------------------------------
 * The drawer: GetDC on another process's window, as Chromium's GPU process
 * draws its software frames (SoftwareOutputDeviceWinDirect)
 * ----------------------------------------------------------------------- */
static int draw_main(HWND target)
{
    HDC dc = GetDC(target);
    if (!dc) return 2;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, 50, 50);
    HGDIOBJ old = SelectObject(mem, bm);
    RECT r = { 0, 0, 50, 50 };
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(mem, &r, red);
    BOOL ok = BitBlt(dc, 0, 0, 50, 50, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteObject(red);
    DeleteDC(mem);
    ReleaseDC(target, dc);
    return ok ? 0 : 5;
}

/* -----------------------------------------------------------------------
 * The host
 * ----------------------------------------------------------------------- */
static HWND g_child;
static int g_result_ok = -1, g_result_bad = -1, g_pix = -1;

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case XP_HELLO: g_child = (HWND)wp; return 0;
    case XP_RESULT: g_result_ok = (int)wp; g_result_bad = (int)lp; return 0;
    case XP_PIXOK: g_pix = (int)wp; if (!wp) printf("pixel: %06lx\n", (unsigned long)lp); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    } while ((LONG)(end - GetTickCount()) > 0);
}

/* pump until *flag is set (or @ms pass) */
static int wait_for(volatile int *flag, int unset, DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (*flag == unset && (LONG)(end - GetTickCount()) > 0) pump(10);
    return *flag != unset;
}

static int rect_is(HWND h, int x, int y, int w, int ht)
{
    RECT r;
    return GetWindowRect(h, &r) && r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == ht;
}

/* the child's window rectangle becomes @x, @y (relative to @parent's client area) */
static int lands(HWND parent, int x, int y, int w, int h)
{
    for (int i = 0; i < 100; i++) {
        POINT o = { 0, 0 };
        ClientToScreen(parent, &o);
        if (rect_is(g_child, o.x + x, o.y + y, w, h)) return 1;
        pump(30);
    }
    RECT r;
    POINT o = { 0, 0 };
    ClientToScreen(parent, &o);
    GetWindowRect(g_child, &r);
    printf("child at %ld,%ld %ldx%ld, wanted %ld,%ld %dx%d\n", r.left, r.top, r.right - r.left, r.bottom - r.top,
           o.x + x, o.y + y, w, h);
    return 0;
}

static int visible_becomes(BOOL want)
{
    for (int i = 0; i < 100; i++) { if (!IsWindowVisible(g_child) == !want) return 1; pump(30); }
    return 0;
}

static BOOL run(const char *args, PROCESS_INFORMATION *pi)
{
    char self[MAX_PATH], cl[MAX_PATH + 64];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cl, sizeof(cl), "\"%s\" %s", self, args);
    STARTUPINFOA si = { sizeof(si) };
    return CreateProcessA(self, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
}

static DWORD wait_exit(HANDLE p, DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (WaitForSingleObject(p, 0) == WAIT_TIMEOUT && (LONG)(end - GetTickCount()) > 0) pump(20);
    DWORD code = 99;
    GetExitCodeProcess(p, &code);
    return code;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "child")) return child_main((HWND)(ULONG_PTR)strtoull(argv[2], NULL, 16));
    if (argc >= 3 && !strcmp(argv[1], "draw")) return draw_main((HWND)(ULONG_PTR)strtoull(argv[2], NULL, 16));
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = host_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"xpwin_host";
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    RegisterClassW(&wc);
    HWND host = CreateWindowExW(0, L"xpwin_host", L"xpwin host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 520, 420,
                                NULL, NULL, wc.hInstance, NULL);
    HWND panel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 30, 400, 300, host, NULL, wc.hInstance, NULL);
    CHECK("host windows", host && panel);
    pump(200);

    g_step = "starting the child";
    char args[64];
    snprintf(args, sizeof(args), "child %llx", (unsigned long long)(ULONG_PTR)host);
    PROCESS_INFORMATION cpi;
    if (!run(args, &cpi)) { printf("FAIL: CreateProcess child\n"); return 1; }
    int hello = 0;
    for (int i = 0; i < 1000 && !g_child; i++) pump(20);
    hello = g_child != NULL;
    CHECK("cross-process PostMessage: the child's handle arrived", hello);
    if (!hello) { TerminateProcess(cpi.hProcess, 1); printf("xpwin: %d passed, %d failed\n", pass, fail); return 1; }

    g_step = "queries";
    DWORD pid = 0;
    CHECK("IsWindow", IsWindow(g_child));
    CHECK("GetWindowThreadProcessId", GetWindowThreadProcessId(g_child, &pid) && pid == cpi.dwProcessId);
    WCHAR text[64] = { 0 };
    CHECK("GetWindowTextW", GetWindowTextW(g_child, text, 64) == 11 && !wcscmp(text, L"xpwin child"));
    CHECK("GetClassNameW", GetClassNameW(g_child, text, 64) && !wcscmp(text, L"xpwin_child"));
    CHECK("SendMessage", SendMessageW(g_child, XP_TWICE, 21, 0) == 42);
    COPYDATASTRUCT cd = { 1000, 4, "\x01\x02\x03\x04" };
    CHECK("WM_COPYDATA", SendMessageW(g_child, WM_COPYDATA, (WPARAM)host, (LPARAM)&cd) == 1010);
    DWORD_PTR res = 0;
    CHECK("SendMessageTimeout", SendMessageTimeoutW(g_child, XP_TWICE, 50, 0, 0 /* SMTO_NORMAL */, 5000, &res) && res == 100);

    /* WebView2's order: place it, make it a child, parent it, show it */
    g_step = "SetWindowPos before parenting";
    CHECK("SetWindowPos (before SetParent)", SetWindowPos(g_child, NULL, 10, 20, 200, 150, SWP_NOZORDER | SWP_NOACTIVATE));
    LONG st = GetWindowLongW(g_child, GWL_STYLE);
    CHECK("GetWindowLong", (st & WS_POPUP) != 0);
    SetWindowLongW(g_child, GWL_STYLE, (st & ~WS_POPUP) | WS_CHILD);
    CHECK("SetWindowLong", (GetWindowLongW(g_child, GWL_STYLE) & (WS_CHILD | WS_POPUP)) == WS_CHILD);
    g_step = "SetParent";
    CHECK("SetParent returns the old parent", SetParent(g_child, panel) == GetDesktopWindow());
    CHECK("GetParent", GetParent(g_child) == panel);
    CHECK("GetAncestor GA_PARENT", GetAncestor(g_child, GA_PARENT) == panel);
    CHECK("GetAncestor GA_ROOT", GetAncestor(g_child, GA_ROOT) == host);
    CHECK("IsChild", IsChild(host, g_child) && IsChild(panel, g_child));
    g_step = "ShowWindow";
    ShowWindow(g_child, SW_SHOWNA);
    CHECK("shown", visible_becomes(TRUE));
    CHECK("in the parent where SetWindowPos put it", lands(panel, 10, 20, 200, 150));
    g_step = "MoveWindow";
    CHECK("MoveWindow", MoveWindow(g_child, 30, 40, 200, 150, TRUE));
    CHECK("moved within the parent", lands(panel, 30, 40, 200, 150));

    g_step = "the child's view";
    POINT po = { 30, 40 };
    ClientToScreen(panel, &po);
    PostMessageW(g_child, XP_REPORT, (WPARAM)panel, MAKELPARAM(po.x, po.y));
    wait_for(&g_result_bad, -1, 10000);
    CHECK("the child sees its parent, size and place", g_result_bad == 0);
    if (g_result_bad) printf("child checks: %d passed, failed bits %#x\n", g_result_ok, g_result_bad);

    g_step = "drawing from a third process";
    snprintf(args, sizeof(args), "draw %llx", (unsigned long long)(ULONG_PTR)g_child);
    PROCESS_INFORMATION dpi;
    DWORD code = 99;
    if (run(args, &dpi)) { code = wait_exit(dpi.hProcess, 20000); CloseHandle(dpi.hProcess); CloseHandle(dpi.hThread); }
    CHECK("GetDC + BitBlt on another process's window", code == 0);
    if (code) printf("drawer exit code %lu\n", code);
    PostMessageW(g_child, XP_PIXEL, 0, 0);
    wait_for(&g_pix, -1, 10000);
    CHECK("the drawing is in the window", g_pix == 1);

    g_step = "the host moves";
    SetWindowPos(host, NULL, 160, 120, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK("follows its host", lands(panel, 30, 40, 200, 150));
    SetWindowPos(panel, NULL, 40, 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    CHECK("follows its parent", lands(panel, 30, 40, 200, 150));
    ShowWindow(panel, SW_HIDE);
    CHECK("hidden with its parent", visible_becomes(FALSE));
    ShowWindow(panel, SW_SHOW);
    CHECK("shown with its parent", visible_becomes(TRUE));

    g_step = "SetParent(NULL)";
    CHECK("SetParent(NULL) returns the parent", SetParent(g_child, NULL) == panel);
    CHECK("a top-level window again", GetAncestor(g_child, GA_PARENT) == GetDesktopWindow() && !IsChild(host, g_child));

    g_step = "closing the child";
    PostMessageW(g_child, WM_CLOSE, 0, 0);
    code = wait_exit(cpi.hProcess, 20000);
    CHECK("the child ended", code == 0);
    CHECK("its window is gone", !IsWindow(g_child));
    CHECK("SendMessage to a gone window returns", SendMessageW(g_child, XP_TWICE, 1, 0) == 0);
    CloseHandle(cpi.hProcess);
    CloseHandle(cpi.hThread);
    DestroyWindow(host);
    printf("xpwin: %d passed, %d failed (%d-bit)\n", pass, fail, (int)sizeof(void *) * 8);
    return fail ? 1 : 0;
}
