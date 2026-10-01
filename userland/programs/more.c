/*
 * more.exe — show text (the Terminal scrolls, so it all goes through)
 *
 *   more [+n] [file ...]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy(FILE *f, long skip)
{
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        if (skip > 0) { skip--; continue; }
        fputs(line, stdout);
    }
}

int main(int argc, char **argv)
{
    long skip = 0;
    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') { skip = atol(argv[i] + 1); continue; }
        if (argv[i][0] == '/') continue;
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "Cannot access file %s\n", argv[i]); continue; }
        copy(f, skip);
        fclose(f);
        files++;
    }
    if (!files) copy(stdin, skip);
    return 0;
}
