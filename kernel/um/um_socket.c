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
#include "../net/net_internal.h"

#define BOUNCE  (16 * 1024)

static bool sock_cancel(void *arg) { (void)arg; return um_stopping(); }

/* Programs' socket addresses are Winsock's: SOCKADDR_IN (16 bytes) or
 * SOCKADDR_IN6 (28: family, port, flow info, address, scope id) */
#define SA_MAX 28

static int sa_from_user(UINT64 uptr, UINT64 len, NetSockAddr *out)
{
    UINT8 b[SA_MAX] = { 0 };
    if (!uptr || len < 16) return -SOCK_EFAULT;
    if (!NT_SUCCESS(CopyFromUser(b, (const void *)(uintptr_t)uptr, len < SA_MAX ? (UINT32)len : SA_MAX)))
        return -SOCK_EFAULT;
    memset(out, 0, sizeof(*out));
    memcpy(&out->family, b, 2);
    memcpy(&out->port_be, b + 2, 2);
    if (out->family == NET_AF_INET) { memcpy(out->addr, b + 4, 4); return 0; }
    if (out->family == NET_AF_INET6) {
        if (len < SA_MAX) return -SOCK_EFAULT;
        memcpy(out->addr, b + 8, 16);
        memcpy(&out->scope, b + 24, 4);
        return 0;
    }
    return -SOCK_EAFNOSUPPORT;
}

static void sa_to_bytes(const NetSockAddr *a, UINT8 b[SA_MAX])
{
    memset(b, 0, SA_MAX);
    memcpy(b, &a->family, 2);
    memcpy(b + 2, &a->port_be, 2);
    if (a->family == NET_AF_INET6) { memcpy(b + 8, a->addr, 16); memcpy(b + 24, &a->scope, 4); }
    else memcpy(b + 4, a->addr, 4);
}

static bool sa_to_user(UINT64 uptr, const NetSockAddr *a)
{
    UINT8 b[SA_MAX];
    sa_to_bytes(a, b);
    return uptr && NT_SUCCESS(CopyToUser((void *)(uintptr_t)uptr, b, SA_MAX));
}

static void sock_destroy(UmObject *o) { NetSockClose(o->sock); }

static int handle_sock(UmProcess *p, UINT64 h)
{
    UmObject *o = um_handle_object(p, h, UO_SOCKET);
    if (!o) return -1;
    int s = o->sock;
    um_ob_unref(o);
    return s;
}

/* NtNovaSocket(type, family): type 0 = TCP, 1 = UDP; family AF_INET (2,
 * also for 0) or AF_INET6 (23).  Returns a handle, or 0 on error. */
static UINT64 sys_socket(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    int fam = a2 ? (int)a2 : NET_AF_INET;
    int s = a1 == 1 ? NetSockUdp(fam) : NetSockTcp(fam);
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

/* NtNovaSockConnect(h, sockaddr, length) */
static UINT64 sys_connect(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    NetSockAddr to;
    int r = sa_from_user(a2, a3, &to);
    if (r < 0) return (UINT64)(INT64)r;
    return (UINT64)(INT64)NetSockConnect(s, &to, sock_cancel, NULL);
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
    NetSockAddr a;
    int r = sa_from_user(a2, a3, &a);
    if (r < 0) return (UINT64)(INT64)r;
    return (UINT64)(INT64)NetSockBind(s, &a);
}

static UINT64 sys_listen(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    return (UINT64)(INT64)NetSockListen(s, (int)a2);
}

/* NtNovaSockAccept(h, PVOID addr_out[28]).  Returns a new handle, or 0. */
static UINT64 sys_accept(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    int s = handle_sock(p, a1);
    if (s < 0) return 0;
    NetSockAddr peer;
    int ns = NetSockAccept(s, &peer, sock_cancel, NULL);
    if (ns < 0) return 0;
    UmObject *o = kzalloc(sizeof(*o));
    if (!o) { NetSockClose(ns); return 0; }
    o->type = UO_SOCKET; o->refs = 1; o->sock = ns; o->destroy = sock_destroy;
    UINT64 hv = um_handle_new_object(p, o);
    um_ob_unref(o);
    if (hv && a2) sa_to_user(a2, &peer);
    return hv;
}

/* NtNovaSockCtl(h, op, arg, outptr):
 *   0 set non-blocking (arg=0/1); 1 shutdown (arg=how);
 *   2 getpeername; 3 getsockname; 4 poll (outptr gets 3 bytes r/w/e) */
static UINT64 sys_ctl(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    /* 6: the network generation (select reads it before looking);
     * 5: sleep until it moves on from arg, at most a4 ms */
    if (a2 == 6) return net_gen();
    if (a2 == 5) {
        UINT64 ticks = (a4 + 9) / 10;
        if (ticks > 10) ticks = 10;
        net_wait_ticks((UINT32)a3, ticks ? ticks : 1);
        return 0;
    }
    int s = handle_sock(UmCurrent(), a1);
    if (s < 0) return (UINT64)(INT64)-SOCK_ENOTSOCK;
    NetSockAddr na;
    switch (a2) {
    case 0: NetSockSetNonblock(s, a3 != 0); return 0;
    case 1: return (UINT64)(INT64)NetSockShutdown(s, (int)a3);
    case 2:
    case 3: {
        int r = a2 == 2 ? NetSockPeerName(s, &na) : NetSockLocalName(s, &na);
        if (r < 0) return (UINT64)(INT64)r;
        return sa_to_user(a4, &na) ? 0 : (UINT64)(INT64)-SOCK_EFAULT;
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
    NetSockAddr to;
    int e = sa_from_user(a4, SA_MAX, &to);                  /* (ws2_32 passes a full 28 bytes) */
    if (e < 0) return (UINT64)(INT64)e;
    UINT8 *tmp = kmalloc(BOUNCE);
    if (!tmp) return (UINT64)(INT64)-SOCK_ENOBUFS;
    int r = NT_SUCCESS(CopyFromUser(tmp, (const void *)(uintptr_t)a2, len))
            ? NetSockSendTo(s, tmp, len, &to) : -SOCK_EFAULT;
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
    NetSockAddr from;
    int n = NetSockRecvFrom(s, tmp, len, &from, sock_cancel, NULL);
    if (n >= 0) {
        if (n && !NT_SUCCESS(CopyToUser((void *)(uintptr_t)a2, tmp, n))) n = -SOCK_EFAULT;
        else if (a4) sa_to_user(a4, &from);
    }
    kfree(tmp);
    return (UINT64)(INT64)n;
}

/* One lookup; true with *out set, false with *err */
static bool resolve_one(const char *name, int family, NetIp *out, int *err)
{
    NetOp *op = NetResolveEx(name, family);
    if (!op) { *err = -SOCK_ENOBUFS; return false; }
    UINT64 deadline = sched_ticks() + 1000;
    for (;;) {
        UINT32 ng = net_gen();
        if (op->state != NET_PENDING) break;
        if (um_stopping() || sched_ticks() > deadline) { NetRelease(op); *err = -SOCK_ETIMEDOUT; return false; }
        net_wait(ng);
    }
    bool ok = op->state == NET_DONE;
    if (ok) *out = op->addr;
    else *err = -SOCK_EHOSTUNREACH;
    NetRelease(op);
    return ok;
}

/* NtNovaResolve(name, sockaddrs_out, max, family): host name → up to @max
 * 28-byte socket addresses (port 0): family 2 or 23 for only that one; 0
 * for IPv6 then IPv4 when the interface has a global IPv6 address, else
 * IPv4 only.  Returns the count, or -err. */
static UINT64 sys_resolve(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    char name[256];
    if (!NT_SUCCESS(CopyStringFromUser(name, sizeof(name), (const char *)(uintptr_t)a1)))
        return (UINT64)(INT64)-SOCK_EFAULT;
    int max = (int)a3, fam = (int)a4, n = 0, err = -SOCK_EHOSTUNREACH;
    if (max < 1) max = 1;
    int order[2] = { 4, 0 };                     /* (AAAA only with IPv6 to use, as Windows) */
    if (fam == NET_AF_INET) order[1] = 0;
    else if (fam == NET_AF_INET6) { order[0] = 6; order[1] = 0; }
    else if (NetHasIp6()) { order[0] = 6; order[1] = 4; }
    for (int i = 0; i < 2 && order[i] && n < max; i++) {
        NetIp ip;
        if (!resolve_one(name, order[i], &ip, &err)) {
            if (err == -SOCK_ETIMEDOUT && um_stopping()) break;
            continue;
        }
        NetSockAddr sa = { 0 };
        sa.family = ip.v6 ? NET_AF_INET6 : NET_AF_INET;
        memcpy(sa.addr, ip.a, ip.v6 ? 16 : 4);
        if (ip.v6 && ip.a[0] == 0xFE && (ip.a[1] & 0xC0) == 0x80) sa.scope = 1;
        if (!sa_to_user(a2 + (UINT64)n * SA_MAX, &sa)) return (UINT64)(INT64)-SOCK_EFAULT;
        n++;
    }
    return n ? (UINT64)n : (UINT64)(INT64)err;
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
