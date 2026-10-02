/*
 * mmwave.h — the waveform-audio types and codes winmm's wave.c (output)
 * and wavein.c (input) share, laid out as Windows' mmeapi.h has them
 */

#pragma once

#include <windows.h>

#define MMAPI __declspec(dllexport)
typedef UINT MMRESULT;
#define MMSYSERR_NOERROR      0
#define MMSYSERR_ERROR        1
#define MMSYSERR_BADDEVICEID  2
#define MMSYSERR_ALLOCATED    4
#define MMSYSERR_INVALHANDLE  5
#define MMSYSERR_NODRIVER     6
#define MMSYSERR_NOMEM        7
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_INVALFLAG    10
#define MMSYSERR_INVALPARAM   11
#define WAVERR_BADFORMAT      32
#define WAVERR_STILLPLAYING   33
#define WAVERR_UNPREPARED     34

#define WAVE_MAPPER         ((UINT)-1)
#define WAVE_FORMAT_QUERY   0x0001
#define CALLBACK_TYPEMASK   0x00070000
#define CALLBACK_WINDOW     0x00010000
#define CALLBACK_THREAD     0x00020000
#define CALLBACK_FUNCTION   0x00030000
#define CALLBACK_EVENT      0x00050000
#define WOM_OPEN            0x3BB
#define WOM_CLOSE           0x3BC
#define WOM_DONE            0x3BD

#define WHDR_DONE       0x01
#define WHDR_PREPARED   0x02
#define WHDR_BEGINLOOP  0x04
#define WHDR_ENDLOOP    0x08
#define WHDR_INQUEUE    0x10

typedef struct wavehdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength, dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags, dwLoops;
    struct wavehdr_tag *lpNext;
    DWORD_PTR reserved;
} WAVEHDR;

typedef struct { UINT wType; union { DWORD ms, sample, cb, ticks; struct { BYTE hour, min, sec, frame, fps, dummy, pad[2]; } smpte; } u; } MMTIME;
#define TIME_MS      0x01
#define TIME_SAMPLES 0x02
#define TIME_BYTES   0x04

#define WIM_OPEN            0x3BE
#define WIM_CLOSE           0x3BF
#define WIM_DATA            0x3C0

/* the kernel's AudioStatus (kernel/drivers/audio.h) */
typedef struct {
    ULONGLONG written, consumed, played;
    UINT32 queued, capacity, running, latency;
} StreamStatus;

/* Post a window or thread callback message (wave.c) */
BOOL mm_post(BOOL thread, DWORD_PTR target, UINT msg, WPARAM wp, LPARAM lp);
const char *mm_error_text(MMRESULT e);
