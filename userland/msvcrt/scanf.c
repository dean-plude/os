/* msvcrt: the scanf family */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

typedef struct {
    int (*get)(void *ctx);
    void (*unget)(int c, void *ctx);
    void *ctx;
    int count;                         /* characters consumed (%n) */
} Src;

static int  sget(Src *s)          { int c = s->get(s->ctx); if (c != EOF) s->count++; return c; }
static void sunget(Src *s, int c) { if (c != EOF) { s->unget(c, s->ctx); s->count--; } }

static int core(Src *src, const char *f, va_list ap)
{
    int assigned = 0, any = 0;
    for (; *f; f++) {
        if (isspace((unsigned char)*f)) {
            int c;
            while ((c = sget(src)) != EOF && isspace(c)) {}
            sunget(src, c);
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') f++;
            int c = sget(src);
            if (c != (unsigned char)*f) { sunget(src, c); return c == EOF && !any ? EOF : assigned; }
            continue;
        }
        f++;
        int skip = 0, width = 0, len = 0;
        if (*f == '*') { skip = 1; f++; }
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        for (;;) {
            if (*f == 'h') { len = len == -1 ? -2 : -1; f++; }
            else if (*f == 'l') { len++; f++; }
            else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'L') { len = *f == 'L' ? 3 : 2; f++; }
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { len = 2; f += 3; }
            else break;
        }
        char conv = *f;
        if (!conv) break;
        if (conv == 'n') { if (!skip) *va_arg(ap, int *) = src->count; continue; }
        int c;
        if (conv != 'c' && conv != '[') {
            while ((c = sget(src)) != EOF && isspace(c)) {}
            sunget(src, c);
        }
        if (!width) width = conv == 'c' ? 1 : 0x7FFFFFFF;
        char buf[512];
        int n = 0;
        if (conv == 'c') {
            char *d = skip ? 0 : va_arg(ap, char *);
            for (; n < width; n++) {
                if ((c = sget(src)) == EOF) break;
                if (d) d[n] = (char)c;
            }
            if (!n) return any ? assigned : EOF;
            any = 1;
            if (!skip) assigned++;
            continue;
        }
        if (conv == 's' || conv == '[') {
            int set[256] = { 0 }, invert = 0;
            if (conv == '[') {
                f++;
                if (*f == '^') { invert = 1; f++; }
                if (*f == ']') { set[']'] = 1; f++; }
                for (; *f && *f != ']'; f++) {
                    if (f[1] == '-' && f[2] && f[2] != ']') { for (int x = (unsigned char)f[0]; x <= (unsigned char)f[2]; x++) set[x] = 1; f += 2; }
                    else set[(unsigned char)*f] = 1;
                }
            }
            char *d = skip ? 0 : va_arg(ap, char *);
            while (n < width) {
                c = sget(src);
                if (c == EOF) break;
                int ok = conv == 's' ? !isspace(c) : set[c] != invert;
                if (!ok) { sunget(src, c); break; }
                if (d) d[n] = (char)c;
                n++;
            }
            if (!n) return any ? assigned : (c == EOF ? EOF : assigned);
            if (d) d[n] = 0;
            any = 1;
            if (!skip) assigned++;
            continue;
        }
        /* numbers: collect the candidate characters, then convert */
        int isfloat = conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a';
        while (n < width && n < (int)sizeof(buf) - 1) {
            c = sget(src);
            if (c == EOF) break;
            int ok = isalnum(c) || c == '+' || c == '-' || (isfloat && c == '.');
            if (ok && (c == '+' || c == '-') && n && !(isfloat && (buf[n - 1] | 0x20) == 'e')) ok = 0;
            if (ok && !isfloat && !isxdigit(c) && !((c | 0x20) == 'x' && n <= 2) && c != '+' && c != '-') ok = 0;
            if (ok && isfloat && isalpha(c) && (c | 0x20) != 'e' && !strchr("infatyINFATY", c)) ok = 0;
            if (!ok) { sunget(src, c); break; }
            buf[n++] = (char)c;
        }
        buf[n] = 0;
        if (!n) return any ? assigned : (c == EOF ? EOF : assigned);
        char *end;
        if (isfloat) {
            double v = strtod(buf, &end);
            if (end == buf) return assigned;
            if (!skip) { if (len >= 1) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = (float)v; }
        } else {
            int base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            unsigned long long v = (conv == 'd' || conv == 'i') ? (unsigned long long)strtoll(buf, &end, base) : strtoull(buf, &end, base);
            if (end == buf) return assigned;
            for (char *e = buf + n; e > end; ) sunget(src, (unsigned char)*--e);
            if (!skip) {
                if (conv == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                else if (len >= 2) *va_arg(ap, long long *) = (long long)v;
                else if (len == 1) *va_arg(ap, long *) = (long)v;
                else if (len == -1) *va_arg(ap, short *) = (short)v;
                else if (len == -2) *va_arg(ap, char *) = (char)v;
                else *va_arg(ap, int *) = (int)v;
            }
        }
        any = 1;
        if (!skip) assigned++;
    }
    return assigned;
}

typedef struct { const char *s; } StrSrc;
static int  str_get(void *ctx)          { StrSrc *p = ctx; return *p->s ? (unsigned char)*p->s++ : EOF; }
static void str_unget(int c, void *ctx) { (void)c; ((StrSrc *)ctx)->s--; }
static int  file_get(void *ctx)          { return fgetc((FILE *)ctx); }
static void file_unget(int c, void *ctx) { ungetc(c, (FILE *)ctx); }

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    StrSrc ss = { s };
    Src src = { str_get, str_unget, &ss, 0 };
    return core(&src, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    Src src = { file_get, file_unget, f, 0 };
    return core(&src, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vsscanf(s, fmt, a); va_end(a); return r; }
int fscanf(FILE *f, const char *fmt, ...)       { va_list a; va_start(a, fmt); int r = vfscanf(f, fmt, a); va_end(a); return r; }
int scanf(const char *fmt, ...)                 { va_list a; va_start(a, fmt); int r = vfscanf(stdin, fmt, a); va_end(a); return r; }
