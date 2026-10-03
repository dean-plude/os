/*
 * treeview.c — the tree view (SysTreeView32).
 *
 * Items form a tree; the ones whose ancestors are all expanded are laid
 * out in a flat list of rows, scrolled by row. Each row shows the tree
 * lines and expand button (TVS_HASLINES, TVS_HASBUTTONS), a state image
 * or check box, the item's image and its label. Text, images and whether
 * an item has children can come from the parent (LPSTR_TEXTCALLBACK,
 * I_IMAGECALLBACK, I_CHILDRENCALLBACK: TVN_GETDISPINFO). Expanding and
 * selecting go through the usual TVN_* notifications, there is label
 * editing, custom draw, type-ahead search and drag start.
 */
#include "cc.h"

#define TV_MAGIC 0x54564954u
#define EDIT_TIMER 0x5456
#define CDIS_SELECTED_ 0x0001
#define CDIS_FOCUS_    0x0010
#define NF_QUERY_ 3
#define TVM_SETUNICODEFORMAT_ 0x2005
#define TVM_GETUNICODEFORMAT_ 0x2006

typedef struct TItem {
    UINT magic;
    struct TItem *parent, *first, *last, *next, *prev;
    WCHAR *text;
    int image, sel_image, children;     /* children: I_CHILDRENCALLBACK asks the parent */
    UINT state;
    LPARAM lp;
    int integral;
    int level;                          /* depth (0 for the roots), kept while laid out */
    int row;                            /* index in the flat list while shown, else -1 */
} TItem;

typedef struct {
    TItem root;                         /* the invisible parent of the roots */
    int count;
    HIMAGELIST il, sil;
    HFONT font;
    int fh;
    int indent, item_h;                 /* indent 0: 19 at 96 DPI; item_set: TVM_SETITEMHEIGHT's */
    int item_set;
    COLORREF bk, text, line, insmark;
    TItem *sel, *drop, *hot;
    TItem **vis;                        /* the rows: items whose ancestors are expanded */
    int nvis, viscap, vis_ok;
    int top, sx;
    HWND edit;
    TItem *edit_item;
    int edit_pending;
    int ansi;
    int down, dragging;
    TItem *down_item;
    POINT down_pt;
    WCHAR search[64];
    int nsearch;
    DWORD search_tick;
    DWORD ex;
    HWND tips;
    TItem *ins_mark;
    int ins_after;
} TV;

/* -----------------------------------------------------------------------
 * Items
 * ----------------------------------------------------------------------- */
static DWORD style_of(HWND h) { return (DWORD)GetWindowLongW(h, GWL_STYLE); }

static TItem *item_of(TV *s, HTREEITEM h)
{
    TItem *it = (TItem *)h;
    if (h == TVI_ROOT) return &s->root;
    if ((ULONG_PTR)h < 0x10000 || it->magic != TV_MAGIC) return NULL;
    return it;
}

static HTREEITEM handle_of(TV *s, TItem *it) { return !it || it == &s->root ? NULL : (HTREEITEM)it; }

static int has_children(TItem *it) { return it->first != NULL || it->children != 0; }

static void invalidate_rows(TV *s) { s->vis_ok = 0; }

/* the flat list: a pre-order walk that does not enter collapsed items */
static void add_row(TV *s, TItem *it, int level)
{
    if (s->nvis == s->viscap) {
        int c = s->viscap ? s->viscap * 2 : 64;
        TItem **p = realloc(s->vis, sizeof(TItem *) * (size_t)c);
        if (!p) return;
        s->vis = p; s->viscap = c;
    }
    it->level = level;
    it->row = s->nvis;
    s->vis[s->nvis++] = it;
    if (it->state & TVIS_EXPANDED) for (TItem *c = it->first; c; c = c->next) add_row(s, c, level + 1);
}

static void clear_rows(TItem *it)
{
    it->row = -1;
    for (TItem *c = it->first; c; c = c->next) clear_rows(c);
}

static void rows(TV *s)
{
    if (s->vis_ok) return;
    clear_rows(&s->root);
    s->nvis = 0;
    for (TItem *c = s->root.first; c; c = c->next) add_row(s, c, 0);
    s->vis_ok = 1;
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

/* -----------------------------------------------------------------------
 * Notifications
 * ----------------------------------------------------------------------- */
static void fill_item(TV *s, TItem *it, TVITEMW *out)
{
    memset(out, 0, sizeof(*out));
    if (!it || it == &s->root) return;
    out->mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    out->hItem = (HTREEITEM)it;
    out->state = it->state;
    out->stateMask = 0xFFFF;
    out->lParam = it->lp;
    out->iImage = it->image;
    out->iSelectedImage = it->sel_image;
}

static LRESULT notify_tv(HWND h, TV *s, UINT code, UINT action, TItem *old, TItem *nw, POINT pt)
{
    NMTREEVIEWW nm;
    memset(&nm, 0, sizeof(nm));
    nm.action = action;
    fill_item(s, old, &nm.itemOld);
    fill_item(s, nw, &nm.itemNew);
    nm.ptDrag = pt;
    if (s->ansi) code += 49;                                /* the A code (TVN_FIRST - n + 49) */
    return cc_notify(h, code, &nm.hdr);
}

static void dispinfo(HWND h, TV *s, TItem *it, UINT mask, TVITEMW *out, WCHAR *buf, int cap)
{
    NMTVDISPINFOW di;
    memset(&di, 0, sizeof(di));
    di.item.mask = mask;
    di.item.hItem = (HTREEITEM)it;
    di.item.lParam = it->lp;
    di.item.state = it->state;
    di.item.stateMask = 0xFFFF;
    di.item.iImage = it->image; di.item.iSelectedImage = it->sel_image;
    di.item.cChildren = it->children;
    char abuf[520];
    if (mask & TVIF_TEXT) {
        if (s->ansi) { abuf[0] = 0; di.item.pszText = (LPWSTR)abuf; di.item.cchTextMax = (int)sizeof abuf; }
        else { buf[0] = 0; di.item.pszText = buf; di.item.cchTextMax = cap; }
    }
    cc_notify(h, s->ansi ? TVN_GETDISPINFOA : TVN_GETDISPINFOW, &di.hdr);
    *out = di.item;
    if (mask & TVIF_TEXT) {
        if (!di.item.pszText) { buf[0] = 0; out->pszText = buf; }
        else if (s->ansi) { if (!MultiByteToWideChar(CP_ACP, 0, (const char *)di.item.pszText, -1, buf, cap)) buf[0] = 0; out->pszText = buf; }
    }
    if (di.item.mask & TVIF_DI_SETITEM) {                   /* keep what the parent gave */
        if (mask & TVIF_TEXT && out->pszText) set_text(&it->text, out->pszText, 1);
        if (mask & TVIF_IMAGE) it->image = out->iImage;
        if (mask & TVIF_SELECTEDIMAGE) it->sel_image = out->iSelectedImage;
        if (mask & TVIF_CHILDREN) it->children = out->cChildren;
    }
}

static const WCHAR *item_text(HWND h, TV *s, TItem *it, WCHAR *buf, int cap)
{
    if (it->text != LPSTR_TEXTCALLBACKW) return it->text ? it->text : L"";
    TVITEMW r;
    dispinfo(h, s, it, TVIF_TEXT, &r, buf, cap);
    return r.pszText ? r.pszText : L"";
}

static int item_image(HWND h, TV *s, TItem *it, int selected)
{
    int v = selected ? it->sel_image : it->image;
    if (v != I_IMAGECALLBACK) return v;
    WCHAR b[2];
    TVITEMW r;
    dispinfo(h, s, it, selected ? TVIF_SELECTEDIMAGE : TVIF_IMAGE, &r, b, 2);
    return selected ? r.iSelectedImage : r.iImage;
}

static int item_children(HWND h, TV *s, TItem *it)
{
    if (it->first) return 1;
    if (it->children != I_CHILDRENCALLBACK) return it->children;
    WCHAR b[2];
    TVITEMW r;
    dispinfo(h, s, it, TVIF_CHILDREN, &r, b, 2);
    return r.cChildren;
}

/* -----------------------------------------------------------------------
 * Layout
 * ----------------------------------------------------------------------- */
static int row_h(TV *s) { return s->item_h; }
static int indent(HWND h, TV *s) { return s->indent ? s->indent : 19 * cc_k(h); }

static int default_item_h(HWND h, TV *s)
{
    int ih = 0, iw;
    if (s->il) ImageList_GetIconSize(s->il, &iw, &ih);
    int k = cc_k(h), r = MAX(s->fh + 4 * k, ih + 2 * k);
    if (r < 16 * k) r = 16 * k;
    if (!(style_of(h) & TVS_NONEVENHEIGHT) && (r & 1)) r++;
    return r;
}

static int rows_visible(HWND h, TV *s)
{
    RECT c;
    GetClientRect(h, &c);
    int n = c.bottom / row_h(s);
    return n < 1 ? 1 : n;
}

static int text_w(HWND h, TV *s, TItem *it)
{
    WCHAR b[520];
    const WCHAR *t = item_text(h, s, it, b, 520);
    HDC dc = GetDC(h);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    int w = cc_text_w(dc, t, -1);
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    return w;
}

/* where the content (state image, icon, label) of a row at LEVEL starts */
static int content_x(HWND h, TV *s, int level)
{
    DWORD st = style_of(h);
    int glyph = (st & (TVS_HASBUTTONS | TVS_HASLINES)) != 0;
    int x = level * indent(h, s);
    if (glyph && (st & TVS_LINESATROOT)) x += indent(h, s);
    return x - s->sx;
}

/* the item's parts, client coordinates: whole row, button, state, icon, label */
typedef struct { RECT row, button, state, icon, label; } Parts;

static BOOL parts(HWND h, TV *s, TItem *it, Parts *p)
{
    rows(s);
    if (!it || it == &s->root || it->row < 0) return FALSE;
    RECT c;
    GetClientRect(h, &c);
    int rh = row_h(s);
    int y = (it->row - s->top) * rh;
    int x = content_x(h, s, it->level);
    SetRect(&p->row, 0, y, c.right, y + rh);
    SetRect(&p->button, x - indent(h, s), y, x, y + rh);
    int k = cc_k(h);
    int sw = 0, sh = 0;
    if (s->sil) ImageList_GetIconSize(s->sil, &sw, &sh);
    else if (style_of(h) & TVS_CHECKBOXES) { sw = 16 * k; sh = 16 * k; }
    SetRect(&p->state, x, y, x + (sw ? sw + 2 * k : 0), y + rh);
    x = p->state.right;
    int iw = 0, ih = 0;
    if (s->il) ImageList_GetIconSize(s->il, &iw, &ih);
    SetRect(&p->icon, x, y, x + (iw ? iw + 3 * k : 0), y + rh);
    x = p->icon.right;
    int tw = text_w(h, s, it);
    SetRect(&p->label, x, y, x + tw + 6 * k, y + rh);
    return TRUE;
}

static int content_width(HWND h, TV *s)
{
    rows(s);
    int w = 0, save = s->sx;
    s->sx = 0;
    for (int i = 0; i < s->nvis; i++) {
        Parts p;
        if (parts(h, s, s->vis[i], &p) && p.label.right > w) w = p.label.right;
    }
    s->sx = save;
    return w + 4 * cc_k(h);
}

static void update_scroll(HWND h, TV *s)
{
    rows(s);
    DWORD st = style_of(h);
    if (st & TVS_NOSCROLL) return;
    RECT c;
    GetClientRect(h, &c);
    int vis = rows_visible(h, s);
    if (s->top > MAX(0, s->nvis - vis)) s->top = MAX(0, s->nvis - vis);
    if (s->top < 0) s->top = 0;
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, s->nvis - 1, (UINT)vis, s->top, 0 };
    SetScrollInfo(h, SB_VERT, &si, TRUE);
    if (!(st & TVS_NOHSCROLL)) {
        GetClientRect(h, &c);
        int cw = content_width(h, s);
        if (s->sx > MAX(0, cw - c.right)) s->sx = MAX(0, cw - c.right);
        SCROLLINFO hs = { sizeof(hs), SIF_RANGE | SIF_PAGE | SIF_POS, 0, cw - 1, (UINT)c.right, s->sx, 0 };
        SetScrollInfo(h, SB_HORZ, &hs, TRUE);
    }
}

static void refresh(HWND h, TV *s) { invalidate_rows(s); update_scroll(h, s); InvalidateRect(h, NULL, TRUE); }

static void inval_item(HWND h, TV *s, TItem *it)
{
    Parts p;
    if (parts(h, s, it, &p)) InvalidateRect(h, &p.row, TRUE);
}

static void ensure_visible(HWND h, TV *s, TItem *it)
{
    if (!it || it == &s->root) return;
    /* expand the ancestors */
    int changed = 0;
    for (TItem *a = it->parent; a && a != &s->root; a = a->parent)
        if (!(a->state & TVIS_EXPANDED)) { a->state |= TVIS_EXPANDED | TVIS_EXPANDEDONCE; changed = 1; }
    if (changed) refresh(h, s);
    rows(s);
    if (it->row < 0) return;
    int vis = rows_visible(h, s);
    if (it->row < s->top) s->top = it->row;
    else if (it->row >= s->top + vis) s->top = it->row - vis + 1;
    else return;
    update_scroll(h, s);
    InvalidateRect(h, NULL, TRUE);
}

static TItem *hit_test(HWND h, TV *s, POINT pt, UINT *flags)
{
    rows(s);
    RECT c;
    GetClientRect(h, &c);
    if (flags) *flags = TVHT_NOWHERE;
    if (!PtInRect(&c, pt)) {
        if (flags) *flags = pt.y < 0 ? TVHT_ABOVE : pt.y >= c.bottom ? TVHT_BELOW : pt.x < 0 ? TVHT_TOLEFT : TVHT_TORIGHT;
        return NULL;
    }
    int i = s->top + pt.y / row_h(s);
    if (i < 0 || i >= s->nvis) return NULL;
    TItem *it = s->vis[i];
    Parts p;
    parts(h, s, it, &p);
    UINT f;
    if (pt.x < p.button.left) f = TVHT_ONITEMINDENT;
    else if (pt.x < p.button.right) f = (style_of(h) & TVS_HASBUTTONS) && has_children(it) ? TVHT_ONITEMBUTTON : TVHT_ONITEMINDENT;
    else if (pt.x < p.state.right) f = TVHT_ONITEMSTATEICON;
    else if (pt.x < p.icon.right) f = TVHT_ONITEMICON;
    else if (pt.x < p.label.right) f = TVHT_ONITEMLABEL;
    else f = TVHT_ONITEMRIGHT;
    if (flags) *flags = f;
    return it;
}

/* -----------------------------------------------------------------------
 * Expanding, selecting
 * ----------------------------------------------------------------------- */
static void free_subtree(HWND h, TV *s, TItem *it, int notify);

static BOOL expand(HWND h, TV *s, TItem *it, UINT code, UINT action)
{
    if (!it || it == &s->root) return FALSE;
    UINT how = code & 0xF;
    if (how == TVE_TOGGLE) how = (it->state & TVIS_EXPANDED) ? TVE_COLLAPSE : TVE_EXPAND;
    if (how == TVE_EXPAND) {
        if (it->state & TVIS_EXPANDED) return TRUE;
        if (!item_children(h, s, it)) return FALSE;
        if (!(it->state & TVIS_EXPANDEDONCE) || (code & TVE_EXPANDPARTIAL) || it->children == I_CHILDRENCALLBACK) {
            POINT z = { 0, 0 };
            if (notify_tv(h, s, TVN_ITEMEXPANDINGW, TVE_EXPAND, NULL, it, z)) return FALSE;
            if (!IsWindow(h) || it->magic != TV_MAGIC) return FALSE;
        }
        it->state |= TVIS_EXPANDED | TVIS_EXPANDEDONCE;
        if (code & TVE_EXPANDPARTIAL) it->state |= TVIS_EXPANDPARTIAL;
        else it->state &= ~TVIS_EXPANDPARTIAL;
        refresh(h, s);
        POINT z = { 0, 0 };
        notify_tv(h, s, TVN_ITEMEXPANDEDW, TVE_EXPAND, NULL, it, z);
        (void)action;
        return TRUE;
    }
    if (how == TVE_COLLAPSE) {
        if (!(it->state & TVIS_EXPANDED)) {
            if (code & TVE_COLLAPSERESET) { free_subtree(h, s, it, 1); it->state &= ~TVIS_EXPANDEDONCE; refresh(h, s); }
            return TRUE;
        }
        POINT z = { 0, 0 };
        if (notify_tv(h, s, TVN_ITEMEXPANDINGW, TVE_COLLAPSE, NULL, it, z)) return FALSE;
        if (!IsWindow(h) || it->magic != TV_MAGIC) return FALSE;
        it->state &= ~(TVIS_EXPANDED | TVIS_EXPANDPARTIAL);
        if (code & TVE_COLLAPSERESET) { free_subtree(h, s, it, 1); it->state &= ~TVIS_EXPANDEDONCE; }
        /* a selection inside goes to the collapsed item */
        for (TItem *a = s->sel ? s->sel->parent : NULL; a; a = a->parent) if (a == it) { s->sel = it; break; }
        refresh(h, s);
        notify_tv(h, s, TVN_ITEMEXPANDEDW, TVE_COLLAPSE, NULL, it, z);
        return TRUE;
    }
    return FALSE;
}

static BOOL select_item(HWND h, TV *s, TItem *it, UINT action, int notify)
{
    if (it == &s->root) it = NULL;
    if (it == s->sel) { if (it) ensure_visible(h, s, it); return TRUE; }
    TItem *old = s->sel;
    POINT z = { 0, 0 };
    if (notify && notify_tv(h, s, TVN_SELCHANGINGW, action, old, it, z)) return FALSE;
    if (!IsWindow(h)) return FALSE;
    if (old && old->magic == TV_MAGIC) { old->state &= ~TVIS_SELECTED; inval_item(h, s, old); }
    s->sel = it;
    if (it) { it->state |= TVIS_SELECTED; ensure_visible(h, s, it); inval_item(h, s, it); }
    if (notify) notify_tv(h, s, TVN_SELCHANGEDW, action, old && old->magic == TV_MAGIC ? old : NULL, it, z);
    if (it && IsWindow(h) && it->magic == TV_MAGIC && (style_of(h) & TVS_SINGLEEXPAND) && action != TVC_UNKNOWN) {
        LRESULT r = notify_tv(h, s, TVN_SINGLEEXPAND, 0, old && old->magic == TV_MAGIC ? old : NULL, it, z);
        if (!(r & TVNRET_SKIPNEW)) expand(h, s, it, TVE_EXPAND, action);
        if (!(r & TVNRET_SKIPOLD) && old && old->magic == TV_MAGIC) {
            int anc = 0;
            for (TItem *a = it; a; a = a->parent) if (a == old) anc = 1;
            if (!anc) expand(h, s, old, TVE_COLLAPSE, action);
        }
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Inserting, deleting, sorting
 * ----------------------------------------------------------------------- */
static void unlink_item(TV *s, TItem *it)
{
    TItem *p = it->parent;
    if (it->prev) it->prev->next = it->next; else p->first = it->next;
    if (it->next) it->next->prev = it->prev; else p->last = it->prev;
    it->next = it->prev = NULL;
}

static void link_after(TV *s, TItem *p, TItem *after, TItem *it)   /* after NULL: first */
{
    (void)s;
    it->parent = p;
    it->prev = after;
    it->next = after ? after->next : p->first;
    if (it->prev) it->prev->next = it; else p->first = it;
    if (it->next) it->next->prev = it; else p->last = it;
}

static int text_cmp(HWND h, TV *s, TItem *a, TItem *b)
{
    WCHAR ba[520], bb[520];
    const WCHAR *ta = item_text(h, s, a, ba, 520);
    const WCHAR *tb = item_text(h, s, b, bb, 520);
    int r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, ta, -1, tb, -1);
    return r ? r - 2 : 0;
}

static HTREEITEM insert_item(HWND h, TV *s, const TVINSERTSTRUCTW *in, int wide)
{
    if (!in) return NULL;
    TItem *p = in->hParent && in->hParent != TVI_ROOT ? item_of(s, in->hParent) : &s->root;
    if (!p) return NULL;
    TItem *it = calloc(1, sizeof(TItem));
    if (!it) return NULL;
    it->magic = TV_MAGIC;
    it->row = -1;
    it->image = it->sel_image = 0;
    const TVITEMW *ti = &in->item;
    if (ti->mask & TVIF_TEXT) set_text(&it->text, ti->pszText, wide);
    if (ti->mask & TVIF_IMAGE) it->image = ti->iImage;
    if (ti->mask & TVIF_SELECTEDIMAGE) it->sel_image = ti->iSelectedImage;
    else if (ti->mask & TVIF_IMAGE) it->sel_image = ti->iImage;
    if (ti->mask & TVIF_PARAM) it->lp = ti->lParam;
    if (ti->mask & TVIF_CHILDREN) it->children = ti->cChildren;
    if (ti->mask & TVIF_STATE) it->state = ti->state & ti->stateMask & ~TVIS_SELECTED;
    if ((ti->mask & TVIF_INTEGRAL) && in->itemex.iIntegral > 0) it->integral = in->itemex.iIntegral;
    if ((style_of(h) & TVS_CHECKBOXES) && !(it->state & TVIS_STATEIMAGEMASK)) it->state |= INDEXTOSTATEIMAGEMASK(1);
    HTREEITEM a = in->hInsertAfter;
    if (a == TVI_FIRST) link_after(s, p, NULL, it);
    else if (a == TVI_LAST || !a) link_after(s, p, p->last, it);
    else if (a == TVI_SORT) {
        TItem *after = NULL;
        for (TItem *c = p->first; c; c = c->next) { if (text_cmp(h, s, it, c) < 0) break; after = c; }
        link_after(s, p, after, it);
    } else {
        TItem *ai = item_of(s, a);
        if (!ai || ai->parent != p) ai = p->last;
        link_after(s, p, ai, it);
    }
    s->count++;
    refresh(h, s);
    return (HTREEITEM)it;
}

static void free_item(HWND h, TV *s, TItem *it, int notify)
{
    free_subtree(h, s, it, notify);
    if (notify) { POINT z = { 0, 0 }; notify_tv(h, s, TVN_DELETEITEMW, 0, it, NULL, z); }
    if (s->sel == it) s->sel = NULL;
    if (s->drop == it) s->drop = NULL;
    if (s->hot == it) s->hot = NULL;
    if (s->ins_mark == it) s->ins_mark = NULL;
    if (s->down_item == it) s->down_item = NULL;
    if (s->edit_item == it) { s->edit_item = NULL; if (s->edit) { DestroyWindow(s->edit); s->edit = NULL; } }
    wfree(it->text);
    it->magic = 0;
    free(it);
    s->count--;
}

static void free_subtree(HWND h, TV *s, TItem *it, int notify)
{
    for (TItem *c = it->first, *n; c; c = n) { n = c->next; free_item(h, s, c, notify); }
    it->first = it->last = NULL;
}

static BOOL delete_item(HWND h, TV *s, HTREEITEM hi)
{
    if (!hi || hi == TVI_ROOT) {
        TItem *old = s->sel;
        free_subtree(h, s, &s->root, 1);
        s->top = 0; s->sx = 0;
        if (old) { POINT z = { 0, 0 }; s->sel = NULL; notify_tv(h, s, TVN_SELCHANGEDW, TVC_UNKNOWN, NULL, NULL, z); }
        refresh(h, s);
        return TRUE;
    }
    TItem *it = item_of(s, hi);
    if (!it || it == &s->root) return FALSE;
    int had_sel = 0;
    for (TItem *a = s->sel; a; a = a->parent) if (a == it) had_sel = 1;
    TItem *next = had_sel ? (it->next ? it->next : it->prev ? it->prev : it->parent != &s->root ? it->parent : NULL) : NULL;
    unlink_item(s, it);
    free_item(h, s, it, 1);
    if (had_sel) {
        s->sel = NULL;
        if (next && next->magic == TV_MAGIC) select_item(h, s, next, TVC_UNKNOWN, 1);
        else { POINT z = { 0, 0 }; notify_tv(h, s, TVN_SELCHANGEDW, TVC_UNKNOWN, NULL, NULL, z); }
    }
    refresh(h, s);
    return TRUE;
}

typedef struct { HWND h; TV *s; PFNTVCOMPARE fn; LPARAM data; } SortCtx;

static int sort_cmp(SortCtx *c, TItem *a, TItem *b)
{
    return c->fn ? c->fn(a->lp, b->lp, c->data) : text_cmp(c->h, c->s, a, b);
}

static void sort_children(SortCtx *c, TItem *p, int recurse)
{
    int n = 0;
    for (TItem *i = p->first; i; i = i->next) n++;
    if (n > 1) {
        TItem **v = malloc(sizeof(TItem *) * (size_t)n);
        if (!v) return;
        int k = 0;
        for (TItem *i = p->first; i; i = i->next) v[k++] = i;
        for (int i = 1; i < n; i++) {                       /* insertion sort: stable, children are few */
            TItem *x = v[i];
            int j = i - 1;
            while (j >= 0 && sort_cmp(c, v[j], x) > 0) { v[j + 1] = v[j]; j--; }
            v[j + 1] = x;
        }
        p->first = p->last = NULL;
        for (int i = 0; i < n; i++) { v[i]->next = v[i]->prev = NULL; link_after(c->s, p, p->last, v[i]); }
        free(v);
    }
    if (recurse) for (TItem *i = p->first; i; i = i->next) sort_children(c, i, 1);
}

/* -----------------------------------------------------------------------
 * Getting and setting
 * ----------------------------------------------------------------------- */
static void copy_out(WCHAR *dst, int cap, const WCHAR *src, int wide)
{
    if (!dst || cap <= 0) return;
    if (!src) src = L"";
    if (wide) { int n = MIN(wlen(src), cap - 1); memcpy(dst, src, 2 * (size_t)n); dst[n] = 0; }
    else if (!WideCharToMultiByte(CP_ACP, 0, src, -1, (char *)dst, cap, NULL, NULL)) ((char *)dst)[cap - 1] = 0;
}

static BOOL get_item(HWND h, TV *s, TVITEMW *out, int wide)
{
    TItem *it = out ? item_of(s, out->hItem) : NULL;
    if (!it || it == &s->root) return FALSE;
    WCHAR b[520];
    if (out->mask & TVIF_TEXT) copy_out(out->pszText, out->cchTextMax, item_text(h, s, it, b, 520), wide);
    if (out->mask & TVIF_IMAGE) out->iImage = it->image;
    if (out->mask & TVIF_SELECTEDIMAGE) out->iSelectedImage = it->sel_image;
    if (out->mask & TVIF_PARAM) out->lParam = it->lp;
    if (out->mask & TVIF_STATE) out->state = it->state & out->stateMask;
    if (out->mask & TVIF_CHILDREN) out->cChildren = it->children == I_CHILDRENCALLBACK ? item_children(h, s, it) : (it->first ? 1 : it->children);
    if (out->mask & TVIF_INTEGRAL) ((TVITEMEXW *)out)->iIntegral = it->integral ? it->integral : 1;
    return TRUE;
}

static BOOL set_item(HWND h, TV *s, const TVITEMW *in, int wide)
{
    TItem *it = in ? item_of(s, in->hItem) : NULL;
    if (!it || it == &s->root) return FALSE;
    if (in->mask & TVIF_STATE) {
        UINT nw = (it->state & ~in->stateMask) | (in->state & in->stateMask);
        if (nw != it->state) {
            NMTVITEMCHANGE ch;
            memset(&ch, 0, sizeof(ch));
            ch.uChanged = TVIF_STATE; ch.hItem = (HTREEITEM)it; ch.uStateNew = nw; ch.uStateOld = it->state; ch.lParam = it->lp;
            if (cc_notify(h, TVN_ITEMCHANGINGW, &ch.hdr)) return FALSE;
            UINT old = it->state;
            it->state = nw;
            if ((old ^ nw) & TVIS_EXPANDED) invalidate_rows(s);
            if ((old ^ nw) & TVIS_SELECTED) {
                if (nw & TVIS_SELECTED) { if (s->sel && s->sel != it) { s->sel->state &= ~TVIS_SELECTED; inval_item(h, s, s->sel); } s->sel = it; }
                else if (s->sel == it) s->sel = NULL;
            }
            cc_notify(h, TVN_ITEMCHANGEDW, &ch.hdr);
        }
    }
    if (in->mask & TVIF_TEXT) set_text(&it->text, in->pszText, wide);
    if (in->mask & TVIF_IMAGE) it->image = in->iImage;
    if (in->mask & TVIF_SELECTEDIMAGE) it->sel_image = in->iSelectedImage;
    if (in->mask & TVIF_PARAM) it->lp = in->lParam;
    if (in->mask & TVIF_CHILDREN) it->children = in->cChildren;
    if ((in->mask & TVIF_INTEGRAL) && ((const TVITEMEXW *)in)->iIntegral > 0) it->integral = ((const TVITEMEXW *)in)->iIntegral;
    if (in->mask & (TVIF_STATE | TVIF_CHILDREN | TVIF_TEXT)) refresh(h, s);
    else inval_item(h, s, it);
    return TRUE;
}

static TItem *next_item(HWND h, TV *s, UINT code, HTREEITEM hi)
{
    rows(s);
    TItem *it = hi ? item_of(s, hi) : NULL;
    switch (code) {
    case TVGN_ROOT: return s->root.first;
    case TVGN_NEXT: return it ? it->next : NULL;
    case TVGN_PREVIOUS: return it ? it->prev : NULL;
    case TVGN_PARENT: return it && it->parent != &s->root ? it->parent : NULL;
    case TVGN_CHILD: return it ? it->first : (it == NULL && hi == TVI_ROOT ? s->root.first : NULL);
    case TVGN_FIRSTVISIBLE: return s->nvis && s->top < s->nvis ? s->vis[s->top] : NULL;
    case TVGN_NEXTVISIBLE: return it && it->row >= 0 && it->row + 1 < s->nvis ? s->vis[it->row + 1] : NULL;
    case TVGN_PREVIOUSVISIBLE: return it && it->row > 0 ? s->vis[it->row - 1] : NULL;
    case TVGN_LASTVISIBLE: { int vis = rows_visible(h, s); int i = MIN(s->nvis - 1, s->top + vis - 1); return i >= 0 ? s->vis[i] : NULL; }
    case TVGN_DROPHILITE: return s->drop;
    case TVGN_CARET: return s->sel;
    case TVGN_NEXTSELECTED: return NULL;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */
static COLORREF c_bk(TV *s) { return s->bk == CLR_DEFAULT || s->bk == CLR_NONE ? GetSysColor(COLOR_WINDOW) : s->bk; }
static COLORREF c_text(TV *s) { return s->text == CLR_DEFAULT ? GetSysColor(COLOR_WINDOWTEXT) : s->text; }
static COLORREF c_line(TV *s) { return s->line == CLR_DEFAULT ? RGB(160, 160, 160) : s->line; }

static void dotted_h(HDC dc, int x0, int x1, int y, COLORREF c)
{
    for (int x = x0 + ((x0 + y) & 1); x < x1; x += 2) { RECT r = { x, y, x + 1, y + 1 }; cc_fill(dc, &r, c); }
}

static void dotted_v(HDC dc, int x, int y0, int y1, COLORREF c)
{
    for (int y = y0 + ((x + y0) & 1); y < y1; y += 2) { RECT r = { x, y, x + 1, y + 1 }; cc_fill(dc, &r, c); }
}

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

static void draw_button(HDC dc, const RECT *r, BOOL expanded, BOOL hot, int s)
{
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    /* a small triangle: right for collapsed, down-right for expanded */
    COLORREF c = hot ? RGB(28, 151, 234) : expanded ? RGB(38, 38, 38) : RGB(150, 150, 150);
    if (!expanded) {
        for (int i = 0; i < 4; i++) { RECT l = { cx + (i - 1) * s, cy + (i - 3) * s, cx + i * s, cy + (4 - i) * s }; cc_fill(dc, &l, c); }
    } else {
        for (int i = 0; i < 4; i++) { RECT l = { cx + (i - 3) * s, cy + (i - 1) * s, cx + 2 * s, cy + i * s }; cc_fill(dc, &l, c); }
    }
}

static void paint_item(HWND h, TV *s, HDC dc, TItem *it, BOOL want_item, BOOL focus)
{
    Parts p;
    if (!parts(h, s, it, &p)) return;
    DWORD st = style_of(h);
    RECT c;
    GetClientRect(h, &c);
    BOOL sel = it == s->sel || (it->state & TVIS_SELECTED);
    BOOL show_sel = sel && (focus || (st & TVS_SHOWSELALWAYS) || s->edit);
    BOOL drop = it == s->drop || (it->state & TVIS_DROPHILITED);
    COLORREF text = c_text(s), textbk = CLR_NONE;
    NMTVCUSTOMDRAW cd;
    if (want_item) {
        memset(&cd, 0, sizeof(cd));
        cd.nmcd.dwDrawStage = CDDS_ITEMPREPAINT;
        cd.nmcd.hdc = dc;
        cd.nmcd.rc = p.row;
        cd.nmcd.dwItemSpec = (DWORD_PTR)it;
        cd.nmcd.uItemState = (sel ? CDIS_SELECTED_ : 0) | (it == s->sel && focus ? CDIS_FOCUS_ : 0);
        cd.nmcd.lItemlParam = it->lp;
        cd.clrText = text;
        cd.clrTextBk = c_bk(s);
        cd.iLevel = it->level;
        LRESULT r = cc_notify(h, NM_CUSTOMDRAW, &cd.nmcd.hdr);
        if (r & CDRF_SKIPDEFAULT) return;
        if (cd.clrText != CLR_DEFAULT) text = cd.clrText;
        if (cd.clrTextBk != CLR_DEFAULT && cd.clrTextBk != c_bk(s)) textbk = cd.clrTextBk;
    }
    /* lines */
    if (st & TVS_HASLINES) {
        COLORREF lc = c_line(s);
        int cx = (p.button.left + p.button.right) / 2, cy = (p.row.top + p.row.bottom) / 2;
        int root_lines = (st & TVS_LINESATROOT) != 0;
        if (it->level > 0 || root_lines) {
            dotted_h(dc, cx, p.button.right, cy, lc);
            dotted_v(dc, cx, it->prev || it->level > 0 || !root_lines ? p.row.top : cy, it->next ? p.row.bottom : cy, lc);
        }
        int x = cx;
        for (TItem *a = it->parent; a && a != &s->root; a = a->parent) {
            x -= indent(h, s);
            if (a->next) dotted_v(dc, x, p.row.top, p.row.bottom, lc);
        }
    }
    /* button */
    if ((st & TVS_HASBUTTONS) && has_children(it) && (it->level > 0 || (st & TVS_LINESATROOT) || !(st & TVS_HASLINES))) {
        if (it->level > 0 || (st & TVS_LINESATROOT) || !(st & TVS_HASLINES)) {
            RECT b = p.button;
            if (st & TVS_HASLINES) { int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2; int k = cc_k(h); RECT bg = { cx - 5 * k, cy - 5 * k, cx + 6 * k, cy + 6 * k }; cc_fill(dc, &bg, c_bk(s)); }
            draw_button(dc, &b, (it->state & TVIS_EXPANDED) != 0, it == s->hot, cc_k(h));
        }
    }
    /* selection background */
    RECT selr = (st & TVS_FULLROWSELECT) ? p.row : p.label;
    if (st & TVS_FULLROWSELECT) selr.left = 0;
    COLORREF selbg = focus ? RGB(204, 232, 255) : RGB(217, 217, 217);
    if (textbk != CLR_NONE && !show_sel && !drop) cc_fill(dc, &selr, textbk);
    if (show_sel || drop) cc_fill(dc, &selr, drop ? RGB(204, 232, 255) : selbg);
    /* state image or check box */
    UINT si = (it->state & TVIS_STATEIMAGEMASK) >> 12;
    if (s->sil) {
        if (si) { int sw, sh; ImageList_GetIconSize(s->sil, &sw, &sh); il_draw(s->sil, (int)si - 1, dc, p.state.left, (p.row.top + p.row.bottom - sh) / 2, ILD_TRANSPARENT); }
    } else if (st & TVS_CHECKBOXES) {
        draw_check(dc, p.state.left + cc_k(h), (p.row.top + p.row.bottom - 13 * cc_k(h)) / 2, si == 2, cc_k(h));
    }
    /* image */
    if (s->il) {
        int img = item_image(h, s, it, sel);
        if (img >= 0) {
            int iw, ih;
            ImageList_GetIconSize(s->il, &iw, &ih);
            il_draw(s->il, img, dc, p.icon.left, (p.row.top + p.row.bottom - ih) / 2, ILD_TRANSPARENT | (it->state & TVIS_OVERLAYMASK));
        }
    }
    /* label */
    if (!(s->edit && s->edit_item == it)) {
        WCHAR b[520];
        const WCHAR *t = item_text(h, s, it, b, 520);
        HFONT f = s->font ? s->font : cc_font_for(h);
        HFONT bold = NULL;
        if (it->state & TVIS_BOLD) {
            LOGFONTW lf;
            if (GetObjectW(f, sizeof(lf), &lf)) { lf.lfWeight = FW_BOLD; bold = CreateFontIndirectW(&lf); }
        }
        HGDIOBJ of = SelectObject(dc, bold ? bold : f);
        SetTextColor(dc, (it->state & TVIS_CUT) ? GetSysColor(COLOR_GRAYTEXT) : text);
        RECT tr = { p.label.left + 3 * cc_k(h), p.label.top, p.label.right - 3 * cc_k(h), p.label.bottom };
        DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_NOCLIP);
        SelectObject(dc, of);
        if (bold) DeleteObject(bold);
    }
    if (it == s->sel && focus && !s->edit) {
        if (show_sel) cc_frame(dc, &selr, RGB(153, 209, 255));
        else DrawFocusRect(dc, &selr);
    }
    /* the insert mark */
    if (s->ins_mark == it) {
        int y = s->ins_after ? p.row.bottom - 1 : p.row.top;
        RECT m = { p.label.left, y - 1, p.label.right, y + 1 };
        cc_fill(dc, &m, s->insmark == CLR_DEFAULT ? RGB(0, 0, 0) : s->insmark);
    }
}

static void paint(HWND h, TV *s, HDC dc, const RECT *upd)
{
    rows(s);
    RECT c;
    GetClientRect(h, &c);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    SetBkMode(dc, TRANSPARENT);
    NMTVCUSTOMDRAW cd;
    memset(&cd, 0, sizeof(cd));
    cd.nmcd.dwDrawStage = CDDS_PREPAINT;
    cd.nmcd.hdc = dc;
    cd.nmcd.rc = c;
    LRESULT pre = cc_notify(h, NM_CUSTOMDRAW, &cd.nmcd.hdr);
    RECT fill;
    if (IntersectRect(&fill, &c, upd)) cc_fill(dc, &fill, c_bk(s));
    if (!(pre & CDRF_SKIPDEFAULT)) {
        BOOL focus = GetFocus() == h;
        BOOL want_item = (pre & CDRF_NOTIFYITEMDRAW) != 0;
        int rh = row_h(s);
        int first = s->top + MAX(0, upd->top) / rh, last = s->top + upd->bottom / rh;
        if (last > s->nvis - 1) last = s->nvis - 1;
        for (int i = first; i <= last; i++) {
            SelectObject(dc, s->font ? s->font : cc_font_for(h));
            paint_item(h, s, dc, s->vis[i], want_item, focus);
        }
        if (pre & CDRF_NOTIFYPOSTPAINT) { cd.nmcd.dwDrawStage = CDDS_POSTPAINT; cc_notify(h, NM_CUSTOMDRAW, &cd.nmcd.hdr); }
    }
    SelectObject(dc, of);
}

/* -----------------------------------------------------------------------
 * Label editing
 * ----------------------------------------------------------------------- */
static LRESULT CALLBACK edit_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

static void end_edit(HWND h, TV *s, BOOL cancel)
{
    if (!s->edit) return;
    HWND e = s->edit;
    TItem *it = s->edit_item;
    s->edit = NULL;
    s->edit_item = NULL;
    WCHAR b[520];
    b[0] = 0;
    if (!cancel) GetWindowTextW(e, b, 520);
    RemoveWindowSubclass(e, edit_sub, 1);
    ShowWindow(e, SW_HIDE);
    if (it && it->magic == TV_MAGIC) {
        NMTVDISPINFOW di;
        memset(&di, 0, sizeof(di));
        di.item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_PARAM;
        di.item.hItem = (HTREEITEM)it;
        di.item.lParam = it->lp;
        di.item.pszText = cancel ? NULL : b;
        di.item.cchTextMax = 520;
        char ab[520];
        if (s->ansi && !cancel) { WideCharToMultiByte(CP_ACP, 0, b, -1, ab, 520, NULL, NULL); di.item.pszText = (LPWSTR)ab; }
        LRESULT ok = cc_notify(h, s->ansi ? TVN_ENDLABELEDITA : TVN_ENDLABELEDITW, &di.hdr);
        if (ok && !cancel && it->magic == TV_MAGIC && it->text != LPSTR_TEXTCALLBACKW) set_text(&it->text, b, 1);
    }
    DestroyWindow(e);
    if (IsWindow(h)) { refresh(h, s); if (GetFocus() == NULL) SetFocus(h); }
}

static HWND edit_label(HWND h, TV *s, TItem *it)
{
    if (!it || it == &s->root) return NULL;
    end_edit(h, s, TRUE);
    SetFocus(h);
    ensure_visible(h, s, it);
    WCHAR b[520];
    const WCHAR *t = item_text(h, s, it, b, 520);
    NMTVDISPINFOW di;
    memset(&di, 0, sizeof(di));
    di.item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    di.item.hItem = (HTREEITEM)it;
    di.item.pszText = (LPWSTR)t;
    di.item.lParam = it->lp;
    di.item.state = it->state;
    if (cc_notify(h, s->ansi ? TVN_BEGINLABELEDITA : TVN_BEGINLABELEDITW, &di.hdr)) return NULL;
    Parts p;
    if (!parts(h, s, it, &p)) return NULL;
    RECT c;
    GetClientRect(h, &c);
    int w = MAX(p.label.right - p.label.left + 20, 60);
    if (p.label.left + w > c.right) w = MAX(c.right - p.label.left, 40);
    s->edit = CreateWindowExW(0, L"Edit", t, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, p.label.left, p.label.top, w, p.label.bottom - p.label.top, h, (HMENU)1, NULL, NULL);
    if (!s->edit) return NULL;
    s->edit_item = it;
    SendMessageW(s->edit, WM_SETFONT, (WPARAM)(s->font ? s->font : cc_font_for(h)), 0);
    SetWindowSubclass(s->edit, edit_sub, 1, (DWORD_PTR)h);
    SendMessageW(s->edit, EM_SETSEL, 0, -1);
    ShowWindow(s->edit, SW_SHOW);
    SetFocus(s->edit);
    inval_item(h, s, it);
    return s->edit;
}

static LRESULT CALLBACK edit_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    HWND tv = (HWND)ref;
    TV *s = ctl_get(tv);
    (void)id;
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DefSubclassProc(h, msg, wp, lp);
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE) { if (s) { end_edit(tv, s, wp == VK_ESCAPE); SetFocus(tv); } return 0; }
        break;
    case WM_CHAR: if (wp == '\r' || wp == 27) return 0; break;
    case WM_KILLFOCUS: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (s && s->edit == h) end_edit(tv, s, FALSE);
        return r;
    }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* -----------------------------------------------------------------------
 * Mouse and keyboard
 * ----------------------------------------------------------------------- */
static void toggle_check(HWND h, TV *s, TItem *it)
{
    UINT si = (it->state & TVIS_STATEIMAGEMASK) >> 12;
    TVITEMW ti;
    memset(&ti, 0, sizeof(ti));
    ti.mask = TVIF_HANDLE | TVIF_STATE;
    ti.hItem = (HTREEITEM)it;
    ti.stateMask = TVIS_STATEIMAGEMASK;
    ti.state = INDEXTOSTATEIMAGEMASK(si == 2 ? 1 : 2);
    set_item(h, s, &ti, 1);
}

static void button_down(HWND h, TV *s, POINT pt, BOOL right, BOOL dbl)
{
    KillTimer(h, EDIT_TIMER);
    s->edit_pending = 0;
    end_edit(h, s, FALSE);
    BOOL had_focus = GetFocus() == h;
    if (!had_focus) SetFocus(h);
    UINT f;
    TItem *it = hit_test(h, s, pt, &f);
    if (!right && it && (f & TVHT_ONITEMBUTTON)) {
        expand(h, s, it, TVE_TOGGLE, TVC_BYMOUSE);
        return;
    }
    if (!right && it && (f & TVHT_ONITEMSTATEICON) && (style_of(h) & TVS_CHECKBOXES) && !dbl) {
        toggle_check(h, s, it);
        return;
    }
    if (dbl) {
        if (cc_notify(h, right ? NM_RDBLCLK : NM_DBLCLK, NULL) || !IsWindow(h)) return;
        if (!right && it && (f & (TVHT_ONITEM))) expand(h, s, it, TVE_TOGGLE, TVC_BYMOUSE);
        return;
    }
    if (it && (f & (TVHT_ONITEM | ((style_of(h) & TVS_FULLROWSELECT) ? TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT : 0)))) {
        BOOL was_sel = it == s->sel && had_focus;
        if (!right || it != s->sel) select_item(h, s, it, TVC_BYMOUSE, 1);
        if (!IsWindow(h) || it->magic != TV_MAGIC) return;
        s->down = right ? 2 : 1;
        s->down_item = it;
        s->down_pt = pt;
        s->dragging = 0;
        s->edit_pending = !right && was_sel && (style_of(h) & TVS_EDITLABELS) && (f & TVHT_ONITEMLABEL);
        SetCapture(h);
    } else {
        s->down = right ? 2 : 1;
        s->down_item = NULL;
        s->down_pt = pt;
        SetCapture(h);
    }
}

static void button_up(HWND h, TV *s, POINT pt, BOOL right)
{
    if (!s->down) return;
    s->down = 0;
    ReleaseCapture();
    if (s->dragging) { s->dragging = 0; return; }
    TItem *it = s->down_item;
    if (right) {
        if (!cc_notify(h, NM_RCLICK, NULL) && IsWindow(h)) {
            POINT sp = pt;
            ClientToScreen(h, &sp);
            SendMessageW(h, WM_CONTEXTMENU, (WPARAM)h, MAKELPARAM(sp.x, sp.y));
        }
        return;
    }
    cc_notify(h, NM_CLICK, NULL);
    if (IsWindow(h) && s->edit_pending && it && it->magic == TV_MAGIC && it == s->sel) SetTimer(h, EDIT_TIMER, GetDoubleClickTime(), NULL);
    else s->edit_pending = 0;
}

static void mouse_move(HWND h, TV *s, POINT pt, WPARAM keys)
{
    if (s->down && !s->dragging && s->down_item && (keys & (MK_LBUTTON | MK_RBUTTON))) {
        int dx = GetSystemMetrics(SM_CXDRAG), dy = GetSystemMetrics(SM_CYDRAG);
        if (ABS(pt.x - s->down_pt.x) >= MAX(dx, 4) || ABS(pt.y - s->down_pt.y) >= MAX(dy, 4)) {
            s->dragging = 1;
            int btn = s->down;
            TItem *it = s->down_item;
            s->down = 0;
            s->edit_pending = 0;
            ReleaseCapture();
            if (!(style_of(h) & TVS_DISABLEDRAGDROP)) notify_tv(h, s, btn == 2 ? TVN_BEGINRDRAGW : TVN_BEGINDRAGW, 0, NULL, it, pt);
            return;
        }
    }
    if (style_of(h) & (TVS_HASBUTTONS | TVS_TRACKSELECT)) {
        UINT f;
        TItem *it = hit_test(h, s, pt, &f);
        TItem *hot = it && (f & TVHT_ONITEMBUTTON) ? it : NULL;
        if (hot != s->hot) {
            TItem *o = s->hot;
            s->hot = hot;
            if (o) inval_item(h, s, o);
            if (hot) inval_item(h, s, hot);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
    }
}

static void key_down(HWND h, TV *s, WPARAM vk)
{
    NMTVKEYDOWN kd;
    memset(&kd, 0, sizeof(kd));
    kd.wVKey = (WORD)vk;
    if (cc_notify(h, TVN_KEYDOWN, &kd.hdr) || !IsWindow(h)) return;
    rows(s);
    TItem *cur = s->sel, *nw = NULL;
    int vis = rows_visible(h, s);
    switch (vk) {
    case VK_UP: nw = cur && cur->row > 0 ? s->vis[cur->row - 1] : (cur ? cur : (s->nvis ? s->vis[0] : NULL)); break;
    case VK_DOWN: nw = cur ? (cur->row + 1 < s->nvis ? s->vis[cur->row + 1] : cur) : (s->nvis ? s->vis[0] : NULL); break;
    case VK_HOME: nw = s->nvis ? s->vis[0] : NULL; break;
    case VK_END: nw = s->nvis ? s->vis[s->nvis - 1] : NULL; break;
    case VK_PRIOR: nw = cur ? s->vis[MAX(0, cur->row - (vis - 1))] : (s->nvis ? s->vis[0] : NULL); break;
    case VK_NEXT: nw = cur ? s->vis[MIN(s->nvis - 1, cur->row + (vis - 1))] : (s->nvis ? s->vis[0] : NULL); break;
    case VK_LEFT:
        if (!cur) break;
        if (cur->state & TVIS_EXPANDED) expand(h, s, cur, TVE_COLLAPSE, TVC_BYKEYBOARD);
        else if (cur->parent != &s->root) nw = cur->parent;
        break;
    case VK_RIGHT:
        if (!cur) break;
        if (!(cur->state & TVIS_EXPANDED)) expand(h, s, cur, TVE_EXPAND, TVC_BYKEYBOARD);
        else if (cur->first) nw = cur->first;
        break;
    case VK_ADD: if (cur) expand(h, s, cur, TVE_EXPAND, TVC_BYKEYBOARD); break;
    case VK_SUBTRACT: if (cur) expand(h, s, cur, TVE_COLLAPSE, TVC_BYKEYBOARD); break;
    case VK_MULTIPLY:
        if (cur) {
            expand(h, s, cur, TVE_EXPAND, TVC_BYKEYBOARD);
            for (TItem *c = cur->first; c; c = c->next) { expand(h, s, c, TVE_EXPAND, TVC_BYKEYBOARD); }
        }
        break;
    case VK_SPACE: if (cur && (style_of(h) & TVS_CHECKBOXES)) toggle_check(h, s, cur); break;
    case VK_RETURN: cc_notify(h, NM_RETURN, NULL); break;
    case VK_BACK: if (cur && cur->parent != &s->root) nw = cur->parent; break;
    case VK_F2: if (cur && (style_of(h) & TVS_EDITLABELS)) edit_label(h, s, cur); break;
    }
    if (nw && nw != cur) select_item(h, s, nw, TVC_BYKEYBOARD, 1);
}

static BOOL prefix_of(const WCHAR *p, int n, const WCHAR *t)
{
    if (wlen(t) < n) return FALSE;
    return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, p, n, t, n) == 2;
}

static void type_search(HWND h, TV *s, WCHAR ch)
{
    DWORD now = GetTickCount();
    if (now - s->search_tick > 1000) s->nsearch = 0;
    s->search_tick = now;
    if (s->nsearch < 63) s->search[s->nsearch++] = ch;
    rows(s);
    if (!s->nvis) return;
    BOOL same = TRUE;
    for (int k = 1; k < s->nsearch; k++) if (s->search[k] != s->search[0]) same = FALSE;
    int len = same ? 1 : s->nsearch;
    int start = !s->sel || s->sel->row < 0 ? 0 : (same ? s->sel->row + 1 : s->sel->row);
    WCHAR b[520];
    for (int k = 0; k < s->nvis; k++) {
        TItem *it = s->vis[(start + k) % s->nvis];
        if (prefix_of(s->search, len, item_text(h, s, it, b, 520))) { select_item(h, s, it, TVC_BYKEYBOARD, 1); return; }
    }
}

static void on_scroll(HWND h, TV *s, int bar, int code)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL, 0, 0, 0, 0, 0 };
    GetScrollInfo(h, bar, &si);
    int line = bar == SB_VERT ? 1 : 16, pos = si.nPos;
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
    if (pos == si.nPos) return;
    end_edit(h, s, FALSE);
    if (bar == SB_VERT) s->top = pos; else s->sx = pos;
    update_scroll(h, s);
    InvalidateRect(h, NULL, TRUE);
}

/* -----------------------------------------------------------------------
 * The window procedure
 * ----------------------------------------------------------------------- */
LRESULT CALLBACK TreeViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    TV *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    int wide = 1;
    switch (msg) {
    case WM_NCCREATE:
        s = calloc(1, sizeof(TV));
        if (!s) return FALSE;
        s->root.magic = TV_MAGIC;
        s->root.row = -1;
        s->root.state = TVIS_EXPANDED;
        s->bk = s->text = s->line = s->insmark = CLR_DEFAULT;
        s->fh = cc_font_h(cc_font_for(h));
        s->item_h = 0;
        ctl_set(h, s);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE: {
        HWND p = GetParent(h);
        s->ansi = p && SendMessageW(p, WM_NOTIFYFORMAT, (WPARAM)h, NF_QUERY_) == NFR_ANSI;
        s->item_h = default_item_h(h, s);
        refresh(h, s);
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(h, EDIT_TIMER);
        free_subtree(h, s, &s->root, 1);
        free(s->vis);
        free(s);
        ctl_set(h, NULL);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NOTIFYFORMAT: return lp == NF_QUERY_ ? NFR_UNICODE : (s->ansi ? NFR_ANSI : NFR_UNICODE);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(h, s, dc, &ps.rcPaint); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: { RECT c; GetClientRect(h, &c); paint(h, s, (HDC)wp, &c); return 0; }
    case WM_SIZE: update_scroll(h, s); InvalidateRect(h, NULL, TRUE); return 0;
    case WM_DPICHANGED_AFTERPARENT:                          /* the window's DPI changed: the defaults' sizes */
        if (!s->font) s->fh = cc_font_h(cc_font_for(h));
        if (!s->item_set) s->item_h = default_item_h(h, s);
        refresh(h, s);
        return 0;
    case WM_SETFONT:
        s->font = (HFONT)wp;
        s->fh = cc_font_h(s->font ? s->font : cc_font_for(h));
        s->item_h = default_item_h(h, s);
        refresh(h, s);
        return 0;
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_SETFOCUS: InvalidateRect(h, NULL, TRUE); cc_notify(h, NM_SETFOCUS, NULL); return 0;
    case WM_KILLFOCUS: InvalidateRect(h, NULL, TRUE); cc_notify(h, NM_KILLFOCUS, NULL); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_VSCROLL: on_scroll(h, s, SB_VERT, LOWORD(wp)); return 0;
    case WM_HSCROLL: on_scroll(h, s, SB_HORZ, LOWORD(wp)); return 0;
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(wp) / 40;
        if (!d) d = (short)HIWORD(wp) > 0 ? 1 : -1;
        int vis = rows_visible(h, s);
        int pos = s->top - d * 3;
        if (pos > MAX(0, s->nvis - vis)) pos = MAX(0, s->nvis - vis);
        if (pos < 0) pos = 0;
        if (pos != s->top) { s->top = pos; update_scroll(h, s); InvalidateRect(h, NULL, TRUE); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        button_down(h, s, pt, msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK, msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK);
        return 0;
    }
    case WM_LBUTTONUP: { POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; button_up(h, s, pt, FALSE); return 0; }
    case WM_RBUTTONUP: { POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; button_up(h, s, pt, TRUE); return 0; }
    case WM_MOUSEMOVE: { POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; mouse_move(h, s, pt, wp); return 0; }
    case WM_MOUSELEAVE: if (s->hot) { TItem *o = s->hot; s->hot = NULL; inval_item(h, s, o); } return 0;
    case WM_CAPTURECHANGED: if ((HWND)lp != h) s->down = 0; return 0;
    case WM_TIMER:
        if (wp == EDIT_TIMER) {
            KillTimer(h, EDIT_TIMER);
            int pending = s->edit_pending;
            s->edit_pending = 0;
            if (pending && s->sel && GetFocus() == h) edit_label(h, s, s->sel);
            return 0;
        }
        break;
    case WM_KEYDOWN: key_down(h, s, wp); return 0;
    case WM_CHAR:
        if (wp >= 32 && wp != 127 && GetKeyState(VK_CONTROL) >= 0) type_search(h, s, (WCHAR)wp);
        return 0;
    case WM_STYLECHANGED: refresh(h, s); return 0;
    case WM_ENABLE: InvalidateRect(h, NULL, TRUE); return 0;

    case TVM_INSERTITEMA: wide = 0; /* fall through */
    case TVM_INSERTITEMW: return (LRESULT)insert_item(h, s, (const TVINSERTSTRUCTW *)lp, wide);
    case TVM_DELETEITEM: return delete_item(h, s, (HTREEITEM)lp);
    case TVM_EXPAND: return expand(h, s, item_of(s, (HTREEITEM)lp), (UINT)wp, TVC_UNKNOWN);
    case TVM_GETITEMRECT: {
        RECT *r = (RECT *)lp;
        if (!r) return FALSE;
        HTREEITEM hi = *(HTREEITEM *)r;
        TItem *it = item_of(s, hi);
        Parts p;
        if (!it || it == &s->root || !parts(h, s, it, &p)) return FALSE;
        if (wp) *r = p.label; else *r = p.row;
        return TRUE;
    }
    case TVM_GETCOUNT: return s->count;
    case TVM_GETINDENT: return indent(h, s);
    case TVM_SETINDENT: s->indent = MAX((int)wp, 5); refresh(h, s); return 0;
    case TVM_GETIMAGELIST: return (LRESULT)(wp == TVSIL_STATE ? s->sil : s->il);
    case TVM_SETIMAGELIST: {
        HIMAGELIST o = wp == TVSIL_STATE ? s->sil : s->il;
        if (wp == TVSIL_STATE) s->sil = (HIMAGELIST)lp; else s->il = (HIMAGELIST)lp;
        s->item_h = default_item_h(h, s);
        refresh(h, s);
        return (LRESULT)o;
    }
    case TVM_GETNEXTITEM: return (LRESULT)handle_of(s, next_item(h, s, (UINT)wp, (HTREEITEM)lp));
    case TVM_SELECTITEM: {
        TItem *it = lp ? item_of(s, (HTREEITEM)lp) : NULL;
        if (lp && !it) return FALSE;
        UINT code = (UINT)wp & ~TVSI_NOSINGLEEXPAND;
        if (code == TVGN_CARET) return select_item(h, s, it, (wp & TVSI_NOSINGLEEXPAND) ? TVC_UNKNOWN : TVC_UNKNOWN, 1);
        if (code == TVGN_DROPHILITE) {
            TItem *o = s->drop;
            s->drop = it;
            if (o) inval_item(h, s, o);
            if (it) inval_item(h, s, it);
            return TRUE;
        }
        if (code == TVGN_FIRSTVISIBLE) {
            if (!it) return FALSE;
            ensure_visible(h, s, it);
            rows(s);
            if (it->row >= 0) { s->top = it->row; update_scroll(h, s); InvalidateRect(h, NULL, TRUE); }
            return TRUE;
        }
        return FALSE;
    }
    case TVM_GETITEMA: wide = 0; /* fall through */
    case TVM_GETITEMW: return get_item(h, s, (TVITEMW *)lp, wide);
    case TVM_SETITEMA: wide = 0; /* fall through */
    case TVM_SETITEMW: return set_item(h, s, (const TVITEMW *)lp, wide);
    case TVM_EDITLABELA: case TVM_EDITLABELW: return (LRESULT)edit_label(h, s, item_of(s, (HTREEITEM)lp));
    case TVM_GETEDITCONTROL: return (LRESULT)s->edit;
    case TVM_ENDEDITLABELNOW: end_edit(h, s, (BOOL)wp); return TRUE;
    case TVM_GETVISIBLECOUNT: return rows_visible(h, s);
    case TVM_HITTEST: {
        TVHITTESTINFO *t = (TVHITTESTINFO *)lp;
        if (!t) return 0;
        t->hItem = handle_of(s, hit_test(h, s, t->pt, &t->flags));
        return (LRESULT)t->hItem;
    }
    case TVM_CREATEDRAGIMAGE: return 0;
    case TVM_SORTCHILDREN: {
        TItem *p = lp ? item_of(s, (HTREEITEM)lp) : &s->root;
        if (!p) return FALSE;
        SortCtx c = { h, s, NULL, 0 };
        sort_children(&c, p, (int)wp);
        refresh(h, s);
        return TRUE;
    }
    case TVM_SORTCHILDRENCB: {
        const TVSORTCB *cb = (const TVSORTCB *)lp;
        if (!cb) return FALSE;
        TItem *p = cb->hParent && cb->hParent != TVI_ROOT ? item_of(s, cb->hParent) : &s->root;
        if (!p) return FALSE;
        SortCtx c = { h, s, cb->lpfnCompare, cb->lParam };
        sort_children(&c, p, (int)wp);
        refresh(h, s);
        return TRUE;
    }
    case TVM_ENSUREVISIBLE: {
        TItem *it = item_of(s, (HTREEITEM)lp);
        if (!it || it == &s->root) return FALSE;
        int t = s->top;
        ensure_visible(h, s, it);
        return t != s->top;
    }
    case TVM_GETISEARCHSTRINGA: case TVM_GETISEARCHSTRINGW: {
        if (GetTickCount() - s->search_tick > 1000) s->nsearch = 0;
        if (lp) copy_out((WCHAR *)lp, 64, s->search, msg == TVM_GETISEARCHSTRINGW);
        return s->nsearch;
    }
    case TVM_SETTOOLTIPS: { HWND o = s->tips; s->tips = (HWND)wp; return (LRESULT)o; }
    case TVM_GETTOOLTIPS: return (LRESULT)s->tips;
    case TVM_SETINSERTMARK: { s->ins_mark = lp ? item_of(s, (HTREEITEM)lp) : NULL; s->ins_after = (int)wp; InvalidateRect(h, NULL, TRUE); return TRUE; }
    case TVM_SETITEMHEIGHT: {
        int o = s->item_h;
        int v = (int)wp;
        s->item_h = v <= 0 ? default_item_h(h, s) : v;
        s->item_set = v > 0;
        if (!(style_of(h) & TVS_NONEVENHEIGHT) && (s->item_h & 1)) s->item_h++;
        refresh(h, s);
        return o;
    }
    case TVM_GETITEMHEIGHT: return s->item_h;
    case TVM_SETBKCOLOR: { COLORREF o = s->bk; s->bk = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return o; }
    case TVM_GETBKCOLOR: return s->bk;
    case TVM_SETTEXTCOLOR: { COLORREF o = s->text; s->text = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return o; }
    case TVM_GETTEXTCOLOR: return s->text;
    case TVM_SETLINECOLOR: { COLORREF o = s->line; s->line = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return o; }
    case TVM_GETLINECOLOR: return s->line;
    case TVM_SETINSERTMARKCOLOR: { COLORREF o = s->insmark; s->insmark = (COLORREF)lp; return o; }
    case TVM_GETINSERTMARKCOLOR: return s->insmark;
    case TVM_SETSCROLLTIME: return 0;
    case TVM_GETSCROLLTIME: return 0;
    case TVM_SETEXTENDEDSTYLE: { DWORD o = s->ex, m = wp ? (DWORD)wp : 0xFFFFFFFFu; s->ex = (s->ex & ~m) | ((DWORD)lp & m); return 0; (void)o; }
    case TVM_GETEXTENDEDSTYLE: return (LRESULT)s->ex;
    case TVM_GETSELECTEDCOUNT: return s->sel ? 1 : 0;
    case TVM_MAPACCIDTOHTREEITEM: case TVM_MAPHTREEITEMTOACCID: return (LRESULT)wp;
    case TVM_GETITEMPARTRECT: return FALSE;
    case TVM_SETUNICODEFORMAT_: { int o = !s->ansi; s->ansi = !wp; return o; }
    case TVM_GETUNICODEFORMAT_: return !s->ansi;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
