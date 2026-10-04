/*
 * dinput.dll — DirectInput 3 to 7, for older games (Cave Story imports
 * DirectInputCreateA, and the process cannot start without this DLL)
 *
 * The same keyboard, mouse and game controllers as dinput8.dll: this is
 * userland/dinput8/dinput8.c built again with NOVA_DINPUT_LEGACY, which
 * changes what differs between the versions: the interfaces' IIDs
 * (IDirectInput, 2 and 7; IDirectInputDevice, 2 and 7, whose methods are
 * IDirectInputDevice8's first ones), IDirectInput7's CreateDeviceEx in the
 * place IDirectInput8 has EnumDevicesBySemantics, the versions taken
 * (0x0300-0x07FF), and the device types (DIDEVTYPE_KEYBOARD, _MOUSE and
 * _JOYSTICK, which number the device classes EnumDevices takes as
 * DirectInput 8's DI8DEVCLASS_* do).
 */
#define NOVA_DINPUT_LEGACY
#include "../dinput8/dinput8.c"

static HRESULT create(DWORD version, REFIID riid, void **out, IUnknown *outer)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (outer) return DIERR_NOAGGREGATION;
    HRESULT hr = check_version(version);
    if (hr != DI_OK) return hr;
    DInput *di = new_dinput();
    if (!di) return DIERR_OUTOFMEMORY;
    di->init = TRUE;
    hr = iw_qi(&di->iw, riid, out);
    iw_release(&di->iw);
    return hr;
}

DINPUTAPI HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD version, LPVOID *out, LPUNKNOWN outer)
{
    (void)inst;
    return create(version, &IID_IDirectInputA, out, outer);
}

DINPUTAPI HRESULT WINAPI DirectInputCreateW(HINSTANCE inst, DWORD version, LPVOID *out, LPUNKNOWN outer)
{
    (void)inst;
    return create(version, &IID_IDirectInputW, out, outer);
}

DINPUTAPI HRESULT WINAPI DirectInputCreateEx(HINSTANCE inst, DWORD version, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    (void)inst;
    if (!riid) return DIERR_INVALIDPARAM;
    return create(version, riid, out, outer);
}
