/*
 * i2chid.c — HID over I2C: laptop touchpads
 *
 * Touchpads in laptops since about 2015 (the ThinkPad T14 Gen 4's among
 * them) are HID devices on an I2C bus rather than PS/2 mice.  The ACPI
 * namespace lists them (PNP0C50, aml.c): the I2C address, the controller
 * (an Intel LPSS I2C function, i2c_dw.c) and the register that holds the
 * device's HID descriptor.  The protocol is Microsoft's "HID over I2C
 * Protocol Specification" v1.0:
 *
 *   - the HID descriptor (30 bytes) names the device's registers: report
 *     descriptor, input, command, data;
 *   - SET_POWER(ON), then RESET, which the device acknowledges with an
 *     empty input report (length 0);
 *   - the report descriptor goes to usbhid.c's parser (HidAttach);
 *   - input reports are read from the input register: two length bytes,
 *     then the report.
 *
 * The device holds an interrupt line low while it has a report: a GPIO
 * pin (its GpioInt resource, handed to the chipset's GPIO controller
 * driver, hal/gpio.c) or, on some firmware, an I/O APIC input of its own
 * (an Interrupt resource).  The interrupt wakes the "i2chid" thread, which
 * reads reports while the line stays asserted and then unmasks it, as
 * Linux's level-triggered flow does; once a second it also looks without
 * one, and a device whose reports keep turning up that way (an interrupt
 * that never comes) is polled from then on.  A device without an interrupt
 * NovaOS can take is polled, as FreeBSD's iichid does without one: every
 * 10 ms while reports come, every 50 ms after half a second without one; a
 * device with nothing to say answers with a length of 0 or 2.
 *
 * Touchpads (Windows precision touchpads) start in mouse mode: relative
 * motion and buttons from their mouse collection.  As Windows does,
 * NovaOS switches them to touchpad mode (SET_REPORT of the Input Mode
 * feature) so that they report their fingers, and usbhid.c makes tap to
 * click, two-finger tap for the right button and two-finger scrolling
 * from those; a touchpad that refuses stays in mouse mode.  A reset (after
 * sleep) puts the device back in mouse mode, so the switch is repeated.
 *
 * I2cHidSelfCheck() runs the protocol against a modelled touchpad on a
 * modelled bus: QEMU has neither an LPSS I2C controller nor an I2C-HID
 * device.  With the self-tests' ACPI table it also drives the modelled
 * touchpad's interrupt line through the modelled GPIO controller there.
 */

#include "i2chid.h"
#include "i2c.h"
#include "hid.h"
#include "../hal/aml.h"
#include "../hal/gpio.h"
#include "../hal/ioapic.h"
#include "../arch/x86_64/idt.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../arch/x86_64/cpu.h"

#define OP_RESET      0x01
#define OP_SET_REPORT 0x03
#define OP_SET_POWER  0x08
#define POWER_ON      0x00
#define POWER_SLEEP   0x01

#define MAX_DEVS      AML_MAX_I2C_HID
#define MAX_REPORT    512
#define MAX_RDESC     4096

typedef struct {
    I2cBus *bus;
    UINT16  addr;
    UINT16  desc_reg;
    UINT16  rdesc_len, rdesc_reg, input_reg, max_input, cmd_reg, data_reg;
    bool    ptp;                         /* switched to touchpad mode */
    UINT16  vendor, product;
    void   *hid;
    char    name[64];
    GpioIrq *gpio;                       /* its interrupt: a GPIO pin, */
    bool    apic, irq_level;             /*   or an I/O APIC input of its own */
    UINT32  gsi;
    bool    polled;                      /* no interrupt (or it never came) */
    volatile UINT32 pending;             /* the interrupt came, not serviced yet */
    volatile UINT32 irqs;
    volatile UINT32 *kick;               /* set and woken by the interrupt */
    Thread *waiter;
    UINT32  missed;                      /* reports the once-a-second look found */
    UINT64  next_look;
    UINT64 (*stamp)(void);               /* the self-check's clock for its reports */
    UINT8   buf[MAX_REPORT];
} I2cHid;

static I2cHid *g_devs[MAX_DEVS];
static int     g_ndevs;
static Thread *g_thread;
static volatile UINT32 g_pause, g_in_poll, g_reinit, g_kick;

static UINT16 le16(const UINT8 *p) { return (UINT16)(p[0] | p[1] << 8); }

static bool read_reg(I2cHid *d, UINT16 reg, UINT8 *out, int n)
{
    UINT8 w[2] = { (UINT8)reg, (UINT8)(reg >> 8) };
    return d->bus->xfer(d->bus, d->addr, w, 2, out, n);
}

static bool command(I2cHid *d, UINT8 op, UINT8 arg)
{
    UINT8 w[4] = { (UINT8)d->cmd_reg, (UINT8)(d->cmd_reg >> 8), arg, op };
    return d->bus->xfer(d->bus, d->addr, w, 4, NULL, 0);
}

static void wait_ms(int ms)
{
    if (!(read_rflags() & 0x200)) { for (volatile int i = 0; i < ms * 20000; i++) { } return; }
    UINT64 end = sched_ticks() + (UINT64)(ms + 9) / 10;
    while (sched_ticks() < end) sched_sleep_until(NULL, end);
}

/* SET_REPORT of feature report @id: the command, then on the data
 * register the length (2 + @n) and @rep (with its ID byte if it has one) */
static bool set_feature(I2cHid *d, UINT8 id, const UINT8 *rep, int n)
{
    UINT8 w[48];
    int at = 0;
    if (n > 32) return false;
    w[at++] = (UINT8)d->cmd_reg; w[at++] = (UINT8)(d->cmd_reg >> 8);
    w[at++] = (UINT8)(0x30 | (id < 15 ? id : 15));               /* report type 3: feature */
    w[at++] = OP_SET_REPORT;
    if (id >= 15) w[at++] = id;
    w[at++] = (UINT8)d->data_reg; w[at++] = (UINT8)(d->data_reg >> 8);
    w[at++] = (UINT8)(n + 2); w[at++] = 0;
    memcpy(w + at, rep, (size_t)n);
    return d->bus->xfer(d->bus, d->addr, w, at + n, NULL, 0);
}

/* A precision touchpad: switch it to touchpad mode (taps, two-finger
 * scrolling); false when it isn't one or refused */
static bool touchpad_mode(I2cHid *d)
{
    UINT8 rep[32], id = 0;
    int n = HidTouchpadModeReport(d->hid, &id, rep, sizeof(rep));
    d->ptp = n && set_feature(d, id, rep, n);
    HidTouchpadMode(d->hid, d->ptp);
    return d->ptp;
}

/* One input report: its length (0: none), the report at d->buf + 2 */
static int read_input(I2cHid *d)
{
    int n = d->max_input < MAX_REPORT ? d->max_input : MAX_REPORT;
    if (n < 2 || !d->bus->xfer(d->bus, d->addr, NULL, 0, d->buf, n)) return 0;
    int len = le16(d->buf);
    return len <= 2 || len > n ? 0 : len - 2;
}

static void deliver(I2cHid *d, int len)
{
    if (d->stamp) HidInputAt(d->hid, d->buf + 2, len, d->stamp());
    else HidInput(d->hid, d->buf + 2, len);
}

/* The interrupt (kernel lock held): a level-triggered line stays masked
 * until the thread has read the reports */
static void irq_fired(void *ctx)
{
    I2cHid *d = ctx;
    if (d->apic && d->irq_level) IoApicMask(d->gsi, true);
    __atomic_fetch_add(&d->irqs, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&d->pending, 1, __ATOMIC_RELEASE);
    if (d->kick) __atomic_store_n(d->kick, 1, __ATOMIC_RELEASE);
    if (d->waiter) sched_unblock(d->waiter);
}

static void irq_unmask(I2cHid *d)
{
    if (d->gpio) GpioIrqUnmask(d->gpio);
    else if (d->apic && d->irq_level) { bkl_acquire(); IoApicMask(d->gsi, false); bkl_release(); }
}

/* After an interrupt: the reports, while the line stays asserted (a GPIO
 * pin says), else until the device has none; the line unmasked after */
static int service(I2cHid *d)
{
    int n = 0;
    for (int i = 0; i < 32; i++) {
        int len = read_input(d);
        if (!len) break;
        deliver(d, len);
        n++;
        if (d->gpio && !GpioIrqAsserted(d->gpio)) break;
    }
    irq_unmask(d);
    return n;
}

static void irq_disconnect(I2cHid *d)
{
    if (d->gpio) GpioIrqDisconnect(d->gpio);
    else if (d->apic) { bkl_acquire(); IoApicMask(d->gsi, true); bkl_release(); }
    d->gpio = NULL;
    d->apic = false;
    d->polled = true;
}

/* Take the device's interrupt from its ACPI resources; @how says what
 * came of it */
static void irq_connect(I2cHid *d, const AmlI2cHid *a, char *how, int cap)
{
    char why[96];
    d->polled = true;
    d->next_look = sched_ticks() + 100;
    if (a->gpio_int) {
        d->gpio = GpioIrqConnect(a->gpio_ctrl, a->gpio_pin, a->gpio_level, a->gpio_low, a->gpio_both,
                                 irq_fired, d, why, sizeof(why));
        if (d->gpio) {
            d->polled = false;
            ksnprintf(how, cap, "interrupt on GPIO pin %u (%s)", a->gpio_pin, GpioIrqName(d->gpio));
        } else {
            ksnprintf(how, cap, "polled (its GPIO pin %u: %s)", a->gpio_pin, why);
        }
        return;
    }
    if (a->irq) {
        d->irq_level = a->irq_level;
        if (IoApicPresent() && IrqInstall(IRQ_I2CHID, irq_fired, d)) {
            if (IoApicRouteIrq(a->irq_num, a->irq_level, a->irq_low, IRQ_I2CHID, &d->gsi)) {
                d->apic = true;
                d->polled = false;
                ksnprintf(how, cap, "interrupt on GSI %u", d->gsi);
                return;
            }
            IrqInstall(IRQ_I2CHID, NULL, NULL);
        }
        ksnprintf(how, cap, "polled (its interrupt %u could not be routed)", a->irq_num);
        return;
    }
    ksnprintf(how, cap, "polled (no interrupt resource)");
}

/* Power on and reset; true when the device acknowledged the reset (an
 * empty report) within a second */
static bool power_and_reset(I2cHid *d)
{
    if (!command(d, OP_SET_POWER, POWER_ON)) return false;
    wait_ms(1);
    if (!command(d, OP_RESET, 0)) return false;
    for (int i = 0; i < 50; i++) {
        wait_ms(20);
        UINT8 ack[2] = { 0xAA, 0xAA };
        if (d->bus->xfer(d->bus, d->addr, NULL, 0, ack, 2) && ack[0] == 0 && ack[1] == 0) return true;
    }
    return false;
}

/* Read the HID descriptor, reset the device and read its report
 * descriptor into the HID parser; @kind says what it turned out to be */
static bool attach(I2cHid *d, const char **kind, char *why, int whycap)
{
    UINT8 desc[30];
    if (!read_reg(d, d->desc_reg, desc, sizeof(desc))) {
        ksnprintf(why, whycap, "no answer at address 0x%02x", d->addr);
        return false;
    }
    if (le16(desc) != 30 || le16(desc + 2) != 0x0100) {
        ksnprintf(why, whycap, "not an HID descriptor (length %u, version %04x)", le16(desc), le16(desc + 2));
        return false;
    }
    d->rdesc_len = le16(desc + 4);
    d->rdesc_reg = le16(desc + 6);
    d->input_reg = le16(desc + 8);
    d->max_input = le16(desc + 10);
    d->cmd_reg   = le16(desc + 16);
    d->data_reg  = le16(desc + 18);
    d->vendor    = le16(desc + 20);
    d->product   = le16(desc + 22);
    if (!d->rdesc_len || d->rdesc_len > MAX_RDESC) {
        ksnprintf(why, whycap, "report descriptor of %u bytes", d->rdesc_len);
        return false;
    }
    bool acked = power_and_reset(d);
    UINT8 *rdesc = kmalloc(d->rdesc_len);
    if (!rdesc) { ksnprintf(why, whycap, "out of memory"); return false; }
    bool ok = read_reg(d, d->rdesc_reg, rdesc, d->rdesc_len);
    d->hid = ok ? HidAttach(rdesc, d->rdesc_len, kind) : NULL;
    kfree(rdesc);
    if (!ok) { ksnprintf(why, whycap, "report descriptor unreadable"); return false; }
    if (!d->hid) { ksnprintf(why, whycap, "nothing NovaOS uses in its reports"); return false; }
    if (!acked) kprintf("[I2C] %s: no reset acknowledge (carrying on)\n", d->name);
    if (touchpad_mode(d)) *kind = "precision touchpad (tap to click, two-finger scrolling)";
    return true;
}

static void reinit_all(void)
{
    DwI2cResume();
    for (int i = 0; i < g_ndevs; i++) {
        I2cHid *d = g_devs[i];
        if (!power_and_reset(d)) kprintf("[I2C] %s: no reset acknowledge after sleep\n", d->name);
        if (d->ptp && !touchpad_mode(d)) kprintf("[I2C] %s: back in mouse mode after sleep\n", d->name);
    }
}

static void i2chid_thread(void *arg)
{
    (void)arg;
    g_thread = sched_current();
    bkl_release();                       /* (transfers busy-wait on the bus) */
    for (int i = 0; i < 3000 && !AmlReady(); i++) sched_sleep_until(NULL, sched_ticks() + 1);
    AmlI2cHid found[AML_MAX_I2C_HID];
    int n = AmlI2cHidDevices(found, AML_MAX_I2C_HID);
    for (int i = 0; i < n; i++) {
        AmlI2cHid *a = &found[i];
        if (!a->bus_pci) {
            kprintf("[I2C] %s (%s): its controller %s is not a PCI function\n", a->path, a->hid, a->bus);
            continue;
        }
        I2cBus *bus = DwI2cOpen(a->bus_dev, a->bus_fn, a->speed, a->has_fmcn ? a->fmcn : NULL,
                                a->has_sscn ? a->sscn : NULL);
        if (!bus) {
            kprintf("[I2C] %s (%s): no I2C controller at PCI 00:%02x.%x\n", a->path, a->hid, a->bus_dev, a->bus_fn);
            continue;
        }
        I2cHid *d = kzalloc(sizeof(I2cHid));
        if (!d) break;
        d->bus = bus;
        d->addr = a->addr;
        d->desc_reg = a->has_desc ? a->desc_reg : 1;
        ksnprintf(d->name, sizeof(d->name), "%s (%s)", a->hid, a->path);
        const char *kind = "?";
        char why[80];
        if (!attach(d, &kind, why, sizeof(why))) {
            kprintf("[I2C] %s: %s\n", d->name, why);
            kfree(d);
            continue;
        }
        HidStart(d->hid);
        d->kick = &g_kick;
        d->waiter = g_thread;
        g_devs[g_ndevs++] = d;
        char how[128];
        irq_connect(d, a, how, sizeof(how));
        kprintf("[I2C] %s %04x:%04x on %s: %s, %s\n", d->name, d->vendor, d->product, bus->name, kind, how);
    }
    if (!g_ndevs) sched_exit_current();

    int idle = 0;
    for (;;) {
        bool polled = false, pending = false;
        __atomic_store_n(&g_kick, 0, __ATOMIC_SEQ_CST);
        for (int i = 0; i < g_ndevs; i++) {
            polled |= g_devs[i]->polled;
            pending |= __atomic_load_n(&g_devs[i]->pending, __ATOMIC_ACQUIRE) != 0;
        }
        if (!pending || __atomic_load_n(&g_pause, __ATOMIC_ACQUIRE)) sched_sleep_until(&g_kick, sched_ticks() + (!polled ? 100 : idle >= 50 ? 5 : 1));
        if (__atomic_load_n(&g_pause, __ATOMIC_ACQUIRE)) continue;
        __atomic_store_n(&g_in_poll, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_pause, __ATOMIC_ACQUIRE)) { __atomic_store_n(&g_in_poll, 0, __ATOMIC_RELEASE); continue; }
        if (__atomic_exchange_n(&g_reinit, 0, __ATOMIC_ACQ_REL)) reinit_all();
        bool any = false;
        for (int i = 0; i < g_ndevs; i++) {
            I2cHid *d = g_devs[i];
            int len;
            if (d->polled) {
                if ((len = read_input(d))) { deliver(d, len); any = true; }
            } else if (__atomic_exchange_n(&d->pending, 0, __ATOMIC_ACQ_REL)) {
                any |= service(d) > 0;
                d->missed = 0;
                d->next_look = sched_ticks() + 100;
            } else if (sched_ticks() >= d->next_look) {
                /* a second without an interrupt: the device should have
                 * nothing; a report here means the interrupt didn't come */
                d->next_look = sched_ticks() + 100;
                if ((len = read_input(d))) {
                    deliver(d, len);
                    any = true;
                    if (!__atomic_load_n(&d->pending, __ATOMIC_ACQUIRE) && ++d->missed == 3) {
                        kprintf("[I2C] %s: reports without its interrupt (%s, %u interrupts): polled from now on\n",
                                d->name, d->gpio ? GpioIrqName(d->gpio) : "I/O APIC", d->irqs);
                        irq_disconnect(d);
                    }
                }
            }
        }
        idle = any ? 0 : idle + 1;
        __atomic_store_n(&g_in_poll, 0, __ATOMIC_RELEASE);
    }
}

void I2cHidInit(void)
{
    sched_create_thread("i2chid", i2chid_thread, NULL, PRIO_DEVICE_IO);   /* (above programs: scheduler.h) */
}

void I2cHidPrepareSleep(void)
{
    __atomic_store_n(&g_pause, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 100000 && __atomic_load_n(&g_in_poll, __ATOMIC_ACQUIRE); i++) pause_cpu();
}

void I2cHidResume(void)
{
    if (g_ndevs) __atomic_store_n(&g_reinit, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pause, 0, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * I2cHidSelfCheck: a modelled precision touchpad on a modelled bus
 *
 * At address 0x2C, HID descriptor at register 0x0020 (Synaptics-style
 * registers 0x21-0x25, vendor 06CB), a report descriptor with a Touch Pad
 * collection (one finger, contact count, the click button; report 1), a
 * mouse collection (two buttons, relative X/Y; report 2) and the
 * configuration collection's Input Mode feature with the surface and
 * button switches (report 3).  It obeys SET_POWER, RESET and SET_REPORT
 * (or refuses SET_REPORT, as a touchpad without touchpad mode would) and
 * hands out the input reports queued for it.
 * ----------------------------------------------------------------------- */
static const UINT8 g_tp_rdesc[] = {
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,                           /* Touch Pad, report 1 */
    0x09, 0x22, 0xA1, 0x02,                                                   /*   finger */
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   /*     tip switch */
    0x09, 0x47, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03,                           /*     confidence, padding */
    0x09, 0x51, 0x25, 0x0F, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02, 0x81, 0x03,   /*     contact identifier, padding */
    0x05, 0x01, 0x26, 0xE4, 0x04, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x11,         /*     X 0-1252, Y 0-694 (cm, 10^-2) */
    0x35, 0x00, 0x46, 0x4E, 0x04, 0x09, 0x30, 0x81, 0x02,
    0x26, 0xB6, 0x02, 0x46, 0xE0, 0x02, 0x09, 0x31, 0x81, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x54, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   /*   contact count */
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,   /* button 1 */
    0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,   /* Mouse, report 2 */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0,
    0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xA1, 0x02,   /* Configuration, report 3 */
    0x09, 0x52, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,   /*   Input Mode (feature) */
    0x09, 0x57, 0x09, 0x58, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0xB1, 0x02,   /*   Surface, Button Switch */
    0x95, 0x06, 0xB1, 0x03,
    0xC0, 0xC0,
};

#define TP_QUEUE 16

static struct {
    bool   powered, reset_ack, power_cmd, reset_cmd;
    bool   refuse_mode;                  /* SET_REPORT is not acknowledged */
    int    mode, switches;               /* what SET_REPORT set (-1: nothing) */
    int    next, count, empties;
    UINT8  rep[TP_QUEUE][12];
    int    len[TP_QUEUE];
    UINT64 at[TP_QUEUE];                 /* when each report comes (ticks) */
    GpioIrq *irq;                        /* its interrupt line: asserted while it has reports */
} g_tp;

static void tp_line(void) { if (g_tp.irq) GpioModelSetLine(g_tp.irq, g_tp.next < g_tp.count); }
static UINT64 tp_stamp(void) { return g_tp.at[g_tp.next ? g_tp.next - 1 : 0]; }

static bool tp_xfer(I2cBus *bus, UINT16 addr, const UINT8 *w, int wn, UINT8 *r, int rn)
{
    (void)bus;
    if (addr != 0x2C) return false;                                   /* (nobody there: no acknowledge) */
    UINT16 reg = wn >= 2 ? (UINT16)(w[0] | w[1] << 8) : 0;
    if (wn >= 4 && reg == 0x0024) {                                   /* a command */
        if (w[3] == OP_SET_POWER) { g_tp.powered = !(w[2] & 1); g_tp.power_cmd = true; }
        if (w[3] == OP_RESET && g_tp.powered) { g_tp.reset_ack = true; g_tp.reset_cmd = true; g_tp.mode = 0; }
        if (w[3] == OP_SET_REPORT) {                                  /* feature 3: length 5, ID, mode, switches */
            if (g_tp.refuse_mode) return false;
            if (wn != 11 || w[2] != 0x33 || w[4] != 0x25 || w[5] != 0 || w[6] != 5 || w[7] != 0 || w[8] != 3) return false;
            g_tp.mode = w[9];
            g_tp.switches = w[10];
        }
        return true;
    }
    memset(r, 0, (size_t)rn);
    if (wn == 2 && reg == 0x0020) {                                   /* the HID descriptor */
        static const UINT8 desc[30] = { 30, 0, 0x00, 0x01, sizeof(g_tp_rdesc) & 0xFF, sizeof(g_tp_rdesc) >> 8,
                                        0x21, 0, 0x22, 0, 12, 0, 0x23, 0, 4, 0, 0x24, 0, 0x25, 0,
                                        0xCB, 0x06, 0x67, 0xCE, 0x01, 0x00 };
        memcpy(r, desc, rn < 30 ? (size_t)rn : 30);
        return true;
    }
    if (wn == 2 && reg == 0x0021) {                                   /* the report descriptor */
        memcpy(r, g_tp_rdesc, rn < (int)sizeof(g_tp_rdesc) ? (size_t)rn : sizeof(g_tp_rdesc));
        return true;
    }
    if (wn == 0 && rn >= 2) {                                         /* the input register */
        if (!g_tp.powered) return false;
        if (g_tp.reset_ack) { g_tp.reset_ack = false; return true; }  /* 00 00 */
        if (g_tp.next == g_tp.count) { g_tp.empties++; return true; } /* nothing to say */
        int k = g_tp.next++, n = g_tp.len[k] + 2;
        r[0] = (UINT8)n; r[1] = 0;
        memcpy(r + 2, g_tp.rep[k], (size_t)(n - 2 < rn - 2 ? n - 2 : rn - 2));
        tp_line();                                                    /* (let go after the last) */
        return true;
    }
    return false;
}

/* A gesture for the modelled touchpad in touchpad mode: each step is one
 * finger report (one finger a report: hybrid mode) */
typedef struct { UINT16 tick; UINT8 tip, conf, cid; UINT16 x, y; UINT8 count, button; } TpStep;

/* Feed @n steps to @d; what came out, as text: "m<buttons>,<dx>,<dy>",
 * with "/z<notches>" or "/w<notches>" for the wheels.  With an interrupt
 * (d->gpio) the touchpad asserts its line and the reports are read as
 * the "i2chid" thread reads them, when the interrupt has come */
static void tp_feed(I2cHid *d, const TpStep *st, int n, char *got, int cap)
{
    if (n > TP_QUEUE) n = TP_QUEUE;
    for (int i = 0; i < n; i++) {
        UINT8 *r = g_tp.rep[i];
        r[0] = 1;
        r[1] = (UINT8)(st[i].tip | st[i].conf << 1);
        r[2] = st[i].cid;
        r[3] = (UINT8)st[i].x; r[4] = (UINT8)(st[i].x >> 8);
        r[5] = (UINT8)st[i].y; r[6] = (UINT8)(st[i].y >> 8);
        r[7] = st[i].count;
        r[8] = st[i].button;
        g_tp.len[i] = 9;
        g_tp.at[i] = st[i].tick;
    }
    g_tp.next = 0;
    g_tp.count = n;
    InputEvent ev[16];
    HidCaptureBegin(d->hid);
    if (d->gpio) {
        tp_line();
        for (int t = 0; t < 100 && (g_tp.next < g_tp.count || d->pending); t++) {
            *d->kick = 0;
            if (!d->pending) sched_sleep_until(d->kick, sched_ticks() + 1);
            if (__atomic_exchange_n(&d->pending, 0, __ATOMIC_ACQ_REL)) service(d);
        }
    } else {
        for (int i = 0; i < n + 1; i++) {
            int len = read_input(d);
            if (len) HidInputAt(d->hid, d->buf + 2, len, g_tp.at[g_tp.next - 1]);
        }
    }
    int k = HidCaptureEnd(ev, 16), at = 0;
    got[0] = 0;
    for (int i = 0; i < k && at < cap - 24; i++) {
        at += ksnprintf(got + at, cap - at, "%sm%X,%d,%d", i ? " " : "", ev[i].buttons, ev[i].dx, ev[i].dy);
        if (ev[i].dz) at += ksnprintf(got + at, cap - at, "/z%d", ev[i].dz);
        if (ev[i].dw) at += ksnprintf(got + at, cap - at, "/w%d", ev[i].dw);
    }
}

int I2cHidSelfCheck(void (*say)(void *ctx, const char *line), void *ctx)
{
    int failed = 0;
    char line[176];
#define CHECK(ok, ...) do { bool ok_ = (ok); char t_[150]; ksnprintf(t_, sizeof(t_), __VA_ARGS__); \
                            ksnprintf(line, sizeof(line), "%s %s", ok_ ? "ok  " : "FAIL", t_); \
                            say(ctx, line); if (!ok_) failed++; } while (0)
    memset(&g_tp, 0, sizeof(g_tp));
    g_tp.refuse_mode = true;                  /* first a touchpad that stays in mouse mode */
    I2cBus bus = { "modelled bus", tp_xfer, NULL };
    I2cHid *d = kzalloc(sizeof(I2cHid));
    if (!d) return -1;
    d->bus = &bus;
    d->addr = 0x2C;
    d->desc_reg = 0x0020;
    strncpy(d->name, "modelled touchpad", sizeof(d->name) - 1);
    const char *kind = "nothing";
    char why[80] = "";
    bool ok = attach(d, &kind, why, sizeof(why));
    CHECK(ok && d->vendor == 0x06CB && d->product == 0xCE67 && d->input_reg == 0x22 && d->cmd_reg == 0x24,
          "touchpad: HID descriptor read (06cb:ce67, input register 0x%04x, command register 0x%04x)%s%s",
          d->input_reg, d->cmd_reg, ok ? "" : ": ", why);
    CHECK(g_tp.power_cmd && g_tp.reset_cmd && g_tp.powered && !g_tp.reset_ack,
          "touchpad: powered on, reset and its acknowledge read");
    CHECK(ok && strcmp(kind, "touchpad (mouse mode)") == 0 && !d->ptp,
          "touchpad: report descriptor read as %s (touchpad mode refused)", kind);
    if (!ok) { kfree(d); return failed; }

    /* a finger down (its touchpad report is left alone), then the mouse
     * reports: moved 5 right and 3 up, the click, released; then nothing */
    static const UINT8 reps[4][12] = {
        { 1, 0x03, 0x00, 0x58, 0x02, 0x2C, 0x01, 1, 0 },
        { 2, 0x00, 5, 0xFD },
        { 2, 0x01, 0, 0 },
        { 2, 0x00, 0, 0 },
    };
    static const int lens[4] = { 9, 4, 4, 4 };
    memcpy(g_tp.rep, reps, sizeof(reps));
    memcpy(g_tp.len, lens, sizeof(lens));
    g_tp.count = 4;
    InputEvent ev[16];
    HidCaptureBegin(d->hid);
    int reports = 0;
    for (int i = 0; i < 6; i++) {
        int len = read_input(d);
        if (len) { HidInput(d->hid, d->buf + 2, len); reports++; }
    }
    int n = HidCaptureEnd(ev, 16);
    char got[96] = "";
    int at = 0;
    for (int i = 0; i < n && at < (int)sizeof(got) - 16; i++)
        at += ksnprintf(got + at, sizeof(got) - at, "%sm%X,%d,%d", i ? " " : "", ev[i].buttons, ev[i].dx, ev[i].dy);
    CHECK(reports == 4 && g_tp.empties == 2, "touchpad: %d reports read, %d empty answers ignored", reports, g_tp.empties);
    CHECK(strcmp(got, "m0,5,-3 m1,0,0 m0,0,0") == 0,
          "touchpad: finger report ignored, mouse reports move and click: %s", got[0] ? got : "(no events)");

    /* nobody at another address */
    I2cHid *e = kzalloc(sizeof(I2cHid));
    if (e) {
        e->bus = &bus;
        e->addr = 0x15;
        e->desc_reg = 0x0001;
        strncpy(e->name, "absent", sizeof(e->name) - 1);
        bool found = attach(e, &kind, why, sizeof(why));
        CHECK(!found, "touchpad: a device that doesn't answer is not taken (%s)", found ? "taken" : why);
        kfree(e);
    }
    /* (d->hid stays allocated: it was never started) */
    kfree(d);

    /* The same touchpad taking touchpad mode: its fingers make the
     * gestures.  The pad is 110.2 x 73.6 mm (X 0-1252, Y 0-694), so a
     * count is 88 um across and 106 um down */
    memset(&g_tp, 0, sizeof(g_tp));
    g_tp.mode = g_tp.switches = -1;
    d = kzalloc(sizeof(I2cHid));
    if (!d) return failed + 1;
    d->bus = &bus;
    d->addr = 0x2C;
    d->desc_reg = 0x0020;
    strncpy(d->name, "modelled touchpad", sizeof(d->name) - 1);
    ok = attach(d, &kind, why, sizeof(why));
    CHECK(ok && d->ptp && g_tp.mode == 3 && g_tp.switches == 3,
          "touchpad: switched to touchpad mode (Input Mode %d, switches %d): %s", g_tp.mode, g_tp.switches, ok ? kind : why);
    if (!ok || !d->ptp) { kfree(d); return failed; }

    static const struct { const char *what, *want; int n; TpStep st[8]; } gestures[] = {
        { "one-finger tap: left click", "m1,0,0 m0,0,0", 2,
          { { 1000, 1, 1, 0, 600, 300, 1, 0 }, { 1005, 0, 1, 0, 600, 300, 1, 0 } } },
        { "one finger moves 10 mm right, 3.6 mm down: the pointer follows, no click", "m0,62,0 m0,63,45", 4,
          { { 1100, 1, 1, 0, 600, 300, 1, 0 }, { 1101, 1, 1, 0, 657, 300, 1, 0 },
            { 1102, 1, 1, 0, 714, 334, 1, 0 }, { 1103, 0, 1, 0, 714, 334, 1, 0 } } },
        { "a touch held longer than 180 ms: no click", "", 2,
          { { 1150, 1, 1, 0, 600, 300, 1, 0 }, { 1175, 0, 1, 0, 600, 300, 1, 0 } } },
        { "two-finger tap: right click", "m2,0,0 m0,0,0", 4,
          { { 1200, 1, 1, 0, 500, 300, 2, 0 }, { 1200, 1, 1, 1, 800, 320, 0, 0 },
            { 1206, 0, 1, 0, 500, 300, 2, 0 }, { 1206, 0, 1, 1, 800, 320, 0, 0 } } },
        { "two fingers 6 mm down: the wheel 2 notches up", "m0,0,0/z1 m0,0,0/z1", 8,
          { { 1300, 1, 1, 0, 500, 200, 2, 0 }, { 1300, 1, 1, 1, 800, 200, 0, 0 },
            { 1301, 1, 1, 0, 500, 230, 2, 0 }, { 1301, 1, 1, 1, 800, 230, 0, 0 },
            { 1302, 1, 1, 0, 500, 260, 2, 0 }, { 1302, 1, 1, 1, 800, 260, 0, 0 },
            { 1303, 0, 1, 0, 500, 260, 2, 0 }, { 1303, 0, 1, 1, 800, 260, 0, 0 } } },
        { "two fingers 7 mm left: the horizontal wheel 2 notches right", "m0,0,0/w1 m0,0,0/w1", 8,
          { { 1400, 1, 1, 0, 500, 300, 2, 0 }, { 1400, 1, 1, 1, 800, 300, 0, 0 },
            { 1401, 1, 1, 0, 460, 300, 2, 0 }, { 1401, 1, 1, 1, 760, 300, 0, 0 },
            { 1402, 1, 1, 0, 420, 300, 2, 0 }, { 1402, 1, 1, 1, 720, 300, 0, 0 },
            { 1403, 0, 1, 0, 420, 300, 2, 0 }, { 1403, 0, 1, 1, 720, 300, 0, 0 } } },
        { "a palm (confidence off) moving: nothing", "", 3,
          { { 1500, 1, 0, 0, 600, 300, 1, 0 }, { 1501, 1, 0, 0, 700, 300, 1, 0 }, { 1502, 0, 0, 0, 700, 300, 1, 0 } } },
        { "the pad pressed with one finger: left click", "m1,0,0 m0,0,0", 3,
          { { 1600, 1, 1, 0, 600, 300, 1, 1 }, { 1610, 1, 1, 0, 600, 300, 1, 0 }, { 1611, 0, 1, 0, 600, 300, 1, 0 } } },
        { "the pad pressed with two fingers: right click", "m2,0,0 m0,0,0", 6,
          { { 1700, 1, 1, 0, 500, 300, 2, 1 }, { 1700, 1, 1, 1, 800, 300, 0, 1 },
            { 1710, 1, 1, 0, 500, 300, 2, 0 }, { 1710, 1, 1, 1, 800, 300, 0, 0 },
            { 1711, 0, 1, 0, 500, 300, 2, 0 }, { 1711, 0, 1, 1, 800, 300, 0, 0 } } },
    };
    for (int g = 0; g < (int)(sizeof(gestures) / sizeof(gestures[0])); g++) {
        char out[96];
        tp_feed(d, gestures[g].st, gestures[g].n, out, sizeof(out));
        CHECK(strcmp(out, gestures[g].want) == 0, "touchpad: %s: %s", gestures[g].what, out[0] ? out : "(no events)");
    }

    /* Its interrupt: the GpioInt of the touchpad in the ACPI tables, a
     * pin of the self-tests' GPIO controller, with the modelled touchpad
     * driving it.  (A real controller's pins are left alone.) */
    AmlI2cHid found[AML_MAX_I2C_HID];
    const AmlI2cHid *a = NULL;
    int nfound = AmlI2cHidDevices(found, AML_MAX_I2C_HID);
    for (int i = 0; i < nfound && !a; i++) if (found[i].gpio_int) a = &found[i];
    if (!a) {
        say(ctx, "     touchpad interrupt: no touchpad with a GpioInt in the ACPI tables: not checked");
    } else if (!GpioControllerIsModel(a->gpio_ctrl)) {
        ksnprintf(line, sizeof(line), "     touchpad interrupt: %s is the machine's own GPIO controller: not driven from here",
                  a->gpio_ctrl);
        say(ctx, line);
    } else {
        CHECK(a->gpio_pin == 277 && a->gpio_level && a->gpio_low && !a->gpio_both,
              "touchpad interrupt: the ACPI touchpad's GpioInt: pin %u of %s, %s, active %s", a->gpio_pin, a->gpio_ctrl,
              a->gpio_level ? "level" : "edge", a->gpio_low ? "low" : "high");
        GpioIrq *bad = GpioIrqConnect(a->gpio_ctrl, 230, true, true, false, irq_fired, d, why, sizeof(why));
        CHECK(!bad, "touchpad interrupt: a pin in no pad group is refused (%s)", bad ? "taken" : why);
        if (bad) GpioIrqDisconnect(bad);
        bad = GpioIrqConnect(a->gpio_ctrl, 256, true, true, false, irq_fired, d, why, sizeof(why));
        CHECK(!bad && strstr(why, "native function"), "touchpad interrupt: a pad in its native function is refused (%s)",
              bad ? "taken" : why);
        if (bad) GpioIrqDisconnect(bad);
        volatile UINT32 flag = 0;
        d->kick = &flag;
        d->waiter = sched_current();
        d->stamp = tp_stamp;
        d->gpio = GpioIrqConnect(a->gpio_ctrl, a->gpio_pin, a->gpio_level, a->gpio_low, a->gpio_both,
                                 irq_fired, d, why, sizeof(why));
        CHECK(d->gpio != NULL, "touchpad interrupt: pin %u connected: %s", a->gpio_pin, d->gpio ? GpioIrqName(d->gpio) : why);
        if (d->gpio) {
            UINT32 cfg;
            bool en, st;
            GpioIrqState(d->gpio, &cfg, &en, &st);
            CHECK(!(cfg & 0x3C00) && !(cfg & 0x06000000) && (cfg & 0x00800000) && !(cfg & 0x001E0000) && en,
                  "touchpad interrupt: pad set up as a GPIO input, level, inverted (active low), no other route, "
                  "interrupt enabled (PADCFG0 %08x)", cfg);
            g_tp.irq = d->gpio;
            g_tp.next = g_tp.count = 0;
            for (int t = 0; t < 10; t++) { flag = 0; sched_sleep_until(&flag, sched_ticks() + 1); }
            CHECK(d->irqs == 0 && !d->pending, "touchpad interrupt: no report, no interrupt (100 ms quiet)");
            static const struct { const char *what, *want; int n; TpStep st[8]; } byirq[] = {
                { "one-finger tap", "m1,0,0 m0,0,0", 2,
                  { { 2000, 1, 1, 0, 600, 300, 1, 0 }, { 2005, 0, 1, 0, 600, 300, 1, 0 } } },
                { "two fingers 6 mm down", "m0,0,0/z1 m0,0,0/z1", 8,
                  { { 2100, 1, 1, 0, 500, 200, 2, 0 }, { 2100, 1, 1, 1, 800, 200, 0, 0 },
                    { 2101, 1, 1, 0, 500, 230, 2, 0 }, { 2101, 1, 1, 1, 800, 230, 0, 0 },
                    { 2102, 1, 1, 0, 500, 260, 2, 0 }, { 2102, 1, 1, 1, 800, 260, 0, 0 },
                    { 2103, 0, 1, 0, 500, 260, 2, 0 }, { 2103, 0, 1, 1, 800, 260, 0, 0 } } },
            };
            for (int g = 0; g < 2; g++) {
                char out[96];
                UINT32 before = d->irqs;
                int empties = g_tp.empties;
                tp_feed(d, byirq[g].st, byirq[g].n, out, sizeof(out));
                GpioIrqState(d->gpio, &cfg, &en, &st);
                CHECK(strcmp(out, byirq[g].want) == 0 && d->irqs > before && g_tp.empties == empties &&
                      !GpioIrqAsserted(d->gpio) && en,
                      "touchpad interrupt: %s read on its interrupt (%u interrupt(s), %d empty reads, line let go, "
                      "unmasked again): %s", byirq[g].what, d->irqs - before, g_tp.empties - empties,
                      out[0] ? out : "(no events)");
            }
            g_tp.irq = NULL;
            GpioIrqDisconnect(d->gpio);
            d->gpio = NULL;
        }
    }
#undef CHECK
    kfree(d);
    return failed;
}
