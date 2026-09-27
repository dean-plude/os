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
#include "../ke/scheduler.h"
#include "../net/net.h"
#include "../um/um.h"

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

/* A network command in progress (advanced by term_tick) */
typedef enum { JOB_NONE, JOB_PING, JOB_LOOKUP, JOB_FETCH, JOB_PROC } JobKind;
enum { PH_RESOLVE, PH_SEND, PH_WAIT, PH_SLEEP, PH_FETCH };

typedef struct {
    JobKind kind;
    int     phase;
    NetOp  *op;
    char    host[128], path[256];
    UINT16  port;
    UINT32  ip;
    int     count, sent, got, rtt_min, rtt_max, rtt_sum;
    UINT16  seq;
    UINT64  wake;              /* ticks */
    bool    save;              /* wget: save the body; curl: print it */
    bool    https;
    int     redirects;
    char    urls[8][256];      /* curl/wget: several URLs, fetched in turn */
    int     nurls, cur;
    UmProcess *proc;           /* JOB_PROC: a Windows program in this console */
    UmConsole *con;
    bool    open_line;         /* the last line is the program's unfinished line */
    int     col;               /* its output column (after '\r') */
} Job;

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
    Job      job;
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
        "  ipconfig            show the network configuration\n"
        "  ping <host> [-n N]  test a connection (ICMP echo)\n"
        "  nslookup <host>     look up a host name (DNS)\n"
        "  wget <url> [url...] download web pages to C:\\Downloads\n"
        "  curl <url> [url...] fetch web pages and print them\n"
        "  certutil            list trusted root certificates\n"
        "  tasklist            list running programs\n"
        "  taskkill /PID <n>   stop a program\n"
        "  <program> [args]    run a Windows program (C:\\Programs: hello, mandel,\n"
        "                      primes, guess, wc, crttest, filetest, crash, spin)\n"
        "  certutil -addstore root <file>   trust a CA certificate (PEM/DER)\n"
        "  cls                 clear the screen (also: clear, Ctrl+L)\n"
        "  exit                close this window\n"
        "Keys: Up/Down history, PgUp/PgDn scroll, Ctrl+C cancel.");
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

/* -----------------------------------------------------------------------
 * Network commands
 * ----------------------------------------------------------------------- */
static void ip_str(UINT32 ip, char *buf) { NetFormatIp(ip, buf, 16); }

static void cmd_ipconfig(Term *t)
{
    NetStatus st;
    NetGetStatus(&st);
    tprint(t, "Ethernet adapter Ethernet:\n");
    tprintf(t, "   Description . . . . : %s", st.adapter);
    if (!st.present) return;
    tprintf(t, "   Physical Address. . : %02x-%02x-%02x-%02x-%02x-%02x",
            st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    if (!st.link) { tprint(t, "   Media State . . . . : Media disconnected"); return; }
    if (!st.configured) { tprint(t, "   IPv4 Address. . . . : (waiting for DHCP)"); return; }
    char a[16], m[16], g[16], d0[16], d1[16];
    ip_str(st.ip, a); ip_str(st.mask, m); ip_str(st.gw, g);
    ip_str(st.dns[0], d0); ip_str(st.dns[1], d1);
    tprintf(t, "   IPv4 Address. . . . : %s", a);
    tprintf(t, "   Subnet Mask . . . . : %s", m);
    tprintf(t, "   Default Gateway . . : %s", g);
    tprintf(t, "   DNS Servers . . . . : %s", d0);
    if (st.dns[1]) tprintf(t, "                         %s", d1);
}

static void job_end(Term *t)
{
    NetRelease(t->job.op);
    if (t->job.proc) UmRelease(t->job.proc);          /* (kills it if still running) */
    if (t->job.con) UmConsoleRelease(t->job.con);
    memset(&t->job, 0, sizeof(t->job));
}

static bool job_resolve(Term *t, const char *host)
{
    t->job.op = NetResolve(host);
    t->job.phase = PH_RESOLVE;
    if (!t->job.op) { terr(t, "The network is busy; try again."); job_end(t); return false; }
    return true;
}

static void cmd_ping(Term *t, int argc, char **argv)
{
    if (argc < 2) { terr(t, "Usage: ping <host> [-n count]"); return; }
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->kind  = JOB_PING;
    j->count = 4;
    for (int i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-n")) {
            int n = 0;
            for (const char *s = argv[i + 1]; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
            if (n > 0 && n <= 100) j->count = n;
        }
    strncpy(j->host, argv[1], sizeof(j->host) - 1);
    j->rtt_min = 1 << 30;
    job_resolve(t, j->host);
}

static void cmd_nslookup(Term *t, const char *host)
{
    if (!host) { terr(t, "Usage: nslookup <host>"); return; }
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->kind = JOB_LOOKUP;
    strncpy(j->host, host, sizeof(j->host) - 1);
    NetStatus st;
    NetGetStatus(&st);
    char d[16];
    ip_str(st.dns[0], d);
    tprintf(t, "Server:  %s", d);
    job_resolve(t, j->host);
}

static bool set_target(Term *t, const char *url)
{
    Job *j = &t->job;
    if (!NetParseUrl(url, j->host, sizeof(j->host), &j->port, j->path, sizeof(j->path), &j->https)) {
        terr(t, "That doesn't look like a web address (try https://example.com).");
        return false;
    }
    return true;
}

/* certutil [-store]            list trusted roots
 * certutil -addstore root FILE  trust the CA certificate(s) in FILE */
static void cmd_certutil(Term *t, int argc, char **argv)
{
    if (argc >= 2 && is(argv[1], "-addstore")) {
        if (argc < 4 || !is(argv[2], "root")) {
            terr(t, "Usage: certutil -addstore root <file.crt>");
            return;
        }
        RamNode *f = RamfsResolve(t->cwd, argv[3]);
        if (!f || f->dir) { terr(t, "The system cannot find the file specified."); return; }
        char err[80];
        int n = NetImportRoots(f->data, f->size, err, sizeof(err));
        if (!n) { tprintf(t, "CertUtil: -addstore command FAILED: %s", err); return; }
        tprintf(t, "Added %d certificate%s to the Trusted Root Certification Authorities store.",
                n, n == 1 ? "" : "s");
        tprint(t, "CertUtil: -addstore command completed successfully. (Lasts until reboot.)");
        return;
    }
    if (argc >= 2 && !is(argv[1], "-store")) {
        terr(t, "Usage: certutil [-store] | certutil -addstore root <file>");
        return;
    }
    int imported, n = NetRootCount(&imported);
    tprint(t, "Trusted Root Certification Authorities:");
    for (int i = 0; i < n; i++) {
        char name[72];
        bool user;
        if (!NetRootName(i, name, sizeof(name), &user)) break;
        tprintf(t, "  %3d  %s%s", i + 1, name[0] ? name : "(unnamed)", user ? "  [imported]" : "");
    }
    tprintf(t, "%d trusted roots (%d built in from the Mozilla CA list, %d imported).",
            n, n - imported, imported);
}

/* This URL is finished (either way): go on to the next one, if any. */
static void fetch_next(Term *t)
{
    Job *j = &t->job;
    NetRelease(j->op);
    j->op = NULL;
    while (++j->cur < j->nurls) {
        j->redirects = 0;
        tprint(t, "");
        if (set_target(t, j->urls[j->cur])) { job_resolve(t, j->host); return; }
    }
    job_end(t);
}

static void cmd_fetch(Term *t, int argc, char **argv, bool save)
{
    if (argc < 2) { terr(t, save ? "Usage: wget <url> [url...]" : "Usage: curl <url> [url...]"); return; }
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->kind = JOB_FETCH;
    j->save = save;
    for (int i = 1; i < argc && j->nurls < 8; i++) {
        strncpy(j->urls[j->nurls], argv[i], sizeof(j->urls[0]) - 1);
        j->nurls++;
    }
    j->cur = -1;
    fetch_next(t);
}

/* File name for a download: last path segment, else index.html */
static void download_name(const char *path, char *out, int cap)
{
    const char *seg = path, *p;
    for (p = path; *p && *p != '?'; p++) if (*p == '/') seg = p + 1;
    int n = 0;
    for (const char *s = seg; s < p && n < cap - 1; s++)
        out[n++] = (*s == ':' || *s == '\\') ? '_' : *s;
    out[n] = '\0';
    if (!n) ksnprintf(out, (size_t)cap, "index.html");
}

static void print_body(Term *t, const char *body, UINT32 len)
{
    enum { MAX_OUT = 16 * 1024 };
    UINT32 n = len < MAX_OUT ? len : MAX_OUT;
    char *buf = kmalloc(n + 1);
    if (!buf) return;
    for (UINT32 i = 0; i < n; i++) {
        char c = body[i];
        buf[i] = (c == '\n' || c == '\t' || (c >= ' ' && c <= '~')) ? c
               : (c == '\r') ? ' ' : '.';
    }
    buf[n] = '\0';
    tprint(t, buf);
    kfree(buf);
    if (len > n) tprintf(t, "... (%u more bytes; use wget to save it all)", (unsigned)(len - n));
}

static void fetch_done(Term *t)
{
    Job *j = &t->job;
    const char *body;
    UINT32 blen;
    char loc[256];
    int status = NetHttpParse(j->op, &body, &blen, loc, sizeof(loc));
    if (!status) { terr(t, "The server sent a response NovaOS could not read."); fetch_next(t); return; }

    /* status line */
    char line[96];
    int n = 0;
    while (n < (int)sizeof(line) - 1 && n < (int)j->op->len && j->op->data[n] != '\r') {
        line[n] = j->op->data[n];
        n++;
    }
    line[n] = '\0';
    if (j->op->tls_info[0])
        tprintf(t, "Secure connection: %s%s", j->op->tls_info,
                j->op->reused ? "" : j->op->resumed ? " (session resumed)" : "");
    tprintf(t, "HTTP request sent, awaiting response... %s", line + (n > 9 ? 9 : 0));

    if (status >= 300 && status < 400 && loc[0] && j->redirects < 5) {
        tprintf(t, "Location: %s", loc);
        NetRelease(j->op);
        j->op = NULL;
        j->redirects++;
        if (loc[0] == '/') {                       /* same host */
            strncpy(j->path, loc, sizeof(j->path) - 1);
        } else if (!set_target(t, loc)) {
            fetch_next(t);
            return;
        }
        job_resolve(t, j->host);
        return;
    }

    if (j->save) {
        RamNode *dir = RamfsResolve(NULL, "\\Downloads");
        if (!dir) dir = t->cwd;
        char name[RAMFS_NAME_MAX];
        download_name(j->path, name, sizeof(name));
        RamNode *f = RamfsCreate(dir, name, false);
        if (f && RamfsWrite(f, body, blen)) {
            char path[RAMFS_PATH_MAX];
            RamfsPath(f, path, sizeof(path));
            tprintf(t, "Saved %u bytes to %s", (unsigned)blen, path);
        } else {
            terr(t, "Could not save the file.");
        }
    } else {
        print_body(t, body, blen);
    }
    fetch_next(t);
}

static void proc_output(Term *t, const char *s, int n);
static void proc_finish(Term *t);

static bool term_tick(WND *w)
{
    Term *t = w->user;
    Job *j = &t->job;
    if (j->kind == JOB_NONE) return false;
    if (j->kind == JOB_PROC) {
        static char buf[4096];
        bool changed = false;
        for (int rounds = 0; rounds < 16; rounds++) {  /* keep the UI responsive */
            int n = UmConsoleRead(j->con, buf, sizeof(buf));
            if (!n) break;
            proc_output(t, buf, n);
            changed = true;
        }
        if (UmHasExited(j->proc, NULL, NULL, 0)) {
            int n;
            while ((n = UmConsoleRead(j->con, buf, sizeof(buf))) > 0) proc_output(t, buf, n);
            proc_finish(t);
            return true;
        }
        return changed;
    }
    NetOp *op = j->op;
    char a[16];

    if (j->phase == PH_RESOLVE) {
        if (op->state == NET_PENDING) return false;
        if (op->state == NET_FAILED) {
            if (j->kind == JOB_PING)
                tprintf(t, "Ping request could not find host %s. Please check the name and try again.", j->host);
            else
                tprintf(t, "*** Can't find %s: %s", j->host, op->error);
            if (j->kind == JOB_FETCH) fetch_next(t); else job_end(t);
            return true;
        }
        j->ip = op->ip;
        NetRelease(op);
        j->op = NULL;
        ip_str(j->ip, a);
        if (j->kind == JOB_LOOKUP) {
            tprintf(t, "Name:    %s\nAddress: %s", j->host, a);
            job_end(t);
        } else if (j->kind == JOB_PING) {
            tprintf(t, "Pinging %s [%s] with 32 bytes of data:", j->host, a);
            j->phase = PH_SEND;
        } else {
            j->op = NetHttpGet(j->ip, j->port, j->host, j->path, j->https);
            j->phase = PH_FETCH;
            if (!j->op) { terr(t, "The network is busy; try again."); job_end(t); return true; }
            if (j->op->reused)
                tprintf(t, "Reusing the open connection to %s:%u", j->host, j->port);
            else
                tprintf(t, "Connecting to %s (%s):%u...", j->host, a, j->port);
        }
        return true;
    }

    if (j->kind == JOB_PING) {
        if (j->phase == PH_SEND) {
            j->op = NetPing(j->ip, ++j->seq);
            j->sent++;
            j->phase = PH_WAIT;
            if (!j->op) { terr(t, "The network is busy; try again."); job_end(t); return true; }
            return false;
        }
        if (j->phase == PH_WAIT) {
            if (op->state == NET_PENDING) return false;
            ip_str(j->ip, a);
            if (op->state == NET_DONE) {
                j->got++;
                j->rtt_sum += op->rtt_ms;
                if (op->rtt_ms < j->rtt_min) j->rtt_min = op->rtt_ms;
                if (op->rtt_ms > j->rtt_max) j->rtt_max = op->rtt_ms;
                tprintf(t, "Reply from %s: bytes=32 time%s%dms TTL=%d", a,
                        op->rtt_ms < 10 ? "<" : "=", op->rtt_ms < 10 ? 10 : op->rtt_ms, op->ttl);
            } else {
                tprint(t, op->error[0] ? op->error : "Request timed out.");
            }
            NetRelease(op);
            j->op = NULL;
            if (j->sent < j->count) {
                j->wake = sched_ticks() + 100;
                j->phase = PH_SLEEP;
            } else {
                int lost = j->sent - j->got;
                tprintf(t, "\nPing statistics for %s:", a);
                tprintf(t, "    Packets: Sent = %d, Received = %d, Lost = %d (%d%% loss)",
                        j->sent, j->got, lost, lost * 100 / j->sent);
                if (j->got)
                    tprintf(t, "Approximate round trip times in milli-seconds:\n"
                               "    Minimum = %dms, Maximum = %dms, Average = %dms",
                            j->rtt_min, j->rtt_max, j->rtt_sum / j->got);
                job_end(t);
            }
            return true;
        }
        if (j->phase == PH_SLEEP && sched_ticks() >= j->wake) j->phase = PH_SEND;
        return false;
    }

    if (j->kind == JOB_FETCH && j->phase == PH_FETCH) {
        if (op->state == NET_PENDING) return false;
        if (op->state == NET_FAILED) {
            tprintf(t, "Failed: %s", op->error);
            fetch_next(t);
        } else {
            fetch_done(t);
        }
        return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Windows programs
 * ----------------------------------------------------------------------- */

/* Append program output: '\n' ends the line, '\r' returns to column 0,
 * '\b' backs up; the unfinished last line stays open for more. */
/* Write @ch at column @col of line @l (space-padding, keeping it terminated) */
static void line_put(char *l, int col, char ch)
{
    int len = (int)strlen(l);
    while (len < col) l[len++] = ' ';
    l[col] = ch;
    if (col >= len) l[col + 1] = '\0';
}

static void proc_output(Term *t, const char *s, int n)
{
    Job *j = &t->job;
    int cols = term_cols(t);
    for (int k = 0; k < n; k++) {
        char c = s[k];
        if ((c & 0xC0) == 0x80) continue;             /* UTF-8 continuation: one '?' per character */
        if (!j->open_line) {
            new_line(t, K_NORMAL, 0);
            j->col = 0;
            if (c == '\n') continue;
            j->open_line = true;
        }
        if (c == '\n') { j->open_line = false; continue; }
        if (c == '\r') { j->col = 0; continue; }
        if (c == '\b') { if (j->col) j->col--; continue; }
        int count = 1;
        if (c == '\t') { c = ' '; count = 8 - j->col % 8; }
        else if ((unsigned char)c < ' ' || (unsigned char)c >= 0x80) c = '?';
        while (count--) {
            if (j->col >= cols) {                      /* wrap */
                new_line(t, K_NORMAL, 0);
                j->col = 0;
            }
            line_put(t->line[t->count - 1], j->col++, c);
        }
    }
    t->scroll = 0;
}

static bool start_program(Term *t, RamNode *exe, const char *cmdline)
{
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->con = UmConsoleNew();
    if (!j->con) { terr(t, "Not enough memory."); return true; }
    char err[160];
    j->proc = UmSpawn(exe, cmdline, t->cwd, j->con, err, sizeof(err));
    if (!j->proc) {
        terr(t, err);
        job_end(t);
        return true;
    }
    j->kind = JOB_PROC;
    return true;
}

static void proc_finish(Term *t)
{
    Job *j = &t->job;
    UINT32 status;
    char why[96];
    UmHasExited(j->proc, &status, why, sizeof(why));
    j->open_line = false;
    char msg[160];
    if (why[0]) {
        ksnprintf(msg, sizeof(msg), "%s %s", UmName(j->proc), why);
        terr(t, msg);
    } else if (status) {
        ksnprintf(msg, sizeof(msg), "[exit code %d]", (int)status);
        tprint_ex(t, K_DIM, 0, msg);
    }
    job_end(t);
}

static void cmd_tasklist(Term *t)
{
    UmProcInfo list[32];
    int n = UmList(list, 32);
    tprint(t, "Image Name                     PID   Mem Usage");
    tprint(t, "========================= ======== ============");
    tprint(t, "System                           4    (kernel)");
    for (int i = 0; i < n; i++) {
        char name[27];
        int k = 0;
        for (; list[i].name[k] && k < 25; k++) name[k] = list[i].name[k];
        while (k < 26) name[k++] = ' ';
        name[k] = '\0';
        tprintf(t, "%s%8u %9u K%s", name, list[i].pid, list[i].mem_kb,
                list[i].exited ? " (exiting)" : "");
    }
}

static void cmd_taskkill(Term *t, int argc, char **argv)
{
    UINT32 pid = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (is(argv[i], "/pid") || is(argv[i], "-pid"))
            for (const char *d = argv[i + 1]; *d >= '0' && *d <= '9'; d++) pid = pid * 10 + (UINT32)(*d - '0');
    if (!pid) { terr(t, "Usage: taskkill /PID <pid>"); return; }
    if (UmKillPid(pid)) tprintf(t, "SUCCESS: Sent termination signal to the process with PID %u.", pid);
    else tprintf(t, "ERROR: The process \"%u\" not found.", pid);
}

static void run(Term *t, char *cmdline)
{
    char original[T_COLS];
    strncpy(original, cmdline, sizeof(original) - 1);
    original[sizeof(original) - 1] = '\0';
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
    else if (is(c, "ipconfig") || is(c, "ifconfig")) cmd_ipconfig(t);
    else if (is(c, "ping"))                     cmd_ping(t, argc, argv);
    else if (is(c, "nslookup"))                 cmd_nslookup(t, a1);
    else if (is(c, "wget"))                     cmd_fetch(t, argc, argv, true);
    else if (is(c, "certutil"))                 cmd_certutil(t, argc, argv);
    else if (is(c, "curl"))                     cmd_fetch(t, argc, argv, false);
    else if (is(c, "cls") || is(c, "clear"))    t->count = 0;
    else if (is(c, "tasklist"))                 cmd_tasklist(t);
    else if (is(c, "taskkill"))                 cmd_taskkill(t, argc, argv);
    else if (is(c, "exit"))                     WmDestroyWindow(t->w);
    else {
        AppId id;
        RamNode *exe = UmFindProgram(t->cwd, c);
        if (exe && !AppByName(c, &id)) {             /* a Windows program */
            start_program(t, exe, original);
            return;
        }
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
    /* + the prompt line (hidden while a command runs); a running program
     * gets an input line of its own unless its last line is still open */
    bool proc = t->job.kind == JOB_PROC;
    int total = t->count + ((t->job.kind == JOB_NONE || (proc && !t->job.open_line)) ? 1 : 0);
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
            if (proc && t->job.open_line && i == t->count - 1) {   /* typing after a prompt */
                int at = t->job.col;
                GdiTextMonoN(x + (at * cell) / 256, y, t->input, t->in_len, T_FG);
                int cx = x + ((at + t->in_len) * cell) / 256;
                if (w->active) GdiAlphaFill(RECT(cx, y + 1, cell / 256, 15), T_FG, 170);
            }
        } else if (proc) {
            GdiTextMonoN(x, y, t->input, t->in_len, T_FG);
            int cx = x + (t->in_len * cell) / 256;
            if (w->active) GdiAlphaFill(RECT(cx, y + 1, cell / 256, 15), T_FG, 170);
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
    if (t->job.kind == JOB_PROC) {            /* keyboard goes to the program */
        Job *j = &t->job;
        if (k->ctrl && k->ch == 'c') { proc_output(t, "^C\n", 3); UmKill(j->proc, 0xC000013A); return; }
        if (k->ctrl && k->ch == 'z') { proc_output(t, "^Z\n", 3); UmConsoleEof(j->con); return; }
        if (k->extended) {
            int page = (WmClientRect(w).h - 2 * T_PAD) / T_LINE_H - 1;
            if (k->scancode == KEY_PGUP) { t->scroll += page; if (t->scroll > t->count) t->scroll = t->count; }
            if (k->scancode == KEY_PGDN) { t->scroll -= page; if (t->scroll < 0) t->scroll = 0; }
            return;
        }
        if (k->ch == '\b') { if (t->in_len) t->input[--t->in_len] = '\0'; return; }
        if (k->ch == '\n') {
            proc_output(t, t->input, t->in_len);       /* echo */
            proc_output(t, "\n", 1);
            UmConsoleWrite(j->con, t->input, t->in_len);
            UmConsoleWrite(j->con, "\r\n", 2);
            t->in_len = 0; t->input[0] = '\0';
            return;
        }
        if (k->ch >= ' ' && k->ch <= '~' && t->in_len < T_COLS - 2) {
            t->input[t->in_len++] = k->ch;
            t->input[t->in_len] = '\0';
            t->scroll = 0;
        }
        return;
    }
    if (t->job.kind != JOB_NONE) {            /* a command is running */
        bool scroll = k->extended && (k->scancode == KEY_PGUP || k->scancode == KEY_PGDN);
        if (k->ctrl && k->ch == 'c') { tprint(t, "^C"); job_end(t); }
        if (!scroll) return;
    }
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
    job_end((Term *)w->user);
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
    w->on_tick  = term_tick;
    tprint_ex(t, K_DIM, 0, "NovaOS Terminal [Version 0.9.8]");
    tprint_ex(t, K_DIM, 0, "Type 'help' to see what you can do.");
    tprint(t, "");
}
