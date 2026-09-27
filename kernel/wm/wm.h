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

#define WM_MAX_WINDOWS   32
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
    char       title[WM_TITLE_MAX];
    UINT32     style;
    GdiColor   client_bg;
    GdiColor   accent;       /* colour of the small app mark in the title */
    bool       visible;
    bool       minimized;
    bool       maximized;
    bool       active;       /* has keyboard focus */
    int        z;            /* z-order; higher = nearer the top */
    int        app;          /* owning app id (for the dock), or -1 */

    WndPaintFn on_paint;     /* draw the client area (clip is set) */
    WndKeyFn   on_key;       /* key pressed while focused */
    WndMouseFn on_mouse;     /* mouse in / captured by the client area */
    WndCloseFn on_close;     /* window is being destroyed: free `user` */
    WndTickFn  on_tick;      /* optional periodic work */
    void      *user;         /* app state */
} WND;

/* -----------------------------------------------------------------------
 * Windows
 * ----------------------------------------------------------------------- */
void WmInitialize(void);

WND *WmCreateWindow(const char *title, GdiRect frame, UINT32 style,
                    GdiColor client_bg, GdiColor accent,
                    WndPaintFn on_paint, void *user);
/* Close: calls on_close, then frees the slot. */
void WmDestroyWindow(WND *w);
void WmShowWindow(WND *w, bool visible);
void WmSetTitle(WND *w, const char *title);

/* Focus and raise (also restores a minimized window). */
void WmSetActive(WND *w);
WND *WmActiveWindow(void);

void WmMinimize(WND *w);
void WmToggleMaximize(WND *w);

/* Client area in screen coordinates. */
GdiRect WmClientRect(const WND *w);

/* Topmost window belonging to app `app`, or NULL. */
WND *WmFindApp(int app);
int  WmWindowCount(void);

/* Draws a window's app icon in its title bar (set by the app layer);
 * windows without an app (app < 0) show a dot in their accent colour. */
typedef void (*WmIconFn)(int app, int x, int y, int size);
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
/* Re-show the cursor after a new frame was presented. */
void WmCursorReshow(void);
int  WmCursorX(void);
int  WmCursorY(void);
