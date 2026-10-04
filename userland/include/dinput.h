/*
 * dinput.h — DirectInput 8 (dinput8.dll): keyboards, mice and game
 * controllers.  The interfaces, structures and constants of the DirectX
 * SDK's DirectInput 8, as its documentation gives them.
 */
#pragma once
#include <windows.h>
#include <objbase.h>

#ifndef LOBYTE
#define LOBYTE(w) ((BYTE)((ULONG_PTR)(w) & 0xFF))
#endif
#ifndef HIBYTE
#define HIBYTE(w) ((BYTE)(((ULONG_PTR)(w) >> 8) & 0xFF))
#endif

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

DEFINE_GUID(CLSID_DirectInput8,        0x25E609E4, 0xB259, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(CLSID_DirectInputDevice8,  0x25E609E5, 0xB259, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInput8A,        0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
DEFINE_GUID(IID_IDirectInput8W,        0xBF798031, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
DEFINE_GUID(IID_IDirectInputDevice8A,  0x54D41080, 0xDC15, 0x4833, 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79);
DEFINE_GUID(IID_IDirectInputDevice8W,  0x54D41081, 0xDC15, 0x4833, 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79);
/* DirectInput 1-7 (dinput.dll, DirectInputCreate) */
DEFINE_GUID(CLSID_DirectInput,         0x25E609E0, 0xB259, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(CLSID_DirectInputDevice,   0x25E609E1, 0xB259, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInputA,         0x89521360, 0xAA8A, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInputW,         0x89521361, 0xAA8A, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInput2A,        0x5944E662, 0xAA8A, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInput2W,        0x5944E663, 0xAA8A, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInput7A,        0x9A4CB684, 0x236D, 0x11D3, 0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE);
DEFINE_GUID(IID_IDirectInput7W,        0x9A4CB685, 0x236D, 0x11D3, 0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE);
DEFINE_GUID(IID_IDirectInputDeviceA,   0x5944E680, 0xC92E, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInputDeviceW,   0x5944E681, 0xC92E, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInputDevice2A,  0x5944E682, 0xC92E, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInputDevice2W,  0x5944E683, 0xC92E, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(IID_IDirectInputDevice7A,  0x57D7C6BC, 0x2356, 0x11D3, 0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE);
DEFINE_GUID(IID_IDirectInputDevice7W,  0x57D7C6BD, 0x2356, 0x11D3, 0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE);

DEFINE_GUID(GUID_XAxis,   0xA36D02E0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_YAxis,   0xA36D02E1, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_ZAxis,   0xA36D02E2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_RxAxis,  0xA36D02F4, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_RyAxis,  0xA36D02F5, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_RzAxis,  0xA36D02E3, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_Slider,  0xA36D02E4, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_Button,  0xA36D02F0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_Key,     0x55728220, 0xD33C, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_POV,     0xA36D02F2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_Unknown, 0xA36D02F3, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_SysMouse,    0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_SysKeyboard, 0x6F1D2B61, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DEFINE_GUID(GUID_Joystick,    0x6F1D2B70, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

/* Return codes */
#define DI_OK                    S_OK
#define DI_NOTATTACHED           S_FALSE
#define DI_BUFFEROVERFLOW        S_FALSE
#define DI_PROPNOEFFECT          S_FALSE
#define DI_NOEFFECT              S_FALSE
#define DI_POLLEDDEVICE          ((HRESULT)0x00000002L)
#define DIERR_OLDDIRECTINPUTVERSION  ((HRESULT)0x8007047EL)
#define DIERR_BETADIRECTINPUTVERSION ((HRESULT)0x80070481L)
#define DIERR_BADDRIVERVER       ((HRESULT)0x80070077L)
#define DIERR_DEVICENOTREG       ((HRESULT)0x80040154L)
#define DIERR_NOTFOUND           ((HRESULT)0x80070002L)
#define DIERR_OBJECTNOTFOUND     ((HRESULT)0x80070002L)
#define DIERR_INVALIDPARAM       E_INVALIDARG
#define DIERR_NOINTERFACE        E_NOINTERFACE
#define DIERR_GENERIC            E_FAIL
#define DIERR_OUTOFMEMORY        E_OUTOFMEMORY
#define DIERR_UNSUPPORTED        E_NOTIMPL
#define DIERR_NOTINITIALIZED     ((HRESULT)0x80070015L)
#define DIERR_ALREADYINITIALIZED ((HRESULT)0x800704DFL)
#define DIERR_NOAGGREGATION      CLASS_E_NOAGGREGATION
#define DIERR_OTHERAPPHASPRIO    E_ACCESSDENIED
#define DIERR_INPUTLOST          ((HRESULT)0x8007001EL)
#define DIERR_ACQUIRED           ((HRESULT)0x800700AAL)
#define DIERR_NOTACQUIRED        ((HRESULT)0x8007000CL)
#define DIERR_READONLY           E_ACCESSDENIED
#define DIERR_HANDLEEXISTS       E_ACCESSDENIED
#define DIERR_NOTBUFFERED        ((HRESULT)0x80040207L)
#define DIERR_UNPLUGGED          ((HRESULT)0x80040209L)

/* Device classes and types */
#define DI8DEVCLASS_ALL          0
#define DI8DEVCLASS_DEVICE       1
#define DI8DEVCLASS_POINTER      2
#define DI8DEVCLASS_KEYBOARD     3
#define DI8DEVCLASS_GAMECTRL     4
#define DI8DEVTYPE_DEVICE        0x11
#define DI8DEVTYPE_MOUSE         0x12
#define DI8DEVTYPE_KEYBOARD      0x13
#define DI8DEVTYPE_JOYSTICK      0x14
#define DI8DEVTYPE_GAMEPAD       0x15
#define DI8DEVTYPE_DRIVING       0x16
#define DI8DEVTYPE_FLIGHT        0x17
#define DI8DEVTYPE_1STPERSON     0x18
#define DI8DEVTYPEMOUSE_TRADITIONAL   2
#define DI8DEVTYPEKEYBOARD_PCENH      4
#define DI8DEVTYPEJOYSTICK_STANDARD   2
#define DI8DEVTYPEGAMEPAD_STANDARD    2
#define DIDEVTYPE_HID            0x00010000
/* DirectInput 1-7's device types (DIDEVICEINSTANCE.dwDevType before version 8) */
#define DIDEVTYPE_DEVICE         1
#define DIDEVTYPE_MOUSE          2
#define DIDEVTYPE_KEYBOARD       3
#define DIDEVTYPE_JOYSTICK       4
#define DIDEVTYPEMOUSE_TRADITIONAL    2
#define DIDEVTYPEKEYBOARD_PCENH       4
#define DIDEVTYPEJOYSTICK_TRADITIONAL 2
#define DIDEVTYPEJOYSTICK_GAMEPAD     4
#define GET_DIDEVICE_TYPE(t)     LOBYTE(t)
#define GET_DIDEVICE_SUBTYPE(t)  HIBYTE(t)

#define DIEDFL_ALLDEVICES        0x00000000
#define DIEDFL_ATTACHEDONLY      0x00000001
#define DIEDFL_FORCEFEEDBACK     0x00000100
#define DIEDFL_INCLUDEALIASES    0x00010000
#define DIEDFL_INCLUDEPHANTOMS   0x00020000
#define DIEDFL_INCLUDEHIDDEN     0x00040000
#define DIENUM_STOP              0
#define DIENUM_CONTINUE          1

/* Objects */
#define DIDFT_ALL                0x00000000
#define DIDFT_RELAXIS            0x00000001
#define DIDFT_ABSAXIS            0x00000002
#define DIDFT_AXIS               0x00000003
#define DIDFT_PSHBUTTON          0x00000004
#define DIDFT_TGLBUTTON          0x00000008
#define DIDFT_BUTTON             0x0000000C
#define DIDFT_POV                0x00000010
#define DIDFT_COLLECTION         0x00000040
#define DIDFT_NODATA             0x00000080
#define DIDFT_ANYINSTANCE        0x00FFFF00
#define DIDFT_INSTANCEMASK       DIDFT_ANYINSTANCE
#define DIDFT_MAKEINSTANCE(n)    ((WORD)(n) << 8)
#define DIDFT_GETTYPE(n)         LOBYTE(n)
#define DIDFT_GETINSTANCE(n)     LOWORD((n) >> 8)
#define DIDFT_FFACTUATOR         0x01000000
#define DIDFT_FFEFFECTTRIGGER    0x02000000
#define DIDFT_OUTPUT             0x10000000
#define DIDFT_VENDORDEFINED      0x04000000
#define DIDFT_ALIAS              0x08000000
#define DIDFT_OPTIONAL           0x80000000
#define DIDOI_ASPECTPOSITION     0x00000100
#define DIDF_ABSAXIS             0x00000001
#define DIDF_RELAXIS             0x00000002
#define DIDC_ATTACHED            0x00000001
#define DIDC_POLLEDDEVICE        0x00000002
#define DIDC_EMULATED            0x00000004
#define DIDC_POLLEDDATAFORMAT    0x00000008
#define DIDC_FORCEFEEDBACK       0x00000100
#define DIPH_DEVICE              0
#define DIPH_BYOFFSET            1
#define DIPH_BYID                2
#define DIPH_BYUSAGE             3
#define DIGDD_PEEK               0x00000001
#define DISCL_EXCLUSIVE          0x00000001
#define DISCL_NONEXCLUSIVE       0x00000002
#define DISCL_FOREGROUND         0x00000004
#define DISCL_BACKGROUND         0x00000008
#define DISCL_NOWINKEY           0x00000010
#define DIPROPAXISMODE_ABS       0
#define DIPROPAXISMODE_REL       1
#define DIPROPAUTOCENTER_OFF     0
#define DIPROPAUTOCENTER_ON      1

#define MAKEDIPROP(p)            ((REFGUID)(ULONG_PTR)(p))
#define DIPROP_BUFFERSIZE        MAKEDIPROP(1)
#define DIPROP_AXISMODE          MAKEDIPROP(2)
#define DIPROP_GRANULARITY       MAKEDIPROP(3)
#define DIPROP_RANGE             MAKEDIPROP(4)
#define DIPROP_DEADZONE          MAKEDIPROP(5)
#define DIPROP_SATURATION        MAKEDIPROP(6)
#define DIPROP_FFGAIN            MAKEDIPROP(7)
#define DIPROP_FFLOAD            MAKEDIPROP(8)
#define DIPROP_AUTOCENTER        MAKEDIPROP(9)
#define DIPROP_CALIBRATIONMODE   MAKEDIPROP(10)
#define DIPROP_CALIBRATION       MAKEDIPROP(11)
#define DIPROP_GUIDANDPATH       MAKEDIPROP(12)
#define DIPROP_INSTANCENAME      MAKEDIPROP(13)
#define DIPROP_PRODUCTNAME       MAKEDIPROP(14)
#define DIPROP_JOYSTICKID        MAKEDIPROP(15)
#define DIPROP_GETPORTDISPLAYNAME MAKEDIPROP(16)
#define DIPROP_PHYSICALRANGE     MAKEDIPROP(18)
#define DIPROP_LOGICALRANGE      MAKEDIPROP(19)
#define DIPROP_KEYNAME           MAKEDIPROP(20)
#define DIPROP_CPOINTS           MAKEDIPROP(21)
#define DIPROP_APPDATA           MAKEDIPROP(22)
#define DIPROP_SCANCODE          MAKEDIPROP(23)
#define DIPROP_VIDPID            MAKEDIPROP(24)
#define DIPROP_USERNAME          MAKEDIPROP(25)
#define DIPROP_TYPENAME          MAKEDIPROP(26)

typedef struct DIDEVICEINSTANCEA {
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    CHAR  tszInstanceName[MAX_PATH];
    CHAR  tszProductName[MAX_PATH];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
} DIDEVICEINSTANCEA, *LPDIDEVICEINSTANCEA;
typedef const DIDEVICEINSTANCEA *LPCDIDEVICEINSTANCEA;
typedef struct DIDEVICEINSTANCEW {
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    WCHAR tszInstanceName[MAX_PATH];
    WCHAR tszProductName[MAX_PATH];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
} DIDEVICEINSTANCEW, *LPDIDEVICEINSTANCEW;
typedef const DIDEVICEINSTANCEW *LPCDIDEVICEINSTANCEW;

typedef struct DIDEVICEOBJECTINSTANCEA {
    DWORD dwSize;
    GUID  guidType;
    DWORD dwOfs;
    DWORD dwType;
    DWORD dwFlags;
    CHAR  tszName[MAX_PATH];
    DWORD dwFFMaxForce;
    DWORD dwFFForceResolution;
    WORD  wCollectionNumber;
    WORD  wDesignatorIndex;
    WORD  wUsagePage;
    WORD  wUsage;
    DWORD dwDimension;
    WORD  wExponent;
    WORD  wReportId;
} DIDEVICEOBJECTINSTANCEA, *LPDIDEVICEOBJECTINSTANCEA;
typedef const DIDEVICEOBJECTINSTANCEA *LPCDIDEVICEOBJECTINSTANCEA;
typedef struct DIDEVICEOBJECTINSTANCEW {
    DWORD dwSize;
    GUID  guidType;
    DWORD dwOfs;
    DWORD dwType;
    DWORD dwFlags;
    WCHAR tszName[MAX_PATH];
    DWORD dwFFMaxForce;
    DWORD dwFFForceResolution;
    WORD  wCollectionNumber;
    WORD  wDesignatorIndex;
    WORD  wUsagePage;
    WORD  wUsage;
    DWORD dwDimension;
    WORD  wExponent;
    WORD  wReportId;
} DIDEVICEOBJECTINSTANCEW, *LPDIDEVICEOBJECTINSTANCEW;
typedef const DIDEVICEOBJECTINSTANCEW *LPCDIDEVICEOBJECTINSTANCEW;

typedef struct DIDEVCAPS {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwDevType;
    DWORD dwAxes;
    DWORD dwButtons;
    DWORD dwPOVs;
    DWORD dwFFSamplePeriod;
    DWORD dwFFMinTimeResolution;
    DWORD dwFirmwareRevision;
    DWORD dwHardwareRevision;
    DWORD dwFFDriverVersion;
} DIDEVCAPS, *LPDIDEVCAPS;

typedef struct _DIOBJECTDATAFORMAT {
    const GUID *pguid;
    DWORD dwOfs;
    DWORD dwType;
    DWORD dwFlags;
} DIOBJECTDATAFORMAT, *LPDIOBJECTDATAFORMAT;
typedef const DIOBJECTDATAFORMAT *LPCDIOBJECTDATAFORMAT;

typedef struct _DIDATAFORMAT {
    DWORD dwSize;
    DWORD dwObjSize;
    DWORD dwFlags;
    DWORD dwDataSize;
    DWORD dwNumObjs;
    LPDIOBJECTDATAFORMAT rgodf;
} DIDATAFORMAT, *LPDIDATAFORMAT;
typedef const DIDATAFORMAT *LPCDIDATAFORMAT;

typedef struct DIDEVICEOBJECTDATA {
    DWORD    dwOfs;
    DWORD    dwData;
    DWORD    dwTimeStamp;
    DWORD    dwSequence;
    UINT_PTR uAppData;
} DIDEVICEOBJECTDATA, *LPDIDEVICEOBJECTDATA;
typedef const DIDEVICEOBJECTDATA *LPCDIDEVICEOBJECTDATA;
typedef struct DIDEVICEOBJECTDATA_DX3 {
    DWORD dwOfs;
    DWORD dwData;
    DWORD dwTimeStamp;
    DWORD dwSequence;
} DIDEVICEOBJECTDATA_DX3;

typedef struct DIPROPHEADER {
    DWORD dwSize;
    DWORD dwHeaderSize;
    DWORD dwObj;
    DWORD dwHow;
} DIPROPHEADER, *LPDIPROPHEADER;
typedef const DIPROPHEADER *LPCDIPROPHEADER;
typedef struct DIPROPDWORD   { DIPROPHEADER diph; DWORD dwData; } DIPROPDWORD, *LPDIPROPDWORD;
typedef struct DIPROPPOINTER { DIPROPHEADER diph; UINT_PTR uData; } DIPROPPOINTER, *LPDIPROPPOINTER;
typedef struct DIPROPRANGE   { DIPROPHEADER diph; LONG lMin; LONG lMax; } DIPROPRANGE, *LPDIPROPRANGE;
typedef struct DIPROPSTRING  { DIPROPHEADER diph; WCHAR wsz[MAX_PATH]; } DIPROPSTRING, *LPDIPROPSTRING;
typedef struct DIPROPGUIDANDPATH { DIPROPHEADER diph; GUID guidClass; WCHAR wszPath[MAX_PATH]; } DIPROPGUIDANDPATH;

typedef struct DIJOYSTATE {
    LONG  lX, lY, lZ, lRx, lRy, lRz;
    LONG  rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE  rgbButtons[32];
} DIJOYSTATE, *LPDIJOYSTATE;
typedef struct DIJOYSTATE2 {
    LONG  lX, lY, lZ, lRx, lRy, lRz;
    LONG  rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE  rgbButtons[128];
    LONG  lVX, lVY, lVZ, lVRx, lVRy, lVRz;
    LONG  rglVSlider[2];
    LONG  lAX, lAY, lAZ, lARx, lARy, lARz;
    LONG  rglASlider[2];
    LONG  lFX, lFY, lFZ, lFRx, lFRy, lFRz;
    LONG  rglFSlider[2];
} DIJOYSTATE2, *LPDIJOYSTATE2;
typedef struct DIMOUSESTATE  { LONG lX, lY, lZ; BYTE rgbButtons[4]; } DIMOUSESTATE, *LPDIMOUSESTATE;
typedef struct DIMOUSESTATE2 { LONG lX, lY, lZ; BYTE rgbButtons[8]; } DIMOUSESTATE2, *LPDIMOUSESTATE2;

#define DIJOFS_X            0
#define DIJOFS_Y            4
#define DIJOFS_Z            8
#define DIJOFS_RX           12
#define DIJOFS_RY           16
#define DIJOFS_RZ           20
#define DIJOFS_SLIDER(n)    (24 + (n) * 4)
#define DIJOFS_POV(n)       (32 + (n) * 4)
#define DIJOFS_BUTTON(n)    (48 + (n))
#define DIMOFS_X            0
#define DIMOFS_Y            4
#define DIMOFS_Z            8
#define DIMOFS_BUTTON0      12

#define DIK_ESCAPE 0x01
#define DIK_RETURN 0x1C
#define DIK_SPACE  0x39
#define DIK_UP     0xC8
#define DIK_LEFT   0xCB
#define DIK_RIGHT  0xCD
#define DIK_DOWN   0xD0

typedef BOOL (CALLBACK *LPDIENUMDEVICESCALLBACKA)(LPCDIDEVICEINSTANCEA, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMDEVICESCALLBACKW)(LPCDIDEVICEINSTANCEW, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMDEVICEOBJECTSCALLBACKA)(LPCDIDEVICEOBJECTINSTANCEA, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMDEVICEOBJECTSCALLBACKW)(LPCDIDEVICEOBJECTINSTANCEW, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMEFFECTSCALLBACK)(const void *, LPVOID);
typedef BOOL (CALLBACK *LPDIENUMCREATEDEFFECTOBJECTSCALLBACK)(void *, LPVOID);

/* ---- IDirectInputDevice8 (A and W have the same methods; the strings differ) ---- */
#undef INTERFACE
#define INTERFACE IDirectInputDevice8W
DECLARE_INTERFACE_(IDirectInputDevice8W, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(GetCapabilities)(THIS_ LPDIDEVCAPS caps) PURE;
    STDMETHOD(EnumObjects)(THIS_ LPDIENUMDEVICEOBJECTSCALLBACKW cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(GetProperty)(THIS_ REFGUID prop, LPDIPROPHEADER ph) PURE;
    STDMETHOD(SetProperty)(THIS_ REFGUID prop, LPCDIPROPHEADER ph) PURE;
    STDMETHOD(Acquire)(THIS) PURE;
    STDMETHOD(Unacquire)(THIS) PURE;
    STDMETHOD(GetDeviceState)(THIS_ DWORD cb, LPVOID data) PURE;
    STDMETHOD(GetDeviceData)(THIS_ DWORD cbod, LPDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags) PURE;
    STDMETHOD(SetDataFormat)(THIS_ LPCDIDATAFORMAT df) PURE;
    STDMETHOD(SetEventNotification)(THIS_ HANDLE ev) PURE;
    STDMETHOD(SetCooperativeLevel)(THIS_ HWND hwnd, DWORD flags) PURE;
    STDMETHOD(GetObjectInfo)(THIS_ LPDIDEVICEOBJECTINSTANCEW oi, DWORD obj, DWORD how) PURE;
    STDMETHOD(GetDeviceInfo)(THIS_ LPDIDEVICEINSTANCEW di) PURE;
    STDMETHOD(RunControlPanel)(THIS_ HWND owner, DWORD flags) PURE;
    STDMETHOD(Initialize)(THIS_ HINSTANCE inst, DWORD version, REFGUID guid) PURE;
    STDMETHOD(CreateEffect)(THIS_ REFGUID guid, const void *eff, void **out, IUnknown *outer) PURE;
    STDMETHOD(EnumEffects)(THIS_ LPDIENUMEFFECTSCALLBACK cb, LPVOID ref, DWORD type) PURE;
    STDMETHOD(GetEffectInfo)(THIS_ void *info, REFGUID guid) PURE;
    STDMETHOD(GetForceFeedbackState)(THIS_ LPDWORD out) PURE;
    STDMETHOD(SendForceFeedbackCommand)(THIS_ DWORD flags) PURE;
    STDMETHOD(EnumCreatedEffectObjects)(THIS_ LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(Escape)(THIS_ void *esc) PURE;
    STDMETHOD(Poll)(THIS) PURE;
    STDMETHOD(SendDeviceData)(THIS_ DWORD cbod, LPCDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags) PURE;
    STDMETHOD(EnumEffectsInFile)(THIS_ LPCWSTR file, void *cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(WriteEffectToFile)(THIS_ LPCWSTR file, DWORD n, void *effects, DWORD flags) PURE;
    STDMETHOD(BuildActionMap)(THIS_ void *af, LPCWSTR user, DWORD flags) PURE;
    STDMETHOD(SetActionMap)(THIS_ void *af, LPCWSTR user, DWORD flags) PURE;
    STDMETHOD(GetImageInfo)(THIS_ void *hdr) PURE;
};
typedef IDirectInputDevice8W *LPDIRECTINPUTDEVICE8W;

#undef INTERFACE
#define INTERFACE IDirectInputDevice8A
DECLARE_INTERFACE_(IDirectInputDevice8A, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(GetCapabilities)(THIS_ LPDIDEVCAPS caps) PURE;
    STDMETHOD(EnumObjects)(THIS_ LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(GetProperty)(THIS_ REFGUID prop, LPDIPROPHEADER ph) PURE;
    STDMETHOD(SetProperty)(THIS_ REFGUID prop, LPCDIPROPHEADER ph) PURE;
    STDMETHOD(Acquire)(THIS) PURE;
    STDMETHOD(Unacquire)(THIS) PURE;
    STDMETHOD(GetDeviceState)(THIS_ DWORD cb, LPVOID data) PURE;
    STDMETHOD(GetDeviceData)(THIS_ DWORD cbod, LPDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags) PURE;
    STDMETHOD(SetDataFormat)(THIS_ LPCDIDATAFORMAT df) PURE;
    STDMETHOD(SetEventNotification)(THIS_ HANDLE ev) PURE;
    STDMETHOD(SetCooperativeLevel)(THIS_ HWND hwnd, DWORD flags) PURE;
    STDMETHOD(GetObjectInfo)(THIS_ LPDIDEVICEOBJECTINSTANCEA oi, DWORD obj, DWORD how) PURE;
    STDMETHOD(GetDeviceInfo)(THIS_ LPDIDEVICEINSTANCEA di) PURE;
    STDMETHOD(RunControlPanel)(THIS_ HWND owner, DWORD flags) PURE;
    STDMETHOD(Initialize)(THIS_ HINSTANCE inst, DWORD version, REFGUID guid) PURE;
    STDMETHOD(CreateEffect)(THIS_ REFGUID guid, const void *eff, void **out, IUnknown *outer) PURE;
    STDMETHOD(EnumEffects)(THIS_ LPDIENUMEFFECTSCALLBACK cb, LPVOID ref, DWORD type) PURE;
    STDMETHOD(GetEffectInfo)(THIS_ void *info, REFGUID guid) PURE;
    STDMETHOD(GetForceFeedbackState)(THIS_ LPDWORD out) PURE;
    STDMETHOD(SendForceFeedbackCommand)(THIS_ DWORD flags) PURE;
    STDMETHOD(EnumCreatedEffectObjects)(THIS_ LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(Escape)(THIS_ void *esc) PURE;
    STDMETHOD(Poll)(THIS) PURE;
    STDMETHOD(SendDeviceData)(THIS_ DWORD cbod, LPCDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags) PURE;
    STDMETHOD(EnumEffectsInFile)(THIS_ LPCSTR file, void *cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(WriteEffectToFile)(THIS_ LPCSTR file, DWORD n, void *effects, DWORD flags) PURE;
    STDMETHOD(BuildActionMap)(THIS_ void *af, LPCSTR user, DWORD flags) PURE;
    STDMETHOD(SetActionMap)(THIS_ void *af, LPCSTR user, DWORD flags) PURE;
    STDMETHOD(GetImageInfo)(THIS_ void *hdr) PURE;
};
typedef IDirectInputDevice8A *LPDIRECTINPUTDEVICE8A;

/* ---- IDirectInput8 ---- */
#undef INTERFACE
#define INTERFACE IDirectInput8W
DECLARE_INTERFACE_(IDirectInput8W, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(CreateDevice)(THIS_ REFGUID guid, LPDIRECTINPUTDEVICE8W *dev, IUnknown *outer) PURE;
    STDMETHOD(EnumDevices)(THIS_ DWORD type, LPDIENUMDEVICESCALLBACKW cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(GetDeviceStatus)(THIS_ REFGUID guid) PURE;
    STDMETHOD(RunControlPanel)(THIS_ HWND owner, DWORD flags) PURE;
    STDMETHOD(Initialize)(THIS_ HINSTANCE inst, DWORD version) PURE;
    STDMETHOD(FindDevice)(THIS_ REFGUID cls, LPCWSTR name, LPGUID out) PURE;
    STDMETHOD(EnumDevicesBySemantics)(THIS_ LPCWSTR user, void *af, void *cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(ConfigureDevices)(THIS_ void *cb, void *params, DWORD flags, LPVOID ref) PURE;
};
typedef IDirectInput8W *LPDIRECTINPUT8W;

#undef INTERFACE
#define INTERFACE IDirectInput8A
DECLARE_INTERFACE_(IDirectInput8A, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(CreateDevice)(THIS_ REFGUID guid, LPDIRECTINPUTDEVICE8A *dev, IUnknown *outer) PURE;
    STDMETHOD(EnumDevices)(THIS_ DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(GetDeviceStatus)(THIS_ REFGUID guid) PURE;
    STDMETHOD(RunControlPanel)(THIS_ HWND owner, DWORD flags) PURE;
    STDMETHOD(Initialize)(THIS_ HINSTANCE inst, DWORD version) PURE;
    STDMETHOD(FindDevice)(THIS_ REFGUID cls, LPCSTR name, LPGUID out) PURE;
    STDMETHOD(EnumDevicesBySemantics)(THIS_ LPCSTR user, void *af, void *cb, LPVOID ref, DWORD flags) PURE;
    STDMETHOD(ConfigureDevices)(THIS_ void *cb, void *params, DWORD flags, LPVOID ref) PURE;
};
typedef IDirectInput8A *LPDIRECTINPUT8A;
#undef INTERFACE

#ifndef DINPUTAPI
#define DINPUTAPI __declspec(dllimport)
#endif
DINPUTAPI HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID *out, LPUNKNOWN outer);
