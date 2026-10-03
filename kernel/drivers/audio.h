/*
 * audio.h — the system mixer: playback streams mixed into the sound card,
 * and capture streams fed from its recording
 *
 * Every stream carries 48 kHz, 16-bit stereo frames (winmm and mmdevapi
 * convert to that).  A kernel thread mixes the running streams into the
 * playing output's ring a little ahead of the hardware: the HD Audio
 * card's DMA ring, or a USB audio device's (usbaudio.c).  Capture streams
 * are fed from the recording input's ring the same way: the HD Audio
 * card's, or a USB microphone's.
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

/* A playback device: a ring of @bytes of interleaved 48 kHz s16 stereo
 * frames that the mixer keeps filled ahead of where the device reads.
 * @position: the device's read offset in the ring (bytes, a whole frame) */
typedef struct {
    const char *name;
    INT16      *ring;
    UINT32      bytes;
    UINT32    (*position)(void *ctx);
    void       *ctx;
} AudioOutput;

/* A recording device: a ring of @bytes of interleaved 48 kHz s16 stereo
 * frames that the device writes.  @position: how far it has written
 * (bytes, a whole frame).  @run (may be NULL): start or stop recording;
 * the mixer reads on from the position it finds after starting it. */
typedef struct {
    const char *name;
    INT16      *ring;
    UINT32      bytes;
    UINT32    (*position)(void *ctx);
    void      (*run)(void *ctx, bool on);
    void       *ctx;
} AudioInput;

/* Start the mixer and probe the sound card; false when there is no card
 * (an output can still be attached later) */
bool        AudioInit(void);
/* Play on @o (kept until detached) from now on, the way Windows switches
 * to a headset when it is plugged in; detaching the playing output goes
 * back to the one attached before it.  An attached output that is not
 * playing gets silence (its ring keeps streaming).  After
 * AudioOutputDetach returns the mixer no longer touches @o's ring. */
bool        AudioOutputAttach(const AudioOutput *o);
void        AudioOutputDetach(const AudioOutput *o);
/* Whether there is an output, and the playing one's name */
bool        AudioPresent(void);
const char *AudioDeviceName(void);

/* Record from @i (kept until detached) from now on, as Windows switches
 * to a USB microphone when it is plugged in; detaching it goes back to
 * the one attached before.  After AudioInputDetach returns the mixer no
 * longer reads @i's ring. */
bool        AudioInputAttach(const AudioInput *i);
void        AudioInputDetach(const AudioInput *i);
/* Whether there is an input, and the recording one's name */
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
