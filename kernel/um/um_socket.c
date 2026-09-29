/*
 * um_socket.c — the kernel half of ws2_32: sockets as handle objects
 *
 * Each socket is a UO_SOCKET object (so it lives in the handle table and is
 * closed by NtClose like any other) wrapping a net/sock.c socket index.
 * User buffers are bounced through kernel memory; blocking calls pass
 * um_stopping as the cancel predicate so a killed program never hangs in
 * the network stack.  The services are NovaOS-private (ws2_32 is built
 * from this header) — real Winsock talks to afd.sys, which NovaOS omits.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../net/sock.h"
#include "../net/net.h"

#define BOUNCE  (16 * 1024)

static bool sock_cancel(void *arg) { (void)arg; return um_stopping(); }

static void sock_destroy(UmObject *o) { NetSockClose(o->sock); }

static int handle_sock(UmProcess *p, UINT64 h)
{
    UmObject *o = um_handle_object(p, h, UO_SOCKET);
    if (!o) return -1;
    int s = o->sock;
    um_ob_unref(o);
    return s;
}

/* NtNovaSocket(type): 0 = TCP, 1 = UDP.  Returns a handle, or 0 on error. */
static UINT64 sys_socket(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    int s = a1 == 1 ? NetSockUdp() : NetSockTcp();
    if (s < 0) return 0;
    UmObject *o = kzalloc(sizeof(*o));
    if (!o) { NetSockClose(s); return 0; }
    o->type = UO_SOCKET;
    o->refs = 1;
    o->sock = s;
    o->destroy = sock_destroy;
    UINT64 hv = um_handle_new_object(p, o);
    um_ob_unref(o);
    return hv;                                   /* 0 if the table was full */
}

static UINT64 sys_connect(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    return (UINT64)(INT64)NetSockConnect(s, (UINT32)a2, (UINT16)a3, sock_cancel, NULL);
}

static UINT64 sys_send(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    int len = (int)a3, total = 0;
    UINT8 *tmp = kmalloc(BOUNCE);
    if (!tmp) return (UINT64)(INT64)-SOCK_ENOBUFS;
    while (total < len) {
        int chunk = len - total < BOUNCE ? len - total : BOUNCE;
        if (!NT_SUCCESS(CopyFromUser(tmp, (const void *)(uintptr_t)(a2 + total), chunk))) {
            kfree(tmp);
            return total ? (UINT64)total : (UINT64)(INT64)-SOCK_EFAULT;
        }
        int w = NetSockSend(s, tmp, chunk, sock_cancel, NULL);
        if (w < 0) { kfree(tmp); return total ? (UINT64)total : (UINT64)(INT64)w; }
        total += w;
        if (w < chunk) break;                    /* non-blocking partial send */
    }
    kfree(tmp);
    return (UINT64)total;
}

static UINT64 sys_recv(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    int len = (int)a3;
    if (len > BOUNCE) len = BOUNCE;
    UINT8 *tmp = kmalloc(BOUNCE);
    if (!tmp) return (UINT64)(INT64)-SOCK_ENOBUFS;
    int n = NetSockRecv(s, tmp, len, sock_cancel, NULL);
    if (n > 0 && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, tmp, n))) n = -SOCK_EFAULT;
    kfree(tmp);
    return (UINT64)(INT64)n;
}

static UINT64 sys_bind(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    return (UINT64)(INT64)NetSockBind(s, (UINT32)a2, (UINT16)a3);
}

static UINT64 sys_listen(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    return (UINT64)(INT64)NetSockListen(s, (int)a2);
}

/* NtNovaSockAccept(h, PVOID addr_out[ip;port]).  Returns a new handle, or 0. */
static UINT64 sys_accept(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    int s = handle_sock(p, a1);
    if (s < 0) return 0;
    UINT32 ip = 0; UINT16 port = 0;
    int ns = NetSockAccept(s, &ip, &port, sock_cancel, NULL);
    if (ns < 0) return 0;
    UmObject *o = kzalloc(sizeof(*o));
    if (!o) { NetSockClose(ns); return 0; }
    o->type = UO_SOCKET; o->refs = 1; o->sock = ns; o->destroy = sock_destroy;
    UINT64 hv = um_handle_new_object(p, o);
    um_ob_unref(o);
    if (hv && a2) { UINT8 sa[8]; memcpy(sa, &ip, 4); memcpy(sa + 4, &port, 2); CopyToUser((void *)(uintptr_t)a2, sa, 8); }
    return hv;
}

/* NtNovaSockCtl(h, op, arg, outptr):
 *   0 set non-blocking (arg=0/1); 1 shutdown (arg=how);
 *   2 getpeername; 3 getsockname; 4 poll (outptr gets 3 bytes r/w/e) */
static UINT64 sys_ctl(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    UINT32 ip; UINT16 port;
    switch (a2) {
    case 0: NetSockSetNonblock(s, a3 != 0); return 0;
    case 1: return (UINT64)(INT64)NetSockShutdown(s, (int)a3);
    case 2:
    case 3: {
        int r = a2 == 2 ? NetSockPeerName(s, &ip, &port) : NetSockLocalName(s, &ip, &port);
        if (r < 0) return (UINT64)(INT64)r;
        UINT8 sa[8]; memcpy(sa, &ip, 4); memcpy(sa + 4, &port, 2);
        return a4 && NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, sa, 8)) ? 0 : (UINT64)(INT64)-SOCK_EFAULT;
    }
    case 4: {
        bool rd, wr, er; NetSockPoll(s, &rd, &wr, &er);
        UINT8 st[3] = { rd, wr, er };
        return a4 && NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, st, 3)) ? 0 : (UINT64)(INT64)-SOCK_EFAULT;
    }
    }
    return (UINT64)(INT64)-SOCK_EINVAL;
}

static UINT64 sys_sendto(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    int len = (int)a3;
    if (len > BOUNCE) len = BOUNCE;
    UINT8 sa[8];
    if (!a4 || !NT_SUCCESS(CopyFromUser(sa, (const void *)(uintptr_t)a4, 8))) return (UINT64)(INT64)-SOCK_EFAULT;
    UINT32 ip; UINT16 port; memcpy(&ip, sa, 4); memcpy(&port, sa + 4, 2);
    UINT8 *tmp = kmalloc(BOUNCE);
    if (!tmp) return (UINT64)(INT64)-SOCK_ENOBUFS;
    int r = NT_SUCCESS(CopyFromUser(tmp, (const void *)(uintptr_t)a2, len))
            ? NetSockSendTo(s, tmp, len, ip, port) : -SOCK_EFAULT;
    kfree(tmp);
    return (UINT64)(INT64)r;
}

static UINT64 sys_recvfrom(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    int len = (int)a3;
    if (len > BOUNCE) len = BOUNCE;
    UINT8 *tmp = kmalloc(BOUNCE);
    if (!tmp) return (UINT64)(INT64)-SOCK_ENOBUFS;
    UINT32 ip = 0; UINT16 port = 0;
    int n = NetSockRecvFrom(s, tmp, len, &ip, &port, sock_cancel, NULL);
    if (n >= 0) {
        if (n && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, tmp, n))) n = -SOCK_EFAULT;
        else if (a4) { UINT8 sa[8]; memcpy(sa, &ip, 4); memcpy(sa + 4, &port, 2); CopyToUser((void *)(uintptr_t)a4, sa, 8); }
    }
    kfree(tmp);
    return (UINT64)(INT64)n;
}

/* NtNovaResolve(name, ip_out): host name → IPv4 (network order).  0 or -err. */
static UINT64 sys_resolve(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    char name[256];
    if (!NT_SUCCESS(CopyStringFromUser(name, sizeof(name), (const char *)(uintptr_t)a1)))
        return (UINT64)(INT64)-SOCK_EFAULT;
    NetOp *op = NetResolve(name);
    if (!op) return (UINT64)(INT64)-SOCK_ENOBUFS;
    UINT64 deadline = sched_ticks() + 1000;
    while (op->state == NET_PENDING) {
        if (um_stopping() || sched_ticks() > deadline) { NetRelease(op); return (UINT64)(INT64)-SOCK_ETIMEDOUT; }
        sched_wait();
    }
    int r;
    if (op->state == NET_DONE) {
        UINT32 ip = op->ip;
        r = a2 && NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, &ip, 4)) ? 0 : -SOCK_EFAULT;
    } else {
        r = -SOCK_EHOSTUNREACH;
    }
    NetRelease(op);
    return (UINT64)(INT64)r;
}

void um_socket_syscalls_init(void)
{
    um_install(SYSCALL_NtNovaSocket,       sys_socket);
    um_install(SYSCALL_NtNovaSockConnect,  sys_connect);
    um_install(SYSCALL_NtNovaSockSend,     sys_send);
    um_install(SYSCALL_NtNovaSockRecv,     sys_recv);
    um_install(SYSCALL_NtNovaSockBind,     sys_bind);
    um_install(SYSCALL_NtNovaSockListen,   sys_listen);
    um_install(SYSCALL_NtNovaSockAccept,   sys_accept);
    um_install(SYSCALL_NtNovaSockCtl,      sys_ctl);
    um_install(SYSCALL_NtNovaSockSendTo,   sys_sendto);
    um_install(SYSCALL_NtNovaSockRecvFrom, sys_recvfrom);
    um_install(SYSCALL_NtNovaResolve,      sys_resolve);
}
