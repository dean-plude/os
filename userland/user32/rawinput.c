/*
 * rawinput.c — Raw Input for HID devices: game controllers
 *
 * A program registers the top-level collections it wants (usage page and
 * usage: Generic Desktop 4 joysticks, 5 game pads, 8 multi-axis
 * controllers, or a whole page with RIDEV_PAGEONLY) and gets each input
 * report of a matching device as WM_INPUT: wParam RIM_INPUT while the
 * process is in the foreground, RIM_INPUTSINK otherwise (only with
 * RIDEV_INPUTSINK), lParam an HRAWINPUT that GetRawInputData reads (a
 * RAWINPUT of type RIM_TYPEHID, one report per message).  It goes to the
 * registration's hwndTarget, or without one to the foreground window's
 * focus.  RIDEV_DEVNOTIFY adds WM_INPUT_DEVICE_CHANGE (GIDC_ARRIVAL,
 * GIDC_REMOVAL, lParam the device handle) as devices come and go, and an
 * arrival for each matching device already there when registering.
 * GetRawInputBuffer takes the calling thread's WM_INPUT messages off its
 * queue into an array of RAWINPUT blocks.
 *
 * The devices are the kernel's game controllers (novapad.h): their HID
 * report descriptors (an Xbox controller has the one Windows' Xbox driver
 * gives its HID side, "IG_" in its name) and the reports they send.  A
 * thread started at the first registration waits for reports and for
 * controllers to come or go, and posts the messages.  Device names are
 * the HID paths CreateFile opens (kernel/um/um_hid.c); RIDI_PREPARSEDDATA
 * is what HidD_GetPreparsedData returns, for hid.dll's HidP_* calls.
 *
 * The keyboard and the mouse are not listed: registering for them is
 * accepted but no raw input comes from them yet.
 */
#include "u32.h"
#include <novapad.h>
#include <novahidp.h>

#define MAX_REG   32
#define HELD      256                 /* raw input handles kept for GetRawInputData */
#define DEV_BASE  0x10000             /* device handles: DEV_BASE + serial * 8 + slot */

typedef struct {
    DWORD id;                         /* HRAWINPUT value (0: free) */
    HANDLE dev;
    WPARAM code;
    DWORD len;
    BYTE data[NOVA_PAD_REPORT_MAX];
} Held;

static SRWLOCK        g_lock = SRWLOCK_INIT;
static RAWINPUTDEVICE g_reg[MAX_REG];
static int            g_nreg;
static Held           g_held[HELD];
static DWORD          g_next_id;
static LONG           g_thread;       /* the reader has started */
static DWORD          g_known[NOVA_PAD_SLOTS];   /* serials the reader has announced */

/* ---------------------------------------------------------------------------
 * Devices
 * ------------------------------------------------------------------------- */

static HANDLE dev_handle(int slot, DWORD serial) { return (HANDLE)(ULONG_PTR)(DEV_BASE + serial * 8 + (DWORD)slot); }

/* The controller a device handle names (its slot), or -1 */
static int dev_slot(HANDLE h, NovaPadInfo *info)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v < DEV_BASE + 8) return -1;
    int slot = (int)(v & 7);
    DWORD serial = (DWORD)((v - DEV_BASE) >> 3);
    NovaPadInfo i;
    if (!nova_pad_info(slot, &i) || i.serial != serial) return -1;
    if (info) *info = i;
    return slot;
}

/* A controller's top-level collection: usage page << 16 | usage */
static DWORD dev_usage(const NovaPadInfo *i)
{
    return (DWORD)(i->usage >> 8) << 16 | (i->usage & 0xFF);
}

/* ---------------------------------------------------------------------------
 * Registrations (under g_lock)
 * ------------------------------------------------------------------------- */

/* The registration that takes devices of @usage (page << 16 | usage), or NULL */
static RAWINPUTDEVICE *reg_for(DWORD usage)
{
    USHORT page = (USHORT)(usage >> 16), u = (USHORT)usage;
    RAWINPUTDEVICE *page_only = NULL;
    for (int i = 0; i < g_nreg; i++) {
        RAWINPUTDEVICE *r = &g_reg[i];
        if (r->usUsagePage != page) continue;
        if (r->usUsage == u && !(r->dwFlags & RIDEV_PAGEONLY))
            return (r->dwFlags & RIDEV_EXCLUDE) ? NULL : r;
        if ((r->dwFlags & RIDEV_PAGEONLY) && !page_only) page_only = r;
    }
    return page_only;
}

/* Where input for registration @r goes now (*@code RIM_INPUT or
 * RIM_INPUTSINK), or NULL when it is not wanted */
static HWND target_of(const RAWINPUTDEVICE *r, WPARAM *code)
{
    HWND fg = GetForegroundWindow();                /* (this process's, if it is in front) */
    if (r->hwndTarget) {
        if (!IsWindow(r->hwndTarget)) return NULL;
        if (fg) { *code = RIM_INPUT; return r->hwndTarget; }
        if (r->dwFlags & (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK)) { *code = RIM_INPUTSINK; return r->hwndTarget; }
        return NULL;
    }
    if (!fg) return NULL;
    GUITHREADINFO gi;
    memset(&gi, 0, sizeof(gi));
    gi.cbSize = sizeof(gi);
    *code = RIM_INPUT;
    if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, NULL), &gi) && gi.hwndFocus) return gi.hwndFocus;
    return fg;
}

/* ---------------------------------------------------------------------------
 * The reader
 * ------------------------------------------------------------------------- */

static void deliver(const NovaPadRaw *r)
{
    NovaPadInfo info;
    if (!nova_pad_info(r->slot, &info) || info.serial != r->serial) return;
    AcquireSRWLockExclusive(&g_lock);
    RAWINPUTDEVICE *reg = reg_for(dev_usage(&info));
    WPARAM code = 0;
    HWND to = reg ? target_of(reg, &code) : NULL;
    DWORD id = 0;
    if (to) {
        if (!++g_next_id) g_next_id = 1;
        id = g_next_id;
        Held *h = &g_held[id % HELD];
        h->id = id;
        h->dev = dev_handle(r->slot, r->serial);
        h->code = code;
        h->len = r->len;
        memcpy(h->data, r->data, r->len);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (to) PostMessageW(to, WM_INPUT, code, (LPARAM)id);
}

/* A controller came (@arrived) or went: to every registration with
 * RIDEV_DEVNOTIFY that takes it */
static void notify(DWORD usage, HANDLE dev, BOOL arrived)
{
    AcquireSRWLockShared(&g_lock);
    RAWINPUTDEVICE *reg = reg_for(usage);
    WPARAM code;
    HWND to = reg && (reg->dwFlags & RIDEV_DEVNOTIFY) ? reg->hwndTarget ? reg->hwndTarget : target_of(reg, &code) : NULL;
    ReleaseSRWLockShared(&g_lock);
    if (to) PostMessageW(to, WM_INPUT_DEVICE_CHANGE, arrived ? GIDC_ARRIVAL : GIDC_REMOVAL, (LPARAM)dev);
}

static NovaPadInfo g_known_info[NOVA_PAD_SLOTS];

/* Announce the controllers that came and went since the last look */
static void changes(void)
{
    for (int s = 0; s < NOVA_PAD_SLOTS; s++) {
        NovaPadInfo i;
        DWORD serial = nova_pad_info(s, &i) ? i.serial : 0;
        if (serial == g_known[s]) continue;
        if (g_known[s]) notify(dev_usage(&g_known_info[s]), dev_handle(s, g_known[s]), FALSE);
        g_known[s] = serial;
        if (serial) { g_known_info[s] = i; notify(dev_usage(&i), dev_handle(s, serial), TRUE); }
    }
}

static DWORD WINAPI reader(LPVOID arg)
{
    (void)arg;
    static NovaPadRead q;
    memset(&q, 0, sizeof(q));
    nova_pad_read(&q);                                 /* (max 0: where the ring is now) */
    DWORD after = q.newest, known = q.changes;
    for (;;) {
        q.after = after;
        q.known_changes = known;
        q.wait_ms = 250;
        q.max = 32;
        int n = nova_pad_read(&q);
        if (q.changes != known) { known = q.changes; changes(); }
        for (int i = 0; i < n; i++) deliver(&q.r[i]);
        after = n ? q.r[n - 1].seq : q.newest > after ? q.newest : after;
    }
    return 0;
}

static void start_reader(void)
{
    if (InterlockedCompareExchange(&g_thread, 1, 0)) return;
    for (int s = 0; s < NOVA_PAD_SLOTS; s++) {         /* what is there now counts as announced */
        NovaPadInfo i;
        g_known[s] = nova_pad_info(s, &i) ? i.serial : 0;
        if (g_known[s]) g_known_info[s] = i;
    }
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---------------------------------------------------------------------------
 * The API
 * ------------------------------------------------------------------------- */

USERAPI BOOL RegisterRawInputDevices(PCRAWINPUTDEVICE d, UINT n, UINT cb)
{
    if (!d || !n || cb != sizeof(RAWINPUTDEVICE)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (UINT i = 0; i < n; i++) {
        if ((d[i].dwFlags & RIDEV_REMOVE) && d[i].hwndTarget) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if ((d[i].dwFlags & (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK)) && !d[i].hwndTarget) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (d[i].hwndTarget && !IsWindow(d[i].hwndTarget)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    }
    BOOL hid = FALSE;
    AcquireSRWLockExclusive(&g_lock);
    for (UINT i = 0; i < n; i++) {
        int at = -1;
        for (int k = 0; k < g_nreg; k++)
            if (g_reg[k].usUsagePage == d[i].usUsagePage && g_reg[k].usUsage == d[i].usUsage) at = k;
        if (d[i].dwFlags & RIDEV_REMOVE) {
            if (at >= 0) g_reg[at] = g_reg[--g_nreg];
            continue;
        }
        if (at < 0) {
            if (g_nreg == MAX_REG) { ReleaseSRWLockExclusive(&g_lock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
            at = g_nreg++;
        }
        g_reg[at] = d[i];
        if (!(d[i].usUsagePage == 1 && (d[i].usUsage == 2 || d[i].usUsage == 6))) hid = TRUE;   /* not the mouse or keyboard */
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!hid) return TRUE;
    start_reader();
    for (UINT i = 0; i < n; i++) {                     /* the devices already there */
        if (!(d[i].dwFlags & RIDEV_DEVNOTIFY) || (d[i].dwFlags & (RIDEV_REMOVE | RIDEV_EXCLUDE))) continue;
        for (int s = 0; s < NOVA_PAD_SLOTS; s++) {
            NovaPadInfo info;
            if (!nova_pad_info(s, &info)) continue;
            DWORD u = dev_usage(&info);
            if ((USHORT)(u >> 16) != d[i].usUsagePage) continue;
            if (!(d[i].dwFlags & RIDEV_PAGEONLY) && (USHORT)u != d[i].usUsage) continue;
            notify(u, dev_handle(s, info.serial), TRUE);
        }
    }
    return TRUE;
}

USERAPI UINT GetRegisteredRawInputDevices(PRAWINPUTDEVICE d, PUINT n, UINT cb)
{
    if (!n || cb != sizeof(RAWINPUTDEVICE)) { SetLastError(ERROR_INVALID_PARAMETER); return (UINT)-1; }
    AcquireSRWLockShared(&g_lock);
    UINT have = (UINT)g_nreg;
    if (!d) { *n = have; ReleaseSRWLockShared(&g_lock); return 0; }
    if (*n < have) { *n = have; ReleaseSRWLockShared(&g_lock); SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
    memcpy(d, g_reg, have * sizeof(*d));
    ReleaseSRWLockShared(&g_lock);
    return have;
}

USERAPI UINT GetRawInputDeviceList(PRAWINPUTDEVICELIST list, PUINT n, UINT cb)
{
    if (!n || cb != sizeof(RAWINPUTDEVICELIST)) { SetLastError(ERROR_INVALID_PARAMETER); return (UINT)-1; }
    RAWINPUTDEVICELIST all[NOVA_PAD_SLOTS];
    UINT have = 0;
    for (int s = 0; s < NOVA_PAD_SLOTS; s++) {
        NovaPadInfo i;
        if (!nova_pad_info(s, &i)) continue;
        all[have].hDevice = dev_handle(s, i.serial);
        all[have].dwType = RIM_TYPEHID;
        have++;
    }
    if (!list) { *n = have; return 0; }
    if (*n < have) { *n = have; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
    memcpy(list, all, have * sizeof(*list));
    return have;
}

static UINT device_info(HANDLE dev, UINT cmd, LPVOID data, PUINT size, BOOL wide)
{
    NovaPadInfo info;
    int slot = dev_slot(dev, &info);
    if (slot < 0) { SetLastError(ERROR_INVALID_HANDLE); return (UINT)-1; }
    if (!size) { SetLastError(998 /* ERROR_NOACCESS */); return (UINT)-1; }
    switch (cmd) {
    case RIDI_DEVICENAME: {                            /* *size in characters */
        WCHAR w[96];
        UINT len = (UINT)nova_pad_path(&info, w, FALSE) + 1;
        if (!data) { *size = len; return 0; }
        if (*size < len) { *size = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
        for (UINT i = 0; i < len; i++) {
            if (wide) ((WCHAR *)data)[i] = w[i];
            else ((char *)data)[i] = (char)w[i];
        }
        return len;
    }
    case RIDI_DEVICEINFO: {
        UINT len = sizeof(RID_DEVICE_INFO);
        if (!data) { *size = len; return 0; }
        if (*size < len) { *size = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
        RID_DEVICE_INFO *ri = data;
        memset(ri, 0, len);
        ri->cbSize = len;
        ri->dwType = RIM_TYPEHID;
        ri->hid.dwVendorId = info.vid;
        ri->hid.dwProductId = info.pid;
        ri->hid.dwVersionNumber = info.kind == NOVA_PAD_HID ? 0x0100 : 0x0114;
        ri->hid.usUsagePage = (USHORT)(info.usage >> 8);
        ri->hid.usUsage = (USHORT)(info.usage & 0xFF);
        return len;
    }
    case RIDI_PREPARSEDDATA: {
        UINT len = nova_pad_preparsed(slot, NULL, 0);
        if (!len) { SetLastError(ERROR_INVALID_HANDLE); return (UINT)-1; }
        if (!data) { *size = len; return 0; }
        if (*size < len) { *size = len; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
        return nova_pad_preparsed(slot, data, *size);
    }
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return (UINT)-1;
}

USERAPI UINT GetRawInputDeviceInfoW(HANDLE dev, UINT cmd, LPVOID data, PUINT size) { return device_info(dev, cmd, data, size, TRUE); }
USERAPI UINT GetRawInputDeviceInfoA(HANDLE dev, UINT cmd, LPVOID data, PUINT size) { return device_info(dev, cmd, data, size, FALSE); }

/* The RAWINPUT of held input @h: its size (and into @out when given) */
static UINT raw_block(const Held *h, RAWINPUT *out)
{
    UINT size = (UINT)(sizeof(RAWINPUTHEADER) + 2 * sizeof(DWORD) + h->len);
    if (out) {
        out->header.dwType = RIM_TYPEHID;
        out->header.dwSize = size;
        out->header.hDevice = h->dev;
        out->header.wParam = h->code;
        out->data.hid.dwSizeHid = h->len;
        out->data.hid.dwCount = 1;
        memcpy(out->data.hid.bRawData, h->data, h->len);
    }
    return size;
}

USERAPI UINT GetRawInputData(HRAWINPUT raw, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    if (header != sizeof(RAWINPUTHEADER) || !size || (cmd != RID_INPUT && cmd != RID_HEADER)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    DWORD id = (DWORD)(ULONG_PTR)raw;
    AcquireSRWLockShared(&g_lock);
    Held h = g_held[id % HELD];
    ReleaseSRWLockShared(&g_lock);
    if (!id || h.id != id) { SetLastError(ERROR_INVALID_HANDLE); return (UINT)-1; }
    UINT need = cmd == RID_HEADER ? (UINT)sizeof(RAWINPUTHEADER) : raw_block(&h, NULL);
    if (!data) { *size = need; return 0; }
    if (*size < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
    if (cmd == RID_HEADER) {
        RAWINPUTHEADER *hd = data;
        hd->dwType = RIM_TYPEHID;
        hd->dwSize = raw_block(&h, NULL);
        hd->hDevice = h.dev;
        hd->wParam = h.code;
    } else raw_block(&h, data);
    return need;
}

/* The calling thread's WM_INPUT messages, oldest first, as many as fit */
USERAPI UINT GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header)
{
    if (header != sizeof(RAWINPUTHEADER) || !size) { SetLastError(ERROR_INVALID_PARAMETER); return (UINT)-1; }
    MSG m;
    if (!data) {
        *size = 0;
        while (PeekMessageW(&m, NULL, WM_INPUT, WM_INPUT, PM_NOREMOVE)) {
            DWORD id = (DWORD)m.lParam;
            AcquireSRWLockShared(&g_lock);
            Held h = g_held[id % HELD];
            ReleaseSRWLockShared(&g_lock);
            if (h.id == id) { *size = raw_block(&h, NULL); break; }
            PeekMessageW(&m, NULL, WM_INPUT, WM_INPUT, PM_REMOVE);   /* (stale: dropped) */
        }
        return 0;
    }
    BYTE *at = (BYTE *)data, *end = at + *size;
    UINT n = 0;
    while (PeekMessageW(&m, NULL, WM_INPUT, WM_INPUT, PM_NOREMOVE)) {
        DWORD id = (DWORD)m.lParam;
        AcquireSRWLockShared(&g_lock);
        Held h = g_held[id % HELD];
        ReleaseSRWLockShared(&g_lock);
        if (h.id == id) {
            UINT need = raw_block(&h, NULL);
            if (at + need > end) {
                if (!n) { *size = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
                break;
            }
            raw_block(&h, (RAWINPUT *)at);
            at = (BYTE *)NEXTRAWINPUTBLOCK((RAWINPUT *)at);
            n++;
        }
        PeekMessageW(&m, NULL, WM_INPUT, WM_INPUT, PM_REMOVE);
        if (at >= end) break;
    }
    return n;
}

/* Raw input nobody handled: nothing more to do with it (header size checked as Windows does) */
USERAPI LRESULT DefRawInputProc(PRAWINPUT *raw, INT n, UINT header)
{
    (void)raw; (void)n;
    return header == sizeof(RAWINPUTHEADER) ? 0 : -1;
}
