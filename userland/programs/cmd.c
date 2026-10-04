/*
 * cmd.exe — the Windows command interpreter
 *
 *   cmd [/c | /k] [/q] [/d] [/s] [/v:on|off] [/e:on] [command]
 *
 * A command line is expanded (%VAR%, and %0-%9 / %* / %~dp0 in batch
 * files), parsed into commands joined by & && || and |, with ( ) blocks,
 * IF, FOR and redirections (< > >> 2> 2>&1 >nul), then run.  Internal
 * commands write to the handles their redirections give them; programs
 * get them as standard handles.  Each side of a pipe that is not a
 * program runs in a child cmd.exe, as on Windows.  Batch files (.bat,
 * .cmd) run here: labels, GOTO, CALL (files and :labels), SHIFT,
 * SETLOCAL/ENDLOCAL, delayed expansion (!VAR!), ECHO ON/OFF.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define LINE_MAX_ 8192

typedef struct { HANDLE in, out, err; } Io;

/* -----------------------------------------------------------------------
 * Output
 * ----------------------------------------------------------------------- */
static void wr(HANDLE h, const char *s, size_t n)
{
    DWORD w;
    if (n) WriteFile(h, s, (DWORD)n, &w, 0);
}

/* Text with "\n" written as "\r\n" (as cmd writes) */
static void puts_h(HANDLE h, const char *s)
{
    const char *start = s;
    for (; *s; s++) {
        if (*s == '\n' && (s == start || s[-1] != '\r')) {
            wr(h, start, (size_t)(s - start));
            wr(h, "\r\n", 2);
            start = s + 1;
        }
    }
    wr(h, start, (size_t)(s - start));
}

static void printf_h(HANDLE h, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    puts_h(h, buf);
}

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
typedef struct Batch Batch;
struct Batch {
    Batch  *parent;
    char   *path;               /* full path of the file */
    char  **lines;
    int     nlines, pc;
    char   *args[64];           /* %0 .. */
    int     nargs, shift;
    int     locals;             /* SETLOCAL depth at entry */
    int     is_call_label;      /* CALL :label (shares the file) */
};

typedef struct Local { struct Local *next; char *env; char cwd[MAX_PATH]; int delayed, ext; } Local;

static int     g_errorlevel;
static int     g_echo = 1;
static int     g_delayed;               /* !VAR! */
static Batch  *g_batch;
static Local  *g_locals;
static int     g_nlocals;
static int     g_jump;                  /* GOTO / EXIT /B: leave the current line */
static int     g_exit;                  /* EXIT: leave cmd.exe */
static int     g_exit_code;
static int     g_interactive;
static char   *g_cmdline;               /* %CMDCMDLINE% */
static char    g_pushd[32][MAX_PATH];
static int     g_npushd;
static char    g_self[MAX_PATH];

static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { puts_h(GetStdHandle(STD_ERROR_HANDLE), "Out of memory.\n"); ExitProcess(1); } return p; }
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *d = xmalloc(n); memcpy(d, s, n); return d; }
static char *xstrndup(const char *s, size_t n) { char *d = xmalloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }

static int ieq(const char *a, const char *b) { return !_stricmp(a, b); }
static int iprefix(const char *s, const char *p) { return !_strnicmp(s, p, strlen(p)); }

/* A growing string */
typedef struct { char *s; size_t n, cap; } Str;
static void sput(Str *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->n + n + 1 > cap) cap *= 2;
        char *d = xmalloc(cap);
        if (b->s) { memcpy(d, b->s, b->n); free(b->s); }
        b->s = d;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void sputs(Str *b, const char *s) { sput(b, s, strlen(s)); }
static void sputc(Str *b, char c) { sput(b, &c, 1); }
static char *sdone(Str *b) { if (!b->s) sput(b, "", 0); return b->s; }

static void cwd(char *out) { if (!GetCurrentDirectoryA(MAX_PATH, out)) strcpy(out, "C:\\"); }

static void set_error(int e) { g_errorlevel = e; }

/* -----------------------------------------------------------------------
 * Variables
 * ----------------------------------------------------------------------- */
/* A variable's value (malloc'd), dynamic ones included; NULL if unset */
static char *getvar(const char *name)
{
    DWORD n = GetEnvironmentVariableA(name, 0, 0);
    if (n) {
        char *v = xmalloc(n + 1);
        GetEnvironmentVariableA(name, v, n + 1);
        return v;
    }
    char buf[MAX_PATH + 32];
    if (ieq(name, "CD")) { cwd(buf); return xstrdup(buf); }
    if (ieq(name, "ERRORLEVEL")) { sprintf(buf, "%d", g_errorlevel); return xstrdup(buf); }
    if (ieq(name, "RANDOM")) { sprintf(buf, "%d", rand() & 0x7FFF); return xstrdup(buf); }
    if (ieq(name, "CMDEXTVERSION")) return xstrdup("2");
    if (ieq(name, "CMDCMDLINE")) return xstrdup(g_cmdline ? g_cmdline : "cmd.exe");
    if (ieq(name, "DATE") || ieq(name, "TIME")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        if (ieq(name, "DATE")) sprintf(buf, "%s %02d/%02d/%04d", days[t.wDayOfWeek % 7], t.wMonth, t.wDay, t.wYear);
        else sprintf(buf, "%2d:%02d:%02d.%02d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds / 10);
        return xstrdup(buf);
    }
    if (ieq(name, "__CD__")) { cwd(buf); size_t l = strlen(buf); if (buf[l - 1] != '\\') strcat(buf, "\\"); return xstrdup(buf); }
    return NULL;
}

/* %VAR:~start,len% and %VAR:old=new% applied to @v */
static char *var_edit(const char *v, const char *spec)
{
    int n = (int)strlen(v);
    if (spec[0] == '~') {
        char *e;
        long start = strtol(spec + 1, &e, 0), len = n;
        int has_len = 0;
        if (*e == ',') { len = strtol(e + 1, &e, 0); has_len = 1; }
        if (start < 0) start += n;
        if (start < 0) start = 0;
        if (start > n) start = n;
        if (has_len && len < 0) len = n - start + len;
        if (len < 0) len = 0;
        if (start + len > n) len = n - start;
        return xstrndup(v + start, (size_t)len);
    }
    const char *eq = strchr(spec, '=');
    if (!eq) return xstrdup(v);
    int star = spec[0] == '*';
    char *old = xstrndup(spec + star, (size_t)(eq - spec - star));
    const char *rep = eq + 1;
    size_t ol = strlen(old);
    Str b = { 0 };
    if (!ol) { free(old); return xstrdup(v); }
    const char *p = v;
    if (star) {                                     /* *old=new: everything up to the first match */
        for (const char *q = v; *q; q++)
            if (!_strnicmp(q, old, ol)) { sputs(&b, rep); sputs(&b, q + ol); free(old); return sdone(&b); }
        free(old);
        return xstrdup(v);
    }
    while (*p) {
        if (!_strnicmp(p, old, ol)) { sputs(&b, rep); p += ol; }
        else sputc(&b, *p++);
    }
    free(old);
    return sdone(&b);
}

/* -----------------------------------------------------------------------
 * %~ modifiers: f d p n x s a t z (on a path) and ~ (quotes off)
 * ----------------------------------------------------------------------- */
static char *strip_quotes(const char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') return xstrndup(s + 1, n - 2);
    if (n >= 1 && s[0] == '"') return xstrdup(s + 1);
    return xstrdup(s);
}

static char *apply_mods(const char *mods, const char *value)
{
    char *v = strip_quotes(value);
    if (!*mods || !*v) return v;
    char full[MAX_PATH];
    if (!GetFullPathNameA(v, MAX_PATH, full, 0)) strcpy(full, v);
    int f = 0, d = 0, p = 0, n = 0, x = 0, a = 0, t = 0, z = 0;
    for (const char *m = mods; *m; m++) {
        switch (tolower((unsigned char)*m)) {
        case 'f': f = 1; break; case 'd': d = 1; break; case 'p': p = 1; break;
        case 'n': n = 1; break; case 'x': x = 1; break; case 'a': a = 1; break;
        case 't': t = 1; break; case 'z': z = 1; break; case 's': break;
        }
    }
    if (f) d = p = n = x = 1;
    Str b = { 0 };
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int have = GetFileAttributesExA(full, GetFileExInfoStandard, &fa);
    if (a && have) {
        char at[10] = "---------";
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) at[0] = 'd';
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_READONLY) at[1] = 'r';
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) at[2] = 'a';
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) at[3] = 'h';
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) at[4] = 's';
        sputs(&b, at);
    }
    if (t && have) {
        FILETIME lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        char tb[40];
        sprintf(tb, "%s%02d/%02d/%04d %02d:%02d %s", b.n ? " " : "", st.wMonth, st.wDay, st.wYear,
                st.wHour % 12 ? st.wHour % 12 : 12, st.wMinute, st.wHour < 12 ? "AM" : "PM");
        sputs(&b, tb);
    }
    if (z && have) {
        char zb[32];
        sprintf(zb, "%s%llu", b.n ? " " : "", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        sputs(&b, zb);
    }
    if (d || p || n || x) {
        if (b.n) sputc(&b, ' ');
        char *slash = strrchr(full, '\\');
        char *name = slash ? slash + 1 : full;
        char *dot = strrchr(name, '.');
        if (!dot) dot = name + strlen(name);
        if (d) sput(&b, full, 2);
        if (p) sput(&b, full + 2, (size_t)(name - full - 2));
        if (n) sput(&b, name, (size_t)(dot - name));
        if (x) sputs(&b, dot);
    }
    free(v);
    return sdone(&b);
}

/* Parse "~mods" + a terminator at @s (after '%'): the modifier letters and
 * the character they apply to; 0 if not a modifier form */
static int parse_mods(const char *s, char *mods, int cap, char *target, int (*ok)(char, void *), void *ctx)
{
    if (*s != '~') return 0;
    int i = 1, k = 0;
    /* the longest run of modifier letters whose next character is a target */
    int best = -1;
    while (1) {
        if (s[i] && ok(s[i], ctx)) best = i;
        if (!s[i] || !strchr("fdpnxsatzFDPNXSATZ", s[i]) || i > 16) break;
        i++;
    }
    if (best < 0) return 0;
    for (k = 0; k < best - 1 && k < cap - 1; k++) mods[k] = s[1 + k];
    mods[k] = 0;
    *target = s[best];
    return best + 1;
}

static int is_digit_or_star(char c, void *ctx) { (void)ctx; return (c >= '0' && c <= '9'); }

static const char *batch_arg(int i)
{
    if (!g_batch) return "";
    int k = i + g_batch->shift;                         /* SHIFT moves %0 too */
    return k < g_batch->nargs ? g_batch->args[k] : "";
}

/* %* : the batch arguments from %1 on */
static char *batch_all(void)
{
    Str b = { 0 };
    for (int i = 1; g_batch && i + g_batch->shift < g_batch->nargs; i++) {
        if (b.n) sputc(&b, ' ');
        sputs(&b, g_batch->args[i + g_batch->shift]);
    }
    return sdone(&b);
}

/* -----------------------------------------------------------------------
 * Expansion
 * ----------------------------------------------------------------------- */
static char *expand_percent(const char *s)
{
    Str b = { 0 };
    int batch = g_batch != NULL;
    while (*s) {
        if (*s != '%') { sputc(&b, *s++); continue; }
        if (batch && s[1] == '%') { sputc(&b, '%'); s += 2; continue; }
        if (batch && s[1] >= '0' && s[1] <= '9') { sputs(&b, batch_arg(s[1] - '0')); s += 2; continue; }
        if (batch && s[1] == '*') { char *a = batch_all(); sputs(&b, a); free(a); s += 2; continue; }
        if (batch && s[1] == '~') {
            char mods[20], t;
            int n = parse_mods(s + 1, mods, sizeof(mods), &t, is_digit_or_star, 0);
            if (n) {
                char *v = apply_mods(mods, batch_arg(t - '0'));
                sputs(&b, v);
                free(v);
                s += 1 + n;
                continue;
            }
        }
        const char *end = strchr(s + 1, '%');
        if (!end) { if (!batch) sputc(&b, '%'); s++; continue; }
        char *name = xstrndup(s + 1, (size_t)(end - s - 1));
        if (!*name || strchr(name, '\n')) {                  /* not a variable */
            free(name);
            if (!batch) sputc(&b, '%');
            s++;
            continue;
        }
        char *colon = strchr(name, ':');
        if (colon) *colon = 0;
        char *v = getvar(name);
        if (v && colon) { char *e = var_edit(v, colon + 1); free(v); v = e; }
        if (v) sputs(&b, v);
        else if (!batch) { if (colon) *colon = ':'; sputc(&b, '%'); sputs(&b, name); sputc(&b, '%'); }
        free(v);
        free(name);
        s = end + 1;
    }
    return sdone(&b);
}

static char *expand_delayed(const char *s)
{
    if (!g_delayed || !strchr(s, '!')) return xstrdup(s);
    Str b = { 0 };
    while (*s) {
        if (*s == '^' && s[1] == '!') { sputc(&b, '!'); s += 2; continue; }
        if (*s != '!') { sputc(&b, *s++); continue; }
        const char *end = strchr(s + 1, '!');
        if (!end) { s++; continue; }
        char *name = xstrndup(s + 1, (size_t)(end - s - 1));
        char *colon = strchr(name, ':');
        if (colon) *colon = 0;
        char *v = getvar(name);
        if (v && colon) { char *e = var_edit(v, colon + 1); free(v); v = e; }
        if (v) sputs(&b, v);
        free(v);
        free(name);
        s = end + 1;
    }
    return sdone(&b);
}

/* -----------------------------------------------------------------------
 * Parsing
 * ----------------------------------------------------------------------- */
typedef enum { N_CMD, N_BLOCK, N_SEQ, N_AND, N_OR, N_PIPE, N_IF, N_FOR, N_REM } Kind;

typedef struct Redir { struct Redir *next; int fd; char op; int dup; char *target; } Redir;
/* op: '<' input, '>' output, 'a' append, '&' duplicate @dup */

typedef struct Node {
    Kind kind;
    struct Node *a, *b;
    char *text;                 /* N_CMD: the command without redirections */
    char *src;                  /* the source text of this node (pipes run it in a child) */
    Redir *redir;
    int quiet;                  /* '@' */
    /* IF */
    int neg, icase, cond;       /* cond: 0 compare, 1 errorlevel, 2 exist, 3 defined, 4 cmdextversion */
    char *l, *op, *r;
    /* FOR */
    char var, mode;             /* mode: 0, 'D', 'R', 'L', 'F' */
    char *opts, *set, *root, *body;
} Node;

typedef struct { const char *s; int i, depth, err, nl_stop; } Parser;

static Node *node(Kind k) { Node *n = xmalloc(sizeof(Node)); memset(n, 0, sizeof(*n)); n->kind = k; return n; }

static void free_redir(Redir *r) { while (r) { Redir *n = r->next; free(r->target); free(r); r = n; } }
static void free_node(Node *n)
{
    if (!n) return;
    free_node(n->a); free_node(n->b);
    free(n->text); free(n->src); free_redir(n->redir);
    free(n->l); free(n->op); free(n->r);
    free(n->opts); free(n->set); free(n->root); free(n->body);
    free(n);
}

static void skip_ws(Parser *p) { while (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == ',' || p->s[p->i] == ';' || p->s[p->i] == '=') p->i++; }
static void skip_sp(Parser *p) { while (p->s[p->i] == ' ' || p->s[p->i] == '\t') p->i++; }

static int word_at(Parser *p, const char *w)
{
    size_t n = strlen(w);
    if (_strnicmp(p->s + p->i, w, n)) return 0;
    char c = p->s[p->i + n];
    return !c || c == ' ' || c == '\t' || c == '(' || c == '\n' || c == '&' || c == '|' || c == ',' || c == ';' || c == '=';
}

static Node *parse_seq(Parser *p);

static int ends_cmd(Parser *p, char c)
{
    return !c || c == '&' || c == '|' || c == '\n' || (c == ')' && p->depth > 0);
}

/* A redirection target: a word (quotes kept off) */
static char *redir_target(Parser *p)
{
    skip_sp(p);
    Str b = { 0 };
    int q = 0;
    for (;;) {
        char c = p->s[p->i];
        if (!c || c == '\n') break;
        if (!q && (c == ' ' || c == '\t' || c == '&' || c == '|' || c == '<' || c == '>' || (c == ')' && p->depth > 0))) break;
        if (c == '^' && !q && p->s[p->i + 1]) { sputc(&b, p->s[p->i + 1]); p->i += 2; continue; }
        if (c == '"') { q = !q; p->i++; continue; }
        sputc(&b, c);
        p->i++;
    }
    return sdone(&b);
}

/* Try a redirection at the current position (@fd: a digit just before) */
static int parse_redir(Parser *p, Node *n, int fd)
{
    char c = p->s[p->i];
    if (c != '<' && c != '>') return 0;
    Redir *r = xmalloc(sizeof(Redir));
    memset(r, 0, sizeof(*r));
    r->fd = fd >= 0 ? fd : (c == '<' ? 0 : 1);
    p->i++;
    if (c == '>' && p->s[p->i] == '>') { r->op = 'a'; p->i++; }
    else r->op = c;
    if (p->s[p->i] == '&' && p->s[p->i + 1] >= '0' && p->s[p->i + 1] <= '9') {
        r->op = '&';
        r->dup = p->s[p->i + 1] - '0';
        p->i += 2;
    } else r->target = redir_target(p);
    Redir **pp = &n->redir;
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    return 1;
}

/* Redirections after a block or command: "(...) > file" */
static void parse_trailing_redirs(Parser *p, Node *n)
{
    for (;;) {
        skip_sp(p);
        char c = p->s[p->i];
        int fd = -1;
        if (c >= '0' && c <= '9' && (p->s[p->i + 1] == '>' || p->s[p->i + 1] == '<')) { fd = c - '0'; p->i++; c = p->s[p->i]; }
        if (!parse_redir(p, n, fd)) { if (fd >= 0) p->i--; return; }
    }
}

static Node *parse_simple(Parser *p)
{
    Node *n = node(N_CMD);
    Str b = { 0 };
    int q = 0;
    for (;;) {
        char c = p->s[p->i];
        if (!c) break;
        if (!q && ends_cmd(p, c)) break;
        if (c == '\n') break;
        if (c == '"') { q = !q; sputc(&b, c); p->i++; continue; }
        if (!q && c == '^') {
            if (p->s[p->i + 1] == '\n') { p->i += 2; continue; }        /* line continuation */
            if (p->s[p->i + 1]) { sputc(&b, p->s[p->i + 1]); p->i += 2; continue; }
            p->i++;
            continue;
        }
        if (!q && (c == '<' || c == '>')) {
            int fd = -1;
            if (b.n && b.s[b.n - 1] >= '0' && b.s[b.n - 1] <= '9' && (b.n == 1 || b.s[b.n - 2] == ' ' || b.s[b.n - 2] == '\t')) {
                fd = b.s[b.n - 1] - '0';
                b.n--;
                b.s[b.n] = 0;
            }
            parse_redir(p, n, fd);
            continue;
        }
        sputc(&b, c);
        p->i++;
    }
    n->text = sdone(&b);                                /* trailing spaces stay, as on Windows */
    return n;
}

/* An IF operand: a quoted string (quotes kept) or a word */
static char *if_word(Parser *p, int stop_at_eq)
{
    skip_sp(p);
    Str b = { 0 };
    int q = 0;
    for (;;) {
        char c = p->s[p->i];
        if (!c || c == '\n') break;
        if (!q && (c == ' ' || c == '\t')) break;
        if (!q && stop_at_eq && c == '=' && p->s[p->i + 1] == '=') break;
        if (!q && (c == '&' || c == '|' || (c == ')' && p->depth > 0) || c == '(')) break;
        if (c == '^' && !q && p->s[p->i + 1]) { sputc(&b, p->s[p->i + 1]); p->i += 2; continue; }
        if (c == '"') q = !q;
        sputc(&b, c);
        p->i++;
    }
    return sdone(&b);
}

static Node *parse_block(Parser *p)
{
    int start = p->i, nl_stop = p->nl_stop;
    p->i++;                                             /* '(' */
    p->depth++;
    p->nl_stop = 0;                                     /* lines inside a block go on */
    Node *n = node(N_BLOCK);
    n->a = parse_seq(p);
    p->nl_stop = nl_stop;
    skip_ws(p);
    while (p->s[p->i] == '\n') { p->i++; skip_ws(p); }
    if (p->s[p->i] == ')') p->i++;
    else p->err = 1;
    p->depth--;
    parse_trailing_redirs(p, n);
    n->src = xstrndup(p->s + start, (size_t)(p->i - start));
    return n;
}

/* The body of an IF or FOR: a block, or the rest of the command line
 * (the end of the line ends it, even inside a block) */
static Node *parse_body(Parser *p)
{
    skip_sp(p);
    if (p->s[p->i] == '(') return parse_block(p);
    p->nl_stop++;
    Node *n = parse_seq(p);
    p->nl_stop--;
    return n;
}

static Node *parse_primary(Parser *p);

static Node *parse_if(Parser *p)
{
    Node *n = node(N_IF);
    p->i += 2;
    skip_sp(p);
    if (word_at(p, "/i")) { n->icase = 1; p->i += 2; skip_sp(p); }
    if (word_at(p, "not")) { n->neg = 1; p->i += 3; skip_sp(p); }
    if (word_at(p, "errorlevel")) { p->i += 10; n->cond = 1; n->r = if_word(p, 0); }
    else if (word_at(p, "exist")) { p->i += 5; n->cond = 2; n->r = if_word(p, 0); }
    else if (word_at(p, "defined")) { p->i += 7; n->cond = 3; n->r = if_word(p, 0); }
    else if (word_at(p, "cmdextversion")) { p->i += 13; n->cond = 4; n->r = if_word(p, 0); }
    else {
        n->l = if_word(p, 1);
        skip_sp(p);
        if (p->s[p->i] == '=' && p->s[p->i + 1] == '=') {
            p->i += 2;
            while (p->s[p->i] == '=') p->i++;
            n->op = xstrdup("==");
        } else {
            n->op = if_word(p, 0);
            static const char *ops[] = { "EQU", "NEQ", "LSS", "LEQ", "GTR", "GEQ" };
            int ok = 0;
            for (int i = 0; i < 6; i++) if (ieq(n->op, ops[i])) ok = 1;
            if (!ok) p->err = 1;
        }
        n->r = if_word(p, 0);
    }
    n->a = parse_body(p);
    skip_sp(p);
    if (n->a && n->a->kind == N_BLOCK && word_at(p, "else")) {
        p->i += 4;
        n->b = parse_body(p);
    }
    return n;
}

static Node *parse_for(Parser *p)
{
    Node *n = node(N_FOR);
    p->i += 3;
    skip_sp(p);
    while (p->s[p->i] == '/') {
        char m = (char)toupper((unsigned char)p->s[p->i + 1]);
        p->i += 2;
        skip_sp(p);
        if (m == 'D' || m == 'L') n->mode = m;
        else if (m == 'R') {
            n->mode = 'R';
            if (p->s[p->i] != '%') n->root = if_word(p, 0);
            char *r = n->root ? strip_quotes(n->root) : 0;
            free(n->root);
            n->root = r;
        } else if (m == 'F') {
            n->mode = 'F';
            if (p->s[p->i] == '"') {
                int s = ++p->i;
                while (p->s[p->i] && p->s[p->i] != '"') p->i++;
                n->opts = xstrndup(p->s + s, (size_t)(p->i - s));
                if (p->s[p->i]) p->i++;
            } else if (p->s[p->i] != '%') n->opts = if_word(p, 0);
        }
        skip_sp(p);
    }
    if (p->s[p->i] != '%' || !p->s[p->i + 1]) { p->err = 1; return n; }
    n->var = p->s[p->i + 1];
    p->i += 2;
    skip_sp(p);
    if (!word_at(p, "in")) { p->err = 1; return n; }
    p->i += 2;
    skip_sp(p);
    if (p->s[p->i] != '(') { p->err = 1; return n; }
    int s = ++p->i, depth = 1, q = 0;
    Str set = { 0 };
    while (p->s[p->i]) {
        char c = p->s[p->i];
        if (c == '"') q = !q;
        if (!q && c == '(') depth++;
        if (!q && c == ')' && --depth == 0) break;
        sputc(&set, c == '\n' ? ' ' : c);
        p->i++;
    }
    (void)s;
    n->set = sdone(&set);
    if (p->s[p->i] == ')') p->i++;
    skip_sp(p);
    if (!word_at(p, "do")) { p->err = 1; return n; }
    p->i += 2;
    skip_sp(p);
    int bs = p->i;
    Node *tmp = parse_body(p);                         /* only for its extent */
    free_node(tmp);
    n->body = xstrndup(p->s + bs, (size_t)(p->i - bs));
    return n;
}

static Node *parse_primary(Parser *p)
{
    skip_ws(p);
    int quiet = 0;
    while (p->s[p->i] == '@') { quiet = 1; p->i++; skip_ws(p); }
    int start = p->i;
    Node *n;
    char c = p->s[p->i];
    if (c == '(') n = parse_block(p);
    else if (word_at(p, "if")) n = parse_if(p);
    else if (word_at(p, "for")) n = parse_for(p);
    else if (word_at(p, "rem") || (c == ':' && p->s[p->i + 1] == ':')) {
        n = node(N_REM);
        while (p->s[p->i] && p->s[p->i] != '\n') p->i++;
    } else if (c == ':' ) {                             /* a label on a command line */
        n = node(N_REM);
        while (p->s[p->i] && p->s[p->i] != '\n' && p->s[p->i] != '&') p->i++;
    } else n = parse_simple(p);
    n->quiet = quiet;
    if (!n->src) n->src = xstrndup(p->s + start, (size_t)(p->i - start));
    return n;
}

static Node *parse_pipe(Parser *p)
{
    Node *a = parse_primary(p);
    skip_sp(p);
    while (p->s[p->i] == '|' && p->s[p->i + 1] != '|') {
        p->i++;
        Node *n = node(N_PIPE);
        n->a = a;
        n->b = parse_primary(p);
        a = n;
        skip_sp(p);
    }
    return a;
}

static Node *parse_andor(Parser *p)
{
    Node *a = parse_pipe(p);
    for (;;) {
        skip_sp(p);
        const char *s = p->s + p->i;
        if (s[0] == '&' && s[1] == '&') { p->i += 2; Node *n = node(N_AND); n->a = a; n->b = parse_pipe(p); a = n; }
        else if (s[0] == '|' && s[1] == '|') { p->i += 2; Node *n = node(N_OR); n->a = a; n->b = parse_pipe(p); a = n; }
        else return a;
    }
}

static Node *parse_seq(Parser *p)
{
    Node *a = parse_andor(p);
    for (;;) {
        skip_sp(p);
        char c = p->s[p->i];
        if (c == '&' && p->s[p->i + 1] != '&') { p->i++; }
        else if (c == '\n' && p->depth > 0 && !p->nl_stop) { p->i++; skip_ws(p); if (p->s[p->i] == ')') return a; }
        else return a;
        skip_ws(p);
        while (p->s[p->i] == '\n' && p->depth > 0 && !p->nl_stop) { p->i++; skip_ws(p); }
        if (!p->s[p->i] || (p->s[p->i] == ')' && p->depth > 0) || p->s[p->i] == '\n') return a;
        Node *n = node(N_SEQ);
        n->a = a;
        n->b = parse_andor(p);
        a = n;
    }
}

static Node *parse_line(const char *s, int *err)
{
    Parser p = { s, 0, 0, 0, 0 };
    Node *n = parse_seq(&p);
    skip_ws(&p);
    if (p.s[p.i] == ')' ) p.err = 1;
    *err = p.err;
    return n;
}

/* -----------------------------------------------------------------------
 * Arguments
 * ----------------------------------------------------------------------- */
/* The next argument of @s (quotes removed), advancing *s; NULL at the end */
static char *next_arg(const char **s)
{
    const char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *s = p; return NULL; }
    Str b = { 0 };
    int q = 0;
    while (*p && (q || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') { q = !q; p++; continue; }
        sputc(&b, *p++);
    }
    *s = p;
    return sdone(&b);
}

/* Split into arguments (quotes removed); returns the count */
static int split_args(const char *s, char **av, int max)
{
    int n = 0;
    char *a;
    while (n < max && (a = next_arg(&s))) av[n++] = a;
    return n;
}
static void free_args(char **av, int n) { for (int i = 0; i < n; i++) free(av[i]); }

static int is_switch(const char *a, const char *sw) { return (a[0] == '/' || a[0] == '-') && ieq(a + 1, sw); }

/* -----------------------------------------------------------------------
 * Running programs
 * ----------------------------------------------------------------------- */
static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int has_ext(const char *name)
{
    const char *base = name;
    for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') base = c + 1;
    return strchr(base, '.') != NULL;
}

/* Find a program as cmd does: the current directory, then PATH, trying
 * each of PATHEXT when the name has no extension */
static int find_command(const char *name, char *out)
{
    char exts[256];
    DWORD n = GetEnvironmentVariableA("PATHEXT", exts, sizeof(exts));
    if (!n || n >= sizeof(exts)) strcpy(exts, ".COM;.EXE;.BAT;.CMD");
    int pathy = strchr(name, '\\') || strchr(name, '/') || strchr(name, ':');
    char dirs[4096];
    char cur[MAX_PATH];
    cwd(cur);
    if (pathy) strcpy(dirs, "");
    else {
        size_t k = (size_t)snprintf(dirs, sizeof(dirs), "%s;", cur);
        DWORD pl = GetEnvironmentVariableA("PATH", dirs + k, (DWORD)(sizeof(dirs) - k));
        if (!pl || pl >= sizeof(dirs) - k) dirs[k] = 0;
    }
    const char *d = dirs;
    for (int first = 1;; first = 0) {
        char dir[MAX_PATH];
        int dl = 0;
        if (pathy) { if (!first) break; }
        else {
            while (*d == ';') d++;
            if (!*d) break;
            int q = 0;
            while (*d && *d != ';' && dl < MAX_PATH - 1) { if (*d == '"') q = !q; else dir[dl++] = *d; d++; }
            (void)q;
        }
        dir[dl] = 0;
        char base[MAX_PATH * 2];
        if (pathy) snprintf(base, sizeof(base), "%s", name);
        else snprintf(base, sizeof(base), "%s%s%s", dir, dl && dir[dl - 1] != '\\' ? "\\" : "", name);
        if (has_ext(name) && file_exists(base)) { GetFullPathNameA(base, MAX_PATH, out, 0); return 1; }
        char e[256];
        strcpy(e, exts);
        for (char *t = strtok(e, ";"); t; t = strtok(0, ";")) {
            char cand[MAX_PATH * 2 + 8];
            snprintf(cand, sizeof(cand), "%s%s", base, t);
            if (file_exists(cand)) { GetFullPathNameA(cand, MAX_PATH, out, 0); return 1; }
        }
    }
    return 0;
}

static int is_batch(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (ieq(dot, ".bat") || ieq(dot, ".cmd"));
}

/* Start a program with the given standard handles; its process handle */
static HANDLE spawn(const char *image, const char *cmdline, Io *io)
{
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = io->in;
    si.hStdOutput = io->out;
    si.hStdError = io->err;
    PROCESS_INFORMATION pi;
    char *cl = xstrdup(cmdline);
    BOOL ok = CreateProcessA(image, cl, 0, 0, FALSE, 0, 0, 0, &si, &pi);
    free(cl);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int wait_exit(HANDLE proc)
{
    DWORD code = 1;
    WaitForSingleObject(proc, INFINITE);
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return (int)code;
}

/* Run "cmd.exe /d /s /c "@text"" (a pipe side, a FOR /F command) */
static HANDLE spawn_cmd(const char *text, Io *io)
{
    size_t n = strlen(text) + strlen(g_self) + 32;
    char *line = xmalloc(n);
    snprintf(line, n, "\"%s\" /d /s /c \"%s\"", g_self, text);
    HANDLE h = spawn(g_self, line, io);
    free(line);
    return h;
}

/* -----------------------------------------------------------------------
 * Batch files
 * ----------------------------------------------------------------------- */
static int run_line(const char *line, Io *io, int from_batch);
static int exec_node(Node *n, Io *io);

static char **read_lines(const char *path, int *count)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, 0), got = 0;
    char *buf = xmalloc(size + 1);
    ReadFile(h, buf, size, &got, 0);
    CloseHandle(h);
    buf[got] = 0;
    int n = 1;
    for (DWORD i = 0; i < got; i++) if (buf[i] == '\n') n++;
    char **lines = xmalloc(sizeof(char *) * (size_t)(n + 1));
    int k = 0;
    char *s = buf;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
    for (;;) {
        char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len && s[len - 1] == '\r') len--;
        lines[k++] = xstrndup(s, len);
        if (!e) break;
        s = e + 1;
    }
    free(buf);
    *count = k;
    return lines;
}

static void endlocal_one(void);

static int find_label(Batch *b, const char *label)
{
    const char *l = label[0] == ':' ? label + 1 : label;
    char want[256];
    int k = 0;
    while (*l && *l != ' ' && *l != '\t' && *l != ':' && k < 255) want[k++] = *l++;
    want[k] = 0;
    for (int i = 0; i < b->nlines; i++) {
        const char *s = b->lines[i];
        while (*s == ' ' || *s == '\t' || *s == '@') s++;
        if (*s != ':' || s[1] == ':') continue;
        s++;
        char got[256];
        int g = 0;
        while (*s && *s != ' ' && *s != '\t' && *s != ':' && *s != '+' && g < 255) got[g++] = *s++;
        got[g] = 0;
        if (ieq(got, want)) return i + 1;
    }
    return -1;
}

/* Lines of a batch file up to where a ( block closes */
static char *read_block(Batch *b, const char *first)
{
    Str s = { 0 };
    sputs(&s, first);
    for (;;) {
        int depth = 0, q = 0;
        for (const char *c = s.s; *c; c++) {
            if (*c == '"') q = !q;
            if (*c == '\n') q = 0;
            if (q) continue;
            if (*c == '^' && c[1]) { c++; continue; }
            if (*c == '(') depth++;
            if (*c == ')' && depth > 0) depth--;
        }
        if (depth <= 0 || b->pc >= b->nlines) break;
        sputc(&s, '\n');
        sputs(&s, b->lines[b->pc++]);
    }
    return sdone(&s);
}

static int run_batch_ctx(Batch *b, Io *io)
{
    Batch *prev = g_batch;
    b->parent = prev;
    b->locals = g_nlocals;
    g_batch = b;
    while (!g_exit && b->pc < b->nlines) {
        const char *raw = b->lines[b->pc++];
        const char *s = raw;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || (*s == ':' )) continue;             /* labels, :: comments */
        char *text = read_block(b, raw);
        run_line(text, io, 1);
        free(text);
        if (g_jump) {
            g_jump = 0;
            if (b->pc < 0) break;                       /* EXIT /B or GOTO :EOF */
        }
    }
    while (g_nlocals > b->locals) endlocal_one();      /* implicit ENDLOCAL */
    g_batch = prev;
    return g_errorlevel;
}

static int run_batch(const char *path, const char *args, Io *io)
{
    Batch b;
    memset(&b, 0, sizeof(b));
    char full[MAX_PATH];
    GetFullPathNameA(path, MAX_PATH, full, 0);
    b.lines = read_lines(full, &b.nlines);
    if (!b.lines) { puts_h(io->err, "The system cannot find the batch label specified - \n"); return set_error(1), 1; }
    b.path = xstrdup(full);
    /* %0 is the name as given; then the arguments (quotes kept) */
    b.args[b.nargs++] = xstrdup(path);
    const char *p = args;
    while (b.nargs < 64) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';' || *p == '=') p++;
        if (!*p) break;
        const char *st = p;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t' && *p != ',' && *p != ';' && *p != '='))) { if (*p == '"') q = !q; p++; }
        b.args[b.nargs++] = xstrndup(st, (size_t)(p - st));
    }
    int r = run_batch_ctx(&b, io);
    for (int i = 0; i < b.nlines; i++) free(b.lines[i]);
    free(b.lines);
    for (int i = 0; i < b.nargs; i++) free(b.args[i]);
    free(b.path);
    return r;
}

/* CALL :label args: the same file from the label, with new arguments */
static int call_label(const char *label, const char *args, Io *io)
{
    Batch *cur = g_batch;
    if (!cur) { puts_h(io->err, "Invalid attempt to call batch label outside of batch script.\n"); return set_error(1), 1; }
    int at = find_label(cur, label);
    if (at < 0) { printf_h(io->err, "The system cannot find the batch label specified - %s\n", label); return set_error(1), 1; }
    Batch b;
    memset(&b, 0, sizeof(b));
    b.lines = cur->lines;
    b.nlines = cur->nlines;
    b.pc = at;
    b.path = cur->path;
    b.is_call_label = 1;
    b.args[b.nargs++] = xstrdup(label);
    const char *p = args;
    while (b.nargs < 64) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        const char *st = p;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t' && *p != ',' && *p != ';'))) { if (*p == '"') q = !q; p++; }
        b.args[b.nargs++] = xstrndup(st, (size_t)(p - st));
    }
    int r = run_batch_ctx(&b, io);
    for (int i = 0; i < b.nargs; i++) free(b.args[i]);
    return r;
}

/* -----------------------------------------------------------------------
 * SETLOCAL / ENDLOCAL
 * ----------------------------------------------------------------------- */
static void setlocal(void)
{
    Local *l = xmalloc(sizeof(Local));
    char *env = GetEnvironmentStringsA();
    size_t n = 0;
    while (env[n] || env[n + 1]) n++;
    l->env = xmalloc(n + 2);
    memcpy(l->env, env, n + 2);
    FreeEnvironmentStringsA(env);
    cwd(l->cwd);
    l->delayed = g_delayed;
    l->next = g_locals;
    g_locals = l;
    g_nlocals++;
}

static void endlocal_one(void)
{
    Local *l = g_locals;
    if (!l) return;
    g_locals = l->next;
    g_nlocals--;
    /* clear everything, then restore the snapshot */
    char *env = GetEnvironmentStringsA();
    for (char *e = env; *e; e += strlen(e) + 1) {
        char *eq = strchr(e + 1, '=');
        if (!eq) continue;
        char name[512];
        size_t k = (size_t)(eq - e) < sizeof(name) - 1 ? (size_t)(eq - e) : sizeof(name) - 1;
        memcpy(name, e, k);
        name[k] = 0;
        SetEnvironmentVariableA(name, 0);
    }
    FreeEnvironmentStringsA(env);
    for (char *e = l->env; *e; e += strlen(e) + 1) {
        char *eq = strchr(e + 1, '=');
        if (!eq) continue;
        *eq = 0;
        SetEnvironmentVariableA(e, eq + 1);
        *eq = '=';
    }
    SetCurrentDirectoryA(l->cwd);
    g_delayed = l->delayed;
    free(l->env);
    free(l);
}

/* -----------------------------------------------------------------------
 * SET /A
 * ----------------------------------------------------------------------- */
typedef struct { const char *s; int err; } Ex;
static long ex_assign(Ex *e);

static void ex_ws(Ex *e) { while (*e->s == ' ' || *e->s == '\t' || *e->s == '"') e->s++; }

static long var_num(const char *name)
{
    char *v = getvar(name);
    long r = v ? strtol(v, 0, 0) : 0;
    free(v);
    return r;
}

static void var_set_num(const char *name, long v)
{
    char b[32];
    sprintf(b, "%ld", v);
    SetEnvironmentVariableA(name, b);
}

static int ex_name(Ex *e, char *name)
{
    ex_ws(e);
    int k = 0;
    const char *s = e->s;
    if (!(isalpha((unsigned char)*s) || *s == '_' || *s == '$' || *s == '.' || *s == '#' || *s == '@' || *s == '[' || *s == ']' || (unsigned char)*s >= 0x80)) return 0;
    while (*s && !strchr(" \t\"+-*/%()<>&|^~!=,", *s) && k < 255) name[k++] = *s++;
    name[k] = 0;
    e->s = s;
    return k;
}

static long ex_unary(Ex *e)
{
    ex_ws(e);
    char c = *e->s;
    if (c == '-') { e->s++; return -ex_unary(e); }
    if (c == '+') { e->s++; return ex_unary(e); }
    if (c == '!') { e->s++; return !ex_unary(e); }
    if (c == '~') { e->s++; return ~ex_unary(e); }
    if (c == '(') {
        e->s++;
        long v = ex_assign(e);
        for (;;) { ex_ws(e); if (*e->s != ',') break; e->s++; v = ex_assign(e); }
        ex_ws(e);
        if (*e->s == ')') e->s++; else e->err = 1;
        return v;
    }
    if (c >= '0' && c <= '9') { char *end; long v = strtol(e->s, &end, 0); e->s = end; return v; }
    char name[256];
    if (ex_name(e, name)) return var_num(name);
    e->err = 1;
    return 0;
}

static long ex_bin(Ex *e, int level)
{
    static const char *ops[][4] = {
        { "|", 0 }, { "^", 0 }, { "&", 0 }, { "<<", ">>", 0 }, { "+", "-", 0 }, { "*", "/", "%", 0 },
    };
    if (level == 6) return ex_unary(e);
    long v = ex_bin(e, level + 1);
    for (;;) {
        ex_ws(e);
        const char *op = 0;
        for (int i = 0; ops[level][i]; i++) {
            size_t n = strlen(ops[level][i]);
            if (!strncmp(e->s, ops[level][i], n) && e->s[n] != '=' &&
                !(n == 1 && (e->s[0] == '<' || e->s[0] == '>')) &&
                !(n == 1 && (e->s[0] == '&' || e->s[0] == '|') && e->s[1] == e->s[0])) { op = ops[level][i]; break; }
        }
        if (!op) return v;
        e->s += strlen(op);
        long r = ex_bin(e, level + 1);
        switch (op[0]) {
        case '|': v |= r; break;
        case '^': v ^= r; break;
        case '&': v &= r; break;
        case '<': v <<= r; break;
        case '>': v >>= r; break;
        case '+': v += r; break;
        case '-': v -= r; break;
        case '*': v *= r; break;
        case '/': if (!r) { e->err = 2; return 0; } v /= r; break;
        case '%': if (!r) { e->err = 2; return 0; } v %= r; break;
        }
    }
}

static long ex_assign(Ex *e)
{
    ex_ws(e);
    const char *save = e->s;
    char name[256];
    if (ex_name(e, name)) {
        ex_ws(e);
        static const char *aops[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "=", 0 };
        for (int i = 0; aops[i]; i++) {
            size_t n = strlen(aops[i]);
            if (strncmp(e->s, aops[i], n) || (n == 1 && e->s[1] == '=')) continue;
            e->s += n;
            long r = ex_assign(e), v = var_num(name);
            switch (aops[i][0]) {
            case '<': v <<= r; break; case '>': v >>= r; break;
            case '+': v += r; break; case '-': v -= r; break;
            case '*': v *= r; break;
            case '/': if (!r) { e->err = 2; return 0; } v /= r; break;
            case '%': if (!r) { e->err = 2; return 0; } v %= r; break;
            case '&': v &= r; break; case '|': v |= r; break; case '^': v ^= r; break;
            default: v = r;
            }
            var_set_num(name, v);
            return v;
        }
        e->s = save;
    }
    return ex_bin(e, 0);
}

static int cmd_set_a(const char *expr, Io *io)
{
    Ex e = { expr, 0 };
    long v = ex_assign(&e);
    for (;;) { ex_ws(&e); if (*e.s != ',') break; e.s++; v = ex_assign(&e); }
    ex_ws(&e);
    if (e.err == 2) { puts_h(io->err, "Divide by zero error.\n"); return 1073750993; }
    if (e.err || *e.s) { puts_h(io->err, "Missing operand.\n"); return 1073750988; }
    if (!g_batch) printf_h(io->out, "%ld", v);         /* typed, or cmd /c: the result is shown */
    return 0;
}

/* -----------------------------------------------------------------------
 * Line input (SET /P, PAUSE, the interactive prompt)
 * ----------------------------------------------------------------------- */
static char g_inbuf[4096];
static int  g_inlen, g_inpos;
static HANDLE g_inh;

/* One line (without the newline) from @h; -1 at the end */
static int read_line(HANDLE h, char *out, int cap)
{
    if (h != g_inh) { g_inh = h; g_inlen = g_inpos = 0; }
    int n = 0, any = 0;
    for (;;) {
        if (g_inpos >= g_inlen) {
            DWORD got = 0;
            if (!ReadFile(h, g_inbuf, sizeof(g_inbuf), &got, 0) || !got) return any ? n : -1;
            g_inlen = (int)got;
            g_inpos = 0;
        }
        char c = g_inbuf[g_inpos++];
        any = 1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* -----------------------------------------------------------------------
 * Internal commands
 * ----------------------------------------------------------------------- */
typedef int (*Builtin)(const char *args, Io *io);

static void print_prompt(HANDLE h)
{
    char *p = getvar("PROMPT");
    const char *s = p ? p : "$P$G";
    Str b = { 0 };
    for (; *s; s++) {
        if (*s != '$' || !s[1]) { sputc(&b, *s); continue; }
        char c = (char)toupper((unsigned char)*++s);
        char cur[MAX_PATH];
        switch (c) {
        case 'P': cwd(cur); sputs(&b, cur); break;
        case 'G': sputc(&b, '>'); break;
        case 'L': sputc(&b, '<'); break;
        case 'B': sputc(&b, '|'); break;
        case 'Q': sputc(&b, '='); break;
        case 'S': sputc(&b, ' '); break;
        case '$': sputc(&b, '$'); break;
        case '_': sputs(&b, "\r\n"); break;
        case 'N': sputc(&b, 'C'); break;
        case 'D': { char *d = getvar("DATE"); sputs(&b, d); free(d); break; }
        case 'T': { char *d = getvar("TIME"); sputs(&b, d); free(d); break; }
        case 'V': sputs(&b, "NovaOS [Version 10.0.18362]"); break;
        default: break;
        }
    }
    wr(h, b.s, b.n);
    free(b.s);
    free(p);
}

static int b_echo(const char *a, Io *io)
{
    if (*a && strchr(" \t.(:;,/=+[]\\", *a)) {
        if (*a == ' ' || *a == '\t') {
            const char *t = a + 1;
            while (*t == ' ' || *t == '\t') t++;
            if (!*t) { printf_h(io->out, "ECHO is %s.\n", g_echo ? "on" : "off"); return 0; }
            if (ieq(t, "on")) { g_echo = 1; return 0; }
            if (ieq(t, "off")) { g_echo = 0; return 0; }
        }
        puts_h(io->out, a + 1);
        puts_h(io->out, "\n");
        return 0;
    }
    if (!*a) { printf_h(io->out, "ECHO is %s.\n", g_echo ? "on" : "off"); return 0; }
    puts_h(io->out, a);
    puts_h(io->out, "\n");
    return 0;
}

static int b_set(const char *a, Io *io)
{
    while (*a == ' ' || *a == '\t') a++;
    if (iprefix(a, "/a")) return cmd_set_a(a + 2, io);
    if (iprefix(a, "/p")) {
        const char *s = a + 2;
        while (*s == ' ') s++;
        char *spec = *s == '"' ? strip_quotes(s) : xstrdup(s);
        char *eq = strchr(spec, '=');
        if (!eq) { free(spec); puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
        *eq = 0;
        puts_h(io->out, eq + 1);
        char line[LINE_MAX_];
        int n = read_line(io->in, line, sizeof(line));
        if (n < 0) { free(spec); return 1; }
        SetEnvironmentVariableA(spec, n ? line : 0);
        free(spec);
        return 0;
    }
    char *spec;
    if (*a == '"') {                                   /* set "name=value" (to the last quote) */
        const char *last = strrchr(a, '"');
        spec = xstrndup(a + 1, (size_t)(last > a ? last - a - 1 : (long)strlen(a + 1)));
    } else spec = xstrdup(a);
    char *eq = strchr(spec, '=');
    if (!eq) {                                         /* list: all, or those starting with @spec */
        size_t n = strlen(spec);
        while (n && spec[n - 1] == ' ') spec[--n] = 0;
        char *env = GetEnvironmentStringsA();
        int found = 0;
        for (char *e = env; *e; e += strlen(e) + 1) {
            if (e[0] == '=') continue;
            if (n && _strnicmp(e, spec, n)) continue;
            puts_h(io->out, e);
            puts_h(io->out, "\n");
            found = 1;
        }
        FreeEnvironmentStringsA(env);
        if (!found && n) { printf_h(io->err, "Environment variable %s not defined\n", spec); free(spec); return 1; }
        free(spec);
        return 0;
    }
    *eq = 0;
    if (!*spec) { free(spec); puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
    SetEnvironmentVariableA(spec, eq[1] ? eq + 1 : 0);
    free(spec);
    return 0;
}

static int change_dir(const char *path, Io *io)
{
    char *p = strip_quotes(path);
    size_t n = strlen(p);
    while (n > 3 && (p[n - 1] == ' ' || p[n - 1] == '\\')) p[--n] = 0;
    if (n == 2 && p[1] == ':') { free(p); return 0; }         /* "C:" */
    int ok = SetCurrentDirectoryA(p);
    free(p);
    if (!ok) { puts_h(io->err, "The system cannot find the path specified.\n"); return 1; }
    char cur[MAX_PATH];
    cwd(cur);
    SetCurrentDirectoryA(cur);
    return 0;
}

static int b_cd(const char *a, Io *io)
{
    while (*a == ' ' || *a == '\t') a++;
    if (iprefix(a, "/d")) { a += 2; while (*a == ' ') a++; }
    if (!*a) { char c[MAX_PATH]; cwd(c); printf_h(io->out, "%s\n", c); return 0; }
    return change_dir(a, io);
}

static void fmt_thousands(unsigned long long v, char *out)
{
    char t[32];
    int n = sprintf(t, "%llu", v), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = t[i];
    }
    out[o] = 0;
}

static void fmt_time(const FILETIME *ft, char *out)
{
    FILETIME lt;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    int h = st.wHour % 12 ? st.wHour % 12 : 12;
    sprintf(out, "%02d/%02d/%04d  %02d:%02d %s", st.wMonth, st.wDay, st.wYear, h, st.wMinute, st.wHour < 12 ? "AM" : "PM");
}

typedef struct { int bare, sub, wide, attr_dirs, attr_files; unsigned long long files, dirs, bytes; } DirOpt;

/* The free bytes, with thousands separators, on the drive holding @path */
static void free_on(const char *path, char *out)
{
    char full[MAX_PATH], root[4] = "C:\\";
    ULARGE_INTEGER fr;
    if (GetFullPathNameA(path, MAX_PATH, full, 0) && full[0] && full[1] == ':') root[0] = full[0];
    strcpy(out, "0");
    if (GetDiskFreeSpaceExA(root, &fr, 0, 0)) fmt_thousands(fr.QuadPart, out);
}

static int dir_list(const char *spec, DirOpt *o, Io *io, int top)
{
    char pattern[MAX_PATH], dirpath[MAX_PATH];
    GetFullPathNameA(spec, MAX_PATH, pattern, 0);
    DWORD at = GetFileAttributesA(pattern);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && !strpbrk(pattern + 2, "*?")) {
        size_t l = strlen(pattern);
        if (pattern[l - 1] != '\\') strcat(pattern, "\\");
        strcat(pattern, "*");
    }
    strcpy(dirpath, pattern);
    char *slash = strrchr(dirpath, '\\');
    if (slash) *(slash == dirpath + 2 ? slash + 1 : slash) = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int any = 0;
    unsigned long long files = 0, dirs = 0, bytes = 0;
    if (h != INVALID_HANDLE_VALUE) {
        int header = 0;
        do {
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (o->attr_dirs && !isdir) continue;
            if (o->attr_files && isdir) continue;
            if (o->bare && (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))) continue;
            any = 1;
            if (!o->bare && !header) { printf_h(io->out, " Directory of %s\n\n", dirpath); header = 1; }
            if (o->bare) {
                if (o->sub) printf_h(io->out, "%s%s%s\n", dirpath, dirpath[strlen(dirpath) - 1] == '\\' ? "" : "\\", fd.cFileName);
                else printf_h(io->out, "%s\n", fd.cFileName);
            } else {
                char t[40], sz[32];
                fmt_time(&fd.ftLastWriteTime, t);
                if (isdir) printf_h(io->out, "%s    <DIR>          %s\n", t, fd.cFileName);
                else {
                    fmt_thousands(((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, sz);
                    printf_h(io->out, "%s %17s %s\n", t, sz, fd.cFileName);
                }
            }
            if (isdir) dirs++;
            else { files++; bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (header && !o->bare) {
            char b[32];
            fmt_thousands(bytes, b);
            printf_h(io->out, "%16llu File(s) %14s bytes\n", files, b);
            if (!o->sub) {
                char f[32];
                free_on(dirpath, f);
                printf_h(io->out, "%16llu Dir(s) %15s bytes free\n", dirs, f);
            } else puts_h(io->out, "\n");
        }
    }
    o->files += files; o->dirs += dirs; o->bytes += bytes;
    if (o->sub) {                                      /* then every subdirectory */
        const char *leaf = strrchr(pattern, '\\') + 1;
        char all[MAX_PATH];
        snprintf(all, sizeof(all), "%s%s*", dirpath, dirpath[strlen(dirpath) - 1] == '\\' ? "" : "\\");
        h = FindFirstFileA(all, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                char sub[MAX_PATH * 2];
                snprintf(sub, sizeof(sub), "%s%s%s\\%s", dirpath, dirpath[strlen(dirpath) - 1] == '\\' ? "" : "\\", fd.cFileName, leaf);
                if (dir_list(sub, o, io, 0)) any = 1;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    (void)top;
    return any;
}

static int b_dir(const char *a, Io *io)
{
    char *av[32];
    int n = split_args(a, av, 32);
    DirOpt o;
    memset(&o, 0, sizeof(o));
    const char *spec = 0;
    for (int i = 0; i < n; i++) {
        if (av[i][0] == '/') {
            for (char *c = av[i] + 1; *c; c++) {
                char s = (char)toupper((unsigned char)*c);
                if (s == 'B') o.bare = 1;
                else if (s == 'S') o.sub = 1;
                else if (s == 'W') o.wide = 1;
                else if (s == 'A') {
                    if (c[1] == ':') c++;
                    if (toupper((unsigned char)c[1]) == 'D') { o.attr_dirs = 1; c++; }
                    else if (c[1] == '-' && toupper((unsigned char)c[2]) == 'D') { o.attr_files = 1; c += 2; }
                }
                else if (s == 'O' || s == 'P' || s == 'Q' || s == 'N' || s == 'L' || s == 'X' || s == 'C' || s == '-' || s == ':' || s == 'T') {}
            }
        } else spec = av[i];
    }
    if (!o.bare) {                                      /* the drive being listed */
        char full[MAX_PATH], root[4] = "C:\\", label[64] = "";
        DWORD serial = 0;
        if (GetFullPathNameA(spec ? spec : ".", MAX_PATH, full, 0) && full[0] && full[1] == ':') root[0] = (char)toupper((unsigned char)full[0]);
        GetVolumeInformationA(root, label, sizeof(label), &serial, 0, 0, 0, 0);
        if (label[0]) printf_h(io->out, " Volume in drive %c is %s\n", root[0], label);
        else printf_h(io->out, " Volume in drive %c has no label.\n", root[0]);
        printf_h(io->out, " Volume Serial Number is %04lX-%04lX\n\n", (unsigned long)(serial >> 16), (unsigned long)(serial & 0xFFFF));
    }
    int any = dir_list(spec ? spec : ".", &o, io, 1);
    if (o.sub && !o.bare && any) {
        char b[32], f[32];
        fmt_thousands(o.bytes, b);
        free_on(spec ? spec : ".", f);
        printf_h(io->out, "     Total Files Listed:\n%16llu File(s) %14s bytes\n%16llu Dir(s) %15s bytes free\n", o.files, b, o.dirs, f);
    }
    free_args(av, n);
    if (!any) { puts_h(o.bare ? io->err : io->out, "File Not Found\n"); return 1; }
    return 0;
}

/* Every file matching @pattern: calls fn(full path, name) */
static int each_file(const char *pattern, int dirs_too, int (*fn)(const char *path, void *ctx), void *ctx)
{
    char full[MAX_PATH];
    GetFullPathNameA(pattern, MAX_PATH, full, 0);
    char dir[MAX_PATH];
    strcpy(dir, full);
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(full, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (!dirs_too && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char p[MAX_PATH * 2];
        snprintf(p, sizeof(p), "%s%s", dir, fd.cFileName);
        n++;
        if (fn(p, ctx)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int type_one(const char *path, void *ctx)
{
    Io *io = ctx;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[8192];
    DWORD got;
    if (!ReadFile(h, buf, sizeof(buf), &got, 0)) got = 0;
    if (got >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE) {
        /* UTF-16 with a byte-order mark (as Windows' type reads it): shown
         * as text; an odd byte or a lone high surrogate at the end of a
         * read waits for the next one */
        WCHAR w[4096 + 2];
        char out[sizeof(w) / sizeof(WCHAR) * 3];
        int carry = got - 2, have = 0;
        memmove(buf, buf + 2, carry);
        for (;;) {
            int whole = carry & ~1;
            memcpy((char *)w + have * 2, buf, whole);
            int n = have + whole / 2;
            have = n && w[n - 1] >= 0xD800 && w[n - 1] < 0xDC00 ? 1 : 0;
            int k = WideCharToMultiByte(CP_UTF8, 0, w, n - have, out, sizeof(out), 0, 0);
            if (k > 0) wr(io->out, out, k);
            if (have) w[0] = w[n - 1];
            memmove(buf, buf + whole, carry - whole);
            carry -= whole;
            if (!ReadFile(h, buf + carry, 8192 - carry, &got, 0) || !got) break;
            carry += got;
        }
    } else {
        while (got) {
            wr(io->out, buf, got);
            if (!ReadFile(h, buf, sizeof(buf), &got, 0)) break;
        }
    }
    CloseHandle(h);
    return 0;
}

static int b_type(const char *a, Io *io)
{
    char *av[32];
    int n = split_args(a, av, 32), rc = 0;
    if (!n) { puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
    for (int i = 0; i < n; i++) {
        if (strpbrk(av[i], "*?")) { if (!each_file(av[i], 0, type_one, io)) rc = 1; continue; }
        if (!file_exists(av[i])) { puts_h(io->err, "The system cannot find the file specified.\n"); rc = 1; continue; }
        type_one(av[i], io);
    }
    free_args(av, n);
    return rc;
}

typedef struct { const char *dst; int dst_dir, count, err; Io *io; } CopyCtx;

static int copy_one(const char *src, void *vctx)
{
    CopyCtx *c = vctx;
    char to[MAX_PATH * 2];
    const char *name = strrchr(src, '\\') ? strrchr(src, '\\') + 1 : src;
    if (c->dst_dir) snprintf(to, sizeof(to), "%s%s%s", c->dst, c->dst[strlen(c->dst) - 1] == '\\' ? "" : "\\", name);
    else snprintf(to, sizeof(to), "%s", c->dst);
    char a[MAX_PATH], b[MAX_PATH];
    GetFullPathNameA(src, MAX_PATH, a, 0);
    GetFullPathNameA(to, MAX_PATH, b, 0);
    if (!_stricmp(a, b)) { puts_h(c->io->err, "The file cannot be copied onto itself.\n"); c->err = 1; return 0; }
    if (!CopyFileA(src, to, FALSE)) { c->err = 1; puts_h(c->io->err, "The system cannot find the file specified.\n"); return 0; }
    c->count++;
    return 0;
}

static int b_copy(const char *a, Io *io)
{
    char *av[32];
    int n = split_args(a, av, 32), k = 0;
    char *files[32];
    for (int i = 0; i < n; i++) if (av[i][0] != '/') files[k++] = av[i];
    if (!k) { free_args(av, n); puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
    const char *dst = k > 1 ? files[k - 1] : ".";
    int srcs = k > 1 ? k - 1 : 1;
    CopyCtx c = { dst, 0, 0, 0, io };
    DWORD at = GetFileAttributesA(dst);
    c.dst_dir = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
    for (int i = 0; i < srcs; i++) {
        if (strchr(files[i], '+')) {                    /* a+b dest: concatenate */
            HANDLE out = CreateFileA(dst, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
            char *parts = xstrdup(files[i]);
            for (char *t = strtok(parts, "+"); t && out != INVALID_HANDLE_VALUE; t = strtok(0, "+")) {
                Io tio = { io->in, out, io->err };
                if (file_exists(t)) { type_one(t, &tio); c.count = 1; }
            }
            free(parts);
            if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
            continue;
        }
        if (!each_file(files[i], 0, copy_one, &c)) { puts_h(io->err, "The system cannot find the file specified.\n"); c.err = 1; }
    }
    printf_h(io->out, "%9d file(s) copied.\n", c.count);
    free_args(av, n);
    return c.err;
}

static int del_one(const char *path, void *ctx)
{
    (void)ctx;
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(path);
    return 0;
}

static int b_del(const char *a, Io *io)
{
    char *av[32];
    int n = split_args(a, av, 32), rc = 0, sub = 0;
    for (int i = 0; i < n; i++) if (is_switch(av[i], "s")) sub = 1;
    for (int i = 0; i < n; i++) {
        if (av[i][0] == '/') continue;
        DWORD at = GetFileAttributesA(av[i]);
        char pat[MAX_PATH * 2];
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) snprintf(pat, sizeof(pat), "%s\\*", av[i]);
        else snprintf(pat, sizeof(pat), "%s", av[i]);
        if (!each_file(pat, 0, del_one, 0) && !sub) { char f[MAX_PATH]; GetFullPathNameA(av[i], MAX_PATH, f, 0); printf_h(io->err, "Could Not Find %s\n", f); }
    }
    free_args(av, n);
    return rc;
}

static int make_dirs(const char *path)
{
    char full[MAX_PATH];
    GetFullPathNameA(path, MAX_PATH, full, 0);
    for (char *c = full + 3; *c; c++) {
        if (*c != '\\') continue;
        *c = 0;
        CreateDirectoryA(full, 0);
        *c = '\\';
    }
    return CreateDirectoryA(full, 0) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int b_md(const char *a, Io *io)
{
    char *av[32];
    int n = split_args(a, av, 32), rc = 0;
    if (!n) { puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
    for (int i = 0; i < n; i++) {
        if (GetFileAttributesA(av[i]) != INVALID_FILE_ATTRIBUTES) {
            printf_h(io->err, "A subdirectory or file %s already exists.\n", av[i]);
            rc = 1;
        } else if (!make_dirs(av[i])) { puts_h(io->err, "The system cannot find the path specified.\n"); rc = 1; }
    }
    free_args(av, n);
    return rc;
}

static int remove_tree(const char *dir)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char p[MAX_PATH * 2];
            snprintf(p, sizeof(p), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else { SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL); DeleteFileA(p); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir);
}

static int b_rd(const char *a, Io *io)
{
    char *av[32];
    int n = split_args(a, av, 32), rc = 0, sub = 0;
    for (int i = 0; i < n; i++) if (is_switch(av[i], "s")) sub = 1;
    for (int i = 0; i < n; i++) {
        if (av[i][0] == '/') continue;
        DWORD at = GetFileAttributesA(av[i]);
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
            puts_h(io->err, at == INVALID_FILE_ATTRIBUTES ? "The system cannot find the file specified.\n" : "The directory name is invalid.\n");
            rc = 1;
            continue;
        }
        if (!(sub ? remove_tree(av[i]) : RemoveDirectoryA(av[i]))) {
            puts_h(io->err, GetLastError() == ERROR_DIR_NOT_EMPTY ? "The directory is not empty.\n" : "Access is denied.\n");
            rc = 1;
        }
    }
    free_args(av, n);
    return rc;
}

static int b_ren(const char *a, Io *io)
{
    char *av[4];
    int n = split_args(a, av, 4);
    if (n != 2) { free_args(av, n); puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
    char to[MAX_PATH * 2];
    const char *slash = strrchr(av[0], '\\');
    if (slash && !strchr(av[1], '\\')) snprintf(to, sizeof(to), "%.*s%s", (int)(slash - av[0] + 1), av[0], av[1]);
    else snprintf(to, sizeof(to), "%s", av[1]);
    int ok = MoveFileA(av[0], to);
    free_args(av, n);
    if (!ok) { puts_h(io->err, GetLastError() == ERROR_ALREADY_EXISTS ? "A duplicate file name exists, or the file\ncannot be found.\n" : "The system cannot find the file specified.\n"); return 1; }
    return 0;
}

static int b_move(const char *a, Io *io)
{
    char *av[8];
    int n = split_args(a, av, 8), k = 0;
    char *f[8];
    for (int i = 0; i < n; i++) if (av[i][0] != '/') f[k++] = av[i];
    if (k != 2) { free_args(av, n); puts_h(io->err, "The syntax of the command is incorrect.\n"); return 1; }
    char to[MAX_PATH * 2];
    DWORD at = GetFileAttributesA(f[1]);
    const char *name = strrchr(f[0], '\\') ? strrchr(f[0], '\\') + 1 : f[0];
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) snprintf(to, sizeof(to), "%s\\%s", f[1], name);
    else snprintf(to, sizeof(to), "%s", f[1]);
    int ok = MoveFileExA(f[0], to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free_args(av, n);
    if (!ok) { puts_h(io->err, "The system cannot find the file specified.\n"); return 1; }
    printf_h(io->out, "        1 file(s) moved.\n");
    return 0;
}

static int b_cls(const char *a, Io *io) { (void)a; wr(io->out, "\x1b[2J\x1b[H", 7); return 0; }

static int b_pause(const char *a, Io *io)
{
    (void)a;
    puts_h(io->out, "Press any key to continue . . . ");
    char line[256];
    read_line(io->in, line, sizeof(line));
    puts_h(io->out, "\n");
    return 0;
}

static int b_ver(const char *a, Io *io) { (void)a; puts_h(io->out, "\nNovaOS [Version 10.0.18362]\n"); return 0; }
static int b_title(const char *a, Io *io) { (void)io; SetConsoleTitleA(a); return 0; }
static int b_rem(const char *a, Io *io) { (void)a; (void)io; return 0; }
static int b_vol(const char *a, Io *io) { (void)a; puts_h(io->out, " Volume in drive C is NOVA\n Volume Serial Number is 4E4F-5641\n"); return 0; }

static int b_path(const char *a, Io *io)
{
    while (*a == ' ') a++;
    if (!*a) { char *p = getvar("PATH"); printf_h(io->out, "PATH=%s\n", p ? p : "(null)"); free(p); return 0; }
    if (*a == '=') a++;
    SetEnvironmentVariableA("PATH", strcmp(a, ";") ? a : 0);
    return 0;
}

static int b_prompt(const char *a, Io *io) { (void)io; while (*a == ' ') a++; SetEnvironmentVariableA("PROMPT", *a ? a : 0); return 0; }

static int b_pushd(const char *a, Io *io)
{
    while (*a == ' ') a++;
    char cur[MAX_PATH];
    cwd(cur);
    if (!*a) { for (int i = g_npushd - 1; i >= 0; i--) printf_h(io->out, "%s\n", g_pushd[i]); return 0; }
    if (change_dir(a, io)) return 1;
    if (g_npushd < 32) strcpy(g_pushd[g_npushd++], cur);
    return 0;
}

static int b_popd(const char *a, Io *io)
{
    (void)a;
    if (!g_npushd) return 0;
    return change_dir(g_pushd[--g_npushd], io);
}

static int b_setlocal(const char *a, Io *io)
{
    (void)io;
    if (!g_batch) return 0;
    setlocal();
    char *av[4];
    int n = split_args(a, av, 4);
    for (int i = 0; i < n; i++) {
        if (ieq(av[i], "enabledelayedexpansion")) g_delayed = 1;
        else if (ieq(av[i], "disabledelayedexpansion")) g_delayed = 0;
    }
    free_args(av, n);
    return 0;
}

static int b_endlocal(const char *a, Io *io)
{
    (void)a; (void)io;
    if (g_batch && g_nlocals > g_batch->locals) endlocal_one();
    return 0;
}

static int b_shift(const char *a, Io *io)
{
    (void)io;
    if (!g_batch) return 0;
    while (*a == ' ') a++;
    if (*a == '/' && a[1] >= '0' && a[1] <= '8') {           /* SHIFT /n: from %n on */
        int k = a[1] - '0' + g_batch->shift;
        if (k < g_batch->nargs) {
            free(g_batch->args[k]);
            for (int i = k; i < g_batch->nargs - 1; i++) g_batch->args[i] = g_batch->args[i + 1];
            g_batch->nargs--;
        }
        return 0;
    }
    g_batch->shift++;
    return 0;
}

static int b_goto(const char *a, Io *io)
{
    while (*a == ' ' || *a == '\t') a++;
    if (!g_batch) return 0;
    if (ieq(a, ":eof") || iprefix(a, ":eof ")) { g_batch->pc = -1; g_jump = 1; return g_errorlevel; }
    int at = find_label(g_batch, a);
    if (at < 0) {
        printf_h(io->err, "The system cannot find the batch label specified - %s\n", a[0] == ':' ? a + 1 : a);
        g_batch->pc = -1;
        g_jump = 1;
        return 1;
    }
    g_batch->pc = at;
    g_jump = 1;
    return g_errorlevel;
}

static int b_exit(const char *a, Io *io)
{
    (void)io;
    while (*a == ' ') a++;
    int b = 0;
    if (iprefix(a, "/b")) { b = 1; a += 2; while (*a == ' ') a++; }
    int code = *a ? atoi(a) : g_errorlevel;
    if (b && g_batch) { g_batch->pc = -1; g_jump = 1; return code; }
    if (b) return code;
    g_exit = 1;
    g_exit_code = code;
    return code;
}

static int b_start(const char *a, Io *io)
{
    const char *p = a;
    while (*p == ' ') p++;
    if (*p == '"') {                                    /* the window title */
        const char *e = strchr(p + 1, '"');
        p = e ? e + 1 : p + strlen(p);
    }
    int wait = 0, same = 0;
    char dir[MAX_PATH] = "";
    for (;;) {
        while (*p == ' ') p++;
        if (*p != '/') break;
        const char *sw = p + 1;
        const char *e = sw;
        while (*e && *e != ' ') e++;
        if (!_strnicmp(sw, "wait", 4)) wait = 1;
        else if (e - sw == 1 && (*sw | 0x20) == 'b') same = 1;     /* /B: in this console */
        else if (!_strnicmp(sw, "d", 1)) {
            const char *v = sw + 1;
            if (v == e) { while (*e == ' ') e++; v = e; while (*e && *e != ' ') e++; }
            snprintf(dir, sizeof(dir), "%.*s", (int)(e - v), v);
        }
        p = e;
    }
    const char *rest = p;
    char *prog = next_arg(&rest);
    if (!prog) { spawn_cmd("", io); return 0; }
    char full[MAX_PATH];
    int rc = 0;
    if (find_command(prog, full) && !is_batch(full)) {
        STARTUPINFOA si;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        char *cl = xstrdup(p);
        /* a console program gets a console (window) of its own, as on Windows */
        if (CreateProcessA(full, cl, 0, 0, FALSE, same ? 0 : CREATE_NEW_CONSOLE, 0, dir[0] ? dir : 0, &si, &pi)) {
            CloseHandle(pi.hThread);
            if (wait) rc = wait_exit(pi.hProcess); else CloseHandle(pi.hProcess);
        } else rc = 1;
        free(cl);
    } else {
        /* a document or folder: the shell opens it */
        HMODULE sh = LoadLibraryA("shell32.dll");
        typedef HINSTANCE (WINAPI *ShellExec)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
        ShellExec fn = sh ? (ShellExec)(void *)GetProcAddress(sh, "ShellExecuteA") : 0;
        if (!fn || (INT_PTR)fn(0, "open", prog, rest && *rest ? rest : 0, dir[0] ? dir : 0, SW_SHOWNORMAL) <= 32) {
            printf_h(io->err, "The system cannot find the file %s.\n", prog);
            rc = 1;
        }
    }
    free(prog);
    return rc;
}

static int b_call(const char *a, Io *io);
static int b_help(const char *a, Io *io);

static int b_date(const char *a, Io *io) { (void)a; char *d = getvar("DATE"); printf_h(io->out, "%s\n", d); free(d); return 0; }
static int b_time(const char *a, Io *io) { (void)a; char *d = getvar("TIME"); printf_h(io->out, "%s\n", d); free(d); return 0; }
static int b_nothing(const char *a, Io *io) { (void)a; (void)io; return 0; }
static int b_verify(const char *a, Io *io) { (void)a; puts_h(io->out, "VERIFY is off.\n"); return 0; }

static const struct { const char *name; Builtin fn; } g_builtins[] = {
    { "echo", b_echo }, { "set", b_set }, { "cd", b_cd }, { "chdir", b_cd }, { "dir", b_dir },
    { "type", b_type }, { "copy", b_copy }, { "del", b_del }, { "erase", b_del }, { "md", b_md },
    { "mkdir", b_md }, { "rd", b_rd }, { "rmdir", b_rd }, { "ren", b_ren }, { "rename", b_ren },
    { "move", b_move }, { "cls", b_cls }, { "pause", b_pause }, { "ver", b_ver }, { "title", b_title },
    { "rem", b_rem }, { "vol", b_vol }, { "path", b_path }, { "prompt", b_prompt }, { "pushd", b_pushd },
    { "popd", b_popd }, { "setlocal", b_setlocal }, { "endlocal", b_endlocal }, { "shift", b_shift },
    { "goto", b_goto }, { "exit", b_exit }, { "start", b_start }, { "call", b_call }, { "help", b_help },
    { "date", b_date }, { "time", b_time }, { "color", b_nothing }, { "verify", b_verify },
    { "assoc", b_nothing }, { "ftype", b_nothing }, { "break", b_nothing },
};
#define NBUILTINS (sizeof(g_builtins) / sizeof(g_builtins[0]))

static int b_help(const char *a, Io *io)
{
    (void)a;
    puts_h(io->out, "For more information on a specific command, type the command name.\n");
    static const char *names[] = { "CALL", "CD", "CLS", "COPY", "DEL", "DIR", "ECHO", "ENDLOCAL", "EXIT", "FOR", "GOTO",
                                   "IF", "MD", "MOVE", "PATH", "PAUSE", "POPD", "PROMPT", "PUSHD", "RD", "REM", "REN",
                                   "SET", "SETLOCAL", "SHIFT", "START", "TITLE", "TYPE", "VER", "VOL" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) printf_h(io->out, "%s\n", names[i]);
    return 0;
}

/* The internal command @text starts with, and where its arguments begin */
static Builtin find_builtin(const char *text, const char **args)
{
    for (unsigned i = 0; i < NBUILTINS; i++) {
        size_t n = strlen(g_builtins[i].name);
        if (_strnicmp(text, g_builtins[i].name, n)) continue;
        char c = text[n];
        if (c && !strchr(" \t.(:;,/=+[]\\\"", c)) continue;
        if (c == '.' && i != 0 && strcmp(g_builtins[i].name, "cd") && strcmp(g_builtins[i].name, "chdir")) {
            /* "type.exe" is a program; only ECHO. CD.. and friends split at '.' */
            continue;
        }
        *args = text + n;
        return g_builtins[i].fn;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Running one command
 * ----------------------------------------------------------------------- */
static int run_command_text(const char *text, Io *io)
{
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) return g_errorlevel;
    const char *args;
    Builtin b = find_builtin(text, &args);
    if (b) {
        int rc = b(args, io);
        if (b != b_goto && b != b_rem && b != b_echo && b != b_setlocal && b != b_endlocal && b != b_title &&
            b != b_cls && b != b_pushd && b != b_popd && b != b_shift && b != b_path && b != b_prompt && b != b_nothing)
            set_error(rc);
        else if (rc) set_error(rc);
        return rc;
    }
    /* "C:" alone switches drives (there is only C:) */
    if (((text[0] | 0x20) >= 'a' && (text[0] | 0x20) <= 'z') && text[1] == ':' && (!text[2] || text[2] == ' ')) return 0;
    const char *rest = text;
    char *name = next_arg(&rest);
    char full[MAX_PATH];
    if (!find_command(name, full)) {
        printf_h(io->err, "'%s' is not recognized as an internal or external command,\noperable program or batch file.\n", name);
        free(name);
        set_error(9009);
        return 9009;
    }
    free(name);
    if (is_batch(full)) {                               /* runs here; no return to the caller's batch */
        while (*rest == ' ') rest++;
        if (g_batch) {
            Batch *caller = g_batch;
            int r = run_batch(full, rest, io);
            caller->pc = -1;                            /* transfer of control, as on Windows */
            g_jump = 1;
            return r;
        }
        return run_batch(full, rest, io);
    }
    HANDLE proc = spawn(full, text, io);
    if (!proc) {
        DWORD e = GetLastError();
        puts_h(io->err, e == 193 ? "This version of the file is not compatible with the version of Windows you're running.\n"
                                 : "The system cannot execute the specified program.\n");
        set_error(9009);
        return 9009;
    }
    int rc = wait_exit(proc);
    set_error(rc);
    return rc;
}

static int b_call(const char *a, Io *io)
{
    while (*a == ' ' || *a == '\t') a++;
    if (*a == ':') {
        const char *rest = a;
        char *label = next_arg(&rest);
        while (*rest == ' ') rest++;
        int r = call_label(label, rest, io);
        free(label);
        g_jump = 0;
        return r;
    }
    const char *rest = a;
    char *name = next_arg(&rest);
    if (!name) return 0;
    char full[MAX_PATH];
    if (find_command(name, full) && is_batch(full)) {
        free(name);
        while (*rest == ' ') rest++;
        int r = run_batch(full, rest, io);
        g_jump = 0;
        return r;
    }
    free(name);
    char *again = expand_percent(a);                  /* CALL expands a second time */
    int r = run_line(again, io, 2);
    free(again);
    return r;
}

/* -----------------------------------------------------------------------
 * Redirections
 * ----------------------------------------------------------------------- */
static int is_nul(const char *t) { return ieq(t, "nul") || ieq(t, "nul:"); }

/* Apply @r to @io; handles opened here go to @opened */
static int apply_redirs(Redir *r, Io *io, HANDLE *opened, int *nopened)
{
    for (; r; r = r->next) {
        HANDLE *slot = r->fd == 0 ? &io->in : r->fd == 2 ? &io->err : &io->out;
        if (r->op == '&') {
            *slot = r->dup == 0 ? io->in : r->dup == 2 ? io->err : io->out;
            continue;
        }
        char *target = expand_delayed(r->target);
        HANDLE h;
        if (r->op == '<') {
            h = CreateFileA(is_nul(target) ? "NUL" : target, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (h == INVALID_HANDLE_VALUE) { puts_h(io->err, "The system cannot find the file specified.\n"); free(target); return 0; }
        } else {
            if (is_nul(target)) h = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            else {
                /* opened for appending, so that everything written here and by
                 * the programs it is handed to lands in order */
                if (r->op == '>') {
                    HANDLE t = CreateFileA(target, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, 0, 0);
                    if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
                }
                h = CreateFileA(target, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, 0, 0);
            }
            if (h == INVALID_HANDLE_VALUE) {
                puts_h(io->err, GetLastError() == ERROR_PATH_NOT_FOUND ? "The system cannot find the path specified.\n" : "Access is denied.\n");
                free(target);
                return 0;
            }
        }
        free(target);
        if (*nopened < 16) opened[(*nopened)++] = h;
        *slot = h;
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * IF, FOR
 * ----------------------------------------------------------------------- */
static int is_number(const char *s, long *v)
{
    char *e;
    if (!*s) return 0;
    *v = strtol(s, &e, 0);
    return !*e;
}

static int eval_if(Node *n)
{
    char *r = n->r ? expand_delayed(n->r) : xstrdup("");
    int res = 0;
    switch (n->cond) {
    case 1: res = g_errorlevel >= atoi(r); break;
    case 2: {
        char *p = strip_quotes(r);
        if (strpbrk(p, "*?")) { WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(p, &fd); res = h != INVALID_HANDLE_VALUE; if (res) FindClose(h); }
        else res = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        free(p);
        break;
    }
    case 3: { char *v = getvar(r); res = v != NULL; free(v); break; }
    case 4: res = 2 >= atoi(r); break;
    default: {
        char *l = expand_delayed(n->l);
        if (!strcmp(n->op, "==")) res = n->icase ? !_stricmp(l, r) : !strcmp(l, r);
        else {
            long a, b;
            int c;
            if (is_number(l, &a) && is_number(r, &b)) c = a < b ? -1 : a > b;
            else c = n->icase ? _stricmp(l, r) : strcmp(l, r);
            if (ieq(n->op, "EQU")) res = c == 0;
            else if (ieq(n->op, "NEQ")) res = c != 0;
            else if (ieq(n->op, "LSS")) res = c < 0;
            else if (ieq(n->op, "LEQ")) res = c <= 0;
            else if (ieq(n->op, "GTR")) res = c > 0;
            else if (ieq(n->op, "GEQ")) res = c >= 0;
        }
        free(l);
    }
    }
    free(r);
    return n->neg ? !res : res;
}

/* The FOR body with %X (and %~modsX) replaced; @vals[k] for variable var+k */
static int for_var_ok(char c, void *ctx) { const char *range = ctx; return c >= range[0] && c < range[0] + range[1]; }

static char *subst_for(const char *body, char var, char **vals, int nvals)
{
    Str b = { 0 };
    char range[2] = { var, (char)nvals };
    for (const char *s = body; *s; ) {
        if (*s == '%') {
            if (s[1] >= var && s[1] < var + nvals) { sputs(&b, vals[s[1] - var]); s += 2; continue; }
            char mods[20], t;
            int n = parse_mods(s + 1, mods, sizeof(mods), &t, for_var_ok, range);
            if (n) {
                char *v = apply_mods(mods, vals[t - var]);
                sputs(&b, v);
                free(v);
                s += 1 + n;
                continue;
            }
        }
        sputc(&b, *s++);
    }
    return sdone(&b);
}

static int for_run(Node *n, char **vals, int nvals, Io *io)
{
    char *text = subst_for(n->body, n->var, vals, nvals);
    int r = run_line(text, io, 2);
    free(text);
    return r;
}

/* One FOR item: a word, or files matching a wildcard */
typedef struct { Node *n; Io *io; } ForCtx;
static int for_file(const char *path, void *vctx)
{
    ForCtx *c = vctx;
    char *v = (char *)path;
    for_run(c->n, &v, 1, c->io);
    return g_exit || g_jump;
}

static void for_items(Node *n, const char *set, const char *dir, Io *io)
{
    const char *s = set;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == ',' || *s == ';' || *s == '=') s++;
        if (!*s || g_exit || g_jump) return;
        const char *st = s;
        int q = 0;
        while (*s && (q || !strchr(" \t,;=", *s))) { if (*s == '"') q = !q; s++; }
        char *item = xstrndup(st, (size_t)(s - st));
        if (strpbrk(item, "*?")) {
            char *pat = strip_quotes(item);
            char full[MAX_PATH * 2];
            if (dir) snprintf(full, sizeof(full), "%s\\%s", dir, pat);
            else snprintf(full, sizeof(full), "%s", pat);
            /* results are named as the pattern named them */
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(full, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                char prefix[MAX_PATH * 2];
                snprintf(prefix, sizeof(prefix), "%s", full);
                char *sl = strrchr(prefix, '\\');
                if (sl) sl[1] = 0; else prefix[0] = 0;
                do {
                    int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                    if ((n->mode == 'D') != isdir) continue;
                    char path[MAX_PATH * 3];
                    snprintf(path, sizeof(path), "%s%s", prefix, fd.cFileName);
                    char *v = path;
                    for_run(n, &v, 1, io);
                } while (!g_exit && !g_jump && FindNextFileA(h, &fd));
                FindClose(h);
            }
            free(pat);
        } else {
            char path[MAX_PATH * 3];
            if (dir) { char *p = strip_quotes(item); snprintf(path, sizeof(path), "%s\\%s", dir, p); free(p); }
            else snprintf(path, sizeof(path), "%s", item);
            char *v = path;
            if (dir && !strcmp(item, ".")) { snprintf(path, sizeof(path), "%s\\.", dir); }
            for_run(n, &v, 1, io);
        }
        free(item);
    }
}

static void for_recurse(Node *n, const char *dir, const char *set, Io *io)
{
    for_items(n, set, dir, io);
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
        for_recurse(n, sub, set, io);
    } while (!g_exit && !g_jump && FindNextFileA(h, &fd));
    FindClose(h);
}

/* FOR /F options */
typedef struct { char eol; int skip; char delims[64]; int tokens[32], ntok, rest, backq; } FOpts;

static void parse_fopts(const char *o, FOpts *f)
{
    memset(f, 0, sizeof(*f));
    f->eol = ';';
    strcpy(f->delims, " \t");
    f->tokens[0] = 1;
    f->ntok = 1;
    while (o && *o) {
        while (*o == ' ') o++;
        if (!_strnicmp(o, "eol=", 4)) { f->eol = o[4]; o += o[4] ? 5 : 4; }
        else if (!_strnicmp(o, "skip=", 5)) { f->skip = atoi(o + 5); o += 5; while (isdigit((unsigned char)*o)) o++; }
        else if (!_strnicmp(o, "usebackq", 8)) { f->backq = 1; o += 8; }
        else if (!_strnicmp(o, "delims=", 7)) {
            o += 7;
            int k = 0;
            /* delims runs to the end, or to a space before another option */
            while (*o && k < 63) {
                if (*o == ' ' && (!_strnicmp(o + 1, "tokens=", 7) || !_strnicmp(o + 1, "eol=", 4) || !_strnicmp(o + 1, "skip=", 5) ||
                                  !_strnicmp(o + 1, "usebackq", 8))) break;
                f->delims[k++] = *o++;
            }
            f->delims[k] = 0;
        } else if (!_strnicmp(o, "tokens=", 7)) {
            o += 7;
            f->ntok = 0;
            while (*o && *o != ' ') {
                if (*o == '*') { f->rest = 1; o++; continue; }
                int a = atoi(o);
                while (isdigit((unsigned char)*o)) o++;
                int b = a;
                if (*o == '-') { o++; b = atoi(o); while (isdigit((unsigned char)*o)) o++; }
                for (int t = a; t <= b && f->ntok < 31; t++) f->tokens[f->ntok++] = t;
                if (*o == ',') o++;
            }
        } else o++;
    }
}

static void for_f_line(Node *n, FOpts *f, const char *line, Io *io)
{
    if (!*line || line[0] == f->eol) return;
    /* split into tokens at the delimiters */
    char *copy = xstrdup(line);
    char *tok[64];
    char *starts[64];
    int nt = 0;
    char *p = copy;
    while (*p && nt < 64) {
        while (*p && strchr(f->delims, *p) && *f->delims) p++;
        if (!*p) break;
        starts[nt] = (char *)line + (p - copy);
        tok[nt++] = p;
        while (*p && !(*f->delims && strchr(f->delims, *p))) p++;
        if (*p) *p++ = 0;
    }
    char *vals[33];
    int nv = 0;
    for (int i = 0; i < f->ntok; i++) vals[nv++] = f->tokens[i] - 1 < nt ? tok[f->tokens[i] - 1] : "";
    int last = f->ntok ? f->tokens[f->ntok - 1] : 0;
    if (f->rest) vals[nv++] = last < nt ? starts[last] : "";
    if (!f->ntok && !f->rest) vals[nv++] = (char *)line;
    if (nt || !*f->delims) for_run(n, vals, nv, io);
    free(copy);
}

static void for_f_text(Node *n, FOpts *f, char *text, Io *io)
{
    int skip = f->skip;
    for (char *s = text; *s && !g_exit && !g_jump; ) {
        char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char *line = xstrndup(s, len);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        if (skip > 0) skip--;
        else if (*line) for_f_line(n, f, line, io);
        free(line);
        if (!e) break;
        s = e + 1;
    }
}

/* The output of "cmd /c @command" */
static char *capture(const char *command, Io *io)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, FALSE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return xstrdup("");
    Io cio = { io->in, w, io->err };
    HANDLE proc = spawn_cmd(command, &cio);
    CloseHandle(w);
    Str b = { 0 };
    char buf[4096];
    DWORD got;
    while (ReadFile(r, buf, sizeof(buf), &got, 0) && got) sput(&b, buf, got);
    CloseHandle(r);
    if (proc) wait_exit(proc);
    return sdone(&b);
}

static int exec_for(Node *n, Io *io)
{
    char *set = expand_delayed(n->set);
    if (n->mode == 'L') {
        long v[3] = { 0, 1, 0 };
        const char *s = set;
        for (int i = 0; i < 3; i++) {
            while (*s == ' ' || *s == ',' || *s == '\t') s++;
            char *e;
            v[i] = strtol(s, &e, 0);
            if (e == s) break;
            s = e;
        }
        for (long i = v[0]; v[1] >= 0 ? i <= v[2] : i >= v[2]; i += v[1]) {
            char num[24];
            sprintf(num, "%ld", i);
            char *val = num;
            for_run(n, &val, 1, io);
            if (g_exit || g_jump || !v[1]) break;
        }
    } else if (n->mode == 'F') {
        FOpts f;
        parse_fopts(n->opts, &f);
        const char *s = set;
        while (*s == ' ') s++;
        size_t len = strlen(s);
        while (len && s[len - 1] == ' ') len--;
        char q = *s, cq = len ? s[len - 1] : 0;
        char cmdq = f.backq ? '`' : '\'', strq = f.backq ? '\'' : '"';
        if (len >= 2 && q == cmdq && cq == cmdq) {
            char *c = xstrndup(s + 1, len - 2);
            char *out = capture(c, io);
            for_f_text(n, &f, out, io);
            free(out);
            free(c);
        } else if (len >= 2 && q == strq && cq == strq) {
            char *str = xstrndup(s + 1, len - 2);
            for_f_line(n, &f, str, io);
            free(str);
        } else {
            char *files = xstrndup(s, len);
            const char *p = files;
            char *fname;
            while ((fname = next_arg(&p))) {
                HANDLE h = CreateFileA(fname, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
                if (h == INVALID_HANDLE_VALUE) { printf_h(io->err, "The system cannot find the file %s.\n", fname); free(fname); continue; }
                Str b = { 0 };
                char buf[4096];
                DWORD got;
                while (ReadFile(h, buf, sizeof(buf), &got, 0) && got) sput(&b, buf, got);
                CloseHandle(h);
                char *text = sdone(&b);
                for_f_text(n, &f, text, io);
                free(text);
                free(fname);
            }
            free(files);
        }
    } else if (n->mode == 'R') {
        char root[MAX_PATH];
        GetFullPathNameA(n->root ? n->root : ".", MAX_PATH, root, 0);
        size_t l = strlen(root);
        if (l > 3 && root[l - 1] == '\\') root[l - 1] = 0;
        for_recurse(n, root, set, io);
    } else for_items(n, set, 0, io);
    free(set);
    return g_errorlevel;
}

/* -----------------------------------------------------------------------
 * Executing the tree
 * ----------------------------------------------------------------------- */
static void close_all(HANDLE *h, int n)
{
    for (int i = 0; i < n; i++) {
        if (h[i] == g_inh) { g_inh = 0; g_inlen = g_inpos = 0; }   /* (its value may come back) */
        CloseHandle(h[i]);
    }
}

static int exec_pipe(Node *n, Io *io)
{
    /* the stages, left to right */
    Node *stages[32];
    int ns = 0;
    Node *cur = n;
    while (cur->kind == N_PIPE && ns < 31) { stages[ns++] = cur->b; cur = cur->a; }
    stages[ns++] = cur;
    for (int i = 0; i < ns / 2; i++) { Node *t = stages[i]; stages[i] = stages[ns - 1 - i]; stages[ns - 1 - i] = t; }
    HANDLE procs[32];
    int np = 0;
    HANDLE prev_read = io->in;
    int rc = 0;
    for (int i = 0; i < ns; i++) {
        HANDLE r = 0, w = io->out;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, FALSE };
        if (i < ns - 1 && !CreatePipe(&r, &w, &sa, 0)) { puts_h(io->err, "The pipe could not be created.\n"); break; }
        Io sio = { prev_read, w, io->err };
        Node *st = stages[i];
        HANDLE proc = 0;
        /* a program runs directly; anything else in a child cmd.exe */
        if (st->kind == N_CMD) {
            char *text = expand_delayed(st->text);
            HANDLE opened[16];
            int no = 0;
            if (apply_redirs(st->redir, &sio, opened, &no)) {
                const char *t = text;
                while (*t == ' ') t++;
                const char *args;
                const char *rest = t;
                char *name = next_arg(&rest);
                char full[MAX_PATH];
                if (name && !find_builtin(t, &args) && find_command(name, full) && !is_batch(full)) proc = spawn(full, t, &sio);
                else proc = spawn_cmd(st->src, &sio);
                free(name);
            }
            close_all(opened, no);
            free(text);
        } else proc = spawn_cmd(st->src, &sio);
        if (proc) procs[np++] = proc;
        if (prev_read != io->in) CloseHandle(prev_read);
        if (i < ns - 1) CloseHandle(w);
        prev_read = r;
    }
    for (int i = 0; i < np; i++) {
        int code = wait_exit(procs[i]);
        if (i == np - 1) rc = code;
    }
    set_error(rc);
    return rc;
}

static int exec_node(Node *n, Io *io)
{
    if (!n || g_exit || g_jump) return g_errorlevel;
    switch (n->kind) {
    case N_REM: return g_errorlevel;
    case N_SEQ: exec_node(n->a, io); return exec_node(n->b, io);
    case N_AND: { exec_node(n->a, io); return (g_errorlevel == 0 && !g_exit && !g_jump) ? exec_node(n->b, io) : g_errorlevel; }
    case N_OR:  { exec_node(n->a, io); return (g_errorlevel != 0 && !g_exit && !g_jump) ? exec_node(n->b, io) : g_errorlevel; }
    case N_PIPE: return exec_pipe(n, io);
    case N_IF:  return eval_if(n) ? exec_node(n->a, io) : exec_node(n->b, io);
    case N_FOR: return exec_for(n, io);
    case N_BLOCK: {
        Io bio = *io;
        HANDLE opened[16];
        int no = 0;
        int r = g_errorlevel;
        if (apply_redirs(n->redir, &bio, opened, &no)) r = exec_node(n->a, &bio);
        else set_error(1);
        close_all(opened, no);
        return r;
    }
    case N_CMD: {
        Io cio = *io;
        HANDLE opened[16];
        int no = 0;
        int r;
        char *text = expand_delayed(n->text);
        if (apply_redirs(n->redir, &cio, opened, &no)) r = run_command_text(text, &cio);
        else { set_error(1); r = 1; }
        close_all(opened, no);
        free(text);
        return r;
    }
    }
    return g_errorlevel;
}

/* Expand and run one line.  @from: 0 typed or /c, 1 a batch line,
 * 2 a FOR body (already expanded) */
static int run_line(const char *line, Io *io, int from)
{
    char *text = from == 2 ? xstrdup(line) : expand_percent(line);
    const char *t = text;
    while (*t == ' ' || *t == '\t') t++;
    if (from == 1 && g_echo && *t != '@' && *t) {      /* ECHO ON: show the command */
        puts_h(io->out, "\n");
        print_prompt(io->out);
        puts_h(io->out, t);
        puts_h(io->out, "\n");
    }
    int err;
    Node *n = parse_line(text, &err);
    int r;
    if (err) { puts_h(io->err, "The syntax of the command is incorrect.\n"); set_error(1); r = 1; }
    else r = exec_node(n, io);
    free_node(n);
    free(text);
    return r;
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
static void interactive(Io *io)
{
    g_interactive = 1;
    char line[LINE_MAX_];
    int first = 1;
    while (!g_exit) {
        if (g_echo) {
            if (!first) puts_h(io->out, "\n");
            print_prompt(io->out);
        }
        first = 0;
        int n = read_line(io->in, line, sizeof(line));
        if (n < 0) break;
        run_line(line, io, 0);
    }
}

int main(void)
{
    GetModuleFileNameA(0, g_self, MAX_PATH);
    srand((unsigned)GetTickCount());
    Io io = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    const char *cl = GetCommandLineA();
    g_cmdline = xstrdup(cl);
    if (!getvar("ComSpec")) SetEnvironmentVariableA("ComSpec", g_self);
    /* skip our own name */
    const char *p = cl;
    while (*p == ' ') p++;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    int mode = 0, strip = 0;                           /* mode: 'c', 'k' */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '/') break;
        char s = (char)toupper((unsigned char)p[1]);
        if (s == 'C' || s == 'K') { mode = s; p += 2; break; }
        if (s == 'Q') g_echo = 0;
        else if (s == 'S') strip = 1;
        else if (s == 'V' && p[2] == ':') g_delayed = !_strnicmp(p + 3, "on", 2);
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (!mode) {
        if (!g_echo) {}
        else puts_h(io.out, "NovaOS [Version 10.0.18362]\n(c) NovaOS. Compatible with the Windows command interpreter.\n\n");
        interactive(&io);
        return g_exit ? g_exit_code : g_errorlevel;
    }
    while (*p == ' ' || *p == '\t') p++;
    char *command = xstrdup(p);
    /* quotes: /S, or anything but exactly two quotes around a program, drops
     * the first quote and the last one */
    size_t len = strlen(command);
    if (command[0] == '"') {
        int quotes = 0;
        for (char *c = command; *c; c++) if (*c == '"') quotes++;
        char *last = strrchr(command, '"');
        int keep = !strip && quotes == 2 && !strpbrk(command, "&<>()@^|");
        if (keep) {
            char *inner = xstrndup(command + 1, (size_t)(last - command - 1));
            char full[MAX_PATH];
            keep = strchr(inner, ' ') && find_command(inner, full);
            free(inner);
        }
        if (!keep) {
            if (last != command) memmove(last, last + 1, strlen(last));   /* (a lone quote: only it goes) */
            memmove(command, command + 1, strlen(command));
        }
    }
    run_line(command, &io, 0);
    free(command);
    if (mode == 'K' && !g_exit) interactive(&io);
    return g_exit ? g_exit_code : g_errorlevel;
}
