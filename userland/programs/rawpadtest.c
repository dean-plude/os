/*
 * rawpadtest.exe — game controllers through Raw Input and hid.dll
 *
 * Runs in the devices self-test boot "gamepad" after padtest, which
 * unplugged the Xbox 360 controller: an Xbox One controller and a HID
 * game pad are left, each tools/padpeer.py behind a QEMU usb-redir
 * device.  "rawpadtest" checks:
 *  - Raw Input's device list, names (A and W), RID_DEVICE_INFO and
 *    preparsed data, read with hid.dll's HidP_* calls (the Xbox
 *    controller's HID side: X Y Rx Ry Z, ten buttons and a hat; the HID
 *    game pad's own descriptor: sixteen buttons, a hat, X Y Z Rz);
 *  - RegisterRawInputDevices for game pads with RIDEV_INPUTSINK and
 *    RIDEV_DEVNOTIFY: an arrival for each controller, then WM_INPUT for
 *    each report the test makes them send (a "[PAD] step N:" line asks the
 *    harness), read with GetRawInputData and parsed with HidP_GetUsages,
 *    HidP_GetUsageValue, HidP_GetScaledUsageValue and HidP_GetData;
 *  - GetRawInputBuffer taking the queued WM_INPUT messages;
 *  - setupapi's and cfgmgr32's HID interface lists (the same paths), and
 *    a HID device handle: HidD_GetAttributes, HidD_GetProductString,
 *    HidD_GetPreparsedData, an overlapped ReadFile that finishes when the
 *    controller sends a report, HidD_GetInputReport;
 *  - HidP_InitializeReportForID, HidP_SetUsages and HidP_SetUsageValue
 *    building a report HidP_GetUsages and HidP_GetUsageValue read back;
 *  - the Xbox controller unplugged: WM_INPUT_DEVICE_CHANGE GIDC_REMOVAL,
 *    one device left, its handle's reads failing.
 * "rawpadtest still" (run 32-bit after that) checks the HID game pad
 * through Raw Input and a HID device handle as the last step left it.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <hidsdi.h>

typedef DWORD CONFIGRET;
__declspec(dllimport) CONFIGRET WINAPI CM_Get_Device_Interface_List_SizeW(PULONG len, LPGUID cls, LPCWSTR id, ULONG flags);
__declspec(dllimport) CONFIGRET WINAPI CM_Get_Device_Interface_ListW(LPGUID cls, LPCWSTR id, WCHAR *buf, ULONG len, ULONG flags);
__declspec(dllimport) HANDLE WINAPI SetupDiGetClassDevsW(const GUID *cls, LPCWSTR enumerator, HWND parent, DWORD flags);
__declspec(dllimport) BOOL WINAPI SetupDiEnumDeviceInterfaces(HANDLE set, PVOID dev, const GUID *cls, DWORD i, PVOID info);
__declspec(dllimport) BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HANDLE set, PVOID iface, PVOID detail, DWORD n, PDWORD need, PVOID dev);
__declspec(dllimport) BOOL WINAPI SetupDiGetDeviceRegistryPropertyW(HANDLE set, PVOID dev, DWORD prop, PDWORD type, PBYTE buf, DWORD n, PDWORD need);
__declspec(dllimport) BOOL WINAPI SetupDiGetDeviceInstanceIdW(HANDLE set, PVOID dev, LPWSTR id, DWORD n, PDWORD need);
__declspec(dllimport) BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE set);
typedef struct { DWORD cbSize; GUID InterfaceClassGuid; DWORD Flags; ULONG_PTR Reserved; } SP_DEVICE_INTERFACE_DATA;
typedef struct { DWORD cbSize; GUID ClassGuid; DWORD DevInst; ULONG_PTR Reserved; } SP_DEVINFO_DATA;
typedef struct { DWORD cbSize; WCHAR DevicePath[1]; } SP_DEVICE_INTERFACE_DETAIL_DATA_W;
#define DIGCF_PRESENT         0x02
#define DIGCF_DEVICEINTERFACE 0x10
#define SPDRP_CLASS           0x07
#define SPDRP_HARDWAREID      0x01

#define VID_MS   0x045E
#define PID_ONE  0x02EA
#define VID_HID  0x1209
#define PID_HID  0x0007

static int g_pass, g_fail;

static void check(int ok, const char *what, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, what);
    vsnprintf(buf, sizeof(buf), what, ap);
    va_end(ap);
    printf("%s %s\n", ok ? "ok  " : "FAIL", buf);
    fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

/* ---- the devices ---- */

typedef struct {
    HANDLE dev;
    RID_DEVICE_INFO info;
    WCHAR name[128];
    PHIDP_PREPARSED_DATA pp;
    UINT pplen;
    HIDP_CAPS caps;
} Dev;

static Dev g_dev[8];
static int g_ndev;

static Dev *dev_by(WORD pid)
{
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].info.hid.dwProductId == pid) return &g_dev[i];
    return NULL;
}

static Dev *dev_of(HANDLE h)
{
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].dev == h) return &g_dev[i];
    return NULL;
}

static int list_devices(void)
{
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0) return -1;
    RAWINPUTDEVICELIST l[10];
    UINT want = n, small = 0;
    UINT r0 = n ? GetRawInputDeviceList(l, &small, sizeof(l[0])) : 0;
    if (n && (r0 != (UINT)-1 || GetLastError() != ERROR_INSUFFICIENT_BUFFER || small != n)) return -2;
    if (n > 10) return -3;
    UINT got = GetRawInputDeviceList(l, &want, sizeof(l[0]));
    if (got != n) return -4;
    int hid = 0;
    for (UINT i = 0; i < got; i++) {
        if (l[i].dwType != RIM_TYPEHID) continue;          /* (the mouse and the keyboard: rawtest's) */
        Dev *d = &g_dev[hid++];
        memset(d, 0, sizeof(*d));
        d->dev = l[i].hDevice;
        UINT sz = sizeof(d->info);
        d->info.cbSize = sizeof(d->info);
        if (GetRawInputDeviceInfoW(d->dev, RIDI_DEVICEINFO, &d->info, &sz) != sizeof(d->info)) return -6;
        sz = 128;
        if (GetRawInputDeviceInfoW(d->dev, RIDI_DEVICENAME, d->name, &sz) == (UINT)-1) return -7;
        sz = 0;
        GetRawInputDeviceInfoW(d->dev, RIDI_PREPARSEDDATA, NULL, &sz);
        d->pp = (PHIDP_PREPARSED_DATA)malloc(sz ? sz : 1);
        d->pplen = sz;
        if (!sz || GetRawInputDeviceInfoW(d->dev, RIDI_PREPARSEDDATA, d->pp, &sz) != d->pplen) return -8;
        if (HidP_GetCaps(d->pp, &d->caps) != HIDP_STATUS_SUCCESS) return -9;
    }
    g_ndev = hid;
    return hid;
}

/* ---- the window and what it is sent ---- */

typedef struct { HANDLE dev; WPARAM code; DWORD len; BYTE data[64]; } Input;
static Input g_in[64];
static int g_nin;
static struct { WPARAM what; HANDLE dev; } g_change[16];
static int g_nchange;
static int g_bad_raw;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INPUT) {
        UINT sz = 0;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) != 0 || sz > 256) { g_bad_raw++; return 0; }
        union { RAWINPUT ri; BYTE b[256]; } u;
        UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, &u, &sz, sizeof(RAWINPUTHEADER));
        RAWINPUTHEADER hd;
        UINT hsz = sizeof(hd);
        UINT hgot = GetRawInputData((HRAWINPUT)lp, RID_HEADER, &hd, &hsz, sizeof(RAWINPUTHEADER));
        if (got != sz || u.ri.header.dwType != RIM_TYPEHID || u.ri.header.dwSize != sz || hgot != sizeof(hd) ||
            hd.hDevice != u.ri.header.hDevice || u.ri.data.hid.dwCount != 1 || u.ri.data.hid.dwSizeHid > 64 ||
            GET_RAWINPUT_CODE_WPARAM(wp) != u.ri.header.wParam) {
            g_bad_raw++;
        } else if (g_nin < 64) {
            Input *in = &g_in[g_nin++];
            in->dev = u.ri.header.hDevice;
            in->code = wp;
            in->len = u.ri.data.hid.dwSizeHid;
            memcpy(in->data, u.ri.data.hid.bRawData, in->len);
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    if (m == WM_INPUT_DEVICE_CHANGE) {
        if (g_nchange < 16) { g_change[g_nchange].what = wp; g_change[g_nchange].dev = (HANDLE)lp; g_nchange++; }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

/* Handle input until none has come for half a second (a controller may
 * send a change as several reports) */
static void settle(void)
{
    int n = -1;
    for (int t = 0; t < 100 && n != g_nin; t++) { n = g_nin; for (int k = 0; k < 25; k++) { pump(); Sleep(20); } }
}

#define PUMP_UNTIL(cond) do { for (int t_ = 0; t_ < 1500 && !(cond); t_++) { pump(); Sleep(20); } } while (0)

/* The latest input from @d whose report satisfies @want (or NULL) */
static Input *last_from(Dev *d)
{
    for (int i = g_nin - 1; i >= 0; i--) if (d && g_in[i].dev == d->dev) return &g_in[i];
    return NULL;
}

/* ---- reading reports with HidP_* ---- */

static int usages_are(Dev *d, BYTE *r, ULONG len, const USAGE *want, int nwant)
{
    USAGE u[32];
    ULONG n = 32;
    if (HidP_GetUsages(HidP_Input, 0x09, 0, u, &n, d->pp, (PCHAR)r, len) != HIDP_STATUS_SUCCESS) return 0;
    if ((int)n != nwant) return 0;
    for (int i = 0; i < nwant; i++) {
        int in = 0;
        for (ULONG k = 0; k < n; k++) in |= u[k] == want[i];
        if (!in) return 0;
    }
    return 1;
}

static ULONG value_of(Dev *d, BYTE *r, ULONG len, USAGE page, USAGE usage)
{
    ULONG v = 0xDEADBEEF;
    HidP_GetUsageValue(HidP_Input, page, 0, usage, &v, d->pp, (PCHAR)r, len);
    return v;
}

static void caps_of(Dev *d, const char *what, USHORT in_len, USHORT nbuttons, USHORT nbutton_caps, USHORT nvalue_caps)
{
    HIDP_CAPS *c = &d->caps;
    check(c->UsagePage == 1 && c->Usage == 5 && c->InputReportByteLength == in_len && c->OutputReportByteLength == 0 &&
          c->NumberInputButtonCaps == nbutton_caps && c->NumberInputValueCaps == nvalue_caps && c->NumberLinkCollectionNodes >= 1,
          "%s HidP_GetCaps: game pad, input report %u bytes, %u button caps, %u value caps, %u link collections",
          what, c->InputReportByteLength, c->NumberInputButtonCaps, c->NumberInputValueCaps, c->NumberLinkCollectionNodes);
    HIDP_BUTTON_CAPS bc[4];
    USHORT n = 4;
    LONG st = HidP_GetButtonCaps(HidP_Input, bc, &n, d->pp);
    check(st == HIDP_STATUS_SUCCESS && n == 1 && bc[0].UsagePage == 9 && bc[0].IsRange && bc[0].Range.UsageMin == 1 &&
          bc[0].Range.UsageMax == nbuttons && bc[0].Range.DataIndexMax - bc[0].Range.DataIndexMin == nbuttons - 1,
          "%s HidP_GetButtonCaps: buttons 1-%u (%u)", what, n ? bc[0].Range.UsageMax : 0, n);
    HIDP_VALUE_CAPS vc[8];
    n = 8;
    st = HidP_GetValueCaps(HidP_Input, vc, &n, d->pp);
    int hat = -1;
    for (int i = 0; i < n; i++) if (vc[i].UsagePage == 1 && !vc[i].IsRange && vc[i].NotRange.Usage == 0x39) hat = i;
    check(st == HIDP_STATUS_SUCCESS && n == nvalue_caps && hat >= 0 && vc[hat].HasNull && vc[hat].BitSize == 4 &&
          vc[hat].PhysicalMax == 315 && vc[hat].Units == 0x14,
          "%s HidP_GetValueCaps: %u values, a hat switch of 4 bits with a null state, 0-315 degrees", what, n);
    USHORT none = 4;
    check(HidP_GetSpecificButtonCaps(HidP_Output, 0, 0, 0, bc, &none, d->pp) == HIDP_STATUS_USAGE_NOT_FOUND && none == 0,
          "%s has no output buttons (HIDP_STATUS_USAGE_NOT_FOUND)", what);
    HIDP_LINK_COLLECTION_NODE ln[4];
    ULONG nl = 4;
    LONG ls = HidP_GetLinkCollectionNodes(ln, &nl, d->pp);
    check(ls == HIDP_STATUS_SUCCESS && nl == c->NumberLinkCollectionNodes &&
          ln[0].LinkUsagePage == 1 && ln[0].LinkUsage == 5 && ln[0].CollectionType == 1,
          "%s HidP_GetLinkCollectionNodes: %lu, the first the game pad application collection", what, (unsigned long)nl);
}

static HWND make_window(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"rawpadtest";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"rawpadtest", L"Raw Input test", WS_OVERLAPPEDWINDOW, 20, 20, 300, 200, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(w, SW_SHOW);
    pump();
    return w;
}

/* ---- the 32-bit run ---- */

static int still(void)
{
    int n = list_devices();
    check(n == 1, "Raw Input lists one controller left (%d)", n);
    Dev *hid = dev_by(PID_HID);
    check(hid && hid->info.dwType == RIM_TYPEHID && hid->info.hid.dwVendorId == VID_HID && hid->caps.InputReportByteLength == 8,
          "the HID game pad, its preparsed data read by HidP_GetCaps (input report %u bytes)", hid ? hid->caps.InputReportByteLength : 0);
    if (!hid) goto out;
    HANDLE f = CreateFileW(hid->name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    check(f != INVALID_HANDLE_VALUE, "CreateFile on its Raw Input name (%lu)", (unsigned long)GetLastError());
    if (f == INVALID_HANDLE_VALUE) goto out;
    HIDD_ATTRIBUTES a = { sizeof(a) };
    check(HidD_GetAttributes(f, &a) && a.VendorID == VID_HID && a.ProductID == PID_HID, "HidD_GetAttributes: %04x:%04x", a.VendorID, a.ProductID);
    BYTE r[8] = { 0 };
    USAGE want[] = { 7 };
    check(HidD_GetInputReport(f, r, sizeof(r)) && usages_are(hid, r, sizeof(r), want, 1),
          "HidD_GetInputReport: button 7 down, as the last step left it");
    CloseHandle(f);
    HWND w = make_window();
    RAWINPUTDEVICE rid = { 1, 5, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, w };
    check(RegisterRawInputDevices(&rid, 1, sizeof(rid)), "RegisterRawInputDevices");
    PUMP_UNTIL(g_nchange >= 1);
    check(g_nchange == 1 && g_change[0].what == GIDC_ARRIVAL && g_change[0].dev == hid->dev,
          "WM_INPUT_DEVICE_CHANGE: the HID game pad's arrival (%d)", g_nchange);
out:
    printf("rawpadtest still: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "still")) return still();

    /* ---- Raw Input's devices ---- */
    int n = list_devices();
    check(n == 2, "GetRawInputDeviceList: two HID devices, the size asked for first (%d)", n);
    Dev *one = dev_by(PID_ONE), *hid = dev_by(PID_HID);
    check(one && one->info.dwType == RIM_TYPEHID && one->info.hid.dwVendorId == VID_MS && one->info.hid.usUsagePage == 1 &&
          one->info.hid.usUsage == 5, "RIDI_DEVICEINFO: the Xbox One controller, 045e:02ea, a game pad");
    check(hid && hid->info.hid.dwVendorId == VID_HID && hid->info.hid.usUsagePage == 1 && hid->info.hid.usUsage == 5,
          "RIDI_DEVICEINFO: the HID game pad, 1209:0007, a game pad");
    if (!one || !hid) goto out;
    check(wcsstr(one->name, L"\\\\?\\HID#VID_045E&PID_02EA&IG_00#") == one->name && wcsstr(one->name, L"{4D1E55B2-F16F-11CF-88CB-001111000030}"),
          "RIDI_DEVICENAME: %ls", one->name);
    check(!wcsstr(hid->name, L"IG_") && wcsstr(hid->name, L"VID_1209&PID_0007#"), "RIDI_DEVICENAME: %ls", hid->name);
    {
        char a[128];
        UINT sz = 0, need = 0;
        UINT r = GetRawInputDeviceInfoA(hid->dev, RIDI_DEVICENAME, NULL, &need);
        sz = need;
        UINT got = GetRawInputDeviceInfoA(hid->dev, RIDI_DEVICENAME, a, &sz);
        check(r == 0 && need == wcslen(hid->name) + 1 && got == need && a[0] == '\\' && a[need - 2] == '}',
              "GetRawInputDeviceInfoA: the name in characters (%u)", need);
        UINT tiny = 4;
        check(GetRawInputDeviceInfoW(hid->dev, RIDI_DEVICENAME, a, &tiny) == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
              tiny == need, "RIDI_DEVICENAME with too little room: ERROR_INSUFFICIENT_BUFFER and the size");
        check(GetRawInputDeviceInfoW((HANDLE)(ULONG_PTR)0x1234, RIDI_DEVICEINFO, a, &tiny) == (UINT)-1 && GetLastError() == ERROR_INVALID_HANDLE,
              "a handle that is no device's: ERROR_INVALID_HANDLE");
    }
    caps_of(one, "Xbox One", 14, 10, 1, 6);
    caps_of(hid, "HID game pad", 8, 16, 1, 5);
    {
        HIDP_VALUE_CAPS vc;
        USHORT nv = 1;
        check(HidP_GetSpecificValueCaps(HidP_Input, 1, 0, 0x32, &vc, &nv, one->pp) == HIDP_STATUS_SUCCESS && nv == 1 &&
              vc.BitSize == 16 && vc.LogicalMax == 65535 && vc.LinkCollection != 0,
              "Xbox One HidP_GetSpecificValueCaps(Z): 16 bits, 0-65535, in a physical collection");
        check(HidP_MaxDataListLength(HidP_Input, hid->pp) == hid->caps.NumberInputDataIndices && hid->caps.NumberInputDataIndices == 21,
              "HID game pad: 21 data indices (16 buttons, 5 values)");
    }

    /* ---- registering ---- */
    HWND w = make_window();
    RAWINPUTDEVICE bad = { 1, 5, RIDEV_INPUTSINK, NULL };
    check(!RegisterRawInputDevices(&bad, 1, sizeof(bad)) && GetLastError() == ERROR_INVALID_PARAMETER,
          "RIDEV_INPUTSINK without a window: ERROR_INVALID_PARAMETER");
    RAWINPUTDEVICE rid[2] = { { 1, 5, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, w }, { 1, 4, RIDEV_INPUTSINK, w } };
    check(RegisterRawInputDevices(rid, 2, sizeof(rid[0])), "RegisterRawInputDevices: game pads and joysticks");
    {
        RAWINPUTDEVICE back[4];
        UINT nb = 0;
        UINT r0 = GetRegisteredRawInputDevices(NULL, &nb, sizeof(back[0]));
        nb = 4;
        UINT r = GetRegisteredRawInputDevices(back, &nb, sizeof(back[0]));
        check(r0 == 0 && r == 2 && back[0].usUsage == 5 && back[0].hwndTarget == w, "GetRegisteredRawInputDevices: both (%u)", r);
    }
    PUMP_UNTIL(g_nchange >= 2);
    check(g_nchange == 2 && g_change[0].what == GIDC_ARRIVAL && g_change[1].what == GIDC_ARRIVAL &&
          dev_of(g_change[0].dev) && dev_of(g_change[1].dev) && g_change[0].dev != g_change[1].dev,
          "WM_INPUT_DEVICE_CHANGE: an arrival for each controller already there (%d)", g_nchange);

    /* ---- 7. WM_INPUT from the HID game pad ---- */
    printf("[PAD] step 7: HID game pad: buttons 2 and 16, hat south, X 10, Rz 200\n");
    Input *in = NULL;
    PUMP_UNTIL((in = last_from(hid)) && in->data[2] == 0x80);
    check(in && in->len == 8 && in->data[0] == 0 && (in->code == RIM_INPUT || in->code == RIM_INPUTSINK) && !g_bad_raw,
          "WM_INPUT: an 8-byte report, its ID byte 0, GetRawInputData's sizes and header agree (%s)",
          in ? in->code == RIM_INPUT ? "RIM_INPUT" : "RIM_INPUTSINK" : "none");
    if (in) {
        USAGE want[] = { 2, 16 };
        check(usages_are(hid, in->data, in->len, want, 2), "HidP_GetUsages: buttons 2 and 16");
        ULONG x = value_of(hid, in->data, in->len, 1, 0x30), rz = value_of(hid, in->data, in->len, 1, 0x35),
              hat = value_of(hid, in->data, in->len, 1, 0x39);
        check(x == 10 && rz == 200 && hat == 4, "HidP_GetUsageValue: X %lu, Rz %lu, hat %lu", (unsigned long)x, (unsigned long)rz, (unsigned long)hat);
        LONG deg = -1, sx = 0;
        LONG st1 = HidP_GetScaledUsageValue(HidP_Input, 1, 0, 0x39, &deg, hid->pp, (PCHAR)in->data, in->len);
        LONG st2 = HidP_GetScaledUsageValue(HidP_Input, 1, 0, 0x30, &sx, hid->pp, (PCHAR)in->data, in->len);
        check(st1 == HIDP_STATUS_SUCCESS && deg == 180 && st2 == HIDP_STATUS_BAD_LOG_PHY_VALUES,
              "HidP_GetScaledUsageValue: the hat 180 degrees; X has no physical range (%08lx %08lx)", (unsigned long)st1, (unsigned long)st2);
        HIDP_DATA dl[32];
        ULONG nd = 32;
        LONG st = HidP_GetData(HidP_Input, dl, &nd, hid->pp, (PCHAR)in->data, in->len);
        int on = 0, vals = 0;
        for (ULONG i = 0; i < nd; i++) { if (dl[i].DataIndex < 16) on += dl[i].On; else vals++; }
        check(st == HIDP_STATUS_SUCCESS && nd == 7 && on == 2 && vals == 5, "HidP_GetData: two buttons on, five values (%lu)", (unsigned long)nd);
        ULONG short_len = 3;
        check(HidP_GetData(HidP_Input, dl, &short_len, hid->pp, (PCHAR)in->data, in->len) == HIDP_STATUS_BUFFER_TOO_SMALL && short_len == 7,
              "HidP_GetData with room for three: HIDP_STATUS_BUFFER_TOO_SMALL and the count");
        check(HidP_GetUsageValue(HidP_Input, 1, 0, 0x30, &x, hid->pp, (PCHAR)in->data, in->len - 1) == HIDP_STATUS_INVALID_REPORT_LENGTH,
              "a report one byte short: HIDP_STATUS_INVALID_REPORT_LENGTH");
        check(HidP_GetUsageValue(HidP_Input, 1, 0, 0x36, &x, hid->pp, (PCHAR)in->data, in->len) == HIDP_STATUS_USAGE_NOT_FOUND,
              "a usage it has not (Slider): HIDP_STATUS_USAGE_NOT_FOUND");
    }

    /* ---- 8. WM_INPUT from the Xbox One controller ---- */
    printf("[PAD] step 8: Xbox One: A, X, left stick 32767,-32768, left trigger 255\n");
    in = NULL;
    PUMP_UNTIL((in = last_from(one)) && in->data[11] == 0x05);
    check(in && in->len == 14, "WM_INPUT: the Xbox One controller's 14-byte report");
    if (in) {
        USAGE want[] = { 1, 3 };
        check(usages_are(one, in->data, in->len, want, 2), "HidP_GetUsages: buttons 1 (A) and 3 (X)");
        ULONG x = value_of(one, in->data, in->len, 1, 0x30), y = value_of(one, in->data, in->len, 1, 0x31),
              z = value_of(one, in->data, in->len, 1, 0x32), rx = value_of(one, in->data, in->len, 1, 0x33),
              hat = value_of(one, in->data, in->len, 1, 0x39);
        check(x == 65535 && y == 65535 && z == 65408 && rx == 32768 && hat == 0,
              "HidP_GetUsageValue: X %lu Y %lu (down), Z %lu (the left trigger), Rx %lu, hat %lu (centred)",
              (unsigned long)x, (unsigned long)y, (unsigned long)z, (unsigned long)rx, (unsigned long)hat);
        LONG deg = 0;
        check(HidP_GetScaledUsageValue(HidP_Input, 1, 0, 0x39, &deg, one->pp, (PCHAR)in->data, in->len) == HIDP_STATUS_NULL,
              "HidP_GetScaledUsageValue: the centred hat is its null state (HIDP_STATUS_NULL)");
    }

    /* ---- 9. GetRawInputBuffer ---- */
    settle();
    int before = g_nin;
    printf("[PAD] step 9: HID game pad: button 5\n");
    UINT need = 0, got = 0;
    for (int t = 0; t < 1500 && !need; t++) {
        GetRawInputBuffer(NULL, &need, sizeof(RAWINPUTHEADER));
        if (!need) Sleep(20);
    }
    union { RAWINPUT ri; BYTE b[1024]; } buf;
    UINT size = sizeof(buf);
    got = GetRawInputBuffer(&buf.ri, &size, sizeof(RAWINPUTHEADER));
    {
        RAWINPUT *ri = &buf.ri;
        int ok = got >= 1 && got != (UINT)-1, last_ok = 0, b1 = -1, b2 = -1;
        for (UINT i = 0; ok && i < got; i++) {
            ok = ri->header.dwType == RIM_TYPEHID && ri->header.hDevice == hid->dev && ri->data.hid.dwSizeHid == 8;
            last_ok = ri->data.hid.bRawData[0] == 0 && ri->data.hid.bRawData[1] == 0x10;
            b1 = ri->data.hid.bRawData[1];
            b2 = ri->data.hid.bRawData[2];
            ri = NEXTRAWINPUTBLOCK(ri);
        }
        pump();
        check(ok && last_ok && need >= sizeof(RAWINPUTHEADER) + 8 + 8 && g_nin == before,
              "GetRawInputBuffer: %u block(s) from the HID game pad, button 5 in the last, taken off the queue (%02x %02x)", got, b1 & 0xff, b2 & 0xff);
    }

    /* ---- HID device handles ---- */
    static const GUID hid_guid = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    GUID g;
    HidD_GetHidGuid(&g);
    check(!memcmp(&g, &hid_guid, sizeof(g)), "HidD_GetHidGuid");
    WCHAR paths[2][128];
    int npaths = 0;
    {
        HANDLE set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        SP_DEVICE_INTERFACE_DATA id = { sizeof(id) };
        for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &id) && npaths < 2; i++) {
            DWORD needd = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &id, NULL, 0, &needd, NULL);
            union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE b[512]; } det;
            det.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA dd = { sizeof(dd) };
            if (needd <= sizeof(det) && SetupDiGetDeviceInterfaceDetailW(set, &id, &det, sizeof(det), NULL, &dd)) {
                wcscpy(paths[npaths++], det.d.DevicePath);
                WCHAR cls[32] = L"", inst[128] = L"";
                SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_CLASS, NULL, (PBYTE)cls, sizeof(cls), NULL);
                SetupDiGetDeviceInstanceIdW(set, &dd, inst, 128, NULL);
                check(!wcscmp(cls, L"HIDClass") && !wcsncmp(inst, L"HID\\VID_", 8), "setupapi: %ls, class %ls, instance %ls",
                      det.d.DevicePath, cls, inst);
            }
        }
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        int match = npaths == 2;
        for (int i = 0; i < npaths; i++) match &= !_wcsicmp(paths[i], one->name) || !_wcsicmp(paths[i], hid->name);
        check(match, "setupapi lists both controllers' HID interfaces, the paths Raw Input names (%d)", npaths);
        ULONG len = 0;
        WCHAR list[512];
        CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&len, &g, NULL, 0);
        CONFIGRET cr2 = len <= 512 ? CM_Get_Device_Interface_ListW(&g, NULL, list, 512, 0) : 99;
        int both = cr == 0 && cr2 == 0 && len == wcslen(list) + 1 + wcslen(list + wcslen(list) + 1) + 2;
        check(both, "CM_Get_Device_Interface_List: the two paths (%lu characters)", (unsigned long)len);
    }
    HANDLE f = CreateFileW(hid->name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    check(f != INVALID_HANDLE_VALUE, "CreateFile on the HID game pad's path, overlapped (%lu)", (unsigned long)GetLastError());
    HANDLE fone = CreateFileW(one->name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    check(fone != INVALID_HANDLE_VALUE, "CreateFile on the Xbox One controller's path, synchronous");
    WCHAR nothere[] = L"\\\\?\\HID#VID_1209&PID_0007#8&7777&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    HANDLE f0 = CreateFileW(nothere, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    check(f0 == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "a HID path no controller has: not found (%lu)",
          (unsigned long)GetLastError());
    if (f != INVALID_HANDLE_VALUE) {
        HIDD_ATTRIBUTES a = { sizeof(a) };
        WCHAR prod[64] = L"";
        check(HidD_GetAttributes(f, &a) && a.VendorID == VID_HID && a.ProductID == PID_HID &&
              HidD_GetProductString(f, prod, sizeof(prod)) && !wcscmp(prod, L"NovaOS Test Gamepad"),
              "HidD_GetAttributes %04x:%04x, HidD_GetProductString \"%ls\"", a.VendorID, a.ProductID, prod);
        PHIDP_PREPARSED_DATA pp = NULL;
        check(HidD_GetPreparsedData(f, &pp) && pp && !memcmp(pp, hid->pp, hid->pplen) && HidD_FreePreparsedData(pp),
              "HidD_GetPreparsedData: the same as Raw Input's RIDI_PREPARSEDDATA");
        WCHAR oprod[64] = L"";
        check(fone != INVALID_HANDLE_VALUE && HidD_GetProductString(fone, oprod, sizeof(oprod)) &&
              !wcscmp(oprod, L"Controller (Xbox One For Windows)"), "the Xbox One controller's HID product string \"%ls\"", oprod);
        BYTE r[8];
        HidD_FlushQueue(f);
        OVERLAPPED ov = { 0 };
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DWORD n_read = 0;
        BOOL now = ReadFile(f, r, sizeof(r), NULL, &ov);
        DWORD err = GetLastError();
        check(!now && err == ERROR_IO_PENDING, "ReadFile on the overlapped handle is pending until a report comes (%lu)", (unsigned long)err);
        printf("[PAD] step 10: HID game pad: button 7\n");
        DWORD wr = WaitForSingleObject(ov.hEvent, 20000);
        BOOL done = GetOverlappedResult(f, &ov, &n_read, FALSE);
        USAGE want[] = { 7 };
        check(wr == WAIT_OBJECT_0 && done && n_read == 8 && usages_are(hid, r, 8, want, 1),
              "the read finished with the report: 8 bytes, button 7 (%lu bytes, %02x %02x)", (unsigned long)n_read, r[1], r[2]);
        BYTE ir[8] = { 0 };
        BOOL gi = HidD_GetInputReport(f, ir, sizeof(ir));
        check(gi && usages_are(hid, ir, 8, want, 1), "HidD_GetInputReport: button 7 (%02x %02x)", ir[1], ir[2]);
        check(!HidD_GetFeature(f, ir, sizeof(ir)) && !HidD_SetOutputReport(f, ir, sizeof(ir)),
              "no feature or output reports");
        CloseHandle(ov.hEvent);
        ULONG nb = 0;
        check(HidD_GetNumInputBuffers(f, &nb) && nb == 32, "HidD_GetNumInputBuffers: 32");
        CloseHandle(f);
    }

    /* ---- building reports ---- */
    {
        BYTE r[8];
        LONG s1 = HidP_InitializeReportForID(HidP_Input, 0, hid->pp, (PCHAR)r, sizeof(r));
        USAGE set[] = { 4, 9 };
        ULONG ns = 2;
        LONG s2 = HidP_SetUsages(HidP_Input, 9, 0, set, &ns, hid->pp, (PCHAR)r, sizeof(r));
        LONG s3 = HidP_SetUsageValue(HidP_Input, 1, 0, 0x31, 77, hid->pp, (PCHAR)r, sizeof(r));
        ULONG hat = 0, y = 0;
        HidP_GetUsageValue(HidP_Input, 1, 0, 0x39, &hat, hid->pp, (PCHAR)r, sizeof(r));
        y = value_of(hid, r, sizeof(r), 1, 0x31);
        check(s1 == HIDP_STATUS_SUCCESS && s2 == HIDP_STATUS_SUCCESS && s3 == HIDP_STATUS_SUCCESS && usages_are(hid, r, 8, set, 2) &&
              y == 77 && hat == 8, "HidP_InitializeReportForID (the hat null), HidP_SetUsages 4 and 9, HidP_SetUsageValue Y 77 read back");
        USAGE unset[] = { 4 };
        ULONG nu = 1;
        USAGE left[] = { 9 };
        check(HidP_UnsetUsages(HidP_Input, 9, 0, unset, &nu, hid->pp, (PCHAR)r, sizeof(r)) == HIDP_STATUS_SUCCESS &&
              usages_are(hid, r, 8, left, 1), "HidP_UnsetUsages 4: 9 left");
        check(HidP_InitializeReportForID(HidP_Input, 3, hid->pp, (PCHAR)r, sizeof(r)) == HIDP_STATUS_REPORT_DOES_NOT_EXIST,
              "a report ID the device has not: HIDP_STATUS_REPORT_DOES_NOT_EXIST");
        USAGE prev[4] = { 1, 2, 3, 0 }, cur[4] = { 2, 3, 4, 0 }, brk[4], mk[4];
        HidP_UsageListDifference(prev, cur, brk, mk, 4);
        check(brk[0] == 1 && brk[1] == 0 && mk[0] == 4 && mk[1] == 0, "HidP_UsageListDifference: 1 up, 4 down");
    }

    /* ---- 11. unplugged ---- */
    g_nchange = 0;
    printf("[PAD] step 11: unplug the Xbox One controller\n");
    PUMP_UNTIL(g_nchange >= 1);
    check(g_nchange == 1 && g_change[0].what == GIDC_REMOVAL && g_change[0].dev == one->dev,
          "WM_INPUT_DEVICE_CHANGE: the Xbox One controller's removal");
    RAWINPUTDEVICELIST all[10];
    UINT nall = 10;
    int left = 0;
    nall = GetRawInputDeviceList(all, &nall, sizeof(all[0]));
    for (UINT i = 0; i < nall && nall != (UINT)-1; i++) left += all[i].dwType == RIM_TYPEHID;
    UINT sz = sizeof(RID_DEVICE_INFO);
    RID_DEVICE_INFO ri = { sizeof(ri) };
    check(left == 1 && GetRawInputDeviceInfoW(one->dev, RIDI_DEVICEINFO, &ri, &sz) == (UINT)-1,
          "one device left; the unplugged one's handle names nothing (%d)", left);
    if (fone != INVALID_HANDLE_VALUE) {
        BYTE r[14];
        DWORD nr = 0;
        check(!ReadFile(fone, r, sizeof(r), &nr, NULL), "ReadFile on its HID handle fails (%lu)", (unsigned long)GetLastError());
        CloseHandle(fone);
    }
    DestroyWindow(w);
out:
    printf("rawpadtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
