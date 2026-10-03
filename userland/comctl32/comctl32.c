/*
 * comctl32.dll — the common controls: their classes are registered when
 * the DLL loads (as version 6 does); subclassing, initialization, task
 * dialogs, and the helpers the controls share
 */
#include "cc.h"

/* -----------------------------------------------------------------------
 * Shared drawing
 * ----------------------------------------------------------------------- */
static HFONT g_font[3];
static HFONT font_k(int k)
{
    if (!g_font[k]) g_font[k] = CreateFontW(-12 * k, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    return g_font[k];
}
HFONT cc_font(void) { return font_k(1); }

/* The UI font at the window's DPI (twice the size in a 192 DPI window) */
HFONT cc_font_for(HWND h) { return font_k(h && GetDpiForWindow(h) >= 192 ? 2 : 1); }

void cc_fill(HDC dc, const RECT *r, COLORREF c)
{
    COLORREF o = SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
    SetBkColor(dc, o);
}

void cc_frame(HDC dc, const RECT *r, COLORREF c)
{
    RECT e;
    SetRect(&e, r->left, r->top, r->right, r->top + 1); cc_fill(dc, &e, c);
    SetRect(&e, r->left, r->bottom - 1, r->right, r->bottom); cc_fill(dc, &e, c);
    SetRect(&e, r->left, r->top, r->left + 1, r->bottom); cc_fill(dc, &e, c);
    SetRect(&e, r->right - 1, r->top, r->right, r->bottom); cc_fill(dc, &e, c);
}

void cc_arrow(HDC dc, const RECT *r, int dir, COLORREF c)
{
    int w = r->right - r->left, h = r->bottom - r->top;
    int s = MIN(w, h) / 3;
    if (s < 2) s = 2;
    if (s > 5) s = 5;
    int cx = r->left + w / 2, cy = r->top + h / 2;
    for (int i = 0; i < s; i++) {
        RECT l;
        switch (dir) {
        case 0: SetRect(&l, cx - i, cy - s / 2 + i, cx + i + 1, cy - s / 2 + i + 1); break;
        case 1: SetRect(&l, cx - (s - 1 - i), cy - s / 2 + i, cx + (s - 1 - i) + 1, cy - s / 2 + i + 1); break;
        case 2: SetRect(&l, cx - s / 2 + i, cy - i, cx - s / 2 + i + 1, cy + i + 1); break;
        default: SetRect(&l, cx - s / 2 + i, cy - (s - 1 - i), cx - s / 2 + i + 1, cy + (s - 1 - i) + 1); break;
        }
        cc_fill(dc, &l, c);
    }
}

int cc_text_w(HDC dc, const WCHAR *s, int n)
{
    if (n < 0) n = wlen(s);
    if (!n) return 0;
    SIZE sz;
    GetTextExtentPoint32W(dc, s, n, &sz);
    return sz.cx;
}

int cc_font_h(HFONT f)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f ? f : cc_font());
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

/* -----------------------------------------------------------------------
 * Classes
 * ----------------------------------------------------------------------- */
static HINSTANCE g_inst;

ATOM cc_register(LPCWSTR name, WNDPROC proc, UINT style, HBRUSH brush)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = style | CS_GLOBALCLASS;
    wc.lpfnWndProc = proc;
    wc.cbWndExtra = sizeof(void *) * 2;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = brush;
    wc.lpszClassName = name;
    return RegisterClassExW(&wc);
}

static int g_registered;
static void register_all(void)
{
    if (g_registered) return;
    g_registered = 1;
    cc_register(PROGRESS_CLASSW, ProgressProc, CS_HREDRAW | CS_VREDRAW, 0);
    cc_register(STATUSCLASSNAMEW, StatusProc, CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, 0);
    cc_register(TOOLTIPS_CLASSW, TooltipProc, CS_SAVEBITS, 0);
    cc_register(UPDOWN_CLASSW, UpDownProc, CS_HREDRAW | CS_VREDRAW, 0);
    cc_register(TRACKBAR_CLASSW, TrackbarProc, CS_HREDRAW | CS_VREDRAW, 0);
    cc_register(WC_TABCONTROLW, TabProc, CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, 0);
    cc_register(WC_HEADERW, HeaderProc, CS_DBLCLKS, 0);
    cc_register(WC_LISTVIEWW, ListViewProc, CS_DBLCLKS, 0);
    cc_register(TOOLBARCLASSNAMEW, ToolbarProc, CS_DBLCLKS, 0);
    cc_register(REBARCLASSNAMEW, RebarProc, CS_DBLCLKS, 0);
    cc_register(WC_COMBOBOXEXW, ComboExProc, CS_DBLCLKS, 0);
    cc_register(WC_TREEVIEWW, TreeViewProc, CS_DBLCLKS, 0);
    cc_register(WC_LINK, LinkProc, CS_DBLCLKS, 0);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { g_inst = inst; register_all(); }
    return TRUE;
}

CC void WINAPI InitCommonControls(void) { register_all(); }
CC BOOL WINAPI InitCommonControlsEx(const INITCOMMONCONTROLSEX *icc) { (void)icc; register_all(); return TRUE; }
CC BOOL WINAPI _TrackMouseEvent(LPTRACKMOUSEEVENT tme) { return TrackMouseEvent(tme); }
CC HRESULT WINAPI DllGetVersion(DWORD *info)
{
    if (!info || info[0] < 20) return 0x80070057L;
    info[1] = 6; info[2] = 16; info[3] = 0; info[4] = 2;    /* 6.16, NT platform */
    return 0;
}

CC HRESULT WINAPI LoadIconMetric(HINSTANCE h, LPCWSTR name, int metric, HICON *out)
{
    int s = metric ? 32 : 16;
    *out = LoadImageW(h, name, IMAGE_ICON, s, s, 0);
    return *out ? 0 : 0x80004005L;
}

CC HRESULT WINAPI LoadIconWithScaleDown(HINSTANCE h, LPCWSTR name, int cx, int cy, HICON *out)
{
    *out = LoadImageW(h, name, IMAGE_ICON, cx, cy, 0);
    return *out ? 0 : 0x80004005L;
}

CC void WINAPI GetEffectiveClientRect(HWND h, LPRECT r, const INT *info)
{
    GetClientRect(h, r);
    /* info: pairs of (menu id, control id), ended by 0; visible controls come off the rectangle */
    for (const INT *p = info; p && p[0]; p += 2) {
        if (!p[1]) continue;
        HWND c = GetDlgItem(h, p[1]);
        if (!c || !IsWindowVisible(c)) continue;
        RECT cr;
        GetWindowRect(c, &cr);
        MapWindowPoints(NULL, h, (POINT *)&cr, 2);
        SubtractRect(r, r, &cr);
    }
}

CC void WINAPI MenuHelp(UINT msg, WPARAM wp, LPARAM lp, HMENU main, HINSTANCE inst, HWND status, UINT *ids)
{
    (void)msg; (void)wp; (void)lp; (void)main; (void)inst; (void)ids;
    if (status) SendMessageW(status, SB_SIMPLE, FALSE, 0);
}
CC BOOL WINAPI ShowHideMenuCtl(HWND h, UINT_PTR id, LPINT info) { (void)h; (void)id; (void)info; return TRUE; }
CC BOOL WINAPI MakeDragList(HWND h) { (void)h; return TRUE; }
CC void WINAPI DrawInsert(HWND p, HWND lb, int item) { (void)p; (void)lb; (void)item; }
CC int WINAPI LBItemFromPt(HWND lb, POINT pt, BOOL scroll) { (void)scroll; ScreenToClient(lb, &pt); return (int)(short)LOWORD(SendMessageW(lb, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y))); }
CC BOOL WINAPI InitMUILanguage(LANGID l) { (void)l; return TRUE; }
CC LANGID WINAPI GetMUILanguage(void) { return 0x0409; }
CC void WINAPI DrawStatusTextW(HDC dc, LPCRECT r, LPCWSTR s, UINT f)
{
    (void)f;
    RECT t = *r;
    cc_fill(dc, &t, GetSysColor(COLOR_3DFACE));
    t.left += 4;
    int m = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s ? s : L"", -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SetBkMode(dc, m);
}
CC void WINAPI DrawStatusTextA(HDC dc, LPCRECT r, LPCSTR s, UINT f)
{
    WCHAR w[512];
    MultiByteToWideChar(CP_ACP, 0, s ? s : "", -1, w, 512);
    DrawStatusTextW(dc, r, w, f);
}

CC HWND WINAPI CreateStatusWindowW(LONG style, LPCWSTR text, HWND parent, UINT id)
{
    return CreateWindowExW(0, STATUSCLASSNAMEW, text, (DWORD)style, 0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, g_inst, NULL);
}

CC HWND WINAPI CreateStatusWindowA(LONG style, LPCSTR text, HWND parent, UINT id)
{
    WCHAR w[512];
    MultiByteToWideChar(CP_ACP, 0, text ? text : "", -1, w, 512);
    return CreateStatusWindowW(style, w, parent, id);
}

CC HWND WINAPI CreateUpDownControl(DWORD style, int x, int y, int cx, int cy, HWND parent, int id, HINSTANCE inst,
                                   HWND buddy, int up, int low, int pos)
{
    (void)inst;
    HWND h = CreateWindowExW(0, UPDOWN_CLASSW, NULL, style, x, y, cx, cy, parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    if (!h) return 0;
    if (buddy) SendMessageW(h, UDM_SETBUDDY, (WPARAM)buddy, 0);
    SendMessageW(h, UDM_SETRANGE32, (WPARAM)low, up);
    SendMessageW(h, UDM_SETPOS32, 0, pos);
    return h;
}

CC HWND WINAPI CreateToolbarEx(HWND parent, DWORD style, UINT id, int nbitmaps, HINSTANCE binst, UINT_PTR bid,
                               LPCTBBUTTON buttons, int nbuttons, int dxb, int dyb, int dxbmp, int dybmp, UINT size)
{
    HWND h = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, style, 0, 0, 100, 30, parent, (HMENU)(UINT_PTR)id, g_inst, NULL);
    if (!h) return 0;
    SendMessageW(h, TB_BUTTONSTRUCTSIZE, size, 0);
    if (dxbmp && dybmp) SendMessageW(h, TB_SETBITMAPSIZE, 0, MAKELPARAM(dxbmp, dybmp));
    if (dxb && dyb) SendMessageW(h, TB_SETBUTTONSIZE, 0, MAKELPARAM(dxb, dyb));
    if (nbitmaps > 0) {
        TBADDBITMAP ab = { binst, bid };
        SendMessageW(h, TB_ADDBITMAP, (WPARAM)nbitmaps, (LPARAM)&ab);
    }
    if (nbuttons > 0) SendMessageW(h, TB_ADDBUTTONSW, (WPARAM)nbuttons, (LPARAM)buttons);
    return h;
}

CC HWND WINAPI CreateToolbar(HWND parent, DWORD style, UINT id, int nbitmaps, HINSTANCE binst, UINT bid, LPCTBBUTTON buttons, int n)
{
    return CreateToolbarEx(parent, style, id, nbitmaps, binst, bid, buttons, n, 0, 0, 0, 0, 20);
}

CC HBITMAP WINAPI CreateMappedBitmap(HINSTANCE inst, INT_PTR id, UINT flags, void *map, int n)
{
    (void)flags; (void)map; (void)n;
    return LoadBitmapW(inst, (LPCWSTR)id);
}

/* -----------------------------------------------------------------------
 * Subclassing: a chain of procedures in front of the window's own
 * ----------------------------------------------------------------------- */
typedef struct { SUBCLASSPROC fn; UINT_PTR id; DWORD_PTR data; } Sub;
typedef struct { Sub s[16]; int n; WNDPROC orig; int level; int wide; } SubInfo;
static const WCHAR SUBPROP[] = L"NovaCC32Subclass";

static LRESULT call_level(HWND h, SubInfo *si, int i, UINT msg, WPARAM wp, LPARAM lp)
{
    int save = si->level;
    si->level = i;
    LRESULT r;
    if (i < 0) r = si->wide ? CallWindowProcW(si->orig, h, msg, wp, lp) : CallWindowProcA(si->orig, h, msg, wp, lp);
    else r = si->s[i].fn(h, msg, wp, lp, si->s[i].id, si->s[i].data);
    si->level = save;
    return r;
}

static LRESULT CALLBACK subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    SubInfo *si = GetPropW(h, SUBPROP);
    if (!si) return DefWindowProcW(h, msg, wp, lp);
    LRESULT r = call_level(h, si, si->n - 1, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        si = GetPropW(h, SUBPROP);
        if (si) {
            RemovePropW(h, SUBPROP);
            if ((WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) == subclass_proc) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)si->orig);
            free(si);
        }
    }
    return r;
}

CC BOOL WINAPI SetWindowSubclass(HWND h, SUBCLASSPROC fn, UINT_PTR id, DWORD_PTR data)
{
    if (!IsWindow(h) || !fn) return FALSE;
    SubInfo *si = GetPropW(h, SUBPROP);
    if (!si) {
        si = calloc(1, sizeof(SubInfo));
        if (!si) return FALSE;
        si->wide = IsWindowUnicode(h);
        si->orig = (WNDPROC)(si->wide ? GetWindowLongPtrW(h, GWLP_WNDPROC) : GetWindowLongPtrA(h, GWLP_WNDPROC));
        si->level = -2;
        SetPropW(h, SUBPROP, si);
        if (si->wide) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)subclass_proc);
        else SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)subclass_proc);
    }
    for (int i = 0; i < si->n; i++)
        if (si->s[i].fn == fn && si->s[i].id == id) { si->s[i].data = data; return TRUE; }
    if (si->n >= 16) return FALSE;
    si->s[si->n].fn = fn; si->s[si->n].id = id; si->s[si->n].data = data;
    si->n++;
    return TRUE;
}

CC BOOL WINAPI GetWindowSubclass(HWND h, SUBCLASSPROC fn, UINT_PTR id, DWORD_PTR *data)
{
    SubInfo *si = GetPropW(h, SUBPROP);
    for (int i = 0; si && i < si->n; i++)
        if (si->s[i].fn == fn && si->s[i].id == id) { if (data) *data = si->s[i].data; return TRUE; }
    return FALSE;
}

CC BOOL WINAPI RemoveWindowSubclass(HWND h, SUBCLASSPROC fn, UINT_PTR id)
{
    SubInfo *si = GetPropW(h, SUBPROP);
    if (!si) return FALSE;
    for (int i = 0; i < si->n; i++) {
        if (si->s[i].fn != fn || si->s[i].id != id) continue;
        memmove(&si->s[i], &si->s[i + 1], sizeof(Sub) * (size_t)(si->n - i - 1));
        si->n--;
        if (si->level >= i) si->level--;
        if (!si->n && si->level < -1) {
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)si->orig);
            RemovePropW(h, SUBPROP);
            free(si);
        }
        return TRUE;
    }
    return FALSE;
}

CC LRESULT WINAPI DefSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    SubInfo *si = GetPropW(h, SUBPROP);
    if (!si) return DefWindowProcW(h, msg, wp, lp);
    return call_level(h, si, si->level - 1, msg, wp, lp);
}


/* -----------------------------------------------------------------------
 * Task dialogs: a message box with the same parts
 * ----------------------------------------------------------------------- */
/* TASKDIALOGCONFIG is byte-packed in the Windows headers */
#pragma pack(push, 1)
typedef struct { int nButtonID; LPCWSTR pszButtonText; } TASKDIALOG_BUTTON_;
typedef struct {
    UINT cbSize; HWND hwndParent; HINSTANCE hInstance; DWORD dwFlags, dwCommonButtons;
    LPCWSTR pszWindowTitle; LPCWSTR pszMainIcon; LPCWSTR pszMainInstruction, pszContent;
    UINT cButtons; const TASKDIALOG_BUTTON_ *pButtons; int nDefaultButton;
    UINT cRadioButtons; const TASKDIALOG_BUTTON_ *pRadioButtons; int nDefaultRadioButton;
    LPCWSTR pszVerificationText, pszExpandedInformation, pszExpandedControlText, pszCollapsedControlText;
    LPCWSTR pszFooterIcon, pszFooter;
    void *pfCallback; LONG_PTR lpCallbackData;
    UINT cxWidth;
} TASKDIALOGCONFIG_;
#pragma pack(pop)

static LPCWSTR res_str(HINSTANCE inst, LPCWSTR s, WCHAR *buf, int n)
{
    if (!s) return NULL;
    if ((ULONG_PTR)s < 0x10000) { LoadStringW(inst, (UINT)(ULONG_PTR)s, buf, n); return buf; }
    return s;
}

static int show(HWND owner, HINSTANCE inst, LPCWSTR title, LPCWSTR main, LPCWSTR content, DWORD buttons, LPCWSTR icon)
{
    WCHAR tb[256], mb[1024], cb[2048];
    title = res_str(inst, title, tb, 256);
    main = res_str(inst, main, mb, 1024);
    content = res_str(inst, content, cb, 2048);
    int n1 = wlen(main), n2 = wlen(content);
    WCHAR *body = malloc(2 * ((size_t)n1 + n2 + 4));
    if (!body) return IDCANCEL;
    int o = 0;
    if (n1) { memcpy(body, main, 2 * (size_t)n1); o = n1; }
    if (n1 && n2) { body[o++] = '\n'; body[o++] = '\n'; }
    if (n2) { memcpy(body + o, content, 2 * (size_t)n2); o += n2; }
    body[o] = 0;
    /* TDCBF_OK 1, YES 2, NO 4, CANCEL 8, RETRY 0x10, CLOSE 0x20 */
    UINT type = MB_OK;
    if ((buttons & 6) == 6) type = (buttons & 8) ? MB_YESNOCANCEL : MB_YESNO;
    else if ((buttons & 0x18) == 0x18) type = MB_RETRYCANCEL;
    else if ((buttons & 9) == 9) type = MB_OKCANCEL;
    if (icon == MAKEINTRESOURCEW(-2)) type |= MB_ICONERROR;             /* TD_ERROR_ICON */
    else if (icon == MAKEINTRESOURCEW(-1)) type |= MB_ICONWARNING;      /* TD_WARNING_ICON */
    else if (icon == MAKEINTRESOURCEW(-3)) type |= MB_ICONINFORMATION;  /* TD_INFORMATION_ICON */
    int r = MessageBoxW(owner, body, title ? title : L"", type);
    free(body);
    return r ? r : IDCANCEL;
}

CC HRESULT WINAPI TaskDialog(HWND owner, HINSTANCE inst, LPCWSTR title, LPCWSTR main, LPCWSTR content, DWORD buttons,
                             LPCWSTR icon, int *pressed)
{
    int r = show(owner, inst, title, main, content, buttons, icon);
    if (pressed) *pressed = r;
    return 0;
}

/* Custom buttons cannot be shown by a message box: the dialog's default
 * button (else its first custom button) is taken as pressed, after the
 * text has been shown with OK */
CC HRESULT WINAPI TaskDialogIndirect(const TASKDIALOGCONFIG_ *c, int *button, int *radio, BOOL *verify)
{
    if (!c || c->cbSize < 36) return 0x80070057L;
    DWORD common = c->dwCommonButtons;
    int custom = 0;
    if (c->cbSize >= sizeof(*c) && c->cButtons && c->pButtons) {
        custom = c->pButtons[0].nButtonID;
        for (UINT i = 0; i < c->cButtons; i++) if (c->pButtons[i].nButtonID == c->nDefaultButton) custom = c->nDefaultButton;
        if (!common) common = 1;                                /* TDCBF_OK_BUTTON */
    }
    int r = show(c->hwndParent, c->hInstance, c->pszWindowTitle, c->pszMainInstruction, c->pszContent, common,
                 (c->dwFlags & 2) ? NULL : c->pszMainIcon);
    if (custom && (r == IDOK || !c->dwCommonButtons)) r = custom;
    if (button) *button = r;
    if (radio) *radio = c->cbSize >= sizeof(*c) && c->cRadioButtons && c->pRadioButtons ? (c->nDefaultRadioButton ? c->nDefaultRadioButton : c->pRadioButtons[0].nButtonID) : 0;
    if (verify) *verify = FALSE;
    return 0;
}

/* -----------------------------------------------------------------------
 * Flat scroll bars and friends: the plain ones
 * ----------------------------------------------------------------------- */
CC BOOL WINAPI InitializeFlatSB(HWND h) { (void)h; return TRUE; }
CC HRESULT WINAPI UninitializeFlatSB(HWND h) { (void)h; return 0; }
CC int WINAPI FlatSB_SetScrollInfo(HWND h, int bar, LPSCROLLINFO si, BOOL redraw) { return SetScrollInfo(h, bar, si, redraw); }
CC BOOL WINAPI FlatSB_GetScrollInfo(HWND h, int bar, LPSCROLLINFO si) { return GetScrollInfo(h, bar, si); }
CC int WINAPI FlatSB_SetScrollPos(HWND h, int bar, int pos, BOOL redraw) { return SetScrollPos(h, bar, pos, redraw); }
CC int WINAPI FlatSB_GetScrollPos(HWND h, int bar) { return GetScrollPos(h, bar); }
CC BOOL WINAPI FlatSB_ShowScrollBar(HWND h, int bar, BOOL show) { return ShowScrollBar(h, bar, show); }
CC BOOL WINAPI FlatSB_EnableScrollBar(HWND h, int bar, UINT arrows) { return EnableScrollBar(h, (UINT)bar, arrows); }
CC BOOL WINAPI FlatSB_SetScrollProp(HWND h, UINT i, INT_PTR v, BOOL redraw) { (void)h; (void)i; (void)v; (void)redraw; return TRUE; }

/* Str_SetPtrW: replace a heap string */
CC BOOL WINAPI Str_SetPtrW(LPWSTR *p, LPCWSTR s)
{
    WCHAR *n = s ? wdup(s) : NULL;
    if (s && !n) return FALSE;
    free(*p);
    *p = n;
    return TRUE;
}
