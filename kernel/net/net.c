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

#include "net_internal.h"
#include "tls.h"
#include "../drivers/e1000.h"
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
static struct raw_pcb  *g_ping_pcb;

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
 * Network interface ↔ e1000
 * ----------------------------------------------------------------------- */
static err_t link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    static UINT8 frame[E1000_MTU_FRAME + 16];
    UINT16 len = pbuf_copy_partial(p, frame, sizeof(frame), 0);
    for (int i = 0; i < 1000; i++) {
        if (E1000Transmit(frame, len)) return ERR_OK;
        pause_cpu();
    }
    return ERR_IF;
}

static err_t netif_setup(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = 'n';
    n->hwaddr_len = 6;
    E1000Mac(n->hwaddr);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = link_output;
    netif_set_hostname(n, "nova-pc");
    return ERR_OK;
}

static void poll_input(void)
{
    static UINT8 buf[2048];
    for (int budget = 32; budget > 0; budget--) {
        int n = E1000Receive(buf, sizeof(buf));
        if (n <= 0) break;
        UINT64 t = rdtsc();                       /* arrival timing: entropy */
        TlsStirEntropy(&t, sizeof(t));
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p) continue;
        pbuf_take(p, buf, (u16_t)n);
        if (g_netif.input(p, &g_netif) != ERR_OK) pbuf_free(p);
    }
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
 * DNS
 * ----------------------------------------------------------------------- */
static void dns_found(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    NetOp *op = arg;
    if (op->released) { net_op_free(op); return; }
    if (addr) { op->ip = ip4_addr_get_u32(ip_2_ip4(addr)); net_op_done(op); }
    else      net_op_fail(op, "Host not found");
}

NetOp *NetResolve(const char *host)
{
    net_lock();
    NetOp *op = net_op_alloc(NETOP_DNS);
    if (!op) { net_unlock(); return NULL; }
    if (!g_up)      { net_op_fail(op, "Network is not available"); net_unlock(); return op; }
    ip_addr_t addr;
    err_t e = dns_gethostbyname(host, &addr, dns_found, op);
    if (e == ERR_OK)               { op->ip = ip4_addr_get_u32(ip_2_ip4(&addr)); net_op_done(op); }
    else if (e != ERR_INPROGRESS)  net_op_fail(op, e == ERR_ARG ? "Invalid host name" : "DNS lookup failed");
    net_unlock();
    return op;
}

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
    st->present = E1000Present();
    st->adapter = E1000Name();
    if (!st->present) return;
    net_lock();
    E1000Mac(st->mac);
    st->link = g_link;
    st->configured = g_up && dhcp_supplied_address(&g_netif);
    st->ip   = ip4_addr_get_u32(netif_ip4_addr(&g_netif));
    st->mask = ip4_addr_get_u32(netif_ip4_netmask(&g_netif));
    st->gw   = ip4_addr_get_u32(netif_ip4_gw(&g_netif));
    for (int i = 0; i < 2; i++)
        st->dns[i] = ip4_addr_get_u32(ip_2_ip4(dns_getserver((u8_t)i)));
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
        bool link = E1000LinkUp();
        if (link != g_link) {
            g_link = link;
            if (link) netif_set_link_up(&g_netif); else netif_set_link_down(&g_netif);
        }
        poll_input();
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
        net_unlock();
        sched_yield();
    }
}

bool NetInitialize(void)
{
    TlsInit();                          /* (root store works without a NIC) */
    if (!E1000Init()) return false;

    UINT8 mac[6];
    E1000Mac(mac);
    TlsStirEntropy(mac, sizeof(mac));
    lwip_init();
    if (!netif_add_noaddr(&g_netif, NULL, netif_setup, netif_input)) {
        kprintf("[NET] netif_add failed\n");
        return false;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    g_link = E1000LinkUp();
    if (g_link) netif_set_link_up(&g_netif);

    g_ping_pcb = raw_new(IP_PROTO_ICMP);
    if (g_ping_pcb) raw_recv(g_ping_pcb, ping_recv, NULL);

    if (dhcp_start(&g_netif) != ERR_OK) kprintf("[NET] DHCP could not start\n");
    g_up = true;

    sched_create_thread_ex("net", net_thread, NULL, 8, NET_STACK);
    kprintf("[NET] lwIP %s ready; requesting an address via DHCP\n", LWIP_VERSION_STRING);
    return true;
}
