/*
 * prof.c — a sampling profiler on the timer tick
 *
 * Each CPU's timer tick (100 Hz) notes where it interrupted: in a program,
 * halted waiting for the big kernel lock, idle, or the kernel function and
 * its caller (frame pointers: see ksym.c).  "profile on" in the terminal
 * starts it, "profile" prints the heaviest functions and callers.
 */
#include "prof.h"
#include "ksym.h"
#include "printf.h"
#include "../lib/string.h"

#define PROF_MAX 32768
enum { P_USER = 1, P_LOCKWAIT, P_IDLE };

static volatile bool g_on;
static volatile UINT32 g_n;
static UINT64 g_rip[PROF_MAX], g_caller[PROF_MAX];

void ProfStart(void)
{
    g_on = false;
    g_n = 0;
    g_on = true;
}

void ProfSample(UINT64 rip, UINT64 rbp, bool user, bool lock_wait, bool idle)
{
    if (!g_on) return;
    UINT32 i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED);
    if (i >= PROF_MAX) return;
    UINT64 caller = 0;
    if (user) rip = P_USER;
    else if (lock_wait) rip = P_LOCKWAIT;
    else if (idle) rip = P_IDLE;
    else if (rbp >= UINT64_C(0xFFFF800000000000) && !(rbp & 7))
        caller = ((const UINT64 *)(uintptr_t)rbp)[1];       /* the return address of the frame */
    g_caller[i] = caller;
    g_rip[i] = rip;
}

typedef struct { const char *a, *b; UINT32 n; } Row;

static int add_row(Row *rows, int n, int cap, const char *a, const char *b)
{
    for (int i = 0; i < n; i++)
        if (rows[i].a == a && rows[i].b == b) { rows[i].n++; return n; }
    if (n == cap) return n;
    rows[n].a = a; rows[n].b = b; rows[n].n = 1;
    return n + 1;
}

static void top(Row *rows, int n, int want, UINT32 total, bool pairs,
                void (*out)(void *, const char *), void *ctx)
{
    char line[160];
    for (int k = 0; k < want && k < n; k++) {
        int best = k;
        for (int i = k + 1; i < n; i++) if (rows[i].n > rows[best].n) best = i;
        Row r = rows[k]; rows[k] = rows[best]; rows[best] = r;
        if (pairs) ksnprintf(line, sizeof(line), "%5u %3u%%  %s <- %s", rows[k].n, rows[k].n * 100 / total, rows[k].a, rows[k].b);
        else ksnprintf(line, sizeof(line), "%5u %3u%%  %s", rows[k].n, rows[k].n * 100 / total, rows[k].a);
        kprintf("[PROF] %s\n", line);
        out(ctx, line);
    }
}

void ProfReport(void (*out)(void *ctx, const char *line), void *ctx)
{
    g_on = false;
    UINT32 n = g_n < PROF_MAX ? g_n : PROF_MAX;
    char line[160];
    if (!n) { out(ctx, "No samples (profile on first)."); return; }
    static Row fn[512], pair[1024];
    int nf = 0, np = 0;
    UINT32 user = 0, wait = 0, idle = 0;
    for (UINT32 i = 0; i < n; i++) {
        if (g_rip[i] == P_USER) { user++; continue; }
        if (g_rip[i] == P_LOCKWAIT) { wait++; continue; }
        if (g_rip[i] == P_IDLE) { idle++; continue; }
        UINT64 off;
        const char *a = KsymLookup(g_rip[i], &off), *b = g_caller[i] ? KsymLookup(g_caller[i], &off) : NULL;
        if (!a) a = "?";
        if (!b) b = "?";
        nf = add_row(fn, nf, 512, a, NULL);
        np = add_row(pair, np, 1024, a, b);
    }
    ksnprintf(line, sizeof(line), "%u samples: programs %u%%, kernel %u%%, waiting for the kernel lock %u%%, idle %u%%",
              n, user * 100 / n, (n - user - wait - idle) * 100 / n, wait * 100 / n, idle * 100 / n);
    kprintf("[PROF] %s\n", line);
    out(ctx, line);
    top(fn, nf, 15, n, false, out, ctx);
    out(ctx, "Callers:");
    top(pair, np, 15, n, true, out, ctx);
}
