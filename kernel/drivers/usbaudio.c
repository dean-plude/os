/*
 * usbaudio.c — USB Audio Class 1.0 and 2.0 speakers, headsets and microphones
 *
 * A USB audio function is an AudioControl interface (its units and
 * terminals: the feature unit has the mute and volume controls) and one
 * or more AudioStreaming interfaces.  A streaming interface's alternate
 * setting 0 has no endpoints (no bandwidth); each other setting has an
 * isochronous endpoint and says which format it carries.  This driver
 * plays on the first streaming interface with a setting whose OUT
 * endpoint carries 48 kHz stereo PCM, and records from the first with a
 * setting whose IN endpoint carries 48 kHz PCM, mono or stereo; samples
 * of 16 to 32 bits in 2-, 3- or 4-byte slots (the mixer's own 16 bits go
 * in the top two bytes of the slot, and are taken from there).  For each
 * it sets the sampling rate, switches to that setting and attaches itself
 * to the mixer (audio.h): as an output, and as an input; the newest of
 * each is used, as on Windows.  The feature units are unmuted and set to
 * 0 dB.
 *
 * The two versions differ in the descriptors and in where the rate is:
 *  - Audio 1.0: the AudioControl header lists the streaming interfaces;
 *    a setting's Format Type I descriptor lists its rates, and the rate is
 *    set on the endpoint (where it has that control).
 *  - Audio 2.0 (the control interface's protocol is 0x20): an Interface
 *    Association descriptor groups the control and streaming interfaces;
 *    the format descriptors carry no rates.  Instead each terminal names
 *    a clock entity: a clock source, or a selector or multiplier in front
 *    of one.  Selectors are switched to their first input and the rate is
 *    set on the clock source (where it is programmable), then read back:
 *    a clock that runs at anything but 48 kHz is not used.  Feature unit
 *    controls take two bits each (3: the host may set it) instead of one.
 *    High-speed devices usually take a packet every microframe (125 us,
 *    6 frames at 48 kHz) rather than every millisecond.
 *
 * Playback: the mixer fills a 64 KiB ring ahead of the device, as for HD
 * Audio.  The pipe streams a ring of isochronous transfers (usb.h), one
 * packet per service interval; each finished transfer is refilled from the
 * mixer's ring, and how far that has got is the output's position.  The
 * device gets exactly 48 frames a millisecond (asynchronous endpoints'
 * feedback is not used).
 *
 * Recording: the IN stream runs from the moment the microphone is plugged
 * in; each packet that arrives is copied into a 64 KiB ring (a mono
 * microphone's samples twice, as stereo), and how far that has got is the
 * input's position, which the mixer reads from while something records.
 */

#include "usb.h"
#include "audio.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define SUB_CONTROL     1
#define SUB_STREAMING   2
#define PROTO_UAC2      0x20
#define USB_DT_IAD      0x0B                  /* interface association */
#define AC_HEADER       0x01
#define AC_INPUT_TERM   0x02
#define AC_OUTPUT_TERM  0x03
#define AC_FEATURE_UNIT 0x06
#define AC2_CLOCK_SOURCE     0x0A
#define AC2_CLOCK_SELECTOR   0x0B
#define AC2_CLOCK_MULTIPLIER 0x0C
#define AS_GENERAL      0x01
#define AS_FORMAT_TYPE  0x02
#define EP_GENERAL      0x01
#define FMT_PCM         0x0001
#define SET_CUR         0x01                  /* (Audio 2.0: CUR, with the direction in the request type) */
#define MUTE_CONTROL    0x01
#define VOLUME_CONTROL  0x02
#define SAMPLING_FREQ   0x01                  /* (Audio 2.0: the clock source's CS_SAM_FREQ_CONTROL) */
#define CX_SELECTOR     0x01                  /* a clock selector's CX_CLOCK_SELECTOR_CONTROL */

#define RING_PAGES      16                    /* 64 KiB: 341 ms, like HD Audio's ring */
#define RING_BYTES      (RING_PAGES * PAGE_SIZE)
#define FRAME           4                     /* s16 stereo */
#define QUEUED_US       64000                 /* streamed ahead: well over a timer tick */
#define MAX_UA          8
#define MAX_AS          8

/* One direction's stream */
typedef struct {
    UsbPipe        *pipe;
    UINT8           as;                       /* the streaming interface */
    UINT8           channels;                 /* on the device: 2 playing, 1 or 2 recording */
    UINT8           sub, bits;                /* bytes a sample takes on the device, and its bits */
    UINT32          frames;                   /* frames a packet (playback) */
    INT16          *ring;
    volatile UINT32 pos;                      /* playback: where the next packet comes from; recording: written up to */
    bool            attached;
} UaStream;

typedef struct {
    AudioOutput    out;
    AudioInput     in;
    UsbDev        *usb;
    UINT8          ac;                        /* the control interface */
    bool           v2;                        /* USB Audio 2.0 */
    UaStream       play, rec;
    char           name[48], iname[64];
} Ua;

/* A streaming interface's setting we can use */
typedef struct {
    bool         found, freq_ctl;             /* (freq_ctl: Audio 1.0, the endpoint has a rate control) */
    UINT8        as, alt, terminal;           /* (terminal: the one the setting is linked to) */
    const UINT8 *ep;
    int          channels, sub, bits;
} UaSetting;

static Ua *g_ua[MAX_UA];                      /* for the streaming interfaces' probes */

static UINT32 le32(const UINT8 *p) { return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 | (UINT32)p[3] << 24; }

/* Refill a transfer with the next packets of the mixer's ring (from the
 * controller's poll, interrupts off) */
static void fill(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p), n = s->frames * FRAME, sub = s->sub;
    for (int i = 0; i < packets; i++) {
        UINT8 *dst = data + (UINT32)i * psize;
        UINT32 pos = s->pos;
        if (sub == 2) {                                  /* the mixer's format: copied */
            UINT32 first = RING_BYTES - pos < n ? RING_BYTES - pos : n;
            memcpy(dst, (UINT8 *)s->ring + pos, first);
            if (first < n) memcpy(dst + first, s->ring, n - first);
        } else {                                         /* in the top two bytes of wider slots */
            for (UINT32 k = 0; k < 2 * s->frames; k++, dst += sub) {
                UINT16 v = (UINT16)s->ring[((pos + k * 2) % RING_BYTES) / 2];
                memset(dst, 0, sub - 2);
                dst[sub - 2] = (UINT8)v;
                dst[sub - 1] = (UINT8)(v >> 8);
            }
        }
        s->pos = (pos + n) % RING_BYTES;
        lens[i] = (UINT16)(2 * s->frames * sub);
    }
}

/* Copy the packets a transfer brought into the recording ring (from the
 * controller's poll, interrupts off) */
static void drain(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p), sub = s->sub, in_frame = sub * s->channels;
    UINT32 pos = s->pos, right = (s->channels - 1u) * sub;
    for (int i = 0; i < packets; i++) {
        const UINT8 *src = data + (UINT32)i * psize;
        UINT32 frames = lens[i] / in_frame;
        for (UINT32 f = 0; f < frames; f++, src += in_frame) {
            INT16 *dst = (INT16 *)((UINT8 *)s->ring + pos);
            dst[0] = (INT16)(src[sub - 2] | src[sub - 1] << 8);
            dst[1] = (INT16)(src[right + sub - 2] | src[right + sub - 1] << 8);
            pos = (pos + FRAME) % RING_BYTES;
        }
    }
    s->pos = pos;
}

static UINT32 position(void *ctx) { return ((UaStream *)ctx)->pos; }

static void stream_free(UaStream *s)
{
    if (s->ring) kernel_free_pages(s->ring, RING_PAGES);
    s->ring = NULL;
}

static void ua_gone(void *inst)
{
    Ua *u = inst;
    if (u->play.attached) AudioOutputDetach(&u->out);
    if (u->rec.attached) AudioInputDetach(&u->in);
    kprintf("[USB] %s: audio device unplugged\n", UsbDevName(u->usb));
    for (int i = 0; i < MAX_UA; i++)
        if (g_ua[i] == u) g_ua[i] = NULL;
    stream_free(&u->play);
    stream_free(&u->rec);
    kfree(u);
}

/* Whether an Audio 1.0 Format Type I descriptor offers 48 kHz PCM; the
 * channels it has (0: no) */
static int format_ok(const UINT8 *t)
{
    if (t[0] < 8 || t[3] != 1) return 0;
    int n = t[7];
    if (n == 0)                                          /* continuous: a range */
        return t[0] >= 14 && (UINT32)(t[8] | t[9] << 8 | t[10] << 16) <= AUDIO_RATE &&
               (UINT32)(t[11] | t[12] << 8 | t[13] << 16) >= AUDIO_RATE ? t[4] : 0;
    for (int i = 0; i < n && 8 + i * 3 + 3 <= t[0]; i++)
        if ((UINT32)(t[8 + i * 3] | t[9 + i * 3] << 8 | t[10 + i * 3] << 16) == AUDIO_RATE) return t[4];
    return 0;
}

/* Whether a setting carries what we play (@in false: stereo) or record
 * (@in: mono or stereo), in samples we can convert */
static bool setting_ok(const UaSetting *s, bool pcm, bool in)
{
    return pcm && s->ep && (in ? s->channels == 1 || s->channels == 2 : s->channels == 2) &&
           s->sub >= 2 && s->sub <= 4 && s->bits >= 16 && s->bits <= s->sub * 8;
}

/* The first setting of streaming interface @as we can use, playing (OUT)
 * or recording (@in).  False: none. */
static bool find_setting(UsbDev *d, UINT8 as, bool in, bool v2, UaSetting *out)
{
    int total;
    const UINT8 *cfg = UsbDevConfig(d, &total);
    bool ours = false, pcm = false;
    UaSetting cur = { 0 };
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            if (ours && setting_ok(&cur, pcm, in)) break;
            ours = p[2] == as && p[3] != 0 && p[5] == 1 && p[6] == SUB_STREAMING;
            pcm = false;
            memset(&cur, 0, sizeof(cur));
            cur.as = as;
            cur.alt = p[3];
        } else if (!ours) {
            continue;
        } else if (p[1] == USB_DT_CS_INTERFACE && p[2] == AS_GENERAL) {
            if (v2 && p[0] >= 16) {                      /* format type I, bmFormats has PCM */
                pcm = p[5] == 1 && (le32(&p[6]) & 1);
                cur.channels = p[10];
            } else if (!v2 && p[0] >= 7) {
                pcm = (p[5] | p[6] << 8) == FMT_PCM;
            }
            cur.terminal = p[3];
        } else if (p[1] == USB_DT_CS_INTERFACE && p[2] == AS_FORMAT_TYPE) {
            if (v2 && p[0] >= 6 && p[3] == 1) {          /* subslot size, bit resolution */
                cur.sub = p[4];
                cur.bits = p[5];
            } else if (!v2 && (cur.channels = format_ok(p)) != 0) {
                cur.sub = p[5];
                cur.bits = p[6];
            }
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 1 && !(p[2] & 0x80) == !in && !cur.ep) {
            cur.ep = p;
        } else if (!v2 && p[1] == USB_DT_CS_ENDPOINT && p[0] >= 4 && p[2] == EP_GENERAL && cur.ep) {
            cur.freq_ctl = (p[3] & 1) != 0;
        }
    }
    cur.found = ours && setting_ok(&cur, pcm, in);
    *out = cur;
    return cur.found;
}

/* The streaming interfaces of an Audio 2.0 function whose control
 * interface is @ac: those its Interface Association groups, or else every
 * Audio 2.0 streaming interface of the device.  Returns how many. */
static int streaming_ifaces(UsbDev *d, UINT8 ac, UINT8 *as)
{
    int total, n = 0;
    const UINT8 *cfg = UsbDevConfig(d, &total);
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_IAD && p[0] >= 8 && p[2] == ac) {
            for (int i = 1; i < p[3] && n < MAX_AS; i++) as[n++] = (UINT8)(ac + i);
            return n;
        }
    }
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[3] == 0 && p[5] == 1 && p[6] == SUB_STREAMING &&
            p[7] == PROTO_UAC2 && n < MAX_AS)
            as[n++] = p[2];
    }
    return n;
}

/* The AudioControl entity @id of subtype @lo..@hi; NULL if none */
static const UINT8 *entity(const UsbIface *f, UINT8 id, UINT8 lo, UINT8 hi)
{
    int off = 0;
    for (const UINT8 *c; (c = UsbIfaceFind(f, USB_DT_CS_INTERFACE, &off)) != NULL;)
        if (c[0] >= 5 && c[2] >= lo && c[2] <= hi && c[3] == id) return c;
    return NULL;
}

/* Audio 2.0: run the clock of terminal @term at 48 kHz.  Follows the
 * terminal's clock entity to its source, switching selectors to their
 * first input; sets the rate where the source lets the host set it, then
 * reads it back.  False if the clock runs at another rate. */
static bool set_clock(UsbDev *d, const UsbIface *f, UINT8 term)
{
    const UINT8 *t = entity(f, term, AC_INPUT_TERM, AC_OUTPUT_TERM);
    if (!t || t[0] < (t[2] == AC_INPUT_TERM ? 17 : 12)) {
        kprintf("[USB] %s: audio terminal %u not found\n", UsbDevName(d), term);
        return false;
    }
    UINT8 id = t[2] == AC_INPUT_TERM ? t[7] : t[8];
    for (int hops = 0; hops < 8; hops++) {
        const UINT8 *c = entity(f, id, AC2_CLOCK_SOURCE, AC2_CLOCK_MULTIPLIER);
        if (!c) break;
        UINT16 index = (UINT16)(id << 8 | f->number);
        if (c[2] == AC2_CLOCK_SOURCE) {
            if (c[0] < 8) break;
            UINT8 rate[4] = { AUDIO_RATE & 0xFF, (AUDIO_RATE >> 8) & 0xFF, AUDIO_RATE >> 16, 0 }, now[4];
            if ((c[5] & 3) == 3) UsbControl(d, 0x21, SET_CUR, SAMPLING_FREQ << 8, index, 4, rate);
            if (UsbControl(d, 0xA1, SET_CUR, SAMPLING_FREQ << 8, index, 4, now) == 4 && le32(now) != AUDIO_RATE) {
                kprintf("[USB] %s: audio clock %u runs at %u Hz, not 48 kHz\n", UsbDevName(d), id, le32(now));
                return false;
            }
            kprintf("[USB] %s: audio clock %u at 48 kHz\n", UsbDevName(d), id);
            return true;
        }
        if (c[2] == AC2_CLOCK_SELECTOR) {
            int pins = c[4];
            if (!pins || c[0] < 7 + pins) break;
            UINT8 pin = 1;
            if ((c[5 + pins] & 3) == 3) UsbControl(d, 0x21, SET_CUR, CX_SELECTOR << 8, index, 1, &pin);
            else if (UsbControl(d, 0xA1, SET_CUR, CX_SELECTOR << 8, index, 1, &pin) != 1 || !pin || pin > pins) pin = 1;
            id = c[4 + pin];
        } else {
            if (c[0] < 7) break;
            id = c[4];                                    /* a multiplier: its source */
        }
    }
    kprintf("[USB] %s: no clock source for audio terminal %u\n", UsbDevName(d), term);
    return false;
}

/* Unmute the feature units and set every volume control to 0 dB */
static void unmute(UsbDev *d, const UsbIface *f, bool v2)
{
    int off = 0;
    for (const UINT8 *u; (u = UsbIfaceFind(f, USB_DT_CS_INTERFACE, &off)) != NULL;) {
        if (u[2] != AC_FEATURE_UNIT) continue;
        int size = v2 ? 4 : u[0] >= 7 ? u[5] : 0;           /* bytes of controls a channel */
        if (!size || u[0] < (v2 ? 10 : 7)) continue;
        int first = v2 ? 5 : 6, channels = (u[0] - first - 1) / size;   /* master (0), then each channel */
        UINT16 index = (UINT16)(u[3] << 8 | f->number);
        for (int c = 0; c < channels; c++) {
            const UINT8 *ctl = &u[first + c * size];
            bool mute = v2 ? (ctl[0] & 3) == 3 : (ctl[0] & 1) != 0;          /* (2.0: 3 = host-settable) */
            bool vol = v2 ? (ctl[0] >> 2 & 3) == 3 : (ctl[0] & 2) != 0;
            UINT8 off0[2] = { 0, 0 };                        /* not muted; 0 dB (1/256 dB units) */
            if (mute) UsbControl(d, 0x21, SET_CUR, (UINT16)(MUTE_CONTROL << 8 | c), index, 1, off0);
            if (vol) UsbControl(d, 0x21, SET_CUR, (UINT16)(VOLUME_CONTROL << 8 | c), index, 2, off0);
        }
    }
}

/* Run setting @st (its clock, or its endpoint's rate control), switch to
 * it, open its endpoint and start streaming; false (back on setting 0) if
 * that fails */
static bool stream_start(Ua *u, const UsbIface *ac, UaStream *s, bool in, const UaSetting *st)
{
    UsbDev *d = u->usb;
    s->as = st->as;
    s->channels = (UINT8)st->channels;
    s->sub = (UINT8)st->sub;
    s->bits = (UINT8)st->bits;
    if (u->v2 && !set_clock(d, ac, st->terminal)) return false;
    s->ring = kernel_alloc_pages(RING_PAGES);
    if (!s->ring) return false;
    memset(s->ring, 0, RING_BYTES);
    if (!UsbSetInterface(d, s->as, st->alt) || !(s->pipe = UsbOpenPipe(d, st->ep, 0))) {
        kprintf("[USB] %s: could not open the audio %s stream\n", UsbDevName(d), in ? "input" : "output");
        goto fail;
    }
    UINT32 us = UsbIsoIntervalUs(s->pipe);
    UINT32 frames = (UINT32)((UINT64)AUDIO_RATE * us / 1000000);
    UINT32 need = frames * s->channels * s->sub;
    if ((UINT64)AUDIO_RATE * us % 1000000 || !need || need > UsbIsoPacketSize(s->pipe)) {
        kprintf("[USB] %s: audio endpoint (%u bytes every %u us) cannot carry 48 kHz %s\n",
                UsbDevName(d), UsbIsoPacketSize(s->pipe), us, s->channels == 1 ? "mono" : "stereo");
        goto fail_alt;
    }
    s->frames = frames;
    if (st->freq_ctl) {
        UINT8 rate[3] = { AUDIO_RATE & 0xFF, (AUDIO_RATE >> 8) & 0xFF, AUDIO_RATE >> 16 };
        UsbControl(d, 0x22, SET_CUR, SAMPLING_FREQ << 8, st->ep[2], 3, rate);
    }

    /* Transfers of about 8 ms, QUEUED_US of them in flight */
    int packets = (int)(8000 / us) ? (int)(8000 / us) : 1;
    int xfers = (int)(QUEUED_US / (us * (UINT32)packets));
    if (xfers * packets > 128) xfers = 128 / packets;
    if (xfers < 2) xfers = 2;
    if (!UsbIsoStart(s->pipe, xfers, packets, in ? drain : fill, s)) {
        kprintf("[USB] %s: the controller cannot stream %s the audio device\n", UsbDevName(d), in ? "from" : "to");
        goto fail_alt;
    }
    kprintf("[USB] %s: audio %s, 48 kHz %u-bit %s, %u-byte packets every %u us%s\n", UsbDevName(d),
            in ? "input" : "output", s->bits, s->channels == 1 ? "mono" : "stereo", need, us,
            u->v2 ? " (USB Audio 2.0)" : "");
    return true;

fail_alt:
    UsbSetInterface(d, s->as, 0);
fail:
    s->pipe = NULL;
    stream_free(s);
    return false;
}

void *UsbAudioProbe(UsbDev *d, const UsbIface *f)
{
    if (f->sub == SUB_STREAMING) {                           /* taken with its control interface */
        for (int i = 0; i < MAX_UA; i++)
            if (g_ua[i] && g_ua[i]->usb == d && ((g_ua[i]->play.ring && g_ua[i]->play.as == f->number) ||
                                                 (g_ua[i]->rec.ring && g_ua[i]->rec.as == f->number)))
                return g_ua[i];
        return NULL;
    }
    if (f->sub != SUB_CONTROL) return NULL;
    bool v2 = f->proto == PROTO_UAC2;

    /* The streaming interfaces (Audio 1.0: the header lists them): the
     * first we can play on, and the first we can record from */
    int off = 0, nas = 0;
    const UINT8 *h = NULL;
    UINT8 as[MAX_AS];
    for (const UINT8 *c; (c = UsbIfaceFind(f, USB_DT_CS_INTERFACE, &off)) != NULL;)
        if (c[0] >= (v2 ? 9 : 8) && c[2] == AC_HEADER) { h = c; break; }
    if (!h) return NULL;
    if (v2)
        nas = streaming_ifaces(d, f->number, as);
    else
        for (int i = 0; i < h[7] && 8 + i < h[0] && nas < MAX_AS; i++) as[nas++] = h[8 + i];
    UaSetting dir[2] = { 0 };
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < nas && !dir[k].found; i++)
            find_setting(d, as[i], k == 1, v2, &dir[k]);
    if (!dir[0].found && !dir[1].found) {
        kprintf("[USB] %s: audio device has no 48 kHz output or input\n", UsbDevName(d));
        return NULL;
    }
    int slot = -1;
    for (int i = 0; i < MAX_UA && slot < 0; i++)
        if (!g_ua[i]) slot = i;
    if (slot < 0) return NULL;

    Ua *u = kzalloc(sizeof(Ua));
    if (!u) return NULL;
    u->usb = d;
    u->ac = f->number;
    u->v2 = v2;
    bool play = dir[0].found && stream_start(u, f, &u->play, false, &dir[0]);
    bool rec = dir[1].found && stream_start(u, f, &u->rec, true, &dir[1]);
    if (!play && !rec) { kfree(u); return NULL; }
    unmute(d, f, v2);

    g_ua[slot] = u;
    UsbBind(d, u, ua_gone);
    if (play) {
        ksnprintf(u->name, sizeof(u->name), "USB Audio Device (%s)", UsbDevName(d));
        u->out.name = u->name;
        u->out.ring = u->play.ring;
        u->out.bytes = RING_BYTES;
        u->out.position = position;
        u->out.ctx = &u->play;
        u->play.attached = AudioOutputAttach(&u->out);
    }
    if (rec) {
        ksnprintf(u->iname, sizeof(u->iname), "USB Microphone (%s)", UsbDevName(d));
        u->in.name = u->iname;
        u->in.ring = u->rec.ring;
        u->in.bytes = RING_BYTES;
        u->in.position = position;
        u->in.ctx = &u->rec;
        u->rec.attached = AudioInputAttach(&u->in);
    }
    return u;
}
