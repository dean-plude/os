/* malloc.h: the C library's allocator and alloca (as the Windows SDK's) */
#pragma once
#include <stdlib.h>

#define alloca(n)  __builtin_alloca(n)
#define _alloca(n) __builtin_alloca(n)
