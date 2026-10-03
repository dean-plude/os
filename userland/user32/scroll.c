/*
 * scroll.c — scroll bars: the ones in a window's frame (WS_HSCROLL,
 * WS_VSCROLL) and the SCROLLBAR control, drawn and tracked alike
 */
#include "u32.h"

#define SBW 17

int sb_width(void) { return SBW; }
int sb_width_k(int k) { return SBW * (k > 1 ? k : 1); }

static int is_ctl(Wnd *w) { return w->cls && !wcsicmp_(w->cls->name, L"ScrollBar"); }

static ScrollBar *bar_of(Wnd *w, int bar)
{
    if (bar == SB_CTL) return is_ctl(w) ? &w->sb[0] : NULL;
    if (bar == SB_HORZ) return &w->sb[0];
    if (bar == SB_VERT) return &w->sb[1];
    return NULL;
}

/* The frame's scroll bars, in window coordinates */
void sb_nc_rects(Wnd *w, RECT *h, RECT *v, RECT *corner)
{
    RECT c = w->client;
    OffsetRect(&c, -w->rect.left, -w->rect.top);
    int sbw = sb_width_k(dpi_k(w));                         /* (at the window's DPI) */
    SetRectEmpty(h); SetRectEmpty(v); SetRectEmpty(corner);
    if (w->style & WS_VSCROLL) SetRect(v, c.right, c.top, c.right + sbw, c.bottom);
    if (w->style & WS_HSCROLL) SetRect(h, c.left, c.bottom, c.right, c.bottom + sbw);
    if ((w->style & WS_VSCROLL) && (w->style & WS_HSCROLL)) SetRect(corner, c.right, c.bottom, c.right + sbw, c.bottom + sbw);
}

static int max_pos(ScrollBar *s) { int p = s->page ? s->page - 1 : 0; return s->max - p; }
static int usable(ScrollBar *s) { return max_pos(s) > s->min && !s->disabled; }

/* Parts of a scroll bar of length @len and thickness @th */
typedef struct { int a1, a2;            /* arrow lengths */
                 int t0, t1;            /* track: from, to */
                 int th0, th1;          /* thumb: from, to (0, 0: none) */ } Geo;

static Geo geometry(ScrollBar *s, int len, int th)
{
    Geo g;
    int arrow = th;
    if (len < 2 * arrow) arrow = len / 2;
    g.a1 = g.a2 = arrow;
    g.t0 = arrow; g.t1 = len - arrow;
    g.th0 = g.th1 = 0;
    int track = g.t1 - g.t0, mn = th >= 2 * SBW ? 16 : 8;  /* (a thicker bar: 192 DPI) */
    if (!usable(s) || track < mn) return g;
    int range = s->max - s->min + 1;
    int tl = s->page ? (int)((long long)track * s->page / (range > 0 ? range : 1)) : th;
    if (tl < mn) tl = mn;
    if (tl > track) tl = track;
    int mp = max_pos(s);
    int pos = s->tracking ? s->track : s->pos;
    if (pos < s->min) pos = s->min;
    if (pos > mp) pos = mp;
    int off = mp > s->min ? (int)((long long)(track - tl) * (pos - s->min) / (mp - s->min)) : 0;
    g.th0 = g.t0 + off;
    g.th1 = g.th0 + tl;
    return g;
}

static int g_press_part = -1;        /* the part being pressed (drawing) */
static Wnd *g_press_wnd;
static int g_press_bar;

enum { P_NONE = -1, P_A1, P_PAGE1, P_THUMB, P_PAGE2, P_A2 };

void sb_draw(Wnd *w, HDC dc, int bar, const RECT *r, int vert)
{
    ScrollBar *s = bar_of(w, bar);
    if (!s) return;
    int len = vert ? r->bottom - r->top : r->right - r->left;
    int th = vert ? r->right - r->left : r->bottom - r->top;
    Geo g = geometry(s, len, th);
    int en = usable(s) && !(w->style & WS_DISABLED);
    int pressed = g_press_wnd == w && g_press_bar == bar ? g_press_part : P_NONE;
    COLORREF bg = 0xF0F0F0;
    fill_rect(dc, r, bg);
    RECT a1, a2, thumb;
    if (vert) {
        SetRect(&a1, r->left, r->top, r->right, r->top + g.a1);
        SetRect(&a2, r->left, r->bottom - g.a2, r->right, r->bottom);
        SetRect(&thumb, r->left + 1, r->top + g.th0, r->right - 1, r->top + g.th1);
    } else {
        SetRect(&a1, r->left, r->top, r->left + g.a1, r->bottom);
        SetRect(&a2, r->right - g.a2, r->top, r->right, r->bottom);
        SetRect(&thumb, r->left + g.th0, r->top + 1, r->left + g.th1, r->bottom - 1);
    }
    int a1en = en && !(s->arrows & ESB_DISABLE_LTUP), a2en = en && !(s->arrows & ESB_DISABLE_RTDN);
    if (pressed == P_A1) fill_rect(dc, &a1, 0x606060);
    if (pressed == P_A2) fill_rect(dc, &a2, 0x606060);
    draw_arrow(dc, &a1, vert ? 0 : 2, pressed == P_A1 ? 0xFFFFFF : a1en ? 0x606060 : 0xBFBFBF);
    draw_arrow(dc, &a2, vert ? 1 : 3, pressed == P_A2 ? 0xFFFFFF : a2en ? 0x606060 : 0xBFBFBF);
    if (g.th1 > g.th0 && en) fill_rect(dc, &thumb, pressed == P_THUMB ? 0x606060 : 0xCDCDCD);
}

static void redraw_bar(Wnd *w, int bar)
{
    if (bar == SB_CTL) { invalidate(w, NULL, FALSE, 0); return; }
    if (!wnd_visible(w)) return;
    HDC dc = wnd_dc(w, 0, 0);
    if (!dc) return;
    RECT hr, vr, corner;
    sb_nc_rects(w, &hr, &vr, &corner);
    if (bar == SB_VERT && (w->style & WS_VSCROLL)) sb_draw(w, dc, SB_VERT, &vr, 1);
    if (bar == SB_HORZ && (w->style & WS_HSCROLL)) sb_draw(w, dc, SB_HORZ, &hr, 0);
    release_dc(dc);
    present(top_of(w));
}

/* -----------------------------------------------------------------------
 * Scroll info
 * ----------------------------------------------------------------------- */
static void show_bar(Wnd *w, int bar, int show)
{
    DWORD bit = bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL;
    int has = (w->style & bit) != 0;
    if (has == show) return;
    if (show) w->style |= bit; else w->style &= ~bit;
    wnd_set_pos(w, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static int set_info(Wnd *w, int bar, const SCROLLINFO *si, BOOL redraw)
{
    ScrollBar *s = bar_of(w, bar);
    if (!s || !si) return 0;
    if (si->fMask & SIF_RANGE) { s->min = si->nMin; s->max = si->nMax; if (s->max < s->min) s->max = s->min; }
    if (si->fMask & SIF_PAGE) { s->page = (int)si->nPage; if (s->page < 0) s->page = 0; if (s->page > s->max - s->min + 1) s->page = s->max - s->min + 1; }
    if (si->fMask & SIF_POS) s->pos = si->nPos;
    int mp = max_pos(s);
    if (s->pos > mp) s->pos = mp;
    if (s->pos < s->min) s->pos = s->min;
    if (bar != SB_CTL && (si->fMask & (SIF_RANGE | SIF_PAGE))) {
        int need = mp > s->min;
        if (si->fMask & SIF_DISABLENOSCROLL) { if (!need) s->disabled = 1; else s->disabled = 0; show_bar(w, bar, 1); }
        else { s->disabled = 0; show_bar(w, bar, need); }
    }
    if (redraw) redraw_bar(w, bar);
    return s->pos;
}

USERAPI int SetScrollInfo(HWND h, int bar, LPCSCROLLINFO si, BOOL redraw)
{
    Wnd *w = W(h);
    if (!w) return 0;
    return set_info(w, bar, si, redraw);
}

USERAPI BOOL GetScrollInfo(HWND h, int bar, LPSCROLLINFO si)
{
    Wnd *w = W(h);
    ScrollBar *s = w ? bar_of(w, bar) : NULL;
    if (!s || !si) return FALSE;
    if (si->fMask & SIF_RANGE) { si->nMin = s->min; si->nMax = s->max; }
    if (si->fMask & SIF_PAGE) si->nPage = (UINT)s->page;
    if (si->fMask & SIF_POS) si->nPos = s->pos;
    if (si->fMask & SIF_TRACKPOS) si->nTrackPos = s->tracking ? s->track : s->pos;
    return TRUE;
}

USERAPI int SetScrollPos(HWND h, int bar, int pos, BOOL redraw)
{
    Wnd *w = W(h);
    ScrollBar *s = w ? bar_of(w, bar) : NULL;
    if (!s) return 0;
    int old = s->pos;
    SCROLLINFO si = { sizeof(si), SIF_POS, 0, 0, 0, pos, 0 };
    set_info(w, bar, &si, redraw);
    return old;
}

USERAPI int GetScrollPos(HWND h, int bar)
{
    Wnd *w = W(h);
    ScrollBar *s = w ? bar_of(w, bar) : NULL;
    return s ? s->pos : 0;
}

USERAPI BOOL SetScrollRange(HWND h, int bar, int mn, int mx, BOOL redraw)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    SCROLLINFO si = { sizeof(si), SIF_RANGE, mn, mx, 0, 0, 0 };
    set_info(w, bar, &si, redraw);
    return TRUE;
}

USERAPI BOOL GetScrollRange(HWND h, int bar, LPINT mn, LPINT mx)
{
    Wnd *w = W(h);
    ScrollBar *s = w ? bar_of(w, bar) : NULL;
    if (!s) { if (mn) *mn = 0; if (mx) *mx = 0; return FALSE; }
    if (mn) *mn = s->min;
    if (mx) *mx = s->max;
    return TRUE;
}

USERAPI BOOL ShowScrollBar(HWND h, int bar, BOOL show)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    if (bar == SB_CTL) { ShowWindow(h, show ? SW_SHOW : SW_HIDE); return TRUE; }
    if (bar == SB_BOTH) { show_bar(w, SB_HORZ, show); show_bar(w, SB_VERT, show); return TRUE; }
    show_bar(w, bar, show);
    return TRUE;
}

USERAPI BOOL EnableScrollBar(HWND h, UINT bar, UINT arrows)
{
    Wnd *w = W(h);
    if (!w) return FALSE;
    for (int b = 0; b < 2; b++) {
        if (bar != SB_BOTH && (int)bar != b && !(bar == SB_CTL && b == 0)) continue;
        ScrollBar *s = bar_of(w, bar == SB_CTL ? SB_CTL : b);
        if (!s) continue;
        s->arrows = (int)arrows;
        s->disabled = arrows == ESB_DISABLE_BOTH;
        redraw_bar(w, bar == SB_CTL ? SB_CTL : b);
    }
    return TRUE;
}

USERAPI BOOL GetScrollBarInfo(HWND h, LONG id, PSCROLLBARINFO sbi)
{
    Wnd *w = W(h);
    if (!w || !sbi) return FALSE;
    int bar = id == OBJID_VSCROLL ? SB_VERT : id == OBJID_HSCROLL ? SB_HORZ : SB_CTL;
    ScrollBar *s = bar_of(w, bar);
    if (!s) return FALSE;
    RECT r, hr, vr, corner;
    if (bar == SB_CTL) GetClientRect(h, &r);
    else { sb_nc_rects(w, &hr, &vr, &corner); r = bar == SB_VERT ? vr : hr; }
    POINT o;
    wnd_screen_origin(w, bar == SB_CTL, &o);
    OffsetRect(&r, o.x, o.y);
    sbi->rcScrollBar = r;
    int vert = bar == SB_VERT || (bar == SB_CTL && (w->style & SBS_VERT));
    int len = vert ? r.bottom - r.top : r.right - r.left, th = vert ? r.right - r.left : r.bottom - r.top;
    Geo g = geometry(s, len, th);
    sbi->dxyLineButton = g.a1;
    sbi->xyThumbTop = g.th0; sbi->xyThumbBottom = g.th1;
    memset(sbi->rgstate, 0, sizeof(sbi->rgstate));
    if (bar != SB_CTL && !(w->style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL))) sbi->rgstate[0] = STATE_SYSTEM_INVISIBLE;
    if (!usable(s)) sbi->rgstate[0] |= STATE_SYSTEM_UNAVAILABLE;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Tracking the mouse on a scroll bar
 * ----------------------------------------------------------------------- */
static void bar_rect(Wnd *w, int bar, RECT *r, int *vert)          /* screen coordinates */
{
    if (bar == SB_CTL) {
        GetClientRect(w->h, r);
        *vert = (w->style & SBS_VERT) != 0;
        POINT o; wnd_screen_origin(w, 1, &o);
        OffsetRect(r, o.x, o.y);
        return;
    }
    RECT hr, vr, corner;
    sb_nc_rects(w, &hr, &vr, &corner);
    *r = bar == SB_VERT ? vr : hr;
    *vert = bar == SB_VERT;
    POINT o; wnd_screen_origin(w, 0, &o);
    OffsetRect(r, o.x, o.y);
}

static int part_at(Wnd *w, int bar, POINT pt, Geo *gout)
{
    RECT r;
    int vert;
    bar_rect(w, bar, &r, &vert);
    ScrollBar *s = bar_of(w, bar);
    int len = vert ? r.bottom - r.top : r.right - r.left, th = vert ? r.right - r.left : r.bottom - r.top;
    Geo g = geometry(s, len, th);
    if (gout) *gout = g;
    if (!PtInRect(&r, pt)) return P_NONE;
    int p = vert ? pt.y - r.top : pt.x - r.left;
    if (p < g.a1) return P_A1;
    if (p >= len - g.a2) return P_A2;
    if (g.th1 > g.th0) {
        if (p < g.th0) return P_PAGE1;
        if (p >= g.th1) return P_PAGE2;
        return P_THUMB;
    }
    return P_NONE;
}

static void send_scroll(Wnd *w, int bar, int code, int pos)
{
    UINT msg = (bar == SB_VERT || (bar == SB_CTL && (w->style & SBS_VERT))) ? WM_VSCROLL : WM_HSCROLL;
    WPARAM wp = MAKEWPARAM(code, (WORD)pos);
    if (bar == SB_CTL) {
        Wnd *p = w->parent ? w->parent : w->owner;
        if (p) send_msg(p, msg, wp, (LPARAM)w->h);
    } else send_msg(w, msg, wp, 0);
}

static const int codes[5] = { SB_LINEUP, SB_PAGEUP, SB_THUMBTRACK, SB_PAGEDOWN, SB_LINEDOWN };

void sb_track(Wnd *w, int bar, POINT pt)
{
    HWND h = w->h;
    ScrollBar *s = bar_of(w, bar);
    if (!s || !usable(s) || (w->style & WS_DISABLED)) return;
    Geo g;
    int part = part_at(w, bar, pt, &g);
    if (part == P_NONE) return;
    if (part == P_A1 && (s->arrows & ESB_DISABLE_LTUP)) return;
    if (part == P_A2 && (s->arrows & ESB_DISABLE_RTDN)) return;
    RECT r;
    int vert;
    bar_rect(w, bar, &r, &vert);
    int grab = (vert ? pt.y - r.top : pt.x - r.left) - g.th0;
    g_press_wnd = w; g_press_bar = bar; g_press_part = part;
    s->track = s->pos;
    s->tracking = 1;
    SetCapture(h);
    redraw_bar(w, bar);
    if (part != P_THUMB) send_scroll(w, bar, codes[part], 0);
    ULONGLONG next = GetTickCount64() + 350;
    POINT cur = pt;
    for (;;) {
        MSG m;
        if (!W_quiet(h) || GetCapture() != h) break;
        if (!pump_one(&m, 0, 0, 0, PM_REMOVE, 1, 40)) {
            m.message = 0;
        }
        if (!W_quiet(h)) break;
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        if (m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST && m.hwnd == h) {
            cur.x = (short)LOWORD(m.lParam); cur.y = (short)HIWORD(m.lParam);
            ClientToScreen(h, &cur);
            if (m.message == WM_LBUTTONUP) break;
            if (m.message == WM_MOUSEMOVE && part == P_THUMB) {
                int len = vert ? r.bottom - r.top : r.right - r.left;
                int track = g.t1 - g.t0, tl = g.th1 - g.th0;
                int p = (vert ? cur.y - r.top : cur.x - r.left) - grab - g.t0;
                int mp = max_pos(s);
                int np = track > tl ? s->min + (int)((long long)p * (mp - s->min) / (track - tl)) : s->min;
                if (np < s->min) np = s->min;
                if (np > mp) np = mp;
                /* far outside: back to where it started */
                int far = vert ? (cur.x < r.left - 140 || cur.x > r.right + 140) : (cur.y < r.top - 140 || cur.y > r.bottom + 140);
                if (far) np = s->pos;
                (void)len;
                if (np != s->track) {
                    s->track = np;
                    send_scroll(w, bar, SB_THUMBTRACK, np);
                    if (!W_quiet(h)) break;
                    redraw_bar(w, bar);
                }
            }
            continue;
        }
        if (m.message && m.message != WM_TIMER) {
            if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST) {
                if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) break;
                continue;
            }
            DispatchMessageW(&m);
        }
        if (part != P_THUMB && GetTickCount64() >= next) {
            Geo g2;
            int now = part_at(w, bar, cur, &g2);
            if (now == part) send_scroll(w, bar, codes[part], 0);
            next = GetTickCount64() + 50;
            if (!W_quiet(h)) break;
        }
    }
    if (W_quiet(h)) {
        if (part == P_THUMB) send_scroll(w, bar, SB_THUMBPOSITION, s->track);
        if (W_quiet(h)) send_scroll(w, bar, SB_ENDSCROLL, 0);
        s = bar_of(w, bar);
        if (s && W_quiet(h)) s->tracking = 0;
    }
    g_press_wnd = NULL; g_press_part = P_NONE;
    if (GetCapture() == h) ReleaseCapture();
    if (W_quiet(h)) redraw_bar(w, bar);
}

/* -----------------------------------------------------------------------
 * The SCROLLBAR control
 * ----------------------------------------------------------------------- */
LRESULT CALLBACK ScrollBarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    ScrollBar *s = &w->sb[0];
    switch (msg) {
    case WM_NCCREATE:
        s->min = 0; s->max = 100; s->pos = 0; s->page = 0;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
        if (dc) {
            if (w->style & (SBS_SIZEGRIP | SBS_SIZEBOX)) DrawFrameControl(dc, &r, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
            else sb_draw(w, dc, SB_CTL, &r, (w->style & SBS_VERT) != 0);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (w->style & (SBS_SIZEGRIP | SBS_SIZEBOX)) return 0;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ClientToScreen(h, &pt);
        if (w->style & WS_TABSTOP) set_focus(h);
        sb_track(w, SB_CTL, pt);
        return 0;
    }
    case WM_KEYDOWN: {
        int code = -1;
        switch (wp) {
        case VK_UP: case VK_LEFT: code = SB_LINEUP; break;
        case VK_DOWN: case VK_RIGHT: code = SB_LINEDOWN; break;
        case VK_PRIOR: code = SB_PAGEUP; break;
        case VK_NEXT: code = SB_PAGEDOWN; break;
        case VK_HOME: code = SB_TOP; break;
        case VK_END: code = SB_BOTTOM; break;
        }
        if (code >= 0) { send_scroll(w, SB_CTL, code, 0); send_scroll(w, SB_CTL, SB_ENDSCROLL, 0); }
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_ENABLE: invalidate(w, NULL, FALSE, 0); return 0;
    case SBM_SETPOS: { int old = s->pos; SCROLLINFO si = { sizeof(si), SIF_POS, 0, 0, 0, (int)wp, 0 }; set_info(w, SB_CTL, &si, (BOOL)lp); return old; }
    case SBM_GETPOS: return s->pos;
    case SBM_SETRANGE: case SBM_SETRANGEREDRAW: {
        int old = s->pos;
        SCROLLINFO si = { sizeof(si), SIF_RANGE, (int)wp, (int)lp, 0, 0, 0 };
        set_info(w, SB_CTL, &si, msg == SBM_SETRANGEREDRAW);
        return old;
    }
    case SBM_GETRANGE: if (wp) *(int *)wp = s->min; if (lp) *(int *)lp = s->max; return 0;
    case SBM_ENABLE_ARROWS: s->arrows = (int)wp; invalidate(w, NULL, FALSE, 0); return TRUE;
    case SBM_SETSCROLLINFO: return set_info(w, SB_CTL, (const SCROLLINFO *)lp, (BOOL)wp);
    case SBM_GETSCROLLINFO: return GetScrollInfo(h, SB_CTL, (SCROLLINFO *)lp);
    case SBM_GETSCROLLBARINFO: return GetScrollBarInfo(h, OBJID_CLIENT, (PSCROLLBARINFO)lp);
    }
    return DefWindowProcW(h, msg, wp, lp);
}
