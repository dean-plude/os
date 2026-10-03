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
 *
 * Laptops (Phase 21.4, the ThinkPad T14 Gen 4): Intel's controllers from
 * Skylake on sit next to an audio DSP, and with the DSP switched on in
 * the firmware they report PCI class 04.01 ("audio device") instead of
 * 04.03, with the same HD Audio registers; those are taken by device ID,
 * or by their HD Audio version registers when the ID is not in the list
 * (an AC'97 controller, also class 04.01, has I/O BARs and is left
 * alone).  Before their reset the clock gating of the link is turned off
 * (CGCTL.MISCBDCGE, as Linux and the Intel datasheets do), and traffic
 * class 0 and snooped DMA are chosen.  Their digital microphones hang off
 * the DSP, not the codec, and stay silent here.
 *
 * Speakers and headphones: a laptop codec has a speaker pin and a
 * headphone jack that can tell whether something is plugged in (pin
 * sense).  HdaPollJacks() reads the jacks twice a second and turns the
 * speaker pins off while headphones are in, on again when they come out.
 * Realtek's ALC256 family (ALC256, ALC257 as in the T14, ALC236) gets the
 * one vendor setting Linux's driver makes for it at start: processing
 * coefficient 0x36 = 0x5757, which takes pin 0x1A off the PC-beep
 * loopback that otherwise reaches every output.
 *
 * HdaSelfCheck() runs the codec setup and the jack handling against a
 * modelled ALC257 (QEMU's codecs have no pin sense and no Realtek
 * registers) and the controller matching against the T14's IDs.
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
#define VERB_GET_SUBSYSTEM  0xF20
#define VERB_EXEC_SENSE     0x709
#define VERB_GET_PIN_SENSE  0xF09
#define VERB4_SET_FORMAT    0x2
#define VERB4_SET_AMP       0x3
#define VERB4_SET_COEF      0x4         /* Realtek: processing coefficient (on NID 0x20) */
#define VERB4_SET_COEF_IDX  0x5

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

#define PINCAP_TRIGGER  (1u << 1)        /* pin sense must be triggered first */
#define PINCAP_PRESENCE (1u << 2)        /* presence detect */
#define PINCAP_OUT      (1u << 4)
#define PINCAP_IN       (1u << 5)
#define PINCAP_HP       (1u << 3)
#define PINCAP_EAPD     (1u << 16)

#define STREAM_TAG  1
#define IN_STREAM_TAG 2
#define FMT_48K_16_STEREO 0x0011        /* base 48 kHz, x1 /1, 16 bits, 2 channels */

/* PCI configuration registers of Intel's controllers */
#define INTEL_TCSEL      0x44           /* traffic class select (bits 2:0) */
#define INTEL_CGCTL      0x48           /* clock gating control (Skylake on) */
#define CGCTL_MISCBDCGE  (1u << 6)      /*   miscellaneous backbone dynamic clock gating */
#define INTEL_DEVC       0x78           /* device control */
#define DEVC_NOSNOOP     (1u << 11)

#define DEV_SPEAKER 0x1                 /* pin configuration default: device */
#define DEV_HP      0x2

#define MAX_JACKS   8

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

/* An output pin the jack handling looks after: a speaker (muted while
 * headphones are in) or a headphone jack that can sense a plug */
typedef struct {
    UINT8 cad, nid, dev;
    bool  trigger;                      /* sense needs an Execute first */
} Jack;

typedef struct {
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
    /* speakers and headphone jacks */
    Jack            jacks[MAX_JACKS];
    int             njacks;
    int             hp_in;              /* headphones plugged in (-1: not read yet) */
    /* HdaSelfCheck: commands go to a modelled codec instead */
    bool          (*model)(UINT32 verb, UINT32 *resp);
} HdaState;

static HdaState g;

static int g_busy;                      /* codec commands in progress (setup, jacks, the check) */

static void lock_codecs(void)   { while (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE)) pause_cpu(); }
static void unlock_codecs(void) { __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); }

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
    if (g.model) return g.model(verb, resp);
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

/* Realtek's ALC256 family: coefficient 0x36 = 0x5757 (pin 0x1A is not
 * PC beep and loops back into no output), as Linux's alc256_init() */
static void realtek_init(int cad, UINT32 vendor)
{
    if (vendor != 0x10EC0256 && vendor != 0x10EC0257 && vendor != 0x10EC0236) return;
    verb4(cad, 0x20, VERB4_SET_COEF_IDX, 0x36);
    verb4(cad, 0x20, VERB4_SET_COEF, 0x5757);
}

static void add_jack(int cad, int nid, UINT32 dev)
{
    Widget *w = &g.w[nid];
    if (g.njacks == MAX_JACKS) return;
    if (dev == DEV_HP) {                /* a jack that senses a plug */
        if ((w->config >> 30) != 0 || !(w->pincaps & PINCAP_PRESENCE) || (w->config & (1u << 8))) return;
    } else if (dev != DEV_SPEAKER) {
        return;
    }
    g.jacks[g.njacks++] = (Jack){ (UINT8)cad, (UINT8)nid, (UINT8)dev, (w->pincaps & PINCAP_TRIGGER) != 0 };
}

static void setup_codec(int cad)
{
    UINT32 vendor = param(cad, 0, PAR_VENDOR);
    UINT32 nodes = param(cad, 0, PAR_NODES);
    int start = (int)((nodes >> 16) & 0xFF), count = (int)(nodes & 0xFF);
    for (int fg = start; fg < start + count; fg++) {
        if ((param(cad, fg, PAR_FG_TYPE) & 0xFF) != 1) continue;     /* audio function group */
        verb12(cad, fg, VERB_SET_POWER, 0);
        UINT32 subsys = verb12(cad, fg, VERB_GET_SUBSYSTEM, 0);
        realtek_init(cad, vendor);
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
            add_jack(cad, nid, dev);
            routed++;
        }
        int had = g.inputs;
        setup_input(cad, ws, wn);
        int spk = 0, hp = 0;
        for (int j = 0; j < g.njacks; j++)
            if (g.jacks[j].cad == cad) { if (g.jacks[j].dev == DEV_HP) hp++; else spk++; }
        kprintf("[HDA] Codec %d (%04x:%04x, subsystem %04x:%04x): %d output%s%s%s%s\n", cad, vendor >> 16,
                vendor & 0xFFFF, subsys >> 16, subsys & 0xFFFF, routed, routed == 1 ? "" : "s",
                spk && hp ? ", speakers muted while headphones are in" : "",
                g.inputs > had ? ", recording from " : "", g.inputs > had ? g.iname : "");
        g.outputs += routed;
    }
}

/* -----------------------------------------------------------------------
 * Jacks: speakers off while headphones are plugged in
 * ----------------------------------------------------------------------- */
static void poll_jacks(bool quiet)
{
    int hp = 0, any = 0;
    for (int j = 0; j < g.njacks; j++) {
        Jack *k = &g.jacks[j];
        if (k->dev != DEV_HP) continue;
        any = 1;
        if (k->trigger) verb12(k->cad, k->nid, VERB_EXEC_SENSE, 0);
        if (verb12(k->cad, k->nid, VERB_GET_PIN_SENSE, 0) & (1u << 31)) hp = 1;
    }
    if (!any || hp == g.hp_in) return;
    int had = g.hp_in;
    g.hp_in = hp;
    int muted = 0;
    for (int j = 0; j < g.njacks; j++) {
        Jack *k = &g.jacks[j];
        if (k->dev != DEV_SPEAKER) continue;
        verb12(k->cad, k->nid, VERB_SET_PIN_CTL, hp ? 0x00 : 0x40);
        muted++;
    }
    if (!quiet && muted && (had >= 0 || hp))
        kprintf("[HDA] Headphones %s: speakers %s\n", hp ? "plugged in" : "unplugged", hp ? "off" : "on");
}

void HdaPollJacks(void)
{
    if (!g.present || !g.njacks) return;
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE)) return;    /* (setup or the check has the codecs) */
    poll_jacks(false);
    unlock_codecs();
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

/* Intel's controllers by device ID; "dsp": Skylake and later, which have
 * the audio DSP beside them (class 04.01 when it is on) and clock gating
 * to turn off around the reset.  IDs from FreeBSD's hdac (BSD licence). */
static const struct { UINT16 id; bool dsp; const char *name; } g_intel[] = {
    { 0x2668, false, "Intel 82801FB (ICH6)" },   { 0x293E, false, "Intel 82801I (ICH9)" },
    { 0x9D70, true,  "Intel Sunrise Point-LP" }, { 0x9D71, true,  "Intel Kaby Lake-LP" },
    { 0xA170, true,  "Intel Sunrise Point" },    { 0xA348, true,  "Intel Coffee Lake" },
    { 0x9DC8, true,  "Intel Cannon Lake" },      { 0x02C8, true,  "Intel Comet Lake-LP" },
    { 0x06C8, true,  "Intel Comet Lake-H" },     { 0xA3F0, true,  "Intel Comet Lake-S" },
    { 0x34C8, true,  "Intel Ice Lake" },         { 0xA0C8, true,  "Intel Tiger Lake" },
    { 0x43C8, true,  "Intel Tiger Lake-H" },     { 0x4DC8, true,  "Intel Jasper Lake" },
    { 0x7AD0, true,  "Intel Alder Lake" },       { 0x51C8, true,  "Intel Alder Lake-P" },
    { 0x51C9, true,  "Intel Alder Lake-PS" },    { 0x51CC, true,  "Intel Alder Lake-M" },
    { 0x51CD, true,  "Intel Alder Lake-P" },     { 0x54C8, true,  "Intel Alder Lake-N" },
    { 0x51CA, true,  "Intel Raptor Lake-P" },    { 0x51CB, true,  "Intel Raptor Lake-P" },
    { 0x51CE, true,  "Intel Raptor Lake-P" },    { 0x51CF, true,  "Intel Raptor Lake-P" },
    { 0x7A50, true,  "Intel Raptor Lake-S" },    { 0x7E28, true,  "Intel Meteor Lake-P" },
};

static int intel_index(UINT16 vendor, UINT16 device)
{
    if (vendor != 0x8086) return -1;
    for (unsigned i = 0; i < sizeof(g_intel) / sizeof(g_intel[0]); i++)
        if (g_intel[i].id == device) return (int)i;
    return -1;
}

int HdaPciMatch(UINT16 vendor, UINT16 device, UINT8 cls, UINT8 sub)
{
    if (cls != 0x04) return HDA_MATCH_NONE;
    if (sub == 0x03) return HDA_MATCH_CLASS;
    if (sub != 0x01 || vendor != 0x8086) return HDA_MATCH_NONE;
    return intel_index(vendor, device) >= 0 ? HDA_MATCH_DSP : HDA_MATCH_PROBE;
}

/* A class 04.01 function that HdaPciMatch() could not decide: HD Audio if
 * BAR0 is memory and the version registers read 1.0 with an output stream */
static bool looks_like_hda(const PciDevice *d)
{
    if (PciRead32(d->bus, d->dev, d->func, 0x10) & 1) return false;         /* an I/O BAR: AC'97 */
    volatile UINT8 *m = PciMapBar(d, 0);
    if (!m) return false;
    PciEnableDevice(d);
    UINT16 gcap = *(volatile UINT16 *)(m + GCAP);
    return m[0x03] == 1 && m[0x02] == 0 && ((gcap >> 12) & 0xF) != 0;      /* VMAJ, VMIN, OSS */
}

static bool find_controller(PciDevice *out, int *how)
{
    PciDevice d;
    for (int i = 0; PciAt(i, &d, NULL); i++) {
        int m = HdaPciMatch(d.vendor, d.device, d.class_code, d.subclass);
        if (m == HDA_MATCH_NONE || (m == HDA_MATCH_PROBE && !looks_like_hda(&d))) continue;
        *out = d;
        *how = m;
        return true;
    }
    return false;
}

/* Intel: TC0, snooped DMA; clock gating off around the reset (Skylake on) */
static void intel_quirks(const PciDevice *d, bool dsp, bool before_reset)
{
    if (d->vendor != 0x8086 || d->device == 0x2668 || d->device == 0x293E) return;   /* (QEMU's ICH6/ICH9) */
    UINT8 b = d->bus, v = d->dev, f = d->func;
    if (before_reset) {
        PciWrite32(b, v, f, INTEL_TCSEL, PciRead32(b, v, f, INTEL_TCSEL) & ~7u);
        PciWrite32(b, v, f, INTEL_DEVC, PciRead32(b, v, f, INTEL_DEVC) & ~DEVC_NOSNOOP);
    }
    if (!dsp) return;
    UINT32 cg = PciRead32(b, v, f, INTEL_CGCTL);
    PciWrite32(b, v, f, INTEL_CGCTL, before_reset ? cg & ~CGCTL_MISCBDCGE : cg | CGCTL_MISCBDCGE);
}

static PciDevice g_dev;
static bool      g_dsp;                 /* Skylake or later (or class 04.01) */

/* Codec addresses that answered: wait up to 100 ms for them after the reset */
static UINT16 codec_mask(void)
{
    UINT16 codecs = 0;
    for (int i = 0; i < 100 && !(codecs = rd16(STATESTS)); i++) delay_ms(1);
    wr16(STATESTS, codecs);
    return codecs;
}

bool HdaInit(void)
{
    PciDevice d;
    int how = HDA_MATCH_NONE;
    if (!find_controller(&d, &how)) {
        kprintf("[HDA] No HD Audio controller\n");
        return false;
    }
    UINT64 bar = PciBarAddress(&d, 0);
    if (!bar) { kprintf("[HDA] BAR0 is not a memory BAR\n"); return false; }
    PciEnableDevice(&d);
    g.mmio = (volatile UINT8 *)PciMapBar(&d, 0);
    if (!g.mmio) { kprintf("[HDA] BAR0 can't be mapped\n"); return false; }
    int ix = intel_index(d.vendor, d.device);
    g_dev = d;
    g_dsp = how == HDA_MATCH_DSP || how == HDA_MATCH_PROBE || (ix >= 0 && g_intel[ix].dsp);
    g.hp_in = -1;

    lock_codecs();
    intel_quirks(&d, g_dsp, true);
    bool up = reset_controller();
    intel_quirks(&d, g_dsp, false);
    if (!up) { unlock_codecs(); kprintf("[HDA] Controller did not leave reset\n"); return false; }
    wr32(INTCTL, 0);                                /* polled */
    wr32(DPLBASE, 0);
    setup_rings();

    UINT16 codecs = codec_mask();
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1u << cad)) setup_codec(cad);
    poll_jacks(true);
    unlock_codecs();
    if (!g.outputs) { kprintf("[HDA] No codec output to play through\n"); return false; }

    UINT16 gcap = rd16(GCAP);
    if (!start_stream(gcap)) { kprintf("[HDA] No output stream\n"); return false; }

    if (ix >= 0) ksnprintf(g.name, sizeof(g.name), "High Definition Audio (%s)", g_intel[ix].name);
    else ksnprintf(g.name, sizeof(g.name), "High Definition Audio (%04x:%04x)", d.vendor, d.device);
    g.present = true;
    PciClaim(&d, "HD Audio");
    kprintf("[HDA] %s at %02x:%02x.%x%s, %d output stream%s, playing 48 kHz 16-bit stereo%s\n",
            g.name, d.bus, d.dev, d.func, d.subclass == 0x01 ? " (class 04.01: the audio DSP is on)" : "",
            (gcap >> 12) & 0xF, ((gcap >> 12) & 0xF) == 1 ? "" : "s",
            g.hp_in > 0 ? ", headphones plugged in" : "");
    return true;
}

/* After S3 the controller and codecs are back at their reset state: set
 * up the command rings, the codec paths and the stream again.  The ring
 * keeps its memory; the mixer sees the position restart as a wrap. */
void HdaResume(void)
{
    if (!g.present) return;
    lock_codecs();
    intel_quirks(&g_dev, g_dsp, true);
    bool up = reset_controller();
    intel_quirks(&g_dev, g_dsp, false);
    if (!up) { unlock_codecs(); kprintf("[HDA] Controller did not leave reset after sleep\n"); return; }
    wr32(INTCTL, 0);
    wr32(DPLBASE, 0);
    setup_rings();
    UINT16 codecs = codec_mask();
    g.outputs = 0;
    g.inputs = 0;
    g.njacks = 0;
    g.hp_in = -1;
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1u << cad)) setup_codec(cad);
    poll_jacks(true);
    unlock_codecs();
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

/* -----------------------------------------------------------------------
 * HdaSelfCheck: a modelled ALC257
 *
 * The widget graph of a Realtek ALC257 as a ThinkPad sets it up: DACs 0x02
 * and 0x03, ADCs 0x08 and 0x09 behind input mixers 0x23 and 0x22, the
 * internal speaker on pin 0x14 (EAPD), the headphone jack on 0x21 and the
 * headset microphone on 0x19 (both sensing a plug), the digital
 * microphones' pins 0x12/0x13 unused (they are on the DSP), the PC-beep
 * pin 0x1D, and the vendor widget 0x20 with the processing coefficients.
 * It answers the verbs the driver sends and remembers what it was told.
 * ----------------------------------------------------------------------- */
typedef struct { UINT8 nid; UINT32 caps, pincaps, config, outamp, inamp; UINT8 conn[8]; } ModelWidget;

static const ModelWidget g_alc257[] = {
    { 0x02, 0x00041D, 0, 0, 0x00025757, 0, { 0 } },
    { 0x03, 0x00041D, 0, 0, 0x00025757, 0, { 0 } },
    { 0x08, 0x10051B, 0, 0, 0, 0x80023F17, { 0x23 } },
    { 0x09, 0x10051B, 0, 0, 0, 0x80023F17, { 0x22 } },
    { 0x0B, 0x20010B, 0, 0, 0, 0x80051F17, { 0x18, 0x19, 0x1A, 0x1B, 0x1D } },
    { 0x12, 0x40000B, 0x00000020, 0x40000000, 0, 0, { 0 } },
    { 0x13, 0x40000B, 0x00000020, 0x40000000, 0, 0, { 0 } },
    { 0x14, 0x40058D, 0x00010010, 0x90170110, 0x80000000, 0, { 0x02, 0x03 } },
    { 0x18, 0x40048B, 0x00003724, 0x411111F0, 0, 0x00270300, { 0 } },
    { 0x19, 0x40048B, 0x00003724, 0x04A11030, 0, 0x00270300, { 0 } },
    { 0x1A, 0x40048B, 0x00003724, 0x411111F0, 0, 0x00270300, { 0 } },
    { 0x1B, 0x40058F, 0x0001373C, 0x411111F0, 0x80000000, 0x00270300, { 0x02, 0x03 } },
    { 0x1D, 0x400400, 0x00000020, 0x40661B45, 0, 0, { 0 } },
    { 0x1E, 0x400781, 0x00000014, 0x411111F0, 0, 0, { 0x06 } },
    { 0x20, 0xF00040, 0, 0, 0, 0, { 0 } },
    { 0x21, 0x40058D, 0x0001001C, 0x04211020, 0x80000000, 0, { 0x02, 0x03 } },
    { 0x22, 0x20010B, 0, 0, 0, 0x80000000, { 0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x0B } },
    { 0x23, 0x20010B, 0, 0, 0, 0x80000000, { 0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x12, 0x13 } },
};

static struct {
    bool   hp, mic;                     /* headphones / headset plugged in */
    UINT8  pinctl[MAX_NODES], eapd[MAX_NODES], stream[MAX_NODES];
    UINT16 coef_idx;
    UINT16 coef[0x80];
} g_m;

static const ModelWidget *model_widget(int nid)
{
    for (unsigned i = 0; i < sizeof(g_alc257) / sizeof(g_alc257[0]); i++)
        if (g_alc257[i].nid == nid) return &g_alc257[i];
    return NULL;
}

static bool model_cmd(UINT32 v, UINT32 *resp)
{
    int cad = (int)(v >> 28), nid = (int)((v >> 20) & 0x7F);
    UINT32 id4 = (v >> 16) & 0xF, verb = (v >> 8) & 0xFFF, pl = v & 0xFF, r = 0;
    if (cad != 0) return false;                                      /* (no such codec: no answer) */
    const ModelWidget *w = model_widget(nid);
    if (id4 == 0x2 || id4 == 0x3) {                                  /* format, amp: accepted */
    } else if (id4 == 0x5) {
        if (nid == 0x20) g_m.coef_idx = (UINT16)(v & 0xFFFF);
    } else if (id4 == 0x4) {
        if (nid == 0x20 && g_m.coef_idx < 0x80) g_m.coef[g_m.coef_idx] = (UINT16)(v & 0xFFFF);
    } else if (verb == VERB_GET_PARAM) {
        switch (pl) {
        case PAR_VENDOR: r = nid == 0 ? 0x10EC0257 : 0; break;
        case PAR_NODES:  r = nid == 0 ? 0x00010001 : nid == 1 ? 0x00020022 : 0; break;
        case PAR_FG_TYPE: r = nid == 1 ? 0x00000101 : 0; break;
        case PAR_WCAPS:  r = w ? w->caps : 0; break;
        case PAR_PINCAPS: r = w ? w->pincaps : 0; break;
        case PAR_OUT_AMP: r = w ? w->outamp : 0; break;
        case PAR_IN_AMP: r = w ? w->inamp : 0; break;
        case PAR_CONN_LEN: { int n = 0; while (w && n < 8 && w->conn[n]) n++; r = (UINT32)n; break; }
        }
    } else if (verb == VERB_GET_CONN_LIST) {
        for (int k = 0; k < 4 && w && pl + k < 8; k++) r |= (UINT32)w->conn[pl + k] << (8 * k);
    } else if (verb == VERB_GET_CONFIG) {
        r = w ? w->config : 0;
    } else if (verb == VERB_GET_SUBSYSTEM) {
        r = nid == 1 ? 0x17AA0000 : 0;
    } else if (verb == VERB_GET_PIN_SENSE) {
        r = (nid == 0x21 && g_m.hp) || (nid == 0x19 && g_m.mic) ? 0x80000000u : 0;
    } else if (verb == VERB_SET_PIN_CTL) {
        g_m.pinctl[nid] = (UINT8)pl;
    } else if (verb == VERB_SET_EAPD) {
        g_m.eapd[nid] = (UINT8)pl;
    } else if (verb == VERB_SET_STREAM) {
        g_m.stream[nid] = (UINT8)pl;
    }
    if (resp) *resp = r;
    return true;
}

int HdaSelfCheck(void (*say)(void *ctx, const char *line), void *ctx)
{
    int failed = 0;
    char line[160];
#define CHECK(ok, ...) do { bool ok_ = (ok); ksnprintf(line, sizeof(line), __VA_ARGS__); \
                            char out_[176]; ksnprintf(out_, sizeof(out_), "%s %s", ok_ ? "ok  " : "FAIL", line); \
                            say(ctx, out_); if (!ok_) failed++; } while (0)

    /* Controllers: the T14 Gen 4's (Raptor Lake-P) with the DSP on and
     * off, QEMU's, and class 04.01 functions that are not HD Audio */
    CHECK(HdaPciMatch(0x8086, 0x51CA, 0x04, 0x01) == HDA_MATCH_DSP, "Raptor Lake-P 8086:51ca, class 04.01 (DSP on): HD Audio by ID");
    CHECK(HdaPciMatch(0x8086, 0x51CA, 0x04, 0x03) == HDA_MATCH_CLASS, "Raptor Lake-P 8086:51ca, class 04.03 (DSP off): HD Audio by class");
    CHECK(HdaPciMatch(0x8086, 0x2668, 0x04, 0x03) == HDA_MATCH_CLASS, "QEMU ICH6 8086:2668, class 04.03: HD Audio by class");
    CHECK(HdaPciMatch(0x8086, 0x2415, 0x04, 0x01) == HDA_MATCH_PROBE, "AC'97 8086:2415, class 04.01: only if its registers are HD Audio's");
    CHECK(HdaPciMatch(0x1022, 0x15E3, 0x04, 0x01) == HDA_MATCH_NONE, "1022:15e3, class 04.01, not Intel: not taken");
    CHECK(HdaPciMatch(0x8086, 0x51C8, 0x04, 0x80) == HDA_MATCH_NONE, "8086:51c8, class 04.80: not taken");

    /* The modelled ALC257 through the real setup */
    static HdaState saved;
    lock_codecs();
    saved = g;
    memset(&g_m, 0, sizeof(g_m));
    g.model = model_cmd;
    g.outputs = 0;
    g.inputs = 0;
    g.njacks = 0;
    g.hp_in = -1;
    g.iname[0] = '\0';
    setup_codec(0);
    poll_jacks(true);
    int outputs = g.outputs, njacks = g.njacks;
    bool speaker_on = g_m.pinctl[0x14] == 0x40 && g_m.eapd[0x14] == 0x02;
    bool hp_pin = g_m.pinctl[0x21] == 0xC0;
    bool dac = g_m.stream[0x02] == STREAM_TAG << 4;
    bool unused_off = g_m.pinctl[0x12] == 0 && g_m.pinctl[0x1D] == 0 && g_m.pinctl[0x18] == 0;
    bool mic = g.inputs == 1 && strcmp(g.iname, "Microphone") == 0 && g_m.pinctl[0x19] == 0x20;
    UINT16 coef36 = g_m.coef[0x36];
    g_m.hp = true;                                    /* headphones in */
    poll_jacks(true);
    bool muted = g_m.pinctl[0x14] == 0x00 && g_m.pinctl[0x21] == 0xC0;
    g_m.hp = false;                                   /* and out again */
    poll_jacks(true);
    bool back = g_m.pinctl[0x14] == 0x40;
    g = saved;
    unlock_codecs();

    CHECK(outputs == 2 && njacks == 2, "ALC257: speaker 0x14 and headphone jack 0x21 routed (%d outputs, %d watched)", outputs, njacks);
    CHECK(speaker_on && dac, "ALC257: speaker pin out with EAPD on, DAC 0x02 on the output stream");
    CHECK(hp_pin, "ALC257: headphone pin out with its amplifier");
    CHECK(unused_off, "ALC257: unused pins (DMIC 0x12, PC beep 0x1D, 0x18) left off");
    CHECK(mic, "ALC257: records from the headset microphone 0x19");
    CHECK(coef36 == 0x5757, "ALC257: coefficient 0x36 = 0x%04x (want 0x5757: no PC-beep loopback)", coef36);
    CHECK(muted, "ALC257: headphones plugged in: speaker pin off, headphones on");
    CHECK(back, "ALC257: headphones unplugged: speaker pin on again");
#undef CHECK
    return failed;
}
