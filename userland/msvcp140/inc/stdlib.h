/*
 * NovaOS: MinGW-w64's <stdlib.h> plus the C++ overloads of abs and div
 * that the UCRT's declares.
 */
#pragma once
#include_next <stdlib.h>
#ifdef __cplusplus
extern "C++" {
inline long abs(long const _X) noexcept { return labs(_X); }
inline long long abs(long long const _X) noexcept { return llabs(_X); }
inline ldiv_t div(long const _A1, long const _A2) noexcept { return ldiv(_A1, _A2); }
inline lldiv_t div(long long const _A1, long long const _A2) noexcept { return lldiv(_A1, _A2); }
}
#endif
