/* loadtest.exe — Winsock under a browser's or a game client's load: many
 * sockets open at once, select and WSAPoll over all of them, a
 * WSAEventSelect registration per socket that ends with closesocket,
 * many UDP sockets, connections opened and closed in quick succession
 * (TIME-WAIT), and, given a host and port, parallel HTTP downloads as
 * Steam's client makes them.  Each of these once ran out of a fixed table
 * (16 TCP and 8 UDP lwIP PCBs, 64 kernel sockets, 64 event registrations
 * that closesocket never freed) and failed with WSAENOBUFS.
 *
 *   loadtest [host port]
 */
#define FD_SETSIZE 1024                 /* as libcurl and others do: select over hundreds */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define PAIRS   200                     /* 400 connected sockets and a listener in one process */
#define UDPS    100
#define CHURN   400
#define DLS     24                      /* parallel downloads */

typedef struct { SOCKET fd; short events, revents; } POLLFD_;
__declspec(dllimport) int WSAAPI WSAPoll(POLLFD_ *fds, ULONG n, INT timeout);

static int passed, failed;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

static SOCKET listener(struct sockaddr_in *a)
{
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)a, sizeof(*a)) || listen(l, 64)) return INVALID_SOCKET;
    int n = sizeof(*a);
    getsockname(l, (struct sockaddr *)a, &n);
    return l;
}

static int wait_one(SOCKET s, int write, int ms)
{
    fd_set f; FD_ZERO(&f); FD_SET(s, &f);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(0, write ? 0 : &f, write ? &f : 0, 0, &tv);
}

static SOCKET cli[PAIRS], srv[PAIRS];

/* PAIRS connections over 127.0.0.1, all open at once */
static int open_pairs(SOCKET l, const struct sockaddr_in *a, int *err)
{
    int n = 0;
    *err = 0;
    for (; n < PAIRS; n++) {
        cli[n] = socket(AF_INET, SOCK_STREAM, 0);
        if (cli[n] == INVALID_SOCKET) { *err = WSAGetLastError(); break; }
        if (connect(cli[n], (const struct sockaddr *)a, sizeof(*a))) { *err = WSAGetLastError(); closesocket(cli[n]); break; }
        srv[n] = accept(l, 0, 0);
        if (srv[n] == INVALID_SOCKET) { *err = WSAGetLastError(); closesocket(cli[n]); break; }
    }
    return n;
}

static void close_pairs(int n)
{
    for (int i = 0; i < n; i++) { closesocket(cli[i]); closesocket(srv[i]); }
}

static void many_sockets(SOCKET l, const struct sockaddr_in *a)
{
    char what[128];
    int err, n = open_pairs(l, a, &err);
    snprintf(what, sizeof(what), "%d connections (%d sockets) open at once (%d made, error %d)", PAIRS, 2 * PAIRS + 1, n, err);
    check(what, n == PAIRS);

    /* every client writes; one select over all the accepted sockets sees them */
    for (int i = 0; i < n; i++) { char m[16]; snprintf(m, sizeof(m), "%d", i); send(cli[i], m, (int)strlen(m) + 1, 0); }
    fd_set *rd = malloc(sizeof(fd_set));
    int seen = 0;
    for (DWORD until = GetTickCount() + 10000; seen < n && GetTickCount() < until; Sleep(10)) {
        FD_ZERO(rd);                    /* (select returns at the first ready: wait until all are) */
        for (int i = 0; i < n; i++) FD_SET(srv[i], rd);
        struct timeval tv = { 2, 0 };
        if (select(0, rd, 0, 0, &tv) >= n) seen = n;
    }
    snprintf(what, sizeof(what), "select over %d sockets reports all %d readable", n, n);
    check(what, seen == n && rd->fd_count == (u_int)n);
    free(rd);

    int good = 0;
    for (int i = 0; i < n; i++) {
        char b[16] = { 0 }, m[16];
        snprintf(m, sizeof(m), "%d", i);
        if (recv(srv[i], b, sizeof(b), 0) == (int)strlen(m) + 1 && !strcmp(b, m) && send(srv[i], b, (int)strlen(b) + 1, 0) > 0) good++;
    }
    snprintf(what, sizeof(what), "each of the %d connections carries its own data (%d did)", n, good);
    check(what, good == n);

    /* WSAPoll over every client: all readable (the echo) */
    POLLFD_ *p = calloc(n, sizeof(POLLFD_));
    for (int i = 0; i < n; i++) { p[i].fd = cli[i]; p[i].events = 0x0100 /* POLLRDNORM */; }
    int r = 0;
    for (DWORD until = GetTickCount() + 10000; r < n && GetTickCount() < until; Sleep(10)) r = WSAPoll(p, (ULONG)n, 2000);
    snprintf(what, sizeof(what), "WSAPoll over %d sockets reports all readable (%d)", n, r);
    check(what, r == n);
    free(p);

    /* a WSAEventSelect registration on every socket, then on a new set
     * after closesocket: registrations end with their sockets */
    WSAEVENT *ev = calloc(n, sizeof(WSAEVENT));
    int reg = 0;
    for (int i = 0; i < n; i++) { ev[i] = WSACreateEvent(); if (WSAEventSelect(cli[i], ev[i], FD_READ | FD_CLOSE) == 0) reg++; }
    snprintf(what, sizeof(what), "WSAEventSelect on %d sockets at once (%d registered)", n, reg);
    check(what, reg == n);
    for (int i = 0; i < n; i++) { char b[16]; recv(cli[i], b, sizeof(b), 0); }
    for (int i = 0; i < n; i++) send(srv[i], "x", 1, 0);
    int fired = 0;
    for (int i = 0; i < n; i++) {
        WSANETWORKEVENTS ne;
        if (WaitForSingleObject(ev[i], 5000) == WAIT_OBJECT_0 && WSAEnumNetworkEvents(cli[i], ev[i], &ne) == 0 &&
            (ne.lNetworkEvents & FD_READ)) fired++;
    }
    snprintf(what, sizeof(what), "every registered socket's event fires on data (%d of %d)", fired, n);
    check(what, fired == n);
    close_pairs(n);
    for (int i = 0; i < n; i++) WSACloseEvent(ev[i]);

    n = open_pairs(l, a, &err);
    reg = 0;
    for (int i = 0; i < n; i++) { ev[i] = WSACreateEvent(); if (WSAEventSelect(cli[i], ev[i], FD_READ) == 0) reg++; }
    snprintf(what, sizeof(what), "after closesocket, %d new sockets register again (%d)", PAIRS, reg);
    check(what, n == PAIRS && reg == n);
    close_pairs(n);
    for (int i = 0; i < n; i++) WSACloseEvent(ev[i]);
    free(ev);
}

static void many_udp(void)
{
    char what[128];
    SOCKET u[UDPS];
    struct sockaddr_in a[UDPS];
    int n = 0, err = 0;
    for (; n < UDPS; n++) {
        u[n] = socket(AF_INET, SOCK_DGRAM, 0);
        if (u[n] == INVALID_SOCKET) { err = WSAGetLastError(); break; }
        memset(&a[n], 0, sizeof(a[n]));
        a[n].sin_family = AF_INET;
        a[n].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(a[n]);
        if (bind(u[n], (struct sockaddr *)&a[n], sizeof(a[n])) || getsockname(u[n], (struct sockaddr *)&a[n], &len)) {
            err = WSAGetLastError(); closesocket(u[n]); break;
        }
    }
    snprintf(what, sizeof(what), "%d UDP sockets open at once (%d made, error %d)", UDPS, n, err);
    check(what, n == UDPS);
    for (int i = 0; i < n; i++) sendto(u[i], (char *)&i, sizeof(i), 0, (struct sockaddr *)&a[(i + 1) % n], sizeof(a[0]));
    int good = 0;
    for (int i = 0; i < n; i++) {
        int v = -1;
        if (wait_one(u[i], 0, 3000) == 1 && recvfrom(u[i], (char *)&v, sizeof(v), 0, 0, 0) == sizeof(v) && v == (i + n - 1) % n) good++;
    }
    snprintf(what, sizeof(what), "a datagram reaches each of them from its neighbour (%d)", good);
    check(what, good == n);
    for (int i = 0; i < n; i++) closesocket(u[i]);
}

/* Short connections one after another: each leaves a PCB in TIME-WAIT,
 * which must make room for the next rather than refuse it */
static void churn(SOCKET l, const struct sockaddr_in *a)
{
    char what[128];
    int ok = 0, err = 0;
    for (int i = 0; i < CHURN; i++) {
        SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
        if (c == INVALID_SOCKET) { err = WSAGetLastError(); break; }
        if (connect(c, (const struct sockaddr *)a, sizeof(*a))) { err = WSAGetLastError(); closesocket(c); break; }
        SOCKET s = accept(l, 0, 0);
        char b = 0;
        if (s != INVALID_SOCKET && send(c, "k", 1, 0) == 1 && recv(s, &b, 1, 0) == 1 && b == 'k') ok++;
        closesocket(c);
        if (s != INVALID_SOCKET) closesocket(s);
    }
    snprintf(what, sizeof(what), "%d connections opened and closed in turn (%d did, error %d)", CHURN, ok, err);
    check(what, ok == CHURN);
}

/* Parallel downloads of /big (300000 bytes, byte i = i*7 % 251) */
static const char *g_host, *g_port;
static volatile LONG g_dl_ok, g_dl_err;

static DWORD WINAPI download(void *arg)
{
    (void)arg;
    struct addrinfo hints = { 0 }, *ai = 0;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host, g_port, &hints, &ai)) { InterlockedExchange(&g_dl_err, WSAGetLastError()); return 0; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET || connect(s, ai->ai_addr, (int)ai->ai_addrlen)) {
        InterlockedExchange(&g_dl_err, WSAGetLastError()); freeaddrinfo(ai); if (s != INVALID_SOCKET) closesocket(s); return 0;
    }
    freeaddrinfo(ai);
    char req[128];
    snprintf(req, sizeof(req), "GET /big HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", g_host);
    send(s, req, (int)strlen(req), 0);
    static __declspec(thread) char buf[16384];
    char head[4] = { 0 };
    long body = -1, got = 0;
    int bad = 0, r;
    while ((r = recv(s, buf, sizeof(buf), 0)) > 0) {
        for (int i = 0; i < r; i++) {
            if (body < 0) {
                head[0] = head[1]; head[1] = head[2]; head[2] = head[3]; head[3] = buf[i];
                if (!memcmp(head, "\r\n\r\n", 4)) body = 0;
                continue;
            }
            if ((unsigned char)buf[i] != (unsigned char)(body * 7 % 251)) bad = 1;
            body++; got++;
        }
    }
    if (r < 0) InterlockedExchange(&g_dl_err, WSAGetLastError());
    closesocket(s);
    if (got == 300000 && !bad) InterlockedIncrement(&g_dl_ok);
    return 0;
}

static void downloads(void)
{
    char what[128];
    HANDLE t[DLS];
    for (int i = 0; i < DLS; i++) t[i] = CreateThread(0, 0, download, 0, 0, 0);
    for (int i = 0; i < DLS; i++) { WaitForSingleObject(t[i], 120000); CloseHandle(t[i]); }
    snprintf(what, sizeof(what), "%d parallel downloads from %s:%s arrive whole (%ld did, last error %ld)",
             DLS, g_host, g_port, g_dl_ok, g_dl_err);
    check(what, g_dl_ok == DLS);
}

int main(int argc, char **argv)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { printf("WSAStartup failed\n"); return 1; }
    struct sockaddr_in a;
    SOCKET l = listener(&a);
    check("a listener on 127.0.0.1", l != INVALID_SOCKET);
    if (l != INVALID_SOCKET) {
        many_sockets(l, &a);
        churn(l, &a);
        closesocket(l);
    }
    many_udp();
    if (argc >= 3) { g_host = argv[1]; g_port = argv[2]; downloads(); }
    printf("loadtest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
