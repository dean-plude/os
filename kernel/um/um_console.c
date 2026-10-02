/*
 * um_console.c — consoles: a program's standard input/output ↔ a Terminal
 *
 * Output is a ring with one reader (the desktop thread) and writers taking
 * turns (out_lock: programs' threads, without the big kernel lock); a
 * program that writes faster than the Terminal drains it waits.
 *
 * Input is a queue of Windows input records (INPUT_RECORD: key events,
 * and whatever programs add with WriteConsoleInput).  The Terminal adds
 * them: a whole line at a time while the program reads in line mode (the
 * Terminal edits the line), each key as it is pressed in raw mode.  Reads
 * in line mode wait for Enter; raw reads take whatever characters are
 * there; ReadConsoleInput takes the records themselves.  The console's
 * object (UO_CONSOLE) is signaled while records are queued, so programs
 * can wait on their input handle.
 */

#include "um_internal.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#define OUT_SIZE  (64 * 1024)
#define IN_RECS   1024

struct UmConsole {
    volatile int    refs;
    char            out[OUT_SIZE];
    volatile UINT32 out_head, out_tail;      /* head: next write, tail: next read */
    UmLock          out_lock;                /* one writer at a time */
    KSpinLock       in_lock;                 /* the input queue and pend[] */
    UmConInput      in[IN_RECS];
    UINT32          in_head, in_tail;
    volatile bool   in_eof;
    volatile bool   waiting;                 /* a program is blocked reading */
    char            pend[4];                 /* UTF-8 bytes of a character a read could not fit */
    int             pend_n;
    volatile UINT32 in_mode, out_mode;       /* SetConsoleMode */
    volatile UINT16 cols, rows;              /* the Terminal's size in cells */
    UmObject       *ob;                      /* UO_CONSOLE: signaled while input is queued */
};

UmConsole *UmConsoleNew(void)
{
    UmConsole *c = kzalloc(sizeof(*c));
    if (!c) return NULL;
    c->ob = kzalloc(sizeof(UmObject));
    if (!c->ob) { kfree(c); return NULL; }
    c->ob->type = UO_CONSOLE;
    c->ob->refs = 1;
    c->ob->ptr  = c;
    c->refs = 1;
    c->in_lock  = (KSpinLock)KSPINLOCK_INIT;
    c->in_mode  = CON_IN_DEFAULT;
    c->out_mode = CON_OUT_DEFAULT;
    c->cols = 80;
    c->rows = 25;
    return c;
}

UmConsole *um_console_ref(UmConsole *c)
{
    if (c) __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    return c;
}

void UmConsoleRelease(UmConsole *c)
{
    if (c && __atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        IrqState s = ob_lock();
        c->ob->ptr = NULL;                      /* waiters may still hold it */
        ob_unlock(s);
        um_ob_unref(c->ob);
        kfree(c);
    }
}

UmObject *um_console_object(UmConsole *c)
{
    return c ? um_ob_ref(c->ob) : NULL;
}

/* Program → Terminal */
int um_console_write(UmConsole *c, const char *data, int len)
{
    if (!c) return len;                                 /* no console: discard */
    UmProcess *p = UmCurrent();
    int done = 0;
    um_lock(&c->out_lock);
    while (done < len) {
        UINT32 used = c->out_head - c->out_tail;
        if (used == OUT_SIZE) {                         /* full: let the Terminal drain */
            if (p && um_stopping()) break;
            sched_wait();
            continue;
        }
        UINT32 n = OUT_SIZE - used;
        if (n > (UINT32)(len - done)) n = (UINT32)(len - done);
        for (UINT32 i = 0; i < n; i++) c->out[(c->out_head + i) % OUT_SIZE] = data[done + i];
        __atomic_store_n(&c->out_head, c->out_head + n, __ATOMIC_RELEASE);
        done += (int)n;
    }
    um_unlock(&c->out_lock);
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

/* -----------------------------------------------------------------------
 * Input queue
 * ----------------------------------------------------------------------- */
/* Under in_lock: the object's state follows the queue */
static void in_changed(UmConsole *c)
{
    bool sig = c->in_head != c->in_tail || c->in_eof;
    IrqState s = ob_lock();
    if (sig && !c->ob->signaled) um_ob_wake(c->ob);
    c->ob->signaled = sig;
    ob_unlock(s);
}

static bool is_key(const UmConInput *r)  { return r->type == CON_KEY_EVENT; }
static bool key_down(const UmConInput *r) { return r->key.down != 0; }

int UmConsolePushInput(UmConsole *c, const UmConInput *recs, int n)
{
    if (!c) return 0;
    IrqState s = spin_lock_irqsave(&c->in_lock);
    int done = 0;
    for (; done < n && c->in_head - c->in_tail < IN_RECS; done++)
        c->in[c->in_head++ % IN_RECS] = recs[done];
    in_changed(c);
    spin_unlock_irqrestore(&c->in_lock, s);
    return done;
}

void UmConsoleKey(UmConsole *c, UINT16 vk, UINT16 scan, UINT16 ch, UINT32 ctrl, bool down)
{
    UmConInput r;
    memset(&r, 0, sizeof(r));
    r.type = CON_KEY_EVENT;
    r.key.down   = down;
    r.key.repeat = 1;
    r.key.vk     = vk;
    r.key.scan   = scan;
    r.key.ch     = ch;
    r.key.ctrl   = ctrl;
    UmConsolePushInput(c, &r, 1);
}

/* Terminal → program: text typed (a line in line mode).  "\r\n" or "\n"
 * is Enter. */
void UmConsoleWrite(UmConsole *c, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        UINT32 ch = (UINT8)data[i];
        if (ch == '\r' && i + 1 < len && data[i + 1] == '\n') continue;
        if (ch == '\n') ch = '\r';
        else if (ch >= 0xC0 && i + 1 < len) {           /* UTF-8 */
            int k = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            ch &= 0x3F >> k;
            for (int j = 0; j < k && i + 1 < len; j++) ch = ch << 6 | ((UINT8)data[++i] & 0x3F);
            if (ch >= 0x10000) ch = '?';
        }
        UINT16 vk = ch == '\r' ? 0x0D : ch == 8 ? 0x08 : ch == 9 ? 0x09 : ch == 0x1B ? 0x1B :
                    (ch >= 'a' && ch <= 'z') ? (UINT16)(ch - 32) : (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' ? (UINT16)ch : 0;
        UmConsoleKey(c, vk, 0, (UINT16)ch, 0, true);
    }
}

void UmConsoleEof(UmConsole *c)
{
    IrqState s = spin_lock_irqsave(&c->in_lock);
    c->in_eof = true;
    in_changed(c);
    spin_unlock_irqrestore(&c->in_lock, s);
}

bool   UmConsoleWantsInput(UmConsole *c)  { return c->waiting; }
UINT32 UmConsoleInputMode(UmConsole *c)   { return c->in_mode; }
UINT32 UmConsoleOutputMode(UmConsole *c)  { return c->out_mode; }

void UmConsoleSetSize(UmConsole *c, int cols, int rows)
{
    if (cols == c->cols && rows == c->rows) return;
    c->cols = (UINT16)cols;
    c->rows = (UINT16)rows;
    if (c->in_mode & CON_ENABLE_WINDOW_INPUT) {         /* WINDOW_BUFFER_SIZE_EVENT */
        UmConInput r;
        memset(&r, 0, sizeof(r));
        r.type = CON_WINDOW_EVENT;
        r.raw[0] = (UINT32)cols | (UINT32)rows << 16;
        UmConsolePushInput(c, &r, 1);
    }
}

/* Under in_lock: append character @ch to @buf as UTF-8, keeping what does
 * not fit for the next read */
static int put_utf8(UmConsole *c, char *buf, int n, int cap, UINT32 ch)
{
    char t[4];
    int k = 0;
    if (ch < 0x80) t[k++] = (char)ch;
    else if (ch < 0x800) { t[k++] = (char)(0xC0 | ch >> 6); t[k++] = (char)(0x80 | (ch & 0x3F)); }
    else { t[k++] = (char)(0xE0 | ch >> 12); t[k++] = (char)(0x80 | ((ch >> 6) & 0x3F)); t[k++] = (char)(0x80 | (ch & 0x3F)); }
    for (int i = 0; i < k; i++) {
        if (n < cap) buf[n++] = t[i];
        else c->pend[c->pend_n++] = t[i];
    }
    return n;
}

/* Under in_lock: whether a line (up to Enter) is queued */
static bool line_ready(UmConsole *c)
{
    for (UINT32 i = c->in_tail; i != c->in_head; i++) {
        const UmConInput *r = &c->in[i % IN_RECS];
        if (is_key(r) && key_down(r) && r->key.ch == '\r') return true;
    }
    return false;
}

/* Under in_lock: whether a character is queued */
static bool char_ready(UmConsole *c)
{
    for (UINT32 i = c->in_tail; i != c->in_head; i++) {
        const UmConInput *r = &c->in[i % IN_RECS];
        if (is_key(r) && key_down(r) && r->key.ch) return true;
    }
    return false;
}

int um_console_read(UmConsole *c, char *buf, int cap, UmProcess *p)
{
    /* Line mode: wait for a complete line (or EOF); return at most that
     * line, with Enter as "\r\n".  Raw mode: wait for a character; return
     * the characters queued.  Records that are not characters (key
     * releases, arrows...) are dropped on the way, as on Windows. */
    if (!c) return 0;                                   /* no console: end of file */
    if (cap <= 0) return 0;
    for (;;) {
        IrqState s = spin_lock_irqsave(&c->in_lock);
        bool line = c->in_mode & CON_ENABLE_LINE_INPUT;
        int n = 0;
        while (c->pend_n && n < cap) {                  /* the rest of a character */
            buf[n++] = c->pend[0];
            memmove(c->pend, c->pend + 1, (size_t)--c->pend_n);
        }
        if (n || (line ? line_ready(c) : char_ready(c))) {
            while (c->in_tail != c->in_head && n < cap && !c->pend_n) {
                UmConInput *r = &c->in[c->in_tail % IN_RECS];
                c->in_tail++;
                if (!is_key(r) || !key_down(r) || !r->key.ch) continue;
                UINT32 ch = r->key.ch;
                if (ch >= 0xD800 && ch < 0xDC00) ch = '?';      /* (no surrogate pairs from keys) */
                if (ch == '\r' && line) {
                    n = put_utf8(c, buf, n, cap, '\r');
                    n = put_utf8(c, buf, n, cap, '\n');
                    break;
                }
                n = put_utf8(c, buf, n, cap, ch);
            }
            c->waiting = false;
            in_changed(c);
            spin_unlock_irqrestore(&c->in_lock, s);
            return n;
        }
        if (c->in_eof) {
            c->in_eof = false;
            c->waiting = false;
            in_changed(c);
            spin_unlock_irqrestore(&c->in_lock, s);
            return 0;
        }
        spin_unlock_irqrestore(&c->in_lock, s);
        if (p && um_stopping()) { c->waiting = false; return -1; }
        c->waiting = true;
        sched_wait();
    }
}

/* ReadConsoleInput / PeekConsoleInput: up to @max records into @out;
 * @wait: block until there is one */
int um_console_records(UmConsole *c, UmConInput *out, int max, bool remove, bool wait)
{
    if (!c) return 0;
    for (;;) {
        IrqState s = spin_lock_irqsave(&c->in_lock);
        int n = 0;
        for (UINT32 i = c->in_tail; i != c->in_head && n < max; i++) out[n++] = c->in[i % IN_RECS];
        if (n || !wait) {
            if (remove) { c->in_tail += (UINT32)n; in_changed(c); }
            spin_unlock_irqrestore(&c->in_lock, s);
            return n;
        }
        spin_unlock_irqrestore(&c->in_lock, s);
        if (um_stopping()) return -1;
        c->waiting = true;
        sched_wait();
    }
}

int um_console_count(UmConsole *c)
{
    if (!c) return 0;
    IrqState s = spin_lock_irqsave(&c->in_lock);
    int n = (int)(c->in_head - c->in_tail);
    spin_unlock_irqrestore(&c->in_lock, s);
    return n;
}

void um_console_flush(UmConsole *c)
{
    if (!c) return;
    IrqState s = spin_lock_irqsave(&c->in_lock);
    c->in_tail = c->in_head;
    c->pend_n = 0;
    in_changed(c);
    spin_unlock_irqrestore(&c->in_lock, s);
}

void um_console_set_mode(UmConsole *c, bool input, UINT32 mode)
{
    if (!c) return;
    if (input) c->in_mode = mode;
    else c->out_mode = mode;
}

void um_console_size(UmConsole *c, int *cols, int *rows)
{
    *cols = c ? c->cols : 80;
    *rows = c ? c->rows : 25;
}
