/*
 * http.c — NovaOS HTTP/1.1 client over lwIP TCP, with HTTPS (tls.c)
 *
 * Connections are pooled: after a response that the server framed
 * (Content-Length or chunked) and did not end with "Connection: close",
 * the connection stays open for up to IDLE_TIMEOUT and the next request to
 * the same server reuses it — no new TCP or TLS handshake.  If a reused
 * connection turns out to have been closed by the server before any of
 * the response arrived, the request is retried once on a new connection.
 * New HTTPS connections resume a cached TLS session when the server
 * allows it (tls.c).  Responses must be complete: a connection that closes
 * before the announced length (or the final chunk) is an error.
 *
 * Runs under the network lock: lwIP callbacks come from the net thread,
 * NetHttpGet()/NetRelease() from app threads (net.c takes the lock).
 */

#include "net_internal.h"
#include "tls.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../mm/vmm.h"
#include "../lib/string.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

#define HTTP_CONNS      16
#define HTTP_TIMEOUT    3000        /* ticks (30 s) for a whole request */
#define IDLE_TIMEOUT    3000        /* ticks an idle connection is kept */

typedef enum { R_HEADERS, R_LENGTH, R_CHUNKED, R_UNTIL_CLOSE } RState;
typedef enum { CH_SIZE, CH_DATA, CH_DATA_END, CH_TRAILER } ChState;

typedef struct {
    bool            used;
    struct tcp_pcb *pcb;
    TlsConn        *tls;
    bool            connected, tls_ready;
    UINT32          ip;
    UINT16          port;
    bool            https;
    char            host[128];
    char            tls_info[64];
    bool            resumed;
    NetOp          *op;             /* request in flight, NULL when idle */
    UINT64          idle_since;
    int             requests;

    UINT32          req_off;        /* bytes of op->request sent */
    RState          rs;
    UINT32          scan;           /* header terminator search position */
    UINT32          hdr_len;        /* status line + headers + blank line */
    UINT64          body_len;       /* R_LENGTH */
    ChState         cs;
    UINT64          chunk_left;
    UINT32          pos;            /* R_CHUNKED: bytes before this are decoded */
    bool            close_after;    /* server closes after this response */
} Conn;

static Conn g_conns[HTTP_CONNS];

/* lwIP: a callback whose pcb we aborted must return ERR_ABRT */
static struct tcp_pcb *g_cb_pcb;
static bool            g_cb_aborted;

static err_t cb_begin(struct tcp_pcb *pcb) { g_cb_pcb = pcb; g_cb_aborted = false; return ERR_OK; }
static err_t cb_end(void)
{
    err_t r = g_cb_aborted ? ERR_ABRT : ERR_OK;
    g_cb_pcb = NULL;
    return r;
}

static void submit(NetOp *op, UINT32 ip, UINT16 port, const char *host, bool https);

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
bool net_op_append(NetOp *op, const void *data, UINT32 n)
{
    if (op->len + n > op->cap) {
        UINT32 need = op->len + n, cap = op->cap ? op->cap : 64 * 1024;
        while (cap < need) cap *= 2;
        if (cap > NET_HTTP_MAX) cap = NET_HTTP_MAX;
        if (need > cap) { net_op_fail(op, "Response is too large"); return false; }
        char *nb = kmalloc(cap);
        if (!nb) { net_op_fail(op, "Out of memory"); return false; }
        if (op->len) memcpy(nb, op->data, op->len);
        kfree(op->data);
        op->data = nb;
        op->cap = cap;
    }
    memcpy(op->data + op->len, data, n);
    op->len += n;
    return true;
}

static bool ieq(const char *a, const char *b)
{
    while (*a && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return !*a && !*b;
}

/* case-insensitive: does [s, s+n) contain word? */
static bool has_token(const char *s, UINT32 n, const char *word)
{
    size_t w = strlen(word);
    for (UINT32 i = 0; i + w <= n; i++) {
        size_t k = 0;
        while (k < w && (s[i + k] | 0x20) == word[k]) k++;
        if (k == w) return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Connection lifetime
 * ----------------------------------------------------------------------- */
static void conn_close(Conn *c, bool graceful)
{
    if (c->op) { c->op->conn = NULL; c->op = NULL; }
    if (c->tls) {
        if (graceful && c->pcb) TlsClose(c->tls); else TlsFree(c->tls);
        c->tls = NULL;
    }
    struct tcp_pcb *pcb = c->pcb;
    c->pcb = NULL;
    c->used = false;
    if (!pcb) return;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        if (pcb == g_cb_pcb) g_cb_aborted = true;
    }
}

static void req_fail(Conn *c, const char *why)
{
    NetOp *op = c->op;
    if (!op) return;
    op->conn = NULL;
    c->op = NULL;
    if (op->state == NET_PENDING) net_op_fail(op, why);
}

/* The response is complete: hand it over and keep or close the connection. */
static void req_done(Conn *c)
{
    NetOp *op = c->op;
    if (c->tls) TlsSaveSession(c->tls);
    op->conn = NULL;
    c->op = NULL;
    net_op_done(op);
    c->requests++;
    if (c->close_after) conn_close(c, true);
    else c->idle_since = sched_ticks();
}

/* The server closed (TCP FIN, TLS close_notify, or reset). */
static void peer_closed(Conn *c, bool reset)
{
    NetOp *op = c->op;
    if (op && c->rs == R_UNTIL_CLOSE && !reset) {       /* body ends at close */
        req_done(c);
        conn_close(c, false);
        return;
    }
    if (op && op->reused && op->len == 0 && !op->retried) {
        /* a kept-alive connection the server had already given up on */
        UINT32 ip = c->ip; UINT16 port = c->port; bool https = c->https;
        char host[128];
        memcpy(host, c->host, sizeof(host));
        op->conn = NULL;
        c->op = NULL;
        conn_close(c, false);
        op->retried = true;
        op->reused = false;
        submit(op, ip, port, host, https);
        return;
    }
    if (op)
        req_fail(c, reset ? "Connection reset" :
                    (c->https && !c->tls_ready) ? "Connection closed during the TLS handshake"
                                                : "Connection closed before the response was complete");
    conn_close(c, false);
}

/* -----------------------------------------------------------------------
 * Response parsing
 * ----------------------------------------------------------------------- */
static void cut(NetOp *op, UINT32 at, UINT32 n)       /* remove n bytes at offset */
{
    memmove(op->data + at, op->data + at + n, op->len - at - n);
    op->len -= n;
}

/* Parse what has arrived.  1 = response complete, 0 = need more, -1 = error. */
static int parse(Conn *c)
{
    NetOp *op = c->op;
    char *d = op->data;

    while (c->rs == R_HEADERS) {
        UINT32 i = c->scan, end = 0;
        for (; i + 3 < op->len; i++)
            if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n') { end = i + 4; break; }
        if (!end) {
            c->scan = op->len > 3 ? op->len - 3 : 0;
            if (op->len > 64 * 1024) { req_fail(c, "Response headers are too large"); return -1; }
            return 0;
        }
        if (op->len < 12 || memcmp(d, "HTTP/1.", 7)) { req_fail(c, "Not an HTTP response"); return -1; }
        int status = (d[9] - '0') * 100 + (d[10] - '0') * 10 + (d[11] - '0');
        if (status >= 100 && status < 200 && status != 101) {   /* interim (e.g. 103) */
            cut(op, 0, end);
            c->scan = 0;
            continue;
        }
        c->hdr_len = end;
        bool http10 = d[7] == '0', chunked = false, have_len = false, keep = false, close = false;
        UINT64 clen = 0;
        UINT32 ls = 0;
        while (ls < end - 2) {                               /* header lines */
            UINT32 le = ls;
            while (le + 1 < end && !(d[le] == '\r' && d[le + 1] == '\n')) le++;
            const char *line = d + ls;
            UINT32 n = le - ls;
            const char *colon = memchr(line, ':', n);
            if (colon && ls) {
                UINT32 kn = (UINT32)(colon - line);
                const char *v = colon + 1;
                UINT32 vn = n - kn - 1;
                char key[32];
                if (kn < sizeof(key)) {
                    memcpy(key, line, kn); key[kn] = '\0';
                    if (ieq(key, "content-length")) {
                        have_len = true;
                        clen = 0;
                        for (UINT32 k = 0; k < vn; k++)
                            if (v[k] >= '0' && v[k] <= '9') clen = clen * 10 + (UINT64)(v[k] - '0');
                    } else if (ieq(key, "transfer-encoding")) {
                        chunked = has_token(v, vn, "chunked");
                    } else if (ieq(key, "connection")) {
                        close |= has_token(v, vn, "close");
                        keep  |= has_token(v, vn, "keep-alive");
                    }
                }
            }
            ls = le + 2;
        }
        c->close_after = close || (http10 && !keep);
        if (status == 204 || status == 304) {                /* never a body */
            op->len = end;
            return 1;
        }
        if (chunked) {
            c->rs = R_CHUNKED; c->cs = CH_SIZE; c->pos = end;
        } else if (have_len) {
            c->rs = R_LENGTH; c->body_len = clen;
        } else {
            c->rs = R_UNTIL_CLOSE; c->close_after = true;
        }
    }

    if (c->rs == R_LENGTH) {
        if ((UINT64)(op->len - c->hdr_len) < c->body_len) return 0;
        if ((UINT64)(op->len - c->hdr_len) > c->body_len) {  /* extra bytes: don't reuse */
            op->len = c->hdr_len + (UINT32)c->body_len;
            c->close_after = true;
        }
        return 1;
    }

    if (c->rs == R_CHUNKED) {
        /* Decode in place: [hdr_len, pos) is decoded body, [pos, len) raw. */
        for (;;) {
            d = op->data;
            if (c->cs == CH_SIZE || c->cs == CH_TRAILER) {
                UINT32 le = c->pos;
                while (le + 1 < op->len && !(d[le] == '\r' && d[le + 1] == '\n')) le++;
                if (le + 1 >= op->len) {
                    if (op->len - c->pos > 4096) { req_fail(c, "Malformed chunked response"); return -1; }
                    return 0;
                }
                UINT32 n = le - c->pos;
                if (c->cs == CH_TRAILER) {
                    cut(op, c->pos, n + 2);
                    if (n == 0) {                            /* blank line: end */
                        if (op->len > c->pos) { op->len = c->pos; c->close_after = true; }
                        return 1;
                    }
                    continue;
                }
                UINT64 size = 0;
                int digits = 0;
                for (UINT32 k = c->pos; k < le; k++) {
                    char ch = d[k];
                    int v = ch >= '0' && ch <= '9' ? ch - '0' : (ch | 0x20) >= 'a' && (ch | 0x20) <= 'f'
                          ? (ch | 0x20) - 'a' + 10 : -1;
                    if (v < 0) break;
                    size = size * 16 + (UINT64)v;
                    if (++digits > 12) { req_fail(c, "Malformed chunked response"); return -1; }
                }
                if (!digits) { req_fail(c, "Malformed chunked response"); return -1; }
                cut(op, c->pos, n + 2);
                if (size == 0) c->cs = CH_TRAILER;
                else { c->chunk_left = size; c->cs = CH_DATA; }
            } else if (c->cs == CH_DATA) {
                UINT32 avail = op->len - c->pos;
                if (!avail) return 0;
                UINT32 take = c->chunk_left < avail ? (UINT32)c->chunk_left : avail;
                c->pos += take;
                c->chunk_left -= take;
                if (!c->chunk_left) c->cs = CH_DATA_END;
            } else {                                         /* CH_DATA_END */
                if (op->len - c->pos < 2) return 0;
                if (d[c->pos] != '\r' || d[c->pos + 1] != '\n') {
                    req_fail(c, "Malformed chunked response");
                    return -1;
                }
                cut(op, c->pos, 2);
                c->cs = CH_SIZE;
            }
        }
    }
    return 0;                                                /* R_UNTIL_CLOSE */
}

/* New response bytes for the request in flight. */
static void got_data(Conn *c, const void *data, UINT32 n)
{
    if (!c->op) { conn_close(c, false); return; }           /* unsolicited: drop */
    if (!net_op_append(c->op, data, n)) {                    /* (op failed) */
        c->op->conn = NULL;
        c->op = NULL;
        conn_close(c, false);
        return;
    }
    int r = parse(c);
    if (r > 0) req_done(c);
    else if (r < 0) conn_close(c, false);
}

/* -----------------------------------------------------------------------
 * Sending and the TLS pump
 * ----------------------------------------------------------------------- */
static void send_plain(Conn *c)
{
    NetOp *op = c->op;
    UINT32 len = (UINT32)strlen(op->request);
    UINT32 n = len - c->req_off, room = tcp_sndbuf(c->pcb);
    if (n > room) n = room;
    if (n && tcp_write(c->pcb, op->request + c->req_off, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        c->req_off += n;
        tcp_output(c->pcb);
    }
}

static void tls_pump(Conn *c)
{
    char buf[80];
    if (!c->tls_ready) {
        int r = TlsHandshake(c->tls);
        if (r == TLS_AGAIN) return;
        if (r < 0) {
            req_fail(c, TlsErrorText(c->tls, r, buf, sizeof(buf)));
            conn_close(c, false);
            return;
        }
        c->tls_ready = true;
        TlsDescribe(c->tls, c->tls_info, sizeof(c->tls_info));
        c->resumed = TlsResumed(c->tls);
    }
    NetOp *op = c->op;
    if (op) {
        memcpy(op->tls_info, c->tls_info, sizeof(op->tls_info));
        op->resumed = c->resumed;
        UINT32 len = (UINT32)strlen(op->request);
        while (c->req_off < len) {
            int r = TlsWrite(c->tls, op->request + c->req_off, len - c->req_off);
            if (r == TLS_AGAIN) break;
            if (r < 0) {
                req_fail(c, TlsErrorText(c->tls, r, buf, sizeof(buf)));
                conn_close(c, false);
                return;
            }
            c->req_off += (UINT32)r;
        }
    }
    static char data[4096];                        /* net thread only */
    for (;;) {
        int r = TlsRead(c->tls, data, sizeof(data));
        if (r == TLS_AGAIN) break;
        if (r == TLS_EOF) { peer_closed(c, false); return; }
        if (r < 0) {
            req_fail(c, TlsErrorText(c->tls, r, buf, sizeof(buf)));
            conn_close(c, false);
            return;
        }
        got_data(c, data, (UINT32)r);
        if (!c->used || !c->op) break;             /* closed, or response done */
    }
    if (c->used && !c->op) TlsSaveSession(c->tls); /* e.g. a late TLS 1.3 ticket */
}

static void start_sending(Conn *c)
{
    if (!c->connected || !c->op) return;
    if (c->https) tls_pump(c); else send_plain(c);
}

/* -----------------------------------------------------------------------
 * lwIP callbacks (arg = Conn)
 * ----------------------------------------------------------------------- */
static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    Conn *c = arg;
    cb_begin(pcb);
    if (err != ERR_OK) { req_fail(c, "Could not connect"); conn_close(c, false); return cb_end(); }
    c->connected = true;
    if (c->https) {
        c->tls = TlsClientNew(c->host, c->port, pcb);
        if (!c->tls) { req_fail(c, "Could not start TLS"); conn_close(c, false); return cb_end(); }
    }
    start_sending(c);
    return cb_end();
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    Conn *c = arg;
    cb_begin(pcb);
    if (!p) {                                      /* FIN */
        if (c->tls) {
            TlsInput(c->tls, NULL);
            tls_pump(c);                           /* drain; notices the EOF */
            if (c->used && c->pcb == pcb) peer_closed(c, false);
        } else {
            peer_closed(c, false);
        }
        return cb_end();
    }
    if (err != ERR_OK) { pbuf_free(p); g_cb_pcb = NULL; return err; }
    if (c->tls) {
        TlsInput(c->tls, p);                       /* (tcp_recved as consumed) */
        tls_pump(c);
    } else {
        u16_t n = p->tot_len;
        tcp_recved(pcb, n);
        static char tmp[2048];
        for (u16_t off = 0; off < n && c->used && c->pcb == pcb; ) {
            u16_t k = pbuf_copy_partial(p, tmp, sizeof(tmp), off);
            off = (u16_t)(off + k);
            got_data(c, tmp, k);
        }
        pbuf_free(p);
    }
    return cb_end();
}

static err_t cb_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)len;
    Conn *c = arg;
    cb_begin(pcb);
    if (c->tls) tls_pump(c);
    else if (c->op) send_plain(c);
    return cb_end();
}

static void cb_err(void *arg, err_t err)
{
    Conn *c = arg;
    if (!c) return;
    c->pcb = NULL;                                 /* lwIP already freed it */
    if (c->tls) { TlsFree(c->tls); c->tls = NULL; }
    if (!c->connected && c->op) {
        req_fail(c, "Could not connect");
        conn_close(c, false);
        return;
    }
    peer_closed(c, err != ERR_CLSD);
}

/* -----------------------------------------------------------------------
 * Requests
 * ----------------------------------------------------------------------- */
static void start_request(Conn *c, NetOp *op)
{
    c->op = op;
    op->conn = c;
    op->len = 0;
    c->req_off = 0;
    c->rs = R_HEADERS;
    c->scan = c->hdr_len = c->pos = 0;
    c->close_after = false;
}

static void submit(NetOp *op, UINT32 ip, UINT16 port, const char *host, bool https)
{
    /* 1. an idle kept-alive connection to the same server */
    for (int i = 0; i < HTTP_CONNS; i++) {
        Conn *c = &g_conns[i];
        if (c->used && !c->op && c->connected && c->pcb && (!https || c->tls_ready) &&
            c->ip == ip && c->port == port && c->https == https && ieq(c->host, host)) {
            op->reused = true;
            start_request(c, op);
            start_sending(c);
            return;
        }
    }
    /* 2. a new connection (evicting the longest-idle one if the pool is full) */
    Conn *c = NULL, *oldest = NULL;
    for (int i = 0; i < HTTP_CONNS && !c; i++) {
        if (!g_conns[i].used) c = &g_conns[i];
        else if (!g_conns[i].op && (!oldest || g_conns[i].idle_since < oldest->idle_since))
            oldest = &g_conns[i];
    }
    if (!c && oldest) { conn_close(oldest, true); c = oldest; }
    if (!c) { net_op_fail(op, "Too many connections in use"); return; }

    memset(c, 0, sizeof(*c));
    c->used = true;
    c->ip = ip; c->port = port; c->https = https;
    strncpy(c->host, host, sizeof(c->host) - 1);
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { c->used = false; net_op_fail(op, "Out of connections"); return; }
    c->pcb = pcb;
    start_request(c, op);
    tcp_arg(pcb, c);
    tcp_err(pcb, cb_err);
    tcp_recv(pcb, cb_recv);
    tcp_sent(pcb, cb_sent);
    ip_addr_t dst;
    ip_addr_set_ip4_u32(&dst, ip);
    if (tcp_connect(pcb, &dst, port, cb_connected) != ERR_OK) {
        req_fail(c, "Could not connect");
        conn_close(c, false);
    }
}

NetOp *NetHttpGet(UINT32 ip_be, UINT16 port, const char *host, const char *path, bool https)
{
    net_lock();
    NetOp *op = net_op_alloc(NETOP_HTTP);
    if (!op) { net_unlock(); return NULL; }
    op->ip = ip_be;
    op->deadline = sched_ticks() + HTTP_TIMEOUT;
    if (!net_up()) { net_op_fail(op, "Network is not available"); net_unlock(); return op; }

    char hostport[140];
    if (port == (https ? 443 : 80)) ksnprintf(hostport, sizeof(hostport), "%s", host);
    else                            ksnprintf(hostport, sizeof(hostport), "%s:%u", host, port);
    ksnprintf(op->request, sizeof(op->request),
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: NovaOS/0.9\r\n"
              "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n",
              path && *path ? path : "/", hostport);
    submit(op, ip_be, port, host, https);
    net_unlock();
    return op;
}

void http_release(NetOp *op)
{
    Conn *c = op->conn;
    if (!c) return;
    /* abandoned mid-response: the connection can't be reused */
    c->op = NULL;
    op->conn = NULL;
    conn_close(c, false);
}

void http_poll(void)
{
    UINT64 now = sched_ticks();
    for (int i = 0; i < HTTP_CONNS; i++) {
        Conn *c = &g_conns[i];
        if (!c->used) continue;
        if (c->op && now > c->op->deadline) {
            req_fail(c, "Timed out");
            conn_close(c, false);
        } else if (!c->op && now - c->idle_since > IDLE_TIMEOUT) {
            conn_close(c, true);
        }
    }
}

/* -----------------------------------------------------------------------
 * URL and response helpers
 * ----------------------------------------------------------------------- */
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
    else if (strstr(p, "://"))           return false;       /* other scheme */

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
