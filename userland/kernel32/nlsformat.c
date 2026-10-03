/*
 * nlsformat.c — GetDateFormat, GetTimeFormat, GetNumberFormat,
 * GetCurrencyFormat (A, W and Ex) and GetDurationFormat in the locale asked
 * for.
 *
 * The pictures, symbols and orders come from GetLocaleInfo's answers
 * (locale.c: the English tables, or ICU for every other locale), so
 * "de-DE" gives "Freitag, 2. Oktober 2026", "14:05:09", "1.234.567,89" and
 * "1.234.567,89 €", and "ja-JP" "2026年10月2日" and "¥1,234,568", as on
 * Windows.  The picture rules (d dd ddd dddd, M... MMMM with the genitive
 * month when the picture has a day number, y yy yyyy, g gg, h hh H HH m mm s ss
 * t tt, '...' quoting) and the number orders are Windows' documented ones.
 * Without LOCALE_NOUSEROVERRIDE the user's locale formats with the user's
 * overrides (locale.c).  Dates are written in the locale's calendar
 * (LOCALE_ICALENDARTYPE: th-TH's Buddhist years, ar-SA's Um Al Qura months),
 * or its alternative one with DATE_USE_ALT_CALENDAR (ja-JP's eras); the
 * conversions and names are calendar.c's.
 */
#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

void *memcpy(void *d, const void *s, size_t n);

#define K32 __declspec(dllexport)

#include "nls.h"

#define LOCALE_USE_CP_ACP_     0x40000000u
#define LOCALE_RETURN_GENITIVE_NAMES_ 0x10000000u

#define DATE_LTRREADING_    0x10
#define DATE_RTLREADING_    0x20
#define DATE_AUTOLAYOUT_    0x40

#define CAP 256                                    /* every result fits (or is refused) */

typedef struct { WCHAR s[CAP]; int n; BOOL over; } Out;

static void add_c(Out *o, WCHAR c) { if (o->n < CAP - 1) o->s[o->n++] = c; else o->over = TRUE; }
static void add_w(Out *o, const WCHAR *s, int max)
{
    for (int i = 0; s[i] && (max < 0 || i < max); i++) add_c(o, s[i]);
}
static void add_num(Out *o, ULONGLONG v, int digits)
{
    WCHAR t[24];
    int k = 0;
    do { t[k++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (k < digits && k < 24) t[k++] = '0';
    while (k) add_c(o, t[--k]);
}

/* the result into the caller's buffer (@cap 0: just the size) */
static int give(const Out *o, LPWSTR out, int cap)
{
    if (o->over) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (cap < 0 || (cap && !out)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!cap) return o->n + 1;
    if (cap < o->n + 1) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, o->s, 2 * (SIZE_T)o->n);
    out[o->n] = 0;
    return o->n + 1;
}

/* a W result as ANSI (the ANSI code page is UTF-8), @cap in bytes */
static int give_a(const WCHAR *w, int n, LPSTR out, int cap)
{
    char tmp[CAP * 3];
    int k = w2u(w, n, tmp, sizeof(tmp));
    if (k < 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (cap < 0 || (cap && !out)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!cap) return k + 1;
    if (cap < k + 1) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, tmp, (SIZE_T)k);
    out[k] = 0;
    return k + 1;
}

static int info(int idx, DWORD type, WCHAR *out)
{
    int n = nls_info(idx, type, out, CAP);
    if (n < 0) { out[0] = 0; n = 0; }
    return n;
}

static int info_int(int idx, DWORD type)
{
    WCHAR w[CAP];
    info(idx, type, w);
    int v = 0;
    for (const WCHAR *c = w; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
    return v;
}

/* -----------------------------------------------------------------------
 * Dates and times
 * ----------------------------------------------------------------------- */
static BOOL leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static BOOL date_ok(const SYSTEMTIME *st)
{
    static const BYTE days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (st->wYear < 1601 || st->wYear > 30827 || st->wMonth < 1 || st->wMonth > 12 || !st->wDay) return FALSE;
    return st->wDay <= days[st->wMonth - 1] + (st->wMonth == 2 && leap(st->wYear));
}
static BOOL time_ok(const SYSTEMTIME *st)
{
    return st->wHour < 24 && st->wMinute < 60 && st->wSecond < 60 && st->wMilliseconds < 1000;
}

/* 0 Sunday ... 6 Saturday (worked out: callers' wDayOfWeek is ignored, as on Windows) */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* A picture split into fields (a letter repeated: "dddd", "HH") and the
 * text between them ('quoted' text unquoted) */
typedef struct { WCHAR c; int run; int at, len; } Item;     /* c 0: text @at/@len in Pic.text */
typedef struct { Item it[64]; int n; WCHAR text[CAP]; int tn; } Pic;

#define DATE_FIELDS     "dMyg"
#define TIME_FIELDS     "hHmst"
#define DURATION_FIELDS "dhHmsf"

static BOOL is_field(WCHAR c, const char *set)
{
    for (; *set; set++) if (c == (WCHAR)*set) return TRUE;
    return FALSE;
}

static void split(const WCHAR *p, const char *set, Pic *pic)
{
    pic->n = pic->tn = 0;
    while (*p && pic->n < 64) {
        Item *it = &pic->it[pic->n];
        if (is_field(*p, set)) {
            it->c = *p;
            it->run = 0;
            while (p[it->run] == *p) it->run++;
            p += it->run;
            pic->n++;
            continue;
        }
        it->c = 0;
        it->at = pic->tn;
        while (*p && !is_field(*p, set)) {
            if (*p == '\'') {                                /* 'text', '' for a quote */
                p++;
                if (*p == '\'') { if (pic->tn < CAP) pic->text[pic->tn++] = '\''; p++; continue; }
                while (*p) {
                    if (*p == '\'') { if (p[1] == '\'') { p++; } else { p++; break; } }
                    if (pic->tn < CAP) pic->text[pic->tn++] = *p;
                    p++;
                }
                continue;
            }
            if (pic->tn < CAP) pic->text[pic->tn++] = *p;
            p++;
        }
        it->len = pic->tn - it->at;
        if (it->len) pic->n++;
    }
}

/* drop field @i and the text that joins it to the rest (" tt", ":ss") */
static void drop(Pic *pic, int i)
{
    int from = i, to = i;
    if (i >= 2 && !pic->it[i - 1].c) from = i - 1;           /* the separator before it */
    else if (i + 2 < pic->n && !pic->it[i + 1].c) to = i + 1; /* (first: the one after) */
    int k = to - from + 1;
    for (int j = from; j + k < pic->n; j++) pic->it[j] = pic->it[j + k];
    pic->n -= k;
}

/* a date in a calendar, for render() */
typedef struct {
    int idx;
    DWORD uo;                                                /* LOCALE_NOUSEROVERRIDE or 0 */
    DWORD cal;
    CalDate cd;
} DateCtx;

static void add_hebrew(Out *o, int v)
{
    WCHAR w[16];
    nls_hebrew_number(v, w, 16);
    add_w(o, w, -1);
}

static int render(const DateCtx *dc, const SYSTEMTIME *st, const WCHAR *picture, BOOL time, DWORD tflags, Out *o)
{
    Pic pic;
    int idx = dc->idx;
    const CalDate *cd = &dc->cd;
    split(picture, time ? TIME_FIELDS : DATE_FIELDS, &pic);
    if (time) {
        for (int i = pic.n - 1; i >= 0; i--) {
            WCHAR c = pic.it[i].c;
            if ((c == 's' && (tflags & (TIME_NOSECONDS | TIME_NOMINUTESORSECONDS))) ||
                (c == 'm' && (tflags & TIME_NOMINUTESORSECONDS)) ||
                (c == 't' && (tflags & TIME_NOTIMEMARKER))) {
                drop(&pic, i);
                if (i > pic.n) i = pic.n;
            }
        }
    }
    BOOL genitive = FALSE;                                   /* "d MMMM": the genitive month */
    for (int i = 0; i < pic.n; i++) if (pic.it[i].c == 'd' && pic.it[i].run <= 2) genitive = TRUE;
    WCHAR w[CAP];
    o->n = 0;
    o->over = FALSE;
    for (int i = 0; i < pic.n; i++) {
        const Item *it = &pic.it[i];
        int r = it->run;
        switch (it->c) {
        case 0:
            for (int k = 0; k < it->len; k++) add_c(o, pic.text[it->at + k]);
            break;
        case 'd':
            if (r <= 2) {
                if (dc->cal == 8) add_hebrew(o, cd->day);
                else add_num(o, (unsigned)cd->day, r);
            } else {
                int mon = (weekday(st->wYear, st->wMonth, st->wDay) + 6) % 7;   /* Monday 0 */
                if (nls_cal_day(idx, dc->cal, mon, r == 3, w, CAP) < 0) w[0] = 0;
                add_w(o, w, -1);
            }
            break;
        case 'M':
            if (r <= 2) add_num(o, (unsigned)cd->month, r);
            else {
                if (nls_cal_month(idx, dc->cal, cd->month, cd->leap, r == 3, genitive && r > 3, w, CAP) < 0) w[0] = 0;
                add_w(o, w, -1);
            }
            break;
        case 'y':
            if (dc->cal == 8) add_hebrew(o, cd->year);
            else if (dc->cal == 3 || dc->cal == 4) add_num(o, (unsigned)cd->year, r == 2 ? 2 : 1);  /* the year of the era */
            else if (r <= 2) add_num(o, (unsigned)cd->year % 100, r);
            else add_num(o, (unsigned)cd->year, 4);
            break;
        case 'g':
            if (nls_cal_era(idx, dc->cal, cd, w, CAP) < 0) w[0] = 0;
            add_w(o, w, -1);
            break;
        case 'h': {
            unsigned h = st->wHour % 12;
            if (tflags & TIME_FORCE24HOURFORMAT) h = st->wHour;
            else if (!h) h = 12;
            add_num(o, h, r > 1 ? 2 : 1);
            break;
        }
        case 'H': add_num(o, st->wHour, r > 1 ? 2 : 1); break;
        case 'm': add_num(o, st->wMinute, r > 1 ? 2 : 1); break;
        case 's': add_num(o, st->wSecond, r > 1 ? 2 : 1); break;
        case 't':
            info(idx, (st->wHour < 12 ? 0x28 /* LOCALE_S1159 */ : 0x29 /* LOCALE_S2359 */) | dc->uo, w);
            add_w(o, w, r == 1 ? 1 : -1);
            break;
        }
    }
    o->s[o->n] = 0;
    return o->n;
}

static int date_format(int idx, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, Out *o)
{
    if (idx == LOC_NONE) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    DWORD kinds = flags & (DATE_SHORTDATE | DATE_LONGDATE | DATE_YEARMONTH | DATE_MONTHDAY);
    if ((kinds & (kinds - 1)) || (fmt && kinds) || (flags & DATE_LTRREADING_ && flags & DATE_RTLREADING_) ||
        (flags & ~(kinds | DATE_USE_ALT_CALENDAR | DATE_LTRREADING_ | DATE_RTLREADING_ | DATE_AUTOLAYOUT_ |
                   LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP_))) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    SYSTEMTIME now;
    if (!st) { GetLocalTime(&now); st = &now; }
    else if (!date_ok(st)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    /* the calendar: the locale's (or the user's choice), or with
     * DATE_USE_ALT_CALENDAR its alternative in that calendar's own format */
    DateCtx dc = { idx, flags & LOCALE_NOUSEROVERRIDE, 0 };
    if (flags & DATE_USE_ALT_CALENDAR) {
        dc.cal = (DWORD)info_int(idx, 0x100B /* LOCALE_IOPTIONALCALENDAR */ | LOCALE_NOUSEROVERRIDE);
        if (dc.cal) dc.uo = LOCALE_NOUSEROVERRIDE;
    }
    if (!dc.cal) dc.cal = (DWORD)info_int(idx, 0x1009 /* LOCALE_ICALENDARTYPE */ | dc.uo);
    if (!nls_cal_date(dc.cal, st, &dc.cd)) { dc.cal = 1; nls_cal_date(1, st, &dc.cd); }
    WCHAR pic[CAP];
    if (!fmt) {
        int kind = kinds == DATE_LONGDATE ? CALPAT_LONG : kinds == DATE_YEARMONTH ? CALPAT_YEARMONTH :
                   kinds == DATE_MONTHDAY ? CALPAT_MONTHDAY : CALPAT_SHORT;
        if (nls_cal_pattern(idx, dc.cal, kind, dc.uo, pic, CAP) < 0) pic[0] = 0;
        fmt = pic;
    }
    render(&dc, st, fmt, FALSE, 0, o);
    return 1;
}

static int time_format(int idx, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, Out *o)
{
    if (idx == LOC_NONE) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if ((fmt && (flags & LOCALE_NOUSEROVERRIDE)) ||
        (flags & ~(TIME_NOMINUTESORSECONDS | TIME_NOSECONDS | TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT |
                   LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP_))) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    SYSTEMTIME now;
    if (!st) { GetLocalTime(&now); st = &now; }
    else if (!time_ok(st)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    DateCtx dc = { idx, flags & LOCALE_NOUSEROVERRIDE, 1 };
    WCHAR pic[CAP];
    if (!fmt) { info(idx, 0x1003 /* LOCALE_STIMEFORMAT */ | dc.uo, pic); fmt = pic; }
    render(&dc, st, fmt, TRUE, flags, o);
    return 1;
}

K32 int WINAPI GetDateFormatEx(LPCWSTR loc, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap, LPCWSTR cal)
{
    (void)cal;
    Out o;
    return date_format(nls_name(loc), flags, st, fmt, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetDateFormatW(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    Out o;
    return date_format(nls_lcid(lcid), flags, st, fmt, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetDateFormatA(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCSTR fmt, LPSTR out, int cap)
{
    WCHAR wf[CAP];
    if (fmt && u2w(fmt, -1, wf, CAP - 1) < 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (fmt) wf[u2w(fmt, -1, wf, CAP - 1)] = 0;
    Out o;
    return date_format(nls_lcid(lcid), flags, st, fmt ? wf : NULL, &o) ? give_a(o.s, o.n, out, cap) : 0;
}

K32 int WINAPI GetTimeFormatEx(LPCWSTR loc, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    Out o;
    return time_format(nls_name(loc), flags, st, fmt, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetTimeFormatW(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    Out o;
    return time_format(nls_lcid(lcid), flags, st, fmt, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetTimeFormatA(LCID lcid, DWORD flags, const SYSTEMTIME *st, LPCSTR fmt, LPSTR out, int cap)
{
    WCHAR wf[CAP];
    if (fmt && u2w(fmt, -1, wf, CAP - 1) < 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (fmt) wf[u2w(fmt, -1, wf, CAP - 1)] = 0;
    Out o;
    return time_format(nls_lcid(lcid), flags, st, fmt ? wf : NULL, &o) ? give_a(o.s, o.n, out, cap) : 0;
}

/* -----------------------------------------------------------------------
 * Numbers and currency
 * ----------------------------------------------------------------------- */
/* NUMBERFMTW is CURRENCYFMTW's start: one type for both (the currency
 * fields are read for currency only) */
typedef CURRENCYFMTW NumFmtW;

/* what one number is formatted with */
typedef struct {
    int digits, lzero;
    BYTE groups[8]; int ngroups; BOOL repeat;      /* sizes from the decimal point out */
    WCHAR dec[8], thou[8], neg[8], sym[16];
    int negorder, posorder;
} Spec;

/* "3;2;0": 3, then 2, the 2 repeating (a final 0) */
static void groups_of(const WCHAR *s, Spec *sp)
{
    sp->ngroups = 0;
    sp->repeat = FALSE;
    while (*s && sp->ngroups < 8) {
        int v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
        if (*s == ';') s++;
        else if (*s) break;
        if (!v) { sp->repeat = sp->ngroups > 0; break; }
        sp->groups[sp->ngroups++] = (BYTE)v;
    }
}

/* NUMBERFMT's Grouping: 3 is "3;0", 32 "3;2;0", 320 "3;2" (0 last: no repeat) */
static void groups_uint(UINT g, Spec *sp)
{
    BYTE d[10];
    int n = 0;
    do d[n++] = (BYTE)(g % 10); while ((g /= 10) && n < 10);
    sp->ngroups = 0;
    sp->repeat = d[0] != 0;
    for (int i = n - 1; i >= (d[0] ? 0 : 1) && sp->ngroups < 8; i--) sp->groups[sp->ngroups++] = d[i];
    if (sp->ngroups == 1 && !sp->groups[0]) { sp->ngroups = 0; sp->repeat = FALSE; }
}

static void copy_sep(WCHAR *to, int cap, const WCHAR *from)
{
    int i = 0;
    if (from) for (; from[i] && i < cap - 1; i++) to[i] = from[i];
    to[i] = 0;
}

static BOOL spec_of(int idx, DWORD uo, const NumFmtW *f, BOOL currency, Spec *sp)
{
    info(idx, 0x51 /* LOCALE_SNEGATIVESIGN */ | uo, sp->sym);
    copy_sep(sp->neg, 8, sp->sym);
    sp->sym[0] = 0;
    if (f) {
        if (f->NumDigits > 9 || f->LeadingZero > 1 || !f->lpDecimalSep || !f->lpThousandSep ||
            f->NegativeOrder > (currency ? 15u : 4u) || (currency && (f->PositiveOrder > 3 || !f->lpCurrencySymbol)) ||
            (f->Grouping > 9 && f->Grouping != 32 && f->Grouping != 320 && f->Grouping != 30))
            return FALSE;
        sp->digits = (int)f->NumDigits;
        sp->lzero = (int)f->LeadingZero;
        groups_uint(f->Grouping, sp);
        copy_sep(sp->dec, 8, f->lpDecimalSep);
        copy_sep(sp->thou, 8, f->lpThousandSep);
        sp->negorder = (int)f->NegativeOrder;
        sp->posorder = currency ? (int)f->PositiveOrder : 0;
        if (currency) copy_sep(sp->sym, 16, f->lpCurrencySymbol);
        return TRUE;
    }
    WCHAR w[CAP];
    sp->digits = info_int(idx, (currency ? 0x19 /* LOCALE_ICURRDIGITS */ : 0x11 /* LOCALE_IDIGITS */) | uo);
    sp->lzero = info_int(idx, 0x12 /* LOCALE_ILZERO */ | uo);
    info(idx, (currency ? 0x18 /* LOCALE_SMONGROUPING */ : 0x10 /* LOCALE_SGROUPING */) | uo, w);
    groups_of(w, sp);
    info(idx, (currency ? 0x16 /* LOCALE_SMONDECIMALSEP */ : 0x0E /* LOCALE_SDECIMAL */) | uo, w);
    copy_sep(sp->dec, 8, w);
    info(idx, (currency ? 0x17 /* LOCALE_SMONTHOUSANDSEP */ : 0x0F /* LOCALE_STHOUSAND */) | uo, w);
    copy_sep(sp->thou, 8, w);
    sp->negorder = info_int(idx, (currency ? 0x1C /* LOCALE_INEGCURR */ : 0x1010 /* LOCALE_INEGNUMBER */) | uo);
    if (currency) {
        sp->posorder = info_int(idx, 0x1B /* LOCALE_ICURRENCY */ | uo);
        info(idx, 0x14 /* LOCALE_SCURRENCY */ | uo, w);
        copy_sep(sp->sym, 16, w);
    }
    if (sp->digits > 9) sp->digits = 9;
    return TRUE;
}

/* @value ("[-]digits[.digits]") rounded to sp->digits places and grouped
 * into @o (no sign); *neg: it is below zero.  FALSE: not a number. */
static BOOL digits_of(LPCWSTR value, const Spec *sp, Out *o, BOOL *neg)
{
    WCHAR ip[CAP], fp[16];
    int ni = 0, nf = 0, i = 0, any = 0;
    BOOL up = FALSE;
    *neg = FALSE;
    if (!value) return FALSE;
    if (value[i] == '-') { *neg = TRUE; i++; }
    for (; value[i] >= '0' && value[i] <= '9'; i++, any++) if (ni < 100) ip[ni++] = value[i];
    if (value[i] == '.') {
        int seen = 0;                                        /* fraction digits read */
        for (i++; value[i] >= '0' && value[i] <= '9'; i++, any++, seen++) {
            if (seen < sp->digits) fp[nf++] = value[i];
            else if (seen == sp->digits) up = value[i] >= '5';   /* the first dropped digit: half away from zero */
        }
    }
    if (value[i] || !any) return FALSE;
    while (ni > 1 && ip[0] == '0') { for (int k = 1; k < ni; k++) ip[k - 1] = ip[k]; ni--; }
    if (!ni) ip[ni++] = '0';
    while (nf < sp->digits) fp[nf++] = '0';
    for (int k = nf - 1; up && k >= 0; k--) { if (fp[k] == '9') fp[k] = '0'; else { fp[k]++; up = FALSE; } }
    for (int k = ni - 1; up && k >= 0; k--) { if (ip[k] == '9') ip[k] = '0'; else { ip[k]++; up = FALSE; } }
    if (up) { for (int k = ni; k > 0; k--) ip[k] = ip[k - 1]; ip[0] = '1'; ni++; }
    BOOL zero = TRUE;
    for (int k = 0; k < ni; k++) if (ip[k] != '0') zero = FALSE;
    for (int k = 0; k < nf; k++) if (fp[k] != '0') zero = FALSE;
    if (zero) *neg = FALSE;                                  /* no "-0.00" */

    /* where the separators go, from the decimal point out */
    BOOL sep_after[CAP] = { 0 };                             /* a separator after ip[k] */
    int pos = ni, g = 0;
    while (sp->ngroups) {
        int size = g < sp->ngroups ? sp->groups[g] : sp->repeat ? sp->groups[sp->ngroups - 1] : 0;
        if (!size || pos - size <= 0) break;
        pos -= size;
        sep_after[pos - 1] = TRUE;
        g++;
    }
    o->n = 0;
    o->over = FALSE;
    if (!(ni == 1 && ip[0] == '0' && !sp->lzero && sp->digits)) {
        for (int k = 0; k < ni; k++) {
            add_c(o, ip[k]);
            if (sep_after[k]) add_w(o, sp->thou, -1);
        }
    }
    if (sp->digits) {
        add_w(o, sp->dec, -1);
        for (int k = 0; k < sp->digits; k++) add_c(o, fp[k]);
    }
    return TRUE;
}

/* @shape: 'n' the number, 'C' the currency symbol, '-' the negative sign,
 * anything else as is */
static void lay_out(const char *shape, const Spec *sp, const Out *num, Out *o)
{
    o->n = 0;
    o->over = FALSE;
    for (const char *c = shape; *c; c++) {
        if (*c == 'n') add_w(o, num->s, num->n);
        else if (*c == 'C') add_w(o, sp->sym, -1);
        else if (*c == '-') add_w(o, sp->neg, -1);
        else add_c(o, (WCHAR)*c);
    }
    o->over |= num->over;
    o->s[o->n] = 0;
}

static int number_format(int idx, DWORD flags, LPCWSTR value, const NumFmtW *f, BOOL currency, Out *o)
{
    if (idx == LOC_NONE || !value) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if ((f && flags) || (flags & ~(LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP_))) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    Spec sp;
    Out num;
    BOOL neg;
    if (!spec_of(idx, flags & LOCALE_NOUSEROVERRIDE, f, currency, &sp) || !digits_of(value, &sp, &num, &neg)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    num.s[num.n] = 0;
    static const char *const negnum[5] = { "(n)", "-n", "- n", "n-", "n -" };
    static const char *const poscur[4] = { "Cn", "nC", "C n", "n C" };
    static const char *const negcur[16] = { "(Cn)", "-Cn", "C-n", "Cn-", "(nC)", "-nC", "n-C", "nC-", "-n C", "-C n",
                                            "n C-", "C n-", "C -n", "n- C", "(C n)", "(n C)" };
    const char *shape = currency ? (neg ? negcur[sp.negorder & 15] : poscur[sp.posorder & 3])
                                 : (neg ? negnum[sp.negorder % 5] : "n");
    lay_out(shape, &sp, &num, o);
    return 1;
}

K32 int WINAPI GetNumberFormatEx(LPCWSTR loc, DWORD flags, LPCWSTR value, const NUMBERFMTW *fmt, LPWSTR out, int cap)
{
    Out o;
    return number_format(nls_name(loc), flags, value, (const NumFmtW *)fmt, FALSE, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetNumberFormatW(LCID lcid, DWORD flags, LPCWSTR value, const NUMBERFMTW *fmt, LPWSTR out, int cap)
{
    Out o;
    return number_format(nls_lcid(lcid), flags, value, (const NumFmtW *)fmt, FALSE, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetCurrencyFormatEx(LPCWSTR loc, DWORD flags, LPCWSTR value, const CURRENCYFMTW *fmt, LPWSTR out, int cap)
{
    Out o;
    return number_format(nls_name(loc), flags, value, fmt, TRUE, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetCurrencyFormatW(LCID lcid, DWORD flags, LPCWSTR value, const CURRENCYFMTW *fmt, LPWSTR out, int cap)
{
    Out o;
    return number_format(nls_lcid(lcid), flags, value, fmt, TRUE, &o) ? give(&o, out, cap) : 0;
}

/* the A forms: NUMBERFMTA/CURRENCYFMTA's strings and the value converted */
static int number_a(LCID lcid, DWORD flags, LPCSTR value, const CURRENCYFMTA *fa, BOOL currency, LPSTR out, int cap)
{
    WCHAR v[CAP], dec[8], thou[8], sym[16];
    NumFmtW fw, *f = NULL;
    if (!value || u2w(value, -1, v, CAP - 1) < 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    v[u2w(value, -1, v, CAP - 1)] = 0;
    if (fa) {
        fw.NumDigits = fa->NumDigits;
        fw.LeadingZero = fa->LeadingZero;
        fw.Grouping = fa->Grouping;
        fw.NegativeOrder = fa->NegativeOrder;
        fw.PositiveOrder = currency ? fa->PositiveOrder : 0;
        fw.lpDecimalSep = fw.lpThousandSep = fw.lpCurrencySymbol = NULL;
        if (fa->lpDecimalSep) { int n = u2w(fa->lpDecimalSep, -1, dec, 7); dec[n < 0 ? 0 : n] = 0; fw.lpDecimalSep = dec; }
        if (fa->lpThousandSep) { int n = u2w(fa->lpThousandSep, -1, thou, 7); thou[n < 0 ? 0 : n] = 0; fw.lpThousandSep = thou; }
        if (currency && fa->lpCurrencySymbol) {
            int n = u2w(fa->lpCurrencySymbol, -1, sym, 15);
            sym[n < 0 ? 0 : n] = 0;
            fw.lpCurrencySymbol = sym;
        }
        f = &fw;
    }
    Out o;
    return number_format(nls_lcid(lcid), flags, v, f, currency, &o) ? give_a(o.s, o.n, out, cap) : 0;
}
K32 int WINAPI GetNumberFormatA(LCID lcid, DWORD flags, LPCSTR value, const NUMBERFMTA *fmt, LPSTR out, int cap)
{
    return number_a(lcid, flags, value, (const CURRENCYFMTA *)fmt, FALSE, out, cap);
}
K32 int WINAPI GetCurrencyFormatA(LCID lcid, DWORD flags, LPCSTR value, const CURRENCYFMTA *fmt, LPSTR out, int cap)
{
    return number_a(lcid, flags, value, fmt, TRUE, out, cap);
}

/* -----------------------------------------------------------------------
 * Durations
 * ----------------------------------------------------------------------- */
/* GetDurationFormat: @ticks (100 ns) or @d's hours, minutes, seconds and
 * milliseconds in the picture's d (days), h/H (hours), m, s and f...
 * (fractions of a second, up to nine); the largest unit in the picture
 * takes what does not fit the next one ("h:mm" of a day and a half is
 * "36:00").  No picture: LOCALE_SDURATION. */
static int duration_format(int idx, DWORD flags, const SYSTEMTIME *d, ULONGLONG ticks, LPCWSTR fmt, Out *o)
{
    static const ULONGLONG unit[4] = { 864000000000ull, 36000000000ull, 600000000ull, 10000000ull };
    if (idx == LOC_NONE) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if ((flags & ~LOCALE_NOUSEROVERRIDE) || (fmt && flags)) { SetLastError(ERROR_INVALID_FLAGS); return 0; }
    if (d) {
        if (!time_ok(d)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        ticks = ((((ULONGLONG)d->wHour * 60 + d->wMinute) * 60 + d->wSecond) * 1000 + d->wMilliseconds) * 10000;
    }
    WCHAR pic[CAP];
    if (!fmt) { info(idx, 0x5D /* LOCALE_SDURATION */ | flags, pic); fmt = pic; }
    Pic p;
    split(fmt, DURATION_FIELDS, &p);
    BOOL used[4] = { FALSE };
    for (int i = 0; i < p.n; i++) {
        WCHAR c = p.it[i].c;
        if (c == 'd') used[0] = TRUE;
        else if (c == 'h' || c == 'H') used[1] = TRUE;
        else if (c == 'm') used[2] = TRUE;
        else if (c == 's') used[3] = TRUE;
    }
    ULONGLONG v[4] = { 0 }, rest = ticks;
    for (int u = 0; u < 4; u++) if (used[u]) { v[u] = rest / unit[u]; rest %= unit[u]; }
    unsigned frac = (unsigned)(ticks % 10000000ull);         /* seven digits */
    o->n = 0;
    o->over = FALSE;
    for (int i = 0; i < p.n; i++) {
        const Item *it = &p.it[i];
        int r = it->run;
        switch (it->c) {
        case 0: for (int k = 0; k < it->len; k++) add_c(o, p.text[it->at + k]); break;
        case 'd': add_num(o, v[0], r); break;
        case 'h': case 'H': add_num(o, v[1], r > 1 ? 2 : 1); break;
        case 'm': add_num(o, v[2], r > 1 ? 2 : 1); break;
        case 's': add_num(o, v[3], r > 1 ? 2 : 1); break;
        case 'f': {
            if (r > 9) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            WCHAR t[10];
            unsigned f = frac;
            for (int k = 6; k >= 0; k--) { t[k] = (WCHAR)('0' + f % 10); f /= 10; }
            t[7] = t[8] = '0';
            for (int k = 0; k < r; k++) add_c(o, t[k]);
            break;
        }
        }
    }
    o->s[o->n] = 0;
    return 1;
}

K32 int WINAPI GetDurationFormatEx(LPCWSTR loc, DWORD flags, const SYSTEMTIME *d, ULONGLONG ticks, LPCWSTR fmt,
                                   LPWSTR out, int cap)
{
    Out o;
    return duration_format(nls_name(loc), flags, d, ticks, fmt, &o) ? give(&o, out, cap) : 0;
}
K32 int WINAPI GetDurationFormat(LCID lcid, DWORD flags, const SYSTEMTIME *d, ULONGLONG ticks, LPCWSTR fmt,
                                 LPWSTR out, int cap)
{
    Out o;
    return duration_format(nls_lcid(lcid), flags, d, ticks, fmt, &o) ? give(&o, out, cap) : 0;
}
