/*
 * midi.c — MIDI output, MIDI streams and the MCI sequencer, on TinySoundFont
 *
 * One synthesizer per process: TinySoundFont (third_party/tinysoundfont)
 * playing the General MIDI soundfont C:\Windows\System32\drivers\gm.sf2
 * (tools/make_gm_soundfont.py; another .sf2 can take its place), rendered
 * by a thread into a kernel mixer stream (48 kHz s16 stereo) about 40 ms
 * ahead.  Every open device shares it, as programs share Windows' GS
 * Wavetable Synth.
 *
 * - midiOut: short messages and system exclusive (GM, GS and XG resets,
 *   master volume) go straight to the synthesizer.
 * - midiStream: buffers of MIDIEVENTs, timed by the stream's tempo and
 *   time division, played by the synthesizer thread a millisecond at a
 *   time; MOM_DONE and MOM_POSITIONCB are delivered after it lets go of
 *   its lock.
 * - MCI "sequencer" devices (mciSendString and mciSendCommand): a MIDI
 *   file parsed by TinySoundFont's tml.h, played the same way.
 */

#include <windows.h>
#include <avrt.h>
#include <winternl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mmwave.h"

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
#include "tsf.h"
#define TML_IMPLEMENTATION
#define TML_NO_STDIO
#include "tml.h"

#define RATE      48000
#define AHEAD     1920                  /* frames kept queued (40 ms) */
#define STEP      48                    /* frames per sequencer step (1 ms) */

#define MIDI_MAPPER       ((UINT)-1)
#define MIDIERR_UNPREPARED   64
#define MIDIERR_STILLPLAYING 65
#define MIDIERR_NOTREADY     67
#define MOM_OPEN          0x3C7
#define MOM_CLOSE         0x3C8
#define MOM_DONE          0x3C9
#define MOM_POSITIONCB    0x3CA
#define MHDR_DONE         0x01
#define MHDR_PREPARED     0x02
#define MHDR_INQUEUE      0x04
#define MHDR_ISSTRM       0x08
#define MEVT_F_LONG       0x80000000
#define MEVT_F_CALLBACK   0x40000000
#define MEVT_SHORTMSG     0x00
#define MEVT_TEMPO        0x01
#define MIDIPROP_SET      0x80000000
#define MIDIPROP_GET      0x40000000
#define MIDIPROP_TIMEDIV  0x00000001
#define MIDIPROP_TEMPO    0x00000002
#define TIME_TICKS        0x20

typedef struct midihdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength, dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags;
    struct midihdr_tag *lpNext;
    DWORD_PTR reserved;
    DWORD dwOffset;
    DWORD_PTR dwReserved[8];
} MIDIHDR;

typedef struct { WORD wMid, wPid; UINT vDriverVersion; WCHAR szPname[32]; WORD wTechnology, wVoices, wNotes, wChannelMask; DWORD dwSupport; } MIDIOUTCAPSW;
typedef struct { WORD wMid, wPid; UINT vDriverVersion; CHAR szPname[32]; WORD wTechnology, wVoices, wNotes, wChannelMask; DWORD dwSupport; } MIDIOUTCAPSA;

/* -----------------------------------------------------------------------
 * Objects
 * ----------------------------------------------------------------------- */
#define MIDI_MAGIC 0x4D494449           /* "MIDI" */

typedef struct MidiOut MidiOut;
struct MidiOut {
    DWORD      magic;
    DWORD      cbtype;
    DWORD_PTR  cb, inst;
    BOOL       stream;
    /* midiStream */
    BOOL       playing;
    MIDIHDR   *queue;                   /* buffers in order (lpNext) */
    DWORD      offset;                  /* bytes into queue's first buffer */
    BOOL       have_event;              /* ev_at is set for the event at offset */
    double     now, ev_at;              /* ticks */
    DWORD      tempo, timediv;          /* microseconds per quarter note; ticks per quarter (or SMPTE) */
    ULONGLONG  ms;                      /* milliseconds played */
    DWORD      volume;
    MidiOut   *next;
};

typedef struct Player Player;           /* an open MCI sequencer */
struct Player {
    UINT          id;
    WCHAR         alias[64];
    tml_message  *msgs, *cur;
    DWORD         pos, length, to;      /* ms */
    int           mode;                 /* 0 stopped, 1 playing, 2 paused */
    HWND          notify;
    HANDLE        done;                 /* set when playing stops ("wait") */
    BOOL          ms_format;
    Player       *next;
};
enum { STOPPED, PLAYING, PAUSED };

typedef struct { MidiOut *m; UINT msg; DWORD_PTR p1; HWND wnd; UINT id; } Note;

static struct {
    CRITICAL_SECTION lock;
    BOOL       lock_ready;
    LONG       users;
    tsf       *f;
    INT_PTR    stream;
    HANDLE     thread, quit;
    MidiOut   *outs;
    Player    *players;
    UINT       next_id;
    Note       notes[64];
    UINT       nnotes;
    float      volume;
} g;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_lock(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g.lock);
    g.next_id = 1;
    g.volume = 1.0f;
    return TRUE;
}
static void lock(void)   { InitOnceExecuteOnce(&g_once, init_lock, 0, 0); EnterCriticalSection(&g.lock); }
static void unlock(void) { LeaveCriticalSection(&g.lock); }

/* -----------------------------------------------------------------------
 * The synthesizer
 * ----------------------------------------------------------------------- */
static BOOL audio_present(void)
{
    struct { UINT32 present, rate; char name[96]; } info = { 0 };
    return NtNovaAudioCtl(0, 5, 0, &info) == 0 && info.present;
}

static void font_path(WCHAR *p, UINT n)
{
    UINT k = GetSystemDirectoryW(p, n);            /* (SysWOW64 has a copy for 32-bit programs) */
    if (k + 20 < n) lstrcpyW(p + k, L"\\drivers\\gm.sf2");
}

static BOOL synth_present(void)
{
    WCHAR p[MAX_PATH];
    font_path(p, MAX_PATH);
    return audio_present() && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void gm_reset(void)
{
    tsf_reset(g.f);
    for (int c = 0; c < 16; c++) {
        tsf_channel_set_presetnumber(g.f, c, 0, c == 9);
        tsf_channel_set_volume(g.f, c, 100 / 127.0f * (100 / 127.0f));
        tsf_channel_set_pan(g.f, c, 0.5f);
        tsf_channel_set_pitchrange(g.f, c, 2.0f);
    }
}

/* A channel message (status, data) on the synthesizer (locked) */
static void apply(DWORD m)
{
    BYTE st = (BYTE)m, d1 = (BYTE)(m >> 8) & 0x7F, d2 = (BYTE)(m >> 16) & 0x7F;
    int ch = st & 15;
    switch (st & 0xF0) {
    case 0x80: tsf_channel_note_off(g.f, ch, d1); break;
    case 0x90: if (d2) tsf_channel_note_on(g.f, ch, d1, d2 / 127.0f); else tsf_channel_note_off(g.f, ch, d1); break;
    case 0xB0: tsf_channel_midi_control(g.f, ch, d1, d2); break;
    case 0xC0: tsf_channel_set_presetnumber(g.f, ch, d1, ch == 9); break;
    case 0xE0: tsf_channel_set_pitchwheel(g.f, ch, d1 | d2 << 7); break;
    case 0xF0: if (st == 0xFF) gm_reset(); break;              /* system reset */
    }
}

/* System exclusive: GM/GS/XG resets and master volume */
static void sysex(const BYTE *p, DWORD n)
{
    static const BYTE gm_on[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    static const BYTE gs_reset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    static const BYTE xg_on[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    if ((n >= sizeof gm_on && !memcmp(p, gm_on, sizeof gm_on)) || (n >= sizeof gs_reset && !memcmp(p, gs_reset, sizeof gs_reset)) ||
        (n >= sizeof xg_on && !memcmp(p, xg_on, sizeof xg_on)))
        gm_reset();
    else if (n >= 8 && p[0] == 0xF0 && p[1] == 0x7F && p[3] == 0x04 && p[4] == 0x01)   /* master volume */
        tsf_set_volume(g.f, (p[5] | p[6] << 7) / 16383.0f * g.volume);
}

static void post(MidiOut *m, UINT msg, DWORD_PTR p1)
{
    if (g.nnotes < 64) g.notes[g.nnotes++] = (Note){ m, msg, p1, 0, 0 };
}

static void deliver(Note *n)
{
    if (n->wnd) {                               /* MCI: MM_MCINOTIFY, successful */
        mm_post(FALSE, (DWORD_PTR)n->wnd, 0x3B9, 1, n->id);
        return;
    }
    MidiOut *m = n->m;
    switch (m->cbtype) {
    case CALLBACK_FUNCTION:
        if (m->cb) ((void (CALLBACK *)(HANDLE, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR))m->cb)((HANDLE)m, n->msg, m->inst, n->p1, 0);
        break;
    case CALLBACK_WINDOW: mm_post(FALSE, m->cb, n->msg, (WPARAM)m, (LPARAM)n->p1); break;
    case CALLBACK_THREAD: mm_post(TRUE, m->cb, n->msg, (WPARAM)m, (LPARAM)n->p1); break;
    case CALLBACK_EVENT:  SetEvent((HANDLE)m->cb); break;
    }
}

/* Hand out the notifications collected under the lock */
static void flush_notes(void)
{
    Note n[64];
    lock();
    UINT k = g.nnotes;
    memcpy(n, g.notes, k * sizeof(Note));
    g.nnotes = 0;
    unlock();
    for (UINT i = 0; i < k; i++) deliver(&n[i]);
}

static double ticks_per_ms(MidiOut *m)
{
    if (m->timediv & 0x8000) {                  /* SMPTE: frames per second x ticks per frame */
        int fps = -(signed char)(m->timediv >> 8);
        return fps * (m->timediv & 0xFF) / 1000.0;
    }
    return (double)m->timediv * 1000.0 / m->tempo;
}

/* One millisecond of a stream: the events that fall due */
static void stream_step(MidiOut *m)
{
    if (!m->playing || !m->queue) return;
    m->now += ticks_per_ms(m);
    m->ms++;
    while (m->queue) {
        MIDIHDR *h = m->queue;
        if (m->offset + 12 > h->dwBytesRecorded) {            /* this buffer is done */
            m->queue = h->lpNext;
            m->offset = 0;
            h->dwFlags = (h->dwFlags & ~MHDR_INQUEUE) | MHDR_DONE;
            post(m, MOM_DONE, (DWORD_PTR)h);
            continue;
        }
        const DWORD *e = (const DWORD *)(h->lpData + m->offset);
        if (!m->have_event) {
            m->ev_at = (m->ev_at > m->now - e[0] - ticks_per_ms(m) ? m->ev_at : m->now - ticks_per_ms(m)) + e[0];
            m->have_event = TRUE;
        }
        if (m->ev_at > m->now) break;
        DWORD ev = e[2], len = 12;
        if (ev & MEVT_F_LONG) {
            DWORD n = ev & 0xFFFFFF;
            if ((ev >> 24 & 0x7F) == 0x00 /* MEVT_LONGMSG */) sysex((const BYTE *)(e + 3), n);
            len += (n + 3) & ~3u;
        } else if ((ev >> 24 & 0x3F) == MEVT_SHORTMSG) {
            apply(ev & 0xFFFFFF);
        } else if ((ev >> 24 & 0x3F) == MEVT_TEMPO) {
            if (ev & 0xFFFFFF) m->tempo = ev & 0xFFFFFF;
        }
        if (ev & MEVT_F_CALLBACK) {
            h->dwOffset = m->offset;
            post(m, MOM_POSITIONCB, (DWORD_PTR)h);
        }
        m->offset += len;
        m->have_event = FALSE;
    }
}

static void stop_notes(void)
{
    for (int c = 0; c < 16; c++) {
        tsf_channel_midi_control(g.f, c, 64, 0);              /* sustain off */
        tsf_channel_note_off_all(g.f, c);
    }
}

/* One millisecond of an MCI sequencer */
static void player_step(Player *p)
{
    if (p->mode != PLAYING) return;
    p->pos++;
    while (p->cur && p->cur->time <= p->pos) {
        tml_message *t = p->cur;
        if (t->type == TML_PITCH_BEND) apply(0xE0 | t->channel | (t->pitch_bend & 0x7F) << 8 | (t->pitch_bend >> 7 & 0x7F) << 16);
        else if (t->type >= 0x80 && t->type < 0xF0) apply(t->type | t->channel | (BYTE)t->key << 8 | (BYTE)t->velocity << 16);
        p->cur = t->next;
    }
    if (!p->cur || p->pos >= p->to) {
        p->mode = STOPPED;
        stop_notes();
        if (p->notify && g.nnotes < 64) g.notes[g.nnotes++] = (Note){ 0, 0, 0, p->notify, p->id };
        p->notify = 0;
        if (p->done) SetEvent(p->done);
    }
}

static DWORD WINAPI synth_thread(LPVOID unused)
{
    (void)unused;
    static SHORT out[AHEAD * 2];
    SetThreadPriority(GetCurrentThread(), 15);
    DWORD mm = 0;
    AvSetMmThreadCharacteristicsW(L"Audio", &mm);   /* (MMCSS: above any busy or boosted program thread) */
    do {
        StreamStatus st;
        if (NtNovaAudioCtl(g.stream, 0, 0, &st)) continue;
        UINT need = st.queued < AHEAD ? (AHEAD - st.queued) / STEP * STEP : 0;
        if (!need) continue;
        lock();
        BOOL busy = tsf_active_voice_count(g.f) > 0;
        for (MidiOut *m = g.outs; m && !busy; m = m->next) busy = m->playing && m->queue;
        for (Player *p = g.players; p && !busy; p = p->next) busy = p->mode == PLAYING;
        if (busy)
            for (UINT k = 0; k < need; k += STEP) {
                for (MidiOut *m = g.outs; m; m = m->next) stream_step(m);
                for (Player *p = g.players; p; p = p->next) player_step(p);
                tsf_render_short(g.f, out + k * 2, STEP, 0);
            }
        unlock();
        flush_notes();
        if (!busy) continue;                    /* silence: let the stream drain */
        UINT done = 0;
        while (done < need) {
            LONG_PTR got = NtNovaAudioWrite(g.stream, out + done * 2, need - done);
            if (got <= 0) break;
            done += (UINT)got;
        }
    } while (WaitForSingleObject(g.quit, 5) == WAIT_TIMEOUT);
    return 0;
}

/* Start the synthesizer for one more user (locked) */
static MMRESULT synth_get(void)
{
    if (g.users++) return MMSYSERR_NOERROR;
    WCHAR path[MAX_PATH];
    font_path(path, MAX_PATH);
    HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD size = fh != INVALID_HANDLE_VALUE ? GetFileSize(fh, 0) : 0, got = 0;
    void *buf = size && size != INVALID_FILE_SIZE ? malloc(size) : 0;
    if (buf && ReadFile(fh, buf, size, &got, 0) && got == size) g.f = tsf_load_memory(buf, (int)size);
    free(buf);
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (g.f) g.stream = NtNovaAudioOpen(AHEAD * 4);
    if (!g.f || !g.stream) {
        if (g.f) tsf_close(g.f);
        g.f = 0;
        g.users--;
        return MMSYSERR_NODRIVER;
    }
    tsf_set_output(g.f, TSF_STEREO_INTERLEAVED, RATE, -3.0f);
    tsf_set_max_voices(g.f, 64);
    gm_reset();
    NtNovaAudioCtl(g.stream, 1, 1, 0);
    g.quit = CreateEventW(0, TRUE, FALSE, 0);
    g.thread = CreateThread(0, 64 * 1024, synth_thread, 0, 0, 0);
    return MMSYSERR_NOERROR;
}

/* ... and one fewer: the last one stops it (not locked: it waits for the thread) */
static void synth_put(void)
{
    lock();
    BOOL last = --g.users == 0;
    unlock();
    if (!last) return;
    SetEvent(g.quit);
    WaitForSingleObject(g.thread, INFINITE);
    CloseHandle(g.thread);
    CloseHandle(g.quit);
    lock();
    if (!g.users) {
        NtClose((HANDLE)g.stream);
        tsf_close(g.f);
        g.f = 0;
        g.stream = 0;
    }
    unlock();
}

/* -----------------------------------------------------------------------
 * midiOut
 * ----------------------------------------------------------------------- */
/* Kept out of line: with -fasync-exceptions, clang 18's instruction selector
 * never finishes a function that inlines this __try together with the
 * synthesizer (midiOutClose, which also inlines tsf_close) */
static __declspec(noinline) MidiOut *get_out(HANDLE h)
{
    MidiOut *m = (MidiOut *)h;
    if (!m) return 0;
    __try { if (m->magic != MIDI_MAGIC) m = 0; } __except (1) { m = 0; }
    return m;
}

MMAPI UINT WINAPI midiOutGetNumDevs(void) { return synth_present() ? 1 : 0; }

static void caps_common(WORD *tech, WORD *voices, WORD *notes, WORD *mask, DWORD *support)
{
    *tech = 7;                                  /* MOD_SWSYNTH */
    *voices = 32;
    *notes = 32;
    *mask = 0xFFFF;
    *support = 0x1 | 0x2 | 0x8;                 /* MIDICAPS_VOLUME | LRVOLUME | STREAM */
}
static const char synth_name[] = "NovaOS Wavetable Synth";

MMAPI MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR dev, MIDIOUTCAPSW *c, UINT n)
{
    if ((dev != 0 && dev != MIDI_MAPPER && !get_out((HANDLE)dev)) || !synth_present()) return MMSYSERR_BADDEVICEID;
    if (!c || n < sizeof(*c)) return MMSYSERR_INVALPARAM;
    memset(c, 0, sizeof(*c));
    c->wMid = 1;
    c->wPid = 102;
    c->vDriverVersion = 0x0100;
    MultiByteToWideChar(CP_ACP, 0, synth_name, -1, c->szPname, 32);
    caps_common(&c->wTechnology, &c->wVoices, &c->wNotes, &c->wChannelMask, &c->dwSupport);
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR dev, MIDIOUTCAPSA *c, UINT n)
{
    MIDIOUTCAPSW w;
    MMRESULT r = midiOutGetDevCapsW(dev, &w, sizeof(w));
    if (r) return r;
    if (!c || n < sizeof(*c)) return MMSYSERR_INVALPARAM;
    memcpy(c, &w, 8);
    WideCharToMultiByte(CP_ACP, 0, w.szPname, -1, c->szPname, 32, 0, 0);
    caps_common(&c->wTechnology, &c->wVoices, &c->wNotes, &c->wChannelMask, &c->dwSupport);
    return MMSYSERR_NOERROR;
}

static MMRESULT open_out(MidiOut **out, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags, BOOL stream)
{
    if (!out) return MMSYSERR_INVALPARAM;
    *out = 0;
    if (dev != 0 && dev != MIDI_MAPPER) return MMSYSERR_BADDEVICEID;
    DWORD cbtype = flags & CALLBACK_TYPEMASK;
    if (cbtype && cbtype != CALLBACK_WINDOW && cbtype != CALLBACK_THREAD && cbtype != CALLBACK_FUNCTION &&
        cbtype != CALLBACK_EVENT) return MMSYSERR_INVALFLAG;
    if (!audio_present()) return MMSYSERR_NODRIVER;
    MidiOut *m = calloc(1, sizeof(*m));
    if (!m) return MMSYSERR_NOMEM;
    m->magic = MIDI_MAGIC;
    m->cbtype = cbtype;
    m->cb = cb;
    m->inst = inst;
    m->stream = stream;
    m->tempo = 500000;
    m->timediv = 480;
    m->volume = 0xFFFFFFFF;
    lock();
    MMRESULT r = synth_get();
    if (!r) {
        m->next = g.outs;
        g.outs = m;
    }
    unlock();
    if (r) { free(m); return r; }
    *out = m;
    Note n = { m, MOM_OPEN, 0, 0, 0 };
    deliver(&n);
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiOutOpen(HANDLE *h, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    return open_out((MidiOut **)h, dev, cb, inst, flags, FALSE);
}

MMAPI MMRESULT WINAPI midiOutClose(HANDLE h)
{
    MidiOut *m = get_out(h);
    if (!m) return MMSYSERR_INVALHANDLE;
    lock();
    if (m->queue) { unlock(); return MIDIERR_STILLPLAYING; }
    MidiOut **pp = &g.outs;
    while (*pp && *pp != m) pp = &(*pp)->next;
    if (*pp) *pp = m->next;
    stop_notes();
    unlock();
    Note n = { m, MOM_CLOSE, 0, 0, 0 };
    deliver(&n);
    m->magic = 0;
    free(m);
    synth_put();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiOutShortMsg(HANDLE h, DWORD msg)
{
    if (!get_out(h)) return MMSYSERR_INVALHANDLE;
    lock();
    apply(msg);
    unlock();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiOutPrepareHeader(HANDLE h, MIDIHDR *hdr, UINT n)
{
    if (!get_out(h)) return MMSYSERR_INVALHANDLE;
    if (!hdr || n < 24 || !hdr->lpData) return MMSYSERR_INVALPARAM;
    hdr->dwFlags = (hdr->dwFlags & ~MHDR_DONE) | MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutUnprepareHeader(HANDLE h, MIDIHDR *hdr, UINT n)
{
    if (!get_out(h)) return MMSYSERR_INVALHANDLE;
    if (!hdr || n < 24) return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;
    hdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiOutLongMsg(HANDLE h, MIDIHDR *hdr, UINT n)
{
    MidiOut *m = get_out(h);
    if (!m) return MMSYSERR_INVALHANDLE;
    if (!hdr || n < 24) return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
    lock();
    sysex((const BYTE *)hdr->lpData, hdr->dwBufferLength);
    unlock();
    hdr->dwFlags |= MHDR_DONE;
    Note note = { m, MOM_DONE, (DWORD_PTR)hdr, 0, 0 };
    deliver(&note);
    return MMSYSERR_NOERROR;
}

/* Return a stream's buffers (locked) */
static void return_buffers(MidiOut *m)
{
    for (MIDIHDR *h = m->queue; h; h = h->lpNext) {
        h->dwFlags = (h->dwFlags & ~MHDR_INQUEUE) | MHDR_DONE;
        post(m, MOM_DONE, (DWORD_PTR)h);
    }
    m->queue = 0;
    m->offset = 0;
    m->have_event = FALSE;
}

MMAPI MMRESULT WINAPI midiOutReset(HANDLE h)
{
    MidiOut *m = get_out(h);
    if (!m) return MMSYSERR_INVALHANDLE;
    lock();
    return_buffers(m);
    for (int c = 0; c < 16; c++) {
        tsf_channel_midi_control(g.f, c, 64, 0);
        tsf_channel_sounds_off_all(g.f, c);
    }
    unlock();
    flush_notes();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiOutSetVolume(HANDLE h, DWORD v)
{
    MidiOut *m = get_out(h);
    if (!m && h) return MMSYSERR_INVALHANDLE;
    lock();
    if (m) m->volume = v;
    g.volume = ((v & 0xFFFF) + (v >> 16)) / 131070.0f;
    if (g.f) tsf_set_volume(g.f, g.volume);
    unlock();
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutGetVolume(HANDLE h, DWORD *v)
{
    MidiOut *m = get_out(h);
    if (!v) return MMSYSERR_INVALPARAM;
    *v = m ? m->volume : (DWORD)(g.volume * 65535) * 0x10001;
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutGetID(HANDLE h, UINT *id)
{
    if (!get_out(h)) return MMSYSERR_INVALHANDLE;
    if (!id) return MMSYSERR_INVALPARAM;
    *id = 0;
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutMessage(HANDLE h, UINT msg, DWORD_PTR a, DWORD_PTR b)
{
    (void)msg; (void)a; (void)b;
    return get_out(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE;
}
MMAPI MMRESULT WINAPI midiOutCachePatches(HANDLE h, UINT bank, WORD *patches, UINT flags)
{ (void)bank; (void)patches; (void)flags; return get_out(h) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiOutCacheDrumPatches(HANDLE h, UINT patch, WORD *keys, UINT flags)
{ (void)patch; (void)keys; (void)flags; return get_out(h) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE; }

static const char *midi_error(MMRESULT e)
{
    switch (e) {
    case MIDIERR_UNPREPARED:   return "The header is not prepared.";
    case MIDIERR_STILLPLAYING: return "Buffers are still queued.";
    case MIDIERR_NOTREADY:     return "The MIDI device is busy.";
    default:                   return mm_error_text(e);
    }
}
MMAPI MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT e, LPWSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    MultiByteToWideChar(CP_ACP, 0, midi_error(e), -1, buf, (int)n);
    buf[n - 1] = 0;
    return MMSYSERR_NOERROR;
}
MMAPI MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT e, LPSTR buf, UINT n)
{
    if (!buf || !n) return MMSYSERR_INVALPARAM;
    lstrcpynA(buf, midi_error(e), (int)n);
    return MMSYSERR_NOERROR;
}

/* No MIDI input ports */
MMAPI UINT WINAPI midiInGetNumDevs(void) { return 0; }
MMAPI MMRESULT WINAPI midiInOpen(HANDLE *h, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)dev; (void)cb; (void)inst; (void)flags; if (h) *h = 0; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR dev, void *caps, UINT n) { (void)dev; (void)caps; (void)n; return MMSYSERR_BADDEVICEID; }
MMAPI MMRESULT WINAPI midiInClose(HANDLE h) { (void)h; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInStart(HANDLE h) { (void)h; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInStop(HANDLE h) { (void)h; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInReset(HANDLE h) { (void)h; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInAddBuffer(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInPrepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInUnprepareHeader(HANDLE h, void *hdr, UINT n) { (void)h; (void)hdr; (void)n; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiInMessage(HANDLE h, UINT msg, DWORD_PTR a, DWORD_PTR b) { (void)h; (void)msg; (void)a; (void)b; return MMSYSERR_INVALHANDLE; }
MMAPI MMRESULT WINAPI midiConnect(HANDLE a, HANDLE b, void *r) { (void)a; (void)b; (void)r; return MMSYSERR_NOTSUPPORTED; }
MMAPI MMRESULT WINAPI midiDisconnect(HANDLE a, HANDLE b, void *r) { (void)a; (void)b; (void)r; return MMSYSERR_NOTSUPPORTED; }

/* -----------------------------------------------------------------------
 * midiStream
 * ----------------------------------------------------------------------- */
MMAPI MMRESULT WINAPI midiStreamOpen(HANDLE *h, UINT *dev, DWORD n, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    if (!h || n != 1) return MMSYSERR_INVALPARAM;
    UINT id = dev ? *dev : 0;
    MMRESULT r = open_out((MidiOut **)h, id, cb, inst, flags, TRUE);
    if (!r && dev) *dev = 0;
    return r;
}

MMAPI MMRESULT WINAPI midiStreamClose(HANDLE h)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    lock();
    m->playing = FALSE;
    return_buffers(m);
    unlock();
    flush_notes();
    return midiOutClose(h);
}

MMAPI MMRESULT WINAPI midiStreamOut(HANDLE h, MIDIHDR *hdr, UINT n)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    if (!hdr || n < 24 || !hdr->lpData || hdr->dwBytesRecorded > hdr->dwBufferLength) return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
    if (hdr->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;
    lock();
    hdr->dwFlags = (hdr->dwFlags & ~MHDR_DONE) | MHDR_INQUEUE | MHDR_ISSTRM;
    hdr->lpNext = 0;
    hdr->dwOffset = 0;
    MIDIHDR **pp = &m->queue;
    while (*pp) pp = &(*pp)->lpNext;
    *pp = hdr;
    unlock();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiStreamRestart(HANDLE h)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    lock();
    m->playing = TRUE;
    unlock();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiStreamPause(HANDLE h)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    lock();
    m->playing = FALSE;
    stop_notes();
    unlock();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiStreamStop(HANDLE h)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    lock();
    m->playing = FALSE;
    return_buffers(m);
    stop_notes();
    m->now = m->ev_at = 0;
    m->ms = 0;
    unlock();
    flush_notes();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiStreamPosition(HANDLE h, MMTIME *t, UINT n)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    if (!t || n < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
    lock();
    StreamStatus st;
    UINT32 lag = NtNovaAudioCtl(g.stream, 0, 0, &st) == 0 ? st.queued * 1000 / RATE : 0;
    ULONGLONG ms = m->ms > lag ? m->ms - lag : 0;
    if (t->wType == TIME_TICKS) {
        double ticks = m->now - lag * ticks_per_ms(m);
        t->u.ticks = ticks > 0 ? (DWORD)ticks : 0;
    } else {
        t->wType = TIME_MS;
        t->u.ms = (DWORD)ms;
    }
    unlock();
    return MMSYSERR_NOERROR;
}

MMAPI MMRESULT WINAPI midiStreamProperty(HANDLE h, BYTE *data, DWORD flags)
{
    MidiOut *m = get_out(h);
    if (!m || !m->stream) return MMSYSERR_INVALHANDLE;
    if (!data || !(flags & (MIDIPROP_GET | MIDIPROP_SET))) return MMSYSERR_INVALPARAM;
    DWORD *p = (DWORD *)data;                   /* { cbStruct, value } */
    DWORD *field = (flags & MIDIPROP_TEMPO) ? &m->tempo : (flags & MIDIPROP_TIMEDIV) ? &m->timediv : 0;
    if (!field) return MMSYSERR_INVALPARAM;
    lock();
    if (flags & MIDIPROP_SET) { if (p[1]) *field = p[1]; }
    else p[1] = *field;
    unlock();
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * MCI: the "sequencer" device
 * ----------------------------------------------------------------------- */
#define MCIERR_BASE                256
#define MCIERR_INVALID_DEVICE_ID   257
#define MCIERR_UNRECOGNIZED_KEYWORD 259
#define MCIERR_UNRECOGNIZED_COMMAND 261
#define MCIERR_HARDWARE            262
#define MCIERR_INVALID_DEVICE_NAME 263
#define MCIERR_OUT_OF_MEMORY       264
#define MCIERR_CANNOT_LOAD_DRIVER  266
#define MCIERR_MISSING_PARAMETER   273
#define MCIERR_DEVICE_NOT_INSTALLED 275
#define MCIERR_FILE_NOT_FOUND      275 + 0   /* (same text family; below) */
#define MCIERR_INVALID_FILE        304
#define MCIERR_SEQ_NOMIDIPRESENT   (MCIERR_BASE + 87)
#define MCIERR_DUPLICATE_ALIAS     289
#define MCIERR_NO_ELEMENT_ALLOWED  (MCIERR_BASE + 45)

static Player *find_player(const WCHAR *name, UINT id)
{
    for (Player *p = g.players; p; p = p->next)
        if (name ? !lstrcmpiW(p->alias, name) : p->id == id) return p;
    return 0;
}

static DWORD player_open(const WCHAR *file, const WCHAR *alias, UINT *id)
{
    if (!synth_present()) return MCIERR_SEQ_NOMIDIPRESENT;
    HANDLE fh = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (fh == INVALID_HANDLE_VALUE) return MCIERR_INVALID_FILE;
    DWORD size = GetFileSize(fh, 0), got = 0;
    void *buf = size && size != INVALID_FILE_SIZE ? malloc(size) : 0;
    tml_message *msgs = 0;
    if (buf && ReadFile(fh, buf, size, &got, 0) && got == size) msgs = tml_load_memory(buf, (int)size);
    free(buf);
    CloseHandle(fh);
    if (!msgs) return MCIERR_INVALID_FILE;
    Player *p = calloc(1, sizeof(*p));
    if (!p) { tml_free(msgs); return MCIERR_OUT_OF_MEMORY; }
    p->msgs = p->cur = msgs;
    tml_get_info(msgs, 0, 0, 0, 0, (unsigned int *)&p->length);
    lstrcpynW(p->alias, alias && *alias ? alias : file, 64);
    p->ms_format = TRUE;
    lock();
    if (find_player(p->alias, 0)) {
        unlock();
        tml_free(msgs);
        free(p);
        return MCIERR_DUPLICATE_ALIAS;
    }
    DWORD r = synth_get();
    if (r) {
        unlock();
        tml_free(msgs);
        free(p);
        return MCIERR_SEQ_NOMIDIPRESENT;
    }
    p->id = g.next_id++;
    p->next = g.players;
    g.players = p;
    unlock();
    if (id) *id = p->id;
    return 0;
}

static void seek(Player *p, DWORD to)              /* (locked) */
{
    p->pos = to > p->length ? p->length : to;
    p->cur = p->msgs;
    /* replay what comes before @to except the notes, so programs and controllers are right */
    while (p->cur && p->cur->time < p->pos) {
        tml_message *t = p->cur;
        if (t->type == TML_PROGRAM_CHANGE || t->type == TML_CONTROL_CHANGE || t->type == TML_PITCH_BEND)
            apply(t->type == TML_PITCH_BEND ? 0xE0u | t->channel | (t->pitch_bend & 0x7Fu) << 8 | (t->pitch_bend >> 7 & 0x7Fu) << 16
                                            : t->type | t->channel | (DWORD)(BYTE)t->key << 8 | (DWORD)(BYTE)t->velocity << 16);
        p->cur = t->next;
    }
}

static DWORD player_close(Player *p)
{
    lock();
    Player **pp = &g.players;
    while (*pp && *pp != p) pp = &(*pp)->next;
    if (*pp) *pp = p->next;
    if (p->mode == PLAYING) stop_notes();
    unlock();
    if (p->done) { SetEvent(p->done); CloseHandle(p->done); }
    tml_free(p->msgs);
    free(p);
    synth_put();
    return 0;
}

/* play [from] [to]: @wait blocks until the end */
static DWORD player_play(Player *p, BOOL has_from, DWORD from, BOOL has_to, DWORD to, HWND notify, BOOL wait)
{
    lock();
    if (has_from) seek(p, from);
    else if (p->mode == STOPPED && p->pos >= p->length) seek(p, 0);
    p->to = has_to ? to : p->length;
    p->notify = notify;
    if (wait && !p->done) p->done = CreateEventW(0, TRUE, FALSE, 0);
    if (wait) ResetEvent(p->done);
    p->mode = PLAYING;
    unlock();
    if (wait) WaitForSingleObject(p->done, INFINITE);
    return 0;
}

static DWORD player_stop(Player *p, BOOL pause)
{
    lock();
    if (p->mode == PLAYING) stop_notes();
    p->mode = pause && p->mode != STOPPED ? PAUSED : STOPPED;
    unlock();
    return 0;
}

/* -- mciSendString -- */
static int word(const WCHAR **s, WCHAR *out, int n)
{
    const WCHAR *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    int k = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (k < n - 1) out[k++] = *p;
        if (*p == '"') p++;
    } else {
        for (; *p && *p != ' ' && *p != '\t'; p++) if (k < n - 1) out[k++] = *p;
    }
    out[k] = 0;
    *s = p;
    return k;
}

static DWORD wnum(const WCHAR *w)
{
    DWORD v = 0;
    while (*w >= '0' && *w <= '9') v = v * 10 + (DWORD)(*w++ - '0');
    return v;
}

static BOOL is_midi_file(const WCHAR *f)
{
    int n = lstrlenW(f);
    return (n > 4 && (!lstrcmpiW(f + n - 4, L".mid") || !lstrcmpiW(f + n - 4, L".rmi"))) ||
           (n > 5 && !lstrcmpiW(f + n - 5, L".midi")) || (n > 4 && !lstrcmpiW(f + n - 4, L".kar"));
}

static void ret_text(LPWSTR ret, UINT n, const WCHAR *t) { if (ret && n) lstrcpynW(ret, t, (int)n); }
static void ret_num(LPWSTR ret, UINT n, DWORD v)
{
    WCHAR b[16];
    int k = 15;
    b[k] = 0;
    do { b[--k] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    ret_text(ret, n, b + k);
}

MMAPI DWORD WINAPI mciSendStringW(LPCWSTR cmd, LPWSTR ret, UINT n, HWND cb)
{
    WCHAR verb[32], dev[MAX_PATH], w[MAX_PATH];
    if (ret && n) ret[0] = 0;
    if (!cmd) return MCIERR_MISSING_PARAMETER;
    const WCHAR *s = cmd;
    if (!word(&s, verb, 32)) return MCIERR_UNRECOGNIZED_COMMAND;
    if (!word(&s, dev, MAX_PATH)) return MCIERR_MISSING_PARAMETER;

    if (!lstrcmpiW(verb, L"open")) {
        WCHAR type[32] = L"", alias[64] = L"";
        while (word(&s, w, MAX_PATH)) {
            if (!lstrcmpiW(w, L"type")) word(&s, type, 32);
            else if (!lstrcmpiW(w, L"alias")) word(&s, alias, 64);
        }
        /* "open sequencer!file", "open file type sequencer", or a .mid file */
        WCHAR *bang = 0;
        for (WCHAR *p = dev; *p; p++) if (*p == '!') { bang = p; break; }
        if (bang) { *bang = 0; lstrcpynW(type, dev, 32); memmove(dev, bang + 1, (lstrlenW(bang + 1) + 1) * sizeof(WCHAR)); }
        if (!(type[0] ? !lstrcmpiW(type, L"sequencer") : is_midi_file(dev))) return MCIERR_DEVICE_NOT_INSTALLED;
        UINT id;
        DWORD r = player_open(dev, alias, &id);
        if (!r) ret_num(ret, n, id);
        return r;
    }

    BOOL all = !lstrcmpiW(dev, L"all");
    lock();
    Player *p = all ? 0 : find_player(dev, 0);
    unlock();
    if (!p && !(all && (!lstrcmpiW(verb, L"close") || !lstrcmpiW(verb, L"stop")))) return MCIERR_INVALID_DEVICE_NAME;

    BOOL notify = FALSE, wait = FALSE, has_from = FALSE, has_to = FALSE;
    DWORD from = 0, to = 0;
    WCHAR item[32] = L"";
    while (word(&s, w, MAX_PATH)) {
        if (!lstrcmpiW(w, L"notify")) notify = TRUE;
        else if (!lstrcmpiW(w, L"wait")) wait = TRUE;
        else if (!lstrcmpiW(w, L"from") && word(&s, w, 32)) { has_from = TRUE; from = wnum(w); }
        else if (!lstrcmpiW(w, L"to") && word(&s, w, 32)) {
            has_to = TRUE;
            to = !lstrcmpiW(w, L"start") ? 0 : !lstrcmpiW(w, L"end") ? 0xFFFFFFFF : wnum(w);
        } else if (!item[0]) lstrcpynW(item, w, 32);
    }
    HWND wnd = notify ? cb : 0;

    if (!lstrcmpiW(verb, L"close")) {
        if (!all) return player_close(p);
        for (;;) {
            lock();
            Player *q = g.players;
            unlock();
            if (!q) return 0;
            player_close(q);
        }
    }
    if (!lstrcmpiW(verb, L"play")) return player_play(p, has_from, from, has_to, to, wnd, wait);
    if (!lstrcmpiW(verb, L"stop")) {
        if (all) {
            lock();
            for (Player *q = g.players; q; q = q->next) if (q->mode == PLAYING) { q->mode = STOPPED; stop_notes(); }
            unlock();
            return 0;
        }
        return player_stop(p, FALSE);
    }
    if (!lstrcmpiW(verb, L"pause")) return player_stop(p, TRUE);
    if (!lstrcmpiW(verb, L"resume")) {
        lock();
        if (p->mode == PAUSED) p->mode = PLAYING;
        unlock();
        return 0;
    }
    if (!lstrcmpiW(verb, L"seek")) {
        lock();
        if (p->mode == PLAYING) stop_notes();
        p->mode = STOPPED;
        seek(p, has_to ? to : 0);
        unlock();
        return 0;
    }
    if (!lstrcmpiW(verb, L"set")) return 0;                 /* time format: milliseconds only */
    if (!lstrcmpiW(verb, L"status")) {
        lock();
        if (!lstrcmpiW(item, L"mode")) ret_text(ret, n, p->mode == PLAYING ? L"playing" : p->mode == PAUSED ? L"paused" : L"stopped");
        else if (!lstrcmpiW(item, L"length")) ret_num(ret, n, p->length);
        else if (!lstrcmpiW(item, L"position")) ret_num(ret, n, p->pos);
        else if (!lstrcmpiW(item, L"ready")) ret_text(ret, n, L"true");
        else if (!lstrcmpiW(item, L"time")) ret_text(ret, n, L"milliseconds");
        else { unlock(); return MCIERR_UNRECOGNIZED_KEYWORD; }
        unlock();
        return 0;
    }
    return MCIERR_UNRECOGNIZED_COMMAND;
}

MMAPI DWORD WINAPI mciSendStringA(LPCSTR cmd, LPSTR ret, UINT n, HWND cb)
{
    WCHAR w[1024], r[256] = L"";
    if (ret && n) ret[0] = 0;
    if (!cmd) return MCIERR_MISSING_PARAMETER;
    MultiByteToWideChar(CP_ACP, 0, cmd, -1, w, 1024);
    w[1023] = 0;
    DWORD e = mciSendStringW(w, r, 256, cb);
    if (ret && n) WideCharToMultiByte(CP_ACP, 0, r, -1, ret, (int)n, 0, 0);
    return e;
}

/* -- mciSendCommand -- */
#define MCI_OPEN    0x0803
#define MCI_CLOSE   0x0804
#define MCI_PLAY    0x0806
#define MCI_SEEK    0x0807
#define MCI_STOP    0x0808
#define MCI_PAUSE   0x0809
#define MCI_SET     0x080D
#define MCI_STATUS  0x0814
#define MCI_RESUME  0x0855
#define MCI_NOTIFY  0x00000001
#define MCI_WAIT    0x00000002
#define MCI_FROM    0x00000004
#define MCI_TO      0x00000008
#define MCI_OPEN_ELEMENT 0x00000200
#define MCI_OPEN_ALIAS   0x00000400
#define MCI_OPEN_TYPE_ID 0x00001000
#define MCI_OPEN_TYPE    0x00002000
#define MCI_SEEK_TO_START 0x00000100
#define MCI_STATUS_ITEM   0x00000100
#define MCI_ALL_DEVICE_ID ((UINT)-1)
#define MCI_DEVTYPE_SEQUENCER 523
#define MCI_MODE_STOP  525
#define MCI_MODE_PLAY  526
#define MCI_MODE_PAUSE 529

typedef struct { DWORD_PTR dwCallback; UINT wDeviceID; LPCWSTR lpstrDeviceType, lpstrElementName, lpstrAlias; } MCI_OPEN_PARMSW;
typedef struct { DWORD_PTR dwCallback; UINT wDeviceID; LPCSTR lpstrDeviceType, lpstrElementName, lpstrAlias; } MCI_OPEN_PARMSA;
typedef struct { DWORD_PTR dwCallback; DWORD dwFrom, dwTo; } MCI_PLAY_PARMS;
typedef struct { DWORD_PTR dwCallback; DWORD_PTR dwReturn; DWORD dwItem, dwTrack; } MCI_STATUS_PARMS;
typedef struct { DWORD_PTR dwCallback; DWORD dwTo; } MCI_SEEK_PARMS;

static DWORD send_command(UINT id, UINT msg, DWORD_PTR flags, DWORD_PTR parms, BOOL ansi)
{
    if (msg == MCI_OPEN) {
        if (!parms) return MCIERR_MISSING_PARAMETER;
        MCI_OPEN_PARMSW *o = (MCI_OPEN_PARMSW *)parms;
        WCHAR file[MAX_PATH] = L"", type[32] = L"", alias[64] = L"";
        if (flags & MCI_OPEN_ELEMENT) {
            if (ansi) MultiByteToWideChar(CP_ACP, 0, ((MCI_OPEN_PARMSA *)o)->lpstrElementName, -1, file, MAX_PATH);
            else lstrcpynW(file, o->lpstrElementName, MAX_PATH);
        }
        if (flags & MCI_OPEN_ALIAS) {
            if (ansi) MultiByteToWideChar(CP_ACP, 0, ((MCI_OPEN_PARMSA *)o)->lpstrAlias, -1, alias, 64);
            else lstrcpynW(alias, o->lpstrAlias, 64);
        }
        BOOL seq = is_midi_file(file);
        if (flags & MCI_OPEN_TYPE) {
            if (flags & MCI_OPEN_TYPE_ID) seq = LOWORD((DWORD_PTR)o->lpstrDeviceType) == MCI_DEVTYPE_SEQUENCER;
            else {
                if (ansi) MultiByteToWideChar(CP_ACP, 0, ((MCI_OPEN_PARMSA *)o)->lpstrDeviceType, -1, type, 32);
                else lstrcpynW(type, o->lpstrDeviceType, 32);
                seq = !lstrcmpiW(type, L"sequencer");
            }
        }
        if (!seq) return MCIERR_DEVICE_NOT_INSTALLED;
        if (!file[0]) return MCIERR_MISSING_PARAMETER;
        UINT nid;
        DWORD r = player_open(file, alias[0] ? alias : 0, &nid);
        if (!r) o->wDeviceID = nid;
        return r;
    }
    if (id == MCI_ALL_DEVICE_ID && msg == MCI_CLOSE) return mciSendStringW(L"close all", 0, 0, 0);
    lock();
    Player *p = find_player(0, id);
    unlock();
    if (!p) return MCIERR_INVALID_DEVICE_ID;
    HWND wnd = (flags & MCI_NOTIFY) && parms ? (HWND)*(DWORD_PTR *)parms : 0;
    switch (msg) {
    case MCI_CLOSE: return player_close(p);
    case MCI_PLAY: {
        MCI_PLAY_PARMS *pp = (MCI_PLAY_PARMS *)parms;
        return player_play(p, (flags & MCI_FROM) && pp, pp ? pp->dwFrom : 0, (flags & MCI_TO) && pp, pp ? pp->dwTo : 0,
                           wnd, (flags & MCI_WAIT) != 0);
    }
    case MCI_STOP:   return player_stop(p, FALSE);
    case MCI_PAUSE:  return player_stop(p, TRUE);
    case MCI_RESUME: lock(); if (p->mode == PAUSED) p->mode = PLAYING; unlock(); return 0;
    case MCI_SEEK:
        lock();
        if (p->mode == PLAYING) stop_notes();
        p->mode = STOPPED;
        seek(p, (flags & MCI_TO) && parms ? ((MCI_SEEK_PARMS *)parms)->dwTo : 0);
        unlock();
        return 0;
    case MCI_SET: return 0;
    case MCI_STATUS: {
        MCI_STATUS_PARMS *st = (MCI_STATUS_PARMS *)parms;
        if (!st || !(flags & MCI_STATUS_ITEM)) return MCIERR_MISSING_PARAMETER;
        lock();
        switch (st->dwItem) {
        case 1: st->dwReturn = p->length; break;                         /* MCI_STATUS_LENGTH */
        case 2: st->dwReturn = p->pos; break;                            /* MCI_STATUS_POSITION */
        case 4: st->dwReturn = p->mode == PLAYING ? MCI_MODE_PLAY : p->mode == PAUSED ? MCI_MODE_PAUSE : MCI_MODE_STOP; break;
        case 7: st->dwReturn = TRUE; break;                              /* MCI_STATUS_READY */
        default: unlock(); return MCIERR_UNRECOGNIZED_KEYWORD;
        }
        unlock();
        return 0;
    }
    }
    return MCIERR_UNRECOGNIZED_COMMAND;
}

MMAPI DWORD WINAPI mciSendCommandW(UINT id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) { return send_command(id, msg, flags, parms, FALSE); }
MMAPI DWORD WINAPI mciSendCommandA(UINT id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) { return send_command(id, msg, flags, parms, TRUE); }

static const char *mci_error(DWORD e)
{
    switch (e) {
    case 0: return "The specified command was carried out.";
    case MCIERR_INVALID_DEVICE_ID: return "Invalid MCI device ID. Use the ID returned when opening the MCI device.";
    case MCIERR_UNRECOGNIZED_KEYWORD: return "The driver cannot recognize the specified command parameter.";
    case MCIERR_UNRECOGNIZED_COMMAND: return "The driver cannot recognize the specified command.";
    case MCIERR_INVALID_DEVICE_NAME: return "The specified device is not open or is not recognized by MCI.";
    case MCIERR_OUT_OF_MEMORY: return "There is not enough memory available for this task.";
    case MCIERR_MISSING_PARAMETER: return "The specified command requires a parameter. Please supply one.";
    case MCIERR_DEVICE_NOT_INSTALLED: return "The specified device is not installed.";
    case MCIERR_INVALID_FILE: return "The specified file cannot be played on the specified MCI device.";
    case MCIERR_DUPLICATE_ALIAS: return "The specified alias is already being used in this application.";
    case MCIERR_SEQ_NOMIDIPRESENT: return "There are no MIDI devices installed on the system.";
    default: return "Unknown MCI error.";
    }
}
MMAPI BOOL WINAPI mciGetErrorStringW(DWORD e, LPWSTR buf, UINT n)
{
    if (!buf || !n) return FALSE;
    MultiByteToWideChar(CP_ACP, 0, mci_error(e), -1, buf, (int)n);
    buf[n - 1] = 0;
    return TRUE;
}
MMAPI BOOL WINAPI mciGetErrorStringA(DWORD e, LPSTR buf, UINT n)
{
    if (!buf || !n) return FALSE;
    lstrcpynA(buf, mci_error(e), (int)n);
    return TRUE;
}
