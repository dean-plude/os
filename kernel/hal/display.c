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
#include "../arch/x86_64/cpu.h"
#include "../ke/printf.h"

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

typedef enum { DRV_NONE, DRV_GOP, DRV_BOCHS, DRV_CIRRUS } DrvKind;

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

static void add_mode(int w, int h)
{
    if (d.nmodes >= DISPLAY_MAX_MODES) return;
    for (int i = 0; i < d.nmodes; i++)
        if (d.modes[i].w == w && d.modes[i].h == h) return;
    int j = d.nmodes++;                   /* keep largest (by area) first */
    while (j > 0 && (UINT64)d.modes[j - 1].w * d.modes[j - 1].h < (UINT64)w * h) {
        d.modes[j] = d.modes[j - 1];
        j--;
    }
    d.modes[j] = (DisplayMode){ w, h };
}

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
    kprintf("[DISPLAY] %s: %dx%d, %d mode(s), %s\n", DisplayDriverName(), d.cur.w, d.cur.h,
            d.nmodes, d.flip ? "page flipping" : "single page");
}

const char *DisplayDriverName(void)
{
    switch (d.kind) {
    case DRV_BOCHS:  return "Bochs VBE";
    case DRV_CIRRUS: return "Cirrus Logic";
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
    switch (d.kind) {
    case DRV_BOCHS:  bochs_restore(); break;
    case DRV_CIRRUS: cirrus_set_mode(d.cur.w, d.cur.h); break;
    default:         return;              /* the GOP's mode: what the firmware's wake path does */
    }
    kprintf("[DISPLAY] %s: %dx%d set again after sleep\n", d.name, d.cur.w, d.cur.h);
}
