/* afdtest.exe — polling sockets through a \Device\Afd helper handle, the
 * way wepoll (the epoll behind Poco's PollSet, which GOG Galaxy's client
 * service uses, libevent and others) does it: SIO_BASE_HANDLE, a helper
 * opened with NtCreateFile("\Device\Afd\Wepoll") and bound to a completion
 * port, IOCTL_AFD_POLL requests through NtDeviceIoControlFile that stay
 * pending until a socket is ready (accept, receive, send, the peer
 * closing, the socket closed here, a time-out) and finish on the port
 * with the request's ApcContext, and NtCancelIoFileEx.  Also the keyed
 * events wepoll's reference locks wait on (NtCreateKeyedEvent,
 * NtWaitForKeyedEvent, NtReleaseKeyedEvent), and a UDP socket bound to
 * 127.0.0.1 or ::1 waking its own poll, as Poco's PollSet::wakeUp does. */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winternl.h>

#define IOCTL_AFD_POLL        0x00012024
#define AFD_POLL_RECEIVE      0x0001
#define AFD_POLL_SEND         0x0004
#define AFD_POLL_DISCONNECT   0x0008
#define AFD_POLL_ABORT        0x0010
#define AFD_POLL_LOCAL_CLOSE  0x0020
#define AFD_POLL_ACCEPT       0x0080
#define AFD_POLL_CONNECT_FAIL 0x0100
#define AFD_ALL (AFD_POLL_RECEIVE | AFD_POLL_SEND | AFD_POLL_DISCONNECT | AFD_POLL_ABORT | \
                 AFD_POLL_LOCAL_CLOSE | AFD_POLL_ACCEPT | AFD_POLL_CONNECT_FAIL)
#define SIO_BASE_HANDLE_      0x48000022
#define ST_PENDING            0x00000103L
#define ST_INVALID_HANDLE     ((LONG)0xC0000008)
#define ST_INVALID_DEVICE_REQ ((LONG)0xC0000010)
#define ST_CANCELLED          ((LONG)0xC0000120)
#define ST_NOT_FOUND          ((LONG)0xC0000225)

typedef struct { HANDLE Handle; ULONG Events; LONG Status; } AFD_POLL_HANDLE_INFO;
typedef struct { LARGE_INTEGER Timeout; ULONG NumberOfHandles; ULONG Exclusive; AFD_POLL_HANDLE_INFO Handles[1]; } AFD_POLL_INFO;

typedef LONG (NTAPI *NtCreateFile_t)(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, LARGE_INTEGER *,
                                     ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef LONG (NTAPI *NtDeviceIoControlFile_t)(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, ULONG, PVOID, ULONG, PVOID, ULONG);
typedef LONG (NTAPI *NtCancelIoFileEx_t)(HANDLE, IO_STATUS_BLOCK *, IO_STATUS_BLOCK *);
static NtCreateFile_t p_NtCreateFile;
static NtDeviceIoControlFile_t p_NtDeviceIoControlFile;
static NtCancelIoFileEx_t p_NtCancelIoFileEx;

typedef LONG (NTAPI *NtCreateKeyedEvent_t)(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, ULONG);
typedef LONG (NTAPI *NtKeyedEvent_t)(HANDLE, PVOID, BOOLEAN, LARGE_INTEGER *);
static NtCreateKeyedEvent_t p_NtCreateKeyedEvent;
static NtKeyedEvent_t p_NtWaitForKeyedEvent, p_NtReleaseKeyedEvent;

static int passed, failed;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

/* One request: its status block (also its ApcContext, as wepoll) and info */
typedef struct { IO_STATUS_BLOCK io; AFD_POLL_INFO info; } Req;

static LONG poll_start(HANDLE afd, Req *r, SOCKET s, ULONG events, LONGLONG timeout)
{
    memset(r, 0, sizeof(*r));
    r->info.Timeout.QuadPart = timeout;
    r->info.NumberOfHandles = 1;
    r->info.Exclusive = FALSE;
    r->info.Handles[0].Handle = (HANDLE)s;
    r->info.Handles[0].Events = events;
    r->io.Status = ST_PENDING;
    return p_NtDeviceIoControlFile(afd, 0, 0, &r->io, &r->io, IOCTL_AFD_POLL,
                                   &r->info, sizeof(r->info), &r->info, sizeof(r->info));
}

/* The next packet on @port: its OVERLAPPED (the ApcContext), status and bytes; NULL on a time-out */
static void *next(HANDLE port, LONG *status, DWORD *bytes, ULONG_PTR *key, DWORD ms)
{
    OVERLAPPED_ENTRY e;
    ULONG n = 0;
    if (!GetQueuedCompletionStatusEx(port, &e, 1, &n, ms, FALSE) || n != 1) return 0;
    *status = (LONG)e.Internal;
    *bytes = e.dwNumberOfBytesTransferred;
    *key = e.lpCompletionKey;
    return e.lpOverlapped;
}

/* A thread that waits on a key, then says how it went */
typedef struct { HANDLE ke; void *key; volatile LONG result; } KeyedArg;
static DWORD WINAPI keyed_waiter(LPVOID p)
{
    KeyedArg *a = p;
    a->result = p_NtWaitForKeyedEvent(a->ke, a->key, FALSE, 0);
    return 0;
}

static void keyed_tests(void)
{
    HANDLE ke = 0;
    check("NtCreateKeyedEvent", p_NtCreateKeyedEvent && p_NtCreateKeyedEvent(&ke, 0xF0003, 0, 0) == 0 && ke);
    if (!ke) return;
    static int k1, k2;
    KeyedArg a = { ke, &k1, -1 };
    HANDLE t = CreateThread(0, 0, keyed_waiter, &a, 0, 0);
    Sleep(100);
    check("a waiter waits until its key is released", a.result == -1);
    check("NtReleaseKeyedEvent meets the waiter", p_NtReleaseKeyedEvent(ke, &k1, FALSE, 0) == 0);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    check("... which then returns STATUS_SUCCESS", a.result == 0);
    LARGE_INTEGER to;
    to.QuadPart = -50 * 10000LL;
    check("a release nobody waits for times out", p_NtReleaseKeyedEvent(ke, &k2, FALSE, &to) == 0x102);
    check("a wait on another key times out", p_NtWaitForKeyedEvent(ke, &k2, FALSE, &to) == 0x102);
    check("an odd key is refused", p_NtWaitForKeyedEvent(ke, (char *)&k1 + 1, FALSE, &to) == (LONG)0xC00000EF);
    KeyedArg b = { 0, &k2, -1 };                         /* the process's own (NULL handle) */
    t = CreateThread(0, 0, keyed_waiter, &b, 0, 0);
    Sleep(100);
    LONG r = p_NtReleaseKeyedEvent(0, &k2, FALSE, 0);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    check("the NULL handle's keyed event works too", r == 0 && b.result == 0);
    CloseHandle(ke);
}

static HANDLE afd_open(void)
{
    static WCHAR name[] = L"\\Device\\Afd\\Wepoll";
    UNICODE_STRING us = { sizeof(name) - 2, sizeof(name), name };
    OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, &us, 0, 0, 0 };
    IO_STATUS_BLOCK io;
    HANDLE h = 0;
    LONG st = p_NtCreateFile(&h, SYNCHRONIZE, &oa, &io, 0, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 1 /* FILE_OPEN */, 0, 0, 0);
    return st >= 0 ? h : 0;
}

int main(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    p_NtCreateFile = (NtCreateFile_t)GetProcAddress(nt, "NtCreateFile");
    p_NtDeviceIoControlFile = (NtDeviceIoControlFile_t)GetProcAddress(nt, "NtDeviceIoControlFile");
    p_NtCancelIoFileEx = (NtCancelIoFileEx_t)GetProcAddress(nt, "NtCancelIoFileEx");
    p_NtCreateKeyedEvent = (NtCreateKeyedEvent_t)GetProcAddress(nt, "NtCreateKeyedEvent");
    p_NtWaitForKeyedEvent = (NtKeyedEvent_t)GetProcAddress(nt, "NtWaitForKeyedEvent");
    p_NtReleaseKeyedEvent = (NtKeyedEvent_t)GetProcAddress(nt, "NtReleaseKeyedEvent");
    keyed_tests();
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);

    /* a listener on 127.0.0.1 */
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 4);
    int alen = sizeof(a);
    getsockname(l, (struct sockaddr *)&a, &alen);

    SOCKET base = INVALID_SOCKET;
    DWORD got = 0;
    int r = WSAIoctl(l, SIO_BASE_HANDLE_, 0, 0, &base, sizeof(base), &got, 0, 0);
    check("SIO_BASE_HANDLE gives the socket itself", r == 0 && base == l && got == sizeof(base));

    HANDLE afd = afd_open();
    check("NtCreateFile opens \\Device\\Afd\\Wepoll", afd != 0);
    if (!afd) goto done;
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    check("the helper binds to a completion port", CreateIoCompletionPort(afd, port, 7, 0) == port);
    check("SetFileCompletionNotificationModes on the helper", SetFileCompletionNotificationModes(afd, 2 /* FILE_SKIP_SET_EVENT_ON_HANDLE */));

    IO_STATUS_BLOCK cio;
    Req other;
    memset(&other, 0, sizeof(other));
    check("an unknown control is an invalid device request",
          p_NtDeviceIoControlFile(afd, 0, 0, &other.io, &other.io, 0x00012003, 0, 0, 0, 0) == ST_INVALID_DEVICE_REQ);
    check("polling a handle that is not a socket fails", poll_start(afd, &other, (SOCKET)afd, AFD_ALL, -1) == ST_INVALID_HANDLE);

    /* accept: pending until a client connects */
    Req ra;
    LONG st = poll_start(afd, &ra, l, AFD_ALL & ~AFD_POLL_SEND, 0x7FFFFFFFFFFFFFFFLL);
    check("a poll for a connection is pending", st == ST_PENDING && ra.io.Status == ST_PENDING);
    LONG ps; DWORD bytes; ULONG_PTR key;
    check("nothing on the port before the client connects", next(port, &ps, &bytes, &key, 300) == 0);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(c, (struct sockaddr *)&a, sizeof(a));
    void *ctx = next(port, &ps, &bytes, &key, 5000);
    check("the connection finishes the poll on the port (ApcContext, key)", ctx == &ra.io && key == 7 && ps == 0);
    check("... with AFD_POLL_ACCEPT for the listener",
          ra.io.Status == 0 && ra.info.NumberOfHandles == 1 && ra.info.Handles[0].Handle == (HANDLE)l &&
          ra.info.Handles[0].Events == AFD_POLL_ACCEPT);
    check("... and the output's size as its information",
          (DWORD)ra.io.Information == bytes && bytes == (DWORD)(sizeof(LARGE_INTEGER) + 8 + sizeof(AFD_POLL_HANDLE_INFO)));
    SOCKET s = accept(l, 0, 0);

    /* receive: pending until the client sends */
    Req rr;
    st = poll_start(afd, &rr, s, AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT | AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE, 0x7FFFFFFFFFFFFFFFLL);
    check("a poll for data is pending", st == ST_PENDING);
    send(c, "ping", 4, 0);
    ctx = next(port, &ps, &bytes, &key, 5000);
    check("data finishes it with AFD_POLL_RECEIVE", ctx == &rr.io && rr.info.NumberOfHandles == 1 &&
          rr.info.Handles[0].Events == AFD_POLL_RECEIVE);
    char buf[8];
    recv(s, buf, sizeof(buf), 0);

    /* send: ready at once (STATUS_SUCCESS), and a packet all the same */
    Req rs;
    st = poll_start(afd, &rs, c, AFD_POLL_SEND, 0x7FFFFFFFFFFFFFFFLL);
    check("a poll for room to send finishes at once", st == 0 && rs.io.Status == 0 && rs.info.Handles[0].Events == AFD_POLL_SEND);
    ctx = next(port, &ps, &bytes, &key, 5000);
    check("... and still queues its packet", ctx == &rs.io && ps == 0);

    /* a time-out: finished with no handles */
    Req rt;
    DWORD t0 = GetTickCount();
    st = poll_start(afd, &rt, s, AFD_POLL_RECEIVE, -200 * 10000LL);
    ctx = next(port, &ps, &bytes, &key, 5000);
    DWORD took = GetTickCount() - t0;
    check("a relative time-out ends the poll with no handles", st == ST_PENDING && ctx == &rt.io && ps == 0 &&
          rt.info.NumberOfHandles == 0 && took >= 150);
    Req rz;
    st = poll_start(afd, &rz, s, AFD_POLL_RECEIVE, 0);
    ctx = next(port, &ps, &bytes, &key, 5000);
    check("a zero time-out answers at once", st == 0 && ctx == &rz.io && rz.info.NumberOfHandles == 0);

    /* cancel */
    Req rc;
    st = poll_start(afd, &rc, s, AFD_POLL_RECEIVE, 0x7FFFFFFFFFFFFFFFLL);
    LONG cs = p_NtCancelIoFileEx(afd, &rc.io, &cio);
    ctx = next(port, &ps, &bytes, &key, 5000);
    check("NtCancelIoFileEx ends a pending poll", st == ST_PENDING && cs == 0 && ctx == &rc.io && ps == ST_CANCELLED &&
          rc.io.Status == ST_CANCELLED);
    check("cancelling it again: not found", p_NtCancelIoFileEx(afd, &rc.io, &cio) == ST_NOT_FOUND);

    /* the peer closes: AFD_POLL_DISCONNECT */
    Req rd;
    st = poll_start(afd, &rd, s, AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT | AFD_POLL_ABORT, 0x7FFFFFFFFFFFFFFFLL);
    closesocket(c);
    ctx = next(port, &ps, &bytes, &key, 5000);
    check("the peer closing finishes it with AFD_POLL_DISCONNECT",
          ctx == &rd.io && rd.info.NumberOfHandles == 1 && (rd.info.Handles[0].Events & AFD_POLL_DISCONNECT));

    /* a UDP socket bound to 127.0.0.1 sending to itself (Poco's PollSet
     * wakes its poll this way): the datagram finishes the poll */
    for (int v6 = 0; v6 < 2; v6++) {
        struct sockaddr_in6 ua = { 0 };
        int ulen = v6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        if (v6) { ua.sin6_family = AF_INET6; ua.sin6_addr.s6_addr[15] = 1; }
        else { ((struct sockaddr_in *)&ua)->sin_family = AF_INET; ((struct sockaddr_in *)&ua)->sin_addr.s_addr = htonl(INADDR_LOOPBACK); }
        SOCKET w = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(w, (struct sockaddr *)&ua, ulen);
        getsockname(w, (struct sockaddr *)&ua, &ulen);
        Req rw;
        st = poll_start(afd, &rw, w, AFD_POLL_RECEIVE, 0x7FFFFFFFFFFFFFFFLL);
        char one = 1;
        int sent = sendto(w, &one, 1, 0, (struct sockaddr *)&ua, ulen);
        ctx = next(port, &ps, &bytes, &key, 5000);
        check(v6 ? "... and one bound to ::1" : "a UDP socket bound to 127.0.0.1 wakes its own poll",
              st == ST_PENDING && sent == 1 && ctx == &rw.io && (rw.info.Handles[0].Events & AFD_POLL_RECEIVE));
        closesocket(w);
    }

    /* the socket closed here: AFD_POLL_LOCAL_CLOSE */
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Req rl;
    st = poll_start(afd, &rl, u, AFD_POLL_RECEIVE | AFD_POLL_LOCAL_CLOSE, 0x7FFFFFFFFFFFFFFFLL);
    check("a poll on an idle UDP socket is pending", st == ST_PENDING);
    closesocket(u);
    ctx = next(port, &ps, &bytes, &key, 5000);
    check("closing the socket finishes it with AFD_POLL_LOCAL_CLOSE",
          ctx == &rl.io && rl.info.NumberOfHandles == 1 && rl.info.Handles[0].Events == AFD_POLL_LOCAL_CLOSE);

    /* an event instead of a port */
    HANDLE afd2 = afd_open();
    HANDLE ev = CreateEventW(0, TRUE, TRUE, 0);
    SOCKET c2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(c2, (struct sockaddr *)&a, sizeof(a));
    Req re;
    memset(&re, 0, sizeof(re));
    re.info.Timeout.QuadPart = 0x7FFFFFFFFFFFFFFFLL;
    re.info.NumberOfHandles = 1;
    re.info.Handles[0].Handle = (HANDLE)c2;
    re.info.Handles[0].Events = AFD_POLL_SEND;
    st = p_NtDeviceIoControlFile(afd2, ev, 0, 0, &re.io, IOCTL_AFD_POLL, &re.info, sizeof(re.info), &re.info, sizeof(re.info));
    check("an unbound helper with an event: the event is set", st == 0 && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0 &&
          re.info.Handles[0].Events == AFD_POLL_SEND);
    closesocket(c2);
    CloseHandle(ev);
    CloseHandle(afd2);

    /* a pending poll when the helper is closed: the process goes on */
    Req rx;
    SOCKET s2 = accept(l, 0, 0);
    poll_start(afd, &rx, s2, AFD_POLL_RECEIVE, 0x7FFFFFFFFFFFFFFFLL);
    check("closing the helper with a poll pending", CloseHandle(afd));
    Sleep(300);
    closesocket(s2);
    closesocket(s);
    closesocket(l);
    CloseHandle(port);

done:
    printf("afdtest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
