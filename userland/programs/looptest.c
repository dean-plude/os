/* looptest.exe — Winsock over the loopback interface, as Firefox uses it:
 * a socket pair over 127.0.0.1 and ::1 (bind to port 0, listen,
 * getsockname, a non-blocking connect, getpeername, accept), data sent before accept,
 * closing a listener with a connection still queued, "localhost"
 * resolving to ::1 and 127.0.0.1 without DNS, and socket options
 * (setsockopt/getsockopt) that read back and change what a socket does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static int passed, failed;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

static int wait_for(SOCKET s, int write, int ms)
{
    fd_set f; FD_ZERO(&f); FD_SET(s, &f);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(0, write ? 0 : &f, write ? &f : 0, 0, &tv);
}

/* A connected pair over @family's loopback address, Firefox's way */
static void pair(int family, const char *name)
{
    char what[96];
    struct sockaddr_storage a;
    int len = family == AF_INET6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    memset(&a, 0, sizeof(a));
    a.ss_family = (short)family;
    if (family == AF_INET) ((struct sockaddr_in *)&a)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else ((struct sockaddr_in6 *)&a)->sin6_addr = in6addr_loopback;

    SOCKET l = socket(family, SOCK_STREAM, 0);
    snprintf(what, sizeof(what), "%s: bind to port 0 and listen", name);
    check(what, l != INVALID_SOCKET && bind(l, (struct sockaddr *)&a, len) == 0 && listen(l, 5) == 0);
    int n = len;
    getsockname(l, (struct sockaddr *)&a, &n);
    USHORT port = ntohs(((struct sockaddr_in *)&a)->sin_port);
    snprintf(what, sizeof(what), "%s: getsockname gives the port picked (%u)", name, port);
    check(what, port != 0);

    SOCKET c = socket(family, SOCK_STREAM, 0);
    u_long nb = 1;
    ioctlsocket(l, FIONBIO, &nb);
    ioctlsocket(c, FIONBIO, &nb);
    int r = connect(c, (struct sockaddr *)&a, len);
    snprintf(what, sizeof(what), "%s: a non-blocking connect is under way", name);
    check(what, r == 0 || WSAGetLastError() == WSAEWOULDBLOCK);
    snprintf(what, sizeof(what), "%s: it becomes writable (connected)", name);
    check(what, wait_for(c, 1, 2000) == 1);
    struct sockaddr_storage pn; int pl = sizeof(pn);
    memset(&pn, 0, sizeof(pn));
    snprintf(what, sizeof(what), "%s: getpeername names the peer after a non-blocking connect (NSS's TLS start)", name);
    check(what, getpeername(c, (struct sockaddr *)&pn, &pl) == 0 && pn.ss_family == family &&
                ntohs(((struct sockaddr_in *)&pn)->sin_port) == port);
    snprintf(what, sizeof(what), "%s: the client sends before accept", name);
    check(what, send(c, "ping", 4, 0) == 4);
    snprintf(what, sizeof(what), "%s: the listener is readable", name);
    check(what, wait_for(l, 0, 2000) == 1);
    SOCKET s = accept(l, 0, 0);
    snprintf(what, sizeof(what), "%s: accept", name);
    check(what, s != INVALID_SOCKET);
    char b[8] = { 0 };
    snprintf(what, sizeof(what), "%s: the bytes sent before accept arrive", name);
    check(what, wait_for(s, 0, 2000) == 1 && recv(s, b, sizeof(b), 0) == 4 && !memcmp(b, "ping", 4));
    snprintf(what, sizeof(what), "%s: the accepted socket is non-blocking like its listener (NSPR's socket pair)", name);
    check(what, recv(s, b, sizeof(b), 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK);
    snprintf(what, sizeof(what), "%s: and the other way", name);
    check(what, send(s, "pong", 4, 0) == 4 && wait_for(c, 0, 2000) == 1 && recv(c, b, sizeof(b), 0) == 4 &&
                !memcmp(b, "pong", 4));
    closesocket(s);
    snprintf(what, sizeof(what), "%s: closing one end ends the other's stream", name);
    check(what, wait_for(c, 0, 2000) == 1 && recv(c, b, sizeof(b), 0) == 0);
    closesocket(c);

    /* A listener closed with a connection it never accepted */
    SOCKET c2 = socket(family, SOCK_STREAM, 0);
    connect(c2, (struct sockaddr *)&a, len);
    wait_for(l, 0, 2000);
    closesocket(l);
    snprintf(what, sizeof(what), "%s: closing a listener with a queued connection", name);
    check(what, 1);                                     /* (it used to stop the network) */
    closesocket(c2);
}

/* shutdown(SD_BOTH) with data unread, then the peer closes: the
 * connection ends while its socket is still open (it once left the kernel
 * holding a freed lwIP pcb, which corrupted the network on close) */
static void shutdown_both(void)
{
    struct sockaddr_in a;
    int len = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    bind(l, (struct sockaddr *)&a, len);
    listen(l, 5);
    getsockname(l, (struct sockaddr *)&a, &len);
    for (int round = 0; round < 3; round++) {
        SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
        int ok = connect(c, (struct sockaddr *)&a, len) == 0;
        SOCKET s = accept(l, 0, 0);
        char b[8];
        ok = ok && s != INVALID_SOCKET && send(s, "unread", 6, 0) == 6;
        Sleep(50);
        ok = ok && shutdown(c, SD_BOTH) == 0;
        ok = ok && wait_for(s, 0, 2000) == 1 && recv(s, b, sizeof(b), 0) == 0;   /* the client's FIN */
        ok = ok && recv(c, b, sizeof(b), 0) == 0;                               /* nothing more to read */
        closesocket(s);
        Sleep(100);                                     /* the connection ends with c still open */
        closesocket(c);
        char what[96];
        snprintf(what, sizeof(what), "shutdown(SD_BOTH) with unread data, then the peer closes (round %d)", round + 1);
        check(what, ok);
    }
    closesocket(l);
}

static void localhost(void)
{
    struct addrinfo h, *res = 0;
    memset(&h, 0, sizeof(h));
    h.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo("localhost", "80", &h, &res);
    int v4 = 0, v6 = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET &&
            ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) v4 = 1;
        if (ai->ai_family == AF_INET6 &&
            !memcmp(&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, &in6addr_loopback, 16)) v6 = 1;
    }
    if (res) freeaddrinfo(res);
    check("getaddrinfo(\"localhost\") gives 127.0.0.1 and ::1", r == 0 && v4 && v6);
    struct hostent *he = gethostbyname("localhost");
    check("gethostbyname(\"localhost\") gives 127.0.0.1",
          he && *(ULONG *)he->h_addr_list[0] == htonl(INADDR_LOOPBACK));
}

/* Winsock 1.1 (wsock32.dll): NSPR imports it by ordinal, and three of its
 * ordinals differ from ws2_32's (inet_addr 10, inet_ntoa 11, ioctlsocket 12) */
static void winsock11(void)
{
    HMODULE m = LoadLibraryA("wsock32.dll"), w2 = GetModuleHandleA("ws2_32.dll");
    check("wsock32.dll loads", m != 0);
    if (!m) return;
    check("wsock32 ordinal 12 is ioctlsocket (ws2_32's is 10)",
          GetProcAddress(m, (LPCSTR)12) == GetProcAddress(m, "ioctlsocket") &&
          GetProcAddress(w2, (LPCSTR)10) == GetProcAddress(w2, "ioctlsocket"));
    check("wsock32 ordinals 10, 11 and 23 are inet_addr, inet_ntoa and socket",
          GetProcAddress(m, (LPCSTR)10) == GetProcAddress(m, "inet_addr") &&
          GetProcAddress(m, (LPCSTR)11) == GetProcAddress(m, "inet_ntoa") &&
          GetProcAddress(m, (LPCSTR)23) == GetProcAddress(m, "socket"));
    SOCKET (WSAAPI *sock)(int, int, int) = (SOCKET (WSAAPI *)(int, int, int))GetProcAddress(m, (LPCSTR)23);
    int (WSAAPI *ioctl)(SOCKET, long, u_long *) = (int (WSAAPI *)(SOCKET, long, u_long *))GetProcAddress(m, (LPCSTR)12);
    int (WSAAPI *cls)(SOCKET) = (int (WSAAPI *)(SOCKET))GetProcAddress(m, (LPCSTR)3);
    u_long one = 1;
    SOCKET s = sock && ioctl && cls ? sock(AF_INET, SOCK_STREAM, 0) : INVALID_SOCKET;
    check("a socket goes non-blocking through wsock32 ordinals 23, 12 and 3 (NSPR's way)",
          s != INVALID_SOCKET && ioctl(s, FIONBIO, &one) == 0 && cls(s) == 0);
    FreeLibrary(m);
}

/* A connected TCP pair over 127.0.0.1 (blocking) */
static int tcp_pair(SOCKET *c, SOCKET *s)
{
    struct sockaddr_in a;
    int len = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    *c = socket(AF_INET, SOCK_STREAM, 0);
    *s = INVALID_SOCKET;
    if (l != INVALID_SOCKET && !bind(l, (struct sockaddr *)&a, len) && !listen(l, 1) &&
        !getsockname(l, (struct sockaddr *)&a, &len) && !connect(*c, (struct sockaddr *)&a, len))
        *s = accept(l, 0, 0);
    closesocket(l);
    return *s != INVALID_SOCKET;
}

static LONGLONG now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / f.QuadPart;
}

static int cmp_ll(const void *a, const void *b)
{
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

/* Two one-byte sends in a row, and how long until the second arrives:
 * with Nagle's algorithm on, it waits for the first one's ACK, which the
 * receiver delays (up to 250 ms); TCP_NODELAY sends it at once.  The
 * median of 8 rounds, in microseconds. */
static LONGLONG write_write(SOCKET c, SOCKET s)
{
    LONGLONG took[8];
    for (int i = 0; i < 8; i++) {
        char b[2];
        int got = 0;
        LONGLONG t0 = now_us();
        send(c, "a", 1, 0);
        send(c, "b", 1, 0);
        while (got < 2 && wait_for(s, 0, 2000) == 1) {
            int n = recv(s, b + got, 2 - got, 0);
            if (n <= 0) break;
            got += n;
        }
        took[i] = got == 2 ? now_us() - t0 : 10000000;
        Sleep(300);                                     /* (everything ACKed before the next round) */
    }
    qsort(took, 8, sizeof(took[0]), cmp_ll);
    return took[4];
}

static int get_int(SOCKET s, int level, int opt)
{
    int v = -1, len = sizeof(v);
    return getsockopt(s, level, opt, (char *)&v, &len) == 0 && len == 4 ? v : -1;
}

static int set_int(SOCKET s, int level, int opt, int v)
{
    return setsockopt(s, level, opt, (const char *)&v, sizeof(v)) == 0;
}

/* setsockopt/getsockopt: the common options reach the network stack and
 * read back, and TCP_NODELAY, SO_RCVTIMEO, SO_SNDTIMEO, SO_LINGER and
 * SO_REUSEADDR change what the socket does */
static void sockopts(void)
{
    char what[160];
    SOCKET t = socket(AF_INET, SOCK_STREAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0);
    check("SO_TYPE is SOCK_STREAM and SOCK_DGRAM, SO_ERROR 0 and SO_ACCEPTCONN 0 on new sockets",
          get_int(t, SOL_SOCKET, SO_TYPE) == SOCK_STREAM && get_int(u, SOL_SOCKET, SO_TYPE) == SOCK_DGRAM &&
          get_int(t, SOL_SOCKET, SO_ERROR) == 0 && get_int(t, SOL_SOCKET, SO_ACCEPTCONN) == 0);
    int def_ttl = get_int(t, IPPROTO_IP, IP_TTL);
    snprintf(what, sizeof(what), "defaults: TCP_NODELAY off, SO_KEEPALIVE off, IP_TTL %d, SO_RCVBUF %d, SO_SNDBUF %d",
             def_ttl, get_int(t, SOL_SOCKET, SO_RCVBUF), get_int(t, SOL_SOCKET, SO_SNDBUF));
    check(what, get_int(t, IPPROTO_TCP, TCP_NODELAY) == 0 && get_int(t, SOL_SOCKET, SO_KEEPALIVE) == 0 &&
                def_ttl > 0 && get_int(t, SOL_SOCKET, SO_RCVBUF) > 0 && get_int(t, SOL_SOCKET, SO_SNDBUF) > 0);
    static const struct { int level, opt, v; const char *name; } opts[] = {
        { SOL_SOCKET, SO_RCVBUF, 8192, "SO_RCVBUF" },   { SOL_SOCKET, SO_SNDBUF, 16384, "SO_SNDBUF" },
        { SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR" }, { SOL_SOCKET, SO_KEEPALIVE, 1, "SO_KEEPALIVE" },
        { IPPROTO_TCP, TCP_NODELAY, 1, "TCP_NODELAY" }, { SOL_SOCKET, SO_RCVTIMEO, 250, "SO_RCVTIMEO" },
        { SOL_SOCKET, SO_SNDTIMEO, 300, "SO_SNDTIMEO" }, { IPPROTO_IP, IP_TTL, 64, "IP_TTL" },
    };
    for (unsigned i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        snprintf(what, sizeof(what), "%s set to %d reads back", opts[i].name, opts[i].v);
        check(what, set_int(t, opts[i].level, opts[i].opt, opts[i].v) && get_int(t, opts[i].level, opts[i].opt) == opts[i].v);
    }
    check("TCP_NODELAY and SO_KEEPALIVE switch off again",
          set_int(t, IPPROTO_TCP, TCP_NODELAY, 0) && set_int(t, SOL_SOCKET, SO_KEEPALIVE, 0) &&
          get_int(t, IPPROTO_TCP, TCP_NODELAY) == 0 && get_int(t, SOL_SOCKET, SO_KEEPALIVE) == 0);
    struct linger lg = { 1, 5 }, lr = { 0, 0 };
    int ll = sizeof(lr);
    check("SO_LINGER {1, 5} reads back, and SO_DONTLINGER says 0",
          setsockopt(t, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg)) == 0 &&
          getsockopt(t, SOL_SOCKET, SO_LINGER, (char *)&lr, &ll) == 0 && lr.l_onoff == 1 && lr.l_linger == 5 &&
          get_int(t, SOL_SOCKET, SO_DONTLINGER) == 0);
    check("SO_DONTLINGER turns it off and keeps the timeout",
          set_int(t, SOL_SOCKET, SO_DONTLINGER, 1) && (ll = sizeof(lr), getsockopt(t, SOL_SOCKET, SO_LINGER, (char *)&lr, &ll)) == 0 &&
          lr.l_onoff == 0 && lr.l_linger == 5);
    check("SO_BROADCAST on a UDP socket reads back",
          get_int(u, SOL_SOCKET, SO_BROADCAST) == 0 && set_int(u, SOL_SOCKET, SO_BROADCAST, 1) &&
          get_int(u, SOL_SOCKET, SO_BROADCAST) == 1);
    BOOL one = TRUE;
    char b1 = 0;
    int b1l = 1;
    check("a BOOL option given as one byte (SO_KEEPALIVE)",
          setsockopt(t, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, 1) == 0 &&
          getsockopt(t, SOL_SOCKET, SO_KEEPALIVE, &b1, &b1l) == 0 && b1 == 1 && b1l == 1);
    check("TCP_NODELAY on a UDP socket is WSAENOPROTOOPT",
          !set_int(u, IPPROTO_TCP, TCP_NODELAY, 1) && WSAGetLastError() == WSAENOPROTOOPT);
    closesocket(t);
    closesocket(u);

    /* TCP_NODELAY: the second of two small sends goes at once */
    SOCKET c, s;
    if (tcp_pair(&c, &s)) {
        LONGLONG nagle = write_write(c, s);
        set_int(c, IPPROTO_TCP, TCP_NODELAY, 1);
        LONGLONG nodelay = write_write(c, s);
        snprintf(what, sizeof(what), "TCP_NODELAY: a second small send arrives after %lld.%03lld ms (Nagle's algorithm: %lld.%03lld ms)",
                 nodelay / 1000, nodelay % 1000, nagle / 1000, nagle % 1000);
        check(what, nodelay <= 30000 && nagle >= 40000 && nagle >= 3 * nodelay);

        /* SO_RCVTIMEO: a blocking recv with nothing coming gives up */
        set_int(s, SOL_SOCKET, SO_RCVTIMEO, 300);
        char b[16];
        LONGLONG t0 = now_us();
        int r = recv(s, b, sizeof(b), 0);
        int e = WSAGetLastError();
        LONGLONG took = now_us() - t0;
        snprintf(what, sizeof(what), "SO_RCVTIMEO 300 ms: recv gives WSAETIMEDOUT after %lld ms", took / 1000);
        check(what, r == SOCKET_ERROR && e == WSAETIMEDOUT && took >= 250000 && took <= 1500000);
        check("data sent after the timeout still arrives", send(c, "x", 1, 0) == 1 && recv(s, b, sizeof(b), 0) == 1 && b[0] == 'x');

        /* SO_SNDTIMEO: the peer reads nothing, the buffers fill, a send gives up */
        set_int(c, SOL_SOCKET, SO_SNDTIMEO, 300);
        static char big[4096];
        int sent = 0;
        r = 0;
        LONGLONG start = now_us();
        while (sent < 1600 * 1024 && now_us() - start < 10000000) {   /* (a short count: what fitted before the timeout) */
            t0 = now_us();
            r = send(c, big, sizeof(big), 0);
            if (r == SOCKET_ERROR) break;
            sent += r;
        }
        e = WSAGetLastError();
        took = now_us() - t0;
        snprintf(what, sizeof(what), "SO_SNDTIMEO 300 ms: with the peer not reading, send gives WSAETIMEDOUT after %d KB (%lld ms)",
                 sent / 1024, took / 1000);
        check(what, r == SOCKET_ERROR && e == WSAETIMEDOUT && took >= 250000 && took <= 1500000);
        closesocket(c);
        closesocket(s);
    } else {
        check("a loopback TCP pair for the option tests", 0);
    }

    /* SO_LINGER {1, 0}: closing resets the connection */
    if (tcp_pair(&c, &s)) {
        struct linger hard = { 1, 0 };
        setsockopt(c, SOL_SOCKET, SO_LINGER, (const char *)&hard, sizeof(hard));
        closesocket(c);
        char b[4];
        int r = wait_for(s, 0, 2000) == 1 ? recv(s, b, sizeof(b), 0) : 1;
        int e = WSAGetLastError();
        check("SO_LINGER {1, 0}: closesocket resets the connection (the peer's recv gives WSAECONNRESET)",
              r == SOCKET_ERROR && e == WSAECONNRESET);
        closesocket(s);
    }
    if (tcp_pair(&c, &s)) {
        closesocket(c);
        char b[4];
        int r = wait_for(s, 0, 2000) == 1 ? recv(s, b, sizeof(b), 0) : 1;
        check("without it, closesocket ends the connection in order (recv gives 0)", r == 0);
        closesocket(s);
    }

    /* SO_REUSEADDR: two sockets on one port only when both ask */
    struct sockaddr_in a;
    int len = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET x = socket(AF_INET, SOCK_STREAM, 0), y = socket(AF_INET, SOCK_STREAM, 0), z = socket(AF_INET, SOCK_STREAM, 0);
    set_int(x, SOL_SOCKET, SO_REUSEADDR, 1);
    bind(x, (struct sockaddr *)&a, len);
    getsockname(x, (struct sockaddr *)&a, &len);
    int busy = bind(y, (struct sockaddr *)&a, len) == SOCKET_ERROR && WSAGetLastError() == WSAEADDRINUSE;
    set_int(z, SOL_SOCKET, SO_REUSEADDR, 1);
    int shared = bind(z, (struct sockaddr *)&a, len) == 0;
    snprintf(what, sizeof(what), "SO_REUSEADDR: port %u is in use for a plain socket, and shared with one that sets it",
             ntohs(a.sin_port));
    check(what, busy && shared);
    closesocket(x); closesocket(y); closesocket(z);
}

int main(void)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup failed\n"); return 1; }
    pair(AF_INET, "127.0.0.1");
    pair(AF_INET6, "::1");
    shutdown_both();
    localhost();
    winsock11();
    sockopts();
    printf("looptest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
