/*
 * hv_input.c — Hyper-V's synthetic keyboard and mouse
 *
 * Both are VMBus channels (vmbus.c) and, on a Generation 2 VM, the only
 * keyboard and mouse there are.  Their protocols (as Linux's
 * hyperv-keyboard and hid-hyperv drivers and FreeBSD's hv_kbd speak them):
 *
 * Keyboard: every message starts with a 32-bit type.  The guest asks for
 * protocol 1.0 (PROTOCOL_REQUEST) and the host accepts it
 * (PROTOCOL_RESPONSE, bit 0); then each key is an EVENT with its set-1
 * make code and flags (break, E0, E1, or a UTF-16 character the host types
 * for "Type clipboard text", which has no key and is skipped), posted as
 * the PS/2 keyboard's keys are (hal/ps2.c).
 *
 * Mouse: HID over VMBus.  Each message is a pipe header (type 1 = data,
 * size) around a synthetic HID message (type, size).  The guest asks for
 * protocol 2.0; the host approves it and sends INITIAL_DEVICE_INFO: the
 * device's IDs, its HID descriptor and right after it the report
 * descriptor, which the guest acknowledges.  Then each INPUT_REPORT holds
 * one HID input report.  The report descriptor and the reports go to the
 * HID decoding USB and I2C mice use (usbhid.c, hid.h): Hyper-V's mouse is
 * an absolute pointer (X and Y 0-32767), buttons and a wheel.
 */

#include "hv_input.h"
#include "hid.h"
#include "../wm/input.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"

/* ---- keyboard ---- */

#define KBD_PROTOCOL_REQUEST  1
#define KBD_PROTOCOL_RESPONSE 2
#define KBD_EVENT             3
#define KBD_VERSION           0x00010000u
#define KBD_UNICODE           (1u << 0)
#define KBD_BREAK             (1u << 1)
#define KBD_E0                (1u << 2)
#define KBD_E1                (1u << 3)

static UINT32 rd32(const UINT8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (UINT32)p[3] << 24; }
static UINT16 rd16(const UINT8 *p) { return (UINT16)(p[0] | p[1] << 8); }

static void *kbd_opened(VmbusChannel *ch)
{
    UINT32 req[2] = { KBD_PROTOCOL_REQUEST, KBD_VERSION };
    if (!VmbusSend(ch, req, sizeof(req), 1, true))
        kprintf("[HVINPUT] keyboard: cannot send the protocol request\n");
    return ch;
}

/* One keyboard message into @ev; false when it is not a key */
static bool kbd_event(const UINT8 *d, UINT32 len, InputEvent *ev)
{
    if (len < 12 || rd32(d) != KBD_EVENT) return false;
    UINT16 make = rd16(d + 4);
    UINT32 info = rd32(d + 8);
    if (info & (KBD_UNICODE | KBD_E1)) return false;   /* typed text; Pause (not reported, as on PS/2) */
    memset(ev, 0, sizeof(*ev));
    ev->type     = INPUT_KEY;
    ev->scancode = (UINT8)(make & 0x7F);
    ev->pressed  = (info & KBD_BREAK) ? 0 : 1;
    ev->extended = (info & KBD_E0) ? 1 : 0;
    return true;
}

static void kbd_packet(void *ctx, const UINT8 *d, UINT32 len)
{
    (void)ctx;
    if (len >= 8 && rd32(d) == KBD_PROTOCOL_RESPONSE) {
        kprintf("[HVINPUT] keyboard: protocol 1.0 %s\n", (rd32(d + 4) & 1) ? "accepted" : "refused");
        return;
    }
    InputEvent ev;
    if (kbd_event(d, len, &ev)) InputPost(&ev);
}

const VmbusDriver HvKeyboardDriver = {
    { 0x6d, 0xad, 0x12, 0xf9, 0x17, 0x2b, 0xea, 0x48, 0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84 },
    "Hyper-V keyboard", kbd_opened, kbd_packet,
};

/* ---- mouse ---- */

#define PIPE_DATA                 1
#define HID_PROTOCOL_REQUEST      0
#define HID_PROTOCOL_RESPONSE     1
#define HID_INITIAL_DEVICE_INFO   2
#define HID_INITIAL_DEVICE_INFO_ACK 3
#define HID_INPUT_REPORT          4
#define HID_VERSION               0x00020000u
#define DEV_INFO_SIZE             30   /* size, vendor, product, version, 11 reserved (16 bits each) */

typedef struct {
    VmbusChannel *ch;
    void         *hid;
} HvMouse;

static void *mouse_opened(VmbusChannel *ch)
{
    HvMouse *m = kzalloc(sizeof(HvMouse));
    if (!m) return NULL;
    m->ch = ch;
    UINT32 req[5] = { PIPE_DATA, 12, HID_PROTOCOL_REQUEST, 4, HID_VERSION };
    if (!VmbusSend(ch, req, sizeof(req), 1, true))
        kprintf("[HVINPUT] mouse: cannot send the protocol request\n");
    return m;
}

/* INITIAL_DEVICE_INFO's report descriptor (@body: after the HID message
 * header, @n bytes); NULL when it does not fit */
static const UINT8 *mouse_report_desc(const UINT8 *body, UINT32 n, int *len)
{
    if (n < DEV_INFO_SIZE + 9) return NULL;
    const UINT8 *hd = body + DEV_INFO_SIZE;            /* the HID descriptor */
    UINT32 hlen = hd[0], rlen = rd16(hd + 7);
    if (hlen < 9 || DEV_INFO_SIZE + hlen + rlen > n) return NULL;
    *len = (int)rlen;
    return hd + hlen;
}

static void mouse_packet(void *ctx, const UINT8 *d, UINT32 len)
{
    HvMouse *m = ctx;
    if (!m || len < 16 || rd32(d) != PIPE_DATA) return;
    UINT32 psize = rd32(d + 4);
    if (psize > len - 8) psize = len - 8;
    const UINT8 *h = d + 8;
    if (psize < 8) return;
    UINT32 type = rd32(h), hsize = rd32(h + 4);
    const UINT8 *body = h + 8;
    if (hsize > psize - 8) hsize = psize - 8;

    switch (type) {
    case HID_PROTOCOL_RESPONSE:
        kprintf("[HVINPUT] mouse: protocol 2.0 %s\n", (psize >= 13 && body[4]) ? "approved" : "refused");
        break;
    case HID_INITIAL_DEVICE_INFO: {
        int rlen = 0;
        const UINT8 *rd = mouse_report_desc(body, psize - 8, &rlen);
        UINT8 ack[17] = { PIPE_DATA, 0, 0, 0, 9, 0, 0, 0, HID_INITIAL_DEVICE_INFO_ACK, 0, 0, 0, 1, 0, 0, 0, 0 };
        VmbusSend(m->ch, ack, sizeof(ack), 2, true);
        if (m->hid) break;                              /* (sent again: the device is set up) */
        const char *kind = NULL;
        m->hid = rd ? HidAttach(rd, rlen, &kind) : NULL;
        kprintf("[HVINPUT] mouse: %04x:%04x, %d-byte report descriptor: %s\n",
                psize - 8 >= 6 ? rd16(body + 2) : 0, psize - 8 >= 6 ? rd16(body + 4) : 0, rlen,
                m->hid ? kind : "nothing NovaOS uses");
        if (m->hid) HidStart(m->hid);
        break;
    }
    case HID_INPUT_REPORT:
        if (m->hid && hsize) HidInput(m->hid, body, (int)hsize);
        break;
    }
}

const VmbusDriver HvMouseDriver = {
    { 0x9e, 0xb6, 0xa8, 0xcf, 0x4a, 0x5b, 0xc0, 0x4c, 0xb9, 0x8b, 0x8b, 0xa1, 0xa1, 0xf3, 0xf9, 0x5a },
    "Hyper-V mouse", mouse_opened, mouse_packet,
};
