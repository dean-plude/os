/*
 * hda.c — Intel High Definition Audio controller and codec setup (output)
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
#define WT_MIXER    0x2
#define WT_SELECTOR 0x3
#define WT_PIN      0x4

#define WCAP_IN_AMP     (1u << 1)
#define WCAP_OUT_AMP    (1u << 2)
#define WCAP_CONN_LIST  (1u << 8)
#define WCAP_POWER      (1u << 10)

#define PINCAP_OUT      (1u << 4)
#define PINCAP_HP       (1u << 3)
#define PINCAP_EAPD     (1u << 16)

#define STREAM_TAG  1
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
        kprintf("[HDA] Codec %d (%04x:%04x): %d output%s\n", cad, vendor >> 16, vendor & 0xFFFF,
                routed, routed == 1 ? "" : "s");
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
    UINT8 *page = kernel_alloc_pages(1);            /* CORB (1 KiB) + RIRB (2 KiB) */
    memset(page, 0, PAGE_SIZE);
    g.corb = (volatile UINT32 *)page;
    g.rirb = (volatile UINT64 *)(page + 2048);

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

static bool start_stream(UINT16 gcap)
{
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) return false;
    g.sd = 0x80 + (UINT32)iss * 0x20;               /* the first output stream */

    g.bdl  = kernel_alloc_pages(1);
    g.ring = kernel_alloc_pages(RING_ENTRIES);
    if (!g.bdl || !g.ring) return false;
    memset(g.bdl, 0, PAGE_SIZE);
    memset(g.ring, 0, RING_BYTES);
    for (int i = 0; i < RING_ENTRIES; i++) {
        g.bdl[i].addr = phys((UINT8 *)g.ring + i * PAGE_SIZE);
        g.bdl[i].len = PAGE_SIZE;
        g.bdl[i].flags = 0;
    }

    wr32(g.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (rd32(g.sd + SD_CTL) & SD_CTL_RUN); i++) pause_cpu();
    wr32(g.sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 1000 && !(rd32(g.sd + SD_CTL) & SD_CTL_SRST); i++) pause_cpu();
    wr32(g.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (rd32(g.sd + SD_CTL) & SD_CTL_SRST); i++) pause_cpu();
    wr8(g.sd + SD_STS, 0x1C);                       /* clear status */

    wr32(g.sd + SD_BDPL, (UINT32)phys(g.bdl));
    wr32(g.sd + SD_BDPU, (UINT32)(phys(g.bdl) >> 32));
    wr32(g.sd + SD_CBL, RING_BYTES);
    wr16(g.sd + SD_LVI, RING_ENTRIES - 1);
    wr16(g.sd + SD_FMT, FMT_48K_16_STEREO);
    wr32(g.sd + SD_CTL, ((UINT32)STREAM_TAG << 20));
    wr32(g.sd + SD_CTL, ((UINT32)STREAM_TAG << 20) | SD_CTL_RUN);
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
    kprintf("[HDA] %s at %02x:%02x.%x, %d output stream%s, playing 48 kHz 16-bit stereo\n",
            g.name, d.bus, d.dev, d.func, (gcap >> 12) & 0xF, ((gcap >> 12) & 0xF) == 1 ? "" : "s");
    return true;
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
