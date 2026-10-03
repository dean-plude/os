/*
 * mmdevapi.dll — the Core Audio API (WASAPI)
 *
 * CLSID_MMDeviceEnumerator lists up to two endpoints: the sound card's
 * speakers and, when the card can record, its microphone or line in.  An
 * IAudioClient runs in shared mode in the client's own format (the mix
 * format is 48 kHz float stereo, but any PCM or float format is accepted
 * and converted).  On the speakers each ReleaseBuffer converts the frames
 * onto a kernel mixer stream and the padding is what the stream still
 * holds beyond the engine's 100 ms lead; on the recording endpoint the client buffer fills from a kernel
 * capture stream and IAudioCaptureClient hands it out a device period at a
 * time.  In event mode a helper thread sets the client's event every
 * device period while the stream runs.  IAudioEndpointVolume is each
 * endpoint's master volume, kept in the kernel for every program.
 *
 * No exclusive mode and no loopback capture.
 */

#include <windows.h>
#include <winternl.h>
#include <objbase.h>
#include "../winmm/audioconv.h"

int _fltused = 1;                  /* floats are used (the converter) */

#define EXPORT __declspec(dllexport)

DEFINE_GUID(CLSID_MMDeviceEnumerator,  0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator,   0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IMMDeviceCollection,   0x0BD7A1BE, 0x7A1A, 0x44DB, 0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E);
DEFINE_GUID(IID_IMMDevice,             0xD666063F, 0x1587, 0x4E43, 0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F);
DEFINE_GUID(IID_IMMEndpoint,           0x1BE09788, 0x6894, 0x4089, 0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5);
DEFINE_GUID(IID_IPropertyStore,        0x886D8EEB, 0x8CF2, 0x4446, 0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99);
DEFINE_GUID(IID_IAudioClient,          0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioClient2,         0x726778CD, 0xF60A, 0x4EDA, 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA);
DEFINE_GUID(IID_IAudioClient3,         0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
DEFINE_GUID(IID_IAudioRenderClient,    0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(IID_IAudioCaptureClient,   0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(IID_IAudioEndpointVolume,  0x5CDF2C82, 0x841E, 0x4546, 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A);
DEFINE_GUID(IID_IAudioEndpointVolumeEx, 0x66E11784, 0xF695, 0x4F28, 0xA5, 0x05, 0xA7, 0x08, 0x00, 0x81, 0xA7, 0x8F);
DEFINE_GUID(IID_IAudioClock,           0xCD63314F, 0x3FBA, 0x4A1B, 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7);
DEFINE_GUID(IID_ISimpleAudioVolume,    0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8);
DEFINE_GUID(IID_IAudioStreamVolume,    0x93014887, 0x242D, 0x4068, 0x8A, 0x15, 0xCF, 0x5E, 0x93, 0xB9, 0x0F, 0xE3);
DEFINE_GUID(IID_IAudioSessionControl,  0xF4B1A599, 0x7266, 0x4319, 0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
static const GUID FMTID_Device       = { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } };
static const GUID FMTID_Interface    = { 0x026E516E, 0xB814, 0x414B, { 0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22 } };
static const GUID FMTID_Endpoint     = { 0x1DA5D803, 0xD492, 0x4EDD, { 0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E } };
static const GUID FMTID_EngineFormat = { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } };

#define AUDCLNT_E_NOT_INITIALIZED          ((HRESULT)0x88890001)
#define AUDCLNT_E_ALREADY_INITIALIZED      ((HRESULT)0x88890002)
#define AUDCLNT_E_WRONG_ENDPOINT_TYPE      ((HRESULT)0x88890003)
#define AUDCLNT_E_NOT_STOPPED              ((HRESULT)0x88890005)
#define AUDCLNT_E_BUFFER_TOO_LARGE         ((HRESULT)0x88890006)
#define AUDCLNT_E_OUT_OF_ORDER             ((HRESULT)0x88890007)
#define AUDCLNT_E_UNSUPPORTED_FORMAT       ((HRESULT)0x88890008)
#define AUDCLNT_E_INVALID_SIZE             ((HRESULT)0x88890009)
#define AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ((HRESULT)0x8889000E)
#define AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED ((HRESULT)0x88890011)
#define AUDCLNT_E_EVENTHANDLE_NOT_SET      ((HRESULT)0x88890014)
#define AUDCLNT_STREAMFLAGS_LOOPBACK       0x00020000
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK  0x00040000
#define AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY 0x1
#define AUDCLNT_BUFFERFLAGS_SILENT         0x2
#define AUDCLNT_S_BUFFER_EMPTY             ((HRESULT)0x08890001)
#define E_NOTFOUND                         ((HRESULT)0x80070490)

#define PERIOD   100000LL           /* 10 ms, in 100 ns units */
#define MIN_BUF  300000LL           /* 30 ms */
#define DEF_BUF  500000LL           /* 50 ms */
/* The engine's own buffer: frames a client has released and the kernel
 * stream still holds beyond this lead count as played, so the client
 * writes LEAD ahead of the mixer and a late wakeup (a loaded host) does
 * not starve the stream (a 30 ms client buffer alone would). */
#define LEAD     (AC_RATE / 10)     /* 100 ms, mixer frames */

typedef struct {
    WORD vt, r1, r2, r3;
    union { LPWSTR pwszVal; ULONG ulVal; struct { ULONG cbSize; BYTE *pBlobData; } blob; BYTE pad[16]; };
} PROPVARIANT;
typedef struct { GUID fmtid; DWORD pid; } PROPERTYKEY;
#define VT_EMPTY  0
#define VT_UI4    19
#define VT_LPWSTR 31
#define VT_BLOB   65


/* eRender (0) or eCapture (1): whether NovaOS has that endpoint; its name
 * from the kernel ("Microphone (Intel ...)") */
static struct { LONG known; char name[96]; } g_dev[2] = { { -1 }, { -1 } };

static BOOL device_present_flow(int flow)
{
    if (flow < 0 || flow > 1) return FALSE;
    if (g_dev[flow].known < 0) {
        struct { UINT32 present, rate; char name[96]; } info = { 0 };
        BOOL ok = NtNovaAudioCtl(0, flow ? 7 : 5, 0, &info) == 0 && info.present;
        memcpy(g_dev[flow].name, info.name, sizeof(info.name));
        g_dev[flow].known = ok;
    }
    return g_dev[flow].known;
}

static void mix_format(AcWaveFormatExt *f)
{
    memset(f, 0, sizeof(*f));
    f->Format.wFormatTag = 0xFFFE;
    f->Format.nChannels = 2;
    f->Format.nSamplesPerSec = AC_RATE;
    f->Format.wBitsPerSample = 32;
    f->Format.nBlockAlign = 8;
    f->Format.nAvgBytesPerSec = AC_RATE * 8;
    f->Format.cbSize = 22;
    f->wValidBitsPerSample = 32;
    f->dwChannelMask = 3;                       /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    f->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

static LPWSTR co_str(LPCWSTR s)
{
    SIZE_T n = ((SIZE_T)lstrlenW(s) + 1) * sizeof(WCHAR);
    LPWSTR p = CoTaskMemAlloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static const WCHAR *const DEVICE_ID[2] = {
    L"{0.0.0.00000000}.{6e6f7661-6864-6100-0000-000000000001}",
    L"{0.0.1.00000000}.{6e6f7661-6864-6100-0000-000000000002}",
};

/* "Speakers", or what the card records from ("Microphone", "Line in") */
static void device_desc(int flow, WCHAR *out, int n)
{
    const char *src = flow ? g_dev[1].name : "Speakers";
    int i = 0;
    while (src[i] && i < n - 1 && !(src[i] == ' ' && src[i + 1] == '(')) { out[i] = (WCHAR)(BYTE)src[i]; i++; }
    out[i] = 0;
    if (!i && n > 10) lstrcpyW(out, flow ? L"Microphone" : L"Speakers");
}

static void device_name(int flow, WCHAR *out, int n)
{
    device_desc(flow, out, n - 26);
    lstrcatW(out, L" (High Definition Audio)");
}

/* -----------------------------------------------------------------------
 * Decibels (no maths library here)
 * ----------------------------------------------------------------------- */
static double ln_(double x)
{
    if (x <= 0) return -1e9;
    int k = 0;
    while (x < 0.5) { x *= 2; k--; }
    while (x > 1) { x /= 2; k++; }
    double y = (x - 1) / (x + 1), y2 = y * y, t = y, sum = 0;
    for (int i = 1; i < 60; i += 2) { sum += t / i; t *= y2; }
    return 2 * sum + k * 0.69314718055994531;
}

static double exp_(double x)
{
    int k = (int)(x / 0.69314718055994531);
    double r = x - k * 0.69314718055994531, t = 1, sum = 1;
    for (int i = 1; i < 30; i++) { t *= r / i; sum += t; }
    for (; k > 0; k--) sum *= 2;
    for (; k < 0; k++) sum /= 2;
    return sum;
}

#define VOL_MIN_DB  -65.25f
#define VOL_STEP_DB 0.03125f
static float db_of(float v)  { return v <= 0.000545f ? VOL_MIN_DB : (float)(20 * ln_(v) / 2.302585092994046); }
static float lin_of(float db) { return db <= VOL_MIN_DB ? 0 : db >= 0 ? 1 : (float)exp_(db * 2.302585092994046 / 20); }

/* -----------------------------------------------------------------------
 * The audio client and the services it hands out
 * ----------------------------------------------------------------------- */
typedef struct Client Client;
typedef struct { const void *vtbl; Client *c; } Sub;

struct Client {
    const void *vtbl;
    LONG        refs;
    CRITICAL_SECTION lock;
    int         flow;               /* 0 render, 1 capture */
    BOOL        init, started, event_mode, in_buffer;
    AudioConv   conv;
    AudioCapConv cconv;             /* capture: the mixer's frames to the client's */
    UINT32      filled;             /* capture: client frames waiting in buf */
    ULONGLONG   dropped;            /* capture: the stream's lost frames, last seen */
    BOOL        discontinuity;
    AcWaveFormatExt fmt;
    UINT32      frames;             /* buffer size, client frames */
    BYTE       *buf;
    UINT32      pending;            /* frames handed out by GetBuffer */
    INT_PTR     stream;
    HANDLE      event, thread, quit;
    float       vol, chan[2];
    BOOL        mute;
    ULONGLONG   written;            /* client frames released since the last reset */
    Sub         render, capture, clock, simple, stream_vol, session;
};

typedef struct {
    ULONGLONG written, consumed, played;
    UINT32 queued, capacity, running, latency;
} StreamStatus;

/* Capture: move what the stream recorded into the client buffer (locked) */
static void cap_pull(Client *c)
{
    SHORT tmp[1024 * 2];
    StreamStatus st;
    if (NtNovaAudioCtl(c->stream, 0, 0, &st) == 0 && st.played != c->dropped) {
        c->dropped = st.played;
        c->discontinuity = TRUE;
    }
    while (c->filled < c->frames) {
        UINT room = c->frames - c->filled, m = acc_src_for(&c->cconv, room);
        if (m > 1024) m = 1024;
        LONG_PTR got = NtNovaAudioCtl(c->stream, 6, m, tmp);
        if (got <= 0) return;
        c->filled += acc_convert(&c->cconv, tmp, (UINT)got, c->buf + (SIZE_T)c->filled * c->conv.block, room);
    }
}

static void apply_volume(Client *c)
{
    float l = c->mute ? 0 : c->vol * c->chan[0], r = c->mute ? 0 : c->vol * c->chan[1];
    DWORD v = (DWORD)(l * 65535.0f + 0.5f) | (DWORD)(r * 65535.0f + 0.5f) << 16;
    if (c->stream) NtNovaAudioCtl(c->stream, 3, v, 0);
}

static UINT32 padding(Client *c)
{
    if (c->flow) { cap_pull(c); return c->filled; }
    StreamStatus st;
    if (!c->stream || NtNovaAudioCtl(c->stream, 0, 0, &st)) return 0;
    if (st.queued <= LEAD) return 0;
    ULONGLONG p = (ULONGLONG)(st.queued - LEAD) * c->conv.rate / AC_RATE;
    return p > c->frames ? c->frames : (UINT32)p;
}

static DWORD WINAPI event_thread(LPVOID p)
{
    Client *c = p;
    while (WaitForSingleObject(c->quit, (DWORD)(PERIOD / 10000)) == WAIT_TIMEOUT)
        if (c->started && c->event) SetEvent(c->event);
    return 0;
}

static HRESULT STDMETHODCALLTYPE ac_qi(Client *c, REFIID riid, void **ppv);
static ULONG STDMETHODCALLTYPE ac_addref(Client *c) { return (ULONG)InterlockedIncrement(&c->refs); }
static ULONG STDMETHODCALLTYPE ac_release(Client *c)
{
    LONG r = InterlockedDecrement(&c->refs);
    if (r) return (ULONG)r;
    if (c->thread) {
        SetEvent(c->quit);
        WaitForSingleObject(c->thread, INFINITE);
        CloseHandle(c->thread);
    }
    if (c->quit) CloseHandle(c->quit);
    if (c->stream) NtClose((HANDLE)c->stream);
    if (c->buf) HeapFree(GetProcessHeap(), 0, c->buf);
    DeleteCriticalSection(&c->lock);
    HeapFree(GetProcessHeap(), 0, c);
    return 0;
}

static HRESULT check_format(const AcWaveFormat *f)
{
    AudioConv conv;
    return f && ac_init(&conv, f) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
}

static HRESULT STDMETHODCALLTYPE ac_initialize(Client *c, int mode, DWORD flags, LONGLONG dur, LONGLONG period,
                                               const AcWaveFormat *fmt, const GUID *session)
{
    (void)period; (void)session;
    if (!fmt) return E_POINTER;
    if (mode != 0) return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    if (flags & AUDCLNT_STREAMFLAGS_LOOPBACK) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    EnterCriticalSection(&c->lock);
    HRESULT hr = S_OK;
    if (c->init) { hr = AUDCLNT_E_ALREADY_INITIALIZED; goto out; }
    if (!ac_init(&c->conv, fmt) || !acc_init(&c->cconv, fmt)) { hr = AUDCLNT_E_UNSUPPORTED_FORMAT; goto out; }
    memset(&c->fmt, 0, sizeof(c->fmt));
    memcpy(&c->fmt, fmt, sizeof(AcWaveFormat) + (fmt->wFormatTag == 0xFFFE ? 22 : 0));
    if (dur <= 0) dur = DEF_BUF;
    if (dur < MIN_BUF) dur = MIN_BUF;
    if (dur > 18000000) dur = 18000000;                         /* 1.8 s + the lead: the most a stream holds (2 s) */
    c->frames = (UINT32)((dur * c->conv.rate + 9999999) / 10000000);
    c->buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)c->frames * c->conv.block);
    UINT32 dev = (UINT32)((ULONGLONG)c->frames * AC_RATE / c->conv.rate) + LEAD + 1024;
    c->stream = c->buf ? NtNovaAudioOpen(dev | (c->flow ? 0x80000000u : 0)) : 0;   /* capture: a recording stream */
    if (!c->stream) { hr = E_OUTOFMEMORY; goto out; }
    c->event_mode = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
    if (c->event_mode) {
        c->quit = CreateEventW(0, TRUE, FALSE, 0);
        c->thread = CreateThread(0, 64 * 1024, event_thread, c, 0, 0);
        if (c->thread) SetThreadPriority(c->thread, 15 /* THREAD_PRIORITY_TIME_CRITICAL */);
    }
    apply_volume(c);
    c->init = TRUE;
out:
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ac_get_buffer_size(Client *c, UINT32 *n)
{
    if (!n) return E_POINTER;
    if (!c->init) return AUDCLNT_E_NOT_INITIALIZED;
    *n = c->frames;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ac_get_latency(Client *c, LONGLONG *t)
{
    if (!t) return E_POINTER;
    if (!c->init) return AUDCLNT_E_NOT_INITIALIZED;
    *t = 800000 + LEAD * 10000000LL / AC_RATE;                  /* the mixer runs 80 ms ahead, plus the lead */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ac_get_padding(Client *c, UINT32 *n)
{
    if (!n) return E_POINTER;
    if (!c->init) return AUDCLNT_E_NOT_INITIALIZED;
    *n = padding(c);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ac_is_format_supported(Client *c, int mode, const AcWaveFormat *f, AcWaveFormat **closest)
{
    (void)c;
    if (closest) *closest = 0;
    if (!f) return E_POINTER;
    if (mode != 0) return AUDCLNT_E_UNSUPPORTED_FORMAT;         /* no exclusive mode */
    if (check_format(f) == S_OK) return S_OK;
    if (closest) {
        AcWaveFormatExt *m = CoTaskMemAlloc(sizeof(*m));
        if (m) { mix_format(m); *closest = &m->Format; }
        return m ? S_FALSE : E_OUTOFMEMORY;
    }
    return AUDCLNT_E_UNSUPPORTED_FORMAT;
}

static HRESULT STDMETHODCALLTYPE ac_get_mix_format(Client *c, AcWaveFormat **out)
{
    (void)c;
    if (!out) return E_POINTER;
    AcWaveFormatExt *m = CoTaskMemAlloc(sizeof(*m));
    if (!m) return E_OUTOFMEMORY;
    mix_format(m);
    *out = &m->Format;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ac_get_period(Client *c, LONGLONG *def, LONGLONG *min)
{
    (void)c;
    if (!def && !min) return E_POINTER;
    if (def) *def = PERIOD;
    if (min) *min = PERIOD;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ac_start(Client *c)
{
    EnterCriticalSection(&c->lock);
    HRESULT hr = !c->init ? AUDCLNT_E_NOT_INITIALIZED : c->started ? AUDCLNT_E_NOT_STOPPED :
                 c->event_mode && !c->event ? AUDCLNT_E_EVENTHANDLE_NOT_SET : S_OK;
    if (hr == S_OK) {
        c->started = TRUE;
        NtNovaAudioCtl(c->stream, 1, 1, 0);
        if (c->event) SetEvent(c->event);
    }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ac_stop(Client *c)
{
    EnterCriticalSection(&c->lock);
    HRESULT hr = !c->init ? AUDCLNT_E_NOT_INITIALIZED : !c->started ? S_FALSE : S_OK;
    if (hr == S_OK) {
        c->started = FALSE;
        NtNovaAudioCtl(c->stream, 1, 0, 0);
    }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ac_reset(Client *c)
{
    EnterCriticalSection(&c->lock);
    HRESULT hr = !c->init ? AUDCLNT_E_NOT_INITIALIZED : c->started ? AUDCLNT_E_NOT_STOPPED :
                 c->in_buffer ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    if (hr == S_OK) {
        NtNovaAudioCtl(c->stream, 2, 0, 0);
        c->written = 0;
        c->filled = 0;
        c->dropped = 0;
        c->conv.pos = c->cconv.pos = 0;
        c->conv.prev[0] = c->conv.prev[1] = 0;
        c->cconv.prev[0] = c->cconv.prev[1] = 0;
    }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ac_set_event(Client *c, HANDLE ev)
{
    if (!ev) return E_INVALIDARG;
    if (!c->init) return AUDCLNT_E_NOT_INITIALIZED;
    if (!c->event_mode) return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    c->event = ev;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ac_get_service(Client *c, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (!c->init) return AUDCLNT_E_NOT_INITIALIZED;
    if (IsEqualIID(riid, &IID_IAudioRenderClient) && c->flow) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    if (IsEqualIID(riid, &IID_IAudioCaptureClient) && !c->flow) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    Sub *s = IsEqualIID(riid, &IID_IAudioRenderClient)   ? &c->render :
             IsEqualIID(riid, &IID_IAudioCaptureClient)  ? &c->capture :
             IsEqualIID(riid, &IID_IAudioClock)          ? &c->clock :
             IsEqualIID(riid, &IID_ISimpleAudioVolume)   ? &c->simple :
             IsEqualIID(riid, &IID_IAudioStreamVolume)   ? &c->stream_vol :
             IsEqualIID(riid, &IID_IAudioSessionControl) ? &c->session : 0;
    if (!s) return E_NOINTERFACE;
    ac_addref(c);
    *ppv = s;
    return S_OK;
}

/* IAudioClient2 */
static HRESULT STDMETHODCALLTYPE ac_is_offload(Client *c, int cat, BOOL *b) { (void)c; (void)cat; if (!b) return E_POINTER; *b = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE ac_set_props(Client *c, const void *p) { (void)c; return p ? S_OK : E_POINTER; }
static HRESULT STDMETHODCALLTYPE ac_get_limits(Client *c, const AcWaveFormat *f, BOOL ev, LONGLONG *mn, LONGLONG *mx)
{
    (void)c; (void)f; (void)ev;
    if (!mn || !mx) return E_POINTER;
    *mn = MIN_BUF;
    *mx = 20000000;
    return S_OK;
}
/* IAudioClient3 */
static HRESULT STDMETHODCALLTYPE ac_shared_period(Client *c, const AcWaveFormat *f, UINT32 *def, UINT32 *fund, UINT32 *mn, UINT32 *mx)
{
    (void)c;
    if (!f || !def || !fund || !mn || !mx) return E_POINTER;
    UINT32 p = f->nSamplesPerSec / 100;
    *def = *fund = *mn = *mx = p;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ac_current_period(Client *c, AcWaveFormat **f, UINT32 *frames)
{
    if (!f || !frames) return E_POINTER;
    HRESULT hr = ac_get_mix_format(c, f);
    if (SUCCEEDED(hr)) *frames = AC_RATE / 100;
    return hr;
}
static HRESULT STDMETHODCALLTYPE ac_init_shared(Client *c, DWORD flags, UINT32 period, const AcWaveFormat *f, const GUID *s)
{
    (void)period;
    return ac_initialize(c, 0, flags, 0, 0, f, s);
}

static const struct {
    void *qi, *addref, *release;
    void *initialize, *get_buffer_size, *get_latency, *get_padding, *is_format_supported, *get_mix_format,
         *get_period, *start, *stop, *reset, *set_event, *get_service;
    void *is_offload, *set_props, *get_limits;                  /* IAudioClient2 */
    void *shared_period, *current_period, *init_shared;         /* IAudioClient3 */
} g_client_vtbl = {
    ac_qi, ac_addref, ac_release,
    ac_initialize, ac_get_buffer_size, ac_get_latency, ac_get_padding, ac_is_format_supported, ac_get_mix_format,
    ac_get_period, ac_start, ac_stop, ac_reset, ac_set_event, ac_get_service,
    ac_is_offload, ac_set_props, ac_get_limits,
    ac_shared_period, ac_current_period, ac_init_shared,
};

static HRESULT STDMETHODCALLTYPE ac_qi(Client *c, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioClient) ||
        IsEqualIID(riid, &IID_IAudioClient2) || IsEqualIID(riid, &IID_IAudioClient3)) {
        ac_addref(c);
        *ppv = c;
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}

/* The services share the client's reference count */
static HRESULT STDMETHODCALLTYPE sub_qi(Sub *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        (s == &s->c->render && IsEqualIID(riid, &IID_IAudioRenderClient)) ||
        (s == &s->c->capture && IsEqualIID(riid, &IID_IAudioCaptureClient)) ||
        (s == &s->c->clock && IsEqualIID(riid, &IID_IAudioClock)) ||
        (s == &s->c->simple && IsEqualIID(riid, &IID_ISimpleAudioVolume)) ||
        (s == &s->c->stream_vol && IsEqualIID(riid, &IID_IAudioStreamVolume)) ||
        (s == &s->c->session && IsEqualIID(riid, &IID_IAudioSessionControl))) {
        ac_addref(s->c);
        *ppv = s;
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sub_addref(Sub *s)  { return ac_addref(s->c); }
static ULONG STDMETHODCALLTYPE sub_release(Sub *s) { return ac_release(s->c); }

/* IAudioRenderClient */
static HRESULT STDMETHODCALLTYPE rc_get_buffer(Sub *s, UINT32 n, BYTE **data)
{
    Client *c = s->c;
    if (!data) return E_POINTER;
    *data = 0;
    EnterCriticalSection(&c->lock);
    HRESULT hr = S_OK;
    if (c->in_buffer) hr = AUDCLNT_E_OUT_OF_ORDER;
    else if (n > c->frames - padding(c)) hr = AUDCLNT_E_BUFFER_TOO_LARGE;
    else if (n) { c->in_buffer = TRUE; c->pending = n; *data = c->buf; }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE rc_release_buffer(Sub *s, UINT32 n, DWORD flags)
{
    Client *c = s->c;
    EnterCriticalSection(&c->lock);
    HRESULT hr = S_OK;
    if (!c->in_buffer) hr = n ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    else if (n > c->pending) hr = AUDCLNT_E_INVALID_SIZE;
    else {
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(c->buf, 0, (SIZE_T)n * c->conv.block);
        SHORT out[1024 * 2];
        UINT32 at = 0;
        while (at < n) {
            UINT m = ac_src_for(&c->conv, 1024);
            if (m > n - at) m = n - at;
            UINT k = ac_convert(&c->conv, c->buf + (SIZE_T)at * c->conv.block, m, out, 1024), done = 0;
            while (done < k) {                                  /* fits: the stream holds the whole buffer */
                LONG_PTR got = NtNovaAudioWrite(c->stream, out + done * 2, k - done);
                if (got <= 0) break;
                done += (UINT)got;
            }
            at += m;
        }
        c->written += n;
        c->in_buffer = FALSE;
    }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static const struct { void *qi, *addref, *release, *get, *rel; } g_render_vtbl = {
    sub_qi, sub_addref, sub_release, rc_get_buffer, rc_release_buffer,
};

/* IAudioCaptureClient: the buffer a device period (10 ms) at a time */
static UINT32 packet(Client *c)
{
    UINT32 per = c->conv.rate / 100;
    return c->filled < per ? c->filled : per;
}

static HRESULT STDMETHODCALLTYPE cc_get_buffer(Sub *s, BYTE **data, UINT32 *n, DWORD *flags, UINT64 *devpos, UINT64 *qpcpos)
{
    Client *c = s->c;
    if (!data || !n || !flags) return E_POINTER;
    *data = 0;
    *n = 0;
    *flags = 0;
    EnterCriticalSection(&c->lock);
    HRESULT hr = S_OK;
    if (c->in_buffer) hr = AUDCLNT_E_OUT_OF_ORDER;
    else {
        cap_pull(c);
        UINT32 k = packet(c);
        if (!k) hr = AUDCLNT_S_BUFFER_EMPTY;
        else {
            c->in_buffer = TRUE;
            c->pending = k;
            *data = c->buf;
            *n = k;
            if (c->discontinuity) { *flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY; c->discontinuity = FALSE; }
            if (devpos) *devpos = c->written;
            if (qpcpos) {
                LARGE_INTEGER t, f;
                QueryPerformanceCounter(&t);
                QueryPerformanceFrequency(&f);
                *qpcpos = (UINT64)((double)t.QuadPart * 10000000.0 / (double)f.QuadPart);
            }
        }
    }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cc_release_buffer(Sub *s, UINT32 n)
{
    Client *c = s->c;
    EnterCriticalSection(&c->lock);
    HRESULT hr = S_OK;
    if (!c->in_buffer) hr = n ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    else if (n && n != c->pending) hr = AUDCLNT_E_INVALID_SIZE;
    else {
        if (n) {                                                /* consumed: drop it from the front */
            memmove(c->buf, c->buf + (SIZE_T)n * c->conv.block, (SIZE_T)(c->filled - n) * c->conv.block);
            c->filled -= n;
            c->written += n;
        }
        c->in_buffer = FALSE;
    }
    LeaveCriticalSection(&c->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cc_next_packet(Sub *s, UINT32 *n)
{
    Client *c = s->c;
    if (!n) return E_POINTER;
    EnterCriticalSection(&c->lock);
    cap_pull(c);
    *n = packet(c);
    LeaveCriticalSection(&c->lock);
    return S_OK;
}

static const struct { void *qi, *addref, *release, *get, *rel, *next; } g_capture_vtbl = {
    sub_qi, sub_addref, sub_release, cc_get_buffer, cc_release_buffer, cc_next_packet,
};

/* IAudioClock: positions in the client's frames */
static HRESULT STDMETHODCALLTYPE ck_freq(Sub *s, UINT64 *f) { if (!f) return E_POINTER; *f = s->c->conv.rate; return S_OK; }
static HRESULT STDMETHODCALLTYPE ck_pos(Sub *s, UINT64 *pos, UINT64 *qpc)
{
    if (!pos) return E_POINTER;
    StreamStatus st;
    if (s->c->flow) *pos = s->c->written + s->c->filled;    /* recorded so far */
    else *pos = NtNovaAudioCtl(s->c->stream, 0, 0, &st) == 0 ? st.played * s->c->conv.rate / AC_RATE : 0;
    if (qpc) {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        *qpc = (UINT64)((double)t.QuadPart * 10000000.0 / (double)f.QuadPart);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ck_chars(Sub *s, DWORD *d) { (void)s; if (!d) return E_POINTER; *d = 0; return S_OK; }
static const struct { void *qi, *addref, *release, *freq, *pos, *chars; } g_clock_vtbl = {
    sub_qi, sub_addref, sub_release, ck_freq, ck_pos, ck_chars,
};

/* ISimpleAudioVolume */
static HRESULT STDMETHODCALLTYPE sv_set(Sub *s, float v, const GUID *ctx)
{
    (void)ctx;
    if (v < 0 || v > 1) return E_INVALIDARG;
    s->c->vol = v;
    apply_volume(s->c);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sv_get(Sub *s, float *v) { if (!v) return E_POINTER; *v = s->c->vol; return S_OK; }
static HRESULT STDMETHODCALLTYPE sv_set_mute(Sub *s, BOOL m, const GUID *ctx) { (void)ctx; s->c->mute = m; apply_volume(s->c); return S_OK; }
static HRESULT STDMETHODCALLTYPE sv_get_mute(Sub *s, BOOL *m) { if (!m) return E_POINTER; *m = s->c->mute; return S_OK; }
static const struct { void *qi, *addref, *release, *set, *get, *set_mute, *get_mute; } g_simple_vtbl = {
    sub_qi, sub_addref, sub_release, sv_set, sv_get, sv_set_mute, sv_get_mute,
};

/* IAudioStreamVolume: the first two channels are what plays */
static HRESULT STDMETHODCALLTYPE st_count(Sub *s, UINT32 *n) { if (!n) return E_POINTER; *n = s->c->conv.channels; return S_OK; }
static HRESULT STDMETHODCALLTYPE st_set(Sub *s, UINT32 i, float v)
{
    if (i >= s->c->conv.channels) return E_INVALIDARG;
    if (v < 0 || v > 1) return E_INVALIDARG;
    if (i < 2) s->c->chan[i] = v;
    if (s->c->conv.channels == 1) s->c->chan[1] = v;
    apply_volume(s->c);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE st_get(Sub *s, UINT32 i, float *v)
{
    if (!v) return E_POINTER;
    if (i >= s->c->conv.channels) return E_INVALIDARG;
    *v = s->c->chan[i < 2 ? i : 0];
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE st_set_all(Sub *s, UINT32 n, const float *v)
{
    if (!v) return E_POINTER;
    if (n != s->c->conv.channels) return E_INVALIDARG;
    for (UINT32 i = 0; i < n; i++) if (v[i] < 0 || v[i] > 1) return E_INVALIDARG;
    s->c->chan[0] = v[0];
    s->c->chan[1] = n > 1 ? v[1] : v[0];
    apply_volume(s->c);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE st_get_all(Sub *s, UINT32 n, float *v)
{
    if (!v) return E_POINTER;
    if (n != s->c->conv.channels) return E_INVALIDARG;
    for (UINT32 i = 0; i < n; i++) v[i] = s->c->chan[i < 2 ? i : 0];
    return S_OK;
}
static const struct { void *qi, *addref, *release, *count, *set, *get, *set_all, *get_all; } g_stream_vol_vtbl = {
    sub_qi, sub_addref, sub_release, st_count, st_set, st_get, st_set_all, st_get_all,
};

/* IAudioSessionControl: one session per client, always active */
static HRESULT STDMETHODCALLTYPE se_state(Sub *s, int *st) { if (!st) return E_POINTER; *st = s->c->started ? 1 : 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE se_get_str(Sub *s, LPWSTR *out) { (void)s; if (!out) return E_POINTER; *out = co_str(L""); return *out ? S_OK : E_OUTOFMEMORY; }
static HRESULT STDMETHODCALLTYPE se_set_str(Sub *s, LPCWSTR v, const GUID *ctx) { (void)s; (void)v; (void)ctx; return S_OK; }
static HRESULT STDMETHODCALLTYPE se_get_group(Sub *s, GUID *g) { (void)s; if (!g) return E_POINTER; memset(g, 0, sizeof(*g)); return S_OK; }
static HRESULT STDMETHODCALLTYPE se_set_group(Sub *s, const GUID *g, const GUID *ctx) { (void)s; (void)g; (void)ctx; return S_OK; }
static HRESULT STDMETHODCALLTYPE se_notify(Sub *s, void *cb) { (void)s; return cb ? S_OK : E_POINTER; }
static const struct { void *qi, *addref, *release, *state, *get_name, *set_name, *get_icon, *set_icon, *get_group, *set_group, *reg, *unreg; }
g_session_vtbl = {
    sub_qi, sub_addref, sub_release, se_state, se_get_str, se_set_str, se_get_str, se_set_str,
    se_get_group, se_set_group, se_notify, se_notify,
};

static HRESULT new_client(int flow, void **ppv)
{
    Client *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) return E_OUTOFMEMORY;
    c->vtbl = &g_client_vtbl;
    c->flow = flow;
    c->refs = 1;
    c->vol = c->chan[0] = c->chan[1] = 1.0f;
    InitializeCriticalSection(&c->lock);
    c->render     = (Sub){ &g_render_vtbl, c };
    c->capture    = (Sub){ &g_capture_vtbl, c };
    c->clock      = (Sub){ &g_clock_vtbl, c };
    c->simple     = (Sub){ &g_simple_vtbl, c };
    c->stream_vol = (Sub){ &g_stream_vol_vtbl, c };
    c->session    = (Sub){ &g_session_vtbl, c };
    *ppv = c;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * The endpoints: for each flow an IMMDevice + IMMEndpoint, its property
 * store, its IAudioEndpointVolume, and the collections (static objects:
 * their reference counts do not matter)
 * ----------------------------------------------------------------------- */
typedef struct { const void *vtbl; int flow; } Static;
static Static g_device[2], g_endpoint[2], g_props[2], g_epvol[2], g_collection[3], g_empty;

static ULONG STDMETHODCALLTYPE static_addref(Static *s)  { (void)s; return 2; }
static ULONG STDMETHODCALLTYPE static_release(Static *s) { (void)s; return 1; }

static HRESULT STDMETHODCALLTYPE dev_qi(Static *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMMDevice) ? (void *)&g_device[s->flow] :
           IsEqualIID(riid, &IID_IMMEndpoint) ? (void *)&g_endpoint[s->flow] : 0;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE dev_activate(Static *s, REFIID riid, DWORD ctx, void *params, void **ppv)
{
    (void)ctx; (void)params;
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (IsEqualIID(riid, &IID_IAudioClient) || IsEqualIID(riid, &IID_IAudioClient2) ||
        IsEqualIID(riid, &IID_IAudioClient3) || IsEqualIID(riid, &IID_IUnknown))
        return new_client(s->flow, ppv);
    if (IsEqualIID(riid, &IID_IAudioEndpointVolume) || IsEqualIID(riid, &IID_IAudioEndpointVolumeEx)) {
        *ppv = &g_epvol[s->flow];
        return S_OK;
    }
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE dev_open_props(Static *s, DWORD access, void **ppv)
{
    (void)access;
    if (!ppv) return E_POINTER;
    *ppv = &g_props[s->flow];
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_get_id(Static *s, LPWSTR *id)
{
    if (!id) return E_POINTER;
    *id = co_str(DEVICE_ID[s->flow]);
    return *id ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE dev_get_state(Static *s, DWORD *st) { (void)s; if (!st) return E_POINTER; *st = 1; return S_OK; }
static const struct { void *qi, *addref, *release, *activate, *open_props, *get_id, *get_state; } g_device_vtbl = {
    dev_qi, static_addref, static_release, dev_activate, dev_open_props, dev_get_id, dev_get_state,
};

static HRESULT STDMETHODCALLTYPE ep_flow(Static *s, int *flow) { if (!flow) return E_POINTER; *flow = s->flow; return S_OK; }
static const struct { void *qi, *addref, *release, *flow; } g_endpoint_vtbl = { dev_qi, static_addref, static_release, ep_flow };

static HRESULT STDMETHODCALLTYPE ps_qi(Static *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyStore) ? s : 0;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE ps_count(Static *s, DWORD *n) { (void)s; if (!n) return E_POINTER; *n = 4; return S_OK; }
static HRESULT STDMETHODCALLTYPE ps_get_at(Static *s, DWORD i, PROPERTYKEY *k)
{
    (void)s;
    static const PROPERTYKEY keys[4] = {
        { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 },
        { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 2 },
        { { 0x026E516E, 0xB814, 0x414B, { 0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22 } }, 2 },
        { { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } }, 0 },
    };
    if (!k) return E_POINTER;
    if (i >= 4) return E_INVALIDARG;
    *k = keys[i];
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ps_get(Static *s, const PROPERTYKEY *k, PROPVARIANT *v)
{
    if (!k || !v) return E_POINTER;
    memset(v, 0, sizeof(*v));
    WCHAR name[96];
    device_present_flow(s->flow);
    if (IsEqualGUID(&k->fmtid, &FMTID_Device) && (k->pid == 14 || k->pid == 2)) {   /* FriendlyName, DeviceDesc */
        if (k->pid == 14) device_name(s->flow, name, 96); else device_desc(s->flow, name, 96);
        v->vt = VT_LPWSTR;
        v->pwszVal = co_str(name);
    } else if (IsEqualGUID(&k->fmtid, &FMTID_Interface) && k->pid == 2) {            /* the adapter */
        v->vt = VT_LPWSTR;
        v->pwszVal = co_str(L"High Definition Audio Device");
    } else if (IsEqualGUID(&k->fmtid, &FMTID_Endpoint) && k->pid == 0) {              /* FormFactor */
        v->vt = VT_UI4;
        device_desc(s->flow, name, 96);
        v->ulVal = !s->flow ? 1 /* Speakers */ : lstrcmpW(name, L"Microphone") ? 2 /* LineLevel */ : 4 /* Microphone */;
    } else if (IsEqualGUID(&k->fmtid, &FMTID_Endpoint) && k->pid == 3 && !s->flow) {  /* PhysicalSpeakers */
        v->vt = VT_UI4;
        v->ulVal = 3;                                           /* front left and right */
    } else if (IsEqualGUID(&k->fmtid, &FMTID_Endpoint) && k->pid == 4) {             /* AudioEndpoint_GUID */
        v->vt = VT_LPWSTR;
        v->pwszVal = co_str(DEVICE_ID[s->flow] + 17);
    } else if (IsEqualGUID(&k->fmtid, &FMTID_EngineFormat) && k->pid == 0) {         /* the device format */
        AcWaveFormatExt *f = CoTaskMemAlloc(sizeof(*f));
        if (!f) return E_OUTOFMEMORY;
        mix_format(f);
        v->vt = VT_BLOB;
        v->blob.cbSize = sizeof(*f);
        v->blob.pBlobData = (BYTE *)f;
    }
    return S_OK;                                                /* unknown: VT_EMPTY */
}
static HRESULT STDMETHODCALLTYPE ps_set(Static *s, const PROPERTYKEY *k, const PROPVARIANT *v) { (void)s; (void)k; (void)v; return E_ACCESSDENIED; }
static HRESULT STDMETHODCALLTYPE ps_commit(Static *s) { (void)s; return S_OK; }
static const struct { void *qi, *addref, *release, *count, *get_at, *get, *set, *commit; } g_props_vtbl = {
    ps_qi, static_addref, static_release, ps_count, ps_get_at, ps_get, ps_set, ps_commit,
};

/* IAudioEndpointVolume: the kernel's master volume for the flow (0..65536
 * a channel); scalars are amplitudes, levels their decibels */
static void ev_read(int flow, float ch[2], BOOL *mute)
{
    UINT32 v[3] = { 65536, 65536, 0 };
    NtNovaAudioCtl(0, 9, (ULONG_PTR)flow, v);
    ch[0] = v[0] / 65536.0f;
    ch[1] = v[1] / 65536.0f;
    if (mute) *mute = v[2] != 0;
}
static void ev_write(int flow, const float ch[2], BOOL mute)
{
    UINT32 v[3] = { (UINT32)(ch[0] * 65536.0f + 0.5f), (UINT32)(ch[1] * 65536.0f + 0.5f), (UINT32)!!mute };
    NtNovaAudioCtl(0, 8, (ULONG_PTR)flow, v);
}
static HRESULT STDMETHODCALLTYPE ev_qi(Static *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioEndpointVolume) ||
           IsEqualIID(riid, &IID_IAudioEndpointVolumeEx) ? s : 0;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE ev_notify(Static *s, void *cb) { (void)s; return cb ? S_OK : E_POINTER; }
static HRESULT STDMETHODCALLTYPE ev_count(Static *s, UINT *n) { (void)s; if (!n) return E_POINTER; *n = 2; return S_OK; }
static HRESULT ev_set_scalar(Static *s, int ch, float v)        /* ch -1: both (the master) */
{
    if (v < 0 || v > 1) return E_INVALIDARG;
    float c[2];
    BOOL mute;
    ev_read(s->flow, c, &mute);
    if (ch < 0) {                                               /* keeps the balance */
        float top = c[0] > c[1] ? c[0] : c[1];
        if (top > 0) { c[0] = c[0] / top * v; c[1] = c[1] / top * v; }
        else c[0] = c[1] = v;
    } else {
        c[ch] = v;
    }
    ev_write(s->flow, c, mute);
    return S_OK;
}
static float ev_get_scalar(Static *s, int ch)
{
    float c[2];
    ev_read(s->flow, c, 0);
    return ch < 0 ? (c[0] > c[1] ? c[0] : c[1]) : c[ch];
}
static HRESULT STDMETHODCALLTYPE ev_set_level(Static *s, float db, const GUID *ctx)
{ (void)ctx; return db < VOL_MIN_DB || db > 0 ? E_INVALIDARG : ev_set_scalar(s, -1, lin_of(db)); }
static HRESULT STDMETHODCALLTYPE ev_set_level_scalar(Static *s, float v, const GUID *ctx) { (void)ctx; return ev_set_scalar(s, -1, v); }
static HRESULT STDMETHODCALLTYPE ev_get_level(Static *s, float *db) { if (!db) return E_POINTER; *db = db_of(ev_get_scalar(s, -1)); return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_get_level_scalar(Static *s, float *v) { if (!v) return E_POINTER; *v = ev_get_scalar(s, -1); return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_set_ch_level(Static *s, UINT ch, float db, const GUID *ctx)
{ (void)ctx; return ch > 1 || db < VOL_MIN_DB || db > 0 ? E_INVALIDARG : ev_set_scalar(s, (int)ch, lin_of(db)); }
static HRESULT STDMETHODCALLTYPE ev_set_ch_scalar(Static *s, UINT ch, float v, const GUID *ctx)
{ (void)ctx; return ch > 1 ? E_INVALIDARG : ev_set_scalar(s, (int)ch, v); }
static HRESULT STDMETHODCALLTYPE ev_get_ch_level(Static *s, UINT ch, float *db)
{ if (!db) return E_POINTER; if (ch > 1) return E_INVALIDARG; *db = db_of(ev_get_scalar(s, (int)ch)); return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_get_ch_scalar(Static *s, UINT ch, float *v)
{ if (!v) return E_POINTER; if (ch > 1) return E_INVALIDARG; *v = ev_get_scalar(s, (int)ch); return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_set_mute(Static *s, BOOL m, const GUID *ctx)
{
    (void)ctx;
    float c[2];
    BOOL was;
    ev_read(s->flow, c, &was);
    ev_write(s->flow, c, m);
    return !!was == !!m ? S_FALSE : S_OK;
}
static HRESULT STDMETHODCALLTYPE ev_get_mute(Static *s, BOOL *m) { float c[2]; if (!m) return E_POINTER; ev_read(s->flow, c, m); return S_OK; }
#define VOL_STEPS 100
static HRESULT STDMETHODCALLTYPE ev_step_info(Static *s, UINT *step, UINT *count)
{
    if (!step || !count) return E_POINTER;
    *count = VOL_STEPS + 1;
    *step = (UINT)(ev_get_scalar(s, -1) * VOL_STEPS + 0.5f);
    return S_OK;
}
static HRESULT ev_step(Static *s, int d)
{
    int step = (int)(ev_get_scalar(s, -1) * VOL_STEPS + 0.5f) + d;
    if (step < 0) step = 0;
    if (step > VOL_STEPS) step = VOL_STEPS;
    return ev_set_scalar(s, -1, (float)step / VOL_STEPS);
}
static HRESULT STDMETHODCALLTYPE ev_step_up(Static *s, const GUID *ctx) { (void)ctx; return ev_step(s, 1); }
static HRESULT STDMETHODCALLTYPE ev_step_down(Static *s, const GUID *ctx) { (void)ctx; return ev_step(s, -1); }
static HRESULT STDMETHODCALLTYPE ev_hw(Static *s, DWORD *mask) { (void)s; if (!mask) return E_POINTER; *mask = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_range(Static *s, float *mn, float *mx, float *inc)
{
    (void)s;
    if (!mn || !mx || !inc) return E_POINTER;
    *mn = VOL_MIN_DB;
    *mx = 0;
    *inc = VOL_STEP_DB;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ev_range_ch(Static *s, UINT ch, float *mn, float *mx, float *inc)
{ return ch > 1 ? E_INVALIDARG : ev_range(s, mn, mx, inc); }
static const struct {
    void *qi, *addref, *release, *reg, *unreg, *count, *set_level, *set_level_scalar, *get_level, *get_level_scalar,
         *set_ch_level, *set_ch_scalar, *get_ch_level, *get_ch_scalar, *set_mute, *get_mute, *step_info,
         *step_up, *step_down, *hw, *range, *range_ch;
} g_epvol_vtbl = {
    ev_qi, static_addref, static_release, ev_notify, ev_notify, ev_count, ev_set_level, ev_set_level_scalar,
    ev_get_level, ev_get_level_scalar, ev_set_ch_level, ev_set_ch_scalar, ev_get_ch_level, ev_get_ch_scalar,
    ev_set_mute, ev_get_mute, ev_step_info, ev_step_up, ev_step_down, ev_hw, ev_range, ev_range_ch,
};

/* A collection: the endpoints of its flow (2: both) that exist */
static HRESULT STDMETHODCALLTYPE col_qi(Static *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMMDeviceCollection) ? s : 0;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static UINT col_list(Static *s, Static **out)
{
    UINT n = 0;
    for (int f = 0; f < 2; f++)
        if ((s->flow == f || s->flow == 2) && device_present_flow(f)) out[n++] = &g_device[f];
    return n;
}
static HRESULT STDMETHODCALLTYPE col_count(Static *s, UINT *n) { Static *l[2]; if (!n) return E_POINTER; *n = col_list(s, l); return S_OK; }
static HRESULT STDMETHODCALLTYPE col_item(Static *s, UINT i, void **dev)
{
    Static *l[2];
    if (!dev) return E_POINTER;
    *dev = 0;
    if (i >= col_list(s, l)) return E_INVALIDARG;
    *dev = l[i];
    return S_OK;
}
static const struct { void *qi, *addref, *release, *count, *item; } g_collection_vtbl = {
    col_qi, static_addref, static_release, col_count, col_item,
};

static Static g_empty         = { &g_collection_vtbl, -1 };
static Static g_device[2]     = { { &g_device_vtbl, 0 }, { &g_device_vtbl, 1 } };
static Static g_endpoint[2]   = { { &g_endpoint_vtbl, 0 }, { &g_endpoint_vtbl, 1 } };
static Static g_props[2]      = { { &g_props_vtbl, 0 }, { &g_props_vtbl, 1 } };
static Static g_epvol[2]      = { { &g_epvol_vtbl, 0 }, { &g_epvol_vtbl, 1 } };
static Static g_collection[3] = { { &g_collection_vtbl, 0 }, { &g_collection_vtbl, 1 }, { &g_collection_vtbl, 2 } };

/* -----------------------------------------------------------------------
 * IMMDeviceEnumerator (a static object too)
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE en_qi(Static *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMMDeviceEnumerator) ? s : 0;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE en_enum(Static *s, int flow, DWORD mask, void **out)
{
    (void)s;
    if (!out) return E_POINTER;
    if (flow < 0 || flow > 2 || !mask || (mask & ~0xFu)) return E_INVALIDARG;
    *out = (mask & 1) ? &g_collection[flow] : &g_empty;      /* DEVICE_STATE_ACTIVE */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE en_default(Static *s, int flow, int role, void **out)
{
    (void)s;
    if (!out) return E_POINTER;
    *out = 0;
    if (flow < 0 || flow > 2 || role < 0 || role > 2) return E_INVALIDARG;
    if (flow == 2 || !device_present_flow(flow)) return E_NOTFOUND;
    *out = &g_device[flow];
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE en_get(Static *s, LPCWSTR id, void **out)
{
    (void)s;
    if (!id || !out) return E_POINTER;
    *out = 0;
    for (int f = 0; f < 2; f++)
        if (device_present_flow(f) && !lstrcmpiW(id, DEVICE_ID[f])) { *out = &g_device[f]; return S_OK; }
    return E_NOTFOUND;
}
static HRESULT STDMETHODCALLTYPE en_notify(Static *s, void *cb) { (void)s; return cb ? S_OK : E_POINTER; }
static const struct { void *qi, *addref, *release, *enum_eps, *get_default, *get, *reg, *unreg; } g_enum_vtbl = {
    en_qi, static_addref, static_release, en_enum, en_default, en_get, en_notify, en_notify,
};
static Static g_enumerator = { &g_enum_vtbl, 0 };

/* -----------------------------------------------------------------------
 * Class factory
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE cf_qi(IClassFactory *cf, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory) ? cf : 0;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *cf)  { (void)cf; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *cf) { (void)cf; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *cf, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)cf;
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION;
    return en_qi(&g_enumerator, riid, ppv);
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *cf, BOOL lock) { (void)cf; (void)lock; return S_OK; }
static const IClassFactoryVtbl g_cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory g_factory = { &g_cf_vtbl };

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (!IsEqualCLSID(clsid, &CLSID_MMDeviceEnumerator)) return CLASS_E_CLASSNOTAVAILABLE;
    return cf_qi(&g_factory, riid, ppv);
}

/* never: the enumerator, device and property store are static objects
 * programs keep across CoUninitialize */
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
