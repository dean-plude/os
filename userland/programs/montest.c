/*
 * montest.exe — more than one monitor (EnumDisplayMonitors, GetMonitorInfo,
 *               MonitorFromPoint/Rect/Window, EnumDisplayDevices,
 *               EnumDisplaySettings and ChangeDisplaySettingsEx per display)
 *
 *   montest N       run the tests on a desktop of N monitors (the graphics
 *                   self-tests boot with two: QEMU's std VGA and a
 *                   secondary-vga), leaving the layout as it found it
 *   montest list    print the monitors and display devices
 *
 * With two monitors it also moves the pointer across: it prints "move the
 * pointer right" and the test pushes the mouse to the right edge
 * (tools/selftest.py), and checks the pointer went onto the second
 * monitor and stopped at the desktop's right edge.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

#define MAX_MON 8
static HMONITOR g_mon[MAX_MON];
static RECT     g_rect[MAX_MON];
static int      g_n;

static BOOL CALLBACK collect(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)lp;
    if (g_n < MAX_MON) { g_mon[g_n] = m; g_rect[g_n] = *r; g_n++; }
    return TRUE;
}

static int enum_monitors(void)
{
    g_n = 0;
    EnumDisplayMonitors(NULL, NULL, collect, 0);
    return g_n;
}

static MONITORINFOEXA info_of(HMONITOR m)
{
    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(m, (MONITORINFO *)&mi)) mi.cbSize = 0;
    return mi;
}

static int same_rect(RECT a, RECT b) { return EqualRect(&a, &b); }

static void print_rect(const char *what, RECT r)
{
    printf("  %s (%ld, %ld)-(%ld, %ld), %ld x %ld\n", what, r.left, r.top, r.right, r.bottom,
           r.right - r.left, r.bottom - r.top);
}

static int list(void)
{
    int n = enum_monitors();
    printf("%d monitor(s)\n", n);
    for (int i = 0; i < n; i++) {
        MONITORINFOEXA mi = info_of(g_mon[i]);
        printf("%s%s\n", mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
        print_rect("monitor", mi.rcMonitor);
        print_rect("work area", mi.rcWork);
    }
    DISPLAY_DEVICEA dd;
    for (DWORD i = 0;; i++) {
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        printf("adapter %s: %s, flags %lx\n", dd.DeviceName, dd.DeviceString, dd.StateFlags);
    }
    return 0;
}

static DEVMODEA current(const char *dev, BOOL *ok)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    *ok = EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &dm);
    return dm;
}

/* Display @dev: @w x @h (0: keep) and/or its place on the virtual screen */
static LONG change(const char *dev, DWORD w, DWORD h, BOOL pos, LONG x, LONG y, DWORD flags)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (w) { dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT; dm.dmPelsWidth = w; dm.dmPelsHeight = h; }
    if (pos) { dm.dmFields |= DM_POSITION; dm.dmPosition.x = x; dm.dmPosition.y = y; }
    return ChangeDisplaySettingsExA(dev, &dm, NULL, flags, NULL);
}

static DWORD saved(const char *key, const char *what)
{
    HKEY k;
    DWORD v = 0xFFFFFFFF, n = sizeof(v), type = 0;
    char path[160];
    snprintf(path, sizeof(path), "SYSTEM\\CurrentControlSet\\Control\\Video\\{NovaOS-Display}\\%s", key);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k)) return v;
    if (RegQueryValueExA(k, what, NULL, &type, (BYTE *)&v, &n) || type != REG_DWORD) v = 0xFFFFFFFF;
    RegCloseKey(k);
    return v;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
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
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXA mi = info_of(mon);
        char s[96];
        snprintf(s, sizeof(s), "montest: this window is on %s", mi.szDevice);
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

static void test_one(void)
{
    check(GetSystemMetrics(SM_CMONITORS) == 1, "SM_CMONITORS is 1");
    check(enum_monitors() == 1, "EnumDisplayMonitors finds one monitor");
    MONITORINFOEXA mi = info_of(g_mon[0]);
    check(mi.rcMonitor.left == 0 && mi.rcMonitor.top == 0 && (mi.dwFlags & MONITORINFOF_PRIMARY), "it is the primary at (0, 0)");
    check(GetSystemMetrics(SM_CXVIRTUALSCREEN) == GetSystemMetrics(SM_CXSCREEN), "the virtual screen is the screen");
}

static void test_two(void)
{
    char s[160];
    int n = enum_monitors();
    snprintf(s, sizeof(s), "EnumDisplayMonitors finds 2 monitors (found %d)", n);
    check(n == 2, s);
    check(GetSystemMetrics(SM_CMONITORS) == 2, "SM_CMONITORS is 2");
    if (n != 2) return;

    MONITORINFOEXA m1 = info_of(g_mon[0]), m2 = info_of(g_mon[1]);
    check(m1.cbSize && m2.cbSize, "GetMonitorInfo answers for both");
    check(!strcmp(m1.szDevice, "\\\\.\\DISPLAY1") && !strcmp(m2.szDevice, "\\\\.\\DISPLAY2"), "they are \\\\.\\DISPLAY1 and 2");
    check((m1.dwFlags & MONITORINFOF_PRIMARY) && !(m2.dwFlags & MONITORINFOF_PRIMARY), "the first is the primary");
    check(m1.rcMonitor.left == 0 && m1.rcMonitor.top == 0, "the primary is at (0, 0)");
    check(same_rect(m1.rcMonitor, g_rect[0]) && same_rect(m2.rcMonitor, g_rect[1]), "EnumDisplayMonitors passes the monitor rectangles");
    RECT x;
    check(!IntersectRect(&x, &m1.rcMonitor, &m2.rcMonitor), "the monitors do not overlap");
    check(m2.rcMonitor.left == m1.rcMonitor.right && m2.rcMonitor.top == 0, "the second is to the right of the primary");
    check(m1.rcWork.bottom < m1.rcMonitor.bottom, "the primary's work area leaves out the dock");
    check(same_rect(m2.rcWork, m2.rcMonitor), "the second monitor's work area is all of it");
    print_rect("display 1", m1.rcMonitor);
    print_rect("display 2", m2.rcMonitor);

    RECT v;
    UnionRect(&v, &m1.rcMonitor, &m2.rcMonitor);
    check(GetSystemMetrics(SM_XVIRTUALSCREEN) == v.left && GetSystemMetrics(SM_YVIRTUALSCREEN) == v.top &&
          GetSystemMetrics(SM_CXVIRTUALSCREEN) == v.right - v.left && GetSystemMetrics(SM_CYVIRTUALSCREEN) == v.bottom - v.top,
          "SM_*VIRTUALSCREEN is the union of the monitors");
    check(GetSystemMetrics(SM_CXSCREEN) == m1.rcMonitor.right, "SM_CXSCREEN is the primary's width");

    /* Monitor from a point / rectangle */
    POINT p1 = { 10, 10 }, p2 = { m2.rcMonitor.left + 10, 10 }, out = { v.right + 500, 10 };
    check(MonitorFromPoint(p1, MONITOR_DEFAULTTONULL) == g_mon[0], "MonitorFromPoint on the primary");
    check(MonitorFromPoint(p2, MONITOR_DEFAULTTONULL) == g_mon[1], "MonitorFromPoint on the second");
    check(MonitorFromPoint(out, MONITOR_DEFAULTTONULL) == NULL, "MonitorFromPoint off every monitor: NULL");
    check(MonitorFromPoint(out, MONITOR_DEFAULTTONEAREST) == g_mon[1], "... the nearest is the second");
    check(MonitorFromPoint(out, MONITOR_DEFAULTTOPRIMARY) == g_mon[0], "... or the primary");
    RECT across = { m2.rcMonitor.left - 100, 100, m2.rcMonitor.left + 300, 200 };
    check(MonitorFromRect(&across, MONITOR_DEFAULTTONULL) == g_mon[1], "MonitorFromRect: the one it overlaps most");

    /* Display devices and their modes */
    DISPLAY_DEVICEA dd;
    int adapters = 0, primary = 0;
    for (DWORD i = 0; i < 8; i++) {
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        adapters++;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) primary++;
        check((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0, "each adapter is attached to the desktop");
    }
    check(adapters == 2 && primary == 1, "EnumDisplayDevices: two adapters, one primary");
    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);
    check(EnumDisplayDevicesA("\\\\.\\DISPLAY2", 0, &dd, 0) && !strncmp(dd.DeviceName, "\\\\.\\DISPLAY2\\Monitor", 20),
          "EnumDisplayDevices names the monitor on \\\\.\\DISPLAY2");

    BOOL ok;
    DEVMODEA d2 = current("\\\\.\\DISPLAY2", &ok);
    check(ok && d2.dmPelsWidth > 0,
          "EnumDisplaySettings answers for \\\\.\\DISPLAY2");
    check((d2.dmFields & DM_POSITION) && d2.dmPosition.x == m2.rcMonitor.left && d2.dmPosition.y == m2.rcMonitor.top,
          "its dmPosition is the monitor's place");
    DEVMODEA mode;
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    check(EnumDisplaySettingsA("\\\\.\\DISPLAY2", 0, &mode) && mode.dmPelsWidth >= d2.dmPelsWidth, "and lists its modes");
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    check(!EnumDisplaySettingsA("\\\\.\\DISPLAY9", ENUM_CURRENT_SETTINGS, &mode), "no \\\\.\\DISPLAY9");

    /* A window on the second monitor; maximized, it fills that monitor */
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "montest";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    HWND h = CreateWindowA("montest", "On the second monitor", WS_OVERLAPPEDWINDOW,
                           m2.rcMonitor.left + 120, 140, 520, 300, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    pump(300);
    RECT wr;
    GetWindowRect(h, &wr);
    snprintf(s, sizeof(s), "the window was created on the second monitor (at %ld, %ld)", wr.left, wr.top);
    check(wr.left >= m2.rcMonitor.left && wr.right <= m2.rcMonitor.right, s);
    check(MonitorFromWindow(h, MONITOR_DEFAULTTONULL) == g_mon[1], "MonitorFromWindow: the second");
    ShowWindow(h, SW_MAXIMIZE);
    pump(300);
    GetWindowRect(h, &wr);
    snprintf(s, sizeof(s), "maximized, it fills the second monitor (%ld, %ld)-(%ld, %ld)", wr.left, wr.top, wr.right, wr.bottom);
    check(wr.left >= m2.rcMonitor.left - 8 && wr.right <= m2.rcMonitor.right + 8 &&
          wr.right - wr.left >= m2.rcMonitor.right - m2.rcMonitor.left - 16, s);
    ShowWindow(h, SW_RESTORE);
    pump(300);
    SetWindowPos(h, NULL, 200, 150, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    pump(200);
    check(MonitorFromWindow(h, MONITOR_DEFAULTTONULL) == g_mon[0], "moved back, it is on the primary");
    SetWindowPos(h, NULL, m2.rcMonitor.left + 120, 140, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    pump(200);
    check(MonitorFromWindow(h, MONITOR_DEFAULTTONULL) == g_mon[1], "and again on the second");

    /* The pointer crosses from the primary onto the second monitor and
     * stops at the right edge of the desktop */
    printf("montest: move the pointer right\n");
    fflush(stdout);
    POINT c = { 0, 0 };
    DWORD end = GetTickCount() + 30000;
    while ((LONG)(end - GetTickCount()) > 0) {
        GetCursorPos(&c);
        if (c.x >= v.right - 1) break;
        pump(100);
    }
    snprintf(s, sizeof(s), "the pointer crossed onto the second monitor and stopped at its right edge (at %ld, %ld)", c.x, c.y);
    check(c.x == v.right - 1, s);
    printf("montest: window on display 2\n");
    fflush(stdout);
    pump(4000);                                             /* (the test's screenshots of both heads) */

    /* Rearrange: the second monitor in 1024x768, left of the primary;
     * saved where Windows keeps it */
    LONG r = change("\\\\.\\DISPLAY2", 1024, 768, TRUE, -1024, 0, CDS_UPDATEREGISTRY);
    snprintf(s, sizeof(s), "ChangeDisplaySettingsEx: \\\\.\\DISPLAY2 1024x768 at (-1024, 0) (returned %ld)", r);
    check(r == DISP_CHANGE_SUCCESSFUL, s);
    pump(300);
    enum_monitors();
    MONITORINFOEXA n2 = info_of(g_mon[1]);
    snprintf(s, sizeof(s), "the second monitor is now (%ld, %ld)-(%ld, %ld)", n2.rcMonitor.left, n2.rcMonitor.top,
             n2.rcMonitor.right, n2.rcMonitor.bottom);
    check(n2.rcMonitor.left == -1024 && n2.rcMonitor.top == 0 && n2.rcMonitor.right == 0 && n2.rcMonitor.bottom == 768, s);
    check(GetSystemMetrics(SM_XVIRTUALSCREEN) == -1024, "the virtual screen starts left of the primary");
    POINT left = { -10, 10 };
    check(MonitorFromPoint(left, MONITOR_DEFAULTTONULL) == g_mon[1], "a point left of the primary is on the second");
    check(MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != NULL, "the window is still on a monitor");
    check(saved("0001", "DefaultSettings.XResolution") == 1024 && saved("0001", "DefaultSettings.YResolution") == 768,
          "the mode is saved under ...\\Video\\{NovaOS-Display}\\0001");
    check((LONG)saved("0001", "Attach.RelativeX") == -1024 && saved("0001", "Attach.RelativeY") == 0,
          "and its place (Attach.RelativeX/Y)");

    /* Overlapping the primary is refused: it goes next to the others */
    change("\\\\.\\DISPLAY2", 0, 0, TRUE, 200, 100, 0);
    pump(200);
    enum_monitors();
    n2 = info_of(g_mon[1]);
    check(!IntersectRect(&x, &n2.rcMonitor, &m1.rcMonitor), "a place overlapping the primary is not taken");

    /* Back as it was */
    r = change("\\\\.\\DISPLAY2", d2.dmPelsWidth, d2.dmPelsHeight, TRUE, m2.rcMonitor.left, m2.rcMonitor.top, CDS_UPDATEREGISTRY);
    check(r == DISP_CHANGE_SUCCESSFUL, "and back to how it was");
    enum_monitors();
    n2 = info_of(g_mon[1]);
    check(same_rect(n2.rcMonitor, m2.rcMonitor), "the second monitor is where it was");
    DestroyWindow(h);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "list")) return list();
    int want = argc > 1 ? atoi(argv[1]) : 0;
    if (!want) want = GetSystemMetrics(SM_CMONITORS);
    if (want == 1) test_one();
    else test_two();
    printf("montest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
