/*
 * audio.h — the system mixer: playback streams mixed into the sound card,
 * and capture streams fed from its recording
 *
 * Every stream carries 16-bit stereo frames at its own rate: 48 kHz
 * unless its program set another (AudioSetRate: winmm plays a program's
 * sound at the rate it comes in).  Every device runs at its own rate too:
 * the HD Audio card at 48 kHz, a USB device at the rate it was set to
 * (usbaudio.c).  A kernel thread mixes the running streams into their
 * output's ring at that output's rate, a little ahead of the hardware,
 * converting each stream whose rate differs (once, and not at all when
 * the rates match): the HD Audio card's DMA ring, or a USB audio device's.
 * Capture streams are fed from their input's ring the same way, converted
 * from the input's rate to theirs.  Each stream uses the default device of
 * its direction, or the one its program chose.
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

/* A playback device: a ring of @bytes of interleaved s16 stereo frames at
 * @rate (0: 48 kHz) that the mixer keeps filled ahead of where the device
 * reads.
 * @position: the device's read offset in the ring (bytes, a whole frame).
 * @key (may be NULL: the name): what the device's saved volume and the
 * choice of default are kept under, the same each time it is attached
 * (a USB device's vendor, product and port) */
typedef struct {
    const char *name;
    const char *key;
    INT16      *ring;
    UINT32      bytes;
    UINT32      rate;
    UINT32    (*position)(void *ctx);
    void       *ctx;
} AudioOutput;

/* A recording device: a ring of @bytes of interleaved s16 stereo frames
 * at @rate (0: 48 kHz) that the device writes.  @position: how far it has written
 * (bytes, a whole frame).  @run (may be NULL): start or stop recording;
 * the mixer reads on from the position it finds after starting it. */
typedef struct {
    const char *name;
    const char *key;                    /* (as an output's) */
    INT16      *ring;
    UINT32      bytes;
    UINT32      rate;
    UINT32    (*position)(void *ctx);
    void      (*run)(void *ctx, bool on);
    void       *ctx;
} AudioInput;

/* An attached device, for the pickers (Settings, waveOut/waveIn device
 * IDs, WASAPI endpoints): @id stays the same while it is attached */
typedef struct {
    UINT32 id;
    bool   is_default;
    bool   mute;
    UINT32 volume;                      /* its endpoint volume (the louder channel), 0..65536 */
    UINT32 rate;                        /* the rate it runs at */
    char   name[96];
} AudioDevice;

/* Start the mixer and probe the sound card; false when there is no card
 * (an output can still be attached later) */
bool        AudioInit(void);
/* Make @o (kept until detached) the default output from now on, the way
 * Windows switches to a headset when it is plugged in; detaching the
 * default output goes back to the one that was the default before it.
 * An attached output nothing plays on gets silence (its ring keeps
 * streaming).  After AudioOutputDetach returns the mixer no longer
 * touches @o's ring. */
bool        AudioOutputAttach(const AudioOutput *o);
void        AudioOutputDetach(const AudioOutput *o);
/* Whether there is an output, and the default one's name and rate */
bool        AudioPresent(void);
const char *AudioDeviceName(void);
UINT32      AudioDeviceRate(void);

/* Make @i (kept until detached) the default input from now on, as
 * Windows switches to a USB microphone when it is plugged in; detaching
 * it goes back to the one before.  After AudioInputDetach returns the
 * mixer no longer reads @i's ring. */
bool        AudioInputAttach(const AudioInput *i);
void        AudioInputDetach(const AudioInput *i);
/* Whether there is an input, and the default one's name and rate */
bool        AudioCanRecord(void);
const char *AudioInputName(void);
UINT32      AudioInputRate(void);

/* The attached outputs (or @capture: inputs), oldest first, up to @max;
 * returns how many */
int         AudioDevices(bool capture, AudioDevice *out, int max);
/* Make device @id the default of its direction (Settings' choice; kept
 * in the registry, so it is the default again after a restart) */
bool        AudioSetDefault(bool capture, UINT32 id);
/* Once the registry is loaded: the saved choice of default and the saved
 * volumes (devices attached later get theirs as they attach) */
void        AudioLoadSettings(void);

/* A stream that can queue @frames (0: the default), playing or (@capture)
 * recording; -1 if none is free.  It runs at 48 kHz until AudioSetRate. */
int    AudioOpen(UINT32 frames, bool capture);
/* The rate stream @s's frames are at (8 kHz to 384 kHz; best set before
 * any are queued); false if out of range */
bool   AudioSetRate(int s, UINT32 rate);
/* Capture streams: take up to @n recorded frames; returns how many */
UINT32 AudioRead(int s, INT16 *frames, UINT32 n);
/* The endpoint volume of output (or @capture: input) device @id (0: the
 * default): 0..65536 a channel; each device has its own, kept in the
 * registry.  False if there is no such device (Get then gives full
 * volume, unmuted). */
bool   AudioSetMaster(int capture, UINT32 id, UINT32 left, UINT32 right, bool mute);
bool   AudioGetMaster(int capture, UINT32 id, UINT32 *left, UINT32 *right, bool *mute);
void   AudioClose(int s);
/* Queue up to @n frames (paused streams keep them); returns how many fit */
UINT32 AudioWrite(int s, const INT16 *frames, UINT32 n);
void   AudioRun(int s, bool run);
/* Play stream @s on (or record it from) device @id from now on; 0: the
 * default.  False if no such device of the stream's direction is attached */
bool   AudioRoute(int s, UINT32 id);
void   AudioFlush(int s);
/* Per-stream volume, 0..65536 for each channel */
void   AudioSetVolume(int s, UINT32 left, UINT32 right);
void   AudioGetVolume(int s, UINT32 *left, UINT32 *right);
bool   AudioGetStatus(int s, AudioStatus *st);
