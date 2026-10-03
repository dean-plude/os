/* overlaptest.exe — overlapped sockets on an I/O completion port, the way
 * proactor event loops (Python's asyncio, libuv, Boost.Asio) use them:
 * the extension functions from WSAIoctl (AcceptEx, ConnectEx...), an
 * AcceptEx and a WSARecv that stay pending until a connection or data
 * comes, their completions on the port, CancelIoEx, and a request
 * pending on a socket that is closed (its completion still reaches the
 * port, even after the handle value is reused). */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* mswsock.h's extension functions */
typedef BOOL (WINAPI *LPFN_ACCEPTEX)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *LPFN_CONNECTEX)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef VOID (WINAPI *LPFN_GETACCEPTEXSOCKADDRS)(PVOID, DWORD, DWORD, DWORD, struct sockaddr **, LPINT, struct sockaddr **, LPINT);
#define WSAID_ACCEPTEX { 0xb5367df1, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } }
#define WSAID_CONNECTEX { 0x25a207b9, 0xddf3, 0x4660, { 0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e } }
#define WSAID_DISCONNECTEX { 0x7fda2e11, 0x8630, 0x436f, { 0xa0, 0x31, 0xf5, 0x36, 0xa6, 0xee, 0xc1, 0x57 } }
#define WSAID_GETACCEPTEXSOCKADDRS { 0xb5367df2, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } }
#define WSAID_TRANSMITFILE { 0xb5367df0, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } }
#define WSAID_WSARECVMSG { 0xf689d7c8, 0x6f1f, 0x436b, { 0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22 } }
#ifndef SO_UPDATE_ACCEPT_CONTEXT
#define SO_UPDATE_ACCEPT_CONTEXT  0x700B
#define SO_UPDATE_CONNECT_CONTEXT 0x7010
#endif

static int passed, failed;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

static LPFN_ACCEPTEX p_AcceptEx;
static LPFN_CONNECTEX p_ConnectEx;
static LPFN_GETACCEPTEXSOCKADDRS p_GetAcceptExSockaddrs;

static void *ext(SOCKET s, GUID id)
{
    void *f = 0;
    DWORD n = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &f, sizeof(f), &n, 0, 0)) return 0;
    return n == sizeof(f) ? f : 0;
}

/* The next completion on @port: its OVERLAPPED, key, bytes and error (0 = success) */
static OVERLAPPED *next(HANDLE port, ULONG_PTR *key, DWORD *bytes, DWORD *err, DWORD ms)
{
    OVERLAPPED *ov = 0;
    *err = GetQueuedCompletionStatus(port, bytes, key, &ov, ms) ? 0 : GetLastError();
    return ov;
}

/* A pending receive whose WSABUF lives on this function's stack only */
static int recv_pending(SOCKET s, char *buf, int len, OVERLAPPED *ov)
{
    WSABUF b = { (ULONG)len, buf };
    DWORD got = 0, flags = 0;
    memset(ov, 0, sizeof(*ov));
    int r = WSARecv(s, &b, 1, &got, &flags, ov, 0);
    return r == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING;
}

static void scribble(void)
{
    volatile char junk[512];
    for (int i = 0; i < (int)sizeof(junk); i++) junk[i] = (char)0xEE;
}

int main(void)
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    check("CreateIoCompletionPort", port != 0);

    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    GUID g_accept = WSAID_ACCEPTEX, g_connect = WSAID_CONNECTEX, g_disc = WSAID_DISCONNECTEX,
         g_addrs = WSAID_GETACCEPTEXSOCKADDRS, g_tf = WSAID_TRANSMITFILE, g_rmsg = WSAID_WSARECVMSG;
    p_AcceptEx = ext(l, g_accept);
    p_ConnectEx = ext(l, g_connect);
    p_GetAcceptExSockaddrs = ext(l, g_addrs);
    check("WSAIoctl gives AcceptEx, ConnectEx and GetAcceptExSockaddrs", p_AcceptEx && p_ConnectEx && p_GetAcceptExSockaddrs);
    check("WSAIoctl gives DisconnectEx, TransmitFile and WSARecvMsg", ext(l, g_disc) && ext(l, g_tf) && ext(l, g_rmsg));
    if (!p_AcceptEx || !p_ConnectEx || !p_GetAcceptExSockaddrs) goto done;

    /* a listener on the port; AcceptEx waits for a connection */
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 4);
    int alen = sizeof(a);
    getsockname(l, (struct sockaddr *)&a, &alen);
    CreateIoCompletionPort((HANDLE)l, port, 1, 0);
    SOCKET acc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    char abuf[2 * (sizeof(struct sockaddr_in6) + 16)];
    OVERLAPPED aov = { 0 };
    DWORD got = 0;
    BOOL r = p_AcceptEx(l, acc, abuf, 0, sizeof(struct sockaddr_in6) + 16, sizeof(struct sockaddr_in6) + 16, &got, &aov);
    check("AcceptEx with no connection yet is pending", !r && WSAGetLastError() == ERROR_IO_PENDING);

    /* a non-blocking client connects with ConnectEx */
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(c, FIONBIO, &nb);
    struct sockaddr_in any = { 0 };
    any.sin_family = AF_INET;
    bind(c, (struct sockaddr *)&any, sizeof(any));
    CreateIoCompletionPort((HANDLE)c, port, 2, 0);
    OVERLAPPED cov = { 0 };
    r = p_ConnectEx(c, (struct sockaddr *)&a, sizeof(a), 0, 0, 0, &cov);
    check("ConnectEx starts", r || WSAGetLastError() == ERROR_IO_PENDING);

    int seen_accept = 0, seen_connect = 0;
    for (int i = 0; i < 2; i++) {
        ULONG_PTR key; DWORD bytes, err;
        OVERLAPPED *ov = next(port, &key, &bytes, &err, 5000);
        if (ov == &aov && key == 1 && !err) seen_accept = 1;
        if (ov == &cov && key == 2 && !err) seen_connect = 1;
    }
    check("AcceptEx completes on the listener's port", seen_accept);
    check("ConnectEx completes on the client's port", seen_connect);
    setsockopt(acc, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&l, sizeof(l));
    setsockopt(c, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, 0, 0);
    struct sockaddr *la, *ra;
    int lal, ral;
    p_GetAcceptExSockaddrs(abuf, 0, sizeof(struct sockaddr_in6) + 16, sizeof(struct sockaddr_in6) + 16, &la, &lal, &ra, &ral);
    struct sockaddr_in cn = { 0 };
    int cnl = sizeof(cn);
    getsockname(c, (struct sockaddr *)&cn, &cnl);
    check("GetAcceptExSockaddrs: the client's address",
          ra->sa_family == AF_INET && ((struct sockaddr_in *)ra)->sin_port == cn.sin_port &&
          la->sa_family == AF_INET && ((struct sockaddr_in *)la)->sin_port == a.sin_port);
    CreateIoCompletionPort((HANDLE)acc, port, 3, 0);
    ioctlsocket(acc, FIONBIO, &nb);

    /* a receive with nothing to read stays pending, then gets the data */
    char rbuf[16] = { 0 };
    OVERLAPPED rov;
    check("WSARecv with nothing to read is pending", recv_pending(acc, rbuf, sizeof(rbuf), &rov));
    scribble();                                       /* the WSABUF is gone; the buffer is not */
    ULONG_PTR key; DWORD bytes, err;
    OVERLAPPED *ov = next(port, &key, &bytes, &err, 300);
    check("nothing completes before data comes", ov == 0 && err == WAIT_TIMEOUT);
    send(c, "ping", 4, 0);
    ov = next(port, &key, &bytes, &err, 5000);
    check("the pending WSARecv completes with the data", ov == &rov && key == 3 && !err && bytes == 4 && !memcmp(rbuf, "ping", 4));

    /* CancelIoEx ends a pending receive with ERROR_OPERATION_ABORTED */
    check("another WSARecv is pending", recv_pending(acc, rbuf, sizeof(rbuf), &rov));
    check("CancelIoEx", CancelIoEx((HANDLE)acc, &rov));
    ov = next(port, &key, &bytes, &err, 5000);
    check("the cancelled WSARecv completes aborted", ov == &rov && err == ERROR_OPERATION_ABORTED);

    /* closing a socket with a receive pending: the completion still comes,
     * to this port, though the handle value is reused at once */
    check("a WSARecv on the client is pending", recv_pending(c, rbuf, sizeof(rbuf), &rov));
    closesocket(c);
    HANDLE ev = CreateEventW(0, TRUE, FALSE, 0);     /* likely the closed socket's value */
    CloseHandle(ev);
    ov = next(port, &key, &bytes, &err, 5000);
    check("closesocket completes the pending WSARecv aborted", ov == &rov && key == 2 && err == ERROR_OPERATION_ABORTED);

    /* the peer is gone: a pending receive on the accepted socket ends with 0 bytes */
    char eof[4];
    OVERLAPPED eov;
    memset(&eov, 0, sizeof(eov));
    WSABUF eb = { sizeof(eof), eof };
    DWORD eg = 0, ef = 0;
    int er = WSARecv(acc, &eb, 1, &eg, &ef, &eov, 0);
    ov = next(port, &key, &bytes, &err, 5000);
    check("a receive after the peer closed completes with 0 bytes",
          (er == 0 || WSAGetLastError() == WSA_IO_PENDING) && ov == &eov && !err && bytes == 0);
    closesocket(acc);
    closesocket(l);

done:
    printf("overlaptest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
