/*
 * novapad.h — game controllers as NovaOS's kernel keeps them
 *
 * NtNovaGuiCtl(0, CTL_GAMEPAD, op | n << 8, ptr) reads the controller
 * slots of kernel/drivers/gamepad.h (whose PadInfo and PadState these
 * mirror): xinput1_4.dll, dinput8.dll and the self-test padtest use it.
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
