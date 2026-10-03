/*
 * msidb.c — the installer database: the string pool and the tables
 *
 * Every string in the package lives once in !_StringData, indexed by
 * !_StringPool (length, reference count per id).  !_Tables lists the
 * tables, !_Columns their columns, and each table's own stream holds its
 * rows column-major: 2 bytes per string reference (3 for large pools),
 * 2 or 4 bytes per integer stored offset by 0x8000 / 0x80000000 so that
 * zero means null.
 */
#include "msi_int.h"

/* A compound file shared by the storages opened in it (a patch's transforms) */
struct MsiFile {
    uint8_t *data;
    size_t   size;
    Cfb      cfb;
    int      refs;
};

/* A storage whose streams count as the database's */
struct MsiOverlay { Cfb view; MsiFile *file; };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

/* The pool's text as UTF-8: code page 65001 is passed through, anything
 * else is treated as Latin-1 (close enough for 1252 in file names) */
static char *pool_string(const uint8_t *data, size_t len, int codepage)
{
    if (codepage == 65001 || codepage == 0) {
        char *s = malloc(len + 1);
        if (!s) return NULL;
        memcpy(s, data, len);
        s[len] = '\0';
        return s;
    }
    char *s = malloc(len * 2 + 1);
    if (!s) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned c = data[i];
        if (c < 0x80) s[n++] = (char)c;
        else { s[n++] = (char)(0xC0 | (c >> 6)); s[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    s[n] = '\0';
    return s;
}

static bool load_strings(MsiDb *db)
{
    size_t psz, dsz;
    uint8_t *pool = cfb_read(&db->cfb, "!_StringPool", &psz);
    uint8_t *data = cfb_read(&db->cfb, "!_StringData", &dsz);
    if (!pool || !data || psz < 4) { free(pool); free(data); return false; }
    int count = (int)(psz / 4);
    db->codepage = rd16(pool) & 0x7FFF;
    db->strref_size = (rd16(pool + 2) & 0x8000) ? 3 : 2;
    db->strings = calloc((size_t)count + 1, sizeof(char *));
    if (!db->strings) { free(pool); free(data); return false; }
    db->nstrings = count;
    size_t off = 0;
    db->strings[0] = "";
    int id = 1;
    for (int i = 1; i < count; id++) {
        uint32_t len = rd16(pool + 4 * i);
        uint16_t refs = rd16(pool + 4 * i + 2);
        if (!len && refs && i + 1 < count) {
            /* a string of 64 KB or more takes two entries but one id: this
             * one is zero-length with a nonzero count, the next holds the
             * low word of the length and the high word in its count */
            len = ((uint32_t)rd16(pool + 4 * (i + 1) + 2) << 16) | rd16(pool + 4 * (i + 1));
            i += 2;
        } else {
            i++;
        }
        if (!len) { db->strings[id] = ""; continue; }
        if (off + len > dsz) { db->strings[id] = ""; continue; }
        db->strings[id] = pool_string(data + off, len, db->codepage);
        if (!db->strings[id]) db->strings[id] = "";
        off += len;
    }
    db->nstrings = id;
    free(pool);
    free(data);
    return true;
}

static const char *str_at(const MsiDb *db, uint32_t id)
{
    return (id < (uint32_t)db->nstrings && db->strings[id]) ? db->strings[id] : "";
}

/* Raw cell value: string id or offset integer; 0 = null */
static uint32_t raw_cell(const MsiDb *db, const MsiTable *t, int row, int col)
{
    (void)db;
    if (row < 0 || row >= t->nrows || col < 0 || col >= t->ncols) return 0;
    size_t off = 0;
    for (int c = 0; c < col; c++) off += (size_t)t->cols[c].width * t->nrows;
    off += (size_t)t->cols[col].width * row;
    const uint8_t *p = t->stream + off;
    switch (t->cols[col].width) {
    case 2:  return rd16(p);
    case 3:  return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
    case 4:  return rd32(p);
    default: return 0;
    }
}

static int col_width(const MsiDb *db, uint16_t type)
{
    if (MSI_IS_BINARY(type)) return 2;             /* a stream reference */
    if (type & MSI_STRING) return db->strref_size;
    return (type & 0xFF) == 4 ? 4 : 2;
}

static void finish_table(MsiDb *db, MsiTable *t)
{
    char sname[128];
    snprintf(sname, sizeof(sname), "!%s", t->name);
    t->stream = cfb_read(&db->cfb, sname, &t->stream_size);
    size_t rowsz = 0;
    for (int c = 0; c < t->ncols; c++) rowsz += (size_t)t->cols[c].width;
    t->nrows = (t->stream && rowsz) ? (int)(t->stream_size / rowsz) : 0;
}

bool msidb_open(MsiDb *db, const void *data, size_t size)
{
    memset(db, 0, sizeof(*db));
    if (!cfb_open(&db->cfb, data, size)) return false;
    if (!load_strings(db)) { msidb_close(db); return false; }

    /* _Columns has a fixed shape: Table s, Number i2, Name s, Type i2 */
    MsiTable cols;
    memset(&cols, 0, sizeof(cols));
    MsiColumn cc[4] = {
        { "Table", MSI_STRING, db->strref_size }, { "Number", 2, 2 },
        { "Name", MSI_STRING, db->strref_size },  { "Type", 2, 2 },
    };
    cols.name = "_Columns";
    cols.ncols = 4;
    cols.cols = cc;
    finish_table(db, &cols);
    if (!cols.stream) { msidb_close(db); return false; }

    /* _Tables: one string column */
    MsiTable tabs;
    memset(&tabs, 0, sizeof(tabs));
    MsiColumn tc[1] = { { "Name", MSI_STRING, db->strref_size } };
    tabs.name = "_Tables";
    tabs.ncols = 1;
    tabs.cols = tc;
    finish_table(db, &tabs);

    db->tables = calloc((size_t)tabs.nrows + 1, sizeof(MsiTable));
    if (!db->tables) { free(cols.stream); free(tabs.stream); msidb_close(db); return false; }
    for (int i = 0; i < tabs.nrows; i++) {
        MsiTable *t = &db->tables[db->ntables];
        const char *name = str_at(db, raw_cell(db, &tabs, i, 0));
        t->name = strdup(name);
        /* its columns, in Number order (they are stored sorted by table) */
        int n = 0;
        for (int r = 0; r < cols.nrows; r++)
            if (!strcmp(str_at(db, raw_cell(db, &cols, r, 0)), name)) n++;
        t->cols = calloc((size_t)n + 1, sizeof(MsiColumn));
        if (!t->name || !t->cols) continue;
        for (int r = 0; r < cols.nrows; r++) {
            if (strcmp(str_at(db, raw_cell(db, &cols, r, 0)), name)) continue;
            int num = (int)((raw_cell(db, &cols, r, 1) - 0x8000) & 0xFFFF);
            if (num < 1 || num > n) continue;
            MsiColumn *c = &t->cols[num - 1];
            c->name = strdup(str_at(db, raw_cell(db, &cols, r, 2)));
            c->type = (uint16_t)((raw_cell(db, &cols, r, 3) - 0x8000) & 0xFFFF);
            c->width = col_width(db, c->type);
        }
        for (int c = 0; c < n; c++) if (!t->cols[c].name) t->cols[c].name = strdup("");
        t->ncols = n;
        finish_table(db, t);
        db->ntables++;
    }
    free(cols.stream);
    free(tabs.stream);
    return true;
}

void msidb_close(MsiDb *db)
{
    for (int i = 0; i < db->ntables; i++) {
        MsiTable *t = &db->tables[i];
        for (int c = 0; c < t->ncols; c++) free(t->cols[c].name);
        free(t->cols);
        free(t->name);
        free(t->stream);
    }
    free(db->tables);
    for (int i = 0; i < db->noverlays; i++) msifile_release(db->overlays[i].file);
    free(db->overlays);
    free(db->hash);
    if (db->strings) {
        for (int i = 1; i < db->nstrings; i++)
            if (db->strings[i] && db->strings[i][0]) free(db->strings[i]);
        free(db->strings);
    }
    cfb_close(&db->cfb);
    memset(db, 0, sizeof(*db));
}

MsiTable *msidb_table(MsiDb *db, const char *name)
{
    for (int i = 0; i < db->ntables; i++)
        if (!strcmp(db->tables[i].name, name)) return db->tables[i].stream ? &db->tables[i] : NULL;
    return NULL;
}

int msidb_col(const MsiTable *t, const char *name)
{
    for (int c = 0; t && c < t->ncols; c++)
        if (!strcmp(t->cols[c].name, name)) return c;
    return -1;
}

const char *msidb_str(const MsiDb *db, const MsiTable *t, int row, int col, char *buf)
{
    if (col < 0 || !t) return "";
    uint32_t v = raw_cell(db, t, row, col);
    if (MSI_IS_BINARY(t->cols[col].type)) return "";
    if (t->cols[col].type & MSI_STRING) return str_at(db, v);
    if (!v) return "";
    bool null;
    snprintf(buf, 16, "%d", msidb_int(db, t, row, col, &null));
    return buf;
}

int msidb_int(const MsiDb *db, const MsiTable *t, int row, int col, bool *null)
{
    if (col < 0 || !t) { if (null) *null = true; return 0; }
    uint32_t v = raw_cell(db, t, row, col);
    if (null) *null = v == 0;
    if (!v) return 0;
    if (t->cols[col].width == 4) return (int)(v ^ 0x80000000u);
    return (int)(int16_t)((v - 0x8000) & 0xFFFF);
}

int msidb_find(const MsiDb *db, const MsiTable *t, int col, const char *value, int from)
{
    char b[16];
    for (int r = from; t && r < t->nrows; r++)
        if (!strcmp(msidb_str(db, t, r, col, b), value)) return r;
    return -1;
}

/* -----------------------------------------------------------------------
 * Changing the tables: transforms (.mst, and the ones in a patch)
 *
 * A transform is a storage with its own string pool and, for each table it
 * changes, a stream of rows, row by row: a 16-bit mask, then cells.  Mask
 * bit 0 set: an inserted row of (mask >> 8) columns.  Mask 0: the row whose
 * keys follow is deleted.  Any other mask: the key cells, then the cells
 * of the columns whose bits are set, which replace the row's.  !_Columns
 * and !_Tables in it add (and drop) tables.  A table changed here is
 * decoded to cells, changed and written back column-major, so the readers
 * above see one kind of table.
 * ----------------------------------------------------------------------- */

MsiFile *msifile_load(void *data, size_t size)
{
    MsiFile *f = calloc(1, sizeof(MsiFile));
    if (!f) { free(data); return NULL; }
    f->data = data;
    f->size = size;
    f->refs = 1;
    if (!cfb_open(&f->cfb, data, size)) { free(data); free(f); return NULL; }
    return f;
}

const Cfb *msifile_cfb(const MsiFile *f) { return &f->cfb; }

void msifile_release(MsiFile *f)
{
    if (!f || --f->refs > 0) return;
    cfb_close(&f->cfb);
    free(f->data);
    free(f);
}


bool msidb_add_streams(MsiDb *db, MsiFile *f, const char *storage)
{
    Cfb view;
    if (storage) { if (!cfb_open_storage(&f->cfb, storage, &view)) return false; }
    else { view = f->cfb; view.borrowed = true; }
    struct MsiOverlay *o = realloc(db->overlays, (size_t)(db->noverlays + 1) * sizeof(*o));
    if (!o) return false;
    db->overlays = o;
    o[db->noverlays].view = view;
    o[db->noverlays].file = f;
    db->noverlays++;
    f->refs++;
    return true;
}

void *msidb_read_stream(const MsiDb *db, const char *name, size_t *size)
{
    for (int i = db->noverlays - 1; i >= 0; i--) {         /* the newest first: a patch's file wins */
        void *d = cfb_read(&db->overlays[i].view, name, size);
        if (d) return d;
    }
    return cfb_read(&db->cfb, name, size);
}

/* Summary information properties (the "\005SummaryInformation" set):
 * strings into @str, integers into @ival; false if the property is absent */
bool msi_suminfo_get(const Cfb *c, int pid, char *str, int cap, int *ival)
{
    size_t size = 0;
    uint8_t *d = cfb_read(c, "\005SummaryInformation", &size);
    bool found = false;
    if (str && cap) str[0] = 0;
    if (d && size >= 48) {
        uint32_t off = rd32(d + 44);
        uint32_t count = off + 8 <= size ? rd32(d + off + 4) : 0;
        for (uint32_t i = 0; i < count && off + 16 + i * 8 <= size; i++) {
            if (rd32(d + off + 8 + i * 8) != (uint32_t)pid) continue;
            uint32_t po = off + rd32(d + off + 12 + i * 8);
            if (po + 8 > size) break;
            uint32_t type = rd32(d + po);
            if (type == 30) {
                uint32_t n = rd32(d + po + 4);
                if (po + 8 + n > size) break;
                if (str && cap) {
                    int k = (int)n < cap - 1 ? (int)n : cap - 1;
                    memcpy(str, d + po + 8, (size_t)k);
                    str[k] = 0;
                }
                found = true;
            } else if (type == 3 || type == 2) {
                if (ival) *ival = type == 3 ? (int)rd32(d + po + 4) : (int16_t)rd16(d + po + 4);
                found = true;
            }
            break;
        }
    }
    free(d);
    return found;
}

/* Strings: interning through a hash of the pool (built on first use) */
static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void hash_put(MsiDb *db, int id)
{
    uint32_t m = (uint32_t)db->hash_cap - 1, h = str_hash(db->strings[id]) & m;
    while (db->hash[h]) h = (h + 1) & m;
    db->hash[h] = id;
}

static bool hash_build(MsiDb *db, int need)
{
    int cap = 64;
    while (cap < need * 2) cap *= 2;
    int *nh = calloc((size_t)cap, sizeof(int));
    if (!nh) return false;
    free(db->hash);
    db->hash = nh;
    db->hash_cap = cap;
    for (int i = 1; i < db->nstrings; i++) if (db->strings[i] && db->strings[i][0]) hash_put(db, i);
    return true;
}

/* The id of @s in @db's pool, added when it is new; 0 for "" */
static uint32_t intern(MsiDb *db, const char *s)
{
    if (!s || !*s) return 0;
    if ((!db->hash || db->nstrings * 2 >= db->hash_cap) && !hash_build(db, db->nstrings + 64)) return 0;
    uint32_t m = (uint32_t)db->hash_cap - 1, h = str_hash(s) & m;
    for (; db->hash[h]; h = (h + 1) & m)
        if (!strcmp(db->strings[db->hash[h]], s)) return (uint32_t)db->hash[h];
    char **ns = realloc(db->strings, (size_t)(db->nstrings + 1) * sizeof(char *));
    if (!ns) return 0;
    db->strings = ns;
    db->strings[db->nstrings] = strdup(s);
    if (!db->strings[db->nstrings]) return 0;
    db->hash[h] = db->nstrings;
    return (uint32_t)db->nstrings++;
}

/* A table as cells, row by row (the raw values: string ids, offset integers) */
static uint32_t *table_cells(const MsiDb *db, const MsiTable *t, int extra)
{
    uint32_t *c = calloc((size_t)(t->nrows + extra) * (size_t)t->ncols + 1, sizeof(uint32_t));
    if (!c) return NULL;
    for (int r = 0; r < t->nrows; r++)
        for (int k = 0; k < t->ncols; k++) c[(size_t)r * t->ncols + k] = raw_cell(db, t, r, k);
    return c;
}

/* ... and back into the table's stream, column-major */
static bool table_store(MsiTable *t, const uint32_t *cells, int nrows)
{
    size_t rowsz = 0;
    for (int k = 0; k < t->ncols; k++) rowsz += (size_t)t->cols[k].width;
    uint8_t *s = malloc(rowsz * (size_t)nrows + 1);
    if (!s) return false;
    size_t off = 0;
    for (int k = 0; k < t->ncols; k++)
        for (int r = 0; r < nrows; r++) {
            uint32_t v = cells[(size_t)r * t->ncols + k];
            for (int b = 0; b < t->cols[k].width; b++) s[off++] = (uint8_t)(v >> (8 * b));
        }
    free(t->stream);
    t->stream = s;
    t->stream_size = off;
    t->nrows = nrows;
    return true;
}

/* A pool past 65535 strings needs 3-byte references in every table */
static bool widen_strings(MsiDb *db)
{
    if (db->strref_size == 3 || db->nstrings <= 0xFFFF) return true;
    for (int i = 0; i < db->ntables; i++) {
        MsiTable *t = &db->tables[i];
        uint32_t *c = table_cells(db, t, 0);
        if (!c) return false;
        for (int k = 0; k < t->ncols; k++)
            if ((t->cols[k].type & MSI_STRING) && !MSI_IS_BINARY(t->cols[k].type)) t->cols[k].width = 3;
        bool ok = table_store(t, c, t->nrows);
        free(c);
        if (!ok) return false;
    }
    db->strref_size = 3;
    return true;
}

static MsiTable *add_table(MsiDb *db, const char *name)
{
    MsiTable *nt = realloc(db->tables, (size_t)(db->ntables + 1) * sizeof(MsiTable));
    if (!nt) return NULL;
    db->tables = nt;
    MsiTable *t = &db->tables[db->ntables++];
    memset(t, 0, sizeof(*t));
    t->name = strdup(name);
    t->stream = malloc(1);
    return t;
}

static void drop_table(MsiDb *db, MsiTable *t)
{
    for (int c = 0; c < t->ncols; c++) free(t->cols[c].name);
    free(t->cols);
    free(t->name);
    free(t->stream);
    int i = (int)(t - db->tables);
    memmove(&db->tables[i], &db->tables[i + 1], (size_t)(db->ntables - i - 1) * sizeof(MsiTable));
    db->ntables--;
}

/* Any table, including one listed without a stream (empty) */
static MsiTable *find_table(MsiDb *db, const char *name)
{
    for (int i = 0; i < db->ntables; i++) if (!strcmp(db->tables[i].name, name)) return &db->tables[i];
    return NULL;
}

/* A transform being applied: its string pool and its stream reader */
typedef struct {
    MsiDb       *db;
    MsiDb        x;                  /* (only the pool is loaded) */
    const Cfb   *cfb;
    int          suppress;           /* MSITRANSFORM_ERROR_* to ignore */
    char        *err;
    int          errcap;
    /* new tables' columns from !_Columns, until !_Tables creates them */
    struct PendingCol { char *table, *name; int num; uint16_t type; } *pend;
    int          npend;
} Xf;

#define XF_ADDEXISTINGROW   0x01
#define XF_DELMISSINGROW    0x02
#define XF_ADDEXISTINGTABLE 0x04
#define XF_DELMISSINGTABLE  0x08
#define XF_UPDATEMISSINGROW 0x10

static int xf_error(Xf *x, int kind, const char *fmt, const char *a, const char *b)
{
    if (x->suppress & kind) return 0;
    snprintf(x->err, (size_t)x->errcap, fmt, a, b ? b : "");
    return 1624;                    /* ERROR_INSTALL_TRANSFORM_FAILURE */
}

/* One cell of a transform row: its db value (strings re-interned) */
static bool xf_cell(Xf *x, const MsiColumn *col, const uint8_t *p, size_t len, size_t *pos, uint32_t *out)
{
    int w = MSI_IS_BINARY(col->type) ? 2 : (col->type & MSI_STRING) ? x->x.strref_size : ((col->type & 0xFF) == 4 ? 4 : 2);
    if (*pos + (size_t)w > len) return false;
    uint32_t v = 0;
    for (int b = 0; b < w; b++) v |= (uint32_t)p[*pos + (size_t)b] << (8 * b);
    *pos += (size_t)w;
    if ((col->type & MSI_STRING) && !MSI_IS_BINARY(col->type)) v = intern(x->db, str_at(&x->x, v));
    *out = v;
    return true;
}

static bool same_keys(const MsiDb *db, const MsiTable *t, const uint32_t *a, const uint32_t *b)
{
    for (int k = 0; k < t->ncols; k++) {
        if (!(t->cols[k].type & MSI_KEY)) continue;
        if ((t->cols[k].type & MSI_STRING) && !MSI_IS_BINARY(t->cols[k].type)) {
            if (strcmp(str_at(db, a[k]), str_at(db, b[k]))) return false;
        } else if (a[k] != b[k]) return false;
    }
    return true;
}

static void key_text(const MsiDb *db, const MsiTable *t, const uint32_t *row, char *out, int cap)
{
    int n = 0;
    out[0] = 0;
    for (int k = 0; k < t->ncols && n < cap - 1; k++) {
        if (!(t->cols[k].type & MSI_KEY)) continue;
        char b[16];
        const char *s = (t->cols[k].type & MSI_STRING) ? str_at(db, row[k]) : (snprintf(b, sizeof(b), "%d",
                         t->cols[k].width == 4 ? (int)(row[k] ^ 0x80000000u) : (int)(row[k] & 0xFFFF) - 0x8000), b);
        n += snprintf(out + n, (size_t)(cap - n), "%s%s", n ? "." : "", s);
    }
}

/* Apply the rows of one table's transform stream */
static int xf_table(Xf *x, MsiTable *t, const uint8_t *p, size_t len)
{
    int cap = t->nrows + 16;
    uint32_t *cells = table_cells(x->db, t, cap - t->nrows);
    if (!cells) return 8;
    int nrows = t->nrows, nc = t->ncols, rc = 0;
    uint32_t *row = calloc((size_t)nc + 1, sizeof(uint32_t));
    size_t pos = 0;
    while (!rc && pos + 2 <= len) {
        unsigned mask = rd16(p + pos);
        pos += 2;
        memset(row, 0, (size_t)nc * sizeof(uint32_t));
        bool insert = mask & 1, ok = true;
        bool present[64] = { 0 };
        int ncols_in = insert ? (int)(mask >> 8) : nc;
        if (ncols_in > nc) { snprintf(x->err, (size_t)x->errcap, "%s: a row with %d columns for %d", t->name, ncols_in, nc); rc = 1624; break; }
        for (int k = 0; k < ncols_in && ok; k++) {
            bool here = insert || (t->cols[k].type & MSI_KEY) || (k < 16 && (mask & (1u << k)));
            if (!here) continue;
            ok = xf_cell(x, &t->cols[k], p, len, &pos, &row[k]);
            if (k < 64) present[k] = true;
        }
        if (!ok) { snprintf(x->err, (size_t)x->errcap, "%s: the transform's rows are cut short", t->name); rc = 1624; break; }
        int at = -1;
        for (int r = 0; r < nrows; r++) if (same_keys(x->db, t, &cells[(size_t)r * nc], row)) { at = r; break; }
        char key[256];
        key_text(x->db, t, row, key, sizeof(key));
        if (insert) {
            if (at >= 0) { rc = xf_error(x, XF_ADDEXISTINGROW, "%s: row %s exists already", t->name, key); continue; }
            if (nrows == cap) {
                cap *= 2;
                uint32_t *nc2 = realloc(cells, (size_t)cap * (size_t)nc * sizeof(uint32_t));
                if (!nc2) { rc = 8; break; }
                cells = nc2;
            }
            memcpy(&cells[(size_t)nrows * nc], row, (size_t)nc * sizeof(uint32_t));
            nrows++;
        } else if (mask == 0) {
            if (at < 0) { rc = xf_error(x, XF_DELMISSINGROW, "%s: no row %s to delete", t->name, key); continue; }
            memmove(&cells[(size_t)at * nc], &cells[(size_t)(at + 1) * nc], (size_t)(nrows - at - 1) * (size_t)nc * sizeof(uint32_t));
            nrows--;
        } else {
            if (at < 0) { rc = xf_error(x, XF_UPDATEMISSINGROW, "%s: no row %s to change", t->name, key); continue; }
            for (int k = 0; k < nc && k < 64; k++)
                if (present[k] && !(t->cols[k].type & MSI_KEY)) cells[(size_t)at * nc + k] = row[k];
        }
    }
    free(row);
    if (!rc && !table_store(t, cells, nrows)) rc = 8;
    free(cells);
    return rc;
}

/* !_Columns: columns of tables to come (or added to existing ones) */
static int xf_columns(Xf *x, const uint8_t *p, size_t len)
{
    MsiColumn cc[4] = { { "Table", MSI_STRING | MSI_KEY, 0 }, { "Number", 2 | MSI_KEY, 2 },
                        { "Name", MSI_STRING, 0 }, { "Type", 2, 2 } };
    size_t pos = 0;
    while (pos + 2 <= len) {
        unsigned mask = rd16(p + pos);
        pos += 2;
        uint32_t v[4] = { 0 };
        int n = (mask & 1) ? (int)(mask >> 8) : 2;
        if (mask && !(mask & 1)) {                   /* a change to a column's definition: keys + masked */
            for (int k = 0; k < 4; k++)
                if (k < 2 || (mask & (1u << k))) if (!xf_cell(x, &cc[k], p, len, &pos, &v[k])) return 1624;
            continue;
        }
        if (n > 4) n = 4;
        for (int k = 0; k < n; k++) if (!xf_cell(x, &cc[k], p, len, &pos, &v[k])) return 1624;
        if (!(mask & 1)) continue;                   /* (a dropped column: tables drop whole) */
        const char *table = str_at(x->db, v[0]);
        int num = (int)((v[1] - 0x8000) & 0xFFFF);
        uint16_t type = (uint16_t)((v[3] - 0x8000) & 0xFFFF);
        MsiTable *t = find_table(x->db, table);
        if (t) {                                     /* a column added to an existing table */
            if (num != t->ncols + 1) continue;
            uint32_t *c = table_cells(x->db, t, 0);
            MsiColumn *ncols = realloc(t->cols, (size_t)(t->ncols + 1) * sizeof(MsiColumn));
            if (!c || !ncols) { free(c); return 8; }
            t->cols = ncols;
            uint32_t *wide = calloc((size_t)t->nrows * (size_t)(t->ncols + 1) + 1, sizeof(uint32_t));
            if (!wide) { free(c); return 8; }
            for (int r = 0; r < t->nrows; r++) memcpy(&wide[(size_t)r * (t->ncols + 1)], &c[(size_t)r * t->ncols], (size_t)t->ncols * 4);
            t->cols[t->ncols].name = strdup(str_at(x->db, v[2]));
            t->cols[t->ncols].type = type;
            t->cols[t->ncols].width = col_width(x->db, type);
            t->ncols++;
            table_store(t, wide, t->nrows);
            free(wide);
            free(c);
            continue;
        }
        struct PendingCol *np = realloc(x->pend, (size_t)(x->npend + 1) * sizeof(*np));
        if (!np) return 8;
        x->pend = np;
        np[x->npend].table = strdup(table);
        np[x->npend].name = strdup(str_at(x->db, v[2]));
        np[x->npend].num = num;
        np[x->npend].type = type;
        x->npend++;
    }
    return 0;
}

/* !_Tables: tables created (with the columns !_Columns gave) or dropped */
static int xf_tables(Xf *x, const uint8_t *p, size_t len)
{
    MsiColumn tc = { "Name", MSI_STRING | MSI_KEY, 0 };
    size_t pos = 0;
    while (pos + 2 <= len) {
        unsigned mask = rd16(p + pos);
        pos += 2;
        uint32_t v = 0;
        if (!xf_cell(x, &tc, p, len, &pos, &v)) return 1624;
        const char *name = str_at(x->db, v);
        MsiTable *t = find_table(x->db, name);
        if (!(mask & 1)) {
            if (!t) { int rc = xf_error(x, XF_DELMISSINGTABLE, "no table %s to drop%s", name, NULL); if (rc) return rc; continue; }
            drop_table(x->db, t);
            continue;
        }
        if (t) { int rc = xf_error(x, XF_ADDEXISTINGTABLE, "table %s exists already%s", name, NULL); if (rc) return rc; continue; }
        int n = 0;
        for (int i = 0; i < x->npend; i++) if (!strcmp(x->pend[i].table, name) && x->pend[i].num > n) n = x->pend[i].num;
        if (!n) { snprintf(x->err, (size_t)x->errcap, "the transform adds table %s without columns", name); return 1624; }
        t = add_table(x->db, name);
        if (!t || !(t->cols = calloc((size_t)n, sizeof(MsiColumn)))) return 8;
        t->ncols = n;
        for (int i = 0; i < x->npend; i++) {
            if (strcmp(x->pend[i].table, name) || x->pend[i].num < 1) continue;
            MsiColumn *c = &t->cols[x->pend[i].num - 1];
            free(c->name);
            c->name = strdup(x->pend[i].name);
            c->type = x->pend[i].type;
            c->width = col_width(x->db, c->type);
        }
        for (int k = 0; k < n; k++) if (!t->cols[k].name) { t->cols[k].name = strdup(""); t->cols[k].width = 2; }
    }
    return 0;
}

typedef struct { char (*names)[80]; int n, cap; } NameList;

static void collect(void *ctx, const char *name, bool is_stream)
{
    NameList *l = ctx;
    if (!is_stream || name[0] != '!' || l->n >= l->cap) return;
    snprintf(l->names[l->n++], 80, "%s", name + 1);
}

int msidb_apply_transform(MsiDb *db, MsiFile *f, const char *storage, int suppress, char *err, int errcap)
{
    Xf x;
    memset(&x, 0, sizeof(x));
    x.db = db;
    x.err = err;
    x.errcap = errcap;
    x.suppress = suppress;
    if (err && errcap) err[0] = 0;
    Cfb view;
    if (storage) {
        if (!cfb_open_storage(&f->cfb, storage, &view)) { snprintf(err, (size_t)errcap, "no transform %s in the file", storage); return 1624; }
    } else { view = f->cfb; view.borrowed = true; }
    x.cfb = &view;
    x.x.cfb = view;
    if (!load_strings(&x.x)) { snprintf(err, (size_t)errcap, "the transform has no string pool"); return 1624; }
    int rc = 0;
    size_t len;
    uint8_t *s = cfb_read(&view, "!_Columns", &len);
    if (s) { rc = xf_columns(&x, s, len); free(s); }
    if (!rc && (s = cfb_read(&view, "!_Tables", &len))) { rc = xf_tables(&x, s, len); free(s); }
    char names[256][80];
    NameList l = { names, 0, 256 };
    if (!rc) cfb_list(&view, collect, &l);
    for (int i = 0; !rc && i < l.n; i++) {
        const char *n = names[i];
        if (!strcmp(n, "_StringPool") || !strcmp(n, "_StringData") || !strcmp(n, "_Columns") || !strcmp(n, "_Tables")) continue;
        MsiTable *t = find_table(db, n);
        if (!t) { rc = xf_error(&x, XF_UPDATEMISSINGROW, "the transform changes table %s, which the package lacks%s", n, NULL); continue; }
        if (!t->stream && !(t->stream = malloc(1))) { rc = 8; break; }
        char sn[90];
        snprintf(sn, sizeof(sn), "!%s", n);
        if (!(s = cfb_read(&view, sn, &len))) continue;
        rc = xf_table(&x, t, s, len);
        free(s);
    }
    if (!rc && !widen_strings(db)) rc = 8;
    if (!rc) msidb_add_streams(db, f, storage);    /* the streams it brings: binaries, cabinets */
    for (int i = 0; i < x.npend; i++) { free(x.pend[i].table); free(x.pend[i].name); }
    free(x.pend);
    for (int i = 1; i < x.x.nstrings; i++) if (x.x.strings[i] && x.x.strings[i][0]) free(x.x.strings[i]);
    free(x.x.strings);
    if (rc == 8 && err && !err[0]) snprintf(err, (size_t)errcap, "out of memory");
    return rc;
}
