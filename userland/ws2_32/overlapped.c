/*
 * overlapped.c — overlapped socket requests that wait, and the Microsoft
 * extension functions (AcceptEx, ConnectEx, DisconnectEx, TransmitFile,
 * GetAcceptExSockaddrs, WSARecvMsg, WSASendMsg) WSAIoctl hands out.
 *
 * NovaOS sockets are synchronous underneath.  An overlapped request that
 * can finish at once still does so inside the call (wsa.c).  One that
 * would wait (WSARecv with nothing to read, AcceptEx with no connection
 * yet, ConnectEx still connecting) returns WSA_IO_PENDING and a thread of
 * ws2_32's own waits for the socket, finishes the request and completes
 * the OVERLAPPED as file I/O does (its event, then a packet on the I/O
 * completion port the socket is bound to).  This is what proactor-style
 * programs (Python's asyncio, libuv, Boost.Asio) need: their event loop
 * keeps a receive pending on every socket and must not be stuck in it.
 * CancelIo/CancelIoEx and closesocket end a pending request with
 * STATUS_CANCELLED (ERROR_OPERATION_ABORTED), as on Windows.
 */
#define WS2_EXPORT
#define NOVA_BUILD_KERNEL32
#include <winsock2.h>
#include <winternl.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
WINBASEAPI LPVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
WINBASEAPI HANDLE WINAPI GetProcessHeap(void);
WINBASEAPI BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID);
WINBASEAPI HMODULE WINAPI LoadLibraryW(LPCWSTR);
WINBASEAPI FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR);
__declspec(dllimport) void WINAPI NovaIoComplete(HANDLE h, OVERLAPPED *o, LONG status, DWORD bytes);
__declspec(dllimport) HANDLE WINAPI NovaIoPort(HANDLE h, ULONG_PTR *key);
__declspec(dllimport) void WINAPI NovaIoCompletePort(HANDLE port, ULONG_PTR key, OVERLAPPED *o, LONG status, DWORD bytes);
__declspec(dllimport) void WINAPI NovaSetSocketCancel(BOOL (WINAPI *fn)(HANDLE, LPOVERLAPPED));

#define STATUS_PENDING_            0x103
#define STATUS_CANCELLED_          0xC0000120
#define STATUS_CONNECTION_REFUSED_ 0xC0000236
#define STATUS_CONNECTION_RESET_   0xC000020D
#define STATUS_IO_TIMEOUT_         0xC00000B5
#define STATUS_INVALID_PARAMETER_  0xC000000D
#define WSA_OPERATION_ABORTED_     995

static void set_err(int e) { *(DWORD *)(NtCurrentTebBytes() + TEB_LAST_ERROR) = (DWORD)e; }

/* ---- pending requests ---- */
enum { OP_RECV, OP_RECVFROM, OP_SEND, OP_ACCEPT, OP_CONNECT };

typedef struct Op {
    struct Op *next;
    int kind;
    SOCKET h;                     /* the socket the request was made on (its port, its cancel) */
    SOCKET us;                    /* the caller's socket (closesocket on it cancels) */
    SOCKET s;                     /* once pending, s and listener are our own duplicates: the
                                   * caller may close theirs and the value be reused meanwhile */
    LPWSAOVERLAPPED ov;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr;
    HANDLE port;                  /* the completion port h was bound to when the request began, its key */
    ULONG_PTR key;
    volatile LONG cancelled;
    WSABUF *bufs;                 /* a copy of the caller's WSABUFs (their data lives until completion) */
    DWORD nbufs, flags;
    DWORD bi, sent;               /* OP_SEND: the buffer and offset it got to, the bytes sent */
    ULONG off;
    struct sockaddr *from;        /* OP_RECVFROM */
    int *fromlen;
    SOCKET listener;              /* OP_ACCEPT: s is the accept socket */
    char *abuf;                   /* OP_ACCEPT: the address buffer, its sizes */
    DWORD adata, alocal, aremote;
} Op;

static Op *g_ops;
static volatile LONG g_lock;
static volatile LONG g_hooked;
static volatile LONG g_io;        /* one attempt at a time: two requests on one socket must not both
                                   * see it readable and then both receive (the second would block) */

static void lock(void) { while (InterlockedExchange(&g_lock, 1)) Sleep(0); }
static void unlock(void) { InterlockedExchange(&g_lock, 0); }

static void unlink_op(Op *o)
{
    lock();
    for (Op **pp = &g_ops; *pp; pp = &(*pp)->next)
        if (*pp == o) { *pp = o->next; break; }
    unlock();
}

static BOOL WINAPI sock_cancel(HANDLE h, LPOVERLAPPED ov)
{
    BOOL any = FALSE;
    lock();
    for (Op *o = g_ops; o; o = o->next)
        if ((HANDLE)o->h == h && (!ov || o->ov == ov)) { o->cancelled = 1; any = TRUE; }
    unlock();
    return any;                                   /* the worker sees the flag within 100 ms */
}

/* closesocket: what was pending on @s ends cancelled */
void ws_cancel_socket(SOCKET s)
{
    if (!g_ops) return;
    lock();
    for (Op *o = g_ops; o; o = o->next)
        if (o->h == s || o->us == s) { o->cancelled = 1; }
    unlock();
}

/* the socket's state: [0] readable, [1] writable, [2] error; FALSE: no socket */
static BOOL poll_sock(SOCKET s, BYTE st[3]) { return NtNovaSockCtl((INT_PTR)s, 4, 0, st) == 0; }

static LONG status_of(int wsa)
{
    switch (wsa) {
    case WSAECONNREFUSED: return (LONG)STATUS_CONNECTION_REFUSED_;
    case WSAETIMEDOUT: return (LONG)STATUS_IO_TIMEOUT_;
    case WSAENOTSOCK: return (LONG)STATUS_CANCELLED_;
    case WSAEINVAL: case WSAEFAULT: return (LONG)STATUS_INVALID_PARAMETER_;
    default: return (LONG)STATUS_CONNECTION_RESET_;
    }
}

/* One attempt at @o's request: 1 = done (*status, *bytes), 0 = would wait */
static int attempt(Op *o, LONG *status, DWORD *bytes)
{
    BYTE st[3];
    *status = 0;
    *bytes = 0;
    if (o->cancelled || !poll_sock(o->s, st) || (o->kind == OP_ACCEPT && !poll_sock(o->listener, st))) {
        *status = (LONG)STATUS_CANCELLED_;
        return 1;
    }
    switch (o->kind) {
    case OP_RECV:
    case OP_RECVFROM: {
        if (!st[0] && !st[2]) return 0;
        DWORD total = 0;
        for (DWORD i = 0; i < o->nbufs; i++) {
            if (!o->bufs[i].len) continue;
            if (total && (!poll_sock(o->s, st) || !st[0])) break;    /* no more waiting: don't wait for the next buffer */
            long r = o->kind == OP_RECVFROM
                   ? recvfrom(o->s, o->bufs[i].buf, (int)o->bufs[i].len, 0, o->from, o->fromlen)
                   : recv(o->s, o->bufs[i].buf, (int)o->bufs[i].len, (int)o->flags);
            if (r == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK && !total) return 0;
                if (e != WSAEWOULDBLOCK && !total) *status = status_of(e);
                break;
            }
            total += (DWORD)r;
            if (o->kind == OP_RECVFROM || (ULONG)r < o->bufs[i].len) break;
        }
        *bytes = total;
        return 1;
    }
    case OP_SEND: {                                       /* resumes where the last attempt stopped */
        if (!st[1] && !st[2]) return 0;
        while (o->bi < o->nbufs) {
            WSABUF *b = &o->bufs[o->bi];
            if (o->off >= b->len) { o->bi++; o->off = 0; continue; }
            int r = send(o->s, b->buf + o->off, (int)(b->len - o->off), 0);
            if (r == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) return 0;        /* the rest when there is room */
                if (!o->sent) *status = status_of(e);
                break;
            }
            o->off += (ULONG)r;
            o->sent += (DWORD)r;
        }
        *bytes = o->sent;
        return 1;
    }
    case OP_ACCEPT: {
        BYTE l[3];
        poll_sock(o->listener, l);
        if (!l[0]) return 0;
        BYTE peer[28], local[28];
        memset(local, 0, sizeof(local));
        long r = NtNovaSockCtl((INT_PTR)o->s, 11, (ULONG_PTR)o->listener, peer);
        if (r == -1 /* SOCK_EWOULDBLOCK */) return 0;
        if (r < 0) { *status = (LONG)STATUS_CONNECTION_RESET_; return 1; }
        NtNovaSockCtl((INT_PTR)o->s, 3, 0, local);
        /* GetAcceptExSockaddrs's layout: data, then the local and remote addresses */
        char *p = o->abuf + o->adata;
        memcpy(p, local, o->alocal < sizeof(local) ? o->alocal : sizeof(local));
        memcpy(p + o->alocal, peer, o->aremote < sizeof(peer) ? o->aremote : sizeof(peer));
        if (o->adata) {                                   /* the first data, if any came */
            int n = recv(o->s, o->abuf, (int)o->adata, 0);
            *bytes = n > 0 ? (DWORD)n : 0;
        }
        return 1;
    }
    case OP_CONNECT:
        if (st[2]) { *status = (LONG)STATUS_CONNECTION_REFUSED_; return 1; }
        if (!st[1]) return 0;
        return 1;
    }
    return 1;
}

static DWORD WINAPI worker(LPVOID arg)
{
    Op *o = arg;
    LONG status;
    DWORD bytes;
    for (;;) {
        ULONG gen = (ULONG)NtNovaSockCtl(0, 6, 0, 0);
        while (InterlockedExchange(&g_io, 1)) Sleep(0);
        int done = attempt(o, &status, &bytes);
        InterlockedExchange(&g_io, 0);
        if (done) break;
        NtNovaSockCtl(0, 5, gen, (void *)100);          /* until the network moves on (or 100 ms, to see a cancel) */
    }
    unlink_op(o);
   
    NtClose((HANDLE)o->s);
    if (o->kind == OP_ACCEPT) NtClose((HANDLE)o->listener);
    NovaIoCompletePort(o->port, o->key, o->ov, status, bytes);
    if (o->cr) {
        DWORD err = status == (LONG)STATUS_CANCELLED_ ? WSA_OPERATION_ABORTED_ : status ? (DWORD)WSAECONNRESET : 0;
        o->cr(err, bytes, o->ov, 0);
    }
    HeapFree(GetProcessHeap(), 0, o);
    return 0;
}

/* Leave @o pending: WSA_IO_PENDING, a thread finishes it */
static int go_pending(Op *o)
{
    if (!InterlockedExchange(&g_hooked, 1)) NovaSetSocketCancel(sock_cancel);
    HANDLE d, dl = 0;
    BOOL ok = NT_SUCCESS(NtDuplicateObject(NtCurrentProcess(), (HANDLE)o->s, NtCurrentProcess(), &d, 0, 0, 2));
    if (ok && o->kind == OP_ACCEPT &&
        !NT_SUCCESS(NtDuplicateObject(NtCurrentProcess(), (HANDLE)o->listener, NtCurrentProcess(), &dl, 0, 0, 2))) {
        NtClose(d);
        ok = FALSE;
    }
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, o);
        set_err(WSAENOTSOCK);
        return SOCKET_ERROR;
    }
    o->us = o->s;
    o->s = (SOCKET)d;
    if (dl) o->listener = (SOCKET)dl;
    o->port = NovaIoPort((HANDLE)o->h, &o->key);
    o->ov->Internal = STATUS_PENDING_;
    o->ov->InternalHigh = 0;
    lock();
    o->next = g_ops;
    g_ops = o;
    unlock();
   
    HANDLE t = CreateThread(0, 64 * 1024, worker, o, 0, 0);
    if (!t) {
        unlink_op(o);
        NtClose((HANDLE)o->s);
        if (dl) NtClose(dl);
        HeapFree(GetProcessHeap(), 0, o);
        set_err(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    CloseHandle(t);
    set_err(WSA_IO_PENDING);
    return SOCKET_ERROR;
}

static Op *new_op(int kind, SOCKET s, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    Op *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*o));
    if (!o) { set_err(WSAENOBUFS); return 0; }
    o->kind = kind; o->h = o->s = s; o->ov = ov; o->cr = cr;
    return o;
}

/* For wsa.c: an overlapped receive/send on @s that would wait goes pending
 * here (returns SOCKET_ERROR with WSA_IO_PENDING, or another error);
 * returns 1 when the request can run at once instead. */
int ws_pend_io(SOCKET s, int send_, LPWSABUF bufs, DWORD n, DWORD flags, struct sockaddr *from, int *fromlen,
               LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    BYTE st[3];
    if (!ov || (flags & MSG_PEEK)) return 1;
    if (!poll_sock(s, st)) { set_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (st[2] || (send_ ? st[1] : st[0])) return 1;
    Op *o = new_op(send_ ? OP_SEND : from ? OP_RECVFROM : OP_RECV, s, ov, cr);
    if (!o) return SOCKET_ERROR;
    /* the WSABUFs themselves may be on the caller's stack: only the data must stay */
    Op *c = HeapAlloc(GetProcessHeap(), 0, sizeof(*o) + n * sizeof(WSABUF));
    if (!c) { HeapFree(GetProcessHeap(), 0, o); set_err(WSAENOBUFS); return SOCKET_ERROR; }
    memcpy(c, o, sizeof(*o));
    HeapFree(GetProcessHeap(), 0, o);
    c->bufs = (WSABUF *)(c + 1);
    memcpy(c->bufs, bufs, n * sizeof(WSABUF));
    c->nbufs = n; c->flags = flags; c->from = from; c->fromlen = fromlen;
    return go_pending(c);
}

/* ---- the extension functions ---- */
static BOOL WINAPI AcceptEx_(SOCKET l, SOCKET a, PVOID buf, DWORD n, DWORD ll, DWORD rl, LPDWORD got, LPOVERLAPPED ov)
{
    if (got) *got = 0;
    if (!buf || ll < sizeof(struct sockaddr_in) + 16 || rl < sizeof(struct sockaddr_in) + 16) { set_err(WSAEINVAL); return FALSE; }
    Op *o = new_op(OP_ACCEPT, a, ov, 0);
    if (!o) return FALSE;
    o->h = o->listener = l; o->abuf = buf; o->adata = n; o->alocal = ll; o->aremote = rl;
    LONG status;
    DWORD bytes;
    if (attempt(o, &status, &bytes)) {                   /* a connection was waiting */
        HeapFree(GetProcessHeap(), 0, o);
        if (status) { set_err(WSAECONNRESET); return FALSE; }
        if (got) *got = bytes;
        if (ov) NovaIoComplete((HANDLE)l, ov, 0, bytes);   /* on the listening socket's port, as on Windows */
        return TRUE;
    }
    if (!ov) {                                            /* no OVERLAPPED: wait here */
        LONG s2;
        while (!attempt(o, &s2, &bytes)) NtNovaSockCtl(0, 5, (ULONG)NtNovaSockCtl(0, 6, 0, 0), (void *)100);
        HeapFree(GetProcessHeap(), 0, o);
        if (s2) { set_err(WSAECONNRESET); return FALSE; }
        if (got) *got = bytes;
        return TRUE;
    }
    go_pending(o);
    return FALSE;
}

static BOOL WINAPI ConnectEx_(SOCKET s, const struct sockaddr *name, int namelen, PVOID send_buf, DWORD send_len,
                              LPDWORD sent, LPOVERLAPPED ov)
{
    if (sent) *sent = 0;
    if (!ov) { set_err(WSAEINVAL); return FALSE; }
    if (connect(s, name, namelen) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return FALSE;
        if (!send_len) {                                  /* non-blocking socket: wait until connected */
            Op *o = new_op(OP_CONNECT, s, ov, 0);
            if (!o) return FALSE;
            go_pending(o);
            return FALSE;
        }
        BYTE st[3];                                       /* with data to send: connect, then send, here */
        while (poll_sock(s, st) && !st[1] && !st[2]) NtNovaSockCtl(0, 5, (ULONG)NtNovaSockCtl(0, 6, 0, 0), (void *)100);
        if (st[2]) { set_err(WSAECONNREFUSED); return FALSE; }
    }
    DWORD done = 0;
    if (send_buf && send_len) {
        WSABUF b = { send_len, send_buf };
        if (WSASend(s, &b, 1, &done, 0, 0, 0) == SOCKET_ERROR) return FALSE;
    }
    if (sent) *sent = done;
    NovaIoComplete((HANDLE)s, ov, 0, done);
    return TRUE;
}

static BOOL WINAPI DisconnectEx_(SOCKET s, LPOVERLAPPED ov, DWORD flags, DWORD reserved)
{
    (void)flags; (void)reserved;
    shutdown(s, SD_BOTH);
    if (ov) NovaIoComplete((HANDLE)s, ov, 0, 0);
    return TRUE;
}

static VOID WINAPI GetAcceptExSockaddrs_(PVOID buf, DWORD n, DWORD ll, DWORD rl, struct sockaddr **la, LPINT lal,
                                         struct sockaddr **ra, LPINT ral)
{
    *la = (struct sockaddr *)((char *)buf + n);
    *ra = (struct sockaddr *)((char *)buf + n + ll);
    *lal = (*la)->sa_family == AF_INET6 ? (INT)sizeof(struct sockaddr_in6) : (INT)sizeof(struct sockaddr_in);
    *ral = (*ra)->sa_family == AF_INET6 ? (INT)sizeof(struct sockaddr_in6) : (INT)sizeof(struct sockaddr_in);
}

/* WSARecvMsg: the buffers and the sender; no control data */
static int WINAPI WSARecvMsg_(SOCKET s, LPWSAMSG msg, LPDWORD got, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    DWORD flags = 0;
    msg->Control.len = 0;
    msg->dwFlags = 0;
    return msg->name ? WSARecvFrom(s, msg->lpBuffers, msg->dwBufferCount, got, &flags, msg->name, &msg->namelen, ov, cr)
                     : WSARecv(s, msg->lpBuffers, msg->dwBufferCount, got, &flags, ov, cr);
}

static void *transmit_file(void)
{
    HMODULE m = LoadLibraryW(L"mswsock.dll");
    return m ? (void *)GetProcAddress(m, "TransmitFile") : 0;
}

static const struct { GUID id; int which; } g_ext[] = {
    { { 0xb5367df1, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } }, 0 },  /* AcceptEx */
    { { 0x25a207b9, 0xddf3, 0x4660, { 0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e } }, 1 },  /* ConnectEx */
    { { 0x7fda2e11, 0x8630, 0x436f, { 0xa0, 0x31, 0xf5, 0x36, 0xa6, 0xee, 0xc1, 0x57 } }, 2 },  /* DisconnectEx */
    { { 0xb5367df2, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } }, 3 },  /* GetAcceptExSockaddrs */
    { { 0xb5367df0, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } }, 4 },  /* TransmitFile */
    { { 0xf689d7c8, 0x6f1f, 0x436b, { 0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22 } }, 5 },  /* WSARecvMsg */
    { { 0xa441e712, 0x754f, 0x43ca, { 0x84, 0xa7, 0x0d, 0xee, 0x44, 0xcf, 0x60, 0x6d } }, 6 },  /* WSASendMsg */
};

/* SIO_GET_EXTENSION_FUNCTION_POINTER: 0, or a WSA error */
int ws_extension(const void *in, DWORD inlen, void *out, DWORD outlen, LPDWORD ret)
{
    if (!in || inlen < sizeof(GUID) || !out || outlen < sizeof(void *)) return WSAEFAULT;
    for (unsigned i = 0; i < sizeof(g_ext) / sizeof(g_ext[0]); i++) {
        if (memcmp(in, &g_ext[i].id, sizeof(GUID))) continue;
        void *f = 0;
        switch (g_ext[i].which) {
        case 0: f = (void *)AcceptEx_; break;
        case 1: f = (void *)ConnectEx_; break;
        case 2: f = (void *)DisconnectEx_; break;
        case 3: f = (void *)GetAcceptExSockaddrs_; break;
        case 4: f = transmit_file(); break;
        case 5: f = (void *)WSARecvMsg_; break;
        case 6: f = (void *)WSASendMsg; break;
        }
        if (!f) return WSAEOPNOTSUPP;
        memcpy(out, &f, sizeof(f));
        if (ret) *ret = sizeof(f);
        return 0;
    }
    return WSAEINVAL;                                     /* an extension NovaOS does not have */
}
