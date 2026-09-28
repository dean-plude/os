/* limits.h — compiler limits plus the POSIX path limits (NovaOS) */
#pragma once
#include_next <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX LLONG_MAX
#endif
