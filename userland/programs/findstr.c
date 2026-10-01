/*
 * findstr.exe — search for strings or regular expressions
 *
 *   findstr [/i] [/v] [/n] [/c] [/m] [/b] [/e] [/x] [/l] [/r] [/s]
 *           [/c:"string"] strings [file ...]
 *
 * Several space-separated strings match any of them; /c: takes one string
 * literally with its spaces.  Regular expressions (the default unless /l):
 * . * ^ $ [class] [^class] \< \> and \ to escape.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int opt_i, opt_v, opt_n, opt_m, opt_b, opt_e, opt_x, opt_lit, opt_s, opt_count;
static char *pats[64];
static int npats;

static int ceq(char a, char b) { return opt_i ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b; }

/* One atom at @p against character @c; *len gets the atom's length */
static int atom(const char *p, char c, int *len)
{
    if (*p == '\\' && p[1]) { *len = 2; return ceq(p[1], c); }
    if (*p == '.') { *len = 1; return c != 0; }
    if (*p == '[') {
        const char *q = p + 1;
        int neg = *q == '^';
        if (neg) q++;
        int ok = 0;
        while (*q && *q != ']') {
            if (q[1] == '-' && q[2] && q[2] != ']') {
                char lo = q[0], hi = q[2];
                if (opt_i ? (tolower((unsigned char)c) >= tolower((unsigned char)lo) && tolower((unsigned char)c) <= tolower((unsigned char)hi))
                          : (c >= lo && c <= hi)) ok = 1;
                q += 3;
            } else { if (ceq(*q, c)) ok = 1; q++; }
        }
        *len = (int)(q - p) + (*q == ']');
        return c && (neg ? !ok : ok);
    }
    *len = 1;
    return ceq(*p, c);
}

static int word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static int match_here(const char *p, const char *s, const char *line)
{
    for (;;) {
        if (!*p) return 1;
        if (p[0] == '$' && !p[1]) return !*s;
        if (p[0] == '\\' && p[1] == '<') { if (s != line && word_char(s[-1])) return 0; p += 2; continue; }
        if (p[0] == '\\' && p[1] == '>') { if (word_char(*s)) return 0; p += 2; continue; }
        int len;
        atom(p, 'x', &len);
        if (p[len] == '*') {
            const char *t = s;
            while (*t && atom(p, *t, &len)) t++;
            for (; t >= s; t--) if (match_here(p + len + 1, t, line)) return 1;
            return 0;
        }
        if (!*s || !atom(p, *s, &len)) return 0;
        p += len;
        s++;
    }
}

static int match_regex(const char *p, const char *line)
{
    if (*p == '^') return match_here(p + 1, line, line);
    for (const char *s = line;; s++) {
        if (match_here(p, s, line)) return 1;
        if (!*s) return 0;
    }
}

static int match_lit(const char *p, const char *line)
{
    size_t n = strlen(p), l = strlen(line);
    if (opt_x) return n == l && (opt_i ? !_stricmp(p, line) : !strcmp(p, line));
    for (size_t i = 0; i + n <= l; i++) {
        if (opt_b && i) break;
        if (opt_e && i + n != l) continue;
        if (opt_i ? !_strnicmp(line + i, p, n) : !strncmp(line + i, p, n)) return 1;
    }
    return 0;
}

static int matches(const char *line)
{
    for (int k = 0; k < npats; k++) {
        if (opt_lit) { if (match_lit(pats[k], line)) return 1; continue; }
        char pat[1024];
        snprintf(pat, sizeof(pat), "%s%s%s", (opt_b || opt_x) && pats[k][0] != '^' ? "^" : "", pats[k],
                 (opt_e || opt_x) && pats[k][strlen(pats[k]) - 1] != '$' ? "$" : "");
        if (match_regex(pat, line)) return 1;
    }
    return 0;
}

static int search(FILE *f, const char *name, int many)
{
    char line[8192];
    long no = 0, count = 0;
    while (fgets(line, sizeof(line), f)) {
        no++;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (matches(line) == !opt_v) {
            count++;
            if (opt_m) { printf("%s\n", name); return 1; }
            if (opt_count) continue;
            if (many) printf("%s:", name);
            if (opt_n) printf("%ld:", no);
            printf("%s\n", line);
        }
    }
    if (opt_count) printf(many ? "%s:%ld\n" : "%.0s%ld\n", name, count);
    return count > 0;
}

static int search_path(const char *spec, int many)
{
    int found = 0;
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", spec);
    char *slash = strrchr(dir, '\\');
    const char *mask = slash ? slash + 1 : spec;
    if (slash) slash[1] = 0; else dir[0] = 0;
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof(pat), "%s%s", dir, mask);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char p[MAX_PATH * 2];
            snprintf(p, sizeof(p), "%s%s", dir, fd.cFileName);
            FILE *f = fopen(p, "rb");
            if (!f) continue;
            if (search(f, p, many)) found = 1;
            fclose(f);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (opt_s) {
        snprintf(pat, sizeof(pat), "%s*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                char sub[MAX_PATH * 2];
                snprintf(sub, sizeof(sub), "%s%s\\%s", dir, fd.cFileName, mask);
                if (search_path(sub, 1)) found = 1;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    return found;
}

int main(int argc, char **argv)
{
    int first_file = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '/' && a[1]) {
            if (!_strnicmp(a, "/c:", 3)) { pats[npats++] = (char *)a + 3; opt_lit = opt_lit || 0; continue; }
            if (!_strnicmp(a, "/g:", 3) || !_strnicmp(a, "/f:", 3) || !_strnicmp(a, "/d:", 3) || !_strnicmp(a, "/a:", 3)) continue;
            for (const char *c = a + 1; *c; c++) {
                switch (toupper((unsigned char)*c)) {
                case 'I': opt_i = 1; break; case 'V': opt_v = 1; break; case 'N': opt_n = 1; break;
                case 'M': opt_m = 1; break; case 'B': opt_b = 1; break; case 'E': opt_e = 1; break;
                case 'X': opt_x = 1; break; case 'L': opt_lit = 1; break; case 'R': opt_lit = 0; break;
                case 'S': opt_s = 1; break; case 'C': opt_count = 1; break;
                default: break;
                }
            }
            continue;
        }
        if (!npats) {                                   /* space-separated alternatives */
            char *copy = _strdup(a);
            for (char *t = strtok(copy, " "); t && npats < 64; t = strtok(0, " ")) pats[npats++] = t;
            if (!npats) pats[npats++] = copy;
            continue;
        }
        first_file = i;
        break;
    }
    if (!npats) { fprintf(stderr, "FINDSTR: Bad command line\n"); return 2; }
    int found = 0;
    if (first_file >= argc) return search(stdin, "", 0) ? 0 : 1;
    int many = argc - first_file > 1 || opt_s || strpbrk(argv[first_file], "*?");
    for (int i = first_file; i < argc; i++) if (search_path(argv[i], many)) found = 1;
    return found ? 0 : 1;
}
