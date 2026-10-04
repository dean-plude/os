/*
 * gamepad.h — game controllers (gamepads and joysticks)
 *
 * The drivers (xpad.c: Xbox 360 and Xbox One controllers, which are not
 * HID devices; gamepad.c: HID gamepads and joysticks, Generic Desktop
 * usages 4 and 5) keep the state of each controller in a slot here.
 * Programs read it through NtNovaGuiCtl's CTL_GAMEPAD (um_gui.c):
 * xinput1_4.dll sees the Xbox controllers, dinput8.dll every controller,
 * as on Windows.  Every controller has the generic view DirectInput
 * shows (buttons, eight axes, a point-of-view hat); Xbox controllers
 * also have XInput's, and an XInput user index 0-3 given when they are
 * plugged in (the lowest free one, shown on the controller's ring).
 */

#pragma once

#include "../include/types.h"

#define PAD_SLOTS     8
#define PAD_XINPUT_USERS 4

/* Kinds */
#define PAD_XBOX360   1               /* XInput: Xbox 360 wired controller protocol */
#define PAD_XBOXONE   2               /* XInput: Xbox One (GIP) protocol */
#define PAD_HID       3               /* a HID gamepad or joystick */

/* Axes, in DirectInput's order (Generic Desktop usages 0x30-0x37) */
enum { PAD_X, PAD_Y, PAD_Z, PAD_RX, PAD_RY, PAD_RZ, PAD_SLIDER, PAD_DIAL, PAD_AXES };

/* What a program reads; the layout is shared with userland (64- and
 * 32-bit alike: no pointers) */
typedef struct {
    UINT32 packet;                    /* changes whenever the state does */
    UINT32 buttons;                   /* bit n: button n + 1 */
    UINT16 axis[PAD_AXES];            /* 0..65535 (32768: centred); X right, Y down */
    INT32  pov;                       /* hundredths of a degree clockwise from up; -1 centred */
    /* Xbox controllers: XInput's view */
    UINT16 xbuttons;                  /* XINPUT_GAMEPAD_* (0x0400: the guide button) */
    UINT8  lt, rt;                    /* 0..255 */
    INT16  lx, ly, rx, ry;            /* Y up */
} PadState;

typedef struct {
    UINT32 serial;                    /* new each time a controller is plugged in (0: the slot is empty) */
    UINT16 vid, pid;
    UINT8  kind;                      /* PAD_* */
    INT8   xuser;                     /* XInput user index, -1 for none */
    UINT8  buttons;                   /* how many */
    UINT8  axes;                      /* bit n: axis n is there */
    UINT8  povs;                      /* 0 or 1 */
    UINT8  rumble;                    /* 1: it has motors */
    UINT16 usage;                     /* HID usage page << 8 | usage: 0x0104 joystick, 0x0105 gamepad */
    char   name[64];
} PadInfo;

/* For the drivers.  A controller gets a slot (-1: none free) with its
 * facts; the driver reports its state (from an interrupt completion:
 * quick, no transfers) and frees the slot when it goes.  @rumble, when
 * the controller has motors, sets them (0..65535 each; any thread) */
typedef bool (*PadRumble)(void *ctx, UINT16 left, UINT16 right);
int  PadAttach(const PadInfo *info, PadRumble rumble, void *ctx);
void PadReport(int slot, const PadState *st);
void PadDetach(int slot);

/* For programs (CTL_GAMEPAD) */
UINT32 PadPresent(void);                                  /* bit n: slot n holds a controller */
bool   PadGetInfo(int slot, PadInfo *out);
bool   PadGetState(int slot, PadState *out);
int    PadXInputSlot(int user);                           /* the slot of XInput user @user, or -1 */
bool   PadSetRumble(int slot, UINT16 left, UINT16 right);

/* Raw HID reports, for Raw Input (WM_INPUT) and HID device handles
 * (um_hid.c): every controller has a HID report descriptor (an Xbox
 * controller the one Windows' Xbox driver gives its HID side: X/Y the
 * left stick, Rx/Ry the right, Z both triggers, ten buttons and a hat)
 * and its input reports go through a ring programs read.  A report starts
 * with its report ID byte (0 when the device uses none), as Windows
 * hands them out, and is the device's input report length long. */
#define PAD_DESC_MAX    1024
#define PAD_REPORT_MAX  64
#define PAD_RAW_RING    128

typedef struct {
    UINT32 seq;                       /* 1, 2, ... */
    UINT32 serial;                    /* the controller's PadInfo.serial */
    UINT8  slot, len;
    UINT16 reserved;
    UINT8  data[PAD_REPORT_MAX];
} PadRaw;

/* For HID drivers, after PadAttach: the device's own descriptor and its
 * input report length (with the ID byte); and each report as it comes
 * (@r without an ID byte when the device uses none: @ids false) */
void PadSetDescriptor(int slot, const UINT8 *desc, int len, int in_len);
void PadRawReport(int slot, const UINT8 *r, int len, bool ids);

/* For programs: a slot's descriptor (its length; *@in_len the input
 * report length), the slot of a controller by serial (-1: gone), the
 * reports after @after (waiting up to @wait_ticks for one, or for a
 * controller to come or go: *@changes counts those) and a slot's latest
 * report with ID @id */
int  PadGetDescriptor(int slot, UINT8 *out, int cap, UINT16 *in_len);
int  PadSlotOfSerial(UINT32 serial);
int  PadReadRaw(UINT32 after, UINT32 known_changes, PadRaw *out, int max, UINT64 wait_ticks,
                UINT32 *newest, UINT32 *changes);
bool PadLastRaw(int slot, UINT8 id, PadRaw *out);

/* Who else takes each report (HID device handles): called outside the
 * slot lock in the device poll thread; @len -1 when the controller goes */
typedef void (*PadRawSink)(int slot, UINT32 serial, const UINT8 *r, int len);
void PadSetRawSink(PadRawSink fn);
