/*
 * wm.h — NovaOS Window Manager
 *
 * Phase 7.  A minimal compositing window manager built on the GDI
 * software renderer.  It maintains a Z-ordered list of windows, draws
 * decorated frames (title bar, close/min/max buttons, client area), and
 * composites them over the desktop wallpaper.
 *
 * This is a kernel-side WM — windows are described by WND structures owned
 * by the kernel.  A future Phase will expose USER32-style HWND handles to
 * user mode via the syscall layer; for now the desktop shell drives it
 * directly to render the login/desktop experience.
 */

#pragma once

#include "../include/types.h"
#include "../gdi/gdi.h"

#define WM_MAX_WINDOWS   32
#define WM_TITLE_MAX     64

/* Window style flags */
#define WS_TITLEBAR   (1u << 0)   /* draw a title bar */
#define WS_CLOSEBTN   (1u << 1)   /* draw close button */
#define WS_MINMAXBTN  (1u << 2)   /* draw minimise / maximise buttons */
#define WS_BORDER     (1u << 3)   /* draw a 1px border */
#define WS_SHADOW     (1u << 4)   /* draw a drop shadow */

#define WS_TOOLWINDOW (WS_TITLEBAR | WS_CLOSEBTN | WS_BORDER | WS_SHADOW)
#define WS_OVERLAPPED (WS_TITLEBAR | WS_CLOSEBTN | WS_MINMAXBTN | WS_BORDER | WS_SHADOW)

/* A paint callback renders the client area in window-local coordinates.
 * The WM has already filled the client background and set up the frame. */
struct WND;
typedef void (*WndPaintFn)(struct WND *w);

typedef struct WND {
    int        id;
    GdiRect    frame;        /* outer rect on screen (includes title bar) */
    char       title[WM_TITLE_MAX];
    UINT32     style;
    GdiColor   client_bg;
    GdiColor   accent;       /* title-bar accent color */
    bool       visible;
    bool       active;       /* has focus (brighter title bar) */
    int        z;            /* z-order; higher = nearer the top */
    WndPaintFn on_paint;
    void      *user;         /* opaque pointer for the paint callback */
} WND;

/* Title-bar height in pixels. */
#define WM_TITLEBAR_H   28

/* -----------------------------------------------------------------------
 * Lifecycle / API
 * ----------------------------------------------------------------------- */
void WmInitialize(void);

/* Create a window.  Returns a WND* owned by the WM, or NULL if full. */
WND *WmCreateWindow(const char *title, GdiRect frame, UINT32 style,
                    GdiColor client_bg, GdiColor accent,
                    WndPaintFn on_paint, void *user);

void WmDestroyWindow(WND *w);
void WmShowWindow(WND *w, bool visible);
void WmSetActive(WND *w);

/* Return the client rectangle (interior, below the title bar) in screen
 * coordinates for the given window. */
GdiRect WmClientRect(const WND *w);

/* Composite the whole scene: desktop background callback first, then all
 * visible windows in z-order, then the taskbar/overlay callback.
 *
 * The desktop shell registers the background/overlay via WmSetDesktop(). */
typedef void (*WmLayerFn)(void);
void WmSetDesktop(WmLayerFn background, WmLayerFn overlay);

/* Render one full frame. */
void WmComposite(void);

int  WmWindowCount(void);

/* -----------------------------------------------------------------------
 * Software mouse cursor (drawn directly to the front buffer with
 * save-under, so moving it does not require recompositing the scene).
 * ----------------------------------------------------------------------- */

/* Draw the cursor at (x,y), saving the pixels beneath it. */
void WmCursorShow(int x, int y);
/* Restore the pixels beneath the cursor (if currently shown). */
void WmCursorHide(void);
/* Hide at the old position, then show at the new (clamped) position. */
void WmCursorMove(int x, int y);
/* Re-show the cursor after a full WmComposite() wiped it (re-grabs the
 * save-under from the freshly drawn scene). */
void WmCursorReshow(void);

int  WmCursorX(void);
int  WmCursorY(void);
