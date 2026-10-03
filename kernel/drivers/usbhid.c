/*
 * usbhid.c — USB HID keyboards, mice, tablets and touch screens
 *
 * Devices run in report protocol: the interface's report descriptor (HID
 * 1.11, section 6.2.2) says where each control sits in the input reports,
 * and the parser below turns it into a list of fields.  Keyboards report
 * the keys held down (an array of usages, or a bitmap), mice relative X/Y,
 * the wheels (vertical, and horizontal as Consumer "AC Pan") and up to five
 * buttons, tablets and touch screens absolute X/Y with a button or a tip
 * switch, and pens (digitizer pens: tip pressure, barrel buttons, eraser,
 * in range) INPUT_PEN events for wintab32 too (wm/tablet.h).  Media, browser and launch keys come as Consumer Control usages
 * (page 0x0C), and the Power, Sleep and Wake keys as System Control
 * (Generic Desktop 0x81-0x83), usually in reports of their own or on an
 * interface of their own; both become the E0-prefixed scancodes a PS/2
 * keyboard sends for those keys.  A boot-class device whose report descriptor can't be
 * read or understood is put in boot protocol and parsed with the boot
 * descriptors from appendix B instead.
 *
 * Keys become set-1 scancodes (the codes a PS/2 keyboard sends), so
 * everything above the drivers sees one kind of keyboard.  USB keyboards
 * do not repeat keys themselves; UsbHidTickAll() does it.
 */

#include "usb.h"
#include "../wm/input.h"
#include "../wm/tablet.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"

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

/* Usages 0xE0-0xE7: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui */
static const UINT16 g_mod_to_set1[8] = {
    0x1D, 0x2A, 0x38, EXT | 0x5B, EXT | 0x1D, 0x36, EXT | 0x38, EXT | 0x5C,
};

/* Boot protocol report descriptors (HID 1.11 appendix B) */
static const UINT8 g_boot_kbd_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};
static const UINT8 g_boot_mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0,
};

/* ---------------------------------------------------------------------------
 * Report descriptor parsing
 * ------------------------------------------------------------------------- */

#define PAGE_DESKTOP   0x01
#define PAGE_KEYBOARD  0x07
#define PAGE_BUTTON    0x09
#define PAGE_LED       0x08
#define PAGE_DIGITIZER 0x0D
#define PAGE_CONSUMER  0x0C

#define USAGE_AC_PAN   0x238          /* Consumer: horizontal wheel */

#define MAX_FIELDS     96

typedef struct {
    UINT8  id;                 /* report ID (0: the device uses none) */
    UINT16 page;
    UINT16 usage;              /* variable: its usage; array: the first usage */
    UINT16 usage_max;          /* array: the last usage */
    UINT16 bit;                /* where in the report (after the ID byte) */
    UINT8  size, count;        /* array: @count elements of @size bits */
    bool   array, relative;
    INT32  lmin, lmax;
    UINT8  coll;               /* the innermost collection it is in (numbered from 1 in order) */
} HidField;

typedef struct {
    HidField f[MAX_FIELDS];
    int      n;
    bool     ids;              /* reports start with an ID byte */
    UINT16   app;              /* the first application collection: page << 8 | usage */
    /* Keyboard LEDs: Num Lock, Caps Lock, Scroll Lock in output report
     * @led_id (@led_bytes long, without the ID byte) */
    INT16    led_bit[3];       /* -1: no such LED */
    UINT8    led_id, led_bytes;
} HidLayout;

static UINT32 item_u(const UINT8 *p, int n)
{
    UINT32 v = 0;
    for (int i = 0; i < n; i++) v |= (UINT32)p[i] << (8 * i);
    return v;
}

static INT32 item_s(const UINT8 *p, int n)
{
    UINT32 v = item_u(p, n);
    if (n == 1) return (INT8)v;
    if (n == 2) return (INT16)v;
    return (INT32)v;
}

typedef struct { UINT16 page; INT32 lmin, lmax; UINT8 size, count, id; } Globals;

static bool parse_report_desc(const UINT8 *d, int len, HidLayout *L)
{
    memset(L, 0, sizeof(*L));
    Globals g = { 0 }, stack[4];
    int sp = 0;
    UINT32 usages[32];
    int nusage = 0;
    UINT32 umin = 0, umax = 0;
    bool have_range = false;
    UINT16 bits[256], obits[256];       /* input and output bits so far, per report ID */
    memset(bits, 0, sizeof(bits));
    memset(obits, 0, sizeof(obits));
    L->led_bit[0] = L->led_bit[1] = L->led_bit[2] = -1;
    int depth = 0;
    UINT8 colls = 0, cstack[16] = { 0 };   /* collections so far; the open ones */

    for (int i = 0; i < len;) {
        UINT8 b = d[i];
        if (b == 0xFE) {                                 /* long item: skip */
            if (i + 1 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        int n = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + n > len) break;
        const UINT8 *v = &d[i + 1];
        UINT8 tag = b & 0xFC;
        i += 1 + n;
        switch (tag) {
        /* Main items */
        case 0x80: {                                     /* Input */
            UINT32 flags = item_u(v, n);
            bool constant = flags & 1, variable = flags & 2, relative = flags & 4;
            UINT16 *pos = &bits[g.id];
            if (!constant && g.size && g.count) {
                if (variable) {
                    for (int k = 0; k < g.count && L->n < MAX_FIELDS; k++) {
                        UINT32 u;
                        if (k < nusage) u = usages[k];
                        else if (have_range && umin + (UINT32)k - (UINT32)nusage <= umax) u = umin + (UINT32)k - (UINT32)nusage;
                        else if (nusage) u = usages[nusage - 1];
                        else if (have_range) u = umax;
                        else continue;
                        HidField *f = &L->f[L->n++];
                        f->id = g.id;
                        f->coll = depth ? cstack[depth < 16 ? depth - 1 : 15] : 0;
                        f->page = (u >> 16) ? (UINT16)(u >> 16) : g.page;
                        f->usage = (UINT16)u;
                        f->bit = (UINT16)(*pos + k * g.size);
                        f->size = g.size;
                        f->count = 1;
                        f->relative = relative;
                        f->lmin = g.lmin; f->lmax = g.lmax;
                    }
                } else if (L->n < MAX_FIELDS) {
                    HidField *f = &L->f[L->n++];
                    UINT32 first = have_range ? umin : nusage ? usages[0] : 0;
                    f->id = g.id;
                    f->coll = depth ? cstack[depth < 16 ? depth - 1 : 15] : 0;
                    f->page = (first >> 16) ? (UINT16)(first >> 16) : g.page;
                    f->usage = (UINT16)first;
                    f->usage_max = have_range ? (UINT16)umax : (UINT16)first;
                    f->bit = *pos;
                    f->size = g.size;
                    f->count = g.count;
                    f->array = true;
                    f->lmin = g.lmin; f->lmax = g.lmax;
                }
            }
            *pos = (UINT16)(*pos + g.size * g.count);
            nusage = 0; have_range = false;
            break;
        }
        case 0x90: {                                     /* Output: keyboard LEDs */
            UINT32 flags = item_u(v, n);
            UINT16 *pos = &obits[g.id];
            if (!(flags & 1) && (flags & 2) && g.size == 1)
                for (int k = 0; k < g.count; k++) {
                    UINT32 u;
                    if (k < nusage) u = usages[k];
                    else if (have_range && umin + (UINT32)k - (UINT32)nusage <= umax) u = umin + (UINT32)k - (UINT32)nusage;
                    else continue;
                    UINT16 page = (u >> 16) ? (UINT16)(u >> 16) : g.page;
                    UINT16 led = (UINT16)u;
                    if (page == PAGE_LED && led >= 1 && led <= 3 &&
                        (L->led_bit[0] < 0 && L->led_bit[1] < 0 && L->led_bit[2] < 0 ? true : L->led_id == g.id)) {
                        L->led_bit[led - 1] = (INT16)(*pos + k);
                        L->led_id = g.id;
                    }
                }
            *pos = (UINT16)(*pos + g.size * g.count);
            nusage = 0; have_range = false;
            break;
        }
        case 0xB0:                                       /* Feature */
            nusage = 0; have_range = false;
            break;
        case 0xA0:                                       /* Collection */
            if (depth == 0 && item_u(v, n) == 1 && !L->app && nusage)
                L->app = (UINT16)((((usages[0] >> 16) ? (usages[0] >> 16) : g.page) << 8) | (usages[0] & 0xFF));
            if (depth < 16) cstack[depth] = ++colls;
            depth++;
            nusage = 0; have_range = false;
            break;
        case 0xC0: if (depth) depth--; break;            /* End Collection */
        /* Global items */
        case 0x04: g.page = (UINT16)item_u(v, n); break;
        case 0x14: g.lmin = item_s(v, n); break;
        case 0x24:
            g.lmax = item_s(v, n);
            if (g.lmin >= 0 && g.lmax < 0) g.lmax = (INT32)item_u(v, n);   /* (unsigned, as many devices mean) */
            break;
        case 0x74: g.size = (UINT8)item_u(v, n); break;
        case 0x84: g.id = (UINT8)item_u(v, n); L->ids = true; break;
        case 0x94: g.count = (UINT8)item_u(v, n); break;
        case 0xA4: if (sp < 4) stack[sp++] = g; break;   /* Push */
        case 0xB4: if (sp) g = stack[--sp]; break;       /* Pop */
        /* Local items */
        case 0x08:
            if (nusage < 32) usages[nusage++] = n == 4 ? item_u(v, n) : item_u(v, n) | ((UINT32)g.page << 16);
            break;
        case 0x18: umin = n == 4 ? item_u(v, n) : item_u(v, n) | ((UINT32)g.page << 16); have_range = true; break;
        case 0x28: umax = n == 4 ? item_u(v, n) : item_u(v, n) | ((UINT32)g.page << 16); have_range = true; break;
        default: break;
        }
    }
    L->led_bytes = (UINT8)((obits[L->led_id] + 7) / 8);
    return L->n > 0;
}

/* @size bits at @bit of @r (@len bytes), sign-extended if @lmin < 0 */
static INT32 get_bits(const UINT8 *r, int len, int bit, int size, INT32 lmin)
{
    UINT32 v = 0;
    for (int k = 0; k < size && k < 32; k++) {
        int at = bit + k;
        if (at / 8 >= len) break;
        if (r[at / 8] & (1u << (at % 8))) v |= 1u << k;
    }
    if (lmin < 0 && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (INT32)v;
}

/* ---------------------------------------------------------------------------
 * Devices
 * ------------------------------------------------------------------------- */

#define MAX_HID 16

#define MAX_CC 8                       /* consumer and system keys held at once */

typedef struct {
    UsbDev    *dev;
    UsbPipe   *pipe;
    HidLayout  L;
    bool       keyboard, pointer, absolute, boot, wake, media, touch, pen;
    UINT8      kbd_id;               /* the report the keys come in */
    UINT8      keys[32];             /* keyboard usages held down (bitmap) */
    UINT16     cc[MAX_CC];           /* media and system keys held down (set-1 codes) */
    UINT8      cc_id[MAX_CC];        /*   and the report each came in */
    int        ncc;
    UINT16     repeat;               /* the set-1 code that repeats (0: none) */
    UINT64     repeat_at;
    UINT8      buttons;
    UINT8      iface;
    UINT8      leds;                 /* lock state the LEDs show (0xFF: not set yet) */
    /* Multi-touch: the device's Contact Identifier in each slot (and
     * whether that slot is touching, and was in this frame), and the
     * contacts the frame still has to come (hybrid mode) */
    INT32      t_cid[TOUCH_MAX];
    bool       t_down[TOUCH_MAX], t_seen[TOUCH_MAX];
    int        t_left;
    bool       dead;
} Hid;

static Hid *g_hids[MAX_HID];

/* usbcheck's device: its events are collected instead of posted */
static const void *g_check_hid;
static InputEvent  g_check_ev[16];
static int         g_check_n;

static void hid_post(const void *h, const InputEvent *ev)
{
    if (h && h == g_check_hid) {
        if (g_check_n < 16) g_check_ev[g_check_n++] = *ev;
        return;
    }
    InputPost(ev);
}

static void post_key(const void *h, UINT16 code, bool pressed)
{
    if (!code) return;
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = INPUT_KEY;
    ev.scancode = (UINT8)(code & 0x7F);
    ev.extended = (code & EXT) ? 1 : 0;
    ev.pressed  = pressed ? 1 : 0;
    hid_post(h, &ev);
}

static UINT16 usage_code(UINT8 u)
{
    if (u >= 0xE0 && u <= 0xE7) return g_mod_to_set1[u - 0xE0];
    if (u == 0x7F) return EXT | KEY_MUTE;                /* Keyboard Mute, Volume Up, Volume Down */
    if (u == 0x80) return EXT | KEY_VOL_UP;
    if (u == 0x81) return EXT | KEY_VOL_DOWN;
    return u < sizeof(g_usage_to_set1) / sizeof(g_usage_to_set1[0]) ? g_usage_to_set1[u] : 0;
}

static inline bool key_held(const UINT8 *m, UINT8 u) { return (m[u >> 3] >> (u & 7)) & 1; }

static void keyboard_state(Hid *h, const UINT8 *now, UINT64 tick)
{
    for (int u = 4; u < 256; u++) {
        bool was = key_held(h->keys, (UINT8)u), is = key_held(now, (UINT8)u);
        if (was == is) continue;
        UINT16 code = usage_code((UINT8)u);
        post_key(h, code, is);
        if (!is && code && h->repeat == code) h->repeat = 0;
        if (is && code && (u < 0xE0 || u > 0xE7)) {
            h->repeat = code;                             /* the newest key repeats */
            h->repeat_at = tick + REPEAT_DELAY;
        }
    }
    memcpy(h->keys, now, sizeof(h->keys));
}

static void keyboard_report(Hid *h, const UINT8 *r, int len, UINT64 tick)
{
    UINT8 now[32];
    memset(now, 0, sizeof(now));
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->id != h->kbd_id || f->page != PAGE_KEYBOARD) continue;
        if (!f->array) {
            if (get_bits(r, len, f->bit, f->size, 0) && f->usage < 256) now[f->usage >> 3] |= (UINT8)(1u << (f->usage & 7));
            continue;
        }
        for (int k = 0; k < f->count; k++) {
            INT32 v = get_bits(r, len, f->bit + k * f->size, f->size, f->lmin);
            if (v < f->lmin || v > f->lmax) continue;
            UINT32 u = (UINT32)(f->usage + (v - f->lmin));
            if (u >= 1 && u <= 3) return;                /* rollover error: keep the previous state */
            if (u > 3 && u < 256) now[u >> 3] |= (UINT8)(1u << (u & 7));
        }
    }
    keyboard_state(h, now, tick);
}

/* Consumer Control (page 0x0C) and System Control (Generic Desktop) usages
 * → E0-prefixed set-1 codes, as Microsoft's keyboard scan code
 * specification lists them for the same keys on PS/2 keyboards */
static const struct { UINT16 page, usage; UINT8 sc; } g_cc_keys[] = {
    { PAGE_CONSUMER, 0x0B5, KEY_NEXT_TRACK },   { PAGE_CONSUMER, 0x0B6, KEY_PREV_TRACK },
    { PAGE_CONSUMER, 0x0B7, KEY_MEDIA_STOP },   { PAGE_CONSUMER, 0x0CD, KEY_PLAY_PAUSE },
    { PAGE_CONSUMER, 0x0E2, KEY_MUTE },         { PAGE_CONSUMER, 0x0E9, KEY_VOL_UP },
    { PAGE_CONSUMER, 0x0EA, KEY_VOL_DOWN },     { PAGE_CONSUMER, 0x183, KEY_MEDIA_SELECT },
    { PAGE_CONSUMER, 0x18A, KEY_MAIL },         { PAGE_CONSUMER, 0x192, KEY_CALCULATOR },
    { PAGE_CONSUMER, 0x194, KEY_MY_COMPUTER },  { PAGE_CONSUMER, 0x221, KEY_WWW_SEARCH },
    { PAGE_CONSUMER, 0x223, KEY_WWW_HOME },     { PAGE_CONSUMER, 0x224, KEY_WWW_BACK },
    { PAGE_CONSUMER, 0x225, KEY_WWW_FORWARD },  { PAGE_CONSUMER, 0x226, KEY_WWW_STOP },
    { PAGE_CONSUMER, 0x227, KEY_WWW_REFRESH },  { PAGE_CONSUMER, 0x22A, KEY_WWW_FAVORITES },
    { PAGE_DESKTOP,  0x081, KEY_POWER },        { PAGE_DESKTOP,  0x082, KEY_SLEEP },
    { PAGE_DESKTOP,  0x083, KEY_WAKE },
};

static UINT16 cc_code(UINT16 page, UINT32 usage)
{
    for (unsigned i = 0; i < sizeof(g_cc_keys) / sizeof(g_cc_keys[0]); i++)
        if (g_cc_keys[i].page == page && g_cc_keys[i].usage == usage) return EXT | g_cc_keys[i].sc;
    return 0;
}

/* A field that carries media or system keys: an array of consumer or
 * system-control usages, or one such usage as an on/off bit */
static bool cc_field(const HidField *f)
{
    if (f->page == PAGE_CONSUMER) return f->array || (!f->relative && f->usage != USAGE_AC_PAN);
    if (f->page == PAGE_DESKTOP) return f->array ? f->usage_max >= 0x81 && f->usage <= 0x83 : f->usage >= 0x81 && f->usage <= 0x83;
    return false;
}

static void cc_add(UINT16 *set, int *n, UINT16 code)
{
    if (!code || *n >= MAX_CC) return;
    for (int i = 0; i < *n; i++) if (set[i] == code) return;
    set[(*n)++] = code;
}

static bool cc_has(const UINT16 *set, int n, UINT16 code)
{
    for (int i = 0; i < n; i++) if (set[i] == code) return true;
    return false;
}

/* Media and system keys: the report lists the ones held now */
static void media_report(Hid *h, UINT8 id, const UINT8 *r, int len, UINT64 tick)
{
    UINT16 now[MAX_CC];
    int nnow = 0;
    bool mine = false;
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->id != id || !cc_field(f)) continue;
        mine = true;
        if (!f->array) {
            if (get_bits(r, len, f->bit, f->size, 0)) cc_add(now, &nnow, cc_code(f->page, f->usage));
            continue;
        }
        for (int k = 0; k < f->count; k++) {
            INT32 v = get_bits(r, len, f->bit + k * f->size, f->size, f->lmin);
            if (v < f->lmin || v > f->lmax) continue;
            UINT32 u = f->usage + (UINT32)(v - f->lmin);
            if (u && u <= f->usage_max) cc_add(now, &nnow, cc_code(f->page, u));
        }
    }
    if (!mine) return;                               /* (a report about something else) */
    /* Keys this report no longer lists go up; those held through other
     * reports stay */
    UINT16 keep[MAX_CC];
    UINT8 keep_id[MAX_CC];
    int nkeep = 0;
    for (int i = 0; i < h->ncc; i++) {
        if (h->cc_id[i] != id) { keep[nkeep] = h->cc[i]; keep_id[nkeep++] = h->cc_id[i]; continue; }
        if (!cc_has(now, nnow, h->cc[i])) {
            post_key(h, h->cc[i], false);
            if (h->repeat == h->cc[i]) h->repeat = 0;
        }
    }
    for (int i = 0; i < nnow; i++)
        if (!cc_has(h->cc, h->ncc, now[i])) {
            post_key(h, now[i], true);
            UINT8 sc = (UINT8)now[i];
            if (sc == KEY_VOL_UP || sc == KEY_VOL_DOWN) {   /* held volume keys repeat, as on Windows */
                h->repeat = now[i];
                h->repeat_at = tick + REPEAT_DELAY;
            }
        }
    for (int i = 0; i < nnow && nkeep < MAX_CC; i++)
        if (!cc_has(keep, nkeep, now[i])) { keep[nkeep] = now[i]; keep_id[nkeep++] = id; }
    memcpy(h->cc, keep, sizeof(keep));
    memcpy(h->cc_id, keep_id, sizeof(keep_id));
    h->ncc = nkeep;
}

static INT32 scale_abs(INT32 v, INT32 lmin, INT32 lmax)
{
    if (lmax <= lmin) return 0;
    if (v < lmin) v = lmin;
    if (v > lmax) v = lmax;
    return (INT32)((INT64)(v - lmin) * 65535 / (lmax - lmin));
}

static void pointer_report(Hid *h, UINT8 id, const UINT8 *r, int len)
{
    INT32 x = 0, y = 0, wheel = 0, hwheel = 0;
    bool have_x = false, have_y = false, any = false, abs = false;
    UINT8 buttons = 0;
    bool tip_seen = false;
    /* a pen's: pressure (0..TABLET_PRESSURE), barrel buttons, in range, eraser */
    INT32 pressure = 0;
    UINT8 pen_buttons = 0;
    bool in_range = false, eraser = false, pen_seen = false, have_pressure = false;
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->id != id || f->array) continue;
        INT32 v = get_bits(r, len, f->bit, f->size, f->lmin);
        if (h->pen && f->page == PAGE_DIGITIZER) {
            pen_seen = true;
            switch (f->usage) {
            case 0x30: have_pressure = true; pressure = f->lmax > f->lmin ? (INT32)((INT64)(v < f->lmin ? 0 : v - f->lmin) * TABLET_PRESSURE / (f->lmax - f->lmin)) : 0; break;
            case 0x32: in_range = v != 0; break;                          /* In Range */
            case 0x44: if (v) { pen_buttons |= 2; buttons |= MOUSE_RIGHT; } break;   /* Barrel Switch */
            case 0x5A: if (v) pen_buttons |= 4; break;                     /* Secondary Barrel Switch */
            case 0x3C: if (v) eraser = true; break;                        /* Invert: the eraser end is near */
            case 0x45: if (v) { eraser = true; pen_buttons |= 1; buttons |= MOUSE_LEFT; any = true; } break;   /* Eraser touching */
            }
        }
        if (f->page == PAGE_BUTTON && f->usage >= 1 && f->usage <= 5) {
            if (v) buttons |= (UINT8)(1u << (f->usage - 1));         /* 1 left, 2 right, 3 middle, 4 back, 5 forward */
            any = true;
        } else if (f->page == PAGE_DIGITIZER && f->usage == 0x42 && !tip_seen) {   /* tip switch: the first contact */
            tip_seen = true;
            if (v) { buttons |= MOUSE_LEFT; pen_buttons |= 1; }
            any = true;
        } else if (f->page == PAGE_DESKTOP && f->usage == 0x30 && !have_x) {
            have_x = true; any = true;
            if (f->relative) x = v; else { x = scale_abs(v, f->lmin, f->lmax); abs = true; }
        } else if (f->page == PAGE_DESKTOP && f->usage == 0x31 && !have_y) {
            have_y = true; any = true;
            if (f->relative) y = v; else { y = scale_abs(v, f->lmin, f->lmax); abs = true; }
        } else if (f->page == PAGE_DESKTOP && f->usage == 0x38) {
            wheel = v; any = true;
        } else if (f->page == PAGE_CONSUMER && f->usage == USAGE_AC_PAN) {
            hwheel = v; any = true;                                   /* + is to the right */
        }
    }
    if (!any) return;
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = INPUT_MOUSE;
    ev.buttons  = buttons;
    ev.dx       = x;
    ev.dy       = y;                         /* USB +y is already down */
    ev.dz       = wheel;                     /* +1 per notch away from the user */
    ev.dw       = hwheel;
    ev.absolute = abs && have_x && have_y;
    if (abs && !ev.absolute) { ev.dx = ev.dy = 0; }  /* (half an absolute position: buttons only) */
    hid_post(h, &ev);
    if (pen_seen && ev.absolute) {
        InputEvent pe;
        memset(&pe, 0, sizeof(pe));
        pe.type     = INPUT_PEN;
        pe.absolute = 1;
        pe.dx = x; pe.dy = y;
        pe.pressure = (UINT16)(!(pen_buttons & 1) ? 0 : have_pressure ? pressure : TABLET_PRESSURE);
        pe.buttons  = pen_buttons;
        pe.pressed  = in_range || pen_buttons;      /* (pens without In Range: while touching) */
        pe.extended = eraser;
        hid_post(h, &pe);
    }
}

/* ---- multi-touch (HID digitizers: Windows' touch-screen descriptors) ----
 *
 * Each contact has a logical collection (a "finger") with its Tip Switch,
 * Contact Identifier and X/Y; the report also carries the Contact Count.
 * In "hybrid" mode a frame with more contacts than a report has fingers
 * comes in several reports, the first with the count and the rest with 0.
 * Every contact becomes an INPUT_TOUCH event in a slot of its own; a
 * contact that a finished frame left out has lifted. */

static void post_touch(Hid *h, int slot, bool down, INT32 x, INT32 y)
{
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = INPUT_TOUCH;
    ev.contact  = (UINT8)slot;
    ev.pressed  = down ? 1 : 0;
    ev.absolute = 1;
    ev.dx = x; ev.dy = y;
    hid_post(h, &ev);
}

static void touch_frame_end(Hid *h)
{
    for (int s = 0; s < TOUCH_MAX; s++) {
        if (h->t_down[s] && !h->t_seen[s]) { post_touch(h, s, false, 0, 0); h->t_down[s] = false; }
        if (!h->t_down[s]) h->t_cid[s] = -1;
        h->t_seen[s] = false;
    }
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_TOUCH;
    ev.contact = TOUCH_FRAME;
    hid_post(h, &ev);
}

/* The field of @usage (@page) in finger collection @coll of report @id */
static const HidField *finger_field(const Hid *h, UINT8 id, UINT8 coll, UINT16 page, UINT16 usage)
{
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->id == id && f->coll == coll && f->page == page && f->usage == usage && !f->array) return f;
    }
    return NULL;
}

static void touch_report(Hid *h, UINT8 id, const UINT8 *r, int len)
{
    /* The fingers of this report: the collections that have a tip switch */
    UINT8 fingers[TOUCH_MAX];
    int nf = 0;
    const HidField *count = NULL;
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->id != id || f->page != PAGE_DIGITIZER || f->array) continue;
        if (f->usage == 0x54) count = f;                                        /* Contact Count */
        if (f->usage == 0x42 && nf < TOUCH_MAX) fingers[nf++] = f->coll;          /* Tip Switch */
    }
    if (!nf) return;
    int valid = nf;
    if (count) {
        INT32 c = get_bits(r, len, count->bit, count->size, 0);
        if (c > 0) h->t_left = c;                                 /* a new frame */
        else if (h->t_left <= 0) h->t_left = nf;                  /* (no count: a whole frame) */
        valid = h->t_left < nf ? h->t_left : nf;
    } else {
        h->t_left = nf;
    }
    for (int k = 0; k < valid; k++) {
        const HidField *tip = finger_field(h, id, fingers[k], PAGE_DIGITIZER, 0x42);
        const HidField *cid = finger_field(h, id, fingers[k], PAGE_DIGITIZER, 0x51);
        const HidField *fx = finger_field(h, id, fingers[k], PAGE_DESKTOP, 0x30);
        const HidField *fy = finger_field(h, id, fingers[k], PAGE_DESKTOP, 0x31);
        if (!tip || !fx || !fy) continue;
        bool down = get_bits(r, len, tip->bit, tip->size, 0) != 0;
        INT32 c = cid ? get_bits(r, len, cid->bit, cid->size, 0) : k;
        int slot = -1;
        for (int s = 0; s < TOUCH_MAX && slot < 0; s++) if (h->t_cid[s] == c) slot = s;
        if (slot < 0) {
            if (!down) continue;                                  /* (an unused finger) */
            for (int s = 0; s < TOUCH_MAX && slot < 0; s++) if (h->t_cid[s] < 0) slot = s;
            if (slot < 0) continue;
            h->t_cid[slot] = c;
        }
        h->t_seen[slot] = true;
        if (!down && !h->t_down[slot]) continue;
        INT32 x = scale_abs(get_bits(r, len, fx->bit, fx->size, fx->lmin), fx->lmin, fx->lmax);
        INT32 y = scale_abs(get_bits(r, len, fy->bit, fy->size, fy->lmin), fy->lmin, fy->lmax);
        post_touch(h, slot, down, x, y);
        h->t_down[slot] = down;
    }
    h->t_left -= valid;
    if (h->t_left <= 0) touch_frame_end(h);
}

static bool on_report(UsbPipe *p, const UINT8 *data, int len, void *ctx)
{
    Hid *h = ctx;
    if (h->dead) return false;
    UINT8 id = 0;
    if (h->L.ids) {
        if (len < 1) return true;
        id = data[0];
        data++; len--;
    }
    if (h->keyboard && id == h->kbd_id) keyboard_report(h, data, len, sched_ticks());
    if (h->media) media_report(h, id, data, len, sched_ticks());
    if (h->touch) touch_report(h, id, data, len);
    else if (h->pointer) pointer_report(h, id, data, len);
    return true;
}

void UsbHidTickAll(UINT64 now)
{
    for (int i = 0; i < MAX_HID; i++) {
        Hid *h = g_hids[i];
        if (!h || h->dead || !h->repeat || now < h->repeat_at) continue;
        post_key(h, h->repeat, true);                   /* a make without a break, as PS/2 sends */
        h->repeat_at = now + REPEAT_RATE;
    }
}

/* Keep keyboard LEDs in step with the lock keys (usb thread) */
void UsbHidSyncLeds(void)
{
    UINT32 want = InputLockState();
    for (int i = 0; i < MAX_HID; i++) {
        Hid *h = g_hids[i];
        if (!h || h->dead || !h->keyboard || h->leds == want || !h->L.led_bytes) continue;
        bool any = false;
        UINT8 rep[9];
        memset(rep, 0, sizeof(rep));
        int at = h->L.led_id ? 1 : 0;                      /* (the report ID comes first) */
        rep[0] = h->L.led_id;
        for (int k = 0; k < 3; k++) {
            int bit = h->L.led_bit[k];
            if (bit < 0 || bit / 8 >= 8) continue;
            any = true;
            if (want & (1u << k)) rep[at + bit / 8] |= (UINT8)(1u << (bit % 8));
        }
        h->leds = (UINT8)want;
        if (!any) continue;
        int len = at + (h->L.led_bytes > 8 ? 8 : h->L.led_bytes);
        if (UsbControl(h->dev, 0x21, 0x09, (UINT16)(0x0200 | h->L.led_id), h->iface, (UINT16)len, rep) < 0)   /* SET_REPORT(output) */
            kprintf("[USB] %s: keyboard LEDs not set\n", UsbDevName(h->dev));
    }
}

static void hid_gone(void *inst)
{
    Hid *h = inst;
    if (h->keyboard) {                               /* release every key it still holds */
        UINT8 none[32];
        memset(none, 0, sizeof(none));
        keyboard_state(h, none, 0);
    }
    for (int i = 0; i < h->ncc; i++) post_key(h, h->cc[i], false);
    h->ncc = 0;
    if (h->pen) TabletDevice(NULL, -1);
    if (h->touch) {                                  /* lift every contact */
        for (int s = 0; s < TOUCH_MAX; s++) h->t_seen[s] = false;
        touch_frame_end(h);
    }
    h->repeat = 0;
    h->dead = true;
    for (int i = 0; i < MAX_HID; i++)
        if (g_hids[i] == h) g_hids[i] = NULL;
    kprintf("[USB] %s: %s removed\n", UsbDevName(h->dev), h->keyboard ? "keyboard" : h->pointer ? "pointer" : "media keys");
    /* (the Hid stays allocated: a report may still be on its way) */
}

static void classify(Hid *h)
{
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->page == PAGE_KEYBOARD && !h->keyboard) { h->keyboard = true; h->kbd_id = f->id; }
        if (cc_field(f)) h->media = true;
        if (f->page == PAGE_DIGITIZER && f->usage == 0x51) h->touch = true;    /* Contact Identifier */
        if (f->page == PAGE_DIGITIZER && f->usage == 0x30) h->pen = true;      /* Tip Pressure */
        if (f->page == PAGE_DESKTOP && (f->usage == 0x30 || f->usage == 0x31)) {
            h->pointer = true;
            if (!f->relative) h->absolute = true;
        }
    }
    if (h->L.app == 0x0D02) h->pen = true;                                    /* a Pen collection */
    if (h->touch) { h->pen = false; for (int s = 0; s < TOUCH_MAX; s++) h->t_cid[s] = -1; }
}

void *UsbHidProbe(UsbDev *d, const UsbIface *f)
{
    int slot = -1;
    for (int i = 0; i < MAX_HID && slot < 0; i++) if (!g_hids[i]) slot = i;
    if (slot < 0) return NULL;

    int off = 0;
    const UINT8 *hd = UsbIfaceFind(f, USB_DT_HID, &off);
    off = 0;
    const UINT8 *ep = NULL;
    for (const UINT8 *e; (e = UsbIfaceFind(f, USB_DT_ENDPOINT, &off)) != NULL;)
        if ((e[2] & 0x80) && (e[3] & 3) == 3) { ep = e; break; }
    if (!ep) return NULL;

    Hid *h = kzalloc(sizeof(Hid));
    if (!h) return NULL;
    h->dev = d;
    h->iface = f->number;
    h->leds = 0xFF;

    /* Report protocol, from the report descriptor */
    bool ok = false;
    if (hd && hd[0] >= 9) {
        UINT16 rlen = (UINT16)(hd[7] | hd[8] << 8);
        UINT8 *desc = rlen ? kmalloc(rlen) : NULL;
        if (desc && UsbControl(d, 0x81, 6, USB_DT_REPORT << 8, f->number, rlen, desc) > 0) {
            ok = parse_report_desc(desc, rlen, &h->L);
            if (ok) classify(h);
            ok = ok && (h->keyboard || h->pointer || h->media);
            if (ok && f->sub == 1) UsbControl(d, 0x21, 0x0B, 1, f->number, 0, NULL);   /* SET_PROTOCOL(report) */
        }
        kfree(desc);
    }
    /* Or boot protocol */
    if (!ok && f->sub == 1 && (f->proto == 1 || f->proto == 2)) {
        if (f->proto == 1) parse_report_desc(g_boot_kbd_desc, sizeof(g_boot_kbd_desc), &h->L);
        else               parse_report_desc(g_boot_mouse_desc, sizeof(g_boot_mouse_desc), &h->L);
        classify(h);
        h->boot = true;
        UsbControl(d, 0x21, 0x0B, 0, f->number, 0, NULL);                         /* SET_PROTOCOL(boot) */
        ok = true;
    }
    if (!ok) { kfree(h); return NULL; }
    if (h->keyboard) UsbControl(d, 0x21, 0x0A, 0, f->number, 0, NULL);           /* SET_IDLE(0): reports on change only */
    if (h->keyboard) {                    /* a key may wake the machine (if the device can) */
        UINT8 cfg[9];
        if (UsbControl(d, 0x80, 6, USB_DT_CONFIG << 8, 0, sizeof(cfg), cfg) == sizeof(cfg) && (cfg[7] & 0x20))
            h->wake = UsbControl(d, 0x00, 3, 1, 0, 0, NULL) >= 0;               /* SET_FEATURE(DEVICE_REMOTE_WAKEUP) */
    }

    UINT16 mps = (UINT16)((ep[4] | ep[5] << 8) & 0x7FF);
    h->pipe = UsbOpenPipe(d, ep, mps);
    if (!h->pipe) { kfree(h); return NULL; }
    g_hids[slot] = h;
    UsbBind(d, h, hid_gone);
    if (h->touch) {                       /* contacts: the Contact Count's maximum, or the fingers a report has */
        int fingers = 0, most = 0;
        for (int i = 0; i < h->L.n; i++) {
            const HidField *t = &h->L.f[i];
            if (t->page != PAGE_DIGITIZER || t->array) continue;
            if (t->usage == 0x42 && t->id == h->L.f[0].id) fingers++;
            if (t->usage == 0x54 && t->lmax > most) most = t->lmax;
        }
        InputTouchScreen(most > fingers ? most : fingers);
    }
    if (h->pen) TabletDevice(NULL, 1);
    const char *kind = h->touch ? "multi-touch screen" : h->pen ? "pen tablet" :
                       h->keyboard && h->pointer ? "keyboard + pointer" : h->keyboard ? "keyboard" :
                       !h->pointer ? "media keys" :
                       h->absolute ? (h->L.app == 0x0D04 ? "touch screen" : "absolute pointer") : "mouse";
    kprintf("[USB] %s: %s (%s protocol, %d fields%s%s)\n", UsbDevName(d), kind,
            h->boot ? "boot" : "report", h->L.n, h->media && (h->keyboard || h->pointer) ? ", media keys" : "",
            h->wake ? ", wakes the machine" : "");
    UsbPipeListen(h->pipe, mps, on_report, h);
    return h;
}

/* ---------------------------------------------------------------------------
 * usbcheck: the report parser against devices QEMU doesn't have
 *
 * Report descriptors of the kinds real keyboards and mice send (media keys
 * as a consumer-control array and as bits, system-control keys, a mouse
 * with five buttons, a wheel and AC Pan), with reports fed through the
 * same code a device's reports go through; the events they make are
 * compared with what they should be.
 * ------------------------------------------------------------------------- */

static const UINT8 g_chk_cc_array[] = {     /* consumer array (ID 2), system control (ID 3) */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x19, 0x00, 0x2A, 0x3C, 0x02, 0x15, 0x00, 0x26,
    0x3C, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xC0,
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x03, 0x19, 0x81, 0x29, 0x83, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0xC0,
};
static const UINT8 g_chk_cc_bits[] = {      /* eight media keys as bits (ID 1) */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7, 0x09, 0xCD, 0x09, 0xE2, 0x09, 0xE9, 0x09, 0xEA, 0x0A, 0x23,
    0x02, 0x81, 0x02, 0xC0,
};
static const UINT8 g_chk_mouse[] = {        /* five buttons, X, Y, wheel, AC Pan */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};
static const UINT8 g_chk_kbd[] = {          /* a keyboard whose keys go up to usage 0xFF (QEMU's) */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0xFF, 0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00, 0xC0,
};

#define FINGER 0x09, 0x22, 0xA1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, \
               0x75, 0x07, 0x81, 0x03, 0x09, 0x51, 0x25, 0x7F, 0x75, 0x08, 0x81, 0x02, 0x05, 0x01, 0x26, 0xFF, \
               0x7F, 0x75, 0x10, 0x09, 0x30, 0x09, 0x31, 0x95, 0x02, 0x81, 0x02, 0x05, 0x0D, 0x95, 0x01, 0xC0
static const UINT8 g_chk_touch[] = {        /* a touch screen, two fingers a report (hybrid mode) */
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01, FINGER, FINGER,
    0x09, 0x54, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0,
};
#undef FINGER
static const UINT8 g_chk_pen[] = {          /* a pen: tip, barrel, eraser, invert, in range, X, Y, pressure (ID 2) */
    0x05, 0x0D, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x20, 0xA1, 0x00,
    0x09, 0x42, 0x09, 0x44, 0x09, 0x45, 0x09, 0x3C, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x05, 0x81, 0x02, 0x95, 0x03, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
    0x05, 0x0D, 0x09, 0x30, 0x26, 0xFF, 0x0F, 0x81, 0x02, 0xC0, 0xC0,
};

typedef struct { const char *what; const UINT8 *desc; int dlen; const char *kind; UINT8 rep[4][16]; int rlen; int nrep;
                 const char *want; } HidCheck;

/* What a check's events were, as text: "+E0 30 -E0 30" for keys, "m18,0,-1" for buttons, wheel, h-wheel */
static void describe(char *out, int cap)
{
    int n = 0;
    out[0] = '\0';
    for (int i = 0; i < g_check_n && n < cap - 1; i++) {
        const InputEvent *e = &g_check_ev[i];
        if (e->type == INPUT_TOUCH && e->contact == TOUCH_FRAME)
            n += ksnprintf(out + n, cap - n, "%s|", i ? " " : "");
        else if (e->type == INPUT_TOUCH && e->pressed)
            n += ksnprintf(out + n, cap - n, "%st%d+%d", i ? " " : "", e->contact, e->dx * 100 / 65536);
        else if (e->type == INPUT_TOUCH)
            n += ksnprintf(out + n, cap - n, "%st%d-", i ? " " : "", e->contact);
        else if (e->type == INPUT_PEN)
            n += ksnprintf(out + n, cap - n, "%sp%d,%X%s%s", i ? " " : "", e->pressure, e->buttons,
                           e->pressed ? "r" : "", e->extended ? "e" : "");
        else if (e->type == INPUT_KEY)
            n += ksnprintf(out + n, cap - n, "%s%c%s%02X", i ? " " : "", e->pressed ? '+' : '-', e->extended ? "E0 " : "", e->scancode);
        else
            n += ksnprintf(out + n, cap - n, "%sm%X,%d,%d", i ? " " : "", e->buttons, e->dz, e->dw);
    }
}

int UsbHidSelfCheck(void (*say)(void *ctx, const char *line), void *ctx)
{
    static const HidCheck checks[] = {
        { "consumer-control array", g_chk_cc_array, sizeof(g_chk_cc_array), "media",
          { { 2, 0xE9, 0 }, { 2, 0, 0 }, { 2, 0xCD, 0 }, { 2, 0x24, 0x02 }, }, 3, 4,
          "+E0 30 -E0 30 +E0 22 -E0 22 +E0 6A" },
        { "system-control keys", g_chk_cc_array, sizeof(g_chk_cc_array), "media",
          { { 3, 0x02 }, { 3, 0x00 }, { 3, 0x01 }, { 2, 0, 0 } }, 2, 4,
          "+E0 5F -E0 5F +E0 5E" },
        { "consumer-control bits", g_chk_cc_bits, sizeof(g_chk_cc_bits), "media",
          { { 1, 0x20 }, { 1, 0x30 }, { 1, 0x00 }, { 1, 0x80 } }, 2, 4,
          "+E0 30 +E0 20 -E0 20 -E0 30 +E0 32" },
        { "mouse buttons 4, 5 and AC Pan", g_chk_mouse, sizeof(g_chk_mouse), "pointer",
          { { 0x08, 0, 0, 0, 0xFF }, { 0x18, 0, 0, 1, 1 }, { 0x00 } }, 5, 3,
          "m8,0,-1 m18,1,1 m0,0,0" },
        { "keyboard volume usages", g_chk_kbd, sizeof(g_chk_kbd), "keyboard",
          { { 0, 0, 0x80 }, { 0, 0, 0x81, 0x80 }, { 0, 0, 0x7F }, { 0 } }, 8, 4,
          "+E0 30 +E0 2E +E0 20 -E0 30 -E0 2E -E0 20" },
        /* three contacts over two reports, then one lifts, one moves and
         * one is left out (lifted) */
        { "multi-touch (hybrid reports)", g_chk_touch, sizeof(g_chk_touch), "touch",
          { { 1, 1, 5, 0x00, 0x40, 0x00, 0x20, 1, 7, 0x00, 0x20, 0x00, 0x10, 3 },
            { 1, 1, 9, 0x00, 0x60, 0x00, 0x30, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 0, 5, 0x00, 0x40, 0x00, 0x20, 1, 7, 0x00, 0x30, 0x00, 0x10, 2 } }, 14, 3,
          "t0+50 t1+25 t2+75 | t0- t1+37 t2- |" },
        /* hovering, the tip at half pressure with the barrel button, the
         * eraser pressed hard, out of range */
        { "pen pressure, barrel and eraser", g_chk_pen, sizeof(g_chk_pen), "pen",
          { { 2, 0x10, 0x00, 0x40, 0x00, 0x20, 0x00, 0x00 }, { 2, 0x13, 0x00, 0x40, 0x00, 0x20, 0x00, 0x08 },
            { 2, 0x1C, 0x00, 0x40, 0x00, 0x20, 0xFF, 0x0F }, { 2, 0x00, 0x00, 0x40, 0x00, 0x20, 0x00, 0x00 } }, 8, 4,
          "m0,0,0 p0,0r m3,0,0 p511,3r m1,0,0 p1023,1re m0,0,0 p0,0" },
    };
    int failed = 0;
    for (unsigned c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
        const HidCheck *k = &checks[c];
        Hid *h = kzalloc(sizeof(Hid));
        if (!h) return -1;
        h->leds = 0xFF;
        bool ok = parse_report_desc(k->desc, k->dlen, &h->L);
        if (ok) classify(h);
        const char *kind = h->touch ? "touch" : h->pen ? "pen" : h->keyboard ? "keyboard" : h->pointer ? "pointer" : h->media ? "media" : "nothing";
        char got[160], line[256];
        if (!ok || strcmp(kind, k->kind) != 0) {
            ksnprintf(line, sizeof(line), "FAIL %s: read as %s, not %s", k->what, kind, k->kind);
            say(ctx, line);
            failed++;
            kfree(h);
            continue;
        }
        g_check_n = 0;
        __atomic_store_n(&g_check_hid, h, __ATOMIC_RELEASE);
        for (int r = 0; r < k->nrep; r++) on_report(NULL, k->rep[r], k->rlen, h);
        __atomic_store_n(&g_check_hid, NULL, __ATOMIC_RELEASE);
        describe(got, sizeof(got));
        bool pass = strcmp(got, k->want) == 0;
        if (pass) ksnprintf(line, sizeof(line), "ok   %s: %s", k->what, got);
        else      ksnprintf(line, sizeof(line), "FAIL %s: %s (want %s)", k->what, got, k->want);
        say(ctx, line);
        if (!pass) failed++;
        kfree(h);
    }
    return failed;
}
