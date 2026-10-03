/*
 * um_crash.c — crash reports in C:\NovaOS\Crashes
 *
 * A program that crashes (an exception nothing handled: UmFaultAt) leaves
 * a text file a user can attach to an issue:
 *
 *   C:\NovaOS\Crashes\crash-20261003-194012-14.txt
 *
 * with the exception, where it happened (module+offset), the return
 * addresses on its stack, the modules it had loaded and the end of the
 * kernel's log.  The report is put together on the crashing thread and
 * written by the desktop thread (UmCrashPoll, from UmPoll), which may take
 * the file-system lock; the Terminal names the file under the crash line.
 *
 * A kernel crash cannot write drive C: (it is kept in memory and saved by
 * the drivers the crash may have broken), so UmCrashKernel writes the
 * report straight into sectors set aside for it on the disk that keeps C:
 * (fs/persist.c, \NOVA\PANIC.TXT on FAT) and the next start moves it to
 * C:\NovaOS\Crashes\kernel-YYYYMMDD-HHMMSS.txt (UmCrashKernelFound).
 */

#include "um_internal.h"
#include "../ke/printf.h"
#include "../ke/spinlock.h"
#include "../ke/probe.h"
#include "../ke/version.h"
#include "../hal/rtc.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../fs/ramfs.h"
#include "../fs/persist.h"

#define CRASH_DIR      "\\NovaOS\\Crashes"
#define REPORT_CAP     (16 * 1024)
#define KERNEL_CAP     (32 * 1024)
#define MAX_REPORTS    100              /* more than this many files: no more are added */
#define KERNEL_TITLE   "NovaOS kernel crash report"

typedef struct Pending {
    struct Pending *next;
    char   path[UM_CRASH_PATH];          /* "C:\NovaOS\Crashes\NAME.txt" */
    char  *text;
    UINT32 len;
} Pending;

static Pending  *g_pending;             /* oldest first */
static KSpinLock g_pending_lock = KSPINLOCK_INIT;

/* Append to a report (cut short when full) */
typedef struct { char *p; UINT32 n, cap; } Rep;

static void add(Rep *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(Rep *r, const char *fmt, ...)
{
    if (r->n + 1 >= r->cap) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(r->p + r->n, r->cap - r->n, fmt, ap);
    __builtin_va_end(ap);
    r->n += (UINT32)strlen(r->p + r->n);
}

static void stamp(const RtcTime *t, char *buf, int cap)
{
    ksnprintf(buf, cap, "%04u%02u%02u-%02u%02u%02u", t->year, t->month, t->day, t->hour, t->minute, t->second);
}

static void add_time(Rep *r, const RtcTime *t)
{
    add(r, "Time:       %04u-%02u-%02u %02u:%02u:%02u UTC\n", t->year, t->month, t->day, t->hour, t->minute,
        t->second);
}

/* The end of the kernel's log, whole lines, at most @max bytes */
static void add_log(Rep *r, UINT32 max)
{
    if (r->n + 2 >= r->cap) return;
    if (max > r->cap - r->n - 1) max = r->cap - r->n - 1;
    UINT32 n = (UINT32)klog_read(r->p + r->n, max);
    UINT32 skip = 0;
    if (n + 1 >= max)                                /* (cut: start at a line) */
        while (skip < n && r->p[r->n + skip++] != '\n') { }
    memmove(r->p + r->n, r->p + r->n + skip, n - skip);
    r->n += n - skip;
    r->p[r->n] = '\0';
}

static void queue(Pending *q)
{
    q->next = NULL;
    IrqState s = spin_lock_irqsave(&g_pending_lock);
    Pending **at = &g_pending;
    while (*at) at = &(*at)->next;
    *at = q;
    spin_unlock_irqrestore(&g_pending_lock, s);
}

/* -----------------------------------------------------------------------
 * A program's crash
 * ----------------------------------------------------------------------- */
void um_crash_report(UmProcess *p, const char *what, UINT32 status, UINT64 rip, UINT64 addr, UINT64 sp)
{
    Pending *q = kzalloc(sizeof(*q));
    char *text = q ? kmalloc(REPORT_CAP) : NULL;
    if (!text) { kfree(q); return; }
    RtcTime t;
    rtc_read(&t);
    char when[20], base[32];
    stamp(&t, when, sizeof(when));
    ksnprintf(base, sizeof(base), "%s", p->name);
    size_t bl = strlen(base);
    if (bl > 4 && !strcmp(base + bl - 4, ".exe")) base[bl - 4] = '\0';
    for (char *c = base; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':' || *c == ' ') *c = '_';
    ksnprintf(q->path, sizeof(q->path), "C:" CRASH_DIR "\\%s-%s-%u.txt", base, when, p->pid);

    Rep r = { text, 0, REPORT_CAP };
    text[0] = '\0';
    const UmModule *exe = NULL, *at = um_module_at(p, rip);
    for (int i = 0; i < p->nmodules && !exe; i++)
        if (!p->modules[i].dll) exe = &p->modules[i];
    int threads = 0;
    for (int i = 0; i < UM_MAX_THREADS; i++)
        if (p->threads[i] && !p->threads[i]->exited) threads++;

    add(&r, "NovaOS crash report\n");
    add(&r, "===================\n\n");
    add(&r, "Program:    %s (PID %u, %s, %d thread%s)\n", p->name, p->pid, p->wow ? "32-bit" : "64-bit",
        threads, threads == 1 ? "" : "s");
    if (exe) add(&r, "Path:       %s\n", exe->path);
    add_time(&r, &t);
    add(&r, "NovaOS:     %s\n\n", NOVA_VERSION);
    add(&r, "Exception:  %s (0x%08X)\n", what, status);
    if (at) add(&r, "At:         %s+0x%llx (0x%llx)\n", at->name, (unsigned long long)(rip - at->base),
                (unsigned long long)rip);
    else    add(&r, "At:         0x%llx (in no module: generated code or a bad jump)\n", (unsigned long long)rip);
    if (status == UM_STATUS_ACCESS_VIOLATION)
        add(&r, "Address:    0x%llx (the memory it touched)\n", (unsigned long long)addr);
    if (sp) add(&r, "Stack:      0x%llx\n", (unsigned long long)sp);

    /* return addresses into a module on the stack, as the serial log shows them */
    if (sp) {
        add(&r, "\nReturn addresses on the stack (newest first):\n");
        int shown = 0, bad = 0;
        unsigned step = p->wow ? 4 : 8;
        for (unsigned i = 0; i < 4096 && shown < 32; i++) {
            UINT64 v = 0;
            if (!NT_SUCCESS(CopyFromUser(&v, (const void *)(uintptr_t)(sp + i * step), step))) {
                if (++bad > 1024) break;
                continue;
            }
            const UmModule *m = um_module_at(p, v);
            if (!m || v - m->base < 0x1000) continue;
            add(&r, "  +%04x  %s+0x%llx\n", i * step, m->name, (unsigned long long)(v - m->base));
            shown++;
        }
        if (!shown) add(&r, "  (none found)\n");
    }

    add(&r, "\nModules:\n");
    for (int i = 0; i < p->nmodules; i++) {
        const UmModule *m = &p->modules[i];
        char name[26];                               /* (the names in a column) */
        int k = 0;
        for (; m->name[k] && k < 24; k++) name[k] = m->name[k];
        while (k < 25) name[k++] = ' ';
        name[k] = '\0';
        add(&r, "  %016llx %08llx  %s%s\n", (unsigned long long)m->base, (unsigned long long)m->size, name, m->path);
    }
    add(&r, "\nThe end of the kernel's log:\n\n");
    add_log(&r, 4096);
    if (r.n + 1 >= r.cap) r.n = r.cap - 1;

    q->text = text;
    q->len = r.n;
    ksnprintf(p->crash_report, sizeof(p->crash_report), "%s", q->path);
    queue(q);
}

const char *UmCrashReport(const UmProcess *p)
{
    return p->crash_report;
}

/* -----------------------------------------------------------------------
 * A kernel crash
 * ----------------------------------------------------------------------- */
void UmCrashKernel(void)
{
    static volatile int once;
    static char text[KERNEL_CAP];                    /* (no allocations now) */
    if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL) || !PersistPanicReady()) return;
    RtcTime t;
    rtc_read(&t);
    Rep r = { text, 0, KERNEL_CAP };
    text[0] = '\0';
    add(&r, KERNEL_TITLE "\n");
    add(&r, "==========================\n\n");
    add_time(&r, &t);
    add(&r, "NovaOS:     %s\n\n", NOVA_VERSION);
    add(&r, "The kernel's log up to the crash (the backtrace is at the end):\n\n");
    add_log(&r, KERNEL_CAP - 1024);
    if (PersistPanicWrite(text, r.n))
        kprintf("[CRASH] Saved the kernel crash report on the disk: it goes to C:" CRASH_DIR " at the next start\n");
}

/* \NOVA\PANIC.TXT held a report (at boot, from fs/persist.c) */
void UmCrashKernelFound(const char *text, UINT32 len)
{
    if (len < sizeof(KERNEL_TITLE) || memcmp(text, KERNEL_TITLE, sizeof(KERNEL_TITLE) - 1)) return;
    Pending *q = kzalloc(sizeof(*q));
    char *copy = q ? kmalloc(len) : NULL;
    if (!copy) { kfree(q); return; }
    memcpy(copy, text, len);
    /* named after the crash's time ("Time:       2026-10-03 19:40:12 UTC") */
    char digits[15];
    int k = 0;
    const char *tl = strstr(copy, "Time:");
    for (const char *c = tl; c && *c && *c != '\n' && k < 14; c++)
        if (*c >= '0' && *c <= '9') digits[k++] = *c;
    digits[k] = '\0';
    if (k == 14) {
        char day[9];
        memcpy(day, digits, 8);
        day[8] = '\0';
        ksnprintf(q->path, sizeof(q->path), "C:" CRASH_DIR "\\kernel-%s-%s.txt", day, digits + 8);
    } else
        ksnprintf(q->path, sizeof(q->path), "C:" CRASH_DIR "\\kernel-unknown-time.txt");
    q->text = copy;
    q->len = len;
    kprintf("[CRASH] The kernel crashed during the last start: report saved as %s\n", q->path);
    queue(q);
}

/* -----------------------------------------------------------------------
 * Writing them (the desktop thread)
 * ----------------------------------------------------------------------- */
static RamNode *crash_dir(void)
{
    RamNode *d = RamfsCreate(RamfsRoot(), "NovaOS", true);
    return d ? RamfsCreate(d, "Crashes", true) : NULL;
}

void UmCrashPoll(void)
{
    if (!__atomic_load_n(&g_pending, __ATOMIC_ACQUIRE)) return;
    IrqState s = spin_lock_irqsave(&g_pending_lock);
    Pending *list = g_pending;
    g_pending = NULL;
    spin_unlock_irqrestore(&g_pending_lock, s);
    FsLock();
    RamNode *dir = crash_dir();
    for (Pending *q = list, *next; q; q = next) {
        next = q->next;
        const char *leaf = strrchr(q->path, '\\') + 1;
        RamNode *f = NULL;
        if (dir && RamfsCount(dir) >= MAX_REPORTS)
            kprintf("[CRASH] C:" CRASH_DIR " holds %d reports: %s was not saved\n", MAX_REPORTS, leaf);
        else if (!dir || !(f = RamfsCreate(dir, leaf, false)) || !RamfsWrite(f, q->text, q->len))
            kprintf("[CRASH] Could not write %s\n", q->path);
        else
            kprintf("[CRASH] Wrote %s (%u bytes)\n", q->path, q->len);
        kfree(q->text);
        kfree(q);
    }
    FsUnlock();
}

/* The newest report (the Terminal's "crashes last"), under the file-system lock */
RamNode *UmCrashNewest(void)
{
    RamNode *dir = RamfsResolve(NULL, CRASH_DIR), *best = NULL;
    for (RamNode *n = dir && dir->dir ? dir->child : NULL; n; n = n->next)
        if (!n->dir && (!best || n->mtime > best->mtime ||
                        (n->mtime == best->mtime && strcmp(n->name, best->name) > 0)))
            best = n;
    return best;
}
