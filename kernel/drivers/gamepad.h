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
