/*
 * sqltest.c — run SQL queries against a package on the host, the way
 * custom actions do through MsiDatabaseOpenView:
 *
 *   gcc -I userland/msi -o sqltest tools/msitest/sqltest.c \
 *       userland/msi/{cfb,msidb,sql}.c && ./sqltest package.msi "SELECT ..." [param]
 *
 * Without a query it runs a built-in set (selects, joins, ORDER BY,
 * parameters, temporary rows, streams) and checks the results.
 */
#include "msi_int.h"

static int failures;
static void check(bool ok, const char *what) { printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failures++; }

static int run(MsiDb *db, const char *sql, const char *param, bool print)
{
    MsiView *v;
    char err[128];
    int r = msisql_open(db, sql, &v, err, sizeof(err));
    if (r) { if (print) printf("open: %d (%s)\n", r, err); return -r; }
    MsiRec *p = NULL;
    if (param) { p = msirec_new(1); msirec_set_str(p, 1, param); }
    r = msisql_execute(v, p);
    msirec_free(p);
    if (r) { msisql_close(v); return -r; }
    int n = 0;
    MsiRec *rec;
    while (!msisql_fetch(v, &rec)) {
        if (print) {
            for (int i = 1; i <= rec->n; i++) {
                char b[16];
                if (rec->f[i].type == MSIF_STREAM) printf("%s[stream %zu bytes]", i > 1 ? " | " : "", rec->f[i].size);
                else printf("%s%s", i > 1 ? " | " : "", msirec_is_null(rec, i) ? "<null>" : msirec_str(rec, i, b));
            }
            printf("\n");
        }
        msirec_free(rec);
        n++;
    }
    msisql_close(v);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: sqltest package.msi [query [param]]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    void *d = malloc(sz);
    if (fread(d, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    MsiDb db;
    if (!msidb_open(&db, d, sz)) { printf("open failed\n"); return 1; }
    if (argc >= 3) { int n = run(&db, argv[2], argc > 3 ? argv[3] : NULL, true); printf("(%d rows)\n", n); return n < 0; }

    check(run(&db, "SELECT `Value` FROM `Property` WHERE `Property` = 'ProductCode'", NULL, true) == 1, "select by literal");
    check(run(&db, "SELECT `Property`.`Value` FROM `Property` WHERE `Property`.`Property` = ?", "ProductName", true) == 1, "select by parameter");
    check(run(&db, "SELECT * FROM `Property` WHERE `Property` = 'NoSuchProperty'", NULL, false) == 0, "no match");
    int nfiles = run(&db, "SELECT `File` FROM `File`", NULL, false);
    check(nfiles > 0, "all files");
    check(run(&db, "SELECT `File`.`FileName`, `Component`.`Directory_` FROM `File`, `Component` WHERE `File`.`Component_` = `Component`.`Component`", NULL, false) == nfiles, "join File and Component");
    check(run(&db, "SELECT `Action`, `Sequence` FROM `InstallExecuteSequence` WHERE `Sequence` > 0 ORDER BY `Sequence`", NULL, true) > 5, "ORDER BY");
    check(run(&db, "SELECT `Name`, `Data` FROM `Binary`", NULL, true) > 0, "streams");
    check(run(&db, "SELECT `Action` FROM `InstallExecuteSequence` WHERE `Condition` IS NULL AND `Sequence` < 1000 OR `Action` = 'InstallFinalize'", NULL, false) > 0, "IS NULL, AND, OR");
    check(run(&db, "SELECT `Bogus` FROM `Property`", NULL, false) == -1615, "bad column is a syntax error");
    check(run(&db, "SELECT * FROM `NoTable`", NULL, false) == -1628, "missing table");
    check(run(&db, "INSERT INTO `Property` (`Property`, `Value`) VALUES ('NovaTemp', 'yes') TEMPORARY", NULL, false) == 0, "insert temporary");
    check(run(&db, "SELECT `Value` FROM `Property` WHERE `Property` = 'NovaTemp'", NULL, false) == 1, "temporary row visible");
    check(run(&db, "UPDATE `Property` SET `Value` = 'changed' WHERE `Property` = 'ProductName'", NULL, false) == 0, "update");
    check(run(&db, "SELECT `Value` FROM `Property` WHERE `Value` = 'changed'", NULL, false) == 1, "updated row");
    check(run(&db, "DELETE FROM `Property` WHERE `Property` = 'NovaTemp'", NULL, false) == 0, "delete");
    check(run(&db, "SELECT `Value` FROM `Property` WHERE `Property` = 'NovaTemp'", NULL, false) == 0, "deleted row gone");
    check(run(&db, "CREATE TABLE `NovaT` (`Id` CHAR(72) NOT NULL, `N` SHORT PRIMARY KEY `Id`)", NULL, false) == 0, "create table");
    check(run(&db, "INSERT INTO `NovaT` (`Id`, `N`) VALUES ('a', 5)", NULL, false) == 0, "insert into new table");
    check(run(&db, "SELECT `Id` FROM `NovaT` WHERE `N` = 5", NULL, false) == 1, "select from new table");
    printf("%s\n", failures ? "FAILED" : "all passed");
    msisql_free_temp(&db);
    msidb_close(&db);
    free(d);
    return failures != 0;
}
