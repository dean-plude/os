/*
 * um_gui.c — the kernel half of user32/gdi32: real windows for programs
 *
 * A program window is a WND in the desktop's window manager whose client
 * area is a bitmap in the program's own memory (COLORREF pixels).  The
 * program draws into that bitmap with gdi32; the WM's paint callback (on
 * the desktop thread) blits it to the screen row by row through the
 * process page tables.  Input and lifecycle events from the WM are turned
 * into Win32 messages and queued for the program's GetMessage loop.
 *
 * The window manager runs on the desktop thread under the desktop lock, so
 * every WM touch from a program thread takes DesktopLock first, and the WM
 * callbacks (which already hold it) enqueue messages without racing.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../wm/wm.h"
#include "../gdi/gdi.h"

/* Win32 window messages we deliver */
#define WM_DESTROY        0x0002
#define WM_SIZE           0x0005
#define WM_PAINT          0x000F
#define WM_CLOSE          0x0010
#define WM_QUIT           0x0012
#define WM_ACTIVATE       0x0006
#define WM_KEYDOWN        0x0100
#define WM_KEYUP          0x0101
#define WM_CHAR           0x0102
#define WM_TIMER          0x0113
#define WM_MOUSEMOVE      0x0200
#define WM_LBUTTONDOWN    0x0201
#define WM_LBUTTONUP      0x0202
#define WM_LBUTTONDBLCLK  0x0203

#define GUI_MAX_WINDOWS   32
#define GUI_QUEUE         128
#define GUI_TIMERS        8
#define GUI_MAX_W         1600
#define GUI_MAX_H         1000
#define GUI_BITMAP_VA     UINT64_C(0x00007FF900000000)
#define GUI_BITMAP_STRIDE UINT64_C(0x01000000)          /* 16 MiB apart */

/* Matches the Win32 MSG structure byte-for-byte */
typedef struct {
    UINT64 hwnd;
    UINT32 message;
    UINT32 _pad;
    UINT64 wParam;
    UINT64 lParam;
    UINT32 time;
    INT32  pt_x, pt_y;
} GuiMsg;

typedef struct {
    bool        used;
    UmProcess  *proc;
    WND        *wnd;
    UINT32      id;                 /* handle value the program sees */
    UINT64      bitmap;             /* user VA of the client bitmap */
    int         cw, ch;            /* client size (logical px) */
    GuiMsg      q[GUI_QUEUE];
    volatile UINT32 head, tail;
    bool        quit;
    struct { UINT32 id; UINT32 period; UINT64 next; bool used; } timers[GUI_TIMERS];
} GuiWin;

static GuiWin g_win[GUI_MAX_WINDOWS];
static UINT32 g_win_next = 1;
static UINT32 g_row[GUI_MAX_W];     /* blit scratch; used on the desktop thread */

static GuiWin *win_of_handle(UmProcess *p, UINT64 h)
{
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].proc == p && g_win[i].id == (UINT32)h) return &g_win[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * Message queue (enqueue: desktop thread under DesktopLock; dequeue:
 * program thread, briefly under DesktopLock)
 * ----------------------------------------------------------------------- */
static void enqueue(GuiWin *g, UINT32 msg, UINT64 wp, UINT64 lp, int x, int y)
{
    if (g->head - g->tail >= GUI_QUEUE) return;             /* full: drop */
    /* Coalesce consecutive paints and mouse moves */
    if ((msg == WM_PAINT || msg == WM_MOUSEMOVE) && g->head != g->tail) {
        GuiMsg *last = &g->q[(g->head - 1) % GUI_QUEUE];
        if (last->message == msg) { last->wParam = wp; last->lParam = lp; last->pt_x = x; last->pt_y = y; return; }
    }
    GuiMsg *m = &g->q[g->head % GUI_QUEUE];
    m->hwnd = g->id;
    m->message = msg;
    m->_pad = 0;
    m->wParam = wp;
    m->lParam = lp;
    m->time = (UINT32)(sched_ticks() * 10);
    m->pt_x = x; m->pt_y = y;
    __atomic_store_n(&g->head, g->head + 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * WM callbacks (desktop thread, DesktopLock held)
 * ----------------------------------------------------------------------- */
static void gui_paint(WND *w)
{
    GuiWin *g = w->user;
    if (!g || !g->proc || g->proc->exited) return;
    GdiRect cr = WmClientRect(w);
    int w_px = cr.w < g->cw ? cr.w : g->cw;
    int h_px = cr.h < g->ch ? cr.h : g->ch;
    if (w_px > GUI_MAX_W) w_px = GUI_MAX_W;
    for (int y = 0; y < h_px; y++) {
        if (!um_read(g->proc, g->bitmap + (UINT64)y * g->cw * 4, g_row, (UINT64)w_px * 4)) break;
        GdiBlitBGRA(RECT(cr.x, cr.y + y, w_px, 1), g_row, w_px);
    }
}

static void gui_key(WND *w, const KeyEvent *k)
{
    GuiWin *g = w->user;
    if (!g) return;
    UINT32 vk = k->scancode;                                /* scancode as a rough VK */
    if (k->pressed) {
        enqueue(g, WM_KEYDOWN, vk, 1, 0, 0);
        if (k->ch) enqueue(g, WM_CHAR, (UINT8)k->ch, 1, 0, 0);
    } else {
        enqueue(g, WM_KEYUP, vk, 0, 0, 0);
    }
}

static void gui_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    GuiWin *g = w->user;
    if (!g) return;
    UINT64 lp = ((UINT64)(UINT16)y << 16) | (UINT16)x;
    switch (msg) {
    case WM_MOUSE_DOWN:   enqueue(g, WM_LBUTTONDOWN, 1, lp, x, y); break;
    case WM_MOUSE_UP:     enqueue(g, WM_LBUTTONUP, 0, lp, x, y); break;
    case WM_MOUSE_MOVE:   enqueue(g, WM_MOUSEMOVE, 0, lp, x, y); break;
    case WM_MOUSE_DBLCLK: enqueue(g, WM_LBUTTONDBLCLK, 1, lp, x, y); break;
    }
}

static bool gui_tick(WND *w)
{
    GuiWin *g = w->user;
    if (!g) return false;
    UINT64 now = sched_ticks();
    for (int i = 0; i < GUI_TIMERS; i++)
        if (g->timers[i].used && now >= g->timers[i].next) {
            enqueue(g, WM_TIMER, g->timers[i].id, 0, 0, 0);
            g->timers[i].next = now + g->timers[i].period;
        }
    return false;
}

static void gui_close(WND *w)
{
    GuiWin *g = w->user;
    if (!g) return;
    /* The user clicked the X: ask the program to close.  The WND is going
     * away now, so detach; the program's Destroy will free the slot. */
    enqueue(g, WM_CLOSE, 0, 0, 0, 0);
    g->wnd = NULL;
    w->user = NULL;
}

/* -----------------------------------------------------------------------
 * Syscalls
 * ----------------------------------------------------------------------- */
/* In/out struct at the pointer passed to NtNovaGuiCreate */
typedef struct {
    INT32  x, y, w, h;
    UINT32 style;                   /* 0 = normal titled window */
    UINT64 title;                   /* UTF-16 title */
    /* out: */
    UINT64 hwnd;
    UINT64 bitmap;
    UINT32 stride;                  /* bytes per row */
    UINT32 cw, ch;
} GuiCreate;

static void utf16_to_ascii(UmProcess *p, UINT64 va, char *out, int cap)
{
    out[0] = '\0';
    if (!va) return;
    UINT16 w[128];
    int n = 0;
    for (; n < cap - 1 && n < 128; n++) {
        if (!um_read(p, va + (UINT64)n * 2, &w[0], 2) || !w[0]) break;
        out[n] = w[0] < 0x80 ? (char)w[0] : '?';
    }
    out[n] = '\0';
}

static UINT64 sys_gui_create(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    GuiCreate gc;
    if (!NT_SUCCESS(CopyFromUser(&gc, (const void *)(uintptr_t)a1, sizeof(gc)))) return 0;
    int cw = gc.w, ch = gc.h;
    if (cw < 1) cw = 320; if (ch < 1) ch = 200;
    if (cw > GUI_MAX_W) cw = GUI_MAX_W; if (ch > GUI_MAX_H) ch = GUI_MAX_H;

    int slot = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) if (!g_win[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    GuiWin *g = &g_win[slot];
    memset(g, 0, sizeof(*g));

    /* Client bitmap in the program's address space */
    UINT64 va = GUI_BITMAP_VA + (UINT64)slot * GUI_BITMAP_STRIDE;
    UINT64 size = ((UINT64)cw * ch * 4 + 0xFFF) & ~0xFFFULL;
    um_lock(&p->lock);
    bool ok = um_is_free(p, va, size) && um_region_add(p, va, size, 0x04, false) &&
              um_commit(p, va, size, 0x04);
    um_unlock(&p->lock);
    if (!ok) return 0;

    char title[128];
    utf16_to_ascii(p, gc.title, title, sizeof(title));

    g->used = true;
    g->proc = p;
    g->id = g_win_next++;
    g->bitmap = va;
    g->cw = cw; g->ch = ch;

    DesktopLock();
    GdiRect wa = WmWorkArea();
    int fx = gc.x, fy = gc.y;
    if (fx <= 0) fx = wa.x + (wa.w - cw) / 2;
    if (fy <= 0) fy = wa.y + (wa.h - (ch + WM_TITLEBAR_H)) / 2;
    GdiRect frame = RECT(fx, fy, cw, ch + WM_TITLEBAR_H);
    WND *w = WmCreateWindow(title[0] ? title : "Program", frame, WS_OVERLAPPED,
                            GDI_C(0xF3, 0xF3, 0xF3), GDI_C(0x00, 0x78, 0xD4), gui_paint, g);
    if (w) {
        w->app = -1;
        w->on_key = gui_key;
        w->on_mouse = gui_mouse;
        w->on_close = gui_close;
        w->on_tick = gui_tick;
        g->wnd = w;
        WmSetActive(w);
    }
    DesktopUnlock();
    if (!w) { um_lock(&p->lock); um_decommit(p, va, size); um_region_remove(p, um_region_find(p, va)); um_unlock(&p->lock); g->used = false; return 0; }

    gc.hwnd = g->id;
    gc.bitmap = va;
    gc.stride = (UINT32)cw * 4;
    gc.cw = (UINT32)cw; gc.ch = (UINT32)ch;
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a1, &gc, sizeof(gc)))) return 0;
    /* Kick off the first paint */
    DesktopLock();
    if (g->wnd) enqueue(g, WM_PAINT, 0, 0, 0, 0);
    DesktopUnlock();
    return g->id;
}

/* NtNovaGuiGetMessage(hwnd_filter (0 = any), MSG *out, wait): 1 = got,
 * 0 = WM_QUIT, -1 = none (only when wait == 0). */
static UINT64 sys_gui_getmessage(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    for (;;) {
        GuiMsg out;
        bool got = false, quit = false;
        DesktopLock();
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            GuiWin *g = &g_win[i];
            if (!g->used || g->proc != p) continue;
            if (a1 && g->id != (UINT32)a1) continue;
            if (g->quit) { quit = true; continue; }
            if (g->head != g->tail) {
                out = g->q[g->tail % GUI_QUEUE];
                __atomic_store_n(&g->tail, g->tail + 1, __ATOMIC_RELEASE);
                got = true;
                break;
            }
        }
        DesktopUnlock();
        if (got) {
            if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, &out, sizeof(GuiMsg)))) return (UINT64)(INT64)-1;
            return 1;
        }
        if (quit) return 0;
        if (!a3) return (UINT64)(INT64)-1;
        if (um_stopping()) return 0;
        sched_yield();
    }
}

static UINT64 sys_gui_invalidate(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    if (g && g->wnd) { enqueue(g, WM_PAINT, 0, 0, 0, 0); WmInvalidate(); }
    DesktopUnlock();
    return 0;
}

static UINT64 sys_gui_settext(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    char title[128];
    utf16_to_ascii(p, a2, title, sizeof(title));
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    if (g && g->wnd) { WmSetTitle(g->wnd, title); WmInvalidate(); }
    DesktopUnlock();
    return 0;
}

static UINT64 sys_gui_show(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    if (g && g->wnd) { WmShowWindow(g->wnd, a2 != 0); if (a2) WmSetActive(g->wnd); WmInvalidate(); }
    DesktopUnlock();
    return 0;
}

static void destroy_window(GuiWin *g)
{
    UINT64 va = g->bitmap;
    UmProcess *p = g->proc;
    if (g->wnd) { WmDestroyWindow(g->wnd); g->wnd = NULL; }   /* on_close won't re-enter: user set below */
    if (p && !p->exited) {
        um_lock(&p->lock);
        UmRegion *r = um_region_find(p, va);
        if (r) { um_decommit(p, r->base, r->size); um_region_remove(p, r); }
        um_unlock(&p->lock);
    }
    g->used = false;
}

static UINT64 sys_gui_destroy(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    if (g) { if (g->wnd) g->wnd->user = NULL; destroy_window(g); WmInvalidate(); }
    DesktopUnlock();
    return 0;
}

static UINT64 sys_gui_settimer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    UINT64 rv = 0;
    if (g) {
        UINT32 period = (UINT32)a3 / 10;
        if (!period) period = 1;
        int slot = -1;
        for (int i = 0; i < GUI_TIMERS; i++) if (g->timers[i].used && g->timers[i].id == (UINT32)a2) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < GUI_TIMERS; i++) if (!g->timers[i].used) { slot = i; break; }
        if (slot >= 0) {
            g->timers[slot].used = true;
            g->timers[slot].id = (UINT32)a2;
            g->timers[slot].period = period;
            g->timers[slot].next = sched_ticks() + period;
            rv = a2;
        }
    }
    DesktopUnlock();
    return rv;
}

static UINT64 sys_gui_killtimer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    if (g) for (int i = 0; i < GUI_TIMERS; i++) if (g->timers[i].used && g->timers[i].id == (UINT32)a2) g->timers[i].used = false;
    DesktopUnlock();
    return 1;
}

/* NtNovaGuiMessageBox(text16, caption16, type): a simple modal box.
 * Returns the button pressed (1 = OK).  Draws itself and waits for a click
 * or a key; the desktop keeps compositing because we only yield. */
static UINT64 sys_gui_messagebox(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    char text[256], caption[128];
    utf16_to_ascii(p, a1, text, sizeof(text));
    utf16_to_ascii(p, a2, caption, sizeof(caption));
    kprintf("[UM] %s: MessageBox \"%s\": %s\n", p->name, caption, text);
    (void)a3;
    /* A full modal box needs its own window; for now report it and return
     * IDOK so programs continue.  (user32 draws in-window dialogs itself.) */
    return 1;
}

static UINT64 sys_gui_screensize(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    DesktopLock();
    UINT32 w = GdiScreenW(), h = GdiScreenH();
    GdiRect wa = WmWorkArea();
    DesktopUnlock();
    if (a1) CopyToUser((void *)(uintptr_t)a1, &w, 4);
    if (a2) CopyToUser((void *)(uintptr_t)a2, &h, 4);
    (void)wa;
    return ((UINT64)h << 32) | w;
}

static UINT64 sys_gui_postmessage(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_of_handle(p, a1);
    if (g) {
        if ((UINT32)a2 == WM_QUIT) g->quit = true;
        else enqueue(g, (UINT32)a2, a3, a4, 0, 0);
    }
    DesktopUnlock();
    return g ? 1 : 0;
}

/* Called from UmPoll when a process has exited: tear down its windows. */
void um_gui_process_gone(UmProcess *p)
{
    DesktopLock();
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        GuiWin *g = &g_win[i];
        if (g->used && g->proc == p) {
            if (g->wnd) { g->wnd->user = NULL; WmDestroyWindow(g->wnd); g->wnd = NULL; }
            g->used = false;
        }
    }
    WmInvalidate();
    DesktopUnlock();
}

void um_gui_syscalls_init(void)
{
    um_install(SYSCALL_NtNovaGuiCreate,      sys_gui_create);
    um_install(SYSCALL_NtNovaGuiGetMessage,  sys_gui_getmessage);
    um_install(SYSCALL_NtNovaGuiInvalidate,  sys_gui_invalidate);
    um_install(SYSCALL_NtNovaGuiSetText,     sys_gui_settext);
    um_install(SYSCALL_NtNovaGuiShow,        sys_gui_show);
    um_install(SYSCALL_NtNovaGuiDestroy,     sys_gui_destroy);
    um_install(SYSCALL_NtNovaGuiSetTimer,    sys_gui_settimer);
    um_install(SYSCALL_NtNovaGuiKillTimer,   sys_gui_killtimer);
    um_install(SYSCALL_NtNovaGuiMessageBox,  sys_gui_messagebox);
    um_install(SYSCALL_NtNovaGuiScreenSize,  sys_gui_screensize);
    um_install(SYSCALL_NtNovaGuiPostMessage, sys_gui_postmessage);
}
