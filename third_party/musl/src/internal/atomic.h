/* NovaOS shim for musl's "atomic.h": only a_clz_64 (fma.c) */
#pragma once
#include <stdint.h>
static inline int a_clz_64(uint64_t x) { return __builtin_clzll(x); }
