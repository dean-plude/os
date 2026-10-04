/*
 * dinput8.dll — DirectInput 8: the keyboard, the mouse and game controllers
 *
 * IDirectInput8 lists the system keyboard and mouse and every game
 * controller the kernel has (kernel/drivers/gamepad.h: Xbox controllers
 * and HID gamepads and joysticks, read with NtNovaGuiCtl CTL_GAMEPAD,
 * novapad.h), and makes an IDirectInputDevice8 for each.  A device is a
 * list of objects (axes, buttons, a point-of-view hat, keys), each with a
 * type GUID, a DIDFT_* type and an instance number; SetDataFormat matches
 * a program's data format (c_dfDIJoystick, c_dfDIKeyboard, ... which
 * programs carry themselves, from dinput8.lib) against them the way
 * DirectInput does: an entry takes the first object not yet taken whose
 * type GUID (if the entry names one), type and instance (unless "any")
 * match, and an entry nothing matches is skipped if it is optional.
 * GetDeviceState then writes each matched object's value at its entry's
 * offset: axes over their range (0-65535 unless DIPROP_RANGE says
 * otherwise; dead zone and saturation applied), buttons 0x80 when down,
 * the hat in hundredths of a degree (-1 centred).  With DIPROP_BUFFERSIZE
 * set, the changes are kept for GetDeviceData; a device with an event
 * (SetEventNotification) is watched by a thread that sets it on a change.
 *
 * Controllers appear as Windows shows them: an Xbox controller as
 * "Controller (XBOX 360 For Windows)" with X/Y the left stick, Rx/Ry the
 * right, Z both triggers, ten buttons and a hat, its DIPROP_GUIDANDPATH
 * holding "IG_" (which programs look for to leave XInput devices to
 * XInput); product GUIDs carry the USB vendor and product ("PIDVID").
 * The keyboard is read from the program's key state (GetAsyncKeyState),
 * the mouse from the cursor's movement.  There is no force feedback and
 * no action mapping (DIERR_UNSUPPORTED).
 */
#define DINPUTAPI __declspec(dllexport)
#include <dinput.h>
#include <novapad.h>
#include <string.h>

#ifndef VK_NUMPAD0
#define VK_NUMPAD0 0x60
#endif
#define VK_NUMPAD(n) (VK_NUMPAD0 + (n))

#define CONTAINER(p, T, m) ((T *)((char *)(p) - __builtin_offsetof(T, m)))

DEFINE_GUID(GUID_HIDClass, 0x745A17A0, 0x74D3, 0x11D0, 0xB6, 0xFE, 0x00, 0xA0, 0xC9, 0x0F, 0x57, 0xDA);

enum { DEV_KBD, DEV_MOUSE, DEV_PAD };

/* ---------------------------------------------------------------------------
 * Objects
 * ------------------------------------------------------------------------- */

typedef struct {
    const GUID *guid;                 /* type GUID */
    DWORD type;                       /* DIDFT_* | DIDFT_MAKEINSTANCE(n) */
    WORD  page, usage;                /* HID usage */
    int   src;                        /* pad: axis index, button index or 0 (hat); keyboard: scan code; mouse: 0-2 axes, 3+ buttons */
    WCHAR name[24];
} Obj;

#define MAX_OBJS 256

static const GUID *const g_axis_guid[NOVA_PAD_AXES] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis,
                                                        &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider };
static const WCHAR *const g_axis_name[NOVA_PAD_AXES] = { L"X Axis", L"Y Axis", L"Z Axis", L"X Rotation",
                                                         L"Y Rotation", L"Z Rotation", L"Slider", L"Dial" };

/* Scan codes DirectInput numbers keys by (DIK_*: E0-prefixed keys + 0x80)
 * and the virtual keys whose state they read */
static UINT key_vk(int sc)
{
    switch (sc) {
    case 0x1D: return VK_LCONTROL;  case 0x2A: return VK_LSHIFT;  case 0x36: return VK_RSHIFT;
    case 0x38: return VK_LMENU;     case 0x45: return VK_NUMLOCK; case 0x46: return VK_SCROLL;
    case 0x47: return VK_NUMPAD(7);   case 0x48: return VK_NUMPAD(8); case 0x49: return VK_NUMPAD(9);
    case 0x4B: return VK_NUMPAD(4);   case 0x4C: return VK_NUMPAD(5); case 0x4D: return VK_NUMPAD(6);
    case 0x4F: return VK_NUMPAD(1);   case 0x50: return VK_NUMPAD(2); case 0x51: return VK_NUMPAD(3);
    case 0x52: return VK_NUMPAD(0);   case 0x53: return VK_DECIMAL;
    case 0x9C: return VK_RETURN;    case 0x9D: return VK_RCONTROL; case 0xB5: return VK_DIVIDE;
    case 0xB7: return VK_SNAPSHOT;  case 0xB8: return VK_RMENU;   case 0xC5: return VK_PAUSE;
    case 0xC7: return VK_HOME;      case 0xC8: return VK_UP;      case 0xC9: return VK_PRIOR;
    case 0xCB: return VK_LEFT;      case 0xCD: return VK_RIGHT;   case 0xCF: return VK_END;
    case 0xD0: return VK_DOWN;      case 0xD1: return VK_NEXT;    case 0xD2: return VK_INSERT;
    case 0xD3: return VK_DELETE;    case 0xDB: return VK_LWIN;    case 0xDC: return VK_RWIN;
    case 0xDD: return VK_APPS;
    }
    return sc > 0 && sc < 0x80 ? MapVirtualKeyW((UINT)sc, 1 /* MAPVK_VSC_TO_VK */) : 0;
}

/* ---------------------------------------------------------------------------
 * Devices
 * ------------------------------------------------------------------------- */

typedef struct { int obj; DWORD ofs; } Map;

typedef struct Dev {
    IDirectInputDevice8A ia;
    IDirectInputDevice8W iw;
    LONG    ref;
    int     kind;
    int     slot;                     /* pad: its slot, and the controller it was made for */
    DWORD   serial;
    NovaPadInfo info;
    Obj     objs[MAX_OBJS];
    int     nobj;
    LONG    lmin[MAX_OBJS], lmax[MAX_OBJS];
    DWORD   dz[MAX_OBJS], sat[MAX_OBJS];
    UINT_PTR appdata[MAX_OBJS];
    int     ofs[MAX_OBJS];            /* each object's offset in the data format (-1: not in it) */
    BOOL    have_fmt;
    DWORD   fmt_size;
    Map    *map;                      /* the format's entries that matched an object */
    int     nmap;
    DWORD  *pov_unmapped;             /* the offsets of POV entries nothing matched (-1 there) */
    int     npov_unmapped;
    BOOL    relative;                 /* the axes report movement (the mouse's default) */
    BOOL    acquired, lost;
    DWORD   val[MAX_OBJS];            /* each object's value as last seen */
    LONG    acc[3];                   /* mouse: movement not yet read by GetDeviceState */
    POINT   cursor;
    DWORD   autocenter;
    /* buffered data */
    DWORD   bufsize;
    DIDEVICEOBJECTDATA *buf;
    DWORD   nbuf, head;
    BOOL    overflow;
    /* SetEventNotification */
    HANDLE  event, thread;
    volatile LONG stop;
    CRITICAL_SECTION cs;
} Dev;

static volatile LONG g_sequence;

static void add_obj(Dev *d, const GUID *g, DWORD type, int inst, WORD page, WORD usage, int src, const WCHAR *name)
{
    if (d->nobj >= MAX_OBJS) return;
    int i = d->nobj++;
    Obj *o = &d->objs[i];
    o->guid = g;
    o->type = type | DIDFT_MAKEINSTANCE(inst);
    o->page = page; o->usage = usage;
    o->src = src;
    lstrcpynW(o->name, name, 24);
    d->lmin[i] = 0; d->lmax[i] = 65535;
    d->dz[i] = 0; d->sat[i] = 10000;
    d->ofs[i] = -1;
}

static void build_objects(Dev *d)
{
    WCHAR n[24];
    if (d->kind == DEV_PAD) {
        int inst = 0;
        for (int a = 0; a < NOVA_PAD_AXES; a++)
            if (d->info.axes & (1 << a)) add_obj(d, g_axis_guid[a], DIDFT_ABSAXIS, inst++, 1, (WORD)(0x30 + a), a, g_axis_name[a]);
        if (d->info.povs) add_obj(d, &GUID_POV, DIDFT_POV, 0, 1, 0x39, 0, L"Hat Switch");
        for (int b = 0; b < d->info.buttons; b++) {
            wsprintfW(n, L"Button %d", b);
            add_obj(d, &GUID_Button, DIDFT_PSHBUTTON, b, 9, (WORD)(b + 1), b, n);
        }
    } else if (d->kind == DEV_MOUSE) {
        add_obj(d, &GUID_XAxis, DIDFT_RELAXIS, 0, 1, 0x30, 0, L"X-axis");
        add_obj(d, &GUID_YAxis, DIDFT_RELAXIS, 1, 1, 0x31, 1, L"Y-axis");
        add_obj(d, &GUID_ZAxis, DIDFT_RELAXIS, 2, 1, 0x38, 2, L"Wheel");
        static const WCHAR *const bn[5] = { L"Button 0", L"Button 1", L"Button 2", L"Button 3", L"Button 4" };
        for (int b = 0; b < 5; b++) add_obj(d, &GUID_Button, DIDFT_PSHBUTTON, 3 + b, 9, (WORD)(b + 1), 3 + b, bn[b]);
        d->relative = TRUE;
    } else {
        for (int sc = 1; sc < 256; sc++) {
            if (!key_vk(sc)) continue;
            if (!GetKeyNameTextW((LONG)((sc & 0x7F) << 16 | (sc & 0x80 ? 1 << 24 : 0)), n, 24)) wsprintfW(n, L"Key %d", sc);
            add_obj(d, &GUID_Key, DIDFT_PSHBUTTON, sc, 7, 0, sc, n);
        }
    }
}

/* The controller the device was made for is still in its slot */
static BOOL pad_here(Dev *d, NovaPadState *st)
{
    NovaPadInfo i;
    if (!nova_pad_info(d->slot, &i) || i.serial != d->serial) return FALSE;
    return nova_pad_state(d->slot, st);
}

static DWORD axis_value(Dev *d, int i, WORD raw)
{
    LONG lo = d->lmin[i], hi = d->lmax[i];
    if (d->dz[i] == 0 && d->sat[i] >= 10000)
        return (DWORD)(lo + (LONG)(((LONGLONG)raw * ((LONGLONG)hi - lo) + 32767) / 65535));
    LONG dist = (LONG)raw - 32768;                                 /* -32768..32767 */
    LONG mag = (LONG)((LONGLONG)(dist < 0 ? -dist : dist) * 10000 / 32768);
    LONG dz = (LONG)d->dz[i], sat = (LONG)(d->sat[i] > 10000 ? 10000 : d->sat[i]);
    LONG v = mag <= dz ? 0 : mag >= sat || sat <= dz ? 10000 : (mag - dz) * 10000 / (sat - dz);
    if (dist < 0) v = -v;                                         /* -10000..10000 */
    return (DWORD)(lo + (LONG)(((LONGLONG)(v + 10000) * ((LONGLONG)hi - lo) + 10000) / 20000));
}

static void push(Dev *d, int obj, DWORD data)
{
    if (!d->bufsize || d->ofs[obj] < 0) return;
    if (d->nbuf == d->bufsize) { d->overflow = TRUE; return; }
    DIDEVICEOBJECTDATA *e = &d->buf[(d->head + d->nbuf++) % d->bufsize];
    e->dwOfs = (DWORD)d->ofs[obj];
    e->dwData = data;
    e->dwTimeStamp = GetTickCount();
    e->dwSequence = (DWORD)InterlockedIncrement(&g_sequence);
    e->uAppData = d->appdata[obj];
}

/* Read the device now: each object's value into d->val, changes into the
 * buffer; FALSE when a controller has gone (under d->cs) */
static BOOL refresh(Dev *d)
{
    NovaPadState st;
    POINT pt = d->cursor;
    if (d->kind == DEV_PAD && !pad_here(d, &st)) return FALSE;
    LONG move[3] = { 0, 0, 0 };
    if (d->kind == DEV_MOUSE) {
        GetCursorPos(&pt);
        move[0] = pt.x - d->cursor.x;
        move[1] = pt.y - d->cursor.y;
        d->cursor = pt;
    }
    for (int i = 0; i < d->nobj; i++) {
        const Obj *o = &d->objs[i];
        DWORD v = 0;
        if (d->kind == DEV_PAD) {
            if (o->type & DIDFT_AXIS) v = axis_value(d, i, st.axis[o->src]);
            else if (o->type & DIDFT_POV) v = st.pov < 0 ? 0xFFFFFFFF : (DWORD)st.pov;
            else v = st.buttons & (1u << o->src) ? 0x80 : 0;
        } else if (d->kind == DEV_MOUSE) {
            if (o->src < 3) {
                d->acc[o->src] += move[o->src];
                if (move[o->src]) push(d, i, (DWORD)move[o->src]);
                if (!d->relative) v = (DWORD)(o->src == 0 ? pt.x : o->src == 1 ? pt.y : 0);
                else continue;
            } else {
                static const int vk[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
                v = GetAsyncKeyState(vk[o->src - 3]) & 0x8000 ? 0x80 : 0;
            }
        } else {
            v = GetAsyncKeyState((int)key_vk(o->src)) & 0x8000 ? 0x80 : 0;
        }
        if (v != d->val[i]) {
            d->val[i] = v;
            push(d, i, v);
        }
    }
    return TRUE;
}

/* The values when the device was acquired: no changes are reported for
 * what was already so */
static void baseline(Dev *d)
{
    DWORD keep = d->bufsize;
    d->bufsize = 0;
    GetCursorPos(&d->cursor);
    for (int i = 0; i < d->nobj; i++) d->val[i] = (d->objs[i].type & DIDFT_POV) ? 0xFFFFFFFF : 0;
    refresh(d);
    d->acc[0] = d->acc[1] = d->acc[2] = 0;
    d->bufsize = keep;
}

static DWORD WINAPI watch(void *arg)
{
    Dev *d = arg;
    DWORD packet = 0;
    while (!d->stop) {
        Sleep(8);
        EnterCriticalSection(&d->cs);
        if (d->acquired && d->event) {
            DWORD before = d->nbuf;
            BOOL changed = FALSE;
            if (d->kind == DEV_PAD) {
                NovaPadState st;
                if (!pad_here(d, &st)) { if (!d->lost) { d->lost = TRUE; changed = TRUE; } }
                else if (st.packet != packet) { packet = st.packet; DWORD v[MAX_OBJS]; memcpy(v, d->val, sizeof(v));
                                                 refresh(d); changed = memcmp(v, d->val, sizeof(v)) != 0; }
            } else {
                DWORD v[MAX_OBJS];
                memcpy(v, d->val, sizeof(v));
                LONG a0 = d->acc[0], a1 = d->acc[1];
                refresh(d);
                changed = memcmp(v, d->val, sizeof(v)) != 0 || d->acc[0] != a0 || d->acc[1] != a1;
            }
            if (changed || d->nbuf != before) SetEvent(d->event);
        }
        LeaveCriticalSection(&d->cs);
    }
    return 0;
}

static Dev *new_dev(int kind, int slot)
{
    Dev *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Dev));
    if (!d) return NULL;
    d->kind = kind;
    d->slot = slot;
    d->ref = 1;
    if (kind == DEV_PAD) {
        if (!nova_pad_info(slot, &d->info)) { HeapFree(GetProcessHeap(), 0, d); return NULL; }
        d->serial = d->info.serial;
    }
    InitializeCriticalSection(&d->cs);
    build_objects(d);
    return d;
}

/* ---- device facts ---- */

static void instance_guid(int slot, const NovaPadInfo *i, GUID *g)
{
    static const GUID base = { 0x9E573ED8, 0x7734, 0x11D2, { 0x8D, 0x4A, 0x23, 0x90, 0x3F, 0xB6, 0xBD, 0xF7 } };
    *g = base;
    g->Data1 += (DWORD)slot;
    g->Data4[7] = (BYTE)(0xF7 ^ (i->serial & 0xFF));
}

static void product_guid(const NovaPadInfo *i, GUID *g)
{
    static const GUID pidvid = { 0, 0, 0, { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' } };
    *g = pidvid;
    g->Data1 = (DWORD)MAKELONG(i->vid, i->pid);
}

static DWORD dev_type(int kind, const NovaPadInfo *i)
{
    if (kind == DEV_KBD) return DI8DEVTYPE_KEYBOARD | DI8DEVTYPEKEYBOARD_PCENH << 8;
    if (kind == DEV_MOUSE) return DI8DEVTYPE_MOUSE | DI8DEVTYPEMOUSE_TRADITIONAL << 8;
    return ((i->usage & 0xFF) == 4 ? DI8DEVTYPE_JOYSTICK | DI8DEVTYPEJOYSTICK_STANDARD << 8
                                    : DI8DEVTYPE_GAMEPAD | DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
}

static void device_instance(int kind, int slot, const NovaPadInfo *i, DIDEVICEINSTANCEW *di)
{
    ZeroMemory(di, sizeof(*di));
    di->dwSize = sizeof(*di);
    if (kind == DEV_KBD) {
        di->guidInstance = di->guidProduct = GUID_SysKeyboard;
        lstrcpyW(di->tszInstanceName, L"Keyboard");
        lstrcpyW(di->tszProductName, L"Keyboard");
        di->wUsagePage = 1; di->wUsage = 6;
    } else if (kind == DEV_MOUSE) {
        di->guidInstance = di->guidProduct = GUID_SysMouse;
        lstrcpyW(di->tszInstanceName, L"Mouse");
        lstrcpyW(di->tszProductName, L"Mouse");
        di->wUsagePage = 1; di->wUsage = 2;
    } else {
        instance_guid(slot, i, &di->guidInstance);
        product_guid(i, &di->guidProduct);
        MultiByteToWideChar(CP_ACP, 0, i->name, -1, di->tszInstanceName, MAX_PATH);
        lstrcpyW(di->tszProductName, di->tszInstanceName);
        di->wUsagePage = 1; di->wUsage = (WORD)(i->usage & 0xFF);
    }
    di->dwDevType = dev_type(kind, i);
}

static void obj_instance(Dev *d, int i, DIDEVICEOBJECTINSTANCEW *oi)
{
    const Obj *o = &d->objs[i];
    ZeroMemory(oi, sizeof(*oi));
    oi->dwSize = sizeof(*oi);
    oi->guidType = *o->guid;
    oi->dwOfs = d->ofs[i] >= 0 ? (DWORD)d->ofs[i] :
                d->kind == DEV_KBD ? (DWORD)o->src :
                (o->type & DIDFT_AXIS) ? (DWORD)(DIDFT_GETINSTANCE(o->type) * 4) :
                (o->type & DIDFT_POV) ? (DWORD)DIJOFS_POV(0) : (DWORD)DIJOFS_BUTTON(o->src);
    oi->dwType = o->type;
    oi->dwFlags = (o->type & DIDFT_AXIS) ? DIDOI_ASPECTPOSITION : 0;
    lstrcpyW(oi->tszName, o->name);
    oi->wUsagePage = o->page;
    oi->wUsage = o->usage;
}

/* The object a property header or GetObjectInfo names; -1 for none */
static int find_obj(Dev *d, DWORD obj, DWORD how)
{
    for (int i = 0; i < d->nobj; i++) {
        const Obj *o = &d->objs[i];
        if (how == DIPH_BYOFFSET && d->have_fmt && d->ofs[i] >= 0 && (DWORD)d->ofs[i] == obj) return i;
        if (how == DIPH_BYID && (o->type & 0x00FFFFFF) == (obj & 0x00FFFFFF)) return i;
        if (how == DIPH_BYUSAGE && o->page == HIWORD(obj) && o->usage == LOWORD(obj) && o->usage) return i;
    }
    return -1;
}

static void stop_watch(Dev *d)
{
    if (!d->thread) return;
    d->stop = 1;
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
    d->thread = NULL;
    d->stop = 0;
}

/* ---- IDirectInputDevice8W ---- */

#define DW(p) CONTAINER(p, Dev, iw)

static HRESULT STDMETHODCALLTYPE dw_qi(IDirectInputDevice8W *This, REFIID riid, void **ppv)
{
    Dev *d = DW(This);
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectInputDevice8W)) *ppv = &d->iw;
    else if (IsEqualIID(riid, &IID_IDirectInputDevice8A)) *ppv = &d->ia;
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&d->ref);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE dw_addref(IDirectInputDevice8W *This) { return (ULONG)InterlockedIncrement(&DW(This)->ref); }

static ULONG STDMETHODCALLTYPE dw_release(IDirectInputDevice8W *This)
{
    Dev *d = DW(This);
    LONG r = InterlockedDecrement(&d->ref);
    if (r) return (ULONG)r;
    stop_watch(d);
    DeleteCriticalSection(&d->cs);
    HeapFree(GetProcessHeap(), 0, d->buf);
    HeapFree(GetProcessHeap(), 0, d->map);
    HeapFree(GetProcessHeap(), 0, d->pov_unmapped);
    HeapFree(GetProcessHeap(), 0, d);
    return 0;
}

static HRESULT STDMETHODCALLTYPE dw_caps(IDirectInputDevice8W *This, LPDIDEVCAPS caps)
{
    Dev *d = DW(This);
    if (!caps) return E_POINTER;
    if (caps->dwSize != sizeof(DIDEVCAPS) && caps->dwSize != 24 /* DIDEVCAPS_DX3 */) return DIERR_INVALIDPARAM;
    DIDEVCAPS c;
    ZeroMemory(&c, sizeof(c));
    c.dwSize = caps->dwSize;
    NovaPadInfo i;
    BOOL here = d->kind != DEV_PAD || (nova_pad_info(d->slot, &i) && i.serial == d->serial);
    c.dwFlags = (here ? DIDC_ATTACHED : 0) | (d->kind == DEV_PAD ? DIDC_POLLEDDATAFORMAT : 0);
    c.dwDevType = dev_type(d->kind, &d->info);
    for (int k = 0; k < d->nobj; k++) {
        if (d->objs[k].type & DIDFT_AXIS) c.dwAxes++;
        else if (d->objs[k].type & DIDFT_POV) c.dwPOVs++;
        else c.dwButtons++;
    }
    memcpy(caps, &c, caps->dwSize);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE dw_enumobjects(IDirectInputDevice8W *This, LPDIENUMDEVICEOBJECTSCALLBACKW cb, LPVOID ref, DWORD flags)
{
    Dev *d = DW(This);
    if (!cb) return DIERR_INVALIDPARAM;
    DWORD want = flags & 0xFF;
    if (flags & (DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER | DIDFT_OUTPUT)) return DI_OK;   /* (none of those) */
    for (int i = 0; i < d->nobj; i++) {
        if (want && !(want & d->objs[i].type & 0xFF)) continue;
        DIDEVICEOBJECTINSTANCEW oi;
        obj_instance(d, i, &oi);
        if (cb(&oi, ref) == DIENUM_STOP) break;
    }
    return DI_OK;
}

/* The controller's HID path (lower case, as DirectInput gives it), the one
 * Raw Input and setupapi list and CreateFile opens */
static void path_of(Dev *d, WCHAR *out)
{
    nova_pad_path(&d->info, out, TRUE);
}

static HRESULT STDMETHODCALLTYPE dw_getprop(IDirectInputDevice8W *This, REFGUID prop, LPDIPROPHEADER ph)
{
    Dev *d = DW(This);
    if (!ph || ph->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
    ULONG_PTR id = (ULONG_PTR)prop;
    if (id > 0xFFFF) return DIERR_UNSUPPORTED;
    int o = -1;
    if (ph->dwHow != DIPH_DEVICE) {
        o = find_obj(d, ph->dwObj, ph->dwHow);
        if (o < 0) return DIERR_OBJECTNOTFOUND;
    }
    DIPROPDWORD *pd = (DIPROPDWORD *)ph;
    DIPROPRANGE *pr = (DIPROPRANGE *)ph;
    DIPROPSTRING *ps = (DIPROPSTRING *)ph;
    int ax = o;
    if (ax < 0) for (int i = 0; i < d->nobj && ax < 0; i++) if (d->objs[i].type & DIDFT_AXIS) ax = i;
    switch (id) {
    case 1:                                                          /* BUFFERSIZE */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = d->bufsize;
        return DI_OK;
    case 2:                                                          /* AXISMODE */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = d->relative ? DIPROPAXISMODE_REL : DIPROPAXISMODE_ABS;
        return DI_OK;
    case 3:                                                          /* GRANULARITY */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = d->kind == DEV_MOUSE && o >= 0 && d->objs[o].src == 2 ? WHEEL_DELTA : 1;
        return DI_OK;
    case 4: case 18: case 19:                                        /* RANGE, PHYSICALRANGE, LOGICALRANGE */
        if (ph->dwSize != sizeof(DIPROPRANGE)) return DIERR_INVALIDPARAM;
        if (ax < 0 || !(d->objs[ax].type & DIDFT_ABSAXIS)) return DIERR_UNSUPPORTED;
        pr->lMin = id == 4 ? d->lmin[ax] : 0;
        pr->lMax = id == 4 ? d->lmax[ax] : 65535;
        return DI_OK;
    case 5: case 6:                                                  /* DEADZONE, SATURATION */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        if (ax < 0 || !(d->objs[ax].type & DIDFT_ABSAXIS)) return DIERR_UNSUPPORTED;
        pd->dwData = id == 5 ? d->dz[ax] : d->sat[ax];
        return DI_OK;
    case 9:                                                          /* AUTOCENTER */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = d->autocenter;
        return DI_OK;
    case 7:                                                          /* FFGAIN */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = 10000;
        return DI_OK;
    case 10:                                                         /* CALIBRATIONMODE */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = 0;
        return DI_OK;
    case 12: {                                                       /* GUIDANDPATH */
        if (ph->dwSize != sizeof(DIPROPGUIDANDPATH)) return DIERR_INVALIDPARAM;
        if (d->kind != DEV_PAD) return DIERR_UNSUPPORTED;
        DIPROPGUIDANDPATH *gp = (DIPROPGUIDANDPATH *)ph;
        gp->guidClass = GUID_HIDClass;
        path_of(d, gp->wszPath);
        return DI_OK;
    }
    case 13: case 14:                                                /* INSTANCENAME, PRODUCTNAME */
        if (ph->dwSize != sizeof(DIPROPSTRING)) return DIERR_INVALIDPARAM;
        if (d->kind == DEV_PAD) MultiByteToWideChar(CP_ACP, 0, d->info.name, -1, ps->wsz, MAX_PATH);
        else lstrcpyW(ps->wsz, d->kind == DEV_KBD ? L"Keyboard" : L"Mouse");
        return DI_OK;
    case 15:                                                         /* JOYSTICKID */
        if (ph->dwSize != sizeof(DIPROPDWORD) || d->kind != DEV_PAD) return DIERR_INVALIDPARAM;
        pd->dwData = (DWORD)d->slot;
        return DI_OK;
    case 20:                                                         /* KEYNAME */
        if (ph->dwSize != sizeof(DIPROPSTRING) || d->kind != DEV_KBD || o < 0) return DIERR_INVALIDPARAM;
        lstrcpyW(ps->wsz, d->objs[o].name);
        return DI_OK;
    case 22:                                                         /* APPDATA */
        if (ph->dwSize != sizeof(DIPROPPOINTER) || o < 0) return DIERR_INVALIDPARAM;
        ((DIPROPPOINTER *)ph)->uData = d->appdata[o];
        return DI_OK;
    case 23:                                                         /* SCANCODE */
        if (ph->dwSize != sizeof(DIPROPDWORD) || d->kind != DEV_KBD || o < 0) return DIERR_INVALIDPARAM;
        pd->dwData = (DWORD)d->objs[o].src;
        return DI_OK;
    case 24:                                                         /* VIDPID */
        if (ph->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        if (d->kind != DEV_PAD) return DIERR_UNSUPPORTED;
        pd->dwData = (DWORD)MAKELONG(d->info.vid, d->info.pid);
        return DI_OK;
    case 26:                                                         /* TYPENAME */
        if (ph->dwSize != sizeof(DIPROPSTRING) || d->kind != DEV_PAD) return DIERR_UNSUPPORTED;
        wsprintfW(ps->wsz, L"VID_%04X&PID_%04X", d->info.vid, d->info.pid);
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE dw_setprop(IDirectInputDevice8W *This, REFGUID prop, LPCDIPROPHEADER ph)
{
    Dev *d = DW(This);
    if (!ph || ph->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
    ULONG_PTR id = (ULONG_PTR)prop;
    if (id > 0xFFFF) return DIERR_UNSUPPORTED;
    int o = -1;
    if (ph->dwHow != DIPH_DEVICE) {
        o = find_obj(d, ph->dwObj, ph->dwHow);
        if (o < 0) return DIERR_OBJECTNOTFOUND;
    } else if (ph->dwObj) {
        return DIERR_INVALIDPARAM;
    }
    const DIPROPDWORD *pd = (const DIPROPDWORD *)ph;
    const DIPROPRANGE *pr = (const DIPROPRANGE *)ph;
    HRESULT hr = DI_OK;
    EnterCriticalSection(&d->cs);
    switch (id) {
    case 1:                                                          /* BUFFERSIZE */
        if (ph->dwSize != sizeof(DIPROPDWORD) || o >= 0) { hr = DIERR_INVALIDPARAM; break; }
        if (d->acquired) { hr = DIERR_ACQUIRED; break; }
        HeapFree(GetProcessHeap(), 0, d->buf);
        d->buf = NULL;
        d->bufsize = d->nbuf = d->head = 0;
        if (pd->dwData) {
            DWORD n = pd->dwData > 65536 ? 65536 : pd->dwData;
            d->buf = HeapAlloc(GetProcessHeap(), 0, n * sizeof(DIDEVICEOBJECTDATA));
            if (!d->buf) { hr = DIERR_OUTOFMEMORY; break; }
            d->bufsize = n;
        }
        break;
    case 2:                                                          /* AXISMODE */
        if (ph->dwSize != sizeof(DIPROPDWORD) || o >= 0) { hr = DIERR_INVALIDPARAM; break; }
        if (d->acquired) { hr = DIERR_ACQUIRED; break; }
        if (d->kind == DEV_MOUSE) d->relative = pd->dwData == DIPROPAXISMODE_REL;
        else if (pd->dwData != DIPROPAXISMODE_ABS) hr = DI_PROPNOEFFECT;
        break;
    case 4: case 5: case 6:                                          /* RANGE, DEADZONE, SATURATION */
        if (ph->dwSize != (id == 4 ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD))) { hr = DIERR_INVALIDPARAM; break; }
        if (id == 4 && pr->lMin >= pr->lMax) { hr = DIERR_INVALIDPARAM; break; }
        if (id != 4 && pd->dwData > 10000) { hr = DIERR_INVALIDPARAM; break; }
        if (o >= 0 && !(d->objs[o].type & DIDFT_ABSAXIS)) { hr = DIERR_UNSUPPORTED; break; }
        for (int i = 0; i < d->nobj; i++) {
            if (!(d->objs[i].type & DIDFT_ABSAXIS) || (o >= 0 && i != o)) continue;
            if (id == 4) { d->lmin[i] = pr->lMin; d->lmax[i] = pr->lMax; }
            else if (id == 5) d->dz[i] = pd->dwData;
            else d->sat[i] = pd->dwData;
        }
        if (d->acquired) baseline(d);
        break;
    case 9:                                                          /* AUTOCENTER */
        if (ph->dwSize != sizeof(DIPROPDWORD)) { hr = DIERR_INVALIDPARAM; break; }
        d->autocenter = pd->dwData;
        break;
    case 7: case 10: case 11: case 21:                               /* FFGAIN, CALIBRATIONMODE, CALIBRATION, CPOINTS */
        break;
    case 22:                                                         /* APPDATA */
        if (ph->dwSize != sizeof(DIPROPPOINTER)) { hr = DIERR_INVALIDPARAM; break; }
        for (int i = 0; i < d->nobj; i++) if (o < 0 || i == o) d->appdata[i] = ((const DIPROPPOINTER *)ph)->uData;
        break;
    case 13: case 14:                                                /* INSTANCENAME, PRODUCTNAME */
        hr = DI_PROPNOEFFECT;
        break;
    default:
        hr = DIERR_UNSUPPORTED;
    }
    LeaveCriticalSection(&d->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dw_acquire(IDirectInputDevice8W *This)
{
    Dev *d = DW(This);
    if (!d->have_fmt) return DIERR_INVALIDPARAM;
    EnterCriticalSection(&d->cs);
    HRESULT hr = DI_OK;
    if (d->acquired) hr = DI_NOEFFECT;
    else {
        NovaPadState st;
        if (d->kind == DEV_PAD && !pad_here(d, &st)) hr = DIERR_UNPLUGGED;
        else {
            d->acquired = TRUE;
            d->lost = FALSE;
            d->nbuf = d->head = 0;
            d->overflow = FALSE;
            baseline(d);
        }
    }
    LeaveCriticalSection(&d->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dw_unacquire(IDirectInputDevice8W *This)
{
    Dev *d = DW(This);
    EnterCriticalSection(&d->cs);
    HRESULT hr = d->acquired ? DI_OK : DI_NOEFFECT;
    d->acquired = FALSE;
    LeaveCriticalSection(&d->cs);
    return hr;
}

/* A controller that has gone: the device is no longer acquired */
static HRESULT lost(Dev *d)
{
    d->acquired = FALSE;
    return DIERR_INPUTLOST;
}

static HRESULT STDMETHODCALLTYPE dw_getstate(IDirectInputDevice8W *This, DWORD cb, LPVOID data)
{
    Dev *d = DW(This);
    if (!data) return E_POINTER;
    if (!d->have_fmt || cb != d->fmt_size) return DIERR_INVALIDPARAM;
    EnterCriticalSection(&d->cs);
    HRESULT hr = DI_OK;
    if (!d->acquired) hr = DIERR_NOTACQUIRED;
    else if (!refresh(d)) hr = lost(d);
    else {
        ZeroMemory(data, cb);
        for (int k = 0; k < d->npov_unmapped; k++) *(DWORD *)((BYTE *)data + d->pov_unmapped[k]) = 0xFFFFFFFF;
        for (int k = 0; k < d->nmap; k++) {
            const Map *m = &d->map[k];
            const Obj *o = &d->objs[m->obj];
            BYTE *p = (BYTE *)data + m->ofs;
            if (o->type & (DIDFT_AXIS | DIDFT_POV)) {
                DWORD v = d->val[m->obj];
                if (d->kind == DEV_MOUSE && o->src < 3 && d->relative) v = (DWORD)d->acc[o->src];
                if (m->ofs + 4 <= cb) memcpy(p, &v, 4);
            } else {
                *p = (BYTE)d->val[m->obj];
            }
        }
        d->acc[0] = d->acc[1] = d->acc[2] = 0;
    }
    LeaveCriticalSection(&d->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dw_getdata(IDirectInputDevice8W *This, DWORD cbod, LPDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags)
{
    Dev *d = DW(This);
    if (!inout) return DIERR_INVALIDPARAM;
    if (cbod != sizeof(DIDEVICEOBJECTDATA) && cbod != sizeof(DIDEVICEOBJECTDATA_DX3)) return DIERR_INVALIDPARAM;
    EnterCriticalSection(&d->cs);
    HRESULT hr = DI_OK;
    if (!d->bufsize) hr = DIERR_NOTBUFFERED;
    else if (!d->acquired) hr = DIERR_NOTACQUIRED;
    else if (!refresh(d)) hr = lost(d);
    if (hr != DI_OK) { LeaveCriticalSection(&d->cs); return hr; }
    DWORD n = *inout < d->nbuf ? *inout : d->nbuf;
    if (od)
        for (DWORD i = 0; i < n; i++)
            memcpy((BYTE *)od + (SIZE_T)i * cbod, &d->buf[(d->head + i) % d->bufsize], cbod);
    *inout = n;
    if (!(flags & DIGDD_PEEK)) {
        d->head = (d->head + n) % d->bufsize;
        d->nbuf -= n;
    }
    if (d->overflow) { hr = DI_BUFFEROVERFLOW; if (!(flags & DIGDD_PEEK)) d->overflow = FALSE; }
    LeaveCriticalSection(&d->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dw_setformat(IDirectInputDevice8W *This, LPCDIDATAFORMAT df)
{
    Dev *d = DW(This);
    if (!df) return E_POINTER;
    if (df->dwSize != sizeof(DIDATAFORMAT) || df->dwObjSize != sizeof(DIOBJECTDATAFORMAT) ||
        (df->dwNumObjs && !df->rgodf) || df->dwDataSize > 65536)
        return DIERR_INVALIDPARAM;
    EnterCriticalSection(&d->cs);
    if (d->acquired) { LeaveCriticalSection(&d->cs); return DIERR_ACQUIRED; }
    Map *map = HeapAlloc(GetProcessHeap(), 0, (df->dwNumObjs + 1) * sizeof(Map));
    DWORD *povs = HeapAlloc(GetProcessHeap(), 0, (df->dwNumObjs + 1) * sizeof(DWORD));
    if (!map || !povs) {
        HeapFree(GetProcessHeap(), 0, map);
        HeapFree(GetProcessHeap(), 0, povs);
        LeaveCriticalSection(&d->cs);
        return DIERR_OUTOFMEMORY;
    }
    BOOL taken[MAX_OBJS];
    int ofs[MAX_OBJS];
    ZeroMemory(taken, sizeof(taken));
    for (int i = 0; i < d->nobj; i++) ofs[i] = -1;
    int nmap = 0, npov = 0;
    HRESULT hr = DI_OK;
    for (DWORD k = 0; k < df->dwNumObjs; k++) {
        const DIOBJECTDATAFORMAT *f = &df->rgodf[k];
        DWORD t = DIDFT_GETTYPE(f->dwType), inst = f->dwType & DIDFT_INSTANCEMASK;
        int found = -1;
        for (int i = 0; i < d->nobj && found < 0; i++) {
            const Obj *o = &d->objs[i];
            if (taken[i]) continue;
            if (f->pguid && !IsEqualGUID(f->pguid, o->guid)) continue;
            if (t && !(t & o->type & 0xFF)) continue;
            if (inst != DIDFT_ANYINSTANCE && inst != (o->type & DIDFT_INSTANCEMASK)) continue;
            found = i;
        }
        DWORD size = (t & (DIDFT_AXIS | DIDFT_POV)) || (found >= 0 && (d->objs[found].type & (DIDFT_AXIS | DIDFT_POV))) ? 4 : 1;
        if (f->dwOfs + size > df->dwDataSize) { hr = DIERR_INVALIDPARAM; break; }
        if (found < 0) {
            if (!(f->dwType & DIDFT_OPTIONAL)) { hr = DIERR_INVALIDPARAM; break; }
            if (t & DIDFT_POV) povs[npov++] = f->dwOfs;
            continue;
        }
        taken[found] = TRUE;
        ofs[found] = (int)f->dwOfs;
        map[nmap].obj = found;
        map[nmap++].ofs = f->dwOfs;
    }
    if (hr != DI_OK) {
        HeapFree(GetProcessHeap(), 0, map);
        HeapFree(GetProcessHeap(), 0, povs);
        LeaveCriticalSection(&d->cs);
        return hr;
    }
    HeapFree(GetProcessHeap(), 0, d->map);
    HeapFree(GetProcessHeap(), 0, d->pov_unmapped);
    d->map = map; d->nmap = nmap;
    d->pov_unmapped = povs; d->npov_unmapped = npov;
    memcpy(d->ofs, ofs, sizeof(ofs));
    d->fmt_size = df->dwDataSize;
    d->have_fmt = TRUE;
    if (d->kind == DEV_MOUSE) d->relative = !(df->dwFlags & DIDF_ABSAXIS);
    LeaveCriticalSection(&d->cs);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE dw_setevent(IDirectInputDevice8W *This, HANDLE ev)
{
    Dev *d = DW(This);
    EnterCriticalSection(&d->cs);
    if (d->acquired) { LeaveCriticalSection(&d->cs); return DIERR_ACQUIRED; }
    d->event = ev;
    LeaveCriticalSection(&d->cs);
    if (ev && !d->thread) d->thread = CreateThread(NULL, 0, watch, d, 0, NULL);
    if (!ev) stop_watch(d);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE dw_setcoop(IDirectInputDevice8W *This, HWND hwnd, DWORD flags)
{
    (void)This; (void)hwnd;
    if (!(flags & (DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE)) || !(flags & (DISCL_FOREGROUND | DISCL_BACKGROUND)) ||
        ((flags & DISCL_EXCLUSIVE) && (flags & DISCL_NONEXCLUSIVE)) || ((flags & DISCL_FOREGROUND) && (flags & DISCL_BACKGROUND)))
        return DIERR_INVALIDPARAM;
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE dw_objinfo(IDirectInputDevice8W *This, LPDIDEVICEOBJECTINSTANCEW oi, DWORD obj, DWORD how)
{
    Dev *d = DW(This);
    if (!oi) return E_POINTER;
    if (oi->dwSize != sizeof(*oi) && oi->dwSize != __builtin_offsetof(DIDEVICEOBJECTINSTANCEW, dwFFMaxForce)) return DIERR_INVALIDPARAM;
    if (how == DIPH_DEVICE) return DIERR_INVALIDPARAM;
    int i = find_obj(d, obj, how);
    if (i < 0) return DIERR_OBJECTNOTFOUND;
    DIDEVICEOBJECTINSTANCEW full;
    obj_instance(d, i, &full);
    full.dwSize = oi->dwSize;
    memcpy(oi, &full, oi->dwSize);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE dw_devinfo(IDirectInputDevice8W *This, LPDIDEVICEINSTANCEW di)
{
    Dev *d = DW(This);
    if (!di) return E_POINTER;
    if (di->dwSize != sizeof(*di) && di->dwSize != __builtin_offsetof(DIDEVICEINSTANCEW, guidFFDriver)) return DIERR_INVALIDPARAM;
    DIDEVICEINSTANCEW full;
    device_instance(d->kind, d->slot, &d->info, &full);
    full.dwSize = di->dwSize;
    memcpy(di, &full, di->dwSize);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE dw_panel(IDirectInputDevice8W *This, HWND owner, DWORD flags) { (void)This; (void)owner; (void)flags; return DI_OK; }
static HRESULT STDMETHODCALLTYPE dw_init(IDirectInputDevice8W *This, HINSTANCE inst, DWORD version, REFGUID guid)
{
    (void)This; (void)inst; (void)guid;
    return version < 0x0800 ? DIERR_OLDDIRECTINPUTVERSION : version > 0x08FF ? DIERR_BETADIRECTINPUTVERSION : DI_OK;
}
static HRESULT STDMETHODCALLTYPE dw_createeffect(IDirectInputDevice8W *This, REFGUID g, const void *e, void **out, IUnknown *outer)
{
    (void)This; (void)g; (void)e; (void)outer;
    if (out) *out = NULL;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dw_enumeffects(IDirectInputDevice8W *This, LPDIENUMEFFECTSCALLBACK cb, LPVOID ref, DWORD type)
{
    (void)This; (void)cb; (void)ref; (void)type;
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE dw_effectinfo(IDirectInputDevice8W *This, void *info, REFGUID g) { (void)This; (void)info; (void)g; return DIERR_DEVICENOTREG; }
static HRESULT STDMETHODCALLTYPE dw_ffstate(IDirectInputDevice8W *This, LPDWORD out) { (void)This; (void)out; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE dw_ffcommand(IDirectInputDevice8W *This, DWORD flags) { (void)This; (void)flags; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE dw_enumcreated(IDirectInputDevice8W *This, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD flags)
{
    (void)This; (void)cb; (void)ref; (void)flags;
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE dw_escape(IDirectInputDevice8W *This, void *esc) { (void)This; (void)esc; return DIERR_UNSUPPORTED; }

static HRESULT STDMETHODCALLTYPE dw_poll(IDirectInputDevice8W *This)
{
    Dev *d = DW(This);
    EnterCriticalSection(&d->cs);
    HRESULT hr = !d->acquired ? DIERR_NOTACQUIRED : !refresh(d) ? lost(d) : d->kind == DEV_PAD ? DI_OK : DI_NOEFFECT;
    LeaveCriticalSection(&d->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dw_senddata(IDirectInputDevice8W *This, DWORD cbod, LPCDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags)
{
    (void)This; (void)cbod; (void)od; (void)flags;
    if (inout) *inout = 0;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dw_effectsinfile(IDirectInputDevice8W *This, LPCWSTR f, void *cb, LPVOID ref, DWORD flags)
{
    (void)This; (void)f; (void)cb; (void)ref; (void)flags;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dw_writeeffects(IDirectInputDevice8W *This, LPCWSTR f, DWORD n, void *e, DWORD flags)
{
    (void)This; (void)f; (void)n; (void)e; (void)flags;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dw_buildmap(IDirectInputDevice8W *This, void *af, LPCWSTR user, DWORD flags)
{
    (void)This; (void)af; (void)user; (void)flags;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dw_setmap(IDirectInputDevice8W *This, void *af, LPCWSTR user, DWORD flags)
{
    (void)This; (void)af; (void)user; (void)flags;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dw_imageinfo(IDirectInputDevice8W *This, void *hdr) { (void)This; (void)hdr; return DIERR_UNSUPPORTED; }

static const IDirectInputDevice8WVtbl g_dw_vtbl = {
    dw_qi, dw_addref, dw_release, dw_caps, dw_enumobjects, dw_getprop, dw_setprop, dw_acquire, dw_unacquire,
    dw_getstate, dw_getdata, dw_setformat, dw_setevent, dw_setcoop, dw_objinfo, dw_devinfo, dw_panel, dw_init,
    dw_createeffect, dw_enumeffects, dw_effectinfo, dw_ffstate, dw_ffcommand, dw_enumcreated, dw_escape, dw_poll,
    dw_senddata, dw_effectsinfile, dw_writeeffects, dw_buildmap, dw_setmap, dw_imageinfo,
};

/* ---- IDirectInputDevice8A: the W methods, with ANSI strings ---- */

#define DA(p) (&CONTAINER(p, Dev, ia)->iw)

static HRESULT STDMETHODCALLTYPE da_qi(IDirectInputDevice8A *This, REFIID riid, void **ppv) { return dw_qi(DA(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE da_addref(IDirectInputDevice8A *This) { return dw_addref(DA(This)); }
static ULONG STDMETHODCALLTYPE da_release(IDirectInputDevice8A *This) { return dw_release(DA(This)); }
static HRESULT STDMETHODCALLTYPE da_caps(IDirectInputDevice8A *This, LPDIDEVCAPS c) { return dw_caps(DA(This), c); }

static void obj_to_a(const DIDEVICEOBJECTINSTANCEW *w, DIDEVICEOBJECTINSTANCEA *a)
{
    ZeroMemory(a, sizeof(*a));
    a->dwSize = sizeof(*a);
    a->guidType = w->guidType;
    a->dwOfs = w->dwOfs;
    a->dwType = w->dwType;
    a->dwFlags = w->dwFlags;
    WideCharToMultiByte(CP_ACP, 0, w->tszName, -1, a->tszName, MAX_PATH, NULL, NULL);
    a->wUsagePage = w->wUsagePage;
    a->wUsage = w->wUsage;
}

static HRESULT STDMETHODCALLTYPE da_enumobjects(IDirectInputDevice8A *This, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags)
{
    Dev *d = CONTAINER(This, Dev, ia);
    if (!cb) return DIERR_INVALIDPARAM;
    DWORD want = flags & 0xFF;
    if (flags & (DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER | DIDFT_OUTPUT)) return DI_OK;
    for (int i = 0; i < d->nobj; i++) {
        if (want && !(want & d->objs[i].type & 0xFF)) continue;
        DIDEVICEOBJECTINSTANCEW w;
        DIDEVICEOBJECTINSTANCEA a;
        obj_instance(d, i, &w);
        obj_to_a(&w, &a);
        if (cb(&a, ref) == DIENUM_STOP) break;
    }
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE da_getprop(IDirectInputDevice8A *This, REFGUID prop, LPDIPROPHEADER ph)
{
    return dw_getprop(DA(This), prop, ph);       /* (DIPROPSTRING is Unicode in both) */
}
static HRESULT STDMETHODCALLTYPE da_setprop(IDirectInputDevice8A *This, REFGUID prop, LPCDIPROPHEADER ph) { return dw_setprop(DA(This), prop, ph); }
static HRESULT STDMETHODCALLTYPE da_acquire(IDirectInputDevice8A *This) { return dw_acquire(DA(This)); }
static HRESULT STDMETHODCALLTYPE da_unacquire(IDirectInputDevice8A *This) { return dw_unacquire(DA(This)); }
static HRESULT STDMETHODCALLTYPE da_getstate(IDirectInputDevice8A *This, DWORD cb, LPVOID data) { return dw_getstate(DA(This), cb, data); }
static HRESULT STDMETHODCALLTYPE da_getdata(IDirectInputDevice8A *This, DWORD cbod, LPDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD flags)
{
    return dw_getdata(DA(This), cbod, od, inout, flags);
}
static HRESULT STDMETHODCALLTYPE da_setformat(IDirectInputDevice8A *This, LPCDIDATAFORMAT df) { return dw_setformat(DA(This), df); }
static HRESULT STDMETHODCALLTYPE da_setevent(IDirectInputDevice8A *This, HANDLE ev) { return dw_setevent(DA(This), ev); }
static HRESULT STDMETHODCALLTYPE da_setcoop(IDirectInputDevice8A *This, HWND h, DWORD f) { return dw_setcoop(DA(This), h, f); }

static HRESULT STDMETHODCALLTYPE da_objinfo(IDirectInputDevice8A *This, LPDIDEVICEOBJECTINSTANCEA oi, DWORD obj, DWORD how)
{
    if (!oi) return E_POINTER;
    if (oi->dwSize != sizeof(*oi) && oi->dwSize != __builtin_offsetof(DIDEVICEOBJECTINSTANCEA, dwFFMaxForce)) return DIERR_INVALIDPARAM;
    DIDEVICEOBJECTINSTANCEW w;
    w.dwSize = sizeof(w);
    HRESULT hr = dw_objinfo(DA(This), &w, obj, how);
    if (FAILED(hr)) return hr;
    DIDEVICEOBJECTINSTANCEA a;
    obj_to_a(&w, &a);
    a.dwSize = oi->dwSize;
    memcpy(oi, &a, oi->dwSize);
    return DI_OK;
}

static void inst_to_a(const DIDEVICEINSTANCEW *w, DIDEVICEINSTANCEA *a)
{
    ZeroMemory(a, sizeof(*a));
    a->dwSize = sizeof(*a);
    a->guidInstance = w->guidInstance;
    a->guidProduct = w->guidProduct;
    a->dwDevType = w->dwDevType;
    WideCharToMultiByte(CP_ACP, 0, w->tszInstanceName, -1, a->tszInstanceName, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, w->tszProductName, -1, a->tszProductName, MAX_PATH, NULL, NULL);
    a->guidFFDriver = w->guidFFDriver;
    a->wUsagePage = w->wUsagePage;
    a->wUsage = w->wUsage;
}

static HRESULT STDMETHODCALLTYPE da_devinfo(IDirectInputDevice8A *This, LPDIDEVICEINSTANCEA di)
{
    if (!di) return E_POINTER;
    if (di->dwSize != sizeof(*di) && di->dwSize != __builtin_offsetof(DIDEVICEINSTANCEA, guidFFDriver)) return DIERR_INVALIDPARAM;
    Dev *d = CONTAINER(This, Dev, ia);
    DIDEVICEINSTANCEW w;
    DIDEVICEINSTANCEA a;
    device_instance(d->kind, d->slot, &d->info, &w);
    inst_to_a(&w, &a);
    a.dwSize = di->dwSize;
    memcpy(di, &a, di->dwSize);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE da_panel(IDirectInputDevice8A *This, HWND o, DWORD f) { return dw_panel(DA(This), o, f); }
static HRESULT STDMETHODCALLTYPE da_init(IDirectInputDevice8A *This, HINSTANCE i, DWORD v, REFGUID g) { return dw_init(DA(This), i, v, g); }
static HRESULT STDMETHODCALLTYPE da_createeffect(IDirectInputDevice8A *This, REFGUID g, const void *e, void **out, IUnknown *outer)
{
    return dw_createeffect(DA(This), g, e, out, outer);
}
static HRESULT STDMETHODCALLTYPE da_enumeffects(IDirectInputDevice8A *This, LPDIENUMEFFECTSCALLBACK cb, LPVOID ref, DWORD type)
{
    return dw_enumeffects(DA(This), cb, ref, type);
}
static HRESULT STDMETHODCALLTYPE da_effectinfo(IDirectInputDevice8A *This, void *info, REFGUID g) { return dw_effectinfo(DA(This), info, g); }
static HRESULT STDMETHODCALLTYPE da_ffstate(IDirectInputDevice8A *This, LPDWORD out) { return dw_ffstate(DA(This), out); }
static HRESULT STDMETHODCALLTYPE da_ffcommand(IDirectInputDevice8A *This, DWORD f) { return dw_ffcommand(DA(This), f); }
static HRESULT STDMETHODCALLTYPE da_enumcreated(IDirectInputDevice8A *This, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD f)
{
    return dw_enumcreated(DA(This), cb, ref, f);
}
static HRESULT STDMETHODCALLTYPE da_escape(IDirectInputDevice8A *This, void *e) { return dw_escape(DA(This), e); }
static HRESULT STDMETHODCALLTYPE da_poll(IDirectInputDevice8A *This) { return dw_poll(DA(This)); }
static HRESULT STDMETHODCALLTYPE da_senddata(IDirectInputDevice8A *This, DWORD cbod, LPCDIDEVICEOBJECTDATA od, LPDWORD inout, DWORD f)
{
    return dw_senddata(DA(This), cbod, od, inout, f);
}
static HRESULT STDMETHODCALLTYPE da_effectsinfile(IDirectInputDevice8A *This, LPCSTR file, void *cb, LPVOID ref, DWORD f)
{
    (void)This; (void)file; (void)cb; (void)ref; (void)f;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE da_writeeffects(IDirectInputDevice8A *This, LPCSTR file, DWORD n, void *e, DWORD f)
{
    (void)This; (void)file; (void)n; (void)e; (void)f;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE da_buildmap(IDirectInputDevice8A *This, void *af, LPCSTR user, DWORD f)
{
    (void)This; (void)af; (void)user; (void)f;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE da_setmap(IDirectInputDevice8A *This, void *af, LPCSTR user, DWORD f)
{
    (void)This; (void)af; (void)user; (void)f;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE da_imageinfo(IDirectInputDevice8A *This, void *hdr) { (void)This; (void)hdr; return DIERR_UNSUPPORTED; }

static const IDirectInputDevice8AVtbl g_da_vtbl = {
    da_qi, da_addref, da_release, da_caps, da_enumobjects, da_getprop, da_setprop, da_acquire, da_unacquire,
    da_getstate, da_getdata, da_setformat, da_setevent, da_setcoop, da_objinfo, da_devinfo, da_panel, da_init,
    da_createeffect, da_enumeffects, da_effectinfo, da_ffstate, da_ffcommand, da_enumcreated, da_escape, da_poll,
    da_senddata, da_effectsinfile, da_writeeffects, da_buildmap, da_setmap, da_imageinfo,
};

/* ---------------------------------------------------------------------------
 * IDirectInput8
 * ------------------------------------------------------------------------- */

typedef struct {
    IDirectInput8A ia;
    IDirectInput8W iw;
    LONG ref;
    BOOL init;
} DInput;

#define IW(p) CONTAINER(p, DInput, iw)

static HRESULT check_version(DWORD v)
{
    return v < 0x0800 ? DIERR_OLDDIRECTINPUTVERSION : v > 0x08FF ? DIERR_BETADIRECTINPUTVERSION : DI_OK;
}

static HRESULT STDMETHODCALLTYPE iw_qi(IDirectInput8W *This, REFIID riid, void **ppv)
{
    DInput *di = IW(This);
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectInput8W)) *ppv = &di->iw;
    else if (IsEqualIID(riid, &IID_IDirectInput8A)) *ppv = &di->ia;
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&di->ref);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE iw_addref(IDirectInput8W *This) { return (ULONG)InterlockedIncrement(&IW(This)->ref); }

static ULONG STDMETHODCALLTYPE iw_release(IDirectInput8W *This)
{
    DInput *di = IW(This);
    LONG r = InterlockedDecrement(&di->ref);
    if (!r) HeapFree(GetProcessHeap(), 0, di);
    return (ULONG)r;
}

/* Which device @guid names: DEV_KBD/DEV_MOUSE, or DEV_PAD with *@slot; -1 */
static int device_of(REFGUID guid, int *slot)
{
    if (IsEqualGUID(guid, &GUID_SysKeyboard)) return DEV_KBD;
    if (IsEqualGUID(guid, &GUID_SysMouse)) return DEV_MOUSE;
    DWORD present = nova_pad_present();
    for (int s = 0; s < NOVA_PAD_SLOTS; s++) {
        NovaPadInfo i;
        if (!(present & (1u << s)) || !nova_pad_info(s, &i)) continue;
        GUID g, p;
        instance_guid(s, &i, &g);
        product_guid(&i, &p);
        if (IsEqualGUID(guid, &g) || IsEqualGUID(guid, &p) || IsEqualGUID(guid, &GUID_Joystick)) { *slot = s; return DEV_PAD; }
    }
    return -1;
}

static HRESULT create_device(DInput *di, REFGUID guid, void **out, BOOL wide)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!guid) return E_POINTER;
    if (!di->init) return DIERR_NOTINITIALIZED;
    int slot = 0, kind = device_of(guid, &slot);
    if (kind < 0) return DIERR_DEVICENOTREG;
    Dev *d = new_dev(kind, slot);
    if (!d) return DIERR_OUTOFMEMORY;
    d->ia.lpVtbl = &g_da_vtbl;
    d->iw.lpVtbl = &g_dw_vtbl;
    *out = wide ? (void *)&d->iw : (void *)&d->ia;
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE iw_create(IDirectInput8W *This, REFGUID guid, LPDIRECTINPUTDEVICE8W *dev, IUnknown *outer)
{
    if (outer) return DIERR_NOAGGREGATION;
    return create_device(IW(This), guid, (void **)dev, TRUE);
}

static BOOL type_wanted(DWORD type, int kind, const NovaPadInfo *i)
{
    if (type == DI8DEVCLASS_ALL) return TRUE;
    if (type == DI8DEVCLASS_KEYBOARD) return kind == DEV_KBD;
    if (type == DI8DEVCLASS_POINTER) return kind == DEV_MOUSE;
    if (type == DI8DEVCLASS_GAMECTRL) return kind == DEV_PAD;
    if (type == DI8DEVCLASS_DEVICE) return FALSE;
    return (BYTE)type == (BYTE)dev_type(kind, i);
}

/* Every device of @type (DI8DEVCLASS_* or DI8DEVTYPE_*), keyboard and mouse first */
static HRESULT enum_devices(DInput *di, DWORD type, DWORD flags, BOOL (*each)(const DIDEVICEINSTANCEW *, void *), void *ctx)
{
    if (!di->init) return DIERR_NOTINITIALIZED;
    if (type > DI8DEVTYPE_1STPERSON || (type > DI8DEVCLASS_GAMECTRL && type < DI8DEVTYPE_DEVICE)) return DIERR_INVALIDPARAM;
    if (flags & DIEDFL_FORCEFEEDBACK) return DI_OK;             /* (no force feedback) */
    NovaPadInfo none;
    ZeroMemory(&none, sizeof(none));
    DIDEVICEINSTANCEW inst;
    for (int kind = DEV_KBD; kind <= DEV_MOUSE; kind++) {
        if (!type_wanted(type, kind, &none)) continue;
        device_instance(kind, 0, &none, &inst);
        if (!each(&inst, ctx)) return DI_OK;
    }
    DWORD present = nova_pad_present();
    for (int s = 0; s < NOVA_PAD_SLOTS; s++) {
        NovaPadInfo i;
        if (!(present & (1u << s)) || !nova_pad_info(s, &i) || !type_wanted(type, DEV_PAD, &i)) continue;
        device_instance(DEV_PAD, s, &i, &inst);
        if (!each(&inst, ctx)) return DI_OK;
    }
    return DI_OK;
}

typedef struct { LPDIENUMDEVICESCALLBACKW w; LPDIENUMDEVICESCALLBACKA a; LPVOID ref; } EnumCtx;

static BOOL each_w(const DIDEVICEINSTANCEW *inst, void *ctx)
{
    EnumCtx *c = ctx;
    return c->w(inst, c->ref) != DIENUM_STOP;
}

static BOOL each_a(const DIDEVICEINSTANCEW *inst, void *ctx)
{
    EnumCtx *c = ctx;
    DIDEVICEINSTANCEA a;
    inst_to_a(inst, &a);
    return c->a(&a, c->ref) != DIENUM_STOP;
}

static HRESULT STDMETHODCALLTYPE iw_enum(IDirectInput8W *This, DWORD type, LPDIENUMDEVICESCALLBACKW cb, LPVOID ref, DWORD flags)
{
    if (!cb) return DIERR_INVALIDPARAM;
    EnumCtx c = { cb, NULL, ref };
    return enum_devices(IW(This), type, flags, each_w, &c);
}

static HRESULT STDMETHODCALLTYPE iw_status(IDirectInput8W *This, REFGUID guid)
{
    if (!IW(This)->init) return DIERR_NOTINITIALIZED;
    int slot;
    return device_of(guid, &slot) >= 0 ? DI_OK : DI_NOTATTACHED;
}

static HRESULT STDMETHODCALLTYPE iw_panel(IDirectInput8W *This, HWND owner, DWORD flags) { (void)This; (void)owner; (void)flags; return DI_OK; }

static HRESULT STDMETHODCALLTYPE iw_init(IDirectInput8W *This, HINSTANCE inst, DWORD version)
{
    (void)inst;
    HRESULT hr = check_version(version);
    if (hr == DI_OK) IW(This)->init = TRUE;
    return hr;
}

static HRESULT STDMETHODCALLTYPE iw_find(IDirectInput8W *This, REFGUID cls, LPCWSTR name, LPGUID out)
{
    (void)This; (void)cls; (void)name; (void)out;
    return DIERR_DEVICENOTREG;
}

static HRESULT STDMETHODCALLTYPE iw_semantics(IDirectInput8W *This, LPCWSTR user, void *af, void *cb, LPVOID ref, DWORD flags)
{
    (void)This; (void)user; (void)af; (void)cb; (void)ref; (void)flags;
    return DIERR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE iw_configure(IDirectInput8W *This, void *cb, void *params, DWORD flags, LPVOID ref)
{
    (void)This; (void)cb; (void)params; (void)flags; (void)ref;
    return DIERR_UNSUPPORTED;
}

static const IDirectInput8WVtbl g_iw_vtbl = {
    iw_qi, iw_addref, iw_release, iw_create, iw_enum, iw_status, iw_panel, iw_init, iw_find, iw_semantics, iw_configure,
};

#define IA(p) (&CONTAINER(p, DInput, ia)->iw)

static HRESULT STDMETHODCALLTYPE ia_qi(IDirectInput8A *This, REFIID riid, void **ppv) { return iw_qi(IA(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE ia_addref(IDirectInput8A *This) { return iw_addref(IA(This)); }
static ULONG STDMETHODCALLTYPE ia_release(IDirectInput8A *This) { return iw_release(IA(This)); }
static HRESULT STDMETHODCALLTYPE ia_create(IDirectInput8A *This, REFGUID guid, LPDIRECTINPUTDEVICE8A *dev, IUnknown *outer)
{
    if (outer) return DIERR_NOAGGREGATION;
    return create_device(CONTAINER(This, DInput, ia), guid, (void **)dev, FALSE);
}
static HRESULT STDMETHODCALLTYPE ia_enum(IDirectInput8A *This, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags)
{
    if (!cb) return DIERR_INVALIDPARAM;
    EnumCtx c = { NULL, cb, ref };
    return enum_devices(CONTAINER(This, DInput, ia), type, flags, each_a, &c);
}
static HRESULT STDMETHODCALLTYPE ia_status(IDirectInput8A *This, REFGUID guid) { return iw_status(IA(This), guid); }
static HRESULT STDMETHODCALLTYPE ia_panel(IDirectInput8A *This, HWND o, DWORD f) { return iw_panel(IA(This), o, f); }
static HRESULT STDMETHODCALLTYPE ia_init(IDirectInput8A *This, HINSTANCE i, DWORD v) { return iw_init(IA(This), i, v); }
static HRESULT STDMETHODCALLTYPE ia_find(IDirectInput8A *This, REFGUID cls, LPCSTR name, LPGUID out)
{
    (void)This; (void)cls; (void)name; (void)out;
    return DIERR_DEVICENOTREG;
}
static HRESULT STDMETHODCALLTYPE ia_semantics(IDirectInput8A *This, LPCSTR user, void *af, void *cb, LPVOID ref, DWORD flags)
{
    (void)This; (void)user; (void)af; (void)cb; (void)ref; (void)flags;
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE ia_configure(IDirectInput8A *This, void *cb, void *params, DWORD flags, LPVOID ref)
{
    return iw_configure(IA(This), cb, params, flags, ref);
}

static const IDirectInput8AVtbl g_ia_vtbl = {
    ia_qi, ia_addref, ia_release, ia_create, ia_enum, ia_status, ia_panel, ia_init, ia_find, ia_semantics, ia_configure,
};

static DInput *new_dinput(void)
{
    DInput *di = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DInput));
    if (!di) return NULL;
    di->ia.lpVtbl = &g_ia_vtbl;
    di->iw.lpVtbl = &g_iw_vtbl;
    di->ref = 1;
    return di;
}

DINPUTAPI HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    (void)inst;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!riid) return DIERR_INVALIDPARAM;
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

/* ---- the class factory (CoCreateInstance of CLSID_DirectInput8; the
 * program then calls Initialize) ---- */

static HRESULT STDMETHODCALLTYPE cf_qi(IClassFactory *cf, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory) ? cf : NULL;
    return *ppv ? S_OK : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *cf) { (void)cf; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *cf) { (void)cf; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *cf, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)cf;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    DInput *di = new_dinput();
    if (!di) return E_OUTOFMEMORY;
    HRESULT hr = iw_qi(&di->iw, riid, ppv);
    iw_release(&di->iw);
    return hr;
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *cf, BOOL lock) { (void)cf; (void)lock; return S_OK; }
static const IClassFactoryVtbl g_cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static struct { const IClassFactoryVtbl *lpVtbl; } g_factory = { &g_cf_vtbl };

DINPUTAPI HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualCLSID(clsid, &CLSID_DirectInput8)) return cf_qi((IClassFactory *)&g_factory, riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

DINPUTAPI HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
DINPUTAPI HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
DINPUTAPI HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

/* The joystick data format dinput8.lib gives programs (c_dfDIJoystick) */
static DIOBJECTDATAFORMAT g_joy_objs[44];
static DIDATAFORMAT g_joy_format = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DIJOYSTATE), 44, g_joy_objs };

DINPUTAPI const DIDATAFORMAT *WINAPI GetdfDIJoystick(void)
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
    return &g_joy_format;
}
