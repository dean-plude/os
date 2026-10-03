/*
 * mdi.c — the multiple-document interface: the MDIClient class, its
 * WM_MDI* messages, DefFrameProc and DefMDIChildProc.
 *
 * NovaOS's child windows have no title bars of their own, so MDI children
 * are always shown as Windows shows maximized ones: each fills the client
 * window, the active one on top, and WM_MDIGETACTIVE reports them
 * maximized.  Programs switch between them through their own tabs or the
 * Window menu (WM_MDIACTIVATE, WM_MDINEXT); MFC's CMDIFrameWnd and
 * CMDIChildWnd (WinMerge) need nothing more.
 */
#include "u32.h"

typedef struct {
    HWND active;
    HMENU window_menu;              /* the program's Window menu (WM_MDISETMENU) */
    UINT first_id;                  /* idFirstChild: children's ids count from it */
    int next_id;
} Mdi;

static Mdi *mdi_of(HWND client)
{
    Wnd *w = W_quiet(client);
    return w && w->proc == MDIClientProc ? w->ctl : NULL;
}

/* the client's MDI children, top of the z-order first; returns the count */
static int children(HWND client, HWND *out, int cap)
{
    int n = 0;
    for (HWND c = GetWindow(client, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        if (GetWindowLongW(c, GWL_EXSTYLE) & WS_EX_MDICHILD) {
            if (n < cap) out[n] = c;
            n++;
        }
    return n < cap ? n : cap;
}

/* a child fills the client window, its frame (if any) outside it */
static void fill(HWND client, HWND child, UINT flags)
{
    RECT r;
    GetClientRect(client, &r);
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(child, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(child, GWL_EXSTYLE));
    SetWindowPos(child, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | flags);
}

static void activate(HWND client, Mdi *m, HWND child)
{
    HWND old = m->active;
    if (old == child) {
        if (child) SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    m->active = child;
    if (old && IsWindow(old)) {
        SendMessageW(old, WM_NCACTIVATE, FALSE, 0);
        SendMessageW(old, WM_MDIACTIVATE, (WPARAM)old, (LPARAM)child);
    }
    if (!child) return;
    fill(client, child, 0);
    SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(child, WM_NCACTIVATE, TRUE, 0);
    SendMessageW(child, WM_MDIACTIVATE, (WPARAM)old, (LPARAM)child);
    /* the keyboard follows when the frame has it */
    HWND frame = GetParent(client), f = GetFocus();
    if (frame && GetActiveWindow() == frame && !(f && (f == child || IsChild(child, f)))) SetFocus(child);
}

static HWND create_child(HWND client, Mdi *m, const MDICREATESTRUCTW *cs)
{
    DWORD style = cs->style;
    if (!(GetWindowLongW(client, GWL_STYLE) & MDIS_ALLCHILDSTYLES))
        style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    style = (style | WS_CHILD | WS_CLIPSIBLINGS) & ~(WS_POPUP | WS_MAXIMIZE | WS_MINIMIZE);
    int id = (int)m->first_id + m->next_id++;
    BOOL visible = (style & WS_VISIBLE) != 0;
    HWND h = CreateWindowExW(WS_EX_MDICHILD, cs->szClass, cs->szTitle, style & ~WS_VISIBLE, 0, 0, 0, 0, client,
                             (HMENU)(INT_PTR)id, (HINSTANCE)cs->hOwner, (LPVOID)cs);
    if (!h) return NULL;
    fill(client, h, 0);
    if (visible) ShowWindow(h, SW_SHOW);
    activate(client, m, h);
    return h;
}

static void destroy_child(HWND client, Mdi *m, HWND child)
{
    if (!child || !IsWindow(child)) return;
    if (m->active == child) {
        HWND list[64];
        int n = children(client, list, 64);
        HWND next = NULL;
        for (int i = 0; i < n; i++) if (list[i] != child) { next = list[i]; break; }
        activate(client, m, next);
        if (m->active == child) m->active = NULL;
    }
    DestroyWindow(child);
}

static void next_child(HWND client, Mdi *m, HWND from, BOOL prev)
{
    HWND list[64];
    int n = children(client, list, 64);
    if (n < 2) return;
    /* the z-order changes as children come to the top: go by the ids instead */
    if (!from) from = m->active;
    int at = -1;
    for (int i = 0; i < n; i++) if (list[i] == from) at = i;
    LONG_PTR cur = from ? GetWindowLongPtrW(from, GWLP_ID) : 0;
    HWND best = NULL;
    LONG_PTR best_d = 0;
    for (int i = 0; i < n; i++) {
        if (list[i] == from) continue;
        LONG_PTR d = GetWindowLongPtrW(list[i], GWLP_ID) - cur;
        if (prev) d = -d;
        if (d < 0) d += 0x10000;
        if (!best || d < best_d) { best = list[i]; best_d = d; }
    }
    (void)at;
    if (best) activate(client, m, best);
}

LRESULT CALLBACK MDIClientProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    Mdi *m = w->ctl;
    switch (msg) {
    case WM_NCCREATE:
        w->ctl = calloc(1, sizeof(Mdi));
        if (!w->ctl) return FALSE;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
        const CLIENTCREATESTRUCT *cc = cs ? cs->lpCreateParams : NULL;
        if (m && cc) { m->window_menu = cc->hWindowMenu; m->first_id = cc->idFirstChild; }
        return 0;
    }
    case WM_NCDESTROY:
        free(w->ctl);
        w->ctl = NULL;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_MDICREATE: return m && lp ? (LRESULT)create_child(h, m, (const MDICREATESTRUCTW *)lp) : 0;
    case WM_MDIDESTROY: if (m) destroy_child(h, m, (HWND)wp); return 0;
    case WM_MDIACTIVATE:
        if (m && wp && GetParent((HWND)wp) == h) activate(h, m, (HWND)wp);
        return 0;
    case WM_MDIGETACTIVE:
        if (m && m->active && !IsWindow(m->active)) m->active = NULL;
        if (lp) *(BOOL *)lp = m && m->active;              /* shown maximized */
        return m ? (LRESULT)m->active : 0;
    case WM_MDINEXT: if (m) next_child(h, m, (HWND)wp, lp != 0); return 0;
    case WM_MDIMAXIMIZE: case WM_MDIRESTORE:
        if (m && wp && GetParent((HWND)wp) == h) activate(h, m, (HWND)wp);
        return 0;
    case WM_MDITILE: case WM_MDICASCADE: case WM_MDIICONARRANGE: return TRUE;
    case WM_MDISETMENU: {
        HWND frame = GetParent(h);
        HMENU old = frame ? GetMenu(frame) : NULL;
        if (wp && frame) SetMenu(frame, (HMENU)wp);
        if (lp && m) m->window_menu = (HMENU)lp;
        return (LRESULT)old;
    }
    case WM_MDIREFRESHMENU: {
        HWND frame = GetParent(h);
        if (frame) DrawMenuBar(frame);
        return frame ? (LRESULT)GetMenu(frame) : 0;
    }
    case WM_SIZE: {
        HWND list[64];
        int n = children(h, list, 64);
        for (int i = 0; i < n; i++) fill(h, list[i], 0);
        return 0;
    }
    case WM_SETFOCUS:
        if (m && m->active && IsWindow(m->active)) SetFocus(m->active);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT frame_proc(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp, int wide)
{
    Mdi *m = client ? mdi_of(client) : NULL;
    switch (msg) {
    case WM_COMMAND:
        /* the Window menu's entries for the children */
        if (m && HIWORD(wp) == 0 && LOWORD(wp) >= m->first_id) {
            HWND list[64];
            int n = children(client, list, 64);
            for (int i = 0; i < n; i++)
                if ((UINT)GetWindowLongPtrW(list[i], GWLP_ID) == LOWORD(wp)) { activate(client, m, list[i]); return 0; }
        }
        break;
    case WM_SETFOCUS:
        if (client && IsWindow(client)) { SetFocus(client); return 0; }
        break;
    case WM_SIZE:
        if (client && IsWindow(client)) MoveWindow(client, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        break;
    }
    return wide ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
}
USERAPI LRESULT DefFrameProcW(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp) { return frame_proc(h, client, msg, wp, lp, 1); }
USERAPI LRESULT DefFrameProcA(HWND h, HWND client, UINT msg, WPARAM wp, LPARAM lp) { return frame_proc(h, client, msg, wp, lp, 0); }

static LRESULT child_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide)
{
    HWND client = GetParent(h);
    Mdi *m = client ? mdi_of(client) : NULL;
    switch (msg) {
    case WM_CHILDACTIVATE:
    case WM_SETFOCUS:
    case WM_MOUSEACTIVATE:
        if (m && m->active != h) activate(client, m, h);
        break;
    case WM_CLOSE:
        if (m) { destroy_child(client, m, h); return 0; }
        break;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_CLOSE: SendMessageW(h, WM_CLOSE, 0, 0); return 0;
        case SC_NEXTWINDOW: if (m) next_child(client, m, h, FALSE); return 0;
        case SC_PREVWINDOW: if (m) next_child(client, m, h, TRUE); return 0;
        case SC_MAXIMIZE: case SC_RESTORE: case SC_MINIMIZE:
            if (m) activate(client, m, h);
            return 0;
        }
        break;
    case WM_WINDOWPOSCHANGING: {
        /* children keep filling the client window */
        WINDOWPOS *p = (WINDOWPOS *)lp;
        if (m && p && (!(p->flags & SWP_NOSIZE) || !(p->flags & SWP_NOMOVE))) {
            RECT r;
            GetClientRect(client, &r);
            AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
            p->x = r.left; p->y = r.top; p->cx = r.right - r.left; p->cy = r.bottom - r.top;
            p->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
        }
        break;
    }
    }
    return wide ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
}
USERAPI LRESULT DefMDIChildProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return child_proc(h, msg, wp, lp, 1); }
USERAPI LRESULT DefMDIChildProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return child_proc(h, msg, wp, lp, 0); }

USERAPI HWND CreateMDIWindowW(LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int cx, int cy, HWND client,
                              HINSTANCE inst, LPARAM lp)
{
    MDICREATESTRUCTW cs = { cls, title, inst, x, y, cx, cy, style, lp };
    return (HWND)SendMessageW(client, WM_MDICREATE, 0, (LPARAM)&cs);
}
USERAPI HWND CreateMDIWindowA(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy, HWND client,
                              HINSTANCE inst, LPARAM lp)
{
    WCHAR *wc = (ULONG_PTR)cls < 0x10000 ? (WCHAR *)cls : a2w(cls, -1);
    WCHAR *wt = title ? a2w(title, -1) : NULL;
    HWND h = CreateMDIWindowW(wc, wt, style, x, y, cx, cy, client, inst, lp);
    if ((ULONG_PTR)cls >= 0x10000) free(wc);
    free(wt);
    return h;
}

/* Ctrl+F4 closes the active child, Ctrl+F6 / Ctrl+Tab go to the next */
USERAPI BOOL TranslateMDISysAccel(HWND client, LPMSG msg)
{
    Mdi *m = mdi_of(client);
    if (!m || !msg || !m->active || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) return FALSE;
    if (!(GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return FALSE;
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (msg->wParam) {
    case VK_F4: SendMessageW(m->active, WM_SYSCOMMAND, SC_CLOSE, 0); return TRUE;
    case VK_F6: case VK_TAB: next_child(client, m, NULL, shift); return TRUE;
    }
    return FALSE;
}
