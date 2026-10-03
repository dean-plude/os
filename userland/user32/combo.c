/*
 * combo.c — the COMBOBOX class: drop-down lists, drop-down combos (an
 * edit box and a list) and simple combos.  The list drops down in a popup
 * window of the ComboLBox class that holds the mouse while it is open.
 */
#include "u32.h"

#define CBI_CANCEL (WM_USER + 0x7F01)
#define CBI_CHOOSE (WM_USER + 0x7F02)

typedef struct {
    HWND edit, list;
    int  dropped, hot, pressed, focus;
    int  field_h;                   /* the selection field's height */
    int  drop_h;                    /* the list's height when dropped (from the creation height) */
    int  drop_w;
    int  min_visible;
    int  ext_ui;
    int  sel_on_drop;
    int  in_notify;
} Cb;

#define CBTYPE(w) ((w)->style & 3)
static Cb *cb_of(Wnd *w) { return w ? (Cb *)w->ctl : NULL; }

int cb_is_string_msg(Wnd *w, UINT msg)
{
    (void)msg;
    if (!w || !w->cls || wcsicmp_(w->cls->name, L"ComboBox")) return 1;
    return !(w->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (w->style & CBS_HASSTRINGS);
}

static HFONT cfont(Wnd *w) { return ctl_font(w); }
static int btn_w(Wnd *w) { return sb_width_k(dpi_k(w)); }   /* (at the window's DPI) */

static void cnotify(Wnd *w, UINT code) { if (w->parent) notify_parent(w, code); }

static LRESULT lb(Cb *c, UINT msg, WPARAM wp, LPARAM lp) { return c->list ? SendMessageW(c->list, msg, wp, lp) : 0; }

static void field_rect(Wnd *w, Cb *c, RECT *r)
{
    SetRect(r, 0, 0, w->client.right - w->client.left, CBTYPE(w) == CBS_SIMPLE ? c->field_h : w->client.bottom - w->client.top);
}

static void button_rect(Wnd *w, Cb *c, RECT *r)
{
    field_rect(w, c, r);
    r->left = r->right - btn_w(w);
}

/* The current item's text into the edit box */
static void sync_edit(Wnd *w, Cb *c)
{
    if (!c->edit) return;
    int i = (int)lb(c, LB_GETCURSEL, 0, 0);
    WCHAR buf[1024];
    buf[0] = 0;
    if (i >= 0 && lb(c, LB_GETTEXTLEN, (WPARAM)i, 0) < 1024) lb(c, LB_GETTEXT, (WPARAM)i, (LPARAM)buf);
    c->in_notify++;
    SetWindowTextW(c->edit, buf);
    c->in_notify--;
    SendMessageW(c->edit, EM_SETSEL, 0, -1);
    (void)w;
}

static void redraw(Wnd *w) { invalidate(w, NULL, FALSE, 0); }

static void close_list(Wnd *w, Cb *c, int ok)
{
    if (!c->dropped) return;
    HWND h = w->h;
    c->dropped = 0;
    c->pressed = 0;
    if (GetCapture() == c->list) ReleaseCapture();
    ShowWindow(c->list, SW_HIDE);
    if (!W_quiet(h)) return;
    if (ok) {
        int i = (int)lb(c, LB_GETCURSEL, 0, 0);
        cnotify(w, CBN_SELENDOK);
        if (!W_quiet(h)) return;
        if (i != c->sel_on_drop) {
            sync_edit(w, c);
            cnotify(w, CBN_SELCHANGE);
            if (!W_quiet(h)) return;
        }
    } else {
        lb(c, LB_SETCURSEL, (WPARAM)c->sel_on_drop, 0);
        cnotify(w, CBN_SELENDCANCEL);
        if (!W_quiet(h)) return;
    }
    cnotify(w, CBN_CLOSEUP);
    if (W_quiet(h)) redraw(w);
}

static void drop(Wnd *w, Cb *c)
{
    if (c->dropped || CBTYPE(w) == CBS_SIMPLE || !c->list) return;
    HWND h = w->h;
    cnotify(w, CBN_DROPDOWN);
    if (!W_quiet(h)) return;
    c->sel_on_drop = (int)lb(c, LB_GETCURSEL, 0, 0);
    int n = (int)lb(c, LB_GETCOUNT, 0, 0);
    int ih = (int)lb(c, LB_GETITEMHEIGHT, 0, 0);
    if (ih <= 0) ih = 16;
    int vis = n < 1 ? 1 : n;
    int maxvis = c->min_visible > 0 ? c->min_visible : 30;
    int h_items = MIN(vis, maxvis) * ih + 2;
    int lh = h_items;
    if (c->drop_h > ih && c->min_visible <= 0 && !(w->style & CBS_NOINTEGRALHEIGHT)) lh = MIN(h_items, MAX(c->drop_h, ih * 3 + 2));
    if (c->drop_h > ih && c->min_visible <= 0) lh = MIN(h_items, MAX(c->drop_h, ih + 2));
    if (lh < ih + 2) lh = ih + 2;
    RECT wr;
    GetWindowRect(h, &wr);
    int lw = MAX(c->drop_w, wr.right - wr.left);
    int y = wr.bottom;
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (y + lh > sh - 48 && wr.top - lh >= 0) y = wr.top - lh;
    Wnd *lw_ = W_quiet(c->list);
    if (lw_) {
        if (n > MIN(vis, maxvis) || lh < h_items) lw_->style |= WS_VSCROLL; else lw_->style &= ~WS_VSCROLL;
    }
    SetWindowPos(c->list, HWND_TOPMOST, wr.left, y, lw, lh, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (c->sel_on_drop >= 0) lb(c, LB_SETTOPINDEX, (WPARAM)c->sel_on_drop, 0);
    c->dropped = 1;
    ShowWindow(c->list, SW_SHOWNA);
    UpdateWindow(c->list);
    SetCapture(c->list);
    redraw(w);
}

static void select_index(Wnd *w, Cb *c, int i, int notify)
{
    int cur = (int)lb(c, LB_GETCURSEL, 0, 0);
    int n = (int)lb(c, LB_GETCOUNT, 0, 0);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (i < 0 || i == cur) return;
    lb(c, LB_SETCURSEL, (WPARAM)i, 0);
    sync_edit(w, c);
    redraw(w);
    if (notify) { HWND h = w->h; cnotify(w, CBN_SELCHANGE); if (W_quiet(h) && !c->dropped) cnotify(w, CBN_SELENDOK); }
}

static void key(Wnd *w, Cb *c, WPARAM vk)
{
    int alt = GetKeyState(VK_MENU) < 0;
    if (vk == VK_F4 || ((vk == VK_DOWN || vk == VK_UP) && alt)) {
        if (c->dropped) close_list(w, c, 1); else drop(w, c);
        return;
    }
    if (c->dropped) {
        switch (vk) {
        case VK_RETURN: close_list(w, c, 1); return;
        case VK_ESCAPE: close_list(w, c, 0); return;
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END: {
            int i = (int)lb(c, LB_GETCURSEL, 0, 0), n = (int)lb(c, LB_GETCOUNT, 0, 0);
            int page = 8;
            switch (vk) {
            case VK_UP: i--; break; case VK_DOWN: i++; break; case VK_PRIOR: i -= page; break;
            case VK_NEXT: i += page; break; case VK_HOME: i = 0; break; case VK_END: i = n - 1; break;
            }
            if (i < 0) i = 0;
            if (i >= n) i = n - 1;
            lb(c, LB_SETCURSEL, (WPARAM)i, 0);
            sync_edit(w, c);
            return;
        }
        }
        return;
    }
    if (c->ext_ui && vk == VK_DOWN) { drop(w, c); return; }
    int i = (int)lb(c, LB_GETCURSEL, 0, 0);
    int n = (int)lb(c, LB_GETCOUNT, 0, 0);
    switch (vk) {
    case VK_UP: case VK_LEFT: select_index(w, c, i < 0 ? 0 : i - 1, 1); break;
    case VK_DOWN: case VK_RIGHT: select_index(w, c, i < 0 ? 0 : i + 1, 1); break;
    case VK_PRIOR: select_index(w, c, i - 8, 1); break;
    case VK_NEXT: select_index(w, c, i + 8, 1); break;
    case VK_HOME: select_index(w, c, 0, 1); break;
    case VK_END: select_index(w, c, n - 1, 1); break;
    }
}

static void paint(Wnd *w, Cb *c, HDC dc)
{
    RECT f;
    field_rect(w, c, &f);
    int disabled = (w->style & WS_DISABLED) != 0, k = dpi_k(w);
    HGDIOBJ of = SelectObject(dc, cfont(w));
    SetBkMode(dc, TRANSPARENT);
    if (CBTYPE(w) == CBS_DROPDOWNLIST) {
        COLORREF fill = 0xE1E1E1, border = 0xADADAD;
        if (disabled) { fill = 0xCCCCCC; border = 0xBFBFBF; }
        else if (c->pressed || c->dropped) { fill = 0xF7E4CC; border = 0x995400; }
        else if (c->hot) { fill = 0xFBF1E5; border = 0xD77800; }
        else if (c->focus) border = 0xD77800;
        fill_rect(dc, &f, fill);
        RECT fr = f;
        for (int n = 0; n < k; n++) { frame_rect(dc, &fr, border); InflateRect(&fr, -1, -1); }
        RECT tr = f;
        tr.left += 3 * k; tr.top += 2 * k; tr.bottom -= 2 * k; tr.right -= btn_w(w);
        int i = (int)lb(c, LB_GETCURSEL, 0, 0);
        if (w->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
            DRAWITEMSTRUCT di;
            memset(&di, 0, sizeof(di));
            di.CtlType = ODT_COMBOBOX; di.CtlID = (UINT)w->id; di.itemID = (UINT)i; di.itemAction = ODA_DRAWENTIRE;
            di.itemState = ODS_COMBOBOXEDIT | (c->focus && !c->dropped ? ODS_SELECTED | ODS_FOCUS : 0) | (disabled ? ODS_DISABLED : 0);
            di.hwndItem = w->h; di.hDC = dc; di.rcItem = tr;
            di.itemData = i >= 0 ? (ULONG_PTR)lb(c, LB_GETITEMDATA, (WPARAM)i, 0) : 0;
            if (w->parent) send_msg(w->parent, WM_DRAWITEM, (WPARAM)w->id, (LPARAM)&di);
        } else {
            if (c->focus && !c->dropped && !disabled) {
                RECT hr = tr;
                fill_rect(dc, &hr, sys_color(COLOR_HIGHLIGHT));
                SetTextColor(dc, sys_color(COLOR_HIGHLIGHTTEXT));
            } else SetTextColor(dc, disabled ? sys_color(COLOR_GRAYTEXT) : sys_color(COLOR_BTNTEXT));
            if (i >= 0) {
                WCHAR buf[512];
                buf[0] = 0;
                if (lb(c, LB_GETTEXTLEN, (WPARAM)i, 0) < 512) lb(c, LB_GETTEXT, (WPARAM)i, (LPARAM)buf);
                RECT t2 = tr;
                t2.left += 2 * k;
                DrawTextW(dc, buf, -1, &t2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
        RECT b;
        button_rect(w, c, &b);
        draw_arrow(dc, &b, 1, disabled ? sys_color(COLOR_GRAYTEXT) : 0x606060);
    } else if (CBTYPE(w) == CBS_DROPDOWN) {
        HBRUSH bg = ctl_color(w, WM_CTLCOLOREDIT, dc);
        FillRect(dc, &f, bg);
        RECT fr = f;
        for (int n = 0; n < k; n++) { frame_rect(dc, &fr, disabled ? 0xBFBFBF : (c->hot || c->focus) ? 0xD77800 : 0x7A7A7A); InflateRect(&fr, -1, -1); }
        RECT b;
        button_rect(w, c, &b);
        InflateRect(&b, 0, -k);
        b.right -= k;
        COLORREF bf = c->pressed || c->dropped ? 0xF7E4CC : c->hot ? 0xFBF1E5 : 0;
        if (bf && !disabled) fill_rect(dc, &b, bf);
        draw_arrow(dc, &b, 1, disabled ? sys_color(COLOR_GRAYTEXT) : 0x606060);
    } else {
        HBRUSH bg = ctl_color(w, WM_CTLCOLOREDIT, dc);
        FillRect(dc, &f, bg);
        RECT fr = f;
        for (int n = 0; n < k; n++) { frame_rect(dc, &fr, 0x7A7A7A); InflateRect(&fr, -1, -1); }
    }
    SelectObject(dc, of);
}

static void layout(Wnd *w, Cb *c)
{
    int cw = w->client.right - w->client.left, k = dpi_k(w);
    if (c->edit) {
        if (CBTYPE(w) == CBS_SIMPLE) MoveWindow(c->edit, k, k, cw - 2 * k, c->field_h - 2 * k, TRUE);
        else MoveWindow(c->edit, 3 * k, 2 * k, cw - btn_w(w) - 4 * k, c->field_h - 4 * k, TRUE);
    }
    if (CBTYPE(w) == CBS_SIMPLE && c->list) {
        int ch = w->client.bottom - w->client.top;
        MoveWindow(c->list, 0, c->field_h, cw, MAX(0, ch - c->field_h), TRUE);
    }
}

static void measure_field(Wnd *w, Cb *c)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, cfont(w));
    int fh = font_height(dc);
    SelectObject(dc, of);
    int k = dpi_k(w);
    c->field_h = fh + 8 * k;
    if (w->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
        MEASUREITEMSTRUCT mi = { ODT_COMBOBOX, (UINT)w->id, (UINT)-1, 0, (UINT)fh, 0 };
        if (w->parent) send_msg(w->parent, WM_MEASUREITEM, (WPARAM)w->id, (LPARAM)&mi);
        if ((int)mi.itemHeight + 6 * k > c->field_h) c->field_h = (int)mi.itemHeight + 6 * k;
    }
}

LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    Cb *c = cb_of(w);
    if (!c && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        c = calloc(1, sizeof(Cb));
        if (!c) return FALSE;
        w->ctl = c;
        w->style &= ~(WS_VSCROLL | WS_HSCROLL);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        measure_field(w, c);
        int total = cs ? cs->cy : w->rect.bottom - w->rect.top;
        c->drop_h = total - c->field_h;
        DWORD lstyle = WS_BORDER | LBS_NOTIFY | LBS_COMBOBOX |
                       ((w->style & CBS_SORT) ? LBS_SORT : 0) | ((w->style & CBS_HASSTRINGS) ? LBS_HASSTRINGS : 0) |
                       ((w->style & CBS_OWNERDRAWFIXED) ? LBS_OWNERDRAWFIXED : 0) | ((w->style & CBS_OWNERDRAWVARIABLE) ? LBS_OWNERDRAWVARIABLE : 0) |
                       ((w->style & CBS_DISABLENOSCROLL) ? LBS_DISABLENOSCROLL : 0) | LBS_NOINTEGRALHEIGHT;
        if (CBTYPE(w) == CBS_SIMPLE) {
            c->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", NULL, lstyle | WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                      0, c->field_h, 10, 10, h, (HMENU)1000, w->inst, NULL);
            Wnd *lw = W_quiet(c->list);
            if (lw) lw->style &= ~WS_BORDER;
        } else {
            c->list = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"ComboLBox", NULL,
                                      lstyle | WS_POPUP | WS_VSCROLL, 0, 0, 10, 10, h, NULL, w->inst, (LPVOID)h);
        }
        if (CBTYPE(w) != CBS_DROPDOWNLIST) {
            DWORD es = WS_CHILD | WS_VISIBLE | ES_LEFT | ((w->style & CBS_AUTOHSCROLL) ? ES_AUTOHSCROLL : 0) |
                       ((w->style & CBS_UPPERCASE) ? ES_UPPERCASE : 0) | ((w->style & CBS_LOWERCASE) ? ES_LOWERCASE : 0) |
                       ((w->style & WS_DISABLED) ? WS_DISABLED : 0);
            if (!(w->style & CBS_AUTOHSCROLL)) es |= ES_AUTOHSCROLL;
            c->edit = CreateWindowExW(0, L"Edit", w->text, es, 0, 0, 10, 10, h, (HMENU)1001, w->inst, NULL);
        }
        /* the combo box is as tall as its selection field */
        if (CBTYPE(w) != CBS_SIMPLE) wnd_set_pos(w, 0, 0, 0, w->rect.right - w->rect.left, c->field_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout(w, c);
        return 0;
    }
    case WM_DESTROY:
        if (c->list && CBTYPE(w) != CBS_SIMPLE) DestroyWindow(c->list);
        c->list = 0;
        return 0;
    case WM_NCDESTROY:
        free(c);
        w->ctl = NULL;
        return 0;
    case WM_SIZE: layout(w, c); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) paint(w, c, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: paint(w, c, (HDC)wp); return 0;
    case WM_SETFONT:
        w->font = (HFONT)wp;
        if (c->edit) SendMessageW(c->edit, WM_SETFONT, wp, lp);
        if (c->list) SendMessageW(c->list, WM_SETFONT, wp, lp);
        measure_field(w, c);
        if (CBTYPE(w) != CBS_SIMPLE) wnd_set_pos(w, 0, 0, 0, w->rect.right - w->rect.left, c->field_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout(w, c);
        if (lp) invalidate(w, NULL, TRUE, 1);
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_ENABLE:
        if (c->edit) EnableWindow(c->edit, (BOOL)wp);
        if (c->list && CBTYPE(w) == CBS_SIMPLE) EnableWindow(c->list, (BOOL)wp);
        redraw(w);
        return 0;
    case WM_GETDLGCODE: {
        LRESULT r = DLGC_WANTARROWS | DLGC_WANTCHARS;
        MSG *m = (MSG *)lp;
        if (c->dropped && m && m->message == WM_KEYDOWN && (m->wParam == VK_RETURN || m->wParam == VK_ESCAPE)) r |= DLGC_WANTMESSAGE;
        return r;
    }
    case WM_SETFOCUS:
        if (c->edit) { set_focus(c->edit); return 0; }
        c->focus = 1;
        redraw(w);
        cnotify(w, CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS: {
        HWND to = (HWND)wp;
        if (c->edit && to == c->edit) return 0;
        if (c->dropped) close_list(w, c, 0);
        c->focus = 0;
        redraw(w);
        if (!c->edit) cnotify(w, CBN_KILLFOCUS);
        return 0;
    }
    case WM_COMMAND:
        if ((HWND)lp == c->edit && c->edit) {
            switch (HIWORD(wp)) {
            case EN_SETFOCUS: c->focus = 1; redraw(w); cnotify(w, CBN_SETFOCUS); break;
            case EN_KILLFOCUS: c->focus = 0; if (c->dropped) close_list(w, c, 0); redraw(w); cnotify(w, CBN_KILLFOCUS); break;
            case EN_CHANGE: if (!c->in_notify) cnotify(w, CBN_EDITCHANGE); break;
            case EN_UPDATE: if (!c->in_notify) cnotify(w, CBN_EDITUPDATE); break;
            }
            return 0;
        }
        if ((HWND)lp == c->list && c->list) {
            if (HIWORD(wp) == LBN_SELCHANGE && CBTYPE(w) == CBS_SIMPLE) { sync_edit(w, c); cnotify(w, CBN_SELCHANGE); }
            if (HIWORD(wp) == LBN_DBLCLK && CBTYPE(w) == CBS_SIMPLE) cnotify(w, CBN_DBLCLK);
            return 0;
        }
        return 0;
    case CBI_CANCEL: close_list(w, c, 0); return 0;
    case CBI_CHOOSE: close_list(w, c, 1); if (W_quiet(h) && !c->edit) set_focus(h); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT b;
        button_rect(w, c, &b);
        if (CBTYPE(w) == CBS_DROPDOWN && !PtInRect(&b, pt)) { set_focus(c->edit); return 0; }
        if (!c->edit) set_focus(h);
        else if (GetFocus() != c->edit) set_focus(c->edit);
        if (!W_quiet(h)) return 0;
        if (CBTYPE(w) == CBS_SIMPLE) return 0;
        if (c->dropped) { close_list(w, c, 0); return 0; }
        c->pressed = 1;
        drop(w, c);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!c->hot) {
            c->hot = 1;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            redraw(w);
        }
        return 0;
    case WM_MOUSELEAVE: c->hot = 0; redraw(w); return 0;
    case WM_MOUSEWHEEL:
        if (!c->dropped && CBTYPE(w) != CBS_SIMPLE) {
            int i = (int)lb(c, LB_GETCURSEL, 0, 0);
            select_index(w, c, (short)HIWORD(wp) > 0 ? i - 1 : i + 1, 1);
            return 0;
        }
        return 0;
    case WM_KEYDOWN: key(w, c, wp); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_DOWN || wp == VK_UP) { key(w, c, wp); return 0; }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CHAR:
        if (c->edit) return SendMessageW(c->edit, msg, wp, lp);
        if (wp >= ' ') {
            WCHAR s[2] = { (WCHAR)wp, 0 };
            int cur = (int)lb(c, LB_GETCURSEL, 0, 0);
            LRESULT i = lb(c, LB_FINDSTRING, (WPARAM)cur, (LPARAM)s);
            if (i >= 0) { if (c->dropped) { lb(c, LB_SETCURSEL, (WPARAM)i, 0); } else select_index(w, c, (int)i, 1); }
        }
        return 0;
    case WM_SETTEXT:
        if (c->edit) { SetWindowTextW(c->edit, (LPCWSTR)lp); return DefWindowProcW(h, msg, wp, lp); }
        {
            LRESULT i = lb(c, LB_FINDSTRINGEXACT, (WPARAM)-1, lp);
            if (i < 0) return CB_ERR;
            lb(c, LB_SETCURSEL, (WPARAM)i, 0);
            redraw(w);
            return TRUE;
        }
    case WM_GETTEXT:
        if (c->edit) return SendMessageW(c->edit, msg, wp, lp);
        {
            int i = (int)lb(c, LB_GETCURSEL, 0, 0);
            if (!wp) return 0;
            ((WCHAR *)lp)[0] = 0;
            if (i < 0) return 0;
            int n = (int)lb(c, LB_GETTEXTLEN, (WPARAM)i, 0);
            if (n < 0) return 0;
            WCHAR *buf = malloc(2 * ((size_t)n + 1));
            if (!buf) return 0;
            lb(c, LB_GETTEXT, (WPARAM)i, (LPARAM)buf);
            int k = MIN(n, (int)wp - 1);
            memcpy((void *)lp, buf, 2 * (size_t)k);
            ((WCHAR *)lp)[k] = 0;
            free(buf);
            return k;
        }
    case WM_GETTEXTLENGTH:
        if (c->edit) return SendMessageW(c->edit, msg, wp, lp);
        {
            int i = (int)lb(c, LB_GETCURSEL, 0, 0);
            return i < 0 ? 0 : lb(c, LB_GETTEXTLEN, (WPARAM)i, 0);
        }
    case WM_CUT: case WM_COPY: case WM_PASTE: case WM_CLEAR: case WM_UNDO:
        return c->edit ? SendMessageW(c->edit, msg, wp, lp) : 0;

    case CB_ADDSTRING: { LRESULT r = lb(c, LB_ADDSTRING, wp, lp); return r; }
    case CB_INSERTSTRING: return lb(c, LB_INSERTSTRING, wp, lp);
    case CB_DELETESTRING: return lb(c, LB_DELETESTRING, wp, lp);
    case CB_RESETCONTENT: lb(c, LB_RESETCONTENT, 0, 0); if (c->edit) { c->in_notify++; SetWindowTextW(c->edit, L""); c->in_notify--; } redraw(w); return 0;
    case CB_GETCOUNT: return lb(c, LB_GETCOUNT, 0, 0);
    case CB_GETCURSEL: return lb(c, LB_GETCURSEL, 0, 0);
    case CB_SETCURSEL: {
        LRESULT r = lb(c, LB_SETCURSEL, wp, 0);
        if ((int)wp < 0) { if (c->edit) { c->in_notify++; SetWindowTextW(c->edit, L""); c->in_notify--; } }
        else sync_edit(w, c);
        redraw(w);
        return (int)wp < 0 ? CB_ERR : r;
    }
    case CB_GETLBTEXT: return lb(c, LB_GETTEXT, wp, lp);
    case CB_GETLBTEXTLEN: return lb(c, LB_GETTEXTLEN, wp, lp);
    case CB_FINDSTRING: return lb(c, LB_FINDSTRING, wp, lp);
    case CB_FINDSTRINGEXACT: return lb(c, LB_FINDSTRINGEXACT, wp, lp);
    case CB_SELECTSTRING: {
        LRESULT i = lb(c, LB_FINDSTRING, wp, lp);
        if (i >= 0) { lb(c, LB_SETCURSEL, (WPARAM)i, 0); sync_edit(w, c); redraw(w); }
        return i;
    }
    case CB_GETITEMDATA: return lb(c, LB_GETITEMDATA, wp, lp);
    case CB_SETITEMDATA: return lb(c, LB_SETITEMDATA, wp, lp);
    case CB_GETTOPINDEX: return lb(c, LB_GETTOPINDEX, 0, 0);
    case CB_SETTOPINDEX: return lb(c, LB_SETTOPINDEX, wp, 0);
    case CB_INITSTORAGE: return lb(c, LB_INITSTORAGE, wp, lp);
    case CB_SHOWDROPDOWN: if (wp) drop(w, c); else close_list(w, c, 0); return TRUE;
    case CB_GETDROPPEDSTATE: return c->dropped;
    case CB_LIMITTEXT: return c->edit ? SendMessageW(c->edit, EM_LIMITTEXT, wp, 0) : TRUE;
    case CB_GETEDITSEL: return c->edit ? SendMessageW(c->edit, EM_GETSEL, wp, lp) : CB_ERR;
    case CB_SETEDITSEL: return c->edit ? (SendMessageW(c->edit, EM_SETSEL, (WPARAM)(SHORT)LOWORD(lp), (LPARAM)(SHORT)HIWORD(lp)), TRUE) : CB_ERR;
    case CB_SETITEMHEIGHT:
        if ((int)wp == -1) {
            c->field_h = (int)lp + 6 * dpi_k(w);
            if (CBTYPE(w) != CBS_SIMPLE) wnd_set_pos(w, 0, 0, 0, w->rect.right - w->rect.left, c->field_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            layout(w, c);
            return 0;
        }
        return lb(c, LB_SETITEMHEIGHT, wp, lp);
    case CB_GETITEMHEIGHT: return (int)wp == -1 ? c->field_h - 6 * dpi_k(w) : lb(c, LB_GETITEMHEIGHT, wp, 0);
    case CB_SETDROPPEDWIDTH: c->drop_w = (int)wp; return c->drop_w;
    case CB_GETDROPPEDWIDTH: return MAX(c->drop_w, w->rect.right - w->rect.left);
    case CB_GETDROPPEDCONTROLRECT: {
        RECT *r = (RECT *)lp;
        GetWindowRect(h, r);
        r->bottom = r->top + c->field_h + MAX(c->drop_h, 0);
        return TRUE;
    }
    case CB_SETEXTENDEDUI: c->ext_ui = wp != 0; return 0;
    case CB_GETEXTENDEDUI: return c->ext_ui;
    case CB_SETMINVISIBLE: c->min_visible = (int)wp; return TRUE;
    case CB_GETMINVISIBLE: return c->min_visible > 0 ? c->min_visible : 30;
    case CB_GETCOMBOBOXINFO: {
        COMBOBOXINFO *ci = (COMBOBOXINFO *)lp;
        if (!ci) return FALSE;
        field_rect(w, c, &ci->rcItem);
        ci->rcItem.right -= btn_w(w);
        button_rect(w, c, &ci->rcButton);
        ci->stateButton = c->dropped ? STATE_SYSTEM_PRESSED : 0;
        ci->hwndCombo = h; ci->hwndItem = c->edit; ci->hwndList = c->list;
        return TRUE;
    }
    case CB_SETCUEBANNER: return c->edit ? SendMessageW(c->edit, EM_SETCUEBANNER, 0, lp) : FALSE;
    case CB_GETCUEBANNER: return c->edit ? SendMessageW(c->edit, EM_GETCUEBANNER, wp, lp) : FALSE;
    case CB_DIR: return CB_ERR;
    case CB_SETLOCALE: case CB_GETLOCALE: return 0x0409;
    case CB_GETHORIZONTALEXTENT: return lb(c, LB_GETHORIZONTALEXTENT, 0, 0);
    case CB_SETHORIZONTALEXTENT: return lb(c, LB_SETHORIZONTALEXTENT, wp, 0);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* An edit box inside a combo box passes it the keys that open the list */
int combo_edit_key(Wnd *edit, UINT msg, WPARAM wp)
{
    Wnd *p = edit->parent;
    if (!p || !p->cls || wcsicmp_(p->cls->name, L"ComboBox")) return 0;
    Cb *c = cb_of(p);
    if (!c) return 0;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (wp == VK_F4 || wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT ||
            (c->dropped && (wp == VK_RETURN || wp == VK_ESCAPE))) {
            if (!c->dropped && (wp == VK_PRIOR || wp == VK_NEXT)) return 0;
            key(p, c, wp);
            return 1;
        }
    }
    return 0;
}

int combo_edit_wants_all(Wnd *edit)
{
    Wnd *p = edit->parent;
    if (!p || !p->cls || wcsicmp_(p->cls->name, L"ComboBox")) return 0;
    Cb *c = cb_of(p);
    return c && c->dropped;
}

USERAPI BOOL GetComboBoxInfo(HWND h, PCOMBOBOXINFO ci) { return (BOOL)SendMessageW(h, CB_GETCOMBOBOXINFO, 0, (LPARAM)ci); }
USERAPI int DlgDirListComboBoxW(HWND h, LPWSTR path, int id, int st, UINT type) { (void)h; (void)path; (void)id; (void)st; (void)type; return 0; }
