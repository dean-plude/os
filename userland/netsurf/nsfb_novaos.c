/*
 * nsfb_novaos.c — a libnsfb surface that is a NovaOS desktop window
 *
 * The framebuffer is the window's client bitmap itself (user32 maps it
 * into the process; its pixels are COLORREFs, 0x00BBGGRR, which is
 * libnsfb's XBGR8888), so NetSurf's plotters draw straight into it and an
 * update only has to ask the window manager to present it.  Input comes
 * from the window's Win32 message queue: set-1 scancodes are mapped to
 * libnsfb's (SDL-style, unshifted) key codes — the framebuffer frontend
 * applies shift itself — and mouse messages to motion and button events.
 * The desktop draws the mouse pointer, so the software cursor is never
 * plotted.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#define POLL_SLICE_MS 5

typedef struct {
    HWND          hwnd;
    NOVA_DC      *dc;
    bool          dirty;            /* drawn since last presented */
    nsfb_event_t  pending[4];       /* events split from one message */
    int           npending;
    int           x, y;             /* last pointer position */
} NovaSurface;

static const char *g_title = "NetSurf";

/* Set-1 scancode -> libnsfb key code (unshifted US layout) */
static const unsigned short g_keymap[0x60] = {
    [0x01] = NSFB_KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = NSFB_KEY_BACKSPACE, [0x0F] = NSFB_KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = NSFB_KEY_RETURN, [0x1D] = NSFB_KEY_LCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2A] = NSFB_KEY_LSHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = NSFB_KEY_RSHIFT, [0x37] = NSFB_KEY_KP_MULTIPLY, [0x38] = NSFB_KEY_LALT,
    [0x39] = ' ', [0x3A] = NSFB_KEY_CAPSLOCK,
    [0x3B] = NSFB_KEY_F1, [0x3C] = NSFB_KEY_F2, [0x3D] = NSFB_KEY_F3, [0x3E] = NSFB_KEY_F4,
    [0x3F] = NSFB_KEY_F5, [0x40] = NSFB_KEY_F6, [0x41] = NSFB_KEY_F7, [0x42] = NSFB_KEY_F8,
    [0x43] = NSFB_KEY_F9, [0x44] = NSFB_KEY_F10,
    [0x47] = NSFB_KEY_KP7, [0x48] = NSFB_KEY_KP8, [0x49] = NSFB_KEY_KP9, [0x4A] = NSFB_KEY_KP_MINUS,
    [0x4B] = NSFB_KEY_KP4, [0x4C] = NSFB_KEY_KP5, [0x4D] = NSFB_KEY_KP6, [0x4E] = NSFB_KEY_KP_PLUS,
    [0x4F] = NSFB_KEY_KP1, [0x50] = NSFB_KEY_KP2, [0x51] = NSFB_KEY_KP3, [0x52] = NSFB_KEY_KP0,
    [0x53] = NSFB_KEY_KP_PERIOD,
    [0x57] = NSFB_KEY_F11, [0x58] = NSFB_KEY_F12,
};

/* E0-prefixed keys */
static int ext_key(int sc)
{
    switch (sc) {
    case 0x1C: return NSFB_KEY_KP_ENTER;
    case 0x1D: return NSFB_KEY_RCTRL;
    case 0x35: return NSFB_KEY_KP_DIVIDE;
    case 0x38: return NSFB_KEY_RALT;
    case 0x47: return NSFB_KEY_HOME;
    case 0x48: return NSFB_KEY_UP;
    case 0x49: return NSFB_KEY_PAGEUP;
    case 0x4B: return NSFB_KEY_LEFT;
    case 0x4D: return NSFB_KEY_RIGHT;
    case 0x4F: return NSFB_KEY_END;
    case 0x50: return NSFB_KEY_DOWN;
    case 0x51: return NSFB_KEY_PAGEDOWN;
    case 0x52: return NSFB_KEY_INSERT;
    case 0x53: return NSFB_KEY_DELETE;
    default:   return NSFB_KEY_UNKNOWN;
    }
}

static LRESULT __stdcall wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    /* the bitmap always holds the current frame: nothing to paint */
    if (msg == WM_PAINT || msg == WM_ERASEBKGND) return 0;
    if (msg == WM_CLOSE) return 0;                  /* handled as a quit event */
    return DefWindowProcA(h, msg, wp, lp);
}

static void present(NovaSurface *s)
{
    if (!s->dirty) return;
    s->dirty = false;
    InvalidateRect(s->hwnd, NULL, FALSE);
}

static int nova_defaults(nsfb_t *nsfb)
{
    nsfb->width = 800;
    nsfb->height = 600;
    nsfb->format = NSFB_FMT_XBGR8888;
    select_plotters(nsfb);
    return 0;
}

static int nova_initialise(nsfb_t *nsfb)
{
    if (nsfb->surface_priv) return -1;
    NovaSurface *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = "NetSurfNovaOS";
    RegisterClassA(&wc);
    s->hwnd = CreateWindowA("NetSurfNovaOS", g_title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            nsfb->width, nsfb->height, NULL, NULL, NULL, NULL);
    if (!s->hwnd) { free(s); return -1; }
    s->dc = (NOVA_DC *)GetDC(s->hwnd);
    if (!s->dc || s->dc->w < nsfb->width || s->dc->h < nsfb->height) {
        /* the desktop may have made it smaller than asked */
        if (s->dc) { nsfb->width = s->dc->w; nsfb->height = s->dc->h; }
    }
    if (!s->dc) { DestroyWindow(s->hwnd); free(s); return -1; }

    nsfb->format = NSFB_FMT_XBGR8888;               /* COLORREF */
    select_plotters(nsfb);
    nsfb->ptr = (uint8_t *)s->dc->bits;
    nsfb->linelen = s->dc->stride * 4;
    nsfb->surface_priv = s;

    ShowWindow(s->hwnd, SW_SHOW);
    s->dirty = true;
    present(s);
    return 0;
}

static int nova_finalise(nsfb_t *nsfb)
{
    NovaSurface *s = nsfb->surface_priv;
    if (!s) return 0;
    DestroyWindow(s->hwnd);
    free(s);
    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    return 0;
}

static int nova_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    NovaSurface *s = nsfb->surface_priv;
    (void)format;
    if (s) {
        /* the window's bitmap is fixed: never grow past it */
        if (width > s->dc->w) width = s->dc->w;
        if (height > s->dc->h) height = s->dc->h;
    }
    if (width > 0) nsfb->width = width;
    if (height > 0) nsfb->height = height;
    nsfb->format = NSFB_FMT_XBGR8888;
    select_plotters(nsfb);
    if (s) nsfb->linelen = s->dc->stride * 4;
    return 0;
}

static bool pop_pending(NovaSurface *s, nsfb_event_t *event)
{
    if (!s->npending) return false;
    *event = s->pending[0];
    memmove(s->pending, s->pending + 1, (size_t)(--s->npending) * sizeof(nsfb_event_t));
    return true;
}

static void push(NovaSurface *s, enum nsfb_event_type_e type, int code)
{
    if (s->npending >= 4) return;
    nsfb_event_t *e = &s->pending[s->npending++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->value.keycode = code;
}

static void push_move(NovaSurface *s, int x, int y)
{
    if (s->npending >= 4) return;
    nsfb_event_t *e = &s->pending[s->npending++];
    memset(e, 0, sizeof(*e));
    e->type = NSFB_EVENT_MOVE_ABSOLUTE;
    e->value.vector.x = x;
    e->value.vector.y = y;
    e->value.vector.z = 0;
    s->x = x;
    s->y = y;
}

/* Translate one window message into pending events */
static void translate(NovaSurface *s, const MSG *m)
{
    int x = (short)(m->lParam & 0xFFFF), y = (short)((m->lParam >> 16) & 0xFFFF);
    switch (m->message) {
    case WM_MOUSEMOVE:
        push_move(s, x, y);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (x != s->x || y != s->y) push_move(s, x, y);
        push(s, NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_1);
        break;
    case WM_LBUTTONUP:
        if (x != s->x || y != s->y) push_move(s, x, y);
        push(s, NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_1);
        break;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        int sc = (int)(m->wParam & 0x7F);
        bool extended = (m->lParam >> 24) & 1;
        int code = extended ? ext_key(sc) : sc < 0x60 ? g_keymap[sc] : 0;
        if (code)
            push(s, m->message == WM_KEYDOWN ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, code);
        break;
    }
    case WM_CLOSE:
    case WM_QUIT: {
        nsfb_event_t *e = &s->pending[s->npending < 4 ? s->npending++ : 3];
        memset(e, 0, sizeof(*e));
        e->type = NSFB_EVENT_CONTROL;
        e->value.controlcode = NSFB_CONTROL_QUIT;
        break;
    }
    default:
        break;
    }
}

static bool nova_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    NovaSurface *s = nsfb->surface_priv;
    if (!s) return false;
    present(s);
    if (pop_pending(s, event)) return true;
    ULONGLONG deadline = timeout > 0 ? GetTickCount64() + (ULONGLONG)timeout : 0;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { translate(s, &m); break; }
            if (m.message != WM_PAINT && m.message != WM_TIMER) DispatchMessageA(&m);
            translate(s, &m);
            if (s->npending) break;
        }
        if (pop_pending(s, event)) return true;
        if (timeout == 0) return false;
        if (timeout > 0 && GetTickCount64() >= deadline) {
            memset(event, 0, sizeof(*event));
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_TIMEOUT;
            return true;
        }
        Sleep(POLL_SLICE_MS);
    }
}

static int nova_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb; (void)box;
    return 0;
}

static int nova_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    NovaSurface *s = nsfb->surface_priv;
    (void)box;
    if (s) s->dirty = true;             /* presented when the frontend next waits */
    return 0;
}

static int nova_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    (void)nsfb; (void)cursor;           /* the desktop draws the pointer */
    return true;
}

static int nova_parameters(nsfb_t *nsfb, const char *parameters)
{
    (void)nsfb;
    if (parameters && *parameters) g_title = parameters;
    return 0;
}

const nsfb_surface_rtns_t novaos_rtns = {
    .defaults = nova_defaults,
    .initialise = nova_initialise,
    .finalise = nova_finalise,
    .geometry = nova_geometry,
    .parameters = nova_parameters,
    .input = nova_input,
    .claim = nova_claim,
    .update = nova_update,
    .cursor = nova_cursor,
};

/* libnsfb has no NovaOS surface type; take the slot of SDL, which isn't built */
NSFB_SURFACE_DEF(novaos, NSFB_SURFACE_SDL, &novaos_rtns)
