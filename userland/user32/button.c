/*
 * button.c — the BUTTON class: push buttons, check boxes, radio buttons,
 * group boxes and owner-drawn buttons (drawn as Windows 10 draws them)
 */
#include "u32.h"

typedef struct {
    int check;                      /* BST_UNCHECKED / CHECKED / INDETERMINATE */
    int pushed, focus, hot, tracking, space;
    HANDLE image;
    int image_type;
} Btn;

#define BTYPE(w) ((w)->style & BS_TYPEMASK)

static int is_check(DWORD t) { return t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE; }
static int is_radio(DWORD t) { return t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON; }
static int is_push(DWORD t) { return t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON || t == BS_SPLITBUTTON || t == BS_DEFSPLITBUTTON || t == BS_COMMANDLINK || t == BS_DEFCOMMANDLINK; }

static HFONT wfont(Wnd *w) { return ctl_font(w); }

static void redraw(Wnd *w) { invalidate(w, NULL, FALSE, 0); }

static void draw_push_frame(HDC dc, RECT *r, Btn *b, Wnd *w, int is_default)
{
    int disabled = (w->style & WS_DISABLED) != 0;
    COLORREF fill = 0xE1E1E1, border = 0xADADAD;
    if (disabled) { fill = 0xCCCCCC; border = 0xBFBFBF; }
    else if (b->pushed || (b->check && (w->style & BS_PUSHLIKE))) { fill = 0xF7E4CC; border = 0x995400; }
    else if (b->hot) { fill = 0xFBF1E5; border = 0xD77800; }
    else if (is_default || b->focus) border = 0xD77800;
    int k = dpi_k(w), lines = (is_default || b->focus) && !disabled && !b->pushed ? 2 * k : k;
    fill_rect(dc, r, fill);
    RECT i = *r;
    for (int n = 0; n < lines; n++) { frame_rect(dc, &i, border); InflateRect(&i, -1, -1); }
}

static UINT text_align(Wnd *w, UINT def)
{
    UINT f = 0;
    DWORD s = w->style;
    if ((s & BS_CENTER) == BS_CENTER) f |= DT_CENTER;
    else if (s & BS_RIGHT) f |= DT_RIGHT;
    else if (s & BS_LEFT) f |= DT_LEFT;
    else f |= def;
    return f;
}

static void draw_label(HDC dc, Wnd *w, RECT *r, UINT align, int multiline_default)
{
    const WCHAR *t = w->text ? w->text : L"";
    if (!*t) return;
    UINT fmt = align | DT_HIDEPREFIX;
    DWORD s = w->style;
    int multi = (s & BS_MULTILINE) || multiline_default;
    if (multi) {
        RECT m = *r;
        int h = DrawTextW(dc, t, -1, &m, fmt | DT_WORDBREAK | DT_CALCRECT);
        RECT d = *r;
        if ((s & BS_VCENTER) == BS_TOP) {}
        else if ((s & BS_VCENTER) == BS_BOTTOM) d.top = r->bottom - h;
        else d.top = r->top + (r->bottom - r->top - h) / 2;
        DrawTextW(dc, t, -1, &d, fmt | DT_WORDBREAK);
    } else {
        UINT v = DT_VCENTER;
        if ((s & BS_VCENTER) == BS_TOP) v = DT_TOP;
        else if ((s & BS_VCENTER) == BS_BOTTOM) v = DT_BOTTOM;
        DrawTextW(dc, t, -1, r, fmt | DT_SINGLELINE | v | DT_END_ELLIPSIS);
    }
}

static void draw_image(HDC dc, Wnd *w, Btn *b, RECT *r)
{
    if (!b->image) return;
    int cx = 0, cy = 0;
    if (b->image_type == IMAGE_ICON || b->image_type == IMAGE_CURSOR) icon_size((HICON)b->image, &cx, &cy);
    else { BITMAP bm; if (GetObjectW(b->image, sizeof(bm), &bm)) { cx = bm.bmWidth; cy = bm.bmHeight; } }
    int x = r->left + (r->right - r->left - cx) / 2, y = r->top + (r->bottom - r->top - cy) / 2;
    if (b->image_type == IMAGE_ICON || b->image_type == IMAGE_CURSOR) draw_icon(dc, x, y, (HICON)b->image, cx, cy);
    else DrawStateW(dc, 0, NULL, (LPARAM)b->image, 0, x, y, cx, cy, DST_BITMAP | ((w->style & WS_DISABLED) ? DSS_DISABLED : 0));
}

static void paint(Wnd *w, HDC dc)
{
    Btn *b = w->ctl;
    RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    DWORD t = BTYPE(w);
    int k = dpi_k(w);                                       /* (sizes at the window's DPI) */
    int disabled = (w->style & WS_DISABLED) != 0;
    HGDIOBJ of = SelectObject(dc, wfont(w));
    SetBkMode(dc, TRANSPARENT);

    if (t == BS_OWNERDRAW) {
        DRAWITEMSTRUCT di;
        memset(&di, 0, sizeof(di));
        di.CtlType = ODT_BUTTON; di.CtlID = (UINT)w->id; di.itemAction = ODA_DRAWENTIRE;
        di.itemState = (b->pushed ? ODS_SELECTED : 0) | (b->focus ? ODS_FOCUS : 0) | (disabled ? ODS_DISABLED : 0) | (b->hot ? ODS_HOTLIGHT : 0);
        di.hwndItem = w->h; di.hDC = dc; di.rcItem = r;
        if (w->parent) send_msg(w->parent, WM_DRAWITEM, (WPARAM)w->id, (LPARAM)&di);
        SelectObject(dc, of);
        return;
    }
    if (is_push(t) || ((is_check(t) || is_radio(t)) && (w->style & BS_PUSHLIKE))) {
        HBRUSH bg = ctl_color(w, WM_CTLCOLORBTN, dc);
        (void)bg;
        draw_push_frame(dc, &r, b, w, t == BS_DEFPUSHBUTTON || t == BS_DEFSPLITBUTTON || t == BS_DEFCOMMANDLINK);
        SetTextColor(dc, disabled ? 0x838383 : sys_color(COLOR_BTNTEXT));
        RECT tr = r;
        InflateRect(&tr, -4 * k, -2 * k);
        if (w->style & (BS_ICON | BS_BITMAP)) draw_image(dc, w, b, &r);
        else draw_label(dc, w, &tr, text_align(w, DT_CENTER), 0);
        if (b->focus && !disabled && 0) { RECT f = r; InflateRect(&f, -3, -3); draw_focus(dc, &f); }
        SelectObject(dc, of);
        return;
    }
    /* check boxes, radio buttons, group boxes: on the parent's background */
    HBRUSH bg = ctl_color(w, WM_CTLCOLORSTATIC, dc);
    COLORREF tc = GetTextColor(dc);
    if (t == BS_GROUPBOX) {
        int fh = font_height(dc);
        RECT fr = r;
        fr.top += fh / 2;
        for (int n = 0; n < k; n++) { frame_rect(dc, &fr, 0xDCDCDC); InflateRect(&fr, -1, -1); }
        const WCHAR *txt = w->text ? w->text : L"";
        if (*txt) {
            RECT m = { 0, 0, r.right - 16 * k, fh };
            DrawTextW(dc, txt, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_HIDEPREFIX);
            RECT tb = { 6 * k, 0, 6 * k + (m.right - m.left) + 4 * k, fh };
            if (tb.right > r.right - 6 * k) tb.right = r.right - 6 * k;
            FillRect(dc, &tb, bg);
            SetTextColor(dc, disabled ? sys_color(COLOR_GRAYTEXT) : tc);
            RECT tt = { tb.left + 2 * k, 0, tb.right, fh };
            DrawTextW(dc, txt, -1, &tt, DT_SINGLELINE | DT_HIDEPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, of);
        return;
    }
    FillRect(dc, &r, bg);
    int box = 13 * k;
    int right = (w->style & BS_LEFTTEXT) != 0;
    int by;
    if ((w->style & BS_VCENTER) == BS_TOP) by = r.top + k;
    else if ((w->style & BS_VCENTER) == BS_BOTTOM) by = r.bottom - box - k;
    else by = r.top + (r.bottom - r.top - box) / 2;
    RECT br = { right ? r.right - box : r.left, by, right ? r.right : r.left + box, by + box };
    COLORREF fill = 0xFFFFFF, border = 0x333333, mark = 0x333333;
    if (disabled) { fill = 0xF0F0F0; border = 0xBFBFBF; mark = 0xBFBFBF; }
    else if (b->pushed) { fill = 0xF7E4CC; border = 0x995400; mark = 0x995400; }
    else if (b->hot) { border = 0xD77800; mark = 0xD77800; }
    if (is_radio(t)) {
        HBRUSH fb = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, k, border);
        HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
        Ellipse(dc, br.left, br.top, br.right, br.bottom);
        if (b->check) {
            HBRUSH mb = CreateSolidBrush(mark);
            SelectObject(dc, mb);
            SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, br.left + 3 * k, br.top + 3 * k, br.right - 2 * k, br.bottom - 2 * k);
            SelectObject(dc, fb);
            DeleteObject(mb);
        }
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(fb); DeleteObject(pen);
    } else {
        fill_rect(dc, &br, fill);
        RECT fr = br;
        for (int n = 0; n < k; n++) { frame_rect(dc, &fr, border); InflateRect(&fr, -1, -1); }
        if (b->check == BST_CHECKED) draw_check_mark(dc, br.left, br.top, box, mark);
        else if (b->check == BST_INDETERMINATE) { RECT i = br; InflateRect(&i, -3 * k, -3 * k); fill_rect(dc, &i, mark); }
    }
    RECT tr = r;
    if (right) tr.right -= box + 5 * k; else tr.left += box + 5 * k;
    SetTextColor(dc, disabled ? sys_color(COLOR_GRAYTEXT) : tc);
    if (w->style & (BS_ICON | BS_BITMAP)) draw_image(dc, w, b, &tr);
    else draw_label(dc, w, &tr, text_align(w, DT_LEFT), 0);
    if (b->focus && w->text && w->text[0]) {
        RECT m = tr;
        DrawTextW(dc, w->text, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_HIDEPREFIX);
        int th = m.bottom - m.top;
        RECT f = { tr.left - 1, tr.top + (tr.bottom - tr.top - th) / 2 - 1, MIN(m.right + 1, tr.right), 0 };
        f.bottom = f.top + th + 2;
        (void)f;
    }
    SelectObject(dc, of);
}

/* Check the radio button and clear the other automatic ones in its group */
static void radio_group_check(Wnd *w)
{
    if (!w->parent) return;
    Wnd *start = w;
    while (!(start->style & WS_GROUP) && start->prev) start = start->prev;
    for (Wnd *s = start; s; s = s->next) {
        if (s != start && (s->style & WS_GROUP)) break;
        if (s == w || !s->cls || wcsicmp_(s->cls->name, L"Button")) continue;
        if ((s->style & BS_TYPEMASK) == BS_AUTORADIOBUTTON) send_msg(s, BM_SETCHECK, BST_UNCHECKED, 0);
    }
}

static void click(Wnd *w)
{
    Btn *b = w->ctl;
    HWND h = w->h;
    switch (BTYPE(w)) {
    case BS_AUTOCHECKBOX: send_msg(w, BM_SETCHECK, b->check ? BST_UNCHECKED : BST_CHECKED, 0); break;
    case BS_AUTO3STATE: send_msg(w, BM_SETCHECK, (b->check + 1) % 3, 0); break;
    case BS_AUTORADIOBUTTON:
        send_msg(w, BM_SETCHECK, BST_CHECKED, 0);
        if (W_quiet(h)) radio_group_check(w);
        break;
    }
    if (W_quiet(h)) notify_parent(w, BN_CLICKED);
}

LRESULT CALLBACK ButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    Btn *b = w->ctl;
    if (!b && msg != WM_NCCREATE && msg != WM_CREATE) return DefWindowProcW(h, msg, wp, lp);
    DWORD t = BTYPE(w);
    switch (msg) {
    case WM_NCCREATE:
        w->ctl = calloc(1, sizeof(Btn));
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE:
        if (t == BS_GROUPBOX) w->exstyle |= WS_EX_TRANSPARENT;
        return 0;
    case WM_NCDESTROY:
        free(w->ctl);
        w->ctl = NULL;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) paint(w, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: paint(w, (HDC)wp); return 0;
    case WM_NCHITTEST:
        if (t == BS_GROUPBOX) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_ENABLE: redraw(w); return 0;
    case WM_SETTEXT: { LRESULT r = DefWindowProcW(h, msg, wp, lp); invalidate(w, NULL, TRUE, 0); if (t == BS_GROUPBOX && w->parent) invalidate(w->parent, &w->rect, TRUE, 1); return r; }
    case WM_SETFONT: w->font = (HFONT)wp; if (lp) redraw(w); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_GETDLGCODE:
        if (t == BS_GROUPBOX) return DLGC_STATIC;
        if (is_radio(t)) return DLGC_BUTTON | DLGC_RADIOBUTTON;
        if (t == BS_DEFPUSHBUTTON || t == BS_DEFSPLITBUTTON || t == BS_DEFCOMMANDLINK) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        if (is_push(t) || t == BS_OWNERDRAW) return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        return DLGC_BUTTON;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (t == BS_GROUPBOX) return 0;
        if (msg == WM_LBUTTONDBLCLK && (is_radio(t) || t == BS_OWNERDRAW) && (w->style & BS_NOTIFY)) { notify_parent(w, BN_DOUBLECLICKED); return 0; }
        set_focus(h);
        if (!W_quiet(h)) return 0;
        SetCapture(h);
        b->tracking = 1;
        b->pushed = 1;
        redraw(w);
        return 0;
    case WM_MOUSEMOVE: {
        if (!b->hot) {
            b->hot = 1;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            redraw(w);
        }
        if (b->tracking) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
            int in = PtInRect(&r, pt);
            if (in != b->pushed) { b->pushed = in; redraw(w); }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (b->hot) { b->hot = 0; redraw(w); }
        return 0;
    case WM_LBUTTONUP:
        if (b->tracking) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
            b->tracking = 0;
            int was = b->pushed;
            b->pushed = 0;
            ReleaseCapture();
            if (!W_quiet(h)) return 0;
            redraw(w);
            if (was && PtInRect(&r, pt)) click(w);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (b->tracking) { b->tracking = 0; b->pushed = 0; redraw(w); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && !(lp & (1 << 30))) { b->space = 1; b->pushed = 1; redraw(w); SetCapture(h); }
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && b->space) {
            b->space = 0;
            b->pushed = 0;
            ReleaseCapture();
            redraw(w);
            click(w);
        }
        return 0;
    case WM_CHAR:
        if (is_check(t) && (wp == '+' || wp == '=' || wp == '-')) {
            if ((wp == '-') == (b->check == BST_CHECKED)) click(w);
        }
        return 0;
    case WM_SYSKEYUP: return DefWindowProcW(h, msg, wp, lp);
    case WM_SETFOCUS:
        b->focus = 1;
        redraw(w);
        if (w->style & BS_NOTIFY) notify_parent(w, BN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        b->focus = 0;
        if (b->space) { b->space = 0; b->pushed = 0; ReleaseCapture(); }
        redraw(w);
        if (w->style & BS_NOTIFY) notify_parent(w, BN_KILLFOCUS);
        return 0;
    case BM_GETCHECK: return b->check;
    case BM_SETCHECK: {
        int v = (int)wp;
        if (!is_check(t) && !is_radio(t)) return 0;
        if (v == BST_INDETERMINATE && t != BS_3STATE && t != BS_AUTO3STATE) v = BST_CHECKED;
        if (v != b->check) { b->check = v; redraw(w); }
        if (is_radio(t)) {
            if (v) w->style |= WS_TABSTOP; else w->style &= ~WS_TABSTOP;
        }
        return 0;
    }
    case BM_GETSTATE:
        return b->check | (b->pushed ? BST_PUSHED : 0) | (b->focus ? BST_FOCUS : 0) | (b->hot ? BST_HOT : 0);
    case BM_SETSTATE:
        if ((wp != 0) != b->pushed) { b->pushed = wp != 0; redraw(w); }
        return 0;
    case BM_SETSTYLE:
        w->style = (w->style & ~BS_TYPEMASK) | (wp & BS_TYPEMASK);
        if (lp) invalidate(w, NULL, TRUE, 0);
        return 0;
    case BM_CLICK:
        if (!(w->style & WS_DISABLED)) click(w);
        return 0;
    case BM_GETIMAGE: return (LRESULT)b->image;
    case BM_SETIMAGE: {
        HANDLE old = b->image;
        b->image = (HANDLE)lp;
        b->image_type = (int)wp;
        redraw(w);
        return (LRESULT)old;
    }
    case BM_SETDONTCLICK: return 0;
    case BCM_GETIDEALSIZE: {
        SIZE *sz = (SIZE *)lp;
        HDC dc = GetDC(h);
        HGDIOBJ of = SelectObject(dc, wfont(w));
        RECT m = { 0, 0, 0, 0 };
        DrawTextW(dc, w->text ? w->text : L"", -1, &m, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, of);
        ReleaseDC(h, dc);
        int extra = is_push(t) ? 20 : 20;
        if (sz) { sz->cx = m.right + extra; sz->cy = MAX(m.bottom + 8, 23); }
        return TRUE;
    }
    case BCM_SETSHIELD: case BCM_SETNOTE: case BCM_SETIMAGELIST: case BCM_SETTEXTMARGIN: case BCM_SETDROPDOWNSTATE:
    case BCM_SETSPLITINFO:
        return TRUE;
    case BCM_GETNOTE: case BCM_GETNOTELENGTH: case BCM_GETIMAGELIST: case BCM_GETTEXTMARGIN: case BCM_GETSPLITINFO:
        return FALSE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* -----------------------------------------------------------------------
 * Dialog button helpers
 * ----------------------------------------------------------------------- */
USERAPI BOOL CheckDlgButton(HWND h, int id, UINT st) { SendDlgItemMessageW(h, id, BM_SETCHECK, st, 0); return TRUE; }
USERAPI UINT IsDlgButtonChecked(HWND h, int id) { return (UINT)SendDlgItemMessageW(h, id, BM_GETCHECK, 0, 0); }

USERAPI BOOL CheckRadioButton(HWND h, int first, int last, int check)
{
    for (int id = first; id <= last; id++) SendDlgItemMessageW(h, id, BM_SETCHECK, id == check ? BST_CHECKED : BST_UNCHECKED, 0);
    return TRUE;
}
