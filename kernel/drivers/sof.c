/*
 * sof.c — Intel's audio DSP running Sound Open Firmware (the ThinkPad T14
 * Gen 4's digital microphones, after Phase 21.4)
 *
 * Intel's HD Audio controllers from Skylake on carry an audio DSP in the
 * same PCI function.  A laptop's digital microphones (DMICs) are wired to
 * that DSP, not to the codec, so the codec path hda.c drives cannot record
 * from them: the DSP has to run firmware that clocks the microphones and
 * hands their samples to the host over an HD Audio stream.
 *
 * This file is the first part of that, for the cAVS 2.5 generation (Tiger
 * Lake, Alder Lake, Raptor Lake: the T14's 8086:51ca):
 *
 *  - the ACPI NHLT table ("Non HD Audio Link Table") says whether there are
 *    digital microphones, how many, in which formats, and carries the DMIC
 *    configuration blobs the firmware needs to clock them;
 *  - the Sound Open Firmware image (Intel-signed sof-bin, BSD-3-Clause,
 *    fetched at build time by tools/fetch_sof_firmware.py and built into
 *    C:\Windows\Firmware; it is not in the repository) is checked: its
 *    extended manifest, the $CPD partition and the $AM1 module list;
 *  - the DSP boots: core 0 powered and held in reset, the ROM asked to
 *    load firmware from a host DMA stream ("purge"), the core let run, the
 *    image streamed to the ROM over an output stream decoupled from the
 *    link (the code loader), the firmware's FW_READY notification taken
 *    and answered, and one IPC4 request sent (the base firmware's
 *    FW_CONFIG, which returns its version) to show the channel works both
 *    ways.
 *
 *  - the capture pipeline: an IPC4 pipeline holding two copier modules,
 *    the first on the DMIC gateway (configured with the NHLT blob), bound
 *    to the second on a host input DMA gateway, which writes the
 *    microphones' samples into a ring of an HD Audio input stream the
 *    kernel owns (decoupled from the link, so the DSP's DMA fills it).
 *    SofCaptureRing() and SofCapturePosition() give that ring to audio.c.
 *
 * Not yet: the recording device audio.c would offer over that ring, and a
 * DSP boot again after sleep; docs/hardware.md lists those steps.
 *
 * Register and message layouts follow Intel's public documents (the HD
 * Audio specification with its processing-pipe and software-position-in-
 * buffer capabilities), the NHLT layout Microsoft publishes, and the
 * BSD-licensed SOF firmware headers; no GPL code.  QEMU has no such DSP,
 * so SofSelfCheck() runs the whole boot against a modelled one; the real
 * hardware is unverified until it runs on the machine, which is why each
 * step is logged ("[DSP]" lines in the boot log).
 *
 * Polled, as hda.c: no interrupts.
 */

#include "sof.h"
#include "hda.h"
#include "../hal/pci.h"
#include "../hal/acpi.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../arch/x86_64/cpu.h"
#include "../um/userland_files.h"

/* HD Audio controller registers (BAR0) */
#define LLCH        0x14                /* first extended capability */
#define SD_BASE     0x80
#define SD_SIZE     0x20
#define SD_CTL      0x00
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C
#define SD_CTL_SRST (1u << 0)
#define SD_CTL_RUN  (1u << 1)
#define EM2         0x1030              /* Intel vendor-specific: L1 entry */
#define EM2_L1SEN   (1u << 13)
#define CL_FORMAT   0x0040              /* (any format: the ROM takes bytes) */

/* Extended capabilities: header bits 27:16 = ID, 15:0 = next */
#define CAP_PP      0x3                 /* processing pipe */
#define CAP_SPB     0x4                 /* software position in buffer */
#define PPCTL       0x04
#define PPCTL_GPROCEN (1u << 30)
#define SPBFCCTL    0x04
#define SPB_SPIB(i) (0x08 + 0x08 * (UINT32)(i))

/* DSP registers (BAR4) */
#define ADSPCS      0x04                /* cores: reset, stall, power */
#define CS_CRST(c)  (1u << (c))
#define CS_CSTALL(c) (1u << (8 + (c)))
#define CS_SPA(c)   (1u << (16 + (c)))
#define CS_CPA(c)   (1u << (24 + (c)))
#define HIPCTDR     0xC0                /* DSP -> host: message (BUSY), */
#define HIPCTDA     0xC4                /*   host's DONE, */
#define HIPCTDD     0xC8                /*   extension */
#define HIPCIDR     0xD0                /* host -> DSP: message (BUSY), */
#define HIPCIDA     0xD4                /*   DSP's DONE, */
#define HIPCIDD     0xD8                /*   extension */
#define IPC_BUSY    (1u << 31)
#define IPC_DONE    (1u << 31)
#define FW_STATUS   0x80000             /* SRAM window 0: the ROM's/firmware's state */
#define FW_ERROR    0x80004
#define OUTBOX      0x81000             /* window 0 + 4 KiB: DSP -> host data */
#define INBOX       0xA0000             /* window 1: host -> DSP data */
#define BOX_SIZE    0x1000
#define BAR4_SPAN   (INBOX + BOX_SIZE)
#define ST_MASK     0xF
#define ST_ROM_INIT 0x1
#define ST_FW_ENTERED 0x5
#define ROM_PURGE   (0x01000000u | 0x00004000u)   /* ROM control: purge, load from the stream (tag-1 << 9) */

/* IPC4 messages: bits 28:24 type, 29 reply, 30 module message */
#define IPC4_TYPE(t)    ((UINT32)(t) << 24)
#define IPC4_TYPE_OF(p) (((p) >> 24) & 0x1F)
#define IPC4_REPLY      (1u << 29)
#define IPC4_MODULE     (1u << 30)
#define GLB_NOTIFICATION 27
#define NOTIFY_FW_READY  8              /* (bits 23:16 of a notification) */
#define MOD_LARGE_CONFIG_GET 3
#define LC_FINAL        (1u << 28)
#define LC_INIT         (1u << 29)
#define FW_PARAM_FW_CONFIG 7
#define FW_CFG_FW_VERSION  0

/* The capture pipeline (SOF's src/include/ipc4/pipeline.h, module.h,
 * gateway.h and src/audio/copier/copier.h, BSD-3-Clause) */
#define GLB_CREATE_PIPELINE    17       /* pri: 10:0 pages, 15:11 priority, 23:16 pipeline */
#define GLB_DELETE_PIPELINE    18       /* pri: 23:16 pipeline */
#define GLB_SET_PIPELINE_STATE 19       /* pri: 15:0 state, 23:16 pipeline */
#define PPL_RESET              2
#define PPL_PAUSED             3
#define PPL_RUNNING            4
#define MOD_INIT_INSTANCE      0        /* pri: 15:0 module, 23:16 instance; ext: 15:0 dwords, 23:16 pipeline, 27:24 core */
#define MOD_BIND               5        /* ext: 15:0 module, 23:16 instance, 26:24 its queue, 29:27 ours */
#define NODE_HOST_INPUT        1        /* gateway node id: 7:0 index, 12:8 class */
#define NODE_DMIC_INPUT        11
#define NODE_ID(cls, i)        ((UINT32)(cls) << 8 | (UINT32)(i))
#define FMT_LSB_INTEGER        1        /* (ipc4_sample_type) */
#define FMT_DWORDS             6        /* ipc4_audio_format */
#define COPIER_CFG_SIZE        (4 * (4 + FMT_DWORDS + FMT_DWORDS + 1 + 3))   /* base, out_fmt, features, gateway */
#define CAP_PIPELINE           0
#define CAP_PAGES              32       /* the host ring: 128 KiB */
#define CAP_DEPTH              16       /* what lands in it: 48 kHz s16, the microphones' channels */

typedef struct __attribute__((packed)) {
    UINT64 addr;
    UINT32 len;
    UINT32 flags;
} BdlEntry;

/* What NHLT says about the digital microphones */
typedef struct {
    int          endpoints;             /* all endpoints */
    int          mics;                  /* microphones (0: no DMIC endpoint) */
    int          formats;
    UINT32       rate;                  /* the first format */
    UINT16       bits, valid, channels;
    UINT8        vbus;                  /* the endpoint's virtual bus: the DMIC gateway's index */
    UINT32       blob_size;             /* its DMIC configuration */
    const UINT8 *blob;
} NhltDmic;

/* What the firmware file holds */
typedef struct {
    const UINT8 *image;                 /* what is streamed to the ROM ($CPD on) */
    UINT32       size;
    UINT16       ver[4];
    int          modules;
    int          copier;                /* the COPIER module's ID (-1: none) */
} SofImage;

static struct {
    volatile UINT8 *hb, *db;            /* BAR0, BAR4 (or the model's memory) */
    UINT32          pp, spb;            /* capability offsets in BAR0 (0: none) */
    int             iss, oss;
    char            status[128];
} d;

/* The modelled DSP (SofSelfCheck) */
static struct {
    bool         on;
    const UINT8 *expect;                /* the image the ROM must receive */
    UINT32       expect_size;
    int          phase;                 /* 0 ROM, 1 waiting for the image, 2 FW_READY sent, 3 running */
    UINT32       tag;                   /* the stream the purge named */
    bool         purge_pending;
    bool         got_image, bad_image, l1sen_was_off;
    int          fw_configs;
    /* the capture pipeline */
    int          copier;                /* the copier's module ID */
    const UINT8 *blob;                  /* the DMIC configuration it must get */
    UINT32       blob_size;
    int          pipe;                  /* 0 none, 1 created, then PPL_* */
    bool         dmic_ok, host_ok, bound;
    int          host_index, deleted;
    int          requests, refuse;      /* pipeline requests; refuse the n-th (0: none) */
    const char  *wrong;                 /* the first thing the model refused */
    UINT32       frames;                /* frames its DMA wrote into the host ring */
} g_m;

static void model_tick(void);
static void model_w4(UINT32 off, UINT32 v);
static UINT32 model_pipeline(UINT32 m, UINT32 x);
static void model_capture(void);

static inline UINT32 r0(UINT32 o)            { return *(volatile UINT32 *)(d.hb + o); }
static inline void   w0(UINT32 o, UINT32 v)  { *(volatile UINT32 *)(d.hb + o) = v; }
static inline void   w0w(UINT32 o, UINT16 v) { *(volatile UINT16 *)(d.hb + o) = v; }
static inline void   w0b(UINT32 o, UINT8 v)  { *(volatile UINT8 *)(d.hb + o) = v; }
static inline UINT32 r4(UINT32 o)            { return *(volatile UINT32 *)(d.db + o); }
static inline void   w4(UINT32 o, UINT32 v)
{
    if (g_m.on) model_w4(o, v);
    else *(volatile UINT32 *)(d.db + o) = v;
}
static inline UINT64 phys(const volatile void *va) { return (UINT64)(uintptr_t)va - PHYSMAP_BASE; }

/* Wait up to @ms for @cond (the model answers between looks instead) */
#define WAIT(ms, cond) ({                                                            \
    bool ok_ = false;                                                                \
    UINT64 end_ = sched_ticks() + (UINT64)(ms) / 10 + 2;                             \
    for (int n_ = 0;; n_++) {                                                        \
        if (g_m.on) model_tick();                                                    \
        if (cond) { ok_ = true; break; }                                             \
        if (g_m.on ? n_ > 64 : sched_ticks() > end_) break;                          \
        if (!g_m.on) sched_sleep_until(NULL, sched_ticks() + 1);                     \
    }                                                                                \
    ok_; })

static UINT16 get16(const UINT8 *p) { return (UINT16)(p[0] | p[1] << 8); }
static UINT32 get32(const UINT8 *p) { return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 | (UINT32)p[3] << 24; }

/* -----------------------------------------------------------------------
 * NHLT: endpoints of link type PDM (2) recording (direction 1) are the
 * digital microphones.  Endpoint: u32 length, u8 link type, u8 instance,
 * u16 vendor, device, revision, u32 subsystem, u8 device type, direction,
 * virtual bus; then the endpoint's capabilities (u32 size + bytes: virtual
 * slot, config type 1 = microphone array, array type), then u8 format
 * count and per format a 40-byte WAVEFORMATEXTENSIBLE and its
 * configuration (u32 size + bytes).
 * ----------------------------------------------------------------------- */
static int array_mics(const UINT8 *caps, UINT32 n)
{
    if (n < 3 || caps[1] != 1) return 0;            /* not a microphone array */
    switch (caps[2] & 0xF) {
    case 0xA: case 0xB: return 2;                   /* small, big linear 2-mic */
    case 0xC: case 0xD: case 0xE: return 4;         /* linear and L-shaped 4-mic */
    case 0xF: return n >= 4 ? caps[3] : 0;          /* vendor defined: the count follows */
    }
    return 0;
}

static bool nhlt_parse(const UINT8 *t, UINT32 len, NhltDmic *o)
{
    memset(o, 0, sizeof(*o));
    if (len < 37 || memcmp(t, "NHLT", 4) != 0 || get32(t + 4) > len) return false;
    len = get32(t + 4);
    int count = t[36];
    UINT32 at = 37;
    for (int e = 0; e < count; e++) {
        if (at + 23 > len) return false;
        const UINT8 *ep = t + at;
        UINT32 elen = get32(ep);
        if (elen < 23 || at + elen > len) return false;
        o->endpoints++;
        if (ep[4] == 2 && ep[17] == 1 && !o->mics) {   /* PDM, capture */
            UINT32 csz = get32(ep + 19), p = 23 + csz;
            if (p + 1 > elen) return false;
            int mics = array_mics(ep + 23, csz);
            int nf = ep[p++];
            for (int f = 0; f < nf; f++) {
                if (p + 44 > elen) return false;
                const UINT8 *wf = ep + p;
                UINT32 bsz = get32(wf + 40);
                if (p + 44 + bsz > elen) return false;
                if (f == 0) {
                    o->channels = get16(wf + 2);
                    o->rate = get32(wf + 4);
                    o->bits = get16(wf + 14);
                    o->valid = get16(wf + 18) ? get16(wf + 18) : o->bits;
                    o->vbus = ep[18];
                    o->blob = wf + 44;
                    o->blob_size = bsz;
                }
                if (!mics && get16(wf + 2) > mics) mics = get16(wf + 2);
                p += 44 + bsz;
            }
            o->formats = nf;
            o->mics = mics ? mics : 1;
        }
        at += elen;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * The firmware file: an extended manifest ("$AE1", u32 its size) first,
 * which the host keeps; the image the ROM takes starts at "$CPD" (the CSE
 * partition directory), and its "$AM1" module manifest lists the modules
 * (116 bytes each, "$AME" + an 8-byte name; a module's ID is its place).
 * ----------------------------------------------------------------------- */
#define AM_ENTRY_SIZE 116

static bool fw_parse(const UINT8 *f, UINT32 len, SofImage *o, char *why, int cap)
{
    memset(o, 0, sizeof(*o));
    o->copier = -1;
    UINT32 skip = 0;
    if (len >= 16 && memcmp(f, "$AE1", 4) == 0) skip = get32(f + 4);
    if (skip >= len || len - skip < 0x40 || memcmp(f + skip, "$CPD", 4) != 0) {
        ksnprintf(why, cap, "not a SOF firmware image (no $CPD partition)");
        return false;
    }
    o->image = f + skip;
    o->size = len - skip;
    UINT32 am = 0;
    for (UINT32 i = 0; i + 0x34 <= o->size && i < 0x10000; i += 4)
        if (memcmp(o->image + i, "$AM1", 4) == 0) { am = i; break; }
    if (!am) { ksnprintf(why, cap, "no $AM1 module manifest"); return false; }
    const UINT8 *h = o->image + am;
    UINT32 hlen = get32(h + 4), n = get32(h + 36);
    for (int k = 0; k < 4; k++) o->ver[k] = get16(h + 28 + 2 * k);
    if (hlen < 0x28 || n > 128 || am + hlen + n * AM_ENTRY_SIZE > o->size) {
        ksnprintf(why, cap, "the module manifest is damaged");
        return false;
    }
    for (UINT32 m = 0; m < n; m++) {
        const UINT8 *e = h + hlen + m * AM_ENTRY_SIZE;
        if (memcmp(e, "$AME", 4) != 0) { ksnprintf(why, cap, "module %u is damaged", m); return false; }
        if (memcmp(e + 4, "COPIER\0", 7) == 0 && o->copier < 0) o->copier = (int)m;
    }
    o->modules = (int)n;
    return true;
}

/* The firmware for this controller: cAVS 2.5 only (Meteor Lake's ACE and
 * the older cAVS take other firmware and loaders) */
static const struct { UINT16 id; const char *plat; } g_plats[] = {
    { 0xA0C8, "tgl" },   { 0x43C8, "tgl-h" },
    { 0x51C8, "adl" },   { 0x51C9, "adl" },   { 0x51CC, "adl" },   { 0x51CD, "adl" },
    { 0x7AD0, "adl-s" }, { 0x54C8, "adl-n" },
    { 0x51CA, "rpl" },   { 0x51CB, "rpl" },   { 0x51CE, "rpl" },   { 0x51CF, "rpl" },
    { 0x7A50, "rpl-s" },
};

static const char *platform(UINT16 device)
{
    for (unsigned i = 0; i < sizeof(g_plats) / sizeof(g_plats[0]); i++)
        if (g_plats[i].id == device) return g_plats[i].plat;
    return NULL;
}

/* \Windows\Firmware\Intel\sof-ipc4\rpl\sof-rpl.ri, as built in */
static const UINT8 *firmware_file(const char *plat, UINT32 *size, char *path, int cap)
{
    ksnprintf(path, cap, "\\Windows\\Firmware\\Intel\\sof-ipc4\\%s\\sof-%s.ri", plat, plat);
    for (int i = 0; i < g_userland_file_count; i++) {
        const UserlandFile *u = &g_userland_files[i];
        if (strcmp(u->path, path) != 0 || u->zsize) continue;    /* (under 1 MiB: stored as is) */
        *size = u->size;
        return u->data;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Streams the DSP reads (decoupled from the link: the processing pipe)
 * ----------------------------------------------------------------------- */
typedef struct {
    int       index;                    /* the stream's number (inputs first) */
    UINT32    sd;
    BdlEntry *bdl;
    UINT8    *buf;
    UINT32    pages;
} DspStream;

static UINT32 find_cap(UINT32 id)
{
    UINT32 off = r0(LLCH) & 0xFFFF;
    for (int n = 0; off && off < 0x4000 && n < 16; n++) {
        UINT32 h = r0(off);
        if (((h >> 16) & 0xFFF) == id) return off;
        off = h & 0xFFFF;
    }
    return 0;
}

/* @spib: the DSP stops at @size (the code loader); without it the stream
 * is a ring the DSP goes round (capture) */
static bool stream_prepare(DspStream *s, int index, UINT32 tag, const UINT8 *data, UINT32 size, UINT16 fmt,
                           bool spib)
{
    memset(s, 0, sizeof(*s));
    s->index = index;
    s->sd = SD_BASE + (UINT32)index * SD_SIZE;
    s->pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!s->pages || s->pages > 256) return false;
    s->bdl = kernel_alloc_pages(1);
    s->buf = kernel_alloc_pages(s->pages);
    if (!s->bdl || !s->buf) return false;
    memset(s->bdl, 0, PAGE_SIZE);
    memset(s->buf, 0, s->pages * PAGE_SIZE);
    if (data) memcpy(s->buf, data, size);
    UINT32 total = (size + 127) & ~127u;            /* (buffer lengths in 128-byte units) */
    for (UINT32 i = 0, left = total; i < s->pages; i++) {
        s->bdl[i].addr = phys(s->buf + i * PAGE_SIZE);
        s->bdl[i].len = left < PAGE_SIZE ? left : PAGE_SIZE;
        left -= s->bdl[i].len;
    }

    w0(s->sd + SD_CTL, 0);
    w0(s->sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 1000 && !(r0(s->sd + SD_CTL) & SD_CTL_SRST); i++) pause_cpu();
    w0(s->sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (r0(s->sd + SD_CTL) & SD_CTL_SRST); i++) pause_cpu();
    w0b(s->sd + SD_STS, 0x1C);
    w0(s->sd + SD_BDPL, (UINT32)phys(s->bdl));
    w0(s->sd + SD_BDPU, (UINT32)(phys(s->bdl) >> 32));
    w0(s->sd + SD_CBL, total);
    w0w(s->sd + SD_LVI, (UINT16)(s->pages - 1));
    w0w(s->sd + SD_FMT, fmt);
    w0(s->sd + SD_CTL, tag << 20);
    w0(d.pp + PPCTL, r0(d.pp + PPCTL) | (1u << index));          /* decoupled: the DSP's DMA */
    if (d.spb && spib) {                                          /* stop at the image's end */
        w0(d.spb + SPBFCCTL, r0(d.spb + SPBFCCTL) | (1u << index));
        w0(d.spb + SPB_SPIB(index), size);
    }
    return true;
}

static void stream_run(DspStream *s, bool run)
{
    UINT32 c = r0(s->sd + SD_CTL);
    w0(s->sd + SD_CTL, run ? c | SD_CTL_RUN : c & ~SD_CTL_RUN);
    if (!run) for (int i = 0; i < 1000 && (r0(s->sd + SD_CTL) & SD_CTL_RUN); i++) pause_cpu();
}

static void stream_free(DspStream *s)
{
    if (!s->bdl && !s->buf) return;
    stream_run(s, false);
    if (d.spb) w0(d.spb + SPBFCCTL, r0(d.spb + SPBFCCTL) & ~(1u << s->index));
    w0(d.pp + PPCTL, r0(d.pp + PPCTL) & ~(1u << s->index));
    if (s->bdl) kernel_free_pages(s->bdl, 1);
    if (s->buf) kernel_free_pages(s->buf, s->pages);
    memset(s, 0, sizeof(*s));
}

/* -----------------------------------------------------------------------
 * IPC
 * ----------------------------------------------------------------------- */

/* A message from the DSP (a reply or a notification), or false */
static bool ipc_recv(UINT32 *pri, UINT32 *ext, int ms)
{
    if (!WAIT(ms, r4(HIPCTDR) & IPC_BUSY)) return false;
    *pri = r4(HIPCTDR) & ~IPC_BUSY;
    *ext = r4(HIPCTDD);
    return true;
}

/* The host is done with the DSP's message: BUSY cleared (write 1), DONE set */
static void ipc_done(void)
{
    w4(HIPCTDR, IPC_BUSY);
    w4(HIPCTDA, r4(HIPCTDA) | IPC_DONE);
}

static void box_read(UINT32 box, void *out, UINT32 n)
{
    for (UINT32 i = 0; i + 4 <= n; i += 4) {
        UINT32 v = r4(box + i);
        memcpy((UINT8 *)out + i, &v, 4);
    }
}

/* Send a request and wait for its reply (notifications on the way are
 * answered and logged).  The reply's data, if any, is in the outbox. */
static bool ipc4(UINT32 pri, UINT32 ext, UINT32 *rpri, UINT32 *rext)
{
    if (r4(HIPCIDR) & IPC_BUSY) return false;
    w4(HIPCIDD, ext);
    w4(HIPCIDR, pri | IPC_BUSY);
    for (int n = 0; n < 8; n++) {
        UINT32 p, e;
        if (!ipc_recv(&p, &e, 500)) return false;
        if (p & IPC4_REPLY) {
            *rpri = p;
            *rext = e;
            if (r4(HIPCIDA) & IPC_DONE) w4(HIPCIDA, IPC_DONE);
            return true;
        }
        if (!g_m.on) kprintf("[DSP] Notification %08x %08x\n", p, e);
        ipc_done();
    }
    return false;
}

/* The base firmware's FW_CONFIG: a list of (u32 type, u32 length, value);
 * the version is type 0 (four u16) */
static bool fw_version(UINT16 ver[4])
{
    UINT32 p, e;
    if (!ipc4(IPC4_MODULE | IPC4_TYPE(MOD_LARGE_CONFIG_GET),
              ((UINT32)FW_PARAM_FW_CONFIG << 20) | LC_FINAL | LC_INIT | BOX_SIZE, &p, &e))
        return false;
    static UINT8 buf[BOX_SIZE];
    UINT32 n = e & 0xFFFFF;
    bool ok = (p & 0xFFFFFF) == 0 && n <= BOX_SIZE;
    if (ok) box_read(OUTBOX, buf, (n + 3) & ~3u);
    ipc_done();
    if (!ok) return false;
    for (UINT32 at = 0; at + 8 <= n;) {
        UINT32 type = get32(buf + at), len = get32(buf + at + 4);
        if (type == FW_CFG_FW_VERSION && len >= 8 && at + 16 <= n) {
            for (int k = 0; k < 4; k++) ver[k] = get16(buf + at + 8 + 2 * k);
            return true;
        }
        at += 8 + ((len + 3) & ~3u);
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Boot
 * ----------------------------------------------------------------------- */
static bool boot(const SofImage *img, UINT16 ver[4], char *why, int cap)
{
    if (d.oss < 1) { ksnprintf(why, cap, "the controller has no output stream for the code loader"); return false; }
    int cl_index = d.iss + d.oss - 1;               /* the last output stream (playback has the first) */
    UINT32 tag = (UINT32)d.oss;                      /* (output stream n carries tag n) */
    UINT32 em2 = r0(EM2), ppctl = r0(d.pp + PPCTL);
    DspStream icc = { 0 }, cl = { 0 };
    bool ok = false;

    w0(d.pp + PPCTL, ppctl | PPCTL_GPROCEN);        /* the processing pipe on */
    w0(EM2, em2 & ~EM2_L1SEN);                      /* no link power saving while loading */
    /* A decoupled input stream held while the DSP boots, so that the
     * platform grants the DSP its full current (ICCMAX) */
    if (d.iss && !stream_prepare(&icc, d.iss - 1, (UINT32)d.iss, NULL, PAGE_SIZE, CL_FORMAT, true)) {
        ksnprintf(why, cap, "out of memory");
        goto out;
    }
    if (!stream_prepare(&cl, cl_index, tag, img->image, img->size, CL_FORMAT, true)) {
        ksnprintf(why, cap, "out of memory for the %u-byte image", img->size);
        goto out;
    }

    /* Core 0: in reset and stalled, then powered */
    w4(ADSPCS, r4(ADSPCS) | CS_CRST(0) | CS_CSTALL(0));
    w4(ADSPCS, r4(ADSPCS) | CS_SPA(0));
    if (!WAIT(50, r4(ADSPCS) & CS_CPA(0))) { ksnprintf(why, cap, "core 0 did not power up (ADSPCS %08x)", r4(ADSPCS)); goto out; }

    /* The ROM: load the firmware from stream @tag; then let core 0 run */
    w4(HIPCIDD, 0);
    w4(HIPCIDR, IPC_BUSY | ROM_PURGE | ((tag - 1) << 9));
    w4(ADSPCS, r4(ADSPCS) & ~(CS_CRST(0) | CS_CSTALL(0)));
    if (!WAIT(500, r4(HIPCIDA) & IPC_DONE)) { ksnprintf(why, cap, "the ROM did not take the load request"); goto out; }
    w4(HIPCIDA, IPC_DONE);
    if (!WAIT(500, (r4(FW_STATUS) & ST_MASK) == ST_ROM_INIT)) {
        ksnprintf(why, cap, "the ROM did not start (status %08x, error %08x)", r4(FW_STATUS), r4(FW_ERROR));
        goto out;
    }
    if (!g_m.on)
        kprintf("[DSP] ROM ready; streaming %u bytes of firmware on stream %d (tag %u)\n", img->size, cl_index, tag);

    /* The image, over the code loader stream */
    stream_run(&cl, true);
    if (!WAIT(3000, (r4(FW_STATUS) & ST_MASK) == ST_FW_ENTERED)) {
        ksnprintf(why, cap, "the firmware did not start (status %08x, error %08x)", r4(FW_STATUS), r4(FW_ERROR));
        goto out;
    }
    stream_free(&cl);
    stream_free(&icc);

    /* FW_READY, then a request to see the firmware answer */
    UINT32 p, e;
    if (!ipc_recv(&p, &e, 2000)) { ksnprintf(why, cap, "no FW_READY from the firmware (status %08x)", r4(FW_STATUS)); goto out; }
    bool ready = !(p & IPC4_MODULE) && IPC4_TYPE_OF(p) == GLB_NOTIFICATION && ((p >> 16) & 0xFF) == NOTIFY_FW_READY;
    ipc_done();
    if (!ready) { ksnprintf(why, cap, "the firmware's first message was %08x, not FW_READY", p); goto out; }
    if (!fw_version(ver)) { ksnprintf(why, cap, "the firmware did not answer an IPC4 FW_CONFIG request"); goto out; }
    ok = true;
out:
    stream_free(&cl);
    stream_free(&icc);
    w0(EM2, em2);
    if (!ok) {                                       /* core 0 off again; the HD Audio path is untouched */
        w4(ADSPCS, r4(ADSPCS) | CS_CRST(0) | CS_CSTALL(0));
        w4(ADSPCS, r4(ADSPCS) & ~CS_SPA(0));
        w0(d.pp + PPCTL, ppctl);
    }
    return ok;
}

/* -----------------------------------------------------------------------
 * The capture pipeline: pipeline 0 holds copier instance 0 on the DMIC
 * gateway (its input; the NHLT blob is the gateway's configuration) bound
 * to copier instance 1 on the host input gateway of the last input stream
 * (its output), which writes 48 kHz s16 frames into that stream's ring.
 * The stream is decoupled (the processing pipe) and runs as a ring with
 * no SPIB: its LPIB says how far the DSP has written.
 * ----------------------------------------------------------------------- */
static struct {
    DspStream s;                        /* the host input stream the DSP fills */
    bool      created;                  /* pipeline 0 exists */
    bool      running;
    UINT32    rate, channels;
} g_cap;

/* A request whose reply carries no data: false (and @why) unless the
 * firmware answers status 0 */
static bool request(UINT32 pri, UINT32 ext, const char *what, char *why, int cap)
{
    UINT32 p, e;
    if (!ipc4(pri, ext, &p, &e)) { ksnprintf(why, cap, "no reply to %s", what); return false; }
    ipc_done();
    if (p & 0xFFFFFF) { ksnprintf(why, cap, "%s refused (status %u)", what, p & 0xFFFFFF); return false; }
    return true;
}

static bool set_state(UINT32 state, const char *what, char *why, int cap)
{
    return request(IPC4_TYPE(GLB_SET_PIPELINE_STATE) | (UINT32)CAP_PIPELINE << 16 | state, 0, what, why, cap);
}

/* ipc4_audio_format: interleaved little-endian integers, channel i in
 * nibble i of the map */
static UINT32 *put_fmt(UINT32 *p, UINT32 rate, UINT32 depth, UINT32 valid, UINT32 ch)
{
    UINT32 map = 0xFFFFFFFFu;
    for (UINT32 i = 0; i < ch && i < 8; i++) map = (map & ~(0xFu << (4 * i))) | i << (4 * i);
    *p++ = rate;
    *p++ = depth;
    *p++ = map;
    *p++ = ch <= 1 ? 0 : ch == 2 ? 1 : ch == 3 ? 3 : ch == 4 ? 5 : 12;   /* mono, stereo, 3.0, quatro, 7.1 */
    *p++ = 0;
    *p++ = ch | valid << 8 | FMT_LSB_INTEGER << 16;
    return p;
}

/* ipc4_copier_module_cfg: the base config (on the input format, 1 ms
 * buffers), the output format, no features, then the gateway: node,
 * DMA buffer (two output or input milliseconds), configuration */
static UINT32 copier_cfg(UINT32 *out, UINT32 rate, UINT32 ch, UINT32 in_depth, UINT32 in_valid,
                         UINT32 out_depth, UINT32 node, const UINT8 *blob, UINT32 blob_size)
{
    UINT32 ibs = rate / 1000 * ch * (in_depth / 8), obs = rate / 1000 * ch * (out_depth / 8);
    UINT32 *p = out;
    *p++ = 0;                                       /* cycles per chunk: the firmware's own figure */
    *p++ = ibs;
    *p++ = obs;
    *p++ = 0;
    p = put_fmt(p, rate, in_depth, in_valid, ch);
    p = put_fmt(p, rate, out_depth, out_depth, ch);
    *p++ = 0;
    *p++ = node;
    *p++ = 2 * ((node >> 8) == NODE_HOST_INPUT ? obs : ibs);
    UINT32 dw = (blob_size + 3) / 4;
    *p++ = dw;
    if (dw) {
        p[dw - 1] = 0;
        memcpy(p, blob, blob_size);
        p += dw;
    }
    return (UINT32)(p - out);
}

static void box_write(UINT32 box, const UINT32 *v, UINT32 dwords)
{
    for (UINT32 i = 0; i < dwords; i++) w4(box + 4 * i, v[i]);
}

/* Pause, stop the host DMA, reset and delete the pipeline, give the
 * stream back to the link */
static void capture_stop(void)
{
    char why[96];
    if (g_cap.running) set_state(PPL_PAUSED, "SET_PIPELINE_STATE PAUSED", why, sizeof(why));
    if (g_cap.s.bdl) stream_run(&g_cap.s, false);
    if (g_cap.created) {
        set_state(PPL_RESET, "SET_PIPELINE_STATE RESET", why, sizeof(why));
        request(IPC4_TYPE(GLB_DELETE_PIPELINE) | (UINT32)CAP_PIPELINE << 16, 0, "DELETE_PIPELINE", why, sizeof(why));
    }
    stream_free(&g_cap.s);
    memset(&g_cap, 0, sizeof(g_cap));
}

static bool capture_start(const NhltDmic *dm, int copier, char *why, int cap)
{
    static UINT32 cfg[BOX_SIZE / 4];
    UINT32 ch = dm->channels ? dm->channels : 2, depth = dm->bits, valid = dm->valid;
    UINT16 fmt;
    if (copier < 0) { ksnprintf(why, cap, "the firmware has no COPIER module"); return false; }
    if (d.iss < 1) { ksnprintf(why, cap, "the controller has no input stream"); return false; }
    if (dm->rate == 48000) fmt = 0x0000;            /* SD_FMT: 48 kHz base, */
    else if (dm->rate == 16000) fmt = 0x0200;       /*   divided by 3 */
    else { ksnprintf(why, cap, "the microphones' %u Hz is not supported yet", dm->rate); return false; }
    if ((depth != 16 && depth != 32) || ch > 8 || !dm->blob_size) {
        ksnprintf(why, cap, "the microphones' format (%u-bit, %u channels) is not supported yet", depth, ch);
        return false;
    }
    if (dm->blob_size > BOX_SIZE - COPIER_CFG_SIZE) { ksnprintf(why, cap, "the DMIC configuration is too big"); return false; }
    fmt |= (UINT16)(1u << 4 | (ch - 1));            /* 16-bit samples, @ch channels */

    int index = d.iss - 1;                          /* the last input stream (hda.c records on the first) */
    memset(&g_cap, 0, sizeof(g_cap));
    if (!stream_prepare(&g_cap.s, index, (UINT32)index + 1, NULL, CAP_PAGES * PAGE_SIZE, fmt, false)) {
        ksnprintf(why, cap, "out of memory for the capture ring");
        stream_free(&g_cap.s);
        return false;
    }
    UINT32 mod = IPC4_MODULE | (UINT32)copier;
    if (!request(IPC4_TYPE(GLB_CREATE_PIPELINE) | (UINT32)CAP_PIPELINE << 16 | 2, 0, "CREATE_PIPELINE", why, cap))
        goto fail;
    g_cap.created = true;
    UINT32 n = copier_cfg(cfg, dm->rate, ch, depth, valid, depth, NODE_ID(NODE_DMIC_INPUT, dm->vbus),
                          dm->blob, dm->blob_size);
    box_write(INBOX, cfg, n);
    if (!request(mod | IPC4_TYPE(MOD_INIT_INSTANCE) | 0u << 16, n | (UINT32)CAP_PIPELINE << 16,
                 "INIT_INSTANCE of the DMIC copier", why, cap))
        goto fail;
    n = copier_cfg(cfg, dm->rate, ch, depth, valid, CAP_DEPTH, NODE_ID(NODE_HOST_INPUT, index), NULL, 0);
    box_write(INBOX, cfg, n);
    if (!request(mod | IPC4_TYPE(MOD_INIT_INSTANCE) | 1u << 16, n | (UINT32)CAP_PIPELINE << 16,
                 "INIT_INSTANCE of the host copier", why, cap))
        goto fail;
    if (!request(mod | IPC4_TYPE(MOD_BIND) | 0u << 16, (UINT32)copier | 1u << 16 | 0u << 24 | 0u << 27,
                 "BIND", why, cap))
        goto fail;
    if (!set_state(PPL_PAUSED, "SET_PIPELINE_STATE PAUSED", why, cap)) goto fail;
    stream_run(&g_cap.s, true);
    if (!set_state(PPL_RUNNING, "SET_PIPELINE_STATE RUNNING", why, cap)) goto fail;
    g_cap.running = true;
    g_cap.rate = dm->rate;
    g_cap.channels = ch;
    if (!g_m.on)
        kprintf("[DSP] Capture pipeline running: DMIC %u -> copier %d -> input stream %d (tag %d), %u Hz %u-bit %u channel%s\n",
                dm->vbus, copier, index, index + 1, dm->rate, depth, ch, ch == 1 ? "" : "s");
    return true;
fail:
    capture_stop();
    return false;
}

/* The loudest sample of the last 100 ms in the ring, in percent of full
 * scale; -1 when nothing is recording */
static int capture_level(void)
{
    if (!g_cap.running) return -1;
    UINT32 size = CAP_PAGES * PAGE_SIZE, n = g_cap.rate / 10 * g_cap.channels * 2;
    UINT32 pos = r0(g_cap.s.sd + SD_LPIB) % size;
    int peak = 0;
    for (UINT32 k = 0; k < n; k += 2) {
        INT16 v = *(volatile INT16 *)(g_cap.s.buf + (pos + size - n + k) % size);
        int a = v < 0 ? -(int)v : v;
        if (a > peak) peak = a;
    }
    return peak * 100 / 32768;
}

/* Everything from the NHLT table to the running firmware and its capture
 * pipeline; @status says how far it got.  True when the firmware runs. */
static bool start(const HdaHost *h, const UINT8 *nhlt, UINT32 nhlt_len, const UINT8 *fw, UINT32 fw_len,
                  const char *fw_path)
{
    char why[112];
    NhltDmic dm;
    if (!nhlt) { ksnprintf(d.status, sizeof(d.status), "no NHLT table: no digital microphones described"); return false; }
    if (!nhlt_parse(nhlt, nhlt_len, &dm)) { ksnprintf(d.status, sizeof(d.status), "the NHLT table is damaged"); return false; }
    if (!g_m.on)
        kprintf("[DSP] NHLT: %d endpoint%s; %s\n", dm.endpoints, dm.endpoints == 1 ? "" : "s",
                dm.mics ? "digital microphones:" : "no digital microphones");
    if (!dm.mics) { ksnprintf(d.status, sizeof(d.status), "NHLT lists no digital microphones"); return false; }
    if (!g_m.on)
        kprintf("[DSP]   %d microphone%s, %d format%s, first %u Hz %u-bit %u channel%s, %u-byte DMIC configuration\n",
                dm.mics, dm.mics == 1 ? "" : "s", dm.formats, dm.formats == 1 ? "" : "s", dm.rate, dm.bits,
                dm.channels, dm.channels == 1 ? "" : "s", dm.blob_size);
    if (!fw) {
        ksnprintf(d.status, sizeof(d.status), "%d digital microphones; no firmware %s in this build", dm.mics, fw_path);
        return false;
    }
    SofImage img;
    if (!fw_parse(fw, fw_len, &img, why, sizeof(why))) {
        ksnprintf(d.status, sizeof(d.status), "firmware %s: %s", fw_path, why);
        return false;
    }
    if (!g_m.on)
        kprintf("[DSP] Firmware %s: version %u.%u.%u.%u, %u bytes, %d modules (copier %d)\n", fw_path,
                img.ver[0], img.ver[1], img.ver[2], img.ver[3], img.size, img.modules, img.copier);

    d.hb = h->mmio;
    d.iss = (h->gcap >> 8) & 0xF;
    d.oss = (h->gcap >> 12) & 0xF;
    d.pp = find_cap(CAP_PP);
    d.spb = find_cap(CAP_SPB);
    if (!d.pp) { ksnprintf(d.status, sizeof(d.status), "the controller has no processing pipe capability"); return false; }
    if (!d.db) {
        d.db = PciMapBar(&h->dev, 4);
        if (!d.db) { ksnprintf(d.status, sizeof(d.status), "the DSP's registers (BAR4) can't be mapped"); return false; }
    }
    UINT16 ver[4] = { 0 };
    if (!boot(&img, ver, why, sizeof(why))) {
        ksnprintf(d.status, sizeof(d.status), "%d digital microphones; DSP boot failed: %s", dm.mics, why);
        return false;
    }
    if (!capture_start(&dm, img.copier, why, sizeof(why)))
        ksnprintf(d.status, sizeof(d.status), "firmware %u.%u.%u.%u running, %d digital microphones; capture pipeline failed: %s",
                  ver[0], ver[1], ver[2], ver[3], dm.mics, why);
    else
        ksnprintf(d.status, sizeof(d.status), "firmware %u.%u.%u.%u running, %d digital microphones recording (%u Hz, %u channel%s)",
                  ver[0], ver[1], ver[2], ver[3], dm.mics, g_cap.rate, g_cap.channels, g_cap.channels == 1 ? "" : "s");
    return true;
}

static HdaHost g_host;
static int     g_busy;                  /* the boot or the self-check owns d and g_m */

static void lock(void)   { while (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE)) sched_sleep_until(NULL, sched_ticks() + 1); }
static bool trylock(void) { return !__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE); }
static void unlock(void) { __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); }

static void sof_thread(void *arg)
{
    (void)arg;
    bkl_release();
    const char *plat = platform(g_host.dev.device);
    const UINT8 *nhlt = AcpiFindTable("NHLT");
    char path[96] = "";
    UINT32 fw_len = 0;
    const UINT8 *fw = plat ? firmware_file(plat, &fw_len, path, sizeof(path)) : NULL;
    if (!plat) {
        ksnprintf(d.status, sizeof(d.status), "this DSP generation (%04x) is not supported yet", g_host.dev.device);
    } else {
        lock();
        start(&g_host, nhlt, nhlt ? get32(nhlt + 4) : 0, fw, fw_len, path);
        unlock();
    }
    kprintf("[DSP] %s\n", d.status);
    sched_exit_current();
}

void SofStart(void)
{
    if (!HdaDspHost(&g_host)) return;
    if (!g_host.dsp_on) {                           /* (QEMU's controllers; DSP off in the firmware) */
        ksnprintf(d.status, sizeof(d.status), "no audio DSP (the controller is class 04.03)");
        return;
    }
    ksnprintf(d.status, sizeof(d.status), "starting");
    if (!sched_create_thread("dsp", sof_thread, NULL, PRIO_DEVICE_IO))
        ksnprintf(d.status, sizeof(d.status), "could not start its thread");
}

const char *SofStatus(void)
{
    static char line[160];
    if (!d.status[0]) return "no HD Audio controller";
    int level = -1;
    if (trylock()) {                                /* (not while the boot or the self-check runs) */
        level = capture_level();
        unlock();
    }
    if (level < 0) return d.status;
    ksnprintf(line, sizeof(line), "%s, level %d%%", d.status, level);
    return line;
}

bool SofCaptureRing(const INT16 **ring, UINT32 *size, UINT32 *rate, UINT32 *channels)
{
    if (!g_cap.running) return false;
    *ring = (const INT16 *)g_cap.s.buf;
    *size = CAP_PAGES * PAGE_SIZE;
    *rate = g_cap.rate;
    *channels = g_cap.channels;
    return true;
}

UINT32 SofCapturePosition(void)
{
    return g_cap.running ? r0(g_cap.s.sd + SD_LPIB) % (CAP_PAGES * PAGE_SIZE) : 0;
}

/* -----------------------------------------------------------------------
 * SofSelfCheck: a modelled NHLT table, firmware image and DSP
 *
 * The DSP model keeps BAR4 in memory and acts between the driver's looks
 * (WAIT): CPA follows SPA; once core 0 runs, a pending purge naming a
 * stream is taken (DONE, status ROM_INIT); when that stream runs
 * decoupled, the model reads the image through its BDL as the ROM would,
 * compares it with the image, then reports FW_ENTERED and sends
 * FW_READY; a LARGE_CONFIG_GET of FW_CONFIG is answered with a version.
 * ----------------------------------------------------------------------- */
static void model_w4(UINT32 off, UINT32 v)
{
    volatile UINT32 *r = (volatile UINT32 *)(d.db + off);
    switch (off) {
    case ADSPCS:
        *r = (v & 0x00FFFFFF) | (((v >> 16) & 0xFF) << 24);       /* CPA = SPA */
        break;
    case HIPCIDR:
        *r = v;
        if (v & IPC_BUSY) g_m.purge_pending = true;
        break;
    case HIPCIDA:                                                 /* DONE: write 1 to clear */
        *r &= ~(v & IPC_DONE);
        break;
    case HIPCTDR:                                                 /* BUSY: write 1 to clear */
        if (v & IPC_BUSY) *r = 0;
        break;
    default:
        *r = v;
    }
}

static void model_send(UINT32 pri, UINT32 ext)
{
    *(volatile UINT32 *)(d.db + HIPCTDD) = ext;
    *(volatile UINT32 *)(d.db + HIPCTDR) = pri | IPC_BUSY;
}

/* The output stream with this tag, running and decoupled: read the image */
static void model_dma(void)
{
    for (int i = d.iss; i < d.iss + d.oss; i++) {
        UINT32 sd = SD_BASE + (UINT32)i * SD_SIZE, c = r0(sd + SD_CTL);
        if (!(c & SD_CTL_RUN) || (c >> 20) != g_m.tag) continue;
        if (!(r0(d.pp + PPCTL) & (1u << i)) || !(r0(d.pp + PPCTL) & PPCTL_GPROCEN)) continue;
        g_m.l1sen_was_off = !(r0(EM2) & EM2_L1SEN);
        UINT32 want = g_m.expect_size;
        if (d.spb && (r0(d.spb + SPBFCCTL) & (1u << i))) want = r0(d.spb + SPB_SPIB(i));
        BdlEntry *bdl = (BdlEntry *)(uintptr_t)(PHYSMAP_BASE + ((UINT64)r0(sd + SD_BDPU) << 32 | r0(sd + SD_BDPL)));
        UINT32 got = 0;
        bool same = want == g_m.expect_size;
        for (UINT32 k = 0; k <= (r0(sd + SD_LVI) & 0xFF) && got < want && same; k++) {
            const UINT8 *p = (const UINT8 *)(uintptr_t)(PHYSMAP_BASE + bdl[k].addr);
            UINT32 n = bdl[k].len < want - got ? bdl[k].len : want - got;
            same = memcmp(p, g_m.expect + got, n) == 0;
            got += n;
        }
        g_m.got_image = same && got == want;
        g_m.bad_image = !g_m.got_image;
        if (g_m.got_image) {
            *(volatile UINT32 *)(d.db + FW_STATUS) = ST_FW_ENTERED;
            model_send(IPC4_TYPE(GLB_NOTIFICATION) | ((UINT32)NOTIFY_FW_READY << 16), 0);
            g_m.phase = 2;
        } else {
            *(volatile UINT32 *)(d.db + FW_ERROR) = 0xBAD1;
        }
        return;
    }
}

static void model_tick(void)
{
    UINT32 cs = r4(ADSPCS);
    bool runs = (cs & CS_CPA(0)) && !(cs & (CS_CRST(0) | CS_CSTALL(0)));
    if (g_m.phase == 0 && runs && g_m.purge_pending) {
        UINT32 m = r4(HIPCIDR);
        g_m.purge_pending = false;
        if ((m & ~(0x1Fu << 9)) == (IPC_BUSY | ROM_PURGE)) {
            g_m.tag = ((m >> 9) & 0x1F) + 1;
            *(volatile UINT32 *)(d.db + HIPCIDR) = m & ~IPC_BUSY;
            *(volatile UINT32 *)(d.db + HIPCIDA) = IPC_DONE;
            *(volatile UINT32 *)(d.db + FW_STATUS) = ST_ROM_INIT;
            g_m.phase = 1;
        }
    } else if (g_m.phase == 1 && !g_m.bad_image) {
        model_dma();
    } else if (g_m.phase == 2 && (r4(HIPCTDA) & IPC_DONE) && !(r4(HIPCTDR) & IPC_BUSY)) {
        *(volatile UINT32 *)(d.db + HIPCTDA) = 0;
        g_m.phase = 3;
    } else if (g_m.phase == 3 && g_m.purge_pending && !(r4(HIPCTDR) & IPC_BUSY)) {
        UINT32 m = r4(HIPCIDR), x = r4(HIPCIDD);
        g_m.purge_pending = false;
        *(volatile UINT32 *)(d.db + HIPCIDR) = m & ~IPC_BUSY;
        *(volatile UINT32 *)(d.db + HIPCIDA) = IPC_DONE;
        UINT32 reply = (m & ~IPC_BUSY & 0x7F000000u) | IPC4_REPLY, size = 0;
        if ((m & IPC4_MODULE) && IPC4_TYPE_OF(m) == MOD_LARGE_CONFIG_GET && (m & 0xFFFFFF) == 0 &&
            ((x >> 20) & 0xFF) == FW_PARAM_FW_CONFIG) {
            static const UINT32 tlv[] = { 9, 4, 0x1000,               /* (another entry first) */
                                          FW_CFG_FW_VERSION, 8, 2 | 12u << 16, 0 | 1u << 16 };
            for (unsigned i = 0; i < sizeof(tlv) / sizeof(tlv[0]); i++)
                *(volatile UINT32 *)(d.db + OUTBOX + 4 * i) = tlv[i];
            size = sizeof(tlv);
            g_m.fw_configs++;
        } else {
            reply |= model_pipeline(m, x);
        }
        model_send(reply, size);
    }
    if (g_m.phase == 3) model_capture();
}

/* The capture pipeline's requests (SOF's IPC4 layouts): the reply's
 * status, 0 when the model takes it */
static UINT32 refuse(const char *what)
{
    if (!g_m.wrong) g_m.wrong = what;
    return 1;
}

/* The host input stream the host copier names: tag index + 1, decoupled,
 * a ring (no SPIB), and running when @run */
static bool model_host_stream(bool run)
{
    int i = g_m.host_index;
    if (i >= d.iss) return false;
    UINT32 sd = SD_BASE + (UINT32)i * SD_SIZE, c = r0(sd + SD_CTL), pp = r0(d.pp + PPCTL);
    if (((c >> 20) & 0xF) != (UINT32)i + 1 || !(pp & (1u << i)) || !(pp & PPCTL_GPROCEN)) return false;
    if (d.spb && (r0(d.spb + SPBFCCTL) & (1u << i))) return false;
    return !run || (c & SD_CTL_RUN);
}

/* INIT_INSTANCE of a copier: ipc4_copier_module_cfg in the inbox
 * (dwords: 0-3 base, 4-9 input format, 10-15 output format, 16 features,
 * 17 node, 18 DMA buffer, 19 configuration length, then the configuration) */
static UINT32 model_copier(UINT32 id, UINT32 inst, UINT32 x)
{
    const UINT32 *c = (const UINT32 *)(uintptr_t)(d.db + INBOX);
    UINT32 dw = x & 0xFFFF;
    if (id != (UINT32)g_m.copier) return refuse("INIT_INSTANCE of a module that is not the copier");
    if (((x >> 16) & 0xFF) != CAP_PIPELINE || !g_m.pipe) return refuse("a copier outside pipeline 0");
    if (dw * 4 < COPIER_CFG_SIZE || dw * 4 > BOX_SIZE || dw != 20 + c[19])
        return refuse("a copier payload whose size does not match its gateway configuration");
    if (c[4] != 48000 || c[10] != 48000 || (c[9] & 0xFF) != 2 || (c[15] & 0xFF) != 2 || c[6] != 0xFFFFFF10 || c[7] != 1)
        return refuse("a copier format other than the microphones' 48000 Hz stereo");
    if (c[1] != 48 * 2 * c[5] / 8 || c[2] != 48 * 2 * c[11] / 8 || ((c[9] >> 16) & 0xFF) != FMT_LSB_INTEGER)
        return refuse("copier buffers not 1 ms of integer samples");
    if (inst == 0) {
        if (c[17] != NODE_ID(NODE_DMIC_INPUT, 0)) return refuse("the first copier not on DMIC gateway 0");
        if (c[5] != 16 || c[11] != 16) return refuse("the DMIC copier not in NHLT's 16-bit format");
        if (c[19] != (g_m.blob_size + 3) / 4 || memcmp(c + 20, g_m.blob, g_m.blob_size) != 0)
            return refuse("the DMIC copier without the NHLT blob");
        g_m.dmic_ok = true;
    } else if (inst == 1) {
        if ((c[17] >> 8) != NODE_HOST_INPUT) return refuse("the second copier not on a host input gateway");
        g_m.host_index = (int)(c[17] & 0xFF);
        if (!model_host_stream(false)) return refuse("the host copier's stream is not a decoupled ring with tag index + 1");
        if (c[11] != CAP_DEPTH || c[19]) return refuse("the host copier not writing 16-bit samples, or configured");
        g_m.host_ok = true;
    } else {
        return refuse("a third copier");
    }
    return 0;
}

static UINT32 model_pipeline(UINT32 m, UINT32 x)
{
    bool mod = m & IPC4_MODULE;
    UINT32 type = IPC4_TYPE_OF(m), lo = m & 0xFFFF, hi = (m >> 16) & 0xFF;
    if (++g_m.requests == g_m.refuse) return 7;
    if (!mod && type == GLB_CREATE_PIPELINE) {
        if (g_m.pipe || hi != CAP_PIPELINE || !(lo & 0x7FF)) return refuse("CREATE_PIPELINE twice or without memory");
        g_m.pipe = 1;
        return 0;
    }
    if (!mod && type == GLB_SET_PIPELINE_STATE) {
        if (!g_m.pipe || hi != CAP_PIPELINE) return refuse("SET_PIPELINE_STATE of a pipeline that does not exist");
        if (lo == PPL_PAUSED && !g_m.bound) return refuse("PAUSED before the copiers were bound");
        if (lo == PPL_RUNNING && (g_m.pipe != PPL_PAUSED || !model_host_stream(true)))
            return refuse("RUNNING other than from PAUSED with the host DMA on");
        if (lo == PPL_RESET && g_m.pipe == PPL_RUNNING) return refuse("RESET while RUNNING");
        if (lo != PPL_RESET && lo != PPL_PAUSED && lo != PPL_RUNNING) return refuse("an unknown pipeline state");
        g_m.pipe = (int)lo;
        return 0;
    }
    if (!mod && type == GLB_DELETE_PIPELINE) {
        if (!g_m.pipe || hi != CAP_PIPELINE || g_m.pipe == PPL_RUNNING) return refuse("DELETE_PIPELINE while RUNNING");
        g_m.pipe = 0;
        g_m.dmic_ok = g_m.host_ok = g_m.bound = false;
        g_m.deleted++;
        return 0;
    }
    if (mod && type == MOD_INIT_INSTANCE) return model_copier(lo, hi, x);
    if (mod && type == MOD_BIND) {
        if (lo != (UINT32)g_m.copier || hi != 0 || (x & 0xFFFF) != (UINT32)g_m.copier || ((x >> 16) & 0xFF) != 1 ||
            ((x >> 24) & 0x3F) != 0 || !g_m.dmic_ok || !g_m.host_ok)
            return refuse("BIND other than DMIC copier queue 0 to host copier queue 0");
        g_m.bound = true;
        return 0;
    }
    return refuse("an unknown request");
}

/* The DSP's host DMA while RUNNING: 10 ms of a 1 kHz square wave at a
 * quarter of full scale into the ring at LPIB, through the BDL */
static void model_capture(void)
{
    if (g_m.pipe != PPL_RUNNING || !model_host_stream(true)) return;
    UINT32 sd = SD_BASE + (UINT32)g_m.host_index * SD_SIZE, cbl = r0(sd + SD_CBL), pos = r0(sd + SD_LPIB);
    BdlEntry *bdl = (BdlEntry *)(uintptr_t)(PHYSMAP_BASE + ((UINT64)r0(sd + SD_BDPU) << 32 | r0(sd + SD_BDPL)));
    for (int f = 0; f < 480; f++, g_m.frames++)
        for (int ch = 0; ch < 2; ch++) {
            UINT32 at = pos, k = 0;
            while (at >= bdl[k].len) at -= bdl[k++].len;
            *(INT16 *)(uintptr_t)(PHYSMAP_BASE + bdl[k].addr + at) = (g_m.frames / 24) & 1 ? 8192 : -8192;
            pos = (pos + 2) % cbl;
        }
    w0(sd + SD_LPIB, pos);
}

/* A table like a two-microphone laptop's: an SSP endpoint (a Bluetooth
 * link) to skip, then the DMIC array with two formats */
static UINT32 model_nhlt(UINT8 *t)
{
    UINT32 n = 37;
    memset(t, 0, 512);
    memcpy(t, "NHLT", 4);
    t[36] = 2;
    /* SSP render endpoint: no capabilities, no formats */
    UINT8 *e = t + n;
    e[0] = 24; e[4] = 3; e[17] = 0;
    n += 24;
    /* PDM capture endpoint */
    e = t + n;
    UINT32 p = 23;
    e[4] = 2; e[16] = 1; e[17] = 1;
    e[19] = 3; p += 3;                              /* capabilities: slot 0, mic array, small linear 2-mic */
    e[23] = 0; e[24] = 1; e[25] = 0x0A;
    e[p++] = 2;                                     /* two formats */
    for (int f = 0; f < 2; f++) {
        UINT8 *w = e + p;
        UINT16 bits = f ? 32 : 16;
        w[0] = 0xFE; w[1] = 0xFF;                   /* WAVE_FORMAT_EXTENSIBLE */
        w[2] = 2;
        w[4] = 0x80; w[5] = 0xBB;                   /* 48000 */
        w[14] = (UINT8)bits;
        w[40] = 16;                                 /* a 16-byte configuration */
        for (int k = 0; k < 16; k++) w[44 + k] = (UINT8)(0xA0 + k);
        p += 44 + 16;
    }
    e[0] = (UINT8)p; e[1] = (UINT8)(p >> 8);
    n += p;
    t[4] = (UINT8)n; t[5] = (UINT8)(n >> 8);
    return n;
}

/* A small firmware file: extended manifest, $CPD, $AM1 with three modules
 * (the copier third), then filler past three pages */
static UINT32 model_firmware(UINT8 *f)
{
    UINT32 size = 0x40 + 3 * PAGE_SIZE + 100;
    for (UINT32 i = 0; i < size; i++) f[i] = (UINT8)(i * 7 + 3);
    memset(f, 0, 0x40 + 0x200 + 3 * AM_ENTRY_SIZE);
    memcpy(f, "$AE1", 4); f[4] = 0x40;
    UINT8 *c = f + 0x40;
    memcpy(c, "$CPD", 4); c[8] = 2; c[9] = 1; c[10] = 0x14; memcpy(c + 12, "ADSP", 4);
    UINT8 *a = c + 0x100;
    memcpy(a, "$AM1", 4); a[4] = 0x34; memcpy(a + 8, "ADSPFW", 6);
    a[28] = 2; a[30] = 12; a[34] = 1;               /* version 2.12.0.1 */
    a[36] = 3;
    static const char *names[] = { "BRNGUP", "BASEFW", "COPIER" };
    for (int m = 0; m < 3; m++) {
        UINT8 *e = a + 0x34 + m * AM_ENTRY_SIZE;
        memcpy(e, "$AME", 4);
        memcpy(e + 4, names[m], strlen(names[m]));
    }
    return size;
}

int SofSelfCheck(void (*say)(void *ctx, const char *line), void *ctx)
{
    int failed = 0;
    char line[176], why[112];
#define CHECK(ok, ...) do { bool ok_ = (ok); ksnprintf(line, sizeof(line), __VA_ARGS__); \
                            char out_[192]; ksnprintf(out_, sizeof(out_), "%s %s", ok_ ? "ok  " : "FAIL", line); \
                            say(ctx, out_); if (!ok_) failed++; } while (0)

    /* NHLT */
    static UINT8 table[512];
    UINT32 tlen = model_nhlt(table);
    NhltDmic dm;
    bool parsed = nhlt_parse(table, tlen, &dm);
    CHECK(parsed && dm.endpoints == 2 && dm.mics == 2,
          "DSP: NHLT: the DMIC endpoint found past an SSP one, %d microphones (array type 0x0A)", dm.mics);
    CHECK(parsed && dm.formats == 2 && dm.rate == 48000 && dm.bits == 16 && dm.channels == 2 &&
          dm.blob_size == 16 && dm.blob && dm.blob[0] == 0xA0,
          "DSP: NHLT: 2 formats, first 48000 Hz 16-bit stereo with its 16-byte DMIC configuration");
    table[37 + 24] = 0xFF;                           /* an endpoint longer than the table */
    CHECK(!nhlt_parse(table, tlen, &dm), "DSP: NHLT: a damaged table is refused");

    /* Firmware manifest; the T14's controller picks the Raptor Lake firmware */
    UINT32 fpages = (0x40 + 3 * PAGE_SIZE + 100 + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT8 *fw = kernel_alloc_pages(fpages);
    UINT8 *bar0 = kernel_alloc_pages(2);
    UINT8 *bar4 = kernel_alloc_pages(BAR4_SPAN / PAGE_SIZE);
    if (!fw || !bar0 || !bar4) {
        say(ctx, "FAIL DSP: out of memory for the model");
        return failed + 1;
    }
    UINT32 flen = model_firmware(fw);
    SofImage img;
    bool fwok = fw_parse(fw, flen, &img, why, sizeof(why));
    CHECK(fwok && img.size == flen - 0x40 && img.modules == 3 && img.copier == 2 && img.ver[0] == 2 && img.ver[1] == 12,
          "DSP: firmware: image from $CPD (%u bytes), version %u.%u.%u.%u, 3 modules, copier is module %d",
          img.size, img.ver[0], img.ver[1], img.ver[2], img.ver[3], img.copier);
    CHECK(platform(0x51CA) && strcmp(platform(0x51CA), "rpl") == 0 && !platform(0x7E28) && !platform(0x2668),
          "DSP: 8086:51ca (T14 Gen 4) takes sof-rpl.ri; Meteor Lake and QEMU's controllers none");

    /* The firmware built in, if tools/fetch_sof_firmware.py fetched it */
    char path[96];
    UINT32 rlen = 0;
    const UINT8 *real = firmware_file("rpl", &rlen, path, sizeof(path));
    if (real) {
        SofImage ri;
        bool rok = fw_parse(real, rlen, &ri, why, sizeof(why));
        CHECK(rok && ri.copier >= 0, "DSP: built-in %s: version %u.%u.%u.%u, %d modules, copier is module %d%s%s",
              path, ri.ver[0], ri.ver[1], ri.ver[2], ri.ver[3], ri.modules, ri.copier, rok ? "" : ": ", rok ? "" : why);
    } else {
        say(ctx, "     DSP: no SOF firmware built in (tools/fetch_sof_firmware.py fetches it)");
    }

    /* The boot against the modelled DSP, then the capture pipeline */
    lock();
    static UINT8 saved[sizeof(d)], saved_cap[sizeof(g_cap)];
    memcpy(saved, &d, sizeof(d));
    memcpy(saved_cap, &g_cap, sizeof(g_cap));
    memset(&g_cap, 0, sizeof(g_cap));
    memset(bar0, 0, 2 * PAGE_SIZE);
    *(UINT16 *)(bar0 + 0x00) = (UINT16)(9 << 12 | 7 << 8);   /* GCAP: 7 in, 9 out (as Raptor Lake) */
    *(UINT32 *)(bar0 + LLCH) = 0x800;
    *(UINT32 *)(bar0 + 0x800) = CAP_PP << 16 | 0x700;        /* processing pipe, then SPB */
    *(UINT32 *)(bar0 + 0x700) = CAP_SPB << 16;
    HdaHost h;
    memset(&h, 0, sizeof(h));
    h.mmio = bar0;
    h.gcap = *(UINT16 *)bar0;
    h.dsp_on = true;
    /* (a fresh modelled DSP: BAR4 cleared, EM2 and the processing pipe as at power-on) */
#define MODEL(image_size, refuse_nth) do {                                           \
        memset(bar4, 0, BAR4_SPAN);                                                  \
        memset(&g_m, 0, sizeof(g_m));                                                \
        *(UINT32 *)(bar0 + EM2) = EM2_L1SEN;                                         \
        *(UINT32 *)(bar0 + 0x800 + PPCTL) = 0;                                       \
        g_m.on = true;                                                               \
        g_m.expect = img.image;                                                      \
        g_m.expect_size = (image_size);                                              \
        g_m.copier = img.copier;                                                     \
        g_m.blob = dm.blob;                                                          \
        g_m.blob_size = dm.blob_size;                                                \
        g_m.refuse = (refuse_nth);                                                   \
        d.db = bar4;                                                                 \
    } while (0)
    tlen = model_nhlt(table);
    nhlt_parse(table, tlen, &dm);
    MODEL(img.size, 0);
    bool ran = start(&h, table, tlen, fw, flen, "(model)");
    char status[128];
    strncpy(status, d.status, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
    UINT32 pp = r0(d.pp + PPCTL);
    bool cl_freed = !(pp & (1u << (7 + 9 - 1))) && !(r0(d.spb + SPBFCCTL));
    bool l1 = (r0(EM2) & EM2_L1SEN) && g_m.l1sen_was_off;
    UINT32 cs = r4(ADSPCS);

    CHECK(g_m.tag == 9, "DSP: core 0 powered, the ROM told to load from output stream tag %u (want 9, the last)", g_m.tag);
    CHECK(g_m.got_image, "DSP: the ROM read the whole image through the code loader's BDL (%u bytes, 4 pages)", img.size);
    CHECK(cl_freed && l1, "DSP: code loader stream recoupled, SPIB off, L1 power saving off while loading and back on");
    CHECK(ran && (cs & CS_CPA(0)) && g_m.fw_configs == 1,
          "DSP: FW_READY answered, IPC4 FW_CONFIG request: %s", status);
    CHECK(g_m.dmic_ok, "DSP: capture: copier (module %d) on DMIC gateway 0 with the NHLT blob, 48000 Hz 16-bit stereo%s%s",
          img.copier, g_m.wrong ? ": " : "", g_m.wrong ? g_m.wrong : "");
    CHECK(g_m.host_ok && g_m.host_index == 6 && (pp & (1u << 6)) && g_cap.s.pages == CAP_PAGES,
          "DSP: capture: copier on host input gateway %d, its stream decoupled, a %u KiB ring with no SPIB",
          g_m.host_index, CAP_PAGES * PAGE_SIZE / 1024);
    CHECK(g_m.bound && g_m.pipe == PPL_RUNNING && g_cap.running && strstr(status, "recording (48000 Hz, 2 channels)"),
          "DSP: capture: CREATE_PIPELINE, two INIT_INSTANCE, BIND, PAUSED, host DMA on, RUNNING");
    for (int k = 0; k < 20; k++) model_tick();                  /* (200 ms of the modelled DMA) */
    UINT32 pos = 0, rsize = 0, rrate = 0, rch = 0;
    const INT16 *ring = NULL;
    bool have = SofCaptureRing(&ring, &rsize, &rrate, &rch);
    pos = SofCapturePosition();
    int level = capture_level();
    CHECK(have && rsize == CAP_PAGES * PAGE_SIZE && rrate == 48000 && rch == 2 && pos == g_m.frames * 4 % rsize &&
          level == 25 && ring[0] == -8192,
          "DSP: capture: samples in the host ring, position %u, level %d%% (a quarter-scale square wave)", pos, level);
    capture_stop();
    pp = r0(d.pp + PPCTL);
    CHECK(g_m.deleted == 1 && !g_m.pipe && !g_m.wrong && !(pp & (1u << 6)) && !g_cap.running,
          "DSP: capture stop: PAUSED, host DMA off, RESET, DELETE_PIPELINE, the stream recoupled%s%s",
          g_m.wrong ? ": " : "", g_m.wrong ? g_m.wrong : "");
    g_m.on = false;
    memcpy(&d, saved, sizeof(d));

    /* A firmware that never reports FW_ENTERED: the core goes off again */
    MODEL(img.size - 1, 0);                          /* (so the image does not match) */
    ran = start(&h, table, tlen, fw, flen, "(model)");
    cs = r4(ADSPCS);
    pp = r0(d.pp + PPCTL);
    g_m.on = false;
    memcpy(&d, saved, sizeof(d));
    CHECK(!ran && !(cs & CS_SPA(0)) && (cs & CS_CRST(0)) && pp == 0,
          "DSP: a bad image: boot fails, core 0 off and in reset, processing pipe as before");

    /* A firmware that refuses the host copier: the pipeline goes, the stream too */
    MODEL(img.size, 3);
    ran = start(&h, table, tlen, fw, flen, "(model)");
    strncpy(status, d.status, sizeof(status) - 1);
    pp = r0(d.pp + PPCTL);
    g_m.on = false;
    memcpy(&d, saved, sizeof(d));
    CHECK(ran && !g_cap.running && !g_cap.s.bdl && g_m.deleted == 1 && !(pp & (1u << 6)) &&
          strstr(status, "capture pipeline failed: INIT_INSTANCE of the host copier refused"),
          "DSP: a refused copier: the pipeline deleted, the stream recoupled, the firmware left running");
#undef MODEL
    memcpy(&g_cap, saved_cap, sizeof(g_cap));

    unlock();
    kernel_free_pages(fw, fpages);
    kernel_free_pages(bar0, 2);
    kernel_free_pages(bar4, BAR4_SPAN / PAGE_SIZE);
#undef CHECK
    return failed;
}
