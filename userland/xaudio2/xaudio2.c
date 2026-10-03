/*
 * xaudio2.c — XAudio2 on FAudio
 *
 * Built three times, as xaudio2_9.dll (XAUDIO2_VER 9), xaudio2_8.dll (8)
 * and xaudio2_7.dll (7, the DirectX SDK's, made with CoCreateInstance).
 * FAudio (third_party/faudio) is the engine; this file gives its objects
 * the COM shapes XAudio2 programs call: IXAudio2 (whose methods moved
 * between 2.7 and 2.8), the voices (plain interfaces without IUnknown),
 * voice and engine callbacks (FAudio calls structures of cdecl function
 * pointers, XAudio2 calls the program's stdcall vtables), and send lists
 * and effect chains, which name voices and XAPOs the program knows.
 * X3DAudio, the built-in effects and FAPOFX come from FAudio too.
 */

#include <windows.h>
#include <objbase.h>
#include <stdlib.h>
#include <string.h>
#include "FAudio.h"
#include "FAudioFX.h"
#include "FAPOFX.h"
#include "F3DAudio.h"
#include "xapo.h"

#ifndef XAUDIO2_VER
#define XAUDIO2_VER 9
#endif

#define EXPORT __declspec(dllexport)
#define CALLBACK_ STDMETHODCALLTYPE

#define XAUDIO2_E_INVALID_CALL ((HRESULT)0x88960001)
#define XAUDIO2_DEBUG_ENGINE   0x0001
#define XAUDIO2_1024_QUANTUM   0x8000

DEFINE_GUID(IID_IXAudio2_27,        0x8BCF1F58, 0x9FE7, 0x4583, 0x8A, 0xC6, 0xE2, 0xAD, 0xC4, 0x65, 0xC8, 0xBB);
DEFINE_GUID(IID_IXAudio2_28,        0x60D8DAC8, 0x5AA1, 0x4E8E, 0xB5, 0x97, 0x2F, 0x5E, 0x28, 0x83, 0xD4, 0x84);
DEFINE_GUID(IID_IXAudio2_29,        0x2B02E3CF, 0x2E0B, 0x4EC3, 0xBE, 0x45, 0x1B, 0x2A, 0x3F, 0xE7, 0x21, 0x0D);
DEFINE_GUID(IID_IXAudio2Extension,  0x84AC29BB, 0xD619, 0x44D2, 0xB1, 0x97, 0xE4, 0xAC, 0xF7, 0xDF, 0x3E, 0xD6);
DEFINE_GUID(CLSID_XAudio2_27,       0x5A508685, 0xA254, 0x4FBA, 0x9B, 0x82, 0x9A, 0x24, 0xB0, 0x03, 0x06, 0xAF);
DEFINE_GUID(CLSID_XAudio2Debug_27,  0xDB05EA35, 0x0329, 0x4D4B, 0xA5, 0x3A, 0x6D, 0xEA, 0xD0, 0x3D, 0x38, 0x52);
DEFINE_GUID(CLSID_AudioVolumeMeter_27, 0xCAC1105F, 0x619B, 0x4D04, 0x83, 0x1A, 0x44, 0xE1, 0xCB, 0xF1, 0x2D, 0x57);
DEFINE_GUID(CLSID_AudioReverb_27,   0x6A93130E, 0x1D53, 0x41D1, 0xA9, 0xCF, 0xE7, 0x58, 0x80, 0x0B, 0xB1, 0x79);

#if XAUDIO2_VER == 7
#define IID_IXAudio2_ME IID_IXAudio2_27
#elif XAUDIO2_VER == 8
#define IID_IXAudio2_ME IID_IXAudio2_28
#else
#define IID_IXAudio2_ME IID_IXAudio2_29
#endif

/* XAudio2's own structures where they differ from FAudio's (packed, as
 * xaudio2.h has them) */
#pragma pack(push, 1)
typedef struct { UINT32 CreationFlags, InputChannels, InputSampleRate; } VoiceDetails27;
typedef struct { UINT32 OutputCount; void **pOutputVoices; } VoiceSends27;
typedef struct { UINT32 Flags; void *pOutputVoice; } SendDescriptor;
typedef struct { UINT32 SendCount; SendDescriptor *pSends; } VoiceSends;
typedef struct { IUnknown *pEffect; BOOL InitialState; UINT32 OutputChannels; } EffectDescriptor;
typedef struct { UINT32 EffectCount; EffectDescriptor *pEffectDescriptors; } EffectChain;
typedef struct {
    UINT64 AudioCyclesSinceLastQuery, TotalCyclesSinceLastQuery;
    UINT32 MinimumCyclesPerQuantum, MaximumCyclesPerQuantum, MemoryUsageInBytes, CurrentLatencyInSamples,
           GlitchesSinceEngineStarted, ActiveSourceVoiceCount, TotalSourceVoiceCount, ActiveSubmixVoiceCount,
           TotalSubmixVoiceCount, ActiveXmaSourceVoices, ActiveXmaStreams;
} PerformanceData27;
#pragma pack(pop)

/* The program's callbacks */
typedef struct {
    void (CALLBACK_ *OnVoiceProcessingPassStart)(void *, UINT32);
    void (CALLBACK_ *OnVoiceProcessingPassEnd)(void *);
    void (CALLBACK_ *OnStreamEnd)(void *);
    void (CALLBACK_ *OnBufferStart)(void *, void *);
    void (CALLBACK_ *OnBufferEnd)(void *, void *);
    void (CALLBACK_ *OnLoopEnd)(void *, void *);
    void (CALLBACK_ *OnVoiceError)(void *, void *, HRESULT);
} VoiceCallbackVtbl;
typedef struct { const VoiceCallbackVtbl *v; } AppVoiceCallback;
typedef struct {
    void (CALLBACK_ *OnProcessingPassStart)(void *);
    void (CALLBACK_ *OnProcessingPassEnd)(void *);
    void (CALLBACK_ *OnCriticalError)(void *, HRESULT);
} EngineCallbackVtbl;
typedef struct { const EngineCallbackVtbl *v; } AppEngineCallback;

typedef struct XA2 XA2;
typedef struct Voice Voice;

enum { SOURCE, SUBMIX, MASTER };

struct Voice {
    const void          *vtbl;
    XA2                 *xa;
    int                  kind;
    FAudioVoice         *fv;
    FAudioVoiceCallback  fcb;           /* FAudio calls these ... */
    AppVoiceCallback    *app;           /* ... which call the program's */
    Voice               *next;
};

typedef struct EngineCb {
    FAudioEngineCallback fcb;
    AppEngineCallback   *app;
    struct EngineCb     *next;
} EngineCb;

struct XA2 {
    const void      *vtbl;              /* IXAudio2 */
    const void      *ext_vtbl;          /* IXAudio2Extension (2.9) */
    LONG             refs;
    FAudio          *fa;
    BOOL             init;
    CRITICAL_SECTION lock;
    Voice           *voices;
    EngineCb        *cbs;
};

static const void *source_vtbl_p, *submix_vtbl_p, *master_vtbl_p;

/* -----------------------------------------------------------------------
 * Callback thunks
 * ----------------------------------------------------------------------- */
#define VOICE_OF(cb) ((Voice *)((char *)(cb) - __builtin_offsetof(Voice, fcb)))

static void FAUDIOCALL vc_buffer_end(FAudioVoiceCallback *cb, void *ctx)
{ Voice *v = VOICE_OF(cb); v->app->v->OnBufferEnd(v->app, ctx); }
static void FAUDIOCALL vc_buffer_start(FAudioVoiceCallback *cb, void *ctx)
{ Voice *v = VOICE_OF(cb); v->app->v->OnBufferStart(v->app, ctx); }
static void FAUDIOCALL vc_loop_end(FAudioVoiceCallback *cb, void *ctx)
{ Voice *v = VOICE_OF(cb); v->app->v->OnLoopEnd(v->app, ctx); }
static void FAUDIOCALL vc_stream_end(FAudioVoiceCallback *cb)
{ Voice *v = VOICE_OF(cb); v->app->v->OnStreamEnd(v->app); }
static void FAUDIOCALL vc_error(FAudioVoiceCallback *cb, void *ctx, uint32_t err)
{ Voice *v = VOICE_OF(cb); v->app->v->OnVoiceError(v->app, ctx, (HRESULT)err); }
static void FAUDIOCALL vc_pass_end(FAudioVoiceCallback *cb)
{ Voice *v = VOICE_OF(cb); v->app->v->OnVoiceProcessingPassEnd(v->app); }
static void FAUDIOCALL vc_pass_start(FAudioVoiceCallback *cb, uint32_t bytes)
{ Voice *v = VOICE_OF(cb); v->app->v->OnVoiceProcessingPassStart(v->app, bytes); }

static void FAUDIOCALL ec_error(FAudioEngineCallback *cb, uint32_t err)
{ EngineCb *e = (EngineCb *)cb; e->app->v->OnCriticalError(e->app, (HRESULT)err); }
static void FAUDIOCALL ec_pass_end(FAudioEngineCallback *cb)
{ EngineCb *e = (EngineCb *)cb; e->app->v->OnProcessingPassEnd(e->app); }
static void FAUDIOCALL ec_pass_start(FAudioEngineCallback *cb)
{ EngineCb *e = (EngineCb *)cb; e->app->v->OnProcessingPassStart(e->app); }

/* -----------------------------------------------------------------------
 * Send lists and effect chains in FAudio's terms
 * ----------------------------------------------------------------------- */
static FAudioVoice *fv_of(const void *voice) { return voice ? ((const Voice *)voice)->fv : NULL; }

typedef struct { FAudioVoiceSends s; FAudioSendDescriptor d[16]; } Sends;

/* NULL: no list (the default send to the mastering voice) */
static FAudioVoiceSends *to_sends(const void *in, Sends *out)
{
    if (!in) return NULL;
#if XAUDIO2_VER == 7
    const VoiceSends27 *s = in;
    UINT32 n = s->OutputCount;
#else
    const VoiceSends *s = in;
    UINT32 n = s->SendCount;
#endif
    FAudioSendDescriptor *d = n <= 16 ? out->d : malloc(n * sizeof(*d));
    if (!d) return NULL;
    for (UINT32 i = 0; i < n; i++) {
#if XAUDIO2_VER == 7
        d[i].Flags = 0;
        d[i].pOutputVoice = fv_of(s->pOutputVoices[i]);
#else
        d[i].Flags = s->pSends[i].Flags;
        d[i].pOutputVoice = fv_of(s->pSends[i].pOutputVoice);
#endif
    }
    out->s.SendCount = n;
    out->s.pSends = d;
    return &out->s;
}
static void free_sends(Sends *s, FAudioVoiceSends *used) { if (used && used->pSends != s->d) free(used->pSends); }

typedef struct { FAudioEffectChain c; FAudioEffectDescriptor d[16]; } Chain;

static HRESULT to_chain(const EffectChain *in, Chain *out, FAudioEffectChain **res)
{
    *res = NULL;
    if (!in) return S_OK;
    UINT32 n = in->EffectCount;
    FAudioEffectDescriptor *d = n <= 16 ? out->d : malloc(n * sizeof(*d));
    if (!d) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < n; i++) {
        d[i].pEffect = xapo_to_fapo(in->pEffectDescriptors[i].pEffect);
        d[i].InitialState = in->pEffectDescriptors[i].InitialState;
        d[i].OutputChannels = in->pEffectDescriptors[i].OutputChannels;
        if (!d[i].pEffect) {
            while (i--) d[i].pEffect->Release(d[i].pEffect);
            if (d != out->d) free(d);
            return E_NOINTERFACE;
        }
    }
    out->c.EffectCount = n;
    out->c.pEffectDescriptors = d;
    *res = &out->c;
    return S_OK;
}
/* FAudio holds its own references now */
static void free_chain(Chain *c, FAudioEffectChain *used)
{
    if (!used) return;
    for (UINT32 i = 0; i < used->EffectCount; i++) used->pEffectDescriptors[i].pEffect->Release(used->pEffectDescriptors[i].pEffect);
    if (used->pEffectDescriptors != c->d) free(used->pEffectDescriptors);
}

/* -----------------------------------------------------------------------
 * Voices (IXAudio2Voice, IXAudio2SourceVoice, IXAudio2SubmixVoice,
 * IXAudio2MasteringVoice)
 * ----------------------------------------------------------------------- */
static void CALLBACK_ v_details(Voice *v, void *out)
{
    FAudioVoiceDetails d;
    FAudioVoice_GetVoiceDetails(v->fv, &d);
#if XAUDIO2_VER == 7
    VoiceDetails27 *o = out;
    o->CreationFlags = d.CreationFlags;
    o->InputChannels = d.InputChannels;
    o->InputSampleRate = d.InputSampleRate;
#else
    memcpy(out, &d, sizeof(d));
#endif
}
static HRESULT CALLBACK_ v_set_outputs(Voice *v, const void *sends)
{
    Sends s;
    FAudioVoiceSends *fs = to_sends(sends, &s);
    HRESULT hr = FAudioVoice_SetOutputVoices(v->fv, fs);
    free_sends(&s, fs);
    return hr;
}
static HRESULT CALLBACK_ v_set_chain(Voice *v, const EffectChain *chain)
{
    Chain c;
    FAudioEffectChain *fc;
    HRESULT hr = to_chain(chain, &c, &fc);
    if (FAILED(hr)) return hr;
    hr = FAudioVoice_SetEffectChain(v->fv, fc);
    free_chain(&c, fc);
    return hr;
}
static HRESULT CALLBACK_ v_enable_fx(Voice *v, UINT32 i, UINT32 op) { return FAudioVoice_EnableEffect(v->fv, i, op); }
static HRESULT CALLBACK_ v_disable_fx(Voice *v, UINT32 i, UINT32 op) { return FAudioVoice_DisableEffect(v->fv, i, op); }
static void CALLBACK_ v_fx_state(Voice *v, UINT32 i, BOOL *on) { FAudioVoice_GetEffectState(v->fv, i, (int32_t *)on); }
static HRESULT CALLBACK_ v_set_fx_params(Voice *v, UINT32 i, const void *p, UINT32 n, UINT32 op)
{ return FAudioVoice_SetEffectParameters(v->fv, i, p, n, op); }
static HRESULT CALLBACK_ v_get_fx_params(Voice *v, UINT32 i, void *p, UINT32 n)
{ return FAudioVoice_GetEffectParameters(v->fv, i, p, n); }
static HRESULT CALLBACK_ v_set_filter(Voice *v, const FAudioFilterParameters *p, UINT32 op)
{ return FAudioVoice_SetFilterParameters(v->fv, p, op); }
static void CALLBACK_ v_get_filter(Voice *v, FAudioFilterParameters *p) { FAudioVoice_GetFilterParameters(v->fv, p); }
static HRESULT CALLBACK_ v_set_out_filter(Voice *v, Voice *dst, const FAudioFilterParameters *p, UINT32 op)
{ return FAudioVoice_SetOutputFilterParameters(v->fv, fv_of(dst), p, op); }
static void CALLBACK_ v_get_out_filter(Voice *v, Voice *dst, FAudioFilterParameters *p)
{ FAudioVoice_GetOutputFilterParameters(v->fv, fv_of(dst), p); }
static HRESULT CALLBACK_ v_set_volume(Voice *v, float vol, UINT32 op) { return FAudioVoice_SetVolume(v->fv, vol, op); }
static void CALLBACK_ v_get_volume(Voice *v, float *vol) { FAudioVoice_GetVolume(v->fv, vol); }
static HRESULT CALLBACK_ v_set_ch_volumes(Voice *v, UINT32 n, const float *vols, UINT32 op)
{ return FAudioVoice_SetChannelVolumes(v->fv, n, vols, op); }
static void CALLBACK_ v_get_ch_volumes(Voice *v, UINT32 n, float *vols) { FAudioVoice_GetChannelVolumes(v->fv, n, vols); }
static HRESULT CALLBACK_ v_set_matrix(Voice *v, Voice *dst, UINT32 src_ch, UINT32 dst_ch, const float *m, UINT32 op)
{ return FAudioVoice_SetOutputMatrix(v->fv, fv_of(dst), src_ch, dst_ch, m, op); }
static void CALLBACK_ v_get_matrix(Voice *v, Voice *dst, UINT32 src_ch, UINT32 dst_ch, float *m)
{ FAudioVoice_GetOutputMatrix(v->fv, fv_of(dst), src_ch, dst_ch, m); }
static void CALLBACK_ v_destroy(Voice *v)
{
    XA2 *xa = v->xa;
    EnterCriticalSection(&xa->lock);
    Voice **pp = &xa->voices;
    while (*pp && *pp != v) pp = &(*pp)->next;
    if (*pp) *pp = v->next;
    LeaveCriticalSection(&xa->lock);
    FAudioVoice_DestroyVoice(v->fv);
    free(v);
}

#define VOICE_METHODS \
    v_details, v_set_outputs, v_set_chain, v_enable_fx, v_disable_fx, v_fx_state, v_set_fx_params, \
    v_get_fx_params, v_set_filter, v_get_filter, v_set_out_filter, v_get_out_filter, v_set_volume, v_get_volume, \
    v_set_ch_volumes, v_get_ch_volumes, v_set_matrix, v_get_matrix, v_destroy

static HRESULT CALLBACK_ s_start(Voice *v, UINT32 flags, UINT32 op) { return FAudioSourceVoice_Start(v->fv, flags, op); }
static HRESULT CALLBACK_ s_stop(Voice *v, UINT32 flags, UINT32 op) { return FAudioSourceVoice_Stop(v->fv, flags, op); }
static HRESULT CALLBACK_ s_submit(Voice *v, const FAudioBuffer *b, const FAudioBufferWMA *wma)
{ return FAudioSourceVoice_SubmitSourceBuffer(v->fv, b, wma); }
static HRESULT CALLBACK_ s_flush(Voice *v) { return FAudioSourceVoice_FlushSourceBuffers(v->fv); }
static HRESULT CALLBACK_ s_discontinuity(Voice *v) { return FAudioSourceVoice_Discontinuity(v->fv); }
static HRESULT CALLBACK_ s_exit_loop(Voice *v, UINT32 op) { return FAudioSourceVoice_ExitLoop(v->fv, op); }
#if XAUDIO2_VER == 7
static void CALLBACK_ s_state(Voice *v, FAudioVoiceState *st) { FAudioSourceVoice_GetState(v->fv, st, 0); }
#else
static void CALLBACK_ s_state(Voice *v, FAudioVoiceState *st, UINT32 flags) { FAudioSourceVoice_GetState(v->fv, st, flags); }
#endif
static HRESULT CALLBACK_ s_set_ratio(Voice *v, float r, UINT32 op) { return FAudioSourceVoice_SetFrequencyRatio(v->fv, r, op); }
static void CALLBACK_ s_get_ratio(Voice *v, float *r) { FAudioSourceVoice_GetFrequencyRatio(v->fv, r); }
static HRESULT CALLBACK_ s_set_rate(Voice *v, UINT32 rate) { return FAudioSourceVoice_SetSourceSampleRate(v->fv, rate); }

static const struct {
    void *m[19];
    void *start, *stop, *submit, *flush, *discontinuity, *exit_loop, *state, *set_ratio, *get_ratio, *set_rate;
} source_vtbl = {
    { VOICE_METHODS },
    s_start, s_stop, s_submit, s_flush, s_discontinuity, s_exit_loop, s_state, s_set_ratio, s_get_ratio, s_set_rate,
};
static const struct { void *m[19]; } submix_vtbl = { { VOICE_METHODS } };

#if XAUDIO2_VER == 7
static const struct { void *m[19]; } master_vtbl = { { VOICE_METHODS } };
#else
static HRESULT CALLBACK_ m_channel_mask(Voice *v, DWORD *mask) { return FAudioMasteringVoice_GetChannelMask(v->fv, (uint32_t *)mask); }
static const struct { void *m[19]; void *channel_mask; } master_vtbl = { { VOICE_METHODS }, m_channel_mask };
#endif

static Voice *new_voice(XA2 *xa, int kind)
{
    Voice *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->vtbl = kind == SOURCE ? (const void *)&source_vtbl : kind == SUBMIX ? (const void *)&submix_vtbl : (const void *)&master_vtbl;
    v->xa = xa;
    v->kind = kind;
    return v;
}
static void add_voice(XA2 *xa, Voice *v)
{
    EnterCriticalSection(&xa->lock);
    v->next = xa->voices;
    xa->voices = v;
    LeaveCriticalSection(&xa->lock);
}

/* -----------------------------------------------------------------------
 * IXAudio2
 * ----------------------------------------------------------------------- */
#define XA_OF_EXT(p) ((XA2 *)((char *)(p) - __builtin_offsetof(XA2, ext_vtbl)))

static HRESULT CALLBACK_ xa_qi(XA2 *xa, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IXAudio2_ME)
#if XAUDIO2_VER == 9
        || IsEqualIID(riid, &IID_IXAudio2_28)        /* 2.9 answers for 2.8's interface too */
#endif
        ) *ppv = xa;
#if XAUDIO2_VER == 9
    else if (IsEqualIID(riid, &IID_IXAudio2Extension)) *ppv = &xa->ext_vtbl;
#endif
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&xa->refs);
    return S_OK;
}
static ULONG CALLBACK_ xa_addref(XA2 *xa) { return InterlockedIncrement(&xa->refs); }
static ULONG CALLBACK_ xa_release(XA2 *xa)
{
    LONG r = InterlockedDecrement(&xa->refs);
    if (r) return r;
    if (xa->fa) {
        FAudio_StopEngine(xa->fa);
        /* voices the program left behind, the mastering voice last */
        for (int pass = 0; pass < 2; pass++)
            for (Voice **pp = &xa->voices; *pp;) {
                Voice *v = *pp;
                if ((v->kind == MASTER) != pass) { pp = &v->next; continue; }
                *pp = v->next;
                FAudioVoice_DestroyVoice(v->fv);
                free(v);
            }
        FAudio_Release(xa->fa);
    }
    while (xa->cbs) { EngineCb *e = xa->cbs; xa->cbs = e->next; free(e); }
    DeleteCriticalSection(&xa->lock);
    free(xa);
    return 0;
}

static HRESULT xa_initialize(XA2 *xa, UINT32 flags, UINT32 processor)
{
    (void)processor;
    if (xa->init) return XAUDIO2_E_INVALID_CALL;
    HRESULT hr = FAudio_Initialize(xa->fa, flags & (XAUDIO2_DEBUG_ENGINE | XAUDIO2_1024_QUANTUM), FAUDIO_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) xa->init = TRUE;
    return hr;
}

static HRESULT CALLBACK_ xa_register(XA2 *xa, AppEngineCallback *cb)
{
    if (!cb) return E_INVALIDARG;
    EngineCb *e = calloc(1, sizeof(*e));
    if (!e) return E_OUTOFMEMORY;
    e->fcb.OnCriticalError = ec_error;
    e->fcb.OnProcessingPassEnd = ec_pass_end;
    e->fcb.OnProcessingPassStart = ec_pass_start;
    e->app = cb;
    HRESULT hr = FAudio_RegisterForCallbacks(xa->fa, &e->fcb);
    if (FAILED(hr)) { free(e); return hr; }
    EnterCriticalSection(&xa->lock);
    e->next = xa->cbs;
    xa->cbs = e;
    LeaveCriticalSection(&xa->lock);
    return S_OK;
}
static void CALLBACK_ xa_unregister(XA2 *xa, AppEngineCallback *cb)
{
    EnterCriticalSection(&xa->lock);
    for (EngineCb **pp = &xa->cbs; *pp; pp = &(*pp)->next)
        if ((*pp)->app == cb) {
            EngineCb *e = *pp;
            *pp = e->next;
            FAudio_UnregisterForCallbacks(xa->fa, &e->fcb);
            free(e);
            break;
        }
    LeaveCriticalSection(&xa->lock);
}

static HRESULT CALLBACK_ xa_create_source(XA2 *xa, Voice **out, const FAudioWaveFormatEx *fmt, UINT32 flags,
                                          float max_ratio, AppVoiceCallback *cb, const void *sends,
                                          const EffectChain *chain)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    Voice *v = new_voice(xa, SOURCE);
    if (!v) return E_OUTOFMEMORY;
    if (cb) {
        v->app = cb;
        v->fcb = (FAudioVoiceCallback){ vc_buffer_end, vc_buffer_start, vc_loop_end, vc_stream_end, vc_error,
                                        vc_pass_end, vc_pass_start };
    }
    Sends s;
    Chain c;
    FAudioEffectChain *fc;
    HRESULT hr = to_chain(chain, &c, &fc);
    if (FAILED(hr)) { free(v); return hr; }
    FAudioVoiceSends *fs = to_sends(sends, &s);
    hr = FAudio_CreateSourceVoice(xa->fa, (FAudioSourceVoice **)&v->fv, fmt, flags, max_ratio, cb ? &v->fcb : NULL,
                                  fs, fc);
    free_sends(&s, fs);
    free_chain(&c, fc);
    if (FAILED(hr)) { free(v); return hr; }
    add_voice(xa, v);
    *out = v;
    return S_OK;
}

static HRESULT CALLBACK_ xa_create_submix(XA2 *xa, Voice **out, UINT32 channels, UINT32 rate, UINT32 flags,
                                          UINT32 stage, const void *sends, const EffectChain *chain)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    Voice *v = new_voice(xa, SUBMIX);
    if (!v) return E_OUTOFMEMORY;
    Sends s;
    Chain c;
    FAudioEffectChain *fc;
    HRESULT hr = to_chain(chain, &c, &fc);
    if (FAILED(hr)) { free(v); return hr; }
    FAudioVoiceSends *fs = to_sends(sends, &s);
    hr = FAudio_CreateSubmixVoice(xa->fa, (FAudioSubmixVoice **)&v->fv, channels, rate, flags, stage, fs, fc);
    free_sends(&s, fs);
    free_chain(&c, fc);
    if (FAILED(hr)) { free(v); return hr; }
    add_voice(xa, v);
    *out = v;
    return S_OK;
}

static HRESULT create_master(XA2 *xa, Voice **out, UINT32 channels, UINT32 rate, UINT32 flags, UINT32 index,
                             const EffectChain *chain)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    Voice *v = new_voice(xa, MASTER);
    if (!v) return E_OUTOFMEMORY;
    Chain c;
    FAudioEffectChain *fc;
    HRESULT hr = to_chain(chain, &c, &fc);
    if (FAILED(hr)) { free(v); return hr; }
    hr = FAudio_CreateMasteringVoice(xa->fa, (FAudioMasteringVoice **)&v->fv, channels, rate, flags, index, fc);
    free_chain(&c, fc);
    if (FAILED(hr)) { free(v); return hr; }
    add_voice(xa, v);
    *out = v;
    return S_OK;
}

static HRESULT CALLBACK_ xa_start(XA2 *xa) { return FAudio_StartEngine(xa->fa); }
static void CALLBACK_ xa_stop(XA2 *xa) { FAudio_StopEngine(xa->fa); }
static HRESULT CALLBACK_ xa_commit(XA2 *xa, UINT32 op) { return FAudio_CommitOperationSet(xa->fa, op); }
static void CALLBACK_ xa_debug(XA2 *xa, const FAudioDebugConfiguration *cfg, void *reserved)
{ FAudio_SetDebugConfiguration(xa->fa, (FAudioDebugConfiguration *)cfg, reserved); }

#if XAUDIO2_VER == 7
static HRESULT CALLBACK_ xa_device_count(XA2 *xa, UINT32 *n) { return FAudio_GetDeviceCount(xa->fa, n); }
static HRESULT CALLBACK_ xa_device_details(XA2 *xa, UINT32 i, FAudioDeviceDetails *d) { return FAudio_GetDeviceDetails(xa->fa, i, d); }
static HRESULT CALLBACK_ xa_init27(XA2 *xa, UINT32 flags, UINT32 processor) { return xa_initialize(xa, flags, processor); }
static HRESULT CALLBACK_ xa_create_master27(XA2 *xa, Voice **out, UINT32 channels, UINT32 rate, UINT32 flags,
                                            UINT32 index, const EffectChain *chain)
{ return create_master(xa, out, channels, rate, flags, index, chain); }
static void CALLBACK_ xa_perf27(XA2 *xa, PerformanceData27 *out)
{
    FAudioPerformanceData d;
    FAudio_GetPerformanceData(xa->fa, &d);
    *out = (PerformanceData27){ d.AudioCyclesSinceLastQuery, d.TotalCyclesSinceLastQuery, d.MinimumCyclesPerQuantum,
                                d.MaximumCyclesPerQuantum, d.MemoryUsageInBytes, d.CurrentLatencyInSamples,
                                d.GlitchesSinceEngineStarted, d.ActiveSourceVoiceCount, d.TotalSourceVoiceCount,
                                d.ActiveSubmixVoiceCount, d.ActiveSubmixVoiceCount, d.ActiveXmaSourceVoices,
                                d.ActiveXmaStreams };
}
static const struct { void *m[16]; } xa_vtbl = { {
    xa_qi, xa_addref, xa_release, xa_device_count, xa_device_details, xa_init27, xa_register, xa_unregister,
    xa_create_source, xa_create_submix, xa_create_master27, xa_start, xa_stop, xa_commit, xa_perf27, xa_debug,
} };
#else
static HRESULT CALLBACK_ xa_create_master(XA2 *xa, Voice **out, UINT32 channels, UINT32 rate, UINT32 flags,
                                          LPCWSTR device, const EffectChain *chain, int category)
{
    (void)category;
    UINT32 index = 0;
    if (device && device[0]) {                     /* our one device, by the ID GetDefaultAudioEndpoint gives */
        UINT32 n = 0;
        FAudio_GetDeviceCount(xa->fa, &n);
        if (!n) return XAUDIO2_E_INVALID_CALL;
    }
    return create_master(xa, out, channels, rate, flags, index, chain);
}
static void CALLBACK_ xa_perf(XA2 *xa, FAudioPerformanceData *d) { FAudio_GetPerformanceData(xa->fa, d); }
static const struct { void *m[13]; } xa_vtbl = { {
    xa_qi, xa_addref, xa_release, xa_register, xa_unregister, xa_create_source, xa_create_submix, xa_create_master,
    xa_start, xa_stop, xa_commit, xa_perf, xa_debug,
} };
#endif

#if XAUDIO2_VER == 9
/* IXAudio2Extension */
static HRESULT CALLBACK_ ext_qi(void *p, REFIID riid, void **ppv) { return xa_qi(XA_OF_EXT(p), riid, ppv); }
static ULONG CALLBACK_ ext_addref(void *p) { return xa_addref(XA_OF_EXT(p)); }
static ULONG CALLBACK_ ext_release(void *p) { return xa_release(XA_OF_EXT(p)); }
static void CALLBACK_ ext_quantum(void *p, UINT32 *num, UINT32 *den)
{ FAudio_GetProcessingQuantum(XA_OF_EXT(p)->fa, num, den); }
static void CALLBACK_ ext_processor(void *p, UINT32 *proc) { (void)p; if (proc) *proc = 1; }
static const struct { void *m[5]; } ext_vtbl = { { ext_qi, ext_addref, ext_release, ext_quantum, ext_processor } };
#endif

static HRESULT new_xaudio2(XA2 **out)
{
    XA2 *xa = calloc(1, sizeof(*xa));
    if (!xa) return E_OUTOFMEMORY;
    xa->vtbl = &xa_vtbl;
#if XAUDIO2_VER == 9
    xa->ext_vtbl = &ext_vtbl;
#endif
    xa->refs = 1;
    InitializeCriticalSection(&xa->lock);
    if (FAudioCOMConstructEXT(&xa->fa, XAUDIO2_VER)) {
        DeleteCriticalSection(&xa->lock);
        free(xa);
        return E_OUTOFMEMORY;
    }
    *out = xa;
    return S_OK;
}

/* -----------------------------------------------------------------------
 * Exports
 * ----------------------------------------------------------------------- */
#if XAUDIO2_VER >= 8
EXPORT HRESULT WINAPI XAudio2Create(void **out, UINT32 flags, UINT32 processor)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    XA2 *xa;
    HRESULT hr = new_xaudio2(&xa);
    if (FAILED(hr)) return hr;
    hr = xa_initialize(xa, flags, processor);
    if (FAILED(hr)) { xa_release(xa); return hr; }
    *out = xa;
    return S_OK;
}
#if XAUDIO2_VER == 9
EXPORT HRESULT WINAPI XAudio2CreateWithVersionInfo(void **out, UINT32 flags, UINT32 processor, DWORD ntddi)
{
    (void)ntddi;
    return XAudio2Create(out, flags, processor);
}
#endif

EXPORT HRESULT WINAPI CreateAudioVolumeMeter(IUnknown **out)
{
    FAPO *f;
    if (!out) return E_INVALIDARG;
    if (FAudioCreateVolumeMeter(&f, 0)) return E_OUTOFMEMORY;
    return fapo_to_xapo(f, out);
}
EXPORT HRESULT WINAPI CreateAudioReverb(IUnknown **out)
{
    FAPO *f;
    if (!out) return E_INVALIDARG;
#if XAUDIO2_VER == 9
    if (FAudioCreateReverb9(&f, 0)) return E_OUTOFMEMORY;
#else
    if (FAudioCreateReverb(&f, 0)) return E_OUTOFMEMORY;
#endif
    return fapo_to_xapo(f, out);
}
EXPORT HRESULT __cdecl CreateFX(REFCLSID clsid, IUnknown **out, const void *init, UINT32 init_size)
{
    FAPO *f;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (FAPOFX_CreateFX((const FAudioGUID *)clsid, &f, init, init_size)) return CLASS_E_CLASSNOTAVAILABLE;
    return fapo_to_xapo(f, out);
}

/* X3DAudio lives in xaudio2_8 and xaudio2_9 */
EXPORT HRESULT __cdecl X3DAudioInitialize(UINT32 mask, float speed_of_sound, void *instance)
{
    return F3DAudioInitialize8(mask, speed_of_sound, instance);
}
EXPORT void __cdecl X3DAudioCalculate(void *instance, const void *listener, const void *emitter, UINT32 flags, void *dsp)
{
    F3DAudioCalculate(instance, listener, emitter, flags, dsp);
}
#else

/* xaudio2_7: COM classes (XAudio2, its debug twin, and the two effects) */
typedef struct { const IClassFactoryVtbl *v; int kind; } Factory;
static HRESULT CALLBACK_ cf_qi(IClassFactory *cf, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory) ? cf : NULL;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static ULONG CALLBACK_ cf_addref(IClassFactory *cf) { (void)cf; return 2; }
static ULONG CALLBACK_ cf_release(IClassFactory *cf) { (void)cf; return 1; }
static HRESULT CALLBACK_ cf_create(IClassFactory *cf, IUnknown *outer, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    int kind = ((Factory *)cf)->kind;
    IUnknown *u;
    HRESULT hr;
    if (kind == 0) {                                /* XAudio2: the program calls Initialize */
        XA2 *xa;
        hr = new_xaudio2(&xa);
        if (FAILED(hr)) return hr;
        hr = xa_qi(xa, riid, ppv);
        xa_release(xa);
        return hr;
    }
    FAPO *f;
    if (kind == 1 ? FAudioCreateVolumeMeter(&f, 0) : FAudioCreateReverb(&f, 0)) return E_OUTOFMEMORY;
    hr = fapo_to_xapo(f, &u);
    if (FAILED(hr)) return hr;
    hr = u->lpVtbl->QueryInterface(u, riid, ppv);
    u->lpVtbl->Release(u);
    return hr;
}
static HRESULT CALLBACK_ cf_lock(IClassFactory *cf, BOOL lock) { (void)cf; (void)lock; return S_OK; }
static const IClassFactoryVtbl cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_xaudio2 = { &cf_vtbl, 0 }, g_meter = { &cf_vtbl, 1 }, g_reverb = { &cf_vtbl, 2 };

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    Factory *f = IsEqualCLSID(clsid, &CLSID_XAudio2_27) || IsEqualCLSID(clsid, &CLSID_XAudio2Debug_27) ? &g_xaudio2 :
                 IsEqualCLSID(clsid, &CLSID_AudioVolumeMeter_27) ? &g_meter :
                 IsEqualCLSID(clsid, &CLSID_AudioReverb_27) ? &g_reverb : NULL;
    if (!f) return CLASS_E_CLASSNOTAVAILABLE;
    return cf_qi((IClassFactory *)f, riid, ppv);
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
EXPORT HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
EXPORT HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }
#endif
