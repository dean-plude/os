/*
 * bmpcurtest.exe — pointers made from a program's bitmaps, the way GTK
 * makes its own (GDK 2 and 3, gdk/win32/gdkcursor-win32.c):
 *
 *  - DIB sections of 1, 4, 8 and 16 bits per pixel: GetObject describes
 *    them, gdi32 draws on them and the program's bits follow, the
 *    program's bits reach what gdi32 reads, and the colour table can be
 *    read and changed (a 1-bit mask is what failed before);
 *  - a cursor from a 32-bit image with alpha (BITMAPV5HEADER) and a 1-bit
 *    mask in a BITMAPV4HEADER section (gdk_cursor_new_from_pixbuf);
 *  - CreateCursor's AND and XOR planes (GDK's built-in X cursor shapes),
 *    and a monochrome mask twice the cursor's height (CreateIconIndirect
 *    with no colour bitmap);
 *  - CopyCursor of it as the class cursor (SetClassLongPtr GCLP_HCURSOR)
 *    and from WM_SETCURSOR: the desktop shows the program's shape.
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTL_CURSOR_SHAPE 20
#define CTL_DISPLAY_MODE 15
#ifndef SPI_GETWORKAREA
#define SPI_GETWORKAREA  0x0030
#endif

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

/* A 32-bit top-down section to draw into and read back */
static HBITMAP canvas(int w, int h, DWORD **bits)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

/* ---- DIB sections of fewer bits per pixel --------------------------- */
static void check_mono_section(void)
{
    struct { BITMAPV4HEADER h; RGBQUAD c[2]; } bi;           /* as GDK's create_color_bitmap(size, bits, 1) */
    memset(&bi, 0, sizeof(bi));
    bi.h.bV4Size = sizeof(BITMAPV4HEADER);
    bi.h.bV4Width = 32; bi.h.bV4Height = 32;                 /* bottom-up */
    bi.h.bV4Planes = 1; bi.h.bV4BitCount = 1; bi.h.bV4V4Compression = BI_RGB;
    bi.c[1].rgbRed = bi.c[1].rgbGreen = bi.c[1].rgbBlue = 0xFF;
    BYTE *bits = NULL;
    HDC sdc = GetDC(NULL);
    HBITMAP bm = CreateDIBSection(sdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, sdc);
    check(bm && bits, "a 1-bit DIB section (BITMAPV4HEADER, two colours)");
    if (!bm) return;
    DIBSECTION ds;
    memset(&ds, 0, sizeof(ds));
    check(GetObjectW(bm, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 1 && ds.dsBm.bmWidthBytes == 4 &&
          ds.dsBm.bmBits == bits && ds.dsBmih.biBitCount == 1, "GetObject: 1 bit per pixel, 4-byte rows, its bits");
    /* the program sets the top row (the last in memory) and the left column */
    memset(bits + 31 * 4, 0xFF, 4);
    for (int y = 0; y < 32; y++) bits[y * 4] |= 0x80;
    DWORD *px;
    HBITMAP cv = canvas(32, 32, &px);
    HDC a = CreateCompatibleDC(NULL), b = CreateCompatibleDC(NULL);
    HGDIOBJ oa = SelectObject(a, bm), ob = SelectObject(b, cv);
    BitBlt(b, 0, 0, 32, 32, a, 0, 0, SRCCOPY);
    GdiFlush();
    check((px[5] & 0xFFFFFF) == 0xFFFFFF && (px[31 * 32] & 0xFFFFFF) == 0xFFFFFF && (px[31 * 32 + 5] & 0xFFFFFF) == 0,
          "the program's 1-bit pixels reach gdi32 (top row, left column set)");
    /* gdi32 draws white at (10..19, 10..19): the bits follow */
    RECT r = { 10, 10, 20, 20 };
    FillRect(a, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    GdiFlush();
    int row = 31 - 15;                                       /* y 15, bottom-up */
    check((bits[row * 4 + 1] & 0x3F) == 0x3F && (bits[row * 4 + 2] & 0xF0) == 0xF0 && !(bits[(31 - 25) * 4 + 1] & 0x3F),
          "what gdi32 draws on a 1-bit section reaches its bits");
    RGBQUAD q[2];
    check(GetDIBColorTable(a, 0, 2, q) == 2 && q[1].rgbRed == 0xFF && q[0].rgbRed == 0, "GetDIBColorTable: black and white");
    SelectObject(a, oa); SelectObject(b, ob);
    DeleteDC(a); DeleteDC(b);
    DeleteObject(cv);
    DeleteObject(bm);
}

static void check_palette_sections(void)
{
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = 8; bi.h.biHeight = -8; bi.h.biPlanes = 1; bi.h.biBitCount = 8; bi.h.biClrUsed = 4;
    bi.c[0].rgbRed = 0;    bi.c[1].rgbRed = 0xFF;            /* black, red, green, blue */
    bi.c[2].rgbGreen = 0xFF; bi.c[3].rgbBlue = 0xFF;
    BYTE *bits = NULL;
    HBITMAP bm = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    check(bm && bits, "an 8-bit DIB section with four colours");
    if (!bm) return;
    bits[0] = 1; bits[1] = 2; bits[2] = 3;
    HDC a = CreateCompatibleDC(NULL);
    HGDIOBJ oa = SelectObject(a, bm);
    check(GetPixel(a, 0, 0) == RGB(255, 0, 0) && GetPixel(a, 1, 0) == RGB(0, 255, 0) && GetPixel(a, 2, 0) == RGB(0, 0, 255),
          "8-bit: the program's indices are its colours");
    SetPixel(a, 4, 4, RGB(0, 250, 10));
    GdiFlush();
    check(bits[4 * 8 + 4] == 2, "8-bit: gdi32's colour becomes the nearest index");
    RGBQUAD yellow = { 0, 0xFF, 0xFF, 0 };
    check(SetDIBColorTable(a, 1, 1, &yellow) == 1 && GetPixel(a, 0, 0) == RGB(255, 255, 0) && bits[0] == 1,
          "SetDIBColorTable recolours the pixels, keeping their indices");
    SelectObject(a, oa);
    DeleteObject(bm);

    bi.h.biBitCount = 4; bi.h.biClrUsed = 0;
    for (int i = 0; i < 16; i++) bi.c[i].rgbRed = bi.c[i].rgbGreen = bi.c[i].rgbBlue = (BYTE)(i * 17);
    bm = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    check(bm != NULL, "a 4-bit DIB section of 16 greys");
    if (bm) {
        bits[0] = 0xF5;                                      /* x 0: 15, x 1: 5 */
        SelectObject(a, bm);
        check(GetPixel(a, 0, 0) == RGB(255, 255, 255) && GetPixel(a, 1, 0) == RGB(85, 85, 85), "4-bit: high nibble first");
        SetPixel(a, 3, 0, RGB(136, 136, 136));
        GdiFlush();
        check((bits[1] & 0x0F) == 8, "4-bit: gdi32's grey is index 8");
        SelectObject(a, oa);
        DeleteObject(bm);
    }

    struct { BITMAPINFOHEADER h; DWORD m[3]; } b16;
    memset(&b16, 0, sizeof(b16));
    b16.h.biSize = sizeof(BITMAPINFOHEADER);
    b16.h.biWidth = 4; b16.h.biHeight = -2; b16.h.biPlanes = 1; b16.h.biBitCount = 16; b16.h.biCompression = BI_BITFIELDS;
    b16.m[0] = 0xF800; b16.m[1] = 0x07E0; b16.m[2] = 0x001F;
    WORD *w16 = NULL;
    bm = CreateDIBSection(NULL, (BITMAPINFO *)&b16, DIB_RGB_COLORS, (void **)&w16, NULL, 0);
    check(bm != NULL, "a 16-bit 5-6-5 DIB section");
    if (bm) {
        w16[0] = 0x07E0;
        SelectObject(a, bm);
        check(GetPixel(a, 0, 0) == RGB(0, 255, 0), "16-bit: 5-6-5 green");
        SetPixel(a, 1, 0, RGB(255, 0, 0));
        GdiFlush();
        check(w16[1] == 0xF800, "16-bit: gdi32's red is 0xF800");
        SelectObject(a, oa);
        DeleteObject(bm);
    }
    DeleteDC(a);
}

/* ---- cursors --------------------------------------------------------- */
/* @c drawn over grey: the pixel at (x, y) */
static DWORD drawn(HCURSOR c, int x, int y)
{
    DWORD *px;
    HBITMAP cv = canvas(32, 32, &px);
    HDC d = CreateCompatibleDC(NULL);
    HGDIOBJ o = SelectObject(d, cv);
    RECT r = { 0, 0, 32, 32 };
    HBRUSH grey = CreateSolidBrush(RGB(128, 128, 128));
    FillRect(d, &r, grey);
    DeleteObject(grey);
    DrawIconEx(d, 0, 0, c, 32, 32, 0, NULL, DI_NORMAL);
    GdiFlush();
    DWORD v = px[y * 32 + x] & 0xFFFFFF;
    SelectObject(d, o);
    DeleteDC(d);
    DeleteObject(cv);
    return v;
}

static int near_grey(DWORD v, int want)
{
    for (int i = 0; i < 3; i++) {
        int c = (int)(v >> (8 * i) & 0xFF);
        if (c < want - 12 || c > want + 12) return 0;
    }
    return 1;
}

/* GDK's pixbuf cursor: a 32 x 32 image with alpha, a 1-bit mask */
static HCURSOR pixbuf_cursor(void)
{
    BITMAPV5HEADER h;
    memset(&h, 0, sizeof(h));
    h.bV5Size = sizeof(h);
    h.bV5Width = 32; h.bV5Height = 32; h.bV5Planes = 1; h.bV5BitCount = 32;
    h.bV5Compression = BI_BITFIELDS;
    h.bV5RedMask = 0x00FF0000; h.bV5GreenMask = 0x0000FF00; h.bV5BlueMask = 0x000000FF; h.bV5AlphaMask = 0xFF000000;
    DWORD *color = NULL;
    HDC sdc = GetDC(NULL);
    HBITMAP hc = CreateDIBSection(sdc, (BITMAPINFO *)&h, DIB_RGB_COLORS, (void **)&color, NULL, 0);
    struct { BITMAPV4HEADER h; RGBQUAD c[2]; } mi;
    memset(&mi, 0, sizeof(mi));
    mi.h.bV4Size = sizeof(BITMAPV4HEADER);
    mi.h.bV4Width = 32; mi.h.bV4Height = 32; mi.h.bV4Planes = 1; mi.h.bV4BitCount = 1;
    mi.c[1].rgbRed = mi.c[1].rgbGreen = mi.c[1].rgbBlue = 0xFF;
    BYTE *mask = NULL;
    HBITMAP hm = CreateDIBSection(sdc, (BITMAPINFO *)&mi, DIB_RGB_COLORS, (void **)&mask, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!hc || !hm) return NULL;
    /* a red square (8..23) with a half-transparent blue bar (rows 2-3), rest clear;
     * rows bottom-up, the mask's bit set where alpha is 0, as GDK does */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int row = 31 - y;
            DWORD v = 0;
            if (x >= 8 && x < 24 && y >= 8 && y < 24) v = 0xFFFF0000;
            else if (y >= 2 && y < 4) v = 0x800000FF;
            color[row * 32 + x] = v;
            if (!(v >> 24)) mask[row * 4 + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    ICONINFO ii = { FALSE, 3, 4, hm, hc };
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(hc);
    DeleteObject(hm);
    return c;
}

static void check_pixbuf_cursor(HCURSOR c)
{
    check(c != NULL, "CreateIconIndirect: a 32-bit image with alpha and a 1-bit mask (GDK's pixbuf cursors)");
    if (!c) return;
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    check(GetIconInfo(c, &ii) && !ii.fIcon && ii.xHotspot == 3 && ii.yHotspot == 4, "it is a cursor with its hot spot at (3, 4)");
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DWORD in = drawn(c, 16, 16), out = drawn(c, 1, 30), half = drawn(c, 16, 2);
    check(in == 0xFF0000, "the opaque red square draws red");
    check(out == 0x808080, "the clear part leaves what is under it");
    check((half & 0xFF) >= 0xB0 && (half & 0xFF) <= 0xD0 && (half >> 16 & 0xFF) >= 0x30 && (half >> 16 & 0xFF) <= 0x50,
          "the half-transparent bar blends with what is under it");
    if (in != 0xFF0000 || out != 0x808080) printf("  drawn: %06lX %06lX %06lX\n", (unsigned long)in, (unsigned long)out, (unsigned long)half);
}

/* GDK's cursor from a pixbuf without alpha: a 24-bit image and a 1-bit mask */
static void check_rgb_with_mask(void)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32; bi.bmiHeader.biHeight = 32; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    BYTE *rgb = NULL;
    HBITMAP hc = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&rgb, NULL, 0);
    struct { BITMAPINFOHEADER h; RGBQUAD c[2]; } mi;
    memset(&mi, 0, sizeof(mi));
    mi.h = bi.bmiHeader;
    mi.h.biBitCount = 1;
    mi.c[1].rgbRed = mi.c[1].rgbGreen = mi.c[1].rgbBlue = 0xFF;
    BYTE *mask = NULL;
    HBITMAP hm = CreateDIBSection(NULL, (BITMAPINFO *)&mi, DIB_RGB_COLORS, (void **)&mask, NULL, 0);
    if (!hc || !hm) { check(0, "a 24-bit image and a 1-bit mask"); return; }
    for (int y = 0; y < 32; y++)                             /* green on the left half, the right half masked out */
        for (int x = 0; x < 32; x++) {
            BYTE *q = rgb + (31 - y) * 96 + 3 * x;
            q[1] = x < 16 ? 0xFF : 0;
            if (x >= 16) mask[(31 - y) * 4 + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    ICONINFO ii = { FALSE, 0, 0, hm, hc };
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(hc);
    DeleteObject(hm);
    check(c && drawn(c, 4, 4) == 0x00FF00 && drawn(c, 20, 4) == 0x808080, "a 24-bit image's mask decides what is clear");
    if (c) DestroyCursor(c);
}

/* GDK's X cursor shapes: CreateCursor with AND and XOR planes (32 x 32,
 * rows of 4 bytes); a white pixel, a black one, a clear one */
static void check_planes(void)
{
    BYTE and[128], xor[128];
    memset(and, 0xFF, sizeof(and));
    memset(xor, 0, sizeof(xor));
    and[10 * 4 + 1] &= (BYTE)~0x80; xor[10 * 4 + 1] |= 0x80;  /* (8, 10) white */
    and[12 * 4 + 1] &= (BYTE)~0x80;                          /* (8, 12) black */
    HCURSOR c = CreateCursor(GetModuleHandleW(NULL), 8, 10, 32, 32, and, xor);
    check(c != NULL, "CreateCursor with AND and XOR planes");
    if (!c) return;
    check(drawn(c, 8, 10) == 0xFFFFFF && drawn(c, 8, 12) == 0 && drawn(c, 9, 10) == 0x808080,
          "CreateCursor: white, black and clear pixels");
    DestroyCursor(c);

    /* the same as a monochrome mask, AND over XOR, 32 x 64 */
    BYTE mono[256];
    memset(mono, 0, sizeof(mono));
    for (int y = 0; y < 32; y++) for (int i = 0; i < 4; i++) { mono[y * 4 + i] = and[y * 4 + i]; mono[(32 + y) * 4 + i] = xor[y * 4 + i]; }
    HBITMAP m = CreateBitmap(32, 64, 1, 1, mono);
    ICONINFO ii = { FALSE, 8, 10, m, NULL };
    c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(m);
    check(c != NULL, "CreateIconIndirect with only a monochrome mask");
    if (!c) return;
    ICONINFO got;
    memset(&got, 0, sizeof(got));
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    check(GetIconInfo(c, &got) && got.hbmColor && GetObjectW(got.hbmColor, sizeof(bm), &bm) && bm.bmHeight == 32,
          "a monochrome cursor is 32 high, not 64");
    if (got.hbmColor) DeleteObject(got.hbmColor);
    if (got.hbmMask) DeleteObject(got.hbmMask);
    check(drawn(c, 8, 10) == 0xFFFFFF && drawn(c, 8, 12) == 0 && drawn(c, 9, 10) == 0x808080,
          "monochrome mask: white (XOR), black and clear pixels");
    DestroyCursor(c);
}

/* ---- the pointer ----------------------------------------------------- */
static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}

static HCURSOR g_copy;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
        DrawTextW(dc, L"A pointer from a program's bitmaps (as GTK makes them)", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g_copy) DrawIconEx(dc, 40, 40, g_copy, 32, 32, 0, NULL, DI_NORMAL);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void check_pointer(HCURSOR c)
{
    if (!c) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"BmpCurTest";
    RegisterClassW(&wc);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND h = CreateWindowExW(0, L"BmpCurTest", L"Pointer from bitmaps", WS_OVERLAPPEDWINDOW,
                             wa.left + 40, wa.top + 40, wa.right - wa.left - 80, wa.bottom - wa.top - 80,
                             NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    pump(300);
    /* GDK keeps a copy (CopyCursor) and sets it as the window's, and from WM_SETCURSOR */
    g_copy = CopyCursor(c);
    check(g_copy != NULL, "CopyCursor");
    SetClassLongPtrW(h, GCLP_HCURSOR, (LONG_PTR)g_copy);
    check((HCURSOR)GetClassLongPtrW(h, GCLP_HCURSOR) == g_copy, "SetClassLongPtr(GCLP_HCURSOR)");
    POINT pt;
    GetCursorPos(&pt);
    SendMessageW(h, WM_SETCURSOR, (WPARAM)h, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    pump(150);
    check(GetCursor() == g_copy, "DefWindowProc's WM_SETCURSOR sets the class cursor");
    INT32 s[6] = { 0 }, mode[4] = { 0 };
    NtNovaGuiCtl(0, CTL_CURSOR_SHAPE, 0, s);
    NtNovaGuiCtl(0, CTL_DISPLAY_MODE, (ULONG_PTR)(LONG_PTR)-1, mode);
    int side = 32 * (mode[0] / GetSystemMetrics(SM_CXSCREEN) > 1 ? 2 : 1);
    check(s[0] == 1 && s[1] == side && s[2] == side, "the desktop shows the program's pointer");
    if (s[0] != 1 || s[1] != side) printf("  (shape %d, %d x %d)\n", s[0], s[1], s[2]);
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
    DestroyCursor(g_copy);
    DestroyWindow(h);
    pump(50);
}

int main(int argc, char **argv)
{
    HCURSOR c = pixbuf_cursor();
    if (argc > 2 && !strcmp(argv[1], "show")) {              /* bmpcurtest show N: the pointer over a window for N s */
        g_copy = c;
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = c; wc.lpszClassName = L"BmpCurShow";
        RegisterClassW(&wc);
        HWND h = CreateWindowExW(0, L"BmpCurShow", L"Pointer from bitmaps", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 100, 100, 600, 400, NULL, NULL, wc.hInstance, NULL);
        pump((DWORD)atoi(argv[2]) * 1000);
        DestroyWindow(h);
        return 0;
    }
    check_mono_section();
    check_palette_sections();
    check_pixbuf_cursor(c);
    check_rgb_with_mask();
    check_planes();
    check_pointer(c);
    if (c) DestroyCursor(c);
    printf("bmpcurtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
