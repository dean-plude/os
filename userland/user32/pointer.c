/*
 * pointer.c — touch: WM_POINTER* and WM_TOUCH; pens (and the mouse, with
 * EnableMouseInPointer) as pointers; synthetic pens; gestures and raw
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
 * touch.  Pointer ids: 1 is the mouse, touch contacts are slot + 2, the
 * pen is 12.
 *
 * A pen moves the pointer like a mouse, and the desktop tags the mouse
 * messages it causes with its packet (pressure, buttons, eraser, tilt,
 * rotation; kernel/wm/tablet.h).  In a client area such a message becomes
 * WM_POINTER* for the pen, as on Windows 8 and later: WM_POINTERENTER when
 * it comes over the window (hovering or touching), WM_POINTERDOWN /
 * UPDATE / UP for the tip, hovering and the barrel button,
 * WM_POINTERLEAVE when it goes to another window or out of range, and
 * WM_POINTERCAPTURECHANGED for the window it was touching when the input
 * goes elsewhere; GetPointerType says PT_PEN, GetPointerPenInfo has the
 * rest.  DefWindowProc gives back the mouse message it was made of, so
 * programs that know only the mouse (or Wintab) work as before.  With
 * EnableMouseInPointer(TRUE) the mouse's own messages come the same way,
 * as PT_MOUSE.  Each message's data stays in a ring of records; the one
 * a thread took last (GetMessage) is what GetPointerInfo answers with.
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

#define PT_PEN               3
#define PEN_FLAG_BARREL      1
#define PEN_FLAG_INVERTED    2
#define PEN_FLAG_ERASER      4
#define PEN_MASK_PRESSURE    1
#define PEN_MASK_ROTATION    2
#define PEN_MASK_TILT_X      4
#define PEN_MASK_TILT_Y      8

typedef struct {
    PointerInfo pointerInfo;
    UINT32    penFlags, penMask, pressure, rotation;
    INT32     tiltX, tiltY;
} PointerPenInfo;

#define PEN_ID 12                   /* the pen's pointer id */
static int g_pen_seen;              /* a pen has been over one of our windows */
static LRESULT pointer_default(Wnd *w, UINT msg, WPARAM wp, LPARAM lp);
static BOOL rec_info(UINT32 id, void *info, int pen);

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
    /* WM_POINTERDOWN, UPDATE, UP: a pen's, or the mouse's */
    if (LOWORD(wp) == 1 || LOWORD(wp) == PEN_ID) return pointer_default(w, msg, wp, lp);
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
    if (id == PEN_ID && g_pen_seen) { *type = PT_PEN; return TRUE; }
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
    if (id == 1 || id == PEN_ID) return rec_info(id, info, 0);
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
    if ((id == 1 || id == PEN_ID) && !touch) {               /* a pen's or the mouse's frame: itself */
        if (info && *n < 1) { *n = 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        *n = 1;
        return !info || rec_info(id, info, 0);
    }
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
/* No gestures (WM_GESTURE): two-finger panning and zooming come as pointers */
USERAPI BOOL GetGestureInfo(HANDLE h, void *info) { (void)h; (void)info; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
USERAPI BOOL CloseGestureInfoHandle(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SetGestureConfig(HWND h, DWORD r, UINT n, void *cfg, UINT size) { (void)h; (void)r; (void)n; (void)cfg; (void)size; return TRUE; }
USERAPI BOOL GetGestureConfig(HWND h, DWORD r, DWORD f, PUINT n, void *cfg, UINT size) { (void)h; (void)r; (void)f; (void)cfg; (void)size; if (n) *n = 0; return TRUE; }
/* Synthetic pens (Windows 10 1809's pointer injection): a pen device the
 * desktop counts as a tablet (wintab32 reports it, SM_DIGITIZER has
 * NID_EXTERNAL_PEN); its input moves the pointer, the tip clicks, and the
 * packets with their pressure, tilt and rotation (penMask) reach wintab32.
 * Synthetic touch is not there. */
typedef struct {
    DWORD type;
    union { PointerTouchInfo touchInfo; PointerPenInfo penInfo; };
} PointerTypeInfo;

typedef struct { DWORD magic, type; } SynthDev;
#define SYNTH_MAGIC 0x6E797350                  /* "Psyn" */

USERAPI HANDLE CreateSyntheticPointerDevice(DWORD type, ULONG max, DWORD mode)
{
    (void)max; (void)mode;
    if (type != PT_PEN) { SetLastError(ERROR_NOT_SUPPORTED); return 0; }
    SynthDev *d = calloc(1, sizeof(*d));
    if (!d) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if (!NtNovaGuiCtl(0, CTL_TABLET, 2, NULL)) { free(d); SetLastError(ERROR_NOT_SUPPORTED); return 0; }
    d->magic = SYNTH_MAGIC; d->type = type;
    return (HANDLE)d;
}

/* A screen position as the desktop's 0-65535 across every monitor */
static INT32 desk_coord(LONG v, LONG org, LONG ext)
{
    int s = display_scale();
    LONG dev = (v - org) * s, n = ext * s - 1;
    if (dev < 0) dev = 0;
    if (n < 1) return 0;
    if (dev > n) dev = n;
    return (INT32)(((long long)dev * 65535 + n - 1) / n);
}

USERAPI BOOL InjectSyntheticPointerInput(HANDLE dev, const void *info, UINT32 n)
{
    SynthDev *d = dev;
    if (!d || d->magic != SYNTH_MAGIC || !info || !n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const PointerTypeInfo *in = info;
    RECT v;
    u32_virtual_screen(&v);
    for (UINT32 i = 0; i < n; i++) {
        if (in[i].type != PT_PEN) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        const PointerPenInfo *p = &in[i].penInfo;
        UINT32 f = p->pointerInfo.pointerFlags;
        int contact = (f & POINTER_FLAG_INCONTACT) != 0 && !(f & POINTER_FLAG_UP);
        int eraser = (p->penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
        UINT32 pr = p->pressure > 1024 ? 1024 : p->pressure;
        int tilt = (p->penMask & (PEN_MASK_TILT_X | PEN_MASK_TILT_Y)) != 0, rot = (p->penMask & PEN_MASK_ROTATION) != 0;
        INT32 tx = p->penMask & PEN_MASK_TILT_X ? p->tiltX : 0, ty = p->penMask & PEN_MASK_TILT_Y ? p->tiltY : 0;
        INT32 pkt[8] = {
            desk_coord(p->pointerInfo.ptPixelLocation.x, v.left, v.right - v.left),
            desk_coord(p->pointerInfo.ptPixelLocation.y, v.top, v.bottom - v.top),
            contact ? (INT32)(pr * 1023 / 1024) : 0,
            (contact ? 1 : 0) | (p->penFlags & PEN_FLAG_BARREL ? 2 : 0),
            (f & (POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT) ? 1 : 0) | (eraser ? 2 : 0) |   /* (lifted, still hovering) */
            (tilt ? 4 : 0) | (rot ? 8 : 0),
            (tx < -90 ? -90 : tx > 90 ? 90 : tx) * 10,                 /* degrees to the desktop's tenths */
            (ty < -90 ? -90 : ty > 90 ? 90 : ty) * 10,
            rot ? (INT32)(p->rotation % 360) * 10 : 0,
        };
        if (!NtNovaGuiCtl(0, CTL_TABLET, 4, pkt)) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    }
    return TRUE;
}

USERAPI void DestroySyntheticPointerDevice(HANDLE dev)
{
    SynthDev *d = dev;
    if (!d || d->magic != SYNTH_MAGIC) return;
    NtNovaGuiCtl(0, CTL_TABLET, 3, NULL);
    d->magic = 0;
    free(d);
}

/* ---- Pens, and the mouse, as pointers ---------------------------------- */
#define POINTER_FLAG_SECONDBUTTON 0x00020
#define POINTER_FLAG_THIRDBUTTON  0x00040

#define WM_POINTERENTER          0x0249
#define WM_POINTERLEAVE          0x024A
#define WM_POINTERCAPTURECHANGED 0x024C

/* POINTER_BUTTON_CHANGE_TYPE */
#define CHANGE_FIRST_DOWN  1
#define CHANGE_FIRST_UP    2
#define CHANGE_SECOND_DOWN 3
#define CHANGE_SECOND_UP   4
#define CHANGE_THIRD_DOWN  5
#define CHANGE_THIRD_UP    6

#define PEN_DEVICE   ((HANDLE)(ULONG_PTR)0x50454E31)    /* "PEN1": sourceDevice of the pen */
#define MOUSE_DEVICE ((HANDLE)(ULONG_PTR)0x4D4F5531)    /* "MOU1" */

/* The desktop's pen packet (kernel/wm/tablet.h TabletPacket, wintab32's KPacket) */
typedef struct {
    UINT32 serial, time;
    INT32  x, y;
    UINT16 pressure;
    UINT8  buttons, flags;          /* buttons: 1 tip, 2 barrel; flags: 1 in range, 2 eraser, 4 tilt, 8 twist */
    INT16  tilt_x, tilt_y;          /* tenths of a degree */
    UINT16 twist, reserved;
} KPen;

/* One pointer message's data */
#define NREC 64
typedef struct {
    UINT32 seq;                     /* 1, 2, 3...; 0: unused */
    int    taken;                   /* a thread took its message */
    HWND   h;
    UINT   msg;                     /* WM_POINTER* */
    UINT32 id;
    UINT   mouse;                   /* the mouse message DefWindowProc makes of it (0: none) */
    WPARAM mk;
    POINT  pt;                      /* screen */
    DWORD  flags, time;
    int    change;                  /* CHANGE_* */
    UINT32 frame;
    UINT32 penFlags, penMask, pressure, rotation;
    INT32  tiltX, tiltY;
} PRec;

static PRec   g_rec[NREC];
static UINT32 g_rec_seq, g_pframe;
static struct { DWORD tid; UINT32 seq; } g_cur[16];  /* the record of each thread's last pointer message */
static struct { HWND in; int contact; } g_pen;     /* the window the pen is over; touching it */
static int    g_mip;                                /* EnableMouseInPointer */

static void emit(Wnd *w, UINT msg, UINT32 id, DWORD flags, POINT pt, DWORD time, const PRec *data, UINT mouse, WPARAM mk,
                 LPARAM lp)
{
    PRec *r = &g_rec[++g_rec_seq % NREC];
    if (!g_rec_seq) r = &g_rec[++g_rec_seq % NREC];
    *r = *data;
    r->seq = g_rec_seq;
    r->taken = 0;
    r->h = w->h;
    r->msg = msg;
    r->id = id;
    r->mouse = mouse;
    r->mk = mk;
    r->pt = pt;
    r->flags = flags;
    r->time = time;
    r->frame = g_pframe;
    input_queue(w, msg, MAKEWPARAM(id, flags & 0xFFFF), lp ? lp : MAKELPARAM(pt.x, pt.y), time);
}

/* The pen packet numbered @serial, if the desktop still has it */
static int pen_packet(UINT32 serial, KPen *k)
{
    struct { UINT32 after, max, wait, newest; KPen pk[1]; } b;
    memset(&b, 0, sizeof(b));
    b.after = serial - 1;
    b.max = 1;
    return serial && NtNovaGuiCtl(0, CTL_TABLET, 1, &b) == 1 && b.pk[0].serial == serial ? (*k = b.pk[0], 1) : 0;
}

int pointer_from_mouse(Wnd *target, UINT msg, WPARAM mk, POINT pt, DWORD time, UINT32 pen)
{
    if (msg != WM_MOUSEMOVE && msg != WM_LBUTTONDOWN && msg != WM_LBUTTONUP && msg != WM_RBUTTONDOWN &&
        msg != WM_RBUTTONUP && msg != WM_MBUTTONDOWN && msg != WM_MBUTTONUP)
        return 0;
    if (!pen && !g_mip) return 0;
    PRec d;
    memset(&d, 0, sizeof(d));
    LOCK();
    g_pframe++;
    if (!pen) {
        /* the mouse: WM_POINTERDOWN for the first button, UP for the last */
        WPARAM bit = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ? MK_LBUTTON :
                     msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? MK_RBUTTON : msg == WM_MOUSEMOVE ? 0 : MK_MBUTTON;
        WPARAM all = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON, after = mk & all;
        int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        if (down) after |= bit; else after &= ~bit;
        WPARAM before = down ? after & ~bit : msg == WM_MOUSEMOVE ? after : after | bit;
        UINT pm = !before && after ? WM_POINTERDOWN : before && !after ? WM_POINTERUP : WM_POINTERUPDATE;
        d.change = bit == MK_LBUTTON ? (down ? CHANGE_FIRST_DOWN : CHANGE_FIRST_UP) :
                   bit == MK_RBUTTON ? (down ? CHANGE_SECOND_DOWN : CHANGE_SECOND_UP) :
                   bit == MK_MBUTTON ? (down ? CHANGE_THIRD_DOWN : CHANGE_THIRD_UP) : 0;
        DWORD f = POINTER_FLAG_INRANGE | POINTER_FLAG_PRIMARY | (after ? POINTER_FLAG_INCONTACT : 0) |
                  (after & MK_LBUTTON ? POINTER_FLAG_FIRSTBUTTON : 0) | (after & MK_RBUTTON ? POINTER_FLAG_SECONDBUTTON : 0) |
                  (after & MK_MBUTTON ? POINTER_FLAG_THIRDBUTTON : 0) |
                  (pm == WM_POINTERDOWN ? POINTER_FLAG_DOWN : pm == WM_POINTERUP ? POINTER_FLAG_UP : POINTER_FLAG_UPDATE);
        emit(target, pm, 1, f, pt, time, &d, msg, mk, 0);
        UNLOCK();
        return 1;
    }

    /* a pen */
    KPen k;
    if (!pen_packet(pen, &k)) {                     /* (gone from the ring: what the message says) */
        memset(&k, 0, sizeof(k));
        k.flags = 1;
        k.buttons = (mk & MK_LBUTTON ? 1 : 0) | (mk & MK_RBUTTON ? 2 : 0);
        if (msg == WM_LBUTTONDOWN) k.buttons |= 1;
        if (msg == WM_LBUTTONUP) k.buttons &= ~1;
        k.pressure = k.buttons & 1 ? 512 : 0;
    }
    g_pen_seen = 1;
    int tip = (k.buttons & 1) != 0, barrel = (k.buttons & 2) != 0, eraser = (k.flags & 2) != 0;
    int near = (k.flags & 1) || tip;
    d.penFlags = (barrel ? PEN_FLAG_BARREL : 0) | (eraser ? PEN_FLAG_INVERTED : 0) | (eraser && tip ? PEN_FLAG_ERASER : 0);
    d.penMask = PEN_MASK_PRESSURE | (k.flags & 4 ? PEN_MASK_TILT_X | PEN_MASK_TILT_Y : 0) | (k.flags & 8 ? PEN_MASK_ROTATION : 0);
    d.pressure = (UINT32)k.pressure * 1024 / 1023;
    d.tiltX = k.flags & 4 ? k.tilt_x / 10 : 0;
    d.tiltY = k.flags & 4 ? k.tilt_y / 10 : 0;
    d.rotation = k.flags & 8 ? (UINT32)k.twist / 10 : 0;
    DWORD state = POINTER_FLAG_PRIMARY | (near ? POINTER_FLAG_INRANGE : 0) | (tip ? POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON : 0) |
                  (barrel ? POINTER_FLAG_SECONDBUTTON : 0);
    Wnd *was = W_quiet(g_pen.in);
    if (!near) {                                    /* out of range */
        if (was) emit(was, WM_POINTERLEAVE, PEN_ID, state | POINTER_FLAG_UPDATE, pt, time, &d, 0, 0, 0);
        g_pen.in = 0;
        g_pen.contact = 0;
        UNLOCK();
        return 1;
    }
    /* touching: it stays with the window it touched down on */
    if (was && g_pen.contact && was != target && msg != WM_LBUTTONDOWN && top_of(was) == top_of(target)) target = was;
    if (was && was != target) {
        if (g_pen.contact) emit(was, WM_POINTERCAPTURECHANGED, PEN_ID, state, pt, time, &d, 0, 0, (LPARAM)target->h);
        emit(was, WM_POINTERLEAVE, PEN_ID, state | POINTER_FLAG_UPDATE, pt, time, &d, 0, 0, 0);
        was = NULL;
    }
    if (!was) {
        emit(target, WM_POINTERENTER, PEN_ID, state | POINTER_FLAG_NEW | POINTER_FLAG_UPDATE, pt, time, &d, 0, 0, 0);
        g_pen.in = target->h;
    }
    UINT pm = msg == WM_LBUTTONDOWN ? WM_POINTERDOWN : msg == WM_LBUTTONUP ? WM_POINTERUP : WM_POINTERUPDATE;
    d.change = msg == WM_LBUTTONDOWN ? CHANGE_FIRST_DOWN : msg == WM_LBUTTONUP ? CHANGE_FIRST_UP :
               msg == WM_RBUTTONDOWN ? CHANGE_SECOND_DOWN : msg == WM_RBUTTONUP ? CHANGE_SECOND_UP : 0;
    if (pm == WM_POINTERDOWN) state |= POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON;
    if (pm == WM_POINTERUP) state &= ~(DWORD)(POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON);
    state |= pm == WM_POINTERDOWN ? POINTER_FLAG_DOWN : pm == WM_POINTERUP ? POINTER_FLAG_UP : POINTER_FLAG_UPDATE;
    emit(target, pm, PEN_ID, state, pt, time, &d, msg, mk, 0);
    g_pen.contact = (state & POINTER_FLAG_INCONTACT) != 0;
    UNLOCK();
    return 1;
}

/* The pointer left @top for another window (the desktop's WM_MOUSELEAVE,
 * which may come after the next window's first move) */
void pointer_left(Wnd *top)
{
    LOCK();
    Wnd *was = W_quiet(g_pen.in);
    if (was && !g_pen.contact && top_of(was) == top) {
        PRec d;
        memset(&d, 0, sizeof(d));
        d.penMask = PEN_MASK_PRESSURE;
        g_pframe++;
        emit(was, WM_POINTERLEAVE, PEN_ID, POINTER_FLAG_PRIMARY | POINTER_FLAG_UPDATE, g_rec[g_rec_seq % NREC].pt,
             GetTickCount(), &d, 0, 0, 0);
        g_pen.in = 0;
    }
    UNLOCK();
}

static UINT32 *cur_slot(int create)
{
    DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < 16; i++) if (g_cur[i].tid == tid) return &g_cur[i].seq;
    if (!create) return NULL;
    for (int i = 0; i < 16; i++) if (!g_cur[i].tid) { g_cur[i].tid = tid; return &g_cur[i].seq; }
    int i = (int)(tid % 16);                       /* (full: share a slot) */
    g_cur[i].tid = tid;
    return &g_cur[i].seq;
}

void pointer_taken(const MSG *m)
{
    if (m->message < 0x0245 || m->message > WM_POINTERCAPTURECHANGED) return;
    UINT32 id = LOWORD(m->wParam);
    if (id != 1 && id != PEN_ID) return;
    LOCK();
    PRec *best = NULL;
    for (int i = 0; i < NREC; i++) {               /* the oldest record of it not taken */
        PRec *r = &g_rec[i];
        if (r->seq && !r->taken && r->h == m->hwnd && r->msg == m->message && r->id == id &&
            (!best || r->seq < best->seq))
            best = r;
    }
    if (best) {
        best->taken = 1;
        UINT32 *c = cur_slot(1);
        if (c) *c = best->seq;
    }
    UNLOCK();
}

/* The record for @id: the thread's current message's, else the newest */
static PRec *rec_for(UINT32 id)
{
    UINT32 *c = cur_slot(0);
    if (c && *c) {
        PRec *r = &g_rec[*c % NREC];
        if (r->seq == *c && r->id == id) return r;
    }
    PRec *best = NULL;
    for (int i = 0; i < NREC; i++)
        if (g_rec[i].seq && g_rec[i].id == id && (!best || g_rec[i].seq > best->seq)) best = &g_rec[i];
    return best;
}

static void rec_fill(const PRec *r, PointerInfo *pi)
{
    RECT v;
    u32_virtual_screen(&v);
    memset(pi, 0, sizeof(*pi));
    pi->pointerType = r->id == PEN_ID ? PT_PEN : PT_MOUSE;
    pi->pointerId = r->id;
    pi->frameId = r->frame;
    pi->pointerFlags = r->flags;
    pi->sourceDevice = r->id == PEN_ID ? PEN_DEVICE : MOUSE_DEVICE;
    pi->hwndTarget = r->h;
    pi->ptPixelLocation = pi->ptPixelLocationRaw = r->pt;
    /* HIMETRIC across the device (GetPointerDeviceRects: the virtual screen) */
    pi->ptHimetricLocation.x = pi->ptHimetricLocationRaw.x = MulDiv(r->pt.x - v.left, 2540, 96);
    pi->ptHimetricLocation.y = pi->ptHimetricLocationRaw.y = MulDiv(r->pt.y - v.top, 2540, 96);
    pi->dwTime = r->time;
    pi->historyCount = 1;
    pi->dwKeyStates = (DWORD)(r->mk & (MK_SHIFT | MK_CONTROL));
    pi->ButtonChangeType = r->change;
}

static BOOL rec_info(UINT32 id, void *info, int pen)
{
    if (!info || (pen && id != PEN_ID)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    LOCK();
    PRec *r = rec_for(id);
    if (r) {
        if (pen) {
            PointerPenInfo *pi = info;
            memset(pi, 0, sizeof(*pi));
            rec_fill(r, &pi->pointerInfo);
            pi->penFlags = r->penFlags;
            pi->penMask = r->penMask;
            pi->pressure = r->pressure;
            pi->rotation = r->rotation;
            pi->tiltX = r->tiltX;
            pi->tiltY = r->tiltY;
        } else {
            rec_fill(r, info);
        }
    }
    UNLOCK();
    if (!r) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

/* DefWindowProc: the mouse message it was made of */
static LRESULT pointer_default(Wnd *w, UINT msg, WPARAM wp, LPARAM lp)
{
    LOCK();
    PRec *r = rec_for(LOWORD(wp));
    PRec c;
    int ok = r && r->h == w->h && r->msg == msg && r->mouse && r->mk != (WPARAM)-1 &&
             r->pt.x == (short)LOWORD(lp) && r->pt.y == (short)HIWORD(lp);
    if (ok) { c = *r; r->mk = (WPARAM)-1; }         /* (once) */
    UNLOCK();
    if (!ok) return 0;
    Wnd *top = top_of(w);
    if (top) input_mouse(top, c.mouse, c.mk, c.pt);
    return 0;
}

USERAPI BOOL GetPointerPenInfo(UINT32 id, void *info) { return rec_info(id, info, 1); }
USERAPI BOOL GetPointerFramePenInfo(UINT32 id, UINT32 *n, void *info)
{
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (info && *n < 1) { *n = 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *n = 1;
    return !info || rec_info(id, info, 1);
}
/* History: the message's own entry */
USERAPI BOOL GetPointerPenInfoHistory(UINT32 id, UINT32 *n, void *info) { return GetPointerFramePenInfo(id, n, info); }
USERAPI BOOL GetPointerInfoHistory(UINT32 id, UINT32 *n, void *info) { return GetPointerFrameInfo(id, n, info); }
USERAPI BOOL GetPointerFramePenInfoHistory(UINT32 id, UINT32 *entries, UINT32 *count, void *info)
{
    if (!entries || !count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (info && (*entries < 1 || *count < 1)) { *entries = *count = 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *entries = *count = 1;
    return !info || rec_info(id, info, 1);
}
USERAPI BOOL GetPointerCursorId(UINT32 id, UINT32 *cursor)
{
    DWORD type;
    if (!cursor || !GetPointerType(id, &type)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *cursor = id == PEN_ID ? 0 : id;
    return TRUE;
}
USERAPI BOOL SkipPointerFrameMessages(UINT32 id) { (void)id; return TRUE; }

USERAPI BOOL EnableMouseInPointer(BOOL on) { g_mip = on != 0; return TRUE; }
USERAPI BOOL IsMouseInPointerEnabled(void) { return g_mip; }

/* The pen as a pointer device (POINTER_DEVICE_INFO): an external pen over
 * the whole virtual screen, when the desktop has one */
#define POINTER_DEVICE_TYPE_EXTERNAL_PEN 2
typedef struct {
    DWORD    displayOrientation;
    HANDLE   device;
    int      pointerDeviceType;
    HMONITOR monitor;
    ULONG    startingCursorId;
    USHORT   maxActiveContacts;
    WCHAR    productString[520];
} PointerDeviceInfo;

static void pen_device(PointerDeviceInfo *d)
{
    static const WCHAR name[] = L"NovaOS pen";
    POINT o = { 0, 0 };
    memset(d, 0, sizeof(*d));
    d->device = PEN_DEVICE;
    d->pointerDeviceType = POINTER_DEVICE_TYPE_EXTERNAL_PEN;
    d->monitor = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    d->maxActiveContacts = 1;
    memcpy(d->productString, name, sizeof(name));
}

static int pen_present(void) { return (int)NtNovaGuiCtl(0, CTL_TABLET, 0, NULL) > 0; }

USERAPI BOOL GetPointerDevices(UINT32 *n, void *devs)
{
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    UINT32 have = pen_present() ? 1 : 0;
    if (devs && *n < have) { *n = have; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (devs && have) pen_device(devs);
    *n = have;
    return TRUE;
}
USERAPI BOOL GetPointerDevice(HANDLE dev, void *info)
{
    if (dev != PEN_DEVICE || !info || !pen_present()) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pen_device(info);
    return TRUE;
}
/* The device's HIMETRIC rectangle and the screen's it maps onto */
USERAPI BOOL GetPointerDeviceRects(HANDLE dev, RECT *pointer, RECT *display)
{
    if ((dev != PEN_DEVICE && dev != MOUSE_DEVICE) || !pointer || !display) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    RECT v;
    u32_virtual_screen(&v);
    *display = v;
    SetRect(pointer, 0, 0, MulDiv(v.right - v.left, 2540, 96), MulDiv(v.bottom - v.top, 2540, 96));
    return TRUE;
}

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
