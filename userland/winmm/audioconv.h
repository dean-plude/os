/*
 * audioconv.h — PCM conversion to the mixer's format (48 kHz s16 stereo)
 *
 * Shared by winmm (waveOut, PlaySound) and mmdevapi (WASAPI).  Takes 8, 16,
 * 24 or 32-bit integer PCM or 32-bit float, 1 to 8 channels (the first two
 * are kept; mono plays on both), at any rate (linear interpolation).
 * Recording runs the other way (AudioCapConv): the mixer's frames become
 * the program's format (mono is the average; channels past two are silent).
 */

#pragma once

#include <windows.h>

void *memcpy(void *d, const void *s, size_t n);         /* from ntdll */
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memmove(void *d, const void *s, size_t n);

#define AC_RATE 48000

typedef enum { AC_U8, AC_S16, AC_S24, AC_S32, AC_F32 } AcSample;

typedef struct {
    AcSample kind;
    UINT     channels, rate, block;     /* block: bytes per source frame */
    double   step, pos;                 /* source frames per output frame; position */
    float    prev[2];                   /* the last source frame of the previous call */
} AudioConv;

/* WAVEFORMATEX / WAVEFORMATEXTENSIBLE as Windows lays them out */
#pragma pack(push, 1)
typedef struct {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} AcWaveFormat;
typedef struct {
    AcWaveFormat Format;
    WORD  wValidBitsPerSample;
    DWORD dwChannelMask;
    GUID  SubFormat;
} AcWaveFormatExt;
#pragma pack(pop)

/* Set up @c for @fmt; false if NovaOS cannot play it */
static inline BOOL ac_init(AudioConv *c, const void *fmt)
{
    const AcWaveFormat *f = fmt;
    WORD tag = f->wFormatTag, bits = f->wBitsPerSample;
    if (tag == 0xFFFE && f->cbSize >= 22)                    /* WAVE_FORMAT_EXTENSIBLE: the subformat's tag */
        tag = (WORD)((const AcWaveFormatExt *)fmt)->SubFormat.Data1;
    if (!f->nChannels || f->nChannels > 8 || f->nSamplesPerSec < 1000 || f->nSamplesPerSec > 384000) return FALSE;
    if (tag == 1) {                                          /* WAVE_FORMAT_PCM */
        c->kind = bits == 8 ? AC_U8 : bits == 16 ? AC_S16 : bits == 24 ? AC_S24 : bits == 32 ? AC_S32 : (AcSample)-1;
        if ((int)c->kind < 0) return FALSE;
    } else if (tag == 3 && bits == 32) {                     /* WAVE_FORMAT_IEEE_FLOAT */
        c->kind = AC_F32;
    } else {
        return FALSE;
    }
    c->channels = f->nChannels;
    c->rate = f->nSamplesPerSec;
    c->block = f->nChannels * (bits / 8);
    c->step = (double)c->rate / AC_RATE;
    c->pos = 0;
    c->prev[0] = c->prev[1] = 0;
    return TRUE;
}

static inline float ac_sample(const AudioConv *c, const BYTE *p)
{
    switch (c->kind) {
    case AC_U8:  return (p[0] - 128) / 128.0f;
    case AC_S16: return *(const SHORT *)p / 32768.0f;
    case AC_S24: return (INT32)((UINT32)p[0] << 8 | (UINT32)p[1] << 16 | (UINT32)p[2] << 24) / 2147483648.0f;
    case AC_S32: return *(const INT32 *)p / 2147483648.0f;
    case AC_F32: return *(const float *)p;
    }
    return 0;
}

static inline void ac_frame(const AudioConv *c, const BYTE *src, LONG i, float out[2])
{
    if (i < 0) { out[0] = c->prev[0]; out[1] = c->prev[1]; return; }
    const BYTE *p = src + (size_t)i * c->block;
    UINT bytes = c->block / c->channels;
    out[0] = ac_sample(c, p);
    out[1] = c->channels > 1 ? ac_sample(c, p + bytes) : out[0];
}

static inline SHORT ac_s16(float v)
{
    v *= 32768.0f;
    return (SHORT)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)v);
}

/* The most source frames that convert into at most @out frames */
static inline UINT ac_src_for(const AudioConv *c, UINT out)
{
    double n = (out > 2 ? out - 2 : 0) * c->step;
    return n < 1 ? 1 : (UINT)n;
}

/* Convert @n source frames into at most @cap output frames (call with
 * n <= ac_src_for(cap)); returns the output frame count */
static inline UINT ac_convert(AudioConv *c, const void *src, UINT n, SHORT *out, UINT cap)
{
    UINT k = 0;
    if (!n) return 0;
    while (k < cap) {
        LONG i = (LONG)c->pos;                               /* floor; -1 is prev */
        if ((double)i > c->pos) i--;
        if (i + 1 > (LONG)n - 1) break;
        float fr = (float)(c->pos - i), a[2], b[2];
        ac_frame(c, src, i, a);
        ac_frame(c, src, i + 1, b);
        out[k * 2]     = ac_s16(a[0] + (b[0] - a[0]) * fr);
        out[k * 2 + 1] = ac_s16(a[1] + (b[1] - a[1]) * fr);
        k++;
        c->pos += c->step;
    }
    float last[2];
    ac_frame(c, src, (LONG)n - 1, last);
    c->prev[0] = last[0];
    c->prev[1] = last[1];
    c->pos -= n;                                             /* now relative to the next call (prev = -1) */
    return k;
}

/* -----------------------------------------------------------------------
 * Recording: 48 kHz s16 stereo to a program's format
 * ----------------------------------------------------------------------- */
typedef struct {
    AudioConv f;                        /* the program's format (f.step and f.pos unused) */
    double    step, pos;                /* mixer frames per program frame; position */
    SHORT     prev[2];
} AudioCapConv;

static inline BOOL acc_init(AudioCapConv *c, const void *fmt)
{
    if (!ac_init(&c->f, fmt)) return FALSE;
    c->step = (double)AC_RATE / c->f.rate;
    c->pos = 0;
    c->prev[0] = c->prev[1] = 0;
    return TRUE;
}

static inline void acc_put(const AudioConv *c, BYTE *p, float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    switch (c->kind) {
    case AC_U8:  *p = (BYTE)(int)(v * 127.0f + 128.0f); break;
    case AC_S16: *(SHORT *)p = ac_s16(v); break;
    case AC_S24: { INT32 x = (INT32)(v * 8388607.0f); p[0] = (BYTE)x; p[1] = (BYTE)(x >> 8); p[2] = (BYTE)(x >> 16); break; }
    case AC_S32: *(INT32 *)p = (INT32)((double)v * 2147483647.0); break;
    case AC_F32: *(float *)p = v; break;
    }
}

/* The most mixer frames that convert into at most @out program frames */
static inline UINT acc_src_for(const AudioCapConv *c, UINT out)
{
    double n = (out > 2 ? out - 2 : 0) * c->step;
    return n < 1 ? 1 : (UINT)n;
}

/* Convert @n mixer frames into at most @cap program frames at @out (call
 * with n <= acc_src_for(cap)); returns the program frame count */
static inline UINT acc_convert(AudioCapConv *c, const SHORT *src, UINT n, void *out, UINT cap)
{
    UINT k = 0, bytes = c->f.block / c->f.channels;
    BYTE *o = out;
    if (!n) return 0;
    while (k < cap) {
        LONG i = (LONG)c->pos;
        if ((double)i > c->pos) i--;
        if (i + 1 > (LONG)n - 1) break;
        float fr = (float)(c->pos - i), v[2];
        for (int ch = 0; ch < 2; ch++) {
            float a = (i < 0 ? c->prev[ch] : src[i * 2 + ch]) / 32768.0f, b = src[(i + 1) * 2 + ch] / 32768.0f;
            v[ch] = a + (b - a) * fr;
        }
        BYTE *p = o + (size_t)k * c->f.block;
        if (c->f.channels == 1) {
            acc_put(&c->f, p, (v[0] + v[1]) * 0.5f);
        } else {
            acc_put(&c->f, p, v[0]);
            acc_put(&c->f, p + bytes, v[1]);
            for (UINT ch = 2; ch < c->f.channels; ch++) acc_put(&c->f, p + ch * bytes, 0);
        }
        k++;
        c->pos += c->step;
    }
    c->prev[0] = src[(n - 1) * 2];
    c->prev[1] = src[(n - 1) * 2 + 1];
    c->pos -= n;
    return k;
}
