/*
 * um_pipe.c — named and anonymous pipes (the kernel half of NPFS)
 *
 * A pipe instance joins a server end (NtCreateNamedPipeFile) and one
 * client end (NtCreateFile on \Device\NamedPipe\NAME); CreatePipe makes an
 * instance with a unique name and opens its client end at once.  Each
 * direction is a ring buffer (growing as needed for overlapped and
 * message writes); message-type pipes also keep the length of every
 * message, so a reader in message mode gets one message per read.
 *
 * End objects are UmObjects, so handles, duplication and inheritance work
 * as for any object, and closing the last handle to an end is what the
 * other side sees: a reader drains what is left and then gets
 * STATUS_PIPE_BROKEN (ERROR_BROKEN_PIPE, end of file), a writer gets
 * STATUS_PIPE_CLOSING (ERROR_NO_DATA).
 *
 * Synchronous handles block (polling, like the console).  On an
 * overlapped handle a read, write or listen that cannot finish at once is
 * left pending: whoever changes the pipe later (the writer, the reader,
 * a connecting client, a close) finishes it, writing the data and the
 * I/O status block into the issuing process and setting its event.
 *
 * Lock order: the big kernel lock, then g_pl; nothing here takes the big
 * lock (or drops the last reference to an event) while holding g_pl.
 */

#include "um_internal.h"
#include "../ke/printf.h"
#include "../ke/probe.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#define ST_SUCCESS               0x00000000u
#define ST_PENDING               0x00000103u
#define ST_BUFFER_OVERFLOW       0x80000005u
#define ST_INVALID_HANDLE        0xC0000008u
#define ST_INVALID_PARAMETER     0xC000000Du
#define ST_INVALID_DEVICE_REQ    0xC0000010u
#define ST_NO_MEMORY             0xC0000017u
#define ST_ACCESS_DENIED         0xC0000022u
#define ST_BUFFER_TOO_SMALL      0xC0000023u
#define ST_OBJECT_NAME_INVALID   0xC0000033u
#define ST_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define ST_INSTANCE_NOT_AVAIL    0xC00000ABu
#define ST_PIPE_NOT_AVAILABLE    0xC00000ACu
#define ST_PIPE_DISCONNECTED     0xC00000B0u
#define ST_PIPE_CLOSING          0xC00000B1u
#define ST_PIPE_CONNECTED        0xC00000B2u
#define ST_PIPE_LISTENING        0xC00000B3u
#define ST_IO_TIMEOUT            0xC00000B5u
#define ST_PIPE_EMPTY            0xC00000D9u
#define ST_CANCELLED             0xC0000120u
#define ST_PIPE_BROKEN           0xC000014Bu
#define ST_NOT_FOUND             0xC0000225u
#define ST_ACCESS_VIOLATION      0xC0000005u

#define PIPE_MIN_BUF   (64 * 1024)
#define PIPE_MAX_BUF   (4 * 1024 * 1024)
#define PIPE_MAX_MSGS  256
#define CHUNK          (64 * 1024)

/* NamedPipeState values (FilePipeLocalInformation, PeekNamedPipe) */
enum { P_DISCONNECTED = 1, P_LISTENING = 2, P_CONNECTED = 3, P_CLOSING = 4 };

typedef struct {
    UINT8  *buf;
    UINT32  cap, head, tail;                /* head: bytes ever written, tail: read */
    UINT32  msg[PIPE_MAX_MSGS];             /* message-type pipes: lengths, oldest first */
    UINT32  mhead, mtail;
} Ring;

typedef struct PipeIo PipeIo;
typedef struct PipeEnd PipeEnd;

typedef struct UmPipe {
    struct UmPipe *next;
    char     name[96];                      /* lower case, without \Device\NamedPipe\ */
    UINT32   max_inst;
    bool     msg_type;                      /* PIPE_TYPE_MESSAGE: every write is a message */
    int      state;
    int      srv_ends, cli_ends;            /* end objects alive */
    UINT32   gen;                           /* which client connection is current */
    UINT32   in_quota, out_quota;
    UINT32   client_pid;
    Ring     r[2];                          /* [0] client -> server, [1] server -> client */
} UmPipe;

struct PipeEnd {
    UmObject ob;                            /* first: um_ob_unref frees the whole end */
    UmPipe  *pipe;
    int      side;                          /* 0 server, 1 client */
    UINT32   gen;                           /* client: its connection */
    bool     msg_read;                      /* PIPE_READMODE_MESSAGE */
    bool     nowait;                        /* PIPE_NOWAIT */
    bool     async;                         /* opened for overlapped I/O */
    bool     can_read, can_write;
};

enum { IO_READ, IO_WRITE, IO_LISTEN };

struct PipeIo {
    PipeIo    *next;
    PipeEnd   *end;                         /* not referenced: its close cancels us */
    UmProcess *proc;
    int        op;
    UINT64     buf, iosb;
    UINT32     len, done;
    UINT8     *data;                        /* a write's bytes not yet in the ring */
    UmObject  *event;                       /* referenced, or NULL */
};

static UmLock  g_pl;
static UmPipe *g_pipes;
static PipeIo *g_io;                        /* every pending operation, oldest first */

/* Events whose last reference may go: dropped after g_pl is released */
#define MAX_DEFER 32
typedef struct { UmObject *o[MAX_DEFER]; int n; } Defer;

static void defer_unref(Defer *d, UmObject *o)
{
    if (!o) return;
    if (d->n < MAX_DEFER) d->o[d->n++] = o;
    else um_ob_unref(o);                    /* (never more than a few at once) */
}

static void defer_run(Defer *d)
{
    for (int i = 0; i < d->n; i++) um_ob_unref(d->o[i]);
    d->n = 0;
}

/* -----------------------------------------------------------------------
 * Rings
 * ----------------------------------------------------------------------- */
static UINT32 ring_used(const Ring *r) { return r->head - r->tail; }
static UINT32 ring_msgs(const Ring *r) { return r->mhead - r->mtail; }

static bool ring_init(Ring *r, UINT32 cap)
{
    if (cap < PIPE_MIN_BUF) cap = PIPE_MIN_BUF;
    r->buf = kmalloc(cap);
    r->cap = r->buf ? cap : 0;
    r->head = r->tail = r->mhead = r->mtail = 0;
    return r->buf != NULL;
}

static void ring_reset(Ring *r) { r->head = r->tail = r->mhead = r->mtail = 0; }

/* Make room for @need more bytes (up to PIPE_MAX_BUF in all) */
static bool ring_grow(Ring *r, UINT32 need)
{
    UINT32 used = ring_used(r);
    if (r->cap - used >= need) return true;
    if (used + need > PIPE_MAX_BUF) return false;
    UINT32 cap = r->cap;
    while (cap - used < need) cap *= 2;
    if (cap > PIPE_MAX_BUF) cap = PIPE_MAX_BUF;
    UINT8 *b = kmalloc(cap);
    if (!b) return false;
    for (UINT32 i = 0; i < used; i++) b[i] = r->buf[(r->tail + i) % r->cap];
    kfree(r->buf);
    r->buf = b;
    r->cap = cap;
    r->head = used;
    r->tail = 0;
    return true;
}

static void ring_put(Ring *r, const UINT8 *src, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) r->buf[(r->head + i) % r->cap] = src[i];
    r->head += n;
}

static void ring_peek(const Ring *r, UINT8 *dst, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) dst[i] = r->buf[(r->tail + i) % r->cap];
}

/* Take @n bytes, keeping the message lengths in step */
static void ring_take(Ring *r, UINT8 *dst, UINT32 n, bool msgs)
{
    if (dst) ring_peek(r, dst, n);
    r->tail += n;
    while (msgs && n && ring_msgs(r)) {
        UINT32 *m = &r->msg[r->mtail % PIPE_MAX_MSGS];
        if (*m > n) { *m -= n; n = 0; break; }
        n -= *m;
        r->mtail++;
    }
}

/* -----------------------------------------------------------------------
 * The operations, under g_pl.  ST_PENDING: cannot finish yet.
 * ----------------------------------------------------------------------- */
static bool connected_client(const PipeEnd *e)
{
    return e->gen == e->pipe->gen && (e->pipe->state == P_CONNECTED || e->pipe->state == P_CLOSING);
}

/* Read up to @len bytes into @dst (a kernel buffer of at least @len) */
static UINT32 try_read(PipeEnd *e, UINT8 *dst, UINT32 len, UINT32 *got)
{
    UmPipe *p = e->pipe;
    *got = 0;
    if (e->side == 1 && !connected_client(e)) return p->srv_ends ? ST_PIPE_DISCONNECTED : ST_PIPE_BROKEN;
    Ring *r = &p->r[e->side == 0 ? 0 : 1];
    UINT32 used = ring_used(r);
    if (p->msg_type && ring_msgs(r)) {
        UINT32 m = r->msg[r->mtail % PIPE_MAX_MSGS];
        if (e->msg_read) {
            UINT32 n = m < len ? m : len;
            ring_take(r, dst, n, true);
            if (n == m && m == 0) r->mtail++;           /* an empty message */
            *got = n;
            return n < m ? ST_BUFFER_OVERFLOW : ST_SUCCESS;
        }
        if (!used) { while (ring_msgs(r) && !r->msg[r->mtail % PIPE_MAX_MSGS]) r->mtail++; }
    }
    if (used) {
        UINT32 n = used < len ? used : len;
        ring_take(r, dst, n, p->msg_type);
        *got = n;
        return ST_SUCCESS;
    }
    if (e->side == 0) {
        switch (p->state) {
        case P_LISTENING:    return ST_PIPE_LISTENING;
        case P_DISCONNECTED: return ST_PIPE_DISCONNECTED;
        case P_CLOSING:      return ST_PIPE_BROKEN;
        }
    } else if (!p->srv_ends) return ST_PIPE_BROKEN;
    return ST_PENDING;
}

/* Put what fits of @src[0..len) (all of it at once for a message, or
 * growing the ring when @grow); *put gets the bytes taken */
static UINT32 try_write(PipeEnd *e, const UINT8 *src, UINT32 len, bool whole_msg, bool grow, UINT32 *put)
{
    UmPipe *p = e->pipe;
    *put = 0;
    if (e->side == 0) {
        switch (p->state) {
        case P_LISTENING:    return ST_PIPE_LISTENING;
        case P_DISCONNECTED: return ST_PIPE_DISCONNECTED;
        case P_CLOSING:      return ST_PIPE_CLOSING;
        }
    } else {
        if (!p->srv_ends) return ST_PIPE_CLOSING;
        if (!connected_client(e)) return ST_PIPE_DISCONNECTED;
    }
    Ring *r = &p->r[e->side == 0 ? 1 : 0];
    if (p->msg_type && whole_msg) {
        if (ring_msgs(r) >= PIPE_MAX_MSGS) return ST_PENDING;
        if (!ring_grow(r, len)) return ST_PENDING;
        ring_put(r, src, len);
        r->msg[r->mhead++ % PIPE_MAX_MSGS] = len;
        *put = len;
        return ST_SUCCESS;
    }
    UINT32 space = r->cap - ring_used(r);
    if (space < len && grow) { ring_grow(r, len); space = r->cap - ring_used(r); }
    UINT32 n = space < len ? space : len;
    ring_put(r, src, n);
    *put = n;
    return n == len ? ST_SUCCESS : ST_PENDING;
}

/* -----------------------------------------------------------------------
 * Pending operations
 * ----------------------------------------------------------------------- */
/* Write an I/O status block into @p (@iosb: bit 63 = the 32-bit layout) */
static void put_iosb(UmProcess *p, UINT64 iosb, UINT32 st, UINT64 info)
{
    if (!iosb) return;
    if (iosb >> 63) {
        UINT32 b[2] = { st, (UINT32)info };
        um_write(p, iosb & ~(UINT64_C(1) << 63), b, sizeof(b));
    } else {
        UINT64 b[2] = { st, info };
        um_write(p, iosb, b, sizeof(b));
    }
}

static void ev_signal(UmObject *ev)
{
    if (!ev) return;
    IrqState s = ob_lock();
    ev->signaled = true;
    um_ob_wake(ev);
    ob_unlock(s);
}

static void io_unlink(PipeIo *io)
{
    for (PipeIo **pp = &g_io; *pp; pp = &(*pp)->next)
        if (*pp == io) { *pp = io->next; return; }
}

static void io_finish(PipeIo *io, UINT32 st, UINT64 info, Defer *d)
{
    io_unlink(io);
    put_iosb(io->proc, io->iosb, st, info);
    ev_signal(io->event);
    defer_unref(d, io->event);
    kfree(io->data);
    kfree(io);
}

/* Move pending operations on @p along as far as they go */
static void service(UmPipe *p, Defer *d)
{
    bool again = true;
    while (again) {
        again = false;
        bool read_busy[2] = { false, false }, write_busy[2] = { false, false };
        for (PipeIo *io = g_io, *next; io; io = next) {
            next = io->next;
            PipeEnd *e = io->end;
            if (e->pipe != p) continue;
            int rd = e->side == 0 ? 0 : 1, wd = 1 - rd;
            if (io->op == IO_LISTEN) {
                if (p->state == P_CONNECTED || p->state == P_CLOSING) io_finish(io, ST_SUCCESS, 0, d);
                continue;
            }
            if (io->op == IO_READ) {
                if (read_busy[rd]) continue;             /* first come, first served */
                UINT32 want = io->len < CHUNK ? io->len : CHUNK, got;
                UINT8 small[256], *tmp = want <= sizeof(small) ? small : kmalloc(want);
                if (!tmp) { read_busy[rd] = true; continue; }
                UINT32 st = try_read(e, tmp, want, &got);
                if (st == ST_PENDING) { read_busy[rd] = true; if (tmp != small) kfree(tmp); continue; }
                if (got && !um_write(io->proc, io->buf, tmp, got)) st = ST_ACCESS_VIOLATION;
                if (tmp != small) kfree(tmp);
                io_finish(io, st, got, d);
                again = true;
                continue;
            }
            /* IO_WRITE */
            if (write_busy[wd]) continue;
            UINT32 put;
            UINT32 st = try_write(e, io->data + io->done, io->len - io->done, true, true, &put);
            io->done += put;
            if (st == ST_PENDING) { write_busy[wd] = true; if (put) again = true; continue; }
            io_finish(io, st, st == ST_SUCCESS ? io->len : io->done, d);
            again = true;
        }
    }
}

static PipeIo *io_new(PipeEnd *e, int op, UINT64 buf, UINT32 len, UINT64 iosb, UmObject *ev)
{
    PipeIo *io = kzalloc(sizeof(*io));
    if (!io) return NULL;
    io->end = e;
    io->proc = UmCurrent();
    io->op = op;
    io->buf = buf;
    io->len = len;
    io->iosb = iosb;
    io->event = ev ? um_ob_ref(ev) : NULL;
    PipeIo **pp = &g_io;
    while (*pp) pp = &(*pp)->next;
    *pp = io;
    return io;
}

/* The event of an overlapped request: reset now, set when it is done */
static UmObject *take_event(UINT64 h)
{
    if (!h) return NULL;
    UmObject *ev = um_handle_object(UmCurrent(), h, UO_EVENT);
    if (ev) { IrqState s = ob_lock(); ev->signaled = false; ob_unlock(s); }
    return ev;
}

static void finish_sync(UmObject *ev, UINT32 st)
{
    if (ev && st != ST_PENDING) ev_signal(ev);
    if (ev) um_ob_unref(ev);
}

/* -----------------------------------------------------------------------
 * Ends
 * ----------------------------------------------------------------------- */
static void pipe_free(UmPipe *p)
{
    for (UmPipe **pp = &g_pipes; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    kfree(p->r[0].buf);
    kfree(p->r[1].buf);
    kfree(p);
}

static void end_destroy(UmObject *o)
{
    PipeEnd *e = (PipeEnd *)o;
    UmPipe *p = e->pipe;
    Defer d = { .n = 0 };
    um_lock(&g_pl);
    for (PipeIo *io = g_io, *next; io; io = next) {           /* its own requests end */
        next = io->next;
        if (io->end == e) io_finish(io, ST_CANCELLED, io->op == IO_WRITE ? io->done : 0, &d);
    }
    if (e->side == 0) p->srv_ends--;
    else {
        p->cli_ends--;
        if (e->gen == p->gen && p->state == P_CONNECTED) p->state = P_CLOSING;
    }
    service(p, &d);                                           /* the other side sees it */
    if (!p->srv_ends && !p->cli_ends) pipe_free(p);
    um_unlock(&g_pl);
    defer_run(&d);
}

static PipeEnd *end_new(UmPipe *p, int side)
{
    PipeEnd *e = kzalloc(sizeof(*e));
    if (!e) return NULL;
    e->ob.type = UO_PIPE;
    e->ob.refs = 1;
    e->ob.signaled = true;                  /* waiting on a pipe handle returns at once */
    e->ob.destroy = end_destroy;
    e->pipe = p;
    e->side = side;
    if (side == 0) p->srv_ends++; else p->cli_ends++;
    return e;
}

static void lower(char *d, const char *s, int cap)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    d[i] = 0;
}

/* "\Device\NamedPipe\NAME" (any case) -> NAME, or NULL */
const char *um_pipe_name(const char *path)
{
    static const char pre[] = "\\device\\namedpipe\\";
    for (int i = 0; pre[i]; i++) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != pre[i]) return NULL;
    }
    return path + sizeof(pre) - 1;
}

static UmPipe *find_listening(const char *name, bool *exists)
{
    *exists = false;
    for (UmPipe *p = g_pipes; p; p = p->next) {
        if (!p->srv_ends || strcmp(p->name, name)) continue;
        *exists = true;
        if (p->state == P_LISTENING) return p;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Creating and opening
 * ----------------------------------------------------------------------- */
UINT32 um_pipe_create(const char *path, UINT32 access, UINT32 disposition, UINT32 options,
                      UINT32 type, UINT32 read_mode, UINT32 completion, UINT32 max_inst,
                      UINT32 in_quota, UINT32 out_quota, UmObject **out, bool *rd, bool *wr)
{
    const char *n = um_pipe_name(path);
    if (!n || !*n || strlen(n) >= sizeof(((UmPipe *)0)->name)) return ST_OBJECT_NAME_INVALID;
    char name[96];
    lower(name, n, sizeof(name));
    um_lock(&g_pl);
    int live = 0;
    UINT32 limit = 0;
    for (UmPipe *p = g_pipes; p; p = p->next)
        if (p->srv_ends && !strcmp(p->name, name)) { live++; limit = p->max_inst; }
    if (live && (disposition == 2 || (UINT32)live >= limit)) {   /* FILE_CREATE: first instance only */
        um_unlock(&g_pl);
        return disposition == 2 ? ST_ACCESS_DENIED : ST_INSTANCE_NOT_AVAIL;
    }
    UmPipe *p = kzalloc(sizeof(*p));
    if (!p || !ring_init(&p->r[0], in_quota) || !ring_init(&p->r[1], out_quota)) {
        if (p) { kfree(p->r[0].buf); kfree(p); }
        um_unlock(&g_pl);
        return ST_NO_MEMORY;
    }
    strcpy(p->name, name);
    p->max_inst = live ? limit : (max_inst ? max_inst : 1);
    p->msg_type = type & 1;
    p->state = P_LISTENING;
    p->in_quota = in_quota;
    p->out_quota = out_quota;
    PipeEnd *e = end_new(p, 0);
    if (!e) { kfree(p->r[0].buf); kfree(p->r[1].buf); kfree(p); um_unlock(&g_pl); return ST_NO_MEMORY; }
    e->msg_read = (read_mode & 1) && p->msg_type;
    e->nowait = completion & 1;
    e->async = !(options & 0x30);                           /* no FILE_SYNCHRONOUS_IO_* */
    e->can_read = access & (0x80000000u | 0x10000000u | 0x0001u);    /* inbound */
    e->can_write = access & (0x40000000u | 0x10000000u | 0x0002u);   /* outbound */
    p->next = g_pipes;
    g_pipes = p;
    um_unlock(&g_pl);
    *rd = e->can_read;
    *wr = e->can_write;
    *out = &e->ob;
    return ST_SUCCESS;
}

UINT32 um_pipe_open(const char *path, UINT32 access, UINT32 options, UmObject **out, bool *rd, bool *wr)
{
    const char *n = um_pipe_name(path);
    if (!n) return ST_OBJECT_NAME_INVALID;
    char name[96];
    lower(name, n, sizeof(name));
    Defer d = { .n = 0 };
    um_lock(&g_pl);
    bool exists;
    UmPipe *p = find_listening(name, &exists);
    if (!p) { um_unlock(&g_pl); return exists ? ST_PIPE_NOT_AVAILABLE : ST_OBJECT_NAME_NOT_FOUND; }
    PipeEnd *e = end_new(p, 1);
    if (!e) { um_unlock(&g_pl); return ST_NO_MEMORY; }
    p->gen++;
    e->gen = p->gen;
    p->state = P_CONNECTED;
    UmProcess *me = UmCurrent();
    p->client_pid = me ? me->pid : 0;
    ring_reset(&p->r[0]);
    ring_reset(&p->r[1]);
    e->async = !(options & 0x30);
    e->can_read = access & (0x80000000u | 0x10000000u | 0x0001u);
    e->can_write = access & (0x40000000u | 0x10000000u | 0x0002u | 0x0004u);
    service(p, &d);                                         /* ConnectNamedPipe returns */
    um_unlock(&g_pl);
    defer_run(&d);
    *rd = e->can_read;
    *wr = e->can_write;
    *out = &e->ob;
    return ST_SUCCESS;
}

/* A kernel-made anonymous pipe (the Terminal): read and write ends */
bool um_pipe_anonymous(UmObject **rd_end, UmObject **wr_end)
{
    static UINT32 seq;
    char path[64];
    ksnprintf(path, sizeof(path), "\\Device\\NamedPipe\\kernel.anon.%u", __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
    bool r, w;
    if (um_pipe_create(path, 0x80000000u, 2, 0x20, 0, 0, 0, 1, 0, 0, rd_end, &r, &w)) return false;
    if (um_pipe_open(path, 0x40000000u, 0x20, wr_end, &r, &w)) { um_ob_unref(*rd_end); return false; }
    return true;
}

/* -----------------------------------------------------------------------
 * Reading and writing (NtReadFile / NtWriteFile on a pipe handle)
 * ----------------------------------------------------------------------- */
UINT32 um_pipe_read(UmObject *o, UINT64 event, UINT64 iosb, UINT64 buf, UINT32 len, UINT64 *info)
{
    PipeEnd *e = (PipeEnd *)o;
    *info = 0;
    if (!e->can_read) return ST_ACCESS_DENIED;
    UmObject *ev = take_event(event);
    UINT32 want = len < CHUNK ? len : CHUNK, got = 0, st;
    UINT8 small[512], *tmp = want <= sizeof(small) ? small : kmalloc(want);
    if (!tmp) { finish_sync(ev, ST_NO_MEMORY); return ST_NO_MEMORY; }
    for (;;) {
        Defer d = { .n = 0 };
        um_lock(&g_pl);
        bool queued = false;
        for (PipeIo *io = g_io; io; io = io->next)          /* keep the order of reads */
            if (io->op == IO_READ && io->end->pipe == e->pipe && io->end->side == e->side) queued = true;
        st = queued ? ST_PENDING : try_read(e, tmp, want, &got);
        if (st != ST_PENDING) service(e->pipe, &d);
        else if (e->async && !e->nowait) {
            if (!io_new(e, IO_READ, buf, len, iosb, ev)) st = ST_NO_MEMORY;
        }
        um_unlock(&g_pl);
        defer_run(&d);
        if (st != ST_PENDING || e->async) break;
        if (e->nowait) { st = ST_PIPE_EMPTY; break; }
        if (um_stopping()) { st = ST_CANCELLED; break; }
        sched_wait();
    }
    if (e->nowait && st == ST_PENDING) st = ST_PIPE_EMPTY;
    if (got && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)buf, tmp, got))) st = ST_ACCESS_VIOLATION;
    if (tmp != small) kfree(tmp);
    *info = got;
    finish_sync(ev, st);
    return st;
}

UINT32 um_pipe_write(UmObject *o, UINT64 event, UINT64 iosb, UINT64 buf, UINT32 len, UINT64 *info)
{
    PipeEnd *e = (PipeEnd *)o;
    *info = 0;
    if (!e->can_write) return ST_ACCESS_DENIED;
    if (len > PIPE_MAX_BUF) len = PIPE_MAX_BUF;
    UmObject *ev = take_event(event);
    UINT8 *data = kmalloc(len ? len : 1);
    if (!data) { finish_sync(ev, ST_NO_MEMORY); return ST_NO_MEMORY; }
    if (len && !NT_SUCCESS(CopyFromUser(data, (const void *)(uintptr_t)buf, len))) {
        kfree(data);
        finish_sync(ev, ST_ACCESS_VIOLATION);
        return ST_ACCESS_VIOLATION;
    }
    UINT32 done = 0, st;
    for (;;) {
        Defer d = { .n = 0 };
        um_lock(&g_pl);
        bool queued = false;
        for (PipeIo *io = g_io; io; io = io->next)
            if (io->op == IO_WRITE && io->end->pipe == e->pipe && io->end->side == e->side) queued = true;
        UINT32 put = 0;
        st = queued ? ST_PENDING : try_write(e, data + done, len - done, true, e->async, &put);
        done += put;
        if (put) service(e->pipe, &d);
        if (st == ST_PENDING && e->async && !e->nowait) {
            PipeIo *io = io_new(e, IO_WRITE, buf, len, iosb, ev);
            if (io) { io->data = data; io->done = done; data = NULL; }
            else st = ST_NO_MEMORY;
        }
        um_unlock(&g_pl);
        defer_run(&d);
        if (st != ST_PENDING || e->async) break;
        if (e->nowait) { st = ST_SUCCESS; break; }          /* PIPE_NOWAIT: what fitted */
        if (um_stopping()) { st = ST_CANCELLED; break; }
        sched_wait();
    }
    kfree(data);
    *info = st == ST_PENDING ? 0 : done;
    finish_sync(ev, st);
    return st;
}

/* -----------------------------------------------------------------------
 * NtFsControlFile
 * ----------------------------------------------------------------------- */
#define FSCTL_PIPE_DISCONNECT  0x110004u
#define FSCTL_PIPE_LISTEN      0x110008u
#define FSCTL_PIPE_PEEK        0x11400Cu
#define FSCTL_PIPE_WAIT        0x110018u
#define FSCTL_PIPE_TRANSCEIVE  0x11C017u

static UINT32 listen(PipeEnd *e, UINT64 event, UINT64 iosb)
{
    if (e->side != 0) return ST_INVALID_PARAMETER;
    UmObject *ev = take_event(event);
    UINT32 st;
    for (;;) {
        um_lock(&g_pl);
        UmPipe *p = e->pipe;
        if (p->state == P_DISCONNECTED) p->state = P_LISTENING;
        st = p->state == P_CONNECTED ? ST_PIPE_CONNECTED :
             p->state == P_CLOSING ? ST_PIPE_CLOSING : ST_PENDING;
        if (st == ST_PENDING && e->async && !e->nowait)
            if (!io_new(e, IO_LISTEN, 0, 0, iosb, ev)) st = ST_NO_MEMORY;
        um_unlock(&g_pl);
        if (st != ST_PENDING || e->async) break;
        if (e->nowait) { st = ST_PIPE_LISTENING; break; }
        if (um_stopping()) { st = ST_CANCELLED; break; }
        sched_wait();
        um_lock(&g_pl);
        bool now = e->pipe->state == P_CONNECTED || e->pipe->state == P_CLOSING;
        um_unlock(&g_pl);
        if (now) { st = ST_SUCCESS; break; }
    }
    /* an overlapped listen that found a client already there does not
     * set the event (as on Windows: ERROR_PIPE_CONNECTED) */
    if (ev && st != ST_PENDING && st != ST_PIPE_CONNECTED) ev_signal(ev);
    if (ev) um_ob_unref(ev);
    return st;
}

static UINT32 disconnect(PipeEnd *e)
{
    if (e->side != 0) return ST_INVALID_PARAMETER;
    Defer d = { .n = 0 };
    um_lock(&g_pl);
    UmPipe *p = e->pipe;
    UINT32 st = p->state == P_LISTENING ? ST_PIPE_LISTENING : ST_SUCCESS;
    if (st == ST_SUCCESS) {
        p->state = P_DISCONNECTED;
        p->gen++;                                           /* the client is cut off */
        ring_reset(&p->r[0]);
        ring_reset(&p->r[1]);
        for (PipeIo *io = g_io, *next; io; io = next) {
            next = io->next;
            if (io->end->pipe == p && io->op != IO_LISTEN)
                io_finish(io, ST_PIPE_DISCONNECTED, io->op == IO_WRITE ? io->done : 0, &d);
        }
    }
    um_unlock(&g_pl);
    defer_run(&d);
    return st;
}

static UINT32 peek(PipeEnd *e, UINT64 out, UINT32 out_len, UINT64 *info)
{
    if (out_len < 16) return ST_BUFFER_TOO_SMALL;
    UINT32 cap = out_len - 16 < CHUNK ? out_len - 16 : CHUNK;
    UINT8 *tmp = kmalloc(16 + cap);
    if (!tmp) return ST_NO_MEMORY;
    um_lock(&g_pl);
    UmPipe *p = e->pipe;
    Ring *r = &p->r[e->side == 0 ? 0 : 1];
    UINT32 used = ring_used(r), msgs = p->msg_type ? ring_msgs(r) : 0;
    UINT32 mlen = msgs ? r->msg[r->mtail % PIPE_MAX_MSGS] : 0;
    UINT32 avail = e->msg_read && msgs ? mlen : used;
    UINT32 n = avail < cap ? avail : cap;
    ring_peek(r, tmp + 16, n);
    UINT32 state = e->side == 1 && !connected_client(e) ? P_DISCONNECTED : (UINT32)p->state;
    bool broken = !used && ((e->side == 0 && p->state == P_CLOSING) ||
                            (e->side == 1 && (!p->srv_ends || !connected_client(e))));
    um_unlock(&g_pl);
    if (broken) { kfree(tmp); return e->side == 1 && p->srv_ends ? ST_PIPE_DISCONNECTED : ST_PIPE_BROKEN; }
    if (e->side == 0 && p->state == P_LISTENING) { kfree(tmp); return ST_PIPE_LISTENING; }
    UINT32 hdr[4] = { state, used, msgs, mlen };
    memcpy(tmp, hdr, 16);
    UINT32 st = NT_SUCCESS(CopyToUser((void *)(uintptr_t)out, tmp, 16 + n)) ? (n < avail ? ST_BUFFER_OVERFLOW : ST_SUCCESS)
                                                                             : ST_ACCESS_VIOLATION;
    kfree(tmp);
    *info = 16 + n;
    return st;
}

/* FILE_PIPE_WAIT_FOR_BUFFER: { LARGE_INTEGER Timeout; ULONG NameLength;
 * BOOLEAN TimeoutSpecified; WCHAR Name[] } */
static UINT32 wait_pipe(UINT64 in, UINT32 in_len)
{
    UINT8 b[14 + 2 * 95];
    if (in_len < 14 || in_len > sizeof(b) || !NT_SUCCESS(CopyFromUser(b, (const void *)(uintptr_t)in, in_len)))
        return ST_INVALID_PARAMETER;
    INT64 timeout;
    UINT32 nl;
    memcpy(&timeout, b, 8);
    memcpy(&nl, b + 8, 4);
    bool has_timeout = b[12];
    if (14 + nl > in_len || nl / 2 >= 95) return ST_INVALID_PARAMETER;
    char name[96];
    int k = 0;
    for (UINT32 i = 0; i < nl / 2; i++) {
        UINT16 c;
        memcpy(&c, b + 14 + 2 * i, 2);
        name[k++] = (char)(c < 0x80 ? c : '?');
    }
    name[k] = 0;
    const char *n = name;
    if (n[0] == '\\') n++;
    char key[96];
    lower(key, n, sizeof(key));
    /* relative (negative) timeouts in 100 ns; the default is 50 ms */
    UINT64 ticks = has_timeout ? (timeout < 0 ? (UINT64)(-timeout) / 100000 : 0) : 5;
    UINT64 until = sched_ticks() + ticks;
    for (;;) {
        bool exists;
        um_lock(&g_pl);
        UmPipe *p = find_listening(key, &exists);
        um_unlock(&g_pl);
        if (p) return ST_SUCCESS;
        if (!exists) return ST_OBJECT_NAME_NOT_FOUND;
        if (has_timeout && timeout == (INT64)(UINT64_C(1) << 63)) { /* NMPWAIT_WAIT_FOREVER */ }
        else if (sched_ticks() >= until) return ST_IO_TIMEOUT;
        if (um_stopping()) return ST_CANCELLED;
        sched_sleep_tick();
    }
}

UINT32 um_pipe_fsctl(UmObject *o, UINT64 event, UINT64 iosb, UINT32 code,
                     UINT64 in, UINT32 in_len, UINT64 out, UINT32 out_len, UINT64 *info)
{
    PipeEnd *e = (PipeEnd *)o;
    *info = 0;
    if (code == FSCTL_PIPE_WAIT) return wait_pipe(in, in_len);
    if (!e) return ST_INVALID_HANDLE;
    switch (code) {
    case FSCTL_PIPE_LISTEN:     return listen(e, event, iosb);
    case FSCTL_PIPE_DISCONNECT: return disconnect(e);
    case FSCTL_PIPE_PEEK:       return peek(e, out, out_len, info);
    case FSCTL_PIPE_TRANSCEIVE: {
        if (!e->pipe->msg_type || !e->msg_read) return 0xC00000ADu;   /* STATUS_INVALID_PIPE_STATE */
        bool async = e->async;
        e->async = false;                                   /* (done synchronously) */
        UINT64 n;
        UINT32 st = um_pipe_write(o, 0, 0, in, in_len, &n);
        if (NT_SUCCESS(st)) st = um_pipe_read(o, 0, 0, out, out_len, info);
        e->async = async;
        if (event) { UmObject *ev = um_handle_object(UmCurrent(), event, UO_EVENT); ev_signal(ev); if (ev) um_ob_unref(ev); }
        return st;
    }
    }
    return ST_INVALID_DEVICE_REQ;
}

/* -----------------------------------------------------------------------
 * Information, modes, cancelling
 * ----------------------------------------------------------------------- */
bool um_pipe_is_async(UmObject *o) { return ((PipeEnd *)o)->async; }

/* FilePipeInformation (23), FilePipeLocalInformation (24),
 * FileStandardInformation (5), FileNameInformation (9) */
UINT32 um_pipe_query(UmObject *o, UINT32 cls, UINT8 *buf, UINT32 cap, UINT32 *len)
{
    PipeEnd *e = (PipeEnd *)o;
    um_lock(&g_pl);
    UmPipe *p = e->pipe;
    Ring *rin = &p->r[e->side == 0 ? 0 : 1], *rout = &p->r[e->side == 0 ? 1 : 0];
    UINT32 st = ST_SUCCESS;
    memset(buf, 0, cap);
    if (cls == 23 && cap >= 8) {
        UINT32 v[2] = { e->msg_read, e->nowait };
        memcpy(buf, v, 8);
        *len = 8;
    } else if (cls == 24 && cap >= 40) {
        int inst = 0;
        for (UmPipe *q = g_pipes; q; q = q->next) if (q->srv_ends && !strcmp(q->name, p->name)) inst++;
        UINT32 cfg = e->can_read && e->can_write ? 2 : 0;   /* FILE_PIPE_FULL_DUPLEX */
        if (!cfg) cfg = (e->side == 0) == e->can_read ? 0 : 1;   /* inbound / outbound (server view) */
        UINT32 v[10] = { p->msg_type, cfg, p->max_inst, (UINT32)inst, rin->cap, ring_used(rin),
                         rout->cap, rout->cap - ring_used(rout),
                         e->side == 1 && !connected_client(e) ? P_DISCONNECTED : (UINT32)p->state, (UINT32)e->side };
        memcpy(buf, v, 40);
        *len = 40;
    } else if (cls == 5 && cap >= 24) {                     /* AllocationSize, EndOfFile, links... */
        UINT64 v[2] = { rin->cap, ring_used(rin) };
        memcpy(buf, v, 16);
        UINT32 links = 1;
        memcpy(buf + 16, &links, 4);
        *len = 24;
    } else if (cls == 9 && cap >= 4) {                      /* FileNameInformation: "\NAME" */
        UINT32 n = (UINT32)strlen(p->name) + 1;
        UINT32 bytes = 2 * n, room = (cap - 4) / 2;
        memcpy(buf, &bytes, 4);
        for (UINT32 i = 0; i < n && i < room; i++) {
            UINT16 c = i ? (UINT8)p->name[i - 1] : '\\';
            memcpy(buf + 4 + 2 * i, &c, 2);
        }
        *len = 4 + 2 * (n < room ? n : room);
        if (n > room) st = ST_BUFFER_OVERFLOW;
    } else st = cap < 8 ? 0xC0000004u /* INFO_LENGTH_MISMATCH */ : 0xC0000003u /* INVALID_INFO_CLASS */;
    um_unlock(&g_pl);
    return st;
}

/* FilePipeInformation: { ReadMode, CompletionMode } */
UINT32 um_pipe_set_mode(UmObject *o, UINT32 read_mode, UINT32 completion)
{
    PipeEnd *e = (PipeEnd *)o;
    if ((read_mode & 1) && !e->pipe->msg_type) return ST_INVALID_PARAMETER;
    e->msg_read = read_mode & 1;
    e->nowait = completion & 1;
    return ST_SUCCESS;
}

/* NtCancelIoFile(Ex): the calling process's requests on this end
 * (@iosb: only that one) */
UINT32 um_pipe_cancel(UmObject *o, UINT64 iosb)
{
    PipeEnd *e = (PipeEnd *)o;
    UmProcess *me = UmCurrent();
    Defer d = { .n = 0 };
    int n = 0;
    um_lock(&g_pl);
    for (PipeIo *io = g_io, *next; io; io = next) {
        next = io->next;
        if (io->end != e || io->proc != me) continue;
        if (iosb && (io->iosb & ~(UINT64_C(1) << 63)) != (iosb & ~(UINT64_C(1) << 63))) continue;
        io_finish(io, ST_CANCELLED, io->op == IO_WRITE ? io->done : 0, &d);
        n++;
    }
    um_unlock(&g_pl);
    defer_run(&d);
    return n || !iosb ? ST_SUCCESS : ST_NOT_FOUND;
}

/* A process is going away: forget its pending requests (its memory goes) */
void um_pipe_process_gone(UmProcess *p)
{
    Defer d = { .n = 0 };
    um_lock(&g_pl);
    for (PipeIo *io = g_io, *next; io; io = next) {
        next = io->next;
        if (io->proc != p) continue;
        io_unlink(io);
        defer_unref(&d, io->event);
        kfree(io->data);
        kfree(io);
    }
    um_unlock(&g_pl);
    defer_run(&d);
}

UINT32 um_pipe_client_pid(UmObject *o) { return ((PipeEnd *)o)->pipe->client_pid; }

/* The name of the pipe an end object belongs to ("" if @o is not a pipe end) */
void um_pipe_end_name(UmObject *o, char *buf, int cap)
{
    buf[0] = 0;
    if (!o || o->type != UO_PIPE || cap < 2) return;
    PipeEnd *e = (PipeEnd *)o;
    if (e->pipe) { strncpy(buf, e->pipe->name, (size_t)cap - 1); buf[cap - 1] = 0; }
}
