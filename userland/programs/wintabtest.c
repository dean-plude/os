/*
 * wintabtest.exe — pen tablets through wintab32.dll, as GTK 2 uses it
 * (gdk/win32/gdkinput-win32.c loads the DLL and finds its functions by
 * name):
 *
 *  - with no pen, WTInfo(0, 0, NULL) is 0 and WTOpen fails;
 *  - a synthetic pen (CreateSyntheticPointerDevice(PT_PEN)) makes a
 *    tablet: one device, two cursors (pen, eraser), pressure 0-1023, and
 *    SM_DIGITIZER says a pen is there;
 *  - a context opened the way GTK opens it (relative buttons, Y growing
 *    downward, messages) gets WT_PROXIMITY, WT_PACKET for each injected
 *    report with its position, pressure and button changes (WTPacket), and
 *    WT_CSRCHANGE when the eraser end is used; the pen moves the pointer
 *    and its tip clicks the window;
 *  - a second context in absolute mode reads its queue with
 *    WTQueuePacketsEx, WTDataPeek and WTPacketsGet;
 *  - the pen's tilt and barrel rotation (penMask's PEN_MASK_TILT_X/Y and
 *    ROTATION) come out as each packet's ORIENTATION: azimuth, altitude
 *    (negative for the eraser) and twist, and DVC_ORIENTATION says the
 *    device reports all three;
 *  - when the pen goes, so does the tablet.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* The Wintab spec's (wintab.h, pktdef.h) */
typedef DWORD WTPKT;
typedef DWORD FIX32;
typedef HANDLE HCTX;
typedef struct { LONG axMin, axMax; UINT axUnits; FIX32 axResolution; } AXIS;
typedef struct {
    char  lcName[40];
    UINT  lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
    WTPKT lcPktData, lcPktMode, lcMoveMask;
    DWORD lcBtnDnMask, lcBtnUpMask;
    LONG  lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
    LONG  lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
    FIX32 lcSensX, lcSensY, lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
    FIX32 lcSysSensX, lcSysSensY;
} LOGCONTEXTA;
typedef struct { int orAzimuth, orAltitude, orTwist; } ORIENTATION;

#define WTI_INTERFACE 1
#define WTI_DEFSYSCTX 4
#define WTI_DEVICES   100
#define WTI_CURSORS   200
#define WTI_DSCTXS    500
#define IFC_SPECVERSION 2
#define IFC_NDEVICES  4
#define IFC_NCURSORS  5
#define DVC_NAME      1
#define DVC_NCSRTYPES 3
#define DVC_FIRSTCSR  4
#define DVC_X         12
#define DVC_Y         13
#define DVC_NPRESSURE 15
#define DVC_ORIENTATION 17
#define CSR_NAME      1
#define CSR_ACTIVE    2
#define CSR_TYPE      20
#define PK_CONTEXT    0x0001
#define PK_STATUS     0x0002
#define PK_SERIAL_NUMBER 0x0010
#define PK_CURSOR     0x0020
#define PK_BUTTONS    0x0040
#define PK_X          0x0080
#define PK_Y          0x0100
#define PK_NORMAL_PRESSURE 0x0400
#define PK_ORIENTATION 0x1000
#define CXO_SYSTEM    0x0001
#define CXO_MESSAGES  0x0004
#define CXO_CSRMESSAGES 0x0008
#define WT_DEFBASE    0x7FF0
#define WT_PACKET     (WT_DEFBASE + 0)
#define WT_PROXIMITY  (WT_DEFBASE + 5)
#define WT_CSRCHANGE  (WT_DEFBASE + 7)
#define TBN_UP        1
#define TBN_DOWN      2
#define TPS_INVERT    0x0010

/* GTK's PACKET: PACKETDATA (PK_CONTEXT | PK_CURSOR | PK_BUTTONS | PK_X |
 * PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION), PACKETMODE PK_BUTTONS */
typedef struct {
    HCTX  pkContext;
    UINT  pkCursor;
    DWORD pkButtons;
    LONG  pkX, pkY;
    UINT  pkNormalPressure;
    ORIENTATION pkOrientation;
} GtkPacket;
/* the absolute context's: PK_STATUS | PK_SERIAL_NUMBER | PK_BUTTONS | PK_X | PK_NORMAL_PRESSURE */
typedef struct { UINT pkStatus, pkSerialNumber; DWORD pkButtons; LONG pkX; UINT pkNormalPressure; } AbsPacket;

/* Synthetic pointers (winuser.h, Windows 10 1809) */
#define PT_PEN 3
#define POINTER_FLAG_INRANGE   0x00002
#define POINTER_FLAG_INCONTACT 0x00004
#define POINTER_FLAG_DOWN      0x10000
#define POINTER_FLAG_UPDATE    0x20000
#define POINTER_FLAG_UP        0x40000
#define PEN_FLAG_BARREL 1
#define PEN_FLAG_ERASER 4
typedef struct {
    DWORD pointerType;
    UINT32 pointerId, frameId, pointerFlags;
    HANDLE sourceDevice;
    HWND hwndTarget;
    POINT ptPixelLocation, ptHimetricLocation, ptPixelLocationRaw, ptHimetricLocationRaw;
    DWORD dwTime;
    UINT32 historyCount;
    INT32 InputData;
    DWORD dwKeyStates;
    UINT64 PerformanceCount;
    int ButtonChangeType;
} PTR_INFO;
typedef struct { PTR_INFO pointerInfo; UINT32 touchFlags, touchMask; RECT rcContact, rcContactRaw; UINT32 orientation, pressure; } PTR_TOUCH;
typedef struct { PTR_INFO pointerInfo; UINT32 penFlags, penMask, pressure, rotation; INT32 tiltX, tiltY; } PTR_PEN;
typedef struct { DWORD type; union { PTR_TOUCH touchInfo; PTR_PEN penInfo; }; } PTR_TYPE_INFO;

typedef UINT (WINAPI *WTInfoA_t)(UINT, UINT, LPVOID);
typedef HCTX (WINAPI *WTOpenA_t)(HWND, LOGCONTEXTA *, BOOL);
typedef BOOL (WINAPI *WTClose_t)(HCTX);
typedef BOOL (WINAPI *WTPacket_t)(HCTX, UINT, LPVOID);
typedef int  (WINAPI *WTPacketsGet_t)(HCTX, int, LPVOID);
typedef int  (WINAPI *WTDataPeek_t)(HCTX, UINT, UINT, int, LPVOID, LPINT);
typedef BOOL (WINAPI *WTQueuePacketsEx_t)(HCTX, UINT *, UINT *);
typedef BOOL (WINAPI *WTQueueSizeSet_t)(HCTX, int);
typedef BOOL (WINAPI *WTOverlap_t)(HCTX, BOOL);
typedef HANDLE (WINAPI *CreateSyn_t)(DWORD, ULONG, DWORD);
typedef BOOL (WINAPI *Inject_t)(HANDLE, const PTR_TYPE_INFO *, UINT32);
typedef void (WINAPI *DestroySyn_t)(HANDLE);

static WTInfoA_t pInfo;
static WTOpenA_t pOpen;
static WTClose_t pClose;
static WTPacket_t pPacket;
static WTPacketsGet_t pPacketsGet;
static WTDataPeek_t pDataPeek;
static WTQueuePacketsEx_t pQueueEx;
static WTQueueSizeSet_t pQueueSize;
static WTOverlap_t pOverlap;

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

/* What the window received */
#define MAXP 64
static GtkPacket g_pk[MAXP];
static int g_np, g_prox_in, g_prox_out, g_csr, g_clicks, g_bad_packet;
static HCTX g_ctx;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WT_PACKET) {
        GtkPacket p;
        if (pPacket((HCTX)lp, (UINT)wp, &p)) { if (g_np < MAXP) g_pk[g_np++] = p; }
        else g_bad_packet++;
        return 0;
    }
    if (msg == WT_PROXIMITY) { if (LOWORD(lp)) g_prox_in++; else g_prox_out++; return 0; }
    if (msg == WT_CSRCHANGE) { g_csr++; return 0; }
    if (msg == WM_LBUTTONDOWN) { g_clicks++; return 0; }
    return DefWindowProcW(h, msg, wp, lp);
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

static Inject_t pInject;
static HANDLE g_pen;

/* One pen report at screen (x, y), upright or tilted and turned */
static void pen_tilted(int x, int y, UINT32 flags, UINT32 pressure, UINT32 penflags, UINT32 mask, int tx, int ty, UINT32 rot);
static void pen(int x, int y, UINT32 flags, UINT32 pressure, UINT32 penflags)
{
    pen_tilted(x, y, flags, pressure, penflags, 1, 0, 0, 0);    /* PEN_MASK_PRESSURE */
}

static void pen_tilted(int x, int y, UINT32 flags, UINT32 pressure, UINT32 penflags, UINT32 mask, int tx, int ty, UINT32 rot)
{
    PTR_TYPE_INFO in;
    memset(&in, 0, sizeof(in));
    in.type = PT_PEN;
    in.penInfo.pointerInfo.pointerType = PT_PEN;
    in.penInfo.pointerInfo.pointerFlags = flags;
    in.penInfo.pointerInfo.ptPixelLocation.x = x;
    in.penInfo.pointerInfo.ptPixelLocation.y = y;
    in.penInfo.penFlags = penflags;
    in.penInfo.pressure = pressure;
    in.penInfo.penMask = mask;
    in.penInfo.tiltX = tx;
    in.penInfo.tiltY = ty;
    in.penInfo.rotation = rot;
    if (!pInject(g_pen, &in, 1)) printf("  InjectSyntheticPointerInput failed (%lu)\n", (unsigned long)GetLastError());
    pump(120);
}

int main(void)
{
    HMODULE wt = LoadLibraryA("wintab32.dll");
    check(wt != NULL, "LoadLibrary(wintab32.dll)");
    if (!wt) { printf("wintabtest: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    pInfo = (WTInfoA_t)GetProcAddress(wt, "WTInfoA");
    pOpen = (WTOpenA_t)GetProcAddress(wt, "WTOpenA");
    pClose = (WTClose_t)GetProcAddress(wt, "WTClose");
    pPacket = (WTPacket_t)GetProcAddress(wt, "WTPacket");
    pPacketsGet = (WTPacketsGet_t)GetProcAddress(wt, "WTPacketsGet");
    pDataPeek = (WTDataPeek_t)GetProcAddress(wt, "WTDataPeek");
    pQueueEx = (WTQueuePacketsEx_t)GetProcAddress(wt, "WTQueuePacketsEx");
    pQueueSize = (WTQueueSizeSet_t)GetProcAddress(wt, "WTQueueSizeSet");
    pOverlap = (WTOverlap_t)GetProcAddress(wt, "WTOverlap");
    check(pInfo && pOpen && pClose && pPacket && pPacketsGet && pDataPeek && pQueueEx && pQueueSize && pOverlap &&
          GetProcAddress(wt, "WTInfoW") && GetProcAddress(wt, (LPCSTR)20) == (FARPROC)pInfo,
          "wintab32 exports GTK's and Qt's functions (WTInfoA is ordinal 20)");

    /* 1. No pen: no tablet */
    HWND scratch = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
    LOGCONTEXTA lc;
    memset(&lc, 0, sizeof(lc));
    UINT any = pInfo(0, 0, NULL);
    check(any == 0, "no pen: WTInfo(0, 0, NULL) is 0 (no Wintab)");
    pInfo(WTI_DEFSYSCTX, 0, &lc);
    check(pOpen(scratch, &lc, TRUE) == NULL, "no pen: WTOpen fails");
    check(!(GetSystemMetrics(94 /* SM_DIGITIZER */) & 0x08), "no pen: SM_DIGITIZER has no NID_EXTERNAL_PEN");
    DestroyWindow(scratch);

    /* 2. A synthetic pen */
    HMODULE u = GetModuleHandleA("user32.dll");
    CreateSyn_t pCreate = (CreateSyn_t)GetProcAddress(u, "CreateSyntheticPointerDevice");
    pInject = (Inject_t)GetProcAddress(u, "InjectSyntheticPointerInput");
    DestroySyn_t pDestroy = (DestroySyn_t)GetProcAddress(u, "DestroySyntheticPointerDevice");
    g_pen = pCreate && pInject && pDestroy ? pCreate(PT_PEN, 1, 1) : NULL;
    check(g_pen != NULL, "CreateSyntheticPointerDevice(PT_PEN)");
    if (!g_pen) { printf("wintabtest: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    check(GetSystemMetrics(94) & 0x08, "SM_DIGITIZER: NID_EXTERNAL_PEN");
    UINT ndev = 0, ncsr = 0, first = 99, ncsrtypes = 0, act = 0, type0 = 0, type1 = 0;
    WORD spec = 0;
    AXIS ax = { 0 }, ay = { 0 }, ap = { 0 }, ao[3];
    char name[64] = "", c0[64] = "", c1[64] = "";
    check(pInfo(0, 0, NULL) >= sizeof(LOGCONTEXTA), "a pen: WTInfo(0, 0, NULL) gives the largest answer's size");
    pInfo(WTI_INTERFACE, IFC_SPECVERSION, &spec);
    pInfo(WTI_INTERFACE, IFC_NDEVICES, &ndev);
    pInfo(WTI_INTERFACE, IFC_NCURSORS, &ncsr);
    pInfo(WTI_DEVICES, DVC_NAME, name);
    pInfo(WTI_DEVICES, DVC_NCSRTYPES, &ncsrtypes);
    pInfo(WTI_DEVICES, DVC_FIRSTCSR, &first);
    pInfo(WTI_DEVICES, DVC_X, &ax);
    pInfo(WTI_DEVICES, DVC_Y, &ay);
    pInfo(WTI_DEVICES, DVC_NPRESSURE, &ap);
    memset(ao, 0, sizeof(ao));
    UINT osz = pInfo(WTI_DEVICES, DVC_ORIENTATION, ao);
    pInfo(WTI_CURSORS + 0, CSR_NAME, c0);
    pInfo(WTI_CURSORS + 1, CSR_NAME, c1);
    pInfo(WTI_CURSORS + 0, CSR_ACTIVE, &act);
    pInfo(WTI_CURSORS + 0, CSR_TYPE, &type0);
    pInfo(WTI_CURSORS + 1, CSR_TYPE, &type1);
    printf("  device \"%s\", spec %x, cursors \"%s\" and \"%s\"\n", name, spec, c0, c1);
    check(spec >= 0x0101 && ndev == 1 && ncsr == 2 && ncsrtypes == 2 && first == 0, "one device with two cursors");
    check(ax.axMax == 65535 && ay.axMax == 65535 && ap.axMin == 0 && ap.axMax == 1023, "X and Y 0-65535, pressure 0-1023");
    check(act && c0[0] && c1[0] && type0 == 0x0802 && type1 == 0x080A, "the cursors: an active pen and its eraser");
    check(pInfo(WTI_DSCTXS, 0, &lc) == sizeof(lc) && (lc.lcOptions & CXO_SYSTEM), "the device's system context");
    printf("  orientation axes: azimuth %ld-%ld, altitude %ld-%ld, twist %ld-%ld\n", ao[0].axMin, ao[0].axMax,
           ao[1].axMin, ao[1].axMax, ao[2].axMin, ao[2].axMax);
    check(osz == sizeof(ao) && ao[0].axMin == 0 && ao[0].axMax == 3599 && ao[1].axMin == -900 && ao[1].axMax == 900 &&
          ao[2].axMax == 3599, "DVC_ORIENTATION: azimuth, altitude and twist (a synthetic pen has tilt and rotation)");

    /* 3. A window and GTK's context */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"WintabTest";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"WintabTest", L"Wintab test", WS_OVERLAPPEDWINDOW, 100, 100, 600, 400, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    SetForegroundWindow(h);
    pump(300);

    lc.lcOptions |= CXO_MESSAGES | CXO_CSRMESSAGES;
    lc.lcStatus = 0;
    lc.lcMsgBase = WT_DEFBASE;
    lc.lcPktRate = 0;
    lc.lcPktData = PK_CONTEXT | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION;
    lc.lcPktMode = PK_BUTTONS;
    lc.lcMoveMask = lc.lcPktData;
    lc.lcBtnUpMask = lc.lcBtnDnMask = ~0u;
    lc.lcOutOrgX = ax.axMin; lc.lcOutOrgY = ay.axMin;
    lc.lcOutExtX = ax.axMax - ax.axMin + 1;
    lc.lcOutExtY = -(ay.axMax - ay.axMin + 1);               /* "we want Y growing downward" */
    g_ctx = pOpen(h, &lc, TRUE);
    check(g_ctx != NULL, "WTOpen with GTK's context");
    if (!g_ctx) { pDestroy(g_pen); printf("wintabtest: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    pOverlap(g_ctx, TRUE);
    int qs = 0;
    for (int i = 32; i >= 1; i >>= 1) if (pQueueSize(g_ctx, i)) { qs = i; break; }
    check(qs == 32, "WTQueueSizeSet(32)");

    /* an absolute-mode context beside it, read without messages */
    LOGCONTEXTA la = lc;
    la.lcOptions &= ~(UINT)(CXO_MESSAGES | CXO_CSRMESSAGES);
    la.lcPktData = PK_STATUS | PK_SERIAL_NUMBER | PK_BUTTONS | PK_X | PK_NORMAL_PRESSURE;
    la.lcPktMode = 0;
    la.lcOutOrgX = 0; la.lcOutExtX = 1000;                   /* X scaled to 0-999 */
    HCTX abs = pOpen(h, &la, TRUE);
    check(abs != NULL, "a second context (absolute mode)");
    if (abs) pQueueSize(abs, 32);

    /* 4. The pen: near, down lightly, harder, up, the eraser, away */
    RECT wr;
    GetClientRect(h, &wr);
    POINT p0 = { 150, 100 };
    ClientToScreen(h, &p0);
    int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN), sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN), sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    pen(p0.x, p0.y, POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, 0, 0);
    pen(p0.x, p0.y, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN, 512, 0);
    pen(p0.x + 40, p0.y + 20, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE, 1024, PEN_FLAG_BARREL);
    pen(p0.x + 40, p0.y + 20, POINTER_FLAG_INRANGE | POINTER_FLAG_UP, 0, 0);
    POINT cur;
    GetCursorPos(&cur);
    pen(p0.x + 80, p0.y, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN, 700, PEN_FLAG_ERASER);
    pen(p0.x + 80, p0.y, 0, 0, PEN_FLAG_ERASER);
    pump(300);

    printf("  %d packets, proximity in %d out %d, cursor changes %d, clicks %d\n", g_np, g_prox_in, g_prox_out, g_csr, g_clicks);
    for (int i = 0; i < g_np; i++)
        printf("  packet %d: cursor %u buttons %u:%u x %ld y %ld pressure %u\n", i, g_pk[i].pkCursor,
               HIWORD(g_pk[i].pkButtons), LOWORD(g_pk[i].pkButtons), g_pk[i].pkX, g_pk[i].pkY, g_pk[i].pkNormalPressure);
    check(g_np == 5 && !g_bad_packet, "a WT_PACKET for each report in range, each found by WTPacket");
    check(g_prox_in >= 1 && g_prox_out >= 1, "WT_PROXIMITY when the pen comes near and goes away");
    check(g_csr >= 2, "WT_CSRCHANGE when the pen comes in and when its eraser end is used");
    if (g_np >= 5) {
        LONG wx = (LONG)((long long)(p0.x - sx) * 65535 / (sw - 1)), wy = (LONG)((long long)(p0.y - sy) * 65535 / (sh - 1));
        int tol = 65536 / sw * 2 + 2;
        check(g_pk[0].pkContext == g_ctx && g_pk[0].pkNormalPressure == 0 && g_pk[0].pkButtons == 0, "hovering: no pressure, no button");
        check(g_pk[1].pkX >= wx - tol && g_pk[1].pkX <= wx + tol && g_pk[1].pkY >= wy - tol && g_pk[1].pkY <= wy + tol,
              "X and Y: the pen's place on the desktop, Y downward");
        check(g_pk[1].pkButtons == MAKELONG(0, TBN_DOWN) && g_pk[1].pkNormalPressure >= 505 && g_pk[1].pkNormalPressure <= 515,
              "the tip goes down (TBN_DOWN, button 0) at half pressure");
        check(g_pk[2].pkButtons == MAKELONG(1, TBN_DOWN) && g_pk[2].pkNormalPressure == 1023 && g_pk[2].pkX > g_pk[1].pkX &&
              g_pk[2].pkY > g_pk[1].pkY, "pressed hard with the barrel button, moving right and down");
        check(HIWORD(g_pk[3].pkButtons) == TBN_UP && g_pk[3].pkNormalPressure == 0, "the tip comes up");
        check(g_pk[4].pkCursor == 1 && g_pk[4].pkNormalPressure > 600 && g_pk[4].pkNormalPressure < 720, "the eraser: cursor 1, with pressure");
        check(g_pk[0].pkOrientation.orAltitude == 900, "orientation: upright");
    }
    check(cur.x >= p0.x + 39 && cur.x <= p0.x + 41 && cur.y >= p0.y + 19 && cur.y <= p0.y + 21, "the pen moves the pointer");
    check(g_clicks >= 1, "the tip clicks the window (WM_LBUTTONDOWN)");

    if (abs) {
        UINT o = 0, n = 0;
        check(pQueueEx(abs, &o, &n) && n > o && n - o + 1 == 5, "absolute context: five packets queued (WTQueuePacketsEx)");
        AbsPacket ap2[8];
        int got = 0;
        memset(ap2, 0xCC, sizeof(ap2));
        pDataPeek(abs, o + 1, o + 2, 8, ap2, &got);
        check(got == 2 && ap2[0].pkSerialNumber == o + 1 && ap2[1].pkSerialNumber == o + 2, "WTDataPeek: the packets numbered between");
        got = pPacketsGet(abs, 8, ap2);
        check(got == 5 && ap2[1].pkButtons == 1 && ap2[2].pkButtons == 3 && ap2[3].pkButtons == 0 && (ap2[4].pkStatus & TPS_INVERT) &&
              ap2[1].pkX >= 0 && ap2[1].pkX < 1000, "WTPacketsGet: absolute buttons, the eraser's TPS_INVERT, X in the output extent");
        check(pPacketsGet(abs, 8, ap2) == 0, "the queue is empty after");
        pClose(abs);
    }
    /* 4b. Tilted and turned: leaning 30 degrees right turned a quarter,
     * 45 away (toward the tablet's top), 30 right and 30 toward the user
     * with the eraser turned 359 degrees */
    g_np = 0;
    pen_tilted(p0.x, p0.y, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN, 300, 0, 1 | 2 | 4 | 8, 30, 0, 90);
    pen_tilted(p0.x + 10, p0.y, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE, 300, 0, 1 | 2 | 4 | 8, 0, -45, 0);
    pen_tilted(p0.x + 20, p0.y, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE, 300, PEN_FLAG_ERASER, 1 | 2 | 4 | 8, 30, 30, 359);
    pen(p0.x + 20, p0.y, 0, 0, 0);
    pump(300);
    for (int i = 0; i < g_np; i++)
        printf("  tilted packet %d: cursor %u pressure %u azimuth %d altitude %d twist %d\n", i, g_pk[i].pkCursor,
               g_pk[i].pkNormalPressure, g_pk[i].pkOrientation.orAzimuth, g_pk[i].pkOrientation.orAltitude,
               g_pk[i].pkOrientation.orTwist);
    check(g_np == 3, "a WT_PACKET for each tilted report");
    if (g_np >= 3) {
        const ORIENTATION *o0 = &g_pk[0].pkOrientation, *o1 = &g_pk[1].pkOrientation, *o2 = &g_pk[2].pkOrientation;
        check(o0->orAzimuth == 900 && o0->orAltitude == 600 && o0->orTwist == 900,
              "30 degrees right: azimuth 90 (east), altitude 60, twist 90");
        check((o1->orAzimuth == 0 || o1->orAzimuth == 3599) && o1->orAltitude >= 449 && o1->orAltitude <= 451 && o1->orTwist == 0,
              "45 degrees toward the top: azimuth 0 (north), altitude 45, no twist");
        check(g_pk[2].pkCursor == 1 && o2->orAzimuth >= 1348 && o2->orAzimuth <= 1352 && o2->orAltitude <= -506 &&
              o2->orAltitude >= -510 && o2->orTwist == 3590,
              "the eraser leaning right and toward the user: azimuth 135, altitude -50.8 (negative), twist 359");
    }

    check(pClose(g_ctx), "WTClose");

    /* 5. The pen goes */
    pDestroy(g_pen);
    check(pInfo(0, 0, NULL) == 0, "no pen again: WTInfo(0, 0, NULL) is 0");
    DestroyWindow(h);
    printf("wintabtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
