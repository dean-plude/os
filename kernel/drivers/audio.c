/*
 * audio.c — the system mixer
 *
 * Streams are rings of 48 kHz s16 stereo frames.  The mixer thread wakes
 * every tick (10 ms), reads how far the hardware has played the playing
 * output's ring, and mixes the running streams into the ring up to LEAD
 * bytes ahead of that.  Outputs are attached by their drivers: the HD
 * Audio card at boot, a USB audio device when it is plugged in.  The one
 * attached last plays; when it leaves, the one before it takes over.
 * The others keep streaming their rings, so the mixer keeps silence ahead
 * of them the same way: what was mixed for an output before another took
 * over still plays, and then it is quiet (not its ring's last 341 ms
 * over and over).  Everything a stream has is guarded by one spinlock;
 * the mixing itself runs under it too (a few thousand frames per tick).
 *
 * Capture streams run the other way: while any is running the sound card
 * records into its capture ring, and every tick the new frames are copied
 * into each running capture stream (the oldest dropped when a stream is
 * full).  Each direction has a master volume and mute (the endpoint
 * volume programs set through IAudioEndpointVolume).
 */

#include "audio.h"
#include "hda.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../ke/spinlock.h"

#define MAX_STREAMS   32
#define FRAME         4                               /* bytes: s16 x 2 */
#define LEAD          (AUDIO_RATE * FRAME * 80 / 1000) /* mix 80 ms ahead of the hardware */
#define CHUNK_FRAMES  1024
#define TICK_FRAMES   (AUDIO_RATE / 100)              /* recorded frames arrive a tick at a time */

typedef struct {
    bool    used, running, capture;
    INT16  *buf;
    UINT32  pages;
    UINT32  cap, rd, queued;                          /* frames */
    UINT64  written, consumed;
    UINT64  last_end;                                 /* ring byte position (absolute) after its last mixed frame */
    UINT32  vol_l, vol_r;                             /* 0..65536 */
    UINT64  dropped;                                  /* capture: frames lost to a full stream */
} Stream;

#define MAX_OUTPUTS   4

typedef struct { UINT32 l, r; bool mute; } Master;

/* Where an output is: its position as an absolute byte count, and how
 * far ahead of it its ring has been written */
typedef struct {
    UINT32 last_pos;
    UINT64 base;                                      /* absolute byte count at the ring's start */
    UINT64 write;
} Track;

static struct {
    bool       hda;                                   /* the HD Audio card (it also records) */
    KSpinLock  lock;
    Stream     s[MAX_STREAMS];
    const AudioOutput *outs[MAX_OUTPUTS];             /* attached, oldest first */
    Track      idle[MAX_OUTPUTS];                     /* each one not playing: silenced up to where */
    int        nouts;
    const AudioOutput *out;                           /* playing: the last of outs */
    INT16     *ring;
    UINT32     ring_bytes;
    UINT32     last_pos;
    UINT64     hw_base;                               /* absolute byte count at the ring's start */
    UINT64     write_abs;                             /* mixed up to here */
    INT32      acc[CHUNK_FRAMES * 2];
    Master     master[2];                             /* render, capture */
    /* recording */
    INT16     *cring;
    UINT32     cring_bytes;
    UINT32     cpos;                                  /* read up to here in the capture ring */
    bool       crun;
} g = { .lock = KSPINLOCK_INIT, .master = { { 65536, 65536, false }, { 65536, 65536, false } } };

/* The hardware position as an absolute byte count (lock held, an output
 * playing) */
static UINT64 hw_abs(void)
{
    UINT32 pos = g.out->position(g.out->ctx);
    if (pos < g.last_pos) g.hw_base += g.ring_bytes;
    g.last_pos = pos;
    return g.hw_base + pos;
}

/* Mix @n frames into the ring at absolute byte offset @at (lock held) */
static void mix_chunk(UINT64 at, UINT32 n)
{
    memset(g.acc, 0, n * 2 * sizeof(INT32));
    Master *mr = &g.master[0];
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = &g.s[i];
        if (!s->used || s->capture || !s->running || !s->queued) continue;
        UINT32 m = s->queued < n ? s->queued : n;
        for (UINT32 f = 0; f < m; f++) {
            const INT16 *src = s->buf + (size_t)s->rd * 2;
            g.acc[f * 2]     += (INT32)(((INT64)src[0] * s->vol_l) >> 16);
            g.acc[f * 2 + 1] += (INT32)(((INT64)src[1] * s->vol_r) >> 16);
            if (++s->rd == s->cap) s->rd = 0;
        }
        s->queued -= m;
        s->consumed += m;
        s->last_end = at + (UINT64)m * FRAME;
    }
    INT16 *dst = (INT16 *)((UINT8 *)g.ring + at % g.ring_bytes);
    for (UINT32 k = 0; k < n * 2; k++) {
        INT32 v = mr->mute ? 0 : (INT32)(((INT64)g.acc[k] * (k & 1 ? mr->r : mr->l)) >> 16);
        dst[k] = (INT16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* Keep silence LEAD bytes ahead of each attached output that is not
 * playing: it still streams its ring (lock held) */
static void silence_idle(void)
{
    for (int i = 0; i < g.nouts; i++) {
        const AudioOutput *o = g.outs[i];
        if (o == g.out) continue;
        Track *t = &g.idle[i];
        UINT32 pos = o->position(o->ctx);
        if (pos < t->last_pos) t->base += o->bytes;
        t->last_pos = pos;
        UINT64 hw = t->base + pos;
        if (t->write < hw) t->write = hw;
        UINT64 target = hw + LEAD;
        while (t->write < target) {
            UINT32 off = (UINT32)(t->write % o->bytes);
            UINT64 n = target - t->write;
            if (n > o->bytes - off) n = o->bytes - off;
            memset((UINT8 *)o->ring + off, 0, (size_t)n);
            t->write += n;
        }
    }
}

static void mix_ahead(void)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    silence_idle();
    if (!g.out) { spin_unlock_irqrestore(&g.lock, st); return; }
    UINT64 hw = hw_abs();
    if (g.write_abs < hw) g.write_abs = hw;           /* fell behind: skip what was missed */
    UINT64 target = hw + LEAD;
    while (g.write_abs < target) {
        UINT32 off = (UINT32)(g.write_abs % g.ring_bytes);
        UINT64 n = (target - g.write_abs) / FRAME;
        UINT32 room = (g.ring_bytes - off) / FRAME;
        if (n > room) n = room;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
        if (!n) break;
        mix_chunk(g.write_abs, (UINT32)n);
        g.write_abs += n * FRAME;
    }
    spin_unlock_irqrestore(&g.lock, st);
}

/* Start or stop recording to match the running capture streams (lock held) */
static void capture_sync(void)
{
    bool want = false;
    for (int i = 0; i < MAX_STREAMS && !want; i++)
        want = g.s[i].used && g.s[i].capture && g.s[i].running;
    if (want == g.crun) return;
    g.crun = want;
    g.cpos = 0;                                       /* the ring restarts */
    HdaCapture(want);
}

/* Copy what the card recorded since the last tick into the running
 * capture streams */
static void pull_capture(void)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    if (g.crun) {
        UINT32 pos = HdaCapturePosition();
        Master *mc = &g.master[1];
        while (g.cpos != pos) {
            UINT32 end = pos > g.cpos ? pos : g.cring_bytes;      /* up to the wrap first */
            UINT32 n = (end - g.cpos) / FRAME;
            const INT16 *src = (const INT16 *)((UINT8 *)g.cring + g.cpos);
            for (int i = 0; i < MAX_STREAMS; i++) {
                Stream *s = &g.s[i];
                if (!s->used || !s->capture || !s->running) continue;
                UINT32 vl = mc->mute ? 0 : (UINT32)(((UINT64)mc->l * s->vol_l) >> 16);
                UINT32 vr = mc->mute ? 0 : (UINT32)(((UINT64)mc->r * s->vol_r) >> 16);
                for (UINT32 f = 0; f < n; f++) {
                    if (s->queued == s->cap) {                    /* full: drop the oldest */
                        if (++s->rd == s->cap) s->rd = 0;
                        s->queued--;
                        s->dropped++;
                    }
                    INT16 *d = s->buf + (size_t)((s->rd + s->queued) % s->cap) * 2;
                    d[0] = (INT16)(((INT32)src[f * 2] * (INT64)vl) >> 16);
                    d[1] = (INT16)(((INT32)src[f * 2 + 1] * (INT64)vr) >> 16);
                    s->queued++;
                }
                s->written += n;
            }
            g.cpos = end == g.cring_bytes ? 0 : end;
        }
    }
    spin_unlock_irqrestore(&g.lock, st);
}

static void mixer_thread(void *arg)
{
    (void)arg;
    bkl_release();                                    /* runs under its own lock */
    for (;;) {
        sched_sleep_tick();
        mix_ahead();
        pull_capture();
    }
}

/* Play on @o from its current position on (lock held; NULL: nothing plays) */
static void switch_output(const AudioOutput *o)
{
    for (int i = 0; i < g.nouts && g.out && g.out != o; i++)
        if (g.outs[i] == g.out)                       /* still attached: silence it after what was mixed */
            g.idle[i] = (Track){ .last_pos = g.last_pos, .base = g.hw_base, .write = g.write_abs };
    g.out = o;
    if (!o) return;
    g.ring = o->ring;
    g.ring_bytes = o->bytes;
    g.last_pos = o->position(o->ctx);
    g.hw_base = 0;
    g.write_abs = g.last_pos;                         /* mix_ahead starts at the device */
    for (int i = 0; i < MAX_STREAMS; i++) g.s[i].last_end = 0;
}

bool AudioOutputAttach(const AudioOutput *o)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool ok = g.nouts < MAX_OUTPUTS;
    if (ok) {
        g.outs[g.nouts++] = o;
        switch_output(o);
    }
    spin_unlock_irqrestore(&g.lock, st);
    if (ok) kprintf("[AUDIO] Playing on %s\n", o->name);
    return ok;
}

void AudioOutputDetach(const AudioOutput *o)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool found = false;
    for (int i = 0; i < g.nouts; i++) {
        if (g.outs[i] != o) continue;
        for (int j = i + 1; j < g.nouts; j++) g.outs[j - 1] = g.outs[j], g.idle[j - 1] = g.idle[j];
        g.nouts--;
        found = true;
        break;
    }
    const AudioOutput *now = g.nouts ? g.outs[g.nouts - 1] : NULL;
    if (found && g.out != now) switch_output(now);
    spin_unlock_irqrestore(&g.lock, st);
    if (found && now) kprintf("[AUDIO] Playing on %s\n", now->name);
    else if (found) kprintf("[AUDIO] No sound output left\n");
}

static UINT32 hda_position(void *ctx) { (void)ctx; return HdaPosition(); }
static AudioOutput g_hda_out = { .position = hda_position };

bool AudioInit(void)
{
    if (!sched_create_thread("audio", mixer_thread, NULL, 12)) {
        kprintf("[AUDIO] Could not start the mixer\n");
        return false;
    }
    if (!HdaInit()) return false;
    g.cring = HdaCaptureRing(&g.cring_bytes);
    g.hda = true;
    g_hda_out.name = HdaName();
    g_hda_out.ring = HdaRing(&g_hda_out.bytes);
    AudioOutputAttach(&g_hda_out);
    return true;
}

bool        AudioPresent(void)    { return g.out != NULL; }
bool        AudioCanRecord(void)  { return g.hda && HdaCanRecord(); }
const char *AudioInputName(void)  { return HdaInputName(); }

const char *AudioDeviceName(void)
{
    const AudioOutput *o = g.out;
    return o ? o->name : "";
}

int AudioOpen(UINT32 frames, bool capture)
{
    if (capture ? !AudioCanRecord() : !AudioPresent()) return -1;
    if (!frames) frames = AUDIO_RATE / 2;
    if (frames > AUDIO_MAX_FRAMES) frames = AUDIO_MAX_FRAMES;
    if (frames < 1024) frames = 1024;
    UINT32 pages = (frames * FRAME + PAGE_SIZE - 1) / PAGE_SIZE;
    INT16 *buf = kernel_alloc_pages(pages);
    if (!buf) return -1;
    IrqState st = spin_lock_irqsave(&g.lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = &g.s[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->buf = buf;
        s->pages = pages;
        s->cap = frames;
        s->capture = capture;
        s->vol_l = s->vol_r = 65536;
        spin_unlock_irqrestore(&g.lock, st);
        return i;
    }
    spin_unlock_irqrestore(&g.lock, st);
    kernel_free_pages(buf, pages);
    return -1;
}

static Stream *get(int s) { return s >= 0 && s < MAX_STREAMS && g.s[s].used ? &g.s[s] : NULL; }

void AudioClose(int i)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    INT16 *buf = s ? s->buf : NULL;
    UINT32 pages = s ? s->pages : 0;
    if (s) { s->used = false; capture_sync(); }
    spin_unlock_irqrestore(&g.lock, st);
    if (buf) kernel_free_pages(buf, pages);
}

UINT32 AudioRead(int i, INT16 *frames, UINT32 n)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    UINT32 done = 0;
    if (s && s->capture) {
        if (n > s->queued) n = s->queued;
        while (done < n) {
            UINT32 m = n - done, tail = s->cap - s->rd;
            if (m > tail) m = tail;
            memcpy(frames + (size_t)done * 2, s->buf + (size_t)s->rd * 2, (size_t)m * FRAME);
            s->rd = (s->rd + m) % s->cap;
            done += m;
        }
        s->queued -= done;
        s->consumed += done;
    }
    spin_unlock_irqrestore(&g.lock, st);
    return done;
}

void AudioSetMaster(int capture, UINT32 left, UINT32 right, bool mute)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Master *m = &g.master[capture ? 1 : 0];
    m->l = left > 65536 ? 65536 : left;
    m->r = right > 65536 ? 65536 : right;
    m->mute = mute;
    spin_unlock_irqrestore(&g.lock, st);
}

void AudioGetMaster(int capture, UINT32 *left, UINT32 *right, bool *mute)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Master *m = &g.master[capture ? 1 : 0];
    *left = m->l; *right = m->r; *mute = m->mute;
    spin_unlock_irqrestore(&g.lock, st);
}

UINT32 AudioWrite(int i, const INT16 *frames, UINT32 n)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    UINT32 done = 0;
    if (s) {
        UINT32 room = s->cap - s->queued;
        if (n > room) n = room;
        UINT32 wr = (s->rd + s->queued) % s->cap;
        while (done < n) {
            UINT32 m = n - done, tail = s->cap - wr;
            if (m > tail) m = tail;
            memcpy(s->buf + (size_t)wr * 2, frames + (size_t)done * 2, (size_t)m * FRAME);
            wr = (wr + m) % s->cap;
            done += m;
        }
        s->queued += done;
        s->written += done;
    }
    spin_unlock_irqrestore(&g.lock, st);
    return done;
}

void AudioRun(int i, bool run)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    if (s) { s->running = run; if (s->capture) capture_sync(); }
    spin_unlock_irqrestore(&g.lock, st);
}

void AudioFlush(int i)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    if (s) {
        s->rd = s->queued = 0;
        s->written = s->consumed = s->dropped = 0;
        s->last_end = 0;
    }
    spin_unlock_irqrestore(&g.lock, st);
}

void AudioSetVolume(int i, UINT32 left, UINT32 right)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    if (s) {
        s->vol_l = left > 65536 ? 65536 : left;
        s->vol_r = right > 65536 ? 65536 : right;
    }
    spin_unlock_irqrestore(&g.lock, st);
}

void AudioGetVolume(int i, UINT32 *left, UINT32 *right)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    *left = s ? s->vol_l : 0;
    *right = s ? s->vol_r : 0;
    spin_unlock_irqrestore(&g.lock, st);
}

bool AudioGetStatus(int i, AudioStatus *out)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    if (s && s->capture) {
        out->written = s->written;                    /* recorded */
        out->consumed = s->consumed;                  /* read */
        out->played = s->dropped;                     /* lost to a full stream */
        out->queued = s->queued;
        out->capacity = s->cap;
        out->running = s->running;
        out->latency = TICK_FRAMES;
    } else if (s) {
        UINT64 hw = g.out ? hw_abs() : s->last_end;
        UINT64 ahead = s->last_end > hw ? (s->last_end - hw) / FRAME : 0;
        out->written = s->written;
        out->consumed = s->consumed;
        out->played = s->consumed > ahead ? s->consumed - ahead : 0;
        out->queued = s->queued;
        out->capacity = s->cap;
        out->running = s->running;
        out->latency = LEAD / FRAME;
    }
    spin_unlock_irqrestore(&g.lock, st);
    return s != NULL;
}
