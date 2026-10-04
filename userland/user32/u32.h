/*
 * u32.h — what user32's sources share (not exported)
 *
 * The window system lives in the program's own process.  Each top-level
 * window owns a window of the desktop (the kernel's GUI syscalls): a frame
 * the desktop draws, and a bitmap for everything inside it.  Child windows
 * are user32's alone: they are rectangles of their top-level's bitmap.
 * Programs draw into a back buffer; user32 copies what changed to the
 * bitmap the desktop reads ("presents") when the program has no more
 * painting to do, so the screen never shows a half-drawn frame.
 */
#pragma once
#define NOVA_BUILD_USER32
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>

#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define abs(x) __builtin_abs(x)
#define PM_QS_NOPAINT_ 0x80000000u         /* internal: no WM_PAINT from this peek */
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Kernel GuiCreate struct (matches um_gui.c) */
typedef struct {
    INT32 x, y, w, h; UINT32 style; UINT64 title;
    UINT64 hwnd, bitmap; UINT32 stride, cw, ch;
    UINT32 flags; UINT64 owner;
    UINT32 rows, _pad;              /* out: the bitmap's height in pixels */
} GuiCreate;
#define GUI_POPUP     0x01
#define GUI_RESIZABLE 0x02
#define GUI_NOMINMAX  0x04
#define GUI_NOACTIVATE 0x08
#define GUI_HIDDEN    0x10
#define GUI_NOCLOSE   0x20
#define GUI_HOVER     0x40
#define GUI_NOFRAME   0x80
enum { CTL_GET_RECT = 1, CTL_SET_RECT, CTL_CAPTURE, CTL_CURSOR, CTL_ACTIVATE, CTL_ENABLE, CTL_SHOW, CTL_PRESENT,
       CTL_WORKAREA, CTL_WAKE, CTL_WINDOW_AT, CTL_ACCEPT_DROPS, CTL_DROP, CTL_DROP_FETCH,
       CTL_DISPLAY_MODE, CTL_SET_DISPLAY, CTL_DROP_DONE, CTL_DROP_STATUS, CTL_SET_CURSOR, CTL_CURSOR_SHAPE,
       CTL_HWND_TAG, CTL_SET_HWND, CTL_FOREIGN, CTL_MONITOR, CTL_HEAD_MODE, CTL_SET_HEAD,
       CTL_SET_SYSCURSOR, CTL_SYSCURSOR_IMAGE, CTL_TOUCH, CTL_TABLET, CTL_SET_DPI, CTL_SET_SCALE, CTL_GAMEPAD,
       CTL_SET_CURSOR_POS, CTL_CLIP_CURSOR, CTL_SET_FRAME,
       /* windows of different processes (xproc.c; um_gui.c says what each does) */
       CTL_XSEND = 48, CTL_XFETCH, CTL_XREPLY, CTL_XRESULT, CTL_EMBED, CTL_EMBED_INFO, CTL_XBLIT, CTL_SET_BACK };
/* display.c: the monitors (GetSystemMetrics' virtual screen) */
int  u32_monitor_count(void);
void u32_virtual_screen(RECT *r);
#define WM_NOVA_TOUCH 0x03FD                    /* from the desktop: a touch contact (pointer.c) */
#define WM_NOVA_DPI   0x03FC                    /* from the desktop: a monitor's DPI changed (dpi.c) */
#define WM_NOVA_RESCALE 0x03FB                  /* to a built-in control: its window's DPI changed (dpi.c) */

/* dpi.c: DPI awareness and the coordinates a DPI-aware process sees */
enum { DPI_UNAWARE = 0, DPI_SYSTEM_AWARE = 1, DPI_PER_MONITOR_AWARE = 2 };
typedef struct Wnd Wnd;
int  dpi_mode(void);                /* the process's awareness (decided at the first call) */
int  dpi_aware(void);               /* the calling thread's isn't DPI_UNAWARE: coordinates may differ from the desktop's */
int  dpi_wnd_aware(Wnd *w);         /* ...the window's (its top-level's context) isn't */
int  dpi_k(Wnd *w);                 /* w's top-level window's pixels per logical pixel */
int  dpi_sys_k(void);               /* the system DPI / 96 the calling thread sees */
int  dpi_new_k(Wnd *owner);         /* the scale a top-level window this thread makes gets */
int  dpi_enter(Wnd *w, HANDLE *saved);  /* the thread takes the window's context (1: put *saved back) */
void dpi_leave(HANDLE saved);
void dpi_to_proc(POINT *p);         /* a logical screen point -> the calling thread's */
void dpi_to_logical(POINT *p);
void dpi_rect_to_proc(RECT *r);
void dpi_monitor_to_proc(int i, RECT *r, RECT *work);
int  dpi_monitor_dpi(int i);
int  dpi_monitor_raw(int i, RECT *r, RECT *work, int *scale, int *dpi);   /* CTL_MONITOR: the count, 0 none */
void dpi_refresh(void);
void dpi_from_kernel(Wnd *w, const INT32 r[9], int k, RECT *rect, POINT *bmp, int *bw, int *bh);
void dpi_to_kernel(Wnd *w, const RECT *b, INT32 out[4]);
void dpi_desktop_rect(RECT *r);
int  dpi_check(Wnd *w, const INT32 *kr);
void dpi_monitors_changed(Wnd *top);
int  dpi_new_window(Wnd *w, const RECT *b);
int  dpi_apply_scale(Wnd *w, int k);  /* CTL_SET_SCALE and the bitmap it gives: the scale set */
HANDLE dpi_thread_context(void);
/* Window @w's coordinates (its awareness's) and the calling thread's: a
 * screen point either way, and a length (client coordinates, sizes) */
void dpi_wnd_to_thread(Wnd *w, POINT *p);
void dpi_thread_to_wnd(Wnd *w, POINT *p);
int  dpi_len_to_thread(Wnd *w, int v);
int  dpi_len_to_wnd(Wnd *w, int v);
void dlg_dpi_changed(Wnd *w, int ok, int nk);   /* dialog.c: a per-monitor v2 dialog's controls and font */
BOOL adjust_window_rect(LPRECT r, DWORD style, BOOL menu, DWORD ex, int k);   /* win.c */
#define WM_NOVA_DROP 0x03FE                     /* from the desktop: a drop from another program (drop.c) */
#define WM_NOVA_EMBED 0x03F4                    /* from the desktop: another process embedded the window (wParam: the parent) or let it go (0) */
#define FRAME_TITLE 32                          /* the desktop's title bar */
#define FRAME_BORDER 1

/* -----------------------------------------------------------------------
 * Classes and windows
 * ----------------------------------------------------------------------- */
typedef struct WClass {
    int       used;
    WCHAR     name[257];                /* up to 256 characters, as on Windows */
    ATOM      atom;
    WNDPROC   proc;                 /* as registered */
    int       wide;                 /* registered with the W functions */
    HBRUSH    brush;
    HICON     icon, icon_sm;
    HCURSOR   cursor;
    HINSTANCE inst;
    UINT      style;
    int       extra, cls_extra;
    BYTE     *cls_data;
    LPCWSTR   menu;                 /* menu name (an ID or a copied string) */
    int       system;               /* a built-in class (global) */
} WClass;

typedef struct Prop { struct Prop *next; ATOM atom; WCHAR *name; HANDLE data; } Prop;

typedef struct ScrollBar {
    int min, max, page, pos, track;
    int shown, disabled;            /* shown: the style bit decides for window scroll bars */
    int arrows;                     /* ESB_* */
    int tracking;                   /* the thumb is being dragged (to `track`) */
} ScrollBar;

struct Wnd {
    int       used;
    UINT      gen;
    HWND      h;
    WClass   *cls;
    WNDPROC   proc;
    int       wide;                 /* the window procedure takes Unicode */
    DWORD     style, exstyle;
    LONG_PTR  id;                   /* GWLP_ID (the menu for a top-level window) */
    LONG_PTR  userdata;
    HINSTANCE inst;
    DWORD     tid;
    Wnd      *parent, *owner;       /* parent NULL: a top-level window */
    Wnd      *child, *next, *prev;  /* children, topmost first; siblings */
    RECT      rect;                 /* window rectangle: parent's client coordinates (screen for top-level) */
    RECT      client;               /* client rectangle, same coordinates */
    WCHAR    *text;
    BYTE     *extra;
    Prop     *props;
    HFONT     font;                 /* WM_SETFONT (controls) */
    HICON     icon_big, icon_small;
    HMENU     menu;                 /* the menu bar (top-level windows) */
    HMENU     sysmenu;
    ScrollBar sb[2];                /* SB_HORZ, SB_VERT */
    void     *ctl;                  /* a built-in control's state */
    int       flags;
    /* painting */
    RECT      upd;                  /* update rectangle, client coordinates (NC: whole window) */
    int       has_upd, erase, nc_paint, internal_paint;
    HDC       paint_dc;     /* BeginPaint's DC, until EndPaint (which presents) */
    void     *kept;                 /* WS_CLIPCHILDREN: the children's pixels kept over the painting */
    /* top-level windows: the desktop window and its bitmap */
    UINT32    kid;
    DWORD     drop_accept;          /* CTL_ACCEPT_DROPS flags (drop.c) */
    DWORD    *front, *back;
    int       stride, maxw, maxh;
    int       bw, bh;               /* bitmap size */
    POINT     bmp;                  /* bitmap origin, screen coordinates */
    RECT      dirty;                /* changed part of the back buffer (bitmap coordinates) */
    int       has_dirty;
    int       shown_kernel;         /* the desktop window is showing */
    int       minimized, maximized;
    RECT      normal;               /* restored window rectangle */
    HWND      focus_save;           /* the focus when it was deactivated */
    int       modal_depth;
    int       dpi_k;                /* top-level: bitmap pixels per logical pixel (dpi.c; 0 = 1) */
    int       kframe;               /* top-level: the frame the desktop draws (frame_flags; -1 a popup) */
    HANDLE    dpi_ctx;              /* the thread's DPI awareness context when it was made */
    INT32     klog[4];              /* DPI-aware: the bitmap rectangle last given the desktop */
    HWND      foreign_parent;       /* top-level: another process's window holds it (SetParent there; xproc.c) */
    void     *kback;                /* top-level: the back buffer the desktop was told of (CTL_SET_BACK) */
};
enum {
    WF_DESTROYING = 1, WF_DESTROYED = 2, WF_CREATED = 4, WF_DIALOG = 8, WF_MENU_TRACK = 16,
    WF_NOTIFYSENT = 32, WF_ERASEBK_DONE = 64, WF_HIDDEN_BY_OWNER = 128, WF_MAPPED = 256,
    WF_NEED_SIZE = 512,             /* WM_SIZE and WM_MOVE still owed, at the first ShowWindow */
    WF_TOUCH = 1024,                /* RegisterTouchWindow: WM_TOUCH instead of WM_POINTER* */
};

void u32_lock(void);                /* a recursive lock over the window table and the queues */
void u32_unlock(void);
#define LOCK()   u32_lock()
#define UNLOCK() u32_unlock()

/* Touch (pointer.c, with msg.c's routing) */
void  touch_from_kernel(Wnd *top, const MSG *km);
Wnd  *input_hit(Wnd *top, POINT pt, int *hit);                 /* the window (and part) at @pt */
void  input_queue(Wnd *w, UINT msg, WPARAM wp, LPARAM lp, DWORD time);
void  input_mouse(Wnd *top, UINT msg, WPARAM mk, POINT pt, ULONG_PTR extra); /* a mouse message at screen @pt, routed */
#define MI_PEN_SIGNATURE   0xFF515700u                       /* GetMessageExtraInfo of a pen's mouse messages */
#define MI_TOUCH_SIGNATURE 0xFF515780u                       /* ... and of a touch's */
LRESULT touch_default(Wnd *w, UINT msg, WPARAM wp, LPARAM lp);  /* DefWindowProc: mouse promotion */
/* Pens and the mouse as pointers (pointer.c): a mouse message for @target
 * at screen @pt over part @hit (WM_NCHITTEST's; HTCLIENT when captured),
 * from the pen packet @pen (0: the mouse); 1 if it became WM_POINTER* */
int   pointer_from_mouse(Wnd *target, UINT msg, WPARAM mk, POINT pt, DWORD time, UINT32 pen, int hit);
void  pointer_left(Wnd *top);          /* the desktop's WM_MOUSELEAVE for @top: the pen (or mouse) left it */
BOOL  raw_nolegacy(BOOL keyboard);      /* rawinput.c: the mouse's (keyboard's) input is raw input only (RIDEV_NOLEGACY) */
HWND  recently_active(void);         /* win.c: the window that was active a moment ago (none is now) */
void  pointer_taken(const MSG *m);     /* GetMessage took @m (GetPointerInfo answers for it) */

int   hwnd_foreign(HWND h);           /* another process's handle */
int   foreign_info(HWND h, INT32 f[11]); /* CTL_FOREIGN: 2 desktop window, 1 other window, 0 none */
Wnd  *W(HWND h);                    /* NULL (and ERROR_INVALID_WINDOW_HANDLE) if not a window */
Wnd  *W_quiet(HWND h);
Wnd  *top_of(Wnd *w);               /* the top-level window holding @w */
WClass *find_class_w(LPCWSTR name, HINSTANCE inst);
void  wnd_screen_origin(Wnd *w, int client, POINT *p);      /* the window's (client) origin on screen */
void  wnd_to_bitmap(Wnd *w, int client, POINT *p);          /* ...in its top-level's bitmap */
int   wnd_visible(Wnd *w);          /* the window and all its ancestors are visible */
void  wnd_calc_client(Wnd *w);      /* WM_NCCALCSIZE for the current rectangle */
void  wnd_set_pos(Wnd *w, HWND after, int x, int y, int cx, int cy, UINT flags);
void  top_sync_from_kernel(Wnd *w, int sized);
void  update_kernel_rect(Wnd *w);
void  default_nc_calc(Wnd *w, RECT *r);
void  top_activated(Wnd *w, int active);
Wnd  *window_at(POINT pt);
int   ensure_back(Wnd *top);
ATOM  register_system_class(LPCWSTR name, WNDPROC proc, UINT style, int extra, HBRUSH brush, HCURSOR cursor);
int   is_child_of(Wnd *parent, Wnd *w);
LRESULT send_msg(Wnd *w, UINT msg, WPARAM wp, LPARAM lp);  /* SendMessageW */
LRESULT call_proc(Wnd *w, WNDPROC proc, int wide, HWND h, UINT msg, WPARAM wp, LPARAM lp, int from_wide);
WCHAR *wstrdup(const WCHAR *s);
int   wlen(const WCHAR *s);
WCHAR *a2w(const char *s, int n);   /* malloc'd UTF-16 copy (ANSI is UTF-8) */
char  *w2a(const WCHAR *s, int n);
int   wcsicmp_(const WCHAR *a, const WCHAR *b);
void  set_text(Wnd *w, const WCHAR *s);
void  notify_parent(Wnd *w, UINT code);                     /* WM_COMMAND to the parent */
void  destroy_children(Wnd *w);
extern HWND g_focus, g_active, g_capture;
extern int  g_capture_nc;           /* capture taken by a scroll bar / menu (internal tracking) */
extern DWORD g_main_tid;
HWND  set_focus(HWND h);

/* -----------------------------------------------------------------------
 * Messages (msg.c)
 * ----------------------------------------------------------------------- */
BOOL  post_msg(Wnd *w, HWND h, UINT msg, WPARAM wp, LPARAM lp);
BOOL  post_thread(DWORD tid, UINT msg, WPARAM wp, LPARAM lp);
int   pump_one(MSG *m, HWND h, UINT mn, UINT mx, UINT flags, int wait, DWORD timeout);
void  process_sent(void);
extern BYTE g_keys[256];
BOOL kbd_set_default(HKL hkl);       /* kbd.c: SPI_SETDEFAULTINPUTLANG */
extern int g_alt_tap;              /* msg.c: Alt pressed alone so far */
extern POINT g_cursor;
void  kill_window_timers(HWND h);
void  remove_window_messages(HWND h);
UINT  set_timer_internal(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn, int system);

/* -----------------------------------------------------------------------
 * Painting (paint.c)
 * ----------------------------------------------------------------------- */
void  invalidate(Wnd *w, const RECT *r, int erase, int children);  /* r: client coordinates (NULL: all) */
void  invalidate_nc(Wnd *w);
void  validate(Wnd *w, const RECT *r);
Wnd  *next_paint(DWORD tid, HWND filter);
void  present(Wnd *top);            /* copy the dirty part of the back buffer to the screen */
void  present_thread(DWORD tid);
void  mark_dirty(Wnd *top, const RECT *r);                  /* bitmap coordinates */
HDC   wnd_dc(Wnd *w, int client, int clip_children);       /* a DC on the window */
void  release_dc(HDC dc);
void  dcs_follow(Wnd *top);                               /* kept DCs onto the window's bitmap as it is now */
void  nc_paint(Wnd *w);
void  paint_drop_kept(Wnd *w);
LRESULT cbt_hook(int code, WPARAM wp, LPARAM lp);   /* WH_CBT (msg.c) */
void  scroll_bits(Wnd *w, int dx, int dy, const RECT *area);
void  caret_hide_for(Wnd *w);
void  caret_restore(void);
void  caret_blink(void);
void  top_resized(Wnd *top);

/* -----------------------------------------------------------------------
 * Drawing helpers (draw.c)
 * ----------------------------------------------------------------------- */
COLORREF sys_color(int i);
HBRUSH sys_brush(int i);
HFONT  gui_font(void);              /* the dialog/control font at 96 DPI */
HFONT  gui_font_k(int k);           /* ...at k times 96 DPI */
HFONT  gui_font_bold_k(int k);
int    is_gui_font(HFONT f);
HFONT  ctl_font(Wnd *w);            /* a control's font: WM_SETFONT's, else gui_font at its DPI */
void   fill_rect(HDC dc, const RECT *r, COLORREF c);
void   frame_rect(HDC dc, const RECT *r, COLORREF c);
void   draw_text_w(HDC dc, const WCHAR *s, int n, RECT *r, UINT fmt);
int    text_width(HDC dc, const WCHAR *s, int n);
int    font_height(HDC dc);
void   draw_check_mark(HDC dc, int x, int y, int size, COLORREF c);
void   draw_arrow(HDC dc, const RECT *r, int dir, COLORREF c);  /* 0 up, 1 down, 2 left, 3 right */
void   draw_focus(HDC dc, const RECT *r);
void   draw_icon(HDC dc, int x, int y, HICON icon, int cx, int cy);
HBRUSH ctl_color(Wnd *w, UINT msg, HDC dc);                     /* WM_CTLCOLOR* from the parent */
int    icon_size(HICON h, int *cx, int *cy);

/* -----------------------------------------------------------------------
 * Built-in controls (their window procedures take Unicode)
 * ----------------------------------------------------------------------- */
LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ListBoxProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ComboProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ComboLBoxProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ScrollBarProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK MenuWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK DesktopProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK MDIClientProc(HWND, UINT, WPARAM, LPARAM);   /* mdi.c */
LRESULT CALLBACK DefDlgProcW(HWND, UINT, WPARAM, LPARAM);
void   register_builtin_classes(void);
int    combo_edit_key(Wnd *edit, UINT msg, WPARAM wp);
int    combo_edit_wants_all(Wnd *edit);
int    lb_is_string_msg(Wnd *w, UINT msg);                     /* A/W conversion helpers */
int    cb_is_string_msg(Wnd *w, UINT msg);

/* scroll bars (scroll.c) */
void   sb_draw(Wnd *w, HDC dc, int bar, const RECT *r, int vert);
void   sb_nc_rects(Wnd *w, RECT *h, RECT *v, RECT *corner);  /* window coordinates */
void   sb_track(Wnd *w, int bar, POINT pt);                    /* pt: screen */
int    sb_width(void);
int    sb_width_k(int k);           /* at k times 96 DPI */

/* menus (menu.c) */
int    menu_bar_height(Wnd *w, int width);
void   menu_bar_draw(Wnd *w, HDC dc, const RECT *r);
void   menu_track_bar(Wnd *w, POINT pt, int key);             /* click on the bar, or Alt/F10/Alt+key */
int    menu_translate_sys(Wnd *w, UINT msg, WPARAM wp);
void   menu_cancel(void);
extern Wnd *g_menu_owner;

/* dialogs (dialog.c) */
BOOL   dlg_nav(HWND dlg, MSG *m);

/* live handles of user32's own objects (menus, icons, accelerator tables) */
void   handle_add(void *p);
void   handle_remove(void *p);
int    handle_live(const void *p);

/* resources (res.c) */
/* An icon or cursor: its images (one per size, chained by `more`); an
 * animated one (.ani) has `ani`: its frames (frames[0] is this one) and the
 * steps that show them.  sys: one of the system's cursors (IDC_*). */
typedef struct Ani { int nframes, nsteps; struct Icon **frames; DWORD *seq, *rate; } Ani;
typedef struct Icon { DWORD magic; int w, h; int cursor; int shared; POINT hot; DWORD *argb; struct Icon *more;
                      int sys; DWORD serial; Ani *ani; } Icon;
#define ICON_MAGIC 0x49434F4Eu
Icon  *icon_of(HICON h);
Icon  *icon_step(Icon *ic, UINT step);  /* the frame an animated icon shows at @step */
void   cursor_to_kernel(HCURSOR c, int hidden);  /* the pointer over our windows */
int    display_scale(void);              /* device pixels per logical pixel */
HICON  load_icon_res(HINSTANCE inst, LPCWSTR name, int cx, int cy, int cursor);
HBITMAP load_bitmap_res(HINSTANCE inst, LPCWSTR name, UINT flags);
HICON  sys_icon(int which);          /* IDI_* */
const void *find_res(HINSTANCE inst, LPCWSTR name, LPCWSTR type, DWORD *size);

/* Windows of other processes (xproc.c).  Calls on another process's
 * window run in that process, in the thread that owns the window, as a
 * message would; a window can be embedded in another process's (SetParent) */
int     x_claim(void);                      /* take what other processes sent us; 1 if any */
BOOL    x_post(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT x_send(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide, DWORD timeout, int *failed);
BOOL    x_set_pos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL    x_show(HWND h, int cmd);
LONG_PTR x_get_long(HWND h, int index, int *failed);
LONG_PTR x_set_long(HWND h, int index, LONG_PTR v, int *failed);
HWND    x_set_focus(HWND h);
BOOL    x_enable(HWND h, BOOL on);
DWORD   x_thread(HWND h);
int     x_get_text(HWND h, WCHAR *buf, int n);       /* -1: failed */
int     x_class_name(HWND h, WCHAR *buf, int n);     /* 0: failed */
HWND    x_set_parent(HWND h, Wnd *parent);           /* embed another process's window (parent NULL: let it go) */
HWND    x_embed_parent(HWND h);                      /* the window of ours that holds it (0: none) */
void    embeds_follow(void);                         /* our windows moved: the embedded ones follow */
void    embed_notified(Wnd *top, HWND parent);       /* WM_NOVA_EMBED */
int     embed_origin(Wnd *top, POINT *p, HWND *root); /* where its foreign parent's client area is (thread coordinates) */
void    x_send_queue(DWORD tid, void (*fn)(void *), void *arg);   /* msg.c: run fn in thread tid's message loop */
int     ensure_kernel_window(Wnd *w);                /* win.c */

/* drop.c */
void drop_from_kernel(Wnd *top, const MSG *km);
Wnd *top_by_kid(UINT32 kid);
