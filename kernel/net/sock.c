/*
 * sock.c — sockets over lwIP's raw API (see sock.h)
 *
 * A socket owns an lwIP PCB plus a receive ring; lwIP's callbacks run on
 * the net thread under the network lock and fill the ring / update flags,
 * while program threads block by yielding until enough state is present.
 * The net lock serializes every lwIP touch, so the callbacks and these
 * calls never race over the PCB.
 */

#include "sock.h"
#include "net_internal.h"
#include "../ke/scheduler.h"
#include "../ke/printf.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"      /* tcp_process_refused_data */
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"

#define NSOCK        64
#define RXBUF        (32 * 1024)
#define ACCEPT_MAX   8
#define DGRAM_HDR    (2 + (int)sizeof(NetSockAddr))

typedef struct {
    bool          used;
    bool          udp;
    bool          nonblock;
    bool          connected;
    bool          connecting;
    bool          peer_closed;    /* FIN received */
    bool          reset;          /* connection error */
    bool          listening;
    bool          send_shut;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp_pcb;

    UINT8        *rx;             /* RXBUF, allocated on creation */
    volatile UINT32 rx_head, rx_tail;
    UINT16        unacked;        /* bytes received but not yet tcp_recved */

    /* UDP datagram source of the last recvfrom chunk boundary */
    /* listener accept queue: sockets for connections not yet accepted
     * (they already receive, so a client may send before accept) */
    int           acc[ACCEPT_MAX];
    volatile int  acc_head, acc_tail;

    int           family;         /* NET_AF_INET or NET_AF_INET6 */
    NetSockAddr   peer;           /* connected peer */
} Sock;

static Sock g_sock[NSOCK];

/* -----------------------------------------------------------------------
 * Ring buffer (single producer: net thread; single consumer: program)
 * ----------------------------------------------------------------------- */
static UINT32 rx_used(Sock *s) { return s->rx_head - s->rx_tail; }

static int rx_put(Sock *s, const UINT8 *d, int n)
{
    int done = 0;
    while (done < n && rx_used(s) < RXBUF) {
        s->rx[s->rx_head % RXBUF] = d[done++];
        s->rx_head++;
    }
    return done;
}

static int rx_get(Sock *s, UINT8 *d, int cap)
{
    int n = 0;
    while (n < cap && rx_used(s) > 0) {
        d[n++] = s->rx[s->rx_tail % RXBUF];
        s->rx_tail++;
    }
    return n;
}

static Sock *slot(int s) { return (s >= 0 && s < NSOCK && g_sock[s].used) ? &g_sock[s] : NULL; }

static bool wait_cancel(SockCancelFn c, void *a) { return c && c(a); }

/* -----------------------------------------------------------------------
 * Addresses.  An IPv6 socket's lwIP PCB takes either family (dual-stack):
 * an IPv4-mapped address (::ffff:a.b.c.d) is plain IPv4 to lwIP, and IPv4
 * peers are reported to it mapped.
 * ----------------------------------------------------------------------- */
static const UINT8 g_mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };

static void to_lwip(const NetSockAddr *a, ip_addr_t *o)
{
    NetIp ip = { 0 };
    if (a->family == NET_AF_INET6 && !memcmp(a->addr, g_mapped, 12)) {
        memcpy(ip.a, a->addr + 12, 4);
    } else if (a->family == NET_AF_INET6) {
        ip.v6 = true;
        memcpy(ip.a, a->addr, 16);
    } else {
        memcpy(ip.a, a->addr, 4);
    }
    net_addr_to_lwip(&ip, o);
}

static void from_lwip(const Sock *s, const ip_addr_t *ip, u16_t port, NetSockAddr *o)
{
    NetIp x;
    net_addr_from_lwip(ip, &x);
    memset(o, 0, sizeof(*o));
    o->port_be = lwip_htons(port);
    if (x.v6) {
        o->family = NET_AF_INET6;
        memcpy(o->addr, x.a, 16);
        if (ip6_addr_islinklocal(ip_2_ip6(ip))) o->scope = 1;     /* (the one interface) */
    } else if (s && s->family == NET_AF_INET6) {
        o->family = NET_AF_INET6;
        memcpy(o->addr, g_mapped, 12);
        memcpy(o->addr + 12, x.a, 4);
    } else {
        o->family = NET_AF_INET;
        memcpy(o->addr, x.a, 4);
    }
}

static bool family_ok(const Sock *s, const NetSockAddr *a)
{
    return a->family == s->family || (s->family == NET_AF_INET6 && a->family == NET_AF_INET);
}

/* -----------------------------------------------------------------------
 * lwIP callbacks (net thread, lock held)
 * ----------------------------------------------------------------------- */
static void tcp_err_cb(void *arg, err_t err)
{
    Sock *s = arg;
    if (!s) return;
    (void)err;
    s->tcp = NULL;                 /* lwIP already freed the pcb */
    s->reset = true;
    s->connecting = false;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    Sock *s = arg;
    if (!s) { if (p) pbuf_free(p); return ERR_OK; }
    if (err != ERR_OK) { if (p) pbuf_free(p); s->reset = true; return ERR_OK; }
    (void)pcb;
    if (!p) { s->peer_closed = true; return ERR_OK; }        /* FIN */
    /* All or nothing: when the ring cannot hold it, the data goes back to
     * lwIP untouched (ERR_MEM keeps it as "refused" data, delivered again
     * once the program reads; it must not be freed here).  The window is
     * reopened only as the program reads (NetSockRecv), and TCP_WND is
     * smaller than the ring, so this is only a safety net. */
    if (!s->rx || RXBUF - rx_used(s) < p->tot_len) return ERR_MEM;
    for (struct pbuf *q = p; q; q = q->next) rx_put(s, (const UINT8 *)q->payload, q->len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)pcb; (void)len;
    net_wake();                    /* send buffer space for a waiting writer */
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    Sock *s = arg;
    if (!s) return ERR_OK;
    (void)pcb;
    if (err == ERR_OK) { s->connected = true; s->connecting = false; }
    else { s->reset = true; s->connecting = false; }
    return ERR_OK;
}

static int alloc_slot(void);

static void tcp_callbacks(struct tcp_pcb *pcb, Sock *s)
{
    tcp_arg(pcb, s);
    tcp_err(pcb, tcp_err_cb);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_sent(pcb, tcp_sent_cb);
}

/* A new connection on a listener: its socket is made now, so data the
 * client sends before accept() waits in its ring instead of being lost */
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    Sock *s = arg;
    if (!s || err != ERR_OK || !newpcb) return ERR_VAL;
    int next = (s->acc_head + 1) % ACCEPT_MAX;
    if (next == s->acc_tail) return ERR_MEM;                 /* queue full */
    int ni = alloc_slot();
    if (ni < 0) return ERR_MEM;
    Sock *ns = &g_sock[ni];
    ns->tcp = newpcb;
    ns->connected = true;
    ns->family = s->family;
    from_lwip(s, &newpcb->remote_ip, newpcb->remote_port, &ns->peer);
    tcp_backlog_delayed(newpcb);
    tcp_callbacks(newpcb, ns);
    s->acc[s->acc_head] = ni;
    s->acc_head = next;
    net_wake();
    return ERR_OK;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    Sock *s = arg;
    (void)pcb;
    if (!s || !p) { if (p) pbuf_free(p); return; }
    /* Datagram framing: 2-byte length, the source NetSockAddr, then data */
    UINT16 len = p->tot_len;
    if (rx_used(s) + len + DGRAM_HDR <= RXBUF) {
        UINT8 hdr[DGRAM_HDR];
        hdr[0] = len & 0xFF; hdr[1] = len >> 8;
        NetSockAddr from;
        from_lwip(s, addr, port, &from);
        memcpy(hdr + 2, &from, sizeof(from));
        rx_put(s, hdr, DGRAM_HDR);
        for (struct pbuf *q = p; q; q = q->next) rx_put(s, q->payload, q->len);
    }
    pbuf_free(p);
}

/* -----------------------------------------------------------------------
 * Creation
 * ----------------------------------------------------------------------- */
static int alloc_slot(void)
{
    for (int i = 0; i < NSOCK; i++) if (!g_sock[i].used) {
        UINT8 *rx = kmalloc(RXBUF);
        if (!rx) return -SOCK_ENOBUFS;
        memset(&g_sock[i], 0, sizeof(Sock));
        g_sock[i].rx = rx;
        g_sock[i].used = true;
        return i;
    }
    return -SOCK_EMFILE;
}

int NetSockTcp(int family)
{
    if (family != NET_AF_INET && family != NET_AF_INET6) return -SOCK_EAFNOSUPPORT;
    if (!net_up()) return -SOCK_ENETDOWN;
    net_lock();
    int i = alloc_slot();
    if (i < 0) { net_unlock(); return i; }
    Sock *s = &g_sock[i];
    s->family = family;
    s->tcp = tcp_new_ip_type(family == NET_AF_INET6 ? IPADDR_TYPE_ANY : IPADDR_TYPE_V4);
    if (!s->tcp) { s->used = false; net_unlock(); return -SOCK_ENOBUFS; }
    tcp_callbacks(s->tcp, s);
    net_unlock();
    return i;
}

int NetSockUdp(int family)
{
    if (family != NET_AF_INET && family != NET_AF_INET6) return -SOCK_EAFNOSUPPORT;
    if (!net_up()) return -SOCK_ENETDOWN;
    net_lock();
    int i = alloc_slot();
    if (i < 0) { net_unlock(); return i; }
    Sock *s = &g_sock[i];
    s->family = family;
    s->udp = true;
    s->udp_pcb = udp_new_ip_type(family == NET_AF_INET6 ? IPADDR_TYPE_ANY : IPADDR_TYPE_V4);
    if (!s->udp_pcb) { s->used = false; net_unlock(); return -SOCK_ENOBUFS; }
    udp_recv(s->udp_pcb, udp_recv_cb, s);
    net_unlock();
    return i;
}

/* -----------------------------------------------------------------------
 * Connect / send / recv
 * ----------------------------------------------------------------------- */
int NetSockConnect(int sd, const NetSockAddr *to, SockCancelFn c, void *ca)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s || s->udp) { net_unlock(); return -SOCK_ENOTSOCK; }
    if (s->connected) { net_unlock(); return -SOCK_EISCONN; }
    if (!s->tcp) { net_unlock(); return -SOCK_ENOTCONN; }
    if (!family_ok(s, to)) { net_unlock(); return -SOCK_EAFNOSUPPORT; }
    ip_addr_t ip; to_lwip(to, &ip);
    s->connecting = true;
    err_t e = tcp_connect(s->tcp, &ip, lwip_htons(to->port_be), tcp_connected_cb);
    net_unlock();
    if (e != ERR_OK) { s->connecting = false; return -SOCK_ENOBUFS; }
    if (s->nonblock) return -SOCK_EWOULDBLOCK;
    UINT64 deadline = sched_ticks() + 1000;                  /* 10 s */
    for (;;) {
        UINT32 ng = net_gen();
        if (!s->connecting || s->reset) break;
        if (wait_cancel(c, ca) || sched_ticks() > deadline) return -SOCK_ETIMEDOUT;
        net_wait(ng);
    }
    if (s->reset || !s->connected) return -SOCK_ECONNREFUSED;
    from_lwip(s, &ip, lwip_ntohs(to->port_be), &s->peer);
    return 0;
}

int NetSockSend(int sd, const void *buf, int len, SockCancelFn c, void *ca)
{
    Sock *s = slot(sd);
    if (!s) return -SOCK_ENOTSOCK;
    if (s->reset) return -SOCK_ECONNRESET;
    if (!s->connected) return -SOCK_ENOTCONN;
    if (s->send_shut) return -SOCK_ENOTCONN;
    if (len <= 0) return 0;
    const UINT8 *p = buf;
    int sent = 0;
    while (sent < len) {
        UINT32 ng = net_gen();
        net_lock();
        if (!s->tcp || s->reset) { net_unlock(); return sent ? sent : -SOCK_ECONNRESET; }
        UINT16 space = tcp_sndbuf(s->tcp);
        if (space == 0) {
            net_unlock();
            if (s->nonblock) return sent ? sent : -SOCK_EWOULDBLOCK;
            if (wait_cancel(c, ca)) return sent ? sent : -SOCK_ETIMEDOUT;
            net_wait(ng);
            continue;
        }
        int chunk = len - sent;
        if (chunk > space) chunk = space;
        err_t e = tcp_write(s->tcp, p + sent, (UINT16)chunk, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK) { tcp_output(s->tcp); sent += chunk; }
        net_unlock();
        if (e == ERR_MEM) {
            if (s->nonblock) return sent ? sent : -SOCK_EWOULDBLOCK;
            net_wait(ng);
        } else if (e != ERR_OK) {
            return sent ? sent : -SOCK_ECONNRESET;
        }
        if (s->nonblock) break;
    }
    return sent;
}

int NetSockRecv(int sd, void *buf, int len, SockCancelFn c, void *ca)
{
    Sock *s = slot(sd);
    if (!s) return -SOCK_ENOTSOCK;
    if (len <= 0) return 0;
    for (;;) {
        UINT32 ng = net_gen();
        net_lock();
        UINT32 avail = rx_used(s);
        if (avail) {
            int n = rx_get(s, buf, len);
            if (s->tcp && !s->udp) {
                tcp_recved(s->tcp, (u16_t)n);                /* reopen the window by what was read */
                if (s->tcp->refused_data) tcp_process_refused_data(s->tcp);
            }
            net_unlock();
            return n;
        }
        bool closed = s->peer_closed, reset = s->reset;
        net_unlock();
        if (reset) return -SOCK_ECONNRESET;
        if (closed) return 0;                                /* orderly shutdown */
        if (!s->connected && !s->connecting) return -SOCK_ENOTCONN;
        if (s->nonblock) return -SOCK_EWOULDBLOCK;
        if (wait_cancel(c, ca)) return -SOCK_ETIMEDOUT;
        net_wait(ng);
    }
}

/* -----------------------------------------------------------------------
 * UDP
 * ----------------------------------------------------------------------- */
int NetSockSendTo(int sd, const void *buf, int len, const NetSockAddr *to)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s || !s->udp || !s->udp_pcb) { net_unlock(); return -SOCK_ENOTSOCK; }
    if (!family_ok(s, to)) { net_unlock(); return -SOCK_EAFNOSUPPORT; }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (UINT16)len, PBUF_RAM);
    if (!p) { net_unlock(); return -SOCK_ENOBUFS; }
    pbuf_take(p, buf, (UINT16)len);
    ip_addr_t ip; to_lwip(to, &ip);
    err_t e = udp_sendto(s->udp_pcb, p, &ip, lwip_htons(to->port_be));
    pbuf_free(p);
    net_unlock();
    return e == ERR_OK ? len : -SOCK_EHOSTUNREACH;
}

int NetSockRecvFrom(int sd, void *buf, int len, NetSockAddr *from, SockCancelFn c, void *ca)
{
    Sock *s = slot(sd);
    if (!s || !s->udp) return -SOCK_ENOTSOCK;
    for (;;) {
        UINT32 ng = net_gen();
        net_lock();
        if (rx_used(s) >= (UINT32)DGRAM_HDR) {
            UINT8 hdr[DGRAM_HDR];
            rx_get(s, hdr, DGRAM_HDR);
            UINT16 dlen = hdr[0] | (hdr[1] << 8);
            if (from) memcpy(from, hdr + 2, sizeof(*from));
            int take = dlen < len ? dlen : len;
            int got = rx_get(s, buf, take);
            for (int drop = got; drop < dlen; drop++) { UINT8 t; rx_get(s, &t, 1); }  /* truncate */
            net_unlock();
            return got;
        }
        net_unlock();
        if (s->nonblock) return -SOCK_EWOULDBLOCK;
        if (wait_cancel(c, ca)) return -SOCK_ETIMEDOUT;
        net_wait(ng);
    }
}

/* -----------------------------------------------------------------------
 * Server side
 * ----------------------------------------------------------------------- */
int NetSockBind(int sd, const NetSockAddr *a)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s) { net_unlock(); return -SOCK_ENOTSOCK; }
    if (!family_ok(s, a)) { net_unlock(); return -SOCK_EAFNOSUPPORT; }
    static const UINT8 zero[16];
    ip_addr_t ip;
    if (s->family == NET_AF_INET6 && !memcmp(a->addr, zero, 16)) ip_addr_copy(ip, *IP_ANY_TYPE);   /* :: takes both families */
    else to_lwip(a, &ip);
    err_t e;
    if (s->udp) e = udp_bind(s->udp_pcb, &ip, lwip_htons(a->port_be));
    else        e = tcp_bind(s->tcp, &ip, lwip_htons(a->port_be));
    net_unlock();
    return e == ERR_OK ? 0 : -SOCK_EADDRINUSE;
}

int NetSockListen(int sd, int backlog)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s || s->udp || !s->tcp) { net_unlock(); return -SOCK_ENOTSOCK; }
    struct tcp_pcb *lp = tcp_listen_with_backlog(s->tcp, backlog > 0 && backlog < 255 ? (UINT8)backlog : 8);
    if (!lp) { net_unlock(); return -SOCK_ENOBUFS; }
    s->tcp = lp;
    s->listening = true;
    tcp_arg(lp, s);
    tcp_accept(lp, tcp_accept_cb);
    net_unlock();
    return 0;
}

int NetSockAccept(int sd, NetSockAddr *peer, SockCancelFn c, void *ca)
{
    Sock *s = slot(sd);
    if (!s || !s->listening) return -SOCK_ENOTSOCK;
    for (;;) {
        UINT32 ng = net_gen();
        net_lock();
        if (s->acc_tail != s->acc_head) {
            int ni = s->acc[s->acc_tail];
            s->acc_tail = (s->acc_tail + 1) % ACCEPT_MAX;
            Sock *ns = &g_sock[ni];
            if (ns->tcp) tcp_backlog_accepted(ns->tcp);
            ns->nonblock = s->nonblock;               /* as on Windows: the listener's mode is inherited */
            if (peer) *peer = ns->peer;
            net_unlock();
            return ni;
        }
        net_unlock();
        if (s->nonblock) return -SOCK_EWOULDBLOCK;
        if (wait_cancel(c, ca)) return -SOCK_ETIMEDOUT;
        net_wait(ng);
    }
}

int NetSockShutdown(int sd, int how)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s || !s->tcp) { net_unlock(); return -SOCK_ENOTSOCK; }
    tcp_shutdown(s->tcp, how == 0 || how == 2, how == 1 || how == 2);
    if (how == 1 || how == 2) s->send_shut = true;
    net_unlock();
    return 0;
}

static void close_locked(Sock *s)
{
    if (s->udp && s->udp_pcb) udp_remove(s->udp_pcb);
    if (!s->udp && s->tcp) {
        tcp_arg(s->tcp, NULL);
        if (s->listening) tcp_accept(s->tcp, NULL);         /* (a listener has no recv/sent/err) */
        else { tcp_recv(s->tcp, NULL); tcp_sent(s->tcp, NULL); tcp_err(s->tcp, NULL); }
        if (tcp_close(s->tcp) != ERR_OK) tcp_abort(s->tcp);
    }
    /* Drop any queued, not-yet-accepted connections */
    while (s->acc_tail != s->acc_head) {
        Sock *ps = &g_sock[s->acc[s->acc_tail]];
        s->acc_tail = (s->acc_tail + 1) % ACCEPT_MAX;
        if (ps->tcp) { tcp_arg(ps->tcp, NULL); tcp_err(ps->tcp, NULL); tcp_abort(ps->tcp); ps->tcp = NULL; }
        close_locked(ps);
    }
    if (s->rx) { kfree(s->rx); s->rx = NULL; }
    s->used = false;
}

void NetSockClose(int sd)
{
    net_lock();
    Sock *s = slot(sd);
    if (s) close_locked(s);
    net_unlock();
}

void NetSockSetNonblock(int sd, bool nb) { Sock *s = slot(sd); if (s) s->nonblock = nb; }

int NetSockLocalName(int sd, NetSockAddr *out)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s) { net_unlock(); return -SOCK_ENOTSOCK; }
    if (s->udp && s->udp_pcb)  from_lwip(s, &s->udp_pcb->local_ip, s->udp_pcb->local_port, out);
    else if (s->tcp)           from_lwip(s, &s->tcp->local_ip, s->tcp->local_port, out);
    else { memset(out, 0, sizeof(*out)); out->family = (UINT16)s->family; }
    if (out->family == NET_AF_INET6 && s->family == NET_AF_INET) out->family = NET_AF_INET;
    if (s->family == NET_AF_INET6 && out->family == NET_AF_INET6 && !memcmp(out->addr, g_mapped, 12) &&
        !out->addr[12] && !out->addr[13] && !out->addr[14] && !out->addr[15])
        memset(out->addr, 0, 16);                         /* unbound: ::, not ::ffff:0.0.0.0 */
    net_unlock();
    return 0;
}

int NetSockPeerName(int sd, NetSockAddr *out)
{
    Sock *s = slot(sd);
    if (!s || !s->connected) return -SOCK_ENOTCONN;
    *out = s->peer;
    return 0;
}

void NetSockPoll(int sd, bool *readable, bool *writable, bool *error)
{
    net_lock();
    Sock *s = slot(sd);
    bool rd = false, wr = false, er = false;
    if (s) {
        er = s->reset;
        rd = rx_used(s) > 0 || s->peer_closed || (s->listening && s->acc_tail != s->acc_head) || s->reset;
        wr = s->udp || (s->connected && s->tcp && tcp_sndbuf(s->tcp) > 0) || (!s->connecting && !s->connected && !s->udp && !s->listening);
    }
    net_unlock();
    if (readable) *readable = rd;
    if (writable) *writable = wr;
    if (error) *error = er;
}
