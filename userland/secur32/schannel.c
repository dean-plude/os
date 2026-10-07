/*
 * schannel.c — the Schannel security package (TLS through SSPI) on
 * Mbed TLS, so programs written for Windows' TLS (curl, ffmpeg, git's
 * schannel backend...) get HTTPS.  Client side only; certificates are
 * checked against the same trusted roots NetSurf uses (the Mozilla list in
 * C:\Windows\System32\ca-bundle.der plus certutil's CertStore).
 *
 * The caller moves the bytes: InitializeSecurityContext takes what came
 * from the server and hands back what to send, DecryptMessage takes one
 * TLS record at a time.  Mbed TLS reads its input through a callback that
 * sees only whole records, so whatever it does not consume goes back to the
 * caller as SECBUFFER_EXTRA, as Schannel does.
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MBEDTLS_ALLOW_PRIVATE_ACCESS              /* to clear the ALPN list */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "tls_glue.h"

#define SSPI __declspec(dllexport)

typedef LONG SECURITY_STATUS;
typedef struct { ULONG_PTR dwLower, dwUpper; } SecHandle;
typedef struct { ULONG cbBuffer, BufferType; void *pvBuffer; } SecBuffer;
typedef struct { ULONG ulVersion, cBuffers; SecBuffer *pBuffers; } SecBufferDesc;
typedef struct { ULONG fCapabilities; USHORT wVersion, wRPCID; ULONG cbMaxToken; void *Name, *Comment; } SecPkgInfo;

#define SEC_E_OK                   0
#define SEC_I_CONTINUE_NEEDED      0x00090312
#define SEC_I_CONTEXT_EXPIRED      0x00090317
#define SEC_E_INSUFFICIENT_MEMORY  ((SECURITY_STATUS)0x80090300)
#define SEC_E_INVALID_HANDLE       ((SECURITY_STATUS)0x80090301)
#define SEC_E_UNSUPPORTED_FUNCTION ((SECURITY_STATUS)0x80090302)
#define SEC_E_INTERNAL_ERROR       ((SECURITY_STATUS)0x80090304)
#define SEC_E_SECPKG_NOT_FOUND     ((SECURITY_STATUS)0x80090305)
#define SEC_E_INVALID_TOKEN        ((SECURITY_STATUS)0x80090308)
#define SEC_E_NO_CREDENTIALS       ((SECURITY_STATUS)0x8009030E)
#define SEC_E_INCOMPLETE_MESSAGE   ((SECURITY_STATUS)0x80090318)
#define SEC_E_BUFFER_TOO_SMALL     ((SECURITY_STATUS)0x80090321)
#define SEC_E_WRONG_PRINCIPAL      ((SECURITY_STATUS)0x80090322)
#define SEC_E_UNTRUSTED_ROOT       ((SECURITY_STATUS)0x80090325)
#define SEC_E_ILLEGAL_MESSAGE      ((SECURITY_STATUS)0x80090326)
#define SEC_E_CERT_UNKNOWN         ((SECURITY_STATUS)0x80090327)
#define SEC_E_CERT_EXPIRED         ((SECURITY_STATUS)0x80090328)
#define SEC_E_ENCRYPT_FAILURE      ((SECURITY_STATUS)0x80090329)
#define SEC_E_DECRYPT_FAILURE      ((SECURITY_STATUS)0x80090330)
#define SEC_E_CONTEXT_EXPIRED      ((SECURITY_STATUS)0x80090317)

#define SECBUFFER_EMPTY          0
#define SECBUFFER_DATA           1
#define SECBUFFER_TOKEN          2
#define SECBUFFER_MISSING        4
#define SECBUFFER_EXTRA          5
#define SECBUFFER_STREAM_TRAILER 6
#define SECBUFFER_STREAM_HEADER  7
#define SECBUFFER_ALERT          17
#define SECBUFFER_APPLICATION_PROTOCOLS 18
#define SECBUFFER_TYPE(t)        ((t) & 0x0FFFFFFF)

#define SECPKG_CRED_INBOUND  1
#define SECPKG_CRED_OUTBOUND 2

#define ISC_REQ_MUTUAL_AUTH             0x00000002
#define ISC_REQ_ALLOCATE_MEMORY         0x00000100
#define ISC_REQ_MANUAL_CRED_VALIDATION  0x00080000
#define ISC_RET_REPLAY_DETECT           0x00000004
#define ISC_RET_SEQUENCE_DETECT         0x00000008
#define ISC_RET_CONFIDENTIALITY         0x00000010
#define ISC_RET_ALLOCATED_MEMORY        0x00000100
#define ISC_RET_STREAM                  0x00008000
#define ISC_RET_MANUAL_CRED_VALIDATION  0x00080000

#define SCH_CRED_NO_SERVERNAME_CHECK    0x00000004
#define SCH_CRED_MANUAL_CRED_VALIDATION 0x00000008
#define SP_PROT_TLS1_2_CLIENT           0x00000800
#define SP_PROT_TLS1_3_CLIENT           0x00002000
#define SCHANNEL_SHUTDOWN               1

#define SECPKG_ATTR_SIZES                0
#define SECPKG_ATTR_STREAM_SIZES         4
#define SECPKG_ATTR_APPLICATION_PROTOCOL 35
#define SECPKG_ATTR_REMOTE_CERT_CONTEXT  0x53
#define SECPKG_ATTR_CONNECTION_INFO      0x5a
#define SECPKG_ATTR_CIPHER_INFO          0x64

static const WCHAR UNISP_W[] = L"Microsoft Unified Security Protocol Provider";
static const char UNISP_A[] = "Microsoft Unified Security Protocol Provider";

/* -----------------------------------------------------------------------
 * Mbed TLS, set up once per process
 * ----------------------------------------------------------------------- */
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_tls_ok;

static BOOL CALLBACK tls_init_once(PINIT_ONCE o, PVOID p, PVOID *ctx)
{
    (void)o; (void)p; (void)ctx;
    g_tls_ok = nova_tls_init("C:\\Windows\\System32\\ca-bundle.der") == 0;
    return TRUE;
}
static int tls_ready(void)
{
    InitOnceExecuteOnce(&g_once, tls_init_once, 0, 0);
    return g_tls_ok;
}

/* -----------------------------------------------------------------------
 * Credentials and contexts
 * ----------------------------------------------------------------------- */
#define CRED_MAGIC 0x43484353u                          /* "SCHC" */
#define CTX_MAGIC  0x58484353u                          /* "SCHX" */

typedef struct {
    DWORD magic;
    DWORD flags;            /* SCH_CRED_* */
    int tls12_only;
} Cred;

typedef struct {
    DWORD magic;
    Cred cred;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    const char *alpn[9];
    char alpn_buf[256];
    const BYTE *in;         /* the records the caller gave, being read */
    size_t in_len, in_pos;
    BYTE *out;              /* what Mbed TLS wrote, to hand back */
    size_t out_len, out_cap;
    int done, shutdown, closed;
    ULONG req;
    unsigned char *plain;   /* DecryptMessage's plaintext, one record's worth */
    HANDLE peer_store;      /* the server's chain, as a crypt32 store (peer_cert) */
} Ctx;

static BOOL (WINAPI *g_close_store)(HANDLE, DWORD);

static Cred *cred_of(SecHandle *h)
{
    Cred *c = h ? (Cred *)h->dwLower : 0;
    return c && c->magic == CRED_MAGIC ? c : 0;
}
static Ctx *ctx_of(SecHandle *h)
{
    Ctx *c = h ? (Ctx *)h->dwLower : 0;
    return c && c->magic == CTX_MAGIC ? c : 0;
}

static int bio_send(void *p, const unsigned char *buf, size_t len)
{
    Ctx *c = p;
    if (c->out_len + len > c->out_cap) {
        size_t cap = (c->out_len + len) * 2 + 1024;
        BYTE *n = realloc(c->out, cap);
        if (!n) return MBEDTLS_ERR_SSL_ALLOC_FAILED;
        c->out = n;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, buf, len);
    c->out_len += len;
    return (int)len;
}

static int bio_recv(void *p, unsigned char *buf, size_t len)
{
    Ctx *c = p;
    size_t n = c->in_len - c->in_pos;
    if (!n) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > len) n = len;
    memcpy(buf, c->in + c->in_pos, n);
    c->in_pos += n;
    return (int)n;
}

/* How many bytes at @p make whole TLS records; *missing: how many more the
 * next (partial) record needs */
static size_t whole_records(const BYTE *p, size_t n, size_t *missing, int first_only)
{
    size_t off = 0;
    *missing = 0;
    while (off + 5 <= n) {
        size_t rl = 5 + ((size_t)p[off + 3] << 8 | p[off + 4]);
        if (off + rl > n) { *missing = off + rl - n; return off; }
        off += rl;
        if (first_only) return off;
    }
    if (off < n) *missing = 5 - (n - off);
    return off;
}

static SecBuffer *find_buf(SecBufferDesc *d, ULONG type)
{
    if (!d || !d->pBuffers) return 0;
    for (ULONG i = 0; i < d->cBuffers; i++)
        if (SECBUFFER_TYPE(d->pBuffers[i].BufferType) == type) return &d->pBuffers[i];
    return 0;
}

static void ctx_free(Ctx *c)
{
    if (!c) return;
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    free(c->out);
    free(c->plain);
    if (c->peer_store) g_close_store(c->peer_store, 0);
    c->magic = 0;
    free(c);
}

/* Certificate checks the caller turned off */
static int verify_cb(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)crt; (void)depth;
    Ctx *c = p;
    if (c->cred.flags & SCH_CRED_NO_SERVERNAME_CHECK) *flags &= ~MBEDTLS_X509_BADCERT_CN_MISMATCH;
    return 0;
}

/* The ALPN list from a SECBUFFER_APPLICATION_PROTOCOLS buffer:
 * SEC_APPLICATION_PROTOCOLS { ULONG size; { int ext; USHORT len; BYTE list[]; }... } */
static void take_alpn(Ctx *c, SecBufferDesc *in)
{
    SecBuffer *b = find_buf(in, SECBUFFER_APPLICATION_PROTOCOLS);
    if (!b || !b->pvBuffer || b->cbBuffer < 10) return;
    const BYTE *p = b->pvBuffer, *end = p + b->cbBuffer;
    ULONG total = *(const ULONG *)p;
    p += 4;
    if (p + total < end) end = p + total;
    int k = 0;
    size_t used = 0;
    while (p + 6 <= end) {
        int ext = *(const int *)p;
        USHORT len = *(const USHORT *)(p + 4);
        const BYTE *list = p + 6, *lend = list + len;
        if (lend > end) break;
        if (ext == 2 /* SecApplicationProtocolNegotiationExt_ALPN */)
            for (const BYTE *q = list; q < lend && k < 8;) {
                BYTE n = *q++;
                if (q + n > lend || used + n + 1 > sizeof(c->alpn_buf)) break;
                memcpy(c->alpn_buf + used, q, n);
                c->alpn_buf[used + n] = 0;
                c->alpn[k++] = c->alpn_buf + used;
                used += n + 1;
                q += n;
            }
        p = lend;
    }
    c->alpn[k] = 0;
}

static Ctx *ctx_new(Cred *cr, const char *host, ULONG req, SecBufferDesc *in)
{
    if (!tls_ready()) return 0;
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    c->magic = CTX_MAGIC;
    c->cred = *cr;
    c->req = req;
    if (req & ISC_REQ_MANUAL_CRED_VALIDATION) c->cred.flags |= SCH_CRED_MANUAL_CRED_VALIDATION;
    mbedtls_ssl_init(&c->ssl);
    if (nova_tls_client_config(&c->conf, c->cred.tls12_only)) { ctx_free(c); return 0; }
    take_alpn(c, in);
    if (c->alpn[0]) mbedtls_ssl_conf_alpn_protocols(&c->conf, c->alpn);
    else c->conf.MBEDTLS_PRIVATE(alpn_list) = 0;        /* no ALPN extension (the setter wants a list) */
    /* manual validation: the program checks the certificate itself */
    if (c->cred.flags & SCH_CRED_MANUAL_CRED_VALIDATION) mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&c->conf, verify_cb, c);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf)) { ctx_free(c); return 0; }
    if (host && *host) mbedtls_ssl_set_hostname(&c->ssl, host);
    else mbedtls_ssl_set_hostname(&c->ssl, 0);
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, 0);
    return c;
}

/* The handshake's failure as an SSPI status */
static SECURITY_STATUS hs_error(Ctx *c, int r)
{
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        uint32_t v = mbedtls_ssl_get_verify_result(&c->ssl);
        if (v & MBEDTLS_X509_BADCERT_NOT_TRUSTED) return SEC_E_UNTRUSTED_ROOT;
        if (v & MBEDTLS_X509_BADCERT_EXPIRED) return SEC_E_CERT_EXPIRED;
        if (v & MBEDTLS_X509_BADCERT_CN_MISMATCH) return SEC_E_WRONG_PRINCIPAL;
        return SEC_E_CERT_UNKNOWN;
    }
    if (r == MBEDTLS_ERR_SSL_ALLOC_FAILED) return SEC_E_INSUFFICIENT_MEMORY;
    return SEC_E_ILLEGAL_MESSAGE;
}

/* The output token buffer: a SECBUFFER_TOKEN one, else the first empty one
 * (which becomes the token, as Windows does) */
static SecBuffer *out_token(SecBufferDesc *out)
{
    SecBuffer *t = find_buf(out, SECBUFFER_TOKEN);
    if (!t && (t = find_buf(out, SECBUFFER_EMPTY))) t->BufferType = SECBUFFER_TOKEN;
    return t;
}

/* Hand Mbed TLS's output back in the caller's token buffer */
static SECURITY_STATUS give_token(Ctx *c, SecBufferDesc *out, ULONG *attr)
{
    SecBuffer *t = out_token(out);
    /* as Schannel: mutual authentication (the server's certificate) and
     * manual validation come back when asked for (Qt's TLS backend
     * refuses a connection whose flags differ from what it asked) */
    if (attr) *attr = ISC_RET_REPLAY_DETECT | ISC_RET_SEQUENCE_DETECT | ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM |
                      (c->req & (ISC_REQ_MUTUAL_AUTH | ISC_REQ_MANUAL_CRED_VALIDATION));
    if (!t) { c->out_len = 0; return SEC_E_OK; }
    if (c->req & ISC_REQ_ALLOCATE_MEMORY) {
        if (attr) *attr |= ISC_RET_ALLOCATED_MEMORY;
        t->pvBuffer = 0;
        t->cbBuffer = 0;
        if (c->out_len) {
            void *p = HeapAlloc(GetProcessHeap(), 0, c->out_len);
            if (!p) return SEC_E_INSUFFICIENT_MEMORY;
            memcpy(p, c->out, c->out_len);
            t->pvBuffer = p;
            t->cbBuffer = (ULONG)c->out_len;
        }
    } else {
        if (c->out_len > t->cbBuffer) return SEC_E_BUFFER_TOO_SMALL;
        if (c->out_len) memcpy(t->pvBuffer, c->out, c->out_len);
        t->cbBuffer = (ULONG)c->out_len;
    }
    c->out_len = 0;
    return SEC_E_OK;
}

static SECURITY_STATUS isc(SecHandle *cred, SecHandle *ctx, const char *host, ULONG req, SecBufferDesc *in,
                           SecHandle *newctx, SecBufferDesc *out, ULONG *attr, LARGE_INTEGER *expiry)
{
    Ctx *c = ctx_of(ctx);
    if (expiry) expiry->QuadPart = 0x7FFFFFFFFFFFFFFFLL;
    if (!c) {
        if (ctx && ctx->dwLower) return SEC_E_INVALID_HANDLE;
        Cred *cr = cred_of(cred);
        if (!cr) return SEC_E_INVALID_HANDLE;
        if (!newctx) return SEC_E_INVALID_HANDLE;
        c = ctx_new(cr, host, req, in);
        if (!c) return SEC_E_INTERNAL_ERROR;
        newctx->dwLower = (ULONG_PTR)c;
        newctx->dwUpper = 0;
    } else if (newctx && newctx != ctx) {
        *newctx = *ctx;
    }
    c->req = req | (c->req & ISC_REQ_MANUAL_CRED_VALIDATION);

    if (c->shutdown) {                                  /* after ApplyControlToken(SCHANNEL_SHUTDOWN) */
        c->out_len = 0;
        mbedtls_ssl_close_notify(&c->ssl);
        c->closed = 1;
        return give_token(c, out, attr);
    }

    SecBuffer *tok = 0, *extra = 0;
    size_t missing = 0, avail = 0;
    if (in) {
        tok = find_buf(in, SECBUFFER_TOKEN);
        if (tok && tok->pvBuffer && tok->cbBuffer) {
            avail = whole_records(tok->pvBuffer, tok->cbBuffer, &missing, 0);
            if (!avail) {
                SecBuffer *m = find_buf(in, SECBUFFER_EMPTY);
                if (m) { m->BufferType = SECBUFFER_MISSING; m->cbBuffer = (ULONG)missing; }
                return SEC_E_INCOMPLETE_MESSAGE;
            }
        }
    }
    c->in = tok ? tok->pvBuffer : 0;
    c->in_len = avail;
    c->in_pos = 0;

    int r = mbedtls_ssl_handshake(&c->ssl);
    if (r && r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
        SecBuffer *t = out_token(out);
        if (t && c->out_len) give_token(c, out, attr);  /* the alert, for the server */
        return hs_error(c, r);
    }
    SECURITY_STATUS st = give_token(c, out, attr);
    if (st) return st;
    /* what was not read goes back */
    size_t left = tok ? tok->cbBuffer - c->in_pos : 0;
    if (left && (extra = find_buf(in, SECBUFFER_EMPTY))) {
        extra->BufferType = SECBUFFER_EXTRA;
        extra->cbBuffer = (ULONG)left;
    }
    c->in = 0;
    c->in_len = c->in_pos = 0;
    if (!r) { c->done = 1; return SEC_E_OK; }
    return SEC_I_CONTINUE_NEEDED;
}

static char *utf8_of(const WCHAR *w)
{
    if (!w) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    char *s = n > 0 ? malloc(n) : 0;
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, 0, 0);
    return s;
}

SSPI SECURITY_STATUS WINAPI InitializeSecurityContextW(SecHandle *cred, SecHandle *ctx, WCHAR *target, ULONG req,
                                                       ULONG r1, ULONG rep, SecBufferDesc *in, ULONG r2,
                                                       SecHandle *newctx, SecBufferDesc *out, ULONG *attr,
                                                       LARGE_INTEGER *expiry)
{
    (void)r1; (void)rep; (void)r2;
    char *host = ctx_of(ctx) ? 0 : utf8_of(target);
    SECURITY_STATUS s = isc(cred, ctx, host, req, in, newctx, out, attr, expiry);
    free(host);
    return s;
}

SSPI SECURITY_STATUS WINAPI InitializeSecurityContextA(SecHandle *cred, SecHandle *ctx, char *target, ULONG req,
                                                       ULONG r1, ULONG rep, SecBufferDesc *in, ULONG r2,
                                                       SecHandle *newctx, SecBufferDesc *out, ULONG *attr,
                                                       LARGE_INTEGER *expiry)
{
    (void)r1; (void)rep; (void)r2;
    return isc(cred, ctx, target, req, in, newctx, out, attr, expiry);
}

/* Server contexts need a certificate and key; not supported */
SSPI SECURITY_STATUS WINAPI AcceptSecurityContext(SecHandle *cred, SecHandle *ctx, SecBufferDesc *in, ULONG req,
                                                  ULONG rep, SecHandle *newctx, SecBufferDesc *out, ULONG *attr,
                                                  LARGE_INTEGER *expiry)
{
    (void)cred; (void)ctx; (void)in; (void)req; (void)rep; (void)newctx; (void)out; (void)attr; (void)expiry;
    return SEC_E_UNSUPPORTED_FUNCTION;
}

SSPI SECURITY_STATUS WINAPI DeleteSecurityContext(SecHandle *ctx)
{
    Ctx *c = ctx_of(ctx);
    if (!c) return SEC_E_INVALID_HANDLE;
    ctx_free(c);
    ctx->dwLower = ctx->dwUpper = 0;
    return SEC_E_OK;
}

SSPI SECURITY_STATUS WINAPI ApplyControlToken(SecHandle *ctx, SecBufferDesc *in)
{
    Ctx *c = ctx_of(ctx);
    if (!c) return SEC_E_INVALID_HANDLE;
    SecBuffer *t = find_buf(in, SECBUFFER_TOKEN);
    if (!t || !t->pvBuffer || t->cbBuffer < sizeof(DWORD)) return SEC_E_INVALID_TOKEN;
    if (*(DWORD *)t->pvBuffer != SCHANNEL_SHUTDOWN) return SEC_E_UNSUPPORTED_FUNCTION;
    c->shutdown = 1;
    return SEC_E_OK;
}

SSPI SECURITY_STATUS WINAPI CompleteAuthToken(SecHandle *ctx, SecBufferDesc *tok)
{
    (void)tok;
    return ctx_of(ctx) ? SEC_E_OK : SEC_E_INVALID_HANDLE;
}

/* -----------------------------------------------------------------------
 * Messages
 * ----------------------------------------------------------------------- */
SSPI SECURITY_STATUS WINAPI EncryptMessage(SecHandle *ctx, ULONG qop, SecBufferDesc *d, ULONG seq)
{
    (void)qop; (void)seq;
    Ctx *c = ctx_of(ctx);
    if (!c) return SEC_E_INVALID_HANDLE;
    if (!c->done || c->closed) return SEC_E_CONTEXT_EXPIRED;
    SecBuffer *h = find_buf(d, SECBUFFER_STREAM_HEADER), *b = find_buf(d, SECBUFFER_DATA),
              *t = find_buf(d, SECBUFFER_STREAM_TRAILER);
    if (!h || !b || !t) return SEC_E_INVALID_TOKEN;
    c->out_len = 0;
    const BYTE *p = b->pvBuffer;
    size_t n = b->cbBuffer, done = 0;
    while (done < n) {
        int r = mbedtls_ssl_write(&c->ssl, p + done, n - done);
        if (r <= 0) { c->out_len = 0; return SEC_E_ENCRYPT_FAILURE; }
        done += (size_t)r;
    }
    /* the records, poured into header, data and trailer in turn (they
     * are sent in that order) */
    size_t total = c->out_len, hn = total < h->cbBuffer ? total : h->cbBuffer;
    size_t dn = total - hn < b->cbBuffer ? total - hn : b->cbBuffer, tn = total - hn - dn;
    if (tn > t->cbBuffer) { c->out_len = 0; return SEC_E_BUFFER_TOO_SMALL; }
    memmove(h->pvBuffer, c->out, hn);
    memmove(b->pvBuffer, c->out + hn, dn);
    memmove(t->pvBuffer, c->out + hn + dn, tn);
    h->cbBuffer = (ULONG)hn;
    b->cbBuffer = (ULONG)dn;
    t->cbBuffer = (ULONG)tn;
    c->out_len = 0;
    return SEC_E_OK;
}

SSPI SECURITY_STATUS WINAPI DecryptMessage(SecHandle *ctx, SecBufferDesc *d, ULONG seq, ULONG *qop)
{
    (void)seq;
    if (qop) *qop = 0;
    Ctx *c = ctx_of(ctx);
    if (!c) return SEC_E_INVALID_HANDLE;
    if (!d || !d->pBuffers || d->cBuffers < 2) return SEC_E_INVALID_TOKEN;
    SecBuffer *b = find_buf(d, SECBUFFER_DATA);
    if (!b || !b->pvBuffer) return SEC_E_INVALID_TOKEN;
    BYTE *p = b->pvBuffer;
    size_t n = b->cbBuffer, missing = 0;
    size_t rec = whole_records(p, n, &missing, 1);
    if (!rec) {
        for (ULONG i = 0; i < d->cBuffers; i++)
            if (&d->pBuffers[i] != b && SECBUFFER_TYPE(d->pBuffers[i].BufferType) == SECBUFFER_EMPTY) {
                d->pBuffers[i].BufferType = SECBUFFER_MISSING;
                d->pBuffers[i].cbBuffer = (ULONG)missing;
                break;
            }
        return SEC_E_INCOMPLETE_MESSAGE;
    }
    c->in = p;
    c->in_len = rec;
    c->in_pos = 0;
    enum { PLAIN = 16384 + 256 };
    if (!c->plain && !(c->plain = malloc(PLAIN))) return SEC_E_INSUFFICIENT_MEMORY;
    unsigned char *plain = c->plain;
    size_t got = 0;
    int closed = 0;
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, plain + got, PLAIN - got);
        if (r > 0) { got += (size_t)r; if (c->in_pos >= c->in_len) break; continue; }
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { closed = 1; break; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) { if (c->in_pos >= c->in_len) break; continue; }
        c->in = 0;
        return SEC_E_DECRYPT_FAILURE;
    }
    c->in = 0;
    if (got > rec - 5) got = rec - 5;                   /* cannot happen: plaintext <= ciphertext */
    memcpy(p + 5, plain, got);
    /* the four buffers Schannel hands back: header, data, trailer, extra */
    SecBuffer *o = d->pBuffers;
    ULONG nb = d->cBuffers;
    SecBuffer res[4] = {
        { 5, SECBUFFER_STREAM_HEADER, p },
        { (ULONG)got, SECBUFFER_DATA, p + 5 },
        { (ULONG)(rec - 5 - got), SECBUFFER_STREAM_TRAILER, p + 5 + got },
        { (ULONG)(n - rec), SECBUFFER_EXTRA, p + rec },
    };
    for (ULONG i = 0; i < nb; i++) { o[i].BufferType = SECBUFFER_EMPTY; o[i].cbBuffer = 0; o[i].pvBuffer = 0; }
    for (ULONG i = 0, k = 0; i < 4 && k < nb; i++) {
        if (i == 3 && n == rec) break;
        o[k++] = res[i];
    }
    if (closed) { c->closed = 1; return SEC_I_CONTEXT_EXPIRED; }
    return SEC_E_OK;
}

/* -----------------------------------------------------------------------
 * Attributes
 * ----------------------------------------------------------------------- */
typedef struct { ULONG cbHeader, cbTrailer, cbMaximumMessage, cBuffers, cbBlockSize; } StreamSizes;
typedef struct { ULONG cbMaxToken, cbMaxSignature, cbBlockSize, cbSecurityTrailer; } Sizes;
typedef struct { DWORD dwProtocol, aiCipher, dwCipherStrength, aiHash, dwHashStrength, aiExch, dwExchStrength; } ConnInfo;
typedef struct { int status, ext; BYTE size; BYTE id[255]; } AppProto;
typedef struct {
    DWORD dwVersion, dwProtocol, dwCipherSuite, dwBaseCipherSuite;
    WCHAR szCipherSuite[64], szCipher[64];
    DWORD dwCipherLen, dwCipherBlockLen;
    WCHAR szHash[64];
    DWORD dwHashLen;
    WCHAR szExchange[64];
    DWORD dwMinExchangeLen, dwMaxExchangeLen;
    WCHAR szCertificate[64];
    DWORD dwKeyType;
} CipherInfo;

static void wcopy(WCHAR *d, const char *s)
{
    int i = 0;
    for (; s && s[i] && i < 63; i++) d[i] = (WCHAR)(unsigned char)s[i];
    d[i] = 0;
}

/* The suite's name as Windows spells it: Mbed TLS's "TLS1-3-AES-128-GCM-SHA256"
 * and "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256" become "TLS_AES_128_GCM_SHA256"
 * and "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256" */
static void suite_name(WCHAR *d, const char *s)
{
    char buf[64];
    int i = 0;
    if (s && !strncmp(s, "TLS1-3-", 7)) { strcpy(buf, "TLS_"); i = 4; s += 7; }
    for (; s && *s && i < 63; s++) buf[i++] = *s == '-' ? '_' : *s;
    buf[i] = 0;
    wcopy(d, buf);
}

/* The server's certificate as a context (PCCERT_CONTEXT) whose store
 * (hCertStore) holds the rest of the chain it sent; NULL if there is none.
 * The certificate functions are crypt32's, as with Windows' Schannel. */
static void *peer_cert(Ctx *c)
{
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&c->ssl);
    static HMODULE crypt32;
    if (!crt || !crt->raw.p || (!crypt32 && !(crypt32 = LoadLibraryA("crypt32.dll")))) return 0;
    typedef HANDLE (WINAPI *OpenStore)(LPCSTR, DWORD, ULONG_PTR, DWORD, const void *);
    typedef BOOL (WINAPI *AddEncoded)(HANDLE, DWORD, const BYTE *, DWORD, DWORD, const void **);
    OpenStore open = (OpenStore)GetProcAddress(crypt32, "CertOpenStore");
    AddEncoded add = (AddEncoded)GetProcAddress(crypt32, "CertAddEncodedCertificateToStore");
    g_close_store = (BOOL (WINAPI *)(HANDLE, DWORD))GetProcAddress(crypt32, "CertCloseStore");
    if (!open || !add || !g_close_store) return 0;
    if (!c->peer_store &&
        !(c->peer_store = open((LPCSTR)2 /* CERT_STORE_PROV_MEMORY */, 1 /* X509_ASN_ENCODING */, 0, 0, 0)))
        return 0;
    /* the store lives as long as this context: crypt32's contexts do not
     * keep theirs open */
    const void *leaf = 0;
    for (const mbedtls_x509_crt *x = crt; x && x->raw.p; x = x->next)
        add(c->peer_store, 1, x->raw.p, (DWORD)x->raw.len, 1 /* CERT_STORE_ADD_NEW */, 0);
    add(c->peer_store, 1, crt->raw.p, (DWORD)crt->raw.len, 2 /* CERT_STORE_ADD_USE_EXISTING */, &leaf);
    return (void *)leaf;
}

static SECURITY_STATUS query(SecHandle *ctx, ULONG attr, void *buf)
{
    Ctx *c = ctx_of(ctx);
    if (!c) return SEC_E_INVALID_HANDLE;
    if (!buf) return SEC_E_INVALID_TOKEN;
    switch (attr) {
    case SECPKG_ATTR_STREAM_SIZES: {
        StreamSizes *s = buf;
        int exp = mbedtls_ssl_get_record_expansion(&c->ssl);
        int max = mbedtls_ssl_get_max_out_record_payload(&c->ssl);
        s->cbHeader = 5;
        s->cbTrailer = (ULONG)(exp > 5 ? exp - 5 : 64) + 16;
        s->cbMaximumMessage = max > 0 ? (ULONG)max : 16384;
        s->cBuffers = 4;
        s->cbBlockSize = 16;
        return SEC_E_OK;
    }
    case SECPKG_ATTR_SIZES: {
        Sizes *s = buf;
        s->cbMaxToken = 0x6000;
        s->cbMaxSignature = 0;
        s->cbBlockSize = 16;
        s->cbSecurityTrailer = 0;
        return SEC_E_OK;
    }
    case SECPKG_ATTR_CONNECTION_INFO: {
        ConnInfo *ci = buf;
        const char *suite = mbedtls_ssl_get_ciphersuite(&c->ssl);
        const char *ver = mbedtls_ssl_get_version(&c->ssl);
        memset(ci, 0, sizeof(*ci));
        ci->dwProtocol = ver && strstr(ver, "1.3") ? SP_PROT_TLS1_3_CLIENT : SP_PROT_TLS1_2_CLIENT;
        if (suite && strstr(suite, "CHACHA20")) { ci->aiCipher = 0; ci->dwCipherStrength = 256; }
        else if (suite && strstr(suite, "AES-256")) { ci->aiCipher = 0x6610; ci->dwCipherStrength = 256; }
        else { ci->aiCipher = 0x660E; ci->dwCipherStrength = 128; }
        if (suite && strstr(suite, "SHA384")) { ci->aiHash = 0x800D; ci->dwHashStrength = 384; }
        else { ci->aiHash = 0x800C; ci->dwHashStrength = 256; }
        ci->aiExch = 0xAE06;                            /* CALG_ECDH_EPHEM */
        ci->dwExchStrength = 256;
        return SEC_E_OK;
    }
    case SECPKG_ATTR_CIPHER_INFO: {
        CipherInfo *ci = buf;
        const char *suite = mbedtls_ssl_get_ciphersuite(&c->ssl);
        const char *ver = mbedtls_ssl_get_version(&c->ssl);
        memset(ci, 0, sizeof(*ci));
        ci->dwVersion = 1;                              /* SECPKGCONTEXT_CIPHERINFO_V1 */
        ci->dwProtocol = ver && strstr(ver, "1.3") ? 0x0304 : 0x0303;
        ci->dwCipherSuite = ci->dwBaseCipherSuite = (DWORD)mbedtls_ssl_get_ciphersuite_id_from_ssl(&c->ssl);
        suite_name(ci->szCipherSuite, suite);
        int aes256 = suite && strstr(suite, "AES-256"), cha = suite && strstr(suite, "CHACHA20");
        wcopy(ci->szCipher, cha ? "CHACHA20_POLY1305" : "AES");
        ci->dwCipherLen = aes256 || cha ? 256 : 128;
        ci->dwCipherBlockLen = cha ? 1 : 16;
        int sha384 = suite && strstr(suite, "SHA384");
        wcopy(ci->szHash, sha384 ? "SHA384" : "SHA256");
        ci->dwHashLen = sha384 ? 384 : 256;
        wcopy(ci->szExchange, "ECDHE");
        ci->dwMinExchangeLen = ci->dwMaxExchangeLen = 256;
        wcopy(ci->szCertificate, suite && strstr(suite, "ECDSA") ? "ECDSA" : "RSA");
        return SEC_E_OK;
    }
    case SECPKG_ATTR_REMOTE_CERT_CONTEXT: {
        void *cert = peer_cert(c);
        *(void **)buf = cert;
        return cert ? SEC_E_OK : SEC_E_INTERNAL_ERROR;
    }
    case SECPKG_ATTR_APPLICATION_PROTOCOL: {
        AppProto *a = buf;
        const char *proto = mbedtls_ssl_get_alpn_protocol(&c->ssl);
        memset(a, 0, sizeof(*a));
        a->ext = 2;                                     /* ALPN */
        if (proto) {
            size_t n = strlen(proto);
            a->status = 1;                              /* SecApplicationProtocolNegotiationStatus_Success */
            a->size = (BYTE)(n > 255 ? 255 : n);
            memcpy(a->id, proto, a->size);
        }
        return SEC_E_OK;
    }
    }
    return SEC_E_UNSUPPORTED_FUNCTION;
}

SSPI SECURITY_STATUS WINAPI QueryContextAttributesW(SecHandle *ctx, ULONG attr, void *buf) { return query(ctx, attr, buf); }
SSPI SECURITY_STATUS WINAPI QueryContextAttributesA(SecHandle *ctx, ULONG attr, void *buf) { return query(ctx, attr, buf); }
SSPI SECURITY_STATUS WINAPI SetContextAttributesW(SecHandle *ctx, ULONG attr, void *buf, ULONG n)
{
    (void)attr; (void)buf; (void)n;
    return ctx_of(ctx) ? SEC_E_OK : SEC_E_INVALID_HANDLE;
}
SSPI SECURITY_STATUS WINAPI SetContextAttributesA(SecHandle *ctx, ULONG attr, void *buf, ULONG n)
{
    return SetContextAttributesW(ctx, attr, buf, n);
}

/* -----------------------------------------------------------------------
 * Credentials
 * ----------------------------------------------------------------------- */
static int is_schannel_w(const WCHAR *p)
{
    return p && (!lstrcmpiW(p, UNISP_W) || !lstrcmpiW(p, L"Schannel") || !lstrcmpiW(p, L"Default TLS SSP") ||
                 !lstrcmpiW(p, L"Microsoft TLS 1.0"));
}

/* SCHANNEL_CRED (version 4) or SCH_CREDENTIALS (version 5) */
static void read_auth(Cred *c, const void *auth)
{
    if (!auth) return;
    DWORD ver = *(const DWORD *)auth;
    if (ver == 4) {
        typedef struct { DWORD dwVersion, cCreds; void *paCred, *hRootStore; DWORD cMappers; void *aphMappers;
                         DWORD cSupportedAlgs; void *palgSupportedAlgs; DWORD grbitEnabledProtocols,
                         dwMinimumCipherStrength, dwMaximumCipherStrength, dwSessionLifespan, dwFlags,
                         dwCredFormat; } SchannelCred;
        const SchannelCred *s = auth;
        c->flags = s->dwFlags;
        if (s->grbitEnabledProtocols && !(s->grbitEnabledProtocols & SP_PROT_TLS1_3_CLIENT)) c->tls12_only = 1;
    } else if (ver == 5) {
        typedef struct { DWORD cAlpnIds; void *rgstrAlpnIds; DWORD grbitDisabledProtocols, cDisabledCrypto;
                         void *pDisabledCrypto; DWORD dwFlags; } TlsParams;
        typedef struct { DWORD dwVersion, dwCredFormat, cCreds; void *paCred, *hRootStore; DWORD cMappers;
                         void *aphMappers; DWORD dwSessionLifespan, dwFlags, cTlsParameters;
                         TlsParams *pTlsParameters; } SchCredentials;
        const SchCredentials *s = auth;
        c->flags = s->dwFlags;
        for (DWORD i = 0; s->pTlsParameters && i < s->cTlsParameters; i++)
            if (s->pTlsParameters[i].grbitDisabledProtocols & SP_PROT_TLS1_3_CLIENT) c->tls12_only = 1;
    }
}

static SECURITY_STATUS acquire(int schannel, ULONG use, const void *auth, SecHandle *cred, LARGE_INTEGER *expiry)
{
    if (!schannel) return SEC_E_SECPKG_NOT_FOUND;
    if (!cred) return SEC_E_INVALID_HANDLE;
    if (!(use & SECPKG_CRED_OUTBOUND)) return SEC_E_NO_CREDENTIALS;     /* no server side */
    Cred *c = calloc(1, sizeof(*c));
    if (!c) return SEC_E_INSUFFICIENT_MEMORY;
    c->magic = CRED_MAGIC;
    read_auth(c, auth);
    cred->dwLower = (ULONG_PTR)c;
    cred->dwUpper = 0;
    if (expiry) expiry->QuadPart = 0x7FFFFFFFFFFFFFFFLL;
    return SEC_E_OK;
}

SSPI SECURITY_STATUS WINAPI AcquireCredentialsHandleW(WCHAR *principal, WCHAR *package, ULONG use, void *logon,
                                                      void *auth, void *getkey, void *getkeyarg, SecHandle *cred,
                                                      LARGE_INTEGER *expiry)
{
    (void)principal; (void)logon; (void)getkey; (void)getkeyarg;
    return acquire(is_schannel_w(package), use, auth, cred, expiry);
}

SSPI SECURITY_STATUS WINAPI AcquireCredentialsHandleA(char *principal, char *package, ULONG use, void *logon,
                                                      void *auth, void *getkey, void *getkeyarg, SecHandle *cred,
                                                      LARGE_INTEGER *expiry)
{
    (void)principal; (void)logon; (void)getkey; (void)getkeyarg;
    WCHAR w[64] = { 0 };
    if (package) MultiByteToWideChar(CP_ACP, 0, package, -1, w, 63);
    return acquire(package && is_schannel_w(w), use, auth, cred, expiry);
}

SSPI SECURITY_STATUS WINAPI FreeCredentialsHandle(SecHandle *cred)
{
    Cred *c = cred_of(cred);
    if (!c) return SEC_E_INVALID_HANDLE;
    c->magic = 0;
    free(c);
    cred->dwLower = cred->dwUpper = 0;
    return SEC_E_OK;
}

SSPI SECURITY_STATUS WINAPI QueryCredentialsAttributesW(SecHandle *cred, ULONG attr, void *buf)
{
    (void)attr; (void)buf;
    return cred_of(cred) ? SEC_E_UNSUPPORTED_FUNCTION : SEC_E_INVALID_HANDLE;
}
SSPI SECURITY_STATUS WINAPI QueryCredentialsAttributesA(SecHandle *cred, ULONG attr, void *buf)
{
    return QueryCredentialsAttributesW(cred, attr, buf);
}

SSPI SECURITY_STATUS WINAPI FreeContextBuffer(void *p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
    return SEC_E_OK;
}

/* -----------------------------------------------------------------------
 * The package list (Schannel only)
 * ----------------------------------------------------------------------- */
#define SCHANNEL_CAPS 0x000107B3
static const char COMMENT_A[] = "Schannel Security Package";
static const WCHAR COMMENT_W[] = L"Schannel Security Package";

static SecPkgInfo *pkg_info(int wide)
{
    size_t names = wide ? sizeof(UNISP_W) + sizeof(COMMENT_W) : sizeof(UNISP_A) + sizeof(COMMENT_A);
    SecPkgInfo *p = HeapAlloc(GetProcessHeap(), 0, sizeof(*p) + names);
    if (!p) return 0;
    p->fCapabilities = SCHANNEL_CAPS;
    p->wVersion = 1;
    p->wRPCID = 14;                                     /* UNISP_RPC_ID */
    p->cbMaxToken = 0x6000;
    char *s = (char *)(p + 1);
    if (wide) {
        memcpy(s, UNISP_W, sizeof(UNISP_W)); p->Name = s;
        memcpy(s + sizeof(UNISP_W), COMMENT_W, sizeof(COMMENT_W)); p->Comment = s + sizeof(UNISP_W);
    } else {
        memcpy(s, UNISP_A, sizeof(UNISP_A)); p->Name = s;
        memcpy(s + sizeof(UNISP_A), COMMENT_A, sizeof(COMMENT_A)); p->Comment = s + sizeof(UNISP_A);
    }
    return p;
}

SSPI SECURITY_STATUS WINAPI QuerySecurityPackageInfoW(WCHAR *name, SecPkgInfo **info)
{
    if (!info) return SEC_E_INVALID_TOKEN;
    *info = 0;
    if (!is_schannel_w(name)) return SEC_E_SECPKG_NOT_FOUND;
    return (*info = pkg_info(1)) ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}
SSPI SECURITY_STATUS WINAPI QuerySecurityPackageInfoA(char *name, SecPkgInfo **info)
{
    WCHAR w[64] = { 0 };
    if (!info) return SEC_E_INVALID_TOKEN;
    *info = 0;
    if (name) MultiByteToWideChar(CP_ACP, 0, name, -1, w, 63);
    if (!name || !is_schannel_w(w)) return SEC_E_SECPKG_NOT_FOUND;
    return (*info = pkg_info(0)) ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}
SSPI SECURITY_STATUS WINAPI EnumerateSecurityPackagesW(ULONG *n, SecPkgInfo **info)
{
    if (!n || !info) return SEC_E_INVALID_TOKEN;
    *info = pkg_info(1);
    *n = *info ? 1 : 0;
    return *info ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}
SSPI SECURITY_STATUS WINAPI EnumerateSecurityPackagesA(ULONG *n, SecPkgInfo **info)
{
    if (!n || !info) return SEC_E_INVALID_TOKEN;
    *info = pkg_info(0);
    *n = *info ? 1 : 0;
    return *info ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}

/* Signing, impersonation and the rest belong to NTLM/Kerberos */
static SECURITY_STATUS unsupported(void) { return SEC_E_UNSUPPORTED_FUNCTION; }
SSPI SECURITY_STATUS WINAPI MakeSignature(SecHandle *c, ULONG q, SecBufferDesc *d, ULONG s) { (void)c; (void)q; (void)d; (void)s; return unsupported(); }
SSPI SECURITY_STATUS WINAPI VerifySignature(SecHandle *c, SecBufferDesc *d, ULONG s, ULONG *q) { (void)c; (void)d; (void)s; (void)q; return unsupported(); }
SSPI SECURITY_STATUS WINAPI ImpersonateSecurityContext(SecHandle *c) { (void)c; return unsupported(); }
SSPI SECURITY_STATUS WINAPI RevertSecurityContext(SecHandle *c) { (void)c; return unsupported(); }
SSPI SECURITY_STATUS WINAPI QuerySecurityContextToken(SecHandle *c, HANDLE *t) { (void)c; if (t) *t = 0; return unsupported(); }
SSPI SECURITY_STATUS WINAPI ExportSecurityContext(SecHandle *c, ULONG f, SecBuffer *b, void **t) { (void)c; (void)f; (void)b; (void)t; return unsupported(); }
SSPI SECURITY_STATUS WINAPI ImportSecurityContextW(WCHAR *p, SecBuffer *b, void *t, SecHandle *c) { (void)p; (void)b; (void)t; (void)c; return unsupported(); }
SSPI SECURITY_STATUS WINAPI ImportSecurityContextA(char *p, SecBuffer *b, void *t, SecHandle *c) { (void)p; (void)b; (void)t; (void)c; return unsupported(); }
SSPI SECURITY_STATUS WINAPI AddCredentialsW(SecHandle *h, WCHAR *a, WCHAR *b, ULONG u, void *d, void *f, void *g, LARGE_INTEGER *e)
{ (void)h; (void)a; (void)b; (void)u; (void)d; (void)f; (void)g; (void)e; return unsupported(); }
SSPI SECURITY_STATUS WINAPI AddCredentialsA(SecHandle *h, char *a, char *b, ULONG u, void *d, void *f, void *g, LARGE_INTEGER *e)
{ (void)h; (void)a; (void)b; (void)u; (void)d; (void)f; (void)g; (void)e; return unsupported(); }
SSPI SECURITY_STATUS WINAPI SetCredentialsAttributesW(SecHandle *h, ULONG a, void *b, ULONG n) { (void)h; (void)a; (void)b; (void)n; return unsupported(); }
SSPI SECURITY_STATUS WINAPI SetCredentialsAttributesA(SecHandle *h, ULONG a, void *b, ULONG n) { (void)h; (void)a; (void)b; (void)n; return unsupported(); }

/* -----------------------------------------------------------------------
 * InitSecurityInterface: the same entry points as a table
 * (SecurityFunctionTableW/A, SECURITY_SUPPORT_PROVIDER_INTERFACE_VERSION_4)
 * ----------------------------------------------------------------------- */
typedef struct {
    ULONG dwVersion;
    void *EnumerateSecurityPackages, *QueryCredentialsAttributes, *AcquireCredentialsHandle, *FreeCredentialsHandle,
         *Reserved2, *InitializeSecurityContext, *AcceptSecurityContext, *CompleteAuthToken, *DeleteSecurityContext,
         *ApplyControlToken, *QueryContextAttributes, *ImpersonateSecurityContext, *RevertSecurityContext,
         *MakeSignature, *VerifySignature, *FreeContextBuffer, *QuerySecurityPackageInfo, *Reserved3, *Reserved4,
         *ExportSecurityContext, *ImportSecurityContext, *AddCredentials, *Reserved8, *QuerySecurityContextToken,
         *EncryptMessage, *DecryptMessage, *SetContextAttributes, *SetCredentialsAttributes, *ChangeAccountPassword;
} FunctionTable;

static FunctionTable g_table_w = {
    4, EnumerateSecurityPackagesW, QueryCredentialsAttributesW, AcquireCredentialsHandleW, FreeCredentialsHandle,
    0, InitializeSecurityContextW, AcceptSecurityContext, CompleteAuthToken, DeleteSecurityContext,
    ApplyControlToken, QueryContextAttributesW, ImpersonateSecurityContext, RevertSecurityContext,
    MakeSignature, VerifySignature, FreeContextBuffer, QuerySecurityPackageInfoW, 0, 0,
    ExportSecurityContext, ImportSecurityContextW, AddCredentialsW, 0, QuerySecurityContextToken,
    EncryptMessage, DecryptMessage, SetContextAttributesW, SetCredentialsAttributesW, 0,
};
static FunctionTable g_table_a = {
    4, EnumerateSecurityPackagesA, QueryCredentialsAttributesA, AcquireCredentialsHandleA, FreeCredentialsHandle,
    0, InitializeSecurityContextA, AcceptSecurityContext, CompleteAuthToken, DeleteSecurityContext,
    ApplyControlToken, QueryContextAttributesA, ImpersonateSecurityContext, RevertSecurityContext,
    MakeSignature, VerifySignature, FreeContextBuffer, QuerySecurityPackageInfoA, 0, 0,
    ExportSecurityContext, ImportSecurityContextA, AddCredentialsA, 0, QuerySecurityContextToken,
    EncryptMessage, DecryptMessage, SetContextAttributesA, SetCredentialsAttributesA, 0,
};

SSPI void *WINAPI InitSecurityInterfaceW(void) { return &g_table_w; }
SSPI void *WINAPI InitSecurityInterfaceA(void) { return &g_table_a; }
