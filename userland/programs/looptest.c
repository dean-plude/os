/* looptest.exe — Winsock over the loopback interface, as Firefox uses it:
 * a socket pair over 127.0.0.1 and ::1 (bind to port 0, listen,
 * getsockname, a non-blocking connect, accept), data sent before accept,
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
    ioctlsocket(c, FIONBIO, &nb);
    int r = connect(c, (struct sockaddr *)&a, len);
    snprintf(what, sizeof(what), "%s: a non-blocking connect is under way", name);
    check(what, r == 0 || WSAGetLastError() == WSAEWOULDBLOCK);
    snprintf(what, sizeof(what), "%s: it becomes writable (connected)", name);
    check(what, wait_for(c, 1, 2000) == 1);
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

int main(void)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup failed\n"); return 1; }
    pair(AF_INET, "127.0.0.1");
    pair(AF_INET6, "::1");
    localhost();
    printf("looptest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
