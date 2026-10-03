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
 * Not yet: the capture pipeline (IPC4 copier modules from the DMIC gateway,
 * configured with the NHLT blob, to a host input stream) and the recording
 * device audio.c would offer for it; docs/hardware.md lists those steps.
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
    UINT16       bits, channels;
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
    int          requests;
} g_m;

static void model_tick(void);
static void model_w4(UINT32 off, UINT32 v);

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

static bool stream_prepare(DspStream *s, int index, UINT32 tag, const UINT8 *data, UINT32 size)
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
    w0w(s->sd + SD_FMT, CL_FORMAT);
    w0(s->sd + SD_CTL, tag << 20);
    w0(d.pp + PPCTL, r0(d.pp + PPCTL) | (1u << index));          /* decoupled: the DSP's DMA */
    if (d.spb) {                                                  /* stop at the image's end */
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
    if (d.iss && !stream_prepare(&icc, d.iss - 1, (UINT32)d.iss, NULL, PAGE_SIZE)) {
        ksnprintf(why, cap, "out of memory");
        goto out;
    }
    if (!stream_prepare(&cl, cl_index, tag, img->image, img->size)) {
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

/* Everything from the NHLT table to the running firmware; @status says
 * how far it got */
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
    ksnprintf(d.status, sizeof(d.status), "firmware %u.%u.%u.%u running, %d digital microphones (not yet recording)",
              ver[0], ver[1], ver[2], ver[3], dm.mics);
    return true;
}

static HdaHost g_host;
static int     g_busy;                  /* the boot or the self-check owns d and g_m */

static void lock(void)   { while (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE)) sched_sleep_until(NULL, sched_ticks() + 1); }
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

const char *SofStatus(void) { return d.status[0] ? d.status : "no HD Audio controller"; }

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
        g_m.requests++;
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
        } else {
            reply |= 1;                                           /* (an error status) */
        }
        model_send(reply, size);
    }
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

    /* The boot against the modelled DSP */
    lock();
    static UINT8 saved[sizeof(d)];
    memcpy(saved, &d, sizeof(d));
    memset(bar0, 0, 2 * PAGE_SIZE);
    memset(bar4, 0, BAR4_SPAN);
    memset(&g_m, 0, sizeof(g_m));
    *(UINT16 *)(bar0 + 0x00) = (UINT16)(9 << 12 | 7 << 8);   /* GCAP: 7 in, 9 out (as Raptor Lake) */
    *(UINT32 *)(bar0 + LLCH) = 0x800;
    *(UINT32 *)(bar0 + 0x800) = CAP_PP << 16 | 0x700;        /* processing pipe, then SPB */
    *(UINT32 *)(bar0 + 0x700) = CAP_SPB << 16;
    *(UINT32 *)(bar0 + EM2) = EM2_L1SEN;
    g_m.on = true;
    g_m.expect = img.image;
    g_m.expect_size = img.size;
    d.db = bar4;
    HdaHost h;
    memset(&h, 0, sizeof(h));
    h.mmio = bar0;
    h.gcap = *(UINT16 *)bar0;
    h.dsp_on = true;
    bool ran = start(&h, table, model_nhlt(table), fw, flen, "(model)");
    char status[128];
    strncpy(status, d.status, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
    bool cl_freed = !(r0(d.pp + PPCTL) & 0x3FFFFFFF) && !(r0(d.spb + SPBFCCTL));
    bool l1 = (r0(EM2) & EM2_L1SEN) && g_m.l1sen_was_off;
    UINT32 cs = r4(ADSPCS);
    g_m.on = false;
    memcpy(&d, saved, sizeof(d));

    CHECK(g_m.tag == 9, "DSP: core 0 powered, the ROM told to load from output stream tag %u (want 9, the last)", g_m.tag);
    CHECK(g_m.got_image, "DSP: the ROM read the whole image through the code loader's BDL (%u bytes, 4 pages)", img.size);
    CHECK(cl_freed && l1, "DSP: code loader stream recoupled, SPIB off, L1 power saving off while loading and back on");
    CHECK(ran && (cs & CS_CPA(0)) && g_m.phase == 3 && g_m.requests == 1,
          "DSP: FW_READY answered, IPC4 FW_CONFIG request: %s", status);

    /* A firmware that never reports FW_ENTERED: the core goes off again */
    memset(bar4, 0, BAR4_SPAN);
    memset(&g_m, 0, sizeof(g_m));
    *(UINT32 *)(bar0 + EM2) = EM2_L1SEN;
    *(UINT32 *)(bar0 + 0x800 + PPCTL) = 0;           /* (the processing pipe off again, as at power-on) */
    g_m.on = true;
    g_m.expect = img.image;
    g_m.expect_size = img.size - 1;                  /* (so the image does not match) */
    d.db = bar4;
    ran = start(&h, table, model_nhlt(table), fw, flen, "(model)");
    strncpy(status, d.status, sizeof(status) - 1);
    cs = r4(ADSPCS);
    UINT32 pp = r0(d.pp + PPCTL);
    g_m.on = false;
    memcpy(&d, saved, sizeof(d));
    CHECK(!ran && !(cs & CS_SPA(0)) && (cs & CS_CRST(0)) && pp == 0,
          "DSP: a bad image: boot fails, core 0 off and in reset, processing pipe as before");

    unlock();
    kernel_free_pages(fw, fpages);
    kernel_free_pages(bar0, 2);
    kernel_free_pages(bar4, BAR4_SPAN / PAGE_SIZE);
#undef CHECK
    return failed;
}
