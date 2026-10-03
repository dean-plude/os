/*
 * pointer.c — touch: WM_POINTER* and WM_TOUCH; gestures, pens and raw
 * input are not there
 *
 * The desktop sends a top-level window each touch contact that went down
 * in its client area (WM_NOVA_TOUCH: the contact's slot, down/move/up,
 * primary, the frame's last; its screen position).  A contact belongs to
 * the window under the point where it touched down.  When a frame is
 * complete, a window that called RegisterTouchWindow (or whose parent
 * did) gets one WM_TOUCH with the frame's contacts (GetTouchInputInfo);
 * any other gets WM_POINTERDOWN / UPDATE / UP per contact
 * (GetPointerInfo, GetPointerTouchInfo, GetPointerFrameTouchInfo).
 * Passed to DefWindowProc, the primary contact's messages become the
 * mouse's (WM_LBUTTONDOWN, WM_MOUSEMOVE, WM_LBUTTONUP), as Windows'
 * "promotion" makes them, so programs that know only the mouse work by
 * touch.  Pointer ids: 1 is the mouse, touch contacts are slot + 2.
 */
#include "u32.h"

#define PT_POINTER 1
#define PT_TOUCH   2
#define PT_MOUSE   4

/* desktop (kernel/wm/wm.h WM_TOUCH_*) */
#define K_DOWN     1
#define K_MOVE     2
#define K_UP       4
#define K_PRIMARY  8

#define POINTER_FLAG_NEW         0x00001
#define POINTER_FLAG_INRANGE     0x00002
#define POINTER_FLAG_INCONTACT   0x00004
#define POINTER_FLAG_FIRSTBUTTON 0x00010
#define POINTER_FLAG_PRIMARY     0x02000
#define POINTER_FLAG_CONFIDENCE  0x04000
#define POINTER_FLAG_DOWN        0x10000
#define POINTER_FLAG_UPDATE      0x20000
#define POINTER_FLAG_UP          0x40000

#define WM_TOUCH          0x0240
#define WM_POINTERUPDATE  0x0245
#define WM_POINTERDOWN    0x0246
#define WM_POINTERUP      0x0247

#define TOUCHEVENTF_MOVE    0x01
#define TOUCHEVENTF_DOWN    0x02
#define TOUCHEVENTF_UP      0x04
#define TOUCHEVENTF_INRANGE 0x08
#define TOUCHEVENTF_PRIMARY 0x10

#define NPTR 10

typedef struct {
    LONG      x, y;                 /* hundredths of a pixel, screen */
    HANDLE    hSource;
    DWORD     dwID, dwFlags, dwMask, dwTime;
    ULONG_PTR dwExtraInfo;
    DWORD     cxContact, cyContact;
} TouchInput;

typedef struct {
    DWORD     pointerType;
    UINT32    pointerId, frameId, pointerFlags;
    HANDLE    sourceDevice;
    HWND      hwndTarget;
    POINT     ptPixelLocation, ptHimetricLocation, ptPixelLocationRaw, ptHimetricLocationRaw;
    DWORD     dwTime;
    UINT32    historyCount;
    INT32     InputData;
    DWORD     dwKeyStates;
    UINT64    PerformanceCount;
    int       ButtonChangeType;
} PointerInfo;

typedef struct {
    PointerInfo pointerInfo;
    UINT32    touchFlags, touchMask;
    RECT      rcContact, rcContactRaw;
    UINT32    orientation, pressure;
} PointerTouchInfo;

typedef struct {
    int    used, down, changed;
    POINT  pt;
    HWND   target;                  /* the window it touched down on */
    HWND   touch;                   /* ... or the RegisterTouchWindow one it goes to (WM_TOUCH) */
    int    kf;                      /* K_* of its last change */
    DWORD  flags;                   /* POINTER_FLAG_* now */
    DWORD  time;
    UINT32 frame;
} Ptr;

static Ptr    g_ptr[NPTR];
static UINT32 g_frame;

/* WM_TOUCH's handles: a frame's inputs each, until CloseTouchInputHandle
 * (or 16 frames later) */
#define NSETS 16
static struct { UINT32 serial; UINT n; TouchInput in[NPTR]; } g_set[NSETS];
static UINT32 g_serial;

static Ptr *ptr_of(UINT32 id)
{
    if (id < 2 || id >= 2 + NPTR || !g_ptr[id - 2].used) return NULL;
    return &g_ptr[id - 2];
}

static Wnd *touch_window(Wnd *w)
{
    for (; w; w = w->parent) if (w->flags & WF_TOUCH) return w;
    return NULL;
}

static void deliver_frame(void)
{
    g_frame++;
    /* WM_TOUCH: a message per window with the frame's contacts */
    int done[NPTR] = { 0 };
    for (int s = 0; s < NPTR; s++) {
        Ptr *p = &g_ptr[s];
        if (!p->changed || !p->touch || done[s]) continue;
        Wnd *t = W_quiet(p->touch);
        UINT32 serial = ++g_serial;
        int k = serial % NSETS;
        g_set[k].serial = serial;
        g_set[k].n = 0;
        for (int j = s; j < NPTR; j++) {
            Ptr *q = &g_ptr[j];
            if (!q->changed || q->touch != p->touch) continue;
            done[j] = 1;
            TouchInput *in = &g_set[k].in[g_set[k].n++];
            memset(in, 0, sizeof(*in));
            in->x = q->pt.x * 100; in->y = q->pt.y * 100;
            in->dwID = (DWORD)j + 2;
            in->dwFlags = (q->kf & K_DOWN ? TOUCHEVENTF_DOWN | TOUCHEVENTF_INRANGE : q->kf & K_UP ? TOUCHEVENTF_UP :
                           TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE) | (q->kf & K_PRIMARY ? TOUCHEVENTF_PRIMARY : 0);
            in->dwTime = q->time;
        }
        if (t) input_queue(t, WM_TOUCH, g_set[k].n, (LPARAM)(ULONG_PTR)(0x7A000000u + serial), p->time);
    }
    /* WM_POINTER*: a message per contact */
    for (int s = 0; s < NPTR; s++) {
        Ptr *p = &g_ptr[s];
        if (!p->changed) continue;
        p->changed = 0;
        p->frame = g_frame;
        if (p->touch) continue;
        Wnd *t = W_quiet(p->target);
        if (!t) continue;
        UINT msg = p->kf & K_DOWN ? WM_POINTERDOWN : p->kf & K_UP ? WM_POINTERUP : WM_POINTERUPDATE;
        input_queue(t, msg, MAKEWPARAM(s + 2, p->flags & 0xFFFF), MAKELPARAM(p->pt.x, p->pt.y), p->time);
    }
}

void touch_from_kernel(Wnd *top, const MSG *km)
{
    int slot = (int)(km->wParam & 0xFF), kf = (int)((km->wParam >> 8) & 0xFF);
    if (slot >= NPTR) return;
    LOCK();
    Ptr *p = &g_ptr[slot];
    POINT pt = { (short)LOWORD(km->lParam), (short)HIWORD(km->lParam) };
    if (kf & K_DOWN) {
        int hit;
        Wnd *w = (top->style & WS_DISABLED) ? NULL : input_hit(top, pt, &hit);
        while (w && w->parent && (w->style & WS_DISABLED)) w = w->parent;
        if (w && (w->style & WS_DISABLED)) w = NULL;
        Wnd *tw = w ? touch_window(w) : NULL;
        p->used = 1;
        p->target = w ? w->h : 0;
        p->touch = tw ? tw->h : 0;
    }
    if (p->used) {
        p->down = !(kf & K_UP);
        p->pt = pt;
        p->kf = kf;
        p->time = km->time ? km->time : GetTickCount();
        DWORD prim = kf & K_PRIMARY ? POINTER_FLAG_PRIMARY : 0;
        if (kf & K_DOWN)
            p->flags = POINTER_FLAG_NEW | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON |
                       POINTER_FLAG_CONFIDENCE | POINTER_FLAG_DOWN | prim;
        else if (kf & K_UP)
            p->flags = POINTER_FLAG_UP | prim;
        else
            p->flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON |
                       POINTER_FLAG_CONFIDENCE | POINTER_FLAG_UPDATE | prim;
        p->changed = 1;
    }
    if (km->wParam & 0x10000) deliver_frame();
    UNLOCK();
}

/* DefWindowProc: the primary contact works the mouse */
static void promote(Wnd *w, int down, int up, POINT pt)
{
    Wnd *top = top_of(w);
    if (!top) return;
    if (down) input_mouse(top, WM_MOUSEMOVE, 0, pt);
    input_mouse(top, down ? WM_LBUTTONDOWN : up ? WM_LBUTTONUP : WM_MOUSEMOVE, up ? 0 : MK_LBUTTON, pt);
}

LRESULT touch_default(Wnd *w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TOUCH) {
        UINT32 serial = (UINT32)((ULONG_PTR)lp - 0x7A000000u);
        int k = serial % NSETS;
        if (g_set[k].serial != serial) return 0;
        for (UINT i = 0; i < g_set[k].n; i++) {
            const TouchInput *in = &g_set[k].in[i];
            if (!(in->dwFlags & TOUCHEVENTF_PRIMARY)) continue;
            POINT pt = { in->x / 100, in->y / 100 };
            promote(w, (in->dwFlags & TOUCHEVENTF_DOWN) != 0, (in->dwFlags & TOUCHEVENTF_UP) != 0, pt);
        }
        g_set[k].serial = 0;                /* (closed: DefWindowProc had it) */
        return 0;
    }
    /* WM_POINTERDOWN, UPDATE, UP */
    if (HIWORD(wp) & POINTER_FLAG_PRIMARY) {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        promote(w, msg == WM_POINTERDOWN, msg == WM_POINTERUP, pt);
    }
    return 0;
}

USERAPI BOOL GetPointerType(UINT32 id, DWORD *type)
{
    if (!type) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (id == 1) { *type = PT_MOUSE; return TRUE; }
    if (!ptr_of(id)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *type = PT_TOUCH;
    return TRUE;
}

static void fill_info(UINT32 id, const Ptr *p, PointerInfo *pi)
{
    memset(pi, 0, sizeof(*pi));
    pi->pointerType = PT_TOUCH;
    pi->pointerId = id;
    pi->frameId = p->frame;
    pi->pointerFlags = p->flags;
    pi->hwndTarget = p->touch ? p->touch : p->target;
    pi->ptPixelLocation = pi->ptPixelLocationRaw = p->pt;
    pi->ptHimetricLocation.x = pi->ptHimetricLocationRaw.x = MulDiv(p->pt.x, 2540, 96);
    pi->ptHimetricLocation.y = pi->ptHimetricLocationRaw.y = MulDiv(p->pt.y, 2540, 96);
    pi->dwTime = p->time;
    pi->historyCount = 1;
    pi->ButtonChangeType = p->kf & K_DOWN ? 1 : p->kf & K_UP ? 2 : 0;   /* POINTER_CHANGE_FIRSTBUTTON_DOWN, _UP */
}

static void fill_touch(UINT32 id, const Ptr *p, PointerTouchInfo *ti)
{
    memset(ti, 0, sizeof(*ti));
    fill_info(id, p, &ti->pointerInfo);
    ti->touchMask = 1;                                         /* TOUCH_MASK_CONTACTAREA */
    SetRect(&ti->rcContact, p->pt.x - 4, p->pt.y - 4, p->pt.x + 4, p->pt.y + 4);
    ti->rcContactRaw = ti->rcContact;
}

USERAPI BOOL GetPointerInfo(UINT32 id, void *info)
{
    LOCK();
    Ptr *p = ptr_of(id);
    if (p && info) fill_info(id, p, info);
    UNLOCK();
    if (!p || !info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

USERAPI BOOL GetPointerTouchInfo(UINT32 id, void *info)
{
    LOCK();
    Ptr *p = ptr_of(id);
    if (p && info) fill_touch(id, p, info);
    UNLOCK();
    if (!p || !info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

/* Every contact in @id's frame (the ones touching, and those that just lifted) */
static BOOL frame_of(UINT32 id, UINT32 *n, void *info, int touch)
{
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    LOCK();
    Ptr *p = ptr_of(id);
    if (!p) { UNLOCK(); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    UINT32 k = 0, cap = *n;
    for (int s = 0; s < NPTR; s++) {
        Ptr *q = &g_ptr[s];
        if (!q->used || !(q->down || q->frame == p->frame)) continue;
        if (info && k < cap) {
            if (touch) fill_touch((UINT32)s + 2, q, (PointerTouchInfo *)info + k);
            else fill_info((UINT32)s + 2, q, (PointerInfo *)info + k);
        }
        k++;
    }
    UNLOCK();
    *n = k;
    if (info && k > cap) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    return TRUE;
}

USERAPI BOOL GetPointerFrameTouchInfo(UINT32 id, UINT32 *n, void *info) { return frame_of(id, n, info, 1); }
USERAPI BOOL GetPointerFrameInfo(UINT32 id, UINT32 *n, void *info) { return frame_of(id, n, info, 0); }
USERAPI BOOL GetPointerPenInfo(UINT32 id, void *info) { (void)id; (void)info; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
USERAPI BOOL GetPointerFramePenInfo(UINT32 id, UINT32 *n, void *info) { (void)id; (void)n; (void)info; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

USERAPI BOOL GetTouchInputInfo(HANDLE h, UINT n, void *info, int size)
{
    UINT32 serial = (UINT32)((ULONG_PTR)h - 0x7A000000u);
    int k = serial % NSETS;
    if (!info || size != (int)sizeof(TouchInput) || !serial || g_set[k].serial != serial) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    UINT m = n < g_set[k].n ? n : g_set[k].n;
    memcpy(info, g_set[k].in, m * sizeof(TouchInput));
    return TRUE;
}
USERAPI BOOL CloseTouchInputHandle(HANDLE h)
{
    UINT32 serial = (UINT32)((ULONG_PTR)h - 0x7A000000u);
    int k = serial % NSETS;
    if (serial && g_set[k].serial == serial) g_set[k].serial = 0;
    return TRUE;
}

USERAPI BOOL RegisterTouchWindow(HWND h, ULONG f)
{
    (void)f;
    Wnd *w = W(h);
    if (!w) return FALSE;
    w->flags |= WF_TOUCH;
    return TRUE;
}
USERAPI BOOL UnregisterTouchWindow(HWND h)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    w->flags &= ~WF_TOUCH;
    return TRUE;
}
USERAPI BOOL IsTouchWindow(HWND h, PULONG flags)
{
    Wnd *w = W_quiet(h);
    if (flags) *flags = 0;
    return w && (w->flags & WF_TOUCH);
}
USERAPI BOOL EnableMouseInPointer(BOOL on) { (void)on; return TRUE; }
USERAPI BOOL IsMouseInPointerEnabled(void) { return FALSE; }
/* No gestures (WM_GESTURE): two-finger panning and zooming come as pointers */
USERAPI BOOL GetGestureInfo(HANDLE h, void *info) { (void)h; (void)info; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
USERAPI BOOL CloseGestureInfoHandle(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SetGestureConfig(HWND h, DWORD r, UINT n, void *cfg, UINT size) { (void)h; (void)r; (void)n; (void)cfg; (void)size; return TRUE; }
USERAPI BOOL GetGestureConfig(HWND h, DWORD r, DWORD f, PUINT n, void *cfg, UINT size) { (void)h; (void)r; (void)f; (void)cfg; (void)size; if (n) *n = 0; return TRUE; }
USERAPI HANDLE CreateSyntheticPointerDevice(DWORD type, ULONG max, DWORD mode)
{
    (void)type; (void)max; (void)mode;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
USERAPI BOOL InjectSyntheticPointerInput(HANDLE dev, const void *info, UINT32 n) { (void)dev; (void)info; (void)n; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
USERAPI void DestroySyntheticPointerDevice(HANDLE dev) { (void)dev; }

#define AR_NOSENSOR 0x10
USERAPI BOOL GetAutoRotationState(DWORD *state) { if (!state) return FALSE; *state = AR_NOSENSOR; return TRUE; }

/* Raw input: no devices to list or read */
USERAPI UINT GetRawInputDeviceList(void *list, PUINT n, UINT size)
{
    (void)list; (void)size;
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return (UINT)-1; }
    *n = 0;
    return 0;
}
USERAPI UINT GetRawInputDeviceInfoW(HANDLE dev, UINT cmd, LPVOID data, PUINT size)
{
    (void)dev; (void)cmd; (void)data; (void)size;
    SetLastError(ERROR_INVALID_HANDLE);
    return (UINT)-1;
}
USERAPI UINT GetRawInputDeviceInfoA(HANDLE dev, UINT cmd, LPVOID data, PUINT size) { return GetRawInputDeviceInfoW(dev, cmd, data, size); }
USERAPI UINT GetRawInputData(HANDLE raw, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    (void)raw; (void)cmd; (void)data; (void)size; (void)header;
    SetLastError(ERROR_INVALID_HANDLE);
    return (UINT)-1;
}

/* Every thread can own windows */
USERAPI BOOL IsGUIThread(BOOL convert) { (void)convert; return TRUE; }

/* Keyboard layouts, ANSI forms: US English only */
USERAPI BOOL GetKeyboardLayoutNameA(LPSTR name)
{
    if (!name) return FALSE;
    const char *s = "00000409";
    for (int i = 0; i < 9; i++) name[i] = s[i];
    return TRUE;
}
USERAPI HANDLE LoadKeyboardLayoutA(LPCSTR id, UINT f) { (void)id; (void)f; return (HANDLE)(ULONG_PTR)0x04090409; }
USERAPI BOOL UnloadKeyboardLayout(HANDLE h) { (void)h; return TRUE; }

/* PrintWindow: ask the window to paint itself into @hdc */
USERAPI BOOL PrintWindow(HWND h, HDC hdc, UINT flags)
{
    (void)flags;
    if (!IsWindow(h) || !hdc) return FALSE;
    SendMessageW(h, WM_PRINT, (WPARAM)hdc, 0x02 | 0x04 | 0x08 | 0x10);  /* PRF_NONCLIENT, CLIENT, ERASEBKGND, CHILDREN */
    return TRUE;
}

/* BroadcastSystemMessage: to every top-level window (posted, so no
 * recipient can block the sender) */
USERAPI long BroadcastSystemMessageW(DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)flags; (void)recipients;
    PostMessageW(HWND_BROADCAST, msg, wp, lp);
    return 1;
}
USERAPI long BroadcastSystemMessageA(DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp)
{
    return BroadcastSystemMessageW(flags, recipients, msg, wp, lp);
}
