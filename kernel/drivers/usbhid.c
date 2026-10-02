/*
 * usbhid.c — USB HID keyboards, mice, tablets and touch screens
 *
 * Devices run in report protocol: the interface's report descriptor (HID
 * 1.11, section 6.2.2) says where each control sits in the input reports,
 * and the parser below turns it into a list of fields.  Keyboards report
 * the keys held down (an array of usages, or a bitmap), mice relative X/Y,
 * wheel and buttons, tablets and touch screens absolute X/Y with a button
 * or a tip switch.  A boot-class device whose report descriptor can't be
 * read or understood is put in boot protocol and parsed with the boot
 * descriptors from appendix B instead.
 *
 * Keys become set-1 scancodes (the codes a PS/2 keyboard sends), so
 * everything above the drivers sees one kind of keyboard.  USB keyboards
 * do not repeat keys themselves; UsbHidTickAll() does it.
 */

#include "usb.h"
#include "../wm/input.h"
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
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
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
#define PAGE_DIGITIZER 0x0D

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
} HidField;

typedef struct {
    HidField f[MAX_FIELDS];
    int      n;
    bool     ids;              /* reports start with an ID byte */
    UINT16   app;              /* the first application collection: page << 8 | usage */
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
    UINT16 bits[256];                   /* input bits so far, per report ID */
    memset(bits, 0, sizeof(bits));
    int depth = 0;

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
        case 0x90: case 0xB0:                            /* Output, Feature */
            nusage = 0; have_range = false;
            break;
        case 0xA0:                                       /* Collection */
            if (depth == 0 && item_u(v, n) == 1 && !L->app && nusage)
                L->app = (UINT16)((((usages[0] >> 16) ? (usages[0] >> 16) : g.page) << 8) | (usages[0] & 0xFF));
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

typedef struct {
    UsbDev    *dev;
    UsbPipe   *pipe;
    HidLayout  L;
    bool       keyboard, pointer, absolute, boot;
    UINT8      kbd_id;               /* the report the keys come in */
    UINT8      keys[32];             /* keyboard usages held down (bitmap) */
    UINT8      repeat;
    UINT64     repeat_at;
    UINT8      buttons;
    bool       dead;
} Hid;

static Hid *g_hids[MAX_HID];

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
    if (u >= 0xE0 && u <= 0xE7) return g_mod_to_set1[u - 0xE0];
    return u < sizeof(g_usage_to_set1) / sizeof(g_usage_to_set1[0]) ? g_usage_to_set1[u] : 0;
}

static inline bool key_held(const UINT8 *m, UINT8 u) { return (m[u >> 3] >> (u & 7)) & 1; }

static void keyboard_state(Hid *h, const UINT8 *now, UINT64 tick)
{
    for (int u = 4; u < 256; u++) {
        bool was = key_held(h->keys, (UINT8)u), is = key_held(now, (UINT8)u);
        if (was == is) continue;
        post_key(usage_code((UINT8)u), is);
        if (!is && h->repeat == u) h->repeat = 0;
        if (is && (u < 0xE0 || u > 0xE7)) {
            h->repeat = (UINT8)u;                         /* the newest key repeats */
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

static INT32 scale_abs(INT32 v, INT32 lmin, INT32 lmax)
{
    if (lmax <= lmin) return 0;
    if (v < lmin) v = lmin;
    if (v > lmax) v = lmax;
    return (INT32)((INT64)(v - lmin) * 65535 / (lmax - lmin));
}

static void pointer_report(Hid *h, UINT8 id, const UINT8 *r, int len)
{
    INT32 x = 0, y = 0, wheel = 0;
    bool have_x = false, have_y = false, any = false, abs = false;
    UINT8 buttons = 0;
    bool tip_seen = false;
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->id != id || f->array) continue;
        INT32 v = get_bits(r, len, f->bit, f->size, f->lmin);
        if (f->page == PAGE_BUTTON && f->usage >= 1 && f->usage <= 3) {
            if (v) buttons |= (UINT8)(1u << (f->usage - 1));         /* 1 left, 2 right, 3 middle */
            any = true;
        } else if (f->page == PAGE_DIGITIZER && f->usage == 0x42 && !tip_seen) {   /* tip switch: the first contact */
            tip_seen = true;
            if (v) buttons |= MOUSE_LEFT;
            any = true;
        } else if (f->page == PAGE_DESKTOP && f->usage == 0x30 && !have_x) {
            have_x = true; any = true;
            if (f->relative) x = v; else { x = scale_abs(v, f->lmin, f->lmax); abs = true; }
        } else if (f->page == PAGE_DESKTOP && f->usage == 0x31 && !have_y) {
            have_y = true; any = true;
            if (f->relative) y = v; else { y = scale_abs(v, f->lmin, f->lmax); abs = true; }
        } else if (f->page == PAGE_DESKTOP && f->usage == 0x38) {
            wheel = v; any = true;
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
    ev.absolute = abs && have_x && have_y;
    if (abs && !ev.absolute) { ev.dx = ev.dy = 0; }  /* (half an absolute position: buttons only) */
    InputPost(&ev);
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
    if (h->pointer) pointer_report(h, id, data, len);
    return true;
}

void UsbHidTickAll(UINT64 now)
{
    for (int i = 0; i < MAX_HID; i++) {
        Hid *h = g_hids[i];
        if (!h || h->dead || !h->repeat || now < h->repeat_at) continue;
        post_key(usage_code(h->repeat), true);       /* a make without a break, as PS/2 sends */
        h->repeat_at = now + REPEAT_RATE;
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
    h->repeat = 0;
    h->dead = true;
    for (int i = 0; i < MAX_HID; i++)
        if (g_hids[i] == h) g_hids[i] = NULL;
    kprintf("[USB] %s: %s removed\n", UsbDevName(h->dev), h->keyboard ? "keyboard" : "pointer");
    /* (the Hid stays allocated: a report may still be on its way) */
}

static void classify(Hid *h)
{
    for (int i = 0; i < h->L.n; i++) {
        const HidField *f = &h->L.f[i];
        if (f->page == PAGE_KEYBOARD && !h->keyboard) { h->keyboard = true; h->kbd_id = f->id; }
        if (f->page == PAGE_DESKTOP && (f->usage == 0x30 || f->usage == 0x31)) {
            h->pointer = true;
            if (!f->relative) h->absolute = true;
        }
    }
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

    /* Report protocol, from the report descriptor */
    bool ok = false;
    if (hd && hd[0] >= 9) {
        UINT16 rlen = (UINT16)(hd[7] | hd[8] << 8);
        UINT8 *desc = rlen ? kmalloc(rlen) : NULL;
        if (desc && UsbControl(d, 0x81, 6, USB_DT_REPORT << 8, f->number, rlen, desc) > 0) {
            ok = parse_report_desc(desc, rlen, &h->L);
            if (ok) classify(h);
            ok = ok && (h->keyboard || h->pointer);
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

    UINT16 mps = (UINT16)((ep[4] | ep[5] << 8) & 0x7FF);
    h->pipe = UsbOpenPipe(d, ep, mps);
    if (!h->pipe) { kfree(h); return NULL; }
    g_hids[slot] = h;
    UsbBind(d, h, hid_gone);
    const char *kind = h->keyboard && h->pointer ? "keyboard + pointer" : h->keyboard ? "keyboard" :
                       h->absolute ? (h->L.app == 0x0D04 ? "touch screen" : "absolute pointer") : "mouse";
    kprintf("[USB] %s: %s (%s protocol, %d fields)\n", UsbDevName(d), kind,
            h->boot ? "boot" : "report", h->L.n);
    UsbPipeListen(h->pipe, mps, on_report, h);
    return h;
}
