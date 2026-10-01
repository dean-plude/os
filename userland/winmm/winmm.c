/*
 * winmm.dll — multimedia timers, and the audio/joystick/MCI entry points.
 * Timers work; sound output (waveOut, PlaySound) is in wave.c.  There are
 * no recording, MIDI or game-controller drivers: those report that no
 * devices exist (the answer programs handle).
 */

#include <windows.h>

#define MMAPI __declspec(dllexport)
typedef UINT MMRESULT;
#define TIMERR_NOERROR      0
#define TIMERR_NOCANDO      97
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NODRIVER   6
#define MMSYSERR_INVALPARAM 11
#define JOYERR_UNPLUGGED    167
#define MCIERR_DEVICE_NOT_INSTALLED 275

/* ---- time ---- */
MMAPI DWORD WINAPI timeGetTime(void) { return (DWORD)GetTickCount64(); }
MMAPI MMRESULT WINAPI timeBeginPeriod(UINT ms) { return ms ? TIMERR_NOERROR : TIMERR_NOCANDO; }
MMAPI MMRESULT WINAPI timeEndPeriod(UINT ms) { return ms ? TIMERR_NOERROR : TIMERR_NOCANDO; }

typedef struct { UINT wPeriodMin, wPeriodMax; } TIMECAPS;
MMAPI MMRESULT WINAPI timeGetDevCaps(TIMECAPS *tc, UINT n)
{
    if (!tc || n < sizeof(*tc)) return TIMERR_NOCANDO;
    tc->wPeriodMin = 1;
    tc->wPeriodMax = 1000000;
    return TIMERR_NOERROR;
}

typedef struct { UINT wType; union { DWORD ms; } u; } MMTIME;
MMAPI MMRESULT WINAPI timeGetSystemTime(MMTIME *t, UINT n)
{
    if (!t || n < sizeof(*t)) return MMSYSERR_INVALPARAM;
    t->wType = 1;                                           /* TIME_MS */
    t->u.ms = timeGetTime();
    return TIMERR_NOERROR;
}

/* timeSetEvent: a thread per timer calls @fn (TIME_ONESHOT 0 / TIME_PERIODIC 1) */
typedef void (CALLBACK *LPTIMECALLBACK)(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR r1, DWORD_PTR r2);
typedef struct Timer {
    struct Timer *next;
    UINT id, delay, flags;
    LPTIMECALLBACK fn;
    DWORD_PTR user;
    HANDLE stop, thread;
} Timer;
static Timer *g_timers;
static SRWLOCK g_lock;
static UINT g_next_id = 1;

static DWORD WINAPI timer_thread(LPVOID p)
{
    Timer *t = p;
    ULONGLONG next = GetTickCount64() + t->delay;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD wait = next > now ? (DWORD)(next - now) : 0;
        if (WaitForSingleObject(t->stop, wait) == WAIT_OBJECT_0) break;
        if (t->flags & 0x10 /* TIME_CALLBACK_EVENT_SET */) SetEvent((HANDLE)t->fn);
        else if (t->flags & 0x20 /* TIME_CALLBACK_EVENT_PULSE */) { SetEvent((HANDLE)t->fn); ResetEvent((HANDLE)t->fn); }
        else t->fn(t->id, 0, t->user, 0, 0);
        if (!(t->flags & 1)) break;                         /* one shot */
        next += t->delay ? t->delay : 1;
    }
    return 0;
}

MMAPI MMRESULT WINAPI timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK fn, DWORD_PTR user, UINT flags)
{
    (void)res;
    Timer *t = LocalAlloc(LMEM_ZEROINIT, sizeof(*t));
    if (!t) return 0;
    t->delay = delay; t->fn = fn; t->user = user; t->flags = flags;
    t->stop = CreateEventW(0, TRUE, FALSE, 0);
    AcquireSRWLockExclusive(&g_lock);
    t->id = g_next_id++;
    t->next = g_timers;
    g_timers = t;
    ReleaseSRWLockExclusive(&g_lock);
    t->thread = CreateThread(0, 64 * 1024, timer_thread, t, 0, 0);
    return t->thread ? t->id : 0;
}

MMAPI MMRESULT WINAPI timeKillEvent(UINT id)
{
    AcquireSRWLockExclusive(&g_lock);
    Timer **pp = &g_timers, *t = 0;
    while (*pp && (*pp)->id != id) pp = &(*pp)->next;
    if (*pp) { t = *pp; *pp = t->next; }
    ReleaseSRWLockExclusive(&g_lock);
    if (!t) return MMSYSERR_INVALPARAM;
    SetEvent(t->stop);
    if (GetCurrentThreadId() != GetThreadId(t->thread)) WaitForSingleObject(t->thread, INFINITE);
    CloseHandle(t->thread);
    CloseHandle(t->stop);
    LocalFree(t);
    return TIMERR_NOERROR;
}

/* ---- audio: playback is wave.c; no recording, MIDI or mixer devices ---- */
MMAPI UINT WINAPI waveInGetNumDevs(void)  { return 0; }
MMAPI UINT WINAPI midiOutGetNumDevs(void) { return 0; }
MMAPI UINT WINAPI midiInGetNumDevs(void)  { return 0; }
MMAPI UINT WINAPI mixerGetNumDevs(void)   { return 0; }
MMAPI UINT WINAPI auxGetNumDevs(void)     { return 0; }
MMAPI MMRESULT WINAPI waveInOpen(HANDLE *h, UINT dev, const void *fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)dev; (void)fmt; (void)cb; (void)inst; (void)flags; if (h) *h = 0; return MMSYSERR_NODRIVER; }
MMAPI MMRESULT WINAPI midiOutOpen(HANDLE *h, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)dev; (void)cb; (void)inst; (void)flags; if (h) *h = 0; return MMSYSERR_NODRIVER; }
MMAPI MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }

/* ---- joysticks: none ---- */
MMAPI UINT WINAPI joyGetNumDevs(void) { return 16; }        /* slots; each reports "unplugged" */
MMAPI MMRESULT WINAPI joyGetPosEx(UINT id, void *info) { (void)id; (void)info; return JOYERR_UNPLUGGED; }
MMAPI MMRESULT WINAPI joyGetPos(UINT id, void *info) { (void)id; (void)info; return JOYERR_UNPLUGGED; }
MMAPI MMRESULT WINAPI joyGetDevCapsW(UINT_PTR id, void *caps, UINT n) { (void)id; (void)caps; (void)n; return JOYERR_UNPLUGGED; }

/* ---- MCI ---- */
MMAPI DWORD WINAPI mciSendStringW(LPCWSTR cmd, LPWSTR ret, UINT n, HWND cb)
{
    (void)cmd; (void)cb;
    if (ret && n) ret[0] = 0;
    return MCIERR_DEVICE_NOT_INSTALLED;
}
MMAPI DWORD WINAPI mciSendStringA(LPCSTR cmd, LPSTR ret, UINT n, HWND cb)
{
    (void)cmd; (void)cb;
    if (ret && n) ret[0] = 0;
    return MCIERR_DEVICE_NOT_INSTALLED;
}
MMAPI DWORD WINAPI mciSendCommandW(UINT id, UINT msg, DWORD_PTR p1, DWORD_PTR p2) { (void)id; (void)msg; (void)p1; (void)p2; return MCIERR_DEVICE_NOT_INSTALLED; }
MMAPI BOOL WINAPI mciGetErrorStringW(DWORD e, LPWSTR buf, UINT n)
{
    const char *t = e == MCIERR_DEVICE_NOT_INSTALLED ? "The specified device is not installed." : "Unknown MCI error.";
    if (!buf || !n) return FALSE;
    MultiByteToWideChar(CP_UTF8, 0, t, -1, buf, (int)n);
    buf[n - 1] = 0;
    return TRUE;
}
