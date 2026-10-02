/*
 * sql.c — the installer database's query language, behind
 * MsiDatabaseOpenView: the SQL subset Windows Installer understands.
 *
 *   SELECT [DISTINCT] col, ... | * FROM t [, t2 ...] [WHERE expr] [ORDER BY col, ...]
 *   INSERT INTO t (col, ...) VALUES (value, ...) [TEMPORARY]
 *   UPDATE t SET col = value, ... [WHERE expr]
 *   DELETE FROM t [WHERE expr]
 *   CREATE TABLE t (col type [NOT NULL] [TEMPORARY] [LOCALIZABLE], ...
 *                   PRIMARY KEY col, ...) [HOLD]
 *   ALTER TABLE t HOLD | FREE | ADD col type,  DROP TABLE t
 *
 *   expr  := cond (AND | OR cond)*,  cond := col op value | col IS [NOT] NULL | (expr)
 *   value := 'string' | integer | ? (a parameter from MsiViewExecute's record) | col
 *
 * The package itself stays read-only: rows inserted, changed or deleted
 * at run time (custom actions do this) live in an overlay, struct
 * MsiTemp, which every query sees.  Uses only the C library.
 */
#include "msi_int.h"
#include <ctype.h>

#define ERR_SUCCESS        0
#define ERR_NO_MORE_ITEMS  259
#define ERR_FUNCTION_FAILED 1627
#define ERR_BAD_QUERY      1615
#define ERR_INVALID_TABLE  1628
#define ERR_INVALID_DATA   13
#define ERR_INVALID_PARAM  87

/* -----------------------------------------------------------------------
 * Records
 * ----------------------------------------------------------------------- */
MsiRec *msirec_new(int n)
{
    if (n < 0 || n > 65535) return NULL;
    MsiRec *r = calloc(1, sizeof(MsiRec));
    if (!r) return NULL;
    r->n = n;
    r->f = calloc((size_t)n + 1, sizeof(MsiField));
    if (!r->f) { free(r); return NULL; }
    return r;
}

static void field_clear(MsiField *f)
{
    free(f->s);
    free(f->data);
    memset(f, 0, sizeof(*f));
}

void msirec_clear(MsiRec *r)
{
    if (!r) return;
    for (int i = 0; i <= r->n; i++) field_clear(&r->f[i]);
}

void msirec_free(MsiRec *r)
{
    if (!r) return;
    msirec_clear(r);
    free(r->f);
    free(r);
}

void msirec_set_str(MsiRec *r, int i, const char *s)
{
    if (!r || i < 0 || i > r->n) return;
    field_clear(&r->f[i]);
    if (s && *s) { r->f[i].type = MSIF_STR; r->f[i].s = strdup(s); }
}

void msirec_set_int(MsiRec *r, int i, int v)
{
    if (!r || i < 0 || i > r->n) return;
    field_clear(&r->f[i]);
    if (v != MSI_NULL_INTEGER) { r->f[i].type = MSIF_INT; r->f[i].i = v; }
}

void msirec_set_stream(MsiRec *r, int i, const void *data, size_t size)
{
    if (!r || i < 0 || i > r->n) return;
    field_clear(&r->f[i]);
    r->f[i].type = MSIF_STREAM;
    r->f[i].data = malloc(size ? size : 1);
    if (r->f[i].data && size) memcpy(r->f[i].data, data, size);
    r->f[i].size = size;
}

bool msirec_is_null(const MsiRec *r, int i)
{
    return !r || i < 0 || i > r->n || r->f[i].type == MSIF_NULL;
}

int msirec_int(const MsiRec *r, int i)
{
    if (msirec_is_null(r, i)) return MSI_NULL_INTEGER;
    const MsiField *f = &r->f[i];
    if (f->type == MSIF_INT) return f->i;
    if (f->type == MSIF_STR) {
        char *end;
        long v = strtol(f->s, &end, 10);
        if (*f->s && !*end) return (int)v;
    }
    return MSI_NULL_INTEGER;
}

const char *msirec_str(const MsiRec *r, int i, char *buf)
{
    if (msirec_is_null(r, i)) return "";
    const MsiField *f = &r->f[i];
    if (f->type == MSIF_INT) { snprintf(buf, 16, "%d", f->i); return buf; }
    if (f->type == MSIF_STR) return f->s;
    return "";
}

MsiRec *msirec_copy(const MsiRec *r)
{
    MsiRec *c = msirec_new(r->n);
    if (!c) return NULL;
    for (int i = 0; i <= r->n; i++) {
        const MsiField *f = &r->f[i];
        if (f->type == MSIF_INT) msirec_set_int(c, i, f->i);
        else if (f->type == MSIF_STR) msirec_set_str(c, i, f->s);
        else if (f->type == MSIF_STREAM) msirec_set_stream(c, i, f->data, f->size);
    }
    return c;
}

/* -----------------------------------------------------------------------
 * The overlay: per table, deleted package rows and added rows
 * ----------------------------------------------------------------------- */
typedef struct {
    MsiField *v;                      /* one per column */
    bool      dead;
} TRow;

typedef struct TTable {
    char          *name;
    MsiTable      *base;              /* the package's table, or NULL for a created one */
    int            ncols;
    MsiColumn     *cols;
    bool           own_cols;
    bool          *deleted;           /* package rows deleted, base->nrows entries */
    TRow          *rows;
    int            nrows, cap;
    bool           dropped;
    struct TTable *next;
} TTable;

struct MsiTemp { TTable *tables; };

static MsiTable *base_table(MsiDb *db, const char *name)
{
    for (int i = 0; i < db->ntables; i++)
        if (!strcmp(db->tables[i].name, name)) return &db->tables[i];
    return NULL;
}

static TTable *ttable(MsiDb *db, const char *name, bool create_overlay)
{
    if (!db->temp) {
        db->temp = calloc(1, sizeof(struct MsiTemp));
        if (!db->temp) return NULL;
    }
    for (TTable *t = db->temp->tables; t; t = t->next)
        if (!strcmp(t->name, name)) return t->dropped ? NULL : t;
    MsiTable *b = base_table(db, name);
    if (!b) return NULL;
    (void)create_overlay;
    TTable *t = calloc(1, sizeof(TTable));
    if (!t) return NULL;
    t->name = strdup(name);
    t->base = b;
    t->ncols = b->ncols;
    t->cols = b->cols;
    t->deleted = calloc((size_t)b->nrows + 1, 1);
    t->next = db->temp->tables;
    db->temp->tables = t;
    return t;
}

void msisql_free_temp(MsiDb *db)
{
    if (!db->temp) return;
    TTable *t = db->temp->tables;
    while (t) {
        TTable *n = t->next;
        for (int r = 0; r < t->nrows; r++) {
            for (int c = 0; c < t->ncols; c++) field_clear(&t->rows[r].v[c]);
            free(t->rows[r].v);
        }
        free(t->rows);
        if (t->own_cols) {
            for (int c = 0; c < t->ncols; c++) free(t->cols[c].name);
            free(t->cols);
        }
        free(t->deleted);
        free(t->name);
        free(t);
        t = n;
    }
    free(db->temp);
    db->temp = NULL;
}

bool msisql_table_exists(MsiDb *db, const char *table)
{
    return ttable(db, table, false) != NULL;
}

static int base_rows(const TTable *t) { return t->base && t->base->stream ? t->base->nrows : 0; }
static int total_rows(const TTable *t) { return base_rows(t) + t->nrows; }

static bool row_alive(const TTable *t, int r)
{
    int nb = base_rows(t);
    if (r < nb) return !t->deleted[r];
    return !t->rows[r - nb].dead;
}

/* A cell as a field (strings point into the pool or the overlay; not owned) */
static void cell(MsiDb *db, const TTable *t, int r, int c, MsiField *out)
{
    memset(out, 0, sizeof(*out));
    if (c < 0 || c >= t->ncols) return;
    int nb = base_rows(t);
    if (r >= nb) { *out = t->rows[r - nb].v[c]; return; }
    uint16_t type = t->cols[c].type;
    if (MSI_IS_BINARY(type)) { out->type = MSIF_STREAM; return; }   /* loaded when fetched */
    if (type & MSI_STRING) {
        char b[16];
        const char *s = msidb_str(db, t->base, r, c, b);
        if (*s) { out->type = MSIF_STR; out->s = (char *)s; }
        return;
    }
    bool null;
    int v = msidb_int(db, t->base, r, c, &null);
    if (!null) { out->type = MSIF_INT; out->i = v; }
}

static int col_index(const TTable *t, const char *name)
{
    for (int c = 0; c < t->ncols; c++) if (!strcmp(t->cols[c].name, name)) return c;
    return -1;
}

/* The stream behind a binary cell: "Table.Key1.Key2" in the compound file */
static void load_stream(MsiDb *db, const TTable *t, int r, int c, MsiField *out)
{
    int nb = base_rows(t);
    if (r >= nb) {
        const MsiField *f = &t->rows[r - nb].v[c];
        out->type = f->type;
        if (f->type == MSIF_STREAM) {
            out->data = malloc(f->size ? f->size : 1);
            if (out->data) memcpy(out->data, f->data, f->size);
            out->size = f->size;
        }
        return;
    }
    char name[512];
    int n = snprintf(name, sizeof(name), "%s", t->name);
    for (int k = 0; k < t->ncols && n < (int)sizeof(name); k++) {
        if (!(t->cols[k].type & MSI_KEY)) continue;
        char b[16];
        n += snprintf(name + n, sizeof(name) - (size_t)n, ".%s", msidb_str(db, t->base, r, k, b));
    }
    size_t size = 0;
    void *data = cfb_read(&db->cfb, name, &size);
    if (!data) { out->type = MSIF_NULL; return; }
    out->type = MSIF_STREAM;
    out->data = data;
    out->size = size;
}

static int add_row(TTable *t, MsiField *vals)
{
    if (t->nrows == t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        TRow *nr = realloc(t->rows, (size_t)nc * sizeof(TRow));
        if (!nr) return -1;
        t->rows = nr;
        t->cap = nc;
    }
    t->rows[t->nrows].v = vals;
    t->rows[t->nrows].dead = false;
    return base_rows(t) + t->nrows++;
}

static void copy_field(MsiField *dst, const MsiField *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->type = src->type;
    dst->i = src->i;
    if (src->type == MSIF_STR) dst->s = strdup(src->s ? src->s : "");
    if (src->type == MSIF_STREAM && src->data) {
        dst->data = malloc(src->size ? src->size : 1);
        if (dst->data) memcpy(dst->data, src->data, src->size);
        dst->size = src->size;
    }
}

/* A field converted to a column's type */
static void typed_field(const MsiColumn *col, const MsiField *in, MsiField *out)
{
    memset(out, 0, sizeof(*out));
    if (in->type == MSIF_NULL) return;
    if (MSI_IS_BINARY(col->type)) { copy_field(out, in); if (out->type != MSIF_STREAM) field_clear(out); return; }
    if (col->type & MSI_STRING) {
        char b[16];
        const char *s = in->type == MSIF_INT ? (snprintf(b, 16, "%d", in->i), b) : in->s;
        if (s && *s) { out->type = MSIF_STR; out->s = strdup(s); }
        return;
    }
    if (in->type == MSIF_INT) { out->type = MSIF_INT; out->i = in->i; return; }
    if (in->type == MSIF_STR && in->s) {
        char *end;
        long v = strtol(in->s, &end, 10);
        if (*in->s && !*end) { out->type = MSIF_INT; out->i = (int)v; }
    }
}

/* Row with the same primary key as @vals (column order of the table); -1 */
static int find_key(MsiDb *db, const TTable *t, const MsiField *vals)
{
    int total = total_rows(t);
    for (int r = 0; r < total; r++) {
        if (!row_alive(t, r)) continue;
        bool same = true, any = false;
        for (int c = 0; c < t->ncols && same; c++) {
            if (!(t->cols[c].type & MSI_KEY)) continue;
            any = true;
            MsiField f;
            cell(db, t, r, c, &f);
            if (f.type != vals[c].type) same = false;
            else if (f.type == MSIF_INT) same = f.i == vals[c].i;
            else if (f.type == MSIF_STR) same = !strcmp(f.s, vals[c].s);
        }
        if (any && same) return r;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Tokens
 * ----------------------------------------------------------------------- */
enum { T_END, T_ID, T_STR, T_INT, T_PARAM, T_OP, T_PUNCT };

typedef struct {
    int  kind;
    char text[256];
    int  num;
} Tok;

typedef struct {
    const char *p;
    Tok         t;
    bool        error;
    int         nparams;
} Lex;

static void next(Lex *l)
{
    while (*l->p && isspace((unsigned char)*l->p)) l->p++;
    Tok *t = &l->t;
    t->text[0] = 0;
    t->num = 0;
    char c = *l->p;
    if (!c) { t->kind = T_END; return; }
    if (c == '`') {
        const char *s = ++l->p;
        while (*l->p && *l->p != '`') l->p++;
        snprintf(t->text, sizeof(t->text), "%.*s", (int)(l->p - s), s);
        if (*l->p) l->p++;
        t->kind = T_ID;
        return;
    }
    if (c == '\'') {
        const char *s = ++l->p;
        while (*l->p && *l->p != '\'') l->p++;
        snprintf(t->text, sizeof(t->text), "%.*s", (int)(l->p - s), s);
        if (*l->p) l->p++;
        t->kind = T_STR;
        return;
    }
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)l->p[1]))) {
        t->num = (int)strtol(l->p, (char **)&l->p, 10);
        snprintf(t->text, sizeof(t->text), "%d", t->num);
        t->kind = T_INT;
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        const char *s = l->p;
        while (isalnum((unsigned char)*l->p) || *l->p == '_') l->p++;
        snprintf(t->text, sizeof(t->text), "%.*s", (int)(l->p - s), s);
        t->kind = T_ID;
        return;
    }
    if (c == '?') { l->p++; t->kind = T_PARAM; t->num = l->nparams++; return; }
    if (c == '<' || c == '>' || c == '=' || c == '!') {
        int n = (l->p[1] == '=' || (c == '<' && l->p[1] == '>')) ? 2 : 1;
        snprintf(t->text, sizeof(t->text), "%.*s", n, l->p);
        l->p += n;
        t->kind = T_OP;
        return;
    }
    t->text[0] = c;
    t->text[1] = 0;
    l->p++;
    t->kind = T_PUNCT;
}

static bool kw(Lex *l, const char *w)
{
    if (l->t.kind != T_ID) return false;
    const char *a = l->t.text;
    for (; *a && *w; a++, w++) if (toupper((unsigned char)*a) != *w) return false;
    return !*a && !*w;
}

static bool accept_kw(Lex *l, const char *w) { if (kw(l, w)) { next(l); return true; } return false; }
static bool punct(Lex *l, char c) { return l->t.kind == T_PUNCT && l->t.text[0] == c; }
static bool accept(Lex *l, char c) { if (punct(l, c)) { next(l); return true; } return false; }

/* -----------------------------------------------------------------------
 * Queries
 * ----------------------------------------------------------------------- */
enum { Q_SELECT, Q_INSERT, Q_UPDATE, Q_DELETE, Q_CREATE, Q_NOP };
enum { O_NONE, O_COL, O_STR, O_INT, O_PARAM };
enum { E_AND, E_OR, E_CMP, E_NULL, E_NOTNULL };

typedef struct { char table[80], col[80]; int src, idx; } ColRef;

typedef struct {
    int    kind;
    ColRef col;
    char   s[256];
    int    i, param;
} Opnd;

typedef struct Expr {
    int          kind;
    char         op[3];
    struct Expr *l, *r;
    Opnd         a, b;
} Expr;

#define MAX_SRC  4
#define MAX_COLS 64

struct MsiView {
    MsiDb  *db;
    int     kind;
    TTable *src[MAX_SRC];
    int     nsrc;
    ColRef  sel[MAX_COLS];
    int     nsel;
    bool    star, distinct;
    Expr   *where;
    ColRef  order[8];
    int     norder;
    /* INSERT / UPDATE */
    ColRef  set[MAX_COLS];
    Opnd    val[MAX_COLS];
    int     nset;
    /* CREATE */
    char        cname[80];
    MsiColumn   ccols[MAX_COLS];
    int         nccols;
    /* results */
    int   (*res)[MAX_SRC];
    int     nres, pos;
    MsiRec *params;
    bool    executed;
};

static void free_expr(Expr *e)
{
    if (!e) return;
    free_expr(e->l);
    free_expr(e->r);
    free(e);
}

static bool parse_colref(Lex *l, ColRef *c)
{
    memset(c, 0, sizeof(*c));
    if (l->t.kind != T_ID) return false;
    snprintf(c->col, sizeof(c->col), "%s", l->t.text);
    next(l);
    if (accept(l, '.')) {
        if (l->t.kind != T_ID) return false;
        snprintf(c->table, sizeof(c->table), "%s", c->col);
        snprintf(c->col, sizeof(c->col), "%s", l->t.text);
        next(l);
    }
    return true;
}

static bool parse_opnd(Lex *l, Opnd *o)
{
    memset(o, 0, sizeof(*o));
    switch (l->t.kind) {
    case T_STR:   o->kind = O_STR; snprintf(o->s, sizeof(o->s), "%s", l->t.text); next(l); return true;
    case T_INT:   o->kind = O_INT; o->i = l->t.num; next(l); return true;
    case T_PARAM: o->kind = O_PARAM; o->param = l->t.num; next(l); return true;
    case T_ID:
        if (kw(l, "NULL")) { o->kind = O_STR; next(l); return true; }
        o->kind = O_COL;
        return parse_colref(l, &o->col);
    default: return false;
    }
}

static Expr *parse_or(Lex *l);

static Expr *parse_cond(Lex *l)
{
    if (accept(l, '(')) {
        Expr *e = parse_or(l);
        if (!accept(l, ')')) l->error = true;
        return e;
    }
    Expr *e = calloc(1, sizeof(Expr));
    if (!e) { l->error = true; return NULL; }
    if (!parse_opnd(l, &e->a)) { l->error = true; return e; }
    if (accept_kw(l, "IS")) {
        e->kind = accept_kw(l, "NOT") ? E_NOTNULL : E_NULL;
        if (!accept_kw(l, "NULL")) l->error = true;
        return e;
    }
    if (l->t.kind != T_OP) { l->error = true; return e; }
    e->kind = E_CMP;
    snprintf(e->op, sizeof(e->op), "%s", l->t.text);
    next(l);
    if (!parse_opnd(l, &e->b)) l->error = true;
    return e;
}

static Expr *parse_and(Lex *l)
{
    Expr *e = parse_cond(l);
    while (!l->error && accept_kw(l, "AND")) {
        Expr *n = calloc(1, sizeof(Expr));
        if (!n) { l->error = true; break; }
        n->kind = E_AND;
        n->l = e;
        n->r = parse_cond(l);
        e = n;
    }
    return e;
}

static Expr *parse_or(Lex *l)
{
    Expr *e = parse_and(l);
    while (!l->error && accept_kw(l, "OR")) {
        Expr *n = calloc(1, sizeof(Expr));
        if (!n) { l->error = true; break; }
        n->kind = E_OR;
        n->l = e;
        n->r = parse_and(l);
        e = n;
    }
    return e;
}

/* Resolve a column name against the view's tables */
static bool resolve(MsiView *v, ColRef *c)
{
    for (int s = 0; s < v->nsrc; s++) {
        if (c->table[0] && strcmp(c->table, v->src[s]->name)) continue;
        int i = col_index(v->src[s], c->col);
        if (i >= 0) { c->src = s; c->idx = i; return true; }
    }
    return false;
}

static bool resolve_expr(MsiView *v, Expr *e)
{
    if (!e) return true;
    if (e->kind == E_AND || e->kind == E_OR) return resolve_expr(v, e->l) && resolve_expr(v, e->r);
    if (e->a.kind == O_COL && !resolve(v, &e->a.col)) return false;
    if (e->b.kind == O_COL && !resolve(v, &e->b.col)) return false;
    return true;
}

static int parse_type(Lex *l, MsiColumn *c)
{
    /* CHAR(n) | CHARACTER(n) | LONGCHAR | SHORT | INT | INTEGER | LONG | OBJECT */
    int len = 0;
    if (kw(l, "CHAR") || kw(l, "CHARACTER")) {
        next(l);
        if (accept(l, '(')) { if (l->t.kind == T_INT) { len = l->t.num; next(l); } accept(l, ')'); }
        c->type = (uint16_t)(MSI_STRING | 0x100 | (len & 0xFF));
    } else if (accept_kw(l, "LONGCHAR")) {
        c->type = MSI_STRING | 0x100;
    } else if (accept_kw(l, "SHORT") || accept_kw(l, "INT")) {
        c->type = 0x100 | 2;
    } else if (accept_kw(l, "INTEGER") || accept_kw(l, "LONG")) {
        c->type = 0x100 | 4;
    } else if (accept_kw(l, "OBJECT")) {
        c->type = 0x0900;
    } else return -1;
    c->type |= MSI_NULLABLE;
    for (;;) {
        if (accept_kw(l, "NOT")) { if (accept_kw(l, "NULL")) c->type &= (uint16_t)~MSI_NULLABLE; }
        else if (accept_kw(l, "TEMPORARY")) c->type |= 0x4000;
        else if (accept_kw(l, "LOCALIZABLE")) c->type |= 0x200;
        else break;
    }
    return 0;
}

static TTable *create_table(MsiDb *db, const char *name, MsiColumn *cols, int ncols)
{
    if (ttable(db, name, false)) return NULL;
    TTable *t = calloc(1, sizeof(TTable));
    if (!t) return NULL;
    t->name = strdup(name);
    t->ncols = ncols;
    t->cols = calloc((size_t)ncols + 1, sizeof(MsiColumn));
    t->own_cols = true;
    for (int c = 0; c < ncols; c++) {
        t->cols[c] = cols[c];
        t->cols[c].name = strdup(cols[c].name);
    }
    t->next = db->temp->tables;
    db->temp->tables = t;
    return t;
}

static int parse(MsiView *v, const char *sql, char *err, int errcap)
{
    Lex l = { sql, { 0 }, false, 0 };
    next(&l);
    MsiDb *db = v->db;
#define FAIL(code, ...) do { snprintf(err, (size_t)errcap, __VA_ARGS__); return code; } while (0)
    if (accept_kw(&l, "SELECT")) {
        v->kind = Q_SELECT;
        if (accept_kw(&l, "DISTINCT")) v->distinct = true;
        if (accept(&l, '*')) v->star = true;
        else {
            do {
                if (v->nsel >= MAX_COLS || !parse_colref(&l, &v->sel[v->nsel])) FAIL(ERR_BAD_QUERY, "bad column list");
                v->nsel++;
            } while (accept(&l, ','));
        }
        if (!accept_kw(&l, "FROM")) FAIL(ERR_BAD_QUERY, "FROM expected");
        do {
            if (l.t.kind != T_ID || v->nsrc >= MAX_SRC) FAIL(ERR_BAD_QUERY, "table name expected");
            TTable *t = ttable(db, l.t.text, false);
            if (!t) FAIL(ERR_INVALID_TABLE, "no table %s", l.t.text);
            v->src[v->nsrc++] = t;
            next(&l);
        } while (accept(&l, ','));
        if (accept_kw(&l, "WHERE")) {
            v->where = parse_or(&l);
            if (l.error) FAIL(ERR_BAD_QUERY, "bad WHERE clause");
        }
        if (accept_kw(&l, "ORDER")) {
            if (!accept_kw(&l, "BY")) FAIL(ERR_BAD_QUERY, "BY expected");
            do {
                if (v->norder >= 8 || !parse_colref(&l, &v->order[v->norder])) FAIL(ERR_BAD_QUERY, "bad ORDER BY");
                v->norder++;
            } while (accept(&l, ','));
        }
        if (v->star) {
            for (int s = 0; s < v->nsrc; s++)
                for (int c = 0; c < v->src[s]->ncols && v->nsel < MAX_COLS; c++) {
                    ColRef *r = &v->sel[v->nsel++];
                    memset(r, 0, sizeof(*r));
                    snprintf(r->table, sizeof(r->table), "%s", v->src[s]->name);
                    snprintf(r->col, sizeof(r->col), "%s", v->src[s]->cols[c].name);
                }
        }
        for (int i = 0; i < v->nsel; i++) if (!resolve(v, &v->sel[i])) FAIL(ERR_BAD_QUERY, "no column %s", v->sel[i].col);
        for (int i = 0; i < v->norder; i++) if (!resolve(v, &v->order[i])) FAIL(ERR_BAD_QUERY, "no column %s", v->order[i].col);
        if (!resolve_expr(v, v->where)) FAIL(ERR_BAD_QUERY, "unknown column in WHERE");
    } else if (accept_kw(&l, "INSERT")) {
        v->kind = Q_INSERT;
        if (!accept_kw(&l, "INTO") || l.t.kind != T_ID) FAIL(ERR_BAD_QUERY, "INTO table expected");
        TTable *t = ttable(db, l.t.text, false);
        if (!t) FAIL(ERR_INVALID_TABLE, "no table %s", l.t.text);
        v->src[v->nsrc++] = t;
        next(&l);
        if (!accept(&l, '(')) FAIL(ERR_BAD_QUERY, "( expected");
        do {
            if (v->nset >= MAX_COLS || !parse_colref(&l, &v->set[v->nset])) FAIL(ERR_BAD_QUERY, "bad column list");
            if (!resolve(v, &v->set[v->nset])) FAIL(ERR_BAD_QUERY, "no column %s", v->set[v->nset].col);
            v->nset++;
        } while (accept(&l, ','));
        if (!accept(&l, ')') || !accept_kw(&l, "VALUES") || !accept(&l, '(')) FAIL(ERR_BAD_QUERY, "VALUES expected");
        int n = 0;
        do {
            if (n >= MAX_COLS || !parse_opnd(&l, &v->val[n])) FAIL(ERR_BAD_QUERY, "bad value list");
            n++;
        } while (accept(&l, ','));
        if (n != v->nset || !accept(&l, ')')) FAIL(ERR_BAD_QUERY, "column and value counts differ");
        accept_kw(&l, "TEMPORARY");
    } else if (accept_kw(&l, "UPDATE")) {
        v->kind = Q_UPDATE;
        if (l.t.kind != T_ID) FAIL(ERR_BAD_QUERY, "table expected");
        TTable *t = ttable(db, l.t.text, false);
        if (!t) FAIL(ERR_INVALID_TABLE, "no table %s", l.t.text);
        v->src[v->nsrc++] = t;
        next(&l);
        if (!accept_kw(&l, "SET")) FAIL(ERR_BAD_QUERY, "SET expected");
        do {
            if (v->nset >= MAX_COLS || !parse_colref(&l, &v->set[v->nset]) || !resolve(v, &v->set[v->nset]))
                FAIL(ERR_BAD_QUERY, "bad SET column");
            if (l.t.kind != T_OP || strcmp(l.t.text, "=")) FAIL(ERR_BAD_QUERY, "= expected");
            next(&l);
            if (!parse_opnd(&l, &v->val[v->nset])) FAIL(ERR_BAD_QUERY, "bad SET value");
            v->nset++;
        } while (accept(&l, ','));
        if (accept_kw(&l, "WHERE")) {
            v->where = parse_or(&l);
            if (l.error || !resolve_expr(v, v->where)) FAIL(ERR_BAD_QUERY, "bad WHERE clause");
        }
    } else if (accept_kw(&l, "DELETE")) {
        v->kind = Q_DELETE;
        if (!accept_kw(&l, "FROM") || l.t.kind != T_ID) FAIL(ERR_BAD_QUERY, "FROM table expected");
        TTable *t = ttable(db, l.t.text, false);
        if (!t) FAIL(ERR_INVALID_TABLE, "no table %s", l.t.text);
        v->src[v->nsrc++] = t;
        next(&l);
        if (accept_kw(&l, "WHERE")) {
            v->where = parse_or(&l);
            if (l.error || !resolve_expr(v, v->where)) FAIL(ERR_BAD_QUERY, "bad WHERE clause");
        }
    } else if (accept_kw(&l, "CREATE")) {
        v->kind = Q_CREATE;
        if (!accept_kw(&l, "TABLE") || l.t.kind != T_ID) FAIL(ERR_BAD_QUERY, "TABLE name expected");
        snprintf(v->cname, sizeof(v->cname), "%s", l.t.text);
        next(&l);
        if (!accept(&l, '(')) FAIL(ERR_BAD_QUERY, "( expected");
        static char names[MAX_COLS][80];
        for (;;) {
            if (accept_kw(&l, "PRIMARY")) {
                if (!accept_kw(&l, "KEY")) FAIL(ERR_BAD_QUERY, "KEY expected");
                do {
                    if (l.t.kind != T_ID) FAIL(ERR_BAD_QUERY, "key column expected");
                    for (int c = 0; c < v->nccols; c++)
                        if (!strcmp(v->ccols[c].name, l.t.text)) v->ccols[c].type |= MSI_KEY;
                    next(&l);
                } while (accept(&l, ','));
                break;
            }
            if (l.t.kind != T_ID || v->nccols >= MAX_COLS) FAIL(ERR_BAD_QUERY, "column expected");
            MsiColumn *c = &v->ccols[v->nccols];
            snprintf(names[v->nccols], 80, "%s", l.t.text);
            c->name = names[v->nccols];
            next(&l);
            if (parse_type(&l, c)) FAIL(ERR_BAD_QUERY, "bad column type");
            v->nccols++;
            if (!accept(&l, ',') && !kw(&l, "PRIMARY")) break;
        }
        if (!accept(&l, ')')) FAIL(ERR_BAD_QUERY, ") expected");
        accept_kw(&l, "HOLD");
    } else if (accept_kw(&l, "ALTER") || accept_kw(&l, "DROP")) {
        v->kind = Q_NOP;                    /* HOLD/FREE and dropping: nothing to keep */
        return ERR_SUCCESS;
    } else {
        FAIL(ERR_BAD_QUERY, "unknown statement");
    }
    if (l.t.kind != T_END) FAIL(ERR_BAD_QUERY, "unexpected \"%s\"", l.t.text);
    return ERR_SUCCESS;
#undef FAIL
}

int msisql_open(MsiDb *db, const char *sql, MsiView **out, char *err, int errcap)
{
    char dummy[8];
    if (!err) { err = dummy; errcap = sizeof(dummy); }
    err[0] = 0;
    *out = NULL;
    if (!db->temp && !(db->temp = calloc(1, sizeof(struct MsiTemp)))) return ERR_FUNCTION_FAILED;
    MsiView *v = calloc(1, sizeof(MsiView));
    if (!v) return ERR_FUNCTION_FAILED;
    v->db = db;
    int r = parse(v, sql, err, errcap);
    if (r) { msisql_close(v); return r; }
    *out = v;
    return ERR_SUCCESS;
}

void msisql_close(MsiView *v)
{
    if (!v) return;
    free_expr(v->where);
    free(v->res);
    msirec_free(v->params);
    free(v);
}

/* An operand's value for row tuple @rows */
static void opnd_value(MsiView *v, const Opnd *o, const int *rows, MsiField *out, char *buf)
{
    memset(out, 0, sizeof(*out));
    switch (o->kind) {
    case O_COL: cell(v->db, v->src[o->col.src], rows[o->col.src], o->col.idx, out); break;
    case O_STR: if (o->s[0]) { out->type = MSIF_STR; out->s = (char *)o->s; } break;
    case O_INT: out->type = MSIF_INT; out->i = o->i; break;
    case O_PARAM:
        if (v->params && o->param + 1 <= v->params->n) {
            const MsiField *f = &v->params->f[o->param + 1];
            out->type = f->type == MSIF_STREAM ? MSIF_NULL : f->type;
            out->i = f->i;
            out->s = f->s;
        }
        break;
    }
    (void)buf;
}

static int compare(const MsiField *a, const MsiField *b, bool *comparable)
{
    *comparable = true;
    if (a->type == MSIF_INT || b->type == MSIF_INT) {
        long x, y;
        char *end;
        if (a->type == MSIF_INT) x = a->i;
        else if (a->type == MSIF_STR && (x = strtol(a->s, &end, 10), !*end)) ;
        else { *comparable = false; return 0; }
        if (b->type == MSIF_INT) y = b->i;
        else if (b->type == MSIF_STR && (y = strtol(b->s, &end, 10), !*end)) ;
        else { *comparable = false; return 0; }
        return x < y ? -1 : x > y;
    }
    const char *s = a->type == MSIF_STR ? a->s : "", *t = b->type == MSIF_STR ? b->s : "";
    return strcmp(s, t);
}

static bool eval(MsiView *v, const Expr *e, const int *rows)
{
    if (!e) return true;
    switch (e->kind) {
    case E_AND: return eval(v, e->l, rows) && eval(v, e->r, rows);
    case E_OR:  return eval(v, e->l, rows) || eval(v, e->r, rows);
    case E_NULL: case E_NOTNULL: {
        MsiField a;
        char b[16];
        opnd_value(v, &e->a, rows, &a, b);
        return (a.type == MSIF_NULL) == (e->kind == E_NULL);
    }
    default: {
        MsiField a, b;
        char ba[16], bb[16];
        opnd_value(v, &e->a, rows, &a, ba);
        opnd_value(v, &e->b, rows, &b, bb);
        bool ok;
        if ((a.type == MSIF_NULL || b.type == MSIF_NULL) && (a.type == MSIF_INT || b.type == MSIF_INT))
            return !strcmp(e->op, "<>") && a.type != b.type;
        int d = compare(&a, &b, &ok);
        if (!ok) return false;
        if (!strcmp(e->op, "="))  return d == 0;
        if (!strcmp(e->op, "<>") || !strcmp(e->op, "!=")) return d != 0;
        if (!strcmp(e->op, "<"))  return d < 0;
        if (!strcmp(e->op, ">"))  return d > 0;
        if (!strcmp(e->op, "<=")) return d <= 0;
        if (!strcmp(e->op, ">=")) return d >= 0;
        return false;
    }
    }
}

static MsiView *g_sort_view;

static int cmp_rows(const void *pa, const void *pb)
{
    const int *a = pa, *b = pb;
    MsiView *v = g_sort_view;
    for (int i = 0; i < v->norder; i++) {
        MsiField fa, fb;
        cell(v->db, v->src[v->order[i].src], a[v->order[i].src], v->order[i].idx, &fa);
        cell(v->db, v->src[v->order[i].src], b[v->order[i].src], v->order[i].idx, &fb);
        bool ok;
        int d;
        if (fa.type == MSIF_NULL || fb.type == MSIF_NULL) d = (fa.type != MSIF_NULL) - (fb.type != MSIF_NULL);
        else d = compare(&fa, &fb, &ok);
        if (d) return d;
    }
    for (int s = 0; s < MAX_SRC; s++) if (a[s] != b[s]) return a[s] - b[s];
    return 0;
}

static void build_values(MsiView *v, const int *rows, MsiField *vals, TTable *t)
{
    /* the values of an INSERT / UPDATE, typed for the table */
    for (int i = 0; i < v->nset; i++) {
        MsiField f;
        char b[16];
        opnd_value(v, &v->val[i], rows, &f, b);
        if (v->val[i].kind == O_PARAM && v->params && v->val[i].param + 1 <= v->params->n &&
            v->params->f[v->val[i].param + 1].type == MSIF_STREAM)
            f = v->params->f[v->val[i].param + 1];
        field_clear(&vals[v->set[i].idx]);
        typed_field(&t->cols[v->set[i].idx], &f, &vals[v->set[i].idx]);
    }
}

static MsiField *row_copy(MsiDb *db, TTable *t, int r)
{
    MsiField *vals = calloc((size_t)t->ncols + 1, sizeof(MsiField));
    if (!vals) return NULL;
    for (int c = 0; c < t->ncols; c++) {
        if (MSI_IS_BINARY(t->cols[c].type)) { load_stream(db, t, r, c, &vals[c]); continue; }
        MsiField f;
        cell(db, t, r, c, &f);
        copy_field(&vals[c], &f);
    }
    return vals;
}

static void delete_row(TTable *t, int r)
{
    int nb = base_rows(t);
    if (r < nb) t->deleted[r] = true;
    else t->rows[r - nb].dead = true;
}

int msisql_execute(MsiView *v, const MsiRec *params)
{
    msirec_free(v->params);
    v->params = params ? msirec_copy(params) : NULL;
    free(v->res);
    v->res = NULL;
    v->nres = v->pos = 0;
    v->executed = true;
    MsiDb *db = v->db;
    if (v->kind == Q_NOP) return ERR_SUCCESS;
    if (v->kind == Q_CREATE) {
        return create_table(db, v->cname, v->ccols, v->nccols) ? ERR_SUCCESS : ERR_BAD_QUERY;
    }
    if (v->kind == Q_INSERT) {
        TTable *t = v->src[0];
        MsiField *vals = calloc((size_t)t->ncols + 1, sizeof(MsiField));
        if (!vals) return ERR_FUNCTION_FAILED;
        int rows[MAX_SRC] = { 0 };
        build_values(v, rows, vals, t);
        if (find_key(db, t, vals) >= 0) {
            for (int c = 0; c < t->ncols; c++) field_clear(&vals[c]);
            free(vals);
            return ERR_FUNCTION_FAILED;
        }
        return add_row(t, vals) >= 0 ? ERR_SUCCESS : ERR_FUNCTION_FAILED;
    }
    /* SELECT, UPDATE, DELETE: the matching row tuples */
    int cap = 64;
    v->res = malloc((size_t)cap * sizeof(*v->res));
    if (!v->res) return ERR_FUNCTION_FAILED;
    int rows[MAX_SRC] = { 0 };
    int total[MAX_SRC];
    for (int s = 0; s < v->nsrc; s++) total[s] = total_rows(v->src[s]);
    for (int s = 0; s < v->nsrc; s++) if (!total[s]) return ERR_SUCCESS;
    for (;;) {
        bool alive = true;
        for (int s = 0; s < v->nsrc; s++) if (!row_alive(v->src[s], rows[s])) alive = false;
        if (alive && eval(v, v->where, rows)) {
            if (v->nres == cap) {
                cap *= 2;
                void *n = realloc(v->res, (size_t)cap * sizeof(*v->res));
                if (!n) return ERR_FUNCTION_FAILED;
                v->res = n;
            }
            memcpy(v->res[v->nres++], rows, sizeof(rows));
        }
        int s = v->nsrc - 1;
        while (s >= 0 && ++rows[s] >= total[s]) rows[s--] = 0;
        if (s < 0) break;
    }
    if (v->kind == Q_SELECT && v->norder) {
        g_sort_view = v;
        qsort(v->res, (size_t)v->nres, sizeof(*v->res), cmp_rows);
    }
    if (v->kind == Q_DELETE) {
        for (int i = 0; i < v->nres; i++) delete_row(v->src[0], v->res[i][0]);
        v->nres = 0;
    } else if (v->kind == Q_UPDATE) {
        TTable *t = v->src[0];
        int n = v->nres;
        for (int i = 0; i < n; i++) {
            int r = v->res[i][0];
            int nb = base_rows(t);
            if (r >= nb) {
                build_values(v, v->res[i], t->rows[r - nb].v, t);
            } else {
                MsiField *vals = row_copy(db, t, r);
                if (!vals) return ERR_FUNCTION_FAILED;
                build_values(v, v->res[i], vals, t);
                t->deleted[r] = true;
                if (add_row(t, vals) < 0) return ERR_FUNCTION_FAILED;
            }
        }
        v->nres = 0;
    }
    return ERR_SUCCESS;
}

static void fill_record(MsiView *v, const int *rows, MsiRec *r)
{
    msirec_clear(r);
    for (int i = 0; i < v->nsel && i < r->n; i++) {
        TTable *t = v->src[v->sel[i].src];
        int row = rows[v->sel[i].src], c = v->sel[i].idx;
        MsiField *f = &r->f[i + 1];
        if (MSI_IS_BINARY(t->cols[c].type)) { load_stream(v->db, t, row, c, f); continue; }
        MsiField src;
        cell(v->db, t, row, c, &src);
        copy_field(f, &src);
    }
    r->view = v;
    memcpy(r->src, rows, sizeof(r->src));
}

int msisql_fetch(MsiView *v, MsiRec **out)
{
    *out = NULL;
    if (v->kind != Q_SELECT || !v->executed) return ERR_FUNCTION_FAILED;
    for (;;) {
        if (v->pos >= v->nres) return ERR_NO_MORE_ITEMS;
        int *rows = v->res[v->pos++];
        bool alive = true;
        for (int s = 0; s < v->nsrc; s++) if (!row_alive(v->src[s], rows[s])) alive = false;
        if (!alive) continue;                 /* deleted since execute */
        MsiRec *r = msirec_new(v->nsel);
        if (!r) return ERR_FUNCTION_FAILED;
        fill_record(v, rows, r);
        if (v->distinct && v->pos > 1) {
            /* (rare) drop a row equal to the previous one */
            MsiRec *p = msirec_new(v->nsel);
            fill_record(v, v->res[v->pos - 2], p);
            bool same = true;
            for (int i = 1; i <= r->n && same; i++) {
                char b1[16], b2[16];
                same = r->f[i].type == p->f[i].type && !strcmp(msirec_str(r, i, b1), msirec_str(p, i, b2));
            }
            msirec_free(p);
            if (same) { msirec_free(r); continue; }
        }
        *out = r;
        return ERR_SUCCESS;
    }
}

static void type_string(const MsiColumn *c, char *out)
{
    bool nullable = c->type & MSI_NULLABLE;
    if (MSI_IS_BINARY(c->type)) { strcpy(out, nullable ? "V0" : "v0"); return; }
    if (c->type & MSI_STRING) {
        char l = (c->type & 0x200) ? (nullable ? 'L' : 'l') : (nullable ? 'S' : 's');
        sprintf(out, "%c%d", l, c->type & 0xFF);
        return;
    }
    sprintf(out, "%c%d", nullable ? 'I' : 'i', (c->type & 0xFF) == 4 ? 4 : 2);
}

int msisql_colinfo(MsiView *v, bool types, MsiRec **out)
{
    *out = NULL;
    if (v->kind != Q_SELECT) return ERR_FUNCTION_FAILED;
    MsiRec *r = msirec_new(v->nsel);
    if (!r) return ERR_FUNCTION_FAILED;
    for (int i = 0; i < v->nsel; i++) {
        const MsiColumn *c = &v->src[v->sel[i].src]->cols[v->sel[i].idx];
        if (types) { char t[16]; type_string(c, t); msirec_set_str(r, i + 1, t); }
        else msirec_set_str(r, i + 1, c->name);
    }
    *out = r;
    return ERR_SUCCESS;
}

/* MSIMODIFY_* */
#define MOD_SEEK -1
#define MOD_REFRESH 0
#define MOD_INSERT 1
#define MOD_UPDATE 2
#define MOD_ASSIGN 3
#define MOD_REPLACE 4
#define MOD_MERGE 5
#define MOD_DELETE 6
#define MOD_INSERT_TEMPORARY 7

int msisql_modify(MsiView *v, int mode, MsiRec *rec)
{
    if (!rec || v->kind != Q_SELECT) return ERR_INVALID_PARAM;
    if (mode >= 8 && mode <= 11) return ERR_SUCCESS;          /* VALIDATE*: everything is valid */
    MsiDb *db = v->db;
    if (mode == MOD_REFRESH) {
        if (rec->view != v) return ERR_FUNCTION_FAILED;
        int rows[4];
        memcpy(rows, rec->src, sizeof(rows));
        fill_record(v, rows, rec);
        return ERR_SUCCESS;
    }
    if (v->nsrc != 1) return ERR_FUNCTION_FAILED;
    TTable *t = v->src[0];
    /* the record's values in table column order */
    MsiField *vals = calloc((size_t)t->ncols + 1, sizeof(MsiField));
    if (!vals) return ERR_FUNCTION_FAILED;
    int existing = -1;
    if ((mode == MOD_UPDATE || mode == MOD_DELETE) && rec->view == v) existing = rec->src[0];
    if (existing >= 0 && !row_alive(t, existing)) existing = -1;
    if (mode == MOD_UPDATE && existing >= 0) {
        MsiField *old = row_copy(db, t, existing);
        if (old) { free(vals); vals = old; }
    }
    for (int i = 0; i < v->nsel && i < rec->n; i++) {
        field_clear(&vals[v->sel[i].idx]);
        typed_field(&t->cols[v->sel[i].idx], &rec->f[i + 1], &vals[v->sel[i].idx]);
    }
    int found = existing >= 0 ? existing : find_key(db, t, vals);
    int r = ERR_SUCCESS;
    switch (mode) {
    case MOD_SEEK:
        if (found < 0) { r = ERR_FUNCTION_FAILED; break; }
        {
            int rows[4] = { found, 0, 0, 0 };
            fill_record(v, rows, rec);
        }
        break;
    case MOD_INSERT: case MOD_INSERT_TEMPORARY:
        if (found >= 0) { r = ERR_FUNCTION_FAILED; break; }
        if (add_row(t, vals) < 0) r = ERR_FUNCTION_FAILED;
        vals = NULL;
        break;
    case MOD_ASSIGN: case MOD_REPLACE: case MOD_MERGE: case MOD_UPDATE:
        if (mode == MOD_UPDATE && found < 0) { r = ERR_FUNCTION_FAILED; break; }
        if (found >= 0) delete_row(t, found);
        if (add_row(t, vals) < 0) r = ERR_FUNCTION_FAILED;
        vals = NULL;
        break;
    case MOD_DELETE:
        if (found < 0) { r = ERR_FUNCTION_FAILED; break; }
        delete_row(t, found);
        break;
    default:
        r = ERR_INVALID_DATA;
    }
    if (vals) {
        for (int c = 0; c < t->ncols; c++) field_clear(&vals[c]);
        free(vals);
    }
    return r;
}

int msisql_primary_keys(MsiDb *db, const char *table, MsiRec **out)
{
    *out = NULL;
    if (!db->temp && !(db->temp = calloc(1, sizeof(struct MsiTemp)))) return ERR_FUNCTION_FAILED;
    TTable *t = ttable(db, table, false);
    if (!t) return ERR_INVALID_TABLE;
    int n = 0;
    for (int c = 0; c < t->ncols; c++) if (t->cols[c].type & MSI_KEY) n++;
    MsiRec *r = msirec_new(n);
    if (!r) return ERR_FUNCTION_FAILED;
    msirec_set_str(r, 0, table);
    int k = 1;
    for (int c = 0; c < t->ncols; c++) if (t->cols[c].type & MSI_KEY) msirec_set_str(r, k++, t->cols[c].name);
    *out = r;
    return ERR_SUCCESS;
}
