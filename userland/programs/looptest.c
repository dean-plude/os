/* looptest.exe — Winsock over the loopback interface, as Firefox uses it:
 * a socket pair over 127.0.0.1 and ::1 (bind to port 0, listen,
 * getsockname, a non-blocking connect, getpeername, accept), data sent before accept,
 * closing a listener with a connection still queued, and "localhost"
 * resolving to ::1 and 127.0.0.1 without DNS. */
#include <stdio.h>
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

int main(void)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup failed\n"); return 1; }
    pair(AF_INET, "127.0.0.1");
    pair(AF_INET6, "::1");
    shutdown_both();
    localhost();
    winsock11();
    printf("looptest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
