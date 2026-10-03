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
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NOERROR 0
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

/* timeSetEvent: a thread per timer calls @fn (TIME_ONESHOT 0 / TIME_PERIODIC 1),
 * woken by a kernel waitable timer (to the TSC, not the 10 ms tick; a
 * periodic one on its own grid, so it doesn't drift) */
typedef void (CALLBACK *LPTIMECALLBACK)(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR r1, DWORD_PTR r2);
typedef struct Timer {
    struct Timer *next;
    UINT id, delay, flags;
    LPTIMECALLBACK fn;
    DWORD_PTR user;
    HANDLE stop, thread, timer;
} Timer;
static Timer *g_timers;
static SRWLOCK g_lock;
static UINT g_next_id = 1;

static DWORD WINAPI timer_thread(LPVOID p)
{
    Timer *t = p;
    HANDLE h[2] = { t->stop, t->timer };
    for (;;) {
        if (WaitForMultipleObjects(2, h, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        if (t->flags & 0x10 /* TIME_CALLBACK_EVENT_SET */) SetEvent((HANDLE)t->fn);
        else if (t->flags & 0x20 /* TIME_CALLBACK_EVENT_PULSE */) { SetEvent((HANDLE)t->fn); ResetEvent((HANDLE)t->fn); }
        else t->fn(t->id, 0, t->user, 0, 0);
        if (!(t->flags & 1)) break;                         /* one shot */
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
    t->timer = CreateWaitableTimerW(0, FALSE, 0);
    LARGE_INTEGER due;
    due.QuadPart = delay ? -(LONGLONG)delay * 10000 : -1;
    if (!t->stop || !t->timer || !SetWaitableTimer(t->timer, &due, flags & 1 ? (LONG)(delay ? delay : 1) : 0, 0, 0, FALSE)) {
        if (t->stop) CloseHandle(t->stop);
        if (t->timer) CloseHandle(t->timer);
        LocalFree(t);
        return 0;
    }
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
    CloseHandle(t->timer);
    LocalFree(t);
    return TIMERR_NOERROR;
}

/* ---- audio: playback is wave.c, recording wavein.c; no MIDI or mixer devices ---- */
MMAPI UINT WINAPI midiOutGetNumDevs(void) { return 0; }
MMAPI UINT WINAPI midiInGetNumDevs(void)  { return 0; }
MMAPI UINT WINAPI mixerGetNumDevs(void)   { return 0; }
MMAPI UINT WINAPI auxGetNumDevs(void)     { return 0; }
MMAPI MMRESULT WINAPI midiOutOpen(HANDLE *h, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)dev; (void)cb; (void)inst; (void)flags; if (h) *h = 0; return MMSYSERR_NODRIVER; }
/* MIDI: no ports to open, so every handle is invalid */
MMAPI MMRESULT WINAPI midiInOpen(HANDLE *h, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)dev; (void)cb; (void)inst; (void)flags; if (h) *h = 0; return MMSYSERR_NODRIVER; }
MMAPI MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
static MMRESULT no_midi(void) { return 5; }                /* MMSYSERR_INVALHANDLE */
MMAPI MMRESULT WINAPI midiStreamOpen(HANDLE *h, UINT *dev, DWORD n, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)dev; (void)n; (void)cb; (void)inst; (void)flags; if (h) *h = 0; return MMSYSERR_NODRIVER; }
MMAPI MMRESULT WINAPI midiStreamClose(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiStreamOut(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiStreamPosition(HANDLE h, void *t, UINT n) { (void)h; (void)t; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiStreamProperty(HANDLE h, LPBYTE prop, DWORD flags) { (void)h; (void)prop; (void)flags; return no_midi(); }
MMAPI MMRESULT WINAPI midiStreamRestart(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiStreamStop(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiStreamPause(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiInClose(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiInStart(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiInStop(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiInReset(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiInAddBuffer(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiInPrepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiInUnprepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiInMessage(HANDLE h, UINT msg, DWORD_PTR a, DWORD_PTR b) { (void)h; (void)msg; (void)a; (void)b; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutClose(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutReset(HANDLE h) { (void)h; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutShortMsg(HANDLE h, DWORD m) { (void)h; (void)m; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutLongMsg(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutPrepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutUnprepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return no_midi(); }
MMAPI MMRESULT WINAPI midiOutMessage(HANDLE h, UINT msg, DWORD_PTR a, DWORD_PTR b) { (void)h; (void)msg; (void)a; (void)b; return no_midi(); }

MMAPI MMRESULT WINAPI midiInGetErrorTextW(MMRESULT e, LPWSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    const char *t = e == 0 ? "The specified command was carried out." : e == 2 ? "The specified device ID is out of range." : "There is no MIDI device.";
    MultiByteToWideChar(CP_UTF8, 0, t, -1, buf, (int)n);
    buf[n - 1] = 0;
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiInGetErrorTextA(MMRESULT e, LPSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    lstrcpynA(buf, e == 0 ? "The specified command was carried out." : e == 2 ? "The specified device ID is out of range." : "There is no MIDI device.", (int)n);
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT e, LPWSTR buf, UINT n) { return midiInGetErrorTextW(e, buf, n); }
MMAPI MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT e, LPSTR buf, UINT n) { return midiInGetErrorTextA(e, buf, n); }

/* ---- mixers: none (the endpoint volume is mmdevapi's) ---- */
MMAPI MMRESULT WINAPI mixerOpen(HANDLE *h, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    (void)id; (void)cb; (void)inst; (void)flags;
    if (h) *h = 0;
    return MMSYSERR_BADDEVICEID;
}
MMAPI MMRESULT WINAPI mixerClose(HANDLE h) { (void)h; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR id, void *caps, UINT n) { (void)id; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR id, void *caps, UINT n) { (void)id; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI mixerGetID(HANDLE h, UINT *id, DWORD flags) { (void)h; (void)flags; if (id) *id = (UINT)-1; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetLineInfoW(HANDLE h, void *line, DWORD flags) { (void)h; (void)line; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetLineInfoA(HANDLE h, void *line, DWORD flags) { (void)h; (void)line; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetLineControlsW(HANDLE h, void *ctl, DWORD flags) { (void)h; (void)ctl; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetLineControlsA(HANDLE h, void *ctl, DWORD flags) { (void)h; (void)ctl; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetControlDetailsW(HANDLE h, void *det, DWORD flags) { (void)h; (void)det; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerGetControlDetailsA(HANDLE h, void *det, DWORD flags) { (void)h; (void)det; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI mixerSetControlDetails(HANDLE h, void *det, DWORD flags) { (void)h; (void)det; (void)flags; return MMSYSERR_INVALHANDLE; }
MMAPI DWORD WINAPI mixerMessage(HANDLE h, UINT msg, DWORD_PTR a, DWORD_PTR b) { (void)h; (void)msg; (void)a; (void)b; return MMSYSERR_INVALHANDLE; }

/* ---- joysticks: none ---- */
MMAPI MMRESULT WINAPI joyGetThreshold(UINT id, UINT *t) { (void)id; if (t) *t = 0; return JOYERR_UNPLUGGED; }
MMAPI MMRESULT WINAPI joySetThreshold(UINT id, UINT t) { (void)id; (void)t; return JOYERR_UNPLUGGED; }
MMAPI MMRESULT WINAPI joySetCapture(HWND w, UINT id, UINT period, BOOL changed) { (void)w; (void)id; (void)period; (void)changed; return JOYERR_UNPLUGGED; }
MMAPI MMRESULT WINAPI joyReleaseCapture(UINT id) { (void)id; return JOYERR_UNPLUGGED; }
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
