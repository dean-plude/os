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
    bool          recv_shut;      /* shutdown(SD_RECEIVE): incoming data is dropped */
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

    /* Options (NetSockSetOpt), kept here as well as in the PCB: a
     * listener's go to the connections it accepts, and they read back
     * after lwIP has freed a PCB */
    UINT32        rcvbuf, sndbuf;         /* SO_RCVBUF/SO_SNDBUF: read back (the ring stays RXBUF) */
    UINT32        rcvtimeo, sndtimeo;     /* ms, 0: wait for ever */
    bool          nodelay;                /* TCP_NODELAY: Nagle's algorithm off */
    bool          linger_on;              /* SO_LINGER */
    UINT16        linger_s;
    UINT8         so_options;             /* lwIP SOF_* (REUSEADDR, KEEPALIVE, BROADCAST) */
    UINT8         ttl;
} Sock;

static Sock g_sock[NSOCK];

/* -----------------------------------------------------------------------
 * Ring buffer (single producer: net thread; single consumer: program)
 * ----------------------------------------------------------------------- */
static UINT32 rx_used(Sock *s) { return s->rx_head - s->rx_tail; }

/* Up to @n bytes in or out, in at most two pieces (the ring wraps once) */
static int rx_put(Sock *s, const UINT8 *d, int n)
{
    UINT32 room = RXBUF - rx_used(s);
    if ((UINT32)n > room) n = (int)room;
    UINT32 at = s->rx_head % RXBUF, first = RXBUF - at < (UINT32)n ? RXBUF - at : (UINT32)n;
    memcpy(s->rx + at, d, first);
    memcpy(s->rx, d + first, (UINT32)n - first);
    s->rx_head += (UINT32)n;
    return n;
}

/* The next @cap bytes without taking them */
static int rx_peek(Sock *s, UINT8 *d, int cap)
{
    UINT32 n = rx_used(s);
    if ((UINT32)cap < n) n = (UINT32)cap;
    UINT32 at = s->rx_tail % RXBUF, first = RXBUF - at < n ? RXBUF - at : n;
    memcpy(d, s->rx + at, first);
    memcpy(d + first, s->rx, n - first);
    return (int)n;
}

static int rx_get(Sock *s, UINT8 *d, int cap)
{
    int n = rx_peek(s, d, cap);
    s->rx_tail += (UINT32)n;
    return n;
}

static Sock *slot(int s) { return (s >= 0 && s < NSOCK && g_sock[s].used) ? &g_sock[s] : NULL; }

static bool wait_cancel(SockCancelFn c, void *a) { return c && c(a); }

/* SO_RCVTIMEO/SO_SNDTIMEO: the tick a blocking call gives up at (0: never) */
static UINT64 deadline_of(UINT32 ms) { return ms ? sched_ticks() + (ms + 9) / 10 : 0; }
static bool past(UINT64 deadline)    { return deadline && sched_ticks() >= deadline; }

/* net_wait, but no later than @deadline */
static void wait_until(UINT32 gen, UINT64 deadline)
{
    if (!deadline) { net_wait(gen); return; }
    UINT64 now = sched_ticks(), left = deadline > now ? deadline - now : 1;
    net_wait_ticks(gen, left < 10 ? left : 10);
}

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
    s->tcp = NULL;                 /* lwIP already freed the pcb */
    if (err == ERR_CLSD) s->peer_closed = true;              /* closed in order after our FIN */
    else s->reset = true;
    s->connecting = false;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    Sock *s = arg;
    if (!s) { if (p) pbuf_free(p); return ERR_OK; }
    if (err != ERR_OK) { if (p) pbuf_free(p); s->reset = true; return ERR_OK; }
    if (!p) { s->peer_closed = true; return ERR_OK; }        /* FIN */
    if (s->recv_shut) { tcp_recved(pcb, p->tot_len); pbuf_free(p); return ERR_OK; }
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
    if (err == ERR_OK) {                                     /* the peer, for getpeername after a non-blocking connect */
        from_lwip(s, &pcb->remote_ip, pcb->remote_port, &s->peer);
        s->connected = true; s->connecting = false;
    }
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
    /* the listener's options, as Winsock's accept() gives them */
    ns->rcvbuf = s->rcvbuf; ns->sndbuf = s->sndbuf;
    ns->rcvtimeo = s->rcvtimeo; ns->sndtimeo = s->sndtimeo;
    ns->nodelay = s->nodelay; ns->linger_on = s->linger_on; ns->linger_s = s->linger_s;
    ns->so_options = s->so_options; ns->ttl = s->ttl;
    newpcb->so_options = s->so_options;
    newpcb->ttl = s->ttl;
    if (s->nodelay) tcp_nagle_disable(newpcb);
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
        g_sock[i].rcvbuf = RXBUF;
        g_sock[i].sndbuf = TCP_SND_BUF;
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
    s->ttl = s->tcp->ttl;
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
    s->ttl = s->udp_pcb->ttl;
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
    UINT64 deadline = deadline_of(s->sndtimeo);
    while (sent < len) {
        UINT32 ng = net_gen();
        net_lock();
        if (!s->tcp || s->reset) { net_unlock(); return sent ? sent : -SOCK_ECONNRESET; }
        UINT16 space = tcp_sndbuf(s->tcp);
        if (space == 0) {
            net_unlock();
            if (s->nonblock) return sent ? sent : -SOCK_EWOULDBLOCK;
            if (wait_cancel(c, ca) || past(deadline)) return sent ? sent : -SOCK_ETIMEDOUT;
            wait_until(ng, deadline);
            continue;
        }
        int chunk = len - sent;
        if (chunk > space) chunk = space;
        err_t e = tcp_write(s->tcp, p + sent, (UINT16)chunk, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK) { tcp_output(s->tcp); sent += chunk; }
        net_unlock();
        if (e == ERR_MEM) {
            if (s->nonblock) return sent ? sent : -SOCK_EWOULDBLOCK;
            if (past(deadline)) return sent ? sent : -SOCK_ETIMEDOUT;
            wait_until(ng, deadline);
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
    UINT64 deadline = deadline_of(s->rcvtimeo);
    for (;;) {
        UINT32 ng = net_gen();
        net_lock();
        UINT32 avail = s->recv_shut ? 0 : rx_used(s);
        if (avail) {
            int n = rx_get(s, buf, len);
            if (s->tcp && !s->udp) {
                tcp_recved(s->tcp, (u16_t)n);                /* reopen the window by what was read */
                if (s->tcp->refused_data) tcp_process_refused_data(s->tcp);
            }
            net_unlock();
            return n;
        }
        bool closed = s->peer_closed || s->recv_shut, reset = s->reset;
        net_unlock();
        if (reset) return -SOCK_ECONNRESET;
        if (closed) return 0;                                /* orderly shutdown */
        if (!s->connected && !s->connecting) return -SOCK_ENOTCONN;
        if (s->nonblock) return -SOCK_EWOULDBLOCK;
        if (wait_cancel(c, ca) || past(deadline)) return -SOCK_ETIMEDOUT;
        wait_until(ng, deadline);
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
    UINT64 deadline = deadline_of(s->rcvtimeo);
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
            s->rx_tail += (UINT32)(dlen - got);                 /* truncate */
            net_unlock();
            return got;
        }
        net_unlock();
        if (s->nonblock) return -SOCK_EWOULDBLOCK;
        if (wait_cancel(c, ca) || past(deadline)) return -SOCK_ETIMEDOUT;
        wait_until(ng, deadline);
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
    /* The receive side is shut here, not in lwIP: a pcb with its receive
     * side closed is freed without a word once the connection ends (no
     * error callback), which would leave s->tcp dangling for close.  The
     * send side's FIN keeps the pcb, and lwIP reports its end through
     * tcp_err_cb (ERR_CLSD). */
    if ((how == 0 || how == 2) && !s->recv_shut) {
        UINT32 n = rx_used(s);
        s->recv_shut = true;
        s->rx_tail = s->rx_head;                             /* what was not read is dropped... */
        while (n) { u16_t k = n > 0xFFFF ? 0xFFFF : (u16_t)n; tcp_recved(s->tcp, k); n -= k; }   /* ...and its window reopened */
        if (s->tcp->refused_data) tcp_process_refused_data(s->tcp);
    }
    if ((how == 1 || how == 2) && !s->send_shut) { tcp_shutdown(s->tcp, 0, 1); s->send_shut = true; }
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
        /* SO_LINGER on with a zero timeout: a hard close, a reset to the
         * peer and what was not sent dropped, as on Windows */
        if (!s->listening && s->linger_on && !s->linger_s) tcp_abort(s->tcp);
        else if (tcp_close(s->tcp) != ERR_OK) tcp_abort(s->tcp);
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

/* The bytes waiting to be read, copied without consuming them (@buf may be
 * NULL to count them); *closed says the peer has finished sending */
int NetSockPeek(int sd, void *buf, int len, bool *closed)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s) { net_unlock(); return -SOCK_ENOTSOCK; }
    int n = (int)rx_used(s);
    if (buf) n = rx_peek(s, buf, len);
    if (closed) *closed = s->peer_closed || s->reset;
    net_unlock();
    return n;
}

bool NetSockListening(int sd)
{
    net_lock();
    Sock *s = slot(sd);
    bool l = s && s->listening;
    net_unlock();
    return l;
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

/* -----------------------------------------------------------------------
 * Options
 * ----------------------------------------------------------------------- */
/* The PCB's own option bits and TTL (an IPv4/IPv6 PCB or a listener: all
 * start with lwIP's IP_PCB fields) */
static void apply_ip_opts(Sock *s)
{
    if (s->udp && s->udp_pcb) { s->udp_pcb->so_options = s->so_options; s->udp_pcb->ttl = s->ttl; }
    else if (s->tcp) {
        if (s->listening) {
            struct tcp_pcb_listen *lp = (struct tcp_pcb_listen *)s->tcp;
            lp->so_options = s->so_options; lp->ttl = s->ttl;
        } else {
            s->tcp->so_options = s->so_options; s->tcp->ttl = s->ttl;
        }
    }
}

int NetSockSetOpt(int sd, int opt, UINT32 v)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s) { net_unlock(); return -SOCK_ENOTSOCK; }
    int r = 0;
    UINT8 bit = opt == SOCKOPT_REUSEADDR ? SOF_REUSEADDR : opt == SOCKOPT_KEEPALIVE ? SOF_KEEPALIVE :
                opt == SOCKOPT_BROADCAST ? SOF_BROADCAST : 0;
    switch (opt) {
    case SOCKOPT_RCVBUF:   s->rcvbuf = v; break;
    case SOCKOPT_SNDBUF:   s->sndbuf = v; break;
    case SOCKOPT_RCVTIMEO: s->rcvtimeo = v; break;
    case SOCKOPT_SNDTIMEO: s->sndtimeo = v; break;
    case SOCKOPT_REUSEADDR:
    case SOCKOPT_KEEPALIVE:
    case SOCKOPT_BROADCAST:
        if (opt == SOCKOPT_KEEPALIVE && s->udp) { r = -SOCK_EINVAL; break; }
        s->so_options = v ? (UINT8)(s->so_options | bit) : (UINT8)(s->so_options & ~bit);
        apply_ip_opts(s);
        break;
    case SOCKOPT_TTL:
        if (v < 1 || v > 255) { r = -SOCK_EINVAL; break; }
        s->ttl = (UINT8)v;
        apply_ip_opts(s);
        break;
    case SOCKOPT_NODELAY:
        if (s->udp) { r = -SOCK_EINVAL; break; }
        s->nodelay = v != 0;
        if (s->tcp && !s->listening) {                  /* (a listener's goes to what it accepts) */
            if (s->nodelay) tcp_nagle_disable(s->tcp);
            else tcp_nagle_enable(s->tcp);
            if (s->nodelay) tcp_output(s->tcp);         /* what Nagle held back goes now */
        }
        break;
    case SOCKOPT_LINGER:
        if (s->udp) { r = -SOCK_EINVAL; break; }
        s->linger_on = (v & 0xFFFF) != 0;
        s->linger_s = (UINT16)(v >> 16);
        break;
    default: r = -SOCK_EINVAL;
    }
    net_unlock();
    return r;
}

int NetSockGetOpt(int sd, int opt, UINT32 *v)
{
    net_lock();
    Sock *s = slot(sd);
    if (!s) { net_unlock(); return -SOCK_ENOTSOCK; }
    int r = 0;
    switch (opt) {
    case SOCKOPT_RCVBUF:     *v = s->rcvbuf; break;
    case SOCKOPT_SNDBUF:     *v = s->sndbuf; break;
    case SOCKOPT_RCVTIMEO:   *v = s->rcvtimeo; break;
    case SOCKOPT_SNDTIMEO:   *v = s->sndtimeo; break;
    case SOCKOPT_REUSEADDR:  *v = (s->so_options & SOF_REUSEADDR) != 0; break;
    case SOCKOPT_KEEPALIVE:  *v = (s->so_options & SOF_KEEPALIVE) != 0; break;
    case SOCKOPT_BROADCAST:  *v = (s->so_options & SOF_BROADCAST) != 0; break;
    case SOCKOPT_TTL:        *v = s->ttl; break;
    case SOCKOPT_NODELAY:    *v = s->nodelay; break;
    case SOCKOPT_LINGER:     *v = (UINT32)s->linger_on | ((UINT32)s->linger_s << 16); break;
    case SOCKOPT_TYPE:       *v = s->udp ? 2 : 1; break;
    case SOCKOPT_ACCEPTCONN: *v = s->listening; break;
    case SOCKOPT_ERROR:      /* a failed connect, or a connection reset */
        *v = !s->reset ? 0 : s->connected ? SOCK_ECONNRESET : SOCK_ECONNREFUSED;
        break;
    default: r = -SOCK_EINVAL;
    }
    net_unlock();
    return r;
}
