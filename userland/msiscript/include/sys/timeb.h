/* mujs's clock on Windows (jsdate.c): _ftime, as msvcrt exports it */
#pragma once
#include <time.h>
struct _timeb { long long time; unsigned short millitm; short timezone, dstflag; };
__declspec(dllimport) void _ftime(struct _timeb *tb);
