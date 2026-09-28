/* winhello.exe — a real Win32 GUI program: window, painting, input, timer */
#include <windows.h>

static int g_ticks;
static int g_clicks;
static int g_lastx = -1, g_lasty;

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);

    /* background */
    HBRUSH bg = CreateSolidBrush(RGB(0x1E, 0x1E, 0x2E));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    /* a header bar */
    RECT hdr = { 0, 0, rc.right, 40 };
    HBRUSH hb = CreateSolidBrush(RGB(0x00, 0x78, 0xD4));
    FillRect(dc, &hdr, hb);
    DeleteObject(hb);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    TextOutA(dc, 12, 12, "Hello from a native Win32 window on NovaOS!", 43);

    /* some shapes */
    SetTextColor(dc, RGB(0xE0, 0xE0, 0xE0));
    char line[64];
    int n = 0;
    const char *m = "Timer ticks: ";
    for (const char *p = m; *p; p++) line[n++] = *p;
    int v = g_ticks, digs = 0, tmp = v; char d[10];
    if (v == 0) d[digs++] = '0';
    while (tmp) { d[digs++] = '0' + tmp % 10; tmp /= 10; }
    while (digs) line[n++] = d[--digs];
    line[n] = 0;
    TextOutA(dc, 12, 60, line, n);
    TextOutA(dc, 12, 84, "Click in the window to draw dots. Close to quit.", 48);

    /* a moving box driven by the timer */
    int bx = 12 + (g_ticks * 6) % (rc.right - 60);
    HBRUSH box = CreateSolidBrush(RGB(0x30, 0xD0, 0x60));
    RECT br = { bx, 120, bx + 40, 160 };
    FillRect(dc, &br, box);
    DeleteObject(box);

    /* the last click */
    if (g_lastx >= 0) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xFF, 0xC0, 0x30));
        HBRUSH yb = CreateSolidBrush(RGB(0xFF, 0xC0, 0x30));
        SelectObject(dc, pen);
        SelectObject(dc, yb);
        Ellipse(dc, g_lastx - 8, g_lasty - 8, g_lastx + 8, g_lasty + 8);
        DeleteObject(pen); DeleteObject(yb);
    }

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 100, 0);           /* 10 fps */
        return 0;
    case WM_TIMER:
        g_ticks++;
        InvalidateRect(hwnd, 0, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        g_lastx = (short)(lp & 0xFFFF);
        g_lasty = (short)((lp >> 16) & 0xFFFF);
        g_clicks++;
        InvalidateRect(hwnd, 0, FALSE);
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int main(void)
{
    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char *)&wc)[i] = 0;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "NovaWinHello";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, "NovaWinHello", "Win32 Hello", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 480, 300, 0, 0, 0, 0);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
