/*
 * anitest.exe — animated cursors (.ani)
 *
 *   anitest          run the tests
 *   anitest show N   a window with the spinner as its pointer for N seconds
 *
 * anitest.ani (made by tools/mkani.py) is linked in as an ANICURSOR
 * resource: 8 frames of 32 x 32 with the hot spot at (16, 16), played as
 * frames 0, 7, 6 ... 1; step 0 lasts 10 jiffies, the others 5.  Frame k
 * has a centre dot of COLORS[k].  The tests load it from the resource,
 * from a file (LoadCursorFromFile, LoadImage) and from memory
 * (CreateIconFromResourceEx), draw its steps, and make it the pointer:
 * the desktop must then show it over the window and step through it.
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SPI_GETWORKAREA
#define SPI_GETWORKAREA  0x0030
#endif
#define CTL_SET_CURSOR   19
#define CTL_CURSOR_SHAPE 20

static const DWORD COLORS[8] = { 0xE81123, 0xFF8C00, 0xFFB900, 0x10893E, 0x00B7C3, 0x0078D7, 0x8764B8, 0xE3008C };
static const DWORD SEQ[8] = { 0, 7, 6, 5, 4, 3, 2, 1 };
static const DWORD RATE[8] = { 10, 5, 5, 5, 5, 5, 5, 5 };

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

/* The steps, rates and frames GetCursorFrameInfo reports */
static void check_frames(HCURSOR c, const char *how)
{
    char what[128];
    DWORD rate = 0, n = 0;
    HCURSOR f0 = GetCursorFrameInfo(c, 0, 0, &rate, &n);
    snprintf(what, sizeof(what), "%s: 8 steps (got %lu)", how, (unsigned long)n);
    check(f0 && n == 8, what);
    int ok = 1;
    HCURSOR seen[8] = { 0 };
    for (DWORD i = 0; i < 8; i++) {
        HCURSOR f = GetCursorFrameInfo(c, 0, i, &rate, &n);
        if (!f || rate != RATE[i]) ok = 0;
        else seen[SEQ[i]] = f;
    }
    snprintf(what, sizeof(what), "%s: each step's rate", how);
    check(ok, what);
    int distinct = 1;
    for (int i = 0; i < 8; i++) for (int j = i + 1; j < 8; j++) if (!seen[i] || seen[i] == seen[j]) distinct = 0;
    snprintf(what, sizeof(what), "%s: 8 different frames", how);
    check(distinct, what);
    snprintf(what, sizeof(what), "%s: step 0 is the cursor itself", how);
    check(f0 == c, what);
    ICONINFO ii;
    snprintf(what, sizeof(what), "%s: a cursor with its hot spot at (16, 16)", how);
    check(GetIconInfo(c, &ii) && !ii.fIcon && ii.xHotspot == 16 && ii.yHotspot == 16, what);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

/* DrawIconEx(step) draws that step's frame: its centre dot's colour */
static void check_draw(HCURSOR c)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32; bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits;
    HBITMAP bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(dc, bm);
    int ok = 1;
    for (UINT step = 0; step < 8; step++) {
        RECT r = { 0, 0, 32, 32 };
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        DrawIconEx(dc, 0, 0, c, 32, 32, step, NULL, DI_NORMAL);
        GdiFlush();
        DWORD px = ((DWORD *)bits)[16 * 32 + 16] & 0xFFFFFF;
        if (px != COLORS[SEQ[step]]) {
            printf("  step %u: centre %06lX, want %06lX\n", step, (unsigned long)px, (unsigned long)COLORS[SEQ[step]]);
            ok = 0;
        }
    }
    check(ok, "DrawIconEx draws each step's frame");
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bm);
}

/* What the desktop shows: { a program's shape?, w, h, frames, step } */
static void shape_now(INT32 out[5])
{
    memset(out, 0, 5 * sizeof(INT32));
    NtNovaGuiCtl(0, CTL_CURSOR_SHAPE, 0, out);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}

static HCURSOR g_spinner;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) { SetCursor(g_spinner); return TRUE; }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Animated cursor (.ani): the pointer spins over this window", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (UINT i = 0; i < 8; i++) DrawIconEx(dc, 40 + i * 48, 40, g_spinner, 32, 32, i, NULL, DI_NORMAL);
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
    wc.hCursor = g_spinner;
    wc.lpszClassName = L"AniTest";
    RegisterClassW(&wc);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND h = CreateWindowExW(0, L"AniTest", L"Animated cursor", WS_OVERLAPPEDWINDOW,
                           wa.left + 40, wa.top + 40, wa.right - wa.left - 80, wa.bottom - wa.top - 80,
                           NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    return h;
}

static void check_pointer(void)
{
    HWND h = make_window();
    pump(300);
    SetCursor(g_spinner);
    pump(100);
    INT32 s[5];
    shape_now(s);
    check(s[0] == 1 && s[1] == 32 && s[2] == 32 && s[3] == 8, "the desktop shows the spinner over the window");
    if (!s[0]) printf("  (shape %d, %d x %d, %d frames: is the pointer over the window?)\n", s[0], s[1], s[2], s[3]);
    int seen = 0;
    DWORD end = GetTickCount() + 1500;
    while ((LONG)(end - GetTickCount()) > 0) {
        pump(20);
        shape_now(s);
        if (s[0] == 1 && s[4] >= 0 && s[4] < 8) seen |= 1 << s[4];
    }
    int n = 0;
    for (int i = 0; i < 8; i++) n += seen >> i & 1;
    printf("  steps shown in 1.5 s: %d of 8\n", n);
    check(n >= 6, "the pointer steps through the animation");

    SetCursor(NULL);
    pump(100);
    shape_now(s);
    check(s[0] == 1 && s[1] == 0, "SetCursor(NULL) hides the pointer");
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
    pump(100);
    shape_now(s);
    check(s[0] == 0, "IDC_ARROW gives the desktop's arrow back");
    DestroyWindow(h);
    pump(50);
}

int main(int argc, char **argv)
{
    HINSTANCE inst = GetModuleHandleW(NULL);

    /* 1. The ANICURSOR resource */
    g_spinner = LoadCursorW(inst, MAKEINTRESOURCEW(1));
    check(g_spinner != NULL, "LoadCursor finds the ANICURSOR resource");
    if (!g_spinner) { printf("anitest: %d passed, %d failed\n", g_pass, g_fail); return 1; }

    if (argc > 2 && !strcmp(argv[1], "show")) {
        HWND h = make_window();
        SetCursor(g_spinner);
        pump((DWORD)atoi(argv[2]) * 1000);
        DestroyWindow(h);
        return 0;
    }

    check_frames(g_spinner, "resource");
    check_draw(g_spinner);

    /* 2. From memory and from a file */
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(21));
    BYTE *data = r ? LockResource(LoadResource(inst, r)) : NULL;
    DWORD size = r ? SizeofResource(inst, r) : 0;
    check(data && size > 1000, "the resource's bytes");
    if (data) {
        HCURSOR m = (HCURSOR)CreateIconFromResourceEx(data, size, FALSE, 0x00030000, 32, 32, 0);
        check(m != NULL, "CreateIconFromResourceEx takes a RIFF ACON");
        if (m) { check_frames(m, "memory"); DestroyCursor(m); }

        WCHAR path[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH, path);
        memcpy(path + len, L"anitest.ani", 12 * sizeof(WCHAR));
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD wrote = 0;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, data, size, &wrote, NULL); CloseHandle(f); }
        check(wrote == size, "wrote anitest.ani");
        HCURSOR fc = LoadCursorFromFileW(path);
        check(fc != NULL, "LoadCursorFromFile reads the .ani");
        if (fc) { check_frames(fc, "LoadCursorFromFile"); DestroyCursor(fc); }
        HCURSOR li = (HCURSOR)LoadImageW(NULL, path, IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        check(li != NULL, "LoadImage(LR_LOADFROMFILE) reads the .ani");
        if (li) { check_frames(li, "LoadImage"); DestroyCursor(li); }
        check(!LoadCursorFromFileW(L"C:\\no\\such.ani"), "LoadCursorFromFile: a missing file gives NULL");
        DeleteFileW(path);
    }

    /* 3. The pointer itself */
    check_pointer();

    printf("anitest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
