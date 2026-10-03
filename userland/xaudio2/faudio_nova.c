/*
 * faudio_nova.c — FAudio's platform layer on NovaOS
 *
 * FAudio (third_party/faudio) is the XAudio2 engine; this file is the part
 * it leaves to the platform: the output device, threads, locks and time.
 * The engine mixes 32-bit float at 48 kHz in the mastering voice's channel
 * count; a thread keeps one kernel mixer stream about three engine passes
 * ahead, folding surround channels into stereo on the way.
 *
 * The devices (GetDeviceCount/GetDeviceDetails in XAudio2 2.7, and the
 * IDs CreateMasteringVoice takes in 2.8 and 2.9) are NovaOS's sound
 * outputs (audiodev.h): index 0 is the default (FAudio's convention, which
 * follows Settings' choice), then each output oldest first with its
 * Windows name and its WASAPI endpoint ID.
 */

#include <windows.h>
#include <winternl.h>
#include <stdlib.h>
#include <string.h>
#include "FAudio_internal.h"
#include "../winmm/audiodev.h"

#define RATE 48000

int _fltused = 1;

typedef struct {
    FAudio *audio;
    INT_PTR stream;
    HANDLE  thread, quit;
    UINT32  channels, quantum;          /* frames per engine pass */
    DWORD   mask;
    float  *mix;
    SHORT  *out;
} NovaDevice;

typedef struct {
    ULONGLONG written, consumed, played;
    UINT32 queued, capacity, running, latency;
} StreamStatus;

void FAudio_Log(char const *msg) { OutputDebugStringA(msg); }

static SHORT s16(float v)
{
    v *= 32768.0f;
    return (SHORT)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)v);
}

/* Fold @n frames of @ch-channel float (speaker order of @mask) into s16
 * stereo: centre and the surrounds at -3 dB on their sides, LFE left out */
static void fold(const float *in, SHORT *out, UINT32 n, UINT32 ch, DWORD mask)
{
    float gl[8], gr[8];
    UINT32 c = 0;
    for (DWORD bit = 1; bit && c < ch; bit <<= 1) {
        if (!(mask & bit)) continue;
        float l = 0, r = 0;
        switch (bit) {
        case SPEAKER_FRONT_LEFT:   l = 1; break;
        case SPEAKER_FRONT_RIGHT:  r = 1; break;
        case SPEAKER_FRONT_CENTER: l = r = 0.7071f; break;
        case SPEAKER_LOW_FREQUENCY: break;
        case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: l = 0.7071f; break;
        case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: r = 0.7071f; break;
        default: l = r = 0.5f; break;
        }
        gl[c] = l;
        gr[c] = r;
        c++;
    }
    for (; c < ch && c < 8; c++) gl[c] = gr[c] = 0;
    if (ch == 1) gl[0] = gr[0] = 1;
    for (UINT32 i = 0; i < n; i++) {
        const float *f = in + (size_t)i * ch;
        float l = 0, r = 0;
        for (UINT32 k = 0; k < ch && k < 8; k++) { l += f[k] * gl[k]; r += f[k] * gr[k]; }
        out[i * 2] = s16(l);
        out[i * 2 + 1] = s16(r);
    }
}

static DWORD WINAPI mixer_thread(LPVOID p)
{
    NovaDevice *d = p;
    SetThreadPriority(GetCurrentThread(), 15);      /* THREAD_PRIORITY_TIME_CRITICAL */
    do {
        for (;;) {
            StreamStatus st;
            if (NtNovaAudioCtl(d->stream, 0, 0, &st) || st.queued >= d->quantum * 3) break;
            memset(d->mix, 0, (size_t)d->quantum * d->channels * sizeof(float));
            if (d->audio->active) FAudio_INTERNAL_UpdateEngine(d->audio, d->mix);
            fold(d->mix, d->out, d->quantum, d->channels, d->mask);
            UINT32 done = 0;
            while (done < d->quantum) {
                LONG_PTR got = NtNovaAudioWrite(d->stream, d->out + done * 2, d->quantum - done);
                if (got <= 0) break;
                done += (UINT32)got;
            }
            if (done < d->quantum) break;
        }
    } while (WaitForSingleObject(d->quit, 5) == WAIT_TIMEOUT);
    return 0;
}

void FAudio_PlatformAddRef(void) {}
void FAudio_PlatformRelease(void) {}

/* The kernel's id of device @index (0: the default, which follows
 * Settings), in *id; FALSE if there is no such device */
static BOOL device_at(uint32_t index, UINT32 *id)
{
    AudioDeviceList l;
    UINT n = audio_devices(0, &l);
    *id = 0;
    if (!n || index > n) return FALSE;
    if (index) *id = l.dev[index - 1].id;
    return TRUE;
}

/* The device index of a WASAPI endpoint ID (XAudio2 2.8's
 * CreateMasteringVoice); 0 (the default) for NULL or ""; -1 if it names no
 * output attached now */
int nova_device_index(const WCHAR *endpoint)
{
    if (!endpoint || !endpoint[0]) return 0;
    int capture = 0;
    UINT32 id = audio_endpoint_parse(endpoint, &capture);
    AudioDeviceList l;
    UINT n = audio_devices(0, &l);
    for (UINT i = 0; id && !capture && i < n; i++)
        if (l.dev[i].id == id) return (int)i + 1;
    return -1;
}

void FAudio_PlatformInit(FAudio *audio, uint32_t flags, uint32_t deviceIndex, FAudioWaveFormatExtensible *mixFormat,
                         uint32_t *updateSize, void **platformDevice)
{
    *platformDevice = NULL;
    FAudio_INTERNAL_InitSIMDFunctions(1, 0);
    UINT32 devid;
    if (!device_at(deviceIndex, &devid)) return;
    UINT32 ch = mixFormat->Format.nChannels ? mixFormat->Format.nChannels : 2;
    if (ch > 8) return;
    /* the engine resamples every voice to the device's rate */
    WriteWaveFormatExtensible(mixFormat, ch, RATE, &DATAFORMAT_SUBTYPE_IEEE_FLOAT);
    NovaDevice *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->audio = audio;
    d->channels = ch;
    d->mask = mixFormat->dwChannelMask;
    d->quantum = (flags & FAUDIO_1024_QUANTUM) ? RATE * 64 / 3 / 1000 : RATE / 100;
    d->mix = malloc((size_t)d->quantum * ch * sizeof(float));
    d->out = malloc((size_t)d->quantum * 2 * sizeof(SHORT));
    d->stream = NtNovaAudioOpen(d->quantum * 8);
    if (!d->mix || !d->out || !d->stream) {
        if (d->stream) NtClose((HANDLE)d->stream);
        free(d->mix);
        free(d->out);
        free(d);
        return;
    }
    if (devid && !audio_route(d->stream, devid)) {  /* (unplugged meanwhile) */
        NtClose((HANDLE)d->stream);
        free(d->mix);
        free(d->out);
        free(d);
        return;
    }
    NtNovaAudioCtl(d->stream, 1, 1, 0);             /* run (streams open paused) */
    d->quit = CreateEventW(0, TRUE, FALSE, 0);
    d->thread = CreateThread(0, 256 * 1024, mixer_thread, d, 0, 0);
    *updateSize = d->quantum;
    *platformDevice = d;
}

void FAudio_PlatformQuit(void *platformDevice)
{
    NovaDevice *d = platformDevice;
    if (!d) return;
    SetEvent(d->quit);
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
    CloseHandle(d->quit);
    NtClose((HANDLE)d->stream);
    free(d->mix);
    free(d->out);
    free(d);
}

uint32_t FAudio_PlatformGetDeviceCount(void)
{
    AudioDeviceList l;
    UINT n = audio_devices(0, &l);
    return n ? n + 1 : 0;
}

/* Index 0: the default device (its name and ID, the default role); then
 * every output */
uint32_t FAudio_PlatformGetDeviceDetails(uint32_t index, FAudioDeviceDetails *details)
{
    memset(details, 0, sizeof(*details));
    AudioDeviceList l;
    UINT n = audio_devices(0, &l), k = n;
    if (!n || index > n) return FAUDIO_E_INVALID_CALL;
    for (UINT i = 0; i < n; i++)
        if (index ? i == index - 1 : l.dev[i].is_default) k = i;
    if (k == n) return FAUDIO_E_INVALID_CALL;
    WCHAR id[56], name[96];
    audio_endpoint_id(0, l.dev[k].id, id);
    audio_friendly_name(0, l.dev[k].name, name, 96);
    memcpy(details->DeviceID, id, sizeof(id));
    lstrcpynW((WCHAR *)details->DisplayName, name, sizeof(details->DisplayName) / sizeof(details->DisplayName[0]));
    details->Role = !index ? FAudioGlobalDefaultDevice : FAudioNotDefaultDevice;
    WriteWaveFormatExtensible(&details->OutputFormat, 2, RATE, &DATAFORMAT_SUBTYPE_IEEE_FLOAT);
    return 0;
}

/* Threads and locks */
FAudioMutex FAudio_PlatformCreateMutex(void)
{
    CRITICAL_SECTION *cs = malloc(sizeof(*cs));
    if (cs) InitializeCriticalSection(cs);
    return cs;
}
void FAudio_PlatformDestroyMutex(FAudioMutex m) { if (m) { DeleteCriticalSection(m); free(m); } }
void FAudio_PlatformLockMutex(FAudioMutex m) { if (m) EnterCriticalSection(m); }
void FAudio_PlatformUnlockMutex(FAudioMutex m) { if (m) LeaveCriticalSection(m); }

typedef struct { FAudioThreadFunc func; void *data; } ThreadStart;
static DWORD WINAPI thread_start(LPVOID p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    return (DWORD)s.func(s.data);
}
FAudioThread FAudio_PlatformCreateThread(FAudioThreadFunc func, const char *name, void *data)
{
    (void)name;
    ThreadStart *s = malloc(sizeof(*s));
    if (!s) return NULL;
    s->func = func;
    s->data = data;
    HANDLE t = CreateThread(0, 0, thread_start, s, 0, 0);
    if (!t) free(s);
    return t;
}
void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
    WaitForSingleObject(thread, INFINITE);
    if (retval) GetExitCodeThread(thread, (DWORD *)retval);
    CloseHandle(thread);
}
void FAudio_PlatformThreadPriority(FAudioThreadPriority priority) { (void)priority; }
uint64_t FAudio_PlatformGetThreadID(void) { return GetCurrentThreadId(); }
void FAudio_sleep(uint32_t ms) { Sleep(ms); }
uint32_t FAudio_timems(void) { return GetTickCount(); }

/* In-memory streams (the effects and F3DAudio do not use files) */
typedef struct { uint8_t *mem; int64_t len, pos; } MemStream;
static size_t FAUDIOCALL mem_read(void *data, void *dst, size_t size, size_t count)
{
    MemStream *m = data;
    int64_t want = (int64_t)(size * count), left = m->len - m->pos;
    if (want > left) want = left;
    memcpy(dst, m->mem + m->pos, (size_t)want);
    m->pos += want;
    return size ? (size_t)want / size : 0;
}
static int64_t FAUDIOCALL mem_seek(void *data, int64_t offset, int whence)
{
    MemStream *m = data;
    int64_t p = whence == FAUDIO_SEEK_SET ? offset : whence == FAUDIO_SEEK_CUR ? m->pos + offset : m->len + offset;
    m->pos = p < 0 ? 0 : p > m->len ? m->len : p;
    return m->pos;
}
static int FAUDIOCALL mem_close(void *data) { free(data); return 0; }
FAudioIOStream *FAudio_memopen(void *mem, int len)
{
    FAudioIOStream *io = malloc(sizeof(*io));
    MemStream *m = malloc(sizeof(*m));
    if (!io || !m) { free(io); free(m); return NULL; }
    *m = (MemStream){ mem, len, 0 };
    io->data = m;
    io->read = mem_read;
    io->seek = mem_seek;
    io->close = mem_close;
    io->lock = FAudio_PlatformCreateMutex();
    return io;
}
uint8_t *FAudio_memptr(FAudioIOStream *io, size_t offset) { return ((MemStream *)io->data)->mem + offset; }
void FAudio_close(FAudioIOStream *io)
{
    io->close(io->data);
    FAudio_PlatformDestroyMutex(io->lock);
    free(io);
}
FAudioIOStream *FAudio_fopen(const char *path) { (void)path; return NULL; }
