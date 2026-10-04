/*
 * rawtest.exe — the mouse and the keyboard through Raw Input
 *
 * What games read in their own loops (SDL's relative mouse mode, engines
 * that take keys as scan codes):
 *  - GetRawInputDeviceList lists a mouse (RIM_TYPEMOUSE) and a keyboard
 *    (RIM_TYPEKEYBOARD); their names are interface paths of the mouse and
 *    keyboard interface classes, RIDI_DEVICEINFO says what they are;
 *  - RegisterRawInputDevices for the mouse (Generic Desktop usage 2, with
 *    RIDEV_DEVNOTIFY: its arrival comes at once) and the keyboard (usage
 *    6, RIDEV_NOLEGACY: no WM_KEYDOWN or WM_CHAR come, only raw input),
 *    both for the test's window;
 *  - the test harness moves the pointer onto the window, then moves the
 *    mouse 30 right and 20 down, clicks the left button, turns the wheel a
 *    notch and clicks the right button: WM_INPUT, read with
 *    GetRawInputData (RID_HEADER and RID_INPUT), carries the motion as
 *    relative lLastX, lLastY, and RI_MOUSE_* flags for each press and
 *    release and the wheel (usButtonData 120 a notch);
 *  - then it presses A, Right (an E0 key) and Alt+X: GetRawInputBuffer
 *    takes the RAWKEYBOARD blocks off the queue: make codes, RI_KEY_BREAK
 *    and RI_KEY_E0, virtual keys, and WM_SYSKEYDOWN for X with Alt held;
 *  - RIDEV_REMOVE leaves nothing registered.
 * "rawtest" runs as a 64-bit program and C:\Programs\x86\rawtest.exe as a
 * 32-bit one (RAWINPUTHEADER and RAWINPUT of the other size).
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>


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

/* ---- what the window gets ---- */

static HANDLE g_mouse_dev, g_kbd_dev;
static int    g_bad;                        /* WM_INPUT that did not read back right */
static int    g_moves;                      /* WM_INPUT for the mouse */
static LONG   g_dx, g_dy;
static int    g_absolute;
static USHORT g_flags[32];                  /* the button flags, in order (motion-only blocks left out) */
static SHORT  g_wheel;
static int    g_nflags;
static int    g_arrival;                    /* WM_INPUT_DEVICE_CHANGE GIDC_ARRIVAL for the mouse */
static int    g_legacy_keys;                /* WM_KEYDOWN, WM_CHAR... (RIDEV_NOLEGACY: none) */
static DWORD  g_last_input;                 /* GetTickCount of the last WM_INPUT */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INPUT) {
        g_last_input = GetTickCount();
        UINT sz = 0;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) != 0 || sz > 256) { g_bad++; return 0; }
        union { RAWINPUT ri; BYTE b[256]; } u;
        UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, &u, &sz, sizeof(RAWINPUTHEADER));
        RAWINPUTHEADER hd;
        UINT hsz = sizeof(hd);
        UINT hgot = GetRawInputData((HRAWINPUT)lp, RID_HEADER, &hd, &hsz, sizeof(RAWINPUTHEADER));
        if (got != sz || hgot != sizeof(hd) || hd.hDevice != u.ri.header.hDevice || hd.dwType != u.ri.header.dwType ||
            u.ri.header.dwSize != sz || GET_RAWINPUT_CODE_WPARAM(wp) != RIM_INPUT) {
            g_bad++;
        } else if (u.ri.header.dwType == RIM_TYPEMOUSE) {
            if (sz != sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) || u.ri.header.hDevice != g_mouse_dev) g_bad++;
            const RAWMOUSE *rm = &u.ri.data.mouse;
            g_moves++;
            g_dx += rm->lLastX;
            g_dy += rm->lLastY;
            if (rm->usFlags & MOUSE_MOVE_ABSOLUTE) g_absolute++;
            if (rm->usButtonFlags && g_nflags < 32) g_flags[g_nflags++] = rm->usButtonFlags;
            if (rm->usButtonFlags & RI_MOUSE_WHEEL) g_wheel += (SHORT)rm->usButtonData;
        } else if (u.ri.header.dwType != RIM_TYPEKEYBOARD || sz != sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) ||
                   u.ri.header.hDevice != g_kbd_dev) {
            g_bad++;
        }                                           /* (a key before the keyboard part: the release of the Enter that started the
                                                     * test, on a fast machine; the test's keys are left for GetRawInputBuffer) */
        return DefWindowProcW(h, m, wp, lp);
    }
    if (m == WM_INPUT_DEVICE_CHANGE) {
        if (wp == GIDC_ARRIVAL && (HANDLE)lp == g_mouse_dev) g_arrival++;
        return 0;
    }
    if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR) {
        g_legacy_keys++;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

/* Pump until @cond or @ms pass */
#define PUMP_UNTIL(cond, ms) do { DWORD t0_ = GetTickCount(); while (!(cond) && GetTickCount() - t0_ < (ms)) { pump(); Sleep(10); } pump(); } while (0)

/* Everything but WM_INPUT */
static void pump_not_input(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, WM_INPUT - 1, PM_REMOVE) ||
           PeekMessageW(&msg, NULL, WM_INPUT + 1, 0xFFFFFFFF, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static int has_flags(USHORT want)
{
    for (int i = 0; i < g_nflags; i++) if (g_flags[i] & want) return 1;
    return 0;
}

/* ---- the devices ---- */

static void devices(void)
{
    UINT n = 0;
    UINT r0 = GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    check(r0 == 0 && n >= 2, "GetRawInputDeviceList: %u devices", n);
    RAWINPUTDEVICELIST l[16];
    if (n > 16) n = 16;
    UINT got = GetRawInputDeviceList(l, &n, sizeof(l[0]));
    int mice = 0, kbds = 0;
    for (UINT i = 0; i < got && got != (UINT)-1; i++) {
        if (l[i].dwType == RIM_TYPEMOUSE) { mice++; g_mouse_dev = l[i].hDevice; }
        if (l[i].dwType == RIM_TYPEKEYBOARD) { kbds++; g_kbd_dev = l[i].hDevice; }
    }
    check(mice == 1 && kbds == 1, "one mouse and one keyboard listed (%d, %d)", mice, kbds);
    if (!g_mouse_dev || !g_kbd_dev) return;

    WCHAR name[200];
    char aname[200];
    UINT sz = 0;
    UINT r = GetRawInputDeviceInfoW(g_mouse_dev, RIDI_DEVICENAME, NULL, &sz);
    UINT len = sz;
    sz = 200;
    UINT r2 = GetRawInputDeviceInfoW(g_mouse_dev, RIDI_DEVICENAME, name, &sz);
    check(r == 0 && r2 == len && len == wcslen(name) + 1 && !wcsncmp(name, L"\\\\?\\", 4) &&
          wcsstr(name, L"{378de44c-56ef-11d1-bc8c-00a0c91405dd}"),
          "the mouse's name: %u characters, a mouse interface path (%ls)", len, name);
    sz = 200;
    r = GetRawInputDeviceInfoA(g_kbd_dev, RIDI_DEVICENAME, aname, &sz);
    check(r != (UINT)-1 && !strncmp(aname, "\\\\?\\", 4) && strstr(aname, "{884b96c3-56ef-11d1-bc8c-00a0c91405dd}"),
          "the keyboard's name (A): a keyboard interface path (%s)", aname);
    sz = 4;
    r = GetRawInputDeviceInfoW(g_mouse_dev, RIDI_DEVICENAME, name, &sz);
    check(r == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER && sz == len, "too small a buffer: the size needed");

    RID_DEVICE_INFO ri;
    memset(&ri, 0, sizeof(ri));
    ri.cbSize = sz = sizeof(ri);
    r = GetRawInputDeviceInfoW(g_mouse_dev, RIDI_DEVICEINFO, &ri, &sz);
    check(r == sizeof(ri) && ri.dwType == RIM_TYPEMOUSE && ri.mouse.dwNumberOfButtons >= 3 && ri.mouse.fHasHorizontalWheel,
          "RIDI_DEVICEINFO: a mouse, %lu buttons", (unsigned long)ri.mouse.dwNumberOfButtons);
    memset(&ri, 0, sizeof(ri));
    ri.cbSize = sz = sizeof(ri);
    r = GetRawInputDeviceInfoW(g_kbd_dev, RIDI_DEVICEINFO, &ri, &sz);
    check(r == sizeof(ri) && ri.dwType == RIM_TYPEKEYBOARD && ri.keyboard.dwNumberOfFunctionKeys == 12 &&
          ri.keyboard.dwNumberOfKeysTotal >= 101,
          "RIDI_DEVICEINFO: a keyboard, %lu keys", (unsigned long)ri.keyboard.dwNumberOfKeysTotal);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    devices();
    if (!g_mouse_dev || !g_kbd_dev) goto out;

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"rawtest";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"rawtest", L"Raw mouse and keyboard", WS_OVERLAPPEDWINDOW, 100, 100, 600, 400,
                             NULL, NULL, wc.hInstance, NULL);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    pump();

    RAWINPUTDEVICE rid[2] = {
        { 1, 2, RIDEV_DEVNOTIFY, w },
        { 1, 6, RIDEV_NOLEGACY, w },
    };
    check(RegisterRawInputDevices(rid, 2, sizeof(rid[0])), "RegisterRawInputDevices: the mouse and the keyboard");
    RAWINPUTDEVICE back[4];
    UINT nb = 4;
    UINT r = GetRegisteredRawInputDevices(back, &nb, sizeof(back[0]));
    check(r == 2 && back[0].usUsage + back[1].usUsage == 8, "GetRegisteredRawInputDevices: both (%d)", (int)r);
    PUMP_UNTIL(g_arrival, 3000);
    check(g_arrival == 1, "RIDEV_DEVNOTIFY: the mouse's arrival (%d)", g_arrival);

    /* ---- the mouse ---- */
    printf("rawtest: point at the window\n");
    PUMP_UNTIL(g_moves && GetTickCount() - g_last_input > 2000, 60000);
    check(g_moves > 0, "WM_INPUT while the pointer moved onto the window (%d)", g_moves);
    check(GetForegroundWindow() == w, "the window is in front");
    g_moves = g_nflags = g_absolute = g_wheel = 0;
    g_dx = g_dy = 0;
    printf("rawtest: move 30 20, click, wheel, right click\n");
    PUMP_UNTIL(has_flags(RI_MOUSE_RIGHT_BUTTON_UP), 30000);
    PUMP_UNTIL(0, 500);
    check(g_bad == 0, "every WM_INPUT read back with GetRawInputData, RID_INPUT and RID_HEADER alike (%d bad)", g_bad);
    check(g_dx == 30 && g_dy == 20 && !g_absolute, "relative motion: %ld, %ld in %d blocks", (long)g_dx, (long)g_dy, g_moves);
    int order = g_nflags == 5 && g_flags[0] == RI_MOUSE_LEFT_BUTTON_DOWN && g_flags[1] == RI_MOUSE_LEFT_BUTTON_UP &&
                g_flags[2] == RI_MOUSE_WHEEL && g_flags[3] == RI_MOUSE_RIGHT_BUTTON_DOWN && g_flags[4] == RI_MOUSE_RIGHT_BUTTON_UP;
    check(order, "button flags: left down, left up, wheel, right down, right up (%d: %x %x %x %x %x)", g_nflags,
          g_flags[0], g_flags[1], g_flags[2], g_flags[3], g_flags[4]);
    check(g_wheel == 120 || g_wheel == -120, "the wheel: one notch, usButtonData %d", g_wheel);

    /* ---- the keyboard, through GetRawInputBuffer ---- */
    printf("rawtest: press a, right, alt-x\n");
    RAWKEYBOARD keys[16];
    int nkeys = 0;
    UINT bad_sizes = 0;
    DWORD t0 = GetTickCount();
    while (nkeys < 8 && GetTickCount() - t0 < 30000) {
        pump_not_input();
        union { RAWINPUT ri; BYTE b[1024]; } u;
        UINT sz = 0;
        if (GetRawInputBuffer(NULL, &sz, sizeof(RAWINPUTHEADER)) != 0) { bad_sizes++; break; }
        if (!sz) { Sleep(20); continue; }
        sz = sizeof(u);
        UINT n = GetRawInputBuffer(&u.ri, &sz, sizeof(RAWINPUTHEADER));
        if (n == (UINT)-1) { bad_sizes++; break; }
        PRAWINPUT p = &u.ri;
        for (UINT i = 0; i < n; i++, p = NEXTRAWINPUTBLOCK(p)) {
            if (p->header.dwType != RIM_TYPEKEYBOARD) continue;   /* (the mouse may have twitched) */
            if (p->header.dwSize != sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) || p->header.hDevice != g_kbd_dev) bad_sizes++;
            if (nkeys < 16) keys[nkeys++] = p->data.keyboard;
        }
    }
    pump();
    check(bad_sizes == 0, "GetRawInputBuffer: keyboard blocks of the right size and device");
    for (int i = 0; i < nkeys; i++)
        printf("     key %d: make %02x flags %x vkey %02x message %04x\n", i, keys[i].MakeCode, keys[i].Flags, keys[i].VKey,
               (unsigned)keys[i].Message);
    check(nkeys == 8, "eight key blocks: a, right and alt-x down and up (%d)", nkeys);
    if (nkeys == 8) {
        check(keys[0].MakeCode == 0x1E && keys[0].Flags == 0 && keys[0].VKey == 'A' && keys[0].Message == WM_KEYDOWN &&
              keys[1].MakeCode == 0x1E && keys[1].Flags == RI_KEY_BREAK && keys[1].Message == WM_KEYUP,
              "A: make 1E, down then up (RI_KEY_BREAK), VK 'A', WM_KEYDOWN and WM_KEYUP");
        check(keys[2].MakeCode == 0x4D && keys[2].Flags == RI_KEY_E0 && keys[2].VKey == VK_RIGHT &&
              keys[3].Flags == (RI_KEY_E0 | RI_KEY_BREAK),
              "Right: make 4D with RI_KEY_E0, VK_RIGHT");
        check(keys[4].MakeCode == 0x38 && keys[4].VKey == VK_MENU && keys[4].Message == WM_SYSKEYDOWN &&
              keys[5].MakeCode == 0x2D && keys[5].VKey == 'X' && keys[5].Message == WM_SYSKEYDOWN,
              "Alt+X: Alt then X down, both WM_SYSKEYDOWN");
    }
    check(g_legacy_keys == 0, "RIDEV_NOLEGACY: no key messages (%d)", g_legacy_keys);

    rid[0].dwFlags = rid[1].dwFlags = RIDEV_REMOVE;
    rid[0].hwndTarget = rid[1].hwndTarget = NULL;
    check(RegisterRawInputDevices(rid, 2, sizeof(rid[0])), "RIDEV_REMOVE");
    nb = 0;
    check(GetRegisteredRawInputDevices(NULL, &nb, sizeof(back[0])) == 0 && nb == 0, "nothing registered (%u)", nb);
    DestroyWindow(w);
out:
    printf("rawtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
