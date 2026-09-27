/*
 * terminal.c — Terminal: a small command shell over the RAM disk
 *
 * Output is kept as wrapped lines in a scrollback buffer; the prompt and
 * the line being typed are drawn after it.  Commands run synchronously on
 * the desktop thread.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../ke/printf.h"
#include "../hal/rtc.h"

#define T_COLS   160
#define T_ROWS   400
#define T_HIST   16
#define T_PAD    10
#define T_LINE_H 18

#define T_BG      GDI_C(0x0C, 0x0C, 0x0C)
#define T_FG      GDI_C(0xCC, 0xCC, 0xCC)
#define T_DIM     GDI_C(0x80, 0x80, 0x80)
#define T_ERR     GDI_C(0xF1, 0x4C, 0x4C)
#define T_PROMPT  GDI_C(0x4C, 0xC2, 0xFF)

enum { K_NORMAL, K_ERROR, K_DIM };

typedef struct {
    char     line[T_ROWS][T_COLS + 1];
    UINT8    kind[T_ROWS];
    UINT8    split[T_ROWS];     /* first `split` chars drawn in accent */
    int      count;
    int      scroll;            /* lines scrolled back from the bottom */
    char     input[T_COLS];
    int      in_len;
    RamNode *cwd;
    char     hist[T_HIST][T_COLS];
    int      hist_n, hist_pos;
    WND     *w;
} Term;

/* -----------------------------------------------------------------------
 * Output
 * ----------------------------------------------------------------------- */
static int term_cols(Term *t)
{
    GdiRect c = WmClientRect(t->w);
    int cols = ((c.w - 2 * T_PAD) * 256) / GdiMonoCellW256();
    if (cols < 20) cols = 20;
    if (cols > T_COLS) cols = T_COLS;
    return cols;
}

static int new_line(Term *t, int kind, int split)
{
    if (t->count == T_ROWS) {                 /* drop the oldest line */
        memmove(t->line[0], t->line[1], sizeof(t->line[0]) * (T_ROWS - 1));
        memmove(t->kind, t->kind + 1, T_ROWS - 1);
        memmove(t->split, t->split + 1, T_ROWS - 1);
        t->count--;
    }
    int i = t->count++;
    t->line[i][0] = '\0';
    t->kind[i]  = (UINT8)kind;
    t->split[i] = (UINT8)split;
    return i;
}

/* Print text (may contain '\n'), wrapping at the window width */
static void tprint_ex(Term *t, int kind, int split, const char *s)
{
    int cols = term_cols(t);
    int i = new_line(t, kind, split);
    int n = 0;
    for (; *s; s++) {
        if (*s == '\n' || n >= cols) {
            t->line[i][n] = '\0';
            i = new_line(t, kind, 0);
            n = 0;
            if (*s == '\n') continue;
        }
        if (*s == '\t') { do { t->line[i][n++] = ' '; } while (n % 4 && n < cols); continue; }
        if (*s == '\r') continue;
        t->line[i][n++] = *s;
    }
    t->line[i][n] = '\0';
    t->scroll = 0;
}

static void tprint(Term *t, const char *s)              { tprint_ex(t, K_NORMAL, 0, s); }
static void terr(Term *t, const char *s)                { tprint_ex(t, K_ERROR, 0, s); }

static void tprintf(Term *t, const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    tprint(t, buf);
}

static void prompt_text(Term *t, char *buf, int cap)
{
    char path[RAMFS_PATH_MAX];
    RamfsPath(t->cwd, path, sizeof(path));
    ksnprintf(buf, (size_t)cap, "%s> ", path);
}

/* -----------------------------------------------------------------------
 * Commands
 * ----------------------------------------------------------------------- */
#define MAX_ARGS 8

/* Split into arguments; "double quotes" group words with spaces */
static int split_args(char *s, char **argv)
{
    int argc = 0;
    while (*s && argc < MAX_ARGS) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '"') {
            argv[argc++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ') s++;
        }
        if (*s) *s++ = '\0';
    }
    return argc;
}

static bool is(const char *a, const char *b)
{
    while (*a && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return !*a && !*b;
}

static void pad_to(char *buf, int width)
{
    int n = (int)strlen(buf);
    while (n < width) buf[n++] = ' ';
    buf[n] = '\0';
}

static void cmd_help(Term *t)
{
    tprint(t,
        "Commands:\n"
        "  dir [path]          list a folder (also: ls)\n"
        "  cd [path]           change folder; 'cd ..' goes up\n"
        "  type <file>         show a text file (also: cat)\n"
        "  echo <text> [> f]   print text, or write it to a file\n"
        "  mkdir <name>        create a folder\n"
        "  del <name>          delete a file or empty folder (also: rm)\n"
        "  start <app> [file]  open notepad, explorer, settings, calendar\n"
        "  mem  uptime  date  time  ver  whoami  sysinfo  dmesg\n"
        "  cls                 clear the screen (also: clear, Ctrl+L)\n"
        "  exit                close this window\n"
        "Keys: Up/Down history, PgUp/PgDn scroll.");
}

static void cmd_dir(Term *t, const char *arg)
{
    RamNode *d = arg ? RamfsResolve(t->cwd, arg) : t->cwd;
    if (!d) { terr(t, "The system cannot find the path specified."); return; }
    if (!d->dir) { d = d->parent; }
    char path[RAMFS_PATH_MAX];
    RamfsPath(d, path, sizeof(path));
    tprintf(t, " Directory of %s\n", path);
    int files = 0, dirs = 0;
    UINT64 bytes = 0;
    for (RamNode *c = d->child; c; c = c->next) {
        char col[32];
        if (c->dir) { ksnprintf(col, sizeof(col), "  <DIR>"); dirs++; }
        else {
            ksnprintf(col, sizeof(col), "  %u", (unsigned)c->size);
            files++; bytes += c->size;
        }
        pad_to(col, 16);
        tprintf(t, "%s%s", col, c->name);
    }
    tprintf(t, "  %d file(s) %u bytes, %d folder(s)", files, (unsigned)bytes, dirs);
}

static void cmd_cd(Term *t, const char *arg)
{
    char path[RAMFS_PATH_MAX];
    if (!arg) { RamfsPath(t->cwd, path, sizeof(path)); tprint(t, path); return; }
    RamNode *d = RamfsResolve(t->cwd, arg);
    if (!d || !d->dir) { terr(t, "The system cannot find the path specified."); return; }
    RamfsUnref(t->cwd);
    RamfsRef(d);
    t->cwd = d;
}

static void cmd_type(Term *t, const char *arg)
{
    if (!arg) { terr(t, "The syntax of the command is incorrect."); return; }
    RamNode *f = RamfsResolve(t->cwd, arg);
    if (!f || f->dir) { terr(t, "The system cannot find the file specified."); return; }
    char *buf = kmalloc(f->size + 1);
    if (!buf) { terr(t, "Not enough memory."); return; }
    memcpy(buf, f->data, f->size);
    buf[f->size] = '\0';
    tprint(t, buf);
    kfree(buf);
}

static void cmd_echo(Term *t, int argc, char **argv)
{
    char text[T_COLS];
    int  n = 0, redirect = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], ">")) { redirect = i + 1; break; }
        for (const char *s = argv[i]; *s && n < T_COLS - 2; s++) text[n++] = *s;
        if (i + 1 < argc && strcmp(argv[i + 1], ">") && n < T_COLS - 2) text[n++] = ' ';
    }
    text[n] = '\0';
    if (redirect < 0) { tprint(t, text); return; }
    if (redirect >= argc) { terr(t, "The syntax of the command is incorrect."); return; }
    RamNode *f = RamfsResolve(t->cwd, argv[redirect]);
    if (!f) f = RamfsCreate(t->cwd, argv[redirect], false);
    text[n++] = '\n';
    if (!f || f->dir || !RamfsWrite(f, text, (UINT32)n))
        terr(t, "Could not write the file.");
}

static void cmd_mkdir(Term *t, const char *arg)
{
    if (!arg) { terr(t, "The syntax of the command is incorrect."); return; }
    if (RamfsFind(t->cwd, arg)) { terr(t, "A file or folder with that name already exists."); return; }
    if (!RamfsCreate(t->cwd, arg, true)) terr(t, "The folder could not be created.");
}

static void cmd_del(Term *t, const char *arg)
{
    if (!arg) { terr(t, "The syntax of the command is incorrect."); return; }
    RamNode *n = RamfsResolve(t->cwd, arg);
    if (!n) { terr(t, "Could not find it."); return; }
    if (n == t->cwd || n == RamfsRoot()) { terr(t, "Cannot delete the current folder."); return; }
    if (!RamfsDelete(n))
        terr(t, n->refs ? "It is in use (open in a window or a terminal)."
                        : "The folder is not empty.");
}

static void cmd_mem(Term *t)
{
    uint64_t total, free_p, used;
    pmm_stats(&total, &free_p, &used);
    tprintf(t, "Physical memory: %u MB total, %u MB used, %u MB free",
            (unsigned)(total * 4 / 1024), (unsigned)(used * 4 / 1024),
            (unsigned)(free_p * 4 / 1024));
}

static void cmd_date(Term *t, bool time)
{
    RtcTime r;
    rtc_read(&r);
    if (time) tprintf(t, "The current time is: %02u:%02u:%02u",
                      r.hour, r.minute, r.second);
    else      tprintf(t, "The current date is: %04u-%02u-%02u",
                      r.year, r.month, r.day);
}

static void cmd_sysinfo(Term *t)
{
    char cpu[64], up[32];
    AppCpuName(cpu, sizeof(cpu));
    AppUptime(up, sizeof(up));
    uint64_t total, free_p, used;
    pmm_stats(&total, &free_p, &used);
    int s = GdiScale();
    char line[T_COLS];
    char b[8][96];
    ksnprintf(b[0], 96, "dean@nova-pc");
    ksnprintf(b[1], 96, "------------");
    ksnprintf(b[2], 96, "OS:      NovaOS 0.9 x86_64");
    ksnprintf(b[3], 96, "Kernel:  Nova (NT-compatible), SMP off");
    ksnprintf(b[4], 96, "Uptime:  %s", up);
    ksnprintf(b[5], 96, "Display: %dx%d @ %d%%", GdiScreenW() * s, GdiScreenH() * s, s * 100);
    ksnprintf(b[6], 96, "CPU:     %s", cpu);
    ksnprintf(b[7], 96, "Memory:  %u MB / %u MB", (unsigned)(used * 4 / 1024),
              (unsigned)(total * 4 / 1024));
    /* A four-pane "window" logo beside the details, logo in the accent */
    for (int i = 0; i < 8; i++) {
        bool pane = (i == 1 || i == 2 || i == 4 || i == 5);
        const char *logo = pane ? "  ######  ######    " : "                    ";
        ksnprintf(line, sizeof(line), "%s%s", logo, b[i]);
        tprint_ex(t, K_NORMAL, 20, line);
    }
}

static void cmd_dmesg(Term *t)
{
    char *buf = kmalloc(16384);
    if (!buf) return;
    size_t n = klog_read(buf, 16384);
    /* show the last ~60 lines */
    int lines = 0;
    size_t start = n;
    while (start > 0 && lines < 60) { start--; if (buf[start] == '\n') lines++; }
    if (start > 0) start++;
    tprint_ex(t, K_DIM, 0, buf + start);
    kfree(buf);
}

static void cmd_start(Term *t, int argc, char **argv)
{
    AppId id;
    if (argc < 2 || !AppByName(argv[1], &id)) {
        terr(t, "Usage: start notepad|explorer|settings|calendar|terminal [file]");
        return;
    }
    if (argc >= 3 && (id == APP_NOTEPAD || id == APP_EXPLORER)) {
        RamNode *n = RamfsResolve(t->cwd, argv[2]);
        if (!n && id == APP_NOTEPAD) n = RamfsCreate(t->cwd, argv[2], false);
        if (!n) { terr(t, "The system cannot find the file specified."); return; }
        if (id == APP_NOTEPAD && !n->dir) AppOpenFile(n);
        else AppOpenFolder(n->dir ? n : n->parent);
        return;
    }
    AppLaunch(id);
}

static void run(Term *t, char *cmdline)
{
    char *argv[MAX_ARGS];
    int argc = split_args(cmdline, argv);
    if (!argc) return;
    const char *a1 = argc > 1 ? argv[1] : NULL;
    const char *c  = argv[0];

    if      (is(c, "help") || is(c, "?"))       cmd_help(t);
    else if (is(c, "dir") || is(c, "ls"))       cmd_dir(t, a1);
    else if (is(c, "cd") || is(c, "chdir"))     cmd_cd(t, a1);
    else if (is(c, "cd.."))                     cmd_cd(t, "..");
    else if (is(c, "type") || is(c, "cat"))     cmd_type(t, a1);
    else if (is(c, "echo"))                     cmd_echo(t, argc, argv);
    else if (is(c, "mkdir") || is(c, "md"))     cmd_mkdir(t, a1);
    else if (is(c, "del") || is(c, "rm") || is(c, "rmdir")) cmd_del(t, a1);
    else if (is(c, "mem"))                      cmd_mem(t);
    else if (is(c, "uptime")) { char up[32]; AppUptime(up, sizeof(up)); tprintf(t, "Up %s", up); }
    else if (is(c, "date"))                     cmd_date(t, false);
    else if (is(c, "time"))                     cmd_date(t, true);
    else if (is(c, "ver"))                      tprint(t, "NovaOS [Version 0.9.8] - Phase 8 desktop");
    else if (is(c, "whoami"))                   tprint(t, "nova-pc\\dean");
    else if (is(c, "sysinfo") || is(c, "neofetch")) cmd_sysinfo(t);
    else if (is(c, "dmesg"))                    cmd_dmesg(t);
    else if (is(c, "start") || is(c, "open"))   cmd_start(t, argc, argv);
    else if (is(c, "cls") || is(c, "clear"))    t->count = 0;
    else if (is(c, "exit"))                     WmDestroyWindow(t->w);
    else {
        AppId id;
        if (AppByName(c, &id)) {                     /* "notepad file.txt" */
            char *sv[3] = { "start", argv[0], argc > 1 ? argv[1] : NULL };
            cmd_start(t, argc > 1 ? 3 : 2, sv);
            return;
        }
        char msg[T_COLS + 64];
        ksnprintf(msg, sizeof(msg), "'%s' is not recognized as a command. Type 'help'.", c);
        terr(t, msg);
    }
}

/* -----------------------------------------------------------------------
 * Window callbacks
 * ----------------------------------------------------------------------- */
static void term_paint(WND *w)
{
    Term *t = w->user;
    GdiRect c = WmClientRect(w);
    int cell = GdiMonoCellW256();
    int rows = (c.h - 2 * T_PAD) / T_LINE_H;
    if (rows < 1) rows = 1;
    int total = t->count + 1;                          /* + prompt line */
    int first = total - rows - t->scroll;
    if (first < 0) first = 0;
    int x = c.x + T_PAD, y = c.y + T_PAD;

    for (int i = first; i < total && i < first + rows; i++, y += T_LINE_H) {
        if (i < t->count) {
            const char *l = t->line[i];
            int sp = t->split[i];
            int n = (int)strlen(l);
            GdiColor fg = t->kind[i] == K_ERROR ? T_ERR : t->kind[i] == K_DIM ? T_DIM : T_FG;
            if (sp > n) sp = n;
            if (sp) GdiTextMonoN(x, y, l, sp, T_PROMPT);
            GdiTextMonoN(x + (sp * cell) / 256, y, l + sp, n - sp, fg);
        } else {
            char p[RAMFS_PATH_MAX + 4];
            prompt_text(t, p, sizeof(p));
            int pl = (int)strlen(p);
            GdiTextMono(x, y, p, T_PROMPT);
            GdiTextMonoN(x + (pl * cell) / 256, y, t->input, t->in_len, T_FG);
            int cx = x + ((pl + t->in_len) * cell) / 256;
            if (w->active) GdiAlphaFill(RECT(cx, y + 1, cell / 256, 15), T_FG, 170);
        }
    }
    if (t->scroll)
        GdiTextT(c.x + c.w - 120, c.y + 6, "(scrolled back)", T_DIM);
}

static void remember(Term *t, const char *cmd)
{
    if (!*cmd) return;
    if (t->hist_n == T_HIST) {
        memmove(t->hist[0], t->hist[1], sizeof(t->hist[0]) * (T_HIST - 1));
        t->hist_n--;
    }
    strncpy(t->hist[t->hist_n], cmd, T_COLS - 1);
    t->hist[t->hist_n][T_COLS - 1] = '\0';
    t->hist_n++;
}

static void term_key(WND *w, const KeyEvent *k)
{
    Term *t = w->user;
    if (k->ctrl && k->ch == 'l') { t->count = 0; return; }
    if (k->ctrl && k->ch == 'c') {
        char p[RAMFS_PATH_MAX + T_COLS + 8];
        prompt_text(t, p, RAMFS_PATH_MAX + 4);
        int pl = (int)strlen(p);
        ksnprintf(p + pl, T_COLS + 4, "%s^C", t->input);
        tprint_ex(t, K_NORMAL, pl, p);
        t->in_len = 0; t->input[0] = '\0';
        return;
    }
    if (k->extended) {
        int page = (WmClientRect(w).h - 2 * T_PAD) / T_LINE_H - 1;
        if (k->scancode == KEY_PGUP) { t->scroll += page; if (t->scroll > t->count) t->scroll = t->count; }
        if (k->scancode == KEY_PGDN) { t->scroll -= page; if (t->scroll < 0) t->scroll = 0; }
        if ((k->scancode == KEY_UP || k->scancode == KEY_DOWN) && t->hist_n) {
            t->hist_pos += (k->scancode == KEY_UP) ? -1 : 1;
            if (t->hist_pos < 0) t->hist_pos = 0;
            if (t->hist_pos >= t->hist_n) { t->hist_pos = t->hist_n; t->in_len = 0; t->input[0] = '\0'; return; }
            strncpy(t->input, t->hist[t->hist_pos], T_COLS - 1);
            t->input[T_COLS - 1] = '\0';
            t->in_len = (int)strlen(t->input);
        }
        return;
    }
    if (k->scancode == KEY_ESC) { t->in_len = 0; t->input[0] = '\0'; return; }
    if (k->ch == '\b') { if (t->in_len) t->input[--t->in_len] = '\0'; return; }
    if (k->ch == '\n') {
        char p[RAMFS_PATH_MAX + T_COLS + 8];
        prompt_text(t, p, RAMFS_PATH_MAX + 4);
        int pl = (int)strlen(p);
        ksnprintf(p + pl, T_COLS + 4, "%s", t->input);
        tprint_ex(t, K_NORMAL, pl, p);
        char cmd[T_COLS];
        memcpy(cmd, t->input, (size_t)t->in_len + 1);
        remember(t, cmd);
        t->hist_pos = t->hist_n;
        t->in_len = 0; t->input[0] = '\0';
        run(t, cmd);               /* may destroy the window: nothing after */
        return;
    }
    if (k->ch >= ' ' && k->ch <= '~' && t->in_len < T_COLS - 2) {
        t->input[t->in_len++] = k->ch;
        t->input[t->in_len] = '\0';
        t->scroll = 0;
    }
}

static void term_close(WND *w)
{
    RamfsUnref(((Term *)w->user)->cwd);
    kfree(w->user);
    w->user = NULL;
}

void TerminalOpen(void)
{
    Term *t = kzalloc(sizeof(Term));
    if (!t) return;
    WND *w = AppCreateWindow(APP_TERMINAL, "Terminal", 760, 440, T_BG);
    if (!w) { kfree(t); return; }
    t->w   = w;
    t->cwd = RamfsResolve(NULL, "\\Documents");
    if (!t->cwd) t->cwd = RamfsRoot();
    RamfsRef(t->cwd);
    w->user     = t;
    w->on_paint = term_paint;
    w->on_key   = term_key;
    w->on_close = term_close;
    tprint_ex(t, K_DIM, 0, "NovaOS Terminal [Version 0.9.8]");
    tprint_ex(t, K_DIM, 0, "Type 'help' to see what you can do.");
    tprint(t, "");
}
