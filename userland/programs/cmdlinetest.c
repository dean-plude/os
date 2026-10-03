/* cmdlinetest.exe — long command lines reach a program whole.
 *
 * "cmdlinetest len=N sum=S ARG..." checks the command line it was given:
 * GetCommandLineW holds N characters from "len=" to its end (GetCommandLineA
 * as many, in UTF-8), the arguments after sum= (the CRT's argv, MSVC
 * quoting rules) hash to S (FNV-1a over their UTF-8 bytes, each followed by
 * a 0) and the last one is END.  The self-test types such a line, over 1,000
 * characters, into the Terminal.
 *
 * "cmdlinetest spawn" starts copies of itself that way with CreateProcessW
 * (1,000, 8,191 and 32,766 characters, the longest Windows takes: spaces,
 * quotes, backslashes and characters outside ASCII in the arguments),
 * CreateProcessA, and cmd.exe /c (8,000 characters), and checks that
 * 32,767 characters fail with ERROR_FILENAME_EXCED_RANGE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static unsigned fnv(unsigned h, const char *s)
{
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return (h ^ 0) * 16777619u;
}

/* The child's side: is the command line the one described? */
static int check_self(int argc, char **argv)
{
    const WCHAR *w = GetCommandLineW();
    const char *a = GetCommandLineA();
    const WCHAR *wl = wcsstr(w, L"len=");
    const char *al = strstr(a, "len=");
    long want = strtol(argv[1] + 4, NULL, 10);
    unsigned sum = argc > 2 && !strncmp(argv[2], "sum=", 4) ? (unsigned)strtoul(argv[2] + 4, NULL, 16) : 0;
    long wn = wl ? (long)wcslen(wl) : -1;
    unsigned h = 2166136261u;
    for (int i = 3; i < argc; i++) h = fnv(h, argv[i]);
    int utf8_ok = al && (long)strlen(al) == (long)WideCharToMultiByte(CP_UTF8, 0, wl, -1, NULL, 0, NULL, NULL) - 1;
    int ok = wn == want && utf8_ok && h == sum && argc > 3 && !strcmp(argv[argc - 1], "END");
    printf("cmdlinetest: command line of %ld characters, %d arguments: %s\n", (long)wcslen(w), argc - 1, ok ? "ok" : "WRONG");
    if (!ok) printf("  len %ld (want %ld), utf-8 %s, sum %08x (want %08x), last \"%.20s\"\n",
                    wn, want, utf8_ok ? "same" : "differs", h, sum, argc > 1 ? argv[argc - 1] : "");
    fflush(stdout);
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------- the parent */
typedef struct { char *s; size_t n, cap; } Buf;

static void put(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

/* @arg quoted as CommandLineToArgvW / the CRT read it back */
static void put_arg(Buf *b, const char *arg)
{
    put(b, " ", 1);
    if (*arg && !strpbrk(arg, " \t\"")) { put(b, arg, strlen(arg)); return; }
    put(b, "\"", 1);
    for (const char *s = arg;; s++) {
        size_t bs = 0;
        while (*s == '\\') { bs++; s++; }
        if (!*s) { for (size_t i = 0; i < 2 * bs; i++) put(b, "\\", 1); break; }
        if (*s == '"') { for (size_t i = 0; i < 2 * bs + 1; i++) put(b, "\\", 1); }
        else for (size_t i = 0; i < bs; i++) put(b, "\\", 1);
        put(b, s, 1);
    }
    put(b, "\"", 1);
}

static int units(const char *s) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0) - 1; }

/* "cmdlinetest len=N sum=S ARGS... xxx END", @total UTF-16 characters long
 * (@prog first; ASCII arguments only with @ascii) */
static char *make_line(const char *prog, int total, int ascii)
{
    static const char *const fixed[] = {
        "C:\\Program Files\\A Folder With Spaces\\and a long file name.txt",
        "say \"hello\" twice",
        "C:\\trailing backslash\\",
        "back\\\\slashes\\in\\the\\middle",
        "",
        "tab\there",
    };
    static const char *const wide[] = { "caf\xc3\xa9", "\xe4\xb8\xad\xe6\x96\x87", "smile \xf0\x9f\x98\x80" };
    Buf args = { 0 };
    unsigned h = 2166136261u;
    char word[32];
    int nfixed = sizeof(fixed) / sizeof(fixed[0]), nwide = ascii ? 0 : 3;
    for (int i = 0; i < nfixed; i++) { put_arg(&args, fixed[i]); h = fnv(h, fixed[i]); }
    for (int i = 0; i < nwide; i++) { put_arg(&args, wide[i]); h = fnv(h, wide[i]); }
    /* the head: prog + " len=NNNNN sum=XXXXXXXX" (23 more) */
    int head = units(prog) + 23;
    int u = units(args.s);
    for (int i = 1; total - head - u - 5 > 8; i++, u += 7) {          /* " w00001": leaving room for " x END" */
        snprintf(word, sizeof(word), "w%05d", i);
        put_arg(&args, word);
        h = fnv(h, word);
    }
    int pad = total - head - u - 5;                                    /* " " + x... + " END" */
    char *x = malloc((size_t)pad + 1);
    memset(x, 'x', (size_t)pad);
    x[pad] = 0;
    put_arg(&args, x);
    h = fnv(h, x);
    free(x);
    put_arg(&args, "END");
    h = fnv(h, "END");
    Buf line = { 0 };
    char hd[64];
    put(&line, prog, strlen(prog));
    snprintf(hd, sizeof(hd), " len=%05d sum=%08x", total - units(prog) - 1, h);
    put(&line, hd, strlen(hd));
    put(&line, args.s, args.n);
    free(args.s);
    return line.s;
}

static DWORD run_child(const WCHAR *app, char *line, int use_a, DWORD *err)
{
    STARTUPINFOA sa = { sizeof(sa) };
    STARTUPINFOW sw = { sizeof(sw) };
    PROCESS_INFORMATION pi;
    BOOL ok;
    *err = 0;
    if (use_a) {
        char app_a[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, app, -1, app_a, MAX_PATH, NULL, NULL);
        ok = CreateProcessA(app_a, line, NULL, NULL, FALSE, 0, NULL, NULL, &sa, &pi);
    } else {
        int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
        WCHAR *w = malloc(sizeof(WCHAR) * (size_t)n);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, w, n);
        ok = CreateProcessW(app, w, NULL, NULL, FALSE, 0, NULL, NULL, &sw, &pi);
        free(w);
    }
    if (!ok) { *err = GetLastError(); return (DWORD)-1; }
    DWORD code = 99;
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static void spawn_tests(void)
{
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    DWORD err;
    static const int lens[] = { 1000, 8191, 32766 };
    for (int i = 0; i < 3; i++) {
        char *line = make_line("cmdlinetest", lens[i], 0);
        char what[64];
        snprintf(what, sizeof(what), "CreateProcessW, %d characters", lens[i]);
        CHECK(what, units(line) == lens[i] && run_child(self, line, 0, &err) == 0);
        free(line);
    }
    char *line = make_line("cmdlinetest", 8191, 1);
    CHECK("CreateProcessA, 8191 characters", run_child(self, line, 1, &err) == 0);
    free(line);

    line = make_line("cmdlinetest", 32767, 0);                      /* one too many */
    DWORD r = run_child(self, line, 0, &err);
    CHECK("CreateProcessW refuses 32767 characters", r == (DWORD)-1 && err == ERROR_FILENAME_EXCED_RANGE);
    if (r != (DWORD)-1 || err != ERROR_FILENAME_EXCED_RANGE) printf("  result %lu, error %lu\n", (unsigned long)r, (unsigned long)err);
    free(line);

    /* through cmd.exe /c: it runs the rest of its line as the command */
    WCHAR cmd[MAX_PATH];
    GetSystemDirectoryW(cmd, MAX_PATH);
    wcscat(cmd, L"\\cmd.exe");
    char *inner = make_line("cmdlinetest", 8000, 1);
    Buf c = { 0 };
    put(&c, "cmd.exe /d /c ", 14);
    put(&c, inner, strlen(inner));
    CHECK("cmd.exe /c, 8000 characters", run_child(cmd, c.s, 0, &err) == 0);
    free(c.s);
    free(inner);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strncmp(argv[1], "len=", 4)) return check_self(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "spawn")) {
        spawn_tests();
        printf("cmdlinetest spawn: %d passed, %d failed\n", pass, fail);
        return fail ? 1 : 0;
    }
    printf("usage: cmdlinetest spawn | cmdlinetest len=N sum=S ARG... END\n");
    return 2;
}
