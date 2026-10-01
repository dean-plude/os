/*
 * disptest.exe — display modes (EnumDisplaySettings / ChangeDisplaySettings)
 *
 *   disptest            run the tests (returns to the starting mode)
 *   disptest W H        switch to W x H and stay there (CDS_UPDATEREGISTRY)
 *   disptest list       print the modes
 *   disptest fs W H     (child) full-screen W x H, then exit: the mode
 *                       must come back by itself
 */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static DEVMODEW mode_of(DWORD i, BOOL *ok)
{
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    *ok = EnumDisplaySettingsW(NULL, i, &dm);
    return dm;
}

static LONG set_mode(DWORD w, DWORD h, DWORD flags)
{
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = 32;
    return ChangeDisplaySettingsW(&dm, flags);
}

static int list(void)
{
    BOOL ok;
    for (DWORD i = 0;; i++) {
        DEVMODEW dm = mode_of(i, &ok);
        if (!ok) break;
        printf("  %lu x %lu, %lu bpp, %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
    }
    DEVMODEW cur = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    printf("current %lu x %lu; desktop %d x %d\n", cur.dmPelsWidth, cur.dmPelsHeight,
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    return 0;
}

static LPARAM g_change;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DISPLAYCHANGE) g_change = lp ? lp : 1;
    return DefWindowProcW(h, m, wp, lp);
}

/* Pump messages until WM_DISPLAYCHANGE (or a timeout) */
static LPARAM wait_change(void)
{
    g_change = 0;
    DWORD end = GetTickCount() + 3000;
    while (!g_change && GetTickCount() < end) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }
    return g_change;
}

/* Desktop size for a device mode: the desktop is scaled 2x when the mode
 * is at least 2560 x 1600 */
static int logical(int w, int h, int *lh)
{
    int s = (w / 1280 < h / 800) ? w / 1280 : h / 800;
    if (s < 1) s = 1;
    *lh = h / s;
    return w / s;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "list")) return list();
    if (argc == 3) {
        LONG r = set_mode(atoi(argv[1]), atoi(argv[2]), CDS_UPDATEREGISTRY);
        printf("ChangeDisplaySettings: %ld\n", r);
        return list() + (r != DISP_CHANGE_SUCCESSFUL);
    }
    if (argc == 4 && !strcmp(argv[1], "fs")) {
        LONG r = set_mode(atoi(argv[2]), atoi(argv[3]), CDS_FULLSCREEN);
        printf("child: full screen %s x %s: %ld (desktop %d x %d)\n", argv[2], argv[3], r,
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        return r != DISP_CHANGE_SUCCESSFUL;
    }

    check(offsetof(DEVMODEW, dmBitsPerPel) == 168 && offsetof(DEVMODEA, dmBitsPerPel) == 104 &&
          sizeof(DEVMODEW) == 220 && sizeof(DEVMODEA) == 156, "DEVMODE layout");

    /* The mode list: largest first, 32 bpp, the current mode among them */
    BOOL ok;
    DEVMODEW start = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    check(ok && start.dmPelsWidth && start.dmPelsHeight, "ENUM_CURRENT_SETTINGS");
    DEVMODEW reg = mode_of(ENUM_REGISTRY_SETTINGS, &ok);
    check(ok && reg.dmPelsWidth == start.dmPelsWidth && reg.dmPelsHeight == start.dmPelsHeight, "ENUM_REGISTRY_SETTINGS");
    int n = 0, has_cur = 0, has_1024 = 0, has_800 = 0, sorted = 1;
    DWORD last_area = 0xFFFFFFFF;
    for (DWORD i = 0; i < 64; i++) {
        DEVMODEW dm = mode_of(i, &ok);
        if (!ok) break;
        n++;
        DWORD area = dm.dmPelsWidth * dm.dmPelsHeight;
        if (area > last_area) sorted = 0;
        last_area = area;
        check(dm.dmBitsPerPel == 32, "32 bits per pixel");
        if (dm.dmPelsWidth == start.dmPelsWidth && dm.dmPelsHeight == start.dmPelsHeight) has_cur = 1;
        if (dm.dmPelsWidth == 1024 && dm.dmPelsHeight == 768) has_1024 = 1;
        if (dm.dmPelsWidth == 800 && dm.dmPelsHeight == 600) has_800 = 1;
    }
    printf("%d modes; current %lu x %lu\n", n, start.dmPelsWidth, start.dmPelsHeight);
    check(n >= 1 && has_cur, "the current mode is listed");
    check(sorted, "modes largest first");
    DEVMODEA dma;
    memset(&dma, 0, sizeof(dma));
    dma.dmSize = sizeof(dma);
    check(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dma) && dma.dmPelsWidth == start.dmPelsWidth &&
          dma.dmPelsHeight == start.dmPelsHeight && dma.dmBitsPerPel == 32, "EnumDisplaySettingsA");

    if (n < 2 || !has_1024 || !has_800) {
        printf("only the boot mode (no mode-setting display adapter): skipping mode changes\n");
        printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail != 0;
    }

    /* Validation without a change */
    check(set_mode(1024, 768, CDS_TEST) == DISP_CHANGE_SUCCESSFUL, "CDS_TEST 1024x768");
    check(set_mode(1234, 567, CDS_TEST) == DISP_CHANGE_BADMODE, "CDS_TEST 1234x567 refused");
    DEVMODEW bad;
    memset(&bad, 0, sizeof(bad));
    bad.dmSize = sizeof(bad);
    bad.dmFields = DM_BITSPERPEL; bad.dmBitsPerPel = 16;
    check(ChangeDisplaySettingsW(&bad, CDS_TEST) == DISP_CHANGE_BADMODE, "16 bpp refused");
    DEVMODEW cur = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    check(cur.dmPelsWidth == start.dmPelsWidth, "CDS_TEST changes nothing");

    /* A real change: the desktop resizes and windows hear about it */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"disptest";
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(0, L"disptest", L"disptest", WS_OVERLAPPEDWINDOW, 40, 40, 300, 200,
                              NULL, NULL, wc.hInstance, NULL);
    check(hw != NULL, "CreateWindow");
    ShowWindow(hw, SW_SHOW);

    int lh, lw = logical(1024, 768, &lh);
    check(set_mode(1024, 768, 0) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettings 1024x768");
    LPARAM lp = wait_change();
    check(LOWORD(lp) == 1024 && HIWORD(lp) == 768, "WM_DISPLAYCHANGE 1024x768");
    cur = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    check(cur.dmPelsWidth == 1024 && cur.dmPelsHeight == 768, "current mode 1024x768");
    check(GetSystemMetrics(SM_CXSCREEN) == lw && GetSystemMetrics(SM_CYSCREEN) == lh, "desktop size follows");
    reg = mode_of(ENUM_REGISTRY_SETTINGS, &ok);
    check(reg.dmPelsWidth == start.dmPelsWidth, "a dynamic change leaves the registry mode");
    printf("1024x768: desktop %d x %d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    Sleep(500);

    /* Only some fields: the rest keep their values */
    DEVMODEA wonly;
    memset(&wonly, 0, sizeof(wonly));
    wonly.dmSize = sizeof(wonly);
    wonly.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    wonly.dmPelsWidth = 800; wonly.dmPelsHeight = 600;
    check(ChangeDisplaySettingsA(&wonly, 0) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsA 800x600");
    lp = wait_change();
    check(LOWORD(lp) == 800 && HIWORD(lp) == 600, "WM_DISPLAYCHANGE 800x600");
    check(GetSystemMetrics(SM_CXSCREEN) == 800 && GetSystemMetrics(SM_CYSCREEN) == 600, "desktop 800x600");

    /* NULL: back to the registry mode */
    check(ChangeDisplaySettingsW(NULL, 0) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettings(NULL)");
    wait_change();
    cur = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    check(cur.dmPelsWidth == start.dmPelsWidth && cur.dmPelsHeight == start.dmPelsHeight, "back to the start mode");

    /* CDS_FULLSCREEN lasts as long as the program that asked */
    char self[MAX_PATH], cl[MAX_PATH + 32];
    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(cl, sizeof(cl), "\"%s\" fs 1024 768", self);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    DWORD code = 1;
    if (CreateProcessA(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 20000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    check(code == 0, "child's CDS_FULLSCREEN");
    DWORD end = GetTickCount() + 5000;
    do {
        Sleep(100);
        cur = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    } while (cur.dmPelsWidth != start.dmPelsWidth && GetTickCount() < end);
    check(cur.dmPelsWidth == start.dmPelsWidth && cur.dmPelsHeight == start.dmPelsHeight,
          "the full-screen mode ends with its program");

    DestroyWindow(hw);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
