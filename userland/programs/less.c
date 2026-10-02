/*
 * less.exe — a pager: a screenful at a time, for git and friends
 *
 *   less [-options] [+n] [file ...]
 *
 * git runs "less" (with LESS=FRX) to show `log`, `diff` and `config --list`
 * on a console, and MinGit ships none.  After each screenful it asks what
 * next and takes single keys, as less does: Space (or f, Page Down) the
 * next page, Enter (or j, Down) one more line, d half a page, "/text" and
 * Enter skips to the next line containing text, q (or Esc) quits.  On a
 * console that only hands over whole lines, the answer ends with Enter
 * (a number shows that many more lines).  Input that fits on one screen
 * goes straight through, as with less -F, and so does everything when the
 * output is not a console.  Color and other escape sequences pass through
 * (less -R); options are accepted and otherwise ignored.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static HANDLE g_in = INVALID_HANDLE_VALUE;    /* the console the keys come from */
static int g_rows = 25, g_cols = 80;

/* The rows a line takes on screen: escape sequences take none, and long
 * lines wrap */
static int screen_rows(const char *s)
{
    int col = 0, esc = 0;
    for (; *s && *s != '\n'; s++) {
        unsigned char c = (unsigned char)*s;
        if (esc == 1) { esc = c == '[' ? 2 : 0; continue; }
        if (esc == 2) { if (c >= 0x40 && c <= 0x7E) esc = 0; continue; }
        if (c == 0x1B) { esc = 1; continue; }
        if (c == '\r') continue;
        if ((c & 0xC0) == 0x80) continue;             /* UTF-8 continuation */
        col += c == '\t' ? 8 - col % 8 : 1;
    }
    return col <= g_cols ? 1 : (col + g_cols - 1) / g_cols;
}

/* Console input records (the headers here are thin) */
typedef struct {
    WORD EventType;
    union {
        struct { BOOL bKeyDown; WORD wRepeatCount, wVirtualKeyCode, wVirtualScanCode;
                 union { WCHAR UnicodeChar; CHAR AsciiChar; } uChar; DWORD dwControlKeyState; } Key;
        BYTE pad[16];
    } Event;
} LESS_INPUT_RECORD;
WINBASEAPI BOOL WINAPI ReadConsoleInputW(HANDLE h, LESS_INPUT_RECORD *rec, DWORD n, LPDWORD read);
#define LESS_LINE_INPUT 0x2
#define LESS_ECHO_INPUT 0x4

#define PROMPT_KEYS "-- More -- (Space: next page, Enter: next line, /text: find, q: quit) "
#define PROMPT_LINE "-- More -- (Enter: next page, /text: find, q: quit) "

/* The answer typed as a line: Enter, a number, "/text" or q */
static int ask_line(char *find, int cap)
{
    char line[256];
    DWORD got = 0;
    if (!ReadFile(g_in, line, sizeof(line) - 1, &got, NULL) || !got) return 0;
    line[got] = '\0';
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    s[strcspn(s, "\r\n")] = '\0';
    if (*s == 'q' || *s == 'Q') return 0;
    if (*s == '/' && s[1]) {
        strncpy(find, s + 1, cap - 1);
        find[cap - 1] = '\0';
        return -1;
    }
    if (*s >= '1' && *s <= '9') return atoi(s);
    return g_rows - 1;
}

/* One key pressed: its character, or a virtual key code + 0x10000; -1
 * when the console is gone */
static int read_key(void)
{
    for (;;) {
        LESS_INPUT_RECORD r;
        DWORD got = 0;
        if (!ReadConsoleInputW(g_in, &r, 1, &got) || !got) return -1;
        if (r.EventType != 1 || !r.Event.Key.bKeyDown) continue;          /* KEY_EVENT presses */
        if (r.Event.Key.uChar.UnicodeChar) return r.Event.Key.uChar.UnicodeChar;
        WORD vk = r.Event.Key.wVirtualKeyCode;
        if (vk == VK_NEXT || vk == VK_DOWN || vk == VK_ESCAPE) return 0x10000 + vk;
    }
}

/* Ask what next; returns the rows to show, 0 to quit, -1 for a search
 * (the text left in find) */
static int ask(char *find, int cap)
{
    DWORD mode = 0, now = 0;
    bool keys = GetConsoleMode(g_in, &mode) &&
                SetConsoleMode(g_in, mode & ~(DWORD)(LESS_LINE_INPUT | LESS_ECHO_INPUT)) &&
                GetConsoleMode(g_in, &now) && !(now & LESS_LINE_INPUT);
    if (!keys) {                                    /* whole lines only */
        fputs(PROMPT_LINE, stdout);
        fflush(stdout);
        return ask_line(find, cap);
    }
    fputs(PROMPT_KEYS, stdout);
    fflush(stdout);
    int n = -2;
    while (n == -2) {
        int k = read_key();
        switch (k) {
        case -1: case 'q': case 'Q': case 0x10000 + VK_ESCAPE: n = 0; break;
        case ' ': case 'f': case 0x10000 + VK_NEXT: n = g_rows - 1; break;
        case '\r': case '\n': case 'j': case 0x10000 + VK_DOWN: n = 1; break;
        case 'd': n = (g_rows - 1) / 2; break;
        case '/':                                   /* the text, as a line */
            fputs("\r\x1b[K/", stdout);
            fflush(stdout);
            SetConsoleMode(g_in, mode);
            {
                char line[256];
                DWORD got = 0;
                if (!ReadFile(g_in, line, sizeof(line) - 1, &got, NULL)) got = 0;
                line[got] = '\0';
                line[strcspn(line, "\r\n")] = '\0';
                if (!line[0]) { n = g_rows - 1; break; }
                strncpy(find, line, cap - 1);
                find[cap - 1] = '\0';
            }
            return -1;
        }
    }
    SetConsoleMode(g_in, mode);
    fputs("\r\x1b[K", stdout);                      /* the prompt goes */
    fflush(stdout);
    return n;
}

/* Returns false when the reader quit */
static bool page(FILE *f, long skip)
{
    static int left = -1;                           /* rows until the next prompt */
    static char find[128];
    char line[8192];
    if (left < 0) left = g_rows - 1;
    while (fgets(line, sizeof(line), f)) {
        if (skip > 0) { skip--; continue; }
        if (find[0]) {                              /* searching: skip to a match */
            if (!strstr(line, find)) continue;
            find[0] = '\0';
        }
        if (g_in != INVALID_HANDLE_VALUE) {
            int rows = screen_rows(line);
            if (rows > left && left < g_rows - 1) {
                int n = ask(find, sizeof(find));
                if (!n) return false;
                left = n < 0 ? g_rows - 1 : n;
                if (n < 0 && !strstr(line, find)) continue;
                find[0] = '\0';
            }
            left -= rows;
            if (left < 0) left = 0;
        }
        fputs(line, stdout);
    }
    return true;
}

int main(int argc, char **argv)
{
    long skip = 0;
    int files = 0, named = 0;
    DWORD mode;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(out, &mode)) {               /* paging only on a console */
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(out, &info)) {
            g_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
            g_cols = info.srWindow.Right - info.srWindow.Left + 1;
            if (g_rows < 3) g_rows = 3;
            if (g_cols < 10) g_cols = 10;
        }
        g_in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') { skip = atol(argv[i] + 1); continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;            /* options */
        named++;
        FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "%s: No such file or directory\n", argv[i]); continue; }
        files++;
        bool more = page(f, skip);
        if (f != stdin) fclose(f);
        if (!more) break;
    }
    if (!named) page(stdin, skip);
    else if (!files) return 1;
    fflush(stdout);
    return 0;
}
