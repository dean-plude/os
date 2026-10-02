#pragma once
#include <_nova.h>
_NOVA_BEGIN
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
struct lconv {                                  /* Windows' layout */
    char *decimal_point, *thousands_sep, *grouping;
    char *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep, *mon_grouping;
    char *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space;
    char p_sign_posn, n_sign_posn;
    unsigned short *_W_decimal_point, *_W_thousands_sep, *_W_int_curr_symbol, *_W_currency_symbol;
    unsigned short *_W_mon_decimal_point, *_W_mon_thousands_sep, *_W_positive_sign, *_W_negative_sign;
};
_CRTIMP char *setlocale(int category, const char *locale);
_CRTIMP struct lconv *localeconv(void);
_NOVA_END
