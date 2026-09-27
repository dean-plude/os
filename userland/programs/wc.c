/* wc.exe FILE... — count lines, words and bytes (file I/O) */
#include <stdio.h>
#include <ctype.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: wc FILE...\n"); return 1; }
    long tl = 0, tw = 0, tb = 0;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); rc = 1; continue; }
        long l = 0, w = 0, b = 0;
        int c, in = 0;
        while ((c = fgetc(f)) != EOF) {
            b++;
            if (c == '\n') l++;
            if (isspace(c)) in = 0; else if (!in) { in = 1; w++; }
        }
        fclose(f);
        printf("%7ld %7ld %7ld %s\n", l, w, b, argv[i]);
        tl += l; tw += w; tb += b;
    }
    if (argc > 2) printf("%7ld %7ld %7ld total\n", tl, tw, tb);
    return rc;
}
