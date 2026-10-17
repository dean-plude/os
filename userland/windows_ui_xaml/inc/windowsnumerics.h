/* NovaOS: the Windows SDK's windowsnumerics.h, MinGW-w64's plus what its
 * declares and never defines that XAML calls: inverting a 3x2 and a 4x4
 * matrix (XAML's 2D and 3D transforms, components/math).  As the SDK's,
 * they return false, and a matrix of NaNs, for a matrix with no inverse. */
#pragma once
#include_next <windowsnumerics.h>
#include <float.h>
#include <math.h>

namespace Windows { namespace Foundation { namespace Numerics {

inline bool invert(const float3x2 &m, float3x2 *out)
{
    float det = m.m11 * m.m22 - m.m21 * m.m12;
    if (fabsf(det) < FLT_EPSILON) {
        float n = NAN;
        *out = float3x2(n, n, n, n, n, n);
        return false;
    }
    float inv = 1.0f / det;
    *out = float3x2(m.m22 * inv, -m.m12 * inv,
                    -m.m21 * inv, m.m11 * inv,
                    (m.m21 * m.m32 - m.m31 * m.m22) * inv, (m.m31 * m.m12 - m.m11 * m.m32) * inv);
    return true;
}

/* by cofactors: the 2x2 determinants of the lower two rows (s) and of the
 * upper two (c) */
inline bool invert(const float4x4 &m, float4x4 *out)
{
    float s0 = m.m11 * m.m22 - m.m21 * m.m12, s1 = m.m11 * m.m23 - m.m21 * m.m13;
    float s2 = m.m11 * m.m24 - m.m21 * m.m14, s3 = m.m12 * m.m23 - m.m22 * m.m13;
    float s4 = m.m12 * m.m24 - m.m22 * m.m14, s5 = m.m13 * m.m24 - m.m23 * m.m14;
    float c5 = m.m33 * m.m44 - m.m43 * m.m34, c4 = m.m32 * m.m44 - m.m42 * m.m34;
    float c3 = m.m32 * m.m43 - m.m42 * m.m33, c2 = m.m31 * m.m44 - m.m41 * m.m34;
    float c1 = m.m31 * m.m43 - m.m41 * m.m33, c0 = m.m31 * m.m42 - m.m41 * m.m32;
    float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (fabsf(det) < FLT_EPSILON) {
        float n = NAN;
        *out = float4x4(n, n, n, n, n, n, n, n, n, n, n, n, n, n, n, n);
        return false;
    }
    float i = 1.0f / det;
    *out = float4x4(
        ( m.m22 * c5 - m.m23 * c4 + m.m24 * c3) * i, (-m.m12 * c5 + m.m13 * c4 - m.m14 * c3) * i,
        ( m.m42 * s5 - m.m43 * s4 + m.m44 * s3) * i, (-m.m32 * s5 + m.m33 * s4 - m.m34 * s3) * i,
        (-m.m21 * c5 + m.m23 * c2 - m.m24 * c1) * i, ( m.m11 * c5 - m.m13 * c2 + m.m14 * c1) * i,
        (-m.m41 * s5 + m.m43 * s2 - m.m44 * s1) * i, ( m.m31 * s5 - m.m33 * s2 + m.m34 * s1) * i,
        ( m.m21 * c4 - m.m22 * c2 + m.m24 * c0) * i, (-m.m11 * c4 + m.m12 * c2 - m.m14 * c0) * i,
        ( m.m41 * s4 - m.m42 * s2 + m.m44 * s0) * i, (-m.m31 * s4 + m.m32 * s2 - m.m34 * s0) * i,
        (-m.m21 * c3 + m.m22 * c1 - m.m23 * c0) * i, ( m.m11 * c3 - m.m12 * c1 + m.m13 * c0) * i,
        (-m.m41 * s3 + m.m42 * s1 - m.m43 * s0) * i, ( m.m31 * s3 - m.m32 * s1 + m.m33 * s0) * i);
    return true;
}

}}}
