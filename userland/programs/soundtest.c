/*
 * soundtest — exercise NovaOS's sound output
 *
 *   soundtest info              the playback device (waveOutGetDevCaps)
 *   soundtest tone [HZ] [MS]    a sine through waveOut, 22.05 kHz mono 16-bit
 *                               (the converter's resampling path), 4 buffers
 *                               cycled through CALLBACK_EVENT
 *   soundtest float [HZ] [MS]   the same as 48 kHz stereo 32-bit float
 *   soundtest play FILE         PlaySound(FILE, SND_FILENAME | SND_SYNC)
 *   soundtest ding              PlaySound(SystemAsterisk) (the default sound)
 *   soundtest msgbeep           MessageBeep(MB_ICONASTERISK) (asynchronous)
 *   soundtest wasapi [HZ] [MS]  a sine through IAudioClient/IAudioRenderClient
 *   soundtest beep [HZ] [MS]    kernel32 Beep
 *   soundtest both [HZ] [MS]    WASAPI at HZ and waveOut at 1.5 x HZ at once
 *                               (the kernel mixer adds them)
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <objbase.h>

typedef UINT MMRESULT;
typedef struct wavehdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength, dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags, dwLoops;
    struct wavehdr_tag *lpNext;
    DWORD_PTR reserved;
} WAVEHDR;
typedef struct {
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;
typedef struct {
    WORD wMid, wPid;
    UINT vDriverVersion;
    WCHAR szPname[32];
    DWORD dwFormats;
    WORD wChannels, wReserved1;
    DWORD dwSupport;
} WAVEOUTCAPSW;
typedef struct { UINT wType; union { DWORD ms, sample, cb; } u; } MMTIME;

__declspec(dllimport) UINT WINAPI waveOutGetNumDevs(void);
__declspec(dllimport) MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR, WAVEOUTCAPSW *, UINT);
__declspec(dllimport) MMRESULT WINAPI waveOutOpen(HANDLE *, UINT, const WAVEFORMATEX *, DWORD_PTR, DWORD_PTR, DWORD);
__declspec(dllimport) MMRESULT WINAPI waveOutPrepareHeader(HANDLE, WAVEHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI waveOutUnprepareHeader(HANDLE, WAVEHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI waveOutWrite(HANDLE, WAVEHDR *, UINT);
__declspec(dllimport) MMRESULT WINAPI waveOutClose(HANDLE);
__declspec(dllimport) MMRESULT WINAPI waveOutGetPosition(HANDLE, MMTIME *, UINT);
__declspec(dllimport) BOOL WINAPI PlaySoundW(LPCWSTR, HMODULE, DWORD);
__declspec(dllimport) BOOL WINAPI Beep(DWORD, DWORD);

#define CALLBACK_EVENT 0x00050000
#define WHDR_DONE      1
#define TIME_SAMPLES   2
#define PI 3.14159265358979

static int info(void)
{
    UINT n = waveOutGetNumDevs();
    printf("waveOut devices: %u\n", n);
    for (UINT i = 0; i < n; i++) {
        WAVEOUTCAPSW c;
        MMRESULT r = waveOutGetDevCapsW(i, &c, sizeof(c));
        printf("  %u: r=%u \"%ls\" channels=%u formats=%05lx support=%lx\n", i, r, c.szPname, c.wChannels,
               c.dwFormats, c.dwSupport);
    }
    return n ? 0 : 1;
}

/* A sine of @hz for @ms through waveOut, in 16-bit mono at 22050 Hz or
 * float stereo at 48 kHz */
static int tone(double hz, DWORD ms, BOOL flt)
{
    WAVEFORMATEX f = { flt ? 3 : 1, flt ? 2 : 1, flt ? 48000 : 22050, 0, 0, flt ? 32 : 16, 0 };
    f.nBlockAlign = f.nChannels * f.wBitsPerSample / 8;
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    HANDLE ev = CreateEventW(0, FALSE, FALSE, 0), wo;
    MMRESULT r = waveOutOpen(&wo, (UINT)-1, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r) { printf("waveOutOpen: %u\n", r); return 1; }
    WaitForSingleObject(ev, 0);                     /* WOM_OPEN */

    DWORD total = f.nSamplesPerSec * ms / 1000, per = f.nSamplesPerSec / 10, made = 0;
    WAVEHDR h[4];
    memset(h, 0, sizeof(h));
    for (int i = 0; i < 4; i++) {
        h[i].lpData = malloc(per * f.nBlockAlign);
        h[i].dwFlags = WHDR_DONE;
    }
    int buffers = 0;
    DWORD t0 = GetTickCount();
    while (made < total) {
        for (int i = 0; i < 4 && made < total; i++) {
            if (!(h[i].dwFlags & WHDR_DONE)) continue;
            if (h[i].dwFlags & 2) waveOutUnprepareHeader(wo, &h[i], sizeof(h[i]));
            DWORD n = total - made < per ? total - made : per;
            for (DWORD k = 0; k < n; k++) {
                double v = 0.5 * sin(2 * PI * hz * (made + k) / f.nSamplesPerSec);
                if (flt) { ((float *)h[i].lpData)[k * 2] = (float)v; ((float *)h[i].lpData)[k * 2 + 1] = (float)v; }
                else ((short *)h[i].lpData)[k] = (short)(v * 32767);
            }
            h[i].dwBufferLength = n * f.nBlockAlign;
            h[i].dwFlags = 0;
            waveOutPrepareHeader(wo, &h[i], sizeof(h[i]));
            r = waveOutWrite(wo, &h[i], sizeof(h[i]));
            if (r) { printf("waveOutWrite: %u\n", r); return 1; }
            made += n;
            buffers++;
        }
        WaitForSingleObject(ev, 1000);
    }
    for (int i = 0; i < 4; i++)
        while (!(h[i].dwFlags & WHDR_DONE)) WaitForSingleObject(ev, 1000);
    DWORD elapsed = GetTickCount() - t0;
    MMTIME t = { TIME_SAMPLES };
    waveOutGetPosition(wo, &t, sizeof(t));
    for (int i = 0; i < 4; i++) { waveOutUnprepareHeader(wo, &h[i], sizeof(h[i])); free(h[i].lpData); }
    r = waveOutClose(wo);
    printf("played %lu samples in %d buffers in %lu ms; position %lu; close %u\n", total, buffers, elapsed,
           t.u.sample, r);
    return 0;
}

/* -----------------------------------------------------------------------
 * WASAPI (declared here: the userland headers have no audioclient.h)
 * ----------------------------------------------------------------------- */
static const GUID CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID IID_IMMDeviceEnumerator  = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID IID_IAudioClient         = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID IID_IAudioRenderClient   = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

typedef struct IUnk { struct { void *qi, *addref; ULONG (STDMETHODCALLTYPE *Release)(void *); } *v; } IUnk;
typedef struct {
    void *qi, *addref;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *EnumAudioEndpoints)(void *, int, DWORD, void **);
    HRESULT (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(void *, int, int, void **);
} EnumVtbl;
typedef struct {
    void *qi, *addref;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *Activate)(void *, const GUID *, DWORD, void *, void **);
} DeviceVtbl;
typedef struct {
    void *qi, *addref;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *Initialize)(void *, int, DWORD, LONGLONG, LONGLONG, const WAVEFORMATEX *, const GUID *);
    HRESULT (STDMETHODCALLTYPE *GetBufferSize)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *GetStreamLatency)(void *, LONGLONG *);
    HRESULT (STDMETHODCALLTYPE *GetCurrentPadding)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *IsFormatSupported)(void *, int, const WAVEFORMATEX *, WAVEFORMATEX **);
    HRESULT (STDMETHODCALLTYPE *GetMixFormat)(void *, WAVEFORMATEX **);
    HRESULT (STDMETHODCALLTYPE *GetDevicePeriod)(void *, LONGLONG *, LONGLONG *);
    HRESULT (STDMETHODCALLTYPE *Start)(void *);
    HRESULT (STDMETHODCALLTYPE *Stop)(void *);
    HRESULT (STDMETHODCALLTYPE *Reset)(void *);
    HRESULT (STDMETHODCALLTYPE *SetEventHandle)(void *, HANDLE);
    HRESULT (STDMETHODCALLTYPE *GetService)(void *, const GUID *, void **);
} ClientVtbl;
typedef struct {
    void *qi, *addref;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(void *, UINT32, BYTE **);
    HRESULT (STDMETHODCALLTYPE *ReleaseBuffer)(void *, UINT32, DWORD);
} RenderVtbl;
#define CALL(obj, T, m, ...) ((*(T **)(obj))->m((obj), ##__VA_ARGS__))

static int wasapi(double hz, DWORD ms)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    void *en = 0, *dev = 0, *ac = 0, *rc = 0;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, 0, CLSCTX_ALL, &IID_IMMDeviceEnumerator, &en);
    if (FAILED(hr)) { printf("CoCreateInstance(MMDeviceEnumerator): %08lx\n", hr); return 1; }
    hr = CALL(en, EnumVtbl, GetDefaultAudioEndpoint, 0 /* eRender */, 0 /* eConsole */, &dev);
    if (FAILED(hr)) { printf("GetDefaultAudioEndpoint: %08lx\n", hr); return 1; }
    hr = CALL(dev, DeviceVtbl, Activate, &IID_IAudioClient, CLSCTX_ALL, 0, &ac);
    if (FAILED(hr)) { printf("Activate(IAudioClient): %08lx\n", hr); return 1; }
    WAVEFORMATEX *mix;
    CALL(ac, ClientVtbl, GetMixFormat, &mix);
    printf("mix format: tag %04x, %u channels, %lu Hz, %u bits\n", mix->wFormatTag, mix->nChannels,
           mix->nSamplesPerSec, mix->wBitsPerSample);
    HANDLE ev = CreateEventW(0, FALSE, FALSE, 0);
    hr = CALL(ac, ClientVtbl, Initialize, 0 /* shared */, 0x00040000 /* EVENTCALLBACK */, 200000, 0, mix, 0);
    if (FAILED(hr)) { printf("Initialize: %08lx\n", hr); return 1; }
    CALL(ac, ClientVtbl, SetEventHandle, ev);
    UINT32 size;
    CALL(ac, ClientVtbl, GetBufferSize, &size);
    hr = CALL(ac, ClientVtbl, GetService, &IID_IAudioRenderClient, &rc);
    if (FAILED(hr)) { printf("GetService(IAudioRenderClient): %08lx\n", hr); return 1; }
    UINT32 total = mix->nSamplesPerSec * ms / 1000, made = 0;
    DWORD t0 = GetTickCount();
    BOOL started = FALSE;
    while (made < total) {
        UINT32 pad;
        CALL(ac, ClientVtbl, GetCurrentPadding, &pad);
        UINT32 n = size - pad;
        if (n > total - made) n = total - made;
        if (n) {
            BYTE *buf;
            hr = CALL(rc, RenderVtbl, GetBuffer, n, &buf);
            if (FAILED(hr)) { printf("GetBuffer: %08lx\n", hr); return 1; }
            float *f = (float *)buf;
            for (UINT32 k = 0; k < n; k++)
                for (UINT c = 0; c < mix->nChannels; c++)
                    f[k * mix->nChannels + c] = (float)(0.5 * sin(2 * PI * hz * (made + k) / mix->nSamplesPerSec));
            CALL(rc, RenderVtbl, ReleaseBuffer, n, 0);
            made += n;
        }
        if (!started) { CALL(ac, ClientVtbl, Start); started = TRUE; }
        WaitForSingleObject(ev, 1000);
    }
    for (;;) {                                      /* let it drain */
        UINT32 pad;
        CALL(ac, ClientVtbl, GetCurrentPadding, &pad);
        if (!pad) break;
        WaitForSingleObject(ev, 1000);
    }
    Sleep(100);
    CALL(ac, ClientVtbl, Stop);
    printf("buffer %u frames; played %u frames in %lu ms\n", size, total, GetTickCount() - t0);
    CoTaskMemFree(mix);
    ((IUnk *)rc)->v->Release(rc);
    ((IUnk *)ac)->v->Release(ac);
    ((IUnk *)dev)->v->Release(dev);
    ((IUnk *)en)->v->Release(en);
    return 0;
}

static double g_hz;
static DWORD g_ms;
static DWORD WINAPI wasapi_thread(LPVOID p) { (void)p; return (DWORD)wasapi(g_hz, g_ms); }

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";
    double hz = argc > 2 ? atof(argv[2]) : 440;
    DWORD ms = argc > 3 ? (DWORD)atoi(argv[3]) : 1000;
    if (!strcmp(cmd, "info")) return info();
    if (!strcmp(cmd, "tone")) return tone(hz, ms, FALSE);
    if (!strcmp(cmd, "float")) return tone(hz, ms, TRUE);
    if (!strcmp(cmd, "wasapi")) return wasapi(hz, ms);
    if (!strcmp(cmd, "both")) {
        g_hz = hz;
        g_ms = ms;
        HANDLE t = CreateThread(0, 0, wasapi_thread, 0, 0, 0);
        int r = tone(hz * 1.5, ms, FALSE);
        WaitForSingleObject(t, INFINITE);
        return r;
    }
    if (!strcmp(cmd, "beep")) {
        BOOL ok = Beep((DWORD)hz, ms);
        printf("Beep: %d\n", ok);
        return !ok;
    }
    if (!strcmp(cmd, "ding")) {
        BOOL ok = PlaySoundW(L"SystemAsterisk", 0, 0x00010000 /* SND_ALIAS */);
        printf("PlaySound: %d\n", ok);
        return !ok;
    }
    if (!strcmp(cmd, "msgbeep")) {
        BOOL ok = MessageBeep(MB_ICONASTERISK);
        Sleep(800);                                 /* it plays on winmm's thread */
        printf("MessageBeep: %d\n", ok);
        return !ok;
    }
    if (!strcmp(cmd, "play") && argc > 2) {
        WCHAR w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, w, MAX_PATH);
        DWORD t0 = GetTickCount();
        BOOL ok = PlaySoundW(w, 0, 0x00020000 /* SND_FILENAME */ | 0x2 /* SND_NODEFAULT */);
        printf("PlaySound: %d (%lu ms)\n", ok, GetTickCount() - t0);
        return !ok;
    }
    printf("usage: soundtest info | tone [HZ] [MS] | float [HZ] [MS] | play FILE | ding | wasapi [HZ] [MS] | beep [HZ] [MS]\n");
    return 1;
}
