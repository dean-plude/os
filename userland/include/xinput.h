/*
 * xinput.h — XInput, the Xbox controller API (xinput1_4.dll, xinput1_3.dll,
 * xinput9_1_0.dll)
 */
#pragma once
#include <windows.h>

#ifndef ERROR_BAD_ARGUMENTS
#define ERROR_BAD_ARGUMENTS 160
#endif
#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif
#ifndef ERROR_EMPTY
#define ERROR_EMPTY 4306
#endif

#define XUSER_MAX_COUNT 4
#define XUSER_INDEX_ANY 0x000000FF

#define XINPUT_DEVTYPE_GAMEPAD    0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01
#define XINPUT_FLAG_GAMEPAD       0x00000001
#define XINPUT_CAPS_FFB_SUPPORTED 0x0001
#define XINPUT_CAPS_WIRELESS      0x0002
#define XINPUT_CAPS_VOICE_SUPPORTED 0x0004
#define XINPUT_CAPS_PMD_SUPPORTED 0x0008
#define XINPUT_CAPS_NO_NAVIGATION 0x0010

#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

#define XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE  7849
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE 8689
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD    30

#define BATTERY_DEVTYPE_GAMEPAD   0x00
#define BATTERY_DEVTYPE_HEADSET   0x01
#define BATTERY_TYPE_DISCONNECTED 0x00
#define BATTERY_TYPE_WIRED        0x01
#define BATTERY_TYPE_ALKALINE     0x02
#define BATTERY_TYPE_NIMH         0x03
#define BATTERY_TYPE_UNKNOWN      0xFF
#define BATTERY_LEVEL_EMPTY       0x00
#define BATTERY_LEVEL_LOW         0x01
#define BATTERY_LEVEL_MEDIUM      0x02
#define BATTERY_LEVEL_FULL        0x03

#define VK_PAD_A                0x5800
#define VK_PAD_B                0x5801
#define VK_PAD_X                0x5802
#define VK_PAD_Y                0x5803
#define VK_PAD_RSHOULDER        0x5804
#define VK_PAD_LSHOULDER        0x5805
#define VK_PAD_LTRIGGER         0x5806
#define VK_PAD_RTRIGGER         0x5807
#define VK_PAD_DPAD_UP          0x5810
#define VK_PAD_DPAD_DOWN        0x5811
#define VK_PAD_DPAD_LEFT        0x5812
#define VK_PAD_DPAD_RIGHT       0x5813
#define VK_PAD_START            0x5814
#define VK_PAD_BACK             0x5815
#define VK_PAD_LTHUMB_PRESS     0x5816
#define VK_PAD_RTHUMB_PRESS     0x5817
#define XINPUT_KEYSTROKE_KEYDOWN 0x0001
#define XINPUT_KEYSTROKE_KEYUP   0x0002
#define XINPUT_KEYSTROKE_REPEAT  0x0004

typedef struct _XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

typedef struct _XINPUT_STATE {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;

typedef struct _XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;

typedef struct _XINPUT_CAPABILITIES {
    BYTE             Type;
    BYTE             SubType;
    WORD             Flags;
    XINPUT_GAMEPAD   Gamepad;
    XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES, *PXINPUT_CAPABILITIES;

/* What xinput1_4.dll's ordinal 108 (XInputGetCapabilitiesEx) fills */
typedef struct _XINPUT_CAPABILITIES_EX {
    XINPUT_CAPABILITIES Capabilities;
    WORD  VendorId;
    WORD  ProductId;
    WORD  ProductVersion;
    WORD  unk1;
    DWORD unk2;
} XINPUT_CAPABILITIES_EX;

typedef struct _XINPUT_BATTERY_INFORMATION {
    BYTE BatteryType;
    BYTE BatteryLevel;
} XINPUT_BATTERY_INFORMATION, *PXINPUT_BATTERY_INFORMATION;

typedef struct _XINPUT_KEYSTROKE {
    WORD  VirtualKey;
    WCHAR Unicode;
    WORD  Flags;
    BYTE  UserIndex;
    BYTE  HidCode;
} XINPUT_KEYSTROKE, *PXINPUT_KEYSTROKE;

#ifndef XINPUTAPI
#define XINPUTAPI __declspec(dllimport)
#endif
XINPUTAPI DWORD WINAPI XInputGetState(DWORD user, XINPUT_STATE *state);
XINPUTAPI DWORD WINAPI XInputSetState(DWORD user, XINPUT_VIBRATION *vibration);
XINPUTAPI DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *caps);
XINPUTAPI void  WINAPI XInputEnable(BOOL enable);
XINPUTAPI DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE devtype, XINPUT_BATTERY_INFORMATION *info);
XINPUTAPI DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE *key);
XINPUTAPI DWORD WINAPI XInputGetAudioDeviceIds(DWORD user, LPWSTR render, UINT *render_n, LPWSTR capture, UINT *capture_n);
XINPUTAPI DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, GUID *render, GUID *capture);
