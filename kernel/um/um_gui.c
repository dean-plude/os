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
 * every WM touch from a program thread takes DesktopLock first.  Message
 * queues have a spinlock of their own (g_gui_lock), so GetMessage,
 * PostMessage, InvalidateRect and timers never wait for a frame to be
 * drawn.  All of these run without the big kernel lock (smp.h).
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../wm/wm.h"
#include "../gdi/gdi.h"
#include "../gdi/syscursor.h"
#include "../gdi/font.h"
#include "../apps/apps.h"
#include "../ke/waitq.h"
#include "../wm/desktop.h"
#include "../wm/tablet.h"
#include "../drivers/gamepad.h"
#include "../wm/kbdlayout.h"
#include "../hal/display.h"
#include "../hal/pci.h"

/* Win32 window messages we deliver */
#define WM_DESTROY        0x0002
#define WM_MOVE           0x0003
#define WM_SIZE           0x0005
#define WM_ACTIVATE       0x0006
#define WM_PAINT          0x000F
#define WM_CLOSE          0x0010
#define WM_QUIT           0x0012
#define WM_KEYDOWN        0x0100
#define WM_KEYUP          0x0101
#define WM_CHAR           0x0102
#define WM_SYSKEYDOWN     0x0104
#define WM_SYSKEYUP       0x0105
#define WM_SYSCHAR        0x0106
#define WM_TIMER          0x0113
#define WM_MOUSEMOVE      0x0200
#define WM_LBUTTONDOWN    0x0201
#define WM_LBUTTONUP      0x0202
#define WM_LBUTTONDBLCLK  0x0203
#define WM_RBUTTONDOWN    0x0204
#define WM_RBUTTONUP      0x0205
#define WM_MBUTTONDOWN    0x0207
#define WM_MBUTTONUP      0x0208
#define WM_MOUSEWHEEL     0x020A
#define WM_XBUTTONDOWN    0x020B
#define WM_XBUTTONUP      0x020C
#define WM_MOUSEHWHEEL    0x020E
#define WM_MOUSELEAVE     0x02A3
#define WM_KEYFIRST       0x0100
#define WM_KEYLAST        0x0109
#define WM_MOUSEFIRST     0x0200
#define WM_MOUSELAST      0x020E
#define WM_NOVA_TOUCH     0x03FD   /* user32's u32.h: a touch contact (gui_touch) */

#define GUI_MAX_WINDOWS   64
#define GUI_QUEUE         256
#define GUI_TIMERS        16
#define GUI_MAX_W         2560
#define GUI_MAX_H         1600
#define GUI_BITMAP_VA     UINT64_C(0x00007FF900000000)
#define GUI_BITMAP_STRIDE UINT64_C(0x01000000)          /* 16 MiB apart */

/* GuiCreate.flags */
#define GUI_POPUP       0x01    /* menus, drop-downs: no frame, above everything, no focus */
#define GUI_RESIZABLE   0x02    /* the user can resize it (the client size changes) */
#define GUI_NOMINMAX    0x04    /* dialogs: only a close button */
#define GUI_NOACTIVATE  0x08    /* shown without taking the focus */
#define GUI_HIDDEN      0x10    /* created hidden (shown by NtNovaGuiCtl) */
#define GUI_NOCLOSE     0x20    /* no close button */
#define GUI_HOVER       0x40    /* gets mouse moves with no button held */
#define GUI_NOFRAME     0x80    /* no title bar or border (the program draws its own), but a normal window */
#define WM_NOVA_DPI     0x03FC  /* user32's u32.h: a monitor's DPI changed (CTL_SET_DPI) */

/* Matches the Win32 MSG structure byte-for-byte */
typedef struct {
    UINT64 hwnd;
    UINT32 message;
    UINT32 _pad;                    /* mouse messages: the pen packet behind it (UmSetInputPen) */
    UINT64 wParam;
    UINT64 lParam;
    UINT32 time;
    INT32  pt_x, pt_y;
} GuiMsg;

typedef struct {
    bool        used;
    UmProcess  *proc;
    UINT32      tid;                /* the thread whose queue gets its messages */
    WND        *wnd;
    UINT32      id;                 /* handle value the program sees */
    UINT32      hwnd;               /* user32's HWND for it (CTL_SET_HWND), seen by other processes */
    INT32       uc[4];              /* user32's client area: offset in the frame, size (uc[2] 0: unknown) */
    UINT32      flags;              /* GUI_* */
    UINT64      bitmap;             /* user VA of the client bitmap */
    int         stride;             /* pixels per bitmap row (the largest width) */
    int         maxw, maxh;         /* the largest client area, logical px (pmaxw / scale) */
    int         pmaxw, pmaxh;       /* the bitmap's size in pixels */
    int         lmaxw, lmaxh;       /* the largest monitor's size, logical px (maxw at scale 1) */
    int         scale;              /* bitmap pixels per logical pixel: 1, or 2 for a DPI-aware
                                       program's window on a 192 DPI monitor (CTL_SET_SCALE) */
    volatile int cw, ch;            /* client size (logical px) */
    /* what the program was last told (gui_tick reports changes) */
    bool        was_active, was_min, was_max;
    int         last_x, last_y;
    GuiMsg      q[GUI_QUEUE];
    volatile UINT32 head, tail;
    bool        quit;
    struct { UINT32 id; UINT32 period; UINT64 next; bool used; } timers[GUI_TIMERS];
    UINT32      accept;             /* drops it takes: 1 files (WM_DROPFILES), 2 an OLE drop target */
    void       *drop;               /* a drop delivered and not yet fetched (kmalloc) */
    UINT32      drop_len;
    UINT32      drop_seq;           /* the drop delivered here that the source waits on (0: none) */
    /* Embedded in another process's window (CTL_EMBED: WebView2's host
     * parents the browser process's window in its own, as SetParent does
     * across processes on Windows).  The WND stays this process's own,
     * frameless, owned by the host's and kept over the parent's client area */
    UINT32      host;               /* the host's window id (0: a top-level window) */
    UINT32      parent_hwnd;        /* the host's user32 handle of the parent window */
    INT32       pox, poy;           /* the parent's client origin in the host's client area */
    INT32       clip[4];            /* the parent's visible client area (left, top, right, bottom), host client coordinates */
    bool        parent_vis;         /* the parent and its ancestors show */
    bool        want_vis;           /* the program's own ShowWindow */
    INT32       rx, ry;             /* its client origin in the parent's client area */
    INT32       src_x, src_y;       /* the part clipped off at the left and the top (logical px) */
    UINT32      saved_style;        /* the WND's own frame and flags, for when it is let go */
    bool        saved_fixed, saved_noact, saved_popup;
    int         saved_owner;
    UINT32      key_embed;          /* (a host) the embedded window that has the keyboard */
    UINT64      back;               /* user32's back buffer (CTL_SET_BACK): CTL_XBLIT writes it with the bitmap */
    int         back_stride;        /* its pixels per row */
} GuiWin;

/* The table's slots (used, proc, id), the message queues, the quit flags
 * and the timers are under g_gui_lock, a spinlock: a program's message
 * loop never waits for the desktop to finish drawing a frame.  The WND
 * side (wnd, bitmap, size, anything that reaches the window manager) is
 * under DesktopLock.  g_guiq wakes programs waiting for messages. */
static GuiWin g_win[GUI_MAX_WINDOWS];
static UINT32 g_win_next = 1;
static KSpinLock g_gui_lock = KSPINLOCK_INIT;
static WaitQueue g_guiq = WAITQ_INIT;
static UINT8 g_keydown[256];        /* keys held (auto-repeat sets lParam bit 30) */

static GuiWin *win_of_handle(UmProcess *p, UINT64 h)
{
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].proc == p && g_win[i].id == (UINT32)h) return &g_win[i];
    return NULL;
}

/* The same, taking g_gui_lock (for callers under DesktopLock, which keeps
 * the slot from being freed while they use it) */
static GuiWin *win_lookup(UmProcess *p, UINT64 h)
{
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    GuiWin *g = win_of_handle(p, h);
    spin_unlock_irqrestore(&g_gui_lock, s);
    return g;
}

/* -----------------------------------------------------------------------
 * Message queue (g_gui_lock)
 * ----------------------------------------------------------------------- */
/* A pen's packet number for the mouse messages going out now (UmSetInputPen) */
static UINT32 g_input_pen;

void UmSetInputPen(UINT32 serial) { g_input_pen = serial; }

static void enqueue_locked(GuiWin *g, UINT32 msg, UINT64 wp, UINT64 lp, int x, int y)
{
    if (g->head - g->tail >= GUI_QUEUE) return;             /* full: drop */
    UINT32 pen = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST ? g_input_pen : 0;
    /* Coalesce consecutive paints, sizes and mouse moves */
    if ((msg == WM_PAINT || msg == WM_MOUSEMOVE || msg == WM_SIZE || msg == WM_MOVE) && g->head != g->tail) {
        GuiMsg *last = &g->q[(g->head - 1) % GUI_QUEUE];
        if (last->message == msg) { last->wParam = wp; last->lParam = lp; last->pt_x = x; last->pt_y = y; last->_pad = pen; return; }
    }
    GuiMsg *m = &g->q[g->head % GUI_QUEUE];
    m->hwnd = g->id;
    m->message = msg;
    m->_pad = pen;                                          /* (the pen's packet: user32 reads it) */
    m->wParam = wp;
    m->lParam = lp;
    m->time = (UINT32)(sched_ticks() * 10);
    m->pt_x = x; m->pt_y = y;
    __atomic_store_n(&g->head, g->head + 1, __ATOMIC_RELEASE);
}

/* Wake the threads waiting for messages; the one @tid (0: all of them)
 * gets win32k's windowing boost, +2, for every message, keyboard and
 * mouse input included.  (NT's +6 for keyboard and mouse is the I/O
 * increment a driver gives the thread reading the device: the device
 * poll thread here.  Given to a window's thread, it lifted a foreground
 * program's NORMAL thread to 15 with the foreground boost on top, level
 * with the TIME_CRITICAL threads that record and play sound.) */
static void gui_wake(UINT32 tid, UINT32 msg)
{
    (void)msg;
    waitq_wake_boost(&g_guiq, BOOST_GUI, tid);
}

static void enqueue(GuiWin *g, UINT32 msg, UINT64 wp, UINT64 lp, int x, int y)
{
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    if (g->used) enqueue_locked(g, msg, wp, lp, x, y);
    spin_unlock_irqrestore(&g_gui_lock, s);
    gui_wake(g->tid, msg);
}

static UINT64 packxy(int x, int y) { return ((UINT64)(UINT16)y << 16) | (UINT16)x; }

static GuiWin *win_by_id(UINT32 id)             /* any process's; under g_gui_lock */
{
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].id == id) return &g_win[i];
    return NULL;
}

/* -----------------------------------------------------------------------
 * Windows embedded in another process's window (CTL_EMBED).  DesktopLock
 * held throughout: it keeps the slots from being freed
 * ----------------------------------------------------------------------- */
#define WM_NOVA_EMBED   0x03F4  /* user32's u32.h: the window was embedded (wParam: the parent) or let go (0) */
#define EMBED_PENDING   16

/* Embeds asked for before the window had a desktop window: they take
 * effect at its CTL_SET_HWND (a hidden window gets one when first shown) */
static struct { UINT32 hwnd, host; INT32 in[8]; } g_embed_pending[EMBED_PENDING];

static GdiRect frame_for(UINT32 style, GdiRect c);

static GuiWin *embed_host(GuiWin *g)
{
    if (!g->host) return NULL;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    GuiWin *h = win_by_id(g->host);
    spin_unlock_irqrestore(&g_gui_lock, s);
    return h && h->wnd && h->proc && !h->proc->exited ? h : NULL;
}

/* Where the client area is on screen: an embedded window's whole client
 * area (its WND shows only the part inside the parent) */
static void client_pos(GuiWin *g, int *x, int *y)
{
    GuiWin *h = embed_host(g);
    if (h) {
        GdiRect hc = WmClientRect(h->wnd);
        *x = hc.x + g->pox + g->rx; *y = hc.y + g->poy + g->ry;
        return;
    }
    GdiRect c = WmClientRect(g->wnd);
    *x = c.x; *y = c.y;
}

/* Put an embedded window where its host has it: over the parent's client
 * area and clipped to it, shown while the program, the parent and the
 * host's window all show it */
static void embed_place(GuiWin *g)
{
    WND *w = g->wnd;
    GuiWin *h = embed_host(g);
    if (!w || !h) return;
    GdiRect hc = WmClientRect(h->wnd);
    int x = hc.x + g->pox + g->rx, y = hc.y + g->poy + g->ry;
    int cl = hc.x + g->clip[0], ct = hc.y + g->clip[1], cr = hc.x + g->clip[2], cb = hc.y + g->clip[3];
    if (cl < hc.x) cl = hc.x;
    if (ct < hc.y) ct = hc.y;
    if (cr > hc.x + hc.w) cr = hc.x + hc.w;
    if (cb > hc.y + hc.h) cb = hc.y + hc.h;
    int l = x > cl ? x : cl, t = y > ct ? y : ct;
    int r = x + g->cw < cr ? x + g->cw : cr, b = y + g->ch < cb ? y + g->ch : cb;
    bool vis = g->want_vis && g->parent_vis && h->wnd->visible && !h->wnd->minimized && r > l && b > t;
    if (r <= l) r = l + 1;
    if (b <= t) b = t + 1;
    g->src_x = l - x; g->src_y = t - y;
    GdiRect f = RECT(l, t, r - l, b - t);
    if (f.x != w->frame.x || f.y != w->frame.y || f.w != w->frame.w || f.h != w->frame.h) WmSetFrame(w, f);
    if (w->visible != vis) {
        WmShowWindow(w, vis);
        if (!vis && WmGetCapture() == w) WmSetCapture(NULL);
    }
}

/* Embed @g in @h's window; in: { op, parent's client origin x, y in h's
 * client area, its visible client area l, t, r, b, it shows } */
static void embed_apply(GuiWin *g, GuiWin *h, UINT32 parent, const INT32 in[8])
{
    WND *w = g->wnd;
    if (!w || !h->wnd || g == h) return;
    bool first = !g->host;
    if (first) {
        g->saved_style = w->style;
        g->saved_fixed = w->fixed_size;
        g->saved_noact = w->no_activate;
        g->saved_popup = w->popup;
        g->saved_owner = w->owner;
        g->want_vis = w->visible;
        /* the numbers of its window rectangle are its place in the parent
         * now, as Windows' SetParent keeps them */
        g->rx = w->frame.x; g->ry = w->frame.y;
        bool was_active = w->active;
        w->style = 0;
        w->fixed_size = true;
        w->no_activate = true;
        w->popup = false;                       /* (a tool window's: above every window, not just its host) */
        w->maximized = w->snapped = false;
        w->active = false;
        if (was_active) WmSetActive(h->wnd);
    }
    g->host = h->id;
    g->parent_hwnd = parent;
    g->pox = in[1]; g->poy = in[2];
    memcpy(g->clip, in + 3, sizeof(g->clip));
    g->parent_vis = in[7] != 0;
    w->owner = h->wnd->id;
    WmKeepAbove(w, h->wnd);
    embed_place(g);
    if (first) enqueue(g, WM_NOVA_EMBED, parent, 0, 0, 0);
    WmInvalidate();
}

/* Back to a top-level window of its own, where its numbers say (Windows'
 * SetParent(NULL) keeps them too) */
static void embed_release(GuiWin *g)
{
    WND *w = g->wnd;
    GuiWin *h = embed_host(g);
    if (h && h->key_embed == g->id) h->key_embed = 0;
    g->host = 0;
    g->src_x = g->src_y = 0;
    if (!w) return;
    w->style = g->saved_style;
    w->fixed_size = g->saved_fixed;
    w->no_activate = g->saved_noact;
    w->popup = g->saved_popup;
    w->owner = g->saved_owner;
    WmSetFrame(w, frame_for(w->style, RECT(g->rx, g->ry, g->cw, g->ch)));
    WmShowWindow(w, g->want_vis);
    enqueue(g, WM_NOVA_EMBED, 0, 0, 0, 0);
    WmInvalidate();
}

/* -----------------------------------------------------------------------
 * WM callbacks (desktop thread, DesktopLock held; the queue side takes
 * g_gui_lock itself)
 * ----------------------------------------------------------------------- */
static void gui_paint(WND *w)
{
    GuiWin *g = w->user;
    if (!g || !g->proc || g->proc->exited) return;
    GdiRect cr = WmClientRect(w);
    int k = g->scale > 1 ? g->scale : 1;
    int sx = g->host ? g->src_x : 0, sy = g->host ? g->src_y : 0;   /* (an embedded window clipped by its parent) */
    int w_px = cr.w < g->cw - sx ? cr.w : g->cw - sx;
    int h_px = cr.h < g->ch - sy ? cr.h : g->ch - sy;
    if ((w_px + sx) * k > g->pmaxw) w_px = g->pmaxw / k - sx;
    if ((h_px + sy) * k > g->pmaxh) h_px = g->pmaxh / k - sy;
    static UINT32 row[GUI_MAX_W * GDI_MAX_SCALE];   /* desktop thread only: k bitmap rows */
    for (int y = 0; y < h_px; y++) {
        bool ok = true;
        for (int j = 0; j < k && ok; j++)
            ok = um_read(g->proc, g->bitmap + (((UINT64)(y + sy) * k + j) * g->stride + (UINT64)sx * k) * 4,
                         row + j * w_px * k, (UINT64)w_px * k * 4);
        if (!ok) break;
        GdiBlitBGRAScaled(RECT(cr.x, cr.y + y, w_px, 1), row, w_px * k, k);
    }
}

/* The foreground process: the one whose window is active (an owned
 * dialog's, a modal one's), which the scheduler gives NT's foreground
 * boost (scheduler.h, BOOST_FOREGROUND).  Not a process of the IDLE class,
 * as on Windows.  While a Terminal is active, the console program running
 * in it (a console's programs are foreground while their console window
 * is, on Windows); none while another built-in app (Settings, File
 * Explorer) or the desktop itself is active.  Desktop thread, DesktopLock held: called
 * every pass of its loop (each tick), so it follows every way the active
 * window changes (a click, Alt+Tab, a window closed or minimized). */
static UmProcess *active_process(void)
{
    WND *w = WmActiveWindow();
    if (w && w->on_paint == gui_paint && w->user) return ((GuiWin *)w->user)->proc;
    return TerminalProgram(w);                  /* a console program while its Terminal is active */
}

static UINT32 g_clip_pid;                       /* the process whose ClipCursor holds (0: none) */

/* A confined pointer is let go once its process leaves the foreground
 * (@p: the active window's process).  Desktop lock. */
static void clip_follow(UmProcess *p)
{
    if (g_clip_pid && (!p || p->exited || p->pid != g_clip_pid)) { g_clip_pid = 0; WmCursorClip(NULL); }
}

void UmUpdateForeground(void)
{
    UmProcess *p = active_process();
    clip_follow(p);
    if (p && (p->exited || p->prio_class == 1 /* PROCESS_PRIORITY_CLASS_IDLE */)) p = NULL;
    if (sched_foreground() != p) sched_set_foreground(p);
}

/* Windows virtual-key code for a set-1 scan code (keypad keys as with
 * Num Lock off; E0-prefixed ones in the second table) */
UINT32 UmScancodeToVk(UINT8 sc, bool ext)
{
    UINT8 typing = ext ? 0 : KbdVk(sc);          /* the typing keys: the user's layout's codes */
    if (typing) return typing;
    static const UINT8 base[0x59] = {
        0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 0x08, 0x09,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 0x0D, 0x11, 'A', 'S',
        'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0, 0x10, 0xDC, 'Z', 'X', 'C', 'V',
        'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0x10, 0x6A, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x24, 0x26, 0x21, 0x6D, 0x25, 0x0C, 0x27, 0x6B, 0x23,
        0x28, 0x22, 0x2D, 0x2E, 0, 0, 0xE2, 0x7A, 0x7B,
    };
    if (ext) {
        switch (sc) {
        case 0x1C: return 0x0D;                             /* keypad Enter */
        case 0x1D: return 0x11;                             /* right Ctrl */
        case 0x35: return 0x6F;                             /* keypad / */
        case 0x37: return 0x2C;                             /* Print Screen */
        case 0x38: return 0x12;                             /* right Alt */
        case 0x47: return 0x24; case 0x48: return 0x26; case 0x49: return 0x21;
        case 0x4B: return 0x25; case 0x4D: return 0x27; case 0x4F: return 0x23;
        case 0x50: return 0x28; case 0x51: return 0x22; case 0x52: return 0x2D; case 0x53: return 0x2E;
        case 0x5B: return 0x5B; case 0x5C: return 0x5C; case 0x5D: return 0x5D;
        /* media, browser and launch keys: VK_MEDIA_*, VK_VOLUME_*, VK_BROWSER_*, VK_LAUNCH_*, VK_SLEEP */
        case 0x19: return 0xB0; case 0x10: return 0xB1; case 0x24: return 0xB2; case 0x22: return 0xB3;
        case 0x20: return 0xAD; case 0x2E: return 0xAE; case 0x30: return 0xAF;
        case 0x6A: return 0xA6; case 0x69: return 0xA7; case 0x67: return 0xA8; case 0x68: return 0xA9;
        case 0x65: return 0xAA; case 0x66: return 0xAB; case 0x32: return 0xAC;
        case 0x6C: return 0xB4; case 0x6D: return 0xB5; case 0x6B: return 0xB6; case 0x21: return 0xB7;
        case 0x5F: return 0x5F;
        }
        return 0;
    }
    return sc < sizeof(base) ? base[sc] : 0;
}

static void gui_key(WND *w, const KeyEvent *k)
{
    GuiWin *g = w->user;
    if (!g) return;
    if (g->key_embed) {                         /* a window embedded in this one has the keyboard */
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *e = win_by_id(g->key_embed);
        spin_unlock_irqrestore(&g_gui_lock, s);
        if (e && e->host == g->id && e->wnd && e->wnd->visible) g = e;
        else g->key_embed = 0;
    }
    UINT32 vk = UmScancodeToVk(k->scancode, k->extended) & 0xFF;
    bool repeat = k->pressed && g_keydown[vk];
    g_keydown[vk] = k->pressed;
    /* lParam as in Win32: repeat count 1, scan code in bits 16-23, bit 24
     * for E0-prefixed (extended) keys, bit 29 with Alt, 30 if it was
     * already down (auto-repeat), 31 on release */
    UINT64 lp = 1 | ((UINT64)k->scancode << 16) | ((UINT64)(k->extended ? 1 : 0) << 24) |
                ((UINT64)(k->alt ? 1 : 0) << 29) | ((UINT64)(repeat ? 1 : 0) << 30);
    /* AltGr (right Alt in a layout with AltGr characters) comes with a left
     * Ctrl, as on Windows: programs see Ctrl+Alt, and ToUnicode the AltGr
     * characters */
    bool altgr_key = k->scancode == KEY_ALT && k->extended && KbdHasAltGr();
    if (altgr_key && k->pressed != g_keydown[0x11]) {
        g_keydown[0x11] = k->pressed;
        enqueue(g, k->pressed ? WM_KEYDOWN : WM_KEYUP, 0x11,
                1 | ((UINT64)KEY_CTRL << 16) | (k->pressed ? 0 : 3ull << 30), 0, 0);
    }
    /* Alt combinations and F10 are "system" keys (menus take them) */
    bool sys = !k->altgr && ((k->alt && !k->ctrl) || vk == 0x12 || vk == 0x79);
    if (k->pressed) {
        enqueue(g, sys ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp, 0, 0);
        UINT32 ch = k->wch;
        if (ch == '\n') ch = '\r';                          /* Enter is CR, as on Windows */
        if (vk == 0x1B) ch = 0x1B;
        if (k->ctrl && !k->alt) {                           /* Ctrl+letter: control characters */
            if (vk >= 'A' && vk <= 'Z') ch = vk - 'A' + 1;
            else if (vk == 0xDB) ch = 0x1B; else if (vk == 0xDD) ch = 0x1D; else if (vk == 0xDC) ch = 0x1C;
            else if (ch != '\r' && ch != 8 && ch != 9 && ch != 0x1B) ch = 0;
        }
        if (ch) enqueue(g, sys && k->alt ? WM_SYSCHAR : WM_CHAR, ch, lp, 0, 0);
    } else {
        enqueue(g, sys ? WM_SYSKEYUP : WM_KEYUP, vk, lp | (3ull << 30), 0, 0);
    }
}

/* MK_* flags for the buttons and modifiers held */
static UINT64 mk_flags(void)
{
    UINT32 b = WmButtons(), m = InputModifiers();
    return (b & 1 ? 0x01 : 0) | (b & 2 ? 0x02 : 0) | (b & 4 ? 0x10 : 0) | (b & 8 ? 0x20 : 0) | (b & 16 ? 0x40 : 0) |
           (m & 1 ? 0x04 : 0) | (m & 2 ? 0x08 : 0);
}

static void gui_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    GuiWin *g = w->user;
    if (!g) return;
    bool press = msg == WM_MOUSE_DOWN || msg == WM_MOUSE_RDOWN || msg == WM_MOUSE_MDOWN || msg == WM_MOUSE_XDOWN ||
                 msg == WM_MOUSE_DBLCLK;
    GuiWin *h = embed_host(g);
    if (h) {
        x += g->src_x; y += g->src_y;           /* (its client area, of which the WND shows a part) */
        if (press) {                            /* a click gives it the keyboard, and its host the focus */
            h->key_embed = g->id;
            if (!h->wnd->active) WmSetActive(h->wnd);
        }
    } else if (press) {
        g->key_embed = 0;
    }
    UINT64 lp = packxy(x, y), mk = mk_flags();
    switch (msg) {
    case WM_MOUSE_DOWN:   enqueue(g, WM_LBUTTONDOWN, mk | 1, lp, x, y); break;
    case WM_MOUSE_UP:     enqueue(g, WM_LBUTTONUP, mk & ~1ull, lp, x, y); break;
    case WM_MOUSE_MOVE:   enqueue(g, WM_MOUSEMOVE, mk, lp, x, y); break;
    case WM_MOUSE_DBLCLK: enqueue(g, WM_LBUTTONDBLCLK, mk | 1, lp, x, y); break;
    case WM_MOUSE_RDOWN:  enqueue(g, WM_RBUTTONDOWN, mk | 2, lp, x, y); break;
    case WM_MOUSE_RUP:    enqueue(g, WM_RBUTTONUP, mk & ~2ull, lp, x, y); break;
    case WM_MOUSE_MDOWN:  enqueue(g, WM_MBUTTONDOWN, mk | 0x10, lp, x, y); break;
    case WM_MOUSE_MUP:    enqueue(g, WM_MBUTTONUP, mk & ~0x10ull, lp, x, y); break;
    case WM_MOUSE_WHEEL: {                                  /* wheel: screen coordinates */
        GdiRect c = WmClientRect(w);
        UINT64 d = (UINT64)(UINT16)(INT16)(WmWheelDelta() * 120);
        enqueue(g, WM_MOUSEWHEEL, (d << 16) | mk, packxy(x + c.x, y + c.y), x, y);
        break;
    }
    case WM_MOUSE_HWHEEL: {                                 /* + is to the right, as on Windows */
        GdiRect c = WmClientRect(w);
        UINT64 d = (UINT64)(UINT16)(INT16)(WmWheelDelta() * 120);
        enqueue(g, WM_MOUSEHWHEEL, (d << 16) | mk, packxy(x + c.x, y + c.y), x, y);
        break;
    }
    case WM_MOUSE_XDOWN:                                    /* HIWORD(wParam): XBUTTON1 or 2 */
        enqueue(g, WM_XBUTTONDOWN, ((UINT64)WmWheelDelta() << 16) | mk, lp, x, y);
        break;
    case WM_MOUSE_XUP:
        enqueue(g, WM_XBUTTONUP, ((UINT64)WmWheelDelta() << 16) | mk, lp, x, y);
        break;
    case WM_MOUSE_LEAVE:  enqueue(g, WM_MOUSELEAVE, 0, 0, 0, 0); break;
    }
}

/* Touch contacts that went down in the client area: one WM_NOVA_TOUCH per
 * contact, wParam = slot | WM_TOUCH_* << 8 | 0x10000 on the frame's last,
 * lParam = its screen position; user32 makes WM_POINTER* or WM_TOUCH */
static void gui_touch(WND *w, const WmTouch *t, int n)
{
    GuiWin *g = w->user;
    if (!g) return;
    GdiRect c = WmClientRect(w);
    for (int i = 0; i < n; i++)
        enqueue(g, WM_NOVA_TOUCH, (UINT64)t[i].id | ((UINT64)t[i].flags << 8) | (i == n - 1 ? 0x10000ull : 0),
                packxy(t[i].x, t[i].y), t[i].x - c.x, t[i].y - c.y);
}

/* Every pass of the desktop loop: timers, and what the user did to the
 * window (resized, moved, focused, minimized) */
static bool gui_tick(WND *w)
{
    GuiWin *g = w->user;
    if (!g) return false;
    UINT64 now = sched_ticks();
    bool any = false;

    if (g->host) {                              /* embedded: follow the host's window, or go back to the desktop */
        if (embed_host(g)) embed_place(g);
        else embed_release(g);
    }
    GdiRect cr = WmClientRect(w);
    if (!g->host && !w->minimized && (g->flags & GUI_RESIZABLE)) {
        int nw = cr.w < 1 ? 1 : cr.w > g->maxw ? g->maxw : cr.w;
        int nh = cr.h < 1 ? 1 : cr.h > g->maxh ? g->maxh : cr.h;
        if (nw != g->cw || nh != g->ch || w->maximized != g->was_max) {
            g->cw = nw; g->ch = nh;
            g->was_max = w->maximized;
            enqueue(g, WM_SIZE, w->maximized ? 2 : 0, packxy(nw, nh), 0, 0);
            any = true;
        }
    }
    if (w->minimized != g->was_min) {
        g->was_min = w->minimized;
        enqueue(g, WM_SIZE, w->minimized ? 1 : w->maximized ? 2 : 0, packxy(g->cw, g->ch), 0, 0);
        any = true;
    }
    int cx, cy;
    client_pos(g, &cx, &cy);
    if (cx != g->last_x || cy != g->last_y) {
        g->last_x = cx; g->last_y = cy;
        enqueue(g, WM_MOVE, 0, packxy(cx, cy), 0, 0);
        any = true;
    }
    bool active = w->active && w->visible && !w->minimized;
    if (g->host) {                              /* embedded: while its host is active and it has the keyboard */
        GuiWin *h = embed_host(g);
        active = h && h->wnd->active && h->key_embed == g->id && w->visible;
    }
    if (active != g->was_active) {
        g->was_active = active;
        enqueue(g, WM_ACTIVATE, active ? 1 : 0, 0, 0, 0);
        any = true;
    }

    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = 0; i < GUI_TIMERS; i++)
        if (g->timers[i].used && now >= g->timers[i].next) {
            enqueue_locked(g, WM_TIMER, g->timers[i].id, 0, 0, 0);
            g->timers[i].next = now + g->timers[i].period;
            any = true;
        }
    spin_unlock_irqrestore(&g_gui_lock, s);
    if (any) gui_wake(g->tid, 0);
    return false;
}

/* The close button or Alt+F4: ask the program (it may refuse, or ask the
 * user first); DestroyWindow closes it */
static void gui_close_request(WND *w)
{
    GuiWin *g = w->user;
    if (g) enqueue(g, WM_CLOSE, 0, 0, 0, 0);
}

static void gui_close(WND *w)
{
    GuiWin *g = w->user;
    if (!g) return;
    /* The window is going away without the program (the process ended, or
     * the desktop closed it): detach; the program's Destroy frees the slot. */
    enqueue(g, WM_CLOSE, 0, 0, 0, 0);
    g->wnd = NULL;
    w->user = NULL;
}

/* -----------------------------------------------------------------------
 * Syscalls
 * ----------------------------------------------------------------------- */
/* In/out struct at the pointer passed to NtNovaGuiCreate */
typedef struct {
    INT32  x, y, w, h;              /* client area: screen position (INT32_MIN: centred) and size */
    UINT32 style;                   /* the thread of this process that gets its messages (0: the caller) */
    UINT64 title;                   /* UTF-16 title */
    /* out: */
    UINT64 hwnd;
    UINT64 bitmap;
    UINT32 stride;                  /* bytes per row */
    UINT32 cw, ch;
    /* in: */
    UINT32 flags;                   /* GUI_* */
    UINT64 owner;                   /* handle of the owner window (0: none) */
    /* out: */
    UINT32 rows;                    /* the bitmap's height in pixels (its width: stride / 4) */
    UINT32 _pad;
} GuiCreate;

/* The desktop's fonts cover ASCII only: punctuation outside it gets its
 * nearest ASCII form, as Windows' best-fit code pages do ("Page — Mozilla
 * Firefox" shows as "Page - Mozilla Firefox"); anything else is '?' */
static const char *ascii_fit(UINT16 c)
{
    if (c >= 0x2010 && c <= 0x2015) return "-";         /* hyphens, en and em dashes */
    switch (c) {
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2022: case 0x00B7: return "*";
    case 0x2026: return "...";
    case 0x2212: return "-";
    }
    return "?";
}

static void utf16_to_ascii(UmProcess *p, UINT64 va, char *out, int cap)
{
    out[0] = '\0';
    if (!va) return;
    UINT16 w;
    int n = 0;
    for (int i = 0; n < cap - 1 && i < 128; i++) {
        if (!um_read(p, va + (UINT64)i * 2, &w, 2) || !w) break;
        if (w < 0x80) { out[n++] = (char)w; continue; }
        for (const char *f = ascii_fit(w); *f && n < cap - 1; f++) out[n++] = *f;
    }
    out[n] = '\0';
}

/* The frame (outer rectangle) of a window of @style whose client area is @c */
static GdiRect frame_for(UINT32 style, GdiRect c)
{
    int top = (style & WS_TITLEBAR) ? WM_TITLEBAR_H : 0;
    int b   = (style & WS_BORDER) ? 1 : 0;
    return RECT(c.x - b, c.y - top, c.w + 2 * b, c.h + top + b);
}

/* Room for two bitmap pixels per logical pixel (CTL_SET_SCALE 2): a new,
 * larger bitmap (under DesktopLock: gui_paint reads it).  64-bit programs
 * keep the address (each window's slot has GUI_BITMAP_STRIDE bytes). */
static bool gui_grow_bitmap(GuiWin *g)
{
    int pw = g->lmaxw * GDI_MAX_SCALE > GUI_MAX_W ? GUI_MAX_W : g->lmaxw * GDI_MAX_SCALE;
    int ph = g->lmaxh * GDI_MAX_SCALE > GUI_MAX_H ? GUI_MAX_H : g->lmaxh * GDI_MAX_SCALE;
    if (g->pmaxw >= pw && g->pmaxh >= ph) return true;
    UmProcess *p = g->proc;
    if (!p || p->exited) return false;
    UINT64 size = ((UINT64)pw * ph * 4 + 0xFFF) & ~0xFFFULL, va = g->bitmap;
    bool ok;
    um_lock_excl(&p->lock);
    UmRegion *r = um_region_find(p, va);
    if (p->wow) {                                     /* elsewhere below 2 GiB, then let the old one go */
        va = um_find_free(p, size, p->lay.alloc_min, p->lay.alloc_max);
        ok = va && um_is_free(p, va, size) && um_region_add(p, va, size, 0x04, false) && um_commit(p, va, size, 0x04);
        if (ok && r) { um_decommit(p, r->base, r->size); um_region_remove(p, r); }
    } else {
        UINT64 old = r ? r->size : 0;
        if (r) { um_decommit(p, r->base, r->size); um_region_remove(p, r); }
        ok = size <= GUI_BITMAP_STRIDE && um_is_free(p, va, size) && um_region_add(p, va, size, 0x04, false) &&
             um_commit(p, va, size, 0x04);
        if (!ok && old) {                             /* (the old one back) */
            UmRegion *n = um_region_find(p, va);
            if (n) um_region_remove(p, n);
            if (um_region_add(p, va, old, 0x04, false)) um_commit(p, va, old, 0x04);
        }
    }
    um_unlock_excl(&p->lock);
    if (!ok) return false;
    g->bitmap = va;
    g->stride = g->pmaxw = pw;
    g->pmaxh = ph;
    return true;
}

static UINT64 sys_gui_create(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    UmThread *t = UmCurrentThread();
    GuiCreate gc;
    if (!NT_SUCCESS(CopyFromUser(&gc, (const void *)(uintptr_t)a1, sizeof(gc)))) return 0;
    int maxw = GdiScreenW(), maxh = GdiScreenH();
    for (int i = 1; i < GdiMonitorCount(); i++) {     /* as large as the largest monitor */
        GdiRect m = GdiMonitorRect(i);
        if (m.w > maxw) maxw = m.w;
        if (m.h > maxh) maxh = m.h;
    }
    if (maxw > GUI_MAX_W) maxw = GUI_MAX_W;
    if (maxh > GUI_MAX_H) maxh = GUI_MAX_H;
    int pmaxw = maxw, pmaxh = maxh;           /* (CTL_SET_SCALE 2 makes it larger) */
    int cw = gc.w, ch = gc.h;
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if (cw > maxw) cw = maxw; if (ch > maxh) ch = maxh;

    int slot = -1;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) if (!g_win[i].used) { slot = i; break; }
    GuiWin *g = slot >= 0 ? &g_win[slot] : NULL;
    if (g) {
        memset(g, 0, sizeof(*g));
        g->used = true;                     /* reserved; no window yet */
        g->proc = p;
        g->tid = gc.style ? gc.style : t ? t->tid : 0;     /* the thread whose queue gets its input */
        g->id = g_win_next++;
        g->flags = gc.flags;
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    if (!g) return 0;

    /* The client bitmap in the program's address space, as large as the
     * screen so resizing never moves it (pages are backed when touched) */
    UINT64 va = GUI_BITMAP_VA + (UINT64)slot * GUI_BITMAP_STRIDE;
    UINT64 size = ((UINT64)pmaxw * pmaxh * 4 + 0xFFF) & ~0xFFFULL;
    um_lock_excl(&p->lock);
    if (p->wow) va = um_find_free(p, size, p->lay.alloc_min, p->lay.alloc_max);   /* below 2 GiB */
    bool ok = va && um_is_free(p, va, size) && um_region_add(p, va, size, 0x04, false) &&
              um_commit(p, va, size, 0x04);
    um_unlock_excl(&p->lock);
    if (!ok) { s = spin_lock_irqsave(&g_gui_lock); g->used = false; spin_unlock_irqrestore(&g_gui_lock, s); return 0; }

    char title[128];
    utf16_to_ascii(p, gc.title, title, sizeof(title));

    DesktopLock();
    g->bitmap = va;
    g->stride = pmaxw;
    g->maxw = maxw; g->maxh = maxh;
    g->pmaxw = pmaxw; g->pmaxh = pmaxh;
    g->lmaxw = maxw; g->lmaxh = maxh;
    g->scale = 1;
    g->cw = cw; g->ch = ch;
    GdiRect wa = WmWorkArea();
    bool popup = (gc.flags & GUI_POPUP) != 0;
    UINT32 style = popup || (gc.flags & GUI_NOFRAME) ? WS_SHADOW :
                   WS_TITLEBAR | WS_BORDER | WS_SHADOW | (gc.flags & GUI_NOCLOSE ? 0 : WS_CLOSEBTN) |
                   (gc.flags & GUI_NOMINMAX ? 0 : WS_MINMAXBTN);
    GdiRect client = RECT(gc.x, gc.y, cw, ch);
    GdiRect frame = frame_for(style, client);
    if (gc.x == INT32_MIN) frame.x = wa.x + (wa.w - frame.w) / 2;
    if (gc.y == INT32_MIN) frame.y = wa.y + (wa.h - frame.h) / 2;
    if (!popup && frame.y < wa.y) frame.y = wa.y;
    GuiWin *owner = gc.owner ? win_of_handle(p, gc.owner) : NULL;
    bool activate = !(gc.flags & (GUI_POPUP | GUI_NOACTIVATE | GUI_HIDDEN));
    WND *w = WmCreateWindowEx(title[0] ? title : popup ? "" : "Program", frame, style,
                              GDI_C(0xF3, 0xF3, 0xF3), GDI_C(0x00, 0x78, 0xD4), gui_paint, g, activate);
    if (w) {
        w->app = popup ? -1 : AppForProgram(p->name);   /* e.g. netsurf.exe -> its dock icon */
        if (!popup) {                       /* its icon: the program's own file */
            const char *path = p->name;
            for (int m = 0; m < p->nmodules; m++) if (!p->modules[m].dll && p->modules[m].path[0]) { path = p->modules[m].path; break; }
            strncpy(w->program, path, sizeof(w->program) - 1);
        }
        w->fixed_size = !(gc.flags & GUI_RESIZABLE);
        w->paint_lock_free = true;          /* gui_paint: the program's memory, under DesktopLock */
        w->popup = popup;
        w->no_activate = popup || (gc.flags & GUI_NOACTIVATE);
        w->hover = true;                    /* programs track the pointer (hot buttons, menus) */
        w->rbutton = true;
        w->owner = owner && owner->wnd ? owner->wnd->id : 0;
        w->on_key = gui_key;
        w->key_releases = true;             /* WM_KEYUP */
        w->on_mouse = gui_mouse;
        w->on_touch = popup ? NULL : gui_touch;   /* (menus: the touch works the mouse) */
        w->on_close = gui_close;
        w->on_close_request = gui_close_request;
        w->on_tick = gui_tick;
        w->tick_lock_free = true;           /* (messages only, no files) */
        if (gc.flags & GUI_HIDDEN) WmShowWindow(w, false);
        w->cursor = p->cursor;
        g->wnd = w;
        GdiRect c2 = WmClientRect(w);
        g->last_x = c2.x; g->last_y = c2.y;
        g->was_active = w->active;
    }
    DesktopUnlock();
    if (!w) {
        um_lock_excl(&p->lock); um_decommit(p, va, size); um_region_remove(p, um_region_find(p, va)); um_unlock_excl(&p->lock);
        s = spin_lock_irqsave(&g_gui_lock); g->used = false; spin_unlock_irqrestore(&g_gui_lock, s);
        return 0;
    }

    gc.hwnd = g->id;
    gc.bitmap = va;
    gc.stride = (UINT32)pmaxw * 4;
    gc.rows = (UINT32)pmaxh;
    gc.cw = (UINT32)cw; gc.ch = (UINT32)ch;
    if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a1, &gc, sizeof(gc)))) return 0;
    return g->id;
}

static bool xmsg_waiting(UmProcess *p);

/* Threads whose message wait was woken by another thread (NtNovaGuiCtl
 * CTL_WAKE: user32 posted to the thread's own queue) */
#define GUI_WAKES 32
static UINT32 g_wake_tid[GUI_WAKES];

/* NtNovaGuiGetMessage(hwnd_filter (0 = any of the calling thread's
 * windows), MSG *out, timeout: 0 = don't wait, 1 = wait forever, n >= 2 =
 * wait up to n - 2 ms): 1 = got, 0 = WM_QUIT, -1 = none, -2 = woken by
 * another thread. */
static UINT64 sys_gui_getmessage(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    UmThread *t = UmCurrentThread();
    UINT32 tid = t ? t->tid : 0;
    UINT64 until = a3 >= 2 ? sched_ticks() + (a3 - 2 + 9) / 10 : 0;
    for (;;) {
        GuiMsg out;
        bool got = false, quit = false, woken = false;
        UINT32 gen = waitq_gen(&g_guiq);
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        for (int i = 0; i < GUI_WAKES; i++)
            if (tid && g_wake_tid[i] == tid) { g_wake_tid[i] = 0; woken = true; }
        for (int i = 0; i < GUI_MAX_WINDOWS && !woken; i++) {
            GuiWin *g = &g_win[i];
            if (!g->used || g->proc != p) continue;
            if (a1 ? g->id != (UINT32)a1 : g->tid != tid) continue;
            if (g->quit) { quit = true; continue; }
            if (g->head != g->tail) {
                out = g->q[g->tail % GUI_QUEUE];
                __atomic_store_n(&g->tail, g->tail + 1, __ATOMIC_RELEASE);
                got = true;
                break;
            }
        }
        bool xmsg = !got && !woken && xmsg_waiting(p);
        spin_unlock_irqrestore(&g_gui_lock, s);
        if (woken) return (UINT64)(INT64)-2;
        if (xmsg) return (UINT64)(INT64)-3;
        if (got) {
            if (!NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, &out, sizeof(GuiMsg)))) return (UINT64)(INT64)-1;
            return 1;
        }
        if (quit) return 0;
        if (!a3) return (UINT64)(INT64)-1;
        if (um_stopping()) return 0;
        UINT64 now = sched_ticks();
        if (a3 >= 2 && now >= until) return (UINT64)(INT64)-1;
        UINT64 nap = a3 >= 2 && until - now < 10 ? until - now : 10;
        waitq_wait_tag(&g_guiq, gen, (UINT32)nap, tid);   /* until a message comes (or 100 ms) */
    }
}

static UINT64 sys_gui_invalidate(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    GuiWin *g = win_of_handle(p, a1);
    if (g) enqueue_locked(g, WM_PAINT, 0, 0, 0, 0);
    spin_unlock_irqrestore(&g_gui_lock, s);
    if (g) { WmInvalidate(); gui_wake(g->tid, WM_PAINT); }
    return 0;
}

static UINT64 sys_gui_settext(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    char title[128];
    utf16_to_ascii(p, a2, title, sizeof(title));
    DesktopLock();
    GuiWin *g = win_lookup(p, a1);
    if (g && g->wnd) { WmSetTitle(g->wnd, title); WmInvalidate(); }
    DesktopUnlock();
    return 0;
}

static UINT64 sys_gui_show(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_lookup(p, a1);
    if (g && g->wnd && g->host) {                   /* embedded: shown with its parent */
        g->want_vis = a2 != 0;
        embed_place(g);
    } else if (g && g->wnd) {
        WmShowWindow(g->wnd, a2 != 0);
        if (a2 && !g->wnd->no_activate) WmSetActive(g->wnd);
        WmInvalidate();
    }
    DesktopUnlock();
    return 0;
}

static void destroy_window(GuiWin *g)
{
    UINT64 va = g->bitmap;
    UmProcess *p = g->proc;
    if (g->wnd) {
        if (WmGetCapture() == g->wnd) WmSetCapture(NULL);
        WmDestroyWindow(g->wnd);            /* on_close won't re-enter: user set below */
        g->wnd = NULL;
    }
    if (p && !p->exited) {
        um_lock_excl(&p->lock);
        UmRegion *r = um_region_find(p, va);
        if (r) { um_decommit(p, r->base, r->size); um_region_remove(p, r); }
        um_unlock_excl(&p->lock);
    }
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    void *drop = g->drop;
    g->drop = NULL; g->drop_len = 0; g->accept = 0; g->drop_seq = 0;
    g->used = false;
    spin_unlock_irqrestore(&g_gui_lock, s);
    kfree(drop);
}

static UINT64 sys_gui_destroy(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    DesktopLock();
    GuiWin *g = win_lookup(p, a1);
    if (g) { if (g->wnd) g->wnd->user = NULL; destroy_window(g); WmInvalidate(); }
    DesktopUnlock();
    return 0;
}

static UINT64 sys_gui_settimer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    UmProcess *p = UmCurrent();
    IrqState s = spin_lock_irqsave(&g_gui_lock);
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
    spin_unlock_irqrestore(&g_gui_lock, s);
    return rv;
}

static UINT64 sys_gui_killtimer(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    GuiWin *g = win_of_handle(p, a1);
    if (g) for (int i = 0; i < GUI_TIMERS; i++) if (g->timers[i].used && g->timers[i].id == (UINT32)a2) g->timers[i].used = false;
    spin_unlock_irqrestore(&g_gui_lock, s);
    return 1;
}

/* NtNovaGuiCtl(hwnd, op, arg, ptr): window position, state, capture...
 *   1 GET_RECT  ptr -> { client x,y,w,h; frame x,y,w,h; state }
 *               state: 1 visible, 2 active, 4 minimized, 8 maximized
 *   2 SET_RECT  ptr <- { client x,y,w,h }, arg: 1 move, 2 size
 *   3 CAPTURE   arg: 1 take the mouse, 0 let it go
 *   4 CURSOR    ptr -> { screen x, y, buttons (MK_*) } (hwnd may be 0)
 *   5 ACTIVATE
 *   6 ENABLE    arg: 0 disabled (input goes to its owned windows), 1 enabled
 *   7 SHOW      arg: 0 hide, 1 show and activate, 2 minimize, 3 maximize,
 *               4 restore, 5 show without activating
 *   8 PRESENT   the bitmap changed: redraw the screen
 *   9 WORKAREA  ptr -> { x, y, w, h } (hwnd may be 0) */
#define CTL_GET_RECT 1
#define CTL_SET_RECT 2
#define CTL_CAPTURE  3
#define CTL_CURSOR   4
#define CTL_ACTIVATE 5
#define CTL_ENABLE   6
#define CTL_SHOW     7
#define CTL_PRESENT  8
#define CTL_WORKAREA 9
#define CTL_WAKE     10                 /* arg: a thread id of this process; its GetMessage returns -2 */
/* Drag and drop between programs (hwnd may be 0 for the first two):
 *  11 WINDOW_AT   ptr <- { screen x, y }, ptr -> { window id, pid, accept flags, 0 }
 *                 (id 0: the desktop or one of its own apps)
 *  12 ACCEPT_DROPS arg: the flags for this window
 *  13 DROP        arg: the target's window id; ptr <- { x, y, effect, bytes,
 *                 then the UTF-16 file list }: queued to the target as WM_NOVA_DROP
 *  14 DROP_FETCH  ptr -> { x, y, effect, source pid, bytes, the list }, arg: room in bytes
 *  17 DROP_DONE   the target has handled its drop; arg: the effect it took
 *  18 DROP_STATUS arg: the number DROP returned; 0 while the target is still
 *                 handling it, 1 + the effect once it has, ~0 if it never
 *                 will (its window is gone or another drop replaced it)
 * A drop is synchronous for the source, as on Windows: DoDragDrop returns
 * once the target's Drop has run, so a source may delete what it dropped
 * (7-Zip's temporary copies of files dragged out of an archive).
 * Display modes (hwnd may be 0):
 *  15 DISPLAY_MODE  arg: a mode index (0 = largest), -1 the current mode,
 *                   -2 the default (the user's) mode; ptr -> { width, height,
 *                   bits per pixel, frequency }.  0: no such mode
 *  16 SET_DISPLAY   ptr <- { width, height, CDS_* flags } (0 x 0: the default
 *                   mode); returns a DISP_CHANGE_* code
 * The pointer over this process's windows (hwnd may be 0):
 *  19 SET_CURSOR    arg 0: the arrow, 1: none (hidden), 2: ptr <- { w, h,
 *                   hot x, hot y, frames, steps }, then per step { frame,
 *                   jiffies (1/60 s) }, then frames * w * h 0xAARRGGBB pixels
 *                   (logical pixels, scaled up on a 2x display); 3: system
 *                   pointer ptr (an OCR_* number); 4: as 2, in device pixels
 *                   (an image made for the display's scale, up to 128 x 128)
 *  20 CURSOR_SHAPE  ptr -> { 1 if the pointer shows a program's shape,
 *                   its w, h, frames, the step shown }, and with arg 1 also
 *                   { the system pointer shown: OCR_*, -OCR_* for a
 *                   SetSystemCursor replacement, 0 none } (for tests)
 *  27 SET_SYSCURSOR arg: an OCR_* number (0: every one), | 0x10000 if the
 *                   pixels are device pixels; ptr <- a shape as 19's arg 2,
 *                   or 0 to put NovaOS's own pointer back (SetSystemCursor,
 *                   SPI_SETCURSORS).  For every process.
 *  28 SYSCURSOR_IMAGE arg: an OCR_* number | scale << 16; ptr -> { w, h,
 *                   hot x, hot y }, then w * h 0xAARRGGBB pixels: NovaOS's
 *                   drawing of it (w = h = 32 * scale) */
#define CTL_WINDOW_AT    11
#define CTL_ACCEPT_DROPS 12
#define CTL_DROP         13
#define CTL_DROP_FETCH   14
#define CTL_DISPLAY_MODE 15
#define CTL_SET_DISPLAY  16
#define CTL_DROP_DONE    17
#define CTL_DROP_STATUS  18
#define CTL_SET_CURSOR   19
#define CTL_CURSOR_SHAPE 20
#define CTL_SET_SYSCURSOR 27
#define CTL_SYSCURSOR_IMAGE 28
/* Window handles other processes can use.  On Windows an HWND names the
 * same window in every process; a GPU or plugin process sizes and draws
 * into its parent's window.  user32 builds its handles from a tag that is
 * unique among running processes, so they never collide, and tells the
 * kernel which handle each desktop window has:
 *  21 HWND_TAG   returns this process's tag (GUI_TAG_MIN..GUI_TAGS-1)
 *  22 SET_HWND   arg: the user32 handle of window id hwnd; ptr (may be 0)
 *                <- { client x, y in the frame, client w, h } as user32 has it
 *  23 FOREIGN    arg: a handle of another process; ptr -> { pid, thread,
 *                state (CTL_GET_RECT's), client x, y, w, h, frame x, y, w, h }.
 *                Returns 2 for a desktop (top-level) window, 1 for another
 *                window of a running process (only pid is filled), 0 if the
 *                handle's process is gone */
#define CTL_HWND_TAG     21
#define CTL_SET_HWND     22
#define CTL_FOREIGN      23
/* More than one monitor (hwnd may be 0); display N is head N - 1
 * (hal/display.h), monitor N - 1 of the GDI:
 *  24 MONITOR     arg: monitor index; ptr -> { count, x, y, w, h, work x, y,
 *                 w, h, scale (1 = 96 DPI) }, logical px on the virtual
 *                 desktop.  0: no such monitor
 *  25 HEAD_MODE   ptr <- { head, mode as DISPLAY_MODE's arg } -> { width,
 *                 height, bits per pixel, frequency }.  0: no such mode
 *  26 SET_HEAD    ptr <- { head, width, height, CDS_* flags, has position,
 *                 x, y } (0 x 0: the default mode; a position moves monitor
 *                 head, not the primary, on the virtual desktop); returns a
 *                 DISP_CHANGE_* code */
#define CTL_MONITOR      24
#define CTL_HEAD_MODE    25
#define CTL_SET_HEAD     26
/*  29 TOUCH      returns the contacts the touch screens have (0: none) */
#define CTL_TOUCH        29
/*  30 TABLET     pen tablets (wm/tablet.h; wintab32.dll and user32's
 *                synthetic pens), by arg:
 *                0 returns the pen devices present (0: none);
 *                1 ptr -> { after, max, wait ms, _ } <- { ..., newest packet's
 *                  number }, then <- up to max TabletPackets numbered after
 *                  `after` (waits up to wait ms for one); returns how many;
 *                2 / 3 this process makes / drops a synthetic pen;
 *                4 ptr -> { x, y (0-65535 across the desktop), pressure
 *                  (0-1023), buttons (bit 0 tip, 1-2 barrel), flags (1 in
 *                  range, 2 eraser, 4 tilt given, 8 twist given), tilt x,
 *                  tilt y (tenths of a degree, -900..900), twist (tenths of
 *                  a degree, 0..3599) }: a synthetic pen's input (the
 *                  pointer follows it while in range); 0 if the process has
 *                  no pen;
 *                5 returns what the pens present report: 1 tilt, 2 barrel
 *                  rotation (TABLET_CAP_*) */
#define CTL_TABLET       30
/* Per-monitor DPI (user32's DPI awareness):
 *  31 SET_DPI     ptr <- { head, DPI (96 or 192; 0: 96), CDS_* flags }: the
 *                 DPI DPI-aware programs see on that monitor (192 only takes
 *                 effect at scale 2; CDS_UPDATEREGISTRY keeps it).  Every
 *                 window gets WM_NOVA_DPI; returns a DISP_CHANGE_* code
 *  32 SET_SCALE   arg: 1 or 2, the window's bitmap pixels per logical pixel;
 *                 returns the scale set (1 if a larger bitmap can't be had).
 *                 The bitmap grows (to 2x the largest monitor, at most
 *                 GUI_MAX_W x GUI_MAX_H) and may move: ptr (may be 0) ->
 *                 { bitmap VA (64 bits), stride in bytes, rows }; what it
 *                 held is lost
 * MONITOR's ptr gets an eleventh value: the monitor's DPI (GdiMonitorDpi). */
#define CTL_SET_DPI      31
#define CTL_SET_SCALE    32
/*  33 GAMEPAD    game controllers (drivers/gamepad.h; xinput1_4.dll and
 *                dinput8.dll), by arg's low byte, with a slot (0-7) or an
 *                XInput user (0-3) in arg >> 8:
 *                0 returns which slots hold a controller (bit n: slot n);
 *                1 ptr <- the slot's PadInfo; returns 0 if it is empty;
 *                2 ptr <- the slot's PadState; returns 0 if it is empty;
 *                3 ptr -> { left, right (0-65535) }: sets its motors;
 *                4 XInput user's slot, or -1;
 *                Raw Input and hid.dll (their raw HID side, gamepad.h):
 *                5 ptr <- the slot's report descriptor (PAD_DESC_MAX bytes
 *                  of room); returns its length | input report length << 16;
 *                6 ptr <-> { after, known changes, wait ms, max (<= 32);
 *                  out: newest seq, changes; PadRaw[max] }: the reports
 *                  after seq "after", waiting up to "wait ms" (1 s at most) for one or
 *                  for a controller to come or go; returns how many;
 *                7 ptr <- the slot's latest report with ID arg >> 16
 *                  (PadRaw); returns 0 if there is none;
 *                8 ptr = a HID device handle: returns the slot it reads,
 *                  or -1;
 *                9 ptr = a HID device handle: drops its waiting reports;
 *                10 arg >> 8 = a serial: returns its slot, or -1 */
#define CTL_GAMEPAD      33
/* The pointer, moved and confined by the foreground process only (the one
 * whose window is active, or a console program while its Terminal is):
 *  34 SET_CURSOR_POS ptr <- { screen x, y } (logical): SetCursorPos; the
 *                 pointer moves there (kept on the monitors and inside a
 *                 ClipCursor rectangle) and the window under it gets the
 *                 mouse move, as for the mouse; no Raw Input.  0: not the
 *                 foreground process
 *  35 CLIP_CURSOR arg 0: ClipCursor(NULL), the pointer goes anywhere
 *                 (from the background: no change, and no error);
 *                 1: ptr <- { left, top, right, bottom }: keep it inside;
 *                 2: ptr -> the rectangle it is kept in; returns 0 when
 *                 it is not confined.  The confinement ends when another
 *                 process comes to the foreground and on a display change.
 *                 Arg 1 returns 0 from a process not in the foreground */
#define CTL_SET_CURSOR_POS 34
#define CTL_CLIP_CURSOR    35
/*  36 SET_FRAME   arg bit 0: the window has the desktop's title bar and
 *                 border (a program changed its style, such as an SDL game
 *                 going full screen with WS_POPUP: the frame goes, and back);
 *                 bits 1-3: no close button, no minimize/maximize buttons,
 *                 resizable (GUI_NOCLOSE, GUI_NOMINMAX, GUI_RESIZABLE's
 *                 meanings).  The client area stays where it is */
#define CTL_SET_FRAME      36
/*  37 ADAPTER     arg: n; ptr -> { vendor, device, subsystem (vendor |
 *                 id << 16), revision, class << 8 | subclass }: the n-th
 *                 display controller on the PCI bus, in bus order (what
 *                 Direct3D's and DXGI's adapter identifiers report).
 *                 0: no such adapter */
#define CTL_ADAPTER        37
/* Windows of different processes working together, as one HWND does in
 * every process on Windows (WebView2: its host parents the browser
 * process's window in its own and sizes it; Chromium's GPU process draws
 * into it).  Handles are user32's (CTL_SET_HWND); hwnd is 0 unless said:
 *  48 XSEND     arg: a handle of another process; ptr <- { flags (1: the
 *               sender waits for an answer), bytes, then the message (user32's
 *               own layout, at most XMSG_MAX bytes) }: queued for that
 *               process, whose GUI threads' NtNovaGuiGetMessage returns -3
 *               until one takes it.  Returns its number, 0 if the handle's
 *               process is gone (or is the caller)
 *  49 XFETCH    ptr <-> { number, flags, bytes (out); room (in); then the
 *               message }: the oldest one queued for this process; returns
 *               1, 0 if there is none.  One nobody waits on is done with
 *  50 XREPLY    arg: the number; ptr <- { answer (64 bits), bytes, 0, then
 *               data (at most XMSG_MAX bytes) }: the answer to one taken
 *  51 XRESULT   arg: the number; ptr <-> { answer (64 bits), bytes (out);
 *               room, wait ms, give up (in); then the data }.  Returns 1
 *               answered, 0 not yet (after waiting up to wait ms; with
 *               give up the answer is dropped when it comes), 2 never (the
 *               window's process is gone), 3 not yet, and a message is
 *               queued for the caller (a sender answers those while it
 *               waits, so two programs sending to each other never stall)
 *  52 EMBED     hwnd: the caller's desktop window holding the parent; arg:
 *               a handle of another process's window; ptr <- { op, the
 *               parent's client origin x, y in hwnd's client area, the
 *               parent's visible client area left, top, right, bottom (the
 *               same coordinates), the parent shows (0/1), the parent's
 *               handle }.  op 1: embed it (SetParent): its desktop window
 *               loses its frame and stays over the parent's client area,
 *               clipped to it, moving, showing and hiding with the host;
 *               the numbers of its window rectangle become its place in the
 *               parent; it gets WM_NOVA_EMBED (wParam: the parent).  op 2:
 *               the parent moved, resized, showed or hid.  op 0: let it go
 *               (SetParent(NULL)): a top-level window again, WM_NOVA_EMBED 0.
 *               A window without a desktop window yet is embedded when it
 *               gets one.  Returns 1, 2 (to be embedded later), 0 no such
 *               window
 *  53 EMBED_INFO hwnd: the caller's window; ptr -> { the parent's handle,
 *               the parent's client origin x, y on screen }; 0 if it is
 *               not embedded
 *  54 XBLIT     arg: a handle of another process's desktop window (or an
 *               embedded one); ptr <- { 0 write / 1 read, x, y, w, h (client
 *               pixels), the buffer's pixels per row, the buffer (64 bits) }:
 *               copies between the caller's buffer and the window's bitmap
 *               (written to user32's back buffer too, CTL_SET_BACK), so a
 *               GDI DC on another process's window draws (GetDC in
 *               Chromium's GPU process).  Returns 1, 0 no such window
 *  55 SET_BACK  hwnd: the caller's window; ptr <- { back buffer (64 bits),
 *               its pixels per row } (0: none) */
#define CTL_XSEND          48
#define CTL_XFETCH         49
#define CTL_XREPLY         50
#define CTL_XRESULT        51
#define CTL_EMBED          52
#define CTL_EMBED_INFO     53
#define CTL_XBLIT          54
#define CTL_SET_BACK       55
#define GUI_TAGS         2048
#define GUI_TAG_MIN      4               /* keeps every handle above 0xFFFF */
#define GUI_TAG_SHIFT    14
#define WM_NOVA_DROP     0x03FE
#define WM_DISPLAYCHANGE 0x007E
#define CDS_UPDATEREGISTRY 0x01
#define CDS_TEST           0x02
#define CDS_FULLSCREEN     0x04
#define DISP_CHANGE_SUCCESSFUL 0
#define DISP_CHANGE_FAILED     (-1)
#define DISP_CHANGE_BADMODE    (-2)

/* The process whose CDS_FULLSCREEN mode is on: the default mode comes back
 * when it ends */
static UmProcess *g_fullscreen_proc;

static UINT64 display_mode_info(UINT64 which, UINT64 ptr)
{
    DisplayMode m;
    INT32 i = (INT32)which;              /* WOW64 passes 32 bits */
    if (i == -1) m = DisplayCurrentMode();
    else if (i == -2) m = DisplayDefaultMode();
    else if (!DisplayModeAt((int)i, &m)) return 0;
    UINT32 out[4] = { (UINT32)m.w, (UINT32)m.h, 32, 60 };
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, out, sizeof(out))) ? 1 : 0;
}

static UINT64 display_set(UmProcess *p, UINT64 ptr)
{
    INT32 in[3];
    if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    UINT32 flags = (UINT32)in[2];
    bool reset = in[0] == 0 && in[1] == 0;
    DisplayMode m = reset ? DisplayDefaultMode() : (DisplayMode){ in[0], in[1] };
    if (!DisplayModeSupported(m.w, m.h)) return (UINT64)(INT64)DISP_CHANGE_BADMODE;
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (!DesktopSetDisplayMode(m.w, m.h)) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    if (flags & CDS_UPDATEREGISTRY) DesktopSaveDisplayMode(m.w, m.h);
    g_fullscreen_proc = !reset && (flags & CDS_FULLSCREEN) && !(flags & CDS_UPDATEREGISTRY) ? p : NULL;
    return DISP_CHANGE_SUCCESSFUL;
}

static UINT64 monitor_info(UINT64 which, UINT64 ptr)
{
    INT32 i = (INT32)which, out[11] = { 0 };
    DesktopLock();
    int n = GdiMonitorCount();
    if (i >= 0 && i < n) {
        GdiRect r = GdiMonitorRect(i), wa = WmMonitorWork(i);
        INT32 v[11] = { n, r.x, r.y, r.w, r.h, wa.x, wa.y, wa.w, wa.h, GdiMonitorScale(i), GdiMonitorDpi(i) };
        memcpy(out, v, sizeof(out));
    }
    DesktopUnlock();
    if (!out[0]) return 0;
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, out, sizeof(out))) ? 1 : 0;
}

/* CTL_ADAPTER: the n-th PCI display controller's identity */
static UINT64 adapter_info(UINT64 n, UINT64 ptr)
{
    PciDevice d;
    const char *drv;
    UINT64 k = 0;
    for (int i = 0; PciAt(i, &d, &drv); i++) {
        if (d.class_code != 0x03 || k++ != n) continue;
        UINT32 out[5] = { d.vendor, d.device, PciRead32(d.bus, d.dev, d.func, 0x2C),
                          PciRead32(d.bus, d.dev, d.func, 0x08) & 0xFF, (UINT32)d.class_code << 8 | d.subclass };
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, out, sizeof(out))) ? 1 : 0;
    }
    return 0;
}

static UINT64 head_mode_info(UINT64 ptr)
{
    INT32 in[2];
    if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return 0;
    DisplayMode m;
    if (in[0] < 0 || in[0] >= DisplayHeadCount()) return 0;
    if (in[1] == -1) m = DisplayHeadMode(in[0]);
    else if (in[1] == -2) m = DisplayHeadDefaultMode(in[0]);
    else if (!DisplayHeadModeAt(in[0], in[1], &m)) return 0;
    UINT32 out[4] = { (UINT32)m.w, (UINT32)m.h, 32, 60 };
    return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, out, sizeof(out))) ? 1 : 0;
}

static UINT64 head_set(UmProcess *p, UINT64 ptr)
{
    INT32 in[7];
    if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    int head = in[0];
    UINT32 flags = (UINT32)in[3];
    if (head == 0 && !in[4]) {                        /* the primary's mode: SET_DISPLAY */
        INT32 d[3] = { in[1], in[2], in[3] };
        bool reset = d[0] == 0 && d[1] == 0;
        DisplayMode m = reset ? DisplayDefaultMode() : (DisplayMode){ d[0], d[1] };
        if (!DisplayModeSupported(m.w, m.h)) return (UINT64)(INT64)DISP_CHANGE_BADMODE;
        if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
        if (!DesktopSetDisplayMode(m.w, m.h)) return (UINT64)(INT64)DISP_CHANGE_FAILED;
        if (flags & CDS_UPDATEREGISTRY) DesktopSaveDisplayMode(m.w, m.h);
        g_fullscreen_proc = !reset && (flags & CDS_FULLSCREEN) && !(flags & CDS_UPDATEREGISTRY) ? p : NULL;
        return DISP_CHANGE_SUCCESSFUL;
    }
    if (head < 0 || head >= DisplayHeadCount()) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    if (in[4] && head == 0) return (UINT64)(INT64)DISP_CHANGE_BADMODE;  /* the primary stays at (0, 0) */
    DisplayMode m = in[1] == 0 && in[2] == 0 ? DisplayHeadDefaultMode(head) : (DisplayMode){ in[1], in[2] };
    if (!DisplayHeadModeSupported(head, m.w, m.h)) return (UINT64)(INT64)DISP_CHANGE_BADMODE;
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (!DesktopSetHeadMode(head, m.w, m.h)) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    if (flags & CDS_UPDATEREGISTRY) DesktopSaveHeadMode(head, m.w, m.h);
    if (in[4]) DesktopSetMonitorOrigin(head, in[5], in[6], (flags & CDS_UPDATEREGISTRY) != 0);
    return DISP_CHANGE_SUCCESSFUL;
}

static UINT64 dpi_set(UINT64 ptr)
{
    INT32 in[3];
    if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    if (in[0] < 0 || in[0] >= DisplayHeadCount()) return (UINT64)(INT64)DISP_CHANGE_FAILED;
    if (in[1] && in[1] != 96 && in[1] != 192) return (UINT64)(INT64)DISP_CHANGE_BADMODE;
    if ((UINT32)in[2] & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    DesktopSetMonitorDpi(in[0], in[1] ? in[1] : 96, ((UINT32)in[2] & CDS_UPDATEREGISTRY) != 0);
    return DISP_CHANGE_SUCCESSFUL;
}

void UmGuiDpiChanged(void)
{
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].proc)
            enqueue_locked(&g_win[i], WM_NOVA_DPI, 0, 0, 0, 0);
    spin_unlock_irqrestore(&g_gui_lock, s);
    waitq_wake(&g_guiq);
}

void UmGuiDisplayChanged(int w, int h)
{
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].proc)
            enqueue_locked(&g_win[i], WM_DISPLAYCHANGE, 32, packxy(w, h), 0, 0);
    spin_unlock_irqrestore(&g_gui_lock, s);
    gui_wake(0, WM_DISPLAYCHANGE);
}
#define DROP_MAX         (64 * 1024)
#define DROP_RESULTS     8

/* drops their targets have finished: { number, effect }; under g_gui_lock */
static UINT32 g_drop_seq;
static UINT32 g_drop_done[DROP_RESULTS][2];

/* the process holding each handle tag; under g_gui_lock */
static UmProcess *g_tag_proc[GUI_TAGS];
static UINT32 g_tag_next = GUI_TAG_MIN;

/* Pen tablets (CTL_TABLET) */
static UINT64 tablet_ctl(UmProcess *p, UINT64 op, UINT64 ptr)
{
    switch (op) {
    case 0: return (UINT64)TabletDevices();
    case 1: {
        UINT32 in[4];
        if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return 0;
        int max = in[1] > 64 ? 64 : (int)in[1];
        TabletPacket *buf = kmalloc(sizeof(TabletPacket) * (max ? max : 1));
        if (!buf) return 0;
        UINT64 wait = in[2] / 10 > 50 ? 50 : in[2] / 10;   /* ticks, at most half a second */
        UINT32 newest = 0;
        int n = TabletRead(in[0], buf, max, wait, &newest);
        UINT64 r = n && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)(ptr + 16), buf, sizeof(TabletPacket) * (UINT64)n)) ? 0 : (UINT64)n;
        CopyToUser((void *)(uintptr_t)(ptr + 12), &newest, sizeof(newest));
        kfree(buf);
        return r;
    }
    case 2: TabletDevice(p, 1, TABLET_CAP_TILT | TABLET_CAP_TWIST); return 1;
    case 3: TabletDevice(p, -1, TABLET_CAP_TILT | TABLET_CAP_TWIST); return 1;
    case 5: return (UINT64)TabletCaps();
    case 4: {
        INT32 in[8];
        if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return 0;
        if (!TabletOwns(p)) return 0;
        InputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.absolute = 1;
        ev.dx = in[0] < 0 ? 0 : in[0] > 65535 ? 65535 : in[0];
        ev.dy = in[1] < 0 ? 0 : in[1] > 65535 ? 65535 : in[1];
        ev.type = INPUT_PEN;
        ev.buttons = (UINT8)(in[3] & 7);
        ev.pressure = (UINT16)(in[2] < 0 ? 0 : in[2] > TABLET_PRESSURE ? TABLET_PRESSURE : in[2]);
        ev.pressed = (in[4] & 1) != 0;
        ev.extended = (in[4] & 2) != 0;
        ev.pen_has = (in[4] & 4 ? PEN_HAS_TILT : 0) | (in[4] & 8 ? PEN_HAS_TWIST : 0);
        if (in[4] & 4) {
            ev.tilt_x = (INT16)(in[5] < -900 ? -900 : in[5] > 900 ? 900 : in[5]);
            ev.tilt_y = (INT16)(in[6] < -900 ? -900 : in[6] > 900 ? 900 : in[6]);
        }
        if (in[4] & 8) ev.twist = (UINT16)(((in[7] % 3600) + 3600) % 3600);
        InputPost(&ev);
        if (in[4] & 1) {                       /* in range: the pointer follows, the tip clicks */
            InputEvent m;
            memset(&m, 0, sizeof(m));
            m.type = INPUT_MOUSE;
            m.absolute = 1;
            m.dx = ev.dx; m.dy = ev.dy;
            m.buttons = (in[3] & 1 ? MOUSE_LEFT : 0) | (in[3] & 2 ? MOUSE_RIGHT : 0);
            m.from_pen = 1;
            InputPost(&m);
        }
        return 1;
    }
    }
    return 0;
}

/* SetCursorPos (CTL_SET_CURSOR_POS): the pointer is the user's, so only
 * the foreground process moves it; the window under it hears of the move
 * as it would of the mouse's (WM_MOUSEMOVE), which programs that recentre
 * the pointer every frame rely on (SDL's relative mouse mode by warping) */
static UINT64 cursor_set_pos(UmProcess *p, UINT64 ptr)
{
    INT32 c[2];
    if (!NT_SUCCESS(CopyFromUser(c, (const void *)(uintptr_t)ptr, sizeof(c)))) return 0;
    DesktopLock();
    UmProcess *a = active_process();
    clip_follow(a);                             /* (the desktop's tick may not have seen a change yet) */
    bool ok = a == p && !p->exited;
    if (ok) {
        WmCursorMove(c[0], c[1]);
        WmMouseMove(WmCursorX(), WmCursorY());  /* even where it was: SDL takes it as the warp's end */
    }
    DesktopUnlock();
    return ok ? 1 : 0;
}

/* ClipCursor (CTL_CLIP_CURSOR) */
static UINT64 cursor_clip(UmProcess *p, UINT64 arg, UINT64 ptr)
{
    INT32 r[4] = { 0 };
    if (arg == 1 && !NT_SUCCESS(CopyFromUser(r, (const void *)(uintptr_t)ptr, sizeof(r)))) return 0;
    UINT64 ok = 1;
    DesktopLock();
    clip_follow(active_process());
    if (arg == 2) {
        GdiRect c;
        ok = WmCursorClipRect(&c);
        r[0] = c.x; r[1] = c.y; r[2] = c.x + c.w; r[3] = c.y + c.h;
    } else if (arg == 0) {
        /* Its own confinement, or the foreground's: let go (another
         * process's, from the background: nothing to do) */
        if (p->pid == g_clip_pid || active_process() == p) { g_clip_pid = 0; WmCursorClip(NULL); }
    } else if (active_process() != p || p->exited) {
        ok = 0;
    } else {
        /* Kept to the desktop (a rectangle off it confines to the nearest edge) */
        GdiRect v = GdiVirtualRect();
        int l = r[0], t = r[1], rt = r[2], b = r[3];
        if (l < v.x) l = v.x;
        if (t < v.y) t = v.y;
        if (rt > v.x + v.w) rt = v.x + v.w;
        if (b > v.y + v.h) b = v.y + v.h;
        if (l >= v.x + v.w) l = v.x + v.w - 1;
        if (t >= v.y + v.h) t = v.y + v.h - 1;
        if (rt <= l) rt = l + 1;
        if (b <= t) b = t + 1;
        GdiRect c = { l, t, rt - l, b - t };
        int ox = WmCursorX(), oy = WmCursorY();
        g_clip_pid = p->pid;
        WmCursorClip(&c);
        if (WmCursorX() != ox || WmCursorY() != oy) WmMouseMove(WmCursorX(), WmCursorY());
    }
    DesktopUnlock();
    if (arg == 2 && ok && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, r, sizeof(r)))) ok = 0;
    return ok;
}

/* Game controllers (CTL_GAMEPAD) */
static UINT64 gamepad_ctl(UINT64 arg, UINT64 ptr)
{
    UmProcess *p = UmCurrent();
    int at = (int)((arg >> 8) & 0xFF);
    switch (arg & 0xFF) {
    case 0: return PadPresent();
    case 1: {
        PadInfo i;
        if (!PadGetInfo(at, &i)) return 0;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &i, sizeof(i))) ? 1 : 0;
    }
    case 2: {
        PadState st;
        if (!PadGetState(at, &st)) return 0;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &st, sizeof(st))) ? 1 : 0;
    }
    case 3: {
        UINT16 m[2];
        if (!NT_SUCCESS(CopyFromUser(m, (const void *)(uintptr_t)ptr, sizeof(m)))) return 0;
        return PadSetRumble(at, m[0], m[1]) ? 1 : 0;
    }
    case 4: return (UINT64)(INT64)PadXInputSlot(at);
    case 5: {
        UINT8 *d = kmalloc(PAD_DESC_MAX);
        if (!d) return 0;
        UINT16 in_len = 0;
        int n = PadGetDescriptor(at, d, PAD_DESC_MAX, &in_len);
        bool ok = n && NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, d, (size_t)n));
        kfree(d);
        return ok ? (UINT64)n | (UINT64)in_len << 16 : 0;
    }
    case 6: {
        UINT32 q[6];
        if (!NT_SUCCESS(CopyFromUser(q, (const void *)(uintptr_t)ptr, 4 * sizeof(UINT32)))) return 0;
        int max = q[3] > 32 ? 32 : (int)q[3];
        PadRaw *r = max ? kmalloc(sizeof(PadRaw) * (size_t)max) : NULL;
        if (max && !r) return 0;
        UINT64 ticks = q[2] ? (q[2] + 9) / 10 : 0;
        int n = PadReadRaw(q[0], q[1], r, max, ticks > 100 ? 100 : ticks, &q[4], &q[5]);
        bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)(ptr + 4 * sizeof(UINT32)), &q[4], 2 * sizeof(UINT32))) &&
                  (!n || NT_SUCCESS(CopyToUser((void *)(uintptr_t)(ptr + sizeof(q)), r, sizeof(PadRaw) * (size_t)n)));
        kfree(r);
        return ok ? (UINT64)n : 0;
    }
    case 7: {
        PadRaw r;
        if (!PadLastRaw(at, (UINT8)(arg >> 16), &r)) return 0;
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &r, sizeof(r))) ? 1 : 0;
    }
    case 8: return (UINT64)(INT64)um_hid_slot(p, ptr, NULL);
    case 9: return um_hid_flush(p, ptr) ? 1 : 0;
    case 10: return (UINT64)(INT64)PadSlotOfSerial((UINT32)(arg >> 8));
    }
    return 0;
}

static UINT64 hwnd_tag(UmProcess *p)
{
    UINT64 r = 0;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = GUI_TAG_MIN; i < GUI_TAGS && !r; i++) if (g_tag_proc[i] == p) r = (UINT64)i;
    /* a new one: the next free tag, so a dead process's tag is reused last */
    for (int n = 0; n < GUI_TAGS && !r; n++) {
        UINT32 i = g_tag_next++;
        if (g_tag_next >= GUI_TAGS) g_tag_next = GUI_TAG_MIN;
        if (i >= GUI_TAG_MIN && !g_tag_proc[i]) { g_tag_proc[i] = p; r = i; }
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    return r;
}

static UINT64 hwnd_foreign(UINT32 h, UINT64 ptr)
{
    INT32 out[11] = { 0 };
    UINT64 r = 0;
    DesktopLock();
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    UmProcess *owner = g_tag_proc[(h >> GUI_TAG_SHIFT) % GUI_TAGS];
    GuiWin *g = NULL;
    for (int i = 0; h && i < GUI_MAX_WINDOWS && !g; i++)
        if (g_win[i].used && g_win[i].hwnd == h && g_win[i].proc && !g_win[i].proc->exited) g = &g_win[i];
    if (g) {
        out[0] = (INT32)g->proc->pid; out[1] = (INT32)g->tid;
        WND *w = g->wnd;
        if (w && g->host) {                     /* embedded: its whole rectangle, shown as the program has it */
            GuiWin *h = NULL;
            for (int i = 0; i < GUI_MAX_WINDOWS && !h; i++)
                if (g_win[i].used && g_win[i].id == g->host && g_win[i].wnd) h = &g_win[i];
            GdiRect hc = h ? WmClientRect(h->wnd) : RECT(0, 0, 0, 0);
            int x = hc.x + g->pox + g->rx, y = hc.y + g->poy + g->ry;
            bool vis = g->want_vis && g->parent_vis && h && h->wnd->visible && !h->wnd->minimized;
            out[2] = (vis ? 1 : 0) | (w->active ? 2 : 0);
            out[3] = out[7] = x; out[4] = out[8] = y;
            out[5] = out[9] = g->cw; out[6] = out[10] = g->ch;
            if (g->uc[2] > 0) { out[3] = x + g->uc[0]; out[4] = y + g->uc[1]; out[5] = g->uc[2]; out[6] = g->uc[3]; }
        } else if (w) {
            GdiRect c = WmClientRect(w), f = w->frame;
            out[2] = (w->visible ? 1 : 0) | (w->active ? 2 : 0) | (w->minimized ? 4 : 0) | (w->maximized ? 8 : 0);
            out[3] = c.x; out[4] = c.y; out[5] = g->cw; out[6] = g->ch;
            out[7] = f.x; out[8] = f.y; out[9] = f.w; out[10] = f.h;
            if (g->uc[2] > 0) { out[3] = f.x + g->uc[0]; out[4] = f.y + g->uc[1]; out[5] = g->uc[2]; out[6] = g->uc[3]; }
        }
        r = 2;
    } else if (owner && !owner->exited) {
        out[0] = (INT32)owner->pid;
        r = 1;
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    DesktopUnlock();
    if (r && ptr && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, out, sizeof(out)))) return 0;
    return r;
}

/* The process's pointer becomes @c (NULL: the arrow) on all its windows */
static void cursor_set(UmProcess *p, GdiCursorShape *c)
{
    DesktopLock();
    GdiCursorShape *old = p->cursor;
    p->cursor = c;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].proc == p && g_win[i].wnd) g_win[i].wnd->cursor = c;
    WmCursorShapeChanged();
    DesktopUnlock();
    kfree(old);
}

/* A shape from user space: SET_CURSOR's blob (how 2, logical pixels, or
 * 4, device pixels); NULL if it is malformed */
static GdiCursorShape *shape_from_user(UINT64 how, UINT64 ptr)
{
    INT32 hd[6] = { 0 };
    if (!NT_SUCCESS(CopyFromUser(hd, (const void *)(uintptr_t)ptr, sizeof(hd)))) return NULL;
    int w = hd[0], h = hd[1], nf = hd[4], ns = hd[5];
    int max = how == 4 ? GDI_CURSOR_MAX * GDI_MAX_SCALE : GDI_CURSOR_MAX;
    if (w < 1 || h < 1 || w > max || h > max ||
        nf < 1 || nf > GDI_CURSOR_FRAMES || ns < 1 || ns > GDI_CURSOR_STEPS) return NULL;
    size_t npx = (size_t)nf * w * h;
    GdiCursorShape *c = kzalloc(sizeof(GdiCursorShape) + npx * 4);
    if (!c) return NULL;
    c->w = w; c->h = h; c->nframes = nf; c->nsteps = ns;
    c->dev = how == 4;
    c->hot_x = hd[2] < 0 ? 0 : hd[2] >= w ? w - 1 : hd[2];
    c->hot_y = hd[3] < 0 ? 0 : hd[3] >= h ? h - 1 : hd[3];
    UINT32 *st = kmalloc((size_t)ns * 8);
    bool ok = st && NT_SUCCESS(CopyFromUser(st, (const void *)(uintptr_t)(ptr + sizeof(hd)), (size_t)ns * 8)) &&
              NT_SUCCESS(CopyFromUser(c->argb, (const void *)(uintptr_t)(ptr + sizeof(hd) + (size_t)ns * 8), npx * 4));
    for (int i = 0; ok && i < ns; i++) {
        UINT32 j = st[i * 2 + 1] ? st[i * 2 + 1] : 1;      /* jiffies: 1/60 s */
        c->steps[i].frame = (UINT16)(st[i * 2] < (UINT32)nf ? st[i * 2] : 0);
        c->steps[i].ticks = (j * 100 + 30) / 60;
        if (!c->steps[i].ticks) c->steps[i].ticks = 1;
        c->total += c->steps[i].ticks;
    }
    kfree(st);
    if (!ok) { kfree(c); return NULL; }
    return c;
}

static UINT64 cursor_from_user(UmProcess *p, UINT64 how, UINT64 ptr)
{
    if (how == 0) { cursor_set(p, NULL); return 1; }
    GdiCursorShape *c;
    if (how == 1 || how == 3) {
        int id = how == 3 ? SysCursorCanon((int)ptr) : 0;
        if (how == 3 && !id) return 0;
        c = kzalloc(sizeof(GdiCursorShape));
        if (!c) return 0;
        c->hidden = how == 1;
        c->sys = id;
    } else if (how == 2 || how == 4) {
        c = shape_from_user(how, ptr);
        if (!c) return 0;
    } else {
        return 0;
    }
    cursor_set(p, c);
    return 1;
}

/* SetSystemCursor: replace (or, with no shape, restore) a system pointer */
static UINT64 syscursor_set(UINT64 arg, UINT64 ptr)
{
    int id = (int)(arg & 0xFFFF) ? SysCursorCanon((int)(arg & 0xFFFF)) : 0;
    if ((arg & 0xFFFF) && !id) return 0;
    if (!id) {                                  /* every one back */
        if (ptr) return 0;
        static const int all[] = { OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP, OCR_SIZENWSE,
                                   OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL, OCR_NO, OCR_HAND,
                                   OCR_APPSTARTING, OCR_HELP };
        for (UINT32 i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            DesktopLock();
            GdiCursorShape *old = WmSetSystemCursor(all[i], NULL);
            DesktopUnlock();
            kfree(old);
        }
        return 1;
    }
    GdiCursorShape *c = NULL;
    if (ptr && !(c = shape_from_user(arg & 0x10000 ? 4 : 2, ptr))) return 0;
    DesktopLock();
    GdiCursorShape *old = WmSetSystemCursor(id, c);
    DesktopUnlock();
    kfree(old);
    return old != c || !c ? 1 : 0;
}

static UINT64 syscursor_image(UINT64 arg, UINT64 ptr)
{
    int id = SysCursorCanon((int)(arg & 0xFFFF)), s = (int)(arg >> 16 & 0xFF);
    if (!id || s < 1 || s > GDI_MAX_SCALE) return 0;
    int side = SYSCUR_BOX * s;
    UINT32 *px = kmalloc((size_t)side * side * 4 + 16);
    void *scratch = kmalloc(SYSCUR_SCRATCH);
    if (!px || !scratch) { kfree(px); kfree(scratch); return 0; }
    INT32 hx, hy;
    SysCursorRender(id, s, 0, false, px + 4, &hx, &hy, scratch);   /* no desktop lock: see syscursor.h */
    kfree(scratch);
    px[0] = (UINT32)side; px[1] = (UINT32)side; px[2] = (UINT32)hx; px[3] = (UINT32)hy;
    bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, px, (size_t)side * side * 4 + 16));
    kfree(px);
    return ok ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Messages between processes (CTL_XSEND and friends).  The slots are under
 * g_gui_lock; their data is kmalloc'd
 * ----------------------------------------------------------------------- */
#define XMSG_SLOTS 64
#define XMSG_MAX   (64 * 1024)
enum { XM_QUEUED = 1, XM_TAKEN, XM_ANSWERED };
typedef struct {
    UINT32      seq;                /* 0: free */
    UmProcess  *to;                 /* the window's process */
    UINT32      from;               /* the sender's pid */
    UINT8       state;
    bool        reply;              /* the sender waits for the answer */
    bool        dropped;            /* ... gave up: the answer is thrown away */
    UINT32      flags;
    UINT64      result;
    UINT32      len;
    void       *data;
} XMsg;
static XMsg g_xmsg[XMSG_SLOTS];
static UINT32 g_xmsg_seq = 0x100;

static void xmsg_free_locked(XMsg *m, void **data)
{
    *data = m->data;
    memset(m, 0, sizeof(*m));
}

/* a message queued for @p that nobody has taken; under g_gui_lock */
static bool xmsg_waiting(UmProcess *p)
{
    for (int i = 0; i < XMSG_SLOTS; i++) if (g_xmsg[i].seq && g_xmsg[i].to == p && g_xmsg[i].state == XM_QUEUED) return true;
    return false;
}

static UmProcess *tag_process(UINT32 h)
{
    return h >= 0x10000 ? g_tag_proc[(h >> GUI_TAG_SHIFT) % GUI_TAGS] : NULL;
}

static UINT64 xmsg_send(UmProcess *p, UINT64 hwnd, UINT64 ptr)
{
    UINT32 hd[2];
    if (!NT_SUCCESS(CopyFromUser(hd, (const void *)(uintptr_t)ptr, sizeof(hd)))) return 0;
    if (hd[1] > XMSG_MAX) return 0;
    void *data = kmalloc(hd[1] ? hd[1] : 1);
    if (!data) return 0;
    if (hd[1] && !NT_SUCCESS(CopyFromUser(data, (const void *)(uintptr_t)(ptr + sizeof(hd)), hd[1]))) { kfree(data); return 0; }
    UINT32 seq = 0;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    UmProcess *to = tag_process((UINT32)hwnd);
    if (to && to != p && !to->exited) {
        for (int i = 0; i < XMSG_SLOTS; i++) {
            XMsg *m = &g_xmsg[i];
            if (m->seq) continue;
            if (++g_xmsg_seq < 0x100) g_xmsg_seq = 0x100;
            seq = m->seq = g_xmsg_seq;
            m->to = to; m->from = p->pid; m->state = XM_QUEUED;
            m->reply = (hd[0] & 1) != 0; m->dropped = false; m->flags = hd[0];
            m->result = 0; m->len = hd[1]; m->data = data;
            data = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    kfree(data);
    if (seq) gui_wake(0, 0);
    return seq;
}

static UINT64 xmsg_fetch(UmProcess *p, UINT64 ptr)
{
    UINT32 hd[4];
    if (!NT_SUCCESS(CopyFromUser(hd, (const void *)(uintptr_t)ptr, sizeof(hd)))) return 0;
    void *data = NULL;
    UINT32 seq = 0, len = 0, flags = 0;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    XMsg *best = NULL;
    for (int i = 0; i < XMSG_SLOTS; i++) {
        XMsg *m = &g_xmsg[i];
        if (m->seq && m->to == p && m->state == XM_QUEUED && (!best || (INT32)(m->seq - best->seq) < 0)) best = m;
    }
    if (best) {
        seq = best->seq; len = best->len; flags = best->flags;
        if (best->reply) {                      /* the answer replaces it */
            data = best->data;
            best->data = NULL; best->len = 0;
            best->state = XM_TAKEN;
        } else {
            xmsg_free_locked(best, &data);
        }
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    if (!seq) return 0;
    UINT32 out[3] = { seq, flags, len };
    bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, out, sizeof(out))) &&
              (!len || len > hd[3] || NT_SUCCESS(CopyToUser((void *)(uintptr_t)(ptr + sizeof(hd)), data, len)));
    kfree(data);
    return ok ? 1 : 0;
}

static UINT64 xmsg_reply(UmProcess *p, UINT64 seq, UINT64 ptr)
{
    struct { UINT64 result; UINT32 len, pad; } hd;
    if (!NT_SUCCESS(CopyFromUser(&hd, (const void *)(uintptr_t)ptr, sizeof(hd)))) return 0;
    if (hd.len > XMSG_MAX) hd.len = 0;
    void *data = hd.len ? kmalloc(hd.len) : NULL;
    if (hd.len && (!data || !NT_SUCCESS(CopyFromUser(data, (const void *)(uintptr_t)(ptr + sizeof(hd)), hd.len)))) {
        kfree(data);
        data = NULL;
        hd.len = 0;
    }
    void *old = NULL;
    bool found = false;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = 0; i < XMSG_SLOTS; i++) {
        XMsg *m = &g_xmsg[i];
        if (m->seq != (UINT32)seq || m->to != p || m->state != XM_TAKEN) continue;
        found = true;
        if (m->dropped) { xmsg_free_locked(m, &old); break; }
        old = m->data;
        m->data = data; m->len = hd.len; m->result = hd.result;
        m->state = XM_ANSWERED;
        data = NULL;
        break;
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    kfree(old);
    kfree(data);
    if (found) gui_wake(0, 0);
    return found ? 1 : 0;
}

static UINT64 xmsg_result(UmProcess *p, UINT64 seq, UINT64 ptr)
{
    struct { UINT64 result; UINT32 len, room, wait, drop; } hd;
    if (!NT_SUCCESS(CopyFromUser(&hd, (const void *)(uintptr_t)ptr, sizeof(hd)))) return 2;
    UmThread *t = UmCurrentThread();
    UINT32 tid = t ? t->tid : 0;
    UINT64 until = sched_ticks() + (hd.wait + 9) / 10;
    for (;;) {
        UINT32 gen = waitq_gen(&g_guiq);
        void *data = NULL;
        int r = 0;
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        XMsg *m = NULL;
        for (int i = 0; i < XMSG_SLOTS && !m; i++) if (g_xmsg[i].seq == (UINT32)seq && g_xmsg[i].from == p->pid) m = &g_xmsg[i];
        if (!m) r = 2;
        else if (m->state == XM_ANSWERED) {
            hd.result = m->result; hd.len = m->len;
            xmsg_free_locked(m, &data);
            r = 1;
        } else if (!m->to || m->to->exited) {
            xmsg_free_locked(m, &data);
            r = 2;
        } else if (hd.drop) {
            if (m->state == XM_QUEUED) xmsg_free_locked(m, &data);
            else m->dropped = true;
            r = 0;
        } else if (xmsg_waiting(p)) {
            r = 3;
        }
        spin_unlock_irqrestore(&g_gui_lock, s);
        if (r == 1) {
            bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)ptr, &hd, 12)) &&
                      (!hd.len || hd.len > hd.room || NT_SUCCESS(CopyToUser((void *)(uintptr_t)(ptr + sizeof(hd)), data, hd.len)));
            kfree(data);
            return ok ? 1 : 2;
        }
        kfree(data);
        if (r || hd.drop || !hd.wait) return (UINT64)r;
        if (um_stopping()) return 0;
        UINT64 now = sched_ticks();
        if (now >= until) return 0;
        waitq_wait_tag(&g_guiq, gen, (UINT32)(until - now < 10 ? until - now : 10), tid);
    }
}

/* A process's windows and messages are gone: the messages it sent and
 * the ones queued for it go too (the senders hear "never") */
static void xmsg_process_gone(UmProcess *p)
{
    for (int i = 0; i < XMSG_SLOTS; i++) {
        void *data = NULL;
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        XMsg *m = &g_xmsg[i];
        if (m->seq && m->to == p) xmsg_free_locked(m, &data);
        else if (m->seq && m->from == p->pid && m->reply) {     /* (posted ones are still delivered) */
            if (m->state == XM_TAKEN) m->dropped = true;
            else xmsg_free_locked(m, &data);
        }
        spin_unlock_irqrestore(&g_gui_lock, s);
        kfree(data);
    }
    for (int i = 0; i < EMBED_PENDING; i++) {
        if (!g_embed_pending[i].hwnd) continue;
        if (tag_process(g_embed_pending[i].hwnd) == p) g_embed_pending[i].hwnd = 0;
    }
}

/* A window of any process by its user32 handle (DesktopLock held) */
static GuiWin *win_by_hwnd(UINT32 h)
{
    GuiWin *g = NULL;
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    for (int i = 0; h && i < GUI_MAX_WINDOWS && !g; i++)
        if (g_win[i].used && g_win[i].hwnd == h && g_win[i].wnd && g_win[i].proc && !g_win[i].proc->exited) g = &g_win[i];
    spin_unlock_irqrestore(&g_gui_lock, s);
    return g;
}

/* CTL_EMBED */
static UINT64 embed_ctl(UmProcess *p, UINT64 hwnd, UINT64 target, UINT64 ptr)
{
    INT32 in[9];
    if (!NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)ptr, sizeof(in)))) return 0;
    UINT64 rv = 0;
    DesktopLock();
    GuiWin *h = win_lookup(p, hwnd);
    UmProcess *tp = tag_process((UINT32)target);
    GuiWin *g = win_by_hwnd((UINT32)target);
    if (g && g->proc == p) g = NULL;            /* (a window of its own: user32's) */
    int pend = -1;
    for (int i = 0; i < EMBED_PENDING; i++) if (g_embed_pending[i].hwnd == (UINT32)target) pend = i;
    if (in[0] == 0) {                           /* let it go */
        if (pend >= 0) g_embed_pending[pend].hwnd = 0;
        if (g && g->host && (!h || g->host == h->id)) embed_release(g);
        rv = g || pend >= 0;
    } else if (h && h->wnd && g) {
        if (in[0] == 1 || g->host == h->id) { embed_apply(g, h, (UINT32)in[8], in); rv = 1; }
    } else if (h && h->wnd && tp && tp != p && !tp->exited && in[0] == 1) {
        if (pend < 0) for (int i = 0; i < EMBED_PENDING && pend < 0; i++) if (!g_embed_pending[i].hwnd) pend = i;
        if (pend >= 0) {
            g_embed_pending[pend].hwnd = (UINT32)target;
            g_embed_pending[pend].host = h->id;
            memcpy(g_embed_pending[pend].in, in, sizeof(g_embed_pending[pend].in));
            g_embed_pending[pend].in[0] = (INT32)in[8];      /* (op 1 implied; keep the parent's handle) */
            rv = 2;
        }
    } else if (h && pend >= 0 && in[0] == 2 && g_embed_pending[pend].host == h->id) {
        memcpy(g_embed_pending[pend].in + 1, in + 1, 7 * sizeof(INT32));
        rv = 2;
    }
    DesktopUnlock();
    return rv;
}

/* CTL_SET_HWND: an embed waiting for this window takes effect (DesktopLock) */
static void embed_pending_check(GuiWin *g)
{
    for (int i = 0; i < EMBED_PENDING; i++) {
        if (!g_embed_pending[i].hwnd || g_embed_pending[i].hwnd != g->hwnd) continue;
        UINT32 host = g_embed_pending[i].host;
        INT32 in[8];
        memcpy(in, g_embed_pending[i].in, sizeof(in));
        g_embed_pending[i].hwnd = 0;
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *h = win_by_id(host);
        spin_unlock_irqrestore(&g_gui_lock, s);
        UINT32 parent = (UINT32)in[0];
        in[0] = 1;
        if (h && h->wnd && h->proc && !h->proc->exited && h->proc != g->proc) embed_apply(g, h, parent, in);
    }
}

/* CTL_XBLIT: rows between the caller's buffer and a window's bitmap */
static UINT64 xblit(UmProcess *p, UINT64 target, UINT64 ptr)
{
    struct { INT32 dir, x, y, w, h, stride; UINT64 buf; } b;
    if (!NT_SUCCESS(CopyFromUser(&b, (const void *)(uintptr_t)ptr, sizeof(b)))) return 0;
    if (b.w <= 0 || b.h <= 0 || b.stride < b.w || b.w > GUI_MAX_W * GDI_MAX_SCALE) return 0;
    UINT32 *row = kmalloc((size_t)b.w * 4);
    if (!row) return 0;
    UINT64 rv = 0;
    DesktopLock();
    GuiWin *g = win_by_hwnd((UINT32)target);
    if (g && g->proc != p) {
        int ox = g->uc[2] > 0 ? g->uc[0] : 0, oy = g->uc[2] > 0 ? g->uc[1] : 0;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        int x = b.x, y = b.y, w = b.w, h = b.h, sx = 0, sy = 0;
        if (x < 0) { sx = -x; w += x; x = 0; }
        if (y < 0) { sy = -y; h += y; y = 0; }
        if (ox + x + w > g->pmaxw) w = g->pmaxw - ox - x;
        if (oy + y + h > g->pmaxh) h = g->pmaxh - oy - y;
        for (int r = 0; r < h && w > 0; r++) {
            UINT64 src = b.buf + ((UINT64)(sy + r) * (UINT64)b.stride + (UINT64)sx) * 4;
            UINT64 dst = g->bitmap + ((UINT64)(oy + y + r) * g->stride + (UINT64)(ox + x)) * 4;
            UINT64 bk = g->back ? g->back + ((UINT64)(oy + y + r) * (UINT64)g->back_stride + (UINT64)(ox + x)) * 4 : 0;
            if (b.dir == 0) {
                if (!um_read(p, src, row, (UINT64)w * 4)) break;
                um_write(g->proc, dst, row, (UINT64)w * 4);
                if (bk) um_write(g->proc, bk, row, (UINT64)w * 4);
            } else {
                if (!um_read(g->proc, bk ? bk : dst, row, (UINT64)w * 4) || !um_write(p, src, row, (UINT64)w * 4)) break;
            }
        }
        rv = 1;
        if (b.dir == 0) WmInvalidate();
    }
    DesktopUnlock();
    kfree(row);
    return rv;
}

static UINT64 sys_gui_ctl(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    if (a2 == CTL_CURSOR) {
        INT32 c[3] = { WmCursorX(), WmCursorY(), (INT32)mk_flags() };
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, c, sizeof(c))) ? 1 : 0;
    }
    if (a2 == CTL_WORKAREA) {
        DesktopLock();
        GdiRect r = WmWorkArea();
        DesktopUnlock();
        INT32 c[4] = { r.x, r.y, r.w, r.h };
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, c, sizeof(c))) ? 1 : 0;
    }
    if (a2 == CTL_PRESENT) { WmInvalidate(); return 1; }
    if (a2 == CTL_SET_CURSOR) return cursor_from_user(p, a3, a4);
    if (a2 == CTL_SET_SYSCURSOR) return syscursor_set(a3, a4);
    if (a2 == CTL_SYSCURSOR_IMAGE) return syscursor_image(a3, a4);
    if (a2 == CTL_CURSOR_SHAPE) {
        INT32 out[6] = { 0 };
        int step;
        DesktopLock();
        const GdiCursorShape *c = WmCursorCurrent(&step);
        if (c && c == p->cursor && !c->sys) { out[0] = 1; out[1] = c->w; out[2] = c->h; out[3] = c->nframes; out[4] = step; }
        out[5] = WmCursorSysCurrent();
        if (out[5]) out[4] = step;                /* the busy ring's phase */
        DesktopUnlock();
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, out, a3 == 1 ? 6 * sizeof(INT32) : 5 * sizeof(INT32))) ? 1 : 0;
    }
    if (a2 == CTL_DISPLAY_MODE) return display_mode_info(a3, a4);
    if (a2 == CTL_SET_DISPLAY) return display_set(p, a4);
    if (a2 == CTL_MONITOR) return monitor_info(a3, a4);
    if (a2 == CTL_SET_DPI) return dpi_set(a4);
    if (a2 == CTL_HEAD_MODE) return head_mode_info(a4);
    if (a2 == CTL_ADAPTER) return adapter_info(a3, a4);
    if (a2 == CTL_SET_HEAD) return head_set(p, a4);
    if (a2 == CTL_WINDOW_AT) {
        INT32 pt[2], out[4] = { 0, 0, 0, 0 };
        if (!NT_SUCCESS(CopyFromUser(pt, (const void *)(uintptr_t)a4, sizeof(pt)))) return 0;
        DesktopLock();
        bool caption;
        WND *w = WmWindowAt(pt[0], pt[1], &caption);
        for (int i = 0; w && i < GUI_MAX_WINDOWS; i++) {
            GuiWin *g = &g_win[i];
            if (g->used && g->wnd == w && g->proc) { out[0] = (INT32)g->id; out[1] = (INT32)g->proc->pid; out[2] = (INT32)g->accept; break; }
        }
        DesktopUnlock();
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, out, sizeof(out))) ? 1 : 0;
    }
    if (a2 == CTL_ACCEPT_DROPS) {
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *g = win_of_handle(p, a1);
        if (g) g->accept = (UINT32)a3;
        spin_unlock_irqrestore(&g_gui_lock, s);
        return g ? 1 : 0;
    }
    if (a2 == CTL_DROP) {
        INT32 hd[4];
        if (!NT_SUCCESS(CopyFromUser(hd, (const void *)(uintptr_t)a4, sizeof(hd)))) return 0;
        UINT32 bytes = (UINT32)hd[3];
        if (bytes > DROP_MAX) return 0;
        UINT32 *buf = kzalloc(20 + bytes);
        if (!buf) return 0;
        buf[0] = (UINT32)hd[0]; buf[1] = (UINT32)hd[1]; buf[2] = (UINT32)hd[2]; buf[3] = p->pid; buf[4] = bytes;
        if (bytes && !NT_SUCCESS(CopyFromUser(buf + 5, (const void *)(uintptr_t)(a4 + 16), bytes))) { kfree(buf); return 0; }
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *g = win_by_id((UINT32)a3);
        void *old = NULL;
        UINT32 seq = 0;
        if (g) {
            old = g->drop;
            g->drop = buf; g->drop_len = 20 + bytes;
            if (!++g_drop_seq) g_drop_seq = 1;
            seq = g->drop_seq = g_drop_seq;
            enqueue_locked(g, WM_NOVA_DROP, bytes, (UINT64)(UINT32)hd[2], hd[0], hd[1]);
        }
        spin_unlock_irqrestore(&g_gui_lock, s);
        kfree(old);
        if (!g) { kfree(buf); return 0; }
        gui_wake(g->tid, WM_NOVA_DROP);
        return seq;
    }
    if (a2 == CTL_DROP_DONE) {
        static UINT32 next;
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *g = win_of_handle(p, a1);
        if (g && g->drop_seq) {
            g_drop_done[next][0] = g->drop_seq;
            g_drop_done[next][1] = (UINT32)a3;
            next = (next + 1) % DROP_RESULTS;
            g->drop_seq = 0;
        }
        spin_unlock_irqrestore(&g_gui_lock, s);
        return g ? 1 : 0;
    }
    if (a2 == CTL_DROP_STATUS) {
        UINT32 seq = (UINT32)a3;
        UINT64 r = ~0ULL;
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        for (int i = 0; seq && i < DROP_RESULTS; i++)
            if (g_drop_done[i][0] == seq) { r = 1 + (UINT64)g_drop_done[i][1]; g_drop_done[i][0] = 0; break; }
        for (int i = 0; seq && r == ~0ULL && i < GUI_MAX_WINDOWS; i++)
            if (g_win[i].used && g_win[i].drop_seq == seq) r = 0;
        spin_unlock_irqrestore(&g_gui_lock, s);
        return r;
    }
    if (a2 == CTL_DROP_FETCH) {
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *g = win_of_handle(p, a1);
        void *buf = g ? g->drop : NULL;
        UINT32 len = g ? g->drop_len : 0;
        if (g) { g->drop = NULL; g->drop_len = 0; }
        spin_unlock_irqrestore(&g_gui_lock, s);
        if (!buf) return 0;
        UINT64 r = len <= a3 && NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, buf, len)) ? len : 0;
        kfree(buf);
        return r;
    }
    if (a2 == CTL_HWND_TAG) return hwnd_tag(p);
    if (a2 == CTL_XSEND) return xmsg_send(p, a3, a4);
    if (a2 == CTL_XFETCH) return xmsg_fetch(p, a4);
    if (a2 == CTL_XREPLY) return xmsg_reply(p, a3, a4);
    if (a2 == CTL_XRESULT) return xmsg_result(p, a3, a4);
    if (a2 == CTL_EMBED) return embed_ctl(p, a1, a3, a4);
    if (a2 == CTL_XBLIT) return xblit(p, a3, a4);
    if (a2 == CTL_SET_BACK) {
        UINT64 bk[2] = { 0, 0 };
        if (a4 && !NT_SUCCESS(CopyFromUser(bk, (const void *)(uintptr_t)a4, sizeof(bk)))) return 0;
        DesktopLock();
        GuiWin *g = win_lookup(p, a1);
        if (g) { g->back = bk[0]; g->back_stride = (int)bk[1]; }
        DesktopUnlock();
        return g ? 1 : 0;
    }
    if (a2 == CTL_EMBED_INFO) {
        INT32 out[3] = { 0, 0, 0 };
        DesktopLock();
        GuiWin *g = win_lookup(p, a1);
        GuiWin *h = g ? embed_host(g) : NULL;
        if (h) {
            GdiRect hc = WmClientRect(h->wnd);
            out[0] = (INT32)g->parent_hwnd; out[1] = hc.x + g->pox; out[2] = hc.y + g->poy;
        }
        DesktopUnlock();
        return h && NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, out, sizeof(out))) ? 1 : 0;
    }
    if (a2 == CTL_TOUCH) return (UINT64)InputTouchContacts();
    if (a2 == CTL_TABLET) return tablet_ctl(p, a3, a4);
    if (a2 == CTL_GAMEPAD) return gamepad_ctl(a3, a4);
    if (a2 == CTL_SET_CURSOR_POS) return cursor_set_pos(p, a4);
    if (a2 == CTL_CLIP_CURSOR) return cursor_clip(p, a3, a4);
    if (a2 == CTL_FOREIGN) return hwnd_foreign((UINT32)a3, a4);
    if (a2 == CTL_SET_HWND) {
        INT32 uc[4] = { 0 };
        if (a4 && !NT_SUCCESS(CopyFromUser(uc, (const void *)(uintptr_t)a4, sizeof(uc)))) return 0;
        IrqState s = spin_lock_irqsave(&g_gui_lock);
        GuiWin *g = win_of_handle(p, a1);
        bool fresh = g && g->hwnd != (UINT32)a3;
        if (g) { g->hwnd = (UINT32)a3; if (a4) memcpy(g->uc, uc, sizeof(uc)); }
        spin_unlock_irqrestore(&g_gui_lock, s);
        if (fresh) {                                    /* (an embed may be waiting for it) */
            DesktopLock();
            g = win_lookup(p, a1);
            if (g && g->wnd && !g->host) embed_pending_check(g);
            DesktopUnlock();
        }
        return g ? 1 : 0;
    }
    if (a2 == CTL_WAKE) {
        IrqState ws = spin_lock_irqsave(&g_gui_lock);
        bool set = false;
        for (int i = 0; i < GUI_WAKES && !set; i++) if (g_wake_tid[i] == (UINT32)a3) set = true;
        for (int i = 0; i < GUI_WAKES && !set; i++) if (!g_wake_tid[i]) { g_wake_tid[i] = (UINT32)a3; set = true; }
        spin_unlock_irqrestore(&g_gui_lock, ws);
        gui_wake((UINT32)a3, 0);                        /* (user32 posted it a message) */
        return set;
    }
    UINT64 rv = 0;
    INT32 in[4] = { 0 };
    if (a2 == CTL_SET_RECT && !NT_SUCCESS(CopyFromUser(in, (const void *)(uintptr_t)a4, sizeof(in)))) return 0;
    DesktopLock();
    GuiWin *g = win_lookup(p, a1);
    WND *w = g ? g->wnd : NULL;
    if (w) {
        switch (a2) {
        case CTL_GET_RECT: {
            GdiRect c = WmClientRect(w), f = w->frame;
            bool vis = w->visible;
            if (g->host) {                      /* embedded: its whole client area (the WND shows a part) */
                client_pos(g, &c.x, &c.y);
                f = RECT(c.x, c.y, g->cw, g->ch);
                vis = g->want_vis;
            }
            INT32 out[9] = { c.x, c.y, g->cw, g->ch, f.x, f.y, f.w, f.h,
                             (vis ? 1 : 0) | (w->active ? 2 : 0) | (w->minimized ? 4 : 0) | (w->maximized ? 8 : 0) };
            DesktopUnlock();
            return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, out, sizeof(out))) ? 1 : 0;
        }
        case CTL_SET_RECT: {
            GdiRect c = WmClientRect(w);
            if (a3 & 2) {
                c.w = in[2] < 1 ? 1 : in[2] > g->maxw ? g->maxw : in[2];
                c.h = in[3] < 1 ? 1 : in[3] > g->maxh ? g->maxh : in[3];
                g->cw = c.w; g->ch = c.h;
            }
            GuiWin *eh = embed_host(g);
            if (eh) {                           /* embedded: a place in the parent (the screen position given) */
                if (a3 & 1) {
                    GdiRect hc = WmClientRect(eh->wnd);
                    g->rx = in[0] - hc.x - g->pox; g->ry = in[1] - hc.y - g->poy;
                }
                embed_place(g);
                client_pos(g, &g->last_x, &g->last_y);
                rv = 1;
                break;
            }
            if (a3 & 1) { c.x = in[0]; c.y = in[1]; }
            if (w->maximized || w->snapped) { w->maximized = w->snapped = false; }
            GdiRect f = frame_for(w->style, c);
            WmSetFrame(w, f);
            g->last_x = WmClientRect(w).x; g->last_y = WmClientRect(w).y;
            rv = 1;
            break;
        }
        case CTL_CAPTURE:
            if (a3) WmSetCapture(w);
            else if (WmGetCapture() == w) WmSetCapture(NULL);
            rv = 1;
            break;
        case CTL_ACTIVATE: {
            GuiWin *eh = embed_host(g);
            if (eh) eh->key_embed = g->id;      /* embedded: it takes its host's keyboard */
            else WmSetActive(w);
            rv = 1;
            break;
        }
        case CTL_SET_SCALE: {
            int k = a3 >= 2 ? 2 : 1;
            if (k == 2 && !gui_grow_bitmap(g)) k = 1;
            if (k != g->scale) {
                g->scale = k;
                g->maxw = g->pmaxw / k < g->lmaxw ? g->pmaxw / k : g->lmaxw;
                g->maxh = g->pmaxh / k < g->lmaxh ? g->pmaxh / k : g->lmaxh;
                if (g->cw > g->maxw) g->cw = g->maxw;
                if (g->ch > g->maxh) g->ch = g->maxh;
                WmInvalidate();
            }
            if (a4) {
                UINT32 geo[4] = { (UINT32)g->bitmap, (UINT32)(g->bitmap >> 32), (UINT32)g->stride * 4, (UINT32)g->pmaxh };
                DesktopUnlock();
                return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, geo, sizeof(geo))) ? (UINT64)k : 0;
            }
            rv = (UINT64)k;
            break;
        }
        case CTL_ENABLE:   w->disabled = a3 == 0; rv = 1; break;
        case CTL_SET_FRAME: {
            if (w->popup) break;
            if (g->host) {                      /* embedded: frameless now; the frame comes back when it is let go */
                g->saved_style = (a3 & 1) ? WS_TITLEBAR | WS_BORDER | WS_SHADOW | (a3 & 2 ? 0 : WS_CLOSEBTN) |
                                            (a3 & 4 ? 0 : WS_MINMAXBTN) : WS_SHADOW;
                g->saved_fixed = !(a3 & 8);
                rv = 1;
                break;
            }
            GdiRect c = WmClientRect(w);
            w->style = (a3 & 1) ? WS_TITLEBAR | WS_BORDER | WS_SHADOW | (a3 & 2 ? 0 : WS_CLOSEBTN) | (a3 & 4 ? 0 : WS_MINMAXBTN)
                                : WS_SHADOW;
            w->fixed_size = !(a3 & 8);
            WmSetFrame(w, frame_for(w->style, c));
            rv = 1;
            break;
        }
        case CTL_SHOW:
            if (g->host) {                      /* embedded: shown with its parent; never activated */
                g->want_vis = a3 != 0 && a3 != 2;
                embed_place(g);
                rv = 1;
                break;
            }
            switch (a3) {
            case 0: WmShowWindow(w, false); if (WmGetCapture() == w) WmSetCapture(NULL); break;
            case 1: WmShowWindow(w, true); if (!w->no_activate) WmSetActive(w); break;
            case 2: WmMinimize(w); break;
            case 3: WmShowWindow(w, true); WmSetActive(w); WmSnap(w, WM_SNAP_MAX); break;
            case 4: WmShowWindow(w, true); WmSnap(w, WM_SNAP_RESTORE); if (!w->no_activate) WmSetActive(w); break;
            case 5: WmShowWindow(w, true); break;
            }
            rv = 1;
            break;
        }
        WmInvalidate();
    }
    DesktopUnlock();
    return rv;
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
    UINT32 w = GdiScreenW(), h = GdiScreenH();       /* fixed once the desktop is up */
    if (a1) CopyToUser((void *)(uintptr_t)a1, &w, 4);
    if (a2) CopyToUser((void *)(uintptr_t)a2, &h, 4);
    return ((UINT64)h << 32) | w;
}

static UINT64 sys_gui_postmessage(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    UmProcess *p = UmCurrent();
    IrqState s = spin_lock_irqsave(&g_gui_lock);
    GuiWin *g = win_of_handle(p, a1);
    if (g) {
        if ((UINT32)a2 == WM_QUIT) g->quit = true;
        else enqueue_locked(g, (UINT32)a2, a3, a4, 0, 0);
    }
    spin_unlock_irqrestore(&g_gui_lock, s);
    if (g) gui_wake(g->tid, 0);                     /* (a posted message: no input boost) */
    return g ? 1 : 0;
}

/* Called from UmPoll when a process has exited: tear down its windows. */
void um_gui_process_gone(UmProcess *p)
{
    DesktopLock();
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        GuiWin *g = &g_win[i];
        if (g->used && g->proc == p) {
            if (g->wnd && WmGetCapture() == g->wnd) WmSetCapture(NULL);
            if (g->wnd) { g->wnd->user = NULL; WmDestroyWindow(g->wnd); g->wnd = NULL; }
            IrqState s = spin_lock_irqsave(&g_gui_lock);
            void *drop = g->drop;
            g->drop = NULL; g->drop_len = 0; g->accept = 0; g->drop_seq = 0;
            g->used = false;
            spin_unlock_irqrestore(&g_gui_lock, s);
            kfree(drop);
        }
    }
    xmsg_process_gone(p);
    IrqState ts = spin_lock_irqsave(&g_gui_lock);
    for (int i = GUI_TAG_MIN; i < GUI_TAGS; i++) if (g_tag_proc[i] == p) g_tag_proc[i] = NULL;
    spin_unlock_irqrestore(&g_gui_lock, ts);
    WmInvalidate();
    TabletOwnerGone(p);                       /* its synthetic pens */
    if (p->cursor) {                          /* no window shows its pointer now */
        WmCursorShapeChanged();
        kfree(p->cursor);
        p->cursor = NULL;
    }
    if (g_fullscreen_proc == p) {             /* its full-screen mode ends with it */
        g_fullscreen_proc = NULL;
        DisplayMode m = DisplayDefaultMode();
        DesktopSetDisplayMode(m.w, m.h);
    }
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
    um_install(SYSCALL_NtNovaGuiCtl,         sys_gui_ctl);
}
