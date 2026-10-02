/*
 * hosttest.c — exercise the Windows Installer's package readers on the
 * host: dump a package's tables, evaluate a few conditions, and extract
 * its first embedded cabinet.
 *
 *   gcc -I userland/msi -o msitest tools/msitest/hosttest.c \
 *       userland/msi/{cfb,msidb,cab,lzx,cond}.c && ./msitest package.msi
 *
 * tools/msitest/make_package.sh builds test packages with msitools.
 */
#include "msi_int.h"
static const char *prop(void *ctx, const char *n) { (void)ctx; if (!strcmp(n,"ProductVersion")) return "1.2.3"; if (!strcmp(n,"Installed")) return ""; if (!strcmp(n,"VersionNT")) return "1000"; return ""; }
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    void *d = malloc(sz);
    if (fread(d, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    MsiDb db;
    if (!msidb_open(&db, d, sz)) { printf("open failed\n"); return 1; }
    printf("codepage %d strref %d strings %d tables %d\n", db.codepage, db.strref_size, db.nstrings, db.ntables);
    for (int i = 0; i < db.ntables; i++) {
        MsiTable *t = &db.tables[i];
        printf("== %s (%d rows)\n", t->name, t->nrows);
        for (int r = 0; r < t->nrows && r < 30; r++) {
            for (int c = 0; c < t->ncols; c++) { char b[16]; printf("%s%s", c ? " | " : "  ", msidb_str(&db, t, r, c, b)); }
            printf("\n");
        }
    }
    printf("cond1=%d cond2=%d cond3=%d cond4=%d\n",
        msi_condition("NOT Installed AND ProductVersion = \"1.2.3\"", prop, 0),
        msi_condition("NEVER", prop, 0), msi_condition("VersionNT >= 600 AND (Installed OR NOT Installed)", prop, 0),
        msi_condition("ProductVersion >< \"2.3\" AND ProductVersion << \"1.\"", prop, 0));
    /* the first Media row's embedded cabinet */
    MsiTable *media = msidb_table(&db, "Media");
    char nb[16];
    const char *cabname = media ? msidb_str(&db, media, 0, msidb_col(media, "Cabinet"), nb) : "";
    if (cabname[0] != '#') { printf("no embedded cabinet\n"); msidb_close(&db); return 0; }
    size_t csz = 0; void *cab = cfb_read(&db.cfb, cabname + 1, &csz);
    printf("cab stream %s: %zu bytes\n", cabname, csz);
    if (!cab) return 1;
    Cab c; if (!cab_open(&c, cab, csz)) { printf("cab open failed\n"); return 1; }
    printf("folders %d files %d\n", c.nfolders, c.nfiles);
    for (int i = 0; i < c.nfiles; i++) printf("  %s size %u off %u folder %d\n", c.files[i].name, c.files[i].size, c.files[i].folder_off, c.files[i].folder);
    CabReader r; if (!cab_reader_start(&r, &c, 0)) { printf("reader: %s\n", r.error); return 1; }
    uint32_t cap = 1 << 20, n = 0; uint8_t *all = malloc(cap);
    while (cab_reader_next(&r)) {
        if (n + r.out_len > cap) { cap *= 2; all = realloc(all, cap); }
        memcpy(all + n, r.out, r.out_len); n += r.out_len;
    }
    printf("decompressed %u bytes (%s)\n", n, r.error);
    for (int i = 0; i < c.nfiles; i++) { char p[300]; snprintf(p, 300, "out_%s", c.files[i].name); FILE *o = fopen(p, "wb"); fwrite(all + c.files[i].folder_off, 1, c.files[i].size, o); fclose(o); }
    msidb_close(&db);
    return 0;
}
