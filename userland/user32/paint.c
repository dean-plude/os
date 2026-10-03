/*
 * paint.c — update regions, WM_PAINT, device contexts, presenting the back
 * buffer, scrolling, the caret, and the non-client area
 *
 * Update regions are kept as rectangles.  A window paints over the part of
 * its parent it covers, parent first, so after a window paints, the
 * children (and higher siblings) it may have painted over are invalidated
 * there in turn.  Nothing reaches the screen until the whole top-level
 * window is painted: then the changed part of the back buffer is copied to
 * the desktop's bitmap.
 */
#include "u32.h"

void note_begin_paint(void);

/* -----------------------------------------------------------------------
 * Update regions
 * ----------------------------------------------------------------------- */
static void wake_owner(Wnd *w)
{
    if (w->tid && w->tid != GetCurrentThreadId()) NtNovaGuiCtl(0, CTL_WAKE, w->tid, NULL);
}

void invalidate(Wnd *w, const RECT *r, int erase, int children)
{
    if (!w || !w->used || w->h == GetDesktopWindow() || !wnd_visible(w)) return;
    RECT cr = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    RECT x;
    if (r) { if (!IntersectRect(&x, r, &cr)) return; }
    else x = cr;
    if (IsRectEmpty(&x)) return;
    if (w->has_upd && !IsRectEmpty(&w->upd)) UnionRect(&w->upd, &w->upd, &x);
    else w->upd = x;
    w->has_upd = 1;
    if (erase) w->erase = 1;
    wake_owner(w);
    if (children) {
        for (Wnd *c = w->child; c; c = c->next) {
            if (!(c->style & WS_VISIBLE)) continue;
            RECT o;
            if (!IntersectRect(&o, &x, &c->rect)) continue;
            OffsetRect(&o, -c->client.left, -c->client.top);
            invalidate(c, &o, erase, 1);
            if (c->rect.left != c->client.left || c->rect.top != c->client.top ||
                c->rect.right != c->client.right || c->rect.bottom != c->client.bottom) invalidate_nc(c);
        }
    }
}

void invalidate_nc(Wnd *w)
{
    if (!w || !w->used || !wnd_visible(w)) return;
    if (EqualRect(&w->rect, &w->client) && !w->parent && !w->menu) return;
    if (w->parent && EqualRect(&w->rect, &w->client)) return;
    w->nc_paint = 1;
    if (!w->has_upd) { SetRectEmpty(&w->upd); w->has_upd = 1; }
    wake_owner(w);
}

void validate(Wnd *w, const RECT *r)
{
    if (!w) return;
    if (!r) { w->has_upd = 0; w->erase = 0; w->nc_paint = 0; SetRectEmpty(&w->upd); return; }
    /* only whole strips come off a rectangle */
    RECT u = w->upd, x;
    if (!IntersectRect(&x, &u, r)) return;
    if (EqualRect(&x, &u)) { w->has_upd = w->nc_paint; SetRectEmpty(&w->upd); return; }
    SubtractRect(&w->upd, &u, r);
}

/* The next window of this thread that needs painting: parents before
 * children, and siblings from the top of the z-order down, as Windows
 * does.  Overlapping siblings without WS_CLIPSIBLINGS therefore end with
 * the lower one's pixels: NSIS's page header is a white static on top of
 * its title texts, which show through because they paint after it. */
static Wnd *find_paint(Wnd *w, HWND filter, int in_filter)
{
    if (!(w->style & WS_VISIBLE) || w->minimized) return NULL;
    if (w->h == filter) in_filter = 1;
    if (w->has_upd && in_filter) return w;
    for (Wnd *c = w->child; c; c = c->next) {
        Wnd *r = find_paint(c, filter, in_filter);
        if (r) return r;
    }
    return NULL;
}

Wnd *next_paint(DWORD tid, HWND filter)
{
    Wnd *d = W_quiet(GetDesktopWindow());
    for (Wnd *t = d->child; t; t = t->next) {
        if (t->tid != tid) continue;
        Wnd *r = find_paint(t, filter, filter == 0);
        if (r) return r;
    }
    return NULL;
}

static int top_needs_paint(Wnd *t) { return find_paint(t, 0, 1) != NULL; }

USERAPI BOOL InvalidateRect(HWND h, const RECT *r, BOOL erase)
{
    if (!h) {                                               /* all windows */
        Wnd *d = W_quiet(GetDesktopWindow());
        for (Wnd *t = d->child; t; t = t->next) { invalidate(t, NULL, erase, 1); invalidate_nc(t); }
        return TRUE;
    }
    Wnd *w = W(h);
    if (!w) return FALSE;
    invalidate(w, r, erase, !(w->style & WS_CLIPCHILDREN));
    return TRUE;
}

USERAPI BOOL InvalidateRgn(HWND h, HRGN rgn, BOOL erase)
{
    RECT r;
    if (rgn && GetRgnBox(rgn, &r)) return InvalidateRect(h, &r, erase);
    return InvalidateRect(h, NULL, erase);
}

USERAPI BOOL ValidateRect(HWND h, const RECT *r)
{
    Wnd *w = h ? W(h) : NULL;
    if (!w) return h == 0;
    validate(w, r);
    return TRUE;
}

USERAPI BOOL ValidateRgn(HWND h, HRGN rgn)
{
    RECT r;
    if (rgn && GetRgnBox(rgn, &r)) return ValidateRect(h, &r);
    return ValidateRect(h, NULL);
}

USERAPI BOOL GetUpdateRect(HWND h, LPRECT r, BOOL erase)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    int any = w->has_upd && !IsRectEmpty(&w->upd);
    if (r) { if (any) *r = w->upd; else SetRectEmpty(r); }
    if (any && erase && w->erase) {
        HDC dc = wnd_dc(w, 1, 0);
        IntersectClipRect(dc, w->upd.left, w->upd.top, w->upd.right, w->upd.bottom);
        if (send_msg(w, WM_ERASEBKGND, (WPARAM)dc, 0)) w->erase = 0;
        release_dc(dc);
    }
    return any;
}

USERAPI int GetUpdateRgn(HWND h, HRGN rgn, BOOL erase)
{
    RECT r;
    if (!GetUpdateRect(h, &r, erase)) { SetRectRgn(rgn, 0, 0, 0, 0); return NULLREGION; }
    SetRectRgn(rgn, r.left, r.top, r.right, r.bottom);
    return SIMPLEREGION;
}

USERAPI int ExcludeUpdateRgn(HDC dc, HWND h) { (void)dc; (void)h; return SIMPLEREGION; }

/* Paint now: WM_PAINT for the window (and its children) straight away */
static void paint_now(Wnd *w, int children)
{
    if (!w || !wnd_visible(w)) return;
    HWND h = w->h;
    for (int guard = 0; guard < 64; guard++) {
        Wnd *p = find_paint(w, 0, 1);
        if (!p || (!children && p != w)) break;
        HWND ph = p->h;
        int before = p->has_upd;
        send_msg(p, WM_PAINT, 0, 0);
        p = W_quiet(ph);
        if (p && p->has_upd && before) validate(p, NULL);    /* it didn't paint */
        if (!W_quiet(h)) return;
    }
    Wnd *t = top_of(w);
    if (!top_needs_paint(t)) present(t);
}

USERAPI BOOL UpdateWindow(HWND h)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    paint_now(w, 1);
    return TRUE;
}

USERAPI BOOL RedrawWindow(HWND h, const RECT *r, HRGN rgn, UINT flags)
{
    Wnd *w = h ? W(h) : W_quiet(GetDesktopWindow());
    if (!w) return FALSE;
    RECT rr, *pr = (RECT *)r;
    if (!pr && rgn && GetRgnBox(rgn, &rr)) pr = &rr;
    if (w->h == GetDesktopWindow()) {
        for (Wnd *t = w->child; t; t = t->next) RedrawWindow(t->h, NULL, NULL, flags);
        return TRUE;
    }
    int children = (flags & RDW_ALLCHILDREN) || (!(flags & RDW_NOCHILDREN) && !(w->style & WS_CLIPCHILDREN));
    if (flags & RDW_INVALIDATE) {
        invalidate(w, pr, (flags & RDW_ERASE) != 0, children);
        if (flags & RDW_FRAME) invalidate_nc(w);
    }
    if (flags & RDW_VALIDATE) validate(w, pr);
    if (flags & RDW_INTERNALPAINT) { if (!w->has_upd) { SetRectEmpty(&w->upd); w->has_upd = 1; } }
    if (flags & RDW_UPDATENOW) paint_now(w, children);
    else if (flags & RDW_ERASENOW) {
        if (w->nc_paint) { w->nc_paint = 0; send_msg(w, WM_NCPAINT, 1, 0); }
        if (w->has_upd && w->erase) GetUpdateRect(h, NULL, TRUE);
    }
    return TRUE;
}

USERAPI BOOL LockWindowUpdate(HWND h) { (void)h; return TRUE; }

/* -----------------------------------------------------------------------
 * Device contexts
 * ----------------------------------------------------------------------- */
typedef struct DcRec { struct DcRec *next; NOVA_DC *dc; HWND h; int own; } DcRec;
static DcRec *g_dcs;

static void clip_to(RECT *r, const RECT *by)
{
    if (r->left < by->left) r->left = by->left;
    if (r->top < by->top) r->top = by->top;
    if (r->right > by->right) r->right = by->right;
    if (r->bottom > by->bottom) r->bottom = by->bottom;
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
}

/* The part of window @w (its client area, or all of it) that can show,
 * in bitmap coordinates */
static void visible_rect(Wnd *w, int client, RECT *out)
{
    POINT o;
    wnd_to_bitmap(w, client, &o);
    int cw = client ? w->client.right - w->client.left : w->rect.right - w->rect.left;
    int ch = client ? w->client.bottom - w->client.top : w->rect.bottom - w->rect.top;
    SetRect(out, o.x, o.y, o.x + cw, o.y + ch);
    for (Wnd *a = w->parent; a; a = a->parent) {
        POINT ao;
        wnd_to_bitmap(a, 1, &ao);
        RECT ar = { ao.x, ao.y, ao.x + a->client.right - a->client.left, ao.y + a->client.bottom - a->client.top };
        clip_to(out, &ar);
    }
    Wnd *t = top_of(w);
    RECT b = { 0, 0, t->bw, t->bh };
    clip_to(out, &b);
}

static NOVA_DC g_screen_dc;
static DWORD g_screen_px[64 * 64];

HDC wnd_dc(Wnd *w, int client, int clip_children)
{
    (void)clip_children;
    Wnd *t = top_of(w);
    if (!t || !ensure_back(t)) return 0;
    NOVA_DC *d = calloc(1, sizeof(NOVA_DC));
    DcRec *rec = calloc(1, sizeof(DcRec));
    if (!d || !rec) { free(d); free(rec); return 0; }
    d->bits = t->back;
    d->stride = t->stride;
    d->w = t->bw; d->h = t->bh;
    POINT o;
    wnd_to_bitmap(w, client, &o);
    d->org_x = d->base_x = o.x;
    d->org_y = d->base_y = o.y;
    visible_rect(w, client, &d->vis);
    d->has_vis = 1;
    d->text_color = 0x000000;
    d->bk_color = 0xFFFFFF;
    d->bk_mode = OPAQUE;
    d->has_pen = 1; d->pen_color = 0; d->pen_width = 1;
    d->has_brush = 1; d->brush_color = 0xFFFFFF;
    d->hwnd = w->h;
    rec->dc = d; rec->h = w->h;
    LOCK();
    rec->next = g_dcs;
    g_dcs = rec;
    UNLOCK();
    return (HDC)d;
}

void release_dc(HDC dc)
{
    NOVA_DC *d = (NOVA_DC *)dc;
    if (!d || d == &g_screen_dc) return;
    LOCK();
    DcRec **pp = &g_dcs, *rec = NULL;
    for (; *pp; pp = &(*pp)->next) if ((*pp)->dc == d) { rec = *pp; *pp = rec->next; break; }
    UNLOCK();
    if (!rec) return;
    Wnd *w = W_quiet(rec->h);
    if (w) {
        RECT r = d->vis;
        if (d->has_clip) clip_to(&r, &d->clip);
        if (!IsRectEmpty(&r)) mark_dirty(top_of(w), &r);
    }
    free(rec);
    free(d);
}

static HDC screen_dc(void)
{
    if (!g_screen_dc.bits) {
        g_screen_dc.bits = g_screen_px;
        g_screen_dc.w = g_screen_dc.h = g_screen_dc.stride = 64;
        g_screen_dc.bk_color = 0xFFFFFF; g_screen_dc.bk_mode = OPAQUE;
        g_screen_dc.has_pen = 1; g_screen_dc.pen_width = 1;
    }
    return (HDC)&g_screen_dc;
}

USERAPI HDC GetDC(HWND h)
{
    if (!h || h == GetDesktopWindow()) return screen_dc();
    Wnd *w = W(h);
    return w ? wnd_dc(w, 1, (w->style & WS_CLIPCHILDREN) != 0) : 0;
}

USERAPI HDC GetWindowDC(HWND h)
{
    if (!h || h == GetDesktopWindow()) return screen_dc();
    Wnd *w = W(h);
    return w ? wnd_dc(w, 0, 0) : 0;
}

USERAPI HDC GetDCEx(HWND h, HRGN rgn, DWORD flags)
{
    if (!h || h == GetDesktopWindow()) return screen_dc();
    Wnd *w = W(h);
    if (!w) return 0;
    HDC dc = wnd_dc(w, !(flags & DCX_WINDOW), 0);
    RECT r;
    if (dc && rgn && (flags & (DCX_INTERSECTRGN | 0x0400)) && GetRgnBox(rgn, &r)) {
        if (flags & DCX_WINDOW) {}
        IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    }
    return dc;
}

USERAPI int ReleaseDC(HWND h, HDC dc)
{
    (void)h;
    if ((NOVA_DC *)dc == &g_screen_dc) return 1;
    NOVA_DC *d = (NOVA_DC *)dc;
    Wnd *w = d ? W_quiet(d->hwnd) : NULL;
    release_dc(dc);
    if (w) {
        Wnd *t = top_of(w);
        if (!top_needs_paint(t)) present(t);
    }
    return 1;
}

/* gdi32's: @r (logical units) of a window's DC changed outside painting
 * (a frame from OpenGL through a DC the program keeps): show it now */
USERAPI void NovaFlushDC(HDC dc, const RECT *r)
{
    NOVA_DC *d = (NOVA_DC *)dc;
    if (!d || d == &g_screen_dc || !r) return;
    Wnd *w = W_quiet(d->hwnd);
    if (!w || w->paint_dc == dc) return;                  /* EndPaint presents */
    RECT x = { r->left + d->org_x, r->top + d->org_y, r->right + d->org_x, r->bottom + d->org_y };
    if (d->has_vis) clip_to(&x, &d->vis);
    if (IsRectEmpty(&x)) return;
    Wnd *t = top_of(w);
    mark_dirty(t, &x);
    present(t);
}

USERAPI HWND WindowFromDC(HDC dc)
{
    NOVA_DC *d = (NOVA_DC *)dc;
    if (!d || d == &g_screen_dc) return 0;
    for (DcRec *r = g_dcs; r; r = r->next) if (r->dc == d) return r->h;
    return 0;
}

/* Our DCs draw without clipping, so a parent's painting reaches over its
 * children.  A WS_CLIPCHILDREN parent's painting must not show on them:
 * BeginPaint keeps the pixels of the children that are up to date and
 * EndPaint puts them back, where without the style the children repaint
 * (cascade).  Repainting them instead would loop when a child's painting
 * invalidates its parent, as SumatraPDF's tab bar does to its caption. */
typedef struct Kept {
    struct Kept *next;
    Wnd *c;
    RECT r;                         /* top-level bitmap coordinates */
    DWORD px[];
} Kept;

static void keep_children(Wnd *w, const RECT *rc)
{
    Wnd *t = top_of(w);
    if (!t || !t->back || !(w->style & WS_CLIPCHILDREN)) return;
    POINT o;
    wnd_to_bitmap(w, 1, &o);
    RECT pr = *rc;
    OffsetRect(&pr, o.x, o.y);
    for (Wnd *c = w->child; c; c = c->next) {
        if (!(c->style & WS_VISIBLE) || c->has_upd || c->nc_paint) continue;
        RECT v, r;
        visible_rect(c, 0, &v);
        if (!IntersectRect(&r, &v, &pr)) continue;
        int cw = r.right - r.left, ch = r.bottom - r.top;
        Kept *k = malloc(sizeof(Kept) + (size_t)cw * ch * 4);
        if (!k) continue;
        k->c = c;
        k->r = r;
        for (int y = 0; y < ch; y++)
            memcpy(k->px + (size_t)y * cw, t->back + (size_t)(r.top + y) * t->stride + r.left, (size_t)cw * 4);
        k->next = w->kept;
        w->kept = k;
    }
}

void paint_drop_kept(Wnd *w)
{
    for (Kept *k = w->kept, *n; k; k = n) { n = k->next; free(k); }
    w->kept = NULL;
}

/* back the kept children's pixels; whether @c was kept */
static int restore_kept(Wnd *w, Wnd *c)
{
    Wnd *t = top_of(w);
    for (Kept *k = w->kept; k; k = k->next) {
        if (k->c != c) continue;
        if (!t || !t->back || c->has_upd || c->nc_paint) return 0;   /* changed meanwhile: repaint it */
        int cw = k->r.right - k->r.left, ch = k->r.bottom - k->r.top;
        for (int y = 0; y < ch; y++)
            memcpy(t->back + (size_t)(k->r.top + y) * t->stride + k->r.left, k->px + (size_t)y * cw, (size_t)cw * 4);
        return 1;
    }
    return 0;
}

USERAPI HDC BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    Wnd *w = W(h);
    if (!w || !ps) return 0;
    caret_hide_for(w);
    note_begin_paint();
    if (w->nc_paint) {
        w->nc_paint = 0;
        send_msg(w, WM_NCPAINT, 1, 0);
        if (!W_quiet(h)) return 0;
    }
    RECT rc = w->has_upd ? w->upd : (RECT){ 0, 0, 0, 0 };
    int erase = w->erase;
    w->has_upd = 0; w->erase = 0;
    SetRectEmpty(&w->upd);
    memset(ps, 0, sizeof(*ps));
    HDC dc = wnd_dc(w, 1, 0);
    if (!dc) return 0;
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    paint_drop_kept(w);
    keep_children(w, &rc);
    ps->hdc = dc;
    ps->rcPaint = rc;
    w->paint_dc = dc;
    if (erase) ps->fErase = !send_msg(w, WM_ERASEBKGND, (WPARAM)dc, 0);
    return dc;
}

/* What a window painted may cover its children, and (WS_CLIPSIBLINGS:
 * we draw without clipping, so they repaint) the siblings above it */
static void cascade(Wnd *w, const RECT *rc)
{
    if (IsRectEmpty(rc)) return;
    for (Wnd *c = w->child; c; c = c->next) {
        if (!(c->style & WS_VISIBLE)) continue;
        RECT o;
        if (!IntersectRect(&o, rc, &c->rect)) continue;
        if (restore_kept(w, c)) continue;
        OffsetRect(&o, -c->client.left, -c->client.top);
        invalidate(c, &o, TRUE, 0);
        if (!EqualRect(&c->rect, &c->client)) invalidate_nc(c);
    }
    if (w->parent && (w->style & WS_CLIPSIBLINGS)) {
        RECT pr = *rc;                                      /* in the parent's client coordinates */
        OffsetRect(&pr, w->client.left, w->client.top);
        for (Wnd *s = w->prev; s; s = s->prev) {
            if (!(s->style & WS_VISIBLE)) continue;
            RECT o;
            if (!IntersectRect(&o, &pr, &s->rect)) continue;
            OffsetRect(&o, -s->client.left, -s->client.top);
            invalidate(s, &o, TRUE, 0);
            if (!EqualRect(&s->rect, &s->client)) invalidate_nc(s);
        }
    }
}

USERAPI BOOL EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    Wnd *w = W_quiet(h);
    if (w && ps && w->paint_dc == ps->hdc) w->paint_dc = 0;
    if (ps) release_dc(ps->hdc);
    if (!w) return TRUE;
    if (ps) cascade(w, &ps->rcPaint);
    paint_drop_kept(w);
    caret_restore();
    Wnd *t = top_of(w);
    if (!top_needs_paint(t)) present(t);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Presenting: the back buffer's changes to the desktop's bitmap
 * ----------------------------------------------------------------------- */
void mark_dirty(Wnd *t, const RECT *r)
{
    if (!t) return;
    if (t->has_dirty) UnionRect(&t->dirty, &t->dirty, r);
    else { t->dirty = *r; t->has_dirty = 1; }
}

void present(Wnd *t)
{
    if (!t || !t->has_dirty || !t->kid || !t->front || !t->back) return;
    RECT r = t->dirty;
    RECT b = { 0, 0, t->bw, t->bh };
    clip_to(&r, &b);
    t->has_dirty = 0;
    if (IsRectEmpty(&r)) return;
    size_t n = (size_t)(r.right - r.left) * 4;
    for (int y = r.top; y < r.bottom; y++) {
        size_t off = (size_t)y * t->stride + r.left;
        memcpy(t->front + off, t->back + off, n);
    }
    NtNovaGuiCtl(t->kid, CTL_PRESENT, 0, NULL);
}

void present_thread(DWORD tid)
{
    Wnd *d = W_quiet(GetDesktopWindow());
    for (Wnd *t = d->child; t; t = t->next) if (t->tid == tid && t->has_dirty) present(t);
}

void top_resized(Wnd *t)
{
    RECT all = { 0, 0, t->bw, t->bh };
    mark_dirty(t, &all);
    invalidate(t, NULL, TRUE, 1);
    invalidate_nc(t);
}

/* -----------------------------------------------------------------------
 * Scrolling
 * ----------------------------------------------------------------------- */
/* Move the pixels of @area (client coordinates) by dx, dy */
void scroll_bits(Wnd *w, int dx, int dy, const RECT *area)
{
    Wnd *t = top_of(w);
    if (!t->back) return;
    RECT vis;
    visible_rect(w, 1, &vis);
    POINT o;
    wnd_to_bitmap(w, 1, &o);
    RECT a = *area;
    OffsetRect(&a, o.x, o.y);
    clip_to(&a, &vis);
    /* source = destination - (dx, dy), both inside a */
    RECT dst = a;
    OffsetRect(&dst, dx, dy);
    clip_to(&dst, &a);
    if (IsRectEmpty(&dst)) return;
    int wpx = dst.right - dst.left;
    if (dy > 0) {
        for (int y = dst.bottom - 1; y >= dst.top; y--)
            memmove(t->back + (size_t)y * t->stride + dst.left, t->back + (size_t)(y - dy) * t->stride + dst.left - dx, (size_t)wpx * 4);
    } else {
        for (int y = dst.top; y < dst.bottom; y++)
            memmove(t->back + (size_t)y * t->stride + dst.left, t->back + (size_t)(y - dy) * t->stride + dst.left - dx, (size_t)wpx * 4);
    }
    mark_dirty(t, &dst);
}

USERAPI int ScrollWindowEx(HWND h, int dx, int dy, const RECT *scroll, const RECT *clip, HRGN rgn, LPRECT upd, UINT flags)
{
    Wnd *w = W(h);
    if (!w) return ERROR;
    caret_hide_for(w);
    RECT cr = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    RECT area = scroll ? *scroll : cr;
    if (clip) IntersectRect(&area, &area, clip);
    IntersectRect(&area, &area, &cr);
    if (wnd_visible(w)) scroll_bits(w, dx, dy, &area);
    /* the pending update moves along */
    if (w->has_upd && !IsRectEmpty(&w->upd)) OffsetRect(&w->upd, dx, dy);
    /* what was uncovered */
    RECT exposed = area;
    if (dx > 0) exposed.right = MIN(area.right, area.left + dx);
    else if (dx < 0) exposed.left = MAX(area.left, area.right + dx);
    RECT ex2 = area;
    if (dy > 0) ex2.bottom = MIN(area.bottom, area.top + dy);
    else if (dy < 0) ex2.top = MAX(area.top, area.bottom + dy);
    RECT u;
    SetRectEmpty(&u);
    if (dx) u = exposed;
    if (dy) { if (IsRectEmpty(&u)) u = ex2; else UnionRect(&u, &u, &ex2); }
    if (abs(dx) >= area.right - area.left || abs(dy) >= area.bottom - area.top) u = area;
    if (upd) *upd = u;
    if (rgn) SetRectRgn(rgn, u.left, u.top, u.right, u.bottom);
    if (flags & SW_SCROLLCHILDREN || !scroll) {
        for (Wnd *c = w->child; c; c = c->next) {
            OffsetRect(&c->rect, dx, dy);
            OffsetRect(&c->client, dx, dy);
            send_msg(c, WM_MOVE, 0, MAKELPARAM(c->client.left, c->client.top));
        }
        if (flags & SW_SCROLLCHILDREN) {}
    }
    if (flags & SW_INVALIDATE) {
        invalidate(w, &u, (flags & SW_ERASE) != 0, 1);
        /* children moved: their new places */
        for (Wnd *c = w->child; c; c = c->next) if (c->style & WS_VISIBLE) { invalidate(c, NULL, TRUE, 1); invalidate_nc(c); }
    }
    caret_restore();
    return IsRectEmpty(&u) ? NULLREGION : SIMPLEREGION;
}

USERAPI BOOL ScrollWindow(HWND h, int dx, int dy, const RECT *r, const RECT *clip)
{
    return ScrollWindowEx(h, dx, dy, r, clip, 0, NULL, SW_INVALIDATE | SW_ERASE | (r ? 0 : SW_SCROLLCHILDREN)) != ERROR;
}

USERAPI BOOL ScrollDC(HDC dc, int dx, int dy, const RECT *scroll, const RECT *clip, HRGN rgn, LPRECT upd)
{
    NOVA_DC *d = (NOVA_DC *)dc;
    HWND h = WindowFromDC(dc);
    Wnd *w = W_quiet(h);
    if (!w) { if (upd) SetRectEmpty(upd); return TRUE; }
    RECT area = scroll ? *scroll : (RECT){ 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    if (clip) IntersectRect(&area, &area, clip);
    (void)d;
    scroll_bits(w, dx, dy, &area);
    RECT u = area;
    if (dy > 0) u.bottom = MIN(area.bottom, area.top + dy);
    else if (dy < 0) u.top = MAX(area.top, area.bottom + dy);
    else if (dx > 0) u.right = MIN(area.right, area.left + dx);
    else if (dx < 0) u.left = MAX(area.left, area.right + dx);
    if (upd) *upd = u;
    if (rgn) SetRectRgn(rgn, u.left, u.top, u.right, u.bottom);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * The caret: drawn by inverting its rectangle in the back buffer
 * ----------------------------------------------------------------------- */
static struct {
    HWND h;
    int x, y, w, hh;
    int hide;                       /* HideCaret count; created hidden */
    int on;                         /* drawn now */
    int suspended;                  /* hidden while its window paints */
    ULONGLONG next;
    HBITMAP bmp;
} g_caret;

static void caret_invert(void)
{
    Wnd *w = W_quiet(g_caret.h);
    if (!w || !wnd_visible(w)) return;
    Wnd *t = top_of(w);
    if (!ensure_back(t)) return;
    RECT vis;
    visible_rect(w, 1, &vis);
    POINT o;
    wnd_to_bitmap(w, 1, &o);
    RECT r = { o.x + g_caret.x, o.y + g_caret.y, o.x + g_caret.x + g_caret.w, o.y + g_caret.y + g_caret.hh };
    clip_to(&r, &vis);
    int gray = g_caret.bmp == (HBITMAP)1;
    for (int y = r.top; y < r.bottom; y++) {
        DWORD *p = t->back + (size_t)y * t->stride;
        for (int x = r.left; x < r.right; x++) p[x] = gray ? p[x] ^ 0x808080 : p[x] ^ 0xFFFFFF;
    }
    if (!IsRectEmpty(&r)) mark_dirty(t, &r);
}

static void caret_set(int on)
{
    if (g_caret.on == on) return;
    caret_invert();
    g_caret.on = on;
}

void caret_hide_for(Wnd *w)
{
    if (!g_caret.h || !g_caret.on) return;
    Wnd *c = W_quiet(g_caret.h);
    if (!c || !w) return;
    if (c == w || is_child_of(w, c) || is_child_of(c, w)) { caret_set(0); g_caret.suspended = 1; }
}

void caret_restore(void)
{
    if (!g_caret.suspended) return;
    g_caret.suspended = 0;
    if (g_caret.h && !g_caret.hide) { caret_set(1); g_caret.next = GetTickCount64() + GetCaretBlinkTime(); }
}

void caret_blink(void)
{
    if (!g_caret.h || g_caret.hide) return;
    Wnd *w = W_quiet(g_caret.h);
    if (!w) { g_caret.h = 0; return; }
    if (w->tid != GetCurrentThreadId()) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_caret.next) return;
    g_caret.next = now + GetCaretBlinkTime();
    caret_set(!g_caret.on);
}

USERAPI BOOL CreateCaret(HWND h, HBITMAP bmp, int w, int hh)
{
    if (!W(h)) return FALSE;
    DestroyCaret();
    g_caret.h = h;
    g_caret.bmp = bmp;
    g_caret.w = w > 0 ? w : 1;
    g_caret.hh = hh > 0 ? hh : GetSystemMetrics(SM_CYBORDER) * 16;
    g_caret.x = g_caret.y = 0;
    g_caret.hide = 1;
    g_caret.on = 0;
    return TRUE;
}

USERAPI BOOL DestroyCaret(void)
{
    if (!g_caret.h) return FALSE;
    caret_set(0);
    g_caret.h = 0;
    g_caret.suspended = 0;
    return TRUE;
}

USERAPI BOOL ShowCaret(HWND h)
{
    if (!g_caret.h || (h && h != g_caret.h)) return FALSE;
    if (g_caret.hide > 0 && --g_caret.hide == 0) {
        caret_set(1);
        g_caret.next = GetTickCount64() + GetCaretBlinkTime();
        Wnd *w = W_quiet(g_caret.h);
        if (w) present(top_of(w));
    }
    return TRUE;
}

USERAPI BOOL HideCaret(HWND h)
{
    if (!g_caret.h || (h && h != g_caret.h)) return FALSE;
    if (g_caret.hide++ == 0) {
        caret_set(0);
        Wnd *w = W_quiet(g_caret.h);
        if (w && !g_caret.suspended) present(top_of(w));
    }
    return TRUE;
}

USERAPI BOOL SetCaretPos(int x, int y)
{
    if (!g_caret.h) return FALSE;
    if (x == g_caret.x && y == g_caret.y) return TRUE;
    int was = g_caret.on;
    caret_set(0);
    g_caret.x = x; g_caret.y = y;
    if ((was || !g_caret.hide) && !g_caret.suspended && !g_caret.hide) {
        caret_set(1);
        g_caret.next = GetTickCount64() + GetCaretBlinkTime();
    }
    return TRUE;
}

USERAPI BOOL GetCaretPos(LPPOINT p)
{
    if (!p) return FALSE;
    p->x = g_caret.x; p->y = g_caret.y;
    return g_caret.h != 0;
}

static UINT g_blink_ms = 530;
USERAPI UINT GetCaretBlinkTime(void) { return g_blink_ms; }
USERAPI BOOL SetCaretBlinkTime(UINT ms) { g_blink_ms = ms ? ms : 530; return TRUE; }

/* -----------------------------------------------------------------------
 * The non-client area: borders, scroll bars, the menu bar
 * ----------------------------------------------------------------------- */
void nc_paint(Wnd *w)
{
    if (!wnd_visible(w)) return;
    int ww = w->rect.right - w->rect.left, wh = w->rect.bottom - w->rect.top;
    HDC dc = wnd_dc(w, 0, 0);
    if (!dc) return;
    RECT r = { 0, 0, ww, wh };
    int top_framed = !w->parent && ((w->style & WS_CAPTION) == WS_CAPTION || !(w->style & WS_POPUP)) && !(w->flags & WF_MENU_TRACK);
    if (top_framed) {
        r.left += w->maximized ? 0 : FRAME_BORDER; r.right -= w->maximized ? 0 : FRAME_BORDER;
        r.top += FRAME_TITLE; r.bottom -= w->maximized ? 0 : FRAME_BORDER;
    } else {
        int b = 0;
        if (w->style & WS_THICKFRAME) b = w->parent ? 3 : 1;
        else if ((w->style & WS_CAPTION) == WS_DLGFRAME || (w->exstyle & WS_EX_DLGMODALFRAME)) b = w->parent ? 3 : 1;
        else if (w->style & WS_BORDER) b = 1;
        if (b) {
            COLORREF c = (w->style & WS_BORDER) && b == 1 && w->parent ? 0x646464 : sys_color(COLOR_3DSHADOW);
            if (!w->parent) c = 0xB0B0B0;
            for (int i = 0; i < b; i++) {
                frame_rect(dc, &r, i == 0 ? c : sys_color(COLOR_3DFACE));
                InflateRect(&r, -1, -1);
            }
        }
    }
    if (w->exstyle & WS_EX_CLIENTEDGE) {
        frame_rect(dc, &r, 0xA0A0A0);
        InflateRect(&r, -1, -1);
        frame_rect(dc, &r, sys_color(COLOR_WINDOW));
        InflateRect(&r, -1, -1);
    }
    if (w->exstyle & WS_EX_STATICEDGE) {
        frame_rect(dc, &r, sys_color(COLOR_3DSHADOW));
        InflateRect(&r, -1, -1);
    }
    if (!w->parent && w->menu && !(w->style & WS_CHILD)) {
        int mh = menu_bar_height(w, r.right - r.left);
        RECT mb = { r.left, r.top, r.right, r.top + mh };
        menu_bar_draw(w, dc, &mb);
    }
    if (w->style & (WS_VSCROLL | WS_HSCROLL)) {
        RECT hr, vr, corner;
        sb_nc_rects(w, &hr, &vr, &corner);
        if (w->style & WS_VSCROLL) sb_draw(w, dc, SB_VERT, &vr, 1);
        if (w->style & WS_HSCROLL) sb_draw(w, dc, SB_HORZ, &hr, 0);
        if (!IsRectEmpty(&corner)) fill_rect(dc, &corner, sys_color(COLOR_3DFACE));
    }
    release_dc(dc);
}

USERAPI BOOL PaintDesktop(HDC dc) { (void)dc; return TRUE; }
USERAPI BOOL GetWindowDCOrg_(HWND h) { (void)h; return FALSE; }
