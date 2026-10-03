/*
 * wave.c — waveOut and PlaySound on NovaOS's mixer
 *
 * Each waveOut device handle owns a kernel stream (NtNovaAudioOpen) and a
 * thread that converts the queued WAVEHDR buffers to 48 kHz s16 stereo,
 * keeps the stream fed, and returns each buffer (WHDR_DONE, WOM_DONE) once
 * the hardware has played its last frame.  PlaySound plays a RIFF WAVE
 * file, resource or memory image on a stream of its own.
 */

#include <windows.h>
#include <winternl.h>
#include "audioconv.h"

#include "mmwave.h"
#include "audiodev.h"

int _fltused = 1;                  /* floats are used (the converter) */

/* (asked each time: a USB speaker can be plugged in or out) */
static BOOL device_present(void)
{
    struct { UINT32 present, rate; char name[96]; } info = { 0 };
    return NtNovaAudioCtl(0, 5, 0, &info) == 0 && info.present;
}

static ULONGLONG played_frames(INT_PTR s)
{
    StreamStatus st;
    return NtNovaAudioCtl(s, 0, 0, &st) == 0 ? st.played : 0;
}

/* user32, for window and thread callbacks (winmm does not import it) */
BOOL mm_post(BOOL thread, DWORD_PTR target, UINT msg, WPARAM wp, LPARAM lp)
{
    static BOOL (WINAPI *pm)(HWND, UINT, WPARAM, LPARAM);
    static BOOL (WINAPI *ptm)(DWORD, UINT, WPARAM, LPARAM);
    if (!pm) {
        HMODULE u = LoadLibraryA("user32.dll");
        ptm = (void *)GetProcAddress(u, "PostThreadMessageW");
        pm = (void *)GetProcAddress(u, "PostMessageW");
    }
    if (thread) return ptm && ptm((DWORD)target, msg, wp, lp);
    return pm && pm((HWND)target, msg, wp, lp);
}

/* -----------------------------------------------------------------------
 * waveOut
 * ----------------------------------------------------------------------- */
#define WO_MAGIC  0x4F57564E        /* "NVWO" */
#define CHUNK     960               /* output frames converted at a time (20 ms) */

typedef struct WaveOut {
    DWORD       magic;
    INT_PTR     stream;
    UINT32      device;             /* the device chosen (0: the default) */
    AudioConv   conv;
    AcWaveFormat fmt;
    DWORD       cbtype;
    DWORD_PTR   cb, inst;
    CRITICAL_SECTION lock;
    WAVEHDR    *head, *tail;        /* queued, oldest first */
    WAVEHDR    *cur;                /* the buffer being converted (in the queue) */
    DWORD       cur_off;            /* bytes of it converted */
    SHORT       pend[CHUNK * 2];    /* converted frames the stream had no room for */
    UINT        pend_n, pend_at;
    ULONGLONG   written;            /* frames accepted by the stream since the last reset */
    ULONGLONG   src_written;        /* source frames converted since the last reset */
    BOOL        paused, quit;
    HANDLE      thread, wake;
    DWORD       volume;
    struct WaveOut *next;
} WaveOut;

static WaveOut *g_waveouts;
static SRWLOCK  g_wo_lock;
static DWORD    g_default_volume = 0xFFFFFFFF;

static void notify(WaveOut *w, UINT msg, DWORD_PTR p1)
{
    switch (w->cbtype) {
    case CALLBACK_FUNCTION:
        if (w->cb) ((void (CALLBACK *)(HANDLE, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR))w->cb)((HANDLE)w, msg, w->inst, p1, 0);
        break;
    case CALLBACK_WINDOW: mm_post(FALSE, w->cb, msg, (WPARAM)w, (LPARAM)p1); break;
    case CALLBACK_THREAD: mm_post(TRUE, w->cb, msg, (WPARAM)w, (LPARAM)p1); break;
    case CALLBACK_EVENT:  SetEvent((HANDLE)w->cb); break;
    }
}

static WaveOut *wo_get(HANDLE h)
{
    WaveOut *w = (WaveOut *)h;
    AcquireSRWLockShared(&g_wo_lock);
    WaveOut *x = g_waveouts;
    while (x && x != w) x = x->next;
    ReleaseSRWLockShared(&g_wo_lock);
    return x && x->magic == WO_MAGIC ? x : 0;
}

/* Feed the stream: converted leftovers first, then the queued buffers */
static void wo_feed(WaveOut *w)
{
    for (;;) {
        if (w->pend_n) {
            LONG_PTR got = NtNovaAudioWrite(w->stream, w->pend + w->pend_at * 2, w->pend_n);
            if (got <= 0) return;
            w->written += (ULONGLONG)got;
            w->pend_at += (UINT)got;
            w->pend_n -= (UINT)got;
            if (w->pend_n) return;                      /* stream full */
        }
        if (!w->cur) return;
        WAVEHDR *h = w->cur;
        DWORD frames_left = (h->dwBufferLength - w->cur_off) / w->conv.block;
        if (!frames_left) {
            h->reserved = (DWORD_PTR)w->written;        /* done once this frame has played */
            w->cur = h->lpNext;
            w->cur_off = 0;
            continue;
        }
        UINT n = ac_src_for(&w->conv, CHUNK);
        if (n > frames_left) n = frames_left;
        w->pend_n = ac_convert(&w->conv, h->lpData + w->cur_off, n, w->pend, CHUNK);
        w->pend_at = 0;
        w->cur_off += n * w->conv.block;
        w->src_written += n;
    }
}

/* Return the buffers the hardware has finished with */
static void wo_retire(WaveOut *w)
{
    ULONGLONG played = played_frames(w->stream);
    for (;;) {
        EnterCriticalSection(&w->lock);
        WAVEHDR *h = w->head;
        if (!h || h == w->cur || (ULONGLONG)h->reserved > played) { LeaveCriticalSection(&w->lock); return; }
        w->head = h->lpNext;
        if (!w->head) w->tail = 0;
        h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
        LeaveCriticalSection(&w->lock);
        notify(w, WOM_DONE, (DWORD_PTR)h);
    }
}

static DWORD WINAPI wo_thread(LPVOID p)
{
    WaveOut *w = p;
    while (!w->quit) {
        EnterCriticalSection(&w->lock);
        if (!w->paused) wo_feed(w);
        LeaveCriticalSection(&w->lock);
        wo_retire(w);
        WaitForSingleObject(w->wake, 10);
    }
    return 0;
}

/* One device ID for each output NovaOS has, oldest first (Settings lists
 * them in the same order); WAVE_MAPPER plays on the default one */
MMAPI UINT WINAPI waveOutGetNumDevs(void)
{
    AudioDeviceList l;
    return audio_devices(0, &l);
}

typedef struct {
    WORD wMid, wPid;
    UINT vDriverVersion;
    WCHAR szPname[32];
    DWORD dwFormats;
    WORD wChannels, wReserved1;
    DWORD dwSupport;
    GUID ManufacturerGuid, ProductGuid, NameGuid;       /* WAVEOUTCAPS2W */
} WAVEOUTCAPSW;
typedef struct {
    WORD wMid, wPid;
    UINT vDriverVersion;
    CHAR szPname[32];
    DWORD dwFormats;
    WORD wChannels, wReserved1;
    DWORD dwSupport;
    GUID ManufacturerGuid, ProductGuid, NameGuid;
} WAVEOUTCAPSA;

static MMRESULT check_device(UINT_PTR dev)
{
    AudioDeviceList l;
    UINT n = audio_devices(0, &l);
    if (!n) return MMSYSERR_NODRIVER;
    if (dev < n || dev == WAVE_MAPPER || dev == (UINT_PTR)-1 || wo_get((HANDLE)dev)) return MMSYSERR_NOERROR;
    return MMSYSERR_BADDEVICEID;
}

static void fill_caps(WAVEOUTCAPSW *c, UINT_PTR dev)
{
    memset(c, 0, sizeof(*c));
    BOOL mapper = dev == WAVE_MAPPER || dev == (UINT_PTR)-1;
    WaveOut *w = mapper ? NULL : wo_get((HANDLE)dev);
    c->wMid = 1;                                    /* MM_MICROSOFT */
    c->wPid = mapper ? 2 : 100;                     /* MM_WAVE_MAPPER / generic */
    c->vDriverVersion = 0x0600;
    AudioDeviceList l;
    UINT n = audio_devices(0, &l), k = w ? n : (UINT)dev;
    for (UINT i = 0; w && i < n; i++)               /* (a handle: its device) */
        if (l.dev[i].id == w->device || (!w->device && l.dev[i].is_default)) k = i;
    const char *name = mapper ? "Microsoft Sound Mapper" : k < n ? l.dev[k].name : "Speakers";
    for (int i = 0; name[i] && i < 31; i++) c->szPname[i] = (WCHAR)(BYTE)name[i];
    c->dwFormats = 0x000FFFFF;                      /* every WAVE_FORMAT_* rate/width/channel combination */
    c->wChannels = 2;
    c->dwSupport = 0x0004 | 0x0008 | 0x0020;        /* WAVECAPS_VOLUME | LRVOLUME | SAMPLEACCURATE */
}

MMAPI MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR dev, WAVEOUTCAPSW *caps, UINT n)
{
    MMRESULT r = check_device(dev);
    if (r) return r;
    if (!caps) return MMSYSERR_INVALPARAM;
    WAVEOUTCAPSW c;
    fill_caps(&c, dev);
    memcpy(caps, &c, n < sizeof(c) ? n : sizeof(c));
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR dev, WAVEOUTCAPSA *caps, UINT n)
{
    MMRESULT r = check_device(dev);
    if (r) return r;
    if (!caps) return MMSYSERR_INVALPARAM;
    WAVEOUTCAPSW w;
    WAVEOUTCAPSA a;
    fill_caps(&w, dev);
    memset(&a, 0, sizeof(a));
    a.wMid = w.wMid; a.wPid = w.wPid; a.vDriverVersion = w.vDriverVersion;
    for (int i = 0; i < 32; i++) a.szPname[i] = (CHAR)w.szPname[i];
    a.dwFormats = w.dwFormats; a.wChannels = w.wChannels; a.dwSupport = w.dwSupport;
    memcpy(caps, &a, n < sizeof(a) ? n : sizeof(a));
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutOpen(HANDLE *out, UINT dev, const AcWaveFormat *fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    if (out) *out = 0;
    AudioDeviceList l;
    UINT n = audio_devices(0, &l);
    if (!n) return MMSYSERR_NODRIVER;
    if (dev >= n && dev != WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
    if (!fmt) return MMSYSERR_INVALPARAM;
    AudioConv conv;
    if (!ac_init(&conv, fmt)) return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    if (!out) return MMSYSERR_INVALPARAM;
    DWORD cbtype = flags & CALLBACK_TYPEMASK;
    if (cbtype && cbtype != CALLBACK_WINDOW && cbtype != CALLBACK_THREAD && cbtype != CALLBACK_FUNCTION &&
        cbtype != CALLBACK_EVENT) return MMSYSERR_INVALFLAG;

    WaveOut *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return MMSYSERR_NOMEM;
    w->stream = NtNovaAudioOpen(AC_RATE / 4);                  /* 250 ms queued at most */
    if (!w->stream) { HeapFree(GetProcessHeap(), 0, w); return MMSYSERR_ALLOCATED; }
    w->device = dev == WAVE_MAPPER ? 0 : l.dev[dev].id;        /* (the mapper: the default, wherever it moves) */
    if (w->device && !audio_route(w->stream, w->device)) {     /* (unplugged meanwhile) */
        CloseHandle((HANDLE)w->stream);
        HeapFree(GetProcessHeap(), 0, w);
        return MMSYSERR_BADDEVICEID;
    }
    w->magic = WO_MAGIC;
    w->conv = conv;
    w->fmt = *fmt;
    w->cbtype = cbtype;
    w->cb = cb;
    w->inst = inst;
    w->volume = g_default_volume;
    InitializeCriticalSection(&w->lock);
    if (w->volume != 0xFFFFFFFF) NtNovaAudioCtl(w->stream, 3, w->volume, 0);
    NtNovaAudioCtl(w->stream, 1, 1, 0);                         /* runs; waits for data */
    w->wake = CreateEventW(0, FALSE, FALSE, 0);
    AcquireSRWLockExclusive(&g_wo_lock);
    w->next = g_waveouts;
    g_waveouts = w;
    ReleaseSRWLockExclusive(&g_wo_lock);
    w->thread = CreateThread(0, 64 * 1024, wo_thread, w, 0, 0);
    if (w->thread) SetThreadPriority(w->thread, 15 /* THREAD_PRIORITY_TIME_CRITICAL */);
    *out = (HANDLE)w;
    notify(w, WOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutPrepareHeader(HANDLE h, WAVEHDR *hdr, UINT n)
{
    if (!wo_get(h)) return MMSYSERR_INVALHANDLE;
    if (!hdr || n < sizeof(WAVEHDR) - sizeof(DWORD_PTR) || !hdr->lpData) return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutUnprepareHeader(HANDLE h, WAVEHDR *hdr, UINT n)
{
    (void)n;
    if (!wo_get(h)) return MMSYSERR_INVALHANDLE;
    if (!hdr) return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags &= ~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutWrite(HANDLE h, WAVEHDR *hdr, UINT n)
{
    (void)n;
    WaveOut *w = wo_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    if (!hdr) return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags = (hdr->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
    hdr->lpNext = 0;
    hdr->reserved = (DWORD_PTR)-1;                  /* not converted yet */
    EnterCriticalSection(&w->lock);
    if (w->tail) w->tail->lpNext = hdr; else w->head = hdr;
    w->tail = hdr;
    if (!w->cur) { w->cur = hdr; w->cur_off = 0; }
    if (!w->paused) wo_feed(w);                     /* start at once */
    LeaveCriticalSection(&w->lock);
    SetEvent(w->wake);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutPause(HANDLE h)
{
    WaveOut *w = wo_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    w->paused = TRUE;
    NtNovaAudioCtl(w->stream, 1, 0, 0);
    LeaveCriticalSection(&w->lock);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutRestart(HANDLE h)
{
    WaveOut *w = wo_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    w->paused = FALSE;
    NtNovaAudioCtl(w->stream, 1, 1, 0);
    LeaveCriticalSection(&w->lock);
    SetEvent(w->wake);
    return MMSYSERR_NOERROR;
}

/* Stop, and hand every queued buffer back */
MMAPI MMRESULT WINAPI waveOutReset(HANDLE h)
{
    WaveOut *w = wo_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    NtNovaAudioCtl(w->stream, 2, 0, 0);
    WAVEHDR *list = w->head;
    w->head = w->tail = w->cur = 0;
    w->cur_off = 0;
    w->pend_n = 0;
    w->written = w->src_written = 0;
    w->conv.pos = 0;
    w->conv.prev[0] = w->conv.prev[1] = 0;
    w->paused = FALSE;
    NtNovaAudioCtl(w->stream, 1, 1, 0);
    LeaveCriticalSection(&w->lock);
    while (list) {
        WAVEHDR *next = list->lpNext;
        list->dwFlags = (list->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
        notify(w, WOM_DONE, (DWORD_PTR)list);
        list = next;
    }
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutClose(HANDLE h)
{
    WaveOut *w = wo_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    BOOL busy = w->head != 0;
    LeaveCriticalSection(&w->lock);
    if (busy) return WAVERR_STILLPLAYING;
    AcquireSRWLockExclusive(&g_wo_lock);
    WaveOut **pp = &g_waveouts;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    ReleaseSRWLockExclusive(&g_wo_lock);
    w->quit = TRUE;
    SetEvent(w->wake);
    if (w->thread) {
        if (GetCurrentThreadId() != GetThreadId(w->thread)) WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
    }
    notify(w, WOM_CLOSE, 0);
    CloseHandle((HANDLE)w->stream);
    CloseHandle(w->wake);
    DeleteCriticalSection(&w->lock);
    w->magic = 0;
    HeapFree(GetProcessHeap(), 0, w);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutGetPosition(HANDLE h, MMTIME *t, UINT n)
{
    WaveOut *w = wo_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    if (!t || n < sizeof(UINT) + sizeof(DWORD)) return MMSYSERR_INVALPARAM;
    ULONGLONG played = played_frames(w->stream);
    EnterCriticalSection(&w->lock);
    ULONGLONG src = w->written ? (ULONGLONG)((double)played * w->src_written / (w->written + w->pend_n)) : 0;
    LeaveCriticalSection(&w->lock);
    switch (t->wType) {
    case TIME_SAMPLES: t->u.sample = (DWORD)src; break;
    case TIME_MS:      t->u.ms = (DWORD)(src * 1000 / w->conv.rate); break;
    default:           t->wType = TIME_BYTES; t->u.cb = (DWORD)(src * w->conv.block); break;
    }
    return MMSYSERR_NOERROR;
}

/* Volume: low word left, high word right.  On a device ID it is the
 * volume for this program's waveOut handles. */
MMAPI MMRESULT WINAPI waveOutSetVolume(HANDLE h, DWORD v)
{
    WaveOut *w = wo_get(h);
    if (w) { w->volume = v; NtNovaAudioCtl(w->stream, 3, v, 0); return MMSYSERR_NOERROR; }
    if (check_device((UINT_PTR)h)) return check_device((UINT_PTR)h);
    g_default_volume = v;
    AcquireSRWLockShared(&g_wo_lock);
    for (WaveOut *x = g_waveouts; x; x = x->next) { x->volume = v; NtNovaAudioCtl(x->stream, 3, v, 0); }
    ReleaseSRWLockShared(&g_wo_lock);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutGetVolume(HANDLE h, LPDWORD v)
{
    if (!v) return MMSYSERR_INVALPARAM;
    WaveOut *w = wo_get(h);
    if (w) { *v = w->volume; return MMSYSERR_NOERROR; }
    MMRESULT r = check_device((UINT_PTR)h);
    if (r) { *v = 0; return r; }
    *v = g_default_volume;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutGetID(HANDLE h, UINT *id)
{
    if (!wo_get(h)) return MMSYSERR_INVALHANDLE;
    if (!id) return MMSYSERR_INVALPARAM;
    *id = 0;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutBreakLoop(HANDLE h) { return wo_get(h) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI waveOutGetPitch(HANDLE h, LPDWORD v) { (void)v; return wo_get(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI waveOutSetPitch(HANDLE h, DWORD v) { (void)v; return wo_get(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI waveOutGetPlaybackRate(HANDLE h, LPDWORD v) { (void)v; return wo_get(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI waveOutSetPlaybackRate(HANDLE h, DWORD v) { (void)v; return wo_get(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI waveOutMessage(HANDLE h, UINT msg, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h; (void)msg; (void)p1; (void)p2;
    return MMSYSERR_NOTSUPPORTED;
}

const char *mm_error_text(MMRESULT e)
{
    switch (e) {
    case MMSYSERR_NOERROR:      return "The specified command was carried out.";
    case MMSYSERR_BADDEVICEID:  return "A device ID has been used that is out of range for your system.";
    case MMSYSERR_ALLOCATED:    return "The specified device is already in use.  Wait until it is free, and then try again.";
    case MMSYSERR_INVALHANDLE:  return "The specified device handle is invalid.";
    case MMSYSERR_NODRIVER:     return "There is no driver installed on your system.";
    case MMSYSERR_NOMEM:        return "There is not enough memory available for this task.";
    case MMSYSERR_NOTSUPPORTED: return "This function is not supported.";
    case MMSYSERR_INVALPARAM:   return "An invalid parameter was passed to a system function.";
    case WAVERR_BADFORMAT:      return "The specified format is not supported or cannot be translated.";
    case WAVERR_STILLPLAYING:   return "Cannot perform this operation while media data is still playing.";
    case WAVERR_UNPREPARED:     return "The wave header was not prepared.";
    }
    return "An unknown error occurred.";
}

MMAPI MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT e, LPWSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    MultiByteToWideChar(CP_UTF8, 0, mm_error_text(e), -1, buf, (int)n);
    buf[n - 1] = 0;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT e, LPSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    lstrcpynA(buf, mm_error_text(e), (int)n);
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * PlaySound
 * ----------------------------------------------------------------------- */
#define SND_ASYNC       0x0001
#define SND_NODEFAULT   0x0002
#define SND_MEMORY      0x0004
#define SND_LOOP        0x0008
#define SND_NOSTOP      0x0010
#define SND_PURGE       0x0040
#define SND_ALIAS       0x00010000
#define SND_FILENAME    0x00020000
#define SND_RESOURCE    0x00040004
#define SND_ALIAS_ID    0x00110000

typedef struct {
    BYTE  *mem;                     /* owned copy of the RIFF image */
    const AcWaveFormat *fmt;
    const BYTE *data;
    DWORD  len;
    BOOL   loop;
} Sound;

static struct {
    SRWLOCK  lock;
    HANDLE   thread;
    volatile LONG stop;
} g_ps;

/* Find "fmt " and "data" in a RIFF WAVE image of @size bytes */
static BOOL parse_wave(Sound *s, DWORD size)
{
    const BYTE *p = s->mem;
    if (size < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return FALSE;
    DWORD riff = *(const DWORD *)(p + 4) + 8;
    if (riff < size) size = riff;
    s->fmt = 0; s->data = 0;
    for (DWORD at = 12; at + 8 <= size;) {
        DWORD len = *(const DWORD *)(p + at + 4);
        if (len > size - at - 8) len = size - at - 8;
        if (!memcmp(p + at, "fmt ", 4) && len >= 16) s->fmt = (const AcWaveFormat *)(p + at + 8);
        else if (!memcmp(p + at, "data", 4)) { s->data = p + at + 8; s->len = len; }
        at += 8 + len + (len & 1);
    }
    return s->fmt && s->data;
}

static BOOL load_file(Sound *s, LPCWSTR path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) {
        /* a bare name: C:\Windows\Media, as Windows looks there too */
        WCHAR alt[MAX_PATH];
        for (LPCWSTR q = path; *q; q++) if (*q == L'\\' || *q == L'/' || *q == L':') return FALSE;
        if (lstrlenW(path) > MAX_PATH - 32) return FALSE;
        lstrcpyW(alt, L"C:\\Windows\\Media\\");
        lstrcatW(alt, path);
        f = CreateFileW(alt, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (f == INVALID_HANDLE_VALUE) return FALSE;
    }
    DWORD size = GetFileSize(f, 0), got = 0;
    if (size == INVALID_FILE_SIZE || size > 256u << 20) { CloseHandle(f); return FALSE; }
    s->mem = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    BOOL ok = s->mem && ReadFile(f, s->mem, size, &got, 0) && got == size;
    CloseHandle(f);
    return ok && parse_wave(s, size);
}

static BOOL load_memory(Sound *s, const void *p)
{
    __try {
        const BYTE *b = p;
        if (memcmp(b, "RIFF", 4)) return FALSE;
        DWORD size = *(const DWORD *)(b + 4) + 8;
        s->mem = HeapAlloc(GetProcessHeap(), 0, size);
        if (!s->mem) return FALSE;
        memcpy(s->mem, b, size);
        return parse_wave(s, size);
    } __except (1) {
        return FALSE;
    }
}

static BOOL load_resource(Sound *s, HMODULE mod, LPCWSTR name)
{
    HRSRC r = FindResourceW(mod, name, L"WAVE");
    if (!r) return FALSE;
    HGLOBAL g = LoadResource(mod, r);
    DWORD size = SizeofResource(mod, r);
    const void *p = g ? LockResource(g) : 0;
    if (!p || !size) return FALSE;
    s->mem = HeapAlloc(GetProcessHeap(), 0, size);
    if (!s->mem) return FALSE;
    memcpy(s->mem, p, size);
    return parse_wave(s, size);
}

/* A system sound name -> its file (HKCU\AppEvents\Schemes\Apps\.Default\NAME\.Current) */
static BOOL load_alias(Sound *s, LPCWSTR alias)
{
    WCHAR key[300], file[MAX_PATH];
    if (lstrlenW(alias) > 200) return FALSE;
    lstrcpyW(key, L"AppEvents\\Schemes\\Apps\\.Default\\");
    lstrcatW(key, alias);
    lstrcatW(key, L"\\.Current");
    DWORD len = sizeof(file), type;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k)) return FALSE;
    LONG e = RegQueryValueExW(k, 0, 0, &type, (BYTE *)file, &len);
    RegCloseKey(k);
    if (e || (type != REG_SZ && type != REG_EXPAND_SZ) || !file[0]) return FALSE;
    WCHAR full[MAX_PATH];
    if (!ExpandEnvironmentStringsW(file, full, MAX_PATH)) return FALSE;
    return load_file(s, full);
}

/* NovaOS ships no .wav files: the default sound is a short two-note chime */
static BOOL make_default(Sound *s)
{
    const DWORD rate = 48000, frames = rate * 35 / 100;
    DWORD bytes = frames * 4;
    s->mem = HeapAlloc(GetProcessHeap(), 0, 44 + bytes);
    if (!s->mem) return FALSE;
    BYTE *p = s->mem;
    AcWaveFormat f = { 1, 2, rate, rate * 4, 4, 16, 0 };
    memcpy(p, "RIFF", 4); *(DWORD *)(p + 4) = 36 + bytes; memcpy(p + 8, "WAVEfmt ", 8);
    *(DWORD *)(p + 16) = 16; memcpy(p + 20, &f, 16); memcpy(p + 36, "data", 4); *(DWORD *)(p + 40) = bytes;
    SHORT *d = (SHORT *)(p + 44);
    double ph = 0;
    for (DWORD i = 0; i < frames; i++) {
        double t = (double)i / rate, freq = t < 0.12 ? 880.0 : 1318.5;
        double t0 = t < 0.12 ? t : t - 0.12, env = t0 < 0.005 ? t0 / 0.005 : 1.0;
        for (double k = t0; k > 0.02; k -= 0.02) env *= 0.86;      /* decay */
        ph += 2 * 3.14159265358979 * freq / rate;
        if (ph > 2 * 3.14159265358979) ph -= 2 * 3.14159265358979;
        double x = ph < 3.14159265358979 ? ph : ph - 2 * 3.14159265358979;  /* sine, by its Taylor series */
        double x2 = x * x, sn = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
        SHORT v = (SHORT)(sn * env * 9000);
        d[i * 2] = d[i * 2 + 1] = v;
    }
    return parse_wave(s, 44 + bytes);
}

static void free_sound(Sound *s) { if (s->mem) HeapFree(GetProcessHeap(), 0, s->mem); s->mem = 0; }

/* Play @s to the end (or until stopped); frees it */
static void play(Sound *s)
{
    AudioConv c;
    INT_PTR st = ac_init(&c, s->fmt) ? NtNovaAudioOpen(AC_RATE / 4) : 0;
    if (st) {
        SHORT buf[CHUNK * 2];
        NtNovaAudioCtl(st, 1, 1, 0);
        ULONGLONG written = 0;
        DWORD frames = s->len / c.block, at = 0;
        while (!g_ps.stop && frames) {
            if (at >= frames) {
                if (!s->loop) break;
                at = 0;
            }
            UINT n = ac_src_for(&c, CHUNK);
            if (n > frames - at) n = frames - at;
            UINT out = ac_convert(&c, s->data + (size_t)at * c.block, n, buf, CHUNK), done = 0;
            at += n;
            while (done < out && !g_ps.stop) {
                LONG_PTR got = NtNovaAudioWrite(st, buf + done * 2, out - done);
                if (got < 0) break;
                done += (UINT)got;
                written += (ULONGLONG)got;
                if (done < out) Sleep(10);
            }
        }
        while (!g_ps.stop && played_frames(st) < written) Sleep(10);
        CloseHandle((HANDLE)st);
    }
    free_sound(s);
}

static DWORD WINAPI play_thread(LPVOID p)
{
    play(p);
    HeapFree(GetProcessHeap(), 0, p);
    return 0;
}

static void stop_playing(void)
{
    AcquireSRWLockExclusive(&g_ps.lock);
    if (g_ps.thread) {
        InterlockedExchange(&g_ps.stop, 1);
        WaitForSingleObject(g_ps.thread, INFINITE);
        CloseHandle(g_ps.thread);
        g_ps.thread = 0;
    }
    InterlockedExchange(&g_ps.stop, 0);
    ReleaseSRWLockExclusive(&g_ps.lock);
}

static BOOL playing(void)
{
    return g_ps.thread && WaitForSingleObject(g_ps.thread, 0) == WAIT_TIMEOUT;
}

static const WCHAR *alias_from_id(UINT_PTR id)
{
    switch (id) {               /* SND_ALIAS_SYSTEM* ('S' | x << 8) */
    case 0x2A53: return L"SystemAsterisk";      case 0x4853: return L"SystemHand";
    case 0x3F53: return L"SystemQuestion";      case 0x2153: return L"SystemExclamation";
    case 0x5353: return L"SystemStart";         case 0x4553: return L"SystemExit";
    case 0x4453: return L"SystemDefault";       case 0x5753: return L"SystemWelcome";
    }
    return L".Default";
}

static BOOL play_sound(LPCWSTR name, HMODULE mod, DWORD flags)
{
    if (!name || (flags & SND_PURGE)) {                         /* stop */
        stop_playing();
        return TRUE;
    }
    if ((flags & SND_NOSTOP) && playing()) return FALSE;
    stop_playing();
    if (!device_present()) return FALSE;

    Sound *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s) return FALSE;
    BOOL ok;
    if ((flags & SND_ALIAS_ID) == SND_ALIAS_ID)      ok = load_alias(s, alias_from_id((UINT_PTR)name));
    else if ((flags & SND_RESOURCE) == SND_RESOURCE) ok = load_resource(s, mod ? mod : GetModuleHandleW(0), name);
    else if (flags & SND_MEMORY)                     ok = load_memory(s, name);
    else if (flags & SND_ALIAS)                      ok = load_alias(s, name);
    else if (flags & SND_FILENAME)                   ok = load_file(s, name);
    else                                             ok = load_alias(s, name) || load_file(s, name);
    if (!ok) {
        free_sound(s);
        if (flags & SND_NODEFAULT) { HeapFree(GetProcessHeap(), 0, s); return FALSE; }
        if (!load_alias(s, L".Default") && !make_default(s)) { HeapFree(GetProcessHeap(), 0, s); return FALSE; }
    }
    s->loop = (flags & SND_LOOP) && (flags & SND_ASYNC);
    if (flags & SND_ASYNC) {
        AcquireSRWLockExclusive(&g_ps.lock);
        g_ps.thread = CreateThread(0, 64 * 1024, play_thread, s, 0, 0);
        ReleaseSRWLockExclusive(&g_ps.lock);
        if (!g_ps.thread) { free_sound(s); HeapFree(GetProcessHeap(), 0, s); return FALSE; }
        return TRUE;
    }
    play(s);
    HeapFree(GetProcessHeap(), 0, s);
    return TRUE;
}

static LPWSTR widen(LPCSTR s, DWORD flags)
{
    if (!s || (flags & SND_MEMORY) || ((flags & SND_ALIAS_ID) == SND_ALIAS_ID) ||
        (((flags & SND_RESOURCE) == SND_RESOURCE) && IS_INTRESOURCE(s))) return (LPWSTR)s;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, 0, 0);
    LPWSTR w = HeapAlloc(GetProcessHeap(), 0, (size_t)n * sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

MMAPI BOOL WINAPI PlaySoundW(LPCWSTR s, HMODULE m, DWORD flags) { return play_sound(s, m, flags); }

MMAPI BOOL WINAPI PlaySoundA(LPCSTR s, HMODULE m, DWORD flags)
{
    LPWSTR w = widen(s, flags);
    BOOL r = play_sound(w, m, flags);
    if (w && w != (LPWSTR)s) HeapFree(GetProcessHeap(), 0, w);
    return r;
}

/* sndPlaySound: the same flags' low bits; a name is an alias or a file */
MMAPI BOOL WINAPI sndPlaySoundW(LPCWSTR s, UINT flags) { return play_sound(s, 0, flags & 0xFF); }
MMAPI BOOL WINAPI sndPlaySoundA(LPCSTR s, UINT flags)  { return PlaySoundA(s, 0, flags & 0xFF); }
