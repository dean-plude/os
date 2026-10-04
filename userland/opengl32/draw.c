/*
 * draw.c — what NovaOS's own OpenGL 1.1 (generic.c) draws: 2D, as
 * programs that use OpenGL 1.1 to put pictures on the screen need
 * (ScummVM, SDL's OpenGL renderer).
 *
 * Textures (RGBA, RGB, BGRA, BGR, luminance and alpha, unsigned bytes;
 * nearest or linear filtering, clamped or repeated), the modelview,
 * projection and texture matrix stacks, vertex, colour and texture
 * coordinate arrays, glDrawArrays/glDrawElements and glBegin/glEnd fill
 * triangles, triangle strips and fans, quads and polygons into the back
 * buffer.  Each fragment's colour is the vertex colour, modulated by (or
 * replaced with) the texture's, then alpha-tested, scissored and blended.
 * Texture coordinates are interpolated in window space, so perspective is
 * not corrected; there is no depth buffer, lighting, fog, stencil, lines
 * or points.
 */
#include "generic.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define GL_TEXTURE_2D            0x0DE1
#define GL_BLEND                 0x0BE2
#define GL_SCISSOR_TEST          0x0C11
#define GL_ALPHA_TEST            0x0BC0
#define GL_VERTEX_ARRAY          0x8074
#define GL_COLOR_ARRAY           0x8076
#define GL_TEXTURE_COORD_ARRAY   0x8078
#define GL_BYTE                  0x1400
#define GL_SHORT                 0x1402
#define GL_UNSIGNED_SHORT        0x1403
#define GL_INT                   0x1404
#define GL_UNSIGNED_INT          0x1405
#define GL_FLOAT                 0x1406
#define GL_DOUBLE                0x140A
#define GL_MODELVIEW             0x1700
#define GL_TEXTURE               0x1702
#define GL_TRIANGLES             4
#define GL_TRIANGLE_STRIP        5
#define GL_TRIANGLE_FAN          6
#define GL_QUADS                 7
#define GL_QUAD_STRIP            8
#define GL_POLYGON               9
#define GL_SRC_COLOR             0x0300
#define GL_ONE_MINUS_SRC_COLOR   0x0301
#define GL_SRC_ALPHA             0x0302
#define GL_ONE_MINUS_SRC_ALPHA   0x0303
#define GL_DST_ALPHA             0x0304
#define GL_ONE_MINUS_DST_ALPHA   0x0305
#define GL_DST_COLOR             0x0306
#define GL_ONE_MINUS_DST_COLOR   0x0307
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_WRAP_S        0x2802
#define GL_TEXTURE_WRAP_T        0x2803
#define GL_NEAREST               0x2600
#define GL_LINEAR                0x2601
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR  0x2703
#define GL_REPEAT                0x2901
#define GL_ALPHA                 0x1906
#define GL_RGB                   0x1907
#define GL_LUMINANCE             0x1909
#define GL_LUMINANCE_ALPHA       0x190A
#define GL_BGR                   0x80E0
#define GL_UNPACK_ROW_LENGTH     0x0CF2
#define GL_UNPACK_SKIP_ROWS      0x0CF3
#define GL_UNPACK_SKIP_PIXELS    0x0CF4
#define GL_TEXTURE_ENV           0x2300
#define GL_TEXTURE_ENV_MODE      0x2200
#define GL_MODULATE              0x2100
#define GL_DECAL                 0x2101
#define GL_REPLACE               0x1E01
#define GL_NEVER                 0x0200
#define GL_LESS                  0x0201
#define GL_EQUAL                 0x0202
#define GL_LEQUAL                0x0203
#define GL_GREATER               0x0204
#define GL_NOTEQUAL              0x0205
#define GL_GEQUAL                0x0206
#define GL_ALWAYS                0x0207

static void err(Ctx *x, DWORD e) { if (!x->error) x->error = e; }

static void ident(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

void gl_init_state(Ctx *x)
{
    for (int i = 0; i < 3; i++) ident(x->m[i][0]);
    x->color[0] = x->color[1] = x->color[2] = x->color[3] = 1;
    x->bsrc = 1; x->bdst = 0;                 /* GL_ONE, GL_ZERO */
    x->afunc = GL_ALWAYS;
    x->env = GL_MODULATE;
    x->unpack_align = 4;
}

void gl_free_state(Ctx *x)
{
    for (unsigned i = 0; i < x->ntex; i++)
        if (x->tex[i]) {
            if (x->tex[i]->px) HeapFree(GetProcessHeap(), 0, x->tex[i]->px);
            HeapFree(GetProcessHeap(), 0, x->tex[i]);
        }
    if (x->tex) HeapFree(GetProcessHeap(), 0, x->tex);
    if (x->imm) HeapFree(GetProcessHeap(), 0, x->imm);
}

void clip_scissor(Ctx *x, int *x0, int *y0, int *x1, int *y1)
{
    if (x->scissor[0] > *x0) *x0 = x->scissor[0];
    if (x->scissor[1] > *y0) *y0 = x->scissor[1];
    if (x->scissor[0] + x->scissor[2] < *x1) *x1 = x->scissor[0] + x->scissor[2];
    if (x->scissor[1] + x->scissor[3] < *y1) *y1 = x->scissor[1] + x->scissor[3];
}

/* ---- state ---- */
static unsigned en_bit(unsigned cap)
{
    switch (cap) {
    case GL_TEXTURE_2D: return EN_TEXTURE_2D;
    case GL_BLEND: return EN_BLEND;
    case GL_SCISSOR_TEST: return EN_SCISSOR;
    case GL_ALPHA_TEST: return EN_ALPHA_TEST;
    }
    return 0;
}
static void WINAPI g_glEnable(unsigned cap) { Ctx *x = current(); if (x) x->enabled |= en_bit(cap); }
static void WINAPI g_glDisable(unsigned cap) { Ctx *x = current(); if (x) x->enabled &= ~en_bit(cap); }
static BYTE WINAPI g_glIsEnabled(unsigned cap) { Ctx *x = current(); return x && (x->enabled & en_bit(cap)) ? 1 : 0; }

static unsigned cs_bit(unsigned a)
{
    return a == GL_VERTEX_ARRAY ? CS_VERTEX : a == GL_COLOR_ARRAY ? CS_COLOR : a == GL_TEXTURE_COORD_ARRAY ? CS_TEXCOORD : 0;
}
static void WINAPI g_glEnableClientState(unsigned a) { Ctx *x = current(); if (x) x->client |= cs_bit(a); }
static void WINAPI g_glDisableClientState(unsigned a) { Ctx *x = current(); if (x) x->client &= ~cs_bit(a); }

static void set_arr(Arr *a, int size, unsigned type, int stride, const void *p) { a->size = size; a->type = type; a->stride = stride; a->ptr = p; }
static void WINAPI g_glVertexPointer(int size, unsigned type, int stride, const void *p) { Ctx *x = current(); if (x) set_arr(&x->va, size, type, stride, p); }
static void WINAPI g_glColorPointer(int size, unsigned type, int stride, const void *p) { Ctx *x = current(); if (x) set_arr(&x->ca, size, type, stride, p); }
static void WINAPI g_glTexCoordPointer(int size, unsigned type, int stride, const void *p) { Ctx *x = current(); if (x) set_arr(&x->ta, size, type, stride, p); }

static void WINAPI g_glBlendFunc(unsigned s, unsigned d) { Ctx *x = current(); if (x) { x->bsrc = s; x->bdst = d; } }
static void WINAPI g_glAlphaFunc(unsigned f, float ref) { Ctx *x = current(); if (x) { x->afunc = f; x->aref = ref; } }
static void WINAPI g_glScissor(int x0, int y0, int w, int h)
{
    Ctx *x = current();
    if (!x) return;
    if (w < 0 || h < 0) { err(x, GL_INVALID_VALUE); return; }
    x->scissor[0] = x0; x->scissor[1] = y0; x->scissor[2] = w; x->scissor[3] = h;
}
static void WINAPI g_glPixelStorei(unsigned p, int v)
{
    Ctx *x = current();
    if (!x) return;
    switch (p) {
    case GL_UNPACK_ALIGNMENT: if (v == 1 || v == 2 || v == 4 || v == 8) x->unpack_align = v; else err(x, GL_INVALID_VALUE); break;
    case GL_UNPACK_ROW_LENGTH: x->unpack_row = v; break;
    case GL_UNPACK_SKIP_ROWS: x->unpack_skip_rows = v; break;
    case GL_UNPACK_SKIP_PIXELS: x->unpack_skip_px = v; break;
    }
}
static void WINAPI g_glTexEnvi(unsigned target, unsigned p, int v)
{
    Ctx *x = current();
    if (x && target == GL_TEXTURE_ENV && p == GL_TEXTURE_ENV_MODE) x->env = (unsigned)v;
}
static void WINAPI g_glTexEnvf(unsigned target, unsigned p, float v) { g_glTexEnvi(target, p, (int)v); }

/* ---- colour and immediate mode ---- */
static void WINAPI g_glColor4f(float r, float g, float b, float a)
{
    Ctx *x = current();
    if (x) { x->color[0] = r; x->color[1] = g; x->color[2] = b; x->color[3] = a; }
}
static void WINAPI g_glColor3f(float r, float g, float b) { g_glColor4f(r, g, b, 1); }
static void WINAPI g_glColor4fv(const float *v) { if (v) g_glColor4f(v[0], v[1], v[2], v[3]); }
static void WINAPI g_glColor4ub(BYTE r, BYTE g, BYTE b, BYTE a) { g_glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
static void WINAPI g_glColor3ub(BYTE r, BYTE g, BYTE b) { g_glColor4ub(r, g, b, 255); }
static void WINAPI g_glTexCoord2f(float s, float t) { Ctx *x = current(); if (x) { x->tc[0] = s; x->tc[1] = t; } }
static void WINAPI g_glTexCoord2fv(const float *v) { if (v) g_glTexCoord2f(v[0], v[1]); }

static void WINAPI g_glBegin(unsigned prim)
{
    Ctx *x = current();
    if (!x) return;
    if (x->in_begin) { err(x, GL_INVALID_OPERATION); return; }
    x->in_begin = 1; x->prim = prim; x->nimm = 0;
}
static void WINAPI g_glVertex4f(float vx, float vy, float vz, float vw)
{
    Ctx *x = current();
    if (!x || !x->in_begin) return;
    if (x->nimm == x->capimm) {
        int cap = x->capimm ? x->capimm * 2 : 64;
        Vtx *n = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)cap * sizeof(Vtx));
        if (!n) { err(x, GL_OUT_OF_MEMORY); return; }
        if (x->imm) { memcpy(n, x->imm, (SIZE_T)x->nimm * sizeof(Vtx)); HeapFree(GetProcessHeap(), 0, x->imm); }
        x->imm = n; x->capimm = cap;
    }
    Vtx *v = &x->imm[x->nimm++];
    v->x = vx; v->y = vy; v->z = vz; v->w = vw;
    v->s = x->tc[0]; v->t = x->tc[1];
    for (int i = 0; i < 4; i++) v->c[i] = x->color[i];
}
static void WINAPI g_glVertex2f(float vx, float vy) { g_glVertex4f(vx, vy, 0, 1); }
static void WINAPI g_glVertex2fv(const float *v) { if (v) g_glVertex4f(v[0], v[1], 0, 1); }
static void WINAPI g_glVertex2i(int vx, int vy) { g_glVertex4f((float)vx, (float)vy, 0, 1); }
static void WINAPI g_glVertex3f(float vx, float vy, float vz) { g_glVertex4f(vx, vy, vz, 1); }
static void WINAPI g_glVertex3fv(const float *v) { if (v) g_glVertex4f(v[0], v[1], v[2], 1); }

/* ---- matrices ---- */
static float *top(Ctx *x) { return x->m[x->mode][x->depth[x->mode]]; }

static void mul(float *r, const float *a, const float *b)       /* r = a * b, column-major */
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            t[c * 4 + rw] = a[rw] * b[c * 4] + a[4 + rw] * b[c * 4 + 1] + a[8 + rw] * b[c * 4 + 2] + a[12 + rw] * b[c * 4 + 3];
    memcpy(r, t, sizeof(t));
}
static void mult_top(Ctx *x, const float *m) { float *t = top(x); mul(t, t, m); }

static void WINAPI g_glMatrixMode(unsigned m)
{
    Ctx *x = current();
    if (!x) return;
    if (m < GL_MODELVIEW || m > GL_TEXTURE) { err(x, GL_INVALID_ENUM); return; }
    x->mode = (int)(m - GL_MODELVIEW);
}
static void WINAPI g_glLoadIdentity(void) { Ctx *x = current(); if (x) ident(top(x)); }
static void WINAPI g_glLoadMatrixf(const float *m) { Ctx *x = current(); if (x && m) memcpy(top(x), m, 16 * sizeof(float)); }
static void WINAPI g_glLoadMatrixd(const double *m) { Ctx *x = current(); if (x && m) for (int i = 0; i < 16; i++) top(x)[i] = (float)m[i]; }
static void WINAPI g_glMultMatrixf(const float *m) { Ctx *x = current(); if (x && m) mult_top(x, m); }
static void WINAPI g_glPushMatrix(void)
{
    Ctx *x = current();
    if (!x) return;
    if (x->depth[x->mode] + 1 >= GL_STACK) { err(x, GL_STACK_OVERFLOW); return; }
    memcpy(x->m[x->mode][x->depth[x->mode] + 1], top(x), 16 * sizeof(float));
    x->depth[x->mode]++;
}
static void WINAPI g_glPopMatrix(void)
{
    Ctx *x = current();
    if (!x) return;
    if (!x->depth[x->mode]) { err(x, GL_STACK_UNDERFLOW); return; }
    x->depth[x->mode]--;
}
static void WINAPI g_glOrtho(double l, double r, double b, double t, double n, double f)
{
    Ctx *x = current();
    if (!x) return;
    if (l == r || b == t || n == f) { err(x, GL_INVALID_VALUE); return; }
    float m[16];
    ident(m);
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    mult_top(x, m);
}
static void WINAPI g_glFrustum(double l, double r, double b, double t, double n, double f)
{
    Ctx *x = current();
    if (!x) return;
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) { err(x, GL_INVALID_VALUE); return; }
    float m[16];
    memset(m, 0, sizeof(m));
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    mult_top(x, m);
}
static void WINAPI g_glTranslatef(float tx, float ty, float tz)
{
    Ctx *x = current();
    if (!x) return;
    float m[16];
    ident(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
    mult_top(x, m);
}
static void WINAPI g_glScalef(float sx, float sy, float sz)
{
    Ctx *x = current();
    if (!x) return;
    float m[16];
    ident(m);
    m[0] = sx; m[5] = sy; m[10] = sz;
    mult_top(x, m);
}

/* ---- textures ---- */
static Tex *bound_tex(Ctx *x) { return x->bound && x->bound <= x->ntex ? x->tex[x->bound - 1] : 0; }

static void WINAPI g_glDeleteTextures(int n, const unsigned *names)
{
    Ctx *x = current();
    if (!x || !names) return;
    for (int i = 0; i < n; i++) {
        unsigned k = names[i];
        if (!k || k > x->ntex || !x->tex[k - 1]) continue;
        if (x->tex[k - 1]->px) HeapFree(GetProcessHeap(), 0, x->tex[k - 1]->px);
        HeapFree(GetProcessHeap(), 0, x->tex[k - 1]);
        x->tex[k - 1] = 0;
        if (x->bound == k) x->bound = 0;
    }
}
static int grow(Ctx *x, unsigned need)
{
    if (need <= x->ntex) return 1;
    unsigned cap = x->ntex ? x->ntex : 16;
    while (cap < need) cap *= 2;
    Tex **t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(Tex *));
    if (!t) { err(x, GL_OUT_OF_MEMORY); return 0; }
    if (x->tex) { memcpy(t, x->tex, x->ntex * sizeof(Tex *)); HeapFree(GetProcessHeap(), 0, x->tex); }
    x->tex = t; x->ntex = cap;
    return 1;
}

static Tex *new_tex(Ctx *x, unsigned k)
{
    Tex *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Tex));
    if (!t) { err(x, GL_OUT_OF_MEMORY); return 0; }
    t->mag = GL_LINEAR; t->min = GL_NEAREST_MIPMAP_LINEAR; t->wrap_s = t->wrap_t = GL_REPEAT;
    x->tex[k] = t;
    return t;
}

static void WINAPI g_glBindTexture(unsigned target, unsigned name)
{
    Ctx *x = current();
    if (!x) return;
    if (target != GL_TEXTURE_2D) { err(x, GL_INVALID_ENUM); return; }
    if (name && (name > GL_TEX_NAMES || !grow(x, name) || (!x->tex[name - 1] && !new_tex(x, name - 1)))) {
        if (name > GL_TEX_NAMES) err(x, GL_OUT_OF_MEMORY);
        return;                                 /* a name nobody generated is made on its first bind */
    }
    x->bound = name;
}

static void WINAPI g_glGenTextures(int n, unsigned *names)
{
    Ctx *x = current();
    if (!x || !names) return;
    if (n < 0) { err(x, GL_INVALID_VALUE); return; }
    for (int i = 0; i < n; i++) {
        unsigned k = 0;
        while (k < x->ntex && x->tex[k]) k++;
        if (!grow(x, k + 1) || !new_tex(x, k)) return;
        names[i] = k + 1;
    }
}
static void WINAPI g_glTexParameteri(unsigned target, unsigned p, int v)
{
    Ctx *x = current();
    Tex *t = x && target == GL_TEXTURE_2D ? bound_tex(x) : 0;
    if (!t) return;
    switch (p) {
    case GL_TEXTURE_MAG_FILTER: t->mag = (unsigned)v; break;
    case GL_TEXTURE_MIN_FILTER: t->min = (unsigned)v; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = (unsigned)v; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = (unsigned)v; break;
    }
}
static void WINAPI g_glTexParameterf(unsigned target, unsigned p, float v) { g_glTexParameteri(target, p, (int)v); }

/* Bytes per pixel of an upload's format, or 0 */
static int fmt_bytes(unsigned format)
{
    switch (format) {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_LUMINANCE: case GL_ALPHA: return 1;
    }
    return 0;
}

static DWORD to_bgra(unsigned format, const BYTE *p)
{
    switch (format) {
    case GL_RGBA: return (DWORD)p[2] | (DWORD)p[1] << 8 | (DWORD)p[0] << 16 | (DWORD)p[3] << 24;
    case GL_BGRA: return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24;
    case GL_RGB: return (DWORD)p[2] | (DWORD)p[1] << 8 | (DWORD)p[0] << 16 | 0xFF000000u;
    case GL_BGR: return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | 0xFF000000u;
    case GL_LUMINANCE_ALPHA: return (DWORD)p[0] * 0x010101u | (DWORD)p[1] << 24;
    case GL_LUMINANCE: return (DWORD)p[0] * 0x010101u | 0xFF000000u;
    case GL_ALPHA: return (DWORD)p[0] << 24;
    }
    return 0;
}

static void upload(Ctx *x, Tex *t, int x0, int y0, int w, int h, unsigned format, const BYTE *src)
{
    int bpp = fmt_bytes(format);
    int rowpx = x->unpack_row > 0 ? x->unpack_row : w;
    int pitch = (rowpx * bpp + x->unpack_align - 1) / x->unpack_align * x->unpack_align;
    src += (SIZE_T)x->unpack_skip_rows * pitch + (SIZE_T)x->unpack_skip_px * bpp;
    for (int r = 0; r < h; r++) {
        const BYTE *p = src + (SIZE_T)r * pitch;
        DWORD *d = t->px + (SIZE_T)(y0 + r) * t->w + x0;
        for (int k = 0; k < w; k++, p += bpp) d[k] = to_bgra(format, p);
    }
}

static void WINAPI g_glTexImage2D(unsigned target, int level, int ifmt, int w, int h, int border,
                                  unsigned format, unsigned type, const void *pixels)
{
    Ctx *x = current();
    if (!x) return;
    (void)ifmt;
    if (target != GL_TEXTURE_2D) { err(x, GL_INVALID_ENUM); return; }
    if (level) return;                                   /* smaller mipmaps: only level 0 is sampled */
    if (w < 0 || h < 0 || w > GL_TEX_MAX || h > GL_TEX_MAX || border) { err(x, GL_INVALID_VALUE); return; }
    if (!fmt_bytes(format) || type != GL_UNSIGNED_BYTE) { err(x, GL_INVALID_ENUM); return; }
    Tex *t = bound_tex(x);
    if (!t) return;                                      /* texture 0: not kept */
    DWORD *px = w && h ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)w * h * 4) : 0;
    if (w && h && !px) { err(x, GL_OUT_OF_MEMORY); return; }
    if (t->px) HeapFree(GetProcessHeap(), 0, t->px);
    t->px = px; t->w = w; t->h = h;
    if (pixels && px) upload(x, t, 0, 0, w, h, format, pixels);
}

static void WINAPI g_glTexSubImage2D(unsigned target, int level, int x0, int y0, int w, int h,
                                     unsigned format, unsigned type, const void *pixels)
{
    Ctx *x = current();
    if (!x) return;
    if (target != GL_TEXTURE_2D) { err(x, GL_INVALID_ENUM); return; }
    if (level) return;
    Tex *t = bound_tex(x);
    if (!t || !t->px) { err(x, GL_INVALID_OPERATION); return; }
    if (x0 < 0 || y0 < 0 || w < 0 || h < 0 || x0 + w > t->w || y0 + h > t->h) { err(x, GL_INVALID_VALUE); return; }
    if (!fmt_bytes(format) || type != GL_UNSIGNED_BYTE) { err(x, GL_INVALID_ENUM); return; }
    if (pixels) upload(x, t, x0, y0, w, h, format, pixels);
}

/* ---- drawing ---- */
static float fetch(const Arr *a, int i, int c, float def)
{
    if (c >= a->size) return def;
    int sz;
    switch (a->type) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: sz = 4; break;
    case GL_DOUBLE: sz = 8; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: sz = 2; break;
    default: sz = 1; break;
    }
    const BYTE *p = (const BYTE *)a->ptr + (SIZE_T)i * (a->stride ? a->stride : a->size * sz) + (SIZE_T)c * sz;
    switch (a->type) {
    case GL_FLOAT: return *(const float *)p;
    case GL_DOUBLE: return (float)*(const double *)p;
    case GL_INT: return (float)*(const int *)p;
    case GL_UNSIGNED_INT: return (float)*(const unsigned *)p;
    case GL_SHORT: return (float)*(const short *)p;
    case GL_UNSIGNED_SHORT: return (float)*(const unsigned short *)p;
    case GL_UNSIGNED_BYTE: return *p / 255.0f;            /* only colours come as bytes */
    case GL_BYTE: return (float)*(const signed char *)p;
    }
    return def;
}

static void from_arrays(Ctx *x, int i, Vtx *v)
{
    v->x = fetch(&x->va, i, 0, 0); v->y = fetch(&x->va, i, 1, 0);
    v->z = fetch(&x->va, i, 2, 0); v->w = fetch(&x->va, i, 3, 1);
    if ((x->client & CS_TEXCOORD) && x->ta.ptr) { v->s = fetch(&x->ta, i, 0, 0); v->t = fetch(&x->ta, i, 1, 0); }
    else { v->s = x->tc[0]; v->t = x->tc[1]; }
    if ((x->client & CS_COLOR) && x->ca.ptr)
        for (int k = 0; k < 4; k++) v->c[k] = fetch(&x->ca, i, k, 1);
    else
        for (int k = 0; k < 4; k++) v->c[k] = x->color[k];
}

/* A vertex in window coordinates (and its texture coordinate through the
 * texture matrix); 0 when it lies behind the eye */
static int to_window(Ctx *x, const Vtx *in, Vtx *out)
{
    float mvp[16], p[4];
    mul(mvp, x->m[1][x->depth[1]], x->m[0][x->depth[0]]);
    for (int r = 0; r < 4; r++) p[r] = mvp[r] * in->x + mvp[4 + r] * in->y + mvp[8 + r] * in->z + mvp[12 + r] * in->w;
    if (p[3] <= 0) return 0;
    *out = *in;
    out->x = x->vp[0] + (p[0] / p[3] + 1) * 0.5f * x->vp[2];
    out->y = x->vp[1] + (p[1] / p[3] + 1) * 0.5f * x->vp[3];
    const float *tm = x->m[2][x->depth[2]];
    out->s = tm[0] * in->s + tm[4] * in->t + tm[12];
    out->t = tm[1] * in->s + tm[5] * in->t + tm[13];
    return 1;
}

static int wrap(int i, int n, unsigned mode)
{
    if (mode == GL_REPEAT) { i %= n; return i < 0 ? i + n : i; }
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

static DWORD texel(const Tex *t, float s, float tt, int linear)
{
    float fx = s * t->w - 0.5f, fy = tt * t->h - 0.5f;
    if (!linear) {
        int ix = (int)(fx + 0.5f + 65536.0f) - 65536, iy = (int)(fy + 0.5f + 65536.0f) - 65536;
        return t->px[wrap(iy, t->h, t->wrap_t) * t->w + wrap(ix, t->w, t->wrap_s)];
    }
    int ix = (int)(fx + 65536.0f) - 65536, iy = (int)(fy + 65536.0f) - 65536;
    int ax = (int)((fx - ix) * 256), ay = (int)((fy - iy) * 256);
    int x0 = wrap(ix, t->w, t->wrap_s), x1 = wrap(ix + 1, t->w, t->wrap_s);
    int y0 = wrap(iy, t->h, t->wrap_t), y1 = wrap(iy + 1, t->h, t->wrap_t);
    DWORD a = t->px[y0 * t->w + x0], b = t->px[y0 * t->w + x1], c = t->px[y1 * t->w + x0], d = t->px[y1 * t->w + x1];
    DWORD out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int top = (int)((a >> sh) & 255) * (256 - ax) + (int)((b >> sh) & 255) * ax;
        int bot = (int)((c >> sh) & 255) * (256 - ax) + (int)((d >> sh) & 255) * ax;
        out |= (DWORD)(((top * (256 - ay) + bot * ay) >> 16) & 255) << sh;
    }
    return out;
}

static int factor(unsigned f, int ch, const int *src, const int *dst)    /* 0..255 */
{
    switch (f) {
    case 0: return 0;
    case 1: return 255;
    case GL_SRC_COLOR: return src[ch];
    case GL_ONE_MINUS_SRC_COLOR: return 255 - src[ch];
    case GL_SRC_ALPHA: return src[3];
    case GL_ONE_MINUS_SRC_ALPHA: return 255 - src[3];
    case GL_DST_ALPHA: return dst[3];
    case GL_ONE_MINUS_DST_ALPHA: return 255 - dst[3];
    case GL_DST_COLOR: return dst[ch];
    case GL_ONE_MINUS_DST_COLOR: return 255 - dst[ch];
    }
    return 255;
}

static int alpha_pass(Ctx *x, int a)
{
    int ref = (int)(x->aref * 255 + 0.5f);
    switch (x->afunc) {
    case GL_NEVER: return 0;
    case GL_LESS: return a < ref;
    case GL_EQUAL: return a == ref;
    case GL_LEQUAL: return a <= ref;
    case GL_GREATER: return a > ref;
    case GL_NOTEQUAL: return a != ref;
    case GL_GEQUAL: return a >= ref;
    }
    return 1;
}

static int clamp255(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (int)(f * 255 + 0.5f); }

/* An edge function: twice the signed area of (p, q, point), as A*x + B*y + C;
 * @in_on: whether a pixel centre exactly on the edge is inside (each edge two
 * triangles share is walked both ways, so exactly one of them takes it) */
typedef struct { float a, b, c; int in_on; } Edge;
static Edge edge(const Vtx *p, const Vtx *q)
{
    Edge e;
    float dx = q->x - p->x, dy = q->y - p->y;
    e.a = -dy; e.b = dx; e.c = dy * p->x - dx * p->y;
    e.in_on = dy > 0 || (dy == 0 && dx < 0);
    return e;
}

/* Fill one triangle (window coordinates): the pixels whose centres lie inside */
static void triangle(Ctx *x, const Vtx *a, const Vtx *b, const Vtx *c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    if (area < 0) { const Vtx *t = b; b = c; c = t; area = -area; }
    float minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    const Vtx *vs[3] = { a, b, c };
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    int x0 = minx < 0 ? 0 : (int)minx, y0 = miny < 0 ? 0 : (int)miny;
    int x1 = maxx + 1 > x->w ? x->w : (int)maxx + 1, y1 = maxy + 1 > x->h ? x->h : (int)maxy + 1;
    if (x->enabled & EN_SCISSOR) clip_scissor(x, &x0, &y0, &x1, &y1);
    if (x0 >= x1 || y0 >= y1) return;

    Tex *t = (x->enabled & EN_TEXTURE_2D) ? bound_tex(x) : 0;
    if (t && (!t->px || !t->w || !t->h)) t = 0;
    Edge ea = edge(b, c), eb = edge(c, a), ec = edge(a, b);     /* each zero on its edge, area at its vertex */
    float inv = 1 / area;
    int same = 1;                                /* one colour over the whole triangle */
    for (int k = 0; k < 4; k++) if (a->c[k] != b->c[k] || a->c[k] != c->c[k]) same = 0;
    int linear = t && t->mag == GL_LINEAR;
    int blend = (x->enabled & EN_BLEND) && !(x->bsrc == 1 && x->bdst == 0);

    for (int py = y0; py < y1; py++) {
        float cy = py + 0.5f;
        DWORD *row = x->back + (SIZE_T)py * x->w;
        for (int px = x0; px < x1; px++) {
            float cx = px + 0.5f;
            float fa = ea.a * cx + ea.b * cy + ea.c, fb = eb.a * cx + eb.b * cy + eb.c, fc = ec.a * cx + ec.b * cy + ec.c;
            if (fa < 0 || fb < 0 || fc < 0) continue;
            if ((fa == 0 && !ea.in_on) || (fb == 0 && !eb.in_on) || (fc == 0 && !ec.in_on)) continue;
            float wa = fa * inv, wb = fb * inv, wc = fc * inv;
            int src[4];
            for (int k = 0; k < 4; k++)
                src[k] = clamp255(same ? a->c[k] : wa * a->c[k] + wb * b->c[k] + wc * c->c[k]);
            if (t) {
                float ts = wa * a->s + wb * b->s + wc * c->s, tt = wa * a->t + wb * b->t + wc * c->t;
                DWORD tx = texel(t, ts, tt, linear);
                int tc[4] = { (int)(tx >> 16) & 255, (int)(tx >> 8) & 255, (int)tx & 255, (int)(tx >> 24) };
                if (x->env == GL_REPLACE) for (int k = 0; k < 4; k++) src[k] = tc[k];
                else if (x->env == GL_DECAL) for (int k = 0; k < 3; k++) src[k] = (src[k] * (255 - tc[3]) + tc[k] * tc[3]) / 255;
                else for (int k = 0; k < 4; k++) src[k] = src[k] * tc[k] / 255;
            }
            if ((x->enabled & EN_ALPHA_TEST) && !alpha_pass(x, src[3])) continue;
            DWORD *d = row + px;
            if (blend) {
                int dst[4] = { (int)(*d >> 16) & 255, (int)(*d >> 8) & 255, (int)*d & 255, (int)(*d >> 24) };
                int out[4];
                for (int k = 0; k < 4; k++) {
                    int v = (src[k] * factor(x->bsrc, k, src, dst) + dst[k] * factor(x->bdst, k, src, dst)) / 255;
                    out[k] = v > 255 ? 255 : v;
                }
                for (int k = 0; k < 4; k++) src[k] = out[k];
            }
            *d = (DWORD)src[2] | (DWORD)src[1] << 8 | (DWORD)src[0] << 16 | (DWORD)src[3] << 24;
        }
    }
}

/* Assemble @n vertices of primitive @prim into triangles */
static void draw(Ctx *x, unsigned prim, Vtx *v, int n)
{
    switch (prim) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) triangle(x, &v[i], &v[i + 1], &v[i + 2]);
        break;
    case GL_TRIANGLE_STRIP: case GL_QUAD_STRIP:
        for (int i = 0; i + 2 < n; i++) triangle(x, &v[i], &v[i + 1], &v[i + 2]);
        break;
    case GL_TRIANGLE_FAN: case GL_POLYGON:
        for (int i = 1; i + 1 < n; i++) triangle(x, &v[0], &v[i], &v[i + 1]);
        break;
    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) { triangle(x, &v[i], &v[i + 1], &v[i + 2]); triangle(x, &v[i], &v[i + 2], &v[i + 3]); }
        break;
    }                                            /* points and lines: not drawn */
}

/* Window-space vertices of @n source vertices; drops the primitive when one is behind the eye */
static void draw_vertices(Ctx *x, unsigned prim, int n, const int *index, const Vtx *imm)
{
    if (n <= 0 || prim > GL_POLYGON) return;
    Vtx stack[64], *v = n <= 64 ? stack : HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(Vtx));
    if (!v) { err(x, GL_OUT_OF_MEMORY); return; }
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        Vtx in;
        if (imm) in = imm[i];
        else from_arrays(x, index ? index[i] : i, &in);
        ok = to_window(x, &in, &v[i]);
    }
    if (ok) draw(x, prim, v, n);
    if (v != stack) HeapFree(GetProcessHeap(), 0, v);
}

static void WINAPI g_glDrawArrays(unsigned prim, int first, int count)
{
    Ctx *x = current_drawing();
    if (!x) return;
    if (count < 0) { err(x, GL_INVALID_VALUE); return; }
    if (!(x->client & CS_VERTEX) || !x->va.ptr) return;
    int stack[64], *idx = count <= 64 ? stack : HeapAlloc(GetProcessHeap(), 0, (SIZE_T)count * sizeof(int));
    if (!idx) { err(x, GL_OUT_OF_MEMORY); return; }
    for (int i = 0; i < count; i++) idx[i] = first + i;
    draw_vertices(x, prim, count, idx, 0);
    if (idx != stack) HeapFree(GetProcessHeap(), 0, idx);
}

static void WINAPI g_glDrawElements(unsigned prim, int count, unsigned type, const void *indices)
{
    Ctx *x = current_drawing();
    if (!x) return;
    if (count < 0) { err(x, GL_INVALID_VALUE); return; }
    if (!(x->client & CS_VERTEX) || !x->va.ptr || !indices) return;
    int stack[64], *idx = count <= 64 ? stack : HeapAlloc(GetProcessHeap(), 0, (SIZE_T)count * sizeof(int));
    if (!idx) { err(x, GL_OUT_OF_MEMORY); return; }
    for (int i = 0; i < count; i++)
        idx[i] = type == GL_UNSIGNED_BYTE ? ((const BYTE *)indices)[i]
               : type == GL_UNSIGNED_SHORT ? ((const unsigned short *)indices)[i] : (int)((const unsigned *)indices)[i];
    draw_vertices(x, prim, count, idx, 0);
    if (idx != stack) HeapFree(GetProcessHeap(), 0, idx);
}

static void WINAPI g_glEnd(void)
{
    Ctx *x = current_drawing();
    if (!x) { x = current(); if (x) x->in_begin = 0; return; }
    if (!x->in_begin) { err(x, GL_INVALID_OPERATION); return; }
    x->in_begin = 0;
    draw_vertices(x, x->prim, x->nimm, 0, x->imm);
}

static void WINAPI g_glRectf(float x0, float y0, float x1, float y1)
{
    g_glBegin(GL_POLYGON);
    g_glVertex2f(x0, y0); g_glVertex2f(x1, y0); g_glVertex2f(x1, y1); g_glVertex2f(x0, y1);
    g_glEnd();
}
static void WINAPI g_glRecti(int x0, int y0, int x1, int y1) { g_glRectf((float)x0, (float)y0, (float)x1, (float)y1); }

static void WINAPI g_glGetFloatv(unsigned name, float *v)
{
    Ctx *x = current();
    if (!x || !v) return;
    switch (name) {
    case 0x0BA6: memcpy(v, x->m[0][x->depth[0]], 16 * sizeof(float)); return;     /* GL_MODELVIEW_MATRIX */
    case 0x0BA7: memcpy(v, x->m[1][x->depth[1]], 16 * sizeof(float)); return;     /* GL_PROJECTION_MATRIX */
    case 0x0BA8: memcpy(v, x->m[2][x->depth[2]], 16 * sizeof(float)); return;     /* GL_TEXTURE_MATRIX */
    case 0x0B00: memcpy(v, x->color, 4 * sizeof(float)); return;                  /* GL_CURRENT_COLOR */
    }
    v[0] = 0;
}

const struct { const char *name; void *fn; } gl_draw[] = {
#define D(n) { #n, (void *)g_##n },
    D(glEnable) D(glDisable) D(glIsEnabled) D(glEnableClientState) D(glDisableClientState)
    D(glVertexPointer) D(glColorPointer) D(glTexCoordPointer)
    D(glBlendFunc) D(glAlphaFunc) D(glScissor) D(glPixelStorei) D(glTexEnvi) D(glTexEnvf)
    D(glColor4f) D(glColor3f) D(glColor4fv) D(glColor4ub) D(glColor3ub) D(glTexCoord2f) D(glTexCoord2fv)
    D(glBegin) D(glEnd) D(glVertex2f) D(glVertex2fv) D(glVertex2i) D(glVertex3f) D(glVertex3fv) D(glVertex4f)
    D(glRectf) D(glRecti)
    D(glMatrixMode) D(glLoadIdentity) D(glLoadMatrixf) D(glLoadMatrixd) D(glMultMatrixf) D(glPushMatrix) D(glPopMatrix)
    D(glOrtho) D(glFrustum) D(glTranslatef) D(glScalef)
    D(glGenTextures) D(glDeleteTextures) D(glBindTexture) D(glTexParameteri) D(glTexParameterf)
    D(glTexImage2D) D(glTexSubImage2D)
    D(glDrawArrays) D(glDrawElements) D(glGetFloatv)
#undef D
    { 0, 0 }
};
