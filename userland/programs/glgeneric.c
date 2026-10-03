/* glgeneric.exe — OpenGL with no OpenGL driver installed: NovaOS's own
 * opengl32.dll still gives a pixel format and a context, says it is
 * OpenGL 1.1 ("GDI Generic", as Windows without a display driver's
 * OpenGL), clears, reads back and swaps, so programs that want more see
 * what they got and fall back (Krita, Qt).  Run before the App Store
 * installs Mesa 3D (the core self-tests). */
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

static int passed, failed;
static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

int main(void)
{
    HMODULE gl = LoadLibraryA("opengl32.dll");
    check("opengl32.dll loads", gl != 0);
    if (!gl) { printf("glgeneric: %d passed, %d failed\n", passed, failed); return 1; }
#define LOAD(n) *(FARPROC *)&n = GetProcAddress(gl, #n)
    LOAD(wglCreateContext); LOAD(wglMakeCurrent); LOAD(wglDeleteContext); LOAD(wglGetProcAddress);
    LOAD(glGetString); LOAD(glGetIntegerv); LOAD(glClearColor); LOAD(glClear); LOAD(glReadPixels); LOAD(glGetError);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "glgeneric";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND w = CreateWindowA("glgeneric", "glgeneric", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240, 0, 0, wc.hInstance, 0);
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
    wglMakeCurrent(0, 0);
    check("wglDeleteContext", wglDeleteContext(rc));
    ReleaseDC(w, dc);
    DestroyWindow(w);
    printf("glgeneric: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
