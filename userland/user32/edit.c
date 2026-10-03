/*
 * edit.c — the EDIT class: single-line and multi-line text boxes with
 * selection, the clipboard, undo, word wrap and scroll bars
 */
#include "u32.h"

typedef struct {
    WCHAR *buf;
    int    len, cap, limit;
    int    anchor, caret;           /* the selection runs between them */
    int    xoff;                    /* horizontal scroll, pixels */
    int    top;                     /* first visible line */
    int   *ls, *le;                 /* line starts and ends (multi-line) */
    int    nlines, lcap;
    int    lh;                      /* line height */
    int    ascent;
    int    modified;
    WCHAR  pwchar;
    int    lm, rm;                  /* margins */
    int    focus, tracking;
    WCHAR *undo;
    int    undo_len, undo_anchor, undo_caret, can_undo;
    int    goal_x;                  /* for up/down */
    int    tabw;
    RECT   fmt;                     /* formatting rectangle (client coordinates) */
    int    fmt_set;
    WCHAR *cue;
    int    cue_focused;
    int    in_update;
} Ed;

#define MULTI(w) (((w)->style & ES_MULTILINE) != 0)
#define WRAP(w)  (MULTI(w) && !((w)->style & (ES_AUTOHSCROLL | WS_HSCROLL)))

static Ed *ed_of(Wnd *w) { return w ? (Ed *)w->ctl : NULL; }

/* A DC to measure text with (not the window's: nothing gets drawn) */
static HDC ed_dc(Wnd *w)
{
    HDC dc = GetDC(NULL);
    SelectObject(dc, ctl_font(w));
    return dc;
}

static void ed_release(Wnd *w, HDC dc) { (void)w; (void)dc; }

static int is_word(WCHAR c) { return c == '_' || IsCharAlphaNumericW(c); }

/* -----------------------------------------------------------------------
 * Geometry and layout
 * ----------------------------------------------------------------------- */
static void calc_fmt(Wnd *w, Ed *e)
{
    RECT c = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    if (!e->fmt_set) {
        e->fmt = c;
        e->fmt.left += e->lm;
        e->fmt.right -= e->rm;
        if (MULTI(w)) { e->fmt.top += 1; }
        else {
            int pad = (c.bottom - e->lh) / 2;
            if (pad < 0) pad = 0;
            e->fmt.top = pad;
            e->fmt.bottom = e->fmt.top + e->lh;
        }
    }
    if (e->fmt.right < e->fmt.left + 1) e->fmt.right = e->fmt.left + 1;
}

static int seg_width(HDC dc, Ed *e, const WCHAR *s, int n, int start_x)
{
    int x = start_x, st = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && s[i] != '\t') continue;
        x += text_width(dc, s + st, i - st);
        if (i < n) x = ((x - start_x) / e->tabw + 1) * e->tabw + start_x;
        st = i + 1;
    }
    return x - start_x;
}

static WCHAR *display_text(Ed *e, int start, int n, WCHAR *tmp, int cap)
{
    if (!e->pwchar) return e->buf + start;
    for (int i = 0; i < n && i < cap; i++) tmp[i] = e->pwchar;
    return tmp;
}

static void add_line(Ed *e, int s, int en)
{
    if (e->nlines == e->lcap) {
        e->lcap = e->lcap ? e->lcap * 2 : 64;
        e->ls = realloc(e->ls, sizeof(int) * e->lcap);
        e->le = realloc(e->le, sizeof(int) * e->lcap);
    }
    e->ls[e->nlines] = s; e->le[e->nlines] = en; e->nlines++;
}

static void layout(Wnd *w, Ed *e)
{
    e->nlines = 0;
    if (!MULTI(w)) { add_line(e, 0, e->len); return; }
    HDC dc = WRAP(w) ? ed_dc(w) : NULL;
    int width = e->fmt.right - e->fmt.left;
    int pos = 0;
    for (;;) {
        int end = pos;
        while (end < e->len && e->buf[end] != '\r' && e->buf[end] != '\n') end++;
        if (dc && width > 4) {
            int s = pos;
            while (1) {
                if (seg_width(dc, e, e->buf + s, end - s, 0) <= width) { add_line(e, s, end); break; }
                int fit = s + 1;
                /* longest prefix that fits */
                int lo = s + 1, hi = end;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    if (seg_width(dc, e, e->buf + s, mid - s, 0) <= width) { fit = mid; lo = mid + 1; }
                    else hi = mid - 1;
                }
                int brk = -1;
                for (int i = fit; i > s; i--) if (e->buf[i - 1] == ' ' || e->buf[i - 1] == '\t') { brk = i; break; }
                int cut = brk > s ? brk : fit;
                add_line(e, s, cut);
                s = cut;
                if (s >= end) break;
            }
        } else add_line(e, pos, end);
        if (end >= e->len) break;
        pos = end + 1;
        if (e->buf[end] == '\r' && pos < e->len && e->buf[pos] == '\n') pos++;
        else if (e->buf[end] == '\r' && pos < e->len && e->buf[pos] == '\r' && pos + 1 < e->len && e->buf[pos + 1] == '\n') pos += 2;
        if (pos > e->len) break;
        if (pos == e->len) { add_line(e, pos, pos); break; }
    }
    if (!e->nlines) add_line(e, 0, 0);
    if (dc) ed_release(w, dc);
}

static int line_of(Ed *e, int idx)
{
    for (int i = e->nlines - 1; i >= 0; i--) if (idx >= e->ls[i]) {
        /* at a soft break the index belongs to the next line */
        return i;
    }
    return 0;
}

static int visible_lines(Wnd *w, Ed *e)
{
    int n = (e->fmt.bottom - e->fmt.top) / (e->lh ? e->lh : 1);
    return n < 1 ? 1 : n;
}

/* Pixel position of character index @idx (client coordinates) */
static POINT pos_of(Wnd *w, Ed *e, int idx)
{
    HDC dc = ed_dc(w);
    int ln = line_of(e, idx);
    WCHAR tmp[512];
    int st = e->ls[ln];
    int n = MIN(idx, e->le[ln]) - st;
    if (n < 0) n = 0;
    int x = seg_width(dc, e, display_text(e, st, n, tmp, 512), n, 0);
    ed_release(w, dc);
    int lw = 0;
    if ((w->style & (ES_CENTER | ES_RIGHT)) && (!MULTI(w) || WRAP(w) || 1)) {
        HDC d2 = ed_dc(w);
        int ln_n = e->le[ln] - st;
        lw = seg_width(d2, e, display_text(e, st, ln_n, tmp, 512), ln_n, 0);
        ed_release(w, d2);
    }
    int width = e->fmt.right - e->fmt.left;
    int base = e->fmt.left;
    if ((w->style & ES_CENTER) && lw < width) base += (width - lw) / 2;
    else if ((w->style & ES_RIGHT) && lw < width) base += width - lw;
    POINT p = { base + x - e->xoff, e->fmt.top + (ln - e->top) * e->lh };
    return p;
}

/* Character index at a client point */
static int char_at(Wnd *w, Ed *e, int x, int y)
{
    int ln = MULTI(w) ? e->top + (y - e->fmt.top) / (e->lh ? e->lh : 1) : 0;
    if (y < e->fmt.top && MULTI(w)) ln = e->top - 1;
    if (ln < 0) ln = 0;
    if (ln >= e->nlines) ln = e->nlines - 1;
    int st = e->ls[ln], en = e->le[ln];
    POINT p0 = pos_of(w, e, st);
    int rx = x - p0.x;
    if (rx <= 0) return st;
    HDC dc = ed_dc(w);
    WCHAR tmp[512];
    int best = en, prev = 0;
    for (int i = st + 1; i <= en; i++) {
        int n = i - st;
        int cw = seg_width(dc, e, display_text(e, st, n, tmp, 512), n, 0);
        if (cw >= rx) { best = (rx - prev < cw - rx) ? i - 1 : i; break; }
        prev = cw;
    }
    ed_release(w, dc);
    return best;
}

/* -----------------------------------------------------------------------
 * Scrolling and the caret
 * ----------------------------------------------------------------------- */
static void update_scrollbars(Wnd *w, Ed *e)
{
    if (!MULTI(w)) return;
    if (w->style & WS_VSCROLL) {
        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0, e->nlines - 1, (UINT)visible_lines(w, e), e->top, 0 };
        SetScrollInfo(w->h, SB_VERT, &si, TRUE);
    }
    if (w->style & WS_HSCROLL) {
        int maxw = 0;
        HDC dc = ed_dc(w);
        for (int i = 0; i < e->nlines && i < 2000; i++) {
            int lw = seg_width(dc, e, e->buf + e->ls[i], e->le[i] - e->ls[i], 0);
            if (lw > maxw) maxw = lw;
        }
        ed_release(w, dc);
        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0, maxw + 8, (UINT)(e->fmt.right - e->fmt.left), e->xoff, 0 };
        SetScrollInfo(w->h, SB_HORZ, &si, TRUE);
    }
}

static void place_caret(Wnd *w, Ed *e)
{
    if (!e->focus) return;
    POINT p = pos_of(w, e, e->caret);
    SetCaretPos(p.x, p.y);
}

static void scroll_to_caret(Wnd *w, Ed *e)
{
    int ln = line_of(e, e->caret);
    int vis = visible_lines(w, e);
    int old_top = e->top, old_x = e->xoff;
    if (MULTI(w)) {
        if (ln < e->top) e->top = ln;
        else if (ln >= e->top + vis) e->top = ln - vis + 1;
    }
    if (!WRAP(w)) {
        POINT p = pos_of(w, e, e->caret);
        int width = e->fmt.right - e->fmt.left;
        int x = p.x + e->xoff - e->fmt.left;
        if (x < e->xoff) e->xoff = MAX(0, x - width / 4);
        else if (x > e->xoff + width - 1) e->xoff = x - width + width / 4;
        if (!MULTI(w) && !(w->style & ES_AUTOHSCROLL) && 0) e->xoff = 0;
    } else e->xoff = 0;
    if (old_top != e->top || old_x != e->xoff) {
        invalidate(w, NULL, TRUE, 0);
        update_scrollbars(w, e);
        if (old_top != e->top && w->parent) notify_parent(w, EN_VSCROLL);
        if (old_x != e->xoff && w->parent) notify_parent(w, EN_HSCROLL);
    }
    place_caret(w, e);
}

static void redraw(Wnd *w) { invalidate(w, NULL, FALSE, 0); }

/* -----------------------------------------------------------------------
 * Editing
 * ----------------------------------------------------------------------- */
static int sel_lo(Ed *e) { return MIN(e->anchor, e->caret); }
static int sel_hi(Ed *e) { return MAX(e->anchor, e->caret); }

static void save_undo(Ed *e)
{
    free(e->undo);
    e->undo = malloc(2 * ((size_t)e->len + 1));
    if (e->undo) { memcpy(e->undo, e->buf, 2 * (size_t)e->len); e->undo_len = e->len; }
    e->undo_anchor = e->anchor; e->undo_caret = e->caret;
    e->can_undo = e->undo != NULL;
}

static int reserve(Ed *e, int n)
{
    if (n + 1 <= e->cap) return 1;
    int nc = e->cap ? e->cap : 64;
    while (nc < n + 1) nc *= 2;
    WCHAR *b = realloc(e->buf, 2 * (size_t)nc);
    if (!b) return 0;
    e->buf = b; e->cap = nc;
    return 1;
}

static void changed(Wnd *w, Ed *e, int notify)
{
    HWND h = w->h;
    e->modified = 1;
    layout(w, e);
    if (e->top > e->nlines - 1) e->top = MAX(0, e->nlines - 1);
    if (notify && w->parent) notify_parent(w, EN_UPDATE);
    if (!W_quiet(h)) return;
    invalidate(w, NULL, TRUE, 0);
    update_scrollbars(w, e);
    scroll_to_caret(w, e);
    if (notify && w->parent) notify_parent(w, EN_CHANGE);
}

/* Replace the selection with @s (n characters) */
static int replace_sel(Wnd *w, Ed *e, const WCHAR *s, int n, int undoable, int notify)
{
    int lo = sel_lo(e), hi = sel_hi(e);
    WCHAR *conv = NULL;
    if (!MULTI(w)) {                                        /* single line: stop at a line break */
        for (int i = 0; i < n; i++) if (s[i] == '\r' || s[i] == '\n') { n = i; break; }
    } else {
        /* bare LFs become CR LF */
        int extra = 0;
        for (int i = 0; i < n; i++) if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) extra++;
        if (extra) {
            conv = malloc(2 * ((size_t)n + extra + 1));
            if (conv) {
                int k = 0;
                for (int i = 0; i < n; i++) {
                    if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) conv[k++] = '\r';
                    conv[k++] = s[i];
                }
                s = conv; n = k;
            }
        }
    }
    int room = e->limit - (e->len - (hi - lo));
    int over = 0;
    if (n > room) { n = room < 0 ? 0 : room; over = 1; }
    if (w->style & (ES_UPPERCASE | ES_LOWERCASE)) {
        if (!conv) { conv = malloc(2 * ((size_t)n + 1)); if (conv) { memcpy(conv, s, 2 * (size_t)n); s = conv; } }
        if (conv) { if (w->style & ES_UPPERCASE) CharUpperBuffW(conv, (DWORD)n); else CharLowerBuffW(conv, (DWORD)n); }
    }
    if (undoable) save_undo(e);
    if (!reserve(e, e->len - (hi - lo) + n)) { free(conv); return 0; }
    memmove(e->buf + lo + n, e->buf + hi, 2 * (size_t)(e->len - hi));
    if (n) memcpy(e->buf + lo, s, 2 * (size_t)n);
    e->len = e->len - (hi - lo) + n;
    e->buf[e->len] = 0;
    e->anchor = e->caret = lo + n;
    free(conv);
    changed(w, e, notify);
    if (over && w->parent && W_quiet(w->h)) notify_parent(w, EN_MAXTEXT);
    return 1;
}

static void set_text_all(Wnd *w, Ed *e, const WCHAR *s)
{
    int n = wlen(s);
    e->anchor = 0; e->caret = e->len;
    int ro = 0;
    int lim = e->limit;
    e->limit = 0x7FFFFFFE;
    replace_sel(w, e, s ? s : L"", n, 0, 0);
    e->limit = lim;
    (void)ro;
    e->anchor = e->caret = 0;
    e->xoff = 0; e->top = 0;
    e->modified = 0;
    e->can_undo = 0;
    scroll_to_caret(w, e);
    invalidate(w, NULL, TRUE, 0);
}

static void set_sel(Wnd *w, Ed *e, int a, int c, int scroll)
{
    if (a < 0) a = 0;
    if (c < 0) c = e->len;
    if (a > e->len) a = e->len;
    if (c > e->len) c = e->len;
    /* never inside a CR LF */
    if (a > 0 && a < e->len && e->buf[a - 1] == '\r' && e->buf[a] == '\n') a++;
    if (c > 0 && c < e->len && e->buf[c - 1] == '\r' && e->buf[c] == '\n') c++;
    if (a == e->anchor && c == e->caret) { if (scroll) scroll_to_caret(w, e); return; }
    e->anchor = a; e->caret = c;
    redraw(w);
    if (scroll) scroll_to_caret(w, e);
    else place_caret(w, e);
}

static void copy_sel(Wnd *w, Ed *e)
{
    int lo = sel_lo(e), hi = sel_hi(e);
    if (lo == hi || e->pwchar) return;
    if (!OpenClipboard(w->h)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, 2 * ((size_t)(hi - lo) + 1));
    if (g) {
        WCHAR *p = GlobalLock(g);
        memcpy(p, e->buf + lo, 2 * (size_t)(hi - lo));
        p[hi - lo] = 0;
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

static void paste(Wnd *w, Ed *e)
{
    if (w->style & ES_READONLY) return;
    if (!OpenClipboard(w->h)) return;
    HANDLE g = GetClipboardData(CF_UNICODETEXT);
    if (g) {
        const WCHAR *p = GlobalLock(g);
        if (p) {
            int n = wlen(p);
            if (w->style & ES_NUMBER) {
                for (int i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') { n = -1; break; }
            }
            if (n >= 0) replace_sel(w, e, p, n, 1, 1);
            else MessageBeep(0);
        }
        GlobalUnlock(g);
    }
    CloseClipboard();
}

static void do_undo(Wnd *w, Ed *e)
{
    if (!e->can_undo || !e->undo) return;
    WCHAR *cur = malloc(2 * ((size_t)e->len + 1));
    int cur_len = e->len;
    if (cur) memcpy(cur, e->buf, 2 * (size_t)e->len);
    reserve(e, e->undo_len);
    memcpy(e->buf, e->undo, 2 * (size_t)e->undo_len);
    e->len = e->undo_len;
    e->buf[e->len] = 0;
    e->anchor = e->undo_anchor; e->caret = e->undo_caret;
    free(e->undo);
    e->undo = cur; e->undo_len = cur_len;
    changed(w, e, 1);
}

static int word_left(Ed *e, int i)
{
    while (i > 0 && !is_word(e->buf[i - 1])) i--;
    while (i > 0 && is_word(e->buf[i - 1])) i--;
    return i;
}

static int word_right(Ed *e, int i)
{
    while (i < e->len && is_word(e->buf[i])) i++;
    while (i < e->len && !is_word(e->buf[i]) && e->buf[i] != '\r') i++;
    return i;
}

/* -----------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */
static void paint(Wnd *w, Ed *e, HDC dc)
{
    RECT c = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    int ro = (w->style & ES_READONLY) || (w->style & WS_DISABLED);
    HBRUSH bg = ctl_color(w, ro ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT, dc);
    COLORREF tc = (w->style & WS_DISABLED) ? sys_color(COLOR_GRAYTEXT) : GetTextColor(dc);
    COLORREF bkc = GetBkColor(dc);
    FillRect(dc, &c, bg);
    HGDIOBJ of = SelectObject(dc, ctl_font(w));
    IntersectClipRect(dc, e->fmt.left, MULTI(w) ? e->fmt.top : 0, e->fmt.right, MULTI(w) ? e->fmt.bottom : c.bottom);
    int lo = sel_lo(e), hi = sel_hi(e);
    int show_sel = lo != hi && (e->focus || (w->style & ES_NOHIDESEL));
    if (!e->len && e->cue && (!e->focus || e->cue_focused)) {
        SetTextColor(dc, sys_color(COLOR_GRAYTEXT));
        SetBkMode(dc, TRANSPARENT);
        ExtTextOutW(dc, e->fmt.left, e->fmt.top, 0, NULL, e->cue, (UINT)wlen(e->cue), NULL);
        SelectObject(dc, of);
        return;
    }
    int vis = visible_lines(w, e) + 1;
    WCHAR tmp[512];
    for (int ln = e->top; ln < e->nlines && ln < e->top + vis; ln++) {
        int st = e->ls[ln], en = e->le[ln];
        POINT p = pos_of(w, e, st);
        int y = p.y;
        /* runs: before, selected, after */
        int cuts[4] = { st, MAX(st, MIN(lo, en)), MAX(st, MIN(hi, en)), en };
        if (!show_sel) { cuts[1] = cuts[2] = en; }
        int x = p.x;
        for (int r = 0; r < 3; r++) {
            int a = cuts[r], b = cuts[r + 1];
            if (b <= a) continue;
            int n = b - a;
            const WCHAR *s = display_text(e, a, n, tmp, 512);
            int wd = seg_width(dc, e, s, n, x - p.x);
            if (r == 1) {
                SetTextColor(dc, e->focus ? sys_color(COLOR_HIGHLIGHTTEXT) : tc);
                RECT sr = { x, y, x + wd, y + e->lh };
                fill_rect(dc, &sr, e->focus ? sys_color(COLOR_HIGHLIGHT) : 0xD9D9D9);
            } else SetTextColor(dc, tc);
            SetBkMode(dc, TRANSPARENT);
            /* text with tabs */
            int cx = x, s0 = 0;
            for (int i = 0; i <= n; i++) {
                if (i < n && s[i] != '\t') continue;
                if (i > s0) ExtTextOutW(dc, cx, y, 0, NULL, s + s0, (UINT)(i - s0), NULL);
                cx += text_width(dc, s + s0, i - s0);
                if (i < n) cx = ((cx - p.x) / e->tabw + 1) * e->tabw + p.x;
                s0 = i + 1;
            }
            x += wd;
        }
        /* a selected line break shows as a little block */
        if (show_sel && lo <= en && hi > en && ln + 1 < e->nlines) {
            RECT sr = { x, y, x + 4, y + e->lh };
            fill_rect(dc, &sr, e->focus ? sys_color(COLOR_HIGHLIGHT) : 0xD9D9D9);
        }
    }
    (void)bkc;
    SelectObject(dc, of);
}

/* -----------------------------------------------------------------------
 * Keys
 * ----------------------------------------------------------------------- */
static void move_to(Wnd *w, Ed *e, int idx, int extend)
{
    if (idx < 0) idx = 0;
    if (idx > e->len) idx = e->len;
    set_sel(w, e, extend ? e->anchor : idx, idx, 1);
}

static int line_move(Wnd *w, Ed *e, int dl)
{
    int ln = line_of(e, e->caret);
    int nl = ln + dl;
    if (nl < 0) nl = 0;
    if (nl >= e->nlines) nl = e->nlines - 1;
    POINT p = pos_of(w, e, e->caret);
    if (e->goal_x < 0) e->goal_x = p.x;
    int y = e->fmt.top + (nl - e->top) * e->lh + 1;
    return char_at(w, e, e->goal_x, y);
}

static void key_down(Wnd *w, Ed *e, WPARAM vk)
{
    int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    int keep_goal = 0;
    switch (vk) {
    case VK_LEFT:
        if (!shift && sel_lo(e) != sel_hi(e) && !ctrl) { move_to(w, e, sel_lo(e), 0); break; }
        {
            int i = ctrl ? word_left(e, e->caret) : e->caret - 1;
            if (i > 0 && e->buf[i] == '\n' && e->buf[i - 1] == '\r') i--;
            move_to(w, e, i, shift);
        }
        break;
    case VK_RIGHT:
        if (!shift && sel_lo(e) != sel_hi(e) && !ctrl) { move_to(w, e, sel_hi(e), 0); break; }
        {
            int i = ctrl ? word_right(e, e->caret) : e->caret + 1;
            if (i < e->len && e->buf[i - 1] == '\r' && e->buf[i] == '\n') i++;
            move_to(w, e, i, shift);
        }
        break;
    case VK_UP: case VK_DOWN:
        if (!MULTI(w)) { move_to(w, e, vk == VK_UP ? 0 : e->len, shift); break; }
        move_to(w, e, line_move(w, e, vk == VK_UP ? -1 : 1), shift);
        keep_goal = 1;
        break;
    case VK_PRIOR: case VK_NEXT:
        if (!MULTI(w)) break;
        {
            int d = visible_lines(w, e);
            int idx = line_move(w, e, vk == VK_PRIOR ? -d : d);
            e->top += vk == VK_PRIOR ? -d : d;
            if (e->top > e->nlines - d) e->top = e->nlines - d;
            if (e->top < 0) e->top = 0;
            invalidate(w, NULL, TRUE, 0);
            update_scrollbars(w, e);
            move_to(w, e, idx, shift);
            keep_goal = 1;
        }
        break;
    case VK_HOME:
        move_to(w, e, ctrl || !MULTI(w) ? 0 : e->ls[line_of(e, e->caret)], shift);
        break;
    case VK_END:
        move_to(w, e, ctrl || !MULTI(w) ? e->len : e->le[line_of(e, e->caret)], shift);
        break;
    case VK_DELETE:
        if (w->style & ES_READONLY) break;
        if (shift) { copy_sel(w, e); if (sel_lo(e) != sel_hi(e)) replace_sel(w, e, L"", 0, 1, 1); break; }
        if (sel_lo(e) == sel_hi(e)) {
            int end = ctrl ? word_right(e, e->caret) : e->caret + 1;
            if (end > e->len) break;
            if (end < e->len && e->buf[end - 1] == '\r' && e->buf[end] == '\n') end++;
            e->anchor = e->caret; e->caret = end;
        }
        replace_sel(w, e, L"", 0, 1, 1);
        break;
    case VK_INSERT:
        if (ctrl) copy_sel(w, e);
        else if (shift) paste(w, e);
        break;
    case VK_RETURN:
        if (MULTI(w) && !(w->style & ES_WANTRETURN) && w->parent && (w->parent->flags & WF_DIALOG)) {
            LRESULT def = send_msg(w->parent, DM_GETDEFID, 0, 0);
            WORD id = HIWORD(def) == DC_HASDEFID ? LOWORD(def) : IDOK;
            HWND b = GetDlgItem(w->parent->h, id);
            PostMessageW(w->parent->h, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)b);
        }
        break;
    case VK_ESCAPE:
        if (MULTI(w) && w->parent && (w->parent->flags & WF_DIALOG))
            PostMessageW(w->parent->h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(w->parent->h, IDCANCEL));
        break;
    }
    if (!keep_goal) e->goal_x = -1;
}

static void key_char(Wnd *w, Ed *e, WCHAR c)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (c) {
    case 1:  set_sel(w, e, 0, e->len, 0); return;           /* Ctrl+A */
    case 3:  copy_sel(w, e); return;                        /* Ctrl+C */
    case 22: paste(w, e); return;                           /* Ctrl+V */
    case 24:                                                /* Ctrl+X */
        if (w->style & ES_READONLY) return;
        copy_sel(w, e);
        if (sel_lo(e) != sel_hi(e)) replace_sel(w, e, L"", 0, 1, 1);
        return;
    case 26: if (!(w->style & ES_READONLY)) do_undo(w, e); return;   /* Ctrl+Z */
    case 0x7F:                                              /* Ctrl+Backspace */
        if (w->style & ES_READONLY) return;
        if (sel_lo(e) == sel_hi(e)) { e->anchor = word_left(e, e->caret); }
        replace_sel(w, e, L"", 0, 1, 1);
        return;
    case '\b':
        if (w->style & ES_READONLY) return;
        if (sel_lo(e) == sel_hi(e)) {
            if (!e->caret) return;
            int st = e->caret - 1;
            if (st > 0 && e->buf[st] == '\n' && e->buf[st - 1] == '\r') st--;
            if (ctrl) st = word_left(e, e->caret);
            e->anchor = st;
        }
        replace_sel(w, e, L"", 0, 1, 1);
        return;
    case '\r': case '\n':
        if (!MULTI(w) || (w->style & ES_READONLY)) return;
        if (!(w->style & ES_WANTRETURN) && w->parent && (w->parent->flags & WF_DIALOG)) return;
        replace_sel(w, e, L"\r\n", 2, 1, 1);
        return;
    case '\t':
        if (!MULTI(w) || (w->style & ES_READONLY)) return;
        replace_sel(w, e, L"\t", 1, 1, 1);
        return;
    case 0x1B: return;
    }
    if (c < 0x20 || (w->style & ES_READONLY)) return;
    if ((w->style & ES_NUMBER) && (c < '0' || c > '9')) {
        MessageBeep(MB_OK);
        return;
    }
    replace_sel(w, e, &c, 1, 1, 1);
}

/* -----------------------------------------------------------------------
 * The window procedure
 * ----------------------------------------------------------------------- */
static void setup_font(Wnd *w, Ed *e)
{
    HDC dc = ed_dc(w);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    ed_release(w, dc);
    e->lh = tm.tmHeight > 0 ? tm.tmHeight : 16;
    e->ascent = tm.tmAscent;
    e->tabw = (tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 7) * 8;
    e->lm = e->rm = MAX(1, tm.tmAveCharWidth / 2);
    calc_fmt(w, e);
    layout(w, e);
}

static LRESULT get_line(Ed *e, int ln, WCHAR *buf)
{
    if (ln < 0 || ln >= e->nlines) return 0;
    int cap = *(WORD *)buf;
    int n = MIN(e->le[ln] - e->ls[ln], cap);
    memcpy(buf, e->buf + e->ls[ln], 2 * (size_t)n);
    if (n < cap) buf[n] = 0;
    return n;
}

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    Ed *e = ed_of(w);
    if (!e && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE: {
        e = calloc(1, sizeof(Ed));
        if (!e) return FALSE;
        w->ctl = e;
        e->limit = 0x7FFFFFFE;
        e->goal_x = -1;
        e->lh = 16;
        e->tabw = 56;
        if (w->style & ES_PASSWORD) e->pwchar = 0x25CF;
        if (MULTI(w)) w->style &= ~ES_PASSWORD, e->pwchar = 0;
        if (!MULTI(w)) w->style &= ~(WS_HSCROLL | WS_VSCROLL);
        reserve(e, 16);
        e->buf[0] = 0;
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_CREATE: {
        setup_font(w, e);
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        const WCHAR *t = w->text;
        (void)cs;
        if (t && *t) set_text_all(w, e, t);
        else { layout(w, e); update_scrollbars(w, e); }
        return 0;
    }
    case WM_NCDESTROY:
        free(e->buf); free(e->ls); free(e->le); free(e->undo); free(e->cue);
        free(e);
        w->ctl = NULL;
        return 0;
    case WM_SIZE:
        if (!e->fmt_set) calc_fmt(w, e);
        layout(w, e);
        update_scrollbars(w, e);
        invalidate(w, NULL, TRUE, 0);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) paint(w, e, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: paint(w, e, (HDC)wp); return 0;
    case WM_NOVA_RESCALE:                                   /* its window's DPI changed: the default font's size */
        if (w->font) return 0;
        setup_font(w, e);
        if (e->focus) { DestroyCaret(); CreateCaret(h, 0, 1, e->lh); place_caret(w, e); ShowCaret(h); }
        invalidate(w, NULL, TRUE, 0);
        return 0;
    case WM_SETFONT:
        w->font = (HFONT)wp;
        setup_font(w, e);
        if (e->focus) { DestroyCaret(); CreateCaret(h, 0, 1, e->lh); place_caret(w, e); ShowCaret(h); }
        if (lp) invalidate(w, NULL, TRUE, 0);
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_SETTEXT:
        set_text_all(w, e, (const WCHAR *)lp);
        set_text(w, (const WCHAR *)lp);
        if (w->parent) { notify_parent(w, EN_UPDATE); if (W_quiet(h)) notify_parent(w, EN_CHANGE); }
        return TRUE;
    case WM_GETTEXT: {
        if (!wp || !lp) return 0;
        int n = MIN(e->len, (int)wp - 1);
        memcpy((void *)lp, e->buf, 2 * (size_t)n);
        ((WCHAR *)lp)[n] = 0;
        return n;
    }
    case WM_GETTEXTLENGTH: return e->len;
    case WM_GETDLGCODE: {
        LRESULT r = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
        if (MULTI(w) || combo_edit_wants_all(w)) r |= DLGC_WANTALLKEYS;
        MSG *m = (MSG *)lp;
        if (m && m->message == WM_KEYDOWN && MULTI(w)) {
            if ((m->wParam == VK_RETURN && !(w->style & ES_WANTRETURN)) || m->wParam == VK_ESCAPE) r &= ~DLGC_WANTALLKEYS;
        }
        return r;
    }
    case WM_SETFOCUS:
        e->focus = 1;
        CreateCaret(h, 0, 1, e->lh);
        place_caret(w, e);
        ShowCaret(h);
        redraw(w);
        if (w->parent) notify_parent(w, EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        e->focus = 0;
        if (e->tracking) { e->tracking = 0; ReleaseCapture(); }
        DestroyCaret();
        redraw(w);
        if (w->parent) notify_parent(w, EN_KILLFOCUS);
        return 0;
    case WM_ENABLE: redraw(w); return 0;
    case WM_LBUTTONDOWN: {
        set_focus(h);
        if (!W_quiet(h)) return 0;
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int idx = char_at(w, e, x, y);
        set_sel(w, e, (wp & MK_SHIFT) ? e->anchor : idx, idx, 1);
        e->tracking = 1;
        e->goal_x = -1;
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int idx = char_at(w, e, x, y);
        int a = idx, b = idx;
        if (idx < e->len && is_word(e->buf[idx])) { while (a > 0 && is_word(e->buf[a - 1])) a--; while (b < e->len && is_word(e->buf[b])) b++; }
        else if (idx > 0 && is_word(e->buf[idx - 1])) { a = idx - 1; while (a > 0 && is_word(e->buf[a - 1])) a--; }
        else if (idx < e->len && e->buf[idx] != '\r') b = idx + 1;
        set_sel(w, e, a, b, 1);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->tracking) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            int idx = char_at(w, e, x, y);
            if (idx != e->caret) set_sel(w, e, e->anchor, idx, 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->tracking) { e->tracking = 0; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED: e->tracking = 0; return 0;
    case WM_MOUSEWHEEL:
        if (MULTI(w)) {
            int d = -(short)HIWORD(wp) / 40;
            if (!d) d = (short)HIWORD(wp) > 0 ? -1 : 1;
            SendMessageW(h, EM_LINESCROLL, 0, d);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_KEYDOWN:
        if (combo_edit_key(w, msg, wp)) return 0;
        key_down(w, e, wp);
        return 0;
    case WM_SYSKEYDOWN:
        if (combo_edit_key(w, msg, wp)) return 0;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CHAR: key_char(w, e, (WCHAR)wp); return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        int code = LOWORD(wp);
        if (msg == WM_VSCROLL) {
            int t = e->top, vis = visible_lines(w, e);
            switch (code) {
            case SB_LINEUP: t--; break;
            case SB_LINEDOWN: t++; break;
            case SB_PAGEUP: t -= vis; break;
            case SB_PAGEDOWN: t += vis; break;
            case SB_TOP: t = 0; break;
            case SB_BOTTOM: t = e->nlines; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: t = HIWORD(wp); break;
            }
            if (t > e->nlines - vis) t = e->nlines - vis;
            if (t < 0) t = 0;
            if (t != e->top) { e->top = t; invalidate(w, NULL, TRUE, 0); update_scrollbars(w, e); place_caret(w, e); if (w->parent) notify_parent(w, EN_VSCROLL); }
        } else {
            int x = e->xoff, page = e->fmt.right - e->fmt.left;
            switch (code) {
            case SB_LINELEFT: x -= 8; break;
            case SB_LINERIGHT: x += 8; break;
            case SB_PAGELEFT: x -= page; break;
            case SB_PAGERIGHT: x += page; break;
            case SB_LEFT: x = 0; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: x = HIWORD(wp); break;
            }
            if (x < 0) x = 0;
            if (x != e->xoff) { e->xoff = x; invalidate(w, NULL, TRUE, 0); update_scrollbars(w, e); place_caret(w, e); if (w->parent) notify_parent(w, EN_HSCROLL); }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        HMENU m = CreatePopupMenu();
        int lo = sel_lo(e), hi = sel_hi(e), ro = (w->style & ES_READONLY) != 0;
        AppendMenuW(m, e->can_undo && !ro ? MF_STRING : MF_GRAYED, 1, L"&Undo");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, lo != hi && !ro && !e->pwchar ? MF_STRING : MF_GRAYED, 2, L"Cu&t");
        AppendMenuW(m, lo != hi && !e->pwchar ? MF_STRING : MF_GRAYED, 3, L"&Copy");
        AppendMenuW(m, !ro && IsClipboardFormatAvailable(CF_UNICODETEXT) ? MF_STRING : MF_GRAYED, 4, L"&Paste");
        AppendMenuW(m, lo != hi && !ro ? MF_STRING : MF_GRAYED, 5, L"&Delete");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, 6, L"Select &All");
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (lp == -1) { POINT p = pos_of(w, e, e->caret); ClientToScreen(h, &p); x = p.x; y = p.y; }
        set_focus(h);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, h, NULL);
        DestroyMenu(m);
        if (!W_quiet(h)) return 0;
        switch (cmd) {
        case 1: SendMessageW(h, WM_UNDO, 0, 0); break;
        case 2: SendMessageW(h, WM_CUT, 0, 0); break;
        case 3: SendMessageW(h, WM_COPY, 0, 0); break;
        case 4: SendMessageW(h, WM_PASTE, 0, 0); break;
        case 5: SendMessageW(h, WM_CLEAR, 0, 0); break;
        case 6: SendMessageW(h, EM_SETSEL, 0, -1); break;
        }
        return 0;
    }
    case WM_CUT: if (!(w->style & ES_READONLY)) { copy_sel(w, e); if (sel_lo(e) != sel_hi(e)) replace_sel(w, e, L"", 0, 1, 1); } return 0;
    case WM_COPY: copy_sel(w, e); return 0;
    case WM_PASTE: paste(w, e); return 0;
    case WM_CLEAR: if (!(w->style & ES_READONLY) && sel_lo(e) != sel_hi(e)) replace_sel(w, e, L"", 0, 1, 1); return 0;
    case WM_UNDO: case EM_UNDO: do_undo(w, e); return TRUE;

    case EM_GETSEL:
        if (wp) *(DWORD *)wp = (DWORD)sel_lo(e);
        if (lp) *(DWORD *)lp = (DWORD)sel_hi(e);
        return MAKELONG(MIN(sel_lo(e), 0xFFFF), MIN(sel_hi(e), 0xFFFF));
    case EM_SETSEL: {
        int a = (int)wp, b = (int)lp;
        if (a == -1) { set_sel(w, e, e->caret, e->caret, 0); return 0; }
        if (b == -1) b = e->len;
        if (a > b && b >= 0) { int t = a; a = b; b = t; set_sel(w, e, b, a, 1); return 0; }
        set_sel(w, e, a, b, 1);
        return 0;
    }
    case EM_REPLACESEL: {
        const WCHAR *s = (const WCHAR *)lp;
        replace_sel(w, e, s ? s : L"", wlen(s), (int)wp, 1);
        return 0;
    }
    case EM_GETLINECOUNT: return MULTI(w) ? e->nlines : 1;
    case EM_LINEINDEX: {
        int ln = (int)wp;
        if (ln == -1) ln = line_of(e, e->caret);
        if (ln < 0 || ln >= e->nlines) return -1;
        return e->ls[ln];
    }
    case EM_LINELENGTH: {
        int idx = (int)wp;
        if (idx == -1) {
            if (sel_lo(e) == sel_hi(e)) return e->le[line_of(e, e->caret)] - e->ls[line_of(e, e->caret)];
            return 0;
        }
        if (idx > e->len) return 0;
        int ln = line_of(e, idx);
        return e->le[ln] - e->ls[ln];
    }
    case EM_LINEFROMCHAR: {
        int idx = (int)wp;
        if (idx == -1) idx = e->caret;
        return line_of(e, MIN(idx, e->len));
    }
    case EM_GETLINE: return get_line(e, MULTI(w) ? (int)wp : 0, (WCHAR *)lp);
    case EM_LIMITTEXT: /* EM_SETLIMITTEXT */
        e->limit = wp ? (int)MIN(wp, 0x7FFFFFFE) : 0x7FFFFFFE;
        return 0;
    case EM_GETLIMITTEXT: return e->limit;
    case EM_SETREADONLY:
        if (wp) w->style |= ES_READONLY; else w->style &= ~ES_READONLY;
        invalidate(w, NULL, TRUE, 0);
        return TRUE;
    case EM_GETMODIFY: return e->modified;
    case EM_SETMODIFY: e->modified = wp != 0; return 0;
    case EM_CANUNDO: return e->can_undo;
    case EM_EMPTYUNDOBUFFER: e->can_undo = 0; return 0;
    case EM_SETPASSWORDCHAR:
        if (MULTI(w)) return 0;
        e->pwchar = (WCHAR)wp;
        if (wp) w->style |= ES_PASSWORD; else w->style &= ~ES_PASSWORD;
        invalidate(w, NULL, TRUE, 0);
        return 0;
    case EM_GETPASSWORDCHAR: return e->pwchar;
    case EM_GETFIRSTVISIBLELINE: return MULTI(w) ? e->top : 0;
    case EM_LINESCROLL: {
        if (!MULTI(w)) return FALSE;
        int t = e->top + (int)lp, vis = visible_lines(w, e);
        if (t > e->nlines - vis) t = e->nlines - vis;
        if (t < 0) t = 0;
        int x = e->xoff + (int)wp * 7;
        if (x < 0 || WRAP(w)) x = 0;
        if (t != e->top || x != e->xoff) { e->top = t; e->xoff = x; invalidate(w, NULL, TRUE, 0); update_scrollbars(w, e); place_caret(w, e); }
        return TRUE;
    }
    case EM_SCROLL: {
        int d = 0, vis = visible_lines(w, e);
        switch (wp) { case SB_LINEUP: d = -1; break; case SB_LINEDOWN: d = 1; break; case SB_PAGEUP: d = -vis; break; case SB_PAGEDOWN: d = vis; break; }
        int old = e->top;
        SendMessageW(h, EM_LINESCROLL, 0, d);
        return MAKELONG(e->top - old, TRUE);
    }
    case EM_SCROLLCARET: scroll_to_caret(w, e); return TRUE;
    case EM_SETMARGINS: {
        HDC dc = ed_dc(w);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        ed_release(w, dc);
        if (wp & EC_LEFTMARGIN) e->lm = LOWORD(lp) == EC_USEFONTINFO ? tm.tmAveCharWidth / 2 : LOWORD(lp);
        if (wp & EC_RIGHTMARGIN) e->rm = HIWORD(lp) == EC_USEFONTINFO ? tm.tmAveCharWidth / 2 : HIWORD(lp);
        calc_fmt(w, e);
        layout(w, e);
        invalidate(w, NULL, TRUE, 0);
        return 0;
    }
    case EM_GETMARGINS: return MAKELONG(e->lm, e->rm);
    case EM_POSFROMCHAR: {
        int idx = (int)wp;
        if (idx > e->len) return -1;
        POINT p = pos_of(w, e, idx);
        return MAKELONG(p.x, p.y);
    }
    case EM_CHARFROMPOS: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int idx = char_at(w, e, x, y);
        return MAKELONG(idx, line_of(e, idx));
    }
    case EM_SETTABSTOPS:
        if (wp >= 1 && lp) {
            int dlu = ((const int *)lp)[0];
            e->tabw = MAX(4, dlu * LOWORD(GetDialogBaseUnits()) / 4);
        } else e->tabw = 32 * LOWORD(GetDialogBaseUnits()) / 4;
        layout(w, e);
        invalidate(w, NULL, TRUE, 0);
        return TRUE;
    case EM_GETRECT: if (lp) *(RECT *)lp = e->fmt; return 0;
    case EM_SETRECT: case EM_SETRECTNP:
        if (lp) { e->fmt = *(RECT *)lp; e->fmt_set = 1; }
        else { e->fmt_set = 0; calc_fmt(w, e); }
        layout(w, e);
        if (msg == EM_SETRECT) invalidate(w, NULL, TRUE, 0);
        return 0;
    case EM_FMTLINES: return wp;
    case EM_GETHANDLE: case EM_SETHANDLE: return 0;
    case EM_SETWORDBREAKPROC: case EM_GETWORDBREAKPROC: return 0;
    case EM_GETTHUMB: return e->top;
    case EM_SETIMESTATUS: case EM_GETIMESTATUS: return 0;
    case EM_SETCUEBANNER:
        free(e->cue);
        e->cue = lp ? wstrdup((const WCHAR *)lp) : NULL;
        e->cue_focused = (int)wp;
        invalidate(w, NULL, TRUE, 0);
        return TRUE;
    case EM_GETCUEBANNER: {
        if (!e->cue || !wp) return FALSE;
        int n = MIN(wlen(e->cue), (int)lp - 1);
        memcpy((void *)wp, e->cue, 2 * (size_t)n);
        ((WCHAR *)wp)[n] = 0;
        return TRUE;
    }
    case EM_SHOWBALLOONTIP: case EM_HIDEBALLOONTIP: return FALSE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
