/*
 * header.c — the header control (SysHeader32): the column titles above a
 * list view. Items can be clicked (HDS_BUTTONS), resized by dragging or
 * double-clicking the divider after them, carry an image and a sort arrow,
 * and have a display order separate from their index.
 */
#include "cc.h"

#define HDM_INSERTITEMA_ (HDM_FIRST + 1)
#define HDM_GETITEMA_    (HDM_FIRST + 3)
#define HDM_SETITEMA_    (HDM_FIRST + 4)
#define HDM_SETHOTDIVIDER_ (HDM_FIRST + 19)
#define HDM_GETBITMAPMARGIN_ (HDM_FIRST + 21)
#define HDM_SETUNICODEFORMAT_ 0x2005
#define HDM_GETUNICODEFORMAT_ 0x2006
#define HDN_ITEMDBLCLICKW_ (HDN_FIRST - 23)
#define HDN_ENDDRAG_       (HDN_FIRST - 11)
#define DIVIDER 5                       /* the grab zone either side of a divider (at 96 DPI) */

typedef struct { WCHAR *text; int cx, fmt, image, order; LPARAM lp; } HItem;

typedef struct {
    HItem *it;
    int n, cap;
    HFONT font;
    HIMAGELIST himl;
    int hot;                            /* item under the mouse, -1 none */
    int pressed;                        /* item held down, -1 none */
    int track;                          /* item whose divider is dragged, -1 none */
    int track_x, track_cx;              /* where the drag started, and the width then */
    int down_x;
} Hdr;

static int *order_list(Hdr *s)          /* index by display position (malloc) */
{
    int *o = malloc(sizeof(int) * (size_t)(s->n ? s->n : 1));
    if (!o) return NULL;
    for (int i = 0; i < s->n; i++) o[i] = i;
    for (int i = 0; i < s->n; i++) {
        int p = s->it[i].order;
        if (p >= 0 && p < s->n) o[p] = i;
    }
    return o;
}

static void renumber(Hdr *s)            /* orders made 0..n-1, keeping their sequence */
{
    int *rank = malloc(sizeof(int) * (size_t)(s->n ? s->n : 1));
    if (!rank) return;
    for (int i = 0; i < s->n; i++) {
        int r = 0;
        for (int j = 0; j < s->n; j++)
            if (s->it[j].order < s->it[i].order || (s->it[j].order == s->it[i].order && j < i)) r++;
        rank[i] = r;
    }
    for (int i = 0; i < s->n; i++) s->it[i].order = rank[i];
    free(rank);
}

static BOOL item_rect(HWND h, Hdr *s, int idx, RECT *r)
{
    if (idx < 0 || idx >= s->n) return FALSE;
    RECT c;
    GetClientRect(h, &c);
    int x = 0;
    for (int p = 0; p < s->n; p++) {
        int i = -1;
        for (int k = 0; k < s->n; k++) if (s->it[k].order == p) { i = k; break; }
        if (i < 0) continue;
        if (i == idx) { SetRect(r, x, 0, x + s->it[i].cx, c.bottom); return TRUE; }
        x += s->it[i].cx;
    }
    return FALSE;
}

static int hit(HWND h, Hdr *s, POINT pt, UINT *flags)
{
    RECT c;
    GetClientRect(h, &c);
    *flags = HHT_NOWHERE;
    if (pt.y < 0 || pt.y >= c.bottom) return -1;
    int *o = order_list(s);
    if (!o) return -1;
    int x = 0, found = -1, dz = DIVIDER * cc_k(h);
    for (int p = 0; p < s->n; p++) {
        int i = o[p], r = x + s->it[i].cx;
        /* the divider after an item: its right edge, or the left edge of the next one */
        if (pt.x >= r - dz && pt.x < r + dz) {
            int next_zero = p + 1 < s->n && s->it[o[p + 1]].cx == 0;
            if (pt.x < r || !next_zero) { *flags = HHT_ONDIVIDER; found = i; break; }
        }
        if (pt.x >= x && pt.x < r) { *flags = HHT_ONHEADER; found = i; }
        x = r;
    }
    free(o);
    return found;
}

static LRESULT notify_item(HWND h, UINT code, int i, int button, HDITEMW *item)
{
    NMHEADERW nm;
    memset(&nm, 0, sizeof(nm));
    nm.iItem = i;
    nm.iButton = button;
    nm.pitem = item;
    return cc_notify(h, code, &nm.hdr);
}

static void set_text(WCHAR **slot, const void *t, int wide)
{
    wfree(*slot);
    *slot = NULL;
    if (!t) return;
    if (wide || t == LPSTR_TEXTCALLBACKW) { *slot = wdup(t); return; }
    int n = MultiByteToWideChar(CP_ACP, 0, t, -1, NULL, 0);
    *slot = malloc(2 * (size_t)(n > 0 ? n : 1));
    if (*slot) { if (n > 0) MultiByteToWideChar(CP_ACP, 0, t, -1, *slot, n); else (*slot)[0] = 0; }
}

static void apply(Hdr *s, HItem *it, const HDITEMW *in, int wide)
{
    if (in->mask & HDI_WIDTH) it->cx = in->cxy < 0 ? 0 : in->cxy;
    if (in->mask & HDI_TEXT) set_text(&it->text, in->pszText, wide);
    if (in->mask & HDI_FORMAT) it->fmt = in->fmt;
    if (in->mask & HDI_LPARAM) it->lp = in->lParam;
    if (in->mask & HDI_IMAGE) it->image = in->iImage;
    if (in->mask & HDI_ORDER) {
        int want = in->iOrder < 0 ? 0 : in->iOrder >= s->n ? s->n - 1 : in->iOrder;
        int old = it->order;
        for (int i = 0; i < s->n; i++) {
            HItem *o = &s->it[i];
            if (o == it) continue;
            if (old < want && o->order > old && o->order <= want) o->order--;
            else if (old > want && o->order >= want && o->order < old) o->order++;
        }
        it->order = want;
    }
}

static int insert(HWND h, Hdr *s, int at, const HDITEMW *in, int wide)
{
    (void)h;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 8;
        HItem *p = realloc(s->it, sizeof(HItem) * (size_t)c);
        if (!p) return -1;
        s->it = p; s->cap = c;
    }
    if (at < 0 || at > s->n) at = s->n;
    memmove(&s->it[at + 1], &s->it[at], sizeof(HItem) * (size_t)(s->n - at));
    HItem *it = &s->it[at];
    memset(it, 0, sizeof(*it));
    it->image = -1;
    for (int i = 0; i < s->n + 1; i++) if (i != at && s->it[i].order >= at) s->it[i].order++;
    it->order = at;
    s->n++;
    HDITEMW c = *in;
    c.mask &= ~HDI_ORDER;
    apply(s, it, &c, wide);
    if (in->mask & HDI_ORDER) { c.mask = HDI_ORDER; c.iOrder = in->iOrder; apply(s, it, &c, wide); }
    renumber(s);
    return at;
}

static void get_item(Hdr *s, int i, HDITEMW *out, int wide)
{
    HItem *it = &s->it[i];
    if (out->mask & HDI_WIDTH) out->cxy = it->cx;
    if (out->mask & HDI_FORMAT) out->fmt = it->fmt;
    if (out->mask & HDI_LPARAM) out->lParam = it->lp;
    if (out->mask & HDI_IMAGE) out->iImage = it->image;
    if (out->mask & HDI_ORDER) out->iOrder = it->order;
    if ((out->mask & HDI_TEXT) && out->pszText && out->cchTextMax > 0) {
        const WCHAR *t = it->text && it->text != LPSTR_TEXTCALLBACKW ? it->text : L"";
        if (wide) {
            int n = MIN(wlen(t), out->cchTextMax - 1);
            memcpy(out->pszText, t, 2 * (size_t)n);
            out->pszText[n] = 0;
        } else if (!WideCharToMultiByte(CP_ACP, 0, t, -1, (char *)out->pszText, out->cchTextMax, NULL, NULL)) {
            ((char *)out->pszText)[out->cchTextMax - 1] = 0;
        }
    }
}

static int height(HWND h, Hdr *s)
{
    int fh = cc_font_h(s->font ? s->font : cc_font_for(h)), k = cc_k(h);
    return fh + 10 * k < 24 * k ? 24 * k : fh + 10 * k;
}

static void paint(HWND h, Hdr *s, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    SetBkMode(dc, TRANSPARENT);
    COLORREF face = RGB(255, 255, 255), line = RGB(229, 229, 229), text = RGB(76, 96, 122);
    DWORD style = GetWindowLongW(h, GWL_STYLE);
    int *o = order_list(s);
    int x = 0, k = cc_k(h);
    for (int p = 0; o && p < s->n; p++) {
        HItem *it = &s->it[o[p]];
        RECT r = { x, 0, x + it->cx, c.bottom };
        x = r.right;
        if (r.right <= r.left) continue;
        COLORREF bg = face;
        if ((style & HDS_BUTTONS) && s->pressed == o[p]) bg = RGB(188, 220, 244);
        else if ((style & HDS_BUTTONS) && s->hot == o[p]) bg = RGB(217, 235, 249);
        cc_fill(dc, &r, bg);
        RECT d = { r.right - k, r.top + 3 * k, r.right, r.bottom - 3 * k };
        cc_fill(dc, &d, line);
        RECT t = { r.left + 6 * k, r.top, r.right - 6 * k, r.bottom };
        if (s->pressed == o[p]) OffsetRect(&t, k, k);
        if ((it->fmt & HDF_IMAGE) && s->himl && it->image >= 0) {
            int iw, ih;
            ImageList_GetIconSize(s->himl, &iw, &ih);
            if (t.right - t.left > iw) {
                il_draw(s->himl, it->image, dc, t.left, (r.top + r.bottom - ih) / 2, ILD_TRANSPARENT);
                t.left += iw + 4 * k;
            }
        }
        if (it->fmt & (HDF_SORTUP | HDF_SORTDOWN)) {             /* the sort arrow, above the text as in Explorer */
            RECT a = { (r.left + r.right) / 2 - 5 * k, r.top, (r.left + r.right) / 2 + 5 * k, r.top + 7 * k };
            cc_arrow(dc, &a, (it->fmt & HDF_SORTUP) ? 0 : 1, RGB(150, 160, 175));
        }
        SetTextColor(dc, text);
        if (it->text && it->text != LPSTR_TEXTCALLBACKW) {
            UINT fl = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            int j = it->fmt & HDF_JUSTIFYMASK;
            fl |= j == HDF_RIGHT ? DT_RIGHT : j == HDF_CENTER ? DT_CENTER : DT_LEFT;
            DrawTextW(dc, it->text, -1, &t, fl);
        }
    }
    free(o);
    if (x < c.right) { RECT r = { x, 0, c.right, c.bottom }; cc_fill(dc, &r, face); }
    RECT b = { 0, c.bottom - k, c.right, c.bottom };
    cc_fill(dc, &b, line);
    SelectObject(dc, of);
}

static void set_width(HWND h, Hdr *s, int i, int cx, BOOL track)
{
    if (cx < 0) cx = 0;
    HDITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = HDI_WIDTH;
    item.cxy = cx;
    if (track && notify_item(h, HDN_TRACKW, i, 0, &item)) return;
    if (notify_item(h, HDN_ITEMCHANGINGW, i, 0, &item)) return;
    s->it[i].cx = cx;
    InvalidateRect(h, NULL, FALSE);
    notify_item(h, HDN_ITEMCHANGEDW, i, 0, &item);
}

LRESULT CALLBACK HeaderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Hdr *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        s = calloc(1, sizeof(Hdr));
        if (!s) return FALSE;
        s->hot = s->pressed = s->track = -1;
        ctl_set(h, s);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NCDESTROY:
        for (int i = 0; i < s->n; i++) wfree(s->it[i].text);
        free(s->it);
        free(s);
        ctl_set(h, NULL);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(h, s, dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: paint(h, s, (HDC)wp); return 0;
    case WM_SETFONT: s->font = (HFONT)wp; if (lp) InvalidateRect(h, NULL, TRUE); return 0;
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        UINT f;
        if (s->track >= 0 || (hit(h, s, pt, &f) >= 0 && f == HHT_ONDIVIDER)) { SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEWE)); return TRUE; }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (s->track >= 0) { set_width(h, s, s->track, s->track_cx + pt.x - s->track_x, TRUE); UpdateWindow(h); return 0; }
        UINT f;
        int i = hit(h, s, pt, &f);
        if (f != HHT_ONHEADER) i = -1;
        if (s->pressed >= 0) {
            RECT r;
            item_rect(h, s, s->pressed, &r);
            return 0;
        }
        if (i != s->hot) {
            s->hot = i;
            InvalidateRect(h, NULL, FALSE);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE: if (s->hot >= 0) { s->hot = -1; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        UINT f;
        int i = hit(h, s, pt, &f);
        if (i < 0) return 0;
        if (f == HHT_ONDIVIDER) {
            if (msg == WM_LBUTTONDBLCLK) { notify_item(h, HDN_DIVIDERDBLCLICKW, i, 0, NULL); return 0; }
            HDITEMW item; memset(&item, 0, sizeof(item)); item.mask = HDI_WIDTH; item.cxy = s->it[i].cx;
            if (notify_item(h, HDN_BEGINTRACKW, i, 0, &item)) return 0;
            s->track = i; s->track_x = pt.x; s->track_cx = s->it[i].cx;
            SetCapture(h);
            return 0;
        }
        if (msg == WM_LBUTTONDBLCLK) notify_item(h, HDN_ITEMDBLCLICKW_, i, 0, NULL);
        if (GetWindowLongW(h, GWL_STYLE) & HDS_BUTTONS) {
            s->pressed = i; s->down_x = pt.x;
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (s->track >= 0) {
            int i = s->track;
            s->track = -1;
            ReleaseCapture();
            HDITEMW item; memset(&item, 0, sizeof(item)); item.mask = HDI_WIDTH; item.cxy = s->it[i].cx;
            notify_item(h, HDN_ENDTRACKW, i, 0, &item);
            return 0;
        }
        if (s->pressed >= 0) {
            int i = s->pressed;
            s->pressed = -1;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            RECT r;
            if (item_rect(h, s, i, &r) && PtInRect(&r, pt)) notify_item(h, HDN_ITEMCLICKW, i, 0, NULL);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h) { s->track = -1; if (s->pressed >= 0) { s->pressed = -1; InvalidateRect(h, NULL, FALSE); } }
        return 0;
    case WM_RBUTTONUP: cc_notify(h, NM_RCLICK, NULL); return DefWindowProcW(h, msg, wp, lp);

    case HDM_GETITEMCOUNT: return s->n;
    case HDM_INSERTITEMW: case HDM_INSERTITEMA_: {
        if (!lp) return -1;
        int r = insert(h, s, (int)wp, (const HDITEMW *)lp, msg == HDM_INSERTITEMW);
        InvalidateRect(h, NULL, FALSE);
        return r;
    }
    case HDM_DELETEITEM: {
        int i = (int)wp;
        if (i < 0 || i >= s->n) return FALSE;
        int ord = s->it[i].order;
        wfree(s->it[i].text);
        memmove(&s->it[i], &s->it[i + 1], sizeof(HItem) * (size_t)(s->n - i - 1));
        s->n--;
        for (int k = 0; k < s->n; k++) if (s->it[k].order > ord) s->it[k].order--;
        if (s->hot >= s->n) s->hot = -1;
        InvalidateRect(h, NULL, FALSE);
        return TRUE;
    }
    case HDM_GETITEMW: case HDM_GETITEMA_: {
        int i = (int)wp;
        if (i < 0 || i >= s->n || !lp) return FALSE;
        get_item(s, i, (HDITEMW *)lp, msg == HDM_GETITEMW);
        return TRUE;
    }
    case HDM_SETITEMW: case HDM_SETITEMA_: {
        int i = (int)wp;
        if (i < 0 || i >= s->n || !lp) return FALSE;
        HDITEMW *in = (HDITEMW *)lp;
        if (notify_item(h, HDN_ITEMCHANGINGW, i, 0, in)) return FALSE;
        apply(s, &s->it[i], in, msg == HDM_SETITEMW);
        InvalidateRect(h, NULL, FALSE);
        notify_item(h, HDN_ITEMCHANGEDW, i, 0, in);
        return TRUE;
    }
    case HDM_LAYOUT: {
        HDLAYOUT *l = (HDLAYOUT *)lp;
        if (!l || !l->prc || !l->pwpos) return FALSE;
        int hh = (GetWindowLongW(h, GWL_STYLE) & HDS_HIDDEN) ? 0 : height(h, s);
        l->pwpos->hwnd = h; l->pwpos->hwndInsertAfter = NULL;
        l->pwpos->x = l->prc->left; l->pwpos->y = l->prc->top;
        l->pwpos->cx = l->prc->right - l->prc->left; l->pwpos->cy = hh;
        l->pwpos->flags = hh ? SWP_NOZORDER | SWP_NOACTIVATE : SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW;
        l->prc->top += hh;
        return TRUE;
    }
    case HDM_HITTEST: {
        HDHITTESTINFO *t = (HDHITTESTINFO *)lp;
        if (!t) return -1;
        t->iItem = hit(h, s, t->pt, &t->flags);
        return t->iItem;
    }
    case HDM_GETITEMRECT: return item_rect(h, s, (int)wp, (RECT *)lp);
    case HDM_SETIMAGELIST: { HIMAGELIST o = s->himl; s->himl = (HIMAGELIST)lp; InvalidateRect(h, NULL, FALSE); return (LRESULT)o; }
    case HDM_GETIMAGELIST: return (LRESULT)s->himl;
    case HDM_ORDERTOINDEX: { for (int i = 0; i < s->n; i++) if (s->it[i].order == (int)wp) return i; return (LRESULT)wp; }
    case HDM_GETORDERARRAY: {
        if ((int)wp < s->n || !lp) return FALSE;
        int *o = order_list(s);
        if (!o) return FALSE;
        memcpy((void *)lp, o, sizeof(int) * (size_t)s->n);
        free(o);
        return TRUE;
    }
    case HDM_SETORDERARRAY: {
        if ((int)wp != s->n || !lp) return FALSE;
        const int *o = (const int *)lp;
        for (int p = 0; p < s->n; p++) if (o[p] >= 0 && o[p] < s->n) s->it[o[p]].order = p;
        renumber(s);
        InvalidateRect(h, NULL, FALSE);
        return TRUE;
    }
    case HDM_SETHOTDIVIDER_: return -1;
    case HDM_GETBITMAPMARGIN_: return 6 * cc_k(h);
    case HDM_SETUNICODEFORMAT_: return TRUE;
    case HDM_GETUNICODEFORMAT_: return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
