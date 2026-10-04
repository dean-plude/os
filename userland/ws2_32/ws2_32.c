/*
 * ws2_32.dll — Winsock 2 over NovaOS's socket syscalls (NtNovaSock*)
 *
 * A SOCKET is the kernel handle the syscalls use.  Errors are recorded per
 * thread (WSAGetLastError, via the TEB last-error slot).  Blocking and
 * non-blocking modes, TCP and UDP, name resolution and a simple select()
 * are supported; this is the surface a typical sockets program (and the
 * NetSurf fetchers) needs, not the whole of Winsock.
 */
#define WS2_EXPORT
#define NOVA_BUILD_KERNEL32          /* reuse WINBASEAPI import decls we call */
#include <winsock2.h>
#include <winternl.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
WINBASEAPI LPVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
WINBASEAPI HANDLE WINAPI GetProcessHeap(void);
WINBASEAPI BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID);

#define WSATYPE_NOT_FOUND_ 10109
static void set_err(int e) { *(DWORD *)(NtCurrentTebBytes() + TEB_LAST_ERROR) = (DWORD)e; }

int WINAPI WSAGetLastError(void) { return (int)*(DWORD *)(NtCurrentTebBytes() + TEB_LAST_ERROR); }
void WINAPI WSASetLastError(int e) { set_err(e); }

/* Map a negated SOCK_* kernel error to a WSA error and return SOCKET_ERROR. */
static int sock_err(long r)
{
    static const int map[] = {
        0, WSAEWOULDBLOCK, WSAECONNRESET, WSAECONNREFUSED, WSAENOTCONN, WSAETIMEDOUT,
        WSAEHOSTUNREACH, WSAEINVAL, WSAENOBUFS, WSAEADDRINUSE, WSAEISCONN, WSAEFAULT,
        WSAENETDOWN, WSAEMFILE, WSAENOTSOCK, WSAEAFNOSUPPORT,
    };
    int e = (int)(-r);
    set_err(e >= 0 && e < (int)(sizeof(map) / sizeof(map[0])) ? map[e] : WSAEINVAL);
    return SOCKET_ERROR;
}

int WSAStartup(WORD ver, LPWSADATA d)
{
    (void)ver;
    if (d) {
        memset(d, 0, sizeof(*d));
        d->wVersion = 0x0202; d->wHighVersion = 0x0202;
        d->iMaxSockets = 64; d->iMaxUdpDg = 8192;
        const char *desc = "NovaOS Winsock 2.2";
        for (int i = 0; desc[i]; i++) d->szDescription[i] = desc[i];
    }
    return 0;
}
int WSACleanup(void) { return 0; }

u_short htons(u_short v) { return (u_short)((v << 8) | (v >> 8)); }
u_short ntohs(u_short v) { return htons(v); }
u_long  htonl(u_long v) { return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF); }
u_long  ntohl(u_long v) { return htonl(v); }
/* the WSA forms take the socket (whose byte order is always the network's) */
__declspec(dllexport) int WSAAPI WSAHtonl(SOCKET s, u_long v, u_long *out) { (void)s; if (!out) return SOCKET_ERROR; *out = htonl(v); return 0; }
__declspec(dllexport) int WSAAPI WSAHtons(SOCKET s, u_short v, u_short *out) { (void)s; if (!out) return SOCKET_ERROR; *out = htons(v); return 0; }
__declspec(dllexport) int WSAAPI WSANtohl(SOCKET s, u_long v, u_long *out) { (void)s; if (!out) return SOCKET_ERROR; *out = ntohl(v); return 0; }
__declspec(dllexport) int WSAAPI WSANtohs(SOCKET s, u_short v, u_short *out) { (void)s; if (!out) return SOCKET_ERROR; *out = ntohs(v); return 0; }

unsigned long inet_addr(const char *cp)
{
    unsigned long parts[4] = { 0 };
    int n = 0;
    for (const char *p = cp; ; p++) {
        if (*p >= '0' && *p <= '9') parts[n] = parts[n] * 10 + (*p - '0');
        else if (*p == '.') { if (++n > 3) return INADDR_NONE; }
        else if (*p == 0) break;
        else return INADDR_NONE;
    }
    if (n != 3) return INADDR_NONE;
    for (int i = 0; i < 4; i++) if (parts[i] > 255) return INADDR_NONE;
    return (unsigned long)(parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24));
}

char *inet_ntoa(struct in_addr in)
{
    static __declspec(thread) char buf[16];
    unsigned char *b = (unsigned char *)&in.s_addr;
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        int v = b[i];
        if (v >= 100) *p++ = '0' + v / 100;
        if (v >= 10)  *p++ = '0' + (v / 10) % 10;
        *p++ = '0' + v % 10;
        if (i < 3) *p++ = '.';
    }
    *p = 0;
    return buf;
}

SOCKET socket(int af, int type, int protocol)
{
    (void)protocol;
    if (af != AF_INET && af != AF_INET6 && af != AF_UNSPEC) { set_err(WSAEAFNOSUPPORT); return INVALID_SOCKET; }
    INT_PTR h = NtNovaSocket(type == SOCK_DGRAM ? 1 : 0, af == AF_INET6 ? AF_INET6 : AF_INET);
    if (!h) { set_err(WSAENOBUFS); return INVALID_SOCKET; }
    return (SOCKET)h;
}

void ws_cancel_socket(SOCKET s);                 /* overlapped.c: pending requests end */
int closesocket(SOCKET s) { ws_cancel_socket(s); NtClose((HANDLE)s); return 0; }

/* The kernel takes and gives Winsock's own SOCKADDR_IN / SOCKADDR_IN6 */
static int addr_ok(const struct sockaddr *sa, int len)
{
    if (!sa || len < (int)sizeof(struct sockaddr_in)) { set_err(WSAEFAULT); return 0; }
    if (sa->sa_family == AF_INET6 && len < (int)sizeof(struct sockaddr_in6)) { set_err(WSAEFAULT); return 0; }
    if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) { set_err(WSAEAFNOSUPPORT); return 0; }
    return 1;
}

/* Copy a kernel address (28 bytes) out as the caller's sockaddr */
static void addr_out(const BYTE sa[28], struct sockaddr *to, int *tolen)
{
    if (!to || !tolen) return;
    int need = *(const USHORT *)sa == AF_INET6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    if (*tolen < need) { *tolen = need; return; }
    memcpy(to, sa, need);
    *tolen = need;
}

int connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    if (!addr_ok(name, namelen)) return SOCKET_ERROR;
    long r = NtNovaSockConnect((INT_PTR)s, name, (ULONG)namelen);
    return r < 0 ? sock_err(r) : 0;
}

int bind(SOCKET s, const struct sockaddr *name, int namelen)
{
    if (!addr_ok(name, namelen)) return SOCKET_ERROR;
    long r = NtNovaSockBind((INT_PTR)s, name, (ULONG)namelen);
    return r < 0 ? sock_err(r) : 0;
}

int listen(SOCKET s, int backlog) { long r = NtNovaSockListen((INT_PTR)s, backlog); return r < 0 ? sock_err(r) : 0; }

void evsel_rearm(SOCKET s, long bits);          /* wsa.c: WSAAsyncSelect re-enabling */

SOCKET accept(SOCKET s, struct sockaddr *addr, int *addrlen)
{
    BYTE sa[28];
    evsel_rearm(s, FD_ACCEPT);
    INT_PTR h = NtNovaSockAccept((INT_PTR)s, sa);
    if (!h) { set_err(WSAEWOULDBLOCK); return INVALID_SOCKET; }
    addr_out(sa, addr, addrlen);
    return (SOCKET)h;
}

/* WSAAccept: accept, then let @cond judge the caller (its address as
 * caller id, the listening address as callee id; no connect data or QoS on
 * TCP).  Without SO_CONDITIONAL_ACCEPT the connection is already made when
 * @cond is asked, as on Windows: CF_REJECT closes it (WSAECONNREFUSED),
 * and CF_DEFER, which needs a connection still pending, closes it too
 * (WSATRY_AGAIN). */
typedef int (__stdcall *WSACONDITIONPROC_)(LPWSABUF caller, LPWSABUF caller_data, void *qos, void *gqos,
                                           LPWSABUF callee, LPWSABUF callee_data, unsigned int *g, DWORD_PTR cb);
__declspec(dllexport) SOCKET __stdcall WSAAccept(SOCKET s, struct sockaddr *addr, int *addrlen, WSACONDITIONPROC_ cond,
                                                DWORD_PTR cb)
{
    BYTE peer[28], local[28];
    int pl = sizeof(peer), ll = sizeof(local);
    SOCKET c = accept(s, (struct sockaddr *)peer, &pl);
    if (c == INVALID_SOCKET) return INVALID_SOCKET;
    if (cond) {
        WSABUF caller = { (ULONG)pl, (CHAR *)peer }, callee = { 0, (CHAR *)local };
        if (!getsockname(s, (struct sockaddr *)local, &ll)) callee.len = (ULONG)ll;
        unsigned int g = 0;
        int r = cond(&caller, 0, 0, 0, callee.len ? &callee : 0, 0, &g, cb);
        if (r != 0) {                                           /* CF_ACCEPT */
            closesocket(c);
            set_err(r == 2 ? 11002 /* WSATRY_AGAIN */ : WSAECONNREFUSED);
            return INVALID_SOCKET;
        }
    }
    if (addr && addrlen) {
        if (*addrlen < pl) { closesocket(c); set_err(WSAEFAULT); return INVALID_SOCKET; }
        memcpy(addr, peer, (size_t)pl);
        *addrlen = pl;
    }
    return c;
}

int send(SOCKET s, const char *buf, int len, int flags)
{
    (void)flags;
    long r = NtNovaSockSend((INT_PTR)s, buf, len);
    if (r < 0) {
        int e = sock_err(r);
        if (WSAGetLastError() == WSAEWOULDBLOCK) evsel_rearm(s, FD_WRITE);
        return e;
    }
    return (int)r;
}

int recv(SOCKET s, char *buf, int len, int flags)
{
    evsel_rearm(s, FD_READ);
    long r = (flags & MSG_PEEK) ? NtNovaSockCtl((INT_PTR)s, 8, len, buf) : NtNovaSockRecv((INT_PTR)s, buf, len);
    return r < 0 ? sock_err(r) : (int)r;
}

int sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    (void)flags;
    if (!addr_ok(to, tolen)) return SOCKET_ERROR;
    BYTE sa[28] = { 0 };
    memcpy(sa, to, to->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    long r = NtNovaSockSendTo((INT_PTR)s, buf, len, sa);
    return r < 0 ? sock_err(r) : (int)r;
}

int recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    (void)flags;
    BYTE sa[28];
    evsel_rearm(s, FD_READ);
    long r = NtNovaSockRecvFrom((INT_PTR)s, buf, len, sa);
    if (r < 0) return sock_err(r);
    addr_out(sa, from, fromlen);
    return (int)r;
}

int shutdown(SOCKET s, int how) { long r = NtNovaSockCtl((INT_PTR)s, 1, how, 0); return r < 0 ? sock_err(r) : 0; }

int ioctlsocket(SOCKET s, long cmd, u_long *argp)
{
    if ((unsigned long)cmd == FIONBIO) { NtNovaSockCtl((INT_PTR)s, 0, argp && *argp ? 1 : 0, 0); return 0; }
    if ((unsigned long)cmd == FIONREAD) {
        long n = NtNovaSockCtl((INT_PTR)s, 7, 0, 0);
        if (n < 0) return sock_err(n);
        if (argp) *argp = (u_long)(n & 0x3FFFFFFF);
        return 0;
    }
    set_err(WSAEINVAL);
    return SOCKET_ERROR;
}

/* Socket options: the kernel's SOCKOPT_* (kernel/net/sock.h) for a level
 * and name, 0 for one NovaOS accepts and ignores (IPV6_V6ONLY, the
 * AcceptEx/ConnectEx context updates, SO_EXCLUSIVEADDRUSE...), as it
 * accepted every option before */
enum { KO_RCVBUF = 1, KO_SNDBUF, KO_REUSEADDR, KO_KEEPALIVE, KO_BROADCAST, KO_NODELAY, KO_RCVTIMEO,
       KO_SNDTIMEO, KO_LINGER, KO_TTL, KO_TYPE, KO_ERROR, KO_ACCEPTCONN };
static int kernel_opt(int level, int opt)
{
    if (level == SOL_SOCKET) switch (opt) {
        case SO_RCVBUF: return KO_RCVBUF;       case SO_SNDBUF: return KO_SNDBUF;
        case SO_REUSEADDR: return KO_REUSEADDR; case SO_KEEPALIVE: return KO_KEEPALIVE;
        case SO_BROADCAST: return KO_BROADCAST; case SO_RCVTIMEO: return KO_RCVTIMEO;
        case SO_SNDTIMEO: return KO_SNDTIMEO;   case SO_LINGER: case SO_DONTLINGER: return KO_LINGER;
        case SO_TYPE: return KO_TYPE;           case SO_ERROR: return KO_ERROR;
        case SO_ACCEPTCONN: return KO_ACCEPTCONN;
    }
    if (level == IPPROTO_TCP && opt == TCP_NODELAY) return KO_NODELAY;
    if ((level == IPPROTO_IP && opt == IP_TTL) || (level == IPPROTO_IPV6 && opt == IPV6_UNICAST_HOPS)) return KO_TTL;
    return 0;
}

/* A BOOL or DWORD option's value: 4 bytes, or 1 (Windows takes a BOOL
 * option given as one byte) */
static int opt_value(const char *val, int len, DWORD *v)
{
    if (!val) { set_err(WSAEFAULT); return 0; }
    if (len >= 4) { memcpy(v, val, 4); return 1; }
    if (len >= 1) { *v = (BYTE)val[0]; return 1; }
    set_err(WSAEFAULT);
    return 0;
}

int setsockopt(SOCKET s, int level, int opt, const char *val, int len)
{
    int ko = kernel_opt(level, opt);
    if (!ko) return 0;                                      /* accepted, ignored */
    if (ko == KO_TYPE || ko == KO_ERROR || ko == KO_ACCEPTCONN) { set_err(WSAENOPROTOOPT); return SOCKET_ERROR; }
    DWORD v;
    if (ko == KO_LINGER && opt == SO_LINGER) {
        if (!val || len < (int)sizeof(struct linger)) { set_err(WSAEFAULT); return SOCKET_ERROR; }
        struct linger l;
        memcpy(&l, val, sizeof(l));
        v = (l.l_onoff ? 1u : 0u) | ((DWORD)(l.l_linger > 0x7FFF ? 0x7FFF : l.l_linger) << 16);
    } else {
        if (!opt_value(val, len, &v)) return SOCKET_ERROR;
        if (ko == KO_LINGER) {                              /* SO_DONTLINGER: off, the timeout kept */
            long cur = NtNovaSockCtl((INT_PTR)s, 10, KO_LINGER, 0);
            if (cur < 0) return sock_err(cur);
            v = (v ? 0u : 1u) | ((DWORD)cur & 0xFFFF0000u);
        } else if (v > 0x7FFFFFFF) {
            v = 0x7FFFFFFF;
        }
    }
    long r = NtNovaSockCtl((INT_PTR)s, 9, (ULONG_PTR)ko, (void *)(ULONG_PTR)v);
    if (r < 0) {
        if (r == -7 /* SOCK_EINVAL */) { set_err(ko == KO_TTL ? WSAEINVAL : WSAENOPROTOOPT); return SOCKET_ERROR; }
        return sock_err(r);
    }
    return 0;
}

/* SOCK_E* codes (SO_ERROR) as WSA errors, as sock_err maps them */
static int wsa_of(long e)
{
    if (!e) return 0;
    sock_err(-e);
    return WSAGetLastError();
}

int getsockopt(SOCKET s, int level, int opt, char *val, int *len)
{
    if (!val || !len) { set_err(WSAEFAULT); return SOCKET_ERROR; }
    int ko = kernel_opt(level, opt);
    if (!ko) {                                              /* (one NovaOS ignores: 0, as before) */
        if (*len >= 4) { *(int *)val = 0; *len = 4; }
        return 0;
    }
    DWORD saved = WSAGetLastError();
    long r = NtNovaSockCtl((INT_PTR)s, 10, (ULONG_PTR)ko, 0);
    if (r < 0) return sock_err(r);
    if (ko == KO_LINGER && opt == SO_LINGER) {
        if (*len < (int)sizeof(struct linger)) { set_err(WSAEFAULT); return SOCKET_ERROR; }
        struct linger l = { (USHORT)(r & 0xFFFF), (USHORT)(r >> 16) };
        memcpy(val, &l, sizeof(l));
        *len = sizeof(l);
        return 0;
    }
    DWORD v = (DWORD)r;
    if (ko == KO_LINGER) v = !(r & 0xFFFF);                 /* SO_DONTLINGER */
    if (ko == KO_ERROR) { v = (DWORD)wsa_of(r); set_err((int)saved); }
    if (*len >= 4) { memcpy(val, &v, 4); *len = 4; }
    else if (*len >= 1 && v <= 0xFF) { val[0] = (char)v; *len = 1; }   /* (a BOOL in one byte) */
    else { set_err(WSAEFAULT); return SOCKET_ERROR; }
    return 0;
}

static int getname(SOCKET s, struct sockaddr *name, int *namelen, int which)
{
    BYTE sa[28];
    long r = NtNovaSockCtl((INT_PTR)s, which, 0, sa);
    if (r < 0) return sock_err(r);
    if (name && namelen) {
        int need = *namelen;
        addr_out(sa, name, namelen);
        if (need < *namelen) { set_err(WSAEFAULT); return SOCKET_ERROR; }
    }
    return 0;
}
int getpeername(SOCKET s, struct sockaddr *n, int *l) { return getname(s, n, l, 2); }
int getsockname(SOCKET s, struct sockaddr *n, int *l) { return getname(s, n, l, 3); }

int FD_ISSET(SOCKET fd, fd_set *set)
{
    for (UINT i = 0; i < set->fd_count; i++) if (set->fd_array[i] == fd) return 1;
    return 0;
}

/* Milliseconds since boot (KUSER_SHARED_DATA.TickCount, 10 ms ticks) */
static ULONGLONG now_ms(void) { return (ULONGLONG)*(volatile ULONG *)(ULONG_PTR)0x7FFE0320 * 10; }

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timeval *tv)
{
    (void)nfds;
    ULONGLONG start = now_ms(), limit = 0;
    int forever = (tv == 0);
    if (tv) limit = (ULONGLONG)tv->tv_sec * 1000 + tv->tv_usec / 1000;
    for (;;) {
        /* the network generation before looking: the wait below returns as
         * soon as anything changes after this point */
        ULONG gen = (ULONG)NtNovaSockCtl(0, 6, 0, 0);
        int ready = 0;
        fd_set r = { 0 }, w = { 0 }, e = { 0 };
        if (rd) for (UINT i = 0; i < rd->fd_count; i++) {
            BYTE st[3]; if (NtNovaSockCtl((INT_PTR)rd->fd_array[i], 4, 0, st) == 0 && (st[0] || st[2]))
                { r.fd_array[r.fd_count++] = rd->fd_array[i]; ready++; }
        }
        if (wr) for (UINT i = 0; i < wr->fd_count; i++) {
            BYTE st[3]; if (NtNovaSockCtl((INT_PTR)wr->fd_array[i], 4, 0, st) == 0 && st[1])
                { w.fd_array[w.fd_count++] = wr->fd_array[i]; ready++; }
        }
        if (ex) for (UINT i = 0; i < ex->fd_count; i++) {
            BYTE st[3]; if (NtNovaSockCtl((INT_PTR)ex->fd_array[i], 4, 0, st) == 0 && st[2])
                { e.fd_array[e.fd_count++] = ex->fd_array[i]; ready++; }
        }
        if (ready) { if (rd) *rd = r; if (wr) *wr = w; if (ex) *ex = e; return ready; }
        ULONGLONG spent = now_ms() - start;
        if (!forever && spent >= limit) { if (rd) FD_ZERO(rd); if (wr) FD_ZERO(wr); if (ex) FD_ZERO(ex); return 0; }
        /* Nothing yet: sleep in the kernel until the network moves on */
        NtNovaSockCtl(0, 5, gen, (void *)(ULONG_PTR)(forever ? 100 : limit - spent));
    }
}

/* "localhost" (and "x.localhost", RFC 6761) is the loopback address,
 * without asking DNS, as on Windows */
static int is_localhost(const char *n)
{
    static const char lh[] = "localhost";
    size_t len = strlen(n);
    if (len && n[len - 1] == '.') len--;
    if (len < 9) return 0;
    const char *t = n + len - 9;
    if (t != n && t[-1] != '.') return 0;
    for (int i = 0; i < 9; i++) if ((t[i] | 0x20) != lh[i]) return 0;
    return 1;
}

struct hostent *gethostbyname(const char *name)
{
    static __declspec(thread) struct hostent he;
    static __declspec(thread) ULONG addr;
    static __declspec(thread) char *alist[2];
    static __declspec(thread) char namebuf[256];
    ULONG ip = inet_addr(name);
    if (ip == INADDR_NONE && is_localhost(name)) ip = htonl(INADDR_LOOPBACK);
    if (ip == INADDR_NONE) {
        BYTE sa[28];
        if (NtNovaResolve(name, sa, 1, AF_INET) < 1) { set_err(WSAHOST_NOT_FOUND); return 0; }
        memcpy(&ip, sa + 4, 4);
    }
    addr = ip;
    alist[0] = (char *)&addr; alist[1] = 0;
    for (int i = 0; i < 255 && name[i]; i++) namebuf[i] = name[i];
    he.h_name = namebuf;
    he.h_aliases = alist + 1;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = alist;
    return &he;
}

/* getaddrinfo: numeric hosts (IPv4 or IPv6) without a lookup, names
 * through the kernel's resolver (IPv6 first when the machine has a global
 * IPv6 address); AI_V4MAPPED maps IPv4 results into AF_INET6 answers. */
static int parse_port(const char *service, USHORT *port)
{
    *port = 0;
    if (!service || !*service) return 0;
    int v = 0;
    const char *p = service;
    for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (*p || v > 65535) {                      /* a service name */
        struct servent *se = getservbyname(service, 0);
        if (!se) return WSATYPE_NOT_FOUND_;
        *port = (USHORT)se->s_port;
        return 0;
    }
    *port = htons((u_short)v);
    return 0;
}

static struct addrinfo *new_ai(const BYTE *sa, const struct addrinfo *hints, USHORT port)
{
    HANDLE h = GetProcessHeap();
    int v6 = *(const USHORT *)sa == AF_INET6;
    int len = v6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    struct addrinfo *ai = HeapAlloc(h, 8, sizeof(struct addrinfo));
    BYTE *a = HeapAlloc(h, 8, sizeof(struct sockaddr_in6));
    if (!ai || !a) { if (ai) HeapFree(h, 0, ai); if (a) HeapFree(h, 0, a); return 0; }
    memset(ai, 0, sizeof(*ai));
    memset(a, 0, sizeof(struct sockaddr_in6));
    memcpy(a, sa, len);
    memcpy(a + 2, &port, 2);
    ai->ai_family = v6 ? AF_INET6 : AF_INET;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints && hints->ai_protocol ? hints->ai_protocol
                    : ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    ai->ai_addrlen = (size_t)len;
    ai->ai_addr = (struct sockaddr *)a;
    return ai;
}

/* "1.2.3.4" or "fd00::1" (with an optional %zone) → a 28-byte address */
static int numeric(const char *node, BYTE sa[28])
{
    memset(sa, 0, 28);
    if (inet_pton(AF_INET, node, sa + 4) == 1) { *(USHORT *)sa = AF_INET; return 1; }
    char tmp[64];
    int i = 0;
    for (; node[i] && node[i] != '%' && i < 63; i++) tmp[i] = node[i];
    tmp[i] = 0;
    if (inet_pton(AF_INET6, tmp, sa + 8) != 1) return 0;
    *(USHORT *)sa = AF_INET6;
    if (node[i] == '%') { ULONG z = 0; for (const char *p = node + i + 1; *p >= '0' && *p <= '9'; p++) z = z * 10 + (ULONG)(*p - '0'); memcpy(sa + 24, &z, 4); }
    return 1;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    int fam = hints ? hints->ai_family : AF_UNSPEC, flags = hints ? hints->ai_flags : 0;
    if (fam != AF_UNSPEC && fam != AF_INET && fam != AF_INET6) return WSAEAFNOSUPPORT;
    USHORT port;
    int e = parse_port(service, &port);
    if (e) return e;
    BYTE list[8][28];
    int n = 0;
    if (!node || is_localhost(node)) {          /* a wildcard or loopback address */
        int both = fam == AF_UNSPEC, any = !node && (flags & AI_PASSIVE);
        if (fam == AF_INET6 || both) {
            memset(list[n], 0, 28); *(USHORT *)list[n] = AF_INET6;
            if (!any) list[n][23] = 1;                              /* ::1 */
            n++;
        }
        if (fam == AF_INET || both) {
            memset(list[n], 0, 28); *(USHORT *)list[n] = AF_INET;
            if (!any) { ULONG lo = htonl(INADDR_LOOPBACK); memcpy(list[n] + 4, &lo, 4); }
            n++;
        }
    } else if (numeric(node, list[0])) {
        USHORT f = *(USHORT *)list[0];
        if (fam != AF_UNSPEC && f != fam && !(fam == AF_INET6 && f == AF_INET && (flags & AI_V4MAPPED)))
            return WSAHOST_NOT_FOUND;
        n = 1;
    } else if (flags & AI_NUMERICHOST) {
        return WSAHOST_NOT_FOUND;
    } else {
        int want = fam == AF_INET6 && (flags & AI_V4MAPPED) ? AF_UNSPEC : fam;
        long r = NtNovaResolve(node, list, 8, (ULONG)want);
        if (r < 1) return WSAHOST_NOT_FOUND;
        n = (int)r;
    }
    struct addrinfo *head = 0, **tail = &head;
    for (int i = 0; i < n; i++) {
        BYTE *sa = list[i];
        if (fam == AF_INET6 && *(USHORT *)sa == AF_INET) {          /* AI_V4MAPPED */
            BYTE m[28] = { 0 };
            *(USHORT *)m = AF_INET6;
            m[18] = m[19] = 0xFF;
            memcpy(m + 20, sa + 4, 4);
            memcpy(sa, m, 28);
        }
        struct addrinfo *ai = new_ai(sa, hints, port);
        if (!ai) { freeaddrinfo(head); return WSAENOBUFS; }
        if (i == 0 && (flags & AI_CANONNAME) && node) {
            size_t len = strlen(node) + 1;
            ai->ai_canonname = HeapAlloc(GetProcessHeap(), 0, len);
            if (ai->ai_canonname) memcpy(ai->ai_canonname, node, len);
        }
        *tail = ai;
        tail = &ai->ai_next;
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai)
{
    HANDLE h = GetProcessHeap();
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        if (ai->ai_addr) HeapFree(h, 0, ai->ai_addr);
        if (ai->ai_canonname) HeapFree(h, 0, ai->ai_canonname);
        HeapFree(h, 0, ai);
        ai = next;
    }
}
