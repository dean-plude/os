/*
 * inputtest.exe — mouse side buttons, the horizontal wheel and the volume
 * keys, as a program sees them
 *
 * Run by tools/selftest.py (tests/selftest/core/) on the core boot (a PS/2
 * mouse and a USB keyboard on xHCI), which does what this program asks:
 *   "inputtest: plug in a USB mouse, press back and forward"
 *        a usb-mouse is plugged in and its buttons 4 and 5 are pressed
 *   "inputtest: unplug it, scroll right and left, press back"
 *        it is unplugged again; the PS/2 mouse (IntelliMouse Explorer)
 *        turns its horizontal wheel and presses button 4
 *   "inputtest: press volume down, volume up, mute twice"
 *        on the USB keyboard
 * and checks the messages a full-screen window gets: WM_XBUTTONDOWN/UP,
 * WM_APPCOMMAND from DefWindowProc (back and forward from the mouse, the
 * volume commands from the keys), WM_MOUSEHWHEEL and WM_KEYDOWN with the
 * VK_VOLUME_* keys.
 */
#include <windows.h>
#include <stdio.h>

#ifndef WM_XBUTTONDOWN
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP   0x020C
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef WM_APPCOMMAND
#define WM_APPCOMMAND  0x0319
#endif

static int passed, failed;

static void check(const char *what, BOOL ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (ok) passed++; else failed++;
}

/* What the window saw */
static int xdown[3], xup[3];           /* by XBUTTON1, 2 */
static int x1_held_seen;               /* GetKeyState(VK_XBUTTON1) was down during WM_XBUTTONDOWN */
static int app_mouse[3];               /* WM_APPCOMMAND back / forward from the mouse */
static int app_key[32];                /* WM_APPCOMMAND from keys, by command */
static int hwheel_right, hwheel_left;
static int vk_down[256];

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_XBUTTONDOWN:
        if (HIWORD(wp) == 1 || HIWORD(wp) == 2) xdown[HIWORD(wp)]++;
        if (HIWORD(wp) == 1 && GetKeyState(0x05) < 0 && (LOWORD(wp) & 0x20)) x1_held_seen = 1;
        break;                             /* (on to DefWindowProc) */
    case WM_XBUTTONUP:
        if (HIWORD(wp) == 1 || HIWORD(wp) == 2) xup[HIWORD(wp)]++;
        break;
    case WM_APPCOMMAND: {
        int cmd = HIWORD(lp) & 0x0FFF, dev = HIWORD(lp) & 0xF000;
        if (dev == 0x8000 && (cmd == 1 || cmd == 2)) app_mouse[cmd]++;
        else if (dev == 0 && cmd < 32) app_key[cmd]++;
        return TRUE;
    }
    case WM_MOUSEHWHEEL:
        if ((short)HIWORD(wp) > 0) hwheel_right++; else if ((short)HIWORD(wp) < 0) hwheel_left++;
        return 0;
    case WM_KEYDOWN:
        vk_down[wp & 0xFF]++;
        break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* Pump messages until @done() or @secs pass */
static BOOL pump(BOOL (*done)(void), int secs)
{
    DWORD end = GetTickCount() + secs * 1000;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (done()) return TRUE;
        Sleep(20);
    }
    return done();
}

static BOOL usb_done(void) { return xup[1] && xup[2] && app_mouse[1] && app_mouse[2]; }
static BOOL ps2_done(void) { return hwheel_right && hwheel_left && xup[1] >= 2; }
static BOOL keys_done(void) { return app_key[10] && app_key[9] && app_key[8] >= 2; }

int main(void)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "inputtest";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND h = CreateWindowExA(0, "inputtest", "inputtest", WS_POPUP | WS_VISIBLE, 0, 0, sw, sh,
                             NULL, NULL, wc.hInstance, NULL);
    if (!h) { printf("FAIL: CreateWindowEx\n"); return 1; }
    SetForegroundWindow(h);
    SetFocus(h);
    MSG msg;
    for (int i = 0; i < 25; i++) {           /* (let it be shown and focused) */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(20);
    }

    printf("inputtest: plug in a USB mouse, press back and forward\n");
    fflush(stdout);
    pump(usb_done, 30);
    check("USB mouse: WM_XBUTTONDOWN/UP for XBUTTON1", xdown[1] >= 1 && xup[1] >= 1);
    check("USB mouse: WM_XBUTTONDOWN/UP for XBUTTON2", xdown[2] >= 1 && xup[2] >= 1);
    check("USB mouse: MK_XBUTTON1 and GetKeyState(VK_XBUTTON1) while it is down", x1_held_seen);
    check("USB mouse: WM_APPCOMMAND APPCOMMAND_BROWSER_BACKWARD (FAPPCOMMAND_MOUSE)", app_mouse[1] >= 1);
    check("USB mouse: WM_APPCOMMAND APPCOMMAND_BROWSER_FORWARD (FAPPCOMMAND_MOUSE)", app_mouse[2] >= 1);

    printf("inputtest: unplug it, scroll right and left, press back\n");
    fflush(stdout);
    pump(ps2_done, 30);
    check("PS/2 mouse: WM_MOUSEHWHEEL to the right (positive)", hwheel_right >= 1);
    check("PS/2 mouse: WM_MOUSEHWHEEL to the left (negative)", hwheel_left >= 1);
    check("PS/2 mouse: WM_XBUTTONUP for XBUTTON1", xup[1] >= 2);

    printf("inputtest: press volume down, volume up, mute twice\n");
    fflush(stdout);
    pump(keys_done, 30);
    check("USB keyboard: WM_KEYDOWN VK_VOLUME_UP, VK_VOLUME_DOWN, VK_VOLUME_MUTE",
          vk_down[0xAF] && vk_down[0xAE] && vk_down[0xAD] >= 2);
    check("USB keyboard: WM_APPCOMMAND APPCOMMAND_VOLUME_UP, _DOWN, _MUTE (FAPPCOMMAND_KEY)",
          app_key[10] && app_key[9] && app_key[8] >= 2);

    DestroyWindow(h);
    printf("inputtest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
