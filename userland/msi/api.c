/*
 * api.c — msi.dll's handle API: records, databases and SQL views, the
 * session calls custom actions make (MsiGetProperty, MsiSetProperty,
 * MsiProcessMessage, MsiDoAction...), and summary information.
 *
 * DLL custom actions run in a custom-action server, as on Windows: a
 * second msiexec.exe ("/novaca", the SysWOW64 one for 32-bit DLLs) loads
 * the DLL and calls its entry point.  There, every call below is sent to
 * the installing process over a pair of pipes and answered from its
 * handle table; in the installing process (and in programs that open
 * databases themselves) the same requests are answered directly.  Each
 * call is a Call: up to six integers and three UTF-8 strings in, an
 * integer result, four integers, a string and a blob out.
 */
#define MSI_EXPORT __declspec(dllexport)
#include "msi.h"
#include "engine.h"

#define MSIAPI __declspec(dllexport) UINT WINAPI
typedef unsigned long MSIHANDLE;

#ifndef ERROR_NO_MORE_ITEMS
#define ERROR_NO_MORE_ITEMS 259
#endif
#define ERROR_INVALID_HANDLE_STATE 1609
#define ERROR_BAD_QUERY_SYNTAX     1615
#define ERROR_FUNCTION_FAILED_MSI  1627
#define ERROR_INVALID_TABLE        1628
#define ERROR_DATATYPE_MISMATCH    1629
#define ERROR_UNKNOWN_FEATURE      1606
#define ERROR_UNKNOWN_COMPONENT    1607
#define ERROR_INSTALL_FAILURE      1603
#define ERROR_DIRECTORY_MSI        267
#define ERROR_OPEN_FAILED          110

/* -----------------------------------------------------------------------
 * Requests
 * ----------------------------------------------------------------------- */
enum {
    F_DONE = 1, F_CLOSE, F_CLOSE_ALL, F_CREATE_RECORD, F_REC_COUNT, F_REC_GET_STR, F_REC_SET_STR,
    F_REC_GET_INT, F_REC_SET_INT, F_REC_IS_NULL, F_REC_DATASIZE, F_REC_CLEAR, F_REC_READ_STREAM,
    F_REC_SET_STREAM, F_FORMAT_RECORD, F_ACTIVE_DB, F_OPEN_VIEW, F_VIEW_EXECUTE, F_VIEW_FETCH,
    F_VIEW_CLOSE, F_VIEW_MODIFY, F_VIEW_COLINFO, F_VIEW_ERROR, F_PRIMARY_KEYS, F_TABLE_PERSISTENT,
    F_DB_COMMIT, F_DB_STATE, F_OPEN_DATABASE, F_GET_PROP, F_SET_PROP, F_GET_MODE, F_SET_MODE,
    F_LANGUAGE, F_GET_TARGET, F_SET_TARGET, F_GET_SOURCE, F_DO_ACTION, F_SEQUENCE, F_EVAL_COND,
    F_MESSAGE, F_FEATURE_STATE, F_SET_FEATURE_STATE, F_COMP_STATE, F_SET_COMP_STATE,
    F_SET_INSTALL_LEVEL, F_SUMINFO, F_SUMINFO_GET, F_SUMINFO_COUNT, F_APPLY_TRANSFORM,
};

typedef struct {
    uint32_t fn;
    uint32_t in[6];
    char    *s[3];                    /* UTF-8 in, NULL allowed */
    uint32_t ret;
    uint32_t out[4];
    char    *out_s;                   /* malloc'd */
    void    *blob;                    /* malloc'd */
    uint32_t blob_size;
} Call;

static void call_free(Call *c)
{
    free(c->out_s);
    free(c->blob);
    c->out_s = NULL;
    c->blob = NULL;
}

/* -----------------------------------------------------------------------
 * Handles (in the process that owns them)
 * ----------------------------------------------------------------------- */
enum { H_FREE, H_REC, H_VIEW, H_DB, H_SESSION, H_SUMINFO };

typedef struct {
    MsiDb  db;
    void  *data;
    bool   owned;                     /* opened by MsiOpenDatabase */
    MsiDb *pdb;                       /* the database (own or the session's) */
    int    refs;
} DbObj;

typedef struct {
    MsiView *v;
    DbObj   *db;
    int      err;
    char     err_col[80];
} ViewObj;

typedef struct { int pid, type, i; FILETIME ft; char *s; } SumProp;
typedef struct { SumProp p[24]; int n; } SumInfo;

typedef struct { int type; void *p; unsigned gen; } HEnt;

static HEnt    *g_handles;
static unsigned g_nhandles;
static CRITICAL_SECTION g_cs;
static bool     g_cs_ready;
static unsigned g_gen;               /* bumped at each custom action, for cleaning up after it */

static void lock(void)
{
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    EnterCriticalSection(&g_cs);
}
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static MSIHANDLE new_handle(int type, void *p)
{
    lock();
    unsigned i;
    for (i = 0; i < g_nhandles; i++) if (g_handles[i].type == H_FREE) break;
    if (i == g_nhandles) {
        unsigned n = g_nhandles ? g_nhandles * 2 : 64;
        HEnt *h = realloc(g_handles, n * sizeof(HEnt));
        if (!h) { unlock(); return 0; }
        memset(h + g_nhandles, 0, (n - g_nhandles) * sizeof(HEnt));
        g_handles = h;
        g_nhandles = n;
    }
    g_handles[i].type = type;
    g_handles[i].p = p;
    g_handles[i].gen = g_gen;
    unlock();
    return i + 1;
}

static void *get_handle(MSIHANDLE h, int type)
{
    if (!h || h > g_nhandles || g_handles[h - 1].type != type) return NULL;
    return g_handles[h - 1].p;
}

static void db_release(DbObj *d)
{
    if (!d || --d->refs > 0) return;
    if (d->owned) { msisql_free_temp(&d->db); msidb_close(&d->db); free(d->data); }
    free(d);
}

static void free_suminfo(SumInfo *s)
{
    for (int i = 0; i < s->n; i++) free(s->p[i].s);
    free(s);
}

static UINT close_handle(MSIHANDLE h)
{
    if (!h) return ERROR_SUCCESS;
    lock();
    if (h > g_nhandles || g_handles[h - 1].type == H_FREE) { unlock(); return ERROR_INVALID_HANDLE; }
    HEnt e = g_handles[h - 1];
    g_handles[h - 1].type = H_FREE;
    g_handles[h - 1].p = NULL;
    unlock();
    switch (e.type) {
    case H_REC: msirec_free(e.p); break;
    case H_VIEW: { ViewObj *v = e.p; msisql_close(v->v); db_release(v->db); free(v); break; }
    case H_DB: db_release(e.p); break;
    case H_SUMINFO: free_suminfo(e.p); break;
    default: break;
    }
    return ERROR_SUCCESS;
}

unsigned api_handle_mark(void) { return ++g_gen; }

unsigned api_session_handle(Inst *in) { return new_handle(H_SESSION, in); }

/* Close what a custom action left open: the handles made since @mark */
void api_close_handles_since(unsigned mark)
{
    for (unsigned i = 0; i < g_nhandles; i++)
        if (g_handles[i].type != H_FREE && g_handles[i].gen >= mark) close_handle(i + 1);
    g_gen = mark ? mark - 1 : 0;
}

/* -----------------------------------------------------------------------
 * Summary information: the "\005SummaryInformation" property set
 * ----------------------------------------------------------------------- */
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static SumInfo *read_suminfo(MsiDb *db)
{
    SumInfo *s = calloc(1, sizeof(SumInfo));
    if (!s) return NULL;
    size_t size = 0;
    uint8_t *d = cfb_read(&db->cfb, "\005SummaryInformation", &size);
    if (!d || size < 48) { free(d); return s; }
    uint32_t off = le32(d + 44);
    if (off + 8 > size) { free(d); return s; }
    uint32_t count = le32(d + off + 4);
    for (uint32_t i = 0; i < count && s->n < 24 && off + 8 + i * 8 + 8 <= size; i++) {
        uint32_t pid = le32(d + off + 8 + i * 8), po = off + le32(d + off + 12 + i * 8);
        if (po + 8 > size) continue;
        uint32_t type = le32(d + po);
        SumProp *p = &s->p[s->n];
        p->pid = (int)pid;
        p->type = (int)type;
        if (type == 2) p->i = (int16_t)(d[po + 4] | d[po + 5] << 8);
        else if (type == 3) p->i = (int)le32(d + po + 4);
        else if (type == 64) { p->ft.dwLowDateTime = le32(d + po + 4); p->ft.dwHighDateTime = le32(d + po + 8); }
        else if (type == 30) {
            uint32_t n = le32(d + po + 4);
            if (po + 8 + n > size) continue;
            p->s = malloc(n + 1);
            if (p->s) { memcpy(p->s, d + po + 8, n); p->s[n] = 0; }
        } else continue;
        s->n++;
    }
    free(d);
    return s;
}

/* -----------------------------------------------------------------------
 * Serving requests
 * ----------------------------------------------------------------------- */
static Inst *session_of(MSIHANDLE h) { return get_handle(h, H_SESSION); }

static DbObj *db_of(MSIHANDLE h) { return get_handle(h, H_DB); }

static void set_out_s(Call *c, const char *s) { free(c->out_s); c->out_s = strdup(s ? s : ""); }

static DbObj *open_db_file(const char *path)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH);
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(f, NULL), rd = 0;
    void *data = malloc(sz ? sz : 1);
    if (!data || !ReadFile(f, data, sz, &rd, NULL) || rd != sz) { CloseHandle(f); free(data); return NULL; }
    CloseHandle(f);
    DbObj *d = calloc(1, sizeof(DbObj));
    if (!d || !msidb_open(&d->db, data, sz)) { free(d); free(data); return NULL; }
    d->data = data;
    d->owned = true;
    d->pdb = &d->db;
    d->refs = 1;
    return d;
}

static void serve(Call *c)
{
    uint32_t h = c->in[0];
    c->ret = ERROR_INVALID_HANDLE;
    switch (c->fn) {
    case F_CLOSE: c->ret = close_handle(h); return;
    case F_CLOSE_ALL: {
        unsigned n = 0;
        for (unsigned i = 0; i < g_nhandles; i++)
            if (g_handles[i].type != H_FREE && g_handles[i].type != H_SESSION) { close_handle(i + 1); n++; }
        c->ret = n;
        return;
    }
    case F_CREATE_RECORD: {
        MsiRec *r = msirec_new((int)h);
        c->ret = r ? new_handle(H_REC, r) : 0;
        if (!c->ret) msirec_free(r);
        return;
    }
    case F_REC_COUNT: { MsiRec *r = get_handle(h, H_REC); c->ret = r ? (uint32_t)r->n : (uint32_t)-1; return; }
    case F_REC_GET_STR: {
        MsiRec *r = get_handle(h, H_REC);
        if (!r) return;
        int i = (int)c->in[1];
        if (i < 0 || i > r->n) { c->ret = ERROR_INVALID_PARAMETER; return; }
        if (r->f[i].type == MSIF_STREAM) { c->ret = ERROR_INVALID_DATATYPE; return; }
        char b[16];
        set_out_s(c, msirec_str(r, i, b));
        c->ret = ERROR_SUCCESS;
        return;
    }
    case F_REC_SET_STR: {
        MsiRec *r = get_handle(h, H_REC);
        if (!r) return;
        int i = (int)c->in[1];
        if (i < 0 || i > r->n) { c->ret = ERROR_INVALID_PARAMETER; return; }
        msirec_set_str(r, i, c->s[0]);
        c->ret = ERROR_SUCCESS;
        return;
    }
    case F_REC_GET_INT: { MsiRec *r = get_handle(h, H_REC); c->ret = r ? (uint32_t)msirec_int(r, (int)c->in[1]) : (uint32_t)MSI_NULL_INTEGER; return; }
    case F_REC_SET_INT: {
        MsiRec *r = get_handle(h, H_REC);
        if (!r) return;
        int i = (int)c->in[1];
        if (i < 0 || i > r->n) { c->ret = ERROR_INVALID_PARAMETER; return; }
        msirec_set_int(r, i, (int)c->in[2]);
        c->ret = ERROR_SUCCESS;
        return;
    }
    case F_REC_IS_NULL: { MsiRec *r = get_handle(h, H_REC); c->ret = !r || msirec_is_null(r, (int)c->in[1]); return; }
    case F_REC_DATASIZE: {
        MsiRec *r = get_handle(h, H_REC);
        int i = (int)c->in[1];
        c->ret = 0;
        if (!r || i < 0 || i > r->n) return;
        const MsiField *f = &r->f[i];
        c->ret = f->type == MSIF_INT ? sizeof(int) : f->type == MSIF_STR ? (uint32_t)strlen(f->s) :
                 f->type == MSIF_STREAM ? (uint32_t)f->size : 0;
        return;
    }
    case F_REC_CLEAR: { MsiRec *r = get_handle(h, H_REC); if (!r) return; msirec_clear(r); c->ret = 0; return; }
    case F_REC_READ_STREAM: {
        MsiRec *r = get_handle(h, H_REC);
        int i = (int)c->in[1];
        if (!r) return;
        if (i < 1 || i > r->n || r->f[i].type != MSIF_STREAM) { c->ret = ERROR_INVALID_DATATYPE; return; }
        MsiField *f = &r->f[i];
        uint32_t left = (uint32_t)(f->size - f->pos);
        if (c->in[3]) { c->out[0] = left; c->ret = 0; return; }       /* size only */
        uint32_t n = c->in[2] < left ? c->in[2] : left;
        c->blob = malloc(n ? n : 1);
        if (!c->blob) { c->ret = ERROR_OUTOFMEMORY; return; }
        memcpy(c->blob, f->data + f->pos, n);
        c->blob_size = n;
        f->pos += n;
        c->ret = 0;
        return;
    }
    case F_REC_SET_STREAM: {
        MsiRec *r = get_handle(h, H_REC);
        int i = (int)c->in[1];
        if (!r) return;
        if (i < 1 || i > r->n) { c->ret = ERROR_INVALID_PARAMETER; return; }
        if (!c->s[0]) { if (r->f[i].type == MSIF_STREAM) r->f[i].pos = 0; c->ret = 0; return; }   /* rewind */
        WCHAR w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, c->s[0], -1, w, MAX_PATH);
        HANDLE fh = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (fh == INVALID_HANDLE_VALUE) { c->ret = ERROR_FILE_NOT_FOUND; return; }
        DWORD sz = GetFileSize(fh, NULL), rd = 0;
        void *data = malloc(sz ? sz : 1);
        if (data && ReadFile(fh, data, sz, &rd, NULL) && rd == sz) { msirec_set_stream(r, i, data, sz); c->ret = 0; }
        else c->ret = ERROR_READ_FAULT;
        CloseHandle(fh);
        free(data);
        return;
    }
    case F_FORMAT_RECORD: {
        MsiRec *r = get_handle(c->in[1], H_REC);
        if (!r) return;
        Inst *in = session_of(h);
        char b[16];
        const char *tmpl = msirec_str(r, 0, b);
        size_t cap = 4096;
        for (int i = 1; i <= r->n; i++) if (r->f[i].type == MSIF_STR) cap += strlen(r->f[i].s);
        cap += strlen(tmpl);
        char *out = malloc(cap);
        if (!out) { c->ret = ERROR_OUTOFMEMORY; return; }
        if (msirec_is_null(r, 0)) {
            /* no template: "1: value 2: value " */
            size_t n = 0;
            out[0] = 0;
            for (int i = 1; i <= r->n; i++) {
                char nb[16];
                n += (size_t)snprintf(out + n, cap - n, "%d: %s ", i, msirec_str(r, i, nb));
                if (n >= cap) break;
            }
        } else if (in) {
            eng_format(in, r, tmpl, out, (int)cap);
        } else {
            /* no session: only [n] and [\c] */
            size_t n = 0;
            for (const char *p = tmpl; *p && n < cap - 1; ) {
                const char *e;
                if (*p == '[' && (e = strchr(p, ']'))) {
                    char key[32];
                    size_t kl = (size_t)(e - p - 1);
                    if (kl < sizeof(key)) {
                        memcpy(key, p + 1, kl);
                        key[kl] = 0;
                        char nb[16];
                        const char *v = "";
                        if (key[0] >= '0' && key[0] <= '9') v = msirec_str(r, atoi(key), nb);
                        else if (key[0] == '\\' && key[1]) { nb[0] = key[1]; nb[1] = 0; v = nb; }
                        for (; *v && n < cap - 1; v++) out[n++] = *v;
                        p = e + 1;
                        continue;
                    }
                }
                out[n++] = *p++;
            }
            out[n] = 0;
        }
        free(c->out_s);
        c->out_s = out;
        c->ret = 0;
        return;
    }
    case F_ACTIVE_DB: {
        Inst *in = session_of(h);
        if (!in) { c->ret = 0; return; }
        DbObj *d = calloc(1, sizeof(DbObj));
        if (!d) { c->ret = 0; return; }
        d->pdb = eng_db(in);
        d->refs = 1;
        c->ret = new_handle(H_DB, d);
        return;
    }
    case F_OPEN_DATABASE: {
        if (!c->s[0]) { c->ret = ERROR_INVALID_PARAMETER; return; }
        DbObj *d = open_db_file(c->s[0]);
        if (!d) { c->ret = ERROR_OPEN_FAILED; return; }
        c->out[0] = new_handle(H_DB, d);
        c->ret = 0;
        return;
    }
    case F_OPEN_VIEW: {
        DbObj *d = db_of(h);
        if (!d) return;
        if (!c->s[0]) { c->ret = ERROR_INVALID_PARAMETER; return; }
        MsiView *v;
        char err[128];
        int r = msisql_open(d->pdb, c->s[0], &v, err, sizeof(err));
        if (r) { c->ret = (uint32_t)r; set_out_s(c, err); return; }
        ViewObj *vo = calloc(1, sizeof(ViewObj));
        if (!vo) { msisql_close(v); c->ret = ERROR_OUTOFMEMORY; return; }
        vo->v = v;
        vo->db = d;
        d->refs++;
        c->out[0] = new_handle(H_VIEW, vo);
        c->ret = 0;
        return;
    }
    case F_VIEW_EXECUTE: {
        ViewObj *v = get_handle(h, H_VIEW);
        if (!v) return;
        MsiRec *p = c->in[1] ? get_handle(c->in[1], H_REC) : NULL;
        c->ret = (uint32_t)msisql_execute(v->v, p);
        return;
    }
    case F_VIEW_FETCH: {
        ViewObj *v = get_handle(h, H_VIEW);
        if (!v) return;
        MsiRec *r;
        c->ret = (uint32_t)msisql_fetch(v->v, &r);
        if (!c->ret) c->out[0] = new_handle(H_REC, r);
        return;
    }
    case F_VIEW_CLOSE: { ViewObj *v = get_handle(h, H_VIEW); c->ret = v ? 0 : ERROR_INVALID_HANDLE; return; }
    case F_VIEW_MODIFY: {
        ViewObj *v = get_handle(h, H_VIEW);
        MsiRec *r = get_handle(c->in[2], H_REC);
        if (!v) return;
        if (!r) { c->ret = ERROR_INVALID_HANDLE; return; }
        c->ret = (uint32_t)msisql_modify(v->v, (int)c->in[1], r);
        return;
    }
    case F_VIEW_COLINFO: {
        ViewObj *v = get_handle(h, H_VIEW);
        if (!v) return;
        MsiRec *r;
        c->ret = (uint32_t)msisql_colinfo(v->v, c->in[1] != 0, &r);
        if (!c->ret) c->out[0] = new_handle(H_REC, r);
        return;
    }
    case F_VIEW_ERROR: { c->ret = 0; set_out_s(c, ""); return; }   /* MSIDBERROR_NOERROR */
    case F_PRIMARY_KEYS: {
        DbObj *d = db_of(h);
        if (!d) return;
        MsiRec *r;
        c->ret = (uint32_t)msisql_primary_keys(d->pdb, c->s[0] ? c->s[0] : "", &r);
        if (!c->ret) c->out[0] = new_handle(H_REC, r);
        return;
    }
    case F_TABLE_PERSISTENT: {
        DbObj *d = db_of(h);
        if (!d) { c->ret = 3; return; }
        const char *t = c->s[0] ? c->s[0] : "";
        bool base = false;
        for (int i = 0; i < d->pdb->ntables; i++) if (!strcmp(d->pdb->tables[i].name, t)) base = true;
        c->ret = base ? 1 : msisql_table_exists(d->pdb, t) ? 0 : 3;
        return;
    }
    case F_DB_COMMIT: c->ret = db_of(h) ? 0 : ERROR_INVALID_HANDLE; return;
    case F_APPLY_TRANSFORM: {                                          /* MsiDatabaseApplyTransform */
        DbObj *d = db_of(h);
        if (!d) return;
        WCHAR w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, c->s[0] ? c->s[0] : "", -1, w, MAX_PATH);
        HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        c->ret = 1624;                                                 /* ERROR_INSTALL_TRANSFORM_FAILURE */
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD sz = GetFileSize(f, NULL), rd = 0;
        void *data = malloc(sz ? sz : 1);
        bool ok = data && ReadFile(f, data, sz, &rd, NULL) && rd == sz;
        CloseHandle(f);
        MsiFile *mf = ok ? msifile_load(data, sz) : (free(data), NULL);
        if (!mf) return;
        char err[256];
        c->ret = (uint32_t)msidb_apply_transform(d->pdb, mf, NULL, (int)c->in[1], err, sizeof(err));
        msifile_release(mf);
        return;
    }
    case F_DB_STATE: c->ret = 0; return;                               /* MSIDBSTATE_READ */
    case F_GET_PROP: {
        Inst *in = session_of(h);
        if (!in) return;
        set_out_s(c, eng_get_prop(in, c->s[0] ? c->s[0] : ""));
        c->ret = 0;
        return;
    }
    case F_SET_PROP: {
        Inst *in = session_of(h);
        if (!in) return;
        if (!c->s[0] || !c->s[0][0]) { c->ret = ERROR_FUNCTION_FAILED_MSI; return; }
        eng_set_prop(in, c->s[0], c->s[1] ? c->s[1] : "");
        c->ret = 0;
        return;
    }
    case F_GET_MODE: { Inst *in = session_of(h); c->ret = in ? eng_get_mode(in, (int)c->in[1]) : 0; return; }
    case F_SET_MODE: { Inst *in = session_of(h); if (!in) return; c->ret = (uint32_t)eng_set_mode(in, (int)c->in[1], c->in[2] != 0); return; }
    case F_LANGUAGE: {
        Inst *in = session_of(h);
        c->ret = in ? (uint32_t)atoi(eng_get_prop(in, "ProductLanguage")) : 0;
        return;
    }
    case F_GET_TARGET: case F_GET_SOURCE: {
        Inst *in = session_of(h);
        if (!in) return;
        char p[MAX_PATH * 2];
        c->ret = (uint32_t)(c->fn == F_GET_TARGET ? eng_target_path : eng_source_path)(in, c->s[0] ? c->s[0] : "", p, sizeof(p));
        if (!c->ret) set_out_s(c, p);
        return;
    }
    case F_SET_TARGET: {
        Inst *in = session_of(h);
        if (!in) return;
        c->ret = (uint32_t)eng_set_target_path(in, c->s[0] ? c->s[0] : "", c->s[1] ? c->s[1] : "");
        return;
    }
    case F_DO_ACTION: { Inst *in = session_of(h); if (!in) return; c->ret = (uint32_t)eng_do_action(in, c->s[0] ? c->s[0] : ""); return; }
    case F_SEQUENCE: { Inst *in = session_of(h); if (!in) return; c->ret = (uint32_t)eng_sequence(in, c->s[0] ? c->s[0] : ""); return; }
    case F_EVAL_COND: {
        Inst *in = session_of(h);
        if (!in) { c->ret = 3; return; }
        const char *cond = c->s[0] ? c->s[0] : "";
        while (*cond == ' ') cond++;
        c->ret = !*cond ? 2 : eng_condition(in, cond) ? 1 : 0;
        return;
    }
    case F_MESSAGE: {
        Inst *in = session_of(h);
        MsiRec *r = get_handle(c->in[2], H_REC);
        if (!in) { c->ret = (uint32_t)-1; return; }
        c->ret = (uint32_t)eng_message(in, (int)c->in[1], r);
        return;
    }
    case F_FEATURE_STATE: case F_COMP_STATE: {
        Inst *in = session_of(h);
        if (!in) return;
        int a = ISTATE_UNKNOWN, b = ISTATE_UNKNOWN;
        c->ret = (uint32_t)(c->fn == F_FEATURE_STATE ? eng_feature_state : eng_component_state)(in, c->s[0] ? c->s[0] : "", &a, &b);
        c->out[0] = (uint32_t)a;
        c->out[1] = (uint32_t)b;
        return;
    }
    case F_SET_FEATURE_STATE: { Inst *in = session_of(h); if (!in) return; c->ret = (uint32_t)eng_set_feature_state(in, c->s[0] ? c->s[0] : "", (int)c->in[1]); return; }
    case F_SET_COMP_STATE: { Inst *in = session_of(h); if (!in) return; c->ret = (uint32_t)eng_set_component_state(in, c->s[0] ? c->s[0] : "", (int)c->in[1]); return; }
    case F_SET_INSTALL_LEVEL: { Inst *in = session_of(h); if (!in) return; c->ret = (uint32_t)eng_set_install_level(in, (int)c->in[1]); return; }
    case F_SUMINFO: {
        DbObj *d = h ? db_of(h) : NULL;
        DbObj *own = NULL;
        if (!d && c->s[0]) d = own = open_db_file(c->s[0]);
        if (!d) { c->ret = h ? ERROR_INVALID_HANDLE : ERROR_OPEN_FAILED; return; }
        SumInfo *s = read_suminfo(d->pdb);
        if (own) db_release(own);
        if (!s) { c->ret = ERROR_OUTOFMEMORY; return; }
        c->out[0] = new_handle(H_SUMINFO, s);
        c->ret = 0;
        return;
    }
    case F_SUMINFO_COUNT: { SumInfo *s = get_handle(h, H_SUMINFO); if (!s) return; c->out[0] = (uint32_t)s->n; c->ret = 0; return; }
    case F_SUMINFO_GET: {
        SumInfo *s = get_handle(h, H_SUMINFO);
        if (!s) return;
        c->ret = 0;
        c->out[0] = 0;                                         /* VT_EMPTY */
        for (int i = 0; i < s->n; i++) {
            if (s->p[i].pid != (int)c->in[1]) continue;
            c->out[0] = (uint32_t)s->p[i].type;
            c->out[1] = (uint32_t)s->p[i].i;
            c->out[2] = s->p[i].ft.dwLowDateTime;
            c->out[3] = s->p[i].ft.dwHighDateTime;
            if (s->p[i].s) set_out_s(c, s->p[i].s);
        }
        return;
    }
    default:
        c->ret = ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/* -----------------------------------------------------------------------
 * The pipe protocol
 * ----------------------------------------------------------------------- */
static bool   g_remote;               /* this process is a custom-action server */
static HANDLE g_req, g_rep;           /* to / from the installing process */

static bool write_all(HANDLE h, const void *p, DWORD n)
{
    const uint8_t *b = p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, b, n, &w, NULL) || !w) return false;
        b += w;
        n -= w;
    }
    return true;
}

static bool read_all(HANDLE h, void *p, DWORD n)
{
    uint8_t *b = p;
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, b, n, &r, NULL) || !r) return false;
        b += r;
        n -= r;
    }
    return true;
}

static bool send_str(HANDLE h, const char *s)
{
    uint32_t n = s ? (uint32_t)strlen(s) : 0xFFFFFFFFu;
    if (!write_all(h, &n, 4)) return false;
    return !s || !n || write_all(h, s, n);
}

static bool recv_str(HANDLE h, char **out)
{
    uint32_t n;
    *out = NULL;
    if (!read_all(h, &n, 4)) return false;
    if (n == 0xFFFFFFFFu) return true;
    if (n > (64u << 20)) return false;
    *out = malloc(n + 1);
    if (!*out) return false;
    if (n && !read_all(h, *out, n)) return false;
    (*out)[n] = 0;
    return true;
}

static bool send_request(HANDLE h, const Call *c)
{
    if (!write_all(h, &c->fn, 4) || !write_all(h, c->in, sizeof(c->in))) return false;
    for (int i = 0; i < 3; i++) if (!send_str(h, c->s[i])) return false;
    return true;
}

static bool recv_request(HANDLE h, Call *c)
{
    memset(c, 0, sizeof(*c));
    if (!read_all(h, &c->fn, 4) || !read_all(h, c->in, sizeof(c->in))) return false;
    for (int i = 0; i < 3; i++) if (!recv_str(h, &c->s[i])) return false;
    return true;
}

static bool send_reply(HANDLE h, const Call *c)
{
    if (!write_all(h, &c->ret, 4) || !write_all(h, c->out, sizeof(c->out)) || !send_str(h, c->out_s)) return false;
    if (!write_all(h, &c->blob_size, 4)) return false;
    return !c->blob_size || write_all(h, c->blob, c->blob_size);
}

static bool recv_reply(HANDLE h, Call *c)
{
    if (!read_all(h, &c->ret, 4) || !read_all(h, c->out, sizeof(c->out)) || !recv_str(h, &c->out_s)) return false;
    if (!read_all(h, &c->blob_size, 4)) return false;
    if (c->blob_size) {
        c->blob = malloc(c->blob_size);
        if (!c->blob || !read_all(h, c->blob, c->blob_size)) return false;
    }
    return true;
}

static void dispatch(Call *c)
{
    if (!g_remote) { serve(c); return; }
    if (!send_request(g_req, c) || !recv_reply(g_rep, c)) {
        /* the installer went away: nothing more to do */
        ExitProcess(ERROR_INSTALL_FAILURE);
    }
}

/* In the custom-action server: load the DLL and call its entry point */
typedef UINT (WINAPI *CaEntry)(MSIHANDLE);

__declspec(dllexport) int WINAPI MsiNovaCaServer(HANDLE req, HANDLE rep, unsigned session, LPCWSTR dll, LPCSTR entry)
{
    g_remote = true;
    g_req = req;
    g_rep = rep;
    UINT r = ERROR_INSTALL_FAILURE;
    char note[400] = "";
    HMODULE m = LoadLibraryW(dll);
    if (!m) snprintf(note, sizeof(note), "could not load the custom action DLL (error %lu)", GetLastError());
    else {
        CaEntry fn = (CaEntry)(void *)GetProcAddress(m, entry);
        if (!fn) snprintf(note, sizeof(note), "the DLL has no entry point %s", entry);
        else r = fn(session);
    }
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = F_DONE;
    c.in[0] = r;
    c.s[0] = note[0] ? note : NULL;
    dispatch(&c);
    call_free(&c);
    return (int)r;
}

/* In the installer: read the PE header to pick the server */
static bool dll_is_32bit(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint8_t hdr[512];
    DWORD rd = 0;
    ReadFile(f, hdr, sizeof(hdr), &rd, NULL);
    CloseHandle(f);
    if (rd < 64) return false;
    uint32_t pe = le32(hdr + 60);
    if (pe + 6 > rd) return false;
    return (hdr[pe + 4] | hdr[pe + 5] << 8) == 0x14C;
}

DWORD api_wait_process(Inst *in, HANDLE proc)
{
    while (WaitForSingleObject(proc, 20) == WAIT_TIMEOUT) eng_pump(in);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    return code;
}

int api_run_dll_action(Inst *in, const WCHAR *dll, const char *entry, bool *crashed)
{
    *crashed = false;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE reqR, reqW, repR, repW;
    if (!CreatePipe(&reqR, &reqW, &sa, 0)) return ERROR_INSTALL_FAILURE;
    if (!CreatePipe(&repR, &repW, &sa, 0)) { CloseHandle(reqR); CloseHandle(reqW); return ERROR_INSTALL_FAILURE; }
    SetHandleInformation(reqR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(repW, HANDLE_FLAG_INHERIT, 0);
    unsigned mark = api_handle_mark();
    MSIHANDLE session = api_session_handle(in);
    bool x86 = dll_is_32bit(dll);
    WCHAR exe[MAX_PATH], cmd[1024], wentry[128];
    GetWindowsDirectoryW(exe, MAX_PATH);
    wcscat(exe, x86 ? L"\\SysWOW64\\msiexec.exe" : L"\\System32\\msiexec.exe");
    MultiByteToWideChar(CP_UTF8, 0, entry, -1, wentry, 128);
    _snwprintf(cmd, 1024, L"\"%s\" /novaca %lu %lu %lu %s \"%s\"", exe, (unsigned long)(ULONG_PTR)reqW,
               (unsigned long)(ULONG_PTR)repR, (unsigned long)session, wentry, dll);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    eng_log(in, "Custom action server (%s): %s in %ls", x86 ? "32-bit" : "64-bit", entry, dll);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(reqW);
    CloseHandle(repR);
    if (!ok) {
        eng_log(in, "Could not start %ls (error %lu)", exe, GetLastError());
        CloseHandle(reqR);
        CloseHandle(repW);
        api_close_handles_since(mark);
        return ERROR_INSTALL_FAILURE;
    }
    int result = -1;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(reqR, NULL, 0, NULL, &avail, NULL) && avail) {
            Call c;
            if (!recv_request(reqR, &c)) break;
            if (c.fn == F_DONE) {
                result = (int)c.in[0];
                if (c.s[0]) eng_log(in, "Custom action %s: %s", entry, c.s[0]);
                c.ret = 0;
                send_reply(repW, &c);
                for (int i = 0; i < 3; i++) free(c.s[i]);
                call_free(&c);
                break;
            }
            serve(&c);
            bool sent = send_reply(repW, &c);
            for (int i = 0; i < 3; i++) free(c.s[i]);
            call_free(&c);
            if (!sent) break;
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_TIMEOUT) {
            /* gone: a last request may still be in the pipe */
            if (PeekNamedPipe(reqR, NULL, 0, NULL, &avail, NULL) && avail) continue;
            break;
        }
        eng_pump(in);
        Sleep(2);
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (result < 0) {
        *crashed = true;
        eng_log(in, "Custom action %s ended without returning (exit code %#lx)", entry, (unsigned long)code);
        result = ERROR_INSTALL_FAILURE;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(reqR);
    CloseHandle(repW);
    api_close_handles_since(mark);
    return result;
}

/* -----------------------------------------------------------------------
 * Strings in and out (Win32 buffer rules)
 * ----------------------------------------------------------------------- */
static char *w2u(LPCWSTR w)
{
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? (size_t)n : 1);
    if (!s) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
    return s;
}

static char *a2u(LPCSTR a)
{
    if (!a) return NULL;
    int n = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
    WCHAR *w = malloc((size_t)(n > 0 ? n : 1) * sizeof(WCHAR));
    if (!w) return NULL;
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, a, -1, w, n); else w[0] = 0;
    char *s = w2u(w);
    free(w);
    return s;
}

static UINT out_w(const char *u8, LPWSTR buf, DWORD *pcch)
{
    if (!u8) u8 = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    DWORD len = n > 0 ? (DWORD)n - 1 : 0;
    if (!pcch) return buf ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    if (!buf) { *pcch = len; return ERROR_SUCCESS; }
    UINT r = ERROR_SUCCESS;
    if (*pcch > len) MultiByteToWideChar(CP_UTF8, 0, u8, -1, buf, (int)len + 1);
    else {
        if (*pcch) {
            WCHAR *tmp = malloc(((size_t)len + 1) * sizeof(WCHAR));
            if (tmp) {
                MultiByteToWideChar(CP_UTF8, 0, u8, -1, tmp, (int)len + 1);
                memcpy(buf, tmp, (*pcch - 1) * sizeof(WCHAR));
                free(tmp);
            }
            buf[*pcch - 1] = 0;
        }
        r = ERROR_MORE_DATA;
    }
    *pcch = len;
    return r;
}

static UINT out_a(const char *u8, LPSTR buf, DWORD *pcch)
{
    if (!u8) u8 = "";
    int wn = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    WCHAR *w = malloc((size_t)(wn > 0 ? wn : 1) * sizeof(WCHAR));
    if (!w) return ERROR_OUTOFMEMORY;
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, wn); else w[0] = 0;
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
    DWORD len = n > 0 ? (DWORD)n - 1 : 0;
    UINT r = ERROR_SUCCESS;
    if (!pcch) r = buf ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    else if (!buf) *pcch = len;
    else {
        char *tmp = malloc((size_t)len + 1);
        if (tmp) WideCharToMultiByte(CP_ACP, 0, w, -1, tmp, (int)len + 1, NULL, NULL);
        if (*pcch > len) { if (tmp) memcpy(buf, tmp, len + 1); }
        else {
            if (*pcch) { if (tmp) memcpy(buf, tmp, *pcch - 1); buf[*pcch - 1] = 0; }
            r = ERROR_MORE_DATA;
        }
        free(tmp);
        *pcch = len;
    }
    free(w);
    return r;
}

/* A call with up to three integers and two strings, the result alone */
static UINT simple(uint32_t fn, uint32_t a, uint32_t b, uint32_t c3, char *s0, char *s1)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = fn;
    c.in[0] = a; c.in[1] = b; c.in[2] = c3;
    c.s[0] = s0; c.s[1] = s1;
    dispatch(&c);
    UINT r = c.ret;
    call_free(&c);
    free(s0);
    free(s1);
    return r;
}

/* A call returning a string through a W or A buffer */
static UINT string_call(uint32_t fn, uint32_t a, uint32_t b, char *s0, void *buf, DWORD *pcch, bool wide)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = fn;
    c.in[0] = a; c.in[1] = b;
    c.s[0] = s0;
    dispatch(&c);
    free(s0);
    UINT r = c.ret;
    if (!r) r = wide ? out_w(c.out_s, buf, pcch) : out_a(c.out_s, buf, pcch);
    call_free(&c);
    return r;
}

/* A call returning a new handle in out[0] */
static UINT handle_call(uint32_t fn, uint32_t a, uint32_t b, char *s0, MSIHANDLE *out)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = fn;
    c.in[0] = a; c.in[1] = b;
    c.s[0] = s0;
    if (!out) { free(s0); return ERROR_INVALID_PARAMETER; }
    dispatch(&c);
    free(s0);
    UINT r = c.ret;
    *out = r ? 0 : c.out[0];
    call_free(&c);
    return r;
}

/* -----------------------------------------------------------------------
 * Exports: handles and records
 * ----------------------------------------------------------------------- */
MSIAPI MsiCloseHandle(MSIHANDLE h) { return simple(F_CLOSE, h, 0, 0, NULL, NULL); }
MSIAPI MsiCloseAllHandles(void) { return simple(F_CLOSE_ALL, 0, 0, 0, NULL, NULL); }
__declspec(dllexport) MSIHANDLE WINAPI MsiCreateRecord(UINT n) { return simple(F_CREATE_RECORD, n, 0, 0, NULL, NULL); }
MSIAPI MsiRecordGetFieldCount(MSIHANDLE h) { return simple(F_REC_COUNT, h, 0, 0, NULL, NULL); }
MSIAPI MsiRecordGetStringW(MSIHANDLE h, UINT i, LPWSTR buf, DWORD *pcch) { return string_call(F_REC_GET_STR, h, i, NULL, buf, pcch, true); }
MSIAPI MsiRecordGetStringA(MSIHANDLE h, UINT i, LPSTR buf, DWORD *pcch) { return string_call(F_REC_GET_STR, h, i, NULL, buf, pcch, false); }
MSIAPI MsiRecordSetStringW(MSIHANDLE h, UINT i, LPCWSTR s) { return simple(F_REC_SET_STR, h, i, 0, w2u(s), NULL); }
MSIAPI MsiRecordSetStringA(MSIHANDLE h, UINT i, LPCSTR s) { return simple(F_REC_SET_STR, h, i, 0, a2u(s), NULL); }
__declspec(dllexport) int WINAPI MsiRecordGetInteger(MSIHANDLE h, UINT i) { return (int)simple(F_REC_GET_INT, h, i, 0, NULL, NULL); }
MSIAPI MsiRecordSetInteger(MSIHANDLE h, UINT i, int v) { return simple(F_REC_SET_INT, h, i, (uint32_t)v, NULL, NULL); }
__declspec(dllexport) BOOL WINAPI MsiRecordIsNull(MSIHANDLE h, UINT i) { return (BOOL)simple(F_REC_IS_NULL, h, i, 0, NULL, NULL); }
MSIAPI MsiRecordDataSize(MSIHANDLE h, UINT i) { return simple(F_REC_DATASIZE, h, i, 0, NULL, NULL); }
MSIAPI MsiRecordClearData(MSIHANDLE h) { return simple(F_REC_CLEAR, h, 0, 0, NULL, NULL); }
MSIAPI MsiRecordSetStreamW(MSIHANDLE h, UINT i, LPCWSTR path) { return simple(F_REC_SET_STREAM, h, i, 0, w2u(path), NULL); }
MSIAPI MsiRecordSetStreamA(MSIHANDLE h, UINT i, LPCSTR path) { return simple(F_REC_SET_STREAM, h, i, 0, a2u(path), NULL); }

MSIAPI MsiRecordReadStream(MSIHANDLE h, UINT i, char *buf, DWORD *pcb)
{
    if (!pcb) return ERROR_INVALID_PARAMETER;
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = F_REC_READ_STREAM;
    c.in[0] = h; c.in[1] = i; c.in[2] = *pcb; c.in[3] = buf == NULL;
    dispatch(&c);
    UINT r = c.ret;
    if (!r) {
        if (!buf) *pcb = c.out[0];
        else { memcpy(buf, c.blob, c.blob_size); *pcb = c.blob_size; }
    }
    call_free(&c);
    return r;
}

MSIAPI MsiFormatRecordW(MSIHANDLE inst, MSIHANDLE rec, LPWSTR buf, DWORD *pcch)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = F_FORMAT_RECORD;
    c.in[0] = inst; c.in[1] = rec;
    dispatch(&c);
    UINT r = c.ret ? c.ret : out_w(c.out_s, buf, pcch);
    call_free(&c);
    return r;
}

MSIAPI MsiFormatRecordA(MSIHANDLE inst, MSIHANDLE rec, LPSTR buf, DWORD *pcch)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = F_FORMAT_RECORD;
    c.in[0] = inst; c.in[1] = rec;
    dispatch(&c);
    UINT r = c.ret ? c.ret : out_a(c.out_s, buf, pcch);
    call_free(&c);
    return r;
}

/* -----------------------------------------------------------------------
 * Exports: databases and views
 * ----------------------------------------------------------------------- */
__declspec(dllexport) MSIHANDLE WINAPI MsiGetActiveDatabase(MSIHANDLE inst) { return simple(F_ACTIVE_DB, inst, 0, 0, NULL, NULL); }

MSIAPI MsiOpenDatabaseW(LPCWSTR path, LPCWSTR persist, MSIHANDLE *out)
{
    (void)persist;            /* read-only access (changes stay in memory) */
    return handle_call(F_OPEN_DATABASE, 0, 0, w2u(path), out);
}

MSIAPI MsiOpenDatabaseA(LPCSTR path, LPCSTR persist, MSIHANDLE *out)
{
    (void)persist;
    return handle_call(F_OPEN_DATABASE, 0, 0, a2u(path), out);
}

MSIAPI MsiDatabaseOpenViewW(MSIHANDLE db, LPCWSTR sql, MSIHANDLE *out) { return handle_call(F_OPEN_VIEW, db, 0, w2u(sql), out); }
MSIAPI MsiDatabaseOpenViewA(MSIHANDLE db, LPCSTR sql, MSIHANDLE *out) { return handle_call(F_OPEN_VIEW, db, 0, a2u(sql), out); }
MSIAPI MsiViewExecute(MSIHANDLE v, MSIHANDLE rec) { return simple(F_VIEW_EXECUTE, v, rec, 0, NULL, NULL); }
MSIAPI MsiViewFetch(MSIHANDLE v, MSIHANDLE *out) { return handle_call(F_VIEW_FETCH, v, 0, NULL, out); }
MSIAPI MsiViewClose(MSIHANDLE v) { return simple(F_VIEW_CLOSE, v, 0, 0, NULL, NULL); }
MSIAPI MsiViewModify(MSIHANDLE v, int mode, MSIHANDLE rec) { return simple(F_VIEW_MODIFY, v, (uint32_t)mode, rec, NULL, NULL); }
MSIAPI MsiViewGetColumnInfo(MSIHANDLE v, int type, MSIHANDLE *out) { return handle_call(F_VIEW_COLINFO, v, (uint32_t)type, NULL, out); }

__declspec(dllexport) int WINAPI MsiViewGetErrorW(MSIHANDLE v, LPWSTR buf, DWORD *pcch)
{
    if (buf && pcch && *pcch) buf[0] = 0;
    if (pcch) *pcch = 0;
    (void)v;
    return 0;                 /* MSIDBERROR_NOERROR */
}

__declspec(dllexport) int WINAPI MsiViewGetErrorA(MSIHANDLE v, LPSTR buf, DWORD *pcch)
{
    if (buf && pcch && *pcch) buf[0] = 0;
    if (pcch) *pcch = 0;
    (void)v;
    return 0;
}

MSIAPI MsiDatabaseGetPrimaryKeysW(MSIHANDLE db, LPCWSTR table, MSIHANDLE *out) { return handle_call(F_PRIMARY_KEYS, db, 0, w2u(table), out); }
MSIAPI MsiDatabaseGetPrimaryKeysA(MSIHANDLE db, LPCSTR table, MSIHANDLE *out) { return handle_call(F_PRIMARY_KEYS, db, 0, a2u(table), out); }
__declspec(dllexport) int WINAPI MsiDatabaseIsTablePersistentW(MSIHANDLE db, LPCWSTR t) { return (int)simple(F_TABLE_PERSISTENT, db, 0, 0, w2u(t), NULL); }
__declspec(dllexport) int WINAPI MsiDatabaseIsTablePersistentA(MSIHANDLE db, LPCSTR t) { return (int)simple(F_TABLE_PERSISTENT, db, 0, 0, a2u(t), NULL); }
MSIAPI MsiDatabaseCommit(MSIHANDLE db) { return simple(F_DB_COMMIT, db, 0, 0, NULL, NULL); }
MSIAPI MsiDatabaseApplyTransformW(MSIHANDLE db, LPCWSTR path, int errors) { return simple(F_APPLY_TRANSFORM, db, (uint32_t)errors, 0, w2u(path), NULL); }
MSIAPI MsiDatabaseApplyTransformA(MSIHANDLE db, LPCSTR path, int errors) { return simple(F_APPLY_TRANSFORM, db, (uint32_t)errors, 0, a2u(path), NULL); }

__declspec(dllexport) int WINAPI MsiGetDatabaseState(MSIHANDLE db) { return (int)simple(F_DB_STATE, db, 0, 0, NULL, NULL); }

/* -----------------------------------------------------------------------
 * Exports: the session
 * ----------------------------------------------------------------------- */
MSIAPI MsiGetPropertyW(MSIHANDLE h, LPCWSTR name, LPWSTR buf, DWORD *pcch) { return string_call(F_GET_PROP, h, 0, w2u(name), buf, pcch, true); }
MSIAPI MsiGetPropertyA(MSIHANDLE h, LPCSTR name, LPSTR buf, DWORD *pcch) { return string_call(F_GET_PROP, h, 0, a2u(name), buf, pcch, false); }
MSIAPI MsiSetPropertyW(MSIHANDLE h, LPCWSTR name, LPCWSTR v) { return simple(F_SET_PROP, h, 0, 0, w2u(name), w2u(v)); }
MSIAPI MsiSetPropertyA(MSIHANDLE h, LPCSTR name, LPCSTR v) { return simple(F_SET_PROP, h, 0, 0, a2u(name), a2u(v)); }
__declspec(dllexport) BOOL WINAPI MsiGetMode(MSIHANDLE h, int mode) { return (BOOL)simple(F_GET_MODE, h, (uint32_t)mode, 0, NULL, NULL); }
MSIAPI MsiSetMode(MSIHANDLE h, int mode, BOOL state) { return simple(F_SET_MODE, h, (uint32_t)mode, (uint32_t)state, NULL, NULL); }
__declspec(dllexport) LANGID WINAPI MsiGetLanguage(MSIHANDLE h) { return (LANGID)simple(F_LANGUAGE, h, 0, 0, NULL, NULL); }
MSIAPI MsiGetTargetPathW(MSIHANDLE h, LPCWSTR f, LPWSTR buf, DWORD *pcch) { return string_call(F_GET_TARGET, h, 0, w2u(f), buf, pcch, true); }
MSIAPI MsiGetTargetPathA(MSIHANDLE h, LPCSTR f, LPSTR buf, DWORD *pcch) { return string_call(F_GET_TARGET, h, 0, a2u(f), buf, pcch, false); }
MSIAPI MsiGetSourcePathW(MSIHANDLE h, LPCWSTR f, LPWSTR buf, DWORD *pcch) { return string_call(F_GET_SOURCE, h, 0, w2u(f), buf, pcch, true); }
MSIAPI MsiGetSourcePathA(MSIHANDLE h, LPCSTR f, LPSTR buf, DWORD *pcch) { return string_call(F_GET_SOURCE, h, 0, a2u(f), buf, pcch, false); }
MSIAPI MsiSetTargetPathW(MSIHANDLE h, LPCWSTR f, LPCWSTR p) { return simple(F_SET_TARGET, h, 0, 0, w2u(f), w2u(p)); }
MSIAPI MsiSetTargetPathA(MSIHANDLE h, LPCSTR f, LPCSTR p) { return simple(F_SET_TARGET, h, 0, 0, a2u(f), a2u(p)); }
MSIAPI MsiDoActionW(MSIHANDLE h, LPCWSTR a) { return simple(F_DO_ACTION, h, 0, 0, w2u(a), NULL); }
MSIAPI MsiDoActionA(MSIHANDLE h, LPCSTR a) { return simple(F_DO_ACTION, h, 0, 0, a2u(a), NULL); }
MSIAPI MsiSequenceW(MSIHANDLE h, LPCWSTR t, int mode) { (void)mode; return simple(F_SEQUENCE, h, 0, 0, w2u(t), NULL); }
MSIAPI MsiSequenceA(MSIHANDLE h, LPCSTR t, int mode) { (void)mode; return simple(F_SEQUENCE, h, 0, 0, a2u(t), NULL); }
__declspec(dllexport) int WINAPI MsiEvaluateConditionW(MSIHANDLE h, LPCWSTR c) { return (int)simple(F_EVAL_COND, h, 0, 0, w2u(c), NULL); }
__declspec(dllexport) int WINAPI MsiEvaluateConditionA(MSIHANDLE h, LPCSTR c) { return (int)simple(F_EVAL_COND, h, 0, 0, a2u(c), NULL); }
__declspec(dllexport) int WINAPI MsiProcessMessage(MSIHANDLE h, int type, MSIHANDLE rec) { return (int)simple(F_MESSAGE, h, (uint32_t)type, rec, NULL, NULL); }
MSIAPI MsiSetInstallLevel(MSIHANDLE h, int level) { return simple(F_SET_INSTALL_LEVEL, h, (uint32_t)level, 0, NULL, NULL); }
__declspec(dllexport) MSIHANDLE WINAPI MsiGetLastErrorRecord(void) { return 0; }
MSIAPI MsiVerifyDiskSpace(MSIHANDLE h) { (void)h; return ERROR_SUCCESS; }

static UINT state_call(uint32_t fn, MSIHANDLE h, char *name, int *installed, int *action)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = fn;
    c.in[0] = h;
    c.s[0] = name;
    dispatch(&c);
    free(name);
    if (!c.ret) {
        if (installed) *installed = (int)c.out[0];
        if (action) *action = (int)c.out[1];
    }
    UINT r = c.ret;
    call_free(&c);
    return r;
}

MSIAPI MsiGetFeatureStateW(MSIHANDLE h, LPCWSTR f, int *i, int *a) { return state_call(F_FEATURE_STATE, h, w2u(f), i, a); }
MSIAPI MsiGetFeatureStateA(MSIHANDLE h, LPCSTR f, int *i, int *a) { return state_call(F_FEATURE_STATE, h, a2u(f), i, a); }
MSIAPI MsiGetComponentStateW(MSIHANDLE h, LPCWSTR f, int *i, int *a) { return state_call(F_COMP_STATE, h, w2u(f), i, a); }
MSIAPI MsiGetComponentStateA(MSIHANDLE h, LPCSTR f, int *i, int *a) { return state_call(F_COMP_STATE, h, a2u(f), i, a); }
MSIAPI MsiSetFeatureStateW(MSIHANDLE h, LPCWSTR f, int s) { return simple(F_SET_FEATURE_STATE, h, (uint32_t)s, 0, w2u(f), NULL); }
MSIAPI MsiSetFeatureStateA(MSIHANDLE h, LPCSTR f, int s) { return simple(F_SET_FEATURE_STATE, h, (uint32_t)s, 0, a2u(f), NULL); }
MSIAPI MsiSetComponentStateW(MSIHANDLE h, LPCWSTR f, int s) { return simple(F_SET_COMP_STATE, h, (uint32_t)s, 0, w2u(f), NULL); }
MSIAPI MsiSetComponentStateA(MSIHANDLE h, LPCSTR f, int s) { return simple(F_SET_COMP_STATE, h, (uint32_t)s, 0, a2u(f), NULL); }

MSIAPI MsiGetFeatureCostW(MSIHANDLE h, LPCWSTR f, int tree, int state, int *cost)
{
    (void)h; (void)f; (void)tree; (void)state;
    if (cost) *cost = 0;
    return ERROR_SUCCESS;
}

MSIAPI MsiGetFeatureCostA(MSIHANDLE h, LPCSTR f, int tree, int state, int *cost)
{
    (void)h; (void)f; (void)tree; (void)state;
    if (cost) *cost = 0;
    return ERROR_SUCCESS;
}

MSIAPI MsiEnumComponentCostsW(MSIHANDLE h, LPCWSTR comp, DWORD index, int state, LPWSTR drive, DWORD *pcch, int *cost, int *temp)
{
    (void)h; (void)comp; (void)state;
    if (index > 0) return ERROR_NO_MORE_ITEMS;
    if (cost) *cost = 0;
    if (temp) *temp = 0;
    return out_w("C:", drive, pcch);
}

/* -----------------------------------------------------------------------
 * Exports: summary information
 * ----------------------------------------------------------------------- */
MSIAPI MsiGetSummaryInformationW(MSIHANDLE db, LPCWSTR path, UINT count, MSIHANDLE *out)
{
    (void)count;
    return handle_call(F_SUMINFO, db, 0, w2u(path), out);
}

MSIAPI MsiGetSummaryInformationA(MSIHANDLE db, LPCSTR path, UINT count, MSIHANDLE *out)
{
    (void)count;
    return handle_call(F_SUMINFO, db, 0, a2u(path), out);
}

MSIAPI MsiSummaryInfoGetPropertyCount(MSIHANDLE h, UINT *n)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = F_SUMINFO_COUNT;
    c.in[0] = h;
    dispatch(&c);
    if (!c.ret && n) *n = c.out[0];
    UINT r = c.ret;
    call_free(&c);
    return r;
}

static UINT suminfo_get(MSIHANDLE h, UINT pid, UINT *type, INT *iv, FILETIME *ft, void *buf, DWORD *pcch, bool wide)
{
    Call c;
    memset(&c, 0, sizeof(c));
    c.fn = F_SUMINFO_GET;
    c.in[0] = h;
    c.in[1] = pid;
    dispatch(&c);
    UINT r = c.ret;
    if (!r) {
        if (type) *type = c.out[0];
        if (iv) *iv = (INT)c.out[1];
        if (ft) { ft->dwLowDateTime = c.out[2]; ft->dwHighDateTime = c.out[3]; }
        if (c.out[0] == 30) r = wide ? out_w(c.out_s, buf, pcch) : out_a(c.out_s, buf, pcch);
    }
    call_free(&c);
    return r;
}

MSIAPI MsiSummaryInfoGetPropertyW(MSIHANDLE h, UINT pid, UINT *type, INT *iv, FILETIME *ft, LPWSTR buf, DWORD *pcch)
{
    return suminfo_get(h, pid, type, iv, ft, buf, pcch, true);
}

MSIAPI MsiSummaryInfoGetPropertyA(MSIHANDLE h, UINT pid, UINT *type, INT *iv, FILETIME *ft, LPSTR buf, DWORD *pcch)
{
    return suminfo_get(h, pid, type, iv, ft, buf, pcch, false);
}

MSIAPI MsiSummaryInfoSetPropertyW(MSIHANDLE h, UINT pid, UINT type, INT iv, FILETIME *ft, LPCWSTR s)
{
    (void)h; (void)pid; (void)type; (void)iv; (void)ft; (void)s;
    return ERROR_SUCCESS;
}

MSIAPI MsiSummaryInfoPersist(MSIHANDLE h) { (void)h; return ERROR_SUCCESS; }
