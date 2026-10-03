/*
 * display.c — monitors and display modes: EnumDisplayMonitors,
 *             MonitorFromWindow/Point/Rect, GetMonitorInfo,
 *             EnumDisplayDevices, EnumDisplaySettings, ChangeDisplaySettings
 *
 * The modes are the kernel display driver's (NtNovaGuiCtl CTL_DISPLAY_MODE
 * and CTL_SET_DISPLAY; CTL_HEAD_MODE and CTL_SET_HEAD for the other
 * displays): the Bochs/QEMU VBE adapters can switch between them at run
 * time, other adapters only report the boot mode.  The monitors are the
 * desktop's (CTL_MONITOR): \\.\DISPLAY1 is the primary at (0, 0), and
 * the others sit around it on the virtual screen.  Coordinates are the
 * desktop's logical pixels for DPI-unaware programs, and the screens' own
 * pixels for DPI-aware ones on a monitor set above 96 DPI (dpi.c).
 */
#include "u32.h"

int __cdecl wsprintfA(LPSTR buf, LPCSTR fmt, ...);

/* -----------------------------------------------------------------------
 * Monitors.  HMONITOR = MON_BASE + index (the primary first).
 * ----------------------------------------------------------------------- */
#define MON_BASE 0x10001

typedef struct { int count; RECT r, work; int dpi; } MonInfo;

static BOOL mon_info(int i, MonInfo *m)
{
    int n = dpi_monitor_raw(i, &m->r, &m->work, NULL, &m->dpi);
    if (!n) {
        if (i == 0) {                                       /* (no desktop: one screen) */
            m->count = 1;
            SetRect(&m->r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
            m->work = m->r;
            m->dpi = 96;
            return TRUE;
        }
        return FALSE;
    }
    m->count = n;
    dpi_monitor_to_proc(i, &m->r, &m->work);               /* a DPI-aware process's coordinates */
    m->dpi = dpi_monitor_dpi(i);
    return TRUE;
}

int u32_monitor_count(void)
{
    MonInfo m;
    return mon_info(0, &m) ? m.count : 1;
}

void u32_virtual_screen(RECT *v)
{
    MonInfo m;
    SetRectEmpty(v);
    for (int i = 0; mon_info(i, &m); i++) UnionRect(v, v, &m.r);
}

static int mon_index(HMONITOR h)
{
    MonInfo m;
    int i = (int)((ULONG_PTR)h - MON_BASE);
    return (ULONG_PTR)h >= MON_BASE && mon_info(i, &m) ? i : -1;
}

static HMONITOR mon_handle(int i) { return i < 0 ? NULL : (HMONITOR)(ULONG_PTR)(MON_BASE + i); }

/* The monitor a rectangle overlaps most; none: by @flags (the nearest one,
 * the primary or NULL) */
static HMONITOR mon_from_rect(const RECT *r, DWORD flags)
{
    MonInfo m;
    int best = -1, nearest = 0;
    LONGLONG area = 0, dist = -1;
    for (int i = 0; mon_info(i, &m); i++) {
        RECT x;
        if (IntersectRect(&x, &m.r, r) || PtInRect(&m.r, *(const POINT *)r)) {
            LONGLONG a = (LONGLONG)(x.right - x.left) * (x.bottom - x.top);
            if (best < 0 || a > area) { best = i; area = a; }
        }
        LONGLONG dx = r->right <= m.r.left ? m.r.left - r->right + 1 : r->left >= m.r.right ? r->left - m.r.right + 1 : 0;
        LONGLONG dy = r->bottom <= m.r.top ? m.r.top - r->bottom + 1 : r->top >= m.r.bottom ? r->top - m.r.bottom + 1 : 0;
        if (dist < 0 || dx * dx + dy * dy < dist) { dist = dx * dx + dy * dy; nearest = i; }
    }
    if (best >= 0) return mon_handle(best);
    if (flags == MONITOR_DEFAULTTONEAREST) return mon_handle(nearest);
    if (flags == MONITOR_DEFAULTTOPRIMARY) return mon_handle(0);
    return NULL;
}

USERAPI HMONITOR MonitorFromPoint(POINT p, DWORD flags)
{
    RECT r = { p.x, p.y, p.x + 1, p.y + 1 };
    return mon_from_rect(&r, flags);
}

USERAPI HMONITOR MonitorFromRect(LPCRECT r, DWORD flags)
{
    if (!r) return flags == MONITOR_DEFAULTTONULL ? NULL : mon_handle(0);
    RECT n = *r;
    if (n.right <= n.left) n.right = n.left + 1;            /* an empty rectangle: its corner */
    if (n.bottom <= n.top) n.bottom = n.top + 1;
    return mon_from_rect(&n, flags);
}

USERAPI HMONITOR MonitorFromWindow(HWND h, DWORD flags)
{
    RECT r;
    if (!h || !GetWindowRect(h, &r)) return flags == MONITOR_DEFAULTTONULL ? NULL : mon_handle(0);
    if (IsIconic(h)) return mon_handle(0);                  /* minimized: the dock is on the primary */
    return MonitorFromRect(&r, flags);
}

static void device_name(int i, char *out) { wsprintfA(out, "\\\\.\\DISPLAY%d", i + 1); }

USERAPI BOOL GetMonitorInfoW(HMONITOR h, void *mi)
{
    MonInfo m;
    int i = mon_index(h);
    if (i < 0 || !mi || !mon_info(i, &m)) return FALSE;
    DWORD *p = (DWORD *)mi;                                 /* cbSize, rcMonitor, rcWork, dwFlags, [szDevice] */
    *(RECT *)(p + 1) = m.r;
    *(RECT *)(p + 5) = m.work;
    p[9] = i == 0 ? MONITORINFOF_PRIMARY : 0;
    if (p[0] >= 104) {
        char n[32];
        device_name(i, n);
        MultiByteToWideChar(CP_ACP, 0, n, -1, (WCHAR *)(p + 10), 32);
    }
    return TRUE;
}

USERAPI BOOL GetMonitorInfoA(HMONITOR h, void *mi)
{
    DWORD *p = (DWORD *)mi;
    if (!p) return FALSE;
    DWORD size = p[0];
    p[0] = 40;
    BOOL ok = GetMonitorInfoW(h, mi);
    p[0] = size;
    if (ok && size >= 72) device_name(mon_index(h), (char *)(p + 10));
    return ok;
}

/* Each monitor (that @clip, if any, meets), with the part of it inside
 * @clip; with @dc, @clip and the rectangles passed are the DC's
 * coordinates (its window's client area) */
USERAPI BOOL EnumDisplayMonitors(HDC dc, LPCRECT clip, MONITORENUMPROC fn, LPARAM lp)
{
    MonInfo m;
    POINT org = { 0, 0 };
    RECT area, *lim = NULL;
    HWND w = dc ? WindowFromDC(dc) : NULL;
    if (w) {                                                /* the DC's window, on the screen */
        ClientToScreen(w, &org);
        GetClientRect(w, &area);
        OffsetRect(&area, org.x, org.y);
        lim = &area;
    }
    if (!fn) return FALSE;
    for (int i = 0; mon_info(i, &m); i++) {
        RECT r = m.r;
        if (lim && !IntersectRect(&r, &r, lim)) continue;
        if (clip) {
            RECT c = *clip;
            OffsetRect(&c, org.x, org.y);
            if (!IntersectRect(&r, &r, &c)) continue;
        }
        OffsetRect(&r, -org.x, -org.y);
        if (!fn(mon_handle(i), dc, &r, lp)) break;
    }
    return TRUE;
}

/* "\\.\DISPLAYn" (any case) → n - 1; NULL → 0 (the primary); -1: not one */
static int head_of_name(const char *n)
{
    if (!n || !*n) return 0;
    if (_strnicmp(n, "\\\\.\\DISPLAY", 11)) return -1;
    char *end;
    long v = strtol(n + 11, &end, 10);
    MonInfo m;
    if (*end || v < 1 || !mon_info((int)v - 1, &m)) return -1;
    return (int)v - 1;
}

static int head_of_wname(LPCWSTR n)
{
    char a[64];
    if (!n || !*n) return 0;
    if (!WideCharToMultiByte(CP_ACP, 0, n, -1, a, sizeof(a), NULL, NULL)) return -1;
    return head_of_name(a);
}

/* EnumDisplayDevices: with no device, the display adapters (one per
 * monitor); with an adapter's name, the monitor on it */
static BOOL display_device(int head, BOOL monitor, DWORD i, char name[32], char what[128], DWORD *state)
{
    MonInfo m;
    if (monitor) {
        if (head < 0 || i) return FALSE;
        wsprintfA(name, "\\\\.\\DISPLAY%d\\Monitor0", head + 1);
        strcpy(what, "Generic PnP Monitor");
        *state = 0x3;                                       /* DISPLAY_DEVICE_ACTIVE | ATTACHED */
        return TRUE;
    }
    if (!mon_info((int)i, &m)) return FALSE;
    device_name((int)i, name);
    strcpy(what, "NovaOS Display Adapter");
    *state = 0x1 | (i == 0 ? 0x4 : 0);                      /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    return TRUE;
}

USERAPI BOOL EnumDisplayDevicesW(LPCWSTR dev, DWORD i, void *dd, DWORD flags)
{
    (void)flags;
    char name[32], what[128];
    DWORD state;
    BYTE *b = (BYTE *)dd;                                   /* cb, DeviceName[32], DeviceString[128], StateFlags, ... */
    if (!b || !display_device(dev ? head_of_wname(dev) : 0, dev != NULL, i, name, what, &state)) return FALSE;
    DWORD cb = *(DWORD *)b;
    if (cb < 328) return FALSE;
    memset(b + 4, 0, cb - 4);
    MultiByteToWideChar(CP_ACP, 0, name, -1, (WCHAR *)(b + 4), 32);
    MultiByteToWideChar(CP_ACP, 0, what, -1, (WCHAR *)(b + 68), 128);
    *(DWORD *)(b + 324) = state;
    return TRUE;
}

USERAPI BOOL EnumDisplayDevicesA(LPCSTR dev, DWORD i, void *dd, DWORD flags)
{
    (void)flags;
    char name[32], what[128];
    DWORD state;
    BYTE *b = (BYTE *)dd;                                   /* cb, DeviceName[32], DeviceString[128], StateFlags, ... */
    if (!b || !display_device(dev ? head_of_name(dev) : 0, dev != NULL, i, name, what, &state)) return FALSE;
    DWORD cb = *(DWORD *)b;
    if (cb < 168) return FALSE;
    memset(b + 4, 0, cb - 4);
    strcpy((char *)(b + 4), name);
    strcpy((char *)(b + 36), what);
    *(DWORD *)(b + 164) = state;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Display modes
 * ----------------------------------------------------------------------- */

#define DISPLAY_NAME "NovaOS Display"

/* Display modes come from the kernel's display driver (NtNovaGuiCtl
 * CTL_HEAD_MODE): mode i = 0, 1, ... largest first, or
 * ENUM_CURRENT_SETTINGS (-1) / ENUM_REGISTRY_SETTINGS (-2); always 32 bpp */
static BOOL display_mode(int head, DWORD mode, DWORD m[4])
{
    if (head < 0) return FALSE;
    if (head == 0) return NtNovaGuiCtl(0, CTL_DISPLAY_MODE, (ULONG_PTR)(LONG_PTR)(LONG)mode, m) != 0;
    INT32 io[4] = { head, (INT32)mode };
    if (!NtNovaGuiCtl(0, CTL_HEAD_MODE, 0, io)) return FALSE;
    memcpy(m, io, sizeof(io));
    return TRUE;
}

/* DEVMODE's display fields (@b: dmSpecVersion, @bpp: dmBitsPerPel; W and
 * A differ in the sizes of the names before them); the position is the
 * monitor's place on the virtual screen */
static void fill_devmode(BYTE *b, BYTE *bpp, int head, const DWORD m[4])
{
    MonInfo mi;
    *(WORD *)(b + 0) = 0x0401;                              /* dmSpecVersion (DM_SPECVERSION) */
    *(DWORD *)(b + 8) = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS |
                        DM_DISPLAYFREQUENCY | DM_POSITION;
    if (mon_info(head, &mi)) {                               /* dmPosition */
        *(LONG *)(b + 12) = mi.r.left;
        *(LONG *)(b + 16) = mi.r.top;
    }
    *(DWORD *)(bpp + 0) = m[2];                             /* dmBitsPerPel */
    *(DWORD *)(bpp + 4) = m[0];                             /* dmPelsWidth */
    *(DWORD *)(bpp + 8) = m[1];                             /* dmPelsHeight */
    *(DWORD *)(bpp + 16) = m[3];                            /* dmDisplayFrequency */
}

static BOOL enum_settings_w(int head, DWORD mode, void *dm)
{
    DWORD m[4];
    if (!dm || !display_mode(head, mode, m)) return FALSE;
    BYTE *b = dm;                                           /* DEVMODEW: dmSize at 68; fields after the name */
    WORD size = *(WORD *)(b + 68), extra = *(WORD *)(b + 70);
    if (size < 188) size = 188;                             /* through dmDisplayFrequency */
    memset(b, 0, (size_t)size + extra);
    *(WORD *)(b + 68) = size;
    *(WORD *)(b + 70) = extra;
    for (int i = 0; DISPLAY_NAME[i]; i++) ((WCHAR *)b)[i] = (WCHAR)DISPLAY_NAME[i];
    fill_devmode(b + 64, b + 168, head, m);
    return TRUE;
}

static BOOL enum_settings_a(int head, DWORD mode, void *dm)
{
    DWORD m[4];
    if (!dm || !display_mode(head, mode, m)) return FALSE;
    BYTE *b = dm;                                           /* DEVMODEA: dmSize at 36 */
    WORD size = *(WORD *)(b + 36), extra = *(WORD *)(b + 38);
    if (size < 124) size = 124;                             /* through dmDisplayFrequency */
    memset(b, 0, (size_t)size + extra);
    memcpy(b, DISPLAY_NAME, sizeof(DISPLAY_NAME));
    *(WORD *)(b + 36) = size;
    *(WORD *)(b + 38) = extra;
    fill_devmode(b + 32, b + 104, head, m);
    return TRUE;
}

USERAPI BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, void *dm) { return enum_settings_w(head_of_wname(dev), mode, dm); }
USERAPI BOOL EnumDisplaySettingsExW(LPCWSTR dev, DWORD mode, void *dm, DWORD flags)
{
    (void)flags;
    return enum_settings_w(head_of_wname(dev), mode, dm);
}
USERAPI BOOL EnumDisplaySettingsA(LPCSTR dev, DWORD mode, void *dm) { return enum_settings_a(head_of_name(dev), mode, dm); }
USERAPI BOOL EnumDisplaySettingsExA(LPCSTR dev, DWORD mode, void *dm, DWORD flags)
{
    (void)flags;
    return enum_settings_a(head_of_name(dev), mode, dm);
}
/* Switch the display mode (the kernel lays the desktop out again and sends
 * WM_DISPLAYCHANGE).  Fields the caller leaves out keep their current
 * values; NULL returns to the registry mode.  CDS_TEST only checks,
 * CDS_UPDATEREGISTRY makes the mode the default, CDS_FULLSCREEN lasts
 * until the program ends. */

/* @b: dmSpecVersion of the caller's DEVMODE, @t: its dmBitsPerPel */
static LONG change_display(int head, const BYTE *b, const BYTE *t, DWORD flags)
{
    DWORD cur[4];
    if (head < 0) return DISP_CHANGE_BADPARAM;
    if (!display_mode(head, (DWORD)-1, cur)) return -1;     /* DISP_CHANGE_FAILED */
    if (!b) {                                               /* back to the registry mode */
        INT32 in[7] = { head, 0, 0, (INT32)flags };
        if (head == 0) return (LONG)NtNovaGuiCtl(0, CTL_SET_DISPLAY, 0, in + 1);
        return (LONG)NtNovaGuiCtl(0, CTL_SET_HEAD, 0, in);
    }
    DWORD fields = *(const DWORD *)(b + 8), bpp = *(const DWORD *)t, w = *(const DWORD *)(t + 4),
          h = *(const DWORD *)(t + 8), hz = *(const DWORD *)(t + 16);
    if ((fields & DM_BITSPERPEL) && bpp && bpp != 32) return -2;            /* DISP_CHANGE_BADMODE */
    if ((fields & DM_DISPLAYFREQUENCY) && hz > 1 && hz != cur[3]) return -2;
    LONG x = *(const LONG *)(b + 12), y = *(const LONG *)(b + 16);
    /* the primary is always at (0, 0): a position for it is no change */
    BOOL pos = (fields & DM_POSITION) && head > 0;
    if ((fields & DM_POSITION) && head == 0 && (x || y)) return DISP_CHANGE_BADPARAM;
    INT32 in[7] = { head, (INT32)((fields & DM_PELSWIDTH) ? w : cur[0]), (INT32)((fields & DM_PELSHEIGHT) ? h : cur[1]),
                    (INT32)flags, pos, x, y };
    if (!pos && head == 0) return (LONG)NtNovaGuiCtl(0, CTL_SET_DISPLAY, 0, in + 1);
    return (LONG)NtNovaGuiCtl(0, CTL_SET_HEAD, 0, in);
}
USERAPI LONG ChangeDisplaySettingsExW(LPCWSTR d, void *dm, HWND h, DWORD f, void *p)
{
    (void)h; (void)p;
    return change_display(head_of_wname(d), dm ? (const BYTE *)dm + 64 : NULL, (const BYTE *)dm + 168, f);
}
USERAPI LONG ChangeDisplaySettingsW(void *dm, DWORD f) { return ChangeDisplaySettingsExW(NULL, dm, NULL, f, NULL); }
USERAPI LONG ChangeDisplaySettingsExA(LPCSTR d, void *dm, HWND h, DWORD f, void *p)
{
    (void)h; (void)p;
    return change_display(head_of_name(d), dm ? (const BYTE *)dm + 32 : NULL, (const BYTE *)dm + 104, f);
}
USERAPI LONG ChangeDisplaySettingsA(void *dm, DWORD f) { return ChangeDisplaySettingsExA(NULL, dm, NULL, f, NULL); }
