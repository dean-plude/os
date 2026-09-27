/*
 * net.c — NovaOS networking: lwIP glue, the net thread, async operations
 *
 * Threading: lwIP is configured NO_SYS (see port/lwipopts.h).  Every call
 * into lwIP happens with g_lock held — from the net thread (receive path,
 * timers, timeouts) or from app threads (starting/releasing operations).
 * lwIP callbacks therefore also run under the lock and only record data.
 * HTTPS connections run BearSSL (tls.c) from the same callbacks, so the
 * TLS handshake's public-key work happens on the net thread.
 */

#include "net.h"
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
#define HTTP_TIMEOUT    3000                   /* ticks (30 s) */
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
static void net_lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        sched_yield();
}

static void net_unlock(void)
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
static NetOp *op_alloc(NetOpKind kind)
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

static bool strstr_simple(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) if (!strncmp(s, needle, n)) return true;
    return false;
}

static void op_free(NetOp *op)
{
    kfree(op->data);
    op->data = NULL;
    TlsFree(op->tls);
    op->tls = NULL;
    op->in_use = false;
}

static void op_fail(NetOp *op, const char *why)
{
    ksnprintf(op->error, sizeof(op->error), "%s", why);
    __atomic_store_n(&op->state, NET_FAILED, __ATOMIC_RELEASE);
}

static void op_done(NetOp *op)
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
    if (op->released) { op_free(op); return; }
    if (addr) { op->ip = ip4_addr_get_u32(ip_2_ip4(addr)); op_done(op); }
    else      op_fail(op, "Host not found");
}

NetOp *NetResolve(const char *host)
{
    net_lock();
    NetOp *op = op_alloc(NETOP_DNS);
    if (!op) { net_unlock(); return NULL; }
    if (!g_up)      { op_fail(op, "Network is not available"); net_unlock(); return op; }
    ip_addr_t addr;
    err_t e = dns_gethostbyname(host, &addr, dns_found, op);
    if (e == ERR_OK)               { op->ip = ip4_addr_get_u32(ip_2_ip4(&addr)); op_done(op); }
    else if (e != ERR_INPROGRESS)  op_fail(op, e == ERR_ARG ? "Invalid host name" : "DNS lookup failed");
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
            op_done(op);
            break;
        }
    }
    pbuf_free(p);
    return 1;                                    /* consumed */
}

NetOp *NetPing(UINT32 ip_be, UINT16 seq)
{
    net_lock();
    NetOp *op = op_alloc(NETOP_PING);
    if (!op) { net_unlock(); return NULL; }
    op->ip = ip_be;
    op->seq = seq;
    if (!g_up || !g_ping_pcb) { op_fail(op, "Network is not available"); net_unlock(); return op; }

    const u16_t data_len = 32;
    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)(sizeof(struct icmp_echo_hdr) + data_len), PBUF_RAM);
    if (!p) { op_fail(op, "Out of memory"); net_unlock(); return op; }
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
    if (raw_sendto(g_ping_pcb, p, &dst) != ERR_OK) op_fail(op, "Send failed");
    pbuf_free(p);
    net_unlock();
    return op;
}

/* -----------------------------------------------------------------------
 * HTTP GET over a raw TCP PCB
 * ----------------------------------------------------------------------- */
/* Stop callbacks and close the connection.  Returns ERR_ABRT if it had to
 * be aborted (a recv callback must then return ERR_ABRT), else ERR_OK. */
static err_t http_detach(NetOp *op)
{
    struct tcp_pcb *pcb = op->pcb;
    if (!pcb) return ERR_OK;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    op->pcb = NULL;
    if (tcp_close(pcb) == ERR_OK) return ERR_OK;
    tcp_abort(pcb);
    return ERR_ABRT;
}

static void http_err(void *arg, err_t err)
{
    NetOp *op = arg;
    if (!op) return;
    op->pcb = NULL;                               /* lwIP already freed it */
    if (op->state == NET_PENDING)
        op_fail(op, err == ERR_RST ? "Connection reset" :
                    err == ERR_ABRT ? "Connection aborted" : "Could not connect");
}

/* Append response bytes; false (op failed) if too large or out of memory. */
static bool op_append(NetOp *op, const void *data, UINT32 n)
{
    if (op->len + n > op->cap) {
        UINT32 need = op->len + n, cap = op->cap ? op->cap : 64 * 1024;
        while (cap < need) cap *= 2;
        if (cap > NET_HTTP_MAX) cap = NET_HTTP_MAX;
        if (need > cap) { op_fail(op, "Response is too large"); return false; }
        char *nb = kmalloc(cap);
        if (!nb) { op_fail(op, "Out of memory"); return false; }
        if (op->len) memcpy(nb, op->data, op->len);
        kfree(op->data);
        op->data = nb;
        op->cap = cap;
    }
    memcpy(op->data + op->len, data, n);
    op->len += n;
    return true;
}

/* ---- TLS ---------------------------------------------------------------
 * The BearSSL engine is a state machine with four buffers: records to
 * send (→ tcp_write), records received (← TCP), application data
 * received (→ op->data) and application data to send (← the request).
 * tls_pump() moves bytes until nothing more can progress. */
static void tls_describe(NetOp *op, br_ssl_engine_context *eng)
{
    static const struct { UINT16 id; const char *name; } suites[] = {
        { BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       "ECDHE-ECDSA-AES128-GCM-SHA256" },
        { BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       "ECDHE-ECDSA-AES256-GCM-SHA384" },
        { BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,         "ECDHE-RSA-AES128-GCM-SHA256" },
        { BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,         "ECDHE-RSA-AES256-GCM-SHA384" },
        { BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, "ECDHE-ECDSA-CHACHA20-POLY1305" },
        { BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,   "ECDHE-RSA-CHACHA20-POLY1305" },
        { BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,         "ECDHE-RSA-AES128-SHA256" },
        { BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,            "ECDHE-RSA-AES128-SHA" },
        { BR_TLS_RSA_WITH_AES_128_GCM_SHA256,               "RSA-AES128-GCM-SHA256" },
        { BR_TLS_RSA_WITH_AES_256_GCM_SHA384,               "RSA-AES256-GCM-SHA384" },
        { BR_TLS_RSA_WITH_AES_128_CBC_SHA,                  "RSA-AES128-SHA" },
    };
    unsigned v = br_ssl_engine_get_version(eng);
    UINT16 cs = eng->session.cipher_suite;
    const char *name = NULL;
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++)
        if (suites[i].id == cs) name = suites[i].name;
    char hex[16];
    if (!name) { ksnprintf(hex, sizeof(hex), "suite 0x%04x", cs); name = hex; }
    ksnprintf(op->tls_info, sizeof(op->tls_info), "TLS 1.%u, %s",
              v >= 0x0301 ? v - 0x0301 : 0, name);
}

static err_t tls_pump(NetOp *op)
{
    TlsConn *c = op->tls;
    struct tcp_pcb *pcb = op->pcb;
    if (!c || !pcb) return ERR_OK;
    br_ssl_engine_context *eng = &c->cc.eng;
    bool wrote = false;

    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) {
            int e = br_ssl_engine_last_error(eng);
            err_t r = http_detach(op);
            if (e == BR_ERR_OK) op_done(op);                 /* close_notify */
            else if (op->state == NET_PENDING) {
                char buf[64];
                op_fail(op, TlsErrorText(e, buf, sizeof(buf)));
            }
            return r;
        }
        bool progress = false;
        size_t len;
        if (st & BR_SSL_SENDREC) {
            unsigned char *b = br_ssl_engine_sendrec_buf(eng, &len);
            size_t room = tcp_sndbuf(pcb);
            if (len > room) len = room;
            if (len > 0xFFFF) len = 0xFFFF;
            if (len && tcp_write(pcb, b, (u16_t)len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
                br_ssl_engine_sendrec_ack(eng, len);
                progress = wrote = true;
            }
        }
        if (st & BR_SSL_RECVAPP) {
            unsigned char *b = br_ssl_engine_recvapp_buf(eng, &len);
            if (!op_append(op, b, (UINT32)len)) return http_detach(op);
            br_ssl_engine_recvapp_ack(eng, len);
            progress = true;
        }
        if ((st & BR_SSL_SENDAPP) && !op->tls_info[0]) tls_describe(op, eng);
        if ((st & BR_SSL_SENDAPP) && !op->req_sent) {
            unsigned char *b = br_ssl_engine_sendapp_buf(eng, &len);
            size_t n = strlen(op->request);
            if (len >= n) {
                memcpy(b, op->request, n);
                br_ssl_engine_sendapp_ack(eng, n);
                br_ssl_engine_flush(eng, 0);
                op->req_sent = true;
                progress = true;
            }
        }
        if (!progress) break;
    }
    if (wrote) tcp_output(pcb);
    return ERR_OK;
}

/* Feed received TLS records into the engine. */
static err_t tls_input(NetOp *op, struct pbuf *p)
{
    br_ssl_engine_context *eng = &((TlsConn *)op->tls)->cc.eng;
    u16_t off = 0;
    while (off < p->tot_len) {
        size_t room;
        unsigned char *b = br_ssl_engine_recvrec_buf(eng, &room);
        if (!b) {
            /* the engine must output first (or has closed) */
            err_t r = tls_pump(op);
            if (!op->pcb) return r;
            b = br_ssl_engine_recvrec_buf(eng, &room);
            if (!b) { op_fail(op, "TLS engine stalled"); return http_detach(op); }
        }
        u16_t n = (u16_t)(p->tot_len - off < room ? p->tot_len - off : room);
        pbuf_copy_partial(p, b, n, off);
        off = (u16_t)(off + n);
        br_ssl_engine_recvrec_ack(eng, n);
        err_t r = tls_pump(op);
        if (!op->pcb) return r;
    }
    return ERR_OK;
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb; (void)len;
    NetOp *op = arg;
    return (op && op->tls && op->pcb) ? tls_pump(op) : ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    NetOp *op = arg;
    if (!p) {                                     /* server closed */
        err_t r = http_detach(op);
        if (op->state != NET_PENDING) return r;
        /* Plain HTTP/1.0 ends at FIN.  Over TLS most servers also just
         * close after "Connection: close" without a close_notify; accept
         * that once the response has arrived (as browsers do). */
        if (!op->tls || op->len) op_done(op);
        else {
            int e = br_ssl_engine_last_error(&((TlsConn *)op->tls)->cc.eng);
            char buf[64];
            op_fail(op, e ? TlsErrorText(e, buf, sizeof(buf))
                          : "Connection closed during the TLS handshake");
        }
        return r;
    }
    if (err != ERR_OK) { pbuf_free(p); return err; }

    u16_t n = p->tot_len;
    err_t r = ERR_OK;
    if (op->tls) {
        r = tls_input(op, p);
    } else {
        char tmp[512];
        for (u16_t off = 0; off < n; ) {
            u16_t k = pbuf_copy_partial(p, tmp, sizeof(tmp), off);
            if (!op_append(op, tmp, k)) { r = http_detach(op); break; }
            off = (u16_t)(off + k);
        }
    }
    if (op->pcb) tcp_recved(pcb, n);
    pbuf_free(p);
    return r;
}

static err_t http_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    NetOp *op = arg;
    if (err != ERR_OK) { op_fail(op, "Could not connect"); return err; }
    if (op->tls) return tls_pump(op);             /* sends the ClientHello */
    size_t n = strlen(op->request);
    if (tcp_write(pcb, op->request, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        op_fail(op, "Send failed");
        return http_detach(op);
    }
    tcp_output(pcb);
    return ERR_OK;
}

NetOp *NetHttpGet(UINT32 ip_be, UINT16 port, const char *host, const char *path,
                  bool https)
{
    net_lock();
    NetOp *op = op_alloc(NETOP_HTTP);
    if (!op) { net_unlock(); return NULL; }
    op->ip = ip_be;
    op->deadline = sched_ticks() + HTTP_TIMEOUT;
    if (!g_up) { op_fail(op, "Network is not available"); net_unlock(); return op; }

    ksnprintf(op->request, sizeof(op->request),
              "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: NovaOS/0.9\r\n"
              "Accept: */*\r\nConnection: close\r\n\r\n",
              path && *path ? path : "/", host);

    if (https) {
        op->tls = TlsClientNew(host);
        if (!op->tls) { op_fail(op, "Could not start TLS"); net_unlock(); return op; }
    }
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { op_fail(op, "Out of connections"); net_unlock(); return op; }
    op->pcb = pcb;
    tcp_arg(pcb, op);
    tcp_err(pcb, http_err);
    tcp_recv(pcb, http_recv);
    tcp_sent(pcb, http_sent);
    ip_addr_t dst;
    ip_addr_set_ip4_u32(&dst, ip_be);
    if (tcp_connect(pcb, &dst, port, http_connected) != ERR_OK) {
        http_detach(op);
        op_fail(op, "Could not connect");
    }
    net_unlock();
    return op;
}

int NetHttpParse(const NetOp *op, const char **body, UINT32 *body_len,
                 char *location, int loc_cap)
{
    if (location && loc_cap) location[0] = '\0';
    if (!op || !op->data || op->len < 12 || memcmp(op->data, "HTTP/", 5)) return 0;
    const char *d = op->data, *end = d + op->len;
    const char *sp = memchr(d, ' ', (size_t)(end - d));
    if (!sp || end - sp < 4) return 0;
    int status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');

    const char *hdr_end = NULL;
    for (const char *p = d; p + 3 < end; p++)
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') { hdr_end = p; break; }
    if (!hdr_end) return 0;

    if (location) {
        for (const char *p = d; p < hdr_end; p++) {
            if ((p == d || p[-1] == '\n') && end - p > 9) {
                const char *k = "location:";
                int i = 0;
                while (k[i] && (p[i] | 0x20) == k[i]) i++;
                if (!k[i]) {
                    const char *v = p + 9;
                    while (v < hdr_end && *v == ' ') v++;
                    int n = 0;
                    while (v < hdr_end && *v != '\r' && n < loc_cap - 1) location[n++] = *v++;
                    location[n] = '\0';
                }
            }
        }
    }
    *body = hdr_end + 4;
    *body_len = (UINT32)(end - (hdr_end + 4));
    return status;
}

bool NetParseUrl(const char *url, char *host, int host_cap, UINT16 *port,
                 char *path, int path_cap, bool *https)
{
    *https = false;
    *port  = 80;
    const char *p = url;
    if (!strncmp(p, "http://", 7))       p += 7;
    else if (!strncmp(p, "https://", 8)) { p += 8; *https = true; *port = 443; }
    else if (strstr_simple(p, "://"))    return false;       /* other scheme */

    int n = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (n >= host_cap - 1) return false;
        host[n++] = *p++;
    }
    host[n] = '\0';
    if (!n) return false;
    if (*p == ':') {
        unsigned v = 0;
        p++;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
        if (!v || v > 65535) return false;
        *port = (UINT16)v;
    }
    n = 0;
    if (*p != '/') path[n++] = '/';               /* "host?q" -> "/?q" */
    while (*p && *p != '#' && n < path_cap - 1) path[n++] = *p++;
    path[n] = '\0';
    return true;
}

void NetRelease(NetOp *op)
{
    if (!op) return;
    net_lock();
    if (op->state == NET_PENDING && op->kind == NETOP_DNS) {
        op->released = true;                      /* dns_found frees it */
    } else {
        if (op->kind == NETOP_HTTP) http_detach(op);
        op_free(op);
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
            op_fail(op, "Request timed out");
        if (op->kind == NETOP_HTTP && now > op->deadline) {
            http_detach(op);
            op_fail(op, "Timed out");
        }
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
    if (!E1000Init()) return false;

    TlsInit();
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
