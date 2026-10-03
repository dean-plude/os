/*
 * xftest.c — apply transforms to a package on the host and print the
 * tables, sorted, to compare with what tools/msitest/mkmsi.py meant:
 *
 *   gcc -I userland/msi -o xftest tools/msitest/xftest.c \
 *       userland/msi/{cfb,msidb}.c
 *   ./xftest package.msi [file.mst | patch.msp:STORAGE] ...
 */
#include "msi_int.h"

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    void *d = malloc((size_t)sz + 1);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return NULL; }
    fclose(f);
    *size = (size_t)sz;
    return d;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv)
{
    size_t sz;
    void *d = slurp(argv[1], &sz);
    MsiDb db;
    if (!d || !msidb_open(&db, d, sz)) { printf("open failed\n"); return 1; }
    for (int i = 2; i < argc; i++) {
        char path[512], *storage = NULL;
        snprintf(path, sizeof(path), "%s", argv[i]);
        char *colon = strrchr(path, ':');
        if (colon) { *colon = 0; storage = colon + 1; }
        size_t xs;
        void *x = slurp(path, &xs);
        MsiFile *f = x ? msifile_load(x, xs) : NULL;
        if (!f) { printf("%s: not a compound file\n", path); return 1; }
        char err[256];
        int rc = msidb_apply_transform(&db, f, storage, 0, err, sizeof(err));
        printf("apply %s%s%s: %d %s\n", path, storage ? ":" : "", storage ? storage : "", rc, err);
        msifile_release(f);
    }
    for (int i = 0; i < db.ntables; i++) {
        MsiTable *t = &db.tables[i];
        printf("== %s", t->name);
        for (int c = 0; c < t->ncols; c++) printf(" %s:%04x", t->cols[c].name, t->cols[c].type);
        printf("\n");
        char **lines = calloc((size_t)t->nrows + 1, sizeof(char *));
        for (int r = 0; r < t->nrows; r++) {
            char line[4096];
            int n = 0;
            for (int c = 0; c < t->ncols; c++) {
                char b[16];
                n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s", c ? "|" : "", msidb_str(&db, t, r, c, b));
            }
            lines[r] = strdup(line);
        }
        qsort(lines, (size_t)t->nrows, sizeof(char *), cmp);
        for (int r = 0; r < t->nrows; r++) printf("  %s\n", lines[r]);
    }
    if (argc > 2) {
        size_t n;
        void *s = msidb_read_stream(&db, "PCW_CAB_NovaPatch", &n);
        printf("patch cabinet: %s\n", s ? "found" : "none");
        free(s);
    }
    msidb_close(&db);
    return 0;
}
