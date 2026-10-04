/*
 * padtest.exe — game controllers through XInput and DirectInput 8
 *
 * The devices self-test boot "gamepad" has three controllers on its xHCI
 * controller, each tools/padpeer.py behind a QEMU usb-redir device: a
 * wired Xbox 360 controller, an Xbox One controller and a HID game pad.
 * "padtest" checks what each API lists, then asks the test harness (a
 * "[PAD] step N:" line) to set a controller's buttons and sticks and
 * checks what XInput (xinput1_4.dll, loaded the way games load it, the
 * Guide button through ordinal 100) and DirectInput (dinput8.dll, made
 * both with DirectInput8Create and with CoCreateInstance; the program's
 * own c_dfDIJoystick, as dinput8.lib gives programs) report:
 *  1. the Xbox 360 controller: A, LB, the D-pad up and right, the left
 *     trigger, the left stick (XInput, XInputGetKeystroke; DirectInput
 *     with the axes' range set to -1000..1000: X, Y, Z, buttons 0 and 4,
 *     the hat at 45 degrees);
 *  2. the Xbox One controller: B, Y, Menu, RB and Guide, the right
 *     trigger and the right stick;
 *  3. the HID game pad: buttons 1, 3 and 12, the hat east, X, Y and Rz
 *     (DirectInput only: XInput must not list it), and what its buffer
 *     (DIPROP_BUFFERSIZE, GetDeviceData) holds;
 *  4. one of its buttons released and another pressed: exactly those two
 *     buffered events;
 *  5. the motors (XInputSetState; the harness checks what reached the
 *     controllers);
 *  6. the Xbox 360 controller unplugged: XInput says it is not
 *     connected, its DirectInput device says DIERR_INPUTLOST.
 * "padtest still" (run 32-bit after that) checks the two controllers
 * left still show steps 2 and 4.  "padtest list" prints what both APIs
 * see.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <xinput.h>
#include <dinput.h>

static int g_pass, g_fail;

static void check(int ok, const char *what, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, what);
    vsnprintf(buf, sizeof(buf), what, ap);
    va_end(ap);
    printf("%s %s\n", ok ? "ok  " : "FAIL", buf);
    if (ok) g_pass++; else g_fail++;
}

/* ---- XInput, loaded as games load it ---- */

typedef DWORD (WINAPI *GetState_t)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *SetState_t)(DWORD, XINPUT_VIBRATION *);
typedef DWORD (WINAPI *GetCapsEx_t)(DWORD, DWORD, DWORD, XINPUT_CAPABILITIES_EX *);
typedef DWORD (WINAPI *GetCaps_t)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef DWORD (WINAPI *GetKeystroke_t)(DWORD, DWORD, XINPUT_KEYSTROKE *);
static GetState_t pGetState, pGetStateEx;
static SetState_t pSetState;
static GetCapsEx_t pGetCapsEx;
static GetCaps_t pGetCaps;
static GetKeystroke_t pGetKeystroke;

static int load_xinput(void)
{
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput1_3.dll");
    if (!m) return 0;
    pGetState = (GetState_t)GetProcAddress(m, "XInputGetState");
    pGetStateEx = (GetState_t)GetProcAddress(m, (LPCSTR)100);
    pSetState = (SetState_t)GetProcAddress(m, "XInputSetState");
    pGetCaps = (GetCaps_t)GetProcAddress(m, "XInputGetCapabilities");
    pGetCapsEx = (GetCapsEx_t)GetProcAddress(m, (LPCSTR)108);
    pGetKeystroke = (GetKeystroke_t)GetProcAddress(m, "XInputGetKeystroke");
    return pGetState && pGetStateEx && pSetState && pGetCaps && pGetCapsEx && pGetKeystroke;
}

/* The XInput user of the controller with USB product @pid, or -1 */
static int xinput_user(WORD pid)
{
    for (DWORD u = 0; u < XUSER_MAX_COUNT; u++) {
        XINPUT_CAPABILITIES_EX c;
        if (pGetCapsEx(1, u, 0, &c) == ERROR_SUCCESS && c.ProductId == pid) return (int)u;
    }
    return -1;
}

/* ---- DirectInput: the data format dinput8.lib gives programs ---- */

static DIOBJECTDATAFORMAT g_joy_objs[44];
static DIDATAFORMAT c_dfDIJoystick = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DIJOYSTATE), 44, g_joy_objs };
static DIOBJECTDATAFORMAT g_kbd_objs[256];
static DIDATAFORMAT c_dfDIKeyboard = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS, 256, 256, g_kbd_objs };

static void make_formats(void)
{
    static const GUID *const axes[8] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis,
                                         &GUID_Slider, &GUID_Slider };
    int n = 0;
    for (int i = 0; i < 8; i++)
        g_joy_objs[n++] = (DIOBJECTDATAFORMAT){ axes[i], (DWORD)(i * 4), DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION };
    for (int i = 0; i < 4; i++)
        g_joy_objs[n++] = (DIOBJECTDATAFORMAT){ &GUID_POV, (DWORD)DIJOFS_POV(i), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0 };
    for (int i = 0; i < 32; i++)
        g_joy_objs[n++] = (DIOBJECTDATAFORMAT){ NULL, (DWORD)DIJOFS_BUTTON(i), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 };
    for (int i = 0; i < 256; i++)
        g_kbd_objs[i] = (DIOBJECTDATAFORMAT){ &GUID_Key, (DWORD)i, DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i), 0 };
}

typedef struct { GUID inst; DWORD pidvid; DWORD type; WCHAR name[MAX_PATH]; } Found;
static Found g_found[8];
static int g_nfound;

static BOOL CALLBACK found_one(LPCDIDEVICEINSTANCEW di, LPVOID ref)
{
    (void)ref;
    if (g_nfound < 8) {
        Found *f = &g_found[g_nfound++];
        f->inst = di->guidInstance;
        f->type = di->dwDevType;
        f->pidvid = memcmp(&di->guidProduct.Data4[2], "PIDVID", 6) == 0 ? di->guidProduct.Data1 : 0;
        lstrcpynW(f->name, di->tszInstanceName, MAX_PATH);
    }
    return DIENUM_CONTINUE;
}

static int enum_pads(IDirectInput8W *di)
{
    g_nfound = 0;
    di->lpVtbl->EnumDevices(di, DI8DEVCLASS_GAMECTRL, found_one, NULL, DIEDFL_ATTACHEDONLY);
    return g_nfound;
}

static const Found *found_pidvid(DWORD pidvid)
{
    for (int i = 0; i < g_nfound; i++) if (g_found[i].pidvid == pidvid) return &g_found[i];
    return NULL;
}

static int g_objs[3];                      /* axes, buttons, POVs EnumObjects listed */
static BOOL CALLBACK count_obj(LPCDIDEVICEOBJECTINSTANCEA oi, LPVOID ref)
{
    (void)ref;
    if (oi->dwType & DIDFT_AXIS) g_objs[0]++;
    else if (oi->dwType & DIDFT_POV) g_objs[2]++;
    else if (oi->dwType & DIDFT_BUTTON) g_objs[1]++;
    return DIENUM_CONTINUE;
}

#define VIDPID(v, p) ((DWORD)(v) | (DWORD)(p) << 16)
#define PID_360  0x028E
#define PID_ONE  0x02EA
#define VP_360   VIDPID(0x045E, PID_360)
#define VP_ONE   VIDPID(0x045E, PID_ONE)
#define VP_HID   VIDPID(0x1209, 0x0007)

static IDirectInputDevice8W *open_pad(IDirectInput8W *di, DWORD pidvid)
{
    const Found *f = found_pidvid(pidvid);
    IDirectInputDevice8W *d = NULL;
    if (!f || FAILED(di->lpVtbl->CreateDevice(di, &f->inst, &d, NULL))) return NULL;
    if (FAILED(d->lpVtbl->SetDataFormat(d, &c_dfDIJoystick)) ||
        FAILED(d->lpVtbl->SetCooperativeLevel(d, NULL, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
        d->lpVtbl->Release(d);
        return NULL;
    }
    return d;
}

static int joy_state(IDirectInputDevice8W *d, DIJOYSTATE *js)
{
    d->lpVtbl->Poll(d);
    return SUCCEEDED(d->lpVtbl->GetDeviceState(d, sizeof(*js), js));
}

/* Wait up to 20 s for @cond */
#define WAIT_FOR(cond) do { for (int t_ = 0; t_ < 1000 && !(cond); t_++) Sleep(20); } while (0)

static LONG scaled(LONG lo, LONG hi, LONG raw) { return lo + (LONG)(((LONGLONG)raw * (hi - lo)) / 65535); }
static int near(LONG a, LONG b) { return a - b <= 2 && b - a <= 2; }

static void buttons_are(const DIJOYSTATE *js, DWORD want, const char *who)
{
    DWORD got = 0;
    for (int i = 0; i < 32; i++) if (js->rgbButtons[i] & 0x80) got |= 1u << i;
    check(got == want, "%s DirectInput buttons %08lx (want %08lx)", who, (unsigned long)got, (unsigned long)want);
}

static void list(void)
{
    if (load_xinput())
        for (DWORD u = 0; u < XUSER_MAX_COUNT; u++) {
            XINPUT_CAPABILITIES_EX c;
            XINPUT_STATE s;
            if (pGetCapsEx(1, u, 0, &c) != ERROR_SUCCESS) { printf("XInput user %lu: not connected\n", (unsigned long)u); continue; }
            pGetStateEx(u, &s);
            printf("XInput user %lu: %04x:%04x buttons %04x triggers %u %u sticks %d,%d %d,%d (packet %lu)\n",
                   (unsigned long)u, c.VendorId, c.ProductId, s.Gamepad.wButtons, s.Gamepad.bLeftTrigger,
                   s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX,
                   s.Gamepad.sThumbRY, (unsigned long)s.dwPacketNumber);
        }
    IDirectInput8W *di;
    if (FAILED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&di, NULL))) return;
    g_nfound = 0;
    di->lpVtbl->EnumDevices(di, DI8DEVCLASS_ALL, found_one, NULL, DIEDFL_ATTACHEDONLY);
    for (int i = 0; i < g_nfound; i++) {
        char name[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, g_found[i].name, -1, name, sizeof(name), NULL, NULL);
        printf("DirectInput: %s (type %02lx, %04lx:%04lx)\n", name, (unsigned long)(g_found[i].type & 0xFF),
               (unsigned long)(g_found[i].pidvid & 0xFFFF), (unsigned long)(g_found[i].pidvid >> 16));
    }
    di->lpVtbl->Release(di);
}

/* 32-bit, after "padtest": the Xbox One controller still as step 2, the
 * HID game pad as step 4, the Xbox 360 controller gone */
static int still(void)
{
    check(load_xinput(), "xinput1_4.dll loads, with ordinals 100 and 108");
    if (g_fail) return 1;
    int one = xinput_user(PID_ONE), n = 0;
    for (DWORD u = 0; u < XUSER_MAX_COUNT; u++) {
        XINPUT_STATE s;
        if (pGetState(u, &s) == ERROR_SUCCESS) n++;
    }
    check(one >= 0 && n == 1, "XInput: one controller, the Xbox One one (user %d)", one);
    XINPUT_STATE s, sx;
    if (one >= 0 && pGetState((DWORD)one, &s) == ERROR_SUCCESS && pGetStateEx((DWORD)one, &sx) == ERROR_SUCCESS) {
        check(s.Gamepad.wButtons == 0xA210 && sx.Gamepad.wButtons == 0xA610, "XInput Xbox One buttons %04x, with Guide %04x",
              s.Gamepad.wButtons, sx.Gamepad.wButtons);
        check(s.Gamepad.bRightTrigger == 255 && s.Gamepad.sThumbRX == -30000 && s.Gamepad.sThumbRY == 30000,
              "XInput Xbox One right trigger %u, right stick %d,%d", s.Gamepad.bRightTrigger, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
    }
    IDirectInput8W *di;
    check(SUCCEEDED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&di, NULL)),
          "DirectInput8Create");
    if (g_fail) return 1;
    make_formats();
    int npads = enum_pads(di);
    check(npads == 2 && found_pidvid(VP_ONE) && found_pidvid(VP_HID) && !found_pidvid(VP_360),
          "DirectInput: two game controllers, the Xbox One one and the HID game pad (%d)", npads);
    IDirectInputDevice8W *hid = open_pad(di, VP_HID);
    DIJOYSTATE js;
    if (hid && SUCCEEDED(hid->lpVtbl->Acquire(hid)) && joy_state(hid, &js)) {
        buttons_are(&js, 0x0806, "HID game pad");
        check(js.lX == 65535 && js.lY == 0 && js.rgdwPOV[0] == 9000, "HID game pad X %ld Y %ld hat %lu",
              (long)js.lX, (long)js.lY, (unsigned long)js.rgdwPOV[0]);
    } else {
        check(0, "HID game pad: device made, acquired and read");
    }
    if (hid) hid->lpVtbl->Release(hid);
    di->lpVtbl->Release(di);
    printf("padtest still: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "list")) { list(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "still")) return still();

    check(load_xinput(), "xinput1_4.dll loads, with ordinals 100 and 108");
    if (g_fail) return 1;
    make_formats();

    /* ---- what the APIs list ---- */
    int u360 = xinput_user(PID_360), uone = xinput_user(PID_ONE), n = 0;
    for (DWORD u = 0; u < XUSER_MAX_COUNT; u++) {
        XINPUT_STATE s;
        if (pGetState(u, &s) == ERROR_SUCCESS) n++;
    }
    check(u360 >= 0 && uone >= 0 && n == 2, "XInput: the two Xbox controllers (users %d and %d), not the HID game pad (%d connected)",
          u360, uone, n);
    XINPUT_CAPABILITIES caps;
    check(u360 >= 0 && pGetCaps((DWORD)u360, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_SUCCESS && caps.Type == XINPUT_DEVTYPE_GAMEPAD &&
          caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD && caps.Vibration.wLeftMotorSpeed, "XInputGetCapabilities: a gamepad with motors");
    {
        XINPUT_STATE s;
        DWORD r = pGetState(3, &s), r2 = pGetState(4, &s);
        check(r == ERROR_DEVICE_NOT_CONNECTED && r2 == ERROR_BAD_ARGUMENTS, "XInputGetState: user 3 not connected (%lu), user 4 bad (%lu)",
              (unsigned long)r, (unsigned long)r2);
    }
    if (u360 < 0 || uone < 0) { printf("padtest: %d passed, %d failed\n", g_pass, g_fail); return 1; }

    IDirectInput8W *di = NULL;
    check(SUCCEEDED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&di, NULL)),
          "DirectInput8Create");
    if (!di) return 1;
    int npads = enum_pads(di);
    check(npads == 3 && found_pidvid(VP_360) && found_pidvid(VP_ONE) && found_pidvid(VP_HID),
          "DirectInput: three game controllers, by product GUID (PIDVID) (%d)", npads);
    {
        const Found *f = found_pidvid(VP_360), *h = found_pidvid(VP_HID);
        check(f && !lstrcmpW(f->name, L"Controller (XBOX 360 For Windows)") && GET_DIDEVICE_TYPE(f->type) == DI8DEVTYPE_GAMEPAD,
              "DirectInput: the Xbox 360 controller as Windows names it, a gamepad");
        check(h && !lstrcmpW(h->name, L"NovaOS Test Gamepad"), "DirectInput: the HID game pad by its product string");
    }
    g_nfound = 0;
    di->lpVtbl->EnumDevices(di, DI8DEVCLASS_ALL, found_one, NULL, DIEDFL_ATTACHEDONLY);
    check(g_nfound == 5 && IsEqualGUID(&g_found[0].inst, &GUID_SysKeyboard) && IsEqualGUID(&g_found[1].inst, &GUID_SysMouse),
          "DirectInput: all devices are the keyboard, the mouse and the controllers (%d)", g_nfound);
    enum_pads(di);
    {
        IDirectInput8W *co = NULL;
        CoInitialize(NULL);
        HRESULT hr = CoCreateInstance(&CLSID_DirectInput8, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectInput8W, (void **)&co);
        HRESULT before = co ? co->lpVtbl->EnumDevices(co, DI8DEVCLASS_GAMECTRL, found_one, NULL, 0) : E_FAIL;
        HRESULT init = co ? co->lpVtbl->Initialize(co, GetModuleHandleW(NULL), DIRECTINPUT_VERSION) : E_FAIL;
        int count = 0;
        if (co && SUCCEEDED(init)) {
            Found keep[8];
            int nkeep = g_nfound;
            memcpy(keep, g_found, sizeof(keep));
            count = enum_pads(co);
            memcpy(g_found, keep, sizeof(keep));
            g_nfound = nkeep;
        }
        check(SUCCEEDED(hr) && before == DIERR_NOTINITIALIZED && SUCCEEDED(init) && count == 3,
              "CoCreateInstance(CLSID_DirectInput8), uninitialized until Initialize, then three controllers (%08lx %08lx %d)",
              (unsigned long)hr, (unsigned long)before, count);
        if (co) co->lpVtbl->Release(co);
    }

    IDirectInputDevice8W *d360 = open_pad(di, VP_360), *done = open_pad(di, VP_ONE);
    IDirectInputDevice8A *dhid = NULL;
    {
        IDirectInputDevice8W *w = open_pad(di, VP_HID);
        if (w) { w->lpVtbl->QueryInterface(w, &IID_IDirectInputDevice8A, (void **)&dhid); w->lpVtbl->Release(w); }
    }
    check(d360 && done && dhid, "DirectInput: a device for each controller, c_dfDIJoystick set");
    if (!d360 || !done || !dhid) { printf("padtest: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    {
        DIPROPGUIDANDPATH gp = { { sizeof(gp), sizeof(DIPROPHEADER), 0, DIPH_DEVICE } };
        HRESULT hr = d360->lpVtbl->GetProperty(d360, DIPROP_GUIDANDPATH, &gp.diph);
        check(SUCCEEDED(hr) && wcsstr(gp.wszPath, L"&ig_00"), "DIPROP_GUIDANDPATH: the Xbox 360 controller's path has IG_");
        hr = dhid->lpVtbl->GetProperty(dhid, DIPROP_GUIDANDPATH, &gp.diph);
        check(SUCCEEDED(hr) && !wcsstr(gp.wszPath, L"&ig_"), "DIPROP_GUIDANDPATH: the HID game pad's has not");
        DIPROPDWORD vp = { { sizeof(vp), sizeof(DIPROPHEADER), 0, DIPH_DEVICE } };
        hr = dhid->lpVtbl->GetProperty(dhid, DIPROP_VIDPID, &vp.diph);
        check(SUCCEEDED(hr) && vp.dwData == VP_HID, "DIPROP_VIDPID %08lx", (unsigned long)vp.dwData);
        DIPROPRANGE r = { { sizeof(r), sizeof(DIPROPHEADER), 0, DIPH_DEVICE }, -1000, 1000 };
        check(SUCCEEDED(d360->lpVtbl->SetProperty(d360, DIPROP_RANGE, &r.diph)), "DIPROP_RANGE -1000..1000 on the Xbox 360 controller");
        DIPROPDWORD buf = { { sizeof(buf), sizeof(DIPROPHEADER), 0, DIPH_DEVICE }, 16 };
        check(SUCCEEDED(dhid->lpVtbl->SetProperty(dhid, DIPROP_BUFFERSIZE, &buf.diph)), "DIPROP_BUFFERSIZE 16 on the HID game pad");
        DIDEVCAPS c = { sizeof(c) };
        dhid->lpVtbl->GetCapabilities(dhid, &c);
        memset(g_objs, 0, sizeof(g_objs));
        dhid->lpVtbl->EnumObjects(dhid, count_obj, NULL, DIDFT_ALL);
        check(c.dwAxes == 4 && c.dwButtons == 16 && c.dwPOVs == 1 && g_objs[0] == 4 && g_objs[1] == 16 && g_objs[2] == 1,
              "HID game pad: 4 axes, 16 buttons, a hat (caps %lu %lu %lu, objects %d %d %d)", (unsigned long)c.dwAxes,
              (unsigned long)c.dwButtons, (unsigned long)c.dwPOVs, g_objs[0], g_objs[1], g_objs[2]);
        c.dwSize = sizeof(c);
        d360->lpVtbl->GetCapabilities(d360, &c);
        check(c.dwAxes == 5 && c.dwButtons == 10 && c.dwPOVs == 1 && (c.dwFlags & DIDC_ATTACHED),
              "Xbox 360 controller: 5 axes, 10 buttons, a hat, attached");
    }
    check(SUCCEEDED(d360->lpVtbl->Acquire(d360)) && SUCCEEDED(done->lpVtbl->Acquire(done)) && SUCCEEDED(dhid->lpVtbl->Acquire(dhid)),
          "Acquire");
    {
        IDirectInputDevice8W *kbd = NULL;
        BYTE keys[256];
        HRESULT hr = di->lpVtbl->CreateDevice(di, &GUID_SysKeyboard, &kbd, NULL);
        if (SUCCEEDED(hr)) hr = kbd->lpVtbl->SetDataFormat(kbd, &c_dfDIKeyboard);
        if (SUCCEEDED(hr)) hr = kbd->lpVtbl->Acquire(kbd);
        if (SUCCEEDED(hr)) hr = kbd->lpVtbl->GetDeviceState(kbd, sizeof(keys), keys);
        check(SUCCEEDED(hr), "the keyboard: c_dfDIKeyboard, acquired, read (%08lx)", (unsigned long)hr);
        if (kbd) kbd->lpVtbl->Release(kbd);
    }

    /* ---- 1. the Xbox 360 controller ---- */
    XINPUT_STATE s;
    DIJOYSTATE js;
    printf("[PAD] step 1: Xbox 360: A, LB, D-pad up+right, left trigger 200, left stick 20000,-10000\n");
    WAIT_FOR(pGetState((DWORD)u360, &s) == ERROR_SUCCESS && s.Gamepad.wButtons == 0x1109);
    check(s.Gamepad.wButtons == 0x1109 && s.Gamepad.bLeftTrigger == 200 && s.Gamepad.bRightTrigger == 0 &&
          s.Gamepad.sThumbLX == 20000 && s.Gamepad.sThumbLY == -10000 && s.Gamepad.sThumbRX == 0,
          "XInput Xbox 360: buttons %04x, triggers %u %u, left stick %d,%d", s.Gamepad.wButtons, s.Gamepad.bLeftTrigger,
          s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY);
    {
        WORD downs[8];
        int nd = 0;
        XINPUT_KEYSTROKE k;
        while (nd < 8 && pGetKeystroke((DWORD)u360, 0, &k) == ERROR_SUCCESS)
            if (k.Flags & XINPUT_KEYSTROKE_KEYDOWN) downs[nd++] = k.VirtualKey;
        int a = 0, lb = 0, up = 0, lt = 0;
        for (int i = 0; i < nd; i++) {
            a |= downs[i] == VK_PAD_A; lb |= downs[i] == VK_PAD_LSHOULDER;
            up |= downs[i] == VK_PAD_DPAD_UP; lt |= downs[i] == VK_PAD_LTRIGGER;
        }
        check(nd == 5 && a && lb && up && lt, "XInputGetKeystroke: A, LB, up, right and the left trigger went down (%d keys)", nd);
    }
    WAIT_FOR(joy_state(d360, &js) && js.rgbButtons[0]);
    check(near(js.lX, scaled(-1000, 1000, 52768)) && near(js.lY, scaled(-1000, 1000, 42767)) &&
          near(js.lZ, scaled(-1000, 1000, 58368)) && near(js.lRx, 0) && near(js.lRy, 0),
          "DirectInput Xbox 360: X %ld Y %ld Z %ld Rx %ld Ry %ld (range -1000..1000)", (long)js.lX, (long)js.lY, (long)js.lZ,
          (long)js.lRx, (long)js.lRy);
    buttons_are(&js, 0x11, "Xbox 360");
    check(js.rgdwPOV[0] == 4500 && js.rgdwPOV[1] == 0xFFFFFFFF, "DirectInput Xbox 360 hat %lu (45 degrees), no second hat",
          (unsigned long)js.rgdwPOV[0]);

    /* ---- 2. the Xbox One controller ---- */
    printf("[PAD] step 2: Xbox One: B, Y, Menu, RB, Guide, right trigger, right stick -30000,30000\n");
    XINPUT_STATE sx;
    WAIT_FOR(pGetStateEx((DWORD)uone, &sx) == ERROR_SUCCESS && sx.Gamepad.wButtons == 0xA610);
    pGetState((DWORD)uone, &s);
    check(s.Gamepad.wButtons == 0xA210 && sx.Gamepad.wButtons == 0xA610, "XInput Xbox One: buttons %04x, with Guide (ordinal 100) %04x",
          s.Gamepad.wButtons, sx.Gamepad.wButtons);
    check(s.Gamepad.bRightTrigger == 255 && s.Gamepad.bLeftTrigger == 0 && s.Gamepad.sThumbRX == -30000 && s.Gamepad.sThumbRY == 30000,
          "XInput Xbox One: right trigger %u, right stick %d,%d", s.Gamepad.bRightTrigger, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
    WAIT_FOR(joy_state(done, &js) && js.rgbButtons[1]);
    check(js.lRx == 2768 && js.lRy == 2767 && js.lZ == 128 && js.lX == 32768, "DirectInput Xbox One: Rx %ld Ry %ld Z %ld X %ld",
          (long)js.lRx, (long)js.lRy, (long)js.lZ, (long)js.lX);
    buttons_are(&js, 0xAA, "Xbox One");
    check(js.rgdwPOV[0] == 0xFFFFFFFF, "DirectInput Xbox One hat centred");

    /* ---- 3. the HID game pad ---- */
    printf("[PAD] step 3: HID game pad: buttons 1, 3, 12, hat east, X 255, Y 0, Rz 64\n");
    WAIT_FOR(SUCCEEDED(dhid->lpVtbl->GetDeviceState(dhid, sizeof(js), &js)) && js.rgbButtons[11]);
    buttons_are(&js, 0x0805, "HID game pad");
    check(js.lX == 65535 && js.lY == 0 && js.lZ == 32896 && js.lRz == 16448 && js.rgdwPOV[0] == 9000,
          "DirectInput HID game pad: X %ld Y %ld Z %ld Rz %ld hat %lu", (long)js.lX, (long)js.lY, (long)js.lZ, (long)js.lRz,
          (unsigned long)js.rgdwPOV[0]);
    n = 0;
    for (DWORD u = 0; u < XUSER_MAX_COUNT; u++) if (pGetState(u, &s) == ERROR_SUCCESS) n++;
    check(n == 2, "XInput still lists only the two Xbox controllers (%d)", n);
    DIDEVICEOBJECTDATA od[16];
    DWORD got = 16;
    HRESULT hr = dhid->lpVtbl->GetDeviceData(dhid, sizeof(od[0]), od, &got, 0);
    {
        int b0 = 0, b2 = 0, b11 = 0, pov = 0;
        for (DWORD i = 0; i < got; i++) {
            b0 |= od[i].dwOfs == DIJOFS_BUTTON(0) && od[i].dwData == 0x80;
            b2 |= od[i].dwOfs == DIJOFS_BUTTON(2) && od[i].dwData == 0x80;
            b11 |= od[i].dwOfs == DIJOFS_BUTTON(11) && od[i].dwData == 0x80;
            pov |= od[i].dwOfs == DIJOFS_POV(0) && od[i].dwData == 9000;
        }
        check(hr == DI_OK && b0 && b2 && b11 && pov, "GetDeviceData: buttons 1, 3, 12 down and the hat (%lu events)", (unsigned long)got);
    }

    /* ---- 4. buffered changes ---- */
    printf("[PAD] step 4: HID game pad: button 1 up, button 2 down\n");
    WAIT_FOR(SUCCEEDED(dhid->lpVtbl->GetDeviceState(dhid, sizeof(js), &js)) && js.rgbButtons[1]);
    got = 16;
    hr = dhid->lpVtbl->GetDeviceData(dhid, sizeof(od[0]), od, &got, 0);
    check(hr == DI_OK && got == 2 && od[0].dwOfs == DIJOFS_BUTTON(0) && od[0].dwData == 0 &&
          od[1].dwOfs == DIJOFS_BUTTON(1) && od[1].dwData == 0x80 && od[1].dwSequence > od[0].dwSequence,
          "GetDeviceData: exactly button 1 up, then button 2 down (%lu events)", (unsigned long)got);

    /* ---- 5. the motors ---- */
    XINPUT_VIBRATION v360 = { 0x8000, 0x4000 }, vone = { 0xFFFF, 0 };
    DWORD r1 = pSetState((DWORD)u360, &v360), r2 = pSetState((DWORD)uone, &vone);
    check(r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS, "XInputSetState on both Xbox controllers");
    printf("[PAD] motors set\n");
    Sleep(500);

    /* ---- 6. unplugged ---- */
    printf("[PAD] step 6: unplug the Xbox 360 controller\n");
    WAIT_FOR(pGetState((DWORD)u360, &s) == ERROR_DEVICE_NOT_CONNECTED);
    check(pGetState((DWORD)u360, &s) == ERROR_DEVICE_NOT_CONNECTED, "XInput: the Xbox 360 controller's user is not connected");
    check(pGetState((DWORD)uone, &s) == ERROR_SUCCESS && s.Gamepad.wButtons == 0xA210, "XInput: the Xbox One controller keeps its user");
    hr = d360->lpVtbl->GetDeviceState(d360, sizeof(js), &js);
    check(hr == DIERR_INPUTLOST, "DirectInput: the Xbox 360 controller's device says DIERR_INPUTLOST (%08lx)", (unsigned long)hr);
    npads = enum_pads(di);
    check(npads == 2, "DirectInput: two game controllers left (%d)", npads);

    d360->lpVtbl->Release(d360);
    done->lpVtbl->Release(done);
    dhid->lpVtbl->Release(dhid);
    di->lpVtbl->Release(di);
    printf("padtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
