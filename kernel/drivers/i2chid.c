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
 * The device raises an interrupt line (a GPIO pin) when it has a report.
 * NovaOS has no GPIO driver yet, so the "i2chid" thread polls instead,
 * as FreeBSD's iichid does without an interrupt: every 10 ms while
 * reports come, every 50 ms after half a second without one; a device
 * with nothing to say answers with a length of 0 or 2.
 *
 * Touchpads (Windows precision touchpads) start in mouse mode: relative
 * motion and buttons from their mouse collection, which is what NovaOS
 * uses (tap-to-click and two-finger scrolling need the touchpad mode).
 *
 * I2cHidSelfCheck() runs the protocol against a modelled touchpad on a
 * modelled bus: QEMU has neither an LPSS I2C controller nor an I2C-HID
 * device.
 */

#include "i2chid.h"
#include "i2c.h"
#include "hid.h"
#include "../hal/aml.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../ke/smp.h"
#include "../arch/x86_64/cpu.h"

#define OP_RESET      0x01
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
    UINT16  rdesc_len, rdesc_reg, input_reg, max_input, cmd_reg;
    UINT16  vendor, product;
    void   *hid;
    char    name[64];
    UINT8   buf[MAX_REPORT];
} I2cHid;

static I2cHid *g_devs[MAX_DEVS];
static int     g_ndevs;
static volatile UINT32 g_pause, g_in_poll, g_reinit;

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

/* One input report: its length (0: none), the report at d->buf + 2 */
static int read_input(I2cHid *d)
{
    int n = d->max_input < MAX_REPORT ? d->max_input : MAX_REPORT;
    if (n < 2 || !d->bus->xfer(d->bus, d->addr, NULL, 0, d->buf, n)) return 0;
    int len = le16(d->buf);
    return len <= 2 || len > n ? 0 : len - 2;
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
    return true;
}

static void reinit_all(void)
{
    DwI2cResume();
    for (int i = 0; i < g_ndevs; i++)
        if (!power_and_reset(g_devs[i])) kprintf("[I2C] %s: no reset acknowledge after sleep\n", g_devs[i]->name);
}

static void i2chid_thread(void *arg)
{
    (void)arg;
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
        g_devs[g_ndevs++] = d;
        kprintf("[I2C] %s %04x:%04x on %s: %s, polled (no GPIO interrupt driver)\n",
                d->name, d->vendor, d->product, bus->name, kind);
    }
    if (!g_ndevs) sched_exit_current();

    int idle = 0;
    for (;;) {
        sched_sleep_until(NULL, sched_ticks() + (idle >= 50 ? 5 : 1));
        if (__atomic_load_n(&g_pause, __ATOMIC_ACQUIRE)) continue;
        __atomic_store_n(&g_in_poll, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_pause, __ATOMIC_ACQUIRE)) { __atomic_store_n(&g_in_poll, 0, __ATOMIC_RELEASE); continue; }
        if (__atomic_exchange_n(&g_reinit, 0, __ATOMIC_ACQ_REL)) reinit_all();
        bool any = false;
        for (int i = 0; i < g_ndevs; i++) {
            I2cHid *d = g_devs[i];
            int len = read_input(d);
            if (!len) continue;
            HidInput(d->hid, d->buf + 2, len);
            any = true;
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
 * configuration collection's Input Mode feature (report 3).  It obeys
 * SET_POWER and RESET and hands out the input reports queued for it.
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
    0xC0, 0xC0,
};

static struct {
    bool  powered, reset_ack, power_cmd, reset_cmd;
    int   next, count, empties;
    UINT8 rep[4][12];
    int   len[4];
} g_tp;

static bool tp_xfer(I2cBus *bus, UINT16 addr, const UINT8 *w, int wn, UINT8 *r, int rn)
{
    (void)bus;
    if (addr != 0x2C) return false;                                   /* (nobody there: no acknowledge) */
    UINT16 reg = wn >= 2 ? (UINT16)(w[0] | w[1] << 8) : 0;
    if (wn == 4 && reg == 0x0024) {                                   /* a command */
        if (w[3] == OP_SET_POWER) { g_tp.powered = !(w[2] & 1); g_tp.power_cmd = true; }
        if (w[3] == OP_RESET && g_tp.powered) { g_tp.reset_ack = true; g_tp.reset_cmd = true; }
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
        return true;
    }
    return false;
}

int I2cHidSelfCheck(void (*say)(void *ctx, const char *line), void *ctx)
{
    int failed = 0;
    char line[176];
#define CHECK(ok, ...) do { bool ok_ = (ok); char t_[150]; ksnprintf(t_, sizeof(t_), __VA_ARGS__); \
                            ksnprintf(line, sizeof(line), "%s %s", ok_ ? "ok  " : "FAIL", t_); \
                            say(ctx, line); if (!ok_) failed++; } while (0)
    memset(&g_tp, 0, sizeof(g_tp));
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
    CHECK(ok && strcmp(kind, "touchpad (mouse mode)") == 0, "touchpad: report descriptor read as %s", kind);
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
#undef CHECK
    /* (d->hid stays allocated: it was never started) */
    kfree(d);
    return failed;
}
