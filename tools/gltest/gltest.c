/* gltest — OpenGL through opengl32.dll (on NovaOS: Mesa's virgl on a 3D
 * virtio-gpu, Mesa's llvmpipe otherwise): pixel format, context, clear,
 * immediate mode, GLSL shaders, read-back, then a few seconds of animated
 * frames presented with SwapBuffers.
 * Build: x86_64-w64-mingw32-gcc -O2 -o gltest.exe gltest.c -lopengl32 -lgdi32 -luser32 -lm
 *        (i686-w64-mingw32-gcc for the 32-bit one).  Usage: gltest [seconds [DRIVER]]
 *        (DRIVER, virgl or llvmpipe: run with GALLIUM_DRIVER=DRIVER, and
 *        GL_RENDERER must name it)
 *
 * gltest fps [seconds]: the frame-rate test.  A scene that keeps the
 * rasterizer busy (64 blended full-window quads at 640x480) runs once on
 * virgl (the host's GPU through the virtio-gpu) and once on llvmpipe
 * (NovaOS's CPU), each in a child process whose GALLIUM_DRIVER names the
 * driver; virgl must draw more frames per second. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static int pass, fail;
static void check(const char *what, int ok) { if (ok) pass++; else { fail++; printf("FAIL %s\n", what); } }

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static int px_near(const unsigned char *p, int r, int g, int b)
{
    return abs(p[0] - r) < 8 && abs(p[1] - g) < 8 && abs(p[2] - b) < 8;
}

#define GLF(t, n) static t n
GLF(PFNGLCREATESHADERPROC, pCreateShader); GLF(PFNGLSHADERSOURCEPROC, pShaderSource);
GLF(PFNGLCOMPILESHADERPROC, pCompileShader); GLF(PFNGLGETSHADERIVPROC, pGetShaderiv);
GLF(PFNGLCREATEPROGRAMPROC, pCreateProgram); GLF(PFNGLATTACHSHADERPROC, pAttachShader);
GLF(PFNGLLINKPROGRAMPROC, pLinkProgram); GLF(PFNGLGETPROGRAMIVPROC, pGetProgramiv);
GLF(PFNGLUSEPROGRAMPROC, pUseProgram); GLF(PFNGLGETUNIFORMLOCATIONPROC, pGetUniformLocation);
GLF(PFNGLUNIFORM1FPROC, pUniform1f); GLF(PFNGLGETSHADERINFOLOGPROC, pGetShaderInfoLog);

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = pCreateShader(kind);
    pShaderSource(s, 1, &src, NULL);
    pCompileShader(s);
    GLint ok = 0;
    pGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; pGetShaderInfoLog(s, sizeof(log), NULL, log); printf("shader: %s\n", log); }
    return s;
}

/* ---- the frame-rate test ------------------------------------------------ */
#define FPS_W 640
#define FPS_H 480
#define FPS_QUADS 64

/* A window with an OpenGL context current on it, @w x @h inside; NULL if not */
static HWND gl_window(const char *title, int x, int y, int w, int h, HDC *dc, HGLRC *rc)
{
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND win = CreateWindowA("gltest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, r.right - r.left,
                             r.bottom - r.top, 0, 0, GetModuleHandleA(NULL), 0);
    *dc = GetDC(win);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    int fmt = ChoosePixelFormat(*dc, &pfd);
    if (fmt <= 0 || !SetPixelFormat(*dc, fmt, &pfd)) return NULL;
    *rc = wglCreateContext(*dc);
    if (!*rc || !wglMakeCurrent(*dc, *rc)) return NULL;
    return win;
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
}

/* The child: draw the scene for @secs seconds; prints "fps-result RENDERER|FRAMES|MS" */
static int fps_child(int secs)
{
    HDC dc;
    HGLRC rc;
    if (!gl_window("OpenGL frame rate", 60, 60, FPS_W, FPS_H, &dc, &rc)) { printf("FAIL context\n"); return 1; }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    glViewport(0, 0, FPS_W, FPS_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, FPS_W, FPS_H, 0, -1, 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    DWORD t0 = GetTickCount(), frames = 0;
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        pump();
        glClearColor(0.06f, 0.06f, ((frames * 4) & 0xFF) / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        for (int i = 0; i < FPS_QUADS; i++) {
            float x0 = (float)(i % 8), y0 = (float)(i / 8 % 8);
            glColor4ub((GLubyte)(40 + i * 3), (GLubyte)(255 - i * 3), (GLubyte)(128 + (i & 7) * 16), 0x20);
            glVertex2f(x0, y0); glVertex2f(FPS_W - 8 + x0, y0);
            glVertex2f(FPS_W - 8 + x0, FPS_H - 8 + y0); glVertex2f(x0, FPS_H - 8 + y0);
        }
        glEnd();
        if (!SwapBuffers(dc)) { printf("FAIL SwapBuffers\n"); return 1; }
        frames++;
    }
    glFinish();
    DWORD ms = GetTickCount() - t0;
    printf("fps-result %s|%lu|%lu\n", renderer ? renderer : "?", (unsigned long)frames, (unsigned long)ms);
    return 0;
}

/* Run the child with GALLIUM_DRIVER=@driver; its frames per second (or
 * -1), the renderer it reported in @renderer */
static double fps_run(const char *driver, int secs, char *renderer, int cap)
{
    char exe[MAX_PATH], cmd[2 * MAX_PATH], out[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    GetTempPathA(sizeof(out), out);
    lstrcatA(out, "gltest-fps.txt");
    DeleteFileA(out);
    snprintf(cmd, sizeof(cmd), "\"%s\" fpsrun %d \"%s\"", exe, secs, out);
    SetEnvironmentVariableA("GALLIUM_DRIVER", driver);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, (DWORD)(secs + 600) * 1000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SetEnvironmentVariableA("GALLIUM_DRIVER", NULL);
    char line[512] = "";
    FILE *f = fopen(out, "r");
    if (!f) return -1;
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    char *bar = strchr(line, '|');
    if (strncmp(line, "fps-result ", 11) || !bar) return -1;
    *bar = 0;
    snprintf(renderer, cap, "%s", line + 11);
    unsigned long frames = 0, ms = 0;
    if (sscanf(bar + 1, "%lu|%lu", &frames, &ms) != 2 || !ms) return -1;
    return frames * 1000.0 / ms;
}

static int fps_test(int secs)
{
    char r_virgl[256] = "", r_llvm[256] = "";
    printf("fps: virgl for %d s\n", secs);
    double v = fps_run("virgl", secs, r_virgl, sizeof(r_virgl));
    printf("virgl     %.2f frames/s  (%s)\n", v, r_virgl);
    printf("fps: llvmpipe for %d s\n", secs);
    double l = fps_run("llvmpipe", secs, r_llvm, sizeof(r_llvm));
    printf("llvmpipe  %.2f frames/s  (%s)\n", l, r_llvm);
    check("virgl ran", v > 0);
    check("virgl is the renderer", strstr(r_virgl, "virgl") != NULL);
    check("llvmpipe ran", l > 0);
    check("llvmpipe is the renderer", strstr(r_llvm, "llvmpipe") != NULL && !strstr(r_llvm, "virgl"));
    check("virgl beats llvmpipe", v > 0 && l > 0 && v > l);
    if (v > 0 && l > 0) printf("virgl is %.1fx llvmpipe\n", v / l);
    printf("gltest fps: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && (!strcmp(argv[1], "fpsrun") || !strcmp(argv[1], "fps"))) {
        WNDCLASSA wc = { CS_OWNDC, proc, 0, 0, GetModuleHandleA(NULL), 0, LoadCursor(NULL, IDC_ARROW), 0, 0, "gltest" };
        RegisterClassA(&wc);
        if (!strcmp(argv[1], "fps")) return fps_test(argc > 2 ? atoi(argv[2]) : 10);
        if (argc > 3) freopen(argv[3], "w", stdout);   /* the child: fpsrun SECS FILE */
        return fps_child(argc > 2 ? atoi(argv[2]) : 5);
    }
    int seconds = argc > 1 ? atoi(argv[1]) : 3;
    const char *want = argc > 2 ? argv[2] : NULL;
    char drv[32];
    if (want && (!GetEnvironmentVariableA("GALLIUM_DRIVER", drv, sizeof(drv)) || strcmp(drv, want))) {
        /* opengl32 picks its driver when it loads: run again with GALLIUM_DRIVER=RENDERER */
        char exe[MAX_PATH], cmd[2 * MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        snprintf(cmd, sizeof(cmd), "\"%s\" %d %s", exe, seconds, want);
        SetEnvironmentVariableA("GALLIUM_DRIVER", want);
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("FAIL CreateProcess\n"); return 1; }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return (int)code;
    }
    WNDCLASSA wc = { CS_OWNDC, proc, 0, 0, GetModuleHandleA(NULL), 0, LoadCursor(NULL, IDC_ARROW), 0, 0, "gltest" };
    RegisterClassA(&wc);
    HWND w = CreateWindowA("gltest", "OpenGL test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 336, 279, 0, 0, wc.hInstance, 0);
    HDC dc = GetDC(w);
    RECT cr; GetClientRect(w, &cr);
    int W = cr.right, H = cr.bottom;
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    int fmt = ChoosePixelFormat(dc, &pfd);
    check("ChoosePixelFormat", fmt > 0);
    PIXELFORMATDESCRIPTOR got;
    check("DescribePixelFormat", DescribePixelFormat(dc, fmt, sizeof(got), &got) > 0 && got.cColorBits >= 24);
    check("SetPixelFormat", SetPixelFormat(dc, fmt, &pfd));
    check("GetPixelFormat", GetPixelFormat(dc) == fmt);
    HGLRC rc = wglCreateContext(dc);
    check("wglCreateContext", rc != NULL);
    check("wglMakeCurrent", wglMakeCurrent(dc, rc));
    if (!rc) { printf("gltest: %d passed, %d failed\n", pass, fail); return 1; }
    printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\nGLSL        %s\n", glGetString(GL_VENDOR),
           glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    check("version", glGetString(GL_VERSION) && atof((const char *)glGetString(GL_VERSION)) >= 3.0);
    if (want) check("renderer", glGetString(GL_RENDERER) && strstr((const char *)glGetString(GL_RENDERER), want));

    unsigned char px[4];
    glViewport(0, 0, W, H);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check("clear", px_near(px, 51, 102, 153));

    glBegin(GL_TRIANGLES);                                 /* fixed function */
    glColor3f(1, 0, 0); glVertex2f(-1, -1); glVertex2f(3, -1); glVertex2f(-1, 3);
    glEnd();
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check("triangle", px_near(px, 255, 0, 0));

#define LOAD(n, s) (n = (void *)wglGetProcAddress(s))
    LOAD(pCreateShader, "glCreateShader"); LOAD(pShaderSource, "glShaderSource"); LOAD(pCompileShader, "glCompileShader");
    LOAD(pGetShaderiv, "glGetShaderiv"); LOAD(pCreateProgram, "glCreateProgram"); LOAD(pAttachShader, "glAttachShader");
    LOAD(pLinkProgram, "glLinkProgram"); LOAD(pGetProgramiv, "glGetProgramiv"); LOAD(pUseProgram, "glUseProgram");
    LOAD(pGetUniformLocation, "glGetUniformLocation"); LOAD(pUniform1f, "glUniform1f"); LOAD(pGetShaderInfoLog, "glGetShaderInfoLog");
    check("wglGetProcAddress", pCreateShader && pUseProgram && pUniform1f);
    GLuint prog = 0;
    GLint loc = -1;
    if (pCreateShader) {
        GLuint vs = shader(GL_VERTEX_SHADER,
            "#version 120\nuniform float t; varying vec3 c;\n"
            "void main(){ float a=t; mat2 r=mat2(cos(a),sin(a),-sin(a),cos(a));\n"
            " gl_Position=vec4(r*gl_Vertex.xy,0.0,1.0); c=gl_Color.rgb; }\n");
        GLuint fs = shader(GL_FRAGMENT_SHADER,
            "#version 120\nvarying vec3 c; void main(){ gl_FragColor=vec4(c,1.0); }\n");
        prog = pCreateProgram();
        pAttachShader(prog, vs); pAttachShader(prog, fs); pLinkProgram(prog);
        GLint ok = 0; pGetProgramiv(prog, GL_LINK_STATUS, &ok);
        check("link", ok);
        pUseProgram(prog);
        loc = pGetUniformLocation(prog, "t");
        pUniform1f(loc, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(0, 1, 0); glVertex2f(-1, -1); glVertex2f(3, -1); glVertex2f(-1, 3);
        glEnd();
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        check("shader", px_near(px, 0, 255, 0));
    }
    check("SwapBuffers", SwapBuffers(dc));

    /* Animated frames */
    DWORD start = GetTickCount(), frames = 0;
    MSG m;
    int quit = 0;
    while (!quit && GetTickCount() - start < (DWORD)seconds * 1000) {
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) quit = 1; DispatchMessageA(&m); }
        float t = (GetTickCount() - start) / 1000.0f;
        glClearColor(0.1f, 0.1f, 0.15f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (prog) pUniform1f(loc, t);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0.3f, 0.2f); glVertex2f(0, 0.8f);
        glColor3f(0.2f, 1, 0.3f); glVertex2f(-0.7f, -0.5f);
        glColor3f(0.2f, 0.4f, 1); glVertex2f(0.7f, -0.5f);
        glEnd();
        SwapBuffers(dc);
        frames++;
    }
    DWORD ms = GetTickCount() - start;
    check("frames", frames > 0);
    printf("%lu frames in %lu ms (%.1f fps)\n", frames, ms, ms ? frames * 1000.0 / ms : 0.0);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(w, dc);
    DestroyWindow(w);
    printf("gltest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

