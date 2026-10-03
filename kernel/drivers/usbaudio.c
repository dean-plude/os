/*
 * usbaudio.c — USB Audio Class 1.0 and 2.0 speakers, headsets and microphones
 *
 * A USB audio function is an AudioControl interface (its units and
 * terminals: the feature unit has the mute and volume controls) and one
 * or more AudioStreaming interfaces.  A streaming interface's alternate
 * setting 0 has no endpoints (no bandwidth); each other setting has an
 * isochronous endpoint and says which format it carries.  This driver
 * plays on the first streaming interface with a setting whose OUT
 * endpoint carries PCM, and records from the first with a setting whose
 * IN endpoint does: one to eight channels, samples of 16 to 32 bits in
 * 2-, 3- or 4-byte slots (the mixer's own 16 bits go in the top two
 * bytes of the slot, and are taken from there).  Of the sampling rates
 * the device offers it takes 48 kHz, what programs mostly play, where it
 * can; otherwise the lowest rate above it (nothing is lost converting
 * up), or failing that the highest below it.  For each it sets the
 * sampling rate, switches to that setting and attaches itself to the
 * mixer (audio.h) at that rate: as an output, and as an input; the newest
 * of each becomes the default, as on Windows.  The mixer converts what
 * plays and records to and from the device's rate, so the device's
 * frames go to and come from its ring as they are.  The feature units are unmuted and set to
 * 0 dB.  The devices are named after their product string, as Windows
 * names an endpoint after its device.
 *
 * The two versions differ in the descriptors and in where the rate is:
 *  - Audio 1.0: the AudioControl header lists the streaming interfaces;
 *    a setting's Format Type I descriptor lists its rates (or gives a
 *    range), and the rate is set on the endpoint (where it has that
 *    control).
 *  - Audio 2.0 (the control interface's protocol is 0x20): an Interface
 *    Association descriptor groups the control and streaming interfaces;
 *    the format descriptors carry no rates.  Instead each terminal names
 *    a clock entity: a clock source, or a selector or multiplier in front
 *    of one.  Selectors are switched to their first input; the source's
 *    rates are its RANGE (subranges of minimum, maximum and step), the
 *    rate chosen from them is set on it (where it is programmable), then
 *    read back: the stream runs at whatever rate the clock reports.  Two
 *    streams on one clock choose the same rate from the same ranges.  Feature unit
 *    controls take two bits each (3: the host may set it) instead of one.
 *    High-speed devices usually take a packet every microframe (125 us,
 *    6 frames at 48 kHz) rather than every millisecond.
 *
 * Playback: the mixer fills a ring of about a third of a second ahead of
 * the device, as for HD Audio, at the device's rate.  The pipe streams a
 * ring of isochronous transfers (usb.h), one packet per service interval;
 * each finished transfer is refilled from the mixer's ring, and how far
 * that has got is the output's position.  Packets carry a whole number of
 * frames that averages the device's rate (44 or 45 a millisecond at 44.1
 * kHz), counted in 1/65536 of a frame.  An asynchronous endpoint runs on
 * the device's own clock, which is never exactly the host's: it has a
 * feedback endpoint (the second endpoint of the setting, or the one its
 * bSynchAddress names) through which it says how many frames it is
 * really playing a (micro)frame, at full speed in 10.14 fixed point
 * (three bytes, frames a millisecond) and at high speed in 16.16 (four,
 * frames a microframe).  That pipe streams too, and the packets carry
 * what it last said instead, so the device neither runs dry nor
 * overflows however long it plays; the mixer follows, since it mixes up
 * to wherever the packets have got.  (A value further than an eighth from
 * the nominal rate is ignored, after trying the other format: some
 * full-speed devices send 16.16.)  A device with other than two channels
 * gets the mixer's left and right in its first two (front left and right,
 * as Windows plays stereo on surround speakers) and silence in the rest,
 * or their average on a mono speaker.
 *
 * Recording: the IN stream runs from the moment the microphone is plugged
 * in; each packet that arrives is copied into a ring of the same size (a
 * mono microphone's samples twice, as stereo; of more channels, the first
 * two), and how far that has got is the input's position, which the mixer
 * reads from (converting to the rate each program records at) while
 * something records.
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
#define RANGE           0x02                  /* Audio 2.0: a control's RANGE (GET) */
#define CX_SELECTOR     0x01                  /* a clock selector's CX_CLOCK_SELECTOR_CONTROL */

#define RING_MS         333                   /* a ring holds 341 ms at 48 kHz (64 KiB), like HD Audio's */
#define FRAME           4                     /* s16 stereo */
#define QUEUED_US       64000                 /* streamed ahead: well over a timer tick */
#define MAX_UA          8
#define MAX_AS          8
#define MAX_CH          8
#define MAX_RATES       16
#define MAX_RATE        192000

/* One direction's stream */
typedef struct {
    UsbPipe        *pipe;
    UsbPipe        *fb;                       /* playback: the feedback pipe of an asynchronous endpoint (or NULL) */
    UsbDev         *usb;
    UINT8           as;                       /* the streaming interface */
    UINT8           channels;                 /* on the device */
    UINT8           sub, bits;                /* bytes a sample takes on the device, and its bits */
    UINT32          rate;                     /* the device's sampling rate */
    UINT32          us;                       /* microseconds between packets */
    UINT32          nominal;                  /* playback: frames a packet at the rate, in 1/65536 */
    volatile UINT32 fpp;                      /*   and what the device's feedback asks for (the nominal without) */
    UINT32          acc;                      /*   the fraction of a frame owed */
    bool            fb_seen;                  /*   feedback has come (logged once) */
    INT16          *ring;
    UINT32          bytes, pages;             /* the ring's */
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
    char           name[96], iname[96];
    char           key[64];                   /* its identity for saved settings (AudioOutput.key) */
} Ua;

/* A streaming interface's setting we can use */
typedef struct {
    bool         found, freq_ctl;             /* (freq_ctl: Audio 1.0, the endpoint has a rate control) */
    UINT8        as, alt, terminal;           /* (terminal: the one the setting is linked to) */
    const UINT8 *ep;
    const UINT8 *fb;                          /* an asynchronous OUT endpoint's feedback endpoint */
    int          channels, sub, bits;
    UINT32       rates[MAX_RATES];           /* (Audio 1.0) the rates it offers */
    int          nrates;
} UaSetting;

/* The rates a device range can be set to: the usual ones */
static const UINT32 g_std_rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000,
                                      88200, 96000, 176400, 192000 };

/* Add to @list (@n of MAX_RATES) the rates from @lo to @hi in steps of
 * @step (0: any): @lo itself when it is @hi, else the usual ones */
static void rates_add(UINT32 *list, int *n, UINT32 lo, UINT32 hi, UINT32 step)
{
    for (int i = -1; i < (int)(sizeof(g_std_rates) / sizeof(g_std_rates[0])); i++) {
        UINT32 r = i < 0 ? lo : g_std_rates[i];
        if (i < 0 && lo != hi) continue;
        if (r < lo || r > hi || (step && (r - lo) % step) || r < 8000 || r > MAX_RATE) continue;
        bool dup = false;
        for (int k = 0; k < *n && !dup; k++) dup = list[k] == r;
        if (!dup && *n < MAX_RATES) list[(*n)++] = r;
    }
}

/* The rate to run at, of @n offered: 48 kHz (what programs mostly play); else the lowest
 * above it; else the highest below it.  0: none */
static UINT32 pick_rate(const UINT32 *list, int n)
{
    UINT32 above = 0, below = 0;
    for (int k = 0; k < n; k++) {
        if (list[k] == AUDIO_RATE) return AUDIO_RATE;
        if (list[k] > AUDIO_RATE && (!above || list[k] < above)) above = list[k];
        if (list[k] < AUDIO_RATE && list[k] > below) below = list[k];
    }
    return above ? above : below;
}

/* "48 kHz", "44.1 kHz" */
static void rate_text(char *out, int cap, UINT32 r)
{
    if (r % 1000 == 0)     ksnprintf(out, cap, "%u kHz", r / 1000);
    else if (r % 100 == 0) ksnprintf(out, cap, "%u.%u kHz", r / 1000, r % 1000 / 100);
    else                   ksnprintf(out, cap, "%u Hz", r);
}

/* "mono", "stereo", "6-channel" */
static void channels_text(char *out, int cap, int ch)
{
    if (ch == 1)      ksnprintf(out, cap, "mono");
    else if (ch == 2) ksnprintf(out, cap, "stereo");
    else              ksnprintf(out, cap, "%d-channel", ch);
}

static Ua *g_ua[MAX_UA];                      /* for the streaming interfaces' probes */

static UINT32 le32(const UINT8 *p) { return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 | (UINT32)p[3] << 24; }

/* A sample into a device slot of @sub bytes (in its top two), and back */
static void slot_put(UINT8 *dst, UINT32 sub, INT32 v)
{
    memset(dst, 0, sub - 2);
    dst[sub - 2] = (UINT8)v;
    dst[sub - 1] = (UINT8)((UINT16)v >> 8);
}

static INT32 slot_get(const UINT8 *src, UINT32 sub) { return (INT16)(src[sub - 2] | src[sub - 1] << 8); }

/* Refill a transfer with the next packets of the mixer's ring, each
 * carrying the frames the device takes in a service interval (from the
 * controller's poll, interrupts off) */
static void fill(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p), sub = s->sub, ch = s->channels, fb = sub * ch, room = psize / fb;
    for (int i = 0; i < packets; i++) {
        UINT8 *dst = data + (UINT32)i * psize;
        s->acc += s->fpp;
        UINT32 n = s->acc >> 16, pos = s->pos;
        s->acc &= 0xFFFF;
        if (n > room) n = room;
        if (sub == 2 && ch == 2) {                       /* the mixer's format: copied */
            UINT32 bytes = n * FRAME, first = s->bytes - pos < bytes ? s->bytes - pos : bytes;
            memcpy(dst, (UINT8 *)s->ring + pos, first);
            if (first < bytes) memcpy(dst + first, s->ring, bytes - first);
        } else {
            for (UINT32 f = 0; f < n; f++, dst += fb) {
                const INT16 *x = (const INT16 *)((UINT8 *)s->ring + (pos + f * FRAME) % s->bytes);
                if (ch == 1) {
                    slot_put(dst, sub, (x[0] + x[1]) / 2);
                } else {
                    slot_put(dst, sub, x[0]);
                    slot_put(dst + sub, sub, x[1]);
                    memset(dst + 2 * sub, 0, (ch - 2) * sub);    /* (rear, centre, LFE: silent) */
                }
            }
        }
        s->pos = (pos + n * FRAME) % s->bytes;
        lens[i] = (UINT16)(n * fb);
    }
}

/* Copy the packets a transfer brought into the recording ring, as stereo
 * (from the controller's poll, interrupts off) */
static void drain(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p), sub = s->sub, ch = s->channels, fb = sub * ch, pos = s->pos;
    for (int i = 0; i < packets; i++) {
        const UINT8 *src = data + (UINT32)i * psize;
        UINT32 frames = lens[i] / fb;
        for (UINT32 f = 0; f < frames; f++, src += fb) {
            INT16 *dst = (INT16 *)((UINT8 *)s->ring + pos);
            dst[0] = (INT16)slot_get(src, sub);
            dst[1] = (INT16)(ch > 1 ? slot_get(src + sub, sub) : dst[0]);
            pos = (pos + FRAME) % s->bytes;
        }
    }
    s->pos = pos;
}

/* Frames a packet, in 1/65536, from what a feedback packet of @len bytes
 * says (@v); 0 if it is not within an eighth of the nominal rate.  Tried
 * as the speed's own format first (full speed: 10.14 frames a
 * millisecond; high speed: 16.16 a microframe), then the others. */
static UINT32 feedback_fpp(const UaStream *s, UINT32 v, int len)
{
    UINT64 as[3] = { (UINT64)v * 4 * s->us / 1000,       /* 10.14 a millisecond */
                     (UINT64)v * s->us / 125,            /* 16.16 a microframe */
                     (UINT64)v * s->us / 1000 };         /* 16.16 a millisecond */
    int order[3] = { len >= 4 ? 1 : 0, len >= 4 ? 0 : 1, 2 };
    for (int k = 0; k < 3; k++) {
        UINT64 f = as[order[k]];
        if (f >= s->nominal - s->nominal / 8 && f <= s->nominal + s->nominal / 8) return (UINT32)f;
    }
    return 0;
}

/* The feedback endpoint's packets: the rate the device really plays at
 * (from the controller's poll, interrupts off) */
static void feedback(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p);
    for (int i = 0; i < packets; i++) {
        const UINT8 *b = data + (UINT32)i * psize;
        if (lens[i] < 3) continue;                       /* (lost, or nothing new) */
        UINT32 v = lens[i] >= 4 ? le32(b) : (UINT32)b[0] | (UINT32)b[1] << 8 | (UINT32)b[2] << 16;
        UINT32 f = feedback_fpp(s, v, lens[i]);
        if (!f) continue;
        s->fpp = f;
        if (!s->fb_seen) {
            s->fb_seen = true;
            UINT64 hz = (UINT64)f * 1000000 / s->us;
            kprintf("[USB] %s: audio feedback: the device plays %u.%03u frames a second (nominal %u)\n",
                    UsbDevName(s->usb), (UINT32)(hz >> 16), (UINT32)((hz & 0xFFFF) * 1000 >> 16), s->rate);
        }
    }
}

static UINT32 position(void *ctx) { return ((UaStream *)ctx)->pos; }

static void stream_free(UaStream *s)
{
    if (s->ring) kernel_free_pages(s->ring, s->pages);
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

static UINT32 le24(const UINT8 *p) { return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16; }

/* The rates an Audio 1.0 Format Type I descriptor offers, into @st;
 * the channels it has (0: not type I, or no rate we can use) */
static int format_ok(const UINT8 *t, UaSetting *st)
{
    if (t[0] < 8 || t[3] != 1) return 0;
    int n = t[7];
    st->nrates = 0;
    if (n == 0 && t[0] >= 14)                            /* continuous: a range */
        rates_add(st->rates, &st->nrates, le24(&t[8]), le24(&t[11]), 0);
    for (int i = 0; n && i < n && 8 + i * 3 + 3 <= t[0]; i++)
        rates_add(st->rates, &st->nrates, le24(&t[8 + i * 3]), le24(&t[8 + i * 3]), 0);
    return st->nrates ? t[4] : 0;
}

/* Whether a setting carries what we play or record (one to eight
 * channels), in samples we can convert */
static bool setting_ok(const UaSetting *s, bool pcm, bool in)
{
    (void)in;
    return pcm && s->ep && s->channels >= 1 && s->channels <= MAX_CH &&
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
            } else if (!v2 && (cur.channels = format_ok(p, &cur)) != 0) {
                cur.sub = p[5];
                cur.bits = p[6];
            }
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 1 && !(p[2] & 0x80) == !in && !cur.ep) {
            cur.ep = p;
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 1 && (p[2] & 0x80) && !in && cur.ep &&
                   !cur.fb && (cur.ep[3] >> 2 & 3) == 1 &&       /* an asynchronous OUT endpoint's feedback: */
                   ((p[3] >> 4 & 3) == 1 || (cur.ep[0] >= 9 && cur.ep[8] == p[2]))) {   /* by usage, or bSynchAddress */
            cur.fb = p;
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

/* Audio 2.0: run the clock of terminal @term at the rate chosen from
 * what its source offers, into *@rate.  Follows the terminal's clock
 * entity to its source, switching selectors to their first input; reads
 * the source's RANGE (none: 48 kHz is asked for), sets the rate where the
 * source lets the host set it, then reads it back: that is the rate.
 * False if there is no source, or it runs at a rate we cannot use. */
static bool set_clock(UsbDev *d, const UsbIface *f, UINT8 term, UINT32 *rate)
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
            UINT8 range[2 + 12 * MAX_RATES], set[4], now[4];
            UINT32 list[MAX_RATES];
            int got = UsbControl(d, 0xA1, RANGE, SAMPLING_FREQ << 8, index, sizeof(range), range), n = 0;
            for (int k = 0; got >= 2 && k < (range[0] | range[1] << 8) && 2 + 12 * k + 12 <= got; k++)
                rates_add(list, &n, le32(&range[2 + 12 * k]), le32(&range[6 + 12 * k]), le32(&range[10 + 12 * k]));
            UINT32 want = n ? pick_rate(list, n) : AUDIO_RATE;
            if (!want) {
                kprintf("[USB] %s: audio clock %u offers no rate NovaOS can use\n", UsbDevName(d), id);
                return false;
            }
            memcpy(set, &want, 4);
            if ((c[5] & 3) == 3) UsbControl(d, 0x21, SET_CUR, SAMPLING_FREQ << 8, index, 4, set);
            UINT32 r = UsbControl(d, 0xA1, SET_CUR, SAMPLING_FREQ << 8, index, 4, now) == 4 ? le32(now) : want;
            char text[16];
            rate_text(text, sizeof(text), r);
            if (r < 8000 || r > MAX_RATE) {
                kprintf("[USB] %s: audio clock %u runs at %u Hz\n", UsbDevName(d), id, r);
                return false;
            }
            kprintf("[USB] %s: audio clock %u at %s\n", UsbDevName(d), id, text);
            *rate = r;
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
    s->usb = d;
    s->as = st->as;
    s->channels = (UINT8)st->channels;
    s->sub = (UINT8)st->sub;
    s->bits = (UINT8)st->bits;
    s->rate = u->v2 ? 0 : pick_rate(st->rates, st->nrates);
    if (u->v2 && !set_clock(d, ac, st->terminal, &s->rate)) return false;
    /* the ring: RING_MS at the device's rate, whole pages */
    s->pages = (UINT32)(((UINT64)s->rate * FRAME * RING_MS / 1000 + PAGE_SIZE - 1) / PAGE_SIZE);
    s->bytes = s->pages * PAGE_SIZE;
    s->ring = kernel_alloc_pages(s->pages);
    if (!s->ring) return false;
    memset(s->ring, 0, s->bytes);
    if (!UsbSetInterface(d, s->as, st->alt) || !(s->pipe = UsbOpenPipe(d, st->ep, 0))) {
        kprintf("[USB] %s: could not open the audio %s stream\n", UsbDevName(d), in ? "input" : "output");
        goto fail;
    }
    /* The frames a packet (1/65536 of one), and the most a packet takes
     * (a frame more where the rate has them vary) */
    s->us = UsbIsoIntervalUs(s->pipe);
    UINT64 dev = (UINT64)s->rate * s->us;
    UINT32 most = (UINT32)(dev / 1000000) + (dev % 1000000 ? 2 : 0);
    UINT32 need = most * s->channels * s->sub;
    char rate[16], chans[16];
    rate_text(rate, sizeof(rate), s->rate);
    channels_text(chans, sizeof(chans), s->channels);
    if (!need || need > UsbIsoPacketSize(s->pipe)) {
        kprintf("[USB] %s: audio endpoint (%u bytes every %u us) cannot carry %s %s\n",
                UsbDevName(d), UsbIsoPacketSize(s->pipe), s->us, rate, chans);
        goto fail_alt;
    }
    s->nominal = s->fpp = (UINT32)((dev << 16) / 1000000);
    s->acc = 0;
    s->fb_seen = false;
    if (st->freq_ctl) {
        UINT8 r3[3] = { s->rate & 0xFF, (s->rate >> 8) & 0xFF, (UINT8)(s->rate >> 16) };
        UsbControl(d, 0x22, SET_CUR, SAMPLING_FREQ << 8, st->ep[2], 3, r3);
    }

    /* Transfers of about 8 ms, QUEUED_US of them in flight */
    int packets = (int)(8000 / s->us) ? (int)(8000 / s->us) : 1;
    int xfers = (int)(QUEUED_US / (s->us * (UINT32)packets));
    if (xfers * packets > 128) xfers = 128 / packets;
    if (xfers < 2) xfers = 2;
    if (!UsbIsoStart(s->pipe, xfers, packets, in ? drain : fill, s)) {
        kprintf("[USB] %s: the controller cannot stream %s the audio device\n", UsbDevName(d), in ? "from" : "to");
        goto fail_alt;
    }
    kprintf("[USB] %s: audio %s, %s %u-bit %s, %u-byte packets every %u us%s\n", UsbDevName(d),
            in ? "input" : "output", rate, s->bits, chans, need, s->us, u->v2 ? " (USB Audio 2.0)" : "");

    /* An asynchronous endpoint's feedback (without it the nominal rate
     * plays on, as before) */
    s->fb = st->fb ? UsbOpenPipe(d, st->fb, 0) : NULL;
    if (s->fb) {
        UINT32 fus = UsbIsoIntervalUs(s->fb);
        int fpk = (int)(8000 / fus) ? (int)(8000 / fus) : 1;
        if (fpk > 32) fpk = 32;
        if (UsbIsoStart(s->fb, 2, fpk, feedback, s))
            kprintf("[USB] %s: audio output is asynchronous: feedback every %u us from endpoint %02x\n",
                    UsbDevName(d), fus, st->fb[2]);
        else {
            kprintf("[USB] %s: the controller cannot stream the audio feedback: the nominal rate plays\n", UsbDevName(d));
            s->fb = NULL;
        }
    }
    return true;

fail_alt:
    UsbSetInterface(d, s->as, 0);
fail:
    s->pipe = NULL;
    stream_free(s);
    return false;
}

/* Name a device the way Windows names an endpoint: "Speakers (Product)",
 * "Microphone (Product)"; "Speakers (2- Product)" when another device
 * already has that name */
static void name_device(Ua *u, char *out, int cap, const char *kind, const char *product)
{
    for (int n = 1; n <= MAX_UA; n++) {
        if (n == 1) ksnprintf(out, cap, "%s (%s)", kind, product);
        else ksnprintf(out, cap, "%s (%d- %s)", kind, n, product);
        bool taken = false;
        for (int i = 0; i < MAX_UA && !taken; i++)
            taken = g_ua[i] && g_ua[i] != u && (!strcmp(g_ua[i]->name, out) || !strcmp(g_ua[i]->iname, out));
        if (!taken) return;
    }
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
        kprintf("[USB] %s: audio device has no output or input NovaOS can use\n", UsbDevName(d));
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
    char product[48];
    if (!UsbDevProductName(d, product, sizeof(product))) strcpy(product, "USB Audio Device");
    /* (what its volume and its being the default are kept under: the same
     * device on the same port next time) */
    ksnprintf(u->key, sizeof(u->key), "VID_%04X&PID_%04X at %s", UsbDevVendor(d), UsbDevProduct(d), UsbDevName(d));
    if (play) {
        name_device(u, u->name, sizeof(u->name), "Speakers", product);
        u->out.name = u->name;
        u->out.key = u->key;
        u->out.ring = u->play.ring;
        u->out.bytes = u->play.bytes;
        u->out.rate = u->play.rate;
        u->out.position = position;
        u->out.ctx = &u->play;
        u->play.attached = AudioOutputAttach(&u->out);
    }
    if (rec) {
        name_device(u, u->iname, sizeof(u->iname), "Microphone", product);
        u->in.name = u->iname;
        u->in.key = u->key;
        u->in.ring = u->rec.ring;
        u->in.bytes = u->rec.bytes;
        u->in.rate = u->rec.rate;
        u->in.position = position;
        u->in.ctx = &u->rec;
        u->rec.attached = AudioInputAttach(&u->in);
    }
    return u;
}
