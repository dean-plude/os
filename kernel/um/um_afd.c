/*
 * um_afd.c — \Device\Afd helper handles and IOCTL_AFD_POLL
 *
 * Winsock on Windows talks to afd.sys.  NovaOS's ws2_32 has its own
 * services (um_socket.c), but some programs talk to AFD themselves to
 * poll many sockets with one completion port: wepoll (the epoll of
 * Poco's PollSet, libevent and others) opens a helper handle with
 * NtCreateFile("\Device\Afd\Wepoll"), binds it to a completion port and
 * keeps one IOCTL_AFD_POLL pending per socket through
 * NtDeviceIoControlFile; NtCancelIoFileEx withdraws one.
 *
 * A helper handle is a UO_AFD object.  A poll that finds a socket ready
 * finishes at once (STATUS_SUCCESS); one that does not stays pending
 * (STATUS_PENDING) on the object.  Finishing writes the output
 * AFD_POLL_INFO (only the handles with events), the I/O status block and
 * the event, if one was given; and when the handle is bound to a
 * completion port (kernel32 says so, op 12 below) a record { ApcContext,
 * status, information } for the port.  Completion ports live in kernel32,
 * so a kernel32 thread per bound handle waits here (op 13), which is
 * also what looks at the pending polls again whenever the network moves
 * on (or every 100 ms: time-outs, sockets closed meanwhile).
 *
 * Events: AFD_POLL_RECEIVE (data), _SEND (room to send), _DISCONNECT (the
 * peer closed), _ABORT (reset), _LOCAL_CLOSE (the handle polled was
 * closed), _ACCEPT (a listening socket has a connection), _CONNECT_FAIL.
 * The output and the status block are written only by the polling
 * process's own threads, in its own address space.
 */

#include "um_internal.h"
#include "../ke/printf.h"
#include "../ke/probe.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../net/sock.h"
#include "../net/net_internal.h"

#define ST_SUCCESS            0x00000000u
#define ST_PENDING            0x00000103u
#define ST_INVALID_HANDLE     0xC0000008u
#define ST_INVALID_PARAMETER  0xC000000Du
#define ST_NO_MEMORY          0xC0000017u
#define ST_CANCELLED          0xC0000120u
#define ST_NOT_FOUND          0xC0000225u

#define IOCTL_AFD_POLL        0x00012024u

#define AFD_POLL_RECEIVE       0x0001u
#define AFD_POLL_SEND          0x0004u
#define AFD_POLL_DISCONNECT    0x0008u
#define AFD_POLL_ABORT         0x0010u
#define AFD_POLL_LOCAL_CLOSE   0x0020u
#define AFD_POLL_ACCEPT        0x0080u
#define AFD_POLL_CONNECT_FAIL  0x0100u

#define AFD_MAX_HANDLES        1024

typedef struct {
    UINT64    hv;                 /* the handle as the program gave it */
    UmObject *sock;               /* its socket (referenced) */
    UINT32    events, got;        /* asked for, found */
} AfdHandle;

typedef struct AfdPoll {
    struct AfdPoll *next;
    UINT64    iosb, ctx, out;     /* (@iosb: bit 63 = the 32-bit layout) */
    UINT32    out_len;
    UmObject *ev;                 /* the event to set (referenced), or NULL */
    UINT64    deadline;           /* system time (100 ns), 0: none */
    UINT8     timeout[8];         /* as given, for the output */
    UINT32    exclusive;
    UINT32    status;             /* when finished */
    UINT32    n;
    AfdHandle h[];
} AfdPoll;

typedef struct AfdDone {
    struct AfdDone *next;
    UINT64 rec[3];                /* ApcContext, status, information */
} AfdDone;

typedef struct {
    UmObject  ob;
    UmLock    lock;               /* pending, done, bound */
    UmProcess *proc;              /* the process that opened it (whose memory the polls name) */
    bool      wow;                /* a 32-bit program: 12-byte handle entries */
    bool      bound;              /* to a completion port: keep records for it */
    UINT32    serial;
    AfdPoll  *pending;
    AfdDone  *done, *done_tail;
} Afd;

static volatile UINT32 g_serial;

static void poll_free(AfdPoll *q)
{
    for (UINT32 i = 0; i < q->n; i++) um_ob_unref(q->h[i].sock);
    um_ob_unref(q->ev);
    kfree(q);
}

static void afd_destroy(UmObject *o)
{
    Afd *a = (Afd *)o;
    while (a->pending) { AfdPoll *q = a->pending; a->pending = q->next; poll_free(q); }
    while (a->done) { AfdDone *d = a->done; a->done = d->next; kfree(d); }
}

/* "\Device\Afd" or "\Device\Afd\ANYTHING" (any case) */
bool um_afd_name(const char *path)
{
    static const char dev[] = "\\device\\afd";
    int i = 0;
    for (; dev[i]; i++) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != dev[i]) return false;
    }
    return !path[i] || path[i] == '\\';
}

/* NtCreateFile on \Device\Afd...: a new helper object */
UmObject *um_afd_open(void)
{
    Afd *a = kzalloc(sizeof(*a));
    if (!a) return NULL;
    a->ob.type = UO_AFD;
    a->ob.refs = 1;
    a->ob.signaled = true;                       /* (waiting on it returns at once, as on a file) */
    a->ob.destroy = afd_destroy;
    a->proc = UmCurrent();
    a->wow = a->proc && a->proc->wow;
    a->serial = __atomic_add_fetch(&g_serial, 1, __ATOMIC_RELAXED) & 0x3FFFFFFF;
    if (!a->serial) a->serial = 1;
    return &a->ob;
}

/* The events @h shows now */
static UINT32 sock_events(UmProcess *p, AfdHandle *h)
{
    UmObject *o = um_handle_object(p, h->hv, UO_SOCKET);
    if (o) um_ob_unref(o);
    if (o != h->sock) return AFD_POLL_LOCAL_CLOSE;          /* closed (or the value reused) */
    int s = h->sock->sock;
    bool rd, wr, er;
    NetSockPoll(s, &rd, &wr, &er);
    UINT32 ev = 0;
    if (rd) {
        if (NetSockListening(s)) ev |= AFD_POLL_ACCEPT;
        else {
            bool closed = false;
            int n = NetSockPeek(s, NULL, 0, &closed);
            if (n > 0) ev |= AFD_POLL_RECEIVE;
            if (closed) ev |= AFD_POLL_DISCONNECT;
            if (n <= 0 && !closed) ev |= AFD_POLL_RECEIVE;   /* (a datagram, an error to read) */
        }
    }
    if (wr) ev |= AFD_POLL_SEND;
    if (er) {
        NetSockAddr peer;
        ev |= NetSockPeerName(s, &peer) < 0 ? AFD_POLL_CONNECT_FAIL : AFD_POLL_ABORT;
    }
    return ev;
}

/* Under a->lock: whether @q is due (events found, timed out); fills got */
static bool poll_due(Afd *a, AfdPoll *q, UINT64 now)
{
    bool any = false;
    for (UINT32 i = 0; i < q->n; i++) {
        q->h[i].got = sock_events(a->proc, &q->h[i]) & q->h[i].events;
        if (q->h[i].got) any = true;
    }
    return any || (q->deadline && now >= q->deadline);
}

static void put_iosb(UINT64 iosb, UINT32 st, UINT64 info)
{
    if (!iosb) return;
    if (iosb >> 63) {
        UINT32 b[2] = { st, (UINT32)info };
        CopyToUser((void *)(uintptr_t)(iosb & ~(UINT64_C(1) << 63)), b, sizeof(b));
    } else {
        UINT64 b[2] = { st, info };
        CopyToUser((void *)(uintptr_t)iosb, b, sizeof(b));
    }
}

/* A finished poll, taken off the list: its output, status block, event
 * and port record (in the polling process, without a->lock) */
static void poll_finish(Afd *a, AfdPoll *q)
{
    UINT64 info = 0;
    if (q->status == ST_SUCCESS) {
        UINT32 es = a->wow ? 12 : 16, k = 0;
        UINT8 *buf = kzalloc(16 + (UINT64)q->n * es);
        if (buf) {
            for (UINT32 i = 0; i < q->n; i++) {
                if (!q->h[i].got) continue;
                UINT8 *e = buf + 16 + (UINT64)k * es;
                if (a->wow) { UINT32 hv = (UINT32)q->h[i].hv; memcpy(e, &hv, 4); memcpy(e + 4, &q->h[i].got, 4); }
                else { memcpy(e, &q->h[i].hv, 8); memcpy(e + 8, &q->h[i].got, 4); }
                k++;
            }
            memcpy(buf, q->timeout, 8);
            memcpy(buf + 8, &k, 4);
            memcpy(buf + 12, &q->exclusive, 4);
            info = 16 + (UINT64)k * es;
            if (info > q->out_len || !NT_SUCCESS(CopyToUser((void *)(uintptr_t)q->out, buf, (UINT32)info))) info = 0;
            kfree(buf);
        }
    }
    put_iosb(q->iosb, q->status, info);
    if (q->ev) {
        IrqState s = ob_lock();
        q->ev->signaled = true;
        um_ob_wake(q->ev);
        ob_unlock(s);
    }
    AfdDone *d = NULL;
    um_lock(&a->lock);
    bool bound = a->bound;
    um_unlock(&a->lock);
    if (bound && (d = kzalloc(sizeof(*d)))) {
        d->rec[0] = q->ctx;
        d->rec[1] = q->status;
        d->rec[2] = info;
        um_lock(&a->lock);
        if (a->done_tail) a->done_tail->next = d; else a->done = d;
        a->done_tail = d;
        um_unlock(&a->lock);
    }
    poll_free(q);
}

/* Look at every pending poll; finish the ones that are due */
static void afd_service(Afd *a)
{
    AfdPoll *fin = NULL, **tail = &fin;
    UINT64 now = um_now_100ns();
    um_lock(&a->lock);
    for (AfdPoll **pp = &a->pending; *pp; ) {
        AfdPoll *q = *pp;
        if (!poll_due(a, q, now)) { pp = &q->next; continue; }
        *pp = q->next;
        q->next = NULL;
        q->status = ST_SUCCESS;
        *tail = q;
        tail = &q->next;
    }
    um_unlock(&a->lock);
    while (fin) { AfdPoll *q = fin; fin = q->next; poll_finish(a, q); }
}

/* NtDeviceIoControlFile on a helper handle */
UINT32 um_afd_ioctl(UmObject *o, UINT64 event, UINT64 ctx, UINT64 iosb, UINT32 code,
                    UINT64 in, UINT32 in_len, UINT64 out, UINT32 out_len)
{
    Afd *a = (Afd *)o;
    UmProcess *p = UmCurrent();
    if (p != a->proc) return ST_INVALID_HANDLE;
    if (code != IOCTL_AFD_POLL) return 0xC0000010u;           /* STATUS_INVALID_DEVICE_REQUEST */
    UINT32 es = a->wow ? 12 : 16;
    UINT8 hdr[16];
    if (in_len < 16 || !NT_SUCCESS(CopyFromUser(hdr, (const void *)(uintptr_t)in, 16))) return ST_INVALID_PARAMETER;
    UINT32 n;
    memcpy(&n, hdr + 8, 4);
    if (!n || n > AFD_MAX_HANDLES || in_len < 16 + n * es || out_len < 16 + n * es) return ST_INVALID_PARAMETER;
    AfdPoll *q = kzalloc(sizeof(*q) + n * sizeof(AfdHandle));
    UINT8 *raw = q ? kmalloc(n * es) : NULL;
    if (!raw) { kfree(q); return ST_NO_MEMORY; }
    if (!NT_SUCCESS(CopyFromUser(raw, (const void *)(uintptr_t)(in + 16), n * es))) {
        kfree(raw); kfree(q);
        return ST_INVALID_PARAMETER;
    }
    q->iosb = iosb; q->ctx = ctx; q->out = out; q->out_len = out_len;
    memcpy(q->timeout, hdr, 8);
    memcpy(&q->exclusive, hdr + 12, 4);
    UINT32 st = ST_SUCCESS;
    for (UINT32 i = 0; i < n; i++) {
        UINT8 *e = raw + (UINT64)i * es;
        AfdHandle *h = &q->h[i];
        if (a->wow) { INT32 hv; memcpy(&hv, e, 4); h->hv = (UINT64)(INT64)hv; memcpy(&h->events, e + 4, 4); }
        else { memcpy(&h->hv, e, 8); memcpy(&h->events, e + 8, 4); }
        h->sock = um_handle_object(p, h->hv, UO_SOCKET);
        q->n = i + 1;
        if (!h->sock) { st = ST_INVALID_HANDLE; break; }
    }
    kfree(raw);
    if (st == ST_SUCCESS && event && !(q->ev = um_handle_object(p, event, UO_EVENT))) st = ST_INVALID_HANDLE;
    if (st) { poll_free(q); return st; }
    INT64 t;
    memcpy(&t, q->timeout, 8);
    UINT64 now = um_now_100ns();
    if (t == 0) q->deadline = 1;                                /* (no wait: due now) */
    else if (t < 0) q->deadline = now + (UINT64)(-t);           /* relative */
    else if (t != 0x7FFFFFFFFFFFFFFFLL) q->deadline = (UINT64)t;          /* absolute */
    if (q->ev) {                                                /* (an event is reset when the request starts) */
        IrqState s = ob_lock();
        q->ev->signaled = false;
        ob_unlock(s);
    }
    um_lock(&a->lock);
    bool due = poll_due(a, q, now);
    if (!due) { q->next = a->pending; a->pending = q; }
    um_unlock(&a->lock);
    if (due) {
        q->status = ST_SUCCESS;
        poll_finish(a, q);
        net_wake();                                             /* (the port's thread has a record) */
        return ST_SUCCESS;
    }
    put_iosb(iosb, ST_PENDING, 0);
    return ST_PENDING;
}

/* NtCancelIoFile(Ex): this process's polls on the handle (@iosb: only that one) */
UINT32 um_afd_cancel(UmObject *o, UINT64 iosb)
{
    Afd *a = (Afd *)o;
    if (UmCurrent() != a->proc) return iosb ? ST_NOT_FOUND : ST_SUCCESS;
    AfdPoll *fin = NULL;
    UINT64 want = iosb & ~(UINT64_C(1) << 63);
    um_lock(&a->lock);
    for (AfdPoll **pp = &a->pending; *pp; ) {
        AfdPoll *q = *pp;
        if (iosb && (q->iosb & ~(UINT64_C(1) << 63)) != want) { pp = &q->next; continue; }
        *pp = q->next;
        q->status = ST_CANCELLED;
        q->next = fin;
        fin = q;
    }
    um_unlock(&a->lock);
    bool any = fin != NULL;
    while (fin) { AfdPoll *q = fin; fin = q->next; poll_finish(a, q); }
    if (any) net_wake();
    return any || !iosb ? ST_SUCCESS : ST_NOT_FOUND;
}

/* NtNovaSockCtl on a helper handle (kernel32's completion-port side):
 *   12: it is bound to a completion port: returns its serial (> 0);
 *   13: wait for the next record for the port (@arg: the serial),
 *       about a second at most: 0 = the record (3 UINT64s) is at @out,
 *       1 = none yet, < 0 = the handle is gone (closed, or another one) */
INT64 um_afd_ctl(UINT64 h, UINT64 op, UINT64 arg, UINT64 out)
{
    UmProcess *p = UmCurrent();
    UINT64 until = um_now_100ns() + 10000000;
    for (;;) {
        UINT32 gen = net_gen();
        UmObject *o = um_handle_object(p, h, UO_AFD);
        if (!o) return -1;
        Afd *a = (Afd *)o;
        if (a->proc != p) { um_ob_unref(o); return -1; }
        if (op == 12) {
            um_lock(&a->lock);
            a->bound = true;
            um_unlock(&a->lock);
            UINT32 s = a->serial;
            um_ob_unref(o);
            return s;
        }
        if (op != 13 || (UINT32)arg != a->serial) { um_ob_unref(o); return -1; }
        afd_service(a);
        um_lock(&a->lock);
        AfdDone *d = a->done;
        if (d) { a->done = d->next; if (!a->done) a->done_tail = NULL; }
        um_unlock(&a->lock);
        um_ob_unref(o);
        if (d) {
            bool ok = NT_SUCCESS(CopyToUser((void *)(uintptr_t)out, d->rec, sizeof(d->rec)));
            kfree(d);
            return ok ? 0 : -1;
        }
        if (um_stopping() || um_now_100ns() >= until) return 1;
        net_wait_ticks(gen, 10);
    }
}
