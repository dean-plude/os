/*
 * fetch_nova.c — NetSurf's http: and https: fetcher on NovaOS
 *
 * NetSurf's own HTTP fetcher is built on libcurl, which NovaOS doesn't
 * have; this one speaks HTTP/1.1 directly over ws2_32 sockets, with Mbed
 * TLS (tls_glue.c) for https.
 *
 * Each fetch runs on a worker thread that does the blocking parts (name
 * lookup, connect, TLS handshake, request, response parsing, chunked and
 * gzip/deflate decoding) and queues what it finds as events.  NetSurf's
 * core is single-threaded, so the events are delivered from the poll hook
 * on the browser thread, which is also where the header-driven decisions
 * (redirects, 304, 401, only_2xx) are made — curl's fetcher does the same
 * from its callbacks.  A fetch is shared by the two threads through a
 * reference count; aborting just raises a flag the worker checks between
 * socket waits.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include <winsock2.h>
#include <windows.h>

#include <libwapcaplet/libwapcaplet.h>
#include <zlib.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/base64.h"

#include "utils/config.h"
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "utils/useragent.h"
#include "utils/nsoption.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"

#include "tls_glue.h"

#define IO_WAIT_MS       250             /* socket wait slice (abort latency) */
#define IDLE_TIMEOUT_MS  (90 * 1000)     /* no progress for this long: time out */
#define QUEUE_HIGH       (2 * 1024 * 1024)   /* worker pauses above this backlog */
#define MAX_HEADER_LINE  8192

typedef enum { EV_HEADER, EV_HEADERS_DONE, EV_DATA, EV_FINISHED, EV_ERROR, EV_TIMEDOUT } EvType;

typedef struct Event {
    struct Event *next;
    EvType        type;
    size_t        len;
    char          data[];              /* header line, body bytes or error text */
} Event;

typedef struct NovaFetch {
    struct NovaFetch *next;             /* g_fetches (browser thread only) */
    struct fetch     *handle;
    nsurl            *url;
    bool              only_2xx, tls12_only, https;
    char              host[256];
    unsigned short    port;
    char            **extra;            /* caller's headers, copied */
    char             *body;             /* POST body (NULL: GET) */
    size_t            body_len;
    char             *content_type;     /* of the POST body */

    /* request text, built by start() on the browser thread */
    char             *request;
    size_t            request_len;

    /* written by the worker before EV_HEADERS_DONE, read after */
    long              http_code;
    char             *location, *realm;

    /* shared */
    volatile LONG     refs;
    volatile LONG     abort;
    Event            *head, *tail;      /* under g_lock */
    size_t            queued;

    /* browser thread */
    bool              started, finished, dead, in_callback, aborted_in_callback;
} NovaFetch;

static CRITICAL_SECTION g_lock;
static NovaFetch       *g_fetches;
static bool             g_polling;
static int              g_schemes;      /* initialise() calls outstanding */

static char g_tls_error[128];

/* -----------------------------------------------------------------------
 * Event queue
 * ----------------------------------------------------------------------- */
static bool post(NovaFetch *f, EvType type, const void *data, size_t len)
{
    Event *e = malloc(sizeof(Event) + len + 1);
    if (!e) return false;
    e->next = NULL;
    e->type = type;
    e->len = len;
    if (len) memcpy(e->data, data, len);
    e->data[len] = 0;
    EnterCriticalSection(&g_lock);
    if (f->tail) f->tail->next = e; else f->head = e;
    f->tail = e;
    f->queued += len;
    LeaveCriticalSection(&g_lock);
    return true;
}

static void post_error(NovaFetch *f, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    post(f, EV_ERROR, buf, strlen(buf));
}

static Event *take(NovaFetch *f)
{
    EnterCriticalSection(&g_lock);
    Event *e = f->head;
    if (e) {
        f->head = e->next;
        if (!f->head) f->tail = NULL;
        f->queued -= e->len;
    }
    LeaveCriticalSection(&g_lock);
    return e;
}

static size_t backlog(NovaFetch *f)
{
    EnterCriticalSection(&g_lock);
    size_t n = f->queued;
    LeaveCriticalSection(&g_lock);
    return n;
}

static void fetch_release(NovaFetch *f)
{
    if (InterlockedDecrement(&f->refs)) return;
    Event *e;
    while ((e = take(f))) free(e);
    if (f->extra) {
        for (char **h = f->extra; *h; h++) free(*h);
        free(f->extra);
    }
    free(f->body);
    free(f->content_type);
    free(f->request);
    free(f->location);
    free(f->realm);
    free(f);
}

/* -----------------------------------------------------------------------
 * Worker: connection
 * ----------------------------------------------------------------------- */
typedef struct {
    NovaFetch          *f;
    SOCKET              s;
    mbedtls_ssl_context ssl;
    bool                tls;
    ULONGLONG           last_progress;
    bool                timed_out;
} Conn;

/* Wait until @s is readable (or writable): 1 ready, 0 aborted/timed out */
static int wait_sock(Conn *c, bool write)
{
    for (;;) {
        if (c->f->abort) return 0;
        if (GetTickCount64() - c->last_progress > IDLE_TIMEOUT_MS) { c->timed_out = true; return 0; }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(c->s, &set);
        struct timeval tv = { 0, IO_WAIT_MS * 1000 };
        int r = select(0, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
        if (r > 0) return 1;
        if (r < 0) return -1;
    }
}

static int raw_send(Conn *c, const unsigned char *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        if (wait_sock(c, true) <= 0) return -1;
        int w = send(c->s, (const char *)p + done, (int)(n - done > 65536 ? 65536 : n - done), 0);
        if (w <= 0) return -1;
        done += (size_t)w;
        c->last_progress = GetTickCount64();
    }
    return (int)done;
}

static int raw_recv(Conn *c, unsigned char *p, size_t n)
{
    int r = wait_sock(c, false);
    if (r <= 0) return -1;
    int got = recv(c->s, (char *)p, (int)(n > 65536 ? 65536 : n), 0);
    if (got > 0) c->last_progress = GetTickCount64();
    return got < 0 ? -1 : got;          /* 0: peer closed */
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int r = raw_send(ctx, buf, len);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int r = raw_recv(ctx, buf, len);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

static int conn_write(Conn *c, const void *p, size_t n)
{
    if (!c->tls) return raw_send(c, p, n) == (int)n ? 0 : -1;
    const unsigned char *b = p;
    while (n) {
        int r = mbedtls_ssl_write(&c->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Bytes read, 0 at end of stream, -1 on error */
static int conn_read(Conn *c, void *p, size_t n)
{
    if (!c->tls) return raw_recv(c, p, n);
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
            r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r < 0 ? -1 : r;
    }
}

static bool conn_open(Conn *c)
{
    NovaFetch *f = c->f;
    char port[8];
    snprintf(port, sizeof(port), "%u", f->port);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(f->host, port, &hints, &ai) || !ai) {
        /* (worker thread: no NetSurf core calls, so no messages_get here) */
        post_error(f, "The server %s could not be found", f->host);
        return false;
    }
    c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->s == INVALID_SOCKET) { freeaddrinfo(ai); post_error(f, "Out of sockets"); return false; }
    int rc = connect(c->s, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc) {
        int e = WSAGetLastError();
        post_error(f, e == WSAECONNREFUSED ? "Connection refused" :
                      e == WSAETIMEDOUT ? "Connection timed out" : "Could not connect to %s", f->host);
        return false;
    }
    c->last_progress = GetTickCount64();
    if (!f->https) return true;

    mbedtls_ssl_init(&c->ssl);
    c->tls = true;
    if (mbedtls_ssl_setup(&c->ssl, nova_tls_config(f->tls12_only)) ||
        mbedtls_ssl_set_hostname(&c->ssl, f->host)) {
        post_error(f, "TLS setup failed");
        return false;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (f->abort) return false;
        if (c->timed_out) { post(f, EV_TIMEDOUT, NULL, 0); return false; }
        uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags) {
            char why[160];
            int n = mbedtls_x509_crt_verify_info(why, sizeof(why), "", flags);
            while (n > 0 && (why[n - 1] == '\n' || why[n - 1] == ' ')) why[--n] = 0;
            post_error(f, "The certificate of %s is not trusted (%s)", f->host, n > 0 ? why : "verification failed");
        } else {
            post_error(f, "Secure connection to %s failed (TLS error -0x%04X)", f->host, (unsigned)-r);
        }
        return false;
    }
    return true;
}

static void conn_close(Conn *c)
{
    if (c->tls) {
        mbedtls_ssl_free(&c->ssl);
        c->tls = false;
    }
    if (c->s != INVALID_SOCKET) {
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
}

/* -----------------------------------------------------------------------
 * Worker: response
 * ----------------------------------------------------------------------- */
typedef struct {
    Conn          *c;
    unsigned char  buf[16384];
    size_t         pos, len;
    bool           eof;
} Reader;

static int rd_fill(Reader *r)
{
    if (r->pos < r->len) return 1;
    if (r->eof) return 0;
    int n = conn_read(r->c, r->buf, sizeof(r->buf));
    if (n < 0) return -1;
    if (n == 0) { r->eof = true; return 0; }
    r->pos = 0;
    r->len = (size_t)n;
    return 1;
}

/* One line without its CRLF: length, or -1 on error/EOF */
static int rd_line(Reader *r, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int k = rd_fill(r);
        if (k <= 0) return n && k == 0 ? (int)n : -1;
        char ch = (char)r->buf[r->pos++];
        if (ch == '\n') break;
        if (n + 1 < cap) line[n++] = ch;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    return (int)n;
}

typedef struct {
    NovaFetch *f;
    int        encoding;           /* 0 identity, 1 gzip/deflate via zlib */
    z_stream   z;
    bool       z_ready, z_done;
} Sink;

static bool sink_throttle(NovaFetch *f)
{
    while (backlog(f) > QUEUE_HIGH) {
        if (f->abort) return false;
        Sleep(10);
    }
    return !f->abort;
}

static bool sink_put(Sink *s, const unsigned char *p, size_t n)
{
    if (!sink_throttle(s->f)) return false;
    if (!s->encoding) return post(s->f, EV_DATA, p, n);
    if (s->z_done) return true;
    if (!s->z_ready) {
        memset(&s->z, 0, sizeof(s->z));
        if (inflateInit2(&s->z, 15 + 32) != Z_OK) return false;     /* gzip or zlib header */
        s->z_ready = true;
    }
    unsigned char out[16384];
    s->z.next_in = (unsigned char *)p;
    s->z.avail_in = (uInt)n;
    while (s->z.avail_in) {
        s->z.next_out = out;
        s->z.avail_out = sizeof(out);
        int r = inflate(&s->z, Z_NO_FLUSH);
        if (r == Z_DATA_ERROR && s->z.total_out == 0) {
            /* "deflate" sent as a raw stream: retry without a header */
            inflateEnd(&s->z);
            memset(&s->z, 0, sizeof(s->z));
            if (inflateInit2(&s->z, -15) != Z_OK) return false;
            s->z.next_in = (unsigned char *)p;
            s->z.avail_in = (uInt)n;
            continue;
        }
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) return false;
        size_t got = sizeof(out) - s->z.avail_out;
        if (got && !post(s->f, EV_DATA, out, got)) return false;
        if (r == Z_STREAM_END) { s->z_done = true; break; }
        if (r == Z_BUF_ERROR && !got) break;
    }
    return true;
}

static void sink_end(Sink *s)
{
    if (s->z_ready) inflateEnd(&s->z);
}

/* Body with a known length (-1: until the connection closes) */
static int read_body(Reader *r, Sink *s, long long length)
{
    while (length != 0) {
        int k = rd_fill(r);
        if (k < 0) return -1;
        if (k == 0) return length < 0 ? 0 : -1;
        size_t n = r->len - r->pos;
        if (length > 0 && (long long)n > length) n = (size_t)length;
        if (!sink_put(s, r->buf + r->pos, n)) return -1;
        r->pos += n;
        if (length > 0) length -= (long long)n;
    }
    return 0;
}

static int read_chunked(Reader *r, Sink *s)
{
    char line[256];
    for (;;) {
        if (rd_line(r, line, sizeof(line)) < 0) return -1;
        long long size = strtoll(line, NULL, 16);
        if (size < 0) return -1;
        if (size == 0) {
            while (rd_line(r, line, sizeof(line)) > 0) { }      /* trailers */
            return 0;
        }
        if (read_body(r, s, size) < 0) return -1;
        if (rd_line(r, line, sizeof(line)) < 0) return -1;     /* CRLF after the data */
    }
}

static const char *header_value(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return NULL;
    line += n + 1;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

static char *dup_trimmed(const char *v)
{
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
    char *d = malloc(n + 1);
    if (d) { memcpy(d, v, n); d[n] = 0; }
    return d;
}

static void run_fetch(NovaFetch *f)
{
    Conn c;
    memset(&c, 0, sizeof(c));
    c.f = f;
    c.s = INVALID_SOCKET;
    if (!conn_open(&c)) { conn_close(&c); return; }
    if (conn_write(&c, f->request, f->request_len) ||
        (f->body_len && conn_write(&c, f->body, f->body_len))) {
        if (c.timed_out) post(f, EV_TIMEDOUT, NULL, 0);
        else if (!f->abort) post_error(f, "Could not send the request to %s", f->host);
        conn_close(&c);
        return;
    }

    Reader *r = calloc(1, sizeof(Reader));
    char *line = malloc(MAX_HEADER_LINE);
    if (!r || !line) { free(r); free(line); post_error(f, "Out of memory"); conn_close(&c); return; }
    r->c = &c;

    long long length = -1;
    bool chunked = false;
    int encoding = 0;
    long code;
    /* status line and headers; 1xx interim responses are skipped */
    for (;;) {
        int n = rd_line(r, line, MAX_HEADER_LINE);
        if (n < 0) goto io_error;
        if (n == 0) continue;                           /* stray CRLF */
        if (strncmp(line, "HTTP/", 5)) { post_error(f, "Not an HTTP response"); goto done; }
        const char *sp = strchr(line, ' ');
        code = sp ? strtol(sp + 1, NULL, 10) : 0;
        if (code < 100 || code > 999) { post_error(f, "Bad HTTP status line"); goto done; }
        if (code >= 200 || code == 101) break;
        while ((n = rd_line(r, line, MAX_HEADER_LINE)) > 0) { }
        if (n < 0) goto io_error;
    }
    post(f, EV_HEADER, line, strlen(line));
    f->http_code = code;
    for (;;) {
        int n = rd_line(r, line, MAX_HEADER_LINE);
        if (n < 0) goto io_error;
        if (n == 0) break;
        const char *v;
        if ((v = header_value(line, "Content-Length"))) length = strtoll(v, NULL, 10);
        else if ((v = header_value(line, "Transfer-Encoding"))) chunked = strstr(v, "chunked") || strstr(v, "Chunked");
        else if ((v = header_value(line, "Content-Encoding")))
            encoding = !strncasecmp(v, "gzip", 4) || !strncasecmp(v, "x-gzip", 6) || !strncasecmp(v, "deflate", 7);
        else if ((v = header_value(line, "Location"))) { free(f->location); f->location = dup_trimmed(v); }
        else if ((v = header_value(line, "WWW-Authenticate"))) {
            const char *q = strstr(v, "realm=\"");
            if (q) {
                q += 7;
                const char *e = strchr(q, '"');
                if (e) {
                    free(f->realm);
                    f->realm = malloc((size_t)(e - q) + 1);
                    if (f->realm) { memcpy(f->realm, q, (size_t)(e - q)); f->realm[e - q] = 0; }
                }
            }
        }
        /* NetSurf gets every header, like curl's header callback */
        post(f, EV_HEADER, line, (size_t)n);
    }
    post(f, EV_HEADERS_DONE, NULL, 0);

    /* body */
    Sink s = { f, encoding };
    int rc;
    if (code == 204 || code == 304 || (code >= 100 && code < 200)) rc = 0;
    else if (chunked) rc = read_chunked(r, &s);
    else rc = read_body(r, &s, length);
    sink_end(&s);
    if (rc == 0) { post(f, EV_FINISHED, NULL, 0); goto done; }

io_error:
    if (f->abort) goto done;
    if (c.timed_out) post(f, EV_TIMEDOUT, NULL, 0);
    else post_error(f, "The connection to %s was interrupted", f->host);
done:
    free(line);
    free(r);
    conn_close(&c);
}

static DWORD WINAPI worker(LPVOID arg)
{
    NovaFetch *f = arg;
    run_fetch(f);
    fetch_release(f);
    return 0;
}

/* -----------------------------------------------------------------------
 * Fetcher operations (browser thread)
 * ----------------------------------------------------------------------- */
static bool nova_initialise(lwc_string *scheme)
{
    (void)scheme;
    if (g_schemes++ == 0) {
        InitializeCriticalSection(&g_lock);
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        char roots[MAX_PATH];
        snprintf(roots, sizeof(roots), "%s/ca-bundle.der", NETSURF_FB_RESPATH);
        if (nova_tls_init(roots))
            snprintf(g_tls_error, sizeof(g_tls_error), "TLS initialisation failed");
        NSLOG(netsurf, INFO, "fetch_nova: %d trusted roots", nova_tls_root_count());
    }
    return true;
}

static void nova_finalise(lwc_string *scheme)
{
    (void)scheme;
    if (--g_schemes == 0) WSACleanup();
}

static bool nova_acceptable(const nsurl *url)
{
    (void)url;
    return true;
}

static char *copy_str(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* multipart/form-data body; returns false on failure */
static bool build_multipart(NovaFetch *f, const struct fetch_multipart_data *m)
{
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----NetSurfNovaOS%08lx%08lx",
             (unsigned long)GetTickCount(), (unsigned long)(uintptr_t)f);
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    if (!b) return false;
#define APPEND(p, n) do { size_t n_ = (n); if (len + n_ + 1 > cap) { \
        while (len + n_ + 1 > cap) cap *= 2; char *nb = realloc(b, cap); \
        if (!nb) { free(b); return false; } b = nb; } memcpy(b + len, (p), n_); len += n_; } while (0)
    char hdr[1024];
    for (; m; m = m->next) {
        if (m->file) {
            const char *fname = m->value ? m->value : "";
            const char *base = strrchr(fname, '\\');
            snprintf(hdr, sizeof(hdr), "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                     "Content-Type: application/octet-stream\r\n\r\n", boundary, m->name, base ? base + 1 : fname);
            APPEND(hdr, strlen(hdr));
            FILE *fp = m->rawfile ? fopen(m->rawfile, "rb") : NULL;
            if (fp) {
                char chunk[4096];
                size_t n;
                while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) APPEND(chunk, n);
                fclose(fp);
            }
        } else {
            snprintf(hdr, sizeof(hdr), "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n",
                     boundary, m->name);
            APPEND(hdr, strlen(hdr));
            if (m->value) APPEND(m->value, strlen(m->value));
        }
        APPEND("\r\n", 2);
    }
    snprintf(hdr, sizeof(hdr), "--%s--\r\n", boundary);
    APPEND(hdr, strlen(hdr));
#undef APPEND
    f->body = b;
    f->body_len = len;
    snprintf(hdr, sizeof(hdr), "multipart/form-data; boundary=%s", boundary);
    f->content_type = strdup(hdr);
    return f->content_type != NULL;
}

static void *nova_setup(struct fetch *parent, nsurl *url, bool only_2xx, bool downgrade_tls,
                        const char *post_urlenc, const struct fetch_multipart_data *post_multipart,
                        const char **headers)
{
    NovaFetch *f = calloc(1, sizeof(NovaFetch));
    if (!f) return NULL;
    f->refs = 1;
    f->handle = parent;
    f->url = nsurl_ref(url);
    f->only_2xx = only_2xx;
    f->tls12_only = downgrade_tls;

    lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
    lwc_string *host = nsurl_get_component(url, NSURL_HOST);
    lwc_string *port = nsurl_get_component(url, NSURL_PORT);
    f->https = scheme && lwc_string_length(scheme) == 5 && !strncasecmp(lwc_string_data(scheme), "https", 5);
    if (host) {
        size_t n = lwc_string_length(host);
        if (n >= sizeof(f->host)) n = sizeof(f->host) - 1;
        memcpy(f->host, lwc_string_data(host), n);
        f->host[n] = 0;
        /* IPv6 literals keep their brackets in NetSurf; NovaOS is IPv4 only */
    }
    f->port = f->https ? 443 : 80;
    if (port) f->port = (unsigned short)atoi(lwc_string_data(port));
    if (scheme) lwc_string_unref(scheme);
    if (host) lwc_string_unref(host);
    if (port) lwc_string_unref(port);
    if (!f->host[0]) goto failed;

    int n = 0;
    while (headers && headers[n]) n++;
    f->extra = calloc((size_t)n + 1, sizeof(char *));
    if (!f->extra) goto failed;
    for (int i = 0; i < n; i++)
        if (!(f->extra[i] = copy_str(headers[i]))) goto failed;

    if (post_urlenc) {
        f->body = copy_str(post_urlenc);
        f->body_len = strlen(post_urlenc);
        f->content_type = copy_str("application/x-www-form-urlencoded");
        if (!f->body || !f->content_type) goto failed;
    } else if (post_multipart) {
        if (!build_multipart(f, post_multipart)) goto failed;
    }
    return f;

failed:
    nsurl_unref(f->url);
    f->url = NULL;
    fetch_release(f);
    return NULL;
}

typedef struct { char *s; size_t len, cap; bool oom; } Buf;

static void buf_add(Buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (b->len + n + 1 > cap) cap *= 2;
        char *ns = realloc(b->s, cap);
        if (!ns) { b->oom = true; return; }
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

static void buf_addf(Buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_add(b, tmp);
}

static bool build_request(NovaFetch *f)
{
    char *target = NULL;
    size_t target_len = 0;
    if (nsurl_get(f->url, NSURL_PATH | NSURL_QUERY, &target, &target_len) != NSERROR_OK) return false;
    Buf b = { 0 };
    buf_addf(&b, "%s ", f->body ? "POST" : "GET");
    buf_add(&b, target_len ? target : "/");
    free(target);
    buf_add(&b, " HTTP/1.1\r\n");
    if (f->port == (f->https ? 443 : 80)) buf_addf(&b, "Host: %s\r\n", f->host);
    else buf_addf(&b, "Host: %s:%u\r\n", f->host, f->port);
    buf_addf(&b, "User-Agent: %s\r\n", user_agent_string());
    buf_add(&b, "Accept: */*\r\nAccept-Encoding: gzip, deflate\r\nConnection: close\r\n");
    if (nsoption_charp(accept_language) && nsoption_charp(accept_language)[0])
        buf_addf(&b, "Accept-Language: %s, *;q=0.1\r\n", nsoption_charp(accept_language));
    if (nsoption_charp(accept_charset) && nsoption_charp(accept_charset)[0])
        buf_addf(&b, "Accept-Charset: %s, *;q=0.1\r\n", nsoption_charp(accept_charset));
    if (nsoption_bool(do_not_track)) buf_add(&b, "DNT: 1\r\n");
    char *cookie = urldb_get_cookie(f->url, true);
    if (cookie) {
        buf_add(&b, "Cookie: ");
        buf_add(&b, cookie);
        buf_add(&b, "\r\n");
        free(cookie);
    }
    const char *auth = urldb_get_auth_details(f->url, f->realm);
    if (auth && auth[0]) {
        unsigned char enc[512];
        size_t olen = 0;
        if (!mbedtls_base64_encode(enc, sizeof(enc) - 1, &olen, (const unsigned char *)auth, strlen(auth))) {
            enc[olen] = 0;
            buf_addf(&b, "Authorization: Basic %s\r\n", enc);
        }
    }
    for (char **h = f->extra; h && *h; h++) {
        buf_add(&b, *h);
        buf_add(&b, "\r\n");
    }
    if (f->body) {
        buf_addf(&b, "Content-Type: %s\r\n", f->content_type);
        buf_addf(&b, "Content-Length: %lu\r\n", (unsigned long)f->body_len);
    }
    buf_add(&b, "\r\n");
    if (b.oom) { free(b.s); return false; }
    f->request = b.s;
    f->request_len = b.len;
    return true;
}

static bool nova_start(void *vf)
{
    NovaFetch *f = vf;
    if (g_tls_error[0] && f->https) {
        post(f, EV_ERROR, g_tls_error, strlen(g_tls_error));
    } else if (!build_request(f)) {
        return false;
    } else {
        InterlockedIncrement(&f->refs);         /* the worker's */
        HANDLE t = CreateThread(NULL, 256 * 1024, worker, f, 0, NULL);
        if (!t) {
            InterlockedDecrement(&f->refs);
            return false;
        }
        CloseHandle(t);
    }
    f->started = true;
    f->next = g_fetches;
    g_fetches = f;
    return true;
}

static void unlink_fetch(NovaFetch *f)
{
    for (NovaFetch **p = &g_fetches; *p; p = &(*p)->next)
        if (*p == f) { *p = f->next; return; }
}

static void nova_abort(void *vf)
{
    NovaFetch *f = vf;
    InterlockedExchange(&f->abort, 1);
    if (f->in_callback) {                       /* poll() finishes it off */
        f->aborted_in_callback = true;
        return;
    }
    fetch_remove_from_queues(f->handle);
    fetch_free(f->handle);
}

static void nova_free(void *vf)
{
    NovaFetch *f = vf;
    InterlockedExchange(&f->abort, 1);
    if (f->url) nsurl_unref(f->url);
    f->url = NULL;
    if (g_polling) {                            /* poll() unlinks it later */
        f->dead = true;
        return;
    }
    if (f->started) unlink_fetch(f);
    fetch_release(f);
}

static void deliver(NovaFetch *f, fetch_msg *msg)
{
    f->in_callback = true;
    fetch_send_callback(msg, f->handle);
    f->in_callback = false;
}

/* The status and headers are in: decide as curl's fetcher does.  True if
 * the fetch is over. */
static bool process_headers(NovaFetch *f)
{
    fetch_msg msg;
    long code = f->http_code;
    fetch_set_http_code(f->handle, code);
    if (code == 304 && !f->body) {
        msg.type = FETCH_NOTMODIFIED;
        deliver(f, &msg);
        return true;
    }
    if (code >= 300 && code < 400 && f->location) {
        msg.type = FETCH_REDIRECT;
        msg.data.redirect = f->location;
        deliver(f, &msg);
        return true;
    }
    if (code == 401) {
        msg.type = FETCH_AUTH;
        msg.data.auth.realm = f->realm ? f->realm : "";
        deliver(f, &msg);
        return true;
    }
    if (f->only_2xx && (code < 200 || code > 299)) {
        msg.type = FETCH_ERROR;
        msg.data.error = messages_get("Not2xx");
        deliver(f, &msg);
        return true;
    }
    return false;
}

static void finish(NovaFetch *f)
{
    f->finished = true;
    InterlockedExchange(&f->abort, 1);          /* stop the worker if it is still going */
    fetch_remove_from_queues(f->handle);
    fetch_free(f->handle);
}

static void poll_one(NovaFetch *f)
{
    Event *e;
    while (!f->finished && !f->dead && (e = take(f))) {
        fetch_msg msg;
        bool over = false;
        switch (e->type) {
        case EV_HEADER:
            if (!strncasecmp(e->data, "Set-Cookie:", 11)) {
                const char *v = e->data + 11;
                while (*v == ' ' || *v == '\t') v++;
                fetch_set_cookie(f->handle, v);
            }
            msg.type = FETCH_HEADER;
            msg.data.header_or_data.buf = (const uint8_t *)e->data;
            msg.data.header_or_data.len = e->len;
            deliver(f, &msg);
            break;
        case EV_HEADERS_DONE:
            over = process_headers(f);
            break;
        case EV_DATA:
            msg.type = FETCH_DATA;
            msg.data.header_or_data.buf = (const uint8_t *)e->data;
            msg.data.header_or_data.len = e->len;
            deliver(f, &msg);
            break;
        case EV_FINISHED:
            msg.type = FETCH_FINISHED;
            deliver(f, &msg);
            over = true;
            break;
        case EV_TIMEDOUT:
            msg.type = FETCH_TIMEDOUT;
            msg.data.error = messages_get("Timeout") ? messages_get("Timeout") : "Timed out";
            deliver(f, &msg);
            over = true;
            break;
        case EV_ERROR:
            msg.type = FETCH_ERROR;
            msg.data.error = e->data;
            deliver(f, &msg);
            over = true;
            break;
        }
        free(e);
        if (f->aborted_in_callback) {
            f->aborted_in_callback = false;
            over = true;
        }
        if (over && !f->dead) finish(f);
    }
}

static void nova_poll(lwc_string *scheme)
{
    (void)scheme;
    if (g_polling) return;                      /* both schemes share the list */
    g_polling = true;
    for (NovaFetch *f = g_fetches; f; f = f->next)
        if (!f->dead && !f->finished) poll_one(f);
    g_polling = false;
    /* sweep the fetches NetSurf freed during the callbacks */
    for (NovaFetch **p = &g_fetches; *p;) {
        NovaFetch *f = *p;
        if (f->dead) {
            *p = f->next;
            fetch_release(f);
        } else {
            p = &f->next;
        }
    }
}

nserror fetch_nova_register(void)
{
    static const struct fetcher_operation_table ops = {
        .initialise = nova_initialise,
        .acceptable = nova_acceptable,
        .setup = nova_setup,
        .start = nova_start,
        .abort = nova_abort,
        .free = nova_free,
        .poll = nova_poll,
        .fdset = NULL,
        .finalise = nova_finalise,
    };
    const char *schemes[] = { "http", "https" };
    for (int i = 0; i < 2; i++) {
        lwc_string *s;
        if (lwc_intern_string(schemes[i], strlen(schemes[i]), &s) != lwc_error_ok) return NSERROR_NOMEM;
        nserror e = fetcher_add(s, &ops);
        if (e != NSERROR_OK) return e;
    }
    return NSERROR_OK;
}
