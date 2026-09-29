/*
 * dialog.c — dialog boxes: templates (DLGTEMPLATE and DLGTEMPLATEEX),
 * modeless and modal dialogs, DefDlgProc, the dialog manager's keyboard
 * interface (IsDialogMessage), dialog item helpers, and message boxes
 */
#include "u32.h"

typedef struct {
    int     ended;
    INT_PTR result;
    HFONT   font;
    int     own_font;
    int     xb, yb;                 /* base units */
    WORD    defid;
    HWND    focus;                  /* focus to restore on activation */
    int     proc_wide;
    int     msgbox;
} Dlg;

static Dlg *dlg_of(Wnd *w) { return w && (w->flags & WF_DIALOG) ? (Dlg *)w->ctl : NULL; }

static DLGPROC dlg_proc(Wnd *w)
{
    if (!w->extra || !w->cls || w->cls->extra < DWLP_USER + 8) return NULL;
    DLGPROC p;
    memcpy(&p, w->extra + DWLP_DLGPROC, sizeof(p));
    return p;
}

static void set_msgresult(Wnd *w, LRESULT r)
{
    if (w->extra && w->cls && w->cls->extra >= DWLP_MSGRESULT + 8) memcpy(w->extra + DWLP_MSGRESULT, &r, sizeof(r));
}

static LRESULT get_msgresult(Wnd *w)
{
    LRESULT r = 0;
    if (w->extra && w->cls && w->cls->extra >= DWLP_MSGRESULT + 8) memcpy(&r, w->extra + DWLP_MSGRESULT, sizeof(r));
    return r;
}

/* -----------------------------------------------------------------------
 * Base units
 * ----------------------------------------------------------------------- */
static void font_base_units(HFONT f, int *xb, int *yb)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, f ? f : GetStockObject(SYSTEM_FONT));
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE sz;
    static const WCHAR abc[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    GetTextExtentPoint32W(dc, abc, 52, &sz);
    SelectObject(dc, of);
    *xb = (sz.cx / 26 + 1) / 2;
    *yb = tm.tmHeight;
    if (*xb < 1) *xb = 6;
    if (*yb < 1) *yb = 13;
}

USERAPI LONG GetDialogBaseUnits(void)
{
    static int xb, yb;
    if (!xb) font_base_units(NULL, &xb, &yb);
    return MAKELONG(xb, yb);
}

USERAPI BOOL MapDialogRect(HWND h, LPRECT r)
{
    Wnd *w = W(h);
    Dlg *d = dlg_of(w);
    int xb, yb;
    if (d) { xb = d->xb; yb = d->yb; }
    else { LONG b = GetDialogBaseUnits(); xb = LOWORD(b); yb = HIWORD(b); }
    r->left = MulDiv(r->left, xb, 4); r->right = MulDiv(r->right, xb, 4);
    r->top = MulDiv(r->top, yb, 8); r->bottom = MulDiv(r->bottom, yb, 8);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Templates
 * ----------------------------------------------------------------------- */
typedef struct {
    DWORD style, exstyle, help;
    int x, y, cx, cy;
    LPCWSTR menu, cls, title;
    int has_font;
    WORD pt, weight;
    BYTE italic, charset;
    LPCWSTR face;
    int items;
    const BYTE *first;
    int ex;
} DlgHdr;

typedef struct {
    DWORD style, exstyle, help, id;
    int x, y, cx, cy;
    LPCWSTR cls, title;
    const BYTE *data;
    const BYTE *next;
} DlgItem;

static const BYTE *sz_or_ord(const BYTE *p, LPCWSTR *out)
{
    WORD w = *(const WORD *)p;
    if (w == 0) { *out = NULL; return p + 2; }
    if (w == 0xFFFF) { *out = (LPCWSTR)(ULONG_PTR)*(const WORD *)(p + 2); return p + 4; }
    *out = (LPCWSTR)p;
    while (*(const WCHAR *)p) p += 2;
    return p + 2;
}

static const BYTE *align4(const BYTE *p, const BYTE *base) { (void)base; return (const BYTE *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }

static void parse_header(const BYTE *p, DlgHdr *h)
{
    memset(h, 0, sizeof(*h));
    const BYTE *base = p;
    if (*(const WORD *)p == 1 && *(const WORD *)(p + 2) == 0xFFFF) {
        h->ex = 1;
        h->help = *(const DWORD *)(p + 4);
        h->exstyle = *(const DWORD *)(p + 8);
        h->style = *(const DWORD *)(p + 12);
        h->items = *(const WORD *)(p + 16);
        h->x = *(const short *)(p + 18); h->y = *(const short *)(p + 20);
        h->cx = *(const short *)(p + 22); h->cy = *(const short *)(p + 24);
        p += 26;
    } else {
        h->style = *(const DWORD *)p;
        h->exstyle = *(const DWORD *)(p + 4);
        h->items = *(const WORD *)(p + 8);
        h->x = *(const short *)(p + 10); h->y = *(const short *)(p + 12);
        h->cx = *(const short *)(p + 14); h->cy = *(const short *)(p + 16);
        p += 18;
    }
    p = sz_or_ord(p, &h->menu);
    p = sz_or_ord(p, &h->cls);
    LPCWSTR t;
    p = sz_or_ord(p, &t);
    h->title = (ULONG_PTR)t < 0x10000 ? L"" : t;
    if (h->style & DS_SETFONT) {
        h->has_font = 1;
        h->pt = *(const WORD *)p; p += 2;
        if (h->ex) { h->weight = *(const WORD *)p; h->italic = p[2]; h->charset = p[3]; p += 4; }
        h->face = (LPCWSTR)p;
        while (*(const WCHAR *)p) p += 2;
        p += 2;
    }
    h->first = align4(p, base);
}

static const BYTE *parse_item(const BYTE *p, int ex, DlgItem *it)
{
    memset(it, 0, sizeof(*it));
    if (ex) {
        it->help = *(const DWORD *)p;
        it->exstyle = *(const DWORD *)(p + 4);
        it->style = *(const DWORD *)(p + 8);
        it->x = *(const short *)(p + 12); it->y = *(const short *)(p + 14);
        it->cx = *(const short *)(p + 16); it->cy = *(const short *)(p + 18);
        it->id = *(const DWORD *)(p + 20);
        p += 24;
    } else {
        it->style = *(const DWORD *)p;
        it->exstyle = *(const DWORD *)(p + 4);
        it->x = *(const short *)(p + 8); it->y = *(const short *)(p + 10);
        it->cx = *(const short *)(p + 12); it->cy = *(const short *)(p + 14);
        it->id = *(const WORD *)(p + 16);
        p += 18;
    }
    p = sz_or_ord(p, &it->cls);
    p = sz_or_ord(p, &it->title);
    WORD extra = *(const WORD *)p;
    it->data = extra ? p : NULL;
    p += 2 + extra;
    it->next = align4(p, NULL);
    return it->next;
}

static LPCWSTR builtin_class(LPCWSTR c)
{
    if ((ULONG_PTR)c >= 0x10000) return c;
    switch ((WORD)(ULONG_PTR)c) {
    case 0x80: return L"Button";
    case 0x81: return L"Edit";
    case 0x82: return L"Static";
    case 0x83: return L"ListBox";
    case 0x84: return L"ScrollBar";
    case 0x85: return L"ComboBox";
    }
    return c;
}

static HFONT make_font(const DlgHdr *h)
{
    if (!h->has_font) return NULL;
    if (h->pt == 0x7FFF) return gui_font();
    int height = -MulDiv(h->pt, 96, 72);
    const WCHAR *face = h->face;
    if (!face || !*face || !wcsicmp_(face, L"MS Shell Dlg") || !wcsicmp_(face, L"MS Shell Dlg 2") || !wcsicmp_(face, L"MS Sans Serif"))
        face = L"Segoe UI";
    return CreateFontW(height, 0, 0, 0, h->ex && h->weight ? h->weight : FW_NORMAL, h->italic, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, face);
}

/* -----------------------------------------------------------------------
 * Creating dialogs
 * ----------------------------------------------------------------------- */
static HWND first_tab(HWND dlg);

static HWND create_dialog(HINSTANCE inst, const void *tmpl, HWND hparent, DLGPROC proc, LPARAM param, int wide, int modal)
{
    if (!tmpl) { SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return 0; }
    DlgHdr hd;
    parse_header(tmpl, &hd);
    DWORD style = hd.style, ex = hd.exstyle;
    if (style & DS_CONTROL) { style &= ~(WS_CAPTION | WS_SYSMENU); ex |= WS_EX_CONTROLPARENT; }
    if (style & DS_MODALFRAME) ex |= WS_EX_DLGMODALFRAME;
    if (modal) style &= ~WS_CHILD;
    HFONT font = make_font(&hd);
    int xb, yb;
    if (font) font_base_units(font, &xb, &yb);
    else { LONG b = GetDialogBaseUnits(); xb = LOWORD(b); yb = HIWORD(b); }

    Wnd *parent = hparent ? W_quiet(hparent) : NULL;
    HWND owner = hparent;
    if (!(style & WS_CHILD) && parent) owner = top_of(parent)->h;
    HMENU menu = hd.menu ? LoadMenuW(inst, hd.menu) : NULL;

    RECT r = { 0, 0, MulDiv(hd.cx, xb, 4), MulDiv(hd.cy, yb, 8) };
    AdjustWindowRectEx(&r, style, menu != NULL, ex);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = MulDiv(hd.x, xb, 4), y = MulDiv(hd.y, yb, 8);
    if (!(style & WS_CHILD)) {
        INT32 wa[4];
        RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (NtNovaGuiCtl(0, CTL_WORKAREA, 0, wa)) SetRect(&work, wa[0], wa[1], wa[0] + wa[2], wa[1] + wa[3]);
        Wnd *o = owner ? W_quiet(owner) : NULL;
        if (style & (DS_CENTER | DS_CENTERMOUSE) || (hd.x == 0 && hd.y == 0)) {
            RECT c = work;
            if (o && (o->style & WS_VISIBLE) && !o->minimized) GetWindowRect(o->h, &c);
            if (style & DS_CENTERMOUSE) { POINT p; GetCursorPos(&p); c.left = c.right = p.x; c.top = c.bottom = p.y; }
            x = (c.left + c.right) / 2 - w / 2;
            y = (c.top + c.bottom) / 2 - h / 2;
        } else if (!(style & DS_ABSALIGN) && o) {
            POINT p = { 0, 0 };
            ClientToScreen(o->h, &p);
            x += p.x + r.left; y += p.y + r.top;
        }
        if (x + w > work.right) x = work.right - w;
        if (y + h > work.bottom) y = work.bottom - h;
        if (x < work.left) x = work.left;
        if (y < work.top) y = work.top;
    }
    LPCWSTR cls = hd.cls ? builtin_class(hd.cls) : L"#32770";
    HWND dh = CreateWindowExW(ex, cls, hd.title, style & ~WS_VISIBLE, x, y, w, h,
                              (style & WS_CHILD) ? hparent : owner, menu ? menu : 0, inst, (LPVOID)param);
    Wnd *dw = W_quiet(dh);
    if (!dw) { if (font && font != gui_font()) DeleteObject(font); return 0; }
    Dlg *d = calloc(1, sizeof(Dlg));
    if (!d) { DestroyWindow(dh); return 0; }
    if (dw->ctl && (dw->flags & WF_DIALOG)) free(dw->ctl);
    dw->ctl = d;
    dw->flags |= WF_DIALOG;
    d->font = font ? font : NULL;
    d->own_font = font && font != gui_font();
    d->xb = xb; d->yb = yb;
    d->proc_wide = wide;
    if (!dw->extra || dw->cls->extra < DLGWINDOWEXTRA) {
        BYTE *e = calloc(1, DLGWINDOWEXTRA + 8);
        if (dw->extra && dw->cls->extra) memcpy(e, dw->extra, (size_t)dw->cls->extra);
        free(dw->extra);
        dw->extra = e;
    }
    if (font) SendMessageW(dh, WM_SETFONT, (WPARAM)font, 0);
    if (!W_quiet(dh)) return 0;

    /* the controls */
    const BYTE *p = hd.first;
    for (int i = 0; i < hd.items; i++) {
        DlgItem it;
        p = parse_item(p, hd.ex, &it);
        LPCWSTR icls = builtin_class(it.cls);
        WCHAR num[16];
        LPCWSTR title = it.title;
        if (title && (ULONG_PTR)title < 0x10000) {
            int v = (int)(ULONG_PTR)title, k = 0;
            WCHAR tmp[8];
            num[0] = '#';
            do { tmp[k++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
            for (int j = 0; j < k; j++) num[1 + j] = tmp[k - 1 - j];
            num[1 + k] = 0;
            title = num;
        }
        int cx = MulDiv(it.x, xb, 4), cy = MulDiv(it.y, yb, 8), cw = MulDiv(it.cx, xb, 4), ch = MulDiv(it.cy, yb, 8);
        HWND c = CreateWindowExW(it.exstyle | WS_EX_NOPARENTNOTIFY, icls, title, it.style | WS_CHILD, cx, cy, cw, ch, dh,
                                 (HMENU)(ULONG_PTR)it.id, inst, (LPVOID)it.data);
        if (!W_quiet(dh)) return 0;
        if (!c) {
            if (!(hd.style & DS_NOFAILCREATE)) { DestroyWindow(dh); return 0; }
            continue;
        }
        if (font) SendMessageW(c, WM_SETFONT, (WPARAM)font, 0);
        if ((it.style & 0xF) == BS_DEFPUSHBUTTON && ((ULONG_PTR)it.cls < 0x10000 ? (ULONG_PTR)it.cls == 0x80 : !wcsicmp_(icls, L"Button")))
            d->defid = (WORD)it.id;
    }
    /* the procedure, then WM_INITDIALOG */
    if (dw->extra) memcpy(dw->extra + DWLP_DLGPROC, &proc, sizeof(proc));
    HWND first = first_tab(dh);
    LRESULT r0 = SendMessageW(dh, WM_INITDIALOG, (WPARAM)first, param);
    if (!W_quiet(dh)) return 0;
    if (r0) {
        HWND f = first_tab(dh);
        if (f) {
            set_focus(f);
            if (SendMessageW(f, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(f, EM_SETSEL, 0, -1);
        }
    }
    if (W_quiet(dh) && (hd.style & WS_VISIBLE) && !modal) { ShowWindow(dh, SW_SHOWNORMAL); UpdateWindow(dh); }
    return W_quiet(dh) ? dh : 0;
}

USERAPI HWND CreateDialogIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp) { return create_dialog(inst, t, p, fn, lp, 1, 0); }
USERAPI HWND CreateDialogIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA t, HWND p, DLGPROC fn, LPARAM lp) { return create_dialog(inst, t, p, fn, lp, 0, 0); }
USERAPI HWND CreateDialogIndirectParamAorW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp, DWORD f) { return create_dialog(inst, t, p, fn, lp, !(f & 2), 0); }

USERAPI HWND CreateDialogParamW(HINSTANCE inst, LPCWSTR name, HWND p, DLGPROC fn, LPARAM lp)
{
    DWORD size;
    return create_dialog(inst, find_res(inst, name, RT_DIALOG, &size), p, fn, lp, 1, 0);
}

USERAPI HWND CreateDialogParamA(HINSTANCE inst, LPCSTR name, HWND p, DLGPROC fn, LPARAM lp)
{
    DWORD size;
    WCHAR *w = (ULONG_PTR)name < 0x10000 ? (WCHAR *)name : a2w(name, -1);
    const void *t = find_res(inst, w, RT_DIALOG, &size);
    if ((ULONG_PTR)name >= 0x10000) free(w);
    return create_dialog(inst, t, p, fn, lp, 0, 0);
}

/* -----------------------------------------------------------------------
 * Modal dialogs
 * ----------------------------------------------------------------------- */
static INT_PTR run_modal(HWND dh, HWND owner)
{
    Wnd *dw = W_quiet(dh);
    Dlg *d = dlg_of(dw);
    if (!d) return -1;
    Wnd *ow = owner ? W_quiet(owner) : NULL;
    int reenable = 0;
    if (ow && !(ow->style & WS_DISABLED)) { EnableWindow(owner, FALSE); reenable = 1; }
    if (W_quiet(dh) && !d->ended) {
        ShowWindow(dh, SW_SHOWNORMAL);
        UpdateWindow(dh);
    }
    MSG m;
    while (W_quiet(dh) && !d->ended) {
        if (!pump_one(&m, 0, 0, 0, PM_REMOVE, 1, INFINITE)) continue;
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        if (CallMsgFilterW(&m, MSGF_DIALOGBOX)) continue;
        if (!IsDialogMessageW(dh, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        dw = W_quiet(dh);
        d = dlg_of(dw);
        if (!d) break;
    }
    INT_PTR r = d ? d->result : -1;
    if (reenable && W_quiet(owner)) EnableWindow(owner, TRUE);
    if (W_quiet(dh)) DestroyWindow(dh);
    if (ow && W_quiet(owner) && (ow->style & WS_VISIBLE)) SetForegroundWindow(owner);
    return r;
}

static HWND modal_owner(HWND p)
{
    Wnd *w = p ? W_quiet(p) : NULL;
    if (!w) return 0;
    return top_of(w)->h;
}

USERAPI INT_PTR DialogBoxIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp)
{
    HWND owner = modal_owner(p);
    HWND dh = create_dialog(inst, t, owner, fn, lp, 1, 1);
    if (!dh) return -1;
    return run_modal(dh, owner);
}

USERAPI INT_PTR DialogBoxIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA t, HWND p, DLGPROC fn, LPARAM lp)
{
    HWND owner = modal_owner(p);
    HWND dh = create_dialog(inst, t, owner, fn, lp, 0, 1);
    if (!dh) return -1;
    return run_modal(dh, owner);
}

USERAPI INT_PTR DialogBoxIndirectParamAorW(HINSTANCE inst, LPCDLGTEMPLATEW t, HWND p, DLGPROC fn, LPARAM lp, DWORD f)
{
    return (f & 2) ? DialogBoxIndirectParamA(inst, t, p, fn, lp) : DialogBoxIndirectParamW(inst, t, p, fn, lp);
}

USERAPI INT_PTR DialogBoxParamW(HINSTANCE inst, LPCWSTR name, HWND p, DLGPROC fn, LPARAM lp)
{
    DWORD size;
    const void *t = find_res(inst, name, RT_DIALOG, &size);
    if (!t) { SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return -1; }
    return DialogBoxIndirectParamW(inst, t, p, fn, lp);
}

USERAPI INT_PTR DialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND p, DLGPROC fn, LPARAM lp)
{
    DWORD size;
    WCHAR *w = (ULONG_PTR)name < 0x10000 ? (WCHAR *)name : a2w(name, -1);
    const void *t = find_res(inst, w, RT_DIALOG, &size);
    if ((ULONG_PTR)name >= 0x10000) free(w);
    if (!t) { SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return -1; }
    return DialogBoxIndirectParamA(inst, t, p, fn, lp);
}

USERAPI BOOL EndDialog(HWND h, INT_PTR r)
{
    Wnd *w = W(h);
    Dlg *d = dlg_of(w);
    if (!d) return FALSE;
    d->ended = 1;
    d->result = r;
    /* the owner is usable again before the dialog goes away */
    Wnd *o = w->owner;
    if (o && (o->style & WS_DISABLED)) EnableWindow(o->h, TRUE);
    ShowWindow(h, SW_HIDE);
    if (o && W_quiet(o->h) && (o->style & WS_VISIBLE)) SetForegroundWindow(o->h);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Tab and group navigation
 * ----------------------------------------------------------------------- */
static int usable_ctl(Wnd *c) { return (c->style & WS_VISIBLE) && !(c->style & WS_DISABLED); }

/* Controls in tab order, descending into WS_EX_CONTROLPARENT children */
static void collect(Wnd *p, HWND *out, int *n, int max)
{
    for (Wnd *c = p->child; c && *n < max; c = c->next) {
        if (!(c->style & WS_VISIBLE)) continue;
        if (c->exstyle & WS_EX_CONTROLPARENT) {
            if (!(c->style & WS_DISABLED)) collect(c, out, n, max);
            continue;
        }
        out[(*n)++] = c->h;
    }
}

USERAPI HWND GetNextDlgTabItem(HWND hd, HWND hc, BOOL prev)
{
    Wnd *d = W(hd);
    if (!d) return 0;
    HWND list[512];
    int n = 0;
    collect(d, list, &n, 512);
    if (!n) return 0;
    int at = -1;
    for (int i = 0; i < n; i++) if (list[i] == hc) { at = i; break; }
    if (at < 0 && hc) {
        /* the control is inside one of the listed ones (a combo's edit) */
        Wnd *c = W_quiet(hc);
        for (int i = 0; i < n && c; i++) if (is_child_of(W_quiet(list[i]), c)) { at = i; break; }
    }
    for (int k = 1; k <= n; k++) {
        int i = at < 0 ? (prev ? n - k : k - 1) : (at + (prev ? -k : k) + n * 2) % n;
        Wnd *c = W_quiet(list[i]);
        if (c && usable_ctl(c) && (c->style & WS_TABSTOP)) return list[i];
    }
    return hc;
}

USERAPI HWND GetNextDlgGroupItem(HWND hd, HWND hc, BOOL prev)
{
    Wnd *d = W(hd);
    Wnd *c = W_quiet(hc);
    if (!d) return 0;
    if (!c || !c->parent) return GetNextDlgTabItem(hd, hc, prev);
    /* the group: from the WS_GROUP control at or before it to the next one */
    Wnd *start = c;
    while (!(start->style & WS_GROUP) && start->prev) start = start->prev;
    Wnd *items[256];
    int n = 0, at = 0;
    for (Wnd *s = start; s && n < 256; s = s->next) {
        if (s != start && (s->style & WS_GROUP)) break;
        if (s == c) at = n;
        items[n++] = s;
    }
    for (int k = 1; k < n; k++) {
        Wnd *s = items[(at + (prev ? -k : k) + n * 2) % n];
        if (usable_ctl(s)) return s->h;
    }
    return hc;
}

static HWND first_tab(HWND dlg)
{
    Wnd *d = W_quiet(dlg);
    if (!d) return 0;
    HWND list[512];
    int n = 0;
    collect(d, list, &n, 512);
    for (int i = 0; i < n; i++) {
        Wnd *c = W_quiet(list[i]);
        if (c && usable_ctl(c) && (c->style & WS_TABSTOP)) return list[i];
    }
    return 0;
}

static int is_button(Wnd *c) { return c && c->cls && !wcsicmp_(c->cls->name, L"Button"); }

/* Focus a control from the keyboard: edit boxes get their text selected;
 * a push button with the focus is the default one */
static void dlg_focus(Wnd *dw, HWND c)
{
    if (!c) return;
    LRESULT code = SendMessageW(c, WM_GETDLGCODE, 0, 0);
    set_focus(c);
    if (code & DLGC_HASSETSEL) SendMessageW(c, EM_SETSEL, 0, -1);
    Dlg *d = dlg_of(dw);
    if (!d) return;
    /* move the default look */
    for (Wnd *k = dw->child; k; k = k->next) {
        if (!is_button(k)) continue;
        DWORD t = k->style & BS_TYPEMASK;
        int want = (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) ? k->h == c : (WORD)k->id == d->defid;
        if (t == BS_DEFPUSHBUTTON && !want) SendMessageW(k->h, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        else if (t == BS_PUSHBUTTON && want) SendMessageW(k->h, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
    }
}

/* The control whose label has the mnemonic @c */
static HWND find_mnemonic(Wnd *d, WCHAR c)
{
    WCHAR up = (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)c);
    HWND list[512];
    int n = 0;
    collect(d, list, &n, 512);
    for (int i = 0; i < n; i++) {
        Wnd *w = W_quiet(list[i]);
        if (!w || !w->text || (w->style & WS_DISABLED)) continue;
        if (w->cls && !wcsicmp_(w->cls->name, L"Static") && (w->style & SS_NOPREFIX)) continue;
        if (w->cls && (!wcsicmp_(w->cls->name, L"Edit") || !wcsicmp_(w->cls->name, L"ComboBox") || !wcsicmp_(w->cls->name, L"ListBox"))) continue;
        for (const WCHAR *p = w->text; *p; p++) {
            if (*p == '&' && p[1] == '&') { p++; continue; }
            if (*p == '&' && p[1]) {
                if ((WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)p[1]) == up) return list[i];
                break;
            }
        }
    }
    return 0;
}

static int do_mnemonic(Wnd *dw, WCHAR c)
{
    HWND t = find_mnemonic(dw, c);
    if (!t) return 0;
    Wnd *tw = W_quiet(t);
    LRESULT code = SendMessageW(t, WM_GETDLGCODE, 0, 0);
    if (code & DLGC_STATIC) {
        /* a label: the next control that can take the focus */
        for (Wnd *n = tw->next; n; n = n->next)
            if (usable_ctl(n) && !(SendMessageW(n->h, WM_GETDLGCODE, 0, 0) & DLGC_STATIC)) { dlg_focus(dw, n->h); return 1; }
        return 1;
    }
    if (code & DLGC_BUTTON) {
        DWORD type = tw->style & BS_TYPEMASK;
        if (type != BS_GROUPBOX) {
            if (!(code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))) dlg_focus(dw, t);
            SendMessageW(t, BM_CLICK, 0, 0);
        }
        return 1;
    }
    dlg_focus(dw, t);
    return 1;
}

static void press_default(Wnd *dw, HWND focus)
{
    HWND dh = dw->h;
    Wnd *f = W_quiet(focus);
    if (f && is_button(f) && (SendMessageW(focus, WM_GETDLGCODE, 0, 0) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) && usable_ctl(f)) {
        SendMessageW(dh, WM_COMMAND, MAKEWPARAM((WORD)f->id, BN_CLICKED), (LPARAM)focus);
        return;
    }
    LRESULT def = SendMessageW(dh, DM_GETDEFID, 0, 0);
    if (HIWORD(def) == DC_HASDEFID) {
        HWND b = GetDlgItem(dh, LOWORD(def));
        Wnd *bw = W_quiet(b);
        if (bw && (bw->style & WS_DISABLED)) { MessageBeep(0); return; }
        SendMessageW(dh, WM_COMMAND, MAKEWPARAM(LOWORD(def), BN_CLICKED), (LPARAM)b);
    } else {
        SendMessageW(dh, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(dh, IDOK));
    }
}

BOOL dlg_nav(HWND dlg, MSG *m) { return IsDialogMessageW(dlg, m); }

USERAPI BOOL IsDialogMessageW(HWND hd, LPMSG m)
{
    Wnd *dw = W_quiet(hd);
    if (!dw || !m || !m->hwnd) return FALSE;
    if (m->hwnd != hd && !is_child_of(dw, W_quiet(m->hwnd))) return FALSE;
    if (CallMsgFilterW(m, MSGF_DIALOGBOX)) return TRUE;
    HWND focus = GetFocus();
    LRESULT code = SendMessageW(m->hwnd, WM_GETDLGCODE, m->wParam, (LPARAM)m);
    if (!W_quiet(hd)) return TRUE;
    if (code & DLGC_WANTMESSAGE) { TranslateMessage(m); DispatchMessageW(m); return TRUE; }
    int shift = GetKeyState(VK_SHIFT) < 0;
    switch (m->message) {
    case WM_KEYDOWN:
        switch (m->wParam) {
        case VK_TAB:
            if (code & DLGC_WANTTAB) break;
            if (GetKeyState(VK_CONTROL) < 0) break;
            dlg_focus(dw, GetNextDlgTabItem(hd, focus, shift));
            return TRUE;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            if (code & DLGC_WANTARROWS) break;
            {
                HWND n = GetNextDlgGroupItem(hd, focus, m->wParam == VK_LEFT || m->wParam == VK_UP);
                if (n && n != focus) {
                    dlg_focus(dw, n);
                    LRESULT nc = SendMessageW(n, WM_GETDLGCODE, 0, 0);
                    Wnd *nw = W_quiet(n);
                    if ((nc & DLGC_RADIOBUTTON) && nw && (nw->style & BS_TYPEMASK) == BS_AUTORADIOBUTTON) SendMessageW(n, BM_CLICK, 0, 0);
                }
            }
            return TRUE;
        case VK_ESCAPE: case VK_CANCEL: {
            HWND b = GetDlgItem(hd, IDCANCEL);
            Wnd *bw = W_quiet(b);
            if (bw && (bw->style & WS_DISABLED)) return TRUE;
            SendMessageW(hd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)b);
            return TRUE;
        }
        case VK_RETURN: case VK_EXECUTE:
            if (code & DLGC_WANTALLKEYS) break;
            press_default(dw, focus);
            return TRUE;
        }
        break;
    case WM_CHAR:
        if (m->wParam == '\t' && !(code & DLGC_WANTTAB)) return TRUE;
        if ((m->wParam == '\r' || m->wParam == 0x1B) && !(code & DLGC_WANTALLKEYS)) return TRUE;
        if (code & (DLGC_WANTCHARS | DLGC_WANTALLKEYS)) break;
        if (m->wParam > ' ' && do_mnemonic(dw, (WCHAR)m->wParam)) return TRUE;
        break;
    case WM_SYSCHAR:
        if (do_mnemonic(dw, (WCHAR)m->wParam)) return TRUE;
        break;
    }
    TranslateMessage(m);
    DispatchMessageW(m);
    return TRUE;
}

USERAPI BOOL IsDialogMessageA(HWND hd, LPMSG m) { return IsDialogMessageW(hd, m); }

/* -----------------------------------------------------------------------
 * DefDlgProc
 * ----------------------------------------------------------------------- */
static int returns_directly(UINT msg)
{
    switch (msg) {
    case WM_COMPAREITEM: case WM_VKEYTOITEM: case WM_CHARTOITEM: case WM_QUERYDRAGICON: case WM_INITDIALOG:
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: case WM_QUERYNEWPALETTE:
        return 1;
    }
    return 0;
}

static WORD find_def_button(Wnd *dw)
{
    for (Wnd *c = dw->child; c; c = c->next)
        if (is_button(c) && (c->style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) return (WORD)c->id;
    return 0;
}

static LRESULT def_dlg(Wnd *w, HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide)
{
    Dlg *d = dlg_of(w);
    DLGPROC proc = dlg_proc(w);
    if (proc) {
        set_msgresult(w, 0);
        int pw = d ? d->proc_wide : wide;
        INT_PTR r = (INT_PTR)call_proc(w, (WNDPROC)proc, pw, h, msg, wp, lp, wide);
        if (!W_quiet(h)) return r;
        if (r) return returns_directly(msg) ? r : get_msgresult(w);
        if (returns_directly(msg) && msg != WM_INITDIALOG && msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) {}
    }
    d = dlg_of(w);
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
        HBRUSH b = (HBRUSH)send_msg(w, WM_CTLCOLORDLG, wp, (LPARAM)h);
        if (!b) b = sys_brush(COLOR_3DFACE);
        FillRect((HDC)wp, &r, b);
        return 1;
    }
    case WM_SHOWWINDOW:
        if (wp) {
            Wnd *f = W_quiet(g_focus);
            if (!f || !is_child_of(w, f)) {
                HWND t = d && d->focus && W_quiet(d->focus) ? d->focus : first_tab(h);
                if (t && (w->flags & WF_DIALOG) && !(w->style & WS_CHILD)) {}
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            Wnd *f = W_quiet(g_focus);
            if (!f || !is_child_of(w, f)) {
                HWND t = d && d->focus && W_quiet(d->focus) && is_child_of(w, W_quiet(d->focus)) ? d->focus : first_tab(h);
                if (t) dlg_focus(w, t);
                else set_focus(h);
            }
        } else if (d) {
            Wnd *f = W_quiet(g_focus);
            if (f && is_child_of(w, f)) d->focus = g_focus;
        }
        return 0;
    case WM_SETFOCUS: {
        HWND t = d && d->focus && W_quiet(d->focus) ? d->focus : first_tab(h);
        if (t && t != h) dlg_focus(w, t);
        return 0;
    }
    case WM_CLOSE: {
        HWND b = GetDlgItem(h, IDCANCEL);
        Wnd *bw = W_quiet(b);
        if (bw && (bw->style & WS_DISABLED)) { MessageBeep(0); return 0; }
        PostMessageW(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)b);
        return 0;
    }
    case DM_GETDEFID: {
        WORD id = d && d->defid ? d->defid : find_def_button(w);
        return id ? MAKELONG(id, DC_HASDEFID) : 0;
    }
    case DM_SETDEFID: {
        if (!d) return FALSE;
        WORD old = d->defid ? d->defid : find_def_button(w);
        d->defid = (WORD)wp;
        Wnd *ob = W_quiet(GetDlgItem(h, old)), *nb = W_quiet(GetDlgItem(h, (int)wp));
        if (ob && is_button(ob) && (ob->style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) SendMessageW(ob->h, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        if (nb && is_button(nb) && (nb->style & BS_TYPEMASK) == BS_PUSHBUTTON) SendMessageW(nb->h, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        return TRUE;
    }
    case DM_REPOSITION: {
        RECT r;
        GetWindowRect(h, &r);
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int x = r.left, y = r.top;
        if (r.right > sw) x = sw - (r.right - r.left);
        if (r.bottom > sh) y = sh - (r.bottom - r.top);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        SetWindowPos(h, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NEXTDLGCTL:
        if (LOWORD(lp)) dlg_focus(w, (HWND)wp);
        else dlg_focus(w, GetNextDlgTabItem(h, GetFocus(), wp != 0));
        return 0;
    case WM_GETFONT: return (LRESULT)(d ? d->font : 0);
    case WM_NCDESTROY:
        if (d) {
            if (d->own_font && d->font) DeleteObject(d->font);
            free(d);
            w->ctl = NULL;
            w->flags &= ~WF_DIALOG;
        }
        return 0;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        return DefWindowProcW(h, msg, wp, lp);
    }
    return wide ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
}

USERAPI LRESULT CALLBACK DefDlgProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    return def_dlg(w, h, msg, wp, lp, 1);
}

USERAPI LRESULT DefDlgProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    return def_dlg(w, h, msg, wp, lp, 0);
}

/* -----------------------------------------------------------------------
 * Dialog items
 * ----------------------------------------------------------------------- */
USERAPI HWND GetDlgItem(HWND h, int id)
{
    Wnd *w = W(h);
    if (!w) return 0;
    for (Wnd *c = w->child; c; c = c->next) if ((int)(WORD)c->id == (WORD)id || c->id == id) return c->h;
    SetLastError(ERROR_CONTROL_ID_NOT_FOUND);
    return 0;
}

USERAPI LRESULT SendDlgItemMessageW(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND c = GetDlgItem(h, id);
    return c ? SendMessageW(c, msg, wp, lp) : 0;
}

USERAPI LRESULT SendDlgItemMessageA(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND c = GetDlgItem(h, id);
    return c ? SendMessageA(c, msg, wp, lp) : 0;
}

USERAPI BOOL SetDlgItemTextW(HWND h, int id, LPCWSTR s) { HWND c = GetDlgItem(h, id); return c ? SetWindowTextW(c, s) : FALSE; }
USERAPI BOOL SetDlgItemTextA(HWND h, int id, LPCSTR s) { HWND c = GetDlgItem(h, id); return c ? SetWindowTextA(c, s) : FALSE; }
USERAPI UINT GetDlgItemTextW(HWND h, int id, LPWSTR s, int n) { HWND c = GetDlgItem(h, id); if (!c) { if (s && n) s[0] = 0; return 0; } return (UINT)GetWindowTextW(c, s, n); }
USERAPI UINT GetDlgItemTextA(HWND h, int id, LPSTR s, int n) { HWND c = GetDlgItem(h, id); if (!c) { if (s && n) s[0] = 0; return 0; } return (UINT)GetWindowTextA(c, s, n); }

USERAPI BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sign)
{
    WCHAR buf[16], tmp[16];
    int n = 0, neg = sign && (int)v < 0;
    unsigned u = neg ? (unsigned)-(int)v : v;
    do { tmp[n++] = (WCHAR)('0' + u % 10); u /= 10; } while (u);
    int k = 0;
    if (neg) buf[k++] = '-';
    while (n) buf[k++] = tmp[--n];
    buf[k] = 0;
    return SetDlgItemTextW(h, id, buf);
}

USERAPI UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sign)
{
    WCHAR buf[32];
    if (ok) *ok = FALSE;
    if (!GetDlgItemTextW(h, id, buf, 32)) return 0;
    const WCHAR *p = buf;
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-' && sign) { neg = 1; p++; }
    else if (*p == '+') p++;
    if (*p < '0' || *p > '9') return 0;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); if (v > 0xFFFFFFFFull) return 0; p++; }
    while (*p == ' ') p++;
    if (*p) return 0;
    if (sign && ((neg && v > 0x80000000ull) || (!neg && v > 0x7FFFFFFF))) return 0;
    if (ok) *ok = TRUE;
    return neg ? (UINT)-(int)v : (UINT)v;
}

/* -----------------------------------------------------------------------
 * Message boxes: a dialog made on the fly
 * ----------------------------------------------------------------------- */
typedef struct { int id; const WCHAR *text; } MbButton;

static INT_PTR CALLBACK mb_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: return FALSE;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            int id = LOWORD(wp);
            if (id == IDCANCEL && !GetDlgItem(h, IDCANCEL)) {
                /* no Cancel button: Esc and the close button only work with a single OK */
                if (GetDlgItem(h, IDOK) && !GetDlgItem(h, IDYES) && !GetDlgItem(h, IDNO)) id = IDOK;
                else return TRUE;
            }
            EndDialog(h, id);
        }
        return TRUE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORDLG: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, 0xFFFFFF);
        SetTextColor(dc, 0x000000);
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        HDC dc = (HDC)wp;
        fill_rect(dc, &r, 0xFFFFFF);
        int fh = (int)(INT_PTR)GetPropW(h, L"NovaMbFooter");
        RECT f = { 0, r.bottom - fh, r.right, r.bottom };
        fill_rect(dc, &f, 0xF0F0F0);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, 1);
        return TRUE;
    }
    }
    return FALSE;
}

static int message_box(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang)
{
    (void)lang;
    static const MbButton sets[7][3] = {
        { { IDOK, L"OK" } },
        { { IDOK, L"OK" }, { IDCANCEL, L"Cancel" } },
        { { IDABORT, L"&Abort" }, { IDRETRY, L"&Retry" }, { IDIGNORE, L"&Ignore" } },
        { { IDYES, L"&Yes" }, { IDNO, L"&No" }, { IDCANCEL, L"Cancel" } },
        { { IDYES, L"&Yes" }, { IDNO, L"&No" } },
        { { IDRETRY, L"&Retry" }, { IDCANCEL, L"Cancel" } },
        { { IDCANCEL, L"Cancel" }, { IDTRYAGAIN, L"&Try Again" }, { IDCONTINUE, L"&Continue" } },
    };
    static const int counts[7] = { 1, 2, 3, 3, 2, 2, 3 };
    UINT kind = type & MB_TYPEMASK;
    if (kind > 6) kind = 0;
    int nb = counts[kind];
    MbButton btn[4];
    for (int i = 0; i < nb; i++) btn[i] = sets[kind][i];
    if (type & MB_HELP) { btn[nb].id = IDHELP; btn[nb].text = L"Help"; nb++; }
    int defbtn = (int)((type & MB_DEFMASK) >> 8);
    if (defbtn >= nb) defbtn = 0;
    if (!text) text = L"";
    if (!caption) caption = L"Error";
    HICON icon = 0;
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: icon = LoadIconW(NULL, IDI_ERROR); break;
    case MB_ICONQUESTION: icon = LoadIconW(NULL, IDI_QUESTION); break;
    case MB_ICONEXCLAMATION: icon = LoadIconW(NULL, IDI_WARNING); break;
    case MB_ICONASTERISK: icon = LoadIconW(NULL, IDI_INFORMATION); break;
    }
    /* measure */
    HFONT font = gui_font();
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, font);
    int maxw = GetSystemMetrics(SM_CXSCREEN) * 2 / 5;
    if (maxw < 280) maxw = 280;
    RECT tr = { 0, 0, maxw, 0 };
    DrawTextW(dc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    RECT cr = { 0, 0, 0, 0 };
    DrawTextW(dc, caption, -1, &cr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int bw = 0;
    for (int i = 0; i < nb; i++) { RECT b = { 0, 0, 0, 0 }; DrawTextW(dc, btn[i].text, -1, &b, DT_CALCRECT | DT_SINGLELINE); if (b.right > bw) bw = b.right; }
    SelectObject(dc, of);
    bw = MAX(bw + 24, 88);
    int bh = 26, gap = 10;
    int icon_w = icon ? 32 + 12 : 0;
    int tw = tr.right, th = tr.bottom;
    int content_w = MAX(icon_w + tw, nb * bw + (nb - 1) * gap);
    content_w = MAX(content_w, cr.right + 60);
    int margin = 24;
    int footer = bh + 2 * 12;
    int body_h = MAX(th, icon ? 32 : 0);
    int cw = content_w + 2 * margin, ch = margin + body_h + margin + footer;
    RECT wr = { 0, 0, cw, ch };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_DLGMODALFRAME);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    HWND ow = modal_owner(owner);
    RECT center = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    Wnd *o = ow ? W_quiet(ow) : NULL;
    if (o && (o->style & WS_VISIBLE) && !o->minimized) GetWindowRect(ow, &center);
    int x = (center.left + center.right - ww) / 2, y = (center.top + center.bottom - wh) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND dh = CreateWindowExW(WS_EX_DLGMODALFRAME | ((type & MB_TOPMOST) ? WS_EX_TOPMOST : 0), L"#32770", caption, style,
                              x, y, ww, wh, ow, 0, 0, NULL);
    Wnd *dw = W_quiet(dh);
    if (!dw) return 0;
    Dlg *d = calloc(1, sizeof(Dlg));
    if (dw->ctl && (dw->flags & WF_DIALOG)) free(dw->ctl);
    dw->ctl = d;
    dw->flags |= WF_DIALOG;
    d->msgbox = 1;
    d->proc_wide = 1;
    d->font = font;
    font_base_units(font, &d->xb, &d->yb);
    if (!dw->extra || dw->cls->extra < DLGWINDOWEXTRA) { free(dw->extra); dw->extra = calloc(1, DLGWINDOWEXTRA + 8); }
    DLGPROC p = mb_proc;
    memcpy(dw->extra + DWLP_DLGPROC, &p, sizeof(p));
    SetPropW(dh, L"NovaMbFooter", (HANDLE)(INT_PTR)footer);
    if (icon) {
        HWND ic = CreateWindowExW(0, L"Static", NULL, WS_CHILD | WS_VISIBLE | SS_ICON, margin, margin, 32, 32, dh, (HMENU)20, 0, NULL);
        SendMessageW(ic, STM_SETICON, (WPARAM)icon, 0);
    }
    int ty = margin + (body_h - th) / 2;
    HWND st = CreateWindowExW(0, L"Static", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
                              margin + icon_w, ty, tw + 4, th, dh, (HMENU)0xFFFF, 0, NULL);
    SendMessageW(st, WM_SETFONT, (WPARAM)font, 0);
    int bx = cw - margin - nb * bw - (nb - 1) * gap, by = ch - footer + 12;
    HWND focus = 0;
    for (int i = 0; i < nb; i++) {
        HWND b = CreateWindowExW(0, L"Button", btn[i].text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == defbtn ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | (i == 0 ? WS_GROUP : 0),
                                 bx + i * (bw + gap), by, bw, bh, dh, (HMENU)(INT_PTR)btn[i].id, 0, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, 0);
        if (i == defbtn) { focus = b; d->defid = (WORD)btn[i].id; }
    }
    if (!(type & MB_SYSTEMMODAL) && (kind == 4 || kind == 2)) {
        /* no Cancel: the close button does nothing */
    }
    set_focus(focus);
    d->focus = focus;
    MessageBeep(type & MB_ICONMASK);
    return (int)run_modal(dh, ow);
}

USERAPI int MessageBoxExW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang) { return message_box(h, text, caption, type, lang); }
USERAPI int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type) { return message_box(h, text, caption, type, 0); }

USERAPI int MessageBoxExA(HWND h, LPCSTR text, LPCSTR caption, UINT type, WORD lang)
{
    WCHAR *t = a2w(text ? text : "", -1), *c = caption ? a2w(caption, -1) : NULL;
    int r = message_box(h, t, c, type, lang);
    free(t); free(c);
    return r;
}

USERAPI int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type) { return MessageBoxExA(h, text, caption, type, 0); }

USERAPI int MessageBoxIndirectW(const MSGBOXPARAMSW *p)
{
    if (!p) return 0;
    WCHAR tbuf[1024], cbuf[256];
    LPCWSTR t = p->lpszText, c = p->lpszCaption;
    if (t && (ULONG_PTR)t < 0x10000) { LoadStringW(p->hInstance, (UINT)(ULONG_PTR)t, tbuf, 1024); t = tbuf; }
    if (c && (ULONG_PTR)c < 0x10000) { LoadStringW(p->hInstance, (UINT)(ULONG_PTR)c, cbuf, 256); c = cbuf; }
    return message_box(p->hwndOwner, t, c, p->dwStyle, (WORD)p->dwLanguageId);
}

USERAPI int MessageBoxIndirectA(const void *pa)
{
    const BYTE *b = pa;                                     /* the same layout with ANSI strings */
    HWND owner = *(HWND *)(b + 8);
    LPCSTR text = *(LPCSTR *)(b + 24), caption = *(LPCSTR *)(b + 32);
    DWORD style = *(DWORD *)(b + 40);
    return MessageBoxA(owner, (ULONG_PTR)text < 0x10000 ? "" : text, (ULONG_PTR)caption < 0x10000 ? NULL : caption, style);
}

USERAPI int MessageBoxTimeoutW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang, DWORD ms) { (void)ms; return message_box(h, text, caption, type, lang); }
USERAPI int MessageBoxTimeoutA(HWND h, LPCSTR text, LPCSTR caption, UINT type, WORD lang, DWORD ms) { (void)ms; return MessageBoxExA(h, text, caption, type, lang); }

USERAPI BOOL MessageBeep(UINT type) { (void)type; return TRUE; }
