#pragma once
#include <_nova.h>
_NOVA_BEGIN
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
struct lconv { char *decimal_point, *thousands_sep, *grouping; };
_CRTIMP char *setlocale(int category, const char *locale);
_CRTIMP struct lconv *localeconv(void);
_NOVA_END
