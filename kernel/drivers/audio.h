/*
 * audio.h — the system mixer: playback streams mixed into the sound card,
 * and capture streams fed from its recording
 *
 * Every stream carries 48 kHz, 16-bit stereo frames (winmm and mmdevapi
 * convert to that).  A kernel thread mixes the running streams into the
 * HD Audio DMA ring a little ahead of the hardware.
 */

#pragma once

#include "../include/types.h"

#define AUDIO_RATE       48000
#define AUDIO_MAX_FRAMES (AUDIO_RATE * 2)   /* the most a stream can queue (2 s) */

/* (for a capture stream: written = frames recorded, consumed = frames
 * read, played = frames dropped because the stream was full) */
typedef struct {
    UINT64 written;     /* frames accepted since opening (or the last flush) */
    UINT64 consumed;    /* of those, frames mixed into the device's ring */
    UINT64 played;      /* of those, frames the hardware has passed */
    UINT32 queued;      /* frames waiting to be mixed */
    UINT32 capacity;    /* frames the stream can hold */
    UINT32 running;
    UINT32 latency;     /* frames the device plays ahead of the mixer */
} AudioStatus;

/* Probe the sound card and start the mixer; false when there is none */
bool        AudioInit(void);
bool        AudioPresent(void);
const char *AudioDeviceName(void);

bool        AudioCanRecord(void);
const char *AudioInputName(void);

/* A stream that can queue @frames (0: the default), playing or (@capture)
 * recording; -1 if none is free */
int    AudioOpen(UINT32 frames, bool capture);
/* Capture streams: take up to @n recorded frames; returns how many */
UINT32 AudioRead(int s, INT16 *frames, UINT32 n);
/* The endpoint volume of playback (0) or recording (1): 0..65536 a channel */
void   AudioSetMaster(int capture, UINT32 left, UINT32 right, bool mute);
void   AudioGetMaster(int capture, UINT32 *left, UINT32 *right, bool *mute);
void   AudioClose(int s);
/* Queue up to @n frames (paused streams keep them); returns how many fit */
UINT32 AudioWrite(int s, const INT16 *frames, UINT32 n);
void   AudioRun(int s, bool run);
void   AudioFlush(int s);
/* Per-stream volume, 0..65536 for each channel */
void   AudioSetVolume(int s, UINT32 left, UINT32 right);
void   AudioGetVolume(int s, UINT32 *left, UINT32 *right);
bool   AudioGetStatus(int s, AudioStatus *st);
