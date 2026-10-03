/*
 * hda.c — Intel High Definition Audio controller and codec setup
 *
 * Register layout and verbs follow Intel's "High Definition Audio
 * Specification" rev. 1.0a.  The driver polls (no interrupts): commands go
 * through the CORB/RIRB rings and the mixer reads the stream's link
 * position (LPIB) every tick.
 *
 * Codec setup is the generic one: in each codec's audio function group,
 * every output-capable pin with something attached is routed, through
 * mixers and selectors, to an audio output converter (DAC).  All of those
 * DACs listen to the one output stream, which plays a ring of 48 kHz
 * 16-bit stereo frames.  That covers QEMU's hda-output/hda-duplex/
 * hda-micro codecs and simple onboard codecs.
 *
 * Recording is the mirror image: one audio input converter (ADC) with a
 * path to an input pin that has something attached (a microphone first,
 * then line in) records into a second ring, 48 kHz 16-bit stereo too, on
 * the first input stream.
 */

#include "hda.h"
#include "../hal/pci.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../arch/x86_64/cpu.h"

/* Controller registers */
#define GCAP        0x00
#define GCTL        0x08
#define STATESTS    0x0E
#define INTCTL      0x20
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E
#define DPLBASE     0x70
#define DPUBASE     0x74

/* Stream descriptor registers (from the descriptor's base) */
#define SD_CTL      0x00
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

#define GCTL_CRST       (1u << 0)
#define SD_CTL_SRST     (1u << 0)
#define SD_CTL_RUN      (1u << 1)

/* Verbs and parameters */
#define VERB_GET_PARAM      0xF00
#define VERB_GET_CONN_LIST  0xF02
#define VERB_SET_CONN_SEL   0x701
#define VERB_SET_POWER      0x705
#define VERB_SET_STREAM     0x706
#define VERB_SET_PIN_CTL    0x707
#define VERB_SET_EAPD       0x70C
#define VERB_GET_CONFIG     0xF1C
#define VERB4_SET_FORMAT    0x2
#define VERB4_SET_AMP       0x3

#define PAR_VENDOR      0x00
#define PAR_NODES       0x04
#define PAR_FG_TYPE     0x05
#define PAR_WCAPS       0x09
#define PAR_PINCAPS     0x0C
#define PAR_IN_AMP      0x0D
#define PAR_CONN_LEN    0x0E
#define PAR_OUT_AMP     0x12

#define WT_OUTPUT   0x0
#define WT_INPUT    0x1
#define WT_MIXER    0x2
#define WT_SELECTOR 0x3
#define WT_PIN      0x4

#define WCAP_IN_AMP     (1u << 1)
#define WCAP_OUT_AMP    (1u << 2)
#define WCAP_CONN_LIST  (1u << 8)
#define WCAP_POWER      (1u << 10)

#define PINCAP_OUT      (1u << 4)
#define PINCAP_IN       (1u << 5)
#define PINCAP_HP       (1u << 3)
#define PINCAP_EAPD     (1u << 16)

#define STREAM_TAG  1
#define IN_STREAM_TAG 2
#define FMT_48K_16_STEREO 0x0011        /* base 48 kHz, x1 /1, 16 bits, 2 channels */

#define RING_ENTRIES 16                 /* BDL entries, a page each: 64 KiB = 341 ms */
#define RING_BYTES   (RING_ENTRIES * PAGE_SIZE)
#define MAX_NODES    128
#define MAX_CONN     32

typedef struct __attribute__((packed)) {
    UINT64 addr;
    UINT32 len;
    UINT32 flags;
} BdlEntry;

typedef struct {
    UINT32 caps, pincaps, config;
    UINT8  type;
    UINT8  nconn;
    UINT8  conn[MAX_CONN];
    bool   used;                        /* on a configured path */
} Widget;

static struct {
    bool            present;
    char            name[96];
    volatile UINT8 *mmio;
    volatile UINT32 *corb;
    volatile UINT64 *rirb;
    UINT16          rirb_rp;
    BdlEntry       *bdl;
    INT16          *ring;
    UINT32          sd;                 /* the output stream descriptor's offset */
    Widget          w[MAX_NODES];
    int             outputs;            /* pins routed */
    /* recording */
    int             inputs;             /* ADCs routed to an input pin */
    UINT32          isd;                /* the input stream descriptor's offset */
    BdlEntry       *ibdl;
    INT16          *iring;
    bool            irunning;
    char            iname[48];          /* what is recorded: "Microphone", "Line in" */
} g;

static inline UINT8  rd8(UINT32 r)            { return *(volatile UINT8 *)(g.mmio + r); }
static inline UINT16 rd16(UINT32 r)           { return *(volatile UINT16 *)(g.mmio + r); }
static inline UINT32 rd32(UINT32 r)           { return *(volatile UINT32 *)(g.mmio + r); }
static inline void   wr8(UINT32 r, UINT8 v)   { *(volatile UINT8 *)(g.mmio + r) = v; }
static inline void   wr16(UINT32 r, UINT16 v) { *(volatile UINT16 *)(g.mmio + r) = v; }
static inline void   wr32(UINT32 r, UINT32 v) { *(volatile UINT32 *)(g.mmio + r) = v; }
static inline UINT64 phys(const volatile void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }

static void delay_ms(int ms)
{
    UINT64 end = sched_ticks() + (UINT64)(ms + 9) / 10;
    if (!(read_rflags() & 0x200)) {                 /* interrupts off (boot): spin */
        for (volatile int i = 0; i < ms * 20000; i++) { }
        return;
    }
    while (sched_ticks() < end) pause_cpu();
}

/* -----------------------------------------------------------------------
 * Commands (CORB/RIRB)
 * ----------------------------------------------------------------------- */
static bool cmd(UINT32 verb, UINT32 *resp)
{
    UINT16 wp = (UINT16)((rd16(CORBWP) + 1) & 0xFF);
    g.corb[wp] = verb;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr16(CORBWP, wp);
    for (int i = 0; i < 200000; i++) {
        UINT16 hw = rd16(RIRBWP) & 0xFF;
        while (g.rirb_rp != hw) {
            g.rirb_rp = (UINT16)((g.rirb_rp + 1) & 0xFF);
            UINT64 e = g.rirb[g.rirb_rp];
            wr8(RIRBSTS, 0x05);                     /* let the next response in */
            if ((e >> 32) & (1u << 4)) continue;    /* unsolicited */
            if (resp) *resp = (UINT32)e;
            return true;
        }
        pause_cpu();
    }
    return false;
}

static UINT32 verb12(int cad, int nid, UINT32 v, UINT32 payload)
{
    UINT32 r = 0;
    if (!cmd(((UINT32)cad << 28) | ((UINT32)nid << 20) | (v << 8) | (payload & 0xFF), &r)) return 0;
    return r;
}

static UINT32 verb4(int cad, int nid, UINT32 v, UINT32 payload)
{
    UINT32 r = 0;
    if (!cmd(((UINT32)cad << 28) | ((UINT32)nid << 20) | (v << 16) | (payload & 0xFFFF), &r)) return 0;
    return r;
}

static UINT32 param(int cad, int nid, UINT32 p) { return verb12(cad, nid, VERB_GET_PARAM, p); }

/* -----------------------------------------------------------------------
 * Codec: routing pins to DACs
 * ----------------------------------------------------------------------- */
static void read_connections(int cad, int nid, Widget *w)
{
    w->nconn = 0;
    if (!(w->caps & WCAP_CONN_LIST)) return;
    UINT32 len = param(cad, nid, PAR_CONN_LEN);
    bool lng = len & 0x80;
    int n = (int)(len & 0x7F), per = lng ? 2 : 4, bits = lng ? 16 : 8;
    UINT32 prev = 0;
    for (int i = 0; i < n; i++) {
        UINT32 r = verb12(cad, nid, VERB_GET_CONN_LIST, (UINT32)(i - i % per));
        UINT32 e = (r >> ((i % per) * bits)) & ((1u << bits) - 1);
        UINT32 range = e & (1u << (bits - 1));
        e &= (1u << (bits - 1)) - 1;
        if (range && prev) {                        /* prev+1 .. e */
            for (UINT32 x = prev + 1; x <= e && w->nconn < MAX_CONN; x++) w->conn[w->nconn++] = (UINT8)x;
        } else if (w->nconn < MAX_CONN) {
            w->conn[w->nconn++] = (UINT8)e;
        }
        prev = e;
    }
}

static void unmute_out(int cad, int nid)
{
    Widget *w = &g.w[nid];
    if (!(w->caps & WCAP_OUT_AMP)) return;
    UINT32 caps = param(cad, nid, PAR_OUT_AMP);
    UINT32 gain = caps & 0x7F;                      /* the 0 dB step */
    if (!gain) gain = (caps >> 8) & 0x7F;
    verb4(cad, nid, VERB4_SET_AMP, 0xB000 | gain);  /* output, left+right, unmuted */
}

static void unmute_in(int cad, int nid, int index)
{
    Widget *w = &g.w[nid];
    if (!(w->caps & WCAP_IN_AMP)) return;
    UINT32 caps = param(cad, nid, PAR_IN_AMP);
    UINT32 gain = caps & 0x7F;
    if (!gain) gain = (caps >> 8) & 0x7F;
    verb4(cad, nid, VERB4_SET_AMP, 0x7000 | ((UINT32)index << 8) | gain);  /* input @index */
}

/* Find a DAC below @nid; on the way back, select and unmute the path */
static bool route(int cad, int nid, int depth)
{
    if (nid <= 0 || nid >= MAX_NODES || depth > 5) return false;
    Widget *w = &g.w[nid];
    if (w->type == WT_OUTPUT) {
        if (!w->used) {
            w->used = true;
            if (w->caps & WCAP_POWER) verb12(cad, nid, VERB_SET_POWER, 0);
            verb12(cad, nid, VERB_SET_STREAM, STREAM_TAG << 4);   /* channels 0/1 */
            verb4(cad, nid, VERB4_SET_FORMAT, FMT_48K_16_STEREO);
            unmute_out(cad, nid);
        }
        return true;
    }
    if (depth > 0 && w->type != WT_MIXER && w->type != WT_SELECTOR) return false;
    for (int i = 0; i < w->nconn; i++) {
        if (!route(cad, w->conn[i], depth + 1)) continue;
        if (w->caps & WCAP_POWER) verb12(cad, nid, VERB_SET_POWER, 0);
        if (w->type == WT_MIXER) unmute_in(cad, nid, i);
        else if (w->nconn > 1) verb12(cad, nid, VERB_SET_CONN_SEL, (UINT32)i);
        unmute_out(cad, nid);
        w->used = true;
        return true;
    }
    return false;
}

/* Find an input pin below @nid (an ADC, or a mixer or selector on its
 * way) of device type @dev; select and unmute the path back up */
static bool route_in(int cad, int nid, UINT32 dev, int depth)
{
    if (nid <= 0 || nid >= MAX_NODES || depth > 5) return false;
    Widget *w = &g.w[nid];
    if (w->type == WT_PIN) {
        if (!(w->pincaps & PINCAP_IN) || (w->config >> 30) == 1) return false;
        if (((w->config >> 20) & 0xF) != dev) return false;
        if (w->caps & WCAP_POWER) verb12(cad, nid, VERB_SET_POWER, 0);
        verb12(cad, nid, VERB_SET_PIN_CTL, 0x20);                   /* input enabled */
        unmute_in(cad, nid, 0);
        return true;
    }
    if (depth > 0 && w->type != WT_MIXER && w->type != WT_SELECTOR) return false;
    for (int i = 0; i < w->nconn; i++) {
        if (!route_in(cad, w->conn[i], dev, depth + 1)) continue;
        if (w->caps & WCAP_POWER) verb12(cad, nid, VERB_SET_POWER, 0);
        if (w->type == WT_MIXER) unmute_in(cad, nid, i);
        else {
            if (w->nconn > 1) verb12(cad, nid, VERB_SET_CONN_SEL, (UINT32)i);
            unmute_in(cad, nid, w->type == WT_INPUT ? i : 0);
        }
        if (w->type != WT_INPUT) unmute_out(cad, nid);
        return true;
    }
    return false;
}

/* One ADC recording from a microphone, else line in, else any input jack */
static void setup_input(int cad, int ws, int wn)
{
    static const struct { UINT32 dev; const char *name; } prefs[] = {
        { 0xA, "Microphone" }, { 0x8, "Line in" }, { 0x9, "Auxiliary input" }, { 0x3, "CD" } };
    for (unsigned k = 0; k < sizeof(prefs) / sizeof(prefs[0]) && !g.inputs; k++)
        for (int nid = ws; nid < ws + wn && nid < MAX_NODES && !g.inputs; nid++) {
            Widget *w = &g.w[nid];
            if (w->type != WT_INPUT || !route_in(cad, nid, prefs[k].dev, 0)) continue;
            if (w->caps & WCAP_POWER) verb12(cad, nid, VERB_SET_POWER, 0);
            verb12(cad, nid, VERB_SET_STREAM, IN_STREAM_TAG << 4);
            verb4(cad, nid, VERB4_SET_FORMAT, FMT_48K_16_STEREO);
            strncpy(g.iname, prefs[k].name, sizeof(g.iname) - 1);
            g.inputs++;
        }
}

static void setup_codec(int cad)
{
    UINT32 vendor = param(cad, 0, PAR_VENDOR);
    UINT32 nodes = param(cad, 0, PAR_NODES);
    int start = (int)((nodes >> 16) & 0xFF), count = (int)(nodes & 0xFF);
    for (int fg = start; fg < start + count; fg++) {
        if ((param(cad, fg, PAR_FG_TYPE) & 0xFF) != 1) continue;     /* audio function group */
        verb12(cad, fg, VERB_SET_POWER, 0);
        UINT32 sub = param(cad, fg, PAR_NODES);
        int ws = (int)((sub >> 16) & 0xFF), wn = (int)(sub & 0xFF);
        memset(g.w, 0, sizeof(g.w));
        for (int nid = ws; nid < ws + wn && nid < MAX_NODES; nid++) {
            Widget *w = &g.w[nid];
            w->caps = param(cad, nid, PAR_WCAPS);
            w->type = (UINT8)((w->caps >> 20) & 0xF);
            if (w->type == WT_PIN) {
                w->pincaps = param(cad, nid, PAR_PINCAPS);
                w->config = verb12(cad, nid, VERB_GET_CONFIG, 0);
            }
            read_connections(cad, nid, w);
        }
        int routed = 0;
        for (int nid = ws; nid < ws + wn && nid < MAX_NODES; nid++) {
            Widget *w = &g.w[nid];
            if (w->type != WT_PIN || !(w->pincaps & PINCAP_OUT)) continue;
            if ((w->config >> 30) == 1) continue;                    /* nothing attached */
            UINT32 dev = (w->config >> 20) & 0xF;
            if (dev > 5) continue;          /* line out, speaker, HP, CD, S/PDIF, digital out */
            if (!route(cad, nid, 0)) continue;
            if (w->caps & WCAP_POWER) verb12(cad, nid, VERB_SET_POWER, 0);
            verb12(cad, nid, VERB_SET_PIN_CTL, 0x40 | ((w->pincaps & PINCAP_HP) && dev == 2 ? 0x80 : 0));
            if (w->pincaps & PINCAP_EAPD) verb12(cad, nid, VERB_SET_EAPD, 0x02);
            unmute_out(cad, nid);
            routed++;
        }
        int had = g.inputs;
        setup_input(cad, ws, wn);
        kprintf("[HDA] Codec %d (%04x:%04x): %d output%s%s%s\n", cad, vendor >> 16, vendor & 0xFFFF,
                routed, routed == 1 ? "" : "s", g.inputs > had ? ", recording from " : "",
                g.inputs > had ? g.iname : "");
        g.outputs += routed;
    }
}

/* -----------------------------------------------------------------------
 * Controller
 * ----------------------------------------------------------------------- */
static bool reset_controller(void)
{
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wr32(GCTL, rd32(GCTL) & ~GCTL_CRST);
    for (int i = 0; i < 1000 && (rd32(GCTL) & GCTL_CRST); i++) delay_ms(1);
    delay_ms(1);
    wr32(GCTL, rd32(GCTL) | GCTL_CRST);
    for (int i = 0; i < 1000 && !(rd32(GCTL) & GCTL_CRST); i++) delay_ms(1);
    if (!(rd32(GCTL) & GCTL_CRST)) return false;
    delay_ms(2);                                    /* codecs announce themselves (521 us) */
    return true;
}

static void setup_rings(void)
{
    if (!g.corb) {
        UINT8 *page = kernel_alloc_pages(1);        /* CORB (1 KiB) + RIRB (2 KiB) */
        g.corb = (volatile UINT32 *)page;
        g.rirb = (volatile UINT64 *)(page + 2048);
    }
    memset((void *)g.corb, 0, PAGE_SIZE);

    wr32(CORBLBASE, (UINT32)phys(g.corb));
    wr32(CORBUBASE, (UINT32)(phys(g.corb) >> 32));
    wr8(CORBSIZE, 2);                               /* 256 entries */
    wr16(CORBRP, 0x8000);                           /* reset the read pointer */
    for (int i = 0; i < 1000 && !(rd16(CORBRP) & 0x8000); i++) pause_cpu();
    wr16(CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(CORBRP) & 0x8000); i++) pause_cpu();
    wr16(CORBWP, 0);
    wr8(CORBCTL, 0x02);                             /* DMA on */

    wr32(RIRBLBASE, (UINT32)phys(g.rirb));
    wr32(RIRBUBASE, (UINT32)(phys(g.rirb) >> 32));
    wr8(RIRBSIZE, 2);
    wr16(RIRBWP, 0x8000);                           /* reset the write pointer */
    wr16(RINTCNT, 1);
    g.rirb_rp = 0;
    wr8(RIRBCTL, 0x03);                             /* DMA on; response status (polled, INTCTL is off) */
}

/* Reset stream descriptor @sd and run it over the ring @bdl describes */
static void program_sd(UINT32 sd, BdlEntry *bdl, UINT32 tag, bool run)
{
    wr32(sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (rd32(sd + SD_CTL) & SD_CTL_RUN); i++) pause_cpu();
    wr32(sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 1000 && !(rd32(sd + SD_CTL) & SD_CTL_SRST); i++) pause_cpu();
    wr32(sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (rd32(sd + SD_CTL) & SD_CTL_SRST); i++) pause_cpu();
    wr8(sd + SD_STS, 0x1C);                         /* clear status */

    wr32(sd + SD_BDPL, (UINT32)phys(bdl));
    wr32(sd + SD_BDPU, (UINT32)(phys(bdl) >> 32));
    wr32(sd + SD_CBL, RING_BYTES);
    wr16(sd + SD_LVI, RING_ENTRIES - 1);
    wr16(sd + SD_FMT, FMT_48K_16_STEREO);
    wr32(sd + SD_CTL, (tag << 20));
    if (run) wr32(sd + SD_CTL, (tag << 20) | SD_CTL_RUN);
}

static void program_stream(void)
{
    program_sd(g.sd, g.bdl, STREAM_TAG, true);
    if (g.ibdl) program_sd(g.isd, g.ibdl, IN_STREAM_TAG, g.irunning);
}

static BdlEntry *make_ring(INT16 **ring)
{
    BdlEntry *bdl = kernel_alloc_pages(1);
    *ring = kernel_alloc_pages(RING_ENTRIES);
    if (!bdl || !*ring) return NULL;
    memset(bdl, 0, PAGE_SIZE);
    memset(*ring, 0, RING_BYTES);
    for (int i = 0; i < RING_ENTRIES; i++) {
        bdl[i].addr = phys((UINT8 *)*ring + i * PAGE_SIZE);
        bdl[i].len = PAGE_SIZE;
        bdl[i].flags = 0;
    }
    return bdl;
}

static bool start_stream(UINT16 gcap)
{
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) return false;
    g.sd = 0x80 + (UINT32)iss * 0x20;               /* the first output stream */
    g.bdl = make_ring(&g.ring);
    if (!g.bdl) return false;
    if (iss && g.inputs) {                          /* recording: the first input stream, started on demand */
        g.isd = 0x80;
        g.ibdl = make_ring(&g.iring);
        if (!g.ibdl) g.inputs = 0;
    }
    program_stream();
    return true;
}

bool HdaInit(void)
{
    PciDevice d;
    if (!PciFindClass(0x04, 0x03, 0x00, 0, &d)) {
        kprintf("[HDA] No HD Audio controller\n");
        return false;
    }
    UINT64 bar = PciBarAddress(&d, 0);
    if (!bar) { kprintf("[HDA] BAR0 is not a memory BAR\n"); return false; }
    PciEnableDevice(&d);
    g.mmio = (volatile UINT8 *)(uintptr_t)(PHYSMAP_BASE + bar);

    if (!reset_controller()) { kprintf("[HDA] Controller did not leave reset\n"); return false; }
    wr32(INTCTL, 0);                                /* polled */
    wr32(DPLBASE, 0);
    setup_rings();

    UINT16 codecs = rd16(STATESTS);
    wr16(STATESTS, codecs);
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1u << cad)) setup_codec(cad);
    if (!g.outputs) { kprintf("[HDA] No codec output to play through\n"); return false; }

    UINT16 gcap = rd16(GCAP);
    if (!start_stream(gcap)) { kprintf("[HDA] No output stream\n"); return false; }

    const char *chip = d.vendor == 0x8086 && d.device == 0x2668 ? "Intel 82801FB (ICH6)" :
                       d.vendor == 0x8086 && d.device == 0x293E ? "Intel 82801I (ICH9)" : 0;
    if (chip) ksnprintf(g.name, sizeof(g.name), "High Definition Audio (%s)", chip);
    else ksnprintf(g.name, sizeof(g.name), "High Definition Audio (%04x:%04x)", d.vendor, d.device);
    g.present = true;
    PciClaim(&d, "HD Audio");
    kprintf("[HDA] %s at %02x:%02x.%x, %d output stream%s, playing 48 kHz 16-bit stereo\n",
            g.name, d.bus, d.dev, d.func, (gcap >> 12) & 0xF, ((gcap >> 12) & 0xF) == 1 ? "" : "s");
    return true;
}

/* After S3 the controller and codecs are back at their reset state: set
 * up the command rings, the codec paths and the stream again.  The ring
 * keeps its memory; the mixer sees the position restart as a wrap. */
void HdaResume(void)
{
    if (!g.present) return;
    if (!reset_controller()) { kprintf("[HDA] Controller did not leave reset after sleep\n"); return; }
    wr32(INTCTL, 0);
    wr32(DPLBASE, 0);
    setup_rings();
    UINT16 codecs = rd16(STATESTS);
    wr16(STATESTS, codecs);
    g.outputs = 0;
    g.inputs = 0;
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1u << cad)) setup_codec(cad);
    program_stream();
}

const char *HdaName(void) { return g.present ? g.name : "No audio device"; }

INT16 *HdaRing(UINT32 *size)
{
    if (size) *size = RING_BYTES;
    return g.ring;
}

UINT32 HdaPosition(void)
{
    UINT32 p = rd32(g.sd + SD_LPIB);
    return p < RING_BYTES ? p & ~3u : 0;
}

bool HdaCanRecord(void) { return g.present && g.ibdl && g.inputs; }
const char *HdaInputName(void) { return HdaCanRecord() ? g.iname : ""; }

INT16 *HdaCaptureRing(UINT32 *size)
{
    if (size) *size = RING_BYTES;
    return g.iring;
}

/* Start or stop the input stream (its ring restarts at 0) */
void HdaCapture(bool run)
{
    if (!HdaCanRecord() || g.irunning == run) return;
    g.irunning = run;
    program_sd(g.isd, g.ibdl, IN_STREAM_TAG, run);
}

/* How far the hardware has written into the capture ring (bytes) */
UINT32 HdaCapturePosition(void)
{
    if (!HdaCanRecord()) return 0;
    UINT32 p = rd32(g.isd + SD_LPIB);
    return p < RING_BYTES ? p & ~3u : 0;
}
