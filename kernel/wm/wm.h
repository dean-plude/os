/*
 * wm.h — NovaOS Window Manager
 *
 * Owns top-level windows: z-order, focus, Windows 11 style frames with
 * caption buttons, dragging, minimize/maximize/close, and routing of mouse
 * and keyboard input to the focused window.  Applications are callbacks
 * that run on the desktop thread (there are no user-mode GUI apps yet).
 *
 * All coordinates are logical pixels (see gdi.h).
 */

#pragma once

#include "../include/types.h"
#include "../gdi/gdi.h"
#include "input.h"

#define WM_MAX_WINDOWS   64
#define WM_TITLE_MAX     64
#define WM_TITLEBAR_H    32

/* Window styles */
#define WS_TITLEBAR   (1u << 0)   /* title bar (drag handle) */
#define WS_CLOSEBTN   (1u << 1)   /* close button */
#define WS_MINMAXBTN  (1u << 2)   /* minimize / maximize buttons */
#define WS_BORDER     (1u << 3)   /* 1px border */
#define WS_SHADOW     (1u << 4)   /* drop shadow */
#define WS_TOOLWINDOW (WS_TITLEBAR | WS_CLOSEBTN | WS_BORDER | WS_SHADOW)
#define WS_OVERLAPPED (WS_TITLEBAR | WS_CLOSEBTN | WS_MINMAXBTN | WS_BORDER | WS_SHADOW)

/* Mouse messages delivered to a window (client-relative coordinates) */
typedef enum {
    WM_MOUSE_DOWN = 1,     /* left button pressed */
    WM_MOUSE_UP,           /* left button released */
    WM_MOUSE_MOVE,         /* motion (while captured or hovering) */
    WM_MOUSE_DBLCLK,       /* second press of a double click */
    /* windows with WND.rbutton / WND.hover set also get: */
    WM_MOUSE_RDOWN,        /* right button */
    WM_MOUSE_RUP,
    WM_MOUSE_MDOWN,        /* middle button */
    WM_MOUSE_MUP,
    WM_MOUSE_WHEEL,        /* the wheel turned: WmWheelDelta() notches (+ = away from the user) */
    WM_MOUSE_LEAVE,        /* the pointer left the client area (hover windows) */
} WmMouseMsg;

struct WND;
typedef void (*WndPaintFn)(struct WND *w);
typedef void (*WndKeyFn)(struct WND *w, const KeyEvent *k);
typedef void (*WndMouseFn)(struct WND *w, WmMouseMsg msg, int x, int y);
typedef void (*WndCloseFn)(struct WND *w);
/* Called every pass of the desktop loop; return true to request a redraw
 * (used for work that completes asynchronously, e.g. network requests). */
typedef bool (*WndTickFn)(struct WND *w);

typedef struct WND {
    int        id;
    GdiRect    frame;        /* outer rect on screen (includes title bar) */
    GdiRect    restore;      /* frame to restore after maximize */
    GdiRect    wanted;       /* the frame before a smaller display mode shrank or
                              * moved it (w 0: none); it gets it back when the
                              * mode grows again */
    GdiRect    shrunk;       /* the frame that change left it with: moved or
                              * resized since, it keeps the new one instead */
    char       title[WM_TITLE_MAX];
    UINT32     style;
    GdiColor   client_bg;
    GdiColor   accent;       /* colour of the small app mark in the title */
    bool       visible;
    bool       minimized;
    bool       maximized;
    bool       active;       /* has keyboard focus */
    bool       snapped;      /* tiled to half the screen (restore holds the size) */
    bool       fixed_size;   /* no resizing, snapping or maximizing (program windows
                              * whose client bitmap has a fixed size) */
    int        z;            /* z-order; higher = nearer the top */
    int        app;          /* owning app id (for the dock), or -1 */
    char       program[96];  /* program image path (e.g. "C:\\Programs\\winhello.exe") for
                              * windows of Windows programs: their icon */
    bool       popup;        /* menus, drop-downs, tooltips: no frame, not in the
                              * dock or Alt+Tab, above every normal window */
    bool       no_activate;  /* a click does not take the focus (menus) */
    bool       hover;        /* on_mouse also gets moves with no button held
                              * while the pointer is over the client area */
    bool       rbutton;      /* on_mouse also gets right/middle buttons and the wheel */
    bool       disabled;     /* input goes to the windows it owns (a modal dialog) */
    int        owner;        /* id of the window this one belongs to (0: none);
                              * it stays above its owner */

    WndPaintFn on_paint;     /* draw the client area (clip is set) */
    bool       paint_lock_free; /* on_paint needs only the desktop lock (else it
                              * runs under the big kernel lock: smp.h) */
    WndKeyFn   on_key;       /* key pressed while focused */
    bool       key_releases; /* on_key also gets releases (pressed = false) */
    WndMouseFn on_mouse;     /* mouse in / captured by the client area */
    WndCloseFn on_close;     /* window is being destroyed: free `user` */
    WndCloseFn on_close_request; /* if set, the close button and Alt+F4 call this
                              * instead of closing (the owner decides) */
    WndTickFn  on_tick;      /* optional periodic work */
    bool       tick_lock_free; /* on_tick takes the file-system lock itself
                              * if it needs it (else WmTick holds it) */
    const GdiCursorShape *cursor; /* the pointer over the client area (a program's
                              * SetCursor), or NULL: the arrow.  Set under the
                              * desktop lock, then WmCursorShapeChanged() */
    void      *user;         /* app state */
} WND;

/* -----------------------------------------------------------------------
 * Windows
 * ----------------------------------------------------------------------- */
void WmInitialize(void);

WND *WmCreateWindow(const char *title, GdiRect frame, UINT32 style,
                    GdiColor client_bg, GdiColor accent,
                    WndPaintFn on_paint, void *user);
/* The same, optionally without taking the focus (popups) */
WND *WmCreateWindowEx(const char *title, GdiRect frame, UINT32 style,
                      GdiColor client_bg, GdiColor accent,
                      WndPaintFn on_paint, void *user, bool activate);
/* Move/resize by the outer frame (a program asking) */
void WmSetFrame(WND *w, GdiRect frame);
/* Every mouse event goes to @w's on_mouse until released (NULL) */
void WmSetCapture(WND *w);
WND *WmGetCapture(void);
/* Close: calls on_close, then frees the slot. */
void WmDestroyWindow(WND *w);
void WmShowWindow(WND *w, bool visible);
void WmSetTitle(WND *w, const char *title);

/* Focus and raise (also restores a minimized window). */
void WmSetActive(WND *w);
WND *WmActiveWindow(void);

void WmMinimize(WND *w);
/* The close button / Alt+F4: asks on_close_request, or closes */
void WmRequestClose(WND *w);
void WmToggleMaximize(WND *w);

/* Client area in screen coordinates. */
GdiRect WmClientRect(const WND *w);

/* Topmost window belonging to app `app`, or NULL. */
WND *WmFindApp(int app);
int  WmWindowCount(void);
/* Windows (minimized ones too), topmost first; returns how many (<= max). */
int  WmListWindows(WND **out, int max);

/* Tile a window: WM_SNAP_LEFT/RIGHT (half the work area), WM_SNAP_MAX,
 * or WM_SNAP_RESTORE (back to its normal size and place). */
enum { WM_SNAP_RESTORE, WM_SNAP_LEFT, WM_SNAP_RIGHT, WM_SNAP_MAX };
void WmSnap(WND *w, int where);
/* Win+D: minimize every window, or bring back the ones it minimized. */
void WmShowDesktopToggle(void);
/* The topmost shown window at (x, y), or NULL; *caption tells whether the
 * point is on its title bar (may be NULL). */
WND *WmWindowAt(int x, int y, bool *caption);
/* A window by id (NULL if it is gone). */
WND *WmWindowById(int id);

/* Draws a window's icon in its title bar (set by the app layer); returns
 * false if the window has none, and it then shows a dot in its accent
 * colour. */
typedef bool (*WmIconFn)(const WND *w, int x, int y, int size);
void WmSetIconPainter(WmIconFn fn);

/* Area windows may occupy (the screen minus the dock). */
void WmSetWorkArea(GdiRect r);
GdiRect WmWorkArea(void);

/* -----------------------------------------------------------------------
 * Input routing (called by the desktop event loop)
 * ----------------------------------------------------------------------- */

/* Mouse press/release/double-click at screen (x, y).  Returns true if a
 * window took it (the desktop should then ignore it). */
bool WmMouseButton(int x, int y, WmMouseMsg msg);
/* Mouse moved to (x, y): drags, hover highlights, captured client moves. */
void WmMouseMove(int x, int y);
/* Right/middle button or wheel (@msg: WM_MOUSE_R*, M*, WHEEL with @dz
 * notches) for a window that takes them; false if none did (the desktop's
 * own menus then). */
bool WmMouseOther(int x, int y, WmMouseMsg msg, int dz);
int  WmWheelDelta(void);
/* The buttons held (bit 0 left, 1 right, 2 middle), kept by the desktop */
void   WmSetButtons(UINT32 b);
UINT32 WmButtons(void);
/* Run every window's on_tick hook (called by the desktop loop). */
void WmTick(void);
/* True while a window drag or client capture is in progress. */
bool WmMouseCaptured(void);
/* Deliver a key press to the focused window.  Alt+F4 closes it.
 * Returns true if a window took it. */
bool WmKey(const KeyEvent *k);

/* -----------------------------------------------------------------------
 * Composition
 * ----------------------------------------------------------------------- */
typedef void (*WmLayerFn)(void);

/* The desktop shell draws the background (wallpaper, icons: cached
 * between frames) and the overlay (dock, Start menu: always on top). */
void WmSetDesktop(WmLayerFn background, WmLayerFn overlay);

/* Mark the scene as needing a redraw / the background as changed. */
void WmInvalidate(void);
void WmInvalidateBackground(void);
bool WmNeedsRedraw(void);

/* Render one full frame into the back buffer and present it. */
void WmComposite(void);

/* -----------------------------------------------------------------------
 * Software mouse cursor (drawn directly to the screen with save-under, so
 * moving it does not require recompositing the scene).  Coordinates are
 * logical pixels.
 * ----------------------------------------------------------------------- */
void WmCursorShow(int x, int y);
void WmCursorHide(void);
void WmCursorMove(int x, int y);
/* Move by a relative mouse delta (one count = one logical pixel). */
void WmCursorMoveBy(int dx, int dy);
/* Move to a position given as 0-65535 across and down the screen (tablets) */
void WmCursorMoveAbs(int nx, int ny);
/* Re-show the cursor after a new frame was presented. */
void WmCursorReshow(void);
/* A window's cursor shape changed (or was freed): redraw the pointer at
 * the next tick.  Under the desktop lock. */
void WmCursorShapeChanged(void);
/* The pointer's current frame and step: shape (NULL = the arrow), step */
const GdiCursorShape *WmCursorCurrent(int *step);

/* The display mode changed (GdiDisplayChanged done, work area set): refit
 * the windows and the pointer.  @old_w/@old_h/@old_s: the old logical
 * size and scale. */
void WmDisplayChanged(int old_w, int old_h, int old_s);
int  WmCursorX(void);
int  WmCursorY(void);
