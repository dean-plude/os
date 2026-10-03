/*
 * terminal.c — Terminal: a small command shell over the RAM disk
 *
 * Output is kept as wrapped lines in a scrollback buffer; the prompt and
 * the line being typed are drawn after it.  Commands run synchronously on
 * the desktop thread.
 */

#include "../ke/prof.h"
#include "../wm/clipboard.h"
#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../ke/version.h"
#include "../ke/kpcr.h"
#include "../ke/printf.h"
#include "../hal/rtc.h"
#include "../ke/timezone.h"
#include "../ke/scheduler.h"
#include "../net/net.h"
#include "../drivers/usb.h"
#include "../drivers/virtio_input.h"
#include "../drivers/hda.h"
#include "../drivers/sof.h"
#include "../drivers/i2chid.h"
#include "../um/um.h"
#include "../fs/persist.h"
#include "../hal/serial.h"
#include "../hal/pci.h"
#include "../fs/setup.h"
#include "../drivers/nvme.h"
#include "vterm.h"

#define T_COLS   160
#define T_ROWS   400
#define T_HIST   16
#define T_INPUT_MAX 8191                 /* characters in a typed command line (cmd.exe's longest) */
#define T_PAD    10
#define T_LINE_H 18

#define T_BG      GDI_C(0x0C, 0x0C, 0x0C)
#define T_FG      GDI_C(0xCC, 0xCC, 0xCC)
#define T_DIM     GDI_C(0x80, 0x80, 0x80)
#define T_ERR     GDI_C(0xF1, 0x4C, 0x4C)
#define T_PROMPT  GDI_C(0x4C, 0xC2, 0xFF)

enum { K_NORMAL, K_ERROR, K_DIM };

/* "serial on": copy everything the Terminal shows to the serial port, and
 * mark the end of each command, for driving it from a test harness */
static bool g_mirror;
static void mirror(const char *s, int n)
{
    if (g_mirror && n > 0) kserial_write(s, (size_t)n);
}

/* A network command in progress (advanced by term_tick) */
typedef enum { JOB_NONE, JOB_PING, JOB_LOOKUP, JOB_FETCH, JOB_PROC, JOB_SETUP } JobKind;
enum { PH_RESOLVE, PH_SEND, PH_WAIT, PH_SLEEP, PH_FETCH };

typedef struct {
    JobKind kind;
    int     phase;
    NetOp  *op;
    char    host[128], path[256];
    UINT16  port;
    NetIp   addr;
    int     family;            /* -4 / -6: only that address family (0: either) */
    int     count, sent, got, rtt_min, rtt_max, rtt_sum;
    UINT16  seq;
    UINT64  wake;              /* ticks */
    bool    save;              /* wget: save the body; curl: print it */
    bool    https;
    int     redirects;
    char    urls[8][256];      /* curl/wget: several URLs, fetched in turn */
    int     nurls, cur;
    UmProcess *proc;           /* JOB_PROC: a Windows program in this console */
    UmSpawnJob *starting;      /* JOB_PROC: being loaded (proc is NULL until then) */
    UmConsole *con;
    bool    adopted;           /* JOB_PROC: CREATE_NEW_CONSOLE's program (held, not ours to release); the window closes when it ends */
    bool    open_line;         /* the last line is the program's unfinished line */
    int     col;               /* its output column (after '\r') */
    int     esc;               /* inside an escape sequence: 1 ESC, 2 CSI, 3 OSC */
    int     esc_arg;           /* the CSI's first number */
    char    csi[24];           /* the CSI's parameter bytes so far */
    int     csi_n;
    /* Screen mode: a program that addresses the screen (cursor moves, the
     * alternate screen) or reads keys one at a time gets a VT emulator
     * (libvterm) the size of the window; lines it scrolls off the top
     * join the scrollback, and its screen does when it ends */
    VTerm       *vt;
    VTermScreen *vs;
    int     vt_rows, vt_cols;
    bool    vt_alt;            /* on the alternate screen */
    bool    vt_cursor;         /* the cursor is shown */
} Job;

typedef struct {
    char     line[T_ROWS][T_COLS + 1];
    UINT8    kind[T_ROWS];
    UINT8    split[T_ROWS];     /* first `split` chars drawn in accent */
    int      count;
    int      scroll;            /* lines scrolled back from the bottom */
    char     input[T_INPUT_MAX + 1];  /* the line being typed: wraps onto as many rows as it needs */
    int      in_len;
    RamNode *cwd;
    char    *hist[T_HIST];            /* (heap) */
    int      hist_n, hist_pos;
    Job      job;
    WND     *w;
    /* a selection (mouse drag): lines of the whole buffer, the prompt line last */
    bool     selecting, has_sel;
    int      sel_l0, sel_c0, sel_l1, sel_c1;
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
    mirror(s, (int)strlen(s));
    mirror("\n", 1);
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
static void prof_line(void *t, const char *s)            { tprint((Term *)t, s); }
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
    FsLock();                                   /* (term_paint draws without it) */
    RamfsPath(t->cwd, path, sizeof(path));
    FsUnlock();
    ksnprintf(buf, (size_t)cap, "%s> ", path);
}

/* A typed line into the scrollback after the prompt (wrapped as it was shown) */
static void echo_line(Term *t, const char *text, const char *tail)
{
    char p[RAMFS_PATH_MAX + 4];
    prompt_text(t, p, sizeof(p));
    UINT32 pl = (UINT32)strlen(p), cap = pl + (UINT32)strlen(text) + (UINT32)strlen(tail) + 1;
    char *b = kmalloc(cap);
    if (!b) return;
    ksnprintf(b, cap, "%s%s%s", p, text, tail);
    tprint_ex(t, K_NORMAL, pl > 255 ? 255 : (int)pl, b);
    kfree(b);
}

/* -----------------------------------------------------------------------
 * Commands
 * ----------------------------------------------------------------------- */
#define MAX_ARGS 32                      /* a Windows program can take a long argument list (VLC takes nine) */

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
        "  copy <src> <dst>    copy a file (also: cp)\n"
        "  start <app> [file]  open notepad, explorer, settings, calendar, browser\n"
        "  store install <name>  get a program from the App Store\n"
        "  store open          open the App Store window\n"
        "  store close         close the App Store window\n"
        "  mem  uptime  date  time  ver  whoami  sysinfo  dmesg\n"
        "  devices             the PCI devices and the driver each one has (also: lspci)\n"
        "  hwcheck             test the laptop drivers on modelled devices (codec, DSP, touchpad)\n"
        "  vol  sync           where drive C: is saved; save it now\n"
        "  install [disk] [/fat]  install NovaOS on a disk (no disk: list them)\n"
        "  ipconfig            show the network configuration\n"
        "  ping [-4|-6] <host> [-n N]  test a connection (ICMP echo)\n"
        "  nslookup <host>     look up a host name (DNS)\n"
        "  wget <url> [url...] download web pages to C:\\Downloads\n"
        "  curl <url> [url...] fetch web pages and print them (-4/-6: IPv4/IPv6 only)\n"
        "  certutil            list trusted root certificates\n"
        "  tasklist            list running programs\n"
        "  crashes [last]      list the crash reports; show the newest one\n"
        "  taskkill /PID <n>   stop a program\n"
        "  <program> [args]    run a Windows program (C:\\Programs: hello, mandel,\n"
        "                      primes, guess, wc, crttest, filetest, crash, spin)\n"
        "  netsurf [url]       the NetSurf web browser (http, https)\n"
        "  certutil -addstore root <file>   trust a CA certificate (PEM/DER)\n"
        "  cls                 clear the screen (also: clear, Ctrl+L)\n"
        "  exit                close this window\n"
        "Keys: Up/Down history, PgUp/PgDn scroll, Ctrl+C cancel.");
}

/* @v as "1,234,567" */
static void thousands(UINT64 v, char *out)
{
    char tmp[32];
    int n = 0, k = 0;
    do { if (n && n % 3 == 0) tmp[k++] = ','; tmp[k++] = (char)('0' + v % 10); v /= 10; n++; } while (v);
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    out[k] = 0;
}

static void cmd_dir(Term *t, const char *arg)
{
    RamNode *d = arg ? RamfsResolve(t->cwd, arg) : t->cwd;
    if (!d) { terr(t, "The system cannot find the path specified."); return; }
    if (!d->dir) { d = d->parent; }
    if (!RamfsLoad(d)) { terr(t, "The disk could not be read."); return; }
    char path[RAMFS_PATH_MAX];
    RamfsPath(d, path, sizeof(path));
    RamNode *root = d;
    while (root->parent) root = root->parent;
    const char *label = "NovaOS";
    if (root != RamfsRoot()) RamfsDriveInfo(root, &label, NULL, NULL);
    char letter = root == RamfsRoot() ? 'C' : RamfsDriveLetter(root);
    if (label && *label) tprintf(t, " Volume in drive %c is %s", letter, label);
    else tprintf(t, " Volume in drive %c has no label.", letter);
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
    UINT64 total, free;                     /* the free space of this directory's own drive */
    AppDriveSpace(d, &total, &free);
    char b[32], f[32];
    thousands(bytes, b);
    thousands(free, f);
    tprintf(t, "%16d File(s) %s%s bytes", files, "              " + (strlen(b) < 14 ? strlen(b) : 14), b);
    tprintf(t, "%16d Dir(s) %s%s bytes free", dirs, "               " + (strlen(f) < 15 ? strlen(f) : 15), f);
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
    RamfsRef(f);                            /* (a file on another drive is read in meanwhile) */
    if (!RamfsLoad(f)) { RamfsUnref(f); terr(t, "The file could not be read."); return; }
    char *buf = kmalloc(f->size + 1);
    if (!buf) { RamfsUnref(f); terr(t, "Not enough memory."); return; }
    memcpy(buf, f->data, f->size);
    buf[f->size] = '\0';
    RamfsUnref(f);
    tprint(t, buf);
    kfree(buf);
}

/* "crashes": the reports in C:\NovaOS\Crashes (um/um_crash.c), newest
 * last; "crashes last": the newest one */
static void cmd_crashes(Term *t, const char *arg)
{
    if (arg && is(arg, "last")) {
        RamNode *f = UmCrashNewest();
        if (!f) { tprint(t, "There are no crash reports."); return; }
        char path[RAMFS_PATH_MAX];
        RamfsPath(f, path, sizeof(path));
        tprintf(t, "%s:", path);
        cmd_type(t, path);
        return;
    }
    RamNode *d = RamfsResolve(NULL, "\\NovaOS\\Crashes");
    int n = 0;
    for (RamNode *f = d && d->dir ? d->child : NULL; f; f = f->next)
        if (!f->dir) n++;
    if (!n) { tprint(t, "There are no crash reports."); return; }
    tprintf(t, "%d crash report%s in C:\\NovaOS\\Crashes:", n, n == 1 ? "" : "s");
    for (RamNode *f = d->child; f; f = f->next)
        if (!f->dir) tprintf(t, "  %s  (%u bytes)", f->name, f->size);
    tprint(t, "'crashes last' shows the newest; attach the file to an issue when reporting a bug.");
}

/* "echo TEXT [> FILE]": @raw is what follows "echo", printed as typed */
static void cmd_echo(Term *t, const char *raw)
{
    if (*raw == ' ') raw++;
    const char *gt = NULL;
    bool q = false;
    for (const char *c = raw; *c && !gt; c++) {
        if (*c == '"') q = !q;
        else if (*c == '>' && !q) gt = c;
    }
    int n = gt ? (int)(gt - raw) : (int)strlen(raw);
    while (gt && n && raw[n - 1] == ' ') n--;
    char *text = kmalloc((UINT32)n + 2);
    if (!text) { terr(t, "Not enough memory."); return; }
    memcpy(text, raw, (size_t)n);
    text[n] = '\0';
    if (!gt) { tprint(t, text); kfree(text); return; }
    char name[RAMFS_PATH_MAX];
    const char *a = gt + 1;
    while (*a == ' ' || *a == '"') a++;
    int k = 0;
    while (*a && *a != '"' && k < (int)sizeof(name) - 1) name[k++] = *a++;
    while (k && name[k - 1] == ' ') k--;
    name[k] = '\0';
    if (!k) { terr(t, "The syntax of the command is incorrect."); kfree(text); return; }
    RamNode *f = RamfsResolve(t->cwd, name);
    if (!f) {                                   /* a new file, maybe in another folder */
        char dir[RAMFS_PATH_MAX];
        strncpy(dir, name, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
        RamNode *d = t->cwd;
        const char *leaf = name;
        if (slash) {
            *slash = '\0';
            d = dir[0] ? RamfsResolve(t->cwd, dir) : RamfsRoot();
            leaf = name + (slash - dir) + 1;
        }
        f = d ? RamfsCreate(d, leaf, false) : NULL;
    }
    text[n++] = '\n';
    if (!f || f->dir || !RamfsWrite(f, text, (UINT32)n))
        terr(t, "Could not write the file.");
    kfree(text);
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

/* copy <source> <destination>: the destination may be a folder */
static void cmd_copy(Term *t, const char *src, const char *dst)
{
    if (!src || !dst) { terr(t, "Usage: copy <source> <destination>"); return; }
    RamNode *f = RamfsResolve(t->cwd, src);
    if (!f) { terr(t, "Could not find the source file."); return; }
    if (f->dir) { terr(t, "Folders cannot be copied."); return; }
    RamNode *dir = RamfsResolve(t->cwd, dst);
    const char *name = f->name;
    if (!dir || !dir->dir) {
        /* "folder\newname": split off the new name */
        char path[RAMFS_PATH_MAX];
        strncpy(path, dst, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        char *sep = NULL;
        for (char *p = path; *p; p++) if (*p == '\\' || *p == '/') sep = p;
        if (sep) { *sep = '\0'; name = sep + 1; dir = RamfsResolve(t->cwd, path[0] ? path : "\\"); }
        else     { name = dst; dir = t->cwd; }
        if (!dir || !dir->dir || !*name) { terr(t, "The destination folder does not exist."); return; }
    }
    RamNode *out = RamfsFind(dir, name);
    if (out == f) { terr(t, "The file cannot be copied onto itself."); return; }
    if (out && out->dir) { terr(t, "A folder with that name is in the way."); return; }
    if (!out) out = RamfsCreate(dir, name, false);
    RamfsRef(f);                            /* (a file on another drive is read in meanwhile) */
    bool ok = out && RamfsLoad(f) && RamfsWrite(out, f->data, f->size);
    RamfsUnref(f);
    if (!ok) { terr(t, "Could not write the copy."); return; }
    tprint(t, "        1 file(s) copied.");
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
    TzLocalNow(&r);                             /* local time, as cmd.exe shows it */
    if (time) tprintf(t, "The current time is: %02u:%02u:%02u",
                      r.hour, r.minute, r.second);
    else      tprintf(t, "The current date is: %04u-%02u-%02u",
                      r.year, r.month, r.day);
}

/* whoami: as Windows prints it, nova-pc\name in lower case (USERNAME: the
 * name given at first boot, apps/welcome.c) */
static void cmd_whoami(Term *t)
{
    char user[40] = "dean", line[64];
    um_registry_get_sz(UM_SETUP_KEY, "UserName", user, sizeof(user));
    ksnprintf(line, sizeof(line), "nova-pc\\%s", user[0] ? user : "dean");
    for (char *p = line; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    tprint(t, line);
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
    ksnprintf(b[2], 96, "OS:      NovaOS " NOVA_VERSION " x86_64");
    ksnprintf(b[3], 96, "Kernel:  Nova (NT-compatible), SMP %s, %u CPU%s",
              g_cpu_count > 1 ? "on" : "off", (unsigned)g_cpu_count, g_cpu_count == 1 ? "" : "s");
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
    if (argc >= 2) {                             /* a .msi package: Windows Installer */
        size_t n = strlen(argv[1]);
        RamNode *msi = n > 4 && !strcmp(argv[1] + n - 4, ".msi") ? RamfsResolve(t->cwd, argv[1]) : NULL;
        if (msi && !msi->dir) {
            if (!AppRunMsi(msi)) terr(t, "Windows Installer (msiexec.exe) is not available.");
            return;
        }
    }
    if (argc == 2 && !AppByName(argv[1], &id)) {  /* a document, folder or shortcut */
        RamNode *f = RamfsResolve(t->cwd, argv[1]);
        size_t n = strlen(argv[1]);
        bool prog = n > 4 && (!strcmp(argv[1] + n - 4, ".exe") || !strcmp(argv[1] + n - 4, ".EXE") ||
                              !strcmp(argv[1] + n - 4, ".com"));
        if (f && !prog) {
            if (f->dir) AppOpenFolder(f);
            else AppOpenFile(f);
            return;
        }
    }
    RamNode *exe = argc >= 2 && !AppByName(argv[1], &id) ? UmFindProgram(t->cwd, argv[1]) : NULL;
    if (exe) {                                   /* a Windows program, detached from the terminal */
        UINT32 cap = 1;
        for (int i = 1; i < argc; i++) cap += (UINT32)strlen(argv[i]) + 3;
        char *line = kmalloc(cap);
        if (!line) { terr(t, "Not enough memory."); return; }
        int n = 0;
        for (int i = 1; i < argc; i++) {
            bool q = strchr(argv[i], ' ') != NULL;
            if (i > 1) line[n++] = ' ';
            if (q) line[n++] = '"';
            for (const char *s = argv[i]; *s; s++) line[n++] = *s;
            if (q) line[n++] = '"';
        }
        line[n] = '\0';
        if (!UmSpawnDetached(exe, line, t->cwd)) terr(t, "Not enough memory.");
        kfree(line);
        return;
    }
    if (argc < 2 || !AppByName(argv[1], &id)) {
        terr(t, "Usage: start notepad|explorer|settings|calendar|browser|terminal|PROGRAM [file|args]");
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

/* devices: every PCI function found at boot and the driver that took it,
 * so a new machine shows at once what NovaOS runs on it and what it lacks
 * (docs/hardware.md) */
static const char *vendor_name(UINT16 v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x10EC: return "Realtek";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x14E4: return "Broadcom";
    case 0x168C: case 0x17CB: return "Qualcomm";
    case 0x144D: return "Samsung";
    case 0x15B7: return "SanDisk/WD";
    case 0x1987: return "Phison";
    case 0x1C5C: return "SK hynix";
    case 0x1E0F: return "KIOXIA";
    case 0x1234: return "QEMU";
    case 0x1B36: return "QEMU";
    case 0x1AF4: return "virtio";
    case 0x1013: return "Cirrus";
    case 0x15AD: return "VMware";
    default:     return "";
    }
}

static void cmd_devices(Term *t)
{
    PciDevice d;
    const char *drv;
    int n = 0, with = 0, bridges = 0, without = 0;
    tprint_ex(t, K_DIM, 0, "Slot     ID         Vendor      Class                Driver");
    for (int i = 0; PciAt(i, &d, &drv); i++) {
        bool bridge = d.class_code == 0x06;
        const char *what = drv ? drv : bridge ? "(bridge)" : "no driver";
        n++;
        if (drv) with++;
        else if (bridge) bridges++;
        else without++;
        char line[T_COLS];
        int k = ksnprintf(line, sizeof(line), "%02x:%02x.%x  %04x:%04x  %s", d.bus, d.dev, d.func,
                          d.vendor, d.device, vendor_name(d.vendor));
        pad_to(line, k - (int)strlen(vendor_name(d.vendor)) + 12);    /* (ksnprintf has no %-12s) */
        k = (int)strlen(line);
        ksnprintf(line + k, sizeof(line) - k, "%s", PciClassName(&d));
        pad_to(line, k + 21);
        k = (int)strlen(line);
        ksnprintf(line + k, sizeof(line) - k, "%s", what);
        tprint_ex(t, drv || bridge ? K_NORMAL : K_ERROR, 0, line);
    }
    tprintf(t, "devices: %d PCI functions, %d with a driver, %d bridges, %d without a driver",
            n, with, bridges, without);
}

/* usbcheck: the USB HID report parser on devices QEMU doesn't have
 * (media keys, five-button mice with a horizontal wheel, pens with tilt),
 * and the virtio-input pen and tablet decoding */
static void usbcheck_say(void *ctx, const char *line) { tprint((Term *)ctx, line); }

static void cmd_usbcheck(Term *t)
{
    int failed = UsbHidSelfCheck(usbcheck_say, t);
    int vfailed = VirtioInputSelfCheck(usbcheck_say, t);
    failed = failed < 0 || vfailed < 0 ? -1 : failed + vfailed;
    tprintf(t, "usbcheck: %s, %d failed", failed ? "done" : "all passed", failed < 0 ? 1 : failed);
}

/* hwcheck: the drivers for the reference laptop's devices QEMU can't
 * show (Phase 21.4), against modelled devices: the HD Audio controller
 * matching and a Realtek ALC257 codec with its headphone jack, the audio
 * DSP's boot (NHLT, SOF firmware, IPC4) on a modelled DSP, and an I2C-HID
 * touchpad; then what the real DSP did at boot */
static void cmd_hwcheck(Term *t)
{
    int failed = HdaSelfCheck(usbcheck_say, t);
    int sfailed = SofSelfCheck(usbcheck_say, t);
    int ifailed = I2cHidSelfCheck(usbcheck_say, t);
    failed = failed < 0 || sfailed < 0 || ifailed < 0 ? -1 : failed + sfailed + ifailed;
    tprintf(t, "     audio DSP on this machine: %s", SofStatus());
    tprintf(t, "hwcheck: %s, %d failed", failed ? "done" : "all passed", failed < 0 ? 1 : failed);
}

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
    for (int i = 0; i < st.ip6_count; i++) {                 /* (as Windows: IPv6 first) */
        NetIp x = { .v6 = true };
        memcpy(x.a, st.ip6[i], 16);
        char a6[48];
        NetFormatAddr(&x, a6, sizeof(a6));
        bool ll = st.ip6[i][0] == 0xFE && (st.ip6[i][1] & 0xC0) == 0x80;
        if (ll) tprintf(t, "   Link-local IPv6 Address : %s%%%u%s", a6, (unsigned)st.if_index,
                        st.ip6_preferred[i] ? "" : " (tentative)");
        else    tprintf(t, "   IPv6 Address. . . . : %s%s", a6, st.ip6_preferred[i] ? "" : " (tentative)");
    }
    if (!st.configured) tprint(t, "   IPv4 Address. . . . : (waiting for DHCP)");
    else {
        char a[16], m[16], g[16];
        ip_str(st.ip, a); ip_str(st.mask, m); ip_str(st.gw, g);
        tprintf(t, "   IPv4 Address. . . . : %s", a);
        tprintf(t, "   Subnet Mask . . . . : %s", m);
        tprintf(t, "   Default Gateway . . : %s", g);
    }
    bool first = true;
    for (int i = 0; i < 3; i++) {
        NetIp *d = &st.dns_all[i];
        static const UINT8 zero[16];
        if (!memcmp(d->a, zero, 16)) continue;
        char ds[48];
        NetFormatAddr(d, ds, sizeof(ds));
        tprintf(t, first ? "   DNS Servers . . . . : %s" : "                         %s", ds);
        first = false;
    }
}

static bool g_marked;          /* this command's end was marked already */
static void done_mark(void)
{
    if (g_mirror && !g_marked) kserial_write("\n[TERM-DONE]\n", 13);
    g_marked = true;
}

static void job_end(Term *t)
{
    if (!t->job.adopted) done_mark();             /* (a new console's window is not the command's) */
    NetRelease(t->job.op);
    if (t->job.proc && t->job.adopted) {
        UmKillConsole(t->job.con, 1);                 /* closing the console ends its programs */
        UmUnhold(t->job.proc);
    } else if (t->job.proc) UmRelease(t->job.proc);   /* (kills it if still running) */
    UmSpawnAbandon(t->job.starting);
    if (t->job.con) UmConsoleRelease(t->job.con);
    if (t->job.vt) vterm_free(t->job.vt);
    memset(&t->job, 0, sizeof(t->job));
}

static bool job_resolve(Term *t, const char *host)
{
    t->job.op = NetResolveEx(host, t->job.family);
    t->job.phase = PH_RESOLVE;
    if (!t->job.op) { terr(t, "The network is busy; try again."); job_end(t); return false; }
    return true;
}

/* install [disk [/fat]]: what the Setup app does, from the keyboard (and
 * the self-tests): NovaOS onto @disk with drive C: on NTFS, or FAT32 */
static void cmd_install(Term *t, int argc, char **argv)
{
    SetupDisk d[8];
    int n = SetupListDisks(d, 8);
    if (argc < 2) {
        for (int i = 0; i < n; i++)
            tprintf(t, "  %s  %u MiB  %s  %s%s%s", d[i].dev->name, (unsigned)(d[i].bytes >> 20), d[i].dev->model,
                    d[i].contents, d[i].boot ? "  (NovaOS is running from it)" : "",
                    d[i].too_small ? "  (too small)" : "");
        if (!n) tprint(t, "There are no disks to install on.");
        if (NvmeBehindVmd())
            tprint(t, "Intel VMD (RST) hides the NVMe disks: turn it off in the firmware setup.");
        tprint(t, "Usage: install <disk> [/fat]   (drive C: on NTFS, or on FAT32 with /fat)");
        return;
    }
    SetupDisk *pick = NULL;
    for (int i = 0; i < n && !pick; i++) if (is(argv[1], d[i].dev->name)) pick = &d[i];
    if (!pick) { tprintf(t, "There is no disk named %s. Type 'install' to list them.", argv[1]); return; }
    if (pick->boot) { terr(t, "NovaOS is running from that disk."); return; }
    if (pick->too_small) { terr(t, "That disk is too small for NovaOS."); return; }
    bool fat = argc > 2 && (is(argv[2], "/fat") || is(argv[2], "fat"));
    if (!SetupStart(pick->dev, !fat)) {
        SetupStatus st;
        SetupGetStatus(&st);
        terr(t, st.error[0] ? st.error : "The installer is already running.");
        return;
    }
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->kind = JOB_SETUP;
    tprintf(t, "Installing NovaOS on %s (everything on it is erased)...", pick->dev->name);
}

static void cmd_ping(Term *t, int argc, char **argv)
{
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->kind  = JOB_PING;
    j->count = 4;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            int n = 0;
            for (const char *s = argv[++i]; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
            if (n > 0 && n <= 100) j->count = n;
        } else if (!strcmp(argv[i], "-4")) j->family = 4;
        else if (!strcmp(argv[i], "-6")) j->family = 6;
        else if (!host) host = argv[i];
    }
    if (!host) { terr(t, "Usage: ping [-4 | -6] [-n count] <host>"); return; }
    strncpy(j->host, host, sizeof(j->host) - 1);
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
    char d[48];
    NetFormatAddr(&st.dns_all[0], d, sizeof(d));
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
        if (!f || f->dir || RamfsReadOnly(f)) { terr(t, "The system cannot find the file specified."); return; }
        char err[80];
        int n = NetImportRoots(f->data, f->size, err, sizeof(err));
        if (!n) { tprintf(t, "CertUtil: -addstore command FAILED: %s", err); return; }
        tprintf(t, "Added %d certificate%s to the Trusted Root Certification Authorities store.",
                n, n == 1 ? "" : "s");
        /* Programs with their own TLS (NetSurf) read the store's files */
        RamNode *sys = RamfsResolve(NULL, "\\Windows\\System32");
        RamNode *store = sys ? RamfsCreate(sys, "CertStore", true) : NULL;
        if (store) {
            char name[24];
            ksnprintf(name, sizeof(name), "root%d.crt", RamfsCount(store) + 1);
            RamNode *copy = RamfsCreate(store, name, false);
            if (copy) RamfsWrite(copy, f->data, f->size);
        }
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
    Job *j = &t->job;
    memset(j, 0, sizeof(*j));
    j->kind = JOB_FETCH;
    j->save = save;
    for (int i = 1; i < argc && j->nurls < 8; i++) {
        if (!strcmp(argv[i], "-4")) { j->family = 4; continue; }
        if (!strcmp(argv[i], "-6")) { j->family = 6; continue; }
        strncpy(j->urls[j->nurls], argv[i], sizeof(j->urls[0]) - 1);
        j->nurls++;
    }
    if (!j->nurls) {
        terr(t, save ? "Usage: wget [-4 | -6] <url> [url...]" : "Usage: curl [-4 | -6] <url> [url...]");
        memset(j, 0, sizeof(*j));
        return;
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
static void screen_resize(Term *t);
static bool screen_enter(Term *t);

static bool term_tick_files(WND *w);

/* A running program's output: no files (the file-system lock stays free
 * for the program); anything else takes it */
static bool term_tick(WND *w)
{
    Term *t = w->user;
    Job *j = &t->job;
    if (j->kind == JOB_NONE) return false;
    if (j->kind == JOB_PROC && !j->starting && j->proc && !UmHasExited(j->proc, NULL, NULL, 0)) {
        static char buf[4096];
        bool changed = false;
        screen_resize(t);
        if (!j->vt && !(UmConsoleInputMode(j->con) & CON_ENABLE_LINE_INPUT) && screen_enter(t)) changed = true;
        for (int rounds = 0; rounds < 16; rounds++) {
            int n = UmConsoleRead(j->con, buf, sizeof(buf));
            if (!n) break;
            proc_output(t, buf, n);
            changed = true;
        }
        return changed;
    }
    FsLock();
    bool r = term_tick_files(w);
    FsUnlock();
    return r;
}

static bool term_tick_files(WND *w)
{
    Term *t = w->user;
    Job *j = &t->job;
    if (j->kind == JOB_NONE) return false;
    if (j->kind == JOB_PROC) {
        static char buf[4096];
        bool changed = false;
        if (j->starting) {                             /* still loading (on a worker thread) */
            char err[160];
            UmProcess *p;
            if (!UmSpawnPoll(j->starting, &p, err, sizeof(err))) return false;
            j->starting = NULL;
            if (!p) { terr(t, err); job_end(t); return true; }
            j->proc = p;
        }
        if (!j->proc) return false;                    /* CREATE_NEW_CONSOLE: not handed over yet */
        screen_resize(t);
        /* a program reading keys one at a time gets the screen */
        if (!j->vt && !(UmConsoleInputMode(j->con) & CON_ENABLE_LINE_INPUT) && screen_enter(t)) changed = true;
        for (int rounds = 0; rounds < 16; rounds++) {  /* keep the UI responsive */
            int n = UmConsoleRead(j->con, buf, sizeof(buf));
            if (!n) break;
            proc_output(t, buf, n);
            changed = true;
        }
        if (UmHasExited(j->proc, NULL, NULL, 0)) {
            int n;
            while ((n = UmConsoleRead(j->con, buf, sizeof(buf))) > 0) proc_output(t, buf, n);
            if (j->adopted) { WmDestroyWindow(t->w); return true; }   /* as a Windows console window does */
            proc_finish(t);
            return true;
        }
        return changed;
    }
    if (j->kind == JOB_SETUP) {
        SetupStatus st;
        SetupGetStatus(&st);
        if (st.state == SETUP_RUNNING) {
            if (!st.step[0] || !strcmp(st.step, j->path)) return false;
            strncpy(j->path, st.step, sizeof(j->path) - 1);
            tprintf(t, "%d%%  %s", st.percent, st.step);
            return true;
        }
        if (st.state == SETUP_DONE)
            tprint(t, "NovaOS is installed. Restart without the USB stick or disc to start it from the disk.");
        else
            terr(t, st.error[0] ? st.error : "The installation failed.");
        job_end(t);
        return true;
    }
    NetOp *op = j->op;
    char a[48];

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
        j->addr = op->addr;
        NetRelease(op);
        j->op = NULL;
        NetFormatAddr(&j->addr, a, sizeof(a));
        if (j->kind == JOB_LOOKUP) {
            tprintf(t, "Name:    %s\nAddress: %s", j->host, a);
            job_end(t);
        } else if (j->kind == JOB_PING) {
            tprintf(t, "Pinging %s [%s] with 32 bytes of data:", j->host, a);
            j->phase = PH_SEND;
        } else {
            j->op = NetHttpGetAddr(&j->addr, j->port, j->host, j->path, j->https);
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
            j->op = NetPingAddr(&j->addr, ++j->seq);
            j->sent++;
            j->phase = PH_WAIT;
            if (!j->op) { terr(t, "The network is busy; try again."); job_end(t); return true; }
            return false;
        }
        if (j->phase == PH_WAIT) {
            if (op->state == NET_PENDING) return false;
            NetFormatAddr(&j->addr, a, sizeof(a));
            if (op->state == NET_DONE) {
                j->got++;
                j->rtt_sum += op->rtt_ms;
                if (op->rtt_ms < j->rtt_min) j->rtt_min = op->rtt_ms;
                if (op->rtt_ms > j->rtt_max) j->rtt_max = op->rtt_ms;
                if (j->addr.v6)                      /* (Windows shows no hop limit for IPv6) */
                    tprintf(t, "Reply from %s: time%s%dms", a,
                            op->rtt_ms < 10 ? "<" : "=", op->rtt_ms < 10 ? 10 : op->rtt_ms);
                else
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

/* -----------------------------------------------------------------------
 * Screen mode (libvterm)
 * ----------------------------------------------------------------------- */
static int term_rows(Term *t)
{
    GdiRect c = WmClientRect(t->w);
    int r = (c.h - 2 * T_PAD) / T_LINE_H;
    return r < 2 ? 2 : r;
}

/* A cell's character as the ASCII the monospace font has: box drawing as
 * lines and corners, anything else outside ASCII as '?' */
static char cell_char(UINT32 c)
{
    if (!c) return ' ';
    if (c >= ' ' && c < 0x7F) return (char)c;
    if (c == 0xA0) return ' ';
    if (c >= 0x2500 && c <= 0x257F) {
        if (c == 0x2500 || c == 0x2501 || c == 0x2504 || c == 0x2505 || c == 0x2508 || c == 0x2509 ||
            c == 0x254C || c == 0x254D || c == 0x2550) return '-';
        if (c == 0x2502 || c == 0x2503 || c == 0x2506 || c == 0x2507 || c == 0x250A || c == 0x250B ||
            c == 0x254E || c == 0x254F || c == 0x2551) return '|';
        return '+';
    }
    if (c == 0x2026) return '.';                       /* ellipsis */
    if (c == 0x2018 || c == 0x2019) return '\'';
    if (c == 0x201C || c == 0x201D) return '"';
    if (c == 0x2022 || c == 0x00B7) return '*';
    if (c >= 0x2580 && c <= 0x259F) return '#';         /* blocks */
    return '?';
}

/* The text of screen row @row (trailing blanks dropped) */
static int vt_row_text(Term *t, int row, char *out, int cap)
{
    Job *j = &t->job;
    int n = 0, last = 0;
    VTermPos pos = { .row = row };
    for (pos.col = 0; pos.col < j->vt_cols && n < cap - 1; pos.col++) {
        VTermScreenCell cell;
        if (!vterm_screen_get_cell(j->vs, pos, &cell)) break;
        if (cell.width == 0) continue;                 /* the second half of a wide one */
        out[n++] = cell_char(cell.chars[0]);
        if (out[n - 1] != ' ') last = n;
    }
    out[last] = '\0';
    return last;
}

/* libvterm: a line scrolled off the top of the primary screen */
static int vt_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    Term *t = user;
    int i = new_line(t, K_NORMAL, 0), n = 0, last = 0;
    for (int c = 0; c < cols && n < T_COLS; c++) {
        if (cells[c].width == 0) continue;
        t->line[i][n++] = cell_char(cells[c].chars[0]);
        if (t->line[i][n - 1] != ' ') last = n;
    }
    t->line[i][last] = '\0';
    return 1;
}

static int vt_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    Job *j = &((Term *)user)->job;
    if (prop == VTERM_PROP_ALTSCREEN) j->vt_alt = val->boolean;
    if (prop == VTERM_PROP_CURSORVISIBLE) j->vt_cursor = val->boolean;
    return 1;
}

/* libvterm's replies (device attributes, cursor position reports) are
 * typed input for the program */
static void vt_output(const char *s, size_t len, void *user)
{
    Job *j = &((Term *)user)->job;
    for (size_t i = 0; i < len && j->con; i++) UmConsoleKey(j->con, 0, 0, (UINT8)s[i], 0, true);
}

static const VTermScreenCallbacks g_vt_cbs = {
    .settermprop = vt_settermprop,
    .sb_pushline = vt_pushline,
};

/* Start screen mode: the lines on show move into the emulator's screen
 * (the last one, still open, with the cursor after it) */
static bool screen_enter(Term *t)
{
    Job *j = &t->job;
    if (j->vt) return true;
    int rows = term_rows(t), cols = term_cols(t);
    VTerm *vt = vterm_new(rows, cols);
    if (!vt) return false;
    vterm_set_utf8(vt, 1);
    j->vt = vt;
    j->vs = vterm_obtain_screen(vt);
    j->vt_rows = rows;
    j->vt_cols = cols;
    j->vt_cursor = true;
    vterm_screen_enable_altscreen(j->vs, 1);
    vterm_screen_set_callbacks(j->vs, &g_vt_cbs, t);
    vterm_output_set_callback(vt, vt_output, t);
    vterm_screen_reset(j->vs, 1);
    int keep = t->count < rows - 1 ? t->count : rows - 1;
    int from = t->count - keep;
    char buf[T_COLS + 3];
    for (int i = from; i < t->count; i++) {
        bool last = i == t->count - 1 && j->open_line;
        int n = ksnprintf(buf, sizeof(buf), last ? "%s" : "%s\r\n", t->line[i]);
        vterm_input_write(vt, buf, (size_t)n);
        if (last) {                                    /* the cursor where the program left it */
            n = ksnprintf(buf, sizeof(buf), "\r\x1b[%dG", (j->col < cols ? j->col : cols - 1) + 1);
            vterm_input_write(vt, buf, (size_t)n);
        }
    }
    t->count = from;                                   /* those lines are on the screen now */
    j->open_line = false;
    t->has_sel = false;
    t->scroll = 0;
    return true;
}

/* End screen mode: the primary screen's lines, down to the last one used,
 * join the scrollback */
static void screen_leave(Term *t)
{
    Job *j = &t->job;
    if (!j->vt) return;
    if (!j->vt_alt) {
        VTermPos cur;
        vterm_state_get_cursorpos(vterm_obtain_state(j->vt), &cur);
        char text[T_COLS + 1];
        int last = cur.col ? cur.row : cur.row - 1;
        for (int r = 0; r < j->vt_rows; r++)
            if (vt_row_text(t, r, text, sizeof(text))) { if (r > last) last = r; }
        for (int r = 0; r <= last; r++) {
            vt_row_text(t, r, text, sizeof(text));
            int i = new_line(t, K_NORMAL, 0);
            strncpy(t->line[i], text, T_COLS);
            t->line[i][T_COLS] = '\0';
        }
    }
    vterm_free(j->vt);
    j->vt = NULL;
    j->vs = NULL;
    j->open_line = false;
}

/* The window changed size: so does the screen */
static void screen_resize(Term *t)
{
    Job *j = &t->job;
    int rows = term_rows(t), cols = term_cols(t);
    if (j->con) UmConsoleSetSize(j->con, cols, rows);
    if (!j->vt || (rows == j->vt_rows && cols == j->vt_cols)) return;
    vterm_set_size(j->vt, rows, cols);
    j->vt_rows = rows;
    j->vt_cols = cols;
}

/* CSI sequences that address the screen start screen mode: cursor
 * positioning, clearing it, scroll regions, the alternate screen */
static bool screen_csi(const char *p, int n, char final)
{
    if (final == 'H' || final == 'f' || final == 'r' || final == 'd') return true;
    if (final == 'J') return n == 1 && (p[0] == '2' || p[0] == '3');
    if ((final == 'h' || final == 'l') && n >= 2 && p[0] == '?') {
        int v = 0;
        for (int i = 1; i < n && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + (p[i] - '0');
        return v == 1049 || v == 1047 || v == 47;
    }
    return false;
}

/* Program output to the screen grid.  As in a Windows console, a line feed
 * also returns the carriage unless the program set DISABLE_NEWLINE_AUTO_RETURN. */
static void vt_write(Job *j, const char *s, size_t n)
{
    if (j->con && (UmConsoleOutputMode(j->con) & 0x0008)) { vterm_input_write(j->vt, s, n); return; }
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n') continue;
        if (i > from) vterm_input_write(j->vt, s + from, i - from);
        vterm_input_write(j->vt, "\r\n", 2);
        from = i + 1;
    }
    if (n > from) vterm_input_write(j->vt, s + from, n - from);
}

static void proc_output(Term *t, const char *s, int n)
{
    mirror(s, n);
    Job *j = &t->job;
    if (j->vt) {
        vt_write(j, s, (size_t)n);
        return;
    }
    int cols = term_cols(t);
    for (int k = 0; k < n; k++) {
        char c = s[k];
        /* ANSI/VT escape sequences (colors, cursor moves) are consumed; the
         * lines have one color each, so colors are dropped */
        if (j->esc == 1) {
            j->esc = c == '[' ? 2 : c == ']' ? 3 : 0;
            j->esc_arg = 0;
            j->csi_n = 0;
            continue;
        }
        if (j->esc == 2) {
            if ((unsigned char)c < 0x40 && j->csi_n < (int)sizeof(j->csi)) j->csi[j->csi_n++] = c;
            if (c >= '0' && c <= '9') j->esc_arg = j->esc_arg * 10 + (c - '0');
            else if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
                j->esc = 0;
                if (screen_csi(j->csi, j->csi_n, c) && screen_enter(t)) {
                    char seq[sizeof(j->csi) + 3];
                    seq[0] = 0x1B; seq[1] = '[';
                    memcpy(seq + 2, j->csi, (size_t)j->csi_n);
                    seq[2 + j->csi_n] = c;
                    vterm_input_write(j->vt, seq, (size_t)j->csi_n + 3);
                    if (k + 1 < n) vt_write(j, s + k + 1, (size_t)(n - k - 1));
                    return;
                }
                if (c == 'G' && j->open_line) j->col = j->esc_arg > 0 ? j->esc_arg - 1 : 0;   /* column */
                if (c == 'K' && j->open_line && j->esc_arg == 0) {                              /* erase to end */
                    char *ln = t->line[t->count - 1];
                    if ((int)strlen(ln) > j->col) ln[j->col] = '\0';
                }
            }
            continue;
        }
        if (j->esc == 3) {                             /* OSC: to BEL or ESC \ */
            if (c == '\a') j->esc = 0;
            else if (c == 0x1B) j->esc = 1;
            continue;
        }
        if (c == 0x1B) { j->esc = 1; continue; }
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
    UmConsoleSetSize(j->con, term_cols(t), term_rows(t));
    /* mapped on a worker thread, so a large program does not hold up the
     * desktop; term_tick picks the process up */
    j->starting = UmSpawnStart(exe, cmdline, t->cwd, j->con);
    if (!j->starting) {
        terr(t, "Not enough memory.");
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
    screen_leave(t);
    j->open_line = false;
    char msg[160];
    if (why[0]) {
        ksnprintf(msg, sizeof(msg), "%s %s", UmName(j->proc), why);
        terr(t, msg);
        if (UmCrashReport(j->proc)[0]) {
            ksnprintf(msg, sizeof(msg), "Crash report: %s", UmCrashReport(j->proc));
            tprint_ex(t, K_DIM, 0, msg);
        }
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

static void run_cmd(Term *t, char *cmdline);

static void run(Term *t, char *cmdline)
{
    char *s = cmdline;
    while (*s == ' ') s++;
    g_marked = false;
    if (!strncmp(s, "serial ", 7)) {                 /* serial on|off (see mirror) */
        g_mirror = !strcmp(s + 7, "on");
        tprint(t, g_mirror ? "Terminal output is copied to the serial port." : "Serial copy off.");
        done_mark();
        return;
    }
    if (!strncmp(s, "profile on", 10)) {                      /* the sampling profiler (ke/prof.c) */
        const char *a = s + 10;                                 /* "profile on [DELAY LENGTH]" (ticks) */
        UINT64 v[2] = { 0, 0 };
        for (int i = 0; i < 2; i++) {
            while (*a == ' ') a++;
            while (*a >= '0' && *a <= '9') v[i] = v[i] * 10 + (UINT64)(*a++ - '0');
        }
        ProfStart(v[0], v[1]);
        tprint(t, "Profiling: \"profile\" shows where the time went.");
        done_mark();
        return;
    }
    if (is(s, "profile")) {
        ProfReport(prof_line, t);
        done_mark();
        return;
    }
    if (!strncmp(s, "trace ", 6) || is(s, "trace")) {        /* trace NAME|off (see UmSetTrace) */
        const char *n = s[5] ? s + 6 : "off";
        UmSetTrace(is(n, "off") ? NULL : n);
        tprintf(t, is(n, "off") ? "System call tracing off." : "Failing system calls of %s go to the serial port.", n);
        done_mark();
        return;
    }
    if (is(s, "exit")) { run_cmd(t, cmdline); return; }   /* destroys the window */
    run_cmd(t, cmdline);
    if (t->job.kind == JOB_NONE) done_mark();
}

/* Pipes, redirections and command chains (| < > & && ||) outside quotes:
 * the line is for the command interpreter */
static bool needs_shell(const char *s)
{
    bool q = false;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (!q && (*s == '|' || *s == '<' || *s == '>' || *s == '&')) return true;
    }
    return false;
}

/* A batch file named by the first word (with or without .bat/.cmd) */
static bool is_batch_name(Term *t, const char *c)
{
    size_t n = strlen(c);
    if (n > 4 && (is(c + n - 4, ".bat") || is(c + n - 4, ".cmd"))) {
        RamNode *f = RamfsResolve(t->cwd, c);
        return f && !f->dir;
    }
    char buf[RAMFS_PATH_MAX];
    for (int i = 0; i < 2; i++) {
        ksnprintf(buf, sizeof(buf), "%s%s", c, i ? ".cmd" : ".bat");
        RamNode *f = RamfsResolve(t->cwd, buf);
        if (f && !f->dir) return true;
    }
    return false;
}

/* Run @line with "cmd.exe /d /c" in this Terminal */
static bool run_in_cmd(Term *t, const char *line)
{
    RamNode *cmd = RamfsResolve(NULL, "\\Windows\\System32\\cmd.exe");
    if (!cmd) return false;
    UINT32 cap = (UINT32)strlen(line) + 16;
    char *full = kmalloc(cap);
    if (!full) { terr(t, "Not enough memory."); return true; }
    ksnprintf(full, cap, "cmd.exe /d /c %s", line);
    start_program(t, cmd, full);
    kfree(full);
    return true;
}

static void run_cmd_line(Term *t, char *cmdline, const char *original);

static void run_cmd(Term *t, char *cmdline)
{
    char *original = kmalloc((UINT32)strlen(cmdline) + 1);    /* (the line as typed, for programs) */
    if (!original) { terr(t, "Not enough memory."); return; }
    strcpy(original, cmdline);
    run_cmd_line(t, cmdline, original);
    kfree(original);
}

static void run_cmd_line(Term *t, char *cmdline, const char *original)
{
    {
        const char *s = original;
        while (*s == ' ') s++;
        char first[RAMFS_PATH_MAX];
        int k = 0;
        bool q = false;
        for (const char *c = s; *c && (q || *c != ' ') && k < (int)sizeof(first) - 1; c++) {
            if (*c == '"') { q = !q; continue; }
            first[k++] = *c;
        }
        first[k] = '\0';
        if ((needs_shell(s) || (k && is_batch_name(t, first))) && run_in_cmd(t, s)) return;
    }
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
    else if (is(c, "echo")) {                   /* (the text as typed, however many words) */
        const char *r = original;
        while (*r == ' ') r++;
        cmd_echo(t, r + 4);
    }
    else if (is(c, "mkdir") || is(c, "md"))     cmd_mkdir(t, a1);
    else if (is(c, "del") || is(c, "rm") || is(c, "rmdir")) cmd_del(t, a1);
    else if (is(c, "copy") || is(c, "cp"))      cmd_copy(t, a1, argc > 2 ? argv[2] : NULL);
    else if (is(c, "mem"))                      cmd_mem(t);
    else if (is(c, "uptime")) { char up[32]; AppUptime(up, sizeof(up)); tprintf(t, "Up %s", up); }
    else if (is(c, "date"))                     cmd_date(t, false);
    else if (is(c, "time"))                     cmd_date(t, true);
    else if (is(c, "vol")) {
        char d[96];
        PersistDescribe(d, sizeof(d));
        tprint(t, " Volume in drive C is kept in memory and saved to:");
        tprint(t, d);
    } else if (is(c, "sync")) {
        UmSaveAll();
        tprint(t, PersistActive() ? "Drive C: and the registry are saved." : "There is no disk to save to.");
    }
    else if (is(c, "ver"))                      tprint(t, "NovaOS [Version " NOVA_VERSION "]");
    else if (is(c, "whoami"))                   cmd_whoami(t);
    else if (is(c, "sysinfo") || is(c, "neofetch")) cmd_sysinfo(t);
    else if (is(c, "dmesg"))                    cmd_dmesg(t);
    else if (is(c, "start") || is(c, "open"))   cmd_start(t, argc, argv);
    else if (is(c, "store")) {
        if (argc == 2 && is(argv[1], "close")) tprint(t, StoreClose());
        else if (argc == 2 && is(argv[1], "open")) { StoreOpen(); tprint(t, "Opened the App Store."); }
        else if (argc < 3 || !is(argv[1], "install")) terr(t, "Usage: store install <program name> | store open | store close");
        else {
            char name[64];
            int n = 0;
            name[0] = '\0';
            for (int i = 2; i < argc && n < (int)sizeof(name) - 1; i++)   /* "store install Mesa 3D" */
                n += ksnprintf(name + n, sizeof(name) - n, i > 2 ? " %s" : "%s", argv[i]);
            tprint(t, StoreInstall(name));
        }
    }
    else if (is(c, "ipconfig") || is(c, "ifconfig")) cmd_ipconfig(t);
    else if (is(c, "ping"))                     cmd_ping(t, argc, argv);
    else if (is(c, "nslookup"))                 cmd_nslookup(t, a1);
    else if (is(c, "wget"))                     cmd_fetch(t, argc, argv, true);
    else if (is(c, "certutil"))                 cmd_certutil(t, argc, argv);
    else if (is(c, "curl"))                     cmd_fetch(t, argc, argv, false);
    else if (is(c, "cls") || is(c, "clear"))    t->count = 0;
    else if (is(c, "tasklist"))                 cmd_tasklist(t);
    else if (is(c, "crashes"))                  cmd_crashes(t, a1);
    else if (is(c, "usbcheck"))                 cmd_usbcheck(t);
    else if (is(c, "hwcheck"))                  cmd_hwcheck(t);
    else if (is(c, "devices") || is(c, "lspci")) cmd_devices(t);
    else if (is(c, "install"))                  cmd_install(t, argc, argv);
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
/* The line being typed, wrapped at the window width onto rows of its own
 * after the scrollback: what comes before the typed text on its first row
 * (the prompt, or a program's unfinished line), the row it starts on and
 * how many rows it takes (the cursor's included) */
typedef struct {
    char pre[RAMFS_PATH_MAX + T_COLS + 8];
    int  pre_len, split;        /* the first `split` characters in accent */
    GdiColor fg;                /* the rest of pre's */
    int  base, rows, cols;
} EditArea;

static bool edit_area(Term *t, EditArea *e)
{
    Job *j = &t->job;
    bool proc = j->kind == JOB_PROC;
    if (j->kind != JOB_NONE && !proc) return false;     /* a command is running: no line */
    e->cols = term_cols(t);
    e->fg = T_FG;
    e->base = t->count;
    if (!proc) {
        prompt_text(t, e->pre, sizeof(e->pre));
        e->pre_len = e->split = (int)strlen(e->pre);
    } else if (j->open_line && t->count) {              /* typing after a program's prompt */
        int i = --e->base;
        int n = (int)strlen(t->line[i]);
        memcpy(e->pre, t->line[i], (size_t)n);
        while (n < j->col && n < (int)sizeof(e->pre) - 1) e->pre[n++] = ' ';
        e->pre_len = n;
        e->split = t->split[i] < n ? t->split[i] : n;
        e->fg = t->kind[i] == K_ERROR ? T_ERR : t->kind[i] == K_DIM ? T_DIM : T_FG;
    } else e->pre_len = e->split = 0;
    e->pre[e->pre_len] = '\0';
    e->rows = (e->pre_len + t->in_len) / e->cols + 1;
    return true;
}

/* Character @k of the edit area's text (pre, then what is typed) */
static char edit_char(Term *t, const EditArea *e, int k)
{
    return k < e->pre_len ? e->pre[k] : t->input[k - e->pre_len];
}

/* The lines on screen: the first one's index and how many fit */
static int term_total(Term *t)
{
    EditArea e;
    return edit_area(t, &e) ? e.base + e.rows : t->count;
}

static int term_first(Term *t, int *rows)
{
    GdiRect c = WmClientRect(t->w);
    int r = (c.h - 2 * T_PAD) / T_LINE_H;
    if (r < 1) r = 1;
    int first = term_total(t) - r - t->scroll;
    if (rows) *rows = r;
    return first < 0 ? 0 : first;
}

/* The text of buffer line @i as shown (a row of the line being typed) */
static void line_text(Term *t, int i, char *out, int cap)
{
    out[0] = '\0';
    EditArea e;
    if (!edit_area(t, &e) || i < e.base) {
        if (i < t->count) ksnprintf(out, (UINT32)cap, "%s", t->line[i]);
        return;
    }
    int a = (i - e.base) * e.cols, b = a + e.cols, end = e.pre_len + t->in_len, n = 0;
    for (int k = a; k < b && k < end && n < cap - 1; k++) out[n++] = edit_char(t, &e, k);
    out[n] = '\0';
}

/* The selection in order: (l0, c0) up to (l1, c1), the end excluded */
static bool sel_ordered(Term *t, int *l0, int *c0, int *l1, int *c1)
{
    if (!t->has_sel) return false;
    bool fwd = t->sel_l0 < t->sel_l1 || (t->sel_l0 == t->sel_l1 && t->sel_c0 <= t->sel_c1);
    *l0 = fwd ? t->sel_l0 : t->sel_l1; *c0 = fwd ? t->sel_c0 : t->sel_c1;
    *l1 = fwd ? t->sel_l1 : t->sel_l0; *c1 = fwd ? t->sel_c1 : t->sel_c0;
    return *l0 != *l1 || *c0 != *c1;
}

/* Put the selected text on the clipboard (lines end with CR LF) */
static bool copy_selection(Term *t)
{
    int l0, c0, l1, c1;
    if (!sel_ordered(t, &l0, &c0, &l1, &c1)) return false;
    int cap = (l1 - l0 + 1) * (T_COLS + RAMFS_PATH_MAX + 4) + 1;
    char *buf = kmalloc((UINT32)cap);
    if (!buf) return false;
    int o = 0;
    char line[T_COLS + RAMFS_PATH_MAX + 8];
    for (int i = l0; i <= l1; i++) {
        line_text(t, i, line, sizeof(line));
        int n = (int)strlen(line);
        int a = i == l0 ? c0 : 0, b = i == l1 ? c1 : n;
        if (a > n) a = n;
        if (b > n) b = n;
        if (i == l1 && b == n && c1 > n && l1 > l0) {}       /* (to the end of the line) */
        if (b > a) { memcpy(buf + o, line + a, (size_t)(b - a)); o += b - a; }
        if (i < l1) { buf[o++] = '\r'; buf[o++] = '\n'; }
    }
    ClipSetText(buf, (UINT32)o);
    kfree(buf);
    return true;
}

static void term_key(WND *w, const KeyEvent *k);

/* Paste: the text goes through the keyboard path (a line end is Enter) */
static void paste(Term *t)
{
    UINT32 len;
    char *s = ClipGetText(&len);
    if (!s) return;
    WND *w = t->w;
    int id = w->id;
    for (UINT32 i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c != '\n' && (c < ' ' || c > '~')) continue;
        KeyEvent k;
        memset(&k, 0, sizeof(k));
        k.pressed = true;
        k.ch = c;
        term_key(w, &k);
        if (WmWindowById(id) != w) break;          /* "exit" closed it */
    }
    kfree(s);
}

static void term_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Term *t = w->user;
    if (msg == WM_MOUSE_RUP) {                     /* right click: paste (or copy a selection) */
        if (t->has_sel && copy_selection(t)) t->has_sel = false;
        else paste(t);
        return;
    }
    if (msg == WM_MOUSE_WHEEL) {
        t->scroll += WmWheelDelta() * 3;
        if (t->scroll > t->count) t->scroll = t->count;
        if (t->scroll < 0) t->scroll = 0;
        return;
    }
    int cell = GdiMonoCellW256();
    int line = term_first(t, NULL) + (y - T_PAD) / T_LINE_H;
    int col = ((x - T_PAD) * 256 + cell / 2) / cell;
    if (line < 0) line = 0;
    if (line >= term_total(t)) line = term_total(t) - 1;
    if (col < 0) col = 0;
    if (msg == WM_MOUSE_DOWN) {
        t->selecting = true;
        t->has_sel = true;
        t->sel_l0 = t->sel_l1 = line;
        t->sel_c0 = t->sel_c1 = col;
    } else if (msg == WM_MOUSE_MOVE && t->selecting) {
        t->sel_l1 = line;
        t->sel_c1 = col;
    } else if (msg == WM_MOUSE_UP) {
        t->selecting = false;
        int a, b, c, d;
        if (!sel_ordered(t, &a, &b, &c, &d)) t->has_sel = false;
    } else if (msg == WM_MOUSE_DBLCLK) {           /* a word */
        char text[T_COLS + RAMFS_PATH_MAX + 8];
        line_text(t, line, text, sizeof(text));
        int n = (int)strlen(text), a = col < n ? col : n, b = a;
        while (a > 0 && text[a - 1] != ' ') a--;
        while (b < n && text[b] != ' ') b++;
        t->has_sel = b > a;
        t->sel_l0 = t->sel_l1 = line;
        t->sel_c0 = a;
        t->sel_c1 = b;
    }
}

/* libvterm's colours as the desktop's (the defaults are the Terminal's) */
static GdiColor vt_color(VTermScreen *vs, VTermColor col, bool fg)
{
    if (fg && VTERM_COLOR_IS_DEFAULT_FG(&col)) return T_FG;
    if (!fg && VTERM_COLOR_IS_DEFAULT_BG(&col)) return T_BG;
    vterm_screen_convert_color_to_rgb(vs, &col);
    return GDI_C(col.rgb.red, col.rgb.green, col.rgb.blue);
}

/* Screen mode: the emulator's cells, runs of one colour at a time */
static void paint_screen(Term *t, GdiRect c)
{
    Job *j = &t->job;
    int cell = GdiMonoCellW256();
    int x0 = c.x + T_PAD, y = c.y + T_PAD;
    VTermPos cur;
    vterm_state_get_cursorpos(vterm_obtain_state(j->vt), &cur);
    for (int r = 0; r < j->vt_rows; r++, y += T_LINE_H) {
        char run[T_COLS + 1];
        int n = 0, run_col = 0;
        GdiColor run_fg = T_FG;
        VTermPos pos = { .row = r };
        for (pos.col = 0; pos.col <= j->vt_cols; pos.col++) {
            VTermScreenCell cl;
            bool end = pos.col == j->vt_cols || !vterm_screen_get_cell(j->vs, pos, &cl);
            GdiColor fg = T_FG, bg = T_BG;
            if (!end) {
                fg = vt_color(j->vs, cl.fg, true);
                bg = vt_color(j->vs, cl.bg, false);
                if (cl.attrs.reverse) { GdiColor x = fg; fg = bg; bg = x; }
                if (bg != T_BG) {
                    int w = cl.width > 1 ? cl.width : 1;
                    int px = x0 + (pos.col * cell) / 256;
                    GdiFillRect(RECT(px, y, x0 + ((pos.col + w) * cell) / 256 - px, T_LINE_H), bg);
                }
            }
            if (n && (end || fg != run_fg)) {
                GdiTextMonoN(x0 + (run_col * cell) / 256, y, run, n, run_fg);
                n = 0;
            }
            if (end) break;
            if (cl.width == 0) continue;
            if (!n) { run_col = pos.col; run_fg = fg; }
            run[n++] = cell_char(cl.chars[0]);
            if (cl.width > 1) run[n++] = ' ';
        }
    }
    if (t->in_len && (UmConsoleInputMode(j->con) & CON_ENABLE_LINE_INPUT)) {   /* a line being typed, wrapped */
        for (int k = 0; k < t->in_len && cur.row < j->vt_rows;) {
            if (cur.col >= j->vt_cols) { cur.col = 0; cur.row++; continue; }
            int n = j->vt_cols - cur.col;
            if (n > t->in_len - k) n = t->in_len - k;
            GdiTextMonoN(x0 + (cur.col * cell) / 256, c.y + T_PAD + cur.row * T_LINE_H, t->input + k, n, T_FG);
            cur.col += n;
            k += n;
        }
        if (cur.col >= j->vt_cols && cur.row + 1 < j->vt_rows) { cur.col = 0; cur.row++; }
    }
    if (j->vt_cursor && t->w->active && cur.row < j->vt_rows && cur.col < j->vt_cols)
        GdiAlphaFill(RECT(x0 + (cur.col * cell) / 256, c.y + T_PAD + cur.row * T_LINE_H + 1, cell / 256, 15), T_FG, 170);
}

/* Row @r of the edit area at (x, y): pre's part in its colours, then the
 * typed text, and the cursor */
static void paint_edit_row(Term *t, const EditArea *e, int r, int x, int y)
{
    int cell = GdiMonoCellW256();
    int a = r * e->cols, b = a + e->cols, end = e->pre_len + t->in_len;
    if (b > end) b = end;
    for (int k = a; k < b;) {                       /* runs of one colour */
        int stop = k < e->split ? e->split : k < e->pre_len ? e->pre_len : b;
        if (stop > b) stop = b;
        GdiColor fg = k < e->split ? T_PROMPT : k < e->pre_len ? e->fg : T_FG;
        const char *src = k < e->pre_len ? e->pre + k : t->input + (k - e->pre_len);
        GdiTextMonoN(x + ((k - a) * cell) / 256, y, src, stop - k, fg);
        k = stop;
    }
    if (end / e->cols == r && t->w->active)
        GdiAlphaFill(RECT(x + ((end % e->cols) * cell) / 256, y + 1, cell / 256, 15), T_FG, 170);
}

static void term_paint(WND *w)
{
    Term *t = w->user;
    GdiRect c = WmClientRect(w);
    if (t->job.kind == JOB_PROC && t->job.vt) { paint_screen(t, c); return; }
    int cell = GdiMonoCellW256();
    int rows = (c.h - 2 * T_PAD) / T_LINE_H;
    if (rows < 1) rows = 1;
    /* + the line being typed (none while a command runs): the prompt's,
     * or a running program's, on its own rows or after its open line */
    EditArea e;
    bool edit = edit_area(t, &e);
    int total = edit ? e.base + e.rows : t->count;
    int first = total - rows - t->scroll;
    if (first < 0) first = 0;
    int x = c.x + T_PAD, y = c.y + T_PAD;

    int s0, sc0, s1, sc1;
    bool sel = sel_ordered(t, &s0, &sc0, &s1, &sc1);
    for (int i = first; i < total && i < first + rows; i++, y += T_LINE_H) {
        if (sel && i >= s0 && i <= s1) {           /* the selected cells */
            int a = i == s0 ? sc0 : 0, b = i == s1 ? sc1 : T_COLS;
            char text[T_COLS + RAMFS_PATH_MAX + 8];
            line_text(t, i, text, sizeof(text));
            int n = (int)strlen(text);
            if (i != s1 || b > n) b = n + (i != s1 ? 1 : 0);
            if (b > a) GdiAlphaFill(RECT(x + (a * cell) / 256, y, ((b - a) * cell) / 256, T_LINE_H), T_PROMPT, 90);
        }
        if (edit && i >= e.base) {
            paint_edit_row(t, &e, i - e.base, x, y);
            continue;
        }
        const char *l = t->line[i];
        int sp = t->split[i];
        int n = (int)strlen(l);
        GdiColor fg = t->kind[i] == K_ERROR ? T_ERR : t->kind[i] == K_DIM ? T_DIM : T_FG;
        if (sp > n) sp = n;
        if (sp) GdiTextMonoN(x, y, l, sp, T_PROMPT);
        GdiTextMonoN(x + (sp * cell) / 256, y, l + sp, n - sp, fg);
    }
    if (t->scroll)
        GdiTextT(c.x + c.w - 120, c.y + 6, "(scrolled back)", T_DIM);
}

static void remember(Term *t, const char *cmd)
{
    if (!*cmd) return;
    if (t->hist_n == T_HIST) {
        kfree(t->hist[0]);
        memmove(t->hist, t->hist + 1, sizeof(t->hist[0]) * (T_HIST - 1));
        t->hist_n--;
    }
    char *h = kmalloc((UINT32)strlen(cmd) + 1);
    if (!h) return;
    strcpy(h, cmd);
    t->hist[t->hist_n++] = h;
}

/* Raw input: a key press as an input record (the character it types, as
 * user32 would make it), or with ENABLE_VIRTUAL_TERMINAL_INPUT as the
 * characters of its xterm sequence */
static void send_key(Term *t, const KeyEvent *k)
{
    UmConsole *con = t->job.con;
    UINT32 vk = UmScancodeToVk(k->scancode, k->extended) & 0xFF;
    if (vk == 0x10 || vk == 0x11 || vk == 0x12 || vk == 0x14) return;   /* modifiers alone */
    if (!vk && !k->ch) return;                          /* (pasted text has characters only) */
    UINT32 ch = (UINT8)k->ch;
    if (ch == '\n') ch = '\r';
    if (vk == 0x1B) ch = 0x1B;
    if (vk == 0x08) ch = 0x08;
    if (k->ctrl && !k->alt) {                           /* Ctrl+letter: control characters */
        if (vk >= 'A' && vk <= 'Z') ch = vk - 'A' + 1;
        else if (vk == 0xDB) ch = 0x1B; else if (vk == 0xDD) ch = 0x1D; else if (vk == 0xDC) ch = 0x1C;
        else if (vk == 0x20 || vk == '2') ch = 0;
        else if (ch != '\r' && ch != 8 && ch != 9 && ch != 0x1B) ch = 0;
    }
    UINT32 ctrl = (k->shift ? CON_SHIFT : 0) | (k->ctrl ? CON_LEFT_CTRL : 0) | (k->alt ? CON_LEFT_ALT : 0) |
                  (k->extended ? CON_ENHANCED_KEY : 0);
    if (!(UmConsoleInputMode(con) & CON_ENABLE_VT_INPUT)) {
        UmConsoleKey(con, (UINT16)vk, k->scancode, (UINT16)ch, ctrl, true);
        return;
    }
    const char *seq = NULL;
    char buf[16];
    int mod = 1 + (k->shift ? 1 : 0) + (k->alt ? 2 : 0) + (k->ctrl ? 4 : 0);
    static const char arrows[] = { 0x26, 'A', 0x28, 'B', 0x27, 'C', 0x25, 'D', 0x24, 'H', 0x23, 'F', 0 };
    for (int i = 0; arrows[i]; i += 2)
        if ((UINT8)arrows[i] == vk) {
            if (mod > 1) ksnprintf(buf, sizeof(buf), "\x1b[1;%d%c", mod, arrows[i + 1]);
            else ksnprintf(buf, sizeof(buf), "\x1b[%c", arrows[i + 1]);
            seq = buf;
        }
    static const UINT8 tilde[] = { 0x2D, 2, 0x2E, 3, 0x21, 5, 0x22, 6, 0x74, 15, 0x75, 17, 0x76, 18,
                                   0x77, 19, 0x78, 20, 0x79, 21, 0x7A, 23, 0x7B, 24, 0 };
    for (int i = 0; tilde[i]; i += 2)
        if (tilde[i] == vk) {
            if (mod > 1) ksnprintf(buf, sizeof(buf), "\x1b[%d;%d~", tilde[i + 1], mod);
            else ksnprintf(buf, sizeof(buf), "\x1b[%d~", tilde[i + 1]);
            seq = buf;
        }
    if (vk >= 0x70 && vk <= 0x73) {                     /* F1-F4 */
        if (mod > 1) ksnprintf(buf, sizeof(buf), "\x1b[1;%d%c", mod, 'P' + (int)(vk - 0x70));
        else ksnprintf(buf, sizeof(buf), "\x1bO%c", 'P' + (int)(vk - 0x70));
        seq = buf;
    }
    if (vk == 0x09 && k->shift) seq = "\x1b[Z";
    if (seq) {
        for (; *seq; seq++) UmConsoleKey(con, 0, 0, (UINT8)*seq, 0, true);
        return;
    }
    if (!ch) return;
    if (k->alt && !k->ctrl) UmConsoleKey(con, 0, 0, 0x1B, 0, true);   /* Alt+x: ESC x */
    UmConsoleKey(con, (UINT16)vk, k->scancode, (UINT16)ch, ctrl, true);
}

static void term_key(WND *w, const KeyEvent *k)
{
    Term *t = w->user;
    /* Ctrl+Shift+C copies; Ctrl+C copies too while something is selected
     * (else it interrupts); Ctrl+V / Ctrl+Shift+V / Shift+Insert paste */
    if (k->ctrl && (k->ch == 'c' || k->ch == 'C') && (k->shift || t->has_sel)) {
        if (copy_selection(t)) t->has_sel = false;
        return;
    }
    if ((k->ctrl && (k->ch == 'v' || k->ch == 'V')) ||
        (k->shift && k->extended && k->scancode == KEY_INSERT)) { t->has_sel = false; paste(t); return; }
    if (k->ch || (k->extended && k->scancode != KEY_PGUP && k->scancode != KEY_PGDN)) t->has_sel = false;
    if (t->job.kind == JOB_PROC) {            /* keyboard goes to the program */
        Job *j = &t->job;
        UINT32 mode = j->con ? UmConsoleInputMode(j->con) : CON_IN_DEFAULT;
        bool ctrl_c = k->ctrl && (k->ch == 'c' || k->ch == 'C');
        if (j->proc && !(mode & CON_ENABLE_LINE_INPUT) && !(ctrl_c && (mode & CON_ENABLE_PROCESSED_INPUT))) {
            send_key(t, k);                   /* raw: every key as it is pressed */
            return;
        }
        if (k->ctrl && k->ch == 'c') {
            proc_output(t, "^C\n", 3);
            if (!j->proc) { job_end(t); return; }      /* still starting: give up on it */
            UmKill(j->proc, 0xC000013A);
            UmKillConsole(j->con, 0xC000013A);     /* and what it started (cmd.exe's programs) */
            return;
        }
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
        if (k->ch >= ' ' && k->ch <= '~' && t->in_len < T_INPUT_MAX) {
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
        echo_line(t, t->input, "^C");
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
            strncpy(t->input, t->hist[t->hist_pos], T_INPUT_MAX);
            t->input[T_INPUT_MAX] = '\0';
            t->in_len = (int)strlen(t->input);
        }
        return;
    }
    if (k->scancode == KEY_ESC) { t->in_len = 0; t->input[0] = '\0'; return; }
    if (k->ch == '\b') { if (t->in_len) t->input[--t->in_len] = '\0'; return; }
    if (k->ch == '\n') {
        echo_line(t, t->input, "");
        char *cmd = kmalloc((UINT32)t->in_len + 1);
        if (!cmd) { terr(t, "Not enough memory."); return; }
        memcpy(cmd, t->input, (size_t)t->in_len + 1);
        remember(t, cmd);
        t->hist_pos = t->hist_n;
        t->in_len = 0; t->input[0] = '\0';
        run(t, cmd);               /* may destroy the window: nothing after but freeing the copy */
        kfree(cmd);
        return;
    }
    if (k->ch >= ' ' && k->ch <= '~' && t->in_len < T_INPUT_MAX) {
        t->input[t->in_len++] = k->ch;
        t->input[t->in_len] = '\0';
        t->scroll = 0;
    }
}

static void term_close(WND *w)
{
    for (int i = 0; i < ((Term *)w->user)->hist_n; i++) kfree(((Term *)w->user)->hist[i]);
    job_end((Term *)w->user);
    RamfsUnref(((Term *)w->user)->cwd);
    kfree(w->user);
    w->user = NULL;
}

static Term *term_new_ex(RamNode *cwd, bool banner)
{
    Term *t = kzalloc(sizeof(Term));
    if (!t) return NULL;
    WND *w = AppCreateWindow(APP_TERMINAL, "Terminal", 760, 440, T_BG);
    if (!w) { kfree(t); return NULL; }
    t->w   = w;
    t->cwd = cwd && cwd->dir ? cwd : RamfsResolve(NULL, "\\Documents");
    if (!t->cwd) t->cwd = RamfsRoot();
    RamfsRef(t->cwd);
    w->user     = t;
    w->on_paint = term_paint;
    w->paint_fs_free = true;                    /* (the prompt's path: prompt_text) */
    w->on_key   = term_key;
    w->on_mouse = term_mouse;
    w->rbutton  = true;                        /* right click pastes, the wheel scrolls */
    w->on_close = term_close;
    w->on_tick  = term_tick;
    w->tick_lock_free = true;
    if (!banner) return t;
    tprint_ex(t, K_DIM, 0, "NovaOS Terminal [Version " NOVA_VERSION "]");
    tprint_ex(t, K_DIM, 0, "Type 'help' to see what you can do.");
    tprint(t, "");
    return t;
}

static Term *term_new(RamNode *cwd) { return term_new_ex(cwd, true); }

/* CreateProcess(CREATE_NEW_CONSOLE): a window of its own, its console the
 * program's.  TerminalConsoleNew makes both (the console sized to the
 * window; *con is referenced by the window) and returns the window's id
 * (0: out of memory); TerminalConsoleAdopt hands it the started program,
 * or closes it if @p is NULL (false: no window any more).  Desktop lock
 * held. */
int TerminalConsoleNew(const char *title, RamNode *cwd, UmConsole **con)
{
    Term *t = term_new_ex(cwd, false);
    if (!t) return 0;
    Job *j = &t->job;
    j->con = UmConsoleNew();
    if (!j->con) { WmDestroyWindow(t->w); return 0; }
    UmConsoleSetSize(j->con, term_cols(t), term_rows(t));
    j->kind = JOB_PROC;
    j->adopted = true;
    if (title && *title) WmSetTitle(t->w, title);
    *con = j->con;
    return t->w->id;
}

bool TerminalConsoleAdopt(int id, UmProcess *p)
{
    WND *w = WmWindowById(id);
    if (!w || w->on_tick != term_tick) return false;        /* closed meanwhile */
    Term *t = w->user;
    if (!p) { WmDestroyWindow(w); return false; }
    UmHold(p);
    t->job.proc = p;
    return true;
}

/* The console program a Terminal runs: the foreground process while the
 * Terminal is active (UmUpdateForeground), as Windows makes a console's
 * programs foreground while their console window is */
struct UmProcess *TerminalProgram(WND *w)
{
    if (!w || w->on_tick != term_tick || !w->user) return NULL;
    Job *j = &((Term *)w->user)->job;
    return j->kind == JOB_PROC ? j->proc : NULL;
}

void TerminalOpen(void)
{
    term_new(NULL);
}

void TerminalRun(const char *cmd, RamNode *cwd)
{
    Term *t = term_new(cwd);
    if (!t || !cmd || !*cmd) return;
    UINT32 n = (UINT32)strlen(cmd);
    if (n > T_INPUT_MAX) n = T_INPUT_MAX;
    char *line = kmalloc(n + 1);
    if (!line) return;
    memcpy(line, cmd, n);
    line[n] = '\0';
    echo_line(t, line, "");                     /* after the prompt, as if typed */
    remember(t, line);
    t->hist_pos = t->hist_n;
    run(t, line);
    kfree(line);
}
