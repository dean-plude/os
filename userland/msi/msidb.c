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
