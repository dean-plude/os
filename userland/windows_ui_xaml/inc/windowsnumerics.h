/* NovaOS: the Windows SDK's windowsnumerics.h, MinGW-w64's plus what its
 * declares and never defines that XAML calls: inverting a 3x2 and a 4x4
 * matrix (XAML's 2D and 3D transforms, components/math).  As the SDK's,
 * they return false, and a matrix of NaNs, for a matrix with no inverse.
 * And, as the SDK's do when the Windows Runtime's numerics are declared
 * (windows.foundation.numerics.h before it), each type converts to and
 * from the Windows Runtime struct of the same layout (float2 and Vector2,
 * float3 and Vector3, float4 and Vector4, float3x2 and Matrix3x2, float4x4
 * and Matrix4x4, plane and Plane, quaternion and Quaternion), which XAML's
 * composition code passes to and takes from composition objects
 * (components/comptree, components/transforms).  MinGW-w64's types have no
 * such conversions, so here each is MinGW-w64's under another name with a
 * type of its own name over it that adds them. */
#pragma once
#include <windows.foundation.numerics.values.h>
#define float2 nova_mingw_float2
#define float3 nova_mingw_float3
#define float4 nova_mingw_float4
#define float3x2 nova_mingw_float3x2
#define float4x4 nova_mingw_float4x4
#define plane nova_mingw_plane
#define quaternion nova_mingw_quaternion
#include_next <windowsnumerics.h>
#undef float2
#undef float3
#undef float4
#undef float3x2
#undef float4x4
#undef plane
#undef quaternion
#include <float.h>
#include <math.h>

namespace Windows { namespace Foundation { namespace Numerics {

#define NOVA_NUMERICS_INTEROP(Cpp, WinRT)                                                                    \
    struct Cpp : nova_mingw_##Cpp                                                                            \
    {                                                                                                        \
        using nova_mingw_##Cpp::nova_mingw_##Cpp;                                                            \
        Cpp() = default;                                                                                     \
        Cpp(const nova_mingw_##Cpp &value) : nova_mingw_##Cpp(value) {}                                      \
        Cpp(const ABI::Windows::Foundation::Numerics::WinRT &value)                                          \
            : nova_mingw_##Cpp(*reinterpret_cast<const nova_mingw_##Cpp *>(&value)) {}                       \
        operator ABI::Windows::Foundation::Numerics::WinRT() const                                           \
        {                                                                                                    \
            return *reinterpret_cast<const ABI::Windows::Foundation::Numerics::WinRT *>(this);              \
        }                                                                                                    \
    };                                                                                                       \
    static_assert(sizeof(Cpp) == sizeof(ABI::Windows::Foundation::Numerics::WinRT), #Cpp " matches " #WinRT);
NOVA_NUMERICS_INTEROP(float2, Vector2)
NOVA_NUMERICS_INTEROP(float3, Vector3)
NOVA_NUMERICS_INTEROP(float4, Vector4)
NOVA_NUMERICS_INTEROP(float3x2, Matrix3x2)
NOVA_NUMERICS_INTEROP(float4x4, Matrix4x4)
NOVA_NUMERICS_INTEROP(plane, Plane)
NOVA_NUMERICS_INTEROP(quaternion, Quaternion)
#undef NOVA_NUMERICS_INTEROP
/* (MinGW-w64's functions take its own types, which have no conversion from
 * the Windows Runtime's: normalizing a Vector3, as XAML's local transform
 * builder does with a rotation axis, goes through float3 here) */
inline float3 normalize(const ABI::Windows::Foundation::Numerics::Vector3 &v) { return normalize(float3(v)); }

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
