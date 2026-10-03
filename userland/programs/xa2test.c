/*
 * xa2test — XAudio2 and X3DAudio on NovaOS
 *
 *   xa2test [HZ] [MS]
 *
 * 1. XAudio2 2.9 (XAudio2Create from xaudio2_9.dll): a mastering voice
 *    with a volume meter (CreateAudioVolumeMeter) in its effect chain and a
 *    44.1 kHz mono source voice playing a sine of HZ for MS in four
 *    buffers, the last marked end of stream; the voice and engine
 *    callbacks must fire, the samples played must add up and the meter
 *    must see the sine's peak.
 * 2. XAudio2 2.7 (CoCreateInstance of the DirectX SDK's class, then
 *    Initialize): a 100 ms buffer of 1.5 x HZ looped four more times
 *    through a submix voice; OnLoopEnd must fire four times.
 * 3. X3DAudio: an emitter to the listener's right must pan right.
 *
 *   xa2test devices [NAME HZ MS]
 *
 * The devices XAudio2 2.7 lists (GetDeviceCount, GetDeviceDetails: index
 * 0 the default, then every output); with NAME, a sine of HZ for MS
 * through XAudio2 2.9 on the device whose name holds NAME, its mastering
 * voice made with that device's ID (CreateMasteringVoice's szDeviceId,
 * the WASAPI endpoint ID GetDeviceDetails gives).
 */

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#define PI 3.14159265358979
#define VT(o) (*(void ***)(o))
#define M(o, i, T) ((T)VT(o)[i])

#pragma pack(push, 1)                /* (as the XAudio2 and X3DAudio headers have them) */
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WFX;
typedef struct {
    UINT32 Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void *pContext;
} XBUF;
typedef struct { void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XSTATE;
typedef struct { IUnknown *pEffect; BOOL InitialState; UINT32 OutputChannels; } XFXDESC;
typedef struct { UINT32 EffectCount; XFXDESC *pEffectDescriptors; } XCHAIN;
typedef struct { UINT32 Flags; void *pOutputVoice; } XSEND;
typedef struct { UINT32 OutputCount; void **pOutputVoices; } XSENDS27;
typedef struct { float *pPeakLevels, *pRMSLevels; UINT32 ChannelCount; } XMETER;
typedef struct { WCHAR DeviceID[256], DisplayName[256]; int Role; BYTE OutputFormat[40]; } XDETAILS;
typedef struct { float x, y, z; } V3;
typedef struct { V3 OrientFront, OrientTop, Position, Velocity; void *pCone; } X3DLISTENER;
typedef struct {
    void *pCone; V3 OrientFront, OrientTop, Position, Velocity;
    float InnerRadius, InnerRadiusAngle; UINT32 ChannelCount; float ChannelRadius; float *pChannelAzimuths;
    void *pVolumeCurve, *pLFECurve, *pLPFDirectCurve, *pLPFReverbCurve, *pReverbCurve;
    float CurveDistanceScaler, DopplerScaler;
} X3DEMITTER;
typedef struct {
    float *pMatrixCoefficients, *pDelayTimes; UINT32 SrcChannelCount, DstChannelCount;
    float LPFDirectCoefficient, LPFReverbCoefficient, ReverbLevel, DopplerFactor, EmitterToListenerAngle,
          EmitterToListenerDistance, EmitterVelocityComponent, ListenerVelocityComponent;
} X3DDSP;
#pragma pack(pop)

typedef HRESULT (WINAPI *XAudio2CreateFn)(void **, UINT32, UINT32);
typedef HRESULT (WINAPI *CreateMeterFn)(IUnknown **);
typedef HRESULT (__cdecl *X3DInitFn)(UINT32, float, void *);
typedef void (__cdecl *X3DCalcFn)(void *, const X3DLISTENER *, const X3DEMITTER *, UINT32, X3DDSP *);

/* -----------------------------------------------------------------------
 * Callbacks
 * ----------------------------------------------------------------------- */
static volatile LONG g_buffer_starts, g_buffer_ends, g_loop_ends, g_stream_ends, g_passes, g_errors;
static HANDLE g_done;

static void STDMETHODCALLTYPE cb_pass_start(void *t, UINT32 n) { (void)t; (void)n; }
static void STDMETHODCALLTYPE cb_pass_end(void *t) { (void)t; }
static void STDMETHODCALLTYPE cb_stream_end(void *t) { (void)t; InterlockedIncrement(&g_stream_ends); SetEvent(g_done); }
static void STDMETHODCALLTYPE cb_buffer_start(void *t, void *c) { (void)t; (void)c; InterlockedIncrement(&g_buffer_starts); }
/* SamplesPlayed starts again from 0 once the end of the stream has played
 * (as on Windows), so it is read as the third of the four buffers ends */
static void *g_sv;
static UINT64 g_played;
static void STDMETHODCALLTYPE cb_buffer_end(void *t, void *c)
{
    (void)t;
    InterlockedIncrement(&g_buffer_ends);
    if (c == (void *)13) {
        XSTATE st;
        M(g_sv, 25, void (STDMETHODCALLTYPE *)(void *, XSTATE *, UINT32))(g_sv, &st, 0);
        g_played = st.SamplesPlayed;
    }
    if (c == (void *)1) SetEvent(g_done);           /* the looped buffer (2.7) has finished */
}
static void STDMETHODCALLTYPE cb_loop_end(void *t, void *c) { (void)t; (void)c; InterlockedIncrement(&g_loop_ends); }
static void STDMETHODCALLTYPE cb_error(void *t, void *c, HRESULT hr) { (void)t; (void)c; (void)hr; InterlockedIncrement(&g_errors); }
static void *voice_cb_vtbl[] = { cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end,
                                 cb_loop_end, cb_error };
static struct { void **v; } voice_cb = { voice_cb_vtbl };

static void STDMETHODCALLTYPE ec_pass_start(void *t) { (void)t; InterlockedIncrement(&g_passes); }
static void STDMETHODCALLTYPE ec_pass_end(void *t) { (void)t; }
static void STDMETHODCALLTYPE ec_error(void *t, HRESULT hr) { (void)t; (void)hr; InterlockedIncrement(&g_errors); }
static void *engine_cb_vtbl[] = { ec_pass_start, ec_pass_end, ec_error };
static struct { void **v; } engine_cb = { engine_cb_vtbl };

static short *sine(double hz, UINT32 rate, UINT32 n)
{
    short *s = malloc(n * 2);
    for (UINT32 i = 0; i < n; i++) s[i] = (short)(16384 * sin(2 * PI * hz * i / rate));
    return s;
}

/* -----------------------------------------------------------------------
 * 1. XAudio2 2.9
 * ----------------------------------------------------------------------- */
static int test29(double hz, DWORD ms)
{
    HMODULE m = LoadLibraryA("xaudio2_9.dll");
    XAudio2CreateFn create = m ? (XAudio2CreateFn)GetProcAddress(m, "XAudio2Create") : 0;
    CreateMeterFn meter_fn = m ? (CreateMeterFn)GetProcAddress(m, "CreateAudioVolumeMeter") : 0;
    if (!create || !meter_fn) { printf("FAIL xaudio2_9.dll\n"); return 1; }
    void *xa = 0, *mv = 0, *sv = 0;
    HRESULT hr = create(&xa, 0, 1 /* XAUDIO2_DEFAULT_PROCESSOR */);
    if (FAILED(hr)) { printf("FAIL XAudio2Create: %08lx\n", hr); return 1; }
    M(xa, 3, HRESULT (STDMETHODCALLTYPE *)(void *, void *))(xa, &engine_cb);
    hr = M(xa, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **, UINT32, UINT32, UINT32, LPCWSTR, const XCHAIN *, int))
        (xa, &mv, 0, 0, 0, NULL, NULL, 6 /* AudioCategory_GameEffects */);
    if (FAILED(hr)) { printf("FAIL CreateMasteringVoice: %08lx\n", hr); return 1; }
    DWORD mask = 0;
    M(mv, 19, HRESULT (STDMETHODCALLTYPE *)(void *, DWORD *))(mv, &mask);
    struct { UINT32 flags, active, channels, rate; } md;
    M(mv, 0, void (STDMETHODCALLTYPE *)(void *, void *))(mv, &md);

    IUnknown *meter = 0;
    hr = meter_fn(&meter);
    if (FAILED(hr)) { printf("FAIL CreateAudioVolumeMeter: %08lx\n", hr); return 1; }
    XFXDESC fx = { meter, TRUE, md.channels };
    XCHAIN chain = { 1, &fx };
    hr = M(mv, 2, HRESULT (STDMETHODCALLTYPE *)(void *, const XCHAIN *))(mv, &chain);
    meter->lpVtbl->Release(meter);
    if (FAILED(hr)) { printf("FAIL SetEffectChain(volume meter): %08lx\n", hr); return 1; }

    WFX f = { 1, 1, 44100, 88200, 2, 16, 0 };
    hr = M(xa, 5, HRESULT (STDMETHODCALLTYPE *)(void *, void **, const WFX *, UINT32, float, void *, const void *, const XCHAIN *))
        (xa, &sv, &f, 0, 2.0f, &voice_cb, NULL, NULL);
    if (FAILED(hr)) { printf("FAIL CreateSourceVoice: %08lx\n", hr); return 1; }
    g_sv = sv;
    UINT32 total = f.nSamplesPerSec * ms / 1000, per = total / 4;
    short *pcm = sine(hz, f.nSamplesPerSec, total);
    for (int i = 0; i < 4; i++) {
        XBUF b = { i == 3 ? 0x40 /* XAUDIO2_END_OF_STREAM */ : 0, (i == 3 ? total - 3 * per : per) * 2,
                   (const BYTE *)(pcm + i * per), 0, 0, 0, 0, 0, (void *)(INT_PTR)(i + 11) };
        hr = M(sv, 21, HRESULT (STDMETHODCALLTYPE *)(void *, const XBUF *, const void *))(sv, &b, NULL);
        if (FAILED(hr)) { printf("FAIL SubmitSourceBuffer: %08lx\n", hr); return 1; }
    }
    DWORD t0 = GetTickCount();
    M(sv, 19, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, UINT32))(sv, 0, 0);
    /* the meter's peak half way through */
    Sleep(ms / 2);
    float peak[8] = { 0 }, rms[8] = { 0 };
    XMETER lv = { peak, rms, md.channels };
    M(mv, 7, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, void *, UINT32))(mv, 0, &lv, sizeof(lv));
    DWORD w = WaitForSingleObject(g_done, ms + 3000);
    DWORD elapsed = GetTickCount() - t0;
    XSTATE st;
    st.SamplesPlayed = g_played;
    printf("2.9: master %u ch %u Hz mask %lx; %llu samples played after 3 of 4 buffers (%u in all) in %lu ms; buffers %ld/%ld, stream end %ld, "
           "passes %ld, errors %ld; meter peak %.2f rms %.2f\n", md.channels, md.rate, mask, st.SamplesPlayed, total,
           elapsed, g_buffer_starts, g_buffer_ends, g_stream_ends, g_passes, g_errors, peak[0], rms[0]);
    int bad = 0;
    if (w != WAIT_OBJECT_0 || g_stream_ends != 1) { printf("FAIL no OnStreamEnd\n"); bad = 1; }
    if (g_buffer_starts != 4 || g_buffer_ends != 4) { printf("FAIL buffer callbacks\n"); bad = 1; }
    if (st.SamplesPlayed < 3 * per || st.SamplesPlayed > 3 * per + 2048) { printf("FAIL samples played\n"); bad = 1; }
    if (elapsed < ms * 9 / 10 || elapsed > ms * 3 / 2 + 500) { printf("FAIL playing took %lu ms\n", elapsed); bad = 1; }
    if (g_passes < (LONG)(ms / 20)) { printf("FAIL engine callbacks\n"); bad = 1; }
    if (peak[0] < 0.4f || peak[0] > 0.6f) { printf("FAIL the meter saw a peak of %.2f\n", peak[0]); bad = 1; }
    M(sv, 18, void (STDMETHODCALLTYPE *)(void *))(sv);
    M(mv, 18, void (STDMETHODCALLTYPE *)(void *))(mv);
    M(xa, 4, void (STDMETHODCALLTYPE *)(void *, void *))(xa, &engine_cb);
    ((IUnknown *)xa)->lpVtbl->Release((IUnknown *)xa);
    free(pcm);
    return bad;
}

/* -----------------------------------------------------------------------
 * 2. XAudio2 2.7, through COM
 * ----------------------------------------------------------------------- */
static const GUID CLSID_XAudio2_27 = { 0x5A508685, 0xA254, 0x4FBA, { 0x9B, 0x82, 0x9A, 0x24, 0xB0, 0x03, 0x06, 0xAF } };
static const GUID IID_IXAudio2_27  = { 0x8BCF1F58, 0x9FE7, 0x4583, { 0x8A, 0xC6, 0xE2, 0xAD, 0xC4, 0x65, 0xC8, 0xBB } };

static int test27(double hz)
{
    void *xa = 0, *mv = 0, *sub = 0, *sv = 0;
    CoInitialize(0);
    HRESULT hr = CoCreateInstance(&CLSID_XAudio2_27, 0, CLSCTX_INPROC_SERVER, &IID_IXAudio2_27, &xa);
    if (FAILED(hr)) { printf("FAIL CoCreateInstance(XAudio2 2.7): %08lx\n", hr); return 1; }
    UINT32 n = 0;
    M(xa, 3, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32 *))(xa, &n);
    hr = M(xa, 5, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, UINT32))(xa, 0, 0xFFFFFFFF /* XAUDIO2_ANY_PROCESSOR */);
    if (FAILED(hr)) { printf("FAIL Initialize: %08lx\n", hr); return 1; }
    hr = M(xa, 10, HRESULT (STDMETHODCALLTYPE *)(void *, void **, UINT32, UINT32, UINT32, UINT32, const XCHAIN *))
        (xa, &mv, 2, 48000, 0, 0, NULL);
    if (FAILED(hr)) { printf("FAIL CreateMasteringVoice: %08lx\n", hr); return 1; }
    hr = M(xa, 9, HRESULT (STDMETHODCALLTYPE *)(void *, void **, UINT32, UINT32, UINT32, UINT32, const void *, const XCHAIN *))
        (xa, &sub, 1, 22050, 0, 0, NULL, NULL);
    if (FAILED(hr)) { printf("FAIL CreateSubmixVoice: %08lx\n", hr); return 1; }
    XSENDS27 sends = { 1, &sub };
    WFX f = { 1, 1, 22050, 44100, 2, 16, 0 };
    hr = M(xa, 8, HRESULT (STDMETHODCALLTYPE *)(void *, void **, const WFX *, UINT32, float, void *, const void *, const XCHAIN *))
        (xa, &sv, &f, 0, 2.0f, &voice_cb, &sends, NULL);
    if (FAILED(hr)) { printf("FAIL CreateSourceVoice: %08lx\n", hr); return 1; }
    /* 100 ms holding a whole number of periods, so the loop is seamless */
    double hz2 = floor(hz * 1.5 / 10 + 0.5) * 10;
    UINT32 per = f.nSamplesPerSec / 10;
    short *pcm = sine(hz2, f.nSamplesPerSec, per);
    XBUF b = { 0x40, per * 2, (const BYTE *)pcm, 0, 0, 0, 0, 4, (void *)1 };
    g_buffer_ends = g_loop_ends = g_errors = 0;

    ResetEvent(g_done);
    M(sv, 21, HRESULT (STDMETHODCALLTYPE *)(void *, const XBUF *, const void *))(sv, &b, NULL);
    DWORD t0 = GetTickCount();
    M(sv, 19, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, UINT32))(sv, 0, 0);
    DWORD w = WaitForSingleObject(g_done, 4000);
    DWORD elapsed = GetTickCount() - t0;
    printf("2.7: %u device(s); %.0f Hz looped: %ld loop ends in %lu ms\n", n, hz2, g_loop_ends, elapsed);
    int bad = 0;
    if (w != WAIT_OBJECT_0 || g_loop_ends != 4) { printf("FAIL loop callbacks\n"); bad = 1; }
    if (elapsed < 450 || elapsed > 1000) { printf("FAIL five times 100 ms took %lu ms\n", elapsed); bad = 1; }
    M(sv, 18, void (STDMETHODCALLTYPE *)(void *))(sv);
    M(sub, 18, void (STDMETHODCALLTYPE *)(void *))(sub);
    M(mv, 18, void (STDMETHODCALLTYPE *)(void *))(mv);
    ((IUnknown *)xa)->lpVtbl->Release((IUnknown *)xa);
    free(pcm);
    return bad;
}

/* -----------------------------------------------------------------------
 * 3. X3DAudio
 * ----------------------------------------------------------------------- */
static int test3d(void)
{
    HMODULE m = LoadLibraryA("x3daudio1_7.dll");
    X3DInitFn init = m ? (X3DInitFn)GetProcAddress(m, "X3DAudioInitialize") : 0;
    X3DCalcFn calc = m ? (X3DCalcFn)GetProcAddress(m, "X3DAudioCalculate") : 0;
    if (!init || !calc) { printf("FAIL x3daudio1_7.dll\n"); return 1; }
    BYTE handle[20];
    init(3 /* SPEAKER_STEREO */, 343.5f, handle);
    X3DLISTENER l = { { 0, 0, 1 }, { 0, 1, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, 0 };
    X3DEMITTER e;
    memset(&e, 0, sizeof(e));
    e.OrientFront = (V3){ 0, 0, 1 };
    e.OrientTop = (V3){ 0, 1, 0 };
    e.Position = (V3){ 10, 0, 0 };
    e.ChannelCount = 1;
    e.CurveDistanceScaler = 1;
    e.DopplerScaler = 1;
    float matrix[2];
    X3DDSP d;
    memset(&d, 0, sizeof(d));
    d.pMatrixCoefficients = matrix;
    d.SrcChannelCount = 1;
    d.DstChannelCount = 2;
    calc(handle, &l, &e, 1 /* X3DAUDIO_CALCULATE_MATRIX */, &d);
    printf("X3DAudio: emitter 10 m right: left %.3f right %.3f, distance %.1f\n", matrix[0], matrix[1],
           d.EmitterToListenerDistance);
    if (!(matrix[1] > matrix[0] * 4) || fabs(d.EmitterToListenerDistance - 10) > 0.01) { printf("FAIL X3DAudio\n"); return 1; }
    return 0;
}

/* -----------------------------------------------------------------------
 * The devices
 * ----------------------------------------------------------------------- */
static int devices(const char *name, double hz, DWORD ms)
{
    void *xa = 0;
    CoInitialize(0);
    HRESULT hr = CoCreateInstance(&CLSID_XAudio2_27, 0, CLSCTX_INPROC_SERVER, &IID_IXAudio2_27, &xa);
    if (FAILED(hr)) { printf("FAIL CoCreateInstance(XAudio2 2.7): %08lx\n", hr); return 1; }
    UINT32 n = 0;
    M(xa, 3, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32 *))(xa, &n);
    printf("2.7: %u device(s)\n", n);
    WCHAR want[64], id[256] = { 0 }, found[256] = { 0 };
    if (name) MultiByteToWideChar(CP_ACP, 0, name, -1, want, 64);
    int bad = !n;
    for (UINT32 i = 0; i < n; i++) {
        XDETAILS d;
        memset(&d, 0, sizeof(d));
        hr = M(xa, 4, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, XDETAILS *))(xa, i, &d);
        if (FAILED(hr)) { printf("FAIL GetDeviceDetails(%u): %08lx\n", i, hr); bad = 1; continue; }
        printf("  %u: \"%ls\" %ls role %d\n", i, d.DisplayName, d.DeviceID, d.Role);
        if (name && i && !id[0] && wcsstr(d.DisplayName, want)) { lstrcpyW(id, d.DeviceID); lstrcpyW(found, d.DisplayName); }
    }
    ((IUnknown *)xa)->lpVtbl->Release((IUnknown *)xa);
    if (!name) return bad;
    if (!id[0]) { printf("FAIL no device named like \"%s\"\n", name); return 1; }

    HMODULE m = LoadLibraryA("xaudio2_9.dll");
    XAudio2CreateFn create = m ? (XAudio2CreateFn)GetProcAddress(m, "XAudio2Create") : 0;
    if (!create) { printf("FAIL xaudio2_9.dll\n"); return 1; }
    void *mv = 0, *sv = 0;
    hr = create(&xa, 0, 1);
    if (FAILED(hr)) { printf("FAIL XAudio2Create: %08lx\n", hr); return 1; }
    hr = M(xa, 7, HRESULT (STDMETHODCALLTYPE *)(void *, void **, UINT32, UINT32, UINT32, LPCWSTR, const XCHAIN *, int))
        (xa, &mv, 0, 0, 0, id, NULL, 6);
    if (FAILED(hr)) { printf("FAIL CreateMasteringVoice(%ls): %08lx\n", id, hr); return 1; }
    WFX f = { 1, 1, 44100, 88200, 2, 16, 0 };
    hr = M(xa, 5, HRESULT (STDMETHODCALLTYPE *)(void *, void **, const WFX *, UINT32, float, void *, const void *, const XCHAIN *))
        (xa, &sv, &f, 0, 2.0f, &voice_cb, NULL, NULL);
    if (FAILED(hr)) { printf("FAIL CreateSourceVoice: %08lx\n", hr); return 1; }
    UINT32 total = f.nSamplesPerSec * ms / 1000;
    short *pcm = sine(hz, f.nSamplesPerSec, total);
    XBUF b = { 0x40, total * 2, (const BYTE *)pcm, 0, 0, 0, 0, 0, 0 };
    g_stream_ends = 0;
    ResetEvent(g_done);
    M(sv, 21, HRESULT (STDMETHODCALLTYPE *)(void *, const XBUF *, const void *))(sv, &b, NULL);
    DWORD t0 = GetTickCount();
    M(sv, 19, HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, UINT32))(sv, 0, 0);
    DWORD w = WaitForSingleObject(g_done, ms + 3000);
    Sleep(200);                                     /* (what the engine queued plays out) */
    printf("2.9 on \"%ls\": %.0f Hz for %lu ms\n", found, hz, GetTickCount() - t0);
    if (w != WAIT_OBJECT_0) { printf("FAIL no OnStreamEnd\n"); bad = 1; }
    M(sv, 18, void (STDMETHODCALLTYPE *)(void *))(sv);
    M(mv, 18, void (STDMETHODCALLTYPE *)(void *))(mv);
    ((IUnknown *)xa)->lpVtbl->Release((IUnknown *)xa);
    free(pcm);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "devices")) {
        g_done = CreateEventW(0, FALSE, FALSE, 0);
        int bad = devices(argc > 2 ? argv[2] : NULL, argc > 3 ? atof(argv[3]) : 440, argc > 4 ? (DWORD)atoi(argv[4]) : 1000);
        printf("%s\n", bad ? "FAILED" : "XAudio2 devices passed");
        return bad != 0;
    }
    double hz = argc > 1 ? atof(argv[1]) : 440;
    DWORD ms = argc > 2 ? (DWORD)atoi(argv[2]) : 1000;
    g_done = CreateEventW(0, FALSE, FALSE, 0);
    int bad = test29(hz, ms);
    Sleep(300);                                     /* (a gap between the tones) */
    bad += test27(hz);
    bad += test3d();
    printf("%s\n", bad ? "FAILED" : "all XAudio2 tests passed");
    return bad != 0;
}
