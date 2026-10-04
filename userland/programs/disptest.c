/*
 * disptest.exe — display modes (EnumDisplaySettings / ChangeDisplaySettings)
 *
 *   disptest            run the tests (returns to the starting mode)
 *   disptest W H        switch to W x H and stay there (CDS_UPDATEREGISTRY:
 *                       also after a restart)
 *   disptest saved W H  check the mode is W x H, as saved (after a restart)
 *   disptest list       print the modes
 *   disptest fs W H     (child) full-screen W x H, a frame through the DC
 *                       its window kept from before, then exit: the mode
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

/* The mode saved for the next boot, where Windows keeps it */
static DWORD saved(const char *what)
{
    HKEY k;
    DWORD v = 0, n = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Video\\{NovaOS-Display}\\0000",
                      0, KEY_READ, &k))
        return 0;
    char name[64];
    snprintf(name, sizeof(name), "DefaultSettings.%s", what);
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &n) || type != REG_DWORD) v = 0;
    RegCloseKey(k);
    return v;
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

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
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
    if (argc == 4 && !strcmp(argv[1], "saved")) {
        BOOL ok;
        DEVMODEW cur = mode_of(ENUM_CURRENT_SETTINGS, &ok), reg = mode_of(ENUM_REGISTRY_SETTINGS, &ok);
        DWORD w = atoi(argv[2]), h = atoi(argv[3]);
        printf("current %lu x %lu, registry mode %lu x %lu, saved %lu x %lu\n", cur.dmPelsWidth, cur.dmPelsHeight,
               reg.dmPelsWidth, reg.dmPelsHeight, saved("XResolution"), saved("YResolution"));
        check(cur.dmPelsWidth == w && cur.dmPelsHeight == h, "booted in the saved mode");
        check(reg.dmPelsWidth == w && reg.dmPelsHeight == h, "ENUM_REGISTRY_SETTINGS is the saved mode");
        check(saved("XResolution") == w && saved("YResolution") == h, "DefaultSettings in the registry");
        printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail != 0;
    }
    if (argc == 4 && !strcmp(argv[1], "fs")) {
        /* as SDL goes full screen: the window and the DC it keeps come
         * first, then the mode, then the window is shown over the screen
         * and frames are blitted through that DC */
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"disptest fs";
        wc.style = CS_OWNDC;
        RegisterClassW(&wc);
        int w = atoi(argv[2]), h = atoi(argv[3]);
        HWND hw = CreateWindowExW(0, wc.lpszClassName, L"disptest fs", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
        HDC kept = hw ? GetDC(hw) : NULL;
        LONG r = set_mode(w, h, CDS_FULLSCREEN);
        printf("child: full screen %s x %s: %ld (desktop %d x %d)\n", argv[2], argv[3], r,
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        int lh, lw = logical(w, h, &lh);
        SetWindowPos(hw, HWND_TOP, 0, 0, lw, lh, SWP_SHOWWINDOW);
        pump();
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = lw; bi.bmiHeader.biHeight = -lh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        DWORD *px = NULL;
        HBITMAP dib = CreateDIBSection(kept, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        HDC mem = CreateCompatibleDC(kept);
        SelectObject(mem, dib);
        for (int i = 0; px && i < lw * lh; i++) px[i] = 0x00C02040;
        BOOL blit = kept && px && BitBlt(kept, 0, 0, lw, lh, mem, 0, 0, SRCCOPY);
        COLORREF c = kept ? GetPixel(kept, lw - 1, lh - 1) : (COLORREF)0xFFFFFFFF;
        printf("child: kept DC %s, corner %06lx\n", blit ? "blitted" : "not blitted", (unsigned long)c);
        DeleteDC(mem);
        DeleteObject(dib);
        return r != DISP_CHANGE_SUCCESSFUL || !blit || c != RGB(0xC0, 0x20, 0x40);
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
    /* A window wider and taller than 800x600 leaves room for: the small
     * mode shrinks it, the start mode gives it back its size */
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int bw = sw - 100 < 1000 ? sw - 100 : 1000, bh = sh - 160 < 680 ? sh - 160 : 680;
    HWND big = NULL;
    RECT big0 = { 0 };
    if (bw > 820 && bh > 620) {
        big = CreateWindowExW(0, L"disptest", L"disptest big", WS_OVERLAPPEDWINDOW, 60, 30, bw, bh,
                              NULL, NULL, wc.hInstance, NULL);
        ShowWindow(big, SW_SHOW);
        GetWindowRect(big, &big0);
    } else {
        printf("(the start mode is too small for the window-size checks)\n");
    }

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
    if (big) {
        RECT r;
        DWORD until = GetTickCount() + 5000;
        do { Sleep(50); pump(); GetWindowRect(big, &r); }
        while ((r.right - r.left > 800 || r.right > 800) && GetTickCount() < until);
        check(r.right - r.left <= 800 && r.bottom - r.top <= 600 && r.left >= 0 && r.right <= 800,
              "800x600 shrinks a larger window to fit");
        printf("800x600: window %ld x %ld at %ld,%ld (was %ld x %ld)\n", r.right - r.left, r.bottom - r.top,
               r.left, r.top, big0.right - big0.left, big0.bottom - big0.top);
    }

    /* NULL: back to the registry mode */
    check(ChangeDisplaySettingsW(NULL, 0) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettings(NULL)");
    wait_change();
    cur = mode_of(ENUM_CURRENT_SETTINGS, &ok);
    check(cur.dmPelsWidth == start.dmPelsWidth && cur.dmPelsHeight == start.dmPelsHeight, "back to the start mode");
    if (big) {
        RECT r;
        DWORD until = GetTickCount() + 5000;
        do { Sleep(50); pump(); GetWindowRect(big, &r); } while (memcmp(&r, &big0, sizeof(r)) && GetTickCount() < until);
        check(!memcmp(&r, &big0, sizeof(r)), "the window grows back to its size and place");
        printf("back: window %ld x %ld at %ld,%ld\n", r.right - r.left, r.bottom - r.top, r.left, r.top);
        DestroyWindow(big);
    }

    /* CDS_UPDATEREGISTRY: the mode to return to, and to boot in */
    check(set_mode(1024, 768, CDS_UPDATEREGISTRY) == DISP_CHANGE_SUCCESSFUL, "CDS_UPDATEREGISTRY 1024x768");
    wait_change();
    reg = mode_of(ENUM_REGISTRY_SETTINGS, &ok);
    check(reg.dmPelsWidth == 1024 && reg.dmPelsHeight == 768, "ENUM_REGISTRY_SETTINGS follows CDS_UPDATEREGISTRY");
    check(saved("XResolution") == 1024 && saved("YResolution") == 768 && saved("BitsPerPel") == 32,
          "the mode is saved in the registry (DefaultSettings)");
    check(set_mode(start.dmPelsWidth, start.dmPelsHeight, CDS_UPDATEREGISTRY) == DISP_CHANGE_SUCCESSFUL,
          "CDS_UPDATEREGISTRY back to the start mode");
    wait_change();
    check(saved("XResolution") == start.dmPelsWidth && saved("YResolution") == start.dmPelsHeight,
          "the registry follows");

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
    check(code == 0, "child's CDS_FULLSCREEN, and a frame through its kept DC");
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
