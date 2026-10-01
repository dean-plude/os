/*
 * sort.exe — sort lines
 *
 *   sort [/r] [/+n] [file] [/o outfile]
 *
 * Case-insensitive, like Windows; /+n compares from column n.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int g_rev, g_col;

static int cmp(const void *a, const void *b)
{
    const char *x = *(const char **)a, *y = *(const char **)b;
    size_t lx = strlen(x), ly = strlen(y);
    x += (size_t)g_col < lx ? (size_t)g_col : lx;
    y += (size_t)g_col < ly ? (size_t)g_col : ly;
    int r = _stricmp(x, y);
    if (!r) r = strcmp(x, y);
    return g_rev ? -r : r;
}

int main(int argc, char **argv)
{
    const char *in = 0, *out = 0;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/r") || !_stricmp(argv[i], "/reverse")) g_rev = 1;
        else if (argv[i][0] == '/' && argv[i][1] == '+') g_col = atoi(argv[i] + 2) - 1;
        else if (!_stricmp(argv[i], "/o") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '/') {}
        else in = argv[i];
    }
    if (g_col < 0) g_col = 0;
    FILE *f = in ? fopen(in, "rb") : stdin;
    if (!f) { fprintf(stderr, "The system cannot find the file specified.\n"); return 1; }
    size_t n = 0, cap = 1024;
    char **lines = malloc(cap * sizeof(char *));
    char buf[8192];
    while (fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (n == cap) { cap *= 2; lines = realloc(lines, cap * sizeof(char *)); }
        lines[n++] = _strdup(buf);
    }
    if (in) fclose(f);
    qsort(lines, n, sizeof(char *), cmp);
    FILE *o = out ? fopen(out, "wb") : stdout;
    if (!o) { fprintf(stderr, "Access is denied.\n"); return 1; }
    for (size_t i = 0; i < n; i++) fprintf(o, "%s\r\n", lines[i]);
    if (out) fclose(o);
    return 0;
}
