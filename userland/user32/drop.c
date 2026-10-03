/*
 * drop.c — drag and drop between programs, user32's half. The desktop
 * tells a dragging program which window is under the pointer and whether
 * it takes drops (CTL_WINDOW_AT), and carries a dropped file list to
 * another program (CTL_DROP), which gets it as WM_NOVA_DROP. Here it
 * becomes WM_DROPFILES for a window with WS_EX_ACCEPTFILES, or goes to
 * ole32's drop-target hook (a program's IDropTarget). ole32 and shell32
 * use the Nova* exports below.
 *
 * As on Windows, the drop is synchronous for the source: NovaSendDrop
 * waits until the target has handled it (CTL_DROP_DONE), so a source may
 * delete the files once DoDragDrop returns. 7-Zip does: files dragged out
 * of an archive are extracted to a temporary folder that goes right after.
 */
#include "u32.h"

#ifndef DROPEFFECT_NONE
#define DROPEFFECT_NONE 0
#define DROPEFFECT_COPY 1
#endif

typedef BOOL (WINAPI *DropHook)(HWND hwnd, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes, DWORD *taken);
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
    dpi_to_logical(&pt);                                    /* the desktop's pixels */
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

#define DROP_WAIT_MS (5 * 60 * 1000)      /* a target that never answers */

/* a file list (UTF-16, double-NUL) dropped on another program's window:
 * waits for the target to handle it and returns the effect it took
 * (DROPEFFECT_NONE: refused, or the window went away) */
USERAPI DWORD NovaSendDrop(UINT32 kid, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes)
{
    if (bytes > 60000) return DROPEFFECT_NONE;
    INT32 *buf = malloc(16 + bytes);
    if (!buf) return DROPEFFECT_NONE;
    dpi_to_logical(&screen);                                /* the target converts it to its own */
    buf[0] = screen.x; buf[1] = screen.y; buf[2] = (INT32)effect; buf[3] = (INT32)bytes;
    memcpy(buf + 4, files, bytes);
    ULONG_PTR seq = (ULONG_PTR)NtNovaGuiCtl(0, CTL_DROP, kid, buf);
    free(buf);
    if (!seq) return DROPEFFECT_NONE;
    DWORD start = GetTickCount();
    for (;;) {
        LONG_PTR r = NtNovaGuiCtl(0, CTL_DROP_STATUS, seq, NULL);
        if (r < 0) return DROPEFFECT_NONE;
        if (r > 0) return (DWORD)(r - 1);
        if (GetTickCount() - start > DROP_WAIT_MS) return DROPEFFECT_NONE;
        MSG m;                              /* keep our windows drawn meanwhile */
        while (PeekMessageW(&m, NULL, WM_PAINT, WM_PAINT, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(10);
    }
}

/* the drop arrived: WM_DROPFILES on the accepting window under the point */
void drop_from_kernel(Wnd *top, const MSG *km)
{
    (void)km;
    UINT32 cap = 20 + 64 * 1024;
    UINT32 *buf = malloc(cap);
    if (!buf) return;
    UINT64 got = NtNovaGuiCtl(top->kid, CTL_DROP_FETCH, cap, buf);
    DWORD taken = DROPEFFECT_NONE;
    if (got < 20) { free(buf); NtNovaGuiCtl(top->kid, CTL_DROP_DONE, taken, NULL); return; }
    POINT pt = { (INT32)buf[0], (INT32)buf[1] };
    dpi_to_proc(&pt);
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
    if (g_hook && g_hook(h, pt, effect, files, bytes, &taken)) {
        free(buf);
        NtNovaGuiCtl(top->kid, CTL_DROP_DONE, taken, NULL);
        return;
    }
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
            /* sent, not posted: the program has read the files when the source hears back */
            SendMessageW(a, WM_DROPFILES, (WPARAM)g, 0);
            taken = DROPEFFECT_COPY;            /* the program has the names; nothing was moved */
        } else if (g) GlobalFree(g);
    }
    free(buf);
    NtNovaGuiCtl(top->kid, CTL_DROP_DONE, taken, NULL);
}
