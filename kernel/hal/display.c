/*
 * display.c — display adapter driver (Bochs VBE "DISPI", Cirrus, GOP fallback)
 *
 * NovaOS boots through UEFI, so the VESA BIOS (int 10h) is gone by the
 * time the kernel runs.  The register interface behind QEMU's and
 * Bochs's VBE BIOS is still there, though: sixteen 16-bit "DISPI"
 * registers, reached through I/O ports 0x1CE/0x1CF (index/data) or, on
 * adapters with an MMIO BAR, at BAR2 + 0x500.  They set the resolution,
 * the bits per pixel, the virtual (scanline) width and the display start
 * offset, and report the video memory size, which is all a 2D driver
 * needs: modes at run time, and page flipping by moving the display
 * start between two pages of video memory.  Every VGA-compatible QEMU
 * adapter but Cirrus has them (std VGA, bochs-display, QXL, virtio-vga,
 * and VMware SVGA's VGA core), and so does VirtualBox's VBoxVGA; the
 * table of them follows OVMF's QemuVideoDxe (BSD-2-Clause-Patent).
 *
 * The Cirrus CL-GD5446 (QEMU -vga cirrus) has no DISPI registers: its
 * modes are VGA timing registers plus Cirrus extensions, set from
 * QemuVideoDxe's register tables (640x480 and 800x600 at 32 bpp).
 *
 * The adapter's linear framebuffer (BAR0; BAR1 on VMware SVGA) is the
 * same memory the GOP handed over at boot.  If it is not the boot
 * framebuffer (the GOP was on another adapter) the driver stays out of
 * the way.
 *
 * S3 sleep powers the adapter off and the firmware doesn't set a mode
 * again on wake (no GOP then), so DisplayResume() puts the current mode
 * back from what the driver knows.  An adapter with no driver here keeps
 * whatever the firmware's wake path does.
 */

#include "display.h"
#include "framebuffer.h"
#include "pci.h"
#include "../drivers/virtio_gpu.h"
#include "../arch/x86_64/cpu.h"
#include "../ke/printf.h"
#include "../lib/string.h"

/* DISPI registers */
#define VBE_INDEX_PORT   0x01CE
#define VBE_DATA_PORT    0x01CF
#define VBE_MMIO_OFFSET  0x0500           /* in BAR2, 16-bit registers */
#define VGA_MMIO_OFFSET  0x0400           /* in BAR2: VGA ports 0x3C0.. */

enum {
    VBE_ID, VBE_XRES, VBE_YRES, VBE_BPP, VBE_ENABLE, VBE_BANK,
    VBE_VIRT_WIDTH, VBE_VIRT_HEIGHT, VBE_X_OFFSET, VBE_Y_OFFSET,
    VBE_VIDEO_MEMORY_64K,
};
#define VBE_ID_MIN        0xB0C2          /* 32 bpp, virtual size, offsets */
#define VBE_ID_MAX        0xB0CF
#define VBE_ENABLED       0x01
#define VBE_GETCAPS       0x02            /* XRES/YRES/BPP read back the maximums */
#define VBE_LFB_ENABLED   0x40
#define VBE_NOCLEARMEM    0x80

/* VGA ports */
#define VGA_ATTR          0x3C0
#define VGA_MISC_WRITE    0x3C2
#define VGA_SEQ           0x3C4
#define VGA_SEQ_DATA      0x3C5
#define VGA_DAC_MASK      0x3C6
#define VGA_GFX           0x3CE
#define VGA_CRTC          0x3D4
#define VGA_STATUS        0x3DA           /* read: resets the attribute flip-flop */

typedef enum { DRV_NONE, DRV_GOP, DRV_BOCHS, DRV_CIRRUS, DRV_VIRTIO } DrvKind;

static struct {
    DrvKind      kind;
    const char  *name;                    /* the adapter */
    bool         mmio;                    /* registers in BAR2, not I/O ports */
    volatile UINT8 *bar2;
    volatile UINT16 *regs;
    bool         vga;                     /* has the VGA registers (unblank on wake) */
    UINT64       vram_phys;
    UINT64       vram_size;
    int          max_w, max_h;
    bool         bgr;

    DisplayMode  modes[DISPLAY_MAX_MODES];
    int          nmodes;
    DisplayMode  boot, cur;
    DisplayMode  def;                     /* the user's choice (the "registry" mode) */
    int          stride;                  /* pixels per scanline */
    bool         flip;                    /* two pages fit */
    int          page;                    /* the page on screen: 0 or 1 */
    int          vdev;                    /* DRV_VIRTIO: the virtio GPU (output 0) */
} d;

static UINT16 vbe_read(int reg)
{
    if (d.mmio) return d.regs[reg];
    outw(VBE_INDEX_PORT, (UINT16)reg);
    return inw(VBE_DATA_PORT);
}

static void vbe_write(int reg, UINT16 v)
{
    if (d.mmio) { d.regs[reg] = v; return; }
    outw(VBE_INDEX_PORT, (UINT16)reg);
    outw(VBE_DATA_PORT, v);
}

/* VGA port @port (0x3C0-0x3DF): through BAR2 on MMIO adapters */
static UINT8 vga_in(UINT16 port)
{
    if (d.mmio) return d.bar2[VGA_MMIO_OFFSET + port - 0x3C0];
    return inb(port);
}

static void vga_out(UINT16 port, UINT8 v)
{
    if (d.mmio) { d.bar2[VGA_MMIO_OFFSET + port - 0x3C0] = v; return; }
    outb(port, v);
}

/* The attribute controller's "palette from the CPU" bit off: the picture
 * on.  The status register (which resets the controller's index/data
 * flip-flop) is at 0x3DA with colour addressing, which a reset clears. */
static void vga_unblank(bool set_colour)
{
    if (set_colour) vga_out(VGA_MISC_WRITE, 0x01);
    (void)vga_in(VGA_STATUS);
    vga_out(VGA_ATTR, 0x20);
}

/* Point the framebuffer console (and so the GDI) at the page on screen */
static void publish_surface(void)
{
    UINT64 off = (UINT64)d.page * d.cur.h * d.stride * 4;
    fb_set_surface(d.vram_phys + off, d.cur.w, d.cur.h, d.stride, d.bgr);
}

/* -----------------------------------------------------------------------
 * Mode list
 * ----------------------------------------------------------------------- */
static const DisplayMode g_common[] = {
    { 3840, 2160 }, { 2560, 1600 }, { 2560, 1440 }, { 1920, 1200 }, { 1920, 1080 },
    { 1680, 1050 }, { 1600, 1200 }, { 1600,  900 }, { 1440,  900 }, { 1360,  768 },
    { 1280, 1024 }, { 1280,  800 }, { 1280,  720 }, { 1152,  864 }, { 1024,  768 },
    {  800,  600 }, {  640,  480 },
};

static bool fits(int w, int h)
{
    return w <= d.max_w && h <= d.max_h && (w & 7) == 0 &&
           (UINT64)w * h * 4 <= d.vram_size;
}

static void add_mode_to(DisplayMode *modes, int *n, int w, int h)
{
    if (*n >= DISPLAY_MAX_MODES) return;
    for (int i = 0; i < *n; i++)
        if (modes[i].w == w && modes[i].h == h) return;
    int j = (*n)++;                       /* keep largest (by area) first */
    while (j > 0 && (UINT64)modes[j - 1].w * modes[j - 1].h < (UINT64)w * h) {
        modes[j] = modes[j - 1];
        j--;
    }
    modes[j] = (DisplayMode){ w, h };
}

static void add_mode(int w, int h) { add_mode_to(d.modes, &d.nmodes, w, h); }

/* -----------------------------------------------------------------------
 * Bochs VBE
 * ----------------------------------------------------------------------- */
/* The adapters with DISPI registers, as OVMF's QemuVideoDxe drives them */
static const struct {
    UINT16      vendor, device;
    bool        vga_only;                 /* not the same IDs' non-VGA variant */
    int         fb_bar;                   /* the linear framebuffer */
    bool        mmio;                     /* registers in BAR2 (else I/O ports) */
    const char *name;
} g_dispi[] = {
    { 0x1234, 0x1111, false, 0, true,  "QEMU standard VGA" },  /* also bochs-display */
    { 0x1AF4, 0x1050, true,  0, true,  "QEMU virtio-vga" },
    { 0x1B36, 0x0100, true,  0, false, "QEMU QXL" },
    { 0x15AD, 0x0405, true,  1, false, "VMware SVGA II" },
    { 0x80EE, 0xBEEF, false, 0, false, "VirtualBox VGA" },
};

static bool bochs_probe(const BootFramebuffer *boot)
{
    PciDevice pci;
    int t = -1;
    UINT64 fb = 0;
    for (unsigned i = 0; i < sizeof(g_dispi) / sizeof(g_dispi[0]); i++) {
        UINT16 id = g_dispi[i].device;
        if (!PciFind(g_dispi[i].vendor, &id, 1, &pci)) continue;
        if (g_dispi[i].vga_only && !(pci.class_code == 0x03 && pci.subclass == 0x00)) continue;
        fb = PciBarAddress(&pci, g_dispi[i].fb_bar);
        if (!fb || fb != boot->base) {
            kprintf("[DISPLAY] %s at %llx is not the boot display (%llx)\n", g_dispi[i].name,
                    (unsigned long long)fb, (unsigned long long)boot->base);
            continue;
        }
        t = (int)i;
        break;
    }
    if (t < 0) return false;
    if (fb + boot->size > PHYSMAP_SIZE) return false;       /* the console's physmap view */
    /* The registers: BAR2 (MMIO) when there is one, else the I/O ports
     * (-vga std decodes both; bochs-display has only MMIO) */
    d.bar2 = g_dispi[t].mmio ? (volatile UINT8 *)PciMapBar(&pci, 2) : NULL;
    d.mmio = d.bar2 != NULL;
    d.vga = pci.class_code == 0x03 && pci.subclass == 0x00;
    if (!d.mmio && !d.vga) return false;
    d.regs = d.mmio ? (volatile UINT16 *)(d.bar2 + VBE_MMIO_OFFSET) : NULL;
    d.name = pci.vendor == 0x1234 && !d.vga ? "QEMU bochs-display" : g_dispi[t].name;

    UINT16 id = vbe_read(VBE_ID);
    if (id < VBE_ID_MIN || id > VBE_ID_MAX) {
        kprintf("[DISPLAY] %s reports VBE ID %04x; not using it\n", d.name, id);
        return false;
    }

    d.vram_phys = fb;
    d.vram_size = (UINT64)vbe_read(VBE_VIDEO_MEMORY_64K) * 64 * 1024;
    if (!d.vram_size) d.vram_size = boot->size;

    /* The maximum resolution: GETCAPS makes XRES/YRES read it back.
     * Setting the bit on an enabled adapter does not touch the mode. */
    UINT16 en = vbe_read(VBE_ENABLE);
    vbe_write(VBE_ENABLE, en | VBE_GETCAPS);
    d.max_w = vbe_read(VBE_XRES);
    d.max_h = vbe_read(VBE_YRES);
    vbe_write(VBE_ENABLE, en);
    if (d.max_w <= 0 || d.max_h <= 0) { d.max_w = 4096; d.max_h = 4096; }

    d.bgr = true;                          /* little-endian XRGB: byte0 = blue */
    d.kind = DRV_BOCHS;
    if (d.vga) vga_unblank(false);

    kprintf("[DISPLAY] %s: VBE %04x (%04x:%04x) at %02x:%02x.%x, %llu MB video memory, "
            "max %dx%d, registers via %s\n", d.name, id, pci.vendor, pci.device,
            pci.bus, pci.dev, pci.func, (unsigned long long)(d.vram_size >> 20),
            d.max_w, d.max_h, d.mmio ? "MMIO" : "I/O ports");
    return true;
}

/* The adapter's current geometry → d.cur, d.stride, d.flip */
static void bochs_read_mode(void)
{
    d.cur.w = vbe_read(VBE_XRES);
    d.cur.h = vbe_read(VBE_YRES);
    d.stride = vbe_read(VBE_VIRT_WIDTH);
    if (d.stride < d.cur.w) d.stride = d.cur.w;
    int vh = vbe_read(VBE_VIRT_HEIGHT);

    /* Flipping needs a second page below the first, and the display start
     * must actually move there (the adapter clamps the offset) */
    d.flip = false;
    d.page = 0;
    if (vh >= 2 * d.cur.h && (UINT64)d.stride * d.cur.h * 8 <= d.vram_size) {
        vbe_write(VBE_Y_OFFSET, (UINT16)d.cur.h);
        d.flip = vbe_read(VBE_Y_OFFSET) == d.cur.h;
    }
    vbe_write(VBE_X_OFFSET, 0);
    vbe_write(VBE_Y_OFFSET, 0);
}

static bool bochs_set_mode(int w, int h)
{
    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_BANK, 0);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_XRES, (UINT16)w);
    vbe_write(VBE_YRES, (UINT16)h);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
    /* Room for two pages if the memory holds them (else the adapter
     * keeps the virtual height at what fits) */
    vbe_write(VBE_VIRT_WIDTH, (UINT16)w);
    vbe_write(VBE_VIRT_HEIGHT, (UINT16)(2 * h));
    vbe_write(VBE_X_OFFSET, 0);
    vbe_write(VBE_Y_OFFSET, 0);
    bochs_read_mode();
    return d.cur.w == w && d.cur.h == h;
}

/* Put the current mode back after S3 (the adapter was reset): the
 * registers bochs_set_mode() writes, without clearing video memory, and
 * the display start on the page that was showing */
static void bochs_restore(void)
{
    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_BANK, 0);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_XRES, (UINT16)d.cur.w);
    vbe_write(VBE_YRES, (UINT16)d.cur.h);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED | VBE_NOCLEARMEM);
    vbe_write(VBE_VIRT_WIDTH, (UINT16)d.stride);
    vbe_write(VBE_VIRT_HEIGHT, (UINT16)(2 * d.cur.h));
    vbe_write(VBE_X_OFFSET, 0);
    vbe_write(VBE_Y_OFFSET, (UINT16)(d.page * d.cur.h));
    if (d.vga) vga_unblank(true);
}

/* -----------------------------------------------------------------------
 * Cirrus Logic CL-GD5446 (QEMU -vga cirrus)
 *
 * The register values are QemuVideoDxe's (OvmfPkg/QemuVideoDxe/
 * Initialize.c and Driver.c, BSD-2-Clause-Patent): standard VGA
 * timing plus the Cirrus extended sequencer (SR07: 32 bpp, SR0E/SR1E:
 * pixel clock) and CRTC registers (CR1B: the pitch's high bit).
 * ----------------------------------------------------------------------- */
typedef struct {
    int    w, h;
    UINT8  crtc[28];
    UINT16 seq[15];                       /* (value << 8) | index */
    UINT8  misc;
} CirrusMode;

static const CirrusMode g_cirrus[] = {
    { 800, 600,
      { 0x7F, 0x63, 0x64, 0x80, 0x6B, 0x1B, 0x72, 0xF0, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x58, 0x8C, 0x57, 0x90, 0x00, 0x5F, 0x91, 0xE3, 0xFF, 0x00, 0x00, 0x32 },
      { 0x0100, 0x0101, 0x0f02, 0x0003, 0x0e04, 0x1907, 0x0008, 0x4a0b,
        0x5b0c, 0x450d, 0x510e, 0x2b1b, 0x2f1c, 0x301d, 0x3a1e }, 0xEF },
    { 640, 480,
      { 0x5d, 0x4f, 0x50, 0x82, 0x53, 0x9f, 0x00, 0x3e, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xe1, 0x83, 0xdf, 0x40, 0x00, 0xe7, 0x04, 0xe3, 0xff, 0x00, 0x00, 0x32 },
      { 0x0100, 0x0101, 0x0f02, 0x0003, 0x0e04, 0x1907, 0x0008, 0x4a0b,
        0x5b0c, 0x450d, 0x7e0e, 0x2b1b, 0x2f1c, 0x301d, 0x331e }, 0xEF },
};
#define CIRRUS_MODES (int)(sizeof(g_cirrus) / sizeof(g_cirrus[0]))

static const UINT8 g_vga_attr[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00,
};
static const UINT8 g_vga_gfx[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF };

static const CirrusMode *cirrus_mode(int w, int h)
{
    for (int i = 0; i < CIRRUS_MODES; i++)
        if (g_cirrus[i].w == w && g_cirrus[i].h == h) return &g_cirrus[i];
    return NULL;
}

static void cirrus_program(const CirrusMode *m)
{
    outw(VGA_SEQ, 0x1206);                /* unlock the extended registers */
    outw(VGA_SEQ, 0x0012);                /* hardware cursor off */
    for (int i = 0; i < 15; i++) outw(VGA_SEQ, m->seq[i]);
    outb(VGA_MISC_WRITE, m->misc);
    outw(VGA_GFX, 0x0506);
    outw(VGA_SEQ, 0x0300);
    outw(VGA_CRTC, 0x2011);               /* CRTC 0-7 writable */
    for (int i = 0; i < 28; i++) outw(VGA_CRTC, (UINT16)((m->crtc[i] << 8) | i));
    for (int i = 0; i < 9; i++) outw(VGA_GFX, (UINT16)((g_vga_gfx[i] << 8) | i));
    (void)inb(VGA_STATUS);
    for (int i = 0; i < 21; i++) {
        outb(VGA_ATTR, (UINT8)i);
        outb(VGA_ATTR, g_vga_attr[i]);
    }
    outb(VGA_ATTR, 0x20);                 /* picture on */
    outw(VGA_GFX, 0x0009);                /* bank offsets: none (linear framebuffer) */
    outw(VGA_GFX, 0x000a);
    outw(VGA_GFX, 0x000b);
    outb(VGA_DAC_MASK, 0xff);
}

static bool cirrus_probe(const BootFramebuffer *boot)
{
    static const UINT16 ids[] = { 0x00B8 };     /* CL-GD5446 */
    PciDevice pci;
    if (!PciFind(0x1013, ids, 1, &pci)) return false;
    UINT64 fb = PciBarAddress(&pci, 0);
    if (!fb || fb != boot->base) {
        kprintf("[DISPLAY] Cirrus at %llx is not the boot display (%llx)\n",
                (unsigned long long)fb, (unsigned long long)boot->base);
        return false;
    }
    /* Only a boot mode we can set again (the bootloader skips the 24 bpp
     * 1024x768 one) */
    if (!cirrus_mode(d.boot.w, d.boot.h) || boot->pixels_per_scanline != (UINT32)d.boot.w) {
        kprintf("[DISPLAY] Cirrus boot mode %dx%d is not one of ours\n", d.boot.w, d.boot.h);
        return false;
    }
    d.kind = DRV_CIRRUS;
    d.name = "Cirrus Logic GD5446";
    d.mmio = false;
    d.vga = true;
    d.vram_phys = fb;
    d.vram_size = 4u << 20;
    d.bgr = true;
    d.cur = d.boot;
    d.stride = d.boot.w;
    d.flip = false;
    d.page = 0;
    kprintf("[DISPLAY] %s at %02x:%02x.%x\n", d.name, pci.bus, pci.dev, pci.func);
    return true;
}

static bool cirrus_set_mode(int w, int h)
{
    const CirrusMode *m = cirrus_mode(w, h);
    if (!m) return false;
    cirrus_program(m);
    d.cur = (DisplayMode){ w, h };
    d.stride = w;
    return true;
}

/* -----------------------------------------------------------------------
 * Further heads: QEMU's secondary-vga and bochs-display (the standard VGA's
 * IDs without the legacy VGA ports), anything but the boot display.  Their
 * DISPI registers are at BAR2 + 0x500 and their video memory is BAR0.
 * And each output of a virtio GPU that has a monitor (virtio_gpu.c): a
 * picture in memory the card shows; those come and go (DisplayPoll).
 * ----------------------------------------------------------------------- */
typedef struct {
    const char      *name;
    volatile UINT16 *regs;                /* DISPI; NULL on a virtio output */
    int              vdev, vscan;         /* the virtio GPU and its output */
    UINT64           vram_phys, vram_size;
    UINT32          *vram;
    int              max_w, max_h, stride;
    DisplayMode      modes[DISPLAY_MAX_MODES];
    int              nmodes;
    DisplayMode      cur, def;
} Head;

static Head g_head[DISPLAY_MAX_HEADS - 1];
static int  g_nheads;                     /* further heads in g_head */
static UINT64 g_boot_fb;

static bool head_set_mode(Head *h, int w, int ht, bool clear)
{
    if (!h->regs) {
        int st;
        UINT32 *px;
        if (!VgpuShow(h->vdev, h->vscan, w, ht, !clear, &px, &st)) return false;
        h->vram = px;
        h->stride = st;
        h->cur = (DisplayMode){ w, ht };
        return true;
    }
    volatile UINT16 *r = h->regs;
    r[VBE_ENABLE] = 0;
    r[VBE_BANK] = 0;
    r[VBE_BPP] = 32;
    r[VBE_XRES] = (UINT16)w;
    r[VBE_YRES] = (UINT16)ht;
    r[VBE_ENABLE] = VBE_ENABLED | VBE_LFB_ENABLED | (clear ? 0 : VBE_NOCLEARMEM);
    r[VBE_VIRT_WIDTH] = (UINT16)w;
    r[VBE_VIRT_HEIGHT] = (UINT16)ht;
    r[VBE_X_OFFSET] = 0;
    r[VBE_Y_OFFSET] = 0;
    int cw = r[VBE_XRES], ch = r[VBE_YRES], st = r[VBE_VIRT_WIDTH];
    if (cw != w || ch != ht) return false;
    h->cur = (DisplayMode){ w, ht };
    h->stride = st < w ? w : st;
    return true;
}

static void heads_probe(const BootFramebuffer *boot)
{
    static const UINT16 ids[] = { 0x1111 };
    PciDevice pci;
    for (int i = 0; g_nheads < DISPLAY_MAX_HEADS - 1 && PciFindNth(0x1234, ids, 1, i, &pci); i++) {
        UINT64 fb = PciBarAddress(&pci, 0);
        if (!fb || fb == boot->base) continue;          /* the boot display: head 0 */
        PciEnableDevice(&pci);
        volatile UINT8 *bar2 = (volatile UINT8 *)PciMapBar(&pci, 2);
        if (!bar2) continue;
        Head *h = &g_head[g_nheads];
        memset(h, 0, sizeof(*h));
        h->vdev = -1;
        h->regs = (volatile UINT16 *)(bar2 + VBE_MMIO_OFFSET);
        UINT16 id = h->regs[VBE_ID];
        if (id < VBE_ID_MIN || id > VBE_ID_MAX) {
            kprintf("[DISPLAY] Head at %02x:%02x.%x reports VBE ID %04x; not using it\n",
                    pci.bus, pci.dev, pci.func, id);
            continue;
        }
        h->name = pci.class_code == 0x03 && pci.subclass == 0x00 ? "QEMU standard VGA" : "QEMU secondary-vga";
        h->vram_phys = fb;
        h->vram_size = (UINT64)h->regs[VBE_VIDEO_MEMORY_64K] * 64 * 1024;
        if (!h->vram_size) continue;
        h->vram = (UINT32 *)PciMapPhysical(fb, h->vram_size);
        if (!h->vram) continue;
        UINT16 en = h->regs[VBE_ENABLE];
        h->regs[VBE_ENABLE] = en | VBE_GETCAPS;
        h->max_w = h->regs[VBE_XRES];
        h->max_h = h->regs[VBE_YRES];
        h->regs[VBE_ENABLE] = en;
        if (h->max_w <= 0 || h->max_h <= 0) { h->max_w = 4096; h->max_h = 4096; }
        for (unsigned m = 0; m < sizeof(g_common) / sizeof(g_common[0]); m++) {
            int w = g_common[m].w, ht = g_common[m].h;
            if (w <= h->max_w && ht <= h->max_h && (UINT64)w * ht * 4 <= h->vram_size)
                add_mode_to(h->modes, &h->nmodes, w, ht);
        }
        if (!h->nmodes) continue;
        /* Start in 1280x800 (a 100% desktop), or the largest mode below it */
        DisplayMode start = h->modes[h->nmodes - 1];
        for (int m = h->nmodes - 1; m >= 0; m--)
            if ((UINT64)h->modes[m].w * h->modes[m].h <= 1280ULL * 800) start = h->modes[m];
        if (!head_set_mode(h, start.w, start.h, true)) continue;
        h->def = h->cur;
        g_nheads++;
        kprintf("[DISPLAY] Head %d: %s at %02x:%02x.%x, %llu MB video memory, %dx%d, %d mode(s)\n",
                g_nheads, h->name, pci.bus, pci.dev, pci.func,
                (unsigned long long)(h->vram_size >> 20), h->cur.w, h->cur.h, h->nmodes);
    }
}

/* Whether output @scan of virtio GPU @dev is a head (0 when it's head 0) */
static int vhead_of(int dev, int scan)
{
    if (d.kind == DRV_VIRTIO && d.vdev == dev && scan == 0) return 0;
    for (int i = 0; i < g_nheads; i++)
        if (!g_head[i].regs && g_head[i].vdev == dev && g_head[i].vscan == scan) return i + 1;
    return -1;
}

/* A virtio GPU whose outputs may be heads: not the boot display's card
 * unless head 0 moved onto it (its other outputs would blank the VGA one) */
static bool vgpu_usable(int dev)
{
    return !VgpuIsAt(dev, g_boot_fb) || (d.kind == DRV_VIRTIO && d.vdev == dev);
}

/* Output @scan of virtio GPU @dev has a monitor that prefers @pw x @ph:
 * a new head, in that mode */
static bool vhead_add(int dev, int scan, int pw, int ph)
{
    if (g_nheads >= DISPLAY_MAX_HEADS - 1) {
        kprintf("[DISPLAY] %s output %d: no room for another monitor\n", VgpuName(dev), scan + 1);
        return false;
    }
    Head *h = &g_head[g_nheads];
    memset(h, 0, sizeof(*h));
    h->name = VgpuName(dev);
    h->vdev = dev;
    h->vscan = scan;
    h->max_w = 3840; h->max_h = 2160;
    if (pw < 640 || ph < 480 || pw > 3840 || ph > 2160 || (pw & 7)) { pw = 1280; ph = 800; }
    for (unsigned m = 0; m < sizeof(g_common) / sizeof(g_common[0]); m++)
        add_mode_to(h->modes, &h->nmodes, g_common[m].w, g_common[m].h);
    add_mode_to(h->modes, &h->nmodes, pw, ph);
    if (!head_set_mode(h, pw, ph, true)) return false;
    h->def = h->cur;
    h->vram_phys = (UINT64)(uintptr_t)h->vram - PHYSMAP_BASE;
    h->vram_size = (UINT64)h->cur.w * h->cur.h * 4;
    g_nheads++;
    kprintf("[DISPLAY] Head %d: %s output %d, %dx%d, %d mode(s)\n", g_nheads, h->name, scan + 1,
            h->cur.w, h->cur.h, h->nmodes);
    return true;
}

static void vheads_probe(void)
{
    for (int dev = 0; dev < VgpuCount(); dev++) {
        if (!vgpu_usable(dev)) continue;
        int w[VGPU_MAX_SCANOUTS], h[VGPU_MAX_SCANOUTS];
        (void)VgpuChanged(dev);                         /* what follows is the news */
        UINT32 con = VgpuConnected(dev, w, h);
        for (int s = 0; s < VgpuScanouts(dev); s++)
            if ((con & (1u << s)) && vhead_of(dev, s) < 0) vhead_add(dev, s, w[s], h[s]);
    }
}

bool DisplayPoll(UINT32 *removed)
{
    bool changed = false;
    *removed = 0;
    for (int dev = 0; dev < VgpuCount(); dev++) {
        if (!vgpu_usable(dev) || !VgpuChanged(dev)) continue;
        int w[VGPU_MAX_SCANOUTS], h[VGPU_MAX_SCANOUTS];
        UINT32 con = VgpuConnected(dev, w, h);
        /* Monitors unplugged: their heads go (the later ones move up) */
        for (int i = g_nheads - 1; i >= 0; i--) {
            Head *hd = &g_head[i];
            if (hd->regs || hd->vdev != dev || (con & (1u << hd->vscan))) continue;
            kprintf("[DISPLAY] Head %d: %s output %d unplugged\n", i + 1, hd->name, hd->vscan + 1);
            VgpuShow(dev, hd->vscan, 0, 0, false, NULL, NULL);
            for (int j = i; j < g_nheads - 1; j++) g_head[j] = g_head[j + 1];
            g_nheads--;
            *removed |= 1u << (i + 1);
            changed = true;
        }
        for (int s = 0; s < VgpuScanouts(dev); s++) {
            if (!(con & (1u << s)) || vhead_of(dev, s) >= 0) continue;
            kprintf("[DISPLAY] %s output %d: a monitor was plugged in (%dx%d)\n", VgpuName(dev), s + 1, w[s], h[s]);
            if (vhead_add(dev, s, w[s], h[s])) changed = true;
        }
    }
    return changed;
}

void DisplayHeadDamage(int head, int x, int y, int w, int ht)
{
    if (head == 0) {
        if (d.kind == DRV_VIRTIO) VgpuFlush(d.vdev, 0, x, y, w, ht);
        return;
    }
    if (head < 1 || head > g_nheads || g_head[head - 1].regs) return;
    VgpuFlush(g_head[head - 1].vdev, g_head[head - 1].vscan, x, y, w, ht);
}

static Head *head_of(int head)
{
    return head >= 1 && head <= g_nheads ? &g_head[head - 1] : NULL;
}

int DisplayHeadCount(void) { return d.kind == DRV_NONE ? 0 : 1 + g_nheads; }

const char *DisplayHeadName(int head)
{
    if (head == 0) return DisplayAdapterName() ? DisplayAdapterName() : DisplayDriverName();
    Head *h = head_of(head);
    return h ? h->name : NULL;
}

int DisplayHeadModeCount(int head)
{
    if (head == 0) return DisplayModeCount();
    Head *h = head_of(head);
    return h ? h->nmodes : 0;
}

bool DisplayHeadModeAt(int head, int i, DisplayMode *out)
{
    if (head == 0) return DisplayModeAt(i, out);
    Head *h = head_of(head);
    if (!h || i < 0 || i >= h->nmodes) return false;
    if (out) *out = h->modes[i];
    return true;
}

bool DisplayHeadModeSupported(int head, int w, int ht)
{
    if (head == 0) return DisplayModeSupported(w, ht);
    Head *h = head_of(head);
    for (int i = 0; h && i < h->nmodes; i++)
        if (h->modes[i].w == w && h->modes[i].h == ht) return true;
    return false;
}

DisplayMode DisplayHeadMode(int head)
{
    if (head == 0) return DisplayCurrentMode();
    Head *h = head_of(head);
    return h ? h->cur : (DisplayMode){ 0, 0 };
}

DisplayMode DisplayHeadDefaultMode(int head)
{
    if (head == 0) return DisplayDefaultMode();
    Head *h = head_of(head);
    return h ? h->def : (DisplayMode){ 0, 0 };
}

void DisplayHeadSetDefaultMode(int head, int w, int ht)
{
    if (head == 0) { DisplaySetDefaultMode(w, ht); return; }
    Head *h = head_of(head);
    if (h && DisplayHeadModeSupported(head, w, ht)) h->def = (DisplayMode){ w, ht };
}

bool DisplayHeadSetMode(int head, int w, int ht)
{
    if (head == 0) return DisplaySetMode(w, ht);
    Head *h = head_of(head);
    if (!h || !DisplayHeadModeSupported(head, w, ht)) return false;
    if (w == h->cur.w && ht == h->cur.h) return true;
    DisplayMode old = h->cur;
    if (!head_set_mode(h, w, ht, true)) {
        head_set_mode(h, old.w, old.h, true);
        kprintf("[DISPLAY] Head %d: %dx%d failed; back to %dx%d\n", head, w, ht, old.w, old.h);
        return false;
    }
    kprintf("[DISPLAY] Head %d: mode %dx%d\n", head, w, ht);
    return true;
}

UINT32 *DisplayHeadSurface(int head, int *stride)
{
    Head *h = head_of(head);
    if (!h) return NULL;
    if (stride) *stride = h->stride;
    return h->vram;
}

/* The boot display is a virtio-vga with more outputs: show it from a
 * picture in memory instead (output 0), so the card's other outputs can
 * be used, keeping what the screen shows */
static void virtio_takeover(void)
{
    for (int dev = 0; dev < VgpuCount(); dev++) {
        if (!VgpuIsAt(dev, d.vram_phys) || VgpuScanouts(dev) < 2) continue;
        UINT32 *px;
        int st;
        if (!VgpuShow(dev, 0, d.cur.w, d.cur.h, false, &px, &st)) return;
        const UINT32 *old = (const UINT32 *)(PHYSMAP_BASE + d.vram_phys + (UINT64)d.page * d.cur.h * d.stride * 4);
        for (int y = 0; y < d.cur.h; y++)
            memcpy(px + (size_t)y * st, old + (size_t)y * d.stride, (size_t)d.cur.w * 4);
        VgpuFlush(dev, 0, 0, 0, d.cur.w, d.cur.h);
        d.kind = DRV_VIRTIO;
        d.vdev = dev;
        d.vram_phys = (UINT64)(uintptr_t)px - PHYSMAP_BASE;
        d.stride = st;
        d.flip = false;
        d.page = 0;
        for (unsigned i = 0; i < sizeof(g_common) / sizeof(g_common[0]); i++)
            add_mode(g_common[i].w, g_common[i].h);         /* no video memory to run out of */
        publish_surface();
        kprintf("[DISPLAY] %s has %d outputs: the primary monitor is its output 1\n", VgpuName(dev), VgpuScanouts(dev));
        return;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void DisplayInit(const BootFramebuffer *boot)
{
    d.kind = DRV_NONE;
    if (!boot || !boot->base || !fb_available()) return;
    d.boot = (DisplayMode){ (int)boot->width, (int)boot->height };
    d.def = d.boot;

    if (bochs_probe(boot)) {
        bochs_read_mode();
        if (d.cur.w != d.boot.w || d.cur.h != d.boot.h) {
            /* Not the mode the GOP reported: trust the GOP, and switch to
             * it so the two agree */
            kprintf("[DISPLAY] VBE reports %dx%d, GOP %dx%d\n", d.cur.w, d.cur.h, d.boot.w, d.boot.h);
            if (!bochs_set_mode(d.boot.w, d.boot.h)) { d.kind = DRV_NONE; }
        }
    } else {
        cirrus_probe(boot);
    }
    if (d.kind == DRV_BOCHS) {
        for (unsigned i = 0; i < sizeof(g_common) / sizeof(g_common[0]); i++)
            if (fits(g_common[i].w, g_common[i].h)) add_mode(g_common[i].w, g_common[i].h);
        add_mode(d.boot.w, d.boot.h);
        publish_surface();
    } else if (d.kind == DRV_CIRRUS) {
        for (int i = 0; i < CIRRUS_MODES; i++) add_mode(g_cirrus[i].w, g_cirrus[i].h);
    } else {
        d.kind = DRV_GOP;
        d.name = NULL;
        d.cur = d.boot;
        d.flip = false;
        d.nmodes = 0;
        add_mode(d.boot.w, d.boot.h);
    }
    g_boot_fb = boot->base;
    VgpuInit();
    if (d.kind == DRV_BOCHS) virtio_takeover();
    kprintf("[DISPLAY] %s: %dx%d, %d mode(s), %s\n", DisplayDriverName(), d.cur.w, d.cur.h,
            d.nmodes, d.flip ? "page flipping" : "single page");
    heads_probe(boot);
    vheads_probe();
}

const char *DisplayDriverName(void)
{
    switch (d.kind) {
    case DRV_BOCHS:  return "Bochs VBE";
    case DRV_CIRRUS: return "Cirrus Logic";
    case DRV_VIRTIO: return "virtio GPU";
    case DRV_GOP:    return "UEFI GOP framebuffer";
    default:        return "None";
    }
}

int DisplayModeCount(void) { return d.nmodes; }

bool DisplayModeAt(int i, DisplayMode *out)
{
    if (i < 0 || i >= d.nmodes) return false;
    if (out) *out = d.modes[i];
    return true;
}

bool DisplayModeSupported(int w, int h)
{
    for (int i = 0; i < d.nmodes; i++)
        if (d.modes[i].w == w && d.modes[i].h == h) return true;
    return false;
}

DisplayMode DisplayCurrentMode(void) { return d.cur; }
DisplayMode DisplayBootMode(void)    { return d.boot; }
DisplayMode DisplayDefaultMode(void) { return d.def; }

void DisplaySetDefaultMode(int w, int h)
{
    if (DisplayModeSupported(w, h)) d.def = (DisplayMode){ w, h };
}

bool DisplaySetMode(int w, int h)
{
    if (!DisplayModeSupported(w, h)) return false;
    if (w == d.cur.w && h == d.cur.h) return true;
    if (d.kind == DRV_CIRRUS) {
        if (!cirrus_set_mode(w, h)) return false;
        publish_surface();
        kprintf("[DISPLAY] Mode %dx%d\n", w, h);
        return true;
    }
    if (d.kind == DRV_VIRTIO) {
        UINT32 *px;
        int st;
        if (!VgpuShow(d.vdev, 0, w, h, false, &px, &st)) {
            kprintf("[DISPLAY] %dx%d failed; staying at %dx%d\n", w, h, d.cur.w, d.cur.h);
            return false;
        }
        d.cur = (DisplayMode){ w, h };
        d.vram_phys = (UINT64)(uintptr_t)px - PHYSMAP_BASE;
        d.stride = st;
        publish_surface();
        kprintf("[DISPLAY] Mode %dx%d (virtio GPU output 1)\n", w, h);
        return true;
    }
    if (d.kind != DRV_BOCHS) return false;
    DisplayMode old = d.cur;
    if (!bochs_set_mode(w, h)) {
        kprintf("[DISPLAY] %dx%d failed; back to %dx%d\n", w, h, old.w, old.h);
        bochs_set_mode(old.w, old.h);
        publish_surface();
        return false;
    }
    publish_surface();
    kprintf("[DISPLAY] Mode %dx%d (stride %d), %s\n", d.cur.w, d.cur.h, d.stride,
            d.flip ? "page flipping" : "single page");
    return true;
}

bool DisplayCanFlip(void) { return d.kind == DRV_BOCHS && d.flip; }

UINT32 *DisplayBackPage(void)
{
    if (!DisplayCanFlip()) return NULL;
    UINT64 off = (UINT64)(d.page ^ 1) * d.cur.h * d.stride * 4;
    return (UINT32 *)(PHYSMAP_BASE + d.vram_phys + off);
}

void DisplayFlip(void)
{
    if (!DisplayCanFlip()) return;
    d.page ^= 1;
    vbe_write(VBE_Y_OFFSET, (UINT16)(d.page * d.cur.h));
    publish_surface();
}

const char *DisplayAdapterName(void) { return d.kind == DRV_GOP ? NULL : d.name; }

void DisplayResume(void)
{
    VgpuResume();                             /* the virtio GPUs' outputs, head 0's too */
    for (int i = 0; i < g_nheads; i++) {
        Head *h = &g_head[i];
        if (!h->regs) continue;
        head_set_mode(h, h->cur.w, h->cur.h, false);
        kprintf("[DISPLAY] Head %d: %dx%d set again after sleep\n", i + 1, h->cur.w, h->cur.h);
    }
    switch (d.kind) {
    case DRV_BOCHS:  bochs_restore(); break;
    case DRV_CIRRUS: cirrus_set_mode(d.cur.w, d.cur.h); break;
    default:         return;              /* the GOP's mode: what the firmware's wake path does */
    }
    kprintf("[DISPLAY] %s: %dx%d set again after sleep\n", d.name, d.cur.w, d.cur.h);
}
