/*
 * winhttp.dll — Windows' HTTP client, on Winsock and the Schannel TLS
 * package (secur32), with HTTP/2 from nghttp2 (third_party/nghttp2).
 *
 * A session holds the defaults, a connection names the server, and each
 * request opens its own TCP connection (TLS for WINHTTP_FLAG_SECURE).
 * When the session or request enabled WINHTTP_PROTOCOL_FLAG_HTTP2, TLS
 * offers "h2" by ALPN and the request goes over HTTP/2 if the server
 * picks it; otherwise it is HTTP/1.1 (Content-Length, chunked, or until
 * the server closes).  Redirects are followed, Basic credentials are sent,
 * and asynchronous sessions (WINHTTP_FLAG_ASYNC) run each call on a worker
 * thread that reports through the status callback, as Windows does.
 *
 * The request body is collected (WinHttpSendRequest's optional data and
 * WinHttpWriteData) and sent with the headers when the response is asked
 * for.  There is no proxy and no connection reuse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define WINHTTPAPI __declspec(dllexport)
#include <winhttp.h>
#include <nghttp2/nghttp2.h>

#define ERROR_WINHTTP_INVALID_QUERY_REQUEST_   12154
#define ERROR_WINHTTP_HEADER_ALREADY_EXISTS_   12155

/* -----------------------------------------------------------------------
 * SSPI (secur32's Schannel)
 * ----------------------------------------------------------------------- */
typedef LONG SECURITY_STATUS;
typedef struct { ULONG_PTR dwLower, dwUpper; } SecHandle;
typedef struct { ULONG cbBuffer, BufferType; void *pvBuffer; } SecBuffer;
typedef struct { ULONG ulVersion, cBuffers; SecBuffer *pBuffers; } SecBufferDesc;
typedef struct { ULONG cbHeader, cbTrailer, cbMaximumMessage, cBuffers, cbBlockSize; } StreamSizes;
typedef struct { int status, ext; BYTE size; BYTE id[255]; } AppProto;
typedef struct { DWORD dwVersion, cCreds; void *paCred, *hRootStore; DWORD cMappers; void *aphMappers;
                 DWORD cSupportedAlgs; void *palgSupportedAlgs; DWORD grbitEnabledProtocols,
                 dwMinimumCipherStrength, dwMaximumCipherStrength, dwSessionLifespan, dwFlags,
                 dwCredFormat; } SchannelCred;

#define SEC_E_OK                   0
#define SEC_I_CONTINUE_NEEDED      0x00090312
#define SEC_I_CONTEXT_EXPIRED      0x00090317
#define SEC_I_RENEGOTIATE          0x00090321
#define SEC_E_INCOMPLETE_MESSAGE   ((SECURITY_STATUS)0x80090318)
#define SEC_E_WRONG_PRINCIPAL      ((SECURITY_STATUS)0x80090322)
#define SEC_E_UNTRUSTED_ROOT       ((SECURITY_STATUS)0x80090325)
#define SEC_E_CERT_EXPIRED         ((SECURITY_STATUS)0x80090328)
#define SECBUFFER_VERSION          0
#define SECBUFFER_EMPTY            0
#define SECBUFFER_DATA             1
#define SECBUFFER_TOKEN            2
#define SECBUFFER_EXTRA            5
#define SECBUFFER_STREAM_TRAILER   6
#define SECBUFFER_STREAM_HEADER    7
#define SECBUFFER_APPLICATION_PROTOCOLS 18
#define SECPKG_CRED_OUTBOUND       2
#define SECPKG_ATTR_STREAM_SIZES   4
#define SECPKG_ATTR_APPLICATION_PROTOCOL 35
#define ISC_REQ_REPLAY_DETECT      0x00000004
#define ISC_REQ_SEQUENCE_DETECT    0x00000008
#define ISC_REQ_CONFIDENTIALITY    0x00000010
#define ISC_REQ_ALLOCATE_MEMORY    0x00000100
#define ISC_REQ_STREAM             0x00008000
#define ISC_REQ_MANUAL_CRED_VALIDATION 0x00080000
#define SCH_CRED_NO_SERVERNAME_CHECK    0x00000004
#define SCH_CRED_MANUAL_CRED_VALIDATION 0x00000008
#define SCHANNEL_SHUTDOWN          1

__declspec(dllimport) SECURITY_STATUS WINAPI AcquireCredentialsHandleW(WCHAR *, WCHAR *, ULONG, void *, void *,
                                                                       void *, void *, SecHandle *, LARGE_INTEGER *);
__declspec(dllimport) SECURITY_STATUS WINAPI InitializeSecurityContextW(SecHandle *, SecHandle *, WCHAR *, ULONG, ULONG,
                                                                        ULONG, SecBufferDesc *, ULONG, SecHandle *,
                                                                        SecBufferDesc *, ULONG *, LARGE_INTEGER *);
__declspec(dllimport) SECURITY_STATUS WINAPI QueryContextAttributesW(SecHandle *, ULONG, void *);
__declspec(dllimport) SECURITY_STATUS WINAPI EncryptMessage(SecHandle *, ULONG, SecBufferDesc *, ULONG);
__declspec(dllimport) SECURITY_STATUS WINAPI DecryptMessage(SecHandle *, SecBufferDesc *, ULONG, ULONG *);
__declspec(dllimport) SECURITY_STATUS WINAPI DeleteSecurityContext(SecHandle *);
__declspec(dllimport) SECURITY_STATUS WINAPI FreeCredentialsHandle(SecHandle *);
__declspec(dllimport) SECURITY_STATUS WINAPI FreeContextBuffer(void *);
__declspec(dllimport) SECURITY_STATUS WINAPI ApplyControlToken(SecHandle *, SecBufferDesc *);

/* -----------------------------------------------------------------------
 * Small helpers
 * ----------------------------------------------------------------------- */
static void *xcalloc(size_t n) { return calloc(1, n); }

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
static char *dup_s(const char *s) { return s ? dup_n(s, strlen(s)) : 0; }

/* UTF-16 → UTF-8 (@n characters, or to the NUL when n is -1) */
static char *utf8_n(const WCHAR *w, int n)
{
    if (!w) return 0;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, 0, 0, 0, 0);
    char *s = malloc(len + 1);
    if (!s) return 0;
    WideCharToMultiByte(CP_UTF8, 0, w, n, s, len, 0, 0);
    s[len] = 0;
    return s;
}
static char *utf8(const WCHAR *w) { return utf8_n(w, -1); }

static WCHAR *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    WCHAR *w = malloc(n * sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (lower((BYTE)a[i]) != lower((BYTE)b[i])) return 0;
    return 1;
}
static int ieq(const char *a, const char *b) { size_t n = strlen(a); return n == strlen(b) && ieq_n(a, b, n); }

/* A growing byte buffer; data lives in [off, len) */
typedef struct { BYTE *p; size_t off, len, cap; } Buf;

static int buf_add(Buf *b, const void *d, size_t n)
{
    if (b->off && b->off == b->len) b->off = b->len = 0;
    if (b->len + n > b->cap) {
        if (b->off) { memmove(b->p, b->p + b->off, b->len - b->off); b->len -= b->off; b->off = 0; }
        if (b->len + n > b->cap) {
            size_t cap = (b->len + n) * 2 + 4096;
            BYTE *p = realloc(b->p, cap);
            if (!p) return 0;
            b->p = p;
            b->cap = cap;
        }
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 1;
}
static int buf_str(Buf *b, const char *s) { return buf_add(b, s, strlen(s)); }
static size_t buf_avail(Buf *b) { return b->len - b->off; }
static void buf_free(Buf *b) { free(b->p); memset(b, 0, sizeof(*b)); }

/* Headers: name/value pairs */
typedef struct { char *name, *value; } Header;
typedef struct { Header *h; int n, cap; } Headers;

static int hdr_add(Headers *hs, const char *name, size_t nl, const char *value, size_t vl)
{
    if (hs->n == hs->cap) {
        int cap = hs->cap ? hs->cap * 2 : 16;
        Header *h = realloc(hs->h, cap * sizeof(Header));
        if (!h) return 0;
        hs->h = h;
        hs->cap = cap;
    }
    hs->h[hs->n].name = dup_n(name, nl);
    hs->h[hs->n].value = dup_n(value, vl);
    hs->n++;
    return 1;
}
static void hdr_remove(Headers *hs, const char *name)
{
    for (int i = 0; i < hs->n;)
        if (ieq(hs->h[i].name, name)) {
            free(hs->h[i].name);
            free(hs->h[i].value);
            memmove(&hs->h[i], &hs->h[i + 1], (hs->n - i - 1) * sizeof(Header));
            hs->n--;
        } else i++;
}
/* the @index'th header called @name (or 0) */
static Header *hdr_find(Headers *hs, const char *name, DWORD index)
{
    for (int i = 0; i < hs->n; i++)
        if (ieq(hs->h[i].name, name) && !index--) return &hs->h[i];
    return 0;
}
static void hdr_free(Headers *hs)
{
    for (int i = 0; i < hs->n; i++) { free(hs->h[i].name); free(hs->h[i].value); }
    free(hs->h);
    memset(hs, 0, sizeof(*hs));
}

/* -----------------------------------------------------------------------
 * Handles
 * ----------------------------------------------------------------------- */
#define HMAGIC 0x50545448u                             /* "HTTP" */

typedef struct Hdr {
    DWORD magic, type;                                 /* WINHTTP_HANDLE_TYPE_* */
    LONG refs;                                         /* the handle, children, running calls */
    struct Hdr *parent;
    WINHTTP_STATUS_CALLBACK cb;
    DWORD cb_flags;
    DWORD_PTR ctx;
    /* options a request takes from its session and connection */
    DWORD async, protocols, security, redirect_policy, max_redirects, disable;
    int timeouts[4];                                   /* resolve, connect, send, receive (ms) */
} Hdr;

typedef struct {
    Hdr h;
    char *agent;
} Session;

typedef struct {
    Hdr h;
    char *host;                                        /* without brackets */
    WORD port;
    struct Conn *idle;
} Connect;

enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_CLOSE };

typedef struct Conn {
    SOCKET s;
    int tls;
    DWORD security;
    SecHandle cred, ctx;
    int have_cred, have_ctx;
    StreamSizes sizes;
    Buf raw;                                           /* TLS records not decrypted yet */
    Buf plain;                                         /* decrypted, not handed out yet */
    int eof;
    int receive_ms;
    ULONGLONG idle_since;
    char alpn[32];
} Conn;

typedef struct {
    Hdr h;
    char *agent;
    char *verb, *path, *version;
    char *host;                                        /* where the request goes (redirects change it) */
    WORD port;
    int secure;
    Headers req;
    char *auth;                                        /* "Basic ..." from WinHttpSetCredentials */
    Buf body;
    DWORD body_total;
    int sent, received, redirects;
    int websocket_upgrade;
    /* the response */
    Conn *conn;
    int http2;
    int status;
    char *status_text, *resp_version;
    Headers resp;
    Buf in;                                            /* HTTP/1.1 bytes read, not parsed yet */
    Buf out;                                           /* body bytes ready for WinHttpReadData */
    int framing, chunk_state, eof;
    unsigned long long left;                           /* in a Content-Length body or a chunk */
    /* HTTP/2 */
    nghttp2_session *h2;
    int32_t stream;
    int h2_headers, h2_closed;
    uint32_t h2_error;
    size_t body_sent;
    DWORD err;                                         /* a callback's error */
} Request;

/* A proxy resolver (WinHttpCreateProxyResolver): one lookup at a time */
#define WINHTTP_HANDLE_TYPE_PROXY_RESOLVER_ 4
typedef struct {
    Hdr h;
    LONG busy;                                         /* a lookup is running */
    DWORD_PTR lookup_ctx;
} Resolver;
#define WINHTTP_HANDLE_TYPE_WEBSOCKET_ 5
typedef struct {
    Hdr h;
    Conn *conn;
    Buf in;
    unsigned long long frame_left;
    int frame_opcode, frame_fin, message_opcode;
    int close_received, close_sent;
    USHORT close_status;
    BYTE close_reason[123];
    DWORD close_reason_len;
} WebSocket;

__declspec(dllimport) BOOLEAN WINAPI SystemFunction036(PVOID buf, ULONG len);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    return TRUE;
}
static void init(void) { InitOnceExecuteOnce(&g_once, init_once, 0, 0); }

static Hdr *get(HINTERNET h, DWORD type)
{
    Hdr *x = h;
    if (!x || x->magic != HMAGIC) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (type && x->type != type) { SetLastError(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE); return 0; }
    return x;
}

static void notify(Hdr *h, DWORD status, void *info, DWORD len)
{
    WINHTTP_STATUS_CALLBACK cb = h->cb;
    if (cb && cb != WINHTTP_INVALID_STATUS_CALLBACK && (h->cb_flags & status))
        cb((HINTERNET)h, h->ctx, status, info, len);
}

static void inherit(Hdr *child, Hdr *parent, DWORD type)
{
    *child = *parent;
    child->magic = HMAGIC;
    child->type = type;
    child->refs = 1;
    child->parent = parent;
    InterlockedIncrement(&parent->refs);
}

static void conn_close(Conn *c);
static void free_hdr(Hdr *h);

static void release(Hdr *h)
{
    while (h && InterlockedDecrement(&h->refs) == 0) {
        Hdr *parent = h->parent;
        notify(h, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, &h, sizeof(h));
        free_hdr(h);
        h = parent;
    }
}

static void reset_response(Request *r)
{
    if (r->h2) { nghttp2_session_del(r->h2); r->h2 = 0; }
    if (r->conn) { conn_close(r->conn); r->conn = 0; }
    hdr_free(&r->resp);
    free(r->status_text); r->status_text = 0;
    free(r->resp_version); r->resp_version = 0;
    buf_free(&r->in);
    buf_free(&r->out);
    r->status = r->http2 = r->framing = r->chunk_state = r->eof = 0;
    r->h2_headers = r->h2_closed = 0;
    r->h2_error = 0;
    r->stream = 0;
    r->left = 0;
    r->body_sent = 0;
    r->websocket_upgrade = 0;
}

static void free_hdr(Hdr *h)
{
    h->magic = 0;
    if (h->type == WINHTTP_HANDLE_TYPE_SESSION) {
        free(((Session *)h)->agent);
    } else if (h->type == WINHTTP_HANDLE_TYPE_CONNECT) {
        conn_close((Conn *)InterlockedExchangePointer((PVOID volatile *)&((Connect *)h)->idle, 0));
        free(((Connect *)h)->host);
    } else if (h->type == WINHTTP_HANDLE_TYPE_PROXY_RESOLVER_) {
        /* (nothing of its own) */
    } else if (h->type == WINHTTP_HANDLE_TYPE_WEBSOCKET_) {
        WebSocket *ws = (WebSocket *)h;
        conn_close(ws->conn);
        buf_free(&ws->in);
    } else {
        Request *r = (Request *)h;
        reset_response(r);
        free(r->agent); free(r->verb); free(r->path); free(r->version); free(r->host); free(r->auth);
        hdr_free(&r->req);
        buf_free(&r->body);
    }
    free(h);
}

/* -----------------------------------------------------------------------
 * The connection: TCP, and TLS through Schannel
 * ----------------------------------------------------------------------- */
static int send_all(SOCKET s, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        int k = send(s, b, n > 0x10000 ? 0x10000 : (int)n, 0);
        if (k <= 0) return 0;
        b += k;
        n -= k;
    }
    return 1;
}

/* Wait for the socket to be readable: 0 on time-out */
static int wait_readable(Conn *c)
{
    if (c->receive_ms <= 0) return 1;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(c->s, &rd);
    struct timeval tv = { c->receive_ms / 1000, (c->receive_ms % 1000) * 1000 };
    return select(0, &rd, 0, 0, &tv) > 0;
}

/* Some bytes from the socket into @b: >0, 0 at the end, -1 on error
 * (WinHTTP's code in *err) */
static int sock_read(Conn *c, Buf *b, DWORD *err)
{
    char tmp[16384];
    if (!wait_readable(c)) { *err = ERROR_WINHTTP_TIMEOUT; return -1; }
    int n = recv(c->s, tmp, sizeof(tmp), 0);
    if (n < 0) { *err = ERROR_WINHTTP_CONNECTION_ERROR; return -1; }
    if (n > 0 && !buf_add(b, tmp, n)) { *err = ERROR_NOT_ENOUGH_MEMORY; return -1; }
    return n;
}

static DWORD tls_error(SECURITY_STATUS st)
{
    switch (st) {
    case SEC_E_UNTRUSTED_ROOT: return ERROR_WINHTTP_SECURE_INVALID_CA;
    case SEC_E_WRONG_PRINCIPAL: return ERROR_WINHTTP_SECURE_CERT_CN_INVALID;
    case SEC_E_CERT_EXPIRED: return ERROR_WINHTTP_SECURE_CERT_DATE_INVALID;
    }
    return ERROR_WINHTTP_SECURE_FAILURE;
}

static DWORD tls_handshake(Conn *c, const char *host, DWORD security, int h2)
{
    SchannelCred sc = { 4 };
    DWORD manual = security & (SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                               SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE | SECURITY_FLAG_IGNORE_CERT_CN_INVALID);
    if (security & SECURITY_FLAG_IGNORE_CERT_CN_INVALID) sc.dwFlags |= SCH_CRED_NO_SERVERNAME_CHECK;
    if (manual & ~SECURITY_FLAG_IGNORE_CERT_CN_INVALID) sc.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;
    if (AcquireCredentialsHandleW(0, L"Microsoft Unified Security Protocol Provider", SECPKG_CRED_OUTBOUND, 0, &sc,
                                  0, 0, &c->cred, 0))
        return ERROR_WINHTTP_SECURE_FAILURE;
    c->have_cred = 1;

    /* SEC_APPLICATION_PROTOCOLS: { size; { ext = ALPN; len; "h2", "http/1.1" } } */
    BYTE alpn[64];
    static const BYTE list_h2[] = "\x02h2\x08http/1.1", list_h1[] = "\x08http/1.1";
    const BYTE *list = h2 ? list_h2 : list_h1;
    USHORT ll = (USHORT)(h2 ? sizeof(list_h2) - 1 : sizeof(list_h1) - 1);
    *(ULONG *)alpn = 6 + ll;
    *(int *)(alpn + 4) = 2;
    *(USHORT *)(alpn + 8) = ll;
    memcpy(alpn + 10, list, ll);

    WCHAR *whost = wide(host);
    ULONG req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                ISC_REQ_STREAM | ((sc.dwFlags & SCH_CRED_MANUAL_CRED_VALIDATION) ? ISC_REQ_MANUAL_CRED_VALIDATION : 0);
    ULONG attr;
    DWORD err = 0;
    int first = 1;
    for (;;) {
        SecBuffer ib[2] = { { 0, SECBUFFER_TOKEN, 0 }, { 0, SECBUFFER_EMPTY, 0 } };
        SecBuffer ob[1] = { { 0, SECBUFFER_TOKEN, 0 } };
        SecBufferDesc in = { SECBUFFER_VERSION, 2, ib }, out = { SECBUFFER_VERSION, 1, ob };
        if (first) {
            ib[0].BufferType = SECBUFFER_APPLICATION_PROTOCOLS;
            ib[0].pvBuffer = alpn;
            ib[0].cbBuffer = 10 + ll;
            in.cBuffers = 1;
        } else {
            ib[0].pvBuffer = c->raw.p + c->raw.off;
            ib[0].cbBuffer = (ULONG)buf_avail(&c->raw);
        }
        SECURITY_STATUS st = InitializeSecurityContextW(&c->cred, first ? 0 : &c->ctx, first ? whost : 0, req, 0, 0,
                                                        &in, 0, &c->ctx, &out, &attr, 0);
        if (first) c->have_ctx = 1;
        first = 0;
        if (ob[0].pvBuffer) {
            int ok = !ob[0].cbBuffer || send_all(c->s, ob[0].pvBuffer, ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!ok) { err = ERROR_WINHTTP_CONNECTION_ERROR; break; }
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (sock_read(c, &c->raw, &err) <= 0) { if (!err) err = ERROR_WINHTTP_SECURE_FAILURE; break; }
            continue;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) { err = tls_error(st); break; }
        /* what the call did not read stays for the next one */
        size_t extra = ib[1].BufferType == SECBUFFER_EXTRA ? ib[1].cbBuffer : 0;
        if (in.cBuffers == 2) c->raw.off = c->raw.len - extra;
        if (st == SEC_E_OK) break;
        if (!extra && sock_read(c, &c->raw, &err) <= 0) { if (!err) err = ERROR_WINHTTP_SECURE_FAILURE; break; }
    }
    free(whost);
    if (err) return err;
    if (QueryContextAttributesW(&c->ctx, SECPKG_ATTR_STREAM_SIZES, &c->sizes)) return ERROR_WINHTTP_SECURE_FAILURE;
    AppProto ap;
    memset(&ap, 0, sizeof(ap));
    if (!QueryContextAttributesW(&c->ctx, SECPKG_ATTR_APPLICATION_PROTOCOL, &ap) && ap.status == 1 &&
        ap.size < sizeof(c->alpn))
        memcpy(c->alpn, ap.id, ap.size);
    c->tls = 1;
    return 0;
}

static DWORD conn_open(Conn **out, const char *host, WORD port, int secure, DWORD security, int h2, int receive_ms)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char ps[8];
    int pl = 0;
    for (WORD p = port, d = 10000; d; d /= 10) if (p / d || pl || d == 1) ps[pl++] = (char)('0' + p / d % 10);
    ps[pl] = 0;
    if (getaddrinfo(host, ps, &hints, &res)) return ERROR_WINHTTP_NAME_NOT_RESOLVED;
    SOCKET s = INVALID_SOCKET;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return ERROR_WINHTTP_CANNOT_CONNECT;
    Conn *c = xcalloc(sizeof(Conn));
    if (!c) { closesocket(s); return ERROR_NOT_ENOUGH_MEMORY; }
    c->s = s;
    c->security = security;
    c->receive_ms = receive_ms;
    if (secure) {
        DWORD e = tls_handshake(c, host, security, h2);
        if (e) { conn_close(c); return e; }
    }
    *out = c;
    return 0;
}

static void conn_close(Conn *c)
{
    if (!c) return;
    if (c->tls) {                                      /* close_notify */
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer b = { sizeof(type), SECBUFFER_TOKEN, &type };
        SecBufferDesc d = { SECBUFFER_VERSION, 1, &b };
        if (!ApplyControlToken(&c->ctx, &d)) {
            SecBuffer ob = { 0, SECBUFFER_TOKEN, 0 };
            SecBufferDesc out = { SECBUFFER_VERSION, 1, &ob };
            ULONG attr;
            InitializeSecurityContextW(&c->cred, &c->ctx, 0, ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM, 0, 0, 0, 0,
                                       &c->ctx, &out, &attr, 0);
            if (ob.pvBuffer) { if (ob.cbBuffer) send_all(c->s, ob.pvBuffer, ob.cbBuffer); FreeContextBuffer(ob.pvBuffer); }
        }
    }
    if (c->have_ctx) DeleteSecurityContext(&c->ctx);
    if (c->have_cred) FreeCredentialsHandle(&c->cred);
    closesocket(c->s);
    buf_free(&c->raw);
    buf_free(&c->plain);
    free(c);
}

static DWORD conn_write(Conn *c, const void *data, size_t n)
{
    if (!c->tls) return send_all(c->s, data, n) ? 0 : ERROR_WINHTTP_CONNECTION_ERROR;
    const BYTE *p = data;
    size_t max = c->sizes.cbMaximumMessage ? c->sizes.cbMaximumMessage : 16384;
    BYTE *rec = malloc(c->sizes.cbHeader + max + c->sizes.cbTrailer);
    if (!rec) return ERROR_NOT_ENOUGH_MEMORY;
    DWORD err = 0;
    while (n && !err) {
        size_t k = n < max ? n : max;
        memcpy(rec + c->sizes.cbHeader, p, k);
        SecBuffer b[4] = {
            { c->sizes.cbHeader, SECBUFFER_STREAM_HEADER, rec },
            { (ULONG)k, SECBUFFER_DATA, rec + c->sizes.cbHeader },
            { c->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, rec + c->sizes.cbHeader + k },
            { 0, SECBUFFER_EMPTY, 0 },
        };
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&c->ctx, 0, &d, 0)) { err = ERROR_WINHTTP_SECURE_FAILURE; break; }
        for (int i = 0; i < 3 && !err; i++)
            if (b[i].cbBuffer && !send_all(c->s, b[i].pvBuffer, b[i].cbBuffer)) err = ERROR_WINHTTP_CONNECTION_ERROR;
        p += k;
        n -= k;
    }
    free(rec);
    return err;
}

/* Some bytes from the server into @b: >0, 0 at the end, -1 on error */
static int conn_read(Conn *c, Buf *b, DWORD *err)
{
    if (!c->tls) return c->eof ? 0 : sock_read(c, b, err);
    for (;;) {
        size_t n = buf_avail(&c->plain);
        if (n) {
            if (!buf_add(b, c->plain.p + c->plain.off, n)) { *err = ERROR_NOT_ENOUGH_MEMORY; return -1; }
            c->plain.off = c->plain.len = 0;
            return (int)n;
        }
        if (c->eof) return 0;
        if (buf_avail(&c->raw)) {
            SecBuffer x[4] = { { (ULONG)buf_avail(&c->raw), SECBUFFER_DATA, c->raw.p + c->raw.off },
                               { 0, SECBUFFER_EMPTY, 0 }, { 0, SECBUFFER_EMPTY, 0 }, { 0, SECBUFFER_EMPTY, 0 } };
            SecBufferDesc d = { SECBUFFER_VERSION, 4, x };
            SECURITY_STATUS st = DecryptMessage(&c->ctx, &d, 0, 0);
            if (st == SEC_E_OK || st == SEC_I_CONTEXT_EXPIRED || st == SEC_I_RENEGOTIATE) {
                size_t extra = 0;
                for (int i = 0; i < 4; i++) {
                    if (x[i].BufferType == SECBUFFER_DATA && x[i].cbBuffer &&
                        !buf_add(&c->plain, x[i].pvBuffer, x[i].cbBuffer)) { *err = ERROR_NOT_ENOUGH_MEMORY; return -1; }
                    if (x[i].BufferType == SECBUFFER_EXTRA) extra = x[i].cbBuffer;
                }
                c->raw.off = c->raw.len - extra;
                if (st == SEC_I_CONTEXT_EXPIRED) c->eof = 1;
                continue;
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE) { *err = ERROR_WINHTTP_SECURE_FAILURE; return -1; }
        }
        int r = sock_read(c, &c->raw, err);
        if (r < 0) return -1;
        if (r == 0) { c->eof = 1; if (!buf_avail(&c->plain)) return 0; }
    }
}

/* -----------------------------------------------------------------------
 * HTTP/1.1
 * ----------------------------------------------------------------------- */
static int default_port(Request *r) { return r->port == (r->secure ? 443 : 80); }

/* "host[:port]", IPv6 literals in brackets */
static void put_authority(Buf *b, Request *r)
{
    int v6 = strchr(r->host, ':') != 0;
    if (v6) buf_str(b, "[");
    buf_str(b, r->host);
    if (v6) buf_str(b, "]");
    if (!default_port(r)) {
        char p[8];
        snprintf(p, sizeof(p), ":%u", r->port);
        buf_str(b, p);
    }
}

static int has_body(Request *r)
{
    return buf_avail(&r->body) || r->body_total || (!ieq(r->verb, "GET") && !ieq(r->verb, "HEAD"));
}

static DWORD h1_send(Request *r)
{
    Buf b = { 0 };
    buf_str(&b, r->verb);
    buf_str(&b, " ");
    buf_str(&b, r->path);
    buf_str(&b, " ");
    buf_str(&b, r->version ? r->version : "HTTP/1.1");
    buf_str(&b, "\r\n");
    if (!hdr_find(&r->req, "Host", 0)) { buf_str(&b, "Host: "); put_authority(&b, r); buf_str(&b, "\r\n"); }
    if ((r->h.disable & WINHTTP_DISABLE_KEEP_ALIVE) && !hdr_find(&r->req, "Connection", 0))
        buf_str(&b, "Connection: close\r\n");
    if (r->agent && *r->agent && !hdr_find(&r->req, "User-Agent", 0)) {
        buf_str(&b, "User-Agent: "); buf_str(&b, r->agent); buf_str(&b, "\r\n");
    }
    if (r->auth && !hdr_find(&r->req, "Authorization", 0)) {
        buf_str(&b, "Authorization: "); buf_str(&b, r->auth); buf_str(&b, "\r\n");
    }
    for (int i = 0; i < r->req.n; i++) {
        if (ieq(r->req.h[i].name, "Content-Length")) continue;
        buf_str(&b, r->req.h[i].name); buf_str(&b, ": "); buf_str(&b, r->req.h[i].value); buf_str(&b, "\r\n");
    }
    if (has_body(r)) {
        char n[32];
        snprintf(n, sizeof(n), "Content-Length: %u\r\n", (unsigned)buf_avail(&r->body));
        buf_str(&b, n);
    }
    buf_str(&b, "\r\n");
    if (buf_avail(&r->body)) buf_add(&b, r->body.p + r->body.off, buf_avail(&r->body));
    DWORD e = conn_write(r->conn, b.p, b.len);
    buf_free(&b);
    return e;
}

/* The offset just past "\r\n\r\n" in @b, or 0 */
static size_t header_end(Buf *b)
{
    const BYTE *p = b->p + b->off;
    size_t n = buf_avail(b);
    for (size_t i = 3; i < n; i++)
        if (p[i] == '\n' && p[i - 1] == '\r' && p[i - 2] == '\n' && p[i - 3] == '\r') return i + 1;
    return 0;
}

static int header_has_token(Headers *hs, const char *name, const char *token);

static DWORD h1_receive(Request *r)
{
    DWORD err = 0;
    for (;;) {                                         /* skip 1xx responses (100 Continue) */
        size_t end;
        while (!(end = header_end(&r->in))) {
            if (buf_avail(&r->in) > 256 * 1024) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
            int n = conn_read(r->conn, &r->in, &err);
            if (n < 0) return err;
            if (n == 0) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        }
        const char *p = (const char *)r->in.p + r->in.off, *e = p + end - 2;
        const char *nl = strstr(p, "\r\n");
        if (nl - p < 12 || memcmp(p, "HTTP/", 5)) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        const char *sp = memchr(p, ' ', nl - p);
        if (!sp) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        int status = 0;
        const char *q = sp + 1;
        while (q < nl && *q >= '0' && *q <= '9') status = status * 10 + (*q++ - '0');
        if (*q == ' ') q++;
        hdr_free(&r->resp);
        free(r->resp_version);
        free(r->status_text);
        r->resp_version = dup_n(p, sp - p);
        r->status_text = dup_n(q, nl - q);
        r->status = status;
        for (const char *l = nl + 2; l < e;) {
            const char *le = strstr(l, "\r\n");
            if (!le || le > e) le = e;
            const char *colon = memchr(l, ':', le - l);
            if (colon) {
                const char *v = colon + 1;
                while (v < le && (*v == ' ' || *v == '\t')) v++;
                const char *ve = le;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                hdr_add(&r->resp, l, colon - l, v, ve - v);
            }
            l = le + 2;
        }
        r->in.off += end;
        if (status == 101 || status >= 200 || status < 100) break;
    }
    r->websocket_upgrade = r->status == 101 &&
        header_has_token(&r->resp, "Upgrade", "websocket") &&
        header_has_token(&r->resp, "Connection", "upgrade") &&
        hdr_find(&r->resp, "Sec-WebSocket-Accept", 0) != 0;
    Header *te = hdr_find(&r->resp, "Transfer-Encoding", 0), *cl = hdr_find(&r->resp, "Content-Length", 0);
    if (r->websocket_upgrade || ieq(r->verb, "HEAD") || r->status == 204 || r->status == 304)
        r->framing = BODY_NONE;
    else if (te && strstr(te->value, "chunked")) r->framing = BODY_CHUNKED;
    else if (cl) {
        r->framing = BODY_LENGTH;
        r->left = 0;
        for (const char *c = cl->value; *c >= '0' && *c <= '9'; c++) r->left = r->left * 10 + (*c - '0');
    } else r->framing = BODY_CLOSE;
    if (r->framing == BODY_NONE || (r->framing == BODY_LENGTH && !r->left)) r->eof = 1;
    return 0;
}

/* Move body bytes from r->in to r->out; read more when nothing moved */
static DWORD h1_fill(Request *r)
{
    DWORD err = 0;
    while (!r->eof && !buf_avail(&r->out)) {
        size_t n = buf_avail(&r->in);
        const char *p = (const char *)r->in.p + r->in.off;
        int need_more = !n;
        if (n && r->framing == BODY_CLOSE) {
            buf_add(&r->out, p, n);
            r->in.off += n;
        } else if (n && r->framing == BODY_LENGTH) {
            size_t k = n < r->left ? n : (size_t)r->left;
            buf_add(&r->out, p, k);
            r->in.off += k;
            r->left -= k;
            if (!r->left) r->eof = 1;
        } else if (n) {                                /* chunked */
            if (r->chunk_state == 1) {                 /* in a chunk's data */
                size_t k = n < r->left ? n : (size_t)r->left;
                buf_add(&r->out, p, k);
                r->in.off += k;
                r->left -= k;
                if (!r->left) r->chunk_state = 2;
                continue;
            }
            const char *nl = 0;
            for (size_t i = 0; i + 1 < n; i++) if (p[i] == '\r' && p[i + 1] == '\n') { nl = p + i; break; }
            if (!nl) need_more = 1;
            else if (r->chunk_state == 2) {            /* the CRLF after a chunk */
                r->in.off += nl - p + 2;
                r->chunk_state = 0;
            } else if (r->chunk_state == 3) {          /* trailers, up to an empty line */
                r->in.off += nl - p + 2;
                if (nl == p) r->eof = 1;
            } else {                                   /* the chunk size line */
                unsigned long long size = 0;
                for (const char *c = p; c < nl; c++) {
                    int d = *c >= '0' && *c <= '9' ? *c - '0' : lower(*c) >= 'a' && lower(*c) <= 'f' ? lower(*c) - 'a' + 10 : -1;
                    if (d < 0) break;
                    size = size * 16 + d;
                }
                r->in.off += nl - p + 2;
                r->left = size;
                r->chunk_state = size ? 1 : 3;
            }
        }
        if (need_more) {
            int k = conn_read(r->conn, &r->in, &err);
            if (k < 0) return err;
            if (k == 0) {
                if (r->framing == BODY_CLOSE) r->eof = 1;
                else return ERROR_WINHTTP_CONNECTION_ERROR;    /* cut short */
            }
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * HTTP/2 (nghttp2)
 * ----------------------------------------------------------------------- */
static nghttp2_ssize h2_send_cb(nghttp2_session *s, const uint8_t *data, size_t len, int flags, void *user)
{
    (void)s; (void)flags;
    Request *r = user;
    DWORD e = conn_write(r->conn, data, len);
    if (e) { r->err = e; return NGHTTP2_ERR_CALLBACK_FAILURE; }
    return (nghttp2_ssize)len;
}

static int h2_header_cb(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                        const uint8_t *value, size_t vl, uint8_t flags, void *user)
{
    (void)s; (void)flags;
    Request *r = user;
    if (f->hd.type != NGHTTP2_HEADERS || f->hd.stream_id != r->stream || r->h2_headers) return 0;
    if (nl == 7 && !memcmp(name, ":status", 7)) {
        int st = 0;
        for (size_t i = 0; i < vl; i++) st = st * 10 + (value[i] - '0');
        r->status = st;
        hdr_free(&r->resp);                            /* a final response after 1xx */
    } else if (nl && name[0] != ':') {
        hdr_add(&r->resp, (const char *)name, nl, (const char *)value, vl);
    }
    return 0;
}

static int h2_frame_cb(nghttp2_session *s, const nghttp2_frame *f, void *user)
{
    (void)s;
    Request *r = user;
    if (f->hd.type == NGHTTP2_HEADERS && f->hd.stream_id == r->stream && r->status >= 200 &&
        (f->hd.flags & NGHTTP2_FLAG_END_HEADERS))
        r->h2_headers = 1;
    return 0;
}

static int h2_data_cb(nghttp2_session *s, uint8_t flags, int32_t id, const uint8_t *data, size_t len, void *user)
{
    (void)s; (void)flags;
    Request *r = user;
    if (id == r->stream && !buf_add(&r->out, data, len)) return NGHTTP2_ERR_CALLBACK_FAILURE;
    return 0;
}

static int h2_close_cb(nghttp2_session *s, int32_t id, uint32_t code, void *user)
{
    (void)s;
    Request *r = user;
    if (id == r->stream) { r->h2_closed = 1; r->h2_error = code; }
    return 0;
}

static nghttp2_ssize h2_body_cb(nghttp2_session *s, int32_t id, uint8_t *buf, size_t len, uint32_t *flags,
                                nghttp2_data_source *src, void *user)
{
    (void)s; (void)id; (void)src;
    Request *r = user;
    size_t left = buf_avail(&r->body) - r->body_sent;
    size_t k = left < len ? left : len;
    memcpy(buf, r->body.p + r->body.off + r->body_sent, k);
    r->body_sent += k;
    if (r->body_sent == buf_avail(&r->body)) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (nghttp2_ssize)k;
}

/* Send what nghttp2 has queued, then read and feed one batch from the server */
static DWORD h2_pump(Request *r)
{
    if (nghttp2_session_send(r->h2)) return r->err ? r->err : ERROR_WINHTTP_CONNECTION_ERROR;
    if (!nghttp2_session_want_read(r->h2) && !nghttp2_session_want_write(r->h2)) {
        if (!r->h2_closed) return ERROR_WINHTTP_CONNECTION_ERROR;
        return 0;
    }
    Buf in = { 0 };
    DWORD err = 0;
    int n = conn_read(r->conn, &in, &err);
    if (n <= 0) { buf_free(&in); return n < 0 ? err : ERROR_WINHTTP_CONNECTION_ERROR; }
    nghttp2_ssize used = nghttp2_session_mem_recv2(r->h2, in.p, in.len);
    buf_free(&in);
    if (used < 0) return r->err ? r->err : ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    if (nghttp2_session_send(r->h2)) return r->err ? r->err : ERROR_WINHTTP_CONNECTION_ERROR;
    return 0;
}

static void nv_add(nghttp2_nv *nv, int *n, const char *name, const char *value)
{
    nv[*n].name = (uint8_t *)name;
    nv[*n].namelen = strlen(name);
    nv[*n].value = (uint8_t *)value;
    nv[*n].valuelen = strlen(value);
    nv[*n].flags = NGHTTP2_NV_FLAG_NONE;
    (*n)++;
}

static DWORD h2_send(Request *r)
{
    nghttp2_session_callbacks *cbs;
    if (nghttp2_session_callbacks_new(&cbs)) return ERROR_NOT_ENOUGH_MEMORY;
    nghttp2_session_callbacks_set_send_callback2(cbs, h2_send_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, h2_header_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, h2_frame_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, h2_data_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, h2_close_cb);
    int rc = nghttp2_session_client_new(&r->h2, cbs, r);
    nghttp2_session_callbacks_del(cbs);
    if (rc) return ERROR_NOT_ENOUGH_MEMORY;
    nghttp2_settings_entry iv[] = { { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
                                    { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 20 } };
    nghttp2_submit_settings(r->h2, NGHTTP2_FLAG_NONE, iv, 2);

    Buf auth = { 0 };
    put_authority(&auth, r);
    buf_add(&auth, "", 1);
    int max = r->req.n + 8, n = 0;
    nghttp2_nv *nv = calloc(max, sizeof(nghttp2_nv));
    char **names = calloc(max, sizeof(char *));
    char len[24];
    if (!nv || !names) { free(nv); free(names); buf_free(&auth); return ERROR_NOT_ENOUGH_MEMORY; }
    nv_add(nv, &n, ":method", r->verb);
    nv_add(nv, &n, ":scheme", "https");
    nv_add(nv, &n, ":authority", (char *)auth.p);
    nv_add(nv, &n, ":path", r->path);
    if (r->agent && *r->agent && !hdr_find(&r->req, "User-Agent", 0)) nv_add(nv, &n, "user-agent", r->agent);
    if (r->auth && !hdr_find(&r->req, "Authorization", 0)) nv_add(nv, &n, "authorization", r->auth);
    for (int i = 0; i < r->req.n; i++) {
        const char *nm = r->req.h[i].name;
        /* connection-specific headers have no place in HTTP/2 */
        if (ieq(nm, "Host") || ieq(nm, "Connection") || ieq(nm, "Keep-Alive") || ieq(nm, "Proxy-Connection") ||
            ieq(nm, "Transfer-Encoding") || ieq(nm, "Upgrade") || ieq(nm, "Content-Length"))
            continue;
        char *l = names[i] = dup_s(nm);
        for (char *c = l; c && *c; c++) *c = (char)lower(*c);
        if (l) nv_add(nv, &n, l, r->req.h[i].value);
    }
    if (has_body(r)) {
        snprintf(len, sizeof(len), "%u", (unsigned)buf_avail(&r->body));
        nv_add(nv, &n, "content-length", len);
    }
    nghttp2_data_provider2 dp = { { 0 }, h2_body_cb };
    r->stream = nghttp2_submit_request2(r->h2, 0, nv, n, buf_avail(&r->body) ? &dp : 0, r);
    for (int i = 0; i < max; i++) free(names[i]);
    free(names);
    free(nv);
    buf_free(&auth);
    if (r->stream < 0) return ERROR_WINHTTP_INTERNAL_ERROR;
    return 0;
}

static DWORD h2_receive(Request *r)
{
    while (!r->h2_headers) {
        if (r->h2_closed) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        DWORD e = h2_pump(r);
        if (e) return e;
    }
    r->resp_version = dup_s("HTTP/2");
    r->status_text = dup_s("");
    if (ieq(r->verb, "HEAD")) r->eof = 1;
    return 0;
}

static DWORD h2_fill(Request *r)
{
    while (!buf_avail(&r->out) && !r->eof) {
        if (r->h2_closed) {
            if (r->h2_error) return ERROR_WINHTTP_CONNECTION_ERROR;
            r->eof = 1;
            break;
        }
        DWORD e = h2_pump(r);
        if (e) return e;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Requests: sending, redirects, reading
 * ----------------------------------------------------------------------- */
static DWORD do_connect(Request *r)
{
    reset_response(r);
    int h2 = r->secure && (r->h.protocols & WINHTTP_PROTOCOL_FLAG_HTTP2);
    Connect *parent = (Connect *)r->h.parent;
    if (parent && parent->h.magic == HMAGIC && parent->h.type == WINHTTP_HANDLE_TYPE_CONNECT &&
        parent->port == r->port && ieq(parent->host, r->host)) {
        Conn *idle = (Conn *)InterlockedExchangePointer((PVOID volatile *)&parent->idle, 0);
        if (idle) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(idle->s, &rd);
            struct timeval tv = { 0, 0 };
            if (idle->security == r->h.security && idle->tls == r->secure &&
                GetTickCount64() - idle->idle_since < 30000 && select(0, &rd, 0, 0, &tv) == 0) {
                idle->receive_ms = r->h.timeouts[3];
                r->conn = idle;
                return 0;
            }
            conn_close(idle);
        }
    }
    DWORD e = conn_open(&r->conn, r->host, r->port, r->secure, r->h.security, h2, r->h.timeouts[3]);
    if (e) return e;
    r->http2 = r->conn->tls && !strcmp(r->conn->alpn, "h2");
    return 0;
}

static int header_has_token(Headers *hs, const char *name, const char *token)
{
    size_t tn = strlen(token);
    for (int i = 0; i < hs->n; i++) {
        if (!ieq(hs->h[i].name, name)) continue;
        const char *p = hs->h[i].value;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            const char *end = p;
            while (*end && *end != ',') end++;
            const char *trim = end;
            while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
            if ((size_t)(trim - p) == tn && ieq_n(p, token, tn)) return 1;
            p = end;
        }
    }
    return 0;
}

static void cache_connection(Request *r)
{
    if (!r->conn || r->http2 || r->websocket_upgrade ||
        (r->h.disable & WINHTTP_DISABLE_KEEP_ALIVE) || r->framing == BODY_CLOSE ||
        buf_avail(&r->in) || header_has_token(&r->req, "Connection", "close") ||
        header_has_token(&r->resp, "Connection", "close") ||
        (!r->resp_version || strcmp(r->resp_version, "HTTP/1.1"))) return;
    Connect *parent = (Connect *)r->h.parent;
    if (!parent || parent->h.type != WINHTTP_HANDLE_TYPE_CONNECT || parent->h.magic != HMAGIC ||
        parent->port != r->port || !ieq(parent->host, r->host)) return;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(r->conn->s, &rd);
    struct timeval tv = { 0, 0 };
    if (select(0, &rd, 0, 0, &tv) != 0) return;
    r->conn->idle_since = GetTickCount64();
    if (!InterlockedCompareExchangePointer((PVOID volatile *)&parent->idle, r->conn, 0))
        r->conn = 0;
}

static DWORD do_send(Request *r)
{
    r->received = 0;
    DWORD e = do_connect(r);
    if (!e) r->sent = 1;
    return e;
}

/* The redirect's target: an absolute URL or a path on the same server */
static DWORD follow(Request *r, const char *loc)
{
    if (!_strnicmp(loc, "http://", 7) || !_strnicmp(loc, "https://", 8)) {
        int secure = lower(loc[4]) == 's';
        if (r->secure && !secure && r->h.redirect_policy != WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS)
            return ERROR_WINHTTP_REDIRECT_FAILED;
        const char *h = loc + (secure ? 8 : 7), *he = h;
        while (*he && *he != '/' && *he != '?' && *he != '#') he++;
        const char *hs = h, *hend = he, *colon = 0;
        if (*hs == '[') { const char *rb = memchr(hs, ']', he - hs); if (!rb) return ERROR_WINHTTP_INVALID_URL; hs++; hend = rb; colon = rb[1] == ':' ? rb + 1 : 0; }
        else { colon = memchr(hs, ':', he - hs); if (colon) hend = colon; }
        WORD port = secure ? 443 : 80;
        if (colon) { port = 0; for (const char *c = colon + 1; c < he; c++) port = (WORD)(port * 10 + (*c - '0')); }
        free(r->host);
        r->host = dup_n(hs, hend - hs);
        r->port = port;
        r->secure = secure;
        free(r->path);
        r->path = dup_s(*he ? he : "/");
    } else if (loc[0] == '/') {
        free(r->path);
        r->path = dup_s(loc);
    } else {                                           /* relative to the current path */
        const char *slash = strrchr(r->path, '/');
        size_t keep = slash ? (size_t)(slash - r->path + 1) : 0;
        char *p = malloc(keep + strlen(loc) + 2);
        if (!p) return ERROR_NOT_ENOUGH_MEMORY;
        if (keep) memcpy(p, r->path, keep); else p[keep++] = '/';
        strcpy(p + keep, loc);
        free(r->path);
        r->path = p;
    }
    return 0;
}

static DWORD do_receive(Request *r)
{
    if (!r->sent) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    for (;;) {
        if (!r->conn) { DWORD e = do_connect(r); if (e) return e; }
        DWORD e = r->http2 ? h2_send(r) : h1_send(r);
        if (!e) e = r->http2 ? h2_receive(r) : h1_receive(r);
        if (e) return e;
        int st = r->status;
        Header *loc = hdr_find(&r->resp, "Location", 0);
        if (!loc || (st != 301 && st != 302 && st != 303 && st != 307 && st != 308) ||
            (r->h.disable & WINHTTP_DISABLE_REDIRECTS) ||
            r->h.redirect_policy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER || r->redirects >= (int)r->h.max_redirects)
            break;
        char *target = dup_s(loc->value);
        if (!target) return ERROR_NOT_ENOUGH_MEMORY;
        e = follow(r, target);
        if (e) { free(target); return e; }
        r->redirects++;
        WCHAR *w = wide(target);
        notify(&r->h, WINHTTP_CALLBACK_STATUS_REDIRECT, w, w ? (DWORD)wcslen(w) : 0);
        free(w);
        free(target);
        if (st == 303 || ((st == 301 || st == 302) && ieq(r->verb, "POST"))) {
            free(r->verb);
            r->verb = dup_s("GET");
            buf_free(&r->body);
            r->body_total = 0;
            hdr_remove(&r->req, "Content-Type");
        }
        reset_response(r);
    }
    r->received = 1;
    return 0;
}

static DWORD fill(Request *r) { return r->http2 ? h2_fill(r) : h1_fill(r); }

static DWORD do_available(Request *r, DWORD *n)
{
    if (!r->received) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    DWORD e = fill(r);
    if (e) return e;
    size_t a = buf_avail(&r->out);
    *n = a > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)a;
    return 0;
}

static DWORD do_read(Request *r, void *buf, DWORD len, DWORD *got)
{
    *got = 0;
    if (!r->received) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    DWORD e = fill(r);
    if (e) return e;
    size_t a = buf_avail(&r->out), k = a < len ? a : len;
    memcpy(buf, r->out.p + r->out.off, k);
    r->out.off += k;
    *got = (DWORD)k;
    if (r->eof && !buf_avail(&r->out)) cache_connection(r);
    return 0;
}

static DWORD do_write(Request *r, const void *buf, DWORD len, DWORD *done)
{
    if (!r->sent || r->received) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if (!buf_add(&r->body, buf, len)) return ERROR_NOT_ENOUGH_MEMORY;
    *done = len;
    return 0;
}

/* -----------------------------------------------------------------------
 * Asynchronous calls: a worker thread runs the call and reports it
 * ----------------------------------------------------------------------- */
typedef struct { Request *r; int api; void *buf; DWORD len; } Job;

static void complete(Request *r, int api, DWORD err, void *buf, DWORD n)
{
    if (err) {
        WINHTTP_ASYNC_RESULT res = { (DWORD_PTR)api, err };
        notify(&r->h, WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &res, sizeof(res));
        return;
    }
    switch (api) {
    case API_SEND_REQUEST: notify(&r->h, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, 0, 0); break;
    case API_RECEIVE_RESPONSE: notify(&r->h, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, 0, 0); break;
    case API_QUERY_DATA_AVAILABLE: notify(&r->h, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &n, sizeof(n)); break;
    case API_READ_DATA: notify(&r->h, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, buf, n); break;
    case API_WRITE_DATA: notify(&r->h, WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, &n, sizeof(n)); break;
    }
}

static DWORD run(Request *r, int api, void *buf, DWORD len, DWORD *n)
{
    switch (api) {
    case API_SEND_REQUEST: return do_send(r);
    case API_RECEIVE_RESPONSE: return do_receive(r);
    case API_QUERY_DATA_AVAILABLE: return do_available(r, n);
    case API_READ_DATA: return do_read(r, buf, len, n);
    case API_WRITE_DATA: return do_write(r, buf, len, n);
    }
    return ERROR_INVALID_PARAMETER;
}

static DWORD WINAPI worker(LPVOID p)
{
    Job *j = p;
    DWORD n = 0, e = run(j->r, j->api, j->buf, j->len, &n);
    complete(j->r, j->api, e, j->buf, n);
    release(&j->r->h);
    free(j);
    return 0;
}

/* Run @api now, or (asynchronous handle) on a worker thread */
static BOOL call(Request *r, int api, void *buf, DWORD len, DWORD *out)
{
    if (r->h.async) {
        Job *j = xcalloc(sizeof(Job));
        if (!j) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        j->r = r; j->api = api; j->buf = buf; j->len = len;
        InterlockedIncrement(&r->h.refs);
        HANDLE t = CreateThread(0, 0, worker, j, 0, 0);
        if (!t) { release(&r->h); free(j); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        CloseHandle(t);
        if (out) *out = 0;
        return TRUE;
    }
    DWORD n = 0, e = run(r, api, buf, len, &n);
    if (out) *out = n;
    if (e) { SetLastError(e); return FALSE; }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * The API
 * ----------------------------------------------------------------------- */
WINHTTPAPI HINTERNET WINAPI WinHttpOpen(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
    (void)access; (void)proxy; (void)bypass;
    init();
    Session *s = xcalloc(sizeof(Session));
    if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    s->h.magic = HMAGIC;
    s->h.type = WINHTTP_HANDLE_TYPE_SESSION;
    s->h.refs = 1;
    s->h.async = (flags & WINHTTP_FLAG_ASYNC) != 0;
    s->h.redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    s->h.max_redirects = 10;
    s->h.timeouts[0] = 0;
    s->h.timeouts[1] = 60000;
    s->h.timeouts[2] = 30000;
    s->h.timeouts[3] = 30000;
    s->agent = utf8(agent);
    return s;
}

WINHTTPAPI HINTERNET WINAPI WinHttpConnect(HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD reserved)
{
    (void)reserved;
    Session *s = (Session *)get(session, WINHTTP_HANDLE_TYPE_SESSION);
    if (!s) return 0;
    if (!server || !*server) { SetLastError(ERROR_WINHTTP_INVALID_URL); return 0; }
    Connect *c = xcalloc(sizeof(Connect));
    if (!c) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    inherit(&c->h, &s->h, WINHTTP_HANDLE_TYPE_CONNECT);
    c->host = utf8(server);
    size_t n = strlen(c->host);
    if (n > 2 && c->host[0] == '[' && c->host[n - 1] == ']') {      /* an IPv6 literal */
        memmove(c->host, c->host + 1, n - 2);
        c->host[n - 2] = 0;
    }
    c->port = port;
    notify(&c->h, WINHTTP_CALLBACK_STATUS_HANDLE_CREATED, &c, sizeof(c));
    return c;
}

WINHTTPAPI HINTERNET WINAPI WinHttpOpenRequest(HINTERNET connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                                               LPCWSTR referrer, LPCWSTR *accept_types, DWORD flags)
{
    Connect *c = (Connect *)get(connect, WINHTTP_HANDLE_TYPE_CONNECT);
    if (!c) return 0;
    Request *r = xcalloc(sizeof(Request));
    if (!r) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    inherit(&r->h, &c->h, WINHTTP_HANDLE_TYPE_REQUEST);
    Session *s = (Session *)c->h.parent;
    r->agent = dup_s(s->agent);
    r->verb = verb && *verb ? utf8(verb) : dup_s("GET");
    r->path = object && *object ? utf8(object) : dup_s("/");
    if (r->path[0] != '/' && r->path[0] != '*') {
        char *p = malloc(strlen(r->path) + 2);
        if (p) { p[0] = '/'; strcpy(p + 1, r->path); free(r->path); r->path = p; }
    }
    r->version = version && *version ? utf8(version) : 0;
    r->host = dup_s(c->host);
    r->secure = (flags & WINHTTP_FLAG_SECURE) != 0;
    r->port = c->port ? c->port : r->secure ? 443 : 80;
    if (referrer && *referrer) { char *v = utf8(referrer); hdr_add(&r->req, "Referer", 7, v, strlen(v)); free(v); }
    if (accept_types && accept_types[0]) {
        Buf b = { 0 };
        for (int i = 0; accept_types[i]; i++) {
            char *v = utf8(accept_types[i]);
            if (i) buf_str(&b, ", ");
            buf_str(&b, v);
            free(v);
        }
        hdr_add(&r->req, "Accept", 6, (char *)b.p, b.len);
        buf_free(&b);
    }
    notify(&r->h, WINHTTP_CALLBACK_STATUS_HANDLE_CREATED, &r, sizeof(r));
    return r;
}

WINHTTPAPI BOOL WINAPI WinHttpCloseHandle(HINTERNET h)
{
    Hdr *x = get(h, 0);
    if (!x) return FALSE;
    release(x);
    return TRUE;
}

static DWORD ws_send_frame(WebSocket *ws, BYTE opcode, int fin, const BYTE *data, DWORD len)
{
    if (len && !data) return ERROR_INVALID_PARAMETER;
    BYTE mask[4], head[14];
    if (!SystemFunction036(mask, sizeof(mask))) return ERROR_WINHTTP_INTERNAL_ERROR;
    size_t hn = 0;
    head[hn++] = (BYTE)((fin ? 0x80 : 0) | opcode);
    if (len < 126) head[hn++] = (BYTE)(0x80 | len);
    else if (len <= 0xffff) {
        head[hn++] = 0x80 | 126;
        head[hn++] = (BYTE)(len >> 8);
        head[hn++] = (BYTE)len;
    } else {
        head[hn++] = 0x80 | 127;
        memset(head + hn, 0, 4);
        hn += 4;
        head[hn++] = (BYTE)(len >> 24);
        head[hn++] = (BYTE)(len >> 16);
        head[hn++] = (BYTE)(len >> 8);
        head[hn++] = (BYTE)len;
    }
    memcpy(head + hn, mask, sizeof(mask));
    hn += sizeof(mask);
    DWORD e = conn_write(ws->conn, head, hn);
    BYTE tmp[4096];
    for (DWORD off = 0; !e && off < len;) {
        DWORD n = len - off < sizeof(tmp) ? len - off : sizeof(tmp);
        for (DWORD i = 0; i < n; i++) tmp[i] = data[off + i] ^ mask[(off + i) & 3];
        e = conn_write(ws->conn, tmp, n);
        off += n;
    }
    return e;
}

static DWORD ws_read_exact(WebSocket *ws, BYTE *dst, size_t len)
{
    DWORD err = 0;
    while (buf_avail(&ws->in) < len) {
        int n = conn_read(ws->conn, &ws->in, &err);
        if (n < 0) return err;
        if (!n) return ERROR_WINHTTP_CONNECTION_ERROR;
    }
    memcpy(dst, ws->in.p + ws->in.off, len);
    ws->in.off += len;
    return 0;
}

static DWORD ws_read_frame_header(WebSocket *ws)
{
    BYTE h[2];
    DWORD e = ws_read_exact(ws, h, sizeof(h));
    if (e) return e;
    if ((h[0] & 0x70) || (h[1] & 0x80)) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    ws->frame_fin = (h[0] & 0x80) != 0;
    ws->frame_opcode = h[0] & 0x0f;
    ws->frame_left = h[1] & 0x7f;
    if (ws->frame_left == 126) {
        BYTE ext[2];
        if ((e = ws_read_exact(ws, ext, sizeof(ext)))) return e;
        ws->frame_left = ((unsigned long long)ext[0] << 8) | ext[1];
    } else if (ws->frame_left == 127) {
        BYTE ext[8];
        if ((e = ws_read_exact(ws, ext, sizeof(ext)))) return e;
        if (ext[0] & 0x80) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        ws->frame_left = 0;
        for (int i = 0; i < 8; i++) ws->frame_left = (ws->frame_left << 8) | ext[i];
    }
    if (ws->frame_opcode >= 8 && (!ws->frame_fin || ws->frame_left > 125))
        return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    if (ws->frame_opcode == 1 || ws->frame_opcode == 2) {
        if (ws->message_opcode) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        ws->message_opcode = ws->frame_opcode;
    } else if (ws->frame_opcode == 0) {
        if (!ws->message_opcode) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    } else if (ws->frame_opcode < 8 || ws->frame_opcode > 10) {
        return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    }
    return 0;
}

WINHTTPAPI HINTERNET WINAPI WinHttpWebSocketCompleteUpgrade(HINTERNET request, DWORD_PTR context)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return 0;
    if (!r->received || !r->websocket_upgrade || !r->conn) {
        SetLastError(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
        return 0;
    }
    WebSocket *ws = xcalloc(sizeof(WebSocket));
    if (!ws) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    inherit(&ws->h, r->h.parent, WINHTTP_HANDLE_TYPE_WEBSOCKET_);
    if (buf_avail(&r->in) && !buf_add(&ws->in, r->in.p + r->in.off, buf_avail(&r->in))) {
        release(&ws->h);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    ws->h.ctx = context;
    ws->conn = r->conn;
    r->conn = 0;
    notify(&ws->h, WINHTTP_CALLBACK_STATUS_HANDLE_CREATED, &ws, sizeof(ws));
    return ws;
}

WINHTTPAPI DWORD WINAPI WinHttpWebSocketSend(HINTERNET websocket, WINHTTP_WEB_SOCKET_BUFFER_TYPE type,
                                              PVOID buffer, DWORD length)
{
    WebSocket *ws = (WebSocket *)get(websocket, WINHTTP_HANDLE_TYPE_WEBSOCKET_);
    if (!ws) return GetLastError();
    BYTE opcode;
    int fin;
    switch (type) {
    case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: opcode = 2; fin = 1; break;
    case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE: opcode = 2; fin = 0; break;
    case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE: opcode = 1; fin = 1; break;
    case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE: opcode = 1; fin = 0; break;
    case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE: opcode = 8; fin = 1; break;
    default: return ERROR_INVALID_PARAMETER;
    }
    if (opcode == 8 && (length > 125 || length == 1)) return ERROR_INVALID_PARAMETER;
    DWORD e = ws_send_frame(ws, opcode, fin, buffer, length);
    if (!e && opcode == 8) ws->close_sent = 1;
    return e;
}

WINHTTPAPI DWORD WINAPI WinHttpWebSocketReceive(HINTERNET websocket, PVOID buffer, DWORD length,
                                                 LPDWORD read, WINHTTP_WEB_SOCKET_BUFFER_TYPE *type)
{
    WebSocket *ws = (WebSocket *)get(websocket, WINHTTP_HANDLE_TYPE_WEBSOCKET_);
    if (!ws) return GetLastError();
    if (!read || !type || (length && !buffer)) return ERROR_INVALID_PARAMETER;
    *read = 0;
    for (;;) {
        if (!ws->frame_left && !ws->close_received) {
            DWORD e = ws_read_frame_header(ws);
            if (e) return e;
            if (ws->frame_opcode == 8) {
                BYTE payload[125];
                DWORD n = (DWORD)ws->frame_left;
                if (n && (e = ws_read_exact(ws, payload, n))) return e;
                if (n == 1) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
                ws->close_status = n >= 2 ? (USHORT)((payload[0] << 8) | payload[1]) : 1005;
                ws->close_reason_len = n > 2 ? n - 2 : 0;
                if (ws->close_reason_len) memcpy(ws->close_reason, payload + 2, ws->close_reason_len);
                ws->close_received = 1;
                if (!ws->close_sent) {
                    e = ws_send_frame(ws, 8, 1, n ? payload : 0, n);
                    if (e) return e;
                    ws->close_sent = 1;
                }
                *type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
                return 0;
            }
            if (ws->frame_opcode == 9 || ws->frame_opcode == 10) {
                BYTE payload[125];
                DWORD n = (DWORD)ws->frame_left;
                if (n && (e = ws_read_exact(ws, payload, n))) return e;
                ws->frame_left = 0;
                if (ws->frame_opcode == 9 && (e = ws_send_frame(ws, 10, 1, payload, n))) return e;
                continue;
            }
        }
        if (ws->close_received) {
            *type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
            return 0;
        }
        if (ws->frame_left && !length) return ERROR_INSUFFICIENT_BUFFER;
        DWORD n = ws->frame_left < length ? (DWORD)ws->frame_left : length;
        DWORD e = n ? ws_read_exact(ws, buffer, n) : 0;
        if (e) return e;
        ws->frame_left -= n;
        *read = n;
        if (!ws->frame_left && ws->frame_fin) {
            *type = ws->message_opcode == 1 ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE :
                                              WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            ws->message_opcode = 0;
        } else {
            *type = ws->message_opcode == 1 ? WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE :
                                              WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        }
        return 0;
    }
}

WINHTTPAPI DWORD WINAPI WinHttpWebSocketShutdown(HINTERNET websocket, USHORT status, PVOID reason, DWORD length)
{
    WebSocket *ws = (WebSocket *)get(websocket, WINHTTP_HANDLE_TYPE_WEBSOCKET_);
    if (!ws) return GetLastError();
    if (ws->close_sent || length > 123 || (length && !reason)) return ERROR_INVALID_PARAMETER;
    BYTE payload[125] = { (BYTE)(status >> 8), (BYTE)status };
    if (length) memcpy(payload + 2, reason, length);
    DWORD e = ws_send_frame(ws, 8, 1, payload, length + 2);
    if (!e) ws->close_sent = 1;
    return e;
}

WINHTTPAPI DWORD WINAPI WinHttpWebSocketClose(HINTERNET websocket, USHORT status, PVOID reason, DWORD length)
{
    WebSocket *ws = (WebSocket *)get(websocket, WINHTTP_HANDLE_TYPE_WEBSOCKET_);
    if (!ws) return GetLastError();
    if (!ws->close_sent) {
        DWORD e = WinHttpWebSocketShutdown(websocket, status, reason, length);
        if (e) return e;
    }
    return WinHttpCloseHandle(websocket) ? 0 : GetLastError();
}

WINHTTPAPI DWORD WINAPI WinHttpWebSocketQueryCloseStatus(HINTERNET websocket, USHORT *status, PVOID reason,
                                                          DWORD length, LPDWORD reason_length)
{
    WebSocket *ws = (WebSocket *)get(websocket, WINHTTP_HANDLE_TYPE_WEBSOCKET_);
    if (!ws) return GetLastError();
    if (!status || !reason_length) return ERROR_INVALID_PARAMETER;
    if (!ws->close_received) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    *status = ws->close_status;
    *reason_length = ws->close_reason_len;
    if (length < ws->close_reason_len) return ERROR_INSUFFICIENT_BUFFER;
    if (ws->close_reason_len && !reason) return ERROR_INVALID_PARAMETER;
    if (ws->close_reason_len) memcpy(reason, ws->close_reason, ws->close_reason_len);
    return 0;
}

/* Headers in "Name: value\r\n" lines */
static BOOL add_headers(Request *r, LPCWSTR headers, DWORD length, DWORD modifiers)
{
    if (!headers) return TRUE;
    char *all = utf8_n(headers, length == (DWORD)-1 || !length ? -1 : (int)length);
    if (!all) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    BOOL ok = TRUE;
    for (char *l = all; *l;) {
        char *e = l;
        while (*e && *e != '\r' && *e != '\n') e++;
        char *colon = memchr(l, ':', e - l);
        if (colon && colon > l) {
            char *v = colon + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            char *name = dup_n(l, colon - l);
            if (modifiers & WINHTTP_ADDREQ_FLAG_REPLACE) {
                hdr_remove(&r->req, name);
                if (v < e) hdr_add(&r->req, l, colon - l, v, e - v);
            } else if (modifiers & WINHTTP_ADDREQ_FLAG_ADD_IF_NEW) {
                if (!hdr_find(&r->req, name, 0)) hdr_add(&r->req, l, colon - l, v, e - v);
                else if (!(modifiers & WINHTTP_ADDREQ_FLAG_ADD)) { SetLastError(ERROR_WINHTTP_HEADER_ALREADY_EXISTS_); ok = FALSE; }
            } else if (modifiers & WINHTTP_ADDREQ_FLAG_COALESCE) {
                Header *h = hdr_find(&r->req, name, 0);
                if (h) {
                    size_t a = strlen(h->value);
                    char *nv = realloc(h->value, a + 2 + (e - v) + 1);
                    if (nv) { memcpy(nv + a, ", ", 2); memcpy(nv + a + 2, v, e - v); nv[a + 2 + (e - v)] = 0; h->value = nv; }
                } else hdr_add(&r->req, l, colon - l, v, e - v);
            } else {
                hdr_add(&r->req, l, colon - l, v, e - v);
            }
            free(name);
        }
        l = e;
        while (*l == '\r' || *l == '\n') l++;
    }
    free(all);
    return ok;
}

WINHTTPAPI BOOL WINAPI WinHttpAddRequestHeaders(HINTERNET request, LPCWSTR headers, DWORD length, DWORD modifiers)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    if (!headers) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return add_headers(r, headers, length, modifiers);
}

WINHTTPAPI BOOL WINAPI WinHttpSendRequest(HINTERNET request, LPCWSTR headers, DWORD headers_length, LPVOID optional,
                                          DWORD optional_length, DWORD total_length, DWORD_PTR context)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    if (headers && !add_headers(r, headers, headers_length, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        return FALSE;
    r->h.ctx = context;
    buf_free(&r->body);
    if (optional && optional_length && !buf_add(&r->body, optional, optional_length)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    r->body_total = total_length > optional_length ? total_length : optional_length;
    r->sent = r->received = 0;
    r->redirects = 0;
    return call(r, API_SEND_REQUEST, 0, 0, 0);
}

WINHTTPAPI BOOL WINAPI WinHttpWriteData(HINTERNET request, LPCVOID buffer, DWORD length, LPDWORD written)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    return call(r, API_WRITE_DATA, (void *)buffer, length, written);
}

WINHTTPAPI BOOL WINAPI WinHttpReceiveResponse(HINTERNET request, LPVOID reserved)
{
    (void)reserved;
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    return call(r, API_RECEIVE_RESPONSE, 0, 0, 0);
}

WINHTTPAPI BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET request, LPDWORD available)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    return call(r, API_QUERY_DATA_AVAILABLE, 0, 0, available);
}

WINHTTPAPI BOOL WINAPI WinHttpReadData(HINTERNET request, LPVOID buffer, DWORD length, LPDWORD read)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    return call(r, API_READ_DATA, buffer, length, read);
}

/* -----------------------------------------------------------------------
 * Headers of the response (or of the request)
 * ----------------------------------------------------------------------- */
static const char *const g_names[] = {
    "Mime-Version", "Content-Type", "Content-Transfer-Encoding", "Content-ID", "Content-Description",
    "Content-Length", "Content-Language", "Allow", "Public", "Date", "Expires", "Last-Modified", "Message-ID",
    "URI", "Derived-From", "Cost", "Link", "Pragma", 0, 0, 0, 0, 0, "Connection", "Accept", "Accept-Charset",
    "Accept-Encoding", "Accept-Language", "Authorization", "Content-Encoding", "Forwarded", "From",
    "If-Modified-Since", "Location", "Orig-URI", "Referer", "Retry-After", "Server", "Title", "User-Agent",
    "WWW-Authenticate", "Proxy-Authenticate", "Accept-Ranges", "Set-Cookie", "Cookie", 0, "Refresh",
    "Content-Disposition", "Age", "Cache-Control", "Content-Base", "Content-Location", "Content-MD5",
    "Content-Range", "ETag", "Host", "If-Match", "If-None-Match", "If-Range", "If-Unmodified-Since",
    "Max-Forwards", "Proxy-Authorization", "Range", "Transfer-Encoding", "Upgrade", "Vary", "Via", "Warning",
    "Expect", "Proxy-Connection", "Unless-Modified-Since",
};

static BOOL put_string(const char *s, size_t n, void *buf, DWORD *len)
{
    int w = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, 0, 0);
    DWORD need = (DWORD)(w + 1) * sizeof(WCHAR);
    if (!buf || *len < need) { *len = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, buf, w);
    ((WCHAR *)buf)[w] = 0;
    *len = (DWORD)w * sizeof(WCHAR);
    return TRUE;
}

static int month_of(const char *m)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) if (ieq_n(m, names + i * 3, 3)) return i + 1;
    return 0;
}

/* "Sun, 06 Nov 1994 08:49:37 GMT" */
static BOOL parse_time(const char *s, SYSTEMTIME *st)
{
    memset(st, 0, sizeof(*st));
    const char *p = strchr(s, ',');
    p = p ? p + 1 : s;
    while (*p == ' ') p++;
    int d = 0, y = 0, h = 0, mi = 0, se = 0;
    while (*p >= '0' && *p <= '9') d = d * 10 + (*p++ - '0');
    while (*p == ' ' || *p == '-') p++;
    int m = month_of(p);
    if (!m || !d) return FALSE;
    p += 3;
    while (*p == ' ' || *p == '-') p++;
    while (*p >= '0' && *p <= '9') y = y * 10 + (*p++ - '0');
    if (y < 100) y += y < 70 ? 2000 : 1900;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    if (*p == ':') p++;
    while (*p >= '0' && *p <= '9') mi = mi * 10 + (*p++ - '0');
    if (*p == ':') p++;
    while (*p >= '0' && *p <= '9') se = se * 10 + (*p++ - '0');
    st->wYear = (WORD)y; st->wMonth = (WORD)m; st->wDay = (WORD)d;
    st->wHour = (WORD)h; st->wMinute = (WORD)mi; st->wSecond = (WORD)se;
    /* the day of the week (Sakamoto's method) */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = m < 3 ? y - 1 : y;
    st->wDayOfWeek = (WORD)((yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7);
    return TRUE;
}

WINHTTPAPI BOOL WINAPI WinHttpQueryHeaders(HINTERNET request, DWORD level, LPCWSTR name, LPVOID buffer,
                                           LPDWORD length, LPDWORD index)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    if (!length) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD info = level & 0xFFFF, mods = level & 0xFFFF0000;
    int reqh = (mods & WINHTTP_QUERY_FLAG_REQUEST_HEADERS) != 0;
    if (!reqh && !r->received) { SetLastError(ERROR_WINHTTP_INCORRECT_HANDLE_STATE); return FALSE; }
    Headers *hs = reqh ? &r->req : &r->resp;
    DWORD idx = index ? *index : 0;
    char num[16];
    const char *val = 0;
    char *owned = 0;
    size_t vlen = 0;

    if (info == WINHTTP_QUERY_RAW_HEADERS || info == WINHTTP_QUERY_RAW_HEADERS_CRLF) {
        Buf b = { 0 };
        const char *sep = info == WINHTTP_QUERY_RAW_HEADERS ? "\0" : "\r\n";
        size_t sl = info == WINHTTP_QUERY_RAW_HEADERS ? 1 : 2;
        if (reqh) {
            buf_str(&b, r->verb); buf_str(&b, " "); buf_str(&b, r->path); buf_str(&b, " ");
            buf_str(&b, r->version ? r->version : "HTTP/1.1");
        } else {
            snprintf(num, sizeof(num), " %d ", r->status);
            buf_str(&b, r->resp_version); buf_str(&b, num); buf_str(&b, r->status_text);
        }
        buf_add(&b, sep, sl);
        for (int i = 0; i < hs->n; i++) {
            buf_str(&b, hs->h[i].name); buf_str(&b, ": "); buf_str(&b, hs->h[i].value); buf_add(&b, sep, sl);
        }
        buf_add(&b, sep, sl);
        BOOL ok = put_string((char *)b.p, b.len, buffer, length);
        if (ok && info == WINHTTP_QUERY_RAW_HEADERS) *length -= sizeof(WCHAR);   /* (the list ends in two NULs) */
        buf_free(&b);
        return ok;
    }
    if (info == WINHTTP_QUERY_STATUS_CODE && !reqh) { snprintf(num, sizeof(num), "%d", r->status); val = num; }
    else if (info == WINHTTP_QUERY_STATUS_TEXT && !reqh) val = r->status_text;
    else if (info == WINHTTP_QUERY_VERSION) val = reqh ? (r->version ? r->version : "HTTP/1.1") : r->resp_version;
    else if (info == 45 /* REQUEST_METHOD */) val = r->verb;
    else {
        const char *hn = 0;
        if (info == WINHTTP_QUERY_CUSTOM) { if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; } owned = utf8(name); hn = owned; }
        else if (info < sizeof(g_names) / sizeof(g_names[0])) hn = g_names[info];
        if (!hn) { free(owned); SetLastError(ERROR_WINHTTP_INVALID_QUERY_REQUEST_); return FALSE; }
        Header *h = hdr_find(hs, hn, idx);
        free(owned);
        owned = 0;
        if (!h) { SetLastError(ERROR_WINHTTP_HEADER_NOT_FOUND); return FALSE; }
        val = h->value;
    }
    if (val && info != WINHTTP_QUERY_CUSTOM && info >= sizeof(g_names) / sizeof(g_names[0]) && idx) val = 0;
    if (!val) { SetLastError(ERROR_WINHTTP_HEADER_NOT_FOUND); return FALSE; }
    vlen = strlen(val);
    BOOL ok;
    if (mods & WINHTTP_QUERY_FLAG_NUMBER64) {
        if (!buffer || *length < 8) { *length = 8; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        unsigned long long v = 0;
        for (const char *c = val; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
        *(unsigned long long *)buffer = v;
        *length = 8;
        ok = TRUE;
    } else if (mods & WINHTTP_QUERY_FLAG_NUMBER) {
        if (!buffer || *length < 4) { *length = 4; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        DWORD v = 0;
        for (const char *c = val; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
        *(DWORD *)buffer = v;
        *length = 4;
        ok = TRUE;
    } else if (mods & WINHTTP_QUERY_FLAG_SYSTEMTIME) {
        if (!buffer || *length < sizeof(SYSTEMTIME)) { *length = sizeof(SYSTEMTIME); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        if (!parse_time(val, buffer)) { SetLastError(ERROR_WINHTTP_INVALID_SERVER_RESPONSE); return FALSE; }
        *length = sizeof(SYSTEMTIME);
        ok = TRUE;
    } else {
        ok = put_string(val, vlen, buffer, length);
    }
    if (ok && index) (*index)++;
    return ok;
}

/* -----------------------------------------------------------------------
 * Options
 * ----------------------------------------------------------------------- */
static BOOL put_dword(DWORD v, void *buf, DWORD *len)
{
    if (!len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!buf || *len < 4) { *len = 4; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *(DWORD *)buf = v;
    *len = 4;
    return TRUE;
}

static BOOL put_wide(const char *s, void *buf, DWORD *len)
{
    if (!len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return put_string(s ? s : "", s ? strlen(s) : 0, buf, len);
}

WINHTTPAPI BOOL WINAPI WinHttpSetOption(HINTERNET h, DWORD option, LPVOID buffer, DWORD length)
{
    Hdr *x = 0;
    if (h) { x = get(h, 0); if (!x) return FALSE; }
    DWORD v = buffer && length >= 4 ? *(DWORD *)buffer : 0;
    if (!x) return TRUE;                               /* global options: accepted */
    switch (option) {
    case WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL: x->protocols = v; break;
    case WINHTTP_OPTION_SECURITY_FLAGS: x->security = v; break;
    case WINHTTP_OPTION_REDIRECT_POLICY: x->redirect_policy = v; break;
    case WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS: x->max_redirects = v; break;
    case WINHTTP_OPTION_DISABLE_FEATURE: x->disable |= v; break;
    case WINHTTP_OPTION_ENABLE_FEATURE: x->disable &= ~v; break;
    case WINHTTP_OPTION_RESOLVE_TIMEOUT: x->timeouts[0] = (int)v; break;
    case WINHTTP_OPTION_CONNECT_TIMEOUT: x->timeouts[1] = (int)v; break;
    case WINHTTP_OPTION_SEND_TIMEOUT: x->timeouts[2] = (int)v; break;
    case WINHTTP_OPTION_RECEIVE_TIMEOUT: case WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT: x->timeouts[3] = (int)v; break;
    case WINHTTP_OPTION_CONTEXT_VALUE:
        if (!buffer || length < sizeof(DWORD_PTR)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        x->ctx = *(DWORD_PTR *)buffer;
        break;
    case WINHTTP_OPTION_USER_AGENT: {
        char *a = utf8_n(buffer, (int)length);
        char **dst = x->type == WINHTTP_HANDLE_TYPE_SESSION ? &((Session *)x)->agent :
                     x->type == WINHTTP_HANDLE_TYPE_REQUEST ? &((Request *)x)->agent : 0;
        if (dst) { free(*dst); *dst = a; } else free(a);
        break;
    }
    default: break;                                    /* accepted, no effect */
    }
    return TRUE;
}

WINHTTPAPI BOOL WINAPI WinHttpQueryOption(HINTERNET h, DWORD option, LPVOID buffer, LPDWORD length)
{
    Hdr *x = get(h, 0);
    if (!x) return FALSE;
    Request *r = x->type == WINHTTP_HANDLE_TYPE_REQUEST ? (Request *)x : 0;
    switch (option) {
    case WINHTTP_OPTION_HANDLE_TYPE: return put_dword(x->type, buffer, length);
    case WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL: return put_dword(x->protocols, buffer, length);
    case WINHTTP_OPTION_HTTP_PROTOCOL_USED:
        if (!r) break;
        return put_dword(r->http2 ? WINHTTP_PROTOCOL_FLAG_HTTP2 : 0, buffer, length);
    case WINHTTP_OPTION_SECURITY_FLAGS:
        return put_dword(x->security | (r && r->secure ? SECURITY_FLAG_SECURE | SECURITY_FLAG_STRENGTH_STRONG : 0),
                         buffer, length);
    case WINHTTP_OPTION_REDIRECT_POLICY: return put_dword(x->redirect_policy, buffer, length);
    case WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS: return put_dword(x->max_redirects, buffer, length);
    case WINHTTP_OPTION_RESOLVE_TIMEOUT: return put_dword(x->timeouts[0], buffer, length);
    case WINHTTP_OPTION_CONNECT_TIMEOUT: return put_dword(x->timeouts[1], buffer, length);
    case WINHTTP_OPTION_SEND_TIMEOUT: return put_dword(x->timeouts[2], buffer, length);
    case WINHTTP_OPTION_RECEIVE_TIMEOUT: return put_dword(x->timeouts[3], buffer, length);
    case WINHTTP_OPTION_CONTEXT_VALUE:
        if (!length) break;
        if (!buffer || *length < sizeof(DWORD_PTR)) { *length = sizeof(DWORD_PTR); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        *(DWORD_PTR *)buffer = x->ctx;
        *length = sizeof(DWORD_PTR);
        return TRUE;
    case WINHTTP_OPTION_PARENT_HANDLE:
        if (!length) break;
        if (!buffer || *length < sizeof(HINTERNET)) { *length = sizeof(HINTERNET); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        *(HINTERNET *)buffer = x->parent;
        *length = sizeof(HINTERNET);
        return TRUE;
    case WINHTTP_OPTION_USER_AGENT:
        return put_wide(x->type == WINHTTP_HANDLE_TYPE_SESSION ? ((Session *)x)->agent : r ? r->agent : 0, buffer, length);
    case WINHTTP_OPTION_URL: {
        if (!r) break;
        Buf b = { 0 };
        buf_str(&b, r->secure ? "https://" : "http://");
        put_authority(&b, r);
        buf_str(&b, r->path);
        buf_add(&b, "", 1);
        BOOL ok = put_wide((char *)b.p, buffer, length);
        buf_free(&b);
        return ok;
    }
    case WINHTTP_OPTION_SECURITY_KEY_BITNESS: return put_dword(r && r->secure ? 256 : 0, buffer, length);
    }
    SetLastError(ERROR_WINHTTP_INVALID_OPTION);
    return FALSE;
}

WINHTTPAPI BOOL WINAPI WinHttpSetTimeouts(HINTERNET h, int resolve, int connect, int send, int receive)
{
    Hdr *x = get(h, 0);
    if (!x) return FALSE;
    if (resolve < -1 || connect < -1 || send < -1 || receive < -1) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    x->timeouts[0] = resolve;
    x->timeouts[1] = connect;
    x->timeouts[2] = send;
    x->timeouts[3] = receive;
    return TRUE;
}

WINHTTPAPI WINHTTP_STATUS_CALLBACK WINAPI WinHttpSetStatusCallback(HINTERNET h, WINHTTP_STATUS_CALLBACK cb,
                                                                   DWORD flags, DWORD_PTR reserved)
{
    (void)reserved;
    Hdr *x = get(h, 0);
    if (!x) return WINHTTP_INVALID_STATUS_CALLBACK;
    WINHTTP_STATUS_CALLBACK old = x->cb;
    x->cb = cb;
    x->cb_flags = flags;
    return old;
}

/* Basic credentials; other schemes are not offered */
WINHTTPAPI BOOL WINAPI WinHttpSetCredentials(HINTERNET request, DWORD target, DWORD scheme, LPCWSTR user,
                                             LPCWSTR password, LPVOID params)
{
    (void)target; (void)params;
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    if (scheme != WINHTTP_AUTH_SCHEME_BASIC) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    char *u = utf8(user ? user : L""), *p = utf8(password ? password : L"");
    size_t n = strlen(u) + 1 + strlen(p);
    char *plain = malloc(n + 1), *out = malloc(6 + (n + 2) / 3 * 4 + 1);
    if (!plain || !out) { free(u); free(p); free(plain); free(out); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    strcpy(plain, u); strcat(plain, ":"); strcat(plain, p);
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    strcpy(out, "Basic ");
    char *o = out + 6;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (BYTE)plain[i] << 16 | (i + 1 < n ? (BYTE)plain[i + 1] << 8 : 0) | (i + 2 < n ? (BYTE)plain[i + 2] : 0);
        *o++ = b64[v >> 18 & 63];
        *o++ = b64[v >> 12 & 63];
        *o++ = i + 1 < n ? b64[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? b64[v & 63] : '=';
    }
    *o = 0;
    free(r->auth);
    r->auth = out;
    free(u); free(p); free(plain);
    return TRUE;
}

WINHTTPAPI BOOL WINAPI WinHttpQueryAuthSchemes(HINTERNET request, LPDWORD supported, LPDWORD first, LPDWORD target)
{
    Request *r = (Request *)get(request, WINHTTP_HANDLE_TYPE_REQUEST);
    if (!r) return FALSE;
    Header *h = r->received ? hdr_find(&r->resp, r->status == 407 ? "Proxy-Authenticate" : "WWW-Authenticate", 0) : 0;
    if (!h || !ieq_n(h->value, "Basic", 5)) { SetLastError(4317 /* ERROR_INVALID_OPERATION */); return FALSE; }
    if (supported) *supported = WINHTTP_AUTH_SCHEME_BASIC;
    if (first) *first = WINHTTP_AUTH_SCHEME_BASIC;
    if (target) *target = r->status == 407 ? WINHTTP_AUTH_TARGET_PROXY : WINHTTP_AUTH_TARGET_SERVER;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * URLs
 * ----------------------------------------------------------------------- */
static BOOL put_part(LPWSTR *dst, DWORD *len, const WCHAR *s, DWORD n)
{
    if (!*len) return TRUE;
    if (!*dst) { *dst = (LPWSTR)s; *len = n; return TRUE; }
    if (*len <= n) { *len = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(*dst, s, n * sizeof(WCHAR));
    (*dst)[n] = 0;
    *len = n;
    return TRUE;
}

WINHTTPAPI BOOL WINAPI WinHttpCrackUrl(LPCWSTR url, DWORD url_len, DWORD flags, LPURL_COMPONENTS uc)
{
    (void)flags;
    if (!url || !uc || uc->dwStructSize != sizeof(*uc)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!url_len) url_len = (DWORD)wcslen(url);
    const WCHAR *end = url + url_len, *p = url;
    while (p < end && *p != ':') p++;
    DWORD slen = (DWORD)(p - url);
    int scheme;
    WORD port;
    if (slen == 4 && !_wcsnicmp(url, L"http", 4)) { scheme = INTERNET_SCHEME_HTTP; port = 80; }
    else if (slen == 5 && !_wcsnicmp(url, L"https", 5)) { scheme = INTERNET_SCHEME_HTTPS; port = 443; }
    else { SetLastError(ERROR_WINHTTP_UNRECOGNIZED_SCHEME); return FALSE; }
    if (end - p < 3 || p[1] != '/' || p[2] != '/') { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
    if (!put_part(&uc->lpszScheme, &uc->dwSchemeLength, url, slen)) return FALSE;
    uc->nScheme = scheme;
    p += 3;
    const WCHAR *aend = p;
    while (aend < end && *aend != '/' && *aend != '?' && *aend != '#') aend++;
    const WCHAR *at = 0, *user = 0, *pass = 0, *host = p;
    DWORD ulen = 0, plen = 0;
    for (const WCHAR *q = p; q < aend; q++) if (*q == '@') at = q;
    if (at) {
        user = p;
        const WCHAR *colon = p;
        while (colon < at && *colon != ':') colon++;
        ulen = (DWORD)(colon - p);
        if (colon < at) { pass = colon + 1; plen = (DWORD)(at - pass); }
        host = at + 1;
    }
    const WCHAR *hend = host;
    if (hend < aend && *hend == '[') { while (hend < aend && *hend != ']') hend++; if (hend < aend) hend++; }
    while (hend < aend && *hend != ':') hend++;
    if (hend < aend) {
        DWORD n = 0;
        for (const WCHAR *q = hend + 1; q < aend; q++) {
            if (*q < '0' || *q > '9') { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
            n = n * 10 + (DWORD)(*q - '0');
        }
        if (n > 65535) { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
        port = (WORD)n;
    }
    if (hend == host) { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
    if (!put_part(&uc->lpszHostName, &uc->dwHostNameLength, host, (DWORD)(hend - host))) return FALSE;
    uc->nPort = port;
    if (!put_part(&uc->lpszUserName, &uc->dwUserNameLength, user ? user : aend, ulen)) return FALSE;
    if (!put_part(&uc->lpszPassword, &uc->dwPasswordLength, pass ? pass : aend, plen)) return FALSE;
    const WCHAR *extra = aend;
    while (extra < end && *extra != '?' && *extra != '#') extra++;
    if (!uc->dwExtraInfoLength) extra = end;           /* not asked for: the path keeps it */
    if (!put_part(&uc->lpszUrlPath, &uc->dwUrlPathLength, aend, (DWORD)(extra - aend))) return FALSE;
    if (!put_part(&uc->lpszExtraInfo, &uc->dwExtraInfoLength, extra, (DWORD)(end - extra))) return FALSE;
    return TRUE;
}

static void wput(WCHAR *out, DWORD *n, DWORD cap, const WCHAR *s, DWORD len)
{
    if (!s) return;
    if (len == 0) len = (DWORD)wcslen(s);
    for (DWORD i = 0; i < len; i++, (*n)++) if (out && *n < cap) out[*n] = s[i];
}

WINHTTPAPI BOOL WINAPI WinHttpCreateUrl(LPURL_COMPONENTS uc, DWORD flags, LPWSTR url, LPDWORD length)
{
    (void)flags;
    if (!uc || !length || uc->dwStructSize != sizeof(*uc)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD n = 0, cap = url ? *length : 0;
    int https = uc->nScheme == INTERNET_SCHEME_HTTPS;
    if (uc->lpszScheme) wput(url, &n, cap, uc->lpszScheme, uc->dwSchemeLength);
    else wput(url, &n, cap, https ? L"https" : L"http", 0);
    wput(url, &n, cap, L"://", 0);
    if (uc->lpszUserName) {
        wput(url, &n, cap, uc->lpszUserName, uc->dwUserNameLength);
        if (uc->lpszPassword) { wput(url, &n, cap, L":", 0); wput(url, &n, cap, uc->lpszPassword, uc->dwPasswordLength); }
        wput(url, &n, cap, L"@", 0);
    }
    wput(url, &n, cap, uc->lpszHostName, uc->dwHostNameLength);
    if (uc->nPort && uc->nPort != (https ? 443 : 80)) {
        WCHAR p[8];
        _snwprintf(p, 8, L":%u", uc->nPort);
        wput(url, &n, cap, p, 0);
    }
    wput(url, &n, cap, uc->lpszUrlPath, uc->dwUrlPathLength);
    wput(url, &n, cap, uc->lpszExtraInfo, uc->dwExtraInfoLength);
    if (!url || n + 1 > *length) { *length = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    url[n] = 0;
    *length = n;
    return TRUE;
}

WINHTTPAPI BOOL WINAPI WinHttpTimeFromSystemTime(const SYSTEMTIME *st, LPWSTR out)
{
    static const WCHAR *const days[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    static const WCHAR *const months[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                           L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    if (!st || !out || st->wMonth < 1 || st->wMonth > 12 || st->wDayOfWeek > 6) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    _snwprintf(out, 30, L"%ls, %02u %ls %04u %02u:%02u:%02u GMT", days[st->wDayOfWeek], st->wDay, months[st->wMonth - 1],
              st->wYear, st->wHour, st->wMinute, st->wSecond);
    return TRUE;
}

WINHTTPAPI BOOL WINAPI WinHttpTimeToSystemTime(LPCWSTR time, SYSTEMTIME *st)
{
    if (!time || !st) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    char *s = utf8(time);
    BOOL ok = s && parse_time(s, st);
    free(s);
    if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
    return ok;
}

/* -----------------------------------------------------------------------
 * Proxies: WinHTTP itself connects directly; the user's Internet Settings
 * (ProxyEnable, ProxyServer, ProxyOverride, AutoConfigURL under HKCU, as
 * Windows keeps them) are reported to programs that pick their own proxy
 * ----------------------------------------------------------------------- */
static LPWSTR ie_setting(HKEY k, const WCHAR *name)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(k, name, 0, &type, 0, &cb) || type != REG_SZ || cb < 4) return 0;
    LPWSTR v = GlobalAlloc(GMEM_ZEROINIT, cb + sizeof(WCHAR));   /* freed with GlobalFree, as on Windows */
    if (v && RegQueryValueExW(k, name, 0, &type, (BYTE *)v, &cb)) { GlobalFree(v); return 0; }
    if (v && !v[0]) { GlobalFree(v); return 0; }
    return v;
}

WINHTTPAPI BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *c)
{
    if (!c) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ZeroMemory(c, sizeof(*c));                         /* direct connections */
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      0, KEY_READ, &k))
        return TRUE;
    DWORD on = 0, cb = sizeof(on), type = 0;
    if (!RegQueryValueExW(k, L"ProxyEnable", 0, &type, (BYTE *)&on, &cb) && type == REG_DWORD && on) {
        c->lpszProxy = ie_setting(k, L"ProxyServer");
        c->lpszProxyBypass = ie_setting(k, L"ProxyOverride");
    }
    c->lpszAutoConfigUrl = ie_setting(k, L"AutoConfigURL");
    RegCloseKey(k);
    return TRUE;
}
WINHTTPAPI BOOL WINAPI WinHttpGetDefaultProxyConfiguration(WINHTTP_PROXY_INFO *p)
{
    if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ZeroMemory(p, sizeof(*p));
    p->dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
    return TRUE;
}
WINHTTPAPI BOOL WINAPI WinHttpSetDefaultProxyConfiguration(WINHTTP_PROXY_INFO *p) { (void)p; return TRUE; }
WINHTTPAPI BOOL WINAPI WinHttpGetProxyForUrl(HINTERNET s, LPCWSTR url, LPVOID opts, WINHTTP_PROXY_INFO *p)
{
    (void)s; (void)url; (void)opts; (void)p;
    SetLastError(ERROR_WINHTTP_AUTODETECTION_FAILED);
    return FALSE;
}
/* The proxy resolver calls (Windows 8 on).  They answer as
 * WinHttpGetProxyForUrl does: NovaOS has no WPAD discovery or PAC script
 * engine, so the lookup ends with ERROR_WINHTTP_AUTODETECTION_FAILED and
 * programs go direct, through the status callback, as an asynchronous
 * session must hear it */
#define API_GET_PROXY_FOR_URL_ 6
typedef struct { LPWSTR pwszProxy; LPWSTR pwszProxyBypass; DWORD dwFlags; BOOL fBypass; INTERNET_PORT ProxyPort; } PROXY_RESULT_ENTRY_;
typedef struct { DWORD cEntries; PROXY_RESULT_ENTRY_ *pEntries; } PROXY_RESULT_;

WINHTTPAPI DWORD WINAPI WinHttpCreateProxyResolver(HINTERNET session, HINTERNET *out)
{
    Hdr *s = get(session, WINHTTP_HANDLE_TYPE_SESSION);
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = 0;
    if (!s) return ERROR_INVALID_HANDLE;
    if (!s->async) return ERROR_WINHTTP_INCORRECT_HANDLE_TYPE;     /* (a WINHTTP_FLAG_ASYNC session only) */
    Resolver *r = xcalloc(sizeof(Resolver));
    if (!r) return ERROR_NOT_ENOUGH_MEMORY;
    inherit(&r->h, s, WINHTTP_HANDLE_TYPE_PROXY_RESOLVER_);
    *out = r;
    return ERROR_SUCCESS;
}

static DWORD WINAPI resolve_worker(LPVOID p)
{
    Resolver *r = p;
    WINHTTP_STATUS_CALLBACK cb = r->h.cb;
    WINHTTP_ASYNC_RESULT res = { API_GET_PROXY_FOR_URL_, ERROR_WINHTTP_AUTODETECTION_FAILED };
    InterlockedExchange(&r->busy, 0);
    if (cb && cb != WINHTTP_INVALID_STATUS_CALLBACK && (r->h.cb_flags & WINHTTP_CALLBACK_STATUS_REQUEST_ERROR))
        cb((HINTERNET)r, r->lookup_ctx, WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &res, sizeof(res));
    release(&r->h);
    return 0;
}

WINHTTPAPI DWORD WINAPI WinHttpGetProxyForUrlEx(HINTERNET resolver, LPCWSTR url, LPVOID opts, DWORD_PTR ctx)
{
    Resolver *r = (Resolver *)get(resolver, WINHTTP_HANDLE_TYPE_PROXY_RESOLVER_);
    if (!r) return GetLastError();
    if (!url || !*url || !opts) return ERROR_INVALID_PARAMETER;
    if (InterlockedExchange(&r->busy, 1)) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    r->lookup_ctx = ctx;
    InterlockedIncrement(&r->h.refs);
    HANDLE t = CreateThread(0, 0, resolve_worker, r, 0, 0);
    if (!t) { InterlockedExchange(&r->busy, 0); release(&r->h); return ERROR_NOT_ENOUGH_MEMORY; }
    CloseHandle(t);
    return ERROR_IO_PENDING;
}

/* Only a lookup that found proxies (WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE)
 * leaves a result to read, and none does */
WINHTTPAPI DWORD WINAPI WinHttpGetProxyResult(HINTERNET resolver, PROXY_RESULT_ *res)
{
    if (!get(resolver, WINHTTP_HANDLE_TYPE_PROXY_RESOLVER_)) return GetLastError();
    if (!res) return ERROR_INVALID_PARAMETER;
    res->cEntries = 0;
    res->pEntries = 0;
    return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
}

WINHTTPAPI VOID WINAPI WinHttpFreeProxyResult(PROXY_RESULT_ *res)
{
    if (!res) return;
    for (DWORD i = 0; res->pEntries && i < res->cEntries; i++) {
        free(res->pEntries[i].pwszProxy);
        free(res->pEntries[i].pwszProxyBypass);
    }
    free(res->pEntries);
    res->pEntries = 0;
    res->cEntries = 0;
}

WINHTTPAPI BOOL WINAPI WinHttpDetectAutoProxyConfigUrl(DWORD flags, LPWSTR *url)
{
    (void)flags; if (url) *url = 0;
    SetLastError(ERROR_WINHTTP_AUTODETECTION_FAILED);
    return FALSE;
}
WINHTTPAPI BOOL WINAPI WinHttpCheckPlatform(void) { return TRUE; }
