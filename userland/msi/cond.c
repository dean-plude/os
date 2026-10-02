/*
 * cond.c — Windows Installer conditional statements
 *
 *   expr   := term ( OR | XOR | EQV | IMP term )*
 *   term   := factor ( AND factor )*
 *   factor := NOT factor | '(' expr ')' | value [ op value ]
 *   value  := property | %ENVIRONMENT | "string" | number
 *             | &Feature | !Feature | $Component | ?Component  (action and
 *               installed states, which the property lookup answers)
 *   op     := = <> > >= < <= >< << >>  (a ~ prefix compares case-insensitively)
 *
 * A property on its own is true when it is set (non-empty); comparing
 * two numbers is numeric, anything else compares as strings.
 */
#include "msi_int.h"
#include <ctype.h>

typedef struct {
    const char *p;
    MsiPropFn   prop;
    void       *ctx;
    bool        error;
} Cond;

typedef struct {
    char *s;                /* string value (malloc'd) or NULL */
    long  n;
    bool  is_num;
} Val;

static void skip_ws(Cond *c) { while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n') c->p++; }

static bool word(Cond *c, const char *w)
{
    size_t n = strlen(w);
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)c->p[i]) != w[i]) return false;
    char e = c->p[n];
    if (isalnum((unsigned char)e) || e == '_' || e == '.') return false;
    c->p += n;
    return true;
}

static bool parse_expr(Cond *c);

static void free_val(Val *v) { free(v->s); v->s = NULL; }

static Val parse_value(Cond *c)
{
    Val v = { NULL, 0, false };
    skip_ws(c);
    if (*c->p == '"') {
        const char *s = ++c->p;
        while (*c->p && *c->p != '"') c->p++;
        v.s = malloc((size_t)(c->p - s) + 1);
        if (v.s) { memcpy(v.s, s, (size_t)(c->p - s)); v.s[c->p - s] = '\0'; }
        if (*c->p == '"') c->p++;
    } else if (isdigit((unsigned char)*c->p) || (*c->p == '-' && isdigit((unsigned char)c->p[1]))) {
        v.n = strtol(c->p, (char **)&c->p, 10);
        v.is_num = true;
    } else if (*c->p == '%' || *c->p == '&' || *c->p == '!' || *c->p == '$' || *c->p == '?') {
        char sigil = *c->p++;
        const char *s = c->p;
        while (isalnum((unsigned char)*c->p) || *c->p == '_' || *c->p == '.') c->p++;
        char name[128];
        size_t n = (size_t)(c->p - s) < sizeof(name) - 1 ? (size_t)(c->p - s) : sizeof(name) - 1;
        memcpy(name, s, n);
        name[n] = '\0';
        char env[136];
        snprintf(env, sizeof(env), "%c%s", sigil, name);
        const char *r = c->prop(c->ctx, env);
        if (sigil != '%' && r && *r) { v.n = strtol(r, NULL, 10); v.is_num = true; }
        else v.s = strdup(r ? r : "");
    } else if (isalpha((unsigned char)*c->p) || *c->p == '_') {
        const char *s = c->p;
        while (isalnum((unsigned char)*c->p) || *c->p == '_' || *c->p == '.') c->p++;
        char name[128];
        size_t n = (size_t)(c->p - s) < sizeof(name) - 1 ? (size_t)(c->p - s) : sizeof(name) - 1;
        memcpy(name, s, n);
        name[n] = '\0';
        const char *r = c->prop(c->ctx, name);
        v.s = strdup(r ? r : "");
    } else {
        c->error = true;
        v.s = strdup("");
    }
    return v;
}

static bool as_num(const Val *v, long *n)
{
    if (v->is_num) { *n = v->n; return true; }
    if (!v->s || !*v->s) return false;
    char *end;
    long r = strtol(v->s, &end, 10);
    if (*end) return false;
    *n = r;
    return true;
}

static int str_cmp(const char *a, const char *b, bool nocase)
{
    if (!nocase) return strcmp(a, b);
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static bool str_find(const char *hay, const char *needle, bool nocase)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (nocase ? tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])
                                        : p[i] == needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static bool parse_factor(Cond *c)
{
    skip_ws(c);
    if (word(c, "NOT")) return !parse_factor(c);
    if (*c->p == '(') {
        c->p++;
        bool r = parse_expr(c);
        skip_ws(c);
        if (*c->p == ')') c->p++; else c->error = true;
        return r;
    }
    Val a = parse_value(c);
    skip_ws(c);
    bool nocase = false;
    if (*c->p == '~') { nocase = true; c->p++; }
    char op[3] = { 0, 0, 0 };
    if (c->p[0] == '=' ) { op[0] = '='; c->p++; }
    else if (c->p[0] == '<' && c->p[1] == '>') { op[0] = '<'; op[1] = '>'; c->p += 2; }
    else if (c->p[0] == '<' && c->p[1] == '=') { op[0] = '<'; op[1] = '='; c->p += 2; }
    else if (c->p[0] == '<' && c->p[1] == '<') { op[0] = '<'; op[1] = '<'; c->p += 2; }
    else if (c->p[0] == '>' && c->p[1] == '=') { op[0] = '>'; op[1] = '='; c->p += 2; }
    else if (c->p[0] == '>' && c->p[1] == '>') { op[0] = '>'; op[1] = '>'; c->p += 2; }
    else if (c->p[0] == '>' && c->p[1] == '<') { op[0] = '>'; op[1] = '<'; c->p += 2; }
    else if (c->p[0] == '<') { op[0] = '<'; c->p++; }
    else if (c->p[0] == '>') { op[0] = '>'; c->p++; }
    if (!op[0]) {
        /* a value alone: set (non-empty) or non-zero */
        bool r = a.is_num ? a.n != 0 : (a.s && a.s[0]);
        free_val(&a);
        return r;
    }
    Val b = parse_value(c);
    long na, nb;
    bool r = false;
    bool substr = op[1] == '<' || op[1] == '>' || (op[0] == '>' && op[1] == '<');
    if (!substr && as_num(&a, &na) && as_num(&b, &nb)) {
        if (!strcmp(op, "="))       r = na == nb;
        else if (!strcmp(op, "<>")) r = na != nb;
        else if (!strcmp(op, ">"))  r = na > nb;
        else if (!strcmp(op, ">=")) r = na >= nb;
        else if (!strcmp(op, "<"))  r = na < nb;
        else if (!strcmp(op, "<=")) r = na <= nb;
        free_val(&a);
        free_val(&b);
        return r;
    }
    char nbuf[2][24];
    const char *sa = a.s ? a.s : (snprintf(nbuf[0], 24, "%ld", a.n), nbuf[0]);
    const char *sb = b.s ? b.s : (snprintf(nbuf[1], 24, "%ld", b.n), nbuf[1]);
    int d = str_cmp(sa, sb, nocase);
    if (!strcmp(op, "="))       r = d == 0;
    else if (!strcmp(op, "<>")) r = d != 0;
    else if (!strcmp(op, ">"))  r = d > 0;
    else if (!strcmp(op, ">=")) r = d >= 0;
    else if (!strcmp(op, "<"))  r = d < 0;
    else if (!strcmp(op, "<=")) r = d <= 0;
    else if (!strcmp(op, "><")) r = str_find(sa, sb, nocase);
    else if (!strcmp(op, "<<")) {         /* starts with */
        size_t n = strlen(sb);
        r = strlen(sa) >= n;
        for (size_t i = 0; r && i < n; i++)
            r = nocase ? tolower((unsigned char)sa[i]) == tolower((unsigned char)sb[i]) : sa[i] == sb[i];
    } else if (!strcmp(op, ">>")) {       /* ends with */
        size_t n = strlen(sb), m = strlen(sa);
        r = m >= n && !str_cmp(sa + m - n, sb, nocase);
    }
    free_val(&a);
    free_val(&b);
    return r;
}

static bool parse_term(Cond *c)
{
    bool r = parse_factor(c);
    for (;;) {
        skip_ws(c);
        if (word(c, "AND")) { bool s = parse_factor(c); r = r && s; }
        else return r;
    }
}

static bool parse_expr(Cond *c)
{
    bool r = parse_term(c);
    for (;;) {
        skip_ws(c);
        if (word(c, "OR"))       { bool s = parse_term(c); r = r || s; }
        else if (word(c, "XOR")) { bool s = parse_term(c); r = r != s; }
        else if (word(c, "EQV")) { bool s = parse_term(c); r = r == s; }
        else if (word(c, "IMP")) { bool s = parse_term(c); r = !r || s; }
        else return r;
    }
}

bool msi_condition(const char *cond, MsiPropFn prop, void *ctx)
{
    if (!cond) return true;
    Cond c = { cond, prop, ctx, false };
    skip_ws(&c);
    if (!*c.p) return true;
    bool r = parse_expr(&c);
    skip_ws(&c);
    if (*c.p) c.error = true;
    return c.error ? false : r;
}
