/*
 * winmm.dll's joystick functions (joyGetPosEx, joyGetDevCaps, ...): the
 * game controllers NovaOS's kernel keeps (kernel/drivers/gamepad.c, read
 * through novapad.h, as xinput1_4 and dinput8 read them).
 *
 * Joystick IDs 0-15 are the plugged-in controllers in slot order, as
 * Windows numbers the controllers its Game Controllers panel lists.  The
 * six winmm axes come from the HID axes the way Windows maps them: X, Y
 * and Z as they are, R from Rz (or Ry when there is no Rz), U from Rx (or
 * the slider), V from Ry when R took Rz (or the dial).  An Xbox
 * controller's HID side has X, Y (left stick), Rx, Ry (right stick) and Z
 * (both triggers), so R is the right stick's up and down and U its left
 * and right, as on Windows; buttons 1-10 are A, B, X, Y, LB, RB, Back,
 * Start and the two stick clicks, and the D-pad is the point of view.
 * Axes run 0-65535 (Y and R down); joySetCapture posts MM_JOY1MOVE,
 * MM_JOY1BUTTONDOWN/UP (and MM_JOY2*) to a window from a polling thread.
 */

#include <windows.h>
#include <novapad.h>

#define MMAPI __declspec(dllexport)
typedef UINT MMRESULT;
#define JOYERR_NOERROR    0
#define JOYERR_PARMS      165
#define JOYERR_NOCANDO    166
#define JOYERR_UNPLUGGED  167
#define MMSYSERR_INVALPARAM 11

#define JOY_RETURNX        0x00000001
#define JOY_RETURNY        0x00000002
#define JOY_RETURNZ        0x00000004
#define JOY_RETURNR        0x00000008
#define JOY_RETURNU        0x00000010
#define JOY_RETURNV        0x00000020
#define JOY_RETURNPOV      0x00000040
#define JOY_RETURNBUTTONS  0x00000080
#define JOY_RETURNPOVCTS   0x00000200
#define JOY_RETURNCENTERED 0x00000400
#define JOY_POVCENTERED    0xFFFF

#define JOYCAPS_HASZ     0x0001
#define JOYCAPS_HASR     0x0002
#define JOYCAPS_HASU     0x0004
#define JOYCAPS_HASV     0x0008
#define JOYCAPS_HASPOV   0x0010
#define JOYCAPS_POV4DIR  0x0020
#define JOYCAPS_POVCTS   0x0040

#define MM_JOY1MOVE       0x3A0
#define MM_JOY2MOVE       0x3A1
#define MM_JOY1ZMOVE      0x3A2
#define MM_JOY2ZMOVE      0x3A3
#define MM_JOY1BUTTONDOWN 0x3B5
#define MM_JOY2BUTTONDOWN 0x3B6
#define MM_JOY1BUTTONUP   0x3B7
#define MM_JOY2BUTTONUP   0x3B8

#define JOY_IDS 16

typedef struct {
    DWORD dwSize, dwFlags, dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos;
    DWORD dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
} JOYINFOEX;

typedef struct { UINT wXpos, wYpos, wZpos, wButtons; } JOYINFO;

#define MAXPNAMELEN 32
#define MAX_JOYSTICKOEMVXDNAME 260
#define JOYCAPS_FIELDS(CH) \
    WORD wMid, wPid; \
    CH szPname[MAXPNAMELEN]; \
    UINT wXmin, wXmax, wYmin, wYmax, wZmin, wZmax, wNumButtons, wPeriodMin, wPeriodMax; \
    UINT wRmin, wRmax, wUmin, wUmax, wVmin, wVmax, wCaps, wMaxAxes, wNumAxes, wMaxButtons; \
    CH szRegKey[MAXPNAMELEN]; \
    CH szOEMVxD[MAX_JOYSTICKOEMVXDNAME];
typedef struct { JOYCAPS_FIELDS(char) } JOYCAPSA;
typedef struct { JOYCAPS_FIELDS(WCHAR) } JOYCAPSW;

/* The slot of joystick @id (the id-th plugged-in controller), or -1 */
static int joy_slot(UINT id, NovaPadInfo *info)
{
    DWORD present = nova_pad_present();
    for (int s = 0; s < NOVA_PAD_SLOTS; s++)
        if (present & (1u << s) && id-- == 0)
            return nova_pad_info(s, info) && info->serial ? s : -1;
    return -1;
}

/* Which HID axes (NOVA_PAD_*) give winmm's X, Y, Z, R, U and V, -1 for
 * none */
static void joy_axes(const NovaPadInfo *info, int ax[6])
{
    BYTE has = info->axes;
    #define HAS(a) (has & (1u << (a)))
    ax[0] = HAS(NOVA_PAD_X) ? NOVA_PAD_X : -1;
    ax[1] = HAS(NOVA_PAD_Y) ? NOVA_PAD_Y : -1;
    ax[2] = HAS(NOVA_PAD_Z) ? NOVA_PAD_Z : -1;
    ax[3] = HAS(NOVA_PAD_RZ) ? NOVA_PAD_RZ : HAS(NOVA_PAD_RY) ? NOVA_PAD_RY : -1;
    ax[4] = HAS(NOVA_PAD_RX) ? NOVA_PAD_RX : HAS(NOVA_PAD_SLIDER) ? NOVA_PAD_SLIDER : -1;
    ax[5] = ax[3] == NOVA_PAD_RZ && HAS(NOVA_PAD_RY) ? NOVA_PAD_RY : HAS(NOVA_PAD_DIAL) ? NOVA_PAD_DIAL : -1;
    #undef HAS
}

static int joy_buttons(const NovaPadInfo *info)
{
    return info->buttons > 32 ? 32 : info->buttons;
}

#define JOYCAPS_FILL(c, info, name) do { \
    int ax_[6], n_ = 0; \
    joy_axes(info, ax_); \
    for (int i_ = 0; i_ < 6; i_++) n_ += ax_[i_] >= 0; \
    (c)->wMid = (info)->vid; \
    (c)->wPid = (info)->pid; \
    (c)->wXmax = (c)->wYmax = (c)->wZmax = (c)->wRmax = (c)->wUmax = (c)->wVmax = 65535; \
    (c)->wNumButtons = (UINT)joy_buttons(info); \
    (c)->wPeriodMin = 10; \
    (c)->wPeriodMax = 1000; \
    (c)->wCaps = (ax_[2] >= 0 ? JOYCAPS_HASZ : 0) | (ax_[3] >= 0 ? JOYCAPS_HASR : 0) | \
                 (ax_[4] >= 0 ? JOYCAPS_HASU : 0) | (ax_[5] >= 0 ? JOYCAPS_HASV : 0) | \
                 ((info)->povs ? JOYCAPS_HASPOV | JOYCAPS_POV4DIR | JOYCAPS_POVCTS : 0); \
    (c)->wMaxAxes = 6; \
    (c)->wNumAxes = (UINT)n_; \
    (c)->wMaxButtons = 32; \
    for (int i_ = 0; name[i_] && i_ < MAXPNAMELEN - 1; i_++) (c)->szPname[i_] = name[i_]; \
    for (int i_ = 0; "DINPUT.DLL"[i_]; i_++) (c)->szRegKey[i_] = "DINPUT.DLL"[i_]; \
} while (0)

/* Windows names every joystick the same in its caps (games read the
 * controller's own name from the registry key szRegKey names) */
static const char g_pname[] = "Microsoft PC-joystick driver";

MMAPI UINT WINAPI joyGetNumDevs(void) { return JOY_IDS; }

MMAPI MMRESULT WINAPI joyGetDevCapsA(UINT_PTR id, JOYCAPSA *caps, UINT n)
{
    NovaPadInfo info;
    if (!caps || n < sizeof(*caps)) return MMSYSERR_INVALPARAM;
    if (id == (UINT_PTR)-1) return JOYERR_NOERROR;          /* (asks whether the driver is there) */
    if (id >= JOY_IDS) return JOYERR_PARMS;
    if (joy_slot((UINT)id, &info) < 0) return JOYERR_PARMS;
    ZeroMemory(caps, sizeof(*caps));
    JOYCAPS_FILL(caps, &info, g_pname);
    return JOYERR_NOERROR;
}

MMAPI MMRESULT WINAPI joyGetDevCapsW(UINT_PTR id, JOYCAPSW *caps, UINT n)
{
    NovaPadInfo info;
    if (!caps || n < sizeof(*caps)) return MMSYSERR_INVALPARAM;
    if (id == (UINT_PTR)-1) return JOYERR_NOERROR;
    if (id >= JOY_IDS) return JOYERR_PARMS;
    if (joy_slot((UINT)id, &info) < 0) return JOYERR_PARMS;
    ZeroMemory(caps, sizeof(*caps));
    JOYCAPS_FILL(caps, &info, g_pname);
    return JOYERR_NOERROR;
}

static UINT g_threshold[JOY_IDS];

/* Joystick @id's state, all of JOYINFOEX's fields */
static MMRESULT joy_read(UINT id, JOYINFOEX *j)
{
    NovaPadInfo info;
    NovaPadState st;
    int ax[6];
    if (id >= JOY_IDS) return JOYERR_PARMS;
    int slot = joy_slot(id, &info);
    if (slot < 0 || !nova_pad_state(slot, &st)) return JOYERR_UNPLUGGED;
    joy_axes(&info, ax);
    DWORD *pos = &j->dwXpos;
    for (int i = 0; i < 6; i++) pos[i] = ax[i] >= 0 ? st.axis[ax[i]] : 32767;
    int nb = joy_buttons(&info);
    j->dwButtons = nb >= 32 ? st.buttons : st.buttons & ((1u << nb) - 1);
    j->dwButtonNumber = 0;
    for (DWORD b = j->dwButtons; b; b &= b - 1) j->dwButtonNumber++;
    j->dwPOV = info.povs && st.pov >= 0 ? (DWORD)st.pov % 36000 : JOY_POVCENTERED;
    return JOYERR_NOERROR;
}

MMAPI MMRESULT WINAPI joyGetPosEx(UINT id, JOYINFOEX *info)
{
    if (!info) return MMSYSERR_INVALPARAM;
    if (info->dwSize < sizeof(*info)) return JOYERR_PARMS;
    JOYINFOEX j;
    MMRESULT r = joy_read(id, &j);
    if (r != JOYERR_NOERROR) return r;
    DWORD f = info->dwFlags;
    DWORD *in = &j.dwXpos, *out = &info->dwXpos;
    for (int i = 0; i < 6; i++)
        if (f & (JOY_RETURNX << i)) out[i] = in[i];
    if (f & JOY_RETURNBUTTONS) {
        info->dwButtons = j.dwButtons;
        info->dwButtonNumber = j.dwButtonNumber;
    }
    if (f & JOY_RETURNPOVCTS)
        info->dwPOV = j.dwPOV;
    else if (f & JOY_RETURNPOV)          /* the four directions only: the nearest one */
        info->dwPOV = j.dwPOV == JOY_POVCENTERED ? JOY_POVCENTERED : (j.dwPOV + 4500) / 9000 % 4 * 9000;
    return JOYERR_NOERROR;
}

MMAPI MMRESULT WINAPI joyGetPos(UINT id, JOYINFO *info)
{
    if (!info) return MMSYSERR_INVALPARAM;
    JOYINFOEX j;
    MMRESULT r = joy_read(id, &j);
    if (r != JOYERR_NOERROR) return r;
    info->wXpos = j.dwXpos;
    info->wYpos = j.dwYpos;
    info->wZpos = j.dwZpos;
    info->wButtons = j.dwButtons & 0xF;
    return JOYERR_NOERROR;
}

MMAPI MMRESULT WINAPI joyGetThreshold(UINT id, UINT *t)
{
    if (!t) return MMSYSERR_INVALPARAM;
    if (id >= JOY_IDS) return JOYERR_PARMS;
    *t = g_threshold[id];
    return JOYERR_NOERROR;
}

MMAPI MMRESULT WINAPI joySetThreshold(UINT id, UINT t)
{
    if (id >= JOY_IDS) return JOYERR_PARMS;
    g_threshold[id] = t;
    return JOYERR_NOERROR;
}

MMAPI MMRESULT WINAPI joyConfigChanged(DWORD flags)
{
    return flags ? JOYERR_PARMS : JOYERR_NOERROR;
}

/* ---- capture: a thread that polls the joystick and posts messages ---- */
typedef BOOL (WINAPI *PostMessageW_t)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *IsWindow_t)(HWND);

static struct Capture {
    HWND hwnd;
    UINT period;
    BOOL changed;
    volatile LONG stop;
    HANDLE thread;
} g_capture[2];
static CRITICAL_SECTION g_capture_lock;
static INIT_ONCE g_capture_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK capture_init(INIT_ONCE *once, void *p, void **ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_capture_lock);
    return TRUE;
}

static DWORD WINAPI capture_thread(void *p)
{
    struct Capture *c = p;
    UINT id = (UINT)(c - g_capture);
    HMODULE u = LoadLibraryA("user32.dll");
    PostMessageW_t post = (PostMessageW_t)(void *)GetProcAddress(u, "PostMessageW");
    IsWindow_t is_window = (IsWindow_t)(void *)GetProcAddress(u, "IsWindow");
    JOYINFOEX last = { 0 };
    BOOL first = TRUE;
    while (!c->stop && post && is_window && is_window(c->hwnd)) {
        JOYINFOEX j;
        if (joy_read(id, &j) == JOYERR_NOERROR) {
            /* wParam carries the buttons held (JOY_BUTTON1-4) and, for a
             * button message, which changed (JOY_BUTTON1CHG << n) */
            WPARAM held = j.dwButtons & 0xF;
            DWORD th = g_threshold[id];
            #define MOVED(a, b) ((a) > (b) ? (a) - (b) : (b) - (a)) > th
            BOOL xy = first || MOVED(j.dwXpos, last.dwXpos) || MOVED(j.dwYpos, last.dwYpos);
            BOOL z = first || MOVED(j.dwZpos, last.dwZpos);
            #undef MOVED
            DWORD down = j.dwButtons & ~last.dwButtons & 0xF, up = last.dwButtons & ~j.dwButtons & 0xF;
            if (down) post(c->hwnd, MM_JOY1BUTTONDOWN + id, held | down << 8, MAKELPARAM(j.dwXpos, j.dwYpos));
            if (up) post(c->hwnd, MM_JOY1BUTTONUP + id, held | up << 8, MAKELPARAM(j.dwXpos, j.dwYpos));
            if (xy || !c->changed) post(c->hwnd, MM_JOY1MOVE + id, held, MAKELPARAM(j.dwXpos, j.dwYpos));
            if (z) post(c->hwnd, MM_JOY1ZMOVE + id, held, MAKELPARAM(j.dwZpos, 0));
            last = j;
            first = FALSE;
        }
        Sleep(c->period);
    }
    return 0;
}

MMAPI MMRESULT WINAPI joySetCapture(HWND hwnd, UINT id, UINT period, BOOL changed)
{
    NovaPadInfo info;
    if (!hwnd) return JOYERR_PARMS;
    if (id >= 2) return JOYERR_PARMS;                     /* (only JOYSTICKID1 and 2 capture) */
    if (joy_slot(id, &info) < 0) return JOYERR_UNPLUGGED;
    InitOnceExecuteOnce(&g_capture_once, capture_init, NULL, NULL);
    EnterCriticalSection(&g_capture_lock);
    struct Capture *c = &g_capture[id];
    MMRESULT r = JOYERR_NOERROR;
    if (c->thread && WaitForSingleObject(c->thread, 0) == WAIT_TIMEOUT) {
        r = JOYERR_NOCANDO;                               /* already captured */
    } else {
        if (c->thread) CloseHandle(c->thread);
        c->hwnd = hwnd;
        c->period = period < 10 ? 10 : period > 1000 ? 1000 : period;
        c->changed = changed;
        c->stop = 0;
        c->thread = CreateThread(NULL, 0, capture_thread, c, 0, NULL);
        if (!c->thread) r = JOYERR_NOCANDO;
    }
    LeaveCriticalSection(&g_capture_lock);
    return r;
}

MMAPI MMRESULT WINAPI joyReleaseCapture(UINT id)
{
    if (id >= 2) return JOYERR_PARMS;
    InitOnceExecuteOnce(&g_capture_once, capture_init, NULL, NULL);
    EnterCriticalSection(&g_capture_lock);
    struct Capture *c = &g_capture[id];
    if (c->thread) {
        InterlockedExchange(&c->stop, 1);
        WaitForSingleObject(c->thread, 2000);
        CloseHandle(c->thread);
        c->thread = NULL;
    }
    LeaveCriticalSection(&g_capture_lock);
    return JOYERR_NOERROR;
}
