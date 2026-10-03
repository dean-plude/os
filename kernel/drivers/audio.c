/*
 * audio.c — the system mixer
 *
 * Streams are rings of 48 kHz s16 stereo frames.  The mixer thread wakes
 * every tick (10 ms) and, for every attached output, reads how far its
 * device has played its ring and mixes the running streams that play on
 * it into the ring up to LEAD bytes ahead of that.  Outputs are attached
 * by their drivers: the HD Audio card at boot, a USB audio device when it
 * is plugged in.  One is the default: the one attached last, until the
 * user chooses another in Settings (AudioSetDefault); when it leaves, the
 * one before it takes over.  A stream plays on the default unless its
 * program chose a device (AudioRoute: a waveOut device ID, a WASAPI
 * endpoint); a stream whose device leaves goes to the default.  Behind
 * what it mixed, the mixer keeps the rest of each ring silent, so a mixer
 * held up for longer than LEAD (a busy machine) leaves a gap rather than
 * replaying the ring's last lap; an output nothing plays on gets silence
 * the same way (what was mixed for it before the default moved still
 * plays, and then it is quiet, not its ring's last 341 ms over and over).
 * Everything a stream has is guarded by one spinlock; the mixing itself
 * runs under it too (a few thousand frames per tick and output).
 *
 * Capture streams run the other way: inputs are attached like outputs
 * (the HD Audio card's microphone or line in at boot, a USB microphone
 * when it is plugged in), with a default chosen the same way, and a
 * stream records from the default or the input its program chose.  While
 * any running capture stream records from an input, that input is
 * recording into its ring, and every tick the new frames are copied into
 * each of those streams (the oldest dropped when a stream is full).  Each
 * direction has a master volume and mute (the endpoint volume programs
 * set through IAudioEndpointVolume).
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
    UINT32  dev;                                      /* the device chosen for it (0: the default) */
    UINT32  mixed_on;                                 /* playback: the output its last frames went to */
    UINT64  last_end;                                 /* that output's absolute byte position after them */
    UINT32  vol_l, vol_r;                             /* 0..65536 */
    UINT64  dropped;                                  /* capture: frames lost to a full stream */
} Stream;

#define MAX_OUTPUTS   4

typedef struct { UINT32 l, r; bool mute; } Master;

/* An attached output: where its device is, as an absolute byte count,
 * how far ahead of that its ring has been mixed, and silenced */
typedef struct {
    const AudioOutput *o;
    UINT32 id, rank;                                  /* (rank: when it was attached or chosen; the highest is the default) */
    UINT32 last_pos;
    UINT64 base;                                      /* absolute byte count at the ring's start */
    UINT64 write;                                     /* mixed up to here */
    UINT64 clear;                                     /* and the ring silenced up to here */
} Out;

/* An attached input: whether it records, and how far the mixer has read */
typedef struct {
    const AudioInput *i;
    UINT32 id, rank;
    UINT32 cpos;
    bool   run;
} In;

static struct {
    KSpinLock  lock;
    Stream     s[MAX_STREAMS];
    Out        outs[MAX_OUTPUTS];                     /* attached, oldest first */
    int        nouts;
    In         ins[MAX_OUTPUTS];                      /* attached, oldest first */
    int        nins;
    UINT32     next_id, next_rank;
    INT32      acc[CHUNK_FRAMES * 2];
    Master     master[2];                             /* render, capture */
} g = { .lock = KSPINLOCK_INIT, .next_id = 1, .master = { { 65536, 65536, false }, { 65536, 65536, false } } };

/* Output @k's device position as an absolute byte count (lock held) */
static UINT64 hw_abs(Out *t)
{
    UINT32 pos = t->o->position(t->o->ctx);
    if (pos < t->last_pos) t->base += t->o->bytes;
    t->last_pos = pos;
    return t->base + pos;
}

/* The default output and input (lock held): the one attached or chosen
 * last, so that when it leaves the one that was the default before it
 * takes over; -1 if there is none */
static int def_out(void)
{
    int d = -1;
    for (int k = 0; k < g.nouts; k++)
        if (d < 0 || g.outs[k].rank > g.outs[d].rank) d = k;
    return d;
}

static int def_in(void)
{
    int d = -1;
    for (int k = 0; k < g.nins; k++)
        if (d < 0 || g.ins[k].rank > g.ins[d].rank) d = k;
    return d;
}

/* The output a playback stream plays on (lock held): the one chosen for
 * it while attached, else the default; -1 if there is none */
static int out_of(const Stream *s)
{
    for (int k = 0; s->dev && k < g.nouts; k++)
        if (g.outs[k].id == s->dev) return k;
    return def_out();
}

/* The input a capture stream records from (lock held), the same way */
static int in_of(const Stream *s)
{
    for (int k = 0; s->dev && k < g.nins; k++)
        if (g.ins[k].id == s->dev) return k;
    return def_in();
}

/* Mix @n frames of the running streams that play on output @k into its
 * ring at absolute byte offset @at (lock held) */
static void mix_chunk(int k, UINT64 at, UINT32 n)
{
    Out *t = &g.outs[k];
    memset(g.acc, 0, n * 2 * sizeof(INT32));
    Master *mr = &g.master[0];
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = &g.s[i];
        if (!s->used || s->capture || !s->running || !s->queued || out_of(s) != k) continue;
        UINT32 m = s->queued < n ? s->queued : n;
        for (UINT32 f = 0; f < m; f++) {
            const INT16 *src = s->buf + (size_t)s->rd * 2;
            g.acc[f * 2]     += (INT32)(((INT64)src[0] * s->vol_l) >> 16);
            g.acc[f * 2 + 1] += (INT32)(((INT64)src[1] * s->vol_r) >> 16);
            if (++s->rd == s->cap) s->rd = 0;
        }
        s->queued -= m;
        s->consumed += m;
        s->mixed_on = t->id;
        s->last_end = at + (UINT64)m * FRAME;
    }
    INT16 *dst = (INT16 *)((UINT8 *)t->o->ring + at % t->o->bytes);
    for (UINT32 j = 0; j < n * 2; j++) {
        INT32 v = mr->mute ? 0 : (INT32)(((INT64)g.acc[j] * (j & 1 ? mr->r : mr->l)) >> 16);
        dst[j] = (INT16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* Every output: mix its streams LEAD bytes ahead of its device (an output
 * nothing plays on gets silence: it still streams its ring), then silence
 * the rest of its ring up to where the device is, which holds the last
 * lap, so that if the mixer is held up for longer than LEAD the device
 * plays a gap, not what it played a lap ago */
static void mix_ahead(void)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    for (int k = 0; k < g.nouts; k++) {
        Out *t = &g.outs[k];
        UINT32 bytes = t->o->bytes;
        UINT64 hw = hw_abs(t);
        if (t->write < hw) t->write = hw;             /* fell behind: skip what was missed */
        UINT64 target = hw + LEAD;
        while (t->write < target) {
            UINT32 off = (UINT32)(t->write % bytes);
            UINT64 n = (target - t->write) / FRAME;
            UINT32 room = (bytes - off) / FRAME;
            if (n > room) n = room;
            if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
            if (!n) break;
            mix_chunk(k, t->write, (UINT32)n);
            t->write += n * FRAME;
        }
        if (t->clear < t->write) t->clear = t->write;
        UINT64 lap = hw + bytes;
        while (t->clear < lap) {
            UINT32 off = (UINT32)(t->clear % bytes);
            UINT64 n = lap - t->clear;
            if (n > bytes - off) n = bytes - off;
            memset((UINT8 *)t->o->ring + off, 0, (size_t)n);
            t->clear += n;
        }
    }
    spin_unlock_irqrestore(&g.lock, st);
}

/* Start or stop each input to match the running capture streams that
 * record from it (lock held) */
static void capture_sync(void)
{
    for (int k = 0; k < g.nins; k++) {
        In *t = &g.ins[k];
        bool want = false;
        for (int i = 0; i < MAX_STREAMS && !want; i++)
            want = g.s[i].used && g.s[i].capture && g.s[i].running && in_of(&g.s[i]) == k;
        if (want == t->run) continue;
        t->run = want;
        if (t->i->run) t->i->run(t->i->ctx, want);
        if (want) t->cpos = t->i->position(t->i->ctx);   /* reads on from here */
    }
}

/* Copy what each recording input recorded since the last tick into the
 * running capture streams that record from it */
static void pull_capture(void)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Master *mc = &g.master[1];
    for (int k = 0; k < g.nins; k++) {
        In *t = &g.ins[k];
        const AudioInput *in = t->i;
        if (!t->run) continue;
        UINT32 pos = in->position(in->ctx);
        while (t->cpos != pos) {
            UINT32 end = pos > t->cpos ? pos : in->bytes;         /* up to the wrap first */
            UINT32 n = (end - t->cpos) / FRAME;
            const INT16 *src = (const INT16 *)((UINT8 *)in->ring + t->cpos);
            for (int i = 0; i < MAX_STREAMS; i++) {
                Stream *s = &g.s[i];
                if (!s->used || !s->capture || !s->running || in_of(s) != k) continue;
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
            t->cpos = end == in->bytes ? 0 : end;
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

bool AudioOutputAttach(const AudioOutput *o)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool ok = g.nouts < MAX_OUTPUTS;
    if (ok) {
        Out *t = &g.outs[g.nouts++];
        UINT32 pos = o->position(o->ctx);
        *t = (Out){ .o = o, .id = g.next_id++, .rank = ++g.next_rank, .last_pos = pos, .write = pos, .clear = pos };
    }
    spin_unlock_irqrestore(&g.lock, st);
    if (ok) kprintf("[AUDIO] Playing on %s\n", o->name);
    return ok;
}

/* Remove entry @k of @arr (@n entries of @size bytes) */
static void remove_at(void *arr, int *n, int k, size_t size)
{
    UINT8 *a = arr;
    memmove(a + (size_t)k * size, a + (size_t)(k + 1) * size, (size_t)(*n - k - 1) * size);
    (*n)--;
}

void AudioOutputDetach(const AudioOutput *o)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool found = false, was_default = false;
    for (int k = 0; k < g.nouts && !found; k++) {
        if (g.outs[k].o != o) continue;
        was_default = k == def_out();
        remove_at(g.outs, &g.nouts, k, sizeof(Out));
        found = true;
    }
    int d = def_out();
    const AudioOutput *now = d >= 0 ? g.outs[d].o : NULL;
    spin_unlock_irqrestore(&g.lock, st);
    if (found && was_default && now) kprintf("[AUDIO] Playing on %s\n", now->name);
    else if (found && !now) kprintf("[AUDIO] No sound output left\n");
}

bool AudioInputAttach(const AudioInput *i)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool ok = g.nins < MAX_OUTPUTS;
    if (ok) {
        g.ins[g.nins++] = (In){ .i = i, .id = g.next_id++, .rank = ++g.next_rank };
        capture_sync();                               /* (default streams move to it) */
    }
    spin_unlock_irqrestore(&g.lock, st);
    if (ok) kprintf("[AUDIO] Recording from %s\n", i->name);
    return ok;
}

void AudioInputDetach(const AudioInput *i)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool found = false, was_default = false;
    for (int k = 0; k < g.nins && !found; k++) {
        if (g.ins[k].i != i) continue;               /* (gone: not stopped through it) */
        was_default = k == def_in();
        remove_at(g.ins, &g.nins, k, sizeof(In));
        found = true;
    }
    if (found) capture_sync();                        /* its streams move to the default */
    int d = def_in();
    const AudioInput *now = d >= 0 ? g.ins[d].i : NULL;
    spin_unlock_irqrestore(&g.lock, st);
    if (found && was_default && now) kprintf("[AUDIO] Recording from %s\n", now->name);
    else if (found && !now) kprintf("[AUDIO] No sound input left\n");
}

int AudioDevices(bool capture, AudioDevice *out, int max)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    int n = capture ? g.nins : g.nouts, d = capture ? def_in() : def_out();
    for (int k = 0; k < n && k < max; k++) {
        out[k].id = capture ? g.ins[k].id : g.outs[k].id;
        out[k].is_default = k == d;
        strncpy(out[k].name, capture ? g.ins[k].i->name : g.outs[k].o->name, sizeof(out[k].name) - 1);
        out[k].name[sizeof(out[k].name) - 1] = 0;
    }
    spin_unlock_irqrestore(&g.lock, st);
    return n < max ? n : max;
}

bool AudioSetDefault(bool capture, UINT32 id)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    bool found = false;
    const char *name = NULL;
    if (capture) {
        for (int k = 0; k < g.nins && !found; k++) {
            if (g.ins[k].id != id) continue;
            g.ins[k].rank = ++g.next_rank;
            name = g.ins[k].i->name;
            found = true;
        }
        if (found) capture_sync();
    } else {
        for (int k = 0; k < g.nouts && !found; k++) {
            if (g.outs[k].id != id) continue;
            g.outs[k].rank = ++g.next_rank;
            name = g.outs[k].o->name;
            found = true;
        }
    }
    spin_unlock_irqrestore(&g.lock, st);
    if (found) kprintf(capture ? "[AUDIO] Recording from %s (chosen)\n" : "[AUDIO] Playing on %s (chosen)\n", name);
    return found;
}

static UINT32 hda_position(void *ctx) { (void)ctx; return HdaPosition(); }
static AudioOutput g_hda_out = { .position = hda_position };
static UINT32 hda_cposition(void *ctx) { (void)ctx; return HdaCapturePosition(); }
static void hda_crun(void *ctx, bool on) { (void)ctx; HdaCapture(on); }
static AudioInput g_hda_in = { .position = hda_cposition, .run = hda_crun };
static char g_hda_in_name[96];

bool AudioInit(void)
{
    if (!sched_create_thread("audio", mixer_thread, NULL, PRIO_DEVICE)) {   /* (above programs and the desktop) */
        kprintf("[AUDIO] Could not start the mixer\n");
        return false;
    }
    if (!HdaInit()) return false;
    g_hda_out.name = HdaName();
    g_hda_out.ring = HdaRing(&g_hda_out.bytes);
    AudioOutputAttach(&g_hda_out);
    if (HdaCanRecord()) {
        ksnprintf(g_hda_in_name, sizeof(g_hda_in_name), "%s (%s)", HdaInputName(), HdaName());
        g_hda_in.name = g_hda_in_name;
        g_hda_in.ring = HdaCaptureRing(&g_hda_in.bytes);
        AudioInputAttach(&g_hda_in);
    }
    return true;
}

bool        AudioPresent(void)    { return g.nouts > 0; }
bool        AudioCanRecord(void)  { return g.nins > 0; }

/* (the name stays valid while the device is attached, as before) */
const char *AudioInputName(void)
{
    int d = def_in();
    return d >= 0 ? g.ins[d].i->name : "";
}

const char *AudioDeviceName(void)
{
    int d = def_out();
    return d >= 0 ? g.outs[d].o->name : "";
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

bool AudioRoute(int i, UINT32 id)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    bool ok = s && !id;
    for (int k = 0; s && id && !ok && k < (s->capture ? g.nins : g.nouts); k++)
        ok = (s->capture ? g.ins[k].id : g.outs[k].id) == id;
    if (ok) {
        s->dev = id;
        if (s->capture) capture_sync();
    }
    spin_unlock_irqrestore(&g.lock, st);
    return ok;
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
        UINT64 ahead = 0;
        for (int k = 0; k < g.nouts; k++) {
            if (g.outs[k].id != s->mixed_on) continue;
            UINT64 hw = hw_abs(&g.outs[k]);
            ahead = s->last_end > hw ? (s->last_end - hw) / FRAME : 0;
        }
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
