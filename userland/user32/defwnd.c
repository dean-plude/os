/*
 * defwnd.c — DefWindowProc: what a window does with the messages its
 * procedure leaves alone
 */
#include "u32.h"

static int g_alt_alone;              /* Alt went down and nothing else happened since */

static LRESULT nc_hit_test(Wnd *w, POINT pt)
{
    POINT o;
    wnd_screen_origin(w, 0, &o);
    int x = pt.x - o.x, y = pt.y - o.y;
    RECT wr = { 0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top };
    POINT wp = { x, y };
    if (!PtInRect(&wr, wp)) return HTNOWHERE;
    RECT cr = w->client;
    OffsetRect(&cr, -w->rect.left, -w->rect.top);
    if (PtInRect(&cr, wp)) return HTCLIENT;
    if (w->style & (WS_VSCROLL | WS_HSCROLL)) {
        RECT hr, vr, corner;
        sb_nc_rects(w, &hr, &vr, &corner);
        if ((w->style & WS_VSCROLL) && PtInRect(&vr, wp)) return HTVSCROLL;
        if ((w->style & WS_HSCROLL) && PtInRect(&hr, wp)) return HTHSCROLL;
        if (PtInRect(&corner, wp)) return (w->style & WS_THICKFRAME) ? HTBOTTOMRIGHT : HTSIZE;
    }
    if (!w->parent && w->menu && y < cr.top && y >= cr.top - menu_bar_height(w, cr.right - cr.left)) return HTMENU;
    if (!w->parent && y < FRAME_TITLE) return HTCAPTION;
    return HTBORDER;
}

static void def_ctlcolor(UINT msg, HDC dc, HBRUSH *br)
{
    switch (msg) {
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, sys_color(COLOR_WINDOWTEXT));
        SetBkColor(dc, sys_color(COLOR_WINDOW));
        *br = sys_brush(COLOR_WINDOW);
        break;
    case WM_CTLCOLORSCROLLBAR:
        SetTextColor(dc, sys_color(COLOR_WINDOWTEXT));
        SetBkColor(dc, sys_color(COLOR_3DFACE));
        *br = sys_brush(COLOR_SCROLLBAR);
        break;
    default:
        SetTextColor(dc, sys_color(COLOR_WINDOWTEXT));
        SetBkColor(dc, sys_color(COLOR_3DFACE));
        *br = sys_brush(COLOR_3DFACE);
    }
}

static HBRUSH class_brush(Wnd *w)
{
    HBRUSH b = w->cls ? w->cls->brush : 0;
    if ((ULONG_PTR)b > 0 && (ULONG_PTR)b <= 31) return sys_brush((int)(ULONG_PTR)b - 1);
    return b;
}

static LRESULT def_common(Wnd *w, HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide)
{
    switch (msg) {
    case WM_NCCREATE:
        if (w->style & (WS_HSCROLL | WS_VSCROLL)) {
            for (int i = 0; i < 2; i++) { w->sb[i].min = 0; w->sb[i].max = 100; }
        }
        return TRUE;
    case WM_NCCALCSIZE:
        if (wp) default_nc_calc(w, &((NCCALCSIZE_PARAMS *)lp)->rgrc[0]);
        else default_nc_calc(w, (RECT *)lp);
        return 0;
    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        return nc_hit_test(w, pt);
    }
    case WM_NCPAINT: nc_paint(w); return 0;
    case WM_NCACTIVATE:
        if (w->menu && !w->parent) invalidate_nc(w);
        return TRUE;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        switch (wp) {
        case HTVSCROLL: sb_track(w, SB_VERT, pt); return 0;
        case HTHSCROLL: sb_track(w, SB_HORZ, pt); return 0;
        case HTMENU: menu_track_bar(w, pt, 0); return 0;
        }
        return 0;
    }
    case WM_NCRBUTTONUP: case WM_RBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (msg == WM_RBUTTONUP) ClientToScreen(h, &pt);
        send_msg(w, WM_CONTEXTMENU, (WPARAM)h, MAKELPARAM(pt.x, pt.y));
        return 0;
    }
    case WM_XBUTTONUP: case WM_NCXBUTTONUP:
        /* the side buttons are Back and Forward (XBUTTON1 and 2 are
         * APPCOMMAND_BROWSER_BACKWARD and _FORWARD) */
        if (HIWORD(wp) == XBUTTON1 || HIWORD(wp) == XBUTTON2)
            send_msg(w, WM_APPCOMMAND, (WPARAM)h, MAKELPARAM(msg == WM_XBUTTONUP ? LOWORD(wp) : 0, FAPPCOMMAND_MOUSE | HIWORD(wp)));
        return TRUE;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_NCXBUTTONDOWN: case WM_NCXBUTTONDBLCLK:
        return TRUE;
    case WM_CONTEXTMENU: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_APPCOMMAND: case WM_HELP:
        if ((w->style & WS_CHILD) && w->parent) return send_msg(w->parent, msg, wp, lp);
        return 0;
    case WM_SETCURSOR:
        if ((w->style & WS_CHILD) && w->parent && send_msg(w->parent, msg, wp, lp)) return TRUE;
        if (LOWORD(lp) == HTCLIENT && w->cls && w->cls->cursor) { SetCursor(w->cls->cursor); return TRUE; }
        return FALSE;
    case WM_MOUSEACTIVATE:
        if ((w->style & WS_CHILD) && w->parent) {
            LRESULT r = send_msg(w->parent, msg, wp, lp);
            if (r) return r;
        }
        return MA_ACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !w->parent) {
            Wnd *f = W_quiet(g_focus);
            if (!f || (f != w && !is_child_of(w, f))) {
                if (w->focus_save && W_quiet(w->focus_save) && is_child_of(w, W_quiet(w->focus_save))) set_focus(w->focus_save);
                else set_focus(h);
            }
        }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_CLOSE: send_msg(w, WM_CLOSE, 0, 0); return 0;
        case SC_MINIMIZE: ShowWindow(h, SW_MINIMIZE); return 0;
        case SC_MAXIMIZE: ShowWindow(h, SW_MAXIMIZE); return 0;
        case SC_RESTORE: ShowWindow(h, SW_RESTORE); return 0;
        case SC_KEYMENU: {
            Wnd *t = top_of(w);
            if (t->menu) { POINT pt = { 0, 0 }; menu_track_bar(t, pt, lp ? (int)lp : -1); }
            else if (lp) MessageBeep(0);
            return 0;
        }
        case SC_MOUSEMENU: return 0;
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_MENU || wp == VK_F10) { g_alt_alone = 1; return 0; }
        g_alt_alone = 0;
        if (wp == VK_F4 && (lp & (1 << 29))) { post_msg(top_of(w), top_of(w)->h, WM_SYSCOMMAND, SC_CLOSE, 0); return 0; }
        return 0;
    case WM_KEYDOWN: {
        g_alt_alone = 0;
        if (wp == VK_F10) { g_alt_alone = 1; return 0; }
        /* Browser, volume, media and launch keys: APPCOMMAND_* 1-18 in
         * the order of their virtual keys (VK_BROWSER_BACK..VK_LAUNCH_APP2) */
        if (wp >= VK_BROWSER_BACK && wp <= VK_LAUNCH_APP2) {
            WORD mk = (GetKeyState(VK_SHIFT) < 0 ? MK_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? MK_CONTROL : 0);
            send_msg(w, WM_APPCOMMAND, (WPARAM)h, MAKELPARAM(mk, FAPPCOMMAND_KEY | (wp - VK_BROWSER_BACK + 1)));
        }
        return 0;
    }
    case WM_SYSKEYUP: case WM_KEYUP:
        if ((wp == VK_MENU || wp == VK_F10) && g_alt_alone && g_alt_tap) {
            g_alt_alone = g_alt_tap = 0;
            Wnd *t = top_of(w);
            if (t->menu) send_msg(t, WM_SYSCOMMAND, SC_KEYMENU, 0);
        }
        if (msg == WM_KEYUP && (wp == VK_APPS)) send_msg(w, WM_CONTEXTMENU, (WPARAM)h, (LPARAM)-1);
        return 0;
    case WM_SYSCHAR:
        g_alt_alone = 0;
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_SPACE) return 0;
        if (lp & (1 << 29)) send_msg(top_of(w), WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)wp);
        return 0;
    case WM_SETTEXT:
        if (wide) set_text(w, (LPCWSTR)lp);
        else { WCHAR *s = a2w((const char *)lp, -1); set_text(w, s); free(s); }
        if (w->parent && !EqualRect(&w->rect, &w->client)) invalidate_nc(w);
        return TRUE;
    case WM_GETTEXT: {
        if (!wp || !lp) return 0;
        const WCHAR *s = w->text ? w->text : L"";
        if (wide) {
            int n = MIN(wlen(s), (int)wp - 1);
            memcpy((void *)lp, s, 2 * (size_t)n);
            ((WCHAR *)lp)[n] = 0;
            return n;
        }
        int n = WideCharToMultiByte(CP_ACP, 0, s, -1, (char *)lp, (int)wp, NULL, NULL);
        if (n <= 0) { n = (int)wp; }
        ((char *)lp)[n - 1] = 0;
        return n - 1;
    }
    case WM_GETTEXTLENGTH: {
        const WCHAR *s = w->text ? w->text : L"";
        if (wide) return wlen(s);
        return WideCharToMultiByte(CP_ACP, 0, s, wlen(s), NULL, 0, NULL, NULL);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        HBRUSH b = class_brush(w);
        if (!b) return 0;
        RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
        FillRect((HDC)wp, &r, b);
        return 1;
    }
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: {
        HBRUSH br;
        def_ctlcolor(msg, (HDC)wp, &br);
        return (LRESULT)br;
    }
    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS *p = (WINDOWPOS *)lp;
        if (!(p->flags & 0x0800))                           /* SWP_NOCLIENTMOVE */
            send_msg(w, WM_MOVE, 0, MAKELPARAM(w->client.left, w->client.top));
        if (!(p->flags & 0x1000))                           /* SWP_NOCLIENTSIZE */
            send_msg(w, WM_SIZE, w->minimized ? SIZE_MINIMIZED : w->maximized ? SIZE_MAXIMIZED : SIZE_RESTORED,
                     MAKELPARAM(w->client.right - w->client.left, w->client.bottom - w->client.top));
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        m->ptMaxSize.x = GetSystemMetrics(SM_CXSCREEN); m->ptMaxSize.y = GetSystemMetrics(SM_CYSCREEN);
        m->ptMinTrackSize.x = GetSystemMetrics(SM_CXMINTRACK); m->ptMinTrackSize.y = GetSystemMetrics(SM_CYMINTRACK);
        m->ptMaxTrackSize = m->ptMaxSize;
        return 0;
    }
    case WM_SETICON: {
        HICON old;
        if (wp == ICON_BIG) { old = w->icon_big; w->icon_big = (HICON)lp; }
        else { old = w->icon_small; w->icon_small = (HICON)lp; }
        return (LRESULT)old;
    }
    case WM_GETICON:
        if (wp == ICON_BIG) return (LRESULT)w->icon_big;
        return (LRESULT)(w->icon_small ? w->icon_small : wp == ICON_SMALL2 ? w->icon_big : 0);
    case WM_QUERYOPEN: case WM_QUERYENDSESSION: return TRUE;
    case WM_NOTIFYFORMAT: return w->wide ? NFR_UNICODE : NFR_ANSI;
    case WM_CANCELMODE:
        if (g_capture == h) ReleaseCapture();
        if (g_menu_owner) menu_cancel();
        return 0;
    case WM_VKEYTOITEM: case WM_CHARTOITEM: return -1;
    case WM_SHOWWINDOW: return 0;
    case WM_GETFONT: return 0;
    case WM_SETREDRAW:
        if (wp) { invalidate(w, NULL, TRUE, 1); }
        return 0;
    case WM_PRINT: {
        /* Draw the window into DC @wp: its background and client area, as
         * double-buffering programs ask of a control they then copy to the
         * screen; and its children.  With PRF_NONCLIENT the DC's origin is
         * the window's corner (the frame itself is not drawn), else the
         * client area's. */
        HDC dc = (HDC)wp;
        if ((lp & PRF_CHECKVISIBLE) && !(w->style & WS_VISIBLE)) return 0;
        POINT o;
        int nc = (lp & PRF_NONCLIENT) != 0;
        if (!OffsetViewportOrgEx(dc, nc ? w->client.left - w->rect.left : 0, nc ? w->client.top - w->rect.top : 0, &o)) return 0;
        if (lp & PRF_ERASEBKGND) send_msg(w, WM_ERASEBKGND, wp, 0);
        if (lp & PRF_CLIENT) send_msg(w, WM_PRINTCLIENT, wp, lp);
        if (lp & PRF_CHILDREN) {
            Wnd *c = w->child;
            while (c && c->next) c = c->next;               /* bottom-most first */
            for (; c; c = c->prev) {
                if (!(c->style & WS_VISIBLE)) continue;
                POINT co;
                OffsetViewportOrgEx(dc, c->rect.left, c->rect.top, &co);
                send_msg(c, WM_PRINT, wp, (lp & ~PRF_CHECKVISIBLE) | PRF_NONCLIENT);
                SetViewportOrgEx(dc, co.x, co.y, NULL);
            }
        }
        SetViewportOrgEx(dc, o.x, o.y, NULL);
        return 0;
    }
    case WM_PRINTCLIENT: return 0;
    case WM_QUERYUISTATE: return UISF_HIDEACCEL | UISF_HIDEFOCUS;
    case WM_UPDATEUISTATE: case WM_CHANGEUISTATE: return 0;
    case WM_ISACTIVEICON: return g_active == h;
    case WM_DROPOBJECT: case WM_QUERYDROPOBJECT: return 0;
    case WM_QUERYDRAGICON: return (LRESULT)(w->cls ? w->cls->icon : 0);
    case WM_INPUTLANGCHANGEREQUEST: return 0;
    case WM_GETOBJECT: return 0;
    }
    return 0;
}

USERAPI LRESULT DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        if (cs && cs->lpszName && (ULONG_PTR)cs->lpszName >= 0x10000) set_text(w, cs->lpszName);
    }
    return def_common(w, h, msg, wp, lp, 1);
}

USERAPI LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        if (cs && cs->lpszName && (ULONG_PTR)cs->lpszName >= 0x10000) {
            WCHAR *s = a2w(cs->lpszName, -1);
            set_text(w, s);
            free(s);
        }
    }
    return def_common(w, h, msg, wp, lp, 0);
}


void notify_parent(Wnd *w, UINT code)
{
    Wnd *p = w->parent;
    if (!p) return;
    send_msg(p, WM_COMMAND, MAKEWPARAM((WORD)w->id, code), (LPARAM)w->h);
}
