/*
 * nls.h — what locale.c, nlsformat.c and calendar.c share.
 *
 * A locale is an index into locale_data.h's table (or LOC_INVARIANT, or
 * LOC_NONE for "no such locale").
 */
#ifndef NOVA_K32_NLS_H
#define NOVA_K32_NLS_H

#define LOC_INVARIANT (-2)
#define LOC_NONE      (-1)

/* locale.c */
int  nls_lcid(LCID lcid);
int  nls_name(LPCWSTR name);
int  nls_info(int idx, DWORD type, WCHAR *out, int cap);    /* type may carry LOCALE_NOUSEROVERRIDE */
int  nls_era(int idx, WCHAR *out, int cap);
int  nls_user(void);
int  nls_en_us(void);
BOOL nls_is_english(int idx);
BOOL nls_icu_loc(int idx, const char *cal, char *out);      /* out: 64 chars */
int  nls_icu_pattern(const char *loc, int style, WCHAR *out, int cap);
int  nls_icu_skeleton(const char *loc, const WCHAR *skel, WCHAR *out, int cap);
int  nls_icu_symbol(const char *loc, int type, int index, WCHAR *out, int cap);
BOOL nls_icu_date(const char *loc, double ms, int f[4]);
int  nls_icu_calname(const char *loc, WCHAR *out, int cap);
int  nls_icu_calendars(int idx, char (*out)[24], int max);

/* calendar.c: a date in one of the calendars (CAL_GREGORIAN 1 ... CAL_UMALQURA 23) */
typedef struct {
    int era;                       /* ICU's era number (Japanese: 236 is Reiwa) */
    int year, month, day;          /* year in the era; month from 1 as Windows counts (Hebrew: 1-13) */
    BOOL leap;                     /* a Hebrew leap year (13 months) */
} CalDate;

int  nls_calendars(int idx, DWORD *out, int max);            /* the locale's calendars, its default first */
BOOL nls_cal_date(DWORD cal, const SYSTEMTIME *st, CalDate *cd);
int  nls_cal_pattern(int idx, DWORD cal, int kind, DWORD uo, WCHAR *out, int cap);  /* kind: CALPAT_* */
int  nls_cal_month(int idx, DWORD cal, int month, BOOL leap, BOOL abbr, BOOL genitive, WCHAR *out, int cap);
int  nls_cal_day(int idx, DWORD cal, int monday0, int abbr, WCHAR *out, int cap);   /* abbr: 0 full, 1 short, 2 shortest */
int  nls_cal_era(int idx, DWORD cal, const CalDate *cd, WCHAR *out, int cap);
int  nls_hebrew_number(int v, WCHAR *out, int cap);
enum { CALPAT_SHORT, CALPAT_LONG, CALPAT_YEARMONTH, CALPAT_MONTHDAY };

#endif
