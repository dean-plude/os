/*
 * cursortest.exe — the system pointers (IDC_* / OCR_*), SetSystemCursor
 *
 *   cursortest          run the tests
 *   cursortest show N   a window of cells, each with one system pointer,
 *                       and their images drawn by DrawIconEx, for N seconds
 *
 * The tests load every IDC_* cursor, check each has its own image and hot
 * spot, make each the pointer over a window and check the desktop draws
 * that system shape (the busy ring turning), replace the I-beam with
 * SetSystemCursor and put it back with SPI_SETCURSORS, and check that a
 * program's 32 x 32 cursor is sent at the display's scale.
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SPI_GETWORKAREA
#define SPI_GETWORKAREA  0x0030
#endif
#define CTL_CURSOR_SHAPE 20
#define CTL_DISPLAY_MODE 15

static const struct { int id; const char *name; } IDS[] = {
    { 32512, "IDC_ARROW" }, { 32513, "IDC_IBEAM" }, { 32514, "IDC_WAIT" }, { 32515, "IDC_CROSS" },
    { 32516, "IDC_UPARROW" }, { 32642, "IDC_SIZENWSE" }, { 32643, "IDC_SIZENESW" }, { 32644, "IDC_SIZEWE" },
    { 32645, "IDC_SIZENS" }, { 32646, "IDC_SIZEALL" }, { 32648, "IDC_NO" }, { 32649, "IDC_HAND" },
    { 32650, "IDC_APPSTARTING" }, { 32651, "IDC_HELP" },
};
#define NIDS ((int)(sizeof(IDS) / sizeof(IDS[0])))

static int g_pass, g_fail;
static HCURSOR g_cur;                   /* the pointer over the test window */

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;
    do {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    } while ((LONG)(end - GetTickCount()) > 0);
}

/* What the desktop draws: { program shape?, w, h, frames, step, system pointer } */
static void shape_now(INT32 s[6])
{
    memset(s, 0, 6 * sizeof(INT32));
    NtNovaGuiCtl(0, CTL_CURSOR_SHAPE, 1, s);
}

static int g_show;                      /* show mode: cells of pointers */

static int cell_at(HWND h, int x, int y)
{
    RECT r;
    GetClientRect(h, &r);
    int cw = (r.right - r.left) / 7, ch = (r.bottom - r.top - 60) / 2;
    if (y < 60 || cw <= 0 || ch <= 0) return -1;
    int c = x / cw, row = (y - 60) / ch;
    return c < 7 && row < 2 ? row * 7 + c : -1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        if (g_show) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            int k = cell_at(h, p.x, p.y);
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(k >= 0 ? IDS[k].id : 32512)));
        } else SetCursor(g_cur);
        return TRUE;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);
        if (g_show) {
            int cw = (r.right - r.left) / 7, ch = (r.bottom - r.top - 60) / 2;
            for (int k = 0; k < NIDS; k++) {
                RECT c = { (k % 7) * cw, 60 + (k / 7) * ch, (k % 7 + 1) * cw, 60 + (k / 7 + 1) * ch };
                FrameRect(dc, &c, (HBRUSH)GetStockObject(GRAY_BRUSH));
                DrawIconEx(dc, c.left + 8, c.top + 8, LoadCursorW(NULL, MAKEINTRESOURCEW(IDS[k].id)), 32, 32, 0, NULL, DI_NORMAL);
                RECT t = c;
                t.top = c.bottom - 24;
                DrawTextA(dc, IDS[k].name + 4, -1, &t, DT_CENTER | DT_SINGLELINE);
            }
            RECT t = { 0, 0, r.right, 60 };
            DrawTextW(dc, L"Each cell shows one system pointer (its image, drawn by DrawIconEx, at the top left)", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else DrawTextW(dc, L"System pointers: the pointer changes over this window", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* A window over most of the screen (the pointer starts in the middle) */
static HWND make_window(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CursorTest";
    RegisterClassW(&wc);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND h = CreateWindowExW(0, L"CursorTest", L"System pointers", WS_OVERLAPPEDWINDOW,
                             wa.left + 40, wa.top + 40, wa.right - wa.left - 80, wa.bottom - wa.top - 80,
                             NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    return h;
}

/* A cursor's 32 x 32 image as DrawIconEx draws it on white */
static unsigned image_hash(HCURSOR c, int *ink)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32; bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(dc, bm);
    DWORD *px = bits;
    for (int i = 0; i < 32 * 32; i++) px[i] = 0xFFFFFF;
    DrawIconEx(dc, 0, 0, c, 32, 32, 0, NULL, DI_NORMAL);
    unsigned h = 2166136261u;
    *ink = 0;
    for (int i = 0; i < 32 * 32; i++) {
        h = (h ^ (px[i] & 0xFFFFFF)) * 16777619u;
        if ((px[i] & 0xFFFFFF) != 0xFFFFFF) (*ink)++;
    }
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
    return h;
}

static void check_images(void)
{
    unsigned hash[NIDS];
    char what[128];
    for (int i = 0; i < NIDS; i++) {
        HCURSOR c = LoadCursorW(NULL, MAKEINTRESOURCEW(IDS[i].id));
        snprintf(what, sizeof(what), "LoadCursor(%s)", IDS[i].name);
        check(c != NULL, what);
        ICONINFO ii;
        memset(&ii, 0, sizeof(ii));
        int ok = c && GetIconInfo(c, &ii);
        snprintf(what, sizeof(what), "%s: a cursor with its hot spot inside 32 x 32 (%lu, %lu)", IDS[i].name,
                 (unsigned long)ii.xHotspot, (unsigned long)ii.yHotspot);
        check(ok && !ii.fIcon && ii.xHotspot < 32 && ii.yHotspot < 32, what);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        int ink;
        hash[i] = image_hash(c, &ink);
        snprintf(what, sizeof(what), "%s: DrawIconEx draws it (%d pixels)", IDS[i].name, ink);
        check(ink > 30, what);
    }
    int distinct = 1;
    for (int i = 0; i < NIDS; i++)
        for (int j = i + 1; j < NIDS; j++)
            if (hash[i] == hash[j]) { distinct = 0; printf("  %s and %s look the same\n", IDS[i].name, IDS[j].name); }
    check(distinct, "every system pointer has its own image");
    check(LoadCursorW(NULL, MAKEINTRESOURCEW(32640)) != NULL, "IDC_SIZE (obsolete) still loads");
}

/* The pointer shows @c; returns what the desktop draws */
static void show(HWND h, HCURSOR c, INT32 s[6])
{
    g_cur = c;
    SetCursor(c);
    (void)h;
    pump(120);
    shape_now(s);
}

static void check_pointer(void)
{
    HWND h = make_window();
    pump(300);
    INT32 s[6];
    char what[128];
    for (int i = 0; i < NIDS; i++) {
        show(h, LoadCursorW(NULL, MAKEINTRESOURCEW(IDS[i].id)), s);
        snprintf(what, sizeof(what), "the desktop draws %s (shows %d)", IDS[i].name, (int)s[5]);
        check(s[0] == 0 && s[5] == IDS[i].id, what);
        if (s[5] != IDS[i].id && i == 0) printf("  (is the pointer over the window?)\n");
    }

    /* the busy ring turns */
    show(h, LoadCursorW(NULL, IDC_WAIT), s);
    int phases = 0, last = -1;
    DWORD end = GetTickCount() + 1000;
    while ((LONG)(end - GetTickCount()) > 0) {
        pump(30);
        shape_now(s);
        if (s[5] == 32514 && s[4] != last) { phases++; last = s[4]; }
    }
    snprintf(what, sizeof(what), "the busy ring turns (%d phases in 1 s)", phases);
    check(phases >= 8, what);

    /* SetSystemCursor: the I-beam becomes a 32 x 32 red square */
    BYTE and[32 * 32 / 8], xr[32 * 32 / 8];
    memset(and, 0, sizeof(and));
    memset(xr, 0, sizeof(xr));
    HCURSOR mine = CreateCursor(GetModuleHandleW(NULL), 4, 4, 32, 32, and, xr);
    check(mine != NULL, "CreateCursor");
    check(SetSystemCursor(mine, OCR_IBEAM), "SetSystemCursor(OCR_IBEAM)");
    show(h, LoadCursorW(NULL, IDC_ARROW), s);
    show(h, LoadCursorW(NULL, IDC_IBEAM), s);
    snprintf(what, sizeof(what), "the desktop draws the replacement I-beam (shows %d)", (int)s[5]);
    check(s[5] == -OCR_IBEAM, what);
    show(h, LoadCursorW(NULL, IDC_ARROW), s);
    check(s[5] == OCR_NORMAL, "the arrow is still NovaOS's");
    check(SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0), "SystemParametersInfo(SPI_SETCURSORS)");
    show(h, LoadCursorW(NULL, IDC_IBEAM), s);
    check(s[5] == OCR_IBEAM, "SPI_SETCURSORS puts NovaOS's I-beam back");

    /* a program's own 32 x 32 cursor reaches the desktop at the display's scale */
    INT32 mode[4] = { 0 };
    NtNovaGuiCtl(0, CTL_DISPLAY_MODE, (ULONG_PTR)(LONG_PTR)-1, mode);
    int scale = mode[0] / GetSystemMetrics(SM_CXSCREEN);
    if (scale < 1) scale = 1;
    memset(xr, 0xFF, 64);
    HCURSOR prog = CreateCursor(GetModuleHandleW(NULL), 0, 0, 32, 32, and, xr);
    show(h, prog, s);
    snprintf(what, sizeof(what), "a program's 32 x 32 cursor is %d x %d at scale %d (got %d x %d)",
             32 * scale, 32 * scale, scale, (int)s[1], (int)s[2]);
    check(s[0] == 1 && s[1] == 32 * scale && s[2] == 32 * scale, what);
    show(h, LoadCursorW(NULL, IDC_ARROW), s);
    DestroyCursor(prog);
    DestroyWindow(h);
    pump(50);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "show")) {
        g_show = 1;
        HWND h = make_window();
        pump((DWORD)atoi(argv[2]) * 1000);
        DestroyWindow(h);
        return 0;
    }
    check_images();
    check_pointer();
    printf("cursortest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
