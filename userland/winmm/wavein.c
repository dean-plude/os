/*
 * wavein.c — waveIn on NovaOS's mixer
 *
 * Each waveIn handle owns a kernel recording stream (NtNovaAudioOpen with
 * the capture flag) and a thread that, while recording, reads the mixer's
 * 48 kHz s16 stereo frames, converts them into the queued WAVEHDR buffers
 * in the program's format, and returns each full buffer (WHDR_DONE,
 * WIM_DATA).
 *
 * A sound card's driver hands buffers back as the hardware fills them, in
 * real time; here a thread does, and it or the program's own thread can be
 * held up on a busy machine (Audacity redrawing under QEMU without KVM
 * held both for 100-270 ms).  What was recorded meanwhile waits in the
 * kernel's stream (1 s), so nothing is lost as long as there is a buffer
 * to put it in.  Two rules keep it so:
 *
 * - The last buffer queued is not handed back full until the program
 *   queues another (or HOLD_MS goes by), when the program ever queued more
 *   than one.  Handing back every buffer is what a program reads as an
 *   overflow: PortAudio's MME host, finding all its buffers done, throws
 *   away all but the newest and reports paInputOverflow, which Audacity
 *   marks as a dropout.  A program that cycles one buffer gets it as soon
 *   as it is full.
 * - With no buffer queued, what is recorded is kept for BACKLOG_MS and
 *   only what is older is dropped (Windows drops it all), so a program
 *   that turns its buffers around late finds the frames still there.
 */

#include <windows.h>
#include <avrt.h>
#include <winternl.h>
#include "audioconv.h"
#include "mmwave.h"
#include "audiodev.h"

#define WI_MAGIC  0x4957564E        /* "NVWI" */
#define CHUNK     960               /* mixer frames read at a time (20 ms) */
#define HOLD_MS   500               /* the last buffer waits this long for the program to queue another */
#define BACKLOG_MS 500              /* recorded frames kept while no buffer is queued */

typedef struct WaveIn {
    DWORD       magic;
    INT_PTR     stream;
    UINT32      device;             /* the device chosen (0: the default) */
    AudioCapConv conv;
    DWORD       cbtype;
    DWORD_PTR   cb, inst;
    CRITICAL_SECTION lock;
    WAVEHDR    *head, *tail;        /* queued, oldest first; head is being filled */
    UINT        most;               /* the most buffers the program has had queued at once */
    DWORD       held;               /* when the last buffer filled up and was held back (0: it is not) */
    ULONGLONG   recorded;           /* program frames delivered since the last reset */
    BOOL        started, quit;
    HANDLE      thread, wake;
    SHORT       tmp[CHUNK * 2];
    struct WaveIn *next;
} WaveIn;

static WaveIn *g_waveins;
static SRWLOCK g_wi_lock;

static void notify(WaveIn *w, UINT msg, DWORD_PTR p1)
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

static WaveIn *wi_get(HANDLE h)
{
    WaveIn *w = (WaveIn *)h;
    AcquireSRWLockShared(&g_wi_lock);
    WaveIn *x = g_waveins;
    while (x && x != w) x = x->next;
    ReleaseSRWLockShared(&g_wi_lock);
    return x && x->magic == WI_MAGIC ? x : 0;
}

/* Take the head buffer off the queue as done (call locked); returns it */
static WAVEHDR *wi_pop(WaveIn *w)
{
    WAVEHDR *h = w->head;
    if (!h) return 0;
    w->head = h->lpNext;
    if (!w->head) w->tail = 0;
    h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    return h;
}

/* Move what the stream recorded into the buffers; returns a filled buffer
 * to hand back (call locked, and again while it returns one) */
static WAVEHDR *wi_fill(WaveIn *w)
{
    for (;;) {
        WAVEHDR *h = w->head;
        if (!h) {                                   /* nowhere to put it: keep the newest */
            StreamStatus st;
            if (NtNovaAudioCtl(w->stream, 0, 0, &st) == 0)
                for (UINT old = st.queued > AC_RATE * BACKLOG_MS / 1000 ? st.queued - AC_RATE * BACKLOG_MS / 1000 : 0;
                     old; ) {
                    LONG_PTR got = NtNovaAudioCtl(w->stream, 6, old < CHUNK ? old : CHUNK, w->tmp);
                    if (got <= 0) break;
                    old -= (UINT)got;
                }
            return 0;
        }
        UINT room = (h->dwBufferLength - h->dwBytesRecorded) / w->conv.f.block;
        if (!room) {
            if (!h->lpNext && w->most > 1) {        /* the last one queued: wait for another */
                DWORD now = GetTickCount() | 1;
                if (!w->held) w->held = now;
                if (now - w->held < HOLD_MS) return 0;
            }
            w->held = 0;
            return wi_pop(w);
        }
        UINT m = acc_src_for(&w->conv, room);
        if (m > CHUNK) m = CHUNK;
        LONG_PTR got = NtNovaAudioCtl(w->stream, 6, m, w->tmp);
        if (got <= 0) return 0;
        UINT k = acc_convert(&w->conv, w->tmp, (UINT)got, h->lpData + h->dwBytesRecorded, room);
        h->dwBytesRecorded += k * w->conv.f.block;
        w->recorded += k;
    }
}

static DWORD WINAPI wi_thread(LPVOID p)
{
    WaveIn *w = p;
    DWORD mm = 0;
    AvSetMmThreadCharacteristicsW(L"Capture", &mm);   /* (MMCSS: above any busy or boosted program thread) */
    while (!w->quit) {
        for (;;) {
            EnterCriticalSection(&w->lock);
            WAVEHDR *done = w->started ? wi_fill(w) : 0;
            LeaveCriticalSection(&w->lock);
            if (!done) break;
            notify(w, WIM_DATA, (DWORD_PTR)done);
        }
        WaitForSingleObject(w->wake, 10);
    }
    return 0;
}

/* One device ID for each input NovaOS has: the default first, then the
 * others oldest first (waveOut's order); WAVE_MAPPER records from the
 * default one */
MMAPI UINT WINAPI waveInGetNumDevs(void)
{
    AudioDeviceList l;
    return audio_wave_devices(1, &l);
}

typedef struct {
    WORD wMid, wPid;
    UINT vDriverVersion;
    WCHAR szPname[32];
    DWORD dwFormats;
    WORD wChannels, wReserved1;
    GUID ManufacturerGuid, ProductGuid, NameGuid;       /* WAVEINCAPS2W */
} WAVEINCAPSW;
typedef struct {
    WORD wMid, wPid;
    UINT vDriverVersion;
    CHAR szPname[32];
    DWORD dwFormats;
    WORD wChannels, wReserved1;
    GUID ManufacturerGuid, ProductGuid, NameGuid;
} WAVEINCAPSA;

static MMRESULT check_device(UINT_PTR dev)
{
    AudioDeviceList l;
    UINT n = audio_wave_devices(1, &l);
    if (!n) return MMSYSERR_NODRIVER;
    if (dev < n || dev == WAVE_MAPPER || dev == (UINT_PTR)-1 || wi_get((HANDLE)dev)) return MMSYSERR_NOERROR;
    return MMSYSERR_BADDEVICEID;
}

static void fill_caps(WAVEINCAPSW *c, UINT_PTR dev)
{
    memset(c, 0, sizeof(*c));
    BOOL mapper = dev == WAVE_MAPPER || dev == (UINT_PTR)-1;
    c->wMid = 1;                                    /* MM_MICROSOFT */
    c->wPid = mapper ? 3 : 101;                     /* MM_WAVE_MAPPER (input) / generic */
    c->vDriverVersion = 0x0600;
    WaveIn *w = mapper ? NULL : wi_get((HANDLE)dev);
    AudioDeviceList l;
    UINT n = audio_wave_devices(1, &l), k = w ? n : (UINT)dev;
    for (UINT i = 0; w && i < n; i++)               /* (a handle: its device) */
        if (l.dev[i].id == w->device || (!w->device && l.dev[i].is_default)) k = i;
    const char *name = mapper ? "Microsoft Sound Mapper" : k < n ? l.dev[k].name : "Microphone";
    for (int i = 0; name[i] && i < 31; i++) c->szPname[i] = (WCHAR)(BYTE)name[i];
    c->dwFormats = 0x000FFFFF;
    c->wChannels = 2;
}

MMAPI MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR dev, WAVEINCAPSW *caps, UINT n)
{
    MMRESULT r = check_device(dev);
    if (r) return r;
    if (!caps) return MMSYSERR_INVALPARAM;
    WAVEINCAPSW c;
    fill_caps(&c, dev);
    memcpy(caps, &c, n < sizeof(c) ? n : sizeof(c));
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR dev, WAVEINCAPSA *caps, UINT n)
{
    MMRESULT r = check_device(dev);
    if (r) return r;
    if (!caps) return MMSYSERR_INVALPARAM;
    WAVEINCAPSW w;
    WAVEINCAPSA a;
    fill_caps(&w, dev);
    memset(&a, 0, sizeof(a));
    a.wMid = w.wMid; a.wPid = w.wPid; a.vDriverVersion = w.vDriverVersion;
    for (int i = 0; i < 32; i++) a.szPname[i] = (CHAR)w.szPname[i];
    a.dwFormats = w.dwFormats; a.wChannels = w.wChannels;
    memcpy(caps, &a, n < sizeof(a) ? n : sizeof(a));
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInOpen(HANDLE *out, UINT dev, const AcWaveFormat *fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    if (out) *out = 0;
    AudioDeviceList l;
    UINT n = audio_wave_devices(1, &l);
    if (!n) return MMSYSERR_NODRIVER;
    if (dev >= n && dev != WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
    if (!fmt) return MMSYSERR_INVALPARAM;
    AudioCapConv conv;
    if (!acc_init(&conv, fmt)) return WAVERR_BADFORMAT;
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    if (!out) return MMSYSERR_INVALPARAM;
    DWORD cbtype = flags & CALLBACK_TYPEMASK;
    if (cbtype && cbtype != CALLBACK_WINDOW && cbtype != CALLBACK_THREAD && cbtype != CALLBACK_FUNCTION &&
        cbtype != CALLBACK_EVENT) return MMSYSERR_INVALFLAG;

    WaveIn *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) return MMSYSERR_NOMEM;
    w->stream = NtNovaAudioOpen(0x80000000u | AC_RATE);          /* records; holds 1 s */
    if (!w->stream) { HeapFree(GetProcessHeap(), 0, w); return MMSYSERR_ALLOCATED; }
    w->device = dev == WAVE_MAPPER ? 0 : l.dev[dev].id;        /* (the mapper: the default, wherever it moves) */
    if (w->device && !audio_route(w->stream, w->device)) {     /* (unplugged meanwhile) */
        CloseHandle((HANDLE)w->stream);
        HeapFree(GetProcessHeap(), 0, w);
        return MMSYSERR_BADDEVICEID;
    }
    w->magic = WI_MAGIC;
    w->conv = conv;
    w->cbtype = cbtype;
    w->cb = cb;
    w->inst = inst;
    InitializeCriticalSection(&w->lock);
    w->wake = CreateEventW(0, FALSE, FALSE, 0);
    AcquireSRWLockExclusive(&g_wi_lock);
    w->next = g_waveins;
    g_waveins = w;
    ReleaseSRWLockExclusive(&g_wi_lock);
    w->thread = CreateThread(0, 64 * 1024, wi_thread, w, 0, 0);
    if (w->thread) SetThreadPriority(w->thread, 15 /* THREAD_PRIORITY_TIME_CRITICAL */);
    *out = (HANDLE)w;
    notify(w, WIM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInPrepareHeader(HANDLE h, WAVEHDR *hdr, UINT n)
{
    if (!wi_get(h)) return MMSYSERR_INVALHANDLE;
    if (!hdr || n < sizeof(WAVEHDR) - sizeof(DWORD_PTR) || !hdr->lpData) return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInUnprepareHeader(HANDLE h, WAVEHDR *hdr, UINT n)
{
    (void)n;
    if (!wi_get(h)) return MMSYSERR_INVALHANDLE;
    if (!hdr) return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags &= ~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInAddBuffer(HANDLE h, WAVEHDR *hdr, UINT n)
{
    (void)n;
    WaveIn *w = wi_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    if (!hdr) return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags = (hdr->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
    hdr->dwBytesRecorded = 0;
    hdr->lpNext = 0;
    EnterCriticalSection(&w->lock);
    if (w->tail) w->tail->lpNext = hdr; else w->head = hdr;
    w->tail = hdr;
    UINT queued = 0;
    for (WAVEHDR *x = w->head; x; x = x->lpNext) queued++;
    if (queued > w->most) w->most = queued;
    BOOL held = w->held != 0;
    LeaveCriticalSection(&w->lock);
    if (held) SetEvent(w->wake);                    /* the held buffer can go back now */
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInStart(HANDLE h)
{
    WaveIn *w = wi_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    if (!w->started) NtNovaAudioCtl(w->stream, 1, 1, 0);
    w->started = TRUE;
    LeaveCriticalSection(&w->lock);
    SetEvent(w->wake);
    return MMSYSERR_NOERROR;
}

/* Stop; the buffer being filled comes back with what it holds */
MMAPI MMRESULT WINAPI waveInStop(HANDLE h)
{
    WaveIn *w = wi_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    WAVEHDR *done = 0;
    if (w->started) {
        NtNovaAudioCtl(w->stream, 1, 0, 0);
        while ((done = wi_fill(w))) {               /* what was recorded before the stop */
            LeaveCriticalSection(&w->lock);
            notify(w, WIM_DATA, (DWORD_PTR)done);
            EnterCriticalSection(&w->lock);
        }
        w->started = FALSE;
        w->held = 0;
        if (w->head && w->head->dwBytesRecorded) done = wi_pop(w);
    }
    LeaveCriticalSection(&w->lock);
    if (done) notify(w, WIM_DATA, (DWORD_PTR)done);
    return MMSYSERR_NOERROR;
}

/* Stop, hand every queued buffer back, and restart the position at 0 */
MMAPI MMRESULT WINAPI waveInReset(HANDLE h)
{
    WaveIn *w = wi_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    NtNovaAudioCtl(w->stream, 1, 0, 0);
    NtNovaAudioCtl(w->stream, 2, 0, 0);
    w->started = FALSE;
    w->held = 0;
    WAVEHDR *list = w->head;
    w->head = w->tail = 0;
    w->recorded = 0;
    w->conv.pos = 0;
    w->conv.prev[0] = w->conv.prev[1] = 0;
    LeaveCriticalSection(&w->lock);
    while (list) {
        WAVEHDR *next = list->lpNext;
        list->dwFlags = (list->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
        notify(w, WIM_DATA, (DWORD_PTR)list);
        list = next;
    }
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInClose(HANDLE h)
{
    WaveIn *w = wi_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    EnterCriticalSection(&w->lock);
    BOOL busy = w->head != 0;
    LeaveCriticalSection(&w->lock);
    if (busy) return WAVERR_STILLPLAYING;
    AcquireSRWLockExclusive(&g_wi_lock);
    WaveIn **pp = &g_waveins;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    ReleaseSRWLockExclusive(&g_wi_lock);
    w->quit = TRUE;
    SetEvent(w->wake);
    if (w->thread) {
        if (GetCurrentThreadId() != GetThreadId(w->thread)) WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
    }
    notify(w, WIM_CLOSE, 0);
    CloseHandle((HANDLE)w->stream);
    CloseHandle(w->wake);
    DeleteCriticalSection(&w->lock);
    w->magic = 0;
    HeapFree(GetProcessHeap(), 0, w);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInGetPosition(HANDLE h, MMTIME *t, UINT n)
{
    WaveIn *w = wi_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    if (!t || n < sizeof(UINT) + sizeof(DWORD)) return MMSYSERR_INVALPARAM;
    EnterCriticalSection(&w->lock);
    ULONGLONG f = w->recorded;
    LeaveCriticalSection(&w->lock);
    switch (t->wType) {
    case TIME_SAMPLES: t->u.sample = (DWORD)f; break;
    case TIME_MS:      t->u.ms = (DWORD)(f * 1000 / w->conv.f.rate); break;
    default:           t->wType = TIME_BYTES; t->u.cb = (DWORD)(f * w->conv.f.block); break;
    }
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInGetID(HANDLE h, UINT *id)
{
    if (!wi_get(h)) return MMSYSERR_INVALHANDLE;
    if (!id) return MMSYSERR_INVALPARAM;
    *id = 0;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInMessage(HANDLE h, UINT msg, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h;
    return audio_wave_devices(1, &(AudioDeviceList){ 0 }) ? wave_mapper_message(msg, p1, p2) : MMSYSERR_NODRIVER;
}

/* the same texts as waveOut's */
MMAPI MMRESULT WINAPI waveInGetErrorTextW(MMRESULT e, LPWSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    MultiByteToWideChar(CP_UTF8, 0, mm_error_text(e), -1, buf, (int)n);
    buf[n - 1] = 0;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI waveInGetErrorTextA(MMRESULT e, LPSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    lstrcpynA(buf, mm_error_text(e), (int)n);
    return MMSYSERR_NOERROR;
}
