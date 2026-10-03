/*
 * net.c — NovaOS networking: lwIP glue, the net thread, async operations
 *
 * Threading: lwIP is configured NO_SYS (see port/lwipopts.h).  Every call
 * into lwIP happens with g_lock held — from the net thread (receive path,
 * timers, timeouts) or from app threads (starting/releasing operations).
 * lwIP callbacks therefore also run under the lock and only record data.
 * The HTTP/HTTPS client lives in http.c; TLS (Mbed TLS, tls.c) runs from
 * the same callbacks, so handshake crypto happens on the net thread.
 */

#include "../ke/waitq.h"
#include "net_internal.h"
#include "tls.h"
#include "../drivers/e1000.h"
#include "../drivers/virtio_net.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../arch/x86_64/cpu.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/raw.h"
#include "lwip/ip6_addr.h"
#include "lwip/nd6.h"
#include "lwip/mld6.h"
#include "lwip/prot/icmp6.h"
#include "lwip/ethip6.h"
#include "lwip/ip.h"
#include "lwip/tcp.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/ip4_addr.h"
#include "netif/etharp.h"
#include "netif/ethernet.h"

#define NET_OPS         32
#define PING_ID         0x4E4F                 /* "NO" */
#define PING_TIMEOUT    200                    /* ticks (2 s) */
#define NET_STACK       (64 * 1024)            /* net thread: room for TLS crypto */

static struct netif     g_netif;
static bool             g_up;
static bool             g_link;
static volatile int     g_lock;
static NetOp            g_ops[NET_OPS];
static struct raw_pcb  *g_ping_pcb, *g_ping6_pcb;

/* The adapter: an Intel e1000/e1000e or a virtio one, the first found */
typedef struct {
    bool        (*present)(void);
    const char *(*name)(void);
    void        (*mac)(UINT8 mac[6]);
    bool        (*link_up)(void);
    bool        (*transmit)(const void *frame, UINT16 len);
    int         (*receive)(void *buf, int cap);
    void        (*resume)(void);
} Nic;
static const Nic g_e1000  = { E1000Present, E1000Name, E1000Mac, E1000LinkUp, E1000Transmit, E1000Receive, E1000Resume };
static const Nic g_virtio = { VirtioNetPresent, VirtioNetName, VirtioNetMac, VirtioNetLinkUp,
                              VirtioNetTransmit, VirtioNetReceive, VirtioNetResume };
static const Nic *g_nic = &g_e1000;

/* -----------------------------------------------------------------------
 * Platform hooks for lwIP (port/arch/cc.h)
 * ----------------------------------------------------------------------- */
u32_t sys_now(void) { return (u32_t)(sched_ticks() * 10); }

unsigned int net_random(void)
{
    static UINT64 s;
    if (!s) s = rdtsc() | 1;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return (unsigned int)s;
}

void net_assert_fail(const char *msg, const char *file, int line)
{
    kprintf("[NET] lwIP assertion failed: %s (%s:%d)\n", msg, file, line);
    for (;;) { cli(); hlt(); }
}

/* -----------------------------------------------------------------------
 * Lock
 * ----------------------------------------------------------------------- */
void net_lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        sched_yield();
}

void net_unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * Network waits (net_internal.h)
 * ----------------------------------------------------------------------- */
static WaitQueue g_netq = WAITQ_INIT;

UINT32 net_gen(void)                              { return waitq_gen(&g_netq); }
void   net_wake(void)                             { waitq_wake(&g_netq); }
void   net_wait_ticks(UINT32 gen, UINT64 max_ticks) { waitq_wait(&g_netq, gen, max_ticks); }
void   net_wait(UINT32 gen)                       { waitq_wait(&g_netq, gen, 10); }

/* -----------------------------------------------------------------------
 * Network interface ↔ the adapter
 * ----------------------------------------------------------------------- */
static err_t link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    static UINT8 frame[E1000_MTU_FRAME + 16];
    UINT16 len = pbuf_copy_partial(p, frame, sizeof(frame), 0);
    for (int i = 0; i < 1000; i++) {
        if (g_nic->transmit(frame, len)) return ERR_OK;
        pause_cpu();
    }
    return ERR_IF;
}

static err_t netif_setup(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = 'n';
    n->hwaddr_len = 6;
    g_nic->mac(n->hwaddr);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6;
    n->output = etharp_output;
    n->output_ip6 = ethip6_output;
    n->linkoutput = link_output;
    netif_set_hostname(n, "nova-pc");
    return ERR_OK;
}

/* Returns the number of frames taken in */
static int poll_input(void)
{
    static UINT8 buf[2048];
    int got = 0;
    for (int budget = 32; budget > 0; budget--) {
        int n = g_nic->receive(buf, sizeof(buf));
        if (n <= 0) break;
        got++;
        UINT64 t = rdtsc();                       /* arrival timing: entropy */
        TlsStirEntropy(&t, sizeof(t));
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p) continue;
        pbuf_take(p, buf, (u16_t)n);
        if (g_netif.input(p, &g_netif) != ERR_OK) pbuf_free(p);
    }
    return got;
}

/* -----------------------------------------------------------------------
 * Operation pool
 * ----------------------------------------------------------------------- */
NetOp *net_op_alloc(NetOpKind kind)
{
    for (int i = 0; i < NET_OPS; i++) {
        NetOp *op = &g_ops[i];
        if (op->in_use) continue;
        memset(op, 0, sizeof(*op));
        op->in_use  = true;
        op->kind    = kind;
        op->state   = NET_PENDING;
        op->started = sched_ticks();
        return op;
    }
    return NULL;
}

void net_op_free(NetOp *op)
{
    kfree(op->data);
    op->data = NULL;
    op->in_use = false;
}

void net_op_fail(NetOp *op, const char *why)
{
    ksnprintf(op->error, sizeof(op->error), "%s", why);
    __atomic_store_n(&op->state, NET_FAILED, __ATOMIC_RELEASE);
}

void net_op_done(NetOp *op)
{
    __atomic_store_n(&op->state, NET_DONE, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * Addresses
 * ----------------------------------------------------------------------- */
void net_addr_to_lwip(const NetIp *a, void *out)
{
    ip_addr_t *o = out;
    if (!a->v6) {
        UINT32 v; memcpy(&v, a->a, 4);
        ip_addr_set_ip4_u32(o, v);
        return;
    }
    UINT32 w[4]; memcpy(w, a->a, 16);
    IP_ADDR6(o, w[0], w[1], w[2], w[3]);
    ip6_addr_assign_zone(ip_2_ip6(o), IP6_UNKNOWN, &g_netif);
}

void net_addr_from_lwip(const void *in, NetIp *out)
{
    const ip_addr_t *a = in;
    memset(out, 0, sizeof(*out));
    if (IP_IS_V6(a)) {
        out->v6 = true;
        memcpy(out->a, ip_2_ip6(a)->addr, 16);
    } else {
        UINT32 v = ip4_addr_get_u32(ip_2_ip4(a));
        memcpy(out->a, &v, 4);
    }
}

void NetFormatAddr(const NetIp *ip, char *buf, int cap)
{
    if (!ip->v6) { UINT32 v; memcpy(&v, ip->a, 4); NetFormatIp(v, buf, cap); return; }
    ip6_addr_t a;
    memcpy(a.addr, ip->a, 16);
    ip6_addr_clear_zone(&a);
    if (!ip6addr_ntoa_r(&a, buf, cap) && cap) buf[0] = '\0';
    for (char *p = buf; *p; p++)                  /* (lowercase, as Windows writes them) */
        if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
}

bool NetParseAddr(const char *s, NetIp *ip)
{
    memset(ip, 0, sizeof(*ip));
    UINT32 v4;
    if (NetParseIp(s, &v4)) { memcpy(ip->a, &v4, 4); return true; }
    ip6_addr_t a;
    if (!strchr(s, ':') || !ip6addr_aton(s, &a)) return false;
    ip->v6 = true;
    memcpy(ip->a, a.addr, 16);
    return true;
}

static bool has_global_ip6(void)
{
    for (int i = 1; i < LWIP_IPV6_NUM_ADDRESSES; i++)
        if (ip6_addr_isvalid(netif_ip6_addr_state(&g_netif, i)) &&
            !ip6_addr_islinklocal(netif_ip6_addr(&g_netif, i)))
            return true;
    return false;
}

bool NetHasIp6(void)
{
    net_lock();
    bool r = g_up && has_global_ip6();
    net_unlock();
    return r;
}

/* -----------------------------------------------------------------------
 * DNS
 * ----------------------------------------------------------------------- */
static void set_result(NetOp *op, const ip_addr_t *addr)
{
    net_addr_from_lwip(addr, &op->addr);
    op->ip = op->addr.v6 ? 0 : ip4_addr_get_u32(ip_2_ip4(addr));
}

static void dns_found(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    NetOp *op = arg;
    if (op->released) { net_op_free(op); return; }
    if (addr) { set_result(op, addr); net_op_done(op); }
    else      net_op_fail(op, "Host not found");
}

NetOp *NetResolveEx(const char *host, int family)
{
    net_lock();
    NetOp *op = net_op_alloc(NETOP_DNS);
    if (!op) { net_unlock(); return NULL; }
    if (!g_up)      { net_op_fail(op, "Network is not available"); net_unlock(); return op; }
    NetIp lit;                                    /* an address literal: no lookup */
    if (NetParseAddr(host, &lit)) {
        if ((family == 4 && lit.v6) || (family == 6 && !lit.v6)) net_op_fail(op, "Wrong address family");
        else { op->addr = lit; if (!lit.v6) memcpy(&op->ip, lit.a, 4); net_op_done(op); }
        net_unlock();
        return op;
    }
    u8_t type = family == 4 ? LWIP_DNS_ADDRTYPE_IPV4 : family == 6 ? LWIP_DNS_ADDRTYPE_IPV6
              : has_global_ip6() ? LWIP_DNS_ADDRTYPE_IPV6_IPV4 : LWIP_DNS_ADDRTYPE_IPV4_IPV6;
    ip_addr_t addr;
    err_t e = dns_gethostbyname_addrtype(host, &addr, dns_found, op, type);
    if (e == ERR_OK)               { set_result(op, &addr); net_op_done(op); }
    else if (e != ERR_INPROGRESS)  net_op_fail(op, e == ERR_ARG ? "Invalid host name" : "DNS lookup failed");
    net_unlock();
    return op;
}

NetOp *NetResolve(const char *host) { return NetResolveEx(host, 0); }

/* -----------------------------------------------------------------------
 * Ping (ICMP echo over a raw PCB)
 * ----------------------------------------------------------------------- */
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)arg; (void)pcb; (void)addr;
    if (p->tot_len < IP_HLEN + sizeof(struct icmp_echo_hdr)) return 0;
    struct ip_hdr *iph = (struct ip_hdr *)p->payload;
    u16_t hlen = IPH_HL_BYTES(iph);
    struct icmp_echo_hdr echo;
    if (pbuf_copy_partial(p, &echo, sizeof(echo), hlen) != sizeof(echo)) return 0;
    if (echo.type != ICMP_ER || echo.id != lwip_htons(PING_ID)) return 0;

    u16_t seq = lwip_ntohs(echo.seqno);
    for (int i = 0; i < NET_OPS; i++) {
        NetOp *op = &g_ops[i];
        if (op->in_use && !op->released && op->kind == NETOP_PING &&
            op->state == NET_PENDING && op->seq == seq) {
            op->rtt_ms = (int)((sched_ticks() - op->started) * 10);
            op->ttl    = IPH_TTL(iph);
            net_op_done(op);
            break;
        }
    }
    pbuf_free(p);
    return 1;                                    /* consumed */
}

NetOp *NetPing(UINT32 ip_be, UINT16 seq)
{
    net_lock();
    NetOp *op = net_op_alloc(NETOP_PING);
    if (!op) { net_unlock(); return NULL; }
    op->ip = ip_be;
    memcpy(op->addr.a, &ip_be, 4);
    op->seq = seq;
    if (!g_up || !g_ping_pcb) { net_op_fail(op, "Network is not available"); net_unlock(); return op; }

    const u16_t data_len = 32;
    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)(sizeof(struct icmp_echo_hdr) + data_len), PBUF_RAM);
    if (!p) { net_op_fail(op, "Out of memory"); net_unlock(); return op; }
    struct icmp_echo_hdr *e = p->payload;
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    ICMPH_CODE_SET(e, 0);
    e->id = lwip_htons(PING_ID);
    e->seqno = lwip_htons(seq);
    e->chksum = 0;
    UINT8 *d = (UINT8 *)(e + 1);
    for (u16_t i = 0; i < data_len; i++) d[i] = (UINT8)('a' + i % 23);
    e->chksum = inet_chksum(e, p->len);

    ip_addr_t dst;
    ip_addr_set_ip4_u32(&dst, ip_be);
    op->started = sched_ticks();
    if (raw_sendto(g_ping_pcb, p, &dst) != ERR_OK) net_op_fail(op, "Send failed");
    pbuf_free(p);
    net_unlock();
    return op;
}

/* ICMPv6 echo replies (other ICMPv6 messages, such as neighbor discovery,
 * are left for lwIP) */
static u8_t ping6_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)arg; (void)pcb; (void)addr;
    u16_t hlen = (u16_t)ip_current_header_tot_len();
    struct icmp6_echo_hdr echo;
    if (pbuf_copy_partial(p, &echo, sizeof(echo), hlen) != sizeof(echo)) return 0;
    if (echo.type != ICMP6_TYPE_EREP || echo.id != lwip_htons(PING_ID)) return 0;
    u16_t seq = lwip_ntohs(echo.seqno);
    for (int i = 0; i < NET_OPS; i++) {
        NetOp *op = &g_ops[i];
        if (op->in_use && !op->released && op->kind == NETOP_PING &&
            op->state == NET_PENDING && op->seq == seq) {
            op->rtt_ms = (int)((sched_ticks() - op->started) * 10);
            op->ttl    = IP6H_HOPLIM((struct ip6_hdr *)p->payload);
            net_op_done(op);
            break;
        }
    }
    pbuf_free(p);
    return 1;
}

NetOp *NetPingAddr(const NetIp *ip, UINT16 seq)
{
    if (!ip->v6) { UINT32 v; memcpy(&v, ip->a, 4); return NetPing(v, seq); }
    net_lock();
    NetOp *op = net_op_alloc(NETOP_PING);
    if (!op) { net_unlock(); return NULL; }
    op->addr = *ip;
    op->seq = seq;
    if (!g_up || !g_ping6_pcb) { net_op_fail(op, "Network is not available"); net_unlock(); return op; }
    const u16_t data_len = 32;
    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)(sizeof(struct icmp6_echo_hdr) + data_len), PBUF_RAM);
    if (!p) { net_op_fail(op, "Out of memory"); net_unlock(); return op; }
    struct icmp6_echo_hdr *e = p->payload;
    e->type = ICMP6_TYPE_EREQ;
    e->code = 0;
    e->chksum = 0;                               /* (lwIP fills it in: chksum_reqd) */
    e->id = lwip_htons(PING_ID);
    e->seqno = lwip_htons(seq);
    UINT8 *d = (UINT8 *)(e + 1);
    for (u16_t i = 0; i < data_len; i++) d[i] = (UINT8)('a' + i % 23);
    ip_addr_t dst;
    net_addr_to_lwip(ip, &dst);
    op->started = sched_ticks();
    if (raw_sendto(g_ping6_pcb, p, &dst) != ERR_OK) net_op_fail(op, "Send failed");
    pbuf_free(p);
    net_unlock();
    return op;
}

void NetRelease(NetOp *op)
{
    if (!op) return;
    net_lock();
    if (op->state == NET_PENDING && op->kind == NETOP_DNS) {
        op->released = true;                      /* dns_found frees it */
    } else {
        if (op->kind == NETOP_HTTP) http_release(op);
        net_op_free(op);
    }
    net_unlock();
}

/* -----------------------------------------------------------------------
 * Trusted roots (tls.c; the store is shared with connections, so lock)
 * ----------------------------------------------------------------------- */
int NetRootCount(int *imported)
{
    net_lock();
    int n = TlsRootCount();
    if (imported) *imported = TlsUserRootCount();
    net_unlock();
    return n;
}

bool NetRootName(int i, char *buf, int cap, bool *imported)
{
    net_lock();
    bool ok = TlsRootName(i, buf, cap, imported);
    net_unlock();
    return ok;
}

int NetImportRoots(const void *data, UINT32 len, char *err, int err_cap)
{
    net_lock();
    int n = TlsImportRoots(data, len, err, err_cap);
    net_unlock();
    return n;
}

/* -----------------------------------------------------------------------
 * Status helpers
 * ----------------------------------------------------------------------- */
void NetFormatIp(UINT32 ip_be, char *buf, int cap)
{
    ip4_addr_t a;
    ip4_addr_set_u32(&a, ip_be);
    if (!ip4addr_ntoa_r(&a, buf, cap) && cap) buf[0] = '\0';
}

bool NetParseIp(const char *s, UINT32 *ip_be)
{
    ip4_addr_t a;
    if (!ip4addr_aton(s, &a)) return false;
    *ip_be = ip4_addr_get_u32(&a);
    return true;
}

bool NetAvailable(void) { return g_up; }
bool net_up(void)       { return g_up; }

void NetGetStatus(NetStatus *st)
{
    memset(st, 0, sizeof(*st));
    st->present = g_nic->present();
    st->adapter = g_nic->name();
    if (!st->present) return;
    net_lock();
    g_nic->mac(st->mac);
    st->link = g_link;
    st->configured = g_up && dhcp_supplied_address(&g_netif);
    st->ip   = ip4_addr_get_u32(netif_ip4_addr(&g_netif));
    st->mask = ip4_addr_get_u32(netif_ip4_netmask(&g_netif));
    st->gw   = ip4_addr_get_u32(netif_ip4_gw(&g_netif));
    for (int i = 0; i < 2; i++) {
        const ip_addr_t *d = dns_getserver((u8_t)i);
        st->dns[i] = IP_IS_V4(d) ? ip4_addr_get_u32(ip_2_ip4(d)) : 0;
    }
    for (int i = 0; i < 3 && i < DNS_MAX_SERVERS; i++)
        if (!ip_addr_isany(dns_getserver((u8_t)i))) net_addr_from_lwip(dns_getserver((u8_t)i), &st->dns_all[i]);
    st->if_index = netif_get_index(&g_netif);
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES && st->ip6_count < NET_IP6_MAX; i++) {
        u8_t state = netif_ip6_addr_state(&g_netif, i);
        if (!ip6_addr_isvalid(state) && !ip6_addr_istentative(state)) continue;
        memcpy(st->ip6[st->ip6_count], netif_ip6_addr(&g_netif, i)->addr, 16);
        st->ip6_preferred[st->ip6_count] = ip6_addr_ispreferred(state);
        st->ip6_count++;
    }
    net_unlock();
}

/* -----------------------------------------------------------------------
 * Net thread
 * ----------------------------------------------------------------------- */
static void check_timeouts(void)
{
    UINT64 now = sched_ticks();
    for (int i = 0; i < NET_OPS; i++) {
        NetOp *op = &g_ops[i];
        if (!op->in_use || op->state != NET_PENDING) continue;
        if (op->kind == NETOP_PING && now - op->started > PING_TIMEOUT)
            net_op_fail(op, "Request timed out");
    }
}

static void net_thread(void *arg)
{
    (void)arg;
    bool announced = false;
    for (;;) {
        net_lock();
        bool link = g_nic->link_up();
        if (link != g_link) {
            g_link = link;
            if (link) netif_set_link_up(&g_netif); else netif_set_link_down(&g_netif);
        }
        bool got = poll_input() > 0;
        struct netif *nf;
        NETIF_FOREACH(nf) got |= nf->loop_first != NULL;    /* to 127.0.0.1, ::1 or ourselves */
        netif_poll_all();
        bool busy = got;
        for (int i = 0; i < NET_OPS && !busy; i++) busy = g_ops[i].in_use;
        sys_check_timeouts();
        check_timeouts();
        http_poll();
        if (!announced && dhcp_supplied_address(&g_netif)) {
            char ip[16], gw[16], dns[16];
            NetFormatIp(ip4_addr_get_u32(netif_ip4_addr(&g_netif)), ip, sizeof(ip));
            NetFormatIp(ip4_addr_get_u32(netif_ip4_gw(&g_netif)), gw, sizeof(gw));
            NetFormatIp(ip4_addr_get_u32(ip_2_ip4(dns_getserver(0))), dns, sizeof(dns));
            kprintf("[NET] DHCP: address %s, gateway %s, DNS %s\n", ip, gw, dns);
            announced = true;
        }
        for (int i = 1; i < LWIP_IPV6_NUM_ADDRESSES; i++) {     /* SLAAC addresses, once each */
            static UINT8 told[LWIP_IPV6_NUM_ADDRESSES][16];
            const ip6_addr_t *a = netif_ip6_addr(&g_netif, i);
            if (!ip6_addr_ispreferred(netif_ip6_addr_state(&g_netif, i)) || !memcmp(told[i], a->addr, 16)) continue;
            memcpy(told[i], a->addr, 16);
            char buf[48], dns[48] = "none";
            NetIp x; ip_addr_t ia; ip_addr_copy_from_ip6(ia, *a);
            net_addr_from_lwip(&ia, &x);
            NetFormatAddr(&x, buf, sizeof(buf));
            for (int k = 0; k < DNS_MAX_SERVERS; k++)
                if (IP_IS_V6(dns_getserver((u8_t)k)) && !ip_addr_isany(dns_getserver((u8_t)k))) {
                    net_addr_from_lwip(dns_getserver((u8_t)k), &x);
                    NetFormatAddr(&x, dns, sizeof(dns));
                    break;
                }
            kprintf("[NET] IPv6: address %s (from a router advertisement), DNS %s\n", buf, dns);
        }
        net_unlock();
        if (busy) net_wake();               /* data, connections or operations moved on */
        /* Nothing arriving, no operation under way and nobody waiting for
         * the network: poll again at the next tick instead of keeping a CPU
         * busy.  (The adapter is polled, so replies to a waiting program
         * are picked up only as fast as this loop comes round.) */
        if (busy || g_netq.sleepers) sched_yield(); else sched_sleep_tick();
    }
}

bool NetInitialize(void)
{
    TlsInit();                          /* (root store works without a NIC) */
    if (E1000Init()) g_nic = &g_e1000;
    else if (VirtioNetInit()) g_nic = &g_virtio;
    else return false;

    UINT8 mac[6];
    g_nic->mac(mac);
    TlsStirEntropy(mac, sizeof(mac));
    lwip_init();
    if (!netif_add_noaddr(&g_netif, NULL, netif_setup, netif_input)) {
        kprintf("[NET] netif_add failed\n");
        return false;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    g_link = g_nic->link_up();
    if (g_link) netif_set_link_up(&g_netif);

    g_ping_pcb = raw_new_ip_type(IPADDR_TYPE_V4, IP_PROTO_ICMP);
    if (g_ping_pcb) raw_recv(g_ping_pcb, ping_recv, NULL);
    g_ping6_pcb = raw_new_ip_type(IPADDR_TYPE_V6, IP6_NEXTH_ICMP6);
    if (g_ping6_pcb) {
        g_ping6_pcb->chksum_reqd = 1;               /* (ICMPv6's covers a pseudo-header) */
        g_ping6_pcb->chksum_offset = 2;
        raw_recv(g_ping6_pcb, ping6_recv, NULL);
    }

    /* IPv6: a link-local address now, global ones from router
     * advertisements (SLAAC), DNS servers from their RDNSS option */
    netif_create_ip6_linklocal_address(&g_netif, 1);
    netif_set_ip6_autoconfig_enabled(&g_netif, 1);

    if (dhcp_start(&g_netif) != ERR_OK) kprintf("[NET] DHCP could not start\n");
    g_up = true;

    sched_create_thread_ex("net", net_thread, NULL, 8, NET_STACK);
    kprintf("[NET] lwIP %s ready; requesting an address via DHCP\n", LWIP_VERSION_STRING);
    return true;
}

/* After S3: set the adapter up again */
void NetResume(void)
{
    g_nic->resume();
}
