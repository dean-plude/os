/*
 * NovaOS: MinGW-w64's <stdlib.h> plus the C++ overloads of abs and div
 * that the UCRT's declares, and its buffer sizes for _itoa_s and friends.
 */
#pragma once
#include_next <stdlib.h>
#ifndef _MAX_ITOSTR_BASE10_COUNT
#define _MAX_ITOSTR_BASE16_COUNT (8 + 1)
#define _MAX_ITOSTR_BASE10_COUNT (1 + 10 + 1)
#define _MAX_ITOSTR_BASE8_COUNT (11 + 1)
#define _MAX_ITOSTR_BASE2_COUNT (32 + 1)
#define _MAX_LTOSTR_BASE16_COUNT (8 + 1)
#define _MAX_LTOSTR_BASE10_COUNT (1 + 10 + 1)
#define _MAX_LTOSTR_BASE8_COUNT (11 + 1)
#define _MAX_LTOSTR_BASE2_COUNT (32 + 1)
#define _MAX_ULTOSTR_BASE16_COUNT (8 + 1)
#define _MAX_ULTOSTR_BASE10_COUNT (10 + 1)
#define _MAX_ULTOSTR_BASE8_COUNT (11 + 1)
#define _MAX_ULTOSTR_BASE2_COUNT (32 + 1)
#define _MAX_I64TOSTR_BASE16_COUNT (16 + 1)
#define _MAX_I64TOSTR_BASE10_COUNT (1 + 19 + 1)
#define _MAX_I64TOSTR_BASE8_COUNT (22 + 1)
#define _MAX_I64TOSTR_BASE2_COUNT (64 + 1)
#define _MAX_U64TOSTR_BASE16_COUNT (16 + 1)
#define _MAX_U64TOSTR_BASE10_COUNT (20 + 1)
#define _MAX_U64TOSTR_BASE8_COUNT (22 + 1)
#define _MAX_U64TOSTR_BASE2_COUNT (64 + 1)
#endif
#ifdef __cplusplus
extern "C++" {
inline long abs(long const _X) noexcept { return labs(_X); }
inline long long abs(long long const _X) noexcept { return llabs(_X); }
inline ldiv_t div(long const _A1, long const _A2) noexcept { return ldiv(_A1, _A2); }
inline lldiv_t div(long long const _A1, long long const _A2) noexcept { return lldiv(_A1, _A2); }
}
#endif
