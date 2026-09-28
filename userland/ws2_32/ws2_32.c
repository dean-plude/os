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

static BYTE *teb(void) { BYTE *t; __asm__("movq %%gs:0x30, %0" : "=r"(t)); return t; }
static void set_err(int e) { *(DWORD *)(teb() + 0x68) = (DWORD)e; }

int WSAGetLastError(void) { return (int)*(DWORD *)(teb() + 0x68); }
void WSASetLastError(int e) { set_err(e); }

/* Map a negated SOCK_* kernel error to a WSA error and return SOCKET_ERROR. */
static int sock_err(long r)
{
    static const int map[] = {
        0, WSAEWOULDBLOCK, WSAECONNRESET, WSAECONNREFUSED, WSAENOTCONN, WSAETIMEDOUT,
        WSAEHOSTUNREACH, WSAEINVAL, WSAENOBUFS, WSAEADDRINUSE, WSAEISCONN, WSAEFAULT,
        WSAENETDOWN, WSAEMFILE, WSAENOTSOCK,
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
    (void)af; (void)protocol;
    INT_PTR h = NtNovaSocket(type == SOCK_DGRAM ? 1 : 0);
    if (!h) { set_err(WSAENOBUFS); return INVALID_SOCKET; }
    return (SOCKET)h;
}

int closesocket(SOCKET s) { NtClose((HANDLE)s); return 0; }

static int addr_of(const struct sockaddr *sa, int len, ULONG *ip, USHORT *port)
{
    if (!sa || len < (int)sizeof(struct sockaddr_in)) { set_err(WSAEFAULT); return -1; }
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    *ip = in->sin_addr.s_addr;
    *port = in->sin_port;
    return 0;
}

int connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    ULONG ip; USHORT port;
    if (addr_of(name, namelen, &ip, &port)) return SOCKET_ERROR;
    long r = NtNovaSockConnect((INT_PTR)s, ip, port);
    return r < 0 ? sock_err(r) : 0;
}

int bind(SOCKET s, const struct sockaddr *name, int namelen)
{
    ULONG ip; USHORT port;
    if (addr_of(name, namelen, &ip, &port)) return SOCKET_ERROR;
    long r = NtNovaSockBind((INT_PTR)s, ip, port);
    return r < 0 ? sock_err(r) : 0;
}

int listen(SOCKET s, int backlog) { long r = NtNovaSockListen((INT_PTR)s, backlog); return r < 0 ? sock_err(r) : 0; }

SOCKET accept(SOCKET s, struct sockaddr *addr, int *addrlen)
{
    BYTE sa[8];
    INT_PTR h = NtNovaSockAccept((INT_PTR)s, sa);
    if (!h) { set_err(WSAEWOULDBLOCK); return INVALID_SOCKET; }
    if (addr && addrlen && *addrlen >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)addr;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        memcpy(&in->sin_addr.s_addr, sa, 4);
        memcpy(&in->sin_port, sa + 4, 2);
        *addrlen = sizeof(*in);
    }
    return (SOCKET)h;
}

int send(SOCKET s, const char *buf, int len, int flags)
{
    (void)flags;
    long r = NtNovaSockSend((INT_PTR)s, buf, len);
    return r < 0 ? sock_err(r) : (int)r;
}

int recv(SOCKET s, char *buf, int len, int flags)
{
    (void)flags;
    long r = NtNovaSockRecv((INT_PTR)s, buf, len);
    return r < 0 ? sock_err(r) : (int)r;
}

int sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    (void)flags;
    ULONG ip; USHORT port;
    if (addr_of(to, tolen, &ip, &port)) return SOCKET_ERROR;
    BYTE sa[8]; memcpy(sa, &ip, 4); memcpy(sa + 4, &port, 2);
    long r = NtNovaSockSendTo((INT_PTR)s, buf, len, sa);
    return r < 0 ? sock_err(r) : (int)r;
}

int recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    (void)flags;
    BYTE sa[8];
    long r = NtNovaSockRecvFrom((INT_PTR)s, buf, len, sa);
    if (r < 0) return sock_err(r);
    if (from && fromlen && *fromlen >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)from;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        memcpy(&in->sin_addr.s_addr, sa, 4);
        memcpy(&in->sin_port, sa + 4, 2);
        *fromlen = sizeof(*in);
    }
    return (int)r;
}

int shutdown(SOCKET s, int how) { long r = NtNovaSockCtl((INT_PTR)s, 1, how, 0); return r < 0 ? sock_err(r) : 0; }

int ioctlsocket(SOCKET s, long cmd, u_long *argp)
{
    if ((unsigned long)cmd == FIONBIO) { NtNovaSockCtl((INT_PTR)s, 0, argp && *argp ? 1 : 0, 0); return 0; }
    if ((unsigned long)cmd == FIONREAD) { if (argp) *argp = 0; return 0; }
    set_err(WSAEINVAL);
    return SOCKET_ERROR;
}

int setsockopt(SOCKET s, int level, int opt, const char *val, int len)
{ (void)s; (void)level; (void)opt; (void)val; (void)len; return 0; }   /* accepted, ignored */

int getsockopt(SOCKET s, int level, int opt, char *val, int *len)
{
    (void)s; (void)level;
    if (opt == SO_ERROR && val && len && *len >= 4) { *(int *)val = 0; *len = 4; return 0; }
    if (val && len && *len >= 4) { *(int *)val = 0; *len = 4; }
    return 0;
}

static int getname(SOCKET s, struct sockaddr *name, int *namelen, int which)
{
    BYTE sa[8];
    long r = NtNovaSockCtl((INT_PTR)s, which, 0, sa);
    if (r < 0) return sock_err(r);
    if (name && namelen && *namelen >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)name;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        memcpy(&in->sin_addr.s_addr, sa, 4);
        memcpy(&in->sin_port, sa + 4, 2);
        *namelen = sizeof(*in);
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

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timeval *tv)
{
    (void)nfds;
    ULONGLONG deadline = 0;
    int forever = (tv == 0);
    if (tv) deadline = (ULONGLONG)tv->tv_sec * 1000 + tv->tv_usec / 1000;
    ULONGLONG waited = 0;
    for (;;) {
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
        if (!forever && waited >= deadline) { if (rd) FD_ZERO(rd); if (wr) FD_ZERO(wr); if (ex) FD_ZERO(ex); return 0; }
        NtYieldExecution();
        waited += 1;                              /* coarse: one poll per yield */
    }
}

struct hostent *gethostbyname(const char *name)
{
    static __declspec(thread) struct hostent he;
    static __declspec(thread) ULONG addr;
    static __declspec(thread) char *alist[2];
    static __declspec(thread) char namebuf[256];
    ULONG ip = inet_addr(name);
    if (ip == INADDR_NONE) {
        if (NtNovaResolve(name, &ip) < 0) { set_err(WSAHOST_NOT_FOUND); return 0; }
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

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    ULONG ip = INADDR_ANY;
    if (node) {
        ip = inet_addr(node);
        if (ip == INADDR_NONE && NtNovaResolve(node, &ip) < 0) return WSAHOST_NOT_FOUND;
    } else if (hints && (hints->ai_flags & AI_PASSIVE)) {
        ip = INADDR_ANY;
    } else {
        ip = htonl(INADDR_LOOPBACK);
    }
    USHORT port = 0;
    if (service) {
        int v = 0;
        for (const char *p = service; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        port = htons((u_short)v);
    }
    HANDLE h = GetProcessHeap();
    struct addrinfo *ai = HeapAlloc(h, 8, sizeof(struct addrinfo));
    struct sockaddr_in *sa = HeapAlloc(h, 8, sizeof(struct sockaddr_in));
    if (!ai || !sa) { if (ai) HeapFree(h, 0, ai); if (sa) HeapFree(h, 0, sa); return WSAENOBUFS; }
    memset(ai, 0, sizeof(*ai));
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = port;
    sa->sin_addr.s_addr = ip;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints && hints->ai_protocol ? hints->ai_protocol : IPPROTO_TCP;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)sa;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai)
{
    HANDLE h = GetProcessHeap();
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        if (ai->ai_addr) HeapFree(h, 0, ai->ai_addr);
        HeapFree(h, 0, ai);
        ai = next;
    }
}
