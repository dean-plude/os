/*
 * dsound.dll — DirectSound on NovaOS's mixer
 *
 * A DirectSound object mixes its playing secondary buffers in software,
 * as Windows' own DirectSound has since Vista: a thread wakes every 10 ms
 * and keeps one kernel mixer stream (48 kHz s16 stereo) about 40 ms ahead,
 * reading each buffer at its own frequency (linear interpolation), with
 * its volume, pan and, for 3D buffers, distance attenuation and pan from
 * the listener.  A buffer's play cursor is where the mixer reads minus what
 * the stream still holds; its write cursor is where the mixer reads.
 * Notification events fire as the mixer passes their offsets.
 *
 * DirectSoundCapture records from the kernel's capture stream into a
 * circular buffer in the program's format.
 *
 * The devices are NovaOS's sound devices (audiodev.h): DirectSoundEnumerate
 * and DirectSoundCaptureEnumerate list the "Primary Sound Driver" (the
 * default, which follows Settings) and then every device by its Windows
 * name, each with its endpoint's GUID (PKEY_AudioEndpoint_GUID), and a
 * program that passes one of those GUIDs plays on (records from) that
 * device.
 *
 * The primary buffer takes a format and a volume but cannot be locked
 * (DSSCL_WRITEPRIMARY is refused), and there is no hardware mixing or FX.
 */

#include <windows.h>
#include <avrt.h>
#include <winternl.h>
#include <objbase.h>
#include "../winmm/audioconv.h"
#include "../winmm/audiodev.h"

int _fltused = 1;
#define EXPORT __declspec(dllexport)
#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED ((HRESULT)0x80070490L)
#endif

DEFINE_GUID(CLSID_DirectSound,          0x47D4D946, 0x62E8, 0x11CF, 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(CLSID_DirectSound8,         0x3901CC3F, 0x84B5, 0x4FA4, 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B);
DEFINE_GUID(CLSID_DirectSoundCapture,   0xB0210780, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16);
DEFINE_GUID(CLSID_DirectSoundCapture8,  0xE4BCAC13, 0x7F99, 0x4908, 0x9A, 0x8E, 0x74, 0xE3, 0xBF, 0x24, 0xB6, 0xE1);
DEFINE_GUID(IID_IDirectSound,           0x279AFA83, 0x4981, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(IID_IDirectSound8,          0xC50A7E93, 0xF395, 0x4834, 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66);
DEFINE_GUID(IID_IDirectSoundBuffer,     0x279AFA85, 0x4981, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(IID_IDirectSoundBuffer8,    0x6825A449, 0x7524, 0x4D82, 0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E);
DEFINE_GUID(IID_IDirectSoundNotify,     0xB0210783, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16);
DEFINE_GUID(IID_IDirectSound3DBuffer,   0x279AFA86, 0x4981, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(IID_IDirectSound3DListener, 0x279AFA84, 0x4981, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(IID_IDirectSoundCapture,    0xB0210781, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16);
DEFINE_GUID(IID_IDirectSoundCaptureBuffer,  0xB0210782, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16);
DEFINE_GUID(IID_IDirectSoundCaptureBuffer8, 0x00990DF4, 0x0DBB, 0x4872, 0x83, 0x3E, 0x6D, 0x30, 0x3E, 0x80, 0xAE, 0xB6);
DEFINE_GUID(IID_IKsPropertySet,         0x31EFAC30, 0x515C, 0x11D0, 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93);
DEFINE_GUID(DSDEVID_DefaultPlayback,     0xDEF00000, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);
DEFINE_GUID(DSDEVID_DefaultCapture,      0xDEF00001, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);
DEFINE_GUID(DSDEVID_DefaultVoicePlayback, 0xDEF00002, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);
DEFINE_GUID(DSDEVID_DefaultVoiceCapture, 0xDEF00003, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);

#define DS_OK                   S_OK
#define DSERR_ALLOCATED         ((HRESULT)0x8878000A)
#define DSERR_CONTROLUNAVAIL    ((HRESULT)0x8878001E)
#define DSERR_INVALIDPARAM      E_INVALIDARG
#define DSERR_INVALIDCALL       ((HRESULT)0x88780032)
#define DSERR_GENERIC           E_FAIL
#define DSERR_PRIOLEVELNEEDED   ((HRESULT)0x88780046)
#define DSERR_OUTOFMEMORY       E_OUTOFMEMORY
#define DSERR_BADFORMAT         ((HRESULT)0x88780064)
#define DSERR_UNSUPPORTED       E_NOTIMPL
#define DSERR_NODRIVER          ((HRESULT)0x88780078)
#define DSERR_ALREADYINITIALIZED ((HRESULT)0x88780082)
#define DSERR_NOAGGREGATION     CLASS_E_NOAGGREGATION
#define DSERR_BUFFERLOST        ((HRESULT)0x88780096)
#define DSERR_UNINITIALIZED     ((HRESULT)0x887800AA)
#define DSERR_NOINTERFACE       E_NOINTERFACE

#define DSBCAPS_PRIMARYBUFFER       0x00000001
#define DSBCAPS_STATIC              0x00000002
#define DSBCAPS_LOCHARDWARE         0x00000004
#define DSBCAPS_LOCSOFTWARE         0x00000008
#define DSBCAPS_CTRL3D              0x00000010
#define DSBCAPS_CTRLFREQUENCY       0x00000020
#define DSBCAPS_CTRLPAN             0x00000040
#define DSBCAPS_CTRLVOLUME          0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY  0x00000100
#define DSBCAPS_CTRLFX              0x00000200
#define DSBCAPS_GLOBALFOCUS         0x00008000
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBPLAY_LOOPING             0x00000001
#define DSBSTATUS_PLAYING           0x00000001
#define DSBSTATUS_BUFFERLOST        0x00000002
#define DSBSTATUS_LOOPING           0x00000004
#define DSBLOCK_FROMWRITECURSOR     0x00000001
#define DSBLOCK_ENTIREBUFFER        0x00000002
#define DSBPN_OFFSETSTOP            0xFFFFFFFF
#define DSBSIZE_MIN                 4
#define DSBSIZE_MAX                 0x0FFFFFFF
#define DSBFREQUENCY_ORIGINAL       0
#define DSBFREQUENCY_MIN            100
#define DSBFREQUENCY_MAX            200000
#define DSBVOLUME_MIN               (-10000)
#define DSBPAN_LEFT                 (-10000)
#define DSBPAN_RIGHT                10000
#define DSSCL_WRITEPRIMARY          4
#define DS3DMODE_NORMAL             0
#define DS3DMODE_HEADRELATIVE       1
#define DS3DMODE_DISABLE            2
#define DSCBSTART_LOOPING           1
#define DSCBSTATUS_CAPTURING        1
#define DSCBSTATUS_LOOPING          2

typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; AcWaveFormat *lpwfxFormat; GUID guid3DAlgorithm; } DSBUFFERDESC;
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead; } DSBCAPS;
typedef struct {
    DWORD dwSize, dwFlags, dwMinSecondarySampleRate, dwMaxSecondarySampleRate, dwPrimaryBuffers,
          dwMaxHwMixingAllBuffers, dwMaxHwMixingStaticBuffers, dwMaxHwMixingStreamingBuffers,
          dwFreeHwMixingAllBuffers, dwFreeHwMixingStaticBuffers, dwFreeHwMixingStreamingBuffers,
          dwMaxHw3DAllBuffers, dwMaxHw3DStaticBuffers, dwMaxHw3DStreamingBuffers, dwFreeHw3DAllBuffers,
          dwFreeHw3DStaticBuffers, dwFreeHw3DStreamingBuffers, dwTotalHwMemBytes, dwFreeHwMemBytes,
          dwMaxContigFreeHwMemBytes, dwUnlockTransferRateHwBuffers, dwPlayCpuOverheadSwBuffers,
          dwReserved1, dwReserved2;
} DSCAPS;
typedef struct { DWORD dwOffset; HANDLE hEventNotify; } DSBPOSITIONNOTIFY;
typedef struct { float x, y, z; } D3DVECTOR;
typedef struct {
    DWORD dwSize; D3DVECTOR vPosition, vVelocity; DWORD dwInsideConeAngle, dwOutsideConeAngle;
    D3DVECTOR vConeOrientation; LONG lConeOutsideVolume; float flMinDistance, flMaxDistance; DWORD dwMode;
} DS3DBUFFER;
typedef struct {
    DWORD dwSize; D3DVECTOR vPosition, vVelocity, vOrientFront, vOrientTop;
    float flDistanceFactor, flRolloffFactor, flDopplerFactor;
} DS3DLISTENER;
typedef struct { DWORD dwSize, dwFlags, dwFormats, dwChannels; } DSCCAPS;
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; AcWaveFormat *lpwfxFormat; DWORD dwFXCount; void *lpDSCFXDesc; } DSCBUFFERDESC;
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; } DSCBCAPS;

typedef struct {
    ULONGLONG written, consumed, played;
    UINT32 queued, capacity, running, latency;
} StreamStatus;

#define TICK      10                /* ms between mixes */
#define AHEAD     1920              /* frames kept queued in the stream (40 ms) */
#define CHUNK     960

/* -----------------------------------------------------------------------
 * Small maths (no maths library here)
 * ----------------------------------------------------------------------- */
static double exp_(double x)
{
    int k = (int)(x / 0.69314718055994531);
    double r = x - k * 0.69314718055994531, t = 1, sum = 1;
    for (int i = 1; i < 30; i++) { t *= r / i; sum += t; }
    for (; k > 0; k--) sum *= 2;
    for (; k < 0; k++) sum /= 2;
    return sum;
}
/* hundredths of a decibel to an amplitude */
static float amp_of(LONG mb) { return mb <= -10000 ? 0.0f : mb >= 0 ? 1.0f : (float)exp_(mb / 2000.0 * 2.302585092994046); }
static float sqrt_(float x)
{
    if (x <= 0) return 0;
    float r = x > 1 ? x : 1;
    for (int i = 0; i < 30; i++) r = 0.5f * (r + x / r);
    return r;
}

/* The device @g names for playback (or @capture: recording): 0 for the
 * default (NULL, GUID_NULL and the DSDEVID_Default* GUIDs), else a
 * device's id; FALSE if there is no such device attached */
static BOOL find_device(const GUID *g, BOOL capture, UINT32 *id)
{
    AudioDeviceList l;
    UINT n = audio_devices(capture, &l);
    *id = 0;
    if (!n) return FALSE;
    if (!g || IsEqualGUID(g, &GUID_NULL)) return TRUE;
    if (capture ? IsEqualGUID(g, &DSDEVID_DefaultCapture) || IsEqualGUID(g, &DSDEVID_DefaultVoiceCapture)
                : IsEqualGUID(g, &DSDEVID_DefaultPlayback) || IsEqualGUID(g, &DSDEVID_DefaultVoicePlayback)) return TRUE;
    UINT32 want = audio_guid_device(g);
    for (UINT i = 0; want && i < n; i++)
        if (l.dev[i].id == want) { *id = want; return TRUE; }
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Objects
 * ----------------------------------------------------------------------- */
typedef struct Device Device;
typedef struct Buffer Buffer;
typedef struct { const void *vtbl; Buffer *b; } BSub;

struct Buffer {
    const void *vtbl;               /* IDirectSoundBuffer8 */
    BSub        notify, b3d, props;
    LONG        refs;
    Device     *dev;
    BOOL        primary;
    DWORD       flags;
    AudioConv   fmt;                /* the samples' format (its rate is the original frequency) */
    AcWaveFormatExt wfx;
    BYTE       *data;               /* shared with duplicates */
    LONG       *data_refs;
    DWORD       size;               /* bytes */
    DWORD       frames;
    double      pos;                /* frames: where the mixer reads */
    DWORD       freq;
    LONG        volume, pan;
    BOOL        playing, looping;
    DSBPOSITIONNOTIFY *notes;
    DWORD       nnotes;
    DS3DBUFFER  p3d;
    Buffer     *next;
};

struct Device {
    const void *vtbl;               /* IDirectSound8 */
    LONG        refs;
    BOOL        init;
    CRITICAL_SECTION lock;
    INT_PTR     stream;
    UINT32      devid;              /* the device it plays on (0: the default) */
    HANDLE      thread, quit;
    Buffer     *buffers;            /* secondary buffers */
    Buffer     *primary;            /* while the program holds it */
    AcWaveFormatExt pfmt;           /* the primary buffer's format */
    LONG        pvolume, ppan;
    DS3DLISTENER listener;
    BSub        listener_sub;
    DWORD       speaker;
    float       mix[AHEAD * 2];
};

static const void *g_buffer_vtbl, *g_notify_vtbl, *g_b3d_vtbl, *g_listener_vtbl, *g_props_vtbl;

/* -----------------------------------------------------------------------
 * The mixer
 * ----------------------------------------------------------------------- */
static void frame_at(const Buffer *b, DWORD i, float out[2])
{
    const BYTE *p = b->data + (size_t)i * b->fmt.block;
    UINT bytes = b->fmt.block / b->fmt.channels;
    out[0] = ac_sample(&b->fmt, p);
    out[1] = b->fmt.channels > 1 ? ac_sample(&b->fmt, p + bytes) : out[0];
}

/* The gains a buffer plays at: volume, pan, and for a 3D buffer its
 * distance from the listener and side */
static void gains(Device *d, Buffer *b, float g[2])
{
    float v = amp_of(b->volume) * amp_of(d->pvolume);
    LONG pan = b->pan;
    float l = pan > 0 ? amp_of(-pan) : 1.0f, r = pan < 0 ? amp_of(pan) : 1.0f;
    if ((b->flags & DSBCAPS_CTRL3D) && b->p3d.dwMode != DS3DMODE_DISABLE) {
        D3DVECTOR rel = b->p3d.vPosition;
        DS3DLISTENER *L = &d->listener;
        if (b->p3d.dwMode == DS3DMODE_NORMAL) {
            rel.x -= L->vPosition.x; rel.y -= L->vPosition.y; rel.z -= L->vPosition.z;
        }
        float dist = sqrt_(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z) * L->flDistanceFactor;
        float mn = b->p3d.flMinDistance > 0 ? b->p3d.flMinDistance : 1.0f, mx = b->p3d.flMaxDistance;
        if (dist > mx) dist = mx;
        float att = dist <= mn ? 1.0f : mn / (mn + L->flRolloffFactor * (dist - mn));
        v *= att;
        /* the listener's right: front x top */
        D3DVECTOR f = L->vOrientFront, t = L->vOrientTop;
        D3DVECTOR right = { t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x };
        float rl = sqrt_(right.x * right.x + right.y * right.y + right.z * right.z);
        float side = 0, rd = sqrt_(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        if (rl > 0 && rd > 0) side = (rel.x * right.x + rel.y * right.y + rel.z * right.z) / (rl * rd);
        if (b->p3d.dwMode == DS3DMODE_HEADRELATIVE && rd > 0) side = rel.x / rd;
        l *= side > 0 ? 1.0f - 0.7f * side : 1.0f;
        r *= side < 0 ? 1.0f + 0.7f * side : 1.0f;
    }
    g[0] = v * l;
    g[1] = v * r;
}

/* Fire the notifications whose offsets lie in [from, to) bytes */
static void notify_range(Buffer *b, DWORD from, DWORD to)
{
    for (DWORD i = 0; i < b->nnotes; i++) {
        DWORD o = b->notes[i].dwOffset;
        if (o == DSBPN_OFFSETSTOP) continue;
        if (from <= to ? (o >= from && o < to) : (o >= from || o < to)) SetEvent(b->notes[i].hEventNotify);
    }
}
static void notify_stop(Buffer *b)
{
    for (DWORD i = 0; i < b->nnotes; i++)
        if (b->notes[i].dwOffset == DSBPN_OFFSETSTOP) SetEvent(b->notes[i].hEventNotify);
}

/* Mix @n frames of every playing buffer into d->mix (locked) */
static void mix(Device *d, UINT n)
{
    memset(d->mix, 0, (size_t)n * 2 * sizeof(float));
    for (Buffer *b = d->buffers; b; b = b->next) {
        if (!b->playing || !b->frames) continue;
        float g[2];
        gains(d, b, g);
        double step = (double)b->freq / AC_RATE;
        DWORD from = (DWORD)b->pos * b->fmt.block;
        BOOL stopped = FALSE;
        for (UINT k = 0; k < n; k++) {
            DWORD i = (DWORD)b->pos;
            float fr = (float)(b->pos - i), a[2], c[2];
            frame_at(b, i, a);
            DWORD j = i + 1;
            if (j >= b->frames) j = b->looping ? 0 : i;
            frame_at(b, j, c);
            d->mix[k * 2]     += (a[0] + (c[0] - a[0]) * fr) * g[0];
            d->mix[k * 2 + 1] += (a[1] + (c[1] - a[1]) * fr) * g[1];
            b->pos += step;
            if (b->pos >= b->frames) {
                if (b->looping) {
                    b->pos -= b->frames;
                    if (b->pos >= b->frames) b->pos = 0;
                } else {
                    b->pos = 0;
                    b->playing = FALSE;
                    stopped = TRUE;
                    break;
                }
            }
        }
        DWORD to = (DWORD)b->pos * b->fmt.block;
        if (stopped) { notify_range(b, from, b->size); notify_stop(b); }
        else if (to != from) notify_range(b, from, to);
    }
}

static DWORD WINAPI mixer_thread(LPVOID p)
{
    Device *d = p;
    SHORT out[AHEAD * 2];
    DWORD mm = 0;
    AvSetMmThreadCharacteristicsW(L"Playback", &mm);   /* (MMCSS: above any busy or boosted program thread) */
    do {
        StreamStatus st;
        if (NtNovaAudioCtl(d->stream, 0, 0, &st)) continue;
        UINT need = st.queued < AHEAD ? AHEAD - st.queued : 0;
        if (!need) continue;
        EnterCriticalSection(&d->lock);
        BOOL any = FALSE;
        for (Buffer *b = d->buffers; b && !any; b = b->next) any = b->playing;
        if (any) mix(d, need);
        LeaveCriticalSection(&d->lock);
        if (!any) continue;                         /* nothing plays: let the stream drain */
        for (UINT k = 0; k < need * 2; k++) out[k] = ac_s16(d->mix[k]);
        UINT done = 0;
        while (done < need) {
            LONG_PTR got = NtNovaAudioWrite(d->stream, out + done * 2, need - done);
            if (got <= 0) break;
            done += (UINT)got;
        }
    } while (WaitForSingleObject(d->quit, TICK) == WAIT_TIMEOUT);
    return 0;
}

/* frames the stream holds that the mixer already read from buffers */
static UINT queued_frames(Device *d)
{
    StreamStatus st;
    return d->stream && NtNovaAudioCtl(d->stream, 0, 0, &st) == 0 ? st.queued : 0;
}

/* -----------------------------------------------------------------------
 * IDirectSoundBuffer8 (the primary buffer is one too, with fewer powers)
 * ----------------------------------------------------------------------- */
static HRESULT new_buffer(Device *d, const DSBUFFERDESC *desc, Buffer **out);
static ULONG STDMETHODCALLTYPE dev_addref(Device *d);
static ULONG STDMETHODCALLTYPE dev_release(Device *d);

static HRESULT STDMETHODCALLTYPE buf_qi(Buffer *b, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundBuffer) ||
        (IsEqualIID(riid, &IID_IDirectSoundBuffer8) && !b->primary))
        *ppv = b;
    else if (IsEqualIID(riid, &IID_IDirectSoundNotify) && !b->primary && (b->flags & DSBCAPS_CTRLPOSITIONNOTIFY))
        *ppv = &b->notify;
    else if (IsEqualIID(riid, &IID_IDirectSound3DBuffer) && !b->primary && (b->flags & DSBCAPS_CTRL3D))
        *ppv = &b->b3d;
    else if (IsEqualIID(riid, &IID_IDirectSound3DListener) && b->primary && (b->flags & DSBCAPS_CTRL3D))
        *ppv = &b->dev->listener_sub;
    else if (IsEqualIID(riid, &IID_IKsPropertySet))
        *ppv = &b->props;
    if (!*ppv) return E_NOINTERFACE;
    InterlockedIncrement(&b->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE buf_addref(Buffer *b) { return (ULONG)InterlockedIncrement(&b->refs); }
static ULONG STDMETHODCALLTYPE buf_release(Buffer *b)
{
    LONG r = InterlockedDecrement(&b->refs);
    if (r) return (ULONG)r;
    Device *d = b->dev;
    EnterCriticalSection(&d->lock);
    if (b->primary) d->primary = 0;
    else {
        Buffer **pp = &d->buffers;
        while (*pp && *pp != b) pp = &(*pp)->next;
        if (*pp) *pp = b->next;
    }
    LeaveCriticalSection(&d->lock);
    if (b->data_refs && !InterlockedDecrement(b->data_refs)) {
        HeapFree(GetProcessHeap(), 0, b->data);
        HeapFree(GetProcessHeap(), 0, b->data_refs);
    }
    if (b->notes) HeapFree(GetProcessHeap(), 0, b->notes);
    HeapFree(GetProcessHeap(), 0, b);
    dev_release(d);
    return 0;
}

static HRESULT STDMETHODCALLTYPE buf_get_caps(Buffer *b, DSBCAPS *c)
{
    if (!c || c->dwSize < sizeof(*c)) return DSERR_INVALIDPARAM;
    c->dwFlags = b->flags | DSBCAPS_LOCSOFTWARE;
    c->dwBufferBytes = b->primary ? AHEAD * 4 : b->size;
    c->dwUnlockTransferRate = 0;
    c->dwPlayCpuOverhead = 0;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_get_pos(Buffer *b, DWORD *play, DWORD *write)
{
    Device *d = b->dev;
    if (b->primary) {
        if (play) *play = 0;
        if (write) *write = 0;
        return DS_OK;
    }
    UINT q = queued_frames(d);
    EnterCriticalSection(&d->lock);
    DWORD w = (DWORD)b->pos;
    DWORD back = b->playing ? (DWORD)(q * ((double)b->freq / AC_RATE)) : 0;
    DWORD p = back > w ? (b->looping ? b->frames - (back - w) % b->frames : 0) : w - back;
    if (p >= b->frames) p = 0;
    LeaveCriticalSection(&d->lock);
    if (play) *play = p * b->fmt.block;
    if (write) *write = w * b->fmt.block;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_get_format(Buffer *b, AcWaveFormat *f, DWORD n, DWORD *written)
{
    const AcWaveFormat *src = b->primary ? &b->dev->pfmt.Format : &b->wfx.Format;
    DWORD need = sizeof(AcWaveFormat) + src->cbSize;
    if (written) *written = need;
    if (!f) return written ? DS_OK : DSERR_INVALIDPARAM;
    memcpy(f, src, n < need ? n : need);
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_get_volume(Buffer *b, LONG *v)
{
    if (!v) return DSERR_INVALIDPARAM;
    if (!(b->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    *v = b->primary ? b->dev->pvolume : b->volume;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_get_pan(Buffer *b, LONG *v)
{
    if (!v) return DSERR_INVALIDPARAM;
    if (!(b->flags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    *v = b->primary ? b->dev->ppan : b->pan;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_get_freq(Buffer *b, DWORD *v)
{
    if (!v) return DSERR_INVALIDPARAM;
    if (b->primary) { *v = b->dev->pfmt.Format.nSamplesPerSec; return DS_OK; }
    if (!(b->flags & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    *v = b->freq;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_get_status(Buffer *b, DWORD *s)
{
    if (!s) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&b->dev->lock);
    *s = b->playing ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0) : 0;
    if (b->primary) *s = DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;
    LeaveCriticalSection(&b->dev->lock);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_initialize(Buffer *b, void *ds, const DSBUFFERDESC *desc)
{ (void)b; (void)ds; (void)desc; return DSERR_ALREADYINITIALIZED; }

static HRESULT STDMETHODCALLTYPE buf_lock(Buffer *b, DWORD off, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags)
{
    if (p1) *p1 = 0;
    if (n1) *n1 = 0;
    if (p2) *p2 = 0;
    if (n2) *n2 = 0;
    if (b->primary) return DSERR_PRIOLEVELNEEDED;
    if (!p1 || !n1) return DSERR_INVALIDPARAM;
    if (flags & DSBLOCK_FROMWRITECURSOR) buf_get_pos(b, 0, &off);
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = b->size;
    if (off >= b->size || !bytes || bytes > b->size) return DSERR_INVALIDPARAM;
    *p1 = b->data + off;
    if (off + bytes <= b->size) { *n1 = bytes; return DS_OK; }
    *n1 = b->size - off;
    if (p2) *p2 = b->data;
    if (n2) *n2 = bytes - *n1;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_play(Buffer *b, DWORD reserved, DWORD prio, DWORD flags)
{
    (void)reserved; (void)prio;
    if (b->primary) return DS_OK;
    EnterCriticalSection(&b->dev->lock);
    b->looping = (flags & DSBPLAY_LOOPING) != 0;
    b->playing = TRUE;
    LeaveCriticalSection(&b->dev->lock);
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_set_pos(Buffer *b, DWORD off)
{
    if (b->primary) return DSERR_INVALIDCALL;
    if (off >= b->size) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&b->dev->lock);
    b->pos = off / b->fmt.block;
    LeaveCriticalSection(&b->dev->lock);
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_set_format(Buffer *b, const AcWaveFormat *f)
{
    if (!b->primary) return DSERR_INVALIDCALL;
    AudioConv c;
    if (!f || !ac_init(&c, f)) return DSERR_BADFORMAT;
    memset(&b->dev->pfmt, 0, sizeof(b->dev->pfmt));
    memcpy(&b->dev->pfmt, f, sizeof(AcWaveFormat) + (f->wFormatTag == 0xFFFE ? 22 : 0));
    if (f->wFormatTag != 0xFFFE) b->dev->pfmt.Format.cbSize = 0;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE buf_set_volume(Buffer *b, LONG v)
{
    if (!(b->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    if (v < DSBVOLUME_MIN || v > 0) return DSERR_INVALIDPARAM;
    if (b->primary) b->dev->pvolume = v; else b->volume = v;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_set_pan(Buffer *b, LONG v)
{
    if (!(b->flags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    if (v < DSBPAN_LEFT || v > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
    if (b->primary) b->dev->ppan = v; else b->pan = v;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_set_freq(Buffer *b, DWORD v)
{
    if (b->primary || !(b->flags & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    if (v == DSBFREQUENCY_ORIGINAL) v = b->fmt.rate;
    if (v < DSBFREQUENCY_MIN || v > DSBFREQUENCY_MAX) return DSERR_INVALIDPARAM;
    b->freq = v;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_stop(Buffer *b)
{
    if (b->primary) return DS_OK;
    EnterCriticalSection(&b->dev->lock);
    BOOL was = b->playing;
    b->playing = FALSE;
    if (was) notify_stop(b);
    LeaveCriticalSection(&b->dev->lock);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_unlock(Buffer *b, void *p1, DWORD n1, void *p2, DWORD n2)
{ (void)p1; (void)n1; (void)p2; (void)n2; return b->primary ? DSERR_PRIOLEVELNEEDED : DS_OK; }
static HRESULT STDMETHODCALLTYPE buf_restore(Buffer *b) { (void)b; return DS_OK; }
/* IDirectSoundBuffer8 */
static HRESULT STDMETHODCALLTYPE buf_set_fx(Buffer *b, DWORD n, void *desc, DWORD *res)
{
    (void)desc;
    if (!(b->flags & DSBCAPS_CTRLFX)) return DSERR_CONTROLUNAVAIL;
    for (DWORD i = 0; res && i < n; i++) res[i] = 3;        /* DSFXR_UNKNOWN */
    return n ? DSERR_UNSUPPORTED : DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_acquire(Buffer *b, DWORD flags, DWORD n, DWORD *res)
{
    (void)b; (void)flags;
    for (DWORD i = 0; res && i < n; i++) res[i] = 3;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE buf_object_in_path(Buffer *b, REFGUID obj, DWORD i, REFGUID iid, void **out)
{ (void)b; (void)obj; (void)i; (void)iid; if (out) *out = 0; return DSERR_UNSUPPORTED; }

static const struct {
    void *qi, *addref, *release, *get_caps, *get_pos, *get_format, *get_volume, *get_pan, *get_freq, *get_status,
         *initialize, *lock, *play, *set_pos, *set_format, *set_volume, *set_pan, *set_freq, *stop, *unlock,
         *restore, *set_fx, *acquire, *object_in_path;
} buffer_vtbl = {
    buf_qi, buf_addref, buf_release, buf_get_caps, buf_get_pos, buf_get_format, buf_get_volume, buf_get_pan,
    buf_get_freq, buf_get_status, buf_initialize, buf_lock, buf_play, buf_set_pos, buf_set_format, buf_set_volume,
    buf_set_pan, buf_set_freq, buf_stop, buf_unlock, buf_restore, buf_set_fx, buf_acquire, buf_object_in_path,
};

/* the sub-interfaces share the buffer's reference count */
static HRESULT STDMETHODCALLTYPE sub_qi(BSub *s, REFIID riid, void **ppv) { return buf_qi(s->b, riid, ppv); }
static ULONG STDMETHODCALLTYPE sub_addref(BSub *s) { return buf_addref(s->b); }
static ULONG STDMETHODCALLTYPE sub_release(BSub *s) { return buf_release(s->b); }

/* IDirectSoundNotify */
static HRESULT STDMETHODCALLTYPE nt_set(BSub *s, DWORD n, const DSBPOSITIONNOTIFY *p)
{
    Buffer *b = s->b;
    if (n && !p) return DSERR_INVALIDPARAM;
    for (DWORD i = 0; i < n; i++)
        if (p[i].dwOffset != DSBPN_OFFSETSTOP && p[i].dwOffset >= b->size) return DSERR_INVALIDPARAM;
    if (b->playing) return DSERR_INVALIDCALL;
    DSBPOSITIONNOTIFY *copy = n ? HeapAlloc(GetProcessHeap(), 0, n * sizeof(*p)) : 0;
    if (n && !copy) return DSERR_OUTOFMEMORY;
    if (n) memcpy(copy, p, n * sizeof(*p));
    EnterCriticalSection(&b->dev->lock);
    if (b->notes) HeapFree(GetProcessHeap(), 0, b->notes);
    b->notes = copy;
    b->nnotes = n;
    LeaveCriticalSection(&b->dev->lock);
    return DS_OK;
}
static const struct { void *qi, *addref, *release, *set; } notify_vtbl = { sub_qi, sub_addref, sub_release, nt_set };

/* IDirectSound3DBuffer: parameters the mixer's gains() reads */
#define B3D(s) (&(s)->b->p3d)
static HRESULT STDMETHODCALLTYPE b3_get_all(BSub *s, DS3DBUFFER *p)
{ if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM; *p = *B3D(s); p->dwSize = sizeof(*p); return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_cone_angles(BSub *s, DWORD *in, DWORD *out)
{ if (in) *in = B3D(s)->dwInsideConeAngle; if (out) *out = B3D(s)->dwOutsideConeAngle; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_cone_orient(BSub *s, D3DVECTOR *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->vConeOrientation; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_cone_vol(BSub *s, LONG *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->lConeOutsideVolume; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_max(BSub *s, float *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->flMaxDistance; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_min(BSub *s, float *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->flMinDistance; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_mode(BSub *s, DWORD *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->dwMode; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_pos(BSub *s, D3DVECTOR *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->vPosition; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_get_vel(BSub *s, D3DVECTOR *v) { if (!v) return DSERR_INVALIDPARAM; *v = B3D(s)->vVelocity; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_all(BSub *s, const DS3DBUFFER *p, DWORD apply)
{ (void)apply; if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM; *B3D(s) = *p; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_cone_angles(BSub *s, DWORD in, DWORD out, DWORD apply)
{ (void)apply; B3D(s)->dwInsideConeAngle = in; B3D(s)->dwOutsideConeAngle = out; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_cone_orient(BSub *s, float x, float y, float z, DWORD apply)
{ (void)apply; B3D(s)->vConeOrientation = (D3DVECTOR){ x, y, z }; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_cone_vol(BSub *s, LONG v, DWORD apply) { (void)apply; B3D(s)->lConeOutsideVolume = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_max(BSub *s, float v, DWORD apply) { (void)apply; B3D(s)->flMaxDistance = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_min(BSub *s, float v, DWORD apply) { (void)apply; B3D(s)->flMinDistance = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_mode(BSub *s, DWORD v, DWORD apply) { (void)apply; if (v > 2) return DSERR_INVALIDPARAM; B3D(s)->dwMode = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_pos(BSub *s, float x, float y, float z, DWORD apply)
{ (void)apply; B3D(s)->vPosition = (D3DVECTOR){ x, y, z }; return DS_OK; }
static HRESULT STDMETHODCALLTYPE b3_set_vel(BSub *s, float x, float y, float z, DWORD apply)
{ (void)apply; B3D(s)->vVelocity = (D3DVECTOR){ x, y, z }; return DS_OK; }
static const struct {
    void *qi, *addref, *release, *get_all, *get_cone_angles, *get_cone_orient, *get_cone_vol, *get_max, *get_min,
         *get_mode, *get_pos, *get_vel, *set_all, *set_cone_angles, *set_cone_orient, *set_cone_vol, *set_max,
         *set_min, *set_mode, *set_pos, *set_vel;
} b3d_vtbl = {
    sub_qi, sub_addref, sub_release, b3_get_all, b3_get_cone_angles, b3_get_cone_orient, b3_get_cone_vol,
    b3_get_max, b3_get_min, b3_get_mode, b3_get_pos, b3_get_vel, b3_set_all, b3_set_cone_angles,
    b3_set_cone_orient, b3_set_cone_vol, b3_set_max, b3_set_min, b3_set_mode, b3_set_pos, b3_set_vel,
};

/* IDirectSound3DListener (reached from the primary buffer; the device's) */
#define LIS(s) (&(s)->b->dev->listener)
static HRESULT STDMETHODCALLTYPE li_qi(BSub *s, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSound3DListener)) {
        *ppv = s;
        dev_addref(s->b->dev);
        return S_OK;
    }
    return s->b->dev->primary ? buf_qi(s->b->dev->primary, riid, ppv) : (*ppv = 0, E_NOINTERFACE);
}
static ULONG STDMETHODCALLTYPE li_addref(BSub *s) { return dev_addref(s->b->dev); }
static ULONG STDMETHODCALLTYPE li_release(BSub *s) { return dev_release(s->b->dev); }
static HRESULT STDMETHODCALLTYPE li_get_all(BSub *s, DS3DLISTENER *p)
{ if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM; *p = *LIS(s); p->dwSize = sizeof(*p); return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_get_dist(BSub *s, float *v) { if (!v) return DSERR_INVALIDPARAM; *v = LIS(s)->flDistanceFactor; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_get_doppler(BSub *s, float *v) { if (!v) return DSERR_INVALIDPARAM; *v = LIS(s)->flDopplerFactor; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_get_orient(BSub *s, D3DVECTOR *f, D3DVECTOR *t)
{ if (!f || !t) return DSERR_INVALIDPARAM; *f = LIS(s)->vOrientFront; *t = LIS(s)->vOrientTop; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_get_pos(BSub *s, D3DVECTOR *v) { if (!v) return DSERR_INVALIDPARAM; *v = LIS(s)->vPosition; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_get_rolloff(BSub *s, float *v) { if (!v) return DSERR_INVALIDPARAM; *v = LIS(s)->flRolloffFactor; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_get_vel(BSub *s, D3DVECTOR *v) { if (!v) return DSERR_INVALIDPARAM; *v = LIS(s)->vVelocity; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_set_all(BSub *s, const DS3DLISTENER *p, DWORD apply)
{ (void)apply; if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM; *LIS(s) = *p; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_set_dist(BSub *s, float v, DWORD apply) { (void)apply; if (v <= 0) return DSERR_INVALIDPARAM; LIS(s)->flDistanceFactor = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_set_doppler(BSub *s, float v, DWORD apply) { (void)apply; LIS(s)->flDopplerFactor = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_set_orient(BSub *s, float fx, float fy, float fz, float tx, float ty, float tz, DWORD apply)
{
    (void)apply;
    LIS(s)->vOrientFront = (D3DVECTOR){ fx, fy, fz };
    LIS(s)->vOrientTop = (D3DVECTOR){ tx, ty, tz };
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE li_set_pos(BSub *s, float x, float y, float z, DWORD apply)
{ (void)apply; LIS(s)->vPosition = (D3DVECTOR){ x, y, z }; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_set_rolloff(BSub *s, float v, DWORD apply) { (void)apply; LIS(s)->flRolloffFactor = v; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_set_vel(BSub *s, float x, float y, float z, DWORD apply)
{ (void)apply; LIS(s)->vVelocity = (D3DVECTOR){ x, y, z }; return DS_OK; }
static HRESULT STDMETHODCALLTYPE li_commit(BSub *s) { (void)s; return DS_OK; }
static const struct {
    void *qi, *addref, *release, *get_all, *get_dist, *get_doppler, *get_orient, *get_pos, *get_rolloff, *get_vel,
         *set_all, *set_dist, *set_doppler, *set_orient, *set_pos, *set_rolloff, *set_vel, *commit;
} listener_vtbl = {
    li_qi, li_addref, li_release, li_get_all, li_get_dist, li_get_doppler, li_get_orient, li_get_pos,
    li_get_rolloff, li_get_vel, li_set_all, li_set_dist, li_set_doppler, li_set_orient, li_set_pos,
    li_set_rolloff, li_set_vel, li_commit,
};

/* IKsPropertySet: no property sets (EAX and the like) are supported */
static HRESULT STDMETHODCALLTYPE ks_get(BSub *s, REFGUID set, ULONG id, void *inst, ULONG ni, void *data, ULONG nd, ULONG *ret)
{ (void)s; (void)set; (void)id; (void)inst; (void)ni; (void)data; (void)nd; if (ret) *ret = 0; return E_PROP_ID_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE ks_set(BSub *s, REFGUID set, ULONG id, void *inst, ULONG ni, void *data, ULONG nd)
{ (void)s; (void)set; (void)id; (void)inst; (void)ni; (void)data; (void)nd; return E_PROP_ID_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE ks_query(BSub *s, REFGUID set, ULONG id, ULONG *support)
{ (void)s; (void)set; (void)id; if (support) *support = 0; return E_PROP_ID_UNSUPPORTED; }
static const struct { void *qi, *addref, *release, *get, *set, *query; } props_vtbl = {
    sub_qi, sub_addref, sub_release, ks_get, ks_set, ks_query,
};

/* -----------------------------------------------------------------------
 * IDirectSound8
 * ----------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE dev_qi(Device *d, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSound) || IsEqualIID(riid, &IID_IDirectSound8)) {
        *ppv = d;
        dev_addref(d);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dev_addref(Device *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG STDMETHODCALLTYPE dev_release(Device *d)
{
    LONG r = InterlockedDecrement(&d->refs);
    if (r) return (ULONG)r;
    if (d->thread) {
        SetEvent(d->quit);
        WaitForSingleObject(d->thread, INFINITE);
        CloseHandle(d->thread);
    }
    if (d->quit) CloseHandle(d->quit);
    if (d->stream) NtClose((HANDLE)d->stream);
    DeleteCriticalSection(&d->lock);
    HeapFree(GetProcessHeap(), 0, d);
    return 0;
}

static HRESULT STDMETHODCALLTYPE dev_create_buffer(Device *d, const DSBUFFERDESC *desc, void **out, IUnknown *outer)
{
    if (!out) return DSERR_INVALIDPARAM;
    *out = 0;
    if (outer) return DSERR_NOAGGREGATION;
    if (!d->init) return DSERR_UNINITIALIZED;
    if (!desc || desc->dwSize < 20) return DSERR_INVALIDPARAM;
    Buffer *b;
    HRESULT hr = new_buffer(d, desc, &b);
    if (SUCCEEDED(hr)) *out = b;
    return hr;
}

static HRESULT STDMETHODCALLTYPE dev_get_caps(Device *d, DSCAPS *c)
{
    if (!c || c->dwSize < sizeof(*c)) return DSERR_INVALIDPARAM;
    if (!d->init) return DSERR_UNINITIALIZED;
    DWORD size = c->dwSize;
    memset(c, 0, sizeof(*c));
    c->dwSize = size;
    c->dwFlags = 0x00000001 | 0x00000004 | 0x00000400 | 0x00000020 | 0x00000010 | 0x00000040 | 0x00000100 |
                 0x00000200 | 0x00000800;   /* PRIMARYMONO/STEREO, 8/16-bit primary and secondary, CONTINUOUSRATE */
    c->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    c->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    c->dwPrimaryBuffers = 1;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE dev_duplicate(Device *d, Buffer *src, void **out)
{
    if (!src || !out) return DSERR_INVALIDPARAM;
    *out = 0;
    if (src->primary) return DSERR_INVALIDCALL;
    Buffer *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b));
    if (!b) return DSERR_OUTOFMEMORY;
    *b = *src;
    b->refs = 1;
    b->notify.b = b->b3d.b = b->props.b = b;
    b->playing = FALSE;
    b->pos = 0;
    b->notes = 0;
    b->nnotes = 0;
    InterlockedIncrement(b->data_refs);
    dev_addref(d);
    EnterCriticalSection(&d->lock);
    b->next = d->buffers;
    d->buffers = b;
    LeaveCriticalSection(&d->lock);
    *out = b;
    return DS_OK;
}

static HRESULT STDMETHODCALLTYPE dev_set_coop(Device *d, HWND w, DWORD level)
{
    (void)w;
    if (!d->init) return DSERR_UNINITIALIZED;
    return level < 1 || level > 4 ? DSERR_INVALIDPARAM : DS_OK;    /* WRITEPRIMARY is refused at Lock */
}
static HRESULT STDMETHODCALLTYPE dev_compact(Device *d) { return d->init ? DS_OK : DSERR_UNINITIALIZED; }
static HRESULT STDMETHODCALLTYPE dev_get_speaker(Device *d, DWORD *c)
{ if (!c) return DSERR_INVALIDPARAM; if (!d->init) return DSERR_UNINITIALIZED; *c = d->speaker; return DS_OK; }
static HRESULT STDMETHODCALLTYPE dev_set_speaker(Device *d, DWORD c) { if (!d->init) return DSERR_UNINITIALIZED; d->speaker = c; return DS_OK; }

static HRESULT STDMETHODCALLTYPE dev_initialize(Device *d, const GUID *dev)
{
    if (d->init) return DSERR_ALREADYINITIALIZED;
    if (!find_device(dev, FALSE, &d->devid)) return DSERR_NODRIVER;
    d->stream = NtNovaAudioOpen(AHEAD * 4);
    if (!d->stream) return DSERR_ALLOCATED;
    if (d->devid && !audio_route(d->stream, d->devid)) {         /* (unplugged meanwhile) */
        NtClose((HANDLE)d->stream);
        d->stream = 0;
        return DSERR_NODRIVER;
    }
    NtNovaAudioCtl(d->stream, 1, 1, 0);
    d->quit = CreateEventW(0, TRUE, FALSE, 0);
    d->thread = CreateThread(0, 64 * 1024, mixer_thread, d, 0, 0);
    if (d->thread) SetThreadPriority(d->thread, 15 /* THREAD_PRIORITY_TIME_CRITICAL */);
    d->init = TRUE;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE dev_verify(Device *d, DWORD *res)
{ if (!res) return DSERR_INVALIDPARAM; if (!d->init) return DSERR_UNINITIALIZED; *res = 0 /* DS_CERTIFIED */; return DS_OK; }

static const struct {
    void *qi, *addref, *release, *create_buffer, *get_caps, *duplicate, *set_coop, *compact, *get_speaker,
         *set_speaker, *initialize, *verify;
} device_vtbl = {
    dev_qi, dev_addref, dev_release, dev_create_buffer, dev_get_caps, dev_duplicate, dev_set_coop, dev_compact,
    dev_get_speaker, dev_set_speaker, dev_initialize, dev_verify,
};

static HRESULT new_buffer(Device *d, const DSBUFFERDESC *desc, Buffer **out)
{
    DWORD flags = desc->dwFlags;
    if ((flags & DSBCAPS_PRIMARYBUFFER) && d->primary) {
        buf_addref(d->primary);
        *out = d->primary;
        return DS_OK;
    }
    Buffer *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b));
    if (!b) return DSERR_OUTOFMEMORY;
    b->vtbl = &buffer_vtbl;
    b->notify = (BSub){ &notify_vtbl, b };
    b->b3d = (BSub){ &b3d_vtbl, b };
    b->props = (BSub){ &props_vtbl, b };
    b->refs = 1;
    b->dev = d;
    b->flags = flags;
    b->p3d = (DS3DBUFFER){ sizeof(DS3DBUFFER), { 0, 0, 0 }, { 0, 0, 0 }, 360, 360, { 0, 0, 1 }, 0, 1.0f, 1e9f, DS3DMODE_NORMAL };
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (desc->dwBufferBytes || desc->lpwfxFormat) { HeapFree(GetProcessHeap(), 0, b); return DSERR_INVALIDPARAM; }
        b->primary = TRUE;
        d->primary = b;
    } else {
        const AcWaveFormat *f = desc->lpwfxFormat;
        if (!f) { HeapFree(GetProcessHeap(), 0, b); return DSERR_INVALIDPARAM; }
        if (!ac_init(&b->fmt, f) || f->nBlockAlign != b->fmt.block) { HeapFree(GetProcessHeap(), 0, b); return DSERR_BADFORMAT; }
        if (desc->dwBufferBytes < DSBSIZE_MIN || desc->dwBufferBytes > DSBSIZE_MAX) {
            HeapFree(GetProcessHeap(), 0, b);
            return DSERR_INVALIDPARAM;
        }
        memcpy(&b->wfx, f, sizeof(AcWaveFormat) + (f->wFormatTag == 0xFFFE ? 22 : 0));
        if (f->wFormatTag != 0xFFFE) b->wfx.Format.cbSize = 0;
        b->frames = desc->dwBufferBytes / b->fmt.block;
        b->size = b->frames * b->fmt.block;
        b->data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, b->size + b->fmt.block);
        b->data_refs = HeapAlloc(GetProcessHeap(), 0, sizeof(LONG));
        if (!b->data || !b->data_refs) { HeapFree(GetProcessHeap(), 0, b); return DSERR_OUTOFMEMORY; }
        *b->data_refs = 1;
        b->freq = b->fmt.rate;
        if (b->fmt.kind == AC_U8) memset(b->data, 0x80, b->size);
    }
    dev_addref(d);
    if (!b->primary) {
        EnterCriticalSection(&d->lock);
        b->next = d->buffers;
        d->buffers = b;
        LeaveCriticalSection(&d->lock);
    }
    *out = b;
    return DS_OK;
}

static HRESULT new_device(void **out)
{
    Device *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return DSERR_OUTOFMEMORY;
    d->vtbl = &device_vtbl;
    d->refs = 1;
    InitializeCriticalSection(&d->lock);
    d->pfmt.Format = (AcWaveFormat){ 1, 2, 22050, 22050 * 2, 2, 8, 0 };    /* Windows' primary default */
    d->listener = (DS3DLISTENER){ sizeof(DS3DLISTENER), { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f, 1.0f };
    d->listener_sub.vtbl = &listener_vtbl;
    d->speaker = 0x00000004;                        /* DSSPEAKER_STEREO */
    *out = d;
    return DS_OK;
}

/* the primary buffer's listener sub-object needs a buffer whose dev is d */
static Buffer *listener_owner(Device *d)
{
    static Buffer stub;
    stub.dev = d;
    return &stub;
}

/* -----------------------------------------------------------------------
 * DirectSoundCapture
 * ----------------------------------------------------------------------- */
typedef struct Capture Capture;
typedef struct CBuffer CBuffer;
typedef struct { const void *vtbl; CBuffer *c; } CSub;

struct Capture { const void *vtbl; LONG refs; BOOL init; UINT32 devid; CBuffer *buffer; };

struct CBuffer {
    const void *vtbl;
    CSub        notify;
    LONG        refs;
    Capture    *cap;
    AudioCapConv conv;
    AcWaveFormatExt wfx;
    BYTE       *data;
    DWORD       size, frames, pos;      /* pos: frames recorded into data (where the next one goes) */
    BOOL        running, looping;
    INT_PTR     stream;
    HANDLE      thread, quit;
    CRITICAL_SECTION lock;
    DSBPOSITIONNOTIFY *notes;
    DWORD       nnotes;
    SHORT       tmp[CHUNK * 2];
};

static DWORD WINAPI capture_thread(LPVOID p)
{
    CBuffer *c = p;
    BYTE conv[CHUNK * 64];
    DWORD mm = 0;
    AvSetMmThreadCharacteristicsW(L"Capture", &mm);   /* (MMCSS: above any busy or boosted program thread) */
    do {
        EnterCriticalSection(&c->lock);
        while (c->running) {
            UINT room = sizeof(conv) / c->conv.f.block;
            UINT m = acc_src_for(&c->conv, room);
            if (m > CHUNK) m = CHUNK;
            LONG_PTR got = NtNovaAudioCtl(c->stream, 6, m, c->tmp);
            if (got <= 0) break;
            UINT k = acc_convert(&c->conv, c->tmp, (UINT)got, conv, room);
            DWORD from = c->pos * c->conv.f.block;
            for (UINT i = 0; i < k && c->running; i++) {
                memcpy(c->data + (size_t)c->pos * c->conv.f.block, conv + (size_t)i * c->conv.f.block, c->conv.f.block);
                if (++c->pos == c->frames) {
                    c->pos = 0;
                    if (!c->looping) {
                        c->running = FALSE;
                        NtNovaAudioCtl(c->stream, 1, 0, 0);
                    }
                }
            }
            DWORD to = c->pos * c->conv.f.block;
            for (DWORD i = 0; i < c->nnotes; i++) {
                DWORD o = c->notes[i].dwOffset;
                if (o == DSBPN_OFFSETSTOP) { if (!c->running) SetEvent(c->notes[i].hEventNotify); continue; }
                if (from == to) continue;
                if (from < to ? (o >= from && o < to) : (o >= from || o < to)) SetEvent(c->notes[i].hEventNotify);
            }
        }
        LeaveCriticalSection(&c->lock);
    } while (WaitForSingleObject(c->quit, TICK) == WAIT_TIMEOUT);
    return 0;
}

static ULONG STDMETHODCALLTYPE cap_release(Capture *c);
static HRESULT STDMETHODCALLTYPE cb_qi(CBuffer *c, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundCaptureBuffer) ||
           IsEqualIID(riid, &IID_IDirectSoundCaptureBuffer8) ? (void *)c :
           IsEqualIID(riid, &IID_IDirectSoundNotify) ? (void *)&c->notify : 0;
    if (!*ppv) return E_NOINTERFACE;
    InterlockedIncrement(&c->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE cb_addref(CBuffer *c) { return (ULONG)InterlockedIncrement(&c->refs); }
static ULONG STDMETHODCALLTYPE cb_release(CBuffer *c)
{
    LONG r = InterlockedDecrement(&c->refs);
    if (r) return (ULONG)r;
    SetEvent(c->quit);
    WaitForSingleObject(c->thread, INFINITE);
    CloseHandle(c->thread);
    CloseHandle(c->quit);
    NtClose((HANDLE)c->stream);
    DeleteCriticalSection(&c->lock);
    if (c->notes) HeapFree(GetProcessHeap(), 0, c->notes);
    HeapFree(GetProcessHeap(), 0, c->data);
    c->cap->buffer = 0;
    cap_release(c->cap);
    HeapFree(GetProcessHeap(), 0, c);
    return 0;
}
static HRESULT STDMETHODCALLTYPE cb_get_caps(CBuffer *c, DSCBCAPS *caps)
{
    if (!caps || caps->dwSize < sizeof(*caps)) return DSERR_INVALIDPARAM;
    caps->dwFlags = 0;
    caps->dwBufferBytes = c->size;
    caps->dwReserved = 0;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_get_pos(CBuffer *c, DWORD *capture, DWORD *read)
{
    EnterCriticalSection(&c->lock);
    DWORD p = c->pos * c->conv.f.block;
    LeaveCriticalSection(&c->lock);
    if (capture) *capture = p;              /* the data up to here is ready */
    if (read) *read = p;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_get_format(CBuffer *c, AcWaveFormat *f, DWORD n, DWORD *written)
{
    DWORD need = sizeof(AcWaveFormat) + c->wfx.Format.cbSize;
    if (written) *written = need;
    if (!f) return written ? DS_OK : DSERR_INVALIDPARAM;
    memcpy(f, &c->wfx, n < need ? n : need);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_get_status(CBuffer *c, DWORD *s)
{
    if (!s) return DSERR_INVALIDPARAM;
    *s = c->running ? DSCBSTATUS_CAPTURING | (c->looping ? DSCBSTATUS_LOOPING : 0) : 0;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_initialize(CBuffer *c, void *cap, const DSCBUFFERDESC *d)
{ (void)c; (void)cap; (void)d; return DSERR_ALREADYINITIALIZED; }
static HRESULT STDMETHODCALLTYPE cb_lock(CBuffer *c, DWORD off, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags)
{
    if (p1) *p1 = 0;
    if (n1) *n1 = 0;
    if (p2) *p2 = 0;
    if (n2) *n2 = 0;
    if (!p1 || !n1) return DSERR_INVALIDPARAM;
    if (flags & 1 /* DSCBLOCK_ENTIREBUFFER */) bytes = c->size;
    if (off >= c->size || !bytes || bytes > c->size) return DSERR_INVALIDPARAM;
    *p1 = c->data + off;
    if (off + bytes <= c->size) { *n1 = bytes; return DS_OK; }
    *n1 = c->size - off;
    if (p2) *p2 = c->data;
    if (n2) *n2 = bytes - *n1;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_start(CBuffer *c, DWORD flags)
{
    EnterCriticalSection(&c->lock);
    c->looping = (flags & DSCBSTART_LOOPING) != 0;
    if (!c->running) {
        NtNovaAudioCtl(c->stream, 2, 0, 0);
        NtNovaAudioCtl(c->stream, 1, 1, 0);
    }
    c->running = TRUE;
    LeaveCriticalSection(&c->lock);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_stop(CBuffer *c)
{
    EnterCriticalSection(&c->lock);
    if (c->running) {
        NtNovaAudioCtl(c->stream, 1, 0, 0);
        c->running = FALSE;
        for (DWORD i = 0; i < c->nnotes; i++)
            if (c->notes[i].dwOffset == DSBPN_OFFSETSTOP) SetEvent(c->notes[i].hEventNotify);
    }
    LeaveCriticalSection(&c->lock);
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cb_unlock(CBuffer *c, void *p1, DWORD n1, void *p2, DWORD n2)
{ (void)c; (void)p1; (void)n1; (void)p2; (void)n2; return DS_OK; }
static HRESULT STDMETHODCALLTYPE cb_object_in_path(CBuffer *c, REFGUID obj, DWORD i, REFGUID iid, void **out)
{ (void)c; (void)obj; (void)i; (void)iid; if (out) *out = 0; return DSERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE cb_fx_status(CBuffer *c, DWORD n, DWORD *res)
{ (void)c; for (DWORD i = 0; res && i < n; i++) res[i] = 0; return n ? DSERR_INVALIDPARAM : DS_OK; }
static const struct {
    void *qi, *addref, *release, *get_caps, *get_pos, *get_format, *get_status, *initialize, *lock, *start, *stop,
         *unlock, *object_in_path, *fx_status;
} cbuffer_vtbl = {
    cb_qi, cb_addref, cb_release, cb_get_caps, cb_get_pos, cb_get_format, cb_get_status, cb_initialize, cb_lock,
    cb_start, cb_stop, cb_unlock, cb_object_in_path, cb_fx_status,
};

static HRESULT STDMETHODCALLTYPE cn_qi(CSub *s, REFIID riid, void **ppv) { return cb_qi(s->c, riid, ppv); }
static ULONG STDMETHODCALLTYPE cn_addref(CSub *s) { return cb_addref(s->c); }
static ULONG STDMETHODCALLTYPE cn_release(CSub *s) { return cb_release(s->c); }
static HRESULT STDMETHODCALLTYPE cn_set(CSub *s, DWORD n, const DSBPOSITIONNOTIFY *p)
{
    CBuffer *c = s->c;
    if (n && !p) return DSERR_INVALIDPARAM;
    for (DWORD i = 0; i < n; i++)
        if (p[i].dwOffset != DSBPN_OFFSETSTOP && p[i].dwOffset >= c->size) return DSERR_INVALIDPARAM;
    if (c->running) return DSERR_INVALIDCALL;
    DSBPOSITIONNOTIFY *copy = n ? HeapAlloc(GetProcessHeap(), 0, n * sizeof(*p)) : 0;
    if (n && !copy) return DSERR_OUTOFMEMORY;
    if (n) memcpy(copy, p, n * sizeof(*p));
    EnterCriticalSection(&c->lock);
    if (c->notes) HeapFree(GetProcessHeap(), 0, c->notes);
    c->notes = copy;
    c->nnotes = n;
    LeaveCriticalSection(&c->lock);
    return DS_OK;
}
static const struct { void *qi, *addref, *release, *set; } cnotify_vtbl = { cn_qi, cn_addref, cn_release, cn_set };

static HRESULT STDMETHODCALLTYPE cap_qi(Capture *c, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundCapture)) {
        *ppv = c;
        InterlockedIncrement(&c->refs);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cap_addref(Capture *c) { return (ULONG)InterlockedIncrement(&c->refs); }
static ULONG STDMETHODCALLTYPE cap_release(Capture *c)
{
    LONG r = InterlockedDecrement(&c->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, c);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE cap_create_buffer(Capture *cap, const DSCBUFFERDESC *desc, void **out, IUnknown *outer)
{
    if (!out) return DSERR_INVALIDPARAM;
    *out = 0;
    if (outer) return DSERR_NOAGGREGATION;
    if (!cap->init) return DSERR_UNINITIALIZED;
    if (!desc || desc->dwSize < 20 || !desc->lpwfxFormat || !desc->dwBufferBytes) return DSERR_INVALIDPARAM;
    if (cap->buffer) return DSERR_ALLOCATED;                      /* one buffer per object, as on Windows */
    CBuffer *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) return DSERR_OUTOFMEMORY;
    const AcWaveFormat *f = desc->lpwfxFormat;
    if (!acc_init(&c->conv, f) || f->nBlockAlign != c->conv.f.block) { HeapFree(GetProcessHeap(), 0, c); return DSERR_BADFORMAT; }
    memcpy(&c->wfx, f, sizeof(AcWaveFormat) + (f->wFormatTag == 0xFFFE ? 22 : 0));
    if (f->wFormatTag != 0xFFFE) c->wfx.Format.cbSize = 0;
    c->frames = desc->dwBufferBytes / c->conv.f.block;
    c->size = c->frames * c->conv.f.block;
    c->data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, c->size);
    c->stream = c->data ? NtNovaAudioOpen(0x80000000u | AC_RATE) : 0;
    if (c->stream && cap->devid && !audio_route(c->stream, cap->devid)) {   /* (unplugged meanwhile) */
        NtClose((HANDLE)c->stream);
        c->stream = 0;
    }
    if (!c->stream) {
        if (c->data) HeapFree(GetProcessHeap(), 0, c->data);
        HeapFree(GetProcessHeap(), 0, c);
        return c->data ? DSERR_ALLOCATED : DSERR_OUTOFMEMORY;
    }
    c->vtbl = &cbuffer_vtbl;
    c->notify = (CSub){ &cnotify_vtbl, c };
    c->refs = 1;
    c->cap = cap;
    InitializeCriticalSection(&c->lock);
    c->quit = CreateEventW(0, TRUE, FALSE, 0);
    c->thread = CreateThread(0, 64 * 1024, capture_thread, c, 0, 0);
    if (c->thread) SetThreadPriority(c->thread, 15);
    cap_addref(cap);
    cap->buffer = c;
    *out = c;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cap_get_caps(Capture *c, DSCCAPS *caps)
{
    if (!caps || caps->dwSize < sizeof(*caps)) return DSERR_INVALIDPARAM;
    if (!c->init) return DSERR_UNINITIALIZED;
    caps->dwFlags = 0;
    caps->dwFormats = 0x000FFFFF;
    caps->dwChannels = 2;
    return DS_OK;
}
static HRESULT STDMETHODCALLTYPE cap_initialize(Capture *c, const GUID *dev)
{
    if (c->init) return DSERR_ALREADYINITIALIZED;
    if (!find_device(dev, TRUE, &c->devid)) return DSERR_NODRIVER;
    c->init = TRUE;
    return DS_OK;
}
static const struct { void *qi, *addref, *release, *create_buffer, *get_caps, *initialize; } capture_vtbl = {
    cap_qi, cap_addref, cap_release, cap_create_buffer, cap_get_caps, cap_initialize,
};

static HRESULT new_capture(void **out)
{
    Capture *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) return DSERR_OUTOFMEMORY;
    c->vtbl = &capture_vtbl;
    c->refs = 1;
    *out = c;
    return DS_OK;
}

/* -----------------------------------------------------------------------
 * Exports
 * ----------------------------------------------------------------------- */
static HRESULT create_device(const GUID *dev, void **out, IUnknown *outer)
{
    if (!out) return DSERR_INVALIDPARAM;
    *out = 0;
    if (outer) return DSERR_NOAGGREGATION;
    void *d;
    HRESULT hr = new_device(&d);
    if (FAILED(hr)) return hr;
    hr = dev_initialize(d, dev);
    if (FAILED(hr)) { dev_release(d); return hr; }
    ((Device *)d)->listener_sub.b = listener_owner(d);
    *out = d;
    return DS_OK;
}

EXPORT HRESULT WINAPI DirectSoundCreate(const GUID *dev, void **out, IUnknown *outer) { return create_device(dev, out, outer); }
EXPORT HRESULT WINAPI DirectSoundCreate8(const GUID *dev, void **out, IUnknown *outer) { return create_device(dev, out, outer); }

static HRESULT create_capture(const GUID *dev, void **out, IUnknown *outer)
{
    if (!out) return DSERR_INVALIDPARAM;
    *out = 0;
    if (outer) return DSERR_NOAGGREGATION;
    void *c;
    HRESULT hr = new_capture(&c);
    if (FAILED(hr)) return hr;
    hr = cap_initialize(c, dev);
    if (FAILED(hr)) { cap_release(c); return hr; }
    *out = c;
    return DS_OK;
}
EXPORT HRESULT WINAPI DirectSoundCaptureCreate(const GUID *dev, void **out, IUnknown *outer) { return create_capture(dev, out, outer); }
EXPORT HRESULT WINAPI DirectSoundCaptureCreate8(const GUID *dev, void **out, IUnknown *outer) { return create_capture(dev, out, outer); }

EXPORT HRESULT WINAPI DirectSoundFullDuplexCreate(const GUID *cdev, const GUID *rdev, const DSCBUFFERDESC *cd,
                                                  const DSBUFFERDESC *rd, HWND w, DWORD level, void **fd, void **cb,
                                                  void **rb, IUnknown *outer)
{
    (void)cdev; (void)rdev; (void)cd; (void)rd; (void)w; (void)level; (void)outer;
    if (fd) *fd = 0;
    if (cb) *cb = 0;
    if (rb) *rb = 0;
    return DSERR_UNSUPPORTED;
}

/* Enumeration: the "Primary Sound Driver" (NULL GUID: the default), then
 * each device, oldest first, as Windows lists them: its endpoint's GUID,
 * its name ("Speakers (Product)") and its endpoint ID as the module */
typedef BOOL (CALLBACK *ENUMA)(GUID *, LPCSTR, LPCSTR, void *);
typedef BOOL (CALLBACK *ENUMW)(GUID *, LPCWSTR, LPCWSTR, void *);
static HRESULT enumerate(BOOL capture, ENUMA cba, ENUMW cbw, void *ctx)
{
    if (!cba && !cbw) return DSERR_INVALIDPARAM;
    AudioDeviceList l;
    UINT n = audio_devices(capture, &l);
    if (!n) return DS_OK;
    const WCHAR *primary = capture ? L"Primary Sound Capture Driver" : L"Primary Sound Driver";
    char a1[96], a2[64];
    WideCharToMultiByte(CP_ACP, 0, primary, -1, a1, sizeof(a1), 0, 0);
    if (cbw ? !cbw(0, primary, L"", ctx) : !cba(0, a1, "", ctx)) return DS_OK;
    for (UINT i = 0; i < n; i++) {
        GUID g = audio_device_guid(l.dev[i].id);
        WCHAR name[96], module[56];
        audio_friendly_name(capture, l.dev[i].name, name, 96);
        audio_endpoint_id(capture, l.dev[i].id, module);
        WideCharToMultiByte(CP_ACP, 0, name, -1, a1, sizeof(a1), 0, 0);
        WideCharToMultiByte(CP_ACP, 0, module, -1, a2, sizeof(a2), 0, 0);
        if (cbw ? !cbw(&g, name, module, ctx) : !cba(&g, a1, a2, ctx)) break;
    }
    return DS_OK;
}
EXPORT HRESULT WINAPI DirectSoundEnumerateA(ENUMA cb, void *ctx) { return cb ? enumerate(FALSE, cb, 0, ctx) : DSERR_INVALIDPARAM; }
EXPORT HRESULT WINAPI DirectSoundEnumerateW(ENUMW cb, void *ctx) { return cb ? enumerate(FALSE, 0, cb, ctx) : DSERR_INVALIDPARAM; }
EXPORT HRESULT WINAPI DirectSoundCaptureEnumerateA(ENUMA cb, void *ctx) { return cb ? enumerate(TRUE, cb, 0, ctx) : DSERR_INVALIDPARAM; }
EXPORT HRESULT WINAPI DirectSoundCaptureEnumerateW(ENUMW cb, void *ctx) { return cb ? enumerate(TRUE, 0, cb, ctx) : DSERR_INVALIDPARAM; }

/* The GUID of the device a default GUID stands for now (others as they are) */
EXPORT HRESULT WINAPI GetDeviceID(const GUID *src, GUID *dst)
{
    if (!src || !dst) return DSERR_INVALIDPARAM;
    int capture = IsEqualGUID(src, &DSDEVID_DefaultCapture) || IsEqualGUID(src, &DSDEVID_DefaultVoiceCapture);
    if (capture || IsEqualGUID(src, &DSDEVID_DefaultPlayback) || IsEqualGUID(src, &DSDEVID_DefaultVoicePlayback)) {
        AudioDeviceList l;
        UINT n = audio_devices(capture, &l);
        for (UINT i = 0; i < n; i++)
            if (l.dev[i].is_default) { *dst = audio_device_guid(l.dev[i].id); return DS_OK; }
        return DSERR_NODRIVER;
    }
    *dst = *src;
    return DS_OK;
}

/* Class factories: CoCreateInstance makes an uninitialized object
 * (the program calls Initialize) */
typedef struct { const IClassFactoryVtbl *vtbl; BOOL capture; } Factory;
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
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION;
    void *o;
    BOOL cap = ((Factory *)cf)->capture;
    HRESULT hr = cap ? new_capture(&o) : new_device(&o);
    if (FAILED(hr)) return hr;
    if (!cap) ((Device *)o)->listener_sub.b = listener_owner(o);
    hr = cap ? cap_qi(o, riid, ppv) : dev_qi(o, riid, ppv);
    if (cap) cap_release(o); else dev_release(o);
    return hr;
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *cf, BOOL lock) { (void)cf; (void)lock; return S_OK; }
static const IClassFactoryVtbl cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_render_factory = { &cf_vtbl, FALSE }, g_capture_factory = { &cf_vtbl, TRUE };

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (IsEqualCLSID(clsid, &CLSID_DirectSound) || IsEqualCLSID(clsid, &CLSID_DirectSound8))
        return cf_qi((IClassFactory *)&g_render_factory, riid, ppv);
    if (IsEqualCLSID(clsid, &CLSID_DirectSoundCapture) || IsEqualCLSID(clsid, &CLSID_DirectSoundCapture8))
        return cf_qi((IClassFactory *)&g_capture_factory, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
