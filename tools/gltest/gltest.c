/* gltest — OpenGL through opengl32.dll (Mesa llvmpipe on NovaOS):
 * pixel format, context, clear, immediate mode, GLSL shaders, read-back,
 * then a few seconds of animated frames presented with SwapBuffers.
 * Build: x86_64-w64-mingw32-gcc -O2 -o gltest.exe gltest.c -lopengl32 -lgdi32 -luser32 -lm
 *        (i686-w64-mingw32-gcc for the 32-bit one).  Usage: gltest [seconds] */
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

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 3;
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

