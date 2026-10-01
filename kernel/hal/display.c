/*
 * display.c — display adapter driver (Bochs VBE "DISPI", GOP fallback)
 *
 * NovaOS boots through UEFI, so the VESA BIOS (int 10h) is gone by the
 * time the kernel runs.  The register interface behind QEMU's and
 * Bochs's VBE BIOS is still there, though: sixteen 16-bit "DISPI"
 * registers, reached through I/O ports 0x1CE/0x1CF (index/data) or, on
 * adapters with an MMIO BAR, at BAR2 + 0x500.  They set the resolution,
 * the bits per pixel, the virtual (scanline) width and the display start
 * offset, and report the video memory size, which is all a 2D driver
 * needs: modes at run time, and page flipping by moving the display
 * start between two pages of video memory.
 *
 * The adapter's linear framebuffer is PCI BAR0 — the same memory the GOP
 * handed over at boot.  If BAR0 is not the boot framebuffer (the GOP was
 * on another adapter) the driver stays out of the way.
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

typedef enum { DRV_NONE, DRV_GOP, DRV_BOCHS } DrvKind;

static struct {
    DrvKind      kind;
    bool         mmio;                    /* registers in BAR2, not I/O ports */
    volatile UINT16 *regs;
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
static bool bochs_probe(const BootFramebuffer *boot)
{
    static const UINT16 qemu_ids[] = { 0x1111 };   /* -vga std, bochs-display */
    static const UINT16 vbox_ids[] = { 0xBEEF };   /* VirtualBox VBoxVGA */
    PciDevice pci;
    if (!PciFind(0x1234, qemu_ids, 1, &pci) && !PciFind(0x80EE, vbox_ids, 1, &pci))
        return false;

    UINT64 bar0 = PciBarAddress(&pci, 0);
    if (!bar0 || bar0 != boot->base) {
        kprintf("[DISPLAY] VBE adapter at %llx is not the boot display (%llx)\n",
                (unsigned long long)bar0, (unsigned long long)boot->base);
        return false;
    }
    UINT64 bar2 = PciBarAddress(&pci, 2);
    d.mmio = bar2 != 0 && pci.vendor == 0x1234;
    d.regs = d.mmio ? (volatile UINT16 *)(PHYSMAP_BASE + bar2 + VBE_MMIO_OFFSET) : NULL;
    bool is_vga = pci.class_code == 0x03 && pci.subclass == 0x00;

    UINT16 id = vbe_read(VBE_ID);
    if (id < VBE_ID_MIN || id > VBE_ID_MAX) {
        kprintf("[DISPLAY] VBE adapter reports ID %04x; not using it\n", id);
        return false;
    }

    d.vram_phys = bar0;
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
    if (is_vga && d.mmio)                  /* unblank (attribute controller) */
        *(volatile UINT8 *)(PHYSMAP_BASE + bar2 + VGA_MMIO_OFFSET) = 0x20;

    kprintf("[DISPLAY] Bochs VBE %04x (%04x:%04x) at %02x:%02x.%x, %llu MB video memory, "
            "max %dx%d, registers via %s\n", id, pci.vendor, pci.device,
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
    }
    if (d.kind == DRV_BOCHS) {
        for (unsigned i = 0; i < sizeof(g_common) / sizeof(g_common[0]); i++)
            if (fits(g_common[i].w, g_common[i].h)) add_mode(g_common[i].w, g_common[i].h);
        add_mode(d.boot.w, d.boot.h);
        publish_surface();
    } else {
        d.kind = DRV_GOP;
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
    case DRV_BOCHS: return "Bochs VBE";
    case DRV_GOP:   return "UEFI GOP framebuffer";
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
