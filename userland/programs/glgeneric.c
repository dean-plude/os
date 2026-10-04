/* glgeneric.exe — OpenGL with no OpenGL driver installed: NovaOS's own
 * opengl32.dll still gives a pixel format and a context, says it is
 * OpenGL 1.1 ("GDI Generic", as Windows without a display driver's
 * OpenGL), clears, reads back and swaps, so programs that want more see
 * what they got and fall back (Krita, Qt).  It also draws in 2D as
 * ScummVM does: a texture uploaded and updated, a quad of two triangles
 * through an orthographic projection (vertex arrays), blended, scissored,
 * modulated by the colour, and glBegin/glEnd quads.  Run before the App
 * Store installs Mesa 3D (the core self-tests). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* wingdi.h's pixel formats */
typedef struct tagPIXELFORMATDESCRIPTOR {
    WORD nSize, nVersion;
    DWORD dwFlags;
    BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift,
         cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits,
         cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved;
    DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PIXELFORMATDESCRIPTOR;
#define PFD_DOUBLEBUFFER 0x1
#define PFD_DRAW_TO_WINDOW 0x4
#define PFD_SUPPORT_OPENGL 0x20
#define PFD_GENERIC_FORMAT 0x40
#define PFD_TYPE_RGBA 0
__declspec(dllimport) int WINAPI ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR *);
__declspec(dllimport) int WINAPI DescribePixelFormat(HDC, int, UINT, PIXELFORMATDESCRIPTOR *);
__declspec(dllimport) BOOL WINAPI SetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR *);
__declspec(dllimport) BOOL WINAPI SwapBuffers(HDC);
typedef HANDLE HGLRC;

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_VIEWPORT 0x0BA2
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGB 0x1907
#define GL_FLOAT 0x1406
#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_VERTEX_ARRAY 0x8074
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_TRIANGLE_STRIP 5
#define GL_QUADS 7
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_NEAREST 0x2600
#define GL_UNPACK_ALIGNMENT 0x0CF5
/* from opengl32.dll by name (its 32-bit exports are undecorated, as Windows') */
static HGLRC (WINAPI *wglCreateContext)(HDC);
static BOOL (WINAPI *wglMakeCurrent)(HDC, HGLRC);
static BOOL (WINAPI *wglDeleteContext)(HGLRC);
static FARPROC (WINAPI *wglGetProcAddress)(LPCSTR);
static const unsigned char *(WINAPI *glGetString)(unsigned);
static void (WINAPI *glGetIntegerv)(unsigned, int *);
static void (WINAPI *glClearColor)(float, float, float, float);
static void (WINAPI *glClear)(unsigned);
static void (WINAPI *glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static unsigned (WINAPI *glGetError)(void);
static void (WINAPI *glEnable)(unsigned), (WINAPI *glDisable)(unsigned);
static void (WINAPI *glEnableClientState)(unsigned), (WINAPI *glDisableClientState)(unsigned);
static void (WINAPI *glVertexPointer)(int, unsigned, int, const void *), (WINAPI *glTexCoordPointer)(int, unsigned, int, const void *);
static void (WINAPI *glDrawArrays)(unsigned, int, int);
static void (WINAPI *glMatrixMode)(unsigned), (WINAPI *glLoadIdentity)(void);
static void (WINAPI *glOrtho)(double, double, double, double, double, double);
static void (WINAPI *glGenTextures)(int, unsigned *), (WINAPI *glBindTexture)(unsigned, unsigned);
static void (WINAPI *glDeleteTextures)(int, const unsigned *);
static void (WINAPI *glTexParameteri)(unsigned, unsigned, int), (WINAPI *glPixelStorei)(unsigned, int);
static void (WINAPI *glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (WINAPI *glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (WINAPI *glBlendFunc)(unsigned, unsigned), (WINAPI *glScissor)(int, int, int, int);
static void (WINAPI *glColor4f)(float, float, float, float);
static void (WINAPI *glBegin)(unsigned), (WINAPI *glEnd)(void), (WINAPI *glVertex2f)(float, float);

static int passed, failed;
static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

/* The colour at window pixel (x, y) counted from the top, as GetPixel counts */
static void at(int x, int y, int h, unsigned char *px) { glReadPixels(x, h - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }

/* 2D drawing as ScummVM's OpenGL renderer does it on OpenGL 1.1 */
static void draw_checks(HWND w)
{
    RECT r;
    GetClientRect(w, &r);
    int h = r.bottom;
    unsigned char px[4];
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, r.right, r.bottom, 0, -1, 1);                /* y down, as ScummVM's matrix */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* a 2x2 texture: red, green / blue, white; then its top-right texel turned yellow */
    unsigned tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    static const unsigned char texels[16] = { 255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 255, 255,  255, 255, 255, 255 };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    static const unsigned char yellow[3] = { 255, 255, 0 };
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, yellow);
    check("textures: glGenTextures, glTexImage2D, glTexSubImage2D", tex != 0 && glGetError() == 0);

    /* the texture over (10,10)-(110,110): a triangle strip from vertex arrays */
    static const float quad[8] = { 10, 10, 110, 10, 10, 110, 110, 110 }, uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, quad);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glColor4f(1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char tl[4], tr[4], bl[4], br[4], out[4];
    at(30, 30, h, tl); at(90, 30, h, tr); at(30, 90, h, bl); at(90, 90, h, br); at(120, 60, h, out);
    check("a textured quad: each quarter shows its texel",
          tl[0] == 255 && tl[1] == 0 && tr[0] == 255 && tr[1] == 255 && tr[2] == 0 &&
          bl[2] == 255 && bl[0] == 0 && br[0] == 255 && br[1] == 255 && br[2] == 255);
    check("nothing drawn outside the quad", out[0] == 0 && out[1] == 0 && out[2] == 0);
    at(10, 10, h, px);
    unsigned char px2[4];
    at(109, 109, h, px2);
    check("the quad covers its corner pixels", px[0] == 255 && px2[0] == 255 && px2[2] == 255);

    /* half-transparent white over black, blended: every pixel once, the diagonal too */
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    static const float quad2[8] = { 150, 10, 250, 10, 150, 110, 250, 110 };
    glVertexPointer(2, GL_FLOAT, 0, quad2);
    glColor4f(1, 1, 1, 0.5f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    int even = 1;
    for (int i = 0; i < 100; i += 3) {
        at(150 + i, 10 + i, h, px);                      /* along the diagonal two triangles share */
        if (px[0] < 120 || px[0] > 136) even = 0;
        at(150 + i, 109 - i, h, px);
        if (px[0] < 120 || px[0] > 136) even = 0;
    }
    check("blending: half white over black is grey, the shared edge blended once", even);
    glDisable(GL_BLEND);

    /* scissored glBegin/glEnd quad: only the scissor box changes */
    glEnable(GL_SCISSOR_TEST);
    glScissor(10, h - 200, 20, 20);                      /* window rows (10..30) x (180..200) from the top */
    glColor4f(0, 1, 0, 1);
    glBegin(GL_QUADS);
    glVertex2f(0, 150); glVertex2f(100, 150); glVertex2f(100, 220); glVertex2f(0, 220);
    glEnd();
    glDisable(GL_SCISSOR_TEST);
    at(20, 190, h, px); at(50, 190, h, px2);
    check("glBegin/glEnd, glScissor: inside green, outside untouched", px[1] == 255 && px[0] == 0 && px2[1] == 0);
    check("no errors", glGetError() == 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDeleteTextures(1, &tex);
}

int main(void)
{
    HMODULE gl = LoadLibraryA("opengl32.dll");
    check("opengl32.dll loads", gl != 0);
    if (!gl) { printf("glgeneric: %d passed, %d failed\n", passed, failed); return 1; }
#define LOAD(n) *(FARPROC *)&n = GetProcAddress(gl, #n)
    LOAD(wglCreateContext); LOAD(wglMakeCurrent); LOAD(wglDeleteContext); LOAD(wglGetProcAddress);
    LOAD(glGetString); LOAD(glGetIntegerv); LOAD(glClearColor); LOAD(glClear); LOAD(glReadPixels); LOAD(glGetError);
    LOAD(glEnable); LOAD(glDisable); LOAD(glEnableClientState); LOAD(glDisableClientState);
    LOAD(glVertexPointer); LOAD(glTexCoordPointer); LOAD(glDrawArrays); LOAD(glMatrixMode); LOAD(glLoadIdentity);
    LOAD(glOrtho); LOAD(glGenTextures); LOAD(glBindTexture); LOAD(glDeleteTextures); LOAD(glTexParameteri);
    LOAD(glPixelStorei); LOAD(glTexImage2D); LOAD(glTexSubImage2D); LOAD(glBlendFunc); LOAD(glScissor);
    LOAD(glColor4f); LOAD(glBegin); LOAD(glEnd); LOAD(glVertex2f);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "glgeneric";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND w = CreateWindowA("glgeneric", "glgeneric", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 320, 0, 0, wc.hInstance, 0);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int fmt = ChoosePixelFormat(dc, &pfd);
    check("ChoosePixelFormat", fmt > 0);
    PIXELFORMATDESCRIPTOR got;
    check("DescribePixelFormat: a generic 32-bit double-buffered format",
          DescribePixelFormat(dc, fmt, sizeof(got), &got) >= 1 && got.cColorBits == 32 &&
          (got.dwFlags & PFD_GENERIC_FORMAT) && (got.dwFlags & PFD_DOUBLEBUFFER));
    check("SetPixelFormat", SetPixelFormat(dc, fmt, &pfd));
    HGLRC rc = wglCreateContext(dc);
    check("wglCreateContext", rc != 0);
    check("wglMakeCurrent", rc && wglMakeCurrent(dc, rc));
    const char *ver = (const char *)glGetString(GL_VERSION), *ren = (const char *)glGetString(GL_RENDERER);
    printf("GL_VERSION %s, GL_RENDERER %s\n", ver ? ver : "(none)", ren ? ren : "(none)");
    check("OpenGL 1.1, GDI Generic", ver && !strncmp(ver, "1.1", 3) && ren && !strcmp(ren, "GDI Generic"));
    check("no extensions, no wglGetProcAddress functions",
          glGetString(GL_EXTENSIONS) && !*glGetString(GL_EXTENSIONS) && !wglGetProcAddress("glCreateShader"));
    int vp[4] = { -1, -1, -1, -1 };
    glGetIntegerv(GL_VIEWPORT, vp);
    RECT r;
    GetClientRect(w, &r);
    check("the viewport is the window", vp[0] == 0 && vp[1] == 0 && vp[2] == r.right && vp[3] == r.bottom);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = { 0 };
    glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check("glClear, glReadPixels", px[0] == 255 && px[1] == 0 && px[2] == 0 && glGetError() == 0);
    check("SwapBuffers", SwapBuffers(dc));
    COLORREF c = GetPixel(dc, 10, 10);
    check("the window shows the cleared colour", c == RGB(255, 0, 0));
    draw_checks(w);
    wglMakeCurrent(0, 0);
    check("wglDeleteContext", wglDeleteContext(rc));
    ReleaseDC(w, dc);
    DestroyWindow(w);
    printf("glgeneric: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
