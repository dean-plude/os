/*
 * xinput1_4.dll (and xinput1_3, xinput1_2, xinput1_1, xinput9_1_0: the same
 * code) — XInput: the Xbox controllers
 *
 * The kernel gives each Xbox controller (kernel/drivers/xpad.c) an XInput
 * user index 0-3 when it is plugged in and keeps its state in XInput's
 * own form, so XInputGetState is one NtNovaGuiCtl (CTL_GAMEPAD,
 * novapad.h).  As on Windows, HID gamepads that are not Xbox controllers
 * are not XInput devices (DirectInput, dinput8.dll, has them).  The
 * packet number changes whenever the controller's state does.  The Guide
 * button only shows through XInputGetStateEx (ordinal 100), which
 * programs look up by number.
 */
#define XINPUTAPI __declspec(dllexport)
#include <xinput.h>
#include <novapad.h>

static volatile LONG g_disabled;     /* XInputEnable(FALSE) */

/* XInput user @user's controller: its state (and *@info), or FALSE */
static BOOL user_state(DWORD user, NovaPadState *st, NovaPadInfo *info)
{
    int slot = nova_pad_xinput_slot((int)user);
    if (slot < 0) return FALSE;
    if (info && !nova_pad_info(slot, info)) return FALSE;
    return nova_pad_state(slot, st);
}

static DWORD get_state(DWORD user, XINPUT_STATE *state, BOOL guide)
{
    if (user >= XUSER_MAX_COUNT || !state) return ERROR_BAD_ARGUMENTS;
    NovaPadState st;
    if (!user_state(user, &st, NULL)) return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = st.packet;
    if (g_disabled) {                         /* (connected, but showing nothing) */
        ZeroMemory(&state->Gamepad, sizeof(state->Gamepad));
        return ERROR_SUCCESS;
    }
    state->Gamepad.wButtons = guide ? st.xbuttons : (WORD)(st.xbuttons & ~0x0400);
    state->Gamepad.bLeftTrigger = st.lt;
    state->Gamepad.bRightTrigger = st.rt;
    state->Gamepad.sThumbLX = st.lx;
    state->Gamepad.sThumbLY = st.ly;
    state->Gamepad.sThumbRX = st.rx;
    state->Gamepad.sThumbRY = st.ry;
    return ERROR_SUCCESS;
}

XINPUTAPI DWORD WINAPI XInputGetState(DWORD user, XINPUT_STATE *state)
{
    return get_state(user, state, FALSE);
}

XINPUTAPI DWORD WINAPI XInputGetStateEx(DWORD user, XINPUT_STATE *state)
{
    return get_state(user, state, TRUE);
}

static XINPUT_VIBRATION g_motors[XUSER_MAX_COUNT];   /* as programs last set them */

XINPUTAPI DWORD WINAPI XInputSetState(DWORD user, XINPUT_VIBRATION *v)
{
    if (user >= XUSER_MAX_COUNT || !v) return ERROR_BAD_ARGUMENTS;
    int slot = nova_pad_xinput_slot((int)user);
    if (slot < 0) return ERROR_DEVICE_NOT_CONNECTED;
    g_motors[user] = *v;
    if (g_disabled) return ERROR_SUCCESS;
    nova_pad_rumble(slot, v->wLeftMotorSpeed, v->wRightMotorSpeed);
    return ERROR_SUCCESS;
}

/* Stops the motors and shows neutral states while disabled (a game that
 * loses the focus); enabling sets the motors as they were */
XINPUTAPI void WINAPI XInputEnable(BOOL enable)
{
    g_disabled = !enable;
    for (int u = 0; u < XUSER_MAX_COUNT; u++) {
        int slot = nova_pad_xinput_slot(u);
        if (slot >= 0)
            nova_pad_rumble(slot, enable ? g_motors[u].wLeftMotorSpeed : 0, enable ? g_motors[u].wRightMotorSpeed : 0);
    }
}

static DWORD get_caps(DWORD user, DWORD flags, XINPUT_CAPABILITIES *caps, NovaPadInfo *info)
{
    if (user >= XUSER_MAX_COUNT || !caps || (flags & ~XINPUT_FLAG_GAMEPAD)) return ERROR_BAD_ARGUMENTS;
    NovaPadState st;
    if (!user_state(user, &st, info)) return ERROR_DEVICE_NOT_CONNECTED;
    ZeroMemory(caps, sizeof(*caps));
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Flags = info->rumble ? XINPUT_CAPS_FFB_SUPPORTED : 0;
    /* what a wired Xbox controller reports: every button, full triggers,
     * the sticks to 10 bits */
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = (SHORT)0xFFC0;
    caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
    if (info->rumble) caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
    return ERROR_SUCCESS;
}

XINPUTAPI DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    NovaPadInfo info;
    return get_caps(user, flags, caps, &info);
}

/* Ordinal 108: the capabilities with the controller's USB vendor and product */
XINPUTAPI DWORD WINAPI XInputGetCapabilitiesEx(DWORD unk, DWORD user, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    (void)unk;
    if (!caps) return ERROR_BAD_ARGUMENTS;
    NovaPadInfo info;
    DWORD r = get_caps(user, flags, &caps->Capabilities, &info);
    if (r != ERROR_SUCCESS) return r;
    caps->VendorId = info.vid;
    caps->ProductId = info.pid;
    caps->ProductVersion = 0x0100;
    caps->unk1 = 0;
    caps->unk2 = 0;
    return ERROR_SUCCESS;
}

XINPUTAPI DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE devtype, XINPUT_BATTERY_INFORMATION *info)
{
    if (user >= XUSER_MAX_COUNT || !info) return ERROR_BAD_ARGUMENTS;
    if (nova_pad_xinput_slot((int)user) < 0) return ERROR_DEVICE_NOT_CONNECTED;
    /* wired controllers, and no headsets */
    info->BatteryType = devtype == BATTERY_DEVTYPE_GAMEPAD ? BATTERY_TYPE_WIRED : BATTERY_TYPE_DISCONNECTED;
    info->BatteryLevel = devtype == BATTERY_DEVTYPE_GAMEPAD ? BATTERY_LEVEL_FULL : BATTERY_LEVEL_EMPTY;
    return ERROR_SUCCESS;
}

/* XInputGetKeystroke: button presses and releases as VK_PAD_* keys, found
 * by comparing each controller's state with what this process saw last */
static const struct { WORD bit, vk; } g_keys[] = {
    { XINPUT_GAMEPAD_A, VK_PAD_A }, { XINPUT_GAMEPAD_B, VK_PAD_B }, { XINPUT_GAMEPAD_X, VK_PAD_X },
    { XINPUT_GAMEPAD_Y, VK_PAD_Y }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
    { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER }, { XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
    { XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN }, { XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
    { XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT }, { XINPUT_GAMEPAD_START, VK_PAD_START },
    { XINPUT_GAMEPAD_BACK, VK_PAD_BACK }, { XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
    { XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
};
static DWORD g_seen[XUSER_MAX_COUNT];  /* buttons, and bits 16/17 the triggers past the threshold */
static DWORD g_seen_serial[XUSER_MAX_COUNT];

static BOOL keystroke(DWORD user, XINPUT_KEYSTROKE *key)
{
    NovaPadState st;
    NovaPadInfo info;
    if (!user_state(user, &st, &info)) return FALSE;
    if (g_seen_serial[user] != info.serial) { g_seen_serial[user] = info.serial; g_seen[user] = 0; }
    DWORD now = (st.xbuttons & ~0x0400u) | (st.lt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1u << 16 : 0) |
                (st.rt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1u << 17 : 0);
    DWORD diff = now ^ g_seen[user];
    if (!diff) return FALSE;
    WORD vk = 0;
    DWORD bit = 0;
    for (unsigned i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]) && !vk; i++)
        if (diff & g_keys[i].bit) { vk = g_keys[i].vk; bit = g_keys[i].bit; }
    if (!vk && (diff & (1u << 16))) { vk = VK_PAD_LTRIGGER; bit = 1u << 16; }
    if (!vk && (diff & (1u << 17))) { vk = VK_PAD_RTRIGGER; bit = 1u << 17; }
    g_seen[user] ^= bit ? bit : diff;
    if (!vk) return FALSE;
    key->VirtualKey = vk;
    key->Unicode = 0;
    key->Flags = (now & bit) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
    key->UserIndex = (BYTE)user;
    key->HidCode = 0;
    return TRUE;
}

XINPUTAPI DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE *key)
{
    (void)reserved;
    if (!key || (user >= XUSER_MAX_COUNT && user != XUSER_INDEX_ANY)) return ERROR_BAD_ARGUMENTS;
    if (g_disabled) return ERROR_EMPTY;
    if (user != XUSER_INDEX_ANY) {
        if (nova_pad_xinput_slot((int)user) < 0) return ERROR_DEVICE_NOT_CONNECTED;
        return keystroke(user, key) ? ERROR_SUCCESS : ERROR_EMPTY;
    }
    BOOL any = FALSE;
    for (DWORD u = 0; u < XUSER_MAX_COUNT; u++) {
        if (nova_pad_xinput_slot((int)u) < 0) continue;
        any = TRUE;
        if (keystroke(u, key)) return ERROR_SUCCESS;
    }
    return any ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
}

/* The controllers have no headsets */
XINPUTAPI DWORD WINAPI XInputGetAudioDeviceIds(DWORD user, LPWSTR render, UINT *render_n, LPWSTR capture, UINT *capture_n)
{
    if (user >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (nova_pad_xinput_slot((int)user) < 0) return ERROR_DEVICE_NOT_CONNECTED;
    if (render && render_n && *render_n) render[0] = 0;
    if (capture && capture_n && *capture_n) capture[0] = 0;
    if (render_n) *render_n = 0;
    if (capture_n) *capture_n = 0;
    return ERROR_SUCCESS;
}

XINPUTAPI DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, GUID *render, GUID *capture)
{
    if (user >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (nova_pad_xinput_slot((int)user) < 0) return ERROR_DEVICE_NOT_CONNECTED;
    if (render) ZeroMemory(render, sizeof(*render));
    if (capture) ZeroMemory(capture, sizeof(*capture));
    return ERROR_SUCCESS;
}

/* Ordinals 101-104: the Guide button waits, turning wireless controllers
 * off and the bus information, which wired controllers don't have */
XINPUTAPI DWORD WINAPI XInputWaitForGuideButton(DWORD user, DWORD flags, void *overlapped)
{
    (void)flags; (void)overlapped;
    return user < XUSER_MAX_COUNT && nova_pad_xinput_slot((int)user) >= 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

XINPUTAPI DWORD WINAPI XInputCancelGuideButtonWait(DWORD user)
{
    return user < XUSER_MAX_COUNT && nova_pad_xinput_slot((int)user) >= 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

XINPUTAPI DWORD WINAPI XInputPowerOffController(DWORD user)
{
    return user < XUSER_MAX_COUNT && nova_pad_xinput_slot((int)user) >= 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

XINPUTAPI DWORD WINAPI XInputGetBaseBusInformation(DWORD user, void *info)
{
    (void)info;
    return user < XUSER_MAX_COUNT && nova_pad_xinput_slot((int)user) >= 0 ? ERROR_NOT_SUPPORTED : ERROR_DEVICE_NOT_CONNECTED;
}
