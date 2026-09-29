/*
 * drop.c — drag and drop between programs, user32's half. The desktop
 * tells a dragging program which window is under the pointer and whether
 * it takes drops (CTL_WINDOW_AT), and carries a dropped file list to
 * another program (CTL_DROP), which gets it as WM_NOVA_DROP. Here it
 * becomes WM_DROPFILES for a window with WS_EX_ACCEPTFILES, or goes to
 * ole32's drop-target hook (a program's IDropTarget). ole32 and shell32
 * use the Nova* exports below.
 */
#include "u32.h"

typedef BOOL (WINAPI *DropHook)(HWND hwnd, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes);
static DropHook g_hook;

USERAPI void NovaSetDropHook(void *fn) { g_hook = (DropHook)fn; }

/* the flags of a window and everything below it */
static DWORD tree_accept(HWND h)
{
    Wnd *w = W_quiet(h);
    DWORD all = w ? w->drop_accept & 3 : 0;
    for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) all |= tree_accept(c);
    return all;
}

/* the window (and its top-level) takes drops of the kind in MASK */
USERAPI BOOL NovaAcceptDrops(HWND h, DWORD mask, BOOL on)
{
    LOCK();
    Wnd *w = W_quiet(h);
    if (!w) { UNLOCK(); return FALSE; }
    Wnd *t = top_of(w);
    if (on) w->drop_accept |= mask; else w->drop_accept &= ~mask;
    DWORD all = tree_accept(t->h);
    t->drop_accept = (t->drop_accept & 3) | (all << 2);     /* bits 2-3: what the tree takes */
    if (t->kid) NtNovaGuiCtl(t->kid, CTL_ACCEPT_DROPS, all, NULL);
    UNLOCK();
    return TRUE;
}

/* the program window under a screen point: its desktop id, or 0 */
USERAPI UINT32 NovaWindowAt(POINT pt, DWORD *pid, DWORD *flags)
{
    INT32 io[4] = { pt.x, pt.y, 0, 0 };
    if (!NtNovaGuiCtl(0, CTL_WINDOW_AT, 0, io)) { if (pid) *pid = 0; if (flags) *flags = 0; return 0; }
    if (pid) *pid = (DWORD)io[1];
    if (flags) *flags = (DWORD)io[2];
    return (UINT32)io[0];
}

USERAPI HWND NovaTopFromKid(UINT32 kid)
{
    LOCK();
    Wnd *t = top_by_kid(kid);
    HWND h = t ? t->h : NULL;
    UNLOCK();
    return h;
}

/* a file list (UTF-16, double-NUL) dropped on another program's window */
USERAPI BOOL NovaSendDrop(UINT32 kid, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes)
{
    if (bytes > 60000) return FALSE;
    INT32 *buf = malloc(16 + bytes);
    if (!buf) return FALSE;
    buf[0] = screen.x; buf[1] = screen.y; buf[2] = (INT32)effect; buf[3] = (INT32)bytes;
    memcpy(buf + 4, files, bytes);
    BOOL ok = NtNovaGuiCtl(0, CTL_DROP, kid, buf) != 0;
    free(buf);
    return ok;
}

/* the drop arrived: WM_DROPFILES on the accepting window under the point */
void drop_from_kernel(Wnd *top, const MSG *km)
{
    (void)km;
    UINT32 cap = 20 + 64 * 1024;
    UINT32 *buf = malloc(cap);
    if (!buf) return;
    UINT64 got = NtNovaGuiCtl(top->kid, CTL_DROP_FETCH, cap, buf);
    if (got < 20) { free(buf); return; }
    POINT pt = { (INT32)buf[0], (INT32)buf[1] };
    DWORD effect = buf[2], bytes = buf[4];
    const WCHAR *files = (const WCHAR *)(buf + 5);
    /* the child under the point */
    HWND h = top->h;
    for (;;) {
        POINT c = pt;
        ScreenToClient(h, &c);
        HWND k = ChildWindowFromPointEx(h, c, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (!k || k == h) break;
        h = k;
    }
    if (g_hook && g_hook(h, pt, effect, files, bytes)) { free(buf); return; }
    /* WS_EX_ACCEPTFILES: this window or an ancestor */
    HWND a = h;
    while (a && !(GetWindowLongW(a, GWL_EXSTYLE) & WS_EX_ACCEPTFILES)) a = GetParent(a);
    if (a) {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 20 + bytes + 4);
        BYTE *d = g ? GlobalLock(g) : NULL;
        if (d) {
            POINT c = pt;
            ScreenToClient(a, &c);
            DWORD off = 20;
            memcpy(d, &off, 4);
            memcpy(d + 4, &c, 8);
            DWORD nc = 0, wide = 1;
            memcpy(d + 12, &nc, 4);
            memcpy(d + 16, &wide, 4);
            memcpy(d + 20, files, bytes);
            GlobalUnlock(g);
            PostMessageW(a, WM_DROPFILES, (WPARAM)g, 0);
        } else if (g) GlobalFree(g);
    }
    free(buf);
}
