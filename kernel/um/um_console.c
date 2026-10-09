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
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#define OUT_SIZE  (64 * 1024)
#define IN_RECS   16384                         /* two lines of 8,191 characters (the Terminal's longest) */
#define CON_SCREENS 8
#define CON_MAX_CELLS (1024u * 1024u)

typedef struct {
    UmConsoleCell *cells;
    UINT16 cols, rows, cursor_x, cursor_y, attr;
    UINT32 utf8_char;
    UINT8 utf8_need;
    UINT32 refs;
    UINT8 esc_state, csi_n;
    char csi[32];
    bool used;
} UmConsoleScreen;

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
    volatile UINT32 cursor_style;          /* low byte: size 1..100, bit 8: visible */
    UmConsoleScreen screen[CON_SCREENS];
    UINT8 active_screen;
    UmObject       *ob;                      /* UO_CONSOLE: signaled while input is queued */
};

static int console_write_locked(UmConsole *c, const char *data, int len);

static UmConsoleScreen *console_screen(UmConsole *c, UINT32 id)
{
    return c && id < CON_SCREENS && c->screen[id].used ? &c->screen[id] : NULL;
}

static void screen_release_locked(UmConsole *c, UINT32 id)
{
    if (id && id < CON_SCREENS && id != c->active_screen &&
        c->screen[id].used && !c->screen[id].refs) {
        kfree(c->screen[id].cells);
        memset(&c->screen[id], 0, sizeof(c->screen[id]));
    }
}

static void screen_blank(UmConsoleScreen *s, UmConsoleCell cell)
{
    UINT32 n = (UINT32)s->cols * s->rows;
    for (UINT32 i = 0; i < n; i++) s->cells[i] = cell;
}

static int screen_sgr(char *seq, size_t cap, UINT16 attr)
{
    static const UINT8 map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    return ksnprintf(seq, cap, "\x1b[0;%u;%um",
                     30 + map[attr & 7] + ((attr & 8) ? 60 : 0),
                     40 + map[(attr >> 4) & 7] + ((attr & 0x80) ? 60 : 0));
}

static bool screen_resize(UmConsoleScreen *s, UINT32 cols, UINT32 rows)
{
    if (!cols || !rows || cols > 32767 || rows > 32767 || cols * (UINT64)rows > CON_MAX_CELLS)
        return false;
    UmConsoleCell *cells = kmalloc((size_t)cols * rows * sizeof(*cells));
    if (!cells) return false;
    for (UINT32 i = 0; i < cols * rows; i++) {
        cells[i].ch = ' ';
        cells[i].attr = s->attr;
    }
    UINT32 copy_cols = cols < s->cols ? cols : s->cols;
    UINT32 copy_rows = rows < s->rows ? rows : s->rows;
    for (UINT32 y = 0; y < copy_rows; y++)
        memcpy(cells + y * cols, s->cells + y * s->cols, copy_cols * sizeof(*cells));
    kfree(s->cells);
    s->cells = cells;
    s->cols = (UINT16)cols;
    s->rows = (UINT16)rows;
    if (s->cursor_x >= cols) s->cursor_x = (UINT16)(cols - 1);
    if (s->cursor_y >= rows) s->cursor_y = (UINT16)(rows - 1);
    return true;
}

/* Caller holds out_lock. */
static void screen_render(UmConsole *c, UmConsoleScreen *s)
{
    char seq[64];
    int n = ksnprintf(seq, sizeof(seq), "\x1b[2J\x1b[H");
    console_write_locked(c, seq, n);
    UINT16 old_attr = 0xFFFF;
    for (UINT32 y = 0; y < s->rows; y++) {
        n = ksnprintf(seq, sizeof(seq), "\x1b[%u;1H", y + 1);
        console_write_locked(c, seq, n);
        for (UINT32 x = 0; x < s->cols; x++) {
            UmConsoleCell *cell = &s->cells[y * s->cols + x];
            if (cell->attr != old_attr) {
                n = screen_sgr(seq, sizeof(seq), cell->attr);
                console_write_locked(c, seq, n);
                old_attr = cell->attr;
            }
            char ch[3];
            int count;
            if (cell->ch < 0x80) { ch[0] = (char)cell->ch; count = 1; }
            else if (cell->ch < 0x800) {
                ch[0] = (char)(0xC0 | (cell->ch >> 6));
                ch[1] = (char)(0x80 | (cell->ch & 0x3F));
                count = 2;
            } else if (cell->ch >= 0xD800 && cell->ch < 0xE000) {
                ch[0] = '?'; count = 1;
            } else {
                ch[0] = (char)(0xE0 | (cell->ch >> 12));
                ch[1] = (char)(0x80 | ((cell->ch >> 6) & 0x3F));
                ch[2] = (char)(0x80 | (cell->ch & 0x3F));
                count = 3;
            }
            console_write_locked(c, ch, count);
        }
    }
    n = screen_sgr(seq, sizeof(seq), s->attr);
    console_write_locked(c, seq, n);
    n = ksnprintf(seq, sizeof(seq), "\x1b[%u;%uH", s->cursor_y + 1, s->cursor_x + 1);
    console_write_locked(c, seq, n);
}

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
    c->cursor_style = 25 | 0x100;
    UmConsoleScreen *s = &c->screen[0];
    s->cells = kmalloc(80 * 25 * sizeof(*s->cells));
    if (!s->cells) {
        um_ob_unref(c->ob);
        kfree(c);
        return NULL;
    }
    s->cols = 80;
    s->rows = 25;
    s->attr = 7;
    s->used = true;
    screen_blank(s, (UmConsoleCell){ ' ', 7 });
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
        for (int i = 0; i < CON_SCREENS; i++) kfree(c->screen[i].cells);
        um_ob_unref(c->ob);
        kfree(c);
    }
}

UmObject *um_console_object(UmConsole *c)
{
    return c ? um_ob_ref(c->ob) : NULL;
}

/* The unfinished line of a detached process, to the kernel log */
void um_console_flush_log(UmProcess *p)
{
    if (!p || !p->log_n) return;
    p->log_line[p->log_n] = 0;
    kprintf("[UM] %s (PID %u, detached): %s\n", p->name, p->pid, p->log_line);
    p->log_n = 0;
}

/* Caller holds out_lock; a control sequence stays together with ordinary output. */
static int console_write_locked(UmConsole *c, const char *data, int len)
{
    UmProcess *p = UmCurrent();
    int done = 0;
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
    return done;
}

/* Program → Terminal */
int um_console_write(UmConsole *c, const char *data, int len)
{
    if (!c) {
        /* No console (a detached process handed its creator's console
         * handles, as Firefox's sandbox does for its child processes):
         * the kernel log, a line at a time.  An unfinished line waits in
         * the process for the rest (VLC writes its log a character at a
         * time) and goes out when it ends or the process does. */
        UmProcess *me = UmCurrent();
        if (!me) return len;
        for (int i = 0; i < len; i++) {
            char ch = data[i];
            if (ch != '\n' && ch != '\r' && me->log_n < (int)sizeof(me->log_line) - 1) me->log_line[me->log_n++] = ch;
            if ((ch == '\n' || me->log_n == (int)sizeof(me->log_line) - 1) && me->log_n) um_console_flush_log(me);
        }
        return len;
    }
    return um_console_write_screen(c, 0, data, len);
}

static void screen_advance(UmConsoleScreen *s)
{
    s->cursor_x = 0;
    if (++s->cursor_y < s->rows) return;
    memmove(s->cells, s->cells + s->cols,
            (size_t)(s->rows - 1) * s->cols * sizeof(*s->cells));
    for (UINT32 x = 0; x < s->cols; x++) {
        s->cells[(s->rows - 1) * s->cols + x].ch = ' ';
        s->cells[(s->rows - 1) * s->cols + x].attr = s->attr;
    }
    s->cursor_y = s->rows - 1;
}

static void screen_put(UmConsoleScreen *s, UINT16 ch)
{
    s->cells[s->cursor_y * s->cols + s->cursor_x] = (UmConsoleCell){ ch, s->attr };
    if (++s->cursor_x >= s->cols) screen_advance(s);
}

static void screen_csi_apply(UmConsoleScreen *s, char final)
{
    int args[4] = { 0, 0, 0, 0 }, n = 0;
    for (int i = 0; i < s->csi_n; i++) {
        char c = s->csi[i];
        if (c >= '0' && c <= '9') args[n] = args[n] * 10 + c - '0';
        else if (c == ';' && n < 3) n++;
    }
    switch (final) {
    case 'H': case 'f':
        s->cursor_y = (UINT16)(args[0] > 0 && args[0] <= s->rows ? args[0] - 1 : 0);
        s->cursor_x = (UINT16)(args[1] > 0 && args[1] <= s->cols ? args[1] - 1 : 0);
        break;
    case 'A': s->cursor_y = args[0] >= s->cursor_y ? 0 : (UINT16)(s->cursor_y - (args[0] ? args[0] : 1)); break;
    case 'B': s->cursor_y = (UINT16)(s->cursor_y + (args[0] ? args[0] : 1) < s->rows ? s->cursor_y + (args[0] ? args[0] : 1) : s->rows - 1); break;
    case 'C': s->cursor_x = (UINT16)(s->cursor_x + (args[0] ? args[0] : 1) < s->cols ? s->cursor_x + (args[0] ? args[0] : 1) : s->cols - 1); break;
    case 'D': s->cursor_x = args[0] >= s->cursor_x ? 0 : (UINT16)(s->cursor_x - (args[0] ? args[0] : 1)); break;
    case 'J':
        if (args[0] == 2) screen_blank(s, (UmConsoleCell){ ' ', s->attr });
        break;
    case 'K':
        if (!args[0]) for (UINT32 x = s->cursor_x; x < s->cols; x++)
            s->cells[s->cursor_y * s->cols + x] = (UmConsoleCell){ ' ', s->attr };
        break;
    case 'm':
        {
        static const UINT8 map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
        for (int i = 0; i <= n; i++) {
            int v = args[i];
            if (!v) s->attr = 7;
            else if (v >= 30 && v <= 37) s->attr = (UINT16)((s->attr & 0xF8) | map[v - 30]);
            else if (v >= 90 && v <= 97) s->attr = (UINT16)((s->attr & 0xF0) | map[v - 90] | 8);
            else if (v >= 40 && v <= 47) s->attr = (UINT16)((s->attr & 0x8F) | (map[v - 40] << 4));
            else if (v >= 100 && v <= 107) s->attr = (UINT16)((s->attr & 0x0F) | (map[v - 100] << 4) | 0x80);
            else if (v == 1) s->attr |= 8;
            else if (v == 22) s->attr &= ~8;
        }
        break;
        }
    }
}

int um_console_write_screen(UmConsole *c, UINT32 id, const char *data, int len)
{
    if (!c) return um_console_write(NULL, data, len);
    um_lock(&c->out_lock);
    UmConsoleScreen *s = console_screen(c, id);
    if (!s) { um_unlock(&c->out_lock); return 0; }
    for (int i = 0; i < len; i++) {
        UINT8 ch = (UINT8)data[i];
        if (s->esc_state == 1) {
            s->esc_state = ch == '[' ? 2 : 0;
            s->csi_n = 0;
            continue;
        }
        if (s->esc_state == 2) {
            if (ch >= 0x40 && ch <= 0x7E) {
                screen_csi_apply(s, (char)ch);
                s->esc_state = 0;
            } else if (ch >= 0x20 && ch <= 0x3F && s->csi_n < sizeof(s->csi)) {
                s->csi[s->csi_n++] = (char)ch;
            } else s->esc_state = 0;
            continue;
        }
        if (ch == 0x1B) { s->esc_state = 1; continue; }
        if (ch == '\r') { s->cursor_x = 0; continue; }
        if (ch == '\n') { screen_advance(s); continue; }
        if (ch == '\b') { if (s->cursor_x) s->cursor_x--; continue; }
        if (ch == '\t') {
            UINT32 next = ((UINT32)s->cursor_x + 8) & ~7u;
            s->cursor_x = (UINT16)(next < s->cols ? next : s->cols - 1);
            continue;
        }
        if (s->utf8_need) {
            if ((ch & 0xC0) == 0x80) {
                s->utf8_char = (s->utf8_char << 6) | (ch & 0x3F);
                if (!--s->utf8_need) screen_put(s, s->utf8_char < 0x10000 ? (UINT16)s->utf8_char : '?');
            } else s->utf8_need = 0;
            continue;
        }
        if (ch >= 0xC2 && ch <= 0xDF) { s->utf8_char = ch & 0x1F; s->utf8_need = 1; continue; }
        if (ch >= 0xE0 && ch <= 0xEF) { s->utf8_char = ch & 0x0F; s->utf8_need = 2; continue; }
        if (ch >= 0xF0 && ch <= 0xF4) { s->utf8_char = ch & 7; s->utf8_need = 3; continue; }
        if (ch < 0x20 || ch >= 0x80) continue;
        screen_put(s, ch);
    }
    int done = id == c->active_screen ? console_write_locked(c, data, len) : len;
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
    if (sig && !c->ob->signaled) um_ob_wake_boost(c->ob, BOOST_KEYBOARD);   /* (console input, as from the keyboard) */
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

/* The classic cursor APIs use the Terminal's existing VT screen. No user
 * output-mode bit is needed: this sequence was generated by the console. */
UINT32 um_console_cursor(UmConsole *c, UINT32 op, UINT32 value, UINT32 *result)
{
    if (!c) return 0xC0000008u;
    if (op == 10) { *result = UmConsoleCursorStyle(c); return 0; }
    UmConsoleScreen *screen = console_screen(c, 0);
    if (!screen) return 0xC0000008u;
    char seq[32];
    int n;
    if (op == 9) {
        int x = (INT16)(value & 0xFFFF), y = (INT16)(value >> 16);
        if (x < 0 || y < 0 || x >= screen->cols || y >= screen->rows) return 0xC000000Du;
        n = ksnprintf(seq, sizeof(seq), "\x1b[%d;%dH", y + 1, x + 1);
    } else if (op == 11) {
        if ((value & 0xFF) < 1 || (value & 0xFF) > 100 || (value & ~0x1FFu)) return 0xC000000Du;
        n = ksnprintf(seq, sizeof(seq), "\x1b[?25%c", (value & 0x100) ? 'h' : 'l');
    } else return 0xC000000Du;
    um_lock(&c->out_lock);
    while (OUT_SIZE - (c->out_head - c->out_tail) < (UINT32)n) {
        if (um_stopping()) { um_unlock(&c->out_lock); return 0xC0000120u; }
        sched_wait();
    }
    int done = console_write_locked(c, seq, n);
    if (done == n && op == 9) { screen->cursor_x = (UINT16)(value & 0xFFFF); screen->cursor_y = (UINT16)(value >> 16); }
    if (done == n && op == 11) __atomic_store_n(&c->cursor_style, value, __ATOMIC_RELEASE);
    um_unlock(&c->out_lock);
    return done == n ? 0 : 0xC0000120u;
}

UINT32 um_console_screen_create(UmConsole *c, UINT32 *id)
{
    if (!c || !id) return 0xC0000008u;
    um_lock(&c->out_lock);
    UINT32 i;
    for (i = 1; i < CON_SCREENS && c->screen[i].used; i++) {}
    if (i == CON_SCREENS) { um_unlock(&c->out_lock); return 0xC000009Au; }
    UmConsoleScreen *s = &c->screen[i];
    memset(s, 0, sizeof(*s));
    s->cols = c->cols;
    s->rows = c->rows;
    s->attr = 7;
    s->cells = kmalloc((size_t)s->cols * s->rows * sizeof(*s->cells));
    if (!s->cells) { um_unlock(&c->out_lock); return 0xC0000017u; }
    s->used = true;
    s->refs = 1;
    screen_blank(s, (UmConsoleCell){ ' ', 7 });
    *id = i;
    um_unlock(&c->out_lock);
    return 0;
}

void um_console_screen_ref(UmConsole *c, UINT32 id)
{
    if (!c || !id) return;
    um_lock(&c->out_lock);
    UmConsoleScreen *s = console_screen(c, id);
    if (s) s->refs++;
    um_unlock(&c->out_lock);
}

void um_console_screen_unref(UmConsole *c, UINT32 id)
{
    if (!c || !id) return;
    um_lock(&c->out_lock);
    UmConsoleScreen *s = console_screen(c, id);
    if (s && s->refs) {
        s->refs--;
        screen_release_locked(c, id);
    }
    um_unlock(&c->out_lock);
}

static bool screen_coord(const UmConsoleScreen *s, int x, int y)
{
    return x >= 0 && y >= 0 && x < s->cols && y < s->rows;
}

UINT32 um_console_screen_call(UmConsole *c, UINT32 id, UINT32 op, void *data, UINT32 len, UINT32 *result)
{
    if (!c) return 0xC0000008u;
    if (result) *result = 0;
    um_lock(&c->out_lock);
    UmConsoleScreen *s = console_screen(c, id);
    if (!s) { um_unlock(&c->out_lock); return 0xC0000008u; }
    UINT32 status = 0;
    bool redraw = false;
    if (op == CON_SCREEN_INFO) {
        if (!data || len < sizeof(UmConsoleScreenInfo)) status = 0xC000000Du;
        else {
            UINT16 win_cols = s->cols < c->cols ? s->cols : c->cols;
            UINT16 win_rows = s->rows < c->rows ? s->rows : c->rows;
            UmConsoleScreenInfo info = {
                (INT16)s->cols, (INT16)s->rows, (INT16)s->cursor_x, (INT16)s->cursor_y,
                s->attr, 0, 0, (INT16)(win_cols - 1), (INT16)(win_rows - 1),
                (INT16)c->cols, (INT16)c->rows
            };
            memcpy(data, &info, sizeof(info));
        }
    } else if (op == CON_SCREEN_CREATE) {
        status = 0xC000000Du;
    } else if (op == CON_SCREEN_ACTIVATE) {
        UINT32 old = c->active_screen;
        c->active_screen = (UINT8)id;
        screen_render(c, s);
        screen_release_locked(c, old);
    } else if (op == CON_SCREEN_SET_ATTR || op == CON_SCREEN_SET_CURSOR ||
               op == CON_SCREEN_SET_SIZE || op == CON_SCREEN_FILL_CHAR ||
               op == CON_SCREEN_FILL_ATTR || op == CON_SCREEN_READ_TEXT ||
               op == CON_SCREEN_READ_ATTR || op == CON_SCREEN_WRITE_CELLS ||
               op == CON_SCREEN_READ_CELLS || op == CON_SCREEN_SCROLL) {
        if (!data || len < sizeof(UmConsoleScreenRequest)) status = 0xC000000Du;
        else {
            UmConsoleScreenRequest *r = data;
            UINT32 total = (UINT32)s->cols * s->rows;
            if (op == CON_SCREEN_SET_ATTR) {
                s->attr = r->value;
                redraw = id == c->active_screen;
            } else if (op == CON_SCREEN_SET_CURSOR) {
                if (!screen_coord(s, r->x, r->y)) status = 0xC000000Du;
                else {
                    char seq[32];
                    int n = ksnprintf(seq, sizeof(seq), "\x1b[%d;%dH", r->y + 1, r->x + 1);
                    int done = id == c->active_screen ? console_write_locked(c, seq, n) : n;
                    if (done != n) status = 0xC0000120u;
                    else { s->cursor_x = (UINT16)r->x; s->cursor_y = (UINT16)r->y; }
                }
            } else if (op == CON_SCREEN_SET_SIZE) {
                if (r->width <= 0 || r->height <= 0) status = 0xC000000Du;
                else if (r->width > 32767 || r->height > 32767 ||
                         (UINT64)(UINT16)r->width * (UINT16)r->height > CON_MAX_CELLS) status = 0xC000000Du;
                else if (!screen_resize(s, (UINT16)r->width, (UINT16)r->height)) status = 0xC0000017u;
                else redraw = id == c->active_screen;
            } else if (op == CON_SCREEN_FILL_CHAR || op == CON_SCREEN_FILL_ATTR ||
                       op == CON_SCREEN_READ_TEXT || op == CON_SCREEN_READ_ATTR) {
                UINT32 at;
                if (!screen_coord(s, r->x, r->y)) status = 0xC000000Du;
                else if (r->count > (len - sizeof(*r)) / (op == CON_SCREEN_READ_TEXT ? 2u :
                                                            op == CON_SCREEN_READ_ATTR ? 2u : 1u) &&
                         (op == CON_SCREEN_READ_TEXT || op == CON_SCREEN_READ_ATTR)) status = 0xC000000Du;
                else {
                    at = (UINT32)r->y * s->cols + (UINT32)r->x;
                    UINT32 done = r->count < total - at ? r->count : total - at;
                    if (op == CON_SCREEN_FILL_CHAR)
                        for (UINT32 i = 0; i < done; i++) s->cells[at + i].ch = r->value;
                    else if (op == CON_SCREEN_FILL_ATTR)
                        for (UINT32 i = 0; i < done; i++) s->cells[at + i].attr = r->value;
                    else if (op == CON_SCREEN_READ_TEXT) {
                        UINT16 *out = (UINT16 *)((UINT8 *)data + sizeof(*r));
                        for (UINT32 i = 0; i < done; i++) out[i] = s->cells[at + i].ch;
                    } else {
                        UINT16 *out = (UINT16 *)((UINT8 *)data + sizeof(*r));
                        for (UINT32 i = 0; i < done; i++) out[i] = s->cells[at + i].attr;
                    }
                    if (result) *result = done;
                    redraw = (op == CON_SCREEN_FILL_CHAR || op == CON_SCREEN_FILL_ATTR) && id == c->active_screen;
                }
            } else if (op == CON_SCREEN_WRITE_CELLS || op == CON_SCREEN_READ_CELLS) {
                if (r->width <= 0 || r->height <= 0 || r->left > r->right || r->top > r->bottom ||
                    r->x < 0 || r->y < 0 || r->x + r->right - r->left >= r->width ||
                    r->y + r->bottom - r->top >= r->height ||
                    (UINT64)(UINT16)r->width * (UINT16)r->height >
                        (len - sizeof(*r)) / sizeof(UmConsoleCell)) status = 0xC000000Du;
                else {
                    int l = r->left, t = r->top, rr = r->right, b = r->bottom;
                    if (l < 0) l = 0;
                    if (t < 0) t = 0;
                    if (rr >= s->cols) rr = s->cols - 1;
                    if (b >= s->rows) b = s->rows - 1;
                    if (l > rr || t > b) status = 0xC000000Du;
                    else {
                        UmConsoleCell *cells = (UmConsoleCell *)((UINT8 *)data + sizeof(*r));
                        for (int y = t; y <= b; y++) for (int x = l; x <= rr; x++) {
                            UmConsoleCell *dst = &s->cells[y * s->cols + x];
                            UmConsoleCell *src = &cells[((r->y + y - r->top) * r->width) + r->x + x - r->left];
                            if (op == CON_SCREEN_WRITE_CELLS) *dst = *src;
                            else *src = *dst;
                        }
                        r->left = (INT16)l; r->top = (INT16)t; r->right = (INT16)rr; r->bottom = (INT16)b;
                        redraw = op == CON_SCREEN_WRITE_CELLS && id == c->active_screen;
                    }
                }
            } else if (op == CON_SCREEN_SCROLL) {
                if (r->left < 0 || r->top < 0 || r->left > r->right || r->top > r->bottom ||
                    r->right >= s->cols || r->bottom >= s->rows ||
                    (r->flags && (r->clip_left > r->clip_right || r->clip_top > r->clip_bottom)))
                    status = 0xC000000Du;
                else {
                    int width = r->right - r->left + 1, height = r->bottom - r->top + 1;
                    size_t bytes = (size_t)width * height * sizeof(UmConsoleCell);
                    UmConsoleCell *copy = kmalloc(bytes);
                    if (!copy) status = 0xC0000017u;
                    else {
                        UmConsoleCell fill = { r->value, r->attributes };
                        for (int y = 0; y < height; y++)
                            memcpy(copy + y * width, s->cells + (r->top + y) * s->cols + r->left,
                                   width * sizeof(*copy));
                        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
                            int sy = r->top + y, sx = r->left + x;
                            s->cells[sy * s->cols + sx] = fill;
                            int dy = r->dest_y + y, dx = r->dest_x + x;
                            bool in_clip = !r->flags ||
                                (dx >= r->clip_left && dx <= r->clip_right &&
                                 dy >= r->clip_top && dy <= r->clip_bottom);
                            if (in_clip && dx >= 0 && dy >= 0 && dx < s->cols && dy < s->rows)
                                s->cells[dy * s->cols + dx] = copy[y * width + x];
                        }
                        kfree(copy);
                        redraw = id == c->active_screen;
                    }
                }
            }
        }
    } else status = 0xC000000Du;
    if (!status && redraw) screen_render(c, s);
    um_unlock(&c->out_lock);
    return status;
}

UINT32 UmConsoleCursorStyle(UmConsole *c)
{
    return c ? __atomic_load_n(&c->cursor_style, __ATOMIC_ACQUIRE) : 25 | 0x100;
}
