/* NovaOS: the part of Microsoft's SafeInt library (MIT; github.com/
 * microsoft/SafeInt) XAML's core calls: SafeMultiply, which stores a * b
 * in result and answers true, or answers false when the product overflows
 * result's type (core/native/text's SharedWicBitmap.cpp sizes a bitmap so). */
#pragma once

template <typename T, typename U, typename E>
inline bool SafeMultiply(T a, U b, E &result) noexcept
{
    return !__builtin_mul_overflow(a, b, &result);
}
