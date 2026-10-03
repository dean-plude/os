/*
 * simple.c — the small common controls: progress bar, status bar,
 * tooltips, up-down, trackbar, tab control, SysLink
 */
#include "cc.h"

static HFONT wfont(HWND h, HFONT f) { (void)h; return f ? f : cc_font_for(h); }

/* =======================================================================
 * Progress bar
 * ======================================================================= */
typedef struct { int lo, hi, pos, step, marquee, mpos, state; COLORREF bar, bk; } Prog;

static void prog_paint(HWND h, Prog *p, HDC dc)
{
    RECT r;
    GetClientRect(h, &r);
    DWORD st = (DWORD)GetWindowLongW(h, GWL_STYLE);
    cc_fill(dc, &r, p->bk != CLR_DEFAULT ? p->bk : 0xE6E6E6);
    if (!(st & WS_BORDER) && !(GetWindowLongW(h, GWL_EXSTYLE) & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE))) cc_frame(dc, &r, 0xBCBCBC);
    RECT in = r;
    InflateRect(&in, -cc_k(h), -cc_k(h));
    COLORREF bar = p->bar != CLR_DEFAULT ? p->bar : p->state == PBST_ERROR ? 0x2B26E6 : p->state == PBST_PAUSED ? 0x00B6DA : 0x25B006;
    int vert = (st & PBS_VERTICAL) != 0;
    int len = vert ? in.bottom - in.top : in.right - in.left;
    RECT f = in;
    if ((st & PBS_MARQUEE) && p->marquee) {
        int seg = len / 4;
        int a = p->mpos % (len + seg) - seg;
        if (vert) { f.bottom = in.bottom - a; f.top = f.bottom - seg; }
        else { f.left = in.left + a; f.right = f.left + seg; }
        IntersectRect(&f, &f, &in);
    } else {
        long long range = (long long)p->hi - p->lo;
        int fill = range > 0 ? (int)((long long)(p->pos - p->lo) * len / range) : 0;
        if (fill < 0) fill = 0;
        if (fill > len) fill = len;
        if (vert) f.top = in.bottom - fill; else f.right = in.left + fill;
    }
    if (!IsRectEmpty(&f)) cc_fill(dc, &f, bar);
}

static int prog_set(HWND h, Prog *p, int pos)
{
    int old = p->pos;
    if (pos < p->lo) pos = p->lo;
    if (pos > p->hi) pos = p->hi;
    if (pos != p->pos) { p->pos = pos; InvalidateRect(h, NULL, FALSE); }
    return old;
}

LRESULT CALLBACK ProgressProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Prog *p = ctl_get(h);
    if (!p && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        p = calloc(1, sizeof(Prog));
        if (!p) return FALSE;
        p->hi = 100; p->step = 10; p->state = PBST_NORMAL; p->bar = p->bk = CLR_DEFAULT;
        ctl_set(h, p);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NCDESTROY: free(p); ctl_set(h, NULL); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); prog_paint(h, p, dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: prog_paint(h, p, (HDC)wp); return 0;
    case WM_TIMER: p->mpos += 4; InvalidateRect(h, NULL, FALSE); return 0;
    case PBM_SETRANGE: { LRESULT o = MAKELONG(p->lo, p->hi); p->lo = LOWORD(lp); p->hi = HIWORD(lp); if (p->hi <= p->lo) p->hi = p->lo + 1; prog_set(h, p, p->pos); InvalidateRect(h, NULL, FALSE); return o; }
    case PBM_SETRANGE32: { LRESULT o = MAKELONG(p->lo, p->hi); p->lo = (int)wp; p->hi = (int)lp; if (p->hi <= p->lo) p->hi = p->lo + 1; prog_set(h, p, p->pos); InvalidateRect(h, NULL, FALSE); return o; }
    case PBM_GETRANGE: { PBRANGE *r = (PBRANGE *)lp; if (r) { r->iLow = p->lo; r->iHigh = p->hi; } return wp ? p->lo : p->hi; }
    case PBM_SETPOS: return prog_set(h, p, (int)wp);
    case PBM_DELTAPOS: return prog_set(h, p, p->pos + (int)wp);
    case PBM_SETSTEP: { int o = p->step; p->step = (int)wp; return o; }
    case PBM_GETSTEP: return p->step;
    case PBM_STEPIT: { int o = p->pos; int n = p->pos + p->step; if (n > p->hi) n = p->lo + (n - p->hi); prog_set(h, p, n); return o; }
    case PBM_GETPOS: return p->pos;
    case PBM_SETBARCOLOR: { COLORREF o = p->bar; p->bar = (COLORREF)lp; InvalidateRect(h, NULL, FALSE); return o; }
    case PBM_SETBKCOLOR: { COLORREF o = p->bk; p->bk = (COLORREF)lp; InvalidateRect(h, NULL, FALSE); return o; }
    case PBM_GETBARCOLOR: return p->bar;
    case PBM_GETBKCOLOR: return p->bk;
    case PBM_SETMARQUEE:
        p->marquee = (int)wp;
        if (wp) SetTimer(h, 1, lp ? (UINT)lp : 30, NULL); else KillTimer(h, 1);
        InvalidateRect(h, NULL, FALSE);
        return TRUE;
    case PBM_SETSTATE: { int o = p->state; p->state = (int)wp; InvalidateRect(h, NULL, FALSE); return o; }
    case PBM_GETSTATE: return p->state;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* =======================================================================
 * Status bar
 * ======================================================================= */
typedef struct { int n; int right[256]; WCHAR *text[256]; UINT flags[256]; HICON icon[256]; int simple; WCHAR *stext; HFONT font; int minh; } Stat;

static int stat_height(HWND h, Stat *s) { int fh = cc_font_h(s->font ? s->font : cc_font_for(h)); return MAX(fh + 8 * cc_k(h), s->minh); }

static void stat_layout(HWND h, Stat *s)
{
    HWND p = GetParent(h);
    DWORD st = (DWORD)GetWindowLongW(h, GWL_STYLE);
    if (!p || (st & CCS_NORESIZE)) return;
    RECT pr;
    GetClientRect(p, &pr);
    int sh = stat_height(h, s);
    if (st & CCS_TOP) SetWindowPos(h, 0, 0, 0, pr.right, sh, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(h, 0, 0, pr.bottom - sh, pr.right, sh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void stat_part_rect(HWND h, Stat *s, int i, RECT *r)
{
    RECT c;
    GetClientRect(h, &c);
    int left = i ? s->right[i - 1] : 0;
    int right = s->right[i] < 0 || s->right[i] > c.right ? c.right : s->right[i];
    SetRect(r, left, c.top + 2 * cc_k(h), right, c.bottom);
}

static void stat_paint(HWND h, Stat *s, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    int k = cc_k(h);
    cc_fill(dc, &c, 0xF0F0F0);
    RECT top = { c.left, c.top, c.right, c.top + k };
    cc_fill(dc, &top, 0xDADADA);
    HGDIOBJ of = SelectObject(dc, wfont(h, s->font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    if (s->simple) {
        RECT r = c;
        r.left += 6 * k;
        DrawTextW(dc, s->stext ? s->stext : L"", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        for (int i = 0; i < s->n; i++) {
            RECT r;
            stat_part_rect(h, s, i, &r);
            if (i + 1 < s->n && !(s->flags[i] & SBT_NOBORDERS)) { RECT sep = { r.right - k, r.top + 3 * k, r.right, r.bottom - 3 * k }; cc_fill(dc, &sep, 0xC8C8C8); }
            RECT t = r;
            t.left += 6 * k;
            t.right -= 4 * k;
            if (s->icon[i]) { DrawIconEx(dc, t.left, r.top + (r.bottom - r.top - 16 * k) / 2, s->icon[i], 16 * k, 16 * k, 0, NULL, DI_NORMAL); t.left += 20 * k; }
            if (s->flags[i] & SBT_OWNERDRAW) {
                DRAWITEMSTRUCT di;
                memset(&di, 0, sizeof(di));
                di.CtlID = (UINT)GetDlgCtrlID(h); di.itemID = (UINT)i; di.hwndItem = h; di.hDC = dc; di.rcItem = r; di.itemData = (ULONG_PTR)s->text[i];
                SendMessageW(GetParent(h), WM_DRAWITEM, di.CtlID, (LPARAM)&di);
                continue;
            }
            const WCHAR *txt = s->text[i] ? s->text[i] : L"";
            /* tabs: centre and right parts */
            const WCHAR *t1 = txt, *t2 = NULL, *t3 = NULL;
            for (const WCHAR *q = txt; *q; q++) if (*q == '\t') { if (!t2) t2 = q + 1; else if (!t3) { t3 = q + 1; break; } }
            int n1 = t2 ? (int)(t2 - t1 - 1) : wlen(t1);
            DrawTextW(dc, t1, n1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (t2) { int n2 = t3 ? (int)(t3 - t2 - 1) : wlen(t2); DrawTextW(dc, t2, n2, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX); }
            if (t3) DrawTextW(dc, t3, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
    }
    if (GetWindowLongW(h, GWL_STYLE) & SBARS_SIZEGRIP) {
        RECT g = { c.right - 14 * k, c.bottom - 14 * k, c.right, c.bottom };
        DrawFrameControl(dc, &g, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
    }
    SelectObject(dc, of);
}

static void set_str(WCHAR **slot, const WCHAR *s, int wide)
{
    free(*slot);
    if (!s) { *slot = NULL; return; }
    if (wide) *slot = wdup(s);
    else {
        int n = MultiByteToWideChar(CP_ACP, 0, (const char *)s, -1, NULL, 0);
        *slot = malloc(2 * (size_t)(n + 1));
        if (*slot) MultiByteToWideChar(CP_ACP, 0, (const char *)s, -1, *slot, n);
    }
}

LRESULT CALLBACK StatusProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Stat *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE: {
        s = calloc(1, sizeof(Stat));
        if (!s) return FALSE;
        s->n = 1; s->right[0] = -1;
        ctl_set(h, s);
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        if (cs && cs->lpszName && (ULONG_PTR)cs->lpszName > 0xFFFF) set_str(&s->text[0], cs->lpszName, 1);
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_CREATE: stat_layout(h, s); return 0;
    case WM_NCDESTROY:
        for (int i = 0; i < 256; i++) free(s->text[i]);
        free(s->stext);
        free(s);
        ctl_set(h, NULL);
        return 0;
    case WM_SIZE: stat_layout(h, s); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); stat_paint(h, s, dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: stat_paint(h, s, (HDC)wp); return 0;
    case WM_SETFONT: s->font = (HFONT)wp; if (lp) InvalidateRect(h, NULL, TRUE); return 0;
    case WM_DPICHANGED_AFTERPARENT: stat_layout(h, s); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_SETTEXT: set_str(&s->text[0], (const WCHAR *)lp, 1); InvalidateRect(h, NULL, FALSE); return TRUE;
    case WM_GETTEXT: { int n = MIN(wlen(s->text[0]), (int)wp - 1); if (n < 0) return 0; memcpy((void *)lp, s->text[0] ? s->text[0] : L"", 2 * (size_t)n); ((WCHAR *)lp)[n] = 0; return n; }
    case WM_GETTEXTLENGTH: return wlen(s->text[0]);
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (r == HTCLIENT && (GetWindowLongW(h, GWL_STYLE) & SBARS_SIZEGRIP)) {
            RECT c; GetClientRect(h, &c);
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(h, &pt);
            if (pt.x >= c.right - 14 * cc_k(h) && pt.y >= c.bottom - 14 * cc_k(h)) return HTBOTTOMRIGHT;
        }
        return r;
    }
    case WM_LBUTTONUP: cc_notify(h, NM_CLICK, NULL); return 0;
    case WM_LBUTTONDBLCLK: cc_notify(h, NM_DBLCLK, NULL); return 0;
    case WM_RBUTTONUP: cc_notify(h, NM_RCLICK, NULL); return 0;
    case SB_SETPARTS: {
        int n = MIN((int)wp, 256);
        if (n < 1) return FALSE;
        for (int i = 0; i < n; i++) s->right[i] = ((const int *)lp)[i];
        s->n = n;
        InvalidateRect(h, NULL, FALSE);
        return TRUE;
    }
    case SB_GETPARTS: {
        int n = MIN((int)wp, s->n);
        for (int i = 0; lp && i < n; i++) ((int *)lp)[i] = s->right[i];
        return s->n;
    }
    case SB_SETTEXTW: case SB_SETTEXTA: {
        int i = (int)(wp & 0xFF);
        if (i == SB_SIMPLEID) { set_str(&s->stext, (const WCHAR *)lp, msg == SB_SETTEXTW); InvalidateRect(h, NULL, FALSE); return TRUE; }
        if (i >= 256) return FALSE;
        s->flags[i] = (UINT)(wp & 0xFF00);
        if (s->flags[i] & SBT_OWNERDRAW) { free(s->text[i]); s->text[i] = (WCHAR *)lp; }
        else set_str(&s->text[i], (const WCHAR *)lp, msg == SB_SETTEXTW);
        RECT r;
        stat_part_rect(h, s, MIN(i, s->n - 1), &r);
        InvalidateRect(h, NULL, FALSE);
        return TRUE;
    }
    case SB_GETTEXTW: case SB_GETTEXTA: {
        int i = (int)(wp & 0xFF);
        if (i >= 256) return 0;
        const WCHAR *t = s->text[i] ? s->text[i] : L"";
        int n = wlen(t);
        if (lp) {
            if (msg == SB_GETTEXTW) { memcpy((void *)lp, t, 2 * ((size_t)n + 1)); }
            else n = WideCharToMultiByte(CP_ACP, 0, t, -1, (char *)lp, 1024, NULL, NULL) - 1;
        }
        return MAKELONG(n, s->flags[i]);
    }
    case SB_GETTEXTLENGTHW: case SB_GETTEXTLENGTHA: { int i = (int)(wp & 0xFF); return i < 256 ? MAKELONG(wlen(s->text[i]), s->flags[i]) : 0; }
    case SB_SETICON: { int i = (int)wp; if (i < 0 || i >= 256) return FALSE; s->icon[i] = (HICON)lp; InvalidateRect(h, NULL, FALSE); return TRUE; }
    case SB_GETICON: { int i = (int)wp; return i >= 0 && i < 256 ? (LRESULT)s->icon[i] : 0; }
    case SB_SIMPLE: s->simple = wp != 0; InvalidateRect(h, NULL, FALSE); return 0;
    case SB_ISSIMPLE: return s->simple;
    case SB_GETRECT: { int i = (int)wp; if (i < 0 || i >= s->n) return FALSE; stat_part_rect(h, s, i, (RECT *)lp); return TRUE; }
    case SB_GETBORDERS: { int *b = (int *)lp; if (b) { b[0] = 0; b[1] = 2 * cc_k(h); b[2] = 2 * cc_k(h); } return TRUE; }
    case SB_SETMINHEIGHT: s->minh = (int)wp; return 0;
    case SB_SETTIPTEXTW: case SB_GETTIPTEXTW: return 0;
    case SB_SETBKCOLOR: return CLR_DEFAULT;
    case WM_USER + 0x7F00:                                  /* the parent resized */
        stat_layout(h, s);
        return 0;
    }
    if (msg == WM_SIZE) {}
    return DefWindowProcW(h, msg, wp, lp);
}

/* =======================================================================
 * Tooltips: they keep their tools but never pop up
 * ======================================================================= */
LRESULT CALLBACK TooltipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case TTM_ADDTOOLW: case (WM_USER + 4) /* TTM_ADDTOOLA */: return TRUE;
    case TTM_ACTIVATE: case TTM_DELTOOLW: case TTM_NEWTOOLRECTW: case TTM_RELAYEVENT: case TTM_SETDELAYTIME:
    case TTM_UPDATETIPTEXTW: case TTM_SETTOOLINFOW: case TTM_TRACKACTIVATE: case TTM_TRACKPOSITION: case TTM_POP:
        return 0;
    case TTM_SETMAXTIPWIDTH: return 300;
    case TTM_GETTOOLINFOW: return FALSE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* =======================================================================
 * Up-down (spin) control
 * ======================================================================= */
typedef struct { int lo, hi, pos, base; HWND buddy; int pressed; } UD;

static void ud_update_buddy(HWND h, UD *u)
{
    if (!u->buddy || !(GetWindowLongW(h, GWL_STYLE) & UDS_SETBUDDYINT)) return;
    WCHAR buf[32];
    if (u->base == 16) wsprintfW(buf, L"0x%X", u->pos);
    else wsprintfW(buf, L"%d", u->pos);
    SetWindowTextW(u->buddy, buf);
}

static void ud_attach(HWND h, UD *u)
{
    DWORD st = (DWORD)GetWindowLongW(h, GWL_STYLE);
    if (!u->buddy || !(st & (UDS_ALIGNRIGHT | UDS_ALIGNLEFT))) return;
    RECT br;
    GetWindowRect(u->buddy, &br);
    MapWindowPoints(NULL, GetParent(h), (POINT *)&br, 2);
    int w = 17 * cc_k(h);
    if (st & UDS_ALIGNRIGHT) {
        SetWindowPos(u->buddy, 0, 0, 0, br.right - br.left - w, br.bottom - br.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h, 0, br.right - w, br.top, w, br.bottom - br.top, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(u->buddy, 0, br.left + w, br.top, br.right - br.left - w, br.bottom - br.top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h, 0, br.left, br.top, w, br.bottom - br.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void ud_step(HWND h, UD *u, int delta)
{
    DWORD st = (DWORD)GetWindowLongW(h, GWL_STYLE);
    if (u->lo > u->hi) delta = -delta;
    NMUPDOWN nm;
    nm.iPos = u->pos; nm.iDelta = delta;
    if (cc_notify(h, UDN_DELTAPOS, &nm.hdr)) return;
    int mn = MIN(u->lo, u->hi), mx = MAX(u->lo, u->hi);
    int np = u->pos + nm.iDelta;
    if (np > mx) np = (st & UDS_WRAP) ? mn : mx;
    if (np < mn) np = (st & UDS_WRAP) ? mx : mn;
    if (np == u->pos) return;
    u->pos = np;
    ud_update_buddy(h, u);
    SendMessageW(GetParent(h), (st & UDS_HORZ) ? WM_HSCROLL : WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, (WORD)np), (LPARAM)h);
}

LRESULT CALLBACK UpDownProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    UD *u = ctl_get(h);
    if (!u && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        u = calloc(1, sizeof(UD));
        if (!u) return FALSE;
        u->lo = 100; u->hi = 0; u->base = 10;
        ctl_set(h, u);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE:
        if (GetWindowLongW(h, GWL_STYLE) & UDS_AUTOBUDDY) { u->buddy = GetWindow(h, GW_HWNDPREV); ud_attach(h, u); }
        return 0;
    case WM_NCDESTROY: free(u); ctl_set(h, NULL); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c; GetClientRect(h, &c);
        int horz = (GetWindowLongW(h, GWL_STYLE) & UDS_HORZ) != 0;
        RECT a = c, b = c;
        if (horz) { a.right = (c.left + c.right) / 2; b.left = a.right; }
        else { a.bottom = (c.top + c.bottom) / 2; b.top = a.bottom; }
        cc_fill(dc, &a, u->pressed == 1 ? 0xF7E4CC : 0xE1E1E1); cc_frame(dc, &a, 0xADADAD);
        cc_fill(dc, &b, u->pressed == 2 ? 0xF7E4CC : 0xE1E1E1); cc_frame(dc, &b, 0xADADAD);
        cc_arrow(dc, &a, horz ? 2 : 0, 0x404040);
        cc_arrow(dc, &b, horz ? 3 : 1, 0x404040);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        RECT c; GetClientRect(h, &c);
        int horz = (GetWindowLongW(h, GWL_STYLE) & UDS_HORZ) != 0;
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int first = horz ? x < (c.left + c.right) / 2 : y < (c.top + c.bottom) / 2;
        u->pressed = first ? 1 : 2;
        InvalidateRect(h, NULL, FALSE);
        ud_step(h, u, (first ^ horz) ? 1 : -1);
        SetCapture(h);
        SetTimer(h, 1, 400, NULL);
        return 0;
    }
    case WM_TIMER:
        if (u->pressed) {
            int horz = (GetWindowLongW(h, GWL_STYLE) & UDS_HORZ) != 0;
            ud_step(h, u, ((u->pressed == 1) ^ horz) ? 1 : -1);
            SetTimer(h, 1, 60, NULL);
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (u->pressed) {
            u->pressed = 0;
            KillTimer(h, 1);
            if (GetCapture() == h) ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            SendMessageW(GetParent(h), (GetWindowLongW(h, GWL_STYLE) & UDS_HORZ) ? WM_HSCROLL : WM_VSCROLL, MAKEWPARAM(SB_ENDSCROLL, (WORD)u->pos), (LPARAM)h);
        }
        return 0;
    case UDM_SETRANGE: u->hi = (short)LOWORD(lp); u->lo = (short)HIWORD(lp); return 0;
    case UDM_GETRANGE: return MAKELONG(u->hi, u->lo);
    case UDM_SETRANGE32: u->lo = (int)wp; u->hi = (int)lp; return 0;
    case UDM_GETRANGE32: if (wp) *(int *)wp = u->lo; if (lp) *(int *)lp = u->hi; return 0;
    case UDM_SETPOS: case UDM_SETPOS32: { int o = u->pos; u->pos = msg == UDM_SETPOS ? (short)LOWORD(lp) : (int)lp; ud_update_buddy(h, u); return o; }
    case UDM_GETPOS: case UDM_GETPOS32: {
        if (u->buddy && (GetWindowLongW(h, GWL_STYLE) & UDS_SETBUDDYINT)) {
            WCHAR buf[32];
            GetWindowTextW(u->buddy, buf, 32);
            int v = 0, neg = 0, i = 0;
            if (buf[0] == '-') { neg = 1; i = 1; }
            for (; buf[i]; i++) if (buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i] - '0');
            u->pos = neg ? -v : v;
        }
        if (msg == UDM_GETPOS32) { if (lp) *(BOOL *)lp = FALSE; return u->pos; }
        return MAKELONG(u->pos, 0);
    }
    case UDM_SETBUDDY: { HWND o = u->buddy; u->buddy = (HWND)wp; ud_attach(h, u); return (LRESULT)o; }
    case UDM_GETBUDDY: return (LRESULT)u->buddy;
    case UDM_SETBASE: { int o = u->base; u->base = (int)wp; return o; }
    case UDM_GETBASE: return u->base;
    case UDM_SETACCEL: return TRUE;
    case UDM_GETACCEL: return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* =======================================================================
 * Trackbar
 * ======================================================================= */
typedef struct { int lo, hi, pos, page, line, drag, focus; } TB;

static void tb_geo(HWND h, TB *t, RECT *chan, RECT *thumb)
{
    RECT c; GetClientRect(h, &c);
    int vert = (GetWindowLongW(h, GWL_STYLE) & TBS_VERT) != 0, k = cc_k(h);
    if (vert) {
        int x = (c.left + c.right) / 2;
        SetRect(chan, x - 2 * k, c.top + 8 * k, x + 2 * k, c.bottom - 8 * k);
        int len = chan->bottom - chan->top;
        int y = t->hi > t->lo ? chan->top + (int)((long long)(t->pos - t->lo) * len / (t->hi - t->lo)) : chan->top;
        SetRect(thumb, x - 10 * k, y - 5 * k, x + 10 * k, y + 5 * k);
    } else {
        int y = (c.top + c.bottom) / 2;
        SetRect(chan, c.left + 8 * k, y - 2 * k, c.right - 8 * k, y + 2 * k);
        int len = chan->right - chan->left;
        int x = t->hi > t->lo ? chan->left + (int)((long long)(t->pos - t->lo) * len / (t->hi - t->lo)) : chan->left;
        SetRect(thumb, x - 5 * k, y - 10 * k, x + 5 * k, y + 10 * k);
    }
}

static void tb_set(HWND h, TB *t, int pos, int code)
{
    if (pos < t->lo) pos = t->lo;
    if (pos > t->hi) pos = t->hi;
    if (pos == t->pos && code != TB_ENDTRACK) return;
    t->pos = pos;
    InvalidateRect(h, NULL, TRUE);
    if (code >= 0)
        SendMessageW(GetParent(h), (GetWindowLongW(h, GWL_STYLE) & TBS_VERT) ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(code, (WORD)pos), (LPARAM)h);
}

LRESULT CALLBACK TrackbarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    TB *t = ctl_get(h);
    if (!t && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        t = calloc(1, sizeof(TB));
        if (!t) return FALSE;
        t->hi = 100; t->page = 20; t->line = 1;
        ctl_set(h, t);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NCDESTROY: free(t); ctl_set(h, NULL); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c; GetClientRect(h, &c);
        HBRUSH bg = (HBRUSH)SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h);
        if (bg) FillRect(dc, &c, bg); else cc_fill(dc, &c, GetSysColor(COLOR_3DFACE));
        RECT chan, thumb;
        tb_geo(h, t, &chan, &thumb);
        cc_fill(dc, &chan, 0xE7EAEA); cc_frame(dc, &chan, 0xD6D6D6);
        cc_fill(dc, &thumb, t->drag ? 0xCCCCCC : IsWindowEnabled(h) ? 0xD77800 : 0xCCCCCC);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_SETFOCUS: case WM_KILLFOCUS: t->focus = msg == WM_SETFOCUS; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        RECT chan, thumb;
        tb_geo(h, t, &chan, &thumb);
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (PtInRect(&thumb, pt)) { t->drag = 1; SetCapture(h); InvalidateRect(h, NULL, FALSE); return 0; }
        int vert = (GetWindowLongW(h, GWL_STYLE) & TBS_VERT) != 0;
        int before = vert ? pt.y < thumb.top : pt.x < thumb.left;
        tb_set(h, t, t->pos + (before ? -t->page : t->page), before ? TB_PAGEUP : TB_PAGEDOWN);
        tb_set(h, t, t->pos, TB_ENDTRACK);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (t->drag) {
            RECT chan, thumb;
            tb_geo(h, t, &chan, &thumb);
            int vert = (GetWindowLongW(h, GWL_STYLE) & TBS_VERT) != 0;
            int p = vert ? (short)HIWORD(lp) - chan.top : (short)LOWORD(lp) - chan.left;
            int len = vert ? chan.bottom - chan.top : chan.right - chan.left;
            if (len > 0) tb_set(h, t, t->lo + (int)((long long)p * (t->hi - t->lo) / len), TB_THUMBTRACK);
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (t->drag) {
            t->drag = 0;
            if (GetCapture() == h) ReleaseCapture();
            tb_set(h, t, t->pos, TB_THUMBPOSITION);
            tb_set(h, t, t->pos, TB_ENDTRACK);
        }
        return 0;
    case WM_KEYDOWN: {
        int d = 0;
        switch (wp) {
        case VK_LEFT: case VK_UP: d = -t->line; break;
        case VK_RIGHT: case VK_DOWN: d = t->line; break;
        case VK_PRIOR: d = -t->page; break;
        case VK_NEXT: d = t->page; break;
        case VK_HOME: tb_set(h, t, t->lo, TB_TOP); tb_set(h, t, t->pos, TB_ENDTRACK); return 0;
        case VK_END: tb_set(h, t, t->hi, TB_BOTTOM); tb_set(h, t, t->pos, TB_ENDTRACK); return 0;
        }
        if (d) { tb_set(h, t, t->pos + d, d < 0 ? TB_LINEUP : TB_LINEDOWN); tb_set(h, t, t->pos, TB_ENDTRACK); }
        return 0;
    }
    case TBM_GETPOS: return t->pos;
    case TBM_SETPOS: tb_set(h, t, (int)lp, -1); return 0;
    case TBM_GETRANGEMIN: return t->lo;
    case TBM_GETRANGEMAX: return t->hi;
    case TBM_SETRANGE: t->lo = (short)LOWORD(lp); t->hi = (short)HIWORD(lp); tb_set(h, t, t->pos, -1); InvalidateRect(h, NULL, TRUE); return 0;
    case TBM_SETRANGEMIN: t->lo = (int)lp; InvalidateRect(h, NULL, TRUE); return 0;
    case TBM_SETRANGEMAX: t->hi = (int)lp; InvalidateRect(h, NULL, TRUE); return 0;
    case TBM_SETPAGESIZE: { int o = t->page; t->page = (int)lp; return o; }
    case TBM_GETPAGESIZE: return t->page;
    case TBM_SETLINESIZE: { int o = t->line; t->line = (int)lp; return o; }
    case TBM_GETLINESIZE: return t->line;
    case TBM_SETTIC: case TBM_CLEARTICS: case TBM_SETSEL: case TBM_SETTICFREQ: return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* =======================================================================
 * Tab control
 * ======================================================================= */
typedef struct { int n; WCHAR *text[64]; LPARAM param[64]; int image[64]; int cur, focus; HFONT font; HIMAGELIST il; int fixw, fixh, padx, pady, pad_set, minw; } Tab;

/* The image list's icon size (0 without one) */
/* TCM_SETPADDING's, else 6 x 3 at 96 DPI */
static int tab_padx(HWND h, Tab *t) { return t->pad_set ? t->padx : 6 * cc_k(h); }
static int tab_pady(HWND h, Tab *t) { return t->pad_set ? t->pady : 3 * cc_k(h); }

static void tab_icon(Tab *t, int *cx, int *cy)
{
    *cx = *cy = 0;
    if (t->il) ImageList_GetIconSize(t->il, cx, cy);
}

static int tab_row_h(HWND h, Tab *t)
{
    if (t->fixh) return t->fixh;
    int ix, iy, fh = cc_font_h(t->font ? t->font : cc_font_for(h));
    tab_icon(t, &ix, &iy);
    return (iy > fh ? iy : fh) + 2 * tab_pady(h, t) + 4 * cc_k(h);
}

static void tab_rect(HWND h, Tab *t, int i, RECT *r)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, wfont(h, t->font));
    int ck = cc_k(h), px = tab_padx(h, t);
    int x = 2 * ck, fixed = (GetWindowLongW(h, GWL_STYLE) & TCS_FIXEDWIDTH) && t->fixw;   /* TCM_SETITEMSIZE's width */
    for (int k = 0; k <= i && k < t->n; k++) {
        int ix, iy;                     /* text, the image list's icon width and padding, padding on both sides */
        tab_icon(t, &ix, &iy);
        int w = fixed ? t->fixw : cc_text_w(dc, t->text[k] ? t->text[k] : L"", -1) + 2 * px +
                                    (ix ? ix + px : 0);
        if (!fixed && w < t->minw) w = t->minw;
        if (k == i) { SetRect(r, x, 2 * ck, x + w, 2 * ck + tab_row_h(h, t)); break; }
        x += w;
    }
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);
}

/* The tabs and the body below them; TCS_OWNERDRAWFIXED: the parent draws
 * each tab (WM_DRAWITEM, ODT_TAB) */
static void tab_paint(HWND h, Tab *t, HDC dc)
{
    RECT c; GetClientRect(h, &c);
    HBRUSH bg = (HBRUSH)SendMessageW(GetParent(h), WM_CTLCOLORDLG, (WPARAM)dc, (LPARAM)h);
    if (bg) FillRect(dc, &c, bg); else cc_fill(dc, &c, GetSysColor(COLOR_3DFACE));
    int rh = tab_row_h(h, t), k = cc_k(h);
    RECT body = { c.left, c.top + rh + k, c.right, c.bottom };
    cc_fill(dc, &body, 0xFFFFFF);
    cc_frame(dc, &body, 0xD9D9D9);
    HGDIOBJ of = SelectObject(dc, wfont(h, t->font));
    SetBkMode(dc, TRANSPARENT);
    int owner = (GetWindowLongW(h, GWL_STYLE) & TCS_OWNERDRAWFIXED) != 0;
    for (int i = 0; i < t->n; i++) {
        RECT r;
        tab_rect(h, t, i, &r);
        int sel = i == t->cur;
        if (owner) {
            DRAWITEMSTRUCT di;
            memset(&di, 0, sizeof(di));
            di.CtlType = ODT_TAB; di.CtlID = (UINT)GetDlgCtrlID(h); di.itemID = (UINT)i;
            di.itemAction = ODA_DRAWENTIRE; di.itemState = sel ? ODS_SELECTED : 0;
            di.hwndItem = h; di.hDC = dc; di.rcItem = r; di.itemData = (ULONG_PTR)t->param[i];
            SendMessageW(GetParent(h), WM_DRAWITEM, di.CtlID, (LPARAM)&di);
            continue;
        }
        if (sel) { r.top -= 2 * k; r.bottom += k; }
        cc_fill(dc, &r, sel ? 0xFFFFFF : 0xF0F0F0);
        RECT fr = r;
        if (sel) fr.bottom -= k;
        cc_frame(dc, &fr, 0xD9D9D9);
        if (sel) { RECT cover = { r.left + 1, r.bottom - 2 * k, r.right - 1, r.bottom }; cc_fill(dc, &cover, 0xFFFFFF); }
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextW(dc, t->text[i] ? t->text[i] : L"", -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_HIDEPREFIX);
    }
    SelectObject(dc, of);
}

LRESULT CALLBACK TabProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Tab *t = ctl_get(h);
    if (!t && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCCREATE:
        t = calloc(1, sizeof(Tab));
        if (!t) return FALSE;
        t->cur = -1;
        ctl_set(h, t);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NCDESTROY:
        for (int i = 0; i < t->n; i++) free(t->text[i]);
        free(t);
        ctl_set(h, NULL);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); tab_paint(h, t, dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: tab_paint(h, t, (HDC)wp); return 0;
    case WM_SETFONT: t->font = (HFONT)wp; if (lp) InvalidateRect(h, NULL, TRUE); return 0;
    case WM_GETFONT: return (LRESULT)t->font;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        for (int i = 0; i < t->n; i++) {
            RECT r;
            tab_rect(h, t, i, &r);
            if (!PtInRect(&r, pt) || i == t->cur) continue;
            if (cc_notify(h, TCN_SELCHANGING, NULL)) return 0;
            t->cur = i;
            InvalidateRect(h, NULL, TRUE);
            cc_notify(h, TCN_SELCHANGE, NULL);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if ((wp == VK_LEFT || wp == VK_RIGHT) && t->n) {
            int n = t->cur + (wp == VK_RIGHT ? 1 : -1);
            if (n >= 0 && n < t->n && !cc_notify(h, TCN_SELCHANGING, NULL)) { t->cur = n; InvalidateRect(h, NULL, TRUE); cc_notify(h, TCN_SELCHANGE, NULL); }
        }
        return 0;
    case TCM_GETITEMCOUNT: return t->n;
    case TCM_INSERTITEMW: case TCM_INSERTITEMA: {
        const TCITEMW *it = (const TCITEMW *)lp;
        int at = (int)wp;
        if (t->n >= 64 || !it) return -1;
        if (at < 0 || at > t->n) at = t->n;
        memmove(&t->text[at + 1], &t->text[at], sizeof(WCHAR *) * (size_t)(t->n - at));
        memmove(&t->param[at + 1], &t->param[at], sizeof(LPARAM) * (size_t)(t->n - at));
        memmove(&t->image[at + 1], &t->image[at], sizeof(int) * (size_t)(t->n - at));
        t->text[at] = NULL;
        if (it->mask & TCIF_TEXT) set_str(&t->text[at], it->pszText, msg == TCM_INSERTITEMW);
        t->param[at] = (it->mask & TCIF_PARAM) ? it->lParam : 0;
        t->image[at] = (it->mask & TCIF_IMAGE) ? it->iImage : -1;
        t->n++;
        if (t->cur < 0) t->cur = 0;
        else if (t->cur >= at && t->n > 1) t->cur++;
        InvalidateRect(h, NULL, TRUE);
        return at;
    }
    case TCM_SETITEMW: case TCM_SETITEMA: {
        int i = (int)wp;
        const TCITEMW *it = (const TCITEMW *)lp;
        if (i < 0 || i >= t->n || !it) return FALSE;
        if (it->mask & TCIF_TEXT) set_str(&t->text[i], it->pszText, msg == TCM_SETITEMW);
        if (it->mask & TCIF_PARAM) t->param[i] = it->lParam;
        if (it->mask & TCIF_IMAGE) t->image[i] = it->iImage;
        InvalidateRect(h, NULL, TRUE);
        return TRUE;
    }
    case TCM_GETITEMW: {
        int i = (int)wp;
        TCITEMW *it = (TCITEMW *)lp;
        if (i < 0 || i >= t->n || !it) return FALSE;
        if ((it->mask & TCIF_TEXT) && it->pszText && it->cchTextMax > 0) {
            int n = MIN(wlen(t->text[i]), it->cchTextMax - 1);
            memcpy(it->pszText, t->text[i] ? t->text[i] : L"", 2 * (size_t)n);
            it->pszText[n] = 0;
        }
        if (it->mask & TCIF_PARAM) it->lParam = t->param[i];
        if (it->mask & TCIF_IMAGE) it->iImage = t->image[i];
        return TRUE;
    }
    case TCM_DELETEITEM: {
        int i = (int)wp;
        if (i < 0 || i >= t->n) return FALSE;
        free(t->text[i]);
        memmove(&t->text[i], &t->text[i + 1], sizeof(WCHAR *) * (size_t)(t->n - i - 1));
        memmove(&t->param[i], &t->param[i + 1], sizeof(LPARAM) * (size_t)(t->n - i - 1));
        memmove(&t->image[i], &t->image[i + 1], sizeof(int) * (size_t)(t->n - i - 1));
        t->n--;
        if (t->cur >= t->n) t->cur = t->n - 1;
        InvalidateRect(h, NULL, TRUE);
        return TRUE;
    }
    case TCM_DELETEALLITEMS: for (int i = 0; i < t->n; i++) free(t->text[i]); t->n = 0; t->cur = -1; InvalidateRect(h, NULL, TRUE); return TRUE;
    case TCM_GETCURSEL: return t->cur;
    case TCM_SETCURSEL: { int o = t->cur; if ((int)wp >= 0 && (int)wp < t->n) { t->cur = (int)wp; InvalidateRect(h, NULL, TRUE); } else return -1; return o; }
    case TCM_GETCURFOCUS: return t->cur;
    case TCM_SETCURFOCUS: if ((int)wp >= 0 && (int)wp < t->n) { t->cur = (int)wp; InvalidateRect(h, NULL, TRUE); } return 0;
    case TCM_GETITEMRECT: if ((int)wp < 0 || (int)wp >= t->n) return FALSE; tab_rect(h, t, (int)wp, (RECT *)lp); return TRUE;
    case TCM_GETROWCOUNT: return 1;
    case TCM_SETITEMSIZE: { LRESULT o = MAKELONG(t->fixw, t->fixh); t->fixw = LOWORD(lp); t->fixh = HIWORD(lp); return o; }
    case TCM_SETIMAGELIST: { HIMAGELIST o = t->il; t->il = (HIMAGELIST)lp; return (LRESULT)o; }
    case TCM_SETMINTABWIDTH: { int o = t->minw; t->minw = (int)lp < 0 ? 0 : (int)lp; return o; }
    case TCM_SETPADDING: t->padx = (short)LOWORD(lp); t->pady = (short)HIWORD(lp); t->pad_set = 1; InvalidateRect(h, NULL, TRUE); return 0;
    case TCM_GETIMAGELIST: return (LRESULT)t->il;
    case TCM_HITTEST: {
        TCHITTESTINFO *hi = (TCHITTESTINFO *)lp;
        for (int i = 0; hi && i < t->n; i++) { RECT r; tab_rect(h, t, i, &r); if (PtInRect(&r, hi->pt)) { hi->flags = 6; return i; } }
        if (hi) hi->flags = 1;
        return -1;
    }
    case TCM_ADJUSTRECT: {
        RECT *r = (RECT *)lp;
        int k = cc_k(h), rh = tab_row_h(h, t) + 2 * k;
        if (wp) { r->top -= rh; r->left -= 3 * k; r->right += 3 * k; r->bottom += 3 * k; }
        else { r->top += rh + 2 * k; r->left += 3 * k; r->right -= 3 * k; r->bottom -= 3 * k; }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* =======================================================================
 * SysLink: text whose <a> parts are links
 * ======================================================================= */

/* the control's text without its <a ...> tags (a quoted attribute may hold '>') */
static void link_text(HWND h, WCHAR *out)
{
    WCHAR buf[1024];
    GetWindowTextW(h, buf, 1024);
    int o = 0, in_tag = 0, quote = 0;
    for (int i = 0; buf[i] && o < 1023; i++) {
        if (in_tag) {
            if (quote) { if (buf[i] == quote) quote = 0; }
            else if (buf[i] == '"' || buf[i] == '\'') quote = buf[i];
            else if (buf[i] == '>') in_tag = 0;
        } else if (buf[i] == '<') in_tag = 1;
        else out[o++] = buf[i];
    }
    out[o] = 0;
}
LRESULT CALLBACK LinkProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c; GetClientRect(h, &c);
        HBRUSH bg = (HBRUSH)SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h);
        if (bg) FillRect(dc, &c, bg); else cc_fill(dc, &c, GetSysColor(COLOR_3DFACE));
        WCHAR out[1024];
        link_text(h, out);
        HGDIOBJ of = SelectObject(dc, cc_font_for(h));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, 0xCC6600);
        DrawTextW(dc, out, -1, &c, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETTEXT: { LRESULT r = DefWindowProcW(h, msg, wp, lp); InvalidateRect(h, NULL, TRUE); return r; }
    case 0x0701: {                                          /* LM_GETIDEALHEIGHT / LM_GETIDEALSIZE: wp = width (0: one line), lp = SIZE out */
        WCHAR out[1024];
        link_text(h, out);
        HDC dc = GetDC(h);
        HGDIOBJ of = SelectObject(dc, cc_font_for(h));
        RECT r = { 0, 0, wp ? (LONG)wp : 1 << 20, 0 };
        DrawTextW(dc, out[0] ? out : L" ", -1, &r, DT_CALCRECT | DT_NOPREFIX | (wp ? DT_WORDBREAK : 0));
        SelectObject(dc, of);
        ReleaseDC(h, dc);
        if (lp) { SIZE *sz = (SIZE *)lp; sz->cx = r.right - r.left; sz->cy = r.bottom - r.top; }
        return r.bottom - r.top;
    }
    case WM_LBUTTONUP: {
        struct { NMHDR hdr; UINT mask; int iLink; UINT state, stateMask; WCHAR szID[48], szUrl[2084]; } nm;
        memset(&nm, 0, sizeof(nm));
        cc_notify(h, NM_CLICK, &nm.hdr);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
