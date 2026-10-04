/*
 * generic.h — the context of NovaOS's own OpenGL 1.1 (generic.c, draw.c)
 */
#ifndef NOVA_GL_GENERIC_H
#define NOVA_GL_GENERIC_H
#include <windows.h>

#define GL_VENDOR               0x1F00
#define GL_RENDERER             0x1F01
#define GL_VERSION              0x1F02
#define GL_EXTENSIONS           0x1F03
#define GL_VIEWPORT             0x0BA2
#define GL_SCISSOR_BOX          0x0C10
#define GL_MATRIX_MODE          0x0BA0
#define GL_TEXTURE_BINDING_2D   0x8069
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_MAX_TEXTURE_SIZE     0x0D33
#define GL_MAX_VIEWPORT_DIMS    0x0D3A
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_RGBA                 0x1908
#define GL_BGRA                 0x80E1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_INVALID_ENUM         0x0500
#define GL_INVALID_VALUE        0x0501
#define GL_INVALID_OPERATION    0x0502
#define GL_STACK_OVERFLOW       0x0503
#define GL_STACK_UNDERFLOW      0x0504
#define GL_OUT_OF_MEMORY        0x0505

#define GL_TEX_MAX      4096        /* the largest texture side */
#define GL_STACK        16          /* matrices on each stack */
#define GL_TEX_NAMES    65536       /* texture names a context can have */
#define CTX_MAGIC       0x4C474E56  /* "VNGL" */

/* glEnable bits */
#define EN_TEXTURE_2D   0x01
#define EN_BLEND        0x02
#define EN_SCISSOR      0x04
#define EN_ALPHA_TEST   0x08
/* glEnableClientState bits */
#define CS_VERTEX       0x01
#define CS_COLOR        0x02
#define CS_TEXCOORD     0x04

typedef struct Tex {
    DWORD *px;                      /* BGRA, row 0 first (glTexImage2D's order) */
    int w, h;
    unsigned mag, min, wrap_s, wrap_t;
} Tex;

typedef struct Arr { int size; unsigned type; int stride; const void *ptr; } Arr;
typedef struct Vtx { float x, y, z, w, s, t, c[4]; } Vtx;

typedef struct Ctx {
    DWORD magic;
    int vp[4];                      /* glViewport */
    BYTE clear[4];                  /* glClearColor, as B, G, R, A */
    DWORD *back;                    /* the back buffer, bottom-up BGRA */
    int w, h;
    DWORD error;
    /* draw.c's */
    float m[3][GL_STACK][16];       /* modelview, projection, texture stacks */
    int depth[3], mode;
    float color[4], tc[2];          /* current colour and texture coordinate */
    unsigned enabled, client;
    unsigned bsrc, bdst, afunc, env;
    float aref;
    int scissor[4];
    int unpack_align, unpack_row, unpack_skip_rows, unpack_skip_px;
    Arr va, ca, ta;
    Tex **tex;                      /* by name - 1 */
    unsigned ntex, bound;
    Vtx *imm;                       /* glBegin's vertices */
    int nimm, capimm;
    unsigned prim;
    int in_begin;
} Ctx;

Ctx *current(void);
Ctx *current_drawing(void);
int fit(Ctx *x, HDC dc);
void gl_init_state(Ctx *x);
void gl_free_state(Ctx *x);
void clip_scissor(Ctx *x, int *x0, int *y0, int *x1, int *y1);

#endif
