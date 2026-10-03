/*
 * listview.c — the list view (SysListView32).
 *
 * Four views: details (report: rows under a header), list (columns of
 * small-icon labels, scrolling sideways), small icons and large icons
 * (rows of cells). Items carry text per column, an image, an lParam and
 * a state; text and images can come from the parent (LPSTR_TEXTCALLBACK,
 * I_IMAGECALLBACK, LVN_GETDISPINFO), and with LVS_OWNERDATA the parent
 * holds everything but the selection. Selection follows the Windows rules
 * for clicks with Ctrl/Shift and the navigation keys; there is
 * incremental search by typing, label editing, sorting, custom draw and
 * the usual notifications (LVN_ITEMCHANGED, NM_CLICK, NM_DBLCLK,
 * LVN_ITEMACTIVATE, LVN_COLUMNCLICK, LVN_KEYDOWN, LVN_BEGINDRAG ...).
 */
#include "cc.h"

#define LVM_INSERTCOLUMNA_  (LVM_FIRST + 27)
#define LVM_CANCELEDITLABEL_ (LVM_FIRST + 179)
#define LVM_SETUNICODEFORMAT_ 0x2005
#define LVM_GETUNICODEFORMAT_ 0x2006
#define LVN_BEGINLABELEDITA_ (LVN_FIRST - 5)
#define LVN_ENDLABELEDITA_   (LVN_FIRST - 6)
#define LVN_GETDISPINFOA_    (LVN_FIRST - 50)
#define CDIS_SELECTED_ 0x0001
#define CDIS_FOCUS_    0x0010
#define NF_QUERY_ 3
#define EDIT_TIMER 0x4C56

typedef struct {
    LPARAM lp;
    UINT state;                         /* LVIS_* except LVIS_FOCUSED (that is lv->focus) */
    int image, indent;
    WCHAR **sub;                        /* text per subitem ([0] = the item's), NULL = none */
    int nsub;
} LItem;

typedef struct { int fmt, cx, sub, image; } LCol;

typedef struct {
    HWND hdr;
    LItem *it;
    int n, cap;
    BYTE *osel;                         /* LVS_OWNERDATA: the selection, one byte per item */
    int ocap;
    LCol *col;
    int ncol, ccap;
    HIMAGELIST il[3];                   /* normal, small, state */
    HFONT font;
    int fh;                             /* font height */
    COLORREF bk, text, textbk;
    DWORD ex;
    UINT cbmask;
    int focus, mark;
    int top;                            /* details: first row shown; list: first column shown */
    int sx, sy;                         /* scroll in pixels: details horizontal; icon views vertical */
    HWND edit;
    int edit_item, edit_pending;
    int ansi;                           /* the parent wants ANSI notifications */
    int down, down_item, down_was_sel, dragging;
    POINT down_pt;
    WCHAR search[64];
    int nsearch;
    DWORD search_tick;
    int hot;
    HWND tips;
} LV;

static LRESULT CALLBACK edit_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static void end_edit(HWND h, LV *s, BOOL cancel);

/* -----------------------------------------------------------------------
 * Basics
 * ----------------------------------------------------------------------- */
static DWORD style_of(HWND h) { return (DWORD)GetWindowLongW(h, GWL_STYLE); }
static int view_of(HWND h) { return style_of(h) & LVS_TYPEMASK; }
static int ownerdata(HWND h) { return (style_of(h) & LVS_OWNERDATA) != 0; }

static void icon_size(LV *s, int which, int *cx, int *cy)
{
    *cx = *cy = which == LVSIL_NORMAL ? 32 : 16;
    if (s->il[which]) ImageList_GetIconSize(s->il[which], cx, cy);
}

static int row_h(HWND hw, LV *s)
{
    int ix, iy, k = cc_k(hw);
    icon_size(s, LVSIL_SMALL, &ix, &iy);
    int h = s->fh + 6 * k;
    if (s->il[LVSIL_SMALL] && iy + 4 * k > h) h = iy + 4 * k;
    if (s->il[LVSIL_STATE]) { int sx, sy; icon_size(s, LVSIL_STATE, &sx, &sy); if (sy + 4 * k > h) h = sy + 4 * k; }
    return h < 18 * k ? 18 * k : h;
}

static int hdr_h(HWND h, LV *s)
{
    if (!s->hdr || !IsWindowVisible(s->hdr)) return 0;
    RECT r;
    GetWindowRect(s->hdr, &r);
    (void)h;
    return r.bottom - r.top;
}

static void client(HWND h, RECT *c) { GetClientRect(h, c); }

static BOOL selected(HWND h, LV *s, int i)
{
    if (i < 0 || i >= s->n) return FALSE;
    if (ownerdata(h)) return s->osel && s->osel[i];
    return (s->it[i].state & LVIS_SELECTED) != 0;
}

static UINT get_state(HWND h, LV *s, int i, UINT mask)
{
    if (i < 0 || i >= s->n) return 0;
    UINT st = ownerdata(h) ? (selected(h, s, i) ? LVIS_SELECTED : 0) : s->it[i].state;
    if (i == s->focus) st |= LVIS_FOCUSED;
    return st & mask;
}

static int total_width(LV *s)
{
    int w = 0;
    for (int i = 0; i < s->ncol; i++) w += s->col[i].cx;
    return w;
}

/* column indices in display order (malloc) */
static int *col_order(LV *s)
{
    int *o = malloc(sizeof(int) * (size_t)(s->ncol ? s->ncol : 1));
    if (!o) return NULL;
    for (int i = 0; i < s->ncol; i++) o[i] = i;
    if (s->hdr && s->ncol) SendMessageW(s->hdr, HDM_GETORDERARRAY, (WPARAM)s->ncol, (LPARAM)o);
    return o;
}

static int col_x(LV *s, int c)          /* the column's left edge in content coordinates */
{
    int *o = col_order(s), x = 0;
    for (int p = 0; o && p < s->ncol; p++) { if (o[p] == c) break; x += s->col[o[p]].cx; }
    free(o);
    return x;
}

/* -----------------------------------------------------------------------
 * Text and images, from the item or from the parent
 * ----------------------------------------------------------------------- */
static LRESULT notify_lv(HWND h, UINT code, int i, int sub, UINT newst, UINT oldst, UINT changed, LPARAM lp)
{
    NMLISTVIEW nm;
    memset(&nm, 0, sizeof(nm));
    nm.iItem = i; nm.iSubItem = sub;
    nm.uNewState = newst; nm.uOldState = oldst; nm.uChanged = changed;
    nm.lParam = lp;
    return cc_notify(h, code, &nm.hdr);
}

static LPARAM item_lparam(HWND h, LV *s, int i)
{
    return !ownerdata(h) && i >= 0 && i < s->n ? s->it[i].lp : 0;
}

/* LVN_GETDISPINFO for MASK; results land in *out */
static void dispinfo(HWND h, LV *s, int i, int sub, UINT mask, LVITEMW *out, WCHAR *buf, int cap)
{
    NMLVDISPINFOW di;
    memset(&di, 0, sizeof(di));
    di.item.mask = mask;
    di.item.iItem = i;
    di.item.iSubItem = sub;
    di.item.lParam = item_lparam(h, s, i);
    di.item.stateMask = s->cbmask;
    di.item.iImage = I_IMAGECALLBACK;
    char abuf[520];
    if (mask & LVIF_TEXT) {
        if (s->ansi) { abuf[0] = 0; di.item.pszText = (LPWSTR)abuf; di.item.cchTextMax = (int)sizeof(abuf); }
        else { buf[0] = 0; di.item.pszText = buf; di.item.cchTextMax = cap; }
    }
    cc_notify(h, s->ansi ? LVN_GETDISPINFOA_ : LVN_GETDISPINFOW, &di.hdr);
    *out = di.item;
    if (mask & LVIF_TEXT) {
        if (!di.item.pszText) { buf[0] = 0; out->pszText = buf; }
        else if (s->ansi) {
            if (!MultiByteToWideChar(CP_ACP, 0, (const char *)di.item.pszText, -1, buf, cap)) buf[0] = 0;
            out->pszText = buf;
        }
    }
}

static const WCHAR *item_text(HWND h, LV *s, int i, int sub, WCHAR *buf, int cap)
{
    if (i < 0 || i >= s->n) return L"";
    const WCHAR *t = NULL;
    if (!ownerdata(h)) {
        LItem *it = &s->it[i];
        t = sub < it->nsub ? it->sub[sub] : NULL;
        if (!t && sub > 0) t = LPSTR_TEXTCALLBACKW;         /* a subitem never set comes from the parent */
        if (t != LPSTR_TEXTCALLBACKW) return t ? t : L"";
    }
    LVITEMW r;
    dispinfo(h, s, i, sub, LVIF_TEXT, &r, buf, cap);
    return r.pszText ? r.pszText : L"";
}

static int item_image(HWND h, LV *s, int i)
{
    if (i < 0 || i >= s->n) return -1;
    if (!ownerdata(h) && s->it[i].image != I_IMAGECALLBACK) return s->it[i].image;
    WCHAR b[2];
    LVITEMW r;
    dispinfo(h, s, i, 0, LVIF_IMAGE, &r, b, 2);
    return r.iImage;
}

/* -----------------------------------------------------------------------
 * Layout: rectangles in client coordinates
 * ----------------------------------------------------------------------- */
typedef struct { int cw, ch, per; } Grid;     /* a cell's size, cells per row (icons) or per column (list) */

static int max_label_w(HWND h, LV *s)
{
    HDC dc = GetDC(h);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    int w = 40;
    WCHAR b[260];
    int n = s->n > 2000 ? 2000 : s->n;          /* long lists: estimate from the first ones */
    for (int i = 0; i < n; i++) { int t = cc_text_w(dc, item_text(h, s, i, 0, b, 260), -1); if (t > w) w = t; }
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    return w;
}

static void grid(HWND h, LV *s, Grid *g)
{
    RECT c;
    client(h, &c);
    int v = view_of(h), ix, iy, k = cc_k(h);
    if (v == LVS_ICON) {
        icon_size(s, LVSIL_NORMAL, &ix, &iy);
        g->cw = MAX(ix + 44 * k, 76 * k);
        g->ch = iy + 8 * k + 2 * s->fh + 6 * k;
        g->per = MAX(1, (c.right - c.left) / g->cw);
    } else if (v == LVS_SMALLICON) {
        icon_size(s, LVSIL_SMALL, &ix, &iy);
        g->cw = MIN(max_label_w(h, s), 300 * k) + ix + 16 * k;
        g->ch = row_h(h, s);
        g->per = MAX(1, (c.right - c.left) / g->cw);
    } else if (v == LVS_LIST) {
        icon_size(s, LVSIL_SMALL, &ix, &iy);
        g->cw = MIN(max_label_w(h, s), 300 * k) + ix + 16 * k;
        g->ch = row_h(h, s);
        g->per = MAX(1, (c.bottom - c.top) / g->ch);
    } else {
        g->cw = total_width(s);
        g->ch = row_h(h, s);
        g->per = 1;
    }
}

/* the whole item */
static BOOL item_bounds(HWND h, LV *s, int i, RECT *r)
{
    if (i < 0 || i >= s->n) return FALSE;
    Grid g;
    grid(h, s, &g);
    switch (view_of(h)) {
    case LVS_REPORT: {
        int y = hdr_h(h, s) + (i - s->top) * g.ch;
        SetRect(r, -s->sx, y, -s->sx + total_width(s), y + g.ch);
        return TRUE;
    }
    case LVS_LIST: {
        int cx = i / g.per - s->top, cy = i % g.per;
        SetRect(r, cx * g.cw, cy * g.ch, cx * g.cw + g.cw, cy * g.ch + g.ch);
        return TRUE;
    }
    default: {
        int cx = i % g.per, cy = i / g.per;
        SetRect(r, cx * g.cw, cy * g.ch - s->sy, cx * g.cw + g.cw, cy * g.ch + g.ch - s->sy);
        return TRUE;
    }
    }
}

/* the parts: LVIR_ICON, LVIR_LABEL, LVIR_BOUNDS, LVIR_SELECTBOUNDS; state image left of the icon */
static BOOL item_part(HWND h, LV *s, int i, int part, RECT *out)
{
    RECT b;
    if (!item_bounds(h, s, i, &b)) return FALSE;
    int v = view_of(h), ix, iy, sw = 0, sh, k = cc_k(h);
    if (s->il[LVSIL_STATE]) { icon_size(s, LVSIL_STATE, &sw, &sh); sw += 2 * k; }
    else if (s->ex & LVS_EX_CHECKBOXES) sw = 18 * k;
    RECT icon, label;
    if (v == LVS_ICON) {
        icon_size(s, LVSIL_NORMAL, &ix, &iy);
        SetRect(&icon, (b.left + b.right - ix) / 2, b.top + 4 * k, (b.left + b.right + ix) / 2, b.top + 4 * k + iy);
        SetRect(&label, b.left + 2 * k, icon.bottom + 2 * k, b.right - 2 * k, b.bottom - 2 * k);
    } else {
        icon_size(s, LVSIL_SMALL, &ix, &iy);
        int x = b.left + 2 * k + sw;
        if (v == LVS_REPORT) {
            int c0 = s->ncol ? col_x(s, 0) : 0;
            x = -s->sx + c0 + 2 * k + sw;
            if (!ownerdata(h)) x += s->it[i].indent * ix;
        }
        if (!s->il[LVSIL_SMALL]) ix = 0;
        SetRect(&icon, x, b.top, x + ix, b.bottom);
        int right = v == LVS_REPORT ? -s->sx + (s->ncol ? col_x(s, 0) + s->col[0].cx : total_width(s)) : b.right;
        SetRect(&label, icon.right + (ix ? 4 : 2) * k, b.top, right - 2 * k, b.bottom);
        if (label.right < label.left) label.right = label.left;
    }
    switch (part) {
    case LVIR_ICON: *out = icon; break;
    case LVIR_LABEL: *out = label; break;
    case LVIR_SELECTBOUNDS:
        if (v == LVS_REPORT && !(s->ex & LVS_EX_FULLROWSELECT)) UnionRect(out, &icon, &label);
        else *out = b;
        break;
    default: *out = b; break;
    }
    return TRUE;
}

static BOOL subitem_rect(HWND h, LV *s, int i, int sub, int part, RECT *r)
{
    if (view_of(h) != LVS_REPORT || sub < 0 || sub >= MAX(s->ncol, 1)) return FALSE;
    if (sub == 0) {
        if (!item_part(h, s, i, part == LVIR_BOUNDS ? LVIR_BOUNDS : part, r)) return FALSE;
        if (part == LVIR_BOUNDS && s->ncol) { r->left = -s->sx + col_x(s, 0); r->right = r->left + s->col[0].cx; }
        return TRUE;
    }
    RECT b;
    if (!item_bounds(h, s, i, &b)) return FALSE;
    int x = -s->sx + col_x(s, sub);
    SetRect(r, x, b.top, x + s->col[sub].cx, b.bottom);
    return TRUE;
}

static int hit_test(HWND h, LV *s, POINT pt, UINT *flags, int *sub)
{
    RECT c;
    client(h, &c);
    if (flags) *flags = LVHT_NOWHERE;
    if (sub) *sub = 0;
    if (pt.y < hdr_h(h, s)) { if (flags) *flags = LVHT_ABOVE; return -1; }
    if (!PtInRect(&c, pt)) {
        if (flags) *flags = pt.y >= c.bottom ? LVHT_BELOW : pt.x >= c.right ? LVHT_TORIGHT : LVHT_TOLEFT;
        return -1;
    }
    int v = view_of(h), i = -1;
    Grid g;
    grid(h, s, &g);
    if (v == LVS_REPORT) {
        i = s->top + (pt.y - hdr_h(h, s)) / g.ch;
        if (pt.x + s->sx >= total_width(s) && s->ncol) i = -1;
    } else if (v == LVS_LIST) {
        int col = pt.x / g.cw + s->top, row = pt.y / g.ch;
        if (row < g.per) i = col * g.per + row;
    } else {
        int col = pt.x / g.cw, row = (pt.y + s->sy) / g.ch;
        if (col < g.per) i = row * g.per + col;
    }
    if (i < 0 || i >= s->n) return -1;
    RECT icon, label, st;
    item_part(h, s, i, LVIR_ICON, &icon);
    item_part(h, s, i, LVIR_LABEL, &label);
    SetRect(&st, icon.left - 18 * cc_k(h), icon.top, icon.left, icon.bottom);
    UINT f = 0;
    if (PtInRect(&icon, pt)) f = LVHT_ONITEMICON;
    else if (PtInRect(&st, pt) && (s->il[LVSIL_STATE] || (s->ex & LVS_EX_CHECKBOXES))) f = LVHT_ONITEMSTATEICON;
    else if (v == LVS_REPORT) {
        if (sub) {
            int *o = col_order(s), x = -s->sx;
            for (int p = 0; o && p < s->ncol; p++) { if (pt.x >= x && pt.x < x + s->col[o[p]].cx) { *sub = o[p]; break; } x += s->col[o[p]].cx; }
            free(o);
        }
        if (PtInRect(&label, pt) || (s->ex & LVS_EX_FULLROWSELECT)) f = LVHT_ONITEMLABEL;
        else { RECT b; item_bounds(h, s, i, &b); f = PtInRect(&b, pt) ? LVHT_ONITEMLABEL : 0; if (!(s->ex & LVS_EX_FULLROWSELECT) && pt.x > label.right) f = 0; }
    } else {
        RECT b;
        item_bounds(h, s, i, &b);
        if (v == LVS_ICON) { if (PtInRect(&label, pt)) f = LVHT_ONITEMLABEL; }
        else if (PtInRect(&b, pt)) f = LVHT_ONITEMLABEL;
    }
    if (!f) return -1;
    if (flags) *flags = f;
    return i;
}

/* -----------------------------------------------------------------------
 * Scrolling
 * ----------------------------------------------------------------------- */
static int rows_visible(HWND h, LV *s)
{
    RECT c;
    client(h, &c);
    int n = (c.bottom - hdr_h(h, s)) / row_h(h, s);
    return n < 1 ? 1 : n;
}

static void place_header(HWND h, LV *s)
{
    if (!s->hdr) return;
    RECT c;
    client(h, &c);
    if (view_of(h) != LVS_REPORT || (style_of(h) & LVS_NOCOLUMNHEADER)) { ShowWindow(s->hdr, SW_HIDE); return; }
    WINDOWPOS wp;
    RECT r = c;
    HDLAYOUT l = { &r, &wp };
    SendMessageW(s->hdr, HDM_LAYOUT, 0, (LPARAM)&l);
    SetWindowPos(s->hdr, NULL, -s->sx, wp.y, MAX(c.right, total_width(s)) + s->sx, wp.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void update_scroll(HWND h, LV *s)
{
    if (style_of(h) & LVS_NOSCROLL) return;
    RECT c;
    client(h, &c);
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0 };
    SCROLLINFO none = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0 };
    Grid g;
    grid(h, s, &g);
    switch (view_of(h)) {
    case LVS_REPORT: {
        int vis = rows_visible(h, s);
        if (s->top > MAX(0, s->n - vis)) s->top = MAX(0, s->n - vis);
        si.nMax = s->n - 1; si.nPage = (UINT)vis; si.nPos = s->top;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        client(h, &c);
        int tw = total_width(s);
        if (s->sx > MAX(0, tw - c.right)) s->sx = MAX(0, tw - c.right);
        si.nMax = tw - 1; si.nPage = (UINT)c.right; si.nPos = s->sx;
        SetScrollInfo(h, SB_HORZ, &si, TRUE);
        place_header(h, s);
        break;
    }
    case LVS_LIST: {
        SetScrollInfo(h, SB_VERT, &none, TRUE);
        int cols = (s->n + g.per - 1) / g.per, vis = MAX(1, c.right / g.cw);
        if (s->top > MAX(0, cols - vis)) s->top = MAX(0, cols - vis);
        si.nMax = cols - 1; si.nPage = (UINT)vis; si.nPos = s->top;
        SetScrollInfo(h, SB_HORZ, &si, TRUE);
        grid(h, s, &g);                           /* a scroll bar changes the rows per column */
        place_header(h, s);
        break;
    }
    default: {
        SetScrollInfo(h, SB_HORZ, &none, TRUE);
        int total = ((s->n + g.per - 1) / g.per) * g.ch;
        if (s->sy > MAX(0, total - c.bottom)) s->sy = MAX(0, total - c.bottom);
        si.nMax = total - 1; si.nPage = (UINT)c.bottom; si.nPos = s->sy;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        place_header(h, s);
        break;
    }
    }
}

static void refresh(HWND h, LV *s) { update_scroll(h, s); InvalidateRect(h, NULL, TRUE); }

static void scroll_to(HWND h, LV *s, int bar, int pos)
{
    int v = view_of(h);
    if (v == LVS_REPORT && bar == SB_VERT) s->top = MAX(0, pos);
    else if (v == LVS_REPORT) s->sx = MAX(0, pos);
    else if (v == LVS_LIST) s->top = MAX(0, pos);
    else s->sy = MAX(0, pos);
    end_edit(h, s, FALSE);
    refresh(h, s);
}

static void ensure_visible(HWND h, LV *s, int i, BOOL partial)
{
    if (i < 0 || i >= s->n) return;
    RECT c;
    client(h, &c);
    Grid g;
    grid(h, s, &g);
    switch (view_of(h)) {
    case LVS_REPORT: {
        int vis = rows_visible(h, s);
        if (i < s->top) s->top = i;
        else if (i >= s->top + vis) s->top = i - vis + 1;
        else return;
        break;
    }
    case LVS_LIST: {
        int col = i / g.per, vis = MAX(1, c.right / g.cw);
        if (col < s->top) s->top = col;
        else if (col >= s->top + vis) s->top = col - vis + 1;
        else return;
        break;
    }
    default: {
        int y = (i / g.per) * g.ch;
        if (y < s->sy) s->sy = y;
        else if (y + g.ch > s->sy + c.bottom && !(partial && y < s->sy + c.bottom)) s->sy = y + g.ch - c.bottom;
        else return;
        break;
    }
    }
    refresh(h, s);
}

static void on_scroll(HWND h, LV *s, int bar, int code)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL, 0, 0, 0, 0, 0 };
    GetScrollInfo(h, bar, &si);
    int v = view_of(h), line = (v == LVS_REPORT && bar == SB_VERT) || v == LVS_LIST ? 1 : v == LVS_REPORT ? 16 : row_h(h, s);
    int pos = si.nPos;
    switch (code) {
    case SB_LINEUP: pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP: pos -= MAX(1, (int)si.nPage); break;
    case SB_PAGEDOWN: pos += MAX(1, (int)si.nPage); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP: pos = si.nMin; break;
    case SB_BOTTOM: pos = si.nMax; break;
    default: return;
    }
    int mx = si.nMax - MAX((int)si.nPage - 1, 0);
    if (pos > mx) pos = mx;
    if (pos < si.nMin) pos = si.nMin;
    if (pos != si.nPos) scroll_to(h, s, bar, pos);
}

/* -----------------------------------------------------------------------
 * Selection and focus
 * ----------------------------------------------------------------------- */
static void inval_item(HWND h, LV *s, int i)
{
    RECT r, c;
    if (!item_bounds(h, s, i, &r)) return;
    client(h, &c);
    if (view_of(h) == LVS_REPORT) { r.left = 0; r.right = c.right; }
    InvalidateRect(h, &r, TRUE);
}

/* change item I's state bits; I = -1 is every item */
static void set_state(HWND h, LV *s, int i, UINT state, UINT mask)
{
    if (i == -1) {
        if (ownerdata(h) && (mask & LVIS_SELECTED)) {
            UINT st = state & LVIS_SELECTED;
            for (int k = 0; k < s->n; k++) s->osel[k] = st ? 1 : 0;
            notify_lv(h, LVN_ITEMCHANGED, -1, 0, st, st ? 0 : LVIS_SELECTED, LVIF_STATE, 0);
            mask &= ~LVIS_SELECTED;
            InvalidateRect(h, NULL, TRUE);
        }
        if (mask) for (int k = 0; k < s->n; k++) set_state(h, s, k, state, mask);
        return;
    }
    if (i < 0 || i >= s->n) return;
    if ((mask & LVIS_SELECTED) && (state & LVIS_SELECTED) && (style_of(h) & LVS_SINGLESEL)) {
        for (int k = 0; k < s->n; k++) if (k != i && selected(h, s, k)) set_state(h, s, k, 0, LVIS_SELECTED);
    }
    UINT old = get_state(h, s, i, ~0u);
    UINT nw = (old & ~mask) | (state & mask);
    if (nw == old) return;
    LPARAM lp = item_lparam(h, s, i);
    if (!ownerdata(h) && notify_lv(h, LVN_ITEMCHANGING, i, 0, nw, old, LVIF_STATE, lp)) return;
    if ((mask & LVIS_FOCUSED) && (nw & LVIS_FOCUSED) && s->focus != i && s->focus >= 0) {
        int f = s->focus;
        UINT fo = get_state(h, s, f, ~0u);
        s->focus = -1;
        inval_item(h, s, f);
        notify_lv(h, LVN_ITEMCHANGED, f, 0, fo & ~LVIS_FOCUSED, fo, LVIF_STATE, item_lparam(h, s, f));
    }
    if (mask & LVIS_FOCUSED) {
        if (nw & LVIS_FOCUSED) s->focus = i;
        else if (s->focus == i) s->focus = -1;
    }
    if (ownerdata(h)) { if (mask & LVIS_SELECTED) s->osel[i] = (nw & LVIS_SELECTED) != 0; }
    else s->it[i].state = nw & ~LVIS_FOCUSED;
    inval_item(h, s, i);
    notify_lv(h, LVN_ITEMCHANGED, i, 0, nw, old, LVIF_STATE, lp);
}

static void deselect_all(HWND h, LV *s, int except)
{
    if (ownerdata(h)) {
        int any = 0;
        for (int k = 0; k < s->n; k++) if (s->osel[k] && k != except) { s->osel[k] = 0; any = 1; }
        if (any) { notify_lv(h, LVN_ITEMCHANGED, -1, 0, 0, LVIS_SELECTED, LVIF_STATE, 0); InvalidateRect(h, NULL, TRUE); }
        return;
    }
    for (int k = 0; k < s->n; k++) if (k != except && selected(h, s, k)) set_state(h, s, k, 0, LVIS_SELECTED);
}

static void select_range(HWND h, LV *s, int a, int b, BOOL keep_others)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (ownerdata(h)) {
        if (!keep_others) for (int k = 0; k < s->n; k++) s->osel[k] = 0;
        for (int k = a; k <= b && k < s->n; k++) s->osel[k] = 1;
        NMLVODSTATECHANGE nm;
        memset(&nm, 0, sizeof(nm));
        nm.iFrom = a; nm.iTo = b; nm.uNewState = LVIS_SELECTED; nm.uOldState = 0;
        cc_notify(h, LVN_ODSTATECHANGED, &nm.hdr);
        notify_lv(h, LVN_ITEMCHANGED, -1, 0, LVIS_SELECTED, 0, LVIF_STATE, 0);
        InvalidateRect(h, NULL, TRUE);
        return;
    }
    for (int k = 0; k < s->n; k++) {
        BOOL in = k >= a && k <= b;
        if (in) set_state(h, s, k, LVIS_SELECTED, LVIS_SELECTED);
        else if (!keep_others && selected(h, s, k)) set_state(h, s, k, 0, LVIS_SELECTED);
    }
}

/* the keyboard or a click moved to I */
static void move_to(HWND h, LV *s, int i, BOOL shift, BOOL ctrl)
{
    if (i < 0 || i >= s->n) return;
    if (style_of(h) & LVS_SINGLESEL) shift = ctrl = FALSE;
    if (shift) {
        if (s->mark < 0) s->mark = s->focus >= 0 ? s->focus : 0;
        select_range(h, s, s->mark, i, ctrl);
        set_state(h, s, i, LVIS_FOCUSED, LVIS_FOCUSED);
    } else if (ctrl) {
        set_state(h, s, i, LVIS_FOCUSED, LVIS_FOCUSED);
    } else {
        deselect_all(h, s, i);
        set_state(h, s, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        s->mark = i;
    }
    ensure_visible(h, s, i, FALSE);
}

/* -----------------------------------------------------------------------
 * Items and columns
 * ----------------------------------------------------------------------- */
static void set_sub_text(LItem *it, int sub, const void *t, int wide)
{
    if (sub < 0 || sub > 255) return;
    if (sub >= it->nsub) {
        WCHAR **p = realloc(it->sub, sizeof(WCHAR *) * (size_t)(sub + 1));
        if (!p) return;
        for (int k = it->nsub; k <= sub; k++) p[k] = NULL;
        it->sub = p;
        it->nsub = sub + 1;
    }
    wfree(it->sub[sub]);
    it->sub[sub] = NULL;
    if (!t) return;
    if (wide || t == (const void *)LPSTR_TEXTCALLBACKW) { it->sub[sub] = wdup(t); return; }
    int n = MultiByteToWideChar(CP_ACP, 0, t, -1, NULL, 0);
    WCHAR *w = malloc(2 * (size_t)(n > 0 ? n : 1));
    if (w) { if (n > 0) MultiByteToWideChar(CP_ACP, 0, t, -1, w, n); else w[0] = 0; }
    it->sub[sub] = w;
}

static void free_item(LItem *it)
{
    for (int k = 0; k < it->nsub; k++) wfree(it->sub[k]);
    free(it->sub);
    it->sub = NULL;
    it->nsub = 0;
}

static int text_cmp(const WCHAR *a, const WCHAR *b)
{
    int r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a, -1, b, -1);
    return r ? r - 2 : 0;
}

static int insert_item(HWND h, LV *s, const LVITEMW *in, int wide)
{
    if (ownerdata(h) || !in) return -1;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 64;
        LItem *p = realloc(s->it, sizeof(LItem) * (size_t)c);
        if (!p) return -1;
        s->it = p; s->cap = c;
    }
    LItem it;
    memset(&it, 0, sizeof(it));
    it.image = (in->mask & LVIF_IMAGE) ? in->iImage : 0;
    if (in->mask & LVIF_PARAM) it.lp = in->lParam;
    if (in->mask & LVIF_INDENT) it.indent = in->iIndent;
    if (in->mask & LVIF_STATE) it.state = in->state & in->stateMask & ~LVIS_FOCUSED;
    if (in->mask & LVIF_TEXT) set_sub_text(&it, 0, in->pszText, wide);
    int at = in->iItem;
    if (at < 0 || at > s->n) at = s->n;
    DWORD st = style_of(h);
    if ((st & (LVS_SORTASCENDING | LVS_SORTDESCENDING)) && it.nsub && it.sub[0] && it.sub[0] != LPSTR_TEXTCALLBACKW) {
        int desc = (st & LVS_SORTDESCENDING) != 0;
        at = 0;
        while (at < s->n) {
            const WCHAR *o = s->it[at].nsub && s->it[at].sub[0] && s->it[at].sub[0] != LPSTR_TEXTCALLBACKW ? s->it[at].sub[0] : L"";
            int c = text_cmp(it.sub[0], o);
            if (desc ? c > 0 : c < 0) break;
            at++;
        }
    }
    memmove(&s->it[at + 1], &s->it[at], sizeof(LItem) * (size_t)(s->n - at));
    s->it[at] = it;
    s->n++;
    if (s->focus >= at) s->focus++;
    if (s->mark >= at) s->mark++;
    if ((in->mask & LVIF_STATE) && (in->stateMask & in->state & LVIS_FOCUSED)) s->focus = at;
    notify_lv(h, LVN_INSERTITEM, at, 0, 0, 0, 0, it.lp);
    refresh(h, s);
    return at;
}

static BOOL delete_item(HWND h, LV *s, int i, BOOL notify)
{
    if (i < 0 || i >= s->n) return FALSE;
    if (s->edit && s->edit_item == i) end_edit(h, s, TRUE);
    if (ownerdata(h)) return TRUE;
    if (notify) notify_lv(h, LVN_DELETEITEM, i, 0, 0, 0, 0, s->it[i].lp);
    if (i >= s->n) return FALSE;
    free_item(&s->it[i]);
    memmove(&s->it[i], &s->it[i + 1], sizeof(LItem) * (size_t)(s->n - i - 1));
    s->n--;
    if (s->focus == i) s->focus = -1; else if (s->focus > i) s->focus--;
    if (s->mark == i) s->mark = -1; else if (s->mark > i) s->mark--;
    return TRUE;
}

static void delete_all(HWND h, LV *s)
{
    end_edit(h, s, TRUE);
    if (!ownerdata(h)) {
        BOOL quiet = notify_lv(h, LVN_DELETEALLITEMS, -1, 0, 0, 0, 0, 0) != 0;
        for (int i = s->n - 1; i >= 0; i--) {
            if (!quiet) notify_lv(h, LVN_DELETEITEM, i, 0, 0, 0, 0, s->it[i].lp);
            if (i < s->n) free_item(&s->it[i]);
        }
    }
    s->n = 0;
    s->focus = s->mark = -1;
    s->top = 0; s->sy = 0;
    refresh(h, s);
}

static BOOL set_item(HWND h, LV *s, const LVITEMW *in, int wide)
{
    if (!in) return FALSE;
    int i = in->iItem;
    if (i < 0 || i >= s->n) return FALSE;
    if (in->mask & LVIF_STATE) set_state(h, s, i, in->state, in->stateMask);
    if (ownerdata(h)) { inval_item(h, s, i); return TRUE; }
    LItem *it = &s->it[i];
    if (in->iSubItem) {
        if (in->mask & LVIF_TEXT) set_sub_text(it, in->iSubItem, in->pszText, wide);
    } else {
        if (in->mask & LVIF_TEXT) set_sub_text(it, 0, in->pszText, wide);
        if (in->mask & LVIF_IMAGE) it->image = in->iImage;
        if (in->mask & LVIF_PARAM) it->lp = in->lParam;
        if (in->mask & LVIF_INDENT) it->indent = in->iIndent;
    }
    if (in->mask & (LVIF_TEXT | LVIF_IMAGE | LVIF_INDENT)) inval_item(h, s, i);
    return TRUE;
}

static void copy_out(WCHAR *dst, int cap, const WCHAR *src, int wide)
{
    if (!dst || cap <= 0) return;
    if (!src) src = L"";
    if (wide) {
        int n = MIN(wlen(src), cap - 1);
        memcpy(dst, src, 2 * (size_t)n);
        dst[n] = 0;
    } else if (!WideCharToMultiByte(CP_ACP, 0, src, -1, (char *)dst, cap, NULL, NULL)) {
        ((char *)dst)[cap - 1] = 0;
    }
}

static BOOL get_item(HWND h, LV *s, LVITEMW *out, int wide)
{
    if (!out) return FALSE;
    int i = out->iItem;
    if (i < 0 || i >= s->n) return FALSE;
    if (out->mask & LVIF_STATE) out->state = get_state(h, s, i, out->stateMask);
    WCHAR buf[520];
    if (out->mask & LVIF_TEXT) copy_out(out->pszText, out->cchTextMax, item_text(h, s, i, out->iSubItem, buf, 520), wide);
    if (ownerdata(h)) {
        if (out->mask & (LVIF_IMAGE | LVIF_PARAM | LVIF_INDENT)) {
            LVITEMW r;
            dispinfo(h, s, i, out->iSubItem, out->mask & (LVIF_IMAGE | LVIF_PARAM | LVIF_INDENT), &r, buf, 2);
            if (out->mask & LVIF_IMAGE) out->iImage = r.iImage;
            if (out->mask & LVIF_PARAM) out->lParam = r.lParam;
            if (out->mask & LVIF_INDENT) out->iIndent = r.iIndent;
        }
        return TRUE;
    }
    LItem *it = &s->it[i];
    if (out->mask & LVIF_IMAGE) out->iImage = out->iSubItem ? 0 : it->image;
    if (out->mask & LVIF_PARAM) out->lParam = it->lp;
    if (out->mask & LVIF_INDENT) out->iIndent = it->indent;
    return TRUE;
}

static int insert_column(HWND h, LV *s, int at, const LVCOLUMNW *in, int wide)
{
    if (!in) return -1;
    if (s->ncol == s->ccap) {
        int c = s->ccap ? s->ccap * 2 : 8;
        LCol *p = realloc(s->col, sizeof(LCol) * (size_t)c);
        if (!p) return -1;
        s->col = p; s->ccap = c;
    }
    if (at < 0 || at > s->ncol) at = s->ncol;
    memmove(&s->col[at + 1], &s->col[at], sizeof(LCol) * (size_t)(s->ncol - at));
    LCol *c = &s->col[at];
    memset(c, 0, sizeof(*c));
    c->fmt = (in->mask & LVCF_FMT) ? in->fmt : LVCFMT_LEFT;
    c->cx = (in->mask & LVCF_WIDTH) ? MAX(in->cx, 0) : 50;
    c->sub = (in->mask & LVCF_SUBITEM) ? in->iSubItem : at;
    c->image = (in->mask & LVCF_IMAGE) ? in->iImage : -1;
    if (at == 0) c->fmt &= ~LVCFMT_JUSTIFYMASK;              /* the first column is always left-aligned */
    s->ncol++;
    HDITEMW hi;
    memset(&hi, 0, sizeof(hi));
    hi.mask = HDI_WIDTH | HDI_FORMAT;
    hi.cxy = c->cx;
    hi.fmt = HDF_STRING | ((c->fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_RIGHT ? HDF_RIGHT : (c->fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_CENTER ? HDF_CENTER : HDF_LEFT);
    if (in->mask & LVCF_TEXT) { hi.mask |= HDI_TEXT; hi.pszText = in->pszText; }
    if (in->mask & LVCF_ORDER) { hi.mask |= HDI_ORDER; hi.iOrder = in->iOrder; }
    if (s->hdr) SendMessageW(s->hdr, wide ? HDM_INSERTITEMW : HDM_FIRST + 1, (WPARAM)at, (LPARAM)&hi);
    refresh(h, s);
    return at;
}

static BOOL get_column(LV *s, int i, LVCOLUMNW *out, int wide)
{
    if (i < 0 || i >= s->ncol || !out) return FALSE;
    LCol *c = &s->col[i];
    if (out->mask & LVCF_FMT) out->fmt = c->fmt;
    if (out->mask & LVCF_WIDTH) out->cx = c->cx;
    if (out->mask & LVCF_SUBITEM) out->iSubItem = c->sub;
    if (out->mask & LVCF_IMAGE) out->iImage = c->image;
    if (out->mask & LVCF_ORDER) {
        HDITEMW hi; memset(&hi, 0, sizeof(hi)); hi.mask = HDI_ORDER;
        out->iOrder = s->hdr && SendMessageW(s->hdr, HDM_GETITEMW, (WPARAM)i, (LPARAM)&hi) ? hi.iOrder : i;
    }
    if ((out->mask & LVCF_TEXT) && out->pszText && out->cchTextMax > 0) {
        HDITEMW hi; memset(&hi, 0, sizeof(hi));
        hi.mask = HDI_TEXT; hi.pszText = out->pszText; hi.cchTextMax = out->cchTextMax;
        if (!s->hdr || !SendMessageW(s->hdr, wide ? HDM_GETITEMW : HDM_FIRST + 3, (WPARAM)i, (LPARAM)&hi)) copy_out(out->pszText, out->cchTextMax, L"", wide);
    }
    return TRUE;
}

static BOOL set_column(HWND h, LV *s, int i, const LVCOLUMNW *in, int wide)
{
    if (i < 0 || i >= s->ncol || !in) return FALSE;
    LCol *c = &s->col[i];
    HDITEMW hi;
    memset(&hi, 0, sizeof(hi));
    if (in->mask & LVCF_FMT) {
        c->fmt = in->fmt;
        hi.mask |= HDI_FORMAT;
        hi.fmt = HDF_STRING | ((c->fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_RIGHT ? HDF_RIGHT : (c->fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_CENTER ? HDF_CENTER : HDF_LEFT);
    }
    if (in->mask & LVCF_WIDTH) { c->cx = MAX(in->cx, 0); hi.mask |= HDI_WIDTH; hi.cxy = c->cx; }
    if (in->mask & LVCF_SUBITEM) c->sub = in->iSubItem;
    if (in->mask & LVCF_IMAGE) c->image = in->iImage;
    if (in->mask & LVCF_TEXT) { hi.mask |= HDI_TEXT; hi.pszText = in->pszText; }
    if (in->mask & LVCF_ORDER) { hi.mask |= HDI_ORDER; hi.iOrder = in->iOrder; }
    if (hi.mask && s->hdr) SendMessageW(s->hdr, wide ? HDM_SETITEMW : HDM_FIRST + 4, (WPARAM)i, (LPARAM)&hi);
    refresh(h, s);
    return TRUE;
}

static int text_px(HWND h, LV *s, const WCHAR *t)
{
    HDC dc = GetDC(h);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    int w = cc_text_w(dc, t, -1);
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    return w;
}

static BOOL set_column_width(HWND h, LV *s, int i, int cx)
{
    int v = view_of(h);
    if (v == LVS_LIST) return FALSE;
    if (i < 0 || i >= s->ncol) return FALSE;
    if (cx == LVSCW_AUTOSIZE || cx == LVSCW_AUTOSIZE_USEHEADER) {
        int w = 0;
        WCHAR b[520];
        int n = s->n > 5000 ? 5000 : s->n;
        HDC dc = GetDC(h);
        HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
        for (int k = 0; k < n; k++) { int t = cc_text_w(dc, item_text(h, s, k, s->col[i].sub, b, 520), -1); if (t > w) w = t; }
        SelectObject(dc, of);
        ReleaseDC(h, dc);
        int ck = cc_k(h);
        w += 12 * ck;
        if (i == 0) {
            int ix, iy;
            icon_size(s, LVSIL_SMALL, &ix, &iy);
            if (s->il[LVSIL_SMALL]) w += ix + 4 * ck;
            if (s->ex & LVS_EX_CHECKBOXES) w += 18 * ck;
        }
        if (cx == LVSCW_AUTOSIZE_USEHEADER) {
            WCHAR t[260] = { 0 };
            HDITEMW hi; memset(&hi, 0, sizeof(hi)); hi.mask = HDI_TEXT; hi.pszText = t; hi.cchTextMax = 260;
            if (s->hdr) SendMessageW(s->hdr, HDM_GETITEMW, (WPARAM)i, (LPARAM)&hi);
            int hw = text_px(h, s, t) + 16 * ck;
            if (hw > w) w = hw;
            int *o = col_order(s);
            if (o && o[s->ncol - 1] == i) {                 /* the last column fills the rest */
                RECT c; client(h, &c);
                int rest = c.right - (col_x(s, i));
                if (rest > w) w = rest;
            }
            free(o);
        }
        cx = w;
    }
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_WIDTH;
    c.cx = cx;
    return set_column(h, s, i, &c, 1);
}

/* -----------------------------------------------------------------------
 * Sorting
 * ----------------------------------------------------------------------- */
typedef struct { PFNLVCOMPARE fn; LPARAM data; int by_index; LV *s; } SortCtx;

static int sort_cmp(SortCtx *c, int a, int b)
{
    if (c->by_index) return c->fn((LPARAM)a, (LPARAM)b, c->data);
    return c->fn(c->s->it[a].lp, c->s->it[b].lp, c->data);
}

static void merge_sort(SortCtx *c, int *v, int *tmp, int n)
{
    if (n < 2) return;
    int m = n / 2;
    merge_sort(c, v, tmp, m);
    merge_sort(c, v + m, tmp, n - m);
    int i = 0, j = m, k = 0;
    while (i < m && j < n) tmp[k++] = sort_cmp(c, v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < m) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, sizeof(int) * (size_t)n);
}

static BOOL sort_items(HWND h, LV *s, PFNLVCOMPARE fn, LPARAM data, int by_index)
{
    if (!fn || ownerdata(h)) return FALSE;
    if (s->n < 2) return TRUE;
    int *v = malloc(sizeof(int) * (size_t)s->n * 2);
    LItem *nw = malloc(sizeof(LItem) * (size_t)s->n);
    if (!v || !nw) { free(v); free(nw); return FALSE; }
    for (int i = 0; i < s->n; i++) v[i] = i;
    SortCtx c = { fn, data, by_index, s };
    merge_sort(&c, v, v + s->n, s->n);
    int focus = -1, mark = -1;
    for (int i = 0; i < s->n; i++) {
        nw[i] = s->it[v[i]];
        if (v[i] == s->focus) focus = i;
        if (v[i] == s->mark) mark = i;
    }
    memcpy(s->it, nw, sizeof(LItem) * (size_t)s->n);
    s->focus = focus; s->mark = mark;
    free(v); free(nw);
    InvalidateRect(h, NULL, TRUE);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Finding
 * ----------------------------------------------------------------------- */
static BOOL prefix_of(const WCHAR *p, int n, const WCHAR *t)
{
    if (wlen(t) < n) return FALSE;
    return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, p, n, t, n) == 2;
}

static int find_item(HWND h, LV *s, int start, const LVFINDINFOW *f)
{
    if (!f || !s->n) return -1;
    if (ownerdata(h)) {
        NMLVFINDITEMW nm;
        memset(&nm, 0, sizeof(nm));
        nm.iStart = start + 1;
        nm.lvfi = *f;
        LRESULT r = cc_notify(h, LVN_ODFINDITEMW, &nm.hdr);
        return r >= 0 && r < s->n ? (int)r : -1;
    }
    int from = start + 1, n = s->n;
    if (from >= n) { if (!(f->flags & LVFI_WRAP)) return -1; from = 0; }
    WCHAR b[520];
    for (int k = 0; k < n; k++) {
        int i = from + k;
        if (i >= n) { if (!(f->flags & LVFI_WRAP)) break; i -= n; }
        if (f->flags & LVFI_PARAM) { if (s->it[i].lp == f->lParam) return i; continue; }
        if (!(f->flags & (LVFI_STRING | LVFI_PARTIAL)) || !f->psz) continue;
        const WCHAR *t = item_text(h, s, i, 0, b, 520);
        if (f->flags & LVFI_PARTIAL) { if (prefix_of(f->psz, wlen(f->psz), t)) return i; }
        else if (CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, f->psz, -1, t, -1) == 2) return i;
    }
    return -1;
}

static void type_search(HWND h, LV *s, WCHAR ch)
{
    DWORD now = GetTickCount();
    if (now - s->search_tick > 1000) s->nsearch = 0;
    s->search_tick = now;
    if (s->nsearch < 63) s->search[s->nsearch++] = ch;
    if (!s->n) return;
    /* the same letter again steps through the items starting with it */
    BOOL same = TRUE;
    for (int k = 1; k < s->nsearch; k++) if (s->search[k] != s->search[0]) same = FALSE;
    int len = same ? 1 : s->nsearch;
    int start = s->focus < 0 ? 0 : (same ? s->focus + 1 : s->focus);
    WCHAR b[520];
    for (int k = 0; k < s->n; k++) {
        int i = (start + k) % s->n;
        if (prefix_of(s->search, len, item_text(h, s, i, 0, b, 520))) { move_to(h, s, i, FALSE, FALSE); return; }
    }
}

/* -----------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */
static COLORREF c_bk(LV *s) { return s->bk == CLR_NONE || s->bk == CLR_DEFAULT ? GetSysColor(COLOR_WINDOW) : s->bk; }
static COLORREF c_text(LV *s) { return s->text == CLR_DEFAULT ? GetSysColor(COLOR_WINDOWTEXT) : s->text; }

static void draw_check(HDC dc, int x, int y, BOOL on, int s)    /* s: the DPI scale */
{
    RECT b = { x, y, x + 13 * s, y + 13 * s };
    cc_fill(dc, &b, RGB(255, 255, 255));
    cc_frame(dc, &b, RGB(51, 51, 51));
    if (on) {
        for (int k = 0; k < 3; k++) { RECT r = { x + (3 + k) * s, y + (6 + k) * s, x + (4 + k) * s, y + (8 + k) * s }; cc_fill(dc, &r, RGB(0, 0, 0)); }
        for (int k = 0; k < 5; k++) { RECT r = { x + (6 + k) * s, y + (7 - k) * s, x + (7 + k) * s, y + (9 - k) * s }; cc_fill(dc, &r, RGB(0, 0, 0)); }
    }
}

static void draw_state_image(HWND h, LV *s, HDC dc, int i, const RECT *icon)
{
    UINT st = get_state(h, s, i, LVIS_STATEIMAGEMASK) >> 12;
    if (s->il[LVSIL_STATE]) {
        if (!st) return;
        int sw, sh;
        icon_size(s, LVSIL_STATE, &sw, &sh);
        il_draw(s->il[LVSIL_STATE], (int)st - 1, dc, icon->left - sw - 2 * cc_k(h), (icon->top + icon->bottom - sh) / 2, ILD_TRANSPARENT);
    } else if (s->ex & LVS_EX_CHECKBOXES) {
        draw_check(dc, icon->left - 16 * cc_k(h), (icon->top + icon->bottom - 13 * cc_k(h)) / 2, st == 2, cc_k(h));
    }
}

static void draw_text_in(HDC dc, const WCHAR *t, RECT *r, int fmt, BOOL multi)
{
    UINT fl = DT_NOPREFIX | DT_END_ELLIPSIS;
    if (multi) fl |= DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL;
    else {
        fl |= DT_SINGLELINE | DT_VCENTER;
        int j = fmt & LVCFMT_JUSTIFYMASK;
        fl |= j == LVCFMT_RIGHT ? DT_RIGHT : j == LVCFMT_CENTER ? DT_CENTER : DT_LEFT;
    }
    DrawTextW(dc, t, -1, r, fl);
}

static LRESULT custom_draw(HWND h, DWORD stage, HDC dc, const RECT *rc, int i, int sub, UINT state, NMLVCUSTOMDRAW *cd)
{
    memset(cd, 0, sizeof(*cd));
    cd->nmcd.dwDrawStage = stage;
    cd->nmcd.hdc = dc;
    if (rc) cd->nmcd.rc = *rc;
    cd->nmcd.dwItemSpec = (DWORD_PTR)(i < 0 ? 0 : i);
    cd->nmcd.uItemState = state;
    LV *s = ctl_get(h);
    cd->nmcd.lItemlParam = item_lparam(h, s, i);
    cd->clrText = c_text(s);
    cd->clrTextBk = s->textbk == CLR_DEFAULT ? c_bk(s) : s->textbk;
    cd->iSubItem = sub;
    return cc_notify(h, NM_CUSTOMDRAW, &cd->nmcd.hdr);
}

static void paint_item(HWND h, LV *s, HDC dc, int i, BOOL want_item, BOOL has_focus)
{
    int v = view_of(h);
    RECT bounds, icon, label, c;
    item_bounds(h, s, i, &bounds);
    item_part(h, s, i, LVIR_ICON, &icon);
    item_part(h, s, i, LVIR_LABEL, &label);
    client(h, &c);
    BOOL sel = selected(h, s, i);
    BOOL show_sel = sel && (has_focus || (style_of(h) & LVS_SHOWSELALWAYS) || s->edit);
    COLORREF text = c_text(s), textbk = CLR_NONE;
    BOOL want_sub = FALSE;
    NMLVCUSTOMDRAW cd;
    if (want_item) {
        UINT st = (sel ? CDIS_SELECTED_ : 0) | (i == s->focus && has_focus ? CDIS_FOCUS_ : 0);
        LRESULT r = custom_draw(h, CDDS_ITEMPREPAINT, dc, &bounds, i, 0, st, &cd);
        if (r & CDRF_SKIPDEFAULT) return;
        if (cd.clrText != CLR_DEFAULT) text = cd.clrText;
        if (cd.clrTextBk != CLR_DEFAULT && cd.clrTextBk != c_bk(s)) textbk = cd.clrTextBk;
        want_sub = (r & CDRF_NOTIFYSUBITEMDRAW) && v == LVS_REPORT;
    }
    COLORREF selbg = has_focus ? RGB(204, 232, 255) : RGB(217, 217, 217);
    RECT selr;
    if (v == LVS_REPORT) {
        selr = (s->ex & LVS_EX_FULLROWSELECT) ? bounds : label;
        if (s->ex & LVS_EX_FULLROWSELECT) selr.right = MAX(selr.right, bounds.right);
    } else if (v == LVS_ICON) {
        selr = label;
    } else {
        HDC mdc = dc;
        WCHAR b[520];
        int tw = cc_text_w(mdc, item_text(h, s, i, 0, b, 520), -1);
        selr = label;
        selr.right = MIN(label.left + tw + 6 * cc_k(h), bounds.right - 2 * cc_k(h));
        selr.left -= 2 * cc_k(h);
    }
    if (textbk != CLR_NONE && !show_sel) {
        RECT r = v == LVS_REPORT ? bounds : selr;
        cc_fill(dc, &r, textbk);
    }
    if (show_sel && v != LVS_ICON) cc_fill(dc, &selr, selbg);

    /* the icon and state image */
    int img = item_image(h, s, i);
    HIMAGELIST il = v == LVS_ICON ? s->il[LVSIL_NORMAL] : s->il[LVSIL_SMALL];
    if (il && img >= 0) {
        int ix, iy;
        ImageList_GetIconSize(il, &ix, &iy);
        UINT fl = ILD_TRANSPARENT | (get_state(h, s, i, LVIS_OVERLAYMASK));
        il_draw(il, img, dc, icon.left, (icon.top + icon.bottom - iy) / 2, fl);
    }
    draw_state_image(h, s, dc, i, &icon);

    /* the label */
    WCHAR b[520];
    const WCHAR *t = item_text(h, s, i, 0, b, 520);
    SetTextColor(dc, text);
    if (v == LVS_ICON) {
        RECT m = label;
        DrawTextW(dc, t, -1, &m, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        int k = cc_k(h), lh = MIN(m.bottom - m.top, 2 * s->fh + 2 * k);
        RECT box = { label.left, label.top, label.right, label.top + lh + 2 * k };
        int tw = MIN(m.right - m.left, label.right - label.left);
        box.left = (label.left + label.right - tw) / 2 - 2 * k;
        box.right = box.left + tw + 4 * k;
        if (show_sel) cc_fill(dc, &box, selbg);
        RECT tr = { label.left, label.top + 1, label.right, label.top + 1 + lh };
        if (i != s->edit_item || !s->edit) draw_text_in(dc, t, &tr, 0, TRUE);
        if (i == s->focus && has_focus) DrawFocusRect(dc, &box);
        return;
    }
    RECT tr = label;
    tr.left += 2 * cc_k(h);
    if (!(s->edit && i == s->edit_item)) {
        if (want_sub) {
            LRESULT r = custom_draw(h, CDDS_ITEMPREPAINT | CDDS_SUBITEM, dc, &label, i, 0, 0, &cd);
            if (!(r & CDRF_SKIPDEFAULT)) { SetTextColor(dc, cd.clrText != CLR_DEFAULT ? cd.clrText : text); draw_text_in(dc, t, &tr, s->ncol ? s->col[0].fmt : 0, FALSE); }
        } else {
            draw_text_in(dc, t, &tr, s->ncol ? s->col[0].fmt : 0, FALSE);
        }
    }
    if (v == LVS_REPORT) {
        for (int k = 1; k < s->ncol; k++) {
            RECT r;
            subitem_rect(h, s, i, k, LVIR_BOUNDS, &r);
            if (r.right <= 0 || r.left >= c.right || r.right - r.left < 4) continue;
            RECT tr2 = { r.left + 6 * cc_k(h), r.top, r.right - 6 * cc_k(h), r.bottom };
            if ((s->ex & LVS_EX_SUBITEMIMAGES) && il) {
                LVITEMW q; WCHAR qb[2];
                if (ownerdata(h) || s->it[i].nsub <= k || s->it[i].sub[k] == LPSTR_TEXTCALLBACKW) {
                    dispinfo(h, s, i, s->col[k].sub, LVIF_IMAGE, &q, qb, 2);
                    if (q.iImage >= 0) { int ix, iy; ImageList_GetIconSize(il, &ix, &iy); il_draw(il, q.iImage, dc, tr2.left - 2, (r.top + r.bottom - iy) / 2, ILD_TRANSPARENT); tr2.left += ix + 2; }
                }
            }
            const WCHAR *st = item_text(h, s, i, s->col[k].sub, b, 520);
            if (want_sub) {
                LRESULT rr = custom_draw(h, CDDS_ITEMPREPAINT | CDDS_SUBITEM, dc, &r, i, k, 0, &cd);
                if (rr & CDRF_SKIPDEFAULT) continue;
                SetTextColor(dc, cd.clrText != CLR_DEFAULT ? cd.clrText : text);
                if (cd.clrTextBk != CLR_DEFAULT && cd.clrTextBk != c_bk(s) && !show_sel) cc_fill(dc, &r, cd.clrTextBk);
            }
            draw_text_in(dc, st, &tr2, s->col[k].fmt, FALSE);
        }
    }
    if (i == s->focus && has_focus) {
        RECT f = v == LVS_REPORT && (s->ex & LVS_EX_FULLROWSELECT) ? bounds : selr;
        if (v == LVS_REPORT && (s->ex & LVS_EX_FULLROWSELECT)) { f.left = MAX(f.left, 0); f.right = MIN(MAX(f.right, 0), c.right); }
        if (show_sel) cc_frame(dc, &f, RGB(153, 209, 255));
        else DrawFocusRect(dc, &f);
    }
}

static void paint(HWND h, LV *s, HDC dc, const RECT *upd)
{
    RECT c;
    client(h, &c);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    SetBkMode(dc, TRANSPARENT);
    NMLVCUSTOMDRAW cd;
    LRESULT pre = custom_draw(h, CDDS_PREPAINT, dc, &c, -1, 0, 0, &cd);
    RECT top = c;
    top.top = hdr_h(h, s);
    RECT fill;
    if (IntersectRect(&fill, &top, upd)) cc_fill(dc, &fill, c_bk(s));
    if (!(pre & CDRF_SKIPDEFAULT)) {
        BOOL focus = GetFocus() == h;
        BOOL want_item = (pre & CDRF_NOTIFYITEMDRAW) != 0;
        int v = view_of(h), first = 0, last = s->n - 1;
        Grid g;
        grid(h, s, &g);
        if (v == LVS_REPORT) {
            first = s->top + MAX(0, (upd->top - top.top) / g.ch);
            last = s->top + (upd->bottom - top.top) / g.ch;
        } else if (v == LVS_LIST) {
            first = (s->top + MAX(0, upd->left / g.cw)) * g.per;
            last = (s->top + upd->right / g.cw + 1) * g.per - 1;
        } else {
            first = ((s->sy + MAX(0, upd->top)) / g.ch) * g.per;
            last = ((s->sy + upd->bottom) / g.ch + 1) * g.per - 1;
        }
        if (first < 0) first = 0;
        if (last > s->n - 1) last = s->n - 1;
        HRGN clip = CreateRectRgn(top.left, top.top, top.right, top.bottom);
        SelectClipRgn(dc, clip);
        for (int i = first; i <= last; i++) {
            HGDIOBJ f = SelectObject(dc, s->font ? s->font : cc_font_for(h));
            paint_item(h, s, dc, i, want_item, focus);
            SelectObject(dc, f);
        }
        if (v == LVS_REPORT && (s->ex & LVS_EX_GRIDLINES)) {
            COLORREF gl = RGB(240, 240, 240);
            int x = -s->sx;
            int *o = col_order(s);
            for (int p = 0; o && p < s->ncol; p++) { x += s->col[o[p]].cx; RECT r = { x - 1, top.top, x, c.bottom }; cc_fill(dc, &r, gl); }
            free(o);
            for (int y = top.top + g.ch - 1; y < c.bottom; y += g.ch) { RECT r = { 0, y, MIN(c.right, total_width(s) - s->sx), y + 1 }; cc_fill(dc, &r, gl); }
        }
        SelectClipRgn(dc, NULL);
        DeleteObject(clip);
        if (pre & CDRF_NOTIFYPOSTPAINT) custom_draw(h, CDDS_POSTPAINT, dc, &c, -1, 0, 0, &cd);
    }
    SelectObject(dc, of);
}

/* -----------------------------------------------------------------------
 * Label editing
 * ----------------------------------------------------------------------- */
static HWND edit_label(HWND h, LV *s, int i)
{
    if (i < 0 || i >= s->n) return NULL;
    end_edit(h, s, TRUE);
    SetFocus(h);
    ensure_visible(h, s, i, FALSE);
    WCHAR b[520];
    const WCHAR *t = item_text(h, s, i, 0, b, 520);
    NMLVDISPINFOW di;
    memset(&di, 0, sizeof(di));
    di.item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
    di.item.iItem = i;
    di.item.pszText = (LPWSTR)t;
    di.item.lParam = item_lparam(h, s, i);
    di.item.state = get_state(h, s, i, ~0u);
    if (cc_notify(h, s->ansi ? LVN_BEGINLABELEDITA_ : LVN_BEGINLABELEDITW, &di.hdr)) return NULL;
    RECT r;
    item_part(h, s, i, LVIR_LABEL, &r);
    if (view_of(h) != LVS_REPORT) { int tw = text_px(h, s, t) + 16; if (view_of(h) != LVS_ICON) r.right = MAX(r.right, r.left + tw); else r.bottom = r.top + s->fh + 6; }
    else r.right = MAX(r.right, r.left + 60);
    s->edit = CreateWindowExW(0, L"Edit", t, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | (view_of(h) == LVS_ICON ? ES_CENTER : 0),
                              r.left, r.top, r.right - r.left, r.bottom - r.top, h, (HMENU)1, NULL, NULL);
    if (!s->edit) return NULL;
    s->edit_item = i;
    SendMessageW(s->edit, WM_SETFONT, (WPARAM)(s->font ? s->font : cc_font_for(h)), 0);
    SetWindowSubclass(s->edit, edit_subclass, 1, (DWORD_PTR)h);
    SendMessageW(s->edit, EM_SETSEL, 0, -1);
    ShowWindow(s->edit, SW_SHOW);
    SetFocus(s->edit);
    inval_item(h, s, i);
    return s->edit;
}

static void end_edit(HWND h, LV *s, BOOL cancel)
{
    if (!s->edit) return;
    HWND e = s->edit;
    int i = s->edit_item;
    s->edit = NULL;
    WCHAR b[520];
    b[0] = 0;
    if (!cancel) GetWindowTextW(e, b, 520);
    NMLVDISPINFOW di;
    memset(&di, 0, sizeof(di));
    di.item.mask = LVIF_TEXT | LVIF_PARAM;
    di.item.iItem = i;
    di.item.lParam = item_lparam(h, s, i);
    di.item.pszText = cancel ? NULL : b;
    di.item.cchTextMax = 520;
    char ab[520];
    if (s->ansi && !cancel) { WideCharToMultiByte(CP_ACP, 0, b, -1, ab, 520, NULL, NULL); di.item.pszText = (LPWSTR)ab; }
    RemoveWindowSubclass(e, edit_subclass, 1);
    ShowWindow(e, SW_HIDE);
    LRESULT ok = cc_notify(h, s->ansi ? LVN_ENDLABELEDITA_ : LVN_ENDLABELEDITW, &di.hdr);
    if (ok && !cancel && !ownerdata(h) && i >= 0 && i < s->n && !(s->it[i].nsub && s->it[i].sub[0] == LPSTR_TEXTCALLBACKW))
        set_sub_text(&s->it[i], 0, b, 1);
    DestroyWindow(e);
    if (IsWindow(h)) { inval_item(h, s, i); if (GetFocus() == NULL) SetFocus(h); }
}

static LRESULT CALLBACK edit_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    HWND lv = (HWND)ref;
    LV *s = ctl_get(lv);
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DefSubclassProc(h, msg, wp, lp);
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE) { if (s) { end_edit(lv, s, wp == VK_ESCAPE); SetFocus(lv); } return 0; }
        break;
    case WM_CHAR: if (wp == '\r' || wp == 27) return 0; break;
    case WM_KILLFOCUS: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (s && s->edit == h) end_edit(lv, s, FALSE);
        return r;
    }
    }
    (void)id;
    return DefSubclassProc(h, msg, wp, lp);
}

/* -----------------------------------------------------------------------
 * Mouse and keyboard
 * ----------------------------------------------------------------------- */
static void notify_activate(HWND h, UINT code, int i, int sub, POINT pt)
{
    NMITEMACTIVATE nm;
    memset(&nm, 0, sizeof(nm));
    nm.iItem = i; nm.iSubItem = sub; nm.ptAction = pt;
    if (GetKeyState(VK_SHIFT) < 0) nm.uKeyFlags |= 4;      /* LVKF_SHIFT */
    if (GetKeyState(VK_CONTROL) < 0) nm.uKeyFlags |= 2;    /* LVKF_CONTROL */
    if (GetKeyState(VK_MENU) < 0) nm.uKeyFlags |= 1;       /* LVKF_ALT */
    cc_notify(h, code, &nm.hdr);
}

static void toggle_check(HWND h, LV *s, int i)
{
    UINT st = get_state(h, s, i, LVIS_STATEIMAGEMASK) >> 12;
    set_state(h, s, i, INDEXTOSTATEIMAGEMASK(st == 2 ? 1 : 2), LVIS_STATEIMAGEMASK);
}

static void button_down(HWND h, LV *s, POINT pt, BOOL right, BOOL dbl)
{
    KillTimer(h, EDIT_TIMER);
    s->edit_pending = 0;
    end_edit(h, s, FALSE);
    BOOL had_focus = GetFocus() == h;
    if (!had_focus) SetFocus(h);
    UINT flags;
    int sub;
    int i = hit_test(h, s, pt, &flags, &sub);
    BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    s->down_was_sel = i >= 0 && selected(h, s, i) && i == s->focus && had_focus;
    if (i < 0) {
        if (!ctrl && !shift) deselect_all(h, s, -1);
    } else if (right) {
        if (!selected(h, s, i)) { deselect_all(h, s, i); set_state(h, s, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); s->mark = i; }
        else set_state(h, s, i, LVIS_FOCUSED, LVIS_FOCUSED);
    } else if ((flags & LVHT_ONITEMSTATEICON) && (s->ex & LVS_EX_CHECKBOXES)) {
        toggle_check(h, s, i);
    } else if (ctrl && !shift && !(style_of(h) & LVS_SINGLESEL)) {
        set_state(h, s, i, (selected(h, s, i) ? 0 : LVIS_SELECTED) | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        s->mark = i;
    } else if (!(selected(h, s, i) && !shift && !dbl)) {
        move_to(h, s, i, shift, ctrl);
    } else {
        set_state(h, s, i, LVIS_FOCUSED, LVIS_FOCUSED);      /* a selection being dragged stays; see button_up */
    }
    s->down = right ? 2 : 1;
    s->down_item = i;
    s->down_pt = pt;
    s->dragging = 0;
    SetCapture(h);
    if (dbl && !right) {
        s->down = 0;
        ReleaseCapture();
        notify_activate(h, NM_DBLCLK, i, sub, pt);
        if (i >= 0 && IsWindow(h)) notify_activate(h, LVN_ITEMACTIVATE, i, sub, pt);
    }
}

static void button_up(HWND h, LV *s, POINT pt, BOOL right)
{
    if (!s->down) return;
    int was = s->down;
    s->down = 0;
    ReleaseCapture();
    if (s->dragging) return;
    UINT flags;
    int sub;
    int i = hit_test(h, s, pt, &flags, &sub);
    BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (!right && was == 1 && i >= 0 && i == s->down_item && !shift && !ctrl) {
        /* a click on part of a selection selects just that item */
        if (selected(h, s, i)) { deselect_all(h, s, i); set_state(h, s, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); s->mark = i; }
        if (s->down_was_sel && (style_of(h) & LVS_EDITLABELS) && (flags & LVHT_ONITEMLABEL)) {
            s->edit_pending = i + 1;
            SetTimer(h, EDIT_TIMER, GetDoubleClickTime(), NULL);
        }
    }
    notify_activate(h, right ? NM_RCLICK : NM_CLICK, i, sub, pt);
}

static void mouse_move(HWND h, LV *s, POINT pt, WPARAM keys)
{
    if (!s->down || s->dragging || s->down_item < 0) return;
    if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return;
    int dx = GetSystemMetrics(SM_CXDRAG), dy = GetSystemMetrics(SM_CYDRAG);
    if (dx < 4) dx = 4;
    if (dy < 4) dy = 4;
    if (ABS(pt.x - s->down_pt.x) < dx && ABS(pt.y - s->down_pt.y) < dy) return;
    s->dragging = 1;
    int i = s->down_item, btn = s->down;
    s->down = 0;
    ReleaseCapture();
    notify_lv(h, btn == 2 ? LVN_BEGINRDRAG : LVN_BEGINDRAG, i, 0, 0, 0, 0, item_lparam(h, s, i));
}

static int page_step(HWND h, LV *s)
{
    RECT c;
    client(h, &c);
    Grid g;
    grid(h, s, &g);
    switch (view_of(h)) {
    case LVS_REPORT: return MAX(1, rows_visible(h, s) - 1);
    case LVS_LIST: return g.per * MAX(1, c.right / g.cw);
    default: return g.per * MAX(1, c.bottom / g.ch);
    }
}

static void key_down(HWND h, LV *s, WPARAM vk)
{
    NMLVKEYDOWN kd;
    memset(&kd, 0, sizeof(kd));
    kd.wVKey = (WORD)vk;
    cc_notify(h, LVN_KEYDOWN, &kd.hdr);
    if (!IsWindow(h)) return;
    BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    int v = view_of(h), f = s->focus < 0 ? -1 : s->focus, ni = -2;
    Grid g;
    grid(h, s, &g);
    int per = g.per;
    switch (vk) {
    case VK_UP:    ni = v == LVS_REPORT || v == LVS_LIST ? f - 1 : f - per; break;
    case VK_DOWN:  ni = v == LVS_REPORT || v == LVS_LIST ? f + 1 : f + per; if (f < 0) ni = 0; break;
    case VK_LEFT:
        if (v == LVS_REPORT) { on_scroll(h, s, SB_HORZ, SB_LINEUP); return; }
        ni = v == LVS_LIST ? f - per : f - 1;
        break;
    case VK_RIGHT:
        if (v == LVS_REPORT) { on_scroll(h, s, SB_HORZ, SB_LINEDOWN); return; }
        ni = v == LVS_LIST ? f + per : f + 1;
        break;
    case VK_PRIOR: ni = f - page_step(h, s); break;
    case VK_NEXT:  ni = f + page_step(h, s); break;
    case VK_HOME:  ni = 0; break;
    case VK_END:   ni = s->n - 1; break;
    case VK_SPACE:
        if (f < 0) return;
        if (s->ex & LVS_EX_CHECKBOXES) { toggle_check(h, s, f); return; }
        if (ctrl) set_state(h, s, f, selected(h, s, f) ? 0 : LVIS_SELECTED, LVIS_SELECTED);
        else if (shift) select_range(h, s, s->mark < 0 ? f : s->mark, f, FALSE);
        else set_state(h, s, f, LVIS_SELECTED, LVIS_SELECTED);
        return;
    case VK_RETURN:
        if (s->n > 0 && f >= 0) {
            if (cc_notify(h, NM_RETURN, NULL)) return;
            if (!IsWindow(h)) return;
            POINT pt = { 0, 0 };
            notify_activate(h, LVN_ITEMACTIVATE, f, 0, pt);
        }
        return;
    default: return;
    }
    if (!s->n) return;
    if (ni < 0) ni = 0;
    if (ni >= s->n) ni = s->n - 1;
    move_to(h, s, ni, shift, ctrl);
}

/* -----------------------------------------------------------------------
 * The window procedure
 * ----------------------------------------------------------------------- */
static void set_view(HWND h, LV *s, int v)
{
    DWORD st = style_of(h);
    if ((int)(st & LVS_TYPEMASK) != v) SetWindowLongW(h, GWL_STYLE, (LONG)((st & ~LVS_TYPEMASK) | (DWORD)v));
    s->top = 0; s->sx = s->sy = 0;
    end_edit(h, s, TRUE);
    refresh(h, s);
    if (s->focus >= 0) ensure_visible(h, s, s->focus, FALSE);
}

static void set_font(HWND h, LV *s, HFONT f)
{
    s->font = f;
    s->fh = cc_font_h(f ? f : cc_font_for(h));
    if (s->hdr) SendMessageW(s->hdr, WM_SETFONT, (WPARAM)f, 0);
    refresh(h, s);
}

static int count_selected(HWND h, LV *s)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) if (selected(h, s, i)) n++;
    return n;
}

static int next_item(HWND h, LV *s, int start, UINT flags)
{
    UINT want = 0;
    if (flags & LVNI_SELECTED) want |= LVIS_SELECTED;
    if (flags & LVNI_FOCUSED) want |= LVIS_FOCUSED;
    if (flags & LVNI_CUT) want |= LVIS_CUT;
    if (flags & LVNI_DROPHILITED) want |= LVIS_DROPHILITED;
    int step = 1;
    int v = view_of(h);
    Grid g;
    grid(h, s, &g);
    if (flags & LVNI_ABOVE) step = v == LVS_REPORT || v == LVS_LIST ? -1 : -g.per;
    else if (flags & LVNI_BELOW) step = v == LVS_REPORT || v == LVS_LIST ? 1 : g.per;
    else if (flags & LVNI_TOLEFT) step = v == LVS_LIST ? -g.per : v == LVS_REPORT ? 0 : -1;
    else if (flags & LVNI_TORIGHT) step = v == LVS_LIST ? g.per : v == LVS_REPORT ? 0 : 1;
    if (!step) return -1;
    if (want == LVIS_FOCUSED && step == 1 && !(flags & (LVNI_ABOVE | LVNI_BELOW | LVNI_TOLEFT | LVNI_TORIGHT)))
        return s->focus > start ? s->focus : -1;
    for (int i = start < 0 && step > 0 ? 0 : start + step; i >= 0 && i < s->n; i += step) {
        if (i == start) continue;
        if ((get_state(h, s, i, want) & want) == want) return i;
    }
    return -1;
}

static LRESULT header_notify(HWND h, LV *s, NMHDR *nm)
{
    LRESULT r = 0;
    HWND parent = GetParent(h);
    /* the parent sees the header's notifications too (it can veto a resize) */
    if (parent) r = SendMessageW(parent, WM_NOTIFY, nm->idFrom, (LPARAM)nm);
    NMHEADERW *hn = (NMHEADERW *)nm;
    switch (nm->code) {
    case HDN_ITEMCHANGINGW: case HDN_BEGINTRACKW: case HDN_TRACKW: return r;
    case HDN_ITEMCHANGEDW:
        if (hn->iItem >= 0 && hn->iItem < s->ncol && hn->pitem && (hn->pitem->mask & HDI_WIDTH)) {
            s->col[hn->iItem].cx = hn->pitem->cxy;
            update_scroll(h, s);
            InvalidateRect(h, NULL, TRUE);
        }
        return r;
    case HDN_ITEMCLICKW:
        if (!(style_of(h) & LVS_NOSORTHEADER)) notify_lv(h, LVN_COLUMNCLICK, -1, hn->iItem, 0, 0, 0, 0);
        return r;
    case HDN_DIVIDERDBLCLICKW:
        set_column_width(h, s, hn->iItem, LVSCW_AUTOSIZE);
        return r;
    case HDN_ENDTRACKW:
        InvalidateRect(h, NULL, TRUE);
        return r;
    case NM_RCLICK: {
        /* a right-click on the header: the parent's context menu, as Explorer shows the column chooser */
        POINT pt; GetCursorPos(&pt);
        SendMessageW(h, WM_CONTEXTMENU, (WPARAM)s->hdr, MAKELPARAM(pt.x, pt.y));
        return 1;
    }
    }
    return r;
}

LRESULT CALLBACK ListViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LV *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    int wide = 1;
    switch (msg) {
    case WM_NCCREATE:
        s = calloc(1, sizeof(LV));
        if (!s) return FALSE;
        s->focus = s->mark = s->hot = s->edit_item = -1;
        s->bk = CLR_DEFAULT; s->text = CLR_DEFAULT; s->textbk = CLR_DEFAULT;
        s->fh = cc_font_h(cc_font_for(h));
        ctl_set(h, s);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE: {
        HWND p = GetParent(h);
        s->ansi = p && SendMessageW(p, WM_NOTIFYFORMAT, (WPARAM)h, NF_QUERY_) == NFR_ANSI;
        s->hdr = CreateWindowExW(0, WC_HEADERW, NULL, WS_CHILD | HDS_HORZ | HDS_BUTTONS | HDS_FULLDRAG | ((style_of(h) & LVS_NOSORTHEADER) ? 0 : 0),
                                 0, 0, 0, 0, h, (HMENU)0, NULL, NULL);
        refresh(h, s);
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(h, EDIT_TIMER);
        if (!ownerdata(h)) {
            for (int i = s->n - 1; i >= 0; i--) { notify_lv(h, LVN_DELETEITEM, i, 0, 0, 0, 0, s->it[i].lp); free_item(&s->it[i]); }
        }
        if (!(style_of(h) & LVS_SHAREIMAGELISTS)) for (int k = 0; k < 3; k++) if (s->il[k]) ImageList_Destroy(s->il[k]);
        free(s->it); free(s->osel); free(s->col); free(s);
        ctl_set(h, NULL);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NOTIFYFORMAT: return lp == NF_QUERY_ ? NFR_UNICODE : (s->ansi ? NFR_ANSI : NFR_UNICODE);
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm && nm->hwndFrom == s->hdr) return header_notify(h, s, nm);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, s, dc, &ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: { RECT c; client(h, &c); paint(h, s, (HDC)wp, &c); return 0; }
    case WM_SIZE: refresh(h, s); return 0;
    case WM_STYLECHANGED:
        if (wp == (WPARAM)GWL_STYLE && lp) {
            const STYLESTRUCT *ss = (const STYLESTRUCT *)lp;
            if ((ss->styleOld ^ ss->styleNew) & (LVS_TYPEMASK | LVS_NOCOLUMNHEADER)) { s->top = 0; s->sx = s->sy = 0; end_edit(h, s, TRUE); refresh(h, s); }
        }
        return 0;
    case WM_SETFONT: set_font(h, s, (HFONT)wp); return 0;
    case WM_DPICHANGED_AFTERPARENT:                          /* the window's DPI changed: the defaults' sizes */
        if (!s->font) set_font(h, s, NULL);
        else refresh(h, s);
        return 0;
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_SETFOCUS:
        InvalidateRect(h, NULL, TRUE);
        cc_notify(h, NM_SETFOCUS, NULL);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL, TRUE);
        cc_notify(h, NM_KILLFOCUS, NULL);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_VSCROLL: on_scroll(h, s, SB_VERT, LOWORD(wp)); return 0;
    case WM_HSCROLL: on_scroll(h, s, SB_HORZ, LOWORD(wp)); return 0;
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(wp) / 40;
        if (!d) d = (short)HIWORD(wp) > 0 ? 1 : -1;
        int v = view_of(h);
        if (v == LVS_LIST) { scroll_to(h, s, SB_HORZ, s->top - (d > 0 ? 1 : -1)); return 0; }
        SCROLLINFO si = { sizeof(si), SIF_ALL, 0, 0, 0, 0, 0 };
        GetScrollInfo(h, SB_VERT, &si);
        int pos = si.nPos - d * (v == LVS_REPORT ? 1 : row_h(h, s));
        int mx = si.nMax - MAX((int)si.nPage - 1, 0);
        if (pos > mx) pos = mx;
        if (pos < 0) pos = 0;
        if (pos != si.nPos) scroll_to(h, s, SB_VERT, pos);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        button_down(h, s, pt, msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK, msg == WM_LBUTTONDBLCLK);
        return 0;
    }
    case WM_LBUTTONUP: { POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; button_up(h, s, pt, FALSE); return 0; }
    case WM_RBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (!s->down) return 0;
        s->down = 0;
        ReleaseCapture();
        UINT f; int sub;
        int i = hit_test(h, s, pt, &f, &sub);
        NMITEMACTIVATE nm;
        memset(&nm, 0, sizeof(nm));
        nm.iItem = i; nm.iSubItem = sub; nm.ptAction = pt;
        if (!cc_notify(h, NM_RCLICK, &nm.hdr) && IsWindow(h)) {
            POINT sp = pt;
            ClientToScreen(h, &sp);
            SendMessageW(h, WM_CONTEXTMENU, (WPARAM)h, MAKELPARAM(sp.x, sp.y));
        }
        return 0;
    }
    case WM_MOUSEMOVE: { POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; mouse_move(h, s, pt, wp); return 0; }
    case WM_CAPTURECHANGED: if ((HWND)lp != h) s->down = 0; return 0;
    case WM_TIMER:
        if (wp == EDIT_TIMER) {
            KillTimer(h, EDIT_TIMER);
            int i = s->edit_pending - 1;
            s->edit_pending = 0;
            if (i >= 0 && i == s->focus && selected(h, s, i) && GetFocus() == h) edit_label(h, s, i);
            return 0;
        }
        break;
    case WM_KEYDOWN: key_down(h, s, wp); return 0;
    case WM_CHAR:
        if (wp >= 32 && wp != 127 && GetKeyState(VK_CONTROL) >= 0) type_search(h, s, (WCHAR)wp);
        return 0;
    case WM_CONTEXTMENU: return DefWindowProcW(h, msg, wp, lp);

    /* ---- items ---- */
    case LVM_GETITEMCOUNT: return s->n;
    case LVM_SETITEMCOUNT:
        if (ownerdata(h)) {
            int n = (int)wp;
            if (n < 0) n = 0;
            if (n > s->ocap) {
                BYTE *p = realloc(s->osel, (size_t)n);
                if (!p) return FALSE;
                memset(p + s->ocap, 0, (size_t)(n - s->ocap));
                s->osel = p; s->ocap = n;
            }
            if (n > s->n && s->osel) memset(s->osel + s->n, 0, (size_t)(n - s->n));
            s->n = n;
            if (s->focus >= n) s->focus = -1;
            if (s->mark >= n) s->mark = -1;
            if (!(lp & LVSICF_NOSCROLL)) { if (s->top >= n) s->top = 0; }
            refresh(h, s);
        } else if ((int)wp > s->cap) {
            LItem *p = realloc(s->it, sizeof(LItem) * (size_t)wp);
            if (p) { s->it = p; s->cap = (int)wp; }
        }
        return TRUE;
    case LVM_INSERTITEMA: wide = 0; /* fall through */
    case LVM_INSERTITEMW: return insert_item(h, s, (const LVITEMW *)lp, wide);
    case LVM_DELETEITEM: { BOOL r = delete_item(h, s, (int)wp, TRUE); refresh(h, s); return r; }
    case LVM_DELETEALLITEMS: delete_all(h, s); return TRUE;
    case LVM_SETITEMA: wide = 0; /* fall through */
    case LVM_SETITEMW: return set_item(h, s, (const LVITEMW *)lp, wide);
    case LVM_GETITEMA: wide = 0; /* fall through */
    case LVM_GETITEMW: return get_item(h, s, (LVITEMW *)lp, wide);
    case LVM_SETITEMTEXTA: wide = 0; /* fall through */
    case LVM_SETITEMTEXTW: {
        LVITEMW *in = (LVITEMW *)lp;
        if (!in) return FALSE;
        LVITEMW c = *in;
        c.iItem = (int)wp;
        c.mask = LVIF_TEXT;
        return set_item(h, s, &c, wide);
    }
    case LVM_GETITEMTEXTA: wide = 0; /* fall through */
    case LVM_GETITEMTEXTW: {
        LVITEMW *out = (LVITEMW *)lp;
        if (!out || !out->pszText || out->cchTextMax <= 0) return 0;
        WCHAR b[520];
        const WCHAR *t = item_text(h, s, (int)wp, out->iSubItem, b, 520);
        copy_out(out->pszText, out->cchTextMax, t, wide);
        return wide ? wlen(out->pszText) : (LRESULT)strlen((char *)out->pszText);
    }
    case LVM_SETITEMSTATE: {
        LVITEMW *in = (LVITEMW *)lp;
        if (!in) return FALSE;
        if ((int)wp != -1 && ((int)wp < 0 || (int)wp >= s->n)) return FALSE;
        set_state(h, s, (int)wp, in->state, in->stateMask);
        if ((in->stateMask & in->state & LVIS_SELECTED) && (int)wp >= 0) s->mark = s->mark < 0 ? (int)wp : s->mark;
        return TRUE;
    }
    case LVM_GETITEMSTATE: return get_state(h, s, (int)wp, (UINT)lp);
    case LVM_GETNEXTITEM: return next_item(h, s, (int)wp, (UINT)LOWORD(lp));
    case LVM_GETSELECTEDCOUNT: return count_selected(h, s);
    case LVM_GETSELECTIONMARK: return s->mark;
    case LVM_SETSELECTIONMARK: { int o = s->mark; s->mark = (int)lp; return o; }
    case LVM_SETCALLBACKMASK: s->cbmask = (UINT)wp; return TRUE;
    case LVM_GETCALLBACKMASK: return s->cbmask;
    case LVM_SORTITEMS: return sort_items(h, s, (PFNLVCOMPARE)lp, (LPARAM)wp, 0);
    case LVM_SORTITEMSEX: return sort_items(h, s, (PFNLVCOMPARE)lp, (LPARAM)wp, 1);
    case LVM_FINDITEMA: {
        const LVFINDINFOW *fa = (const LVFINDINFOW *)lp;
        if (!fa) return -1;
        LVFINDINFOW f = *fa;
        WCHAR w[260];
        if (fa->psz && (fa->flags & (LVFI_STRING | LVFI_PARTIAL))) { MultiByteToWideChar(CP_ACP, 0, (const char *)fa->psz, -1, w, 260); f.psz = w; }
        return find_item(h, s, (int)wp, &f);
    }
    case LVM_FINDITEMW: return find_item(h, s, (int)wp, (const LVFINDINFOW *)lp);
    case LVM_REDRAWITEMS: for (int i = MAX(0, (int)wp); i <= (int)lp && i < s->n; i++) inval_item(h, s, i); return TRUE;
    case LVM_UPDATE: refresh(h, s); return TRUE;
    case LVM_ENSUREVISIBLE: ensure_visible(h, s, (int)wp, (BOOL)lp); return TRUE;
    case LVM_ISITEMVISIBLE: {
        RECT r, c, x;
        client(h, &c);
        c.top = hdr_h(h, s);
        return item_bounds(h, s, (int)wp, &r) && IntersectRect(&x, &r, &c);
    }
    case LVM_GETITEMRECT: {
        RECT *r = (RECT *)lp;
        if (!r) return FALSE;
        return item_part(h, s, (int)wp, r->left, r);
    }
    case LVM_GETSUBITEMRECT: {
        RECT *r = (RECT *)lp;
        if (!r) return FALSE;
        return subitem_rect(h, s, (int)wp, r->top, r->left, r);
    }
    case LVM_GETITEMPOSITION: {
        RECT r;
        POINT *p = (POINT *)lp;
        if (!p || !item_part(h, s, (int)wp, view_of(h) == LVS_ICON ? LVIR_ICON : LVIR_BOUNDS, &r)) return FALSE;
        p->x = r.left; p->y = r.top;
        return TRUE;
    }
    case LVM_SETITEMPOSITION: case LVM_SETITEMPOSITION32: return TRUE;     /* auto-arranged */
    case LVM_HITTEST: {
        LVHITTESTINFO *t = (LVHITTESTINFO *)lp;
        if (!t) return -1;
        t->iItem = hit_test(h, s, t->pt, &t->flags, NULL);
        t->iSubItem = 0;
        return t->iItem;
    }
    case LVM_SUBITEMHITTEST: {
        LVHITTESTINFO *t = (LVHITTESTINFO *)lp;
        if (!t) return -1;
        int sub = 0;
        t->iItem = hit_test(h, s, t->pt, &t->flags, &sub);
        t->iSubItem = sub;
        return t->iItem;
    }
    case LVM_GETTOPINDEX: {
        int v = view_of(h);
        if (v == LVS_REPORT) return s->top;
        if (v == LVS_LIST) { Grid g; grid(h, s, &g); return s->top * g.per; }
        return 0;
    }
    case LVM_GETCOUNTPERPAGE: {
        int v = view_of(h);
        if (v == LVS_REPORT) return rows_visible(h, s);
        if (v == LVS_LIST) { RECT c; client(h, &c); Grid g; grid(h, s, &g); return g.per * MAX(1, c.right / g.cw); }
        return s->n;
    }
    case LVM_GETORIGIN: {
        POINT *p = (POINT *)lp;
        int v = view_of(h);
        if (!p || v == LVS_REPORT || v == LVS_LIST) return FALSE;
        p->x = 0; p->y = s->sy;
        return TRUE;
    }
    case LVM_GETVIEWRECT: {
        RECT *r = (RECT *)lp;
        if (!r) return FALSE;
        Grid g; grid(h, s, &g);
        int v = view_of(h);
        if (v == LVS_REPORT) SetRect(r, -s->sx, 0, total_width(s) - s->sx, s->n * g.ch);
        else if (v == LVS_LIST) SetRect(r, 0, 0, ((s->n + g.per - 1) / g.per) * g.cw, g.per * g.ch);
        else SetRect(r, 0, -s->sy, g.per * g.cw, ((s->n + g.per - 1) / g.per) * g.ch - s->sy);
        return TRUE;
    }
    case LVM_APPROXIMATEVIEWRECT: {
        Grid g; grid(h, s, &g);
        int n = (int)wp < 0 ? s->n : (int)wp;
        return MAKELONG(MIN(total_width(s), 0x7FFF), MIN(n * g.ch + hdr_h(h, s), 0x7FFF));
    }
    case LVM_SCROLL: {
        int v = view_of(h);
        if (v == LVS_REPORT) {
            if ((int)lp) scroll_to(h, s, SB_VERT, s->top + (int)lp / row_h(h, s));
            if ((int)wp) scroll_to(h, s, SB_HORZ, s->sx + (int)wp);
        } else if (v == LVS_LIST) {
            Grid g; grid(h, s, &g);
            if ((int)wp) scroll_to(h, s, SB_HORZ, s->top + (int)wp / g.cw);
        } else if ((int)lp) scroll_to(h, s, SB_VERT, s->sy + (int)lp);
        return TRUE;
    }
    case LVM_ARRANGE: refresh(h, s); return TRUE;
    case LVM_GETSTRINGWIDTHA: {
        WCHAR w[520];
        if (!lp) return 0;
        MultiByteToWideChar(CP_ACP, 0, (const char *)lp, -1, w, 520);
        return text_px(h, s, w);
    }
    case LVM_GETSTRINGWIDTHW: return lp ? text_px(h, s, (const WCHAR *)lp) : 0;
    case LVM_GETITEMSPACING: {
        Grid g; int v = view_of(h);
        LONG st = GetWindowLongW(h, GWL_STYLE);
        SetWindowLongW(h, GWL_STYLE, (st & ~LVS_TYPEMASK) | (wp ? LVS_SMALLICON : LVS_ICON));
        grid(h, s, &g);
        SetWindowLongW(h, GWL_STYLE, (st & ~LVS_TYPEMASK) | v);
        return MAKELONG(g.cw, g.ch);
    }
    case LVM_SETICONSPACING: return MAKELONG(76, 76);

    /* ---- columns ---- */
    case LVM_INSERTCOLUMNA_: wide = 0; /* fall through */
    case LVM_INSERTCOLUMNW: return insert_column(h, s, (int)wp, (const LVCOLUMNW *)lp, wide);
    case LVM_DELETECOLUMN: {
        int i = (int)wp;
        if (i < 0 || i >= s->ncol) return FALSE;
        memmove(&s->col[i], &s->col[i + 1], sizeof(LCol) * (size_t)(s->ncol - i - 1));
        s->ncol--;
        if (s->hdr) SendMessageW(s->hdr, HDM_DELETEITEM, (WPARAM)i, 0);
        refresh(h, s);
        return TRUE;
    }
    case LVM_GETCOLUMNA: wide = 0; /* fall through */
    case LVM_GETCOLUMNW: return get_column(s, (int)wp, (LVCOLUMNW *)lp, wide);
    case LVM_SETCOLUMNA: wide = 0; /* fall through */
    case LVM_SETCOLUMNW: return set_column(h, s, (int)wp, (const LVCOLUMNW *)lp, wide);
    case LVM_GETCOLUMNWIDTH: {
        int v = view_of(h);
        if (v == LVS_LIST) { Grid g; grid(h, s, &g); return g.cw; }
        return (int)wp >= 0 && (int)wp < s->ncol ? s->col[wp].cx : 0;
    }
    case LVM_SETCOLUMNWIDTH: return set_column_width(h, s, (int)wp, (short)LOWORD(lp));
    case LVM_GETCOLUMNORDERARRAY: return s->hdr ? SendMessageW(s->hdr, HDM_GETORDERARRAY, wp, lp) : FALSE;
    case LVM_SETCOLUMNORDERARRAY: {
        LRESULT r = s->hdr ? SendMessageW(s->hdr, HDM_SETORDERARRAY, wp, lp) : FALSE;
        refresh(h, s);
        return r;
    }
    case LVM_GETHEADER: return (LRESULT)s->hdr;
    case LVM_SETSELECTEDCOLUMN: InvalidateRect(h, NULL, TRUE); return 0;
    case LVM_GETSELECTEDCOLUMN: return -1;

    /* ---- looks ---- */
    case LVM_SETIMAGELIST: {
        int k = (int)wp;
        if (k < 0 || k > 2) return 0;
        HIMAGELIST o = s->il[k];
        s->il[k] = (HIMAGELIST)lp;
        refresh(h, s);
        return (LRESULT)o;
    }
    case LVM_GETIMAGELIST: return (int)wp >= 0 && (int)wp <= 2 ? (LRESULT)s->il[wp] : 0;
    case LVM_SETBKCOLOR: s->bk = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return TRUE;
    case LVM_GETBKCOLOR: return s->bk == CLR_DEFAULT ? GetSysColor(COLOR_WINDOW) : s->bk;
    case LVM_SETTEXTCOLOR: s->text = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return TRUE;
    case LVM_GETTEXTCOLOR: return c_text(s);
    case LVM_SETTEXTBKCOLOR: s->textbk = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return TRUE;
    case LVM_GETTEXTBKCOLOR: return s->textbk;
    case LVM_SETEXTENDEDLISTVIEWSTYLE: {
        DWORD o = s->ex, mask = wp ? (DWORD)wp : 0xFFFFFFFFu;
        s->ex = (s->ex & ~mask) | ((DWORD)lp & mask);
        if ((s->ex ^ o) & LVS_EX_CHECKBOXES) {
            for (int i = 0; i < s->n && !ownerdata(h); i++)
                if ((s->ex & LVS_EX_CHECKBOXES) && !(s->it[i].state & LVIS_STATEIMAGEMASK)) s->it[i].state |= INDEXTOSTATEIMAGEMASK(1);
        }
        if (s->hdr && ((s->ex ^ o) & LVS_EX_HEADERDRAGDROP)) {
            LONG hs = GetWindowLongW(s->hdr, GWL_STYLE);
            SetWindowLongW(s->hdr, GWL_STYLE, (s->ex & LVS_EX_HEADERDRAGDROP) ? hs | HDS_DRAGDROP : hs & ~HDS_DRAGDROP);
        }
        refresh(h, s);
        return (LRESULT)o;
    }
    case LVM_GETEXTENDEDLISTVIEWSTYLE: return (LRESULT)s->ex;
    case LVM_SETVIEW: {
        int v = (int)wp;
        int st = v == LV_VIEW_DETAILS ? LVS_REPORT : v == LV_VIEW_SMALLICON ? LVS_SMALLICON : v == LV_VIEW_LIST ? LVS_LIST : v == LV_VIEW_ICON ? LVS_ICON : -1;
        if (st < 0) return -1;
        set_view(h, s, st);
        return 1;
    }
    case LVM_GETVIEW: {
        int v = view_of(h);
        return v == LVS_REPORT ? LV_VIEW_DETAILS : v == LVS_SMALLICON ? LV_VIEW_SMALLICON : v == LVS_LIST ? LV_VIEW_LIST : LV_VIEW_ICON;
    }
    case LVM_SETHOTITEM: { int o = s->hot; s->hot = (int)wp; return o; }
    case LVM_GETHOTITEM: return s->hot;
    case LVM_SETHOTCURSOR: return 0;
    case LVM_SETHOVERTIME: return 0;
    case LVM_SETTOOLTIPS: { HWND o = s->tips; s->tips = (HWND)wp; return (LRESULT)o; }
    case LVM_GETTOOLTIPS: return (LRESULT)s->tips;
    case LVM_ENABLEGROUPVIEW: return 0;
    case LVM_SETUNICODEFORMAT_: { int o = !s->ansi; s->ansi = !wp; return o; }
    case LVM_GETUNICODEFORMAT_: return !s->ansi;

    /* ---- editing ---- */
    case LVM_EDITLABELA: case LVM_EDITLABELW: return (LRESULT)edit_label(h, s, (int)wp);
    case LVM_GETEDITCONTROL: return (LRESULT)s->edit;
    case LVM_CANCELEDITLABEL_: end_edit(h, s, TRUE); return 0;
    case LVM_CREATEDRAGIMAGE: return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
