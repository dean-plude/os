/*
 * dpitest.exe — per-monitor DPI: the manifest's dpiAwareness, GetDpiForMonitor,
 *               GetDpiForWindow/System, WM_DPICHANGED with its suggested
 *               rectangle, and the coordinates DPI-aware and unaware
 *               programs see
 *
 *   dpitest              the test (the core self-tests' boot: one 2560x1600
 *                        monitor at scale 2); with a second monitor at 96
 *                        DPI it also moves its window there and back
 *   dpitest child MODE W H   (run by the test with __COMPAT_LAYER set):
 *                        MODE unaware or system, W x H the primary's
 *                        logical size; the exit code is the failures
 *
 * Its manifest makes it per-monitor aware (v2).  It sets the primary to
 * 192 DPI with NtNovaGuiCtl CTL_SET_DPI (what Settings would), checks its
 * window gets WM_DPICHANGED and twice the pixels while an unaware child
 * keeps seeing 96 DPI and logical pixels and a system-aware one sees 192
 * everywhere, then sets 96 again.
 *
 * At 192 DPI it also checks user32's own parts in windows of both DPIs:
 * a window made by a thread whose context is DPI-unaware (and one made by
 * this per-monitor-aware thread) gets list box items, a combo box field,
 * a vertical scroll bar, a menu bar and a dialog's DLUs and font at 96
 * (twice that at 192 DPI); the metrics and stock fonts follow the system
 * DPI the thread sees; a window procedure runs with its window's context.
 * The children do the same the other way round: an aware thread in the
 * unaware process, an unaware one in the system-aware process.  comctl32's
 * controls are measured in both windows too.
 *
 * A window with default-font controls and a dialog made at 96 DPI are
 * measured again at 192 and back at 96 (their controls rescale), and the
 * coordinate calls are checked across awareness contexts: the aware thread
 * sees an unaware window in its own pixels, an unaware context sees the
 * aware window in logical ones.
 */
#include <windows.h>
#include <winternl.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTL_SET_DPI 31

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static void checkv(long got, long want, const char *what)
{
    char b[160];
    snprintf(b, sizeof(b), "%s: %ld, expected %ld", what, got, want);
    check(got == want, b);
}

static void check_rect(RECT got, RECT want, const char *what)
{
    char b[200];
    snprintf(b, sizeof(b), "%s: (%ld, %ld)-(%ld, %ld), expected (%ld, %ld)-(%ld, %ld)", what,
             got.left, got.top, got.right, got.bottom, want.left, want.top, want.right, want.bottom);
    check(EqualRect(&got, &want), b);
}

static LONG set_dpi(int head, int dpi)
{
    INT32 in[3] = { head, dpi, 0 };
    return (LONG)NtNovaGuiCtl(0, CTL_SET_DPI, 0, in);
}

static HMONITOR primary(void)
{
    POINT o = { 0, 0 };
    return MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
}

static RECT monitor_rect(HMONITOR m)
{
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(m, &mi);
    return mi.rcMonitor;
}

static UINT monitor_dpi(HMONITOR m)
{
    UINT x = 0, y = 0;
    if (GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y) != S_OK) return 0;
    return x == y ? x : 0;
}

/* -----------------------------------------------------------------------
 * The window
 * ----------------------------------------------------------------------- */
static int  g_dpichanged;              /* WM_DPICHANGED seen */
static WPARAM g_dpi_wp;
static RECT g_dpi_rect;
static int  g_size_w, g_size_h;        /* the last WM_SIZE */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DPICHANGED) {
        const RECT *r = (const RECT *)lp;
        g_dpichanged++;
        g_dpi_wp = wp;
        g_dpi_rect = *r;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (m == WM_SIZE) { g_size_w = LOWORD(lp); g_size_h = HIWORD(lp); return 0; }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(0x20, 0x60, 0xC0));
        FillRect(dc, &r, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        char s[96];
        snprintf(s, sizeof(s), "dpitest: %u DPI, client %ld x %ld", GetDpiForWindow(h), r.right, r.bottom);
        TextOutA(dc, 16, 16, s, (int)strlen(s));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static void pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(20);
    }
}

/* Pump until WM_DPICHANGED (up to 5 s) */
static int wait_dpichanged(int before)
{
    DWORD end = GetTickCount() + 5000;
    MSG msg;
    while (g_dpichanged == before && (LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(20);
    }
    pump(200);                              /* the WM_SIZE and painting after it */
    return g_dpichanged > before;
}

static HWND make_window(int x, int y, int w, int h)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dpitest";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    HWND hw = CreateWindowExA(0, "dpitest", "dpitest", WS_OVERLAPPEDWINDOW, x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
    pump(300);
    return hw;
}

/* The window's rectangles at @k times 96 DPI: frame at (x, y) w x h; the
 * desktop's frame is a 32-pixel title bar and a 1-pixel border (k times) */
static void check_window(HWND hw, int k, int x, int y, int w, int h, const char *when)
{
    char what[96];
    RECT r, c;
    GetWindowRect(hw, &r);
    GetClientRect(hw, &c);
    snprintf(what, sizeof(what), "%s: GetWindowRect", when);
    check_rect(r, (RECT){ x, y, x + w, y + h }, what);
    snprintf(what, sizeof(what), "%s: GetClientRect", when);
    check_rect(c, (RECT){ 0, 0, w - 2 * k, h - 33 * k }, what);
    POINT p = { 0, 0 };
    ClientToScreen(hw, &p);
    snprintf(what, sizeof(what), "%s: ClientToScreen x", when);
    checkv(p.x, x + k, what);
    snprintf(what, sizeof(what), "%s: ClientToScreen y", when);
    checkv(p.y, y + 32 * k, what);
    snprintf(what, sizeof(what), "%s: GetDpiForWindow", when);
    checkv((long)GetDpiForWindow(hw), 96 * k, what);
}

/* -----------------------------------------------------------------------
 * user32's own controls, menus, scroll bars and dialogs at a window's DPI
 * ----------------------------------------------------------------------- */
typedef struct {
    int dpi;                            /* GetDpiForWindow */
    int aware;                          /* the window's context's awareness */
    int lb_item, cb_field;              /* LB_GETITEMHEIGHT, CB_GETITEMHEIGHT -1 */
    int sb_w, menu_h;                   /* the vertical scroll bar's width, the menu bar's height */
    int dlu_x, dlu_y;                   /* MapDialogRect of 100 x 100 DLUs */
    int dlg_cw, dlg_ch;                 /* a 200 x 100 DLU dialog's client area */
    int proc_aware;                     /* the awareness its window procedure ran with */
    RECT wr;                            /* GetWindowRect */
    /* the common controls (comctl32) */
    int hdr_h, tv_item, tv_indent;      /* HDM_LAYOUT's height, TVM_GETITEMHEIGHT, TVM_GETINDENT */
    int tb_padx, tb_pady;               /* TB_GETPADDING */
    int sbar_h, tab_h, lv_row;          /* a status bar's height, a tab's, a report list view row's */
} Meas;

static int g_proc_aware = -2;

static LRESULT CALLBACK ctlproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_APP) {
        g_proc_aware = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
        return 1;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static INT_PTR CALLBACK dlgproc(HWND h, UINT m, WPARAM wp, LPARAM lp) { (void)h; (void)m; (void)wp; (void)lp; return FALSE; }

/* A 200 x 100 DLU dialog in 8-point MS Shell Dlg, as resource scripts make them */
static HWND make_dialog(HWND owner)
{
    static DWORD buf[32];
    memset(buf, 0, sizeof(buf));
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = WS_POPUP | WS_CAPTION | DS_SETFONT;
    t->x = 10; t->y = 10; t->cx = 200; t->cy = 100;
    WORD *p = (WORD *)((BYTE *)buf + 18);
    *p++ = 0; *p++ = 0; *p++ = 0;                           /* no menu, the dialog class, no title */
    *p++ = 8;
    const WCHAR *f = L"MS Shell Dlg";
    while (*f) *p++ = *f++;
    *p = 0;
    return CreateDialogIndirectParamW(GetModuleHandleW(NULL), t, owner, dlgproc, 0);
}

static void register_ctl(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ctlproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"dpictl";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);                                    /* (again: fails, as it is there) */
}

/* Measure a window made at (x, y) w x h by this thread, in its context */
static void measure(Meas *o, int x, int y, int w, int h)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = GetModuleHandleW(NULL);
    register_ctl();
    HMENU menu = CreateMenu();
    AppendMenuW(menu, MF_STRING, 1, L"&File");
    AppendMenuW(menu, MF_STRING, 2, L"&Help");
    HWND hw = CreateWindowExW(0, L"dpictl", L"dpitest controls", WS_OVERLAPPEDWINDOW | WS_VSCROLL, x, y, w, h,
                              NULL, menu, wc.hInstance, NULL);
    memset(o, 0, sizeof(*o));
    if (!hw) return;
    HWND lb = CreateWindowExW(0, L"ListBox", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 10, 100, 100, hw, (HMENU)10, NULL, NULL);
    HWND cb = CreateWindowExW(0, L"ComboBox", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 120, 10, 100, 200, hw, (HMENU)11, NULL, NULL);
    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"one");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"one");
    /* the common controls, each with its default font and sizes */
    HWND hdr = CreateWindowExW(0, WC_HEADERW, NULL, WS_CHILD, 0, 0, 200, 30, hw, (HMENU)12, NULL, NULL);
    HWND tv = CreateWindowExW(0, WC_TREEVIEWW, NULL, WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS, 230, 10, 100, 100, hw, (HMENU)13, NULL, NULL);
    HWND tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | CCS_NORESIZE | CCS_NOPARENTALIGN, 0, 0, 200, 40, hw, (HMENU)14, NULL, NULL);
    HWND sbar = CreateWindowExW(0, STATUSCLASSNAMEW, L"status", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hw, (HMENU)15, NULL, NULL);
    HWND tab = CreateWindowExW(0, WC_TABCONTROLW, NULL, WS_CHILD | WS_VISIBLE, 10, 120, 200, 60, hw, (HMENU)16, NULL, NULL);
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT, 230, 120, 150, 100, hw, (HMENU)17, NULL, NULL);
    if (tab) { TCITEMW ti = { TCIF_TEXT }; ti.pszText = L"one"; SendMessageW(tab, TCM_INSERTITEMW, 0, (LPARAM)&ti); }
    if (lv) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH }; c.cx = 100; c.pszText = L"Name";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        LVITEMW li = { LVIF_TEXT }; li.pszText = L"one";
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&li);
    }
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
    pump(200);
    o->dpi = (int)GetDpiForWindow(hw);
    o->aware = GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hw));
    o->lb_item = (int)SendMessageW(lb, LB_GETITEMHEIGHT, 0, 0);
    o->cb_field = (int)SendMessageW(cb, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
    RECT wr, cr;
    GetWindowRect(hw, &wr);
    GetClientRect(hw, &cr);
    o->wr = wr;
    int k = o->dpi / 96;
    o->sb_w = (wr.right - wr.left - 2 * k) - cr.right;           /* the frame: a k-pixel border */
    o->menu_h = (wr.bottom - wr.top - 33 * k) - cr.bottom;        /* ... and a 32k-pixel title bar */
    HWND dlg = make_dialog(hw);
    if (dlg) {
        RECT d = { 0, 0, 100, 100 };
        MapDialogRect(dlg, &d);
        o->dlu_x = d.right; o->dlu_y = d.bottom;
        GetClientRect(dlg, &d);
        o->dlg_cw = d.right; o->dlg_ch = d.bottom;
        DestroyWindow(dlg);
    }
    if (hdr) {
        RECT r = { 0, 0, 200, 200 };
        WINDOWPOS wp;
        HDLAYOUT hl = { &r, &wp };
        memset(&wp, 0, sizeof(wp));
        if (SendMessageW(hdr, HDM_LAYOUT, 0, (LPARAM)&hl)) o->hdr_h = wp.cy;
    }
    if (tv) { o->tv_item = (int)SendMessageW(tv, TVM_GETITEMHEIGHT, 0, 0); o->tv_indent = (int)SendMessageW(tv, TVM_GETINDENT, 0, 0); }
    if (tb) { LRESULT pd = SendMessageW(tb, TB_GETPADDING, 0, 0); o->tb_padx = LOWORD(pd); o->tb_pady = HIWORD(pd); }
    if (sbar) { RECT r; GetWindowRect(sbar, &r); o->sbar_h = r.bottom - r.top; }
    if (tab) { RECT r; if (SendMessageW(tab, TCM_GETITEMRECT, 0, (LPARAM)&r)) o->tab_h = r.bottom - r.top; }
    if (lv) { RECT r = { LVIR_BOUNDS }; if (SendMessageW(lv, LVM_GETITEMRECT, 0, (LPARAM)&r)) o->lv_row = r.bottom - r.top; }
    g_proc_aware = -2;
    SendMessageW(hw, WM_APP, 0, 0);
    o->proc_aware = g_proc_aware;
    DestroyWindow(hw);
    DestroyMenu(menu);
    pump(50);
}

static void check_near(long got, long want, long tol, const char *what)
{
    char b[160];
    snprintf(b, sizeof(b), "%s: %ld, expected %ld (+-%ld)", what, got, want, tol);
    check(got >= want - tol && got <= want + tol, b);
}

/* @hi (192 DPI) against @lo (96 DPI): user32's parts twice the size */
static void check_scaled(const Meas *lo, const Meas *hi, const char *who)
{
    char what[120];
#define W_(s) (snprintf(what, sizeof(what), "%s: %s", who, s), what)
    printf("%s: 96 DPI: item %d, field %d, scroll bar %d, menu bar %d, DLUs %d x %d, dialog %d x %d\n", who,
           lo->lb_item, lo->cb_field, lo->sb_w, lo->menu_h, lo->dlu_x, lo->dlu_y, lo->dlg_cw, lo->dlg_ch);
    printf("%s: 192 DPI: item %d, field %d, scroll bar %d, menu bar %d, DLUs %d x %d, dialog %d x %d\n", who,
           hi->lb_item, hi->cb_field, hi->sb_w, hi->menu_h, hi->dlu_x, hi->dlu_y, hi->dlg_cw, hi->dlg_ch);
    checkv(lo->dpi, 96, W_("GetDpiForWindow of the 96 DPI window"));
    checkv(hi->dpi, 192, W_("GetDpiForWindow of the 192 DPI window"));
    checkv(lo->sb_w, GetSystemMetricsForDpi(SM_CXVSCROLL, 96), W_("scroll bar width at 96"));
    checkv(hi->sb_w, GetSystemMetricsForDpi(SM_CXVSCROLL, 192), W_("scroll bar width at 192"));
    /* text: the font is twice the size, which rounds its own way */
    check_near(hi->lb_item, 2 * lo->lb_item, 2, W_("list box item height at 192"));
    check_near(hi->cb_field, 2 * lo->cb_field, 2, W_("combo box field height at 192"));
    check_near(hi->menu_h, 2 * lo->menu_h, 2, W_("menu bar height at 192"));
    check(lo->lb_item >= 14 && lo->menu_h >= 18, W_("96 DPI sizes are plausible"));
    /* dialogs: 8 points is 10.67 px at 96 DPI and 21.33 at 192 */
    check_near(hi->dlu_x, 2 * lo->dlu_x, lo->dlu_x / 5, W_("MapDialogRect x at 192"));
    check_near(hi->dlu_y, 2 * lo->dlu_y, lo->dlu_y / 5, W_("MapDialogRect y at 192"));
    check_near(hi->dlg_cw, 2 * lo->dlg_cw, lo->dlg_cw / 5, W_("dialog client width at 192"));
    check_near(hi->dlg_ch, 2 * lo->dlg_ch, lo->dlg_ch / 5, W_("dialog client height at 192"));
    check(lo->dlu_x > 100 && lo->dlu_y > 100, W_("DLUs at 96 are plausible"));
    /* the common controls' fixed sizes scale with the window's DPI too */
    printf("%s: comctl32 at 96 DPI: header %d, tree item %d indent %d, toolbar padding %d x %d, status bar %d, tab %d, list row %d\n",
           who, lo->hdr_h, lo->tv_item, lo->tv_indent, lo->tb_padx, lo->tb_pady, lo->sbar_h, lo->tab_h, lo->lv_row);
    printf("%s: comctl32 at 192 DPI: header %d, tree item %d indent %d, toolbar padding %d x %d, status bar %d, tab %d, list row %d\n",
           who, hi->hdr_h, hi->tv_item, hi->tv_indent, hi->tb_padx, hi->tb_pady, hi->sbar_h, hi->tab_h, hi->lv_row);
    check(lo->hdr_h >= 20 && lo->tv_item >= 16 && lo->sbar_h >= 16 && lo->tab_h >= 16 && lo->lv_row >= 16, W_("comctl32 sizes at 96 are plausible"));
    check_near(hi->hdr_h, 2 * lo->hdr_h, 2, W_("header height at 192"));
    check_near(hi->tv_item, 2 * lo->tv_item, 2, W_("tree view item height at 192"));
    checkv(lo->tv_indent, 19, W_("tree view indent at 96"));
    checkv(hi->tv_indent, 38, W_("tree view indent at 192"));
    checkv(hi->tb_padx, 2 * lo->tb_padx, W_("toolbar padding x at 192"));
    checkv(hi->tb_pady, 2 * lo->tb_pady, W_("toolbar padding y at 192"));
    check_near(hi->sbar_h, 2 * lo->sbar_h, 2, W_("status bar height at 192"));
    check_near(hi->tab_h, 2 * lo->tab_h, 2, W_("tab height at 192"));
    check_near(hi->lv_row, 2 * lo->lv_row, 2, W_("list view row height at 192"));
#undef W_
}

/* The metrics and stock fonts at the system DPI the thread sees */
static void check_system_parts(int k, const char *who)
{
    char what[120];
#define W_(s) (snprintf(what, sizeof(what), "%s: %s", who, s), what)
    checkv((long)GetDpiForSystem(), 96 * k, W_("GetDpiForSystem"));
    checkv(GetSystemMetrics(SM_CXVSCROLL), 17 * k, W_("SM_CXVSCROLL"));
    checkv(GetSystemMetrics(SM_CYMENU), 20 * k, W_("SM_CYMENU"));
    checkv(GetSystemMetrics(SM_CYCAPTION), 31 * k, W_("SM_CYCAPTION"));
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    checkv(lf.lfHeight, -12 * k, W_("DEFAULT_GUI_FONT height"));
    NONCLIENTMETRICSW nm;
    memset(&nm, 0, sizeof(nm));
    nm.cbSize = sizeof(nm);
    check(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0), W_("SPI_GETNONCLIENTMETRICS"));
    checkv(nm.lfMessageFont.lfHeight, -12 * k, W_("NONCLIENTMETRICS message font height"));
    checkv(nm.lfCaptionFont.lfHeight, -12 * k, W_("NONCLIENTMETRICS caption font height"));
    checkv(nm.iScrollWidth, 17 * k, W_("NONCLIENTMETRICS scroll width"));
    NONCLIENTMETRICSA na;
    memset(&na, 0, sizeof(na));
    na.cbSize = sizeof(na);
    check(SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(na), &na, 0), W_("SPI_GETNONCLIENTMETRICS (A)"));
    checkv(na.lfMessageFont.lfHeight, -12 * k, W_("NONCLIENTMETRICSA message font height"));
    check(!strcmp(na.lfMessageFont.lfFaceName, "Segoe UI"), W_("NONCLIENTMETRICSA message font face"));
    /* any DPI, whatever the thread's */
    memset(&nm, 0, sizeof(nm));
    nm.cbSize = sizeof(nm);
    check(SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0, 192), W_("SystemParametersInfoForDpi"));
    checkv(nm.lfMenuFont.lfHeight, -24, W_("SystemParametersInfoForDpi menu font at 192"));
    checkv(nm.iMenuHeight, 38, W_("SystemParametersInfoForDpi menu height at 192"));
    checkv(GetSystemMetricsForDpi(SM_CXVSCROLL, 96), 17, W_("GetSystemMetricsForDpi at 96"));
    checkv(GetSystemMetricsForDpi(SM_CXVSCROLL, 192), 34, W_("GetSystemMetricsForDpi at 192"));
#undef W_
}

/* The DPI_AWARENESS_CONTEXT pseudo-handles */
static void check_contexts(void)
{
    checkv(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE), DPI_AWARENESS_UNAWARE, "UNAWARE's awareness");
    checkv(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE), DPI_AWARENESS_SYSTEM_AWARE, "SYSTEM_AWARE's awareness");
    checkv(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE), DPI_AWARENESS_PER_MONITOR_AWARE, "PER_MONITOR_AWARE's awareness");
    checkv(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), DPI_AWARENESS_PER_MONITOR_AWARE, "PER_MONITOR_AWARE_V2's awareness");
    checkv(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED), DPI_AWARENESS_UNAWARE, "UNAWARE_GDISCALED's awareness");
    checkv(GetAwarenessFromDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(LONG_PTR)-77), DPI_AWARENESS_INVALID, "an invalid context");
    check(!AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
          "per-monitor v1 and v2 differ");
    check(AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT_UNAWARE, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED),
          "UNAWARE and UNAWARE_GDISCALED are both unaware");
    check(!IsValidDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(LONG_PTR)-77), "IsValidDpiAwarenessContext");
    check(!SetThreadDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(LONG_PTR)-77) && GetLastError() == ERROR_INVALID_PARAMETER,
          "SetThreadDpiAwarenessContext refuses an invalid context");
}

/* A thread whose context is @ctx measures a window at (x, y) w x h */
typedef struct { DPI_AWARENESS_CONTEXT ctx; int x, y, w, h; Meas m; int sys_dpi, cx_screen, aware; } ThreadJob;

static DWORD WINAPI thread_measure(void *p)
{
    ThreadJob *j = p;
    SetThreadDpiAwarenessContext(j->ctx);
    j->aware = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
    j->sys_dpi = (int)GetDpiForSystem();
    j->cx_screen = GetSystemMetrics(SM_CXSCREEN);
    measure(&j->m, j->x, j->y, j->w, j->h);
    return 0;
}

static void in_thread(ThreadJob *j)
{
    HANDLE t = CreateThread(NULL, 0, thread_measure, j, 0, NULL);
    check(t != NULL, "CreateThread");
    if (!t) return;
    check(WaitForSingleObject(t, 60000) == WAIT_OBJECT_0, "the measuring thread finished");
    CloseHandle(t);
}

/* -----------------------------------------------------------------------
 * A DPI change: a window's built-in controls measure their default font
 * again, and a per-monitor v2 dialog's controls and font scale with it
 * ----------------------------------------------------------------------- */
typedef struct {
    int dpi, dlg_dpi;
    int lb_item, cb_field, cb_h, ed_line, tv_item;     /* in a window of this thread's, no WM_SETFONT */
    RECT item;                                          /* the dialog's edit control, in its client area */
    int font_h, dlu_x;                                  /* its font (LOGFONT height), MapDialogRect of 100 DLUs */
} Rescale;

static HWND g_rw, g_rdlg;

/* A 120 x 60 DLU dialog in 8-point MS Shell Dlg with one edit control at
 * (10, 10) 80 x 14 DLUs */
static HWND make_item_dialog(HWND owner)
{
    static DWORD buf[64];
    memset(buf, 0, sizeof(buf));
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = WS_POPUP | WS_CAPTION | DS_SETFONT;
    t->cdit = 1;
    t->x = 10; t->y = 10; t->cx = 120; t->cy = 60;
    WORD *p = (WORD *)((BYTE *)buf + 18);
    *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 8;
    const WCHAR *f = L"MS Shell Dlg";
    while (*f) *p++ = *f++;
    *p++ = 0;
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
    it->x = 10; it->y = 10; it->cx = 80; it->cy = 14; it->id = 100;
    p = (WORD *)((BYTE *)it + 18);
    *p++ = 0xFFFF; *p++ = 0x0081;                           /* Edit */
    *p++ = 0;                                               /* no text */
    *p++ = 0;                                               /* no creation data */
    return CreateDialogIndirectParamW(GetModuleHandleW(NULL), t, owner, dlgproc, 0);
}

static void make_rescale(HWND owner)
{
    register_ctl();
    g_rw = CreateWindowExW(0, L"dpictl", L"dpitest rescale", WS_OVERLAPPEDWINDOW, 100, 450, 400, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
    check(g_rw != NULL, "the rescale window");
    if (!g_rw) return;
    HWND lb = CreateWindowExW(0, L"ListBox", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 10, 100, 100, g_rw, (HMENU)10, NULL, NULL);
    HWND cb = CreateWindowExW(0, L"ComboBox", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 120, 10, 100, 200, g_rw, (HMENU)11, NULL, NULL);
    HWND ed = CreateWindowExW(0, L"Edit", L"a\r\nb", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE, 230, 10, 100, 100, g_rw, (HMENU)12, NULL, NULL);
    CreateWindowExW(0, WC_TREEVIEWW, NULL, WS_CHILD | WS_VISIBLE, 10, 120, 100, 100, g_rw, (HMENU)13, NULL, NULL);
    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"one");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"one");
    (void)ed;
    ShowWindow(g_rw, SW_SHOWNOACTIVATE);
    g_rdlg = make_item_dialog(owner);
    check(g_rdlg != NULL, "the rescale dialog");
    if (g_rdlg) ShowWindow(g_rdlg, SW_SHOWNOACTIVATE);
    pump(200);
}

static void measure_rescale(Rescale *o)
{
    memset(o, 0, sizeof(*o));
    if (!g_rw || !g_rdlg) return;
    o->dpi = (int)GetDpiForWindow(g_rw);
    o->dlg_dpi = (int)GetDpiForWindow(g_rdlg);
    o->lb_item = (int)SendDlgItemMessageW(g_rw, 10, LB_GETITEMHEIGHT, 0, 0);
    o->cb_field = (int)SendDlgItemMessageW(g_rw, 11, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
    RECT r;
    GetWindowRect(GetDlgItem(g_rw, 11), &r);
    o->cb_h = r.bottom - r.top;
    LRESULT a = SendDlgItemMessageW(g_rw, 12, EM_POSFROMCHAR, 0, 0), b = SendDlgItemMessageW(g_rw, 12, EM_POSFROMCHAR, 3, 0);
    o->ed_line = (short)HIWORD(b) - (short)HIWORD(a);
    o->tv_item = (int)SendDlgItemMessageW(g_rw, 13, TVM_GETITEMHEIGHT, 0, 0);
    HWND it = GetDlgItem(g_rdlg, 100);
    GetWindowRect(it, &o->item);
    MapWindowPoints(NULL, g_rdlg, (POINT *)&o->item, 2);
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    GetObjectW((HFONT)SendMessageW(it, WM_GETFONT, 0, 0), sizeof(lf), &lf);
    o->font_h = lf.lfHeight;
    RECT d = { 0, 0, 100, 100 };
    MapDialogRect(g_rdlg, &d);
    o->dlu_x = d.right;
}

static void print_rescale(const Rescale *o, const char *when)
{
    printf("rescale %s: %d DPI (dialog %d): item %d, field %d, combo %d, edit line %d, tree item %d; dialog item (%ld, %ld)-(%ld, %ld), font %d, DLUs %d\n",
           when, o->dpi, o->dlg_dpi, o->lb_item, o->cb_field, o->cb_h, o->ed_line, o->tv_item,
           o->item.left, o->item.top, o->item.right, o->item.bottom, o->font_h, o->dlu_x);
}

/* @hi: after the window went from 96 to 192 DPI, against @lo before it */
static void check_rescaled(const Rescale *lo, const Rescale *hi)
{
    checkv(hi->dpi, 192, "the rescale window's DPI after the change");
    checkv(hi->dlg_dpi, 192, "the rescale dialog's DPI after the change");
    check(lo->lb_item >= 14 && lo->ed_line >= 14 && lo->cb_h >= 18, "rescale sizes at 96 are plausible");
    check_near(hi->lb_item, 2 * lo->lb_item, 2, "list box item height rescaled");
    check_near(hi->cb_field, 2 * lo->cb_field, 2, "combo box field height rescaled");
    check_near(hi->cb_h, 2 * lo->cb_h, 2, "combo box height rescaled");
    check_near(hi->ed_line, 2 * lo->ed_line, 2, "edit line height rescaled");
    check_near(hi->tv_item, 2 * lo->tv_item, 2, "tree view item height rescaled (WM_DPICHANGED_AFTERPARENT)");
    check_rect(hi->item, (RECT){ 2 * lo->item.left, 2 * lo->item.top, 2 * lo->item.right, 2 * lo->item.bottom },
               "the dialog's control scaled with it");
    checkv(lo->font_h, -11, "the dialog font at 96 (8 points)");
    checkv(hi->font_h, -21, "the dialog font made again at 192");
    check_near(hi->dlu_x, 2 * lo->dlu_x, lo->dlu_x / 5, "the dialog's DLUs at 192");
}

/* -----------------------------------------------------------------------
 * The children: an unaware and a system-aware process, by __COMPAT_LAYER
 * ----------------------------------------------------------------------- */
static int child(const char *mode, int mw, int mh)
{
    int k = !strcmp(mode, "system") ? 2 : 1;
    char what[64];
    snprintf(what, sizeof(what), "%s child: awareness", mode);
    checkv(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()), k == 2 ? 1 : 0, what);
    snprintf(what, sizeof(what), "%s child: GetDpiForSystem", mode);
    checkv((long)GetDpiForSystem(), 96 * k, what);
    snprintf(what, sizeof(what), "%s child: GetDpiForMonitor", mode);
    checkv((long)monitor_dpi(primary()), 96 * k, what);
    snprintf(what, sizeof(what), "%s child: rcMonitor", mode);
    check_rect(monitor_rect(primary()), (RECT){ 0, 0, mw * k, mh * k }, what);
    snprintf(what, sizeof(what), "%s child: SM_CXSCREEN", mode);
    checkv(GetSystemMetrics(SM_CXSCREEN), mw * k, what);
    HDC dc = GetDC(NULL);
    snprintf(what, sizeof(what), "%s child: LOGPIXELSX", mode);
    checkv(GetDeviceCaps(dc, LOGPIXELSX), 96 * k, what);
    ReleaseDC(NULL, dc);
    /* the same logical place and size as the parent's window */
    HWND hw = make_window(100 * k, 100 * k, 400 * k, 300 * k);
    snprintf(what, sizeof(what), "%s child", mode);
    check_window(hw, k, 100 * k, 100 * k, 400 * k, 300 * k, what);
    DestroyWindow(hw);

    /* user32's parts at the system DPI, and a thread of the other kind:
     * an aware one in the unaware process, an unaware one in the
     * system-aware process */
    snprintf(what, sizeof(what), "%s child", mode);
    check_system_parts(k, what);
    Meas mine, other;
    measure(&mine, 100 * k, 100 * k, 400 * k, 300 * k);
    snprintf(what, sizeof(what), "%s child: its window's awareness", mode);
    checkv(mine.aware, k == 2 ? DPI_AWARENESS_SYSTEM_AWARE : DPI_AWARENESS_UNAWARE, what);
    ThreadJob j;
    memset(&j, 0, sizeof(j));
    j.ctx = k == 2 ? DPI_AWARENESS_CONTEXT_UNAWARE : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
    int ok = k == 2 ? 1 : 2;                                /* the other thread's scale */
    j.x = 100 * ok; j.y = 100 * ok; j.w = 400 * ok; j.h = 300 * ok;
    in_thread(&j);
    other = j.m;
    snprintf(what, sizeof(what), "%s child: the other thread's context", mode);
    checkv(j.aware, k == 2 ? DPI_AWARENESS_UNAWARE : DPI_AWARENESS_PER_MONITOR_AWARE, what);
    snprintf(what, sizeof(what), "%s child: the other thread's window's context", mode);
    checkv(other.aware, j.aware, what);
    snprintf(what, sizeof(what), "%s child: the other thread's SM_CXSCREEN", mode);
    checkv(j.cx_screen, mw * ok, what);
    snprintf(what, sizeof(what), "%s child: the other thread's window rectangle", mode);
    check_rect(other.wr, (RECT){ 100 * ok, 100 * ok, 500 * ok, 400 * ok }, what);
    snprintf(what, sizeof(what), "%s child: its window procedure's context", mode);
    checkv(other.proc_aware, j.aware, what);
    snprintf(what, sizeof(what), "%s child", mode);
    if (k == 2) check_scaled(&other, &mine, what); else check_scaled(&mine, &other, what);
    printf("dpitest %s child: %d passed, %d failed\n", mode, g_pass, g_fail);
    return g_fail;
}

static void run_child(const char *mode, const char *layer, int mw, int mh)
{
    char self[MAX_PATH], cl[MAX_PATH + 64];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cl, sizeof(cl), "\"%s\" child %s %d %d", self, mode, mw, mh);
    SetEnvironmentVariableA("__COMPAT_LAYER", layer);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessA(self, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("__COMPAT_LAYER", NULL);
    char what[64];
    snprintf(what, sizeof(what), "%s child started", mode);
    check(ok, what);
    if (!ok) return;
    DWORD code = 99;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    snprintf(what, sizeof(what), "%s child's failures", mode);
    checkv((long)code, 0, what);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* -----------------------------------------------------------------------
 * The test
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    InitCommonControls();
    if (argc >= 5 && !strcmp(argv[1], "child")) return child(argv[2], atoi(argv[3]), atoi(argv[4]));

    /* the manifest: per-monitor aware v2, which a call can't change */
    HANDLE ctx = GetThreadDpiAwarenessContext();
    check(AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "the manifest makes it per-monitor aware v2");
    check(IsProcessDPIAware(), "IsProcessDPIAware");
    check(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "setting the same awareness again");
    check(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) && GetLastError() == ERROR_ACCESS_DENIED,
          "the manifest's awareness can't be changed");
    PROCESS_DPI_AWARENESS pa = PROCESS_DPI_UNAWARE;
    check(GetProcessDpiAwareness(NULL, &pa) == S_OK && pa == PROCESS_PER_MONITOR_DPI_AWARE, "GetProcessDpiAwareness");
    /* a thread's context */
    HANDLE old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    check(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) == DPI_AWARENESS_UNAWARE, "SetThreadDpiAwarenessContext");
    SetThreadDpiAwarenessContext(old);
    check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "the thread's context restored");

    /* At 96 DPI (the default) aware and unaware programs see the same */
    checkv(set_dpi(0, 96), DISP_CHANGE_SUCCESSFUL, "CTL_SET_DPI 96");
    pump(200);
    HMONITOR mon = primary();
    RECT m = monitor_rect(mon);
    int mw = m.right - m.left, mh = m.bottom - m.top;
    printf("primary monitor: %d x %d logical pixels\n", mw, mh);
    checkv((long)monitor_dpi(mon), 96, "GetDpiForMonitor at 96 DPI");
    checkv((long)GetDpiForSystem(), 96, "GetDpiForSystem");
    HMODULE shcore = LoadLibraryA("shcore.dll");      /* (programs that link shcore.lib import it by name) */
    check(shcore && GetProcAddress(shcore, "GetDpiForMonitor"), "shcore.dll GetDpiForMonitor");
    HWND hw = make_window(100, 100, 400, 300);
    check_window(hw, 1, 100, 100, 400, 300, "at 96 DPI");
    check(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(hw), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
          "GetWindowDpiAwarenessContext");

    /* a window and a dialog with controls at 96 DPI, for the change */
    Rescale r96, r192, rback;
    make_rescale(hw);
    measure_rescale(&r96);
    print_rescale(&r96, "at 96 DPI");

    /* The primary at 192 DPI: twice the pixels for this process */
    int before = g_dpichanged;
    checkv(set_dpi(0, 192), DISP_CHANGE_SUCCESSFUL, "CTL_SET_DPI 192");
    check(wait_dpichanged(before), "WM_DPICHANGED when the monitor went to 192 DPI");
    checkv((long)g_dpi_wp, (long)MAKEWPARAM(192, 192), "WM_DPICHANGED wParam");
    check_rect(g_dpi_rect, (RECT){ 200, 200, 1000, 800 }, "WM_DPICHANGED suggested rectangle");
    check_window(hw, 2, 200, 200, 800, 600, "at 192 DPI");
    checkv(g_size_w, 796, "WM_SIZE width at 192 DPI");
    checkv(g_size_h, 534, "WM_SIZE height at 192 DPI");
    checkv((long)monitor_dpi(primary()), 192, "GetDpiForMonitor at 192 DPI");
    check_rect(monitor_rect(primary()), (RECT){ 0, 0, 2 * mw, 2 * mh }, "rcMonitor at 192 DPI");
    checkv(GetSystemMetrics(SM_CXSCREEN), 2 * mw, "SM_CXSCREEN at 192 DPI");
    checkv((long)GetDpiForSystem(), 96, "GetDpiForSystem stays what it was at the start");
    check(MonitorFromWindow(hw, MONITOR_DEFAULTTONULL) == primary(), "MonitorFromWindow at 192 DPI");
    pump(300);                                              /* (the other windows' changes) */
    measure_rescale(&r192);
    print_rescale(&r192, "at 192 DPI");
    check_rescaled(&r96, &r192);
    RECT a = { 0, 0, 400, 300 };
    AdjustWindowRectExForDpi(&a, WS_OVERLAPPEDWINDOW, FALSE, 0, 192);
    check_rect(a, (RECT){ -2, -64, 402, 302 }, "AdjustWindowRectExForDpi at 192");
    checkv(GetSystemMetricsForDpi(SM_CXVSCROLL, 192), 2 * GetSystemMetrics(SM_CXVSCROLL), "GetSystemMetricsForDpi");

    /* user32's own parts: a window of this (per-monitor aware) thread is
     * at 192 DPI, one an unaware thread makes at 96; the metrics and stock
     * fonts follow the system DPI, 96 here (the process started at 96) */
    check_contexts();
    check_system_parts(1, "per-monitor process");
    Meas hi, lo;
    measure(&hi, 200, 200, 800, 600);
    check_rect(hi.wr, (RECT){ 200, 200, 1000, 800 }, "the 192 DPI window's rectangle");
    checkv(hi.aware, DPI_AWARENESS_PER_MONITOR_AWARE, "the 192 DPI window's context");
    checkv(hi.proc_aware, DPI_AWARENESS_PER_MONITOR_AWARE, "the 192 DPI window procedure's context");
    ThreadJob j;
    memset(&j, 0, sizeof(j));
    j.ctx = DPI_AWARENESS_CONTEXT_UNAWARE;
    j.x = 100; j.y = 100; j.w = 400; j.h = 300;
    in_thread(&j);
    lo = j.m;
    checkv(j.aware, DPI_AWARENESS_UNAWARE, "the unaware thread's context");
    checkv(j.sys_dpi, 96, "the unaware thread's GetDpiForSystem");
    checkv(j.cx_screen, mw, "the unaware thread's SM_CXSCREEN (logical pixels)");
    checkv(lo.aware, DPI_AWARENESS_UNAWARE, "the unaware thread's window's context");
    checkv(lo.proc_aware, DPI_AWARENESS_UNAWARE, "the unaware window procedure's context");
    check_rect(lo.wr, (RECT){ 100, 100, 500, 400 }, "the unaware thread's window rectangle (logical pixels)");
    check_scaled(&lo, &hi, "per-monitor process");
    check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
          "this thread's context is still its own");
    /* a window made under another context keeps it, and its window
     * procedure runs with it whichever thread context sends to it */
    HANDLE prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    HWND uw = CreateWindowExW(0, L"dpictl", L"unaware", WS_OVERLAPPEDWINDOW, 100, 100, 300, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetThreadDpiAwarenessContext(prev);
    check(uw != NULL, "a window made under the unaware context");
    checkv(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(uw)), DPI_AWARENESS_UNAWARE,
           "GetWindowDpiAwarenessContext of it");
    checkv((long)GetDpiForWindow(uw), 96, "GetDpiForWindow of it");
    g_proc_aware = -2;
    SendMessageW(uw, WM_APP, 0, 0);
    checkv(g_proc_aware, DPI_AWARENESS_UNAWARE, "its window procedure runs unaware");
    check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
          "the thread's context afterwards");

    /* rectangles and points across awareness contexts, as on Windows: this
     * per-monitor-aware thread sees the unaware window in its own (192 DPI)
     * pixels, and an unaware context sees the aware window in logical ones */
    ShowWindow(uw, SW_SHOWNOACTIVATE);
    pump(100);
    RECT xr;
    GetWindowRect(uw, &xr);
    check_rect(xr, (RECT){ 200, 200, 800, 600 }, "GetWindowRect of the unaware window from the aware thread");
    GetClientRect(uw, &xr);
    check_rect(xr, (RECT){ 0, 0, 596, 334 }, "GetClientRect of the unaware window from the aware thread");
    POINT xp = { 0, 0 };
    ClientToScreen(uw, &xp);
    checkv(xp.x, 202, "ClientToScreen x of the unaware window from the aware thread");
    checkv(xp.y, 264, "ClientToScreen y of the unaware window from the aware thread");
    xp.x = 222; xp.y = 274;
    ScreenToClient(uw, &xp);
    check(xp.x == 20 && xp.y == 10, "ScreenToClient of the unaware window from the aware thread");
    xp.x = 10; xp.y = 10;
    MapWindowPoints(uw, NULL, &xp, 1);
    check(xp.x == 212 && xp.y == 274, "MapWindowPoints from the unaware window in the aware thread's pixels");
    SetWindowPos(uw, NULL, 1200, 200, 600, 400, SWP_NOZORDER | SWP_NOACTIVATE);
    pump(100);
    GetWindowRect(uw, &xr);
    check_rect(xr, (RECT){ 1200, 200, 1800, 600 }, "SetWindowPos of the unaware window from the aware thread");
    check(WindowFromPoint((POINT){ 1500, 400 }) == uw, "WindowFromPoint in the aware thread's pixels");
    prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    RECT ur, ar, ac;
    GetWindowRect(uw, &ur);
    GetWindowRect(hw, &ar);
    GetClientRect(hw, &ac);
    POINT ap = { 0, 0 };
    ClientToScreen(hw, &ap);
    HWND at = WindowFromPoint((POINT){ 750, 200 });
    SetThreadDpiAwarenessContext(prev);
    check_rect(ur, (RECT){ 600, 100, 900, 300 }, "GetWindowRect of the unaware window in its own context");
    check_rect(ar, (RECT){ 100, 100, 500, 400 }, "GetWindowRect of the aware window from an unaware context");
    check_rect(ac, (RECT){ 0, 0, 398, 267 }, "GetClientRect of the aware window from an unaware context");
    check(ap.x == 101 && ap.y == 132, "ClientToScreen of the aware window from an unaware context");
    check(at == uw, "WindowFromPoint in logical pixels from an unaware context");
    DestroyWindow(uw);

    /* other processes: unaware keeps 96 DPI and logical pixels; system
     * aware (started now) sees the primary's 192 everywhere */
    run_child("unaware", "DpiUnaware", mw, mh);
    run_child("system", "HighDpiAware", mw, mh);

    /* A second monitor at 96 DPI: moving there is another WM_DPICHANGED */
    RECT m2;
    int two = 0;
    for (int i = 1; i < 4 && !two; i++) {
        DISPLAY_DEVICEA dd;
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, (DWORD)i, &dd, 0)) break;
        POINT far = { 2 * mw + 10, 10 };
        HMONITOR mm = MonitorFromPoint(far, MONITOR_DEFAULTTONULL);
        if (mm && mm != primary()) { m2 = monitor_rect(mm); two = 1; }
    }
    if (two) {
        checkv((long)monitor_dpi(MonitorFromPoint((POINT){ m2.left, m2.top }, MONITOR_DEFAULTTONULL)), 96, "the second monitor's DPI");
        before = g_dpichanged;
        SetWindowPos(hw, NULL, m2.left + 50, m2.top + 50, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
        check(wait_dpichanged(before), "WM_DPICHANGED on the 96 DPI monitor");
        checkv((long)g_dpi_wp, (long)MAKEWPARAM(96, 96), "WM_DPICHANGED wParam there");
        checkv((long)GetDpiForWindow(hw), 96, "GetDpiForWindow there");
        before = g_dpichanged;
        SetWindowPos(hw, NULL, 200, 200, 400, 300, SWP_NOZORDER | SWP_NOACTIVATE);
        check(wait_dpichanged(before), "WM_DPICHANGED back on the 192 DPI monitor");
        checkv((long)GetDpiForWindow(hw), 192, "GetDpiForWindow back");
        SetWindowPos(hw, NULL, 200, 200, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
        pump(200);
    } else printf("one monitor: moving between monitors not tested\n");

    /* Back to 96 DPI */
    before = g_dpichanged;
    checkv(set_dpi(0, 96), DISP_CHANGE_SUCCESSFUL, "CTL_SET_DPI 96 again");
    check(wait_dpichanged(before), "WM_DPICHANGED back at 96 DPI");
    checkv((long)g_dpi_wp, (long)MAKEWPARAM(96, 96), "WM_DPICHANGED wParam at 96");
    check_rect(g_dpi_rect, (RECT){ 100, 100, 500, 400 }, "the suggested rectangle at 96");
    check_window(hw, 1, 100, 100, 400, 300, "back at 96 DPI");
    checkv((long)monitor_dpi(primary()), 96, "GetDpiForMonitor back at 96");
    pump(300);
    measure_rescale(&rback);
    print_rescale(&rback, "back at 96 DPI");
    checkv(rback.dpi, 96, "the rescale window's DPI back at 96");
    check_near(rback.lb_item, r96.lb_item, 1, "list box item height back at 96");
    check_near(rback.cb_h, r96.cb_h, 1, "combo box height back at 96");
    check_near(rback.ed_line, r96.ed_line, 1, "edit line height back at 96");
    check_near(rback.tv_item, r96.tv_item, 1, "tree view item height back at 96");
    check_rect(rback.item, r96.item, "the dialog's control back at 96");
    checkv(rback.font_h, r96.font_h, "the dialog font back at 96");
    if (g_rdlg) DestroyWindow(g_rdlg);
    if (g_rw) DestroyWindow(g_rw);
    DestroyWindow(hw);

    printf("dpitest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
