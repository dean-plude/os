/*
 * warptest.exe — SetCursorPos and ClipCursor
 *
 * What games do with the pointer (SDL's relative mouse mode by warping
 * recentres it every frame; games keep it inside their window):
 *  - with its window in front, SetCursorPos moves the pointer: GetCursorPos
 *    reads the new place back and the window gets WM_MOUSEMOVE there, but
 *    no WM_INPUT (Raw Input is the mouse's own motion);
 *  - the pointer stays on the desktop (a place off it ends at its edge);
 *  - ClipCursor keeps it in a rectangle: GetClipCursor returns it, the
 *    pointer moves inside at once, SetCursorPos stops at its edges, and so
 *    does the mouse when the test harness moves it 300 left and up (Raw
 *    Input still reports the whole motion); ClipCursor(NULL) lets it go;
 *  - a process whose window is not in front cannot move or confine the
 *    pointer (ERROR_ACCESS_DENIED): a child copy tries; when its own
 *    window comes to the front, the parent's confinement ends and the
 *    child moves the pointer.
 * "warptest" runs as a 64-bit program and C:\Programs\x86\warptest.exe as
 * a 32-bit one.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static int g_pass, g_fail;

static void check(int ok, const char *what, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, what);
    vsnprintf(buf, sizeof(buf), what, ap);
    va_end(ap);
    printf("%s %s\n", ok ? "ok  " : "FAIL", buf);
    fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

static int  g_moves;                        /* WM_MOUSEMOVE */
static POINT g_last;                        /* ... its client point */
static int  g_inputs;                       /* WM_INPUT for the mouse */
static LONG g_raw_dx, g_raw_dy;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_MOUSEMOVE) {
        g_moves++;
        g_last.x = (short)LOWORD(lp);
        g_last.y = (short)HIWORD(lp);
        return 0;
    }
    if (m == WM_INPUT) {
        RAWINPUT ri;
        UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            g_inputs++;
            g_raw_dx += ri.data.mouse.lLastX;
            g_raw_dy += ri.data.mouse.lLastY;
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

#define PUMP_UNTIL(cond, ms) do { DWORD t0_ = GetTickCount(); while (!(cond) && GetTickCount() - t0_ < (ms)) { pump(); Sleep(10); } pump(); } while (0)

static HWND make_window(const wchar_t *title, int x, int y)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"warptest";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"warptest", title, WS_OVERLAPPEDWINDOW, x, y, 600, 400, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    PUMP_UNTIL(GetForegroundWindow() == w, 5000);
    return w;
}

static int at(int x, int y)
{
    POINT p;
    return GetCursorPos(&p) && p.x == x && p.y == y;
}

/* The child: in the background first, then in front.  Exit code: the
 * checks that failed, one bit each */
static int child(void)
{
    int bad = 0;
    SetLastError(0);
    if (SetCursorPos(10, 10) || GetLastError() != ERROR_ACCESS_DENIED) bad |= 1;
    RECT r = { 0, 0, 50, 50 };
    SetLastError(0);
    if (ClipCursor(&r) || GetLastError() != ERROR_ACCESS_DENIED) bad |= 2;
    if (at(10, 10)) bad |= 4;
    HWND w = make_window(L"warptest child", 150, 150);
    if (GetForegroundWindow() != w) bad |= 8;
    for (DWORD t0 = GetTickCount(); !SetCursorPos(20, 20) && GetTickCount() - t0 < 3000; ) Sleep(10);   /* the desktop activates it */
    RECT c, d;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetRect(&d, vx, vy, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vy + GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (!GetClipCursor(&c) || !EqualRect(&c, &d)) bad |= 16;
    if (!SetCursorPos(10, 10) || !at(10, 10)) bad |= 32;
    DestroyWindow(w);
    return bad;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "child")) return child();

    HWND w = make_window(L"SetCursorPos and ClipCursor", 100, 100);
    check(GetForegroundWindow() == w, "the window is in front");
    RAWINPUTDEVICE rid = { 1, 2, 0, w };
    check(RegisterRawInputDevices(&rid, 1, sizeof(rid)), "Raw Input for the mouse");
    pump();

    /* ---- SetCursorPos ---- */
    RECT wr;
    GetWindowRect(w, &wr);
    int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
    g_moves = g_inputs = 0;
    check(SetCursorPos(cx, cy), "SetCursorPos(%d, %d)", cx, cy);
    check(at(cx, cy), "GetCursorPos reads it back");
    POINT want = { cx, cy };
    ScreenToClient(w, &want);
    PUMP_UNTIL(g_moves && g_last.x == want.x && g_last.y == want.y, 3000);
    check(g_moves && g_last.x == want.x && g_last.y == want.y, "WM_MOUSEMOVE at the client point %ld, %ld (%d moves, last %ld, %ld)",
          (long)want.x, (long)want.y, g_moves, (long)g_last.x, (long)g_last.y);
    PUMP_UNTIL(0, 300);
    check(g_inputs == 0, "no WM_INPUT for it (%d)", g_inputs);
    check(SetPhysicalCursorPos(cx + 10, cy) && at(cx + 10, cy), "SetPhysicalCursorPos");

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vr = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    check(SetCursorPos(-5000, -5000) && at(vx, vy), "off the desktop: its top-left corner");
    check(SetCursorPos(50000, 50000) && at(vr - 1, vb - 1), "... its bottom-right corner");

    /* ---- ClipCursor ---- */
    RECT d, c, r = { 200, 150, 500, 350 };
    SetRect(&d, vx, vy, vr, vb);
    check(GetClipCursor(&c) && EqualRect(&c, &d), "GetClipCursor: the whole desktop (%ld, %ld, %ld, %ld)",
          (long)c.left, (long)c.top, (long)c.right, (long)c.bottom);
    check(ClipCursor(&r), "ClipCursor(200, 150, 500, 350)");
    check(GetClipCursor(&c) && EqualRect(&c, &r), "GetClipCursor returns it");
    check(at(499, 349), "the pointer moved inside");
    check(SetCursorPos(10, 10) && at(200, 150), "SetCursorPos stops at its top-left");
    check(SetCursorPos(900, 900) && at(499, 349), "... and its bottom-right");
    check(SetCursorPos(300, 200) && at(300, 200), "... and goes anywhere inside");

    g_inputs = 0;
    g_raw_dx = g_raw_dy = 0;
    printf("warptest: move the mouse 300 left and up\n");
    PUMP_UNTIL(g_raw_dx <= -300 && g_raw_dy <= -300, 30000);
    PUMP_UNTIL(0, 500);
    check(g_raw_dx == -300 && g_raw_dy == -300, "Raw Input saw all of it: %ld, %ld", (long)g_raw_dx, (long)g_raw_dy);
    check(at(200, 150), "the mouse stopped at the rectangle's corner");

    check(ClipCursor(NULL), "ClipCursor(NULL)");
    check(GetClipCursor(&c) && EqualRect(&c, &d), "GetClipCursor: the whole desktop again");
    check(SetCursorPos(10, 10) && at(10, 10), "the pointer goes anywhere");

    /* ---- the background ---- */
    check(ClipCursor(&r), "confined again");
    char me[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, me, sizeof(me));
    snprintf(cmd, sizeof(cmd), "\"%s\" child", me);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = ~0u;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DWORD t0 = GetTickCount();
        while (WaitForSingleObject(pi.hProcess, 10) == WAIT_TIMEOUT && GetTickCount() - t0 < 30000) pump();
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    check(!(code & 3), "a process in the background: SetCursorPos and ClipCursor refused (%lx)", (unsigned long)code);
    check(!(code & 4), "... the pointer did not move");
    check(!(code & 8), "the child's window came to the front");
    check(!(code & 16), "... which ended the parent's confinement");
    check(!(code & 32), "... and the child moved the pointer");
    check(GetClipCursor(&c) && EqualRect(&c, &d), "not confined any more");

    DestroyWindow(w);
    printf("warptest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
