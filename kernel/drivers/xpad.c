/*
 * xpad.c — Xbox 360 and Xbox One controllers over USB
 *
 * Neither is a HID device: both have a vendor-specific interface with an
 * interrupt IN endpoint for input and an interrupt OUT endpoint for the
 * motors and lights, the protocols the Free60 project and the Xbox One
 * "GIP" notes document.  Most other makers' PC gamepads speak the Xbox
 * 360 one (it is what XInput was made for).
 *
 * Xbox 360 (interface class FF, subclass 5D, protocol 01): input reports
 * of 20 bytes, type 0x00: bytes 2-3 the buttons, already in XInput's
 * order (XINPUT_GAMEPAD_*: D-pad, Start, Back, the sticks' buttons, the
 * shoulders, Guide, A, B, X, Y), 4 and 5 the triggers, 6-13 the sticks
 * (16-bit signed, Y up).  The motors take {00 08 00 big small 00 00 00},
 * the ring of lights {01 03 n} (6-9: player 1-4).
 *
 * Xbox One (FF/47/D0): nothing comes until the host sends "power on"
 * {05 20 seq 01 00}; then input reports of type 0x20: byte 4 Menu, View,
 * A, B, X, Y (bits 2-7), byte 5 the D-pad, the shoulders and the sticks'
 * buttons, 6-9 the triggers (0-1023), 10-17 the sticks, and the Guide
 * button in reports of type 0x07.  The motors take a type 0x09 report.
 *
 * Both become a slot with XInput's view and the one DirectInput shows for
 * them on Windows (X/Y the left stick, Rx/Ry the right one, Z the two
 * triggers together, buttons A B X Y LB RB Back Start and the sticks, the
 * D-pad as a hat).
 */

#include "gamepad.h"
#include "usb.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"

#define XB_UP     0x0001
#define XB_DOWN   0x0002
#define XB_LEFT   0x0004
#define XB_RIGHT  0x0008
#define XB_START  0x0010
#define XB_BACK   0x0020
#define XB_LTHUMB 0x0040
#define XB_RTHUMB 0x0080
#define XB_LB     0x0100
#define XB_RB     0x0200
#define XB_GUIDE  0x0400
#define XB_A      0x1000
#define XB_B      0x2000
#define XB_X      0x4000
#define XB_Y      0x8000

typedef struct {
    UsbDev   *dev;
    UsbPipe  *in, *out;
    int       kind;                   /* PAD_XBOX360 or PAD_XBOXONE */
    int       slot;
    UINT8     seq;                    /* Xbox One: the next packet's sequence number */
    PadState  st;
    bool      dead;
} Xpad;

static INT16 rd16(const UINT8 *p) { return (INT16)(p[0] | p[1] << 8); }

/* DirectInput's view from XInput's */
static void generic_view(PadState *s)
{
    static const UINT16 order[10] = { XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_BACK, XB_START, XB_LTHUMB, XB_RTHUMB };
    s->buttons = 0;
    for (int i = 0; i < 10; i++) if (s->xbuttons & order[i]) s->buttons |= 1u << i;
    s->axis[PAD_X]  = (UINT16)(s->lx + 32768);
    s->axis[PAD_Y]  = (UINT16)(32767 - s->ly);
    s->axis[PAD_RX] = (UINT16)(s->rx + 32768);
    s->axis[PAD_RY] = (UINT16)(32767 - s->ry);
    INT32 z = 32768 + (INT32)s->lt * 128 - (INT32)s->rt * 128;      /* the left trigger pushes Z up */
    s->axis[PAD_Z]  = (UINT16)(z < 0 ? 0 : z > 65535 ? 65535 : z);
    bool up = s->xbuttons & XB_UP, down = s->xbuttons & XB_DOWN, left = s->xbuttons & XB_LEFT, right = s->xbuttons & XB_RIGHT;
    if (up && down) up = down = false;
    if (left && right) left = right = false;
    s->pov = up ? (right ? 4500 : left ? 31500 : 0) : down ? (right ? 13500 : left ? 22500 : 18000) :
             right ? 9000 : left ? 27000 : -1;
}

static void publish(Xpad *x, const PadState *now)
{
    PadState s = *now;
    generic_view(&s);
    s.packet = x->st.packet;
    if (memcmp(&s, &x->st, sizeof(s)) == 0) return;
    x->st = s;
    PadReport(x->slot, &s);
}

static bool on_input(UsbPipe *p, const UINT8 *d, int len, void *ctx)
{
    (void)p;
    Xpad *x = ctx;
    if (x->dead) return false;
    PadState s = x->st;
    if (x->kind == PAD_XBOX360) {
        if (len < 14 || d[0] != 0x00 || d[1] < 14) return true;      /* (LED and rumble status reports) */
        s.xbuttons = (UINT16)(d[2] | d[3] << 8) & ~0x0800;
        s.lt = d[4]; s.rt = d[5];
        s.lx = rd16(d + 6); s.ly = rd16(d + 8);
        s.rx = rd16(d + 10); s.ry = rd16(d + 12);
    } else if (len >= 18 && d[0] == 0x20) {
        UINT16 b = s.xbuttons & XB_GUIDE;
        static const UINT16 b4[6] = { XB_START, XB_BACK, XB_A, XB_B, XB_X, XB_Y };   /* byte 4, bits 2-7 */
        static const UINT16 b5[8] = { XB_UP, XB_DOWN, XB_LEFT, XB_RIGHT, XB_LB, XB_RB, XB_LTHUMB, XB_RTHUMB };
        for (int i = 0; i < 6; i++) if (d[4] & (4 << i)) b |= b4[i];
        for (int i = 0; i < 8; i++) if (d[5] & (1 << i)) b |= b5[i];
        s.xbuttons = b;
        UINT32 lt = (UINT32)(d[6] | d[7] << 8) & 0x3FF, rt = (UINT32)(d[8] | d[9] << 8) & 0x3FF;
        s.lt = (UINT8)(lt >> 2); s.rt = (UINT8)(rt >> 2);
        s.lx = rd16(d + 10); s.ly = rd16(d + 12);
        s.rx = rd16(d + 14); s.ry = rd16(d + 16);
    } else if (len >= 5 && d[0] == 0x07) {                           /* the Guide button */
        s.xbuttons = (UINT16)((s.xbuttons & ~XB_GUIDE) | (d[4] & 1 ? XB_GUIDE : 0));
    } else {
        return true;
    }
    publish(x, &s);
    return true;
}

static bool send_out(Xpad *x, const UINT8 *pkt, int len)
{
    UINT8 buf[16];
    memcpy(buf, pkt, (size_t)len);
    return UsbBulk(x->out, buf, (UINT32)len, 100, NULL) == len;
}

static bool rumble(void *ctx, UINT16 left, UINT16 right)
{
    Xpad *x = ctx;
    if (x->dead || !x->out) return false;
    if (x->kind == PAD_XBOX360) {
        UINT8 r[8] = { 0x00, 0x08, 0x00, (UINT8)(left >> 8), (UINT8)(right >> 8), 0, 0, 0 };
        return send_out(x, r, 8);
    }
    UINT8 r[13] = { 0x09, 0x00, x->seq++, 0x09, 0x00, 0x0F, 0x00, 0x00,
                    (UINT8)(left / 656), (UINT8)(right / 656), 0xFF, 0x00, 0xFF };   /* (percent) */
    return send_out(x, r, 13);
}

static void gone(void *inst)
{
    Xpad *x = inst;
    x->dead = true;
    PadDetach(x->slot);
    kprintf("[USB] %s: Xbox controller removed\n", UsbDevName(x->dev));
    /* (the Xpad stays allocated: a report may still be on its way) */
}

void *UsbXpadProbe(UsbDev *d, const UsbIface *f)
{
    int kind = f->cls == 0xFF && f->sub == 0x5D && f->proto == 0x01 ? PAD_XBOX360 :
               f->cls == 0xFF && f->sub == 0x47 && f->proto == 0xD0 && f->number == 0 ? PAD_XBOXONE : 0;
    if (!kind) return NULL;
    const UINT8 *ein = NULL, *eout = NULL;
    int off = 0;
    for (const UINT8 *e; (e = UsbIfaceFind(f, USB_DT_ENDPOINT, &off)) != NULL;) {
        if ((e[3] & 3) != 3) continue;
        if ((e[2] & 0x80) && !ein) ein = e;
        if (!(e[2] & 0x80) && !eout) eout = e;
    }
    if (!ein) return NULL;
    Xpad *x = kzalloc(sizeof(Xpad));
    if (!x) return NULL;
    x->dev = d;
    x->kind = kind;
    UINT16 mps = (UINT16)((ein[4] | ein[5] << 8) & 0x7FF);
    x->in = UsbOpenPipe(d, ein, mps);
    if (eout) x->out = UsbOpenPipe(d, eout, 64);
    if (!x->in) { kfree(x); return NULL; }

    PadInfo info;
    memset(&info, 0, sizeof(info));
    info.kind = (UINT8)kind;
    info.vid = UsbDevVendor(d);
    info.pid = UsbDevProduct(d);
    info.buttons = 10;
    info.axes = 1 << PAD_X | 1 << PAD_Y | 1 << PAD_Z | 1 << PAD_RX | 1 << PAD_RY;
    info.povs = 1;
    info.rumble = x->out != NULL;
    info.usage = 0x0105;
    /* the name DirectInput shows for the controller on Windows */
    strcpy(info.name, kind == PAD_XBOX360 ? "Controller (XBOX 360 For Windows)" : "Controller (Xbox One For Windows)");
    x->st.pov = -1;
    x->slot = PadAttach(&info, rumble, x);
    if (x->slot < 0) { kprintf("[USB] %s: no room for another game controller\n", UsbDevName(d)); kfree(x); return NULL; }
    PadInfo got;
    PadGetInfo(x->slot, &got);
    UsbBind(d, x, gone);
    {
        PadState s;
        memset(&s, 0, sizeof(s));
        publish(x, &s);                                             /* (centred, until it reports) */
    }
    if (x->out && kind == PAD_XBOX360 && got.xuser >= 0) {
        UINT8 led[3] = { 0x01, 0x03, (UINT8)(0x06 + got.xuser) };   /* light the player's quadrant */
        send_out(x, led, 3);
    }
    if (x->out && kind == PAD_XBOXONE) {
        UINT8 on[5] = { 0x05, 0x20, x->seq++, 0x01, 0x00 };         /* power on: start reporting */
        if (!send_out(x, on, 5)) kprintf("[USB] %s: the Xbox One controller did not take \"power on\"\n", UsbDevName(d));
    }
    char product[48];
    if (!UsbDevProductName(d, product, sizeof(product))) product[0] = '\0';
    kprintf("[USB] %s: %s controller%s%s%s (XInput user %d)\n", UsbDevName(d), kind == PAD_XBOX360 ? "Xbox 360" : "Xbox One",
            product[0] ? " \"" : "", product, product[0] ? "\"" : "", got.xuser + 1);
    UsbPipeListen(x->in, mps, on_input, x);
    return x;
}
