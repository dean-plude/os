/*
 * generic.c — OpenGL with no driver installed: a minimal OpenGL 1.1 of
 * NovaOS's own, as Windows' "GDI Generic" is what a Windows machine
 * without a display driver's OpenGL has.
 *
 * A program can choose and set a pixel format, create a context, make it
 * current, ask what it got (GL_VERSION "1.1.0", GL_RENDERER "GDI Generic",
 * no extensions, no wglGetProcAddress functions), clear, read pixels back
 * and swap buffers onto its window.  That is what programs that need more
 * look at before they fall back to drawing without OpenGL (Qt, Krita).
 * Programs that draw in 2D with OpenGL 1.1 itself (ScummVM, SDL's OpenGL
 * renderer) get what they need for that (draw.c): textures, the matrix
 * stacks, vertex arrays and glBegin/glEnd, and triangles filled with a
 * colour or a texture, blended and scissored; no lighting, depth, fog,
 * lines or points (those calls do nothing).  The App Store's Mesa 3D or
 * Venus replace all of it with a real OpenGL.
 */
#include <windows.h>

void *memset(void *d, int c, size_t n);
typedef HANDLE HGLRC;
int _fltused = 0x9875;      /* the compiler references it for float code */

typedef struct {
    WORD nSize, nVersion;
    DWORD dwFlags;
    BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift,
         cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits,
         cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved;
    DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PFD;


#include "generic.h"

static DWORD g_tls = TLS_OUT_OF_INDEXES;
typedef struct { Ctx *ctx; HDC dc; } Cur;

static Cur *cur(int create)
{
    if (g_tls == TLS_OUT_OF_INDEXES) {
        DWORD i = TlsAlloc();
        if (InterlockedCompareExchange((LONG *)&g_tls, (LONG)i, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
            TlsFree(i);
    }
    Cur *c = TlsGetValue(g_tls);
    if (!c && create) {
        c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
        TlsSetValue(g_tls, c);
    }
    return c;
}

Ctx *current(void)
{
    Cur *c = cur(0);
    return c ? c->ctx : 0;
}

/* The window's client size: the back buffer's */
static void size_of(HDC dc, int *w, int *h)
{
    RECT r = { 0, 0, 1, 1 };
    HWND wnd = WindowFromDC(dc);
    if (wnd) GetClientRect(wnd, &r);
    *w = r.right - r.left > 0 ? r.right - r.left : 1;
    *h = r.bottom - r.top > 0 ? r.bottom - r.top : 1;
}

int fit(Ctx *x, HDC dc)
{
    int w, h;
    size_of(dc, &w, &h);
    if (x->back && x->w == w && x->h == h) return 1;
    DWORD *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)w * h * 4);
    if (!b) return 0;
    if (x->back) HeapFree(GetProcessHeap(), 0, x->back);
    x->back = b; x->w = w; x->h = h;
    return 1;
}

/* The current context with its back buffer the window's size (draw.c) */
Ctx *current_drawing(void)
{
    Cur *c = cur(0);
    Ctx *x = c ? c->ctx : 0;
    return x && fit(x, c->dc) ? x : 0;
}

/* ---- wgl ---- */
static int WINAPI g_ChoosePixelFormat(HDC dc, const PFD *want) { (void)dc; (void)want; return 1; }

static int WINAPI g_DescribePixelFormat(HDC dc, int fmt, UINT size, PFD *p)
{
    (void)dc;
    if (!p) return 1;
    if (fmt != 1 || size < sizeof(PFD)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    memset(p, 0, sizeof(*p));
    p->nSize = sizeof(PFD);
    p->nVersion = 1;
    p->dwFlags = 0x4 | 0x20 | 0x1 | 0x40;   /* PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_GENERIC_FORMAT */
    p->iPixelType = 0;                      /* PFD_TYPE_RGBA */
    p->cColorBits = 32;
    p->cRedBits = 8; p->cRedShift = 16;
    p->cGreenBits = 8; p->cGreenShift = 8;
    p->cBlueBits = 8; p->cBlueShift = 0;
    p->cAlphaBits = 8; p->cAlphaShift = 24;
    p->cDepthBits = 24;
    p->cStencilBits = 8;
    return 1;
}

static BOOL WINAPI g_SetPixelFormat(HDC dc, int fmt, const PFD *p) { (void)dc; (void)p; return fmt == 1; }
static int WINAPI g_GetPixelFormat(HDC dc) { (void)dc; return 1; }

static HGLRC WINAPI g_CreateContext(HDC dc)
{
    (void)dc;
    Ctx *x = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*x));
    if (!x) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    x->magic = CTX_MAGIC;
    gl_init_state(x);
    return (HGLRC)x;
}

static Ctx *ctx_of(HGLRC h)
{
    Ctx *x = (Ctx *)h;
    return x && x->magic == CTX_MAGIC ? x : 0;
}

static BOOL WINAPI g_DeleteContext(HGLRC h)
{
    Ctx *x = ctx_of(h);
    if (!x) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    Cur *c = cur(0);
    if (c && c->ctx == x) { c->ctx = 0; c->dc = 0; }
    x->magic = 0;
    gl_free_state(x);
    if (x->back) HeapFree(GetProcessHeap(), 0, x->back);
    HeapFree(GetProcessHeap(), 0, x);
    return TRUE;
}

static BOOL WINAPI g_MakeCurrent(HDC dc, HGLRC h)
{
    Cur *c = cur(1);
    if (!c) return FALSE;
    if (!h) { c->ctx = 0; c->dc = 0; return TRUE; }
    Ctx *x = ctx_of(h);
    if (!x || !dc) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    c->ctx = x;
    c->dc = dc;
    if (!x->back) {                          /* the first time: the viewport is the window */
        fit(x, dc);
        x->vp[2] = x->w; x->vp[3] = x->h;
    }
    return TRUE;
}

static HGLRC WINAPI g_GetCurrentContext(void) { return (HGLRC)current(); }
static HDC WINAPI g_GetCurrentDC(void) { Cur *c = cur(0); return c && c->ctx ? c->dc : 0; }
static BOOL WINAPI g_ShareLists(HGLRC a, HGLRC b) { return ctx_of(a) && ctx_of(b); }

static BOOL WINAPI g_SwapBuffers(HDC dc)
{
    Cur *c = cur(0);
    Ctx *x = c && c->dc == dc ? c->ctx : 0;
    if (!x || !x->back) return TRUE;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = x->w;
    bi.bmiHeader.biHeight = x->h;            /* bottom-up, as OpenGL's rows */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(dc, 0, 0, (DWORD)x->w, (DWORD)x->h, 0, 0, 0, (UINT)x->h, x->back, &bi, DIB_RGB_COLORS);
    return TRUE;
}

/* ---- gl ---- */
static const unsigned char *WINAPI g_GetString(unsigned name)
{
    if (!current()) return 0;
    switch (name) {
    case GL_VENDOR: return (const unsigned char *)"NovaOS";
    case GL_RENDERER: return (const unsigned char *)"GDI Generic";
    case GL_VERSION: return (const unsigned char *)"1.1.0";
    case GL_EXTENSIONS: return (const unsigned char *)"";
    }
    Ctx *x = current();
    x->error = GL_INVALID_ENUM;
    return 0;
}

static unsigned WINAPI g_GetError(void)
{
    Ctx *x = current();
    if (!x) return 0;
    unsigned e = x->error;
    x->error = 0;
    return e;
}

static void WINAPI g_GetIntegerv(unsigned name, int *v)
{
    Ctx *x = current();
    if (!x || !v) return;
    switch (name) {
    case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = x->vp[i]; return;
    case GL_MAX_TEXTURE_SIZE: v[0] = GL_TEX_MAX; return;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; i++) v[i] = x->scissor[i]; return;
    case GL_MATRIX_MODE: v[0] = 0x1700 + x->mode; return;
    case GL_TEXTURE_BINDING_2D: v[0] = (int)x->bound; return;
    case GL_UNPACK_ALIGNMENT: v[0] = x->unpack_align; return;
    case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 16384; return;
    }
    v[0] = 0;
}

static void WINAPI g_Viewport(int x0, int y0, int w, int h)
{
    Ctx *x = current();
    if (!x) return;
    x->vp[0] = x0; x->vp[1] = y0; x->vp[2] = w; x->vp[3] = h;
}

static BYTE unit(float f) { return (BYTE)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255.0f + 0.5f); }

static void WINAPI g_ClearColor(float r, float g, float b, float a)
{
    Ctx *x = current();
    if (!x) return;
    x->clear[0] = unit(b); x->clear[1] = unit(g); x->clear[2] = unit(r); x->clear[3] = unit(a);
}

static void WINAPI g_Clear(unsigned mask)
{
    Cur *c = cur(0);
    Ctx *x = c ? c->ctx : 0;
    if (!x || !(mask & GL_COLOR_BUFFER_BIT) || !fit(x, c->dc)) return;
    DWORD v = (DWORD)x->clear[0] | (DWORD)x->clear[1] << 8 | (DWORD)x->clear[2] << 16 | (DWORD)x->clear[3] << 24;
    int x0 = 0, y0 = 0, x1 = x->w, y1 = x->h;
    if (x->enabled & EN_SCISSOR) clip_scissor(x, &x0, &y0, &x1, &y1);
    for (int r = y0; r < y1; r++)
        for (int k = x0; k < x1; k++) x->back[r * x->w + k] = v;
}

static void WINAPI g_ReadPixels(int x0, int y0, int w, int h, unsigned format, unsigned type, void *data)
{
    Ctx *x = current();
    if (!x || !data || w <= 0 || h <= 0) return;
    if (type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_BGRA)) { x->error = GL_INVALID_ENUM; return; }
    BYTE *out = data;
    for (int r = 0; r < h; r++)
        for (int k = 0; k < w; k++, out += 4) {
            int px = x0 + k, py = y0 + r;
            DWORD v = x->back && px >= 0 && py >= 0 && px < x->w && py < x->h ? x->back[py * x->w + px] : 0;
            BYTE b = (BYTE)v, g = (BYTE)(v >> 8), rd = (BYTE)(v >> 16), a = (BYTE)(v >> 24);
            if (format == GL_RGBA) { out[0] = rd; out[1] = g; out[2] = b; out[3] = a; }
            else { out[0] = b; out[1] = g; out[2] = rd; out[3] = a; }
        }
}

/* What the front's exports jump to when no driver is installed */
const struct { const char *name; void *fn; } gl_generic[] = {
    { "wglChoosePixelFormat", (void *)g_ChoosePixelFormat },
    { "wglDescribePixelFormat", (void *)g_DescribePixelFormat },
    { "wglSetPixelFormat", (void *)g_SetPixelFormat },
    { "wglGetPixelFormat", (void *)g_GetPixelFormat },
    { "wglCreateContext", (void *)g_CreateContext },
    { "wglDeleteContext", (void *)g_DeleteContext },
    { "wglMakeCurrent", (void *)g_MakeCurrent },
    { "wglGetCurrentContext", (void *)g_GetCurrentContext },
    { "wglGetCurrentDC", (void *)g_GetCurrentDC },
    { "wglShareLists", (void *)g_ShareLists },
    { "wglSwapBuffers", (void *)g_SwapBuffers },
    { "glGetString", (void *)g_GetString },
    { "glGetError", (void *)g_GetError },
    { "glGetIntegerv", (void *)g_GetIntegerv },
    { "glViewport", (void *)g_Viewport },
    { "glClearColor", (void *)g_ClearColor },
    { "glClear", (void *)g_Clear },
    { "glReadPixels", (void *)g_ReadPixels },
    { 0, 0 }
};
