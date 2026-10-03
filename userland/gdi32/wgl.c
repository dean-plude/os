/*
 * gdi32 — the OpenGL side of GDI: pixel formats and SwapBuffers.
 *
 * As on Windows, these go to the OpenGL implementation, opengl32.dll
 * (on NovaOS, Mesa's: a software renderer that draws through GDI): GDI
 * loads it on first use and calls its wgl* functions.  GDI itself only
 * remembers each window's pixel format, which may be set once.  OpenGL
 * calls back into these (Mesa sets the format through SetPixelFormat
 * too), so a call made while one is running on the same thread answers
 * from what GDI knows instead of going round again.
 */
#define NOVA_BUILD_GDI32
#include "gdi_int.h"

typedef struct tagPIXELFORMATDESCRIPTOR PIXELFORMATDESCRIPTOR;

static HMODULE g_gl;
static int (WINAPI *p_choose)(HDC, const PIXELFORMATDESCRIPTOR *);
static int (WINAPI *p_describe)(HDC, int, UINT, PIXELFORMATDESCRIPTOR *);
static BOOL (WINAPI *p_set)(HDC, int, const PIXELFORMATDESCRIPTOR *);
static BOOL (WINAPI *p_swap)(HDC);

static int gl_load(void)
{
    if (g_gl) return 1;
    HMODULE m = LoadLibraryA("opengl32.dll");
    if (!m) return 0;
    p_choose = (void *)GetProcAddress(m, "wglChoosePixelFormat");
    p_describe = (void *)GetProcAddress(m, "wglDescribePixelFormat");
    p_set = (void *)GetProcAddress(m, "wglSetPixelFormat");
    p_swap = (void *)GetProcAddress(m, "wglSwapBuffers");
    g_gl = m;
    return 1;
}

/* Re-entry from opengl32 on this thread (a TLS slot: nonzero inside) */
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static int inside(void)
{
    if (g_tls == TLS_OUT_OF_INDEXES) return 0;
    return TlsGetValue(g_tls) != NULL;
}
static void enter(int on)
{
    if (g_tls == TLS_OUT_OF_INDEXES) {
        DWORD i = TlsAlloc();
        if (InterlockedCompareExchange((LONG *)&g_tls, (LONG)i, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
            TlsFree(i);
    }
    TlsSetValue(g_tls, on ? (LPVOID)1 : NULL);
}

/* Each window's pixel format */
#define MAX_FORMATS 64
static struct { void *hwnd; int fmt; } g_fmt[MAX_FORMATS];
static SRWLOCK g_fmt_lock;

static void *window_of(HDC h)
{
    NOVA_DC *d = dc_of(h);
    return d && !d->mem ? d->hwnd : NULL;
}

static int format_of(void *hwnd)
{
    int f = 0;
    AcquireSRWLockShared(&g_fmt_lock);
    for (int i = 0; i < MAX_FORMATS; i++) if (g_fmt[i].hwnd == hwnd) { f = g_fmt[i].fmt; break; }
    ReleaseSRWLockShared(&g_fmt_lock);
    return f;
}

static void remember(void *hwnd, int fmt)
{
    AcquireSRWLockExclusive(&g_fmt_lock);
    static int next;
    int slot = -1;
    for (int i = 0; i < MAX_FORMATS; i++) {
        if (g_fmt[i].hwnd == hwnd) { slot = i; break; }
        if (slot < 0 && !g_fmt[i].hwnd) slot = i;
    }
    if (slot < 0) slot = next++ % MAX_FORMATS;       /* (the oldest go first) */
    g_fmt[slot].hwnd = hwnd;
    g_fmt[slot].fmt = fmt;
    ReleaseSRWLockExclusive(&g_fmt_lock);
}

GDIAPI int ChoosePixelFormat(HDC h, const PIXELFORMATDESCRIPTOR *pfd)
{
    if (!dc_of(h) || !pfd) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (inside() || !gl_load() || !p_choose) return 0;
    enter(1);
    int r = p_choose(h, pfd);
    enter(0);
    return r;
}

GDIAPI int DescribePixelFormat(HDC h, int fmt, UINT size, PIXELFORMATDESCRIPTOR *pfd)
{
    if (!dc_of(h)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (inside() || !gl_load() || !p_describe) return 0;
    enter(1);
    int r = p_describe(h, fmt, size, pfd);
    enter(0);
    return r;
}

GDIAPI int GetPixelFormat(HDC h)
{
    void *w = window_of(h);
    return w ? format_of(w) : 0;
}

GDIAPI BOOL SetPixelFormat(HDC h, int fmt, const PIXELFORMATDESCRIPTOR *pfd)
{
    void *w = window_of(h);
    if (!w || fmt <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    int cur = format_of(w);
    if (cur) {                                      /* once per window */
        if (cur == fmt) return TRUE;
        SetLastError(2000 /* ERROR_INVALID_PIXEL_FORMAT */);
        return FALSE;
    }
    remember(w, fmt);
    if (inside()) return TRUE;                      /* opengl32 telling GDI */
    if (!gl_load() || !p_set) return TRUE;
    enter(1);
    BOOL r = p_set(h, fmt, pfd);
    enter(0);
    if (!r) remember(w, 0);
    return r;
}

GDIAPI BOOL SwapBuffers(HDC h)
{
    if (!dc_of(h)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (inside() || !gl_load() || !p_swap) return FALSE;
    enter(1);
    BOOL r = p_swap(h);
    enter(0);
    return r;
}

