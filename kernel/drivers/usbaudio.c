/*
 * usbaudio.c — USB Audio Class 1.0 speakers and headsets
 *
 * A UAC1 function is an AudioControl interface (its units and terminals:
 * the feature unit has the mute and volume controls) and one or more
 * AudioStreaming interfaces, listed in the AudioControl header.  A
 * streaming interface's alternate setting 0 has no endpoints (no
 * bandwidth); each other setting has an isochronous endpoint and says
 * which format it carries.  This driver plays: it takes the first setting
 * whose OUT endpoint carries 48 kHz, 16-bit stereo PCM (the mixer's own
 * format, so nothing is converted), switches to it, sets the sampling
 * rate where the endpoint has that control, unmutes the feature unit and
 * sets its volume to 0 dB, and attaches itself to the mixer as an output
 * (audio.h): the newest output plays, as on Windows.
 *
 * The mixer fills a 64 KiB ring ahead of the device, as for HD Audio.  The
 * pipe streams a ring of isochronous transfers (usb.h), one packet per
 * service interval; each finished transfer is refilled from the mixer's
 * ring, and how far that has got is the output's position.  Recording
 * (IN endpoints) and asynchronous endpoints' feedback are not used: the
 * device gets exactly 48 frames a millisecond.  USB Audio 2.0 devices
 * (protocol 0x20) are not supported.
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

typedef struct {
    AudioOutput    out;
    UsbDev        *usb;
    UsbPipe       *pipe;
    UINT8          ac, as;                    /* interface numbers */
    UINT32         packet;                    /* bytes per packet */
    INT16         *ring;
    volatile UINT32 pos;                      /* where the next packet comes from */
    bool           attached;
    char           name[48];
} Ua;

static Ua *g_ua[MAX_UA];                      /* for the streaming interface's probe */

/* Refill a transfer with the next packets of the mixer's ring (from the
 * controller's poll, interrupts off) */
static void fill(UsbPipe *p, UINT8 *data, UINT16 *lens, int packets, void *ctx)
{
    Ua *u = ctx;
    UINT32 psize = UsbIsoPacketSize(p), n = u->packet;
    for (int i = 0; i < packets; i++) {
        UINT8 *dst = data + (UINT32)i * psize;
        UINT32 pos = u->pos, first = RING_BYTES - pos < n ? RING_BYTES - pos : n;
        memcpy(dst, (UINT8 *)u->ring + pos, first);
        if (first < n) memcpy(dst + first, u->ring, n - first);
        u->pos = (pos + n) % RING_BYTES;
        lens[i] = (UINT16)n;
    }
}

static UINT32 position(void *ctx) { return ((Ua *)ctx)->pos; }

static void ua_gone(void *inst)
{
    Ua *u = inst;
    if (u->attached) AudioOutputDetach(&u->out);
    kprintf("[USB] %s: audio output unplugged\n", UsbDevName(u->usb));
    for (int i = 0; i < MAX_UA; i++)
        if (g_ua[i] == u) g_ua[i] = NULL;
    kernel_free_pages(u->ring, RING_PAGES);
    kfree(u);
}

/* Whether a Format Type I descriptor offers 48 kHz, 16-bit stereo */
static bool format_ok(const UINT8 *t)
{
    if (t[0] < 8 || t[3] != 1 || t[4] != 2 || t[5] != 2 || t[6] != 16) return false;
    int n = t[7];
    if (n == 0)                                          /* continuous: a range */
        return t[0] >= 14 && (UINT32)(t[8] | t[9] << 8 | t[10] << 16) <= AUDIO_RATE &&
               (UINT32)(t[11] | t[12] << 8 | t[13] << 16) >= AUDIO_RATE;
    for (int i = 0; i < n && 8 + i * 3 + 3 <= t[0]; i++)
        if ((UINT32)(t[8 + i * 3] | t[9 + i * 3] << 8 | t[10 + i * 3] << 16) == AUDIO_RATE) return true;
    return false;
}

/* The first playback setting of streaming interface @as we can use: its
 * alternate setting, OUT endpoint descriptor and whether that endpoint
 * has a sampling-rate control.  False: none. */
static bool find_setting(UsbDev *d, UINT8 as, UINT8 *alt, const UINT8 **ep, bool *freq_ctl)
{
    int total;
    const UINT8 *cfg = UsbDevConfig(d, &total);
    bool in = false, pcm = false, fmt = false;
    const UINT8 *e = NULL;
    for (int off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        const UINT8 *p = &cfg[off];
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            if (in && pcm && fmt && e) break;
            in = p[2] == as && p[3] != 0 && p[5] == 1 && p[6] == SUB_STREAMING;
            pcm = fmt = false;
            e = NULL;
            *alt = p[3];
            *freq_ctl = false;
        } else if (!in) {
            continue;
        } else if (p[1] == USB_DT_CS_INTERFACE && p[0] >= 7 && p[2] == AS_GENERAL) {
            pcm = (p[5] | p[6] << 8) == FMT_PCM;
        } else if (p[1] == USB_DT_CS_INTERFACE && p[2] == AS_FORMAT_TYPE) {
            fmt = format_ok(p);
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 1 && !(p[2] & 0x80) && !e) {
            e = p;
        } else if (p[1] == USB_DT_CS_ENDPOINT && p[0] >= 4 && p[2] == EP_GENERAL && e) {
            *freq_ctl = (p[3] & 1) != 0;
        }
    }
    *ep = e;
    return in && pcm && fmt && e;
}

/* Unmute the feature unit and set every volume control to 0 dB */
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

void *UsbAudioProbe(UsbDev *d, const UsbIface *f)
{
    if (f->sub == SUB_STREAMING) {                           /* taken with its control interface */
        for (int i = 0; i < MAX_UA; i++)
            if (g_ua[i] && g_ua[i]->usb == d && g_ua[i]->as == f->number) return g_ua[i];
        return NULL;
    }
    if (f->sub != SUB_CONTROL || f->proto == 0x20) return NULL;           /* (0x20: Audio 2.0) */

    /* The header lists the streaming interfaces: the first we can play on */
    int off = 0;
    const UINT8 *h = NULL;
    for (const UINT8 *c; (c = UsbIfaceFind(f, USB_DT_CS_INTERFACE, &off)) != NULL;)
        if (c[0] >= 8 && c[2] == AC_HEADER) { h = c; break; }
    if (!h) return NULL;
    UINT8 alt = 0, as = 0;
    const UINT8 *ep = NULL;
    bool freq_ctl = false, found = false;
    for (int i = 0; i < h[7] && 8 + i < h[0] && !found; i++) {
        as = h[8 + i];
        found = find_setting(d, as, &alt, &ep, &freq_ctl);
    }
    if (!found) {
        kprintf("[USB] %s: audio device has no 48 kHz 16-bit stereo output\n", UsbDevName(d));
        return NULL;
    }
    int slot = -1;
    for (int i = 0; i < MAX_UA && slot < 0; i++)
        if (!g_ua[i]) slot = i;
    if (slot < 0) return NULL;

    Ua *u = kzalloc(sizeof(Ua));
    if (!u) return NULL;
    u->ring = kernel_alloc_pages(RING_PAGES);
    if (!u->ring) { kfree(u); return NULL; }
    memset(u->ring, 0, RING_BYTES);
    u->usb = d;
    u->ac = f->number;
    u->as = as;

    if (!UsbSetInterface(d, as, alt) || !(u->pipe = UsbOpenPipe(d, ep, 0))) {
        kprintf("[USB] %s: could not open the audio stream\n", UsbDevName(d));
        goto fail;
    }
    UINT32 us = UsbIsoIntervalUs(u->pipe);
    u->packet = (UINT32)((UINT64)AUDIO_RATE * us / 1000000) * FRAME;
    if ((UINT64)AUDIO_RATE * us % 1000000 || !u->packet || u->packet > UsbIsoPacketSize(u->pipe)) {
        kprintf("[USB] %s: audio endpoint (%u bytes every %u us) cannot carry 48 kHz stereo\n",
                UsbDevName(d), UsbIsoPacketSize(u->pipe), us);
        goto fail_alt;
    }
    if (freq_ctl) {
        UINT8 rate[3] = { AUDIO_RATE & 0xFF, (AUDIO_RATE >> 8) & 0xFF, AUDIO_RATE >> 16 };
        UsbControl(d, 0x22, SET_CUR, SAMPLING_FREQ << 8, ep[2], 3, rate);
    }
    unmute(d, f);

    /* Transfers of about 8 ms, QUEUED_US of them in flight */
    int packets = (int)(8000 / us) ? (int)(8000 / us) : 1;
    int xfers = (int)(QUEUED_US / (us * (UINT32)packets));
    if (xfers * packets > 128) xfers = 128 / packets;
    if (xfers < 2) xfers = 2;
    if (!UsbIsoStart(u->pipe, xfers, packets, fill, u)) {
        kprintf("[USB] %s: the controller cannot stream to the audio device\n", UsbDevName(d));
        goto fail_alt;
    }

    ksnprintf(u->name, sizeof(u->name), "USB Audio Device (%s)", UsbDevName(d));
    u->out.name = u->name;
    u->out.ring = u->ring;
    u->out.bytes = RING_BYTES;
    u->out.position = position;
    u->out.ctx = u;
    g_ua[slot] = u;
    UsbBind(d, u, ua_gone);
    kprintf("[USB] %s: audio output, 48 kHz 16-bit stereo, %u-byte packets every %u us\n",
            UsbDevName(d), u->packet, us);
    u->attached = AudioOutputAttach(&u->out);
    return u;

fail_alt:
    UsbSetInterface(d, as, 0);
fail:
    kernel_free_pages(u->ring, RING_PAGES);
    kfree(u);
    return NULL;
}
