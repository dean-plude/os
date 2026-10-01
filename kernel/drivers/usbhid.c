/*
 * usbhid.c — HID boot-protocol keyboard and mouse reports
 *
 * Boot keyboard report (HID 1.11, appendix B.1): byte 0 holds the eight
 * modifier keys as bits, byte 1 is reserved, bytes 2-7 list the usages
 * (keyboard page 0x07) of up to six keys held down.  A key press or
 * release is a usage appearing in or vanishing from that list, so each
 * report is compared with the previous one.  Boot mouse report (B.2):
 * buttons, dx, dy and, on most mice, a wheel byte.
 */

#include "usbhid.h"
#include "../wm/input.h"
#include "../lib/string.h"

#define EXT 0x100                     /* E0-prefixed set-1 code */

#define REPEAT_DELAY 50               /* ticks (500 ms) before a held key repeats */
#define REPEAT_RATE  3                /* then a repeat every 30 ms */

/* Keyboard usages 0x00-0x65 → set-1 scancode (0 = none) */
static const UINT16 g_usage_to_set1[0x66] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,   /* a b c d */
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,   /* e f g h */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,   /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,   /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,   /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,   /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C,                                 /* y z */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,   /* 1 2 3 4 */
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,   /* 5 6 7 8 */
    [0x26] = 0x0A, [0x27] = 0x0B,                                 /* 9 0 */
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F,   /* Enter Esc Backspace Tab */
    [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,   /* Space - = [ */
    [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,   /* ] \ (non-US #) ; */
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34,   /* ' ` , . */
    [0x38] = 0x35, [0x39] = 0x3A,                                 /* / Caps Lock */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,   /* F1-F4 */
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,   /* F5-F8 */
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,   /* F9-F12 */
    [0x46] = EXT | 0x37, [0x47] = 0x46,                           /* Print Screen, Scroll Lock */
    [0x49] = EXT | 0x52, [0x4A] = EXT | 0x47, [0x4B] = EXT | 0x49, /* Insert Home PgUp */
    [0x4C] = EXT | 0x53, [0x4D] = EXT | 0x4F, [0x4E] = EXT | 0x51, /* Delete End PgDn */
    [0x4F] = EXT | 0x4D, [0x50] = EXT | 0x4B,                     /* Right Left */
    [0x51] = EXT | 0x50, [0x52] = EXT | 0x48,                     /* Down Up */
    [0x53] = 0x45, [0x54] = EXT | 0x35, [0x55] = 0x37,            /* Num Lock, keypad / * */
    [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = EXT | 0x1C,            /* keypad - + Enter */
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B,   /* keypad 1-4 */
    [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48,   /* keypad 5-8 */
    [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53,                  /* keypad 9 0 . */
    [0x64] = 0x56, [0x65] = EXT | 0x5D,                           /* non-US \, Application */
};

/* Modifier bits 0-7: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui */
static const UINT16 g_mod_to_set1[8] = {
    0x1D, 0x2A, 0x38, EXT | 0x5B, EXT | 0x1D, 0x36, EXT | 0x38, EXT | 0x5C,
};

static void post_key(UINT16 code, bool pressed)
{
    if (!code) return;
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = INPUT_KEY;
    ev.scancode = (UINT8)(code & 0x7F);
    ev.extended = (code & EXT) ? 1 : 0;
    ev.pressed  = pressed ? 1 : 0;
    InputPost(&ev);
}

static UINT16 usage_code(UINT8 u)
{
    return u < sizeof(g_usage_to_set1) / sizeof(g_usage_to_set1[0]) ? g_usage_to_set1[u] : 0;
}

static bool has_usage(const UINT8 *r, UINT8 u)
{
    for (int i = 2; i < 8; i++)
        if (r[i] == u) return true;
    return false;
}

void UsbHidKeyboardReport(UsbHidKbd *k, const UINT8 *r, int len, UINT64 now)
{
    if (len < 8) return;
    /* Too many keys down: the keyboard reports "rollover error" in every
     * slot; keep the previous state until it can tell again */
    if (r[2] == 0x01) return;

    UINT8 mods = r[0], old = k->prev[0];
    for (int b = 0; b < 8; b++)
        if ((mods ^ old) & (1u << b))
            post_key(g_mod_to_set1[b], (mods >> b) & 1);

    for (int i = 2; i < 8; i++) {                 /* released */
        UINT8 u = k->prev[i];
        if (u > 0x03 && !has_usage(r, u)) {
            post_key(usage_code(u), false);
            if (k->repeat == u) k->repeat = 0;
        }
    }
    for (int i = 2; i < 8; i++) {                 /* pressed */
        UINT8 u = r[i];
        if (u > 0x03 && !has_usage(k->prev, u)) {
            post_key(usage_code(u), true);
            k->repeat = u;                        /* the newest key repeats */
            k->repeat_at = now + REPEAT_DELAY;
        }
    }
    memcpy(k->prev, r, 8);
}

void UsbHidTick(UsbHidKbd *k, UINT64 now)
{
    if (!k->repeat || now < k->repeat_at) return;
    post_key(usage_code(k->repeat), true);        /* a make without a break, as PS/2 sends */
    k->repeat_at = now + REPEAT_RATE;
}

void UsbHidKeyboardGone(UsbHidKbd *k)
{
    UINT8 none[8] = { 0 };
    UsbHidKeyboardReport(k, none, 8, 0);
    k->repeat = 0;
}

void UsbHidMouseReport(const UINT8 *r, int len)
{
    if (len < 3) return;
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = INPUT_MOUSE;
    ev.buttons = (UINT8)(r[0] & 0x07);          /* L|R|M, as MOUSE_* */
    ev.dx      = (INT8)r[1];
    ev.dy      = (INT8)r[2];                    /* USB +y is already down */
    ev.dz      = len >= 4 ? (INT8)r[3] : 0;     /* +1 per notch away from the user */
    InputPost(&ev);
}
