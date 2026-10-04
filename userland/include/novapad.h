/*
 * novapad.h — game controllers as NovaOS's kernel keeps them
 *
 * NtNovaGuiCtl(0, CTL_GAMEPAD, op | n << 8, ptr) reads the controller
 * slots of kernel/drivers/gamepad.h (whose PadInfo and PadState these
 * mirror): xinput1_4.dll, dinput8.dll and the self-test padtest use it.
 * Raw Input (user32), hid.dll, setupapi and cfgmgr32 also use its raw HID
 * side: each controller's report descriptor and input reports, and the
 * controller a HID device handle reads (kernel/um/um_hid.c).
 */
#pragma once
#include <windows.h>
#include <winternl.h>

#define NOVA_CTL_GAMEPAD 33
#define NOVA_PAD_SLOTS   8

#define NOVA_PAD_XBOX360 1
#define NOVA_PAD_XBOXONE 2
#define NOVA_PAD_HID     3

enum { NOVA_PAD_X, NOVA_PAD_Y, NOVA_PAD_Z, NOVA_PAD_RX, NOVA_PAD_RY, NOVA_PAD_RZ, NOVA_PAD_SLIDER, NOVA_PAD_DIAL, NOVA_PAD_AXES };

typedef struct {
    DWORD packet;
    DWORD buttons;                   /* bit n: button n + 1 */
    WORD  axis[NOVA_PAD_AXES];       /* 0..65535; X right, Y down */
    LONG  pov;                       /* hundredths of a degree, -1 centred */
    WORD  xbuttons;                  /* XINPUT_GAMEPAD_* (0x0400 Guide) */
    BYTE  lt, rt;
    SHORT lx, ly, rx, ry;
} NovaPadState;

typedef struct {
    DWORD serial;                    /* 0: empty */
    WORD  vid, pid;
    BYTE  kind;
    signed char xuser;
    BYTE  buttons, axes, povs, rumble;
    WORD  usage;
    char  name[64];
} NovaPadInfo;

#define NOVA_PAD_DESC_MAX   1024
#define NOVA_PAD_REPORT_MAX 64

/* An input report (gamepad.h's PadRaw): data[0] is its report ID (0 when
 * the device uses none), len the device's input report length */
typedef struct {
    DWORD seq;
    DWORD serial;
    BYTE  slot, len;
    WORD  reserved;
    BYTE  data[NOVA_PAD_REPORT_MAX];
} NovaPadRaw;

/* The mouse's and the keyboard's raw input come through the same ring as
 * reports of these two slots (serial 1), each a NovaRawMouse or a
 * NovaRawKey (gamepad.h's PadRawMouse and PadRawKey) */
#define NOVA_RAW_MOUSE    NOVA_PAD_SLOTS
#define NOVA_RAW_KEYBOARD (NOVA_PAD_SLOTS + 1)
typedef struct {
    USHORT flags, button_flags;       /* RAWMOUSE's usFlags, usButtonFlags */
    SHORT  button_data;               /* ... usButtonData */
    USHORT reserved;
    ULONG  raw_buttons;               /* ... ulRawButtons */
    LONG   x, y;                      /* ... lLastX, lLastY */
} NovaRawMouse;
typedef struct {
    USHORT make, flags, vkey, reserved;   /* RAWKEYBOARD's MakeCode, Flags, VKey */
    ULONG  message;                   /* ... Message */
} NovaRawKey;

typedef struct {
    DWORD after, known_changes, wait_ms, max;     /* in */
    DWORD newest, changes;                        /* out */
    NovaPadRaw r[32];
} NovaPadRead;

static inline DWORD nova_pad_present(void)
{
    return (DWORD)NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 0, NULL);
}

static inline BOOL nova_pad_info(int slot, NovaPadInfo *i)
{
    return NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 1 | (ULONG_PTR)slot << 8, i) != 0;
}

static inline BOOL nova_pad_state(int slot, NovaPadState *s)
{
    return NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 2 | (ULONG_PTR)slot << 8, s) != 0;
}

static inline BOOL nova_pad_rumble(int slot, WORD left, WORD right)
{
    WORD m[2] = { left, right };
    return NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 3 | (ULONG_PTR)slot << 8, m) != 0;
}

/* the slot of XInput user @user, or -1 */
static inline int nova_pad_xinput_slot(int user)
{
    return (int)NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 4 | (ULONG_PTR)user << 8, NULL);
}

/* The slot's HID report descriptor into @buf (NOVA_PAD_DESC_MAX bytes):
 * its length (0: none), *@in_len its input report length */
static inline int nova_pad_descriptor(int slot, BYTE *buf, WORD *in_len)
{
    ULONG_PTR r = (ULONG_PTR)NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 5 | (ULONG_PTR)slot << 8, buf);
    if (in_len) *in_len = (WORD)(r >> 16);
    return (int)(r & 0xFFFF);
}

/* The reports after q->after (up to q->max), waiting up to q->wait_ms for
 * one or for a controller to come or go (q->changes != q->known_changes) */
static inline int nova_pad_read(NovaPadRead *q)
{
    return (int)NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 6, q);
}

/* The slot's latest input report with ID @id */
static inline BOOL nova_pad_last(int slot, BYTE id, NovaPadRaw *r)
{
    return NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 7 | (ULONG_PTR)slot << 8 | (ULONG_PTR)id << 16, r) != 0;
}

/* The slot a HID device handle reads, or -1 */
static inline int nova_pad_handle_slot(HANDLE h)
{
    return (int)NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 8, h);
}

static inline BOOL nova_pad_flush(HANDLE h)
{
    return NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 9, h) != 0;
}

/* The slot of the controller with this serial, or -1 (unplugged) */
static inline int nova_pad_slot_of_serial(DWORD serial)
{
    return (int)NtNovaGuiCtl(0, NOVA_CTL_GAMEPAD, 10 | (ULONG_PTR)serial << 8, NULL);
}

/* A controller's HID path (kernel/um/um_hid.c), upper case as Raw Input
 * and setupapi give it or lower case as DirectInput does: at most 96
 * characters with the NUL */
static inline int nova_pad_path(const NovaPadInfo *i, WCHAR *out, BOOL lower)
{
    static const char hex[] = "0123456789ABCDEF";
    char a[96];
    int n = 0;
    const char *p1 = "\\\\?\\HID#VID_";
    for (const char *c = p1; *c; c++) a[n++] = *c;
    for (int k = 12; k >= 0; k -= 4) a[n++] = hex[(i->vid >> k) & 15];
    for (const char *c = "&PID_"; *c; c++) a[n++] = *c;
    for (int k = 12; k >= 0; k -= 4) a[n++] = hex[(i->pid >> k) & 15];
    if (i->kind != NOVA_PAD_HID) for (const char *c = "&IG_00"; *c; c++) a[n++] = *c;
    a[n++] = '#'; a[n++] = '8'; a[n++] = '&';
    int k = 28;
    while (k > 0 && !((i->serial >> k) & 15)) k -= 4;
    for (; k >= 0; k -= 4) a[n++] = hex[(i->serial >> k) & 15];
    for (const char *c = "&0&0000#{4D1E55B2-F16F-11CF-88CB-001111000030}"; *c; c++) a[n++] = *c;
    a[n] = 0;
    for (int j = 0; j <= n; j++) {
        char c = a[j];
        out[j] = (WCHAR)(lower && c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return n;
}

