/*
 * usbaudio.c — USB Audio Class 1.0 speakers, headsets and microphones
 *
 * A UAC1 function is an AudioControl interface (its units and terminals:
 * the feature unit has the mute and volume controls) and one or more
 * AudioStreaming interfaces, listed in the AudioControl header.  A
 * streaming interface's alternate setting 0 has no endpoints (no
 * bandwidth); each other setting has an isochronous endpoint and says
 * which format it carries.  This driver plays on the first streaming
 * interface with a setting whose OUT endpoint carries 48 kHz, 16-bit
 * stereo PCM (the mixer's own format, so nothing is converted), and
 * records from the first with a setting whose IN endpoint carries 48 kHz
 * 16-bit PCM, mono or stereo.  For each it switches to that setting, sets
 * the sampling rate where the endpoint has that control, and attaches
 * itself to the mixer (audio.h): as an output, and as an input; the
 * newest of each is used, as on Windows.  The feature units are unmuted
 * and set to 0 dB.
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
 *
 * USB Audio 2.0 devices (protocol 0x20) are not supported.
 */

#include "usb.h"
#include "audio.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define SUB_CONTROL     1
#define SUB_STREAMING   2
#define AC_HEADER       0x01
#define AC_FEATURE_UNIT 0x06
#define AS_GENERAL      0x01
#define AS_FORMAT_TYPE  0x02
#define EP_GENERAL      0x01
#define FMT_PCM         0x0001
#define SET_CUR         0x01
#define MUTE_CONTROL    0x01
#define VOLUME_CONTROL  0x02
#define SAMPLING_FREQ   0x01

#define RING_PAGES      16                    /* 64 KiB: 341 ms, like HD Audio's ring */
#define RING_BYTES      (RING_PAGES * PAGE_SIZE)
#define FRAME           4                     /* s16 stereo */
#define QUEUED_US       64000                 /* streamed ahead: well over a timer tick */
#define MAX_UA          8

/* One direction's stream */
typedef struct {
    UsbPipe        *pipe;
    UINT8           as;                       /* the streaming interface */
    UINT8           channels;                 /* 1 or 2 (recording) */
    UINT32          packet;                   /* bytes per packet (playback) */
    INT16          *ring;
    volatile UINT32 pos;                      /* playback: where the next packet comes from; recording: written up to */
    bool            attached;
} UaStream;

typedef struct {
    AudioOutput    out;
    AudioInput     in;
    UsbDev        *usb;
    UINT8          ac;                        /* the control interface */
    UaStream       play, rec;
    char           name[48], iname[64];
} Ua;

static Ua *g_ua[MAX_UA];                      /* for the streaming interfaces' probes */

/* Refill a transfer with the next packets of the mixer's ring (from the
 * controller's poll, interrupts off) */
static void fill(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p), n = s->packet;
    for (int i = 0; i < packets; i++) {
        UINT8 *dst = data + (UINT32)i * psize;
        UINT32 pos = s->pos, first = RING_BYTES - pos < n ? RING_BYTES - pos : n;
        memcpy(dst, (UINT8 *)s->ring + pos, first);
        if (first < n) memcpy(dst + first, s->ring, n - first);
        s->pos = (pos + n) % RING_BYTES;
        lens[i] = (UINT16)n;
    }
}

/* Copy the packets a transfer brought into the recording ring (from the
 * controller's poll, interrupts off) */
static void drain(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    UaStream *s = ctx;
    UINT32 psize = UsbIsoPacketSize(p), in_frame = 2u * s->channels;
    UINT32 pos = s->pos;
    for (int i = 0; i < packets; i++) {
        const INT16 *src = (const INT16 *)(data + (UINT32)i * psize);
        UINT32 frames = lens[i] / in_frame;
        for (UINT32 f = 0; f < frames; f++) {
            INT16 *dst = (INT16 *)((UINT8 *)s->ring + pos);
            dst[0] = src[f * s->channels];
            dst[1] = src[f * s->channels + s->channels - 1];
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

/* Whether a Format Type I descriptor offers 48 kHz, 16-bit, @ch channels
 * (0: 1 or 2); the channels it has */
static int format_ok(const UINT8 *t, int ch)
{
    if (t[0] < 8 || t[3] != 1 || t[5] != 2 || t[6] != 16) return 0;
    if (ch ? t[4] != ch : t[4] != 1 && t[4] != 2) return 0;
    int n = t[7];
    if (n == 0)                                          /* continuous: a range */
        return t[0] >= 14 && (UINT32)(t[8] | t[9] << 8 | t[10] << 16) <= AUDIO_RATE &&
               (UINT32)(t[11] | t[12] << 8 | t[13] << 16) >= AUDIO_RATE ? t[4] : 0;
    for (int i = 0; i < n && 8 + i * 3 + 3 <= t[0]; i++)
        if ((UINT32)(t[8 + i * 3] | t[9 + i * 3] << 8 | t[10 + i * 3] << 16) == AUDIO_RATE) return t[4];
    return 0;
}

/* The first setting of streaming interface @as we can use, playing (OUT,
 * stereo) or recording (@in: mono or stereo): its alternate setting,
 * endpoint descriptor, channels and whether that endpoint has a
 * sampling-rate control.  False: none. */
static bool find_setting(UsbDev *d, UINT8 as, bool in, UINT8 *alt, const UINT8 **ep, int *channels, bool *freq_ctl)
{
    int total;
    const UINT8 *cfg = UsbDevConfig(d, &total);
    bool ours = false, pcm = false;
    int ch = 0;
    const UINT8 *e = NULL;
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            if (ours && pcm && ch && e) break;
            ours = p[2] == as && p[3] != 0 && p[5] == 1 && p[6] == SUB_STREAMING;
            pcm = false;
            ch = 0;
            e = NULL;
            *alt = p[3];
            *freq_ctl = false;
        } else if (!ours) {
            continue;
        } else if (p[1] == USB_DT_CS_INTERFACE && p[0] >= 7 && p[2] == AS_GENERAL) {
            pcm = (p[5] | p[6] << 8) == FMT_PCM;
        } else if (p[1] == USB_DT_CS_INTERFACE && p[2] == AS_FORMAT_TYPE) {
            ch = format_ok(p, in ? 0 : 2);
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 1 && !(p[2] & 0x80) == !in && !e) {
            e = p;
        } else if (p[1] == USB_DT_CS_ENDPOINT && p[0] >= 4 && p[2] == EP_GENERAL && e) {
            *freq_ctl = (p[3] & 1) != 0;
        }
    }
    *ep = e;
    *channels = ch;
    return ours && pcm && ch && e;
}

/* Unmute the feature units and set every volume control to 0 dB */
static void unmute(UsbDev *d, const UsbIface *f)
{
    int off = 0;
    for (const UINT8 *u; (u = UsbIfaceFind(f, USB_DT_CS_INTERFACE, &off)) != NULL;) {
        if (u[0] < 7 || u[2] != AC_FEATURE_UNIT || !u[5]) continue;
        int size = u[5], channels = (u[0] - 7) / size;      /* master (0), then each channel */
        UINT16 index = (UINT16)(u[3] << 8 | f->number);
        for (int c = 0; c < channels; c++) {
            UINT8 ctl = u[6 + c * size];
            UINT8 off0[2] = { 0, 0 };                        /* not muted; 0 dB (1/256 dB units) */
            if (ctl & 1) UsbControl(d, 0x21, SET_CUR, (UINT16)(MUTE_CONTROL << 8 | c), index, 1, off0);
            if (ctl & 2) UsbControl(d, 0x21, SET_CUR, (UINT16)(VOLUME_CONTROL << 8 | c), index, 2, off0);
        }
    }
}

/* Switch to alternate setting @alt of @s->as, open endpoint @ep and start
 * streaming it; false (back on setting 0) if that fails */
static bool stream_start(Ua *u, UaStream *s, bool in, UINT8 alt, const UINT8 *ep, bool freq_ctl)
{
    UsbDev *d = u->usb;
    s->ring = kernel_alloc_pages(RING_PAGES);
    if (!s->ring) return false;
    memset(s->ring, 0, RING_BYTES);
    if (!UsbSetInterface(d, s->as, alt) || !(s->pipe = UsbOpenPipe(d, ep, 0))) {
        kprintf("[USB] %s: could not open the audio %s stream\n", UsbDevName(d), in ? "input" : "output");
        goto fail;
    }
    UINT32 us = UsbIsoIntervalUs(s->pipe);
    UINT32 frame = in ? 2u * s->channels : FRAME;
    UINT32 need = (UINT32)((UINT64)AUDIO_RATE * us / 1000000) * frame;
    if ((UINT64)AUDIO_RATE * us % 1000000 || !need || need > UsbIsoPacketSize(s->pipe)) {
        kprintf("[USB] %s: audio endpoint (%u bytes every %u us) cannot carry 48 kHz %s\n",
                UsbDevName(d), UsbIsoPacketSize(s->pipe), us, s->channels == 1 ? "mono" : "stereo");
        goto fail_alt;
    }
    s->packet = need;
    if (freq_ctl) {
        UINT8 rate[3] = { AUDIO_RATE & 0xFF, (AUDIO_RATE >> 8) & 0xFF, AUDIO_RATE >> 16 };
        UsbControl(d, 0x22, SET_CUR, SAMPLING_FREQ << 8, ep[2], 3, rate);
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
    kprintf("[USB] %s: audio %s, 48 kHz 16-bit %s, %u-byte packets every %u us\n", UsbDevName(d),
            in ? "input" : "output", s->channels == 1 ? "mono" : "stereo", s->packet, us);
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
    if (f->sub != SUB_CONTROL || f->proto == 0x20) return NULL;           /* (0x20: Audio 2.0) */

    /* The header lists the streaming interfaces: the first we can play
     * on, and the first we can record from */
    int off = 0;
    const UINT8 *h = NULL;
    for (const UINT8 *c; (c = UsbIfaceFind(f, USB_DT_CS_INTERFACE, &off)) != NULL;)
        if (c[0] >= 8 && c[2] == AC_HEADER) { h = c; break; }
    if (!h) return NULL;
    struct { bool found, freq_ctl; UINT8 as, alt; const UINT8 *ep; int channels; } dir[2] = { 0 };
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < h[7] && 8 + i < h[0] && !dir[k].found; i++) {
            dir[k].as = h[8 + i];
            dir[k].found = find_setting(d, dir[k].as, k == 1, &dir[k].alt, &dir[k].ep, &dir[k].channels,
                                        &dir[k].freq_ctl);
        }
    if (!dir[0].found && !dir[1].found) {
        kprintf("[USB] %s: audio device has no 48 kHz 16-bit output or input\n", UsbDevName(d));
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
    u->play.as = dir[0].as;
    u->play.channels = 2;
    u->rec.as = dir[1].as;
    u->rec.channels = (UINT8)dir[1].channels;
    bool play = dir[0].found && stream_start(u, &u->play, false, dir[0].alt, dir[0].ep, dir[0].freq_ctl);
    bool rec = dir[1].found && stream_start(u, &u->rec, true, dir[1].alt, dir[1].ep, dir[1].freq_ctl);
    if (!play && !rec) { kfree(u); return NULL; }
    unmute(d, f);

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
