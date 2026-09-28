/*
 * um_console.c — consoles: a program's standard input/output ↔ a Terminal
 *
 * Output is a single-producer (program thread) / single-consumer (desktop
 * thread) ring; a program that writes faster than the Terminal drains it
 * waits.  Input is a second ring filled by the Terminal with whole lines
 * as the user presses Enter.  Neither side ever blocks the other.
 */

#include "um_internal.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#define OUT_SIZE  (64 * 1024)
#define IN_SIZE   4096

struct UmConsole {
    volatile int    refs;
    char            out[OUT_SIZE];
    volatile UINT32 out_head, out_tail;      /* head: next write, tail: next read */
    char            in[IN_SIZE];
    volatile UINT32 in_head, in_tail;
    volatile bool   in_eof;
    volatile bool   waiting;                 /* a program is blocked reading */
};

UmConsole *UmConsoleNew(void)
{
    UmConsole *c = kzalloc(sizeof(*c));
    if (c) c->refs = 1;
    return c;
}

UmConsole *um_console_ref(UmConsole *c)
{
    if (c) __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    return c;
}

void UmConsoleRelease(UmConsole *c)
{
    if (c && __atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(c);
}

/* Program → Terminal */
int um_console_write(UmConsole *c, const char *data, int len)
{
    if (!c) return len;                                 /* no console: discard */
    UmProcess *p = UmCurrent();
    int done = 0;
    while (done < len) {
        UINT32 used = c->out_head - c->out_tail;
        if (used == OUT_SIZE) {                         /* full: let the Terminal drain */
            if (p && um_stopping()) break;
            sched_yield();
            continue;
        }
        UINT32 n = OUT_SIZE - used;
        if (n > (UINT32)(len - done)) n = (UINT32)(len - done);
        for (UINT32 i = 0; i < n; i++) c->out[(c->out_head + i) % OUT_SIZE] = data[done + i];
        __atomic_store_n(&c->out_head, c->out_head + n, __ATOMIC_RELEASE);
        done += (int)n;
    }
    return done;
}

int UmConsoleRead(UmConsole *c, char *buf, int cap)
{
    UINT32 head = __atomic_load_n(&c->out_head, __ATOMIC_ACQUIRE);
    int n = 0;
    while (c->out_tail + (UINT32)n != head && n < cap) {
        buf[n] = c->out[(c->out_tail + (UINT32)n) % OUT_SIZE];
        n++;
    }
    __atomic_store_n(&c->out_tail, c->out_tail + (UINT32)n, __ATOMIC_RELEASE);
    return n;
}

/* Terminal → program */
void UmConsoleWrite(UmConsole *c, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        if (c->in_head - c->in_tail == IN_SIZE) break;           /* full: drop */
        c->in[c->in_head % IN_SIZE] = data[i];
        __atomic_store_n(&c->in_head, c->in_head + 1, __ATOMIC_RELEASE);
    }
}

void UmConsoleEof(UmConsole *c)           { c->in_eof = true; }
bool UmConsoleWantsInput(UmConsole *c)    { return c->waiting; }

int um_console_read(UmConsole *c, char *buf, int cap, UmProcess *p)
{
    /* Wait for a complete line (or EOF); return at most that line. */
    if (!c) return 0;                                   /* no console: end of file */
    for (;;) {
        UINT32 head = __atomic_load_n(&c->in_head, __ATOMIC_ACQUIRE);
        UINT32 avail = head - c->in_tail;
        bool line = false;
        for (UINT32 i = 0; i < avail; i++)
            if (c->in[(c->in_tail + i) % IN_SIZE] == '\n') { line = true; avail = i + 1; break; }
        if (line || (avail && c->in_eof)) {
            int n = (int)(avail < (UINT32)cap ? avail : (UINT32)cap);
            for (int i = 0; i < n; i++) buf[i] = c->in[(c->in_tail + (UINT32)i) % IN_SIZE];
            __atomic_store_n(&c->in_tail, c->in_tail + (UINT32)n, __ATOMIC_RELEASE);
            c->waiting = false;
            return n;
        }
        if (c->in_eof) { c->in_eof = false; c->waiting = false; return 0; }
        if (p && um_stopping()) { c->waiting = false; return -1; }
        c->waiting = true;
        sched_yield();
    }
}
