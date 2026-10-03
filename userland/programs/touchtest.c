/*
 * touchtest.exe — multi-touch as programs see it: WM_TOUCH and WM_POINTER
 *
 * Run by tools/selftest.py's devices suite on a boot with a virtio
 * multi-touch screen, which does what this program asks:
 *   "touchtest: two fingers on the left, move them, lift them"
 *   "touchtest: tap on the right"
 * The left half of the screen is a window that called RegisterTouchWindow
 * (WM_TOUCH and GetTouchInputInfo); the right half one that didn't
 * (WM_POINTERDOWN / UPDATE / UP, GetPointerInfo, GetPointerType,
 * GetPointerFrameTouchInfo), and that passes them to DefWindowProc, which
 * should turn the primary contact into WM_LBUTTONDOWN / WM_LBUTTONUP.
 */
#include <windows.h>
#include <stdio.h>

#define WM_TOUCH_         0x0240
#define WM_POINTERUPDATE_ 0x0245
#define WM_POINTERDOWN_   0x0246
#define WM_POINTERUP_     0x0247
#define TOUCHEVENTF_MOVE_    0x01
#define TOUCHEVENTF_DOWN_    0x02
#define TOUCHEVENTF_UP_      0x04
#define TOUCHEVENTF_PRIMARY_ 0x10
#define POINTER_FLAG_PRIMARY_ 0x2000
#define POINTER_FLAG_DOWN_    0x10000
#define PT_TOUCH_ 2

typedef struct {
    LONG x, y; HANDLE hSource; DWORD dwID, dwFlags, dwMask, dwTime; ULONG_PTR dwExtraInfo; DWORD cxContact, cyContact;
} TouchIn;
typedef struct {
    DWORD pointerType; UINT32 pointerId, frameId, pointerFlags; HANDLE sourceDevice; HWND hwndTarget;
    POINT ptPixelLocation, ptHimetricLocation, ptPixelLocationRaw, ptHimetricLocationRaw;
    DWORD dwTime; UINT32 historyCount; INT32 InputData; DWORD dwKeyStates; UINT64 PerformanceCount; int ButtonChangeType;
} PtrInfo;
typedef struct { PtrInfo pointerInfo; UINT32 touchFlags, touchMask; RECT rcContact, rcContactRaw; UINT32 orientation, pressure; } PtrTouchInfo;

typedef BOOL (WINAPI *RegisterTouchWindowFn)(HWND, ULONG);
typedef BOOL (WINAPI *GetTouchInputInfoFn)(HANDLE, UINT, TouchIn *, int);
typedef BOOL (WINAPI *CloseTouchInputHandleFn)(HANDLE);
typedef BOOL (WINAPI *GetPointerInfoFn)(UINT32, PtrInfo *);
typedef BOOL (WINAPI *GetPointerTypeFn)(UINT32, DWORD *);
typedef BOOL (WINAPI *GetPointerFrameTouchInfoFn)(UINT32, UINT32 *, PtrTouchInfo *);

static RegisterTouchWindowFn pRegisterTouchWindow;
static GetTouchInputInfoFn pGetTouchInputInfo;
static CloseTouchInputHandleFn pCloseTouchInputHandle;
static GetPointerInfoFn pGetPointerInfo;
static GetPointerTypeFn pGetPointerType;
static GetPointerFrameTouchInfoFn pGetPointerFrameTouchInfo;

static int passed, failed, sw, sh;

static void check(const char *what, BOOL ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (ok) passed++; else failed++;
}

/* Left window (WM_TOUCH) */
static int touch_msgs, two_down, moves, ups, primaries, ids_ok = 1, left_half = 1;
static DWORD first_ids[2];
/* Right window (pointers) */
static int pdown, pup, pupdate, ptype_ok, pinfo_ok, pframe_ok, lbdown, lbup, lb_where_ok;

static LRESULT CALLBACK left_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_TOUCH_) {
        TouchIn in[10];
        UINT n = LOWORD(wp) > 10 ? 10 : LOWORD(wp);
        touch_msgs++;
        if (pGetTouchInputInfo((HANDLE)lp, n, in, sizeof(TouchIn))) {
            int downs = 0;
            for (UINT i = 0; i < n; i++) {
                if (in[i].dwFlags & TOUCHEVENTF_DOWN_) { if (downs < 2) first_ids[downs] = in[i].dwID; downs++; }
                if (in[i].dwFlags & TOUCHEVENTF_MOVE_) moves++;
                if (in[i].dwFlags & TOUCHEVENTF_UP_) ups++;
                if ((in[i].dwFlags & TOUCHEVENTF_DOWN_) && (in[i].dwFlags & TOUCHEVENTF_PRIMARY_)) primaries++;
                if (in[i].x / 100 >= sw / 2) left_half = 0;
            }
            if (downs == 2) { two_down++; if (first_ids[0] == first_ids[1]) ids_ok = 0; }
        }
        pCloseTouchInputHandle((HANDLE)lp);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static LRESULT CALLBACK right_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_POINTERDOWN_: {
        UINT32 id = LOWORD(wp);
        DWORD type = 0;
        PtrInfo pi;
        pdown++;
        if (pGetPointerType(id, &type) && type == PT_TOUCH_) ptype_ok = 1;
        if (pGetPointerInfo(id, &pi) && (pi.pointerFlags & POINTER_FLAG_DOWN_) && (pi.pointerFlags & POINTER_FLAG_PRIMARY_) &&
            pi.hwndTarget == h && pi.ptPixelLocation.x == (short)LOWORD(lp) && pi.ptPixelLocation.x >= sw / 2)
            pinfo_ok = 1;
        UINT32 n = 10;
        PtrTouchInfo ti[10];
        if (pGetPointerFrameTouchInfo(id, &n, ti) && n == 1 && ti[0].pointerInfo.pointerId == id) pframe_ok = 1;
        break;                                   /* on to DefWindowProc: the mouse */
    }
    case WM_POINTERUPDATE_: pupdate++; break;
    case WM_POINTERUP_: pup++; break;
    case WM_LBUTTONDOWN:
        lbdown++;
        if ((short)LOWORD(lp) >= 0 && (short)LOWORD(lp) < sw / 2) lb_where_ok = 1;   /* (client coordinates) */
        return 0;
    case WM_LBUTTONUP: lbup++; return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

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

static BOOL left_done(void) { return ups >= 2; }
static BOOL right_done(void) { return pup >= 1 && lbup >= 1; }

static HWND make(const char *cls, WNDPROC proc, int x)
{
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = cls;
    wc.hbrBackground = (HBRUSH)GetStockObject(x ? GRAY_BRUSH : WHITE_BRUSH);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    return CreateWindowExA(0, cls, cls, WS_POPUP | WS_VISIBLE, x, 0, sw / 2, sh, NULL, NULL, wc.hInstance, NULL);
}

int main(void)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    pRegisterTouchWindow = (RegisterTouchWindowFn)GetProcAddress(u, "RegisterTouchWindow");
    pGetTouchInputInfo = (GetTouchInputInfoFn)GetProcAddress(u, "GetTouchInputInfo");
    pCloseTouchInputHandle = (CloseTouchInputHandleFn)GetProcAddress(u, "CloseTouchInputHandle");
    pGetPointerInfo = (GetPointerInfoFn)GetProcAddress(u, "GetPointerInfo");
    pGetPointerType = (GetPointerTypeFn)GetProcAddress(u, "GetPointerType");
    pGetPointerFrameTouchInfo = (GetPointerFrameTouchInfoFn)GetProcAddress(u, "GetPointerFrameTouchInfo");
    if (!pRegisterTouchWindow || !pGetTouchInputInfo || !pCloseTouchInputHandle || !pGetPointerInfo || !pGetPointerType ||
        !pGetPointerFrameTouchInfo) {
        printf("FAIL: user32 lacks the touch functions\n");
        return 1;
    }
    int dig = GetSystemMetrics(94), most = GetSystemMetrics(95);    /* SM_DIGITIZER, SM_MAXIMUMTOUCHES */
    printf("SM_DIGITIZER 0x%x, SM_MAXIMUMTOUCHES %d\n", dig, most);
    check("GetSystemMetrics: an integrated multi-touch screen, ready", (dig & 0xC1) == 0xC1 && most >= 2);

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    HWND left = make("touchleft", left_proc, 0), right = make("touchright", right_proc, sw / 2);
    if (!left || !right) { printf("FAIL: CreateWindowEx\n"); return 1; }
    check("RegisterTouchWindow", pRegisterTouchWindow(left, 0));
    MSG msg;
    for (int i = 0; i < 25; i++) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(20);
    }

    printf("touchtest: two fingers on the left, move them, lift them\n");
    fflush(stdout);
    pump(left_done, 30);
    printf("WM_TOUCH: %d messages, %d with two contacts down, %d moves, %d ups, %d primary\n",
           touch_msgs, two_down, moves, ups, primaries);
    check("WM_TOUCH: two contacts touching down in one frame, different ids", two_down >= 1 && ids_ok);
    check("WM_TOUCH: the contacts moved", moves >= 2);
    check("WM_TOUCH: both lifted", ups >= 2);
    check("WM_TOUCH: exactly one primary contact", primaries == 1);
    check("WM_TOUCH: positions in the left window (hundredths of a pixel)", left_half);
    check("WM_TOUCH: none of it reached the right window", pdown == 0);

    printf("touchtest: tap on the right\n");
    fflush(stdout);
    pump(right_done, 30);
    printf("WM_POINTER: %d down, %d update, %d up; mouse: %d down, %d up\n", pdown, pupdate, pup, lbdown, lbup);
    check("WM_POINTERDOWN and WM_POINTERUP", pdown == 1 && pup == 1);
    check("GetPointerType: PT_TOUCH", ptype_ok);
    check("GetPointerInfo: down, primary, the target window and position", pinfo_ok);
    check("GetPointerFrameTouchInfo: the frame's one contact", pframe_ok);
    check("DefWindowProc promotes the primary contact to WM_LBUTTONDOWN/UP (client coordinates)",
          lbdown == 1 && lbup == 1 && lb_where_ok);
    check("WM_TOUCH stayed with the left window", touch_msgs > 0 && ups == 2);

    DestroyWindow(left);
    DestroyWindow(right);
    printf("touchtest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
