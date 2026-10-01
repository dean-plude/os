/*
 * audio.c — the system mixer
 *
 * Streams are rings of 48 kHz s16 stereo frames.  The mixer thread wakes
 * every tick (10 ms), reads how far the hardware has played the DMA ring,
 * and mixes the running streams into the ring up to LEAD bytes ahead of
 * that.  Everything a stream has is guarded by one spinlock; the mixing
 * itself runs under it too (a few thousand frames per tick).
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

typedef struct {
    bool    used, running;
    INT16  *buf;
    UINT32  pages;
    UINT32  cap, rd, queued;                          /* frames */
    UINT64  written, consumed;
    UINT64  last_end;                                 /* ring byte position (absolute) after its last mixed frame */
    UINT32  vol_l, vol_r;                             /* 0..65536 */
} Stream;

static struct {
    bool       present;
    KSpinLock  lock;
    Stream     s[MAX_STREAMS];
    INT16     *ring;
    UINT32     ring_bytes;
    UINT32     last_pos;
    UINT64     hw_base;                               /* absolute byte count at the ring's start */
    UINT64     write_abs;                             /* mixed up to here */
    INT32      acc[CHUNK_FRAMES * 2];
} g = { .lock = KSPINLOCK_INIT };

/* The hardware position as an absolute byte count (lock held) */
static UINT64 hw_abs(void)
{
    UINT32 pos = HdaPosition();
    if (pos < g.last_pos) g.hw_base += g.ring_bytes;
    g.last_pos = pos;
    return g.hw_base + pos;
}

/* Mix @n frames into the ring at absolute byte offset @at (lock held) */
static void mix_chunk(UINT64 at, UINT32 n)
{
    memset(g.acc, 0, n * 2 * sizeof(INT32));
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *s = &g.s[i];
        if (!s->used || !s->running || !s->queued) continue;
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
        INT32 v = g.acc[k];
        dst[k] = (INT16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static void mix_ahead(void)
{
    IrqState st = spin_lock_irqsave(&g.lock);
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

static void mixer_thread(void *arg)
{
    (void)arg;
    bkl_release();                                    /* runs under its own lock */
    for (;;) {
        sched_sleep_tick();
        mix_ahead();
    }
}

bool AudioInit(void)
{
    if (!HdaInit()) return false;
    g.ring = HdaRing(&g.ring_bytes);
    g.present = true;
    if (!sched_create_thread("audio", mixer_thread, NULL, 12)) {
        kprintf("[AUDIO] Could not start the mixer\n");
        g.present = false;
        return false;
    }
    return true;
}

bool        AudioPresent(void)    { return g.present; }
const char *AudioDeviceName(void) { return HdaName(); }

int AudioOpen(UINT32 frames)
{
    if (!g.present) return -1;
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
    if (s) s->used = false;
    spin_unlock_irqrestore(&g.lock, st);
    if (buf) kernel_free_pages(buf, pages);
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
    if (s) s->running = run;
    spin_unlock_irqrestore(&g.lock, st);
}

void AudioFlush(int i)
{
    IrqState st = spin_lock_irqsave(&g.lock);
    Stream *s = get(i);
    if (s) {
        s->rd = s->queued = 0;
        s->written = s->consumed = 0;
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
    if (s) {
        UINT64 hw = hw_abs();
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
