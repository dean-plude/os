/*
 * d2d1.dll — Direct2D.  NovaOS has no Direct2D renderer yet, so no
 * factory can be made; programs that try it fall back to GDI.  The math
 * helpers work.
 */
#include <windows.h>
#include <math.h>

#define D2D __declspec(dllexport)

int _fltused = 0x9875;      /* the compiler references it for float code */

D2D HRESULT WINAPI D2D1CreateFactory(int type, const GUID *iid, const void *opts, void **out)
{
    (void)type; (void)iid; (void)opts;
    if (!out) return E_POINTER;
    *out = 0;
    return E_NOTIMPL;
}
D2D HRESULT WINAPI D2D1CreateDevice(void *dxgi, const void *props, void **out)
{
    (void)dxgi; (void)props;
    if (out) *out = 0;
    return E_NOTIMPL;
}
D2D HRESULT WINAPI D2D1CreateDeviceContext(void *surface, const void *props, void **out)
{
    (void)surface; (void)props;
    if (out) *out = 0;
    return E_NOTIMPL;
}

typedef struct { float x, y; } D2D_POINT_2F_;
typedef struct { float m11, m12, m21, m22, dx, dy; } D2D_MATRIX_3X2_F_;

D2D void WINAPI D2D1MakeRotateMatrix(float angle, D2D_POINT_2F_ c, D2D_MATRIX_3X2_F_ *m)
{
    float a = angle * 3.14159265358979f / 180.0f, s = sinf(a), k = cosf(a);
    m->m11 = k;  m->m12 = s;
    m->m21 = -s; m->m22 = k;
    m->dx = c.x - c.x * k + c.y * s;
    m->dy = c.y - c.x * s - c.y * k;
}
D2D void WINAPI D2D1MakeSkewMatrix(float ax, float ay, D2D_POINT_2F_ c, D2D_MATRIX_3X2_F_ *m)
{
    float tx = tanf(ax * 3.14159265358979f / 180.0f), ty = tanf(ay * 3.14159265358979f / 180.0f);
    m->m11 = 1;  m->m12 = ty;
    m->m21 = tx; m->m22 = 1;
    m->dx = -c.y * tx;
    m->dy = -c.x * ty;
}
D2D BOOL WINAPI D2D1IsMatrixInvertible(const D2D_MATRIX_3X2_F_ *m)
{
    return m->m11 * m->m22 - m->m12 * m->m21 != 0.0f;
}
D2D BOOL WINAPI D2D1InvertMatrix(D2D_MATRIX_3X2_F_ *m)
{
    float det = m->m11 * m->m22 - m->m12 * m->m21;
    if (det == 0.0f) return FALSE;
    D2D_MATRIX_3X2_F_ r;
    r.m11 = m->m22 / det;  r.m12 = -m->m12 / det;
    r.m21 = -m->m21 / det; r.m22 = m->m11 / det;
    r.dx = -(m->dx * r.m11 + m->dy * r.m21);
    r.dy = -(m->dx * r.m12 + m->dy * r.m22);
    *m = r;
    return TRUE;
}
D2D void WINAPI D2D1SinCos(float a, float *s, float *c) { *s = sinf(a); *c = cosf(a); }
D2D float WINAPI D2D1Tan(float a) { return tanf(a); }
D2D float WINAPI D2D1Vec3Length(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }
