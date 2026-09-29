/*
 * listbox.c — the LISTBOX class (and ComboLBox, a combo box's drop-down
 * list): single, multiple and extended selection, sorting, owner-drawn
 * items, scrolling
 */
#include "u32.h"

typedef struct { WCHAR *s; ULONG_PTR data; int sel; int h; } Item;

typedef struct {
    Item *it;
    int   n, cap;
    int   top, caret, anchor, cursel;
    int   ih;                       /* item height */
    int   focus, tracking, moved;
    int   xoff, hext;
    int   colw;
    Wnd  *combo;                    /* the combo box this is the list of */
    int   ntabs, tabs[32];
    int   no_redraw;
    int   hover_select;             /* a drop-down: selection follows the pointer */
} LB;

#define LBSTYLE(w) ((w)->style)
static int has_strings(Wnd *w) { return !(w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (w->style & LBS_HASSTRINGS); }
static int multi(Wnd *w) { return (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0; }
static LB *lb_of(Wnd *w) { return w ? (LB *)w->ctl : NULL; }

int lb_is_string_msg(Wnd *w, UINT msg)
{
    (void)msg;
    if (!w || !w->cls || (wcsicmp_(w->cls->name, L"ListBox") && wcsicmp_(w->cls->name, L"ComboLBox"))) return 1;
    return has_strings(w);
}

static HFONT lfont(Wnd *w) { return w->font ? w->font : gui_font(); }

static int client_h(Wnd *w) { return w->client.bottom - w->client.top; }
static int client_w(Wnd *w) { return w->client.right - w->client.left; }

static int page(Wnd *w, LB *l)
{
    int p = client_h(w) / (l->ih ? l->ih : 1);
    return p < 1 ? 1 : p;
}

static Wnd *notify_target(Wnd *w, LB *l) { return l->combo ? l->combo : w->parent ? w->parent : w->owner; }

static void notify(Wnd *w, LB *l, UINT code)
{
    if (l->combo) return;                                    /* the combo box tells its own parent */
    if (!(w->style & LBS_NOTIFY)) return;
    notify_parent(w, code);
}

static void update_sb(Wnd *w, LB *l)
{
    if (!(w->style & WS_VSCROLL)) return;
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | ((w->style & LBS_DISABLENOSCROLL) ? SIF_DISABLENOSCROLL : 0),
                      0, l->n ? l->n - 1 : 0, (UINT)page(w, l), l->top, 0 };
    SetScrollInfo(w->h, SB_VERT, &si, TRUE);
}

static void redraw(Wnd *w, LB *l) { if (!l->no_redraw) invalidate(w, NULL, TRUE, 0); }

static void set_top(Wnd *w, LB *l, int t)
{
    int mx = l->n - page(w, l);
    if (t > mx) t = mx;
    if (t < 0) t = 0;
    if (t == l->top) return;
    l->top = t;
    update_sb(w, l);
    redraw(w, l);
}

static void ensure_visible(Wnd *w, LB *l, int i)
{
    if (i < 0 || i >= l->n) return;
    if (i < l->top) set_top(w, l, i);
    else if (i >= l->top + page(w, l)) set_top(w, l, i - page(w, l) + 1);
}

static void measure(Wnd *w, LB *l, int i)
{
    if (!(w->style & LBS_OWNERDRAWVARIABLE)) { l->it[i].h = l->ih; return; }
    Wnd *t = notify_target(w, l);
    MEASUREITEMSTRUCT mi = { l->combo ? ODT_COMBOBOX : ODT_LISTBOX, (UINT)(l->combo ? l->combo->id : w->id), (UINT)i, 0, (UINT)l->ih, l->it[i].data };
    if (t && t != l->combo) send_msg(t, WM_MEASUREITEM, mi.CtlID, (LPARAM)&mi);
    else if (l->combo && l->combo->parent) send_msg(l->combo->parent, WM_MEASUREITEM, mi.CtlID, (LPARAM)&mi);
    l->it[i].h = (int)mi.itemHeight;
}

static int insert_item(Wnd *w, LB *l, int at, const WCHAR *s, ULONG_PTR data)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        Item *n = realloc(l->it, sizeof(Item) * nc);
        if (!n) return LB_ERRSPACE;
        l->it = n; l->cap = nc;
    }
    if (at < 0 || at > l->n) at = l->n;
    memmove(l->it + at + 1, l->it + at, sizeof(Item) * (size_t)(l->n - at));
    Item *it = &l->it[at];
    memset(it, 0, sizeof(*it));
    if (has_strings(w)) it->s = wstrdup(s ? s : L"");
    else it->data = (ULONG_PTR)s;
    if (data) it->data = data;
    l->n++;
    if (l->cursel >= at) l->cursel++;
    if (l->caret >= at && l->n > 1) l->caret++;
    measure(w, l, at);
    update_sb(w, l);
    redraw(w, l);
    return at;
}

static int compare(Wnd *w, LB *l, const WCHAR *s, ULONG_PTR data, int i)
{
    if (has_strings(w)) return wcsicmp_(s ? s : L"", l->it[i].s ? l->it[i].s : L"");
    Wnd *t = notify_target(w, l);
    Wnd *dest = l->combo ? l->combo->parent : t;
    COMPAREITEMSTRUCT ci = { l->combo ? ODT_COMBOBOX : ODT_LISTBOX, (UINT)(l->combo ? l->combo->id : w->id), l->combo ? l->combo->h : w->h,
                             (UINT)-1, data, (UINT)i, l->it[i].data, 0 };
    return dest ? (int)send_msg(dest, WM_COMPAREITEM, ci.CtlID, (LPARAM)&ci) : 0;
}

static int add_item(Wnd *w, LB *l, const WCHAR *s)
{
    int at = l->n;
    if (w->style & LBS_SORT) {
        ULONG_PTR data = has_strings(w) ? 0 : (ULONG_PTR)s;
        int lo = 0, hi = l->n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (compare(w, l, s, data, mid) < 0) hi = mid; else lo = mid + 1;
        }
        at = lo;
    }
    return insert_item(w, l, at, s, 0);
}

static void delete_item(Wnd *w, LB *l, int i)
{
    if (!has_strings(w) || (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))) {
        Wnd *dest = l->combo ? l->combo->parent : notify_target(w, l);
        DELETEITEMSTRUCT di = { l->combo ? ODT_COMBOBOX : ODT_LISTBOX, (UINT)(l->combo ? l->combo->id : w->id), (UINT)i, l->combo ? l->combo->h : w->h, l->it[i].data };
        if (dest) send_msg(dest, WM_DELETEITEM, di.CtlID, (LPARAM)&di);
    }
    free(l->it[i].s);
    memmove(l->it + i, l->it + i + 1, sizeof(Item) * (size_t)(l->n - i - 1));
    l->n--;
    if (l->cursel == i) l->cursel = -1;
    else if (l->cursel > i) l->cursel--;
    if (l->caret >= l->n) l->caret = l->n - 1;
    if (l->top > 0 && l->top >= l->n) l->top = MAX(0, l->n - 1);
}

static void reset(Wnd *w, LB *l)
{
    while (l->n) delete_item(w, l, l->n - 1);
    l->top = 0; l->caret = 0; l->anchor = 0; l->cursel = -1;
    update_sb(w, l);
    redraw(w, l);
}

static int item_y(LB *l, int i)
{
    int y = 0;
    for (int k = l->top; k < i && k < l->n; k++) y += l->it[k].h ? l->it[k].h : l->ih;
    return y;
}

static int item_at(Wnd *w, LB *l, int x, int y, int *outside)
{
    (void)x;
    int o = 0;
    if (y < 0) { o = 1; y = 0; }
    int yy = 0, i = l->top;
    for (; i < l->n; i++) {
        int ih = l->it[i].h ? l->it[i].h : l->ih;
        if (y < yy + ih) break;
        yy += ih;
    }
    if (i >= l->n) { i = l->n - 1; o = 1; }
    if (y >= client_h(w)) o = 1;
    if (x < 0 || x >= client_w(w)) o = 1;
    if (outside) *outside = o;
    return i;
}

/* -----------------------------------------------------------------------
 * Selection
 * ----------------------------------------------------------------------- */
static void sel_changed(Wnd *w, LB *l) { redraw(w, l); notify(w, l, LBN_SELCHANGE); }

static int set_cursel(Wnd *w, LB *l, int i)
{
    if (i >= l->n) i = -1;
    if (multi(w)) return LB_ERR;
    if (l->cursel >= 0 && l->cursel < l->n) l->it[l->cursel].sel = 0;
    l->cursel = i;
    if (i >= 0) { l->it[i].sel = 1; l->caret = i; ensure_visible(w, l, i); }
    redraw(w, l);
    return i < 0 ? LB_ERR : i;
}

static void select_range(LB *l, int a, int b, int on)
{
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = MAX(a, 0); i <= b && i < l->n; i++) l->it[i].sel = on;
}

static void click_select(Wnd *w, LB *l, int i, UINT keys, int drag)
{
    if (i < 0 || i >= l->n || (w->style & LBS_NOSEL)) return;
    if (!multi(w)) {
        if (l->cursel != i) { set_cursel(w, l, i); notify(w, l, LBN_SELCHANGE); if (l->combo) send_msg(l->combo, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), (LPARAM)w->h); }
        return;
    }
    if (w->style & LBS_MULTIPLESEL) {
        if (!drag) { l->it[i].sel = !l->it[i].sel; l->caret = i; sel_changed(w, l); }
        return;
    }
    /* extended */
    if (keys & MK_SHIFT) {
        if (!(keys & MK_CONTROL)) for (int k = 0; k < l->n; k++) l->it[k].sel = 0;
        select_range(l, l->anchor, i, 1);
    } else if (keys & MK_CONTROL) {
        if (!drag) { l->it[i].sel = !l->it[i].sel; l->anchor = i; }
    } else {
        if (drag) { for (int k = 0; k < l->n; k++) l->it[k].sel = 0; select_range(l, l->anchor, i, 1); }
        else { for (int k = 0; k < l->n; k++) l->it[k].sel = 0; l->it[i].sel = 1; l->anchor = i; }
    }
    l->caret = i;
    ensure_visible(w, l, i);
    sel_changed(w, l);
}

/* -----------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */
static void draw_item(Wnd *w, LB *l, HDC dc, int i, RECT *r)
{
    int sel = l->it[i].sel;
    int focus = l->focus && i == l->caret;
    if (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) {
        DRAWITEMSTRUCT di;
        memset(&di, 0, sizeof(di));
        di.CtlType = l->combo ? ODT_COMBOBOX : ODT_LISTBOX;
        di.CtlID = (UINT)(l->combo ? l->combo->id : w->id);
        di.itemID = (UINT)i; di.itemAction = ODA_DRAWENTIRE;
        di.itemState = (sel ? ODS_SELECTED : 0) | (focus ? ODS_FOCUS : 0) | ((w->style & WS_DISABLED) ? ODS_DISABLED : 0);
        di.hwndItem = l->combo ? l->combo->h : w->h;
        di.hDC = dc; di.rcItem = *r; di.itemData = l->it[i].data;
        Wnd *dest = l->combo ? l->combo->parent : notify_target(w, l);
        if (dest) send_msg(dest, WM_DRAWITEM, di.CtlID, (LPARAM)&di);
        return;
    }
    COLORREF tc = GetTextColor(dc);
    if (sel) {
        fill_rect(dc, r, sys_color(COLOR_HIGHLIGHT));
        SetTextColor(dc, sys_color(COLOR_HIGHLIGHTTEXT));
    }
    if (w->style & WS_DISABLED) SetTextColor(dc, sys_color(COLOR_GRAYTEXT));
    RECT tr = *r;
    tr.left += 2 - l->xoff;
    const WCHAR *s = l->it[i].s ? l->it[i].s : L"";
    if ((w->style & LBS_USETABSTOPS) && l->ntabs) TabbedTextOutW(dc, tr.left, tr.top + 1, s, -1, l->ntabs, l->tabs, tr.left);
    else DrawTextW(dc, s, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | ((w->style & LBS_USETABSTOPS) ? DT_EXPANDTABS : 0));
    SetTextColor(dc, tc);
    if (focus && multi(w)) draw_focus(dc, r);
}

static void paint(Wnd *w, LB *l, HDC dc)
{
    RECT c = { 0, 0, client_w(w), client_h(w) };
    HBRUSH bg = ctl_color(l->combo ? l->combo : w, WM_CTLCOLORLISTBOX, dc);
    FillRect(dc, &c, bg);
    HGDIOBJ of = SelectObject(dc, lfont(l->combo ? l->combo : w));
    SetBkMode(dc, TRANSPARENT);
    int y = 0;
    for (int i = l->top; i < l->n && y < c.bottom; i++) {
        int ih = l->it[i].h ? l->it[i].h : l->ih;
        RECT r = { 0, y, c.right, y + ih };
        draw_item(w, l, dc, i, &r);
        y += ih;
    }
    SelectObject(dc, of);
}

/* -----------------------------------------------------------------------
 * The window procedure
 * ----------------------------------------------------------------------- */
static void setup_height(Wnd *w, LB *l)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, lfont(l->combo ? l->combo : w));
    int fh = font_height(dc);
    SelectObject(dc, of);
    l->ih = fh + 2;
    if (w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) {
        MEASUREITEMSTRUCT mi = { l->combo ? ODT_COMBOBOX : ODT_LISTBOX, (UINT)(l->combo ? l->combo->id : w->id), 0, 0, (UINT)l->ih, 0 };
        Wnd *dest = l->combo ? l->combo->parent : notify_target(w, l);
        if ((w->style & LBS_OWNERDRAWFIXED) && dest) {
            send_msg(dest, WM_MEASUREITEM, mi.CtlID, (LPARAM)&mi);
            if (mi.itemHeight) l->ih = (int)mi.itemHeight;
        }
    }
}

static LRESULT find(Wnd *w, LB *l, int start, const WCHAR *s, int exact)
{
    if (!l->n) return LB_ERR;
    int n = wlen(s);
    for (int k = 1; k <= l->n; k++) {
        int i = (start + k) % l->n;
        if (start < 0) i = k - 1;
        if (has_strings(w)) {
            const WCHAR *t = l->it[i].s ? l->it[i].s : L"";
            if (exact) { if (!wcsicmp_(t, s)) return i; }
            else {
                int m = 0;
                while (m < n && t[m] && (t[m] == s[m] || CharUpperW((LPWSTR)(ULONG_PTR)t[m]) == CharUpperW((LPWSTR)(ULONG_PTR)s[m]))) m++;
                if (m == n) return i;
            }
        } else if (l->it[i].data == (ULONG_PTR)s) return i;
    }
    return LB_ERR;
}

static void key(Wnd *w, LB *l, WPARAM vk)
{
    int i = multi(w) ? l->caret : l->cursel;
    int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (i < 0) i = 0;
    int ni = i;
    switch (vk) {
    case VK_UP: ni = (multi(w) || l->cursel >= 0) ? i - 1 : 0; break;
    case VK_DOWN: ni = (multi(w) || l->cursel >= 0) ? i + 1 : 0; break;
    case VK_PRIOR: ni = i - page(w, l) + 1; break;
    case VK_NEXT: ni = i + page(w, l) - 1; break;
    case VK_HOME: ni = 0; break;
    case VK_END: ni = l->n - 1; break;
    case VK_SPACE:
        if (multi(w) && l->caret >= 0 && l->caret < l->n) {
            if (w->style & LBS_MULTIPLESEL || ctrl) l->it[l->caret].sel = !l->it[l->caret].sel;
            else { for (int k = 0; k < l->n; k++) l->it[k].sel = 0; l->it[l->caret].sel = 1; l->anchor = l->caret; }
            sel_changed(w, l);
        }
        return;
    default: return;
    }
    if (ni < 0) ni = 0;
    if (ni >= l->n) ni = l->n - 1;
    if (ni < 0) return;
    if (!multi(w)) {
        if (ni != l->cursel) { set_cursel(w, l, ni); notify(w, l, LBN_SELCHANGE); }
        return;
    }
    l->caret = ni;
    if (w->style & LBS_EXTENDEDSEL) {
        if (shift) { for (int k = 0; k < l->n; k++) l->it[k].sel = 0; select_range(l, l->anchor, ni, 1); sel_changed(w, l); }
        else if (!ctrl) { for (int k = 0; k < l->n; k++) l->it[k].sel = 0; l->it[ni].sel = 1; l->anchor = ni; sel_changed(w, l); }
    }
    ensure_visible(w, l, ni);
    redraw(w, l);
}

static LRESULT lb_proc(Wnd *w, HWND h, UINT msg, WPARAM wp, LPARAM lp, int combo_list)
{
    LB *l = lb_of(w);
    if (!l && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE: {
        l = calloc(1, sizeof(LB));
        if (!l) return FALSE;
        w->ctl = l;
        l->cursel = -1;
        if (combo_list) {
            CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
            l->combo = cs ? W_quiet((HWND)cs->lpCreateParams) : NULL;
            l->hover_select = 1;
            if (l->combo) w->font = l->combo->font;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_CREATE: setup_height(w, l); update_sb(w, l); return 0;
    case WM_NCDESTROY:
        for (int i = 0; i < l->n; i++) free(l->it[i].s);
        free(l->it);
        free(l);
        w->ctl = NULL;
        return 0;
    case WM_SIZE: update_sb(w, l); set_top(w, l, l->top); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) paint(w, l, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: paint(w, l, (HDC)wp); return 0;
    case WM_SETFONT: w->font = (HFONT)wp; setup_height(w, l); for (int i = 0; i < l->n; i++) measure(w, l, i); update_sb(w, l); if (lp) redraw(w, l); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_SETREDRAW: l->no_redraw = !wp; if (wp) { update_sb(w, l); invalidate(w, NULL, TRUE, 0); } return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: l->focus = 1; redraw(w, l); notify(w, l, LBN_SETFOCUS); return 0;
    case WM_KILLFOCUS: l->focus = 0; redraw(w, l); notify(w, l, LBN_KILLFOCUS); return 0;
    case WM_ENABLE: redraw(w, l); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), out;
        int i = item_at(w, l, x, y, &out);
        if (combo_list) {
            if (out) { send_msg(l->combo, WM_USER + 0x7F01, 0, 0); return 0; }   /* clicked outside: close */
            l->tracking = 1;
            if (i >= 0) set_cursel(w, l, i);
            return 0;
        }
        set_focus(h);
        if (!W_quiet(h)) return 0;
        if (i < 0) return 0;
        if (msg == WM_LBUTTONDBLCLK) {
            if (!out) notify(w, l, LBN_DBLCLK);
            return 0;
        }
        click_select(w, l, i, (UINT)wp, 0);
        l->tracking = 1;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), out;
        if (combo_list) {
            int i = item_at(w, l, x, y, &out);
            if (!out && i >= 0 && i != l->cursel) { set_cursel(w, l, i); l->moved = 1; }
            if (!out) l->moved = 1;
            if (out && l->tracking && (y < 0 || y >= client_h(w))) {           /* dragging past the ends: scroll */
                if (y < 0 && l->top > 0) { set_top(w, l, l->top - 1); set_cursel(w, l, l->top); }
                else if (y >= client_h(w) && l->top + page(w, l) < l->n) { set_top(w, l, l->top + 1); set_cursel(w, l, MIN(l->n - 1, l->top + page(w, l) - 1)); }
            }
            return 0;
        }
        if (!l->tracking) return 0;
        int i = item_at(w, l, x, y, &out);
        if (y < 0 && l->top > 0) i = l->top - 1;
        else if (y >= client_h(w) && l->top + page(w, l) < l->n) i = l->top + page(w, l);
        if (i >= 0 && i < l->n && (w->style & LBS_MULTIPLESEL) == 0 && i != (multi(w) ? l->caret : l->cursel))
            click_select(w, l, i, (UINT)wp, 1);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (combo_list) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp), out;
            item_at(w, l, x, y, &out);
            int was = l->tracking || l->moved;
            l->tracking = 0;
            if (!out && was) send_msg(l->combo, WM_USER + 0x7F02, 0, 0);          /* chosen: close */
            return 0;
        }
        if (l->tracking) { l->tracking = 0; ReleaseCapture(); }
        return 0;
    }
    case WM_CAPTURECHANGED: if (!combo_list) l->tracking = 0; return 0;
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(wp) / 40;
        if (!d) d = (short)HIWORD(wp) > 0 ? 1 : -1;
        set_top(w, l, l->top - d);
        return 0;
    }
    case WM_VSCROLL: {
        int t = l->top;
        switch (LOWORD(wp)) {
        case SB_LINEUP: t--; break;
        case SB_LINEDOWN: t++; break;
        case SB_PAGEUP: t -= page(w, l); break;
        case SB_PAGEDOWN: t += page(w, l); break;
        case SB_TOP: t = 0; break;
        case SB_BOTTOM: t = l->n; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: t = HIWORD(wp); break;
        }
        set_top(w, l, t);
        return 0;
    }
    case WM_HSCROLL: {
        int x = l->xoff;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: x -= 8; break;
        case SB_LINERIGHT: x += 8; break;
        case SB_PAGELEFT: x -= client_w(w); break;
        case SB_PAGERIGHT: x += client_w(w); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: x = HIWORD(wp); break;
        }
        if (x > l->hext - client_w(w)) x = l->hext - client_w(w);
        if (x < 0) x = 0;
        if (x != l->xoff) { l->xoff = x; redraw(w, l); SetScrollPos(h, SB_HORZ, x, TRUE); }
        return 0;
    }
    case WM_KEYDOWN: {
        if ((w->style & LBS_WANTKEYBOARDINPUT) && w->parent) {
            LRESULT r = send_msg(w->parent, WM_VKEYTOITEM, MAKEWPARAM(wp, multi(w) ? l->caret : l->cursel), (LPARAM)h);
            if (r == -2) return 0;
            if (r >= 0) { set_cursel(w, l, (int)r); return 0; }
        }
        key(w, l, wp);
        return 0;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if ((w->style & LBS_WANTKEYBOARDINPUT) && w->parent && !has_strings(w)) {
            LRESULT r = send_msg(w->parent, WM_CHARTOITEM, MAKEWPARAM(wp, l->caret), (LPARAM)h);
            if (r >= 0) set_cursel(w, l, (int)r);
            return 0;
        }
        if (c < ' ' || !has_strings(w)) return 0;
        WCHAR s[2] = { c, 0 };
        LRESULT i = find(w, l, multi(w) ? l->caret : l->cursel, s, 0);
        if (i >= 0) {
            if (!multi(w)) { set_cursel(w, l, (int)i); notify(w, l, LBN_SELCHANGE); }
            else { l->caret = (int)i; ensure_visible(w, l, (int)i); redraw(w, l); }
        }
        return 0;
    }

    case LB_ADDSTRING: case LB_ADDFILE: return add_item(w, l, (const WCHAR *)lp);
    case LB_INSERTSTRING: return insert_item(w, l, (int)wp, (const WCHAR *)lp, 0);
    case LB_DELETESTRING:
        if ((int)wp < 0 || (int)wp >= l->n) return LB_ERR;
        delete_item(w, l, (int)wp);
        update_sb(w, l);
        redraw(w, l);
        return l->n;
    case LB_RESETCONTENT: reset(w, l); return 0;
    case LB_GETCOUNT: return l->n;
    case LB_GETTEXT: {
        int i = (int)wp;
        if (i < 0 || i >= l->n) return LB_ERR;
        if (!has_strings(w)) { *(ULONG_PTR *)lp = l->it[i].data; return sizeof(ULONG_PTR); }
        int n = wlen(l->it[i].s);
        memcpy((void *)lp, l->it[i].s, 2 * ((size_t)n + 1));
        return n;
    }
    case LB_GETTEXTLEN: {
        int i = (int)wp;
        if (i < 0 || i >= l->n) return LB_ERR;
        return has_strings(w) ? wlen(l->it[i].s) : (LRESULT)sizeof(ULONG_PTR);
    }
    case LB_GETCURSEL: return multi(w) ? (l->n ? l->caret : LB_ERR) : (l->cursel >= 0 ? l->cursel : LB_ERR);
    case LB_SETCURSEL: return set_cursel(w, l, (int)wp);
    case LB_GETSEL: return (int)wp >= 0 && (int)wp < l->n ? l->it[wp].sel : LB_ERR;
    case LB_SETSEL: {
        if (!multi(w)) return LB_ERR;
        int i = (int)lp;
        if (i == -1) { for (int k = 0; k < l->n; k++) l->it[k].sel = wp != 0; }
        else if (i >= 0 && i < l->n) { l->it[i].sel = wp != 0; if (wp) { l->caret = i; l->anchor = i; } }
        else return LB_ERR;
        redraw(w, l);
        return 0;
    }
    case LB_GETSELCOUNT: {
        if (!multi(w)) return LB_ERR;
        int c = 0;
        for (int i = 0; i < l->n; i++) c += l->it[i].sel;
        return c;
    }
    case LB_GETSELITEMS: {
        if (!multi(w)) return LB_ERR;
        int c = 0;
        for (int i = 0; i < l->n && c < (int)wp; i++) if (l->it[i].sel) ((int *)lp)[c++] = i;
        return c;
    }
    case LB_SELITEMRANGE: select_range(l, LOWORD(lp), HIWORD(lp), wp != 0); redraw(w, l); return 0;
    case LB_SELITEMRANGEEX: {
        int a = (int)wp, b = (int)lp;
        if (a <= b) select_range(l, a, b, 1); else select_range(l, b, a, 0);
        redraw(w, l);
        return 0;
    }
    case LB_FINDSTRING: return find(w, l, (int)wp, (const WCHAR *)lp, 0);
    case LB_FINDSTRINGEXACT: return find(w, l, (int)wp, (const WCHAR *)lp, 1);
    case LB_SELECTSTRING: {
        LRESULT i = find(w, l, (int)wp, (const WCHAR *)lp, 0);
        if (i >= 0) { if (multi(w)) { l->it[i].sel = 1; redraw(w, l); } else set_cursel(w, l, (int)i); }
        return i;
    }
    case LB_GETITEMDATA: return (int)wp >= 0 && (int)wp < l->n ? (LRESULT)l->it[wp].data : LB_ERR;
    case LB_SETITEMDATA:
        if ((int)wp == -1) { for (int i = 0; i < l->n; i++) l->it[i].data = (ULONG_PTR)lp; return 0; }
        if ((int)wp < 0 || (int)wp >= l->n) return LB_ERR;
        l->it[wp].data = (ULONG_PTR)lp;
        return 0;
    case LB_GETTOPINDEX: return l->top;
    case LB_SETTOPINDEX: set_top(w, l, (int)wp); return 0;
    case LB_GETITEMHEIGHT: return (int)wp >= 0 && (int)wp < l->n && l->it[wp].h ? l->it[wp].h : l->ih;
    case LB_SETITEMHEIGHT:
        if ((w->style & LBS_OWNERDRAWVARIABLE) && (int)wp >= 0 && (int)wp < l->n) l->it[wp].h = LOWORD(lp);
        else { l->ih = LOWORD(lp); for (int i = 0; i < l->n; i++) l->it[i].h = l->ih; }
        update_sb(w, l);
        redraw(w, l);
        return 0;
    case LB_GETITEMRECT: {
        int i = (int)wp;
        if (i < 0 || i >= l->n) return LB_ERR;
        RECT *r = (RECT *)lp;
        int y = i >= l->top ? item_y(l, i) : -(l->top - i) * l->ih;
        SetRect(r, 0, y, client_w(w), y + (l->it[i].h ? l->it[i].h : l->ih));
        return 1;
    }
    case LB_ITEMFROMPOINT: {
        int out;
        int i = item_at(w, l, (short)LOWORD(lp), (short)HIWORD(lp), &out);
        return MAKELONG(i < 0 ? 0xFFFF : i, out);
    }
    case LB_SETCARETINDEX: if ((int)wp >= 0 && (int)wp < l->n) { l->caret = (int)wp; ensure_visible(w, l, (int)wp); redraw(w, l); } return 0;
    case LB_GETCARETINDEX: return l->caret;
    case LB_SETANCHORINDEX: l->anchor = (int)wp; return 0;
    case LB_GETANCHORINDEX: return l->anchor;
    case LB_SETHORIZONTALEXTENT:
        l->hext = (int)wp;
        if (w->style & WS_HSCROLL) {
            SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE, 0, l->hext, (UINT)client_w(w), 0, 0 };
            SetScrollInfo(h, SB_HORZ, &si, TRUE);
        }
        return 0;
    case LB_GETHORIZONTALEXTENT: return l->hext;
    case LB_SETCOLUMNWIDTH: l->colw = (int)wp; return 0;
    case LB_INITSTORAGE: return l->n + (int)wp;
    case LB_SETTABSTOPS: {
        l->ntabs = MIN((int)wp, 32);
        int bu = LOWORD(GetDialogBaseUnits());
        for (int i = 0; i < l->ntabs; i++) l->tabs[i] = ((const int *)lp)[i] * bu / 4;
        if (!wp) { l->ntabs = 1; l->tabs[0] = 32 * bu / 4; }
        return TRUE;
    }
    case LB_GETLISTBOXINFO: return page(w, l);
    case LB_SETCOUNT: {
        if (!(w->style & LBS_NODATA)) return LB_ERR;
        while (l->n < (int)wp) insert_item(w, l, l->n, NULL, 0);
        while (l->n > (int)wp) delete_item(w, l, l->n - 1);
        return 0;
    }
    case LB_DIR: return LB_ERR;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK ListBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    return lb_proc(w, h, msg, wp, lp, 0);
}

LRESULT CALLBACK ComboLBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return lb_proc(w, h, msg, wp, lp, 1);
}

USERAPI int DlgDirListW(HWND h, LPWSTR path, int lb, int st, UINT type) { (void)h; (void)path; (void)lb; (void)st; (void)type; return 0; }
USERAPI BOOL DlgDirSelectExW(HWND h, LPWSTR s, int n, int id) { (void)h; (void)s; (void)n; (void)id; return FALSE; }
USERAPI DWORD GetListBoxInfo(HWND h) { return (DWORD)SendMessageW(h, LB_GETLISTBOXINFO, 0, 0); }
