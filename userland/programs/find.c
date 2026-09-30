/*
 * find.exe — lines that contain a string
 *
 *   find [/v] [/c] [/n] [/i] "string" [file ...]
 *
 * Reads the files, or standard input.  Exit code 0 if a line matched.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int opt_v, opt_c, opt_n, opt_i;
static const char *needle;

static int contains(const char *line)
{
    size_t n = strlen(needle);
    if (!n) return 0;                           /* no line contains "" (find /c /v "" counts lines) */
    for (const char *s = line; *s; s++) {
        if (opt_i ? !_strnicmp(s, needle, n) : !strncmp(s, needle, n)) return 1;
    }
    return 0;
}

static int search(FILE *f, const char *name)
{
    char line[8192];
    long count = 0, no = 0;
    if (name) printf("\n---------- %s", name);
    if (name && !opt_c) printf("\n");
    while (fgets(line, sizeof(line), f)) {
        no++;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (contains(line) == !opt_v) {
            count++;
            if (opt_c) continue;
            if (opt_n) printf("[%ld]", no);
            printf("%s\n", line);
        }
    }
    if (opt_c) printf(name ? ": %ld\n" : "%ld\n", count);
    return count > 0;
}

int main(int argc, char **argv)
{
    int files = 0, found = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && argv[i][1] && !argv[i][2]) {
            switch (toupper((unsigned char)argv[i][1])) {
            case 'V': opt_v = 1; break;
            case 'C': opt_c = 1; break;
            case 'N': opt_n = 1; break;
            case 'I': opt_i = 1; break;
            default: fprintf(stderr, "FIND: Invalid switch\n"); return 2;
            }
        } else if (!needle) needle = argv[i];
        else files++;
    }
    if (!needle) { fprintf(stderr, "FIND: Parameter format not correct\n"); return 2; }
    if (!files) return search(stdin, 0) ? 0 : 1;
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' || argv[i] == needle) continue;
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "File not found - %s\n", argv[i]); bad = 1; continue; }
        char up[MAX_PATH];
        snprintf(up, sizeof(up), "%s", argv[i]);
        for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
        if (search(f, up)) found = 1;
        fclose(f);
    }
    return bad && !found ? 2 : found ? 0 : 1;
}
